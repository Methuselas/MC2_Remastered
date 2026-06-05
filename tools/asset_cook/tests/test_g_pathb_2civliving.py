#!/usr/bin/env python3
"""tools/asset_cook/tests/test_g_pathb_2civliving.py — Track G path B parity gate.

Path B = stock .tgl -> (workbench --export-tgl-meshdump) -> meshdump.json ->
tglmeshdump_to_glb (axis-inverted) -> stage. The OFFLINE PARITY ORACLE: the
staged engine-space EXTENTS must reproduce the stock MeshData extents (the
importer round-trips the inversion; footprint + height preserved, Y re-grounded).

This proves the convention is correct for a REAL non-symmetric building
(2civliving) — bigbox was a symmetric cube and could hide axis errors.

  py -3 tools/asset_cook/tests/test_g_pathb_2civliving.py
Exit 0 = pass. Uses the committed meshdump fixture (no workbench rebuild needed).
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
COOK = HERE.parent / "trackg_cook.py"
TOGLB = HERE.parent / "tglmeshdump_to_glb.py"
DUMP = HERE / "fixtures" / "2civliving_meshdump.json"


def run(*a):
    return subprocess.run([sys.executable, *map(str, a)], capture_output=True, text=True)


def ext(mn, mx):
    return [round(mx[i] - mn[i], 2) for i in range(3)]


def main() -> int:
    if not DUMP.exists():
        print(f"SKIP test_g_pathb_2civliving (fixture missing: {DUMP})")
        return 0
    dump = json.loads(DUMP.read_text())
    stock_ext = ext(dump["bmin"], dump["bmax"])

    errs = []
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        glb = td / "2civliving.glb"
        r = run(TOGLB, DUMP, glb)
        if r.returncode != 0:
            print("FAIL toglb:\n" + r.stdout + r.stderr); return 1
        r = run(COOK, "stage", glb, td / "out", "--id", "2civliving",
                "--class", "staticprop", "--appearance", "2civliving")
        if r.returncode != 0:
            print("FAIL stage:\n" + r.stdout + r.stderr); return 1
        b = json.loads((td / "out" / "staged.json").read_text())["geometry"]["bounds"]
        stage_ext = ext(b["min"], b["max"])

        if any(abs(stock_ext[i] - stage_ext[i]) > 0.05 for i in range(3)):
            errs.append(f"extent mismatch: stock {stock_ext} vs staged {stage_ext}")
        # Y must be the grounded (base-0) vertical axis after import
        if abs(b["min"][1]) > 0.01:
            errs.append(f"Y not grounded to 0: min.y={b['min'][1]}")

    if errs:
        print("FAIL test_g_pathb_2civliving:")
        for e in errs:
            print(f"  - {e}")
        return 1
    print(f"PASS test_g_pathb_2civliving (stock->meshdump->inverted-glb->stage reproduces "
          f"extents {stock_ext} [X,Y,Z], Y grounded to 0 — real-building convention correct)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
