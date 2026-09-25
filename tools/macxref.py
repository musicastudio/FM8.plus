#!/usr/bin/env python3
"""Find direct call/jmp (E8/E9 rel32) and pointer-sized data references to a function in a thin
x86_64 Mac FM8 slice: python tools/macxref.py <macho> <pattern|0xaddr>"""
import re, struct, sys, bisect
sys.path.insert(0, __import__("os").path.dirname(__file__))
from macdis import load

def main():
    path, pat = sys.argv[1], sys.argv[2]
    b, cpu, segs, S = load(path)
    addrs = [a for a, _ in S]
    def nm(a):
        i = bisect.bisect_right(addrs, a) - 1
        return f"{S[i][1]}+{a - S[i][0]:#x}" if i >= 0 else hex(a)
    t = int(pat, 16) if pat.startswith("0x") else next(a for a, s in S if pat in s)
    print(f"target {t:#x} {nm(t)}")
    text = next(s for s in segs if s[0] <= t < s[0] + s[1])
    base, fo, sz = text[0], text[2], text[3]
    code = b[fo:fo + sz]
    for m in re.finditer(rb"[\xe8\xe9]", code):
        i = m.start()
        if i + 5 > len(code): break
        rel = struct.unpack_from("<i", code, i + 1)[0]
        if base + i + 5 + rel == t:
            print(f"  {'call' if code[i] == 0xe8 else 'jmp '} from {base + i:#x} {nm(base + i)}")
    needle = struct.pack("<Q", t)
    for s in segs:
        if s is text: continue
        for m in re.finditer(re.escape(needle), b[s[2]:s[2] + s[3]]):
            a = s[0] + m.start(); print(f"  data ptr at {a:#x} {nm(a)}")

if __name__ == "__main__":
    main()
