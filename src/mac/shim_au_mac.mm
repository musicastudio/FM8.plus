// FM8.plus Audio Unit wrapper for macOS. FM8.plus.component sits beside the untouched FM8.component,
// calls FM8's own AU factory, and wraps the plug-in interface it returns: every AU call reaches FM8
// through us, and the few that carry a feature are handled on the way.
//
//   Render            arp routing (Core::current), Increase Gain, arp MIDI out, morph apply
//   MIDIEvent         Morph Rotate Control takes its CC before FM8 sees it
//   HostCallbacks     Tempo Override scales the tempo and beat FM8 reads from the host
//   ClassInfo         per-instance settings travel in the host's preset as an extra key
//   MIDIOutputCallback(Info)  the arp's MIDI output, which FM8 does not offer itself
//   CocoaUI           FM8's editor inside our container view, for the wordmark and GUI Scale
#import <Cocoa/Cocoa.h>
#import <AudioUnit/AudioUnit.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <CoreMIDI/CoreMIDI.h>
#include <dlfcn.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include "core_mac.h"
#include "ui_mac.h"
#include "../core/settings.h"

using namespace fm8plus;

namespace {
const OSType kFm8Type = 'aumu', kFm8Sub = 'Nif8', kFm8Maker = '-NI-';
CFStringRef const kStateKey = CFSTR("FM8.plus");
constexpr AudioUnitParameterID kParamArpBpmSync = 153;

using FactoryFn = AudioComponentPlugInInterface* (*)(const AudioComponentDescription*);
FactoryFn g_fm8Factory = nullptr;
bool g_coreHooked = false;
std::string g_rsrc;
NSURL* g_fm8ViewBundle = nil;   // FM8's own Cocoa view factory, which our factory wraps
NSString* g_fm8ViewClass = nil;

struct Inst {
    AudioComponentPlugInInterface iface;   // first: the AU framework hands this back as `self`
    AudioComponentPlugInInterface* inner = nullptr;
    AudioComponentInstance ci = nullptr;
    InstanceState st;
    AudioUnitRenderProc render = nullptr;
    MusicDeviceMIDIEventProc midiEvent = nullptr;
    AudioUnitSetParameterProc setParam = nullptr;
    HostCallbackInfo host{}, ours{};
    AUMIDIOutputCallbackStruct midiOut{};
    bool customApplied = false;
    NSView* container = nil;
    NSView* editor = nil;
    NSSize base{};
};

std::string fm8BundlePath() {
    Dl_info di{};
    dladdr((void*)&fm8BundlePath, &di);
    std::string self = di.dli_fname ? di.dli_fname : "";
    const size_t at = self.rfind(".component/Contents/MacOS/");
    if (at != std::string::npos) {
        const std::string p = self.substr(0, self.rfind('/', at) + 1) + "FM8.component";
        if (access((p + "/Contents/MacOS/FM8").c_str(), R_OK) == 0) return p;
    }
    return "/Library/Audio/Plug-Ins/Components/FM8.component";
}

bool ensureCore() {
    static bool tried = false;
    if (tried) return g_fm8Factory != nullptr;
    tried = true;
    const std::string path = fm8BundlePath();
    CFBundleRef b = CFBundleCreate(nullptr, (__bridge CFURLRef)[NSURL fileURLWithPath:@(path.c_str())]);
    if (!b || !CFBundleLoadExecutable(b)) return false;
    g_fm8Factory = (FactoryFn)CFBundleGetFunctionPointerForName(b, CFSTR("NIAudioUnitSynthFactory"));
    if (!g_fm8Factory) return false;
    settings::load(nullptr);
    g_rsrc = path + "/Contents/Resources/FM8.rsrc";
    g_coreHooked = Core::installMac((const void*)g_fm8Factory);
    Core::setGuiScale(settings::guiScale());
    if (g_coreHooked) Core::serveLogoMac(g_rsrc.c_str());
    return true;
}

// ---- instances ---------------------------------------------------------------------------------
constexpr int kMaxInst = 64;
std::atomic<Inst*> g_insts[kMaxInst];
void track(Inst* i) { for (auto& s : g_insts) { Inst* e = nullptr; if (s.compare_exchange_strong(e, i)) return; } }
void untrack(Inst* i) { for (auto& s : g_insts) { Inst* e = i; s.compare_exchange_strong(e, nullptr); } }
Inst* byCi(AudioUnit ci) { for (auto& s : g_insts) if (Inst* i = s.load(); i && i->ci == ci) return i; return nullptr; }
Inst* bySt(InstanceState* st) { for (auto& s : g_insts) if (Inst* i = s.load(); i && &i->st == st) return i; return nullptr; }

// ---- tempo -------------------------------------------------------------------------------------
double factor(Inst* i) { return tempoFactor(i->st.tempoMode.load(std::memory_order_relaxed)); }

OSStatus beatAndTempo(void* ud, Float64* beat, Float64* tempo) {
    auto* i = (Inst*)ud;
    const OSStatus e = i->host.beatAndTempoProc(i->host.hostUserData, beat, tempo);
    const double f = factor(i);
    if (!e && f != 1.0) { if (beat) *beat *= f; if (tempo) *tempo *= f; }
    return e;
}
OSStatus musicalTime(void* ud, UInt32* delta, Float32* num, UInt32* den, Float64* downBeat) {
    auto* i = (Inst*)ud;
    const OSStatus e = i->host.musicalTimeLocationProc(i->host.hostUserData, delta, num, den, downBeat);
    if (!e && downBeat) *downBeat *= factor(i);
    return e;
}
OSStatus transport(void* ud, Boolean* playing, Boolean* changed, Float64* sample, Boolean* cycling,
                   Float64* cycStart, Float64* cycEnd) {
    auto* i = (Inst*)ud;
    const OSStatus e = i->host.transportStateProc(i->host.hostUserData, playing, changed, sample, cycling, cycStart, cycEnd);
    const double f = factor(i);
    if (!e && f != 1.0) { if (cycStart) *cycStart *= f; if (cycEnd) *cycEnd *= f; }
    return e;
}
OSStatus transport2(void* ud, Boolean* playing, Boolean* recording, Boolean* changed, Float64* sample,
                    Boolean* cycling, Float64* cycStart, Float64* cycEnd) {
    auto* i = (Inst*)ud;
    const OSStatus e = i->host.transportStateProc2(i->host.hostUserData, playing, recording, changed, sample, cycling, cycStart, cycEnd);
    const double f = factor(i);
    if (!e && f != 1.0) { if (cycStart) *cycStart *= f; if (cycEnd) *cycEnd *= f; }
    return e;
}

// ---- the calls we sit on -----------------------------------------------------------------------
OSStatus auRender(void* self, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus,
                  UInt32 frames, AudioBufferList* io) {
    auto* i = (Inst*)self;
    if (bus != 0) return i->render(i->inner, flags, ts, bus, frames, io);
    i->st.clearBlock();
    Core::armAudioThread();
    Core::current = &i->st;
    const OSStatus e = i->render(i->inner, flags, ts, bus, frames, io);
    Core::current = nullptr;
    if (e) return e;
    const int8_t db = i->st.gainDb.load(std::memory_order_relaxed);
    if (db > 0 && io) {
        const float g = gainLinear(db);
        for (UInt32 b = 0; b < io->mNumberBuffers; ++b) {
            auto* p = (float*)io->mBuffers[b].mData;
            const UInt32 n = io->mBuffers[b].mDataByteSize / sizeof(float);
            if (p) for (UInt32 k = 0; k < n; ++k) p[k] *= g;
        }
    }
    Core::applyPendingMorphInternal(i->st);
    const bool custom = i->st.tempoMode.load(std::memory_order_relaxed) == 5;
    if (custom != i->customApplied && i->setParam) {
        i->setParam(i->inner, kParamArpBpmSync, kAudioUnitScope_Global, 0, custom ? 0.0f : 1.0f, 0);
        i->customApplied = custom;
    }
    if (i->st.outCount > 0 && i->midiOut.midiOutputCallback) {
        // One packet per message; the timestamp is the sample offset in this buffer, as AU MIDI out expects.
        Byte buf[sizeof(MIDIPacketList) + InstanceState::kMaxOut * sizeof(MIDIPacket)];
        auto* list = (MIDIPacketList*)buf;
        MIDIPacket* pk = MIDIPacketListInit(list);
        for (int k = 0; k < i->st.outCount && pk; ++k) {
            const MidiMsg& m = i->st.outBuf[k];
            const Byte data[3] = {m.status, m.data1, m.data2};
            pk = MIDIPacketListAdd(list, sizeof buf, pk, (MIDITimeStamp)m.offset, 3, data);
        }
        i->midiOut.midiOutputCallback(i->midiOut.userData, ts, 0, list);
    }
    i->st.clearBlock();
    return noErr;
}

OSStatus auMidiEvent(void* self, UInt32 status, UInt32 d1, UInt32 d2, UInt32 offset) {
    auto* i = (Inst*)self;
    const int16_t mc = i->st.morphCc.load(std::memory_order_relaxed);
    if (mc >= 0 && (status & 0xf0) == 0xb0 && d1 == (UInt32)mc) {
        i->st.morphPending.store((uint8_t)d2, std::memory_order_relaxed);
        return noErr;
    }
    return i->midiEvent(i->inner, status, d1, d2, offset);
}

using GetPropFn = OSStatus (*)(void*, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, void*, UInt32*);
using SetPropFn = OSStatus (*)(void*, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void*, UInt32);
using InfoFn = OSStatus (*)(void*, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, UInt32*, Boolean*);

template <typename F> F innerFn(Inst* i, SInt16 sel) { return (F)i->inner->Lookup(sel); }

OSStatus auGetPropertyInfo(void* self, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, UInt32* size, Boolean* w) {
    auto* i = (Inst*)self;
    if (id == kAudioUnitProperty_MIDIOutputCallbackInfo) { if (size) *size = sizeof(CFArrayRef); if (w) *w = false; return noErr; }
    if (id == kAudioUnitProperty_MIDIOutputCallback) { if (size) *size = sizeof(AUMIDIOutputCallbackStruct); if (w) *w = true; return noErr; }
    return innerFn<InfoFn>(i, kAudioUnitGetPropertyInfoSelect)(i->inner, id, sc, el, size, w);
}

OSStatus auGetProperty(void* self, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, void* out, UInt32* size) {
    auto* i = (Inst*)self;
    if (id == kAudioUnitProperty_MIDIOutputCallbackInfo) {
        if (!out || !size || *size < sizeof(CFArrayRef)) return kAudioUnitErr_InvalidPropertyValue;
        CFStringRef name = CFSTR("FM8.plus Arp Out");
        *(CFArrayRef*)out = CFArrayCreate(nullptr, (const void**)&name, 1, &kCFTypeArrayCallBacks);
        *size = sizeof(CFArrayRef);
        return noErr;
    }
    const OSStatus e = innerFn<GetPropFn>(i, kAudioUnitGetPropertySelect)(i->inner, id, sc, el, out, size);
    if (e || !out) return e;
    if (id == kAudioUnitProperty_ClassInfo && sc == kAudioUnitScope_Global) {
        auto dict = *(CFPropertyListRef*)out;
        if (dict && CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
            CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(nullptr, 0, (CFDictionaryRef)dict);
            const int16_t mc = i->st.morphCc.load();
            const UInt8 blob[6] = {i->st.arpMode.load(), i->st.tempoMode.load(), (UInt8)i->st.gainDb.load(),
                                   (UInt8)(mc & 0xff), (UInt8)((mc >> 8) & 0xff), 0};
            CFDataRef d = CFDataCreate(nullptr, blob, sizeof blob);
            CFDictionarySetValue(m, kStateKey, d);
            CFRelease(d);
            CFRelease(dict);
            *(CFPropertyListRef*)out = m;
        }
    } else if (id == kAudioUnitProperty_CocoaUI && *size >= sizeof(AudioUnitCocoaViewInfo)) {
        auto* info = (AudioUnitCocoaViewInfo*)out;
        if (!g_fm8ViewBundle) {
            g_fm8ViewBundle = (__bridge NSURL*)info->mCocoaAUViewBundleLocation;
            g_fm8ViewClass = (__bridge NSString*)info->mCocoaAUViewClass[0];
        }
        CFRelease(info->mCocoaAUViewBundleLocation);
        CFRelease(info->mCocoaAUViewClass[0]);
        info->mCocoaAUViewBundleLocation = (CFURLRef)CFBridgingRetain([[NSBundle bundleForClass:NSClassFromString(@"FM8PlusAUViewFactory")] bundleURL]);
        info->mCocoaAUViewClass[0] = (CFStringRef)CFBridgingRetain(@"FM8PlusAUViewFactory");
        *size = sizeof(AudioUnitCocoaViewInfo);
    }
    return e;
}

OSStatus auSetProperty(void* self, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, const void* in, UInt32 size) {
    auto* i = (Inst*)self;
    auto set = innerFn<SetPropFn>(i, kAudioUnitSetPropertySelect);
    if (id == kAudioUnitProperty_MIDIOutputCallback) {
        if (in && size >= sizeof(AUMIDIOutputCallbackStruct)) i->midiOut = *(const AUMIDIOutputCallbackStruct*)in;
        else i->midiOut = {};
        return noErr;
    }
    if (id == kAudioUnitProperty_HostCallbacks && in) {
        i->host = {};
        memcpy(&i->host, in, std::min<size_t>(size, sizeof i->host));
        i->ours = {};
        i->ours.hostUserData = i;
        if (i->host.beatAndTempoProc) i->ours.beatAndTempoProc = &beatAndTempo;
        if (i->host.musicalTimeLocationProc) i->ours.musicalTimeLocationProc = &musicalTime;
        if (i->host.transportStateProc) i->ours.transportStateProc = &transport;
        if (i->host.transportStateProc2) i->ours.transportStateProc2 = &transport2;
        return set(i->inner, id, sc, el, &i->ours, std::min<UInt32>(size, sizeof i->ours));
    }
    if (id == kAudioUnitProperty_ClassInfo && sc == kAudioUnitScope_Global && in) {
        auto dict = *(const CFPropertyListRef*)in;
        if (dict && CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
            auto d = (CFDataRef)CFDictionaryGetValue((CFDictionaryRef)dict, kStateKey);
            if (d && CFGetTypeID(d) == CFDataGetTypeID() && CFDataGetLength(d) >= 5) {
                const UInt8* b = CFDataGetBytePtr(d);
                i->st.arpMode.store(b[0]); i->st.tempoMode.store(b[1]); i->st.gainDb.store((int8_t)b[2]);
                i->st.morphCc.store((int16_t)(b[3] | (b[4] << 8)));
            }
        }
    }
    return set(i->inner, id, sc, el, in, size);
}

// ---- the plug-in interface ---------------------------------------------------------------------
OSStatus auOpen(void* self, AudioComponentInstance ci) {
    auto* i = (Inst*)self;
    i->ci = ci;
    const OSStatus e = i->inner->Open(i->inner, ci);
    if (e) return e;
    i->render = (AudioUnitRenderProc)i->inner->Lookup(kAudioUnitRenderSelect);
    i->midiEvent = (MusicDeviceMIDIEventProc)i->inner->Lookup(kMusicDeviceMIDIEventSelect);
    i->setParam = (AudioUnitSetParameterProc)i->inner->Lookup(kAudioUnitSetParameterSelect);
    i->st.arpMode.store((uint8_t)settings::arpModeDefault());
    i->st.morphCc.store((int16_t)settings::morphCcDefault());
    i->st.morphRadius.store(settings::morphRadius());
    i->st.morphStartDeg.store(settings::morphStartDeg());
    track(i);
    Core::bindInstance(&i->st, i->inner);
    return noErr;
}

OSStatus auClose(void* self) {
    auto* i = (Inst*)self;
    Core::unbindInstance(&i->st);
    untrack(i);
    const OSStatus e = i->inner->Close(i->inner);
    delete i;
    return e;
}

AudioComponentMethod auLookup(SInt16 sel) {
    switch (sel) {
        case kAudioUnitRenderSelect:          return (AudioComponentMethod)&auRender;
        case kMusicDeviceMIDIEventSelect:     return (AudioComponentMethod)&auMidiEvent;
        case kAudioUnitGetPropertyInfoSelect: return (AudioComponentMethod)&auGetPropertyInfo;
        case kAudioUnitGetPropertySelect:     return (AudioComponentMethod)&auGetProperty;
        case kAudioUnitSetPropertySelect:     return (AudioComponentMethod)&auSetProperty;
        default:                              return nullptr;   // filled per instance below
    }
}

// Any instance's inner interface: Lookup is per class, so one answers for all of them.
AudioComponentPlugInInterface* g_proto = nullptr;
} // namespace

