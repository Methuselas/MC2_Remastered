#!/usr/bin/env python3
"""
MC2 Render-State MCP Server (MC2-MCP-WRAPPER-1)

Read-only bridge between Claude agents and the running MC2 engine.
Reads JSON snapshots written by MC2_DEBUG_STATE_DUMP=1.

Configuration (env vars):
  MC2_DEPLOY_DIR  — path to mc2-win64-v0.4 deploy directory
                    default: A:/Games/mc2-opengl/mc2-win64-v0.4

The engine must be running with MC2_DEBUG_STATE_DUMP=1 for state reads to
return live data. History tools additionally require MC2_DEBUG_STATE_DUMP_HISTORY=1.

No gameplay, renderer, or feature-gate mutation. Read-only.
"""

import json
import os
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
    return json.dumps(data, indent=2)


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
    lines = [f"# Feature gates — snapshot at frame {frame}"]
    for k, v in features.items():
        lines.append(f"  {k}: {v}")
    return "\n".join(lines)


@mcp.tool()
def get_visual_settings() -> str:
    """
    Return StaticPropOpaque visual settings: IBL/SH ambient, PBR specular,
    material debug mode, dispatch path.

    These are the runtime slider values and gate states for the
    StaticPropOpaque render lane. IBL/PBR strengths may change during
    the run via ImGui sliders; this reflects the value at the last dump.
    """
    data = _latest()
    if data is None:
        return _not_available()

    sp = data.get("staticPropOpaque", {})
    frame = data.get("frame", "?")

    return json.dumps({
        "frame": frame,
        "dispatch": {
            "snapshotDefault": sp.get("snapshotDispatchDefault"),
            "legacy": sp.get("legacyDispatch"),
        },
        "material": {
            "gpuEnabled": sp.get("materialGpuEnabled"),
            "gpuSample": sp.get("materialGpuSample"),
            "debugMode": sp.get("debugMaterialMode"),
        },
        "iblSh": {
            "enabled": sp.get("iblShEnabled"),
            "strength": sp.get("iblShStrength"),
            "set": sp.get("iblShSet"),
        },
        "pbr": {
            "enabled": sp.get("pbrEnabled"),
            "strength": sp.get("pbrStrength"),
            "roughnessOverrideEnabled": sp.get("pbrRoughnessOverrideEnabled"),
            "roughnessOverride": sp.get("pbrRoughnessOverride"),
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

    if errors:
        lines = [f"FAIL — {len(errors)} error(s) in snapshot at frame {frame}:"]
        for e in errors:
            lines.append(f"  - {e}")
        return "\n".join(lines)

    ok = data.get("renderSnapshot", {}).get("ok", False)
    return f"PASS — schema valid, frame={frame}, renderSnapshot.ok={ok}"


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

    return "\n".join([
        f"schema:  {schema}",
        f"frame:   {frame}",
        f"mission: {mission.get('name', '')} (known={mission.get('known', False)})",
        f"build:   config={build.get('config', '?')} commit={build.get('commit', '?')}",
        f"stateDir: {_STATE_DIR}",
    ])


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    mcp.run(transport="stdio")
