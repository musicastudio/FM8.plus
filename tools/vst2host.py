#!/usr/bin/env python3
"""Tiny headless VST2 host (ctypes) for poking FM8.dll and, later, the FM8.plus wrapper.

    python tools/vst2host.py info   [dll]     effect name, vendor, params, canDo answers
    python tools/vst2host.py params [dll]     dump every parameter index and name
    python tools/vst2host.py arp    [dll]     turn the arp on, hold a chord, process blocks,
                                              report MIDI events the plugin sends to the host

Default dll: the installed 64-bit FM8.dll. The plugin is opened without an editor.
"""
from __future__ import annotations

import ctypes as C
import math
import sys
import time

DEFAULT_DLL = r"C:\Program Files\Native Instruments\VSTPlugins 64 bit\FM8.dll"
SR, BLOCK = 44100.0, 512

# --- opcodes (subset) ---
effOpen, effClose, effGetParamName, effSetSampleRate, effSetBlockSize, effMainsChanged = 0, 1, 8, 10, 11, 12
effProcessEvents, effGetEffectName, effGetVendorString, effGetProductString = 25, 45, 47, 48
effGetVendorVersion, effCanDo, effGetVstVersion, effStartProcess, effStopProcess = 49, 51, 58, 71, 72
effGetPlugCategory, effGetParamDisplay, effGetParamLabel = 35, 7, 6
amVersion, amCurrentId, amGetTime, amProcessEvents, amGetSampleRate, amGetBlockSize = 1, 2, 7, 8, 16, 17
amGetCurrentProcessLevel, amGetVendorString, amGetProductString, amGetVendorVersion = 23, 32, 33, 34
amCanDo, amGetLanguage, amGetDirectory, amUpdateDisplay = 37, 38, 41, 42

CANDO = ["sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents",
         "receiveVstMidiEvent", "receiveVstTimeInfo", "offline", "midiProgramNames", "bypass",
         "sendVstMidiEventFlagIsRealtime", "MPE", "hasCockosExtensions", "supportsViewDpiScaling"]


class AEffect(C.Structure):
    pass


DISPATCHER = C.CFUNCTYPE(C.c_ssize_t, C.POINTER(AEffect), C.c_int32, C.c_int32, C.c_ssize_t, C.c_void_p, C.c_float)
PROCESS = C.CFUNCTYPE(None, C.POINTER(AEffect), C.POINTER(C.POINTER(C.c_float)), C.POINTER(C.POINTER(C.c_float)), C.c_int32)
SETPARAM = C.CFUNCTYPE(None, C.POINTER(AEffect), C.c_int32, C.c_float)
GETPARAM = C.CFUNCTYPE(C.c_float, C.POINTER(AEffect), C.c_int32)
AEffect._fields_ = [
    ("magic", C.c_int32), ("dispatcher", DISPATCHER), ("process", C.c_void_p),
    ("setParameter", SETPARAM), ("getParameter", GETPARAM),
    ("numPrograms", C.c_int32), ("numParams", C.c_int32), ("numInputs", C.c_int32), ("numOutputs", C.c_int32),
    ("flags", C.c_int32), ("resvd1", C.c_ssize_t), ("resvd2", C.c_ssize_t), ("initialDelay", C.c_int32),
    ("realQualities", C.c_int32), ("offQualities", C.c_int32), ("ioRatio", C.c_float),
    ("object", C.c_void_p), ("user", C.c_void_p), ("uniqueID", C.c_int32), ("version", C.c_int32),
    ("processReplacing", PROCESS), ("processDoubleReplacing", C.c_void_p), ("future", C.c_char * 56)]


class VstMidiEvent(C.Structure):
    _fields_ = [("type", C.c_int32), ("byteSize", C.c_int32), ("deltaFrames", C.c_int32), ("flags", C.c_int32),
                ("noteLength", C.c_int32), ("noteOffset", C.c_int32), ("midiData", C.c_char * 4),
                ("detune", C.c_char), ("noteOffVelocity", C.c_char), ("r1", C.c_char), ("r2", C.c_char)]


class VstTimeInfo(C.Structure):
    _fields_ = [(n, C.c_double) for n in ("samplePos", "sampleRate", "nanoSeconds", "ppqPos", "tempo",
                                          "barStartPos", "cycleStartPos", "cycleEndPos")] + \
               [(n, C.c_int32) for n in ("timeSigNumerator", "timeSigDenominator", "smpteOffset",
                                         "smpteFrameRate", "samplesToNextClock", "flags")]


