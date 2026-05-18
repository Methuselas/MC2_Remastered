# Static-Lighting Option A — Persistent Static-Light UBO Partition — Recon

- **Branch / HEAD:** `claude/gpu-driven-rendering` @ `2dca942` (verified `git rev-parse HEAD`)
- **Scope:** READ-ONLY recon. No code written. All file:line grep-verified at `2dca942` this invocation.
- **Decision (settled, not relitigated):** implement Option A — a persistent
  static-light UBO partition. Per-frame light cost for a static actor → ZERO.
  Mechs keep today's per-frame dynamic path (D2 floor).
- **Load-bearing premises (NOT re-derived):**
  - No dynamic light emitters exist; static-actor light is a mission-load
    constant (`lighting_is_mission_load_static_no_dynamic_emitters.md`).
  - Offload must be SUBSTITUTIVE: done = the static-class per-frame
    `CacheGpuLightData`→…→`addLightDataStructure` chain ABSENT from a fresh
    capture + every consumer repointed
    (`feedback_offload_must_be_substitutive_not_additive.md`).
  - Cull gates are load-bearing; the bake's per-frame consult must stay inside
    the existing `inView || g_useGpuStaticProps` gate
    (`cull_gates_are_load_bearing.md`).
  - Verify the producer path / armed cost against telemetry before sizing
    (`verify_producer_path_against_telemetry_before_substitution.md`).

---

## 0. Verified mechanics (handed facts, confirmed at `2dca942`)

- **UBO creation / sizing.** `lightDataBuffer_` created
  `txmmgr.cpp:321` (`gos_CreateBuffer(UNIFORM, STATIC_DRAW,
  sizeof(TG_HWLightsData)*lightDataStructuresCapacity, …)`,
  `lightDataStructuresCapacity = 128` at `:318`), bound once at `:322`
  (`gos_BindBufferBase(lightDataBuffer_, LIGHT_DATA_ATTACHMENT_SLOT)`).
- **Per-frame upload.** `txmmgr.cpp:1552-1570`. `cpu_buf_size =
  max(lightDataStructuresCount*sizeof(TG_HWLightsData), 64*sizeof(...))`
  (`:1558-1561`). If `gpu_buf_size < cpu_buf_size` → DestroyBuffer +
  CreateBuffer (`:1563-1565`); else `gos_UpdateBuffer(lightDataBuffer_,
  lightData_, 0, cpu_buf_size)` (`:1568`). **The whole `lightData_` array
  `[0..count)` is re-uploaded from offset 0 every frame.**
- **`gos_UpdateBuffer` is NOT a sub-range update.** `gameos_graphics.cpp:6383-6391`:
  it calls `glBufferData(gl_target, num_bytes, data, GL_DYNAMIC_DRAW)` — a
  FULL buffer reallocation+orphan. The `offset` parameter is **declared but
  ignored** (no `glBufferSubData`, no offset passed to GL). This is a
  load-bearing constraint for Q4.
- **`resetLightData()`** `txmmgr.cpp:1266-1280`: `lightDataStructuresCount = 0`
  (`:1272`); clears `s_lightDataDedupMap` (`:1276`), `s_sceneLightTemplateMap`
  (`:1277`), `s_lightSlotByActorKey` (`:1278`); `s_sceneLightTemplateFrame =
  0xFFFFFFFFu` (`:1279`); drains `[LIGHTBRIDGE v1]` (`:1270`). The
  load-bearing co-reset comment is `:1273-1275`: "Both must reset together —
  slot indices restart from 0 each frame, so any stale hash→slot entries from
  the prior frame are invalid."
- **`addLightDataStructure`** `txmmgr.cpp:1146-1194`: dedup `find` `:1154`,
  memcmp-verify `:1158-1159`, grow-by-128 `:1168-1175`, append at
  `lightData_[lightDataStructuresCount]` `:1177`, return `rv =
  lightDataStructuresCount` then `count++` `:1178-1179`, `s_lightDataDedupMap.emplace`
  `:1180`. **The returned slot is always `== old count`, monotonically from 0.**
