#!/usr/bin/env python3
"""GUI editor probe for the FM8.plus wrappers (ctypes, no DAW needed).

Opens the plug-in editor in a real top-level window, pumps messages, then reports what the FM8.plus
"+" overlay window is doing: does it exist, who owns it, where it sits relative to the editor, and
which sibling window is above it in the Z order. Saves a screenshot of just the probe window (never
the desktop), and runs two live experiments on the overlay: raise it to the top of the Z order, then
move it back to where the shim intended, with a screenshot after each.

    python tools/vsteditor.py vst2 [dll]      default I:\\vstplugins64\\FM8.plus.dll
    python tools/vsteditor.py vst3 [vst3]     default C:\\Program Files\\Common Files\\VST3\\FM8.plus.vst3

    --seconds N   pump the editor this long before the dump (default 3)
    --out DIR     screenshot folder (default: this session's scratchpad, else cwd)
    --keep        after the dump, keep the window open until you close it
"""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vst2host as v2  # noqa: E402
import vst3host as v3  # noqa: E402

DEFAULT_VST2 = r"I:\vstplugins64\FM8.plus.dll"
DEFAULT_VST3 = r"C:\Program Files\Common Files\VST3\FM8.plus.vst3"
OVERLAY_CLASS = "FM8plusOverlay"

user32 = C.WinDLL("user32", use_last_error=True)
kernel32 = C.WinDLL("kernel32", use_last_error=True)

WNDPROC = C.WINFUNCTYPE(C.c_ssize_t, C.c_void_p, C.c_uint, C.c_size_t, C.c_ssize_t)
ENUMPROC = C.WINFUNCTYPE(C.c_int, C.c_void_p, C.c_ssize_t)
VP = C.c_void_p


class WNDCLASSW(C.Structure):
    _fields_ = [("style", C.c_uint), ("lpfnWndProc", WNDPROC), ("cbClsExtra", C.c_int), ("cbWndExtra", C.c_int),
                ("hInstance", VP), ("hIcon", VP), ("hCursor", VP), ("hbrBackground", VP),
                ("lpszMenuName", C.c_wchar_p), ("lpszClassName", C.c_wchar_p)]


class MSG(C.Structure):
    _fields_ = [("hwnd", VP), ("message", C.c_uint), ("wParam", C.c_size_t), ("lParam", C.c_ssize_t),
                ("time", C.c_uint), ("pt", W.POINT)]


def _sig(name, restype, *argtypes):
    f = getattr(user32, name)
    f.restype, f.argtypes = restype, argtypes
    return f


CreateWindowExW = _sig("CreateWindowExW", VP, C.c_uint, C.c_wchar_p, C.c_wchar_p, C.c_uint, C.c_int, C.c_int,
                       C.c_int, C.c_int, VP, VP, VP, VP)
DefWindowProcW = _sig("DefWindowProcW", C.c_ssize_t, VP, C.c_uint, C.c_size_t, C.c_ssize_t)
GetWindowRect = _sig("GetWindowRect", C.c_int, VP, C.POINTER(W.RECT))
GetClassNameW = _sig("GetClassNameW", C.c_int, VP, C.c_wchar_p, C.c_int)
GetWindowTextW = _sig("GetWindowTextW", C.c_int, VP, C.c_wchar_p, C.c_int)
GetWindowLongPtrW = _sig("GetWindowLongPtrW", C.c_ssize_t, VP, C.c_int)
IsWindowVisible = _sig("IsWindowVisible", C.c_int, VP)
GetWindow = _sig("GetWindow", VP, VP, C.c_uint)
GetParent = _sig("GetParent", VP, VP)
ClientToScreen = _sig("ClientToScreen", C.c_int, VP, C.POINTER(W.POINT))
SetWindowPos = _sig("SetWindowPos", C.c_int, VP, VP, C.c_int, C.c_int, C.c_int, C.c_int, C.c_uint)
DestroyWindow = _sig("DestroyWindow", C.c_int, VP)
PeekMessageW = _sig("PeekMessageW", C.c_int, C.POINTER(MSG), VP, C.c_uint, C.c_uint, C.c_uint)
TranslateMessage = _sig("TranslateMessage", C.c_int, C.POINTER(MSG))
DispatchMessageW = _sig("DispatchMessageW", C.c_ssize_t, C.POINTER(MSG))
PostQuitMessage = _sig("PostQuitMessage", None, C.c_int)
AdjustWindowRectEx = _sig("AdjustWindowRectEx", C.c_int, C.POINTER(W.RECT), C.c_uint, C.c_int, C.c_uint)
EnumWindows = _sig("EnumWindows", C.c_int, ENUMPROC, C.c_ssize_t)
GetWindowThreadProcessId = _sig("GetWindowThreadProcessId", C.c_uint, VP, C.POINTER(C.c_uint))
RegisterClassW = _sig("RegisterClassW", C.c_uint16, C.POINTER(WNDCLASSW))
kernel32.GetModuleHandleW.restype = VP
kernel32.GetModuleHandleW.argtypes = [C.c_wchar_p]
kernel32.GetCurrentProcess.restype = VP        # pseudo-handle is 64-bit -1; a c_int restype truncates it
kernel32.TerminateProcess.argtypes = [VP, C.c_uint]

