# Static-Lighting Mission-Load Bake — RE-SCOPE post-SSBO (b41baec)

- **Branch / HEAD:** `claude/gpu-driven-rendering` @ `b41baec` (verified
  `git rev-parse HEAD` = `b41baec636d2b3e122c9f705089115db4e3581cd`).
- **Scope:** READ-ONLY recon. No code written. This is a RE-SCOPE, not a
  from-scratch recon. The prior recons (mission-load-bake-recon,
  option-a-persistent-partition-recon) and plans are INPUT — verified-then-
  extended, not re-derived.
- **The trigger for the re-scope:** `LightsData` is now an UNBOUNDED
  `std430 buffer { ObjectLights light[]; }` SSBO at binding 20 (commit
  `b41baec`). There is NO 64-slot window anymore. Every prior recon/plan was
  written UNDER the 64-slot UBO ceiling; that ceiling is the load-bearing
  premise of the entire Option-A complexity layer, and it is gone.
- **Load-bearing premises (NOT re-derived, carried verbatim):**
  - No dynamic light emitters; static-actor light is a mission-load constant
    (`lighting_is_mission_load_static_no_dynamic_emitters.md`).
  - Offload must be SUBSTITUTIVE: done = the static-class per-frame recompute
    chain ABSENT from a fresh capture + consumers repointed
    (`feedback_offload_must_be_substitutive_not_additive.md`).
  - Cull gates are load-bearing; the bake's per-frame consult must stay inside
    the existing `inView || g_useGpuStaticProps` gate
    (`cull_gates_are_load_bearing.md`).
  - Verify the producer/armed cost against telemetry before sizing
    (`verify_producer_path_against_telemetry_before_substitution.md`).
  - Shader/exe deploy lockstep (`shader_exe_deploy_lockstep.md`) — see Q4.

All file:line citations grep-verified at `b41baec` during this invocation.
Line numbers DRIFTED from the prior recons (written @ `2dca942`); the symbols
are stable, the lines below are the b41baec truth.

---

## 0. What b41baec actually changed (grep-verified)

- **Shader.** `shaders/include/lighting.hglsl:53-56`:
  `layout (binding = LIGHT_DATA_SSBO_BINDING, std430) buffer LightsData {
  ObjectLights light[]; }`. `LIGHT_DATA_SSBO_BINDING 20` defined `:15`. The
  array is UNBOUNDED — no `[64]`. Consumed as a raw absolute subscript
  `light[lights_index]` (`lighting.hglsl:207`); no region base / tag bit
  anywhere (the prior recon's "absolute, region-agnostic index" finding
  still holds, now with no upper bound).
