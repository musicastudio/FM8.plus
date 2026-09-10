#!/usr/bin/env python3
"""Import, auto-analyze, and batch-decompile the FM8 binaries with pyghidra.

Adapted from a prior build script. One JVM per process; run
with --only to process a single binary so several can run in parallel.

    python tools/build_fm8_ghidra.py --dry-run
    python tools/build_fm8_ghidra.py --only exe
"""
from __future__ import annotations

import argparse
import io
import json
import sqlite3
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

if sys.stdout.encoding != "utf-8":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

GHIDRA_INSTALL_DIR = r"E:\ghidra_12.1.2_PUBLIC"
ROOT = Path(r"E:\Sync\James Hansen\Projects\Musica.Studio\FM8_DISASM")

VERSIONS = [
    dict(key="exe", proj_name="FM8_EXE", program="FM8.exe",
         binary=ROOT / "FM8_EXE" / "FM8.exe",
         proj=ROOT / "FM8_EXE_GHIDRA_PROJ", analysis=ROOT / "FM8_EXE_GHIDRA_ANALYSIS"),
    dict(key="vst3", proj_name="FM8_VST3", program="FM8.vst3",
         binary=ROOT / "FM8_VST3" / "FM8.vst3",
         proj=ROOT / "FM8_VST3_GHIDRA_PROJ", analysis=ROOT / "FM8_VST3_GHIDRA_ANALYSIS"),
    dict(key="vst2", proj_name="FM8_VST2", program="FM8.dll",
         binary=ROOT / "FM8_VST_64" / "FM8.dll",
         proj=ROOT / "FM8_VST2_GHIDRA_PROJ", analysis=ROOT / "FM8_VST2_GHIDRA_ANALYSIS"),
]


def log(msg: str) -> None:
    print(f"[{datetime.now():%H:%M:%S}] {msg}", flush=True)


def analysis_done_path(v) -> Path:
    return v["analysis"] / "ghidra_analysis_done.json"


def is_complete(v) -> bool:
    db = v["analysis"] / "decomp.db"
    if not db.exists():
        return False
    try:
        c = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True)
        n = c.execute("SELECT COUNT(*) FROM decompilations WHERE status='decompiled'").fetchone()[0]
        c.close()
        return n > 1000
    except Exception:
        return False


def project_exists(v) -> bool:
    return (v["proj"] / f"{v['proj_name']}.gpr").exists() and (v["proj"] / f"{v['proj_name']}.rep").exists()


def count_functions(program) -> int:
    return sum(1 for _ in program.getFunctionManager().getFunctions(True))


def init_db(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path))
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("""
        CREATE TABLE IF NOT EXISTS decompilations (
            address INTEGER PRIMARY KEY, name TEXT, size INTEGER,
            raw_decomp TEXT, status TEXT DEFAULT 'pending', error TEXT,
            decomp_time_ms INTEGER, created_at TEXT DEFAULT (datetime('now')))""")
    conn.execute("""
        CREATE TABLE IF NOT EXISTS progress (
            id INTEGER PRIMARY KEY CHECK (id = 1), total_functions INTEGER,
            completed INTEGER DEFAULT 0, errors INTEGER DEFAULT 0, updated_at TEXT)""")
    conn.execute("INSERT OR IGNORE INTO progress (id, total_functions) VALUES (1, 0)")
    conn.commit()
    return conn


def analyze(program, v):
    from ghidra.app.plugin.core.analysis import AutoAnalysisManager
    from ghidra.app.script import GhidraScriptUtil
    from ghidra.util.task import ConsoleTaskMonitor

    mgr = AutoAnalysisManager.getAnalysisManager(program)
    GhidraScriptUtil.acquireBundleHostReference()
    t0 = time.time()
    try:
        mgr.initializeOptions()
        mgr.reAnalyzeAll(None)
        mgr.startAnalysis(ConsoleTaskMonitor())
    finally:
        GhidraScriptUtil.releaseBundleHostReference()
    n = count_functions(program)
    analysis_done_path(v).parent.mkdir(parents=True, exist_ok=True)
    analysis_done_path(v).write_text(json.dumps({
        "status": "complete", "binary": str(v["binary"]), "project_dir": str(v["proj"]),
        "functions": n, "duration_seconds": round(time.time() - t0, 1),
        "completed_at": datetime.now().isoformat()}, indent=2))
    log(f"  analysis done: {n} functions in {timedelta(seconds=int(time.time()-t0))}")


