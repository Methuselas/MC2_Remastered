# PIPELINEKEY-VERTEXLAYOUT-AUTHORITY-1

**Status:** SHIPPED. Promotes the PipelineKey `vertexLayout` axis from **DESCRIPTIVE → AUTHORITATIVE** (recorded at runtime + statically checked). Narrow: no VAO/draw rewrite, no render-state change, no Vulkan, no runtime/visual change.

## What changed

The `vertexLayout` axis of the logical PipelineKey (see `pipeline-key-schema.json`)
previously carried only a free doc string (`static_prop_40B`, `mech_GpuMechVertex_48B`)
with no repo-owned id and no checker enforcement. It is now a fixed, finite,
repo-owned id space that is recorded in the pipeline-key path and cross-checked.

### Source of truth — `RenderCore::VertexLayoutId` (`RenderCore/PipelineRegistry.h`)

```cpp
enum class VertexLayoutId : uint32_t {
    Invalid          = 0,
    StaticProp40B    = 1,   // layout: static_prop_40B (gos_static_prop_batcher.cpp:2310-2323)
    MechGpuVertex48B = 2,   // layout: mech_GpuMechVertex_48B (gos_mech_batcher.cpp:1376-1382)
    Count_           = 3,
};
```

The `// layout: <token>` comment is the **authoritative** canonical name. A
`kVertexLayoutNames[]` table in `PipelineRegistry.cpp` mirrors it
(`static_assert` on `Count_`), exposed via `vertexLayoutName(id)`.

### Recorder (parallel to `recordPipelineVariantKey`)

`recordPipelineVertexLayout(PipelineId, VertexLayoutId)` / `getPipelineVertexLayout(PipelineId)`
store a per-`PipelineId` layout id (value-initialized to `Invalid`). **Pure
metadata — no GL state, no VAO, no draw touched.**

Call sites (at the existing `bindProgram` points, beside the variant-key recording):
- `gos_mech_batcher.cpp` — `MechOpaque` → `MechGpuVertex48B`. The `[PIPELINE_VARIANT]`
  stderr line now also prints `vertexLayout=…`, expressing that the **MechOpaque
  PipelineKey = (shaderVariantId + vertexLayoutId)**.
- `gos_static_prop_batcher.cpp` — `StaticPropOpaque` / `StaticPropAlphaTest` /
  `StaticPropDepth` → `StaticProp40B`.

### Schema + checker

- `pipeline-key-schema.json`: `fields[].vertexLayout` → `status: AUTHORITATIVE`,
  `authoritative: true`; each `registered_pipelines[]` gains a `vertexLayoutId`
  (the enum value name) alongside its `vertexLayout` string. Summary/answers
  text updated to move `vertexLayout` to the authoritative set.
- `check-pipeline-key.py` (the `pipeline_key` seam check) now parses
  `VertexLayoutId` from the header and **FAILS** on:
  1. `vertexLayout` field not declared `AUTHORITATIVE` (promotion regression),
  2. any enum value missing its `// layout:` canonical-name comment,
  3. a pipeline with **no `vertexLayoutId`** (missing id),
  4. a `vertexLayoutId` **absent from the enum** (stale/renamed),
  5. a `vertexLayout` string that **disagrees** with the enum's canonical name
     for its id (rename drift).

## Verification

- `check-pipeline-key.py` → PASS (0 fail, 0 warn).
- Adversarial: rename-drift, stale-id, and missing-id mutations each force exit 1
  with the expected FAIL message (then reverted).
- Full build (`--target mc2`, RelWithDebInfo) → exit 0 (header enum + recorder +
  call sites compile; `static_assert` holds).
- `check-contracts.sh --quiet` → `pipeline_key` PASS; the only fails are the 4
  pre-existing foreign-WIP checks (env_registry, include_firewall,
  no_raw_gl_from_game, render_contract_gbuf1).
- No smoke run: change is non-functional (metadata + one stderr line); no GL,
  draw, or visual path touched.

## Files

`RenderCore/PipelineRegistry.h`, `RenderCore/PipelineRegistry.cpp`,
`GameOS/gameos/gos_mech_batcher.cpp`, `GameOS/gameos/gos_static_prop_batcher.cpp`,
`scripts/check-pipeline-key.py`, `docs/render-backend-seams/pipeline-key-schema.json`,
this doc.

## Deferred (unchanged by this slice)

StaticProp SPIR-V family (depth-parity gate), material binding-5 SPIR-V, Vulkan
backend, RenderDevice, texture-manager, ssao_apply. The remaining DESCRIPTIVE
axis is `shaderVariantId` (awaits offline SPIR-V variant baking).
