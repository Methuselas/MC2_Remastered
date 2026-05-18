# Object/Prop Iteration GPU Port - Open-Questions Resolution (recon + design-resolution)

Date: 2026-05-18. Branch: claude/gpu-driven-rendering. HEAD verified `4d9529a`
("docs(static-decor): de-stale snapshot + resolve gate + implementation handoff").
Role: MC2 CPU-to-GPU offload methodology advisor. RECON + DESIGN-RESOLUTION only -
no code, no plan. Every file:line below grep-verified at HEAD `4d9529a`; symbols
stable, lines drift - re-grep at plan-write.

This report resolves the 4 open questions in
`docs/superpowers/specs/2026-05-18-object-prop-iteration-gpu-port-stage0.md`
("Open questions for the plan") and reconciles the contract with the
pre-existing `g_useGpuStaticProps` machinery the contract does not mention.

---

## Reconciliation with existing `g_useGpuStaticProps` (read this first - it gates slice coherence)

This is the single most important finding. The contract proposes a render-consumer
flip but does not mention that a **parallel render-consumer bypass already exists
and is already woven through every terrain-static render gate**. Resolving this
wrong produces the exact `feedback_offload_must_be_substitutive_not_additive.md`
failure (a second GPU path added without retiring the CPU twin).

**What `g_useGpuStaticProps` is** (grep-verified):
- Declared `GameOS/gameos/gos_static_prop_killswitch.h:8`; defined
  `GameOS/gameos/gos_static_prop_batcher.cpp:27` (`bool g_useGpuStaticProps = false;`).
- Runtime toggle only (RAlt+0, `gameosmain.cpp:344`); **default false**; NOT
  env-gated; mutually exclusive with `g_useGpuObjects` (killswitch.h:11-13).
- It is the **GPU static-prop batcher killswitch**, a different machine from the
  contract's `gpu_cull::readback_*` path.

**What it gates** (every terrain-static render/cull gate, grep-verified):
- `code/terrobj.cpp:796` - `if (appearance->canBeSeen() || g_useGpuStaticProps)`
  - the exact render gate the contract's Q1 targets.
- `code/bldng.cpp:1081`, `code/gate.cpp:599`, `code/artlry.cpp:1334` - sibling
  render gates (Building/Gate/Artillery, same `canBeSeen() || g_useGpuStaticProps`
  / `inView || g_useGpuStaticProps` pattern).
- `mclib/bdactor.cpp:1654` (`BldgAppearance::render`), `:2343`/`:2489`
  (Bldg submit paths), `:4582`/`:4944` (`TreeAppearance::render` / submit),
  `mclib/genactor.cpp:779`/`:1198` - the appearance-side render bodies all
  bypass `inView` when `g_useGpuStaticProps`.
- `code/terrobj.cpp:723` - `const bool gpuPath = g_useGpuObjects || g_useGpuStaticProps;`
  drives the Stage-3.B static-**update** skip (terrobj.cpp:738), i.e. it already
  participates in the update-loop the `GameLogic.Units.TerrainObjects` zone wraps.

**Critical structural fact:** `g_useGpuStaticProps` gates the **render submission
path** (does the actor's geometry get drawn by the GPU batcher instead of the
legacy TGL path). It does **NOT** retire the CPU `recalcBounds()` projection. With
`g_useGpuStaticProps` ON today, `recalcBounds()` still runs in full per object per
active block (terrobj.cpp:694, inside the `GameLogic.Units.TerrainObjects` zone) -
the comment at `bdactor.cpp:1650-1653` confirms this is deliberate: the GPU path
"bypasses inView here" at *render* time but the angular-cull `recalcBounds` still
executes (its `~87% false-negative rate` is the stated reason render bypasses its
result, not a claim that recalcBounds stopped running).

**Ruling: the contract's readback flip is COMPLEMENTARY to `g_useGpuStaticProps`,
NOT redundant and NOT in conflict - but ONLY IF the slice is scoped as a
recalcBounds-body DELETE, not a second render-gate OR-term.** Concretely:

- `g_useGpuStaticProps` answers "who *draws* the prop" (GPU batcher vs legacy TGL).
  It is a render-submission killswitch. It already makes the render path not
  depend on `inView`.
- The contract's slice must answer "does the CPU still *compute* `recalcBounds`'s
  projection at all". That cost (~1.43ms `GameLogic.Units.TerrainObjects`) is
  **unaffected by `g_useGpuStaticProps`** - it is paid regardless of which render
  path is active, because the projection runs in the update loop
  (objmgr.cpp:2051 -> terrobj.cpp:694), upstream of any render gate.