WS_OVERLAPPEDWINDOW, WS_VISIBLE, WS_CHILD, WS_CLIPSIBLINGS, WS_CLIPCHILDREN = 0x00CF0000, 0x10000000, 0x40000000, 0x04000000, 0x02000000
WS_EX_LAYERED, WS_EX_TOPMOST, WS_EX_TRANSPARENT, WS_EX_TOOLWINDOW = 0x80000, 0x8, 0x20, 0x80
GW_CHILD, GW_HWNDNEXT, GW_HWNDPREV = 5, 2, 3
SWP_NOSIZE, SWP_NOMOVE, SWP_NOZORDER, SWP_NOACTIVATE = 1, 2, 4, 0x10
WM_CLOSE, WM_DESTROY, WM_QUIT = 0x10, 0x2, 0x12

_keep = []   # ctypes callbacks that must outlive the window


def _wndproc(h, m, w, l):
    if m == WM_CLOSE:
        DestroyWindow(h)
        return 0
    if m == WM_DESTROY:
        PostQuitMessage(0)
        return 0
    return DefWindowProcW(h, m, w, l)


def make_window(w, h, title):
    wc = WNDCLASSW()
    wc.lpfnWndProc = WNDPROC(_wndproc)
    _keep.append(wc.lpfnWndProc)
    wc.hInstance = kernel32.GetModuleHandleW(None)
    wc.hbrBackground = VP(6)   # COLOR_WINDOW + 1
    wc.lpszClassName = "FM8plusProbe"
    RegisterClassW(C.byref(wc))
    style = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN
    r = W.RECT(0, 0, w, h)
    AdjustWindowRectEx(C.byref(r), style, 0, 0)
    hwnd = CreateWindowExW(0, "FM8plusProbe", title, style, 100, 100, r.right - r.left, r.bottom - r.top,
                           None, None, wc.hInstance, None)
    assert hwnd, f"CreateWindowExW failed: {C.get_last_error()}"
    # A fresh process cannot take the foreground, so keep the probe above other windows: the
    # screenshots are screen grabs of its rect and must show it, not whatever was covering it.
    SetWindowPos(hwnd, VP(-1), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)   # HWND_TOPMOST
    return hwnd


def pump(seconds, idle=None):
    """Pump messages for `seconds` (or until the window closes); returns False on WM_QUIT."""
    msg = MSG()
    end, last = time.time() + seconds, 0.0
    while time.time() < end:
        while PeekMessageW(C.byref(msg), None, 0, 0, 1):
            if msg.message == WM_QUIT:
                return False
            TranslateMessage(C.byref(msg))
            DispatchMessageW(C.byref(msg))
        if idle and time.time() - last > 0.05:
            idle()
            last = time.time()
        time.sleep(0.01)
    return True


# --- window tree dump -------------------------------------------------------------------------

def info(h):
    cls, txt = C.create_unicode_buffer(256), C.create_unicode_buffer(256)
    GetClassNameW(h, cls, 256)
    GetWindowTextW(h, txt, 256)
    r = W.RECT()
    GetWindowRect(h, C.byref(r))
    return cls.value, txt.value, (r.left, r.top, r.right, r.bottom), GetWindowLongPtrW(h, -16), GetWindowLongPtrW(h, -20), bool(IsWindowVisible(h))


