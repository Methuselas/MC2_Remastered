# (E) SpotLight_ → Real Illumination — Design Spec

- **Status:** DRAFT — ready for plan-phase after open questions resolved
- **Date:** 2026-05-20
- **Worktree:** `claude/nifty-mendeleev`
- **Companion specs:** [(A) gosFX retirement](2026-05-20-gosfx-retirement-or-replacement-design.md); [(C) Light GameObject cleanup](2026-05-20-light-gameobject-retirement-design.md); [(D) real-lighting sketch](2026-05-20-light-real-illumination-plumbing-design.md) (superseded by this spec for the spotlight subset)
- **Greybeard verdict:** META-FIX. Stop drawing `SpotLight_*` cone geometry; emit a `TG_Light` instead. Same .ase/.cmf data; existing lighting consumers (terrain, props, mechs) pick up real spillage. Dissolves the "opaque triangles" symptom AND the f77f135 alpha-blend program-switch state-leak class in one move.
- **All file:line citations grep-verified at write time against `nifty-mendeleev`.**
- **Session divergence note:** the CPU-reduce campaign continues elsewhere. This spec is a parallel visual-quality slice that does not depend on (A)/(B)/(C)/(D).

---

## 1. What "SpotLight_" actually means in the codebase

Per-instance tagging at TG_Shape construction (TWO load paths):
- [mclib/tgl.cpp:278](mclib/tgl.cpp): `newShape->isSpotlight = (S_strnicmp(newShape->getNodeName(),"SpotLight_",10) == 0);`
- [mclib/tgl.cpp:494](mclib/tgl.cpp): same, second loader path (.ase vs .cmf variants).
- [mclib/tgl.cpp:281-283](mclib/tgl.cpp): `if (newShape->isSpotlight) newShape->noShadow = true;` — spotlights already gate off shadow casting.

CPU render path (legacy, mostly dead per the memory):
- [mclib/tgl.cpp:1728-1730](mclib/tgl.cpp): `if (isSpotlight && !isNight) listOfVertices = NULL;` — spotlight geometry is skipped during daytime in the CPU rasterizer.
- [mclib/txmmgr.cpp:2173](mclib/txmmgr.cpp): the `MC2_ISSPOTLGT` queue is dead under Renderer 3 (probe `MC2_SPOTLIGHT_TRACE=1` measured 0 verts across 3600 calls on mc2_05). Do not invest there.

GPU render path (live):
- [GameOS/gameos/gos_static_prop_batcher.cpp](GameOS/gameos/gos_static_prop_batcher.cpp) submitMultiShape: sets `flags |= (1u << 2)` when `child->isSpotlight`. The bit gates lighting math in `static_prop.vert` but does NOT affect blend state. Result: opaque colored triangles regardless of day/night. This is the "shitty ugly light projection thing" the user described.
- [GameOS/gameos/gos_mech_batcher.cpp:553-563](GameOS/gameos/gos_mech_batcher.cpp): spotlight child shapes are SKIPPED entirely with `event=skip_spotlight`. Mech spotlights don't reach the screen at all. Comment notes "Slice B+ can re-enable with a per-actor lightsOut/spotlight flag."

