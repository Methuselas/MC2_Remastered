# Lane B — Editor Build Integrity Audit

**Audited:** 2026-06-02  
**Branch:** `terrain-pbr-mod` / worktree `mc2-trackv-ci-gate-restore`  
**Scope:** `editor/CMakeLists.txt` (EditRel), `tools/asset_viewer/CMakeLists.txt` (mc2_asset_viewer), root `CMakeLists.txt` wiring, Viewer/ legacy target.

---

## 1. Target Build-Status Table

| Target | Built by default? | Guard/condition | CI/smoke coverage | Concern |
|---|---|---|---|---|
| `EditRel` | **YES** — unconditional `add_subdirectory("./editor" ...)` at root:541 | Guards itself with `if(NOT MSVC) return()` — skipped silently on non-MSVC | **None** — no test, no smoke, no CI job | Silently un-built on GCC/Clang; no rot guard on MSVC |
| `mc2_asset_viewer` | **YES** — `add_subdirectory` at root:285-287, but inside `if(EXISTS ...)` file-presence check | File-presence only (not a CMake `option()`); active when source tree present | **None** | Existence check is not the same as an option; can silently disappear if dir is absent |
| `viewer` (legacy `Viewer/`) | **NO** — `add_subdirectory` commented out at root:535 | Hard-commented: `# add_subdirectory("./Viewer" "./out/Viewer")` | None | Dead CMake wired to broken source; see §4 |

---

## 2. Findings

### P1 — F-B-01: EditRel and mc2_asset_viewer have zero test/CI coverage

**Evidence:** Grepping all of `tests/` for `EditRel` or `mc2_asset_viewer` returns 0 results. No smoke, no unit test, no CI gate invokes either target. Both are shipped tools (editor = mission authoring; asset_viewer = texture/asset pipeline).

**Risk:** Both targets can silently rot between releases. Any lib API change in `rendercore`, `renderworld`, `gameos_editor`, or `imgui` that breaks the editor link will not be caught until someone manually builds `EditRel`. The asset_viewer is even more exposed: it was still receiving Stage-2 feature commits at time of audit.

**Recommended slice:** S — add a CI build-only step (`cmake --build build64 --target EditRel mc2_asset_viewer`) as a post-merge gate. No runtime test needed; compile+link is sufficient to catch rot.

---

### P1 — F-B-02: editor/CMakeLists.txt:78 — `find_package(SDL2_ttf REQUIRED)` triggers a CMake policy warning

**Evidence:** `sdl2_ttf-config.cmake` (vendored at `3rdparty/3rdparty/cmake/sdl2_ttf-config.cmake:10`) calls `cmake_minimum_required(VERSION 3.0...3.5)` inside a find_package config file. The root project sets `cmake_minimum_required(VERSION 3.10)`. CMake emits a compatibility/deprecation warning when a `find_package` config calls `cmake_minimum_required` with a version range that predates the host minimum.

**Exact package:** SDL2_ttf (vendored SDL2_ttf-devel-2.x.y-VC legacy config file).

**Policy implication:** In CMake ≥ 3.19, calling `cmake_minimum_required` inside a config file also triggers **CMP0048** (project version set by cmake_minimum_required) and can pollute project-level version variables. In CMake ≥ 3.27, `cmake_minimum_required` inside an included/config file is deprecated with a warning. This will **become an error** in a future CMake release. Currently benign but latent.

**Secondary concern:** `SDL2_ttf` is found (line 78) and its DLL is copied POST_BUILD (line 152 as `SDL2_ttf::SDL2_ttf`), but `SDL2_ttf::SDL2_ttf` is **not in `target_link_libraries(EditRel ...)`**. The editor is MFC-based and may not directly call TTF APIs (gameos handles fonts), but the `REQUIRED` keyword on the find_package will hard-fail configure if SDL2_ttf is absent, even though EditRel never links it. This is a fragile over-requirement.

**Recommended slice:** S — update the vendored `sdl2_ttf-config.cmake:10` to `cmake_minimum_required(VERSION 3.10)` to match the project floor, and change `find_package(SDL2_ttf REQUIRED)` to `find_package(SDL2_ttf)` (no REQUIRED) since it is only needed for POST_BUILD DLL deploy.

