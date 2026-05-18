# Static-Actor Lighting Mission-Load Bake — Recon

- **Branch / HEAD:** `claude/gpu-driven-rendering` @ `2dca942` (verified `git rev-parse HEAD`)
- **Scope:** READ-ONLY recon. No code written.
- **Load-bearing premises (NOT re-derived):** no dynamic light emitters exist
  (`lighting_is_mission_load_static_no_dynamic_emitters.md`); offload must be
  substitutive not additive (`feedback_offload_must_be_substitutive_not_additive.md`);
  cost-split absolutes are observer-effect-dominated, the terrain Shape-C
  `buildTerrainFaceCache` mission-load-bake precedent is the model
  (`cost_split_instrumentation_is_observer_effect_dominated.md`); cull gates are
  load-bearing (`cull_gates_are_load_bearing.md`); verify producer path against
  telemetry before substitution (`verify_producer_path_against_telemetry_before_substitution.md`).

All file:line citations grep-verified at HEAD `2dca942` during this invocation.

---

## 0. Verified mechanics (the handed facts, confirmed + extended)

- **Call sites confirmed.** Static: buildings `mclib/bdactor.cpp:2478` (gpuEligible)
  and `:2521` (full-bake branch); trees `bdactor.cpp:4912` and `:4933`; generic
  props `mclib/genactor.cpp:1219`. Dynamic: mechs `mclib/mech3d.cpp:4501`. All
  reach `TG_MultiShape::CacheGpuLightData()` (`mclib/msl.cpp:1892`), whose sole
  populate is `firstShapeNodeLeaf->GatherGpuObjectLightDataOnly()` (`msl.cpp:1918`)
  → `addLightDataStructureWithPerActorColor` (`txmmgr.cpp:1196`).
- **Per-frame reset confirmed.** `MC_TextureManager::resetLightData()`
  (`txmmgr.cpp:1266`) sets `lightDataStructuresCount = 0` and clears
  `s_lightDataDedupMap`, `s_sceneLightTemplateMap`, `s_lightSlotByActorKey`, and
  resets `s_sceneLightTemplateFrame = 0xFFFFFFFFu` (`txmmgr.cpp:1272-1279`). The
  `lightData_` UBO array and ALL slot indices are rebuilt from count=0 each frame.
- **Template key is deliberately per-frame.** `sceneLightTemplateKey`
  (`txmmgr.cpp:986`) folds `g_mc2FrameCounter` into the hash at `:991`
  (`h = fnv1a_64_u32(h, g_mc2FrameCounter)`). Nothing in the template/slot caches
  survives a frame boundary today — by construction.
- **D2 cheap path confirmed.** On the (templateKey + actorARGB) cache hit
  `addLightDataStructureWithPerActorColor` returns `sit->second.slot` at
  `txmmgr.cpp:1250` skipping the 1792B FNV + 1792B memcmp in
  `addLightDataStructure` (`txmmgr.cpp:1146-1194`). Residual on the hit path:
  `*light_data = it->second.data` 1792B copy (`txmmgr.cpp:1225`), the
  `s_sceneLightTemplateMap.find` (`:1214`), the `s_lightSlotByActorKey.find`
  (`:1247`), an optional `decomposeFirstActiveLightColor` (`:1228`), the
  `LbScope` chrono pair (`:1199`, `:1061-1069`), plus the
  `firstShapeNodeLeaf` linear scan in `CacheGpuLightData` (`msl.cpp:1904-1915`).
- **GPU read is already GPU-owned.** Confirmed `cachedGpuLightIndex_` written at
  `msl.cpp:1918`, consumed by the batcher (`gos_static_prop_registry.cpp:101`,
  `:283`, `:386` reference `multi->cachedGpuLightIndex_`; the
  `markVisible(regIdx, lightDataIndex)` ferry is `gos_static_prop_registry.cpp:278-288`).

---

## 1. Architectural options + recommendation

