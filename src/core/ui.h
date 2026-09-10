// FM8.plus in-editor toggle UI: a small "FM8+" button overlaid on the FM8 editor window that
// opens a popup menu with the mod-wheel-morph checkbox and the three-way arp MIDI-out radio.
// Self-contained Win32, so it needs no FM8 GUI reverse engineering. The standalone reuses the
// same overlay on its main window.
#pragma once
#include <windows.h>

namespace fm8plus {
struct InstanceState;

namespace ui {

class Overlay {
public:
    void attach(HWND parent, InstanceState* st, HMODULE self); // create the button on the editor
    void detach();                                             // destroy it (editor closing)
    void refresh(InstanceState& st);                           // re-sync check marks after a state load

    // Standalone helper: find this process's main window (blocking up to timeoutMs) and attach the
    // overlay to it. Safe to call from a worker thread; the button is created on that thread.
    void attachToMainWindow(InstanceState* st, HMODULE self, unsigned timeoutMs);
    // Optional: list MIDI out devices for the standalone port picker (names via midiOutGetDevCaps).
    void setStandalone(bool v) { standalone_ = v; }
private:
    HWND hwnd_ = nullptr;
    InstanceState* st_ = nullptr;
    bool standalone_ = false;
};

} // namespace ui
} // namespace fm8plus