def decompile(program, v):
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor

    conn = init_db(v["analysis"] / "decomp.db")
    try:
        done = {r[0] for r in conn.execute("SELECT address FROM decompilations WHERE status='decompiled'")}
        funcs = list(program.getFunctionManager().getFunctions(True))
        pending = [f for f in funcs if int(f.getEntryPoint().getOffset()) not in done]
        conn.execute("UPDATE progress SET total_functions=? WHERE id=1", (len(funcs),))
        conn.commit()
        log(f"  decompile: {len(pending)} pending of {len(funcs)}")
        if pending:
            di = DecompInterface()
            di.openProgram(program)
            cmon = ConsoleTaskMonitor()
            ok = err = 0
            t_start = time.time()
            try:
                for func in pending:
                    addr = int(func.getEntryPoint().getOffset())
                    name = str(func.getName())
                    size = int(func.getBody().getNumAddresses())
                    t = time.time()
                    try:
                        res = di.decompileFunction(func, max(30, min(120, size // 100)), cmon)
                        ms = int((time.time() - t) * 1000)
                        if res.decompileCompleted():
                            conn.execute("INSERT OR REPLACE INTO decompilations "
                                "(address,name,size,raw_decomp,status,decomp_time_ms,created_at) "
                                "VALUES (?,?,?,?,'decompiled',?,datetime('now'))",
                                (addr, name, size, str(res.getDecompiledFunction().getC()), ms))
                            ok += 1
                        else:
                            conn.execute("INSERT OR REPLACE INTO decompilations "
                                "(address,name,size,status,error,decomp_time_ms,created_at) "
                                "VALUES (?,?,?,'error',?,?,datetime('now'))",
                                (addr, name, size, res.getErrorMessage() or "unknown", ms))
                            err += 1
                    except Exception as e:
                        conn.execute("INSERT OR REPLACE INTO decompilations "
                            "(address,name,size,status,error,decomp_time_ms,created_at) "
                            "VALUES (?,?,?,'error',?,?,datetime('now'))",
                            (addr, name, size, str(e), int((time.time() - t) * 1000)))
                        err += 1
                    if (ok + err) % 500 == 0:
                        conn.execute("UPDATE progress SET completed=?, errors=?, updated_at=datetime('now') WHERE id=1",
                                     (len(done) + ok, err))
                        conn.commit()
                        el = time.time() - t_start
                        rate = (ok + err) / el
                        left = (len(pending) - ok - err) / max(rate, 0.01)
                        log(f"  {ok+err}/{len(pending)} ({err} err), {rate:.1f}/s, eta {timedelta(seconds=int(left))}")
            finally:
                di.dispose()
            conn.execute("UPDATE progress SET completed=?, errors=?, updated_at=datetime('now') WHERE id=1",
                         (len(done) + ok, err))
            conn.commit()
            log(f"  decompile done: {ok} ok, {err} err")
        (v["analysis"] / "decompile_done.json").write_text(json.dumps({
            "status": "complete", "project_dir": str(v["proj"]),
            "db": str(v["analysis"] / "decomp.db"), "total": len(funcs),
            "completed_at": datetime.now().isoformat()}, indent=2))
    finally:
        conn.close()


def process(v):
    import pyghidra
    v["analysis"].mkdir(parents=True, exist_ok=True)
    first_import = not project_exists(v)
    log(f"FM8 {v['key']}: {'IMPORT+ANALYZE' if first_import else 'open existing'} -> decompile")
    with pyghidra.open_program(
        str(v["binary"]) if first_import else None,
        project_location=str(v["proj"]), project_name=v["proj_name"],
        analyze=False, program_name=None if first_import else v["program"],
        nested_project_location=False,
    ) as flat_api:
        program = flat_api.getCurrentProgram()
        if analysis_done_path(v).exists() and count_functions(program) > 1000:
            log("  analysis already done, skipping")
        else:
            analyze(program, v)
        decompile(program, v)
    log(f"FM8 {v['key']}: complete")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="comma list of keys: exe,vst3,vst2")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    keys = {k.strip() for k in args.only.split(",") if k.strip()}
    todo = [v for v in VERSIONS if not keys or v["key"] in keys]
    pending = []
    for v in todo:
        if is_complete(v):
            print(f"  {v['key']:<5} DONE     -> {v['analysis'] / 'decomp.db'}")
        elif not v["binary"].exists():
            print(f"  {v['key']:<5} MISSING  -> {v['binary']}")
        else:
            state = "skip" if analysis_done_path(v).exists() else "yes"
            print(f"  {v['key']:<5} PENDING  -> analyze={state}, decompile=yes")
            pending.append(v)
    if args.dry_run or not pending:
        return
    import pyghidra
    log("Starting pyghidra JVM...")
    pyghidra.start(install_dir=GHIDRA_INSTALL_DIR)
    t0 = time.time()
    for v in pending:
        process(v)
    print(f"\nAll done in {timedelta(seconds=int(time.time() - t0))}.")


if __name__ == "__main__":
    main()
