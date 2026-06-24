#!/usr/bin/env python3
"""
BRAIN-DISPATCH-HARNESS-1: check_brain_fixture_deploy_sync.py
Layer 4 — worktree-vs-deploy sha256 sync checker.

Checks each manifest fixture against the deployed copy in the game dir.

Results:
  FAIL: sha256 MISMATCH (worktree vs deploy differ)
  FAIL: MISSING fixture file in worktree
  WARN: ORPHAN — file exists in deploy fixture dir but has no manifest entry
  SKIP: deploy fixture dir does not exist (engine not yet deployed)
  PASS: all manifest entries match their deployed copies

Convention: scripts/check-*.py (--root/--quiet/--json, FAIL/WARN/PASS, exit 0/1/2)
  exit 0 = all pass (or deploy dir absent — SKIP)
  exit 1 = FAIL found
  exit 2 = manifest unreadable / fixture-dir missing

Usage:
  py -3 scripts/check_brain_fixture_deploy_sync.py [--root <worktree-root>] [--deploy <deploy-root>] [--quiet] [--json]

Default deploy root: A:/Games/mc2-opengl/mc2-win64-v0.4
"""

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

DEFAULT_DEPLOY_ROOT = "A:/Games/mc2-opengl/mc2-win64-v0.4"
DEPLOY_FIXTURE_SUBPATH = "tests/fixtures/brain_runtime"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description="Brain fixture deploy-sync checker")
    parser.add_argument("--root",        default=".", help="Worktree root (default: .)")
    parser.add_argument("--deploy",      default=DEFAULT_DEPLOY_ROOT, help=f"Deploy root (default: {DEFAULT_DEPLOY_ROOT})")
    parser.add_argument("--fixture-dir", default=None, help="Override worktree fixture dir")
    parser.add_argument("--manifest",    default=None, help="Override manifest path")
    parser.add_argument("--quiet", "-q", action="store_true")
    parser.add_argument("--json",        action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    fixture_dir = Path(args.fixture_dir) if args.fixture_dir else root / "tests" / "fixtures" / "brain_runtime"
    manifest_path = Path(args.manifest) if args.manifest else fixture_dir / "manifest.json"
    deploy_fixture_dir = Path(args.deploy) / DEPLOY_FIXTURE_SUBPATH

    findings = []

    # --- Check fixture-dir exists ---
    if not fixture_dir.is_dir():
        findings.append(("FAIL", f"worktree fixture-dir not found: {fixture_dir}"))
        _report(findings, args)
        return 2

    # --- Load manifest ---
    if not manifest_path.is_file():
        findings.append(("FAIL", f"manifest not found: {manifest_path}"))
        _report(findings, args)
        return 2

    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except (json.JSONDecodeError, OSError) as e:
        findings.append(("FAIL", f"manifest error: {e}"))
        _report(findings, args)
        return 2

    if "fixtures" not in manifest or not isinstance(manifest["fixtures"], list):
        findings.append(("FAIL", "manifest missing 'fixtures' array"))
        _report(findings, args)
        return 2

    # --- Check deploy dir ---
    if not deploy_fixture_dir.is_dir():
        findings.append(("SKIP", f"deploy fixture dir absent (not yet deployed): {deploy_fixture_dir}"))
        _report(findings, args)
        return 0

    manifest_files = set()
    for entry in manifest["fixtures"]:
        fname = entry.get("file", "")
        if not fname:
            continue
        manifest_files.add(fname)
        src = fixture_dir / fname
        dst = deploy_fixture_dir / fname

        if not src.is_file():
            findings.append(("FAIL", f"MISSING in worktree: {fname}"))
            continue

        if not dst.is_file():
            findings.append(("WARN", f"NOT_DEPLOYED (in manifest, not in deploy dir): {fname}"))
            continue

        src_hash = sha256_file(src)
        dst_hash = sha256_file(dst)
        if src_hash != dst_hash:
            findings.append(("FAIL", f"MISMATCH {fname}: worktree={src_hash[:12]}... deploy={dst_hash[:12]}..."))
        else:
            findings.append(("PASS", f"OK {fname} ({src_hash[:12]}...)"))

    # --- ORPHAN check: files in deploy dir but not in manifest ---
    if deploy_fixture_dir.is_dir():
        for p in sorted(deploy_fixture_dir.glob("*.fit")):
            if p.name not in manifest_files:
                findings.append(("WARN", f"ORPHAN in deploy (no manifest entry): {p.name}"))

    fail_count = sum(1 for lvl, _ in findings if lvl == "FAIL")

    if not findings:
        findings.append(("PASS", f"all {len(manifest_files)} manifest fixtures match deploy"))

    _report(findings, args)
    return 1 if fail_count > 0 else 0


def _report(findings, args):
    if args.json:
        print(json.dumps([{"level": lvl, "message": msg} for lvl, msg in findings], indent=2))
        return
    for lvl, msg in findings:
        if args.quiet and lvl == "PASS":
            continue
        print(f"{lvl}: {msg}")


if __name__ == "__main__":
    sys.exit(main())
