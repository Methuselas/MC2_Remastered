# RECON — DRYRUN-OBSERVE-COVERAGE-1

Scout begin-sites and guard-safety for closing dry-run coverage 7/11 → 11/11.

> All `file:line` refs are drift-prone. Re-grep before any derived slice.
> Prior recon: `docs/render-backend-seams/frame-graph-executor-dryrun-recon-1.md` (`f1e8408f`).

---

## TL;DR

Three of the four invisible passes (**Water**, **VegetationCards**, **VFX/ParticleEffect**)
each have a single, clean, once-per-frame begin-site that takes a `noteRenderPass` call
safely with ZERO ambient/FBO guard-trip risk (no ambient contract, no declared FBO target →
observe-only-safe). **Shadow** is the hard case: it has two non-nesting passes
(`beginShadowPrePass` / `beginDynamicShadowPass`) that would each need a call, and the
ambient contract already declares `RenderPassId::Shadow` with `ColorMaskState::AllOff` and
`DepthWriteState::On` — adding a `noteRenderPass` call will START sampling the ambient probe
for shadow, which fires `compareAmbient()`. That is safe IF the live state at the
`beginShadowPrePass` call already matches the declaration; however, this must be verified
before ship. All four passes already have `PassIdentity` and `toRenderPassId` values — no
new enum values needed. The existing `FramePassEntry::fired` mechanism already carries the
observed/missing signal — no new field needed; the dump can derive the gap directly from
`unobservedCount` in `DryRunReport`.

---

## Per-Pass Summary Table

| Pass | Begin-site file:line | Single/multi callsite | PassIdentity exists? | Guard-safety verdict | Config-variance | Decision |
|---|---|---|---|---|---|---|
| Shadow | `gameos_graphics.cpp:6269` (`beginShadowPrePass`) + `:6481` (`beginDynamicShadowPass`) | **MULTI** — 2 non-nesting passes | `ShadowCaster` ✓ | **HIGH — NEEDS CONTRACT CHECK FIRST** | gate: `shadowsEnabled_` / `beginShadowPrePass` early-returns when disabled | INSTRUMENT (with once-per-frame latch; verify ambient match first) |
| Water | `gameos_graphics.cpp:3132` — `gosRenderer::renderWaterFastPath()` body entry | **SINGLE** — called via `land->renderWaterFastPath()` at `gamecam.cpp:530` or `simplecamera.cpp:261`; self-guards `if (!water_fast_prog_ ... || recordCount == 0) return` | `Water` ✓ | SAFE-OBSERVE-ONLY | `MC2_RENDER_WATER_FASTPATH` gate; no-op legacy path → UNOBSERVED-this-frame if off | INSTRUMENT |
| VegetationCards | `gos_vegetation.cpp:378-381` — `GosVegetation::flush()` before `assertPassContract` | **SINGLE** — called via `GameAdapters::Vegetation::flush()` → `GosVegetation::flush()` once at `gamecam.cpp:554` | `VegetationCards` ✓ | SAFE-OBSERVE-ONLY | `MC2_VEGETATION_CARDS` gate; outer adapter early-returns if `!isEnabled()` → UNOBSERVED-this-frame | INSTRUMENT |
| VFX/ParticleEffect | `gamecam.cpp:585` — `mc2::particles::Batcher::Instance().Flush()` | **SINGLE** — one `Flush()` call per frame; `gos_tube_ribbon_flush_deferred()` is a separate effect sub-type, same frame phase | `ParticleEffect` ✓ | SAFE-OBSERVE-ONLY | `MC2_GPU_PARTICLES` gate (default ON); no-op frames when no particles enqueued | INSTRUMENT |

---

## Q1 — Begin Point Per Pass

### Shadow

`gosRenderer::beginShadowPrePass()` at `GameOS/gameos/gameos_graphics.cpp:6228`.
The function guards on `!pp || !pp->shadowsEnabled_ || !shadow_terrain_material_` and
returns early — no shadow draw at all. The existing `assertPassContract(ShadowCaster, ...)`
at line 6269 fires unconditionally inside that function (after the early-return guard) and
is the natural home for `noteRenderPass(PassIdentity::ShadowCaster, ...)` alongside it.

There is also `gosRenderer::beginDynamicShadowPass()` at `gameos_graphics.cpp:6481`
(called at `:9588`) for the dynamic/CSM shadow pass. Both are non-nesting (terrain static
then dynamic), so naively adding a call to each would record `ShadowCaster` twice per frame.

### Water

