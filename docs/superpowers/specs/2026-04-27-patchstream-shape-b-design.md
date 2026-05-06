# Shape B PatchStream — Design Spec (M0b, solid terrain color path only)

Status: spec, planning only. No source modifications yet.
Worktree: `.claude/worktrees/nifty-mendeleev/`
Branch: `claude/nifty-mendeleev`
Author: planning session 2026-04-27, post Shape A (TexResolveTable, default-ON).
Revision 2 (2026-04-27): mechanism changed from SSBO → persistent-mapped
VBO after the static-shadow / shader-input / state-binding audits. See §1
and §5 for rationale.

Revision 3 (2026-04-27): static-shadow audit landed at
[2026-04-27-static-shadow-terrain-dependency.md](../explorations/2026-04-27-static-shadow-terrain-dependency.md).
Three refinements: (a) `MC_VertexArrayNode` uses pointer-delta arithmetic
(`currentVertex - vertices`, `currentExtra - extras`) as the fill count
— a load-bearing invariant Shape B must preserve; (b) the index data is
a global identity buffer (`indexArray[i] = i`, `txmmgr.cpp:249–251`),
not per-node, so Shape B's index ring can be a simple identity range
or eliminated entirely with `glDrawArrays`; (c) the shadow shader
*binds* but does not *read* the `gos_VERTEX` color/fog fields — only
position data is sampled — so what's load-bearing is the buffer's
existence + size + position bytes, not its color content. §3 / §7.3 /
§9 / §10 BR3 updated accordingly.

Revision 5 (2026-04-27): one stale operational SSBO reference at §7.2
("`glBindBufferRange` + one draw per bucket") rewritten to
`glDrawArrays(GL_PATCHES, firstVertex, vertexCount)`. All remaining
SSBO mentions in this document are deliberate decision-justifying
references in the rev banners, "Why VBO not SSBO" section (§1), §5.1,
and §7.1's Q-B1 answer — those contrast the chosen mechanism against
the rejected alternative and must NOT be edited away. Operational
language is now uniformly persistent-mapped-VBO.

Revision 4 (2026-04-27): consistency cleanup pass. All stale SSBO
references rewritten in VBO terms. Extras-VBO policy made explicit
(option A: modern path uses persistent-mapped extras ring for its own
draws AND issues one consolidated per-frame `updateBuffer` of
`terrain_extra_vb_` for grass — see §7.5 and §9.1). Index strategy
made explicit (preference: `glDrawArrays(GL_PATCHES,…)` since the
legacy index pattern is identity; fallback: `GL_UNSIGNED_INT` indices
or per-bucket split if any bucket exceeds 65,535 vertices — see §6.5).
Overflow policy upgraded from per-triangle drop to whole-frame
fallback to legacy `Render.TerrainSolid` (§8.3). Verification §12 #1
relaxed: shader-cache clear is optional defensive hygiene, not
required (no new shaders in M0b). Grass-deprecation framing added
to §7.5. Default-on policy made explicit in §8.1.

---

## 1. One-line thesis

