#!/usr/bin/env python3
"""Unit tests for scripts/run_visual.py compare logic (no game launch).

Builds a synthetic BLESSED golden set + a matching capture dir in temp dirs,
monkeypatches the module's path globals, and exercises every verdict via the
--capture-dir path (so rvc.run_one is never invoked).
"""
from __future__ import annotations

import json
import platform
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import manifest_schema as ms  # noqa: E402
import run_visual as rv  # noqa: E402

MISSION = "mc2_test"
SET_ID = "fixtureA"


def _write_png(path: Path, content: bytes) -> str:
    path.write_bytes(content)
    return ms.file_sha256(path)


def _setup(tmp: Path, *, capture_matches=True, drop_b=False, exe_sha_ok=True,
           cap_deterministic=True):
    """Create bookmarks + golden set + capture dir. Returns (registry, exe)."""
    goldens = tmp / "baselines"
    bookmarks = tmp / "bookmarks"
    capdir = tmp / "cap"
    for d in (goldens / SET_ID, bookmarks, capdir):
        d.mkdir(parents=True, exist_ok=True)
    rv.GOLDENS_DIR = goldens
    rv.BOOKMARK_DIR = bookmarks

    # bookmarks json
    (bookmarks / f"{MISSION}.json").write_text(json.dumps({
        "mission": MISSION,
        "bookmarks": [{"name": "a", "covers": ["t"]}, {"name": "b", "covers": ["t"]}],
    }), encoding="utf-8")

    # fake exe -> sha
    exe = tmp / "mc2.exe"
    exe.write_bytes(b"exe-bytes-v1")
    exe_sha = ms.file_sha256(exe)

    # golden PNGs (define the golden hashes) + capture PNGs
    gsha_a = _write_png(goldens / SET_ID / f"{MISSION}_a.png", b"frame-A")
    gsha_b = _write_png(goldens / SET_ID / f"{MISSION}_b.png", b"frame-B")
    _write_png(capdir / f"{MISSION}_a.png",
               b"frame-A" if capture_matches else b"frame-A-CHANGED")
    if not drop_b:
        _write_png(capdir / f"{MISSION}_b.png", b"frame-B")
    # capture sidecars (deterministic flag)
    for name in ("a", "b"):
        side = capdir / f"{MISSION}_{name}.json"
        side.write_text(json.dumps({"deterministic": cap_deterministic}), encoding="utf-8")

    # golden set.json with identity (exe sha + host)
    set_json = {
        "set_id": SET_ID, "status": "blessed", "mission": MISSION,
        "identity": {
            "schema": ms.SCHEMA, "generator": "visual_capture",
            "exe": {"sha256": exe_sha if exe_sha_ok else "deadbeef" * 8},
            "host": platform.node(),
        },
        "bookmarks": [
            {"name": "a", "sha256": gsha_a, "covers": ["t"]},
            {"name": "b", "sha256": gsha_b, "covers": ["t"]},
        ],
    }
    (goldens / SET_ID / "set.json").write_text(json.dumps(set_json), encoding="utf-8")

    registry = {"schema_v": 1, "sets": {SET_ID: {
        "set_id": SET_ID, "status": "blessed", "mission": MISSION}}}
    return registry, exe, capdir


def _run(tmp, **kw):
    registry, exe, capdir = _setup(tmp, **kw)
    return rv.compare_set(SET_ID, registry, exe, capdir, 120, 30, 45,
                          allow_exe=False, allow_device=False)


def test_pass(tmp_path):
    rec = _run(tmp_path)
    assert rec["verdict"] == rv.PASS, rec
    assert rec["matched_count"] == 2 and rec["changed_count"] == 0


def test_changed(tmp_path):
    rec = _run(tmp_path, capture_matches=False)
    assert rec["verdict"] == rv.FAIL_CHANGED, rec
    assert rec["changed_count"] == 1 and rec["matched_count"] == 1


def test_missing(tmp_path):
    rec = _run(tmp_path, drop_b=True)
    assert rec["verdict"] == rv.FAIL_MISSING, rec
    assert rec["missing_count"] == 1


def test_nondeterministic_capture_is_missing(tmp_path):
    rec = _run(tmp_path, cap_deterministic=False)
    # both frames present but flagged non-deterministic -> not comparable
    assert rec["verdict"] == rv.FAIL_MISSING, rec
    assert rec["missing_count"] == 2


def test_identity_mismatch(tmp_path):
    rec = _run(tmp_path, exe_sha_ok=False)
    assert rec["verdict"] == rv.FAIL_IDENTITY, rec
    assert "exe sha mismatch" in rec.get("reason", "")


def test_no_blessed_golden(tmp_path):
    registry, exe, capdir = _setup(tmp_path)
    registry["sets"][SET_ID]["status"] = "candidate"
    rec = rv.compare_set(SET_ID, registry, exe, capdir, 120, 30, 45,
                         allow_exe=False, allow_device=False)
    assert rec["verdict"] == rv.FAIL_NO_BLESSED, rec


def test_allow_exe_mismatch_overrides(tmp_path):
    registry, exe, capdir = _setup(tmp_path, exe_sha_ok=False)
    rec = rv.compare_set(SET_ID, registry, exe, capdir, 120, 30, 45,
                         allow_exe=True, allow_device=False)
    assert rec["verdict"] == rv.PASS, rec


if __name__ == "__main__":
    import inspect
    import traceback
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    passed = failed = 0
    for fn in fns:
        try:
            with tempfile.TemporaryDirectory() as td:
                if "tmp_path" in inspect.signature(fn).parameters:
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
