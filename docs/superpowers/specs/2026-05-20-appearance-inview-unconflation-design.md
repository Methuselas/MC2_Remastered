# Appearance::inView unconflation — alpha-Stage 1 design

Date: 2026-05-20. Worktree: `claude/nifty-mendeleev` @ `dc2e8f6` (post-§4
revert, post-§2.5 sticky-bit ship). Status: DESIGN v1. Adopts the
"alpha-Stage 1" name the v3 Stage 0.5 spec carves out for the deeper
META-FIX. Cited symbols all grep-verified at write-time.

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

The campaign promotes `GpuConsumerFlags` (`gpu_cull_record.h:63-66`)
from substrate-emission instrumentation to the actual Appearance-side
gate dispatch. Each `Consumer_*` bit gets a dedicated `Appearance`
member, written by `recalcBounds()`, consumed at the matching
callsite.

### 4.1 Gate definitions

| Gate (member) | Set by | Read by | Semantics |
|---|---|---|---|
| `Appearance::renderVisible` | Producer-side: union of (coarse-frustum AND on-screen) — initially mirror of `inView` for stage-1 byte-identical behavior | All `XXX::render()` enqueue points + shadow gates + GpuStaticPropBatcher submit | "Should this be drawn this frame?" — frustum-correctness question |
| `Appearance::simActive` | Producer-side: hysteresis-with-floor over `inView` history (~N=4 frame floor like `framesSinceActive`); statics: TRUE when `inView` OR `objBlockInfo.active`; movers: always TRUE for alive actors regardless of view | `TerrainObject::update` lifecycle-block gate (`terrobj.cpp:796`), per-class `update()` body gates, animation/skinning entry | "Should we run per-frame work?" — tick-budget question with hysteresis |
| `Appearance::lifecycleAlive` | Producer-side: TRUE while actor exists; cleared by event (destroy, mission end). NOT camera-driven. | `mech.cpp:6047` destroy-on-withdrawal, `gvehicl.cpp:3484`, `terrobj.cpp:destroy`, mouse-pick gates | "Is this actor in the active world?" — event-driven, not cull-driven |
| `Appearance::aiPresentable` | Producer-side: matches `Consumer_AIGate` semantics — game-presentable visibility; movers use readback-lagged (already wired via `MC2_GPU_CULL_LIFECYCLE`) | HUD fire-icon (`mech.cpp:6453-6513`), sensor text (`gvehicl.cpp:3944-3959`), AI behavior gates | "Is this actor presentable to the player this frame?" — 1-frame HUD lag acceptable |

### 4.2 Save-game schema invariant

`code/artlry.cpp:1746` writes `data->inView = inView;` and `:1764`
reads `inView = data->inView;`. Save-game schema currently round-trips
`inView`. The spec MUST preserve this round-trip OR migrate the schema
in a versioned way. **Stage 1's byte-identical behavior covers this:**
the legacy `inView` field stays as a deprecated alias (set to
`renderVisible || simActive` or equivalent) until save schema migrates
in a later phase. Save migration is OUT OF SCOPE for this spec; treat
`inView` as a serialization sentinel that must stay write-able.

### 4.3 Legacy `inView` field write preservation

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

### Stage 0 — Disagreement-rate measurement (PRECONDITION)

Per adversarial-steelman deciding question. Instrument every read of
`inView`/`canBeSeen()` with a callsite tag (`render`/`update`/`lifecycle`/`ai`)
and log per-frame disagreement: for each actor-frame, how often do the
4 effective reads demand different answers?

Implementation: a thin probe at `Appearance::canBeSeen()` and
`Appearance::setInView()` that records call-site (via
`__builtin_return_address(0)` or explicit macro tag), accumulates a
2D matrix of (consumer × frame) reads, and emits per-frame
disagreement counts.

Output gate:
- **< 5% actor-frames with disagreement:** abort this spec. The
  conflation is benign; Position B (preserve + BIH/octree) is the
  right move. File as memory; pivot to acceleration-structure work.
- **5-20% actor-frames with disagreement:** soft-ship. Stage 1
  proceeds but be conservative on Stage 3+ scope.
- **> 20% actor-frames with disagreement:** confirm META-FIX. Full
  rollout proceeds with confidence.

User-driven mc2_10 worst-case (matches Stage 0.5 §3 precondition
methodology). Camera regime: pan + spin + corner + zoom range.
Duration: 120s minimum. Probe stays env-gated
(`MC2_INVIEW_CONFLATION_TRACE=1`).