- **`addLightDataStructureWithPerActorColor`** `txmmgr.cpp:1196-1264` (C5
  entry). `LbScope _lb_;` `:1199`. Template-miss runs `GatherLightsParameters`
  + `decomposeFirstActiveLightColor` `:1216-1220`; D2 cheap path returns
  `sit->second.slot` `:1250` skipping FNV/memcmp.
- **Static call sites confirmed.** Buildings `bdactor.cpp:2478` (gpuEligible)
  and `:2521` (full-bake); trees `bdactor.cpp:4912` and `:4933`. Each is
  preceded by `…Shape->SetLightList(eye->getWorldLights(), eye->getNumLights())`
  (bldg `:2444`, tree `:4894`) — this is where `s_listOfLights` becomes valid,
  confirming the user's premise that it is NOT valid at `registerRecipe`.
- **`CacheGpuLightData()`** `msl.cpp:1892-1921`: early-out `if (!g_useGpuObjects
  && !g_useGpuMechs) return` `:1900`; linear `firstShapeNodeLeaf` scan
  `:1904-1915`; `cachedGpuLightIndex_ =
  firstShapeNodeLeaf->GatherGpuObjectLightDataOnly()` `:1918`; `cachedFrame_ =
  g_mc2FrameCounter` `:1919`.
- **`registerRecipe`** `gos_static_prop_registry.cpp:230-276`: does NOT call
  `CacheGpuLightData()`, does NOT set a valid `cachedGpuLightIndex_` — only
  `multi->setCachedFrame(g_mc2FrameCounter)` `:274` (Track B first-frame
  staleness-gate seed). **Confirms: no valid light data is available at
  `registerRecipe` time** (premise verified, not assumed).
- **Batcher render read** `gos_static_prop_batcher.cpp:2533-2540`:
  `if (multi->cachedGpuLightIndex_ != 0xFFFFFFFFu) lightDataIndex =
  multi->cachedGpuLightIndex_;` (`:2534-2536`) `else if (firstShapeNodeLeaf)
  lightDataIndex = firstShapeNodeLeaf->GatherGpuObjectLightDataOnly();`
  (`:2537-2539` — the C7 per-prop render-time fallback). Passed to
  `submit(...,lightDataIndex)` `:2572-2574`.
- **Registry ferry** `gos_static_prop_registry.cpp`: `RecipeRange.lightDataIndex`
  field `:103`; `markVisible(regIdx, lightDataIndex)` captures it `:278-288`
  (`rng.lightDataIndex = lightDataIndex` `:287`); flush patches per-frame from
  `s_perInstanceLight ? rng.lightDataIndex : multi->cachedGpuLightIndex_`
  `:391-392`; `invalidate(regIdx)` resets `rng.lightDataIndex = 0xFFFFFFFFu`
  `:301`. `s_recipeRanges.reserve(15000)` `:175`; `regIdx =
  s_recipeRanges.size()` monotonic `:242`.

---

## 1. Partition scheme

### Shader index is absolute and region-agnostic — NO shader change needed

`shaders/include/lighting.hglsl:39-55`: `layout (binding =
LIGHT_DATA_ATTACHMENT_SLOT, std140) uniform LightsData { ObjectLights
light[64]; }`. Consumers index it by an **absolute integer**:

- `static_prop.vert:249` `calc_light(int(inst.lightDataIndex), …)`;
  `:306` `ObjectLights ld = light[int(inst.lightDataIndex)]`.
- `mech.vert:162` `calc_light(int(inst.lightDataIndex), …)`.
- `gos_tex_vertex_lighted.{vert,frag}:79/49` `int(light_offset_.x)`.

