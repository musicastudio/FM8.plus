#!/usr/bin/env python3
"""Query the FM8 decomp databases without Ghidra.

    python tools/q.py vst2 name FM8Midi          # functions whose name contains text
    python tools/q.py vst2 grep 'Arp On'         # functions whose decompiled C contains text (regex)
    python tools/q.py vst2 fn 0x1800bd090        # print one function (address or name)
    python tools/q.py vst2 callers FUN_1800bd090 # functions whose C mentions this name
    python tools/q.py vst2 callees FUN_1800bd090 # FUN_/names called from this function
    python tools/q.py vst2 sym Arpeggiator       # symbols (needs export_ghidra_meta.py)
    python tools/q.py vst2 xrefs 0x1800bd090     # xrefs to an address (needs export)
    python tools/q.py vst2 xfrom 0x1800bd090     # xrefs from a function body (needs export)
    python tools/q.py vst2 stats

Binary keys: exe, vst2, vst3 (1.4.6); exe141, vst64_141, vst32_141 (1.4.1). Add -n N to cap rows (default 40), -f to print full bodies.
"""
from __future__ import annotations

import argparse
import os
import re
import sqlite3
import sys
from pathlib import Path

# The Ghidra projects live outside the repo (see README "Layout"). Override with FM8_DISASM.
ROOT = Path(os.environ.get("FM8_DISASM", Path(__file__).resolve().parents[1].parent / "FM8_DISASM"))
DBS = {"exe": "FM8_EXE_GHIDRA_ANALYSIS", "vst2": "FM8_VST2_GHIDRA_ANALYSIS", "vst3": "FM8_VST3_GHIDRA_ANALYSIS",
       "exe141": "FM8_141_EXE_GHIDRA_ANALYSIS", "vst64_141": "FM8_141_VST_64_GHIDRA_ANALYSIS",
       "vst32_141": "FM8_141_VST_32_GHIDRA_ANALYSIS"}


def connect(key: str) -> sqlite3.Connection:
    db = ROOT / DBS[key] / "decomp.db"
    c = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True)
    c.row_factory = sqlite3.Row
    return c


def has_table(c, name) -> bool:
    return c.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)).fetchone() is not None


def to_addr(s: str) -> int:
    return int(s, 16) if s.lower().startswith("0x") else int(s, 16) if re.fullmatch(r"[0-9a-fA-F]{8,}", s) else int(s)


def resolve(c, ident: str):
    """Row for an address (0x..), a FUN_ name, or an exact/unique function name."""
    if ident.lower().startswith("0x") or re.fullmatch(r"[0-9a-fA-F]{9,}", ident):
        return c.execute("SELECT * FROM decompilations WHERE address=?", (to_addr(ident),)).fetchone()
    m = re.fullmatch(r"(?:FUN|thunk_FUN)_([0-9a-fA-F]+)", ident)
    if m:
        row = c.execute("SELECT * FROM decompilations WHERE address=?", (int(m.group(1), 16),)).fetchone()
        if row:
            return row
    rows = c.execute("SELECT * FROM decompilations WHERE name=?", (ident,)).fetchall()
    if len(rows) == 1:
        return rows[0]
    rows = c.execute("SELECT * FROM decompilations WHERE name LIKE ?", (f"%{ident}%",)).fetchall()
    return rows[0] if len(rows) == 1 else None


