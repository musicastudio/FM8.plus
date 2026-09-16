#!/usr/bin/env python3
"""Compose two address maps and report each rvas.h site in the target build.

    python tools/compose141.py --map vst2:vst64_141=a.tsv --map vst64_141:exe141=b.tsv --to exe141

Matching a 1.4.1 binary against the 1.4.1 x64 VST2 is far more reliable than matching it across
seven years to 1.4.6: same compiler, same source. So the site addresses travel 1.4.6 -> 1.4.1 x64
(verified by hand) and then sideways within 1.4.1. A second, direct map can be passed with
--check to flag any site where the two routes disagree.
"""
from __future__ import annotations
import argparse, re
from pathlib import Path

BASE = {"exe": 0x140000000, "vst2": 0x180000000, "vst3": 0x180000000,
        "exe141": 0x140000000, "vst64_141": 0x180000000, "vst32_141": 0x10000000}
COLUMN = {"exe": 0, "vst2": 1, "vst3": 2}


def load(path: str) -> dict[int, int]:
    out = {}
    for line in Path(path).read_text().splitlines()[1:]:
        p = line.split("\t")
        if len(p) >= 2:
            out[int(p[0], 0)] = int(p[1], 0)
    return out


def load_anchors(path: str) -> dict[int, int]:
    out = {}
    for line in Path(path).read_text().splitlines():
        f = line.split("#")[0].split()
        if len(f) >= 2:
            out[int(f[0], 0)] = int(f[1], 0)
    return out


def sites():
    pat = re.compile(r"constexpr\s+Site\s+(\w+)\s*=\s*\{([^}]*)\};")
    txt = (Path(__file__).resolve().parents[1] / "src/core/rvas.h").read_text(encoding="utf-8")
    for m in pat.finditer(txt):
        yield m.group(1), tuple(int(x.strip(), 0) for x in m.group(2).split(","))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", action="append", required=True, help="src:dst=file.tsv, applied in order")
    ap.add_argument("--anchors", action="append", default=[], help="src:dst=file.txt verified pairs, override the map")
    ap.add_argument("--to", required=True)
    ap.add_argument("--check", help="src:dst=file.tsv a direct map to cross-check the last hop")
    args = ap.parse_args()

    hops = []
    for spec in args.map:
        pair, path = spec.split("=", 1)
        s, d = pair.split(":")
        hops.append((s, d, load(path)))
    for spec in args.anchors:
        pair, path = spec.split("=", 1)
        s, d = pair.split(":")
        for i, (hs, hd, m) in enumerate(hops):
            if (hs, hd) == (s, d):
                m.update(load_anchors(path))
    direct = None
    if args.check:
        pair, path = args.check.split("=", 1)
        direct = (pair.split(":")[0], load(path))

    start = hops[0][0]
    col = COLUMN[start]
    print(f"{'site':<22} {args.to:<14} rva          route")
    for name, vals in sites():
        if not vals[col]:
            continue
        va = BASE[start] + vals[col]
        cur, ok = va, True
        for _, _, m in hops:
            if cur in m:
                cur = m[cur]
            else:
                ok = False
                break
        note = ""
        if ok and direct:
            dva = BASE[direct[0]] + vals[COLUMN[direct[0]]] if direct[0] in COLUMN else None
            if dva and dva in direct[1]:
                note = "agrees" if direct[1][dva] == cur else f"DISAGREES ({direct[1][dva]:#x})"
            else:
                note = "composed only"
        print(f"{name:<22} {cur:#x} " if ok else f"{name:<22} {'-':<14} ",
              f"{cur - BASE[args.to]:#08x}    {note}" if ok else "  UNMATCHED")


if __name__ == "__main__":
    main()
