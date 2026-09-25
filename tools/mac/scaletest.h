// GUI Scale test shared by the three Mac probes, the twin of the Windows standalone checks: each
// probe supplies how to open and close its editor and what size the plug-in last asked the host
// for, and this drives FM8.plus's own "+" menu the way a user would.
//
//     <probe> ... --scale <out-dir> <expected start scale> <sequence, e.g. 1,2,3,4,1,3,2,1,2>
//
// Checks, printing one PASS/FAIL line each:
//   start     the editor opens at the scale saved in the INI
//   toggle    each step picked from the GUI Scale submenu: FM8's view covers logical x scale in the
//             window with its bounds still logical (whichever view carries the scaling), the host
//             asked for the same, and the screenshot matches the 1x one pixel for pixel (sampled at
//             each logical pixel's centre)
//   click     a click at the physical position of Navigator > Attributes lands on it
//   reopen    the editor closed and reopened keeps the scale
//   about     "About FM8" at the current scale opens FM8's panel
// A host honours every resize request (the window follows), as a DAW would. Screenshots of every
// step land in <out-dir>. Menu items that are greyed are reported, so an oversize step shows up.
#pragma once
#import <Cocoa/Cocoa.h>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace scaletest {

struct Host {
    NSWindow* win = nil;
    std::function<void()> open, close;
    std::function<NSView*()> fm8View;    // FM8's own view (its bounds stay logical)
    std::function<NSSize()> asked;       // the size the plug-in last asked the host for
};

// Logical FM8 coordinates, top-down from the editor's corner (FM8 1.4.6, 948x562).
constexpr double kPlusX = 118.4, kPlusY = 47;
constexpr double kAttrX = 60, kAttrY = 160, kBrowserX = 60, kBrowserY = 134;

inline int g_fail = 0;
inline void verdict(bool ok, const std::string& what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}

inline void pump(double secs) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:secs];
    while ([until timeIntervalSinceNow] > 0)
        if (NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                               inMode:NSDefaultRunLoopMode dequeue:YES]) [NSApp sendEvent:e];
}

struct Image { int w = 0, h = 0; std::vector<uint8_t> px; };   // RGBA8

// The window's content area only: the capture includes the title bar, which does not scale.
inline Image grab(NSWindow* w, const std::string& path) {
    Image out;
    CGImageRef whole = CGWindowListCreateImage(CGRectNull, kCGWindowListOptionIncludingWindow,
                                               (CGWindowID)w.windowNumber, kCGWindowImageBoundsIgnoreFraming);
    if (!whole) return out;
    const size_t ch = (size_t)w.contentView.frame.size.height, wh = CGImageGetHeight(whole);
    CGImageRef img = CGImageCreateWithImageInRect(whole, CGRectMake(0, wh > ch ? wh - ch : 0, CGImageGetWidth(whole), std::min(ch, wh)));
    CGImageRelease(whole);
    if (!img) return out;
    out.w = (int)CGImageGetWidth(img); out.h = (int)CGImageGetHeight(img);
    out.px.assign((size_t)out.w * out.h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(out.px.data(), out.w, out.h, 8, out.w * 4, cs, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(cg, CGRectMake(0, 0, out.w, out.h), img);
    CGContextRelease(cg); CGColorSpaceRelease(cs);
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(path.c_str()) atomically:YES];
    CGImageRelease(img);
    return out;
}

// Fraction of logical pixels whose colour differs noticeably between the 1x reference and the
// scaled image sampled at each logical pixel's centre. Clipped to the part both images show, and
// to a logical clip (the part of the window on screen) when one is given.
inline double mismatch(const Image& ref, const Image& big, double s, int clipW = 1 << 30, int clipH = 1 << 30) {
    if (!ref.w || !big.w) return 1.0;
    const int w = std::min({ref.w, (int)(big.w / s), clipW}), h = std::min({ref.h, (int)(big.h / s), clipH});
    long bad = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* a = &ref.px[((size_t)y * ref.w + x) * 4];
            const int bx = std::min(big.w - 1, (int)(x * s + s / 2)), by = std::min(big.h - 1, (int)(y * s + s / 2));
            const uint8_t* b = &big.px[((size_t)by * big.w + bx) * 4];
            if (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) > 48) ++bad;
        }
    return w && h ? (double)bad / ((double)w * h) : 1.0;
}