Replace the per-batch `glBufferData(GL_DYNAMIC_DRAW)` orphan + upload of
the `gos_VERTEX` color VBO and `gos_TERRAIN_EXTRA` extras VBO inside
`terrainDrawIndexedPatches` with **triple-buffered persistent-mapped
VBOs** (same `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` + fence-rotation
pattern as `gos_static_prop_batcher.cpp`, but bound as `GL_ARRAY_BUFFER`
with the **existing** `gos_VERTEX` and `gos_TERRAIN_EXTRA` attribute
layouts at locations 0–5). The new `TerrainPatchStream::issueDraws()`
keeps the existing one-`glDrawElements(GL_PATCHES,…)`-per-tile structure
that today's path is forced into by `tex1` being a per-node `uniform
sampler2D`. After Shape A's measured 0.85 ms/frame net saving, this
targets the residual `Terrain.DrawPatches ~0.51 ms/frame` plus the
~7.6 MB/frame color-pass upload churn at Wolfman
(`visibleVerticesPerSide=200`, ~237,606 vertices/frame × 32 B
`gos_VERTEX`).

**Why VBO, not SSBO** (the explicit reversal from rev 1):

1. The shader-input audit
   ([2026-04-27-terrain-shader-input-map.md](../explorations/2026-04-27-terrain-shader-input-map.md))
   confirms the entire 4-stage tessellation pipeline (VS→TCS→TES→FS) has
   **zero SSBO bindings today**. `worldPos`/`worldNorm` already arrive
   via VBO at locations 4–5. Switching to SSBO would require new TES
   logic that reads by `gl_VertexID` — and `gl_VertexID` semantics in a
   tessellated primitive are subtle (post-tessellation in TES, requiring
   TCS-side per-control-point plumbing to round-trip the original index)
   versus pre-tessellation in VS. None of that complexity is needed if
   the data still arrives as vertex attributes.
2. A persistent-mapped **VBO** with the same coherent flags as the
   static-prop batcher gives the exact GPU-upload-elimination win we
   want, with **zero shader changes** and **zero attribute-layout
   changes**. BR2 (GBuffer1 contract) drops to a non-issue for M0b.
3. Replacing both `terrain_extra_vb_` and the color VBO inside
   `terrainDrawIndexedPatches` is a buffer-mechanism swap, not a
   pipeline rewrite. The shader cache concern (BR8) drops as well —
   no new shader files in M0b.

---

## 2. Pool-headroom note (justification framing)

**Shape B is justified by submission overhead and GPU upload churn,
not by pool pressure.** Measured TGL pool peaks from a Wolfman
`mission_unload` event (`[TGL_POOL v1]`):

| Pool | Peak | Cap | Headroom |
|---|---:|---:|---:|
| vertex / color / shadow | 18,011 | 500,000 | 96.4% |
| triangle | 22,293 | 200,000 | 88.9% |
| face | 44,586 | 200,000 | 77.7% |
| MC textures | 823 | 3,000 | 72.6% |
| ABL functions | 310 | 512 | 39.5% |

Pools are comfortable. Any future PR description claiming Shape B
"prevents pool exhaustion" is **wrong** and must be corrected. The
two real wins are:

1. Eliminate ~7.6 MB/frame color-pass `glBufferData` upload (and a
   matching ~5.7 MB/frame extras VBO upload — see §9 caveat).
2. Reduce per-batch GL submission overhead in `Terrain.DrawPatches`
   (~0.51 ms/frame today).

---

## 3. What stays legacy / what becomes modern (verified)

### Stays legacy (M0b)

| Piece | File:line | Reason |
|---|---|---|
| `Terrain::geometry` admission, `vertexProjectLoop`, `setObjBlockActive`, `setObjVertexActive`, `clipInfo`, all 8 `projectFor*` wrappers, `pz` gate | `mclib/quad.cpp` (admission), `mclib/terrain.cpp` | Unchanged behavior; pre-cull pipeline is correct |
| `TerrainQuad::setupTextures` | `mclib/quad.cpp:setupTextures` | Owned by Shape A's TexResolveTable |
| Crater / decal / water / dynamic-shadow paths | `mclib/quad.cpp` various | Out of M0b scope |
| `MC_VertexArrayNode::vertices` ring (color VBO data) | `mclib/txmmgr.cpp` | Buffer is *bound* by `Shadow.StaticAccum` but the shadow shader reads only position fields (clip reconstruction) — color/fog are inert. M0b keeps the full `gos_VERTEX` ring populated for simplicity; B' may switch to a position-only buffer once the shadow shader's `applyVertexDeclaration` is decoupled |
| `MC_VertexArrayNode::extras` ring (`gos_TERRAIN_EXTRA`) | `mclib/txmmgr.cpp` | Load-bearing — `Shadow.StaticAccum` reads all six floats for light-space transform; grass also reads via `terrain_extra_vb_`. Lazy-allocated on first `addTerrainExtra` and `free()`+memset by `clearArrays()` every frame (`txmmgr.h:1120–1141`) |
| Pointer-delta fill-count invariant (`currentVertex - vertices`, `currentExtra - extras`) | `mclib/txmmgr.h:72–100` | Any modern path that advances one pointer without the other silently submits zero geometry. Modern `flush()` does not touch these pointers; legacy path's `addVertices` / `addTerrainExtra` continue to advance them |
| Global identity index buffer `indexArray[i] = i` | `mclib/txmmgr.cpp:249–251` | Shared across all terrain draws, MC_MAXFACES entries. Shape B can re-use it or replace its identity-index draw with `glDrawArrays(GL_PATCHES, …)` |
| `terrain_extra_vb_` (the legacy extras VBO upload) | `GameOS/gameos/gameos_graphics.cpp:2793` | Grass pass reads this VBO directly (§9 / BR7) |
| `fillTerrainExtra` (5.7 MB/frame writes) | `mclib/quad.cpp:93` | Always-on through M0b; B' migration |
| `addTriangle` capacity reservation (extras lazy-alloc depends on `numVertices`) | `mclib/txmmgr.h:689–822` and `:1085–1109` | Pure counter, no cull-cascade side effects (BR5 ✓), but extras alloc keys off it |

### Becomes modern (M0b)

| Piece | What |
|---|---|
| New class `TerrainPatchStream` | Two triple-buffered persistent-mapped VBO rings (one for `gos_VERTEX`, one for `gos_TERRAIN_EXTRA`) plus an index buffer ring. 3 slots each, `GL_MAP_WRITE_BIT \| GL_MAP_PERSISTENT_BIT \| GL_MAP_COHERENT_BIT`, one `glBufferStorage` per buffer, fence per slot. Bound as `GL_ARRAY_BUFFER` (and `GL_ELEMENT_ARRAY_BUFFER` for the index ring) — same target the existing path uses. **No SSBO binding.** |
| `TerrainQuad::draw` SOLID branch | Adds an *additional* append into the active patch-stream slot, gated by `MC2_MODERN_TERRAIN_SURFACE=1`. Legacy `addVertices` + `fillTerrainExtra` calls **stay** in M0b — the duplication caveat (§9) is preserved |
| `Render.TerrainSolid` arm | When killswitch is on: bypass the per-node `gos_RenderIndexedArray` → `terrainDrawIndexedPatches` upload path. Dispatch `TerrainPatchStream::flush()` instead, which still issues one `glDrawElements(GL_PATCHES,…)` per material bucket (~6–10 draws at Wolfman). Shadow loop is untouched |
| **Shaders** | **Unchanged.** No new TES, no new FS, no new attribute layout. The patch-stream VBOs use the existing `applyVertexDeclaration` for `gos_VERTEX` (locs 0–3) and the existing manual `glVertexAttribPointer` for `gos_TERRAIN_EXTRA` (locs 4–5). BR2 is a non-issue for M0b |
| `terrainDrawIndexedPatches` | Untouched on disk; bypassed when killswitch is on. A follow-on slice may delete it once parity is proven |

---

## 4. M0b slice steps (verified line numbers)

The brainstorm's approximate line numbers are replaced below with audit-verified citations.

1. **Add `GameOS/gameos/gos_terrain_patch_stream.{h,cpp}`** modeled on
   `GameOS/gameos/gos_static_prop_batcher.cpp:163–183` (storage flags
   + `glBufferStorage`) and `:148–154, 813` (fence + slot rotation).
   Constants in same style:
   ```cpp
   constexpr uint32_t RING_FRAMES = 3;
   constexpr uint32_t INITIAL_VERTICES_PER_FRAME = ~250000;   // see §6.4
   ```
   State save/restore must follow `gos_static_prop_batcher.cpp:692–712, 817–834`.

2. **No shader changes.** The persistent-mapped VBOs use the existing
   attribute layout (gos_VERTEX at locs 0–3, gos_TERRAIN_EXTRA at locs
   4–5). The existing `gos_terrain.{vert,tcs,tese,frag}` files are
   reused unmodified. This drops BR2 (GBuffer1 contract) and BR8 (stale
   shader cache) for M0b.

3. **`TerrainQuad::draw` SOLID branch** at `mclib/quad.cpp:1608, 1752,
   1911, 2053`. Inside the existing `pz` gate brace (so the modern
   append cannot bypass the gate — BR4), append the same triangle's
   3 vertices into the active patch-stream slot when
   `MC2_MODERN_TERRAIN_SURFACE=1`. The legacy `addVertices` and
   `fillTerrainExtra` calls **stay**, because:
   - Shadow.StaticAccum reads `MC_VertexArrayNode::vertices` (§9).
   - Grass pass reads `terrain_extra_vb_` (§9 / BR7).

4. **`Render.TerrainSolid`** at `mclib/txmmgr.cpp:1297–1358`. Wrap
   the iteration in an env-checked branch: when modern is on, replace
   the `for (long i=0;i<nextAvailableVertexNode...)` body with
   `TerrainPatchStream::flush()`. The shadow loop at lines 1210–1252
   is untouched.

5. **`TerrainPatchStream::issueDraws()`** issues one
   `glDrawElements(GL_PATCHES, ni, GL_UNSIGNED_SHORT, …)` per material
   group — see §6.5 below. Each draw binds the modern shader, calls
   the same direct `glUniform*` set as
   `GameOS/gameos/gameos_graphics.cpp:2700–2780`
   (terrainMVP `GL_FALSE`, terrainViewport, tessLevel, tessDistanceRange,
   tessDisplace, cameraPos, mapHalfExtent, terrainLightDir,
   detailNormalTiling, detailNormalStrength, pomParams,
   terrainWorldScale, cellBombParams, time, per-material normal maps
   on units 5–8/9, shadow maps on 9/10).

6. **Static-shadow re-feed path is unchanged.** `fillTerrainExtra`
   stays always-on. `Shadow.StaticAccum` continues to call
   `gos_DrawShadowBatchTessellated` reading from
   `MC_VertexArrayNode::vertices` and `::extras`.

7. **Lifecycle prints** (see §10) land in the same commit. No
   `_DEBUG`-only prints.

---

## 5. Modern buffer layout

### 5.1 Mechanism: persistent-mapped VBO, not SSBO

**Decision: triple-buffered persistent-mapped VBOs bound as
`GL_ARRAY_BUFFER`**, with the existing `gos_VERTEX` and
`gos_TERRAIN_EXTRA` layouts unchanged. Justification, in priority order:

1. **The shader path is already attribute-driven, no SSBO bindings
   exist.** Per the shader-input audit, the TES reads `worldPos`/
   `worldNorm` from VS-stage interpolated varyings sourced from VBO at
   locations 4–5. Migrating to SSBO would require new TES code that
   reads by `gl_VertexID` — and `gl_VertexID` in TES is **post-tessellation**
   (it indexes the tessellated vertex, not the original control point),
   so the SSBO read would have to happen in VS, then forward through
   TCS/TES as varyings — at which point the data flow is identical to
   keeping the VBO. Net change: zero, plus a new buffer type to bind.
2. **In-tree precedent** for `GL_MAP_PERSISTENT_BIT |
   GL_MAP_COHERENT_BIT`: `gos_static_prop_batcher.cpp:163–164` and
   `:166–183`. The flag set is independent of the bind target; the
   batcher uses SSBO because instance data is naturally indexed,
   terrain uses ARRAY_BUFFER because vertex data is naturally streamed.
3. **No new shader risk**: BR2 (GBuffer1 contract drift) and BR8 (stale
   shader cache) drop out of M0b entirely.
4. **AMD compatibility**: `docs/amd-driver-rules.md` rule line 13 bans
   `sampler2DArray` — this independently rules out collapsing draws to
   one (see §6). Independent of mechanism choice.
5. The `MC2 ARGB packing is BGRA-in-memory` memory rule continues to
   apply: the existing `applyVertexDeclaration` already handles the
   `argb` UBYTE4N decode at location 1 (offset 16) — Shape B preserves
   that exact path.

### 5.2 Buffer layout (unchanged from current path)

The persistent-mapped buffers carry the **same byte layout** as the
existing transient VBOs:

```c
// Buffer A (color/screen): 32 B per vertex, locations 0–3
typedef struct {
    float x, y, z, rhw;     // loc 0, 16 B
    DWORD argb;             // loc 1, 4 B  (UBYTE4N)
    DWORD frgb;             // loc 2, 4 B  (UBYTE4N)
    float u, v;              // loc 3, 8 B
} gos_VERTEX;                // 32 B

