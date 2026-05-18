# Static Decorative Elimination - Design

Date: 2026-05-17
Branch: claude/nifty-mendeleev
Status: Approved for Stage 0 after edits (user spec review 2026-05-17)

> All `file:line` anchors below were a starting point at write-time. Per the
> worktree Documentation Discipline, every cited symbol MUST be grep-confirmed
> at plan-write time; symbols are stable, line numbers drift. Anchors verified
> first-hand this session are marked (V); anchors from the slice3c landed-state
> audit are marked (A) and must be re-grepped before the plan.

## 1. Problem

Registered static decoratives (trees/rocks; `terrainObjects=2445/2445` at
mission load) are no longer run through the heavy `appearance->update()` path
-- `MC2_STATIC_UPDATE_SKIP` defaults on and routes ~99.99% to
`appearance->touch()` (`terrobj.cpp:738` V, default `terrobj.cpp:92` A). But
this is **work relocation, not elimination**:

- The CPU still iterates all registered statics every frame to evaluate the
  skip predicate (`objmgr.cpp:2004-2036` V; `[STATIC_UPDATE v1]`
  `delta_seen` ~433K / 600 frames).
- `touch()` (`ResubmitCachedGpuLightData` + `Touch`) still runs per prop
  per frame.
- `markVisible()` is still pushed per visible prop per frame.
- The registry `flush()` per-live-range loop is itself a large per-frame CPU
  cost: on the sibling `gpu-driven-rendering` branch the Tracy zone
  `Render.GpuStaticProps` (txmmgr.cpp:2050) measures ~598 us self +
  `GpuStaticProps.Flush` ~193 us per frame. The nifty-mendeleev analog is the
  `flush()` per-live-range loop (`gos_static_prop_registry.cpp:311/338` A).

The standard being applied (`feedback_offload_must_be_substitutive_not_additive`):
the slice only counts when the per-frame CPU work for decoratives is **absent
from the object/update/render coupling**, not made cheaper inside it.

## 2. Scope

In scope: pure-static decoratives only -- trees/rocks with zero per-frame
logic.

Out of scope (decomposed to follow-on specs):

- Buildings (140), turrets (26), gates -- they have load-bearing per-frame
  logic even when offscreen (turret tracking, gate logic, power supply), per
  the `objmgr.cpp:1996-2003` (V) safety contract. Separate spec.
- The 18 currently-unregistered objects (`buildings skip=12`,
  `turrets skip=6`) stay on the normal dynamic path. Accepted residual, not a
  regression -- they were never eliminated. They MUST be excluded from both
  the denominator and the zero-counter, or reported separately, so they do not
  create fake regressions.

## 3. Approach (selected: A - severance at the source)

Decoratives are lifted out of the terrain-block `objList`/`objBlockInfo`
working set at mission load into a dedicated `StaticDecorativeSet` that the
`objmgr.cpp:2004-2036` loop has no path to. `seen` for decoratives goes to
**0 by construction**, not by a faster skip.

Rejected alternatives:

- B (partition + single bounds-skip): keeps `objList` entanglement; weaker
  guarantee (any new consumer iterating `objList` silently re-touches
  decoratives); not truly severed.
- C (cheaper skip predicate): this is today's architecture and the exact
  "relocation not elimination" trap. Named only to rule it out.

## 4. Architecture and components

`StaticDecorativeSet` extends the existing `GpuStaticPropRegistry`
(`gos_static_prop_registry.{h,cpp}` A) -- widen its contract, do not
duplicate it. Three units, each independently testable:

- **`StaticDecorativeSet` (CPU, mission-lifetime)** -- authoritative list of
  decorative instance records (transform, AABB, LOD ranges, material/texture
  slot, baked light). Built once at mission load; mutated only on discrete
  deregister events. No dependency on per-frame state -- the load-bearing
  invariant.
- **GPU resident static data + mutable liveness state** -- instance SSBO,
  per-LOD index/vertex ranges, indirect-draw command buffer. Geometry, LOD
  ranges, material slots, and baked-light records are **immutable after
  mission load**. Liveness/generation/tombstone state is the **only mutable
  GPU-resident field**, and may be updated only by `deregister(handle)`.
- **Per-frame submit path (fixed CPU work)** -- enqueue one compute-cull
  dispatch over the instance SSBO (frustum + distance->LOD, writing the
  indirect buffer), then one `glMultiDrawElementsIndirect`. Replaces the
  per-decorative `markVisible()` push, the per-decorative `emitGpuCullRecord`,
  and the `flush()` per-live-range loop.

**Per-frame cost contract:** CPU work is O(1) for the whole set: enqueue one
compute cull dispatch and one indirect draw. GPU work remains proportional to
decorative instance count, but is data-parallel and has no CPU per-instance
replay.

**Boundary contract:** the only ways into `StaticDecorativeSet` after mission
load are `deregister(handle)` (discrete event) and the GPU read. The
`objmgr.cpp:2004-2036` loop has no path to a decorative.

## 5. Data flow and lifecycle

**Mission load (once):**

