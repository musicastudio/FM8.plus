#!/usr/bin/env python3
"""Match functions across two FM8 builds: normalized-body and string seeds, then call-graph growth.

    python tools/bindiff141.py vst2 vst64_141                  # match, then report the rvas.h sites
    python tools/bindiff141.py vst2 vst64_141 --save map.tsv    # write the whole mapping
    python tools/bindiff141.py exe exe141 --rounds 8

Text fingerprints alone cannot separate the small helper functions: hundreds of them share the
same handful of tokens. So seed only on evidence that is unique on both sides (a normalized
function body, or a set of string literals), then grow along the call graph, where a candidate
pair is judged by how many of their callees and callers are already matched to each other. That
is the property a 7-year-old rebuild preserves even when the code around it moved.

Prints, per rvas.h site, the matched address and how it was reached. `seed` and a high `via`
count are solid; `weak` means the tie was broken on body similarity alone. Verify before use.
"""
from __future__ import annotations

import argparse
import hashlib
import math
import os
import re
import sqlite3
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(os.environ.get("FM8_DISASM", Path(__file__).resolve().parents[1].parent / "FM8_DISASM"))
DBS = {"exe": "FM8_EXE_GHIDRA_ANALYSIS", "vst2": "FM8_VST2_GHIDRA_ANALYSIS", "vst3": "FM8_VST3_GHIDRA_ANALYSIS",
       "exe141": "FM8_141_EXE_GHIDRA_ANALYSIS", "vst64_141": "FM8_141_VST_64_GHIDRA_ANALYSIS",
       "vst32_141": "FM8_141_VST_32_GHIDRA_ANALYSIS"}
BASE = {"exe": 0x140000000, "vst2": 0x180000000, "vst3": 0x180000000,
        "exe141": 0x140000000, "vst64_141": 0x180000000, "vst32_141": 0x10000000}
COLUMN = {"exe": 0, "vst2": 1, "vst3": 2}

RE_CALL = re.compile(r"\bFUN_([0-9a-f]+)\b")
RE_STR = re.compile(r'"((?:[^"\\]|\\.){2,})"')
RE_SYM = re.compile(r"\b([A-Z][A-Za-z0-9_]{4,})\b")  # API imports and class-ish names
# Everything a rebuild is free to renumber. What survives is the shape.
RE_NORM = [(re.compile(r"\bFUN_[0-9a-f]+\b"), "C"), (re.compile(r"\b(?:DAT|PTR|UNK|LAB|SUB)_[0-9a-f]+\b"), "D"),
           (re.compile(r"\b(?:local|param|in|unaff|extraout)_[0-9a-zA-Z_]+\b"), "L"),
           (re.compile(r"\b[a-z]{1,2}Var\d+\b"), "V"), (re.compile(r"\b0x[0-9a-f]{6,}\b"), "A"),
           (re.compile(r"\s+"), " ")]


def connect(key: str) -> sqlite3.Connection:
    db = ROOT / DBS[key] / "decomp.db"
    if not db.exists():
        sys.exit(f"no decomp.db for {key}: {db}")
    c = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True)
    c.row_factory = sqlite3.Row
    return c


def normalize(c: str) -> str:
    for pat, rep in RE_NORM:
        c = pat.sub(rep, c)
    return c.strip()


