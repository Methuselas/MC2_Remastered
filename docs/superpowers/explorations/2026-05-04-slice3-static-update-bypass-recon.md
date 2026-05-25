# Slice 3 Recon — Static-object update bypass

**Date:** 2026-05-04
**Worktree HEAD:** `61f6a66` (post-Stage-2.E pause + GPU objects default-on)
**Scope:** read-only recon only; no code, no plan yet.

## Goal

Identify which terrain objects can safely skip per-frame `update()` /
`appearanceUpdate()` work without breaking animation, destruction,
culling, shadows, LOD, texture handles, or scripted-event firing.

## Tracy starting point

`GameLogic.Units.TerrainObjects` zone at `code/objmgr.cpp:1707`:
- 869 objects iterated per frame
- 1.55ms in `appearanceUpdate` (77% of zone, ×778 calls)
- 132µs in `recalcBounds` (×869)
- 53µs in `TerrainObject::update` (×869)
- 25µs in `MC_TextureNode::get_gosTextureHandle` (×1894 — 2.18 calls/object avg)
- 22µs in `appearanceSetup` (×869)
- Total zone: ~2.0ms

---

## Q1. Which populations are iterated every frame?

`code/objmgr.cpp:1700-1900` — three sub-populations under
`if (terrain && renderObjects)`:

1. **Special buildings** (`objmgr.cpp:1715-1732`): `specialBuildings[]` array, each calls `update()`. Returns 0 → `MC2_DESTROY` invoked at `:1725`. Counter: `specialBuildingsUpdated`.
2. **Gates** (`objmgr.cpp:1737-1754`): `gates[]` array. Per-class comment at `:1736`: *"MUST update every frame or they don't open!!"* Counter: `gatesUpdated`.
3. **Terrain objects** (`objmgr.cpp:1756-1783`): walks active blocks, calls `update()` on each contained object. Counter: `terrainObjectsUpdated`.

All three converge on `TerrainObject::update()` (`code/terrobj.cpp:523`)
or its building/gate overrides. The `appearanceUpdate()` call happens
at `terrobj.cpp:608` (Tracy zone `TerrainObject::update appearanceUpdate`).

Tracy ratios match: ×869 outer updates, ×778 appearance updates →
some objects' update() returned early before reaching appearanceUpdate
(probably because `inView == false` after recalcBounds).

---

## Q2. Side effects of `TerrainObject::update()`

`code/terrobj.cpp:523-640` body breakdown:

| Lines | Side effect | Load-bearing? |
|---|---|---|
| :526-559 | Just-created init: tangibility, passability | First-frame only |
| :563-568 | Power-supply linkage: `appearance->setLightsOut(true)` if parent destroyed | YES — gates damage state |
| :572-590 | Tree-falling animation: `fallRate`, `pitchAngle`, sound trigger | YES — only for falling trees |
| :593-596 | Appearance param sync: `setObjectParameters()` — transform state | YES — animated types only |
| :599-601 | `recalcBounds()` returns `inView` — **load-bearing cull gate** | YES — see `cull_gates_are_load_bearing.md` |
| :603-609 | Conditional `appearanceUpdate()` — only if `inView == true` | YES — animated types |
| :611-635 | GOSFX effect tick: `bldgDustPoofEffect->Execute()` etc. | YES — only when effect active |
| :639 | Returns 0 on destruction → `setExists(false)` upstream | YES — lifecycle |

**Critical:** `recalcBounds` cannot be skipped — it's the cull gate
referenced in `memory/cull_gates_are_load_bearing.md`. Any "skip update"
path must still call `recalcBounds` to keep `inView` / cull state fresh.

The skip target is **only** the `appearanceUpdate()` call at `:603-609`
(the 1.55ms hot path), not the full `update()` function.

---

## Q3. Safe candidates for `StaticNoUpdate`

**Provably static (no animation, no destruction, no scripted events):**

- **Trees** (`TERROBJ_TREE`, `terrobj.cpp:536-540`):
  - `TreeAppearance::update` (`bdactor.cpp:4295-4436`) has no `currentFrame` increment
  - No destruction path from update
  - Falling trees go through a SEPARATE state (`fallRate != 0` at `terrobj.cpp:572`) — gate static-skip on `fallRate == 0`
