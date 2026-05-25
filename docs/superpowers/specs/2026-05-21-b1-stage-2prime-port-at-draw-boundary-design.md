# B1 Stage 2' — Port gosFX at `Effect::Draw` Boundary; gosFX-Card-family MLR Retirement

**Date:** 2026-05-21
**Revision:** v2 (adversarial-review fold-in: PertCloud + honest scope + premult-fold
blend + per-subclass Tracy + reentrance audit + line-drift re-grep).
**Branch base:** `claude/nifty-mendeleev` @ `5233e70` / HEAD `0ca63c1` (axis-swap +
`age=0.5` hack made particles visible for one frame; C9/C11/C14/C17 stack stable on
mc2_10 60s+120s smokes but particles still invisible).
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
offloads net ~0 ms. The gosFX-Card-family share of the 412 us MLR cost must die for
B1 to qualify as a real offload.

Cross-ref: `memory/HANDOFF_2026_05_21_b1_stable_invisible_clipspace_test_next.md`.

---

## 2. Goals

(a) Eliminate the single-frame visibility caveat (one Spawn per active particle per
    frame, sourced from Executed-state members).
(b) Retire the **gosFX-Card-family share** of `mlr_total` per-frame cost
    **substitutively** (the gosFX-Card-family work goes away under env-on; non-Card-
    family MLR consumers REMAIN ALIVE — see §6 and §9).
(c) Retire the `age=0.5` constant and the `max(p.size, 8.0)` shader floor.
(d) **Partially** unblock the unified-projection v2 HIGH-RISK R-MLR item: gosFX-Card-
    family share of MLR is gone, but full MLR retirement waits for a later slice that
    retires Shape/ShapeCloud/DebrisCloud + direct `theClipper` consumers
    (`memory/HANDOFF_2026_05_20_late_alpha_stage_1_ship_unified_projection_draft.md`).

## 3. Non-goals

(a) Retiring `Singleton::Execute` curve resample — it is cold (~54 us p95) and is the
    semantic simulator. Stays alive.
(b) GPU-side simulation / compute-tick age advance. That is B-scope, separate later
    slice.
(c) **Actively** removing the legacy MLR clipper this slice. The gosFX-Card-family
    consumers go silent under env-on but `MLRClipper::DrawShape` and
    `MLRClipper::DrawScalableShape` (Shape/ShapeCloud/DebrisCloud) and direct
    `theClipper` callers (artlry / missiongui / carnage / terrobj / weaponbolt) keep
    the clipper alive. The actual code excision is a follow-up after those consumers
    retire.
(d) Unified-projection v2. This slice partially unblocks but does not deliver it.
(e) B2 (skipping legacy `Execute` under env-on). `Execute` is the simulator and stays
    alive.
(f) PertCloud unique animation semantics. PertCloud spawns one record per active
    particle this slice; if the legacy PertCloud per-vertex "perturbation" animation
    (handled in `MLRIndexedTriangleCloud`-derived primitive) is visually distinct
    from a static billboard, that fidelity gap is filed as B2 polish debt. The
    Card-family billboard rendering is the unified target.

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
      |                           m_radius, m_scale) — see singleton.cpp:241-245
      v
    Draw(DrawInfo)           [per-frame, HOT in legacy path]
      |                        - LEGACY (env-off OR non-Card-family path):
      |                          MLR per-vertex Multiply + clip
      |                          (mlrpointcloud.cpp:115)
      |                        - NEW (env-on, Card-family):
      |                          emit one record to GPU batch with
      |                          CURRENT (just-Executed) state; the
      |                          per-leaf MLR enqueue (info->m_clipper->
      |                          DrawEffect(...)) is env-gated to no-op.

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

### After Stage 2'

```
    Start                    [one-shot]
      | - removed Spawn calls (kept C9/C17 legacy parent-Start fall-through)
      v
    Execute(...)             [unchanged — still samples curves into
      |                       m_color / m_halfY / m_halfX / m_radius / m_scale]
      v
    Draw(DrawInfo)           [overridden in each of 6 Card-family subclasses]
      | (KEEP) tail call to parent Draw (Singleton/SpinningCloud/Effect)
      |        so child Effects continue to receive Draw; env-gate is at the
      |        per-leaf MLR-enqueue inside EACH leaf Draw, not at the parent
      |        walk.
      | + mc2::particles::Spawn*(...) using CURRENT m_color, m_halfY, m_age, ...
      | + MC2_GPU_PARTICLES=1: the local info->m_clipper->DrawEffect(&dInfo)
      |                        call is SKIPPED (the work-leaf no-ops).
      | + MC2_GPU_PARTICLES=0: legacy DrawEffect runs (parity).
      v
```

**Tail-Draw policy (per adversarial M5):** the env-gate is at the per-leaf MLR
enqueue (the `info->m_clipper->DrawEffect(&dInfo)` line), **not** at the parent-tail
Draw. We keep the tail (`Singleton::Draw(info)` / `SpinningCloud::Draw(info)` /
`Effect::Draw(info)`) under env-on so child Effects iterated by `Effect::Draw`
(`effect.cpp:826`) still receive their `Draw` call. The gate is per-leaf-Draw, not
per-parent-Draw; each leaf carries its own gate; composition works correctly.