Cost estimate: ~30 LOC instrumentation + 1 user-driven session.

### Stage 1 — Producer-side decomposition (BYTE-IDENTICAL)

Add 4 new `Appearance` members (`renderVisible`, `simActive`,
`lifecycleAlive`, `aiPresentable`). Set them all from
`recalcBounds()` to the SAME value as `inView`. ZERO behavior change.

**6 subclass virtual overrides** must set all 4 new bools in lockstep
(per §1.2 enumeration; per mech-runtime advisor, mover overrides don't
call base): `BldgAppearance`, `TreeAppearance`, `VFXAppearance`,
`GenericAppearance`, `GVAppearance`, `Mech3DAppearance`. Plus base
`Appearance::recalcBounds()`.

`Artillery::recalcBounds(CameraPtr)` is on a separate signature; if
it shadows the polymorphism in practice, audit it separately. Stage 1
adversarial review checklist: confirm `Artillery` instances either
inherit base behavior or have their own gate logic that Stage 1
preserves.

**Constructor + init zero-init contract.** Each of the 4 new bools
gets a zero/false init in BOTH `Appearance()` ctor (`appear.h:85-95`)
AND `Appearance::init()` (`appear.h:97-107`), mirroring the existing
`inView = FALSE` lines exactly. Save-game `ArtilleryData` does NOT
serialize the new bools — Stage 1 explicitly leaves them
recomputed-on-next-recalcBounds; the byte-identical claim depends on
ctor init being correct so the first frame after construction has
defined values until the first `recalcBounds()` call. (Adversarial
review M3 finding.)

**`setInView()` semantics decision (adversarial review M4).** The
existing `setInView()` setter (`appear.h:181-184`) is called by 8
sites (`gamecam.cpp:686, 701` compass/sky, `gate.cpp:351`,
`turret.cpp:579, 810`, `missiongui.cpp:4485, 5936` HUD-VTOL,
`mover.cpp:3471, 3514` LOS save/restore). It writes only `inView`.
After Stage 1, this creates intra-frame skew where
`inView != renderVisible` until the next `recalcBounds()`.

**Decision (RECOMMENDED):** `setInView()` writes ALL 5 fields
(`inView`, `renderVisible`, `simActive`, `lifecycleAlive`,
`aiPresentable`) to the new value. This preserves the existing
"force visible" semantics at every legacy callsite and keeps Stage 1
truly byte-identical. The 8 callsites that force `setInView(true)`
genuinely want the actor to act as if visible across ALL concerns
(compass/sky/VTOL force-true; mover LOS save/restore needs the round-
trip to be coherent for all consumers).

**Alternative (DEFERRED):** introduce `setRenderVisible()`,
`setSimActive()`, etc. separately and audit each `setInView()` caller
to pick which subset to write. Higher precision but expands Stage 1
scope substantially.

**`sizeof(Appearance)` audit.** Adding 4 bools changes the class
footprint. Adversarial review M3 recommends a grep pass for:
`sizeof.*Appearance`, `memcpy.*appearance`, `placement new` patterns
that could be silently broken. Stage 0 instrumentation pass extends
to include this audit.

**No consumer migrations in this stage.** Producer-side only.

Gate: tier1 5/5 30s PASS, no regression in any Tracy zone, no
behavior diff from pre-Stage-1. Class layout change → `--clean-first`
mandatory.

### Stage 2 — Render-gate migration (substitutive single-consumer)

Migrate ONLY `renderVisible` consumers to the new member. Each
callsite: `if (inView || g_useGpuStaticProps)` → `if (renderVisible
|| g_useGpuStaticProps)`. Same byte-identical fail-open rule as v3
spec §4.

**This is the Stage 0.5 §4 work, properly scoped.** The black-tree
class is dissolved because Stage 1 set `renderVisible = inView`
exactly — no false-positives possible. The dropout class is
dissolved because the value of `renderVisible` is producer-controlled
(matches whatever `recalcBounds` currently computes), not
readback-driven.

Optional sub-stage 2b: **v4 reframe** — Stage 1's
`renderVisible` producer expands to include sticky-bit-admit
(`renderVisible = inView || blockSticky(blockIdx)`). This widens the
admit set monotonically per mission (matches `R-NEW-8` analysis from
v3 §5). Ship only if Stage 2 ships clean.

Gate: tier1 5/5 + user-driven mc2_10 visual canary (the same 60s
worst-case as Stage 0.5 §6.5). NO observable pop; NO black textures;
NO LOD-swap regression at ~5000 units.