`inst.lightDataIndex` flows: `multi->cachedGpuLightIndex_`
(`gos_static_prop_batcher.cpp:2536`) → `submit(...,lightDataIndex)` `:2574` →
packed into `inst.lightDataIndex` `:2166` → consumed in shader as a raw array
subscript into `light[64]`. **There is no region base, no tag bit, no
discriminator anywhere in the shader path.** A static slot at absolute index
`k` is read identically to a dynamic slot at index `k`. **Conclusion: a
persistent static partition needs ZERO shader change** provided static slots
occupy real absolute indices in the same `light[64]` window and the C++ side
uploads that window such that those indices hold the baked structs every
frame.

### Recommended carve: fixed reserved static prefix `[0 .. S)` + dynamic `[S .. count)`

Three candidates evaluated:

| Scheme | Verdict |
|---|---|
| **(A1) Fixed static prefix `[0..S)`, dynamic grows `[S..)`** | **RECOMMENDED.** Simplest count-base refactor: dynamic allocator starts at `S` instead of `0`; `resetLightData` resets the dynamic count to `S` not `0`. Static slots are stable absolute indices in `[0..S)`, never touched by the per-frame path. One scalar (`S`) threads everything. |
| (A2) Static grows top-down from capacity | Rejected. The buffer is grown by `+128` chunks (`txmmgr.cpp:1170-1174`) and **destroyed+recreated** when `gpu_buf_size < cpu_buf_size` (`:1563-1565`); a top-anchored static region's absolute indices shift on every buffer recreate → static `cachedGpuLightIndex_` values silently invalidate. Fatal interaction with the existing realloc path. |
| (A3) Separate second UBO at a new binding slot | Rejected for first cut. Requires a new `*_ATTACHMENT_SLOT`, a second `gos_BindBufferBase`, and a shader change to read static from a different block — violates "NO shader change," doubles upload bookkeeping. Reconsider only if `S` cannot fit under the `light[64]` window (see sizing). |

### Concrete count-base scheme (A1)

The `light[64]` window is the hard cap (`lighting.hglsl:54`). Sizing evidence:
the registry reserves `s_recipeRanges.reserve(15000)`
(`gos_static_prop_registry.cpp:175`) and `s_recipes.reserve(20000)` `:174` —
**there are far more static recipes than 64 UBO slots.** This is the critical
sizing finding: **`S` cannot be "one slot per static recipe."** It must be
"one slot per DISTINCT static `TG_HWLightsData`." Today the dedup map
(`s_lightDataDedupMap`) already collapses identical structs to one slot, and
the load-bearing memory says static light is `getTerrainLight(position)` +
sun/global — so distinct static structs ≈ distinct (terrain-light-bucket)
values, empirically small (the existing `[LIGHT_DEDUP v1]` count printout at
`txmmgr.cpp:1186-1191` is the instrument to size this; the existing 64-slot
window has sufficed for combined mech+static today, observed `maxIdx=57`
per `lighting.hglsl:41-43`). So:

