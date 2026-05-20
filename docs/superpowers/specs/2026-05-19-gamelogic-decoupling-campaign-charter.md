# GameLogic-Decoupling Campaign Charter (2026-05-19)

Worktree: `claude/nifty-mendeleev`, HEAD 7ab53a3 (post-Phase-5 inverse-
projection consumer-collapse). Status: SYNTHESIS of two parallel
fresh-context advisor recons (terrain-indirect + cpu-gpu-offload) +
the user-captured worst-case-zoomed-out Tracy baseline. **NOT a plan -
NOT a spec for a single slice - a campaign charter.** Implementation
sessions branch from this; each slice writes its own spec.

## 1. User mandate (load-bearing)

User said verbatim 2026-05-19: *"we are modernizing this engine one
way or another"* + *"don't care if it is a ton of work to eliminate
it"*. The work in scope: retire the per-frame per-quad/per-vertex
terrain CPU walk that dominates GameLogic-thread time on worst-case-
zoomed-out captures. User's headline thesis: *"~90% of the work could
be killed if we stopped having to iterate everything every frame."*
The advisor verdict: *the arithmetic is right; the slicing is wrong* -
90% is a CAMPAIGN target, not a slice.

## 2. Baseline (USER-driven worst-case zoomed-out big-map, HEAD 7ab53a3)

| Zone | mean | P99 | P99.9 | % Frame |
|---|---|---|---|---|
| Frame | 8.05ms | 22.53ms | - | 100% |
| GameLogic (envelope) | 5.08ms | 7.87ms | 18.47ms | 63% |
| GameCamera::render activeScene | 2.62ms | 3.84ms | 13.26ms | 33% |
| **quadSetupTextures** | **1.75ms** | **2.79ms** | **13.47ms** | **22%** |
| **GameLogic.Units.TerrainObjects** | **1.17ms** | **1.89ms** | **5.57ms** | **14.5%** |
| **slimReduce** | **535us** | **837us** | **877us** | **7%** |

User-confirmed: GameLogic + render-activeScene = ~96% of Frame.

## 3. Corrected understanding (the recons' adjudicated rulings)

### 3.1 "slimReduce is worse than what it replaced" - PARTIALLY TRUE, ALREADY ADJUDICATED

- slimReduce replaced VPL (VertexProjectLoop) at commit `0c8e06b`
  (2026-05-15, the architectural endpoint of an 8-step VPL retirement).
- It DID land as a regression: commit `0c8e06b` called
  `projectForTerrainAdmission` UNCONDITIONALLY for every vert; VPL
  gated it on `onScreen`. Mean jumped 475us -> 662us.
- Commit `6c9d4b5` (same day) restored the `onScreenR` gate
  (`terrain.cpp:1780-1785`). PROJ since matches VPL's gated PROJ.
- Post-fix slimReduce ~535us at user's worst-case = the **load-bearing
  residual**, NOT a regression to revert. Decomposition:
  - PROJ (`projectForTerrainAdmission` + matmul + persp divide):
    ~47 cyc/call x ~20k surviving verts = ~120-180us. Matches VPL.
  - **CULL (`clipInfo` + `objBlockInfo[].active` + `objVertexActive[]`
    scattered stores): ~1.44M cyc/frame ~ 360-480us @ 3-4GHz.
    Camera-independent, memory-bound, NOT codegen-recoverable
    ([[slimreduce-cull-is-structural-memory-bound-floor]]).**
  - onScreenR angular x 40k verts: ~200us.
  - Step-8b raster re-home (`rv->px/py/pz/pw = sp.{x,y,z,w}`): small,
    feeds the default-dead legacy CPU raster path - **candidate
    dead-work delete pending consumer audit (see Section 7).**
- Reverting to VPL: STRICTLY WORSE. VPL had a legacy-twin parity body
  the slim form deleted; reverting also loses Phase 3-5's downstream
  deletions (`setInverseProject` + 4 fields, `inverseProjectZ`, RED
  reduction, 6 globals, water-block reductions).
- Verdict: **slimReduce's CULL IS regressed work but is structural
  (memory-bound floor); not codegen-fixable; only architectural
  decoupling kills it.**

### 3.2 "~90% killable" - CAMPAIGN-SCALE, NOT SLICE-SCALE

Honest math (both recons converge):

