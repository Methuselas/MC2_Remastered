# Light (GameObject) Retirement — Design Spec

- **Status:** DRAFT (design only; no code changes)
- **Date:** 2026-05-20
- **Worktree:** `claude/nifty-mendeleev`
- **Sibling specs:** [(A) gosFX retirement](2026-05-20-gosfx-retirement-or-replacement-design.md); [F3 cost-baseline](2026-05-20-cpu-projection-cost-baseline-design.md); [per-object cull recon](../explorations/2026-05-20-per-object-cull-gpu-recon.md)
- **Greybeard verdict:** META-FIX. Delete `Light : public GameObject` and `LightType`. Real lighting (`TG_Light` + `addWorldLight`) is untouched and already ships.
- **All file:line citations grep-verified at write time against `nifty-mendeleev`.**

---

## 1. The naming-collision finding

MC2 has TWO completely unrelated classes named "light":

| Class | File | Purpose | Status |
|---|---|---|---|
| `Light : public GameObject` | [code/light.h:76](code/light.h) | Cosmetic 2D animated billboard sprite (alpha-blended colored triangle masquerading as a glow). Does NOT contribute illumination. | **Retire (this spec)** |
| `TG_Light` (engine class) | mclib/ | Real world-illumination class. Consumed via [mclib/camera.h:769](mclib/camera.h) `addWorldLight(TG_LightPtr)`. Used by [mclib/bdactor.cpp:1939](mclib/bdactor.cpp) (buildings), [mclib/mech3d.cpp:3343](mclib/mech3d.cpp) (mechs), [code/weaponbolt.cpp:1262](code/weaponbolt.cpp) (weapon bolts). | **Untouched (already ships)** |

The class name collision is what made this analysis hard in v1 of the per-object-cull recon. The 950-1000 `projectForObjectAdmission` calls/frame come from the GameObject `Light` class — the billboard-sprite one — not from anything that actually lights the scene. Real per-object lighting is `TG_Light` and is independent infrastructure.

**Conclusion.** When user says "the lights are basically a light colored triangle with an alpha on them; they don't actually supply light" — confirmed by grep. Retiring `Light : public GameObject` does NOT retire real lighting. There is no "build real lighting later" prerequisite; real lighting already runs.

## 2. What `Light` (the GameObject) actually does

- Pool-allocated dynamic GameObject ([code/objmgr.cpp:985-996](code/objmgr.cpp) `getLight()`).
- Uses `VFXAppearance` (the 2D billboard sprite appearance class) — [code/light.cpp:178-182](code/light.cpp).
- `VFXAppearance::render` ([code/actor.cpp:361-413](code/actor.cpp)) writes a 2D `TextureElement` at `(screenPos.x, screenPos.y)` with `appearType->actorStateData[].textureSize`. Alpha-blended sprite at projected world position.
- Per-frame texture-frame animation ([code/actor.cpp:454-490](code/actor.cpp)), terrain-light coloring ([code/actor.cpp:385](code/actor.cpp)), depth offset ([code/light.cpp:25](code/light.cpp) `LIGHT_DEPTH_FIXUP = -500`).
- Position set once at spawn ([code/light.cpp:112-113](code/light.cpp)), never updated thereafter.
- Each pool slot calls `Light::update` → `GameObject::onScreen()` → `projectForObjectAdmission` once per frame for the entire pool's `exists=true` population. F3 baseline: ~950-1000 calls/frame.
- `createLight` ([code/objmgr.cpp:3461](code/objmgr.cpp)) is **dead code** (zero callers). Live spawn path is polymorphic `LightType::createInstance` via mission `.fit` data load.

## 3. Substitutive test (greybeard step 4)

For (C) to be "done":

- `Light` class and `LightType` class removed from the build (or stubbed to no-op).
- `numLights` set to 0 (or the pool reclaimed).
- `code/light.cpp` and `code/light.h` reduced to stubs or deleted.
- `mc2.exe` F3 capture shows `eventdriven_projection_total` falls by ~1000/frame (the entire Light contribution).
- Visual regression: cosmetic glow sprites disappear (lamp posts, building window glows, ambient atmosphere). Per stock-playability rule, missions still progress.
- Real lighting (`TG_Light` + `addWorldLight`) confirmed untouched by post-retirement smoke.

Substitutive by construction: producer (`Light::update`) deleted; sole consumer (`Light::render`) deleted; pool deleted.

