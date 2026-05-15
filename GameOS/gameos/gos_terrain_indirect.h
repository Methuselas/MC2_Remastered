// GameOS/gameos/gos_terrain_indirect.h
//
// Indirect terrain SOLID-only PR1 — Stage 0 scaffolding.
//
// Architectural endpoint of the SOLID-arc CPU->GPU offload. SOLID-only PR1
// retires the per-frame Terrain::quadSetupTextures SOLID main-emit setup
// loop in favour of:
//   - Static dense TerrainQuadRecipe SSBO  (Stage 2; existing 144 B / 9-vec4
//     schema from gos_terrain_patch_stream.h:87 verbatim — no growth)
//   - Per-frame TerrainQuadThinRecord SSBO (Stage 3; existing 32 B M2 schema)
//   - Per-frame DrawArraysIndirectCommand  (Stage 3; one cmd for SOLID-PR1,
//     headroom for 16 future buckets)
// Detail (MC2_DRAWALPHA), overlay (gos_PushTerrainOverlay), and mine paths
// stay legacy in this slice. Detail/overlay/mine consolidation = follow-up.
//
// Pattern template: gos_terrain_water_stream.{h,cpp} (renderWater Stage 1+2+3
// shipped 2026-04-30); preflight-arming hazard analysis from advisor stop-the-
// line on plan v1.
//
// Plan: docs/superpowers/plans/2026-04-30-indirect-terrain-draw-plan.md
// Design: docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md
//
// ---------------------------------------------------------------------------
// Tracy zone name reservations (consumed across stages — keep names verbatim)
// ---------------------------------------------------------------------------
//   Stage 1 zones (cost-split capture; gated under MC2_TERRAIN_COST_SPLIT=1):
//     "Terrain::SetupSolidBranch"          per-frame, around SOLID admit clusters
//     "Terrain::SetupDetailOverlayBranch"  per-frame, around DRAWALPHA / mine /
//                                          overlay clusters
//   Stage 2 zones:
//     "Terrain::IndirectRecipeBuild"       one-shot @ primeMissionTerrainCache
//     "Terrain::IndirectRecipeReset"       one-shot @ Terrain::destroy /
//                                          mission teardown
//   Stage 3 zones:
//     "Terrain::IndirectPreflight"         per-frame @ ComputePreflight()
//     "Terrain::ThinRecordPack"            per-frame @ packer entry
//     "Terrain::IndirectDraw"              per-frame @ glMultiDrawArraysIndirect
//   Aggregator (Stage 3, optional):
//     "Terrain::TotalCPU"                  per-frame @ outer-most caller scope

#pragma once

#include <cstdint>

// Forward-declare so callers of RecipeForVertexNum don't need to include
// gos_terrain_patch_stream.h just for the return type.
struct TerrainQuadRecipe;

