// FM8.plus VST3 wrapper for macOS. The same design as src/shim_vst3/shim_vst3.cpp: FM8.plus.vst3 sits
// beside the untouched FM8.vst3, wraps FM8's factory to present "FM8.plus" under its own class IDs,
// and adds the arp's event output bus, morph, tempo, gain, the wordmark menu and GUI Scale.
//
// Where the Windows shim detours shared code, this one gives each FM8.plus instance's interfaces
// their own copy of the vtable with our entries in it (swapVtable). Plain FM8 objects keep FM8's
// vtables, so nothing needs gating, and no code page is written, so it holds on arm64 too.
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include "core_mac.h"
#include "ui_mac.h"
#include "../core/settings.h"

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/gui/iplugview.h"

using namespace fm8plus;
namespace S = Steinberg;
namespace V = Steinberg::Vst;

DEF_CLASS_IID(Steinberg::FUnknown)
DEF_CLASS_IID(Steinberg::IPluginFactory)
DEF_CLASS_IID(Steinberg::IPluginFactory2)
DEF_CLASS_IID(Steinberg::IPluginFactory3)
DEF_CLASS_IID(Steinberg::Vst::IComponent)
DEF_CLASS_IID(Steinberg::Vst::IAudioProcessor)
DEF_CLASS_IID(Steinberg::Vst::IMidiMapping)
DEF_CLASS_IID(Steinberg::Vst::IEditController)
DEF_CLASS_IID(Steinberg::Vst::IParameterChanges)

