# PLAN: Static-actor lighting mission-load bake (SIMPLIFIED, post-SSBO)

> Branch `claude/gpu-driven-rendering`, HEAD `b41baec` (LightsData is now
> an unbounded std430 SSBO). RE-GREP all file:line at edit-time.
> Re-scope recon: `docs/superpowers/plans/progress/2026-05-17-static-lighting-bake-rescope-post-ssbo.md`.
> SUPERSEDES the Option-A complexity in
> `2026-05-17-static-lighting-mission-load-bake.md` /
> `...-option-a-persistent-partition.md` (those layers are deleted —
> their sole justification, the 64-slot UBO window, shipped away as
> b41baec). C++-ONLY slice — no shader change (deploy-lockstep risk N/A,
> but still deploy exe).

## Goal (the real per-frame CPU-zone death)

Retire the per-frame light RECOMPUTE for the static class (bdactor
buildings + trees). Their per-actor light is a mission-load constant
(position-derived `getTerrainLight` + frozen sun/`nightFactor`; no
dynamic emitters — `lighting_is_mission_load_static_no_dynamic_emitters.md`).
The D2 ON/OFF capture measured ~2.0 ms/frame of surviving populate body
for the static population; this slice deletes that recompute. Per
`feedback_offload_must_be_substitutive_not_additive.md`: done = the
static-class per-frame `CacheGpuLightData`->`GatherGpuObjectLightDataOnly`
->`GatherLightsParameters`+`decomposeFirstActiveLightColor`+template-hash
chain ABSENT from a fresh capture; the per-frame slot WRITE legitimately
remains O(1) (terrain Shape-C precedent: recompute dies, consumer stays).

## What is DELETED vs the Option-A plan (do not reintroduce)

The unbounded SSBO (`shaders/include/lighting.hglsl` `std430 buffer {
ObjectLights light[]; }` @ binding 20) removes the window. Therefore
GONE: fixed static partition `[0..S)`; per-window static dedup;
self-bounding fallback-to-D2; partition-full canary; abandoned-slot
exhaustion bound; the `S`-sizing BLOCKING measurement gate; the
count-base `0->S` rebase + its CRITICAL co-reset adversarial item.
Adversarial review MUST NOT re-add any of these.

## Design (the simple Option B; ~= the stashed WIP, post-SSBO-correct)

### B1. Mission-scoped baked cache
`std::unordered_map<int32_t /*recipeIndex, monotonic never-reused*/,
TG_HWLightsData> s_bakedStaticLight` in `mclib/txmmgr.cpp` anon ns.
NOT touched by per-frame `resetLightData` (grep current line — it still
does `lightDataStructuresCount=0`; the bake cache is the *source of the
struct*, not a slot map, so the per-frame reset is irrelevant to it).
Cleared on mission unload via `GpuStaticPropRegistry::destroy()`
(`code/mission.cpp` call site — grep; reuse `mc2ClearAllBakedStaticLight`).
Per-entry erased in `GpuStaticPropRegistry::invalidate(recipeIndex)`
(reuse `mc2EraseBakedStaticLight`) so destruction/LOD multi-swap forces
a lazy re-bake of the same position-derived constant.

### B2. `BldgAppearance` gate helper (single logical chokepoint)
One free helper `mc2CacheOrBakeStaticGpuLight(TG_MultiShape*, bool
registered, int32_t recipeIndex)` replaces the 4 raw
`shape->CacheGpuLightData()` static calls (bldg `bdactor.cpp:2478/2521`,
tree `:4912/4933` — RE-GREP, lines drifted at b41baec). The trailing
`staticReg.lightDataIndex = shape->getCachedGpuLightIndex()` per-instance
capture stays UNCHANGED (both CacheGpuLightData and the baked emit set
`cachedGpuLightIndex_`). Logic:
- guard `!mc2LightBakeEnabled() || !registered || recipeIndex < 0`
  (int32_t sentinel -1) -> unchanged `shape->CacheGpuLightData()`.
- cache HIT -> `shape->EmitBakedGpuLightData(recipeIndex, baked)`:
  emit the baked constant into a per-frame slot and set
  `cachedGpuLightIndex_`/`cachedFrame_` WITHOUT GatherLights/decompose/
  template-hash. Per-frame slot WRITE remains (Shape-C O(1) consumer);
  the RECOMPUTE is what dies.
- cache MISS (frame 1 / post-invalidate) -> `shape->CacheGpuLightData()`
  (real gather), then snapshot `firstShapeNodeLeaf->lightData_` (TG_Shape
  member; TG_MultiShape is friend, `tgl.h` — RE-GREP) via
  `peekCachedLeafLightData()` into `s_bakedStaticLight[recipeIndex]`.
