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

**Key budget reality:** the SHADER side has 16 slots, not 256. The 256-slot world pool is filtered each frame down to the 16 most relevant for the camera's view. So a per-frame culling / scoring step already runs; (E) just adds to its input population.

## 3. Three approaches

### (α) Delete cone geometry, emit TG_Light only (recommended, user's preference)

At `GpuStaticPropBatcher::submitMultiShape` and `gos_mech_batcher.cpp:553-563`: when `child->isSpotlight==true`, SKIP submitting the cone geometry; instead, register a `TG_Light` of type `TG_LIGHT_POINT` (or `TG_LIGHT_SPOT` — see OQ2) at the child's world-space position via `eye->addWorldLight()`. Store the slot index for `removeWorldLight` at building destruction / mech-killed lifecycle events.

**Pros.**
- No new shader code (existing terrain.frag, mech shaders, static_prop.frag already sample LightsData).
- No alpha sub-pass → no glUseProgram swap → no state-leak class (sidesteps `feedback_static_prop_subpass_program_switch.md`).
- Substitutive by construction: cone draws delete, real light registrations replace.
- Real spillage onto surrounding terrain/props — the actual visual the user wants.

**Cons.**
- Loses the visible cone beam (searchlight-through-air, lamp-cone aesthetic). If content relies on the cone being visible (e.g. atmospheric VTOL searchlight), it's gone.
- Building-static spotlights become real lights consuming pool slots; large stock missions may overflow the 16-active-slot shader budget.

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

### OQ5 — Shader coverage

Which materials sample LightsData? Buildings (per `peekCachedLeafLightData`), mechs (per the MultiTransformShape path), and... terrain? props? Need to enumerate shader-side `light_falloff[]` reads in `shaders/`. If terrain.frag doesn't sample LightsData, lamp posts won't illuminate the ground (only the building they're on). Partial visual win.

Recommendation: dispatch `mc2-render-expert` advisor to enumerate. If terrain doesn't sample, file as follow-up shader work; (E) v1 ships with the consumers that already sample.

### OQ6 — Per-frame budget overflow (16 shader slots vs N spotlights in active blocks)

Each terrain block at wolfman zoom may have many lamp posts. If active blocks contain >16 lamps, only the per-frame scoring selects which 16 illuminate. Currently the scoring runs against ~handful of mech/building dynamic lights; adding hundreds of static lamp lights may starve the dynamic lights.

Recommendation: route static spotlights through the LIGHTBAKE v2 path (`mc2WriteStaticLightSlot`) which is recipe-indexed and bakes per-shape — possibly bypasses the 16-slot dynamic budget by emitting pre-baked spillage in `TG_HWLightsData` per-leaf. Need `mc2-render-expert` advisor to confirm the baking-path budget semantics.

### OQ7 — Day vs night gating

CPU path skips spotlight geometry when `!isNight` ([tgl.cpp:1728](mclib/tgl.cpp)). Does the night gate apply to real illumination too? Probably yes — daytime lamp glow is invisible against bright sun. Need a runtime gate on the addWorldLight call.

Recommendation: gate registration on `isNight` (the same flag tgl.cpp:1728 reads). Re-evaluate at mission state change (day→night transition mid-mission, if such transitions exist).

### OQ8 — Lifecycle (building destruction, mech death)

Buildings can be destroyed; mechs can die. Their spotlights should turn off. Need `removeWorldLight` calls at destruction events. Per [memory/mission_load_inits_mirror_init_per_subsystem.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\mission_load_inits_mirror_init_per_subsystem.md), init/teardown must mirror or state leaks across missions.

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

## 10. Recommended next step

Dispatch `mc2-render-expert` advisor to answer OQ5 + OQ6 + R5 with the greybeard skill. The other open questions (OQ1/2/3/4/7/8) can be answered in plan-phase. After OQ5/6/R5 have answers, plan-phase (E) Stage 0.