## 4. Adversarial review

Grep-verified at write time:

- **ABL hooks.** Grep `code/abl/` for `light` (case-insensitive, excluding `lighting`/`copyright`/`highlight`/`delight` false-positives): **zero matches**. No mission script gates on Light presence.
- **Save/load persistence.** Grep `code/saveload.cpp` for Light persistence: only `HighlightColor` strings (unrelated mech damage data). `Light` GameObjects are dynamic pool members, not persisted.
- **AI hooks.** Grep for `isLight`, `ObjectClass.*LIGHT`, `LIGHT_CLASS`: only matches are `isLightMechSpecialist` / `isLightACSpecialist` (pilot skill, unrelated). No AI iterates Light specifically.
- **Weapon bolt coupling.** [code/weaponbolt.cpp:1261-1269](code/weaponbolt.cpp) calls `((WeaponBoltTypePtr)getObjectType())->getLight()` returning `TG_LightPtr` (not GameObject Light). Different class entirely. Untouched by this retirement.
- **mc2.fx coupling.** The gosFX retirement spec § keeps `mc2.fx` on disk. Light data is in separate `.fit` files (LightType objects); retirement deletes the LightType loader path. mc2.fx is unaffected.
- **GameObjectManager pool churn.** [objmgr.cpp:2341-2345](code/objmgr.cpp) iterates `lights[i]` per frame; this loop body is deleted along with the array. No fallout.

**Residual risks:**

- **R1.** Visual gap is larger than gosFX retirement perceptually. gosFX retirement loses explosion/muzzle/hit visuals during combat. (C) retirement loses ambient glow visuals that are present at all times in some missions (lamp posts, etc.). Mission "feel" degrades more in ambient/atmospheric scenes than combat-only scenes.
- **R2.** Unknown population: F3 stationary baseline shows ~950-1000 active concurrent. If these are all mission-static decoratives, every mission's ambient look changes. If a fraction are dynamic (spawned by weapon hits etc.), only combat scenes are affected. Worth a one-mission visual canary post-retirement to characterize.
- **R3.** Per-particle terrain-light color sampling — a feature `Light` had ([code/actor.cpp:385](code/actor.cpp)) that the (B) GPU particle pipeline does NOT require by default. Since (C) deletes `Light` entirely, (B) does NOT need to absorb this feature. The L3 absorption analysis from the per-object-cull recon is now obsolete; (B) scope reduces.
- **R4.** `LightAppearance` / `lightAppearance` — these may have refs elsewhere in `mclib/appear.cpp`. Need to ensure deletion is clean (orphan-walk the symbols). Per the gosFX spec's compile-time discovery mechanism: delete the type, let the compiler enumerate breakage.

None of R1-R4 invalidate the retirement. All are scoping inputs for the plan-phase.

## 5. Three approaches (honest matrix)

### (A') Delete entirely now (recommended)

Stub `Light::update` and `Light::render` to no-op, then delete the class behind an env-gate (`MC2_DISABLE_LIGHT_GAMEOBJECT=1`), then flip default-on, then delete code. Same staging shape as (A) gosFX retirement Stage 0-4.

