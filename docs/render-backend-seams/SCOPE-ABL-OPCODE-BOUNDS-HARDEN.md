# SCOPE: ABL-OPCODE-BOUNDS-HARDEN

**Status:** SCOPED — not started  
**Branch:** off `claude/nifty-mendeleev`  
**Blocks:** TECHSCRIPT-SPECIAL-DISPATCH (Brain 2.14 native Special bindings)  
**Parallel-safe with:** BRAIN-FIT-SCHEMA-1  
**Prior recon:** `docs/render-backend-seams/borrow-scripting-1.md` §crash-surface, §ABL-RUNTIME-SOFTFAIL  
**Prereq surface from:** `docs/render-backend-seams/brain-ai-2.14-techscript-recon-1.md` §Specials  

---

## Goal

Harden the ABL native-binding dispatch path so that:

1. An out-of-range function index cannot dispatch into garbage memory.
2. A null or bad out-pointer passed to any `exec*` binding fails gracefully instead of crashing.
3. A gate-controlled soft-fail mode preserves a mission when a buggy-mod script hits the guard.
4. Trace output is rich enough to identify the offending script, function, and source line without attaching a debugger.

This is a **crash-safety prerequisite only**. It adds no new ABL language features, no new native bindings, no TechScript bridge, and no behavior conversion of existing scripts.

---

## Why this is a prerequisite

