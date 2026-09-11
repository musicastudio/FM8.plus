#!/usr/bin/env python3
"""Tiny headless VST3 probe (ctypes) for FM8.vst3: classes, buses, parameters.

    python tools/vst3host.py buses  [path]    list every audio/event bus in and out
    python tools/vst3host.py params [path]    dump parameter ids and titles

Default path: the installed C:\\Program Files\\Common Files\\VST3\\FM8.vst3.
"""
from __future__ import annotations

import ctypes as C
import os
import sys

DEFAULT = r"C:\Program Files\Common Files\VST3\FM8.vst3"
kResultOk, kNoInterface, kNotImplemented = 0, 0x80004002, 0x80004001


def uid(l1, l2, l3, l4):
    """TUID bytes in the COM-compatible layout the Windows SDK uses."""
    b = [l1 & 0xFF, (l1 >> 8) & 0xFF, (l1 >> 16) & 0xFF, (l1 >> 24) & 0xFF,
         (l2 >> 16) & 0xFF, (l2 >> 24) & 0xFF, l2 & 0xFF, (l2 >> 8) & 0xFF,
         (l3 >> 24) & 0xFF, (l3 >> 16) & 0xFF, (l3 >> 8) & 0xFF, l3 & 0xFF,
         (l4 >> 24) & 0xFF, (l4 >> 16) & 0xFF, (l4 >> 8) & 0xFF, l4 & 0xFF]
    return (C.c_ubyte * 16)(*b)


IID = {
    "FUnknown": uid(0x00000000, 0x00000000, 0xC0000000, 0x00000046),
    "IPluginFactory": uid(0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F),
    "IComponent": uid(0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802),
    "IAudioProcessor": uid(0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D),
    "IEditController": uid(0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E),
    "IMidiMapping": uid(0xDF0FF9F7, 0x49B74669, 0xB63AB732, 0x7ADBF5E5),
    "IHostApplication": uid(0x58E595CC, 0xDB2D4969, 0x8B6AAF8C, 0x36A664E5),
}

TRESULT = C.c_uint32
VP = C.c_void_p


def vt(this, index, restype, *argtypes):
    """Call slot `index` of a COM-style object's vtable."""
    vtable = C.cast(this, C.POINTER(C.POINTER(C.c_void_p))).contents
    fn = C.cast(vtable[index], C.CFUNCTYPE(restype, VP, *argtypes))
    return fn


class PClassInfo(C.Structure):
    _fields_ = [("cid", C.c_ubyte * 16), ("cardinality", C.c_int32), ("category", C.c_char * 32), ("name", C.c_char * 64)]


class BusInfo(C.Structure):
    _fields_ = [("mediaType", C.c_int32), ("direction", C.c_int32), ("channelCount", C.c_int32),
                ("name", C.c_wchar * 128), ("busType", C.c_int32), ("flags", C.c_uint32)]


class ParameterInfo(C.Structure):
    _fields_ = [("id", C.c_uint32), ("title", C.c_wchar * 128), ("shortTitle", C.c_wchar * 128),
                ("units", C.c_wchar * 128), ("stepCount", C.c_int32), ("defaultNormalizedValue", C.c_double),
                ("unitId", C.c_int32), ("flags", C.c_int32)]


# --- a minimal IHostApplication so the plugin has a context to talk to ---
QI = C.CFUNCTYPE(TRESULT, VP, VP, C.POINTER(VP))
ADDREF = C.CFUNCTYPE(C.c_uint32, VP)
GETNAME = C.CFUNCTYPE(TRESULT, VP, VP)
CREATE = C.CFUNCTYPE(TRESULT, VP, VP, VP, C.POINTER(VP))


