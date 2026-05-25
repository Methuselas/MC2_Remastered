# Track B — Widen static-prop registry recon

Date: 2026-05-06
Worktree: `nifty-mendeleev`
Author: Claude (Opus 4.7, recon-zero)
Scope: read-only; no code changes.

---

## 1. TL;DR

- **Mission-load enumeration is feasible.** `GameObjectManager::loadTerrainObjects`
  (code/objmgr.cpp:937) walks `objData` and dispatches via `addObject`
  (code/objmgr.cpp:1132) to TREE / BUILDING / TREEBUILDING / TURRET / GATE
  init paths. This runs during `Mission::init` BEFORE
  `GpuStaticPropBatcher::finalizeGeometry()` (code/mission.cpp:3044). A
  registry walk slot at the same point is structurally available.
- **The "late-register cascade" risk is smaller than the cull-lessons doc
  suggested.** A direct trace of all 24 campaign missions found only TWO
  unique unregistered nodeIds — `Cylinder01` (skybox) and `compassplane`
  (HUD compass) — both vestigial/non-world. (Commit `06ac847` 2026-05-02.)
  The "Shilone" bomber and other artillery-spawned actors create
  appearances at gameplay time but are GVs/animated, not static-prop
  scope.
- **TGL pool sizing is comfortably within budget.** Observed peaks across
  tier1 (mc2_10, mc2_17 with actual gameplay) hit 3–8% of the 500K vertex
  pool, 22–44% of the 200K face/triangle pools. Widening "every static
  prop registered, regardless of visibility" is bounded by total mission
  prop count, not theoretical worst case; at observed tier1 densities the
  current pools have 12–30× headroom.
- **Pre-bake "static lighting" is NOT trivially static.** `GatherLightsParameters`
  (mclib/txmmgr.cpp:959) reads `TG_Shape::s_listOfLights` — a global
  mutable list that includes weapon flashes, fires, mech spotlights. A
  permanent "bake once at register" of the lighting cache would freeze a
  building's lighting at mission-load state. The slice 3.C/3.D fix
  already addresses this with the per-frame `cachedFrame_` stamp pattern;
  Track B should preserve and widen that pattern, not replace it with a
  truly-static bake.
- **The 3.C/3.D black-tree fix DOES still apply, and the invariant
  generalizes cleanly.** The frame-stamp gate `multi->getCachedFrame() != currentFrame`
  in `GpuStaticPropRegistry::flush` (gos_static_prop_registry.cpp:144) is
  the keystone protection against cull-skipped-update + render-bypass
  staleness. Track B widening (more registered actors) does NOT break it,
  but it DOES expand the population subject to it: every newly-widened
  actor type must inherit the same `update()` → `CacheGpuLightData()` →
  `cachedFrame_=g_mc2FrameCounter` lifecycle.

**Net read on the 3-week estimate:** The estimate looks correct in
shape but the dominant risk axis is NOT mission-load enumeration (which
is straightforward) — it's correctly extending IsStaticNow / staticReg /
cachedFrame_ to GenericAppearance, GVAppearance (where applicable), and
Gate (which mutates), without recreating the black-tree class of bug.

---

## 2. Current registry surface — verified API

`GpuStaticPropRegistry` is a free-namespace API in
`GameOS/gameos/gos_static_prop_registry.{h,cpp}` (~50 LOC declaration,
~190 LOC implementation).

| Function | Signature | Purpose | Citation |
|---|---|---|---|
| `init` | `void()` | Reserves CPU vectors (20K recipes, 15K ranges, 15K live indices). | gos_static_prop_registry.cpp:68-77 |
| `destroy` | `void()` | Clears + shrinks vectors. | :79-83 |
| `isEnabled` | `bool()` | Returns startup-parsed `MC2_STATIC_PROP_REGISTRY` (default ON 2026-05-05). | :66; default at :38 |
| `frameBegin` | `void()` | Clears `s_liveRangeIndices`. Called from gamecam.cpp:201. | :85-88 |
| `registerRecipe` | `int32_t(TG_MultiShape*, const std::vector<GpuStaticPropInstance>&)` | Inserts batch into `s_recipes`, pushes a `RecipeRange{first,count,multi}` onto `s_recipeRanges`. Returns regIdx. | :90-102 |
| `markVisible` | `void(int32_t regIdx)` | Appends regIdx to `s_liveRangeIndices` with bounds + tombstone guard. | :104-109 |
| `invalidate` | `void(int32_t regIdx)` | Zeroes the recipe entries; sets `count=0` (tombstone), `multi=nullptr`. | :111-120 |
| `isReady` | `bool(int32_t regIdx)` | Bounds + count>0 check. | :122-126 |
| `flush` | `void()` | For each live regIdx, applies cull-aware frame-stamp gate, patches `lightDataIndex` from `multi->getCachedGpuLightIndex()`, calls `batcher.submitCachedInstance` per leaf. | :128-186 |

**Per-instance storage today** (`RecipeRange`, anon namespace,
gos_static_prop_registry.cpp:47-52):
```c++
struct RecipeRange {
    uint32_t       first;   // index into s_recipes
    uint32_t       count;   // 0 = tombstone
    TG_MultiShape* multi;   // for cachedGpuLightIndex / cachedFrame_
};
```
The actual instance data lives in `std::vector<GpuStaticPropInstance>
s_recipes` (file-scope, line :54). Each leaf submission is one
`GpuStaticPropInstance` (112 bytes — `gos_static_prop_batcher.h:13-35`)
containing `modelMatrix[16]`, `typeID`, `firstColorOffset`, `flags`,
`lightDataIndex`, `aRGBHighlight[4]`, `fogRGB[4]`.

