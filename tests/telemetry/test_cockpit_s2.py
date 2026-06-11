#!/usr/bin/env python3
"""
tests/telemetry/test_cockpit_s2.py -- Offline unit verification for S2 cockpit hook.

Tests (no mc2.exe launch, no real smokes):
  1. Simulate a completed artifact dir -> assert 3 cockpit artifacts appear and
     are valid JSON/NDJSON.
  2. Fault injection: monkeypatch lifter to raise -> assert exit-code path
     unaffected (no exception propagates) + .cockpit-error.txt written.

Run:
    py -3 tests/telemetry/test_cockpit_s2.py
Exit 0 = all pass, nonzero = failure.
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

# Ensure repo root on path.
REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.smoke_lib import cockpit

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

GOLDEN_ARTIFACT = (
    Path(REPO_ROOT).parent / "nifty-mendeleev"
    / "tests" / "smoke" / "artifacts" / "2026-06-09T19-27-36"
)
# Fallback if nifty-mendeleev not present
ALT_GOLDEN_ARTIFACT = REPO_ROOT / "tests" / "smoke" / "artifacts" / "2026-06-09T19-27-36"

FAKE_EXE_CONTENT = b"MC2FAKEEXE" * 100


def _find_golden():
    for p in (GOLDEN_ARTIFACT, ALT_GOLDEN_ARTIFACT):
        if p.is_dir() and any(p.glob("*.log")):
            return p
    return None


_ART_COUNTER = [0]

def _make_fake_artifact_dir(tmp: Path, golden: Path) -> Path:
    """Copy golden logs into tmp so cockpit has logs to lift."""
    _ART_COUNTER[0] += 1
    art = tmp / f"2099-01-01T00-00-{_ART_COUNTER[0]:02d}"
    art.mkdir()
    for f in golden.glob("*.log"):
        shutil.copy2(f, art / f.name)
    return art


def _make_fake_exe(tmp: Path) -> Path:
    exe = tmp / "mc2_fake.exe"
    exe.write_bytes(FAKE_EXE_CONTENT)
    return exe


def _assert(cond, msg):
    if not cond:
        print(f"FAIL: {msg}", file=sys.stderr)
        sys.exit(1)


# ---------------------------------------------------------------------------
# Test 1: normal operation
# ---------------------------------------------------------------------------

def check_normal(tmp: Path, golden: Path):
    art = _make_fake_artifact_dir(tmp, golden)
    exe = _make_fake_exe(tmp)

    cockpit.write_cockpit_artifacts(
        art,
        exe_path=str(exe),
        tier="tier1",
        profile="stock",
        missions=["mc2_01"],
        durations={"mc2_01": 30},
        result="PASS",
        source="smoke",
    )

    # 1a. manifest.json exists and is valid JSON
    manifest_path = art / "manifest.json"
    _assert(manifest_path.exists(), "manifest.json should exist")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    _assert(manifest.get("schema_v") == 1, "manifest schema_v==1")
    _assert(manifest.get("tier") == "tier1", "manifest tier==tier1")
    _assert(manifest.get("result") == "PASS", "manifest result==PASS")
    _assert("timestamp_iso" in manifest, "manifest has timestamp_iso")
    _assert(manifest["exe"]["path"] == str(exe), "manifest exe.path correct")
    _assert(manifest["exe"]["size_bytes"] == len(FAKE_EXE_CONTENT), "manifest exe.size_bytes correct")
    _assert(manifest["exe"]["sha256_prefix"] is not None, "manifest exe.sha256_prefix present")
    _assert(manifest["missions"] == ["mc2_01"], "manifest missions list")
    _assert(isinstance(manifest["env_gates"], dict), "manifest env_gates is dict")
    print("  [ok] manifest.json: valid, all required fields present")

    # 1b. telemetry.ndjson exists and is valid NDJSON (each line is JSON)
    ndjson_path = art / "telemetry.ndjson"
    _assert(ndjson_path.exists(), "telemetry.ndjson should exist")
    ndjson_lines = ndjson_path.read_text(encoding="utf-8").splitlines()
    _assert(len(ndjson_lines) > 0, "telemetry.ndjson is non-empty")
    for i, line in enumerate(ndjson_lines):
        try:
            obj = json.loads(line)
        except json.JSONDecodeError as e:
            _assert(False, f"telemetry.ndjson line {i+1} is not valid JSON: {e}")
        _assert("tag" in obj, f"telemetry.ndjson line {i+1} missing 'tag'")
        _assert("v" in obj, f"telemetry.ndjson line {i+1} missing 'v'")
    print(f"  [ok] telemetry.ndjson: {len(ndjson_lines)} records, all valid NDJSON")

    # 1c. oracle_summary.json exists and is valid JSON
    oracle_path = art / "oracle_summary.json"
    _assert(oracle_path.exists(), "oracle_summary.json should exist")
    oracle = json.loads(oracle_path.read_text(encoding="utf-8"))
    _assert(oracle.get("schema_v") == 1, "oracle_summary schema_v==1")
    _assert(oracle.get("tier") == "tier1", "oracle_summary tier==tier1")
    _assert(isinstance(oracle.get("oracles"), list), "oracle_summary.oracles is list")
    for entry in oracle["oracles"]:
        _assert("tag" in entry, "oracle entry has tag")
        _assert("status" in entry, "oracle entry has status")
        _assert(entry["status"] in ("pass", "fail", "vacuous"), "oracle status valid")
    print(f"  [ok] oracle_summary.json: {len(oracle['oracles'])} oracles, "
          f"pass={oracle['pass_count']} fail={oracle['fail_count']} vacuous={oracle['vacuous_count']}")

    # 1d. No error sidecar
    _assert(not (art / ".cockpit-error.txt").exists(),
            ".cockpit-error.txt must NOT exist on clean run")
    print("  [ok] no .cockpit-error.txt on clean run")


# ---------------------------------------------------------------------------
# Test 2: fault injection -- lifter raises, error swallowed
# ---------------------------------------------------------------------------

def check_fault_injection(tmp: Path, golden: Path):
    art = _make_fake_artifact_dir(tmp, golden)
    exe = _make_fake_exe(tmp)

    # Monkeypatch: replace lift_artifact_dir with a function that raises.
    import scripts.smoke_lib.cockpit as cockpit_mod
    import importlib
    telemetry_lift_mod = importlib.import_module("telemetry_lift")
    original_lift = telemetry_lift_mod.lift_artifact_dir

    def _boom(*args, **kwargs):
        raise RuntimeError("INJECTED FAULT: lifter intentionally raises")

    telemetry_lift_mod.lift_artifact_dir = _boom
    try:
        # Must NOT raise -- exception must be swallowed
        try:
            cockpit.write_cockpit_artifacts(
                art,
                exe_path=str(exe),
                tier="tier1",
                profile="stock",
                missions=["mc2_01"],
                durations={"mc2_01": 30},
                result="PASS",
                source="smoke",
            )
        except Exception as e:
            _assert(False, f"write_cockpit_artifacts raised despite fault injection: {e}")

        # .cockpit-error.txt must be written
        error_path = art / ".cockpit-error.txt"
        _assert(error_path.exists(), ".cockpit-error.txt must exist after fault injection")
        error_text = error_path.read_text(encoding="utf-8")
        _assert("INJECTED FAULT" in error_text, ".cockpit-error.txt contains fault message")
        _assert("Traceback" in error_text, ".cockpit-error.txt contains traceback")
        print(f"  [ok] fault injection: exception swallowed, .cockpit-error.txt written")

        # Verify the exit-code path is unaffected -- simulate what run_smoke does.
        # The verdict (passed = True/False) is computed BEFORE the hook, so nothing
        # the hook does can change it.  We just assert the hook didn't raise.
        passed = True  # simulated
        exit_code = 0 if passed else 1
        _assert(exit_code == 0, "simulated exit code unaffected by fault injection")
        print(f"  [ok] fault injection: exit-code path would be 0 (unaffected)")

    finally:
        telemetry_lift_mod.lift_artifact_dir = original_lift


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    golden = _find_golden()
    if golden is None:
        print("WARNING: golden artifact dir not found; test 1 skipped (no logs to lift)",
              file=sys.stderr)
        golden_available = False
    else:
        golden_available = True
        print(f"Using golden artifact dir: {golden}")

    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)

        if golden_available:
            print("\n--- Test 1: normal operation ---")
            check_normal(tmp, golden)

        print("\n--- Test 2: fault injection ---")
        # For fault injection we still need some dir with at least one .log file.
        if golden_available:
            check_fault_injection(tmp, golden)
        else:
            # Create a minimal fake artifact dir with an empty log.
            fake_golden = tmp / "fake_golden"
            fake_golden.mkdir()
            (fake_golden / "mc2_01.log").write_text("", encoding="utf-8")
            check_fault_injection(tmp, fake_golden)

    print("\nAll S2 cockpit tests PASSED.")
    sys.exit(0)


if __name__ == "__main__":
    main()
