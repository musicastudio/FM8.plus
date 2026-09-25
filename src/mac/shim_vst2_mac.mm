// FM8.plus VST2 wrapper for macOS. The same design as src/shim_vst2/shim_vst2.cpp: FM8.plus.vst sits
// beside the untouched FM8.vst, loads it in place, wraps its AEffect, and presents itself as the
// distinct plug-in "FM8.plus". FM8's own bundle is never modified.
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#define VSTCALLBACK
#include "../vst2/vst2.h"
#include "core_mac.h"
#include "ui_mac.h"
#include "../core/settings.h"

using namespace fm8plus;

namespace {
AudioMasterCallback g_hostMaster = nullptr;
CFBundleRef g_fm8 = nullptr;
bool g_coreHooked = false;
std::string g_rsrc;   // FM8.vst/Contents/Resources/FM8.rsrc, for the "FM8+" wordmark

constexpr int32_t kFm8PlusUniqueId = 0x466D382B;   // 'Fm8+', as on Windows
const char kFm8PlusName[] = "FM8.plus";
const char kFm8PlusVendor[] = "Native Instruments GmbH / musica.studio";
const char kTrailerMagic[8] = {'F','M','8','P','L','U','S','2'};
constexpr int kTrailerLen = 14;
constexpr int kParamMorphX = 21, kParamMorphY = 22, kParamArpBpmSync = 153;

struct Record {
    std::atomic<AEffect*> eff{nullptr};
    InstanceState st;
    AEffectDispatcherProc origDispatcher = nullptr;
    AEffectProcessProc origProcess = nullptr;
    AEffectProcessDoubleProc origProcessD = nullptr;
    std::string chunkBuf;
    std::vector<char> drainBuf;
    VstTimeInfo timeInfo{};
    ERect editRect{}, baseRect{};
    bool customApplied = false;
    NSView* parent = nil;   // the host's view the editor lives in
};
constexpr int kMaxInst = 64;
Record g_rec[kMaxInst];

Record* recFor(AEffect* e) {
    for (auto& r : g_rec) if (r.eff.load(std::memory_order_relaxed) == e) return &r;
    return nullptr;
}
Record* recOf(InstanceState* st) {
    for (auto& r : g_rec) if (&r.st == st) return &r;
    return nullptr;
}
Record* recAlloc(AEffect* e) {
    for (auto& r : g_rec) { AEffect* exp = nullptr; if (r.eff.compare_exchange_strong(exp, e)) return &r; }
    return nullptr;
}

// FM8.vst in the same folder as us, else the system VST folder.
std::string fm8BundlePath() {
    Dl_info di{};
    dladdr((void*)&fm8BundlePath, &di);
    std::string self = di.dli_fname ? di.dli_fname : "";
    const size_t at = self.rfind(".vst/Contents/MacOS/");
    if (at != std::string::npos) {
        std::string dir = self.substr(0, self.rfind('/', at) + 1);
        std::string p = dir + "FM8.vst";
        if (access((p + "/Contents/MacOS/FM8").c_str(), R_OK) == 0) return p;
    }
    return "/Library/Audio/Plug-Ins/VST/FM8.vst";
}

using MainFn = AEffect* (*)(AudioMasterCallback);
MainFn g_realMain = nullptr;

bool ensureCore() {
    if (g_fm8) return g_coreHooked;
    const std::string path = fm8BundlePath();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), (CFIndex)path.size(), true);
    g_fm8 = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (!g_fm8 || !CFBundleLoadExecutable(g_fm8)) return false;
    g_realMain = (MainFn)CFBundleGetFunctionPointerForName(g_fm8, CFSTR("VSTPluginMain"));
    if (!g_realMain) return false;
    settings::load(nullptr);
    g_rsrc = path + "/Contents/Resources/FM8.rsrc";
    g_coreHooked = Core::installMac((const void*)g_realMain);
    Core::setGuiScale(settings::guiScale());
    if (g_coreHooked) Core::serveLogoMac(g_rsrc.c_str());
    return g_coreHooked;
}

