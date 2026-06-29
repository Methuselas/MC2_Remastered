# UNIT-KIND CLASS-CHECK AUDIT (migration blueprint)

> **Status:** SEEDED (Task 0 sweep + jump-gate inventory + taxonomy decision).
> Full two-bucket catalog with replacement-intent columns lands in **Task 8**.
> Anchors below located by grepping the quoted code string in the canonical
> worktree `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/` at
> `HEAD fda48be5`. Line numbers are hints — re-grep before editing.

## Purpose

`UNIT-PROFILE-SEAM-1` introduces a data-owned unit-profile seam
(`ObjectType::UnitProfile` baseline → `Mover` runtime state →
`Mover::canPerform(UnitAction)`). This audit records every
`getObjectClass() == KIND` style class-check so each can be classified as a
*gameplay* decision (candidate for migration onto a profile facet) versus a
*non-gameplay* mechanical check (serialization / spawn / FX / debug / editor),
and assigned an owning facet + future slice.

## Capability taxonomy decision (Task 0, Step 3)

**Slice 1 enumerates `CAP_JUMP` ONLY.** Capabilities are *abilities*. Sensor
concepts (ext-sensor / optics / adv-sensor / active-probe) are a **separate
"sensor" facet** and are deliberately NOT enumerated as capability bits this
slice — adding them now would build a wrong taxonomy (junk-drawer risk) before
the sensor facet exists. Structural truths (locomotion, location schema, UI,
repair, AI role) are likewise separate facets, never capability bits.

Rationale: the `CapabilitySet` bitmask is hidden behind `has()/set()/clear()`
so a future swap to a string-key registry + JSON values is internal churn-free;
but the *enum membership* is the public taxonomy and must not accrete
speculative bits. Each concept's facet is decided by this audit before a bit is
added.

## Jump-permission gate inventory (Task 0, Step 2) — the proof facet

Both gates live in the **same `TACTICAL_ORDER_JUMPTO_POINT /
TACTICAL_ORDER_JUMPTO_OBJECT` case** in `Mover::handleTacticalOrder`
(`code/mover.cpp`):

| anchor | code | role | this-slice action |
|---|---|---|---|
| `mover.cpp:3256` | `bool canJump = (getObjectClass() == BATTLEMECH);` | seeds the per-order jump permission from unit class | **REWIRED in Task 6** (gated): `mc2UnitProfileDataEnabled() ? canPerform(UnitAction::Jump) : (getObjectClass() == BATTLEMECH)`. **Risk LOW / behavior-identical gate-ON** for stock — executor stays class-gated via the `BattleMech` override, so only the capability *fact* is decoupled here, NOT the executor. Executor coupling remains until `LOCOMOTION-DATA-1`. |
| `mover.cpp:3262` | `if (getObjectClass() == BATTLEMECH) {` | guards a passability map check (`worldToCell` / `getPassable`) before allowing the jump | **NOT rewired this slice** — catalogued only. Safe to leave: a non-mech order is already rejected because `canJump` (from `:3256`, now `canPerform(Jump)`) is false and the `if (!canJump)` at `:3268` rejects it before this block matters. Decoupling belongs to `LOCOMOTION-DATA-1` alongside the jump executor. |

`canJump()` itself: declared `virtual bool canJump (void)` in `code/mover.h:1652`
(base returns false) and overridden in `code/mech.h:656`
(`return(numJumpJets > 0);`). Task 6 makes both runtime-gated wrappers over
`canPerform(UnitAction::Jump)`.

`numJumpJets` finalize sites (Task 4 targets, recorded here for completeness):
`code/mech.cpp:1568`, `:2339`, `:3084` (the three `BattleMech::init` overloads,
all `numJumpJets = 5;` inside a fall-through `case COMPONENT_FORM_JUMPJET:` — do
NOT alter that control flow), reset `:1197`/`:2235` (`= 0`), serialized
save `:9282` (`data->numJumpJets = numJumpJets;`) / load `:9353`
(`numJumpJets = data->numJumpJets;`). `numJumpJets` is **instance state**
(member of `BattleMech`, `code/mech.h:292`, serialized in `MechData`
`code/mech.h:366`) — NOT on `BattleMechType` — so the baseline is written from
the instance loadout scan into the shared type profile (Task 4).

## `getObjectClass() == KIND` sweep — `code/mover.cpp` (full)

(Other TUs swept; mover.cpp is where the gameplay-relevant jump gate lives.
Full cross-TU catalog with axis + facet + slice columns lands in Task 8.)

| anchor | code | preliminary axis (Task 8 confirms) |
|---|---|---|
| `:517` | `(mover->getObjectClass() == BATTLEMECH) && ...->inJump` | jump-state FX/anim read |
| `:801` | `run && (mover->getObjectClass() == BATTLEMECH)` | locomotion/run gating |
| `:2435` | `if (getObjectClass() == BATTLEMECH)` | (Task 8) |
| `:2450` | `if (getObjectClass() == BATTLEMECH) {` | (Task 8) |
| `:2630` | `if (getObjectClass() == BATTLEMECH) {` | (Task 8) |
| `:2649` | `else if (getObjectClass() == GROUNDVEHICLE) {` | (Task 8) |
| `:3256` | `bool canJump = (getObjectClass() == BATTLEMECH);` | **JUMP GATE — rewired (see above)** |
| `:3262` | `if (getObjectClass() == BATTLEMECH) {` | **JUMP GATE — catalogued, not rewired** |
| `:3603` | `if (getObjectClass() == BATTLEMECH)` | (Task 8) |
| `:3631` | `if (getObjectClass() == BATTLEMECH)` | (Task 8) |
| `:3698` | `if (getObjectClass() == BATTLEMECH)` | (Task 8) |
| `:4831` | `(getObjectClass() == ELEMENTAL) && target && getGroup()` | elemental-specific |
| `:5050` | `if (getObjectClass() == ELEMENTAL) {` | elemental-specific |
| `:5125` | `if (getObjectClass() == ELEMENTAL) {` | elemental-specific |
| `:5232` | `if (getObjectClass() == ELEMENTAL) {` | elemental-specific |
| `:5360` | `if (getObjectClass() == BATTLEMECH)` | jump-state read (`inJump`) |
| `:5499` | `if (getObjectClass() == ELEMENTAL) {` | elemental-specific |
| `:6066` | `!refitBuddyWID && getObjectClass() == BATTLEMECH` | refit |
| `:6556` | `if (getObjectClass() == BATTLEMECH)` | damage-state |
| `:6726` | `(getObjectClass() == BATTLEMECH) /*&& ...*/` | damage-state |
| `:6885` | `(getObjectClass() == BATTLEMECH) /*&& ...*/` | damage-state |

(Other call-site families — `controlgui.cpp`, `forcegroupbar.cpp`,
`mechicon.cpp`, `missiongui.cpp`, `warrior.cpp` — read `canJump()` / vehicle
jump range and so flow through the rewired seam automatically once gate-ON;
they are not class-checks themselves and need no migration.)

## Vehicle jump baseline note (Task 4 Step 4)

A vehicle's capability baseline stays **default-empty** unless its `.fit` carries
an optional additive `[UnitProfile] Jump = TRUE` block (the slice-1
data-extensibility proof). No redundant `set(CAP_JUMP, false)` is added. Stock
`.fit`s have no `[UnitProfile]` block (grep-confirmed) → stock parity preserved.
