// FM8.plus VST3 wrapper. Ships as its own plug-in FM8.plus.vst3 beside the UNTOUCHED stock FM8.vst3
// (Common Files\VST3). It loads the real FM8.vst3 in place and exposes a DISTINCT plug-in "FM8.plus"
// (its own class UID) by wrapping FM8's factory. Only instances created through our factory get the
// features: an event OUTPUT bus ("FM8.plus Arp Out") FM8 does not have, arp-note draining into
// data.outputEvents, morph on the selected CC, tempo override, and extra gain. Plain FM8 VST3
// instances share the same module but are left completely stock (hooks are gated to our instances).
//
// Stock FM8.vst3 is never renamed, copied, or modified, so a Native Access reinstall cannot break us.
#include <windows.h>
#include <atomic>
#include <cstring>
#include <cmath>
#include "../core/fm8plus.h"
#include "../core/settings.h"
#include "../core/ui.h"
#include "MinHook.h"

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

// The SDK declares interface IIDs but leaves their storage to the client; define the ones we use.
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
HMODULE g_self = nullptr, g_core = nullptr;
bool g_hooked = false;

inline bool tuidEq(const S::TUID a, const S::TUID b) { return std::memcmp(a, b, 16) == 0; }

// FM8.vst3 exposes two audio classes ("FM8" and "FM8 FX"), each with its own UID. We give each a
// DISTINCT FM8+ UID (the real UID with its first byte flipped: deterministic, unique, and different
// from stock so both can coexist) and map ours back to the real one at createInstance.
struct CidMap { S::TUID ours; S::TUID real; };
CidMap g_cidmap[8];
int    g_ncid = 0;
bool   g_cidDone = false;
const char* kAudioClassCategory = "Audio Module Class";

void buildCidMap(S::IPluginFactory* real) {
    if (g_cidDone || !real) return;
    g_cidDone = true;
    S::int32 n = real->countClasses();
    for (S::int32 i = 0; i < n && g_ncid < 8; ++i) {
        S::PClassInfo ci; std::memset(&ci, 0, sizeof ci);
        if (real->getClassInfo(i, &ci) != S::kResultOk) continue;
        if (std::strcmp(ci.category, kAudioClassCategory) == 0) {
            std::memcpy(g_cidmap[g_ncid].real, ci.cid, 16);
            std::memcpy(g_cidmap[g_ncid].ours, ci.cid, 16);
            auto* b0 = (unsigned char*)&g_cidmap[g_ncid].ours[0];
            *b0 ^= 0x80u;   // flip the top bit: deterministic, distinct from stock
            ++g_ncid;
        }
    }
}
const char* ourForReal(const char* real) {
    for (int i = 0; i < g_ncid; ++i) if (tuidEq(g_cidmap[i].real, real)) return g_cidmap[i].ours;
    return nullptr;
}
const char* realForOurs(const char* ours) {
    for (int i = 0; i < g_ncid; ++i) if (tuidEq(g_cidmap[i].ours, ours)) return g_cidmap[i].real;
    return nullptr;
}
// Our suffix on a class name ("FM8" -> "FM8.plus", "FM8 FX" -> "FM8 FX.plus") and the credit we
// answer instead of FM8's own: FM8 is Native Instruments' synth, FM8.plus is the layer around it.
const char kPlusSuffix[] = ".plus";
const char kFm8PlusVendor[] = "Native Instruments GmbH / musica.studio";

template <typename C>
void appendSuffix(C* name, size_t cap) {
    size_t L = 0; while (L < cap && name[L]) ++L;
    for (const char* p = kPlusSuffix; *p && L + 1 < cap; ++p) name[L++] = (C)*p;
    if (L < cap) name[L] = 0;
}
template <typename C>
void setVendor(C* dst, size_t cap) {
    size_t i = 0;
    for (const char* p = kFm8PlusVendor; *p && i + 1 < cap; ++p) dst[i++] = (C)*p;
    if (i < cap) dst[i] = 0;
}

std::wstring selfDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p); auto k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

// Per-instance state, keyed by an interface pointer of one of OUR instances. Fixed array; audio
// thread does a lock-free scan. One logical instance registers several keys (the IComponent,
// IAudioProcessor and IEditController subobjects, later its IPlugView), all pointing at one primary
// record whose InstanceState the audio thread and the logo menu share.
struct Rec {
    std::atomic<void*> key{nullptr};
    Rec* primary = nullptr;          // record owning the shared state (itself for the first key)
    InstanceState st;
    V::IMidiMapping* midiMap = nullptr;
    int16_t cachedCc = -2;
    uint32_t cachedPid = 0;
    ui::LogoMenu logoMenu;             // used on the primary only
};
constexpr int kMax = 256;   // four keys per logical instance, so ~64 instances
Rec g_rec[kMax];

