# DRYRUN-DRAWSITE-ORDER-RECON-1

Evidence and recommendation for modeling terrain's pre-renderLists draw site.

Branch: `claude/nifty-mendeleev`  HEAD at recon: `7f9e1fe8`

---

## TL;DR

The FRAME-GRAPH-EXECUTOR-DRYRUN-1 dry-run (~2 out-of-order/frame,
`firstOutOfOrderPass = RenderPassId::Terrain`) is detecting a real model/reality gap:
the declared `kFramePassOrder[]` puts StaticPropOpaque (slot 1) before Terrain (slot 2),
but at runtime the LOD-chunk terrain branch (the DEFAULT and increasingly-sole production
renderer) fires its `noteRenderPass` at `gamecam.cpp:508` BEFORE `renderLists()`, while
`GpuStaticPropBatcher::flush` fires INSIDE `renderLists()`.

**Recommendation: Option B — add a per-pass `knownEarlyDrawSite` flag to the
dry-run trace (set from `TerrainSubPass::drawSite`) and suppress out-of-order counting
for the Gamecam draw site.  Do NOT reorder `kFramePassOrder`.**

The draw site is **config-variable** (Q4 YES): LODChunk uses `TerrainDrawSite::Gamecam`,
the three other branches all use `TerrainDrawSite::RenderLists`.  A static reorder
would be wrong for non-default configs.

**Additional HIGH finding:**
`terrain_subpass_contract.h` marks IndirectBridge
`latchActuallyImplemented=false`, but `gos_terrain_indirect.cpp:3750` contains
`TERRAIN-INDIRECT-LATCH-FIX-1` which does call `markTerrainDrawn()`.  The contract
row is stale — it should be updated to `latchActuallyImplemented=true` (with
reachability caveat noted below).

---

## Q1 — Real draw order per terrain mode

### LODChunk (DEFAULT — production renderer since 2026-06-09 cutover)

Draw site: `TerrainDrawSite::Gamecam` — `Terrain::flushDrawCommands()` at
`code/gamecam.cpp:508`, BEFORE `renderLists()`.

`noteRenderPass(TerrainBase)` fires at:
`GameOS/gameos/gos_terrain_lod_chunk.cpp:628` (inside
`gos_TerrainLodChunk_SubmitDrawCommands`, called by `Terrain::flushDrawCommands`).

`markTerrainDrawn()` fires at: `GameOS/gameos/gos_terrain_lod_chunk.cpp:1161`
(within the same flush function).

The hoist is **intentional**: the comment at `code/gamecam.cpp:502-507` states
"Placed before renderLists() so chunk geometry is fully drawn before post-process;
after shadow pass so shadows resolve."  This is a deliberate architectural choice, not
an accidental hoist.

### IndirectBridge (non-default; needs `MC2_TERRAIN_INDIRECT=1` or armed camera)

Draw site: `TerrainDrawSite::RenderLists` — fires inside `renderLists()` via
`gos_terrain_indirect.cpp`.

`noteRenderPass(TerrainBase)` fires at: `gameos_graphics.cpp:7289` (the original
tessellation patch path, which is also the indirect bridge entry — line confirmed at
`7289`).

`markTerrainDrawn()`: `gos_terrain_indirect.cpp:3750` (`TERRAIN-INDIRECT-LATCH-FIX-1`).
**Contract row `terrain_subpass_contract.h:77` still says
`latchActuallyImplemented=false` — this is STALE.** The fix is present in source.

### PatchStreamThin (non-default)

Draw site: `TerrainDrawSite::RenderLists` — fires inside `renderLists()`.

`markTerrainDrawn()` at: `GameOS/gameos/gos_terrain_patch_stream.cpp:1501`.

### LegacyMLR (non-default; vestigial CPU tessellation path)

Draw site: `TerrainDrawSite::RenderLists` — inside `renderLists()` /
`gameos_graphics.cpp:7289`.

