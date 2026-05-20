# Appearance::inView unconflation — alpha-Stage 1 design

Date: 2026-05-20. Worktree: `claude/nifty-mendeleev` @ `dc2e8f6` (post-§4
revert, post-§2.5 sticky-bit ship). Status: **DESIGN v3** — corrected
per user blocking review 2026-05-20 (see §9.5 v3 amendments). v1 → v2
folded a 10-finding adversarial pass; v2 → v3 fixes 5 blocking design
defects (no simulation-migration stage, Stage 2b black-tree
resurrection risk, GpuConsumerFlags taxonomy mismatch, circular
lifecycle-destroy condition, Stage 0 metric measures wrong thing).
Adopts the "alpha-Stage 1" name the Stage 0.5 v3 spec carves out for
the deeper META-FIX. Cited symbols all grep-verified at write-time.

Scope: this spec covers the architectural retirement of the single
`Appearance::inView` boolean (`mclib/appear.h:71`) which is currently
read by at least 17 callsites across 4+ distinct semantic concerns
(render, simulation/update, lifecycle/destroy, AI/combat). The
retirement decomposes `inView` into a small set of named bools at the
**producer side** (`recalcBounds()`), promotes the **already-declared
`GpuConsumerFlags` taxonomy** (`gpu_cull_record.h:63-66`) from
instrumentation to live dispatch, and migrates consumers per-concern
under explicit per-stage gates. This spec is NOT a CPU-to-GPU offload
slice; it is a control-flow unconflation campaign. See §1 problem
framing.

## Provenance

- §2.5 sticky-bit shipped `91b6991` (META-FIX of `056c365`). Independent
  of this spec.
- §4 of Stage 0.5 v3 shipped tentatively as `40a54b7`, reverted
  `dc2e8f6` after user-driven mc2_10 visual canary surfaced two
  structural bug classes (readback non-superset + RESURRECTED black-tree
  class). The reversion documented in `memory/stage_0_5_section_4_blocked_on_readback_non_superset.md`.
- This spec is the v3 spec's referenced "alpha-Stage 1" carve-out. v3
  §1.4: "DEEPER META-FIX exists (alpha-Stage 1: drive lifecycle +
  destroy gates off the readback too, then delete the CPU coarse-cull
  producer entirely)." This document is that META-FIX.
- 6 advisor + greybeard + adversarial inputs converged on
  producer-side decomposition. Convergence summary at §3.

---

## 1. Problem framing — gate-pair invariants under conflation

### 1.1 The empirical signal

The Stage 0.5 §4 tentative ship (`40a54b7`) moved one consumer (render
gate) off `inView` while leaving update gate on `inView`. Visual canary
result on mc2_10 worst-case (90s user-driven pan+spin+corner under
`MC2_GPU_CULL=1 MC2_GPU_CULL_READBACK=1`):

- **Bug class 1:** Static-prop / tree / mech POPPING in/out during
  camera motion — readback non-superset (false-negative class).
- **Bug class 2:** BLACK building textures — RESURRECTED 2026-05-05
  black-tree bug class (`memory/black_tree_bug_investigation_state.md`)
  because false-positives admitted actors whose `update()` was
  cull-skipped, leaving `cachedGpuLightIndex_` stale and registry
  flush emitting draws with stale light data.

Neither bug is a probe-tuning failure or a readback envelope failure.
**Both are predictable consequences of a single boolean being asked to
answer two unrelated questions** (should this draw? should this tick?).
Every readback-quality fix would have left the gate-pair-invariant
violation intact.

### 1.2 The conflation

`Appearance::inView` (`mclib/appear.h:71`) is set by `recalcBounds()`.
**6 true virtual overrides** of `Appearance::recalcBounds()`
grep-verified:

1. `mclib/bdactor.cpp:1163` `BldgAppearance::recalcBounds()`
2. `mclib/bdactor.cpp:3926` `TreeAppearance::recalcBounds()`
3. `code/actor.cpp:280` `VFXAppearance::recalcBounds()`
4. `mclib/genactor.cpp:587` `GenericAppearance::recalcBounds()`
5. `mclib/gvactor.cpp:1614` `GVAppearance::recalcBounds()`
6. `mclib/mech3d.cpp:2132` `Mech3DAppearance::recalcBounds()`

Plus the base at `mclib/appear.h:208`. Note: `code/artlry.cpp:1166`
declares `Artillery::recalcBounds(CameraPtr myEye)` — a DIFFERENT
signature (takes `CameraPtr`); NOT a polymorphism override of
`Appearance::recalcBounds()` and outside the lockstep edit
discipline. Adversarial review (2026-05-20) M1 finding.

`inView` (and its accessor `canBeSeen()` at `mclib/appear.h:176-179`,
which returns `inView`) is read by **≥ 30 reader callsites** across
at least 4 semantic concerns (per adversarial review m1 — initial
"17+" undercount was excluding `canBeSeen()` indirect reads):

| Concern | Sample readers | Effective contract |
|---|---|---|
| Render submission | `bdactor.cpp:1302, 1838, 4007, 4268`, `terrobj.cpp:945, 1015`, `bldng.cpp:1081`, `gate.cpp:599`, `artlry.cpp:1334`, `turret.cpp:2034` | "Should this actor be enqueued for draw?" — frustum + size question |
| Simulation / update | `terrobj.cpp:796`, `mech.cpp:6044, 6110, 6182`, `gvehicl.cpp:3270, 3321`, `bldng.cpp:805`, `artlry.cpp:870, 1194, 1218` | "Should we run per-frame work (animation, AI, lifecycle stamping)?" — tick-budget question |
| Lifecycle / destroy | `mech.cpp:6047, 6048` (`withdrawing && !inView` -> `handleDestruction`), `gvehicl.cpp:3484`, `terrobj.cpp:1026` (destroy path) | "Is this actor still in the active world?" — event/decay question |
| AI / combat-fire / weapon-spawn | `code/mech.cpp:6453-6513` (HUD fire-icon via `canBeSeen()`), `code/gvehicl.cpp:3944-3959` (sensor text), `mover.cpp:3471/3514` (`oldInView` save/restore for LOS) | "Is this actor visible-enough to fire / present to player?" — game-logic question |
| Mouse picking | `code/objmgr.cpp:2645, 2714, 2757` (`canBeSeen()`) | "Should this actor accept a mouse-pick ray?" — UI frustum question |
| Save-game serialization | `code/artlry.cpp:1746, 1764` (`data->inView = inView; ... inView = data->inView;`) | **Persistence schema** — saves round-trip `inView` |

### 1.3 Why the conflation is a load-bearing bug

The Stage 0.5 §4 NO-GO proved you cannot move ONE reader to a tighter
predicate without making the producer a non-superset for the OTHERS.
Every §4-shaped patch is contradictory by construction: if the
producer remains a per-actor frustum test, then "block-sticky-admitted"
is a strict-superset of it for render but a strict-superset is the
wrong contract for picking (UI frustum) or for destroy (event timing).
The bit cannot simultaneously be:

- Tight enough for frustum-cull (small admit set → CPU savings on
  enqueue)
- Loose enough for save/restore (must round-trip identically)
- Latency-free enough for destroy (no readback lag)
- AI-aware enough for combat (player-presentable state)
- UI-correct for picking (matches mouse-ray frustum, which differs
  from scene frustum in editor mode)

These are five different contracts. One bit cannot honor them.

### 1.4 What the campaign has been doing instead

For ~2 weeks, every Stage 0.5 slice has tried to make one bit honest:
K-window admit, dilation, conservative-OR, motion-tolerance,
sticky-bit, readback repoint. Each slice ships, each fixes some
narrow envelope, the conflation survives. The 1.17ms baseline (per
`docs/superpowers/explorations/2026-04-30-cross-track-perf-budget-audit.md`
cited by §6.6) has not moved because none of the slices retire any
consumer's read; they tune the producer.

---

## 2. The dormant taxonomy we are activating (corrected per 2026-05-20 adversarial review)

`GameOS/gameos/gpu_cull_record.h:61-66` (grep-verified at write-time)
already declares:

```cpp
enum GpuConsumerFlags : uint32_t {
    Consumer_None             = 0u,
    Consumer_AIGate           = 1u << 0,
    Consumer_WeaponSpawnNode  = 1u << 1,
    Consumer_LifecycleGate    = 1u << 2,
    Consumer_RenderGate       = 1u << 3,
};
```

And `code/objmgr.cpp:1994-2166` (grep-verified) already TAGS every
substrate emission with its consumer mask:

- Special-buildings (`:1994`): `Consumer_RenderGate | Consumer_LifecycleGate`
- Gates (`:2020`): `Consumer_RenderGate | Consumer_LifecycleGate`
- Other terrain objects (`:2066`): `Consumer_RenderGate | Consumer_LifecycleGate`
- Mech movers (`:2115-2116`): `Consumer_AIGate | Consumer_WeaponSpawnNode | Consumer_LifecycleGate | Consumer_RenderGate`
- GV movers (`:2138-2139`): same as mech
- Turrets (`:2166`): `Consumer_RenderGate | Consumer_LifecycleGate`

**The substrate already CARRIES consumer-mask metadata per-actor. No
GLSL kernel or C++ consumer reads it for gating today** — adversarial
review (2026-05-20) grep-verified that `consumerFlags` is a
write-only field on `GpuActorRecord`. The GLSL declarations in
`shaders/gpu_cull.comp` and `shaders/gpu_cull_block_rollup.comp` exist
solely to keep std430 stride parity with the C++ struct; no shader
reads or branches on it.

What this means for the campaign: the substrate-emission code has
already CLASSIFIED every actor by which Consumer gates it participates
in (this work landed in Track-C3a / `a1d3190`). That classification
has been dormant — payload riding on the substrate ring but never
consumed. This spec ACTIVATES the dormant taxonomy: we promote
`consumerFlags` from inert metadata to live dispatch by adding the
matching Appearance-side bools and wiring each consumer site to the
correct bool.