---

### P1 — F-B-03: EditRel is not built in ASan configuration

**Evidence:** Root CMakeLists.txt:103 defines `option(MC2_ASAN ...)`. The ASan POST_BUILD DLL copy logic at root:487-524 applies **only to `mc2`**. `EditRel` is added unconditionally but the ASan DLL is never copied to the editor output dir. If `MC2_ASAN=ON`, EditRel will fail to launch (missing `clang_rt.asan_dynamic-x86_64.dll`).

**Risk:** P1-level deploy hazard; launch failure on ASan build is not obvious at build time.

**Recommended slice:** XS — add a mirroring `add_custom_command(TARGET EditRel ...)` in editor/CMakeLists.txt that copies the same ASan DLL when `MC2_ASAN` is ON.

---

### P1 — F-B-04: mc2_asset_viewer has no `rendercore`/`gameos` link; incomplete for GPU-backed features

**Evidence:** `tools/asset_viewer/CMakeLists.txt` links only: `imgui`, `SDL2::SDL2`, `SDL2::SDL2main` (conditional), `OpenGL::GL`, `GLEW::GLEW`, `ole32`, `windowscodecs`, `uuid`. It does NOT link `rendercore`, `gameos`, `mclib`, or `GameOS/gameos/utils/Image.cpp`'s transitive deps.

However, it directly compiles `${CMAKE_SOURCE_DIR}/GameOS/gameos/utils/Image.cpp` (line 18) and `${CMAKE_SOURCE_DIR}/ui_editor/UiEditorImageCache.cpp` (line 17) as raw sources. If these TUs transitively include gameos headers that pull in gameos-linked symbols at link time, the link will fail or silently pick up unresolved symbols. Currently buildable only because the used subset is header-only or self-contained — fragile as features are added.

**Recommended slice:** M (Stage 2 context) — track in asset_viewer Stage-2 debt; add `gameos_editor` or a new minimal `gameos_headless` target if GPU init is needed.

---

### P2 — F-B-05: Viewer/ legacy target is dead CMake with broken source references

**Evidence:** `Viewer/CMakeLists.txt` lists source files `ViewerHostGlobals.cpp`, `ViewerImgui.cpp`, `ViewerGuiFit.cpp`, `ViewerFontCatalog.cpp`, `ViewerVersion.cpp`, `ViewerTextureLoader.cpp` that **do not exist** in `Viewer/` (only `View.cpp` is present). The comment at root:530-535 documents why it is disabled ("one missing source fails CMake generate step"). The `viewer` target also links `gui_runtime`-less but that is moot since it is disabled.

**Risk:** The comment is adequate documentation for the disable decision. Risk is that the CMakeLists.txt will diverge further from any future re-enable attempt.

**Recommended slice:** defer — either delete `Viewer/CMakeLists.txt` and log it as DEBT, or track as `VIEWER-RENABLE-1` in the backlog.

---

### P2 — F-B-06: Redundant `find_package(SDL2 ...)` in editor/CMakeLists.txt

**Evidence:** Root CMakeLists.txt already runs `find_package(SDL2 REQUIRED CONFIG REQUIRED COMPONENTS SDL2)` at line 150. `editor/CMakeLists.txt:76` repeats the identical call. Because `find_package` is idempotent on cached results this is harmless, but inconsistent with the comment at editor line 74-75 ("SDL2 is found by the root CMakeLists.txt; inherit its variables").

**Recommended slice:** XS — remove the redundant call at editor:76; the root already guarantees SDL2::SDL2 target visibility.

---

### P2 — F-B-07: mc2_asset_viewer SDL include-dir resolution is over-engineered

