# TECHSCRIPT-DISPATCH-1D-VARS-1

Per-unit Var namespace + minimal store for the TechScript brain dispatcher.

## Summary

Adds a fixed-cap per-warrior Var store to `MechBrainRuntime`, with parser and
handlers for `Var.Set` and `Var.Get` verbs. Provides a safe place for the
dispatcher to put state before subroutine support (1E) is added.

Gate: `MC2_BRAIN_DISPATCH_VAR=1` (default OFF). Requires `MC2_BRAIN_DISPATCH=1`.
Gate-OFF: byte-identical — Var.* keep tracing `[BRAIN_DISPATCH_UNKNOWN]` as before.

## Store shape

```
struct VarEntry { char key[32]; char value[32]; VarScope scope; };
struct VarStore  { VarEntry entries[32]; int count = 0; };
```

- Cap: **32 entries** per warrior (`VarStore::kVarStoreCap = 32`).
- Keys and values: `char[32]` (31 chars + NUL). Keys truncated on store if longer.
- Values: raw token text — int/bool/float/string all stored as-is.
- Scope enum (`VarScope`): `Unit = 0` (default), `Mission = 1`.
- Linear-scan get/set. 32 entries is fast enough; no STL containers.
- Allocated inline in `MechBrainRuntime` (no heap). Mission-ephemeral. NOT serialized.
- Freed implicitly when `MechBrainRuntime` is destroyed (same lifecycle as rest of runtime).

## Scope policy

### Unit scope (default)
Key is namespaced per-warrior by the store itself (each warrior has its own `VarStore`
in its `MechBrainRuntime`). `Var.Set "foo" 1` → writes `{key="foo", value="1", scope=Unit}`
to this warrior's store. On overflow (cap=32) → soft-fail trace `[BRAIN_DISPATCH_VAR_SET_FULL]`.

### Mission scope (`scope=Mission`)
**Trace-only in 1D. No shared store, no cross-warrior write.**
Rationale: a shared mission-scope store requires a single-writer guard policy to avoid
cross-warrior nondeterminism (two warriors racing to write the same key). That design
decision is deferred to a later slice. 1D emits `[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE]` /
`[BRAIN_DISPATCH_VAR_GET_MISSION_TRACE]` traces so modders can see their scope=Mission
calls are being observed — and fix them before the write policy lands.

Open question deferred to a later slice: "mission-scope writes: single-writer guard
(owner warrior per key) vs. read-mostly (set once at init, read many) vs. explicit
allowlist?"

## Parser helper

`parseVarVerb` — `code/brain_special_dispatch.cpp` (defined at the block starting
after the recognized-verb table, before `executeSpecialBody_TraceOnly`).

Supports two key-delimiter forms:
- `"key"` — double-quoted (canonical carver spec syntax; for in-memory / non-FIT use)
- `[key]` — bracket-quoted (FIT-safe form; FIT outer-double-quotes cannot embed `"`)

Fixture file uses `[key]` form since FIT `readIdString` reads from first `"` to next `"`.
Value is the next whitespace-delimited token. `scope=Mission` is a literal trailing token.
Returns false (soft-fail) on empty key or missing value for `Var.Set`.
No expression parsing, no escape sequences, no string concatenation.

## Integration points

| Call site | File:line (approx) | Gate condition |
|---|---|---|
| `executeSpecialBody_TraceOnly` intercept | `code/brain_special_dispatch.cpp` ~line 78 | `s_dispatchVarGate()` + verb prefix check |
| `executeSpecialBody_Apply` intercept | `code/brain_special_dispatch.cpp` ~line 138 | same |
| `varStore` member on `MechBrainRuntime` | `code/mech_brain_runtime.h` | always present |
| `VarStore` + `VarEntry` + `VarScope` defs | `code/mech_brain_runtime.h` | inline struct |
| Gate-dependency warn (DISPATCH_VAR w/o DISPATCH) | `code/mission.cpp` ~line 3114 | mission load |
| `executeSpecialBody_TraceOnly` caller passes `varStore` | `code/warrior.cpp` ~line 2323 | Enhanced + `!s_dispatchApply` |
| `executeSpecialBody_Apply` caller passes `varStore` | `code/warrior.cpp` ~line 2293 | Enhanced + `s_dispatchApply` |
| Smoke allowlist for `MC2_BRAIN_DISPATCH_VAR` | `scripts/run_smoke.py` ~line 1360 | smoke env passthrough |

