// FM8.plus shared core implementation: the two arp detours and the feature routing.
#include "fm8plus.h"
#include <windows.h>
#include <windowsx.h>
#include <cmath>
#include "MinHook.h"
#include "rsrc.h"

namespace fm8plus {
namespace Core {

thread_local InstanceState* current = nullptr;

namespace {
InstanceState* g_singleton = nullptr;
void (*g_arpBlockCb)(InstanceState&) = nullptr;
bool g_logoWidened = false;   // set by serveLogo: the wordmark carries the "+" and the wider rect
}
void setSingleton(InstanceState* s) { g_singleton = s; }
void setArpBlockCallback(void (*cb)(InstanceState&)) { g_arpBlockCb = cb; }

// Resolved absolute addresses of the two detoured functions and the callees we invoke.
namespace {
void*  g_base       = nullptr;
Bin    g_bin        = Bin::Vst2_146;
bool   g_installed  = false;

// Original (trampoline) pointers filled by MinHook.
//
// On x86 all three of these are __thiscall: `this` arrives in ECX and the rest go on the stack,
// callee cleaned. MSVC will not put __thiscall on a free function, so declare __fastcall with an
// unused EDX slot, which lays the arguments out identically and cleans the same number of bytes.
// Confirmed against the disassembly: the arp dispatch ends RET 0x8 for its two stack arguments and
// the morph setter RET 0xc for its three.
#if defined(_M_IX86)
  #define FM8_EDX void* edx_unused_,
  #define FM8_EDX_PASS nullptr,
using ArpRunDispatchFn = void (__fastcall*)(void* core, void*, uint32_t destSel, int inBlockPos);
using MidiHandlerFn    = void (__fastcall*)(void* fm8midi, void*, void* ev, int flag);
#else
  #define FM8_EDX
  #define FM8_EDX_PASS
using ArpRunDispatchFn = void   (*)(void* core, uint32_t destSel, int inBlockPos);
using MidiHandlerFn    = void   (*)(void* fm8midi, void* ev, int flag);
#endif
ArpRunDispatchFn o_arpRun  = nullptr;
MidiHandlerFn    o_midiHnd = nullptr;

// thread_local marker: are we currently inside the arp dispatch loop? If so, events reaching the
// MIDI handler are arp-generated, not live input.
thread_local bool  tl_inArp   = false;
thread_local int32_t tl_arpPos = 0;

inline uint32_t rvaFor(const Site& s) { return rva(s, g_bin); }

// Null, not base+0, when a site is absent from this binary. 1.4.1 has no VST3 and no HiDPI layer,
// so several sites are legitimately 0, and hooking "base + 0" would detour the DOS header.
inline void* addr(const Site& s) {
    const uint32_t r = rvaFor(s);
    return r ? (void*)((uint8_t*)g_base + r) : nullptr;
}
inline bool has(const Site& s) { return rvaFor(s) != 0; }
inline const Layout& lay() { return kLayout[(int)g_bin]; }

// False when this build's MidiEvent/EditBuffer offsets are not confirmed. The arp and morph
// detours walk those pointers on the audio thread, so they stay out rather than guess.
inline bool midiOk() { return lay().midiVerified && lay().vstObjEditBuf != 0; }

// Decide routing for one arp-generated event; returns true if FM8 should still play it internally.
// Updates the note masks and enqueues to the instance out-buffer as the mode requires.
bool routeArpEvent(InstanceState& st, uint8_t status, uint8_t d1, uint8_t d2, int32_t off) {
    const uint8_t type = status & 0xf0;
    const int ch = status & 0x0f;
    const bool isOn  = (type == 0x90) && (d2 > 0);
    const bool isOff = (type == 0x80) || ((type == 0x90) && (d2 == 0));
    const ArpMode mode = (ArpMode)st.arpMode.load(std::memory_order_relaxed);

    // External (MIDI out) side.
    if (mode != ArpMode::Internal) {
        if (isOn) { st.pushOut(status, d1, d2, off); st.extOn.set(ch, d1); }
        else if (isOff) { if (st.extOn.test(ch, d1)) { st.pushOut(status, d1, d2, off); st.extOn.clear(ch, d1); } }
        else st.pushOut(status, d1, d2, off);  // non-note arp event, pass through unchanged
    }

    // Internal (voice) side.
    bool playInternal;
    if (mode == ArpMode::MidiOnly) {
        if (isOn) playInternal = false;                 // suppress new internal voices
        else if (isOff) playInternal = st.intOn.test(ch, d1); // let offs through for voices already on
        else playInternal = true;
    } else {
        playInternal = true;                             // Internal and Clone both play
    }
    if (isOn && playInternal) st.intOn.set(ch, d1);
    if (isOff && playInternal) st.intOn.clear(ch, d1);
    return playInternal;
}

// Internal morph setter type: setParameterByTag(EditBuffer* this, uint tag, float value, char thread).
#if defined(_M_IX86)
using SetByTagFn = intptr_t (__fastcall*)(void* editBuf, void*, uint32_t tag, float value, char thread);
#else
using SetByTagFn = intptr_t (*)(void* editBuf, uint32_t tag, float value, char thread);
#endif

// Detour of ArpRunDispatch (0x1800e7250 family): mark the arp window so the MIDI-handler detour
// can tell arp events from live input, forward the in-block position, and capture the EditBuffer
// pointer (param_1 == EditBuffer; +0x29e8 -> arp) for the internal morph setter.
// Walk core -> VstObject (*(core+8)) -> EditBuffer (*(VstObject+0x55d0)), matching what the
// dispatch does internally (FUN_1800ef7f0 returns *(x+0x55d0)). SEH-guarded: a bad chain yields null.
// Both pointers hang off the same object, so one guarded walk fetches them together. `outer` is the
// FM8VstObject: the arp dispatch and FormMain's command sink hand it to the same accessor, which is
// what ties the About argument (*(outer + 0x5620)) to the instance we are processing.
void coreObjects(void* core, void*& editBuf, void*& app) {
    editBuf = app = nullptr;
    __try {
        void* outer = *(void**)((uint8_t*)core + lay().coreVstObj);
        if (!outer) return;
        editBuf = *(void**)((uint8_t*)outer + lay().vstObjEditBuf);
        if (lay().vstObjApp) app = *(void**)((uint8_t*)outer + lay().vstObjApp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { editBuf = app = nullptr; }
}

void __fastcall detourArpRun(void* core, FM8_EDX uint32_t destSel, int inBlockPos) {
    InstanceState* st = current ? current : g_singleton;
    if (!st) { o_arpRun(core, FM8_EDX_PASS destSel, inBlockPos); return; }
    if (st->pendingFlush.exchange(false, std::memory_order_relaxed)) flushExternal(*st);  // audio-thread flush
    void *eb, *app;
    coreObjects(core, eb, app);
    if (eb) st->editBuf.store(eb, std::memory_order_relaxed);
    if (app) st->appObj.store(app, std::memory_order_relaxed);
    const bool prev = tl_inArp; const int32_t prevPos = tl_arpPos;
    tl_inArp = true; tl_arpPos = inBlockPos;
    o_arpRun(core, FM8_EDX_PASS destSel, inBlockPos);   // runs the engine, dispatching through the MIDI handler
    tl_inArp = prev; tl_arpPos = prevPos;
    if (g_arpBlockCb) g_arpBlockCb(*st);   // standalone: flush to WinMM + apply morph
}

// Detour of MidiEventHandler (0x1800e6660 family). Arp events (tl_inArp) are routed by mode: cloned
// or suppressed for MIDI out, and only played internally as the mask logic allows. Live input passes
// straight through, except the morph CC, which is captured and swallowed.
void __fastcall detourMidiHandler(void* fm8midi, FM8_EDX void* ev, int flag) {
    InstanceState* st = current ? current : g_singleton;
    if (!st) { o_midiHnd(fm8midi, FM8_EDX_PASS ev, flag); return; }

    const auto* b = (const uint8_t*)ev + lay().elemWord;
    const uint8_t status = lay().statusFirst ? b[0] : b[2];
    const uint8_t d1 = b[1] & 0x7f;
    const uint8_t d2 = (lay().statusFirst ? b[2] : b[0]) & 0x7f;

    if (tl_inArp && st->arpMode.load(std::memory_order_relaxed) != (uint8_t)ArpMode::Internal) {
        if (routeArpEvent(*st, status, d1, d2, tl_arpPos))
            o_midiHnd(fm8midi, FM8_EDX_PASS ev, flag);
        return;
    }
    // Live input: if this is the selected morph CC, capture its value and swallow the event, so the
    // CC drives only the morph and FM8 never sees it (no mod-wheel movement, no MIDI-learn). The
    // caller owns the event, this handler only consumes it, so returning early strands nothing.
    const int16_t mc = st->morphCc.load(std::memory_order_relaxed);
    if (mc >= 0 && (status & 0xf0) == 0xb0 && d1 == (uint8_t)mc) {
        st->morphPending.store(d2, std::memory_order_relaxed);
        return;
    }
    o_midiHnd(fm8midi, FM8_EDX_PASS ev, flag);
}

// ---- GUI scale -------------------------------------------------------------
// FM8 carries a complete HiDPI layer it never switches on: NI::UIA sizes every window it creates by
// a per-window scale, divides incoming mouse coordinates by it, multiplies the dirty rects it sends
// to InvalidateRect, and stretches the final DIB blit to match. The scale is GetDpiForWindow/96, so
// it is always 1 (FM8 never calls SetProcessDpiAwareness), and one byte in the NI::UIA app object
// gates the lot off anyway. These three detours supply our own number instead.
std::atomic<float> g_guiScale{1.0f};
bool g_guiHooked = false;

using AppObjFn   = void* (*)();
using DpiScaleFn = float (*)(void*);
AppObjFn   o_appObj    = nullptr;
DpiScaleFn o_dpiScale  = nullptr;
DpiScaleFn o_surfScale = nullptr;

// Every site that reads the HiDPI gate calls this getter immediately before it, so setting the byte
// here turns the path on wherever it is used, as soon as the object exists. No startup ordering.
void* detourAppObject() {
    void* o = o_appObj();
    if (o && g_guiScale.load(std::memory_order_relaxed) != 1.0f)
        *((uint8_t*)o + kUiaHiDpiFlag) = 1;
    return o;
}

// Editor windows belonging to FM8+ instances (see addScaledWindow). Dead entries are recycled, so
// no teardown bookkeeping is needed. UI thread only.
HWND g_scaled[32] = {};
bool g_gateWindows = false;

bool ownsWindow(HWND h) {
    if (!g_gateWindows) return true;                  // standalone: the whole process is ours
    for (; h; h = GetParent(h))
        for (HWND w : g_scaled) if (w == h) return true;
    return false;
}

// The scale itself. Deliberately ignores the window's real DPI: at 1x FM8 must look exactly as it
// does today, including on a HiDPI monitor where its artwork has always been rendered 1:1.
float scaleFor(HWND h) {
    const float s = g_guiScale.load(std::memory_order_relaxed);
    return (s == 1.0f || ownsWindow(h)) ? s : 1.0f;
}
float detourDpiScale(void* hwnd) { return scaleFor((HWND)hwnd); }

// Stock this returns ceil(scale): NI::UIA renders the DIB at an integer supersample and lets the
// blit fit it to the exact window. FM8 has no high-resolution artwork to supersample from, so hold
// it at 1, keeping the surface at logical size and leaving all the scaling to the StretchDIBits.
float detourSurfScale(void*) { return 1.0f; }

// That StretchDIBits is preceded by FM8's only SetStretchBltMode call, which asks for HALFTONE:
// GDI then interpolates, and FM8's artwork comes out soft and smeared when it is enlarged. The scale
// steps are whole numbers, so COLORONCOLOR replicates each pixel exactly and the GUI stays crisp.
// Swapping the mode in FM8's own GDI32 import is enough; the one call site is that blit.
using SetStretchBltModeFn = int (WINAPI*)(HDC, int);
SetStretchBltModeFn o_setStretchMode = nullptr;
int WINAPI detourSetStretchBltMode(HDC dc, int mode) {
    if (mode == HALFTONE) mode = COLORONCOLOR;
    return o_setStretchMode(dc, mode);
}

// ---- GUI scale on 1.4.1 ----------------------------------------------------
// The 2015 build has no HiDPI layer to wake: GetDpiForWindow and StretchDIBits appear nowhere in
// it, so there is no scaled blit to switch on and no gate byte to flip. Rather than reimplement
// NI::UIA's seven scale sites in each of three binaries, intercept the Win32 calls that layer
// would have adjusted. FM8 goes on thinking in logical pixels and never learns its window is
// bigger: it asks how large it is and hears the logical size, it invalidates a logical rect and we
// enlarge it, it blits 1:1 and we stretch. One implementation, both architectures, no NI internals.
//
// ponytail: only the calls FM8 actually makes are patched, checked against its import table. It
// imports no MoveWindow and no StretchDIBits, so neither is here.
using CreateWindowExWFn  = HWND (WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using SetWindowPosFn     = BOOL (WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using GetRectFn          = BOOL (WINAPI*)(HWND, LPRECT);
using InvalidateRectFn   = BOOL (WINAPI*)(HWND, const RECT*, BOOL);
using MapPointFn         = BOOL (WINAPI*)(HWND, LPPOINT);
using SetDIBitsToDeviceFn= int  (WINAPI*)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const void*, const BITMAPINFO*, UINT);

CreateWindowExWFn   o_createWindowExW = nullptr;
SetWindowPosFn      o_setWindowPos    = nullptr;
GetRectFn           o_getClientRect   = nullptr;
GetRectFn           o_getWindowRect   = nullptr;
InvalidateRectFn    o_invalidateRect  = nullptr;
MapPointFn          o_screenToClient  = nullptr;
MapPointFn          o_clientToScreen  = nullptr;
SetDIBitsToDeviceFn o_setDIBits       = nullptr;

// Armed by the shim just before it hands FM8 the host's window, so the editor FM8 is about to
// create is recognised at creation time, when it has no HWND to test yet. Consumed once.
thread_local bool tl_expectEditor = false;

inline int up(int v, float s)   { return (int)lroundf((float)v * s); }
inline int down(int v, float s) { return (int)lroundf((float)v / s); }

float scaleForWindow(HWND h) {
    const float s = g_guiScale.load(std::memory_order_relaxed);
    return (s != 1.0f && h && ownsWindow(h)) ? s : 1.0f;
}

HWND WINAPI detourCreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y,
                                  int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID p) {
    const float s = g_guiScale.load(std::memory_order_relaxed);
    const bool ours = s != 1.0f && (tl_expectEditor || (parent && ownsWindow(parent)));
    if (ours && w > 0 && h > 0) { w = up(w, s); h = up(h, s); }
    HWND r = o_createWindowExW(ex, cls, name, style, x, y, w, h, parent, menu, inst, p);
    if (ours && r) { tl_expectEditor = false; addScaledWindow(r); }
    return r;
}

// FM8 sizes in logical pixels, so enlarge. Moves (SWP_NOSIZE) pass through: the position is the
// host's, in the host's coordinates, and is not ours to touch.
BOOL WINAPI detourSetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f) {
    const float s = scaleForWindow(h);
    if (s != 1.0f && !(f & SWP_NOSIZE)) { cx = up(cx, s); cy = up(cy, s); }
    return o_setWindowPos(h, after, x, y, cx, cy, f);
}

// The other direction: FM8 asks how big it is and must hear the logical size, or it relays out to
// the physical one and the scaling cancels itself.
BOOL WINAPI detourGetClientRect(HWND h, LPRECT r) {
    BOOL ok = o_getClientRect(h, r);
    const float s = scaleForWindow(h);
    if (ok && s != 1.0f && r) { r->right = r->left + down(r->right - r->left, s);
                                r->bottom = r->top + down(r->bottom - r->top, s); }
    return ok;
}
BOOL WINAPI detourGetWindowRect(HWND h, LPRECT r) {
    BOOL ok = o_getWindowRect(h, r);
    const float s = scaleForWindow(h);
    if (ok && s != 1.0f && r) { r->right = r->left + down(r->right - r->left, s);
                                r->bottom = r->top + down(r->bottom - r->top, s); }
    return ok;
}

// A logical dirty rect covers scale-times as many physical pixels. Round outwards, since a rect
// that is one pixel short leaves a seam along the edge of every repaint.
BOOL WINAPI detourInvalidateRect(HWND h, const RECT* r, BOOL erase) {
    const float s = scaleForWindow(h);
    if (s == 1.0f || !r) return o_invalidateRect(h, r, erase);
    RECT p{ (LONG)floorf(r->left * s), (LONG)floorf(r->top * s),
            (LONG)ceilf(r->right * s), (LONG)ceilf(r->bottom * s) };
    return o_invalidateRect(h, &p, erase);
}

// And back: Windows hands the paint rect in physical pixels, and FM8 redraws the logical rect it reads
// there. Unconverted, a partial repaint the host asks for (a window being resized, say) redraws the
// wrong region at twice the size and leaves the one asked for blank. Round outwards; the blit is
// clipped to the real update region anyway.
using BeginPaintFn = HDC (WINAPI*)(HWND, LPPAINTSTRUCT);
BeginPaintFn o_beginPaint = nullptr;
HDC WINAPI detourBeginPaint(HWND h, LPPAINTSTRUCT ps) {
    HDC dc = o_beginPaint(h, ps);
    const float s = scaleForWindow(h);
    if (dc && ps && s != 1.0f) {
        RECT& r = ps->rcPaint;
        r = {(LONG)floorf(r.left / s), (LONG)floorf(r.top / s), (LONG)ceilf(r.right / s), (LONG)ceilf(r.bottom / s)};
    }
    return dc;
}

BOOL WINAPI detourScreenToClient(HWND h, LPPOINT pt) {
    BOOL ok = o_screenToClient(h, pt);
    const float s = scaleForWindow(h);
    if (ok && s != 1.0f && pt) { pt->x = down(pt->x, s); pt->y = down(pt->y, s); }
    return ok;
}
BOOL WINAPI detourClientToScreen(HWND h, LPPOINT pt) {
    const float s = scaleForWindow(h);
    POINT q = pt ? *pt : POINT{};
    if (s != 1.0f && pt) { q.x = up(q.x, s); q.y = up(q.y, s); }
    BOOL ok = o_clientToScreen(h, &q);
    if (pt) *pt = q;
    return ok;
}

// The blit. FM8 hands over a logical dest rect and a source rect into its top-down DIB; stretching
// the dest while leaving the source alone is exactly what 1.4.6 does once its surface scale is 1.
// COLORONCOLOR, not the HALFTONE 1.4.6 asks for: the steps are whole numbers, so replicating
// pixels keeps the artwork crisp where interpolation would smear it.
int WINAPI detourSetDIBitsToDevice(HDC hdc, int xD, int yD, DWORD w, DWORD h, int xS, int yS,
                                   UINT startScan, UINT cLines, const void* bits,
                                   const BITMAPINFO* bmi, UINT usage) {
    const float s = scaleForWindow(WindowFromDC(hdc));
    if (s == 1.0f)
        return o_setDIBits(hdc, xD, yD, w, h, xS, yS, startScan, cLines, bits, bmi, usage);
    SetStretchBltMode(hdc, COLORONCOLOR);
    return StretchDIBits(hdc, up(xD, s), up(yD, s), up((int)w, s), up((int)h, s),
                         xS, yS, (int)w, (int)h, bits, bmi, usage, SRCCOPY);
}

// ponytail: best effort. A failure here costs the scale menu, not the arp and morph features.
void installGuiScale() {
    if (is141(g_bin)) {
        // No UIA HiDPI layer in 2015: scale at the Win32 boundary instead.
        auto imp = [](const char* dll, const char* fn, void* det, void** orig) {
            void* o = Rsrc::patchImport((HMODULE)g_base, dll, fn, det);
            if (o) *orig = o;
            return o != nullptr;
        };
        g_guiHooked =
            imp("USER32.dll", "CreateWindowExW",   (void*)&detourCreateWindowExW, (void**)&o_createWindowExW)
          & imp("USER32.dll", "SetWindowPos",      (void*)&detourSetWindowPos,    (void**)&o_setWindowPos)
          & imp("USER32.dll", "GetClientRect",     (void*)&detourGetClientRect,   (void**)&o_getClientRect)
          & imp("USER32.dll", "GetWindowRect",     (void*)&detourGetWindowRect,   (void**)&o_getWindowRect)
          & imp("USER32.dll", "InvalidateRect",    (void*)&detourInvalidateRect,  (void**)&o_invalidateRect)
          & imp("USER32.dll", "BeginPaint",        (void*)&detourBeginPaint,      (void**)&o_beginPaint)
          & imp("USER32.dll", "ScreenToClient",    (void*)&detourScreenToClient,  (void**)&o_screenToClient)
          & imp("USER32.dll", "ClientToScreen",    (void*)&detourClientToScreen,  (void**)&o_clientToScreen)
          & imp("GDI32.dll",  "SetDIBitsToDevice", (void*)&detourSetDIBitsToDevice, (void**)&o_setDIBits);
        return;
    }
    auto mk = [](const Site& site, void* det, void** orig) {
        void* t = addr(site);
        return t && MH_CreateHook(t, det, orig) == MH_OK && MH_EnableHook(t) == MH_OK;
    };
    g_guiHooked = mk(kUiaAppObject, (void*)&detourAppObject,  (void**)&o_appObj)
               && mk(kUiaDpiScale,  (void*)&detourDpiScale,   (void**)&o_dpiScale)
               && mk(kUiaSurfScale, (void*)&detourSurfScale,  (void**)&o_surfScale);
    o_setStretchMode = (SetStretchBltModeFn)Rsrc::patchImport(
        (HMODULE)g_base, "GDI32.dll", "SetStretchBltMode", (void*)&detourSetStretchBltMode);
}
} // namespace

// Mouse messages arrive in physical pixels; FM8's own hit testing works in logical ones. The
// subclass in ui.cpp calls this before passing a message down, so every control lands where it
// looks. Returns false when the message carries no coordinates or the window is not scaled.
bool scaleMouseParam(void* hwnd, unsigned msg, intptr_t& lp) {
    if (!is141(g_bin)) return false;        // 1.4.6's UIA maps the coordinates itself
    if (msg < WM_MOUSEFIRST || msg > WM_MOUSELAST || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
        return false;                       // wheel carries screen coordinates, not client ones
    const float s = scaleForWindow((HWND)hwnd);
    if (s == 1.0f) return false;
    const int x = down(GET_X_LPARAM(lp), s), y = down(GET_Y_LPARAM(lp), s);
    lp = (intptr_t)MAKELPARAM((WORD)(SHORT)x, (WORD)(SHORT)y);
    return true;
}

// Armed by the VST2 shim before FM8 builds its editor, so the new window is scaled from birth.
void expectEditorWindow() { tl_expectEditor = true; }

void setGuiScale(float s) {
    if (!(s >= 1.0f)) s = 1.0f;          // also catches NaN
    if (s > 4.0f) s = 4.0f;
    s = (float)lroundf(s);               // whole numbers only: a fractional scale cannot blit pixel-exact
    g_guiScale.store(g_guiHooked ? s : 1.0f, std::memory_order_relaxed);
}
float guiScale() { return g_guiScale.load(std::memory_order_relaxed); }

void addScaledWindow(void* hwnd) {
    auto h = (HWND)hwnd;
    if (!h) return;
    g_gateWindows = true;
    for (HWND w : g_scaled) if (w == h) return;
    for (HWND& w : g_scaled) if (!w || !IsWindow(w)) { w = h; return; }
}

// Read the PE timestamp of a loaded module. 0 for anything that is not a PE we can walk.
static uint32_t moduleStamp(void* base) {
    if (!base) return 0;
    __try {
        auto* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return nt->FileHeader.TimeDateStamp;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool validateBuild(void* base, Host h) {
    Bin b;
    return resolveBin(h, moduleStamp(base), b);
}

bool install(void* base, Host host) {
    if (g_installed) return true;
    Bin which;
    if (!base || !resolveBin(host, moduleStamp(base), which)) return false;  // unmapped FM8 -> stay stock
    g_base = base; g_bin = which;

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    void* pArpRun = addr(kArpRunDispatch);
    void* pMidi   = addr(kMidiEventHandler);
    if (midiOk() && pArpRun && pMidi) {
        if (MH_CreateHook(pArpRun, (void*)&detourArpRun, (void**)&o_arpRun) != MH_OK) return false;
        if (MH_CreateHook(pMidi,   (void*)&detourMidiHandler, (void**)&o_midiHnd) != MH_OK) return false;
        if (MH_EnableHook(pArpRun) != MH_OK) return false;
        if (MH_EnableHook(pMidi)   != MH_OK) return false;
    }
    installGuiScale();

    g_installed = true;
    return true;
}

void uninstall() {
    if (!g_installed) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_installed = false;
}

void morphXYFromCc(const InstanceState& st, uint8_t cc1, float& x, float& y) {
    const float r = st.morphRadius.load(std::memory_order_relaxed);
    const float start = st.morphStartDeg.load(std::memory_order_relaxed) * 3.14159265358979f / 180.0f;
    const float theta = start + (cc1 / 127.0f) * 6.28318530717959f;
    x = 0.5f + r * std::cos(theta);
    y = 0.5f + r * std::sin(theta);
    if (x < 0) x = 0; else if (x > 1) x = 1;
    if (y < 0) y = 0; else if (y > 1) y = 1;
}

void flushExternal(InstanceState& st) {
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n)
            if (st.extOn.test(ch, n)) { st.pushOut((uint8_t)(0x80 | ch), (uint8_t)n, 0, 0); st.extOn.clear(ch, n); }
}

void* addressOf(const Site& s) { return g_installed ? addr(s) : nullptr; }

bool midiFeaturesAvailable() { return g_installed && midiOk(); }

bool aboutReady(const InstanceState& st) {
    return g_installed && st.appObj.load(std::memory_order_relaxed) != nullptr;
}

bool showAbout(InstanceState& st) {
    void* app = st.appObj.load(std::memory_order_relaxed);
    if (!app || !g_installed) return false;
    auto show = (void (*)(void*))addr(kShowAboutDialog);
    __try {
        show(app);   // modal: returns when the user closes FM8's About panel
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st.appObj.store(nullptr, std::memory_order_relaxed);   // stale pointer, stop offering it
        return false;
    }
}

bool serveLogo(void* module) {
    if (!Rsrc::install((HMODULE)module) || !Rsrc::serveLogo()) return false;
    g_logoWidened = true;
    return true;
}

bool logoWidened() { return g_logoWidened; }

bool setMorphXY(InstanceState& st, float x, float y) {
    void* eb = st.editBuf.load(std::memory_order_relaxed);
    if (!eb || !g_installed) return false;
    auto set = (SetByTagFn)addr(kSetParameterByTag);
    __try {
        set(eb, FM8_EDX_PASS kTagMorphX, x, 1);
        set(eb, FM8_EDX_PASS kTagMorphY, y, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st.editBuf.store(nullptr, std::memory_order_relaxed);  // bad pointer, stop trying until re-captured
        return false;
    }
}

void applyPendingMorphInternal(InstanceState& st) {
    if (st.morphCc.load(std::memory_order_relaxed) < 0) return;
    uint8_t cc = st.morphPending.exchange(0xff, std::memory_order_relaxed);
    if (cc == 0xff) return;
    float x, y; morphXYFromCc(st, cc, x, y);
    setMorphXY(st, x, y);
}

} // namespace Core
} // namespace fm8plus
