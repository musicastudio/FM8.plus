// FM8.plus VST2 proxy. Ships as FM8.dll beside the renamed original (FM8.plus.core). Forwards the
// real plugin unchanged, then wraps its AEffect dispatcher and processReplacing to add the two
// features. No FM8 export or uniqueID changes, so existing projects keep loading.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>
#include "../vst2/vst2.h"
#include "../core/fm8plus.h"
#include "../core/settings.h"
#include "../core/ui.h"

using namespace fm8plus;

namespace {
HMODULE g_self = nullptr;
HMODULE g_core = nullptr;                 // the renamed real FM8.dll
AudioMasterCallback g_hostMaster = nullptr;
bool g_coreHooked = false;

const char kTrailerMagic[8] = {'F','M','8','P','L','U','S','1'};
constexpr int kTrailerLen = 12;           // magic(8) + modwheel(1) + arpmode(1) + reserved(2)

// One record per plugin instance. Fixed array, linear scan by AEffect* on the audio thread
// (allocation-free, instance count is tiny).
struct Record {
    std::atomic<AEffect*> eff{nullptr};
    InstanceState st;
    AEffectDispatcherProc origDispatcher = nullptr;
    AEffectProcessProc     origProcess = nullptr;
    AEffectProcessDoubleProc origProcessD = nullptr;
    std::string chunkBuf;                 // persists our effGetChunk return (dispatch thread only)
    std::vector<char> drainBuf;           // preallocated VstEvents scratch (audio thread, no alloc)
    ui::Overlay overlay;
};
constexpr int kMaxInst = 64;
Record g_rec[kMaxInst];

Record* recFor(AEffect* e) {
    for (auto& r : g_rec) if (r.eff.load(std::memory_order_relaxed) == e) return &r;
    return nullptr;
}
Record* recAlloc(AEffect* e) {
    for (auto& r : g_rec) { AEffect* exp = nullptr; if (r.eff.compare_exchange_strong(exp, e)) return &r; }
    return nullptr;
}

std::wstring selfDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p); auto slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

// Load the renamed original and install the shared arp hooks against its module base (once).
bool ensureCore() {
    if (g_core) return g_coreHooked;
    std::wstring path = selfDir() + L"\\FM8.plus.core";
    g_core = LoadLibraryW(path.c_str());
    if (!g_core) return false;
    settings::load(g_self);
    g_coreHooked = Core::install((void*)g_core, Bin::Vst2);
    return g_coreHooked;
}

// Send the instance's queued arp events to the host via audioMasterProcessEvents. Uses the record's
// preallocated buffer, so nothing is heap-allocated on the audio thread.
void drainToHost(AEffect* eff, Record& r) {
    InstanceState& st = r.st;
    if (st.outCount <= 0) return;
    // VstEvents header + pointer array, then the VstMidiEvent bodies.
    const int n = st.outCount;
    const size_t hdrSize = offsetof(VstEvents, events);   // 16 on x64 (numEvents + pad + reserved)
    char* buf = r.drainBuf.data();
    auto* hdr = (VstEvents*)buf;
    hdr->numEvents = n;
    hdr->reserved = 0;
    auto* ptrs = (VstEvent**)(buf + hdrSize);
    auto* bodies = (VstMidiEvent*)(buf + hdrSize + sizeof(void*) * n);
    for (int i = 0; i < n; ++i) {
        VstMidiEvent& m = bodies[i];
        std::memset(&m, 0, sizeof m);
        m.type = kVstMidiType;
        m.byteSize = sizeof(VstMidiEvent);
        m.deltaFrames = st.outBuf[i].offset;
        m.midiData[0] = (char)st.outBuf[i].status;
        m.midiData[1] = (char)st.outBuf[i].data1;
        m.midiData[2] = (char)st.outBuf[i].data2;
        ptrs[i] = (VstEvent*)&m;
    }
    g_hostMaster(eff, audioMasterProcessEvents, 0, 0, hdr, 0.0f);
    st.clearBlock();
}

void applyMorph(AEffect* eff, InstanceState& st) {
    if (!st.modWheelMorph.load(std::memory_order_relaxed)) return;
    uint8_t cc = st.lastCc1.exchange(0xff, std::memory_order_relaxed);
    if (cc == 0xff) return;
    float x, y; Core::morphXYFromCc(st, cc, x, y);
    // Test hook: FM8PLUS_INTERNAL_MORPH exercises the same internal setter path the VST3 and
    // standalone shims use, so the headless VST2 host can validate the EditBuffer capture.
    static const bool useInternal = getenv("FM8PLUS_INTERNAL_MORPH") != nullptr;
    if (useInternal) { Core::setMorphXY(st, x, y); return; }
    eff->setParameter(eff, 21, x);   // Morph X
    eff->setParameter(eff, 22, y);   // Morph Y
}

void __cdecl thunkProcess(AEffect* eff, float** in, float** out, int32_t frames) {
    Record* r = recFor(eff);
    if (!r) return;
    r->st.clearBlock();
    Core::current = &r->st;
    r->origProcess(eff, in, out, frames);   // core detours capture CC1 and fill the out-buffer
    Core::current = nullptr;
    applyMorph(eff, r->st);                 // CC1 seen this block -> host setParameter for next block
    drainToHost(eff, *r);
}

