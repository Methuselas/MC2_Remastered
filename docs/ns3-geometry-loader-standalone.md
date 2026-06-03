# NS3 — Game-Free TGL/ASE Geometry Loader (handoff to model-preview / Backend A)

**Status:** DONE (the gate). The MC2 `.tgl`/`.ase` geometry loader **links and runs with zero
`code/` (game) dependency.** Proven by `tools/tgl_loader_standalone_spike` — links only mclib +
Stuff + a thin stub TU, loads real `.tgl` files, and reads out vertices / indices / UVs /
per-triangle texture names. On nifty.

This is the one thing the model-preview session was gated on. Rendering is not a dependency —
only geometry loading was, and it's clear.

---

## How to use it in `mc2_asset_viewer`

### Load
```cpp
TG_TypeMultiShape ms;
ms.LoadBinaryCopy("data/tgl/bushwacker.tgl");   // mclib/msl.cpp:194 — direct .tgl reader
// or ms.LoadTGMultiShapeFromASE("foo.ase");     // mclib/msl.cpp:563 — ASE + .tgl sidecar
```

### Read geometry out (public API)
```cpp
long nShapes = ms.GetNumShapes();                       // msl.h
TG_TypeNodePtr node = ms.GetTypeNode(i);                // -> node->node (TG_TypeShape*)
TG_TypeShape* shape = node->node;

long nV = shape->GetNumTypeVertices();                  // already public
const TG_TypeVertex*   V = shape->GetTypeVertices();    // NEW getter (this change is on nifty)
long nT = shape->GetNumTypeTriangles();                 // NEW getter
const TG_TypeTriangle* T = shape->GetTypeTriangles();   // NEW getter

// vertex: V[k].position (Stuff::Point3D), V[k].normal (Stuff::Vector3D), V[k].aRGBLight
// triangle: T[k].Vertices[3] (indices), T[k].localTextureHandle, T[k].uvdata (u0,v0,u1,v1,u2,v2 — per-corner)
char texName[256];
ms.GetTextureName(T[k].localTextureHandle, texName, sizeof(texName));  // per-tri texture basename
```
UVs are **per-triangle-corner** (`TG_UVData`), not per-vertex — expand to a vertex buffer the
same way the batcher does.

### The one source change (already on nifty)
`mclib/tgl.h` `TG_TypeShape` gained 3 read-only getters (the geometry arrays were `protected`):
`GetTypeVertices()`, `GetTypeTriangles()`, `GetNumTypeTriangles()`. Nothing else changed.

---

## The verified game-free link set

Copy this into the `mc2_asset_viewer` link (it's the `tools/tgl_loader_standalone_spike` recipe;
mirror its `CMakeLists.txt`). **Zero `code/` TUs. No `gameos.cpp`. No GL/SDL needed for loading.**

**mclib:** `msl.cpp tgl.cpp cident.cpp mathfunc.cpp file.cpp fastfile.cpp ffile.cpp packet.cpp
lzdecomp.cpp fst_hash.cpp heap.cpp paths.cpp timing.cpp` (+ a few helper TUs that share the
loader's TUs: `cpu_proj_cost_split.cpp projectz_trace.cpp object_admission_predicate.cpp`).
**Stuff:** the math/container library TUs (point3d, vector3d, matrix, … — each type is its own TU;
see the spike CMake for the full list). **Platform:** `GameOS/src/platform_str.cpp` (`S_stricmp`).
**Lib:** vendored `zlib.lib` (for `.tgl` LZ/packet decode). **Defines:** `LINUX_BUILD`,
`PLATFORM_WINDOWS`, `_CRT_SECURE_NO_WARNINGS`. Do **NOT** define `ENABLE_ASSIMP_IMPORTER`.

### Caller-provided services (the stubs — these map to host-services)
The loader needs a small host layer (`tools/tgl_loader_standalone_spike/stubs.cpp` is the
reference impl — copy it, or back these with real services):
- **Heap:** install `TG_Shape::tglHeap` and `systemHeap` as `UserHeap`s; `gos_CreateMemoryHeap`
  must return a **non-null** dummy handle (a null handle routes to a dead asm path → crash),
  `gos_Malloc/gos_Free` → CRT.
- **File I/O:** `gos_OpenFile/ReadFile/CloseFile/FileSize/DoesFileExist` → CRT `fopen/fread`.
  (`.tgl` opens via `file.cpp`'s own CRT path; the gos file API is only the ASE/anim path.)
  Works on **raw disk** — no FastFile registry needed.
- **GPU no-ops:** `gos_CreateBuffer`/`gos_CreateVertexDeclaration` → dummy handles. The CPU
  geometry arrays are fully populated **before** these GPU-upload calls, so a read-out tool never
  needs a GL context.
- **Error/env:** `Environment` global, `InternalFunctionStop`/`gosASSERT` no-ops.

### `code/` symbols on the link closure — all stubbed, none on the load path
`eye`, `land`, `mcTextureManager`, `Camera::HazeFactor`/`projectModernClipGL`,
`Terrain::getTerrainElevation`, `MC_TextureManager::{addLightDataStructure,addRenderShape,…}`,
`MC_TextureNode::get_gosTextureHandle`, `eligibleForGpuObjects`. Every one is reachable **only**
from `TG_Shape` per-instance **render/transform** methods (and texture-bind), which the load path
**never calls** — they link in only because they share a `.cpp` with `LoadBinaryCopy`. Stub them
to null/no-op (the spike does). **This is the proof the loader is game-free.** If you never call
`TG_Shape::CreateFrom` / `MultiTransform*` / `SetTextureHandle`, none of them run.

---

## Verified output (spike)
```
[spike] LOAD OK: shapes=47 textures=2   (bushwacker.tgl)
[spike] texture names: BushwackerRGB.tga, BushwackerRGBX.tga
[spike] shape[33]: vertices=29 triangles=30
    v[0] pos=(2.09,-2.20,-1.79) nrm=(0.42,0.57,-0.71) ...
    tri[0] idx=(23,24,25) texHandle=0 tex="BushwackerRGB.tga" uv0=(0.74,0.75) ...
[spike] SUCCESS — geometry loaded game-free.   EXIT=0
```
Also verified on `armoredcardam.tgl` (1 shape / 2 tex / 24 v / 38 tri).

---

## Definition-of-done check (vs the request)
- ✅ **(gate)** geometry-loader TU list + headers, link-clean to **zero `code/`**, on nifty —
  proven by `tools/tgl_loader_standalone_spike`.
- ✅ **host-services contract** (`engine-host-services-0`) merged to nifty (earlier) — the stub
  list above is exactly what the host provides (file/heap/log), aligning with `mc2host::HostServices`.
- ⏳ **(nice / not blocking)** static-prop **draw** seam for pixel-faithful Backend A: NOT done.
  The rendercore spike (`tools/rendercore_standalone_spike`) already proves init→view→
  `applyPipeline(StaticPropOpaque)`→FBO readback game-free; promoting it to a real single-mesh
  draw through the `static_prop` program means feeding the batcher buffer contract
  (Instances/PerType/Colors/LightsData/MaterialTable SSBOs + ViewUniforms UBO + texture array —
  see `gos_static_prop_batcher.cpp` finalizeGeometry/draw). Your MVP renders viewer-local
  (Backend B, labeled approximate) without it — start there; the draw seam is a later upgrade.

**Net:** you can start the model-preview MVP now — load real prop/mech geometry via the recipe
above, render with the viewer's own lit shader (Backend B). Pixel-faithful Backend A is a later
draw-seam task, not a blocker.
