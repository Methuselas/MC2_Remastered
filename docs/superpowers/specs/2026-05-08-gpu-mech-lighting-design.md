# Track D Slice B1 — GPU Mech Lighting Design

**Date:** 2026-05-08
**Branch:** `claude/gpu-mech-batcher`
**Predecessor:** Slice A (GPU mech batcher, opt-in via `MC2_GPU_MECHS=1`)
**Memory pin:** [`track_d_slice_a_shipped.md`](../../../../../C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/track_d_slice_a_shipped.md)

## Goal

Replace Slice A's flat `baseLight = vec3(1.0)` in `shaders/mech.vert` with per-vertex GPU lighting via `calc_light(int lights_index, vec3 normal, vec3 world_pos, vec3 base_light)` from `shaders/include/lighting.hglsl`, mirroring `static_prop.vert`'s Stage 2.C.2 flip. Mechs in `MC2_GPU_MECHS=1` mode currently render textured-but-unlit; B1 lights them.

## Guiding principle (from user)

> Cheap on the GPU, flexible, not blocking future upgrades.

Concretely: VS-side calc_light (per-vertex, not per-pixel), separate killswitch for incremental rollout/bisect, env-gated capacity trace, no propagation of lightsOut/window flags that mechs don't currently use.

## Architecture

### Data flow

```
Mech3DAppearance::update()  (mclib/mech3d.cpp)
    └─ if (g_useGpuMechs)                                    [Slice B1 add]
         mechShape->CacheGpuLightData()                       [existing, msl.cpp:1828]
            ├─ scans listOfShapes for first SHAPE_NODE leaf
            ├─ leaf->GatherGpuObjectLightDataOnly()           [existing, tgl.cpp]
            │    └─ writes ObjectLights entry in
            │       MC_TextureManager::lightData_[]
            │    └─ uploads to LightsData UBO (binding=0)
            │       via mcTextureManager->update()
            └─ caches the dedup index in
               TG_MultiShape::cachedGpuLightIndex_

Mech3DAppearance::render()  (mclib/mech3d.cpp)
    └─ desc.lightDataIndex = mechShape->getCachedGpuLightIndex()  [B1 wire]
       (falls back to 0 if sentinel 0xFFFFFFFFu — unlikely with
        the unconditional cache call in update())

GpuMechBatcher::flush()  (gos_mech_batcher.cpp)
    └─ inst.lightDataIndex = desc.lightDataIndex                  [already plumbed]
       (no shader-side change to the SSBO write path)

shaders/mech.vert
    └─ if (u_lightingMode != 0)                                   [B1 new uniform]
         vec3 worldPos = worldMC2;                                [already computed]
         vec3 base = vec3(0.35);                                  [B1 ambient floor]
         vec3 litRGB = calc_light(int(inst.lightDataIndex),
                                  worldNormal,
                                  worldPos,
                                  base);
         baseLight = clamp(litRGB + inst.aRGBHighlight.rgb
                                  * inst.aRGBHighlight.a, 0, 1);
       else
         baseLight = vec3(1.0);   // Slice-A passthrough preserved

       v_litColor = vec4(baseLight, 1.0);
```

The `LightsData` UBO at `LIGHT_DATA_ATTACHMENT_SLOT=0` (defined in
`shaders/include/lighting.hglsl:11`) is bound **once** at session
start by `MC_TextureManager::init` (`mclib/txmmgr.cpp:318`) and
re-bound only when capacity grows (`mclib/txmmgr.cpp:1390`). Both
static_prop and mech share the same binding without conflict.

### Components

