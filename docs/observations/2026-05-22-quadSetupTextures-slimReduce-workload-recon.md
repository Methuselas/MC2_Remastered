# quadSetupTextures + slimReduce Workload Recon
# 2026-05-22 — post-F6-T1/T2/T3 ship, HEAD 1127d11

## Executive Summary

After F6 T1-T3 retired projectZ from the admission hot paths, the remaining
1.39ms self-time in TerrainQuad::setupTextures is dominated by generic per-quad
CPU work with no projectZ contamination: Shape-C recipe cache fetch, texture
handle resolution, `addTriangleBulk` enqueue, water-vertex frustum tests x4,
and a per-vertex boundary check (`IsGameSelectTerrainPosition`). The legacy CPU
lighting block is hard-gated out (`s_lightingGpuAuth = true`; quad.cpp:1325).
The only surviving projectZ-era concept is `rv->px/py/pz/pw` in slimReduce --
gated `!s_admissionModern` (terrain.cpp:1891) and thus SKIPPED on the Modern
path used in the Tracy capture. RenderWorld boundary spec can be written today
without encoding any projectZ/pz weirdness; no decontamination surgery is
required first.

---

## 1. TerrainQuad::setupTextures Workload Map

Source: `mclib/quad.cpp:715-1966` (grep-verified at write-time).

### 1a. One-time sentinel reset (quad.cpp:736-741)
Reset terrainHandle / waterHandle / overlayHandle etc. to 0xffffffff.
**Category C** (mandatory bookkeeping, ~10 stores, negligible).

### 1b. `legacyWaterDraw` gate evaluation (quad.cpp:756)
One call: `WaterFastPathOwnsArmedDraw()` -- non-inline, fans into
`IsFrameSolidArmed()` + `WaterStream::IsReady()` + `GetRecipeCount()`.
Hoisted once per quad (comment at 744-755 explains why).
**Category C** -- cross-TU function call, ~50-100ns, unavoidable while GPU
water fast path exists.

### 1c. Shape-C branch (terrainTextures2 path, quad.cpp:946-1023)
This is the live default path:

- `isTerrainQuadVisible()` (quad.cpp:950-954): reads 6 `clipInfo` values,
  two integer sums. **Category C** -- ~5ns, trivial.
- `getTerrainFaceCacheEntry()` (quad.cpp:974): array index compute + bounds
  check. **Category C** -- ~5ns.
- `ensureTerrainFaceCacheEntryResident()` (quad.cpp:977): 3-4 `tex_resolve`
  calls IF handles not resident. In steady state mostly fast (resident check
  only). Hot on first-frame or after texture eviction. **Category B/C** mix
  -- per-quad, but driven by texture residency state.
- `tryGetCachedTerrainRecipe()` (quad.cpp:991): 5 field copies from cache
  entry if valid. **Category C** -- ~5ns.
- `addTerrainTriangles()` (quad.cpp:1006): 1-2 `mcTextureManager->addTriangleBulk()`
  calls; each walks the master-node sorted list to insert a slot reservation.
  **Category B** -- per-quad, this is the single largest intrinsic cost in
  the shape-C path. Not movable to GPU (it feeds the legacy dual-queue
  MC_TextureManager that drives `renderLists()` CPU submission).
- `pz_emit_terrain_tris()` (quad.cpp:1010-1012): early-returns unless
  `g_pzTrace` env set. **Effectively Category C** (zero-cost unless probing).
- `enqueueTerrainMineState()` (quad.cpp:1019-1021): gated
  `!IsFrameMineArmed()`. Default-armed frames skip this entirely.

### 1d. Water block (quad.cpp:1032-1298)
Per-vertex (x4) pattern:
- Water-flag read from `pVertex->water` (bitfield, ~1ns).
- Deduplicated via `calcThisFrame & 2` bit: each vertex processed ONCE even
  if shared across quads.
- **Under Modern (default)**: `quadAabbInFrustum()` called per vertex.
  No matrix multiply. Then `IsGameSelectTerrainPosition()` -- 4 float
  comparisons, **Category C**.
- **Under legacyWaterDraw = true** (rare; GPU fast path NOT armed):
  additionally calls `projectForTerrainAdmission()` to fill wx/wy/wz/ww.
  **Category A** contamination -- but only active when GPU water fast path
  is absent (opt-out path).
- `addTriangleBulk(waterHandle, ...)` x2 -- only under `legacyWaterDraw`.
  **Category B** when active; **zero-cost** on armed frames.

Water block cost on armed Modern frames: ~4x `quadAabbInFrustum` + 4x
`IsGameSelectTerrainPosition` + `calcThisFrame` bit checks. Shared-vertex
dedup means many quads skip 2-3 of the 4 vertices. Dominates ~111-300us
of the Tracy "TerrainLightingDispatch + IndirectPreflight" range.