**This is architectural work, not mechanical.** Adversarial review
flagged that the original framing ("we are not introducing a three-gate
model from first principles") understated the scope. The
classification taxonomy exists; the producer/consumer wiring does not.
Stage 1 IS the architectural retirement of the conflation; we just
get to skip the "decide which gates are needed" design step because
the substrate emission code already answered it.

---

## 3. Convergence from 6 advisors

| Advisor | Key recommendation | Backing |
|---|---|---|
| mc2-cpu-gpu-offload-expert | Decompose `inView` at PRODUCER side into 4 named bools. Render is the only true offload candidate; other 4 stay CPU. Ship v4 (gate render on `blockVisBits[]`) substitutively first. | "Track-C is the template: per-frame GPU artifact consumed at submission, CPU paths survive for orthogonal concerns" |
| mc2-render-expert | Block-level sticky-superset for visibility + per-actor `lastSimFrame_` stamp for freshness. Migration: statics-under-GPU → shadows → dynamics LAST. | "Generalize the `cachedFrame_` infrastructure already shipped for static-prop registry to every render consumer" |
| mc2-mech-update-geometry-expert | Mechs already partial-three-gate via `MC2_GPU_CULL_LIFECYCLE`. Mover overrides need lockstep edits (don't call base). `oldInView` save/restore becomes dead code. NO combat-awareness 4th gate needed. | `mech.cpp:6453-6513` already routes combat-fire HUD to readback signal under env gate |
| mc2-editor-bringup-expert | Editor renders THROUGH `recalcBounds()` return; forked-loop META-FIX is HARD PREREQUISITE not follow-on. Either editor-first-class in design, or keep `Appearance::inView` as named LEAVE-site. | Editor uses `eye->usePerspective=false` ortho; any new gate keyed on perspective frustum is empty in ortho = universal false-negative |
| Greybeard | **META-FIX verdict.** "Constraint no longer holds." Promote `Consumer_*` enum from instrumentation to dispatch. | Steel-man of "preserve" defeated by grep: `Consumer_*` enum exists, `framesSinceActive` composite already documented at `gameobj.h:344-349` — "informally maintaining a two-gate model for months without admitting it" |
| Adversarial steelman | Both sides held tight. **Deciding question:** instrumented disagreement-rate measurement of the 4 reads of `inView` on the same actor-frame. <5% disagreement → preserve + BIH. >20% (especially at wolfman zoom where `bdactor.cpp:1300` documents ~87% false-negative) → retire. | Single tier1 instrumented run forces concession |

---

## 4. The four-gate model

The campaign INFORMS its gate decomposition from `GpuConsumerFlags`
(`gpu_cull_record.h:63-66`) but does NOT claim a strict 1:1 mapping.
The substrate's Consumer_* taxonomy was scoped to substrate-emission
classification (which downstream consumers a given actor record
participates in), not to a complete enumeration of every gate concern
on the Appearance class. Specifically:

- `Consumer_RenderGate` ↔ `Appearance::renderVisible` (1:1)
- `Consumer_LifecycleGate` ↔ `Appearance::lifecycleAlive` (1:1 in
  spirit; semantics differ — see §4.1)
- `Consumer_AIGate` and `Consumer_WeaponSpawnNode` both fold into
  `Appearance::aiPresentable` (N:1 — both express "presentable to AI
  / game logic"). Weapon-spawn-node visibility is already gated by
  the same readback the rest of AI-presentability uses (per
  mech-runtime advisor), and no callsite distinguishes them.
- `Appearance::simActive` has **no Consumer_* equivalent**. The
  substrate emission code lives downstream of the simulation gate —
  only actors whose `update()` already ran can be emitted. Sim
  gating is an Appearance-level concern that the substrate taxonomy
  doesn't model.

**This is partial-taxonomy promotion, not 1:1 lift.** Some gates
inherit from the substrate enum; one is novel; one folds two enum
bits. Adversarial review (v2→v3) flagged the initial "promote 1:1"
framing as overfit; corrected here.

### 4.1 Gate definitions

| Gate (member) | Set by | Read by | Semantics |
|---|---|---|---|
| `Appearance::renderVisible` | Producer-side via `setVisibilityGatesFromLegacy()` (see §4.4): union of (coarse-frustum AND on-screen). Initially equals `inView`; widens in Stage 6 to include sticky-bit-admit | All `XXX::render()` enqueue points + shadow gates + GpuStaticPropBatcher submit | "Should this be drawn this frame?" — frustum-correctness question |
| `Appearance::simActive` | Producer-side: hysteresis-with-floor over `inView` history (~N=4 frame floor like `framesSinceActive`); statics: TRUE when `inView` OR `objBlockInfo.active`; movers: always TRUE for alive actors regardless of view | `TerrainObject::update` lifecycle-block gate (`terrobj.cpp:796`), per-class `update()` body gates, animation/skinning entry | "Should we run per-frame work?" — tick-budget question with hysteresis. **Must always be a superset of `renderVisible` at any given frame** to preserve the gate-pair invariant that prevents black-tree resurrection. |
| `Appearance::lifecycleAlive` | Set TRUE in `Appearance::init()`. Cleared only when the actor is removed from the active world (mission-end teardown, AI-driven removal). NOT camera-driven. NOT consumed by destroy-trigger code. | Mouse picking (`objmgr.cpp:2645, 2714, 2757` via `canBeSeen()` migration), destroy GUARD checks, save-load lifecycle audits | "Is this actor in the active world?" — pure event-driven membership check |
| `Appearance::aiPresentable` | Producer-side: matches both `Consumer_AIGate` + `Consumer_WeaponSpawnNode` semantics — game-presentable visibility; movers use readback-lagged (already wired via `MC2_GPU_CULL_LIFECYCLE`) | HUD fire-icon (`mech.cpp:6453-6513` via `canBeSeen()` indirection), sensor text (`gvehicl.cpp:3944-3959`), AI behavior gates, weapon-spawn-node visibility | "Is this actor presentable to the player / AI this frame?" — 1-frame HUD lag acceptable |

### 4.2 The destroy-trigger is a SEPARATE concern from `lifecycleAlive`

(v3 correction per user blocking finding 4.) The current
`mech.cpp:6047` predicate `withdrawing && !inView` is **camera-driven
destruction**: when a withdrawing mech leaves the screen, it's cheap
to clean up. The semantic is "withdrawing AND camera-lost = good time
to destroy."

Under unconflation, this stays camera-driven and migrates to
`withdrawing && !renderVisible` (Stage 2 work — render-gate
migration). It does NOT consume `lifecycleAlive`. `lifecycleAlive` is
the GUARD that protects code paths from operating on a destroyed
actor — it's the answer to "is this still here?", not "should I
destroy this?".

Using `!lifecycleAlive` as the destroy-trigger predicate would be
circular: `lifecycleAlive` is only cleared by destruction, so
`!lifecycleAlive` means "already destroyed" — that's a no-op
condition, not a trigger.

The v2 spec made this mistake at §5 Stage 3. v3 fixes it: destroy
trigger stays camera-driven (`renderVisible`-bound); `lifecycleAlive`
serves the GUARD use case (picking, save, AI's "can I target this?").

### 4.3 The gate-pair invariant (load-bearing)

**Invariant:** for every frame, every actor:
`simActive ⊇ renderVisible`.

If render fires for an actor, its update MUST have run. Violating
this is the black-tree class (§1.1 bug 2). The §2 v4-style widening
that the v2 spec proposed under Stage 2b (sticky-bit widening
`renderVisible` without widening `simActive`) violated this
invariant; v3 corrects the rollout ordering — widening cannot land
until `simActive` migration ships and the producer ensures
`simActive` widens in lockstep.

Producer-side enforcement: `setVisibilityGatesFromLegacy()` writes
both fields from the same source. Any widening of `renderVisible`
must widen `simActive` first or simultaneously.

### 4.4 The `setVisibilityGatesFromLegacy(bool)` helper

(v3 addition per user blocking finding "Recommended edit 1".)
Manual lockstep assignment across 6 subclass overrides is the
inherited-citations anti-pattern waiting to happen — one missed
override silently breaks the invariant.

The helper is a base-class non-virtual method:

```cpp
// In Appearance, line ~180:
void setVisibilityGatesFromLegacy(bool v) {
    inView         = v;
    renderVisible  = v;
    simActive      = v;
    aiPresentable  = v;
    // lifecycleAlive NOT touched — event-driven, not camera-driven
}
```

Every `recalcBounds()` override calls this once with the computed
visibility result. Existing direct writes to `inView` become a single
call. This:
- Removes the lockstep burden from the 6 overrides.
- Centralizes the byte-identical Stage 1 contract.
- Provides a single audit point if Stage 6 widening changes the
  invariant.
- **Notably excludes `lifecycleAlive`** — preserving its
  event-driven semantics. `recalcBounds()` does NOT clear it; only
  destroy-events do.

`setInView()` legacy setter is DEPRECATED but not removed in Stage 1.
Its 8 force-true callsites are audited per stage; the recommended
disposition by v3 is to have `setInView()` ALSO call
`setVisibilityGatesFromLegacy(v)` to keep the force-true callsites
coherent across all gates while Stage 1 is in flight. But by Stage 4
(after `lifecycleAlive` migration), `setInView(false)` would
incorrectly clear lifecycleAlive — at that stage we either (a) audit
each `setInView()` caller and migrate to per-gate setters, or (b)
make `setInView()` exclude `lifecycleAlive` from its write set.

(v3 correction per user blocking finding "Major cleanup: setInView
writing all five fields is acceptable for Stage 1 byte-identity, but
becomes dangerous before Stage 3+".)

### 4.5 Save-game schema invariant

`code/artlry.cpp:1746` writes `data->inView = inView;` and `:1764`
reads `inView = data->inView;`. Save-game schema currently round-trips
`inView`. The spec MUST preserve this round-trip OR migrate the schema
in a versioned way.

**The legacy `inView` field continues to receive the EXACT
recalcBounds() visibility result** — not a derived expression like
`renderVisible || simActive`. (v3 correction: v2 said the latter,
which would silently change save semantics once `simActive` widens
beyond frustum visibility.) This means `inView` writes are
unchanged from pre-Stage-1 code; only the new gate fields are
introduced. Save migration is OUT OF SCOPE for this spec; the legacy
field stays as a frozen serialization sentinel.

### 4.6 Legacy `inView` field write preservation

**Sole grounding (corrected per adversarial review 2026-05-20):**
save-game schema. `code/artlry.cpp:1746` writes `data->inView =
inView;` and `:1764` reads `inView = data->inView;`. Any spec that
stops writing the legacy `inView` field breaks save round-trip.

**This spec keeps the legacy `Appearance::inView` field WRITTEN by
`recalcBounds()` exactly as today**, even after the 4-gate decomposition.
The field becomes a serialization-sentinel: runtime consumers migrate
to the new bools; the legacy field stays written only so saves
round-trip.

The earlier draft of this spec also cited an editor LEAVE-site for
preservation. Adversarial review grep-confirmed that
`engine-standalone/editor/` does NOT exist in this worktree
(`claude/nifty-mendeleev`); the editor advisor was cross-worktree
material. Editor migration is a separate worktree's slice and OUT
OF SCOPE here. The save-schema grounding ALONE is sufficient to
keep `inView` writes intact.

---

## 5. Phased rollout

Each stage has an explicit gate. No stage starts before its
predecessor's gate passes.

### Stage 0 — Candidate-predicate disagreement measurement (PRECONDITION)

(v3 correction per user blocking finding 5.) The v2 plan to "instrument
`canBeSeen()` and `setInView()`" was wrong on two counts: (a) many
consumers read `inView` directly without going through `canBeSeen()`,
and (b) recording reads of the SAME bit doesn't produce disagreement
— every consumer sees the same value because there IS only one value.

The corrected Stage 0 measures **candidate-predicate disagreement**:
for each actor each frame, compute four CANDIDATE predicates that
WOULD have been emitted by the unconflated design, then count
disagreement.

Implementation: a probe in `GameObjectManager::update` (after
`framesSinceActive` sweep at `objmgr.cpp:1918-1941`) that for each
live actor computes:

```cpp
struct InViewCandidates {
    bool render_cand;     // coarse-frustum strict (current inView)
    bool sim_cand;        // coarse-frustum OR objBlockInfo.active OR (mover ? alive : framesSinceActive < N)
    bool lifecycle_cand;  // actor-alive (always TRUE for live actors)
    bool ai_cand;         // for movers: readback-lagged; for statics: coarse-frustum
};
```

Per-frame disagreement counter increments when any pair of these
candidates differs for the same actor. Per-N-frame summary logs:
- Total actor-frames sampled
- Actor-frames with ANY pairwise disagreement
- Per-pair disagreement counts: render×sim, render×lifecycle,
  render×ai, sim×lifecycle, sim×ai, lifecycle×ai
- Worst-case actor (for triage)

Env-gated `MC2_INVIEW_CONFLATION_TRACE=1` per
`debug_instrumentation_rule.md`. 120-frame summary roll like
`[TOBJPARITY v1]`.

Output gate:
- **< 5% actor-frames with ANY disagreement:** abort this spec. The
  conflation is benign in practice; pivot to BIH/octree
  (acceleration-structure) work as the CPU-perf path.
- **5-20% actor-frames with disagreement:** soft-ship. Stages 1-2
  proceed; reassess before Stage 3 sim migration.
- **> 20% actor-frames with disagreement:** confirm META-FIX. Full
  rollout proceeds with confidence. (Predicted regime — `bdactor.cpp:1300`
  comment documents ~87% false-negative on the render-candidate at
  wolfman zoom.)

User-driven mc2_10 worst-case (matches Stage 0.5 §3 precondition
methodology). Camera regime: pan + spin + corner + zoom range,
including wolfman-zoom worst case. Duration: 120s minimum.

`sizeof(Appearance)` audit also runs during Stage 0: grep for
`sizeof.*Appearance`, `memcpy.*appearance`, `placement new` patterns
that could be silently broken by Stage 1's class-layout change.
(M3 fold-in.)

Cost estimate: ~80 LOC probe (candidate computation +
disagreement matrix + summary roll) + 1 user-driven session.

### Stage 1 — Producer-side decomposition (BYTE-IDENTICAL)

Add 4 new `Appearance` members (`renderVisible`, `simActive`,
`lifecycleAlive`, `aiPresentable`). Add the
`setVisibilityGatesFromLegacy(bool)` helper (per §4.4). Each
`recalcBounds()` override replaces its `inView = <expr>` line with
`setVisibilityGatesFromLegacy(<expr>)`. `lifecycleAlive` is set TRUE
in `Appearance::init()` and remains TRUE for the actor's lifetime
(NOT touched by `recalcBounds()`). ZERO behavior change for consumer
reads (they still read `inView`).

**6 subclass virtual overrides** must call the helper instead of
manually assigning `inView`: `BldgAppearance`, `TreeAppearance`,
`VFXAppearance`, `GenericAppearance`, `GVAppearance`,
`Mech3DAppearance`. Plus base `Appearance::recalcBounds()`. The
helper-based approach replaces the v2 "lockstep manual assignment"
discipline that adversarial review correctly flagged as fragile.

`Artillery::recalcBounds(CameraPtr)` is on a separate signature; if
it shadows the polymorphism in practice, audit it separately. Stage 1
adversarial review checklist: confirm `Artillery` instances either
inherit base behavior or call the helper themselves.

**Constructor + init zero-init contract.** Both `Appearance()` ctor
(`appear.h:85-95`) and `Appearance::init()` (`appear.h:97-107`):

```cpp
inView          = FALSE;
renderVisible   = FALSE;
simActive       = FALSE;
lifecycleAlive  = TRUE;   // <-- TRUE because actor is alive at construction
aiPresentable   = FALSE;
```

`lifecycleAlive` defaults TRUE because construction means the actor
exists. Other gates default FALSE because the first `recalcBounds()`
call will overwrite them. Save-game `ArtilleryData` does NOT
serialize the new bools — Stage 1 explicitly leaves them
recomputed-on-next-recalcBounds; the byte-identical claim depends on
ctor init being correct.

**`setInView()` semantics (v3 correction per user blocking finding).**
The existing setter (`appear.h:181-184`) is called by 8 sites
(`gamecam.cpp:686, 701` compass/sky, `gate.cpp:351`,
`turret.cpp:579, 810`, `missiongui.cpp:4485, 5936` HUD-VTOL,
`mover.cpp:3471, 3514` LOS save/restore). In Stage 1 it is updated to
route through `setVisibilityGatesFromLegacy(v)` BUT explicitly
EXCLUDES `lifecycleAlive` from its write set (lifecycleAlive is
event-driven; a force-visible call from compass/sky/VTOL/LOS code
must NOT clear it). This preserves byte-identical behavior at the 8
force-true callsites while keeping the invariant that
`lifecycleAlive` is only cleared by destroy events.

**Stage 3+ prerequisite:** Stage 3 (simulation migration) introduces
the first divergence between `inView` and `simActive` (hysteresis).
At that point, `setInView(false)` callers — if any exist — could
inappropriately clear `simActive`. Stage 3 must audit the
`setInView(false)` callsites:
- `gate.cpp:351` (only path that calls `setInView(inView)` with the
  recalcBounds-computed value — equivalent to `recalcBounds()`'s own
  write; safe through helper)
- `mover.cpp:3514` (`setInView(oldInView)` — LOS restore; oldInView
  is captured at `:3471` before the force-true, so restores to the
  pre-call value; safe)
- All other 6 callsites force TRUE only — never FALSE — so no
  Stage 3 risk.

Audit confirmed at v3 write-time: NO Stage 3+ risk from
`setInView(false)`. The 8 sites are either force-TRUE or
restore-pre-captured-value patterns.

**`sizeof(Appearance)` audit.** Adding 4 bools + 4-byte alignment
slack changes the class footprint by 4-8 bytes. Stage 0
instrumentation already includes a grep pass for `sizeof.*Appearance`,
`memcpy.*appearance`, `placement new` patterns.

**No consumer migrations in this stage.** Producer-side only.
Consumers still read `inView` and `canBeSeen()` (which returns
`inView`); they see byte-identical values.

Gate: tier1 5/5 30s PASS, no regression in any Tracy zone, no
behavior diff from pre-Stage-1. Class layout change → `--clean-first`
mandatory.

### Stage 2 — Render-gate migration (substitutive, no widening)

Migrate ONLY `renderVisible` consumers to the new member. Each
callsite: `if (inView || g_useGpuStaticProps)` → `if (renderVisible
|| g_useGpuStaticProps)`. Stage 1 set `renderVisible = inView` via
the helper; Stage 2's gate change is byte-identical.

**This is the Stage 0.5 §4 work, properly scoped.** The black-tree
class is dissolved because `renderVisible == inView` exactly — no
false-positives possible. The dropout class is dissolved because
`renderVisible` is producer-controlled (matches `recalcBounds()`
output), not readback-driven.

Stage 2 does NOT widen `renderVisible`. The sticky-bit / v4-style
admission widening (formerly Stage 2b) is deferred to Stage 6, AFTER
simulation gate migrates. This avoids the v2 black-tree
resurrection risk that user blocking finding 2 flagged.

Gate: tier1 5/5 + user-driven mc2_10 visual canary (60s worst-case,
Stage 0.5 §6.5 methodology). NO observable pop; NO black textures;
NO LOD-swap regression at ~5000 units.

### Stage 3 — Simulation-gate migration (NEW — fixes user blocking finding 1)

Migrate `simActive` consumers. Sites to migrate:
- `code/terrobj.cpp:796` `if (inView)` → `if (simActive)` (lifecycle/
  static-update-bypass inner block gate)
- Per-class `update()` body internal gates that check `inView` or
  `canBeSeen()` for "should we do per-frame work?": grep all
  `Appearance` subclasses for `update()` consumers of `inView`.
- Animation/skinning entry points that gate on visibility.

**Hysteresis activation.** Stage 3 also activates the hysteresis
discipline in the producer: `simActive` becomes a SUPERSET of
`inView` (statics: `inView OR objBlockInfo.active OR framesSinceActive
< N` floor; movers: always TRUE for alive actors per mech-runtime
advisor "no inView gate on update for movers"). The helper
`setVisibilityGatesFromLegacy(v)` is updated:

```cpp
void setVisibilityGatesFromLegacy(bool v) {
    inView         = v;
    renderVisible  = v;
    simActive      = v || (framesSinceActive < SIM_HYSTERESIS_N);
    aiPresentable  = v;  // movers' override widens to readback-lagged in Stage 5
}
```

**Critical invariant check (§4.3).** After Stage 3, `simActive ⊇
renderVisible` by construction (hysteresis-OR is monotonic widening).
Stage 6 (render widening) becomes safe to land BECAUSE `simActive` is
already a superset — any future widening of `renderVisible` will be
covered by the still-wider `simActive` so update + render agree.

**setInView() audit.** With `simActive ≠ inView` after Stage 3, the
8 `setInView()` callsites get a final audit. Per Stage 1 audit
section, none are `setInView(false)` in adversarial position; safe.

Gate: tier1 5/5 + user-driven mc2_10 visual canary + tier1 perf
delta check (simActive widens admit set, so per-frame update count
may rise; verify GameLogic.Units.TerrainObjects doesn't regress
beyond noise threshold). Adversarial review mandatory.

### Stage 4 — Lifecycle-gate migration (NOT destroy-trigger)

Migrate `lifecycleAlive` CONSUMERS. The semantics: `lifecycleAlive`
stays TRUE while the actor exists in the active world; cleared ONLY
by destroy/mission-end events.

Sites to migrate:
- Mouse picking (`code/objmgr.cpp:2645, 2714, 2757` —
  `canBeSeen()` reads). Migration: replace `appearance->canBeSeen()`
  with `appearance->lifecycleAlive` at these sites if and only if
  the original intent was "is this still a pickable target." Audit
  each site individually — picking may actually want visibility (UI
  frustum), in which case it migrates to `renderVisible` instead.
- Save-load lifecycle audits — if any code path reads `inView` to
  decide "is this actor still in the world," migrate to
  `lifecycleAlive`.

**v3 correction per user blocking finding 4:** the destroy-trigger
at `mech.cpp:6047` (`withdrawing && !inView`) does NOT migrate to
`!lifecycleAlive` (which would be circular). It migrates to
`!renderVisible` in Stage 2 — already done. `lifecycleAlive` is a
GUARD, not a TRIGGER.

Stage 4 also clears `lifecycleAlive` at the event sites:
- `TerrainObject::destroy()` (`terrobj.cpp:1026`)
- `Mech::handleDestruction()` (called from `mech.cpp:6048`)
- `GVehicle::handleDestruction()` (called from `gvehicl.cpp:3484`'s
  destroy path)
- Mission teardown (`Mission::destroy` for any per-mission cleanup
  that wasn't already destroy-event-driven)

Gate: tier1 5/5 + 5-minute mission completion canary (focus on
picking behavior + withdrawing-mech destroy timing). Adversarial
review mandatory.

### Stage 5 — AI/combat-presentability migration

Migrate `aiPresentable` consumers. HUD fire-icon, sensor text,
weapon-spawn-node visibility — these already route through
`MC2_GPU_CULL_LIFECYCLE` readback when the env is set. Stage 5
formalizes this:
- `Appearance::aiPresentable` producer: for movers, set from
  readback-lagged signal when `MC2_GPU_CULL_LIFECYCLE` is on; else
  from coarse `inView`. For statics, mirror `renderVisible` (no
  separate AI semantics).
- Sites: `mech.cpp:6453-6513` (HUD fire-icon via `canBeSeen()`
  indirection — migrate to `aiPresentable`), `gvehicl.cpp:3944-3959`
  (sensor text), AI behavior gates.

Mover-override producer: Stage 5 is when `Mech3DAppearance::recalcBounds`
and `GVAppearance::recalcBounds` overrides extend their helper call
to set `aiPresentable` to the readback-lagged signal when enabled.

Gate: tier1 5/5 + AI behavior canary (verify HUD-fire decisions
unchanged).

### Stage 6 — Render widening (formerly Stage 2b)

NOW safe to ship after Stage 3 simulation migration. `renderVisible`
producer widens: `renderVisible = inView || blockSticky(blockIdx)`.

The gate-pair invariant (§4.3) is preserved because Stage 3
already widened `simActive` with hysteresis; `simActive` remains a
superset of the widened `renderVisible` for any actor whose block
is sticky-admitted (since sticky-admit only fires for actors that
were `inView` at some point, and the hysteresis floor was already
keeping their `simActive` true).

(v3 correction per user blocking finding 2: in v2 this was Stage 2b
positioned BEFORE simulation migration, which would have resurrected
the black-tree class by widening render-admit while update was still
gated on the narrower `inView`. v3 reorders this stage AFTER Stage
3 so the invariant `simActive ⊇ renderVisible` holds by construction
across the widening.)

Gate: tier1 5/5 + user-driven mc2_10 visual canary specifically
targeting the v2 black-tree symptoms (camera-motion popping +
black textures). Cleared canary = sticky-bit widening shipped.

### Stage 7 — Deprecate `Appearance::inView`

(v3 correction per user blocking finding "Stage 5 vs deferred LEAVE-sites".)
After Stages 2-6 ship, the live runtime readers of `inView`
are reduced to a small named LEAVE-site set (not zero):

**Permitted-remaining direct `inView` readers** (named LEAVE-sites):
- `code/artlry.cpp:1746, 1764` — save-game schema round-trip
- `code/gamecam.cpp:686, 701` — compass/sky force-visible (separate
  contract; doesn't participate in gate model)
- `code/mover.cpp:3471, 3514` — LOS save/restore (becomes dead after
  consumers migrate; not deleted by this spec)
- Cinematic intro/outro unarmed-camera paths (per
  `step8b_gated_retirement_is_not_wholesale_delete.md`)

The deprecation:
- Rename `inView` → `legacyInView_DEPRECATED` with a `#define inView
  legacyInView_DEPRECATED` alias scoped to the LEAVE-site
  translation units so they continue to compile without textual
  edits.
- Compile-time error on any unintended read elsewhere (controlled by
  removing the broad header alias and only defining it in the LEAVE-
  site .cpp files).
- Save-game schema continues to read/write the renamed field.

Gate: codebase grep returns ONLY the named LEAVE-site reads.

---

## 6. Negative space — explicit list of what we are NOT doing

Per `cull_gates_are_load_bearing.md`: the danger is in unstated
assumptions about what's safe to leave alone. Explicit enumeration:

| NOT touching | Why |
|---|---|
| The `Appearance::inView` legacy field (write side) | Save-game schema round-trip at `artlry.cpp:1746, 1764` + editor consumer. Stays written exactly as today. Read-side migrates; write-side does not. |
| `recalcBounds()` math itself | The 1.17ms baseline could potentially be moved with BIH/octree (Position B) — but that's an independent perf slice (option 2 in the menu). This spec is gate decomposition, not math optimization. |
| Update tick-rate decoupling | The `cachedFrame_` infra exists (option 3 in the menu). Separate slice. Not gated by this spec. |
| GPU-compute coarse cull (option 4) | Multi-quarter scope. This spec sets up the architecture that makes option 4 feasible later, but doesn't ship it. |
| Editor migration entirely | `engine-standalone/editor/` is NOT in this worktree (adversarial review C2). Any cross-worktree editor concern owned by a different slice. Mentioned here only so future planners don't re-add it to the in-scope list. |
| `framesSinceActive` composite | `gameobj.h:349`, `objmgr.cpp:1918-1941` — already does a 3-condition composite sweep. Stage 1's producer-side decomposition aligns with it; later stages may retire the sweep, but not this spec. |
| `windowsVisible` (FOW related) | Separate concern from frustum visibility. `code/gameobj.cpp:1603, 2094` etc. set this to gameplay turn-counter for FOW reveal. Untouched. |
| `objBlockInfo[i].active` (Terrain block lifecycle) | Already a separate signal. Used in `objmgr.cpp:1929` composite. Stays as-is; `simActive` may consume it. |
| Mouse picking (`objmgr.cpp:2645, 2714, 2757`) | Could be Stage 4 work, but UI frustum is a different question than scene frustum. Spec defers picking migration to a follow-on. Reads `canBeSeen()` until then. |
| Cinematic intro/outro pan unarmed-camera | Per `step8b_gated_retirement_is_not_wholesale_delete.md`. Has its own contract. Reads coarse-`inView` semantics via the legacy field. Untouched. |
| `mover::oldInView` cache (`mover.cpp:3470, 3514`) | Per mech-runtime advisor: this is a save/force/restore bypass; becomes dead when `inView` consumers all migrate. Cleanup is a Stage 5+ task. |
| `gamecam.cpp:686, 701` `setInView(true)` compass/sky | Force-set bypasses cull entirely. These objects don't participate in the gate model. Untouched. |
| `missiongui.cpp:4485, 5936` `setInView(true)` HUD-VTOL | Same pattern. Untouched. |
| `MC2_GPU_CULL_LIFECYCLE` plumbing | Already a partial three-gate. Stage 4 absorbs it; Stages 1-3 leave it alone. |

---

## 7. Risk surface

**R1 — Save-game schema breakage.** `artlry.cpp:1746, 1764` round-trip
`inView`. Stage 1's "byte-identical" requirement covers this if
`inView` keeps being written. Adversarial-review every recalcBounds
override to confirm `inView` write is preserved.

**R2 — Editor cross-worktree concern.** This worktree
(`claude/nifty-mendeleev`) does not contain `engine-standalone/editor/`
per adversarial review C2 grep. The editor lives in a separate
worktree; this spec's `inView`-write preservation incidentally
benefits any editor that shares Appearance class layout, but no
editor consumer is in scope here. A separate editor-worktree slice
will own editor migration if/when needed.

**R3 — Subclass override silence.** Mover `recalcBounds` overrides
don't call base. A spec change that only edits base `Appearance::recalcBounds`
is silently a no-op for movers. Every Stage 1 commit must edit **all
6 polymorphism override sites** in lockstep (Bldg, Tree, VFX,
Generic, GV, Mech3D). `Artillery::recalcBounds(CameraPtr)` is a
separate-signature method, NOT on the polymorphism chain — audit
independently. Adversarial review checklist (M1 corrected).

**R4 — Save-game determinism.** RTS replay determinism may depend on
`inView` value being identical across re-runs. Stage 1's byte-identical
mode covers this. Stages 2+ must preserve determinism via fixed-seed
camera replay tests (out of scope here; flag for adversarial review).

**R5 — Black-tree resurrection.** Stage 2's substitutive nature
prevents this. Each gate read swaps `inView` → `<consumer>Visible`
where `<consumer>Visible` is set by the same producer in the same
frame. Update + render agree by construction at Stage 2; only
diverge at Stage 3 (lifecycle hysteresis) where it's intentional and
the cause is decoupled from camera motion.

**R6 — Track-C `framesSinceActive` interaction.** The composite at
`objmgr.cpp:1929-1934` reads `s_gpuCullLifecycle ? readback : (inView
|| canBeSeen || objBlockInfo.active)`. Stage 3's `simActive` should
ALIGN with this composite; misalignment means two different
notions of "active" in the codebase.

**R7 — Mouse picking gap.** Stages 1-4 leave picking on legacy
`canBeSeen()`. If save-game restore changes editor mouse-pick
behavior, surface as a Stage-4-follow-on.

**R8 — `mech.cpp:6453-6513` IS an `inView` reader via `canBeSeen()`
indirection.** Greybeard advisor flagged that the direct `inView`
token doesn't appear at 6453-6513, but adversarial review (2026-05-20
M2) re-grepped: `appearance->canBeSeen()` IS called at `:6456, :6478,
:6513`, and `canBeSeen()` reads `inView` (`appear.h:178`
`return(inView)`). §4.1's classification of this range as an
`aiPresentable` consumer is correct. The greybeard's "no inView
reference" claim was imprecise. **Lesson:** when auditing `inView`
consumers, ALWAYS grep both `\binView\b` and `\bcanBeSeen\b`
together; the accessor hides the read.

---

## 8. Test / gate strategy summary

| Stage | Smoke gate | Visual canary | Adversarial review | --clean-first? |
|---|---|---|---|---|
| 0 (instrument) | tier1 5/5 30s | mc2_10 120s (collect candidate-disagreement data) | optional | no |
| 1 (decompose + helper) | tier1 5/5 30s | none (byte-identical) | yes (helper completeness, ctor init, setInView audit) | YES (class layout) |
| 2 (render) | tier1 5/5 + mc2_10 60s | yes (Stage 0.5 §6.5 methodology) | yes | no |
| 3 (simulation) | tier1 5/5 + perf-delta check | yes (mc2_10 60s; verify update count doesn't visibly regress) | YES (hysteresis + invariant verification) | no |
| 4 (lifecycle GUARD) | tier1 5/5 + 5-min mission | yes (picking + withdrawing-mech destroy) | YES (semantic shift) | no |
| 5 (AI presentability) | tier1 5/5 | yes (HUD-fire decisions) | yes | no |
| 6 (render widening) | tier1 5/5 + mc2_10 60s | YES (v2-revert symptoms specifically) | YES (invariant proof) | no |
| 7 (deprecate) | tier1 5/5 + grep audit | none | yes | no |

`MC2_GL_DEBUG_FATAL=1` mandatory for all stages per worktree CLAUDE.md
Tier 1.2.

---

## 9. Open questions for next planner

1. **Stage 0 candidate-predicate definitions.** The candidate
   semantics defined in v3 §5 Stage 0 are first-pass; the user-driven
   canary may surface that one or more candidates needs adjusting
   (e.g., should the AI candidate use raw readback or
   conservative-OR? Should the sim candidate's hysteresis floor be
   N=4 or N=8?). Treat candidates as tunable knobs during Stage 0
   data collection; lock them after the canary lands.

2. **Save-game schema deprecation.** Stages 2-7 leave the legacy
   `inView` field written. Stage 7 wants to rename it. Should saves
   migrate to a versioned schema that drops `inView` entirely, or do
   we keep writing it forever as a frozen field? Confirm with user.

3. **`SIM_HYSTERESIS_N` floor value.** Stage 3 introduces
   `simActive = inView OR (framesSinceActive < N)`. Adversarial review
   for Stage 3 should re-examine `objmgr.cpp:1918-1941`'s existing
   sweep — the `framesSinceActive` increment cadence is per-frame, so
   N=4 means "kept simActive for 4 frames after coarse inView drops."
   Should we match `framesSinceActive`'s consumer (currently used for
   instrumentation only) or pick a different N? Recommend N=4 as
   conservative; let Stage 3 canary tune.

4. **`Artillery::recalcBounds(CameraPtr)` audit.** Adversarial review
   M1 flagged this as a separate-signature method not on the polymorphism
   chain. Stage 1 needs to confirm whether `Artillery` instances
   actually reach base `Appearance::recalcBounds()` or their own
   shadow. If shadow: add helper call to it. If unreachable: untouched.

5. **Mover override producer divergence.** Stage 5 has mover overrides
   write `aiPresentable` from readback-lagged signal while still
   writing `simActive` from "always TRUE for alive movers." Verify
   this divergence in the helper or use a Mover-specific override of
   `setVisibilityGatesFromLegacy`.

6. **`framesSinceActive` retirement.** Once `simActive` exists as a
   first-class member, `framesSinceActive` may become redundant.
   Retire it in Stage 3 (when simActive consumes its hysteresis) or
   keep both? Recommend keep as instrumentation; small cost.

7. **Editor migration (out-of-scope acknowledgment).** Editor lives
   in a separate worktree (per adversarial review C2). When editor
   work resumes, the editor must either migrate to the new gates or
   stay on the legacy `inView` field (which we keep written
   forever per save schema). Recommend: roadmap-only; do not
   schedule from this spec.

---

## 9.5 v3 amendments — user blocking review (2026-05-20)

User blocking review on v2 returned 5 design defects + 4 cleanup
items. All folded into v3:

| Finding | v3 disposition |
|---|---|
| **1. Rollout never migrates `simActive`** — Stages 2/3/4/deprecate covered render/lifecycle/AI but not sim; yet §11 claimed "no runtime readers after Stages 2-4." | Inserted **new Stage 3 (Simulation-gate migration)**. Renumbered subsequent stages: lifecycle is now Stage 4, AI is Stage 5, render-widening (was 2b) is Stage 6, deprecate is Stage 7. Stage 8 gate strategy table updated. |
| **2. Stage 2b can resurrect black-tree** — widening renderVisible via sticky while update still on inView creates false-positives → stale cached light data. | Moved Stage 2b → Stage 6, gated on Stage 3 (sim migration) having shipped. The gate-pair invariant (§4.3 new section) is enforced producer-side: `simActive ⊇ renderVisible` always. Render widening is safe only after sim widens. |
| **3. `GpuConsumerFlags` doesn't map 1:1** — enum has `WeaponSpawnNode` (no member); `simActive` has no enum bit. | Rewrote §4 lead-in: "informs gate decomposition but does NOT claim 1:1." Explicit per-bit mapping table: RenderGate↔renderVisible (1:1), LifecycleGate↔lifecycleAlive (1:1 in spirit), AIGate+WeaponSpawnNode→aiPresentable (N:1), simActive has no enum equivalent. "This is partial-taxonomy promotion, not 1:1 lift." |
| **4. `lifecycleAlive` circular destroy** — using `!lifecycleAlive` to TRIGGER destruction, where lifecycleAlive is cleared only by destruction, is a no-op condition. | New §4.2 separating destroy-TRIGGER from lifecycle-GUARD. Destroy trigger stays camera-driven (`withdrawing && !renderVisible`) and migrates in Stage 2. `lifecycleAlive` is the GUARD (picking, save) and migrates in Stage 4. NOT consumed by destroy code. |
| **5. Stage 0 metric measures wrong thing** — recording reads of single bit doesn't produce disagreement; many reads bypass canBeSeen(). | Rewrote Stage 0: candidate-predicate disagreement matrix. For each actor-frame, compute 4 CANDIDATE predicates (render/sim/lifecycle/ai) and count pairwise disagreement. Probe lives in `GameObjectManager::update` after `framesSinceActive` sweep — covers every live actor regardless of consumer path. |
| Cleanup: legacy `inView` write inconsistency | §4.5 fixed: legacy `inView` continues to receive EXACT `recalcBounds()` result (not `renderVisible \|\| simActive`). |
| Cleanup: `setInView()` writing all 5 fields dangerous post-Stage 3 | Stage 1 setInView semantics rewritten: routes through helper but EXCLUDES `lifecycleAlive` (event-driven). Audit of all 8 callsites confirms NO `setInView(false)` adversarial cases (all are force-TRUE or capture-and-restore patterns). |
| Cleanup: Stage 5 (now 7) reconciliation with deferred LEAVE-sites | Stage 7 deprecation rewritten: explicit LEAVE-site list (artlry save, gamecam compass/sky, mover LOS, cinematic). `#define inView` alias scoped to LEAVE-site TUs; compile error elsewhere. |
| Cleanup: stale v1/v2 + 17/30 + 5/6 references | §1.2 fixed to "≥30 readers" + "6 polymorphism overrides." Open questions Q4 fixed. Stage 1 enumerates 6 overrides explicitly. Status line bumped to v3. |
| Recommended edit: `setVisibilityGatesFromLegacy(bool)` helper | Added as §4.4 first-class. Every override calls it from `recalcBounds()`. Replaces v2 "lockstep manual assignment" discipline. |

The v3 design is what would have been written if the spec had been
properly grounded from the start. Three of the five blocking findings
(simActive migration, Stage 2b resurrection, lifecycle circularity)
were direct consequences of v2 promoting `Consumer_*` as a complete
taxonomy when it's actually partial — the missing `Consumer_SimActive`
bit hid the missing migration stage, and the absence of a destroy-
trigger / lifecycle-guard split came from over-mapping
`Consumer_LifecycleGate` to a single semantic.

The v2 → v3 lesson for future architectural specs: **a partial
taxonomy is more dangerous than no taxonomy**, because it tempts you
to believe gaps are intentional rather than accidental. The user
review caught what 6 advisors + 1 adversarial-plan-review missed.

## 9.6 Adversarial review summary (2026-05-20, against v1)

Adversarial-plan-review run against v1 of this spec returned 2
CRITICAL + 4 MAJOR + 4 MINOR findings. All folded into v2 (the version
you are reading). Summary of fold-ins:

| Finding | Disposition |
|---|---|
| **C1** — `Consumer_*` is dormant write-only metadata, not "live instrumentation the codebase has been maintaining" | §2 fully rewritten. Framing now: "dormant taxonomy we are activating" / "Stage 1 IS the architectural retirement, not mechanical work" |
| **C2** — Editor LEAVE-site protects a cross-worktree phantom; `engine-standalone/editor/` doesn't exist in `nifty-mendeleev` | §4.3 re-grounded on save-schema alone. §7 R2 demoted. Negative-space row updated. |
| **M1** — Override count: 4 vs 7 vs 5 inconsistencies; actual = 6 true polymorphism overrides + 1 separate-signature `Artillery::recalcBounds(CameraPtr)` | §1.2 and §5 Stage 1 enumerate 6 overrides explicitly. §7 R3 corrected to "all 6 polymorphism overrides." |
| **M2** — §7 R8 inherited greybeard's imprecise claim that `mech.cpp:6453-6513` has no `inView` reference; actually `canBeSeen()` IS called there (which reads `inView`) | §7 R8 rewritten: range IS a reader via `canBeSeen()` indirection. Lesson: grep both `\binView\b` and `\bcanBeSeen\b` together. |
| **M3** — Stage 1 byte-identical claim ignored class-layout cascade: new bools need ctor zero-init, `sizeof(Appearance)` audit recommended | Stage 1 expanded with "Constructor + init zero-init contract" and `sizeof` audit recommendation. |
| **M4** — `setInView()` writes only legacy `inView`, creating intra-frame skew with the 4 new bools after Stage 1 | Stage 1 adds "setInView() semantics decision" with RECOMMENDED policy: `setInView()` writes ALL 5 fields. Alternative (per-field setters) deferred. |
| **m1** — Reader count "17+" was undercount; real ≥ 30 with `canBeSeen()` indirect reads | §1.2 corrected to "≥ 30 reader callsites." |
| **m2** — 1.17ms cite not quoted from source | Noted as future-grep TODO; perf-budget audit doc exists at `docs/superpowers/explorations/2026-04-30-cross-track-perf-budget-audit.md` per grep. |
| **m3** — Span 1993 vs 1994 off-by-one | §2 corrected to `:1994-2166`. |
| **m4** — `bdactor.cpp:1300` 87%-false-negative comment | Confirmed ✓ no change needed. |

The adversarial review's key reframing: **this IS architectural work,
not the "mechanical promotion" the v1 spec claimed.** The substrate
emission code at `code/objmgr.cpp:1994-2166` carries consumer-mask
metadata but no consumer reads it for gating yet. The dormant
taxonomy is real; the producer/consumer wiring is what this campaign
introduces. Cost estimate adjusted accordingly: Stage 1 is a real
architectural slice, not a rename pass.

## 10. References

- v3 Stage 0.5 spec (the predecessor):
  `docs/superpowers/specs/2026-05-19-alpha-stage-0-5-rearm-readback-render-gate-design-v3.md`
  (POSTSCRIPT documents §4 NO-GO + 2nd structural bug class)
- Empirical NO-GO memory:
  `memory/stage_0_5_section_4_blocked_on_readback_non_superset.md`
- Black-tree bug class:
  `memory/black_tree_bug_investigation_state.md`
- Cull gate load-bearing rule:
  `memory/cull_gates_are_load_bearing.md`
- Editor convergence debt:
  `memory/feedback_editor_must_converge_with_runtime_paths.md`
- CPU→GPU offload substitutive rule:
  `memory/feedback_offload_must_be_substitutive_not_additive.md`
- Step-8b gated retirement pattern (for the CPU-producer-stays
  reasoning):
  `memory/step8b_gated_retirement_is_not_wholesale_delete.md`
- Substrate emission with Consumer mask:
  `code/objmgr.cpp:1993-2166` (grep-verified)
- Consumer flag declaration:
  `GameOS/gameos/gpu_cull_record.h:61-66` (grep-verified)
- Save-game inView round-trip:
  `code/artlry.cpp:1746, 1764` (grep-verified)
- Latent multi-gate sweep:
  `code/objmgr.cpp:1918-1941` (`framesSinceActive` composite)
- Sticky-bit ship commit: `91b6991`
- §4 tentative ship + revert: `40a54b7` + `dc2e8f6`
- Doc trail commit: `f40a77a`
- Sibling slices: §0 prereq (`4008185`), savegame restore canary unblock

---

## 11. What this spec is NOT

- Not a CPU perf slice. The 1.17ms baseline does not move
  mechanically from gate decomposition alone. Real CPU win requires
  options 2 (BIH/octree) and/or 3 (tick-rate decouple) and/or 4
  (GPU-compute) — those are SEPARATE specs.
- Not a save-game migration. Schema preserved by keeping `inView`
  written.
- Not an editor migration. Bounded by LEAVE-site.
- Not a single-session implementation. Stages 0-5 are independent
  shippable slices.
- Not a Stage 0.5 replacement. Sticky-bit (§2.5) shipped
  independently; this spec stands or falls on its own.

This spec retires the CONFLATION. Other specs retire the COST.