// Buffer B (world): 24 B per vertex, locations 4–5
typedef struct {
    float wx, wy, wz;       // loc 4, 12 B
    float nx, ny, nz;       // loc 5, 12 B
} gos_TERRAIN_EXTRA;        // 24 B
```

**No new packed `uint`s, no new SSBO struct, no `vec4` padding.** The
mapped pointer is written as raw `gos_VERTEX` / `gos_TERRAIN_EXTRA`
records — identical bytes to today, just into a persistent ring slot
instead of a per-batch `glBufferData` orphan.

The index buffer also rolls into the persistent ring, bound as
`GL_ELEMENT_ARRAY_BUFFER`, `GL_UNSIGNED_SHORT` indices, 0-based per
material bucket. `glDrawElementsBaseVertex` selects the bucket's vertex
range, OR a per-bucket `glBindBuffer` re-bind selects sub-ranges of the
ring slot — implementation chooses; both are valid.

### 5.3 Buffer flags

```cpp
const GLbitfield storageFlags =
    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
```

Identical to `gos_static_prop_batcher.cpp:163–164`. **Implicit flush
(coherent path) — no `glFlushMappedBufferRange` call.** This resolves
Q-B1 (see §7.1).

### 5.4 Sizing (Wolfman worst case)

Two persistent rings (color VBO + extras VBO) plus an optional index
ring. Numbers per ring slot, three slots per ring:

- `visibleVerticesPerSide = 200` (Wolfman)
- `TILESPERSIDE = 199`, visible quads = 199² = 39,601
- triangles/quad = 2 → ~79,202 triangles
- vertices/frame = 79,202 × 3 = **237,606**

| Ring | Per-vertex | Per-slot raw | + 25% headroom | × 3 slots |
|---|---:|---:|---:|---:|
| `gos_VERTEX` color | 32 B | 7.6 MB | 9.5 MB | **28.5 MB** |
| `gos_TERRAIN_EXTRA` | 24 B | 5.7 MB | 7.1 MB | **21.4 MB** |
| Index (`uint16_t`) — only if not using `glDrawArrays` | 2 B | 0.5 MB | 0.6 MB | **1.8 MB** |

Total persistent allocation: ~50 MB (color + extras) plus ~1.8 MB
optional index. The overflow detector (BR6) is mandatory — see §8.3
for the full-frame fallback policy and §10 for the lifecycle event.

**`gos_VERTEX` is 32 B fixed by `applyVertexDeclaration`. Do not pad
or repack** — the layout is consumed by an existing shader pipeline
that has not changed.

### 5.5 Slot rotation + fence

Same scheme as `gos_static_prop_batcher.cpp:148–154, 813`:
- Map full ring once at init via `glMapBufferRange(0, total, flags)`.
- Slot index = `frameCount % RING_FRAMES`.
- At end of `flush()`: `s_fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)`.
- On next visit to that slot the fence is *deleted* without waiting,
  relying on triple-buffering for safety. `glClientWaitSync` only on
  resize (which forces all in-flight slots to drain). Same "optimistic
  no-wait" pattern. If overflow is detected, ring grows on the
  resize path; growing requires draining all 3 fences.

---

## 6. Draw call architecture

### 6.1 How many draws

`terrainDrawIndexedPatches` today issues exactly **one
`glDrawElements(GL_PATCHES, …)` per call** (verified at
`GameOS/gameos/gameos_graphics.cpp:2822`). It is invoked **once per
populated `MC_VertexArrayNode` slot**, i.e. once per
`(textureIndex × flagSet)` pair. **The observed range is 5–40
draws/frame depending on scene** — ~5–15 at standard RTS zoom,
~15–40 at Wolfman altitude with more colormap tiles in view. The
implementation must log `[PATCH_STREAM v1] event=draw_count value=N`
at end-of-frame during validation so we can pin per-mission numbers
rather than relying on a single estimate.

**Shape B cannot collapse this.** The structural draw boundary is forced
by `tex1` being a per-node `uniform sampler2D` — each unique terrain
colormap tile must bind its own texture before drawing. The only
mechanism that would reduce draws is `sampler2DArray`, which is
explicitly banned by `docs/amd-driver-rules.md` line 13 (and CLAUDE.md
restates this). M0b therefore preserves the same one-draw-per-material
shape; the win is upload-elimination + per-batch state-set reduction,
not draw-call reduction.

### 6.2 What triggers a new draw

Material change. Specifically: a different `tex1` (colormap) bind
on unit 0 (or whatever unit terrain uses), since the AMD-banned
`sampler2DArray` rule (`docs/amd-driver-rules.md` rule line 13)
forces individual `sampler2D` per-material binds.

### 6.3 Material-binding mechanism in modern path

Inside `TerrainPatchStream::issueDraws()` (one slot's bind setup is
done once, then per-bucket draws walk the ranges within that slot):

```
// once per flush(), after slot rotation:
glBindBuffer(GL_ARRAY_BUFFER, colorRing.bufferId);
material->applyVertexDeclaration();              // locs 0–3 from gos_VERTEX
glBindBuffer(GL_ARRAY_BUFFER, extraRing.bufferId);
glEnableVertexAttribArray(/*loc 4 worldPos*/);
glVertexAttribPointer(/*loc 4*/, 3, GL_FLOAT, GL_FALSE, 24, 0);
glEnableVertexAttribArray(/*loc 5 worldNorm*/);
glVertexAttribPointer(/*loc 5*/, 3, GL_FLOAT, GL_FALSE, 24, (void*)12);
// (attribute locations cached at material init — see §12.5 #1)

