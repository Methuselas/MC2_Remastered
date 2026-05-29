# SHADOW-STATIC-BUILDINGS-RECON-0

**Date:** 2026-05-29
**Scope:** Recon/plan only. No implementation, no shader/behavior changes, one docs commit.
**Goal (audit "C"):** Give rigid buildings stable far-field shadows by adding them as casters to the world-fixed **static** shadow map, while trees/foliage stay dynamic and the bounded-near dynamic fit (B′) keeps near shadows crisp.

## TL;DR — feasible, but gated by one prerequisite

Adding rigid building casters to the static map is architecturally supported (the static prepass already accepts extra casters; buildings are already baked in the static-prop batcher geometry). **But the terrain shadow receiver combines the two maps MULTIPLICATIVELY (`staticShadow * dynShadow`), which double-darkens any building that appears in both maps** — and since the static map is world-fixed (all buildings) while the dynamic bounded-near map moves with the camera (near buildings), the overlap is camera-dependent → a moving double-dark seam on terrain. The static map is also **coarser** (~4.64 WU/texel vs the dynamic bounded-near ~2.0) and the **static terrain prepass applies no polygon offset**.

**Recommendation: two slices.** First **C-pre** — make the terrain combine idempotent (`min()` instead of `*`, matching the screen-space pass) so a caster in both maps is harmless. Then **C** — add a gated, building-population-filtered, static-matrix `flushShadow` into the static prepass, with a polygon-offset/bias fix. Without C-pre, C produces visible terrain double-shadowing.

## Pass flow (current)

```
renderLists() [txmmgr.cpp]
 ├─ STATIC terrain shadow build (ONCE per mission, latch)         [1916-1940]
 │   gos_BuildStaticLightMatrix (world-fixed, sun baked at load)
 │   gos_BeginShadowPrePass(true)  → bind shadowFBO_ (4096², ~4.64 WU/texel)
 │       renderStaticTerrainShadowFullMap  → TERRAIN HEIGHTFIELD ONLY
 │   gos_EndShadowPrePass            (NO glPolygonOffset in this path)
 ├─ GpuStaticPropRegistry::flush()   → fills s_bucketsByType        [~1941, SHADOW-ORDER FIX]
 ├─ DYNAMIC sun-shadow pass                                         [1946-1990]
 │   gos_BeginDynamicShadowPass → bind dynShadowFBO_ (4096², bounded-near ~2.0 WU/texel)
 │       GpuStaticPropBatcher::flushShadow()  → ALL static props (trees+buildings), DYNAMIC matrix
 │       GpuMechBatcher::flushShadow()
 │   gos_EndDynamicShadowPass
 └─ ... terrain/props/mechs draw (receivers) ...

Receivers:
  terrain (gos_terrain.frag): shadow = calcShadow(STATIC) * calcDynamicShadow(DYNAMIC)   ← MULTIPLY
  static props / mechs (shadow_screen.frag): shadow = min(STATIC, DYNAMIC)               ← MIN (idempotent)
```

## Area 1 — Static shadow map build

- **Trigger:** `txmmgr.cpp:1916` — `gos_IsTerrainTessellationActive() && !gos_StaticLightMatrixBuilt() && Terrain::mapData`. Builds **once per mission**; latch `staticLightMatrixBuilt_` (`gos_postprocess.cpp:1136`) re-armed **only** by `Terrain::destroy()` → `gos_ResetStaticLightMatrix()` (`mclib/terrain.cpp:781`).
- **Matrix:** `buildStaticLightMatrix` (`gos_postprocess.cpp:1249`), center (0,0,0), `r = mapHalfExtent*√2*1.05` (~9503 WU for a 100-side map), near 1, far 2r. **Sun direction is read at build time and frozen** — no re-arm on sun change.
- **FBO/texture:** `shadowFBO_` + `shadowDepthTex_`, **4096² DEPTH_COMPONENT24**, `RenderResourceId::ShadowStaticMap`. Separate from the dynamic `dynShadowFBO_`/`dynShadowDepthTex_` (also 4096²).
- **Prepass GL state** (`gos_BeginShadowPrePass(true)`, gameos_graphics.cpp:4966): depth test on, `GL_LESS`, depth write on, depth clear (one-shot), **cull disabled**, viewport 4096², `shadow_terrain` program, `lightSpaceMatrix = staticLightSpaceMatrix_`. **NO `glPolygonOffset`** in this gosRenderer path (the 2.0/4.0 offset lives only in the old `gosPostProcess::beginShadowPass` dynamic path) — see Area 3 acne risk.
- **Casters today:** terrain heightfield only (`renderStaticTerrainShadowFullMap`, `mclib/mapdata.cpp:1294`).
- **Prepass window is already open for extra casters:** `gos_DrawShadowObjectBatchStatic()` (gameos_graphics.cpp:7587) exists, wired for "Plan 2C decorative-mesh static casters"; while `shadow_prepass_active_` is true, `active_light_space_matrix_` already points at the static matrix. Extra caster draws can be appended between Begin/End with no API change.
- **Texel density:** 2r/4096 ≈ **4.64 WU/texel** (vs dynamic bounded-near ~2.0). Far buildings: acceptable (small on screen); near buildings: noticeably softer than the dynamic map.