### Stage 3 — Lifecycle-gate migration (hysteresis)

Migrate `lifecycleAlive` consumers. The hysteresis discipline:
`lifecycleAlive` STAYS true while alive in the world; cleared by
event (destroy, mission end), NOT by camera-driven cull.

This is the load-bearing change: `mech.cpp:6047` currently destroys
on `withdrawing && !inView` — that becomes `withdrawing &&
!lifecycleAlive`. Since `lifecycleAlive` is event-driven, withdrawing
mechs no longer destroy because the camera turned away. Destroy
fires when the AI logic decides withdrawal completes.

Save-game schema: `lifecycleAlive` is NOT serialized (already
implicit in actor-list membership). Legacy `inView` field stays
written for save round-trip; saves restore `inView` from data
verbatim and `recalcBounds()` overwrites it next frame.

Gate: tier1 5/5 + user-driven 5-minute mission completion canary
(focus on withdrawing-mech behavior). Adversarial review mandatory
because this changes destroy timing.

### Stage 4 — AI/combat gate migration

Migrate `aiPresentable` consumers. HUD fire-icon, sensor text,
etc. — these already route through `MC2_GPU_CULL_LIFECYCLE`
readback when the env is set. Stage 4 just renames the read.

Gate: tier1 5/5 + AI behavior canary (verify HUD-fire decisions
unchanged).

### Stage 5 — Deprecate `Appearance::inView`

After Stages 2-4 ship, no live runtime consumer reads `inView`
directly. Save-game schema and editor still need the field. The
deprecation:
- Rename `inView` → `legacyInView_DEPRECATED_save_schema_only`
- All current writers keep writing (save schema invariant)
- Compile-time error on any new direct read
- Editor migration: separate slice; either editor migrates to new
  gates OR keeps reading the renamed field

Gate: codebase grep returns 0 unintended reads.

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
| 0 (instrument) | tier1 5/5 30s | mc2_10 120s (collect data) | optional | no |
| 1 (decompose) | tier1 5/5 30s | none (byte-identical) | yes (override completeness) | YES (class layout) |
| 2 (render) | tier1 5/5 + mc2_10 60s | yes (Stage 0.5 §6.5 methodology) | yes | no |
| 2b (v4) | same as 2 | same | yes | no |
| 3 (lifecycle) | tier1 5/5 + 5-min mission | yes (withdrawing-mech) | YES (destroy timing) | no |
| 4 (AI) | tier1 5/5 | yes (HUD-fire decisions) | yes | no |
| 5 (deprecate) | tier1 5/5 + grep audit | none | yes | no |

`MC2_GL_DEBUG_FATAL=1` mandatory for all stages per worktree CLAUDE.md
Tier 1.2.

---

## 9. Open questions for next planner

1. **Stage 0 instrumentation form.** Macro tag at every consumer
   callsite, or `__builtin_return_address(0)` bucketing? Macro tag is
   explicit; return-address is non-invasive but symbol-stripped in
   RelWithDebInfo. Recommend macro tag.

2. **Save-game schema deprecation.** Stages 2-4 leave the legacy
   `inView` field written. Stage 5 wants to rename it. Should saves
   migrate to a versioned schema that drops `inView` entirely, or do
   we keep writing it forever as a frozen field? Confirm with user.

3. **Stage 2b v4 reframe scope.** Should `renderVisible` producer
   include sticky-bit-admit (`|| blockSticky(blockIdx)`), or stay
   pure mirror of coarse `inView` to keep Stage 2 minimum-diff?
   Recommend: stay pure in 2; v4 lands as 2b after 2 ships clean.

4. **Mover override edit audit.** All 5 subclass `recalcBounds`
   overrides must set all 4 new bools. Should we add a base-class
   non-virtual helper that sets all four, and require overrides to
   call it? Or accept the lockstep edit discipline? Recommend
   helper.

5. **Editor migration deferral.** Should this spec call out a
   follow-on slice for editor convergence (the
   `feedback_editor_must_converge_with_runtime_paths.md` debt), or
   leave it unscheduled? Recommend: add to a roadmap, don't schedule.

6. **`framesSinceActive` retirement.** Once `simActive` exists as a
   first-class member, `framesSinceActive` becomes redundant
   instrumentation. Retire it in Stage 1 or defer? Recommend defer.

---

## 9.5 Adversarial review summary (2026-05-20)

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
