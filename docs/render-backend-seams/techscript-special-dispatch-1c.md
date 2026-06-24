# TECHSCRIPT-SPECIAL-DISPATCH-1C — FSM-TODO Surfacer

**Branch:** `claude/techscript-dispatch-1c`
**Gate:** `MC2_BRAIN_DISPATCH_FSM_TODO` (default OFF; requires `MC2_BRAIN_DISPATCH=1`)
**Status:** SHIPPED on branch (pending user review + merge)

---

## Discovery: modder specials files have no DSL — only TODO comments

The recon for 1C assumed that auto-converted `mission_specials.fit` files would contain
`Unit.InState`/`Unit.SetState` DSL tokens. Investigation of the actual carver conversion
output revealed the opposite:

> ABL state machine constructs (`state X;` / `trans Y;` / `transBack;` / `endstate;`) survive
> conversion **ONLY as FIT comments** of the form:
> ```
> ; TODO: manual ABL line: state attack;
> ; TODO: manual ABL line: trans noWeapons;
> ```

`FitIniFile` strips comments before key/value parsing. The 1A verb loader
(`parseBrainSpecialBody`) therefore never sees these lines. The FSM skeleton is completely
invisible to the current stack.

**Consequence:** the "FSM-state guard" planned for 1C is premature — there is no live FSM DSL
content to gate. Instead, 1C surfaces the TODO markers as structured traces, giving the modder
and engine team an inventory of what state machine logic the conversion dropped and needs
hand-porting. No speculative DSL added. No behavior change.

---

## What 1C builds

### Gate

`MC2_BRAIN_DISPATCH_FSM_TODO=1` (default OFF).
- Requires `MC2_BRAIN_DISPATCH=1`; warns and is inert if set alone.
- Gate-OFF: byte-identical to pre-1C. No extra parsing, no extra traces, no `std::ifstream` opens.

### Parser hook — `scanFsmTodosFromFile`

**File:** `code/brain_special_dispatch.cpp` (appended after `parseBrainSpecialBody`)
**Declaration:** `code/brain_special_dispatch.h`

Opens `data/missions/<missionName>_specials.fit` with `std::ifstream` (raw text, not FitIniFile)
and line-scans for the pattern:

```
^\s*;\s*TODO:\s*manual ABL line:\s*<payload>
```

For each matching line, classifies the payload with `std::regex`:

| Payload pattern | Kind |
|---|---|
| `^\s*state\s+(\w+);` | `STATE_DEF` — name = captured group |
| `^\s*endstate\s*;?` | `STATE_END` |
| `^\s*trans\s+(\w+);` | `TRANS` — target name = captured group |
| `^\s*transBack\s*;` | `TRANS_BACK` |
| anything else | `OTHER_TODO` — variable decls, misc |

Results stored in `BrainSpecialBody::fsmTodos` (`std::vector<FsmTodoEntry>`, capped at
`BrainSpecialBody::kMaxFsmTodos = 64`).

### Trace emission hook — `code/mission.cpp` (~line 3101)

The FSM-TODO scan runs **once per mission load** (mission-level, not per-warrior) because:
- The specials file is mission-level.
- The scan is information-only; it does not need `getBrainRuntime()` context.
- The per-warrior brainRuntime loop (1A/1B) is unchanged.

Emit format:

```
[BRAIN_DISPATCH_FSM_TODO] mission=mc2_01 wid=-1 stateDefs=N transitions=M otherTodos=L
[BRAIN_DISPATCH_FSM_TODO] kind=STATE_DEF name=start wid=-1
[BRAIN_DISPATCH_FSM_TODO] kind=TRANS target=attack wid=-1
[BRAIN_DISPATCH_FSM_TODO] kind=TRANS_BACK wid=-1
[BRAIN_DISPATCH_FSM_TODO] kind=STATE_END wid=-1
```

`wid=-1` signals mission-level context (no specific warrior). `OTHER_TODO` entries are counted
in the summary (`otherTodos=`) but suppressed from detail lines (noise reduction).

### Forbidden-call contract (unchanged from 1B; 1C addition annotated)

`scanFsmTodosFromFile` calls **ONLY**: `std::ifstream` + `std::regex` + `fprintf`/`fflush`.
No order functions. No movement/attack/OPORD calls. No warrior pointer. No MechWarrior type.
No tac-order writes. Verified by inspection.

---

## Test fixture

`tests/fixtures/brain_runtime/mc2_01_specials.fit` extended with:

```
; TODO: manual ABL line: state start;
; TODO: manual ABL line: trans attack;
; TODO: manual ABL line: transBack;
; TODO: manual ABL line: endstate;
; TODO: manual ABL line: real[3] worldLoc;
```

The existing DO verbs (`Brain.CorePower false`, etc.) are preserved for 1A/1B regression.

---

## Gate matrix validation (smoke mc2_01, 30s each)

| Gates | Result | FSM_TODO lines in log |
|---|---|---|
| none | PASS | 0 (byte-identical) |
| RUNTIME=1 APPLY=1 DISPATCH=1 | PASS | 0 (no FSM_TODO gate) |
| RUNTIME=1 APPLY=1 DISPATCH=1 **FSM_TODO=1** | PASS | summary + 4 detail lines (see below) |
| RUNTIME=1 APPLY=1 DISPATCH=1 **DISPATCH_APPLY=1** FSM_TODO=1 | PASS | same FSM_TODO lines; 1B behavior unchanged |

### Verbatim FSM_TODO trace (gate 3 and gate 4):

```
[BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission=mc2_01
[BRAIN_DISPATCH_FSM_TODO] mission=mc2_01 wid=-1 stateDefs=1 transitions=2 otherTodos=1
[BRAIN_DISPATCH_FSM_TODO] kind=STATE_DEF name=start wid=-1
[BRAIN_DISPATCH_FSM_TODO] kind=TRANS target=attack wid=-1
[BRAIN_DISPATCH_FSM_TODO] kind=TRANS_BACK wid=-1
[BRAIN_DISPATCH_FSM_TODO] kind=STATE_END wid=-1
```

(`otherTodos=1` = the `real[3] worldLoc;` line; not in detail trace.)

---

## What 1C does NOT do (deferred)

- Per-unit `activeState` field
- `Unit.InState` / `Unit.SetState` DSL handling
- FSM execution gating
- New effect verbs
- Variable store / `Var.*` writes
- Save changes
- Multi-warrior beyond existing single-warrior fixture
- FitIniFile changes
- Per-warrior FSM TODO storage (fsmTodos on brainRuntime; 1C uses a temporary mission-level body)

---

## Files changed

| File | Change |
|---|---|
| `code/brain_special_dispatch.h` | Added `FsmTodoKind` enum, `FsmTodoEntry` struct, `fsmTodos` field on `BrainSpecialBody`, `scanFsmTodosFromFile` declaration |
| `code/brain_special_dispatch.cpp` | Added `#include <fstream>`, `#include <regex>`, implemented `scanFsmTodosFromFile` |
| `code/mission.cpp` | Refactored DISPATCH block (~line 3101): 1A/1B per-warrior loop unchanged; 1C mission-level FSM_TODO scan added after it |
| `tests/fixtures/brain_runtime/mc2_01_specials.fit` | Added 5 TODO comment lines (1 each: STATE_DEF, TRANS, TRANS_BACK, STATE_END, OTHER_TODO) |
| `docs/render-backend-seams/techscript-special-dispatch-1c.md` | This file |
