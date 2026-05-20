# F3 — CPU projection cost baseline (measurement-only) — v2

Date: 2026-05-20. Worktree: `claude/nifty-mendeleev` @ `34c9b65`+.
Status: DRAFT v2 (pre-greybeard/adversarial). Measurement slice. No
consumer retired, no shader touched, no architectural change. Reverts
cleanly.

**v2 changes from v1 (`34c9b65`):** external review folded — 6 blocking
+ 3 non-blocking issues. MLR census folded (gosFX is the SOLE live MLR
consumer). `matrix_build` boundary rewritten (v1 was wrong: cameraToClip
is composed event-driven in setOrthogonal/setPerspective, not per-frame).
Outcome adjudication separated from skinning sidecar. Windowed
percentile reporting replaces single-sample-at-cadence.

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
- Not per-element instrumentation. ALL scopes are coarse — one per
  subsystem per frame, at the outer-call boundary. NO `std::chrono`
  inside any hot-loop function (including `Camera::projectZ`).

## 3. Provenance

- Brainstorming 2026-05-20 (late). User framing: eliminate per-frame
  CPU work; CPU projection is the structural blocker that has stopped
  terrain-as-surface and gameLogic-reduction efforts twice.
- External review 2026-05-20 on spec v1: 6 blocking + 3 non-blocking
  issues. All folded into v2.
- MLR census 2026-05-20: gosFX is the SOLE live MLR consumer; 6
  in-game effect render sites + 2 UI-only briefing sites all go
  gosFX → MLRClipper. ObjectManager does NOT draw via MLR directly
  (architecture.md memory is stale). MLR retirement = rewrite gosFX
  backend; clean substitution set.
- Memory: `cost_split_instrumentation_is_observer_effect_dominated.md`
  (2026-05-16 terrain CPU campaign chased phantom 7.3ms because
  cost-split scopes wrapped per-quad bodies; ~5ms was pure timer
  overhead).
- Memory: `feedback_cost_split_worst_case_camera.md`,
  `capped_fps_is_not_a_cpu_cost_ab_signal.md`,
  `gpu_offload_must_be_substitutive_not_additive.md`.

## 4. Design

### 4.1 Env gate, reporting, and frame lifecycle

- Env: `MC2_CPU_PROJ_COST_SPLIT=1`. Default OFF. Banner
  `[INSTR v1] enabled: CPU_PROJ_COST_SPLIT` at startup.
- **When env is OFF:** counters tick (free); chronos do NOT execute.
  Always-on monotonic line at frame-1500 reports counters only (no
  timing). This makes the env truly free in production.
- **When env is ON:** per-frame samples are accumulated in a ring
  buffer. Every 500 frames, the window's stats are computed and
  printed to stderr; ring buffer is then reset.

**Per-frame ring-buffer commit format (every 500 frames):**

```
[CPU_PROJ v1] window=N..N+499 worst-case-camera=mc2_10
  projection_total           p50=Xus  p95=Yus  max=Zus     budget=100us  status=PASS|OVER
    matrix_build             p50=...  p95=...  max=...
    recalcBounds_render      p50=...  p95=...  max=...     n_visible_p50=NNN
    tgl_transform            p50=...  p95=...  max=...     n_multishapes_p50=NN  n_verts_p50=NNNN
    mlr_total                p50=...  p95=...  max=...     n_prims_p50=NNN
    projectZ_eventdriven     n_calls_p50=NN   time=COUNT-ONLY|outer=Zus  (see §4.4)
  sidecar (NOT in projection_total):
    skinning_chain           p50=...  p95=...  max=...     n_nodes_p50=NNNN
    cpu_pipeline_total       p50=...  p95=...  max=...     (= projection_total + skinning_chain)
```

Status: `PASS` if `projection_total p95 < 100us`, `OVER` otherwise.

**Frame lifecycle (canonical, NOT discovered-by-implementation):**