The crux is real: because `lightData_` is rebuilt from `count=0` every frame
(`txmmgr.cpp:1272`) and `s_lightDataDedupMap` correctness *depends* on that
co-reset (`txmmgr.cpp:1273-1276` comment is explicit: "Both must reset together
— slot indices restart from 0 each frame"), "compute the slot once at mission
load" cannot be literal without touching the per-frame array invariant.

### Option A — Persistent static-light UBO partition

A region of `lightDataBuffer_` (or a second buffer) NOT cleared by
`resetLightData()`; static-actor slots allocated once at mission load;
`cachedGpuLightIndex_` points into it permanently for static actors; the
per-frame dynamic array keeps `[0..N)` for mechs only.

- **Blast radius: LARGE.** Splits the single `lightData_`/`lightDataBuffer_`
  contract into two address spaces. Every consumer that indexes by
  `cachedGpuLightIndex_` (batcher flush `gos_static_prop_registry.cpp:386`,
  the UBO upload, `LIGHT_DATA_ATTACHMENT_SLOT` shader read) must learn a
  partition discriminator (high-bit tag or base offset). The shader-side
  indexing changes — defer that question to `mc2-shader-expert` /
  `mc2-gameos-expert`; do NOT assert the shader can absorb a tagged index here.
- **Vulkan-prep alignment: GOOD in principle** (a persistent device-resident
  buffer written once is the std430 "static recipe" shape from
  `water_ssbo_pattern.md`), but only if the partition is a clean device buffer,
  not a host-poked sub-range of the per-frame ring.
- **resetLightData invariant interaction: HOSTILE.** `s_lightDataDedupMap`
  dedups within the per-frame array; a persistent partition needs its OWN
  mission-scoped dedup map that is NOT cleared per frame, plus a rule that the
  per-frame dedup never collides into persistent slot space. This is the same
  "two maps that must reset on different cadences" hazard the
  `txmmgr.cpp:1273-1276` comment warns about, doubled.
- **Substitutive?** YES if done — the per-frame `CacheGpuLightData` becomes a
  literal no-op for static actors (CPU zone death per
  `feedback_offload_must_be_substitutive_not_additive.md`).
- **Risk:** highest blast radius of the three; touches the UBO contract that
  `cull_gates_are_load_bearing.md` adjacency (batcher flush, markVisible) sits on.

### Option B — Keep the per-frame array, skip only the COMPUTE (recommended)

Static actors re-emit a cached constant `TG_HWLightsData` into the per-frame
slot each frame WITHOUT re-running `GatherLightsParameters` /
`decomposeFirstActiveLightColor`. The per-frame slot is still allocated (via
the existing `addLightDataStructure` append/dedup) but the *computation* that
produces the struct is a mission-load constant looked up by a stable
(actor-identity) key instead of recomputed.

- **Blast radius: SMALL.** Localized to `addLightDataStructureWithPerActorColor`
  (`txmmgr.cpp:1196`) plus a static-actor-keyed cache that survives
  `resetLightData()`. The UBO contract, `cachedGpuLightIndex_` semantics, the
  batcher, and the shader are UNCHANGED — a per-frame slot is still produced;
  only its *derivation* is retired. This is a direct extension of the D2/C6
  pattern (`txmmgr.cpp:1230-1261`), not a new architecture.
- **Vulkan-prep alignment: GOOD.** No new buffer; no device-binding change;
  preserves the existing enqueue/flush + std430 lockstep.
- **resetLightData invariant interaction: BENIGN.** The persistent cache is a
  *source of the struct*, not a slot map. `s_lightDataDedupMap` /
  `s_lightSlotByActorKey` still reset per frame and still own slot allocation;
  the bake only short-circuits the `GatherLightsParameters` +
  `decompose` work behind the existing template-miss branch
  (`txmmgr.cpp:1215-1220`). No second-cadence reset hazard.
- **Substitutive?** PARTIALLY. It retires the *compute* (GatherLights +
  decompose + template hash) but the per-frame slot WRITE
  (`addLightDataStructure` append + the `*light_data` copy) still runs. Honest
  framing: this is the same shape as terrain Shape-C
  (`buildTerrainFaceCache` bakes the recipe; the per-frame consumer
  `getTerrainFaceCacheEntry` is still O(1)-called) — the *recompute* dies, the
  per-frame O(1) consumer remains. That precedent is explicitly the model
  named in the load-bearing memory.
- **Risk:** the keying. The bake key must be actor-identity-stable AND capture
  everything that feeds the struct (see §3). After D2 the *measurable*
  remaining cost is small (see §4) — Option B is correctness/architecture-clean
  and a modest perf delta, NOT a multi-ms win.

### Option C — Skip the per-frame CALL entirely for static actors

Gate the `CacheGpuLightData()` call sites so static actors do not call it at
all after the first populate; `cachedGpuLightIndex_` retains its first-frame
value permanently.

- **FATAL flaw, grep-verified:** `cachedGpuLightIndex_` points into the
  per-frame `lightData_` array which IS reset to count=0 every frame
  (`txmmgr.cpp:1272`). A retained index from frame N points at whatever slot N+1
  happens to allocate there — guaranteed wrong-light corruption. Option C is
  only viable if combined with Option A's persistent partition (i.e. C is not
  independent; it is "A + stop calling"). Reject C standalone.

