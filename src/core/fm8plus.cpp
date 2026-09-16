// FM8.plus shared core implementation: the two arp detours and the feature routing.
#include "fm8plus.h"
#include <windows.h>
#include <cmath>
#include "MinHook.h"
#include "rsrc.h"

namespace fm8plus {
namespace Core {

thread_local InstanceState* current = nullptr;

namespace {
InstanceState* g_singleton = nullptr;
void (*g_arpBlockCb)(InstanceState&) = nullptr;
bool g_logoWidened = false;   // set by serveLogo: the wordmark carries the "+" and the wider rect
}
void setSingleton(InstanceState* s) { g_singleton = s; }
void setArpBlockCallback(void (*cb)(InstanceState&)) { g_arpBlockCb = cb; }

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

// Internal morph setter type: setParameterByTag(EditBuffer* this, uint tag, float value, char thread).
using SetByTagFn = intptr_t (*)(void* editBuf, uint32_t tag, float value, char thread);

// Detour of ArpRunDispatch (0x1800e7250 family): mark the arp window so the MIDI-handler detour
// can tell arp events from live input, forward the in-block position, and capture the EditBuffer
// pointer (param_1 == EditBuffer; +0x29e8 -> arp) for the internal morph setter.
// Walk core -> VstObject (*(core+8)) -> EditBuffer (*(VstObject+0x55d0)), matching what the
// dispatch does internally (FUN_1800ef7f0 returns *(x+0x55d0)). SEH-guarded: a bad chain yields null.
// Both pointers hang off the same object, so one guarded walk fetches them together. `outer` is the
// FM8VstObject: the arp dispatch and FormMain's command sink hand it to the same accessor, which is
// what ties the About argument (*(outer + 0x5620)) to the instance we are processing.
void coreObjects(void* core, void*& editBuf, void*& app) {
    editBuf = app = nullptr;
    __try {
        void* outer = *(void**)((uint8_t*)core + 8);
        if (!outer) return;
        editBuf = *(void**)((uint8_t*)outer + kVstObjEditBuf);
        app = *(void**)((uint8_t*)outer + kVstObjApp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { editBuf = app = nullptr; }
}

void __fastcall detourArpRun(void* core, uint32_t destSel, int inBlockPos) {
    InstanceState* st = current ? current : g_singleton;
    if (!st) { o_arpRun(core, destSel, inBlockPos); return; }
    if (st->pendingFlush.exchange(false, std::memory_order_relaxed)) flushExternal(*st);  // audio-thread flush
    void *eb, *app;
    coreObjects(core, eb, app);
    if (eb) st->editBuf.store(eb, std::memory_order_relaxed);
    if (app) st->appObj.store(app, std::memory_order_relaxed);
    const bool prev = tl_inArp; const int32_t prevPos = tl_arpPos;
    tl_inArp = true; tl_arpPos = inBlockPos;
    o_arpRun(core, destSel, inBlockPos);   // runs the engine and dispatches events through the MIDI handler
    tl_inArp = prev; tl_arpPos = prevPos;
    if (g_arpBlockCb) g_arpBlockCb(*st);   // standalone: flush to WinMM + apply morph
}

// Detour of MidiEventHandler (0x1800e6660 family). Arp events (tl_inArp) are routed by mode: cloned
// or suppressed for MIDI out, and only played internally as the mask logic allows. Live input passes
// straight through, except the morph CC, which is captured and swallowed.
void __fastcall detourMidiHandler(void* fm8midi, void* ev, int flag) {
    InstanceState* st = current ? current : g_singleton;
    if (!st) { o_midiHnd(fm8midi, ev, flag); return; }

    const uint32_t w = *(uint32_t*)((uint8_t*)ev + kElemPackedWord);
    const uint8_t status = (uint8_t)(w >> 16);
    const uint8_t d1 = (uint8_t)(w >> 8) & 0x7f;
    const uint8_t d2 = (uint8_t)w & 0x7f;

    if (tl_inArp && st->arpMode.load(std::memory_order_relaxed) != (uint8_t)ArpMode::Internal) {
        if (routeArpEvent(*st, status, d1, d2, tl_arpPos))
            o_midiHnd(fm8midi, ev, flag);
        return;
    }
    // Live input: if this is the selected morph CC, capture its value and swallow the event, so the
    // CC drives only the morph and FM8 never sees it (no mod-wheel movement, no MIDI-learn). The
    // caller owns the event, this handler only consumes it, so returning early strands nothing.
    const int16_t mc = st->morphCc.load(std::memory_order_relaxed);
    if (mc >= 0 && (status & 0xf0) == 0xb0 && d1 == (uint8_t)mc) {
        st->morphPending.store(d2, std::memory_order_relaxed);
        return;
    }
    o_midiHnd(fm8midi, ev, flag);
}

// ---- GUI scale -------------------------------------------------------------
// FM8 carries a complete HiDPI layer it never switches on: NI::UIA sizes every window it creates by
// a per-window scale, divides incoming mouse coordinates by it, multiplies the dirty rects it sends
// to InvalidateRect, and stretches the final DIB blit to match. The scale is GetDpiForWindow/96, so
// it is always 1 (FM8 never calls SetProcessDpiAwareness), and one byte in the NI::UIA app object
// gates the lot off anyway. These three detours supply our own number instead.
std::atomic<float> g_guiScale{1.0f};
bool g_guiHooked = false;

using AppObjFn   = void* (*)();
using DpiScaleFn = float (*)(void*);
AppObjFn   o_appObj    = nullptr;
DpiScaleFn o_dpiScale  = nullptr;
DpiScaleFn o_surfScale = nullptr;

// Every site that reads the HiDPI gate calls this getter immediately before it, so setting the byte
// here turns the path on wherever it is used, as soon as the object exists. No startup ordering.
void* detourAppObject() {
    void* o = o_appObj();
    if (o && g_guiScale.load(std::memory_order_relaxed) != 1.0f)
        *((uint8_t*)o + kUiaHiDpiFlag) = 1;
    return o;
}

// Editor windows belonging to FM8+ instances (see addScaledWindow). Dead entries are recycled, so
// no teardown bookkeeping is needed. UI thread only.
HWND g_scaled[32] = {};
bool g_gateWindows = false;

bool ownsWindow(HWND h) {
    if (!g_gateWindows) return true;                  // standalone: the whole process is ours
    for (; h; h = GetParent(h))
        for (HWND w : g_scaled) if (w == h) return true;
    return false;
}

// The scale itself. Deliberately ignores the window's real DPI: at 1x FM8 must look exactly as it
// does today, including on a HiDPI monitor where its artwork has always been rendered 1:1.
float scaleFor(HWND h) {
    const float s = g_guiScale.load(std::memory_order_relaxed);
    return (s == 1.0f || ownsWindow(h)) ? s : 1.0f;
}
float detourDpiScale(void* hwnd) { return scaleFor((HWND)hwnd); }

// Stock this returns ceil(scale): NI::UIA renders the DIB at an integer supersample and lets the
// blit fit it to the exact window. FM8 has no high-resolution artwork to supersample from, so hold
// it at 1, keeping the surface at logical size and leaving all the scaling to the StretchDIBits.
float detourSurfScale(void*) { return 1.0f; }

// That StretchDIBits is preceded by FM8's only SetStretchBltMode call, which asks for HALFTONE:
// GDI then interpolates, and FM8's artwork comes out soft and smeared when it is enlarged. The scale
// steps are whole numbers, so COLORONCOLOR replicates each pixel exactly and the GUI stays crisp.
// Swapping the mode in FM8's own GDI32 import is enough; the one call site is that blit.
using SetStretchBltModeFn = int (WINAPI*)(HDC, int);
SetStretchBltModeFn o_setStretchMode = nullptr;
int WINAPI detourSetStretchBltMode(HDC dc, int mode) {
    if (mode == HALFTONE) mode = COLORONCOLOR;
    return o_setStretchMode(dc, mode);
}

// ponytail: best effort. A failure here costs the scale menu, not the arp and morph features.
void installGuiScale() {
    auto mk = [](const Site& site, void* det, void** orig) {
        void* t = addr(site);
        return MH_CreateHook(t, det, orig) == MH_OK && MH_EnableHook(t) == MH_OK;
    };
    g_guiHooked = mk(kUiaAppObject, (void*)&detourAppObject,  (void**)&o_appObj)
               && mk(kUiaDpiScale,  (void*)&detourDpiScale,   (void**)&o_dpiScale)
               && mk(kUiaSurfScale, (void*)&detourSurfScale,  (void**)&o_surfScale);
    o_setStretchMode = (SetStretchBltModeFn)Rsrc::patchImport(
        (HMODULE)g_base, "GDI32.dll", "SetStretchBltMode", (void*)&detourSetStretchBltMode);
}
} // namespace

void setGuiScale(float s) {
    if (!(s >= 1.0f)) s = 1.0f;          // also catches NaN
    if (s > 4.0f) s = 4.0f;
    s = (float)lroundf(s);               // whole numbers only: a fractional scale cannot blit pixel-exact
    g_guiScale.store(g_guiHooked ? s : 1.0f, std::memory_order_relaxed);
}
float guiScale() { return g_guiScale.load(std::memory_order_relaxed); }

void addScaledWindow(void* hwnd) {
    auto h = (HWND)hwnd;
    if (!h) return;
    g_gateWindows = true;
    for (HWND w : g_scaled) if (w == h) return;
    for (HWND& w : g_scaled) if (!w || !IsWindow(w)) { w = h; return; }
}

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
    installGuiScale();

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

void* addressOf(const Site& s) { return g_installed ? addr(s) : nullptr; }

bool aboutReady(const InstanceState& st) {
    return g_installed && st.appObj.load(std::memory_order_relaxed) != nullptr;
}

bool showAbout(InstanceState& st) {
    void* app = st.appObj.load(std::memory_order_relaxed);
    if (!app || !g_installed) return false;
    auto show = (void (*)(void*))addr(kShowAboutDialog);
    __try {
        show(app);   // modal: returns when the user closes FM8's About panel
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st.appObj.store(nullptr, std::memory_order_relaxed);   // stale pointer, stop offering it
        return false;
    }
}

bool serveLogo(void* module) {
    if (!Rsrc::install((HMODULE)module) || !Rsrc::serveLogo()) return false;
    g_logoWidened = true;
    return true;
}

bool logoWidened() { return g_logoWidened; }

bool setMorphXY(InstanceState& st, float x, float y) {
    void* eb = st.editBuf.load(std::memory_order_relaxed);
    if (!eb || !g_installed) return false;
    auto set = (SetByTagFn)addr(kSetParameterByTag);
    __try {
        set(eb, kTagMorphX, x, 1);
        set(eb, kTagMorphY, y, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st.editBuf.store(nullptr, std::memory_order_relaxed);  // bad pointer, stop trying until re-captured
        return false;
    }
}

void applyPendingMorphInternal(InstanceState& st) {
    if (st.morphCc.load(std::memory_order_relaxed) < 0) return;
    uint8_t cc = st.morphPending.exchange(0xff, std::memory_order_relaxed);
    if (cc == 0xff) return;
    float x, y; morphXYFromCc(st, cc, x, y);
    setMorphXY(st, x, y);
}

} // namespace Core
} // namespace fm8plus
