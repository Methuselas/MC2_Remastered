#!/usr/bin/env python3
"""check-ambient-guard.py — FRAME-GRAPH-AMBIENT-GUARD-1 enforcement gate.

Reads the latest debug-state dump and FAILS (exit 1) if the runtime ambient guard
recorded any mismatch between declared ambient state (RenderCore/ambient_contract.h:
colorMask / depthFunc / depthWrite) and live GL. The guard is default-ON in the engine
(MC2_FRAMEGRAPH_AMBIENT_GUARD=0 to disable); this script turns its counter into a
dev/smoke gate so a bad ambient handoff trips immediately instead of surfacing later as
flicker / missing geometry / wrong depth.

Usage:
  py -3 scripts/check-ambient-guard.py [path/to/latest_render_state.json]
Exit: 0 = PASS (mismatches==0). 1 = FAIL (mismatches>0) or dump unreadable.
      2 = INCONCLUSIVE (guard never sampled: samples==0 -> engine not run with guard).
"""
import json
import os
import sys
from pathlib import Path

def candidates(argv):
    if len(argv) > 1:
        return [Path(argv[1])]
    out = []
    dep = os.environ.get("MC2_DEPLOY_DIR")
    if dep:
        out.append(Path(dep) / "debug_state" / "latest_render_state.json")
    out += [
        Path("A:/Games/mc2-opengl/mc2-win64-0.4c/debug_state/latest_render_state.json"),
        Path(__file__).resolve().parent.parent / "debug_state" / "latest_render_state.json",
    ]
    return out

def main():
    path = next((p for p in candidates(sys.argv) if p.exists()), None)
    if path is None:
        print("check-ambient-guard: no latest_render_state.json found", file=sys.stderr)
        return 1
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"check-ambient-guard: cannot read {path}: {e}", file=sys.stderr)
        return 1

    fg = data.get("frame_graph", {})
    samples = fg.get("ambient_probe_samples", 0)
    mism = fg.get("ambient_probe_mismatches", 0)

    if samples in (0, "?"):
        print(f"check-ambient-guard: INCONCLUSIVE — guard sampled 0 passes ({path}). "
              "Run the engine with the guard enabled (default-ON).", file=sys.stderr)
        return 2
    if mism not in (0, "?"):
        print(f"check-ambient-guard: FAIL — {mism} ambient mismatch(es) over {samples} "
              f"samples ({path}). See [AMBIENT_GUARD] lines in the engine log for the "
              "offending pass + axis (declared vs live).", file=sys.stderr)
        return 1
    print(f"check-ambient-guard: OK — 0 mismatches over {samples} samples ({path.name})")
    return 0

if __name__ == "__main__":
    sys.exit(main())