`gosRenderer::renderWaterFastPath()` at `GameOS/gameos/gameos_graphics.cpp:3132`.
Called once per frame from `gamecam.cpp:530` (`land->renderWaterFastPath()`) or
`simplecamera.cpp:261`. The function self-guards:

```cpp
if (!water_fast_prog_ || !water_fast_prog_->shp_ || recordCount == 0) return;
```

`noteRenderPass(PassIdentity::Water, "renderWaterFastPath")` belongs immediately after
these guards, at approximately line 3155.

### VegetationCards

`GosVegetation::flush()` at `GameOS/gameos/gos_vegetation.cpp`. The function already has
`assertPassContract(PassIdentity::VegetationCards, "GosVegetation::flush")` at line 379.
A `noteRenderPass(PassIdentity::VegetationCards, "GosVegetation::flush")` alongside it (or
immediately before) is the correct location. The outer `GameAdapters::Vegetation::flush()`
at `GameAdapters/VegetationAdapter.cpp:482` early-returns `if (!GosVegetation::isEnabled())`,
so the call only fires when vegetation actually draws.

### VFX / ParticleEffect

`mc2::particles::Batcher::Instance().Flush()` is called once per frame at
`code/gamecam.cpp:585`. There is no single engine-layer entry function with a natural
`noteRenderPass` callsite inside the GPU particle system (batcher.cpp does not include
render_contract.h). The simplest and safest placement is **at the call-site** in
`gamecam.cpp`, immediately before the `Flush()` call:

```cpp
// proposed insertion point — gamecam.cpp ~:584
render_contract::noteRenderPass(render_contract::PassIdentity::ParticleEffect,
                                "gamecam::particlesFlush");
::mc2::particles::Batcher::Instance().Flush();
```

`gos_tube_ribbon_flush_deferred()` (line 594) is a supplementary ribbon sub-flush of the
same frame's VFX pass — it does NOT need a second `noteRenderPass` call because the
`recordPassFired()` kernel ignores duplicate fires (`if (e.fired) return;`).

---

## Q2 — Single vs Multi Callsite

| Pass | Multi? | Guard needed? |
|---|---|---|
| Shadow | YES — `beginShadowPrePass` + `beginDynamicShadowPass` fire in sequence each frame both draw into shadow atlas | A once-per-frame latch is needed: record `ShadowCaster` only on the FIRST begin (`beginShadowPrePass`). `beginDynamicShadowPass` should NOT call `noteRenderPass` (duplicate fires are ignored by the kernel, but the ambient probe fires again — that is wasteful and would double the GL-sample cost). |
| Water | NO — single site | None needed |
| VegetationCards | NO — single site | None needed |
| VFX | NO — single site (one `Flush()` per frame; tube ribbon is same pass, second call ignored by kernel) | None needed for the kernel; skip second call for probe efficiency |

---

## Q3 — PassIdentity Coverage

All four passes already have `PassIdentity` entries in `mclib/render_contract.h:40-56` and
`toRenderPassId()` mappings in `mclib/render_contract.cpp:545-562`:

| PassIdentity | toRenderPassId maps to |
|---|---|
| `ShadowCaster` | `R::Shadow` |
| `Water` | `R::Water` |
| `VegetationCards` | `R::VegetationCards` |
| `ParticleEffect` | `R::VFX` |

**No new enum values needed.**

---

## Q4 — Guard-Surface Safety

### ambient_contract.h (`kPassAmbient[]`)

Only five passes have an `AmbientContract` row (lines 77–118):
`Shadow`, `StaticPropOpaque`, `Terrain`, `MechOpaque`, `PostProcess`.

| Pass | AmbientContract row? | Declared axes |
|---|---|---|
| **Shadow** | **YES** | `ColorMaskState::AllOff`, `DepthFuncState::ShadowLess`, `ViewportKind::ShadowMap`, `DepthWriteState::On` |
| Water | NO | — |
| VegetationCards | NO | — |
| VFX/ParticleEffect | NO | — |

### fbo_ledger.h (`kPassFboTarget[]`)

`kPassFboTarget[]` (lines 58–63) has only: `StaticPropOpaque`, `Terrain`, `TerrainOverlay`,
`TerrainDecal` — all mapped to `MainColor`. None of the four invisible passes are declared.

| Pass | kPassFboTarget entry? |
|---|---|
| Shadow | NO |
| Water | NO |
| VegetationCards | NO |
| VFX/ParticleEffect | NO |

### Guard-safety verdicts

