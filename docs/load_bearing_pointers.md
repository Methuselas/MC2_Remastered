# Load-bearing memory pointers (read before touching the area)

Extracted from CLAUDE.md 2026-05-24. These memory files contain non-obvious
constraints that govern specific subsystems. Read them BEFORE editing in the
named area, not as part of post-hoc debugging.

## Render / object lifecycle

- **Cull gates** (`memory/cull_gates_are_load_bearing.md`): `inView` / `canBeSeen` / `objBlockInfo` / `objVertexActive` gate `update()` AND allocation AND lifecycle. Mechs are the canary.
- **Render funcs are enqueuers** (`memory/render_functions_are_enqueuers_not_submitters.md`): `XXX::render()` enqueues into `MC_TextureManager`. Actual GL submission = `gos_RendererEndFrame -> renderLists()`. MLR objects are the immediate-draw exception.
- **PAUSE / UNPAUSE diagnostic** (`memory/pause_unpause_diagnostic_for_static_render_bugs.md`): bug clears on pause and returns on unpause = `mcTextureManager->update()` cache eviction without `objectManager->update` re-cache.

## TGL pools / shape rendering

- **TGL pool exhaustion is silent** (`memory/tgl_pool_exhaustion_is_silent.md`): `getVerticesFromPool` returns NULL = shapes vanish. Pools at 500K.

## Texture manager / retirement debt

- **Dual-queue retirement debt** (`memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`): legacy `masterVertexNodes` + modern `masterHardwareVertexNodes` coexist; every mod slice has been ADDITIVE; retire legacy soon.

## GPU-direct fast paths

- **GPU-direct bring-up checklist** (`memory/gpu_direct_renderer_bringup_checklist.md`): 10 traps every new fast path hits. READ FIRST before any GPU-direct renderer.

## GPU cull

- **GPU cull predicate is HELPER** (`memory/gpu_cull_predicate_is_helper_real_consumer_is_cullubo.md`): `shaders/gpu_cull_predicate.glsl` declares no uniforms; takes `vec4 clip` as input. Real matrix consumer is `CullUBO.viewProj` at `gpu_cull.comp:169-170`, written from `gpu_cull_compute.cpp:831` via cache. Migration work targeting the predicate file is fictional.

## MaterialGpu / DrawPacket producer-consumer (read before touching albedo wiring)

`albedoTex` is an **overloaded, transitional field** — its meaning differs by subsystem:

| Subsystem | `albedoTex` meaning | Shader-actionable? |
|---|---|---|
| **Static props** | Texture array layer index | YES — `static_prop.frag` samples `u_texArray[albedoTex]` via MaterialGpu by default |
| **Mechs** | `mcTextureManager` texHandle/slot | NO — compare-only; NOT usable as array index in `mech.frag` |

**Static-prop pipeline (DEFAULT-ON as of v7):**
- Producer: `GpuStaticPropBatcher::finalizeGeometry` → builds `MaterialGpu` table
- Dispatch: `DrawPacket[]` + `StaticPropDispatchMeta[]` per flush; kill-switch `MC2_STATIC_PROP_LEGACY_DISPATCH=1`
- Consumer: `static_prop.frag` — samples `materials[materialIdx].albedoTex` as array layer
- Fallback compare authority: `texArrayLayer` (retained for kill-switch path + invariant checks)
- Kill switches: `MC2_STATIC_PROP_LEGACY_DISPATCH=1` (dispatch), `MC2_MATERIAL_GPU=0` (table upload), `MC2_MATERIAL_GPU_SAMPLE=0` (shader sampling)

**Mech pipeline (Mech-2 BLOCKED):**
- Producer: `GpuMechInstance.materialIdx` at byte 52 — holds compare value only
- Consumer: `mech.frag` still uses legacy `sampler2D u_tex` (NOT MaterialGpu table)
- Mech-2 requires a texture model decision before shader sampling can proceed
- Do NOT copy the static-prop shader switch directly — `albedoTex` is a texHandle/slot here, not an array layer

## Stock-install / build platform

- **Stock install must remain playable** (`memory/stock_install_must_remain_playable.md`): renderer modernization data must degrade to stock-compatible; no savegame depends on render caches.
- **Path separator** (`memory/mc2_path_separator_linux_build.md`): engine builds with `-DLINUX_BUILD`; `PATH_SEPARATOR` is `/`. Never hardcode `\\` against `_WIN32`.