## Area 2 — Building caster authority

- **Buildings = `BldgAppearance`** (bdactor.cpp:1157), `TG_MultiShape`, baked into the **same** static-prop batcher geometry (`s_types`/`s_typeRanges`, `registerType` gos_static_prop_batcher.cpp:1621) as trees.
- **Rigid predicate:** `isStaticEligible()` (bdactor.cpp:2516) — false if `spinMe`, type animations, `drawFlash`, `destructFX`, `activity/activity1`, `bdAnimationState!=-1`. **Clean filter for "static-map safe" buildings.**
- **`flushShadow()` is already depth-only** (`shadow_static_prop` prog 79, no texture/lighting) — the right primitive. **But two blockers for a building-only static-map call:**
  1. It takes **no args** and hardcodes `pp->getDynamicLightSpaceMatrix()` (gos_static_prop_batcher.cpp:5491). Needs a matrix parameter to use the static matrix.
  2. **No population filter** — it iterates ALL `s_typeRanges` (trees+buildings together). Population (`Building/Tree/...`) is **not stored per-type** (only in frame counters); `GpuStaticPropType` has no population field. Building-only draw requires a schema add (population byte on `GpuStaticPropType` populated in `registerType`, or a parallel `typeID→population` side-map).
- **Timing:** buildings register at mission load (`registerStaticPropsForMissionLoad`, objmgr.cpp:1405) and reach `s_bucketsByType` via `GpuStaticPropRegistry::flush()`. That flush runs at `txmmgr.cpp:~1941` — **after** the static terrain prepass block (1916-1940). So on the build frame, buildings are **not yet in buckets** when the static prepass runs → a building static-caster draw there needs either a registry-flush reorder (before the static block) or to fire one frame later via its own latch.

## Area 3 — Receiver composition + double-shadow (the load-bearing finding)

| Receiver | Shader | Samples | Combine | Double-shadow |
|---|---|---|---|---|
| **Terrain** | gos_terrain.frag:723 | static `calcShadow` × dynamic `calcDynamicShadow` | **MULTIPLY** | **UNSAFE** — caster in both maps: 0.4×0.4=**0.16** vs single 0.4 (>2× darker) |
| Static props | shadow_screen.frag:184 | `min(static, dynamic)` | **MIN** | SAFE (idempotent: min(0.4,0.4)=0.4) |
| Mechs | shadow_screen.frag | `min(static, dynamic)` | **MIN** | SAFE |
| Buildings (as receivers) | shadow_screen.frag | `min(static, dynamic)` | **MIN** | SAFE (but self-shadow acne, below) |

- **Samplers:** static = `shadowMap` + `lightSpaceMatrix` (unit 9 terrain / 2 screen); dynamic = `dynamicShadowMap` + `dynamicLightSpaceMatrix` (unit 10 / 3). Terrain excludes itself from the screen pass via GBuffer1.a=1.0; props/mechs set 0.0 (screen-eligible).
- **Critical:** the **terrain** multiplicative combine double-darkens any building present in both maps. The static map is world-fixed (every rigid building, always); the dynamic bounded-near map covers a moving near region (near buildings). So a near building is in **both** → terrain under it darkens to 0.16, and the dark patch **moves/pulses with the camera** (as the bounded-near region shifts). This cannot be fixed by static-side distance culling — the static map has no camera knowledge.
- **Bias/acne:** static prepass has **no polygon offset** (Area 1); building casters there get no depth bias → terrain-contact acne. Receiver-side: terrain uses slope bias 0.002–0.005, screen pass fixed 0.003 (calibrated for terrain, may under-bias oblique building faces).
- **Self-shadow:** buildings receive static-map shadows via the screen pass; a building casting into AND receiving from the static map self-shadows with only the 0.003 fixed bias → acne risk on oblique faces.

