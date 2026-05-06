# Static-Shadow Terrain Dependency Audit
**Date:** 2026-04-27  
**Branch:** terrain-pbr-mod  
**Worktree:** `.claude/worktrees/nifty-mendeleev/`  
**Mode:** read-only code trace  
**Purpose:** Determine what `Shadow.StaticAccum` reads from `MC_VertexArrayNode` before Shape B (`PatchStream`) ships its M0b slice.

---

## Q1 — Does `Shadow.StaticAccum` read `extras`, `vertices`, both, or neither?

**Answer: Both.**

The `Shadow.StaticAccum` arm in `mclib/txmmgr.cpp` reads every non-trivial field of `MC_VertexArrayNode` for terrain solid nodes:

| Field | Lines | How used |
|---|---|---|
| `flags` | 1227–1228 | Gate: must have `MC2_DRAWSOLID` AND `MC2_ISTERRAIN` |
| `vertices` | 1229, 1235, 1246 | NULL check; pointer arithmetic for `totalVerts`; passed to draw call |
| `numVertices` | 1232 | Initializes `totalVerts` count |
| `currentVertex` | 1233 | Pointer-delta to count actually-filled vertices |
| `extras` | 1230, 1239, 1248 | NULL check; pointer arithmetic for `extraCount`; passed to draw call |
| `currentExtra` | 1238 | Pointer-delta to count actually-filled extras |
| `textureIndex` | 1243 | Index into `masterTextureNodes` for texture handle lookup |

The arm exits early (skips the node) if any of three conditions fail — all checked before the draw:

```cpp
// txmmgr.cpp:1227-1230
if (!(masterVertexNodes[si].flags & MC2_DRAWSOLID)) continue;
if (!(masterVertexNodes[si].flags & MC2_ISTERRAIN)) continue;
if (!masterVertexNodes[si].vertices) continue;
if (!masterVertexNodes[si].extras) continue;
```

A node whose `extras` is `nullptr` is silently skipped — no crash, no assert.  
A node whose `vertices` is `nullptr` is also silently skipped.

The filled counts are derived by pointer arithmetic, not a separate count field:

```cpp
// txmmgr.cpp:1232-1240
int totalVerts = masterVertexNodes[si].numVertices;
// actual filled verts = (currentVertex - vertices)
totalVerts = (int)(masterVertexNodes[si].currentVertex
                 - masterVertexNodes[si].vertices);
// actual filled extras = (currentExtra - extras)
int extraCount = (int)(masterVertexNodes[si].currentExtra
                     - masterVertexNodes[si].extras);
```

Both buffers — along with a pre-allocated sequential identity index buffer (`indexArray`, txmmgr.cpp:249–251, `indexArray[i] = i`) — are passed to the GPU upload function:

```cpp
// txmmgr.cpp:1242-1248
if (totalVerts > 0 && extraCount > 0) {
    gos_SetRenderState(gos_State_Texture,
        masterTextureNodes[masterVertexNodes[si].textureIndex].get_gosTextureHandle());
    gos_DrawShadowBatchTessellated(
        masterVertexNodes[si].vertices, totalVerts,   // vertex data + count
        indexArray, totalVerts,                        // index buffer (sequential 0..N-1)
        masterVertexNodes[si].extras, extraCount);     // extras data + count
}
```

`indexArray` is a global `WORD*` pre-filled with `0, 1, 2, …` up to `MC_MAXFACES`. It is not part of `MC_VertexArrayNode`; the shadow path reuses the shared sequential IB for all batches.

The `masterTextureNodes` access is read-only: only `get_gosTextureHandle()` is called (txmmgr.cpp:1243), which reads the `gosTextureHandle` field of `MC_TextureNode`.

---

## Q2 — What specific fields are required for correct shadow output?

### From `MC_VertexArrayNode`

All of the following must be non-null and correctly populated for the shadow path to produce output at all:

- **`flags`** — must include both `MC2_DRAWSOLID` and `MC2_ISTERRAIN` (gate, `txmmgr.cpp:1227–1228`)
- **`vertices`** — must be non-null (`txmmgr.cpp:1229`); content is uploaded to the main VBO
- **`currentVertex`** — must point past the last filled entry so pointer-delta gives the correct `totalVerts` (`txmmgr.cpp:1233`)
- **`extras`** — must be non-null (`txmmgr.cpp:1230`); content is uploaded to the terrain-extra VBO
- **`currentExtra`** — must point past the last filled extra so pointer-delta gives the correct `extraCount` (`txmmgr.cpp:1238`)
- **`textureIndex`** — must be a valid index into `masterTextureNodes` (`txmmgr.cpp:1243`)

