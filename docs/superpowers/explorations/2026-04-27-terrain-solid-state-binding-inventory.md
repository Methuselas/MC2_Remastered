# Terrain Solid Render-State / Material Binding Inventory

**Date:** 2026-04-27  
**Branch:** terrain-pbr-mod  
**Purpose:** Determine whether Render.TerrainSolid can collapse to one draw per frame (PatchStream architecture viability).

---

## 1. gos_SetRenderState / gos_SetTextureState Calls

All calls are in `mclib/txmmgr.cpp` inside the `Render.TerrainSolid` zone (lines 1296–1358).

| Call Site | State Enum | Value | Frequency | Changes Between Batches? |
|-----------|-----------|-------|-----------|--------------------------|
| txmmgr.cpp:1306 | `gos_State_TextureAddress` | `gos_TextureClamp` | Per-node (MC2_ISTERRAIN branch) | Yes — flips to Wrap for non-terrain nodes |
| txmmgr.cpp:1307 | `gos_State_Terrain` | `1` | Per-node (MC2_ISTERRAIN branch) | Yes — auto-resets to 0 after each draw (gameos_graphics.cpp:2105-2106) |
| txmmgr.cpp:1309 | `gos_State_TextureAddress` | `gos_TextureWrap` | Per-node (!MC2_ISTERRAIN branch) | Yes |
| txmmgr.cpp:1310 | `gos_State_Terrain` | `0` | Per-node (!MC2_ISTERRAIN branch) | Yes |
| txmmgr.cpp:1331 | `gos_State_Texture` | `tex_resolve(textureIndex)` | Per-node, before the single-chunk draw | **Yes — changes every node** |
| txmmgr.cpp:1336 | `gos_State_Texture` | `tex_resolve(textureIndex)` | Per-node, before the multi-chunk loop | **Yes — changes every node** |
| txmmgr.cpp:1324 | `gos_SetTerrainBatchExtras` | `(extras, extraCount)` | Per-node (MC2_ISTERRAIN with extras) | Yes — per-node extras pointer |
| txmmgr.cpp:1326 | `gos_SetTerrainBatchExtras` | `(NULL, 0)` | Per-node (no extras) | Yes |

**State application model:**  
`gos_SetRenderState` writes into `renderStates_[]` (gameos_graphics.cpp:~1252). `applyRenderStates()` is called inside `drawIndexedTris` at line 2984 — *before* the tessellation condition check. This means every node's texture bind is applied before its draw. `gos_State_Terrain` auto-resets after being read (line 2105-2106: copy-and-clear pattern), preventing terrain state from bleeding into subsequent non-terrain draws.

---

## 2. Texture Binds

### Unit assignment per draw:

| Unit | Bound To | Binding Site | Frequency |
|------|----------|--------------|-----------|
| GL_TEXTURE0 (tex1) | Terrain colormap / splat tile | `applyRenderStates()` via gos_State_Texture → glBindTexture | **Per-node** (changes with each unique tile) |
| GL_TEXTURE1 (tex2) | Detail normal (engine default, fallback) | applyRenderStates via gos_State_Texture2 | Per-batch (static after init) |
| GL_TEXTURE2 (tex3) | Detail displacement (legacy, unused with POM) | applyRenderStates via gos_State_Texture3 | Per-batch (static) |
| GL_TEXTURE5–9 (matNormal0–4) | Per-material normal maps: rock/grass/dirt/concrete/snow | gameos_graphics.cpp:2741–2747 inside `terrainDrawIndexedPatches` | **Per-invocation of terrainDrawIndexedPatches**, but all five are the same handles every frame — effectively static |
| GL_TEXTURE9 (shadowMap) | Static terrain shadow map | gameos_graphics.cpp:2759–2760 | Per-invocation (same handle every frame) |
| GL_TEXTURE10 (dynamicShadowMap) | Dynamic object shadow map | gameos_graphics.cpp:2770–2771 | Per-invocation (same handle every frame; conditional on FBO existing) |

**Critical finding:** Unit 0 (`tex1`) is the only binding that *changes* between nodes within a single frame's terrain submission. Units 5–10 are re-bound on every `terrainDrawIndexedPatches` call but carry the same GL handle every time — they are conceptually one bind per frame wasted across multiple calls.

---

## 3. Shader Program Switches

**No shader switches within the terrain solid pass.**

The path calls `material->apply()` (gameos_graphics.cpp:2688) once per `terrainDrawIndexedPatches` invocation. `apply()` calls `glUseProgram`. Because the same `terrain_material_` pointer is used for every node, the shader program is the same every time — `glUseProgram` with the same program ID does not cause a GPU pipeline stall in drivers, but the call still happens per-draw.

**Distinct programs used:** 1 (the terrain tessellation shader: `gos_terrain.vert / .tcs / .tes / .frag`).

---

## 4. Draw Call Structure Inside `terrainDrawIndexedPatches`

