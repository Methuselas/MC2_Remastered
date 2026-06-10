#!/usr/bin/env bash
# check-editor-build.sh — compile + link gate for the EDITOR targets.
#
# Targets: EditRel ("Mission Editor.exe") and mc2_asset_viewer.
#
# WHY THIS EXISTS
#   The editor links engine libraries (rendercore, gameos_editor, imgui,
#   gui_runtime) but is covered by NO unit test, smoke run, or contract check.
#   The main `mc2` build does NOT build it. So an engine API change — a renamed
#   adapter method, a changed signature, a removed symbol — silently breaks the
#   editor link and is only discovered when a human builds it by hand.
#
#   Concrete instance that motivated this gate: the static-prop GameAdapters
#   firewall refactor (TRACKV) removed the thin
#   GameAdapters::StaticProp::beginMission()/endMission() that editor/EditorData.cpp
#   consumed. Contracts, unit tests, the full mc2 build, AND tier1 smoke all
#   passed green — the editor was the only consumer and nothing built it.
#
#   See: docs/editor-build-architecture-integrity-audit.md (2026-06-02), Lane B.
#
# WHAT IT DOES
#   Builds the two editor targets in an already-configured CMake build dir and
#   fails (exit 1) if either does not compile + link. Run it pre-merge whenever
#   engine headers / GameAdapters / RenderCore / gameos public API changed.
#
# USAGE
#   scripts/check-editor-build.sh [BUILD_DIR]
#     BUILD_DIR   CMake build dir (default: build64). MUST be configured already.
#   Env:
#     CMAKE                     path to cmake (default: PATH, then VS BuildTools)
#     MC2_EDITOR_BUILD_CONFIG   build config (default: RelWithDebInfo)
#
# EXIT CODES
#   0  PASS — both editor targets compile + link
#   1  FAIL — editor build broken (likely engine API drift)
#   2  ENV  — cmake missing or build dir not configured (cannot run the gate)

set -uo pipefail

BUILD_DIR="${1:-build64}"
CONFIG="${MC2_EDITOR_BUILD_CONFIG:-RelWithDebInfo}"
TARGETS=(EditRel mc2_asset_viewer)

# --- resolve cmake: $CMAKE > PATH > VS 2022 BuildTools fallback ---------------
CMAKE="${CMAKE:-}"
if [ -z "$CMAKE" ]; then
  if command -v cmake >/dev/null 2>&1; then
    CMAKE="cmake"
  else
    BT="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
    [ -x "$BT" ] && CMAKE="$BT"
  fi
fi
if [ -z "$CMAKE" ]; then
  echo "check-editor-build: ERROR — cmake not found; set CMAKE=/path/to/cmake.exe" >&2
  exit 2
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  echo "check-editor-build: ERROR — '$BUILD_DIR' is not CMake-configured (no CMakeCache.txt)." >&2
  echo "  Configure it first, e.g.:" >&2
  echo "    \"\$CMAKE\" -S . -B $BUILD_DIR -G \"Visual Studio 17 2022\" -A x64 \\" >&2
  echo "      -DCMAKE_PREFIX_PATH=<repo>/3rdparty/3rdparty -DGLEW_INCLUDE_DIR=... (see docs/asan-mvp-runbook.md)" >&2
  exit 2
fi

# The editor targets require MC2_IMGUI=ON (Map Generator dialog + MissionValidator
# are ImGui-gated and used unconditionally). A build dir configured OFF is the
# silent-invalid-config trap. Fail early with the fix instead of a confusing
# mid-compile error.
if grep -q '^MC2_IMGUI:BOOL=OFF' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
  echo "check-editor-build: ERROR — '$BUILD_DIR' is configured MC2_IMGUI=OFF; the editor needs ON." >&2
  echo "  Reconfigure: \"\$CMAKE\" -S . -B $BUILD_DIR -DMC2_IMGUI=ON" >&2
  exit 2
fi

echo "check-editor-build: building [${TARGETS[*]}] in '$BUILD_DIR' ($CONFIG)"
if "$CMAKE" --build "$BUILD_DIR" --config "$CONFIG" --target "${TARGETS[@]}"; then
  echo "check-editor-build: PASS — editor targets compile + link"
  exit 0
else
  echo "check-editor-build: FAIL — editor build broken (engine API drift? see header)" >&2
  exit 1
fi