// ---- editor ------------------------------------------------------------------------------------
@interface FM8PlusAUViewFactory : NSObject <AUCocoaUIBase>
@end

namespace {
void scaleEditor(Inst* i) {
    if (!i->container || !i->editor || i->base.width <= 0) return;
    const CGFloat s = Core::guiScale();
    const NSSize big = NSMakeSize(i->base.width * s, i->base.height * s);
    [i->container setFrameSize:big];
    [i->editor setFrameOrigin:NSZeroPoint];
    [i->editor setFrameSize:big];
    [i->editor setBoundsSize:i->base];
    [i->editor setNeedsDisplay:YES];
}
void applyScale(void* ctx, float) { scaleEditor((Inst*)ctx); }
void onLogo(InstanceState* st) {
    Inst* i = bySt(st);
    macui::ScaleHost sh;
    if (i && i->container) { sh.apply = &applyScale; sh.ctx = i; }
    macui::showMenu(st, sh);
}
} // namespace

@implementation FM8PlusAUViewFactory
- (unsigned)interfaceVersion { return 0; }
- (NSString*)description { return @"FM8.plus"; }
- (NSView*)uiViewForAudioUnit:(AudioUnit)au withSize:(NSSize)size {
    Inst* i = byCi(au);
    if (!g_fm8ViewBundle || !g_fm8ViewClass) return nil;
    Core::serveLogoMac(g_rsrc.c_str());   // FM8 empties its resource map with its last instance
    NSBundle* b = [NSBundle bundleWithURL:g_fm8ViewBundle];
    id<AUCocoaUIBase> f = [[[b classNamed:g_fm8ViewClass] alloc] init];
    NSView* v = [f uiViewForAudioUnit:au withSize:size];
    if (!v || !i) return v;
    if (!i->st.appObj.load()) Core::bindInstance(&i->st, i->inner);
    i->editor = v;
    i->base = v.frame.size;
    i->container = [[NSView alloc] initWithFrame:v.frame];
    [i->container addSubview:v];
    scaleEditor(i);
    return i->container;
}
@end