void __cdecl thunkProcessD(AEffect* eff, double** in, double** out, int32_t frames) {
    Record* r = recFor(eff);
    if (!r || !r->origProcessD) return;
    r->st.clearBlock();
    Core::current = &r->st;
    r->origProcessD(eff, in, out, frames);
    Core::current = nullptr;
    applyMorph(eff, r->st);
    drainToHost(eff, *r);
}

intptr_t __cdecl thunkDispatch(AEffect* eff, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
    Record* r = recFor(eff);
    if (!r) return 0;
    switch (op) {
        case effGetChunk: {
            intptr_t n = r->origDispatcher(eff, op, idx, val, ptr, opt);
            void** pp = (void**)ptr;
            if (n <= 0 || !pp || !*pp) return n;
            r->chunkBuf.assign((char*)*pp, (char*)*pp + n);
            r->chunkBuf.append(kTrailerMagic, 8);
            r->chunkBuf.push_back((char)(r->st.modWheelMorph.load() ? 1 : 0));
            r->chunkBuf.push_back((char)r->st.arpMode.load());
            r->chunkBuf.push_back(0); r->chunkBuf.push_back(0);
            *pp = r->chunkBuf.data();
            return (intptr_t)r->chunkBuf.size();
        }
        case effSetChunk: {
            // Parse our trailer from the tail if present, then forward the whole blob (FM8 ignores
            // trailing bytes, verified against the stock plugin).
            if (ptr && val >= kTrailerLen) {
                char* tail = (char*)ptr + val - kTrailerLen;
                if (std::memcmp(tail, kTrailerMagic, 8) == 0) {
                    r->st.modWheelMorph.store(tail[8] != 0);
                    r->st.arpMode.store((uint8_t)tail[9]);
                    r->overlay.refresh(r->st);
                }
            }
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        }
        case effEditOpen:
            r->overlay.attach((HWND)ptr, &r->st, settings::self());
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        case effEditClose:
            Core::flushExternal(r->st);
            r->overlay.detach();
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        case effClose: {
            intptr_t rv = r->origDispatcher(eff, op, idx, val, ptr, opt);
            r->overlay.detach();
            r->eff.store(nullptr);   // release the record slot
            return rv;
        }
        case effStopProcess:
            Core::flushExternal(r->st);
            drainToHost(eff, *r);
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        default:
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
    }
}

AEffect* wrap(AEffect* real) {
    if (!real || real->magic != kEffectMagic) return real;
    Record* r = recAlloc(real);
    if (!r) return real;   // out of slots, pass through unwrapped
    r->st.arpMode.store((uint8_t)settings::arpModeDefault());
    r->st.modWheelMorph.store(settings::defaultModWheelMorph());
    r->st.morphRadius.store(settings::morphRadius());
    r->st.morphStartDeg.store(settings::morphStartDeg());
    r->drainBuf.resize(offsetof(VstEvents, events) +
                       InstanceState::kMaxOut * (sizeof(void*) + sizeof(VstMidiEvent)));
    r->origDispatcher = real->dispatcher;
    r->origProcess = real->processReplacing;
    r->origProcessD = real->processDoubleReplacing;
    real->dispatcher = &thunkDispatch;
    real->processReplacing = &thunkProcess;
    if (real->processDoubleReplacing) real->processDoubleReplacing = &thunkProcessD;
    return real;
}
} // namespace

intptr_t VSTCALLBACK shimMaster(AEffect* eff, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
    if (op == audioMasterCanDo && ptr) {
        const char* s = (const char*)ptr;
        if (!std::strcmp(s, "sendVstEvents") || !std::strcmp(s, "sendVstMidiEvent")) return 1;
    }
    return g_hostMaster ? g_hostMaster(eff, op, idx, val, ptr, opt) : 0;
}

extern "C" __declspec(dllexport) AEffect* VSTPluginMain(AudioMasterCallback host) {
    g_hostMaster = host;
    if (!ensureCore()) {
        // Degrade to plain forwarding if the core is missing or the build is unexpected.
        if (g_core) {
            auto real = (AEffect*(*)(AudioMasterCallback))GetProcAddress(g_core, "VSTPluginMain");
            if (real) return real(host);
        }
        return nullptr;
    }
    auto realMain = (AEffect*(*)(AudioMasterCallback))GetProcAddress(g_core, "VSTPluginMain");
    if (!realMain) return nullptr;
    AEffect* real = realMain(&shimMaster);
    return g_coreHooked ? wrap(real) : real;
}

// FM8's FX shim resolves this symbol; forward it so the effect variant keeps working.
extern "C" __declspec(dllexport) void* NICreatePlugInInstance(void* a, uint32_t b, uint32_t c, uint32_t d) {
    if (!ensureCore()) return nullptr;
    auto real = (void*(*)(void*, uint32_t, uint32_t, uint32_t))GetProcAddress(g_core, "NICreatePlugInInstance");
    return real ? real(a, b, c, d) : nullptr;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
