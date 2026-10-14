// FM8.plus menu on macOS: the popup the "FM8+" wordmark opens, as an NSMenu with the same items as
// the Windows one (core/ui.cpp). FM8 itself draws the wordmark and routes its click to us through
// FormMain's vtable (core_mac.cpp), so there is no window or hit test of ours here.
#pragma once

namespace fm8plus {
struct InstanceState;
namespace macui {

// How the menu applies a GUI Scale step. The wrapper owns the editor view and the host resize path.
struct ScaleHost {
    void (*apply)(void* ctx, float scale) = nullptr;   // null: the GUI Scale submenu is hidden
    void* ctx = nullptr;
    void* view = nullptr;   // the scaled NSView holding FM8's editor, to tell which steps fit the screen
};

// Pop the menu at the mouse and act on the choice. UI thread, from inside FM8's click handling.
void showMenu(InstanceState* st, const ScaleHost& scale);

// Size the wrapper view (an NSView*) holding FM8's editor of `w` x `h` logical points for GUI Scale
// `s`: frame scaled and bounds logical, so Cocoa scales drawing and mouse together, with the frame
// rounded up to whole pixels and FM8's view filling it. A frame of 702.5 (562 rows at 1.25x) made
// Core Animation resample the layer and blur every horizontal edge.
void scaleWrapper(void* wrapper, double w, double h, double s);

} // namespace macui
} // namespace fm8plus