`markTerrainDrawn()` at: `gameos_graphics.cpp:7293` — CONDITIONAL on `extras-count>0`
(secondary latch-skip risk; already documented in `terrain_subpass_contract.h:116`).

---

## Q2 — What does kFramePassOrder actually represent?

Comment at `RenderCore/RenderPassContract.h:346-352` (drift-prone):

> "The order passes actually hit GL across a frame. Shadow resolves FIRST inside
> renderLists() even though shadow-caster enqueue happens after geometry enqueue —
> frameBegin() pre-seeds ShadowDynamicMap to paper over that; the canonical dependency
> is Shadow-before-geometry, encoded here by position. Edges are DERIVED from
> reads[]/writes[] against this order, not stored per-row."

So `kFramePassOrder` is self-declared as "the order passes actually hit GL" — a
**real-call-order** model, not a pure-logical-dependency or Vulkan-future order.  This
means a mismatch between the declared order and the actual call order IS a contract
violation, and the dry-run is right to flag it.

However, the declaration also implicitly treats Terrain as a single monolithic pass,
with one draw-site.  That is the modeling gap: there is no single static position that
is correct for all terrain branches.  The `kFramePassOrder` comment predates the
LODChunk hoist.

**`kFramePassOrder` today = logical-dependency skeleton + mostly-real-call-order for
the default config, but stale for the Terrain slot where the default draw site changed
and was not reflected.**  It is NOT a "Vulkan-future scheduler order"; it is the
runtime order guide that the validator, ambient guard, and dry-run use.

---

## Q3 — Terrain branch reachability

| Branch | Reachable in gameplay? | Reachable in smoke/editor? | Status |
|--------|----------------------|---------------------------|--------|
| LODChunk | YES — default-ON (`MC2_TERRAIN_LOD_CHUNK` default 1) | YES | **Production authority** |
| IndirectBridge | YES — opt-in (`MC2_TERRAIN_INDIRECT=1` or armed camera) | YES (smoke tests might not arm it, but editor exercises it) | Active option; deletion target after LODChunk fully replaces it |
| PatchStreamThin | Needs explicit env var; less common | YES if armed | Active option; deletion target |
| LegacyMLR | Needs `MC2_TERRAIN_LOD_CHUNK=0` AND old tessellation path | Edge case; `[8Z_VESTIGIAL]` log printed at startup | **Vestigial** — `terrain.cpp:145` explicitly names it; no production renderer; smoke will not hit it normally |

LODChunk is the AUTHORITATIVE production terrain pass occurrence.  IndirectBridge and
PatchStreamThin are reachable transition-state paths.  LegacyMLR is vestigial but
reachable by explicit opt-out.

---

## Q4 — Is the draw site config-variable?

**YES.**  Evidence from `RenderCore/terrain_subpass_contract.h:41-117`:

| TerrainPath | `drawSite` | Default? |
|-------------|-----------|---------|
| LODChunk | `TerrainDrawSite::Gamecam` — `gamecam.cpp:508` BEFORE `renderLists()` | **YES (default)** |
| IndirectBridge | `TerrainDrawSite::RenderLists` | No |
| PatchStreamThin | `TerrainDrawSite::RenderLists` | No |
| LegacyMLR | `TerrainDrawSite::RenderLists` | No (vestigial) |

**HIGH: A static reorder of `kFramePassOrder` (Terrain before StaticPropOpaque) would
be correct only when `MC2_TERRAIN_LOD_CHUNK` is on (the default).  In the three
non-default branches Terrain draws INSIDE `renderLists()`, after
`GpuStaticPropBatcher::flush`.  Option A would either mis-model non-default configs
(silently suppressing a real out-of-order if any non-LODChunk branch were dominant)
or would be a lying declaration for configs where the real order matches slot 2.**

The `TerrainDrawSite` enum already models this per-branch distinction precisely for
this purpose.

---

## Q5 — Consumers that depend on terrain-before-renderLists

The intentional hoist to `gamecam.cpp:508` exists so:
1. Chunk geometry is fully drawn to depth buffer before `renderLists()` flushes
   static props and mechs (depth-correct occlusion).
