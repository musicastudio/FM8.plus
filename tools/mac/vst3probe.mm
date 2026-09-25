// Minimal VST3 host probe for FM8.plus.vst3 on macOS, the twin of vst2probe.mm and auprobe.mm.
//
//     vst3probe <bundle.vst3> --classes        list the factory's classes (expect FM8.plus)
//     vst3probe <bundle.vst3> --arp            check the added event-out bus, count arp notes on it
//     vst3probe <bundle.vst3> --morph          send CC 11 and watch Morph X/Y (needs morph_cc=11)
//     vst3probe <bundle.vst3> --editor out.png [x y]   open the editor, optionally click a logical point
#import <Cocoa/Cocoa.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include <unistd.h>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/gui/iplugview.h"

namespace S = Steinberg;
namespace V = Steinberg::Vst;
DEF_CLASS_IID(Steinberg::FUnknown)
DEF_CLASS_IID(Steinberg::IPluginFactory)
DEF_CLASS_IID(Steinberg::Vst::IComponent)
DEF_CLASS_IID(Steinberg::Vst::IAudioProcessor)
DEF_CLASS_IID(Steinberg::Vst::IEditController)
DEF_CLASS_IID(Steinberg::Vst::IMidiMapping)
DEF_CLASS_IID(Steinberg::Vst::IHostApplication)
DEF_CLASS_IID(Steinberg::Vst::IEventList)
DEF_CLASS_IID(Steinberg::Vst::IParameterChanges)
DEF_CLASS_IID(Steinberg::Vst::IParamValueQueue)
DEF_CLASS_IID(Steinberg::IPlugFrame)

