#include "ui.h"
#include "fm8plus.h"
#include "settings.h"
#include <windowsx.h>

namespace fm8plus::ui {
namespace {
const wchar_t* kClass = L"FM8plusOverlay";
constexpr int kW = 52, kH = 18;
enum { ID_MORPH = 1, ID_ARP_INT = 2, ID_ARP_CLONE = 3, ID_ARP_MIDI = 4 };

void showMenu(HWND hwnd, InstanceState* st) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (st->modWheelMorph.load() ? MF_CHECKED : 0), ID_MORPH, L"Mod wheel rotates Morph");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU arp = CreatePopupMenu();
    auto mode = (ArpMode)st->arpMode.load();
    AppendMenuW(arp, MF_STRING | (mode == ArpMode::Internal    ? MF_CHECKED : 0), ID_ARP_INT,   L"Internal");
    AppendMenuW(arp, MF_STRING | (mode == ArpMode::CloneToMidi ? MF_CHECKED : 0), ID_ARP_CLONE, L"Clone to MIDI");
    AppendMenuW(arp, MF_STRING | (mode == ArpMode::MidiOnly    ? MF_CHECKED : 0), ID_ARP_MIDI,  L"MIDI only (FM8 silent)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)arp, L"Arpeggiator MIDI out");

    POINT pt; GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    switch (cmd) {
        case ID_MORPH: {
            bool v = !st->modWheelMorph.load();
            st->modWheelMorph.store(v);
            settings::setDefaultModWheelMorph(v);
            break;
        }
        case ID_ARP_INT:   Core::flushExternal(*st); st->arpMode.store((uint8_t)ArpMode::Internal); settings::setArpModeDefault(0); break;
        case ID_ARP_CLONE: st->arpMode.store((uint8_t)ArpMode::CloneToMidi); settings::setArpModeDefault(1); break;
        case ID_ARP_MIDI:  st->arpMode.store((uint8_t)ArpMode::MidiOnly); settings::setArpModeDefault(2); break;
    }
    DestroyMenu(m);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (InstanceState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_LBUTTONUP:
            if (st) showMenu(hwnd, st);
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
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                            2, 2, kW, kH, parent, nullptr, self, nullptr);
    if (hwnd_) SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)st);
}

void Overlay::detach() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
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
    const unsigned step = 250;
    for (unsigned waited = 0; waited <= timeoutMs; waited += step) {
        FindCtx c{GetCurrentProcessId(), nullptr};
        EnumWindows(findMain, (LPARAM)&c);
        if (c.found) { attach(c.found, st, self); return; }
        Sleep(step);
    }
}

} // namespace fm8plus::ui