- **Buffer ownership moved out of txmmgr.** `lightDataBuffer_` member DELETED
  (`txmmgr.h:398` comment "lightDataBuffer_ removed: LightsData is now an
  SSBO owned by gameos_graphics.cpp (gos_LightDataSsbo_*)"). The SSBO is a
  raw-GL singleton in `gameos_graphics.cpp` (`gos_LightDataSsbo_Upload` /
  `gos_LightDataSsbo_Destroy` / `gos_BindLightDataStorageBlock`).
- **Eager create-at-construct.** `txmmgr.cpp:329`
  `gos_LightDataSsbo_Upload(lightData_, capacity*sizeof(TG_HWLightsData))`
  in `MC_TextureManager::start()` (capacity = 128, `:318`). Comment
  `:321-328`: eager so binding 20 always has a valid buffer for the batcher
  (which runs in a different phase than the txmmgr per-frame upload). This is
  the lifetime fix for the black-props regression.
- **Upload floor RETAINED.** `txmmgr.cpp:1573-1577`:
  `constexpr uint32_t kLightUploadFloor = 64u;` then
  `gos_LightDataSsbo_Upload(lightData_, max(count, 64)*sizeof)`. Comment
  `:1565-1572` is explicit: this floor is NOT the removable UBO-window
  artifact — it backs the engine's deliberate tolerance of transient
  over-count `lightDataIndex` for cull-stale actors (the static-prop registry
  relies on it). Falsified-as-removable 2026-05-17; kept on purpose.
- **Per-frame upload still whole-buffer.** `txmmgr.cpp:1576` re-uploads
  `lightData_[0 .. max(count,64))` from offset 0 every frame via
  `gos_LightDataSsbo_Upload`. There is still NO sub-range upload primitive.
- **`resetLightData` UNCHANGED in its load-bearing behavior.**
  `txmmgr.cpp:1273-1287`: still `lightDataStructuresCount = 0;` (`:1279`),
  still clears `s_lightDataDedupMap` (`:1283`), `s_sceneLightTemplateMap`
  (`:1284`), `s_lightSlotByActorKey` (`:1285`), `s_sceneLightTemplateFrame =
  0xFFFFFFFFu` (`:1286`). The co-reset comment (`:1280-1282`) is verbatim
  what it was: "slot indices restart from 0 each frame." **The per-frame
  tear-down-and-rebuild of `lightData_` from count=0 is exactly as before.**
- **D2 cheap path UNCHANGED.** `addLightDataStructureWithPerActorColor`
  `txmmgr.cpp:1203`; template miss runs `GatherLightsParameters` +
  `decomposeFirstActiveLightColor` (`:1225-1226`); D2 small-key cache hit
  returns `sit->second.slot` skipping FNV/memcmp (`:1254-1258`). `LbScope`
  `:1206`. The `[LIGHTBRIDGE v1]` accumulator (`:1206`, drained
  `lbDrainPerFrame` at `resetLightData` `:1277`) is intact and armed on
  `MC2_LIGHTBRIDGE` / object-recon env.
- **`CacheGpuLightData` UNCHANGED.** `msl.cpp:1892`; early-out
  `if (!g_useGpuObjects && !g_useGpuMechs) return;` (`:1900-1901`);
  `firstShapeNodeLeaf` scan (`:1903-1915`); populate
  `cachedGpuLightIndex_ = firstShapeNodeLeaf->GatherGpuObjectLightDataOnly();`
  (`:1918`); `cachedFrame_ = g_mc2FrameCounter;` (`:1919`).
- **Batcher / ferry UNCHANGED.** `gos_static_prop_batcher.cpp:2534-2539`:
  `if (multi->cachedGpuLightIndex_ != 0xFFFFFFFFu) lightDataIndex =
  multi->cachedGpuLightIndex_; else ... GatherGpuObjectLightDataOnly()` (the
  C7 per-prop render-time fallback `:2539`). Ferry
  `gos_static_prop_registry.cpp:391-392`
  `(s_perInstanceLight && rng.lightDataIndex != 0xFFFFFFFFu) ?
  rng.lightDataIndex : multi->cachedGpuLightIndex_`. `markVisible`
  `:278-287`; `invalidate` resets `rng.lightDataIndex = 0xFFFFFFFFu` `:301`.

**Net:** b41baec removed ONLY the 64-element shader window and moved buffer
ownership to a raw-GL SSBO singleton. It did NOT change `resetLightData`'s
per-frame count=0 tear-down, the D2 path, `CacheGpuLightData`, or the
batcher/ferry. The static recompute is still fully live per frame.

---

## Q1. What survives from the prior recons (verified at b41baec)

Every load-bearing finding of the two prior recons survives the SSBO
conversion. Verified item-by-item:

| Prior finding | Verdict @ b41baec | Evidence |
|---|---|---|
| Lazy-first-successful-gather trigger inside `TG_MultiShape::CacheGpuLightData()` is the single chokepoint all static call sites reach, past `SetLightList` | **SURVIVES.** `CacheGpuLightData` `msl.cpp:1892`; all four static call sites still route to it; SetLightList still precedes them | bldg `bdactor.cpp:2444` (SetLightList) → `:2478`/`:2521` (CacheGpuLightData); tree `:4894` → `:4912`/`:4933`; populate `msl.cpp:1918` |
| Key = monotonic-never-reused registry `recipeIndex` | **SURVIVES.** `staticReg.recipeIndex` set from `registerRecipe` (`bdactor.cpp:1799`/`:2958`/`:5186`), guard `recipeIndex >= 0` (`:3004`/`:5109`); int32 sentinel `-1` | `bdactor.cpp:3002-3005` (`invalidate(staticReg.recipeIndex)`), `:5107-5110` |
| Snapshot source = `firstShapeNodeLeaf->lightData_` (TG_Shape member, friend access) AFTER `CacheGpuLightData` ran — NOT `lightData_[idx]` | **SURVIVES & STILL REQUIRED.** `lightData_[idx]` is still the per-frame scratch array reset to count=0 (`txmmgr.cpp:1279`) and overwritten by siblings same-frame; the friend-snapshot of the leaf member is still the only stable source | `msl.cpp:1918` populate; `txmmgr.cpp:1279` count=0 reset unchanged |
| In-place re-bake on `invalidateStaticRegistration` → `GpuStaticPropRegistry::invalidate` | **SURVIVES.** Hook unchanged: bldg `bdactor.cpp:3002-3005`, tree `:5107-5110`; swap paths still route through it (`:1720/:1726/:1767/:1786` bldg, `:4559/:4564/:4585/:4598` tree) | grep-confirmed at b41baec |
| Cull-gate adjacency (per-frame consult stays inside `inView \|\| g_useGpuStaticProps`) | **SURVIVES.** Call sites unchanged; still inside the gate; `markVisible` ferry (`registry:278-287`) unchanged | `bdactor.cpp:2478/2521/4912/4933` |
| Static class = bdactor bldg/trees; NOT genactor (`SetLightList(NULL,0)`); NOT mechs (D2 floor) | **SURVIVES.** genactor still zaps `s_listOfLights` → `no_actor_light` passthrough branch (`txmmgr.cpp:1214-1218`); mechs (`mech3d.cpp:4501`) unchanged D2 | `txmmgr.cpp:1214` `actorLightSource == 0xFFFFFFFFu` branch |
| nightFactor mission-constant assumption | **SURVIVES** (engine premise, not code-state-dependent; `lighting_is_mission_load_static_no_dynamic_emitters.md`) | unchanged |

Also surviving from the option-a recon, now MOOT (see Q2): the
"absolute, region-agnostic shader index" finding (`lighting.hglsl:207`)
survives and is now even more permissive — an unbounded array means any
absolute index is in-bounds as long as the C++ buffer covers it.

**Nothing in Q1 needs re-derivation. The bake's mechanical skeleton
(chokepoint, key, snapshot source, invalidate hook, cull adjacency, class
split) is exactly the Option-B plan's, unchanged by the SSBO.**

---

## Q2. The now-simpler persistent mechanism

### What the unbounded SSBO deletes

The entire reason Option B ("keep the per-frame slot, skip only the compute")
was *recommended over* Option A was the 64-slot ceiling:

- Option A needed a fixed static partition `[0..S)` because static slots had
  to coexist with dynamic slots inside a 64-entry window.
- It needed per-window dedup of the static structs (`s_staticLightSlots`
  hash map) so distinct-static ≤ S.
- It needed a self-bounding fallback-to-D2 when `s_staticLightCount == S`
  (the static partition could overflow the 64 window).
- It needed an `S`-sizing BLOCKING measurement gate (measure distinct-static
  via `[LIGHT_DEDUP v1]` on worst-case zoomed-out before choosing `S`).
- It needed the count-base `0 -> S` rebase of `lightDataStructuresCount`
  across `:319/:1272/:1286/:1559` + the load-bearing co-reset invariant
  rebased verbatim.
- It carried the "if distinct-total > 64, lockstep `light[N]` enlargement
  and possibly UBO→SSBO" hard dependency as its HIGHEST risk.

**Every one of those exists ONLY because the buffer was capped at 64.**
With `light[]` unbounded (`lighting.hglsl:55`) and the C++ array already
grow-by-128 (`txmmgr.cpp:1175-1182`), there is no window pressure:

- **DELETED: fixed static partition `[0..S)`.** No partition needed — a
  persistent static slot is just an index `j` that is never reused by the
  per-frame allocator; with an unbounded buffer it can be ANY stable index.
- **DELETED: `S`-sizing BLOCKING gate.** There is no `S`. The
  `verify_producer_path` BLOCKING measurement (recon §6.1 / Open Q1) was
  *entirely* a function of "does distinct-static + peak-dynamic fit in 64."
  No cap → no gate. (The `[LIGHT_DEDUP v1]` counter `txmmgr.cpp:1193-1199`
  remains as a passive growth telemetry, not a pre-gate.)
- **DELETED: per-window dedup of static structs / self-bound fallback /
  static-partition-full canary / abandoned-slot exhaustion bound.** These
  all defended a finite window. Unbounded → none apply. (Dedup can still be
  *kept* as a memory optimization but is no longer correctness-load-bearing;
  per `feedback_ram_cost_not_a_concern_below_500mb.md` it is not worth the
  complexity for the first cut — see chosen design.)
- **DELETED: count-base `0 -> S` rebase.** `lightDataStructuresCount` does
  not need to start at `S`; there is no static prefix to protect. The
  load-bearing `:1280-1282` co-reset invariant stays VERBATIM, untouched
  (no rebase). This was Option A's MEDIUM-risk CRITICAL adversarial item —
  it ceases to exist.
- **DELETED: the "if >64, lockstep enlargement + UBO→SSBO" hard
  dependency** — that dependency was *already executed and shipped* as
  b41baec itself. The Option-A HIGHEST risk is retired by the very commit
  this re-scope is anchored on.

### The three candidate designs, re-evaluated post-SSBO

**(D-A) Persistent partition (old Option A):** keep a reserved static
region the per-frame upload doesn't overwrite, `cachedGpuLightIndex_` points
there forever. Post-SSBO this still *works* but its entire justification
(squeeze static into a finite window without corrupting it) is gone. It
still carries: a second mission-scoped slot allocator, the dual-cadence
`resetLightData` reasoning ("static region survives, dynamic resets"), and a
genuine "two regions" mental model. **Disproportionate complexity for zero
remaining benefit now that there is no window to protect.** REJECT for first
cut (same verdict the mission-load-bake plan reached, now with a stronger
reason: the SSBO removed the only thing that made A interesting).

**(D-B) Keep per-frame slot, skip only the COMPUTE (old Option B,
recommended):** static actors re-emit the cached constant `TG_HWLightsData`
into a per-frame slot each frame WITHOUT re-running `GatherLightsParameters`
/ `decompose` / template hash. The per-frame slot WRITE
(`addLightDataStructure` append + dedup, then `*light_data` copy) still runs;
its *derivation* is a mission-load constant looked up by `recipeIndex`.
Blast radius small, localized to a `BldgAppearance` gate helper + a
mission-scoped `recipeIndex -> TG_HWLightsData` cache that survives
`resetLightData`. `resetLightData`, the dedup map, the count, the batcher,
the ferry, the shader: ALL UNCHANGED. **This is the substitutive Shape-C
shape: the recompute dies, the O(1) per-frame consumer (slot write) stays.**

**(D-C) Skip the per-frame CALL entirely:** gate the call sites so static
actors never call `CacheGpuLightData` after the first populate;
`cachedGpuLightIndex_` retains its first-frame value forever. Under the OLD
UBO this was FATAL because `cachedGpuLightIndex_` pointed into the per-frame
`lightData_` array reset to count=0 every frame (`txmmgr.cpp:1279`) — a
retained index aliases whatever slot the next frame allocates there. **This
hazard is UNCHANGED at b41baec** (`resetLightData` still count=0,
`lightData_` still per-frame scratch — Q0). D-C standalone is STILL fatal
post-SSBO. It is only viable combined with D-A's persistent region (i.e.
"D-A + stop calling"). REJECT standalone.

