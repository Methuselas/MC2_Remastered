#!/usr/bin/env python3
# scripts/run_smoke.py
"""MC2 smoke matrix runner.

Examples:
  python scripts/run_smoke.py --tier tier1 --fail-fast
  python scripts/run_smoke.py --tier tier1 --with-menu-canary
  python scripts/run_smoke.py --tier tier2
  python scripts/run_smoke.py --tier tier3 --kill-existing
  python scripts/run_smoke.py --mission mc2_01 --mission mc2_03
  python scripts/run_smoke.py --menu-canary
"""
from __future__ import annotations

import argparse
import atexit
import datetime as dt
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.smoke_lib import baselines, manifest, report
from scripts.smoke_lib.runner import RunConfig, run_one

DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")
ARTIFACT_ROOT = ROOT / "tests" / "smoke" / "artifacts"
MANIFEST_PATH = ROOT / "tests" / "smoke" / "smoke_missions.txt"
BASELINE_PATH = ROOT / "tests" / "smoke" / "baselines.json"
DEFAULT_MENU_SCRIPT = ROOT / "tests" / "smoke" / "menu_canary_first_mission.txt"
GAME_AUTO = ROOT / "scripts" / "game_auto.py"


def _running_mc2() -> list[int]:
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq mc2.exe", "/NH", "/FO", "CSV"],
            text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return []
    pids = []
    for line in out.splitlines():
        if "mc2.exe" in line:
            parts = [p.strip('"') for p in line.split(",")]
            if len(parts) > 1 and parts[1].isdigit():
                pids.append(int(parts[1]))
    return pids


def _taskkill_mc2():
    subprocess.run(["taskkill", "/F", "/IM", "mc2.exe"],
                   stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)