| Hook | Action |
|---|---|
| `gos_RendererBeginFrame` (`GameOS/gameos/gameos_graphics.cpp:2384`) | Reset all per-frame bucket accumulators. ONE reset/frame, before any bucket increment. |
| During frame | Each bucket scope opens/closes around its outer boundary, accumulating chrono time + workload count. |
| `gos_RendererEndFrame` (`GameOS/gameos/gameos_graphics.cpp:2389`) | Commit current frame's samples into the ring buffer (one entry per bucket). |
| Every 500 entries | Compute window p50/p95/max per bucket; print `[CPU_PROJ v1]` line; clear ring buffer. |
| Mission load/transition | Hard reset ring buffer and frame counter. (Mission-init hook to be identified pre-implementation; see §5 step 0.) |

### 4.2 Buckets (in `projection_total`)

ALL scopes use one `std::chrono::steady_clock::now()` pair per SUBSYSTEM
per FRAME, at the outer-call boundary. Never per element. Each bucket
also accumulates a workload counter for extrapolability.

| Bucket | Wraps | Workload counter | Existing Tracy zone? |
|---|---|---|---|
| `matrix_build` | Sum of three per-frame matrix-composition sites: (a) the `Camera.BuildMVP` block at `code/gamecam.cpp:153-191` (existing Tracy zone); (b) `TG_Shape::SetCameraMatrices` call at `code/gamecam.cpp:1651` whose body composes `s_worldToClip` at `mclib/tgl.cpp:1624`; (c) the matrix work inside `MLRClipper::StartDraw` — **note (c) is captured by `mlr_total`, not double-counted here.** So `matrix_build` = (a) + (b) only. `cameraToClip` itself is event-driven (composed in `setOrthogonal`/`setPerspective` at `mclib/camera.cpp:1876-1894`/`:1930+`); per-frame matrix work is the three derived compositions, not cameraToClip itself | 2 sites/frame | Yes — existing `Camera.BuildMVP` at `gamecam.cpp:153`. New zone needed only for site (b) |
| `recalcBounds_render` | OUTER dispatcher loop in `ObjectManager::render()` that visits each visible object's `Appearance::recalcBounds()`. Wraps the OUTER loop, NOT per-object calls. (Renamed from v1's `recalcBounds_total` to clarify "from-render-loop only" — event-driven recalcBounds is out of scope; if it exists, it's `projectZ_eventdriven` territory) | `n_visible_objects` | No |
| `tgl_transform` | OUTER call to `TG_Shape::MultiTransformShape` (`mclib/tgl.cpp:1702`) per multishape — captures the whole per-shape CPU vertex transform pass | `n_multishapes`, `n_verts_total` | Yes — adjacent zone `TG.MultiShape.PerShapeLoop` at `mclib/msl.cpp:1452`. Cost-split adds its own outer scope at the same boundary |
| `mlr_total` | The existing `ZoneScopedN("GameCamera::render clipperRenderNow")` at `code/gamecam.cpp:268`, wrapping `theClipper->RenderNow()` at `:269`. (Also instrument `StartDraw` at `:142` as a separate sub-scope; sum into `mlr_total`.) **Per MLR census, this measures the entire CPU cost of the gosFX particle render path — MLR's only live consumer. Substitution = GPU-side gosFX.** Acceptable that this includes vertex submission to driver in addition to clip/project; MLR retirement substitutes BOTH | `n_prims_clipped` — add static counter inside `MLRClipper` at the per-primitive dispatch site (location to be greped pre-implementation; bumped exactly once per primitive submitted) | Yes — existing `clipperRenderNow` at `code/gamecam.cpp:268` (covers RenderNow only; StartDraw scope is new) |
| `projectZ_eventdriven` | **NO chrono inside `Camera::projectZ`.** Counter-only inside projectZ, with TLS render-loop flag (see §4.4). If a per-frame outer event-dispatch boundary is identified during the pre-implementation audit (e.g., a per-frame AI tick), wrap that with one chrono pair; otherwise time stays UNMEASURED and is excluded from `projection_total` adjudication | `n_calls_eventdriven` (always); outer-scope time IF outer boundary found | No |

### 4.3 Sidecar bucket (NOT in `projection_total`)