**Failed prior attempt (do not repeat):**
- [f77f135](#) added a blended sub-pass in `GpuStaticPropBatcher::flush()` that switched `glUseProgram` from the coalesce program to the legacy program without proper restore + with uninitialized legacy-program uniforms. Reverted as [d91e639](#) because Vedettes / LRMCs went invisible. The rule that came out of it: [memory/feedback_static_prop_subpass_program_switch.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_static_prop_subpass_program_switch.md). **(E) avoids re-entering this trap by deleting the draw entirely, not by adding a blended draw.**

## 2. Existing lighting infrastructure (the consumer)

Per the (D) recon plus tonight's grep:

| Element | Where | Status |
|---|---|---|
| `MAX_LIGHTS_IN_WORLD = 256` (CPU pool size) | [mclib/tgl.h:170](mclib/tgl.h) | Live |
| `light_falloff[16]` (shader-side budget) | `shaders/include/lighting.hglsl` | Live — the real cap |
| `worldLights[256]` | [mclib/camera.cpp:411](mclib/camera.cpp) | Live |
| `activeLights[256]` (per-frame filtered subset of worldLights) | [mclib/camera.cpp:423](mclib/camera.cpp) | Live |
| `addWorldLight(TG_LightPtr)` | [mclib/camera.h:769](mclib/camera.h) — first-empty-slot, returns slot 0..255 or -1 | Live |
| `removeWorldLight(slot, light)` | [mclib/camera.h:~790](mclib/camera.h) | Live |
| TG_Light types: AMBIENT/INFINITE/INFINITE_WITH_FALLOFF/POINT/SPOT | [mclib/tgl.h:163-167](mclib/tgl.h) | Live |
| TG_Light fields: `closeDistance` / `farDistance` / `oneOverDistance` | [mclib/tgl.h:193-194](mclib/tgl.h) | Live |
| `GetFalloff(length, &falloff)` | [mclib/tgl.h:261-275](mclib/tgl.h) — piecewise linear inner→outer | Live |
| Active consumers via `addWorldLight` | [mclib/bdactor.cpp:1939](mclib/bdactor.cpp), [mclib/mech3d.cpp:3343](mclib/mech3d.cpp), [code/weaponbolt.cpp:1262](code/weaponbolt.cpp) | Live |
| LightsData baked-static path (`mc2WriteStaticLightSlot`) | [mclib/txmmgr.cpp:1205-1253](mclib/txmmgr.cpp) | Live, recipe-indexed |

**Key budget reality (corrected 2026-05-20 after user pushback + lighting.hglsl re-read):**

The two `MAX_LIGHTS_IN_WORLD` values share a name but operate at different scopes:

- **CPU world pool = 256** ([mclib/tgl.h:170](mclib/tgl.h)). The total registered lights for the mission.
- **Shader per-shape array = 16** ([shaders/include/lighting.hglsl:23](shaders/include/lighting.hglsl)). The lights packed into a single `TG_HWLightsData` struct for one draw object.

The pipeline:
1. CPU registers up to 256 lights into `worldLights[]` via `addWorldLight`.
2. Per-shape, CPU runs `CacheGpuLightData` ([mclib/bdactor.cpp:1880](mclib/bdactor.cpp), [:1887](mclib/bdactor.cpp)) to PICK the most-relevant ≤16 lights for that shape and pack them into a `TG_HWLightsData`. Slot cached in `cachedGpuLightIndex_`.
3. At draw time, shader iterates `numLights.x` (≤16) from that bound struct.

So 950 SpotLights in the world pool would NOT make the shader iterate 950 per pixel. Each pixel iterates only its draw object's pre-filtered 16. The CPU filter already handles "more world lights than shader slots" today.

**What (E) actually constrains:**
- (a) CPU world pool: 256 → may need to bump to 1024 or more depending on stock-content SpotLight_ population (see OQ6).
- (b) Shader per-shape array: 16 → may need to bump to 32 or 64 if dense building/mech areas have more lights affecting one shape than 16 best-quality.
- (c) Per-shape filter quality: the existing scoring picks "best 16" — needs sanity-check for static spotlights (distance-based is likely fine).

Per user: "if there isn't some actual limit, just increase the number of slots, or go to a different architecture for lights ... we have a piss ton of room." So both (a) and (b) are permissive scope. Plan-phase should: count actual stock-content SpotLight_ population, set pool size accordingly, and bump shader array only if quality demands it.

## 3. Three approaches

### (α) Delete cone geometry, emit TG_Light only (recommended, user's preference)

At `GpuStaticPropBatcher::submitMultiShape` and `gos_mech_batcher.cpp:553-563`: when `child->isSpotlight==true`, SKIP submitting the cone geometry; instead, register a `TG_Light` of type `TG_LIGHT_POINT` (or `TG_LIGHT_SPOT` — see OQ2) at the child's world-space position via `eye->addWorldLight()`. Store the slot index for `removeWorldLight` at building destruction / mech-killed lifecycle events.

**Pros.**
- No new shader code required for the basic case (existing terrain.frag, mech shaders, static_prop.frag already sample LightsData).
- No alpha sub-pass → no glUseProgram swap → no state-leak class (sidesteps `feedback_static_prop_subpass_program_switch.md`).
- Substitutive by construction: cone draws delete, real light registrations replace.
- Real spillage onto surrounding terrain/props — the actual visual the user wants.

**Cons.**
- Loses the visible cone beam (searchlight-through-air, lamp-cone aesthetic). If content relies on the cone being visible (e.g. atmospheric VTOL searchlight), it's gone. User confirmed: don't care, drop them.
- World pool may need bumping (currently 256 CPU slots) and per-shape shader array may need bumping (currently 16) — both pre-approved per user 2026-05-20 ("piss ton of room"). See OQ6.

### (β) Keep cone geometry opaque (current state), add TG_Light alongside

Don't touch the static-prop cone draw. Add the TG_Light registration on top. Result: opaque colored triangle (current ugly look) PLUS real illumination spillage.

**Pros.** Zero risk of visual regression on cone-dependent content. Real light spillage added.

**Cons.** User explicitly rejected ("the alpha blended billboards are ugly, I don't care about them"). Doubling visual weight. Doesn't address the cone-ugliness symptom at all.

### (γ) Land alpha sub-pass (re-attempt f77f135) AND TG_Light

Combine the reverted alpha fix with new TG_Light registration. Cone billboard alpha-blended + real spillage.

**Pros.** Best-of-both visuals.

**Cons.** Re-enters the state-leak class the f77f135 revert reason documented. The "use a sidecar per-draw SSBO instead of switching programs" approach from the memory's "v2 should..." note is open scope. Vedettes/LRMCs went invisible last time. User said "we couldn't get them to alpha blend right" — strong signal not to try again.

## 4. Recommendation

**(α). Drop the cones, emit real lights. Both static-prop building lamps AND mech spotlights.**

User confirmed: "I don't care about them. would much rather have real lights, and have it actually spill / illuminate." User confirmed mech spotlights "as illumination" not as alpha-blended billboards. Sidesteps the state-leak class entirely.

## 5. Greybeard ruling

1. **Subsystem pin.** `SpotLight_*`-prefixed TG_Shape children of building TG_MultiShapes and mech TG_MultiShapes; render path through `GpuStaticPropBatcher::submitMultiShape` and `gos_mech_batcher.cpp:553-563`. Consumer of "is there a spotlight here" is currently the static-prop opaque draw + the mech batcher's drop.
2. **Symptom vs cause.** Symptom (1): ugly opaque colored triangles for building lamps. Symptom (2): mech spotlights invisible. Upstream condition: the `SpotLight_*` tagging produces geometry-with-special-treatment instead of producing illumination data. The legacy CPU path (txmmgr.cpp:2173 MC2_ISSPOTLGT queue) is dead under Renderer 3.
3. **The meta-fix.** Recognize the tag at submit time; emit a `TG_Light` via `addWorldLight()` instead of (or in addition to — see OQ1) geometry. Same .ase/.cmf author intent ("there's a light here"); different consumer (the lighting pipeline, not the geometry pipeline).
4. **Substitutive test.** The static-prop batcher does NOT draw opaque cones for `SpotLight_*` children. `addWorldLight` slot allocations correspond to those positions. Existing shaders that sample LightsData show illumination spillage around lamp positions in `mc2_05` (night-building canary mission). `event=skip_spotlight` count at gos_mech_batcher.cpp:553-563 drops to 0 — mech spotlights are now routed to the lighting pipeline.
5. **Verdict.** `META-FIX`. Single change retires the opaque-cone bug class AND the mech-invisible-spotlight bug class. Does NOT inherit the f77f135 state-leak class because no blended draw is added.

## 6. Open questions before plan-phase

### OQ1 — Keep the cone OR delete it entirely?

User explicitly said "drop the ugly triangles, real lights only." But: some missions may have content where the visible cone is the desired aesthetic (VTOL searchlight beam through air, atmospheric lamp post). Recommendation: ship (α) drop-cones; if any specific mission needs a visible cone, file as content-side debt (could ship a separate gosFX-pipeline-(B)-particle for that case, post-(B)).

### OQ2 — TG_LIGHT_POINT or TG_LIGHT_SPOT?

`TG_LIGHT_POINT` = omnidirectional falloff. `TG_LIGHT_SPOT` = cone with direction + angle (matches the original cone authoring intent). SPOT requires knowing the cone direction from the .ase/.cmf data. The TG_Shape has a local transform — the cone's forward axis is implicit in that. Need to read the shape's local axes to derive direction at registration time.

Recommendation: POINT for v1 (simpler — no direction extraction), SPOT for v2 if directionality is visually missed.

### OQ3 — Color / intensity authority

The cone geometry has a texture-authored color (the "saturated base to grey tip"). TG_Light has an `aRGB` field (per the bdactor.cpp:2180 comment about worldLights[0]->aRGB). Where does the light color come from?

Options:
- (a) Hardcoded yellow-warm for all spotlights (simplest).
- (b) Sample the spotlight texture's average color at registration time (one texture read).
- (c) Read from .ase/.cmf material properties if present (open question — does the data carry color?).

Recommendation: (a) for v1, (b) for v2 if mission-specific lamp colors matter.

### OQ4 — Falloff distances (closeDistance / farDistance)

`TG_Light::GetFalloff` does piecewise linear from inner to outer. Need numbers. Options:
- Hardcoded (e.g. 5 units inner, 30 units outer) — fastest.
- Derived from the cone shape's bounding box at registration time.
- Per-LightType .fit entry (open question — does mission data carry this? See (D) OQ5).

Recommendation: hardcoded defaults for v1; derived from bbox for v2.

### OQ5 — Shader coverage — RESOLVED 2026-05-20

mc2-render-expert advisor enumerated LightsData consumers, then a follow-up grep CORRECTED the advisor's terrain claim. The actual coverage:

| Shader | Samples LightsData / point+spot? | Path |
|---|---|---|
| [shaders/static_prop.vert:249](shaders/static_prop.vert) | YES (calc_light VS) | Buildings / static props |
| [shaders/mech.vert:162](shaders/mech.vert) | YES (calc_light VS, MC2_GPU_MECH_LIGHTING) | Mech bodies |
| [shaders/gos_tex_vertex_lighted.vert:84](shaders/gos_tex_vertex_lighted.vert) + .frag:54 | YES | Legacy lit vertex path |
| [shaders/gos_terrain_lighting.comp:231](shaders/gos_terrain_lighting.comp) | **YES** for `TG_LIGHT_POINT_GPU` and `TG_LIGHT_SPOT_GPU` | Terrain ground illumination via compute |
| `gos_tex_vertex.frag`, `decal.frag`, `terrain_overlay.frag`, `mech.frag` | NO | Modulate / decal paths |

**The terrain story (corrected from advisor's first pass):** [mclib/camera.cpp:1912](mclib/camera.cpp) at `Camera::updateLights` adds POINT and SPOT lights to `terrainLights[]` via the comparison `light->lightType >= TG_LIGHT_POINT && light->lightType < TG_LIGHT_TERRAIN` (POINT=3, SPOT=4, TERRAIN=5 per [mclib/tgl.h:163-167](mclib/tgl.h)). The compute kernel at line 231 then iterates and applies falloff. Ground illumination from SpotLight_ TG_Lights is **free** — no new shader code, no schema extension. The infrastructure was built for exactly this case.

**Implication for (E) v1:** lamp posts will illuminate building + props + mechs + the GROUND around them via the existing pipeline. No Stage-2 shader follow-up needed for the user's stated visual.

### OQ6 — Budget sizing — RESOLVED 2026-05-20

User decision: **bump CPU world pool to 1024, keep per-shape shader array at 16.**

Rationale per user: stock content is "really simple"; actual SpotLight_ population is "extremely low"; 1024 is plenty of headroom. Per-shape best-16 selection only becomes a problem with dense mech clustering, which is rare. Don't pre-measure; just bump and ship.

Plan-phase actions:
1. **Bump `MAX_LIGHTS_IN_WORLD` from 256 → 1024** in [mclib/tgl.h:170](mclib/tgl.h). Audit the three pool allocations at [mclib/camera.cpp:411, :423, :430](mclib/camera.cpp) (worldLights, activeLights, terrainLights) — they all multiply 256 × sizeof(TG_LightPtr); 1024 × 8 bytes = 8KB per pool, 24KB total. Negligible.
2. **Keep shader-side `MAX_LIGHTS_IN_WORLD = 16`** in [shaders/include/lighting.hglsl:23](shaders/include/lighting.hglsl) (lockstep with `TG_HWLightsData`'s `lightFalloff[16][4]`). Do NOT bump unless a mech-cluster canary visibly demands it.
3. **Mission canaries:** mc2_04 (night mission, the visual proof case) and mc2_10 (existing tier1 stress). Skip the F3 telemetry pre-measurement — sizing is settled.
4. **Fallback path remains documented:** if a future mission overflows 1024 OR per-shape truncation becomes visually objectionable, route through LIGHTBAKE v2 (`mc2WriteStaticLightSlot`). Not needed for v1.

### OQ7 — Day vs night gating

CPU path skips spotlight geometry when `!isNight` ([tgl.cpp:1728](mclib/tgl.cpp)). Does the night gate apply to real illumination too? Probably yes — daytime lamp glow is invisible against bright sun. Need a runtime gate on the addWorldLight call.

Recommendation: gate registration on `isNight` (the same flag tgl.cpp:1728 reads). Re-evaluate at mission state change (day→night transition mid-mission, if such transitions exist).

### OQ8 — Lifecycle (building destruction, mech death) — META-FIX from advisor

Per render-expert greybeard ruling, the canonical pattern already exists at [mclib/mech3d.cpp:3333-3383](mclib/mech3d.cpp) for the mech anubis searchlight. The pattern:

1. **First-visibility (or mission load for static buildings):** `addWorldLight(lightPtr)` ONCE, retain slot index / pointer on the actor.
2. **Per-frame for moving objects:** `SetLightToWorld(...)` + `SetPosition(...)` + toggle `active` based on `inView`. NEVER `removeWorldLight + addWorldLight` per frame — pool churn is O(N) per call.
3. **Destruction:** `removeWorldLight(slot, light)` only at actor destroy event.

(E) generalizes this from "one anubis light" to "N SpotLight_ children." No new design needed.

**Latent leak to NOT inherit:** advisor flagged that existing mech3d.cpp anubis path leaks pointLight at mech destruction (removeWorldLight called only in two narrow contexts). When (E) generalizes the pattern, the destroy hook must be properly wired for both buildings and mechs, per [memory/mission_load_inits_mirror_init_per_subsystem.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\mission_load_inits_mirror_init_per_subsystem.md).

**`active` flag is the cull gate.** [mclib/camera.cpp:1890](mclib/camera.cpp) `updateLights` filters on `active`. Mechs that are `inView==false` MUST set their spotlight's `active=false` so they don't consume per-shape best-16 slots from visible shapes. Same pattern as [mech3d.cpp:3382](mclib/mech3d.cpp).

## 7. Adversarial review of this spec

Greybeard step 9 (cross-cutting state changes) — does (E) change a global rendering convention? It changes a per-shape *treatment* (geometry → light), not a global state. No need for an exhaustive global-convention census. But:

- **R1.** `SpotLight_*` is a node-name prefix match. Are there ANY non-spotlight shapes that incidentally start with "SpotLight_"? Unlikely in stock content but worth a one-time grep of all `.ase`/`.cmf` data files to confirm uniqueness. If false-positives exist, they're already treated as spotlights today (so behavior is consistent).
- **R2.** The `isSpotlight && noShadow=true` invariant ([tgl.cpp:282](mclib/tgl.cpp)) — if (E) deletes the cone but the noShadow flag persists, no harm (the shape no longer renders, the flag is irrelevant).
- **R3.** Mech un-skip introduces NEW shader light contributions from mech positions. Mechs move every frame (when not stationary-suppressed). TG_Light positions must update per-frame for moving mechs — that means `removeWorldLight + addWorldLight` per mech per frame, OR an in-place position update method on TG_Light. The latter likely exists; need to grep.
- **R4.** Per-instance registration vs per-type. Buildings have one TG_TypeShape but many instances. If we register at `registerType`, one light. If at `submitMultiShape`, one per instance per frame (need de-dup). Recommended: register at instance create (mission load for static buildings; spawn time for dynamic mechs), tear down at destruction.
- **R5.** "Stage 2.D.2 fix" comment at [bdactor.cpp:2180-2183](mclib/bdactor.cpp) hints at an ordering trap where building lighting caching depends on `worldLights[0]->aRGB` not being mutated by later actor inits. Adding 100+ new worldLights between sun/ambient and the first mech may interact with this. Read that comment + surrounding code before plan-phase.
- **R6.** Save/load: TG_Light slots are NOT persisted across save/load (worldLights is per-process). Mission load init covers it; building-destroyed-then-savegame requires the same `removeWorldLight` discipline. Need to verify with the saveload-canary memory.
- **R7.** AMD constraints — dynamic light loops on the 7900 XTX have specific gotchas per [docs/amd-driver-rules.md](docs/amd-driver-rules.md). If (E) significantly increases the per-pixel light loop count, need shader review.
- **R8.** The `f77f135` revert message specifically noted the previous attempt broke "Vedettes / LRMCs invisible" via the state-leak. (E) does NOT introduce a new draw call, so the same class doesn't apply — but worth a Vedette-LRMC visual canary on the FIRST smoke after (E) lands to confirm no related regression.

## 8. Suggested staging

Mirror the (A) gosFX retirement staging shape:

- **Stage 0 — instrument.** `MC2_SPOTLIGHT_REAL_TRACE=1` counter on `addWorldLight` calls from the new path + `event=skip_spotlight_drawn` count drop at the static-prop batcher. Tier1 baseline.
- **Stage 1 — gate.** `MC2_SPOTLIGHT_REAL=1` short-circuits the static-prop spotlight cone draw + un-skips mech batcher spotlight + calls addWorldLight at appropriate hook points. Default-off. Tier1 visual canary (mc2_05 night-lamps, mc2_24 mech-heavy).
- **Stage 2 — flip default-on.** Soak. User-driven canary across tier1.
- **Stage 3 — delete the cone path.** Remove the `flags |= (1u << 2)` static-prop submit branch when isSpotlight (since it now does nothing). Remove the mech batcher skip-spotlight branch entirely. Substitutive complete.

## 9. Cross-references

- [memory/spotlight_billboards_static_prop_opaque_bug.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\spotlight_billboards_static_prop_opaque_bug.md) — the original bug analysis, f77f135 root cause
- [memory/feedback_static_prop_subpass_program_switch.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_static_prop_subpass_program_switch.md) — why f77f135 was reverted (state-leak class)
- [memory/cpp_glsl_ubo_struct_lockstep.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\cpp_glsl_ubo_struct_lockstep.md) — if (E) extends TG_HWLightsData
- [memory/mission_load_inits_mirror_init_per_subsystem.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\mission_load_inits_mirror_init_per_subsystem.md) — R6 (mission load/teardown)
- [memory/amd_auto_lod_strict_fail.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\amd_auto_lod_strict_fail.md) + [docs/amd-driver-rules.md](docs/amd-driver-rules.md) — R7
- [docs/superpowers/specs/2026-05-20-light-real-illumination-plumbing-design.md](2026-05-20-light-real-illumination-plumbing-design.md) — (D) sketch, partially superseded for the spotlight subset by this spec
- f77f135 / d91e639 commits — the alpha-blend attempt and its revert
- `mc2-render-expert` advisor — OQ5 (shader coverage), OQ6 (budget routing), R5 (worldLights[0] ordering)
- `mc2-shader-expert` advisor — OQ5 + R7 (AMD)

## 10. Status — READY FOR PLAN-PHASE 2026-05-20

All load-bearing open questions resolved:

| Item | Resolution | Source |
|---|---|---|
| OQ5 (shader coverage) | Terrain compute already handles POINT/SPOT; ground spillage works for free | grep verified against `gos_terrain_lighting.comp:231` + `camera.cpp:1912` |
| OQ6 (sizing) | World pool 256→1024; shader array stays at 16 | User decision 2026-05-20 |
| R5 (worldLights[0] trap) | Sun-only mutation; slots 2+ are untouched per-actor | grep `setLightColor(` shows only `(0,*)` and `(1,*)` writes |
| OQ8 (lifecycle / per-frame update) | Reuse mech3d.cpp:3333-3383 anubis pattern verbatim; no per-frame remove/add | Render-expert greybeard ruling |

Remaining OQ1 / OQ2 / OQ3 / OQ4 / OQ7 can be decided during plan-phase (they're parameter choices, not architectural blockers):
- OQ1 keep/drop cone: drop (user-confirmed)
- OQ2 POINT vs SPOT: POINT for v1, SPOT optional v2
- OQ3 color: hardcoded warm for v1, texture-sample for v2
- OQ4 falloff distances: hardcoded for v1, bbox-derived for v2
- OQ7 day/night gate: match `isNight` flag from CPU path

**Plan-phase actions on day 1:**
1. Bump `MAX_LIGHTS_IN_WORLD` 256→1024 ([mclib/tgl.h:170](mclib/tgl.h)) + audit three pool allocations at [camera.cpp:411,:423,:430](mclib/camera.cpp).
2. Add `MC2_SPOTLIGHT_REAL_TRACE=1` registration counter + `event=skip_spotlight_drawn` count at the static-prop batcher (per the spec's Stage 0).
3. Add the env gate `MC2_SPOTLIGHT_REAL=1` for the four sites: `gos_static_prop_batcher::submitMultiShape` (skip cone for isSpotlight), `gos_mech_batcher.cpp:553-563` (un-skip + register), building spawn (mission load addWorldLight + retain slot), building destroy (removeWorldLight). Default-off.
4. Tier1 + mc2_04 (night) + mc2_10 visual canary. Vedette/LRMC canary on mc2_24 (the f77f135 revert canary class).
5. Flip default-on after soak; delete the gate code at Stage 3.

The spec is now actionable. Plan-phase can proceed without further open questions.