| Lever | Frame reclaim | Status |
|---|---|---|
| beta1 alone (slimReduce block-cull) | ~3-5% | Shovel-ready |
| beta1 + beta2 (quadSetupTextures block-cull) | ~10-14% | Both shovel-ready, one-TU-each |
| beta + Step-8b dead-work delete | +~1-2% if confirmed | Pending consumer audit |
| alpha-Stage 0 (in-flight APPROVED spec) | ~14.5% (TerrainObjects 1.17ms -> ~0) | Ready-for-impl handoff |
| alpha-Stage 1 (extend alpha to retire slimReduce) | ~7% + structural unblock | CHARTER NEEDED, catastrophic-axis |
| Combined feasible (sub-campaign) | **~25-35%** | Across multiple slices, no single-quarter blocker |
| Full GameLogic-thread decoupling | up to ~90% | **Multi-quarter CAMPAIGN** |

**90% is reachable** - over a campaign, not a slice. The bounded-effort
realistic target (alpha-Stage 0 + beta1 + beta2 + Step-8b) is **~25-35%
of Frame on worst-case-zoomed-out**, deliverable across 3-4 chartered
slices without requiring the destroy-gate decoupling.

## 4. The named meta-fix (campaign-scale)

**GameLogic same-frame-destroy contract decoupling**: move object
`update()` + destruction from "frame N synchronous gating on frame N
cull set" to "frame N decision against frame N-2 readback with bounded
lag tolerance" (per the existing `89e35ac` conservative-OR + frustum-
dilation motion-safety primitive). This is the upstream change that
retires the CPU-side cull-cascade producer's *reason to exist*. Blast
radius: every `getExists()` consumer (~hundreds of sites), `MC2_DESTROY`
semantics, mech/vehicle/building/turret/gate lifecycles. **NOT a
single slice; a multi-quarter effort.**

Carve-out invariant (catastrophic-axis,
[[cull-gates-are-load-bearing]]): the cull set MUST be a same-frame
*superset* of the camera-visible set for the destroy gate; N-1 lag = a
subset under camera motion = silent mech/building destruction. The
campaign retires this invariant by re-architecting the consumer, not
by violating the producer.

## 5. The slice decomposition (chartered separately, each writes its own spec)

### 5.1 alpha-Stage 0 - APPROVED, READY-FOR-IMPL HANDOFF

Authoritative artifact:
`docs/superpowers/specs/2026-05-18-object-prop-iteration-gpu-port-
stage0.md` + plan `2026-05-18-object-prop-iteration-gpu-port.md`.

Scope: DELETE the per-object `recalcBounds()` projection body for
terrain statics (Bldg `bdactor.cpp:~1152`, Tree `bdactor.cpp:~4278`);
REPLACE render-consumer of `inView`/`canBeSeen()` (terrobj.cpp:~796 +
:~866) with `gpu_cull::readback_isActorVisibleLagged(handle)` w/
conservative-OR + dilation. KEEP UNTOUCHED: the slimReduce cull
cascade; the lifecycle gate (`recalcBounds` return gating
`appearance->update()`); `screenPos` consumers (mouse-pick selection).

Target: `GameLogic.Units.TerrainObjects` 1.17ms -> ~0 (~14.5% Frame).

Status: spec approved, plan written. Preconditions all satisfied
(VPL retirement complete `0c8e06b`/`5296de4`; motion-safety primitive
shipped `89e35ac`; readback API wired default-off objmgr.cpp:~1930
under `MC2_GPU_CULL_LIFECYCLE`; substrate emits per-frame visibility
records via `emitGpuCullRecord` objmgr.cpp:~2065). Sized against 1.43ms
worst-case (paired prior captures).

### 5.2 beta1 - slimReduce block-cull (shovel-ready, one-TU)

Build per-block AABB array at mission load (`verticesBlockSide=20`,
`blocksMapSide^2 = numObjBlocks`, reuse `vertex->getBlockNumber()`
vertex.h:149). Block-frustum test fronts terrain.cpp:1685 vertex loop:
- OUT blocks: range-zero `objVertexActive[firstVert..lastVert]` +
  `objBlockInfo[b].active = false` (memset-friendly contiguous range).
- IN blocks: walk verts as today (full cull writes).
- BOUNDARY blocks: walk verts as today (per-vert onScreenR descent).

