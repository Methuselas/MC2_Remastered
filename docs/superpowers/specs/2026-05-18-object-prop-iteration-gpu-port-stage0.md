# Object/Prop Iteration GPU Port - Stage 0 Design Contract

Date: 2026-05-18
Branch: claude/gpu-driven-rendering (HEAD 38f2b7a at write)
Status: Stage-0 contract APPROVED-pending measurement gate. NOT sized/planned
until the clean worst-case-zoomed-out Tracy resolves 913us-vs-1.43ms.

> Grounding discipline: trust commits/code over docs/plans/snapshots. The
> render-perf-snapshot is stale on VPL ("PLAN DRAFTED") - commits prove VPL
> retirement COMPLETE. Re-grep every file:line at plan-write; symbols stable,
> lines drift.

## What this is

The VPL retirement arc's explicitly-named, now-UNBLOCKED downstream:
"object/prop iteration GPU port". It eliminates the residual ~1.3ms
`GameLogic.Units.TerrainObjects` (objmgr.cpp ~:1967) that survives after the
shipped lighting bake (2db2a04 + C6 4fed1b6 + LIGHTBRIDGE 2dca942 - the
lighting half is DONE and confirmed substitutive; this is NOT lighting).

## Substitutive contract

- DELETE the per-object `recalcBounds()` projection body for terrain statics
  (Bldg `bdactor.cpp:~1152`, Tree `bdactor.cpp:~4278`): camera-frame
  transform + sphere-clip + `projectForScreenXY` + selection-box build, run
  per object per active block via `objList[objIndex]->update()`
  (objmgr.cpp:~2051 -> terrobj.cpp:~694).
- REPLACE the render-consumer of `inView`/`canBeSeen()` (terrobj.cpp:~796
  render, :~866 renderShadows) with the already-shipped GPU readback
  `gpu_cull::readback_isActorVisibleLagged(handle)` (gpu_cull_readback.h:~82;
  snapshot built at objmgr.cpp:~1911; key = `obj->getHandle()`). The substrate
  already emits these objects' visibility every frame
  (`emitGpuCullRecord` objmgr.cpp:~2065) - the CPU recalcBounds is the
  redundant twin.
- KEEP UNTOUCHED: the cull cascade (`slimReduce` owns
  `objBlockInfo[].active`/`objVertexActive[]`, terrain.cpp:~1741/1744,
  byte-identical to terrain.cpp:~2061/2075 per 63a0b3e); the lifecycle gate
  (terrobj.cpp:~694 `recalcBounds` return gating `appearance->update()` -
  cull_gates_are_load_bearing + the objmgr.cpp:~2024-2031 "UNSAFE to
  inner-gate buildings/turrets" comment); `screenPos` consumers
  (mouse-pick/selection bdactor.cpp:~1246).

## Path-A 1-frame-lag avoidance (mandatory mechanism)

The VPL arc rejected naive GPU-port+readback (1-frame-lag bug class,
substrate_coalesce_sync_point_lesson). This slice consumes a LAGGED readback
so lag is intrinsic and must be made motion-safe via the already-shipped
89e35ac primitive: `gpu_cull.comp`/`gpu_cull_predicate.glsl` frustum
DILATION + `readback_isConservativeOrEnabled()` conservative-OR merge
(gpu_cull_readback.cpp:~71/85). Contract: over-inclusion is correctness-safe
(one wasted draw); false-negative (dropped prop) is catastrophic. Requires
`gpu_cull::readback_isEnabled()` (else fail-open all-visible = safe
degradation, stock_install_must_remain_playable). Conservative-OR+dilation
MUST be on for the render repoint (visible-artifact surface). This is a
SEPARATE render-consumer flip, NOT a reuse of `MC2_GPU_CULL_LIFECYCLE`
(objmgr.cpp:~126, different safety contract).

## Preconditions

Commit-verified satisfied: (1) VPL retirement complete (0c8e06b deletes VPL
body, 5296de4 Step 10); (2) slimReduce owns cull-cascade writes (63a0b3e);
(3) motion-safety primitive shipped (89e35ac); (4) readback consumer API
wired default-off (objmgr.cpp:~1930 under `MC2_GPU_CULL_LIFECYCLE`).
NOT satisfied - BLOCKING: (5) measurement gate - the user-captured
1.53/1.43ms vs post-C6 ~913us discrepancy must be resolved by a clean
worst-case-zoomed-out non-COST_SPLIT Tracy before the slice is sized.
Treat as a Stage-0.5 blocking gate.

## Substitutive proof gate (contamination-immune)

1. User-driven, non-`MC2_TERRAIN_COST_SPLIT`, total-frame Tracy,
   worst-case zoomed-out big-map: `GameLogic.Units.TerrainObjects`
   self-time -> ~0 AND total frame drops (anti-mirage: zone->0 alone is
   never the proof).
2. Logic/counter superset parity (NOT chrono): the readback-visible set
   must be a SUPERSET of legacy `inView` for every terrain static; zero
   `(legacyCanBeSeen && !readbackVisible)` violations; legitimate
   over-inclusion expected. Demote-don't-delete.
3. Visual canary: no prop pop/vanish on fast zoomed-out pan (Fix-A
   ghost/streak class = CRITICAL).
4. Adversarial review MANDATED (architectural endpoint + cull/visibility
   consumer chain). Dispatch prompt MUST contain verbatim "use the
   adversarial-plan-review skill". ~3 design + ~2 plan rounds, alternating
   opus/sonnet.

## Open questions for the plan

1. Render repoint site: inside `canBeSeen()` (appear.h:~176, broad blast
   radius, all appearance types) vs a terrain-static-specific gate at
   terrobj.cpp:~796/866 (narrow, safer) - plan must specify exactly which
   bdactor.cpp lines are deleted vs kept-for-`screenPos`.
2. What feeds `screenPos` after the projection delete (mouse-pick needs
   projected coords): lazy per-selected/under-cursor projection vs defer to
   the queued `gpu_mech_aware_mouse_pick` AABB precursor. Likely CRIT-1
   (analogue of VPL Step 6 "re-home the survivor").
3. Whether the lifecycle gate at terrobj.cpp:~694 needs any change (if
   `recalcBounds` keeps writing `inView` via the matrix-free angular
   sphere-clip and only the projection is skipped, lifecycle is
   undisturbed - quantify projection vs angular-clip split, RDTSC
   `[SLIMSPLIT v1]` pattern terrain.cpp:~1719 is the precedent).
4. Measurement-gate dependency (precondition 5) is a Stage-0.5 blocking
   sizing gate.

## Scope

Terrain statics only (Bldg/Tree). Movers (mech/GV/artillery/turret) share
`recalcBounds` but are a different consumer chain and a different slice
(gpu_mech_aware_mouse_pick_queued.md). Multithreading/job-system is OFF the
table (no job system exists; explicitly SUPERSEDED - "VPL retired not
parallelized").
