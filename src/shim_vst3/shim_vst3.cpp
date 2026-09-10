// FM8.plus VST3 proxy. Ships as FM8.vst3 beside the renamed original (FM8.plus.core). Forwards the
// real factory unchanged, then hooks four of the component's methods so that:
//   - an event OUTPUT bus ("FM8+ Arp Out") is advertised (FM8 registers none), so the host allocates
//     data.outputEvents;
//   - process() drives the shared arp/morph core and drains queued arp notes into data.outputEvents,
//     and reads the mod-wheel parameter (id 0x6d69646b, per FM8's IMidiMapping) to rotate the Morph.
#include <windows.h>
#include <cstring>
#include "../core/fm8plus.h"
#include "../core/settings.h"
#include "MinHook.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

using namespace fm8plus;
namespace S = Steinberg;
namespace V = Steinberg::Vst;

namespace {
HMODULE g_self = nullptr, g_core = nullptr;
bool g_hooked = false;

std::wstring selfDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p); auto k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

// Per-component state, keyed by the process() `this` pointer. Fixed array, audio-thread-safe scan.
struct Rec { std::atomic<void*> key{nullptr}; InstanceState st; };
constexpr int kMax = 64;
Rec g_rec[kMax];
Rec* recFor(void* k) {
    for (auto& r : g_rec) if (r.key.load(std::memory_order_relaxed) == k) return &r;
    for (auto& r : g_rec) { void* e = nullptr; if (r.key.compare_exchange_strong(e, k)) {
        r.st.modWheelMorph.store(settings::defaultModWheelMorph());
        r.st.arpMode.store((uint8_t)settings::arpModeDefault());
        r.st.morphRadius.store(settings::morphRadius());
        r.st.morphStartDeg.store(settings::morphStartDeg());
        return &r;
    } }
    return nullptr;
}

// Trampolines to the originals.
using GetBusCountFn  = S::int32   (*)(void*, V::MediaType, V::BusDirection);
using GetBusInfoFn   = S::tresult (*)(void*, V::MediaType, V::BusDirection, S::int32, V::BusInfo&);
using ActivateBusFn  = S::tresult (*)(void*, V::MediaType, V::BusDirection, S::int32, S::TBool);
using ProcessFn      = S::tresult (*)(void*, V::ProcessData&);
GetBusCountFn o_busCount = nullptr;
GetBusInfoFn  o_busInfo  = nullptr;
ActivateBusFn o_activate = nullptr;
ProcessFn     o_process  = nullptr;

// Our single added event-output bus sits at index == the real event-out count (which is 0 for FM8).
S::int32 h_getBusCount(void* self, V::MediaType type, V::BusDirection dir) {
    S::int32 n = o_busCount(self, type, dir);
    if (type == V::kEvent && dir == V::kOutput) n += 1;
    return n;
}

S::tresult h_getBusInfo(void* self, V::MediaType type, V::BusDirection dir, S::int32 index, V::BusInfo& bus) {
    if (type == V::kEvent && dir == V::kOutput) {
        S::int32 real = o_busCount(self, type, dir);   // 0 for FM8
        if (index == real) {
            std::memset(&bus, 0, sizeof bus);
            bus.mediaType = V::kEvent;
            bus.direction = V::kOutput;
            bus.channelCount = 16;
            bus.busType = V::kMain;
            bus.flags = V::BusInfo::kDefaultActive;
            const wchar_t* nm = L"FM8+ Arp Out";
            for (int i = 0; i < 12; ++i) bus.name[i] = (S::char16)nm[i];
            return S::kResultTrue;
        }
    }
    return o_busInfo(self, type, dir, index, bus);
}

S::tresult h_activateBus(void* self, V::MediaType type, V::BusDirection dir, S::int32 index, S::TBool state) {
    if (type == V::kEvent && dir == V::kOutput && index == o_busCount(self, type, dir))
        return S::kResultTrue;   // accept activation of our added bus
    return o_activate(self, type, dir, index, state);
}