- Mechs never reach this helper (`mech3d.cpp` calls CacheGpuLightData
  directly) -> D2 path, untouched (their per-frame floor). Generic
  props (`genactor.cpp` SetLightList(NULL,0)) take the no-actor-light
  path, not template -> out of scope, unaffected.

### B3. Per-frame baked-slot emit (M1 — mechanism PINNED)
`EmitBakedGpuLightData(recipeIndex, baked)` -> `mc2SubmitBakedLightSlot(
recipeIndex, baked)`: a `std::unordered_map<int32_t,uint32_t>
s_bakedSlotByRecipe` (per-frame), cleared in `resetLightData` alongside
the D2 maps. Miss (first call this frame for this recipe) ->
`addLightDataStructure(&baked)` (fresh per-frame slot, the legitimate
O(1) consumer), store; hit -> return the cached slot. This is a genuine
FRESH per-frame resolution (map cleared every `resetLightData`) — NOT a
retained cross-frame index (Option-C-safe). With the unbounded SSBO
there is NO window risk; the slot just needs to be valid this frame and uploaded
(the b41baec eager-create + `kLightUploadFloor` + per-frame upload
already guarantee the SSBO is bound and sized). NO persistent-partition
indexing needed — keep it the simple per-frame slot the engine already
expects; only the COMPUTE feeding it is retired.

### B4. Kill-switch + lifecycle (same commit)
`MC2_LIGHTBAKE` env, default **ON** (=0 -> unchanged D2 path
bit-for-bit). Free fn `mc2LightBakeEnabled()`. `[LIGHTBAKE v1]`
lifecycle: `event=enabled mode=bake|passthrough`, `event=first_bake
recipe=`, `event=rebake_on_invalidate` (first). Demote-not-delete.

## CRITICAL adversarial-review items (the re-scope's flagged risks)

1. **Cull-gate adjacency (TOP, `cull_gates_are_load_bearing.md`).** The
   4 helper sites are already inside `inView || g_useGpuStaticProps`
   (b41baec did not change call-site gating). A baked struct may be
   STORED for a culled actor but only EMITTED into a per-frame slot
   when visible (today's behavior). The helper must not reach past the
   gate. Verbatim the prior B3 item — top check.
2. **Do NOT "simplify" B into the fatal Option-C.** `resetLightData`
   still does `lightDataStructuresCount=0` per frame; the unbounded
   SSBO removed the *capacity ceiling*, NOT the per-frame slot reset. A
   retained raw slot index across frames still dangles. B keeps the
   per-frame slot (just skips the recompute) — correct. C (retain the
   index, stop emitting) is still fatal. Review must confirm B not C.
3. **Snapshot source** = `firstShapeNodeLeaf->lightData_` (durable
   post-decompose TG_Shape member), NOT the per-frame scratch slot.
4. **b41baec interaction**: eager-create + `kLightUploadFloor` are
   buffer-sizing only; they do not conflict with a source-struct cache.
   Confirm no interaction.
5. **C++-only**: confirm zero `shaders/` edits -> deploy-lockstep N/A
   (but still deploy the rebuilt exe).
6. **nightFactor**: bake validity assumes `eye->nightFactor` mission-
   constant; name it the documented revisit trigger.

## Verification (soak waived; probes + canary)
- Adversarial-plan-review != STOP THE LINE.
- Parity probe (env-gated): for a baked actor, baked struct bit-equal
  to a fresh gather this frame (sample 1/N) —
  `feedback_soak_waiver_with_probes_and_reviews_validated`.
- tier1 5/5, `MC2_LIGHTBAKE` default, `GL_INVALID_*`=0, `+0` destroys.
- USER-driven: static-prop + mech + HUD lighting bit-visually identical
  bake-on vs off, heaviest mission + mc2_03; `[LIGHTBRIDGE v1]`
  populate ns -> ~0 for the static population (the armed CPU-zone-death
  proof); total-frame ON vs OFF (anti-mirage).
- Commit: exe (no shader change); append outcome to
  `lighting_is_mission_load_static_no_dynamic_emitters.md`
  (ceiling removed by b41baec; bake retired the static recompute) +
  `feedback_offload_must_be_substitutive_not_additive.md`. Refresh
  `docs/render-perf-snapshot.md` (closing step). Drop the obsolete
  pre-SSBO bake-WIP git-stash.

## Out of scope
Mechs (D2 floor). Generic props (no-actor-light path). Any
persistent-partition / window machinery (deleted with the UBO).
`glBufferSubData` sub-range upload (separate deferred follow-on).