namespace gos_terrain_indirect {

// ---------------------------------------------------------------------------
// Env-gate readers (boot-time once; cached in function-scope statics so
// subsequent calls are a single branch-predicted bool load).
// ---------------------------------------------------------------------------
//
// Stage 0..3 default OFF: only literal "1" turns the path on. Stage 4
// flip shipped (this commit): IsEnabled() defaults on (only literal
// "0" opts out); other gates stay default-off.
bool IsEnabled();              // MC2_TERRAIN_INDIRECT
bool IsParityCheckEnabled();   // MC2_TERRAIN_INDIRECT_PARITY_CHECK
bool IsTraceEnabled();         // MC2_TERRAIN_INDIRECT_TRACE — gates
                                // [TERRAIN_INDIRECT v1] event=... lifecycle prints
bool IsCostSplitEnabled();     // MC2_TERRAIN_COST_SPLIT — gates Stage 1
                                // per-frame steady_clock accumulators in quad.cpp.
                                // When unset, the RAII timer scopes are zero-cost
                                // no-ops (single branch-predicted bool load each).

// ---------------------------------------------------------------------------
// Stage 1 cost-split accumulators — used by RAII timers in quad.cpp.
//
// Per-frame steady_clock nanosecond totals split between SOLID admit clusters
// (target of this slice) and DRAWALPHA detail/mine/overlay clusters (out of
// scope here). Reported via the existing 600-frame summary line, suppressed
// when MC2_TERRAIN_COST_SPLIT is unset to avoid all-zero noise.
//
// Per-quad ZoneScopedN() rejected for this measurement: 8 clusters x 14K
// quads x 60 fps would saturate Tracy's queue and ZoneScopedN overhead would
// become comparable to the work being measured. Per-frame summation matches
// slice 2b's mine-counter convention.
// ---------------------------------------------------------------------------
void      CostSplit_AddSolidNanos(long long n);
void      CostSplit_AddDetailOverlayNanos(long long n);
// PR2c Stage 0c — mine-specific accumulators. Independent of solid /
// detail-overlay buckets; same RollFrame cadence.
void      CostSplit_AddMineEnqueueNanos(long long n);
void      CostSplit_AddMineDrawNanos(long long n);
// PR2b Stage 0b — overlay-specific accumulator. Brackets ONLY the M2d
// fast-path block (CostSplitDetailOverlayScope brackets the legacy
// Shape-C fallback at quad.cpp:485-503 which fires ~0 times in tier1
// steady-state). Same RollFrame cadence.
void      CostSplit_AddOverlayNanos(long long n);
// 1A-alt Slice 0 — recon buckets for the unmeasured sub-tasks inside
// TerrainQuad::setupTextures. Path 2 onion-peel uses these to identify
// which sub-task to port to GPU compute first. Same RollFrame cadence.
//   WaterVertProj — water-elevation vertex projection block at
//     quad.cpp:833-1140 (4 per-vertex projectForTerrainAdmission calls
//     + per-vertex leastZ/mostZ reductions + waterHandle resolution +
//     addTriangleBulk water/waterDetail). Expected hot at wolfman zoom
//     on water-heavy missions (mc2_10).
//   Lighting — per-vertex lighting block at quad.cpp:1147-1771
//     (4 vertices × numTerrainLights × falloff + RGB accumulation +
//     lightRGB pack). Expected hot at wolfman zoom on lit missions.
//   RecipeCache — Shape-C recipe lookup + member assignments +
//     addTerrainTriangles at quad.cpp:781-818. Expected small post
//     Shape-C cache flip; instrumented to confirm.
void      CostSplit_AddWaterVertProjNanos(long long n);
void      CostSplit_AddLightingNanos(long long n);
void      CostSplit_AddRecipeCacheNanos(long long n);
// 1A-alt Slice 0 follow-up — close the "missing 8ms" gap between Tracy
// outer-zone (~11ms) and sum of named sub-buckets (~3.5ms).
//   SetupTotal     — brackets the entire TerrainQuad::setupTextures body.
//                    sum_setup_total - (recipe + water + lighting + cache_resident)
//                    = unmeasured residual inside setupTextures (function-entry
//                    preamble, isTerrainQuadVisible, dispatch, mine state gate).
//   CacheResident  — brackets the cache-fetch + residency check between
//                    cold-path tex loads and the recipe-cache RAII block
//                    (rowCol math + getTerrainFaceCacheEntry +
//                    ensureTerrainFaceCacheEntryResident).
//   tracy_zone - sum_setup_total = outer for-loop overhead + per-frame setup
//   (ComputePreflight, BeginFrameNarrow, AppendNarrowCandidate gate, etc.)
void      CostSplit_AddSetupTotalNanos(long long n);
void      CostSplit_AddCacheResidentNanos(long long n);
// 1A-alt Slice 0 follow-up #2 — VisibilityCheck wraps isTerrainQuadVisible(*this)
// + the invisible-quad-handle-reset branch. Prime suspect for the remaining
// ~3ms inside setup_total that's unaccounted for by recipe + water + lighting +
// cache_resident. At wolfman zoom ~14K quads are iterated and ~8K are visible
// per frame counter inference, so ~6K quads pay this cost without entering
// any other measured block.
void      CostSplit_AddVisibilityCheckNanos(long long n);
// Call once per frame at the close of the per-quad setupTextures loop
// (terrain.cpp:1684 boundary). Internally gated on IsCostSplitEnabled() —
// safe to call unconditionally.
void      CostSplit_RollFrame();
long long CostSplit_GetSolidNanosTotal();
long long CostSplit_GetDetailOverlayNanosTotal();
long long CostSplit_GetMineEnqueueNanosTotal();
long long CostSplit_GetMineDrawNanosTotal();
long long CostSplit_GetOverlayNanosTotal();
long long CostSplit_GetWaterVertProjNanosTotal();
long long CostSplit_GetLightingNanosTotal();
long long CostSplit_GetRecipeCacheNanosTotal();
long long CostSplit_GetSetupTotalNanosTotal();
long long CostSplit_GetCacheResidentNanosTotal();
long long CostSplit_GetVisibilityCheckNanosTotal();
int       CostSplit_GetFramesObserved();

// ---------------------------------------------------------------------------
// N1 counters — units = per-quad (per cluster), NOT per-triangle.
//
// Each call increments by exactly ONE quad. Callers wrap a paired addTriangle
// admit cluster (e.g. quad.cpp:466-467 — two MC2_DRAWSOLID admits) and call
// the matching Add* helper ONCE per cluster, not once per addTriangle. Detail/
// overlay clusters bump the legacy_detail_overlay counter; SOLID clusters bump
// either legacy_solid_setup (un-armed legacy admit) or indirect_solid_packed
// (armed packer iteration).
//
// Without these counters, Stage 3 Gate B can pass on "renderer time went
// down" while completely missing the CPU-offload goal — see plan v2 N1.
// ---------------------------------------------------------------------------
void Counters_AddLegacySolidSetupQuad();      // un-armed legacy SOLID admit cluster
void Counters_AddIndirectSolidPackedQuad();   // armed indirect packer per packed quad
void Counters_AddLegacyDetailOverlayQuad();   // legacy DRAWALPHA / detail / mine /
                                              // overlay cluster (passive — never gated)

long long Counters_GetLegacySolidSetupQuads();
long long Counters_GetIndirectSolidPackedQuads();
long long Counters_GetLegacyDetailOverlayQuads();

// PR2c Stage 0c — mine-specific counters (independent of legacy_detail_overlay).
//   legacy_mine_enqueue_quads — incremented once per call to
//     enqueueTerrainMineState (mclib/quad.cpp:251). Drops to zero post Stage 2c
//     gate-off.
//   legacy_mine_draw_quads    — incremented once per call to TerrainQuad::drawMine
//     (mclib/quad.cpp:4240). Drops to zero post Stage 2c gate-off.
//   indirect_mine_drawn_cells — Stage 2c wires; placeholder here.
void      Counters_AddLegacyMineEnqueueQuad();
void      Counters_AddLegacyMineDrawQuad();
void      Counters_AddIndirectMineDrawnCells(long long n);
long long Counters_GetLegacyMineEnqueueQuads();
long long Counters_GetLegacyMineDrawQuads();
long long Counters_GetIndirectMineDrawnCells();

// PR2a Stage 0a — M2c detail-emit counter. Confidence guard for the
// upcoming Stage 1a delete: pre-delete this should be non-zero on
// water-interest tiles (~5800/frame on mc2_01 normal zoom per
// m2_thin_record_cpu_reduction_results.md). Post-Stage-1a it MUST be
// zero (the call site is deleted along with the M2c block at
// quad.cpp:2001-2070). Counter declaration STAYS post-delete per the
// "demote, don't delete" debug-instrumentation rule (worktree
// CLAUDE.md "Debug Instrumentation Rule"); body becomes orphan no-op.
void      Counters_AddM2cDetailEmitQuad();
long long Counters_GetM2cDetailEmitQuads();

// PR2b Stage 0b — overlay-specific counters.
//   legacy_m2d_overlay_emit_quads — incremented once per call to the M2d
//     overlay block at quad.cpp:2035-2083 (the
//     `if (useOverlayTexture && overlayHandle != 0xffffffff)`).
//     Drops to zero post Stage 3b legacy gate-off.
//   indirect_overlay_packed_quads — Stage 2b wires per-frame thin-record
//     packer; placeholder here.
//   gos_push_overlay_calls — incremented once per call to
//     gos_PushTerrainOverlay regardless of which producer (M2d fast-path
//     OR Shape-C fallback). Diagnostic probe: confirms whether the
//     overlay-producer pipeline is live anywhere on this mission. If
//     zero across tier1, PR2b reframes from indirect-draw to delete-slice.
void      Counters_AddM2dOverlayEmitQuad();
void      Counters_AddIndirectOverlayPackedQuad();
void      Counters_AddGosPushOverlayCall();
long long Counters_GetM2dOverlayEmitQuads();
long long Counters_GetIndirectOverlayPackedQuads();
long long Counters_GetGosPushOverlayCalls();

// PR2c Stage 0c — env gate readers.
//   IsMineEnabled()       — MC2_TERRAIN_INDIRECT_MINE (default OFF until Stage 4
//                           default-on flip after Stage 2c soak).
//   IsFrameMineArmed()    — Stage 2c will wire to a real preflight latch; Stage 0c
//                           stub returns false unconditionally so callers compile
//                           and the gate-off sites can be staged ahead of arming.
bool IsMineEnabled();
bool IsFrameMineArmed();

// PR2b Stage 0b — env gate readers.
//   IsOverlayEnabled()        — MC2_TERRAIN_INDIRECT_OVERLAY (default OFF).
//   IsOverlayParityCheckEnabled() — MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK.
//   IsFrameOverlayArmed()     — Stage 3b wires the real predicate; Stage 0b
//                               stub returns false so gate-off sites can be
//                               staged ahead of the live draw.
bool IsOverlayEnabled();
bool IsOverlayParityCheckEnabled();
bool IsFrameOverlayArmed();

// ---------------------------------------------------------------------------
// PR2c Stage 1c — mine static-bake infrastructure.
//
// Lifecycle (matches spec at docs/superpowers/specs/2026-05-08-pr2c-mine-static-bake-design.md):
//   ResetMineStaticVBO()          — called from Terrain::primeMissionTerrainCache
//                                    + Terrain::destroy. CPU-clears state, keeps
//                                    GL buffer allocation.
//   ResetMineTextureArray()       — same chokepoints. Keeps GL_TEXTURE_2D_ARRAY
//                                    allocation across missions (texture content
//                                    is process-stable: defaults/mine_00.tga +
//                                    defaults/minescorch_00.tga).
//   MarkMineDirty()               — single chokepoint hook. Called from
//                                    MissionMap::setMine (move.h:634-646) +
//                                    MissionMap::rebuildTileMineCounts
//                                    (move.cpp:875). Idempotent — multiple
//                                    setMine events between frames produce
//                                    one rebuild.
//   RebuildMineStaticVBOIfDirty() — Stage 2c wires this to fire from the
//                                    Render.TerrainMines bridge before draw.
//                                    On first call (FirstBuildPending), invokes
//                                    BuildMineTextureArray (defensive lazy-load
//                                    of mineTextureHandle/blownTextureHandle if
//                                    still 0xffffffff — see Spec R7).
//   BuildMineStaticVBO()          — walks MissionMap; for each cell with
//                                    mine != 0, emits 6 verts (2 tris) per cell.
//                                    Uploads via glBufferData into g_mineStaticVBO_GL.
//   BuildMineTextureArray()       — 2-layer GL_TEXTURE_2D_ARRAY at sampler unit
//                                    5 (Stage 2c bridge binds). Layer 0 = mine,
//                                    layer 1 = blown. NEAREST filter, no mips
//                                    (matches gosHint_DisableMipmap | DontShrink
//                                    flags at quad.cpp:524, :531).
//
// Accessors (Stage 2c bridge consumes):
//   GetMineStaticVBO_GL()         — GLuint name; 0 if unallocated.
//   GetMineVertCount()            — int; 0 if mission has no mines.
//   GetMineTextureArrayGL()       — GLuint name; 0 if unallocated.
//   IsMineTextureArrayReady()     — true iff BuildMineTextureArray succeeded.
//
// Stage 1c verification: smoke clean, no behavior change. Stage 2c wires
// the rebuild + draw + legacy gate-off (single-PR per N2 partial-landing
// hazard rule).
void  ResetMineStaticVBO();
void  ResetMineTextureArray();
void  MarkMineDirty();
void  RebuildMineStaticVBOIfDirty();
void  BuildMineStaticVBO();
void  BuildMineTextureArray();
unsigned int GetMineStaticVBO_GL();
int          GetMineVertCount();
unsigned int GetMineTextureArrayGL();
bool         IsMineTextureArrayReady();

// Stage 2c — Render.TerrainMines hook calls this. Internally:
//   1. RebuildMineStaticVBOIfDirty (lazy first-build of texture-array + VBO).
//   2. If vert count > 0, calls gos_terrain_bridge_drawMineStatic.
//   3. Bumps Counters_AddIndirectMineDrawnCells(vertCount/6).
// Returns false on bridge failure (program not loaded etc.); does NOT return
// false when mission has no mines (that's a successful zero-emit frame).
bool DrawMineStatic();

// ---------------------------------------------------------------------------
// Stage 2: dense recipe SSBO build / lifecycle / per-entry invalidation.
//
// Dense recipe indexing convention (Option A):
//   vn (vertexNum) ∈ [0, mapSide²)  → g_denseRecipes[vn] is the slot.
//   vn == -1 (blankVertex)          → no recipe; lookup returns nullptr.
//   vn ≥ mapSide²                   → out-of-range; lookup returns nullptr.
// All references (parity-check, GLSL shader-side indexing through
// TerrainQuadThinRecord.recipeIdx in Stage 3) consume vn DIRECTLY.
// There is no +1 offset.
// ---------------------------------------------------------------------------

// Recipe-build / lifecycle
void BuildDenseRecipe();           // called from primeMissionTerrainCache
void ResetDenseRecipe();           // called from Terrain::destroy + start of Build
bool IsDenseRecipeReady();
const ::TerrainQuadRecipe* RecipeForVertexNum(int32_t vn);  // nullptr for vn<0 or out-of-range
void InvalidateRecipeForVertexNum(int32_t vn);             // precise; CPU recompute + mark dirty
void InvalidateAllRecipes();                               // whole-map; rebuild all slots + mark dirty

// Internal helper used by Stage 3's preflight too
void FlushDirtyRecipeSlotsToGPU();  // glBufferSubData per dirty slot

// Stage 2 parity body — walks live quadList, byte-compares recipe against
// per-quad legacy-equivalent computation. Returns quads_checked count.
int  ParityCompareRecipeFrame();

// ---------------------------------------------------------------------------
// Stage 3: per-frame indirect SOLID draw.
//
// Public API (callers: terrain.cpp, txmmgr.cpp, gameos_graphics.cpp,
// quad.cpp gate-off helpers):
//
//   ComputePreflight()      — call ONCE per frame BEFORE the setupTextures
//                             loop. Packs thin records, builds indirect cmds,
//                             sets IsFrameSolidArmed(). Safe when disabled.
//   IsFrameSolidArmed()     — true IFF preflight armed this frame. Read by the
//                             SOLID gate-off helpers in quad.cpp and the txmmgr
//                             hook. Result is stable for the rest of the frame.
//   DrawIndirect()          — thin executor: calls the bridge, handles hard
//                             failure (logs, ForceDisableArmingForProcess).
//                             Returns false if not armed or bridge fails.
//   ForceDisableArmingForProcess() — sticky process-wide latch. Once set,
//                             IsFrameSolidArmed() always returns false until
//                             process exit. Documented recovery = restart with
//                             MC2_TERRAIN_INDIRECT=0.
// ---------------------------------------------------------------------------
void BeginFrame();              // reset armed flag; call unconditionally once per frame
bool ComputePreflight();
// v4 split: ComputePreflight() does arming gates only. ComputeDispatch()
// runs after Phase 1's PackAndDispatch at terrain.cpp so it can read
// gos_terrain_lighting::GetOutputSsbo() with same-frame data.
// No-op when MC2_GPU_DRIVEN_TERRAIN_SOLID is unset or 0, or when not armed.
void ComputeDispatch();
// VPL parity-infra retirement (cpu-pack-retirement plan §7 OQ-2): the
// gos_terrain_indirect:: ComputeDispatchParity_Check declaration is removed
// (zero consumers post Step 4 2e11617; txmmgr caller removed same commit).
bool IsFrameSolidArmed();
bool DrawIndirect();
void ForceDisableArmingForProcess();

// ---------------------------------------------------------------------------
// Parity-check printer + 600-frame summary cadence.
//
// Stage 0 lands the printer skeleton; Stage 2 plugs in the actual recipe-
// content comparisons. Throttled to 16 mismatch prints per frame to keep
// logs bounded when an early-frame cascade fires.
//
// Schema (grep-friendly):
//   [TERRAIN_INDIRECT_PARITY v1] event=mismatch frame=N quad=Q layer=<name>
//                                tri=T vert=V field=<name> legacy=0xHEX fast=0xHEX
//   [TERRAIN_INDIRECT_PARITY v1] event=summary frames=N quads_checked=Q
//                                total_mismatches=K
//                                legacy_solid_setup_quads=N
//                                indirect_solid_packed_quads=N
//                                legacy_detail_overlay_quads=N
// ---------------------------------------------------------------------------
void ParityPrintMismatch(int frame, int quad, const char* layer, int tri,
                         int vert, const char* field,
                         uint32_t legacy, uint32_t fast);

// Call once per frame at the close of the per-quad setupTextures loop
// (terrain.cpp:1684 boundary). Resets the per-frame mismatch throttle and
// emits the summary line every 600 frames. Internally a no-op when neither
// IsParityCheckEnabled nor any counter has been bumped — cheap to call
// unconditionally.
void ParityFrameTick(int quadsCheckedThisFrame);

}  // namespace gos_terrain_indirect
