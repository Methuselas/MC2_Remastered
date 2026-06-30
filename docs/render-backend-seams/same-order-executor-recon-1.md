# SAME-ORDER-EXECUTOR-1-RECON-1
## Same-Order Frame-Graph Executor — gaps and minimal first slice

**TL;DR** — A same-order, validate-only executor that wraps every kFramePassOrder pass
in executor-owned begin/end pairs (mirroring the existing PostProcess island pattern) is
roughly **45-50% ready** today. The PostProcess island and all 5 sub-stages are fully
owned. Water and VFX now have noteRenderPass seams (just landed). Shadow still has only
assertPassContract (no noteRenderPass). UI has beginPassScope/endPassScope but not the
executor island wrapper. MechOpaque and StaticPropOpaque are fully observed, ambient-
declared, and FBO-declared, but lack per-top-level-pass begin/end seams in the flat draw
owners. The **critical path** to slice 1 is: (1) add noteRenderPass to Shadow's begin site
(SHADOW-OBSERVE-3, 1-line); (2) build per-top-level-pass executor wrapper analogous to
executorOwnBeginSub/EndSub but for kFramePassOrder slots; (3) StatePack is not needed for
validate-only — the body sets its own state. No ambient/FBO declarations needed beyond
what already exists to start validate-only ownership.

**Readiness (validate-only same-order): ~45%.**
Critical path: Shadow observe seam + per-top-level-pass wrapper API.

---

## Q1 — Per-Pass Readiness Table

Sources: `RenderCore/RenderPassContract.h` (kFramePassOrder L373-386, kRenderPassContracts L163+),
`RenderCore/ambient_contract.h` (kPassAmbient L77), `RenderCore/fbo_ledger.h` (kPassFboTarget L58-63),
`mclib/render_contract.h` (PassIdentity enum L40), grep of all noteRenderPass callsites.

All file:line refs are drift-prone; re-grep before acting.