| Bucket | Wraps | Workload counter | Existing Tracy zone? |
|---|---|---|---|
| `skinning_chain` | Outer Mech3D per-frame node-chain compose — `ZoneScopedN("GameLogic.Mech3D.UpdateGeometry")` at `mclib/mech3d.cpp:3205` | `n_nodes_processed` | Yes — reuse outer zone (inclusive time covers sub-zones AnimPose/BodyXform/Arms) |

**Why sidecar:** skinning is CPU work but it is NOT projection cost. If
`skinning_chain` dominates the frame, that is a different arc (mech
skinning offload to GPU compute), not the projection arc. Reporting
it alongside means we surface "the projection framing is wrong, the
real fish is skinning" without conflating it into the adjudicating
total. See §6 outcome triggers.

`cpu_pipeline_total = projection_total + skinning_chain` is also printed
as a broader-context signal but is NOT what adjudicates this slice.

### 4.4 `projectZ_eventdriven` attribution (count-only by default)

Per external-review issue #1: putting chrono inside `Camera::projectZ()`
is per-call timing — even gated by a TLS flag, it recreates the
observer-effect problem if AI LOS / similar fires many times per frame.

Design:
- **Always:** TLS `bool s_inRenderLoop` set true at entry of
  `ObjectManager::render()` (RAII guard restores at exit). Static
  counter inside `Camera::projectZ()` increments `n_calls_eventdriven`
  iff `!s_inRenderLoop`. Cost: one TLS read + one branch + one
  counter bump per call — no chrono.
- **Conditional (if pre-implementation audit identifies an outer
  event-dispatch boundary):** wrap that boundary with one chrono
  pair. The boundary candidate: per-frame AI tick dispatcher (if
  it exists and is bounded), OR a per-frame mouse/input handler.
  If no clean outer boundary exists, the time stays UNMEASURED.
- **Reporting:** if outer-scope time exists, print it; otherwise
  print `time=COUNT-ONLY`. Either way, `projectZ_eventdriven` is
  EXCLUDED from `projection_total` adjudication (only the count
  is informative when no time is measured).

Rationale: counts answer "is this site actually exercised in worst-case
capture?" Time without observer effect requires a coarse outer scope.
Don't compromise discipline for time data we may not actually need.

### 4.5 Tracy cross-validation (INCLUSIVE time, not self-time)

Per external-review issue #2: cost-split scopes are inclusive (they
include time spent in child zones). Comparing to Tracy SELF-time would
falsely report disagreement for any bucket whose zone has children
(notably `skinning_chain`'s `GameLogic.Mech3D.UpdateGeometry` outer
which has 6+ child zones).

**Compare cost-split bucket totals to Tracy INCLUSIVE (elapsed)
time** for the matching boundary zones. Tracy's flame-graph view shows
inclusive time as the full bar width; self-time is the unshaded
portion only.

Sanity threshold: per-bucket cost-split total within 2x of Tracy
inclusive time. >2x disagreement = instrumentation broken; halt before
sizing decisions.

## 5. Capture protocol

0. **Pre-implementation audits (gate; do BEFORE adding any instrumentation):**
   - **Threading audit:** grep `ObjectManager::render` and the
     per-frame render dispatcher chain for any worker-thread fan-out.
     If render invokes `projectZ()` from worker threads, the TLS
     attribution design in §4.4 must be revised. Single-threaded
     confirmation = proceed.
   - **`cameraToClip` write-site exhaustive grep.** Confirm
     `setOrthogonal`/`setPerspective` are the only writers, and that
     they are NOT called per-frame in steady state. If a per-frame
     writer is found, add it to `matrix_build`.
   - **Event-dispatch outer boundary search** for `projectZ_eventdriven`
     §4.4. If found, scope name + line + reset cadence. If not found,
     accept count-only reporting.
   - **MLR per-primitive dispatch site** for the `n_prims_clipped`
     counter (within `mclib/mlr/mlrclipper.cpp` or its callees). Grep
     for the per-prim hot loop entry.
   - **Mission init/load hook** for ring-buffer hard reset. Likely
     candidates: `Mission::init` (`code/mission.cpp`), `Mission::load`
     (savegame path), `Logistics::beginMission` (campaign resume).
     All three must call the reset (per
     `mission_load_inits_mirror_init_per_subsystem.md`).