**Pros.** Substitutive by construction; eliminates 1000 calls/frame (potentially ~1ms per the F3 mc2_10 memory); cleanest possible architecture (kills the naming collision; one class named "light", and it's the real one).

**Cons.** Ambient/atmospheric visual gap during transitional period. No automated parity oracle (visual canary only).

### (B') Cache + block-cull (the per-object-cull recon's L1+L2)

Keep `Light`, optimize its CPU cost via screenPos caching + block-based pre-cull. Reduces calls/frame by ~99% stationary, ~85% motion.

**Pros.** Zero visual regression.

**Cons.** Keeps the wrong abstraction (lights as GameObjects). Doesn't unblock anything architectural. The 1000 calls/frame become ~10-100/frame; still alive, still measured, still a tax on every future change to `GameObject` / `onScreen` / projection.

### (C') Absorb into (B) GPU particle pipeline

Move Light's content into (B). The recon's L3.

**Pros.** Preserves visual fidelity.

**Cons.** Adds scope to (B) — per-particle terrain-light, depth-fixup, LightType .fit loader. (B) is already required-for-ship debt; bloating its scope risks (B) shipping slower. And (B)'s parity oracle problem (no byte-compare against MLR particles) applies again here.

## 6. Recommendation

**Ship (A') — delete entirely.** Same architectural shape and same risk profile as (A) gosFX retirement. The "real lighting" justification the user invoked already exists as `TG_Light` and ships in production. No build-X-first debt.

If post-deletion visual canary shows specific missions need certain ambient glows, those can be reinstated case-by-case as (B)-pipeline content, not as a full `Light` reanimation.

**Reject (B').** Preserves wrong abstraction; defers the cleanup permanently.

**Reject (C').** Adds non-essential scope to (B); the user's explicit framing was "rip it out, replace with actual light later" — i.e. the visual debt is acceptable.

## 7. Open questions for the user before plan-phase

1. **Visual fidelity floor.** Symmetric to gosFX Q1: is "no cosmetic glow sprites" acceptable as a shipping state, or only as a transitional state? Recommendation: transitional only, file "ambient glow restoration via (B)" as ship-required debt analogous to (B) particle reinstatement.
2. **Stage ordering vs (A) gosFX.** (A) and (C) are independent slices. Both can ship in parallel; the F3 perf claim becomes additive. Recommendation: ship in whichever order the plan-phase prefers.
3. **`numLights` pool reclamation.** Hard zero (set the configured count to 0 in mission .fit data load) OR soft zero (allocate the array but skip iteration)? Recommendation: hard zero — simpler retirement.
4. **Editor convergence.** Same as (A): runtime exe scope; editor transitions separately.

---

## Appendix A — grep-verification log (write-time)

- `class Light` definition: [code/light.h:76](code/light.h).
- `class LightType` definition: [code/light.h:40](code/light.h).
- `Light::update`: [code/light.cpp:107-132](code/light.cpp).
- `Light::render`: [code/light.cpp:136-148](code/light.cpp).
- `VFXAppearance::render` (the actual 2D billboard draw): [code/actor.cpp:361-413](code/actor.cpp); reads `screenPos.x/y` at line 379.
- `getLight()` pool allocator: [code/objmgr.cpp:985-996](code/objmgr.cpp); ring-buffer reuse.
- `createLight` (dead code, zero callers): [code/objmgr.cpp:3461](code/objmgr.cpp).
- Pool iteration: [code/objmgr.cpp:1719-1727](code/objmgr.cpp), [:1876-1878](code/objmgr.cpp), [:2341-2345](code/objmgr.cpp), [:3657-3663](code/objmgr.cpp).
- `numLights` config: [code/objmgr.cpp:478](code/objmgr.cpp), [:583](code/objmgr.cpp).
- `TG_Light` (the REAL lighting class): [mclib/camera.h:769](mclib/camera.h) `addWorldLight(TG_LightPtr)`; consumers `mclib/bdactor.cpp:1939`, `mclib/mech3d.cpp:3343`, `code/weaponbolt.cpp:1261-1262`. Untouched by this retirement.
- ABL grep for `\blight\b` in `code/abl/`: zero matches (excluding false-positives `lighting`/`copyright`/`highlight`).
- Save/load grep for `Light`: only `HighlightColor` strings (unrelated).

## Cross-references

- [docs/superpowers/specs/2026-05-20-gosfx-retirement-or-replacement-design.md](2026-05-20-gosfx-retirement-or-replacement-design.md) — (A) gosFX retirement, same staging shape
- [docs/superpowers/explorations/2026-05-20-per-object-cull-gpu-recon.md](../explorations/2026-05-20-per-object-cull-gpu-recon.md) — v2 recon; this spec supersedes its L1/L2/L3 recommendation
- [memory/light_is_2d_billboard_effect_shape_identical_to_cardcloud.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\light_is_2d_billboard_effect_shape_identical_to_cardcloud.md) — the analysis underlying this spec
- [memory/f3_tier1_baseline_2026_05_20.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\f3_tier1_baseline_2026_05_20.md) — F3 stationary baseline
- [memory/f3_mc2_10_worstcase_2026_05_20.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\f3_mc2_10_worstcase_2026_05_20.md) — F3 worst-case (parallel session, "potentially ~1ms" eventdriven)
- [memory/stock_install_must_remain_playable.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\stock_install_must_remain_playable.md) — playability constraint satisfied
- [memory/feedback_offload_must_be_substitutive_not_additive.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_offload_must_be_substitutive_not_additive.md) — substitutive by construction