void drainToHost(AEffect* eff, Record& r) {
    InstanceState& st = r.st;
    if (st.outCount <= 0) return;
    const int n = st.outCount;
    const size_t hdrSize = offsetof(VstEvents, events);
    char* buf = r.drainBuf.data();
    auto* hdr = (VstEvents*)buf;
    hdr->numEvents = n;
    hdr->reserved = 0;
    auto* ptrs = (VstEvent**)(buf + hdrSize);
    auto* bodies = (VstMidiEvent*)(buf + hdrSize + sizeof(void*) * n);
    for (int i = 0; i < n; ++i) {
        VstMidiEvent& m = bodies[i];
        memset(&m, 0, sizeof m);
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
    const uint8_t cc = st.morphPending.exchange(0xff, std::memory_order_relaxed);
    if (cc == 0xff) return;
    float x, y; Core::morphXYFromCc(st, cc, x, y);
    static const bool useInternal = getenv("FM8PLUS_INTERNAL_MORPH") != nullptr;   // test hook, as on Windows
    if (useInternal) { Core::setMorphXY(st, x, y); return; }
    eff->setParameter(eff, kParamMorphX, x);
    eff->setParameter(eff, kParamMorphY, y);
}

template <typename T>
void applyGain(InstanceState& st, T** out, int nOut, int32_t frames) {
    const int8_t db = st.gainDb.load(std::memory_order_relaxed);
    if (db <= 0 || !out) return;
    const T g = (T)gainLinear(db);
    for (int c = 0; c < nOut; ++c)
        if (out[c]) for (int i = 0; i < frames; ++i) out[c][i] *= g;
}

void afterProcess(AEffect* eff, Record& r) {
    applyMorph(eff, r.st);
    const uint8_t tm = r.st.tempoMode.load(std::memory_order_relaxed);
    if (tm == 5 && !r.customApplied) { eff->setParameter(eff, kParamArpBpmSync, 0.0f); r.customApplied = true; }
    else if (tm != 5 && r.customApplied) { eff->setParameter(eff, kParamArpBpmSync, 1.0f); r.customApplied = false; }
}

void thunkProcess(AEffect* eff, float** in, float** out, int32_t frames) {
    Record* r = recFor(eff);
    if (!r) return;
    r->st.clearBlock();
    Core::armAudioThread();
    Core::current = &r->st;
    r->origProcess(eff, in, out, frames);
    Core::current = nullptr;
    afterProcess(eff, *r);
    applyGain(r->st, out, eff->numOutputs, frames);
    drainToHost(eff, *r);
}

void thunkProcessD(AEffect* eff, double** in, double** out, int32_t frames) {
    Record* r = recFor(eff);
    if (!r || !r->origProcessD) return;
    r->st.clearBlock();
    Core::armAudioThread();
    Core::current = &r->st;
    r->origProcessD(eff, in, out, frames);
    Core::current = nullptr;
    afterProcess(eff, *r);
    applyGain(r->st, out, eff->numOutputs, frames);
    drainToHost(eff, *r);
}

// GUI Scale: FM8's view keeps drawing and hit-testing in its own logical coordinates (its bounds),
// while its frame, and the host window around it, grow by the scale.
void scaleEditor(Record& r) {
    if (!r.parent) return;
    const float s = Core::guiScale();
    const CGFloat w = r.baseRect.right - r.baseRect.left, h = r.baseRect.bottom - r.baseRect.top;
    if (w <= 0 || h <= 0) return;
    for (NSView* v in r.parent.subviews) {
        [v setFrameSize:NSMakeSize(w * s, h * s)];
        [v setBoundsSize:NSMakeSize(w, h)];
        [v setNeedsDisplay:YES];
    }
}

void applyScale(void* ctx, float) {
    auto* r = (Record*)ctx;
    scaleEditor(*r);
    const float s = Core::guiScale();
    const int w = (int)lroundf((r->baseRect.right - r->baseRect.left) * s);
    const int h = (int)lroundf((r->baseRect.bottom - r->baseRect.top) * s);
    if (g_hostMaster) g_hostMaster(r->eff.load(), audioMasterSizeWindow, w, h, nullptr, 0.0f);
}

void onLogo(InstanceState* st) {
    Record* r = recOf(st);
    macui::ScaleHost sh;
    if (r && r->parent) { sh.apply = &applyScale; sh.ctx = r; }
    macui::showMenu(st, sh);
}

intptr_t thunkDispatch(AEffect* eff, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
    Record* r = recFor(eff);
    if (!r) return 0;
    switch (op) {
        case effOpen: {
            const intptr_t rv = r->origDispatcher(eff, op, idx, val, ptr, opt);
            Core::bindInstance(&r->st, eff->object);
            return rv;
        }
        case effProcessEvents: {
            const int16_t mc = r->st.morphCc.load(std::memory_order_relaxed);
            if (mc >= 0 && ptr) {
                auto* ve = (VstEvents*)ptr;
                int w = 0;
                for (int i = 0; i < ve->numEvents; ++i) {
                    VstEvent* e = ve->events[i];
                    bool drop = false;
                    if (e && e->type == kVstMidiType) {
                        auto* m = (VstMidiEvent*)e;
                        if (((uint8_t)m->midiData[0] & 0xf0) == 0xb0 && (uint8_t)m->midiData[1] == (uint8_t)mc) {
                            r->st.morphPending.store((uint8_t)m->midiData[2], std::memory_order_relaxed);
                            drop = true;
                        }
                    }
                    if (!drop) ve->events[w++] = e;
                }
                ve->numEvents = w;
            }
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        }
        case effGetChunk: {
            const intptr_t n = r->origDispatcher(eff, op, idx, val, ptr, opt);
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
            intptr_t fm8Len = val;
            if (ptr && val >= kTrailerLen) {
                const char* tail = (const char*)ptr + val - kTrailerLen;
                if (!memcmp(tail, kTrailerMagic, 8)) {
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
            const intptr_t rv = r->origDispatcher(eff, op, idx, val, ptr, opt);
            auto** pp = (ERect**)ptr;
            if (!rv || !pp || !*pp) return rv;
            r->baseRect = **pp;
            const float s = Core::guiScale();
            if (s != 1.0f) {
                r->editRect = r->baseRect;
                r->editRect.bottom = (int16_t)lroundf(r->baseRect.top + (r->baseRect.bottom - r->baseRect.top) * s);
                r->editRect.right = (int16_t)lroundf(r->baseRect.left + (r->baseRect.right - r->baseRect.left) * s);
                *pp = &r->editRect;
            }
            return rv;
        }
        case effEditOpen: {
            Core::serveLogoMac(g_rsrc.c_str());   // FM8 empties its resource map with its last instance
            if (!r->baseRect.right) { ERect* er = nullptr; thunkDispatch(eff, effEditGetRect, 0, 0, &er, 0); }
            const intptr_t rv = r->origDispatcher(eff, op, idx, val, ptr, opt);
            r->parent = (__bridge NSView*)ptr;
            if (!r->st.appObj.load()) Core::bindInstance(&r->st, eff->object);
            if (Core::guiScale() != 1.0f) scaleEditor(*r);
            return rv;
        }
        case effEditClose:
            r->st.pendingFlush.store(true);
            r->parent = nil;
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        case effClose: {
            const intptr_t rv = r->origDispatcher(eff, op, idx, val, ptr, opt);
            Core::unbindInstance(&r->st);
            r->parent = nil;
            r->eff.store(nullptr);
            return rv;
        }
        case effStopProcess:
            Core::flushExternal(r->st);
            drainToHost(eff, *r);
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
        case effGetEffectName:
        case effGetProductString:
            if (ptr) { strncpy((char*)ptr, kFm8PlusName, 31); ((char*)ptr)[31] = 0; return 1; }
            return 0;
        case effGetVendorString:
            if (ptr) { strncpy((char*)ptr, kFm8PlusVendor, 63); ((char*)ptr)[63] = 0; return 1; }
            return 0;
        default:
            return r->origDispatcher(eff, op, idx, val, ptr, opt);
    }
}

AEffect* wrap(AEffect* real) {
    if (!real || real->magic != kEffectMagic) return real;
    Record* r = recAlloc(real);
    if (!r) return real;
    r->st.arpMode.store((uint8_t)settings::arpModeDefault());
    r->st.morphCc.store((int16_t)settings::morphCcDefault());
    r->st.morphRadius.store(settings::morphRadius());
    r->st.morphStartDeg.store(settings::morphStartDeg());
    r->st.tempoMode.store(0); r->st.gainDb.store(0); r->customApplied = false;
    r->baseRect = {}; r->parent = nil;
    r->drainBuf.resize(offsetof(VstEvents, events) + InstanceState::kMaxOut * (sizeof(void*) + sizeof(VstMidiEvent)));
    r->origDispatcher = real->dispatcher;
    r->origProcess = real->processReplacing;
    r->origProcessD = real->processDoubleReplacing;
    real->dispatcher = &thunkDispatch;
    real->processReplacing = &thunkProcess;
    if (real->processDoubleReplacing) real->processDoubleReplacing = &thunkProcessD;
    real->uniqueID = kFm8PlusUniqueId;
    Core::bindInstance(&r->st, real->object);
    return real;
}

intptr_t shimMaster(AEffect* eff, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
    if (op == audioMasterCanDo && ptr) {
        const char* s = (const char*)ptr;
        if (!strcmp(s, "sendVstEvents") || !strcmp(s, "sendVstMidiEvent")) return 1;
    }
    if (op == audioMasterGetTime) {
        const intptr_t t = g_hostMaster ? g_hostMaster(eff, op, idx, val, ptr, opt) : 0;
        Record* r = t ? recFor(eff) : nullptr;
        if (r) {
            const double f = tempoFactor(r->st.tempoMode.load(std::memory_order_relaxed));
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
} // namespace

extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(AudioMasterCallback host) {
    g_hostMaster = host;
    if (!ensureCore()) return g_realMain ? g_realMain(host) : nullptr;   // unknown FM8: plain passthrough
    Core::setLogoHandler(&onLogo);
    return wrap(g_realMain(&shimMaster));
}