| Pass (kFramePassOrder slot) | Observed (noteRenderPass) | reads/writes declared | ambient declared | FBO declared | Wrappable now |
|---|---|---|---|---|---|
| **Shadow** (0) | **NO** — only assertPassContract at beginShadowPrePass (`gameos_graphics.cpp:6216`); no noteRenderPass | NO (snapshotRowAuthoritative=false) | YES — kPassAmbient row exists (SHADOW-OBSERVE-2, colorMask=Inherit, depthWrite=On, ShadowLess) | **NO** — explicitly absent from kPassFboTarget | **GAP: missing noteRenderPass** |
| **MechOpaque** (1) | YES — PassIdentity::OpaqueObject→MechOpaque at `txmmgr.cpp:2362` and `mclib/tgl.cpp:3016` (lossy: covers vehicles+buildings too) | YES (snapshotRowAuthoritative=true, reads=ShadowDynamicMap, writes=MainColor/MainDepth/MainNormal/SceneObjectId) | YES — kPassAmbient row (SceneGEqual, MainScene, depthWrite=On) | YES — MainColor via OpaqueObject→MechOpaque mapping (implicit; no kPassFboTarget row) | **Mostly ready; begin/end seam needs building** |
| **StaticPropOpaque** (2) | YES — PassIdentity::StaticProp at `gos_static_prop_batcher.cpp:5387` | YES (snapshotRowAuthoritative=true, reads=ShadowDynamicMap, writes=MainColor/MainDepth/MainNormal/SceneObjectId) | YES — kPassAmbient row (SceneGEqual, MainScene, depthWrite=On) | YES — kPassFboTarget row: MainColor | **Ready; begin/end seam needs building** |
| **Terrain** (3) | YES — PassIdentity::TerrainBase at `gos_terrain_lod_chunk.cpp:628` | PARTIAL (snapshotRowAuthoritative=false; reads=ShadowDynamicMap, writes=MainColor/MainDepth/MainNormal declared but not authoritative) | YES — kPassAmbient row (AllOn reassert, SceneGEqual, MainScene, **producesTerrainLatch=true**, depthWrite=On) | YES — kPassFboTarget row: MainColor | **Ready for validate-only; latch production is a cross-phase constraint (see Q3)** |
| **TerrainOverlay** (4) | YES — `gameos_graphics.cpp:9796` (beginPassScope + noteRenderPass at :9798) | PARTIAL (not authoritative) | **NO** — no kPassAmbient row | YES — kPassFboTarget row: MainColor | **Mostly ready; ambient gap is non-blocking for validate-only** |
| **TerrainDecal** (5) | YES — `gameos_graphics.cpp:9992` (beginPassScope at :9992) | PARTIAL | **NO** — no kPassAmbient row | YES — kPassFboTarget row: MainColor | **Mostly ready; ambient gap is non-blocking for validate-only** |
| **Water** (6) | YES — PassIdentity::Water at `gameos_graphics.cpp:3101` (DRYRUN-OBSERVE-COVERAGE-1) | PARTIAL (Water contract row exists; snapshotRowAuthoritative=false) | **NO** — no kPassAmbient row, comment at :3098 confirms | **NO** — no kPassFboTarget row | **Observed; both ambient and FBO gaps exist; validate-only can skip those checks** |
| **VegetationCards** (7) | YES — `gos_vegetation.cpp:386` (noteRenderPass + beginPassScope at :380/:387) | PARTIAL | **NO** — no kPassAmbient row | **NO** — no kPassFboTarget row | **Observed; ambient+FBO gaps; validate-only viable** |
| **VFX** (8) | YES — PassIdentity::ParticleEffect at `mclib/render_contract.cpp:840` (injected by noteRenderPass wrapper for gamecam particlesFlush) | PARTIAL | **NO** — no kPassAmbient row | **NO** — no kPassFboTarget row | **Observed; gaps; validate-only viable** |
| **UI** (9) | YES (partial) — beginPassScope/endPassScope at `gameos_graphics.cpp:7461/7550`; no standalone noteRenderPass | PARTIAL (writes=MainColor, no reads) | **NO** — no kPassAmbient row | **NO** — no kPassFboTarget row | **Has scope wrapper but no executor island; wrappable if scope → island** |
| **PostProcess** (10) | YES — noteRenderPass at `gameosmain.cpp:612`, beginPassScope at :618/626 | YES (reads=MainColor/MainDepth/ShadowDynamicMap; MainNormal missing — DEFERRED HIGH-3 per postprocess_subgraph.h:17) | YES — kPassAmbient row (consumesTerrainLatch=true, Inherit depth) | **NO** — intentionally absent (FBO at sample seam is sceneFBO_ not Backbuffer per executor-island-recon-1.md:278) | **FULLY OWNED — 5 sub-stages already executor-islands (CloudShadow/Shoreline/EdgeFog/FogOob/Composite)** |

**Summary: 1 pass (PostProcess) fully owned. 4 passes (MechOpaque, StaticPropOpaque, Terrain, TerrainOverlay+Decal as pair) observed + partial-ambient + FBO declared. 6 passes (Shadow, Water, VFX, VegetationCards, UI, + implied MechOpaque top-level wrap) need seam or wrapper work.**

---

## Q2 — Wrapping Seam Map

### PostProcess (already done — reference model)
`gos_postprocess.cpp` wraps each sub-stage inline:
```
executorOwnBeginSub(this, ExecutorIslandId::CloudShadow);  // :2457
runCloudShadow();
executorOwnEndSub(this, ExecutorIslandId::CloudShadow);    // :2459
```
`executorOwnBeginSub/End` live at :4723/:4767 and call `findIslandContract()` → validate pre/post
conditions. The outer composite is wrapped via `beginPassScope`/`endPassScope` in `gameosmain.cpp:618/626`.

### Passes with clean single begin/end seams (wrappable like PostProcess today)