// Click a logical FM8 point at its physical position: the view's frame rect in window coordinates,
// top-left corner, plus the point times the scale.
inline void click(Host& h, double lx, double ly, double s) {
    NSView* v = h.fm8View();
    const NSRect r = [v.superview convertRect:v.frame toView:nil];
    const NSPoint p = NSMakePoint(NSMinX(r) + lx * s, NSMaxY(r) - ly * s);
    for (NSEventType t : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp})
        [NSApp postEvent:[NSEvent mouseEventWithType:t location:p modifierFlags:0
                                           timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:h.win.windowNumber
                                             context:nil eventNumber:0 clickCount:1 pressure:1] atStart:NO];
    pump(1.0);
}

// Open the "+" menu and pick an item: a GUI Scale step (scaleIdx >= 0) or "About FM8".
// Returns false when the menu did not open or the item was greyed.
inline bool pick(Host& h, double s, int scaleIdx, bool about, std::string* greyed = nullptr) {
    __block bool opened = false, done = false;
    __block std::string grey;
    id obs = [[NSNotificationCenter defaultCenter] addObserverForName:NSMenuDidBeginTrackingNotification object:nil queue:nil
        usingBlock:^(NSNotification* n) {
            NSMenu* m = n.object;
            if (opened || ![m.title isEqualToString:@"FM8.plus"]) return;
            opened = true;
            NSInteger at = -1;
            NSMenu* target = m;
            if (about) at = [m indexOfItemWithTitle:@"About FM8"];
            else if (NSMenuItem* sc = [m itemWithTitle:@"GUI Scale"]) {
                target = sc.submenu;
                for (NSMenuItem* it in target.itemArray) if (!it.enabled) grey += std::string(it.title.UTF8String) + " ";
                at = scaleIdx;
            }
            if (at >= 0 && at < target.numberOfItems && [target itemAtIndex:at].enabled) {
                [target performActionForItemAtIndex:at];
                done = true;
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 3), dispatch_get_main_queue(), ^{ [m cancelTracking]; });
        }];
    click(h, kPlusX, kPlusY, s);
    pump(1.0);
    [[NSNotificationCenter defaultCenter] removeObserver:obs];
    if (greyed) *greyed = grey;
    return opened && done;
}

// The host's half of a resize: the window follows whatever the plug-in asked for, pinned to the
// screen's top-left so the top-left of the editor stays visible when the window outgrows the screen.
inline void follow(Host& h) {
    const NSSize a = h.asked();
    if (a.width > 0 && a.height > 0) [h.win setContentSize:a];
    const NSRect vf = h.win.screen.visibleFrame;
    [h.win setFrameTopLeftPoint:NSMakePoint(NSMinX(vf), NSMaxY(vf))];
    pump(1.0);
}

// The logical size of the editor area that is on screen. macOS leaves the off-screen part of a
// window unpainted after a partial redraw, so only this part can be compared after a click.
inline NSSize onScreen(Host& h, double s) {
    const NSRect vf = h.win.screen.visibleFrame;
    const NSRect c = [h.win contentRectForFrameRect:h.win.frame];
    const NSRect vis = NSIntersectionRect(vf, c);
    return NSMakeSize(floor(vis.size.width / s), floor(vis.size.height / s));
}

inline bool near(double a, double b) { return std::fabs(a - b) <= 1.0; }

// Size checks at scale s, then a screenshot compared against the 1x reference.
inline void check(Host& h, const std::string& dir, const std::string& tag, double s, NSSize base, Image& ref1x) {
    NSView* v = h.fm8View();
    const NSSize f = [v convertRect:v.bounds toView:nil].size;   // in window pixels
    const NSSize b = v.bounds.size, a = h.asked(), c = h.win.contentView.frame.size;
    const NSRect screen = h.win.screen.visibleFrame;
    char msg[400];
    const bool sizes = near(f.width, base.width * s) && near(f.height, base.height * s) && near(b.width, base.width) &&
                       near(b.height, base.height) && near(a.width, base.width * s) && near(a.height, base.height * s);
    snprintf(msg, sizeof msg, "%s %.0fx: view in window %.0fx%.0f bounds %.0fx%.0f, asked host %.0fx%.0f, window %.0fx%.0f%s",
             tag.c_str(), s, f.width, f.height, b.width, b.height, a.width, a.height, c.width, c.height,
             (c.height > screen.size.height || c.width > screen.size.width) ? " (bigger than the screen)" : "");
    verdict(sizes, msg);
    Image img = grab(h.win, dir + "/" + tag + ".png");
    if (s == 1.0 && !ref1x.w) { ref1x = img; return; }
    if (ref1x.w) {
        const double mm = mismatch(ref1x, img, s);
        snprintf(msg, sizeof msg, "%s %.0fx: pixels vs 1x differ on %.2f%% of logical pixels (%dx%d image)", tag.c_str(), s,
                 mm * 100, img.w, img.h);
        verdict(mm < 0.01, msg);
    }
}

