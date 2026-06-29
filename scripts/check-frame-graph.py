#!/usr/bin/env python3
"""check-frame-graph.py — CI gate for the render-frame contract's offline suites.

Builds + runs the pure-logic doctest suites that guard the contract (no GL/engine/
deploy/smoke):
  - FrameGraph:   resource-DAG validation -- reorder / missing-producer regression guard
                  (FRAME-GRAPH-SKELETON-1)
  - ViewCurrency: object MVP view-epoch currency + same-camera dedupe, incl. the
                  counter-proof of the old stale-every-frame bug (VIEW-EPOCH-DEDUPE-1)

Delegates to scripts/run-unit-tests.sh so there is ONE builder/runner. Exit 0 iff both
suites pass.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
runner = ROOT / "scripts" / "run-unit-tests.sh"
if not runner.exists():
    print(f"check-frame-graph: missing {runner}", file=sys.stderr)
    sys.exit(2)

# Git Bash drives the standalone cmake configure/build/run. --ts filters to the two
# contract suites so the gate is fast and focused.
cmd = ["bash", str(runner), "--ts=FrameGraph,ViewCurrency"]
sys.exit(subprocess.call(cmd))
