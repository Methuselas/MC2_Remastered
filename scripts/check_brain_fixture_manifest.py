#!/usr/bin/env python3
"""
BRAIN-DISPATCH-HARNESS-1: check_brain_fixture_manifest.py
Layer 3 — anti-orphan + manifest completeness checker.

Checks:
  FAIL: any .fit in fixture-dir that has NO manifest entry (orphan fixture)
  FAIL: any manifest entry whose 'file' does not exist in fixture-dir (missing file)
  WARN: manifest entry missing required fields (name, file)

Convention: scripts/check-*.py (--root/--quiet/--json, FAIL/WARN/PASS, exit 0/1/2)
  exit 0 = all pass
  exit 1 = FAIL found
  exit 2 = manifest unreadable / fixture-dir missing

Usage:
  py -3 scripts/check_brain_fixture_manifest.py [--root <worktree-root>] [--quiet] [--json]
"""

import argparse
import json
import os
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Brain fixture manifest checker")
    parser.add_argument("--root",        default=".", help="Worktree root (default: .)")
    parser.add_argument("--fixture-dir", default=None, help="Override fixture dir (default: <root>/tests/fixtures/brain_runtime)")
    parser.add_argument("--manifest",    default=None, help="Override manifest path")
    parser.add_argument("--quiet", "-q", action="store_true")
    parser.add_argument("--json",        action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    fixture_dir = Path(args.fixture_dir) if args.fixture_dir else root / "tests" / "fixtures" / "brain_runtime"
    manifest_path = Path(args.manifest) if args.manifest else fixture_dir / "manifest.json"

    findings = []  # list of (level, msg)

    # --- Check fixture-dir exists ---
    if not fixture_dir.is_dir():
        findings.append(("FAIL", f"fixture-dir not found: {fixture_dir}"))
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
    except json.JSONDecodeError as e:
        findings.append(("FAIL", f"manifest JSON parse error: {e}"))
        _report(findings, args)
        return 2
    except OSError as e:
        findings.append(("FAIL", f"cannot read manifest: {e}"))
        _report(findings, args)
        return 2

    if "fixtures" not in manifest or not isinstance(manifest["fixtures"], list):
        findings.append(("FAIL", "manifest missing 'fixtures' array"))
        _report(findings, args)
        return 2

    manifest_entries = manifest["fixtures"]

    # --- Build sets ---
    # .fit files in fixture-dir
    fit_files = {p.name for p in fixture_dir.glob("*.fit")}

    # manifest file names (one entry per file name)
    manifest_files = {}  # file name -> entry (first occurrence wins)
    for entry in manifest_entries:
        fname = entry.get("file", "")
        if not fname:
            findings.append(("WARN", f"manifest entry missing 'file' field: {entry.get('name', '<unnamed>')}"))
            continue
        if not entry.get("name"):
            findings.append(("WARN", f"manifest entry missing 'name' field for file={fname}"))
        if fname not in manifest_files:
            manifest_files[fname] = entry

    # --- Check 1: orphan .fit files (in dir but not in manifest) ---
    for fname in sorted(fit_files):
        if fname not in manifest_files:
            findings.append(("FAIL", f"ORPHAN fixture (no manifest entry): {fname}"))

    # --- Check 2: missing files (in manifest but not in dir) ---
    for fname, entry in sorted(manifest_files.items()):
        fpath = fixture_dir / fname
        if not fpath.is_file():
            findings.append(("FAIL", f"MISSING fixture file (in manifest but not on disk): {fname}"))

    # --- Summary ---
    fail_count = sum(1 for lvl, _ in findings if lvl == "FAIL")
    warn_count = sum(1 for lvl, _ in findings if lvl == "WARN")

    if not findings:
        findings.append(("PASS", f"manifest complete: {len(manifest_files)} entries, {len(fit_files)} .fit files, no orphans or missing files"))

    _report(findings, args)

    if fail_count > 0:
        return 1
    return 0


def _report(findings, args):
    if args.json:
        import json as _json
        print(_json.dumps([{"level": lvl, "message": msg} for lvl, msg in findings], indent=2))
        return
    for lvl, msg in findings:
        if args.quiet and lvl == "PASS":
            continue
        print(f"{lvl}: {msg}")


if __name__ == "__main__":
    sys.exit(main())