---

## 5. Surgical changes (per file; grep-verified at write time)

All file:line citations re-verified against worktree HEAD `0ca63c1` immediately
before this spec was written. Symbols stable, lines may drift before implementation —
re-grep at implementation time. The Card-family is **6 subclasses**:
Card, CardCloud, PertCloud, PointCloud, ShardCloud, Tube. PertCloud was missed in
v1 of this spec (it has a `Draw(DrawInfo*)` override at `pertcloud.cpp:394` and a
`DrawEffect` enqueue at `pertcloud.cpp:678`, but no Start override and no Spawn
plumbing today; see §5.6.).

### 5.1 `mclib/gosfx/card.cpp`

- `Card::Start` at `card.cpp:347` (entry); the Spawn block sits at
  `card.cpp:369-371`:
  ```
  if (mc2::particles::Batcher::is_enabled()) {
      (void)mc2::particles::Spawn(m_specification, &m_localToWorld, (float)m_seed);
  }
  ```
  — **remove** this block.
- **Keep** the C17 legacy parent-Start fall-through above it
  (`card.cpp:362-367`: `m_halfY` / `m_halfX` / `m_radius` init via
  `spec->m_halfHeight.ComputeValue` etc. + `m_cardCloud->TurnOn(0)`).
  Crash-class fix; separate correctness contract; preserved.
- `Card::Draw(DrawInfo *info)` at `card.cpp:493` — **add** Tracy zone
  `ZoneScopedN("gosFX::Card::Draw")` at function entry (per M4). **Add** Spawn call
  near the top (after `Check_Object` / `Check_Pointer`, before the `MidLevelRenderer::DrawEffectInformation dInfo;` block at `card.cpp:503`). Pass current
  per-instance state (Card has `m_color`, `m_halfY`, `m_halfX`, `m_radius`, etc.
  already populated by Execute). **Env-gate** the `info->m_clipper->DrawEffect(&dInfo)`
  call at `card.cpp:566`: under `MC2_GPU_PARTICLES=1`, skip the DrawEffect
  enqueue. The trailing `Effect::Draw(info)` / `Singleton::Draw(info)` tail walk
  stays unconditional (per §4 tail-Draw policy).

### 5.2 `mclib/gosfx/cardcloud.cpp`

- `CardCloud::Start` at `cardcloud.cpp:898` (entry); Spawn block at
  `cardcloud.cpp:909-911` — **remove** the Spawn block. **Keep** the C9 fall-through
  `SpinningCloud::Start(info);` at `cardcloud.cpp:907`.
- `CardCloud::Draw(DrawInfo *info)` at `cardcloud.cpp:487` — **add** Tracy zone
  `ZoneScopedN("gosFX::CardCloud::Draw")` at function entry. **Add** Spawn at top
  before the `DrawEffectInformation dInfo;` block at `cardcloud.cpp:499`.
  **Env-gate** `info->m_clipper->DrawEffect(&dInfo)` at `cardcloud.cpp:877`. Keep
  the tail `SpinningCloud::Draw(info)`.

### 5.3 `mclib/gosfx/pertcloud.cpp` (NEW IN v2 — C1 fold-in)

- **No `PertCloud::Start` override exists today** (re-grep `::Start\(` in
  `pertcloud.cpp` returned no hits). PertCloud inherits Start from
  `SpinningCloud`/`ParticleCloud` and inherits the parent C9 fall-through
  transitively. No Start-side Spawn to remove. **No new Start override is added**
  in this slice — the Draw-boundary Spawn is the only producer-side hook.
- `PertCloud::Draw(DrawInfo *info)` at `pertcloud.cpp:394` — **add** Tracy zone
  `ZoneScopedN("gosFX::PertCloud::Draw")` at function entry. **Add** Spawn at top
  before the `DrawEffectInformation dInfo;` block at `pertcloud.cpp:406`.
  **Env-gate** `info->m_clipper->DrawEffect(&dInfo)` at `pertcloud.cpp:678`. Keep
  the tail `SpinningCloud::Draw(info)` at `pertcloud.cpp:681`.

### 5.4 `mclib/gosfx/pointcloud.cpp`

- `PointCloud::Start` at `pointcloud.cpp:504` (entry); Spawn block at
  `pointcloud.cpp:522-524` — **remove** the Spawn block. **Keep** the C9
  fall-through `ParticleCloud::Start(info);` at `pointcloud.cpp:520`.
- `PointCloud::Draw(DrawInfo *info)` at `pointcloud.cpp:463` — **add** Tracy zone
  `ZoneScopedN("gosFX::PointCloud::Draw")` at function entry. **Add** Spawn at top
  before the `DrawEffectInformation dInfo;` block at `pointcloud.cpp:470`.
  **Env-gate** `info->m_clipper->DrawEffect(&dInfo)` at `pointcloud.cpp:480`. Keep
  the tail `ParticleCloud::Draw(info)`.

### 5.5 `mclib/gosfx/shardcloud.cpp`

- `ShardCloud::Start` at `shardcloud.cpp:682` (entry); Spawn block at
  `shardcloud.cpp:694-696` — **remove**. **Keep** the C9 fall-through
  `SpinningCloud::Start(info);` at `shardcloud.cpp:692`.
