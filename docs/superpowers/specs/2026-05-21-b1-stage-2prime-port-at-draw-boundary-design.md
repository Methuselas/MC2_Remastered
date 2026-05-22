# B1 Stage 2' — Port gosFX at `Effect::Draw` Boundary; Substitutive MLR Retirement

**Date:** 2026-05-21
**Branch base:** `claude/nifty-mendeleev` @ `5233e70` (axis-swap + `age=0.5` hack made
particles visible for one frame).
**Scope:** G2 above-level META-FIX for B1. Spec-only — no implementation in this slice.
**Status:** DESIGN. Implementation gated on user acceptance of this spec.

---

## 1. Problem statement

B1 (GPU particle emission) currently ships with a single-frame visibility caveat and
two embedded hacks. Two independent greybeard rulings converged on a structural
diagnosis: **B1 was ported at the wrong lifecycle boundary.** The C5–C17 stack added
`mc2::particles::Spawn*()` calls in each gosFX subclass `Start()` — a one-shot
construction-time hook. The gosFX per-frame protocol is:

> `Make` → `Execute(ExecuteInfo)` → `Draw(DrawInfo)`

Real per-frame state lives in `Execute`, which advances `m_age` and resamples spec
curves (color, halfHeight, scale, lifeSpan) at the new age. Spawning at `Start()`
captures a stale snapshot taken before any `Execute` cycle has run; the GPU shader
then renders that single snapshot every frame.

The two hacks compensate for this boundary error:

1. **`parent_age = 0.5f` constant in 4 producers** —
   `mclib/particles/spawn_tube.cpp:97`,
   `mclib/particles/spawn_shard.cpp:107`,
   `mclib/particles/spawn_point.cpp:83`,
   `mclib/particles/spawn_cardcloud.cpp:83`.
   (`spawn_card.cpp:71` uses the variable name `age` not `parent_age`; same intent,
   same value, same producer-side hack.) Picks the typical curve peak so fade-in /
   fade-out envelopes do not bake an invisible degenerate particle at `age=0`.
2. **`max(p.size, 8.0)` floor at `shaders/particle_billboard.vert:78`** —
   prevents sub-pixel size from collapsing the billboard to zero footprint.

The legacy MLR per-vertex CPU transform path remains the dominant per-frame cost.
Telemetry (R3 artifact `2026-05-20T19-44-27`, mc2_10 worst-case):

| Zone | p95 | Calls / frame | State |
|---|---|---|---|
| `mlr_total` | **412.9 us** | — | HOT (`mlrpointcloud.cpp:115` per-vertex `Multiply(points[i], effectToClipMatrix)`) |
| `effect_admission_perframe` | 54 us | ~990 | COLD (`~54 ns/call`, Execute curve sample + admission) |
| `matrix_build` | 0.7 us | — | dead (FOV/res-change only) |

The current B1 commit stack is **additive**: GPU emission was bolted in alongside the
legacy MLR path. Per
`memory/feedback_offload_must_be_substitutive_not_additive.md` and
`memory/verify_producer_path_against_telemetry_before_substitution.md`, additive
offloads net ~0 ms. The 412 us MLR cost must die for B1 to qualify as a real offload.

Cross-ref: `memory/HANDOFF_2026_05_21_b1_stable_invisible_clipspace_test_next.md`.

---

## 2. Goals

(a) Eliminate the single-frame visibility caveat.
(b) Retire `mlr_total` 412 us per-frame cost **substitutively** (the work goes away,
    not just gets duplicated on the GPU side).
(c) Retire the `age=0.5` constant and the `max(p.size, 8.0)` shader floor.
(d) Set up the legacy MLR clipper for consumer-death retirement (unblocks the
    unified-projection v2 HIGH-RISK R-MLR item enumerated in
    `memory/HANDOFF_2026_05_20_late_alpha_stage_1_ship_unified_projection_draft.md`).

## 3. Non-goals

(a) Retiring `Singleton::Execute` curve resample — it is cold (~54 us p95) and is the
    semantic simulator. Stays alive.
(b) GPU-side simulation / compute-tick age advance. That is B-scope, separate later
    slice.
(c) **Actively** removing the legacy MLR clipper this slice. It dies by consumer
    death as a side-effect — the actual code excision is a follow-up.
(d) Unified-projection v2. This slice unblocks but does not deliver it.
(e) B2 (skipping legacy `Execute` under env-on). `Execute` is the simulator and stays
    alive.

---

## 4. Architecture: lifecycle diagram

### Correct protocol (target after this slice)