**Registration cadence today: per-render (NOT mission-load).** The first
successful `submitMultiShape()` for a given actor returns a snapshot via
`getLastBuiltBatch()` (gos_static_prop_batcher.h:214), which the
appearance's render() captures and forwards to `registerRecipe`:

- `BldgAppearance::render` — bdactor.cpp:1653-1663 (first-render registration block)
- `TreeAppearance::render` — bdactor.cpp:4285-4293 (parallel block)

So the registry today is "fast-path replay of cull-approved instances
that have rendered at least once" — the Track B widening hypothesis is
correct in framing.

---

## 3. Static-prop spawn enumeration

| Category | Spawn site | Registration timing today | Track-B target |
|---|---|---|---|
| **Buildings (BUILDING / TREEBUILDING)** | `objmgr.cpp:1212-1240` via `addObject`, called once per record from `loadTerrainObjects` :962-972 (mission-load) | First-render via `BldgAppearance::render` bdactor.cpp:1653 | Mission-load: feasible — actor, type, and shape are valid by mission.cpp:3044. |
| **Trees (TREE / TERRAINOBJECT)** | `objmgr.cpp:1153-1170` via `addObject` (mission-load) | First-render via `TreeAppearance::render` bdactor.cpp:4285 | Mission-load: feasible (same path). |
| **Generics** | Spawn-time only (skybox: gamecam.cpp:510 "theSky"; compass: gamecam.cpp:654) | NOT registered in registry today (no staticReg field on GenericAppearance — verified via `grep staticReg mclib/genactor.cpp` = no match) | Allowlist: compass + skybox both vestigial/HUD (objbatcher_late_register_allowlist.txt). Track B should not register either. |
| **Gates** | `objmgr.cpp:1192-1210` via `addObject` (mission-load) — wraps as `GatePtr` but `Gate::init` constructs a `BldgAppearance` (code/gate.cpp:699). Appearance is mission-load-spawned; the GAMEPLAY animation (open/close) is the dynamic part. | First-render via Bldg path (Gate uses BldgAppearance). | Conditional: gate is "static when closed, animated mid-open"; the existing `staticReg` shape-pointer/needsFullBakeNextFrame machinery handles mesh swaps but `bdAnimationState != -1` already disqualifies via `BldgAppearance::isStaticEligible` (bdactor.cpp:2638-2652). Static when settled. |
| **Turrets** | `objmgr.cpp:1172-1189` via `addObject` (mission-load), `Turret::init` at turret.cpp:2148 creates `BldgAppearance`. | First-render via Bldg path. | Conditional: turret rotates while tracking (animated). `BldgAppearance::isStaticEligible` covers `activity` / `bdAnimationState` already; turret will register only while idle. |
| **Artillery towers (LARGE_ARTLRY/SMALL_ARTLRY/SENSOR_ARTLRY)** | code/artlry.cpp:1577 / 1645 / etc. — `Artillery::init` creates `BldgAppearance` for the building. Spawned at gameplay time, not mission-load. | First-render via Bldg path (currently); potentially fires AFTER finalizeGeometry. | LATE-REGISTER candidate: spawn happens on artillery init, which runs from `MissionGUI::artillery*` paths during play. Captured in late-reg trace? See section 4. |
| **Bombers (Shilone)** | artlry.cpp:1597 / 1664 — `Artillery::init` creates `GVAppearance` named "Shilone". | NOT in registry (no GVAppearance staticReg). | Out of Track B: Shilone is animated (flying GV), Track D's territory. |
| **Buildings spawned at gameplay** | `vTol[]` — BldgAppearance created at missiongui.cpp:4294, 4336, 5750, 5756; warrior.cpp:7576; simplecamera.cpp:495 | First-render via Bldg path. | LATE-REGISTER candidates: vTol is gameplay-spawned. |
| **Compass / skybox** | gamecam.cpp:510 (skybox), :654 (compass via BldgAppearance). | Allowlisted via nodeId (`Cylinder01`, `compassplane`). | Out of Track B (allowlisted). |

**Key observations:**

- The mission-load enumeration site is `addObject` at code/objmgr.cpp:1132,
  called from `loadTerrainObjects` :962-972, called from `Mission::init`
  via `ObjectManager->loadTerrainObjects(&pakFile, loadProgress, 30)` at
  mission.cpp:2809. This runs BEFORE `finalizeGeometry()` at
  mission.cpp:3044.
- A subset of static props (artillery towers, vTol, compass, skybox) are
  created at gameplay time, NOT in `addObject`. The mission-load
  enumeration cannot reach them. They must remain on the first-render
  registration path OR be covered by an explicit gameplay-time
  registration hook.
- The "spawn list determinable before any rendering happens" claim is
  TRUE for the dominant population (terrain objects in the .ase pak),
  FALSE for artillery/vTol/HUD which spawn during play.

---

## 4. Late-register types — the actual list

**Direct traced inventory across all 24 campaign missions** (commit
`06ac847f`, 2026-05-02, MC2_GPU_OBJECTS=1 MC2_OBJBATCHER_TRACE=1):

| nodeId | caller | missions | classification |
|---|---|---|---|
| `Cylinder01` | skybox | 24/24 | vestigial post terrain CPU→GPU |
| `compassplane` | compass | 22/24 | in-game compass HUD overlay |

That is the complete late-register inventory.

The "Shilone" bomber and artillery-tower `BldgAppearance` instances
referenced in the prior cull-lessons doc (`docs/gpu-static-prop-cull-lessons.md`
lines 204-209: "2 types per mission") were the WORKING THEORY in
2026-04-20, before the actual nodeId-keyed trace was done. The
nodeId-keyed allowlist (data/objbatcher_late_register_allowlist.txt:22, :28)
contains only `Cylinder01` and `compassplane`.