- `ShardCloud::Draw(DrawInfo *info)` at `shardcloud.cpp:307` — **add** Tracy zone
  `ZoneScopedN("gosFX::ShardCloud::Draw")` at function entry. **Add** Spawn at top
  before the `DrawEffectInformation dInfo;` block at `shardcloud.cpp:319`.
  **Env-gate** `info->m_clipper->DrawEffect(&dInfo)` at `shardcloud.cpp:678`. Keep
  the tail `SpinningCloud::Draw(info)` at `shardcloud.cpp:681`.

### 5.6 `mclib/gosfx/tube.cpp`

- `Tube::Start` at `tube.cpp:729` (entry); Spawn block at `tube.cpp:756-758` —
  **remove**. **Keep** the C17 legacy parent-Start fall-through
  (`m_birthAccumulator = 1.0f;` at `tube.cpp:754` + the preceding comment block
  and `Effect::Start(info);` at `tube.cpp:739`).
- `Tube::Draw(DrawInfo *info)` at `tube.cpp:1163` — **add** Tracy zone
  `ZoneScopedN("gosFX::Tube::Draw")` at function entry. **Add** Spawn at top
  before the `DrawEffectInformation dInfo;` block at `tube.cpp:1175`. **Env-gate**
  `info->m_clipper->DrawEffect(&dInfo)` at `tube.cpp:1297`. Keep the tail
  `Effect::Draw(info)` at `tube.cpp:1300`.

### 5.7 `mc2::particles::SpawnPertCloud` (NEW — C1 fold-in)

`mc2::particles::Spawn` (`mclib/particles/spawn.cpp:24-60`) today dispatches Card /
CardCloud / PointCloud / ShardCloud / Tube only; the `default:` branch returns
`false` for `Pert / Shape / Debris / EffectCloud` per `spawn.h:20-24` comment.
PertCloud is in the Card family per this slice, so a `SpawnPertCloud` helper is
required.

Files added:
- `mclib/particles/spawn_pertcloud.h` (declaration; mirrors `spawn_cardcloud.h`).
- `mclib/particles/spawn_pertcloud.cpp` (definition; mirrors `spawn_cardcloud.cpp`).

Dispatcher update:
- `mclib/particles/spawn.cpp:35-58` — add `case gosFX::PertCloudClassID:` arm
  mirroring the CardCloudClassID arm; `#include "spawn_pertcloud.h"` near
  `mclib/particles/spawn.cpp:16`.
- `mclib/particles/spawn.h:11-24` — update the docstring: PertCloud moves OUT of
  the deferred list and into the dispatched list.
- `mclib/gosfx/pertcloud.hpp` — confirm `gosFX::PertCloud__Specification` is the
  spec class (re-grep at impl time).
- CMake: add `spawn_pertcloud.cpp` to `mclib/particles/CMakeLists.txt` particle
  sources list (re-grep at impl time).

PertCloud spawn semantics: one record per active particle per frame, identical
shape to the other 5 helpers (position from `m_localToWorld`, size from already-
Executed `m_halfY` member, color from Executed `m_color`). No curve resampling
on the producer side (Option A per §5.8).

### 5.8 Producer-side curve sampling (6 files)

The current `spawn_*.cpp` files **sample curves themselves** at the synthesized
`age=0.5` because the Spawn call runs from `Start` before `Execute` has populated
`m_color`/`m_halfY`/etc. After the boundary move, the producers have two viable
shapes:

- **Option A (preferred):** Spawn signatures grow explicit per-frame state
  parameters (color RGBA, halfX/halfY, radius, lifetime-remaining). The subclass
  `Draw` reads its already-resampled members (`m_color.red`, `m_halfY`, ...) and
  passes them in. Producers stop sampling curves entirely. **Delete** the
  `const Stuff::Scalar parent_age = 0.5f;` line (or `age = 0.5f` in spawn_card)
  and every `mut_spec->m_X.ComputeValue(age, seed)` block in each producer.

- **Option B:** Keep producer signatures as-is, pass `m_age` through, let the
  producers `ComputeValue(m_age, m_seed)`. Functionally equivalent but duplicates
  the `Singleton::Execute` curve-sample work the engine already did this frame.
  Wasteful. Reject.

This spec adopts **Option A**.

Producer changes (each must be re-verified at impl time):

| File | Action |
|---|---|
| `mclib/particles/spawn_card.cpp:71` | Delete `const Stuff::Scalar age = 0.5f;` and the four `ComputeValue(age, seed)` blocks at lines 80-92, 96-97. Grow signature to accept resolved state from caller. |
| `mclib/particles/spawn_cardcloud.cpp:83` | Delete `const Stuff::Scalar parent_age = 0.5f;` and curve-sample block. Grow signature. |
| `mclib/particles/spawn_point.cpp:83` | Same. |
| `mclib/particles/spawn_shard.cpp:107` | Same. |
| `mclib/particles/spawn_tube.cpp:97` | Same. |
| `mclib/particles/spawn_pertcloud.cpp` (NEW) | Born Option A: no `parent_age` constant, no curve sample; signature takes resolved state. |