#### `shaders/mech.vert` — modified
- Add `#define MC2_MECH_LIGHTING` → `#include <include/lighting.hglsl>` (mirroring static_prop's `#define MC2_STATIC_PROP_LIGHTING`).
- Add `uniform int u_lightingMode;` (0 = Slice A passthrough, 1 = calc_light enabled). Sourced from `MC2_GPU_MECH_LIGHTING` env var on the C++ side.
- Replace the `baseLight = vec3(1.0)` block with the conditional shown above.

#### `mclib/mech3d.cpp` — modified
- `Mech3DAppearance::update()`: after the existing `updateGeometry()`, add `if (g_useGpuMechs && mechShape) mechShape->CacheGpuLightData();`. Mirrors `bdactor.cpp:2314` pattern.
- `Mech3DAppearance::render()`: change `desc.lightDataIndex = 0;` to `desc.lightDataIndex = mechShape->getCachedGpuLightIndex();` (falls back to 0 sentinel if uncached).

#### `GameOS/gameos/gos_mech_batcher.cpp` — modified
- Add `s_loc_u_lightingMode = loc("u_lightingMode");` to the cached uniform locations.
- Read `MC2_GPU_MECH_LIGHTING` env var at process start: `bool g_useGpuMechLighting = (getenv("MC2_GPU_MECH_LIGHTING") != nullptr);` (separate killswitch from `g_useGpuMechs`).
- In `flush()`: `glUniform1i(s_loc_u_lightingMode, g_useGpuMechLighting ? 1 : 0);`.

#### `mech.frag` — modified
- Add debug mode 9: `c = vec4(v_litColor.rgb / max(tex_color.rgb, vec3(0.001)), 1.0)` — visualizes the lighting-only contribution (lit color divided by texture so texture detail is removed and only the per-light tint shows). Helps bisect lighting-vs-texture issues. Existing modes 1–8 untouched.

#### `MC2_MECH_LIGHT_TRACE` instrumentation
- Add to `gos_mech_batcher.cpp` at flush time: enumerate this frame's submitted actors' `lightDataIndex` values; if `>= 32` (the `LightsData[32]` UBO cap) emit `[MECHLIGHT v1] event=cache_full count=N` once per cache-full event.
- Default off; env-gated by `MC2_MECH_LIGHT_TRACE=1`.
- When `cache_full` fires, the recipe is to raise the cap to 64 in C++ (`TG_HWLightsData`) and GLSL (`LightsData[64]`) in lockstep per `memory/cpp_glsl_ubo_struct_lockstep.md`.

## Killswitches

Three independent switches give bisect granularity:

| Env | Default | Behavior when set |
|---|---|---|
| `MC2_GPU_MECHS` | off | Enables GPU mech batcher path entirely (Slice A). |
| `MC2_GPU_MECH_LIGHTING` | off | Enables calc_light in mech.vert (B1). Requires `MC2_GPU_MECHS=1` to take effect. |
| `MC2_MECH_LIGHT_TRACE` | off | Emits `[MECHLIGHT v1]` capacity-audit lines. |

**Why three:** if B1 ships and a regression surfaces, flipping just `MC2_GPU_MECH_LIGHTING=0` retains GPU mech rendering with Slice A's flat-white lighting (still better than CPU mech path performance). The slider-style escalation lets soak operators bisect cleanly.

## Failure modes & error handling

- **Sentinel `0xFFFFFFFFu` from `getCachedGpuLightIndex()`** — happens if `CacheGpuLightData()` was skipped (killswitch off or actor not yet updated). Fall back to `desc.lightDataIndex = 0`. The shader's `calc_light(0, ...)` reads UBO slot 0 which is the engine's default ambient; visually equivalent to Slice A flat-white minus the ambient term. Safe.
- **`lightDataIndex >= 32`** — UBO out-of-bounds read. Spec-undefined per std140; AMD typically returns zeros. Visible as flat-black mech for that actor. The `MC2_MECH_LIGHT_TRACE` instrumentation surfaces this; if observed, raise the cap.
- **`cachedGpuLightIndex_` stale across LOD swap or replacement** — addressed by Slice A black-tree-bug fix pattern (`memory/black_tree_bug_investigation_state.md`): `CacheGpuLightData()` re-runs every frame in `update()`, refreshing the cache before render. No additional plumbing needed.

## Testing

- **Smoke** (mc2_01 30s, `MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1`): PASS, +0 destroys, no GL errors, `[MECHBATCHER v1] event=summary fallback_total=0`.
- **Operator visual A/B/C canary**:
  - `MC2_GPU_MECHS=0` (CPU baseline) — reference.
  - `MC2_GPU_MECHS=1` only — Slice A flat-white-lit.
  - `MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1` — B1 calc_light.
  Operator confirms B1 visually matches CPU baseline within "GPU lit" tolerance: mechs in shadow are darker; mechs lit by powerplants/spotlights show colored highlights.
- **Capacity trace** (`MC2_MECH_LIGHT_TRACE=1` on tier1 5/5): no `event=cache_full` lines.
- **Tier1 5-mission run** at all three killswitch combinations.

## Slice B1 gate (mirrors Slice A)

- Tier1 5/5 PASS at `MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1`, +0 destroys.
- Operator visual canary explicit pass.
- `[MECHBATCHER v1] event=summary fallback_total=0` across full smoke.
- No `[MECHLIGHT v1] event=cache_full` events.
- Adversarial review verdict ≠ STOP-THE-LINE.
- Memory file written: `memory/track_d_slice_b1_shipped.md` + MEMORY.md index.
- Slice B1 ships default-off (`MC2_GPU_MECH_LIGHTING=0` by default). Default-on flip is post-soak, separate slice.

## Out of scope for B1

- **Per-pixel (FS) calc_light** — quality bump, but per-vertex matches static_prop's gold-standard and is cheap. Revisit if banding observed at smooth-shaded normals.
- **`lightsOut` / `isWindow` propagation** — explicitly deferred to B+. NOTE: shutdown/powerup mech states are a real gameplay feature in MC2 (mechs can be powered down via Shutdown command, restored via Startup). The CPU mech rendering path's relationship to those states isn't carried into B1's GPU calc_light path — mechs in shutdown won't render dark in B1. B+ wires this through `GpuMechSubmitDesc::renderFlags` bit 1 once a per-actor `lightsOut`-equivalent signal is sourced from `Mech3DAppearance` (likely `pilotState`/`status` or a powerdown-specific flag). Do not regard B1 as a faithful shutdown-state visual until that follow-up.
- **Skinning** — vertex format already skinning-ready; actual bone weighting is Slice C.
- **Real shadow casting from mech bodies** — current shadow path is CPU-driven; out of scope.
- **`MC2_MECH_GPU_PARITY` dual-FBO gate** — explicitly deferred per Slice A's advisor sign-off; required before any default-on flip.

## File map

| Action | File | Responsibility |
|---|---|---|
| Modify | `shaders/mech.vert` | `#include lighting.hglsl`, conditional calc_light, u_lightingMode uniform |
| Modify | `shaders/mech.frag` | Add debug mode 9 (lighting viz) |
| Modify | `mclib/mech3d.cpp` | CacheGpuLightData call in update; lightDataIndex source in render |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | u_lightingMode uniform, MC2_GPU_MECH_LIGHTING env, MC2_MECH_LIGHT_TRACE diag |
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | extern bool g_useGpuMechLighting |

## Spec self-review

- **Placeholder scan:** none.
- **Internal consistency:** killswitch table matches code locations; failure-mode entries match implementation choices.
- **Scope:** single implementation plan covers everything. No decomposition needed.
- **Ambiguity:** `base_light = vec3(0.35)` is a magic number; documented as "ambient floor" but the value itself is tunable. Acceptable — flagged in implementation as `kAmbientFloor` constant for easy tweaking post-soak.
