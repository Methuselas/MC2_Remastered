# MECHOPAQUE-PASSIDENTITY-RECON-1
# Map the lossy OpaqueObject identity + propose a clean ownable MechOpaque seam

> **Status:** RECON — read-only, no source changes.
> **Branch:** `claude/nifty-mendeleev`
> **Date:** 2026-06-29
> **Precursor:** SAME-ORDER-EXECUTOR-VALIDATE-1 (`0f4f2637`) — deferred MechOpaque because
> PassIdentity::OpaqueObject is lossy (vehicles + buildings + mechs).

---

## TL;DR + Recommended Seam Approach

**OpaqueObject covers: mechs (GPU batcher + legacy MLR), vehicles (legacy MLR TG_Shape::Render),
AND buildings (legacy MLR TG_Shape::Render).** StaticProps are SEPARATE (PassIdentity::StaticProp).

**The txmmgr.cpp:2362 note is definitively mis-placed.** It fires at the top of `renderLists()`
as a preamble note before any actual object draws. The real mech draw (GpuMechBatcher::flush) is
~900 lines later at txmmgr.cpp:3260, and legacy MLR object draws (vehicles/buildings/mechs without
GPU path) happen in the `Render.3DObjects` loop at txmmgr.cpp:2479–2544. The mis-placement is the
same class as the Shadow/Mech ordering issue noted in SHADOW-OBSERVE-3.

**Recommended option: Option C — aggregate "OpaqueObject" seam at the batcher-flush boundary.**
Keep `RenderPassId::MechOpaque` as the single aggregate pass covering all non-terrain, non-static-prop
opaque objects. Move the noteRenderPass to the actual draw entry (or add a dedicated
`PassIdentity::MechOpaque` note at GpuMechBatcher::flush entry). Do NOT split into
VehicleOpaque/BuildingOpaque — the kFramePassOrder invariant makes it costly (3 new IDs + 3
contract rows + static_assert changes) and vehicles/buildings share identical state/FBO contracts
with mechs, so there is no executor benefit from splitting.

**Minimal first slice (MECHOPAQUE-SEAM-1):** Add `render_contract::noteRenderPass(PassIdentity::OpaqueObject, "GpuMechBatcher::flush(entry)")` at the top of `GpuMechBatcher::flush()` (gos_mech_batcher.cpp:1777), and wrap the `Render.3DObjects` zone at txmmgr.cpp:2479 with
`executorOwnBeginTopLevel`/`EndTopLevel(PassIdentity::OpaqueObject)`. The txmmgr:2362 preamble
note becomes a secondary advisory note (retag it "preamble" to disambiguate) or remove it.

---

## Q1 — OpaqueObject Coverage Map

### What PassIdentity::OpaqueObject covers

Two noteRenderPass sites exist:

| Site | File:Line | Comment in source |
|---|---|---|
| Preamble (mis-placed) | `mclib/txmmgr.cpp:2362` | "MC_TextureManager_renderLists(submit)" — fires at top of renderLists(), BEFORE any actual draws |
| Enqueue (per-shape) | `mclib/tgl.cpp:3016` | "TG_Shape_Render(enqueue)" — fires inside TG_Shape::Render() for each visible face batch |

**DRIFT-PRONE:** tgl.cpp:3016 line number may shift with TG_Shape::Render body edits.

### Draws that fire under OpaqueObject

The `Render.3DObjects` loop (txmmgr.cpp:2479–2544) iterates `masterHardwareVertexNodes[]` and
calls `ShapeRenderer::render()` or `gos_RenderIndexedArray()` for all nodes flagged
`MC2_DRAWSOLID` and NOT `MC2_ISTERRAIN`. This includes:

- **Mechs (legacy MLR path):** `TG_Shape::Render()` enqueues mech geometry via gos_DrawTriangles.
  Submitted here when `MC2_GPU_MECHS` is off.
- **Vehicles:** `TG_Shape::Render()` enqueues vehicle geometry via the same MLR path.
  No separate vehicle batcher exists. Vehicles draw in the same `Render.3DObjects` loop as mechs.
- **Buildings:** `TG_Shape::Render()` enqueues building geometry the same way. Building shapes
  go through the legacy MLR queue, same loop.
