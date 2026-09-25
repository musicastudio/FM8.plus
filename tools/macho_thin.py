#!/usr/bin/env python3
"""Split universal Mach-O files into FM8.<arch> slices beside them: python tools/macho_thin.py <file>..."""
import struct,sys
from pathlib import Path
CPU={0x01000007:"x86_64",0x0100000c:"arm64"}
for p in sys.argv[1:]:
    p=Path(p); b=p.read_bytes()
    if b[:4]!=b"\xca\xfe\xba\xbe": print(p,"thin"); continue
    n=struct.unpack(">I",b[4:8])[0]
    for i in range(n):
        cpu,sub,off,size,al=struct.unpack(">5I",b[8+20*i:28+20*i])
        out=p.with_name(p.name+"."+CPU[cpu]); out.write_bytes(b[off:off+size]); print(out,size)