def head(row, full=False) -> str:
    body = row["raw_decomp"] or f"<{row['status']}: {row['error']}>"
    if not full:
        body = "\n".join(body.splitlines()[:3])
    return f"{row['address']:#x} {row['name']} size={row['size']}\n{body}\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("key", choices=DBS)
    ap.add_argument("cmd")
    ap.add_argument("arg", nargs="?")
    ap.add_argument("-n", type=int, default=40)
    ap.add_argument("-f", action="store_true", help="full bodies")
    a = ap.parse_args()
    c = connect(a.key)

    if a.cmd == "stats":
        p = c.execute("SELECT * FROM progress").fetchone()
        n = c.execute("SELECT COUNT(*) FROM decompilations WHERE status='decompiled'").fetchone()[0]
        e = c.execute("SELECT COUNT(*) FROM decompilations WHERE status='error'").fetchone()[0]
        print(f"total={p['total_functions']} decompiled={n} errors={e}")
        for t in ("symbols", "xrefs"):
            if has_table(c, t):
                print(f"{t}: {c.execute(f'SELECT COUNT(*) FROM {t}').fetchone()[0]} rows")
    elif a.cmd == "name":
        for r in c.execute("SELECT * FROM decompilations WHERE name LIKE ? ORDER BY address LIMIT ?", (f"%{a.arg}%", a.n)):
            print(head(r, a.f))
    elif a.cmd == "grep":
        rx = re.compile(a.arg)
        shown = 0
        for r in c.execute("SELECT * FROM decompilations WHERE status='decompiled' AND raw_decomp LIKE ? ORDER BY address",
                           (f"%{re.sub(r'[\\\[\]().*+?^$|{}]', '%', a.arg)}%",)):
            if rx.search(r["raw_decomp"]):
                lines = [l for l in r["raw_decomp"].splitlines() if rx.search(l)]
                print(f"{r['address']:#x} {r['name']} size={r['size']}")
                for l in (r["raw_decomp"].splitlines() if a.f else lines[:5]):
                    print("   ", l.strip())
                shown += 1
                if shown >= a.n:
                    print("... (capped, use -n)")
                    break
    elif a.cmd == "fn":
        r = resolve(c, a.arg)
        print(head(r, True) if r else f"not found: {a.arg}")
    elif a.cmd == "callers":
        r = resolve(c, a.arg)
        name = r["name"] if r else a.arg
        names = {name}
        if r:
            names.add(f"FUN_{r['address']:x}")
        shown = 0
        for nm in names:
            for x in c.execute("SELECT address,name,size FROM decompilations WHERE status='decompiled' AND raw_decomp LIKE ? ORDER BY address",
                               (f"%{nm}(%",)):
                print(f"{x['address']:#x} {x['name']} size={x['size']}")
                shown += 1
                if shown >= a.n:
                    print("... (capped)"); return
    elif a.cmd == "callees":
        r = resolve(c, a.arg)
        if not r:
            print("not found"); return
        seen = []
        for m in re.finditer(r"\b((?:thunk_)?FUN_[0-9a-f]+|[A-Za-z_][A-Za-z0-9_:]*)\(", r["raw_decomp"]):
            nm = m.group(1)
            if nm not in seen and nm not in ("if", "while", "for", "switch", "return", "sizeof"):
                seen.append(nm)
        for nm in seen:
            t = c.execute("SELECT address,name,size FROM decompilations WHERE name=?", (nm,)).fetchone()
            print(f"{nm}  -> {t['address']:#x} size={t['size']}" if t else nm)
    elif a.cmd == "sym":
        if not has_table(c, "symbols"):
            print("no symbols table; run tools/export_ghidra_meta.py"); return
        for r in c.execute("SELECT * FROM symbols WHERE name LIKE ? ORDER BY address LIMIT ?", (f"%{a.arg}%", a.n)):
            print(f"{r['address']:#x} {r['kind']:<8} {r['name']}")
    elif a.cmd in ("xrefs", "xfrom"):
        if not has_table(c, "xrefs"):
            print("no xrefs table; run tools/export_ghidra_meta.py"); return
        r = resolve(c, a.arg)
        addr = r["address"] if r else to_addr(a.arg)
        if a.cmd == "xrefs":
            q = "SELECT * FROM xrefs WHERE to_addr=? ORDER BY from_addr LIMIT ?"
        else:
            q = "SELECT * FROM xrefs WHERE from_func=? ORDER BY from_addr LIMIT ?"
        for x in c.execute(q, (addr, a.n)):
            fn = c.execute("SELECT name FROM decompilations WHERE address=?", (x["from_func"],)).fetchone()
            where = fn["name"] if fn else (f"{x['from_func']:#x}" if x["from_func"] is not None else "(none)")
            print(f"{x['from_addr']:#x} in {where} -> {x['to_addr']:#x} {x['ref_type']}")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