1. Build `--config RelWithDebInfo --clean-first` (per CLAUDE.md
   class-layout discipline; instrumentation adds static counters +
   thread-local flag + ring buffer).
2. Deploy per CLAUDE.md (`cp -f` per file + `diff -q`).
3. **Baseline (stock workload):** tier1 5/5 30s with
   `MC2_CPU_PROJ_COST_SPLIT=1`. ~1800 frames/mission @ 60fps = ~3-4
   window-stats lines per mission. Confirm banner present; verify
   cost-split vs Tracy inclusive-time sanity per §4.5.
4. **Worst-case (load-bearing):** user-driven mc2_10 60s, camera at
   corner-of-map zoomed-out (per `feedback_cost_split_worst_case_camera.md`).
   ~3600 frames = ~7 window-stats lines, each containing 500 in-window
   samples for percentile computation. mc2_10 chosen per user
   2026-05-20 as the stock mission with heaviest gosFX exercise (so
   `mlr_total` is best stimulated here).
5. **Event-driven coverage (optional, per §4.4):** if `projectZ_eventdriven`
   reads zero or near-zero in capture #4, a follow-up user-driven
   capture deliberately exercises mouse clicks, unit selection, and
   targeting reticle movement to stimulate event-driven paths. Bucket
   classification (LIVE vs not-stimulated) requires this if the
   default capture is zero.
6. **Analysis:** extract `[CPU_PROJ v1]` window lines from
   `tests/smoke/artifacts/<latest>/mc2_10.log`. The window p95 across
   the ~7 windows is the load-bearing number. Cross-validate against
   Tracy export.

## 6. Predeclared outcome triggers

`projection_total = matrix_build + recalcBounds_render + tgl_transform + mlr_total`.
`projectZ_eventdriven` is informational; its count matters but its
time is excluded unless an outer boundary was found. `skinning_chain`
is reported as sidecar.

Adjudication uses **window p95** at worst-case camera (mc2_10 corner
zoomed-out), not single-frame max and not p50.

| Condition | Action |
|---|---|
| `projection_total` p95 < 100us AND `skinning_chain` p95 < 100us | **Both framings falsified.** CPU projection is not the cost-dominant problem AND skinning isn't either. Hunt elsewhere (AI / mission update / asset streaming). Reframe the brainstorm. |
| `projection_total` p95 < 100us BUT `skinning_chain` p95 > 100us | **Projection framing falsified, but a different CPU arc exists.** Park projection; open separate "GPU skinning compute" arc. Bring data back to brainstorm. |
| `projection_total` p95 100us–500us, single bucket > 60% of `projection_total` | F2 with that bucket as first eviction target. Bucket dictates the slice: `tgl_transform` dominant → TG_MultiShape retirement; `recalcBounds_render` dominant → recalcBounds unconflation continues; `mlr_total` dominant → gosFX GPU backend slice (clean per MLR census). |
| `projection_total` p95 100us–500us, spread across 2-3 buckets | F1 foundation slice (matrix unification) first — multiple consumers need a clean unified GPU-side matrix to point at. Then F2 evictions in cost-descending order. |
| `projection_total` p95 > 500us, spread across 3+ buckets | F1 first; F2 evictions become a multi-slice campaign in cost-descending order. |
| `mlr_total` ≈ 0 in worst-case (mc2_10) | gosFX particles not exercising MLR meaningfully under stock content. MLR retirement becomes near-free (no measurable substitution gain, but removes a CPU/code-debt class). Schedule as a low-risk cleanup, not a perf slice. |
| `projectZ_eventdriven` n_calls == 0 in steady-state capture | Need event-driven coverage capture (§5 step 5). If still zero with stimulation, bucket is dead-code in current build — schedule deletion of `Camera::projectZ` itself as a separate slice. |
| `tgl_transform` n_verts == 0 or `recalcBounds_render` n_visible == 0 | Coverage hole — capture didn't hit content that exercises this bucket. Try different stock mission OR document as "stock content does not exercise this; bucket cannot be sized." |
| `matrix_build` n_calls == 0 | Bucket implementation bug (matrix_build always has work>0 by definition); fix before re-running. |
| Cost-split per-bucket sum disagrees with Tracy INCLUSIVE time > 2x | **Stop.** Instrumentation broken. Fix and re-measure. Do not size any slice on broken data. |