- `S` = a compile-time reserved static-prefix size (proposal: derive from a
  worst-case distinct-static-struct measurement via `[LIGHT_DEDUP v1]`; the
  plan's Stage 0.5 must measure it, NOT guess). Static slots `[0..S)`,
  dynamic `[S..64)`. **If measured distinct-static + peak-dynamic > 64, the
  `light[64]` window must grow (a lockstep C++/GLSL struct-array change per
  `cpp_glsl_ubo_struct_lockstep.md`) — this is a plan dependency, defer the
  exact window size to `mc2-shader-expert` / `mc2-gameos-expert`.**
- `lightDataStructuresCount` semantics change to "next free DYNAMIC slot,"
  initialized to `S` (not 0) at `txmmgr.cpp:319` and reset to `S` (not 0) in
  `resetLightData` `:1272`. `addLightDataStructure` then never returns a slot
  `< S` because `rv = lightDataStructuresCount` and count never drops below
  `S` — **no collision into static space, by construction.**
- A separate `s_staticLightCount` (0-based within `[0..S)`) tracks the baked
  static slots; assigned lazily, never reset per frame, only on mission-unload
  / per-recipe invalidation (Q5).

This is **one buffer, two regions** — recommended over a second UBO.

---

## 2. Permanent-assign trigger + the "skip per-frame entirely" mechanism

### Trigger: lazy first-successful-gather, NOT `registerRecipe`

`registerRecipe` cannot bake (premise verified §0: `s_listOfLights` invalid
there, no `CacheGpuLightData` call, only `setCachedFrame`). The bake must fire
at the first frame the static actor's `CacheGpuLightData()` would have run
with valid lights — i.e. inside `TG_MultiShape::CacheGpuLightData()`
(`msl.cpp:1892`), which is the ONE chokepoint all four static call sites
(`bdactor.cpp:2478/2521/4912/4933`) reach, AND which is past
`SetLightList` (`bdactor.cpp:2444/4894`) so `s_listOfLights` is valid.

**Mechanism (lazy, inside `CacheGpuLightData()`):**

1. The multi carries a `staticBaked_` flag + `staticSlot_` (proposal — new
   `TG_MultiShape` members) and a `staticClass_` flag (set true for
   bdactor/tree-owned multis, false for mechs).
2. First call where `staticClass_ && !staticBaked_ && firstShapeNodeLeaf`:
   run the existing `GatherGpuObjectLightDataOnly()` ONCE, but write its
   resulting `TG_HWLightsData` (the post-decompose struct durably held in
   `firstShapeNodeLeaf->lightData_`) into a permanent static slot `j ∈
   [0..S)` (allocate `j = s_staticLightCount++` or dedup-reuse via a
   mission-scoped static dedup map — see Q3), set `cachedGpuLightIndex_ = j`,
   `staticBaked_ = true`.
3. **Every subsequent frame:** at the top of `CacheGpuLightData()`, `if
   (staticClass_ && staticBaked_) return;` — `cachedGpuLightIndex_` already
   holds the permanent slot `j`, untouched. **No `GatherLights`, no
   `decompose`, no FNV/memcmp, no `addLightDataStructure`, no slot churn.**
   This is the literal per-frame-call death for the static class.

### Why the batcher read is safe with a never-reset permanent index

`gos_static_prop_batcher.cpp:2534`: `if (multi->cachedGpuLightIndex_ !=
0xFFFFFFFFu) lightDataIndex = multi->cachedGpuLightIndex_;`. With Option A,
`cachedGpuLightIndex_ = j ∈ [0..S)` is a **persistent slot whose content
`resetLightData` never clears** (Q3) and which the per-frame upload still
re-sends every frame (Q4). So the batcher reads a permanently-valid absolute
index → the shader reads `light[j]` which holds the baked struct every frame.
**This is exactly the Option C dangling-index hazard FIXED:** Option C failed
because the retained index pointed into the per-frame `[0..)` region that
resets to count 0; Option A's `j` is in the immutable `[0..S)` region, so the
retained index stays valid forever. The C7 fallback at `:2537-2539` is never
taken for a baked static actor (its `cachedGpuLightIndex_` is never the
sentinel after first bake).

### The per-instance `markVisible` / `RecipeRange.lightDataIndex` ferry

`gos_static_prop_registry.cpp:391-392`: flush uses `s_perInstanceLight ?
rng.lightDataIndex : multi->cachedGpuLightIndex_`. With Option A the
`multi->cachedGpuLightIndex_` branch reads a STABLE permanent slot — the
ferry becomes **trivially stable** (the value never changes after bake). The
`s_perInstanceLight` branch (`MC2_STATIC_PER_INSTANCE_LIGHT=1`,
`gos_static_prop_registry.cpp:67`) captures the same permanent slot at
`markVisible` and stores it in `rng.lightDataIndex` — also stable. The bake
must still call `markVisible(regIdx, j)` per frame for visible actors
(unchanged ferry contract, just a constant `j` instead of a per-frame-varying
one). **No ferry change required; it becomes degenerate-stable.** Note: the
per-frame `markVisible` call still runs (cull-gated) — that is the O(1)
per-frame consumer that legitimately remains (Shape-C precedent), distinct
from the retired *recompute*.

---

## 3. `resetLightData` dual-cadence hazard — resolution

**The clean design IS:** static region `[0..S)` is immutable post-assign;
dynamic region `[S..count)` keeps its own count + the existing dedup map
operating only on `[S..)`.

`resetLightData` (`txmmgr.cpp:1266-1280`) changes:

- `lightDataStructuresCount = S;` (was `= 0` at `:1272`) — dynamic count
  resets to the partition base, not zero. Static `[0..S)` survives untouched.
- `s_lightDataDedupMap.clear()` `:1276` stays (it only ever held dynamic-slot
  entries because dynamic alloc never returns `< S`). The load-bearing
  `:1273-1275` co-reset comment **stays true for the dynamic region**: dynamic
  slots restart from `S` each frame, dynamic hash→slot entries are stale, both
  must reset together. The invariant is preserved verbatim, just rebased from
  0 to `S`.
- `s_sceneLightTemplateMap.clear()` `:1277`, `s_lightSlotByActorKey.clear()`
  `:1278`, `s_sceneLightTemplateFrame = 0xFFFFFFFFu` `:1279` — all UNCHANGED
  (they are dynamic-path D2 caches; static actors no longer reach this code
  because `CacheGpuLightData` early-returns before `addLightDataStructure*`).

**Static region has its OWN mission-scoped state, never per-frame-reset:**
`s_staticLightCount` + (optional) a `s_staticLightDedupMap` keyed by the
static struct hash, cleared ONLY in `MC_TextureManager::destroy()` /
`resetLightData`'s mission-unload caller — NOT in the per-frame
`resetLightData`. The clean rule: the per-frame `resetLightData` resets ONLY
`[S..)`; the static `[0..S)` + its dedup map reset only on mission-unload.

**Every read/write of `lightDataStructuresCount` / the dedup map that must
become dynamic-region-relative (grep-verified):**

- `txmmgr.cpp:319` init `= 0` → `= S`.
- `txmmgr.cpp:1158` `slot < lightDataStructuresCount` memcmp-verify bound —
  fine as-is (slot is dynamic, count ≥ S, still a valid upper bound).
- `txmmgr.cpp:1168` `lightDataStructuresCount + 1 >= capacity` grow check —
  fine (capacity sized for the whole buffer incl. `[0..S)`; ensure initial
  `lightDataStructuresCapacity` ≥ `S + peak-dynamic`).
- `txmmgr.cpp:1177-1179` append `lightData_[count]`, `rv = count`, `count++`
  — correct once `count` starts at `S` (returns ≥ S, never collides static).
- `txmmgr.cpp:1186` `(count & 0xFF)` diagnostic — cosmetic only.
- `txmmgr.cpp:1272` reset `= 0` → `= S` (the load-bearing change).
- `txmmgr.cpp:1286` `peekLightSlot` `idx >= lightDataStructuresCount` bound —
  must change to `idx >= max(lightDataStructuresCount, S)` or be aware
  static `idx < S` are valid even when dynamic count == S; diagnostic only
  but will mis-report static slots if left as-is.
- `txmmgr.cpp:1560` upload `cpu_buf_size = lightDataStructuresCount *
  sizeof(...)` — see Q4 (must cover `[0..S)` too).
- `s_lightDataDedupMap`: `:1154/1155/1180/1276` — stays dynamic-only by
  construction (alloc never returns `<S`); no code change, only the semantic
  note that it never holds static entries.

---

## 4. GPU upload

**Honest first-cut win: re-upload the whole buffer per frame but STOP
RECOMPUTING the static part.** Grep evidence forces this:

- `gos_UpdateBuffer` (`gameos_graphics.cpp:6383-6391`) is `glBufferData` —
  full orphan+realloc, **`offset` ignored, no `glBufferSubData`**. There is
  NO partial/sub-range upload primitive in the current GameOS API. A true
  "upload static once, dynamic per frame" requires either a new
  `glBufferSubData`-based `gos_UpdateBufferSubRange` API (a GameOS-layer
  addition — defer to `mc2-gameos-expert`) or a second buffer (rejected A3).
- The honest framing per `feedback_offload_must_be_substitutive_not_additive.md`:
  **the CPU recompute death is the lever; the GPU upload is secondary.**
  After Option A, the static structs are computed ONCE and then merely *sit*
  in `lightData_[0..S)`; the per-frame `gos_UpdateBuffer` at `txmmgr.cpp:1568`
  still memcpys the whole `[0..count)` to the GPU, but it copies *constant
  bytes* for `[0..S)` — no CPU recompute, just a `memcpy`+`glBufferData` of
  bytes that didn't change. That is acceptable for the first cut: the
  multi-ms lever was the per-frame `GatherLights`+`decompose`+FNV/memcmp+slot
  churn (now dead for the static class), NOT the buffer memcpy.
- `cpu_buf_size` at `txmmgr.cpp:1559-1561` must be floored to cover `[0..S)`:
  `max(lightDataStructuresCount, S) * sizeof(TG_HWLightsData)` (already
  floored to `64*sizeof(...)` at `:1558`, so if `S ≤ 64` this is already
  satisfied — verify `S ≤ 64` or raise the floor).
- **Optional, deferred:** a future `gos_UpdateBufferSubRange` (glBufferSubData
  on `[S..count)` only, leaving `[0..S)` GPU-resident across frames) is a
  clean secondary optimization but is NOT required for the substitutive win
  and adds a GameOS API surface. Recommend the plan ship the "stop
  recomputing static, keep whole-buffer upload" first cut and file the
  sub-range upload as a follow-on.

---

## 5. Invalidation in place

Destruction / LOD fire `BldgAppearance::invalidateStaticRegistration` →
`gos_static_prop_registry.cpp invalidate(int32_t)` `:291-302` (resets
`rng.count = 0` tombstone, `rng.lightDataIndex = 0xFFFFFFFFu` `:301`).

**Recommended invalidation handling (stable-slot, in-place overwrite):**

- Add a per-recipe / per-multi `staticNeedsRebake_` flag, set true by the
  `invalidate` hook (or by `invalidateStaticRegistration` at
  `bdactor.cpp:3002-3005`, deferred grep — verify the exact hook in plan
  Stage 0.5).
- On the next `CacheGpuLightData()` for that multi: because
  `staticNeedsRebake_` is set, do NOT early-return; re-run the gather ONCE
  and **overwrite the SAME `staticSlot_` `j` in place** (`lightData_[j] =
  newStruct`), keep `cachedGpuLightIndex_ = j` unchanged, clear the flag,
  re-set `staticBaked_`. **No slot free, no reshuffle, no
  `s_staticLightCount` change.**
- Per the invalidation memory (`lighting_is_mission_load_static_no_dynamic_emitters.md`)
  and §3 of the prior recon: the LIGHT VALUE for tree↔stump at the same
  position is identical (position-derived + frozen sun); the only real reason
  to re-bake is a *multi-identity* change (destruction/LOD swaps the
  `TG_MultiShape`). If the new multi is a different object, it bakes its own
  `staticSlot_` lazily on its first `CacheGpuLightData` (same path as initial
  bake) — the OLD multi's slot `j` is simply abandoned-in-place (immutable
  region, never reclaimed mid-mission; acceptable per the no-RAM-pressure
  rule `feedback_ram_cost_not_a_concern_below_500mb.md`, but the plan must
  bound total distinct static slots vs `S` — abandoned slots count against
  `S`, so destruction-heavy missions could exhaust `[0..S)`; size `S` with
  destruction churn headroom or add a free-list, a Stage 0.5 decision).