### 5.9 `shaders/particle_billboard.vert`

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
  See R-4 below for the per-blend-mode caveat; with the premultiplied-fold blend
  strategy in §5.10, the alphaScale applies uniformly because additive paths
  use the same `(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` op with alpha=0.

### 5.10 Blend strategy: premultiplied-alpha fold (per adversarial M1 + C3)

The current bridge sets `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` at
`GameOS/gameos/gos_particle_bridge.cpp:198`. Legacy gosFX has two blend families
(alpha-over and additive); the GPU path today renders everything as alpha-over,
which under-saturates additive FX (explosions, muzzle flash).

**Strategy:** premultiplied-alpha fold. Single flush, single draw, single
`glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`. The FS writes:
- For ALPHA-blend particles: `outColor.rgb = color.rgb * alpha; outColor.a = alpha;`
  → blend yields `src.rgb*1 + dst*(1-alpha) = color.rgb*alpha + dst*(1-alpha)`
  (correct alpha-over).
- For ADDITIVE particles: `outColor.rgb = color.rgb * alpha; outColor.a = 0.0;`
  → blend yields `src.rgb*1 + dst*(1-0) = dst + color.rgb*alpha` (correct
  additive, since the `(1 - srcA)` factor collapses to 1.0).

**Blend-mode bit packing (M1, no struct grow):** the existing `GpuParticle`
struct (`mclib/particles/spec.h:41-51`) has `uint32_t atlasIndex` at offset 60
(asserted at `mclib/particles/spec.h:66`). Pack blend mode in the upper 4 bits:
- C++ producer writes: `p.atlasIndex = (page & 0x0FFFFFFFu) | ((blendMode & 0xFu) << 28u);`
- GLSL VS reads:
  - texture page: `int(p.atlasIndex & 0x0FFFFFFFu)` (replace any direct cast in
    `shaders/particle_billboard.vert` — currently no atlasIndex consumer there at
    HEAD; the FS will be the consumer per below).
  - blend mode: `uint(p.atlasIndex >> 28) & 0xFu;` — passed via flat-qualified out
    varying `flat out uint v_blendMode;` to the FS.
- GLSL FS (`shaders/particle_billboard.frag`) reads `v_blendMode`; folds rgb*alpha
  and conditionally zeros alpha for ADDITIVE.

No struct size change. No stride change. No std430 churn. The
`offsetof(GpuParticle, atlasIndex) == 60` assert at `spec.h:66` stands unchanged
(field is still `uint32_t` at offset 60; only the interpretation of the high bits
changes).

Recommended owner of the fold: **FS** (simpler, keeps VS focused on geometry +
projection; avoids smuggling the flag through a second varying).

Bridge change:
- `GameOS/gameos/gos_particle_bridge.cpp:198` — change
  `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` to
  `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`. The save/restore around it
  (`gos_particle_bridge.cpp:210`) stays as-is; the saved `savedSrcRGB` /
  `savedDstRGB` are still the right things to restore.

Blend-mode source: each Card-family subclass reads its `m_specification`'s blend
mode (gosFX spec carries this; field name `m_blendMode` / `m_state` flags — re-grep
the gosFX MLRState definition at implementation time and confirm the additive
vs alpha-over distinction propagates). The subclass `Draw` resolves the blend
mode at the Spawn callsite and passes it into the Spawn helper as a parameter.

Unit-of-correctness check (user A/B): under env-off (legacy MLR path), additive
FX render with their correct saturation; under env-on (new GPU path), the same FX
render visually equivalent (no under-saturated explosions, no over-bright smoke).
Driven by the smoke gate §7 visual canary.

---

## 6. Substitutive proof (gosFX-Card-family share only)

Per `memory/feedback_offload_must_be_substitutive_not_additive.md`: zone-death is the
ship metric. **This slice retires the gosFX-Card-family share of `mlr_total` only,
not the whole `mlr_total`.**

Non-Card-family MLR consumers that REMAIN ALIVE after this slice:

- `MLRClipper::DrawShape` at `mclib/mlr/mlrclipper.cpp:400` (per v3 audit). Consumers:
  none direct in gosFX/ today (per v3 audit §5 "zero external callers").
- `MLRClipper::DrawScalableShape` at `mclib/mlr/mlrclipper.cpp:565`. Consumers
  (re-grep at impl time):
  - `mclib/gosfx/shape.cpp:317`
  - `mclib/gosfx/shapecloud.cpp:394, :459, :525, :568`
  - `mclib/gosfx/debriscloud.cpp:851`
- `MLRClipper::DrawEffect` at `mclib/mlr/mlrclipper.cpp:679`. After this slice the
  6 gosFX-Card-family callers (Card / CardCloud / PertCloud / PointCloud /
  ShardCloud / Tube — verified by grep of `DrawEffect` in `mclib/gosfx/` at lines
  `card.cpp:566`, `cardcloud.cpp:877`, `pertcloud.cpp:678`, `pointcloud.cpp:480`,
  `shardcloud.cpp:678`, `tube.cpp:1297`) are env-gated.
- `mlrclipper.cpp:316,321` — `worldToClipMatrix` composition. **Still alive** —
  consumed by `DrawShape` and `DrawScalableShape` paths (Shape/ShapeCloud/
  DebrisCloud). **NOT** orphaned by this slice.