2. `markTerrainDrawn()` is set before post-process chains check `sceneHasTerrain_`
   (screenShadow / cloudShadow / shoreline / edgeFog / godrays — 5 passes at
   `gos_postprocess.cpp:1303/1936/2030/2173/2234/2284/2341`).
3. Water uses depth written by chunk draws (gamecam comment at `code/gamecam.cpp:519-527`).
4. MVP snapshot: LODChunk reads either the dispatch-MVP or the live MVP
   (`gos_terrain_lod_chunk.cpp:635`), which is established before the flush call.

None of these dependencies require StaticPropOpaque to have already drawn.
StaticPropOpaque writes `MainColor/MainDepth` but nothing downstream of Terrain reads
those writes before Terrain itself.  The DAG does not require StaticProp-before-Terrain.

---

## Q6 — DAG safety walk

DAG walk for hypothetical `kFramePassOrder = {Shadow, Terrain, StaticPropOpaque, ...}`:

| Pass | reads[] | produced before? | verdict |
|------|---------|-----------------|---------|
| Shadow | {} | (external) | OK |
| Terrain | {ShadowDynamicMap} | Shadow wrote it | OK |
| StaticPropOpaque | {ShadowDynamicMap} | Shadow wrote it | OK |
| MechOpaque | {ShadowDynamicMap} | Shadow wrote it | OK |
| PostProcess | {MainColor, MainDepth, ShadowDynamicMap} | all written | OK |

The reordered order is DAG-valid.  The DAG alone does NOT prevent Option A.  The
blocker is that Option A is a static model for a dynamic property (Q4).

---

## Q7 — kFramePassOrder users and blast radius of Option A

Files referencing `kFramePassOrder` or `kFramePassOrderCount` (live, `7f9e1fe8`):

| File | Use | Impact of Terrain↔StaticProp reorder |
|------|-----|--------------------------------------|
| `RenderCore/RenderPassContract.h:353` | Definition | changes |
| `RenderCore/frame_pass_trace.h:36-93` | Indexed by declared slot; slot indices shift for Terrain (2→1) and StaticProp (1→2) | silent slot renumber; no logic break |
| `RenderCore/frame_graph_validate.h:83` | DAG validator sweep | runs correctly with new order (DAG-valid, Q6) |
| `RenderCore/fbo_ledger.h:59-60` | `kPassFboTarget[]` keyed by `RenderPassId`, not position | no impact |
| `RenderCore/ambient_contract.h:77-118` | `kPassAmbient[]` keyed by `RenderPassId`, not position | no impact |
| `mclib/render_contract.cpp:715,730,747,771` | `resetTrace`/`dryRunCompare` calls | correct with any valid permutation |
| `tests/unit/test_frame_graph.cpp:380-387` | "dryrun (b)" — 7-pass observable set fires in declared order | Terrain/StaticProp must swap in this list |
| `tests/unit/test_frame_graph.cpp:398-418` | **"dryrun (c)"** — hardcodes Terrain-before-StaticProp as the BROKEN scenario; asserts `outOfOrderCount > 0` | **BREAKS: with Option A this is now the correct order and `outOfOrderCount==0`, flipping the assertion** |
| `tests/unit/test_rendercore.cpp:408` | `CHECK(kFramePassOrderCount == 11)` | count unchanged |
| `tests/unit/test_rendercore.cpp:429-432` | Every-id-once coverage | no impact |
| `tools/render_pass_table_harness/render_pass_table_harness.cpp` | DAG coverage harness | re-runs correctly |

**HIGH: `tests/unit/test_frame_graph.cpp:398-418` ("dryrun (c)") explicitly uses
Terrain-before-StaticProp as the canonical out-of-order failure scenario.  Option A
inverts that test's assertion from pass to fail.**

---

## Q8 — Option B shape (pass-occurrence modeling via knownEarlyDrawSite)

