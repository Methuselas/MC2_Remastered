#!/usr/bin/env python3
"""NIGHTLY-EVIDENCE-2 Phase 3+4 orchestration script.
Runs targeted feature smokes sequentially (no --kill-existing).
Writes JSON results to tests/smoke/artifacts/NIGHTLY-EVIDENCE-2-results.json.
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUN_SMOKE = ROOT / "scripts" / "run_smoke.py"
ARTIFACT_ROOT = ROOT / "tests" / "smoke" / "artifacts"
RESULTS_PATH = ARTIFACT_ROOT / "NIGHTLY-EVIDENCE-2-results.json"

PHASE3_RUNS = [
    # Static props
    {"id": "static_default",           "env": {},                                                          "missions": ["mc2_01","mc2_10"], "duration": 30},
    {"id": "static_ibl_sh_on",         "env": {"MC2_STATIC_PROP_IBL_SH":        "1"},                     "missions": ["mc2_01","mc2_10"], "duration": 30},
    {"id": "static_pbr_v1",            "env": {"MC2_STATIC_PROP_PBR_V1":        "1",
                                                "MC2_VIEW_UNIFORMS":              "1"},                    "missions": ["mc2_01","mc2_10"], "duration": 30},
    {"id": "static_debug_material_3",  "env": {"MC2_STATIC_PROP_DEBUG_MATERIAL": "3"},                    "missions": ["mc2_01"],          "duration": 30},
    # Terrain
    {"id": "terrain_nfh",              "env": {"MC2_TERRAIN_NORMALS_FROM_HEIGHT": "1"},                   "missions": ["mc2_01","mc2_24"], "duration": 30},
    {"id": "terrain_lighting_v1",      "env": {"MC2_TERRAIN_LIGHTING_V1":        "1"},                    "missions": ["mc2_01","mc2_24"], "duration": 30},
    {"id": "terrain_lighting_v2",      "env": {"MC2_TERRAIN_LIGHTING_V1":        "1",
                                                "MC2_TERRAIN_LIGHTING_V2":        "1"},                    "missions": ["mc2_01","mc2_24"], "duration": 30},
    {"id": "terrain_debug_10",         "env": {"MC2_TERRAIN_NORMALS_FROM_HEIGHT": "1",
                                                "MC2_TERRAIN_DEBUG_MODE":         "10"},                   "missions": ["mc2_01"],          "duration": 30},
    {"id": "terrain_debug_11",         "env": {"MC2_TERRAIN_DEBUG_MODE":         "11"},                   "missions": ["mc2_01"],          "duration": 30},
    # Water
    {"id": "water_gpu_driven",         "env": {"MC2_GPU_DRIVEN_WATER":           "1"},                    "missions": ["mc2_01","mc2_03"], "duration": 30},
    {"id": "water_debug_mode_2",       "env": {"MC2_WATER_DEBUG_MODE":           "2"},                    "missions": ["mc2_01"],          "duration": 30},
    {"id": "water_skytint",            "env": {"MC2_WATER_SKYTINT":              "1"},                    "missions": ["mc2_01","mc2_03"], "duration": 30},
    # VFX  (mc2_10 only: only tier1 mission with active gosFX)
    {"id": "vfx_debug_4",              "env": {"MC2_VFX_DEBUG_MODE":             "4"},                    "missions": ["mc2_10"],          "duration": 45},
    {"id": "vfx_additive_brightness",  "env": {"MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS": "2"},                 "missions": ["mc2_10"],          "duration": 45},
    {"id": "vfx_age_sample",           "env": {"MC2_VFX_AGE_SAMPLE":             "1"},                   "missions": ["mc2_10"],          "duration": 45},
    # Mechs
    {"id": "mech_viewuniforms",        "env": {"MC2_MECH_VIEWUNIFORMS":          "1"},                    "missions": ["mc2_01","mc2_24"], "duration": 30},
    {"id": "mech_normals_mode_2",      "env": {"MC2_MECH_NORMALS_MODE":          "2"},                   "missions": ["mc2_01","mc2_24"], "duration": 30},
    {"id": "mech_frag_debug_4",        "env": {"MC2_MECH_FRAG_DEBUG":            "4"},                   "missions": ["mc2_01","mc2_24"], "duration": 30},
    {"id": "mech_snapshot_extract",    "env": {"MC2_SNAPSHOT_MECH_EXTRACT":      "1"},                   "missions": ["mc2_01","mc2_24"], "duration": 30},
    # Shadows
    {"id": "shadow_debug_state_dump",  "env": {"MC2_DEBUG_STATE_DUMP":           "1"},                   "missions": ["mc2_01"],          "duration": 30},
]


def _extract_metrics(artifact_dir: Path):
    """Pull fps/gl_errors/result from summary.md or *.log in artifact dir."""
    metrics = {}
    # Look for summary.md
    summary = artifact_dir / "summary.md"
    if summary.exists():
        text = summary.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if "avg_fps" in line or "fps_avg" in line:
                m = re.search(r"[\d.]+", line.split(":")[-1])
                if m:
                    metrics["fps"] = float(m.group())
            if "gl_errors" in line:
                m = re.search(r"\d+", line.split(":")[-1])
                if m:
                    metrics["gl_errors"] = int(m.group())
            if "result" in line.lower():
                if "pass" in line.lower():
                    metrics["result"] = "PASS"
                elif "fail" in line.lower():
                    metrics["result"] = "FAIL"
    return metrics


def run_smoke_entry(entry: dict, run_index: int, total: int) -> dict:
    run_id = entry["id"]
    missions = entry["missions"]
    duration = entry["duration"]
    env_overrides = entry["env"]

    env_str = "  ".join(f"{k}={v}" for k, v in env_overrides.items()) or "(default)"
    print(f"\n[P3 {run_index+1}/{total}] {run_id}  env={env_str}", flush=True)

    cmd = [sys.executable, str(RUN_SMOKE), "--keep-logs", "--duration", str(duration)]
    for m in missions:
        cmd += ["--mission", m]

    env = os.environ.copy()
    # Clear all MC2_* vars first (clean environment)
    for k in list(env.keys()):
        if k.startswith("MC2_"):
            del env[k]
    env.update(env_overrides)

    t0 = time.monotonic()
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    elapsed = time.monotonic() - t0

    rc = result.returncode
    passed = rc == 0
    status = "PASS" if passed else "FAIL"
    print(f"[P3 {run_index+1}/{total}] {run_id}  {status}  elapsed={elapsed:.0f}s", flush=True)

    # Find the artifact dir for this run (most recent)
    artifact_dirs = sorted(ARTIFACT_ROOT.glob("*"), key=lambda p: p.stat().st_mtime, reverse=True)
    artifact_dir = artifact_dirs[0] if artifact_dirs else None
    metrics = _extract_metrics(artifact_dir) if artifact_dir else {}

    if not passed:
        # Print stderr/stdout tail for diagnosis
        tail = (result.stdout or "")[-2000:]
        print(f"[P3 FAIL output tail]\n{tail}", flush=True)

    return {
        "id": run_id,
        "env": env_overrides,
        "missions": missions,
        "duration": duration,
        "passed": passed,
        "rc": rc,
        "elapsed_s": round(elapsed, 1),
        "artifact_dir": str(artifact_dir) if artifact_dir else None,
        **metrics,
    }


def main():
    print(f"[NIGHTLY-EVIDENCE-2] Phase 3 starting — {len(PHASE3_RUNS)} feature smokes", flush=True)
    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)

    results = []
    failures = []
    for i, entry in enumerate(PHASE3_RUNS):
        r = run_smoke_entry(entry, i, len(PHASE3_RUNS))
        results.append(r)
        if not r["passed"]:
            failures.append(r["id"])

    passed_count = sum(1 for r in results if r["passed"])
    print(f"\n[NIGHTLY-EVIDENCE-2] Phase 3 complete: {passed_count}/{len(results)} passed", flush=True)
    if failures:
        print(f"[NIGHTLY-EVIDENCE-2] FAILURES: {', '.join(failures)}", flush=True)

    RESULTS_PATH.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"[NIGHTLY-EVIDENCE-2] Results written to {RESULTS_PATH}", flush=True)
    sys.exit(0 if not failures else 1)


if __name__ == "__main__":
    main()