## Forbidden-call guard — UNCHANGED

`Var.Set` and `Var.Get` handlers (`handleVarSet`, `handleVarGet`) call ONLY:
- `store->set` / `store->get` (inline array ops — no malloc, no STL)
- `fprintf` / `fflush`

NO calls to `setGeneralTacOrder`, `setPlayerTacOrder`, `setAlarmTacOrder`,
`requestHelp`, `requestTarget`, `clearCurTacOrder`, `setMainGoal`, `calcTacOrder`,
`coreMoveTo`, or any movement/attack/OPORD-advance/commander function.

Verified by inspection: `handleVarSet` and `handleVarGet` have no warrior pointer and
no `TacticalOrder` type. The only order function in `brain_special_dispatch.cpp` remains
`warrior->setGeneralTacOrder(pdOrder)` at exactly one call site: `Brain.CorePower false`
in `executeSpecialBody_Apply` (1B, UNCHANGED).

## Acceptance evidence

Fingerprint: SHA256 `BA8923EB8CE2AAD73258476FBC40CFBA755B2F885CE6A2E0694DB53717C0A9BE`
(mc2.exe, build from worktree HEAD `2de341d3b6b2` + 1D dirty changes)
Deploy target: `A:/Games/mc2-opengl/mc2-win64-0.4c`

### Gate A (no env)
Tier1 5/5 PASS. No `[BRAIN_*]` lines in logs. Byte-identical baseline.
Run: `2026-06-24T10-01-22`

### Gate B (RUNTIME=1 APPLY=1 FORCE_MODE=enhanced TASKQ=1 DISPATCH=1 DISPATCH_APPLY=1 VAR=0)
mc2_01 PASS. `[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=4` present (1B regression confirmed).
Var.* all trace as `[BRAIN_DISPATCH_UNKNOWN]` — no `[BRAIN_DISPATCH_VAR_*]` lines.
Run: `2026-06-24T09-50-26`

### Gate C (+ VAR=1, no APPLY)
mc2_01 PASS. All six trace lines present:
```
[BRAIN_DISPATCH_VAR_SET] key=foo value=1 scope=Unit wid=4
[BRAIN_DISPATCH_VAR_SET] key=bar value=hello scope=Unit wid=4
[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=shared value=5 wid=4 (write deferred -- no shared-global writes in 1D)
[BRAIN_DISPATCH_VAR_GET] key=foo value=1 scope=Unit wid=4 (hit)
[BRAIN_DISPATCH_VAR_GET] key=nope value=0 scope=Unit wid=4 (default)
[BRAIN_DISPATCH_VAR_GET_MISSION_TRACE] key=shared wid=4 (default -- mission scope read deferred)
```
No `[BRAIN_DISPATCH_APPLY]` line (APPLY=0 → HOLD path; TraceOnly runs, not Apply).
Run: `2026-06-24T09-55-58`

### Gate D (+ APPLY=1)
mc2_01 + mc2_24 PASS 2/2. 1B POWERDOWN fires once. Var.* traces present.
No additional slot writes from Var.*. Supersede-HOLD intact.
Run: `2026-06-24T10-06-31`

### DISPATCH_VAR=1 DISPATCH=0
mc2_01 PASS. Warn line present:
```
[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT
```
Run: `2026-06-24T10-00-18`

## Explicitly NOT in 1D

- BrainSpecial.Call chaining (1E)
- FSM-state DSL (1K)
- New effect verbs (1F+)
- Mission-scope WRITES (deferred; open question above)
- Expression parser, string concatenation, escape sequences
- Save/load persistence
- Multi-warrior cross-talk
- Anything that reads Var values to drive behavior (effect verbs 1G+)