- **Batcher / ferry change required: NONE.** A stable-slot in-place overwrite
  keeps `cachedGpuLightIndex_` constant, so `gos_static_prop_batcher.cpp:2536`
  and the registry ferry `:391-392` read an unchanged value. The `invalidate`
  hook + a per-recipe needs-rebake flag is sufficient. (The slot-abandonment
  exhaustion risk above is the one caveat — flag for Stage 0.5 sizing.)

---

## 6. Honest substitutive sizing + the biggest risk

### What CPU zone dies

For the STATIC class only, the entire per-frame chain
`bdactor.cpp:2478/2521/4912/4933` → `TG_MultiShape::CacheGpuLightData()`
(`msl.cpp:1892`: `firstShapeNodeLeaf` linear scan `:1904-1915` +
`GatherGpuObjectLightDataOnly()` `:1918`) → `addLightDataStructureWithPerActorColor`
(`txmmgr.cpp:1196`: template hash, `GatherLightsParameters` `:1209/1218`,
`decomposeFirstActiveLightColor` `:1219/1228`, the D2 map lookups
`:1214/1247`, the 1792B `*light_data = it->second.data` copy `:1225`, the
`LbScope` chrono pair `:1199`) → `addLightDataStructure` (`:1146`, slot
churn) **goes to ZERO per frame** (replaced by an early `return` at the top of
`CacheGpuLightData`). This is true per-frame-call death, not a cheaper
recompute — the substitutive bar (`feedback_offload_must_be_substitutive_not_additive.md`).
The legitimately-remaining O(1) per-frame work for the static class is only
the cull-gated `markVisible(regIdx, j)` ferry call (Shape-C precedent: the
recompute dies, the O(1) consumer remains). The measured envelope: the D2
ON/OFF capture attributed ~the ~2ms/frame populate-body to this chain for the
static population; Option A retires that body for the static class. Mechs keep
the D2 path (their floor) — `mech3d.cpp` static-class flag is false.