### 6.1 Skinning sidecar interpretation

If `skinning_chain` p95 > `projection_total` p95, the broader
"eliminate per-frame CPU work" arc has its hottest fish in skinning,
not projection. This is data for opening a parallel/successor arc;
it does not change the projection arc's adjudication. The projection
arc proceeds or stops on `projection_total` alone.

If `skinning_chain` p95 < 30us, skinning is irrelevant to budget and
this sidecar gets dropped from future captures.

## 7. Risk surface

**R1 — Observer effect masquerading as signal.** Canonical trap
(`cost_split_instrumentation_is_observer_effect_dominated.md`).
Mitigations: ALL scopes at OUTER call boundary; Tracy INCLUSIVE-time
cross-validation MANDATORY; `projectZ_eventdriven` is count-only by
default with no chrono inside `Camera::projectZ`.

**R2 — Workload counter accuracy.** If `n_verts_total` or `n_prims_clipped`
mis-counts (e.g., counts skipped prims), per-vert extrapolation breaks.
Mitigation: counters increment exactly where work is done, not where
work is queued.

**R3 — mc2_10 doesn't represent the envelope.** Stock-only constraint
binds us to stock missions; mc2_10 is the agreed worst-case representative
per user. If a bucket reads suspiciously low, classify as "coverage
hole" (per §6) not "irrelevant."

**R4 — TLS flag fragments on threading.** Pre-implementation audit
(§5 step 0) gates instrumentation. If multi-threaded, redesign.

**R5 — Capped FPS hides CPU time on the wrong axis.** Measure
SUBSYSTEM time directly (chrono pairs), not FPS-derived. Per
`capped_fps_is_not_a_cpu_cost_ab_signal.md`.

**R6 — Static counters across mission transitions.** Ring buffer + frame
counter hard-reset at mission init/load hooks (§5 step 0 identifies
all of them per `mission_load_inits_mirror_init_per_subsystem.md`).

**R7 — `cameraToClip` per-frame writers not yet found.** Pre-implementation
grep (§5 step 0) is the gate. If found, add to `matrix_build`.

**R8 — Tracy inclusive-vs-self confusion at analysis time.** Folded
into design (§4.5) but worth flagging at analysis: a Tracy view that
auto-shows self-time will trigger false sanity-check failures. Verify
view mode before declaring disagreement.

**R9 — `mlr_total` includes non-projection MLR work** (vertex submit,
driver-facing setup) intentionally. Per MLR census this is fine: MLR
retirement substitutes the WHOLE block, so `mlr_total` accurately
sizes the substitution target. Documented here so the analysis stage
doesn't argue the scope is "too broad."

**R10 — Ring-buffer memory cost.** 500 samples * 6 buckets * (8 bytes
time + 4 bytes counter) ≈ 36KB. Trivial; flagged only for completeness.

## 8. What the data does NOT tell us

