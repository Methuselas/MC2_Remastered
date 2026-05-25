# Object Offload Arc — Initial Brainstorm

Date: 2026-05-02
Worktree: `nifty-mendeleev`
Author: ThranduilsRing + Claude (Opus 4.7, 1M context)
Status: brainstorm; do NOT spec or code from this without sign-off
Previous arc (closed): terrain offload (M0–M2, renderWater, indirect-terrain SOLID)
Prior attempt (killswitched): `docs/superpowers/specs/2026-04-19-gpu-static-prop-renderer-design.md`

---

## Arc summary (one paragraph)

The terrain offload arc closed end-to-end on 2026-05-02 with the indirect-terrain
SOLID default-on flip. Per-frame CPU iteration over ~40K terrain quads is now
zero; recipes are written once on terrain mutation and the GPU does the work.
This brainstorm opens a **separate, parallel arc** that asks the same question
of mechs / vehicles / buildings: can the per-frame CPU work for object
rendering be moved to the GPU? The starting position is **fundamentally
different** from terrain. Terrain quads are not lifecycle-managed: they have no
`update()` call, no pool allocation, no `setExists(false)` cascade — GPU
frustum cull was sufficient. Objects ARE lifecycle-managed via the
`inView`/`canBeSeen`/`objBlockInfo.active`/`objVertexActive` chain (verified
live at `code/objmgr.cpp:1491-1511` and `code/objmgr.cpp:1758-1784`), and per
`memory/cull_gates_are_load_bearing.md` "bypass cascades into streak artifacts,
destroyed objects, or silent shape drop-outs." A prior GPU static-prop attempt
(commits `f0b85f0` through `e23746c`, branch `claude/nifty-mendeleev`) was
killswitched and now functions as a "static-props-OFF toggle" rather than a
working alternate path. The load-bearing question for this brainstorm is **not**
"what's the seam?" — it's **"what made the prior attempt fail, and how does
this attempt avoid the same failure class?"**

This document answers Q1–Q8 with grep-grounded evidence, builds a
Failure-Mode Countermeasure Matrix, and a code-grounding verification
appendix. It does NOT propose a spec. The closing section names the
ready-for-spec / needs-more-recon / blocked decision.

---

## Q0. Empirical cost baseline (from user, 2026-05-02)

User-supplied Tracy data for object work per frame:

| Zone | Cost | Source |
|---|---|---|
| `TerrainObject::update appearanceUpdate` | **~2.4 ms** (~80% of 3 ms total update) | `code/terrobj.cpp:607` `ZoneScopedN("TerrainObject::update appearanceUpdate"); appearance->update();` (gated by `inView` at 603) |
| `TerrainObject::update recalcBounds` | < 100 µs (rest of the 3 ms is mover update + housekeeping) | `code/terrobj.cpp:599` separate Tracy zone |
| All-objects render | ~900 µs | per user |
| **Total objects** | **~4 ms** | |

**The cost is in update, not render. Update is ~3× the render cost.**

What runs inside `appearance->update()` for buildings/trees/generics
(grep-verified):

- `BldgAppearance::update` at `mclib/bdactor.cpp:1932-2200+`:
  - per-actor housekeeping (point-light placement, flash timers, rotation,
    LOD-related fog/light)
  - `bldgShape->TransformMultiShape(&xlatPosition, &rot)` at `mclib/bdactor.cpp:2171`
- `TG_TypeMultiShape::TransformMultiShape` at `mclib/msl.cpp:1359+`:
  - per-child loop (typically 1-10 children per building):
    - `SetTextureHandle` per texture per child per frame at `msl.cpp:1365`
      (see `memory/mc2_texture_handle_is_live.md`)
    - hierarchy traversal, compute child `shapeToWorld`
    - `TG_Shape::TransformShape` per leaf — the inner kernel:
      - `getVerticesFromPool(numVertices)` at `mclib/tgl.h:1092`
      - per-vertex transform of local → world → screen via `s_worldToClip`
      - per-vertex lighting evaluation against the active light list
        (this is where `multiSetLightList` lives in the call chain;
        full grep deferred to recon)
      - bake the result into `listOfVertices[j].argb` (gos_VERTEX, offset 16 —
        `memory/mc2_argb_packing.md`)

**The 2.4 ms is dominated by per-vertex lighting bake** for the surviving
(in-view) static-prop population, run on CPU every frame.

