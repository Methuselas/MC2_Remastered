# MC2 Asset Viewer — Stage 1 Design (Texture/Asset Shell)

**Date:** 2026-06-01
**Slice:** `MC2-ASSET-VIEWER-STAGE1-0`
**Base:** the @Methuselas merge (`claude/merge-methuselas-1`) — stage 1 reuses
`ui_editor/UiEditorImageCache.cpp`, which only exists post-merge.

## Goal

Stand up the **asset-viewer application shell** for a modder-facing MC2 asset
tool. This is explicitly **not** a complete viewer. The stage-1 deliverable is:

- the standalone app skeleton (window + GL + ImGui + frame loop),
- a file/folder browser,
- a working **texture** preview,
- the `PreviewSurface` seam that lets stages 2–3 add richer rendering additively,
- an asset-type sidebar vocabulary (textures live; the rest visible-but-disabled).

It must feel like the front of an asset tool, not "just a PNG viewer" — achieved
by the sidebar vocabulary, with zero extra logic.

## Design stance: "A's footprint, B's architecture"

Approach **A** (thin) for dependencies and scope: link only imgui + SDL2 +
OpenGL + GLEW (mirrors `ui_editor`). **Do not** link RenderCore yet — YAGNI until
stage 2/3 need it. But every seam is **shaped to become B** (RenderCore-linked)
additively:

- the app talks to a `PreviewSurface` interface, never to GL/render specifics;
- init and frame loop are render-backend-agnostic;
- adding the RenderCore link later is a one-line CMake change plus a new
  `PreviewSurface` implementation — not a rewire.

## Architecture

New executable target **`mc2_asset_viewer`**.

- **Links (stage 1):** `imgui`, `SDL2`/`SDL2main`, `OpenGL::GL`, `GLEW::GLEW`.
  (Reserved for later, not added now: `rendercore`, `renderworld`, `gameadapters`, `mclib`.)
- **Reused source (compiled in, not copied):**
  - `ui_editor/UiEditorImageCache.cpp` — public API `UiEditorImageCache_Get(path)` → `UiEditorImageTexture{loaded,unavailable,width,height,textureId}`; decode + GL upload.
  - `GameOS/gameos/utils/Image.cpp` — decode backend: PNG/JPG via WIC (`<wincodec.h>`, Windows-only), TGA/BMP via hand-rolled parsers. (Not stb_image. Stage 1 is Windows-only.)