### Single biggest implementation hazard (ranked)

1. **HIGHEST — the `light[64]` window vs distinct-static-slot count
   (sizing).** This is bigger than the count-base refactor. The registry
   reserves 15000 recipes (`gos_static_prop_registry.cpp:175`) but the UBO is
   `light[64]` (`lighting.hglsl:54`). Option A only works if DISTINCT static
   `TG_HWLightsData` (post-dedup) + peak dynamic ≤ 64. If not, `S` overflows
   the window and the slice requires a lockstep C++/GLSL `ObjectLights
   light[N]` enlargement (`cpp_glsl_ubo_struct_lockstep.md` — the mc2_24
   regression precedent) AND a possible UBO→SSBO conversion if `N` exceeds
   `MAX_UNIFORM_BLOCK_SIZE` (`lighting.hglsl:48-50` already names this
   fallback). **Stage 0.5 MUST measure distinct-static via `[LIGHT_DEDUP v1]`
   (`txmmgr.cpp:1186-1191`, already armed) on the worst-case
   zoomed-out-big-map camera before committing — do not guess `S`.** This is
   the producer/telemetry-before-substitution rule
   (`verify_producer_path_against_telemetry_before_substitution.md`).
2. **MEDIUM — count-base refactor correctness.** Rebasing
   `lightDataStructuresCount` from 0→S touches `resetLightData` `:1272`, init
   `:319`, `peekLightSlot` `:1286`, the upload floor `:1559`. Mechanical but
   the load-bearing co-reset invariant (`:1273-1275`) must be preserved
   verbatim-rebased; an off-by-S in any one site = wrong-light corruption.
   Adversarial-review CRITICAL item.