**File:** `GameOS/gameos/gameos_graphics.cpp:2677–2836`

### glDraw* calls per invocation: **exactly 1**

```
glDrawElements(GL_PATCHES, ni,
    mesh->getIndexSizeBytes() == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
    NULL);  // gameos_graphics.cpp:2822
```

No internal loop. The hardware tessellation pipeline (TCS/TES) handles subdivision at the GPU level for all patches in `indexed_tris_` at once.

### What happens per invocation:
1. `mesh->uploadBuffers()` — re-uploads the `indexed_tris_` VBO+IBO (line 2685); **per-draw**
2. `material->apply()` — `glUseProgram` + flush cached uniforms (line 2688); **per-draw**
3. ~15 direct `glUniform*` calls for tessellation params, MVP, light dir, tiling, POM, cell-bomb, time, map extent (lines 2700–2738); **per-draw**
4. 5× `glBindTexture` for matNormal0–4 on units 5–9 (lines 2741–2747); **per-draw**
5. `glBindTexture` for static shadow map on unit 9 (line 2760); **per-draw**
6. Conditional `glBindTexture` for dynamic shadow map on unit 10 (line 2771); **per-draw**
7. `updateBuffer(terrain_extra_vb_, ...)` — re-uploads per-node extras (world pos + normals) (lines 2793–2794); **per-draw**
8. `glGetAttribLocation` × 2 for worldPos/worldNorm (lines 2797–2798); **per-draw** (not cached)
9. **1× `glDrawElements(GL_PATCHES, ...)`** (line 2822)

### What drives the number of invocations:
The outer loop in `Render.TerrainSolid` (txmmgr.cpp:1300) iterates `masterVertexNodes[0..nextAvailableVertexNode-1]`. Each node that passes the `MC2_DRAWSOLID && vertices` check produces one `gos_RenderIndexedArray` call → one `drawIndexedTris` call → one `terrainDrawIndexedPatches` call → one `glDrawElements`.

If a single node's vertex count exceeds `MAX_SENDDOWN` (10,002), the chunking loop at txmmgr.cpp:1341–1351 issues multiple `gos_RenderIndexedArray` calls for the same node with the same texture. Each chunk produces its own `terrainDrawIndexedPatches` + `glDrawElements`.

---

## 5. Material Grouping Mechanism

### Node structure (`MC_VertexArrayNode`, txmmgr.h:72–100):

```cpp
typedef struct _MC_VertexArrayNode {
    DWORD           textureIndex;   // maps to masterTextureNodes slot = GL handle
    DWORD           flags;          // MC2_ISTERRAIN, MC2_DRAWSOLID, etc.
    long            numVertices;
    gos_VERTEX     *currentVertex;  // write pointer
    gos_VERTEX     *vertices;       // start of vertex pool block
    gos_TERRAIN_EXTRA *extras;      // per-vertex world pos + normal
    gos_TERRAIN_EXTRA *currentExtra;
} MC_VertexArrayNode;
```

### Grouping model:
`masterVertexNodes` is **pre-grouped by texture at population time** (during `land->render()` calls). Each unique `textureIndex` gets its own node slot. The `Render.TerrainSolid` loop processes nodes in allocation order — no sort step at render time.

**Consequence:** The number of draw calls equals the number of unique terrain tile textures visible on screen, plus extra draws for any tiles exceeding the MAX_SENDDOWN threshold.

### Does terrainDrawIndexedPatches batch across nodes?
**No.** Every call to `gos_RenderIndexedArray` for a terrain node immediately triggers `applyRenderStates()` + `terrainDrawIndexedPatches` inside `drawIndexedTris`, then rewinds `indexed_tris_`. There is no accumulation across nodes.

---

## 6. Depth, Blend, and Cull State

State is set at `renderLists()` entry (txmmgr.cpp:~1010–1060) and **does not change** within the `Render.TerrainSolid` zone.

| State | Value | GL Mapping | Changes in TerrainSolid? |
|-------|-------|-----------|--------------------------|
| `gos_State_ZCompare` | `1` | GL_LEQUAL depth test ON | No |
| `gos_State_ZWrite` | `1` | `glDepthMask(GL_TRUE)` | No |
| `gos_State_AlphaMode` | `gos_Alpha_OneZero` | Blending OFF | No |
| `gos_State_Culling` | `gos_Cull_CW` | Back-face cull, CW winding | No |
| Wireframe | `terrain_wireframe_` flag | `glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)` | Set once per terrainDrawIndexedPatches call (line 2814) |

The render contract header in `gos_terrain.frag` (lines 9–19) confirms: `depthTest=true, depthWrite=true, blend=Opaque`.

---

## 7. Estimated Draw Count per Frame

