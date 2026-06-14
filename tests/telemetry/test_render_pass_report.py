#!/usr/bin/env python3
"""Unit tests for scripts/render_pass_report.py (synthetic [RENDER_PASS] logs)."""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import render_pass_report as rpr  # noqa: E402

SAMPLE = """\
[SMOKE v1] event=banner mode=passive mission=mc2_01
[RENDER_PASS v1] frame=1 pass=ShadowCaster fbo=7 viewport=0,0,2048,2048 drawbuffers=0 phase=begin hint=renderStaticTerrainShadow
[RENDER_PASS v1] frame=1 pass=TerrainBase fbo=3 viewport=0,0,800,600 drawbuffers=2 phase=begin hint=gos_TerrainLodChunk_SubmitDrawCommands
[RENDER_PASS v1] frame=1 pass=Water fbo=3 viewport=0,0,800,600 drawbuffers=2 phase=begin hint=renderWaterFastPath
[RENDER_PASS v1] frame=1 pass=PostProcess fbo=0 viewport=0,0,800,600 drawbuffers=1 phase=begin hint=bloom
[RENDER_PASS v1] frame=301 pass=ShadowCaster fbo=7 viewport=0,0,2048,2048 drawbuffers=0 phase=begin hint=renderStaticTerrainShadow
[RENDER_PASS v1] frame=301 pass=TerrainBase fbo=3 viewport=0,0,800,600 drawbuffers=2 phase=begin hint=gos_TerrainLodChunk_SubmitDrawCommands
"""


def _write(tmp: Path, name: str, text: str) -> Path:
    p = tmp / name
    p.write_text(text, encoding="utf-8")
    return p


def test_parse_fields_and_order(tmp_path):
    log = _write(tmp_path, "mc2_01.log", SAMPLE)
    recs = rpr.parse_log(log)
    assert len(recs) == 6
    f1 = [r for r in recs if r["frame"] == 1]
    # order_in_frame follows line order within the frame
    assert [r["pass"] for r in sorted(f1, key=lambda r: r["order_in_frame"])] == \
        ["ShadowCaster", "TerrainBase", "Water", "PostProcess"]
    tb = next(r for r in f1 if r["pass"] == "TerrainBase")
    assert tb["fbo"] == 3 and tb["viewport"] == "0,0,800,600" and tb["drawbuffers"] == 2
    assert tb["hint"] == "gos_TerrainLodChunk_SubmitDrawCommands"


def test_build_report_rollup_and_stability(tmp_path):
    log = _write(tmp_path, "mc2_01.log", SAMPLE)
    recs, sources = rpr.collect(tmp_path)
    rep = rpr.build_report(tmp_path, recs, sources)
    assert rep["telemetry_present"] is True
    assert rep["record_count"] == 6
    assert set(rep["passes"]) == {"ShadowCaster", "TerrainBase", "Water", "PostProcess"}
    # TerrainBase appears in 2 frames, stable fbo/viewport/order
    tb = rep["passes"]["TerrainBase"]
    assert tb["frames"] == [1, 301]
    assert tb["fbo"]["stable"] and tb["fbo"]["values"] == [3]
    assert tb["order_in_frame"]["stable"] and tb["order_in_frame"]["values"] == [1]
    # gaps are listed, not invented
    gap_fields = {g["field"] for g in rep["gaps"]}
    assert {"draws", "gpu_ms", "clears"} <= gap_fields


def test_empty_log_no_crash(tmp_path):
    _write(tmp_path, "empty.log", "[SMOKE v1] nothing here\n")
    recs, sources = rpr.collect(tmp_path)
    rep = rpr.build_report(tmp_path, recs, sources)
    assert rep["telemetry_present"] is False
    assert rep["record_count"] == 0
    assert rep["telemetry_note"]


def test_compare_detects_changes(tmp_path):
    # baseline report
    base = _write(tmp_path, "base.log", SAMPLE)
    recs, sources = rpr.collect(base)
    base_rep = rpr.build_report(base, recs, sources)
    import json
    prev_path = tmp_path / "prev.json"
    prev_path.write_text(json.dumps(base_rep), encoding="utf-8")

    # current: TerrainBase moves to fbo=4, Water vanishes, add Grass
    changed = SAMPLE.replace("pass=TerrainBase fbo=3", "pass=TerrainBase fbo=4")
    changed = "\n".join(l for l in changed.splitlines() if "pass=Water" not in l)
    changed += "\n[RENDER_PASS v1] frame=1 pass=Grass fbo=3 viewport=0,0,800,600 drawbuffers=2 phase=begin hint=grass\n"
    cur = _write(tmp_path, "cur.log", changed)
    crecs, csources = rpr.collect(cur)
    cur_rep = rpr.build_report(cur, crecs, csources)
    diffs = rpr.compare(cur_rep, prev_path)
    joined = " | ".join(diffs)
    assert "TerrainBase: fbo [3] -> [4]" in joined
    assert "pass VANISHED: Water" in joined
    assert "pass APPEARED: Grass" in joined


if __name__ == "__main__":
    import inspect
    import traceback
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    passed = failed = 0
    for fn in fns:
        try:
            if "tmp_path" in inspect.signature(fn).parameters:
                with tempfile.TemporaryDirectory() as td:
                    fn(Path(td))
            else:
                fn()
            passed += 1
            print(f"PASS {fn.__name__}")
        except Exception:
            failed += 1
            print(f"FAIL {fn.__name__}")
            traceback.print_exc()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)
