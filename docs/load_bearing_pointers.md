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

## Stock-install / build platform

- **Stock install must remain playable** (`memory/stock_install_must_remain_playable.md`): renderer modernization data must degrade to stock-compatible; no savegame depends on render caches.
- **Path separator** (`memory/mc2_path_separator_linux_build.md`): engine builds with `-DLINUX_BUILD`; `PATH_SEPARATOR` is `/`. Never hardcode `\\` against `_WIN32`.
