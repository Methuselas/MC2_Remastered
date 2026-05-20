# F3 — CPU projection cost baseline (measurement-only)

Date: 2026-05-20. Worktree: `claude/nifty-mendeleev` @ `09a47ac`.
Status: DRAFT (pre-review). Measurement slice. No consumer retired,
no shader touched, no architectural change. Reverts cleanly.

## 1. Purpose

Establish empirical per-frame CPU cost of the projection / clip /
camera pipeline so the larger GPU-driven-everything arc can be sized,
sequenced, and falsified against a fixed budget. The arc framing is
in this conversation's transcript and in the unified-projection DRAFT
(`2026-05-20-unified-projection-meta-fix-design.md`); this spec is its
F3 foundation slice.

**Budget commitment:** total CPU projection cost must fit under 100us
per frame at worst-case camera, OR the work gets evicted to GPU. This
slice produces the number that adjudicates that test.

## 2. What this is NOT

- Not an architectural change. No consumer is retired. No shader
  touched. No matrix convention changes.
- Not a Tracy refresh. Existing Tracy zones stay; we add coarse
  cost-split scopes that mirror or extend them.
- Not a sizing of any specific F1/F2 slice. It produces the data that
  sizes those slices; it does not commit to them.
- Not a per-element instrumentation. Per
  `cost_split_instrumentation_is_observer_effect_dominated.md` and the
  worktree CLAUDE.md "100 ns floor" rule, ALL scopes are coarse —
  one per subsystem per frame, at the outer-call boundary.

## 3. Provenance

- Brainstorming conversation 2026-05-20 (late). User framing: eliminate
  per-frame CPU work; CPU projection is the structural blocker that
  has stopped terrain-as-surface and gameLogic-reduction efforts twice.
- Memory: `cost_split_instrumentation_is_observer_effect_dominated.md`
  (2026-05-16 terrain CPU campaign chased phantom 7.3ms because cost-split
  scopes wrapped per-quad bodies; ~5ms was pure timer overhead).
- Memory: `feedback_cost_split_worst_case_camera.md` (cost-split numbers
  only meaningful at corner-of-map zoomed-out, NOT mission-start default).
- Memory: `capped_fps_is_not_a_cpu_cost_ab_signal.md` (frame-rate cap
  hides CPU-time changes; measure CPU zone time directly, not FPS).
- Memory: `gpu_offload_must_be_substitutive_not_additive` (CPU zone
  DEATHS measure substitutive offload, not GPU additions; this baseline
  is the substitution target).

## 4. Design

### 4.1 Env gate and reporting