**Consequence for Track B:** The "late-registering pointer-form types"
risk listed in the roadmap risk register (`docs/superpowers/mc3-rendering-modernization-roadmap.md:274`)
is empirically smaller than feared. Mission-load enumeration handles the
real population (everything spawned by `addObject`); the only
non-mission-load spawns are the two HUD-class allowlisted types.

The artillery / vTol / `compass` BldgAppearance spawn paths described in
section 3 do NOT fire as late-register events because they create
appearances whose TG_TypeShape was registered at map load via the
existing `GpuStaticPropBatcher::registerType` walk during early actor
init (a different code path). The mission-load enumeration still misses
their *actor-instance* registration timing — but the existing
first-render registration block in `BldgAppearance::render` catches them
as soon as they spawn. Track B can either (a) keep first-render
registration for these and only widen mission-load registration to
`addObject`-spawned actors, or (b) add an explicit hook in
`Artillery::init` / `MissionGUI::artillery*` paths.

---

## 5. Mission-load enumeration feasibility

**Conditional YES.** The dominant population (every static prop spawned
via `loadTerrainObjects`) is fully enumerable at mission load. The
non-trivial actors (artillery/vTol/HUD) need either keep-on-first-render
or an explicit secondary hook.

Evidence:

- The `addObject` switch at `objmgr.cpp:1152-1244` already calls
  `((TerrainObjectPtr)obj)->init(true, objType)` etc. The init path
  reaches `BldgAppearance::init` / `TreeAppearance::init`, which sets
  `bldgShape`/`treeShape`. After the init returns, the actor has all the
  state required for `registerRecipe` to succeed: shape pointer, type,
  cell position (set at `objmgr.cpp:1248`), rotation (`:1255`).
- The full sequence in `Mission::init` is:
  1. `GpuStaticPropBatcher::onMapLoad()` at mission.cpp:1645 (resets state)
  2. `GpuStaticPropRegistry::init()` at mission.cpp:1646 (reserves vectors)
  3. (lots of setup, terrain load, mission file parse...)
  4. `ObjectManager->loadTerrainObjects(&pakFile, ...)` at mission.cpp:2809
     — this is where every TerrainObject/Building/Gate/Turret is
     constructed via `addObject`.
  5. `GpuStaticPropBatcher::instance().finalizeGeometry()` at mission.cpp:3044
     — uploads shared VBO/IBO.
- A Track-B "mission-load registry walk" can hook anywhere between
  step 4 and step 5. The natural site is right after step 4: a small
  loop iterating `terrainObjects[0..numTerrainObjects)`,
  `buildings[0..numBuildings)`, `turrets[]`, `gates[]` and calling each
  appearance's `registerStatic()` (new virtual or member method) which
  does the equivalent of today's first-render registration block.
