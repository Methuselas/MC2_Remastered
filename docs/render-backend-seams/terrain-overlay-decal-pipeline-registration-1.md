# TERRAIN-OVERLAY-DECAL-PIPELINE-REGISTRATION-1

**Status:** SHIPPED. Registers `TerrainOverlay` + `TerrainDecal` as **DESCRIPTIVE**
PipelineDesc rows and moves them UNREGISTERED → DESCRIPTIVE_REGISTERED in the
pass-coverage ledger. **No applyPipeline routing, no GL behavior change, no new
PipelineDesc fields, no smoke.** TerrainDecal is the **first AlphaBlend** pipeline
row — it validates that the blend-factor axis generalizes beyond Opaque/AlphaTest.

## Verified pass state (read from source, not assumed)

The recon's "GEQUAL" was correct; the earlier `glDepthFunc(GL_LESS)` grep hits
(gameos_graphics.cpp:9765/9958) are the **restore-after** lines, not draw state.

| Pass | Function | Program | Blend | Depth | Cull | Color |
|---|---|---|---|---|---|---|
| **TerrainOverlay** | `drawTerrainOverlays` (`gameos_graphics.cpp:9732-9736`) + `drawDecalStaticBatch` (`:9850-9854`) | `terrain_overlay.vert`+`.frag` | Opaque (`glDisable(GL_BLEND)`) | test on, **GEQUAL**, write on | None (disabled) | color0+color1 (frag `location=0,1`) |
| **TerrainDecal** | `drawDecals` (`gameos_graphics.cpp:9924-9929`) | `terrain_overlay.vert`+`decal.frag` | **AlphaBlend** (`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`, `FUNC_ADD`) | test on, **GEQUAL**, **write OFF** | None (disabled) | color0+color1 |

## What changed

- **`RenderCore/PipelineRegistry.h`** — `TerrainOverlay=8`, `TerrainDecal=9`
  (`Count_=10`). DESCRIPTIVE (glProgramName=0, NOT routed; state still hand-set in
  the draw functions).
- **`RenderCore/PipelineRegistry.cpp`** — rows [8]/[9]. No new field: `BlendMode::AlphaBlend`
  and `depthWriteEnable=false` already exist. `static_assert(s_descs==Count_)` holds.
- **`pipeline-key-schema.json`** — two `registered_pipelines` entries; TerrainDecal's
  `blendState` carries `{factor: AlphaBlend, srcFactor: SRC_ALPHA, dstFactor:
  ONE_MINUS_SRC_ALPHA, equation: FUNC_ADD}` (explicit alpha-blend representation).
  Removed both from `excluded_draw_families`.
- **`pipeline-pass-coverage-ledger.json` + `.md`** — both UNREGISTERED →
  DESCRIPTIVE_REGISTERED, `pipelineId` set, `next = route_applyPipeline`.

No checker code change needed: the enum-vs-schema and enum-vs-ledger coverage
checks pick the new ids up automatically; `check-pipeline-desc.py`'s positional
field list is unchanged (no new field).

## Verification

- **Full build green** (mc2, isolated worktree); `static_assert(s_descs==Count_)` (10) holds.
- `pipeline_key` PASS, `pass_coverage` PASS, `pipeline_desc` PASS (2 benign WARN:
  "TerrainOverlay/TerrainDecal have a RenderPassId but pipelineDescRegistered=false"
  — the accurate descriptive-not-yet-routed state, the `next` step).
- **Adversarial:** dropping the TerrainDecal ledger entry → `pass_coverage` FAIL
  ("registered PipelineId 'TerrainDecal' missing from ledger"); dropping the
  TerrainDecal schema entry → `pipeline_key` FAIL ("missing from schema"). Both
  restored PASS.
- **Alpha-blend explicitly represented:** schema `blendState.factor=AlphaBlend`
  (+ src/dst/equation) and `PipelineDesc` `BlendMode::AlphaBlend`.
- **No smoke** — descriptive-only, zero GL behavior change (rows not applied;
  glProgramName=0; draw functions untouched).

## Out of scope (held)
No terrain main solid pass · no water · no VFX · no UI/HUD · no new fields · no
applyPipeline routing (the `next` step — a provably-no-op routing candidate, to
be done later behind a pixel gate).

## Next (per the ledger)
WATER-ARMED-PIPELINE-REGISTRATION-1 → VFX-PIPELINE-REGISTRATION-RECON-1. Optional:
route TerrainOverlay/TerrainDecal through applyPipeline → VISUAL_PROVEN.