class Build:
    def __init__(self, key: str):
        self.key = key
        self.size: dict[int, int] = {}
        self.callees: dict[int, set[int]] = {}
        self.callers: dict[int, set[int]] = defaultdict(set)
        self.body: dict[int, str] = {}      # normalized body hash
        self.feats: dict[int, set[str]] = {}
        conn = connect(key)
        for r in conn.execute("SELECT address, size, raw_decomp FROM decompilations "
                              "WHERE status='decompiled' AND raw_decomp IS NOT NULL"):
            a, c = r["address"], r["raw_decomp"]
            self.size[a] = r["size"]
            self.body[a] = hashlib.md5(normalize(c).encode("utf8", "replace")).hexdigest()
            self.callees[a] = {int(x, 16) for x in RE_CALL.findall(c)} - {a}
            self.feats[a] = ({"s:" + m for m in RE_STR.findall(c)} |
                             {"y:" + m for m in RE_SYM.findall(c)})
        conn.close()
        for a, cs in self.callees.items():
            for c in cs:
                if c in self.size:
                    self.callers[c].add(a)
        df: dict[str, int] = defaultdict(int)
        for f in self.feats.values():
            for t in f:
                df[t] += 1
        n = len(self.size)
        self.idf = {t: math.log(n / d) for t, d in df.items()}

    def __len__(self):
        return len(self.size)


def unique_index(d: dict[int, str]) -> dict[str, int]:
    """key -> address, keeping only keys that belong to exactly one function."""
    seen: dict[str, int | None] = {}
    for a, k in d.items():
        seen[k] = None if k in seen else a
    return {k: a for k, a in seen.items() if a is not None}


def seed(src: Build, dst: Build) -> dict[int, int]:
    """Pairs justified on their own: same normalized body, or the same unique set of strings."""
    m: dict[int, int] = {}
    si, di = unique_index(src.body), unique_index(dst.body)
    for k, a in si.items():
        if k in di:
            m[a] = di[k]
    def strkey(b: Build) -> dict[int, str]:
        out = {}
        for a, f in b.feats.items():
            s = sorted(t for t in f if t.startswith("s:"))
            if len("".join(s)) >= 24:  # enough text to be distinctive
                out[a] = "".join(s)
        return out
    si, di = unique_index(strkey(src)), unique_index(strkey(dst))
    for k, a in si.items():
        if k in di and a not in m:
            m[a] = di[k]
    return m


def sim(src: Build, dst: Build, a: int, b: int) -> float:
    """Feature agreement, IDF-weighted, plus a size-ratio term. 0..1-ish."""
    fa, fb = src.feats[a], dst.feats[b]
    if fa or fb:
        shared = sum(src.idf.get(t, 1.0) for t in fa & fb)
        total = sum(src.idf.get(t, 1.0) for t in fa | fb) or 1.0
        f = shared / total
    else:
        f = 0.0
    sa, sb = src.size[a], dst.size[b]
    return 0.7 * f + 0.3 * (min(sa, sb) / max(sa, sb, 1))


def grow(src: Build, dst: Build, m: dict[int, int], rounds: int, min_votes: int = 2):
    """Spread matches along the call graph: neighbours of a matched pair vote for each other."""
    how = {a: ("seed", 0) for a in m}
    taken = set(m.values())
    for rnd in range(rounds):
        votes: dict[tuple[int, int], int] = defaultdict(int)
        for a, b in m.items():
            for sa, sb in ((src.callees.get(a, ()), dst.callees.get(b, ())),
                           (src.callers.get(a, ()), dst.callers.get(b, ()))):
                for x in sa:
                    if x in m or x not in src.size:
                        continue
                    for y in sb:
                        if y in taken or y not in dst.size:
                            continue
                        votes[(x, y)] += 1
        cand: dict[int, list[tuple[float, int, int]]] = defaultdict(list)
        for (x, y), v in votes.items():
            if v >= min_votes:
                cand[x].append((v + sim(src, dst, x, y), v, y))
        added = 0
        for x, lst in sorted(cand.items(), key=lambda kv: -max(s for s, _, _ in kv[1])):
            lst.sort(reverse=True)
            score, v, y = lst[0]
            runner = lst[1][0] if len(lst) > 1 else 0.0
            if y in taken or x in m:
                continue
            if score - runner < 0.15 and v < 4:   # ambiguous: leave it for a later round
                continue
            m[x], how[x] = y, (f"via{v}" if score - runner > 0.5 else "weak", v)
            taken.add(y)
            added += 1
        print(f"# round {rnd+1}: +{added} -> {len(m)} matched", file=sys.stderr)
        if not added:
            break
    return m, how