Add a per-slot `knownEarlyDrawSite` flag to the dry-run trace, set by the runtime
layer from the dominant branch's `TerrainDrawSite`, checked in `dryRunCompare` before
the sequence-monotonicity test.

This mirrors the existing `TerrainSubPass::drawSite` enum, which was introduced
precisely for this distinction.  No new concept needed.

### Minimal diff (not built)

**`RenderCore/frame_pass_trace.h`** — add one field to `FramePassEntry`:
```cpp
bool  knownEarlyDrawSite = false;
// When true: this pass is declared to draw before its declared slot position
// (e.g. LODChunk terrain fires at gamecam.cpp:508 before renderLists).
// dryRunCompare() skips the out-of-order check for this entry.
```

**`RenderCore/frame_pass_trace.h` — `dryRunCompare` kernel (line ~132)**:
```cpp
if (e.sequenceIdx < prevSeq && !e.knownEarlyDrawSite) {
    ++r.outOfOrderCount;
    ...
} else {
    prevSeq = e.sequenceIdx;
}
```

**`mclib/render_contract.cpp` — `dryrunFrameBoundary()`**:
After snapshotting `terrainBranch`, look up whether the dominant branch draws early:
```cpp
const TerrainSubPass* sp = findTerrainSubPass(g_dryrunTrace.terrainBranch);
if (sp && sp->drawSite == TerrainDrawSite::Gamecam) {
    const int slot = declaredOrderIndex(RenderPassId::Terrain,
                                        kFramePassOrder, kFramePassOrderCount);
    if (slot >= 0) g_dryrunTrace.entries[slot].knownEarlyDrawSite = true;
}
```

`kFramePassOrder` untouched.  No FBO/ambient/DAG validator ripple.  No existing test
assertion flips.

---

## Decision: A vs B

| Criterion | Option A (reorder kFramePassOrder) | Option B (knownEarlyDrawSite flag) |
|-----------|-----------------------------------|-----------------------------------|
| Correct for LODChunk (default) | YES | YES |
| Correct for IndirectBridge/PatchStreamThin/LegacyMLR | NO — reorder is wrong; those branches draw inside renderLists | YES — draw site checked per dominant branch at frame boundary |
| `kFramePassOrder` blast radius | Load-bearing slot indices shift; slot rename propagates to trace indexing | kFramePassOrder unchanged |
| FBO/ambient contract impact | None (keyed by RenderPassId, not position) | None |
| Test impact | "dryrun (c)" assertion flips from pass to fail; 2+ tests need editing | No existing test breaks; add 2 new tests for suppression |
| Honesty when config changes | Silent mis-model: if LODChunk is off and IndirectBridge is dominant, dry-run would stop flagging the now-real StaticProp-before-Terrain order (Indirect draws inside renderLists, so that IS the correct order under Indirect — but Option A would have accepted it as wrong) | Correct: `knownEarlyDrawSite` is only set when dominant branch is Gamecam, adapts per-frame |
| Models the right abstraction | Treats terrain as a single monolithic pass with one static draw-site | Models terrain as a pass with a per-branch draw-site occurrence — consistent with the existing `TerrainSubPass` contract table |

**Recommendation: Option B.**

LODChunk is the authoritative production renderer.  Its Gamecam draw-site is
intentional and permanent (not a bug to fix away).  `TerrainDrawSite::Gamecam` is
already the declared, reviewed model for this.  Option B extends the dry-run kernel
to consult it.  Option A imposes a static reorder that is only correct for the default
config and would be incorrect (or at best misleading) for transition-state configs.

Avoid static reorder unless evidence proves all reachable terrain branches share the
same draw site.  They do not (Q3/Q4).

---

## Additional HIGH findings

### HIGH-1: terrain_subpass_contract.h IndirectBridge latch row is stale