| Scenario | Governing Variable | Estimated glDraw* Calls |
|----------|-------------------|-------------------------|
| Standard zoom (~500 altitude) | `nextAvailableVertexNode` for terrain tiles visible | **5–15 draws** |
| Wolfman altitude (visibleVerticesPerSide=200) | Same, but more tiles visible | **15–40 draws** |
| MAX_SENDDOWN chunking triggered | Tiles with >10,002 vertices | +1 draw per chunk per tile |

**Governing variable:** `nextAvailableVertexNode` — the count of populated `MC_VertexArrayNode` entries. Each unique terrain colormap tile gets one slot. A typical MC2 map uses 10–30 distinct tile textures visible at any zoom; Wolfman mode expands the visible set to potentially 50+ tile textures.

**Secondary factor:** MAX_SENDDOWN chunking (txmmgr.cpp:1334–1351). If a single tile's triangle count exceeds ~10,002 vertices, it is split into multiple 10k-vertex sub-draws with the same texture. At Wolfman altitude a single high-frequency tile could generate 2–3 chunks.

---

## PatchStream Draw-Call Verdict

### 1. Can terrain solid be collapsed to one draw per frame?

**No — not without significant restructuring.**

The blocking constraint: **`tex1` (unit 0) is a `uniform sampler2D`** (gos_terrain.frag:34), binding one terrain colormap tile per draw. Setting `gos_State_Texture` per-node before each `gos_RenderIndexedArray` call changes the texture on unit 0. Because `applyRenderStates()` fires synchronously inside `drawIndexedTris` before the tessellation check, each node immediately flushes with its own texture bound. There is no deferred batching across nodes.

### 2. Minimum number of draws, and what must change to reach it

**Current minimum:** 1 draw per unique visible terrain tile texture. At standard zoom: ~5–15. At Wolfman: ~15–40.

**To reach 1 draw per frame**, the following changes are required:

| Change | Where | Complexity |
|--------|-------|-----------|
| Replace `uniform sampler2D tex1` with `uniform sampler2DArray tex1` in `gos_terrain.frag` | Shader | Low |
| Pack all terrain colormap tiles into a `GL_TEXTURE_2D_ARRAY` at load time | Texture manager init | Medium |
| Store a per-vertex `float tileLayer` attribute in `gos_VERTEX` or `gos_TERRAIN_EXTRA` | Vertex layout | Medium |
| Pass `tileLayer` through TCS/TES to frag shader, use as `texture(tex1, vec3(uv, layer))` | All terrain shader stages | Low |
| Remove per-node `gos_SetRenderState(gos_State_Texture, ...)` call in Render.TerrainSolid | txmmgr.cpp:1331/1336 | Low |
| Stop rewinding `indexed_tris_` after each node; let all terrain nodes accumulate | gameos_graphics.cpp:2998 | Medium — changes flush semantics |
| Remove MAX_SENDDOWN chunking (replace with GPU VBO of full mesh) | txmmgr.cpp:1334–1351 | Medium |
| Cache `glGetAttribLocation` for worldPos/worldNorm outside `terrainDrawIndexedPatches` | gameos_graphics.cpp:2797–2798 | Low |

**Achievable intermediate target (1 draw per material class, ~2–4 draws):** Pack tile colormaps of the same biome into texture arrays. Keep the 5-way material-normal structure. This requires texture-array support in the tile loader but no changes to the vertex format.

### 3. Is per-material texture rebinding the primary barrier?

**Yes.** The per-node `gos_State_Texture` change (unit 0, `tex1`) is the direct cause of one-draw-per-tile. Everything else — shader switches (none), depth/blend state (static), shadow/normal map binds (same handle each call) — is free or near-free.

**Shader uses individual `sampler2D` declarations, not a texture array:**

```glsl
// gos_terrain.frag:34-44
uniform sampler2D tex1;        // colormap — ONE tile per draw
uniform sampler2D tex2;        // detail normal (fallback)
uniform sampler2D tex3;        // displacement (legacy)
uniform sampler2D matNormal0;  // rock
uniform sampler2D matNormal1;  // grass
uniform sampler2D matNormal2;  // dirt
uniform sampler2D matNormal3;  // concrete
uniform sampler2D matNormal4;  // snow
```

The per-material normal maps (matNormal0–4) are rebound every `terrainDrawIndexedPatches` call but carry the same GL handle each time — wasted state setting, not a structural barrier. The colormap `tex1` is the only binding that genuinely changes per draw and forces the draw boundary.

**Summary table:**

| Barrier | Impact | Required fix |
|---------|--------|-------------|
| `tex1` per-node rebind (colormap tile) | Forces 5–40 draws/frame | `sampler2DArray` + per-vertex layer index |
| MAX_SENDDOWN 10k-vertex chunking | +1–3 draws on large tiles | Remove limit; use full VBO |
| matNormal0–4 redundant rebind | Wasted GL calls, not extra draws | Cache outside draw loop |
| `glGetAttribLocation` per-draw | CPU overhead, not extra draws | Cache at shader compile time |

The path to PatchStream is gated entirely on the `sampler2DArray` migration for terrain colormap tiles.