```
gosFX::Effect (per particle / per instance):

    Make                     [construction; one-shot]
      |
      v
    Execute(ExecuteInfo)     [per-frame, COLD]
      |                        - advances m_age
      |                        - samples spec curves at m_age
      |                          (m_color.{r,g,b,a}, m_halfY, m_halfX,
      |                           m_radius, m_scale)  — see singleton.cpp:241-245
      v
    Draw(DrawInfo)           [per-frame, HOT in legacy path]
      |                        - LEGACY: MLR per-vertex Multiply + clip
      |                          (mlrpointcloud.cpp:115)
      |                        - NEW: emit one record to GPU batch with
      |                          CURRENT (just-Executed) state

```

### B1-today (incorrect boundary)

```
    Start                    [one-shot]
      | + mc2::particles::Spawn(...)    <-- captures m_localToWorld, m_seed
      v                                       only; Execute has never run, so
    Execute(...)                              m_color / m_halfY are still
      | legacy untouched                      uninitialized.
      v
    Draw(...)
      | legacy MLR untouched (still hot)
      v
```

The Spawn call lives at the wrong arrow. It runs **once**, before any `Execute`, so
the producers `spawn_*.cpp` must synthesize a plausible age (`0.5`) and resample
curves locally rather than reading the values `Singleton::Execute` would have
written.

### After Stage 2'

```
    Start                    [one-shot]
      | - removed Spawn calls (kept C9/C17 legacy parent-Start fall-through)
      v
    Execute(...)             [unchanged — still samples curves into
      |                       m_color / m_halfY / m_halfX / m_radius / m_scale]
      v
    Draw(DrawInfo)           [overridden in each of 5 subclasses]
      | + mc2::particles::Spawn(...) using CURRENT m_color, m_halfY, m_age, ...
      | + MC2_GPU_PARTICLES=1: legacy MLR Draw chain SKIPPED
      | + MC2_GPU_PARTICLES=0: legacy MLR Draw chain runs (parity)
      v
```

---

## 5. Surgical changes (per file; grep-verified at write time)

All file:line citations re-verified against worktree HEAD `5233e70` immediately
before this spec was written. Symbols stable, lines may drift before implementation —
re-grep at implementation time.

### 5.1 `mclib/gosfx/card.cpp`

- `Card::Start` at `card.cpp:370` — **remove** the conditional Spawn block:
  ```
  if (mc2::particles::Batcher::is_enabled()) {
      (void)mc2::particles::Spawn(m_specification, &m_localToWorld, (float)m_seed);
  }
  ```
- **Keep** the C17 legacy parent-Start fall-through block above it
  (`card.cpp:362-367`: `m_halfY`/`m_halfX`/`m_radius` init + `m_cardCloud->TurnOn(0)`).
  Crash-class fix; separate correctness contract.
- `Card::Draw(DrawInfo *info)` at `card.cpp:493` — **add** Spawn call near the top
  (after `Check_Object` / `Check_Pointer`, before legacy MLR chain). Pass current
  per-instance state. **Add** an env-on `MC2_GPU_PARTICLES=1` guard around the
  remainder of the function (the legacy MLR call path) so it short-circuits.
  Subclass tail still calls `Singleton::Draw(info)` at `card.cpp:567` — under env-on
  this should also be elided.

### 5.2 `mclib/gosfx/cardcloud.cpp`

- `CardCloud::Start` at `cardcloud.cpp:910` — **remove** the Spawn block (same
  shape as 5.1; the C11 marker is in `cardcloud.hpp:184`).
- `CardCloud::Draw(DrawInfo *info)` at `cardcloud.cpp:487` — **add** Spawn at top;
  env-gate the legacy chain through to the tail `SpinningCloud::Draw(info)` at
  `cardcloud.cpp:880`.

### 5.3 `mclib/gosfx/pointcloud.cpp`

- `PointCloud::Start` at `pointcloud.cpp:523` — **remove** the Spawn block. The C8
  marker is in `pointcloud.hpp:149`; the comment trail explaining the Spawn
  rationale lives at `pointcloud.cpp:498`.
- `PointCloud::Draw(DrawInfo *info)` at `pointcloud.cpp:463` — **add** Spawn at top;
  env-gate the legacy chain through to `ParticleCloud::Draw(info)` at
  `pointcloud.cpp:482`.

### 5.4 `mclib/gosfx/shardcloud.cpp`

- `ShardCloud::Start` at `shardcloud.cpp:695` — **remove** the Spawn block.
  C8 marker `shardcloud.hpp:169`.