- Direct `theClipper` entry points (these set `drawInfo.m_clipper = theClipper`
  then call `gosFX::Effect::Draw`; whether they hit Card-family or non-Card-family
  leaves depends on the effect type loaded):
  - `code/artlry.cpp:87` (extern decl), `:1339`, `:1423`.
  - `code/missiongui.cpp:145` (extern decl), `:2904`, `:2927`.
  - `code/carnage.cpp:66` (extern decl), `:830`.
  - `code/terrobj.cpp:281` (extern decl), `:974`.
  - `code/weaponbolt.cpp:49` (extern decl), `:1610`.
  These callsites do NOT change. When the effect they Draw is a Card-family leaf,
  the env-gated MLR-enqueue inside that leaf no-ops. When it is a Shape/
  ShapeCloud/DebrisCloud, the MLR enqueue runs.

The substitutive claim, narrowed: `mlrpointcloud.cpp:115`'s per-vertex
`Multiply(points[i], effectToClipMatrix)`, **invoked from gosFX-Card-family
DrawEffect paths**, ceases to be invoked once each subclass `Draw` env-gates the
DrawEffect enqueue. Per the v3 audit, `mlrpointcloud.cpp:115` is reached via
`MLRClipper::DrawEffect` → primitive draw → `MLRPointCloud::TransformAndDraw`. The
6 gosFX-Card-family callers above are the documented dominant invokers.

R-1 below holds that the **gosFX-Card-family share** of `mlr_total` must be
quantified by Tracy capture before the slice is declared shipped — this is no
longer "the whole `mlr_total` retires"; it is "the Card-family share retires."

Full MLR retirement (Shape / ShapeCloud / DebrisCloud consumers + direct theClipper
callers feeding non-Card-family leaves) is **out of scope for this slice** and is a
prerequisite for unified-projection v2's R-MLR resolution.

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
  single-frame caveat. **Additive FX (explosions, muzzle flash) must render with
  the same brightness/saturation as env-off** (premultiplied-fold correctness check
  per §5.10).
- **Per-subclass Tracy zones (M4):** the 6 subclass `Draw` zones must be present
  in the capture and behave as follows:
  - Under `MC2_GPU_PARTICLES=0`: `gosFX::Card::Draw`, `gosFX::CardCloud::Draw`,
    `gosFX::PertCloud::Draw`, `gosFX::PointCloud::Draw`, `gosFX::ShardCloud::Draw`,
    `gosFX::Tube::Draw` — **all 6 present with non-zero time** (baseline).
  - Under `MC2_GPU_PARTICLES=1`: same 6 zones present, **time drops to ~0** (Spawn
    path is constant-time; the legacy MLR-feeding DrawEffect enqueue is gated out;
    only Tracy overhead + Spawn `Emit` + parent-tail walk remain).
