// Minimal native VST2 host probe for macOS, the Mac twin of tools/vst2probe.cpp.
//
//     vst2probe <bundle.vst> --arp            hold a chord through the arp, count MIDI sent to the host
//     vst2probe <bundle.vst> --morph          send CC 11 and watch Morph X/Y (needs morph_cc=11)
//     vst2probe <bundle.vst> --params         print the parameters FM8.plus drives by index
//     vst2probe <bundle.vst> --editor out.png [click-x click-y] [seconds]
//                                             open the editor, optionally click a logical point,
//                                             report any menu that opens, and save a screenshot
//
// Exit code 0 when the check passes.
#import <Cocoa/Cocoa.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#define VSTCALLBACK
#include "../../src/vst2/vst2.h"

namespace {
int g_events = 0, g_noteOns = 0, g_first = 0;
unsigned char g_firstEv[8][3];
int g_sizeW = 0, g_sizeH = 0;

intptr_t host(AEffect*, int32_t op, int32_t, intptr_t val, void* ptr, float opt) {
    if (op == audioMasterVersion) return 2400;
    if (op == audioMasterSizeWindow) { g_sizeW = (int)val; g_sizeH = (int)opt; return 1; }
    if (op == audioMasterProcessEvents && ptr) {
        auto* evs = (VstEvents*)ptr;
        for (int i = 0; i < evs->numEvents; ++i) {
            auto* e = (VstMidiEvent*)evs->events[i];
            if (!e || e->type != kVstMidiType) continue;
            ++g_events;
            if (((unsigned char)e->midiData[0] & 0xf0) == 0x90 && e->midiData[2]) ++g_noteOns;
            if (g_first < 8) memcpy(g_firstEv[g_first++], e->midiData, 3);
        }
        return 1;
    }
    return 0;
}

int paramIndex(AEffect* eff, const char* want) {
    char name[256];
    for (int i = 0; i < eff->numParams; ++i) {
        memset(name, 0, sizeof name);
        eff->dispatcher(eff, effGetParamName, i, 0, name, 0);
        if (!strcasecmp(name, want)) return i;
    }
    return -1;
}

void start(AEffect* eff) {
    eff->dispatcher(eff, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    eff->dispatcher(eff, effSetBlockSize, 0, 512, nullptr, 0);
    eff->dispatcher(eff, effMainsChanged, 0, 1, nullptr, 0);
}

void send(AEffect* eff, int n, const unsigned char (*msgs)[3]) {
    // Static: FM8 keeps the pointer and reads the events during the next process call.
    static VstMidiEvent ev[8];
    static struct { int32_t numEvents; intptr_t reserved; VstEvent* events[8]; } evs;
    memset(ev, 0, sizeof ev);
    for (int i = 0; i < n; ++i) {
        ev[i].type = kVstMidiType; ev[i].byteSize = sizeof(VstMidiEvent);
        memcpy(ev[i].midiData, msgs[i], 3);
        evs.events[i] = (VstEvent*)&ev[i];
    }
    evs.numEvents = n;
    eff->dispatcher(eff, effProcessEvents, 0, 0, &evs, 0);
}

float render(AEffect* eff, int blocks) {
    static float l[512], r[512], il[512], ir[512];
    float* in[2] = {il, ir}; float* out[2] = {l, r};
    float peak = 0;
    for (int b = 0; b < blocks; ++b) {
        memset(l, 0, sizeof l); memset(r, 0, sizeof r);
        eff->processReplacing(eff, in, out, 512);
        for (float v : l) peak = fmaxf(peak, fabsf(v));
    }
    return peak;
}

int runArp(AEffect* eff) {
    start(eff);
    int arpOn = paramIndex(eff, "Arp On");
    if (arpOn < 0) arpOn = paramIndex(eff, "Arpeggiator On");
    printf("Arp On param index: %d\n", arpOn);
    if (arpOn < 0) { printf("RESULT: could not find the arpeggiator parameter\n"); return 1; }
    eff->setParameter(eff, arpOn, 1.0f);
    render(eff, 4);   // let the arp's On switch reach the engine before the chord arrives
    const unsigned char chord[3][3] = {{0x90, 60, 100}, {0x90, 64, 100}, {0x90, 67, 100}};
    send(eff, 3, chord);
    const float peak = render(eff, 200);
    printf("audio peak %.3f; MIDI events from plugin: %d (note-ons %d)\n", peak, g_events, g_noteOns);
    for (int i = 0; i < g_first; ++i) printf("  %02x %02x %02x\n", g_firstEv[i][0], g_firstEv[i][1], g_firstEv[i][2]);
    const bool ok = g_noteOns > 0 && peak > 0;
    printf("RESULT: %s\n", ok ? "arpeggiator emitted MIDI and audio" : "NO ARP MIDI OUT");
    return ok ? 0 : 1;
}

int runMorph(AEffect* eff) {
    start(eff);
    const int vals[5] = {0, 32, 64, 96, 127};
    float xs[5], ys[5];
    for (int k = 0; k < 5; ++k) {
        const unsigned char cc[1][3] = {{0xb0, 11, (unsigned char)vals[k]}};
        send(eff, 1, cc);
        render(eff, 4);
        xs[k] = eff->getParameter(eff, 21); ys[k] = eff->getParameter(eff, 22);
        printf("  CC11=%3d -> Morph X=%.3f Y=%.3f\n", vals[k], xs[k], ys[k]);
    }
    bool moved = false;
    for (int k = 1; k < 5; ++k) if (xs[k] != xs[0] || ys[k] != ys[0]) moved = true;
    printf("RESULT: %s\n", moved ? "morph follows the CC" : "MORPH DID NOT MOVE");
    return moved ? 0 : 1;
}

int runParams(AEffect* eff) {
    char name[256];
    for (int i : {21, 22, 153}) {
        memset(name, 0, sizeof name);
        eff->dispatcher(eff, effGetParamName, i, 0, name, 0);
        printf("param %d = %s\n", i, name);
    }
    return 0;
}

bool savePng(NSWindow* w, const char* path) {
    CGImageRef img = CGWindowListCreateImage(CGRectNull, kCGWindowListOptionIncludingWindow,
                                             (CGWindowID)w.windowNumber, kCGWindowImageBoundsIgnoreFraming);
    if (!img) return false;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    CGImageRelease(img);
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
}

// Dispatch events too: a bare run loop spin draws but never delivers the posted clicks.
void pump(double secs) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:secs];
    while ([until timeIntervalSinceNow] > 0) {
        NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                           inMode:NSDefaultRunLoopMode dequeue:YES];
        if (e) [NSApp sendEvent:e];
    }
}