def make_host():
    def qi(this, iid, obj):
        want = bytes(C.cast(iid, C.POINTER(C.c_ubyte * 16)).contents)
        if want in (bytes(IID["FUnknown"]), bytes(IID["IHostApplication"])):
            obj[0] = this
            return kResultOk
        obj[0] = None
        return kNoInterface

    def addref(this):
        return 1

    def getname(this, name):
        C.memmove(name, "FM8.plus probe".encode("utf-16le") + b"\0\0", 30)
        return kResultOk

    def create(this, cid, iid, obj):
        return kNotImplemented

    funcs = [QI(qi), ADDREF(addref), ADDREF(addref), GETNAME(getname), CREATE(create)]
    vtable = (C.c_void_p * 5)(*[C.cast(f, C.c_void_p) for f in funcs])
    obj = C.c_void_p(C.addressof(vtable))
    holder = C.pointer(obj)
    holder._keep = (funcs, vtable, obj)
    return holder


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "buses"
    path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT
    lib = C.CDLL(path)
    if hasattr(lib, "InitDll"):
        lib.InitDll.restype = C.c_bool
        print("InitDll ->", lib.InitDll())
    lib.GetPluginFactory.restype = VP
    factory = lib.GetPluginFactory()
    host = make_host()
    n = vt(factory, 4, C.c_int32)(factory)
    print(f"factory classes: {n}")
    comp_cid = None
    for i in range(n):
        ci = PClassInfo()
        vt(factory, 5, TRESULT, C.c_int32, VP)(factory, i, C.addressof(ci))
        print(f"  [{i}] {ci.category.decode():<24} {ci.name.decode():<32} cid={bytes(ci.cid).hex()}")
        if ci.category == b"Audio Module Class" and comp_cid is None:
            comp_cid = bytes(ci.cid)

    comp = VP()
    r = vt(factory, 6, TRESULT, VP, VP, C.POINTER(VP))(factory, (C.c_ubyte * 16)(*comp_cid), IID["IComponent"], C.byref(comp))
    print(f"createInstance(IComponent) -> {r:#x} {comp.value:#x}")
    r = vt(comp, 3, TRESULT, VP)(comp, C.addressof(host.contents))
    print(f"component.initialize(host) -> {r:#x}")

    if cmd == "buses":
        for mt, mname in ((0, "audio"), (1, "event")):
            for d, dname in ((0, "in"), (1, "out")):
                cnt = vt(comp, 7, C.c_int32, C.c_int32, C.c_int32)(comp, mt, d)
                print(f"{mname} {dname}: {cnt} bus(es)")
                for b in range(cnt):
                    bi = BusInfo()
                    vt(comp, 8, TRESULT, C.c_int32, C.c_int32, C.c_int32, VP)(comp, mt, d, b, C.addressof(bi))
                    print(f"    [{b}] {bi.name!r} channels={bi.channelCount} busType={bi.busType} flags={bi.flags:#x}")

    # controller: separate class or the same object
    ctrl = VP()
    ccid = (C.c_ubyte * 16)()
    r = vt(comp, 5, TRESULT, VP)(comp, ccid)
    if r == kResultOk:
        r2 = vt(factory, 6, TRESULT, VP, VP, C.POINTER(VP))(factory, ccid, IID["IEditController"], C.byref(ctrl))
        print(f"controller class {bytes(ccid).hex()} createInstance -> {r2:#x}")
        if ctrl.value:
            print(f"controller.initialize -> {vt(ctrl, 3, TRESULT, VP)(ctrl, C.addressof(host.contents)):#x}")
    else:
        r2 = vt(comp, 0, TRESULT, VP, C.POINTER(VP))(comp, IID["IEditController"], C.byref(ctrl))
        print(f"single-component: queryInterface(IEditController) -> {r2:#x}")

    if ctrl.value:
        pc = vt(ctrl, 8, C.c_int32)(ctrl)
        print(f"parameters: {pc}")
        if cmd == "params":
            for i in range(pc):
                pi = ParameterInfo()
                vt(ctrl, 9, TRESULT, C.c_int32, VP)(ctrl, i, C.addressof(pi))
                print(f"{i:5d} id={pi.id:<10} {pi.title!r:44} short={pi.shortTitle!r:20} steps={pi.stepCount} "
                      f"default={pi.defaultNormalizedValue:.3f} unit={pi.unitId} flags={pi.flags:#x}")
        else:
            for i in range(pc):
                pi = ParameterInfo()
                vt(ctrl, 9, TRESULT, C.c_int32, VP)(ctrl, i, C.addressof(pi))
                if "Morph" in pi.title or "Arp" in pi.title[:4] or "Arpeggiator On" in pi.title:
                    print(f"  {i:5d} id={pi.id:<10} {pi.title!r} flags={pi.flags:#x}")
        # IMidiMapping: does CC1 map to a parameter?
        mm = VP()
        if vt(ctrl, 0, TRESULT, VP, C.POINTER(VP))(ctrl, IID["IMidiMapping"], C.byref(mm)) == kResultOk:
            pid = C.c_uint32(0xFFFFFFFF)
            r = vt(mm, 3, TRESULT, C.c_int32, C.c_int16, C.c_int16, C.POINTER(C.c_uint32))(mm, 0, 0, 1, C.byref(pid))
            print(f"IMidiMapping: bus0 ch0 CC1 -> {r:#x} paramId={pid.value:#x}")
        else:
            print("IMidiMapping: not supported")
    # ponytail: the plugin's DLL detach deadlocks on exit (even os._exit), so skip detach entirely.
    sys.stdout.flush()
    # The pseudo-handle is a 64-bit -1; the default c_int restype truncated it, so TerminateProcess
    # failed and every run of this probe used to hang in that detach (12 zombie hosts, 2026-09-10).
    C.windll.kernel32.GetCurrentProcess.restype = C.c_void_p
    C.windll.kernel32.TerminateProcess.argtypes = [C.c_void_p, C.c_uint]
    C.windll.kernel32.TerminateProcess(C.windll.kernel32.GetCurrentProcess(), 0)


if __name__ == "__main__":
    main()