### Recommendation: **Option B**, with the explicit caveat that it is an
architecture/correctness slice with a marginal measured perf delta post-D2
(§4), framed exactly as the terrain Shape-C precedent: the *recompute* dies,
the O(1) per-frame consumer (slot write) remains. Option A is the only path to
literal per-frame-call death but its blast radius (UBO contract split + dual
dedup-reset cadence) is disproportionate to the post-D2 residual; recommend
A be reconsidered ONLY if a future dynamic-light feature forces the
static/dynamic buffer split anyway, or if §4 telemetry shows the per-frame
slot WRITE (not the compute) is itself a measured multi-ms zone.

---

## 2. Static/dynamic split + gate site + mission-load hook

### The split (confirmed)

- **Static class = bdactor + genactor.** Buildings/TerrainObjects
  (`bdactor.cpp:2478/2521`), trees (`bdactor.cpp:4912/4933`), generic props
  (`genactor.cpp:1219`). All are position-fixed; per-actor color is
  `land->getTerrainLight(position)` (`bdactor.cpp:2322`, `:4823`) with `position`
  constant after mission load.
- **Dynamic class = mech3d.** Mechs (`mech3d.cpp:4501`) move, so
  `getTerrainLight(position)` genuinely changes per frame. Mechs KEEP the D2
  cheap per-frame path; that path stays load-bearing for them.

### Where the gate should live

**Recommendation: a per-multi "static & baked" flag consulted inside
`TG_MultiShape::CacheGpuLightData()` (`msl.cpp:1892`), populated by the
mission-load hook.** Rationale:

- NOT at the bdactor/genactor call sites: there are FOUR static call sites
  (`bdactor.cpp:2478, 2521, 4912, 4933`, `genactor.cpp:1219`) with subtly
  different surrounding logic (gpuEligible vs full-bake branch, per-instance
  `staticReg.lightDataIndex` capture at `bdactor.cpp:2482/2523/4914/4935`).
  Gating at five call sites is the additive-pattern trap
  (`feedback_offload_must_be_substitutive_not_additive.md`): high blast radius,
  easy to miss one, and each site also feeds the markVisible ferry.
- Inside `CacheGpuLightData()` keyed off a flag the multi already carries is
  ONE chokepoint, mirrors how D2's repoint lives inside
  `addLightDataStructureWithPerActorColor` (`txmmgr.cpp:1240-1261`), and
  preserves the `staticReg.lightDataIndex = ...getCachedGpuLightIndex()`
  contract because the function still writes `cachedGpuLightIndex_`/`cachedFrame_`
  (`msl.cpp:1918-1919`) — just from the baked value instead of recompute.

### The analogous mission-load hook (the lighting equivalent of Shape-C)

**`registerStaticProp(Appearance*)` / `registerRecipe()` in
`GameOS/gameos/gos_static_prop_registry.cpp` is the existing mission-time
registration pass and the correct bake hook.** Grep-verified:

- `registerStaticProp(Appearance* app)` at `gos_static_prop_registry.cpp:160`
  is the late-spawn/mission registration entry; `registerRecipe(TG_MultiShape*,
  batch)` at `:230` is the per-multi registration that ALREADY pre-populates
  mission-time state: `multi->setCachedFrame(g_mc2FrameCounter)` at
  `gos_static_prop_registry.cpp:274` with the comment "Track B: structural
  first-frame fix. Pre-populate cachedFrame_ so the first flush() after
  registration passes the staleness gate ... without requiring a prior
  CacheGpuLightData() call." This is *precisely* the lighting analog of
  `Terrain::primeMissionTerrainCache` → `buildTerrainFaceCache`: a registration
  pass that already runs once per static multi at mission/spawn time and already
  pokes the exact field (`cachedFrame_`) the bake needs to extend.
- The bake would add: at `registerRecipe` (`:230-276`), after the existing
  `setCachedFrame`, compute the static actor's `TG_HWLightsData` ONCE and store
  it keyed by actor identity into a mission-scoped (NOT per-frame-reset) cache
  that `addLightDataStructureWithPerActorColor` consults before the
  template-miss branch (`txmmgr.cpp:1215`).
- **Caveat (defer to `mc2-gameos-expert` / terrain-indirect expert):** whether
  `registerRecipe` runs for ALL static actors or only batched-eligible ones,
  and whether `s_listOfLights` / `TG_Shape::s_numLights` are populated at
  registration time, is a registry-internals question this advisor does not
  own. The plan's Stage 0.5 must confirm the bake can compute the same
  `GatherLightsParameters` inputs at registration time as at render time.

---

## 3. The exact invalidation set (grep-verified per-item verdicts)

What feeds a static actor's `TG_HWLightsData`:
`GatherLightsParameters(light_data)` (`txmmgr.cpp:1380`, called `:1209/:1218`)
over `TG_Shape::s_listOfLights`, plus the per-actor first-light color override
`decomposeFirstActiveLightColor` (`txmmgr.cpp:954-970`) which copies
`listOfLights[i]->GetaRGB()` into `lightColor[0]`. The per-actor aRGB is set
upstream from `lightRGB = (lightr<<16)+(lightg<<8)+lightb` where
`lightr/g/b = eye->getLightRed/Green/Blue(lightIntensity)` and
`lightIntensity = land->getTerrainLight(position)` (`bdactor.cpp:2319-2328`,
mirror `:4818-4828`).

