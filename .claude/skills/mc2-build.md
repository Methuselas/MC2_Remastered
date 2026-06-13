---
name: mc2-build
description: Build mc2.exe in the current worktree (or main repo) using CMake + MSVC RelWithDebInfo
---

# MC2 Build

Build mc2.exe from the current worktree or main repository.

## Steps

1. **Detect worktree**: Check if CWD is inside a worktree under `.claude/worktrees/`. If so, use that worktree root. Otherwise use `A:/Games/mc2-opengl-src`.

1.5. **Bootstrap if unconfigured (FRESH worktrees) — DO NOT hand-discover CMake flags**: If `<worktree_root>/build64/CMakeCache.txt` does NOT exist, run the bootstrap instead of guessing flags one find_package error at a time (that wastes a whole session every time):
   ```bash
   py -3 <worktree_root>/scripts/bootstrap_worktree_build.py --build
   ```
   It copies the git-untracked `3rdparty/{cmake,lib,include,ffmpeg}` deps from a sibling worktree that has them, configures with the full known-good `-D` set (`CMAKE_PREFIX_PATH` + GLEW×3 + ZLIB×2 + `MC2_IMGUI=ON`), then builds `mc2`. Then STOP. (Drop `--build` to configure only.) **Root cause it guards:** the prebuilt 3rdparty binary deps are untracked, so a fresh `git worktree add` has no deps and a bare `cmake -S . -B build64` fails SDL2→GLEW→ZLIB→MC2_IMGUI in sequence.

2. **Verify build tree** (already-configured worktrees): If `<worktree_root>/build64/CMakeCache.txt` exists, grep it for `mc2_SOURCE_DIR:STATIC=`. If the value does NOT end in `/nifty-mendeleev`, print:
   ```
   ⚠ BUILD-DIR WARNING: CMakeCache.txt shows mc2_SOURCE_DIR=<actual value>
     Expected path ending in /nifty-mendeleev. You may be building from the
     wrong source tree (root checkout is stale terrain-pbr-mod). Proceeding anyway.
   ```
   If the file does not exist (unconfigured tree), skip silently.

3. **Run CMake build**:
```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
cd "<worktree_root>" && "$CMAKE" --build build64 --config RelWithDebInfo --target mc2
```

3. **Report result**: Show success or failure. On failure, show the last 30 lines of output to capture error messages.

## Important
- ALWAYS use `--config RelWithDebInfo`, never Release or Debug
- ALWAYS use `--target mc2`
- Build directory is always `build64/` relative to worktree root
- Output exe lands at `build64/RelWithDebInfo/mc2.exe`