int runEditor(AEffect* eff, const char* out, double cx, double cy, double secs) {
    start(eff);
    ERect* er = nullptr;
    eff->dispatcher(eff, effEditGetRect, 0, 0, &er, 0);
    if (!er) { printf("FAIL no editor rect\n"); return 1; }
    const int w = er->right - er->left, h = er->bottom - er->top;
    printf("effEditGetRect -> %dx%d\n", w, h);
    NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, w, h)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                  backing:NSBackingStoreBuffered defer:NO];
    win.title = @"FM8.plus probe";
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    const intptr_t opened = eff->dispatcher(eff, effEditOpen, 0, 0, (__bridge void*)win.contentView, 0);
    printf("effEditOpen -> %d, subviews %lu\n", (int)opened, (unsigned long)win.contentView.subviews.count);
    pump(secs);

    __block NSString* menuTitle = nil;
    id obs = [[NSNotificationCenter defaultCenter] addObserverForName:NSMenuDidBeginTrackingNotification object:nil
        queue:nil usingBlock:^(NSNotification* n) {
            NSMenu* m = n.object;
            menuTitle = m.title;
            NSMutableString* s = [NSMutableString string];
            for (NSMenuItem* it in m.itemArray) [s appendFormat:@"%@%@ | ", it.title, it.enabled ? @"" : @" (grey)"];
            printf("menu opened: %s: %s\n", m.title.UTF8String, s.UTF8String);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                savePng(win, "/tmp/fm8plus_menu.png");
                [m cancelTracking];
            });
        }];
    if (cx >= 0) {
        NSView* v = win.contentView.subviews.firstObject;
        // Logical FM8 coordinates run top-down from the editor's corner; the window's run bottom-up.
        const NSPoint p = NSMakePoint(cx, h - cy);
        for (NSEventType t : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp}) {
            NSEvent* e = [NSEvent mouseEventWithType:t location:p modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                                        windowNumber:win.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
            [NSApp postEvent:e atStart:NO];
        }
        (void)v;
        pump(secs);
    }
    [[NSNotificationCenter defaultCenter] removeObserver:obs];
    const bool shot = out && savePng(win, out);
    if (shot) printf("screenshot -> %s\n", out);
    eff->dispatcher(eff, effEditClose, 0, 0, nullptr, 0);
    const bool ok = opened && (cx < 0 || menuTitle != nil);
    printf("RESULT: %s\n", ok ? (cx < 0 ? "editor opened" : "logo click opened the menu") : "FAILED");
    return ok ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) { printf("usage: vst2probe <bundle.vst> --arp|--morph|--params|--editor out.png [x y] [secs]\n"); return 2; }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSString* path = [NSString stringWithUTF8String:argv[1]];
        CFBundleRef b = CFBundleCreate(nullptr, (__bridge CFURLRef)[NSURL fileURLWithPath:path]);
        if (!b || !CFBundleLoadExecutable(b)) { printf("FAIL load %s\n", argv[1]); return 1; }
        auto entry = (AEffect * (*)(AudioMasterCallback))CFBundleGetFunctionPointerForName(b, CFSTR("VSTPluginMain"));
        if (!entry) { printf("FAIL no VSTPluginMain\n"); return 1; }
        AEffect* eff = entry(&host);
        if (!eff || eff->magic != kEffectMagic) { printf("FAIL bad AEffect\n"); return 1; }
        char name[64] = {};
        eff->dispatcher(eff, effGetEffectName, 0, 0, name, 0);
        printf("loaded: %s uniqueID=0x%08x numParams=%d\n", name, eff->uniqueID, eff->numParams);
        eff->dispatcher(eff, effOpen, 0, 0, nullptr, 0);
        int rc = 2;
        const char* mode = argv[2];
        if (!strcmp(mode, "--arp")) rc = runArp(eff);
        else if (!strcmp(mode, "--morph")) rc = runMorph(eff);
        else if (!strcmp(mode, "--params")) rc = runParams(eff);
        else if (!strcmp(mode, "--editor"))
            rc = runEditor(eff, argc > 3 ? argv[3] : nullptr, argc > 5 ? atof(argv[4]) : -1, argc > 5 ? atof(argv[5]) : -1,
                           argc > 6 ? atof(argv[6]) : 2.0);
        eff->dispatcher(eff, effMainsChanged, 0, 0, nullptr, 0);
        eff->dispatcher(eff, effClose, 0, 0, nullptr, 0);
        return rc;
    }
}
