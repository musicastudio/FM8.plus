// Minimal Audio Unit host probe for FM8.plus.component, the AU twin of vst2probe.mm.
//
//     auprobe --arp                      hold a chord through the arp, count the AU's MIDI output
//     auprobe --morph                    send CC 11 and watch Morph X/Y (needs morph_cc=11)
//     auprobe --state                    round-trip ClassInfo and check the FM8.plus key survives
//     auprobe --editor out.png [x y]     open the Cocoa view, optionally click a logical point
//     auprobe --scale <dir> <start> <seq>   GUI Scale test (tools/mac/scaletest.h)
//
// Must run in the GUI login session (the AU registrar lives there), e.g. from Terminal or `open`.
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <CoreMIDI/CoreMIDI.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <initializer_list>
#include "scaletest.h"

namespace {
AudioUnit g_au = nullptr;
int g_events = 0, g_noteOns = 0;
unsigned char g_first[8][3];
int g_nFirst = 0;
double g_beat = 0;

OSStatus midiOut(void*, const AudioTimeStamp*, UInt32, const MIDIPacketList* list) {
    const MIDIPacket* p = &list->packet[0];
    for (UInt32 k = 0; k < list->numPackets; ++k, p = MIDIPacketNext(p)) {
        ++g_events;
        if ((p->data[0] & 0xf0) == 0x90 && p->data[2]) ++g_noteOns;
        if (g_nFirst < 8) memcpy(g_first[g_nFirst++], p->data, 3);
    }
    return noErr;
}

OSStatus beatAndTempo(void*, Float64* beat, Float64* tempo) {
    if (beat) *beat = g_beat;
    if (tempo) *tempo = 120.0;
    return noErr;
}
OSStatus transportState(void*, Boolean* playing, Boolean* changed, Float64* sample, Boolean* cycling, Float64*, Float64*) {
    if (playing) *playing = true;
    if (changed) *changed = false;
    if (sample) *sample = g_beat * 22050.0;
    if (cycling) *cycling = false;
    return noErr;
}

float render(int blocks) {
    static float l[512], r[512];
    struct { UInt32 n; AudioBuffer b[2]; } abl;
    float peak = 0;
    for (int k = 0; k < blocks; ++k) {
        abl.n = 2;
        abl.b[0] = {1, sizeof l, l};
        abl.b[1] = {1, sizeof r, r};
        AudioUnitRenderActionFlags f = 0;
        AudioTimeStamp ts{};
        ts.mSampleTime = k * 512.0;
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        if (AudioUnitRender(g_au, &f, &ts, 0, 512, (AudioBufferList*)&abl)) return -1;
        for (float v : l) peak = fmaxf(peak, fabsf(v));
        g_beat += 512.0 / 22050.0;
    }
    return peak;
}

int runArp() {
    AudioUnitSetParameter(g_au, 136, kAudioUnitScope_Global, 0, 1.0f, 0);   // Arp On, as the VST2 index
    render(4);
    for (int n : {60, 64, 67}) MusicDeviceMIDIEvent(g_au, 0x90, n, 100, 0);
    const float peak = render(200);
    printf("audio peak %.3f; MIDI events from plugin: %d (note-ons %d)\n", peak, g_events, g_noteOns);
    for (int i = 0; i < g_nFirst; ++i) printf("  %02x %02x %02x\n", g_first[i][0], g_first[i][1], g_first[i][2]);
    const bool ok = g_noteOns > 0;
    printf("RESULT: %s\n", ok ? "arpeggiator emitted MIDI" : "NO ARP MIDI OUT");
    return ok ? 0 : 1;
}

int runMorph() {
    float xs[5], ys[5];
    const int vals[5] = {0, 32, 64, 96, 127};
    for (int k = 0; k < 5; ++k) {
        MusicDeviceMIDIEvent(g_au, 0xb0, 11, vals[k], 0);
        render(4);
        AudioUnitGetParameter(g_au, 21, kAudioUnitScope_Global, 0, &xs[k]);
        AudioUnitGetParameter(g_au, 22, kAudioUnitScope_Global, 0, &ys[k]);
        printf("  CC11=%3d -> Morph X=%.3f Y=%.3f\n", vals[k], xs[k], ys[k]);
    }
    bool moved = false;
    for (int k = 1; k < 5; ++k) if (xs[k] != xs[0] || ys[k] != ys[0]) moved = true;
    printf("RESULT: %s\n", moved ? "morph follows the CC" : "MORPH DID NOT MOVE");
    return moved ? 0 : 1;
}

int runState() {
    CFPropertyListRef pl = nullptr;
    UInt32 sz = sizeof pl;
    if (AudioUnitGetProperty(g_au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &pl, &sz) || !pl) {
        printf("RESULT: FAILED get ClassInfo\n"); return 1;
    }
    const bool has = CFDictionaryContainsKey((CFDictionaryRef)pl, CFSTR("FM8.plus"));
    const OSStatus e = AudioUnitSetProperty(g_au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &pl, sizeof pl);
    printf("ClassInfo keys %ld, FM8.plus key %d, set back -> %d\n", (long)CFDictionaryGetCount((CFDictionaryRef)pl), has, (int)e);
    CFRelease(pl);
    printf("RESULT: %s\n", has && !e ? "state carries FM8.plus settings" : "FAILED");
    return has && !e ? 0 : 1;
}

bool savePng(NSWindow* w, const char* path) {
    CGImageRef img = CGWindowListCreateImage(CGRectNull, kCGWindowListOptionIncludingWindow,
                                             (CGWindowID)w.windowNumber, kCGWindowImageBoundsIgnoreFraming);
    if (!img) return false;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    CGImageRelease(img);
    return [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(path) atomically:YES];
}

void pump(double secs) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:secs];
    while ([until timeIntervalSinceNow] > 0) {
        NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                           inMode:NSDefaultRunLoopMode dequeue:YES];
        if (e) [NSApp sendEvent:e];
    }
}

