#!/usr/bin/env python3
"""
check-debug-state-json.py — validate a MC2_DEBUG_STATE_V1 JSON snapshot.

Usage:
    py -3 scripts/check-debug-state-json.py [path]

Default path: debug_state/latest_render_state.json (relative to cwd).

Exit 0 = PASS. Exit 1 = FAIL (prints specific errors).
"""

import json
import sys
from pathlib import Path

SCHEMA = "MC2_DEBUG_STATE_V1"

REQUIRED_FEATURES = [
    "MC2_DEBUG_STATE_DUMP",
    "MC2_VIEW_UNIFORMS",
    "MC2_SNAPSHOT_STATIC_PROP_BUILD",
    "MC2_MATERIAL_GPU",
    "MC2_MATERIAL_GPU_SAMPLE",
    "MC2_STATIC_PROP_IBL_SH",
    "MC2_STATIC_PROP_PBR_V1",
]

VIEW_MODES = {"Visual", "ObjectIdDebug", "TacticalOverlay", "Thermal", "Infrared", "LowLight"}

errors = []


def fail(msg):
    errors.append(msg)


def check_type(obj, key, expected_type, path=""):
    full = f"{path}.{key}" if path else key
    if key not in obj:
        fail(f"missing field: {full}")
        return False
    val = obj[key]
    if not isinstance(val, expected_type):
        fail(f"{full}: expected {expected_type.__name__}, got {type(val).__name__} ({val!r})")
        return False
    return True


def check_bool(obj, key, path=""):
    return check_type(obj, key, bool, path)


def check_int(obj, key, path=""):
    # JSON integers come through as int; reject float
    full = f"{path}.{key}" if path else key
    if key not in obj:
        fail(f"missing field: {full}")
        return False
    val = obj[key]
    if not isinstance(val, int) or isinstance(val, bool):
        fail(f"{full}: expected int, got {type(val).__name__} ({val!r})")
        return False
    return True


def check_float_or_int(obj, key, path=""):
    full = f"{path}.{key}" if path else key
    if key not in obj:
        fail(f"missing field: {full}")
        return False
    val = obj[key]
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        fail(f"{full}: expected number, got {type(val).__name__} ({val!r})")
        return False
    return True


def check_str(obj, key, path=""):
    return check_type(obj, key, str, path)