- GPU cost of any consumer (GPU instrumentation out of scope for F3).
- Whether a CPU consumer is correctness-load-bearing (even if a bucket
  is small, deleting it without replacing its function still produces
  visual bugs — that's an F2 per-bucket design question).
- Latency tolerance of any GPU readback alternative (Stage 0.5 §4
  empirically proved readback has lag + non-superset false negatives;
  eviction choice per consumer is a separate per-bucket design question).
- Whether observed numbers will hold under content scale (mods, future
  assets). Workload counters allow extrapolation; the spec doesn't do
  that extrapolation.

## 9. Negative space — what is NOT instrumented

| Site | Why excluded |
|---|---|
| GPU-side projection (compute, vertex shaders) | Out of scope; F3 is CPU baseline only. |
| Per-element zones inside hot loops | Violates 100ns floor + observer-effect rule. OUTER bucket captures aggregate. |
| Camera input handling (mouse drag, zoom, view-mode switch, cinematic) | Camera STATE mutations, distinct from projectZ reads. Out of scope. |
| Stuff library internal compositions (`Matrix4D::Multiply` etc.) | Captured implicitly by wrapping bucket. |
| Editor's forked render loop | Different worktree, separate arc per `feedback_editor_must_converge_with_runtime_paths.md`. F3 measures runtime path only. |
| `mclib/mlr/mlrclipper.cpp:313` LAB_ONLY birds-eye composition | `#ifdef LAB_ONLY` gated. |
| HUD overlays, compass, UI 2D | Not projection consumers per MLR census; pre-rendered or screen-space direct. |
| MissionGUI briefing-only VTOL effects (`code/missiongui.cpp:2849, :2872`) | Out-of-mission per MLR census; not in worst-case capture window. |

## 10. References

- Memory: `cost_split_instrumentation_is_observer_effect_dominated.md`
- Memory: `feedback_cost_split_worst_case_camera.md`
- Memory: `capped_fps_is_not_a_cpu_cost_ab_signal.md`
- Memory: `gpu_offload_must_be_substitutive_not_additive.md`
- Memory: `feedback_smoke_serial_only.md`
- Memory: `mission_load_inits_mirror_init_per_subsystem.md`
- Sibling DRAFT: `2026-05-20-unified-projection-meta-fix-design.md`
- Sibling spec: `2026-05-20-appearance-inview-unconflation-design.md`
- CLAUDE.md "100 ns floor" + "Tier-1 instrumentation env vars" +
  "Smoke sessions are USER-DRIVEN"
- External review folded 2026-05-20 (this conversation transcript)
- MLR census 2026-05-20 (gosFX is sole live MLR consumer; 6 in-game
  + 2 UI-only render sites; ObjectManager does NOT draw via MLR
  contrary to stale architecture.md claim)

## 11. Grep-verified file:line citations (all 2026-05-20)

- `mclib/bdactor.cpp:1163` — `BldgAppearance::recalcBounds`
- `mclib/bdactor.cpp:3926` — `TreeAppearance::recalcBounds`
- `mclib/genactor.cpp:587` — `GenericAppearance::recalcBounds`
- `mclib/gvactor.cpp:1614` — `GVAppearance::recalcBounds`
- `mclib/mech3d.cpp:2132` — `Mech3DAppearance::recalcBounds`
- `mclib/mech3d.cpp:3205` — `ZoneScopedN("GameLogic.Mech3D.UpdateGeometry")`
  (sidecar `skinning_chain`; reuse this zone)
- `mclib/mech3d.cpp:3380,3451,3483,3670,4523` — sub-zones inside UpdateGeometry
- `mclib/msl.cpp:1452` — `ZoneScopedN("TG.MultiShape.PerShapeLoop")`
- `mclib/msl.cpp:1793,1809` — inner per-leaf + shadow-proj zones
- `mclib/tgl.cpp:55` — `TG_Shape::s_worldToClip` static decl
- `mclib/tgl.cpp:1617` — `TG_Shape::SetCameraMatrices` entry
- `mclib/tgl.cpp:1624` — per-frame `s_worldToClip.Multiply(...)`
- `mclib/tgl.cpp:1702` — `TG_Shape::MultiTransformShape` per-vert transform entry
- `mclib/camera.cpp:1572` — `Camera::update()` entry (NOT a matrix
  composition site; included for context only)
- `mclib/camera.cpp:1749` — existing `ZoneScopedN("Camera.MotionTracking")`
- `mclib/camera.cpp:1876-1894` — orthographic `cameraToClip` composition
  (event-driven; NOT per-frame)
- `mclib/camera.cpp:1930+` — perspective `cameraToClip` composition
  (event-driven; NOT per-frame)
- `mclib/camera.cpp:732` — `Camera::inverseProject` (deprecated)
- `mclib/camera.h:431` — `projectZ` declaration
- `mclib/camera.h:514-617` — 7 policy-split projectZ wrappers
- `mclib/mlr/mlrclipper.cpp:116` — `MLRClipper::StartDraw` entry
- `mclib/mlr/mlrclipper.cpp:206` — `cameraToClip(2,2)` direct read
  for `farClipReciprocal`
- `mclib/mlr/mlrclipper.cpp:318` — `worldToClipMatrix.Multiply(...)` per-frame
- `code/gamecam.cpp:142` — `theClipper->StartDraw(...)` per-frame driver
- `code/gamecam.cpp:143` — duplicate `farClipReciprocal` compute (MLR path)
- `code/gamecam.cpp:147` — existing `ZoneScopedN("GameCamera::render activeScene")`
- `code/gamecam.cpp:153` — existing `ZoneScopedN("Camera.BuildMVP")`
  (reuse for `matrix_build` site (a))
- `code/gamecam.cpp:1651` — `TG_Shape::SetCameraMatrices(...)` per-frame call
  (`matrix_build` site (b))
- `code/gamecam.cpp:213` — `ObjectManager->render(true, true, true)`
  (outer scope for TLS render-loop flag + `recalcBounds_render` bucket)
- `code/gamecam.cpp:268` — existing `ZoneScopedN("GameCamera::render clipperRenderNow")`
- `code/gamecam.cpp:269` — `theClipper->RenderNow()` (inner of `mlr_total`)
- `GameOS/gameos/gameos_graphics.cpp:2384` — `gos_RendererBeginFrame`
  (canonical per-frame reset hook)
- `GameOS/gameos/gameos_graphics.cpp:2389` — `gos_RendererEndFrame`
  (canonical per-frame commit hook)
- `code/mechcmd2.cpp:1569,1825` — `theClipper` construct/destroy
- `mclib/txmmgr.cpp:349` — `new MidLevelRenderer::MLRClipper(...)`
  startup creation

## 12. v1 → v2 changelog

External review folded:

- **(blocker 1)** Removed chrono from `Camera::projectZ`; bucket is
  count-only with optional outer-event-boundary timing.
- **(blocker 2)** Tracy cross-validation explicitly uses INCLUSIVE
  time, not self-time. §4.5 added.
- **(blocker 3)** 500-frame cadence now reports window p50/p95/max
  computed from in-memory ring buffer of per-frame samples, not a
  single sample at cadence. ~7 windows in 60s capture × 500 in-window
  samples = real percentile basis.
- **(blocker 4)** `mlr_total` scope clarified — explicitly includes
  vertex-submit work; per MLR census this is fine because MLR
  retirement substitutes the whole block. Documented in §4.2 and R9.
- **(blocker 5)** `matrix_build` boundary rewritten — v1 wrapped
  `Camera::update()` which is wrong (`cameraToClip` is event-driven,
  not per-frame). v2 wraps the two actual per-frame composition sites
  (`Camera.BuildMVP` and `SetCameraMatrices`).
- **(blocker 6)** Frame lifecycle fully specified — canonical
  reset/commit hooks at `gos_RendererBeginFrame`/`gos_RendererEndFrame`
  with mission-load hard-reset.
- **(non-blocker 1)** Always-on monotonic line: counters only when env
  OFF, no chrono. Truly free in production.
- **(non-blocker 2)** Skinning moved to sidecar; `projection_total`
  excludes it; outcome table has skinning-dominant row.
- **(non-blocker 3)** Bucket-specific coverage rules — `matrix_build`
  always has work, `projectZ_eventdriven` zero-call has its own
  follow-up capture protocol, etc.

MLR census folded:

- §3 provenance + §10 references cite MLR-is-gosFX-only finding.
- §4.2 `mlr_total` description includes "substitution = GPU-side
  gosFX."
- §6 outcome trigger for `mlr_total ≈ 0` added.
- §9 negative space includes briefing-only VTOL effects and
  architecture.md staleness note.
- Architecture.md memory update is a separate follow-on task (the
  "ObjectManager draws immediately via MLR" claim is stale).
