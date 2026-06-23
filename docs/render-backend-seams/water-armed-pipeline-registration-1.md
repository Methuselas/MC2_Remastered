# WATER-ARMED-PIPELINE-REGISTRATION-1

**Status:** SHIPPED. Registers the **armed water fast-path base** as a DESCRIPTIVE
PipelineDesc row (`WaterArmed`) and moves Water UNREGISTERED → DESCRIPTIVE_REGISTERED
in the pass-coverage ledger. **No applyPipeline routing, no GL change, no new
fields, no shader change, no SPIR-V, no smoke.** Only the armed fast-path base is
modeled — the legacy quad fallback and the GPU-driven MDI sub-variant are excluded.

## Source-state verification (recon was WRONG — this is why we verify)

The pass-coverage recon recorded the armed water base as "**OPAQUE no-blend GEQUAL
depthwrite BACK**". Reading the actual code (`gosRenderer::renderWaterFastPath`,
`gameos_graphics.cpp:3275-3288`) shows it is **none of "opaque", none of "cull-back"**:

| Axis | Recon claim | **Actual (source)** | Citation |
|---|---|---|---|
| blend | opaque / no-blend | **AlphaBlend** `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` | `:3278-3279` |
| cull | Back | **None** (explicitly disabled) | `:3275` (CINEMATIC-WATER-CULL-1) |
| depth func | GEQUAL | GEQUAL ✓ | `:3277` |
| depth write | on | **on (default)** — `MC2_WATER_NO_DEPTH_WRITE` is a debug A/B gate only | `:3285-3288` (OOB-FOG-WATER-DEPTH-1) |
| depth test | — | on | `:3276` |

Program: `gos_terrain_water_fast.vert` + `gos_tex_vertex.frag` (writes color0
`FragColor` + color1 `GBuffer1`).

## The row

`PipelineId::WaterArmed=10` (`Count_=11`). DESCRIPTIVE (glProgramName=0, NOT
applyPipeline-routed; `renderWaterFastPath` hand-sets + save/restores its state).

```
blend=AlphaBlend  depthTest=on  depthWrite=on  depthFunc=GreaterEqual
cull=None  frontFace=Ccw  polygonOffsetEnable=false  color0+color1
```

No new field: `BlendMode::AlphaBlend` + `depthWriteEnable` already exist. This is
the second AlphaBlend pipeline (after TerrainDecal) — same factor, but depth-write
**on** (vs decal's off), so it also exercises the depthWrite axis under AlphaBlend.

## Verification

- **Full build green** (mc2, isolated worktree); `static_assert(s_descs==Count_=11)` holds.
- `pipeline_key` PASS, `pass_coverage` PASS, `pipeline_desc` PASS (2 benign WARN
  for TerrainOverlay/TerrainDecal; WaterArmed reported as descriptive sub-pass —
  the name-match heuristic, `pass_coverage` is the authority for Water→WaterArmed).
- **Adversarial:** dropping the Water ledger row → `pass_coverage` FAIL
  ("registered PipelineId 'WaterArmed' missing from ledger"); dropping the
  WaterArmed schema entry → `pipeline_key` FAIL ("missing from schema"). Restored PASS.
- Row matches source state (AlphaBlend / depthWrite on / GEQUAL / cull None).
- **No smoke** — descriptive-only, zero GL behavior change.

## Out of scope (held)
Legacy quad fallback (`quad.cpp:2612`, data-driven) · GPU-driven MDI sub-variant
(`gos_terrain_water_mdi.frag`) · water refraction/reflection · shader changes ·
SPIR-V · applyPipeline routing (the `next` step — more involved than terrain
because water save/restores its own state).

## Next (per the ledger)
VFX-PIPELINE-REGISTRATION-RECON-1 (recon, not build — blend-selector family).