def flags(style, ex):
    out = []
    for bit, name in ((WS_VISIBLE, "VISIBLE"), (WS_CHILD, "CHILD"), (WS_CLIPSIBLINGS, "CLIPSIB"), (WS_CLIPCHILDREN, "CLIPCHILD")):
        if style & bit:
            out.append(name)
    for bit, name in ((WS_EX_LAYERED, "LAYERED"), (WS_EX_TOPMOST, "TOPMOST"), (WS_EX_TRANSPARENT, "TRANSPARENT"), (WS_EX_TOOLWINDOW, "TOOLWIN")):
        if ex & bit:
            out.append("EX_" + name)
    return " ".join(out)


def client_origin(h):
    p = W.POINT(0, 0)
    ClientToScreen(h, C.byref(p))
    return p.x, p.y


def dump_tree(root):
    """Print every descendant of `root`, top of Z order first per level, with rects relative to the
    editor's client origin. Returns the list of (hwnd, class) found."""
    ox, oy = client_origin(root)
    found = []

    def walk(parent, depth):
        h = GetWindow(parent, GW_CHILD)
        z = 0
        while h:
            cls, txt, r, st, ex, vis = info(h)
            rel = (r[0] - ox, r[1] - oy, r[2] - ox, r[3] - oy)
            mark = "  <-- FM8.plus overlay" if cls == OVERLAY_CLASS else ""
            print(f"{'  ' * depth}z{z} {h:#x} {cls!r} text={txt!r} rel={rel} size={r[2]-r[0]}x{r[3]-r[1]} "
                  f"[{flags(st, ex)}]{' HIDDEN' if not vis else ''}{mark}")
            found.append((h, cls))
            walk(h, depth + 1)
            h = GetWindow(h, GW_HWNDNEXT)
            z += 1

    print(f"editor host window {root:#x}, client origin {(ox, oy)}")
    walk(root, 1)
    return found


def overlays_anywhere():
    """Any FM8plusOverlay top-level windows owned by this process (the standalone-style popup form)."""
    pid, out = os.getpid(), []

    def cb(h, _):
        p = C.c_uint()
        GetWindowThreadProcessId(h, C.byref(p))
        if p.value == pid and info(h)[0] == OVERLAY_CLASS:
            out.append(h)
        return 1
    EnumWindows(ENUMPROC(cb), 0)
    return out


def screenshot(root, path):
    from PIL import ImageGrab
    r = W.RECT()
    GetWindowRect(root, C.byref(r))
    ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), include_layered_windows=True).save(path)
    print(f"screenshot -> {path}")


def zorder(root):
    """Class names of root's direct children, top of Z order first."""
    out, h = [], GetWindow(root, GW_CHILD)
    while h:
        out.append(info(h)[0][:16])   # 'NIVSTChildWindow<base>' shortens to its class stem
        h = GetWindow(h, GW_HWNDNEXT)
    return " > ".join(out) or "(no children)"