Projected ~35-47% slimReduce reduction at ~50% frustum-occupancy
zoomed-out. Substitutive done-criterion: byte-parity probe on
`objBlockInfo[]` + `objVertexActive[]` arrays vs current producer
across a camera sweep; measurable Tracy delta from 535us baseline.

Carve-outs (catastrophic-axis):
- ortho/`!clipUsesOnScreen` branch must still walk per-vert (clipR
  depends on projection); preserve.
- Gate is fail-OPEN at block level: over-include is safe; under-include
  is catastrophic ([[cull-gates-are-load-bearing]]).
- MUST measure at user-driven worst-case zoomed-out
  ([[zoomed-out-big-map-is-an-unexercised-stress-path...]] - tier1 is
  structurally blind to this slice; recurred 3x 2026-05-15).

### 5.3 beta2 - quadSetupTextures block-cull (shovel-ready, one-TU)

Same block infrastructure as beta1. Front terrain.cpp:1918 (`for (i=0;
i<numberQuads; i++) currentQuad->setupTextures();`): skip whole 400-
quad range per OUT block; walk per-quad for IN/boundary. Note: this is
DIFFERENT from the slice-1+2 efforts that tried to retire the *inner*
recipe-shuttle in `setupTextures`'s body (exhaustively falsified by
[[setuptextures-is-a-multiwriter-tangle-not-a-clean-shuttle]] +
`docs/superpowers/specs/2026-05-19-slice1-postmortem.md`). beta2 does
NOT retire any inner work; it skips the function call entirely for
OUT-block quads.

Projected ~45-55% quadSetupTextures reduction at zoomed-out (~800us-
1ms saved).

### 5.4 alpha-Stage 1 - REQUIRES NEW CHARTER

Extension of alpha-Stage 0 to retire the slimReduce cull-cascade
producer. Drive `objmgr.cpp:2040`'s `MC2_DESTROY` destruction gate off
the same `gpu_cull::readback_isActorVisibleLagged()` (with conservative-
OR + dilation motion-safety). Requires either:
- (a) Re-architecting object update to not require a per-frame
  visibility set (objects update unconditionally; visibility gates
  only render/shadow; destruction decoupled from visibility), OR
- (b) Splitting consumers by latency-class: render/shadow consume N-2
  lagged GPU set; update keeps a cheap same-frame CPU coarse-cull
  fallback (the beta1 block-cull serves nicely).

(b) is the more tractable evolution of the existing system; (a) is the
end-state cleanup. (b) likely first, (a) over time.

Blast radius: every `getExists()` consumer; `MC2_DESTROY` semantics;
mech/vehicle/building/turret/gate lifecycle interaction with the cull
set. Catastrophic-axis review mandated (any subset-under-motion is the
silent-destruction canary failure).

Targets: slimReduce 535us -> ~0; the structural memory-bound CULL
floor is killed (not patched). Plus the strategic gain: GameLogic
becomes decoupled from per-frame terrain compute = unblocks further
multi-thread / GPU-driven evolution.

### 5.5 Step-8b dead-work audit (small parallel task)

Grep `rv->px|rv->py|rv->pz|rv->pw|->px\b|->py\b|->pz\b|->pw\b` across
mclib/code/GameOS. If only the legacy CPU raster path (default-dead
per `60f2ef8` drawPass-retirement default-ON flip) reads them:
- Delete the Step-8b raster re-home (terrain.cpp:1841-1853, the four
  stores per onScreenR vert).
- The `projectForTerrainAdmission` call in slimReduce may also become
  deletable (if NO consumer outside Step-8b + the deleted RED reduction
  needs `sp`) - which would retire the entire PROJ bucket (~120-180us)
  of slimReduce, leaving only the CULL bucket as the residual.

Small effort, high-leverage if confirmed. Could be done in any of the
sub-slices or as its own thin slice.

## 6. Sequencing recommendation

Per user mandate ("modernize one way or another"), recommended order:

1. **alpha-Stage 0 NEXT** (spec approved, ready, bounded effort,
   retires 14.5% Frame). Single biggest atomic win available right
   now.
2. **beta1 + beta2 in parallel or right after** (one-TU-each, shovel-
   ready, ~10-14% Frame combined). Different files from alpha-Stage 0
   (terrain.cpp/quad.cpp for beta vs objmgr.cpp/bdactor.cpp/terrobj.cpp
   for alpha-Stage 0); concurrent sessions can do them in parallel.