- `ShardCloud::Draw(DrawInfo *info)` at `shardcloud.cpp:307` — **add** Spawn at top;
  env-gate legacy chain through to `SpinningCloud::Draw(info)` at
  `shardcloud.cpp:666`.

### 5.5 `mclib/gosfx/tube.cpp`

- `Tube::Start` at `tube.cpp:757` — **remove** the Spawn block.
  **Keep** the C17 legacy parent-Start fall-through (`m_birthAccumulator = 1.0f` at
  `tube.cpp:754` plus its preceding comment block).
- `Tube::Draw(DrawInfo *info)` at `tube.cpp:1163` — **add** Spawn at top; env-gate
  legacy chain through to `Effect::Draw(info)` at `tube.cpp:1300`.

### 5.6 Producer-side curve sampling (5 files)

The current `spawn_*.cpp` files **sample curves themselves** at the synthesized
`age=0.5` because the Spawn call runs from `Start` before `Execute` has populated
`m_color`/`m_halfY`/etc. After the boundary move, the producers have two viable
shapes:

- **Option A (preferred):** Spawn signatures grow explicit per-frame state
  parameters (color RGBA, halfX/halfY, radius, lifetime-remaining). The subclass
  `Draw` reads its already-resampled members (`m_color.red`, `m_halfY`, ...) and
  passes them in. Producers stop sampling curves entirely. **Delete** the
  `const Stuff::Scalar parent_age = 0.5f;` line and every `mut_spec->m_X.ComputeValue(age, seed)` block in each of the 5 producers.

- **Option B:** Keep producer signatures as-is, pass `m_age` through, let the
  producers `ComputeValue(m_age, m_seed)`. Functionally equivalent but duplicates
  the `Singleton::Execute` curve-sample work the engine already did this frame.
  Wasteful. Reject.

This spec adopts **Option A**.

Producer changes (each must be reverified at impl time):

| File | Action |
|---|---|
| `mclib/particles/spawn_card.cpp:71` | Delete `const Stuff::Scalar age = 0.5f;` and the four `ComputeValue(age, seed)` blocks at lines 80-92, 96-97. Grow the function signature to accept resolved state. |
| `mclib/particles/spawn_cardcloud.cpp:83` | Delete `const Stuff::Scalar parent_age = 0.5f;` and curve-sample block. |
| `mclib/particles/spawn_point.cpp:83` | Same. |
| `mclib/particles/spawn_shard.cpp:107` | Same. |
| `mclib/particles/spawn_tube.cpp:97` | Same. |

### 5.7 `shaders/particle_billboard.vert`

- Line 78: `float effSize = max(p.size, 8.0);` — **replace** with energy-preserving
  analytic sub-pixel formulation:
  ```
  // Pseudocode — final form TBD at implementation time.
  float pxSize = /* computed projected pixel size */;
  float effSize;
  float alphaScale;
  if (pxSize < 1.0) {
      effSize    = 1.0;
      alphaScale = pxSize * pxSize;       // energy preservation
  } else {
      effSize    = pxSize;
      alphaScale = 1.0;
  }
  ```
  Multiply the output `p.color.a` by `alphaScale`. The 8.0 hard floor goes away.
  See R-4 below for the alpha-blend caveat.

---

## 6. Substitutive proof

Per `memory/feedback_offload_must_be_substitutive_not_additive.md`: zone-death is the
ship metric. The implementation must demonstrate, on the same hardware and on the
same `mc2_10` worst-window slice as the R3 baseline:

| Tracy zone | Baseline (B1-today) | Target (Stage 2') | Direction |
|---|---|---|---|
| `mlr_total` | 412.9 us p95 | **<50 us** under `MC2_GPU_PARTICLES=1` | DROPS (HOT path retires) |
| `effect_admission_perframe` | 54 us p95 / ~990 calls | unchanged (within noise) | STABLE (cold sim survives) |
| `matrix_build` | 0.7 us | unchanged | STABLE (already dead) |

The substitutive claim: `mlrpointcloud.cpp:115`'s `(*transformedCoords)[i].Multiply(points[i], effectToClipMatrix)` ceases to be invoked from gosFX paths once each subclass `Draw` short-circuits before reaching the legacy MLR enqueue.

R-1 below holds that this claim must be **re-verified by Tracy capture** before
committing the slice — `mlr_total` may include MLR work from non-gosFX callers (UI,
sky, mech debris), in which case the substitutive percentage shrinks.

---

## 7. Ship-gate

- **Smoke:** tier1 `mc2_10` 40s with `MC2_GPU_PARTICLES=1` (per
  `memory/mc2_10_is_only_tier1_with_gosfx.md`, the other tier1 missions do not
  exercise gosFX). 0 GL errors. No crash. No heap corruption (the C9/C17 fall-through
  fixes must stay live — they are independent of the Spawn-move and protect the
  off-path).
- **A/B visual:** user-driven side-by-side env-on vs env-off on the same mission.
  FX envelope (fade in, peak, fade out) must match stock to user's eye. Particles
  must remain visible for the duration the legacy effect remained visible — no
  single-frame caveat.
- **Telemetry gate:** `mlr_total` worst-window p95 drops to <50 us under env-on.
  `effect_admission_perframe` p95 unchanged (within noise). Both captured against the
  R3 baseline window.
- **`[GPU_PARTICLES v1]` summary:** `records_per_flush_max` should rise materially —
  every active particle now re-emits every frame from Execute-driven state, rather
  than once at construction. This is the producer-side oracle that the boundary moved
  successfully.
- **CI:** `scripts/check-particles-no-cpu-projection.sh` continues to pass (producers
  remain projection-free; the new Draw-side Spawn call must not re-introduce any
  forbidden projection wrapper).

---

## 8. Risks + open questions

### OQ-1 — Draw override exhaustiveness (resolved at spec time)

Each of the 5 subclasses has a `Draw(DrawInfo *)` override at HEAD; verified by grep
in §5. None inherit `Singleton::Draw` directly. Tube and Card chain through
`Singleton::Draw` / `Effect::Draw` at the tail; CardCloud, PointCloud, ShardCloud
chain through `SpinningCloud::Draw` / `ParticleCloud::Draw`. The Spawn call goes at
the head of each subclass override.

### R-1 — `mlr_total` composition

`mlr_total` may include MLR work from non-gosFX callers. The substitutive proof
needs a Tracy capture **before** the slice lands that shows `mlrpointcloud.cpp:115`
specifically dominates the gosFX subset of `mlr_total`. If the dominant cost is in
`mlrclipper.cpp` Clip/Transform inside non-gosFX callers, the 412 us substitutive
claim weakens to "the gosFX share of `mlr_total` goes to zero" and the user-visible
delta is smaller. **Action:** capture a Tracy frame on mc2_10 with subzones expanded
under `mlr_total` before implementation; record the gosFX-attributable share. If the
share is <50 % the slice still ships but the goal-metric in §7 must scale down.

### R-2 — `Effect::Draw` reentrance

`Effect::Draw` (effect.cpp:798) may be invoked more than once per frame in some FX
configurations (multi-pass / multi-layer). A naive Spawn-at-Draw insertion would
double-emit. **Action at implementation time:** grep `Effect::Draw` callers; confirm
single-call-per-frame-per-instance, or gate the Spawn on an `m_lastSpawnFrame`
counter.

### R-3 — Member-lifetime semantics

`Singleton::Execute` populates `m_color.{r,g,b,a}` at `singleton.cpp:241-244` and
`m_halfY` / `m_halfX` / `m_radius` in each subclass's `Execute` (Card writes them at
`card.cpp:364-366` mirroring the Start fall-through). These members must be valid at
`Draw` time, not just at `Execute` time. Verified by reading `Singleton::Execute`:
the members are plain instance state with no execute-scope tear-down. Safe to read
from `Draw`. **Implementation must not assume the same for any field not on this
list** — if more state is needed, re-verify per-field.

### R-4 — Energy-preserving sub-pixel alpha + additive blends

The proposed shader change scales alpha by `pxSize^2` when projected size falls
below one pixel. This preserves total luminous flux under **alpha-blended**
compositing. Most gosFX effects are additive-blended sprite billboards
(explosions, muzzle flash, smoke trails), and additive blends already preserve
energy correctly under-sample without an alpha rescale. Applying the `pxSize^2`
scale to an additive billboard halves the visible far-away brightness twice.
**Mitigation:** branch the shader sub-pixel rescale on the blend-mode bit carried
in the GPU particle record. If the record does not yet carry blend mode, that
field is added in this slice. Confirm at implementation: enumerate gosFX
subclasses by stock blend mode (additive vs alpha-blend) and verify the GPU
particle record carries enough bits.

### R-5 — Producer-signature churn

Option A in §5.6 grows the Spawn signature for each producer (color RGBA, halfX,
halfY, radius, lifetime). Five producers each need a signature change plus
callsite updates at the new `Draw`-side hook. Signature churn is mechanical; the
risk is forgetting a callsite. **Mitigation:** delete the old signature outright
(do not overload). Compiler will fail on every missed callsite.