- **Mechs (GPU path):** `GpuMechBatcher::instance().flush()` at txmmgr.cpp:3260, fires AFTER
  `Render.3DObjects` (the GPU batcher does its own DrawCall, not through masterHardwareVertexNodes).

**StaticProps are SEPARATE:** `GpuStaticPropBatcher::instance().flush()` at txmmgr.cpp:3244 uses
`PassIdentity::StaticProp` and maps to `RenderPassId::StaticPropOpaque`. Confirmed separate in
`render_contract.cpp:557` and `gos_static_prop_batcher.cpp:5337` (executorOwnBeginTopLevel StaticProp).

**Conclusion: OpaqueObject = mechs + vehicles + buildings (all non-terrain, non-static-prop solids).**
The comment in `render_contract.cpp:555` confirms: `// lossy: also vehicles + legacy buildings`.

---

## Q2 — Per-Batcher Seam Map

### Actual draw sequence inside renderLists()

```
txmmgr.cpp:2362  noteRenderPass(OpaqueObject, "preamble")   ← MIS-PLACED (fires here)
txmmgr.cpp:2366  ZoneScopedN("RenderLists.Preamble")        ← state setup only, no draws
txmmgr.cpp:2417  ZoneScopedN("RenderLists.LightDataUpload") ← SSBO upload only
txmmgr.cpp:2479  ZoneScopedN("Render.3DObjects")            ← ACTUAL DRAW: MLR vehicles/buildings/mechs
  txmmgr.cpp:2482  for(masterHardwareVertexNodes) → ShapeRenderer::render() / gos_RenderIndexedArray
  tgl.cpp:3016   noteRenderPass(OpaqueObject, "enqueue")    ← fires per shape during enqueue
txmmgr.cpp:2544  end Render.3DObjects
txmmgr.cpp:2547  ZoneScopedN("RenderLists.PostObjectsStateRestore")
...
txmmgr.cpp:3241  ZoneScopedN("GpuSP.BatcherFlush")          ← StaticPropBatcher (StaticPropOpaque)
  txmmgr.cpp:3244  GpuStaticPropBatcher::instance().flush()
txmmgr.cpp:3257  ZoneScopedN("Render.GpuMechs")             ← ACTUAL DRAW: GPU mech batcher
  txmmgr.cpp:3260  GpuMechBatcher::instance().flush()        ← fires ~900 lines after :2362 preamble
```

**DRIFT-PRONE:** txmmgr.cpp line numbers above — this function is long and actively edited.

### Per-batcher seam analysis

| Batcher | Function | Begin seam | End seam | Clean? |
|---|---|---|---|---|
| Legacy MLR (vehicles/buildings/mechs) | `Render.3DObjects` loop (txmmgr.cpp:2479) | ZoneScopedN entry | zone exit at :2544 | YES — single contiguous block |
| GPU mech batcher | `GpuMechBatcher::flush()` (gos_mech_batcher.cpp:1777) | flush() entry | flush() return | YES — clean function boundary |
| Static prop batcher | `GpuStaticPropBatcher::flush()` | executorOwnBeginTopLevel at :5337 | executorOwnEndTopLevel | ALREADY OWNED |

**No separate vehicle or building batcher exists.** Vehicles and buildings draw through the same
`masterHardwareVertexNodes[]` loop as MLR mechs. They are indistinguishable at the OpaqueObject level
without per-node type tagging.

---

## Q3 — Is txmmgr:2362 Mis-placed?

**YES. HIGH.**

The note at txmmgr.cpp:2362 fires at the TOP of `renderLists()` as part of the `Preamble` zone,
which only sets render state (AlphaMode, ShadeMode, Fog, Culling). No actual object draws happen
until the `Render.3DObjects` zone at txmmgr.cpp:2479 (~117 lines later). The GPU mech batcher
flush (the dominant mech draw path) is at txmmgr.cpp:3260 (~900 lines later).

This is the same mis-placement class as the Shadow-vs-Mech ordering issue:
- SHADOW-OBSERVE-3 found that the shadow noteRenderPass fires before the shadow-caster enqueue
  has been resolved, leading to false ordering signals in the dryrun tracer.
- Here, the OpaqueObject preamble note fires before the actual draws, which can make the dryrun
  tracer believe MechOpaque has "started" before StaticPropOpaque even though StaticPropBatcher
  flush is the FIRST major draw after the preamble state setup.