3. **Step-8b dead-work audit** any time (low effort, high information
   value - if positive, it changes beta1 design + reduces slimReduce's
   PROJ bucket).
4. **alpha-Stage 1 chartered** after beta lands (or in parallel if the
   team has bandwidth). The catastrophic-axis review must precede ANY
   code; recon-extension required.
5. **Future quarters**: full GameLogic-thread decoupling as the
   campaign endpoint (`getExists()` consumer migration + `MC2_DESTROY`
   contract redesign).

## 7. Open questions for the next implementing session

1. **beta1 block-frustum test composition with onScreenR**: do they
   share predicate math cleanly, or does the block test need a
   different (e.g., AABB-vs-frustum-planes) formulation? Quick
   grep-and-prototype answers this.
2. **Step-8b consumer audit**: where exactly are `rv->px/py/pz/pw`
   read? Default-dead-only, or live readers exist?
3. **alpha-Stage 1 destroy-gate decoupling**: is the 89e35ac
   conservative-OR + frustum-dilation primitive sufficient for
   objmgr.cpp:2040's MC2_DESTROY semantics under camera motion? Or
   does the destroy gate need a separate same-frame cheap CPU coarse-
   cull fallback (which beta1 conveniently provides)?
4. **Tracy P99.9 fat-tail on quadSetupTextures (13.47ms)** - what
   causes it? GPU-readback stall? GC-style hitches? Map-edge
   transitions? Worth a separate investigation thread.
5. **alpha-Stage 0 measurement gate at HEAD 7ab53a3**: the spec was
   sized against 1.43ms `TerrainObjects` from a 2026-05-18 capture.
   Current user Tracy shows 1.17ms - is the spec's sizing still valid,
   or has the residual shifted enough to re-size?

## 8. References

- alpha-Stage 0 spec: `docs/superpowers/specs/2026-05-18-object-prop-
  iteration-gpu-port-stage0.md`
- alpha-Stage 0 plan: `docs/superpowers/plans/2026-05-18-object-prop-
  iteration-gpu-port.md`
- alpha-Stage 0 open-questions resolution:
  `docs/superpowers/specs/2026-05-18-object-prop-iteration-open-
  questions-resolution.md`
- Slice-1+2 postmortem (the 6 collisions, the corrections):
  `docs/superpowers/specs/2026-05-19-slice1-postmortem.md`
- Inverse-projection consumer-collapse spec (this branch's just-
  completed work): `docs/superpowers/specs/2026-05-19-inverseproject-
  consumer-collapse-design.md`
- VPL retirement plan: `docs/superpowers/plans/2026-05-14-vertex-
  project-loop-retirement.md`
- Memories: `setuptextures_is_a_multiwriter_tangle_not_a_clean_
  shuttle.md` (CORRECTION block), `HANDOFF_actual_terrain_perframe_
  cull_fix.md`, `cull_gates_are_load_bearing.md`,
  `slimreduce_cull_is_structural_memory_bound_floor.md`,
  `zoomed_out_big_map_is_an_unexercised_stress_path...md`,
  `vertexproject_loop_asymptotic.md`,
  `water_invproj_parity_is_DIVERGENT_not_freebie.md`,
  `feedback_class_layout_change_needs_clean_first.md`,
  `feedback_offload_must_be_substitutive_not_additive.md`,
  `cost_split_instrumentation_is_observer_effect_dominated.md`.
- Phase 1-5 commits (the just-completed prerequisite cleanup):
  `1b9a9e4` (Phase 1 surgical) `762609b` (Phase 2) `6d61801` (Phase 3)
  `5324caa` (Phase 4) `7ab53a3` (Phase 5).

## 9. Greybeard verdict (both advisors converged)

`META-FIX` named: GameLogic same-frame-destroy contract decoupling
(campaign-scale, multi-quarter). `PATCH (justified)` for beta1+beta2
and alpha-Stage 0 as bankable substitutive sub-slices that don't
require the campaign-endpoint change. **The 90% Frame-reclaim target
is rejected as a slice gate** - it is a campaign target. The session
that pursues alpha-Stage 1 / destroy-gate decoupling will need fresh-
context advisor + greybeard + adversarial-plan-review at catastrophic-
axis level; the cheap-consumer-rearch paths are exhaustively known
dead this session (slice-1+2 postmortem; do not re-derive).