def validate(data):
    # Top-level schema
    if not isinstance(data, dict):
        fail("root is not a JSON object")
        return

    check_str(data, "schema")
    if data.get("schema") != SCHEMA:
        fail(f"schema: expected {SCHEMA!r}, got {data.get('schema')!r}")

    check_int(data, "frame")
    if "frame" in data and isinstance(data["frame"], int) and data["frame"] < 0:
        fail("frame: must be >= 0")

    # mission
    if "mission" not in data:
        fail("missing section: mission")
    else:
        m = data["mission"]
        check_str(m, "name", "mission")
        check_bool(m, "known", "mission")

    # build
    if "build" not in data:
        fail("missing section: build")
    else:
        bd = data["build"]
        check_str(bd, "commit", "build")
        if check_str(bd, "config", "build"):
            if bd["config"] not in {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}:
                fail(f"build.config: unexpected value {bd['config']!r}")

    # features
    if "features" not in data:
        fail("missing section: features")
    else:
        ft = data["features"]
        for key in REQUIRED_FEATURES:
            check_bool(ft, key, "features")
        if "MC2_DEBUG_STATE_DUMP" in ft and ft.get("MC2_DEBUG_STATE_DUMP") is not True:
            fail("features.MC2_DEBUG_STATE_DUMP: must be true (file only written when gate is on)")

    # engineView
    if "engineView" not in data:
        fail("missing section: engineView")
    else:
        ev = data["engineView"]
        check_bool(ev, "known", "engineView")
        check_int(ev, "viewId", "engineView")
        check_str(ev, "viewKind", "engineView")
        if check_str(ev, "viewMode", "engineView"):
            if ev["viewMode"] not in VIEW_MODES:
                fail(f"engineView.viewMode: unknown value {ev['viewMode']!r} (expected one of {sorted(VIEW_MODES)})")
        check_int(ev, "viewUniformsBinding", "engineView")
        if "viewUniformsBinding" in ev and ev.get("viewUniformsBinding") != 3:
            fail(f"engineView.viewUniformsBinding: expected 3, got {ev.get('viewUniformsBinding')}")
        if "viewport" not in ev:
            fail("missing field: engineView.viewport")
        else:
            vp = ev["viewport"]
            if not isinstance(vp, list) or len(vp) != 4:
                fail(f"engineView.viewport: expected array of 4 ints, got {vp!r}")
            elif not all(isinstance(x, int) and not isinstance(x, bool) for x in vp):
                fail(f"engineView.viewport: all elements must be int, got {vp!r}")

    # renderSnapshot
    if "renderSnapshot" not in data:
        fail("missing section: renderSnapshot")
    else:
        rs = data["renderSnapshot"]
        check_bool(rs, "ok", "renderSnapshot")
        for key in ("staticPropValidationFail", "staticPropPacketRangesFail",
                    "staticPropPacketInvalid", "spBuildAttempted", "spBuildFallback",
                    "spBuildCountMismatch", "spBuildPacketMismatch", "spBuildMetaMismatch"):
            check_int(rs, key, "renderSnapshot")
        check_bool(rs, "arenaOverflow", "renderSnapshot")

    # renderPasses
    if "renderPasses" not in data:
        fail("missing section: renderPasses")
    else:
        rp = data["renderPasses"]
        for key in ("shadow", "screenShadow", "bloom", "fxaa", "tonemap"):
            check_bool(rp, key, "renderPasses")

    # staticPropOpaque
    if "staticPropOpaque" not in data:
        fail("missing section: staticPropOpaque")
    else:
        sp = data["staticPropOpaque"]
        for key in ("snapshotDispatchDefault", "legacyDispatch", "materialGpuEnabled",
                    "materialGpuSample", "iblShEnabled", "pbrEnabled",
                    "pbrRoughnessOverrideEnabled"):
            check_bool(sp, key, "staticPropOpaque")
        for key in ("iblShStrength", "pbrStrength", "pbrRoughnessOverride"):
            check_float_or_int(sp, key, "staticPropOpaque")
        check_str(sp, "iblShSet", "staticPropOpaque")
        check_str(sp, "shaderVariant", "staticPropOpaque")
        check_int(sp, "debugMaterialMode", "staticPropOpaque")
        for key in ("spV6DrawCalls", "spAlphaOffPackets",
                    "materialGpuTableSize", "materialInventorySize"):
            check_int(sp, key, "staticPropOpaque")

        # Consistency checks
        if sp.get("snapshotDispatchDefault") is True and sp.get("legacyDispatch") is True:
            fail("staticPropOpaque: snapshotDispatchDefault and legacyDispatch cannot both be true")


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("debug_state/latest_render_state.json")
    if not path.exists():
        print(f"FAIL: file not found: {path}", file=sys.stderr)
        sys.exit(1)

    try:
        text = path.read_text(encoding="utf-8")
    except Exception as e:
        print(f"FAIL: could not read {path}: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        print(f"FAIL: invalid JSON in {path}: {e}", file=sys.stderr)
        sys.exit(1)

    validate(data)

    if errors:
        print(f"FAIL: {path}", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        sys.exit(1)

    frame = data.get("frame", "?")
    schema = data.get("schema", "?")
    ok = data.get("renderSnapshot", {}).get("ok", "?")
    print(f"PASS: {path}  schema={schema}  frame={frame}  renderSnapshot.ok={ok}")
    sys.exit(0)


if __name__ == "__main__":
    main()