**Where the note SHOULD be:**
1. At `GpuMechBatcher::flush()` entry (gos_mech_batcher.cpp:1777) — this is the canonical GPU
   mech draw entry, a clean function boundary, and matches how StaticPropBatcher uses
   `executorOwnBeginTopLevel` at its flush entry.
2. At the top of the `Render.3DObjects` zone (txmmgr.cpp:2479) — for the legacy MLR path
   (vehicles/buildings/MLR mechs), this is the correct begin marker.

The existing tgl.cpp:3016 per-shape enqueue note is lower-level advisory telemetry and can remain
as a secondary diagnostic. It is NOT a placement error (it accurately labels the enqueue moment).
The txmmgr:2362 preamble note is the error — it should be retagged as a preamble diagnostic
`"MC_TextureManager_renderLists(preamble-state-setup)"` or removed in favour of the two
begin markers above.

**Cross-ref SHADOW-OBSERVE-3:** The MechOpaque and Shadow preamble mis-placements are symmetric.
Both fire a "begin" note before the actual work. SHADOW-OBSERVE-3's recommendation was to move
the note to the actual shadow-caster draw entry; the same fix applies here.

---

## Q4/Q5 — Ownership Options + kFramePassOrder Impact

### Option A — Keep aggregate MechOpaque, move note to actual mech draw

Add `noteRenderPass(OpaqueObject, "GpuMechBatcher::flush(entry)")` at gos_mech_batcher.cpp:1777
and `executorOwnBeginTopLevel(OpaqueObject)` wrapping the `Render.3DObjects` zone. Rename the
preamble note or remove it. OpaqueObject→MechOpaque mapping is already in `render_contract.cpp:555`.

**kFramePassOrder impact:** ZERO. No new RenderPassId. No new kFramePassOrder entry.
kRenderPassIdCount stays at 11. The static_assert is untouched.

**Honesty cost:** The "MechOpaque" pass identity still covers vehicles and buildings. This is a
known approximation documented in the existing `// lossy: also vehicles + legacy buildings` comment.

### Option B — Split into MechOpaque + VehicleOpaque + BuildingOpaque

Three separate `RenderPassId` values, three `kRenderPassContracts[]` rows, three `kFramePassOrder`
entries.

**kFramePassOrder impact: HIGH.** Currently kRenderPassIdCount = 11 (Shadow, MechOpaque,
StaticPropOpaque, Terrain, TerrainOverlay, TerrainDecal, Water, VegetationCards, VFX, UI,
PostProcess). Splitting MechOpaque → 3 would raise the count to 13. Every place that touches
the count or iterates kFramePassOrder must be updated. The `static_assert` at
RenderPassContract.h:410-411 enforces that kFramePassOrder lists every real RenderPassId exactly
once — adding 2 new IDs requires 2 new kFramePassOrder slots.

**Executor benefit: NONE for slice 2.** Vehicles and buildings go through the same
`Render.3DObjects` loop with identical GL state contracts (same FBOs, same depth/blend state,
same kPassAmbient row). There is no per-type begin/end boundary inside the loop — splitting
PassIdentity without a corresponding batcher split would require adding per-node type discrimination
logic to the loop, which is higher churn than the pass identity split itself.

**Verdict: NOT RECOMMENDED for same-order slice 2.**

### Option C (RECOMMENDED) — Aggregate MechOpaque with a clean seam at the batcher-flush boundary

Use the existing `RenderPassId::MechOpaque` aggregate. The executor "owns" the entire
opaque-object draw block as one pass. The begin/end seam is:

- **Begin:** Entry of the `Render.3DObjects` zone (txmmgr.cpp:2479) — first actual draw call.
  For the GPU mech path, a secondary begin marker at `GpuMechBatcher::flush()` entry (gos_mech_batcher.cpp:1777)
  disambiguates within the aggregate if per-batcher telemetry is needed.
- **End:** Return from `GpuMechBatcher::flush()` (txmmgr.cpp:3261) — last actual object draw.

This is a clean, ownable seam matching the StaticPropBatcher pattern (`executorOwnBeginTopLevel`
at flush entry / `executorOwnEndTopLevel` at flush exit).