def explain(src: Build, dst: Build, m: dict[int, int], va: int, top: int = 6):
    """Why a function stayed unmatched: rank candidates the matched neighbours point at."""
    rev = {b: a for a, b in m.items()}
    votes: dict[int, list[str]] = defaultdict(list)
    for tag, sset, pick in (("callee", src.callees.get(va, set()), dst.callers),
                            ("caller", src.callers.get(va, set()), dst.callees)):
        for nb in sset:
            if nb not in m:
                continue
            for y in pick.get(m[nb], ()):
                if y in dst.size and y not in rev:
                    votes[y].append(f"{tag} {nb:#x}")
    if not votes:
        print(f"  {va:#x}: no matched neighbour, nothing to go on")
        return
    ranked = sorted(votes.items(), key=lambda kv: (-len(kv[1]), -sim(src, dst, va, kv[0])))[:top]
    for y, why in ranked:
        print(f"  {y:#x} {dst.size[y]:>6}b  votes={len(why):<3} sim={sim(src, dst, va, y):.2f}  {', '.join(why[:3])}")


def parse_sites(path: Path):
    pat = re.compile(r"constexpr\s+Site\s+(\w+)\s*=\s*\{([^}]*)\};(?:\s*//\s*(.*))?")
    for m in pat.finditer(path.read_text(encoding="utf-8")):
        yield m.group(1), tuple(int(x.strip(), 0) for x in m.group(2).split(",")), (m.group(3) or "").strip()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("src", choices=list(DBS))
    ap.add_argument("dst", choices=list(DBS))
    ap.add_argument("--rounds", type=int, default=6)
    ap.add_argument("--save", help="write the full address map as TSV")
    ap.add_argument("--explain", action="store_true", help="list candidates for every unmatched site")
    ap.add_argument("--anchors", help="file of verified 'src_va dst_va' pairs to pin the graph")
    args = ap.parse_args()

    print(f"# loading {args.src} ...", file=sys.stderr)
    src = Build(args.src)
    print(f"# loading {args.dst} ...", file=sys.stderr)
    dst = Build(args.dst)
    m = seed(src, dst)
    if args.anchors:
        pinned = 0
        for line in Path(args.anchors).read_text().splitlines():
            line = line.split("#")[0].split()
            if len(line) >= 2:
                m[int(line[0], 0)] = int(line[1], 0)
                pinned += 1
        print(f"# {pinned} verified anchors pinned", file=sys.stderr)
    print(f"# {len(src)} vs {len(dst)} functions, {len(m)} seeds", file=sys.stderr)
    m, how = grow(src, dst, m, args.rounds)

    if args.save:
        with open(args.save, "w", encoding="utf8") as fh:
            fh.write("src_va\tdst_va\thow\tvotes\n")
            for a, b in sorted(m.items()):
                h, v = how[a]
                fh.write(f"{a:#x}\t{b:#x}\t{h}\t{v}\n")
        print(f"# wrote {args.save}", file=sys.stderr)

    if args.src not in COLUMN:   # no rvas.h column for a 1.4.1 source; the map file is the output
        return
    col, sbase, dbase = COLUMN[args.src], BASE[args.src], BASE[args.dst]
    print(f"\n{'site':<22} {args.src:<14} {args.dst:<14} how      rva")
    for name, vals, note in parse_sites(Path(__file__).resolve().parents[1] / "src/core/rvas.h"):
        if not vals[col]:
            continue
        va = sbase + vals[col]
        if va in m:
            b = m[va]
            h, v = how[va]
            print(f"{name:<22} {va:#x}    {b:#x}    {h:<8} {b - dbase:#08x}")
        else:
            print(f"{name:<22} {va:#x}    {'-':<14} UNMATCHED")
            if args.explain:
                explain(src, dst, m, va)


if __name__ == "__main__":
    main()
