#!/usr/bin/env python3
"""Dump a thin Mach-O's symbol table as "addr type name" lines: python tools/macho_syms.py <file> [out.txt]"""
import struct, sys

def syms(path):
    b = open(path, "rb").read()
    assert struct.unpack_from("<I", b)[0] == 0xfeedfacf, "not a thin 64-bit Mach-O"
    ncmds = struct.unpack_from("<I", b, 16)[0]
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", b, off)
        if cmd == 2:  # LC_SYMTAB
            symoff, nsyms, stroff, _ = struct.unpack_from("<4I", b, off + 8)
            for i in range(nsyms):
                strx, ntype, sect, desc, val = struct.unpack_from("<IBBHQ", b, symoff + 16 * i)
                if ntype & 0xe0:  # stab
                    continue
                end = b.index(b"\0", stroff + strx)
                yield val, ntype, b[stroff + strx:end].decode("latin1")
        off += size

if __name__ == "__main__":
    out = open(sys.argv[2], "w") if len(sys.argv) > 2 else sys.stdout
    for v, t, n in sorted(syms(sys.argv[1])):
        out.write(f"{v:012x} {t:02x} {n}\n")
