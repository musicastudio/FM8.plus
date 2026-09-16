// FM8.plus in-editor toggle UI. FM8 itself draws the button: Core::serveLogo widens the header
// form's wordmark control and hands FM8 a wordmark bitmap with a "+" on it, so the "FM8+" logo is
// part of FM8's own GUI (it scales, clips and repaints with everything else, and no window of ours
// sits above the host's). All that is left here is the click: we subclass the window FM8 draws the
// editor into, and a press inside the wordmark opens the popup menu instead of reaching FM8. That
// press is what stock FM8 uses to open its About panel, so the menu ends with "About FM8", which
// calls FM8's own dialog function (Core::showAbout), and "About FM8.plus", which opens the project
// page.
#pragma once
#include <windows.h>

namespace fm8plus {
struct InstanceState;

namespace ui {

class LogoMenu {
public:
    void attach(HWND parent, InstanceState* st);   // parent = the editor window the host gave us
    void detach();                                 // editor closing

    // Standalone helper: find this process's main window (blocking up to timeoutMs) and hook it.
    // Safe to call from a worker thread; the thread returns once the subclass is in place.
    void attachToMainWindow(InstanceState* st, unsigned timeoutMs);

    // GUI Scale: a hosted plug-in cannot resize its own editor, it has to ask the host. The shim
    // registers the host's way of doing that (VST2 audioMasterSizeWindow, VST3 IPlugFrame::resizeView)
    // and the menu calls it with the new editor size in pixels. The standalone leaves this unset and
    // the menu resizes FM8's own top-level window itself.
    using HostResizeFn = void (*)(void* ctx, int w, int h);
    void setHostResize(HostResizeFn fn, void* ctx) { resize_ = fn; resizeCtx_ = ctx; }

private:
    void hookTree(HWND root, InstanceState* st, bool topLevel);
    static LRESULT CALLBACK installProc(int code, WPARAM wp, LPARAM lp);   // runs on FM8's UI thread
    static constexpr int kMaxHooked = 8;
    HWND hooked_[kMaxHooked] = {};
    int count_ = 0;
    HostResizeFn resize_ = nullptr;
    void* resizeCtx_ = nullptr;
};

} // namespace ui
} // namespace fm8plus
