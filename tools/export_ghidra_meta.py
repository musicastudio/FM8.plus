#!/usr/bin/env python3
"""Export symbols and cross-references from an analyzed Ghidra project into decomp.db.

Run after tools/build_fm8_ghidra.py has finished for that binary (the project must not
be open elsewhere). Adds two tables to decomp.db:

    symbols(address, name, kind)             every named symbol (functions, labels, vtables, data)
    xrefs(from_addr, from_func, to_addr, ref_type)   all references

    set JAVA_HOME=C:\\Program Files\\Java\\jdk-26.0.2
    python tools/export_ghidra_meta.py vst2
"""
from __future__ import annotations

import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from build_fm8_ghidra import GHIDRA_INSTALL_DIR, VERSIONS, log  # noqa: E402


def export(v):
    import pyghidra
    db = v["analysis"] / "decomp.db"
    with pyghidra.open_program(None, project_location=str(v["proj"]), project_name=v["proj_name"],
                               analyze=False, program_name=v["program"],
                               nested_project_location=False) as api:
        program = api.getCurrentProgram()
        conn = sqlite3.connect(str(db))
        conn.execute("DROP TABLE IF EXISTS symbols")
        conn.execute("DROP TABLE IF EXISTS xrefs")
        conn.execute("CREATE TABLE symbols (address INTEGER, name TEXT, kind TEXT)")
        conn.execute("CREATE TABLE xrefs (from_addr INTEGER, from_func INTEGER, to_addr INTEGER, ref_type TEXT)")

        t0 = time.time()
        rows = []
        for s in program.getSymbolTable().getAllSymbols(True):
            rows.append((int(s.getAddress().getOffset()), str(s.getName(True)), str(s.getSymbolType())))
        conn.executemany("INSERT INTO symbols VALUES (?,?,?)", rows)
        conn.execute("CREATE INDEX symbols_name ON symbols(name)")
        conn.execute("CREATE INDEX symbols_addr ON symbols(address)")
        conn.commit()
        log(f"  {len(rows)} symbols in {time.time()-t0:.0f}s")

        t0 = time.time()
        fm = program.getFunctionManager()
        rm = program.getReferenceManager()
        rows = []
        it = rm.getReferenceIterator(program.getMinAddress())
        while it.hasNext():
            r = it.next()
            fa = r.getFromAddress()
            f = fm.getFunctionContaining(fa)
            rows.append((int(fa.getOffset()), int(f.getEntryPoint().getOffset()) if f else None,
                         int(r.getToAddress().getOffset()), str(r.getReferenceType())))
            if len(rows) >= 200000:
                conn.executemany("INSERT INTO xrefs VALUES (?,?,?,?)", rows)
                conn.commit()
                rows.clear()
        conn.executemany("INSERT INTO xrefs VALUES (?,?,?,?)", rows)
        conn.execute("CREATE INDEX xrefs_to ON xrefs(to_addr)")
        conn.execute("CREATE INDEX xrefs_from_func ON xrefs(from_func)")
        conn.commit()
        n = conn.execute("SELECT COUNT(*) FROM xrefs").fetchone()[0]
        log(f"  {n} xrefs in {time.time()-t0:.0f}s")
        conn.close()


def main():
    keys = set(sys.argv[1:]) or {v["key"] for v in VERSIONS}
    import pyghidra
    pyghidra.start(install_dir=GHIDRA_INSTALL_DIR)
    for v in VERSIONS:
        if v["key"] in keys:
            log(f"export {v['key']}")
            export(v)


if __name__ == "__main__":
    main()
