<!-- DISPATCH-LOADER-RAW-1 — design-as-built -->
# DISPATCH-LOADER-RAW-1: Raw brace-block loader for TechSpecial body verbs

## Why this was necessary

The prior loader (slices 1A–1D) used `FitIniFile::readIdString` to extract `DO0`, `DO1`, ... values from a `[BrainSpecial] [Body]` section. `FitIniFile` parses FIT-format key/value lines where strings are delimited by outer double-quotes:

```
st DO4 = "Var.Set [foo] 1"
```

The real carver_v_enhanced mission files use a brace-block `TechSpecial { Body { ... } }` format where DO lines contain inline-quoted args:

```
DO Var.Set "ScenarioResult" PLAYING scope=Mission
DO TechSpecial.Call "mission.mc2_01.special.init"
```

`FitIniFile` truncates strings at the first inner `"`, so `Var.Set "foo" 1` loads as `Var.Set ` (empty after verb). The 1D `parseVarVerb` function supports both `"key"` and `[key]` delimiters, but it never received the quoted form because the loader ate it first.

## Design

### Dual-mode loader (`parseBrainSpecialBody`)

`code/brain_special_dispatch.cpp` — public entry point unchanged.

1. **Try raw brace-block scanner** (`parseBrainSpecialBody_RawScan`): `std::ifstream` line-by-line scan. State machine: `OUTER → IN_SPECIAL → IN_BODY`. Finds `TechSpecial {` blocks, finds `Body {` sub-blocks, collects all `DO <rest>` lines as verb strings.
2. **Fallback to FitIniFile** (`parseBrainSpecialBody_FitIni`): the original 1A–1D logic, unchanged. Activates only when the raw scanner finds zero TechSpecial blocks (bracket-form fixtures: `[BrainSpecial] [Body] DO0=...`).

Both paths populate `BrainSpecialBody.verbs` identically. Everything downstream (verb dispatch, Var.Set/Get handlers, POWERDOWN effect) is unmodified.

### Scanner details

`parseBrainSpecialBody_RawScan` in `brain_special_dispatch.cpp`:

- Opens file with `std::ifstream`.
- Per-line: strip comments (`;`-prefixed), strip trailing whitespace + CR.
- State machine transitions:
  - OUTER → IN_SPECIAL on `TechSpecial {`
  - IN_SPECIAL → IN_BODY on `Body {`; back to OUTER on bare `}`
  - IN_BODY: `DO <rest>` → push verb (strip trailing `;` and whitespace); `STOP` → skip; `}` → back to IN_SPECIAL
- Multi-block: ALL TechSpecial bodies are parsed; verbs accumulate in order.
- Key/alias/type/sourceABLFunction fields are NOT parsed (not needed for verb dispatch in 1A–1E scope).

### Safety caps

| Cap | Value | Scope |
|-----|-------|-------|
| `kMaxLineLen` | 512 bytes | Lines longer than this are skipped with a `[BRAIN_DISPATCH_RAW] WARN` trace |
| `kMaxVerbsPerBody` | 256 | Excess DO lines within one Body block are skipped |
| `kMaxBodies` | 256 | Excess TechSpecial blocks stop processing entirely |

Caps are `static const int` at file scope (brain_special_dispatch.cpp ~190–192).

### No nested braces

The scanner does not recurse on nested braces. Inspection of carver content confirms `Body { ... }` blocks are not nested (STOP terminates them; inner `{`s don't occur in DO lines). If a `{` appears inside a Body line it would be treated as part of a verb string (the scanner only transitions on bare `}`).

### FORBIDDEN-CALL GUARD preserved

`parseBrainSpecialBody_RawScan` and `parseBrainSpecialBody_FitIni` call only `std::ifstream` + `FitIniFile` API + `std::fprintf`. No `setGeneralTacOrder` or any order/movement function. The POWERDOWN call remains exclusively in `executeSpecialBody_Apply` (1B, unchanged).

## Fixtures

| File | Form | Purpose |
|------|------|---------|
| `tests/fixtures/brain_runtime/mc2_01_specials.fit` | Bracket `[BrainSpecial]/[Body]/DO0=...` | 1A–1D regression; FitIni fallback path |
| `tests/fixtures/brain_runtime/mc2_01_specials_raw.fit` | Brace-block `TechSpecial { Body { DO ... } }` | Gate C/D raw scanner path; exercises inline-quoted `"foo"` key form |
| `tests/fixtures/brain_runtime/mc2_01_specials_carver_slice.fit` | Verbatim carver_v_enhanced content | Headline proof: real content loads |

## Gate matrix (acceptance)

| Gate | Env | Fixture | Result | Key evidence |
|------|-----|---------|--------|--------------|
| A | (none) | bracket | PASS | No BRAIN traces; byte-identical |
| B | RUNTIME+APPLY+DISPATCH+DISPATCH_APPLY | bracket | PASS | `[BRAIN_DISPATCH_RAW] no TechSpecial blocks … trying FitIni fallback`; `[BRAIN_DISPATCH_RAW] FitIni fallback loaded …: 10 verbs` |
| C | RUNTIME+APPLY+DISPATCH+DISPATCH_VAR | raw | PASS | `[BRAIN_DISPATCH] parsed mc2_01_specials.fit: 7 verbs`; `[BRAIN_DISPATCH_VAR_SET] key=foo value=1 scope=Unit wid=4`; `[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=shared value=5 wid=4` |
| D | + DISPATCH_APPLY | raw | PASS | `[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=4` (fires once) |
| Headline | RUNTIME+APPLY+DISPATCH+DISPATCH_VAR | carver_slice | PASS | `[BRAIN_DISPATCH_UNKNOWN] verb=TechSpecial.Call "mission.mc2_01.special.init" wid=4`; `[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=ScenarioResult value=PLAYING wid=4` |

Fingerprint: exe sha=`b1396af061b6` dirty=1 branch=`claude/dispatch-loader-raw-1`. All runs confirmed `[DEPLOY_FINGERPRINT] OK: exe sha=b1396af061b6 matches worktree HEAD`.

## Evidence: real carver content now loads

Verbatim trace from carver_slice run (artifact `2026-06-24T10-32-01`):

```
[BRAIN_DISPATCH] parsed mc2_01_specials.fit: 11 verbs
[BRAIN_DISPATCH_UNKNOWN] verb=TechSpecial.Call "mission.mc2_01.special.init" wid=4
[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=ScenarioResult value=PLAYING wid=4 (write deferred -- no shared-global writes in 1D)
[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=PlayerForceDead value=false wid=4 (write deferred -- no shared-global writes in 1D)
[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=ClanForceDead value=false wid=4 (write deferred -- no shared-global writes in 1D)
```

The verb `TechSpecial.Call "mission.mc2_01.special.init"` preserves the full inline-quoted arg that FitIniFile would have truncated. The `Var.Set "ScenarioResult" PLAYING` line is parsed as key=`ScenarioResult` value=`PLAYING` (unquoted value form) — correct.

## Scope

EXPLICITLY NOT in this slice: new verbs, effect changes, FitIniFile changes to other subsystems, var-store changes, call-chain work (1E), FSM execution, save changes.