### From the `gos_TERRAIN_EXTRA` struct (the `extras` buffer)

Defined in `GameOS/include/gameos.hpp:2144–2147`:

```c
typedef struct {
    float wx, wy, wz;   // world-space position
    float nx, ny, nz;   // vertex normal
} gos_TERRAIN_EXTRA;
```

Both sub-groups are consumed by the GPU inside `gosRenderer::drawShadowBatchTessellated`
(`GameOS/gameos/gameos_graphics.cpp:2516–2524`):

```cpp
// gameos_graphics.cpp:2517-2524
updateBuffer(terrain_extra_vb_, GL_ARRAY_BUFFER,
    extras, extraCount * sizeof(gos_TERRAIN_EXTRA), GL_DYNAMIC_DRAW);
glBindBuffer(GL_ARRAY_BUFFER, terrain_extra_vb_);

glEnableVertexAttribArray(4);  // worldPos
glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE,
    sizeof(gos_TERRAIN_EXTRA), (void*)0);              // wx,wy,wz → attrib 4
glEnableVertexAttribArray(5);  // worldNorm
glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE,
    sizeof(gos_TERRAIN_EXTRA), (void*)(3*sizeof(float))); // nx,ny,nz → attrib 5
```

`wx/wy/wz` drive the light-space transform in the shadow TES shader.  
`nx/ny/nz` drive the flat geometric normal used by `calcShadow`.

### From the `gos_VERTEX` struct (the `vertices` buffer)

Defined in `GameOS/include/gameos.hpp:2131–2140`. `drawShadowBatchTessellated` uploads these to
`indexed_tris_` and calls `shadow_terrain_material_->applyVertexDeclaration()` (`gameos_graphics.cpp:2514`),
which binds attribute locations 0–3. The fields that actually matter to the shadow shader are:

- **`x, y, z`** — screen-space position (used to reconstruct clip coordinates in the TES)
- **`rhw`** — reciprocal homogeneous w (paired with x/y/z for clip reconstruction)
- **`u, v`** — texture coordinates (passed through; may or may not be used by shadow shader)

`argb` and `frgb` are bound to the vertex declaration but the shadow shader does not read color or fog — however the binding itself must succeed, so the vertex stride must be correct (i.e., a valid `gos_VERTEX`-sized buffer must be present).

### From `fillTerrainExtra` → `addTerrainExtra`

`quad.cpp:93–101` → `txmmgr.h:1085–1109`. The extras buffer is populated from game-mesh data:

```cpp
// quad.cpp:95-100
textra[i].wx = vertex->vx;                        // game-world x
textra[i].wy = vertex->vy;                        // game-world y
textra[i].wz = vertex->pVertex->elevation;        // terrain elevation
textra[i].nx = vertex->pVertex->vertexNormal.x;
textra[i].ny = vertex->pVertex->vertexNormal.y;
textra[i].nz = vertex->pVertex->vertexNormal.z;
```

These six floats per vertex are the **only correctness-critical data** in the extras buffer. The `vertices` buffer (gos_VERTEX) does not supply world-position or normal to the shadow shader — the shadow TES reads those exclusively from `extras`.

`addTerrainExtra` lazily allocates the extras block on first write (`txmmgr.h:1101–1103`), sized to `node->numVertices * sizeof(gos_TERRAIN_EXTRA)`, and advances `currentExtra` by 3 per call (one triangle). No extras buffer exists until `fillTerrainExtra` fires for that node.

---

## Q3 — What happens if the solid-color path stops populating `MC_VertexArrayNode` entries?

Three distinct failure modes depending on which field stops being written:

### Case A — `vertices` not populated (null or zero-filled)

`txmmgr.cpp:1229` gates on `vertices != nullptr`. If the solid-color path no longer allocates or writes `vertices` for terrain nodes:

- The null check fires → the node is **silently skipped**
- `gos_DrawShadowBatchTessellated` is never called for that node
- Static shadow map contribution for that node's geometry is **absent, not garbage**
- No crash; **graceful degradation** (missing terrain shadow patches, not a hang or assert)

### Case B — `extras` not populated (null or zero-filled)

`txmmgr.cpp:1230` gates on `extras != nullptr`. `addTerrainExtra()` lazily allocates the extras buffer on first write (`txmmgr.h:1101`). If `fillTerrainExtra` is never called for a node:

- `extras` remains `nullptr` (lazy alloc never fires)
- The null check fires → the node is **silently skipped**
- Static shadow map has no contribution from that node
- No crash; **graceful degradation** (same as Case A)

If `extras` is allocated but `currentExtra` is left pointing at `extras` (zero entries written):

- `extraCount = currentExtra - extras = 0`
- The `if (totalVerts > 0 && extraCount > 0)` guard at `txmmgr.cpp:1242` prevents the draw call entirely
- `gos_DrawShadowBatchTessellated` is **never reached** for that node
- `drawShadowBatchTessellated` itself also has a second guard at `gameos_graphics.cpp:2456` (`if (!shadow_prepass_active_ || numVerts <= 0 || extraCount <= 0) return;`)
- No VBO is updated, no draw call is issued — **no output, no crash**

### Case C — `currentVertex` or `currentExtra` not advanced (count = 0)

Pointer-delta yields 0 for `totalVerts` or `extraCount`. The `if (totalVerts > 0 && extraCount > 0)` guard at `txmmgr.cpp:1242` prevents the draw call. Silent omission — same as Case B's zero-count sub-case.

### Case D — `flags` missing `MC2_DRAWSOLID` or `MC2_ISTERRAIN`

Node is skipped at `txmmgr.cpp:1227–1228`. Same silent omission.

### Summary

The shadow path **never crashes** if `MC_VertexArrayNode` stops being populated. Every access is guarded by a null or zero check that silently skips the node. The failure mode is always **missing shadow coverage** (terrain patches that cast no static shadow), not undefined behavior or a hang.

The only way to get *wrong* rather than *absent* shadow output is to have `extras` non-null but containing stale data from a prior frame alongside `currentExtra` not reset. This cannot happen in practice: `clearArrays` (`txmmgr.h:1120–1141`) calls `free(masterVertexNodes[j].extras)` for every node, then `memset`s the entire `masterVertexNodes` array to zero — leaving both `extras` and `currentExtra` as `NULL`. The lazy-alloc in `addTerrainExtra` re-allocates fresh each frame only when `fillTerrainExtra` is actually called. There is no "dirty" flag that `fillTerrainExtra` sets; freshness is guaranteed by the free+memset cycle.

---

## Shape B Safety Verdict

Given what `Shadow.StaticAccum` actually reads, Shape B's M0b slice must preserve the following minimum contract for any terrain solid node that should cast static shadows:

**Must preserve:**
1. **`MC_VertexArrayNode::extras` must be non-null** and populated via `fillTerrainExtra` / `addTerrainExtra` in the same frame. The six floats per vertex (`wx, wy, wz, nx, ny, nz`) are the only data the shadow TES shader reads for light-space transform and surface normal. Shape B **cannot** stop calling `fillTerrainExtra` for terrain nodes without losing shadow coverage for those nodes.

2. **`MC_VertexArrayNode::currentExtra` must be advanced** past the last written extra. The shadow path computes `extraCount` purely from `currentExtra - extras`. If Shape B changes the extras-fill loop and forgets to advance this pointer, the shadow path will silently draw nothing for that node.

3. **`MC_VertexArrayNode::vertices` must be non-null** and contain a valid `gos_VERTEX`-sized buffer. The shadow shader does not read color or fog from these vertices, but the GPU vertex declaration binds attributes 0–3 from this buffer. A null `vertices` causes the node to be skipped entirely (safe but shadow-absent). A valid-but-zero-filled `vertices` would produce shadow patches at screen-origin — which would be visually wrong. Shape B must either keep vertices populated normally or accept the null-skip.

4. **`currentVertex` must be advanced** so the totalVerts pointer-delta is correct.

5. **`flags` must retain `MC2_DRAWSOLID | MC2_ISTERRAIN`** for terrain solid nodes.

**Can safely stop writing:**
- `argb`, `frgb` in `gos_VERTEX` — shadow shader ignores color and fog; these are bound but not read by the shadow program
- Any fields inside `extras` beyond `wx/wy/wz/nx/ny/nz` — there are none; the struct is exactly 6 floats

**The one-line verdict:** Shape B may freely restructure how `vertices` are built (including switching to a PatchStream path) as long as it continues calling `fillTerrainExtra` for each terrain triangle and advancing `currentExtra` correctly — the shadow path will continue to produce correct static shadows regardless of what the color vertices contain.