- **Telemetry gate (NARROWED per C2):** `mlr_total` as a **whole** may STILL be
  material under env-on (Shape/ShapeCloud/DebrisCloud + artlry/missiongui/carnage
  paths feed it from non-Card-family leaves). **Do NOT gate on raw
  `mlr_total < 50us`.** Gate instead on:
  - The 6 per-subclass Draw zones drop to ~0 under env-on (above).
  - A new `mc2::particles::flush` Tracy zone (add inside `Batcher::Flush` or
    `gos_particle_bridge_flush` if it doesn't already exist) is non-zero under
    env-on, zero under env-off — proves the new path is doing work.
  - `mlr_total` worst-window p95 under env-on **decreases by the gosFX-Card-family
    share** measured via R-1 pre-capture (delta, not absolute target).
- **`[GPU_PARTICLES v1]` summary:** `records_per_flush_max` should rise materially —
  every active Card-family particle now re-emits every frame from Execute-driven
  state, rather than once at construction. The smoke artifact log should contain
  `[GPU_PARTICLES v1]` lines proving Spawn ran from Draw, AND a `[FX_TRACE v1]`
  line (existing prefix in `mclib/fx_trace/fx_trace.cpp:53-80`) proving legacy
  Start init ran. Both prefixes are non-overlapping and already used.
- **CI:** `scripts/check-particles-no-cpu-projection.sh` continues to pass (producers
  remain projection-free; the new Draw-side Spawn call must not re-introduce any
  forbidden projection wrapper).

---

## 8. Risks + open questions

### OQ-1 — Draw override exhaustiveness (resolved at spec time)

Each of the 6 Card-family subclasses has a `Draw(DrawInfo *)` override at HEAD;
verified by grep:
- `card.cpp:493`, `cardcloud.cpp:487`, `pertcloud.cpp:394`, `pointcloud.cpp:463`,
  `shardcloud.cpp:307`, `tube.cpp:1163`.
None inherit `Singleton::Draw` directly. Tube chains through `Effect::Draw` at the
tail (`tube.cpp:1300`); Card chains through `Singleton::Draw` (resolves to
`Effect::Draw`); CardCloud / PertCloud / ShardCloud chain through `SpinningCloud::
Draw` (`cardcloud.cpp:880`, `pertcloud.cpp:681`, `shardcloud.cpp:681`); PointCloud
through `ParticleCloud::Draw` (`pointcloud.cpp:482`). The Spawn call goes at the
head of each subclass override.

### R-1 — `mlr_total` composition (NARROWED per C2)

`mlr_total` includes MLR work from non-Card-family callers (Shape, ShapeCloud,
DebrisCloud + artlry/missiongui/carnage non-Card-family effects). **Action:**
capture a Tracy frame on mc2_10 with subzones expanded under `mlr_total` before
implementation; record:
- gosFX-Card-family-attributable share (callers in §6 list above).
- Non-Card-family share (Shape/ShapeCloud/DebrisCloud paths).
The §7 telemetry gate uses the Card-family share as the substitutive target,
NOT the whole `mlr_total`.

### R-2 — `Effect::Draw` reentrance (RESOLVED per M2 fold-in)

Adversarial M2 mandated a code-grounded answer. Result of the audit:

- `Effect::m_children` (`effect.cpp:670`: `m_children.Add(effect);`) is populated
  in `Effect::Execute` event handling — children are MakeEffect'd from event-list
  entries and added to the parent's `m_children` chain.
- `EffectCloud::particle->m_effect` (`effectcloud.cpp:191`: `particle->m_effect =
  EffectLibrary::Instance->MakeEffect(...)`) is populated in `EffectCloud::Create
  NewParticle` and stored on the **particle**, NOT added to the parent's
  `m_children` chain. `EffectCloud::Draw` (`effectcloud.cpp:324`) walks
  `particle->m_effect->Draw(info)` directly; the trailing `SpinningCloud::Draw(info)`
  (`effectcloud.cpp:330`) eventually reaches `Effect::Draw` which iterates
  `m_children` (`effect.cpp:826`) — but `m_children` does NOT contain the
  `particle->m_effect` instances. So the two populations are disjoint.
- **Conclusion:** for the Card-family leaves at Stage 2', no Effect instance is
  Draw'd twice in the same frame from the same root traversal. **No
  `m_lastSpawnFrame` guard is required.** The Spawn emit at the head of each
  Card-family subclass `Draw` runs exactly once per Effect instance per frame.

If a future slice promotes Shape / ShapeCloud / DebrisCloud / EffectCloud into the
GPU pipeline, the audit must be re-run (EffectCloud's `particle->m_effect`
recursion path goes through arbitrary subclass `Draw` overrides; double-counting
exposure depends on which Effect type the particle holds).

### R-3 — Member-lifetime semantics

`Singleton::Execute` populates `m_color.{r,g,b,a}` at `singleton.cpp:241-244` and
`m_halfY` / `m_halfX` / `m_radius` in each subclass's `Execute` (Card writes them at
`card.cpp:364-366` mirroring the Start fall-through). These members must be valid at
`Draw` time, not just at `Execute` time. Verified by reading `Singleton::Execute`:
the members are plain instance state with no execute-scope tear-down. Safe to read
from `Draw`. **Implementation must not assume the same for any field not on this
list** — if more state is needed, re-verify per-field. PertCloud carries
`SpinningCloud` per-particle state (`m_activeParticleCount`, per-particle
`Particle` array at `pertcloud.cpp:308-326`); the Spawn emits **one record per
active particle**, sourced from those per-particle slots after Execute has
advanced them.

### R-4 — Energy-preserving sub-pixel alpha (RESOLVED per §5.10)

With the premultiplied-fold blend strategy, both alpha-over and additive paths
go through the same `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` op. The
`pxSize^2` alphaScale at §5.9 applies uniformly to the FS-emitted `outColor.rgb`
(which is `color.rgb * alpha` in both modes); for ADDITIVE the FS also forces
`outColor.a = 0`, which is unaffected by the sub-pixel alphaScale. Energy
preservation holds in both modes.

### R-5 — Producer-signature churn

Option A in §5.8 grows the Spawn signature for each producer (color RGBA, halfX,
halfY, radius, lifetime, **blend mode** per §5.10). Six producers each need a
signature change plus callsite updates at the new `Draw`-side hook (5 existing +
1 new SpawnPertCloud). Signature churn is mechanical; the risk is forgetting a
callsite. **Mitigation:** delete the old signature outright (do not overload).
Compiler will fail on every missed callsite. Also: `mc2::particles::Spawn`
dispatcher (`mclib/particles/spawn.cpp:24`) gains a new arm + must be updated to
forward resolved state to each helper — the dispatcher's three-argument signature
(`spec`, `parentToWorld`, `spawnSeed`) is no longer sufficient. Two options:
- (i) widen the dispatcher signature to include resolved state. Callers (the 6
  subclass `Draw`s) pass everything.
- (ii) drop the dispatcher; have each subclass `Draw` call its specific
  `SpawnCard` / `SpawnCardCloud` / `SpawnPertCloud` / ... directly (subclass
  already knows its own type). Simpler. Spec adopts **(ii)**.

### R-6 — Legacy MLR off-path correctness under `MC2_GPU_PARTICLES=0`

Under env-off the legacy MLR chain must run unchanged. The `Start`-side C9/C17
fall-through fixes already ensure legacy `Execute`/`Draw` walk valid state. Spec
preserves them. Verified by reading the comment block at `card.cpp:353-361`
(C17 rationale: legacy init runs always; B1 emit was additive on top under env-on).
After this slice, B1 emit becomes a Draw-side branch instead of additive in Start;
the same off-path safety property holds. The env-gate is purely around the
DrawEffect enqueue line; everything else (Execute, tail-Draw, child walk) runs
unconditionally.

