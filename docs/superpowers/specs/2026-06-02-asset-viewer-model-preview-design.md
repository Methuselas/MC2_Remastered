# MC2 Asset Viewer — Model Preview (Static Props) Design

**Date:** 2026-06-02
**Slice:** `MC2-ASSET-VIEWER-MODELPREVIEW-0`
**Base:** `claude/asset-viewer-model-preview` off `claude/nifty-mendeleev` (has the NS3 game-free
TGL/ASE loader gate + the rendercore-standalone spike + host-services).
**Status:** SPEC — design approved 2026-06-02 (geometry-source decision = tgl.fst). Next: writing-plans → adversarial → execute.

## Goal

Light up the **Static Props** sidebar entry: load real MC2 prop geometry from `tgl.fst` and
render it on an orbit-camera turntable with its real albedo texture. Renders **viewer-local
(Backend B, labeled approximate)** — the NS3 gate cleared geometry *loading*; pixel-faithful
RenderCore drawing (Backend A) is a later upgrade. Mechs/Vehicles reuse the same loader after.

## Locked decisions

| Question | Decision |
|---|---|
| Geometry source | **`tgl.fst`** (deployed FastFile archive) — self-contained to the deploy, matches the game. Not the loose `.ase` source tree (absent in deploy). |
| Loader | Reuse the NS3 game-free recipe (`tools/tgl_loader_standalone_spike`): `TG_TypeMultiShape::LoadBinaryCopy` + the spike's `stubs.cpp`. Link the same TU set into `mc2_asset_viewer`. |
| Render | Backend B (viewer-local lit shader), labeled approximate. Real per-submesh **albedo** bound via the existing `Ktx2Decoder` (resolve tex name → `data/tgl/<tier>/<name>.ktx2`). |
| Browse | Enumerate `tgl.fst` via `FastFile::getFilesInfo()` (full filenames present) → list `*.tgl`. |
| Scope | Static Props MVP. Mechs (animation frames) + Vehicles + Backend A draw seam deferred. |

## Risk-first plan: Task 0 = link + load proof

The dominant unknown is **the link integration**: merging ~30 mclib + ~50 Stuff TUs + `stubs.cpp`
+ zlib into `mc2_asset_viewer`, which already links imgui/SDL/GLEW + `UiEditorImageCache.cpp` +
`GameOS/gameos/utils/Image.cpp` + `RenderCore/KtxLoader.cpp`. Risks: duplicate symbols / ODR
between the engine TUs and the viewer's existing TUs, and stub-vs-real `gos_*` collisions.

**Task 0 (gates the slice):** add the loader TU set + `engine_stubs.cpp` to the viewer's CMake,
add a headless `--smoke-tgl-load` that `FastFileInit("…/tgl.fst")` → enumerates → `LoadBinaryCopy`
one known prop → asserts shapes>0 and verts/tris read out. If the link is clean and a prop loads,
the slice is unblocked. If there are symbol clashes, resolve (or isolate the loader behind its own
static lib) BEFORE building the renderer.

## Architecture (new files under `tools/asset_viewer/`)

| File | Responsibility |
|---|---|
| `engine_stubs.cpp` | Copy of `tools/tgl_loader_standalone_spike/stubs.cpp` — game-free host stubs (heap, CRT file I/O, `gos_*` GPU no-ops, `Environment`, error no-ops) the loader closure needs. |
| `TglMeshLoader.{h,cpp}` | Owns FastFile registration (`FastFileInit("tgl.fst")` once), archive enumeration (`*.tgl` names), and `loadMesh(tglName)` → `TG_TypeMultiShape::LoadBinaryCopy` → walk shapes → expand per-corner `TG_UVData` UVs into a unified vertex buffer (pos/normal/uv) + index buffer **per submesh**, with each submesh's texture name (`GetTextureName(localTextureHandle)`). Returns a CPU `MeshData { submeshes[], bounds }`. No GL. |
| `MeshGpu.{h,cpp}` | Uploads a `MeshData` to GL (VAO/VBO/EBO per submesh) + per-submesh albedo texture (resolve name → `data/tgl/<tier>/<stem>.ktx2` via `Ktx2Decoder`; flat-white fallback). Owns/frees GL resources. |
| `MeshPreview3D.{h,cpp}` | The `PreviewSurface`. `setSource(tglName)` → `TglMeshLoader` → `MeshGpu`; `draw()` renders all submeshes (lit shader, per-submesh albedo) into an FBO (GL-state contained, like `MaterialPreviewPBR`), orbit camera + light auto-framed to bounds, blits via `ImGui::Image`; persistent "approximate" label. |
| `ModelBrowser.{h,cpp}` | Lists the `*.tgl` entries from `TglMeshLoader`'s enumeration (filter box); selecting one calls `MeshPreview3D.setSource`. (Distinct from the texture `FileBrowser`.) |

