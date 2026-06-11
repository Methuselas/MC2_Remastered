#!/usr/bin/env python3
# scripts/tests/test_deploy_coherence.py
"""Tests for write-deploy-manifest.py / check-deploy-coherence.py.

Uses TEMP fake deploy dirs only -- never touches A:/Games/mc2-opengl/.

Run:
  py -3 -m pytest scripts/tests/test_deploy_coherence.py -v
"""
from __future__ import annotations

import importlib.util
import io
import json
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPTS = Path(__file__).resolve().parents[1]


def _load(name: str):
    spec = importlib.util.spec_from_file_location(
        name.replace("-", "_"), SCRIPTS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


writer = _load("write-deploy-manifest")
checker = _load("check-deploy-coherence")


@pytest.fixture
def deploy(tmp_path: Path) -> Path:
    """Fake deploy dir: exe + dll + shaders + sidecar, plus manifest."""
    d = tmp_path / "fake-v0.4"
    (d / "shaders" / "include").mkdir(parents=True)
    (d / "mc2.exe").write_bytes(b"EXE" * 1000)
    (d / "avcodec-61.dll").write_bytes(b"DLL" * 500)
    (d / "shaders" / "gos_terrain.frag").write_text("void main() {}\n")
    (d / "shaders" / "include" / "shadow.hglsl").write_text("// pcf\n")
    (d / "options.cfg").write_text("sidecar=1\n")  # sidecar listed explicitly
    files = writer.collect_files(
        d, ["mc2.exe", "options.cfg"],
        ["*.dll", "shaders/*.frag", "shaders/include/*"])
    manifest = writer.build_manifest(d, files, tmp_path)  # non-git worktree
    (d / writer.MANIFEST_NAME).write_text(json.dumps(manifest))
    return d


def _warns(deploy_dir: Path, worktree: Path) -> tuple[int, str]:
    out = io.StringIO()
    n = checker.check(deploy_dir, worktree, out=out)
    return n, out.getvalue()


def test_fresh_manifest_no_warns(deploy, tmp_path):
    n, text = _warns(deploy, tmp_path)
    assert n == 0
    assert "[DEPLOY_COHERENCE] OK" in text


def test_exe_mutated_warns(deploy, tmp_path):
    (deploy / "mc2.exe").write_bytes(b"NEWEXE" * 999)
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "mc2.exe" in text and "[DEPLOY_COHERENCE] WARN" in text


def test_exe_same_size_content_drift_warns(deploy, tmp_path):
    # same byte count, different content -> hash drift path
    (deploy / "mc2.exe").write_bytes(b"XXE" * 1000)
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "hash drift" in text and "mc2.exe" in text


def test_shader_mutated_warns(deploy, tmp_path):
    (deploy / "shaders" / "gos_terrain.frag").write_text("void main() { /*x*/ }\n")
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "gos_terrain.frag" in text


def test_sidecar_mutated_warns(deploy, tmp_path):
    (deploy / "options.cfg").write_text("sidecar=2 drifted\n")
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "options.cfg" in text


def test_deployed_file_missing_warns(deploy, tmp_path):
    (deploy / "avcodec-61.dll").unlink()
    n, text = _warns(deploy, tmp_path)
    assert n >= 1
    assert "missing" in text and "avcodec-61.dll" in text


def test_missing_manifest_advisory_exit0(tmp_path):
    d = tmp_path / "no-manifest"
    d.mkdir()
    (d / "mc2.exe").write_bytes(b"EXE")
    # library level: zero warns, advisory note
    n, text = _warns(d, tmp_path)
    assert n == 0
    assert "coherence unknown" in text
    # process level: exit 0 always
    r = subprocess.run(
        [sys.executable, str(SCRIPTS / "check-deploy-coherence.py"),
         str(d), "--worktree", str(tmp_path)],
        capture_output=True, text=True)
    assert r.returncode == 0
    assert "[DEPLOY_COHERENCE]" in r.stdout


def test_check_exit0_even_with_drift(deploy, tmp_path):
    (deploy / "mc2.exe").write_bytes(b"stale!")
    r = subprocess.run(
        [sys.executable, str(SCRIPTS / "check-deploy-coherence.py"),
         str(deploy), "--worktree", str(tmp_path)],
        capture_output=True, text=True)
    assert r.returncode == 0  # advisory only, never hard-fail
    assert "WARN" in r.stdout


def test_writer_cli_writes_manifest_only_when_invoked(tmp_path):
    d = tmp_path / "explicit"
    d.mkdir()
    (d / "mc2.exe").write_bytes(b"EXE")
    assert not (d / writer.MANIFEST_NAME).exists()
    r = subprocess.run(
        [sys.executable, str(SCRIPTS / "write-deploy-manifest.py"),
         str(d), "mc2.exe", "--worktree", str(tmp_path)],
        capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    data = json.loads((d / writer.MANIFEST_NAME).read_text())
    assert data["files"][0]["path"] == "mc2.exe"
    assert "written_at" in data and "git_head" in data


def test_writer_merge_keeps_other_entries(tmp_path):
    d = tmp_path / "merge"
    d.mkdir()
    (d / "mc2.exe").write_bytes(b"GAME")
    (d / "Mission Editor.exe").write_bytes(b"EDITOR")
    for argset in (["mc2.exe"], ["Mission Editor.exe", "--merge"]):
        r = subprocess.run(
            [sys.executable, str(SCRIPTS / "write-deploy-manifest.py"),
             str(d), *argset, "--worktree", str(tmp_path)],
            capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
    paths = {e["path"] for e in
             json.loads((d / writer.MANIFEST_NAME).read_text())["files"]}
    assert paths == {"mc2.exe", "Mission Editor.exe"}


def test_staleness_warn_against_real_repo(deploy):
    # Point the manifest at an ancient commit of THIS repo; commits touching
    # code/mclib/GameOS/shaders since then must trigger the STALE warn.
    repo = SCRIPTS.parent
    old = subprocess.check_output(
        ["git", "-C", str(repo), "rev-list", "--max-parents=0", "HEAD"],
        text=True).splitlines()[0].strip()
    mpath = deploy / writer.MANIFEST_NAME
    data = json.loads(mpath.read_text())
    data["git_head"] = old
    mpath.write_text(json.dumps(data))
    n, text = _warns(deploy, repo)
    assert "STALE" in text