1. The existing `registerStatic` walk (`bdactor.cpp:5044` A) classifies
   decoratives (pure-static trees/rocks; not buildings/turrets/gates; not the
   18 unregistered).
2. Bake instance record (transform from fixed position/rotation, AABB, LOD
   ranges, texture slot, `CacheGpuLightData` result) into the
   `StaticDecorativeSet` CPU array; upload immutable fields to the instance
   SSBO.
3. Bake decorative shadow contribution into the static world-fixed shadow map
   (Section 7).
4. Remove decoratives from `objList`/`objBlockInfo` working set so
   `objmgr.cpp:2004-2036` cannot reach them.

**Per frame (fixed CPU work for the whole set):**

- One compute dispatch: frustum + distance->LOD over the instance SSBO ->
  writes indirect command buffer. No readback (no sync stall; honors
  `substrate_coalesce_sync_point_lesson`).
- One `glMultiDrawElementsIndirect`, post-`renderLists` hook
  (`render_order_post_renderlists_hook`).
- Eliminated: per-decorative `markVisible()`, per-decorative
  `emitGpuCullRecord`, the `flush()` per-live-range loop (the
  ~800 us `Render.GpuStaticProps` + `Flush` class of cost).

**Discrete events only:**

- *Destroy/fall:* collision/damage callback (trees `terrobj.cpp:352-353` A;
  buildings out of scope) resolves `decorativeHandle + generation` and calls
  `StaticDecorativeSet::deregister`, frees the instance slot (one liveness
  write), re-admits the object to the normal dynamic `objList` path for its
  fall/death animation exactly once, then it dies normally.
- *LOD:* distance->LOD resolved GPU-side in the cull compute (instance carries
  all LOD ranges). No CPU `needsFullBakeNextFrame` re-bake bursts for
  decoratives.

**Collision/damage routing requirement (hard):** severed decoratives must
retain a collision/damage proxy or handle map **outside `objList`**. A hit
must resolve `decorativeHandle + generation` to the `StaticDecorativeSet`
slot, call `deregister`, and spawn/re-admit the dynamic fall/death object
**exactly once**. The route must not implicitly rely on normal `objList`
traversal.

## 6. Cull-gate cascade disposition

`cull_gates_are_load_bearing` gates five things. Disposition for decoratives:

1. **Per-object `update()`** -- eliminated; decoratives have no per-frame
   logic by definition.
2. **Lifecycle (`MC2_DESTROY` on update-false)** -- does not apply; a
   decorative leaves the set only via the explicit deregister event.
   Destruction is event-sourced, not poll-sourced.
3. **TGL pool budget** -- decoratives no longer draw through the TG_Shape pool
   path; pool pressure drops (`tgl_pool_exhaustion_is_silent` risk reduced).
4. **Per-instance state refresh (`TransformMultiShape`)** -- eliminated;
   transform is baked, invariant.
5. **Manual projection rhw guard / `clip_w_sign_trap`** -- handled GPU-side in
   the cull predicate (`gpu_cull_predicate` convention, `pz in [0,1)`, never
   `sign(clip.w)`).

**Staleness frame-stamp (`gos_static_prop_registry.cpp:338` A,
`getCachedFrame() != currentFrame`):** this gate exists only to skip ranges
whose CPU `update()` was cull-skipped -- a workaround for the additive
coupling. For a severed decorative there is no CPU update to be stale against;
the GPU cull is authoritative. **The frame-stamp gate is removed from the
decorative path entirely** (not refreshed differently -- removed, because the
reason it existed is gone). This deletes the `black_tree_bug` class at its
source rather than papering over it.

**Stage 0 boundary proof (hard gate before code):** enumerate every consumer
that resolves an object by `objList` handle and prove none reaches a
decorative after severance, using opposite-direction grep (grep the consumer,
not the obvious name; `feedback_data_flow_audit_asymmetry`). Dangerous
consumer classes that MUST each be proven clean: render/update, collision,
damage, script triggers, mission objectives, save/load,
selection/targeting, pathing/blocking, fog/LOS/reveal, audio/event emitters,
cleanup/destruction, network/replay (if applicable), and any
handle-to-object lookup helpers.

## 7. Shadow bake

Sun is static per mission (confirmed) -> a decorative's shadow is invariant ->
bake once.

- At mission load, after the instance bake, render decoratives into the
  static world-fixed shadow map (the "design ready" known-issue artifact).
  This replaces the stubbed `GpuStaticPropBatcher::flushShadow()`
  (`gos_static_prop_batcher.cpp:3567` A) for the decorative set -- the stub is
  resolved by removal of need, not by writing a per-frame shadow draw (which
  would be the same relocation trap).
- Per frame: decorative shadows are sampled from the static map; zero
  per-frame shadow cost for the set.
- **Caster LOD determinism:** shadow bake never distance-rejects decorative
  casters. Caster mesh selection is deterministic and mission-load stable:
  either conservative/highest acceptable caster LOD, or a documented
  light-space/static-shadow LOD rule. It MUST NOT depend on the runtime
  camera. (`distant_buildings_render_at_lower_lod_never_distance_culled`:
  distant decoratives still cast, at the deterministic caster LOD.)