namespace {
CFBundleRef g_fm8 = nullptr;
bool g_hooked = false;
std::string g_rsrc;
constexpr uint32_t kModWheelParamId = 0x6d69646b;   // the parameter FM8 maps CC1 to (IMidiMapping)

inline bool tuidEq(const S::TUID a, const S::TUID b) { return memcmp(a, b, 16) == 0; }

// FM8's two audio classes get FM8.plus IDs: the real ID with its top bit flipped, as on Windows.
struct CidMap { S::TUID ours; S::TUID real; };
CidMap g_cidmap[8];
int g_ncid = 0;
const char* kAudioClassCategory = "Audio Module Class";
const char kPlusSuffix[] = ".plus";
const char kFm8PlusVendor[] = "Native Instruments GmbH / musica.studio";

void buildCidMap(S::IPluginFactory* real) {
    if (g_ncid || !real) return;
    for (S::int32 i = 0, n = real->countClasses(); i < n && g_ncid < 8; ++i) {
        S::PClassInfo ci{};
        if (real->getClassInfo(i, &ci) != S::kResultOk || strcmp(ci.category, kAudioClassCategory)) continue;
        memcpy(g_cidmap[g_ncid].real, ci.cid, 16);
        memcpy(g_cidmap[g_ncid].ours, ci.cid, 16);
        g_cidmap[g_ncid].ours[0] ^= (char)0x80;
        ++g_ncid;
    }
}
const char* ourForReal(const char* r) { for (int i = 0; i < g_ncid; ++i) if (tuidEq(g_cidmap[i].real, r)) return g_cidmap[i].ours; return nullptr; }
const char* realForOurs(const char* o) { for (int i = 0; i < g_ncid; ++i) if (tuidEq(g_cidmap[i].ours, o)) return g_cidmap[i].real; return nullptr; }

template <typename C> void appendSuffix(C* name, size_t cap) {
    size_t L = 0; while (L < cap && name[L]) ++L;
    for (const char* p = kPlusSuffix; *p && L + 1 < cap; ++p) name[L++] = (C)*p;
    if (L < cap) name[L] = 0;
}
template <typename C> void setVendor(C* dst, size_t cap) {
    size_t i = 0;
    for (const char* p = kFm8PlusVendor; *p && i + 1 < cap; ++p) dst[i++] = (C)*p;
    if (i < cap) dst[i] = 0;
}

// ---- per-object vtables ------------------------------------------------------------------------
// Copy `obj`'s vtable, including the offset-to-top and typeinfo words in front of it, so RTTI and
// dynamic_cast keep working, and point `obj` at the copy. Returns the original table. The interface
// may be FM8's primary base, whose table goes on with FM8's own virtuals, so copy far past the
// interface's slots; the surplus is never called through. Lives as long as the object.
void** swapVtable(void* obj) {
    constexpr int kCopy = 512;
    void** orig = *(void***)obj;
    auto** copy = new void*[kCopy + 2];
    memcpy(copy, orig - 2, sizeof(void*) * (kCopy + 2));
    *(void***)obj = copy + 2;
    return orig;
}
void setSlot(void* obj, int slot, void* fn) { (*(void***)obj)[slot] = fn; }
template <typename F> F orig(void** table, int slot) { return (F)table[slot]; }

// ---- instances ---------------------------------------------------------------------------------
// One per FM8.plus instance, found from any of its interface pointers.
struct Inst {
    std::atomic<bool> used{false};
    void* comp = nullptr; void* proc = nullptr; void* ctrl = nullptr; void* view = nullptr;
    void** compVt = nullptr; void** procVt = nullptr; void** ctrlVt = nullptr; void** viewVt = nullptr;
    InstanceState st;
    V::IMidiMapping* midiMap = nullptr;
    int16_t cachedCc = -2;
    uint32_t cachedPid = 0;
    S::IPlugFrame* frame = nullptr;
    NSView* parent = nil;   // our wrapper inside the host's view; FM8 builds its editor in it
    S::ViewRect base{};
};
constexpr int kMax = 64;
Inst g_inst[kMax];

Inst* byPtr(void* p) {
    for (auto& i : g_inst)
        if (i.used.load() && (i.comp == p || i.proc == p || i.ctrl == p || i.view == p)) return &i;
    return nullptr;
}
Inst* bySt(InstanceState* st) { for (auto& i : g_inst) if (i.used.load() && &i.st == st) return &i; return nullptr; }
Inst* alloc() {
    for (auto& i : g_inst) {
        bool e = false;
        if (i.used.compare_exchange_strong(e, true)) {
            i.comp = i.proc = i.ctrl = i.view = nullptr;
            i.midiMap = nullptr; i.cachedCc = -2; i.frame = nullptr; i.parent = nil; i.base = {};
            i.st.morphCc.store((int16_t)settings::morphCcDefault());
            i.st.arpMode.store((uint8_t)settings::arpModeDefault());
            i.st.morphRadius.store(settings::morphRadius());
            i.st.morphStartDeg.store(settings::morphStartDeg());
            i.st.tempoMode.store(0); i.st.gainDb.store(0);
            return &i;
        }
    }
    return nullptr;
}

// ---- IComponent: the added event output bus ----------------------------------------------------
enum { kGetBusCount = 7, kGetBusInfo = 8, kActivateBus = 10 };   // IComponent slots
S::int32 PLUGIN_API busCount(void* self, V::MediaType t, V::BusDirection d) {
    Inst* i = byPtr(self);
    const S::int32 n = orig<S::int32 (*)(void*, V::MediaType, V::BusDirection)>(i->compVt, kGetBusCount)(self, t, d);
    return (t == V::kEvent && d == V::kOutput) ? n + 1 : n;
}
S::int32 realBusCount(Inst* i, void* self, V::MediaType t, V::BusDirection d) {
    return orig<S::int32 (*)(void*, V::MediaType, V::BusDirection)>(i->compVt, kGetBusCount)(self, t, d);
}
S::tresult PLUGIN_API busInfo(void* self, V::MediaType t, V::BusDirection d, S::int32 idx, V::BusInfo& bus) {
    Inst* i = byPtr(self);
    if (t == V::kEvent && d == V::kOutput && idx == realBusCount(i, self, t, d)) {
        memset(&bus, 0, sizeof bus);
        bus.mediaType = V::kEvent;
        bus.direction = V::kOutput;
        bus.channelCount = 16;
        bus.busType = V::kMain;
        bus.flags = V::BusInfo::kDefaultActive;
        const char* nm = "FM8.plus Arp Out";
        for (int k = 0; nm[k]; ++k) bus.name[k] = (S::char16)nm[k];
        return S::kResultTrue;
    }
    return orig<S::tresult (*)(void*, V::MediaType, V::BusDirection, S::int32, V::BusInfo&)>(i->compVt, kGetBusInfo)(self, t, d, idx, bus);
}
S::tresult PLUGIN_API activateBus(void* self, V::MediaType t, V::BusDirection d, S::int32 idx, S::TBool on) {
    Inst* i = byPtr(self);
    if (t == V::kEvent && d == V::kOutput && idx == realBusCount(i, self, t, d)) return S::kResultTrue;
    return orig<S::tresult (*)(void*, V::MediaType, V::BusDirection, S::int32, S::TBool)>(i->compVt, kActivateBus)(self, t, d, idx, on);
}

// ---- IAudioProcessor::process ------------------------------------------------------------------
enum { kProcess = 9 };   // IAudioProcessor slot

// The value 0..127 of parameter `pid` in this block's changes, or -1.
int readCcParam(V::IParameterChanges* changes, uint32_t pid) {
    if (!changes || !pid) return -1;
    for (S::int32 k = 0, n = changes->getParameterCount(); k < n; ++k) {
        V::IParamValueQueue* q = changes->getParameterData(k);
        if (!q || q->getParameterId() != pid) continue;
        const S::int32 pts = q->getPointCount();
        S::int32 off = 0; V::ParamValue v = 0;
        if (pts > 0 && q->getPoint(pts - 1, off, v) == S::kResultTrue) return (int)(v * 127.0 + 0.5);
        return -1;
    }
    return -1;
}

// Hosts turn a CC into a parameter change (IMidiMapping), so the morph CC is hidden from FM8 here.
struct FilteredChanges : V::IParameterChanges {
    V::IParameterChanges* inner = nullptr;
    uint32_t hide = 0;
    S::tresult PLUGIN_API queryInterface(const S::TUID id, void** obj) override {
        if (tuidEq(id, V::IParameterChanges::iid) || tuidEq(id, S::FUnknown::iid)) { *obj = this; return S::kResultOk; }
        *obj = nullptr; return S::kNoInterface;
    }
    S::uint32 PLUGIN_API addRef() override { return 1; }
    S::uint32 PLUGIN_API release() override { return 1; }
    S::int32 PLUGIN_API getParameterCount() override {
        S::int32 n = 0;
        for (S::int32 k = 0, c = inner->getParameterCount(); k < c; ++k)
            if (V::IParamValueQueue* q = inner->getParameterData(k); q && q->getParameterId() != hide) ++n;
        return n;
    }
    V::IParamValueQueue* PLUGIN_API getParameterData(S::int32 index) override {
        for (S::int32 k = 0, c = inner->getParameterCount(); k < c; ++k) {
            V::IParamValueQueue* q = inner->getParameterData(k);
            if (!q || q->getParameterId() == hide) continue;
            if (index-- == 0) return q;
        }
        return nullptr;
    }
    V::IParamValueQueue* PLUGIN_API addParameterData(const V::ParamID& id, S::int32& index) override {
        return inner->addParameterData(id, index);
    }
};

uint32_t paramIdForCc(Inst* i, int16_t cc) {
    if (cc == i->cachedCc) return i->cachedPid;
    if (!i->midiMap) ((S::FUnknown*)i->comp)->queryInterface(V::IMidiMapping::iid, (void**)&i->midiMap);
    V::ParamID pid = 0;
    if (!i->midiMap || i->midiMap->getMidiControllerAssignment(0, 0, (V::CtrlNumber)cc, pid) != S::kResultTrue)
        pid = cc == 1 ? kModWheelParamId : 0;
    i->cachedCc = cc; i->cachedPid = pid;
    return pid;
}

void sendArpEvents(InstanceState& st, V::IEventList* out) {
    for (int k = 0; k < st.outCount; ++k) {
        const MidiMsg& m = st.outBuf[k];
        V::Event e{};
        e.sampleOffset = m.offset;
        const uint8_t type = m.status & 0xf0;
        const S::int16 ch = m.status & 0x0f;
        if (type == 0x90 && m.data2) {
            e.type = V::Event::kNoteOnEvent;
            e.noteOn = {ch, (S::int16)m.data1, 0.f, m.data2 / 127.0f, 0, -1};
        } else if (type == 0x80 || type == 0x90) {
            e.type = V::Event::kNoteOffEvent;
            e.noteOff = {ch, (S::int16)m.data1, m.data2 / 127.0f, -1, 0.f};
        } else {
            e.type = V::Event::kLegacyMIDICCOutEvent;
            e.midiCCOut = {m.data1, (S::int8)ch, (S::int8)m.data2, 0};
        }
        out->addEvent(e);
    }
}

S::tresult PLUGIN_API process(void* self, V::ProcessData& data) {
    Inst* i = byPtr(self);
    InstanceState& st = i->st;
    FilteredChanges filtered;
    V::IParameterChanges* changes = data.inputParameterChanges;
    if (const int16_t mc = st.morphCc.load(std::memory_order_relaxed); mc >= 0) {
        const uint32_t pid = paramIdForCc(i, mc);
        const int v = readCcParam(changes, pid);
        if (v >= 0) st.morphPending.store((uint8_t)v, std::memory_order_relaxed);
        if (pid && changes) { filtered.inner = changes; filtered.hide = pid; data.inputParameterChanges = &filtered; }
    }
    const double tf = tempoFactor(st.tempoMode.load(std::memory_order_relaxed));
    if (tf != 1.0 && data.processContext) {
        data.processContext->tempo *= tf;
        data.processContext->projectTimeMusic *= tf;
    }
    st.clearBlock();
    Core::armAudioThread();
    Core::current = &st;
    const S::tresult rv = orig<S::tresult (*)(void*, V::ProcessData&)>(i->procVt, kProcess)(self, data);
    Core::current = nullptr;
    data.inputParameterChanges = changes;
    Core::applyPendingMorphInternal(st);
    if (const int8_t db = st.gainDb.load(std::memory_order_relaxed); db > 0 && data.symbolicSampleSize == V::kSample32) {
        const float g = gainLinear(db);
        for (S::int32 b = 0; b < data.numOutputs; ++b)
            for (S::int32 c = 0; c < data.outputs[b].numChannels; ++c)
                if (float* p = data.outputs[b].channelBuffers32[c]) for (S::int32 k = 0; k < data.numSamples; ++k) p[k] *= g;
    }
    if (st.outCount > 0 && data.outputEvents) sendArpEvents(st, data.outputEvents);
    st.clearBlock();
    return rv;
}

// ---- editor ------------------------------------------------------------------------------------
enum { kCreateView = 17 };                                          // IEditController slot
enum { kAttached = 4, kRemoved = 5, kGetSize = 9, kOnSize = 10, kSetFrame = 12};   // IPlugView slots

// FM8's editor is built inside our wrapper view (i->parent), whose frame grows by the scale while
// its bounds stay logical, so Cocoa scales drawing and mouse together and FM8's own view never learns
// it is scaled (as in the VST2 and AU wrappers).
void scaleEditor(Inst* i) {
    if (!i->parent || i->base.right <= i->base.left) return;
    const CGFloat s = Core::guiScale();
    const CGFloat w = i->base.right - i->base.left, h = i->base.bottom - i->base.top;
    [i->parent setFrameSize:NSMakeSize(w * s, h * s)];
    [i->parent setBoundsSize:NSMakeSize(w, h)];
    for (NSView* v in i->parent.subviews) [v setNeedsDisplay:YES];
}

S::tresult PLUGIN_API viewGetSize(void* self, S::ViewRect* r) {
    Inst* i = byPtr(self);
    const S::tresult rv = orig<S::tresult (*)(void*, S::ViewRect*)>(i->viewVt, kGetSize)(self, r);
    if (rv != S::kResultOk || !r) return rv;
    i->base = *r;
    const float s = Core::guiScale();
    r->right = r->left + (S::int32)lroundf((r->right - r->left) * s);
    r->bottom = r->top + (S::int32)lroundf((r->bottom - r->top) * s);
    return rv;
}

// The host sizes the view in window pixels; FM8 must only ever hear its logical size, since our
// wrapper does the scaling. Passed through, FM8 grew its view to the scaled size inside the already
// scaled wrapper, drawing at twice the scale and off the window.
S::tresult PLUGIN_API viewOnSize(void* self, S::ViewRect* r) {
    Inst* i = byPtr(self);
    const float s = Core::guiScale();
    S::ViewRect logical = r ? *r : S::ViewRect{};
    if (r && s != 1.0f) {
        logical.right = logical.left + (S::int32)lroundf((r->right - r->left) / s);
        logical.bottom = logical.top + (S::int32)lroundf((r->bottom - r->top) / s);
    }
    scaleEditor(i);
    return orig<S::tresult (*)(void*, S::ViewRect*)>(i->viewVt, kOnSize)(self, r ? &logical : r);
}

S::tresult PLUGIN_API viewSetFrame(void* self, S::IPlugFrame* frame) {
    Inst* i = byPtr(self);
    i->frame = frame;
    return orig<S::tresult (*)(void*, S::IPlugFrame*)>(i->viewVt, kSetFrame)(self, frame);
}

S::tresult PLUGIN_API viewAttached(void* self, void* parent, S::FIDString type) {
    Inst* i = byPtr(self);
    Core::serveLogoMac(g_rsrc.c_str());   // FM8 empties its resource map with its last instance
    if (i->base.right <= i->base.left) { S::ViewRect r{}; viewGetSize(self, &r); }
    void* into = parent;
    if (parent && type && !strcmp(type, S::kPlatformTypeNSView)) {
        NSView* host = (__bridge NSView*)parent;
        [i->parent removeFromSuperview];
        i->parent = [[NSView alloc] initWithFrame:host.bounds];
        [host addSubview:i->parent];
        scaleEditor(i);
        into = (__bridge void*)i->parent;
    }
    const S::tresult rv = orig<S::tresult (*)(void*, void*, S::FIDString)>(i->viewVt, kAttached)(self, into, type);
    if (rv == S::kResultOk && i->parent) {
        if (!i->st.appObj.load()) Core::bindInstance(&i->st, i->comp);
    } else {
        [i->parent removeFromSuperview];
        i->parent = nil;
    }
    return rv;
}

S::tresult PLUGIN_API viewRemoved(void* self) {
    Inst* i = byPtr(self);
    i->st.pendingFlush.store(true);
    const S::tresult rv = orig<S::tresult (*)(void*)>(i->viewVt, kRemoved)(self);
    [i->parent removeFromSuperview];
    i->parent = nil;
    return rv;
}

void* PLUGIN_API createView(void* self, S::FIDString name) {
    Inst* i = byPtr(self);
    void* view = orig<void* (*)(void*, S::FIDString)>(i->ctrlVt, kCreateView)(self, name);
    if (!view) return view;
    i->view = view;
    i->viewVt = swapVtable(view);
    setSlot(view, kAttached, (void*)&viewAttached);
    setSlot(view, kRemoved, (void*)&viewRemoved);
    setSlot(view, kGetSize, (void*)&viewGetSize);
    setSlot(view, kOnSize, (void*)&viewOnSize);
    setSlot(view, kSetFrame, (void*)&viewSetFrame);
    return view;
}

void applyScale(void* ctx, float) {
    auto* i = (Inst*)ctx;
    scaleEditor(i);
    if (i->frame && i->view) {
        S::ViewRect r{};
        viewGetSize(i->view, &r);
        i->frame->resizeView((S::IPlugView*)i->view, &r);
    }
}

void onLogo(InstanceState* st) {
    Inst* i = bySt(st);
    macui::ScaleHost sh;
    if (i && i->parent) { sh.apply = &applyScale; sh.ctx = i; }
    macui::showMenu(st, sh);
}

// Give a new FM8.plus instance its own vtables for the three interfaces we sit on.
void adopt(S::FUnknown* unk) {
    Inst* i = alloc();
    if (!i) return;   // out of slots: the instance still works, as plain FM8
    auto get = [unk](const S::TUID iid) {
        void* p = nullptr;
        if (unk->queryInterface(iid, &p) == S::kResultOk && p) ((S::FUnknown*)p)->release();
        return p;
    };
    i->comp = get(V::IComponent::iid);
    i->proc = get(V::IAudioProcessor::iid);
    i->ctrl = get(V::IEditController::iid);   // FM8 is single-component: the controller is the same object
    if (i->comp) {
        i->compVt = swapVtable(i->comp);
        setSlot(i->comp, kGetBusCount, (void*)&busCount);
        setSlot(i->comp, kGetBusInfo, (void*)&busInfo);
        setSlot(i->comp, kActivateBus, (void*)&activateBus);
    }
    if (i->proc) { i->procVt = swapVtable(i->proc); setSlot(i->proc, kProcess, (void*)&process); }
    if (i->ctrl) { i->ctrlVt = swapVtable(i->ctrl); setSlot(i->ctrl, kCreateView, (void*)&createView); }
    Core::bindInstance(&i->st, i->comp);
}

// ---- factory -----------------------------------------------------------------------------------
class Factory : public S::IPluginFactory3 {
    S::IPluginFactory* f1_ = nullptr;
    S::IPluginFactory2* f2_ = nullptr;
    S::IPluginFactory3* f3_ = nullptr;
    std::atomic<S::uint32> ref_{1};
public:
    explicit Factory(S::IPluginFactory* base) : f1_(base) {
        f1_->addRef();
        if (f1_->queryInterface(S::IPluginFactory2::iid, (void**)&f2_) != S::kResultOk) f2_ = nullptr;
        if (f1_->queryInterface(S::IPluginFactory3::iid, (void**)&f3_) != S::kResultOk) f3_ = nullptr;
        buildCidMap(f1_);
    }
    virtual ~Factory() { if (f3_) f3_->release(); if (f2_) f2_->release(); f1_->release(); }