def resource_hook_check(module_name):
    """Where FM8's own FindResourceA import points: kernel32 (stock) or FM8.plus (forms served)."""
    import pefile
    k32 = C.WinDLL("kernel32", use_last_error=True)
    k32.GetModuleHandleW.restype = VP
    k32.GetModuleHandleW.argtypes = [C.c_wchar_p]
    k32.GetModuleFileNameW.argtypes = [VP, C.c_wchar_p, C.c_uint]
    base = k32.GetModuleHandleW(module_name)
    if not base:
        print(f"resource hook: {module_name} not loaded"); return
    path = C.create_unicode_buffer(260); k32.GetModuleFileNameW(base, path, 260)
    pe = pefile.PE(path.value, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    rva = next((imp.address - pe.OPTIONAL_HEADER.ImageBase for e in pe.DIRECTORY_ENTRY_IMPORT for imp in e.imports
                if imp.name == b"FindResourceA"), None)
    target = C.cast(base + rva, C.POINTER(C.c_void_p)).contents.value
    GET_FROM_ADDR, owner = 0x4, VP()
    k32.GetModuleHandleExW.argtypes = [C.c_uint, VP, C.POINTER(VP)]
    k32.GetModuleHandleExW(GET_FROM_ADDR, VP(target), C.byref(owner))
    who = C.create_unicode_buffer(260); k32.GetModuleFileNameW(owner, who, 260)
    print(f"resource hook: {module_name}!FindResourceA import -> {os.path.basename(who.value)}"
          f" ({'FM8.plus serves rebuilt forms' if 'plus' in who.value.lower() else 'stock resource path'})")


def diagnose(root, out_dir, tag, idle=None):
    found = dump_tree(root)
    ov = [h for h, cls in found if cls == OVERLAY_CLASS]
    popups = overlays_anywhere()
    screenshot(root, os.path.join(out_dir, f"{tag}_1_initial.png"))
    if not ov and not popups:
        print("RESULT: no FM8plusOverlay window exists anywhere in this process -> the shim never created it")
        return
    if popups:
        for h in popups:
            print(f"RESULT: overlay exists as a TOP-LEVEL popup {h:#x} rect={info(h)[2]}")
    for h in ov:
        cls, txt, r, st, ex, vis = info(h)
        ox, oy = client_origin(root)
        parent = GetParent(h)
        above = GetWindow(h, GW_HWNDPREV)
        print(f"RESULT: overlay {h:#x} is a child of {parent:#x} ({info(parent)[0]!r}), "
              f"rel={(r[0]-ox, r[1]-oy)} size={r[2]-r[0]}x{r[3]-r[1]} visible={vis}")
        if above:
            acls, _, ar, _, _, _ = info(above)
            print(f"        directly ABOVE it in Z order: {above:#x} {acls!r} size={ar[2]-ar[0]}x{ar[3]-ar[1]}"
                  f"{'  (covers the overlay)' if ar[0] <= r[0] and ar[1] <= r[1] and ar[2] >= r[2] and ar[3] >= r[3] else ''}")
        else:
            print("        nothing above it in Z order (it is the topmost sibling)")
        # Experiment 1: raise it. If the "+" appears now, the cause is Z order. Then keep pumping to
        # see whether FM8 pushes its own window back above ours.
        SetWindowPos(h, VP(0), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        pump(0.5, idle)
        screenshot(root, os.path.join(out_dir, f"{tag}_2_raised.png"))
        print(f"        z-order right after raise: {zorder(root)}")
        pump(3, idle)
        print(f"        z-order 3 s later:         {zorder(root)}")
        # Experiment 2: put it where the shim intended (109,22 in the parent's client). If it only
        # appears now, the cause is a bad position (UpdateLayeredWindow's pptDst on a child window).
        SetWindowPos(h, None, 109, 22, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
        pump(0.5, idle)
        screenshot(root, os.path.join(out_dir, f"{tag}_3_raised_moved.png"))
        r2 = info(h)[2]
        print(f"        after raise+move: rel={(r2[0]-ox, r2[1]-oy)}")


# --- VST2 ---------------------------------------------------------------------------------------

class ERect(C.Structure):
    _fields_ = [("top", C.c_int16), ("left", C.c_int16), ("bottom", C.c_int16), ("right", C.c_int16)]


effEditGetRect, effEditOpen, effEditClose, effEditIdle = 13, 14, 15, 19


def run_vst2(dll, seconds, out_dir, keep):
    h = v2.Host(dll)
    print(f"loaded {dll}: uniqueID={h.e.uniqueID:#x} name={h.string(v2.effGetEffectName)!r} flags={h.e.flags:#x} (hasEditor={bool(h.e.flags & 1)})")
    h.open()
    resource_hook_check("FM8.dll")
    pp = VP()
    h.d(effEditGetRect, 0, 0, C.cast(C.pointer(pp), VP))
    er = C.cast(pp, C.POINTER(ERect)).contents if pp.value else ERect(0, 0, 600, 900)
    w, ht = er.right - er.left, er.bottom - er.top
    print(f"effEditGetRect -> {w}x{ht}")
    hwnd = make_window(w, ht, "FM8.plus VST2 probe")
    rv = h.d(effEditOpen, 0, 0, hwnd)
    print(f"effEditOpen(hwnd={hwnd:#x}) -> {rv}")
    print(f"z-order right after effEditOpen: {zorder(hwnd)}")
    idle = lambda: h.d(effEditIdle)
    pump(0.3, idle)
    print(f"z-order after 0.3 s of idle:     {zorder(hwnd)}")
    pump(seconds, idle)
    print(f"z-order after {seconds:g} s more:       {zorder(hwnd)}")
    diagnose(hwnd, out_dir, "vst2", idle)
    if keep:
        print("window kept open; close it to finish")
        pump(1e9, idle)
    h.d(effEditClose)
    h.close()


# --- VST3 ---------------------------------------------------------------------------------------

class ViewRect(C.Structure):
    _fields_ = [("left", C.c_int32), ("top", C.c_int32), ("right", C.c_int32), ("bottom", C.c_int32)]


def run_vst3(path, seconds, out_dir, keep):
    lib = C.CDLL(path)
    if hasattr(lib, "InitDll"):
        lib.InitDll.restype = C.c_bool
        lib.InitDll()
    lib.GetPluginFactory.restype = VP
    factory = lib.GetPluginFactory()
    host = v3.make_host()
    n = v3.vt(factory, 4, C.c_int32)(factory)
    cid = None
    for i in range(n):
        ci = v3.PClassInfo()
        v3.vt(factory, 5, v3.TRESULT, C.c_int32, VP)(factory, i, C.addressof(ci))
        if ci.category == b"Audio Module Class" and cid is None:
            cid = bytes(ci.cid)
            print(f"class [{i}] {ci.name.decode()!r} cid={cid.hex()}")
    comp = VP()
    r = v3.vt(factory, 6, v3.TRESULT, VP, VP, C.POINTER(VP))(factory, (C.c_ubyte * 16)(*cid), v3.IID["IComponent"], C.byref(comp))
    print(f"createInstance(IComponent) -> {r:#x}; initialize -> {v3.vt(comp, 3, v3.TRESULT, VP)(comp, C.addressof(host.contents)):#x}")
    resource_hook_check("FM8.vst3")
    ctrl = VP()
    r = v3.vt(comp, 0, v3.TRESULT, VP, C.POINTER(VP))(comp, v3.IID["IEditController"], C.byref(ctrl))
    print(f"queryInterface(IEditController) -> {r:#x} ctrl={ctrl.value:#x}")
    view = v3.vt(ctrl, 17, VP, C.c_char_p)(ctrl, b"editor")   # IEditController::createView
    print(f"createView('editor') -> {view:#x}" if view else "createView('editor') -> NULL")
    if not view:
        return
    print(f"isPlatformTypeSupported('HWND') -> {v3.vt(view, 3, v3.TRESULT, C.c_char_p)(view, b'HWND'):#x}")
    vr = ViewRect()
    v3.vt(view, 9, v3.TRESULT, VP)(view, C.addressof(vr))
    w, ht = vr.right - vr.left, vr.bottom - vr.top
    print(f"getSize -> {w}x{ht}")
    hwnd = make_window(w or 900, ht or 600, "FM8.plus VST3 probe")
    r = v3.vt(view, 4, v3.TRESULT, VP, C.c_char_p)(view, hwnd, b"HWND")   # IPlugView::attached
    print(f"attached(hwnd={hwnd:#x}) -> {r:#x}")
    print(f"z-order right after attached: {zorder(hwnd)}")
    pump(0.3)
    print(f"z-order after 0.3 s:          {zorder(hwnd)}")
    pump(seconds)
    print(f"z-order after {seconds:g} s more:    {zorder(hwnd)}")
    diagnose(hwnd, out_dir, "vst3")
    if keep:
        print("window kept open; close it to finish")
        pump(1e9)
    v3.vt(view, 5, v3.TRESULT)(view)   # removed
    sys.stdout.flush()
    # ponytail: FM8.vst3's DLL detach deadlocks on exit, so end the process hard (same as vst3host.py).
    kernel32.TerminateProcess(kernel32.GetCurrentProcess(), 0)


def main():
    args = sys.argv[1:]
    seconds, out_dir, keep = 3.0, None, False
    if "--seconds" in args:
        i = args.index("--seconds"); seconds = float(args[i + 1]); del args[i:i + 2]
    if "--out" in args:
        i = args.index("--out"); out_dir = args[i + 1]; del args[i:i + 2]
    if "--keep" in args:
        args.remove("--keep"); keep = True
    kind = args[0] if args else "vst2"
    path = args[1] if len(args) > 1 else (DEFAULT_VST2 if kind == "vst2" else DEFAULT_VST3)
    out_dir = out_dir or os.environ.get("CLAUDE_SCRATCHPAD") or os.getcwd()
    os.makedirs(out_dir, exist_ok=True)
    user32.SetProcessDPIAware()
    (run_vst2 if kind == "vst2" else run_vst3)(path, seconds, out_dir, keep)


if __name__ == "__main__":
    main()