// per material bucket g, in the active slot:
for (each material group g in this slot):
    gos_SetRenderState(gos_State_Texture, tex_resolve(g.textureIndex));
    material->apply();                           // direct uniforms after this
    // direct glUniform* set (terrainMVP GL_FALSE, etc.) per §4.5
    glDrawArrays(GL_PATCHES, g.firstVertex, g.vertexCount);
    // OR (if indexed path is needed; see §6.5):
    // glDrawElementsBaseVertex(GL_PATCHES, g.indexCount,
    //     GL_UNSIGNED_INT, (void*)g.indexByteOffset, g.firstVertex);
```

The grouping is built during `TerrainQuad::draw` append: each append
is keyed by `textureIndex` and bucketed into a per-material range
within the slot. `flush()` walks the buckets in `textureIndex` order
and emits one draw per bucket. **The bind target is `GL_ARRAY_BUFFER`,
not `GL_SHADER_STORAGE_BUFFER`** — the rev-2 mechanism reversal made
this a vertex-attribute path, not an SSBO path.

### 6.4 Required by AMD rules per draw

- `material->end()` deactivates the program (rule line 11), so the
  outer loop **must re-`apply()` and re-upload all direct uniforms
  every iteration**. Do not optimize this away.
- `glUniformMatrix4fv(... GL_FALSE ...)` for `terrainMVP` (rule line 9
  + memory `terrain_mvp_gl_false`).
- No `sampler2DArray`. Per-material colormaps on individual units.

### 6.5 Index assembly

The static-shadow audit confirmed the legacy index buffer is a
global identity sequence (`indexArray[i] = i`, `txmmgr.cpp:249–251`).
That makes the index data redundant and gives M0b two clean options:

**Preferred: `glDrawArrays(GL_PATCHES, firstVertex, vertexCount)`** —
no index buffer at all. Each material bucket draws its contiguous
vertex range directly. Eliminates index width and bucket-overflow
concerns entirely. This is the recommended path.

**Fallback: indexed draw with `GL_UNSIGNED_INT`** — if some future
extension needs index reuse (e.g. a tessellation patch ABI that
requires explicit indices), use 32-bit indices, **not** 16-bit.
Wolfman's ~237,606 vertices/frame can be split across 5–40 buckets,
so individual buckets *usually* stay under 65,535 vertices, but
**any single material bucket exceeding 65,535 verts breaks
`GL_UNSIGNED_SHORT`**. The implementation must:
1. Use `GL_UNSIGNED_INT` indices unconditionally, OR
2. Detect at append time when a bucket would exceed 65,535 vertices
   and split it into a sibling bucket of the same `textureIndex`
   (extra draw, same material). Log
   `[PATCH_STREAM v1] event=bucket_split textureIndex=N`.

Given that `glDrawArrays` is simpler, faster, and avoids the index
ring entirely, **the implementation plan should default to it**.
The indexed path is documented for completeness only.

---

## 7. Open questions resolved

### 7.1 Q-B1 / Q-B1a — buffer mechanism + flush variant

**Answer (rev 2 reversal, see §1):** Persistent-mapped **VBO** bound
as `GL_ARRAY_BUFFER`, with `GL_MAP_COHERENT_BIT` (implicit flush).
No `glFlushMappedBufferRange`. Two rings: one for `gos_VERTEX` (locs
0–3), one for `gos_TERRAIN_EXTRA` (locs 4–5). No SSBO is introduced.

**Evidence — flag set:** `GameOS/gameos/gos_static_prop_batcher.cpp:
163–164` uses `GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
GL_MAP_COHERENT_BIT` in production on AMD RX 7900 XTX. The flag set
is independent of the bind target. The static-prop batcher binds the
result as SSBO because instance data is naturally indexed; terrain
binds as `GL_ARRAY_BUFFER` because the existing shader path is
attribute-driven.

**Evidence — bind target:** Per the shader-input audit
([2026-04-27-terrain-shader-input-map.md](../explorations/2026-04-27-terrain-shader-input-map.md)),
`shaders/gos_terrain.vert` declares attribute locations 0–5 and
neither `gos_terrain.tese` nor `.frag` declares any SSBO binding.
Switching to SSBO would require new TES code that reads by
`gl_VertexID` — and `gl_VertexID` in TES is post-tessellation, so
the SSBO read would have to be moved to VS and forwarded as
varyings, at which point the data flow is identical to keeping the
VBO. Net change of going SSBO: zero structural benefit, plus risk
(BR2 GBuffer1 contract drift, BR8 stale shader cache). VBO wins.

**Evidence — AMD compatibility:** `docs/amd-driver-rules.md`
contains no rule against this pattern; the relevant constraints
(`GL_FALSE` direct matrix upload — rule line 9; `sampler2DArray`
ban — line 13; re-apply-after-end — line 11) are independent of
mechanism choice.

### 7.2 Q-B2 — draw count and material binding

**Answer:** ~6–10 draws/frame at Wolfman, one per material bucket.
**Cannot collapse to one draw** because per-material colormaps must
bind as individual `sampler2D` (AMD rule line 13 bans
`sampler2DArray`).

**Evidence:**
- `GameOS/gameos/gameos_graphics.cpp:2822` — single
  `glDrawElements(GL_PATCHES, …)` per `terrainDrawIndexedPatches`
  invocation.
- `mclib/txmmgr.cpp:1297–1358` — `Render.TerrainSolid` calls
  `gos_RenderIndexedArray` once per populated `masterVertexNodes[i]`,
  which dispatches one draw per slot.
- `docs/amd-driver-rules.md` line 13 — no `sampler2DArray`.
- AMD rules line 11 — `material->end()` calls `glUseProgram(0)`,
  forcing re-apply and re-upload of all direct uniforms every batch.

`flush()` must group by `textureIndex` before issuing draws and emit
one `glDrawArrays(GL_PATCHES, firstVertex, vertexCount)` per bucket
against the persistent-mapped VBO ring (no `glBindBufferRange` — that
was rev-1 SSBO language; rev-2+ uses standard `GL_ARRAY_BUFFER` bind
once per slot, per-bucket draws walk vertex ranges within).

### 7.3 Q-B3 — shadow re-feed

**Answer (refined by static-shadow audit):** `Shadow.StaticAccum`
**binds both** `MC_VertexArrayNode::vertices` and
`MC_VertexArrayNode::extras` and passes the global identity
`indexArray` (`txmmgr.cpp:249–251`). The shadow shader **reads** all
six `gos_TERRAIN_EXTRA` floats (the load-bearing data) but only the
position fields of `gos_VERTEX` for clip reconstruction — color/fog
are bound through `applyVertexDeclaration` but never sampled.

Failure modes are graceful skips, never crashes:
- `vertices == nullptr` → guard at `mclib/txmmgr.cpp:1229` skips.
- `extras == nullptr` (lazy-alloc never fired because `fillTerrainExtra`
  was not called) → guard at `txmmgr.cpp:1230` skips.
- `extraCount == 0` → guard at `txmmgr.cpp:1242` skips before
  dispatch (does NOT call `gos_DrawShadowBatchTessellated` with
  count=0).
- Inner guard at `gameos_graphics.cpp:2456` is a belt-and-suspenders
  re-check.

**Consequences for M0b:**
- `addVertices` + `fillTerrainExtra` continue to populate both rings,
  including the pointer-delta advance (see §3 invariant row). The
  modern stream does not touch these pointers.
- A node that only writes through the modern stream (and never
  through `addVertices`/`addTerrainExtra`) would be silently absent
  from the static-shadow accumulation. This is graceful but **wrong**
  for M0b; we want shadows preserved. Hence the legacy ring writes
  stay on for every solid terrain triangle.
- B' migration path: replace shadow's `vertices` bind with a
  position-only buffer (or read positions from the modern VBO) and
  drop the `addVertices` calls. Extras path (`fillTerrainExtra`)
  stays until shadow TES is rewritten to read from the modern stream.

Full evidence: see [2026-04-27-static-shadow-terrain-dependency.md](../explorations/2026-04-27-static-shadow-terrain-dependency.md).

### 7.4 Q-B4 — deferred vs direct uniforms

**Answer:** Direct `glUniform*` calls only, after
`material->apply()` (which calls `glUseProgram`). No deferred-uniform
plumbing required.

**Evidence:**
- `GameOS/gameos/gameos_graphics.cpp:2700–2780` — every uniform in
  `terrainDrawIndexedPatches` is direct `glUniform*` after
  `material->apply()`. There are no `setFloat`/`setInt` calls inside
  the draw.
- `docs/amd-driver-rules.md` line 10 — direct `glUniform*` AFTER
  `apply()`. Memory `deferred_vs_direct_uniforms.md` confirms.
- `gos_static_prop_batcher.cpp` follows the same direct-uniform
  pattern in production.

The modern `issueDraws()` keeps the same flush-order: at slot setup,
bind the persistent color and extras VBOs (once per frame); then per
material bucket, `material->apply()` → direct `glUniform*` set →
`glDrawArrays(GL_PATCHES, firstVertex, vertexCount)` (per §6.3 / §6.5).

### 7.5 Q-B5 — grass pass and the extras-VBO policy

**Answer:** **Legacy reader through M0b.** Grass binds
`terrain_extra_vb_` directly, so M0b must keep that buffer populated.

**Evidence:** `gosRenderer::drawGrassPass` at
`GameOS/gameos/gameos_graphics.cpp:2847–2953`, specifically lines
2927–2942, binds `terrain_extra_vb_` directly via
`glBindBuffer(GL_ARRAY_BUFFER, terrain_extra_vb_)` and reads
`worldPos`/`worldNorm` via manual `glVertexAttribPointer`.

**Extras-VBO policy for M0b — Option A (chosen):**

The modern terrain draw uses its own **persistent-mapped extras ring**
(separate from `terrain_extra_vb_`) bound at locations 4–5 for its
own draws. This is what the new patch stream owns.

`terrain_extra_vb_` (the legacy buffer) **stays alive** in M0b. It
is populated by **one consolidated per-frame `updateBuffer` of the
concatenated extras** issued at the start of `Render.TerrainSolid`
when modern mode is on, sourced from `MC_VertexArrayNode::extras` (which
`fillTerrainExtra` continues to populate, see §3 / §10 BR3). This
replaces the per-batch `glBufferData(terrain_extra_vb_, …)` that
`terrainDrawIndexedPatches` issued — N uploads collapse to 1.

`Shadow.StaticAccum` continues to issue its own per-batch
`updateBuffer(terrain_extra_vb_, …)` when the camera-move threshold
fires (`gameos_graphics.cpp:~2517–2528`). After the shadow pass
finishes, the buffer's contents reflect shadow's last batch — the
modern path's start-of-frame consolidated upload restores grass-correct
contents on the next frame. The order is: legacy `fillTerrainExtra`
appends → modern `Render.TerrainSolid` consolidates and uploads
`terrain_extra_vb_` once → modern flush draws → grass draws (reads
the consolidated upload) → shadow accumulation may overwrite at end of
frame.

**Why not Option B (modern draw uses `terrain_extra_vb_` directly)?**
Because `terrain_extra_vb_` is a transient `GL_DYNAMIC_DRAW` buffer
that gets `glBufferData`-orphaned by shadow and (today) by
`terrainDrawIndexedPatches`. To get the upload-elimination win,
the modern draw must bind a **persistent-mapped** ring of its own.
Option A keeps the legacy buffer as a small one-shot per-frame
upload — the cheapest way to keep grass fed without redesigning grass.

**Grass deprecation framing:** Grass is **not** a modernization
target. M0b preserves `terrain_extra_vb_` purely because keeping it
fed via one consolidated upload is cheaper than redesigning grass.
A future Shape B' may either migrate grass to read from the modern
extras ring or disable grass under modern terrain mode entirely;
either is acceptable, neither is in M0b's scope.

---

## 8. Killswitch spec

### 8.1 Env variable

```
MC2_MODERN_TERRAIN_SURFACE = 0 | 1   (default 0)
```

- `0` (**default through M0b validation**): legacy
  `terrainDrawIndexedPatches` path. Zero behavior change.
- `1`: persistent-mapped patch-stream path. `Render.TerrainSolid`
  dispatches `TerrainPatchStream::flush()`. Legacy ring writes
  (`addVertices` + `fillTerrainExtra`) still happen for shadow + grass.

Read once at engine init into a `static const bool` flag stored in
`gos_terrain_patch_stream.cpp`. No runtime re-read.

**Default-on policy:** M0b ships with `default = 0`. Default flips
to `1` only after: (a) Tracy parity at killswitch=0 confirmed
across tier1, (b) all §11 success-threshold criteria met at
killswitch=1 across tier1 + Magic canary + Wolfman, (c) at least
one full release cycle running default-off. Default flip is a
follow-on PR that touches one literal — no redesign.

### 8.2 RAlt+Shift+T — deferred to a later slice

**Live mid-frame toggle is not safe in M0b.** Reasons:
- The persistent rings and their fences are allocated once at engine
  init. A live toggle from off → on would require triggering the
  init path mid-frame; from on → off would orphan the in-flight
  fence slot while the legacy path continues to consume `vertices`.
- Even though M0b uses unchanged shaders, the two paths bind buffers
  differently (`glBufferData`-orphan VBOs vs. persistent VBO ring),
  and switching mid-frame would risk partial-frame inconsistency.

**Decision:** env-only for M0b. A frame-boundary toggle (re-evaluate
the flag at the start of `gosRenderer::beginFrame`, drain all fences
when transitioning) can be added in a follow-on slice once the
patch-stream infrastructure is proven. A killswitch is mandatory; a
live hotkey is not.

### 8.3 Failure modes

If `glBufferStorage` or `glMapBufferRange` fails at init:
1. Log `[PATCH_STREAM v1] event=init_fail reason=…`.
2. Force `MC2_MODERN_TERRAIN_SURFACE=0` for the remainder of the
   process and proceed on the legacy path.

If overflow is detected mid-frame (BR6 — bucket would write past
the active slot's capacity):
1. Log `[PATCH_STREAM v1] event=overflow slot=N peak=… cap=…`.
2. **Whole-frame fallback to legacy.** The current frame's modern
   flush is aborted before any modern draw is issued, and
   `Render.TerrainSolid` falls back to the legacy
   `terrainDrawIndexedPatches` path **for this frame only**, using
   the `MC_VertexArrayNode::vertices` and `::extras` data that the
   legacy path is still populating in M0b (per §3, §9). This means
   no terrain holes, no stale-frame regions — the frame renders
   exactly as if the killswitch were off.
3. On next frame, `ensureRingCapacity` grows the ring (drains all
   fences, resizes, remaps) and modern mode resumes.

The whole-frame fallback is safe in M0b precisely *because* the
duplication caveat (§9) keeps the legacy rings populated. This is
why M0b is feasible at all as a default-off-then-default-on rollout:
the legacy path is always one branch away.

This honors `stock_install_must_remain_playable.md`: missing or
overflowing modern terrain data degrades to legacy, never crashes,
never requires generated sidecar assets.

---

## 9. Duplication caveat — verdict

**M0b is an infrastructure slice, not an immediate net upload win on
the CPU side.** The wins are concentrated in (a) eliminating the
color-VBO `glBufferData` upload via `terrainDrawIndexedPatches` and
(b) reducing per-batch GL state submission overhead.

### 9.1 Per-frame writes during M0b at Wolfman

| Stream | Bytes/frame | Lives in M0b? | Why |
|---|---:|---|---|
| `addVertices` → `MC_VertexArrayNode::vertices` (CPU memcpy) | ~7.6 MB | **Yes (kept)** | `Shadow.StaticAccum` reads it (§7.3) |
| `fillTerrainExtra` → `MC_VertexArrayNode::extras` (CPU memcpy) | ~5.7 MB | **Yes (kept)** | Shadow + grass read it |
| `terrainDrawIndexedPatches` → color VBO `glBufferData` (per batch) | ~7.6 MB | **No (eliminated)** | Replaced by persistent-VBO writes via mapped pointer |
| `terrainDrawIndexedPatches` → extras VBO `glBufferData` (per batch) | ~5.7 MB | **No (collapsed)** | Replaced by **one** per-frame consolidated `updateBuffer(terrain_extra_vb_)` for grass (§7.5 Option A) |
| New: PatchStream color ring append (CPU writes via mapped pointer) | ~7.6 MB | **Yes (added)** | The new path |
| New: PatchStream extras ring append (CPU writes via mapped pointer) | ~5.7 MB | **Yes (added)** | The new path's worldPos/worldNorm at locs 4–5 |
| New: one-shot consolidated `updateBuffer(terrain_extra_vb_)` for grass | ~5.7 MB | **Yes (added, single upload)** | Keeps grass fed without per-batch upload |

### 9.2 Net delta in M0b (Wolfman, all numbers approximate)

- **CPU memcpy bytes:** legacy color ring ~7.6 MB + legacy extras
  ring ~5.7 MB (both retained for shadow) + modern color ring append
  ~7.6 MB + modern extras ring append ~5.7 MB ≈ **~26.6 MB/frame**,
  vs. ~13 MB/frame today. **CPU writes go up by ~13.6 MB/frame**
  during the duplication window. (B' will recover this when
  shadow/grass migrate off the legacy rings.)
- **GL upload bytes:** ~7.6 MB color VBO `glBufferData` per-batch
  **eliminated**. ~5.7 MB extras VBO per-batch `glBufferData`
  **collapsed** to a single per-frame `updateBuffer` (~5.7 MB once,
  not N times). Persistent-mapped VBO writes do not go through the
  driver upload path (coherent map). **Net GL upload at Wolfman:
  from ~13 MB/frame (~6–10 batches × ~13.3 MB-batch-equivalent
  duplication, i.e. all per-batch uploads) down to ~5.7 MB/frame
  (one consolidated extras upload).**
- **Submission overhead in `Terrain.DrawPatches`:** decreases — fewer
  state changes per material bucket, no per-batch `glBufferData`,
  no per-draw `glGetAttribLocation` if the §12.5 #1 fix is taken
  alongside, uniforms still re-set per draw (AMD rule line 11).
  Estimated saving: most of the residual 0.51 ms/frame.

### 9.3 Which stream removals are safe in M0b

**Safe to remove in M0b:**
- The `mesh->uploadBuffers()` color-VBO upload inside
  `terrainDrawIndexedPatches` for the modern-mode invocation path.
  This is achieved by simply not calling `terrainDrawIndexedPatches`
  when the killswitch is on.

**NOT safe to remove in M0b (move to Shape B'):**
- `addVertices` / `MC_VertexArrayNode::vertices` writes — Shadow.StaticAccum
  consumer (§7.3).
- `fillTerrainExtra` / `MC_VertexArrayNode::extras` writes — shadow +
  grass consumers.
- `terrain_extra_vb_` upload — grass pass consumer (§7.5).
- `addTriangle` numVertices counter — extras lazy-alloc depends on it.

**Shape B' (post-M0b) tasks to convert this into a full performance
win:**
1. Migrate `Shadow.StaticAccum` to read from the same persistent
   extras ring via a depth-only TES variant (Q-B3 follow-up).
2. Migrate the grass pass to read `worldPos`/`worldNorm` from the
   persistent extras ring instead of `terrain_extra_vb_` (Q-B5
   follow-up); or disable grass under modern terrain mode.
3. Once both consumers are migrated, gate `addVertices` and
   `fillTerrainExtra` off in modern mode. CPU memcpy delta becomes
   net negative.

**Verdict:** Treat M0b as enabling/infrastructure. Set the success
threshold (§11) accordingly: a small Tracy improvement and zero
visual regression is a pass; the headline wins arrive in Shape B'.

---

## 10. Risk register (BR1–BR8) — updated with audit evidence

| # | Risk | Verified evidence | Mitigation |
|---|---|---|---|
| BR1 | AMD GL state regression — persistent-mapped VBO bind, `GL_ARRAY_BUFFER`/`GL_ELEMENT_ARRAY_BUFFER` rebind across slot rotation, fence-wait, attrib-pointer reissue | `docs/amd-driver-rules.md` lines 9, 10, 11, 13 are the only rules touching state. None forbid persistent-mapped VBO. State save/restore reference: `gos_static_prop_batcher.cpp:692–712, 817–834` (same flag set, different bind target) | Save/restore the same set of GL state as the static-prop batcher, plus `GL_ARRAY_BUFFER_BINDING`. AMD smoke run before merge |
| BR2 | GBuffer1 contract violated by modern FS | **DROPPED for M0b.** Mechanism revision (rev 2) keeps the existing shaders unchanged. Re-evaluate at B' if shadow path migrates to modern buffer. Helpers at `shaders/include/render_contract.hglsl:20–65` documented for the future migration |
| BR3 | Shadow re-feed mismatch | Static-shadow audit confirms graceful-skip semantics: null `vertices` or null `extras` skips the node, `extraCount==0` skips before dispatch (`txmmgr.cpp:1229/1230/1242`). The shader reads only positions from `vertices`, all six floats from `extras` | M0b keeps both legacy rings populated by `addVertices` + `fillTerrainExtra`, **including pointer-delta advance** (the load-bearing invariant). Shape B's modern stream does not touch those pointers. B' migration risk re-evaluated when shadow shader is moved off the legacy bind |
| BR4 | `pz` gate bypassed when migrating SOLID branch | Append callsites at `mclib/quad.cpp:1608, 1752, 1911, 2053` are all inside the existing `pz` gate brace | Append must be added inside the existing brace, not after it. Code review checklist + smoke RTS-zoom check (red-bands canary) |
| BR5 | Cull cascade disturbed if modern path skips `addTriangle` | `addTriangle` (`mclib/txmmgr.h:689–822`) is a pure counter; `addTerrainExtra` lazy-alloc (`txmmgr.h:1085–1109`) reads `numVertices`. No mech/building consumer found | Skipping `addTriangle` is safe **only** when `fillTerrainExtra` is also gated off (Shape B'). Keep `addTriangle` in M0b |
| BR6 | Wolfman patch-stream size insufficient | Worst-case = 237,606 verts × 32 B ≈ 7.6 MB/slot color, 5.7 MB/slot extras; size to 9.5 MB and 7.1 MB respectively with 25 % headroom (§5.4) | Allocate per §5.4. Add `[PATCH_STREAM v1] event=overflow` from day one. **On overflow: whole-frame fallback to legacy** (§8.3) — never partial-frame holes. Grow ring on next frame |
| BR7 | Grass pass breaks if `terrain_extra_vb_` content changes | Grass binds `terrain_extra_vb_` directly at `GameOS/gameos/gameos_graphics.cpp:2927–2942` | Modern path does **not** rewrite `terrain_extra_vb_`. Extras stay populated by legacy path. Verification: RAlt+5 grass-visibility A/B |
| BR8 | Stale-shader-cache mimic | **DROPPED for M0b.** Mechanism revision (rev 2) ships no new shaders. Smoke procedure still clears cache as a precaution but does not depend on it for parity |

---

## 11. Success threshold

**Comparison baseline:** Shape A landed with ~0.85 ms/frame net saving
at Wolfman. M0b is held to a **lower** bar because most of its
benefit is deferred to Shape B' (§9). A pass means **all** of:

1. **Tracy delta:** `Terrain.DrawPatches` reduction of at least
   0.20 ms/frame at Wolfman (residual is 0.51 ms/frame today; target
   0.30 ms/frame or lower). `Render.TerrainSolid` overall ≤ today.
   No regression in `Terrain::geometry`.
2. **GL upload delta:** `glBufferData` from `terrainDrawIndexedPatches`
   reduced to 0 in modern mode (one-time `terrain_extra_vb_` upload
   per frame is acceptable). Verify with a renderdoc / apitrace
   capture or by counting calls to `updateBuffer` from
   `terrainDrawIndexedPatches` in modern mode.
3. **Visual A/B:** zero visual diff vs. legacy at:
   - Wolfman (`visibleVerticesPerSide=200`)
   - default RTS zoom
   - mc2_01 stock first mission, opening cinematic position
4. **No new GL errors** (`[GL_ERROR v1]` channel silent under
   modern mode).
5. **`MC2_MODERN_TERRAIN_SURFACE=0` parity:** legacy capture shows
   no Tracy or visual change vs. pre-Shape-B baseline.

A *win* (§9) is bigger numbers; M0b is allowed to land on infrastructure
correctness alone, with B' tracked as the headline performance follow-up.

---

## 12. Verification gates

Pre-merge smoke must pass each gate in order:

1. **Shader-cache clear (optional, defensive).** M0b ships **no
   shader changes**, so `stale_shader_cache_symptom.md` does not
   apply structurally. A precautionary clear is fine but not required
   for parity; if a visual diff appears at killswitch=0 vs.
   pre-PR, suspect the C++ binding/upload path before the shader
   cache.
2. **`MC2_MODERN_TERRAIN_SURFACE=0` legacy parity run.** Capture Tracy
   + screenshot diff vs. pre-PR baseline. Both must be no-op.
3. **`MC2_MODERN_TERRAIN_SURFACE=1` stock-install run.** Run on
   stock mc2-win64-v0.2 install with no modern sidecar assets, no
   regenerated cache. `stock_install_must_remain_playable.md` —
   missing modern data must degrade gracefully (the killswitch's
   init-fail path must trigger if any glBufferStorage/Map call
   returns error; legacy path takes over).
4. **Magic / mod-content canary.** `magic_abl_contamination_rule.md`
   not directly affected, but the canary mission still runs to catch
   any indirect regression.
5. **Static-shadow A/B (RAlt+F3 toggle).** Because BR3 and §7.3 keep
   the legacy ring writes, static shadows must look identical before
   and after.
6. **Grass visibility A/B (RAlt+5 toggle).** BR7 — confirm grass
   density and worldPos placement are unchanged. This is the gate
   that catches a broken Option-A consolidated-extras upload (§7.5).
7. **Pool-headroom regression check.** `[TGL_POOL v1] event=mission_unload`
   peaks must remain in the same comfortable range (§2). The
   persistent VBO rings live in driver-managed memory and do not
   consume TGL pools, but a regression here would indicate a CPU
   ring-write blow-up or accidental double-allocation in the
   duplication window.
8. **`[PATCH_STREAM v1]` lifecycle prints present.** init,
   first-flush, draw_count (per-frame during validation),
   bucket_split (if indexed fallback is used), overflow,
   init_fail, shutdown. `debug_instrumentation_rule.md` — these
   prints land in the same commit.
9. **Overflow-fallback verification.** Force an overflow in a
   debug build (e.g. shrink slot capacity to 1 KB) and confirm
   `[PATCH_STREAM v1] event=overflow` fires + the frame renders
   correctly via the legacy fallback path (§8.3). Visual diff
   at the overflow frame must be zero compared to killswitch=0.
10. **AMD canary.** RX 7900 XTX run end-to-end, including a
    static-shadow toggle and a grass toggle.

---

## 12.5 Out-of-scope wins surfaced by the audits

The state-binding inventory and shader-input audit surfaced three
issues that are independent of PatchStream and worth tracking
separately. **None block Shape B.** Listed here so they aren't lost.

1. **`glGetAttribLocation` per-draw stall**
   (`GameOS/gameos/gameos_graphics.cpp:2797–2798`). Two synchronous
   driver queries fire inside `terrainDrawIndexedPatches` every
   invocation — at 5–40 terrain draws/frame, that's 10–80 driver
   round-trips per frame for attribute lookup. Cache the locations
   once at material init. Independent of Shape B; trivial fix.

2. **`matNormal0–4` redundant per-node rebind**. The five material
   normal samplers on units 5–9 are static across the whole frame
   but rebound inside every per-tile draw. Hoisting these binds
   outside the per-node loop saves N-1 redundant `glActiveTexture` +
   `glBindTexture` pairs per frame. Independent of Shape B.

3. **Unit-9 collision: `matNormal4` ↔ `shadowMap`** (pre-existing
   bug). The C++ binding side uploads `matNormal4` (snow) to unit 9,
   then `shadowMap` to unit 9 later in the same frame; the shadow
   bind wins. This means the snow biome's per-material normal map
   silently aliases the shadow depth map whenever both are active.
   Worth a separate fix — move `shadowMap` to a free unit (e.g. 11)
   or move `matNormal4` to a free unit. Not Shape B's job, but a
   reviewer who walks the unit assignments will spot it.

4. **`sampler2DArray` colormap fork** — **active, gated on Canary B.**
   Canary A (synthetic 4×4×4 RGBA8 probe) PASSED on AMD RX 7900 XTX —
   see [2026-04-27-amd-sampler2darray-canary.md](../explorations/2026-04-27-amd-sampler2darray-canary.md).
   `gl_err=0x0`, `max_error=0`, all four layers sampled exactly. The
   `docs/amd-driver-rules.md` line 13 ban is **refuted for the
   synthetic case**. Canary B (real terrain colormap copied to layer 0,
   visual A/B vs. legacy `sampler2D` path) is the conservative next
   step before re-architecting. If Canary B also passes, a B-array
   PatchStream variant unlocks single-draw (or single-digit-draw)
   terrain. **M0b stays as designed** — changing mechanism this late
   would re-open closed questions. The B-array variant lands on top
   of M0b as a separate slice. To ease that future, M0b's per-vertex
   record may optionally include a 1-byte material-slot field now
   (e.g., reusing `gos_VERTEX.frgb`'s spare byte) — non-blocking,
   forward-compatible.

---

## 13. Implementation-readiness verdict

**Ready for implementation plan: Yes.**

All open questions Q-B1 through Q-B5 (and Q-B1a) are resolved against
verified file:line evidence, and three audit cycles (shader-input,
state-binding inventory, static-shadow dependency) have refined the
mechanism choice. The buffer mechanism is **persistent-mapped VBO**
bound as `GL_ARRAY_BUFFER` with `GL_MAP_PERSISTENT_BIT |
GL_MAP_COHERENT_BIT`, mirroring the static-prop batcher's flag set
but at the existing terrain attribute layout — **no shader changes**,
no SSBO introduction, no `gl_VertexID`-in-tessellation complications.
Draw count stays one-per-tile (~6–10 at Wolfman) because
`sampler2DArray` is AMD-banned; that is structural and not a Shape B
deliverable.

The duplication caveat is reframed as an architectural fact (§9):
M0b is infrastructure, not a CPU-write win. The headline win is
elimination of ~7.6 MB/frame `glBufferData` upload churn for the
color VBO and reduction of per-batch GL state submission overhead
(target: residual `Terrain.DrawPatches` from 0.51 ms → ≤ 0.30 ms).
Shape B' converts the infrastructure into the full CPU-write win
once shadow + grass migrate off the legacy rings.

Pointer-delta fill-count invariant (rev 3) is now an explicit row
in §3 so the implementation plan can guard it.

The next step is a `superpowers:writing-plans` session that turns
§4 into ordered TDD tasks with the cited file:line anchors as the
edit targets.