3. **LOWER — batcher index stability.** Option A's design *inherently* makes
   this safe (permanent `[0..S)` slot, §2): the dangling-index hazard that
   killed Option C is structurally fixed. Low residual risk.
4. **The mc2_03 `Render.GpuStaticProps` blowup (independent investigation).**
   The stashed per-frame-bake WIP showed a mission-3-specific
   `Render.GpuStaticProps` blowup. **Hypothesis:** that WIP was "the wrong
   abstraction" (per the prompt) — likely a *per-frame* bake that produced a
   transiently-invalid `cachedGpuLightIndex_` and forced the C7 render-time
   fallback (`gos_static_prop_batcher.cpp:2537-2539`,
   `GatherGpuObjectLightDataOnly()` per-prop at render — the exact thing the
   prior recon notes "blows up `Render.GpuStaticProps` if the index is bad").
   Option A's PERMANENT valid-index design **inherently removes the C7
   fallback path for baked static actors** (`cachedGpuLightIndex_` is never
   the sentinel after first bake), so if the mc2_03 blowup was C7-fallback
   churn, Option A fixes it as a side effect. **BUT** this is a hypothesis,
   not verified — there may be an independent mc2_03 bug (e.g. a
   mission-3-specific recipe count, destruction churn exhausting `[0..S)` per
   §5, or a distinct-static count that overflows `S` only on mc2_03). The
   plan must include a mc2_03-specific armed smoke (`[LIGHT_DEDUP v1]` count +
   `Render.GpuStaticProps` Tracy) as a Stage gate and NOT assume Option A
   fixes mc2_03 for free. Rank: investigate as an independent risk, treat the
   "Option A fixes it" outcome as the optimistic case to be *proven*, not
   assumed (`verify_producer_path_against_telemetry_before_substitution.md`).

