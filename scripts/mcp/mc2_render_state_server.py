#!/usr/bin/env python3
"""
MC2 Render-State MCP Server (MC2-MCP-WRAPPER-1 + MC2-MCP-WRAPPER-2)

Read-only bridge between Claude agents and the running MC2 engine.
Reads JSON snapshots written by MC2_DEBUG_STATE_DUMP=1.
Also wraps run_smoke.py to capture baselines and read artifact reports.

Configuration (env vars):
  MC2_DEPLOY_DIR  — path to mc2-win64-v0.4 deploy directory
                    default: A:/Games/mc2-opengl/mc2-win64-v0.4

The engine must be running with MC2_DEBUG_STATE_DUMP=1 for state reads to
return live data. History tools additionally require MC2_DEBUG_STATE_DUMP_HISTORY=1.

No gameplay, renderer, or feature-gate mutation. Read-only.
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

_DEPLOY_DIR = Path(os.environ.get("MC2_DEPLOY_DIR", "A:/Games/mc2-opengl/mc2-win64-v0.4"))
_STATE_DIR = _DEPLOY_DIR / "debug_state"
_LATEST = _STATE_DIR / "latest_render_state.json"
_HISTORY_SLOTS = 8
_SCHEMA = "MC2_DEBUG_STATE_V1"

# Worktree root — two levels up from scripts/mcp/
_WORKTREE_DIR = Path(__file__).resolve().parents[2]
_ARTIFACTS_DIR = _WORKTREE_DIR / "tests" / "smoke" / "artifacts"
_RUN_SMOKE = _WORKTREE_DIR / "scripts" / "run_smoke.py"

_VALID_MISSION = re.compile(r"^[a-z0-9_]{1,32}$")

mcp = FastMCP(
    "mc2-render-state",
    instructions=(
        "Read-only access to MC2 engine render state. "
        "The engine must be running with MC2_DEBUG_STATE_DUMP=1. "
        "State updates every 300 frames (~5s at 60fps). "
        "All tools are safe — no gameplay or renderer mutation."
    ),
)

# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _read_json(path: Path) -> dict[str, Any] | None:
    """Read and parse a JSON file. Returns None on missing/invalid."""
    if not path.exists():
        return None
    try:
        text = path.read_text(encoding="utf-8")
        return json.loads(text)
    except (json.JSONDecodeError, OSError):
        # Engine may be mid-write; retry once after a short sleep
        time.sleep(0.05)
        try:
            text = path.read_text(encoding="utf-8")
            return json.loads(text)
        except Exception:
            return None


def _latest() -> dict[str, Any] | None:
    return _read_json(_LATEST)


_STALE_THRESHOLD_S = 30  # seconds; dump cadence ~5s at 60fps so 30s = clearly not running


def _file_age_s() -> float | None:
    """Return age of latest snapshot file in seconds, or None if missing."""
    try:
        mtime = _LATEST.stat().st_mtime
        return time.time() - mtime
    except OSError:
        return None


def _stale_banner() -> str:
    """Return a STALE warning line if the snapshot is old, else empty string."""
    age = _file_age_s()
    if age is None:
        return ""
    if age >= _STALE_THRESHOLD_S:
        return f"*** STALE — file is {age:.0f}s old; game likely not running or MC2_DEBUG_STATE_DUMP=1 not set\n"
    return ""


def _not_available(detail: str = "") -> str:
    msg = (
        f"State file not available: {_LATEST}\n"
        "Ensure the engine is running with MC2_DEBUG_STATE_DUMP=1.\n"
        f"Deploy dir: {_DEPLOY_DIR}"
    )
    if detail:
        msg += f"\nDetail: {detail}"
    return msg


def _validate(data: dict[str, Any]) -> list[str]:
    """Return list of validation errors (empty = PASS)."""
    errors: list[str] = []

    def require(obj, key, typ, path=""):
        full = f"{path}.{key}" if path else key
        if key not in obj:
            errors.append(f"missing: {full}")
            return False
        val = obj[key]
        if not isinstance(val, typ) or (typ is int and isinstance(val, bool)):
            errors.append(f"{full}: expected {typ.__name__}, got {type(val).__name__}")
            return False
        return True

    if not isinstance(data, dict):
        errors.append("root is not an object")
        return errors

    if data.get("schema") != _SCHEMA:
        errors.append(f"schema: expected {_SCHEMA!r}, got {data.get('schema')!r}")

    require(data, "frame", int)
    for section in ("mission", "build", "features", "engineView", "renderSnapshot", "staticPropOpaque"):
        if section not in data:
            errors.append(f"missing section: {section}")

    rs = data.get("renderSnapshot", {})
    for k in ("staticPropValidationFail", "staticPropPacketRangesFail",
              "staticPropPacketInvalid", "spBuildCountMismatch",
              "spBuildPacketMismatch", "spBuildMetaMismatch"):
        require(rs, k, int, "renderSnapshot")
    require(rs, "ok", bool, "renderSnapshot")
    require(rs, "arenaOverflow", bool, "renderSnapshot")

    sp = data.get("staticPropOpaque", {})
    for k in ("snapshotDispatchDefault", "legacyDispatch", "materialGpuEnabled",
              "materialGpuSample", "iblShEnabled", "pbrEnabled", "pbrRoughnessOverrideEnabled"):
        require(sp, k, bool, "staticPropOpaque")
    if sp.get("snapshotDispatchDefault") and sp.get("legacyDispatch"):
        errors.append("staticPropOpaque: snapshotDispatchDefault and legacyDispatch both true")

    return errors

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@mcp.tool()
def get_render_state() -> str:
    """
    Return the full latest MC2 render-state snapshot as JSON.

    Includes: schema, frame, mission, build, feature gates, engineView,
    renderSnapshot counters, and staticPropOpaque visual globals.
    Requires engine running with MC2_DEBUG_STATE_DUMP=1.
    """
    data = _latest()
    if data is None:
        return _not_available()
    return _stale_banner() + json.dumps(data, indent=2)


@mcp.tool()
def get_render_health() -> str:
    """
    Return a compact render health summary.

    Reports: frame, mission, renderSnapshot.ok, all mismatch counters,
    arenaOverflow, and any failing counters highlighted. Fast diagnostic check.
    """
    data = _latest()
    if data is None:
        return _not_available()

    rs = data.get("renderSnapshot", {})
    sp_opaque = data.get("staticPropOpaque", {})
    mission = data.get("mission", {})
    frame = data.get("frame", "?")
    ok = rs.get("ok", False)

    lines = [
        f"frame:   {frame}",
        f"mission: {mission.get('name', '?')} (known={mission.get('known', False)})",
        f"ok:      {ok}",
    ]

    counters = {
        "staticPropValidationFail":  rs.get("staticPropValidationFail", "?"),
        "staticPropPacketRangesFail": rs.get("staticPropPacketRangesFail", "?"),
        "staticPropPacketInvalid":   rs.get("staticPropPacketInvalid", "?"),
        "arenaOverflow":             rs.get("arenaOverflow", "?"),
        "spBuildCountMismatch":      rs.get("spBuildCountMismatch", "?"),
        "spBuildPacketMismatch":     rs.get("spBuildPacketMismatch", "?"),
        "spBuildMetaMismatch":       rs.get("spBuildMetaMismatch", "?"),
        "spBuildFallback":           rs.get("spBuildFallback", "?"),
    }

    any_fail = False
    for k, v in counters.items():
        nonzero = v not in (0, False, "?")
        flag = " *** FAIL" if nonzero else ""
        if nonzero:
            any_fail = True
        lines.append(f"  {k}: {v}{flag}")

    lines.append("")
    lines.append(f"snapshotDispatch: {sp_opaque.get('snapshotDispatchDefault', '?')}")
    lines.append(f"materialGpu:      {sp_opaque.get('materialGpuEnabled', '?')} / sample={sp_opaque.get('materialGpuSample', '?')}")
    lines.append(f"iblSh:            {sp_opaque.get('iblShEnabled', '?')} strength={sp_opaque.get('iblShStrength', '?')} set={sp_opaque.get('iblShSet', '?')!r}")
    lines.append(f"pbr:              {sp_opaque.get('pbrEnabled', '?')} strength={sp_opaque.get('pbrStrength', '?')}")

    if any_fail:
        lines.insert(0, "HEALTH: FAIL — nonzero counters detected")
    elif not ok:
        lines.insert(0, "HEALTH: FAIL — ok=false")
    else:
        lines.insert(0, "HEALTH: PASS")

    banner = _stale_banner()
    if banner:
        lines.insert(0, banner.rstrip())

    age = _file_age_s()
    lines.append(f"\nfile_age: {age:.0f}s" if age is not None else "\nfile_age: unknown")

    return "\n".join(lines)


@mcp.tool()
def get_feature_gates() -> str:
    """
    Return current MC2 feature gate states as JSON.

    Shows which MC2_* feature env vars are active (true) or suppressed (false)
    at the time of the last snapshot. Gates are sampled once at process start;
    this reflects the launch-time env, not any runtime change.
    """
    data = _latest()
    if data is None:
        return _not_available()

    features = data.get("features", {})
    frame = data.get("frame", "?")
    lines = [_stale_banner().rstrip()] if _stale_banner() else []
    lines.append(f"# Feature gates — snapshot at frame {frame}")
    for k, v in features.items():
        lines.append(f"  {k}: {v}")
    return "\n".join(lines)


@mcp.tool()
def get_visual_settings() -> str:
    """
    Return full StaticPropOpaque visual state: render passes, shader path,
    dispatch, draw counts, material table sizes, IBL/SH, PBR, debug mode.

    Covers: what pass? what shader path? what material? what sliders?
    All fields reflect the value at the last dump (~300 frames / ~5s cadence).
    IBL/PBR strengths may change during the run via ImGui sliders.
    """
    data = _latest()
    if data is None:
        return _not_available()

    sp = data.get("staticPropOpaque", {})
    rp = data.get("renderPasses", {})
    frame = data.get("frame", "?")

    return _stale_banner() + json.dumps({
        "frame": frame,
        "renderPasses": {
            "shadow":       rp.get("shadow"),
            "screenShadow": rp.get("screenShadow"),
            "bloom":        rp.get("bloom"),
            "fxaa":         rp.get("fxaa"),
            "tonemap":      rp.get("tonemap"),
        },
        "dispatch": {
            "snapshotDefault": sp.get("snapshotDispatchDefault"),
            "legacy":          sp.get("legacyDispatch"),
            "shaderVariant":   sp.get("shaderVariant"),
        },
        "drawCounts": {
            "spV6DrawCalls":    sp.get("spV6DrawCalls"),
            "spAlphaOffPackets": sp.get("spAlphaOffPackets"),
        },
        "material": {
            "gpuEnabled":          sp.get("materialGpuEnabled"),
            "gpuSample":           sp.get("materialGpuSample"),
            "gpuTableSize":        sp.get("materialGpuTableSize"),
            "inventorySize":       sp.get("materialInventorySize"),
            "debugMode":           sp.get("debugMaterialMode"),
        },
        "iblSh": {
            "enabled":  sp.get("iblShEnabled"),
            "strength": sp.get("iblShStrength"),
            "set":      sp.get("iblShSet"),
        },
        "pbr": {
            "enabled":                sp.get("pbrEnabled"),
            "strength":               sp.get("pbrStrength"),
            "roughnessOverrideEnabled": sp.get("pbrRoughnessOverrideEnabled"),
            "roughnessOverride":      sp.get("pbrRoughnessOverride"),
        },
    }, indent=2)


@mcp.tool()
def get_history(slot: int) -> str:
    """
    Return a historical render-state snapshot from the rolling ring.

    slot: 0..7 (0 = oldest written, 7 = most recently written, wrapping).
    Requires the engine was launched with MC2_DEBUG_STATE_DUMP_HISTORY=1.
    Each slot covers 300 frames of history.
    """
    if not 0 <= slot < _HISTORY_SLOTS:
        return f"Invalid slot {slot}. Valid range: 0..{_HISTORY_SLOTS - 1}."

    path = _STATE_DIR / f"history_{slot}.json"
    data = _read_json(path)
    if data is None:
        return (
            f"History slot {slot} not available: {path}\n"
            "Ensure engine was launched with MC2_DEBUG_STATE_DUMP=1 and MC2_DEBUG_STATE_DUMP_HISTORY=1."
        )
    return json.dumps(data, indent=2)


@mcp.tool()
def validate_state() -> str:
    """
    Validate the latest render-state snapshot against the MC2_DEBUG_STATE_V1 schema.

    Checks: schema version, required fields, types, value ranges, consistency
    invariants (e.g. snapshotDispatchDefault XOR legacyDispatch).
    Returns PASS or FAIL with specific errors.
    """
    data = _latest()
    if data is None:
        return _not_available()

    errors = _validate(data)
    frame = data.get("frame", "?")
    banner = _stale_banner()

    if errors:
        lines = [f"FAIL — {len(errors)} error(s) in snapshot at frame {frame}:"]
        for e in errors:
            lines.append(f"  - {e}")
        return banner + "\n".join(lines)

    ok = data.get("renderSnapshot", {}).get("ok", False)
    return banner + f"PASS — schema valid, frame={frame}, renderSnapshot.ok={ok}"


@mcp.tool()
def get_frame_info() -> str:
    """
    Return frame index, mission name, and build configuration from the snapshot.

    Lightweight call to orient an agent to what mission is loaded,
    how many frames have elapsed, and what build config is running.
    """
    data = _latest()
    if data is None:
        return _not_available()

    frame = data.get("frame", "?")
    mission = data.get("mission", {})
    build = data.get("build", {})
    schema = data.get("schema", "?")

    age = _file_age_s()
    age_str = f"{age:.0f}s" if age is not None else "unknown"
    stale = age is not None and age >= _STALE_THRESHOLD_S

    views = data.get("registeredViews", [])
    view_names = [f"{v.get('viewKind', '?')}(id={v.get('viewId', '?')})" for v in views]

    lines = [
        f"schema:   {schema}",
        f"frame:    {frame}",
        f"mission:  {mission.get('name', '')} (known={mission.get('known', False)})",
        f"build:    config={build.get('config', '?')} commit={build.get('commit', '?')}",
        f"views:    [{', '.join(view_names)}]" if view_names else "views:    []",
        f"stateDir: {_STATE_DIR}",
        f"file_age: {age_str}" + (" *** STALE — game likely not running" if stale else ""),
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Capture / artifact helpers (MC2-MCP-WRAPPER-2)
# ---------------------------------------------------------------------------

def _latest_artifact_dir() -> Path | None:
    """Return the newest artifact directory, or None if none exist."""
    if not _ARTIFACTS_DIR.exists():
        return None
    dirs = sorted(
        (d for d in _ARTIFACTS_DIR.iterdir() if d.is_dir()),
        key=lambda d: d.name,
        reverse=True,
    )
    return dirs[0] if dirs else None


@mcp.tool()
def run_capture_baseline(mission: str = "mc2_01", duration: int = 30) -> str:
    """
    Run a single-mission smoke capture and return the report summary.

    Launches run_smoke.py with --tier adhoc for one mission. Blocks until
    the run completes (roughly duration + ~15s startup). Returns the parsed
    report rows as text, or an error message.

    mission: mission stem, e.g. mc2_01, mc2_10, mc2_24 (default: mc2_01)
    duration: seconds per mission, clamped to 10..120 (default: 30)
    """
    if not _VALID_MISSION.match(mission):
        return f"Invalid mission name {mission!r}. Use alphanumeric + underscore, max 32 chars."

    duration = max(10, min(120, duration))

    # run_smoke.py contract: missions are passed as repeated --mission (there is
    # no --missions flag and no 'adhoc' tier), and --kill-existing is forbidden
    # (it taskkills concurrent mc2.exe -> false crash_silent; run_smoke already
    # holds a concurrency-safe lock). See scripts/check-smoke-matrices.py.
    cmd = [
        "py", "-3", str(_RUN_SMOKE),
        "--mission", mission,
        "--duration", str(duration),
        "--keep-logs",
    ]

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=duration + 120,
            cwd=str(_WORKTREE_DIR),
        )
    except subprocess.TimeoutExpired:
        return f"run_smoke.py timed out after {duration + 120}s."
    except Exception as e:
        return f"Failed to launch run_smoke.py: {e}"

    artifact_dir = _latest_artifact_dir()
    if artifact_dir is None:
        return (
            f"run_smoke.py exited {result.returncode} but no artifact dir found.\n"
            f"stdout: {result.stdout[-1000:]}\nstderr: {result.stderr[-500:]}"
        )

    report_path = artifact_dir / "report.json"
    report = _read_json(report_path)
    if report is None:
        return (
            f"run_smoke.py exited {result.returncode}. No report.json in {artifact_dir}.\n"
            f"stdout: {result.stdout[-1000:]}"
        )

    lines = [
        f"exit: {result.returncode}",
        f"dir:  {artifact_dir.name}",
        f"tier: {report.get('tier', '?')}  profile: {report.get('profile', '?')}",
        "",
    ]
    for row in report.get("rows", []):
        r = row.get("result", "?")
        fps = row.get("avg_fps", "?")
        ready_ms = row.get("mission_ready_ms", "?")
        lines.append(f"  {row.get('stem', '?'):12s}  result={r}  fps={fps}  ready_ms={ready_ms}")

    return "\n".join(lines)


@mcp.tool()
def list_capture_sets() -> str:
    """
    List all smoke artifact sets, newest first.

    Each entry shows the timestamp directory name, tier, and per-mission results.
    Requires at least one prior run_capture_baseline() or run_smoke.py call.
    """
    if not _ARTIFACTS_DIR.exists():
        return f"Artifact directory not found: {_ARTIFACTS_DIR}"

    dirs = sorted(
        (d for d in _ARTIFACTS_DIR.iterdir() if d.is_dir()),
        key=lambda d: d.name,
        reverse=True,
    )
    if not dirs:
        return f"No artifact sets found in {_ARTIFACTS_DIR}"

    lines = [f"# Capture sets in {_ARTIFACTS_DIR} ({len(dirs)} total)"]
    for d in dirs:
        report = _read_json(d / "report.json")
        if report is None:
            lines.append(f"  {d.name}  (no report.json)")
            continue
        tier = report.get("tier", "?")
        rows = report.get("rows", [])
        summary = "  ".join(f"{r.get('stem', '?')}={r.get('result', '?')}" for r in rows)
        lines.append(f"  {d.name}  tier={tier}  {summary}")

    return "\n".join(lines)


@mcp.tool()
def summarize_latest_capture() -> str:
    """
    Return a detailed summary of the most recent smoke artifact set.

    Shows tier, profile, fps_note, and per-mission result + FPS + counters.
    """
    artifact_dir = _latest_artifact_dir()
    if artifact_dir is None:
        return f"No artifact sets found in {_ARTIFACTS_DIR}"

    report = _read_json(artifact_dir / "report.json")
    if report is None:
        return f"No report.json in {artifact_dir}"

    lines = [
        f"dir:      {artifact_dir.name}",
        f"tier:     {report.get('tier', '?')}",
        f"profile:  {report.get('profile', '?')}",
        f"fps_note: {report.get('fps_note', '(none)')}",
        "",
    ]
    for row in report.get("rows", []):
        lines.append(f"mission: {row.get('stem', '?')}")
        lines.append(f"  result:      {row.get('result', '?')}")
        lines.append(f"  avg_fps:     {row.get('avg_fps', '?')}")
        lines.append(f"  p1low_fps:   {row.get('p1low_fps', '?')}")
        lines.append(f"  ready_ms:    {row.get('mission_ready_ms', '?')}")
        lines.append(f"  destroys_d:  {row.get('destroys_delta', '?')}")
        buckets = row.get("buckets", {})
        if buckets:
            lines.append(f"  buckets:     {json.dumps(buckets)}")
        details = row.get("details", "")
        if details:
            lines.append(f"  details:     {details}")

    return "\n".join(lines)


@mcp.tool()
def get_latest_artifact_paths() -> str:
    """
    List all files in the most recent smoke artifact directory with sizes.

    Useful for locating log files, screenshots, or the report itself after a capture.
    """
    artifact_dir = _latest_artifact_dir()
    if artifact_dir is None:
        return f"No artifact sets found in {_ARTIFACTS_DIR}"

    files = sorted(artifact_dir.iterdir(), key=lambda f: f.name)
    lines = [f"# {artifact_dir}"]
    for f in files:
        if f.is_file():
            size_kb = f.stat().st_size / 1024
            lines.append(f"  {f.name:<40s}  {size_kb:8.1f} KB")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    mcp.run(transport="stdio")
