#!/usr/bin/env python3
"""RENDER-VIEW-CURRENCY-1 invariant check.

The terrain dispatch MVP snapshot (gos_terrain_indirect_getDispatchMvp16) is a
CAMERA-DEPENDENT SNAPSHOT. Using it without proving its view epoch matches the
current view is the BUG1/BUG3/BUG5 root (objects projected through a stale /
mid-frame-superseded camera -> zoom wobble, rotate flicker, props vanish until
pause). To make that bug class structurally impossible:

  * raw gos_terrain_indirect_getDispatchMvp16() is PHASE-PRIVATE to terrain-coupled
    draw passes (terrain / water / shoreline / overlay / decal), which run in the
    same phase the snapshot is published, so currency is structural for them;
  * every OTHER consumer (static props, mechs, vehicles, buildings, generic object
    batchers, UI/editor draw) MUST obtain the MVP via gos_GetObjectDrawMVP()
    (gos_object_draw_mvp.h), which validates the view epoch and falls back to the
    live MVP when it does not match.

This script FAILS the build if a non-allowlisted translation unit references the
raw snapshot getter. Add a file to ALLOWLIST only if it is a genuine terrain-phase
consumer — never to silence a violation in object/mech draw.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Terrain-coupled draw paths + the sanctioned accessor/definition. Basenames.
ALLOWLIST = {
    "gos_terrain_indirect.cpp",    # defines the snapshot + getter
    "gos_terrain_indirect.h",
    "gos_object_draw_mvp.h",       # the currency-checked accessor (only sanctioned reader)
    "gameos_graphics.cpp",         # water / overlay / decal — terrain phase (correct as-is)
    "gos_terrain_lod_chunk.cpp",   # terrain solid draw
    "gos_terrain_water_stream.cpp",# water fast path
    "gos_terrain_water_fast.vert", # (shader, never matches the C symbol; harmless)
    "debug_renderer.cpp",          # diagnostics dump
}

# The raw phase-private symbol. gos_GetObjectDrawMVP / getDispatchMvpViewEpoch /
# getDispatchMvpFrameIdx / getDispatchMvpFp are NOT this — only the matrix getter.
RAW = re.compile(r"\bgos_terrain_indirect_getDispatchMvp16\b")

SCAN_DIRS = ["GameOS/gameos", "mclib", "code", "RenderCore", "RenderWorld", "GameAdapters"]
EXTS = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".inl"}

def main() -> int:
    violations = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for f in base.rglob("*"):
            if f.suffix not in EXTS or f.name in ALLOWLIST:
                continue
            try:
                text = f.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for i, line in enumerate(text.splitlines(), 1):
                # Strip // line-comment and single-line /* */ so prose mentioning
                # the symbol (e.g. "phase-private" NOTEs) is not a violation.
                code = re.sub(r"/\*.*?\*/", "", line)
                code = code.split("//", 1)[0]
                if RAW.search(code):
                    violations.append((f.relative_to(ROOT), i, line.strip()))

    if violations:
        print("RENDER-VIEW-CURRENCY-1 VIOLATION: raw dispatch-MVP snapshot read "
              "outside a terrain-phase translation unit.\n"
              "Object/mech/prop draw must use gos_GetObjectDrawMVP() "
              "(gos_object_draw_mvp.h), which validates the view epoch.\n", file=sys.stderr)
        for path, ln, src in violations:
            print(f"  {path}:{ln}: {src}", file=sys.stderr)
        print(f"\n{len(violations)} violation(s).", file=sys.stderr)
        return 1

    print("check-object-mvp-currency: OK (no raw dispatch-MVP reads outside terrain phase)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