inline int run(Host& h, const std::string& dir, double start, const std::string& seq) {
    h.open();
    pump(3);
    const NSSize base = h.fm8View().bounds.size;
    follow(h);
    printf("logical editor %.0fx%.0f, screen %.0fx%.0f\n", base.width, base.height,
           h.win.screen.visibleFrame.size.width, h.win.screen.visibleFrame.size.height);
    Image ref1x, attr1x;
    check(h, dir, "start", start, base, ref1x);
    double s = start;

    // A 1x reference is needed for the pixel comparisons; take it now if we did not start there.
    auto clickTest = [&](double sc) {
        click(h, kAttrX, kAttrY, sc);
        Image img = grab(h.win, dir + "/click_" + std::to_string((int)sc) + "x.png");
        if (sc == 1.0) attr1x = img;
        else if (attr1x.w) {
            const NSSize vis = onScreen(h, sc);
            const double mm = mismatch(attr1x, img, sc, (int)vis.width, (int)vis.height);
            char msg[200];
            snprintf(msg, sizeof msg, "click %.0fx: Attributes clicked at its physical position (page differs from 1x on %.2f%%"
                     " of the %.0fx%.0f logical px on screen)", sc, mm * 100, vis.width, vis.height);
            verdict(mm < 0.01, msg);
        }
        click(h, kBrowserX, kBrowserY, sc);
    };
    if (s == 1.0) clickTest(1.0);

    size_t i = 0;
    for (int step = 0; i <= seq.size(); ++step) {
        const size_t j = seq.find(',', i);
        const std::string tok = seq.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = (j == std::string::npos) ? seq.size() + 1 : j + 1;
        if (tok.empty()) continue;
        const int k = atoi(tok.c_str());
        std::string grey;
        const bool picked = pick(h, s, k - 1, false, &grey);
        if (!grey.empty()) printf("      greyed in GUI Scale: %s\n", grey.c_str());
        char msg[120];
        snprintf(msg, sizeof msg, "toggle %d: picked %dx from the GUI Scale menu at %.0fx", step, k, s);
        verdict(picked, msg);
        if (!picked) continue;
        s = k;
        follow(h);
        check(h, dir, "toggle" + std::to_string(step) + "_" + tok + "x", s, base, ref1x);
        if (s == 1.0 && !attr1x.w) clickTest(1.0);
        else if (s != 1.0 && attr1x.w) clickTest(s);
    }

    h.close();
    pump(1);
    h.open();
    pump(3);
    follow(h);
    check(h, dir, "reopen", s, base, ref1x);

    NSArray* before = [NSApp.windows copy];
    __block std::string about = "no new window";
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        for (NSWindow* w in NSApp.windows)
            if (![before containsObject:w] && w.visible) {
                char b[160];
                snprintf(b, sizeof b, "'%s' %.0fx%.0f", w.title.UTF8String, w.contentView.frame.size.width,
                         w.contentView.frame.size.height);
                about = b;
                grab(w, dir + "/about.png");
                if (NSApp.modalWindow == w) [NSApp stopModal];
                [w close];
                break;
            }
    });
    const bool aboutPicked = pick(h, s, -1, true);
    pump(3);
    verdict(aboutPicked && about != "no new window", "about at " + std::to_string((int)s) + "x: " + about);

    h.close();
    printf("RESULT: %s (%d failed)\n", g_fail ? "GUI Scale test FAILED" : "GUI Scale test passed", g_fail);
    return g_fail ? 1 : 0;
}

} // namespace scaletest