| Pass | Where to insert executorOwnBegin/End | Confidence |
|---|---|---|
| **StaticPropOpaque** | Around `GpuStaticPropBatcher::flush()` call, alongside the existing `noteRenderPass` at `gos_static_prop_batcher.cpp:5387` | HIGH — single flush call, clean seam |
| **Terrain** | Around the LOD-chunk dispatch at `gos_terrain_lod_chunk.cpp:628` (where noteRenderPass already fires) | HIGH — single site (default path); other 3 branches would need additional wraps |
| **TerrainOverlay** | Already has beginPassScope/endPassScope at `gameos_graphics.cpp:9798/9852`; upgrade scope → executor island | HIGH — scope pair already there |
| **TerrainDecal** | Already has beginPassScope/endPassScope at `gameos_graphics.cpp:9992/10046`; upgrade scope → executor island | HIGH — scope pair already there |
| **VegetationCards** | At `gos_vegetation.cpp:380/387` (beginPassScope + noteRenderPass already call-sited) | HIGH — single flush site |
| **PostProcess outer** | Already owned (gameosmain.cpp:618/626) | DONE |

### Passes that need a seam built first

| Pass | Problem | Required work |
|---|---|---|
| **Shadow** | **HIGH** — `beginShadowPrePass` (`gameos_graphics.cpp:6175`) has only `assertPassContract`; no `noteRenderPass`; the shadow render is split across two functions (`beginShadowPrePass` + a dynamic shadow pass). A once-per-frame latch is needed (per dryrun-observe-coverage-recon-1.md:100) | Add `noteRenderPass(ShadowCaster, "beginShadowPrePass")` at :6216 alongside the existing assertPassContract. Only FIRST call counts. |
| **MechOpaque** | noteRenderPass fires at `txmmgr.cpp:2362` (OpaqueObject) — this covers vehicles+buildings (lossy). The GPU mech batcher has its own path (`gos_mech_batcher.cpp`). No single "top-level MechOpaque begin" exists — the draw is scattered across renderLists' opaque flush. | **HIGH** — the "begin" is distributed; the clean seam is at the renderLists preamble in `txmmgr.cpp:2362` (already observed). An executor wrapper here is architecturally ambiguous (OpaqueObject != MechOpaque 1:1). Needs disambiguation or accept lossy wrap. |
| **Water** | noteRenderPass exists (`gameos_graphics.cpp:3101`), but the "end" boundary is not marked. No scope wrapper. | Add endPassScope or executor-island EndSub around `renderWaterFastPath` call. Low effort. |
| **VFX** | noteRenderPass fires via gamecam particle flush wrapper (`render_contract.cpp:840`) which is INJECTED, not at the VFX draw site directly. No endPassScope. | Add scope wrapper at `gamecam.cpp` particle flush site. Medium effort. |
| **UI** | Has beginPassScope/endPassScope at `gameos_graphics.cpp:7461/7550` but no executor island contract row. | Promote to executor island: add `IslandContract` row + call `executorOwnBeginSub`/`End` in place of the raw scope calls. |

**HIGH finding: MechOpaque has no single clean top-level begin/end.** The OpaqueObject PassIdentity is shared with vehicles and buildings. A same-order executor wrapping "MechOpaque" at the renderLists preamble would own a multi-pass aggregate, not a single logical pass. This is the primary structural gap. Shadow's missing noteRenderPass is the easiest fix (1 line).

---

## Q3 — Validate-Only vs Apply Analysis

### What "validate-only" means for the same-order executor
The existing island executor validates BEFORE the body call (pre-conditions) and AFTER (post-conditions),
but does NOT apply state — the body sets its own GL state. This is safe because:
1. No reorder occurs (same-order executor preserves the existing call sequence).
2. No synthetic state mutations are introduced.
3. The executor's contribution is purely observational + counter increment.

### Per-pass analysis

| Pass | Validate-only viable now? | Needs StatePack to start? |
|---|---|---|
| PostProcess (all sub-stages) | YES — already done | NO |
| StaticPropOpaque | YES — ambient + FBO declared; pre: program valid + sceneColor exists; post: FBO unchanged | NO |
| Terrain | YES — ambient declared; latch production is a BODY responsibility, not executor's | NO |
| TerrainOverlay / TerrainDecal | YES — FBO declared; ambient missing but validate-only skips undeclared axes | NO |
| MechOpaque | YES with caveats — lossy OpaqueObject mapping; pre: ambient + FBO via implicit mapping | NO (but seam gap first) |
| Water / VFX / VegetationCards | YES — observe seam exists; undeclared ambient+FBO just means those checks are skipped (no false positive by design) | NO |
| Shadow | YES once noteRenderPass is added | NO |
| UI | YES once promoted to executor island | NO |

