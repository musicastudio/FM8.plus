#include "ui.h"
#include "fm8plus.h"
#include "settings.h"
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <cmath>

namespace fm8plus::ui {
namespace {

// The wordmark rect in FM8's own form coordinates. Stock FM8 is 95x23 at (21,35); serveLogo moves
// it kShift px left and grows it kPlusW px to the right to hold the "+". These three numbers must
// match src/core/rsrc.cpp, which builds the widened form and bitmap FM8 draws from.
constexpr int kLogoX1 = 21, kLogoY1 = 35, kLogoX2 = 116, kLogoY2 = 58;
constexpr int kShift = 11, kPlusW = 22;

constexpr UINT_PTR kSubclassId = 1;

// Command id ranges (kept apart so one TrackPopupMenu return value tells us which control fired).
enum {
    ID_MORPH_OFF = 1000, ID_MORPH_CC0 = 1001,          // ID_MORPH_CC0 + n  for CC n (0..127)
    ID_ARP_INT = 2000, ID_ARP_CLONE, ID_ARP_MIDI,
    ID_TEMPO_OFF = 3000,                                // ID_TEMPO_OFF + mode (0..5)
    ID_GAIN_OFF = 4000,                                 // ID_GAIN_OFF + db (0..10)
    ID_SCALE_0 = 5000,                                  // ID_SCALE_0 + index into kScales
    ID_ABOUT_FM8 = 6000, ID_ABOUT_PLUS,
};

// Where "About FM8.plus" sends the browser.
const wchar_t* const kProjectUrl = L"https://github.com/musicastudio/FM8.plus";

// GUI Scale steps. 1x is stock FM8 down to the pixel; the rest are whole numbers so the blit
// replicates each pixel exactly instead of interpolating (see Core::setGuiScale).
constexpr float kScales[] = {1.0f, 2.0f, 3.0f, 4.0f};
const wchar_t* const kScaleLabels[] = {L"1x (off)", L"2x", L"3x", L"4x"};
constexpr int kScaleCount = (int)(sizeof kScales / sizeof kScales[0]);

// Per-window data behind the subclass: the instance state, the window FM8 draws the form into
// (clicks from any nested child are mapped into its client area), and the host resize hook.
struct OData {
    InstanceState* st;
    HWND root;
    bool topLevel;                 // standalone: root is FM8's own top-level window
    LogoMenu::HostResizeFn resize;
    void* resizeCtx;
};

// The clickable wordmark in physical client pixels of `root`. GUI Scale stretches FM8's whole GUI
// about the client origin, so every form coordinate just multiplies; FM8's own window procedure
// divides the mouse back down the same way before it hit-tests its controls.
RECT logoRect() {
    const float s = Core::guiScale();
    const bool wide = Core::logoWidened();
    const int x1 = wide ? kLogoX1 - kShift : kLogoX1;
    const int x2 = wide ? kLogoX2 + kPlusW - kShift : kLogoX2;
    auto sc = [s](int v) { return (LONG)lroundf(v * s); };
    return {sc(x1), sc(kLogoY1), sc(x2), sc(kLogoY2)};
}

bool onLogo(HWND hwnd, OData* d, LPARAM lp) {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (hwnd != d->root) { ClientToScreen(hwnd, &p); ScreenToClient(d->root, &p); }
    RECT r = logoRect();
    return PtInRect(&r, p) != FALSE;
}

// Common MIDI CC names; unnamed controllers show just "CC n".
const wchar_t* ccName(int cc) {
    switch (cc) {
        case 0: return L"Bank Select"; case 1: return L"Mod Wheel"; case 2: return L"Breath";
        case 4: return L"Foot"; case 5: return L"Portamento Time"; case 6: return L"Data Entry";
        case 7: return L"Volume"; case 8: return L"Balance"; case 10: return L"Pan";
        case 11: return L"Expression"; case 64: return L"Sustain"; case 65: return L"Portamento";
        case 66: return L"Sostenuto"; case 67: return L"Soft Pedal"; case 71: return L"Resonance";
        case 74: return L"Cutoff"; case 84: return L"Portamento Ctrl"; case 91: return L"Reverb";
        case 93: return L"Chorus"; case 94: return L"Detune"; case 95: return L"Phaser";
        case 120: return L"All Sound Off"; case 121: return L"Reset Controllers"; case 123: return L"All Notes Off";
        default: return nullptr;
    }
}

// Multiply one window's client area by `ratio`, keeping its top-left. A top-level window needs the
// frame added back; a child's client area is its whole window.
void rescaleClient(HWND w, float ratio) {
    RECT c; GetClientRect(w, &c);
    int nw = (int)lroundf(c.right * ratio), nh = (int)lroundf(c.bottom * ratio);
    if (nw <= 0 || nh <= 0) return;
    const LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    if (!(style & WS_CHILD)) {
        RECT r{0, 0, nw, nh};
        AdjustWindowRectEx(&r, (DWORD)style, GetMenu(w) != nullptr, (DWORD)GetWindowLongPtrW(w, GWL_EXSTYLE));
        nw = r.right - r.left; nh = r.bottom - r.top;
    }
    SetWindowPos(w, nullptr, 0, 0, nw, nh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// GUI Scale change. FM8 itself does the work from here (it sizes every window it creates by the
// scale, maps the mouse back and stretches the blit); all we have to do is resize the window that
// already exists, since nothing re-creates it.
void applyScale(OData* d, float want) {
    const float old = Core::guiScale();
    Core::setGuiScale(want);
    settings::setGuiScale(Core::guiScale());   // persist what the core accepted
    const float ratio = Core::guiScale() / old;    // re-read: the core clamps to 1..4
    if (ratio == 1.0f) return;

    RECT c; GetClientRect(d->root, &c);
    const int nw = (int)lroundf(c.right * ratio), nh = (int)lroundf(c.bottom * ratio);
    rescaleClient(d->root, ratio);
    // Hosted: FM8's editor child fills the window the host gave us, so the host has to make room.
    // A host that ignores the request picks the size up the next time the editor is opened, since
    // the shim answers the editor rect at the current scale.
    if (!d->topLevel && d->resize) d->resize(d->resizeCtx, nw, nh);
}

void showMenu(HWND hwnd, OData* d) {
    InstanceState* st = d->st;
    HMENU m = CreatePopupMenu();

    // (0) A greyed title, so the menu says whose it is when it opens over FM8's own GUI.
    AppendMenuW(m, MF_STRING | MF_GRAYED | MF_DISABLED, 0, L"FM8.plus");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    // (1) Morph Rotate Control: Off, then CC 0..127 (named where known). The current CC is checked.
    const int16_t curCc = st->morphCc.load();
    HMENU morph = CreatePopupMenu();
    AppendMenuW(morph, MF_STRING | (curCc < 0 ? MF_CHECKED : 0), ID_MORPH_OFF, L"Off");
    AppendMenuW(morph, MF_SEPARATOR, 0, nullptr);
    for (int cc = 0; cc < 128; ++cc) {
        wchar_t label[48]; const wchar_t* nm = ccName(cc);
        if (nm) swprintf(label, 48, L"CC %d (%s)", cc, nm); else swprintf(label, 48, L"CC %d", cc);
        AppendMenuW(morph, MF_STRING | (curCc == cc ? MF_CHECKED : 0), ID_MORPH_CC0 + cc, label);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)morph, L"Morph Rotate Control");

    // (2) Arpeggiator MIDI out.
    HMENU arp = CreatePopupMenu();
    auto mode = (ArpMode)st->arpMode.load();
    AppendMenuW(arp, MF_STRING | (mode == ArpMode::Internal    ? MF_CHECKED : 0), ID_ARP_INT,   L"Internal");
    AppendMenuW(arp, MF_STRING | (mode == ArpMode::CloneToMidi ? MF_CHECKED : 0), ID_ARP_CLONE, L"Clone to MIDI");
    AppendMenuW(arp, MF_STRING | (mode == ArpMode::MidiOnly    ? MF_CHECKED : 0), ID_ARP_MIDI,  L"MIDI only (FM8 silent)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)arp, L"Arpeggiator MIDI out");

    // (3) Tempo Override.
    const uint8_t tm = st->tempoMode.load();
    const wchar_t* tempoLabels[] = {L"Off", L"0.25x Host", L"0.5x Host", L"2x Host", L"4x Host", L"Custom"};
    HMENU tempo = CreatePopupMenu();
    for (int i = 0; i < 6; ++i)
        AppendMenuW(tempo, MF_STRING | (tm == i ? MF_CHECKED : 0), ID_TEMPO_OFF + i, tempoLabels[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tempo, L"Tempo Override");

    // (4) Increase Gain.
    const int8_t db = st->gainDb.load();
    HMENU gain = CreatePopupMenu();
    AppendMenuW(gain, MF_STRING | (db == 0 ? MF_CHECKED : 0), ID_GAIN_OFF, L"Off");
    for (int n = 1; n <= 10; ++n) {
        wchar_t label[16]; swprintf(label, 16, L"+%d dB", n);
        AppendMenuW(gain, MF_STRING | (db == n ? MF_CHECKED : 0), ID_GAIN_OFF + n, label);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)gain, L"Increase Gain");

    // (5) GUI Scale.
    const float gs = Core::guiScale();
    HMENU scale = CreatePopupMenu();
    for (int i = 0; i < kScaleCount; ++i)
        AppendMenuW(scale, MF_STRING | (std::fabs(gs - kScales[i]) < 0.01f ? MF_CHECKED : 0),
                    ID_SCALE_0 + i, kScaleLabels[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)scale, L"GUI Scale");

    // (6) The two About items. Clicking the logo is how stock FM8 opens its About panel, and the
    // wordmark is now our button, so the panel keeps its place here. It is greyed until the audio
    // thread has handed us FM8's own pointer for it.
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (Core::aboutReady(*st) ? 0 : MF_GRAYED), ID_ABOUT_FM8, L"About FM8");
    AppendMenuW(m, MF_STRING, ID_ABOUT_PLUS, L"About FM8.plus");

    SetForegroundWindow(GetAncestor(hwnd, GA_ROOT));   // so the popup dismisses on a click elsewhere
    POINT pt; GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, nullptr);

    if (cmd == ID_MORPH_OFF) { st->morphCc.store(-1); settings::setMorphCcDefault(-1); }
    else if (cmd >= ID_MORPH_CC0 && cmd < ID_MORPH_CC0 + 128) {
        int cc = cmd - ID_MORPH_CC0; st->morphCc.store((int16_t)cc); settings::setMorphCcDefault(cc);
    }
    // Arp mode changes ask the audio thread to flush stranded external notes.
    else if (cmd == ID_ARP_INT)   { st->arpMode.store((uint8_t)ArpMode::Internal);    st->pendingFlush.store(true); settings::setArpModeDefault(0); }
    else if (cmd == ID_ARP_CLONE) { st->arpMode.store((uint8_t)ArpMode::CloneToMidi); st->pendingFlush.store(true); settings::setArpModeDefault(1); }
    else if (cmd == ID_ARP_MIDI)  { st->arpMode.store((uint8_t)ArpMode::MidiOnly);    st->pendingFlush.store(true); settings::setArpModeDefault(2); }
    else if (cmd >= ID_TEMPO_OFF && cmd <= ID_TEMPO_OFF + 5) st->tempoMode.store((uint8_t)(cmd - ID_TEMPO_OFF));
    else if (cmd >= ID_GAIN_OFF && cmd <= ID_GAIN_OFF + 10)  st->gainDb.store((int8_t)(cmd - ID_GAIN_OFF));
    else if (cmd >= ID_SCALE_0 && cmd < ID_SCALE_0 + kScaleCount) applyScale(d, kScales[cmd - ID_SCALE_0]);
    else if (cmd == ID_ABOUT_FM8)  Core::showAbout(*st);   // FM8's own dialog, modal until closed
    else if (cmd == ID_ABOUT_PLUS) ShellExecuteW(hwnd, L"open", kProjectUrl, nullptr, nullptr, SW_SHOWNORMAL);

    DestroyMenu(m);
}

// Sits in front of FM8's own NI::UIA window procedure. Everything outside the wordmark passes
// straight through, so FM8 behaves exactly as it does without us.
LRESULT CALLBACK sub(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto* d = (OData*)ref;
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            // Swallowed, so FM8's own logo Switch never sees the press and takes no capture.
            if (onLogo(hwnd, d, lp)) { showMenu(hwnd, d); return 0; }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p; GetCursorPos(&p); ScreenToClient(d->root, &p);
                RECT r = logoRect();
                if (PtInRect(&r, p)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, sub, id);
            delete d;
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// SetWindowSubclass only takes effect on the window's own thread. The standalone finds FM8's window
// from a worker thread, so it rides into FM8's UI thread on a one-shot WH_CALLWNDPROC hook.
struct Pending { LogoMenu* ov; HWND root; InstanceState* st; };
Pending g_pending{};
HHOOK g_installHook = nullptr;

struct Kids { HWND* out; int cap, n; };
BOOL CALLBACK collect(HWND h, LPARAM lp) {
    auto* k = (Kids*)lp;
    if (k->n < k->cap) k->out[k->n++] = h;
    return TRUE;
}
} // namespace

void LogoMenu::hookTree(HWND root, InstanceState* st, bool topLevel) {
    if (!root) return;
    // ponytail: FM8 draws the whole editor into the one window, but taking its descendants too costs
    // three lines and makes the click work wherever the toolkit decides to put its surface.
    HWND all[kMaxHooked] = {root};
    Kids k{all + 1, kMaxHooked - 1, 0};
    EnumChildWindows(root, collect, (LPARAM)&k);
    for (int i = 0; i < 1 + k.n; ++i) {
        auto* d = new OData{st, root, topLevel, resize_, resizeCtx_};   // freed in WM_NCDESTROY/detach
        if (SetWindowSubclass(all[i], sub, kSubclassId, (DWORD_PTR)d)) hooked_[count_++] = all[i];
        else delete d;
    }
}

void LogoMenu::attach(HWND parent, InstanceState* st) {
    if (count_ || !parent) return;
    // FM8 creates its editor child inside the host's window before this runs, and that child is
    // what the mouse goes to. Its client origin is the form origin, so it is our coordinate root.
    HWND fm8 = GetWindow(parent, GW_CHILD);
    hookTree(fm8 ? fm8 : parent, st, false);
}

void LogoMenu::detach() {
    for (int i = 0; i < count_; ++i) {
        DWORD_PTR ref = 0;
        if (IsWindow(hooked_[i]) && GetWindowSubclass(hooked_[i], sub, kSubclassId, &ref)) {
            RemoveWindowSubclass(hooked_[i], sub, kSubclassId);
            delete (OData*)ref;
        }
    }
    count_ = 0;
}

namespace {
struct FindCtx { DWORD pid; HWND found; };
BOOL CALLBACK findMain(HWND h, LPARAM lp) {
    auto* c = (FindCtx*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == c->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
        wchar_t t[128]; GetWindowTextW(h, t, 128);
        if (wcsstr(t, L"FM8")) { c->found = h; return FALSE; }
    }
    return TRUE;
}
} // namespace

LRESULT CALLBACK LogoMenu::installProc(int code, WPARAM wp, LPARAM lp) {
    HHOOK h = g_installHook;
    if (code == HC_ACTION && h) {
        g_installHook = nullptr;                                     // one shot
        g_pending.ov->hookTree(g_pending.root, g_pending.st, true);
        UnhookWindowsHookEx(h);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void LogoMenu::attachToMainWindow(InstanceState* st, unsigned timeoutMs) {
    HWND fm8 = nullptr;
    const unsigned step = 250;
    for (unsigned waited = 0; waited <= timeoutMs && !fm8; waited += step) {
        FindCtx c{GetCurrentProcessId(), nullptr};
        EnumWindows(findMain, (LPARAM)&c);
        if (c.found) fm8 = c.found; else Sleep(step);
    }
    if (!fm8) return;
    const DWORD tid = GetWindowThreadProcessId(fm8, nullptr);
    if (tid == GetCurrentThreadId()) { hookTree(fm8, st, true); return; }
    g_pending = {this, fm8, st};
    g_installHook = SetWindowsHookExW(WH_CALLWNDPROC, &LogoMenu::installProc, nullptr, tid);
    if (g_installHook)   // a cross-thread send is what makes the hook fire, and WM_NULL does nothing else
        SendMessageTimeoutW(fm8, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 5000, nullptr);
}

} // namespace fm8plus::ui
