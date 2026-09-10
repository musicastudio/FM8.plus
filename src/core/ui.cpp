#include "ui.h"
#include "fm8plus.h"
#include "settings.h"
#include <windowsx.h>

namespace fm8plus::ui {
namespace {
const wchar_t* kClass = L"FM8plusOverlay";
constexpr int kW = 58, kH = 20;

// Command id ranges (kept apart so one TrackPopupMenu return value tells us which control fired).
enum {
    ID_MORPH_OFF = 1000, ID_MORPH_CC0 = 1001,          // ID_MORPH_CC0 + n  for CC n (0..127)
    ID_ARP_INT = 2000, ID_ARP_CLONE, ID_ARP_MIDI,
    ID_TEMPO_OFF = 3000,                                // ID_TEMPO_OFF + mode (0..5)
    ID_GAIN_OFF = 4000,                                 // ID_GAIN_OFF + db (0..10)
};

// Per-window data behind GWLP_USERDATA: the instance state and, for the standalone's top-level
// floating button, the FM8 window it tracks (null for the plugin child window).
struct OData { InstanceState* st; HWND target; };

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

void showMenu(HWND hwnd, InstanceState* st) {
    HMENU m = CreatePopupMenu();

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
    for (int d = 1; d <= 10; ++d) {
        wchar_t label[16]; swprintf(label, 16, L"+%d dB", d);
        AppendMenuW(gain, MF_STRING | (db == d ? MF_CHECKED : 0), ID_GAIN_OFF + d, label);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)gain, L"Increase Gain");

    SetForegroundWindow(hwnd);   // required so the popup dismisses correctly for a top-level tool window
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

    DestroyMenu(m);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = (OData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    InstanceState* st = d ? d->st : nullptr;
    switch (msg) {
        case WM_LBUTTONUP:
            if (st) showMenu(hwnd, st);
            return 0;
        case WM_TIMER:
            // Standalone: keep the floating button glued to FM8's window; close when FM8 goes away.
            if (d && d->target) {
                if (!IsWindow(d->target)) { DestroyWindow(hwnd); return 0; }
                RECT r; GetWindowRect(d->target, &r);
                SetWindowPos(hwnd, HWND_TOPMOST, r.left + 8, r.top + 62, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return 0;
        case WM_NCDESTROY:
            delete d; SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HBRUSH b = CreateSolidBrush(RGB(30, 30, 34));
            FillRect(dc, &rc, b); DeleteObject(b);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(220, 150, 40));
            DrawTextW(dc, L"FM8+", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensureClass(HMODULE self) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = self;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}
} // namespace

void Overlay::attach(HWND parent, InstanceState* st, HMODULE self) {
    if (hwnd_ || !parent) return;
    st_ = st;
    ensureClass(self);
    auto* d = new OData{st, nullptr};   // freed in WM_NCDESTROY
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                            2, 2, kW, kH, parent, nullptr, self, nullptr);
    if (hwnd_) SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)d);
    else delete d;
}

void Overlay::detach() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }   // WM_NCDESTROY frees the OData
    st_ = nullptr;
}

void Overlay::refresh(InstanceState&) {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
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

void Overlay::attachToMainWindow(InstanceState* st, HMODULE self, unsigned timeoutMs) {
    HWND fm8 = nullptr;
    const unsigned step = 250;
    for (unsigned waited = 0; waited <= timeoutMs && !fm8; waited += step) {
        FindCtx c{GetCurrentProcessId(), nullptr};
        EnumWindows(findMain, (LPARAM)&c);
        if (c.found) fm8 = c.found; else Sleep(step);
    }
    if (!fm8) return;

    // A top-level floating button (this worker thread owns it and pumps its messages), glued to the
    // FM8 window by a timer. A child of FM8's own window would be dead here, since this thread has no
    // pump and FM8's GL surface would cover it.
    ensureClass(self);
    RECT r; GetWindowRect(fm8, &r);
    auto* d = new OData{st, fm8};
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP | WS_VISIBLE,
                            r.left + 8, r.top + 62, kW, kH, nullptr, nullptr, self, nullptr);
    if (!hwnd_) { delete d; return; }
    st_ = st;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)d);
    SetTimer(hwnd_, 1, 500, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    hwnd_ = nullptr;   // window destroyed (FM8 closed); the message loop and this thread end
}

} // namespace fm8plus::ui