namespace {
bool same(const S::TUID a, const S::TUID b) { return !memcmp(a, b, 16); }
#define UNKNOWN_SELF(I) \
    S::tresult PLUGIN_API queryInterface(const S::TUID id, void** o) override { \
        if (same(id, I::iid) || same(id, S::FUnknown::iid)) { *o = this; return S::kResultOk; } \
        *o = nullptr; return S::kNoInterface; } \
    S::uint32 PLUGIN_API addRef() override { return 1; } \
    S::uint32 PLUGIN_API release() override { return 1; }

struct Host : V::IHostApplication {
    UNKNOWN_SELF(V::IHostApplication)
    S::tresult PLUGIN_API getName(V::String128 n) override { const char* s = "vst3probe"; int k = 0; for (; s[k]; ++k) n[k] = s[k]; n[k] = 0; return S::kResultOk; }
    S::tresult PLUGIN_API createInstance(S::TUID, S::TUID, void** o) override { *o = nullptr; return S::kNotImplemented; }
} g_host;

struct Events : V::IEventList {
    UNKNOWN_SELF(V::IEventList)
    std::vector<V::Event> ev;
    S::int32 PLUGIN_API getEventCount() override { return (S::int32)ev.size(); }
    S::tresult PLUGIN_API getEvent(S::int32 i, V::Event& e) override { if (i < 0 || i >= (S::int32)ev.size()) return S::kResultFalse; e = ev[i]; return S::kResultOk; }
    S::tresult PLUGIN_API addEvent(V::Event& e) override { ev.push_back(e); return S::kResultOk; }
};

struct Queue : V::IParamValueQueue {
    UNKNOWN_SELF(V::IParamValueQueue)
    V::ParamID id = 0; double v = 0;
    V::ParamID PLUGIN_API getParameterId() override { return id; }
    S::int32 PLUGIN_API getPointCount() override { return 1; }
    S::tresult PLUGIN_API getPoint(S::int32, S::int32& off, V::ParamValue& val) override { off = 0; val = v; return S::kResultTrue; }
    S::tresult PLUGIN_API addPoint(S::int32, V::ParamValue, S::int32&) override { return S::kResultFalse; }
};
struct Changes : V::IParameterChanges {
    UNKNOWN_SELF(V::IParameterChanges)
    std::vector<Queue> q;
    S::int32 PLUGIN_API getParameterCount() override { return (S::int32)q.size(); }
    V::IParamValueQueue* PLUGIN_API getParameterData(S::int32 i) override { return i < (S::int32)q.size() ? &q[i] : nullptr; }
    V::IParamValueQueue* PLUGIN_API addParameterData(const V::ParamID&, S::int32&) override { return nullptr; }
};

struct Frame : S::IPlugFrame {
    UNKNOWN_SELF(S::IPlugFrame)
    int w = 0, h = 0;
    S::tresult PLUGIN_API resizeView(S::IPlugView* v, S::ViewRect* r) override { w = r->getWidth(); h = r->getHeight(); return v->onSize(r); }
} g_frame;

V::IComponent* g_comp = nullptr;
V::IAudioProcessor* g_proc = nullptr;
V::IEditController* g_ctrl = nullptr;
Events g_in, g_out;
Changes g_changes;
V::ProcessContext g_ctx{};

V::ParamID paramId(const char* title) {
    for (S::int32 k = 0, n = g_ctrl->getParameterCount(); k < n; ++k) {
        V::ParameterInfo pi{};
        g_ctrl->getParameterInfo(k, pi);
        char t[128] = {};
        for (int c = 0; c < 127 && pi.title[c]; ++c) t[c] = (char)pi.title[c];
        if (!strcasecmp(t, title)) return pi.id;
    }
    return (V::ParamID)-1;
}

float render(int blocks) {
    static float l[512], r[512];
    float* ch[2] = {l, r};
    V::AudioBusBuffers out{}; out.numChannels = 2; out.channelBuffers32 = ch;
    float peak = 0;
    for (int b = 0; b < blocks; ++b) {
        V::ProcessData d{};
        d.processMode = V::kRealtime; d.symbolicSampleSize = V::kSample32; d.numSamples = 512;
        d.numOutputs = 1; d.outputs = &out;
        d.inputEvents = &g_in; d.outputEvents = &g_out;
        d.inputParameterChanges = &g_changes;
        g_ctx.state = V::ProcessContext::kPlaying | V::ProcessContext::kTempoValid | V::ProcessContext::kProjectTimeMusicValid;
        g_ctx.sampleRate = 44100; g_ctx.tempo = 120;
        d.processContext = &g_ctx;
        g_proc->process(d);
        g_ctx.projectTimeSamples += 512; g_ctx.projectTimeMusic += 512.0 / 22050.0;
        g_in.ev.clear(); g_changes.q.clear();
        for (float v : l) peak = fmaxf(peak, fabsf(v));
    }
    return peak;
}

void note(int pitch) {
    V::Event e{}; e.type = V::Event::kNoteOnEvent;
    e.noteOn.pitch = (S::int16)pitch; e.noteOn.velocity = 0.8f; e.noteOn.noteId = -1;
    g_in.ev.push_back(e);
}

int runArp() {
    const S::int32 outs = g_comp->getBusCount(V::kEvent, V::kOutput);
    V::BusInfo bi{};
    g_comp->getBusInfo(V::kEvent, V::kOutput, 0, bi);
    char nm[64] = {};
    for (int c = 0; c < 63 && bi.name[c]; ++c) nm[c] = (char)bi.name[c];
    printf("event output buses: %d (%s)\n", outs, nm);
    V::ParamID arp = paramId("Arp On");
    if (arp == (V::ParamID)-1) arp = paramId("Arpeggiator On");
    if (arp == (V::ParamID)-1)
        for (S::int32 k = 0, n = g_ctrl->getParameterCount(); k < n; ++k) {
            V::ParameterInfo pi{};
            g_ctrl->getParameterInfo(k, pi);
            char t[128] = {};
            for (int c = 0; c < 127 && pi.title[c]; ++c) t[c] = (char)pi.title[c];
            if (strstr(t, "Arp") || k < 3) printf("  param %d id %u: %s\n", k, pi.id, t);
        }
    printf("Arp On param id: %u\n", arp);
    g_changes.q.push_back({}); g_changes.q.back().id = arp; g_changes.q.back().v = 1.0;
    render(4);
    for (int p : {60, 64, 67}) note(p);
    const float peak = render(200);
    int ons = 0;
    for (auto& e : g_out.ev) if (e.type == V::Event::kNoteOnEvent) ++ons;
    printf("audio peak %.3f; events out %zu (note-ons %d)\n", peak, g_out.ev.size(), ons);
    const bool ok = outs == 1 && ons > 0;
    printf("RESULT: %s\n", ok ? "arpeggiator emitted MIDI on the FM8.plus bus" : "NO ARP MIDI OUT");
    return ok ? 0 : 1;
}

int runMorph() {
    V::IMidiMapping* mm = nullptr;
    g_comp->queryInterface(V::IMidiMapping::iid, (void**)&mm);
    V::ParamID cc11 = 0;
    if (!mm || mm->getMidiControllerAssignment(0, 0, 11, cc11) != S::kResultTrue) { printf("RESULT: no CC11 mapping\n"); return 1; }
    const V::ParamID mx = paramId("Morph X"), my = paramId("Morph Y");
    printf("CC11 -> param %u; Morph X id %u, Y id %u\n", cc11, mx, my);
    float xs[5], ys[5];
    const int vals[5] = {0, 32, 64, 96, 127};
    for (int k = 0; k < 5; ++k) {
        g_changes.q.push_back({}); g_changes.q.back().id = cc11; g_changes.q.back().v = vals[k] / 127.0;
        render(4);
        xs[k] = (float)g_ctrl->getParamNormalized(mx); ys[k] = (float)g_ctrl->getParamNormalized(my);
        printf("  CC11=%3d -> Morph X=%.3f Y=%.3f\n", vals[k], xs[k], ys[k]);
    }
    bool moved = false;
    for (int k = 1; k < 5; ++k) if (xs[k] != xs[0] || ys[k] != ys[0]) moved = true;
    printf("RESULT: %s\n", moved ? "morph follows the CC" : "MORPH DID NOT MOVE");
    return moved ? 0 : 1;
}

void pump(double secs) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:secs];
    while ([until timeIntervalSinceNow] > 0)
        if (NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                               inMode:NSDefaultRunLoopMode dequeue:YES]) [NSApp sendEvent:e];
}

