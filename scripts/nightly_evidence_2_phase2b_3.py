#!/usr/bin/env python3
"""NIGHTLY-EVIDENCE-2 Phase 2b (tier2) + Phase 3 chained runner.
Run after Phase 2a (tier1) completes. No --kill-existing."""

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
RESULTS_PATH = ARTIFACT_ROOT / "NIGHTLY-EVIDENCE-2-phase2b3-results.json"


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
    # VFX  (mc2_10: only tier1 mission with active gosFX)
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


def _extract_summary_from_artifact(artifact_dir: Path) -> dict:
    """Read the smoke summary.md for key metrics."""
    metrics = {}
    summary = artifact_dir / "summary.md"
    if not summary.exists():
        # Try individual mission logs
        for log in artifact_dir.glob("*.log"):
            text = log.read_text(encoding="utf-8", errors="replace")
            fps_m = re.search(r"avg_fps[:\s]+([0-9.]+)", text)
            if fps_m:
                metrics.setdefault("fps", []).append(float(fps_m.group(1)))
            gl_m = re.search(r"gl_errors[:\s]+(\d+)", text)
            if gl_m:
                metrics["gl_errors"] = metrics.get("gl_errors", 0) + int(gl_m.group(1))
        if "fps" in metrics:
            metrics["fps"] = round(sum(metrics["fps"]) / len(metrics["fps"]), 1)
        return metrics

    text = summary.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        ll = line.lower()
        if "avg_fps" in ll or "fps_avg" in ll:
            m = re.search(r"([0-9.]+)", line.split(":")[-1])
            if m:
                metrics["fps"] = float(m.group(1))
        if "gl_error" in ll:
            m = re.search(r"(\d+)", line.split(":")[-1])
            if m:
                metrics["gl_errors"] = int(m.group(1))
    return metrics


def run_one(entry: dict, idx: int, total: int, phase_label: str) -> dict:
    run_id = entry["id"]
    missions = entry.get("missions", [])
    duration = entry["duration"]
    env_overrides = entry.get("env", {})

    env_str = "  ".join(f"{k}={v}" for k, v in env_overrides.items()) or "(default)"
    print(f"\n[{phase_label} {idx+1}/{total}] {run_id}  env={env_str}", flush=True)

    cmd = [sys.executable, str(RUN_SMOKE), "--keep-logs", "--duration", str(duration)]
    for m in missions:
        cmd += ["--mission", m]

    env = os.environ.copy()
    for k in list(env.keys()):
        if k.startswith("MC2_"):
            del env[k]
    env.update(env_overrides)

    t0 = time.monotonic()
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    elapsed = time.monotonic() - t0

    rc = proc.returncode
    passed = rc == 0
    print(f"[{phase_label} {idx+1}/{total}] {run_id}  {'PASS' if passed else 'FAIL'}  {elapsed:.0f}s", flush=True)

    # Most recent artifact dir
    dirs = sorted(ARTIFACT_ROOT.glob("*"), key=lambda p: p.stat().st_mtime, reverse=True)
    artifact_dir = dirs[0] if dirs else None
    metrics = _extract_summary_from_artifact(artifact_dir) if artifact_dir else {}

    if not passed:
        tail = (proc.stdout or "")[-3000:]
        print(f"FAIL TAIL:\n{tail}", flush=True)

    return {
        "id": run_id,
        "phase": phase_label,
        "env": env_overrides,
        "missions": missions,
        "duration": duration,
        "passed": passed,
        "rc": rc,
        "elapsed_s": round(elapsed, 1),
        "artifact_dir": str(artifact_dir) if artifact_dir else None,
        **metrics,
    }


def run_tier2():
    """Phase 2b: tier2 full campaign smoke at 60s/mission."""
    print("\n[NIGHTLY-EVIDENCE-2] Phase 2b: tier2 (24 missions × 60s)", flush=True)
    cmd = [sys.executable, str(RUN_SMOKE), "--tier", "tier2", "--duration", "60", "--keep-logs"]
    env = os.environ.copy()
    for k in list(env.keys()):
        if k.startswith("MC2_"):
            del env[k]
    t0 = time.monotonic()
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    elapsed = time.monotonic() - t0
    passed = proc.returncode == 0
    print(f"[NIGHTLY-EVIDENCE-2] Phase 2b tier2: {'PASS' if passed else 'FAIL'}  {elapsed:.0f}s", flush=True)
    if not passed:
        print(proc.stdout[-4000:], flush=True)
    dirs = sorted(ARTIFACT_ROOT.glob("*"), key=lambda p: p.stat().st_mtime, reverse=True)
    artifact_dir = dirs[0] if dirs else None
    return {
        "id": "tier2_full",
        "phase": "P2b",
        "env": {},
        "missions": ["tier2"],
        "duration": 60,
        "passed": passed,
        "rc": proc.returncode,
        "elapsed_s": round(elapsed, 1),
        "artifact_dir": str(artifact_dir) if artifact_dir else None,
    }


def main():
    all_results = []

    # Phase 2b
    r = run_tier2()
    all_results.append(r)

    # Phase 3
    print(f"\n[NIGHTLY-EVIDENCE-2] Phase 3: {len(PHASE3_RUNS)} feature smokes", flush=True)
    for i, entry in enumerate(PHASE3_RUNS):
        r = run_one(entry, i, len(PHASE3_RUNS), "P3")
        all_results.append(r)

    passed = sum(1 for r in all_results if r["passed"])
    total = len(all_results)
    failures = [r["id"] for r in all_results if not r["passed"]]

    print(f"\n[NIGHTLY-EVIDENCE-2] Phase 2b+3 done: {passed}/{total} passed", flush=True)
    if failures:
        print(f"FAILURES: {', '.join(failures)}", flush=True)

    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)
    RESULTS_PATH.write_text(json.dumps(all_results, indent=2), encoding="utf-8")
    print(f"Results: {RESULTS_PATH}", flush=True)

    sys.exit(0 if not failures else 1)


if __name__ == "__main__":
    main()
