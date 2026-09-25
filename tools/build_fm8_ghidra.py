#!/usr/bin/env python3
"""Import, auto-analyze, and batch-decompile the FM8 binaries with pyghidra.

    python tools/build_fm8_ghidra.py --dry-run
    python tools/build_fm8_ghidra.py --only exe
"""
from __future__ import annotations

import argparse
import io
import json
import os
import sqlite3
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

if sys.stdout.encoding != "utf-8":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

# Both are overridable by environment variable; the defaults match the README "Layout" (the Ghidra
# projects and FM8 binaries live in a sibling FM8_DISASM folder, outside this repo).
GHIDRA_INSTALL_DIR = os.environ.get("GHIDRA_INSTALL_DIR", r"C:\ghidra_12.1.2_PUBLIC")
ROOT = Path(os.environ.get("FM8_DISASM", Path(__file__).resolve().parents[1].parent / "FM8_DISASM"))

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
    # 1.4.1 (R1599, 2015-10-20), the last release with a 32-bit plugin. No VST3 existed yet.
    dict(key="exe141", proj_name="FM8_141_EXE", program="FM8.exe",
         binary=ROOT / "FM8_141_EXE" / "FM8.exe",
         proj=ROOT / "FM8_141_EXE_GHIDRA_PROJ", analysis=ROOT / "FM8_141_EXE_GHIDRA_ANALYSIS"),
    dict(key="vst64_141", proj_name="FM8_141_VST_64", program="FM8.dll",
         binary=ROOT / "FM8_141_VST_64" / "FM8.dll",
         proj=ROOT / "FM8_141_VST_64_GHIDRA_PROJ", analysis=ROOT / "FM8_141_VST_64_GHIDRA_ANALYSIS"),
    dict(key="vst32_141", proj_name="FM8_141_VST_32", program="FM8.dll",
         binary=ROOT / "FM8_141_VST_32" / "FM8.dll",
         proj=ROOT / "FM8_141_VST_32_GHIDRA_PROJ", analysis=ROOT / "FM8_141_VST_32_GHIDRA_ANALYSIS"),
    # 1.0.3 (2007-10-18), the last release with a DXi. The DXi and the x86 VST2 are the same build,
    # so diffing them isolates NI's DXi layer.
    dict(key="dxi103", proj_name="FM8_103_DXi", program="FM8DXi.dll",
         binary=ROOT / "FM8_103_DXi" / "FM8DXi.dll",
         proj=ROOT / "FM8_103_DXi_GHIDRA_PROJ", analysis=ROOT / "FM8_103_DXi_GHIDRA_ANALYSIS"),
    dict(key="vst32_103", proj_name="FM8_103_VST_32", program="FM8.dll",
         binary=ROOT / "FM8_103_VST_32" / "FM8.dll",
         proj=ROOT / "FM8_103_VST_32_GHIDRA_PROJ", analysis=ROOT / "FM8_103_VST_32_GHIDRA_ANALYSIS"),
]

# macOS 1.4.6. Universal bundles are split into thin FM8.<arch> slices beside the original first
# (tools/macho_thin.py); the VST2 is x86_64-only. The binaries keep their full C++ symbol table.
_MAC = {"vst": "FM8.vst", "vst3": "FM8.vst3", "au": "FM8.component", "aax": "FM8.aaxplugin", "app": "FM8.app"}
for _t, _bundle in _MAC.items():
    for _arch in ("x86_64", "arm64"):
        if _t == "vst" and _arch == "arm64":
            continue
        _name = f"FM8_MAC_146_{_t.upper()}" + ("" if _arch == "x86_64" else "_ARM64")
        _bin = "FM8" if _t == "vst" else f"FM8.{_arch}"
        VERSIONS.append(dict(key=f"mac_{_t}" + ("" if _arch == "x86_64" else "_arm64"), proj_name=_name,
                             program=_bin,
                             binary=ROOT / f"FM8_MAC_146_{_t.upper()}" / _bundle / "Contents" / "MacOS" / _bin,
                             proj=ROOT / f"{_name}_GHIDRA_PROJ", analysis=ROOT / f"{_name}_GHIDRA_ANALYSIS"))


VTABLES = False


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


def define_vtable_functions(program) -> int:
    """Auto-analysis misses functions reached only through a vtable (common in the DXi layer).
    Define one at every slot of each run of >= 3 code pointers in the data sections."""
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    mem, fm, af = program.getMemory(), program.getFunctionManager(), program.getAddressFactory()
    ptr = program.getDefaultPointerSize()
    code = [b for b in mem.getBlocks() if b.isExecute()]
    in_code = lambda off: any(b.getStart().getOffset() <= off <= b.getEnd().getOffset() for b in code)
    targets = set()
    import jpype
    for b in mem.getBlocks():
        if b.isExecute() or not b.isInitialized() or str(b.getName()).startswith(".rsrc"):
            continue
        buf = jpype.JArray(jpype.JByte)(int(b.getSize())); b.getBytes(b.getStart(), buf)
        data = memoryview(buf).tobytes()
        run = []
        for i in range(0, len(data) - ptr + 1, ptr):
            val = int.from_bytes(data[i:i + ptr], "little")
            if in_code(val):
                run.append(val)
                continue
            if len(run) >= 3:
                targets.update(run)
            run = []
    made = 0
    tx = program.startTransaction("vtable functions")
    try:
        for off in sorted(targets):
            a = af.getDefaultAddressSpace().getAddress(off)
            if fm.getFunctionContaining(a) is not None:
                continue
            DisassembleCommand(a, None, True).applyTo(program)
            if CreateFunctionCmd(a).applyTo(program):
                made += 1
    finally:
        program.endTransaction(tx, True)
    log(f"  vtable pass: {made} functions defined from {len(targets)} vtable slots")
    return made


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
        if VTABLES:
            define_vtable_functions(program)
        decompile(program, v)
    log(f"FM8 {v['key']}: complete")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="comma list of keys: exe,vst3,vst2,exe141,vst64_141,vst32_141,dxi103,vst32_103,mac_vst,mac_vst3,mac_au,mac_aax,mac_app (+_arm64)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--vtables", action="store_true", help="define vtable-only functions, then decompile them")
    args = ap.parse_args()
    global VTABLES
    VTABLES = args.vtables
    keys = {k.strip() for k in args.only.split(",") if k.strip()}
    todo = [v for v in VERSIONS if not keys or v["key"] in keys]
    pending = []
    for v in todo:
        if is_complete(v) and not VTABLES:
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