**kFramePassOrder impact:** ZERO. The "OpaqueObject covers mechs+vehicles+buildings" approximation
is acceptable because:
1. They share identical FBO contracts (MainColor/MainDepth/MainNormal/SceneObjectId writes,
   ShadowDynamicMap read).
2. They share identical depth/blend state contracts (kPassAmbient row SceneGEqual/depthWrite=On).
3. The executor cares about resource ordering, not object type taxonomy.

---

## Q6 — Same-Order Ownership Readiness + Minimal First Slice

### MECHOPAQUE-SEAM-1 (minimal)

**Goal:** Make `RenderPassId::MechOpaque` executor-ownable with the same
`executorOwnBeginTopLevel`/`executorOwnEndTopLevel` pattern as StaticPropOpaque and Terrain.

**Files to touch (source changes only — this recon does NOT make them):**

| File | Change | Risk |
|---|---|---|
| `mclib/txmmgr.cpp:2362` | Retag preamble note: `"MC_TextureManager_renderLists(preamble-ADVISORY)"` or remove | Low |
| `mclib/txmmgr.cpp:2479` | Add `executorOwnBeginTopLevel(PassIdentity::OpaqueObject, ...)` just before `Render.3DObjects` zone | Low |
| `mclib/txmmgr.cpp:2544` (after 3DObjects end, before PostObjects zone) | Add `executorOwnEndTopLevel(PassIdentity::OpaqueObject, ...)` for MLR path | Low |
| `GameOS/gameos/gos_mech_batcher.cpp:1777` | Add `noteRenderPass(PassIdentity::OpaqueObject, "GpuMechBatcher::flush(entry)")` at flush() entry | Low |

**The Render.3DObjects begin/end bracket covers the aggregate (MLR mechs + vehicles + buildings).
The GpuMechBatcher::flush note at :1777 makes the GPU mech sub-draw visible to telemetry separately.**

**Do NOT add a separate begin/end around GpuMechBatcher::flush in txmmgr.cpp** — the flush is
already inside the broader OpaqueObject ownership bracket. The `executorOwnBeginTopLevel` at
Render.3DObjects entry is the top-level begin; the last object draw (GpuMechBatcher::flush end)
is the effective end. A single `executorOwnEndTopLevel` placed after txmmgr.cpp:3261 cleanly
closes the bracket.

**Gating:** Same `MC2_FRAMEGRAPH_EXECUTOR` gate as TerrainOverlay/TerrainDecal/StaticProp.

**After MECHOPAQUE-SEAM-1, MechOpaque can participate in the same-order executor as a
top-level-owned pass** — executorOwnBeginTopLevel marks the enter, executorOwnEndTopLevel marks
the exit, and the ordering check validates MechOpaque fires before StaticPropOpaque in the
kFramePassOrder sequence (as currently declared at RenderPassContract.h:396-398).

---

## Findings Summary

| # | Finding | Severity |
|---|---|---|
| 1 | txmmgr:2362 OpaqueObject note fires ~117 lines BEFORE any actual draw (preamble state-set only) | HIGH |
| 2 | OpaqueObject conflates mechs + vehicles + buildings in one pass — no separate vehicle/building batcher exists to split on | HIGH (lossy identity, acknowledged) |
| 3 | GpuMechBatcher::flush (the dominant mech draw) has NO noteRenderPass at its entry | HIGH |
| 4 | The preamble mis-placement is symmetric with SHADOW-OBSERVE-3's shadow note mis-placement | MED |
| 5 | Splitting MechOpaque into 3 would require 2 new RenderPassIds + 2 kFramePassOrder entries + kRenderPassIdCount=13 → high churn with no executor benefit | MED |
| 6 | StaticPropBatcher already uses executorOwnBeginTopLevel/EndTopLevel pattern — MechOpaque can adopt same pattern | INFO (positive: template exists) |

**DRIFT-PRONE lines (re-verify before coding slice):**
- txmmgr.cpp: 2362 (preamble note), 2479 (Render.3DObjects begin), 2544 (Render.3DObjects end),
  3241 (GpuSP.BatcherFlush begin), 3260 (GpuMechBatcher::flush call)
- gos_mech_batcher.cpp: 1777 (flush() entry)
- tgl.cpp: 3016 (TG_Shape::Render enqueue note)

---

*Recon complete. No source files modified.*
