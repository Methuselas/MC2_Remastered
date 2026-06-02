# mc2_asset_viewer (stage 1: texture/asset shell)

Standalone modder tool. Stage 1 = app shell + folder-path browser + texture preview +
`PreviewSurface` seam + asset-type sidebar (Textures live; rest deferred).

**Not a mission editor.** This tool is for assets/modding only (textures,
materials, meshes, VFX, manifests, validation, cooking, packaging). Mission
layout, triggers, objectives, camera paths, and unit placement stay with MC2's
existing mission editor.

## Build

This worktree's `3rdparty/` has no `lib/` — the vendored SDL2/GLEW/ZLIB binaries
live in the root checkout at `A:/Games/mc2-opengl-src/3rdparty/3rdparty` and are
shared across worktrees. Libs are under `lib/x64`, which stock `FindGLEW`/`FindZLIB`
don't search, so pass them explicitly:

```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
D="A:/Games/mc2-opengl-src/3rdparty/3rdparty"
"$CMAKE" -G "Visual Studio 17 2022" -A x64 -S . -B build64 \
  -DCMAKE_PREFIX_PATH="$D" -DCMAKE_LIBRARY_PATH="$D/lib/x64" \
  -DGLEW_INCLUDE_DIR="$D/include" \
  -DGLEW_SHARED_LIBRARY_RELEASE="$D/lib/x64/glew32.lib" \
  -DGLEW_STATIC_LIBRARY_RELEASE="$D/lib/x64/glew32s.lib"
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```

Exe: `build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe`

## Running

The exe needs its runtime DLLs (`SDL2.dll`, `glew32.dll`, etc.) on PATH or
co-located. They are produced in `build64/RelWithDebInfo/`, NOT next to the exe's
`out/` path — copy the exe beside those DLLs (or add that dir to PATH) before
running. When deployed to a runtime dir (e.g. `mc2-opengl/mc2-win64-v0.4`) the
DLLs are already present.

Usage: launch the exe, type a folder path into the **Folder** field, click
**Load**, and select a texture from the list. PNG/JPG decode is Windows-only
(WIC); BMP/TGA via hand-rolled parsers.

## Smoke

```bash
mc2_asset_viewer.exe --smoke tests/fixtures/asset_viewer   # prints [smoke] PASS, exit 0
```
Regenerate the fixture with `python tests/fixtures/asset_viewer/make_fixture.py`.

## Deferred (stages 2-3 + later)

RenderCore link, lit PBR material preview, real model preview, FIT material
parsing, validation flags ("explain what failed"), texture cooking, KTX2 display,
mod packaging. The `PreviewSurface` seam (`draw(const ImVec2&)`) is shaped so a
RenderCore-backed `PreviewSurface` slots in without a rewrite. **SimpleCamera is
assumed dead** (mech-bay render regression, tracked separately) — stage 3 uses
RenderCore.

**Stage-1.5 debt:** extract a shared `tools/common/MC2AppShell` (SDL/GL/ImGui
bring-up) consumed by both `ui_editor` and `mc2_asset_viewer` (greybeard meta-fix;
retires the SDL-attribute / ImGui-backend skew bug class).

Reuses @Methuselas's `ui_editor/UiEditorImageCache.cpp` + `GameOS/gameos/utils/Image.cpp`.

## KTX2 textures (RGBA8 + BC7)

The browser now lists and previews `.ktx2` files alongside PNG/JPG/TGA/BMP.
Supported KTX2 formats: uncompressed RGBA8 (unorm/sRGB) and stored BC7
(unorm/sRGB). The preview follows the file's KTX color-space metadata
(sRGB vs linear).

> BC7 preview requires the `GL_ARB_texture_compression_bptc` OpenGL extension.
> On GPUs/drivers without it, BC7 files show a friendly "unsupported" message
> instead of a preview. RGBA8 KTX2 always previews.

> The asset viewer follows KTX color-space metadata for preview only. This does
> not change the in-game static-prop BC7 runtime policy, which is audited
> separately.

Not yet supported (deferred): DDS, Basis/supercompressed KTX2, CPU BC7
transcoding, asset cooking.

## Resolution tiers + display sizing

When the current folder sits in a set of numeric sibling folders (e.g.
`data/tgl/{128,256,512}` or `data/textures/{64,128,256}`), a **Resolution** row
appears with a button per available tier. Switching tiers keeps the selected
texture (same filename) and reloads it at the new resolution. Only tiers that
exist on disk are shown.

The preview now fits each texture to the view area (1.00× = fit); the zoom slider
multiplies that. Switching resolution tiers — or opening a higher-resolution
texture — no longer changes the on-screen size, only the detail.

## Materials (Stage 2)

Select **Materials** in the sidebar to preview a PBR material on a lit sphere.
Assign up to four slots via **Browse...**: Base Color (sRGB), Normal (linear),
ORM (linear; R=AO, G=Roughness, B=Metallic), Emissive (sRGB). Use the View and
Light controls to orbit, zoom, and rotate the directional light.

> **Preview mode: Local PBR approximation, not exact MC2 shader.** The viewer
> renders with a self-contained Cook-Torrance shader (Backend B). MC2 has no
> standalone ORM material shader to mirror, so this preview is approximate and
> must not be treated as pixel-exact to in-game rendering.