Modified: `AssetTypeSidebar` (Static Props → live), `AssetViewerApp` (own `MeshPreview3D` + `ModelBrowser`, dispatch when active==StaticProps), `CMakeLists.txt` (loader TU set + stubs + zlib + RenderCore/GameOS includes), `README.md`.

## Loader integration detail (from the NS3 handoff)

- **Link set:** mirror `tools/tgl_loader_standalone_spike/CMakeLists.txt` — mclib (`msl, tgl, cident,
  mathfunc, file, fastfile, ffile, packet, lzdecomp, fst_hash, heap, paths, timing,
  cpu_proj_cost_split, projectz_trace, object_admission_predicate`), Stuff (the ~50 math/container
  TUs), `GameOS/src/platform_str.cpp`, vendored `zlib.lib`. Defines `PLATFORM_WINDOWS LINUX_BUILD
  _CRT_SECURE_NO_WARNINGS`; **do NOT** define `ENABLE_ASSIMP_IMPORTER`. Includes: `mclib`, `mclib/stuff`,
  `GameOS/include`, `GameOS/gameos`, `3rdparty/include` (zlib.h), `3rdparty/tracy`.
- **FastFile register:** set `maxFastFiles`, `fastFiles = malloc(...)`, `FastFileInit("<deploy>/tgl.fst")`
  once. Globals the loader needs: `systemHeap` non-null (`UserHeap`), `Environment` (checkCDForFiles=false),
  empty `CDInstallPath`. CRT-only; no GameOS.
- **Read-out API:** `ms.GetNumShapes()` → `GetTypeNode(i)->node` (`TG_TypeShape*`) →
  `GetNumTypeVertices()/GetTypeVertices()` + `GetNumTypeTriangles()/GetTypeTriangles()`;
  `T[k].Vertices[3]` indices, `T[k].uvdata` per-corner UVs, `T[k].localTextureHandle` →
  `ms.GetTextureName(handle, buf, n)`. Vertex: `position` (Stuff::Point3D), `normal`, `aRGBLight`.
- **Never call** `TG_Shape::CreateFrom`/`MultiTransform*`/`SetTextureHandle` (the render/transform path
  that pulls the stubbed `code/` symbols) — load + read-out only.

## Coordinate / UV notes

- Positions are Stuff space; apply the Stuff→GL swap (`x'=-x, y'=z, z'=y`) consistent with how the
  engine draws props (see `static_prop.vert`), so the model isn't mirrored/rotated wrong. Frame the
  orbit camera to the mesh bounds.
- UVs are **per-triangle-corner** — expand to per-vertex by duplicating vertices per face corner
  (don't index-share across differing UVs), exactly as the batcher does.

## Rendering (Backend B)

Reuse the `MaterialPreviewPBR` GL-containment + FBO + orbit/light patterns. A lit shader
(Lambert + the existing Cook-Torrance is overkill; a simple N·L diffuse × albedo + small ambient is
fine for a model preview) samples each submesh's albedo. Multi-submesh: draw each with its own
albedo bound. Same "Local preview, not exact MC2 shader" label.

## Testing (existing `--smoke*` harness)

- **`--smoke-tgl-load <deployDir>`** (no GL, the Task-0 gate): `FastFileInit(deployDir/tgl.fst)`,
  enumerate (assert count>0 and at least one `*.tgl`), `LoadBinaryCopy` a known prop, assert
  `GetNumShapes()>0` and a shape yields verts>0/tris>0 + a non-empty texture name. Proves the link +
  load inside the viewer's link unit.
- **`--smoke-mesh-build <deployDir>`** (no GL): `TglMeshLoader::loadMesh` → assert ≥1 submesh, each
  with VB/IB sizes consistent (indices in range, UV count == vert count after expansion).
- **`--smoke-mesh-render <deployDir>`** (GL 3.3): load + upload + render to FBO, assert the center
  region is non-empty / brighter than background + `glGetError()` clean (no brittle goldens).
- All existing viewer smokes (texture/material) must stay green — esp. confirm the loader-TU merge
  didn't break the existing link.

## Risks

1. **Link/ODR (primary)** — engine TUs vs the viewer's existing TUs / stubs vs any real `gos_*`.
   Mitigation: Task 0 first; if needed, wrap the loader TUs in a static lib with the stubs to isolate.
2. **Stuff→GL transform** — wrong swap → mirrored/upside-down model. Mitigation: match `static_prop.vert`.
3. **Texture name → file resolution** — names are basenames (e.g. `BushwackerRGB.tga`); map to
   `data/tgl/<tier>/<stem>.ktx2`. Missing → flat-white fallback (don't fail the whole mesh).
4. **FastFile global state** — `FastFileInit` is process-global; init once, guard re-entry.

## Deferred

- **Backend A (pixel-faithful)** — promote the rendercore spike to a real `static_prop` draw
  (batcher buffer contract). Separate task.
- **Mechs** (animation-frame `.ase`/`.tgl` sets; pick base frame) + **Vehicles**.
- LOD selection, wireframe/normals overlay, turntable capture, multi-material ORM.