| Event | Verdict | Evidence |
|---|---|---|
| (a) Destruction tree→stump / building→rubble | **PROBABLY NO bake invalidation for the LIGHT slot; mesh changes, light does not** | Per-actor color is `getTerrainLight(position)` (`bdactor.cpp:2322`) — `position` is unchanged by destruction. `eye->getLightRed/Green/Blue` is sun/global (mission-constant per load-bearing memory: no day/night sweep). The struct's per-actor color is position-derived, so tree↔stump at the same position yields the SAME `lightColor[0]`. **BUT**: destruction may swap to a different `TG_MultiShape` (different mesh → different `firstShapeNodeLeaf` → different `cachedGpuLightIndex_` *owner*), so the bake must be keyed/invalidated on the *multi identity*, not assume one multi per actor for the actor's whole life. Verdict: light VALUE does not change; the multi the index lives on MAY. Plan must key the bake per-multi and re-bake on mesh swap (cheap — same constant recomputed for the new multi). |
| (b) LOD swaps | **Likely re-bake required (multi identity changes), light value unchanged** | Same reasoning as (a): a LOD swap changes the `TG_MultiShape`/leaf, so `cachedGpuLightIndex_` must be re-established for the new multi, but the computed `TG_HWLightsData` (position-derived) is identical. Treat as "re-bake the new multi with the same constant," not "invalidate the value." NOT independently grep-confirmed which LOD mechanism swaps the multi for bdactor/genactor — flag as a Stage 0.5 open question for `mc2-render-expert`. |
| (c) Cull / off-screen→on-screen transition | **NO value invalidation, but the bake MUST survive the cull gate and NOT bypass it** | `cull_gates_are_load_bearing.md`: `inView`/`canBeSeen` gate `update()` AND allocation AND lifecycle. The bake must NOT compute or refresh the slot for culled actors by reaching past the cull (that cascades into pool/lifecycle bugs). The static call sites are already INSIDE the `inView \|\| g_useGpuStaticProps` cull gate (`bdactor.cpp:4897-4898` comment: "Branch lives INSIDE the existing inView\|\|g_useGpuStaticProps cull gate to preserve slice 1's R1 invariant"). The bake's per-frame consult must stay inside that same gate — i.e. Option B's chokepoint in `CacheGpuLightData()` is only reached when the actor is already past cull, which is correct. A baked value computed at registration is fine to *store* for a culled actor; it must only be *emitted into the per-frame slot* when the actor is visible (exactly today's gating). Verdict: value does not change; gate adjacency is load-bearing — do not bypass. |
| (d) Gameplay that moves a "static" actor | **Would invalidate, but grep-confirmed not a real case for the static class** | `getTerrainLight(position)` (`bdactor.cpp:2322`) is position-dependent. Buildings/trees/props do not move (that is the definition of the static class per `lighting_is_mission_load_static_no_dynamic_emitters.md`). Any actor that moves is by definition in the dynamic class and keeps the D2 per-frame path. The `spinMe` case (`bdactor.cpp:2306-2311`) only clamps z above water at setup; it does not move the actor per frame. Verdict: provably does NOT require invalidation for the genuine static class; if a "static" actor is ever made mobile it must be reclassified to the mech/dynamic path, not bake-invalidated. |
| (e) `nightFactor` / per-type point lights | **Confirm OFF in current build; assumption must be documented** | `bdactor.cpp:2226` (`appearType->terrainLightRGB != 0xffffffff && eye->nightFactor > 0.0f`) and `:2253` (`SetIntensity(... * eye->getNightFactor())`) show a per-type emissive point-light path that IS night/time-dependent. Per the load-bearing memory, game time is frozen and no day/night sweep moves the sun, so `nightFactor` is mission-constant today — but this is the single most important assumption to STATE EXPLICITLY in the plan (the memory itself mandates this). If a future time-of-day feature lands, `nightFactor` changing per frame breaks the bake; the plan must name `eye->getNightFactor()` / `eye->nightFactor` as the designated invalidation trigger to revisit. Verdict today: constant; flag as the documented assumption. |

**Summary:** the per-actor LIGHT VALUE for a genuine static actor is a
mission-load constant (position-derived + sun/global, both frozen). The only
real invalidation is *multi-identity change* (destruction/LOD swap), which is a
cheap re-bake of the same constant for the new multi, NOT a value
recomputation. The load-bearing risk is the cull-gate adjacency (c), not the
value.

---

## 4. Sizing / telemetry — honest assessment

Per `verify_producer_path_against_telemetry_before_substitution.md` and
`feedback_offload_must_be_substitutive_not_additive.md`:

- **The existing armed lever.** The `[LIGHTBRIDGE v1]` accumulator (`LbScope`,
  `txmmgr.cpp:1061-1069`, drained at `lbDrainPerFrame` `:1071-1089`, gated on
  `MC2_OBJECT_RECON_TRACY`) times the WHOLE
  `addLightDataStructureWithPerActorColor` body via the RAII `LbScope _lb_;`
  at `txmmgr.cpp:1199`. It reports `{ns, calls, tmpl_hit, tmpl_miss,
  no_actor_light}`. `tmpl_hit` (`s_lbFrameHit`, incremented `:1222`) is exactly
  the population whose trailing redundancy the bake targets.
- **Post-D2 residual is small and KNOWN.** After D2/C6, the bake-eligible
  static population returns via the cheap hit at `txmmgr.cpp:1250` (no FNV, no
  memcmp). The residual per hit-path call is: `s_sceneLightTemplateMap.find`
  (`:1214`) + `s_lightSlotByActorKey.find` (`:1247`) + the 1792B
  `*light_data = it->second.data` copy (`:1225`) + optional
  `decomposeFirstActiveLightColor` (`:1228`) + the `LbScope` chrono pair +
  the `firstShapeNodeLeaf` linear scan in `CacheGpuLightData`
  (`msl.cpp:1904-1915`). At ~1600 calls/frame this is sub-millisecond — two
  hash-map lookups and a 1.8KB memcpy are ~hundreds of ns each, not the
  multi-ms regime D2 already drained.
- **The bake's incremental armed lever is the `tmpl_hit` residual + the
  `firstShapeNodeLeaf` scan**, provable by: (1) `tmpl_hit` count from
  `[LIGHTBRIDGE v1]` (already armed, no new instrumentation), and (2) a
  worst-case non-COST_SPLIT total-frame Tracy (per
  `cost_split_instrumentation_is_observer_effect_dominated.md` — cost-split
  absolutes are observer-effect-dominated and must NOT size this slice; the
  `LbScope` chrono pair is itself ~30-50ns/call observer effect the
  comment at `txmmgr.cpp:1033-1037` acknowledges).
- **Honest verdict (per `feedback_offload_must_be_substitutive_not_additive.md`):**
  **D2 already captured ~all of the recoverable per-frame PERF cost.** The
  static-lighting mission-load bake is primarily a CORRECTNESS / ARCHITECTURE
  slice — it makes the static-actor light data structurally a mission-load
  constant (matching the engine reality that no dynamic emitters exist), which
  is *cleaner* and removes a class of "why is this recomputed per frame"
  redundancy — but it is **perf-marginal post-D2**, not a multi-ms win. The
  multi-ms win was the FNV+memcmp, and D2 took it. State this plainly in the
  plan: do NOT scope or sell this as a perf slice. Its justification is
  architectural correctness + setting up the eventual static/dynamic buffer
  split, NOT frame time. If the plan needs a perf number to justify itself, it
  does not have one — and per the substitutive-not-additive memory, that is the
  honest answer, not a reason to inflate the residual.

---

## 5. Open questions for the plan / adversarial review

1. **Option A vs B fork.** Recommendation is B (small blast radius,
   perf-marginal but architecturally clean). A is the only literal
   per-frame-call death but disproportionate blast radius post-D2. The plan
   must explicitly RULE this fork, not leave it implicit. Decide whether the
   eventual static/dynamic buffer split (a future dynamic-light feature) makes
   A worth doing now.
2. **Registry hook reach.** Does `registerRecipe` (`gos_static_prop_registry.cpp:230`)
   run for EVERY static actor, or only GPU-batch-eligible ones? Are
   `TG_Shape::s_listOfLights` / `s_numLights` populated at registration time so
   `GatherLightsParameters` can be computed there? **Defer to
   `mc2-gameos-expert` / terrain-indirect expert** — this advisor does not own
   registry internals.
3. **LOD/destruction multi-swap.** Confirm which mechanism swaps the
   `TG_MultiShape` for bdactor/genactor on destruction and LOD, so the bake key
   is multi-identity-stable and re-bakes (same constant) on swap. **Defer to
   `mc2-render-expert`.**
4. **`nightFactor` assumption.** The plan MUST state explicitly: bake validity
   assumes `eye->nightFactor` / `eye->getNightFactor()` (`bdactor.cpp:2226,2253`)
   is mission-constant (no time-of-day). Name it as the designated
   revisit-trigger for any future dynamic-light/time-of-day feature.
5. **Cull-gate adjacency.** Verify the bake's per-frame consult stays inside
   the existing `inView \|\| g_useGpuStaticProps` gate
   (`bdactor.cpp:4897-4898`); a baked value may be STORED for a culled actor
   but only EMITTED into the per-frame slot when visible (today's behavior).
   This is a CRITICAL adversarial-review item (`cull_gates_are_load_bearing.md`).
6. **Honest perf framing.** Adversarial review must verify the plan does NOT
   claim a perf win D2 already took. The slice's stated deliverable should be
   "static-actor light data is a mission-load constant; per-frame recompute
   (GatherLights + decompose + template hash) retired for the static class;
   per-frame slot WRITE remains O(1) (Shape-C precedent)" — NOT a frame-time
   number.

---

*Recon only. No code written. Citations grep-verified at `2dca942`.*