- **Walls** (`TERROBJ_WALL_*`, `terrobj.cpp:549-556`): passability locked at create, geometry static.
- **Fences/decorations** (`TERROBJ_NONE`, `terrobj.cpp:535-540`): same path as trees.
- **Bridges** (`TERROBJ_BRIDGE`, `terrobj.cpp:542-547`): passability locked at create.

**Conditional (need per-instance check):**

- **Civilian non-animated buildings** (`BldgAppearance` with `bdFrameRate == 0` AND `bdAnimData[i] == NULL` for all i): animation guard at `bdactor.cpp:2161-2210` shows the increment is gated by `animate && bdFrameRate != 0.0f`. If both conditions are false at type-load time (`bdactor.cpp:321-334`), the building is provably non-animated. Still must check destructibility (Q4 disqualifier).

---

## Q4. MUST remain Dynamic (disqualifiers)

Each disqualifier cited with the side effect that gates it:

1. **Gates** (`gate.cpp:225-368`): gesture state machine; `Gate::update` forces `setInView(true)` at `:351-353` before calling `appearance->update()`. Comment at `:348`: *"MUST update appearance every frame or animation goes HINKY!"* Calls `openGate()` unconditionally at `:326`.
2. **Popup turrets / animated buildings** (`bdactor.cpp:1982-2210`): `BldgAppearance::update` increments `currentFrame` when `animate && bdFrameRate != 0.0f`. Detector: `appearType->bdAnimData[i] != NULL` at type load. Skipping freezes turret animation. Per `memory/bldg_animation_lod_swap_unsafe.md`, animated buildings have known LOD-swap fragility — extra reason to leave update path alone.
3. **Destructible buildings** (`terrobj.cpp:658`, `bdactor.cpp:1992-2003`): weapon-node recycling timers decay per frame. Detector: `type->getDamageLevel() > 0.0`.
4. **Scripted / ABL-triggered objects**: cannot statically determine. ABL scripts expect per-frame lifecycle. Conservative: assume dynamic.
5. **Active GOSFX effects** (`terrobj.cpp:611-635`): `bldgDustPoofEffect`, etc. tick per frame; skipping freezes the effect mid-animation. Detector: any effect-pointer member non-null.
6. **Power-supply dependent** (`terrobj.cpp:563-568`): checks parent destruction status per frame. Detector: `powerSupply != 0xffffffff`.
7. **Falling trees** (`terrobj.cpp:572-590`): once knocked down, tree advances `fallRate`/`pitchAngle` per frame until rest. Detector: `fallRate != 0`.

---

## Q5. Counters + smoke/perf gates

**Env-gated instrumentation (matches `MC2_TGL_POOL_TRACE` pattern from worktree CLAUDE.md):**

```
MC2_STATIC_UPDATE_TRACE=1 → per-frame counters, summary every 600 frames:
  [STATIC_UPDATE v1] objects_seen=N updates_run=N updates_skipped=N
                     dyn_gates=N dyn_animated=N dyn_destructible=N
                     dyn_power=N dyn_effects=N dyn_falling=N
                     zone_time_us=N
```

**Tracy plots (always-on):**
- `TracyPlot("TerrainObjects dynamic updates", int64_t)`
- `TracyPlot("TerrainObjects static skipped", int64_t)`

**Smoke gate discipline:**
- Tier1 with `MC2_STATIC_UPDATE_SKIP=0` (baseline) vs `MC2_STATIC_UPDATE_SKIP=1` (feature)
- Required parity: destroys delta `+0`, frame count within 1%, visible-objects count within 1%
- Visual canaries (manual): trees-don't-disappear (mc2_01 forest), gates-still-open (any gate mission), no popup-turret-stuck-at-rest, no frozen GOSFX

**Regression signatures (what would prove a misclassification):**
- Object lifecycle: destroy count diverges → an object that should have died via update() didn't
- Animation: popup turret frozen at rest pose → animated building misclassified
- Gate: stuck closed → gesture machine not ticking
- Weapon hardpoint: unresponsive → destructible recycle timers stalled
- GOSFX: stuck mid-animation → effect-bearing object misclassified

---

## Q6. Where should the skip live?

**Recommendation: inside `appearance->update()` (per-class early-out), NOT at the objmgr loop.**

