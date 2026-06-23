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

## Current state (2026-06-23)

| Pass | Status | RenderPassId | PipelineId |
|---|---|---|---|
| StaticPropOpaque | ROUTED_BY_APPLYPIPELINE | StaticPropOpaque | StaticPropOpaque |
| StaticPropAlphaTest | DESCRIPTIVE_REGISTERED | StaticPropOpaque | StaticPropAlphaTest |
| StaticPropDepth | ROUTED_BY_APPLYPIPELINE | StaticPropOpaque | StaticPropDepth |
| MechOpaque | SPIRV_ELIGIBLE | MechOpaque | MechOpaque |
| ShadowTerrain / ShadowStaticProp / ShadowMech | VISUAL_PROVEN | Shadow | Shadow* |
| Terrain (main solid) | UNREGISTERED | Terrain | — |
| TerrainOverlay | UNREGISTERED | TerrainOverlay | — |
| TerrainDecal | UNREGISTERED | TerrainDecal | — |
| Water | UNREGISTERED | Water | — |
| VFX | UNREGISTERED | VFX | — |
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

1. **TERRAIN-OVERLAY-DECAL-PIPELINE-REGISTRATION-1** — TerrainOverlay + TerrainDecal
   UNREGISTERED → DESCRIPTIVE_REGISTERED (TerrainDecal = first AlphaBlend row).
2. **WATER-ARMED-PIPELINE-REGISTRATION-1** — Water armed fast-path base only.
3. **VFX-PIPELINE-REGISTRATION-RECON-1** — the blend-selector family (later).

DO NOT re-open: UI, Picking, RoadsRunways (terminal `DO_NOT_MODEL`).