def _run_menu_canary(exe: Path, script_path: Path, artifact_dir: Path,
                     keep_logs: bool, settle_s: int) -> int:
    env = os.environ.copy()
    for var in ["MC2_SMOKE_MODE", "MC2_HEARTBEAT", "MC2_SMOKE_SEED"]:
        env.pop(var, None)
    env["MC2_MENU_CANARY_SKIP_INTRO"] = "1"
    exe_dir = str(exe.resolve().parent)
    proc = subprocess.Popen(
        [str(exe)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        cwd=exe_dir,
        env=env,
    )

    start_wall = time.monotonic()
    time.sleep(1.0)
    exited_before_replay = proc.poll() is not None
    auto = subprocess.run(
        [sys.executable, str(GAME_AUTO), "script", str(script_path)],
        text=True,
        capture_output=True,
        cwd=str(ROOT),
    )
    replay_elapsed_s = time.monotonic() - start_wall
    time.sleep(max(0, settle_s))

    game_alive = proc.poll() is None
    if game_alive:
        try:
            proc.wait(timeout=max(1, settle_s))
        except subprocess.TimeoutExpired:
            proc.kill()
    try:
        stdout, _ = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate(timeout=2)
    exit_code = proc.returncode
    lowered = (stdout or "").lower()
    clean_exit = "[exit] gos_terminateapplication called" in lowered
    crashy = any(token in lowered for token in [
        "unhandled exception",
        "fatal error",
        "access violation",
        "stack overflow",
        "abort",
        "assert",
    ])
    early_exit = exit_code == 0 and replay_elapsed_s > 0 and not game_alive and exited_before_replay
    passed = (
        auto.returncode == 0
        and clean_exit
        and not crashy
        and exit_code == 0
        and not early_exit
    )

    md = [
        "# Menu Canary",
        "",
        f"- script: `{script_path.name}`",
        f"- replay_exit: `{auto.returncode}`",
        f"- replay_elapsed_s: `{replay_elapsed_s:.2f}`",
        f"- exited_before_replay: `{str(exited_before_replay).lower()}`",
        f"- game_alive_after_replay: `{str(game_alive).lower()}`",
        f"- game_exit_code: `{exit_code}`",
        f"- clean_exit_marker: `{str(clean_exit).lower()}`",
        f"- crash_signature: `{str(crashy).lower()}`",
        f"- early_exit: `{str(early_exit).lower()}`",
        f"- result: `{'PASS' if passed else 'FAIL'}`",
    ]
    report_text = "\n".join(md) + "\n"
    (artifact_dir / "menu_canary_report.md").write_text(report_text, encoding="utf-8")
    if keep_logs or not passed:
        (artifact_dir / "menu_canary_game.log").write_text(stdout or "", encoding="utf-8", errors="replace")
        (artifact_dir / "menu_canary_replay.log").write_text(
            (auto.stdout or "") + ("\n" if auto.stdout and auto.stderr else "") + (auto.stderr or ""),
            encoding="utf-8",
            errors="replace",
        )
    sys.stdout.buffer.write(report_text.encode("utf-8"))
    sys.stdout.buffer.flush()
    return 0 if passed else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["tier1", "tier2", "tier3"])
    ap.add_argument("--mission", action="append", default=[])
    ap.add_argument("--menu-canary", action="store_true")
    ap.add_argument("--with-menu-canary", action="store_true")
    ap.add_argument("--menu-script", default=str(DEFAULT_MENU_SCRIPT))
    ap.add_argument("--menu-settle", type=int, default=5)
    ap.add_argument("--fail-fast", action="store_true")
    ap.add_argument("--continue", dest="cont", action="store_true", default=True)
    ap.add_argument("--keep-logs", action="store_true")
    ap.add_argument("--baseline-update", action="store_true")
    ap.add_argument("--kill-existing", action="store_true")
    ap.add_argument("--duration", type=int)
    ap.add_argument("--profile", default="stock")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    # Tier 1.2 (docs/testing-strategy.md): opt-in safety net that promotes
    # GL_DEBUG_SEVERITY_HIGH from a silent log to abort(). Off by default.
    ap.add_argument("--gl-debug-fatal", action="store_true",
                    help="set MC2_GL_DEBUG_FATAL=1 (abort on GL_DEBUG_SEVERITY_HIGH)")
    args = ap.parse_args()
    if args.gl_debug_fatal:
        os.environ["MC2_GL_DEBUG_FATAL"] = "1"

    # F1 unified-projection: forbid MC2_DISABLE_GOSFX=0 in smoke runs.
    # Visual regression accepted only in dev-override sessions; smoke must
    # represent shipped default state.
    if os.environ.get("MC2_DISABLE_GOSFX") == "0":
        print("[run_smoke] FATAL: MC2_DISABLE_GOSFX=0 conflicts with unified "
              "projection; smoke would record regressed visuals. Unset or set =1.",
              file=sys.stderr)
        sys.exit(2)

    # Concurrency lock: prevent two smoke runners from stepping on each other.
    # A second invocation with --kill-existing would taskkill the first run's
    # owned mc2.exe (image-name kill, not PID-specific), producing crash_silent
    # for whichever mission was in flight.  The lock is a flat file next to the
    # artifact root; open(..., 'x') is atomic on Windows (CreateFile CREATE_NEW).
    _LOCK_PATH = ARTIFACT_ROOT / "smoke.lock"
    _LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    try:
        _lf = open(_LOCK_PATH, 'x')
        _lf.write(str(os.getpid()))
        _lf.flush()
        atexit.register(lambda: _LOCK_PATH.unlink(missing_ok=True))
    except FileExistsError:
        # May be stale (previous run crashed without cleanup). Check the PID.
        try:
            _stale_pid = int(_LOCK_PATH.read_text().strip())
            _still_alive = subprocess.run(
                ["tasklist", "/FI", f"PID eq {_stale_pid}", "/NH", "/FO", "CSV"],
                capture_output=True, text=True).stdout.count(str(_stale_pid)) > 0
        except Exception:
            _still_alive = False
        if _still_alive:
            print(f"[runner] ERROR: another smoke run is already in progress "
                  f"(PID {_stale_pid}); wait for it to finish or remove "
                  f"{_LOCK_PATH} if it is stale.", file=sys.stderr)
            sys.exit(5)
        # Stale lock — re-acquire.
        _LOCK_PATH.unlink(missing_ok=True)
        _lf = open(_LOCK_PATH, 'x')
        _lf.write(str(os.getpid()))
        _lf.flush()
        atexit.register(lambda: _LOCK_PATH.unlink(missing_ok=True))

    # Existing-process safety.
    pids = _running_mc2()
    if pids:
        if args.kill_existing:
            print(f"[runner] killing existing mc2.exe PIDs {pids}", file=sys.stderr)
            _taskkill_mc2()
        else:
            print(f"[runner] ERROR: mc2.exe already running (PIDs {pids}); "
                  f"pass --kill-existing to override.", file=sys.stderr)
            sys.exit(4)

    if args.menu_canary:
        if args.tier or args.mission:
            ap.error("--menu-canary cannot be combined with --tier/--mission")
        timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
        artifact_dir = ARTIFACT_ROOT / timestamp
        artifact_dir.mkdir(parents=True, exist_ok=True)
        sys.exit(_run_menu_canary(Path(args.exe), Path(args.menu_script),
                                  artifact_dir, args.keep_logs, args.menu_settle))

    entries = manifest.parse_manifest(MANIFEST_PATH)
    if args.mission:
        wanted = set(args.mission)
        selected = []
        seen = set()
        for e in entries:
            if e.tier == "skip" or e.stem not in wanted or e.stem in seen:
                continue
            selected.append(e)
            seen.add(e.stem)
    elif args.tier:
        selected = [e for e in entries if e.tier == args.tier]
    else:
        ap.error("--tier or --mission required")

    if not selected:
        print("[runner] no missions selected", file=sys.stderr)
        sys.exit(0)

    baseline_data = baselines.load(BASELINE_PATH)
    timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
    artifact_dir = ARTIFACT_ROOT / timestamp
    artifact_dir.mkdir(parents=True, exist_ok=True)

    menu_canary_rc = None
    if args.with_menu_canary:
        print("[runner] running menu canary", file=sys.stderr)
        menu_canary_rc = _run_menu_canary(Path(args.exe), Path(args.menu_script),
                                          artifact_dir, args.keep_logs, args.menu_settle)
        if menu_canary_rc != 0 and args.fail_fast:
            print("[runner] --fail-fast: stopping after menu canary failure", file=sys.stderr)
            sys.exit(menu_canary_rc)

    rows: list[report.Row] = []
    for e in selected:
        duration = args.duration or e.duration or 120
        tier = args.tier or "adhoc"
        cfg = RunConfig(
            exe=[args.exe],
            profile=e.profile or args.profile,
            stem=e.stem,
            duration=duration,
            heartbeat_timeout_load_s=e.heartbeat_timeout_load or 60,
            heartbeat_timeout_play_s=e.heartbeat_timeout_play or 3,
            grace_s=60,
            allow_asset_oob=e.allow_asset_oob,
            env_extra={
                "MC2_SMOKE_SEED": "0xC0FFEE",
                # Propagate PatchStream env vars from parent if set —
                # subprocess.Popen's env arg replaces the inherited env
                # entirely, so vars not explicitly listed get dropped.
                **{k: v for k, v in os.environ.items()
                   if k in ("MC2_FX_COUNT_LOG",
                            "MC2_SCREENSHOT_AT_FRAME",
                            "MC2_SCREENSHOT_PATH",
                            "MC2_OBJECT_RECON_TRACY",
                            "MC2_MODERN_TERRAIN_SURFACE",
                            "MC2_MODERN_TERRAIN_PATCHES",
                            "MC2_SHAPE_C_PARITY_CHECK",
                            "MC2_PATCH_STREAM_TRACE",
                            "MC2_PATCH_STREAM_FORCE_INIT_FAIL",
                            "MC2_PATCHSTREAM_QUAD_RECORDS",
                            "MC2_PATCHSTREAM_QUAD_RECORDS_DRAW",
                            "MC2_PATCHSTREAM_THIN_RECORDS",
                            "MC2_PATCHSTREAM_THIN_RECORDS_DRAW",
                            "MC2_PATCHSTREAM_THIN_RECORD_FASTPATH",
                            "MC2_THIN_DEBUG",
                            "MC2_WATER_DEBUG",
                            "MC2_WATER_STREAM_DEBUG",
                            "MC2_RENDER_WATER_FASTPATH",
                            "MC2_RENDER_WATER_PARITY_CHECK",
                            "MC2_VERTEX_PROJECT_FAST",
                            "MC2_VERTEX_PROJECT_PARITY",
                            "MC2_TERRAIN_INDIRECT",
                            "MC2_TERRAIN_INDIRECT_PARITY_CHECK",
                            "MC2_TERRAIN_INDIRECT_TRACE",
                            # Decal static-bake (drawPass-retirement Slice A) kill-switch
                            "MC2_TERRAIN_INDIRECT_OVERLAY",
                            # Ring-buffer hazard probe (raster-triangle bug)
                            "MC2_RING_TRACE",
                            # Probe 7: force glFinish() between compute and draw
                            "MC2_RING_FORCE_FINISH",
                            # GPU object batcher gate (bisect partner for terrain bug)
                            "MC2_GPU_OBJECTS",
                            # Shadow-lane gated features (smoke coverage for the
                            # dynamic sun-shadow caster path + static building map).
                            "MC2_SHADOW_ENABLE",
                            "MC2_STATIC_PROP_BUILDING_SHADOW",
                            "MC2_SHADOW_DYNAMIC_PROP_CASTERS",
                            "MC2_SHADOW_BOUNDED_NEAR_FIT",
                            "MC2_SHADOW_BOUNDED_NEAR_RADIUS",
                            "MC2_SHADOW_FRUSTUM_DIAG",
                            # SHADOW-CASTER-LIGHTBOX-CULL-1 (2026-06-04): per-frame
                            # cull of dynamic prop shadow casters to the shadow
                            # frustum (default OFF). Without these in the allowlist
                            # subprocess.Popen drops them and the gate-ON smoke
                            # state is meaningless (cull never engages).
                            "MC2_SHADOW_CASTER_LIGHTBOX_CULL",
                            "MC2_SHADOW_CASTER_CULL_MARGIN",
                            "MC2_SHADOW_CULL_DEBUG",
                            # SHADOW-FOCUS-CENTER-1 (2026-06-04): center the
                            # dynamic shadow box on the camera near-ground focus
                            # point instead of the frustum-corner AABB centroid
                            # (default OFF). Popen replaces env -- without these
                            # in the allowlist the gate-ON smoke does nothing.
                            "MC2_SHADOW_FOCUS_CENTER",
                            "MC2_SHADOW_FOCUS_DIST",
                            # Mask-dispatch (pre-bake-terrain merge)
                            "MC2_TERRAIN_MASK_DISPATCH",
                            "MC2_TERRAIN_MASK_DISPATCH_PARITY",
                            "MC2_TERRAIN_INDIRECT_MINE",
                            "MC2_TERRAIN_INDIRECT_OVERLAY",
                            "MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK",
                            "MC2_TERRAIN_COST_SPLIT",
                            "MC2_LIGHT_COST_SPLIT",
                            "MC2_SLIM_COST_SPLIT",
                            "MC2_TOBJ_COST_SPLIT",
                            "MC2_STATIC_PROP_FLUSH_COST_SPLIT",
                            "MC2_STATIC_PROP_FLUSH_CACHED_BLOB",
                            "MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE",
                            "MC2_STATIC_PROP_COLORS_FILL",
                            "MC2_STATIC_PROP_PERSISTENT_BUCKETS",
                            "MC2_STATIC_PROP_PERSISTENT_BUCKETS_COMPARE",
                            "MC2_BUCKET_ORDER_TRACE",
                            # GPU-cull ownership-port Slice A: cut-off upper-bound oracle
                            "MC2_GPU_CULL_OWNERSHIP_PARITY",
                            # Task 7 — superset-parity counter probe (proof-gate #2)
                            "MC2_TOBJ_PARITY",
                            # alpha-Stage 1 §5 Stage 0 — candidate-predicate
                            # disagreement probe for inView unconflation.
                            "MC2_INVIEW_CONFLATION_TRACE",
                            "MC2_GPUPROPS_TRACE",
                            "MC2_MECH_RESTORE_TRACE",
                            # Phase 1 — terrain lighting GPU compute
                            "MC2_TERRAIN_LIGHTING_GPU",
                            "MC2_TERRAIN_LIGHTING_PARITY",
                            "MC2_TERRAIN_LIGHTING_GPU_TRACE",
                            # Track A1 — object admission predicate + trace
                            "MC2_OBJECT_ADMISSION_PREDICATE",
                            "MC2_PROJECTZ_TRACE",
                            "MC2_PROJECTZ_HEATMAP",
                            "MC2_PROJECTZ_SUMMARY",
                            "MC2_PROJECTZ_GUARD_PX",
                            # Task 7 — cascade-safety DESTROY trace
                            "MC2_DESTROY_TRACE",
                            # Track C0/C1a — GPU cull substrate + AABB parity + compute dispatch
                            "MC2_GPU_CULL_SUBSTRATE",
                            "MC2_GPU_CULL_AABB_PARITY",
                            "MC2_GPU_CULL_SUBSTRATE_TRACE",
                            "MC2_GPU_CULL",
                            "MC2_GPU_CULL_COMPUTE_TRACE",
                            "MC2_CAMERA_MOVE_DIAG",
                            "MC2_STATIC_FORCE_ADMIT",
                            "MC2_WATER_GATE_DIAG",
                            "MC2_MISSION_INTRO_LEGACY_RENDER",
                            # Track C2 — async readback ring buffer
                            "MC2_GPU_CULL_READBACK",
                            "MC2_GPU_CULL_FORCE_FENCE_NOT_READY",
                            "MC2_GPU_CULL_READBACK_TRACE",
                            # Track C3 — lifecycle gates (mech3d/gvactor/objmgr routing)
                            # m-4: missing permutation: LIFECYCLE=1+READBACK=1 and
                            # LIFECYCLE=1+READBACK=0 (fail-open regression) smoke runs.
                            # Both require --with-env overrides; not automated yet.
                            "MC2_GPU_CULL_LIFECYCLE",
                            "MC2_GPU_CULL_LIFECYCLE_TRACE",
                            # Phase C — GPU-driven unified path
                            "MC2_GPU_DRIVEN",
                            "MC2_GPU_DRIVEN_WATER",
                            "MC2_GPU_DRIVEN_TERRAIN_SOLID",
                            "MC2_GPU_DRIVEN_OVERLAY",
                            "MC2_GPU_DRIVEN_PARITY",
                            "MC2_GPU_DRIVEN_TRACE",
                            # Reverse-Z depth verification probes (2026-05-18):
                            # depth-transition zoom-pop, water render/depth
                            # parity, reverse-Z proj-matrix lifecycle trace.
                            "MC2_DEPTH_TRANSITION_PROBE",
                            "MC2_WATER_RENDERPROBE",
                            "MC2_WATER_DEPTHPROBE",
                            "MC2_REVERSE_Z_TRACE",
                            # Terrain continuous-surface producer (2026-05-18):
                            # MC2_TERRAIN_SURFACE = path-select kill-switch
                            # (PR-1+, default-OFF); MC2_TERRAIN_SURFACE_TRACE =
                            # the PR-0 [TERRAIN_SURFACE v1] lifecycle trace gate
                            # (trace-only, default-OFF). Without these in the
                            # allowlist subprocess.Popen drops them and the
                            # forced-ON / trace smoke states are meaningless.
                            "MC2_TERRAIN_SURFACE",
                            "MC2_TERRAIN_SURFACE_TRACE",
                            # quadSetupTextures-retirement recon (2026-05-19):
                            # MC2_WATER_INVPROJ_PARITY = the [WATER_INVPROJ v1]
                            # snapshot-A-vs-B 6-tuple parity probe (terrain.cpp;
                            # trace-only, default-OFF). Decides whether the
                            # setupTextures water block contributes unique
                            # extrema beyond slimReduce. Without this in the
                            # allowlist subprocess.Popen drops it and the probe
                            # never fires.
                            "MC2_WATER_INVPROJ_PARITY",
                            # F3 CPU-projection cost-split (2026-05-20):
                            # MC2_CPU_PROJ_COST_SPLIT = master env gate for the
                            # measurement-only bucket/sidecar instrumentation
                            # (mclib/cpu_proj_cost_split.{h,cpp}). Without this
                            # in the allowlist subprocess.Popen drops it and
                            # tier1 captures emit "[CPU_PROJ v1 disabled]".
                            "MC2_CPU_PROJ_COST_SPLIT",
                            # Tier 1.2 — KHR_debug fatal-on-HIGH opt-in
                            # (docs/testing-strategy.md). Forwarded so
                            # --gl-debug-fatal reaches the engine subprocess.
                            "MC2_GL_DEBUG_FATAL",
                            # Integrated gosFX-retirement / GPU-particles plan
                            # (2026-05-20-integrated-gosfx-retirement-*):
                            # MC2_FX_TRACE = neutral fx invocation counter
                            # (mclib/fx_trace; default-OFF). Without this in
                            # the allowlist subprocess.Popen drops it and the
                            # Stage 0' content-recon trace never fires.
                            # MC2_DISABLE_GOSFX = A1 MLR work-leaf gate
                            # (default-ON since A2). Forwarded so Stage 0'
                            # captures can opt back to legacy-gosFX-ON for
                            # the histogram-baseline run only.
                            "MC2_FX_TRACE",
                            "MC2_DISABLE_GOSFX",
                            # B1 Stage 1'+: GPU particle batcher opt-in gate.
                            # (default-OFF; flipped ON for Stage 1' canaries.)
                            "MC2_GPU_PARTICLES",
                            # VFX oracle render gate (CPU-side harvest path).
                            # MC2_VFX_ORACLE_RENDER=1 enables the oracle path in
                            # CardCloud/ShardCloud/Card Draw() (default-OFF).
                            # MC2_GPU_PARTICLES_LOG=1 enables [VFX_ORACLE v1] stderr.
                            "MC2_VFX_ORACLE_RENDER",
                            "MC2_GPU_PARTICLES_LOG",
                            # VFX-DEBUG-VIEWS-1: particle billboard debug mode.
                            # 0=Final (default/byte-identical), 1=Albedo, 2=Alpha,
                            # 3=ParticleKind, 4=Overdraw, 5=Age (VFX-SHADER-AGE-FADE-PARITY-1).
                            "MC2_VFX_DEBUG_MODE",
                            # VFX-TUNING-UI-1: look-only VFX intensity scales.
                            # All default 1.0 or 0.0 (byte-identical no-ops).
                            "MC2_TUNE_VFX_BRIGHTNESS",
                            "MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS",
                            "MC2_TUNE_VFX_ALPHA_SCALE",
                            # VFX-SHADER-AGE-FADE-PARITY-1: age-driven soft death
                            # fade for oracle particles. Default 0.0 = byte-identical.
                            "MC2_TUNE_VFX_AGE_FADE",
                            # VFX-SOFT-PARTICLES-MVP-1 / VFX-LIT-PARTICLES-MVP-1
                            "MC2_VFX_SOFT_PARTICLES",
                            "MC2_VFX_LIT_PARTICLES",
                            "MC2_TUNE_VFX_LIT_STRENGTH",
                            # StaticPropTypeTable v0 Task 5: candidate draw log
                            # proof gate. MC2_TYPE_TABLE_CAND_LOG=1 enables the
                            # [DRAW_CAND v0] summary line every frame.
                            # MC2_TYPE_TABLE_CAND_VERBOSE=1 also enables per-packet
                            # detail lines every frame (not just first-frame).
                            "MC2_TYPE_TABLE_CAND_LOG",
                            "MC2_TYPE_TABLE_CAND_VERBOSE",
                            # DrawPacket v2 per-frame field compare gate.
                            # MC2_DRAW_PACKET_COMPARE=1 enables [DRAW_PACKET_COMPARE v1]
                            # summary lines. MC2_DRAW_PACKET_COMPARE_VERBOSE=1 adds per-packet detail.
                            "MC2_DRAW_PACKET_COMPARE",
                            "MC2_DRAW_PACKET_COMPARE_VERBOSE",
                            # DrawPacket v3 ABI-promotion probe gate.
                            # MC2_DRAW_PACKET_V3=1 enables [DRAW_PACKET v3] build-stats line per frame.
                            "MC2_DRAW_PACKET_V3",
                            # DrawPacket v4B count-level coalesce coverage compare.
                            "MC2_DRAW_PACKET_COALESCE_COMPARE",
                            "MC2_DRAW_PACKET_COALESCE_VERBOSE",
                            # DrawPacket v4C slot-level coalesce coverage soak.
                            "MC2_DRAW_PACKET_COALESCE_V4C",
                            # DrawPacket v5 substitutive per-draw-call dispatch.
                            "MC2_DRAW_PACKET_COALESCE_V5",
                            "MC2_DRAW_PACKET_COALESCE_V5_TRACE",
                            # DrawPacket v6 canonical packet+meta array dispatch.
                            "MC2_DRAW_PACKET_STATIC_PROP_V6",
                            "MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE",
                            # DrawPacket v7 kill-switch (reverts to legacy multidraw).
                            "MC2_STATIC_PROP_LEGACY_DISPATCH",
                            # DrawPacket v8 kill-switch: =1 restores live build + snapshot compare.
                            "MC2_STATIC_PROP_LIVE_BUILDER",
                            # MaterialGpu v7 kill-switch (set =0 to disable shader sampling).
                            "MC2_MATERIAL_GPU_SAMPLE",
                            # MaterialGpu master kill-switch (set =0 to disable table upload).
                            "MC2_MATERIAL_GPU",
                            # Extraction v2.3: snapshot-assisted static-prop
                            # snap-cull gate. MC2_SNAP_CULL=1 enables the v6
                            # dispatch loop to skip draw slots whose instanceCount
                            # was zero in the previous frame's RenderSnapshot. Opt-in
                            # for testing; requires snap->ok==1. Forwarded in the
                            # allowlist so tier1 smoke runs can use this kill-switch.
                            "MC2_SNAP_CULL",
                            # Extraction v3 snapshot build gate + log gate.
                            # MC2_SNAPSHOT_STATIC_PROP_BUILD=1 enables the v3
                            # draw-packet builder from snapshot rows. Forwarded so
                            # tier1 forced-ON canaries work correctly.
                            # MC2_RENDER_SNAPSHOT_LOG=1 enables the per-frame
                            # [RENDER_SNAPSHOT v3] + [v3 build] log lines (used
                            # to verify spBuild* counters in smoke step 2).
                            "MC2_SNAPSHOT_STATIC_PROP_BUILD",
                            "MC2_RENDER_SNAPSHOT_LOG",
                            # MC2_SNAPSHOT_MECH_EXTRACT=1 enables MECH-EXTRACTION-0
                            # persist buffer + compare counters (gate OFF by default).
                            "MC2_SNAPSHOT_MECH_EXTRACT",
                            # F1-3A ViewUniforms UBO upload gate (default-OFF).
                            # MC2_VIEW_UNIFORMS=1 uploads per-frame view matrices
                            # to UBO at binding=3. No shader consumption yet (F1-3B).
                            "MC2_VIEW_UNIFORMS",
                            # StaticPropAmbientV1 hemisphere ambient term (default-OFF).
                            # MC2_STATIC_PROP_AMBIENT_V1=1 enables the ambient V1 path.
                            "MC2_STATIC_PROP_AMBIENT_V1",
                            # V-MATERIAL-DEBUG-1 gated debug material views
                            # (default-OFF). 1=albedo, 2=materialIdx, 3=normal,
                            # 4=texArrayLayer, 5=roughness, 6=metallic.
                            # Clamped 0..6 by CPU.
                            "MC2_STATIC_PROP_DEBUG_MATERIAL",
                            # V-IBL-STATIC-1 SH-L2 image-based ambient (default-ON).
                            # MC2_STATIC_PROP_IBL_SH=0 disables; ImGui slider
                            # tunes u_iblShStrength when the gate is on.
                            "MC2_STATIC_PROP_IBL_SH",
                            # V-IBL-STATIC-1-SOAK: optional default strength
                            # override (clamped 0..3). Only meaningful when
                            # MC2_STATIC_PROP_IBL_SH=1. ImGui slider may
                            # still override at runtime.
                            "MC2_STATIC_PROP_IBL_SH_STRENGTH",
                            # V-IBL-STATIC-2: optional SH-set override by name
                            # (e.g. "default"). Unset/empty/unknown falls back
                            # to mission registry or default set. Dev knob.
                            "MC2_STATIC_PROP_IBL_SH_SET",
                            # V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel
                            # + power-lobe specular gate (default-OFF).
                            # MC2_STATIC_PROP_PBR_V1=1 enables; ImGui slider
                            # tunes u_pbrV1Strength when on.
                            "MC2_STATIC_PROP_PBR_V1",
                            # V-MATERIAL-PBR-3: optional default-strength
                            # override (clamped 0..3). Only meaningful when
                            # MC2_STATIC_PROP_PBR_V1=1.
                            "MC2_STATIC_PROP_PBR_V1_STRENGTH",
                            # V-MATERIAL-PBR-3-DIAG: diagnostic sunFound
                            # visualizer (cyan/magenta). Default-OFF; only
                            # meaningful with MC2_STATIC_PROP_PBR_V1=1.
                            "MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND",
                            # STATICPROP-MATERIAL-ORM-1: per-bucket linear ORM
                            # (occlusion-roughness-metallic) texture slots +
                            # sidecar feed (default-OFF). =1 enables.
                            "MC2_STATICPROP_MATERIAL_PBR_SLOTS",
                            # STATICPROP-MATERIAL-ORM-1: ORM strength tuning
                            # (only meaningful with MC2_STATICPROP_MATERIAL_PBR_SLOTS=1).
                            "MC2_STATICPROP_ORM_STRENGTH",
                            # Render-contract Phase 2 assert mode (default-OFF).
                            # MC2_RENDER_CONTRACT_ASSERT=1 enables runtime GL state
                            # validation against declared render_contract expectations.
                            "MC2_RENDER_CONTRACT_ASSERT",
                            # DEBUG-STATE-DUMP-1: JSON render-state snapshots.
                            # Default-OFF; =1 writes debug_state/latest_render_state.json.
                            "MC2_DEBUG_STATE_DUMP",
                            # DEBUG-STATE-DUMP-1: override output directory.
                            "MC2_DEBUG_STATE_DUMP_DIR",
                            # DEBUG-STATE-DUMP-2: rolling 8-slot history ring.
                            "MC2_DEBUG_STATE_DUMP_HISTORY",
                            # STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1: independent
                            # registry cache + field-by-field compare probe.
                            # Default-OFF; =1 emits [SNAPSHOT_BRIDGE_COMPARE v1]
                            # per-frame to stderr.
                            "MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE",
                            # STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1: clean-generation
                            # fast path that skips fillStaticPropSlots + WriteLoop
                            # and memcpy's cached rows into the snapshot arena.
                            # Default-OFF; =1 enables the dirty-only path.
                            "MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY",
                            # Terrain fast-path drop transition log (default-OFF).
                            # MC2_FASTPATH_DROP_LOG=1 emits [FASTPATH_DROP] on
                            # armed<->fallback transitions in terrain.cpp geometry().
                            "MC2_FASTPATH_DROP_LOG")},
            },
        )
        # Clear the file-sink probe log next to mc2.exe before each mission
        # so each run captures its own probe events.  See gos_terrain_indirect
        # PROBE_LOG / RING_SINK in the engine: ring-buffer / cmd-patch /
        # overshoot / near-clip-w / recipe-spread tripwires.
        ring_sink_path = Path(cfg.exe[0]).resolve().parent / "ring_trace.log"
        try:
            ring_sink_path.unlink(missing_ok=True)
        except Exception:
            pass

        print(f"[runner] running {e.stem} (tier={tier} duration={duration})",
              file=sys.stderr)
        result = run_one(cfg)

        key = baselines.key(cfg.profile, e.stem, tier, duration)
        delta = baselines.destroys_delta(baseline_data, key, result.summary.destroys)

        rows.append(report.Row(stem=e.stem, verdict=result.verdict,
                               summary=result.summary, destroys_delta=delta or 0))

        if not result.verdict.passed or args.keep_logs:
            (artifact_dir / f"{e.stem}.log").write_text(result.stdout_text, encoding="utf-8", errors="replace")
            # Snapshot the probe file-sink alongside the regular log.  Survives
            # across runs (the per-mission unlink above resets it for next).
            try:
                if ring_sink_path.exists():
                    (artifact_dir / f"{e.stem}.ring_trace.log").write_text(
                        ring_sink_path.read_text(encoding="utf-8", errors="replace"),
                        encoding="utf-8", errors="replace")
            except Exception as exc:
                print(f"[runner] could not snapshot ring_trace.log: {exc}", file=sys.stderr)
        if args.baseline_update and result.verdict.passed:
            baseline_data.setdefault(key, {})["destroys"] = {
                "mean": result.summary.destroys, "stddev": 0, "samples": 1,
                "updated": timestamp,
            }
            baseline_data[key]["perf"] = {
                "avg_fps": result.summary.perf.avg_fps,
                "p1low_fps": result.summary.perf.p1low_fps,
                "peak_ms": result.summary.perf.peak_ms,
            }

        if args.fail_fast and not result.verdict.passed:
            print(f"[runner] --fail-fast: stopping at {e.stem}", file=sys.stderr)
            break

        # 2s grace for PDB lock release before next spawn.
        import time as _t; _t.sleep(2)

    md = report.render_markdown(rows, tier=args.tier or "adhoc",
                                 profile=args.profile, timestamp=timestamp)
    (artifact_dir / "report.md").write_text(md, encoding="utf-8")
    (artifact_dir / "report.json").write_text(
        json.dumps(report.render_json(rows, tier=args.tier or "adhoc",
                                      profile=args.profile, timestamp=timestamp),
                   indent=2), encoding="utf-8")

    if args.baseline_update:
        baselines.save(BASELINE_PATH, baseline_data)

    sys.stdout.buffer.write(md.encode("utf-8"))
    sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()
    passed = all(r.verdict.passed for r in rows) and (menu_canary_rc in (None, 0))
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