`docs/render-backend-seams/brain-ai-2.14-techscript-recon-1.md` proposes adding TechScript Specials as a new class of native ABL bindings (modeled on `ablmc2.cpp`'s existing 291-entry `ABLi_addFunction` registry). Every new binding added by that work inherits the identical unguarded-out-pointer crash class documented in `borrow-scripting-1.md`. Shipping that dispatch surface before this hardening layer multiplies the crash footprint proportionally. The guard must land first.

---

## Non-goals (explicitly out of scope)

- ABL language replacement or interpreter rewrite.
- TechScript bridge or any new binding registration.
- New opcodes or new ABL language features.
- Behavior conversion of existing ABL scripts.
- Changes to `.abl` compilation or the FitIni load path.
- Changing the `ABLFatalCallback` registration signature.

---

## Grounded current state

### Registry — `mclib/ablsymt.cpp` + `mclib/ablsymt.h`

- `MAX_STANDARD_FUNCTIONS` = **512** (`ablsymt.h:166`).
- `NumStandardFunctions` starts at `NUM_ABL_ROUTINES` (65 built-ins, `ablsymt.h:65`) and grows with each `ABLi_addFunction` call.
- `code/ablmc2.cpp` registers **291** native MC2 bindings (confirmed by `grep -c ABLi_addFunction`). Total live count at runtime = ~356 entries.
- `FunctionCallbackTable[MAX_STANDARD_FUNCTIONS]` and `FunctionInfoTable[MAX_STANDARD_FUNCTIONS]` are parallel fixed-size arrays (`ablsymt.cpp:70-72`). Registration overflow guard added at `ablsymt.cpp:529-535` (skip + log on full; earlier 256-cap OOB-write fixed).

### Dispatch path — `mclib/ablxstd.cpp:975-1034`

The `execStdFunction()` default case dispatches by `RoutineKey key`:

```
ablxstd.cpp:985  — EXISTING bounds check: key < 0 || key >= NumStandardFunctions → ABL_Fatal
ablxstd.cpp:1007 — FunctionCallbackTable[key] read
ablxstd.cpp:1014 — EXISTING bogus-pointer guard: cbVal < 0x00007F0000000000 → log + synthesize return
ablxstd.cpp:1025 — (*cb)() dispatch
```

**Finding:** the index bounds check at `ablxstd.cpp:985` and the bogus-pointer guard at `ablxstd.cpp:1014` are already present from prior hardening. Both paths currently call `ABL_Fatal` (which routes through `ABLFatalCallback`, a process-abort in release). They are NOT soft-fail. A mod brain that triggers either guard still kills the mission.

### The proven crash site — `code/ablmc2.cpp:4365-4399`

`execGetRelativePositionToObject` registered as `ABLi_addFunction("getrelativepositiontoobject", ...)` (exact name not shown in the registry block — look up by `execGetRelativePositionToObject` symbol). The body:

```cpp
// ablmc2.cpp:4391
float* relPos = ABLi_popRealPtr();   // pops a VAR/OUT pointer from the ABL stack
...
if (object && object->isMover()) {
    relPos[0] = newPos.x;            // line ~4397: UNGUARDED write if relPos == NULL
    relPos[1] = newPos.y;
    relPos[2] = newPos.z;
}
```

`ABLi_popRealPtr` (`ablxstd.cpp:163`) returns `float*` pointing into ABL stack memory derived from `tos->address`. If the calling script passed a NULL or wrong-type argument, `relPos` is NULL or a stale stack address; the write at ~4397 faults. No null-check exists before the write. This is the confirmed crash case cited in `borrow-scripting-1.md:27`.

This pattern — `ABLi_popRealPtr()` / `ABLi_popIntegerPtr()` return value used without a null check — is repeated across an estimated ~250 `exec*` bindings in `code/ablmc2.cpp` that take array or reference output parameters.

### The fatal path — `mclib/ablerr.cpp:186-206`

`runtimeError(errCode)` → `ABL_Fatal(-ABL_ERR_RUNTIME_ABORT, message)` → `ABLFatalCallback(errCode, s)`.  
`ABLFatalCallback` is set at `mclib/ablrtn.cpp:387` from whatever callback the caller installs at `ABLi_init` time.  
In release MC2 this resolves to a hard process abort with an error dialog — no per-call unwind, no per-module recovery.

### Existing MC2_ABL_* gates

- `MC2_ABL_REG_TRACE` — `ablsymt.cpp:544`. Dumps function key/name/callback at registration time. Default OFF.
- `MC2_ABL_TRACE` — `ablmc2.cpp:7035,7081,7099,7112,7125,7150`. Logs stub-function calls. Default OFF.

No soft-fail gate and no out-pointer guard gate exist today.

---

## Proposed hardening

### H1 — Out-pointer null guard in `exec*` bindings (the proven crash fix)

Add a `MC2_ABL_ARG_GUARD`-gated null check immediately after each `ABLi_popRealPtr()` / `ABLi_popIntegerPtr()` / `ABLi_popBooleanPtr()` call in `code/ablmc2.cpp` bindings that write through the pointer.

Pattern (using `execGetRelativePositionToObject` as the template):

```cpp
float* relPos = ABLi_popRealPtr();
#ifdef ABL_ARG_GUARD_ENABLED   // or runtime check — see H3
if (!relPos) {
    ABL_arg_guard_fail("execGetRelativePositionToObject", "relPos", CurModule->getName(), execLineNumber);
    return;  // stack already balanced by popRealPtr; synthesize no return value (void binding)
}
#endif
```

For the initial slice, prioritize the ~30 bindings that write through `Real[N]` or `Integer[N]` output arrays (the array-write pattern is the highest-severity class). The scope doc does NOT require all 250 to be swept in a single commit; the acceptance criteria target the proven site plus a representative sweep of array-output bindings.

### H2 — Index bounds enforcement upgrade at dispatch

`ablxstd.cpp:985` already has the bounds check; it calls `ABL_Fatal`. Under `MC2_ABL_RUNTIME_SOFTFAIL=1` (H3), replace `ABL_Fatal` here with the soft-fail path (log + synthesize return + continue).

No new bounds logic is required — only the error disposition changes under the gate.

### H3 — Soft-fail / fatal gate

Introduce two runtime gates, consistent with the `MC2_*` env-var convention:

| Gate | Default | Effect |
|---|---|---|
| `MC2_ABL_ARG_GUARD=1` | OFF (0) | Enables H1 null-checks and H2 dispatch-bounds soft path. Gate-off = byte-identical to current behavior. |
| `MC2_ABL_RUNTIME_SOFTFAIL=1` | OFF (0) | Replaces `ABL_Fatal` calls inside `runtimeError()` and the dispatch-bounds guard with log + synthesize-return + continue. Only meaningful when `MC2_ABL_ARG_GUARD=1`. |

Default-OFF preserves exact current semantics for all valid scripts, satisfying the regression-safety requirement.

Implementation note: both gates should be read once at startup (static `bool` initialized from `getenv()`), following the `MC2_ABL_REG_TRACE` pattern at `ablsymt.cpp:544`.

### H4 — Trace output

All guard paths must emit a structured trace line on `stdout` (mirroring the existing `[ABL_BAD_CB]` pattern at `ablxstd.cpp:1016-1018`) sufficient to identify the fault in smoke logs without a debugger:

```
[ABL_ARG_GUARD] func=execGetRelativePositionToObject param=relPos module=<name> file=<src> line=<N> — null ptr, skipping
[ABL_SOFTFAIL]  key=<N> func=<name> module=<name> file=<src> line=<N> errCode=<N> — continuing
```

Fields required: function/binding name, param name or index, CurModule name, source file (from `CurModule->getSourceFile(FileNumber)` if available, else "unavailable"), `execLineNumber`. These are already available at the dispatch site — see `ablerr.cpp:205` for the existing format.

---

## Env gates (summary)

| Gate | Files touched | Notes |
|---|---|---|
| `MC2_ABL_ARG_GUARD` | `code/ablmc2.cpp`, `mclib/ablxstd.cpp` | New. Default OFF. Enables all H1/H2 guards. |
| `MC2_ABL_RUNTIME_SOFTFAIL` | `mclib/ablerr.cpp` | New. Default OFF. Requires `MC2_ABL_ARG_GUARD`. Replaces ABL_Fatal with log+continue. |
| `MC2_ABL_REG_TRACE` | `mclib/ablsymt.cpp` | Existing. Unchanged. |
| `MC2_ABL_TRACE` | `code/ablmc2.cpp` | Existing. Unchanged. |

---

## Acceptance criteria

**AC-1 — Proven crash site fixed.**  
With `MC2_ABL_ARG_GUARD=1 MC2_ABL_RUNTIME_SOFTFAIL=1`: calling `execGetRelativePositionToObject` from a script that passes a NULL/bad pointer no longer crashes. The guard logs `[ABL_ARG_GUARD]` to stdout, skips the write, and returns. The ABL VM continues executing subsequent orders.

**AC-2 — Out-of-range index rejected.**  
With `MC2_ABL_ARG_GUARD=1 MC2_ABL_RUNTIME_SOFTFAIL=1`: injecting a RoutineKey >= NumStandardFunctions into the dispatch (e.g. via a test module or a forced bad key) logs `[ABL_SOFTFAIL]` and returns a default value rather than ABL_Fatal-aborting the process.

**AC-3 — Default behavior byte-identical.**  
With both gates OFF (default): no behavioral change versus baseline for any script that exercises `execGetRelativePositionToObject` with a valid pointer. Verified by tier1 smoke passing at exit 0 on both `MC2_ABL_ARG_GUARD=0` and `MC2_ABL_ARG_GUARD=1 MC2_ABL_RUNTIME_SOFTFAIL=0`.

**AC-4 — Trace identifies module + file + function + line.**  
A forced guard trigger (AC-1 or AC-2 scenario) produces a log line on stdout containing all four fields: function/binding name, module name, source file name (or "unavailable"), exec line number.

**AC-5 — Tier1 smoke unaffected.**  
Canonical smoke command (from `CLAUDE.md`) exits 0 with `MC2_ABL_ARG_GUARD=1 MC2_ABL_RUNTIME_SOFTFAIL=1`. No new crashes, no new error-dialog hangs. (Do not run the smoke command during this scope-authoring step — state it as the regression gate for the implementation commit.)

---

## Relink determination

`code/ablmc2.cpp` is a large TU (~7800 lines, all 291 MC2 native bindings). Any change to it requires a full exe relink (~4-5 min on this machine). `mclib/ablxstd.cpp` and `mclib/ablerr.cpp` are also `mclib` TUs that link into the main binary. All three will need recompile and relink. Plan for a full `--clean-first` relink on the implementation commit. No class-layout changes are involved (gates are `static bool` locals or added function parameters are pointer-only) — ABI is stable.

---

## Dependencies and ordering

```
[NOW]   ABL-OPCODE-BOUNDS-HARDEN  (this slice)
           ↓ blocks
[NEXT]  TECHSCRIPT-SPECIAL-DISPATCH  (Brain 2.14 Specials — new native bindings)
           ↓ also depends on
        BRAIN-FIT-SCHEMA-1  (parallel-safe with this slice)
```

No dependency on any in-flight render arc (RENDER-BACKEND-SEAMS, FRAME-JOBS, RenderWorld). Safe to start from clean nifty HEAD.