- Env: `MC2_CPU_PROJ_COST_SPLIT=1`. Default OFF. Banner
  `[INSTR v1] enabled: CPU_PROJ_COST_SPLIT` at startup (per the
  existing `[INSTR v1]` convention; see CLAUDE.md "Tier-1
  instrumentation env vars").
- Output: per-frame totals printed every **500 frames** to stderr
  under `[CPU_PROJ v1]` header. Format:

  ```
  [CPU_PROJ v1] frame=N  total=XXXus  budget=100us  status=PASS|OVER
    matrix_build         Yus  (work=1)
    recalcBounds_total   Yus  (n_visible=NNN)
    tgl_transform        Yus  (n_multishapes=NN, n_verts=NNNN)
    mlr_total            Yus  (n_prims=NNN)
    skinning_chain       Yus  (n_nodes=NNNN)
    projectZ_eventdriven Yus  (n_calls=NN)
  ```

- Always-on (independent of env gate): a once-per-1500-frames
  `[CPU_PROJ v1 monotonic]` summary line with running totals
  (matches the `MC2_TGL_POOL_TRACE` always-on cadence pattern; this
  catches the case where we forgot to set the env var).

### 4.2 Buckets

ALL buckets use one `std::chrono::steady_clock::now()` pair per
SUBSYSTEM per FRAME, at the outer-call boundary. Never per element.
Each bucket also accumulates a workload counter so the data is
extrapolable to different scene complexity.

| Bucket | Wraps | Workload counter | Existing Tracy zone? |
|---|---|---|---|
| `matrix_build` | `Camera::update()` body in `mclib/camera.cpp:1572` — the per-frame camera state update where `cameraToClip` is composed | 1/frame | No (Tracy zone `Camera.MotionTracking` at `camera.cpp:1749` covers motion, NOT matrix build) |
| `recalcBounds_total` | OUTER dispatcher loop in `ObjectManager::render()` that visits each visible object's `Appearance::recalcBounds()` — NOT the per-object call | `n_visible_objects` | No |
| `tgl_transform` | OUTER call to `TG_MultiShape::TransformMultiShape` (each is one full multishape's CPU vertex transform) — `mclib/msl.cpp` per-multishape entry point | `n_multishapes`, `n_verts_total` | Adjacent zone `TG.MultiShape.PerShapeLoop` at `mclib/msl.cpp:1452`; cost-split mirrors this boundary |
| `mlr_total` | Per-frame MLR clip work — wrap from `theClipper->StartDraw(...)` at `code/gamecam.cpp:142` through `theClipper->RenderNow()` at `code/gamecam.cpp:269`. Captures matrix setup + farClipReciprocal compute + all per-prim CPU clipping during the FX/MLR render block | `n_prims_clipped` — add a static counter inside `MLRClipper` incremented at the per-primitive dispatch site (grep for the per-prim clip entry within `mclib/mlr/mlrclipper.cpp`; bumped exactly once per primitive submitted to the clipper). Reset at frame boundary alongside other buckets | No |
| `skinning_chain` | Mech3D per-frame node-chain compose. Adjacent existing zones `Mech3D.UpdateGeometry.AnimPose` (`mech3d.cpp:3380`), `.BodyXform` (`:3451`), `.Arms` (`:4523`). Cost-split aggregator wraps the OUTER `Mech3D.UpdateGeometry` dispatcher at `mech3d.cpp:3205` so all sub-zones aggregate into one bucket | `n_nodes_processed` | Outer zone `GameLogic.Mech3D.UpdateGeometry` at `mech3d.cpp:3205` |
| `projectZ_eventdriven` | Sum across all `Camera::projectZ()` callers OUTSIDE the per-object render loop — ray pick, AI LOS, mouse hit-test, click-to-world. Implementation: aggregate counter inside `projectZ()` itself, with a TLS flag set by the render-loop entry/exit to distinguish render-loop calls (counted in `recalcBounds_total`) from event-driven calls (counted here) | `n_calls` | No |

### 4.3 Cross-validation against Tracy

Tracy zones at the same boundaries (extend existing zones where they
already match the boundary; add new zones where they don't):

- `CPU_PROJ.matrix_build` — NEW, in `Camera::update()` around the
  matrix composition section.
- `CPU_PROJ.recalcBounds_total` — NEW, in `ObjectManager::render()`
  around the dispatcher loop.
- Reuse existing `TG.MultiShape.PerShapeLoop` for `tgl_transform`
  (boundary matches).
- `CPU_PROJ.mlr_total` — NEW, in `code/gamecam.cpp` wrapping
  `StartDraw` through `RenderNow`.
- Reuse existing `GameLogic.Mech3D.UpdateGeometry` for `skinning_chain`
  (boundary matches).
- `CPU_PROJ.projectZ_eventdriven` — NEW; coarse per-frame zone that
  fires only if `n_calls > 0` (avoids 100ns-floor violation when zero).

**Sanity check (load-bearing):** sum of cost-split bucket totals MUST
agree with sum of corresponding Tracy zone self-times within 2x.
Disagreement = instrumentation broken; fix before any sizing decision
rides on the numbers. (Per `cost_split_instrumentation_is_observer_effect_dominated.md`:
this is the discipline the 2026-05-16 terrain campaign lacked.)

### 4.4 TLS render-loop flag for projectZ attribution

`Camera::projectZ()` is called from BOTH the per-object render loop
(`Appearance::recalcBounds` family — counted under `recalcBounds_total`)
AND event-driven sites (ray pick, AI LOS, mouse hit-test — counted
under `projectZ_eventdriven`). To attribute correctly without per-call
overhead:

- Thread-local `bool s_inRenderLoop` flag.
- Set to `true` at entry of `ObjectManager::render()` dispatcher,
  restored at exit (RAII guard).
- Inside `Camera::projectZ()`, branch the counter increment based
  on the flag. Cost: one TLS read + branch per `projectZ()` call.
  Negligible because we're not adding a per-call chrono — only a
  counter bump.
- The TIME for in-render-loop `projectZ` calls is captured by the
  outer `recalcBounds_total` bucket. The TIME for event-driven
  calls needs its own chrono pair around the projectZ() body — but
  only the slow path (`!s_inRenderLoop`), so the per-call chrono
  overhead is bounded to event frequency (mouse-clicks, AI ticks),
  not render frequency.

## 5. Capture protocol

0. **Pre-implementation audits (gate; do BEFORE adding instrumentation):**
   - **Threading audit:** grep `ObjectManager::render` and the per-frame
     render dispatcher chain for any worker-thread fan-out. If render
     invokes `projectZ()` from worker threads, the TLS attribution
     design in §4.4 must be revised before instrumenting. Single-
     threaded confirmation = proceed.
   - **`cameraToClip` write-site audit:** grep ALL writes to
     `cameraToClip` (assignment, `Multiply`, `BuildPerspective`, or
     any compose call). Confirm `Camera::update()` is the only
     per-frame write site, OR enumerate the others and wrap each in
     the `matrix_build` bucket. Per R7.
   - **Frame-boundary reset point:** identify the canonical per-frame
     reset hook (likely `Camera::update` entry OR `gos_RendererBeginFrame`
     OR `ObjectManager::render` entry). All bucket accumulators reset
     here. ONE reset per frame, before any bucket increment.

1. Build `--config RelWithDebInfo --clean-first` (load-bearing per
   CLAUDE.md `class_layout_change_needs_clean_first` if any struct
   layout changes; instrumentation adds static counters and thread-
   local flag, so clean rebuild is mandatory).
2. Deploy per CLAUDE.md (`cp -f` per file + `diff -q`, never `cp -r`).
3. **Baseline (stock workload):** tier1 5/5 30s with
   `MC2_CPU_PROJ_COST_SPLIT=1`. Confirm banner present, sanity
   check (cost-split vs Tracy sum) passes. Numbers from this run
   are the "normal" reference.
4. **Worst-case (load-bearing):** user-driven mc2_10 60s, camera
   at corner-of-map zoomed-out (per `feedback_cost_split_worst_case_camera.md`).
   mc2_10 chosen because per user 2026-05-20 it exercises gosFX
   heaviest (the only stock mission with significant MLR particle
   load). User is the visual observer; this MUST be user-driven,
   not automated.
5. **Analysis:** extract `[CPU_PROJ v1]` lines from
   `tests/smoke/artifacts/<latest>/mc2_10.log`. Compute per-bucket
   median and 95p across the ~30 worst-case samples (500-frame
   cadence over ~60s at 60fps = ~7-8 samples; if too few, extend
   capture). Compare to Tracy export.

## 6. Predeclared outcome triggers

The outcome table is committed BEFORE the measurement so the framing
cannot be retroactively reshaped to fit the data.

| Worst-case 95p total | Action |
|---|---|
| < 100us | **Framing falsified.** CPU projection is NOT the cost-dominant problem. Stop the arc. Hunt elsewhere (skinning if `skinning_chain` is the actual top, AI/mission update if neither is). Bring data back; reframe the brainstorm. |
| 100us–500us, single bucket >60% of total | F2 with that bucket as first eviction target. Bucket dictates the slice (e.g., if `tgl_transform` dominates, F2 starts with TG_MultiShape retirement; if `recalcBounds_total` dominates, F2 starts with recalcBounds unconflation). |
| 100us–500us, spread across 2-3 buckets | F1 foundation slice (matrix unification) first to enable cleanly pointing the multiple consumers at one GPU-side matrix, then F2 evictions in cost-descending order. |
| > 500us, spread across 3+ buckets | F1 first; F2 evictions become a multi-slice campaign in cost-descending order. |
| Any bucket has `n_visible_objects == 0` or `n_prims == 0` for worst-case | Coverage hole; mc2_10 didn't exercise that subsystem. Either find a stock mission that does, or accept the bucket is uninstrumentable in stock content (which means it's also irrelevant to the budget). |
| Cost-split sum disagrees with Tracy sum >2x | **Stop.** Instrumentation broken. Fix and re-measure. Do not size any slice on broken data. |

## 7. Risk surface

**R1 — Observer effect masquerading as signal.** This is the canonical
trap (`cost_split_instrumentation_is_observer_effect_dominated.md`).
Mitigations:
- ALL scopes at OUTER call boundary, never per element.
- Tracy cross-validation MANDATORY; 2x disagreement halts.
- `projectZ_eventdriven` uses TLS branch + chrono only on event path,
  not render path.

**R2 — Workload counter accuracy.** If `n_verts_total` or `n_prims`
is mis-counted (e.g., counts skipped/culled prims), per-vert
extrapolation breaks. Mitigation: counters increment exactly where
work is actually done, not where work is queued or considered.

**R3 — mc2_10 doesn't represent the envelope.** A single mission
captures one workload shape. If MLR or skinning depends heavily on
content not in mc2_10, the worst-case number undercounts. Mitigation
acceptance: stock-only constraint binds us to stock missions; mc2_10
is the agreed worst-case representative for THIS arc. If a bucket
reads suspiciously low, document as "coverage hole" not "irrelevant."

**R4 — TLS flag breaks on threading model assumptions.** If
`ObjectManager::render` invokes worker threads that call `projectZ`,
the TLS flag attribution fragments. Mitigation: pre-implementation
audit — confirm render loop is single-threaded. If not, escalate
attribution design before instrumenting.

**R5 — Capped FPS hides time delta on the wrong axis.** The frame
rate cap means total CPU time per frame is bounded by the cap. We
measure CPU SUBSYSTEM time directly (chrono pairs around named
work), not derived from FPS. Per
`capped_fps_is_not_a_cpu_cost_ab_signal.md`. Stated here so the
analysis stage doesn't confuse this.

**R6 — Static counters across mission transitions.** Counter reset
must happen at mission load (or at first frame of a mission). If
counters carry across mission boundaries within one process,
500-frame summaries straddle missions and the number is garbage for
the second mission onward. Mitigation: reset at mission init hook.

**R7 — Camera::update is not the only matrix touch site.** If
`cameraToClip` is also mutated by editor / debug overlays / cinematic
modes inside the per-frame window, `matrix_build` undercounts.
Mitigation: pre-implementation grep ALL writes to `cameraToClip`
and wrap each in the bucket. Acceptable to start with `Camera::update`
only and document the gap if found.

## 8. What the data does NOT tell us

- Does not tell us GPU cost of any consumer (GPU side instrumentation
  is out of scope for F3).
- Does not tell us if a CPU consumer is correctness-load-bearing
  (i.e., even if MLR clip is 5us, deleting it without replacing the
  per-particle clipping correctness still produces visual bugs).
  That's an F2 design question.
- Does not tell us the latency budget for any GPU readback alternative
  (Stage 0.5 §4 already empirically proved readback has lag + false
  negatives; the eviction choice for each consumer is a separate
  per-bucket design question informed by the consumer's lag tolerance).

## 9. Negative space — what is NOT instrumented

| Site | Why excluded |
|---|---|
| GPU-side projection (compute shaders, vertex shaders) | Out of scope; F3 is CPU baseline only. |
| Per-element zones inside hot loops (per-vertex divide, per-quad cull, per-particle clip) | Violates 100ns floor + observer-effect rule. The OUTER bucket captures their aggregate. |
| Camera input handling (mouse drag, zoom, view-mode switch, cinematic setup) | Not per-frame steady-state; event-driven. Distinct from `projectZ_eventdriven` (which is `Camera::projectZ()` calls from non-render code paths) — these are camera STATE mutations, not clip-space projection reads. Out of scope. |
| Stuff library internal compositions (`Matrix4D::Multiply`, etc.) | Captured implicitly by the wrapping bucket. Per-`Multiply` instrumentation would be observer-effect-dominated. |
| Editor's forked render loop | Different worktree, separate arc per `feedback_editor_must_converge_with_runtime_paths.md`. F3 measures the runtime path only. |
| `mclib/mlr/mlrclipper.cpp:313` LAB_ONLY birds-eye composition | `#ifdef LAB_ONLY` gated, not in production build. |

## 10. References

- Memory: `cost_split_instrumentation_is_observer_effect_dominated.md`
- Memory: `feedback_cost_split_worst_case_camera.md`
- Memory: `capped_fps_is_not_a_cpu_cost_ab_signal.md`
- Memory: `gpu_offload_must_be_substitutive_not_additive.md`
- Memory: `feedback_smoke_serial_only.md` (capture sequencing)
- Memory: `feedback_documents_are_very_low_priority.md` (this spec is
  load-bearing for F1/F2 sizing, not narrative — kept terse)
- Sibling DRAFT: `2026-05-20-unified-projection-meta-fix-design.md`
  (F3 produces the data that decides whether the unified-projection
  arc is right-sized, oversized, or undersized vs the actual CPU
  cost dominance)
- Sibling spec: `2026-05-20-appearance-inview-unconflation-design.md`
  (alpha-Stage 1; Stage 0 measured 60% conflation at sim/render layer
   — different metric, not directly comparable)
- CLAUDE.md "100 ns floor" rule + "Tier-1 instrumentation env vars"
- CLAUDE.md "Smoke sessions are USER-DRIVEN" (load-bearing for
  worst-case capture)

## 11. Grep-verified file:line citations (all 2026-05-20)

- `mclib/bdactor.cpp:1163` — `BldgAppearance::recalcBounds`
- `mclib/bdactor.cpp:3926` — `TreeAppearance::recalcBounds`
- `mclib/genactor.cpp:587` — `GenericAppearance::recalcBounds`
- `mclib/gvactor.cpp:1614` — `GVAppearance::recalcBounds`
- `mclib/mech3d.cpp:2132` — `Mech3DAppearance::recalcBounds`
- `mclib/mech3d.cpp:3205` — `ZoneScopedN("GameLogic.Mech3D.UpdateGeometry")` outer skinning entry
- `mclib/mech3d.cpp:3380,3451,3483,3670,4523` — sub-zones inside UpdateGeometry
- `mclib/msl.cpp:1452` — `ZoneScopedN("TG.MultiShape.PerShapeLoop")` per-multishape transform boundary
- `mclib/msl.cpp:1793,1809` — inner per-leaf and shadow-proj zones
- `mclib/tgl.cpp:55` — `TG_Shape::s_worldToClip` static declaration
- `mclib/tgl.cpp:1617` — `TG_Shape::SetCameraMatrices`
- `mclib/tgl.cpp:1624` — per-frame `s_worldToClip.Multiply(...)` composition
- `mclib/tgl.cpp:1702` — `TG_Shape::MultiTransformShape` per-vert transform entry
- `mclib/camera.cpp:1572` — `Camera::update()` per-frame entry
- `mclib/camera.cpp:1749` — existing `ZoneScopedN("Camera.MotionTracking")`
- `mclib/camera.cpp:732` — `Camera::inverseProject` (deprecated; documented in
  `inverseproject_reduction_feeds_only_deprecated_tacmap.md`; included for
   completeness, not instrumented)
- `mclib/camera.h:431` — `projectZ` declaration
- `mclib/camera.h:514-617` — 7 policy-split projectZ wrappers
- `mclib/mlr/mlrclipper.cpp:116` — `MLRClipper::StartDraw` entry
- `mclib/mlr/mlrclipper.cpp:206` — `cameraToClip(2,2)` direct read for `farClipReciprocal`
- `mclib/mlr/mlrclipper.cpp:318` — `worldToClipMatrix.Multiply(...)` per-frame
- `code/gamecam.cpp:142` — `theClipper->StartDraw(...)` per-frame driver
- `code/gamecam.cpp:143` — duplicate `farClipReciprocal` compute (MLR path)
- `code/gamecam.cpp:269` — `theClipper->RenderNow()` end of MLR draw block
