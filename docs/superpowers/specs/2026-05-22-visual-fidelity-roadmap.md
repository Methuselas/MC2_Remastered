# Visual Fidelity Roadmap — Track V (2026-05-22)

Status: living reference. Companion to the engine convergence roadmap.
Scope: "fast clean pretty" — the user's lane once engine architecture (Track R) stabilizes.
North star: MechWarrior 5 Mercenaries visual quality (UE4-era, not UE5/Nanite/Lumen).

---

## Status backfill — 2026-06-04 (first amendment since 2026-05-22)

This doc was a pristine forward-spec, never amended, while real Track V work
shipped through MEMORY handoffs. Backfilled here. **Headline: the Track R
prerequisites this doc gates V behind are now largely DONE** (RenderWorld
StaticProp+Mech, PipelineDesc, VisibilityRequest v0, Object-ID buffer fully
realized, ViewUniforms F1 substrate) — so **Track V proper is UNBLOCKED.** The
gating warning at "Sequencing" below is satisfied; V4 HDR (the doc's own #1 ROI)
is the next clean pick.

Delivery diverged from the prescribed order: **V8 impostor shipped FIRST**, out
of sequence, under perf pressure (foliage was a multi-second GPU sink). That was
correct triage, not a process failure.

| Item | Status (2026-06-04) | Evidence |
|---|---|---|
| V1 PBR MaterialGpu | **PARTIAL** | static-prop MaterialGpu DEFAULT-ON (v7, `MaterialGpu.h`+`material_gpu.hglsl`, mirror-checked); mech `materialIdx` field present but shader sampling **BLOCKED** on texture-model decision; terrain splat = reference (done); KTX2 colormap atlas shipped (`7768d4e5`, 108→27MB) |
| V2 IBL | **PARTIAL (substrate only)** | HDRI sky `SkyRenderAdapter`+EXR shipped (`e3a29a86..b06dff00`); SH-L2 sky reflection for water shipped default-OFF (`68343329`). NOT wired as object-PBR ambient/irradiance; no BRDF-LUT |
| V3 stable CSM | **PARTIAL** | dynamic-projection fix + caster feed (`69522900`, 733 casters); shadow-dirty-only perf; bias sliders. NO texel-snap stable cascades, no cascade-debug view |
| V4 HDR post stack | **NOT STARTED** | doc's own "highest visual ROI"; next clean pick — independent of cook |
| V5 SSAO/GTAO-lite | **NOT STARTED** | — |
| V6 decals | **PARTIAL (bake only)** | road/building decal-bake static, default-ON; no lifetime/impact/scorch system |
| V7 VFX substrate | **PARTIAL** | GPU particles default-ON (FX-GPU B3); weapon-fx restore (`9dfff4a9`); CardCloud/ShardCloud GPU-compute proven (maxPosError=0); age-sample. flipbook-asset-table + age-lifetime-upload IN FLIGHT (uncommitted worktrees). No soft/lit particles, no unified substrate |
| V8 LOD+impostor | **SHIPPED (foliage); generalization pending** | foliage 2-card impostor `Render.GpuStaticProps` 6.77s→161µs (`01f3c1b6..d12f7c2b`); `tree_lod_bake` proven; foliage depth-prepass (`MC2_STATIC_PROP_DEPTH_PREPASS`). Trees only — generalized hysteresis stack + non-foliage impostors not formalized; needs Track G cook |
| V9 terrain material | **PARTIAL** | PBR splat foundation (`gos_terrain.frag`) done; macro-variation / detail-normals / slope-blend / scorch not done |
| V10 object-ID inspect | **PARTIAL (substrate done, UX not)** | object-ID buffer FULLY REALIZED (Track R item 10) + pick works; the art-inspection click→material/LOD/packet workflow not built |
| V11 reactive surface/heat | **NOT STARTED** | hero feature; all 4 slices S0–S3 unstarted |

**Completion read: ~50%** (1 shipped, 7 partial, 3 not-started). The not-started
high-ROI frontier is **V4 HDR → V5 SSAO → V2 IBL-Ph0 (wire object ambient)**,
plus **V11** (hero) much later. Partials needing a finish pass: V1 mech-sampling,
V3 stable-CSM, V8 generalization (behind Track G), V10 inspect-UX.

See `2026-06-04-engine-convergence-and-fidelity-next-arc.md` for the unified
next-arc sequencing across Tracks R/G/V/E.

---

**Division of labor context:**
- This project: renderer, engine architecture, asset pipeline, visual fidelity, performance.
- Collaborator: GUI, mission editor, animations, TechScript/Lua scripting, AI brains.
- Do NOT scope Track V into animation rigs, scripting, or AI — those are the collaborator's lanes.

---

## What "UE4-level" means for this engine

UE4-era does not require: hardware ray tracing, full Nanite, full Lumen,
virtual shadow maps, mega open-world streaming, or AAA editor pipeline.

It does require consistency across:

```
1.  PBR materials                   — BaseColor / Normal / Metallic / Roughness / AO / Emissive
2.  Modern lighting model           — directional sun + IBL/ambient + emissives
3.  Shadow quality and stability    — stable CSM, reverse-Z aware, RTS-stable texel snapping
4.  Post-processing stack           — HDR scene, bloom, ACES-ish tonemap, color grading, gamma
5.  Terrain/object LOD              — meshOptimizer LODs + meshlets + impostors
6.  GPU-driven visibility           — compute cull + indirect draw (already strong)
7.  Particles / VFX                 — GPU buffers, soft particles, lit particles, flipbook
8.  Decals + destruction readability — projected decals, terrain scorch, impact marks
9.  Asset cook / import pipeline    — Assimp → meshOptimizer → .cdag sidecar
10. Debuggable render architecture  — object-ID buffer, per-pass timers, visual inspectability
```

Items 6, 9, 10 are already in the Track R roadmap and directly prerequisite Track V.

The hardest part is not one feature — it is **consistency**: all materials
obey one model, all lights obey one model, all shadows are stable, all objects
have LOD/fallback, all passes use the same view data. That is why Track R
(engine contracts) must precede Track V (visual features).

---

## Track V item list

### V1. PBR material contract

**First "pretty" meta-fix.** Define one canonical `MaterialGpu` struct
that every static prop, building, and mech eventually flows through:

```cpp
struct MaterialGpu {
    uint albedoKtxIndex;       // index into KTX2-backed texture array
    uint normalKtxIndex;
    uint mrKtxIndex;           // metallic-roughness packed
    uint aoKtxIndex;
    uint emissiveKtxIndex;
    uint teamColorMaskIndex;
    uint damageMaskIndex;
    uint flags;
};
```

KTX2 texture arrays (via KTX-Software / BasisU) are the texture format for all
MaterialGpu indices. KTX files contain mip chains, block-compressed formats,
and Basis Universal (UASTC/ETC1S) for GPU-side transcoding. This is more
Vulkan-shaped than per-draw GL binds, and co-ships with the material milestone
rather than being deferred to a later asset-pipeline concern.

No renderer path invents its own lighting model. Terrain PBR splatting
already exists as a reference — promote it to the project-wide material
contract.

Migration order: static props/buildings first → mechs second → terrain
(already done) → particles/VFX separate.

---

### V2. IBL / environment lighting

Split into two phases. Phase 0 lands shortly after V1 PBR + V4 HDR because a
simple ambient term is what makes PBR feel correct, not just different.

**Phase 0 (land with V1/V4):**
```
Directional sun (already exists)
+ ambient sky color term (single vec3 or simple dome gradient)
+ one global roughness-based reflection cubemap (offline pre-baked)
+ irradiance term for diffuse (SH or baked irradiance texture)
```
Debug mode: roughness mip visualization + reflection override toggle.

**Phase 1 (later — after LOD/impostor work):**
```
Better probes / SH / per-area reflection improvements
Specular environment BRDF LUT for correct energy conservation
Debug reflection probe placement view
```

---

### V3. Stable cascaded shadow maps

For MW/RTS visuals, mech/building/terrain shadow quality is critical.
RTS camera motion makes shimmer very visible — stability beats resolution.

Requirements:
- Stable CSM (texel snapping, fixed light-space pivot)
- Reverse-Z-aware depth (scene is `GL_GEQUAL`, shadow FBOs stay forward-Z `GL_LESS` — already documented)
- Per-cascade debug view (colored cascade visualization)
- Per-object shadow flags (shadow caster / receiver policy)
- Far-building LOD shadow policy (lower-detail shadow geometry for far props)

Connects to the multi-view model (Track R item 7): shadow cascades are
views, not a special case. Defer until after F1 + RenderWorld.

---

### V4. HDR post-processing stack

Highest visual ROI item for "UE4 look." Even a simple ACES-ish tonemap
plus bloom dramatically modernizes visuals.

```
Scene color RGBA16F buffer
→ Bloom extract / threshold / blur (half-res ping-pong)
→ ACES-ish tonemap
→ Color grade (LUT or simple lift/gamma/gain)
→ Gamma correction (linear to sRGB)
→ UI composite (post-tonemap)
```

Already have a post-process FBO path. Extend it rather than rewrite.
Bloom and tonemap are the first two passes to add.

---

### V5. SSAO / GTAO-lite (clean reintroduction)

The old SSAO was deleted/disabled because it was entangled with legacy
projection. Add it clean:

```
Depth buffer + view-space normals
→ half-res SSAO kernel (16-tap, screen-space)
→ normal-aware bilateral blur
→ composite into lighting pass
→ feature flag (MC2_SSAO, default OFF initially)
```

Provides the "grounding" effect under mechs/buildings/rocks that is
a hallmark of the UE4 look. Do NOT resurrect the old path — add a
new one consuming the `ViewUniforms` UBO.

---

### V6. Decals as a real system

BattleTech visuals rely heavily on decals for readability:

```
Scorch marks (weapon impacts, explosions)
Mud / dust / tire tracks
Road and building overlays (already partially done via decal-bake)
Building damage states
Mech leg/footfall marks
Selection / command ring projections
```

First useful version: **depth-projected terrain impact decals** with:
- Bounded lifetime
- Material ID support
- Object-ID exclusion flag
- Debug draw volumes

Defer screen-space / deferred decal volumes until after `ObjectIDBuffer`
and `PipelineDesc` exist.

---

### V7. GPU particle / VFX substrate modernization

MW5 visuals live or die on particle scale: dust clouds, autocannon impacts,
missile trails, PPC arc, laser bloom, explosion fire/smoke.

Rendering meta-fix:
```
Particles use ViewUniforms, MaterialGpu, FeatureRegistry,
object-ID exclusion flag, and explicit pass contracts.
Not one-off particle code.
```

VFX substrate items:
- GPU particle buffers (already B1 GPU particle work)
- Soft particles (depth-based fade at intersection)
- Lit particles (receive directional + ambient from lighting pass)
- Flipbook atlas (animated textures via UV offset)
- Beam / trail renderer (muzzle flash cones, laser traces)
- Heat haze (deferred — requires refraction/distortion pass)

---

### V8. LOD + impostor stack for RTS readability

```
Near:       full mesh / meshlets
Mid:        simplified LOD (meshOptimizer simplify)
Far:        impostor atlas (pre-rendered N-view card)
Very far:   strategic silhouette or icon
```

The existing constraint (`distant_buildings_render_at_lower_lod_never_distance_culled.md`)
is architecturally correct for this stack: performance reduction, not
information removal. The LOD stack reduces triangle cost; it never culls
to invisible.

Hysteresis bands (switch_to_lower at D1, switch_to_higher at D2 < D1)
are required to prevent shimmer/popping at RTS camera pan speeds.

Impostor generation requires: pre-cook step (N-view atlas render),
cooked asset manifest entry, MeshCapability flag `HasImpostor`.

---

### V9. Terrain material modernization

Priority order for terrain visual quality:

```
1. Splat/weight material layers (already partially done via PBR splatting)
2. Macro texture variation (large-scale tiling breaker)
3. Detail normal maps (per-material micro-surface)
4. Slope/height blending (grass on flat, rock on steep)
5. Road/overlay integration (cement, runway, transition via decal-bake)
6. Damage/scorch layer (dynamic terrain decals from V6)
```

Defer:
- Terrain virtual texturing / clipmap (post-meshlet terrain LOD)
- Wetness/snow/dust (late-stage environmental variation)

The existing `gos_terrain.frag` PBR splat is the foundation. Extend
rather than rewrite.

---

### V10. Object-ID as art inspection tool

Object-ID buffer (Track R item 10) becomes the visual debugging
backbone:

```
Click pixel → object name → material → mesh LOD → draw packet → feature path
```

For visual fidelity work specifically:
- "Which material is this building using?" — object-ID → material lookup
- "Why is this prop using the wrong LOD?" — object-ID → LOD level overlay
- "Is this mech receiving shadows?" — object-ID → shadow contract flag

This makes fidelity bugs diagnosable in minutes instead of hours.

---

### V11. Reactive Surface & Heat FX (weather/heat surface response)

**Hero close-up feature.** A rain-soaked mech that vents steam when it fires is
not just cosmetic — it sells mech mass, BattleTech heat mechanics, weather, and
close-up spectacle simultaneously. It sits at the intersection of Track V (visual
fidelity), Track C (command-scale heat state readability), and S1/S11 (thermal
sensor view).

---

#### Surface state model

The mech has a surface state, not just a material. A single `SurfaceState` SSBO
entry per mech (or per object bucket), updated by gameplay → rendered by shader.

```cpp
struct SurfaceState {
    float wetness;           // 0..1 — weather-driven
    float puddling;          // 0..1 — local pooling on flatter panels
    float heatLevel;         // 0..1 — normalized mech heat bar
    float recentWeaponHeat;  // 0..1 — short-lived firing burst (decays ~2s)
    float damageSoot;        // 0..1 — burn / grime accumulation
    float mudiness;          // 0..1 — optional late polish
};

struct WeatherState {
    float rainIntensity;       // 0..1
    float ambientWetness;      // 0..1 — ground/environment saturation
    float windStrength;        // optional — streaking direction
    float ambientTemperature;  // normalized — cold air = more steam
};

struct MechVisualState {
    float heat;
    float recentFireHeat;
    float damageLevel;
    float wetnessOverride;
    uint32_t flags;
};
```

The renderer combines:
```
WeatherState (global UBO) + MechVisualState (per-object SSBO) + MaterialGpu
    → final surface shading + VFX emission rate
```

`WeatherState` lives in its own small UBO (one per frame, global). It is NOT
baked into `MaterialGpu` — weather is a rendering-layer modifier, not a material
property.

---

#### Visual effect of wetness on PBR

Rain-wet metal via material modifiers (no fluid simulation needed):

```
albedo:    multiply by (1.0 - wetness * 0.25)  — darker surface
roughness: lerp(base_roughness, 0.05, wetness * mask) — smoother wet panels
metallic:  small increase on wet flat panels
env_refl:  stronger IBL sample weight on wet surfaces
streaks:   normal map perturbation on vertical panels (animated UV offset)
```

The wetness mask is a per-material channel stored in `MaterialGpu` (a
`wetnessMaskIndex` texture, similar to AO). Flat panels (horizontal or
near-horizontal facing) pool more aggressively than vertical surfaces.

---

#### Steam VFX design

Steam spawns when `rainIntensity > 0 AND heatLevel > threshold`.
Emitter sockets on the mesh (bone/socket attachment points), not procedurally
over the whole surface:

```
Weapon barrels / laser emitters      — fire-event burst + linger
Missile rack doors / launch cells    — launch-event burst
Engine vent locations                — sustained proportional to heatLevel
Shoulder/torso armor vents           — medium-heat continuous
Leg joint vents (optional)           — stylized heat bleed
Damage hotspots                      — persistent after taking damage
```

Steam particle properties:
```
Lifetime:    0.5s – 3s depending on vent type
Opacity:     scale by rainIntensity * heatLevel
Color:       white → warm grey → dirty if damageSoot > 0.5
Soft:        yes — depth-fade at intersection (V7 soft particles)
Lit:         yes — receive directional light (V7 lit particles)
Scale:       larger at high heat, finer at low heat
```

The mech communicates its thermal state visually:
```
low heat:         subtle wet sheen, no steam
medium heat:      occasional vent vapor, warm metal reflections
high heat:        continuous steam after firing, strong plumes from vents
near-shutdown:    dramatic venting, thermal signature obvious (S1 integration)
```

---

#### Thermal view integration (S1)

In `ViewMode::Thermal`, the surface heat state drives the thermal render:
```
bright weapon housings (recentWeaponHeat)
hot engine core (heatLevel)
cooling streaks after cease-fire
steam partially diffuses/occludes the thermal signature
recently fired barrels stay warm briefly
shutdown mechs cool visibly over time
```

This is a free payoff from having the data model correct — the thermal shader
just reads `heatLevel` and `recentWeaponHeat` from the same SSBO.

---

#### Implementation ladder (four slices)

**V11-S0 — Wetness material only (first payoff)**
```
Global WeatherState UBO (rainIntensity, ambientWetness)
Per-object wetness factor fed from gameplay (can be hardcoded at start)
Mech/prop PBR shader: albedo darkening + roughness reduction + env_refl boost
No VFX yet
Gate: MC2_SURFACE_WEATHER (default OFF)
```

Already looks significantly better in rain. Delivers the "wet Mad Cat" visual.

**V11-S1 — Firing steam (emotional payoff)**
```
On weapon fire: spawn short-lived steam puffs from weapon socket positions
Scale by: rainIntensity * recentWeaponHeat
Gate behind MC2_SURFACE_WEATHER
Uses V7 GPU particle substrate (prerequisite)
```

Delivers the "steam peel off vents when firing" visual.

**V11-S2 — Systemic heat/wet interaction**
```
MechVisualState SSBO (heat, recentFireHeat, wetnessOverride, flags)
Persistent heat value per mech (driven from gameplay heat bar)
Vent steam proportional to heatLevel
Cooling over time (decay in GPU buffer)
Damage hotspot emitters
Gate: MC2_SURFACE_HEAT
```

This is the "real" version — visual state driven by actual BattleTech heat.

**V11-S3 — Polish layer (later)**
```
Wetness mask texture per material (flat-panel pooling)
Rain streak normal animation (vertical surfaces)
Heat shimmer near vents (screen-space distortion pass — requires V7 heat haze)
Steam lit by muzzle flash / searchlights (V7 lit particles)
Stronger effect at night (Track C night mode integration)
Thermal mode integration (S1 ViewMode)
Wet footprints / disturbed mud (V6 decal integration)
```

---

#### Gates

```
V11-S0: V1 PBR MaterialGpu contract (base material to modify)
V11-S1: V7 GPU particle substrate (steam VFX emitters)
V11-S2: V1 + V7 + gameplay heat state accessible via MechVisualState SSBO
V11-S3: V6 decals + V7 heat haze + S1 ViewMode
```

---

## Debug mode requirement (per Track V feature)

Every Track V feature must ship with at minimum:

| Feature | Required debug view | Counter/cost | Feature flag | Object-ID inspect |
|---|---|---|---|---|
| V1 PBR material | Material channel view (albedo/normal/metallic/roughness/AO) | material table occupancy | MC2_PBR_MATERIALS | material lookup from pixel |
| V2 IBL | Roughness mip visualization + reflection override toggle | — | MC2_IBL | — |
| V3 CSM shadows | Cascade color view (each cascade a different color) | cascade count + texel density | MC2_CSM | shadow receiver flag from pixel |
| V4 HDR/bloom | Bloom threshold mask view + pre/post-tonemap toggle | bloom pass ms | MC2_HDR_POST | — |
| V5 SSAO | AO-only view (render just AO term to screen) | AO pass ms | MC2_SSAO | — |
| V6 Decals | Decal volume wireframe + object ownership view | active decal count | MC2_DECALS | decal owner from pixel |
| V7 Particles | Overdraw heat + soft-particle fade debug | emitter count + particle count | MC2_VFX_SUBSTRATE | — |
| V8 LOD/impostor | Current LOD band + impostor switch overlay | LOD distribution histogram | MC2_LOD_STACK | LOD level from pixel |
| V9 Terrain materials | Splat weight per-layer view + macro variation toggle | — | MC2_TERRAIN_MAT_V2 | — |
| V10 Object-ID | Full object-ID read-back view (already the feature itself) | — | MC2_OBJECT_ID_BUFFER | every other feature uses this |
| V11 Reactive Surface & Heat FX | Wetness mask overlay + heat/steam debug view (SurfaceState channels to screen) | steam emitter count + WeatherState values | MC2_SURFACE_WEATHER + MC2_SURFACE_HEAT | heat state + wetness from pixel via object-ID → MechVisualState lookup |

These must be wired before the feature is considered complete. The existing
`MC2_TERRAIN_DEBUG_MODE`, `[SUBSYS v1]` banners, and Tracy zones are the
precedent — Track V follows the same discipline, not a new one.

---

## Sequencing ("fast clean pretty" order)

Track R prerequisites must come first. Do NOT add V features before
the R foundation is stable — V features land as special cases without
R, and you end up with the `terrainMVP`/`projectZ` split again.

```
Already in flight (Track R):
  F1 ViewUniforms UBO
  quadSetupTextures retirement

Then (Track R before Track V):
  RenderWorld boundary spec
  PipelineDesc + render contract Phase 2
  VisibilityRequest wrapper
  Object-ID buffer (R10 / V10 shared)
  Assimp + meshOptimizer cook (shared R/V)

Then (Track V proper):
  V1. PBR MaterialGpu contract (static props/buildings first)
  V4. HDR scene color + tonemap + bloom
  V2. IBL Phase 0 (simple ambient sky + one global reflection cubemap — land here, PBR needs it)
  V3. Stable CSM shadow cleanup
  V10. Object-ID inspect workflow
  V8. LOD + impostor stack (after Track G cook pipeline)
  V6. Decals v1
  V5. SSAO/GTAO-lite v1
  V7. Particle/VFX substrate cleanup
  V11-S0. Reactive Surface wetness-only (V1 PBR required — hero close-up visual)
  V11-S1. Steam on firing (V7 particles required)
  V9. Terrain material modernization
  V11-S2. Systemic heat/wet (gameplay heat state integration)
  V2. IBL Phase 1 (better probes / SH / energy-conserving BRDF LUT)
  V11-S3. Surface polish (V6 decals + heat haze + S1 thermal integration)
```

---

## MW5 target realism check

MechWarrior 5 is UE4-era, NOT UE5/Nanite/Lumen. The look is achievable with:

```
Required:
  PBR materials, CSM shadows, SSAO, postprocessing (HDR/bloom/tonemap),
  decals, GPU-driven visibility/LOD, particles, terrain detail, good assets

NOT required:
  Hardware ray tracing
  Full Nanite / virtual geometry
  Full Lumen / global illumination
  Virtual shadow maps
  Mega open-world streaming
  AAA editor pipeline
```

The hardest part is not any single feature. It is **consistency**: all
materials in one model, all lights in one model, all shadows stable, all
objects with LOD/fallback, all passes consuming the same `ViewUniforms`.

That is why the meta-fix principle (Phase 2, engine convergence roadmap)
applies equally to visual fidelity: every time two render paths handle
the same visual concern differently, create one owner.