- **Caveat**: today's first-render registration uses the result of a
  successful `submitMultiShape()` to obtain `getLastBuiltBatch()`. A
  mission-load walk would need to either (a) drive `submitMultiShape`
  during the walk (no GL state assumed safe — `finalizeGeometry` hasn't
  run yet, the shared VBO isn't uploaded), OR (b) factor out a "build
  batch without submitting" path. Option (b) is the cleaner engineering
  move and is exactly what Stage 3.C's `getLastBuiltBatch()` is already
  built to support — call submit, capture, register — but the order
  matters: shape + texture handoffs need to be valid first.

**No structural blocker.** The enumeration data is reachable; the design
question is "build the batch synchronously at register time vs.
opportunistically at first render." Either works.

---

## 6. Pool sizing audit

**Current pool sizes** (code/mission.cpp:3141-3154, set at every
`initTGLForMission` startup):

```c
colorPool   ->init(500000);   // 500K × 16B = 8 MB
vertexPool  ->init(500000);   // 500K × 32B = 16 MB
facePool    ->init(200000);   // 200K × 4B  = 800 KB
shadowPool  ->init(500000);   // 500K × ~16B
trianglePool->init(200000);
```

`tglHeapSize = 128 * 1024 * 1024` (mission.cpp:3127). All allocate from
this heap.

**Observed peaks** (tier1 smoke run 2026-05-05T15-24-25, registry
default-on):

| Mission | vertex peak | color peak | face peak | shadow peak | triangle peak |
|---|---|---|---|---|---|
| mc2_01 | (no gameplay frames) | — | — | — | — |
| mc2_03 | (no gameplay frames) | — | — | — | — |
| mc2_10 | 18,639 / 500,000 (3%) | 18,639 (3%) | 44,022 (22%) | 18,639 (3%) | 22,011 (11%) |
| mc2_17 | 44,215 / 500,000 (8%) | 44,215 (8%) | 89,606 (44%) | 44,215 (8%) | 44,803 (22%) |
| mc2_24 | (no gameplay frames) | — | — | — | — |

(mc2_01/03/24 logs show only the logistics-stage 0/2000 peaks because
the smoke harness exits before reaching a gameplay frame in those runs.
mc2_10 and mc2_17 are the data points with real gameplay peaks. Source:
tests/smoke/artifacts/2026-05-05T15-24-25/*.log.)

**Key observations:**

- **Vertex pool peaks at 8% (mc2_17), face pool peaks at 44%.** The face
  pool is the proximate constraint, not the vertex pool. A 2× increase
  in registered actors would push the face pool to ~88% on mc2_17 — too
  close to ceiling. Track B's "mission-load register every static prop
  whether visible or not" must NOT translate to "TransformShape every
  prop every frame" — that's the cascade hazard documented in
  `cull_gates_are_load_bearing.md`.
- **Track B's actual pool impact is bounded by the static-replay path,
  not by re-running TransformShape for offscreen actors.** Once an actor
  is registered, the static path `GpuStaticPropRegistry::flush` reuses
  the captured batch; no TGL pool allocation per-frame for static
  replay. The TGL pool remains bounded by the count of actors that DO
  fall through to dynamic submitMultiShape (animated/destruction/etc.).
- **Pool budget is fine for Track B** as long as the widening path
  preserves "static actors do not allocate from TGL pools per frame."
  Verify in M1 by checking face/triangle pool peaks do not regress.

---

## 7. Pre-bake static lighting feasibility

**Verdict: NOT trivially static.** The "static lighting input is also
static" premise in the roadmap (`mc3-rendering-modernization-roadmap.md:118`)
is partially false:

- `GatherLightsParameters` (mclib/txmmgr.cpp:959-1045) iterates
  `TG_Shape::s_listOfLights[0..s_numLights)` (txmmgr.cpp:974-975).
- `s_listOfLights` is the GLOBAL active-lights list. It includes the sun
  (static), but ALSO includes weapon flashes, fires, mech spotlights —
  all dynamic. Mech `BldgAppearance::touch()` and the slice 3.C
  `ResubmitCachedGpuLightData` path (msl.cpp:1810) explicitly recompute
  per frame because of this.
- A truly-static "bake once at register" of `lightData_` would freeze a
  building's lighting at mission-load state — the building would not
  light up under nearby weapon fire, would not get spotlight
  contribution from a mech beside it. Visually wrong.

**What IS already static across frames for a static actor:**

- `modelMatrix[16]` — set from `shapeToWorld` once the actor settles.
- `typeID`, `firstColorOffset`, `aRGBHighlight`, `fogRGB`, `flags` —
  modulo destruction or paint changes, also static.
- The vertex SSBO — geometry is per-type (shared), not per-instance.

**What is NOT static frame-to-frame:**

- `lightDataIndex` — must be recomputed any frame the active lights set
  changes. The slice 3.C/3.D fix uses `cachedFrame_ == g_mc2FrameCounter`
  to detect "this actor's update() ran this frame" → cache is fresh →
  use it. Otherwise: skip the static draw and let next frame catch it
  when update() runs.

**Per-instance vertex SSBO size cost** if Track B chose to bake a
per-instance vertex stream (not per-type): N_instances × ~200 verts ×
~32B ≈ 6 KB per actor. For ~3000 actors typical mission, ~20 MB. Cheap
on a 24 GB card.

**Per-instance light-cache cost as it stands today**: 1 UBO slot per
actor per frame. Already implemented. No new per-instance cost.

**Conclusion:** Track B should NOT replace the per-frame
`CacheGpuLightData` machinery with a true mission-load bake. The
correct extension is: register every static actor at mission load with
its `modelMatrix` / `firstColorOffset` / `flags` / `aRGBHighlight` /
`fogRGB` baked into the static recipe (those ARE static), and continue
to patch `lightDataIndex` per-frame from the existing `cachedFrame_`-stamped
cache.

---

## 8. Ever-visible pre-cull feasibility

**Data reachable, algorithm not designed.** Confirming the data inputs:

- **Camera altitude bounds:** `Camera::AltitudeMinimum` / `AltitudeMaximumLo` / `AltitudeMaximumHi`
  loaded from system config (code/mechcmd2.cpp:1263-1276); defaults
  ~110/1500/1600. Wolfman zoom env-pushes higher (~6000 per memory
  files).
- **Camera position envelope:** Bounded by the playable terrain extent.
  `Terrain::worldUnitsMapSide` (mclib/terrain.cpp:102) is computed at
  load. `tileRowToWorldCoord` and `tileColToWorldCoord` (mclib/terrain.cpp:286-289)
  give the absolute world-coordinate range. The camera cannot escape the
  map.
- **Per-actor world position:** Already known at registration time —
  `obj->setTerrainPosition(objData->vector, numbers)` at
  code/objmgr.cpp:1248 and `obj->getPosition()` thereafter.
- **Per-actor bounding extent:** `recalcBounds` already maintains
  per-actor sphere bounds (mclib/bdactor.cpp:1090-1167 per cull-lessons).

**Pragmatic answer:** The data exists. Building the union-of-frustums
algorithm is non-trivial (camera pitch + yaw + zoom × map extent), but
even a conservative version — "drop actors more than (max camera
diagonal + max zoom-out FOV-half-extent + safety margin) from the
nearest playable cell" — would prune nothing in stock missions because
all stock spawns are within playable cells. **Realistic per-tier1
benefit is ~0%.** The pre-cull is worth budgeting only if MC2X/Wolfman
mods spawn props off-map; for stock content it's a non-feature.

Recommendation: descope from M1 of Track B unless mod content evidence
arises.

---

## 9. Static/dynamic classification machinery — what Track B inherits

The classification surface lives in three places and is incomplete:

**`Appearance::IsStaticNow()` (virtual, default false)** —
mclib/appear.h:138. The default returns false, meaning "every appearance
type is dynamic by default." Subclasses opt in:

- `BldgAppearance::IsStaticNow()` — bdactor.cpp:2654-2660. Returns true
  iff `staticReg.registered && staticReg.shape == bldgShape && !needsFullBakeNextFrame && isStaticEligible()`.
- `TreeAppearance::IsStaticNow()` — bdactor.cpp:4733-4736. Same shape
  pointer + `isStaticEligible` pattern.
- GenericAppearance, GVAppearance, Mech3DAppearance — NOT overridden.
  They inherit the default `false` and never enter the static path.

**`BldgAppearance::isStaticEligible()`** — bdactor.cpp:2638-2652. The
canonical "type-level + instance-level disqualifiers" predicate:

```c++
if (!appearType)                       return false;
if (appearType->spinMe)                return false;   // spinning radar dish, etc.
if (bldgTypeHasAnimations(appearType)) return false;   // animated building type
if (drawFlash)                         return false;   // mid-flash (damage)
if (destructFX)                        return false;   // mid-destruction
if (activity)                          return false;
if (activity1)                         return false;
if (bdAnimationState != -1)            return false;   // currently animating
return true;
```

**`StaticRegistration` struct** — bdactor.h:209-214 (Bldg) and :503-508
(Tree). Both have:

```c++
struct StaticRegistration {
    int32_t        recipeIndex;   // -1 = unregistered
    bool           registered;
    TG_MultiShape* shape;         // for shape-swap detection
};
```

**`invalidateStaticRegistration()` virtual** — appear.h:151 (default
no-op). Bldg + Tree override (bdactor.cpp:2676-2681, :4754-4758) to call
`GpuStaticPropRegistry::invalidate(staticReg.recipeIndex)` and zero the
struct.

**`needsFullBakeNextFrame` flag** — set by Stage 2.B late-registration
recovery (bdactor.cpp:1645, :4279). Must clear and re-register on the
next frame.

**Track B inheritance picture:**

- For Track B to widen "every static prop," the `StaticRegistration`
  struct + `IsStaticNow` + `isStaticEligible` + `invalidateStaticRegistration`
  + `cachedFrame_` discipline must extend to:
  - **GenericAppearance** — wire if there are world-space static
    Generics. Verify with a registration trace; current allowlist suggests
    Generics are mostly HUD/skybox.
  - **Gate** — already routed through BldgAppearance and gated by
    `bdAnimationState`; no change.
  - **Turret** — already routed through BldgAppearance and gated by
    `activity`; no change.
- **GVAppearance and Mech3DAppearance are out of Track B scope** (animated
  by definition; Track D's territory).

**Static-flips-during-gameplay machinery:**

- "Building destroyed → ruin" is handled by `bldgShape` swap. The
  shape-swap detection `staticReg.shape != bldgShape` (bdactor.cpp:1628,
  :4266) routes to `invalidateStaticRegistration` then re-registration on
  next render.
- "Gate opens → animated for a few seconds → static again" — the
  `bdAnimationState != -1` clause in `isStaticEligible` keeps gates out
  of the static path while animating; they re-enter when the animation
  state clears.
- "Turret rotates while tracking" — `activity` clause guards.

---

## 10. Risk register — Track B-specific

| Risk | Severity | Note |
|---|---|---|
| Widening to Generic without auditing world-space vs HUD usage | HIGH | The two known unregistered types (`Cylinder01`, `compassplane`) are both Generics. Adding Generic to the registry walk WITHOUT a world-vs-HUD predicate will register HUD overlay actors as world geometry. Need an explicit check (e.g., is the actor in `Terrain::IsGameSelectTerrainPosition`). |
| Mission-load registration runs before TransformMultiShape; `lightData_` not populated yet | HIGH | First-render registration today succeeds because `submitMultiShape` runs AFTER `update()` runs `TransformMultiShape`, which calls `CacheGpuLightData` and stamps `cachedFrame_`. A mission-load registration walk runs BEFORE the first update, so `cachedFrame_ == 0xFFFFFFFFu` (sentinel) — flush() will silently drop every static draw on the first ~1 frame after mission start until update() catches up. Likely harmless (1-frame pop) but verify. |
| Pool peak regression if static-actor TransformShape gets called per-frame | MEDIUM | Face pool already at 44% on mc2_17. Track B must preserve the "static replay does NOT allocate per-frame TGL" invariant. Validate via `[TGL_POOL v1] event=mission_unload` smoke gate. |
| Pre-bake `firstColorOffset` becomes stale across mid-mission state changes | MEDIUM | `firstColorOffset` indexes a per-frame color SSBO populated from `TransformMultiShape`'s output. If Track B bakes it once at register, but the actor's `listOfColors` is later remapped (paint scheme change, damage decal), the static replay reads stale offsets. Need to either keep `firstColorOffset` per-frame OR explicitly invalidate-on-state-change. |
| `cachedFrame_` invariant breakage on the wider population | HIGH | The fix from black-tree-bug (gos_static_prop_registry.cpp:144) skips draws when `cachedFrame_ != currentFrame`. For Track B's wider population, the invariant must hold for every newly-registered actor: every frame in which they should render via the static path, their `update()` must have run. If wolfman zoom (where MC2's CPU cull false-negates ~87%) skips update for a registered actor whose render still wants to fire, the actor silently drops out. Operator escape: `MC2_FORCE_DYNAMIC_BUILDINGS=1` etc. |
| Generic / GV staticReg adds `StaticRegistration` field to classes whose static-state lifecycle hasn't been audited | MEDIUM | `tg_shape_static_state_lifecycle_trap.md` (memory) records that `TG_Shape::init()` blowing class-static fields without coordinated reset cascades. Adding `StaticRegistration` to GenericAppearance / GVAppearance must include parallel reset in their `init()` overrides — see bdactor.cpp:570 (Bldg) and :3541 (Tree) for the existing pattern. |
| Artillery/vTol gameplay-time spawns miss the mission-load walk | LOW | Already handled correctly today by first-render registration. Track B doesn't need to displace this; it should layer mission-load registration on top of (not replace) first-render. |
| Cull cascade: forcing mission-load registration ≠ forcing per-frame TransformMultiShape | LOW (if discipline holds) | The cull-lessons doc warns about pool exhaustion / setExists(false) cascades. Track B widening of the registry alone doesn't trip these — registration is a one-time CPU vector insert. The cascade fires only if Track B also widens cull bypass to force update on out-of-block actors. Don't do that. |

---

## 11. Open questions for brainstorm

1. **Mission-load registration vs. first-render registration — keep both
   or replace?** The simple form of Track B replaces first-render
   registration with mission-load. But that strands artillery/vTol/etc.
   that spawn at gameplay time. Likely answer: keep first-render for
   gameplay-spawned, add mission-load for `addObject`-spawned.
2. **What's the failure mode if mission-load registration races
   `TransformMultiShape`?** Specifically, on the very first frame after
   mission start, `cachedFrame_ == 0xFFFFFFFFu` for every newly
   mission-load-registered actor — flush() silently drops them. Does
   that produce a 1-frame visible pop? Tier1 smoke would not catch it
   (passive 30s mc2_01 likely doesn't pan during frame 0).
3. **Should GenericAppearance be in Track B at all?** If the only
   Generics in the wild are skybox + compass (both allowlisted
   non-world), the cost-benefit of widening to Generic is net negative
   (more code surface, no actors gained). Verify with a per-actor-class
   trace before committing the surface.
4. **Pre-cull feasibility vs. value.** Section 8 concludes ~0% benefit
   on stock content. Defer or descope unless evidence of map-edge
   spawns surfaces.
5. **Pool-headroom gate at default-on flip.** Track B's exit criterion
   should include `[TGL_POOL v1] event=mission_unload` peaks not
   regressing more than (say) 10% on tier1. Set the gate explicitly so
   the flip doesn't depend on subjective "pool seems fine."
6. **`firstColorOffset` and damage decals / paint schemes.** Today the
   color offset is per-frame. Mission-load-baking it as part of the
   static recipe assumes the actor's color stream layout never changes
   mid-mission. Building destruction (mesh swap) already invalidates
   via the shape-pointer check; verify damage decals / paint changes
   travel the same path.
7. **3-week estimate validation.** Most of the substrate exists
   (registry, cachedFrame_ pattern, isStaticEligible, frame-stamp
   gate). The work is: (a) extract a "build-batch-without-submit"
   factoring, (b) add a mission-load walk in objmgr.cpp:loadTerrainObjects
   tail (~200 LOC), (c) audit + extend Generic, (d) extensive smoke +
   wolfman validation. (a) is the only design uncertainty; (b)–(d) are
   mechanical. 3 weeks is plausible if (a) lands cleanly in week 1.

---

## 12. References

**Source files (verified at write-time):**

- `GameOS/gameos/gos_static_prop_registry.h:1-49` — registry API surface
- `GameOS/gameos/gos_static_prop_registry.cpp:1-189` — registry implementation including frame-stamp fix at :144
- `GameOS/gameos/gos_static_prop_batcher.h:13-35` — `GpuStaticPropInstance` layout
- `GameOS/gameos/gos_static_prop_batcher.h:79-84` — `GpuStaticPropPopulation` enum
- `GameOS/gameos/gos_static_prop_batcher.h:96-221` — batcher API (registerType, finalizeGeometry, submitMultiShape, getLastBuiltBatch, submitCachedInstance)
- `code/mission.cpp:1645-1646` — onMapLoad / registry::init call sites
- `code/mission.cpp:2809` — `loadTerrainObjects` call from `Mission::init`
- `code/mission.cpp:3044` — `finalizeGeometry()` site (between actor spawn and mission start)
- `code/mission.cpp:3127-3154` — TGL pool + heap sizes (128 MB heap, 500K vertex/color/shadow, 200K face/triangle)
- `code/mission.cpp:3173-3174` — destroy / registry::destroy call sites
- `code/objmgr.cpp:937-973` — `loadTerrainObjects` enumeration
- `code/objmgr.cpp:1132-1244` — `addObject` switch (TERRAINOBJECT/TREE/TURRET/GATE/BUILDING/TREEBUILDING)
- `code/artlry.cpp:1577-1670` — Artillery spawn paths (gameplay-time `BldgAppearance` + Shilone GVAppearance)
- `code/gamecam.cpp:201` — `GpuStaticPropRegistry::frameBegin()` call
- `code/gamecam.cpp:510, :654` — skybox + compass actor construction (allowlist sources)
- `code/gamecam.cpp:577-579` — camera altitude read
- `code/mechcmd2.cpp:1263-1276` — `Camera::AltitudeMinimum/MaximumLo/MaximumHi` config load
- `mclib/bdactor.h:209-214` — Bldg `StaticRegistration` struct
- `mclib/bdactor.h:288-294` — Bldg static virtual overrides + `isStaticEligible`
- `mclib/bdactor.h:503-508` — Tree `StaticRegistration` struct
- `mclib/bdactor.cpp:1605-1670` — `BldgAppearance::render` static-path / first-render registration
- `mclib/bdactor.cpp:2638-2652` — `BldgAppearance::isStaticEligible`
- `mclib/bdactor.cpp:2654-2660` — `BldgAppearance::IsStaticNow`
- `mclib/bdactor.cpp:2676-2681` — `BldgAppearance::invalidateStaticRegistration`
- `mclib/bdactor.cpp:4237-4296` — `TreeAppearance::render` static-path / first-render registration
- `mclib/bdactor.cpp:4733-4758` — Tree IsStaticNow / invalidateStaticRegistration
- `mclib/appear.h:138-151` — `Appearance::IsStaticNow / touch / invalidateStaticRegistration` virtual defaults
- `mclib/msl.h:275-337` — `cachedGpuLightIndex_` / `cachedFrame_` fields + accessors
- `mclib/msl.cpp:1786-1811` — `CacheGpuLightData` / `ResubmitCachedGpuLightData` cachedFrame_ stamping
- `mclib/txmmgr.cpp:959-1045` — `GatherLightsParameters` (consumes global `s_listOfLights`)
- `mclib/txmmgr.cpp:1497` — `GpuStaticPropRegistry::flush()` call before batcher flush
- `mclib/terrain.cpp:92-102, :286-289` — terrain world-units constants

**Tracking docs:**

- `docs/superpowers/mc3-rendering-modernization-roadmap.md:101-126` — Track B definition
- `docs/superpowers/specs/2026-04-19-gpu-static-prop-renderer-design.md` — original GPU static-prop design doc
- `docs/superpowers/specs/2026-05-05-slice3c-static-prop-registry-design.md` — slice 3.C registry design
- `docs/superpowers/plans/2026-05-05-slice3c-static-prop-registry.md` — slice 3.C plan (registration block, frame-stamp wiring)
- `docs/gpu-static-prop-cull-lessons.md` — five ways the cull gates are load-bearing
- `data/objbatcher_late_register_allowlist.txt:22, :28` — `Cylinder01`, `compassplane` allowlist

**Memory files (load-bearing, point-in-time as of dates inline):**

- `memory/cull_gates_are_load_bearing.md` (11 days old) — five-way cascade hazard
- `memory/tgl_pool_exhaustion_is_silent.md` (11 days old) — pool budget rationale
- `memory/black_tree_bug_investigation_state.md` — frame-stamp pattern that resolved the 3.C/3.D bug
- `memory/skybox_actor_vestigial_post_terrain_gpu.md` (3 days old) — Cylinder01 allowlist context
- `memory/tg_shape_static_state_lifecycle_trap.md` — class-static reset hygiene rule
- `memory/stock_install_must_remain_playable.md` — architectural rule to honor

**Smoke artifacts inspected:**

- `tests/smoke/artifacts/2026-05-05T15-24-25/mc2_10.log`, `mc2_17.log` — TGL peak measurements
- Commit `06ac847` (2026-05-02) — late-register inventory across 24/24 missions

---

## Spike outcome (Task 1 — 2026-05-06)

### Chosen approach
**Hybrid A+B: extract `buildRecipeFromShape` (A) AND drive `TransformMultiShape_PositionsOnly` synchronously at mission-load (B).** Each candidate alone is insufficient; the combination is clean.

### HC-2: firstColorOffset resolution
**(b) Patch-per-frame at flush — and the patch is already implemented.**

`GpuStaticPropBatcher::submitCachedInstance` already overwrites `firstColorOffset` on every call:

```cpp
// gos_static_prop_batcher.cpp:1832-1833
GpuStaticPropInstance updated = inst;
updated.firstColorOffset = static_cast<uint32_t>(bucket.colors.size());
```

This is the same per-frame patch pattern the registry uses for `lightDataIndex` (`gos_static_prop_registry.cpp:276`). The recipe's stored `firstColorOffset` is never read by GPU; it's a placeholder. Task 2's `buildRecipeFromShape` can write `firstColorOffset = 0` and it will be overwritten before any draw. No architectural change required (rules out (c)); bake-at-register (a) would be safe but redundant since flush() already patches.

Additionally, `submitCachedInstance:1852` zero-fills the bucket's color block (`bucket.colors.insert(bucket.colors.end(), type.vertexCount, 0u)`) — the static-replay path never reads per-instance vertex colors. This is the keystone fact making A+B viable: the recipe does NOT need to capture `shape->listOfVertices[].argb`, so the listOfVertices==NULL guard at `gos_static_prop_batcher.cpp:1166` is irrelevant for the registration phase.

### Candidate A assessment
**Viable as a code factoring, NOT viable standalone.**

Recipe-construction in `submit()` (`gos_static_prop_batcher.cpp:797-868`) reads only:
- `shape->myType` (typeID lookup, line 807-820) — set at TG_Shape::init, valid at mission-load
- `shapeToWorld` (input arg copied to `inst.modelMatrix`, line 834) — **the load-bearing ambiguity**
- `child->aRGBHighlight`, `child->fogRGB`, `child->lightsOut`, `child->isWindow`, `child->isSpotlight` — TG_Shape instance fields, valid at init
- `lightDataIndex` (input arg) — placeholder, patched per-frame at flush

Per-frame state writes (`bucket.instances.push_back`, `s_lastBuiltBatch.push_back`, color-block append at 946-956) are confined to a distinct second phase after `inst{}` is fully built. The block at lines 831-849 IS cleanly separable as a pure function `buildRecipeFromShape(shape, shapeToWorld, highlight, fog, flags) -> GpuStaticPropInstance`.

**The fatal problem with A alone:** `multi->listOfShapes[i].shapeToWorld` is initialized to `Stuff::LinearMatrix4D::Identity` at multishape construction (`msl.cpp:155`) and only gets the actor's world transform when `TransformMultiShape` runs the hierarchy walk (`msl.cpp:1474, 1486, 1538, 1562`). At mission-load registration time — before any `update()` has run — every leaf's `rec.shapeToWorld` is still Identity. A recipe baked from Identity would render every static prop at world origin. `setTerrainPosition` at `objmgr.cpp:1306` populates the actor's `position`/`rotation` fields, not the multishape's per-leaf `shapeToWorld`.

### Candidate B assessment
**Viable. Cost is acceptable.**

`TG_MultiShape::TransformMultiShape_PositionsOnly` (`msl.cpp:1759-1765`) wraps `TransformMultiShape` with an internal `s_multiShapePositionsOnly` flag (`msl.cpp:1358, 1724`) that swaps `MultiTransformShape` for `MultiTransformShape_PositionsOnly` per leaf. This:
- Runs the full hierarchy animation walk
- Computes `shapeToWorld` for every leaf (the data we need)
- Does NOT touch `listOfVertices` / `listOfColors` / per-vertex lighting

Globals it requires:
- `TG_Shape::s_cameraOrigin` (`msl.cpp:1386`) — set by camera init before mission start
- `frameNum`, `turn` — set by mission init before `loadTerrainObjects`
- `eye`/worldLights are NOT touched by positions-only path

`isStaticEligible` disqualifiers (`bdactor.cpp:2638-2652`) at mission-load:
- `appearType->spinMe`, `bldgTypeHasAnimations(appearType)` — type-level, correctly stable
- `drawFlash`, `destructFX`, `activity`, `activity1`, `bdAnimationState != -1` — instance-level, all default to false/-1 at construction; verified per recon Section 3 ("for mission-load actors, none of these are active yet")

`primeAppearanceForMissionLoad` (`code/terrobj.cpp:554-616`) already runs at `mission.cpp:2818` between `loadTerrainObjects` (2817) and `finalizeGeometry` (3052), already calls `setObjectParameters(position, rotation, ...)` (line 606) and `recalcBounds()` (line 613). Track B's mission-load registration walk slots in directly after primeAppearance with all required state populated.

**Cost estimate:** ~3000 actors × (~3-5 leaves × matrix-multiply hierarchy walk) ≈ 15K matrix ops. Well under 1ms on the ~3GHz target CPU. One-shot at mission load — invisible against the seconds-long mission load already in flight.

### Decision rationale
Candidate A alone fails because Identity matrices break the modelMatrix bake. Candidate B alone (run synthetic `update()` then call existing `submitMultiShape`) fails because `submitMultiShape`'s second pass (`gos_static_prop_batcher.cpp:1166`) skips children with `listOfVertices == NULL` — and `TransformMultiShape_PositionsOnly` deliberately does NOT populate listOfVertices (that's its whole point). Calling FULL `TransformMultiShape` to populate vertices works but pulls in worldLights / lighting setup, doubling cost and dragging in cross-actor `worldLights[0]->aRGB` ordering hazards (the bug `Stage 2.D.2 fix` exists to address — `msl.cpp:1768-1805`).

The hybrid:
1. Mission-load walk calls `TransformMultiShape_PositionsOnly` per actor (drives shapeToWorld; no vertex/color work).
2. Walk calls a NEW factor `buildRecipeFromShape(shape, rec.shapeToWorld, highlight, fog, flags)` per leaf, accumulating into a local batch.
3. Walk calls `GpuStaticPropRegistry::registerRecipe(multi, batch)`.

This keeps the recipe-construction code path identical to what `submit()` does today (preserving any subtle invariants), avoids the listOfVertices NULL trap (because the new factor doesn't touch the bucket color stream), and pays a single `TransformMultiShape_PositionsOnly` cost per actor at load.

The first-render registration path in `BldgAppearance::render` (`bdactor.cpp:1653-1663`) and `TreeAppearance::render` (`bdactor.cpp:4285-4293`) STAYS — it covers gameplay-spawned actors (artillery, vTol) that miss the mission-load walk per recon Section 4.

### Impact on downstream tasks

- **Task 2 (extract `buildRecipeFromShape`):** factor must be a pure function — no bucket writes, no parity snapshot, no `s_lastBuiltBatch` push, no color-block append. Inputs: `TG_Shape*`, `const Stuff::Matrix4D& shapeToWorld`, highlight/fog/flags. Output: `GpuStaticPropInstance` with `firstColorOffset = 0` and `lightDataIndex = 0` (both patched per-frame). It must continue to share the typeID lookup with `submit()` (refactor: extract a private `lookupTypeID` helper, call from both `submit()` and `buildRecipeFromShape`). HC-2 already resolves the `firstColorOffset` placeholder concern.
- **Task 3 (`cachedFrame_` structural fix):** unchanged. The existing flush() gate at `gos_static_prop_registry.cpp:239` (`rng.multi->getCachedFrame() != currentFrame`) already enforces the cull-aware skip. Mission-load registered actors will silently no-op draw on frame 0 (cachedFrame_==UINT32_MAX sentinel until first update) — Risk Register row "Mission-load registration runs before TransformMultiShape; lightData_ not populated yet" already flags this; visible as a 1-frame pop at most.
- **Task 5 (mission-load registration walk):** site is `code/objmgr.cpp` after `primeTerrainObjectsForMissionLoad` (i.e., a new pass at `code/mission.cpp:2818-2820`), not at the end of `loadTerrainObjects`. Walk iterates `terrainObjects[]`, `buildings[]`, `turrets[]`, `gates[]` (NOT generics — recon Section 11 Q3). For each: check `IsStaticNow()` would be premature — instead call `isStaticEligible()` directly (the disqualifier subset that doesn't depend on registration state), then drive `TransformMultiShape_PositionsOnly`, then build batch via `buildRecipeFromShape`, then `registerRecipe`. Set `staticReg.recipeIndex = result; staticReg.shape = bldgShape; staticReg.registered = true`.
- **Task 6 (register-on-spawn API for late types):** unaffected — first-render path stays. The existing `submitMultiShape` + `getLastBuiltBatch` recipe is fine for late types because by the time `render()` runs, `update()` has already populated listOfVertices/listOfColors via TransformMultiShape.
- **No additional schema changes** — `GpuStaticPropInstance` layout, `RecipeRange` struct, `registerRecipe` signature all stay as-is.