**Evidence:** `tools/asset_viewer/CMakeLists.txt:38-90` implements a 50-line manual SDL include-dir probe (checking `SDL2_INCLUDE_DIRS`, `SDL2_INCLUDE_DIR`, iterating `CMAKE_PREFIX_PATH`, iterating vcpkg triplets, hardcoding triplet names `x64-windows`/`x86-windows`/`x64-linux`). This is entirely unnecessary when `SDL2::SDL2` INTERFACE_INCLUDE_DIRECTORIES already propagates the correct include path via `target_link_libraries`. The probe is cargo-culted from a pre-modern-cmake era.

**Risk:** The hardcoded triplet names (`x64-windows`, `x86-windows`, `x64-linux`) are fragile across different vcpkg configurations. In practice redundant but waste cognitive overhead.

**Recommended slice:** S — replace with `target_link_libraries(mc2_asset_viewer PRIVATE SDL2::SDL2)` only (already present at line 103); drop the `MC2R_AV_SDL_INCLUDE_DIRS` block and `target_include_directories` SDL entry. The imgui SDL backend include will resolve via the SDL2::SDL2 target's INTERFACE_INCLUDE_DIRECTORIES.

---

### P3 — F-B-08: No `cmake_minimum_required` or `cmake_policy` in editor/CMakeLists.txt

**Evidence:** `editor/CMakeLists.txt` has no `cmake_minimum_required` or `cmake_policy`. It relies entirely on the root CMake state. Standard CMake practice for subdirectory CMakeLists files is to omit these (they inherit the root), so this is not a defect — noted for completeness.

---

### P3 — F-B-09: C++ standard not explicitly set for EditRel; inherits root `CMAKE_CXX_STANDARD 17`

**Evidence:** `editor/CMakeLists.txt` has no `target_compile_features` or `set(CMAKE_CXX_STANDARD ...)` call. It inherits `CMAKE_CXX_STANDARD 17` from root (set at root:11). `mc2_asset_viewer` explicitly sets `target_compile_features(mc2_asset_viewer PRIVATE cxx_std_17)` at tools:23 which is correct.

**Risk:** If the root standard ever changes, EditRel silently re-compiles at the new standard. Low risk given the project's C++17 commitment is documented.

---

### P3 — F-B-10: No warnings-as-errors on EditRel or mc2_asset_viewer; no `/WX`

**Evidence:** Neither editor CMakeLists sets `/WX` (MSVC) or `-Werror`. The root does not set a global `/WX` either. The non-MSVC path at root:57 sets `-Werror=array-bounds` only. Editor targets may accumulate silent warnings that mask real issues.

**Recommended slice:** defer — editor is MFC legacy code, `/WX` would require significant warning triage first.

---

## 3. The :78 Warning — Verdict

**BENIGN but LATENT.** The warning is emitted by `sdl2_ttf-config.cmake:10` calling `cmake_minimum_required(VERSION 3.0...3.5)` inside a find_package config file, violating the principle that config files should not raise the cmake_minimum_required. The warning is currently non-fatal but will become an error in a future CMake version. The `REQUIRED` keyword also makes configure fail if SDL2_ttf is missing, even though EditRel does not link it. Fix is XS: update config file floor, drop REQUIRED.

---

## 4. Recommended Guardrails (priority order)

1. **Add CI build-only step** for `EditRel` and `mc2_asset_viewer` — catches link rot early (S, highest ROI).
2. **Fix SDL2_ttf config** — drop REQUIRED, update cmake_minimum_required in vendored config (XS).
3. **Mirror ASan DLL copy to EditRel** when `MC2_ASAN=ON` (XS).
4. **Document asset_viewer link-gap** in Stage-2 spec as a tracked debt item (XS).
5. **Delete or archive Viewer/CMakeLists.txt** to avoid false-positive re-enable attempts (defer/XS).

---

## Summary

| Severity | Count | Items |
|---|---|---|
| P0 | 0 | — |
| P1 | 4 | F-B-01 (no CI coverage), F-B-02 (:78 warning + over-required SDL2_ttf), F-B-03 (ASan DLL missing for EditRel), F-B-04 (asset_viewer link gap) |
| P2 | 3 | F-B-05 (Viewer dead CMake), F-B-06 (redundant find_package), F-B-07 (SDL include over-engineering) |
| P3 | 3 | F-B-08, F-B-09, F-B-10 |