- **Therefore the deliverable is the DELETE of the projection body** (the
  substitutive contract's first bullet), with the render/pick consumers of its
  byproducts repointed. Adding `|| readback_isActorVisibleLagged(...)` as a third
  term next to `|| g_useGpuStaticProps` at terrobj.cpp:796 would be pure additive
  failure: it changes which boolean the render gate reads while the CPU projection
  it was meant to retire keeps running. The zone does not move; the slice fails
  the substitutive proof gate #1 (`TerrainObjects -> ~0`).

- **Interaction hazard for the plan/review:** because `g_useGpuStaticProps`
  already makes *render* `inView`-independent, the ONLY live consumers that still
  need `recalcBounds`'s outputs after the projection delete are (a) the
  **lifecycle update gate** (terrobj.cpp:697, consumes the returned `inView`) and
  (b) the **mouse-pick path** (`canBeSeen()`/`windowsVisible`/`upperLeft`/
  `lowerRight`). Render is already covered by the existing killswitch + the
  readback consumer the contract names. This narrows CRIT-1 to pick + lifecycle,
  and means the render repoint at terrobj.cpp:796 is largely a **no-op cleanup
  once the projection body is deleted** (see Q1).

- **Coherence verdict:** the slice is coherent IF and only if it is framed as
  "delete the recalcBounds projection body for Bldg/Tree; the render consumer is
  already covered by `g_useGpuStaticProps` + the contract's readback gate; the
  real work is re-homing the pick path (CRIT-1) and proving the lifecycle gate is
  undisturbed (Q3)." Framed as "add a readback term to the render gate" it is
  incoherent (additive). The contract's wording ("REPLACE the render-consumer")
  is correct in intent; the plan must make the *projection-body delete* the
  load-bearing change and treat the render-gate edit as consequential, not
  primary.

---

## Q1 - Render repoint site + exact delete/keep line list

**RESOLUTION: terrain-static-specific gate at `code/terrobj.cpp:796` (render) and
`:866` (renderShadows). Do NOT touch `appear.h:176` `canBeSeen()`.** The delete is
the `recalcBounds()` *projection body* in the two `bdactor.cpp` overrides; the
render-gate edits are minor and consequential.

**Grep-verified sites:**

- `Appearance::canBeSeen()` - `mclib/appear.h:176-179`: `return(inView);` a bare
  member read shared by ALL appearance types (mech/GV/turret/effect). Editing it
  has the broad blast radius the contract warns of (`mech3d.cpp:2454`,
  `gvactor.cpp:2120` already consume it through their own readback wiring). **Do
  not touch.**
- `BldgAppearance::recalcBounds` - `mclib/bdactor.cpp:1152-1588` (return at
  `:1587` `return(inView);`).
- `TreeAppearance::recalcBounds` - `mclib/bdactor.cpp:4278`..(structure mirrors
  Bldg; `inView` final set at `:4453`, on-screen-rect refinements follow).
- Render consumer (terrain statics): `code/terrobj.cpp:796`
  `if (appearance->canBeSeen() || g_useGpuStaticProps)`.
- Shadow consumer: `code/terrobj.cpp:866` `if (appearance->canBeSeen())` (note:
  the shadow gate does **NOT** currently carry `|| g_useGpuStaticProps` - it is
  pure `canBeSeen()`; the plan must account for this asymmetry, see Residual
  Risks).
- `projectForScreenXY` call sites inside the bodies:
  - Bldg position projection: `bdactor.cpp:1207`
    (`[PROJECTZ:ScreenXYOracle id=bdactor_screen_pos_a]`).
  - Bldg selection-box 8-corner projection loop: `bdactor.cpp:1349`
    (`[PROJECTZ:ScreenXYOracle id=bdactor_box_rect_a]`).
  - Tree position projection: `bdactor.cpp:4332`
    (`[PROJECTZ:ScreenXYOracle id=bdactor_screen_pos_b]`).
  - Tree selection-box loop: `bdactor.cpp:4421`
    (`[PROJECTZ:ScreenXYOracle id=bdactor_box_rect_b]`).

**What is DELETED (the projection body, Bldg `bdactor.cpp` - mirror for Tree):**
- The post-angular-clip `if (inView)` projection + fog + on-screen-rect +
  selection-box blocks: `bdactor.cpp:1202-1583` for Bldg
  (`eye->projectForScreenXY(position,screenPos)` at :1207; the `upperLeft`/
  `lowerRight` screen-rect math :1246-1257 and :1372-1375; the 8-corner
  `boxCoords`/`bcsp` projection loop :1264-1370; the fog/distance `inView`
  refinement :1209-1232). For Tree the analogous block is the post-clip
  `if (inView)` region from `bdactor.cpp:4327` through the on-screen-rect /
  box-projection (mirrors Bldg; re-grep exact tail at plan-write).
- **Important nuance the plan must encode:** the projection block also performs
  LOD selection + per-LOD texture (re)load as a *side effect*
  (`bdactor.cpp:1383-1583` Bldg, `:4455`+ Tree). That side effect is currently
  pinned to LOD-0 by the 2026-05-12 TEMP workaround (`bdactor.cpp:1389-1415`,
  `selectLOD = 0; (void)useHighObjectDetail;`) so LOD selection is effectively a
  constant today, but the **texture-handle (re)load on first valid frame still
  runs inside this block**. Deleting the block wholesale removes that texture
  load. The plan MUST determine whether the texture handles are otherwise
  initialized (type-init path) or whether this is the only loader; if the only
  loader, the texture load must be re-homed (a second, smaller CRIT). This is a
  delete-blast-radius item the contract did not surface - flag for adversarial
  review.

**What is KEPT:**
- `bdactor.cpp:1155-1198` (Bldg) / `:4281-4322` (Tree): the matrix-free angular
  sphere-clip that sets a *coarse* `inView` BEFORE any projection. This is the
  separable part (see Q3). It writes `inView` and reads only camera frame +
  `GetExtentRadius()` - no `projectForScreenXY`.
- `Appearance::screenPos`/`upperLeft`/`lowerRight` *members* stay declared
  (`appear.h:64,72-73`); they simply stop being written by the deleted body.
  Their consumers are re-homed per Q2.
- `code/terrobj.cpp:796`/`:866` gates stay; once `inView` is coarse-angular-only
  (Q3) the render gate's behavior is governed by the existing
  `g_useGpuStaticProps` + the contract's readback gate. The minimal edit is to
  replace the `canBeSeen()` term with the contract's
  `gpu_cull::readback_isActorVisibleLagged(obj->getHandle())` (conservative-OR +
  dilation is applied transparently inside the snapshot - see Q-supporting note),
  fail-open when `!readback_isEnabled()`.

**Rationale for the narrow site:** `canBeSeen()` is type-agnostic and movers
already route through a *different* readback wiring (`mech3d.cpp:2452-2455`,
`gvactor.cpp:2118-2121` - `gpuVisGate ? (readback... || g_useGpuStaticProps) :
(inView || g_useGpuStaticProps)`). Editing `canBeSeen()` itself would double-gate
movers and collide with their in-flight slice. The terrobj.cpp site is the exact
terrain-static boundary the contract scopes.

**Residual risk for review:** the shadow gate (terrobj.cpp:866) is `canBeSeen()`
only (no `g_useGpuStaticProps` OR-term) - it currently *does* depend on the
projection-refined `inView`. If the projection delete makes `inView` coarse-only
(Q3), shadows for terrain statics widen to the coarse angular set unless the
shadow gate is also repointed to the readback. The plan must repoint :866 in
lockstep with :796 or accept (and prove acceptable) coarser shadow inclusion.
Over-inclusion is correctness-safe per the contract; a *dropped* shadow is not -
coarse angular is a superset of projected, so this is safe-direction, but state it.

---

## Q2 / CRIT-1 - What feeds `screenPos`/pick after the projection delete

**RESOLUTION: lazy per-candidate projection at the pick call site
(`GameObjectManager::findObjectByMouse`, `code/objmgr.cpp:2459`), NOT a defer to
the queued `gpu_mech_aware_mouse_pick` AABB precursor.** This is the VPL-Step-6
"re-home the survivor" analogue.

**The consumer chain (opposite-direction grep - grepped the pick path, not
`screenPos`):**

- Terrain-static pick entry: `objmgr.cpp:2685`
  `findTerrainObjectByMouse(mouseX,mouseY,true)` ->
  `objmgr.cpp:2625 findTerrainObjectByMouse` iterates `objBlockInfo[].active`
  blocks -> `objmgr.cpp:2641` `findObjectByMouse(&objList[objIndex],...)`.
- `findObjectByMouse` (`objmgr.cpp:2459-2519`) hard-depends on projection-body
  outputs:
  - `:2475` `if (objAppearance && objAppearance->canBeSeen())` - reads `inView`.
  - `:2477` `if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD))` -
    `windowsVisible` is set only inside the `if (inView)` branch at
    `terrobj.cpp:699` (the update-loop path) and `terrobj.cpp:502`
    (`TerrainObject::isVisible()`, which has **no callers in `code/*.cpp`** -
    grep `isVisible()` returned no matches; treat as the dead second caller, not
    a live freshness path).
  - `:2480-2483` reads `objAppearance->upperLeft.{x,y}` / `lowerRight.{x,y}` -
    written ONLY by the recalcBounds projection body (verified: `upperLeft.`/
    `lowerRight.` writes in `bdactor.cpp` are at `:1247-1257`,`:1372-1375` (Bldg)
    and `:4444-4447` (Tree) - all inside the to-be-deleted block; no other
    writer).
  - `:2499`/`:2508` `objAppearance->PerPolySelect(mouseX,mouseY)` ->
    `BldgAppearance::PerPolySelect` `bdactor.cpp:1055-1057`
    `return bldgShape->PerPolySelect(...)` - **geometry-space test, NOT
    screenPos-dependent**; survives the delete untouched.
  - Tree exclusion: `objmgr.cpp:2497` and `:2505` explicitly skip
    `obj->getObjectClass() != TREE` - **trees are never pick targets**
    (consistent with `mc2_selection_picking_model_water_terrain_never_picked.md`:
    only mechs + buildings/props are pickable). So CRIT-1 reduces to **buildings/
    props only**; the Tree projection body can be deleted with zero pick-path
    consumer (its only pick-relevant output, `upperLeft`/`lowerRight`, is never
    read for trees).

- Drag-select rect (`objmgr.cpp:2715-2718`, `moverInRect`) reads
  `getScreenPos()` but is **movers-only** (`getMover`, `Team::home`,
  `objmgr.cpp:2698-2699`) - out of this slice's terrain-static scope.
- `getScreenPos()` other consumers: `gvehicl.cpp:3949/3961`, `mech.cpp:6460-6480`
  (`drawSensorTextHelp`) - all mover/mech, not terrain statics. No terrain-static
  consumer of `getScreenPos()` exists outside the pick path. (Opposite-direction
  grep confirmed: the only terrain-static reader of the projection-body byproducts
  is `findObjectByMouse` via `upperLeft`/`lowerRight`/`canBeSeen`/`windowsVisible`.)

**Why lazy per-candidate projection, not the AABB-precursor defer:**

1. **Selection is not hot.** `findObjectByMouse` runs once per click / per
   drag-frame over only `objBlockInfo[].active` blocks (already cull-narrowed),
   for buildings/props only. Projecting one `position` + 8 box corners per
   candidate on click is negligible CPU - it does not reintroduce the per-frame
   ~1.43ms (that cost was per-object-per-active-block *every frame* in the update
   loop; pick is per-click). This is the precise VPL-Step-6 pattern: delete the
   per-frame producer, re-home the surviving rare consumer to compute on demand.
2. **The `gpu_mech_aware_mouse_pick` precursor is for movers and is queued/not
   shipped.** Coupling terrain-static pick to an unshipped mover-pick AABB
   precursor creates a cross-slice dependency the contract explicitly forbids
   (Scope: "Movers ... are a different consumer chain and a different slice").
   The handoff names CRIT-1 as the analogue of VPL Step 6 (re-home the survivor),
   which is the lazy-projection answer, not a defer.
3. **`PerPolySelect` already does the precise hit-test** in geometry space and is
   screenPos-independent - the screen-rect (`upperLeft`/`lowerRight`) is only a
   coarse pre-filter at `objmgr.cpp:2485-2488`. The lazy path only needs to
   reproduce that coarse rect for the click's candidates, then `PerPolySelect`
   does the rest unchanged.

**Concrete shape (for the plan to specify, not implemented here):** a
pick-time-only helper that, for a candidate building/prop appearance, runs the
same `eye->projectForScreenXY(position,...)` + 8-corner box projection that the
deleted block ran, populating a *local* rect (or transiently the members) so
`findObjectByMouse`'s `:2480-2488` rect test + `PerPolySelect` proceed unchanged.
**[AMENDED 2026-05-18 by design-gate review CRIT-A — split verdict; the two
guards at `objmgr.cpp:2475/2477` are NOT symmetric and the original "both must
be re-expressed" instruction below is PARTIALLY SUPERSEDED.]**

- `canBeSeen()` at `:2475`: re-expression against the readback-visible set is
  **CORRECT and remains in force** — gate pick on
  `readback_isActorVisibleLagged(handle)` (option (a), superset-safe, matches
  the render gate; consistent with proof gate #2). This part of the original
  instruction stands.
- `windowsVisible == turn-VISIBLE_THRESHOLD` at `:2477`: the premise
  "`windowsVisible` is no longer stamped by a per-frame projecting
  `recalcBounds`" was traced **FALSE** (CRIT-A). `terrobj.cpp:697 if(inView)`
  consumes the *returned* coarse `inView` (a strict superset), so the stamp
  still fires every frame for a superset of pick-eligible objects and the
  equality still holds. **DO NOT re-express or re-home this guard. DO NOT touch
  `terrobj.cpp:694/697/699`. Leave the `:2477` equality UNCHANGED (comment-only
  annotation per plan Task 4 Step 1).** The "load-bearing CRIT-1 sub-decision"
  framing applied ONLY to the (now-struck) `windowsVisible` half.

~~`canBeSeen()` and the `windowsVisible == turn-VISIBLE_THRESHOLD` guards at
`objmgr.cpp:2475/2477` must be re-expressed ... the plan must pick (a) or (b).~~
(Superseded by the split verdict above; retained struck for provenance.)

**[AMENDED 2026-05-18 by design-gate review CRIT-A — the following original
residual risk was traced FALSE; retained for provenance, superseded by the
RESOLVED note in "Residual risks" #1.]** ~~`windowsVisible == (turn -
VISIBLE_THRESHOLD)` ... pick silently returns nothing. The plan MUST re-home the
`windowsVisible` stamp.~~ **Correction:** `:697 if(inView)` consumes the
*returned* coarse `inView` (superset), so the stamp survives for a superset of
today's objects; the equality still holds for every pick-eligible object. NO
re-home; do not touch `terrobj.cpp:694/697/699`. The only genuine pick gap is
`upperLeft`/`lowerRight`/`screenPos`, handled by the lazy pick-time projection.

---

## Q3 - Lifecycle gate at `terrobj.cpp:694` - does it need to change?

**RESOLUTION: the lifecycle gate is UNDISTURBED if and only if the coarse
angular sphere-clip is retained and continues to write `inView`. The contract's
assumption ("recalcBounds keeps writing inView via the matrix-free angular
sphere-clip and only the projection is skipped") is STRUCTURALLY VALID but the
split is NOT as clean as the contract implies - the post-projection blocks also
refine `inView`. The plan must measure the split with the SLIMSPLIT RDTSC pattern
and decide the coarse-only `inView` semantics deliberately.**

**Grep-verified control flow (`BldgAppearance::recalcBounds`, bdactor.cpp):**
- `:1155` `inView = false;`
- `:1163` `inView = true;` then the **matrix-free angular sphere-clip**
  `:1165-1198` sets `inView=false` on vertical/horizontal angular reject. This
  block uses only `Camera::cameraFrame.trans_to_frame` + `GetExtentRadius()` -
  **no `projectForScreenXY`**. This is the separable coarse gate. It is set
  BEFORE the projection at `:1207` (confirms the contract's "I saw inView
  assignments ... BEFORE projectForScreenXY" - verified).
- `:1202 if (inView)` -> `:1207 projectForScreenXY` -> `:1209-1232` fog/distance
  **refines `inView`** (`distanceToEye > MaxClipDistance => inView=false`) ->
  `:1235 if (inView)` -> on-screen-rect + box projection -> `:1377-1382` and
  `:1576`/`:1581` further set `inView=false` if the projected rect is off-screen.
- `:1587 return(inView);`

So the **returned `inView`** (consumed by `terrobj.cpp:694 inView =
appearance->recalcBounds()` -> `:697 if (inView)` gating `windowsVisible=turn`
:699 and `appearance->update()` :752) is the *post-projection-refined* value,
NOT the coarse angular value. Deleting the projection block changes the lifecycle
gate's input from "projected-on-screen" to "coarse-angular-in-cone".

**Why this is still safe (the quantified split):**
- Coarse angular `inView` is a strict **superset** of projected `inView`
  (projection only ever sets `inView=false` further; it never resurrects a
  reject). So a coarse-only `inView` admits *more* objects to `update()`, never
  fewer. Per `cull_gates_are_load_bearing.md` and the contract's
  over-inclusion-is-safe / drop-is-catastrophic asymmetry, **widening the
  lifecycle gate is the safe direction** (more `update()` calls, never a missed
  one). The objmgr.cpp:2024-2031 comment ("UNSAFE to inner-gate buildings/turrets
  ... need update() even when offscreen") reinforces that *more* update() is the
  conservative choice; the slice must not *narrow* it.
- The cost concern is the inverse: coarse-only `inView` => more `update()` calls
  => the very `GameLogic.Units.TerrainObjects` zone we are trying to shrink could
  partially refill with `appearance->update()` work for objects the projection
  used to reject. The slice's net win = (projection cost removed) - (extra
  update() cost from the wider gate). The contract's ~1.43ms sizing baseline is
  the projection-inclusive number; the plan must size the *delta*, not assume the
  whole 1.43ms evaporates.

**Precedent for measuring the split: the SLIMSPLIT RDTSC pattern, verified at
`mclib/terrain.cpp:1459-1822`** (env `MC2_SLIM_COST_SPLIT`, `__rdtsc()` per-leaf,
`g_ssProjCyc`/`g_ssCullCyc`/`g_ssRedCyc` accumulators, summary every 600 frames
at `:1494` `[SLIMSPLIT v1] event=summary`, once-per-frame roll at `:1822`). The
plan MUST add an analogous `[*SPLIT v1]` RDTSC probe (own env var, e.g.
`MC2_TOBJ_COST_SPLIT`, forwarded in `scripts/run_smoke.py` allowlist) decomposing
`recalcBounds` into: ANGULAR (the kept :1163-1198 clip), PROJ (the to-delete
:1207-1583 block), and UPDATE (the `appearance->update()` at terrobj.cpp:752, to
size the refill). This is the Stage-0.5 measurement that turns "delete it" into a
quantified substitutive delta. Use RDTSC (~5-10ns), NEVER chrono per-call
(`cost_split_instrumentation_is_observer_effect_dominated.md`: chrono per-call in
this exact loop class fabricated ~5ms of phantom cost).

**Decision the plan must own:** the lifecycle gate does NOT need a *structural*
change (no edit to terrobj.cpp:694/697) - it keeps consuming the returned
`inView`. But the *semantics* of that `inView` change from projected to coarse.
The plan must (1) confirm via the RDTSC split that the refill from the wider gate
is small relative to the deleted PROJ cost, and (2) state explicitly that
coarse-only `inView` is the new lifecycle input and that it is superset-safe.

**Residual risk for review:** the on-screen-rect block also writes
`upperLeft`/`lowerRight` AND performs LOD/texture (re)load as side effects
(bdactor.cpp:1383-1583). The lifecycle gate itself does not consume those, but
deleting the block deletes the texture-handle load. If that is the sole texture
loader for late-registered/recovered actors, objects could update() with
unloaded textures. This is a delete-blast-radius the contract did not enumerate -
the plan must grep the type-init texture path and confirm a second loader exists,
or re-home the load. Flag as a CRIT candidate for adversarial review.

---

## Q4 - Measurement-gate dependency (precondition 5)

**RESOLUTION: precondition 5 is RESOLVED and nothing in this recon reopens it.
The plan's sizing baseline is ~1.43ms `GameLogic.Units.TerrainObjects` self-time,
worst-case zoomed-out big-map, captured with NO env vars on the latest 0.4 exe
with C6 necessarily live.**

- The contract (lines 5-11, 64-75) and handoff (lines 45-48) both record the
  user's clean capture: no env vars, latest 0.4 exe, C6 complete since the prior
  stage shipped it -> ~1.43ms is the real post-C6 worst-case residual; the
  earlier ~913us was a lighter-camera/earlier point, not production truth.
- Nothing in this recon contradicts that. The ~1.43ms is the *projection-
  inclusive* `GameLogic.Units.TerrainObjects` self-time (the zone at
  `objmgr.cpp:1967` wrapping the update loop `:1973-2070`, whose dominant cost is
  `objList[objIndex]->update()` at `:2051` -> `terrobj.cpp:694`
  `appearance->recalcBounds()`).
- **Caveat the plan must carry forward (consistent with Q3, not a reopen):** the
  ~1.43ms is the *target the slice attacks*, not the *guaranteed recovery*. The
  recoverable amount is (PROJ cost deleted) minus (extra `update()` from the
  wider coarse-only lifecycle gate). The Stage-0.5 SLIMSPLIT-pattern RDTSC probe
  (Q3) sizes that delta. This is a sizing *refinement* the plan performs, not a
  reopening of the measurement gate - the user's ~1.43ms total stands as the
  zone baseline; the probe partitions it.
- Substitutive proof gate (contract §"Substitutive proof gate") is unchanged and
  load-bearing: user-driven non-`MC2_TERRAIN_COST_SPLIT` total-frame Tracy,
  `GameLogic.Units.TerrainObjects` self-time -> ~0 AND total frame drops
  (anti-mirage), plus superset parity (readback-visible superset of legacy
  `inView`, zero `legacyCanBeSeen && !readbackVisible`), plus the
  fast-zoomed-out-pan visual canary (no prop pop/vanish = CRITICAL). The
  capped-FPS / Avg-FPS metric is invalid here
  (`capped_fps_is_not_a_cpu_cost_ab_signal.md`); the proof is the zone self-time
  + total-frame delta from a user-driven Tracy, not a smoke FPS average.

---

## Residual risks for adversarial review (prioritized)

1. **(RESOLVED — was CRIT, traced FALSE by the 2026-05-18 design-gate adversarial
   review, opus, CRIT-A.)** Original claim: `windowsVisible` must be re-homed or
   pick silently fails. **This is wrong.** `windowsVisible` is stamped at
   `terrobj.cpp:699` gated by `if(inView)` at `:697`, where `inView` is the
   **return value** of `recalcBounds()` at `:694` — NOT projection-internal
   state. The projection-body delete only changes that return value from
   projected to coarse-angular, a strict superset (the whole projection block is
   `if(inView)`-gated at `bdactor.cpp:1202` and can only narrow, never resurrect
   a coarse reject). The stamp therefore fires for a SUPERSET of today's objects;
   the `:2477` equality `getWindowsVisible() == (turn - 1)` still holds for every
   pick-eligible object; newly-admitted objects are filtered by the lazy
   screen-rect + geometry-space `PerPolySelect`. **NO re-home. DO NOT touch
   `terrobj.cpp:694/697/699`.** The original CRIT-1 misread textual proximity of
   `windowsVisible` to "projection" as a data dependency — an instance of the
   `feedback_data_flow_audit_asymmetry` failure inside this very recon. The
   genuine surviving pick gap is ONLY `upperLeft`/`lowerRight`/`screenPos`
   (deleted-block-only writers), handled by the plan's Task 4 lazy pick-time
   projection. See plan `2026-05-18-object-prop-iteration-gpu-port.md` Task 4
   Step 1 for the authoritative grep-traced statement.

2. **(CRIT) Projection-block delete also deletes the LOD/texture (re)loader**
   (`bdactor.cpp:1383-1583` Bldg, `:4455`+ Tree). If this is the sole texture
   loader for late-registered/recovered actors, props update()/render() with
   unloaded textures. The contract did not enumerate this side effect. Plan must
   confirm a type-init loader exists or re-home the load.

3. **(MAJOR) Shadow gate asymmetry.** `terrobj.cpp:866` is `canBeSeen()` only -
   no `g_useGpuStaticProps` OR-term, unlike the render gate `:796`. After the
   projection delete, `inView` is coarse-angular-only; shadows widen to the
   coarse set unless `:866` is repointed to the readback in lockstep with `:796`.
   Safe-direction (coarse is a superset, no dropped shadow) but must be stated
   and the wider shadow-draw cost considered against the slice's net win.

4. **(MAJOR) Net-win sizing, not zone-evaporation.** Coarse-only `inView` widens
   the lifecycle gate => more `appearance->update()` calls refill part of
   `GameLogic.Units.TerrainObjects`. The recoverable amount is the PROJ-vs-UPDATE
   delta, not the full ~1.43ms. The Stage-0.5 RDTSC SLIMSPLIT-pattern probe must
   quantify this before the slice is committed; a plan that asserts "1.43ms ->
   ~0" without the split is unsubstantiated.

5. **(MAJOR) Additive-not-substitutive trap via the existing
   `g_useGpuStaticProps`.** If the plan frames the change as adding
   `|| readback_isActorVisibleLagged(...)` next to the existing
   `|| g_useGpuStaticProps` at terrobj.cpp:796 *without deleting the recalcBounds
   projection body*, the zone does not move and the slice silently fails proof
   gate #1. The projection-body DELETE is the deliverable; the render-gate edit
   is consequential. Review must verify the delete is the primary change.

6. **(MINOR) `readback_isConservativeOrEnabled()` is file-static, not header-
   exposed** (`gpu_cull_readback.cpp:71`, default-ON, opt-out
   `MC2_GPU_CULL_CONSERVATIVE_OR=0`). The conservative-OR + dilation are applied
   *inside* `readback_buildActorVisSnapshot` (`:670`), so the render/pick consumer
   only calls `readback_isActorVisibleLagged()` and gets the motion-safe result
   transparently - there is no separate "turn conservative-OR on at the render
   site" action. The contract's phrasing ("Conservative-OR+dilation MUST be on
   for the render repoint") is satisfied by default and by the snapshot build at
   `objmgr.cpp:1908-1911` (built when `readback_isEnabled()`), NOT by anything the
   render gate does. Plan should not invent a render-site OR-flip.

7. **(MINOR) `objmgr.cpp:1908` snapshot is built under
   `s_gpuCullLifecycle || readback_isEnabled()`.** The render/pick repoint depends
   on `readback_buildActorVisSnapshot` having run that frame. Confirm the snapshot
   build precedes the `GameLogic.Units.TerrainObjects` loop (it does:
   `:1908-1911` is before `:1967`) and that `readback_isEnabled()` is the
   required gate (contract precondition 3). Fail-open (all-visible) when disabled
   is the safe degradation (`stock_install_must_remain_playable.md`).

8. **(MINOR) `TerrainObject::isVisible()` (terrobj.cpp:488-506) is a second
   recalcBounds caller with no `code/*.cpp` callers** (grep `isVisible()` =>
   no matches). Treat as dead; do not let a reviewer assume it is a live pick
   freshness path. If a virtual-dispatch caller exists outside `code/`, the plan
   must grep `mclib/` for it before declaring it dead.

---

## Summary table

| Q | Resolution | Primary site(s) | Residual risk |
|---|---|---|---|
| Q1 | Narrow gate at terrobj.cpp:796/:866; delete recalcBounds PROJ body in bdactor.cpp:1202-1583 (Bldg) + Tree mirror; do NOT touch appear.h:176 | bdactor.cpp:1152/4278; terrobj.cpp:796/866 | Shadow gate :866 lacks g_useGpuStaticProps OR-term; texture-loader side effect |
| Q2 (CRIT-1) | Lazy per-candidate projection at pick time (findObjectByMouse), gate on readback-visible; trees never picked so Tree body delete is consumer-free | objmgr.cpp:2459/2475/2477/2480-2483; bdactor.cpp:1055 (PerPolySelect survives) | AMENDED: windowsVisible re-home is NOT needed (review CRIT-A traced it FALSE — coarse-inView stamp survives, superset-safe); only upperLeft/lowerRight/screenPos need the lazy projection |
| Q3 | No structural change to terrobj.cpp:694/697; inView becomes coarse-angular-only (superset-safe); size split via RDTSC SLIMSPLIT-pattern probe | bdactor.cpp:1155-1198 (kept) vs :1202-1583 (deleted); terrain.cpp:1459-1822 (precedent) | Net win is PROJ-minus-refill delta, not full 1.43ms |
| Q4 | RESOLVED, not reopened. Baseline = ~1.43ms TerrainObjects self-time, no-env worst-case zoomed-out, C6 live | objmgr.cpp:1967 zone | Recoverable != baseline; Stage-0.5 RDTSC probe partitions it |

All citations grep-verified at HEAD `4d9529a`. The single most important
plan-shaping conclusion: **frame the slice as the recalcBounds projection-body
DELETE with CRIT-1 (pick) + Q3 (lifecycle-semantics) re-homing; the render-gate
edit is consequential because `g_useGpuStaticProps` already makes render
`inView`-independent. Framed as a render-gate OR-term addition the slice is
additive and fails the substitutive proof.**

---

## Stage-0.5 measured split (Task 0)

Probe: `[TOBJSPLIT v1]` env-gated RDTSC (`MC2_TOBJ_COST_SPLIT=1`). Accumulators
in `code/terrobj.cpp`; probe points in `mclib/bdactor.cpp` (both
`BldgAppearance::recalcBounds` and `TreeAppearance::recalcBounds`). Summary
interval: 120 frames (not 600 -- smoke hard-capped at 30s). Smoke:
`MC2_TOBJ_COST_SPLIT=1 py -3 scripts/run_smoke.py --tier tier1 --duration 30
--kill-existing --keep-logs`. Result: PASS 5/5. Artifact dir:
`tests/smoke/artifacts/2026-05-18T11-03-39/`.

ANGULAR = matrix-free sphere angular clip (kept). PROJ = `projectForScreenXY` +
8-corner box + fog refine (targeted for deletion). UPDATE = `appearance->update()`
(refill risk from the wider coarse-only lifecycle gate after the delete).

Last full 120-frame window per mission (steady-state; first window is inflated by
mission-load transitions):

| Mission | ANGULAR cyc | PROJ cyc   | UPDATE cyc | PROJ ratio |
|---------|-------------|------------|------------|------------|
| mc2_01  | 1,737,542   | 6,092,088  | 0          | 77.8%      |
| mc2_03  | 7,339,781   | 16,074,881 | 41,261     | 68.5%      |
| mc2_10  | 9,684,723   | 46,776,228 | 0          | 82.8%      |
| mc2_17  | 5,216,370   | 20,023,176 | 0          | 79.3%      |
| mc2_24  | 3,202,059   | 3,473,188  | 0          | 52.0%      |
| **AGG** | 27,180,475  | 92,439,561 | 41,261     | **77.3%**  |

PROJ ratio = PROJ / (ANGULAR + PROJ + UPDATE).

Key observations:
- PROJ is the dominant cost in 4 of 5 missions (68-83%). mc2_24 is the outlier
  at 52% (PROJ and ANGULAR are near-equal; this mission has fewer on-screen static
  props so the absolute cycle counts are lower and the ANGULAR fraction is larger).
- UPDATE is near-zero in steady state (0 in 4/5 missions). The first 120-frame
  window shows elevated UPDATE (mission-load dynamic activity); by steady state it
  drops to nearly zero. This is the critical sizing signal: after the projection
  delete, the refill from the wider coarse-only lifecycle gate is expected to be
  small relative to the deleted PROJ cost.
- mc2_10 has the highest PROJ absolute (46.8M cyc / 120 frames ~= 390K cyc/frame).
  At ~3.5 GHz that is ~0.11ms/frame from PROJ alone per window -- corroborates the
  ~1.43ms Tracy zone having PROJ as the primary contributor (the RDTSC accumulators
  sum Bldg + Tree for the whole update loop, amortized across all active objects).

These numbers are raw RDTSC cycle counts accumulated over a 120-frame window, NOT
wall-time. They are relative ratios; absolute ms values require dividing by CPU
frequency and are NOT the right metric for this probe (see
`cost_split_instrumentation_is_observer_effect_dominated.md` -- ratios from RDTSC
are valid; the absolute-vs-Tracy delta is a separate concern).

Status: **DONE**. PROJ is clearly dominant across the mission set. Controller +
user determine go/no-go for Task 1+.

Format note (2026-05-18, post-review): the probe now emits `*_cyc_per_frame`
fields (e.g. `angular_cyc_per_frame`, `proj_cyc_per_frame`, `update_cyc_per_frame`)
normalized by the 120-frame window, matching SLIMSPLIT (terrain.cpp:1492-1498).
The numbers in the table above are the originally-recorded raw 120-frame cumulative
totals; the PROJ ratio 77.3% aggregate / dominant 4-of-5 is interval-invariant and
stands unchanged.

---

## Task 1 -- texture-loader precondition (CRIT-2)

Date: 2026-05-18. Branch: claude/gpu-driven-rendering. All file:line citations
grep-verified at HEAD (post-Task-0 commits 2285da6 / ecc7a1b / 40ab1e1).

### Step 1: Texture-load side effect mapped in the projection block

The projection block being deleted in Task 2 (`BldgAppearance::recalcBounds`
and `TreeAppearance::recalcBounds`, the `if (inView)` body after the coarse
angular clip) contains two per-LOD texture-load sub-blocks, both reachable only
under specific conditions:

**Bldg -- `bdactor.cpp` projection block (currently lines ~1407-1595):**

- `if (selectLOD != currentLOD)` block (~:1442): deletes and recreates
  `bldgShape` from `appearType->bldgShape[currentLOD]`, then loads textures
  into the new shape via `mcTextureManager->loadTexture` / `SetTextureHandle`
  loop for all `bldgShape->GetNumTextures()` entries.
- `if (currentLOD && baseLOD)` block (~:1546): resets `currentLOD = 0`, same
  delete/recreate/load pattern for `bldgShape[0]`.

**Verdict -- both sub-blocks are DEAD CODE today (grep-confirmed):**
- `selectLOD` is a local variable declared and initialized to `0` at the top
  of the LOD section (`DWORD selectLOD = 0;`). The original dynamic selection
  logic was replaced by the 2026-05-12 TEMP LOD-0 pin (`(void)useHighObjectDetail;`);
  `selectLOD` is never modified after initialization.
- `currentLOD` is initialized to `0` in `BldgAppearance::init` at
  `bdactor.cpp:658`. With `selectLOD == 0` and `currentLOD == 0`, the
  condition `selectLOD != currentLOD` is always `false`. The LOD-swap texture
  loader never fires.
- `baseLOD` is declared `true` and never modified. `currentLOD` is `0`. The
  condition `currentLOD && baseLOD` evaluates to `0 && true == false`. The
  base-LOD-reset texture loader never fires.

**Tree -- `bdactor.cpp` projection block (currently lines ~4500-4603):**
- Same structure: `selectLOD = 0` local pin, `currentLOD` initialized to `0`
  in `TreeAppearance::init` at `bdactor.cpp:3942`. Both sub-blocks (`if
  (selectLOD != currentLOD)` at ~:4508 and `if (currentLOD && baseLOD)` at
  ~:4556) are unreachable by the same argument.

**Summary:** the projection block contains texture loaders that are exclusively
for LOD-swap events (switching from LOD-0 to LOD-N) and base-LOD resets (back
to LOD-0). Under the 2026-05-12 TEMP pin, `selectLOD` is always `0` and
`currentLOD` is always `0`, so neither condition is ever true. No texture
handle is written by this block on any path reachable at runtime today.

### Step 2: Opposite-direction grep -- second (type-init) loader

Per `feedback_data_flow_audit_asymmetry.md`: a negative claim ("the projection
block is not the only loader") requires grepping the TYPE-INIT path, not the
obvious symbol. Grep `BldgAppearance::init` and `TreeAppearance::init`
directly, not the texture-load symbol.

**`BldgAppearance::init` -- `bdactor.cpp:611-715` (grep-verified):**

Definition: `bdactor.cpp:611` `void BldgAppearance::init (AppearanceTypePtr
tree, GameObjectPtr obj)`.

At `bdactor.cpp:658`: `currentLOD = 0;` (zero-init).

At `bdactor.cpp:676-715` (inside `if (appearType)` block):
```
bldgShape = appearType->bldgShape[0]->CreateFrom();  // line 678
// Load the texture and store its handle.
for (int i=0;i<bldgShape->GetNumTextures();i++)      // line 682
{
    ...GetTextureName(i,txmName,256)...
    ...mcTextureManager->loadTexture(textureName, gos_Texture_Alpha/Solid, ...)
    ...bldgShape->SetTextureHandle(i, gosTextureHandle)...
}
```

This is the sole live LOD-0 texture loader for buildings. It runs unconditionally
at actor init time (during mission setup, before the first frame), loads LOD-0
textures for the created `bldgShape`, and sets all texture handles. It is NOT
gated on `inView` or any projection result.

**`TreeAppearance::init` -- `bdactor.cpp:3919-3998` (grep-verified):**

Definition: `bdactor.cpp:3919` `void TreeAppearance::init (AppearanceTypePtr
tree, GameObjectPtr obj)`.

At `bdactor.cpp:3942`: `currentLOD = 0;` (zero-init).

At `bdactor.cpp:3959-3998` (inside `if (appearType)` block):
```
treeShape = appearType->treeShape[0]->CreateFrom();  // line 3961
// Load the texture and store its handle.
for (long i=0;i<treeShape->GetNumTextures();i++)     // line 3965
{
    ...GetTextureName(i,txmName,256)...
    ...mcTextureManager->loadTexture(textureName, gos_Texture_Alpha/Solid, ...)
    ...treeShape->SetTextureHandle(i, gosTextureHandle)...
}
```

Exact mirror of the Bldg loader: unconditional at init time, loads LOD-0
textures for `treeShape`, sets all texture handles.

**Conclusion:** a concrete second (type-init) loader exists at both
`BldgAppearance::init:676-715` and `TreeAppearance::init:3959-3998`. These
are the SOLE live LOD-0 texture loaders. The projection-block loaders are
unreachable dead code.

### Step 3: Outcome

**Outcome (a): second loader confirmed.** The projection-block texture loaders
are dead code under the 2026-05-12 TEMP LOD-0 pin and are unreachable today.
The init-time loaders (`BldgAppearance::init:676-715`,
`TreeAppearance::init:3959-3998`) are the sole live LOD-0 texture loaders.

Task 2's projection-body delete is texture-safe: deleting the projection block
removes only dead code from the LOD-swap / base-LOD-reset paths. The LOD-0
textures that actors carry at runtime were loaded at init time and will remain
valid after the delete.

**No code change required by this task.**

### Latent-hazard note (for resolution doc AND for Task 2 deletion point)

The following comment text is to be placed at the Task 2 deletion point in
`bdactor.cpp` (both the Bldg and Tree analogous locations), verbatim:

```cpp
// LATENT HAZARD (texture-loader): the deleted projection block contained the
// per-LOD-swap texture (re)loader (SetTextureHandle loop under
// `if (selectLOD != currentLOD)` and `if (currentLOD && baseLOD)`). These
// paths are dead under the 2026-05-12 TEMP LOD-0 pin (selectLOD = 0 always,
// currentLOD = 0 always => conditions never true). The SOLE live LOD-0 texture
// loader is BldgAppearance::init (bdactor.cpp:676-715) / TreeAppearance::init
// (bdactor.cpp:3959-3998), which runs unconditionally at actor-init time.
// If the TEMP pin is ever reverted (when the LOD-1 invisibility root cause is
// fixed), LOD selection + the per-LOD texture loader MUST be re-homed to a
// path that runs outside the projection block BEFORE the revert lands --
// the init-time loader covers LOD-0 only; LOD-1+ textures will be unloaded
// after any LOD swap if this comment is not addressed first.
```

Status: **DONE (outcome a)**. No code change. Task 2's projection-body delete
is texture-safe at the current TEMP-pin state. The latent hazard above must be
applied as a code comment at the deletion point in Task 2.