Rec* recFor(void* k) {   // lookup only, never allocates: null => not one of ours => stay stock
    for (auto& r : g_rec) if (r.key.load(std::memory_order_relaxed) == k) return &r;
    return nullptr;
}
Rec* registerOurs(void* k, Rec* primary = nullptr) {
    if (!k) return nullptr;
    if (Rec* e = recFor(k)) return e;
    for (auto& r : g_rec) {
        void* e = nullptr;
        if (r.key.compare_exchange_strong(e, k)) {
            r.primary = primary ? primary : &r;
            if (!primary) {
                r.st.morphCc.store((int16_t)settings::morphCcDefault());
                r.st.arpMode.store((uint8_t)settings::arpModeDefault());
                r.st.morphRadius.store(settings::morphRadius());
                r.st.morphStartDeg.store(settings::morphStartDeg());
            }
            return &r;
        }
    }
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

// Our single added event-output bus sits at index == the real event-out count (0 for FM8). Only our
// instances advertise it; plain FM8 instances fall through to the original.
S::int32 h_getBusCount(void* self, V::MediaType type, V::BusDirection dir) {
    S::int32 n = o_busCount(self, type, dir);
    if (recFor(self) && type == V::kEvent && dir == V::kOutput) n += 1;
    return n;
}

S::tresult h_getBusInfo(void* self, V::MediaType type, V::BusDirection dir, S::int32 index, V::BusInfo& bus) {
    if (recFor(self) && type == V::kEvent && dir == V::kOutput) {
        S::int32 real = o_busCount(self, type, dir);   // 0 for FM8
        if (index == real) {
            std::memset(&bus, 0, sizeof bus);
            bus.mediaType = V::kEvent;
            bus.direction = V::kOutput;
            bus.channelCount = 16;
            bus.busType = V::kMain;
            bus.flags = V::BusInfo::kDefaultActive;
            const wchar_t* nm = L"FM8.plus Arp Out";
            for (int i = 0; i < 12; ++i) bus.name[i] = (S::char16)nm[i];
            return S::kResultTrue;
        }
    }
    return o_busInfo(self, type, dir, index, bus);
}

S::tresult h_activateBus(void* self, V::MediaType type, V::BusDirection dir, S::int32 index, S::TBool state) {
    if (recFor(self) && type == V::kEvent && dir == V::kOutput && index == o_busCount(self, type, dir))
        return S::kResultTrue;   // accept activation of our added bus
    return o_activate(self, type, dir, index, state);
}

// Read one parameter (by id) from the block's input changes; returns 0..127 or -1.
int readCcParam(V::IParameterChanges* changes, uint32_t pid) {
    if (!changes || !pid) return -1;
    S::int32 count = changes->getParameterCount();
    for (S::int32 i = 0; i < count; ++i) {
        V::IParamValueQueue* q = changes->getParameterData(i);
        if (!q || q->getParameterId() != pid) continue;
        S::int32 pts = q->getPointCount();
        if (pts <= 0) return -1;
        S::int32 off = 0; V::ParamValue val = 0;
        if (q->getPoint(pts - 1, off, val) == S::kResultTrue)
            return (int)(val * 127.0 + 0.5);
    }
    return -1;
}

// The host maps a CC to a parameter (IMidiMapping) and delivers it in inputParameterChanges, not as
// an event, so the core's event-level swallow never sees it. This wrapper hides that one parameter's
// queue from FM8 for the block, leaving the CC to drive only the morph.
struct FilteredChanges : V::IParameterChanges {
    V::IParameterChanges* inner = nullptr;
    uint32_t hide = 0;

    S::tresult PLUGIN_API queryInterface(const S::TUID id, void** obj) override {
        if (std::memcmp(id, V::IParameterChanges::iid, 16) == 0 || std::memcmp(id, S::FUnknown::iid, 16) == 0) {
            *obj = this; return S::kResultOk;
        }
        *obj = nullptr; return S::kNoInterface;
    }
    S::uint32 PLUGIN_API addRef() override { return 1; }   // block-scoped, lives on the caller's stack
    S::uint32 PLUGIN_API release() override { return 1; }

    S::int32 PLUGIN_API getParameterCount() override {
        S::int32 n = 0;
        for (S::int32 i = 0, c = inner->getParameterCount(); i < c; ++i) {
            V::IParamValueQueue* q = inner->getParameterData(i);
            if (q && q->getParameterId() != hide) ++n;
        }
        return n;
    }
    V::IParamValueQueue* PLUGIN_API getParameterData(S::int32 index) override {
        for (S::int32 i = 0, c = inner->getParameterCount(); i < c; ++i) {
            V::IParamValueQueue* q = inner->getParameterData(i);
            if (!q || q->getParameterId() == hide) continue;
            if (index-- == 0) return q;
        }
        return nullptr;
    }
    V::IParamValueQueue* PLUGIN_API addParameterData(const V::ParamID& id, S::int32& index) override {
        return inner->addParameterData(id, index);
    }
};

uint32_t paramIdForCc(Rec* r, void* self, int16_t cc) {
    if (cc == r->cachedCc) return r->cachedPid;
    if (!r->midiMap) {
        auto* unk = (S::FUnknown*)self;
        unk->queryInterface(V::IMidiMapping::iid, (void**)&r->midiMap);
    }
    uint32_t pid = 0;
    if (r->midiMap && r->midiMap->getMidiControllerAssignment(0, 0, (V::CtrlNumber)cc, pid) != S::kResultTrue)
        pid = (cc == 1) ? kVst3ModWheelParamId : 0;
    r->cachedCc = cc; r->cachedPid = pid;
    return pid;
}

S::tresult h_process(void* self, V::ProcessData& data) {
    Rec* r = recFor(self);
    if (!r) return o_process(self, data);   // not ours: stock FM8 processing
    InstanceState& st = r->primary->st;

    FilteredChanges filtered;
    V::IParameterChanges* origChanges = data.inputParameterChanges;
    const int16_t mc = st.morphCc.load(std::memory_order_relaxed);
    if (mc >= 0) {
        uint32_t pid = paramIdForCc(r, self, mc);
        int v = readCcParam(data.inputParameterChanges, pid);
        if (v >= 0) st.morphPending.store((uint8_t)v, std::memory_order_relaxed);
        if (pid && origChanges) {          // hide it for this block: morph only, no mod wheel
            filtered.inner = origChanges;
            filtered.hide = pid;
            data.inputParameterChanges = &filtered;
        }
    }

    const double tf = fm8plus::tempoFactor(st.tempoMode.load(std::memory_order_relaxed));
    if (tf != 1.0 && data.processContext) {
        data.processContext->tempo *= tf;
        data.processContext->projectTimeMusic *= tf;
    }

    st.clearBlock();
    Core::current = &st;
    S::tresult rv = o_process(self, data);
    Core::current = nullptr;
    data.inputParameterChanges = origChanges;   // the host owns ProcessData, hand it back unchanged

    Core::applyPendingMorphInternal(st);

    const int8_t db = st.gainDb.load(std::memory_order_relaxed);
    if (db > 0 && data.symbolicSampleSize == V::kSample32 && data.outputs) {
        const float g = gainLinear(db);
        for (S::int32 b = 0; b < data.numOutputs; ++b)
            for (S::int32 c = 0; c < data.outputs[b].numChannels; ++c)
                if (float* buf = data.outputs[b].channelBuffers32[c])
                    for (S::int32 i = 0; i < data.numSamples; ++i) buf[i] *= g;
    }

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

// --- editor hook: hook IEditController::createView and the returned IPlugView's attached/removed
// so the "FM8+" logo click is picked up inside the host's editor HWND (same path the VST2 shim uses). The
// functions are found from the live vtables (slot 17 of IEditController; slots 4 and 5 of IPlugView),
// so no reverse-engineered addresses are needed; both are gated to our instances by the registry.
using CreateViewFn = void*     (PLUGIN_API*)(void* self, S::FIDString name);
using AttachedFn   = S::tresult (PLUGIN_API*)(void* self, void* parent, S::FIDString type);
using RemovedFn    = S::tresult (PLUGIN_API*)(void* self);
using GetSizeFn    = S::tresult (PLUGIN_API*)(void* self, S::ViewRect* size);
using SetFrameFn   = S::tresult (PLUGIN_API*)(void* self, void* frame);
CreateViewFn o_createView = nullptr;
AttachedFn   o_attached   = nullptr;
RemovedFn    o_removed    = nullptr;
GetSizeFn    o_getSize    = nullptr;
SetFrameFn   o_setFrame   = nullptr;

inline void* vslot(void* obj, int i) { return (*(void***)obj)[i]; }
bool hookSlot(void* obj, int slot, void* det, void** orig) {   // idempotent per target address
    void* t = vslot(obj, slot);
    if (*orig) return true;
    return MH_CreateHook(t, det, orig) == MH_OK && MH_EnableHook(t) == MH_OK;
}

// GUI Scale. FM8 answers the stock 1x rect (it applies the scale only when it creates a window),
// so the host is told the scaled size it has to make room for.
S::tresult PLUGIN_API h_getSize(void* self, S::ViewRect* size) {
    S::tresult rv = o_getSize(self, size);
    // MinHook patches the shared vtable entry, so gate on the registry: a plain FM8 view is not
    // scaled and must keep reporting the stock rect.
    const float s = recFor(self) ? Core::guiScale() : 1.0f;
    if (rv == S::kResultOk && size && s != 1.0f) {
        size->right = size->left + (S::int32)lroundf((size->right - size->left) * s);
        size->bottom = size->top + (S::int32)lroundf((size->bottom - size->top) * s);
    }
    return rv;
}

// The host hands the view its IPlugFrame here; slot 3 of that is resizeView, the only way a VST3
// plug-in can change its own editor size. Kept per view so a GUI Scale change reaches the host.
struct ViewFrame { void* view; void* frame; };
ViewFrame g_frames[64];
S::tresult PLUGIN_API h_setFrame(void* self, void* frame) {
    ViewFrame* slot = nullptr;
    for (auto& f : g_frames) {
        if (f.view == self) { slot = &f; break; }
        if (!slot && !f.view) slot = &f;
    }
    if (slot) { slot->view = frame ? self : nullptr; slot->frame = frame; }   // null frame releases the slot
    return o_setFrame(self, frame);
}
void hostResize(void* ctx, int w, int h) {
    using ResizeViewFn = S::tresult (PLUGIN_API*)(void* self, void* view, S::ViewRect* r);
    for (auto& f : g_frames) {
        if (f.view != ctx || !f.frame) continue;
        S::ViewRect r{0, 0, w, h};
        ((ResizeViewFn)(*(void***)f.frame)[3])(f.frame, ctx, &r);
        return;
    }
}

S::tresult PLUGIN_API h_attached(void* self, void* parent, S::FIDString type) {
    if (recFor(self)) Core::addScaledWindow(parent);   // before FM8 sizes its own child inside it
    S::tresult rv = o_attached(self, parent, type);   // FM8 creates its child first; we subclass it
    Rec* r = recFor(self);
    if (r && rv == S::kResultOk) {
        r->primary->logoMenu.setHostResize(&hostResize, self);   // GUI Scale: ask the host to resize
        r->primary->logoMenu.attach((HWND)parent, &r->primary->st);
    }
    return rv;
}
S::tresult PLUGIN_API h_removed(void* self) {
    if (Rec* r = recFor(self)) {
        r->primary->logoMenu.detach();
        r->key.store(nullptr);   // ponytail: a view is attached once per createView in every host we know
    }
    return o_removed(self);
}
void* PLUGIN_API h_createView(void* self, S::FIDString name) {
    void* view = o_createView(self, name);
    Rec* r = recFor(self);
    if (view && r) {
        registerOurs(view, r->primary);
        hookSlot(view, 4, (void*)&h_attached, (void**)&o_attached);
        hookSlot(view, 5, (void*)&h_removed,  (void**)&o_removed);
        hookSlot(view, 9, (void*)&h_getSize,  (void**)&o_getSize);    // IPlugView::getSize
        hookSlot(view, 12, (void*)&h_setFrame, (void**)&o_setFrame);  // IPlugView::setFrame
    }
    return view;
}

bool ensureCore() {
    if (g_core) return g_hooked;
    g_core = LoadLibraryW((selfDir() + L"\\FM8.vst3").c_str());   // the UNTOUCHED stock module
    if (!g_core) return false;
    settings::load(g_self);
    if (!Core::install((void*)g_core, Host::Vst3)) return false;
    Core::setGuiScale(settings::guiScale());   // GUI Scale is live before the first editor is built
    Core::serveLogo(g_core);   // the "FM8+" wordmark FM8 draws itself
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

// --- factory wrapper: presents a distinct plug-in identity ("FM8.plus") over FM8's real factory ------

class Factory : public S::IPluginFactory3 {
    S::IPluginFactory*  f1_ = nullptr;
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
    ~Factory() {
        if (f3_) f3_->release();
        if (f2_) f2_->release();
        if (f1_) f1_->release();
    }

    // FUnknown
    S::tresult PLUGIN_API queryInterface(const S::TUID riid, void** obj) override {
        if (tuidEq(riid, S::FUnknown::iid) || tuidEq(riid, S::IPluginFactory::iid)
            || (f2_ && tuidEq(riid, S::IPluginFactory2::iid))
            || (f3_ && tuidEq(riid, S::IPluginFactory3::iid))) {
            addRef(); *obj = this; return S::kResultOk;
        }
        *obj = nullptr; return S::kNoInterface;
    }
    S::uint32 PLUGIN_API addRef() override { return ++ref_; }
    S::uint32 PLUGIN_API release() override {
        S::uint32 r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // IPluginFactory
    S::tresult PLUGIN_API getFactoryInfo(S::PFactoryInfo* info) override {
        S::tresult r = f1_->getFactoryInfo(info);
        if (r == S::kResultOk && info) setVendor(info->vendor, sizeof info->vendor);
        return r;
    }
    S::int32   PLUGIN_API countClasses() override { return f1_->countClasses(); }
    S::tresult PLUGIN_API getClassInfo(S::int32 i, S::PClassInfo* info) override {
        S::tresult r = f1_->getClassInfo(i, info);
        if (r == S::kResultOk && std::strcmp(info->category, kAudioClassCategory) == 0) {
            if (const char* oc = ourForReal(info->cid)) std::memcpy(info->cid, oc, 16);
            appendSuffix(info->name, sizeof info->name);
        }
        return r;
    }
    S::tresult PLUGIN_API createInstance(S::FIDString cid, S::FIDString _iid, void** obj) override {
        const char* rc = realForOurs((const char*)cid);
        S::FIDString eff = rc ? (S::FIDString)rc : cid;
        S::tresult r = f1_->createInstance(eff, _iid, obj);
        if (rc && r == S::kResultOk && obj && *obj) {
            auto* unk = (S::FUnknown*)*obj;
            Rec* prim = nullptr;
            void* comp = nullptr;
            if (unk->queryInterface(V::IComponent::iid, &comp) == S::kResultOk && comp) {
                prim = registerOurs(comp); ((S::FUnknown*)comp)->release();
            }
            void* ap = nullptr;
            if (unk->queryInterface(V::IAudioProcessor::iid, &ap) == S::kResultOk && ap) {
                registerOurs(ap, prim); ((S::FUnknown*)ap)->release();
            }
            // FM8 is single-component: the controller is a subobject of the same instance. Register it
            // and hook its createView so the editor's IPlugView reaches h_attached.
            void* ctrl = nullptr;
            if (unk->queryInterface(V::IEditController::iid, &ctrl) == S::kResultOk && ctrl) {
                registerOurs(ctrl, prim);
                hookSlot(ctrl, 17, (void*)&h_createView, (void**)&o_createView);   // IEditController::createView
                ((S::FUnknown*)ctrl)->release();
            }
        }
        return r;
    }

    // IPluginFactory2
    S::tresult PLUGIN_API getClassInfo2(S::int32 i, S::PClassInfo2* info) override {
        if (!f2_) return S::kNotImplemented;
        S::tresult r = f2_->getClassInfo2(i, info);
        if (r == S::kResultOk && std::strcmp(info->category, kAudioClassCategory) == 0) {
            if (const char* oc = ourForReal(info->cid)) std::memcpy(info->cid, oc, 16);
            appendSuffix(info->name, sizeof info->name);
            setVendor(info->vendor, sizeof info->vendor);
        }
        return r;
    }

    // IPluginFactory3
    S::tresult PLUGIN_API getClassInfoUnicode(S::int32 i, S::PClassInfoW* info) override {
        if (!f3_) return S::kNotImplemented;
        S::tresult r = f3_->getClassInfoUnicode(i, info);
        if (r == S::kResultOk && std::strcmp(info->category, kAudioClassCategory) == 0) {
            if (const char* oc = ourForReal(info->cid)) std::memcpy(info->cid, oc, 16);
            appendSuffix(info->name, sizeof(info->name) / sizeof(info->name[0]));
            setVendor(info->vendor, sizeof(info->vendor) / sizeof(info->vendor[0]));
        }
        return r;
    }
    S::tresult PLUGIN_API setHostContext(S::FUnknown* ctx) override {
        return f3_ ? f3_->setHostContext(ctx) : S::kNotImplemented;
    }
};

} // namespace

extern "C" __declspec(dllexport) S::IPluginFactory* PLUGIN_API GetPluginFactory() {
    ensureCore();   // hooks installed regardless
    auto real = (S::IPluginFactory*(PLUGIN_API*)())GetProcAddress(g_core, "GetPluginFactory");
    S::IPluginFactory* rf = real ? real() : nullptr;
    if (!rf) return nullptr;
    return g_hooked ? new Factory(rf) : rf;   // if hooks failed, forward the real factory unchanged
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