This data **inverts a load-bearing claim** the brainstorm originally made
(Q3 hypothesis "Render is the cost, not recalcBounds") and **sharpens
Q1(a4)** ("populations were correctly scoped; cost-blindspot was
elsewhere"). The downstream sections incorporate the correction.

---

## Q1. What did the prior attempt actually fail at?

The prior attempt produced shipping code (commits `f0b85f0`..`e23746c`) but
the resulting GPU path is documented (in `CLAUDE.md` "Load-Bearing Cull
Infrastructure" lines 85-89, in `docs/gpu-static-prop-cull-lessons.md`
lines 187-203, and in the orchestrator's "Blocked / parked" row at
`cpu-to-gpu-offload-orchestrator.md:113`) as **a "static-props-off
toggle" — not a working alternate path.** That's the headline failure.
The categorized substructure:

### (a) Architectural decisions that turned out wrong

**a1. "Bypass the cull" was the load-bearing seam decision; it cascaded.**
The design's culling section (`2026-04-19-gpu-static-prop-renderer-design.md:447-460`)
adopted **C2 — "render everything, let the GPU clip."** That decision was
made knowing `inView`/`canBeSeen` had a ~87% false-negative rate at wolfman
zoom, but **without modeling how those gates couple to update / pool
allocation / lifecycle.** The cascade is documented in
`memory/cull_gates_are_load_bearing.md` and verified live:

- `inView` gates `Mech3DAppearance::update`'s `updateGeometry()` call at
  `mclib/mech3d.cpp:4183` (`if ((turn < 3) || inView || (currentGestureId
  == GestureJump) || g_useGpuStaticProps)` — the killswitch term was
  added by commit `aef7e14` because without it, mech `TransformShape`
  never ran, leaving stale `listOfVertices` and triggering the silent
  early-out at `mclib/tgl.cpp:2560-2567`).
- `canBeSeen()` gates `Building::render` at `code/bldng.cpp:1071` (and
  similar at `code/terrobj.cpp`, `code/gate.cpp`, `code/artlry.cpp`).
- `objBlockInfo[].active` + `objVertexActive[]` gate the entire
  iteration loop at `code/objmgr.cpp:1491-1511` (render) and
  `code/objmgr.cpp:1758-1784` (update). These gates were attempted to
  be bypassed (commits `b4cc927` then reverted at `27a1434`); reverted
  because forcing all blocks active caused `Building::update`-style
  state checks to fail and trigger `MC2_DESTROY(...)` at `objmgr.cpp:1775`.
  **Objects were destroyed permanently for the rest of the mission.**

The C2 decision is correct *in isolation* — it solves visibility. It
**failed in coupling** because the C2 designer assumed cull was
visibility, when grep would have shown it gates four other things.

**a2. Pool sizes were treated as policy rather than as part of the
contract.** Commit `4888084` bumped pools 30K→500K. That fixed the
"half the mechs vanish" symptom (per `memory/tgl_pool_exhaustion_is_silent.md`)
but didn't address the architectural question: **pool sizing was part
of the cull contract, not an independent variable.** The note at
`docs/gpu-static-prop-cull-lessons.md:84-94` makes this explicit
("the cull gates weren't just a correctness issue — they were
load-bearing for shared-resource budgets"). The pool bump survives
to this day at `code/mission.cpp:3140-3152` (verified). It is a band-aid
that costs ~60 MB resident even when the killswitch is OFF. A proper
architectural answer would have been "GPU path owns its own staging,
doesn't share the CPU pool" — but that wasn't in the spec.

**a3. The shadow path was an open task at session end.**
`flushShadow()` was empty per `2026-04-20-static-prop-handoff.md:99-101`.
Static props in GPU mode therefore cast no shadows — losing a feature
the CPU path provides. The design treated this as "Tasks 13-14 (deferred)"
when in practice the absence of shadows was visible in side-by-side
diffs and contributed to the user perception that GPU mode "looks
worse." Lesson: **shadow parity is part of the visual gate, not a
follow-up.**

**a4. Cost-blindspot: deferred lighting was the actual cost.**
Per Q0's empirical baseline, the 2.4 ms is spent INSIDE `appearance->update()`
in per-vertex lighting bake (`TransformMultiShape` → `TransformShape` chain,
`mclib/bdactor.cpp:2171` and `mclib/msl.cpp:1359+`). The prior C2 design
explicitly **deferred** GPU lighting:
> "Shader-evaluated world lights. CPU vertex lighting via
> `TransformMultiShape` + `multiSetLightList` is retained for now;
> moving lighting to the GPU is a pure-perf follow-up, not part of this
> spec." (`2026-04-19-gpu-static-prop-renderer-design.md:46-49`)

The data flow per frame in the prior design (`2026-04-19-gpu-static-prop-renderer-design.md:303-314`):

```
*Appearance::update()      ← unchanged; runs TransformMultiShape ← bakes listOfColors
*Appearance::render()
  └─ batcher.submit()
       └─ memcpy shape->listOfColors into per-instance color staging
batcher.flush()
  └─ glDrawElementsInstancedBaseVertex      ← reads baked colors via SSBO
```

The submit path **memcpy's CPU-baked colors into an SSBO**. The CPU
TransformMultiShape work — the 2.4 ms — runs unchanged. The GPU path
moves only the inner-loop emit (`TG_Shape::Render` per-vertex `gVertex[3]`
build, the ~900 µs render zone). **The arc shipped scaffolding that
couldn't move the cost it claimed to target.** Population scoping
(static props) was correct on cost grounds — buildings/trees/generics
ARE where the 2.4 ms lives — but the cost lived in update-time lighting,
not render-time emit, and the design deferred the only mechanism that
could have moved it.

A separate observation about animated populations: GVs and mechs were
also out of scope, requiring per-node bone matrices the batcher didn't
support. Per Q0 their aggregate cost is the residual ~600 µs of update
(20% of 3 ms), not the dominant share. **Animated objects are NOT the
high-value perf target on current data; static-prop GPU lighting is.**

### (b) Implementation bugs that compounded

**b1. Cached texture handle.** Commit `4af44f7` fixed it. The packet
table cached `listOfTextures[slot].gosTextureHandle` at registration
time; per `memory/mc2_texture_handle_is_live.md`, that handle is
mutated every frame by `TG_TypeMultiShape::TransformMultiShape` at
`mclib/msl.cpp:1365` (`SetTextureHandle(j, myMultiType->listOfTextures[j].mcTextureNodeIndex)`).
First frame after registration was correct; subsequent frames bound
texture 0 → black. The current code in
`gos_static_prop_batcher.h:39-42` correctly stores `textureSlot` and
resolves at draw time. **This bug class generalizes:** any new GPU
path that treats per-frame-mutated MC2 state as static will produce
"correct first frame, drift afterward" symptoms.

**b2. Read of wrong color stream.** Same commit (`4af44f7`) — the
batcher was reading `shape->listOfColors` (TG_Vertex: specular only,
usually zero) instead of `shape->listOfVertices[j].argb` (gos_VERTEX,
the actual lit ARGB). Symptom: black buildings. The two zero-input
multiplication is almost a tautology — modes 3/4/5 in
`shaders/static_prop.frag` were specifically built to bisect this.
Lesson: **debug-mode color cycle is mandatory infrastructure, not
optional polish.**

**b3. Layer B false positives.** Commit `1585db1` — `submitMultiShape`
was returning false on any child that was a helper/bone node, daytime
spotlight, or null `listOfColors`. Per
`2026-04-20-static-prop-handoff.md:29-37`, "most buildings have at
least one such child, so the safety net was firing on ~100% of
actors." Result: the entire renderer fell back to CPU and looked
identical to killswitch-OFF. Lesson: **a fallback that fires on
~100% of inputs is not a fallback, it's a no-op disguised as
correctness.** Visual canary should have caught this immediately
because GPU mode would have been pixel-identical to CPU mode (no
draws issued).

**b4. Behind-camera projection produces screen-spanning streaks.**
Commit `ea96c13` — D3D-style `1/clip.w` math at
`shaders/static_prop.vert` reverses sign for behind-camera vertices,
producing garbage NDC and triangles that span the screen as diagonal
streaks. The guard `if (clip4.w < 0.1) gl_Position = vec4(2,2,2,1)`
solved the visible streaks but per
`memory/gpu_direct_renderer_bringup_checklist.md:31-34`, this is the
trap #7 (CPU pre-cull is the load-bearing frustum gate). **Without
CPU cull, no GPU-side test can fully distinguish behind-camera
vertices from in-front ones.** The threshold guard catches the worst
case but is fundamentally a band-aid for "no CPU cull" — the
indirect-terrain path solved this by per-vertex `pzValid` bits in
the thin record, computed CPU-side. The static-prop path inherited
the band-aid.

### (c) Latent system properties not understood at design time

**c1. TGL pool exhaustion is silent.** Per
`memory/tgl_pool_exhaustion_is_silent.md`, `getVerticesFromPool` (at
`mclib/tgl.h:1092` — verified) returns NULL when exhausted,
`TransformShape` stores it, `TG_Shape::Render` early-outs. **No
log, no assert, no GL error.** The design didn't model this because
it didn't know. The 30K→500K bump was a discovery of the property,
not a design accommodation of it. The current code has Tier-1
instrumentation that addresses this — `tgl.h:1126-1138` records the
first NULL per frame with caller name + shape pointer + monotonic
counter — but that infrastructure landed AFTER the bug, and the
pool-bump cost survives.

**c2. `objBlockInfo` activation is itself a derivative of terrain
vertex angular cull.** Per `docs/gpu-static-prop-cull-lessons.md:28-65`,
the chain is:

```
terrain.cpp:geometry() → angular cull (terrain.cpp:1517-1532)
  → terrain.cpp:1610-1611 setObjBlockActive + setObjVertexActive
    → objmgr.cpp:1491,1758 iterate active blocks (render/update)
      → individual object's update/render runs, gated also by inView
```

The 87% false-negative rate is at the TERRAIN-VERTEX angular cull
step, not at the object cull step. Bypassing the per-actor cull
without addressing the terrain-vertex cull means the object
iteration never even reaches the per-actor stage — the iteration
loop's outer `if (Terrain::objBlockInfo[terrainBlock].active)` guard
at `objmgr.cpp:1491` skips the whole block. Bypass attempts of THIS
gate (`b4cc927`) destroyed objects via the `MC2_DESTROY(...)` cascade
at `objmgr.cpp:1775`. **The design treated cull as a single thing;
it's actually a 4-stage cascade with different fix targets at each
stage.**

**c3. Late `registerType`.** Per
`2026-04-20-static-prop-handoff-part2.md:124-132`, two types per
mission load are registered AFTER `finalizeGeometry()` and trigger
the immutable-VBO trap. Suspected source: artillery/bomber spawns
(`code/artlry.cpp:1580` "Shilone"). The design's "Layer A
enumeration" assumed all types were knowable at map-load; the
runtime reality has late spawns. **Mid-game VBO reallocation is
prohibited by the design (`2026-04-19-gpu-static-prop-renderer-design.md:209-211`),
so late types fall to CPU permanently** — meaning the CPU path
still has to be alive and correct for these. The promise "retire
the CPU path eventually" is harder than the design admitted.

### Failure synthesis

The prior attempt failed because the **C2 "render everything"
decision was correct in isolation but wrong in coupling.** Cull
gates the design wanted to bypass were also gating four other
systems (update, pool, lifecycle, transform freshness). Bypassing
cull without compensating those four cascaded into pool exhaustion,
destroyed objects, stale matrices, and the screen-spanning streak
bug. The pool bump and the projection guard are still in tree as
band-aids; the architectural question (separate iteration loop with
own pool, OR fix terrain-vertex angular cull at source) was never
answered.

**This brainstorm's central question, restated:** if the next
attempt's seam decision ALSO ends up coupling to the cull cascade
in a way the designer didn't model, this attempt fails the same way.
Therefore: **what is the seam decision that does not bypass the
cull cascade?**

---

## Q2. What's the actual goal?

Three plausible framings. Pick one with reasoning grounded in Q1.

### (i) Replicate the terrain offload pattern: SSBO + GPU draw, one population at a time

Buildings first (most static), then vehicles, then mechs. Each population
gets a recipe SSBO + thin record + dedicated draw, hooked AFTER
`renderLists()` per the established pattern (`memory/render_order_post_renderlists_hook.md`).

**Pros:**
- Pattern is proven (renderWater, indirect-terrain SOLID).
- Per-population gating means rollback is cheap.
- Buildings are the cheapest case (T-pose, no animation FSM) — least
  risk for the first slice.

**Cons (LOAD-BEARING):**
- Buildings are NOT the population whose CPU cost matters. Per Q1(a4),
  static props are cheap per-instance. The expensive populations
  (mechs, GVs) require per-node animation matrices, which the terrain
  pattern does not handle.
- Replicating the pattern means a recipe SSBO indexed by some
  map-stable key. Objects don't HAVE a map-stable key in the way
  terrain quads do (`vertexNum = mapY * realVerticesMapSide + mapX`,
  per `memory/water_ssbo_pattern.md`). Buildings have a `WatchID` but
  it's not contiguous, not mission-stable across save/load (verify),
  and not a natural SSBO index. Late-spawn artillery has no slot at
  map-load time at all.
- The cull cascade is still load-bearing. (i) doesn't say which
  cull-cascade step it's compensating.

### (ii) Solve the cull cascade FIRST (separate prerequisite slice), THEN do offload

Two sub-options:

**(ii-A)** Fix the terrain-vertex angular cull at `mclib/terrain.cpp:1517-1532`
so its 87% false-negative rate at wolfman zoom drops to <5%. Per
`docs/gpu-static-prop-cull-lessons.md:200-203`, this requires "deep
projectZ / verticalSphereClipConstant understanding." The math is
intricate enough that nobody has cracked it across multiple sessions.

**(ii-B)** Build a separate object-iteration loop that doesn't share
`ObjectManager` infrastructure with the CPU path. Has its own update
pass and its own pool. This is the path
`docs/gpu-static-prop-cull-lessons.md:198-200` calls "the proper
architectural fix (not done this session)."

**Pros (both sub-options):**
- Addresses the root cause Q1 identifies (cull-bypass cascade).
- The benefit transfers to every subsequent population — buildings,
  vehicles, mechs all stand to gain.
- (ii-A) might fix wolfman-zoom artifacts in CPU mode too as a
  side benefit.

**Cons:**
- (ii-A) is gated on math understanding nobody has. Could be a
  multi-week recon. High variance.
- (ii-B) is a major refactor. The CPU path's `ObjectManager::update`
  loop is load-bearing for game logic, not just rendering — a parallel
  GPU loop has to either re-derive the same lifecycle decisions or
  consume the CPU path's results, in which case it's not really
  parallel.
- Neither sub-option produces a perf win on its own; the offload
  arc only starts AFTER the prerequisite. Multi-month before any
  Tracy delta lands.

### (iii) Reframe entirely: not all objects need to be GPU-resident

Maybe just **buildings** (most static, no animation FSM, ~50-150 per
mission). Vehicles and mechs stay CPU-driven; their CPU cost is
small enough that offloading them isn't worth the lifecycle complexity.

**Pros:**
- Aligns with the Q1(a4) finding that static props are cheap. Doesn't
  pretend offloading them is the high-value target.
- Smallest blast radius: doesn't touch animated-object code paths at
  all, so no per-node-matrix design needed.
- Validation surface is tractable: buildings are visually
  characterizable (no animation), parity is bytewise on a small set
  of attributes.

**Cons:**
- If buildings aren't where the cost is, why is this offload
  happening at all? **The honest answer might be: it isn't.** Maybe
  the object offload arc is not justified on cost grounds, and this
  brainstorm should conclude "blocked — no compelling perf target
  exists for objects until animated populations are addressed,
  which requires (ii-B), which is too large."

### Pros / cons of (iii) revisited under Q0 data

**(iii) reframed:** static buildings/trees/generics are NOT cheap —
they own ~2.4 ms of update each frame. (iii) is no longer "modest
substrate, small perf delta." It is **the correct population for the
high-value perf target** — provided the slice actually moves the
cost. The cost is in `BldgAppearance::update`'s `TransformMultiShape`
call (per-vertex lighting bake), NOT in the render path the prior
attempt replaced.

This bifurcates (iii) into two distinct slice shapes:

**(iii-A) Substrate-only — replace the render path, leave update alone.**
Same shape as the prior attempt's design but with Q1's bug-class fixes
applied. Ships ~0% perf delta on update (TransformMultiShape unchanged)
and ~minor improvement on render (~900 µs → smaller). **Honest
expectation: lost in noise.** Worth shipping ONLY as substrate for
(iii-B). Useless on its own.

**(iii-B) Lighting offload — move TransformShape's per-vertex lighting
bake to a GPU vertex shader.** Reads per-vertex normals + light list
from SSBOs, computes lit ARGB in the VS, no CPU `TransformShape` per
in-view actor. **This is the slice that moves the 2.4 ms.** Requires
substrate from (iii-A) plus: GPU-resident per-vertex normals (already
on type VBO per prior design), GPU-readable light list (new per-frame
SSBO), VS-side lighting model (port `multiSetLightList` math), and a
parity gate that compares CPU-baked ARGB to GPU-computed ARGB
bytewise (or with ULP tolerance — TBD per Q5).

### Decision (deferred to user)

My recommendation: **(iii-A) followed by (iii-B), as a two-slice arc.**

> **Slice 1 (iii-A):** GPU-resident static buildings/trees/generics with
> CPU-baked colors uploaded via SSBO. Q3 = (a) — CPU cull unchanged.
> Population scope: buildings + trees + generics (all three share
> `TransformMultiShape` cost; no reason to slice further on populations
> when the seam is the same). **Honest perf expectation: ~0%.** Ships
> as substrate and as a Q1-bug-fix landing pad (texture-handle
> resolution, ARGB stream, Layer B semantics, behind-camera guard).
>
> **Slice 2 (iii-B):** GPU vertex lighting for the same population.
> Replaces `TG_Shape::TransformShape`'s per-vertex lighting kernel.
> **Perf expectation: substantial portion of the 2.4 ms recovered**
> — bounded above by total update cost minus housekeeping; needs
> recon to refine. This is the actual perf-justified slice.

Slice 1 is **substrate-mandatory** for slice 2: slice 2 needs the
per-instance SSBO + custom shader infrastructure, which is what
slice 1 builds. They cannot collapse into one slice because: (a) the
parity surface is different (slice 1 = matrix + visibility + color
stream parity; slice 2 = bytewise lit ARGB parity, harder), (b) the
risk surface is different (slice 1 inherits prior bug-class with known
fixes; slice 2 is novel work — porting `multiSetLightList` to GLSL).

If the user is OK with shipping slice 1 as "no perf, just substrate,
followed by slice 2 within the same arc" — proceed. If the user wants
slice 1 to demonstrate perf BEFORE slice 2 commits, the answer is
"no, slice 1 cannot show perf — the cost lives in update, slice 1
doesn't touch update, end of story."

(i) is rejected — it's the prior attempt with a different population
label and the same cost-blindspot.

(ii-A/B) — cull cascade refactor — is still out of scope. Nothing in
Q0's data argues for it: the 2.4 ms is in `TransformMultiShape` for
**cull-survivors**, not in the cull machinery itself. Fixing the cull
doesn't move that cost.

---

## Q3. Lifecycle integration seam

If we go with Q2(iii) — buildings only, no cull bypass — Q3 simplifies.
But documenting the seam choice for the record:

### (a) CPU still computes cull state every frame; GPU draw consumes the cull state

**This is the choice for Q2(iii).** `BldgAppearance::recalcBounds()`
at `mclib/bdactor.cpp:1091` continues to compute `inView` per actor,
exactly as it does today. `BldgAppearance::render` at
`mclib/bdactor.cpp:1546` continues to be called per-actor by
`code/bldng.cpp:1071`'s `if (appearance->canBeSeen() || g_useGpuStaticProps)`
loop (which itself is gated by `objmgr.cpp:1491`'s active-block check).
**The seam:** when `g_useGpuObjects=true`, `BldgAppearance::render`
calls `g_objectBatcher.submit(actor, shapeToWorld, ...)` instead of
issuing the CPU draw. Submit = thin-record append. Flush draws all
submitted instances at frame end with one `glDrawElementsInstanced`
per type (or a multi-draw indirect if cement-multi-sampler-style
extension is desired).

Cull cascade unchanged. Pool budget unchanged (because
`TransformMultiShape` continues to run for the same set of actors —
the ones in active blocks). Lifecycle unchanged.

### (b) GPU compute computes cull state; CPU reads back per frame

REJECTED. Reintroduces the readback stall problem from terrain D2/D3
(per orchestrator). Adds latency to the cull decision while solving
no problem the (a) path doesn't already solve.

### (c) GPU compute computes cull state; CPU reads back NEXT frame (1-frame stale)

REJECTED. The 1-frame ghost-cull risk is documented in
`memory/cull_gates_are_load_bearing.md`. Stale `inView` for one frame
means stale `updateGeometry()` decision means stale `listOfVertices`
means silent shape drop-out (per `mclib/tgl.cpp:2560-2567`). Compounds
exactly the failure mode Q1 identifies.

### Defending (a) explicitly

The "obvious safe choice" is the right one because:

1. The CPU cull cascade is load-bearing for systems besides rendering
   (update, pool, lifecycle). Trying to remove the CPU cull doesn't
   just save the cull cost — it requires rewriting four other systems.
   That's the Q1(a1) cascade.
2. **Hypothesis (needs Tracy verification before spec):** per-actor
   `recalcBounds()` is not the dominant cost; per-actor `Render()` is.
   Specifically: `TG_Shape::Render`'s per-vertex `gVertex[3]` build
   loop (`mclib/tgl.cpp:2598-2611+`) is the inner kernel. If true,
   GPU-resident replaces this; the cull cascade is not on the
   critical path. **If recalcBounds turns out to dominate, Q2(iii)'s
   small-Tracy-delta expectation gets even smaller — the slice
   ships only for substrate, not perf.**
3. Tracy data from indirect-terrain SOLID arc shows that "remove the
   per-frame iteration" wins ~80% of the available delta even when
   the recipe-build cost is non-zero. The same shape applies here.

**Decision:** Q3 = (a). CPU computes cull, GPU draws what survived.

### One residual risk (Q3-r1)

`BldgAppearance::recalcBounds` modifies `bldgShape->ClearAnimation()`
+ `delete bldgShape; bldgShape = appearType->bldgShape[currentLOD]->CreateFrom()`
when LOD changes (`mclib/bdactor.cpp:1373-1377`). The animation guard
at `mclib/bdactor.cpp:1336-1366` already suppresses LOD swap for
animated buildings (turrets, gates, dropships). **For the GPU path,
LOD swap means `shape` pointer changes mid-mission**, which means
the batcher's per-instance `typeID` lookup invalidates for that actor.
Compensating mechanism: the batcher must look up `typeID` from
`shape->myType` at submit time, not cache it. The prior batcher code
at `gos_static_prop_batcher.cpp:414` already does this — verified
at `submit()`: `TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(shape->myType);`.
**Verify the next slice preserves this.**

---

## Q4. Population scoping

Which object types in scope for THIS arc? Per Q2(iii) decision: **buildings
only.** But documenting the per-population characterization for the record
and for future slices:

### Scope normalization (advisor 2026-05-02)

**Slice 1 = static-prop multishapes: buildings + trees + generics**, with
an explicit admission rule: only appearances whose update path is proven
TransformMultiShape-compatible AND non-mover-lifecycle. Earlier prose
in this brainstorm called this "buildings only"; that wording is
incorrect and superseded — slice 1 covers all three populations
because they share the seam, the substrate, and the cost driver.

### Buildings (`BldgAppearance`, `mclib/bdactor.cpp`)

- **Count per mission:** typically 50-150 (rough estimate; needs
  `MC2_DESTROY_TRACE=1` summary or a fresh ZoneScopedN counter to confirm).
- **Per-frame update cost:** dominant share of the 2.4 ms appearanceUpdate zone
  per Q0; specifically `TransformMultiShape` at `mclib/bdactor.cpp:2171`.
- **Animation:** mostly static. Animated subset (turrets, gates,
  dropships) is small and identified by `bdAnimData[i] != NULL` per
  `mclib/bdactor.cpp:1337-1344`.
- **LOD:** swappable via `currentLOD`, suppressed for animated.
- **Texture handles:** mutated per-frame via `SetTextureHandle` at
  `mclib/bdactor.cpp:879-893`. Resolve at draw time per
  `memory/mc2_texture_handle_is_live.md`.
- **Lifecycle:** Permanent for mission duration. `Building::update`
  always returns 1 (`code/bldng.cpp` end of function — verified). They
  don't `MC2_DESTROY` themselves under stale state, so the Q1(a1) cascade
  is mostly a non-issue for buildings specifically (it's a worse hazard
  for movers).
- **Recommended:** **IN SCOPE for this arc.** Slice 1 (iii-A) replaces
  render path; slice 2 (iii-B) replaces lighting bake.

### Trees / Generic / Plane fallback

- Similar shape to buildings. Both call `TransformMultiShape` per frame
  (trees at `mclib/bdactor.cpp:4261`, generics inside their analogue
  update path — verify in recon). They share the appearanceUpdate cost.
- The prior attempt wired `TreeAppearance::render` (`mclib/bdactor.cpp:3984`)
  and `GenericAppearance::render` (`mclib/genactor.cpp:771`). They were
  killswitched along with everything else.
- **Recommended (revised under Q0 data):** **IN SCOPE alongside buildings.**
  No reason to artificially split — they share the seam, the substrate,
  and the cost driver. Slicing on populations when the offload mechanism
  is identical adds slice count without adding risk-information gain.

### Vehicles (`GVAppearance`, `mclib/gvactor.cpp`)

- **Count per mission:** 10-30.
- **Animation:** per-node (torso, weapon mounts). Requires per-instance
  bone-matrix data in the SSBO, which the prior batcher didn't support.
- **Lifecycle:** Mover. Can `MC2_DESTROY` via `objmgr.cpp:1804-1809`
  if `update()` returns false. Active cull cascade hazard.
- **`updateGeometry` gate:** `mclib/gvactor.cpp:2287` (function start).
- **Recommended:** **OUT OF SCOPE for this arc.** Defer until
  per-node-animation-on-GPU is its own design. That design likely
  doubles or triples the SSBO schema and is its own brainstorm.

### Mechs (`Mech3DAppearance`, `mclib/mech3d.cpp`)

- **Count per mission:** 20-40 (capped at `MAX_MOVERS=255` per
  `mclib/dmovemgr.h:16`).
- **Animation:** per-node + gesture FSM (`mclib/mech3d.cpp:3813`
  `update(bool animate)`, full FSM body 3813-4180+).
- **Texture:** per-instance (paint scheme). `localTextureHandle` at
  render time (`mclib/mech3d.cpp:2369`).
- **Lifecycle:** Mover, full destroy hazard. `inView` gates
  `updateGeometry()` at `mclib/mech3d.cpp:4183` (via
  `g_useGpuStaticProps` cascading bypass).
- **Cost characterization:** Mechs have the highest per-instance
  CPU cost across all object populations because of: (a) per-node
  animation FSM, (b) per-frame `TransformMultiShape` running across
  ~10-20 nodes, (c) per-frame texture-handle rewrite for
  paint+arms (`mclib/mech3d.cpp:2369-2375`).
- **Recommended:** **OUT OF SCOPE for this arc.** This is the
  high-value target on cost grounds, but it's also the highest
  risk. Animated-mech-on-GPU is its own arc, gated on (ii-B).

### Slice ordering for this arc

Under the revised Q2 (iii-A → iii-B):

1. **Slice 1 (iii-A):** buildings + trees + generics, render-path
   replacement only. Substrate. ~0% perf delta. Lands all Q1
   bug-class fixes in one go on a known-shape problem.
2. **Slice 2 (iii-B):** same population, GPU vertex lighting.
   Replaces the per-vertex lighting bake. **This is the perf slice**;
   Tracy delta target needs recon to set, but is bounded above by
   the ~2 ms of `TransformShape` cost minus housekeeping.

For animated populations (vehicles, mechs): out of scope for THIS arc.
Their cost is the residual ~600 µs of update; ROI is low. If the user
later wants per-node-animation-on-GPU, that's a third slice with its
own brainstorm — and the iii-A substrate is reusable for it.

**Don't pretend slice 1 unlocks slice 3 without re-deriving the seam.**
Slice 2 is a natural extension of slice 1's substrate; slice 3 is not.

---

## Q5. Parity strategy

Object parity is harder than terrain parity because:

- Per-frame state mutates (animation, damage, rotation, gesture).
- Texture handles mutate per-frame (`memory/mc2_texture_handle_is_live.md`).
- Some draws are conditional on game state (selected, paused,
  `drawFlash`, `drawBars`, etc.).

Three candidates:

### (i) Per-object transform + visibility bit per frame

For each in-active-block actor: byte-compare
`(typeID, shapeToWorld_matrix, highlight_argb, fog_argb, flags)`
between CPU-emit and fast-path-emit. Equivalent to the `[PATCH_STREAM
v1] event=thin_record_parity` pattern from the M2 arc.

**Pros:**
- Small (~100 B per actor per frame), tractable.
- Mirrors the established pattern (renderWater, indirect-terrain).
- Bytewise; no FP-tolerance fudge needed.

**Cons:**
- Doesn't catch per-vertex divergence (which is where the prior
  attempt's b1/b2 bugs lived). Texture handle resolution and
  per-vertex ARGB stream don't show up at the per-instance level.

### (ii) Vertex-stream byte-compare

Compare the per-vertex output stream — every triangle's 3 `gos_VERTEX`
records (as legacy `addVertices` would have built them) — against the
fast-path's per-instance + per-vertex SSBO read.

**Pros:**
- Catches everything (i) catches plus the per-vertex bugs.

**Cons:**
- Large. ~200 verts/instance × ~100 instances × 32 B/vert = 640 KB/frame.
- Comparison harder: legacy emits via `mcTextureManager->addVertices`,
  fast-path emits via SSBO; the comparable form requires synthesizing
  the legacy emit on the CPU and the fast-path equivalent ALSO on the
  CPU before draw — not easy.

### (iii) Pixel-level screenshot diff at fixed camera

Captures fully-realized output. Decouples from animation-state
divergence by pinning animation phase. New tooling needed.

**Pros:**
- Catches everything visible.
- Robust to internal representation changes (SSBO refactors).

**Cons:**
- Slow: need pixel-perfect determinism (animation phase, lighting
  state, camera, time-of-day) which MC2 doesn't natively provide.
- Tooling-heavy: existing `tests/smoke/` doesn't pixel-diff. Would
  need a new pinned-camera mode.
- Slow iteration loop: dev runs the game, captures, diffs.

### Recommendation

**Combine (i) + a downsized (iii):**

- **(i) at every frame** as the fast in-loop parity check
  (`MC2_OBJECT_PARITY_CHECK=1`, silent-on-pass, 600-frame summary,
  per established pattern). Catches the per-instance class of bugs.
- **(iii) at one fixed camera at smoke-gate time** (1 mission, 1
  pinned camera, pixel diff against legacy reference). Catches the
  per-vertex / texture-binding / shader class of bugs that (i)
  misses. Doesn't run per-iteration; runs once per slice as a gate.

Skip (ii). The CPU cost of synthesizing the legacy emit stream is
high and the bugs it catches are caught by (i)+(iii) faster.

**For this arc specifically:** (i) is sufficient because the substrate
is just SSBO writes and a single draw. (iii) is the gate before
default-on flip, not before merge.

---

## Q6. Modding angle (forward extensibility)

The terrain arc ended with the multi-sampler pattern at
`memory/indirect_terrain_solid_endpoint.md` as a modder-friendly
substrate (cement catalog at sampler unit 3, future tex4-tex15 free
for decals/overlays/scorch/footprints).

Does the object offload arc have an analogous extensibility target?

### Candidate 1: Per-mech custom shaders

Modders adding effects (heat shimmer, damage-state fades, paint
overlays). Currently MC2 has `mech.frag` (or whatever — verify) and
modders can't extend without touching engine code.

**As an extensibility seam:** the per-instance SSBO entry could
carry a `shaderVariantID` field, and the batcher could draw each
variant in its own glDraw* call. Modders register variants via
sidecar config.

**Out of scope for this arc** (scope = buildings, no mechs). But
worth flagging for slice 4.

### Candidate 2: Sidecar object types

Mods adding new object classes. Currently this is a major engine
change because new types need ABL bindings, lifecycle hooks, etc.

**Not addressed by offload arc.** This is a content-pipeline
question, not a render-path question. Out of scope.

### Candidate 3: Per-instance tinting / override

Cement-multi-sampler analogue. Buildings could expose per-instance
tint, per-instance LOD-pin, per-instance shadow override. Modders
ship a sidecar "buildings.tint.json" that maps actor types to
overrides, and the batcher reads the table at registration time.

**Light-touch and reusable.** Suggested as a stretch goal IF slice 1
naturally produces a per-instance struct field that can carry an
override. Don't add it speculatively.

### Recommendation

**Document as candidate seams. Do not lock in for slice 1.** Slice 1
ships the substrate (per-instance SSBO + custom shader); slices 2-4
revisit modder-extensibility once the populations they cover are in
view.

This mirrors the cement-multi-sampler arc's discipline: the modder-
friendly seam emerged at the END of the arc, after the architectural
pattern was proven, not at the beginning.

---

## Q7. Gate ladder for object slices

Terrain's 4-gate ladder (visual canary, Tracy delta, parity, tier1
5/5 in config triples) transferred cleanly across slices. For the
object arc — adapted:

### A. Visual canary

Side-by-side legacy/fast at fixed camera. Buildings have animation
in their subset (turrets, gates rotate; dropships descend) — this
makes naive screenshot diff harder. **Mitigation:** pin the canary
camera on a mission with no animated buildings (mc2_01 has clean
static airbase scenery, verify). Or run the canary on a single
frame after pause-from-deterministic-replay.

### A'. Pinned-camera screenshot diff

Per Q5(iii). Once-per-slice gate, not per-iteration.

### B. Tracy delta

Per Q0, the cost zones are:
- `TerrainObject::update appearanceUpdate` — 2.4 ms — slice 1 (iii-A) does NOT touch
- All-objects render — 900 µs — slice 1 (iii-A) replaces

**Slice 1 (iii-A) Tracy gate:** no regression on either zone, +small
improvement on render (target: ≥30% reduction on render, since the
SSBO+single-draw replaces the `mcTextureManager->addVertices` + per-actor
build loop). On `appearanceUpdate`: target zero delta — slice 1
explicitly does not move that work. **Slice 1 ships as substrate; the
Tracy gate is "didn't make it worse."**

**Slice 2 (iii-B) Tracy gate:** ≥30% reduction on `appearanceUpdate`
as a starter target. The realistic ceiling is harder to set without
recon — `TransformMultiShape` is more than just lighting (texture-handle
rewrite, hierarchy traversal, child shapeToWorld compute), so the
fraction of the 2.4 ms attributable to TransformShape's lighting kernel
specifically needs measurement. **Recon item for slice 2 spec write.**

**State explicitly in the spec:** slice 1 is a substrate slice,
not a perf slice. Don't claim a perf delta target on slice 1 that
the architectural shape can't deliver — that was Q1(a4)'s failure
mode, and avoiding it here is load-bearing.

### C. Parity

`MC2_OBJECT_PARITY_CHECK=1` zero mismatches across stock-content
tier1. Per Q5 recommendation: per-instance (i) for in-loop, pinned
camera (iii) for gate.

### D. tier1 5/5 PASS triple

unset / FASTPATH=1 / FASTPATH=1+PARITY_CHECK=1. +0 destroys delta.
**+0 destroys is load-bearing here** — Q1(a1)'s cull cascade lived
in `MC2_DESTROY` calls. Any delta in destroys = the slice has
re-introduced a cull-cascade hazard.

### Additional gate (object-specific)

**E. TGL pool peak ≤ pre-slice peak.** With Q3=(a), pool consumption
should be unchanged (same actors call `TransformShape`). If peak
rises, the slice has bypassed the cull cascade somewhere it
shouldn't have. Use the existing
`MC2_TGL_POOL_TRACE` summary line at shutdown.

### F. "GPU actually drew something" counter (advisor 2026-05-02)

The prior Layer B failure made the fast path **pixel-identical** to
the CPU path because fallback fired on ~100% of inputs — a fallback
that never lets the GPU draw can pass visual canary, parity, and
Tracy gates while doing zero real work. Required summary line at
shutdown / 600-frame cadence:

```
[OBJBATCHER v1] event=summary frames=N
  submitted_instances=S gpu_drawn_instances=G
  cpu_fallback_instances=F eligible_actors=E
  fallback_rate=F/E
```

Gate: `G > 0` AND `F/E < threshold` (initial threshold suggestion: 5%).
A run where `F/E ≈ 1.0` means the slice is shipping a no-op disguised
as correctness — slice does not pass.

### Late-registration accounting (advisor 2026-05-02)

`[OBJBATCHER v1] event=late_register type=<typeName> count=N`
**aggregate per type, not log-once.** Gate: zero unexpected late
static-prop registrations OR the source is named in an explicit
allowlist in the spec. Implicit fallback to CPU path at first
late-registration is documented but not silent.

### Pinned-camera screenshot diff (advisor 2026-05-02)

**Pre-condition for default-on, NOT pre-condition for flagged merge.**
Slice 1 may merge behind `MC2_GPU_OBJECTS=1` (default off) when
gates A–F pass. Default-on flip requires the pinned-camera diff
to clear separately.

### Triggers (slice will not ship behind flag)

- Any visual canary regression.
- Tracy delta NEGATIVE on render zone.
- Parity mismatches > 0.
- Destroys delta != 0.
- Pool peak rises.
- Gate F: `gpu_drawn_instances == 0` or fallback rate ≥ 5%.
- Gate (late-register): unexpected types not in allowlist.

---

## Q8. Risk inventory & countermeasure for each prior failure

This is the load-bearing reverse of Q1. For each Q1 finding, what
mechanism prevents this attempt from re-treading it?

See **"Failure-Mode Countermeasure Matrix"** below.

If a finding has no countermeasure, the slice is not ready to spec
— surface to user. None do.

---

## Failure-Mode Countermeasure Matrix

| Q1 finding | Prior failure | Countermeasure for this arc |
|---|---|---|
| **a1** Cull-bypass cascade | C2 design bypassed cull; pool exhausted, objects destroyed, streaks | **Q3 = (a):** CPU cull unchanged. GPU draws ONLY actors that survive existing cull. No bypass anywhere. |
| **a2** Pool sizes treated as policy | 30K→500K bump; ~60 MB resident even when killswitch off | Q3 = (a) means pool consumption unchanged. **Verify with gate E** (TGL pool peak ≤ pre-slice peak). Don't bump pools. |
| **a3** Shadow path open at session end | `flushShadow()` empty in prior batcher | **Slice 1 explicitly excludes shadows.** Buildings continue to cast shadows via the existing CPU path. The fast-path replaces only the color render call, not the shadow render call. Shadow integration is its own slice. |
| **a4** Cost-blindspot: deferred lighting was the cost | Prior design memcpy'd CPU-baked colors into SSBO — moved render emit but not the per-vertex lighting bake (the actual 2.4 ms) | **Two-slice arc:** slice 1 (iii-A) ships substrate with explicit "no perf claim" framing. Slice 2 (iii-B) ships GPU vertex lighting — that's where the 2.4 ms reduction lives. Don't ship slice 1 alone with a perf framing it can't deliver. |
| **b1** Cached texture handle | Black buildings after frame 1 | Spec mandates: store `textureSlot`, resolve at draw time via `type.source->listOfTextures[slot].gosTextureHandle`. Re-cite `memory/mc2_texture_handle_is_live.md` in the spec. |
| **b2** Wrong color stream | Black buildings (different cause) | Spec mandates: read per-vertex ARGB from `listOfVertices[j].argb` (gos_VERTEX, offset 16), NOT from `listOfColors`. |
| **b3** Layer B fires on ~100% of inputs | Whole-multishape CPU fallback fires constantly | Spec mandates: skip ineligible children individually (helper, spotlight, null colors), don't fail whole multishape. Mirror `mclib/tgl.cpp:2560-2567`'s skip semantics. |
| **b4** Behind-camera streak | clip.w<0 produces garbage NDC | Q3 = (a) means cull-survivor set is finite and known-in-frustum at admission time. The `clip4.w < 0.1` shader guard is BACKUP, not primary defense. Per `memory/gpu_direct_renderer_bringup_checklist.md` trap #7 — pre-cull is the real fix. |
| **c1** TGL pool exhaustion silent | `getVerticesFromPool` NULL → silent shape drop-out | (1) Q3 = (a) → no extra TransformShape calls → pool consumption unchanged. (2) `MC2_TGL_POOL_TRACE=1` already exists per CLAUDE.md "Tier-1 Instrumentation Env Vars"; gate E uses its summary line. |
| **c2** `objBlockInfo` activation derives from terrain-vertex angular cull | Bypass attempts at `objmgr.cpp:1491` destroyed objects | Q3 = (a) means `objmgr.cpp:1491` and `objmgr.cpp:1758` are not touched. The terrain-vertex angular cull at `mclib/terrain.cpp:1517-1532` is also not touched. Both stay as-is. |
| **c3** Late `registerType` | Two types/mission registered after `finalizeGeometry` | (1) Slice 1 = buildings only; artillery/bomber are out of scope. (2) Spec must include a "late-registration soft-fail policy": unregistered types fall to CPU path *for that frame*, log once. (3) **Test:** stderr-grep for `[OBJBATCHER] late registerType` after each smoke run; non-zero count surfaces to user before merge. |

**No findings without countermeasures.** Slice 1 is not blocked by
unresolved Q1 cascades.

---

## Code-grounding verification appendix

Every cited symbol has been grep'd at write-time. Format:
`citation → actual file:line → status`.

| Citation | Verified | Status |
|---|---|---|
| `cull_gates_are_load_bearing.md` "objmgr.cpp:1731 update iterates active blocks" | `code/objmgr.cpp:1758` (line drift; loop body is `if (Terrain::objBlockInfo[terrainBlock].active)` then per-object update gated by `objVertexActive`) | matches claim, line drifted |
| `cull_gates_are_load_bearing.md` "objmgr.cpp:1748 setExists(false) on update false" | `code/objmgr.cpp:1775` `MC2_DESTROY(objList[objIndex], "update_false");` (now wrapped in MC2_DESTROY macro per destroy-invariant rule) | matches in spirit; macro replaces direct call |
| `cull_gates_are_load_bearing.md` "tgl.h:1022 getVerticesFromPool returns NULL when exhausted" | `mclib/tgl.h:1092-1104` `gos_VERTEX * getVerticesFromPool (DWORD numRequested, ...)` returns NULL when `numVertices >= totalVertices` | matches, line drifted; instrumented via `recordNull` |
| `cull_gates_are_load_bearing.md` "tgl.cpp:2536 silent early-out on null listOfVertices" | `mclib/tgl.cpp:2560-2567` `if (!listOfVertices || !listOfColors || ... ) return;` | matches, line drifted |
| `cull_gates_are_load_bearing.md` "mech3d.cpp:4170 inView gates updateGeometry" | `mclib/mech3d.cpp:4183` `if ((turn < 3) \|\| inView \|\| (currentGestureId == GestureJump) \|\| g_useGpuStaticProps)` | matches; killswitch term added by commit `aef7e14` |
| `cull_gates_are_load_bearing.md` "gvactor.cpp:2702 GVAppearance same pattern" | `mclib/gvactor.cpp:2287` `void GVAppearance::updateGeometry (void)` (function start; gate is in caller `update()` at 2613) | matches in spirit; line drift |
| Prior design "BldgAppearance::recalcBounds at bdactor.cpp:1090" | `mclib/bdactor.cpp:1091` `bool BldgAppearance::recalcBounds (void)` | matches, +1 line |
| Prior design "Building::render at bldng.cpp" canBeSeen gate | `code/bldng.cpp:1071` `if (appearance->canBeSeen() \|\| g_useGpuStaticProps)` | matches; killswitch term added |
| Prior handoff "mission.cpp:3097-3110 pool sizes 500K" | `code/mission.cpp:3140-3152` `colorPool->init(500000); vertexPool->init(500000); facePool->init(200000); shadowPool->init(500000); trianglePool->init(200000);` | matches, line drifted |
| Prior handoff "tglHeap 128MB" | `code/mission.cpp:3125` `unsigned long tglHeapSize = 128 * 1024 * 1024;` | matches |
| `mc2_texture_handle_is_live.md` "msl.cpp:1321 SetTextureHandle per-frame" | `mclib/msl.cpp:1365` `listOfShapes[i].node->myType->SetTextureHandle(j,myMultiType->listOfTextures[j].mcTextureNodeIndex);` | matches in spirit, line drifted |
| Cull-lessons doc "terrain.cpp:1127 setObjBlockActive setObjVertexActive on clipInfo" | `mclib/terrain.cpp:1610-1611` `setObjBlockActive(currentVertex->getBlockNumber(), true); setObjVertexActive(currentVertex->vertexNum,true);` (gated by `currentVertex->clipInfo`) | matches, line drifted |
| Cull-lessons doc "terrain.cpp:1040-1053 angular cull" | `mclib/terrain.cpp:1517-1532` `if (object_angle > (vClipConstant + extent_angle)) onScreen = false;` etc. | matches in spirit, line drifted (terrain.cpp has grown) |
| Prior design "GpuStaticPropBatcher singleton" | `GameOS/gameos/gos_static_prop_batcher.h:57-112` class definition with `submit`, `submitMultiShape`, `flush`, `flushShadow` | matches |
| Prior design "GpuStaticPropInstance 112 bytes std430" | `gos_static_prop_batcher.h:13-32` `static_assert(sizeof(GpuStaticPropInstance) == 112, ...);` and offsetof asserts | matches |
| Prior design "killswitch g_useGpuStaticProps" | `gos_static_prop_killswitch.h:8` `extern bool g_useGpuStaticProps;` | matches |
| Prior design "BldgAppearance::render at bdactor.cpp:1521" | `mclib/bdactor.cpp:1546` `long BldgAppearance::render (long depthFixup)` | matches in spirit, line drifted |
| Prior design "TreeAppearance::render at bdactor.cpp:3953" | `mclib/bdactor.cpp:3984` `long TreeAppearance::render (long depthFixup)` | matches, line drifted |
| Prior design "GenericAppearance::render at genactor.cpp:768" | `mclib/genactor.cpp:771` `long GenericAppearance::render (long depthFixup)` | matches, +3 lines |
| Memory `bldg_animation_lod_swap_unsafe.md` "fix on agile-hopper" | `mclib/bdactor.cpp:1336-1366` LOD-swap suppression for animated buildings is ALREADY HERE on `terrain-pbr-mod` branch | **memory is stale on branch attribution; the fix has been merged.** Update memory after sign-off. |
| Cull-lessons doc "mission.cpp:3091 tglHeapSize" | `code/mission.cpp:3125` `unsigned long tglHeapSize = 128 * 1024 * 1024;` | matches in spirit, line drifted |
| `tgl_pool_exhaustion_is_silent.md` "mission.cpp:3097-3110 pool init" | Same as above; pools are at 3140-3152 in current code | matches in spirit, line drifted |
| Orchestrator "Blocked/parked GPU static props" | `cpu-to-gpu-offload-orchestrator.md:113` `\| **GPU static props** ... \| Cull-bypass infrastructure cascades into pool exhaustion + stale matrices.` | matches |
| `MAX_MOVERS` 255 | `mclib/dmovemgr.h:16` `#define MAX_MOVERS 255` | matches |
| `MC2_DESTROY` macro existence | `code/objmgr.cpp:1775` `MC2_DESTROY(objList[objIndex], "update_false");` and `objmgr.cpp:1725` `MC2_DESTROY(specialBuildings[spBuilding], "update_false");` | matches; macro is the destroy-instrumentation envelope |
| `[PROJECTZ:BoolAdmission] terrain_cpu_vert_admit` | `mclib/terrain.cpp:1576-1577` `// [PROJECTZ:BoolAdmission id=terrain_cpu_vert_admit] PROJECTZ_SITE("terrain_cpu_vert_admit", "BoolAdmission");` | matches; suggests projectZ subsystem has registered admission sites |
| `MC2_TGL_POOL_TRACE` env var | `CLAUDE.md:173` documents this env-gated logger; pool tier-1 instrumentation in `mclib/tgl.h:1118-1144` (`firstNullSnapshot` etc.) | matches |
| Q0 cost zone "TerrainObject::update appearanceUpdate" | `code/terrobj.cpp:607` `ZoneScopedN("TerrainObject::update appearanceUpdate"); appearance->update();` (gated by `inView` at line 603) | matches |
| Q0 cost zone "TerrainObject::update recalcBounds" | `code/terrobj.cpp:599` `ZoneScopedN("TerrainObject::update recalcBounds"); inView = appearance->recalcBounds();` | matches |
| Q0 claim "TransformMultiShape inside BldgAppearance::update" | `mclib/bdactor.cpp:2171` `bldgShape->TransformMultiShape (&xlatPosition,&rot);` inside `BldgAppearance::update` body | matches |
| Q0 claim "TreeAppearance same shape" | `mclib/bdactor.cpp:4261` `treeShape->TransformMultiShape (&xlatPosition,&rot);` inside `TreeAppearance::update` | matches |
| Q1(a4) claim "prior design deferred GPU lighting" | `docs/superpowers/specs/2026-04-19-gpu-static-prop-renderer-design.md:46-49` "Shader-evaluated world lights. CPU vertex lighting via `TransformMultiShape` + `multiSetLightList` is retained for now" | matches verbatim |
| Q1(a4) claim "design memcpy's CPU-baked colors into SSBO" | `2026-04-19-gpu-static-prop-renderer-design.md:303-314` data-flow diagram; "memcpy shape->listOfColors into per-instance color staging buffer" | matches verbatim |
| Q3-r1 claim "batcher resolves typeID at submit time, not registration" | `GameOS/gameos/gos_static_prop_batcher.cpp:414` `TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(shape->myType);` inside `submit()` (line 442 packs `inst.typeID`) | matches; corrected from earlier 404 typo |

**Findings:**
- All cited symbols verified live. Line numbers have drifted from
  prior memory — expected; the affected files have grown.
- One memory file (`bldg_animation_lod_swap_unsafe.md`) is stale on
  branch attribution: the LOD-swap-suppression fix is already on
  `terrain-pbr-mod` (this worktree's parent). Update after user
  sign-off.
- No fictional symbols. No wrong signatures. No missing constraints.

---

## Closing: ready-for-spec / needs-more-recon / blocked

### Slice 1 ships behind flag, NOT default-on (advisor 2026-05-02)

Slice 1 may merge with `MC2_GPU_OBJECTS=1` (default off) once the
gate ladder passes. **Default-on flip is gated separately** on
either:

- a material render-cost reduction demonstrated by Tracy data
  (the 900 µs render zone shrinks meaningfully), OR
- slice 2 landing in the same arc (so default-on coincides with
  the actual perf win, not the substrate-only state).

This avoids the prior attempt's framing trap: shipping a
"working-looking substrate" as the endpoint when it doesn't move
the target cost. The discipline matches the indirect-terrain SOLID
PR1 → cement multi-sampler pattern: substrate landed flagged,
default-on followed only after the architectural seam was complete.

### Ready-for-spec for slice 1 (iii-A); slice 2 (iii-B) needs recon

**Slice 1 (iii-A — buildings + trees + generics, render-path only,
no cull bypass, Q3=(a) seam):** ready for spec writing. All eight
Q's resolved. Failure-Mode Countermeasure Matrix accounts for every
Q1 finding. Spec must explicitly frame slice 1 as **substrate, no
perf claim** — Q1(a4)'s failure was claiming perf on a path that
can't deliver it.

**Slice 2 (iii-B — GPU vertex lighting for the same population):**
NOT ready for spec. Slice 2 is gated on a recon that the brainstorm
must surface explicitly because skipping it could re-introduce the
prior attempt's failure class:

#### Recon item ZERO (load-bearing — advisor 2026-05-02)

**Identify every consumer of `TG_Shape::listOfVertices`,
`listOfColors`, `listOfShadowTVertices`, and per-shape transformed
state after `appearance->update()` for buildings/trees/generics.**

Initial grep evidence (this brainstorm session):

- `TG_Shape::TransformShape` at `mclib/tgl.cpp:1687-1690` allocates
  `listOfVertices`, `listOfColors`, AND `listOfShadowTVertices`
  from the TGL pools.
- `TG_Shape::RenderShadows` at `mclib/tgl.cpp:3262` **consumes
  `listOfShadowTVertices`** — references at `mclib/tgl.cpp:2562,
  2808, 2819, 2906, 3022` (the last writes
  `eye->projectForScreenXY(...)` results into `listOfShadowTVertices[index].transformedPosition`).
- `BldgAppearance::renderShadows` at `mclib/bdactor.cpp:1924-1926`
  calls `bldgShape->RenderShadows()` directly.
- Same pattern at `TreeAppearance::renderShadows` (`mclib/bdactor.cpp:4144`)
  and `GenericAppearance::renderShadows` (`mclib/genactor.cpp:1015`).

**Implication:** removing CPU `TransformShape` to bake lighting on
GPU breaks the CPU shadow path because shadows read state that
`TransformShape` produces. Slice 2 cannot simply "replace lighting
bake" without one of:

- (2-a) Move shadows to GPU as part of slice 2 (this is the
  prior batcher's `flushShadow()` Task 13-14 work, never landed).
- (2-b) Keep a reduced CPU `TransformShape` pass that produces
  `listOfShadowTVertices` but skips the lighting bake. Partial
  offload; the perf win shrinks proportionally.
- (2-c) Disable shadows for the GPU population temporarily.
  Visual regression; not acceptable.

**Other consumers to enumerate during recon:**

- Selection (`drawBars`, `drawSelectBrackets`, mouse hit testing)
- Damage flash visualization
- Highlight ARGB animation
- Screen-space label rendering (`drawTextHelp` etc.)
- Debug overlays
- Fog/haze per-actor application

**Until this recon is complete, slice 2's perf claim is
unbacked.** The "slice 2 recovers a substantial portion of 2.4 ms"
framing is too optimistic; the recovery is bounded by what's left
after preserving consumers' inputs. Realistic perf might be a
fraction of the 2.4 ms, not most of it.

#### Other recon items (post-Recon Zero)

1. Decompose the 2.4 ms `appearanceUpdate` zone — what fraction is
   `TransformShape` per-vertex lighting kernel vs. the surrounding
   `TransformMultiShape` machinery (hierarchy traversal,
   `SetTextureHandle`, child shapeToWorld). Use a ZoneScopedN drill-down.
2. Grep + read the active `multiSetLightList` lighting model — light
   types supported, falloff math, light-list iteration shape. Determine
   GLSL portability.
3. Determine SSBO budget for per-vertex normals + per-frame light list.
   Per-vertex normals are already on the prior batcher's type VBO
   (`gos_static_prop_batcher.cpp` registration walks); per-frame light
   list is bounded by `eye->getWorldLights()` count, typically small.
4. Parity strategy for slice 2: bytewise comparison of CPU `argb` vs
   GPU `argb` will fail at sub-ULP FP differences. Either (a) ULP-tolerance
   compare, (b) move the parity gate from per-instance to pixel-screenshot
   (Q5(iii)), or (c) deliberately keep CPU lighting alongside GPU lighting
   for one frame of dual-emit and compare each pass-through.

**Sequence recommendation:** ship slice 1 first as substrate. Run slice 2
recon in parallel or after. Don't block slice 1 on slice 2 recon — slice 1
substrate has independent value as a Q1-bug-class fix landing pad.

### Specifically NOT in scope of any near-term work

- **Cull cascade refactor (ii-A or ii-B).** Multi-week recon work,
  no near-term return. Brainstorm separately if/when the user wants
  to pursue animated-objects-on-GPU.
- **Late `registerType` source.** Soft-fail and log; investigate if
  count is high enough to matter. Buildings are typically all
  registered at map-load; the late types are artillery/bomber
  spawns, out of scope.
- **GVAppearance / Mech3DAppearance offload.** Out of scope; they
  need per-node-animation-on-GPU which is its own design.
- **Shadow path for GPU buildings.** Out of scope; legacy CPU shadow
  path remains. Slice 2 addresses.

### Specifically pinned for surface-to-user before spec

1. **Two-slice arc framing.** Slice 1 ships substrate with no perf
   claim; slice 2 ships the actual perf win. User must accept that
   slice 1's value is a Q1-bug-class fix + substrate for slice 2,
   not a Tracy delta. Alternative: don't start the arc.
2. **Q5(iii) tooling investment.** Pinned-camera screenshot diff
   gate is new tooling; estimate ~half-day to a full day depending
   on camera-determinism work. Acceptable budget?
3. **Stale memory file.** Update `bldg_animation_lod_swap_unsafe.md`
   to reflect that the fix is on `terrain-pbr-mod`, not pending
   on `agile-hopper`. Cosmetic but worth keeping memory healthy.
4. **Save Q0 baseline as memory.** The 2.4 ms / 900 µs / 4 ms total
   numbers are load-bearing for slice 2's perf-target setting and
   for any future reasoning about object-side cost. Worth a memory
   file: `object_update_cost_baseline.md`.

### Hand-off to next session

If this brainstorm is approved with no scope changes:

- Next session: spec write at `docs/superpowers/specs/2026-05-XX-object-offload-buildings-design.md`.
- Recon may not be required if the user accepts Q2(iii) as scoped here;
  Q1-Q8 already grep-ground the cited code. Adversarial review of the
  spec mandatory per `CLAUDE.md` "Review Discipline" — this slice
  qualifies as architectural-endpoint-class because (a) it touches a
  load-bearing infrastructure (object render path) and (b) it's the
  start of an arc, so failure modes don't get caught by a follow-up
  slice.

If the user rejects Q2(iii):

- Surface alternatives. (i) is rejected because it's "the prior attempt
  with a different label." (ii-A) and (ii-B) are real options but not
  this brainstorm — they need their own brainstorm with a different
  shape (cull-cascade-refactor-first).

---

## Adversarial self-review note

Per the brief, this brainstorm runs `.claude/skills/adversarial-plan-review.md`
self-review after this section. The discipline:

- Every cited symbol grep'd at write-time. ✓ (verification appendix)
- Q1's failure analysis grounded against prior design + handoff +
  cull-cascade memory. ✓ (each finding cites file:line + commit SHA)
- Q3's lifecycle seam defended explicitly with rejection reasoning
  for (b) and (c). ✓
- Failure-Mode Countermeasure Matrix has no entries without a
  mechanism. ✓ (verified in-table)
- Open items surfaced explicitly, not buried. ✓ (closing section)

The remaining adversarial check — looking for stale prose claims about
cull-state management — was done inline at write time. Two points
required additional grep verification AFTER the prose was drafted:

1. The claim "Building::update always returns 1" was verified by
   reading `code/bldng.cpp` end of function (returns 1, no early
   false). ✓
2. The claim "mission.cpp pool init is at 3097-3110 in memory but
   actually 3140-3152 in current code" was verified by grep + read. ✓

No stale claims survive the verification appendix.