- **Interaction:** dynamic casters (mechs) still use the dynamic shadow map;
  the static map composites under them as today. The static/dynamic split
  stops being a CPU-cost workaround and becomes a deliberate correct two-map
  design (`shadow_static_dynamic_split_was_cpu_terrain_cost_workaround` -- the
  split is now justified, not legacy debt).

## 8. Validation contract

Fresh Tracy is contaminated (parallel agent sessions); the gate is
contamination-immune by construction.

**Primary gate -- logic counter (proof of elimination):** extend the
`[STATIC_UPDATE v1]` summary with `decoratives_seen_in_objmgr_loop`. Pass
criterion: **exactly 0** over a full tier1 run (every mission, full
duration). Logic invariant, not a timing delta. `> 0` (excluding the 18
known-unregistered residuals, which are reported separately) = severance
leaked = hard fail.

**Secondary gate -- dual-output bit-identity parity:** env
`MC2_DECOR_PARITY=1` runs the legacy CPU path alongside and compares a
**canonical packed parity record** (not raw engine structs): all padding
zeroed, field order and byte width fixed, every source field initialized
before comparison. Compared quantities: the packed instance record
(matrix / fog / highlight / lightDataIndex) plus per-leaf world AABB, against
the baked record. Both sides CPU-from-identical-inputs -> exact compare valid,
no FP carve-out. Pass: zero mismatch over tier1 round-robin sampling
(1 prop/type/frame, cumulative coverage). Catches a wrong bake the counter
alone would not (`parity_finds_gpu_substrate_bugs_visual_smoke_misses`).

**Coverage accounting (blocks default-on):** parity output reports total
decorative archetypes, total instances, covered archetypes, covered LODs,
covered material/texture slots, covered light-data buckets, and remaining
uncovered cases. Default-on is blocked until coverage is complete or each
exclusion is explicitly waived.

**Error handling / failure modes:**

- Bake failure (SSBO alloc, BAR budget) -> fail closed: decorative stays on
  legacy CPU path. Log `[DECOR v1] event=bake_fail` with: mission,
  object/archetype id, handle, failure class, requested bytes, remaining
  budget, texture/material slot, and whether fallback entered the legacy
  path. The zero-counter will then be `> 0` and fail the gate (no silent
  degradation -- intended during development).
- Deregister race (hit on the same frame as cull dispatch) -> instance slot
  uses generation/tombstone so a freed slot draws nothing that frame rather
  than a stale mesh; the dynamic re-admit animates the fall.
- Pause/unpause -> `StaticDecorativeSet` is mission-lifetime and independent
  of `mcTextureManager->update()` eviction, so the `pause_unpause_diagnostic`
  class cannot occur for decoratives (structurally fixed; side benefit).
- Killswitch `MC2_STATIC_DECOR_GPU=0` restores the legacy path bit-for-bit
  (demote-not-delete; legacy body retained as retirement telemetry per
  `debug_instrumentation_rule`).

## 9. Staging and ship posture

- **Stage 0:** this spec + boundary proof (Section 6 consumer enumeration) +
  adversarial review.
- **Stage 1:** `StaticDecorativeSet` + SSBO scaffold; output unused.
- **Stage 2:** bake + cull/draw + parity. **Stage 2 parity runs BEFORE
  severance** and must reach zero mismatch with decoratives still in
  `objList` (dual-output, legacy authoritative).
- **Stage 3:** severance flip -- decoratives leave the `objmgr` working set;
  killswitch `MC2_STATIC_DECOR_GPU` default-off. **Stage 3 zero-counter gate
  runs AFTER severance.** The two gates do not blur: Stage 2 proves the bake
  is correct; Stage 3 proves the severance is structural.
- **Stage 4:** soak-substitute (below).
- **Stage 5:** default-on flip (env semantics invert; explicit `0` opts out).
  Blocked until coverage accounting (Section 8) is complete or waived.
- **Stage 6:** demote-not-delete the legacy path.

**Soak-substitute** (`feedback_soak_waiver_with_probes_and_reviews_validated`):
no calendar soak. Substitute = env-gated parity probe + the zero-counter gate
every tier1 run + **two-gate adversarial review** (design-delta, then
implementation; alternate opus/sonnet) because this is an
architectural-endpoint legacy-retirement slice. Dispatch prompts say
"use the adversarial-plan-review skill" verbatim per worktree Review
Discipline.

**Smoke gate:** default tier1
(`--tier tier1 --duration 30 --kill-existing`). Substrate must be ON for
static-prop visual validity (`substrate_off_renders_no_static_props`).

## 10. Open questions for the plan

- Exact `objList`/`objBlockInfo` removal mechanism that keeps block
  bookkeeping (`firstHandle`/`numObjects`) consistent for the surviving
  non-decorative objects in the same block.
- Generation/tombstone width and slot-reuse policy in the instance SSBO.
- Whether the collision/damage proxy is a separate spatial structure or a
  handle map keyed off the existing terrain-object spatial index.
- Canonical parity-record layout (field list + packing) -- to be fixed in
  the plan and shared C++/GLSL per `cpp_glsl_ubo_struct_lockstep` if any part
  is GPU-read.
