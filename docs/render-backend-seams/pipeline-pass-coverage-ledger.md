# Pipeline / Pass Coverage Ledger

**Source of truth:** [`pipeline-pass-coverage-ledger.json`](pipeline-pass-coverage-ledger.json)
(machine-read by `scripts/check-pass-coverage.py`, registered in
`check-contracts.sh` as `pass_coverage`). This `.md` is a human companion — edit
the JSON, not this file's tables.

## Why this exists

The PSO **state vocabulary** is nearly complete (see
`pipelinekey-remaining-state-gaps-recon-1.md`). The remaining Vulkan-prep work is
no longer "what fields are missing from `PipelineDesc`?" — it is "which ad-hoc
passes have not yet been indexed, registered, and routed?". This ledger gives
every render/pass/program family a **lifecycle status** so a slice MOVES a pass
one step forward instead of re-deciding from scratch whether it should be modeled
(no more re-opening UI / picking / roads every week).

## Lifecycle

```
UNREGISTERED → DESCRIPTIVE_REGISTERED → ROUTED_BY_APPLYPIPELINE → VISUAL_PROVEN → SPIRV_ELIGIBLE
                                                                                   (DO_NOT_MODEL = terminal, off to the side)
```

The proven per-pass arc: **descriptive registration → checker → applyPipeline
routing → pixel-identical proof** (→ SPIR-V later, behind a parity gate).

### `proofStatus` — for passes that can't byte-hash
A pass routed through `applyPipeline` whose output is **nondeterministic** (e.g.
VFX/particles: random spawn defeats a byte-hash A/B) may sit at
`ROUTED_BY_APPLYPIPELINE` with an explicit `proofStatus`:
`byte_identical` | `perceptual_ab` | `oracle_coverage` |
`nondeterministic_visual_gate_pending`. The checker **forbids `VISUAL_PROVEN`
while `proofStatus` is `*_pending`** — you cannot mark a pass proven on a gate
that hasn't been landed. This keeps "routed + correct by construction" honestly
distinct from "visually proven", instead of faking a byte-hash that can't hold.

## Current state (2026-06-23)

| Pass | Status | RenderPassId | PipelineId |
|---|---|---|---|
| StaticPropOpaque | ROUTED_BY_APPLYPIPELINE | StaticPropOpaque | StaticPropOpaque |
| StaticPropAlphaTest | DESCRIPTIVE_REGISTERED | StaticPropOpaque | StaticPropAlphaTest |
| StaticPropDepth | ROUTED_BY_APPLYPIPELINE | StaticPropOpaque | StaticPropDepth |
| MechOpaque | SPIRV_ELIGIBLE | MechOpaque | MechOpaque |
| ShadowTerrain / ShadowStaticProp / ShadowMech | VISUAL_PROVEN | Shadow | Shadow* |
| Terrain (main solid) | UNREGISTERED | Terrain | — |
| TerrainOverlay | ROUTED_BY_APPLYPIPELINE (proof: pass_not_exercised_in_smoke) | TerrainOverlay | TerrainOverlay |
| TerrainDecal | ROUTED_BY_APPLYPIPELINE (proof: pass_not_exercised_in_smoke) | TerrainDecal | TerrainDecal |
| Water (armed fast path) | VISUAL_PROVEN (proof: oracle_coverage) | Water | WaterArmed |
| VFX | VISUAL_PROVEN (proof: oracle_coverage) | VFX | Vfx{Billboard,Tube,Mesh}{Alpha,Additive} (6) |
| VegetationCards | UNREGISTERED | VegetationCards | — |
| PostProcess | UNREGISTERED | PostProcess | — |
| UI | DO_NOT_MODEL | UI | — |
| Picking | DO_NOT_MODEL | — | — |
| RoadsRunways | DO_NOT_MODEL | — | — |
| MineStatic | UNREGISTERED (needs RenderPassId) | — | — |

## What the checker enforces (FAIL)

1. Every ledger entry has a status in the allowed set.
2. `DO_NOT_MODEL` entries carry a `reason`.
3. Entry `pipelineId` / `renderPassId` exist in the C++ enums (no stale).
4. Every registered `PipelineId` appears in the ledger.
5. Every `RenderPassId` is classified by ≥1 entry (no unclassified family).
6. `ROUTED_BY_APPLYPIPELINE` / `VISUAL_PROVEN` / `SPIRV_ELIGIBLE` entries have
   real `applyPipeline(...PipelineId::<id>)` evidence in the sources.
7. `SPIRV_ELIGIBLE` entries name a baked SPIR-V family.

(WARN: an `UNREGISTERED` entry with no `next` step.)

## Next slices (use the ledger, don't re-recon)

1. ~~TERRAIN-OVERLAY-DECAL-PIPELINE-REGISTRATION-1~~ **DONE** — TerrainOverlay +
   TerrainDecal now DESCRIPTIVE_REGISTERED (TerrainDecal = first AlphaBlend row).
   Their `next` is route_applyPipeline → VISUAL_PROVEN.
2. ~~WATER-ARMED-PIPELINE-REGISTRATION-1~~ **DONE** — WaterArmed (armed fast-path
   base) now DESCRIPTIVE_REGISTERED. AlphaBlend, cull None, GEQUAL, depth-write
   (source-verified — NOT the recon's "opaque cull-back"). Legacy quad + MDI
   sub-variant not modeled.
3. **VFX-PIPELINE-REGISTRATION-RECON-1** — the blend-selector family (recon, not build).
4. (optional) route TerrainOverlay/TerrainDecal/WaterArmed through applyPipeline
   → VISUAL_PROVEN, when convenient (terrain are provably-no-op candidates; water
   save/restores its own state so it is more involved).

DO NOT re-open: UI, Picking, RoadsRunways (terminal `DO_NOT_MODEL`).
