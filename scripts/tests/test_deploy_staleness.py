#!/usr/bin/env python3
# scripts/tests/test_deploy_staleness.py
"""Tests for deploy_payload.staleness_report (SMOKE-FINGERPRINT-FULL-COVERAGE-1).

Pure filesystem compare against .deployed_manifest.csv — no game launch, no git.
Proves the stale-shader / missing-file / no-manifest / bad-version paths that the
run_smoke default [DEPLOY_STALENESS] advisory relies on.

Run:
  py -3 -m pytest scripts/tests/test_deploy_staleness.py -v
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def _make_target(tmp_path, files: dict, manifest_files=None, src_commit="abc123"):
    """Create a deploy dir with `files` {relpath: content} and a manifest that
    records `manifest_files` (defaults to all files) with their real sha256."""
    d = tmp_path / "target"
    for rel, content in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")
    rows = [["manifest_version", dp.MANIFEST_VERSION],
            ["relpath", "sha256", "bytes", "src_commit", "timestamp"]]
    for rel in (manifest_files if manifest_files is not None else list(files)):
        p = d / rel
        h = dp.sha256_file(str(p)) if p.is_file() else "0" * 64
        rows.append([rel, h, str(p.stat().st_size if p.is_file() else 0),
                     src_commit, "2026-06-22T00:00:00"])
    with open(d / dp.MANIFEST_NAME, "w", newline="") as f:
        csv.writer(f).writerows(rows)
    return d


def test_fresh_all_match(tmp_path):
    d = _make_target(tmp_path, {
        "mc2.exe": "exe-bytes",
        "shaders/gos_terrain.frag": "frag-v1",
        "mc2.pdb": "pdb-bytes",
    })
    rep = dp.staleness_report(str(d))
    assert rep["has_manifest"] and rep["version_ok"]
    assert rep["ok"] == 3
    assert rep["stale"] == [] and rep["missing"] == []
    assert rep["src_commit"] == "abc123"


def test_stale_shader_detected(tmp_path):
    d = _make_target(tmp_path, {
        "mc2.exe": "exe-bytes",
        "shaders/gos_terrain.frag": "frag-v1",
    })
    # Edit a shader AFTER the manifest was written -> sha drift.
    (d / "shaders" / "gos_terrain.frag").write_text("frag-v2-EDITED", encoding="utf-8")
    rep = dp.staleness_report(str(d))
    assert rep["stale"] == ["shaders/gos_terrain.frag"]
    assert rep["missing"] == []
    assert rep["ok"] == 1


def test_missing_file_detected(tmp_path):
    d = _make_target(tmp_path, {
        "mc2.exe": "exe-bytes",
        "shaders/gos_terrain.frag": "frag-v1",
    })
    (d / "shaders" / "gos_terrain.frag").unlink()
    rep = dp.staleness_report(str(d))
    assert rep["missing"] == ["shaders/gos_terrain.frag"]
    assert rep["stale"] == []
    assert rep["ok"] == 1


def test_no_manifest(tmp_path):
    d = tmp_path / "bare"
    d.mkdir()
    (d / "mc2.exe").write_text("x", encoding="utf-8")
    rep = dp.staleness_report(str(d))
    assert rep["has_manifest"] is False
    assert rep["stale"] == [] and rep["missing"] == []


def test_bad_manifest_version(tmp_path):
    d = _make_target(tmp_path, {"mc2.exe": "x"})
    with open(d / dp.MANIFEST_NAME, "w", newline="") as f:
        csv.writer(f).writerows([["manifest_version", "v999"],
                                 ["relpath", "sha256", "bytes"]])
    rep = dp.staleness_report(str(d))
    assert rep["has_manifest"] is True
    assert rep["version_ok"] is False


def test_verify_only_still_delegates(tmp_path, capsys):
    # Behavior-preserving: verify_only (non-strict) on a drifted tree still
    # exits 0 and prints the advisory line, now via staleness_report.
    d = _make_target(tmp_path, {"mc2.exe": "x", "shaders/a.frag": "v1"})
    (d / "shaders" / "a.frag").write_text("v2", encoding="utf-8")
    rc = dp.verify_only(str(d), strict=False)
    assert rc == 0
    out = capsys.readouterr().out
    assert "1 stale" in out
    assert "STALE:   shaders/a.frag" in out