int runEditor(const char* out, double cx, double cy) {
    S::IPlugView* v = g_ctrl->createView(V::ViewType::kEditor);
    if (!v) { printf("RESULT: FAILED no view\n"); return 1; }
    v->setFrame(&g_frame);
    S::ViewRect r{};
    v->getSize(&r);
    printf("view %dx%d\n", r.getWidth(), r.getHeight());
    NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, r.getWidth(), r.getHeight())
                                                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    win.title = @"FM8.plus VST3 probe";
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (v->attached((__bridge void*)win.contentView, S::kPlatformTypeNSView) != S::kResultOk) { printf("RESULT: FAILED attach\n"); return 1; }
    pump(3);
    __block bool menu = false;
    id obs = [[NSNotificationCenter defaultCenter] addObserverForName:NSMenuDidBeginTrackingNotification object:nil queue:nil
        usingBlock:^(NSNotification* n) {
            NSMenu* m = n.object; menu = true;
            printf("menu opened: %s, %ld items\n", m.title.UTF8String, (long)m.numberOfItems);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{ [m cancelTracking]; });
        }];
    if (cx >= 0) {
        for (NSEventType t : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp})
            [NSApp postEvent:[NSEvent mouseEventWithType:t location:NSMakePoint(cx, r.getHeight() - cy) modifierFlags:0
                                               timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber
                                                 context:nil eventNumber:0 clickCount:1 pressure:1] atStart:NO];
        pump(2);
    }
    [[NSNotificationCenter defaultCenter] removeObserver:obs];
    if (out) {
        CGImageRef img = CGWindowListCreateImage(CGRectNull, kCGWindowListOptionIncludingWindow,
                                                 (CGWindowID)win.windowNumber, kCGWindowImageBoundsIgnoreFraming);
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
        CGImageRelease(img);
        if ([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(out) atomically:YES])
            printf("screenshot -> %s\n", out);
    }
    v->removed();
    v->release();
    const bool ok = cx < 0 || menu;
    printf("RESULT: %s\n", ok ? (cx < 0 ? "editor opened" : "logo click opened the menu") : "FAILED");
    return ok ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) { printf("usage: vst3probe <bundle.vst3> --classes|--arp|--morph|--editor out.png [x y]\n"); return 2; }
        setvbuf(stdout, nullptr, _IOLBF, 0);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        CFBundleRef b = CFBundleCreate(nullptr, (__bridge CFURLRef)[NSURL fileURLWithPath:@(argv[1])]);
        if (!b || !CFBundleLoadExecutable(b)) { printf("FAIL load\n"); return 1; }
        auto entry = (bool (*)(CFBundleRef))CFBundleGetFunctionPointerForName(b, CFSTR("bundleEntry"));
        if (entry && !entry(b)) { printf("FAIL bundleEntry\n"); return 1; }
        auto getFactory = (S::IPluginFactory* (*)())CFBundleGetFunctionPointerForName(b, CFSTR("GetPluginFactory"));
        S::IPluginFactory* f = getFactory ? getFactory() : nullptr;
        if (!f) { printf("FAIL no factory\n"); return 1; }
        S::PFactoryInfo fi{};
        f->getFactoryInfo(&fi);
        printf("vendor: %s\n", fi.vendor);
        S::TUID cid{};
        bool found = false;
        for (S::int32 k = 0; k < f->countClasses(); ++k) {
            S::PClassInfo ci{};
            f->getClassInfo(k, &ci);
            printf("class %d: %s [%s]\n", k, ci.name, ci.category);
            if (!found && !strcmp(ci.category, "Audio Module Class")) { memcpy(cid, ci.cid, 16); found = true; }
        }
        if (!strcmp(argv[2], "--classes")) return found ? 0 : 1;
        if (f->createInstance(cid, V::IComponent::iid, (void**)&g_comp) != S::kResultOk || !g_comp) { printf("FAIL createInstance\n"); return 1; }
        g_comp->initialize(&g_host);
        g_comp->queryInterface(V::IAudioProcessor::iid, (void**)&g_proc);
        g_comp->queryInterface(V::IEditController::iid, (void**)&g_ctrl);
        if (!g_proc || !g_ctrl) { printf("FAIL interfaces\n"); return 1; }
        for (S::int32 k = 0; k < g_comp->getBusCount(V::kEvent, V::kOutput); ++k) g_comp->activateBus(V::kEvent, V::kOutput, k, true);
        g_comp->activateBus(V::kEvent, V::kInput, 0, true);
        g_comp->activateBus(V::kAudio, V::kOutput, 0, true);
        V::ProcessSetup ps{V::kRealtime, V::kSample32, 512, 44100.0};
        g_proc->setupProcessing(ps);
        g_comp->setActive(true);
        g_proc->setProcessing(true);
        int rc = 2;
        if (!strcmp(argv[2], "--arp")) rc = runArp();
        else if (!strcmp(argv[2], "--morph")) rc = runMorph();
        else if (!strcmp(argv[2], "--editor"))
            rc = runEditor(argc > 3 ? argv[3] : nullptr, argc > 5 ? atof(argv[4]) : -1, argc > 5 ? atof(argv[5]) : -1);
        g_proc->setProcessing(false);
        g_comp->setActive(false);
        g_comp->terminate();
        _exit(rc);   // FM8's background product scan may still run; static teardown under it crashes
    }
}