**StatePack (FRAMEGRAPH-STATEPACK-1) is NOT required for a validate-only executor.** StatePack is needed when the executor is going to APPLY ambient state (set GL state before calling the body). Since the body sets its own state and validate-only just CHECKS it, StatePack can be deferred to a later apply-phase slice.

### Cross-phase latches the executor must respect

**HIGH — terrain-latch constraint.** `kPassAmbient` declares:
- `Terrain.producesTerrainLatch = true` — body sets `markTerrainDrawn()` / `sceneHasTerrain_` (`ambient_contract.h:106`)
- `PostProcess.consumesTerrainLatch = true` — 4 post sub-stages bail if `!sceneHasTerrain_` (`ambient_contract.h:124`)

A same-order executor that wraps Terrain before PostProcess is safe (order preserved). If the executor ever deferred Terrain past PostProcess, the latch would be unset and 4 sub-stages would silently vanish. The same-order constraint is the guard. The executor must NOT allow order changes.

**HIGH — dispatch-MVP snapshot.** `g_dispatchMvp16` (`gos_terrain_indirect.cpp:1833`) is snapshotted
when the terrain dispatch executes and consumed by object batchers (static prop, mech). This is a
cross-pass data dependency that the executor must NOT break by reordering Shadow before terrain or
vice versa. Same-order-only: safe by construction.

### executor_owned_passes accounting (Q3b)

Current: `mc2_framegraph_executor_owned_passes()` at `gos_postprocess.cpp:4857` returns a count of
owned PostProcess sub-stage passes. It is a process-static counter inside gos_postprocess.cpp.

**Proposal for top-level ownership:** Add a second counter `mc2_framegraph_toplevel_owned_passes`
(or a bitmask of kFramePassOrder slots) to the same extern "C" surface. The debug_state_dump.cpp
already serializes both `executor_owned_passes` and `executor_validation_failures`
(`debug_state_dump.cpp:261-262`) — add a third field `executor_toplevel_owned_passes` as a bitmask
or count alongside. No arch change required; just extend the existing extern "C" block.

---

## Q4 — Ranked Blockers

**Rank 1 — Shadow noteRenderPass missing (SHADOW-OBSERVE-3)**
Shadow is the first pass in kFramePassOrder. Without `noteRenderPass(ShadowCaster)` at
`gameos_graphics.cpp:6216` (alongside the existing assertPassContract), the dryrun recorder
will always flag Shadow as `didNotFire`. Fix: 1 line. Low risk, no GL change.
Sources: dryrun-observe-coverage-recon-1.md:100, ambient_contract.h:87.

**Rank 2 — Per-top-level-pass executor begin/end wrapper API**
The existing `executorOwnBeginSub`/`EndSub` are PostProcess-specific (gos_postprocess.cpp).
For top-level passes, equivalent logic needs to live in a shared location. The cleanest approach:
a new `executorOwnBeginTopLevel(RenderPassId, ...)` / `EndTopLevel(...)` pair in
`mclib/render_contract.cpp` or a new `RenderCore/top_level_executor.h`, gated on
`MC2_FRAMEGRAPH_EXECUTOR`. Each wrappable pass callsite adds Begin/End around the body call.

**Rank 3 — MechOpaque seam disambiguation**
OpaqueObject PassIdentity is lossy (mechs + vehicles + buildings). To own MechOpaque cleanly,
either: (a) add a dedicated `PassIdentity::MechOpaque` note at the GpuMechBatcher flush path,
or (b) accept that the "MechOpaque" executor island owns the aggregate OpaqueObject slot (lossy
but correct for ordering). Option (b) is lower churn for slice 1.

**Rank 4 — Ambient/FBO gaps for Water, VFX, VegetationCards, UI, TerrainOverlay, TerrainDecal**
These passes have no kPassAmbient or kPassFboTarget rows. For validate-only, this means the
pre/post checks simply skip those axes (no false positive). These gaps are acceptable in slice 1
but should be filled for any apply-phase slice (FRAMEGRAPH-STATEPACK-1 territory).