### R-6 — Legacy MLR off-path correctness under `MC2_GPU_PARTICLES=0`

Under env-off the legacy MLR chain must run unchanged. The `Start`-side C9/C17
fall-through fixes already ensure legacy `Execute`/`Draw` walk valid state. Spec
preserves them. Verified by reading the comment block at `card.cpp:355-361`
(C17 rationale: legacy init runs always; B1 emit is additive on top under env-on).
After this slice, B1 emit becomes a Draw-side branch instead of additive in Start;
the same off-path safety property holds.

### OQ-2 — Particle persistence in the GPU batch

Today: one Spawn per `Start` ==> one record per particle instance, lifetime owned by
the GPU age-from-spawn-timestamp shader logic (which doesn't actually work — hence
the caveat). After Stage 2': one Spawn per `Draw` ==> one record **per frame per
active particle**, lifetime = one frame. The batcher's `records_per_flush_max` will
rise (§7). **Open:** confirm the batcher capacity is sized for this. Current
`gos_particle_bridge.cpp` reset-per-flush semantics are already correct for this
shape (per the handoff). Implementation must measure peak per-flush record count on
mc2_10 worst-window and confirm headroom.

---

## 9. What's deleted (substitutive accounting)

Under `MC2_GPU_PARTICLES=1`, the following lines have **zero callers from gosFX**
after this slice. They are NOT removed in this slice (consumer-death pattern; actual
excision is a follow-up tied to unified-projection v2):

- `mlrpointcloud.cpp:115` — per-vertex CPU `(*transformedCoords)[i].Multiply(points[i], effectToClipMatrix)`. Non-gosFX callers may persist (R-1).
- `mlrclipper.cpp:679` — `MLRClipper::DrawEffect (DrawEffectInformation *dInfo)`. gosFX is the documented dominant caller; this entry point goes silent on the gosFX side.
- `mlrclipper.cpp:316,321` — `worldToClipMatrix` composition. Composed unconditionally during clipper construction; orphaned of gosFX consumer but cheap and not on the per-vertex path.

**Memory file to create at implementation time:**
`memory/gosfx_mlr_consumer_death.md` — list the pending-removal symbols above, link
to this spec, mark as "ready for excision once unified-projection v2 lands and any
non-gosFX MLR caller has been audited."

---

## 10. Cross-references

- `memory/HANDOFF_2026_05_21_b1_stable_invisible_clipspace_test_next.md` — current
  B1 state, the clip-space-identity test plan superseded by this spec, single-frame
  caveat, parent_age and size-floor hacks.
- `memory/HANDOFF_2026_05_20_late_alpha_stage_1_ship_unified_projection_draft.md` —
  R-MLR HIGH-RISK item this slice unblocks for unified-projection v2.
- `memory/feedback_offload_must_be_substitutive_not_additive.md` — substitutive
  discipline. Zone-death is the ship metric.
- `memory/verify_producer_path_against_telemetry_before_substitution.md` — R-1
  rationale; producer-path identity must be verified by capture before claiming the
  substitution.
- `memory/architecture_md_stale_objectmanager_via_mlr.md` — context for why the MLR
  clipper layer is in legacy-retirement orbit at all.
- `memory/cull_gates_are_load_bearing.md` — relevant to R-6 (off-path); legacy
  gosFX is gated through standard mech/object cull, so the off-path must remain
  consistent.
- `memory/mc2_10_is_only_tier1_with_gosfx.md` — smoke gate scope justification.
- `memory/gpu_direct_renderer_bringup_checklist.md` — checklist this slice's
  implementation must walk (zero implicit GL state, explicit device-mediated binding,
  shader-recompile lockstep, etc.).
- `docs/render-perf-snapshot.md` — refresh after the slice lands to record the new
  `mlr_total` value and gosFX share retirement state.

---

## 11. Out of spec scope (recorded so the next planner doesn't redo this)

- **Removing `mlrclipper.cpp:679 DrawEffect` entirely.** Consumer-death only here;
  excision tied to unified-projection v2 + non-gosFX caller audit.
- **GPU-side age advancement.** B-scope; the shader-side curve evaluation that would
  remove the need for per-frame CPU re-emit lives in a later slice. Today we accept
  one record per particle per frame.
- **Retiring `Singleton::Execute`.** Cold, semantic simulator, stays.
- **`max(p.size, 8.0)` floor** — replaced by the energy-preserving analytic, not
  retained. (Cited as deleted in §5.7 to keep the substitutive accounting honest.)
