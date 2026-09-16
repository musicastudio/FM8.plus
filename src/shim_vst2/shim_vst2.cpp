// FM8.plus VST2 wrapper. Ships as its own plug-in FM8.plus.dll beside the UNTOUCHED stock FM8.dll
// (same VST2 folder). It loads the real FM8.dll in place, wraps its AEffect dispatcher and
// processReplacing to add the features, and presents itself as a DISTINCT plug-in "FM8.plus" with its
// own uniqueID. Stock FM8 is never renamed, copied, or modified, so a Native Access reinstall cannot
// break us; plain FM8 keeps its own AEffect (our pointers are only swapped on our instances).
//
// When this DLL is instead injected into FM8.exe by the FM8.plus launcher, DllMain detects the host
// and runs the standalone attach; the VST path below stays dormant in that case.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <cwctype>
#include <cmath>
#include <string>
#include <vector>
#include "../vst2/vst2.h"
#include "../core/fm8plus.h"
#include "../core/settings.h"
#include "../core/ui.h"
#include "../core/standalone.h"

using namespace fm8plus;

namespace {
HMODULE g_self = nullptr;
HMODULE g_core = nullptr;                 // the stock FM8.dll, loaded in place (untouched)
AudioMasterCallback g_hostMaster = nullptr;
bool g_coreHooked = false;

// FM8.plus presents itself as a distinct plug-in so it can coexist with stock FM8. Stock FM8 is
// uniqueID 'Nif8' (0x4e696638); ours is 'Fm8+'. The display name is answered as "FM8.plus" below.
constexpr int32_t kFm8PlusUniqueId = 0x466D382B;   // 'F','m','8','+'
const char kFm8PlusName[] = "FM8.plus";
// FM8 is Native Instruments' synth; FM8.plus is the layer around it, so the credit names both.
const char kFm8PlusVendor[] = "Native Instruments GmbH / musica.studio";

const char kTrailerMagic[8] = {'F','M','8','P','L','U','S','2'};
constexpr int kTrailerLen = 14;           // magic(8) + arpMode + tempoMode + gainDb + morphCc(2) + reserved

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
    VstTimeInfo timeInfo{};               // scaled copy returned to FM8 for Tempo Override
    ERect editRect{};                     // scaled editor rect returned for GUI Scale
    bool customApplied = false;           // whether we have turned FM8's arp BPM-Sync off for Custom
    ui::LogoMenu logoMenu;
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

// Load the UNTOUCHED stock FM8.dll from our own folder and install the shared arp hooks against its
// module base (once). Windows dedupes by path, so if the host already loaded stock FM8.dll as plain
// FM8, we get the same module; the core detours stay inert for instances we did not wrap.
bool ensureCore() {
    if (g_core) return g_coreHooked;
    std::wstring path = selfDir() + L"\\FM8.dll";
    g_core = LoadLibraryW(path.c_str());
    if (!g_core) return false;
    settings::load(g_self);
    g_coreHooked = Core::install((void*)g_core, Bin::Vst2);
    Core::setGuiScale(settings::guiScale());   // GUI Scale is live before the first editor is built
    // Make room for the "+" before the editor form is built: serve the rebuilt header forms, or
    // fall back to patching the wordmark rect in the mapped resource.
    if (g_coreHooked) Core::serveForms(g_core);   // the "FM8+" wordmark FM8 draws itself
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
    if (st.morphCc.load(std::memory_order_relaxed) < 0) return;
    uint8_t cc = st.morphPending.exchange(0xff, std::memory_order_relaxed);
    if (cc == 0xff) return;
    float x, y; Core::morphXYFromCc(st, cc, x, y);
    // Test hook: FM8PLUS_INTERNAL_MORPH exercises the same internal setter path the VST3 and
    // standalone shims use, so the headless VST2 host can validate the EditBuffer capture.
    static const bool useInternal = getenv("FM8PLUS_INTERNAL_MORPH") != nullptr;
    if (useInternal) { Core::setMorphXY(st, x, y); return; }
    eff->setParameter(eff, 21, x);   // Morph X
    eff->setParameter(eff, 22, y);   // Morph Y
}

// Multiply the rendered output by the extra-gain setting (Increase Gain), post-fader.
void applyGain(InstanceState& st, float** out, int nOut, int32_t frames) {
    int8_t db = st.gainDb.load(std::memory_order_relaxed);
    if (db <= 0 || !out) return;
    const float g = gainLinear(db);
    for (int c = 0; c < nOut; ++c)
        if (out[c]) for (int i = 0; i < frames; ++i) out[c][i] *= g;
}

void __cdecl thunkProcess(AEffect* eff, float** in, float** out, int32_t frames) {
    Record* r = recFor(eff);
    if (!r) return;
    r->st.clearBlock();
    Core::current = &r->st;
    r->origProcess(eff, in, out, frames);   // core detours capture the morph CC and fill the out-buffer
    Core::current = nullptr;
    applyMorph(eff, r->st);                 // morph CC seen this block -> host setParameter for next block
    // Custom tempo: turn FM8's arp BPM-Sync off so its on-screen Tempo control is live; restore on exit.
    const uint8_t tm = r->st.tempoMode.load(std::memory_order_relaxed);
    if (tm == 5 && !r->customApplied) { eff->setParameter(eff, 153, 0.0f); r->customApplied = true; }
    else if (tm != 5 && r->customApplied) { eff->setParameter(eff, 153, 1.0f); r->customApplied = false; }
    applyGain(r->st, out, eff->numOutputs, frames);
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
    int8_t db = r->st.gainDb.load(std::memory_order_relaxed);
    if (db > 0 && out) { const double g = gainLinear(db);
        for (int c = 0; c < eff->numOutputs; ++c) if (out[c]) for (int i = 0; i < frames; ++i) out[c][i] *= g; }
    drainToHost(eff, *r);
}

// GUI Scale, hosted: only the host can resize the editor window it owns.
void hostResize(void* ctx, int w, int h) {
    if (g_hostMaster) g_hostMaster((AEffect*)ctx, audioMasterSizeWindow, w, h, nullptr, 0.0f);
}

intptr_t __cdecl thunkDispatch(AEffect* eff, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
    Record* r = recFor(eff);
    if (!r) return 0;
    switch (op) {
        case effProcessEvents: {
            // Block the morph CC: capture its value and remove the event so FM8 never acts on it.
            // Done at the input, upstream of FM8's queue.
            const int16_t mc = r->st.morphCc.load(std::memory_order_relaxed);
            if (mc >= 0 && ptr) {
                auto* ve = (VstEvents*)ptr;
                VstEvent** evs = &ve->events[0];
                int w = 0;
                for (int i = 0; i < ve->numEvents; ++i) {
                    VstEvent* e = evs[i];
                    bool drop = false;
                    if (e && e->type == kVstMidiType) {
                        auto* m = (VstMidiEvent*)e;
                        const uint8_t status = (uint8_t)m->midiData[0], d1 = (uint8_t)m->midiData[1];
                        if ((status & 0xf0) == 0xb0 && d1 == (uint8_t)mc) {
                            r->st.morphPending.store((uint8_t)m->midiData[2], std::memory_order_relaxed);
                            drop = true;
                        }
                    }
                    if (!drop) evs[w++] = e;
                }
                ve->numEvents = w;
            }
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        }
        case effGetChunk: {
            intptr_t n = r->origDispatcher(eff, op, idx, val, ptr, opt);
            void** pp = (void**)ptr;
            if (n <= 0 || !pp || !*pp) return n;
            r->chunkBuf.assign((char*)*pp, (char*)*pp + n);
            r->chunkBuf.append(kTrailerMagic, 8);
            const int16_t mc = r->st.morphCc.load();
            r->chunkBuf.push_back((char)r->st.arpMode.load());
            r->chunkBuf.push_back((char)r->st.tempoMode.load());
            r->chunkBuf.push_back((char)r->st.gainDb.load());
            r->chunkBuf.push_back((char)(mc & 0xff));
            r->chunkBuf.push_back((char)((mc >> 8) & 0xff));
            r->chunkBuf.push_back(0);
            *pp = r->chunkBuf.data();
            return (intptr_t)r->chunkBuf.size();
        }
        case effSetChunk: {
            // Parse our trailer if present, then hand FM8 ONLY its own bytes (val - trailer), so FM8
            // never parses our appended data at all. Safer than relying on it to ignore trailing bytes.
            intptr_t fm8Len = val;
            if (ptr && val >= kTrailerLen) {
                char* tail = (char*)ptr + val - kTrailerLen;
                if (std::memcmp(tail, kTrailerMagic, 8) == 0) {
                    r->st.arpMode.store((uint8_t)tail[8]);
                    r->st.tempoMode.store((uint8_t)tail[9]);
                    r->st.gainDb.store((int8_t)tail[10]);
                    r->st.morphCc.store((int16_t)((uint8_t)tail[11] | ((uint8_t)tail[12] << 8)));
                    fm8Len = val - kTrailerLen;
                }
            }
            return r->origDispatcher(eff, op, idx, fm8Len, ptr, opt);
        }
        case effEditGetRect: {
            // GUI Scale: FM8 answers the stock 1x rect (its own window layer applies the scale only
            // when it creates a window), so hand the host the scaled one it must make room for.
            intptr_t rv = r->origDispatcher(eff, op, idx, val, ptr, opt);
            auto** pp = (ERect**)ptr;
            const float s = Core::guiScale();
            if (rv && pp && *pp && s != 1.0f) {
                const ERect e = **pp;
                r->editRect.top = e.top;
                r->editRect.left = e.left;
                r->editRect.bottom = (int16_t)lroundf(e.top + (e.bottom - e.top) * s);
                r->editRect.right = (int16_t)lroundf(e.left + (e.right - e.left) * s);
                *pp = &r->editRect;
            }
            return rv;
        }
        case effEditOpen: {
            Core::addScaledWindow(ptr);   // before FM8 sizes its own child inside the host's window
            intptr_t rv = r->origDispatcher(eff, op, idx, val, ptr, opt);   // FM8 creates its child first,
            r->logoMenu.setHostResize(&hostResize, eff);                      // GUI Scale asks the host to resize
            r->logoMenu.attach((HWND)ptr, &r->st);                            // then we subclass that child
            return rv;
        }
        case effEditClose:
            r->st.pendingFlush.store(true);   // audio thread flushes; UI thread must not touch the buffer
            r->logoMenu.detach();
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        case effClose: {
            intptr_t rv = r->origDispatcher(eff, op, idx, val, ptr, opt);
            r->logoMenu.detach();
            r->eff.store(nullptr);   // release the record slot
            return rv;
        }
        case effStopProcess:
            Core::flushExternal(r->st);
            drainToHost(eff, *r);
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        case effGetEffectName:
        case effGetProductString:
            // Report our own name so the host lists us as "FM8.plus", distinct from stock "FM8".
            if (ptr) { std::strncpy((char*)ptr, kFm8PlusName, 31); ((char*)ptr)[31] = 0; return 1; }
            return 0;
        case effGetVendorString:
            if (ptr) { std::strncpy((char*)ptr, kFm8PlusVendor, 63); ((char*)ptr)[63] = 0; return 1; }
            return 0;
        default:
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
    }
}

AEffect* wrap(AEffect* real) {
    if (!real || real->magic != kEffectMagic) return real;
    Record* r = recAlloc(real);
    if (!r) return real;   // out of slots, pass through unwrapped
    r->st.arpMode.store((uint8_t)settings::arpModeDefault());
    r->st.morphCc.store((int16_t)settings::morphCcDefault());
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
    real->uniqueID = kFm8PlusUniqueId;   // distinct identity so we coexist with stock FM8
    return real;
}
} // namespace