### OQ-2 — Particle persistence in the GPU batch

Today: one Spawn per `Start` ==> one record per particle instance, lifetime owned by
the GPU age-from-spawn-timestamp shader logic (which doesn't actually work — hence
the caveat). After Stage 2': one Spawn per `Draw` ==> one record **per frame per
active particle**, lifetime = one frame. The batcher's `records_per_flush_max` will
rise (§7). **Open:** confirm the batcher capacity is sized for this. Current
`gos_particle_bridge.cpp` reset-per-flush semantics are already correct for this
shape (per the handoff). Implementation must measure peak per-flush record count on
mc2_10 worst-window and confirm headroom. PertCloud may multiply the per-frame
record count materially (PertCloud's primitive holds many sub-particles per
instance; each becomes one record per frame). Measure on mc2_10 before promoting
default-on.

### OQ-3 — Blend-mode field source on gosFX spec

§5.10 names the blend-mode source as "gosFX spec carries this; field name
`m_blendMode` / `m_state` flags." This needs grep verification at implementation
time. If the gosFX `MLRState` does not surface a clean alpha/additive distinction,
the implementation may need to derive it from the spec's pre-existing state-bit
pattern (e.g. the SetAlphaBlendOn vs SetAdditive bits used at the legacy DrawEffect
sites). Either way, the bit is resolvable from the spec at Spawn time and packs
into the existing `atlasIndex` upper bits — no schema change needed.

---

## 9. What's deleted (consumer-death accounting, gosFX-Card-family ONLY)

Under `MC2_GPU_PARTICLES=1`, the following call paths have **zero invocations from
gosFX-Card-family `Draw` paths** after this slice. They are NOT removed in this slice
(consumer-death pattern; actual excision is a follow-up tied to retiring the
non-Card-family consumers + unified-projection v2):

- **Card-family DrawEffect callsites become env-gated no-ops under env-on:**
  - `mclib/gosfx/card.cpp:566` — `info->m_clipper->DrawEffect(&dInfo);`
  - `mclib/gosfx/cardcloud.cpp:877` — same
  - `mclib/gosfx/pertcloud.cpp:678` — same
  - `mclib/gosfx/pointcloud.cpp:480` — same
  - `mclib/gosfx/shardcloud.cpp:678` — same
  - `mclib/gosfx/tube.cpp:1297` — same
- `mlrpointcloud.cpp:115` — per-vertex CPU `Multiply(points[i], effectToClipMatrix)`.
  **Card-family share** goes to zero. Non-Card-family share survives.
- `mclib/mlr/mlrclipper.cpp:679` — `MLRClipper::DrawEffect(DrawEffectInformation*)`.
  Card-family entry goes silent; **NOT** orphaned (no non-Card-family caller of
  DrawEffect exists today per the v3 audit, but the function body stays alive in
  case any later non-gosFX caller appears, and because the next slice may need it
  during transition).

**Stays alive (NOT orphaned by this slice):**
- `MLRClipper::DrawShape` / `DrawScalableShape` and all `mlrclipper.cpp:316,321`
  worldToClipMatrix composition — consumed by Shape / ShapeCloud / DebrisCloud paths
  (§6 list). Full retirement deferred to a later slice.
- All direct `theClipper` entry sites in `artlry.cpp` / `missiongui.cpp` /
  `carnage.cpp` / `terrobj.cpp` / `weaponbolt.cpp` — they pass `theClipper` into
  `gosFX::Effect::DrawInfo::m_clipper` and call `effect->Draw(&drawInfo)`. The
  callsites are unchanged by this slice; the per-leaf MLR-enqueue inside each
  Card-family leaf is what env-gates.

**Memory file to create at implementation time:**
`memory/gosfx_card_family_mlr_consumer_death.md` — list the per-leaf gated lines
above, link to this spec, mark as "ready for excision once non-Card-family MLR
consumers (Shape / ShapeCloud / DebrisCloud + any future direct callers) also
retire, AND unified-projection v2 lands."

---

## 10. Cross-references

- `memory/HANDOFF_2026_05_21_b1_stable_invisible_clipspace_test_next.md` — current
  B1 state, the clip-space-identity test plan superseded by this spec, single-frame
  caveat, parent_age and size-floor hacks.
- `memory/HANDOFF_2026_05_20_late_alpha_stage_1_ship_unified_projection_draft.md` —
  R-MLR HIGH-RISK item this slice **partially** unblocks for unified-projection v2.
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
- `memory/cpp_glsl_ubo_struct_lockstep.md` — std430 lockstep discipline; the
  blend-mode bit packing in §5.10 does NOT trigger this discipline because the
  schema-visible layout is unchanged (still `uint32_t atlasIndex` at offset 60).
- `docs/superpowers/plans/2026-05-20-integrated-gosfx-retirement-INSTRUMENTATION-AUDIT-v3.md` —
  authoritative MLR work-leaf census; Shape/ShapeCloud/DebrisCloud caller list
  consumed by §6 "stays alive."