### Chosen design: D-B (the mission-load-bake plan's Option B), and the
### SSBO makes it the unambiguous pick — not merely the lower-blast-radius
### compromise it was under the UBO

Under the 64-slot UBO, the mission-load-bake plan picked B over A as a
blast-radius tradeoff ("A is the only literal per-frame-call death but its
UBO-contract split is disproportionate"). **Post-SSBO that tradeoff
collapses entirely in B's favor:** A's complexity existed *only* to survive
the window; with the window gone, A is pure cost with no benefit, and B is
not a compromise — it is the correct minimal design. The Option-A plan
(`2026-05-17-static-lighting-option-a-persistent-partition.md`) is
**SUPERSEDED in full**; its partition/dedup/count-base/self-bound machinery
is deleted, not deferred.

**Justification against `feedback_offload_must_be_substitutive_not_additive`
(must DELETE the per-frame static recompute, not add beside it):** D-B
DELETES the static-class per-frame `GatherLightsParameters` +
`decomposeFirstActiveLightColor` + `sceneLightTemplateKey` hash + the
`s_sceneLightTemplateMap`/`s_lightSlotByActorKey` map traffic — that whole
recompute body (`txmmgr.cpp:1208-1235`) is replaced, for the bdg/tree class,
by a single `unordered_map<int32_t,TG_HWLightsData>::find` on the stable
`recipeIndex`. It is not a cheaper recompute and not an added GPU twin: the
CPU recompute is *gone* for the static class. The slot WRITE that remains
(`addLightDataStructure` for the per-frame index) is the legitimately-
surviving O(1) consumer, exactly the terrain Shape-C precedent
(`buildTerrainFaceCache` bakes the recipe; `getTerrainFaceCacheEntry` per-
frame O(1) consumer remains). This satisfies the substitutive bar: a named
CPU zone (the static recompute) disappears; the consumer (slot write +
batcher index) is repointed to read the baked constant.

**One open simplification the SSBO enables but D-B does NOT take (and why):**
with an unbounded SSBO one *could* now also retire the per-frame slot WRITE
by giving the baked struct a genuinely persistent SSBO index that
`resetLightData` does not clear (true D-C-on-an-unbounded-buffer — no
partition needed, just "don't reset count for indices the static cache
owns"). That would delete the surviving O(1) write too. It is NOT chosen for
the first cut because: (a) `resetLightData`'s count=0 reset and the
`s_lightDataDedupMap` co-reset invariant (`txmmgr.cpp:1279-1283`) are
load-bearing for the dynamic path, and carving static indices out of the
reset re-introduces a (smaller) version of A's dual-cadence reasoning; (b)
the surviving write is O(1) and provably not the lever (the lever is the
recompute, per the D2 capture, Q3); (c) substitutive-not-additive scores
CPU-zone-death, and the recompute IS the zone — the write is not. D-B
retires the zone with minimal blast radius; the persistent-index follow-on
is a documented deferred optimization (analogous to the deferred
`glBufferSubData` sub-range upload), to be reconsidered only if telemetry
ever shows the per-frame slot WRITE itself is a measured multi-ms zone.

---

## Q3. Substitutive sizing (honest, per verify_producer_path)

### What the D2 ON/OFF capture established (carried, not re-derived)

The mission-load-bake plan records (load-bearing, user-confirmed
2026-05-17): the D2 ON/OFF capture REFUTED the first recon's "perf-marginal
post-D2" framing. D2 retired only the FNV+memcmp (~390 ns/call,
~0.6-0.75 ms/frame). The **surviving**
`addLightDataStructureWithPerActorColor` body is **~2.0 ms/frame** for the
static population (the 1792B `*light_data` copy + `decompose` +
template-map traffic + `GatherLights`-on-miss). That ~2.0 ms is the real
CPU zone the bake targets, and it is genuinely-still-live at b41baec
(grep-confirmed: D2 path unchanged Q0).

### What zone dies under D-B

For the bdg/tree static class only, the per-frame chain:

```
bdactor.cpp:2478/2521/4912/4933  (the 4 static call sites)
  -> TG_MultiShape::CacheGpuLightData()                 msl.cpp:1892
       -> firstShapeNodeLeaf->GatherGpuObjectLightDataOnly()  :1918
            -> addLightDataStructureWithPerActorColor    txmmgr.cpp:1203
                 -> sceneLightTemplateKey hash           :1220
                 -> GatherLightsParameters               :1216/:1225
                 -> decomposeFirstActiveLightColor       :1226/:1235
                 -> s_sceneLightTemplateMap find/emplace :1221-1227
                 -> s_lightSlotByActorKey traffic        :1254-1261
```

**dies** — replaced, for the static class, by a `recipeIndex` map lookup
returning the baked constant. The legitimately-surviving O(1) per-frame work
is the slot WRITE: `addLightDataStructure` (`txmmgr.cpp:1153`) append/dedup
to resolve a per-frame index for the baked constant, plus the cull-gated
`markVisible(regIdx, idx)` ferry (`registry:278-287`). Mechs keep the full
D2 path (their floor) — `mech3d.cpp:4501`, untouched.

### The armed counter that proves it

**No new instrumentation needed.** The existing `[LIGHTBRIDGE v1]`
accumulator is the armed lever, intact at b41baec:

- `LbScope _lb_;` `txmmgr.cpp:1206` times the WHOLE
  `addLightDataStructureWithPerActorColor` body; drained per-frame in
  `lbDrainPerFrame` at `resetLightData` (`:1277`); emits
  `[LIGHTBRIDGE v1] frame=... populate={ns,calls,...}` (`:1086`) and a
  per-600 summary (`:1103`). Gated on `MC2_LIGHTBRIDGE` / the object-recon
  env.
- The `s_lbFrameMiss` (`:1223`, template miss → `GatherLightsParameters`
  ran) and `s_lbFrameHit` (`:1229`) and `s_lbFrameNo` (`:1215`) counters
  decompose the population. With the bake ON, the bdg/tree population stops
  REACHING `addLightDataStructureWithPerActorColor` at all (the
  `BldgAppearance` gate helper short-circuits before
  `CacheGpuLightData`), so `populate.calls` and `populate.ns` drop by
  exactly the bdg/tree contribution. The delta IN the existing counter,
  ON vs OFF (`MC2_LIGHTBAKE` toggled), is the armed substitutive proof —
  the producer-path-against-telemetry rule satisfied with zero added
  instrumentation.
- Per `cost_split_instrumentation_is_observer_effect_dominated` and the
  mission-load-bake plan's recorded **user waiver (explicit 2026-05-17)**:
  the substitutive total-frame anti-mirage Tracy proof is USER-WAIVED for
  this slice. The slice ships on the substitutive STRUCTURE (the recompute
  chain is structurally absent for the static class) + the
  `[LIGHTBRIDGE v1]` populate-delta + the parity probe, NOT a claimed
  wall-clock frame number. The commit states the populate-body delta in
  code dimensions only.

---

## Q4. Open questions / CRITICAL items for plan + adversarial review

1. **C++-ONLY confirmed; shader/exe lockstep risk is LOW.** D-B changes NO
   file under `shaders/`. The shader already reads an unbounded
   `light[]` by absolute index (`lighting.hglsl:55,207`); a baked constant
   written into a per-frame slot is read identically to a recomputed one —
   zero shader change. Per `shader_exe_deploy_lockstep.md` the deploy risk
   (stale-UBO-vs-new-SSBO black props) was the b41baec bring-up hazard;
   D-B inherits NONE of it because it touches no shader. **Confirm in the
   plan: the bake commit is C++-only; deploy is exe-only (the lockstep rule
   does not trigger). Adversarial review must verify no `shaders/` file is
   in the bake diff.**

2. **Eager-create + `kLightUploadFloor` interaction with D-B — BENIGN, but
   state it.** b41baec's eager `gos_LightDataSsbo_Upload` (`txmmgr.cpp:329`)
   and the `max(count,64)` upload floor (`:1573-1576`) are about the SSBO
   *buffer lifetime and over-count tolerance*, NOT about which slot a static
   actor uses. D-B does not introduce a persistent SSBO index (it reuses the
   per-frame `addLightDataStructure` slot — Q2), so it does NOT interact with
   the floor or eager-create at all: the baked constant flows through the
   exact same per-frame slot path the recompute used, just with a
   cached-derivation. **The interaction the option-A plan worried about
   (persistent static region vs the floor) does not arise for D-B because
   D-B has no persistent region.** If a future follow-on takes the
   persistent-index optimization (Q2 deferred), THEN the floor/eager-create
   interaction becomes live and must be re-recon'd. Document this boundary in
   the plan.

3. **Invalidate hook exact site (carry the plan's verified anchors).** bldg:
   `BldgAppearance::invalidateStaticRegistration` `bdactor.cpp:3002`, calls
   `GpuStaticPropRegistry::invalidate(staticReg.recipeIndex)` `:3005` under
   guard `staticReg.registered && staticReg.recipeIndex >= 0` `:3004`. tree:
   `TreeAppearance::invalidateStaticRegistration` `bdactor.cpp:5107-5110`
   (same shape). Swap paths route through it: bldg `:1720/:1726/:1767/:1786`,
   tree `:4559/:4564/:4585/:4598`. The mission-scoped
   `recipeIndex -> TG_HWLightsData` cache entry is erased here (growth-
   bounding; the fresh monotonic key already guarantees correct re-bake —
   `registerRecipe` always push_back, `invalidate` tombstones count=0 never
   frees the index, so a post-invalidate re-bake gets a FRESH key with no
   stale-key aliasing). **No batcher/ferry change** — D-B keeps the per-frame
   slot mechanism so `cachedGpuLightIndex_` / `rng.lightDataIndex` semantics
   are unchanged (this was Option A's "stable-slot in-place overwrite"
   complexity — also deleted with A; D-B never had it because the slot is
   per-frame anyway).

4. **Cull-gate adjacency (CRITICAL — `cull_gates_are_load_bearing`).** The
   `BldgAppearance` gate helper replaces the 4 raw
   `shape->CacheGpuLightData()` calls at `bdactor.cpp:2478/2521/4912/4933`,
   which are already INSIDE the existing `inView || g_useGpuStaticProps`
   cull gate. A baked struct may be STORED (in the mission-scoped cache) for
   a culled actor, but is only EMITTED into a per-frame slot when the actor
   is past cull — exactly today's behavior, because the helper is only
   reached when already past the gate. The bake MUST NOT compute or emit for
   culled actors by reaching past the gate. **This remains the single most
   important adversarial-review item; b41baec did not change the call-site
   gating, so the prior plan's B3 stands verbatim.**

5. **D-C-standalone is STILL fatal post-SSBO (negative claim, verified).**
   Adversarial review may ask "the buffer is unbounded now — can't we just
   keep the first-frame index?" The answer is NO unless a persistent index
   carved out of `resetLightData`'s reset is also built (the deferred
   follow-on, Q2). At b41baec `resetLightData` still does
   `lightDataStructuresCount = 0` (`txmmgr.cpp:1279`) and `lightData_` is
   still per-frame scratch overwritten by siblings; a retained first-frame
   `cachedGpuLightIndex_` aliases a wrong slot. The unbounded SSBO does NOT
   fix this — it only removes the *capacity* ceiling, not the per-frame
   *reset*. Plan must state this so review does not mistakenly "simplify"
   D-B into the fatal D-C.

6. **mc2_03 `Render.GpuStaticProps` blowup — now LOWER risk, still check.**
   The option-A recon's HIGHEST-but-one risk (#4) was a mission-3-specific
   blowup hypothesized as C7-render-time-fallback churn from a bad
   `cachedGpuLightIndex_`. D-B keeps `cachedGpuLightIndex_` flowing through
   the unchanged per-frame slot path (valid every frame, never a dangling
   retained index), so it does NOT introduce the C7-fallback hazard. The
   prior recon's worry that mc2_03 might overflow `S` is MOOT (no `S`).
   Residual: a generic "did the gate helper regress any mission" check.
   Keep a mc2_03 controlled `MC2_LIGHTBAKE=0` vs default smoke as a
   defensive gate (cheap; not a blocking pre-gate), but the structural risk
   that motivated it is largely removed by both D-B's design and the SSBO.

7. **Genactor / `no_actor_light` boundary (carry verbatim).** Generic props
   call `genactor SetLightList(NULL,0)` → `actorLightSource == 0xFFFFFFFFu`
   → the `no_actor_light` passthrough branch (`txmmgr.cpp:1214-1218`,
   `GatherLightsParameters` + plain `addLightDataStructure`, no template).
   They are OUT of scope (unaffected by the bake — the helper only wraps the
   bdg/tree call sites). Confirmed unchanged at b41baec.

---

## Summary of the re-scope (what dies vs survives)

- **SURVIVES (Q1):** the entire bake skeleton — lazy-first-gather chokepoint
  in `CacheGpuLightData` (`msl.cpp:1892`), `recipeIndex` monotonic key,
  `firstShapeNodeLeaf->lightData_` friend snapshot, `invalidate` re-bake
  hook (`bdactor.cpp:3002-3005`/`:5107-5110`), cull-gate adjacency, the
  bdg/tree-vs-genactor-vs-mech class split, the nightFactor assumption, and
  the `[LIGHTBRIDGE v1]` armed counter as the proof.
- **DELETED by the SSBO (Q2):** the entire Option-A complexity layer — fixed
  static partition `[0..S)`, per-window static dedup map, self-bounding
  fallback-to-D2, static-partition-full canary, abandoned-slot exhaustion
  bound, the `S`-sizing BLOCKING measurement pre-gate, the count-base
  `0 -> S` rebase + its CRITICAL co-reset-rebase adversarial item, and the
  ">64 → lockstep enlargement / UBO→SSBO" hard dependency (the last
  already shipped AS b41baec). The Option-A plan is SUPERSEDED in full.
- **CHOSEN:** D-B (the mission-load-bake plan's Option B), now the
  unambiguous minimal design rather than a blast-radius compromise, because
  A's only justification (survive the 64 window) is gone.

*Recon only. No code written. All citations grep-verified at `b41baec`.*