**Rank 5 — StatePack (FRAMEGRAPH-STATEPACK-1)**
Needed only for an apply-phase executor that sets GL state before body calls. Validate-only
defers this entirely. When apply lands, StatePack must exist for ALL 11 passes before it fires.
This is the parallel recon's deliverable.

**Rank 6 — PostProcess MainNormal read gap**
`postprocess_subgraph.h:17` notes: "DEFERRED (HIGH-3): top-level kRenderPassContracts PostProcess
row MainNormal producer." The Shoreline sub-stage reads sceneNormalTex_ but the contract row omits
MainNormal from reads[]. Existing sub-stage ownership is unaffected; fixing this is a follow-up
for POSTPROCESS-SUBGRAPH-2.

---

## Q5 — Minimal First Same-Order Slice: FRAME-GRAPH-SAME-ORDER-EXECUTOR-1

### Scope (In for slice 1)

**Gate:** `MC2_FRAMEGRAPH_EXECUTOR` (already exists)

**Passes in scope (validate-only wrapping):**
1. **StaticPropOpaque** — clean single flush site; full ambient+FBO declared; add executorOwnBeginTopLevel/End around gos_static_prop_batcher.cpp flush.
2. **Terrain** — clean LOD-chunk site (default path); ambient declared; note terrain latch is body-produced (no executor mutation needed).
3. **TerrainOverlay** — upgrade existing beginPassScope → executor begin/end.
4. **TerrainDecal** — upgrade existing beginPassScope → executor begin/end.
5. **VegetationCards** — existing noteRenderPass at single flush site; add begin/end.
6. **PostProcess** (all sub-stages) — ALREADY OWNED; extend `executor_toplevel_owned_passes` counter to include this slot.

**Passes deferred from slice 1:**
- **Shadow** — add noteRenderPass (SHADOW-OBSERVE-3) as a prerequisite; then include in slice 2.
- **MechOpaque** — seam disambiguation (Q4 rank 3); include in slice 2.
- **Water** — add endPassScope; include in slice 2.
- **VFX** — add scope wrapper at gamecam particle site; include in slice 2.
- **UI** — promote scope to island; include in slice 2.

### New infrastructure for slice 1

```cpp
// RenderCore/top_level_pass_executor.h (new, header-only, ~40 lines)
// Gated on MC2_FRAMEGRAPH_EXECUTOR.
namespace RenderCore { namespace framegraph {
struct TopLevelIslandContract {
    RenderPassId id;
    bool requiresFboInLedger;   // assert fboLedger can resolve before call
    bool postRequiresFboUnchanged; // assert same logical FBO after call
};
// executorOwnBeginTopLevel / EndTopLevel pattern mirrors executorOwnBeginSub/EndSub
// but keys on RenderPassId instead of ExecutorIslandId.
void executorOwnBeginTopLevel(RenderPassId, const TopLevelIslandContract&);
void executorOwnEndTopLevel(RenderPassId, const TopLevelIslandContract&);
// Implemented in mclib/render_contract.cpp (same TU as noteRenderPass).
}}
```

**Counter extension:** add `mc2_framegraph_toplevel_owned_passes` (bitmask `uint32_t` over
kFramePassOrder index) to `gos_postprocess.cpp`'s extern "C" block and to
`debug_state_dump.cpp:261` output.

### Exit criteria

- Gate OFF: byte-identical output vs baseline (same smoke trace).
- Gate ON: smoke tier1 green, 0 validation failures logged in diagnostic_trace.jsonl, `executor_toplevel_owned_passes` bitmask has bits 1,2,3,4,7,10 set (StaticPropOpaque, Terrain, TerrainOverlay, TerrainDecal, VegetationCards, PostProcess).

---

## Q6 — Readiness % and Critical Path

**Honest readiness for validate-only same-order executor (all 11 passes): 45%**