// Per-selector bridges for everything we pass through: swap our `self` for FM8's and jump.
namespace {
template <SInt16 Sel, typename R, typename... A>
R bridge(void* self, A... a) {
    auto* i = (Inst*)self;
    return ((R (*)(void*, A...))i->inner->Lookup(Sel))(i->inner, a...);
}
AudioComponentMethod passthrough(SInt16 sel) {
    switch (sel) {
#define B(S, ...) case S: return (AudioComponentMethod)&bridge<S, OSStatus, ##__VA_ARGS__>;
        B(kAudioUnitInitializeSelect)
        B(kAudioUnitUninitializeSelect)
        B(kAudioUnitRemovePropertyListenerSelect, AudioUnitPropertyID, AudioUnitPropertyListenerProc)
        B(kAudioUnitAddPropertyListenerSelect, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void*)
        B(kAudioUnitRemovePropertyListenerWithUserDataSelect, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void*)
        B(kAudioUnitAddRenderNotifySelect, AURenderCallback, void*)
        B(kAudioUnitRemoveRenderNotifySelect, AURenderCallback, void*)
        B(kAudioUnitGetParameterSelect, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue*)
        B(kAudioUnitSetParameterSelect, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue, UInt32)
        B(kAudioUnitScheduleParametersSelect, const AudioUnitParameterEvent*, UInt32)
        B(kAudioUnitResetSelect, AudioUnitScope, AudioUnitElement)
        B(kAudioUnitProcessSelect, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, AudioBufferList*)
        B(kMusicDeviceSysExSelect, const UInt8*, UInt32)
        B(kMusicDevicePrepareInstrumentSelect, MusicDeviceInstrumentID)
        B(kMusicDeviceReleaseInstrumentSelect, MusicDeviceInstrumentID)
        B(kMusicDeviceStartNoteSelect, MusicDeviceInstrumentID, MusicDeviceGroupID, NoteInstanceID*, UInt32, const MusicDeviceNoteParams*)
        B(kMusicDeviceStopNoteSelect, MusicDeviceGroupID, NoteInstanceID, UInt32)
#undef B
        default: return nullptr;
    }
}

// Selectors we do not sit on go straight to FM8's own method with FM8's `self`, so each one needs a
// bridge; AU calls carry `self` first, which is all a bridge swaps. Only what FM8 implements is offered,
// and never the MIDI 2.0 event list, so hosts send MIDI through auMidiEvent where the morph CC is taken.
AudioComponentMethod lookup(SInt16 sel) {
    if (!g_proto || !g_proto->Lookup(sel)) return nullptr;
    if (AudioComponentMethod m = auLookup(sel)) return m;
    return passthrough(sel);
}
} // namespace

extern "C" __attribute__((visibility("default"))) void* FM8PlusAUFactory(const AudioComponentDescription*) {
    if (!ensureCore()) return nullptr;
    const AudioComponentDescription d{kFm8Type, kFm8Sub, kFm8Maker, 0, 0};
    AudioComponentPlugInInterface* inner = g_fm8Factory(&d);
    if (!inner) return nullptr;
    Core::setLogoHandler(&onLogo);
    if (!g_proto) g_proto = inner;
    auto* i = new Inst;
    i->inner = inner;
    i->iface.Open = &auOpen;
    i->iface.Close = &auClose;
    i->iface.Lookup = [](SInt16 sel) -> AudioComponentMethod { return lookup(sel); };
    i->iface.reserved = nullptr;
    return i;
}
