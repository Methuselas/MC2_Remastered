# (D) Real Lighting Plumbing — Design Sketch

- **Status:** DRAFT SKETCH — revised 2026-05-20 after `USE_LIGHT_APPEARANCE` discovery (open questions before plan-phase)
- **Date:** 2026-05-20
- **Worktree:** `claude/nifty-mendeleev`
- **Companion:** [(C) Light GameObject retirement](2026-05-20-light-gameobject-retirement-design.md) deletes the pool-occupying ghost class; (D) routes the same mission-static `.fit` data into the live `TG_Light` pipeline so it actually illuminates.
- **Revision note (2026-05-20 R2):** Original framing said "(C) deletes the cosmetic billboard, (D) replaces it with real light." Investigation revealed `USE_LIGHT_APPEARANCE` is referenced only inside three `#ifdef` blocks in `code/light.cpp` and is NEVER defined anywhere. There IS no cosmetic billboard currently rendering — the entire visual path was disabled long ago. So (D) is not a replacement; it's the FIRST TIME mission-static lights actually illuminate anything. The visual delta from (D) is purely additive — currently dark, will glow.
- **Greybeard angle:** META-FIX. (C) deletes ghost code; (D) makes mission-static light data actually do work. Same .fit data; different consumer; one of them does nothing today and the other illuminates.
- **All file:line citations grep-verified at write time against `nifty-mendeleev`.**

---

## 1. The plumbing already exists — large fraction

Grep reveals that real per-object dynamic lighting is far more present than the v1/v2 recon credited:

| Element | Where | Status |
|---|---|---|
| `MAX_LIGHTS_IN_WORLD = 256` budget | [mclib/tgl.h:170](mclib/tgl.h) | Production |
| Light types `AMBIENT/INFINITE/INFINITE_WITH_FALLOFF/POINT/SPOT` | [mclib/tgl.h:163-167](mclib/tgl.h) | Production |
| `worldLights[256]` array | [mclib/camera.cpp:411](mclib/camera.cpp) | Production |
| `activeLights[256]` (per-frame filtered set) | [mclib/camera.cpp:423](mclib/camera.cpp) | Production |
| `terrainLights[256]` (terrain-specific subset) | [mclib/camera.cpp:430](mclib/camera.cpp) | Production |
| `worldLights[0]` = sun (TG_LIGHT_INFINITE) | [mclib/camera.cpp:417-418](mclib/camera.cpp) | Production |
| `worldLights[1]` = ambient (TG_LIGHT_AMBIENT) | [mclib/camera.cpp:444-445](mclib/camera.cpp) | Production |
| `addWorldLight(TG_LightPtr)` API | [mclib/camera.h:769](mclib/camera.h) | Production — first-empty-slot scan, returns slot index or -1 |
| `removeWorldLight(slotNum, light)` API | [mclib/camera.h:~790](mclib/camera.h) | Production |
| `TG_HWLightsData` GPU struct | [mclib/bdactor.cpp:1883](mclib/bdactor.cpp) | Production |
| Static-light baking pipeline (`mc2WriteStaticLightSlot`, `mc2GetBakedStaticLight`) | [mclib/txmmgr.cpp:1205-1253](mclib/txmmgr.cpp) | Production [LIGHTBAKE v2] |
| Per-leaf cached static light data | [mclib/bdactor.cpp:1895](mclib/bdactor.cpp) (`peekCachedLeafLightData`) | Production |
| Live `addWorldLight` consumers | [mclib/bdactor.cpp:1939](mclib/bdactor.cpp) (buildings), [mclib/mech3d.cpp:3343](mclib/mech3d.cpp) (mechs), [code/weaponbolt.cpp:1262](code/weaponbolt.cpp) (weapon bolts) | Production |

**What this means.** Mechs, buildings, and weapon bolts already emit real `TG_Light` instances into the 256-slot world light pool. Existing shaders that sample `LightsData` (terrain / props / mechs — to be confirmed in §3.OQ1) already illuminate accordingly. The "real lighting" the user wants is NOT a future-build; it's a pipeline that ships and serves three real consumer classes already.

**The gap.** Mission-static `Light : public GameObject` instances do NOT enter `worldLights[]`. They were billboard-sprite GameObjects, not `TG_Light` registrations. The 950+ positions encoded in mission `.fit` data go to the cosmetic sprite pool, not the real lighting pool. (C) deletes the cosmetic system; (D) routes the same .fit data into the real one.

## 2. Minimal-viable plumbing (the gap-sized fix)

**Sketch.** At mission load, for each LightType-class GameObject the .fit parser produces:

1. Read position, color, falloff radius, intensity from the .fit data (already parsed for the deleted Light GameObject).
2. Construct a `TG_Light` of type `TG_LIGHT_POINT` (or `TG_LIGHT_INFINITE_WITH_FALLOFF` if the mission data expresses a directional cone; check mc2.fx-style data fields).
3. Call `eye->addWorldLight(lightPtr)`. If slot returned (0..255), light is live. If -1, slot pool was full — fall back to either: (a) silently drop, (b) prioritize by distance-to-camera-spawn-point at mission load, (c) emit the LIGHTBAKE static slot path instead (which is per-recipe-indexed, may not hit the 256 budget).
4. Store the slot index for `removeWorldLight` at mission end.

That's the entire fix. No new shader. No new SSBO. Reuses everything.

**Substitutive test.** Real illumination spills from mission-static light positions onto terrain/mechs/buildings/props in shader output. Verify via mc2_10 visual canary (lamp positions glow on surrounding ground at night/dusk).

## 3. Open questions (need user / advisor input before plan-phase)

### OQ1 — Shader coverage

`LightsData` is sampled by mech shaders (confirmed via bdactor caching pattern) and buildings (per `peekCachedLeafLightData`). Is it sampled by terrain shaders too? By GPU static-prop shaders? Need `mc2-render-expert` or `mc2-shader-expert` advisor to enumerate consumers in `shaders/`. If terrain doesn't sample `LightsData`, lamp posts won't illuminate the ground — the visual win is partial.

### OQ2 — Budget overflow strategy

256-slot world budget vs ~950 candidate mission-static lights. Three options:
- **(a) First-N drop.** Simple; non-deterministic visual loss.
- **(b) Distance-to-spawn prioritization.** Pick the 256 lights closest to mission spawn camera. Deterministic but spawn-biased.
- **(c) Static-light baking integration.** Route into `mc2WriteStaticLightSlot` (the LIGHTBAKE v2 path) which is recipe-indexed and bakes into per-shape leaf data — possibly bypassing the 256-slot budget at the cost of being static-only (no per-frame attenuation/animation).

Strong intuition: (c) is right for mission-static lights because they ARE static; the baking system was built for exactly this shape. But (c) requires understanding the recipe-index keying for non-shape lights (currently baking is per-prop-leaf-recipe; ad-hoc light positions need a different key). Surface to `mc2-render-expert` advisor.

### OQ3 — Perf cost of doubling worldLights population

Existing `worldLights` has ~handful of sun/ambient/mech/building/weapon-bolt entries per frame. Adding 256 mission-static lights doubles or 10×s the per-pixel light-loop count for terrain/mech/prop shaders that iterate `LightsData`. F3 PCs may handle it; this is a GPU regression that wasn't in the budget. Worth a one-mission GPU-time canary before committing to (a) or (b); (c) avoids this entirely.

### OQ4 — AMD driver rules

Per [docs/amd-driver-rules.md](docs/amd-driver-rules.md) and [memory/amd_auto_lod_strict_fail.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\amd_auto_lod_strict_fail.md), AMD 7900 XTX has specific gotchas for dynamic light loops. Need `amd-shader-reviewer` skill applied to the (D) shader work if any new shader code lands.

### OQ5 — Light type from .fit data

LightType (.fit data) has `oneShotFlag` and `altitudeOffset`. Does it have radius, color, intensity, falloff? Need to read `LightType::init` at [code/light.cpp:51-78](code/light.cpp) to enumerate the actual schema. If the .fit data is impoverished (e.g. position-only), (D) needs to invent reasonable defaults per LightType ID. If the .fit data is rich, (D) just routes fields.

### OQ6 — Ship sequencing

Options:
- **Ship (C) first, soak, then (D).** Lights go dark (transitional), then real illumination spills in. Two visible visual transitions; clean staging.
- **Ship (C) and (D) together.** Single visible transition: billboard cosmetic glows replaced by real-lit spillage. Cleaner from a player-perception standpoint but couples two slices.
- **Ship (D) first under env-gate, (C) second.** Real lighting comes online; later (C) cleans up the now-redundant billboard system.

Recommendation: **(C) first, then (D) on a near-term milestone.** (C) is plan-ready; (D) has 5 open questions. Don't gate (C) on (D)'s scoping work. Mirror the (A)/(B) pattern.

## 4. Adversarial review of this sketch

Greybeard step verification:

1. **Subsystem pin.** Lighting infrastructure (`mclib/camera.h`/`camera.cpp` + `mclib/tgl.h` + `mclib/txmmgr.cpp` baking) + mission .fit Light data path (currently feeding the dying GameObject Light pool).
2. **Symptom vs cause.** Symptom: lights don't actually light anything. Upstream condition: mission .fit Light data has been routed to a billboard-sprite system since the OpenGL port; the real per-object lighting pipeline was built for mechs/buildings/weapons in parallel but never extended to consume mission-static light positions.
3. **Meta-fix.** Route the .fit data into `addWorldLight` or `mc2WriteStaticLightSlot`. Single producer of mission-static lights; multiple existing consumers. No new shader work if shader coverage (OQ1) is acceptable.
4. **Substitutive test.** Real illumination visible at lamp positions in mc2_10 visual canary; no new CPU cost; potentially new GPU cost (OQ3).
5. **Verdict.** `META-FIX` candidate — but underspec until OQ1-OQ5 are answered. The verdict is "ship the shape; the parameters need data."

**Critical risks the sketch might be missing:**

- **R1.** Maybe mission-static "lights" aren't placed where real illumination would be visually correct. E.g. a lamp post lighthead is at the .fit position, but the actual emitter (the bulb) is offset by a few meters; the light spills onto the wrong patch of ground. Worth a one-mission canary before committing.
- **R2.** `worldLights` is a process-lifetime pool. Per `mission_load_inits_mirror_init_per_subsystem.md`, mission load/teardown must add/remove slots cleanly or the next mission inherits stale lights. (D) must wire init AND teardown.
- **R3.** Save/load: are TG_Light slots persisted across save/load? If not, mission-load init covers it. If yes, (D) needs serialization. Grep `saveload.cpp` for `worldLights`/`TG_Light` references.
- **R4.** The LIGHTBAKE v2 system (txmmgr.cpp:1205-1253) is per-prop-leaf-recipe; extending it to per-mission-light may require a new keying scheme. Possibly bigger scope than first-N world-light registration.
- **R5.** "Stage 2.D.2 fix" comment at [bdactor.cpp:2180-2183](mclib/bdactor.cpp) hints at an ordering trap where building lighting depends on `worldLights[0]->aRGB` not being mutated by later actor inits. Adding 256 new entries between sun/ambient and the first mech may interact with this ordering. Read that comment + surrounding code before scoping.

## 5. Cross-references

- [docs/superpowers/specs/2026-05-20-light-gameobject-retirement-design.md](2026-05-20-light-gameobject-retirement-design.md) — (C) retirement; creates the gap (D) fills
- [docs/superpowers/specs/2026-05-20-gosfx-retirement-or-replacement-design.md](2026-05-20-gosfx-retirement-or-replacement-design.md) — (A); same staging pattern as (D) is to (C)
- [memory/light_is_2d_billboard_effect_shape_identical_to_cardcloud.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\light_is_2d_billboard_effect_shape_identical_to_cardcloud.md) — naming-collision finding underlying (C)/(D) split
- [memory/cpp_glsl_ubo_struct_lockstep.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\cpp_glsl_ubo_struct_lockstep.md) — if (D) extends TG_HWLightsData
- [memory/amd_auto_lod_strict_fail.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\amd_auto_lod_strict_fail.md) — AMD constraints on dynamic light loops
- [docs/amd-driver-rules.md](docs/amd-driver-rules.md) — broader AMD rules
- [memory/mission_load_inits_mirror_init_per_subsystem.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\mission_load_inits_mirror_init_per_subsystem.md) — R2 (mission load/teardown ordering)
- `mc2-render-expert` advisor (`.claude/agents/mc2-render-expert.md`) — OQ1 (shader coverage), OQ2 (budget strategy), R5 (Stage 2.D.2 ordering trap)
- `mc2-shader-expert` advisor — OQ1, OQ4 (AMD rules for light loops)

## 6. Recommended next step

Do NOT plan-phase this spec yet. Instead:

1. **Dispatch `mc2-render-expert` advisor** to enumerate `LightsData` shader consumers (OQ1), evaluate the LIGHTBAKE-v2 keying for ad-hoc light positions (OQ2c), and explain the Stage 2.D.2 ordering trap (R5). Include "run the greybeard skill" verbatim per CLAUDE.md.
2. **Read `LightType::init` in light.cpp:51-78** to enumerate the .fit schema (OQ5). Trivial; can do alongside (C) plan-phase.
3. **Once OQ1-OQ5 have answers,** the (D) sketch firms into a real spec with: chosen approach for OQ2 (a vs b vs c), shader coverage matrix, budget headroom estimate, R5 ordering plan, .fit schema mapping.

Then plan-phase. Don't write the plan against unanswered open questions.
