# PLAN: Static-actor lighting mission-load bake (Option B, lazy-first-frame)

> Branch `claude/gpu-driven-rendering`, worktree
> `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-driven-rendering\`.
> Anchors grep-verified 2026-05-17 @ `2dca942`. RE-GREP at edit-time.
> Recon: `docs/superpowers/plans/progress/2026-05-17-static-lighting-mission-load-bake-recon.md`
> + gameos feasibility pass (this session). Follows the shipped
> LIGHTBRIDGE D2 slice (`2dca942`).

## Premise corrections vs the recon (load-bearing — read first)

1. **Trigger is NOT `registerRecipe`.** gameos feasibility proved
   `TG_Shape::s_listOfLights`/`s_numLights` are set ONLY at
   update/render via `SetLightList` (`bdactor.cpp:2444` buildings,
   `:4894` trees); never in the registration path. `registerStatic`
   only runs `TransformMultiShape_BuildRecipe` (positions-only) —
   in-code confirmed at `bdactor.cpp:2959-2968`. `GatherLightsParameters`
   CANNOT run at `registerRecipe`. **Corrected trigger: lazy bake on the
   actor's FIRST `CacheGpuLightData()` (frame 1); short-circuit
   thereafter.** Every other recon conclusion stands.
2. **Bakeable static class = bdactor buildings + trees ONLY.** Generic
   props call `genactor.cpp:1201 SetLightList(NULL,0)` (zaps
   `s_listOfLights`, comment `:1217`) → they take the
   `no_actor_light` branch in `addLightDataStructureWithPerActorColor`
   (`txmmgr.cpp:1208`), not the per-actor template path the bake
   targets. Generic props are OUT of scope (unaffected). Mechs
   (`mech3d.cpp:4501`) are dynamic — keep the D2 cheap per-frame path.
3. **Gate lives bdactor-side, not inside `CacheGpuLightData()`.** The
   stable key `staticReg.recipeIndex` is a `BldgAppearance` member
   (`bdactor.cpp:3002-3005`, `GpuStaticPropRegistry::invalidate(
   staticReg.recipeIndex)`); it is NOT reachable inside
   `TG_MultiShape::CacheGpuLightData()` (`msl.cpp:1892`, which has only
   `cachedGpuLightIndex_`/`cachedFrame_`). The recon's "one chokepoint
   inside CacheGpuLightData" is therefore not implementable as written;
   the decision must live where `recipeIndex` exists = the 4
   `BldgAppearance` call sites. Mitigation: ONE new `BldgAppearance`
   helper called at those 4 sites (not logic duplicated 4x).

## Goal

For the genuine static class (bdg/trees), retire the per-frame
RECOMPUTE of `TG_HWLightsData` (`GatherLightsParameters` +
`decomposeFirstActiveLightColor` + per-frame template hash) — it is a
mission-load constant (position-derived `getTerrainLight` + frozen
sun/`nightFactor`; no dynamic emitters per
`lighting_is_mission_load_static_no_dynamic_emitters.md`). The per-frame
slot WRITE remains O(1) (terrain Shape-C precedent: the recompute dies,
the per-frame consumer stays). Mechs unaffected; D2 stays their floor.

## Honest perf framing (do not soften, do not inflate)

The recon §4 called this "perf-marginal post-D2." The D2 ON/OFF capture
(2026-05-17) REFUTED that: D2 retired only the FNV+memcmp (~390 ns/call,
~0.6-0.75 ms/frame); the surviving `addLightDataStructureWithPerActorColor`
body is **~2.0 ms/frame** (1792B copy + decompose + template-map + the
GatherLights-on-miss). The bake retires the recompute portion of that
residual for the bdg/tree population. So this IS a real lever, NOT
marginal. BUT: per `feedback_offload_must_be_substitutive_not_additive`
"done = total frame drops ON vs OFF; zone↓ alone is not proof
(drawPass)". **The total-frame anti-mirage proof is USER-WAIVED for this
slice (explicit override 2026-05-17).** The plan ships on the
substitutive structure + parity, not a claimed frame number. Commit
message states the populate-body delta in code dimensions, no
wall-clock, and records the waiver.

## Files / symbols (re-grep at edit-time)

- `mclib/bdactor.cpp`: `BldgAppearance::invalidateStaticRegistration`
  `:3002` (calls `GpuStaticPropRegistry::invalidate(staticReg.recipeIndex)`
  `:3005`); `SetLightList` then `CacheGpuLightData` call sites —
  buildings `:2444`/`~:2473`, `:2521`; trees `:4894`/`:4912`, `:4933`;
  `staticReg.recipeIndex` member. The 4 `*->CacheGpuLightData()` static
  call sites are the gate insertion points.
- `mclib/msl.cpp` / `msl.h`: `TG_MultiShape::CacheGpuLightData` `:1892`
  (sole populate `firstShapeNodeLeaf->GatherGpuObjectLightDataOnly()`
  `:1918`); `cachedGpuLightIndex_` `msl.h:276`, `cachedFrame_` `:286`,
  `getCachedGpuLightIndex` `:337`.
- `mclib/tgl.cpp`: `GatherGpuObjectLightDataOnly` `:2858` →
  `addLightDataStructureWithPerActorColor`; `ResubmitCachedLightData`
  `:2865`.
- `mclib/txmmgr.cpp`: `addLightDataStructureWithPerActorColor` `:1196`
  (D2 small-key path `:1240-1261`); `addLightDataStructure` `:1110`;
  `resetLightData` `:1232`; `[LIGHTBRIDGE v1]` accumulator `:1061`.
- `GameOS/gameos/gos_static_prop_registry.cpp`: `GpuStaticPropRegistry::
  invalidate` (recipeIndex teardown); `registerRecipe` `:230`.

## Design (Option B, lazy-first-frame, recipeIndex-keyed)

### B1. Mission-scoped baked-light cache
`std::unordered_map<int32_t /*recipeIndex, monotonic, never reused*/,
TG_HWLightsData>` owned by `GpuStaticPropRegistry` (mission lifetime;
NOT touched by `resetLightData`). `recipeIndex` is `int32_t` with
sentinel `-1` (`gos_static_prop_registry.h:66`, guard pattern
`bdactor.cpp:3004 staticReg.recipeIndex >= 0`); it is MONOTONIC and
never reused (`registerRecipe` always push_back; `invalidate` tombstones
count=0, never frees the slot — review-confirmed) so a post-invalidate
re-bake gets a FRESH key with zero stale-key aliasing hazard. Cleared on
mission unload (where the registry itself is cleared — confirm exact
site at edit-time). Per-entry erased inside
`GpuStaticPropRegistry::invalidate(recipeIndex)` (one added line) — this
is for GROWTH-BOUNDING (one dead entry per LOD/damage swap), NOT
correctness (the fresh monotonic key already guarantees re-bake). The
swap path already routes through `invalidateStaticRegistration` →
`invalidate` (`bdactor.cpp:3002-3005`, `:1720/:1726/:1767/:1786`),
forcing a lazy re-bake of the same position-derived constant for the
new multi. No new swap-detection.

### B2. `BldgAppearance` gate helper
New `BldgAppearance::cacheOrBakeGpuLightData(TG_MultiShape* shape)`
replacing the 4 raw `shape->CacheGpuLightData()` static calls
(`bdactor.cpp` bldg `~:2473`/`:2521`, tree `:4912`/`:4933`):
- Invalid guard is `!staticReg.registered || staticReg.recipeIndex < 0`
  (int32_t sentinel `-1`, NOT a uint `0xFFFFFFFF`). Invalid OR bake
  kill-switch OFF → `shape->CacheGpuLightData()` (unchanged legacy/D2
  path). Mechs never reach this helper (they call `mech3d.cpp:4501`
  directly) → untouched.
- Cache MISS for `recipeIndex` (first frame, or post-invalidate):
  `shape->CacheGpuLightData()` as normal (runs the real
  `SetLightList`-fed gather), THEN snapshot the post-`decompose`
  per-actor struct. **Snapshot source (M1, required — review-corrected):
  `firstShapeNodeLeaf->lightData_`** (the `TG_Shape` member,
  `tgl.h:746`). After `CacheGpuLightData()` returns,
  `GatherGpuObjectLightDataOnly` (`tgl.cpp:2858-2860`) has written the
  post-template-copy + in-place `decomposeFirstActiveLightColor`
  (`txmmgr.cpp:1225/1228`) struct durably into `firstShapeNodeLeaf->
  lightData_`, which `resetLightData` does NOT touch. Expose
  `TG_MultiShape::peekCachedLeafLightData()` that re-runs the SAME
  first-SHAPE_NODE-leaf scan as `CacheGpuLightData` (`msl.cpp:1904-1915`)
  for leaf identity and returns `&firstShapeNodeLeaf->lightData_`.
  **Do NOT snapshot via `cachedGpuLightIndex_`/`lightData_[idx]`** — that
  per-frame scratch slot is wiped by `resetLightData` and overwritten by
  sibling actors same-frame (the exact hazard `staticReg.lightDataIndex`
  at `:2482` was added to dodge). That option is DELETED.
- Cache HIT (frame ≥ 2, unchanged): skip `CacheGpuLightData()` entirely;
  call new `TG_MultiShape::EmitBakedGpuLightData(const TG_HWLightsData&)`
  → resolves a per-frame slot for the baked constant (reuse
  `addLightDataStructure` + D2 small-key so per-frame dedup still
  works) and sets `cachedGpuLightIndex_`/`cachedFrame_`. This is the
  Shape-C "recompute dies, O(1) per-frame consumer stays" shape:
  `GatherLightsParameters`+`decompose`+template-hash are GONE; only the
  slot write remains.

### B3. Cull-gate adjacency (CRITICAL — `cull_gates_are_load_bearing`)
The helper is invoked at the existing static call sites which are
already INSIDE the `inView || g_useGpuStaticProps` cull gate
(`bdactor.cpp:4897-4898` comment). The baked value may be STORED for a
culled actor but is only EMITTED into a per-frame slot when the actor is
visible — i.e. the helper is only reached when already past cull, exactly
today's behavior. The bake MUST NOT compute/emit for culled actors by
reaching past the gate. Adversarial review item.

### B4. Kill-switch + lifecycle (same commit)
Env `MC2_LIGHTBAKE` (default ON; `=0` → every static actor takes the
unchanged D2/legacy `CacheGpuLightData` path, bit-for-bit). Free fn
`mc2LightBakeEnabled()` (mirror `mc2LightBridgeRepointEnabled`).
`[LIGHTBAKE v1]` lifecycle: `event=enabled mode=bake|passthrough`,
`event=first_bake recipe=<n>`, `event=rebake_on_invalidate` (first
only), `event=teardown`. Gated, demote-not-delete. The existing
`[LIGHTBRIDGE v1]` accumulator already measures the armed delta: with
the bake ON the bdg/tree population stops reaching the
`GatherLights`/`decompose` path → `tmpl_hit`/populate-ns for that
population drops; no new sizing instrumentation needed.

### B5. nightFactor assumption (documented invalidation trigger)
Bake validity assumes `eye->nightFactor`/`eye->getNightFactor()`
(`bdactor.cpp:2226,2253`) is mission-constant (no time-of-day; per the
load-bearing memory). Name it in code at the bake site as the
designated revisit-trigger if a future day/night or dynamic-light
feature lands. NOT machinery built ahead of need — a comment + the
kill-switch are the safety valve.

## Parity / verification (soak waived; probes substitute)

- Adversarial-plan-review on THIS plan != STOP THE LINE; cull-gate
  adjacency (B3), recipeIndex key stability across swap (B1/B2), the
  bdactor-gate-not-chokepoint divergence, and bit-identity of the baked
  constant vs the per-frame recompute for the unchanged static actor
  all accepted.
- Parity probe: env-gated assert that, for a baked actor, the baked
  `TG_HWLightsData` is bit-equal to what a fresh
  `GatherGpuObjectLightDataOnly` would produce this frame (sample 1/N
  baked actors/frame; the substitutive-correctness proof in lieu of a
  calendar soak, per `feedback_soak_waiver_with_probes_and_reviews_validated`).
- tier1 5/5 with `MC2_LIGHTBAKE` default (bake active) + `GL_INVALID_*`=0
  + `+0` destroy delta.
- Visual canary: bake on/off side-by-side, heaviest mission — bdg/tree
  lighting bit-visually identical (the aRGB landmine surfaces as
  wrong/hot static-prop colors).
- Memory: append outcome to
  `lighting_is_mission_load_static_no_dynamic_emitters.md` /
  `feedback_offload_must_be_substitutive_not_additive.md`. Commit
  carries the populate-body delta in code dimensions; records the
  user-waived total-frame proof.

## Out of scope
Option A persistent UBO partition (revisit only if a future dynamic-light
feature forces the static/dynamic buffer split). Generic props
(no-actor-light path). Mechs (D2 is their floor). Total-frame Tracy proof
(user-waived for this slice).