intptr_t VSTCALLBACK shimMaster(AEffect* eff, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
    if (op == audioMasterCanDo && ptr) {
        const char* s = (const char*)ptr;
        if (!std::strcmp(s, "sendVstEvents") || !std::strcmp(s, "sendVstMidiEvent")) return 1;
    }
    // Tempo Override: hand FM8 a scaled copy of the host time info so the arp runs at the chosen
    // multiple. Custom (mode 5) leaves host time alone; FM8's own arp BPM drives it instead.
    if (op == audioMasterGetTime) {
        intptr_t t = g_hostMaster ? g_hostMaster(eff, op, idx, val, ptr, opt) : 0;
        Record* r = t ? recFor(eff) : nullptr;
        if (r) {
            const double f = fm8plus::tempoFactor(r->st.tempoMode.load(std::memory_order_relaxed));
            if (f != 1.0) {
                r->timeInfo = *(VstTimeInfo*)t;
                if (r->timeInfo.flags & kVstTempoValid)  r->timeInfo.tempo  *= f;
                if (r->timeInfo.flags & kVstPpqPosValid) r->timeInfo.ppqPos *= f;
                return (intptr_t)&r->timeInfo;
            }
        }
        return t;
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

// True when the running process is FM8.exe (case-insensitive), i.e. we were injected by the launcher.
static bool hostIsFm8Exe() {
    wchar_t p[MAX_PATH]; if (!GetModuleFileNameW(nullptr, p, MAX_PATH)) return false;
    std::wstring s(p); auto slash = s.find_last_of(L"\\/");
    std::wstring base = slash == std::wstring::npos ? s : s.substr(slash + 1);
    for (auto& c : base) c = (wchar_t)towlower(c);
    return base == L"fm8.exe";
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h; DisableThreadLibraryCalls(h);
        // Injected into FM8.exe by the launcher: attach the standalone features and leave the VST
        // path dormant. The GUI resources must be served before FM8 builds its GUI, so do it here
        // (the process is still suspended at injection); the heavier init is deferred onto a thread.
        if (hostIsFm8Exe()) standalone::attachExe(h);
    }
    return TRUE;
}