### 1e. Lighting block (quad.cpp:1311-1965)
`s_lightingGpuAuth = true` (quad.cpp:1325) hard-gates the body:
`if (!s_lightingGpuAuth ...)` is never entered. The GPU path
(`gos_terrain_lighting::CopyResultsToVertexPool`) writes lightRGB/fogRGB
before the loop starts (terrain.cpp:1943).
**Category C** -- dead code at runtime, zero cost in production. The
111us `TerrainLightingDispatch` Tracy bucket is the GPU dispatch cost
(terrain.cpp:1942), NOT per-quad CPU lighting.

### 1f. `pz_emit_terrain_tris` callsites (quad.cpp:805, 889, 1010)
All three call `pz_emit_terrain_tris()` which early-returns unless
`g_pzTrace` env var is set (quad.cpp:398). Default: zero cost.
**Category C** -- gated diagnostic.

### Summary of setupTextures self-time (1.39ms)

| Block | Category | Rough fraction |
|---|---|---|
| `addTriangleBulk` x2 in addTerrainTriangles | B | ~35% |
| `ensureTerrainFaceCacheEntryResident` (resident check + tex_resolve) | B/C | ~20% |
| Water frustum tests x4 (quadAabbInFrustum + IsGameSelectTerrainPosition) | B/C | ~20% |
| Sentinel resets + recipe cache fetch + isTerrainQuadVisible | C | ~10% |
| `WaterFastPathOwnsArmedDraw` hoist + minor overhead | C | ~5% |
| Dead lighting block | -- | 0% |
| pz_emit (gated off) | -- | 0% |
| Legacy water wx/wy/wz/ww writes | A (gated off by default) | 0% |

**No surviving projectZ contamination in the hot path under Modern + armed.**

---

## 2. slimReduce Workload Map (495us)

Source: `mclib/terrain.cpp:1697-1923` (grep-verified at write-time).

Per-vertex loop over `numberVertices` (full terrain vertex grid):

### 2a. Angular/sphere pre-cull (terrain.cpp:1718-1765)
`onScreenR` computation: eye-space transform via `Camera::cameraFrame.trans_to_frame`,
two `GetApproximateLength()` calls, angle comparison against `vClipConstant`.
Also `IsGameSelectTerrainPosition()` for edge-of-world verts.
**Category C** -- intrinsic cull logic, world-space only, no projection.

### 2b. Admission test (terrain.cpp:1808-1825)
Under Modern: `quadAabbInFrustum()` on a degenerate AABB (one point).
Under Legacy: `projectForTerrainAdmission()` -- full matrix mul + homogeneous
divide. Measured by SLIMSPLIT PROJ bucket.
**Category A** (legacy only) / **Category B** (Modern -- could move to GPU
frustum cull compute, but degenerate-AABB point test is already cheap).

### 2c. Cull-cascade writes (terrain.cpp:1835-1861)
`rv->clipInfo = clipR`, then `objBlockInfo[blockNum].active = true` and
`objVertexActive[vertNum] = true`. These are load-bearing (cull_gates_are_load_bearing.md).
`AppendSolidWindowCandidate()` under `s_solidNarrowOn`.
**Category C** -- intrinsic; GPU cannot own these without full GPU-driven
cull pipeline refactor (out of scope for current arc).

### 2d. `rv->px/py/pz/pw` raster coord writes (terrain.cpp:1891-1906)
Gated `!s_admissionModern`. Under Modern (Tracy capture): SKIPPED.
Under Legacy: writes D3D-era screen-space coords consumed by legacy raster
`TerrainQuad::draw()` on unarmed frames.
**Category A** (projectZ-era concept) -- but already gated off under Modern.

### 2e. RED reduction bracket (terrain.cpp:1914-1920)
The leastZ/mostZ/leastW/mostW/leastWY/mostWY writes were deleted in Phase 4
(2026-05-19). The SLIMSPLIT RED bracket is a retained empty envelope -- zero
cost except the rdtsc pair itself when MC2_SLIM_COST_SPLIT is set.

### SLIMSPLIT sub-bucket attribution

| Sub-bucket | What it measures | Category |
|---|---|---|
| PROJ | projectForTerrainAdmission (legacy) / quadAabbInFrustum (Modern) | A/B |
| CULL | clipInfo write + objBlockInfo/objVertexActive stores + solidWindow append | C |
| RED | Empty (deleted Phase 4) | -- |
| front/other (remainder) | onScreenR sphere math + px/py/pz/pw write (gated) | C/A (gated) |

On Modern path, PROJ is `quadAabbInFrustum` (fast), pz write is skipped, RED
is empty. The dominant cost is CULL (per-vertex stores into two flat arrays)
and onScreenR sphere math. Both are intrinsic CPU work.

---

## 3. Cost Classification Summary