**Water, VegetationCards, VFX: SAFE-OBSERVE-ONLY.**
`ambientProbeAtPassBegin(toRenderPassId(id))` calls `findAmbient(id)` — returns `nullptr`
for these three → no ambient compare. `declaredFboTarget(id)` returns `Unknown` → FBO guard
skips. The call is pure observe-only; no new guard surfaces activated.

**HIGH — Shadow: NEEDS CONTRACT CHECK FIRST.**
`findAmbient(RenderPassId::Shadow)` returns the declared contract row. Adding
`noteRenderPass(ShadowCaster, ...)` will START calling `ambientProbeAtPassBegin(Shadow)`,
which samples `glGet*` and calls `compareAmbient()` against the declared contract
(`AllOff / ShadowLess / On`). If the live GL state at `beginShadowPrePass` already matches
exactly what the ambient contract declares, this is safe. However:

- The probe samples at *entry*, before the shadow pass establishes its own state
  (`applyPipeline(ShadowTerrain)` runs after `captureShadowGLState` at line 6264).
- The declared `colorMaskOnEntry = AllOff` models the state this pass *establishes* (disables
  color write), not the state on *entry* to the function. At entry, `captureShadowGLState`
  captures the *caller's* state, which may have `AllOn`. This semantic mismatch is exactly
  the caveat documented in `ambient_contract.h:138-140`:
  > "colorMaskOnEntry models the state a pass ESTABLISHES, which for terrain is re-asserted
  > mid-pass. Sampling at beginPass-ENTRY may therefore legitimately differ."
- The `compareAmbient` comparator (lines 152-167) checks `decl != Inherit && live != Inherit`
  before flagging. Since the live sample at shadow entry may be `AllOn` (scene color write
  still enabled) but the declared value is `AllOff`, a **colorMask mismatch** will fire
  unless the ambient probe is measure-only (not CI-fatal). Checking:
  `render_contract.cpp` ambient probe is diagnostic / logged — not `abort()` / not
  CI-fatal in the current default-ON guard path. Still, adding this call will generate
  spurious ambient mismatch log lines on every shadow-instrumented frame.

**Recommendation:** place `noteRenderPass(ShadowCaster, ...)` AFTER `applyPipeline` and the
depth clear (i.e., after line 6268, where `assertPassContract` already sits), not at
function entry. At that point the shadow state is established (`AllOff` / `ShadowLess`) and
the ambient sample will match the declaration. This also respects the `once-per-frame` latch
requirement (add only in `beginShadowPrePass`, skip `beginDynamicShadowPass`).

---

## Q5 — Config/Path Variance

| Pass | Variance | Handling |
|---|---|---|
| Shadow | Disabled when `!shadowsEnabled_` or `!shadow_terrain_material_`; `beginShadowPrePass` early-returns → no call fires → UNOBSERVED-this-frame | Existing `unobservedCount` mechanism handles correctly |
| Water | `MC2_RENDER_WATER_FASTPATH` gate; when unset, legacy water drains inside `renderLists()` (no `renderWaterFastPath` call → no note); `recordCount == 0` early-return also suppresses | UNOBSERVED-this-frame correctly |
| VegetationCards | `MC2_VEGETATION_CARDS` gate (default OFF); outer adapter `isEnabled()` check prevents the call | UNOBSERVED-this-frame correctly |
| VFX/ParticleEffect | `MC2_GPU_PARTICLES` gate (default ON); frames with no enqueued particles: `Flush()` is still called but submits no draw calls — call fires regardless → `ShadowCaster` recorded even on empty-particle frames. This is **correct** (the pass was entered; it just had nothing to draw). | No special handling needed |

