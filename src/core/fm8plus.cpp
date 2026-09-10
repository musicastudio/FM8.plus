// FM8.plus shared core implementation: the two arp detours and the feature routing.
#include "fm8plus.h"
#include <windows.h>
#include <cmath>
#include "MinHook.h"

namespace fm8plus {
namespace Core {

thread_local InstanceState* current = nullptr;

// Resolved absolute addresses of the two detoured functions and the callees we invoke.
namespace {
void*  g_base       = nullptr;
Bin    g_bin        = Bin::Vst2;
bool   g_installed  = false;

// Original (trampoline) pointers filled by MinHook.
using ArpRunDispatchFn = void   (*)(void* core, uint32_t destSel, int inBlockPos);
using MidiHandlerFn    = void   (*)(void* fm8midi, void* ev, int flag);
ArpRunDispatchFn o_arpRun  = nullptr;
MidiHandlerFn    o_midiHnd = nullptr;

// thread_local marker: are we currently inside the arp dispatch loop? If so, events reaching the
// MIDI handler are arp-generated, not live input.
thread_local bool  tl_inArp   = false;
thread_local int32_t tl_arpPos = 0;

inline uint32_t rvaFor(const Site& s) { return rva(s, g_bin); }
inline void* addr(const Site& s) { return (uint8_t*)g_base + rvaFor(s); }

// Decide routing for one arp-generated event; returns true if FM8 should still play it internally.
// Updates the note masks and enqueues to the instance out-buffer as the mode requires.
bool routeArpEvent(InstanceState& st, uint8_t status, uint8_t d1, uint8_t d2, int32_t off) {
    const uint8_t type = status & 0xf0;
    const int ch = status & 0x0f;
    const bool isOn  = (type == 0x90) && (d2 > 0);
    const bool isOff = (type == 0x80) || ((type == 0x90) && (d2 == 0));
    const ArpMode mode = (ArpMode)st.arpMode.load(std::memory_order_relaxed);

    // External (MIDI out) side.
    if (mode != ArpMode::Internal) {
        if (isOn) { st.pushOut(status, d1, d2, off); st.extOn.set(ch, d1); }
        else if (isOff) { if (st.extOn.test(ch, d1)) { st.pushOut(status, d1, d2, off); st.extOn.clear(ch, d1); } }
        else st.pushOut(status, d1, d2, off);  // non-note arp event, pass through unchanged
    }

    // Internal (voice) side.
    bool playInternal;
    if (mode == ArpMode::MidiOnly) {
        if (isOn) playInternal = false;                 // suppress new internal voices
        else if (isOff) playInternal = st.intOn.test(ch, d1); // let offs through for voices already on
        else playInternal = true;
    } else {
        playInternal = true;                             // Internal and Clone both play
    }
    if (isOn && playInternal) st.intOn.set(ch, d1);
    if (isOff && playInternal) st.intOn.clear(ch, d1);
    return playInternal;
}

// Detour of ArpRunDispatch (0x1800e7250 family): mark the arp window so the MIDI-handler detour
// can tell arp events from live input, and forward the in-block position.
void __fastcall detourArpRun(void* core, uint32_t destSel, int inBlockPos) {
    InstanceState* st = current;
    if (!st) { o_arpRun(core, destSel, inBlockPos); return; }
    const bool prev = tl_inArp; const int32_t prevPos = tl_arpPos;
    tl_inArp = true; tl_arpPos = inBlockPos;
    o_arpRun(core, destSel, inBlockPos);   // runs the engine and dispatches events through the MIDI handler
    tl_inArp = prev; tl_arpPos = prevPos;
}

// Detour of MidiEventHandler (0x1800e6660 family). Live input passes straight through. Arp events
// (tl_inArp) are routed by mode: cloned or suppressed for MIDI out, and only played internally as
// the mask logic allows.
void __fastcall detourMidiHandler(void* fm8midi, void* ev, int flag) {
    InstanceState* st = current;
    if (!st || !tl_inArp || st->arpMode.load(std::memory_order_relaxed) == (uint8_t)ArpMode::Internal) {
        o_midiHnd(fm8midi, ev, flag);
        return;
    }
    const uint32_t w = *(uint32_t*)((uint8_t*)ev + kElemPackedWord);
    const uint8_t status = (uint8_t)(w >> 16);
    const uint8_t d1 = (uint8_t)(w >> 8) & 0x7f;
    const uint8_t d2 = (uint8_t)w & 0x7f;
    if (routeArpEvent(*st, status, d1, d2, tl_arpPos))
        o_midiHnd(fm8midi, ev, flag);
}
} // namespace

bool validateBuild(void* base) {
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    return nt->FileHeader.TimeDateStamp == kFm8TimeDateStamp;
}

bool install(void* base, Bin which) {
    if (g_installed) return true;
    if (!base || !validateBuild(base)) return false;
    g_base = base; g_bin = which;

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    void* pArpRun = addr(kArpRunDispatch);
    void* pMidi   = addr(kMidiEventHandler);
    if (MH_CreateHook(pArpRun, (void*)&detourArpRun, (void**)&o_arpRun) != MH_OK) return false;
    if (MH_CreateHook(pMidi,   (void*)&detourMidiHandler, (void**)&o_midiHnd) != MH_OK) return false;
    if (MH_EnableHook(pArpRun) != MH_OK) return false;
    if (MH_EnableHook(pMidi)   != MH_OK) return false;

    g_installed = true;
    return true;
}

void uninstall() {
    if (!g_installed) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_installed = false;
}

void morphXYFromCc(const InstanceState& st, uint8_t cc1, float& x, float& y) {
    const float r = st.morphRadius.load(std::memory_order_relaxed);
    const float start = st.morphStartDeg.load(std::memory_order_relaxed) * 3.14159265358979f / 180.0f;
    const float theta = start + (cc1 / 127.0f) * 6.28318530717959f;
    x = 0.5f + r * std::cos(theta);
    y = 0.5f + r * std::sin(theta);
    if (x < 0) x = 0; else if (x > 1) x = 1;
    if (y < 0) y = 0; else if (y > 1) y = 1;
}

void flushExternal(InstanceState& st) {
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n)
            if (st.extOn.test(ch, n)) { st.pushOut((uint8_t)(0x80 | ch), (uint8_t)n, 0, 0); st.extOn.clear(ch, n); }
}

} // namespace Core
} // namespace fm8plus
