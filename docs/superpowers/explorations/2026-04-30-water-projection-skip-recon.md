# Water vertex projection skip — recon findings (2026-04-30)

> **⚠️ SUPERSEDED 2026-05-02 — Section C is INCORRECT on a load-bearing row.** Stage 0 M3
> audit (executing-plans pre-Stage-1 stop gate) discovered that
> `gos_terrain_water_stream.cpp::UploadAndBindThinRecords()` (line ~370) reads
> `q.waterHandle` (used as per-frame inclusion gate) AND `q.vertices[i]->wz` (used for
> per-triangle pz validity). Section C's claim "Read by fast-path: No" for
> `vertices[i]->wx/wy/wz/ww` is wrong — it audited only `drawWater()` consumers
> without grep'ing the fast-path stream. The legacy projection block IS the CPU
> pre-cull for water rendering (per `gpu_direct_renderer_bringup_checklist.md` trap #7).
> The slice's "stranded upstream" premise is invalidated. Brainstorm at
> `brainstorms/2026-05-01-water-projection-skip-scope.md` is superseded; Tracy hygiene
> bundle queued as consolation deliverable. **Process lesson:** data-flow audits are
> asymmetric — proving "X is NOT consumed by Y" requires grep'ing Y for X reads,
> not just grep'ing the obvious-named consumer (drawWater) for X.

> **Status:** SUPERSEDED. (Was: COMPLETE re-recon, second pass.)
>
> **Scope:** stock missions only (`memory/feedback_offload_scope_stock_only.md`).
>
> **What is being recon'd:** the renderWater architectural slice (shipped 2026-04-30) retired the water DRAW via a post-`renderLists()` fast path. The legacy water CPU projection in `mclib/quad.cpp` still runs every frame even when `MC2_RENDER_WATER_FASTPATH=1`. Its output (`addTriangleBulk(waterHandle, …DRAWALPHA|ISWATER, 2)` reservation + per-vertex `wx/wy/wz/ww` writes consumed by `drawWater()`) is wasted because the post-renderLists fast path overrides the water draw and `drawWater()` is never called. This is the pattern *"fast-path with stranded upstream."*

---

## Verified facts (preserved from the first recon agent's pre-stall work)

### Existing renderWater "armed" check is an inline compound predicate

**File:line:** `mclib/terrain.cpp:1048-1051`

**Predicate:**
```cpp
if (s_fastPath
    && WaterStream::IsReady()
    && WaterStream::GetRecipeCount() > 0
    && Terrain::terrainTextures2 != nullptr)
```

**Implication for this slice:** there is NO `WaterStream::IsArmed()` helper today. The water-projection-skip slice's gate must mirror this exact compound predicate — either by hoisting it into a new `IsArmed()` helper at design time, or by inlining the same compound check at the new gate site.

**Cross-reference:** the indirect-terrain plan v2 brief proposes an analogous preflight-armed pattern (`terrainIndirectSolidArmed = IsEnabled() && isDenseRecipeReady() && resourcesReady() && !inMissionTransition()`). Same shape, distinct inputs. Both could share a "preflight pattern" doc once both ship; they are NOT the same predicate.

### Second projection site at `terrain.cpp:~1615` — characterized in Section B

The first agent flagged `terrain.cpp:~1615` without finishing the trace. Section B confirms it is INSIDE the legacy `Terrain::geometry` `vertexProjectLoop` — a `screenPos.z < leastZ` accumulator update for **terrain (not water) vertices**. NOT a sibling water projection. NOT a slice target.

---

## Section A — Cited code locations (verified independently)

User's stub cited line ranges that were approximate. Actual locations:

### A.1 Water projection block start

**Stub said:** `mclib/quad.cpp:715-770`.
**Actual:** the water projection block begins at **`mclib/quad.cpp:773`** with the comment `//-----------------------------------------------------\n\t// NEW(tm) water texture code here.` and runs through `mclib/quad.cpp:1100`.

The block is gated by an outer:
```cpp
if ((vertices[0]->pVertex->water & 1) ||
    (vertices[1]->pVertex->water & 1) ||
    (vertices[2]->pVertex->water & 1) ||
    (vertices[3]->pVertex->water & 1))
```
at `quad.cpp:775-778`. Quads with no water bit set on any corner skip the block entirely (they hit the `else` at 1096-1100 which sentinels `waterHandle`/`waterDetailHandle`).

The `setupTextures water vertex projection` Tracy zone is at `quad.cpp:780`.

The four per-corner projection sub-blocks are at `quad.cpp:795-861` (vert0), `863-931` (vert1), `933-1001` (vert2), `1003-1071` (vert3).

### A.2 wAlpha displacement

**Stub said:** `mclib/quad.cpp:743-755`.
**Actual:** wAlpha writes are scattered across the four per-corner sub-blocks at **`quad.cpp:801, 806, 869, 874, 939, 944, 1009, 1014`** (eight writes total; two per corner). Each sits inside a `if (vertices[i]->pVertex->water & 128)` / `else if (… & 64)` ladder.

Pattern (vert 0 at quad.cpp:799-808):
```cpp
if (vertices[0]->pVertex->water & 128)
{
    vertices[0]->wAlpha = -Terrain::frameCosAlpha;
    ourCos = negCos;
}
else if (vertices[0]->pVertex->water & 64)
{
    vertices[0]->wAlpha = Terrain::frameCosAlpha;
    ourCos = Terrain::frameCos;
}
```

Section D analyses cross-effects of these eight writes.

### A.3 `addTriangleBulk(waterHandle, …)` and `addTriangleBulk(waterDetailHandle, …)`

**Stub said:** `mclib/quad.cpp:1018-1019`.
**Actual:** the two bucket-reservation calls are at **`mclib/quad.cpp:1087-1088`**:
```cpp
mcTextureManager->addTriangleBulk(waterHandle, MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER, 2);
mcTextureManager->addTriangleBulk(waterDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL, 2);
```

These are inside `if (clipped1 || clipped2)` (line 1073) — i.e. they only register when at least one of the two triangles has a non-zero `clipped*` bit-sum. The `else` branch at 1090-1094 sentinels `waterHandle`/`waterDetailHandle = 0xffffffff`.

`addTriangleBulk` itself (`mclib/txmmgr.h:830`) only RESERVES bucket slots and bumps `numVertices` by `3 * triCount`. It does NOT push vertex data. The actual vertex push happens later in `TerrainQuad::drawWater()` (`mclib/quad.cpp:2834`) via `addVertices(waterHandle, …)` calls at `quad.cpp:2998, 3138, 3287, 3426` (and `addVertices(waterDetailHandle, …)` at `3022, 3162, 3311, 3450`) — but only when `Terrain::renderWater()` calls `currentQuad->drawWater()` at `terrain.cpp:1078`, which the fast path early-returns and skips entirely at `terrain.cpp:1054`.

---

## Section B — Second projection site at `terrain.cpp:~1615` (CHARACTERIZED)

**Verdict: NOT a sibling water projection. NOT a slice target.**

Reading `mclib/terrain.cpp:1411-1474` (the fast-path D1 hoist branch) and `mclib/terrain.cpp:1481-1641` (the legacy branch), `terrain.cpp:1615` falls inside the **legacy `Terrain::geometry` `vertexProjectLoop`** — specifically inside the per-vertex `if (currentVertex->clipInfo) { … if (inView) { … } }` block (legacy `for` loop at `terrain.cpp:1482`, `currentVertex` iterating `vertexList`).

What `terrain.cpp:1611-1636` does:
```cpp
if (currentVertex->clipInfo)
{
    setObjBlockActive(currentVertex->getBlockNumber(), true);
    setObjVertexActive(currentVertex->vertexNum,true);
    if (inView)
    {
        if (screenPos.z < leastZ) leastZ = screenPos.z;
        if (screenPos.z > mostZ)  mostZ  = screenPos.z;
        if (screenPos.w < leastW) { leastW = screenPos.w; leastWY = screenPos.y; }
        if (screenPos.w > mostW)  { mostW  = screenPos.w; mostWY  = screenPos.y; }
    }
}
```

This projects **terrain vertices** (using `currentVertex->vx, vy, pVertex->elevation` — terrain elevation, not water elevation), accumulates `leastZ/mostZ/leastW/mostW` for `eye->setInverseProject()`, and sets the cull cascade `objBlockInfo[].active` / `objVertexActive[]` flags.

This is the **exact loop targeted by the vertexProjectLoop D1 slice** (closed asymptotic 2026-04-30). It is unrelated to water — wrong projection plane (terrain elevation, not `Terrain::waterElevation`), wrong loop (per-vertex, not per-quad), wrong consumer (cull cascade + camera inverse-project, not `drawWater`'s `wx/wy/wz/ww`). The water projection block in `quad.cpp:773-1100` ALSO writes `leastZ/mostZ/leastW/mostW` (Section D) but does so via the per-quad `setupTextures` walk, not the per-vertex `vertexList` walk.

**Implication:** the water-projection-skip slice does NOT need to gate `terrain.cpp:1615`. It is owned by the (already-shipped) vertexProjectLoop slice and projects terrain, not water.

---

## Section C — Stranded-upstream confirmation (post-renderWater-default-on output flow)

**Claim:** the entire output of the `quad.cpp:773-1100` water projection block is wasted when `MC2_RENDER_WATER_FASTPATH=1` and the armed predicate evaluates true.

**Outputs produced by the block:**

| Field written | Site | Read by (legacy) | Read by (fast-path)? |
|---|---|---|---|
| `vertices[i]->wx/wy/wz/ww` | `quad.cpp:829-1042` | `drawWater()` at `quad.cpp:2883-3333` | **No** — `drawWater()` not called |
| `vertices[i]->clipInfo` | `quad.cpp:825-1037` | own block at `quad.cpp:784-790`; `drawWater()` clipped check at `quad.cpp:3463-3989` | **No** for `drawWater` consumers; **yes (mutated)** for the block's own `clipped*` test next frame |
| `vertices[i]->calcThisFrame |= 2` | `quad.cpp:834-1044` | own block's `if (!(vertices[i]->calcThisFrame & 2))` at `quad.cpp:795/863/933/1003` | **Self-consumed only** — bit 2 is water-exclusive (terrain uses bit 1 at `quad.cpp:1111-1723`) |
| `vertices[i]->wAlpha` | `quad.cpp:801, 806, 869, 874, 939, 944, 1009, 1014` | **NOWHERE** in the active codebase (Section D) | n/a |
| `vertices[i]->hazeFactor = 1.0f` (off-map) | `quad.cpp:821, 891, 961, 1031` | terrain shaders / fog calc (TBD if water-projection-specific) | TBD |
| `leastZ/mostZ/leastW/mostW/leastWY/mostWY` accumulators | `quad.cpp:838-858, 906-928, 976-998, 1046-1068` | `eye->setInverseProject()` at `terrain.cpp:1732` | **Still consumed** — fast-path does not replace this; mouse-pick depth uses these |
| `waterHandle = …getWaterTextureHandle()` | `quad.cpp:1083-1085` | `drawWater()` `if (waterHandle != 0xffffffff)` | **No** — drawWater not called |
| `waterDetailHandle = …getWaterDetailHandle(…)` | `quad.cpp:1084` | `drawWater()` | **No** |
| `addTriangleBulk(waterHandle, …, 2)` | `quad.cpp:1087` | `renderLists()` flush at `txmmgr.cpp:1494-1503` (water-bucket loop at 1466-1530) | **Yes — but render is no-op** (Section C.2) |
| `addTriangleBulk(waterDetailHandle, …, 2)` | `quad.cpp:1088` | same | same |

### C.1 `renderLists()` flush flow for water buckets

The water flush is at `mclib/txmmgr.cpp:1466-1530` (inside `Render.Overlays`, `gos_State_AlphaMode = gos_Alpha_AlphaInvAlpha`). The loop iterates `nextAvailableVertexNode` and matches:
```cpp
if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
    !(masterVertexNodes[i].flags & MC2_ISCRATERS) &&
    (masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
    (masterVertexNodes[i].vertices))
```
then guards `if (!(MC2_ISWATER || MC2_ISWATERDETAIL))` continue (line 1481). Water buckets pass the guard, set `gos_State_Water` mode (1487-1492), and call `gos_RenderIndexedArray` on `totalVertices = currentVertex - vertices` (line 1497).

### C.2 Stranded-upstream behavior with fast path on

- `addTriangleBulk` at `quad.cpp:1087-1088` reserves bucket slots (allocates `masterVertexNodes[]` slot, sets `flags`, bumps `numVertices += 6`).
- `drawWater()` is never called → `addVertices(waterHandle, …)` never runs → `currentVertex == vertices` (start of array).
- `renderLists()` reaches the water bucket, computes `totalVertices = currentVertex - vertices = 0` (the underfill detection at `txmmgr.cpp:1495-1498` triggers).
- `if (totalVertices && (totalVertices < MAX_SENDDOWN))` at `txmmgr.cpp:1500` fails (totalVertices == 0) → `gos_RenderIndexedArray` NOT called → no draw.

**Net:** the post-renderLists fast-path FULLY overrides the legacy water draw. The water bucket is reserved but never filled. `renderLists()` doesn't issue a draw call for it. There is NO partial-overlap (no double-rendering, no flicker).

**Caveats / leftover work the fast path's bridge already handles:**
- The fast path's bridge in `gameos_graphics.cpp` (`gos_terrain_bridge_renderWaterFast`) explicitly sets `gos_State_Water` mode for its own draw (per `renderwater_fastpath_stage2.md` §Architecture). The legacy `gos_State_Water` setpoint at `txmmgr.cpp:1487-1492` is dead code in the fast-path-armed case (the loop body still runs but the draw is no-op).
- Water bucket allocation does still consume `nextAvailableVertexNode` slots (one per `(handle, flags)` tuple); on stock missions this is two slots (waterHandle + waterDetailHandle) and is bounded by `MC_MAXTEXTURES`. Not a correctness hazard, but a minor resource waste worth noting.

### C.3 Wasted CPU is not just the projection — it's the per-quad `setupTextures` walk

The water projection block runs **per-quad** inside `setupTextures()` for every quad that has any water bit set. On a water-heavy mission (mc2_17), the walk dominates a meaningful slice of `Terrain::geometry quadSetupTextures`. The orchestrator status board cites this slice as "~0.96 ms / 20% of quadSetupTextures at high zoom"; this recon does not reproduce that measurement (Tracy run is design-stage work) but the architectural shape — every-frame, every-water-quad CPU work feeding a bucket whose flush is no-op — matches.

---

## Section D — wAlpha displacement cross-effects

### D.1 Reads of `wAlpha`

Grep `wAlpha` across the entire active worktree (`A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\`):

- **Writes:** 8 sites in `mclib/quad.cpp` (lines 801, 806, 869, 874, 939, 944, 1009, 1014).
- **Reads:** **zero**. Pattern grep `->wAlpha[^=]|\.wAlpha[^=]` returns only the same writes (with their `=` operator) and `.codex_tmp_isolate/` snapshot duplicates.
- **Declaration:** `mclib/vertex.h:95` — `float wAlpha; //Used to environment Map Sky onto water.`

The comment names a feature (env-mapped sky on water) that no consumer implements in this codebase. Possibly removed during the OpenGL port; possibly never wired up post-D3D7 retirement.

### D.2 `ourCos` side-channel in the same `if/else if` ladder

The eight `wAlpha = …` writes sit in `if (water & 128)` / `else if (water & 64)` ladders (e.g. `quad.cpp:799-808`):

```cpp
if (vertices[0]->pVertex->water & 128) {
    vertices[0]->wAlpha = -Terrain::frameCosAlpha;
    ourCos = negCos;
}
else if (vertices[0]->pVertex->water & 64) {
    vertices[0]->wAlpha = Terrain::frameCosAlpha;
    ourCos = Terrain::frameCos;
}
```

`ourCos` is a local float used at `quad.cpp:810` (`vertex3D.z = ourCos + Terrain::waterElevation;`). Per-corner the `wAlpha` write and the `ourCos` displacement come together. `ourCos` is NOT propagated outside `setupTextures` — it's used only to displace the projection-Z input within this same block.

### D.3 Implication for gate-off

**`wAlpha` is dead data.** Skipping the projection block (and therefore the eight `wAlpha` writes) loses zero observable behavior on the active GL path. No cross-effects.

The `ourCos` displacement is also load-bearing only inside the same block — its only consumer is the `vertex3D.z` set at lines 810/878/948/1018, which feeds the projection that we're skipping anyway.

**Caveat (must verify in design):** if a future env-map-water shader hooks back into `wAlpha` (e.g. sourcing it from the SSBO), the slice would need to mirror the writes into the WaterStream recipe or thin record. For TODAY's GL path, it is dead.

### D.4 `clipInfo` and `calcThisFrame` re-use are SELF-CONTAINED to the water block

`calcThisFrame & 2` is the water-projection bit (used as guard at `quad.cpp:795/863/933/1003`). Terrain projection uses `& 1` (set at `quad.cpp:1263, 1417, 1571, 1723`). Skipping the water block means bit 2 stays cleared — no other reader depends on it.

`clipInfo` writes inside the water block (`quad.cpp:825-1037`) **overwrite** the value the vertexProjectLoop set in `terrain.cpp:1454/1603/1606`. Materially, the legacy water-projection's `clipInfo` is derived from a **water-elevation-displaced** `vertex3D` (`vertex3D.z = ourCos + Terrain::waterElevation` at `quad.cpp:810/878/948/1018`), whereas the vertexProjectLoop's value is from `pVertex->elevation` (terrain). They are NOT the same value when terrain elevation ≠ water elevation (the typical case).

**Audit of `clipInfo` readers OUTSIDE the water block itself (when modern `terrainTextures2` path is active — which is the armed-fastpath precondition):**

- `quad.cpp:540-541, 624-625` — these are inside `if (!Terrain::terrainTextures2)` at `quad.cpp:536` (the legacy single-bitmap fallback path). **Unreachable** when `terrainTextures2 != nullptr`, which is exactly the armed-fastpath precondition.
- `quad.cpp:3463-3469` (`drawLine`), `3848-3854` (`drawLOSLine`), `3983-3989` (`drawDebugCellLine`) — debug-grid renderers, not gameplay; called from `terrain.cpp:990-1011` only when the corresponding debug toggles are on.
- `quad.cpp:240-246` is inside `pz_capture_quad_clipped` (PROJECTZ instrumentation, `pzv[i].legacyAccepted` at `quad.cpp:207`) — debug-only.

**Conclusion (Section D.4 refined):** in the modern `terrainTextures2` path that renderWater fast-path requires, NO non-debug code consumes the water-elevation-derived `clipInfo` from this block. The block's own `calcThisFrame & 2` re-entry guard makes the writes self-consumed. Skipping is safe with respect to `clipInfo`. The earlier draft's concern about cross-effects was over-broad; the modern-path constraint eliminates all gameplay readers.

### D.5 Accumulator contributions

`leastZ/mostZ/leastW/mostW/leastWY/mostWY` are written from the water block at `quad.cpp:838-858, 906-928, 976-998, 1046-1068`. These feed `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` at `terrain.cpp:1732`, which configures `Camera::inverseProjectZ()` for mouse-picking depth estimation.

If the water block is skipped, the accumulators would still be populated by the vertexProjectLoop (`terrain.cpp:1411-1474`), but **water-elevation extrema** are absent. On stock missions this is a small delta (water elevation is below or equal to terrain elevation; water-only quads rarely contribute the global min/max), but it is NOT zero.

**Design-stage requirement:** the slice must either (a) preserve the accumulator contribution by computing water-vertex z/w extrema cheaply (e.g. by hoisting min/max out of the projection chain), or (b) verify on stock missions that mouse-pick depth doesn't visibly degrade. Option (b) is plausible — the renderWater Stage 2 testing already exercised mouse-pick on water tiles (UI feedback) and didn't flag a regression — but it has not been formally measured.

---

## Section E — Slice scope estimate

Reference points from the orchestrator's Status Board (`docs/superpowers/cpu-to-gpu-offload-orchestrator.md`):

| Slice | Files touched | Architectural complexity |
|---|---|---|
| **M2d-overlay** (low end) | `mclib/quad.cpp` overlay-emit absorption, gate logic; one shipped commit (`258e584`) | Single hot-path branch, no new SSBO, no shader change |
| **renderWater Stage 1+2+3** (high end) | `gos_terrain_water_stream.{h,cpp}` (NEW), `gos_terrain_water_fast.vert` (NEW), `mclib/terrain.{h,cpp}`, `code/gamecam.cpp`, `mclib/mapdata.h`, `gameos_graphics.cpp` bridge, `gameosmain.cpp` instr banner — see `renderwater_fastpath_stage2.md` §Files | New SSBO schema, new VS, new bridge, parity check infra, three real-bug fixes |

This slice's expected scope:

- Touches: `mclib/quad.cpp:773-1100` (the water projection block — wrap in armed-check early return), `mclib/quad.cpp:1087-1088` (skip the bucket reservation, OR leave it but document the no-op flush), possibly factor the inline armed predicate into `WaterStream::IsArmed()` helper (`gos_terrain_water_stream.h`).
- No new SSBO. No new shader. No new bridge. No new parity check infra (existing `MC2_RENDER_WATER_PARITY_CHECK` exercises the fast path's recipe + thin record; the legacy-water-projection skip is a removal, not a fast-path addition).
- Possible NEW: a `MC2_WATER_PROJECTION_SKIP` env gate, defaulting to follow `MC2_RENDER_WATER_FASTPATH` (or a single env that arms both, to simplify env-flag matrix).

**Estimate: closer to M2d-overlay than renderWater Stage 2.** ~0.5× the latter's complexity. Single commit if `WaterStream::IsArmed()` extraction is folded in; two commits if armed-helper lands separately as a refactor precursor.

---

## Section F — Parity gate shape (options)

The challenge: the water projection block writes per-vertex `wx/wy/wz/ww` that are consumed only by `drawWater()`. With the fast path on, `drawWater()` is bypassed, so a "byte-compare drawWater output stream" parity check is moot — the fast-path doesn't produce that stream. A "byte-compare wx/wy/wz/ww after setupTextures" check IS possible but only validates that we are producing the same writes both ways — which is trivially equivalent to the question "did we run the same code?"

Three viable options:

### F.1 Option A — Skip-mode A/B canary on accumulators

Add `MC2_WATER_PROJECTION_SKIP_PARITY=1`. With both `MC2_RENDER_WATER_FASTPATH=1` AND projection skip armed, DO run the projection block but RECORD `leastZ/mostZ/leastW/mostW/leastWY/mostWY` deltas (skipped vs not-skipped) per frame. Print a `[WATER_PROJ_SKIP_PARITY v1] event=accumulator_delta` line when ratios exceed a threshold (e.g. 1% range change).

**Tradeoff:** narrow scope, validates the only post-skip externally-visible state (mouse-pick depth). Cheap to implement. Doesn't catch surface-pixel regressions if a future shader change reads `wAlpha`.

### F.2 Option B — Per-quad-vertex wx/wy/wz/ww snapshot, compare on-skipped to on-not-skipped

Run the projection block always, but in the skip-armed case ALSO snapshot `wx/wy/wz/ww/clipInfo/calcThisFrame|=2` into a per-vertex shadow buffer. After the frame, byte-compare against the values left from a baseline reference run (or vs a parallel non-skipped recompute). Mismatch printer + 600-frame summary, mirroring renderWater Stage 3.

**Tradeoff:** very thorough but contradicts the slice — running the block for parity defeats the perf gain. Useful only for one validation pass during development; can't ship as a default-off env that adds zero cost.

### F.3 Option C — No new parity check; rely on existing renderWater parity + visual canary

The water-projection-skip slice changes ONLY the upstream CPU work (which is already validated to be no-op for the fast-path's downstream draw, per Section C.2). The fast-path's own Stage 3 parity (`MC2_RENDER_WATER_PARITY_CHECK`) covers the GPU output. The only remaining risk is the accumulator delta (Section D.5), addressable via Option A as a side-channel diagnostic.

**Tradeoff:** lowest cost; relies on the renderWater Stage 3 parity to have been silent-on-pass, which is already proven for tier1 stock 5/5. Adds a "tier1 visual canary on inverse-project consumers" gate (mouse-pick on water tiles, terrain selection cursor) before declaring done. Risks a regression in `inverseProjectZ`-using code paths if the accumulator delta IS load-bearing (currently believed not, per Section D.5).

**Recommendation for design stage:** Option C as the default + Option A as an opt-in diagnostic for one development cycle, dropped before ship if accumulator deltas are consistently zero (means water-vertex extrema were never the global extrema on tier1).

---

## Section G — Load-bearing constraints checklist (cross-reference vs `gpu_direct_renderer_bringup_checklist.md`)

The bring-up checklist enumerates 9 traps. This slice is a **CPU-side removal**, not a new GPU-direct renderer, so most traps don't apply. Per-trap relevance:

| # | Trap | Relevance to this slice |
|---|---|---|
| 1 | `uniform uint` shader-builder crash | **N/A** — no shader change |
| 2 | Two-tier texture handle indirection | **N/A** — slice doesn't bind textures; reservation is removed |
| 3 | `terrainMVP` GL_FALSE vs `mvp/projection_` GL_TRUE | **N/A** — no matrix uniform upload |
| 4 | VAO 0 silent draw drop | **N/A** — no new draw call |
| 5 | Sampler inheritance | **N/A** — no new draw |
| 6 | Render order — must run AFTER `renderLists()` | **Partially relevant.** Skip happens BEFORE renderLists(). The fact that the legacy bucket reservation becomes harmless (Section C.2) is a consequence of the post-renderLists fast-path order; the slice's correctness inherits from that order. No new ordering work. |
| 7 | CPU pre-cull is THE frustum gate | **Indirectly relevant.** The water projection block's `clipInfo` writes feed the legacy land/detail clipped tests at `quad.cpp:540-541, 624-625, 3463-3989`. Skipping the block leaves `clipInfo` set by the terrain projection — which is the SAME field in legacy code, just with different writer. Worth validating in design that all readers tolerate this (Section D.4 calls it out). |
| 8 | Map-stable indexing for static SSBO recipe | **N/A** — no new SSBO; the existing WaterStream recipe is map-stable per renderWater Stage 2 |
| 9 | Depth-state inheritance for alpha-blended overlays | **N/A** — already handled by the renderWater fast-path bridge per `gpu_direct_depth_state_inheritance.md` |

**Additional load-bearing constraints from `MEMORY.md` ⭐ items:**

- **Stock-install playable** (`stock_install_must_remain_playable.md`): slice removes CPU work behind an env gate; default-off means stock install behaviour unchanged. Compliant.
- **Cull gates load-bearing** (`cull_gates_are_load_bearing.md`): the water block sets `clipInfo` (no `setObjBlockActive` / `setObjVertexActive` calls in the water block — those are vertexProjectLoop only). Skipping it does not bypass the cull cascade. Compliant.
- **TGL pool exhaustion silent** (`tgl_pool_exhaustion_is_silent.md`): water projection block doesn't pull from TGL pool. N/A.
- **MC2 texture handle is live** (`mc2_texture_handle_is_live.md`): the `waterHandle` set in the block is consumed only by `drawWater` (skipped) and the bucket reservation (now no-op). Skipping is safe. Compliant.
- **MC2 ARGB packing** (`mc2_argb_packing.md`): no ARGB writes in the projection block (those are in drawWater's gVertex emit, not the projection). N/A.
- **Visual preference knobs**: water alpha bands (`Terrain::alphaMiddle/alphaEdge/alphaDeep`) are computed in `drawWater` (`quad.cpp:2935-2965`), not the projection block. Skipping the projection doesn't touch them. Compliant.

---

## Section H — Open questions for the future brainstorm

1. **Inverse-project accumulator preservation.** Does mouse-pick depth on stock tier1 missions (mc2_01 / mc2_17 in particular — water-heavy) actually depend on water-vertex z/w extrema contributing to `leastZ/mostZ/leastW/mostW`? Or are terrain-vertex extrema always the global min/max? Section D.5 conjectures the latter; brainstorm should confirm via a lightweight one-frame instrumentation OR commit to Option A side-channel diagnostic for one development cycle.

2. **`WaterStream::IsArmed()` helper extraction — single slice or precursor refactor?** The compound predicate at `terrain.cpp:1048-1051` is also referenced in `terrain.cpp:1118-1123` (renderWaterFastPath own armed check). Hoisting into a single `IsArmed()` reduces the gate-site count from two to one (or three if the new skip site is added inline). Precursor commit improves readability; single commit reduces churn. Brainstorm should pick.

3. **Env flag matrix — single arm or separate skip flag?** Two designs:
   - (a) `MC2_RENDER_WATER_FASTPATH=1` arms BOTH the fast-path draw AND the upstream skip. Simpler to set, but cannot test fast-path-only without skip (used to bisect).
   - (b) New `MC2_WATER_PROJECTION_SKIP=1` separate from `MC2_RENDER_WATER_FASTPATH=1`. Skip is gated by `(MC2_WATER_PROJECTION_SKIP || (default_skip && fastpath_armed))`. Allows isolating perf delta of skip vs full fast-path. More env knobs.

4. ~~**`clipInfo` overwrite delta — is there a legacy-land path where the water-projection-derived `clipInfo` value is materially different from the terrain-projection-derived value?**~~ **RESOLVED in Section D.4 audit:** the only non-debug `clipInfo` readers that could see water-derived values are inside the legacy single-bitmap path (`!terrainTextures2`), which is mutually exclusive with the armed-fastpath precondition. Modern path has no readers. Brainstorm should confirm the audit is exhaustive (look for any new sites added post-2026-04-30) before greenlighting.

5. **Bucket-reservation removal vs leave-as-no-op.** Section C.2 shows the `addTriangleBulk(waterHandle, …)` calls at `quad.cpp:1087-1088` are harmless when the fast path is on (renderLists computes `totalVertices=0`, skips draw). Removing them is mechanical; leaving them costs ~2 slot allocations per water-bearing mission. Design pick: code clarity (remove) vs minimal-diff (leave).

6. **Future env-map water sidechannel.** `wAlpha` is documented as "environment Map Sky onto water" but has zero readers. If a future env-map-water shader pulls the value, the slice's removal of the writes would silently regress that shader. Brainstorm should decide: remove writes (simplify, document via commit message) vs keep writes (reduce future-friction; cost is ~8 fmuls per water-quad).

7. **Slice can be folded into the indirect-terrain endpoint.** Indirect-terrain SOLID-only PR1 (in progress) retires SOLID main-emit; a follow-up consolidation slice retires detail/overlay/mine. Water projection skip is an architecturally similar "fast-path with stranded upstream" cleanup. Could be picked up as a separate slice (low risk, fast turnaround) OR bundled into indirect-terrain followup work. Brainstorm should sequence.

---

## Section I — Recommendation

**Status: ready-for-brainstorm.**

Findings indicate the slice is well-bounded, low-risk, and architecturally simple compared to the renderWater Stage 1+2+3 work. The path is:

1. Wrap the `quad.cpp:773-1100` water projection block in an armed-check early return (skip when `WaterStream` is armed). Either inline the same compound predicate at `terrain.cpp:1048-1051` or extract `WaterStream::IsArmed()` first.
2. Decide bucket-reservation handling (Section H.5): remove or leave-as-no-op.
3. Validate via Option C (Section F.3) — rely on existing renderWater Stage 3 parity check + accumulator side-channel diagnostic + visual canary on mouse-pick.
4. Confirm tier1 5/5 PASS (unset / FASTPATH=1 / FASTPATH=1+SKIP=1 / FASTPATH=1+SKIP=1+PARITY_CHECK=1).

**Risks (in order of concern):**

- **Medium:** accumulator delta (Section D.5) — requires either a confirming measurement or a chosen-to-tolerate stance. If brainstorm decides to tolerate, ship with a visual-canary mouse-pick gate.
- **Low:** `clipInfo` overwrite delta (Section D.4) — requires readers-audit during design.
- **Low:** future env-map water shader regression (Section H.6) — well documented, easy to undo.
- **Low:** bucket reservation slot consumption (Section C.2 caveat) — bounded, harmless on stock missions.

**Estimated scope:** single commit to land the gate; an optional precursor commit for `IsArmed()` extraction. Closer to M2d-overlay than renderWater. Plan-write should be straightforward; no new SSBO / shader / bridge.

**Adjacency note:** the brainstorm should also decide whether this slice ships as a standalone (cheap, fast win) OR is rolled into the indirect-terrain follow-up consolidation work. The "fast-path with stranded upstream" pattern documented here applies to other potential slices (e.g. CPU-side cement-overlay computation if a future overlay slice retires its draw).

---

## Appendix — adversarial-review self-check (claims grep-verified at write-time)

Each non-obvious claim names a symbol; each named symbol was grep-verified at write time. Spot-checks:

- `mclib/quad.cpp:773` — verified by `Read` (water-projection block start with comment `NEW(tm) water texture code here`).
- `mclib/quad.cpp:1087-1088` — verified by `Read`; matched `Grep` result for `addTriangleBulk` (lines 1087, 1088 in nifty-mendeleev).
- `mclib/quad.cpp:801, 806, 869, 874, 939, 944, 1009, 1014` — verified by direct `Grep` for `wAlpha` in worktree path; results matched.
- `mclib/terrain.cpp:1048-1051` — pre-confirmed by first agent + this agent's Read.
- `mclib/terrain.cpp:1611-1636` — verified by Read; confirmed inside legacy `vertexProjectLoop` per-vertex branch.
- `mclib/terrain.cpp:1732` — verified `eye->setInverseProject(...)` call site by `Grep`.
- `mclib/txmmgr.h:830` — verified `addTriangleBulk` signature by `Read`.
- `mclib/txmmgr.cpp:1466-1530` — verified water-bucket flush loop by `Read`.
- `mclib/txmmgr.cpp:1494-1497` — verified underfill detection (`totalVertices = currentVertex - vertices`) by `Read`.
- `mclib/quad.cpp:2998, 3138, 3287, 3426` and `3022, 3162, 3311, 3450` — verified `addVertices(waterHandle/waterDetailHandle, …)` sites by `Grep`.
- `mclib/vertex.h:95` — verified `wAlpha` declaration by `Grep`.
- `wAlpha` reads: confirmed zero by `Grep` pattern `->wAlpha[^=]|\.wAlpha[^=]` returning only `=` writes.
- `calcThisFrame & 1` (terrain) at `quad.cpp:1111-1723` — verified by `Grep` content match.

Intentions about NEW symbols (no grep possible): `WaterStream::IsArmed()` helper (proposed, not present), `MC2_WATER_PROJECTION_SKIP` env flag (proposed, not present), `MC2_WATER_PROJECTION_SKIP_PARITY` (proposed, not present). These are explicitly framed as design proposals, not existing-code claims.