| Work item | Cat | Comment |
|---|---|---|
| addTriangleBulk (SOLID/detail) in setupTextures | B | Per-quad MC_TextureManager enqueue; feeds CPU-driven renderLists() |
| ensureTerrainFaceCacheEntryResident tex_resolve | B/C | Mostly resident; spikes on eviction |
| Water quadAabbInFrustum x4 per water quad | B/C | Modern path; already fast vs legacy projectZ |
| onScreenR sphere/cone math in slimReduce | C | Intrinsic cull; no projectZ dependency |
| clipInfo / objBlock / objVertexActive writes | C | Load-bearing; cannot move to GPU in isolation |
| AppendSolidWindowCandidate | C | In-loop collect; trivially fast |
| Legacy px/py/pz/pw write (slimReduce) | A | Gated !Modern; dead on Modern path |
| Legacy wx/wy/wz/ww water write | A | Gated !legacyWaterDraw; dead when GPU water armed |
| CPU lighting block | -- | Hard-gated dead (s_lightingGpuAuth = true) |
| pz_emit_terrain_tris | -- | Gated off by default; diagnostic only |

**Verdict: the remaining 1.39ms self-time is Category B/C. No Category A
contamination is active on the Modern + armed-GPU path.**

---

## 4. RenderWorld Contamination Assessment

Contaminating concepts that would pollute a RenderWorld spec if encoded:

### 4a. `rv->px/py/pz/pw` raster coords (Category A -- gated, survivable)
Exist solely for legacy `TerrainQuad::draw()` on unarmed frames (mission
deploy / unit-select screen). Written by slimReduce under `!s_admissionModern`.
Not consumed by any GPU shader (verified: no `rv->px` read in any .comp/.vert
file per grep at write-time). RenderWorld spec can note these as "legacy
raster path only; not part of the GPU draw contract" without encoding them
as a contract output. Retire when drawPass fully retires.

### 4b. `legacyWaterDraw` branch in setupTextures water block
Writes wx/wy/wz/ww screen-space coords only when GPU water fast path is
NOT armed. Armed frames (default in-mission) skip it entirely. The contract
output for water is `waterHandle != 0xffffffff` (set unconditionally) plus
the water-tile predicate. RenderWorld spec needs only the armed-path contract.

### 4c. `pz_emit_terrain_tris` callsites
Diagnostic only; gated off by default. Not a contract output.

### 4d. `calcThisFrame` bitmask
Bits 1 and 2 are set to deduplicate per-vertex water and lighting work across
quads sharing vertices. This IS a legitimate per-frame state machine concept
(not projectZ-derived); it would appear in a clean RenderWorld spec as
"per-vertex frame-stamp for shared-vertex dedup."

### 4e. `clipInfo` field
Set by slimReduce. Read by `isTerrainQuadVisible()` and the water block.
This is a clean frustum-cull output -- the Modern path derives it from
`quadAabbInFrustum`, not projectZ. Clean inheritance for RenderWorld.

**RenderWorld spec can be written now.** The only concepts that would make it
ugly are px/py/pz/pw (already gated off) and wx/wy/wz/ww (already gated off).
The spec can describe them as "legacy raster path residuals, not part of the
GPU terrain draw contract."

---

## 5. Recommendation

### Path B: Move to RenderWorld spec now; treat bucket optimization as separate arc.

Rationale:

1. The 1.39ms self-time is Category B/C -- dominated by `addTriangleBulk`
   (MC_TextureManager per-quad slot walk) and texture residency checks. These
   are coupling artifacts of the legacy dual-queue MC_TextureManager, NOT
   projectZ artifacts. Attacking them before RenderWorld would mean speccing
   MC_TextureManager retirement -- a larger, separate effort documented in
   `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`.

2. No Category A contamination is active on the Modern + armed-GPU default
   path. The two surviving legacy concepts (px/py/pz/pw, wx/wy/wz/ww) are
   already gated off and can be annotated as residuals in the spec without
   encoding them as a contract.

3. RenderWorld spec written today would have a clean terrain-admission
   contract: "frustum-plane test on cached planes per vertex; `clipInfo` bool
   output; per-quad `waterHandle` / `terrainHandle` / `overlayHandle` emitted
   unconditionally for downstream draw/indirect consumers." No projectZ terms.

4. The natural next perf slice AFTER RenderWorld is `addTriangleBulk`
   elimination -- valid only after MC_TextureManager dual-queue retirement,
   which is the meta-fix for the whole bucket. That is a separate campaign arc.

### Surgical decontamination NOT required (Path C ruled out)

There is nothing to decontaminate on the hot path. The Category A items are
already gated off. No pre-surgery needed before writing the spec.

### If attacking the bucket independently (Path A flavor, separate arc)

The substitutive cut for `addTriangleBulk` cost requires retiring the legacy
`masterVertexNodes` queue in MC_TextureManager for terrain SOLID -- the GPU
indirect compute path already owns SOLID dispatch (`IsFrameSolidArmed()`
gates it). The BeginLegacySolidCluster() guard at quad.cpp:794/878/676 already
skips legacy SOLID under armed frames. The remaining `addTriangleBulk` calls
on armed frames are for detail/alpha/overlay/water -- these still feed legacy
renderLists(). Full elimination requires MC_TextureManager detail-overlay +
water retirement, which is a multi-slice campaign (see drawpass_retirement_
decal_bake_state_and_raster_sheet_trap.md for current state).