Rationale:
- **Architectural fit:** MC2 already short-circuits inside appearance classes (`BldgAppearance::update:2069` gates on `inView || g_useGpuStaticProps`). A static-skip is a natural extension.
- **Load-bearing cull preservation:** `cull_gates_are_load_bearing.md` warns that bypassing gates at loop level cascades into stale state. `TerrainObject::update` MUST still run because of `recalcBounds` (cull gate) and lifecycle return value. Only `appearanceUpdate()` is the skip target — and that's the call appearance owns.
- **Per-class criteria:** gates check gesture state, animated buildings check `bdFrameRate`, destructibles check damage level, falling trees check `fallRate`. Each class self-knows its dynamism predicate. Forcing objmgr to know all of them is leaky.
- **Existing precedent:** `TreeAppearance::update` already exists as a separate class. A virtual `bool IsStaticNow()` predicate fits the pattern.

Concrete shape:
```cpp
// at appearance class level (virtual)
virtual bool IsStaticNow() const { return false; }  // default: dynamic

// each class overrides:
TreeAppearance::IsStaticNow() — return owner->fallRate == 0 && !owner->isFalling
BldgAppearance::IsStaticNow() — return type.bdFrameRate == 0 && type.bdAnimData[0] == NULL && damage == 0 && powerSupply == 0xffffffff && !hasActiveEffect
GateAppearance::IsStaticNow() — return false (always)

// in TerrainObject::update at terrobj.cpp:603-609:
if (!appearance->IsStaticNow()) {
    appearance->update();  // existing path
} else {
    s_staticSkipCounter++;  // count for trace
}
```

`recalcBounds`, lifecycle return, GOSFX tick all stay outside the skip
gate — only the appearance update is bypassed.

---

## Bottom line

**Recommended slice plan (no code yet):**

1. **Stage 3.A — instrumentation only** (no behavior change). Add the counter framework, env-gated trace, Tracy plots. Land default-off.
2. **Stage 3.B — virtual `IsStaticNow()` predicate** with conservative overrides:
   - `TreeAppearance::IsStaticNow` — true when `fallRate == 0` and not falling.
   - All other classes — return false (default).
   - Gate the actual skip behind `MC2_STATIC_UPDATE_SKIP=1`. Default-off.
   - Smoke tier1 with feature on/off; verify counters; +0 destroys.
3. **Stage 3.C — expand allowlist** to walls, fences, bridges (`TERROBJ_WALL_*`, `TERROBJ_NONE`, `TERROBJ_BRIDGE`). Smoke again.
4. **Stage 3.D — non-animated civilian buildings** (`BldgAppearance::IsStaticNow` with all 5 disqualifier checks). Most subtle; needs careful per-instance verification + visual canaries.
5. **Stage 3.E — perf measurement + default-on flip**. Need ≥30% reduction in `GameLogic.Units.TerrainObjects` zone time on tier1 forest-heavy mission (mc2_01) to justify default-on.

**Conservative allowlist (Stage 3.B start):**
- Trees only — most populous on forest missions, no animation, no destruction, no lifecycle complexity.

**Disqualifier list (never classify static):**
Gates, popup turrets, animated structures, destructible buildings, weapon-node-bearing buildings, power-supply-dependent, GOSFX-active, falling trees, scripted objects.

**Counter design:** see Q5.

**Suggested next step:** brainstorm-format design doc for Slice 3 architecture — specifically the `IsStaticNow()` predicate hierarchy and the per-class disqualifier checks. Then plan, then code.

---

## Findings citations summary

| Claim | File:line |
|---|---|
| Object loop start | `code/objmgr.cpp:1707` |
| Special buildings update | `objmgr.cpp:1715-1732` |
| Gates "MUST update" comment | `objmgr.cpp:1736` |
| Terrain objects loop | `objmgr.cpp:1756-1783` |
| TerrainObject::update entry | `code/terrobj.cpp:523` |
| Tree fall animation | `terrobj.cpp:572-590` |
| recalcBounds cull gate | `terrobj.cpp:599-601` |
| Conditional appearanceUpdate call site | `terrobj.cpp:603-609` |
| GOSFX effect tick | `terrobj.cpp:611-635` |
| Power supply linkage | `terrobj.cpp:563-568` |
| Lifecycle destroy return | `terrobj.cpp:639` + `objmgr.cpp:1775` |
| Gate update body | `code/gate.cpp:225-368` |
| Gate "MUST update" inline comment | `gate.cpp:348` |
| BldgAppearance::update body | `mclib/bdactor.cpp:1982-2210` |
| Animation gate (frameRate) | `bdactor.cpp:2161-2210` |
| bdAnimData type-load | `bdactor.cpp:321-334` |
| BldgAppearance::update inView gate | `bdactor.cpp:2069` |
| TreeAppearance::update body | `bdactor.cpp:4295-4436` |