---

## Open questions for the plan / adversarial review

1. **`S` sizing (BLOCKING).** Measure distinct-static `TG_HWLightsData`
   (post-dedup) + peak dynamic on worst-case zoomed-out-big-map across all
   tier1 missions (esp. mc2_03) via `[LIGHT_DEDUP v1]` before choosing `S`.
   If > 64, the slice carries a lockstep `light[N]` enlargement (and possibly
   UBO→SSBO) as a hard dependency. Defer the window-size mechanics to
   `mc2-shader-expert` / `mc2-gameos-expert`.
2. **Static-class flag plumbing.** How does a `TG_MultiShape` know it is
   static vs mech? The four static call sites are bdactor/tree; mechs are
   `mech3d.cpp`. Confirm a clean per-multi `staticClass_` discriminator
   (set at registration or owner-side) — defer ownership to `mc2-render-expert`.
3. **Invalidation hook + slot-abandonment exhaustion.** Confirm the exact
   `invalidate` → needs-rebake hook (`bdactor.cpp:3002-3005`
   `invalidateStaticRegistration`, deferred grep). Decide: abandon-in-place
   (simple, but destruction-heavy missions exhaust `[0..S)`) vs a static
   free-list (complex). Size `S` with destruction churn headroom either way.
4. **`registerRecipe` reach (carried from prior recon).** Does the static
   class always pass through `CacheGpuLightData()` after `SetLightList`?
   Confirmed for bdactor/tree at `:2444→2478/2521`, `:4894→4912/4933`;
   genactor path NOT re-verified this invocation — flag for Stage 0.5.
5. **`nightFactor` / time-of-day assumption.** Bake validity assumes
   `eye->nightFactor` mission-constant (no day/night). State explicitly; name
   it the designated revisit trigger for a future dynamic-light feature
   (`lighting_is_mission_load_static_no_dynamic_emitters.md`).
6. **Cull-gate adjacency (CRITICAL adversarial item).** The per-frame
   `markVisible(regIdx, j)` ferry call MUST stay inside the existing `inView
   || g_useGpuStaticProps` gate; a baked struct may be STORED for a culled
   actor but only ferried/emitted when visible (`cull_gates_are_load_bearing.md`).
7. **mc2_03 independent-bug check.** Do NOT assume Option A fixes the stashed
   WIP's mc2_03 `Render.GpuStaticProps` blowup; require a mc2_03-specific
   armed smoke gate proving C7-fallback count → 0 AND `Render.GpuStaticProps`
   normal before declaring it fixed.

---

*Recon only. No code written. All citations grep-verified at `2dca942`.*