- **New sources** under `tools/asset_viewer/`:
  - `main.cpp` — SDL window + GL context + ImGui init + frame loop (trimmed mirror
    of `UiEditorMain.cpp`'s bring-up).
  - `AssetViewerApp.{h,cpp}` — app lifecycle, current selection, panel wiring.
  - `PreviewSurface.h` — the seam (interface).
  - `TexturePreview2D.{h,cpp}` — stage-1 `PreviewSurface` impl (ImGui image).
  - `FileBrowser.{h,cpp}` — open file/folder, scan + filter texture extensions.
  - `TextureInspectorPanel.{h,cpp}` — draws active surface + metadata.
  - `AssetTypeSidebar.{h,cpp}` — the asset-type vocabulary list (see below).

## Components (each one job, understandable in isolation)

| Unit | Does | Uses | Depends on |
|---|---|---|---|
| `ImageCache` *(reused)* | `path → UiEditorImageTexture{loaded,unavailable,width,height,textureId}` | `UiEditorImageCache_Get` | Image.cpp (WIC/TGA/BMP), GL |
| `FileBrowser` | open file/folder; enumerate + filter texture files; emit selected path | ImGui file dialog / dir scan | std::filesystem, ImGui |
| `PreviewSurface` (interface) | `setSource(path)` / `draw(rect)` | — | nothing (pure interface) |
| `TexturePreview2D` | hold a cached GL texture; `draw` = `ImGui::Image` with zoom/pan | ImageCache | ImageCache, ImGui |
| `TextureInspectorPanel` | render active surface + metadata (path, dims, channels, GL fmt, file size) | PreviewSurface | PreviewSurface, ImGui |
| `AssetTypeSidebar` | show asset-type vocabulary; only "Textures" selectable | — | ImGui |
| `AssetViewerApp` | own window/GL/ImGui + loop; hold selection; wire browser→cache→surface | all above | SDL, GL, ImGui |

**Seam check:** `AssetViewerApp` and `TextureInspectorPanel` depend on the
`PreviewSurface` interface only. Stage 2 adds `MaterialPreviewPBR`, stage 3 adds
`ModelPreviewRenderCore` — neither requires touching the app or panel.

## Asset-type sidebar vocabulary

A left sidebar lists the modder-facing asset kinds. Only **Textures** is
implemented/selectable in stage 1; the rest render greyed-out/disabled with a
"deferred" tooltip. No logic behind the disabled entries — purely directional.

```
Implemented now:
- Textures

Visible but disabled/deferred:
- Materials
- Static Props
- Trees
- Mechs
- Vehicles
- VFX
- Terrain Materials
- Mod Package
```

## Data flow

```
folder/file path
  → FileBrowser enumerates texture files (.png/.jpg/.bmp/.tga)
  → user selects one
  → UiEditorImageCache_Get(path) → UiEditorImageTexture{loaded, width, height, textureId}
  → TexturePreview2D holds it
  → TextureInspectorPanel: ImGui::Image(glTexture) + metadata readout
```

No 3D, no FIT parsing, no validation, no cooking in stage 1.

## Error handling

Load failure surfaces as readable panel text — "file not found",
"decode failed", "unsupported format" — never a crash. `ImageCache` already
returns a bool; the panel renders the failure state instead of an image.

## Testing

- **Smoke (offscreen GL):** `UiEditorImageCache_Get` decodes AND
  uploads (`glTexImage2D`), so it needs a live GL context. The smoke creates a
  hidden SDL window + GL context, loads a known fixture texture, asserts
  dimensions + channel count, then tears down. Keep one small fixture under
  `tests/fixtures/`. (A pure-decode unit test would require splitting decode from
  upload in his file — deferred; not worth touching `UiEditorImageCache` yet.)
- **Manual:** run `mc2_asset_viewer`, open
  `data/art/gui/test/flat-ass plane_BaseColor.png` (present in the v0.5.3 data),
  confirm it displays with correct dims/metadata; open a missing/garbage file and
  confirm the error state renders.

## Build / deploy

- `add_executable(mc2_asset_viewer ...)` + the guarded
  `if(EXISTS .../tools/asset_viewer/CMakeLists.txt)` subdirectory pattern, matching
  how `ui_editor` is wired.
- Same fresh-configure caveats as the rest of this tree: vendored deps, pass
  `-DCMAKE_LIBRARY_PATH=<deps>/lib/x64`.
- Deploy the built exe alongside `mc2.exe` / `ui_editor.exe` into the v0.4 runtime
  when ready to run.

## Explicitly deferred

- RenderCore link + the lit PBR material preview (**stage 2**).
- Real MC2 model preview via RenderCore (**stage 3**).
- FIT material parsing, validation flags ("explain what failed"), texture cooking,
  KTX2 display, mod packaging.
- **SimpleCamera is assumed dead** (mechs no longer render in the logistics/mech
  bay — a regression in nifty; tracked separately). Stage 3 will use RenderCore,
  not SimpleCamera.

## Risks

- Reusing `UiEditorImageCache.cpp` couples the viewer to @Methuselas's file; if his
  upstream changes it, the viewer recompiles against the new version (acceptable —
  shared source by intent). Graduate to a small shared static lib only if it earns it.
- Stage 1 builds only on the merge branch; promoting this work depends on the
  merge becoming a permanent base (open decision).

## Credit

Reuses @Methuselas's `UiEditorImageCache.cpp` + `Image.cpp`. Any commit building on
them credits **@Methuselas** (github.com/Methuselas) via `Co-Authored-By`.