## Injection options (ranked)

| Option | Approach | Schema | Timing fix | Risk | Rank |
|---|---|---|---|---|---|
| **B** | `flushShadow(overrideLightMatrix=nullptr, popFilter=ALL)`; call inside static prepass with static matrix + `Building` filter | population byte on `GpuStaticPropType` + matrix param | registry-flush reorder or +1-frame latch | Low-Med | **1st** |
| A | (same as B, "call from prepass") | same | same | same | =1st |
| C-list | separate baked building-caster SSBO at first frame, drawn once | new SSBO + latch | same | Med | 2nd (highest quality, most work) |
| D | bake building occlusion into terrain heightfield | — | — | **architectural nonstarter** | reject |

## Invalidation / rebuild policy (recommended)

- **Mission load:** build once (existing latch). Buildings present at load.
- **Mission exit/reload:** existing `Terrain::destroy` re-arm covers it.
- **Building destruction / damage / gate open:** those buildings fail `isStaticEligible()` and are excluded from the static caster set; their shadow simply won't update in the static map (stale shadow of a destroyed building) UNLESS we add a re-arm on destruction. **Defer destruction-invalidation to a follow-up** (acceptable first-cut: destroyed-building static shadows persist; rare, low-visibility). If needed, add a `gos_ResetStaticLightMatrix()` call on building-destruct to force one rebuild.
- **Sun-direction change:** static matrix is baked at load; MC2 sun is fixed per mission, so OK. If time-of-day is ever added, the static map needs a sun-change re-arm (out of scope).
- **LOD/graphics options:** no impact (geometry baked per-type; resolution unchanged).

## Double-shadow strategy (the decision)

**Prerequisite C-pre — make the terrain combine idempotent.** Change `gos_terrain.frag:725` from `staticShadow * dynShadow` to `min(staticShadow, dynShadow)` (matching the screen-space pass already used for props/mechs). Then a building in both maps yields `min(0.4,0.4)=0.4` — identical to single-map, no double-darken, no moving seam. Small, localized shader change; run `/mc2-amd-shader-review` + shader_reflect goldens. This is a **hard prerequisite** for C on terrain.

Alternatives considered & rejected for the first cut:
- Buildings static-only (remove from dynamic): loses bounded-near crispness for near buildings — contradicts B′.
- Static-side near exclusion: impossible (static map is world-fixed, camera-agnostic).
- Accept double-darken: visible camera-dependent artifact, not acceptable.

## Risks

1. **Terrain double-shadow (multiplicative)** — blocker; fixed by C-pre.
2. **No polygon offset in static prepass** — building-contact acne; add `glPolygonOffset` to the static caster draw (or per-caster bias).
3. **Self-shadow acne** on buildings (0.003 fixed bias) — may need a small bias bump for building receivers.
4. **Coarse static texels (4.64 WU/texel)** — far building shadows soft; acceptable for far field, not a substitute for dynamic near.
5. **Timing** — registry buckets not warm when the static prepass runs on the build frame; reorder or +1-frame latch.
6. **Stale shadow of destroyed buildings** — accepted for first cut; destruction re-arm is a follow-up.
7. **Schema churn** — population byte on `GpuStaticPropType` touches `registerType` + callers; keep additive.

## Recommended implementation slices

1. **SHADOW-TERRAIN-COMBINE-MIN-1 (C-pre, prerequisite):** terrain receiver `*` → `min()` for static×dynamic shadow combine. Gated (`MC2_SHADOW_COMBINE_MIN=1`?) or straight swap if reviewer agrees it's strictly-better. Shader-review + reflect goldens. Makes double-shadow idempotent across ALL receivers.
2. **SHADOW-STATIC-BUILDINGS-1 (C):** add `population` to `GpuStaticPropType` + `flushShadow(overrideLightMatrix, popFilter)`; call `flushShadow(pp->getLightSpaceMatrix(), Building)` inside the static prepass (registry-flush reorder/latch for timing); add `glPolygonOffset` to the static caster draw. Gate `MC2_SHADOW_STATIC_BUILDINGS=1`, default OFF. Trees stay dynamic. Destruction-invalidation deferred.
3. **Defer:** destruction re-arm, self-shadow bias tuning, and (long-term) CSM (E). Foliage shadow shape stays SHADOW-FOLIAGE-ALPHA-DISCARD-1 (separate).

**Sequencing:** C-pre → C. Do NOT ship C without C-pre (terrain double-darken). Each behind a default-off gate.