def make_events(msgs, delta=0):
    """msgs: list of (status, d1, d2). Returns a VstEvents-compatible buffer."""
    n = len(msgs)
    evs = (VstMidiEvent * max(n, 1))()
    for i, (s, d1, d2) in enumerate(msgs):
        evs[i].type, evs[i].byteSize, evs[i].deltaFrames = 1, C.sizeof(VstMidiEvent), delta
        evs[i].midiData = bytes([s, d1, d2, 0])
    class VstEvents(C.Structure):
        _fields_ = [("numEvents", C.c_int32), ("reserved", C.c_ssize_t), ("events", C.c_void_p * max(n, 1))]
    ve = VstEvents()
    ve.numEvents = n
    for i in range(n):
        ve.events[i] = C.addressof(evs[i])
    ve._keep = evs
    return ve


class Host:
    def __init__(self, dll=DEFAULT_DLL):
        self.time = VstTimeInfo(sampleRate=SR, tempo=120.0, timeSigNumerator=4, timeSigDenominator=4,
                                flags=(1 << 1) | (1 << 9) | (1 << 10) | (1 << 13))
        self.received = []  # (block, deltaFrames, bytes)
        self.block = 0
        self.cb = DISPATCHER(self._callback)
        self.lib = C.CDLL(dll)
        main = self.lib.VSTPluginMain
        main.restype = C.POINTER(AEffect)
        main.argtypes = [DISPATCHER]
        self.eff = main(self.cb)
        assert self.eff and self.eff.contents.magic == 0x56737450, "not a VST2 plugin"
        self.e = self.eff.contents

    def _callback(self, eff, opcode, index, value, ptr, opt):
        if opcode == amVersion:
            return 2400
        if opcode == amGetSampleRate:
            return int(SR)
        if opcode == amGetBlockSize:
            return BLOCK
        if opcode == amGetTime:
            self.time.samplePos = self.block * BLOCK
            self.time.ppqPos = self.time.samplePos / SR * self.time.tempo / 60.0
            return C.addressof(self.time)
        if opcode == amCanDo:
            s = C.cast(ptr, C.c_char_p).value or b""
            return 1 if s in (b"sendVstEvents", b"sendVstMidiEvent", b"sendVstTimeInfo", b"receiveVstEvents",
                              b"receiveVstMidiEvent", b"sizeWindow", b"supplyIdle") else 0
        if opcode == amProcessEvents:
            class VstEventsHdr(C.Structure):
                _fields_ = [("numEvents", C.c_int32), ("reserved", C.c_ssize_t)]
            hdr = C.cast(ptr, C.POINTER(VstEventsHdr)).contents
            arr = C.cast(ptr + C.sizeof(VstEventsHdr), C.POINTER(C.c_void_p))
            for i in range(hdr.numEvents):
                ev = C.cast(arr[i], C.POINTER(VstMidiEvent)).contents
                if ev.type == 1:
                    self.received.append((self.block, ev.deltaFrames, bytes(ev.midiData[:3])))
            return 1
        if opcode in (amGetVendorString, amGetProductString):
            C.memmove(ptr, b"FM8.plus host\0", 14)
            return 1
        if opcode == amGetVendorVersion:
            return 1
        if opcode == amGetLanguage:
            return 1
        if opcode in (amGetCurrentProcessLevel, amCurrentId, amGetDirectory, amUpdateDisplay):
            return 0
        return 0

    def d(self, opcode, index=0, value=0, ptr=None, opt=0.0):
        return self.e.dispatcher(self.eff, opcode, index, value, ptr, opt)

    def string(self, opcode, index=0, size=256):
        buf = C.create_string_buffer(size)
        self.d(opcode, index, 0, C.cast(buf, C.c_void_p))
        return buf.value.decode(errors="replace")

    def open(self):
        self.d(effOpen)
        self.d(effSetSampleRate, opt=SR)
        self.d(effSetBlockSize, value=BLOCK)
        self.d(effMainsChanged, value=1)
        self.d(effStartProcess)

    def close(self):
        self.d(effStopProcess)
        self.d(effMainsChanged, value=0)
        self.d(effClose)

    def param_index(self, name):
        for i in range(self.e.numParams):
            if self.string(effGetParamName, i) == name:
                return i
        return None

    def send(self, msgs):
        # Keep the VstEvents buffer alive until the next block is processed: FM8 stores the pointer in
        # effProcessEvents and reads it during processReplacing, so letting it be freed here is a
        # use-after-free that occasionally crashes when Python reuses the memory.
        ve = make_events(msgs)
        self._pending_events = ve
        self.d(effProcessEvents, 0, 0, C.cast(C.pointer(ve), C.c_void_p))

    def process(self, blocks=1):
        nin, nout = max(self.e.numInputs, 1), max(self.e.numOutputs, 1)
        ins = [(C.c_float * BLOCK)() for _ in range(nin)]
        outs = [(C.c_float * BLOCK)() for _ in range(nout)]
        pin = (C.POINTER(C.c_float) * nin)(*[C.cast(b, C.POINTER(C.c_float)) for b in ins])
        pout = (C.POINTER(C.c_float) * nout)(*[C.cast(b, C.POINTER(C.c_float)) for b in outs])
        peak = 0.0
        for _ in range(blocks):
            self.e.processReplacing(self.eff, pin, pout, BLOCK)
            peak = max(peak, max(abs(x) for x in outs[0]))
            self.block += 1
        return peak


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "info"
    dll = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_DLL
    h = Host(dll)
    e = h.e
    print(f"loaded {dll}")
    print(f"uniqueID={e.uniqueID:#x} version={e.version} flags={e.flags:#x} params={e.numParams} "
          f"programs={e.numPrograms} in={e.numInputs} out={e.numOutputs} initialDelay={e.initialDelay}")
    h.open()
    try:
        if cmd == "info":
            print("effect:", h.string(effGetEffectName), "| vendor:", h.string(effGetVendorString),
                  "| product:", h.string(effGetProductString), "| vstVersion:", h.d(effGetVstVersion),
                  "| category:", h.d(effGetPlugCategory))
            for s in CANDO:
                print(f"  canDo {s:<32} -> {h.d(effCanDo, 0, 0, C.cast(C.c_char_p(s.encode()), C.c_void_p))}")
        elif cmd == "params":
            for i in range(e.numParams):
                print(f"{i:5d} {h.string(effGetParamName, i)!r:40} = {e.getParameter(h.eff, i):.4f} "
                      f"{h.string(effGetParamDisplay, i)!r} {h.string(effGetParamLabel, i)!r}")
        elif cmd == "chunk":
            # Does effSetChunk tolerate trailing bytes? Decides per-instance state persistence.
            effGetChunk, effSetChunk = 23, 24

            def show(tag):
                h.process(2)
                print(f"  {tag}: Morph X={e.getParameter(h.eff, 21):.3f} Morph Y={e.getParameter(h.eff, 22):.3f} "
                      f"Arp On={e.getParameter(h.eff, 136):.3f} Volume={e.getParameter(h.eff, 26):.3f}")

            show("initial")
            e.setParameter(h.eff, 21, 0.75); e.setParameter(h.eff, 22, 0.25)
            e.setParameter(h.eff, 136, 1.0); e.setParameter(h.eff, 26, 0.5)
            show("after setParameter")
            pp = C.c_void_p()
            for idx, what in ((0, "bank"), (1, "program")):
                n = h.d(effGetChunk, idx, 0, C.cast(C.pointer(pp), C.c_void_p))
                data = C.string_at(pp, n)
                print(f"getChunk({what}) -> {n} bytes, head={data[:16].hex()} tail={data[-16:].hex()}")
                e.setParameter(h.eff, 21, 0.10); e.setParameter(h.eff, 22, 0.90)
                e.setParameter(h.eff, 136, 0.0); e.setParameter(h.eff, 26, 0.9)
                show("changed")
                for trailer in (b"", b"FM8+" + bytes([1, 2, 0, 0, 0, 0, 0, 0])):
                    blob = C.create_string_buffer(data + trailer, len(data) + len(trailer))
                    r = h.d(effSetChunk, idx, len(blob), C.cast(blob, C.c_void_p))
                    print(f"setChunk({what}, {len(data)}+{len(trailer)}) -> {r}")
                    show("after setChunk")
                    e.setParameter(h.eff, 21, 0.10); e.setParameter(h.eff, 22, 0.90)
                    e.setParameter(h.eff, 136, 0.0); e.setParameter(h.eff, 26, 0.9)
                    show("changed again")
                n2 = h.d(effGetChunk, idx, 0, C.cast(C.pointer(pp), C.c_void_p))
                print(f"getChunk({what}) after trailer set -> {n2} bytes (was {n})")
        elif cmd == "plus":
            # End-to-end test of the FM8.plus proxy: arp MIDI out, any-CC morph + blocking, gain, tempo.
            effGetChunk, effSetChunk = 23, 24
            pp = C.c_void_p()

            def set_trailer(arpmode=0, tempomode=0, gaindb=0, morphcc=-1):
                n = h.d(effGetChunk, 0, 0, C.cast(C.pointer(pp), C.c_void_p))
                data = bytearray(C.string_at(pp, n))
                assert bytes(data[-14:-6]) == b"FM8PLUS2", "no FM8.plus v2 trailer (proxy not active?)"
                mc = morphcc & 0xffff
                data[-6] = arpmode; data[-5] = tempomode; data[-4] = gaindb & 0xff
                data[-3] = mc & 0xff; data[-2] = (mc >> 8) & 0xff
                blob = C.create_string_buffer(bytes(data), len(data))
                h.d(effSetChunk, 0, len(data), C.cast(blob, C.c_void_p))

            def run_chord(blocks=180):
                h.received.clear()
                h.send([(0x90, 60, 100), (0x90, 64, 100), (0x90, 67, 100)])
                peak = h.process(blocks)
                h.send([(0x80, 60, 0), (0x80, 64, 0), (0x80, 67, 0)])
                peak = max(peak, h.process(20))
                non = [m for _, _, m in h.received if (m[0] & 0xf0) == 0x90 and m[2] > 0]
                return peak, len(h.received), non

            idx = h.param_index("Arpeggiator On") or 136
            e.setParameter(h.eff, idx, 1.0)
            print(f"Arp On index {idx} set\n(1+2) Arp MIDI out:")
            for label, mode in (("Internal", 0), ("Clone to MIDI", 1), ("MIDI only", 2)):
                set_trailer(arpmode=mode)
                peak, total, non = run_chord()
                print(f"  arp={label:14} audio_peak={peak:.3f}  host_events={total:3d}  arp_noteons={len(non)}")

            print("(1) Morph Rotate Control on an arbitrary CC (CC 11 Expression), CC1 should be inert:")
            set_trailer(morphcc=11)
            e.setParameter(h.eff, 21, 0.5); e.setParameter(h.eff, 22, 0.5)
            seen = []
            for cc in (0, 32, 64, 96, 127):
                h.send([(0xB0, 11, cc)]); h.process(4)
                seen.append((cc, round(e.getParameter(h.eff, 21), 3), round(e.getParameter(h.eff, 22), 3)))
            for cc, x, y in seen:
                print(f"    CC11={cc:3d} -> Morph X={x:.3f} Y={y:.3f}")
            moved11 = len({(x, y) for _, x, y in seen}) > 1
            e.setParameter(h.eff, 21, 0.5); e.setParameter(h.eff, 22, 0.5)
            h.send([(0xB0, 1, 0)]); h.process(4); h.send([(0xB0, 1, 127)]); h.process(4)
            moved1 = (round(e.getParameter(h.eff, 21), 3), round(e.getParameter(h.eff, 22), 3)) != (0.5, 0.5)
            print(f"  morph follows CC11: {'YES' if moved11 else 'NO'};  responds to CC1 (should be NO): {'YES' if moved1 else 'NO'}")

            print("(3) Increase Gain (measure output peak of a held note):")
            for db in (0, 6, 10):
                set_trailer(gaindb=db)
                h.received.clear(); h.send([(0x90, 60, 100)]); peak = h.process(60); h.send([(0x80, 60, 0)]); h.process(8)
                print(f"    gain=+{db:2d} dB -> peak {peak:.3f}  (expected x{10**(db/20):.2f})")

            print("(2b) Tempo Override (arp note density over 200 blocks; 2x should ~double):")
            for label, tm in (("Off", 0), ("2x Host", 3), ("0.5x Host", 2)):
                set_trailer(arpmode=1, tempomode=tm)
                _, _, non = run_chord(200)
                print(f"    tempo={label:10} -> {len(non)} arp note-ons")
        elif cmd == "arp":
            idx = h.param_index("Arp On") or h.param_index("Arpeggiator On")
            print("Arp On index:", idx)
            if idx is not None:
                e.setParameter(h.eff, idx, 1.0)
            h.send([(0x90, 60, 100), (0x90, 64, 100), (0x90, 67, 100)])
            peak = h.process(200)  # ~2.3 s at 120 bpm
            h.send([(0x80, 60, 0), (0x80, 64, 0), (0x80, 67, 0)])
            h.process(20)
            print(f"peak output {peak:.3f}; MIDI events received from plugin: {len(h.received)}")
            for b, d, m in h.received[:40]:
                print(f"  block {b:4d} +{d:4d}  {m.hex(' ')}")
    finally:
        h.close()


if __name__ == "__main__":
    main()