    S::tresult PLUGIN_API queryInterface(const S::TUID riid, void** obj) override {
        if (tuidEq(riid, S::FUnknown::iid) || tuidEq(riid, S::IPluginFactory::iid) ||
            (f2_ && tuidEq(riid, S::IPluginFactory2::iid)) || (f3_ && tuidEq(riid, S::IPluginFactory3::iid))) {
            addRef(); *obj = this; return S::kResultOk;
        }
        *obj = nullptr; return S::kNoInterface;
    }
    S::uint32 PLUGIN_API addRef() override { return ++ref_; }
    S::uint32 PLUGIN_API release() override { const S::uint32 r = --ref_; if (!r) delete this; return r; }

    S::tresult PLUGIN_API getFactoryInfo(S::PFactoryInfo* info) override {
        const S::tresult r = f1_->getFactoryInfo(info);
        if (r == S::kResultOk && info) setVendor(info->vendor, sizeof info->vendor);
        return r;
    }
    S::int32 PLUGIN_API countClasses() override { return f1_->countClasses(); }
    S::tresult PLUGIN_API getClassInfo(S::int32 k, S::PClassInfo* info) override {
        const S::tresult r = f1_->getClassInfo(k, info);
        if (r == S::kResultOk && !strcmp(info->category, kAudioClassCategory)) {
            if (const char* o = ourForReal(info->cid)) memcpy(info->cid, o, 16);
            appendSuffix(info->name, sizeof info->name);
        }
        return r;
    }
    S::tresult PLUGIN_API createInstance(S::FIDString cid, S::FIDString iid, void** obj) override {
        const char* real = realForOurs(cid);
        const S::tresult r = f1_->createInstance(real ? real : cid, iid, obj);
        if (real && g_hooked && r == S::kResultOk && obj && *obj) adopt((S::FUnknown*)*obj);
        return r;
    }
    S::tresult PLUGIN_API getClassInfo2(S::int32 k, S::PClassInfo2* info) override {
        if (!f2_) return S::kNotImplemented;
        const S::tresult r = f2_->getClassInfo2(k, info);
        if (r == S::kResultOk && !strcmp(info->category, kAudioClassCategory)) {
            if (const char* o = ourForReal(info->cid)) memcpy(info->cid, o, 16);
            appendSuffix(info->name, sizeof info->name);
            setVendor(info->vendor, sizeof info->vendor);
        }
        return r;
    }
    S::tresult PLUGIN_API getClassInfoUnicode(S::int32 k, S::PClassInfoW* info) override {
        if (!f3_) return S::kNotImplemented;
        const S::tresult r = f3_->getClassInfoUnicode(k, info);
        if (r == S::kResultOk && !strcmp(info->category, kAudioClassCategory)) {
            if (const char* o = ourForReal(info->cid)) memcpy(info->cid, o, 16);
            appendSuffix(info->name, sizeof info->name / sizeof info->name[0]);
            setVendor(info->vendor, sizeof info->vendor / sizeof info->vendor[0]);
        }
        return r;
    }
    S::tresult PLUGIN_API setHostContext(S::FUnknown* ctx) override { return f3_ ? f3_->setHostContext(ctx) : S::kNotImplemented; }
};

std::string fm8BundlePath() {
    Dl_info di{};
    dladdr((void*)&fm8BundlePath, &di);
    const std::string self = di.dli_fname ? di.dli_fname : "";
    const size_t at = self.rfind(".vst3/Contents/MacOS/");
    if (at != std::string::npos) {
        const std::string p = self.substr(0, self.rfind('/', at) + 1) + "FM8.vst3";
        if (access((p + "/Contents/MacOS/FM8").c_str(), R_OK) == 0) return p;
    }
    return "/Library/Audio/Plug-Ins/VST3/FM8.vst3";
}

bool ensureCore() {
    if (g_fm8) return true;
    const std::string path = fm8BundlePath();
    g_fm8 = CFBundleCreate(nullptr, (__bridge CFURLRef)[NSURL fileURLWithPath:@(path.c_str())]);
    if (!g_fm8 || !CFBundleLoadExecutable(g_fm8)) return false;
    auto entry = (bool (*)(CFBundleRef))CFBundleGetFunctionPointerForName(g_fm8, CFSTR("bundleEntry"));
    if (entry && !entry(g_fm8)) return false;   // FM8's own module init, with its own bundle
    settings::load(nullptr);
    g_rsrc = path + "/Contents/Resources/FM8.rsrc";
    g_hooked = Core::installMac(CFBundleGetFunctionPointerForName(g_fm8, CFSTR("GetPluginFactory")));
    Core::setGuiScale(settings::guiScale());
    if (g_hooked) { Core::serveLogoMac(g_rsrc.c_str()); Core::setLogoHandler(&onLogo); }
    return true;
}
} // namespace

extern "C" {
__attribute__((visibility("default"))) bool bundleEntry(CFBundleRef) { return ensureCore(); }
__attribute__((visibility("default"))) bool bundleExit() {
    auto exitFn = g_fm8 ? (bool (*)())CFBundleGetFunctionPointerForName(g_fm8, CFSTR("bundleExit")) : nullptr;
    return exitFn ? exitFn() : true;
}
__attribute__((visibility("default"))) S::IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (!ensureCore()) return nullptr;
    auto real = (S::IPluginFactory* (*)())CFBundleGetFunctionPointerForName(g_fm8, CFSTR("GetPluginFactory"));
    S::IPluginFactory* rf = real ? real() : nullptr;
    if (!rf) return nullptr;
    return g_hooked ? new Factory(rf) : rf;
}
}
