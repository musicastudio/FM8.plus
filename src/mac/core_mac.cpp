// FM8.plus shared core, macOS. The same feature logic as core/fm8plus.cpp, reaching FM8 through its
// symbol table instead of RVA tables. What each hook is, and why, is in docs/mac.md.
//
// Three kinds of hook, least invasive first:
//  - data: FM8's in-memory resource map (the "FM8+" wordmark) and FormMain's vtable (the logo
//    click). Works in every host on both architectures.
//  - calls: FM8's own functions invoked directly (morph setter, About panel).
//  - breakpoint: the arp dispatch, the only place arp notes can be told from live ones, is
//    redirected by a hardware breakpoint (machook.h redirect) to replaceArpDispatch. No code page
//    is written, so it holds in hardened hosts and on arm64 as well.
#include "core_mac.h"
#include "machook.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace fm8plus {
namespace Core {

thread_local InstanceState* current = nullptr;

namespace {
InstanceState* g_singleton = nullptr;
void (*g_arpBlockCb)(InstanceState&) = nullptr;
void (*g_logoHandler)(InstanceState*) = nullptr;
bool g_installed = false, g_arpHooked = false, g_logoWidened = false;
std::atomic<float> g_guiScale{1.0f};

enum Sym {
    kArpDispatch, kMidiHandler, kGetMidiEvents, kGetEditBuffer, kSetParameter, kGetFormManager, kAboutDoShow,
    kVtFm8, kTiFm8, kVtFormMain, kOnControl, kOnControlThunk, kResourceMap, kSymCount
};
const char* const kNames[kSymCount] = {
    "__ZN7FM8Midi36processMidiEventsFromMIDIArpeggiatorEN2NI2AB7eThreadEj",
    "__ZN7FM8Midi16processMidiEventERKN2NI2AB9MidiEventENS1_7eThreadE",
    "__ZN2NI15MIDIArpeggiator13getMidiEventsEj",
    "__ZNK3FM813GetEditBufferEv",
    "__ZN13FM8EditBuffer12SetParameterE13ParameterTagsfb",
    "__ZNK3FM814getFormManagerEv",
    "__ZN2NI3NGL20StandardAboutDialog210MSVCHelperI14FM8AboutDialogE6doShowEPNS_3UIA10WindowBaseE",
    "__ZTV3FM8",
    "__ZTI3FM8",
    "__ZTV8FormMain",
    "__ZN8FormMain14onControlEventEjjPN2NI3NGL16ControlEventDataE",
    "__ZThn608_N8FormMain14onControlEventEjjPN2NI3NGL16ControlEventDataE",
    "__ZN2NI3UIA14ResourceFacadeL16g_theResourceMapE",
};
void* g_sym[kSymCount] = {};

// Layout FM8 1.4.6 uses on the Mac, read from the disassembly (docs/mac.md): the MIDI event packs
// [data2][data1][status] at +0x10 as on Windows, FM8Midi keeps its FM8 at +8, FormMain at +0x270,
// the EditBuffer its arpeggiator at +0x29a0, and the arp's event array counts at +8 and points at
// 0x28-byte elements from +0x10. The same on x86_64 and arm64.
constexpr size_t kEvWord = 0x10, kMidiFm8 = 0x08, kFormMainFm8 = 0x270, kThunkDelta = 608;
constexpr size_t kEditBufArp = 0x29a0, kArrCount = 0x08, kArrData = 0x10, kArrStride = 0x28;
constexpr uint32_t kTagMorphX = 0x84, kTagMorphY = 0x85;
constexpr unsigned kLogoControl = 5, kNotifyClick = 0x186a1;

using CtlFn  = void (*)(void* form, unsigned ctl, unsigned notify, void* data);
CtlFn o_ctl = nullptr;

// ---- safe memory probing -----------------------------------------------------------------------
bool peek(const void* p, void* out, size_t n) {
    mach_vm_size_t got = 0;
    return p && mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)p, n,
                                       (mach_vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}

// The complete FM8 object `p` points into, or null. An Itanium vtable pointer carries its class's
// typeinfo one slot before it and the distance to the object's start two slots before it, so this
// recognises any base-class subobject of FM8 without knowing FM8's layout.
void* asFm8(void* p) {
    void* vp = nullptr;
    if (!peek(p, &vp, sizeof vp)) return nullptr;
    const uintptr_t v = (uintptr_t)vp, vt = (uintptr_t)g_sym[kVtFm8];
    if (v < vt || v > vt + 0x4000 || (v & 7)) return nullptr;
    intptr_t pre[2];
    if (!peek((void*)(v - 16), pre, sizeof pre) || (void*)pre[1] != g_sym[kTiFm8]) return nullptr;
    return (char*)p + pre[0];
}

// ---- instances ---------------------------------------------------------------------------------
constexpr int kMaxInst = 64;
struct Binding { std::atomic<InstanceState*> st{nullptr}; std::atomic<void*> fm8{nullptr}; };
Binding g_bind[kMaxInst];

InstanceState* instanceFor(void* fm8) {
    for (auto& b : g_bind) if (fm8 && b.fm8.load() == fm8) return b.st.load();
    return nullptr;
}

// ---- arp and MIDI detours (as core/fm8plus.cpp) -----------------------------------------------
bool routeArpEvent(InstanceState& st, uint8_t status, uint8_t d1, uint8_t d2, int32_t off) {
    const uint8_t type = status & 0xf0;
    const int ch = status & 0x0f;
    const bool isOn  = (type == 0x90) && (d2 > 0);
    const bool isOff = (type == 0x80) || ((type == 0x90) && (d2 == 0));
    const ArpMode mode = (ArpMode)st.arpMode.load(std::memory_order_relaxed);
    if (mode != ArpMode::Internal) {
        if (isOn) { st.pushOut(status, d1, d2, off); st.extOn.set(ch, d1); }
        else if (isOff) { if (st.extOn.test(ch, d1)) { st.pushOut(status, d1, d2, off); st.extOn.clear(ch, d1); } }
        else st.pushOut(status, d1, d2, off);
    }
    bool playInternal = true;
    if (mode == ArpMode::MidiOnly) playInternal = isOn ? false : isOff ? st.intOn.test(ch, d1) : true;
    if (isOn && playInternal) st.intOn.set(ch, d1);
    if (isOff && playInternal) st.intOn.clear(ch, d1);
    return playInternal;
}

// Takes over FM8Midi::processMidiEventsFromMIDIArpeggiator whenever the breakpoint fires. It is
// FM8's own function line for line (fetch the arp's events for this position, hand each to
// processMidiEvent), with the FM8.plus routing between the two. It runs for plain FM8 instances on
// an armed thread too, which then get exactly the stock loop. It must never call the original:
// that address is the breakpoint.
void replaceArpDispatch(void* fm8midi, int thread, unsigned pos) {
    void* fm8 = *(void**)((char*)fm8midi + kMidiFm8);
    void* eb = ((void* (*)(void*))g_sym[kGetEditBuffer])(fm8);
    void* arp = *(void**)((char*)eb + kEditBufArp);
    auto* arr = (char*)((void* (*)(void*, unsigned))g_sym[kGetMidiEvents])(arp, pos);
    const auto handle = (void (*)(void*, const void*, int))g_sym[kMidiHandler];
    InstanceState* st = current ? current : g_singleton;
    const uint32_t n = *(uint32_t*)(arr + kArrCount);
    const char* ev = *(char**)(arr + kArrData);
    if (!st) { for (uint32_t k = 0; k < n; ++k) handle(fm8midi, ev + k * kArrStride, thread); return; }
    if (st->pendingFlush.exchange(false, std::memory_order_relaxed)) flushExternal(*st);
    st->appObj.store(fm8, std::memory_order_relaxed);
    st->editBuf.store(eb, std::memory_order_relaxed);
    const bool route = st->arpMode.load(std::memory_order_relaxed) != (uint8_t)ArpMode::Internal;
    for (uint32_t k = 0; k < n; ++k) {
        const char* e = ev + k * kArrStride;
        const uint32_t w = *(const uint32_t*)(e + kEvWord);
        if (!route || routeArpEvent(*st, (uint8_t)(w >> 16), (w >> 8) & 0x7f, w & 0x7f, (int32_t)pos))
            handle(fm8midi, e, thread);
    }
    if (g_arpBlockCb) g_arpBlockCb(*st);
}

// ---- the logo click: FormMain's vtable ---------------------------------------------------------
// FormMain::onControlEvent is virtual and reached through two vtable entries (the primary one and
// a this-adjusting thunk for its listener base). Both are redirected; the class is shared with any
// plain FM8 in the process, so the detour only acts for instances we have bound.
const bool g_trace = getenv("FM8PLUS_TRACE") != nullptr;   // stderr trace for the probe

void detourControl(void* form, unsigned ctl, unsigned notify, void* data) {
    if (g_trace) fprintf(stderr, "FM8.plus: control %u notify %#x\n", ctl, notify);
    if (ctl == kLogoControl && notify == kNotifyClick && g_logoHandler) {
        void* fm8 = nullptr;
        peek((char*)form + kFormMainFm8, &fm8, sizeof fm8);
        if (InstanceState* st = instanceFor(asFm8(fm8))) { g_logoHandler(st); return; }
    }
    o_ctl(form, ctl, notify, data);
}
void detourControlThunk(void* sub, unsigned ctl, unsigned notify, void* data) {
    detourControl((char*)sub - kThunkDelta, ctl, notify, data);
}

bool patchPointer(void** slot, void* value) {
    const uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(vm_page_size - 1);
    if (mach_vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS)
        return false;
    *slot = value;
    mach_vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
    return true;
}

bool hookControlEvents() {
    if (!g_sym[kVtFormMain] || !g_sym[kOnControl] || !g_sym[kOnControlThunk]) return false;
    auto** vt = (void**)g_sym[kVtFormMain];
    void** primary = nullptr; void** thunk = nullptr;
    for (int i = 0; i < 0x200 && !(primary && thunk); ++i) {
        if (vt[i] == g_sym[kOnControl]) primary = &vt[i];
        else if (vt[i] == g_sym[kOnControlThunk]) thunk = &vt[i];
    }
    if (!primary || !thunk) return false;
    o_ctl = (CtlFn)g_sym[kOnControl];
    return patchPointer(primary, (void*)&detourControl) && patchPointer(thunk, (void*)&detourControlThunk);
}

// ---- the "FM8+" wordmark -----------------------------------------------------------------------
// FM8's resource facade looks in g_theResourceMap, an in-memory std::map<type, std::map<id, entry>*>,
// before it opens FM8.rsrc (NI::UIA::ResourceFacade::getResource), so serving a resource is an
// insert into that map. Same edit as core/rsrc.cpp: FRM 5 and 15 get the wordmark rect widened, the
// XTGA 193 bitmap gets 22px of room and the "+".
struct ResEntry { const void* data; size_t size; };
using ResInner = std::map<uint32_t, ResEntry>;
using ResMap = std::map<uint32_t, ResInner*>;

constexpr int32_t kLogoX1 = 21, kLogoY1 = 35, kLogoX2 = 116, kLogoY2 = 58, kShift = 11, kPlusW = 22;
constexpr int kTextDx = 4;
constexpr float kPlusCx = 108.4f, kPlusCy = 12.46f, kPlusHalf = 7.37f, kPlusThick = 1.515f, kPlusSlant = 0.1767f;
constexpr uint32_t kPlusRgb = 0x6b7d86;

std::vector<uint8_t> g_frm5, g_frm15, g_pic;

constexpr uint32_t fourcc(const char* t) {
    return ((uint32_t)(uint8_t)t[0] << 24) | ((uint32_t)(uint8_t)t[1] << 16) | ((uint32_t)(uint8_t)t[2] << 8) | (uint8_t)t[3];
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

// One resource out of a classic resource-fork file (FM8.rsrc).
bool rsrcGet(const std::vector<uint8_t>& f, uint32_t type, int16_t id, std::vector<uint8_t>& out) {
    if (f.size() < 16) return false;
    const uint32_t dOff = be32(&f[0]), mOff = be32(&f[4]);
    if (mOff + 30 > f.size()) return false;
    const uint8_t* m = &f[mOff];
    const uint32_t tl = be16(m + 24);
    const int nt = be16(m + tl) + 1;
    for (int i = 0; i < nt; ++i) {
        const uint8_t* t = m + tl + 2 + 8 * i;
        if (be32(t) != type) continue;
        const int n = be16(t + 4) + 1;
        const uint8_t* ref = m + tl + be16(t + 6);
        for (int k = 0; k < n; ++k, ref += 12) {
            if ((int16_t)be16(ref) != id) continue;
            const uint32_t off = dOff + (be32(ref + 4) & 0xffffff);
            if (off + 4 > f.size()) return false;
            const uint32_t len = be32(&f[off]);
            if (off + 4 + len > f.size()) return false;
            out.assign(&f[off + 4], &f[off + 4] + len);
            return true;
        }
    }
    return false;
}

// The Mac forms are big-endian, so the rect is four big-endian int32s (the Windows forms hold the
// same four numbers little-endian).
bool widenForm(std::vector<uint8_t>& frm) {
    auto put = [](uint8_t* p, int32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; };
    uint8_t from[16], to[16];
    const int32_t a[4] = {kLogoX1, kLogoY1, kLogoX2, kLogoY2};
    const int32_t b[4] = {kLogoX1 - kShift, kLogoY1, kLogoX2 + kPlusW - kShift, kLogoY2};
    for (int i = 0; i < 4; ++i) { put(from + 4 * i, a[i]); put(to + 4 * i, b[i]); }
    for (size_t o = 0; o + 16 <= frm.size(); ++o)
        if (!memcmp(&frm[o], from, 16)) { memcpy(&frm[o], to, 16); return true; }
    return false;
}

bool inPlus(const float* px, const float* py, float x, float y) {
    bool in = false;
    for (int i = 0, j = 11; i < 12; j = i++)
        if ((py[i] > y) != (py[j] > y) && x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i]) in = !in;
    return in;
}

void drawPlus(uint32_t* px, int w, int h) {   // 0xAARRGGBB, straight alpha
    const float a = kPlusHalf, t = kPlusThick;
    const float xs[12] = { a,  t,  t, -t, -t, -a, -a, -t, -t,  t,  t,  a};
    const float ys[12] = {-t, -t, -a, -a, -t, -t,  t,  t,  a,  a,  t,  t};
    float ox[12], oy[12];
    for (int i = 0; i < 12; ++i) { ox[i] = kPlusCx + xs[i] - ys[i] * kPlusSlant; oy[i] = kPlusCy + ys[i]; }
    for (int y = 0; y < h; ++y)
        for (int x = (int)(kPlusCx - a - 3); x <= (int)(kPlusCx + a + 3) && x < w; ++x) {
            int cov = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    if (inPlus(ox, oy, x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4)) ++cov;
            if (cov) px[(size_t)y * w + x] = ((uint32_t)(cov * 255 / 16) << 24) | kPlusRgb;
        }
}

// Decode FM8's wordmark PNG, widen it, add the "+", and hand it back as an uncompressed 32-bit TGA,
// which FM8's picture loader reads straight into memory (as on Windows, core/rsrc.cpp).
bool buildPicture(const std::vector<uint8_t>& png, std::vector<uint8_t>& tga) {
    CFDataRef d = CFDataCreate(nullptr, png.data(), (CFIndex)png.size());
    CGImageSourceRef src = CGImageSourceCreateWithData(d, nullptr);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
    bool ok = false;
    if (img) {
        const int w = (int)CGImageGetWidth(img), h = (int)CGImageGetHeight(img), nw = w + kPlusW;
        std::vector<uint32_t> buf((size_t)nw * h, 0);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        // Premultiplied BGRA (the only 32-bit layout CoreGraphics draws into), straightened below.
        CGContextRef cg = CGBitmapContextCreate(buf.data() + kTextDx, (size_t)w, (size_t)h, 8, (size_t)nw * 4, cs,
                                                kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        if (cg) {
            CGContextSetBlendMode(cg, kCGBlendModeCopy);
            CGContextDrawImage(cg, CGRectMake(0, 0, w, h), img);
            CGContextRelease(cg);
            for (auto& p : buf) {
                const uint32_t al = p >> 24;
                if (al && al < 255) {
                    auto un = [al](uint32_t c) { return std::min<uint32_t>(255, (c * 255 + al / 2) / al); };
                    p = (al << 24) | un((p >> 16) & 255) << 16 | un((p >> 8) & 255) << 8 | un(p & 255);
                }
            }
            drawPlus(buf.data(), nw, h);
            tga.assign(18 + buf.size() * 4, 0);
            tga[2] = 2; tga[12] = (uint8_t)nw; tga[13] = (uint8_t)(nw >> 8);
            tga[14] = (uint8_t)h; tga[15] = (uint8_t)(h >> 8); tga[16] = 32; tga[17] = 0x28;
            memcpy(tga.data() + 18, buf.data(), buf.size() * 4);   // BGRA in memory on a little-endian Mac
            ok = true;
        }
        CGColorSpaceRelease(cs);
        CGImageRelease(img);
    }
    if (src) CFRelease(src);
    CFRelease(d);
    return ok;
}

void serve(const char* type, uint32_t id, const std::vector<uint8_t>& blob) {
    auto& outer = *(ResMap*)g_sym[kResourceMap];
    ResInner*& inner = outer[fourcc(type)];
    if (!inner) inner = new ResInner;   // freed by FM8's own clearResourceMap with the nodes
    (*inner)[id] = {blob.data(), blob.size()};
}
} // namespace

void setSingleton(InstanceState* s) { g_singleton = s; }
void setArpBlockCallback(void (*cb)(InstanceState&)) { g_arpBlockCb = cb; }
void setLogoHandler(void (*fn)(InstanceState*)) { g_logoHandler = fn; }

bool installMac(const void* anyAddressInFm8) {
    if (g_installed) return true;
    const void* img = mac::imageOf(anyAddressInFm8);
    if (!img) return false;
    // All or nothing: a build missing any of these is not the 1.4.6 this was read from.
    if (mac::resolve(img, kNames, g_sym, kSymCount) != kSymCount) return false;
    g_arpHooked = mac::redirect(g_sym[kArpDispatch], (void*)&replaceArpDispatch);
    const bool ctl = hookControlEvents();
    if (g_trace) fprintf(stderr, "FM8.plus: installed, arp redirect %d, control hook %d\n", g_arpHooked, ctl);
    g_installed = true;
    return true;
}

bool install(void* base, Host) { return installMac(base); }
void uninstall() {}   // the detours live as long as FM8's image, which is never unloaded under us

bool bindInstance(InstanceState* st, void* root) {
    if (!g_installed || !st || !root) return false;
    // Breadth-first over the pointers in the first 2 KB of each object, three levels deep.
    std::vector<void*> level{root}, next;
    void* fm8 = nullptr;
    for (int depth = 0; depth < 3 && !fm8; ++depth, level.swap(next), next.clear())
        for (void* o : level) {
            void* words[256];
            if (!peek(o, words, sizeof words)) continue;
            for (void* w : words) {
                if ((uintptr_t)w < 0x100000000ull || ((uintptr_t)w & 7)) continue;
                if ((fm8 = asFm8(w))) break;
                if (next.size() < 4096) next.push_back(w);
            }
            if (fm8) break;
        }
    if (g_trace) fprintf(stderr, "FM8.plus: bind %p -> FM8 %p\n", root, fm8);
    if (!fm8) return false;
    st->appObj.store(fm8);
    for (auto& b : g_bind) if (b.st.load() == st) { b.fm8.store(fm8); return true; }
    for (auto& b : g_bind) { InstanceState* e = nullptr; if (b.st.compare_exchange_strong(e, st)) { b.fm8.store(fm8); return true; } }
    return false;
}

void unbindInstance(InstanceState* st) {
    for (auto& b : g_bind) if (b.st.load() == st) { b.fm8.store(nullptr); b.st.store(nullptr); }
}

bool validateBuild(void*, Host) { return g_installed; }

void morphXYFromCc(const InstanceState& st, uint8_t cc1, float& x, float& y) {
    const float r = st.morphRadius.load(std::memory_order_relaxed);
    const float start = st.morphStartDeg.load(std::memory_order_relaxed) * 3.14159265358979f / 180.0f;
    const float theta = start + (cc1 / 127.0f) * 6.28318530717959f;
    x = std::fmin(1.0f, std::fmax(0.0f, 0.5f + r * std::cos(theta)));
    y = std::fmin(1.0f, std::fmax(0.0f, 0.5f + r * std::sin(theta)));
}

bool setMorphXY(InstanceState& st, float x, float y) {
    void* fm8 = st.appObj.load(std::memory_order_relaxed);
    if (!g_installed || !fm8) return false;
    void* eb = ((void* (*)(void*))g_sym[kGetEditBuffer])(fm8);
    if (!eb) return false;
    auto set = (bool (*)(void*, uint32_t, float, bool))g_sym[kSetParameter];
    set(eb, kTagMorphX, x, true);
    set(eb, kTagMorphY, y, true);
    if (g_trace) {   // read back through the EditBuffer's GetParameter (vtable +0x60)
        auto get = (*(float (***)(void*, uint32_t))eb)[0x60 / 8];
        fprintf(stderr, "FM8.plus: morph set %.3f,%.3f -> engine %.3f,%.3f\n", x, y, get(eb, kTagMorphX), get(eb, kTagMorphY));
    }
    return true;
}

void applyPendingMorphInternal(InstanceState& st) {
    if (st.morphCc.load(std::memory_order_relaxed) < 0) return;
    const uint8_t cc = st.morphPending.exchange(0xff, std::memory_order_relaxed);
    if (cc == 0xff) return;
    float x, y; morphXYFromCc(st, cc, x, y);
    setMorphXY(st, x, y);
}

void flushExternal(InstanceState& st) {
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n)
            if (st.extOn.test(ch, n)) { st.pushOut((uint8_t)(0x80 | ch), (uint8_t)n, 0, 0); st.extOn.clear(ch, n); }
}

// The arp features need the two code detours; morph only needs the instance's FM8 object, which a
// bound instance always has, and it filters its CC at the plug-in's MIDI input.
bool midiFeaturesAvailable() { return g_installed && g_arpHooked; }

// Arms this thread's breakpoint; a thread that cannot be armed simply sends no arp MIDI out.
void armAudioThread() { if (g_arpHooked) mac::armThread(); }

bool aboutReady(const InstanceState& st) { return g_installed && st.appObj.load() != nullptr; }

bool showAboutMac(InstanceState* st) {
    void* fm8 = st ? st->appObj.load() : nullptr;
    if (!fm8 || !g_installed) return false;
    // What FormMain::onControlEvent does for the wordmark: the form manager's main window, then
    // that window's UIA base, handed to FM8AboutDialog's doShow.
    void* fm = ((void* (*)(void*))g_sym[kGetFormManager])(fm8);
    if (!fm) return false;
    void* win = (*(void* (**)(void*))(*(char**)fm + 0x130))(fm);
    if (!win) return false;
    void* base = (*(void* (**)(void*))(*(char**)win + 0x18))(win);
    ((void (*)(void*))g_sym[kAboutDoShow])(base);
    return true;
}
bool showAbout(InstanceState& st) { return showAboutMac(&st); }

bool serveLogoMac(const char* rsrcPath) {
    if (!g_installed || !rsrcPath) return false;
    if (g_pic.empty()) {
        FILE* f = fopen(rsrcPath, "rb");
        if (!f) return false;
        std::vector<uint8_t> file;
        fseek(f, 0, SEEK_END); file.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
        const bool read = fread(file.data(), 1, file.size(), f) == file.size();
        fclose(f);
        std::vector<uint8_t> png, pic;
        if (!read || !rsrcGet(file, fourcc("FRM "), 5, g_frm5) || !rsrcGet(file, fourcc("FRM "), 15, g_frm15) ||
            !rsrcGet(file, fourcc("XTGA"), 193, png) || !widenForm(g_frm5) || !widenForm(g_frm15) ||
            !buildPicture(png, pic)) {
            g_frm5.clear(); g_frm15.clear();
            return false;
        }
        g_pic.swap(pic);
    }
    serve("FRM ", 5, g_frm5);
    serve("FRM ", 15, g_frm15);
    serve("XTGA", 193, g_pic);
    g_logoWidened = true;
    return true;
}
bool logoWidened() { return g_logoWidened; }

void setGuiScale(float s) {
    if (!(s >= 1.0f)) s = 1.0f;
    if (s > 4.0f) s = 4.0f;
    g_guiScale.store((float)lroundf(s));
}
float guiScale() { return g_guiScale.load(); }

// Windows-only entry points kept so the shared header links; the Mac paths above replace them.
bool serveLogo(void*) { return false; }
void addScaledWindow(void*) {}
void expectEditorWindow() {}
void* addressOf(const Site&) { return nullptr; }

} // namespace Core
} // namespace fm8plus