- `docs/render-perf-snapshot.md` — refresh after the slice lands to record the
  gosFX-Card-family share retirement state.

---

## 11. Cross-cutting state-change census (per CLAUDE.md adversarial-review skill)

Blend-mode bit packing into `GpuParticle::atlasIndex` upper bits (§5.10) touches
the GpuParticle schema interpretation but NOT its byte layout. Every consumer
that reads `atlasIndex` must mask explicitly. Census (re-grep at impl time):

**C++ producers (6 spawn_*.cpp files — one per Card-family subclass, including new PertCloud):**
- `mclib/particles/spawn_card.cpp` (re-grep current Emit / atlasIndex writer line at impl).
- `mclib/particles/spawn_cardcloud.cpp` (same).
- `mclib/particles/spawn_pertcloud.cpp` (NEW per §5.7).
- `mclib/particles/spawn_point.cpp` (same).
- `mclib/particles/spawn_shard.cpp` (same).
- `mclib/particles/spawn_tube.cpp` (same).
Each must OR the blend-mode bits into `atlasIndex` upper 4 bits before `Emit`.

**C++ schema enforcement:**
- `mclib/particles/spec.h:41-51` — `GpuParticle` struct definition.
- `mclib/particles/spec.h:54-66` — sizeof / alignof / offsetof static_asserts.
  `offsetof(GpuParticle, atlasIndex) == 60` (`spec.h:66`). UNCHANGED by this slice
  (field name and offset stand).

**C++ batcher staging:**
- `mclib/particles/batcher.cpp:18` — `gos_particle_bridge_flush(const GpuParticle*, ...)`.
- `mclib/particles/batcher.cpp:25` — `std::vector<GpuParticle> staging;`.
- `mclib/particles/batcher.cpp:86` — `void Batcher::Emit(const GpuParticle& p)`.
No `atlasIndex` read here; pass-through.

**C++ dispatcher:**
- `mclib/particles/spawn.cpp:24-60` — gains PertCloudClassID arm (§5.7); may be
  retired in favor of direct subclass calls per R-5 option (ii).

**C++ bridge consumer:**
- `GameOS/gameos/gos_particle_bridge.cpp:111` — `sizeof(mc2::particles::GpuParticle)` for SSBO capacity.
- `GameOS/gameos/gos_particle_bridge.cpp:147` — same for upload.
- `GameOS/gameos/gos_particle_bridge.cpp:198` — `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` → change to `(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` per §5.10.
- `GameOS/gameos/gos_particle_bridge.cpp:210` — `glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB)` restore. Stays as-is (restores caller's state).

**GLSL particle struct mirror:**
- `shaders/include/particles.hglsl:21-29` — `struct Particle { ... uint atlasIndex; }`. UNCHANGED (offset comment block at lines 12-19 stays).

**GLSL VS reader:**
- `shaders/particle_billboard.vert:66` — `Particle p = particles[particleId];`.
- `shaders/particle_billboard.vert:78` — `effSize = max(p.size, 8.0)` → energy-preserving formulation per §5.9.
- `shaders/particle_billboard.vert` — currently no `p.atlasIndex` consumer at HEAD
  (grep of `atlasIndex` in this file returned no hits). Add `flat out uint v_blendMode;` and write `v_blendMode = uint(p.atlasIndex >> 28) & 0xFu;`. Texture-page lookup (when introduced) reads `int(p.atlasIndex & 0x0FFFFFFFu)`.

**GLSL FS reader (NEW consumer):**
- `shaders/particle_billboard.frag:24-27` — current trivial passthrough; **modify**
  per §5.10 to read `flat in uint v_blendMode;` and emit
  `outColor.rgb = (tex * v_color).rgb * v_color.a; outColor.a = (v_blendMode == ADDITIVE) ? 0.0 : v_color.a;` (final form TBD at impl; pseudocode).
  The `v_color.a` for the additive case also folds through the §5.9 alphaScale,
  but the alpha-channel write is forced to 0 for additive.

Every item above must cite file:line confirmed at write time. The §11 list is the
authoritative impl-time checklist; nothing not on this list may be touched without
returning to §11.

---

## 12. Out of spec scope (recorded so the next planner doesn't redo this)

- **Removing `mlrclipper.cpp:679 DrawEffect` entirely.** Consumer-death only here;
  excision tied to non-Card-family MLR consumer retirement + unified-projection v2.
- **Retiring Shape / ShapeCloud / DebrisCloud + direct `theClipper` callsites in
  `code/`.** Separate slice; prerequisite for full MLR clipper retirement.
- **GPU-side age advancement.** B-scope; the shader-side curve evaluation that would
  remove the need for per-frame CPU re-emit lives in a later slice. Today we accept
  one record per particle per frame.
- **Retiring `Singleton::Execute`.** Cold, semantic simulator, stays.
- **`max(p.size, 8.0)` floor** — replaced by the energy-preserving analytic, not
  retained (§5.9).
- **PertCloud per-particle "perturbation" fidelity.** This slice renders PertCloud
  particles as static billboards (same as Card / CardCloud). If the legacy
  perturbation animation produces a visually distinct effect (waveform-driven
  vertex displacement on the `MLRIndexedTriangleCloud` primitive), restoring that
  on the GPU side is a B2 polish item.