int runEditor(const char* out, double cx, double cy) {
    AudioUnitCocoaViewInfo info{};
    UInt32 sz = sizeof info;
    if (AudioUnitGetProperty(g_au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &info, &sz)) {
        printf("RESULT: FAILED no CocoaUI\n"); return 1;
    }
    NSBundle* b = [NSBundle bundleWithURL:(__bridge NSURL*)info.mCocoaAUViewBundleLocation];
    NSString* cls = (__bridge NSString*)info.mCocoaAUViewClass[0];
    printf("view factory %s in %s\n", cls.UTF8String, b.bundlePath.UTF8String);
    id<AUCocoaUIBase> f = [[[b classNamed:cls] alloc] init];
    NSView* v = [f uiViewForAudioUnit:g_au withSize:NSZeroSize];
    if (!v) { printf("RESULT: FAILED no view\n"); return 1; }
    const NSSize s = v.frame.size;
    printf("view %.0fx%.0f\n", s.width, s.height);
    NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, s.width, s.height)
                                                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    win.title = @"FM8.plus AU probe";
    win.contentView = v;
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    pump(3);
    __block NSString* menu = nil;
    id obs = [[NSNotificationCenter defaultCenter] addObserverForName:NSMenuDidBeginTrackingNotification object:nil queue:nil
        usingBlock:^(NSNotification* n) {
            NSMenu* m = n.object;
            menu = m.title;
            printf("menu opened: %s, %ld items\n", m.title.UTF8String, (long)m.numberOfItems);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{ [m cancelTracking]; });
        }];
    if (cx >= 0) {
        for (NSEventType t : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp})
            [NSApp postEvent:[NSEvent mouseEventWithType:t location:NSMakePoint(cx, s.height - cy) modifierFlags:0
                                               timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber
                                                 context:nil eventNumber:0 clickCount:1 pressure:1] atStart:NO];
        pump(2);
    }
    [[NSNotificationCenter defaultCenter] removeObserver:obs];
    if (out && savePng(win, out)) printf("screenshot -> %s\n", out);
    const bool ok = cx < 0 || menu;
    printf("RESULT: %s\n", ok ? (cx < 0 ? "editor opened" : "logo click opened the menu") : "FAILED");
    return ok ? 0 : 1;
}
// An AU host learns of a resize from the view's frame, so "asked" is the container's frame size.
int runScale(const char* dir, double startScale, const char* seq) {
    render(8);   // "About FM8" waits for the audio thread to have run once
    NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(40, 40, 400, 300)
                                                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    win.title = @"FM8.plus AU scale test";
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    NSView* container = nil;
    scaletest::Host h;
    h.win = win;
    h.open = [&] {
        AudioUnitCocoaViewInfo info{};
        UInt32 sz = sizeof info;
        if (AudioUnitGetProperty(g_au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &info, &sz)) return;
        NSBundle* b = [NSBundle bundleWithURL:(__bridge NSURL*)info.mCocoaAUViewBundleLocation];
        id<AUCocoaUIBase> f = [[[b classNamed:(__bridge NSString*)info.mCocoaAUViewClass[0]] alloc] init];
        container = [f uiViewForAudioUnit:g_au withSize:NSZeroSize];
        [win setContentSize:container.frame.size];
        [win.contentView addSubview:container];
        [container setFrameOrigin:NSZeroPoint];
    };
    h.close = [&] { [container removeFromSuperview]; container = nil; };
    h.fm8View = [&] { return (NSView*)(container.subviews.firstObject ?: container); };
    h.asked = [&] { return container ? container.frame.size : NSZeroSize; };
    return scaletest::run(h, dir, startScale, seq);
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) { printf("usage: auprobe --arp|--morph|--state|--editor out.png [x y]\n"); return 2; }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        AudioComponentDescription d{'aumu', 'F8pl', 'Msca', 0, 0};
        AudioComponent c = AudioComponentFindNext(nullptr, &d);
        if (!c || AudioComponentInstanceNew(c, &g_au)) { printf("FAIL: FM8.plus AU not found or would not open\n"); return 1; }
        UInt32 maxFrames = 4096;
        AudioUnitSetProperty(g_au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
        HostCallbackInfo hc{};
        hc.beatAndTempoProc = &beatAndTempo;
        hc.transportStateProc = &transportState;
        AudioUnitSetProperty(g_au, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0, &hc, sizeof hc);
        AUMIDIOutputCallbackStruct mo{&midiOut, nullptr};
        const OSStatus me = AudioUnitSetProperty(g_au, kAudioUnitProperty_MIDIOutputCallback, kAudioUnitScope_Global, 0, &mo, sizeof mo);
        CFArrayRef outs = nullptr; UInt32 sz = sizeof outs;
        AudioUnitGetProperty(g_au, kAudioUnitProperty_MIDIOutputCallbackInfo, kAudioUnitScope_Global, 0, &outs, &sz);
        printf("MIDI outputs: %ld (set callback -> %d)\n", outs ? (long)CFArrayGetCount(outs) : 0L, (int)me);
        if (AudioUnitInitialize(g_au)) { printf("FAIL: initialize\n"); return 1; }
        int rc = 2;
        if (!strcmp(argv[1], "--arp")) rc = runArp();
        else if (!strcmp(argv[1], "--morph")) rc = runMorph();
        else if (!strcmp(argv[1], "--state")) rc = runState();
        else if (!strcmp(argv[1], "--scale") && argc > 4) rc = runScale(argv[2], atof(argv[3]), argv[4]);
        else if (!strcmp(argv[1], "--editor"))
            rc = runEditor(argc > 2 ? argv[2] : nullptr, argc > 4 ? atof(argv[3]) : -1, argc > 4 ? atof(argv[4]) : -1);
        AudioUnitUninitialize(g_au);
        AudioComponentInstanceDispose(g_au);
        return rc;
    }
}