Breakdown:
- PostProcess (all sub-stages): DONE — 100% for this slot.
- StaticPropOpaque, TerrainOverlay, TerrainDecal, VegetationCards: ~80% ready (seam API needed; everything else exists).
- Terrain: ~75% (seam API + note that only LOD-chunk default path is covered; 3 legacy draw paths need separate wraps if active).
- Water, VFX, UI: ~40% (observed; end-scope or island promotion needed; ambient+FBO gaps).
- Shadow: ~25% (ambient declared; noteRenderPass missing; executor wrapping blocked until observe seam lands).
- MechOpaque: ~30% (observed lossy; no clean single begin/end; disambiguation needed).

**Critical path (shortest to first slice green):**

```
1. Add executorOwnBeginTopLevel / EndTopLevel API (~40 lines, header-only or render_contract.cpp)
2. Wire StaticPropOpaque (1 callsite), TerrainOverlay (upgrade scope), TerrainDecal (upgrade scope),
   VegetationCards (1 callsite) — each is 2-4 lines at existing callsite.
3. Wire Terrain LOD-chunk default path (1 callsite).
4. Extend extern "C" counter + debug_state_dump.
5. Smoke OFF (byte-identical) + ON (0 failures).
```

Estimated effort: 1-2 day slice (no ambient/FBO work required, no StatePack, no Shadow/Mech disambiguation).

**Dependency on FRAMEGRAPH-STATEPACK-1:** Not needed for slice 1 (validate-only). Required before
any apply-phase executor that emits GL state calls before body execution. Recommend proceeding
independently of that parallel recon.

---

## Key File References (drift-prone — re-grep before acting)

| Symbol / location | File | Line (approx) |
|---|---|---|
| `kFramePassOrder[]` | `RenderCore/RenderPassContract.h` | 373 |
| `kRenderPassContracts[]` | `RenderCore/RenderPassContract.h` | 163 |
| `kPassAmbient[]` | `RenderCore/ambient_contract.h` | 77 |
| `kPassFboTarget[]` | `RenderCore/fbo_ledger.h` | 58 |
| `ExecutorIslandId` enum | `RenderCore/frame_executor.h` | 27 |
| `executorOwnBeginSub/End` | `GameOS/gameos/gos_postprocess.cpp` | 4723/4767 |
| `mc2_framegraph_executor_owned_passes` | `GameOS/gameos/gos_postprocess.cpp` | 4857 |
| `noteRenderPass(Water)` | `GameOS/gameos/gameos_graphics.cpp` | 3101 |
| `noteRenderPass(StaticProp)` | `GameOS/gameos/gos_static_prop_batcher.cpp` | 5387 |
| `noteRenderPass(TerrainBase)` | `GameOS/gameos/gos_terrain_lod_chunk.cpp` | 628 |
| `noteRenderPass(VegetationCards)` | `GameOS/gameos/gos_vegetation.cpp` | 386 |
| `assertPassContract(ShadowCaster)` | `GameOS/gameos/gameos_graphics.cpp` | 6216 |
| `beginPassScope(UI)` | `GameOS/gameos/gameos_graphics.cpp` | 7461 |
| `beginPassScope(TerrainOverlay)` | `GameOS/gameos/gameos_graphics.cpp` | 9798 |
| `beginPassScope(TerrainDecal)` | `GameOS/gameos/gameos_graphics.cpp` | 9992 |
| `beginPassScope(PostProcess)` | `GameOS/gameos/gameosmain.cpp` | 618 |
| `markTerrainDrawn()` decl | `GameOS/gameos/gos_postprocess.h` | 248 |
| `g_dispatchMvp16` | `GameOS/gameos/gos_terrain_indirect.cpp` | 1833 |
| `dryrunFrameBoundary()` | `mclib/render_contract.cpp` | 736 |
| `PassIdentity` enum | `mclib/render_contract.h` | 40 |
| `OpaqueObject → MechOpaque` mapping | `mclib/render_contract.cpp` | 554 |
| `PostProcessSubpass` table | `RenderCore/postprocess_subgraph.h` | 60+ |

---

*Generated 2026-06-29. Branch claude/nifty-mendeleev HEAD a9f6d613.*
*Dependency: FRAMEGRAPH-STATEPACK-1 (parallel recon) for apply-phase; not needed for validate-only slice 1.*