`terrain_subpass_contract.h:77` says `latchActuallyImplemented=false` for
IndirectBridge.  But `gos_terrain_indirect.cpp:3750` contains
`TERRAIN-INDIRECT-LATCH-FIX-1` which DOES call `markTerrainDrawn()`.  The fix is
present in source; the contract row was not updated.  This causes:
- `terrainLatchMissActive=true` in the dry-run whenever IndirectBridge is dominant
- `test_frame_graph.cpp:427` (`CHECK(r.terrainLatchMissActive == true)`) will start
  failing once `latchActuallyImplemented` is corrected — the test has a comment warning
  about this at line 311.

**Fix:** update `terrain_subpass_contract.h:77` to `latchActuallyImplemented=true`
in a separate slice (boundary-condition: the fix at line 3750 IS gated on
`s_frameSolidCmdCount > 0`, so a terrainless frame still won't set the latch — that
is acceptable behavior matching the LegacyMLR `extras>0` caution).

### HIGH-2: kFramePassOrder comment is stale regarding Terrain's draw site

`RenderCore/RenderPassContract.h:346-352` says the order is "the order passes actually
hit GL across a frame."  The Terrain slot (slot 2, after StaticPropOpaque slot 1) has
not matched reality for the default LODChunk config since the gamecam hoist landed
(2026-06-09 per `[8Z_VESTIGIAL]` evidence).  The comment should be updated to note
that the Terrain slot reflects logical-dependency position, and that the LODChunk
branch actually draws before its declared slot (modeled in `terrain_subpass_contract.h`).

---

## SLICE PROPOSAL (not built)

### Slice: DRYRUN-DRAWSITE-ORDER-1

Goal: suppress the ~2/frame spurious `outOfOrderCount` for the LODChunk terrain
draw-site, update the stale IndirectBridge latch contract row, and add a comment
clarifying `kFramePassOrder`'s Terrain slot.

#### Files to change

| File | Change |
|------|--------|
| `RenderCore/frame_pass_trace.h` | Add `knownEarlyDrawSite = false` to `FramePassEntry`; add suppression guard in `dryRunCompare` (~line 132) |
| `mclib/render_contract.cpp` | In `dryrunFrameBoundary()`: after snapshotting `terrainBranch`, set `knownEarlyDrawSite` on the Terrain slot when dominant branch `drawSite == TerrainDrawSite::Gamecam` |
| `RenderCore/terrain_subpass_contract.h` | Update IndirectBridge row: `latchActuallyImplemented=true` (line 77); update `note` string; update `allDeclaredLatchProducersImplemented()` to now return true |
| `RenderCore/RenderPassContract.h` | Update comment at line 346-352 to note Terrain slot is logical-dependency position; LODChunk draws before this slot (see `terrain_subpass_contract.h`) |
| `tests/unit/test_frame_graph.cpp` | Add "dryrun (f): Terrain early-draw-site suppressed when Gamecam" test; add "dryrun (g): knownEarlyDrawSite=false still flags it"; update test at line 427 (`terrainLatchMissActive == true` for Indirect → now false) |

#### Symbols touched

- `RenderCore::framegraph::FramePassEntry` — add `knownEarlyDrawSite`
- `RenderCore::framegraph::dryRunCompare` — add guard
- `render_contract::dryrunFrameBoundary` — set `knownEarlyDrawSite` at frame boundary

#### No changes needed

- `kFramePassOrder` array — untouched
- `RenderCore/frame_graph_validate.h` — DAG validator unchanged
- `RenderCore/fbo_ledger.h` — unchanged
- `RenderCore/ambient_contract.h` — unchanged

#### Slice preflight command (run before coding)

```powershell
py -3 tools\repo_intel\repo_query.py slice-preflight --slice DRYRUN-DRAWSITE-ORDER-1 `
    --symbols "FramePassEntry,dryRunCompare,dryrunFrameBoundary,kFramePassOrder,latchActuallyImplemented" `
    --paths RenderCore/frame_pass_trace.h mclib/render_contract.cpp `
            RenderCore/terrain_subpass_contract.h `
            tests/unit/test_frame_graph.cpp `
    --base 7f9e1fe8
```

---

*All file:line citations are live against HEAD `7f9e1fe8` and are drift-prone.*