// Read the mod-wheel parameter (id 0x6d69646b) from the block's input changes; returns 0..127 or -1.
int readModWheel(V::IParameterChanges* changes) {
    if (!changes) return -1;
    S::int32 count = changes->getParameterCount();
    for (S::int32 i = 0; i < count; ++i) {
        V::IParamValueQueue* q = changes->getParameterData(i);
        if (!q || q->getParameterId() != kVst3ModWheelParamId) continue;
        S::int32 pts = q->getPointCount();
        if (pts <= 0) return -1;
        S::int32 off = 0; V::ParamValue val = 0;
        if (q->getPoint(pts - 1, off, val) == S::kResultTrue)
            return (int)(val * 127.0 + 0.5);
    }
    return -1;
}

S::tresult h_process(void* self, V::ProcessData& data) {
    Rec* r = recFor(self);
    if (!r) return o_process(self, data);
    InstanceState& st = r->st;

    if (st.modWheelMorph.load(std::memory_order_relaxed)) {
        int cc = readModWheel(data.inputParameterChanges);
        if (cc >= 0) st.lastCc1.store((uint8_t)cc, std::memory_order_relaxed);
    }

    st.clearBlock();
    Core::current = &st;
    S::tresult rv = o_process(self, data);   // core arp hooks fill st.outBuf and capture editBuf
    Core::current = nullptr;

    Core::applyPendingMorphInternal(st);     // uses the captured EditBuffer

    if (st.outCount > 0 && data.outputEvents) {
        for (int i = 0; i < st.outCount; ++i) {
            const MidiMsg& m = st.outBuf[i];
            V::Event e; std::memset(&e, 0, sizeof e);
            e.busIndex = 0;
            e.sampleOffset = m.offset;
            const uint8_t type = m.status & 0xf0;
            const int ch = m.status & 0x0f;
            if (type == 0x90 && m.data2 > 0) {
                e.type = V::Event::kNoteOnEvent;
                e.noteOn.channel = (S::int16)ch; e.noteOn.pitch = m.data1;
                e.noteOn.velocity = m.data2 / 127.0f; e.noteOn.noteId = -1;
            } else if (type == 0x80 || (type == 0x90 && m.data2 == 0)) {
                e.type = V::Event::kNoteOffEvent;
                e.noteOff.channel = (S::int16)ch; e.noteOff.pitch = m.data1;
                e.noteOff.velocity = m.data2 / 127.0f; e.noteOff.noteId = -1;
            } else {
                e.type = V::Event::kLegacyMIDICCOutEvent;
                e.midiCCOut.controlNumber = m.data1; e.midiCCOut.channel = (S::int8)ch;
                e.midiCCOut.value = (S::int8)m.data2;
            }
            data.outputEvents->addEvent(e);
        }
    }
    st.clearBlock();
    return rv;
}

bool ensureCore() {
    if (g_core) return g_hooked;
    g_core = LoadLibraryW((selfDir() + L"\\FM8.plus.core").c_str());
    if (!g_core) return false;
    settings::load(g_self);
    if (!Core::install((void*)g_core, Bin::Vst3)) return false;
    // Install the four VST3 vtable-function hooks by RVA (MinHook already initialized by the core).
    auto mk = [](const Site& s, void* det, void** orig) {
        void* t = Core::addressOf(s);
        return t && MH_CreateHook(t, det, orig) == MH_OK && MH_EnableHook(t) == MH_OK;
    };
    g_hooked = mk(kVst3GetBusCount, (void*)&h_getBusCount, (void**)&o_busCount)
            && mk(kVst3GetBusInfo,  (void*)&h_getBusInfo,  (void**)&o_busInfo)
            && mk(kVst3ActivateBus, (void*)&h_activateBus, (void**)&o_activate)
            && mk(kVst3Process,     (void*)&h_process,     (void**)&o_process);
    return g_hooked;
}
} // namespace

extern "C" __declspec(dllexport) S::IPluginFactory* PLUGIN_API GetPluginFactory() {
    ensureCore();   // hooks installed regardless; factory is forwarded unchanged
    auto real = (S::IPluginFactory*(PLUGIN_API*)())GetProcAddress(g_core, "GetPluginFactory");
    return real ? real() : nullptr;
}
extern "C" __declspec(dllexport) bool PLUGIN_API InitDll() {
    ensureCore();
    auto real = (bool(PLUGIN_API*)())GetProcAddress(g_core, "InitDll");
    return real ? real() : true;
}
extern "C" __declspec(dllexport) bool PLUGIN_API ExitDll() {
    auto real = g_core ? (bool(PLUGIN_API*)())GetProcAddress(g_core, "ExitDll") : nullptr;
    return real ? real() : true;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