All four: when a pass legitimately does not fire, the `noteRenderPass` call either never
runs (early-return guards) or the gate keeps the outer function from being called. The
`unobservedCount` in `DryRunReport` is already the correct classification ("not observed
this frame") and produces ZERO false alarms.

---

## Q6 — Exclusion Candidates

None of the four warrant exclusion. Verdicts:

| Pass | INSTRUMENT vs EXCLUDE | Reasoning |
|---|---|---|
| Shadow | **INSTRUMENT** (with once-per-frame latch + placement after `applyPipeline`) | Single begin-site inside `beginShadowPrePass`; the ambient guard trip is avoidable by correct placement |
| Water | **INSTRUMENT** | Single clean begin-site; observe-only-safe |
| VegetationCards | **INSTRUMENT** | Single clean begin-site; `assertPassContract` already there |
| VFX/ParticleEffect | **INSTRUMENT** | Single clean call-site in gamecam.cpp |

---

## Q7 — Observed/Missing Surfacing

`FramePassTrace::entries[i].fired` already carries per-slot observed/unobserved state.
`DryRunReport::unobservedCount` accumulates the gap count. `DryRunReport::firedCount`
gives the observed total. After this slice ships all four callsites, the dump/MCP
`frame_graph_dryrun` block can derive:

- `firedCount` = N of 11 passes observed this frame
- `unobservedCount` = 11 - firedCount (legitimately absent passes, e.g. VegetationCards
  when gate is OFF)

**No new field needed.** The existing `FramePassEntry::fired` bitmask derivable from
`entries[0..10].fired` already provides per-pass granularity. If the MCP wants a named
per-pass list, it can map slot index → `kFramePassOrder[i]` name — all data is present.

---

## SLICE PROPOSAL (not built)

### Files to edit

1. **`GameOS/gameos/gameos_graphics.cpp`** — `gosRenderer::beginShadowPrePass()` (line ~6269):
   Add `render_contract::noteRenderPass(render_contract::PassIdentity::ShadowCaster, "beginShadowPrePass")` AFTER the existing `assertPassContract(ShadowCaster, ...)` call (which already sits after `applyPipeline`). Do NOT add a call to `beginDynamicShadowPass`.

2. **`GameOS/gameos/gameos_graphics.cpp`** — `gosRenderer::renderWaterFastPath()` (line ~3155):
   Add `render_contract::noteRenderPass(render_contract::PassIdentity::Water, "renderWaterFastPath")` immediately after the early-return guards (`if (!water_fast_prog_ ... || recordCount == 0) return;`).

3. **`GameOS/gameos/gos_vegetation.cpp`** — `GosVegetation::flush()` (line ~378):
   Add `render_contract::noteRenderPass(render_contract::PassIdentity::VegetationCards, "GosVegetation::flush")` alongside the existing `assertPassContract(VegetationCards, ...)`. Already includes `render_contract.h` (confirmed at gos_vegetation.cpp header).

4. **`code/gamecam.cpp`** — particle flush block (line ~584):
   Add `render_contract::noteRenderPass(render_contract::PassIdentity::ParticleEffect, "gamecam::particlesFlush")` before `::mc2::particles::Batcher::Instance().Flush()`. Requires `#include "../mclib/render_contract.h"` if not already present (check before adding).

### New PassIdentity values

None required.

### Once-per-frame guard

Shadow only: `beginShadowPrePass` fires once; `beginDynamicShadowPass` fires zero or more
times per frame. The `recordPassFired()` kernel's `if (e.fired) return;` guard already
deduplicates, so a defensive `noteRenderPass` call in `beginDynamicShadowPass` would be
harmless to the kernel but wasteful for the ambient probe. The correct approach is to
add the call ONLY to `beginShadowPrePass` and omit it from `beginDynamicShadowPass`.

Water, VegetationCards, VFX: no guard needed (each fires at most once per frame by
construction).

### Observed/missing surfacing

No new fields. Post-slice: `DryRunReport::unobservedCount` should drop from 4 to 0 on
frames where all four passes fire; remain > 0 on frames where a gated pass is absent
(shadow disabled, vegetation gate OFF, water legacy path). The MCP `get_frame_context`
block already surfaces `unobservedCount` and `firedCount` from the dryrun telemetry
counters at `render_contract.cpp:794-800`.

### Verification plan

1. **out_of_order == 0 preserved**: Shadow records first in declared order (slot 0 of
   `kFramePassOrder`); water/veg/vfx occupy slots 6–8. All four fire after the passes they
   depend on (MechOpaque etc.), so no new out-of-order events are expected. Smoke gate
   confirms.

2. **No new ambient/FBO mismatches**: Water/VegetationCards/VFX have no ambient contract or
   FBO target — probe is skipped for all three. Shadow placement AFTER `applyPipeline`
   ensures the live state at sample time matches `AllOff / ShadowLess / On`. Verify by
   checking ambient-mismatch log output on a smoke run with `MC2_AMBIENT_PROBE=1`.

3. **Smoke 2/2 OFF and ON**: Run smoke tier1 with gate OFF (default, `MC2_FRAMEGRAPH_DRYRUN`
   unset) — byte-identical, zero log lines from dryrun. Run with gate ON and confirm
   `mc2_framegraph_dryrun_unobserved()` drops from 4 toward 0 on a full-mission frame
   (exact value depends on which gated passes fired that frame).

4. **Telemetry dump**: `MC2_RENDER_PASS_TELEMETRY=1` should now emit `[RENDER_PASS v1]` lines
   for all 11 passes (subject to gate configuration) on sampled frames.