Citations punted to implementation (couldn't fully verify in recon):
- Exact `IsStaticNow()` virtual override sites for each appearance class — implementer adds at Stage 3.B.
- The exact set of GOSFX effect pointer fields that mark "active effect" — implementer enumerates at Stage 3.D.
- Whether ABL-scripted objects have a flag at type-load time, or if "scripted" can only be detected at runtime — needs Stage 3 design-doc resolution.

---

## Addendum — 2026-05-05: post-slice-2 context update

**Date:** 2026-05-05  
**Context:** slice 2 axis-swap fix landed this session (`static_prop.vert` swap correction). GPU objects path now renders buildings and trees correctly. Re-measurement via Tracy requires game launch (not possible from planning context); this addendum provides code-analysis grounding to replace the stale 2.0ms baseline.

### Stale citation correction

The recon cited `TerrainObject::update` entry at `terrobj.cpp:523` and the `appearanceUpdate` call site at `terrobj.cpp:603-609`. As of 2026-05-05 code, the actual locations are:
- Function entry: `terrobj.cpp:589` (verified via grep)
- `appearanceUpdate` ZoneScopedN + call site: `terrobj.cpp:669-677` (verified via read)

The shift is caused by the JUSTCREATED block restructure in commits since the recon was written.

### Does GPU offload make slice 3 unnecessary for trees?

The prompt's concern: with `BldgAppearance::update` short-circuiting at `bdactor.cpp:2069` (`inView || g_useGpuStaticProps`), maybe the 1.55ms hot path is already mostly gone.

Code-grounded finding: `TreeAppearance::update` at `bdactor.cpp:4295-4436` has **no early-return short-circuit**. The structure is:

1. **Lines 4300-4375** (unconditional): rotation normalization, fog factor compute, `lightRGB` setup via `land->getTerrainLight`. Runs for EVERY `inView` tree.
2. **Lines 4382-4430** (`if (inView || g_useGpuStaticProps)`): `TransformMultiShape_PositionsOnly` + `CacheGpuLightData` for GPU-eligible trees, or full `TransformMultiShape` otherwise.

Since `appearance->update()` in terrobj.cpp is only called when `inView == true`, the GPU-mode expansion (`|| g_useGpuStaticProps`) doesn't affect the invocation count from the objmgr loop. The per-call work is similar or slightly heavier than pre-slice-2 (fog/light setup unchanged; GPU-eligible trees now run `TransformMultiShape_PositionsOnly` + `CacheGpuLightData` instead of the full `TransformMultiShape`).

**Conclusion:** `TreeAppearance::update()` is not short-circuited by GPU offload. The tree population is still the dominant consumer in `GameLogic.Units.TerrainObjects`, and slice 3 remains worth doing. Re-measure Tracy to confirm absolute zone time and tree fraction before Stage 3.E perf-gate decision.

### GPU-tree skip safety argument

The v2 plan's `IsStaticNow()` skip causes `TreeAppearance::update()` to NOT run for static in-view trees. For GPU-eligible trees, this skips `CacheGpuLightData` and `TransformMultiShape_PositionsOnly`. Safety argument:

- `TransformMultiShape_PositionsOnly` computes world-space vertex positions from `xlatPosition` + `rot`. For a truly static tree, position and rotation don't change frame-to-frame → data is invariant → skipping is safe.
- `CacheGpuLightData` caches per-tree light colors. Light intensity from `land->getTerrainLight()` is terrain-geometry-derived and doesn't change at runtime (MC2 has no dynamic time-of-day). → data is invariant → skipping is safe.
- Visual canary in Stage 3.B (Task 7 Step 7): manual check that tree lighting unchanged after enabling skip. This is the empirical backstop.

### LOD-handoff coordination reminder

The in-flight LOD-handoff fix has three paths. Path 1 reintroduces `TransformMultiShape` for offloaded actors (texture-handle side effect). If Path 1 ships first, it puts per-frame CPU work into exactly the block slice 3 would skip. **Preferred ordering:** ship slice 3's Stage 3.B before the LOD-handoff fix, or ensure LOD-handoff uses Path 2 or Path 3. If Path 1 ships first, add `!g_useGpuObjects` to the slice 3 call-site composition as a temporary non-skippable guard. See `memory/mc2_texture_handle_is_live.md`.
