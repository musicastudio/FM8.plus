#!/usr/bin/env python3
"""Disassemble a function of a thin Mac FM8 slice by (demangled-substring | 0xaddr), annotating
direct call/jump targets with their symbols: python tools/macdis.py <macho> <pattern> [max_insns]"""
import bisect, struct, sys
import capstone
from itanium_demangler import parse
sys.path.insert(0, __import__("os").path.dirname(__file__))
from macho_syms import syms

def demangle(n):
    if n.startswith("__Z"):
        try: return str(parse(n[1:])) or n
        except Exception: return n
    return n

def load(path):
    b = open(path, "rb").read()
    cpu = struct.unpack_from("<I", b, 4)[0]
    segs = []
    ncmds = struct.unpack_from("<I", b, 16)[0]; off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", b, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", b, off + 24)
            segs.append((vmaddr, vmsize, fileoff, filesize))
        off += size
    S = sorted((v, demangle(n)) for v, t, n in syms(path) if (t & 0x0e) == 0x0e and v)
    return b, cpu, segs, S

def main():
    path, pat = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    b, cpu, segs, S = load(path)
    addrs = [a for a, _ in S]
    def name_at(a):
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0: return None
        return S[i][1] if S[i][0] == a else f"{S[i][1]}+{a - S[i][0]:#x}"
    if pat.startswith("0x"):
        start = int(pat, 16)
    else:
        hits = [(a, s) for a, s in S if pat in s]
        for a, s in hits[:20]: print(f"# {a:#x} {s}")
        if len(hits) != 1 and not any(s == pat for _, s in hits): print(f"# {len(hits)} matches"); 
        start = next((a for a, s in hits if s == pat), hits[0][0])
    i = bisect.bisect_right(addrs, start)
    end = addrs[i] if i < len(addrs) else start + 0x1000
    seg = next(s for s in segs if s[0] <= start < s[0] + s[1])
    code = b[seg[2] + start - seg[0]: seg[2] + end - seg[0]]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64) if cpu == 0x01000007 else \
         capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    print(f"== {start:#x} {name_at(start)}  ({end - start:#x} bytes)")
    for k, ins in enumerate(md.disasm(code, start)):
        if k >= n: break
        ann = ""
        if ins.mnemonic.startswith(("call", "j", "bl", "b.", "b ")) or ins.mnemonic in ("b", "bl"):
            try:
                t = int(ins.op_str.split()[-1].lstrip("#"), 16); nm = name_at(t)
                if nm: ann = f"   ; {nm}"
            except ValueError: pass
        print(f"{ins.address:#010x}  {ins.mnemonic:8} {ins.op_str}{ann}")

if __name__ == "__main__":
    main()
