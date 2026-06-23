#!/usr/bin/env python3
"""pipeline_visual_gate.py — PIPELINE-VISUAL-GATE-HARNESS-1

Thin, CONCURRENT-SAFE wrapper over scripts/run_visual_capture.py that proves a
routed render pass (a pipeline-pass-coverage-ledger entry) actually renders
correctly, so its proofStatus can move to a LANDED value and the pass to
VISUAL_PROVEN.

It does NOT re-implement capture/compare — it drives the existing tools through a
per-pass PROFILE:
  - run_visual_capture.py --no-kill (never taskkills foreign mc2.exe; restores
    the desktop cursor) with the deterministic clock (MC2_SMOKE_MODE=1 +
    MC2_SMOKE_FIXED_TIMESTEP=1 + MC2_SMOKE_SEED, applied by run_visual_capture)
    + sim-freeze at the trigger frame.
  - --runs 3 --warmup 1 byte-STABILITY (same exe, deterministic).
  - optional BEFORE/AFTER byte A/B across two exes (pre- vs post-routing).
  - a "pass actually drew" confirmation via the MC2_PIPELINE_BIND_TRACE
    [PIPELINE_BIND] <Row> line (and/or an occlusion-coverage oracle env).

On success the operator flips the ledger proofStatus to the landed value
(byte_identical / perceptual_ab / oracle_coverage) and status -> VISUAL_PROVEN
(check-pass-coverage.py enforces: no VISUAL_PROVEN while proofStatus pending).

This script LAUNCHES the game (capture) — only run it with explicit go-ahead and
when concurrent-safe (it passes --no-kill so a foreign mc2.exe is left alone).
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RVC = ROOT / "scripts" / "run_visual_capture.py"
DEFAULT_EXE = "A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe"

# Per-pass profiles. Each names the ledger pass it proves, the mission +
# bookmark that frames the pass, capture timing, the [PIPELINE_BIND] row whose
# presence confirms the pass drew, and any extra env to force it in-frame.
PROFILES = {
    "water": {
        "ledger_pass": "Water",
        "pipeline_row": "WaterArmed",
        "mission": "mc2_01",                 # clearwater — water by default
        "bookmark": "tests/visual/bookmarks/mc2_01_water.json",
        "trigger_frame": 120,
        "settle": 30,
        "duration": 45,
        "force_env": {},                     # water-fast is the DEFAULT path (no arming env)
        "gate": "byte_identical",
        "note": "WaterArmed draws every frame in clearwater; bookmark must FRAME "
                "water (stock cameras minimize it). Confirm via [PIPELINE_BIND] "
                "WaterArmed + visible water pixels.",
    },
    # VFX profile lands in VFX-VISUAL-GATE-1 (reuses MC2_FX_FORCE_SPAWN +
    # mc2_01_werewolf.json + MC2_VFX_ORACLE_TUBE_COVERAGE).
}


def _capture(exe: str, prof: dict, out_dir: Path, runs: int, warmup: int,
             trace: bool, dry_run: bool) -> int:
    env = os.environ.copy()
    env.update(prof.get("force_env", {}))
    if trace:
        env["MC2_PIPELINE_BIND_TRACE"] = "1"
    cmd = [sys.executable, str(RVC),
           "--no-kill",                       # concurrent-safe: never kill foreign mc2.exe
           "--exe", exe,
           "--mission", prof["mission"],
           "--bookmarks", str(ROOT / prof["bookmark"]),
           "--out-dir", str(out_dir),
           "--trigger-frame", str(prof["trigger_frame"]),
           "--settle", str(prof["settle"]),
           "--duration", str(prof["duration"]),
           "--runs", str(runs), "--warmup", str(warmup)]
    print(f"[gate] {'DRY-RUN ' if dry_run else ''}capture: {' '.join(cmd)}")
    if dry_run:
        return 0
    return subprocess.run(cmd, env=env).returncode


def _row_drew(out_dir: Path, row: str) -> int:
    """Count [PIPELINE_BIND] <row> lines in capture logs (pass actually drew)."""
    n = 0
    for log in out_dir.rglob("*_capture.log"):
        try:
            for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
                if "PIPELINE_BIND" in line and f" {row} " in line:
                    n += 1
        except OSError:
            pass
    return n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("profile", choices=sorted(PROFILES))
    ap.add_argument("--exe", default=DEFAULT_EXE, help="AFTER (routed) exe")
    ap.add_argument("--before-exe", default=None,
                    help="pre-routing exe for a before/after byte A/B (optional)")
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--dry-run", action="store_true",
                    help="print the capture plan without launching the game")
    args = ap.parse_args()

    prof = PROFILES[args.profile]
    base = Path(args.out_dir) if args.out_dir else (ROOT / "tests" / "visual" /
                                                    "gate" / args.profile)
    after_dir = base / "after"

    print(f"[gate] profile={args.profile} pass={prof['ledger_pass']} "
          f"row={prof['pipeline_row']} gate={prof['gate']}")

    # AFTER: 3-run byte-stability on the routed exe.
    rc = _capture(args.exe, prof, after_dir, args.runs, args.warmup,
                  trace=True, dry_run=args.dry_run)
    if args.dry_run:
        if args.before_exe:
            _capture(args.before_exe, prof, base / "before", args.runs,
                     args.warmup, trace=False, dry_run=True)
        print("[gate] DRY-RUN complete (no launch).")
        return 0
    if rc != 0:
        print(f"[gate] FAIL: AFTER capture/stability rc={rc}", file=sys.stderr)
        return 1

    drew = _row_drew(after_dir, prof["pipeline_row"])
    print(f"[gate] {prof['pipeline_row']} drew: {drew} [PIPELINE_BIND] lines "
          f"({'OK' if drew else 'NOT DRAWN — bookmark may not frame the pass'})")
    if drew == 0:
        print("[gate] FAIL: pass did not draw — fix the bookmark framing/force env",
              file=sys.stderr)
        return 2

    # Optional BEFORE/AFTER byte A/B.
    if args.before_exe:
        rcb = _capture(args.before_exe, prof, base / "before", args.runs,
                       args.warmup, trace=False, dry_run=False)
        if rcb != 0:
            print(f"[gate] FAIL: BEFORE capture rc={rcb}", file=sys.stderr)
            return 1
        # Compare last-run PNGs by sha (deterministic captures).
        print("[gate] before/after: compare the materialized run shas "
              "(use scripts/visual_compare.py for perceptual fallback).")

    print(f"[gate] PASS prerequisites met for {prof['ledger_pass']}: stable + drew. "
          "Operator: set ledger proofStatus -> landed + status VISUAL_PROVEN.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
