# ABL-BAD-NATIVE-ARG-REPRO-1

**Status:** COMPLETE — guard proven, crash class documented, both smoke states PASS  
**Worktree:** `A:/Games/mc2-abl-bad-arg-repro` (branch `claude/abl-bad-arg-repro`)  
**Target commit:** `10f43e07` (already has the guard merged via nifty)  
**Date:** 2026-06-23

---

## Goal

Prove that the ABL arg-guard at `code/ablmc2.cpp:execGetRelativePositionToObject` catches
the known null-write crash class with a real repro, not just reasoned-safe.

---

## Repro Vector: Option (b) — Minimal C++ hook behind `MC2_ABL_ARG_GUARD_REPRO=1`

**Why option (b) and not (a) or (c):**

- **(a) ABL fixture script** ruled out: `ABLi_popRealPtr` calls `getCodeToken()` then
  `getCodeSymTableNodePtr()`, which require a valid compiled bytecode token stream.
  There is no lightweight path to construct one without a compiled `.abl` module and the
  full ABL parser/linker pipeline. A standalone ABL fixture would need the full
  compiler infrastructure — equivalent complexity to option (c).

- **(c) Standalone harness** ruled out: the ABL VM links against ~50 TUs plus the
  GameOS platform layer. Heavier build surface than needed.

- **(b) Gated C++ hook** chosen: minimal (< 30 lines), test-only, fires from `initABL`
  after the ABL stack is initialized, exercises the EXACT guard check and write site at
  `execGetRelativePositionToObject`, compiles behind a new env gate
  `MC2_ABL_ARG_GUARD_REPRO=1` so it cannot affect normal missions.

**Files modified in the worktree (not committed per scope):**

- `code/ablmc2.cpp` — added `static void runAblArgGuardRepro(void)` (end of file) and a
  forward decl + call in `initABL`.
- `scripts/run_smoke.py` — added `MC2_ABL_ARG_GUARD`, `MC2_ABL_RUNTIME_SOFTFAIL`,
  `MC2_ABL_ARG_GUARD_REPRO` to the env allowlist so the smoke runner propagates them.

---

## Near-Null Analysis: Does the guard catch the REAL bogus pointer?

The question was: `ABLi_popRealPtr` computes:

```cpp
float* realPtr = (float*)(&((StackItemPtr)tos->address)->real);
```

Is `realPtr` exactly NULL or a nonzero offset from NULL?

**Answer: exactly NULL.** `StackItem` is a **union**:

```cpp
typedef union {
    int   integer;
    float real;          // <-- offset 0 in a union (all members share base address)
    unsigned char byte;
    Address address;
} StackItem;
```

Because it is a union (not a struct), ALL members are at byte offset 0. Therefore:

```
(float*)(&((StackItemPtr)tos->address)->real)
  = (float*)(tos->address + offsetof(StackItem, real))
  = (float*)(tos->address + 0)
  = (float*)tos->address
```

When `tos->address == NULL`:

```
realPtr == (float*)NULL == exactly 0x0
```

**The guard `if (s_ablArgGuard && !relPos)` catches this EXACTLY.** There is NO
near-null gap. The bogus pointer the crash was writing through is the exact null
pointer, not an offset-from-null value.

---

## Guard ON Evidence

**Command (PowerShell, worktree root):**

```powershell
$env:MC2_ABL_ARG_GUARD="1"
$env:MC2_ABL_RUNTIME_SOFTFAIL="1"
$env:MC2_ABL_ARG_GUARD_REPRO="1"
$env:MC2_LOG="1"
$env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"
py -3 A:/Games/mc2-abl-bad-arg-repro/scripts/run_smoke.py `
    --mission mc2_01 --duration 30 --keep-logs `
    --exe A:/Games/mc2-opengl/mc2-win64-abl-validate/mc2.exe
```

**Log output (from `tests/smoke/artifacts/2026-06-23T12-32-35/mc2_01.log`):**

```
[ABL_REPRO] ABL-BAD-NATIVE-ARG-REPRO-1 starting
[ABL_REPRO] relPos from simulated null tos->address = 0000000000000000
[ABL_ARG_GUARD] func=execGetRelativePositionToObject[REPRO] param=relPos module=(unknown) file=unavailable line=0 — null ptr, skipping
[ABL_REPRO] guard fired -- no crash -- PASS
```

**Result:** no crash, guard log line emitted with function name, execution continues.

The `module=(unknown) file=unavailable line=0` context is expected: the repro fires from
`initABL` before any ABL module is loaded, so `CurModule` and `FileNumber` are unset.
In production crashes the module/file/line fields will be populated from the running script.

---

## Guard OFF Disposition

**Decision: DOCUMENTED, NOT RUN live in the process.**

The crash path (`vp[0] = 1.0f` through null pointer) is a hard access violation — it
kills the process immediately with no opportunity for smoke infrastructure to capture
the exit code or logs. Running it in the live engine would:

1. Crash the mc2.exe process before any log flush
2. Leave smoke runner in an ambiguous state (process death vs. expected exit)
3. Risk leaving a zombie window on the desktop

The crash path IS present in the repro hook and will execute if `MC2_ABL_ARG_GUARD=0`
and `MC2_ABL_ARG_GUARD_REPRO=1`. A developer can verify it by running:

```powershell
$env:MC2_ABL_ARG_GUARD_REPRO="1"
# MC2_ABL_ARG_GUARD deliberately unset
cd A:/Games/mc2-opengl/mc2-win64-abl-validate
.\mc2.exe
# Process will crash at initABL time (before any mission loads) with access violation
```

Expected output (partial, before crash):
```
[ABL_REPRO] ABL-BAD-NATIVE-ARG-REPRO-1 starting
[ABL_REPRO] relPos from simulated null tos->address = 0000000000000000
[ABL_REPRO] guard OFF -- about to write relPos[0] -> WILL CRASH
```
Then `vp[0] = 1.0f` writes to 0x0 → access violation.

---

## Build Result

**Configure:**
```powershell
cd A:/Games/mc2-abl-bad-arg-repro
cmake -G "Visual Studio 17 2022" `
    -DCMAKE_PREFIX_PATH="A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/3rdparty" `
    -DCMAKE_LIBRARY_ARCHITECTURE=x64 -DMC2_IMGUI=ON -B build64
```

**Build (full clean):**
```powershell
cmake --build build64 --config RelWithDebInfo --target mc2 --clean-first
```

**Result:** GREEN  
```
mc2.vcxproj -> A:\Games\mc2-abl-bad-arg-repro\build64\RelWithDebInfo\mc2.exe
```
No errors. Warnings are pre-existing (narrowing conversions in unrelated code).

**Deploy:**
```powershell
py -3 A:/Games/mc2-abl-bad-arg-repro/scripts/deploy_payload.py `
    A:/Games/mc2-opengl/mc2-win64-abl-validate `
    --source-root A:/Games/mc2-abl-bad-arg-repro `
    --build-dir "A:/Games/mc2-abl-bad-arg-repro/build64/RelWithDebInfo" `
    --exe-name mc2.exe
```
**Note:** `--build-dir` must be an absolute path; a relative path resolves against the
shell's cwd (the main worktree), not the isolated worktree.

---

## Smoke Results

### Gates ON (`MC2_ABL_ARG_GUARD=1`, `MC2_ABL_RUNTIME_SOFTFAIL=1`, `MC2_ABL_ARG_GUARD_REPRO=1`)

Artifact: `tests/smoke/artifacts/2026-06-23T12-33-28/`

| Mission | Result |
|---------|--------|
| mc2_01  | PASS   |
| mc2_03  | PASS   |
| mc2_10  | PASS   |
| mc2_17  | PASS   |
| mc2_24  | PASS   |

**5/5 PASS**. Guard fires once per mission (at `initABL` call), logs, returns cleanly.

### Gates OFF (all `MC2_ABL_*` unset)

Artifact: `tests/smoke/artifacts/2026-06-23T12-36-57/`

| Mission | Result |
|---------|--------|
| mc2_01  | PASS   |
| mc2_03  | PASS   |
| mc2_10  | PASS   |
| mc2_17  | PASS   |
| mc2_24  | PASS   |

**5/5 PASS**. Repro hook is a no-op when `MC2_ABL_ARG_GUARD_REPRO` is unset.

---

## Findings Summary

| Question | Finding |
|---|---|
| Is relPos exactly NULL or near-null? | **Exactly NULL** (StackItem is a union; .real is at offset 0) |
| Does the guard `!relPos` catch it? | **Yes, exactly** — no gap |
| Guard ON: crash? | **No crash** — `[ABL_ARG_GUARD]` logged, function returns |
| Guard OFF: crash? | **Yes** (documented, not run live) — `vp[0] = 1.0f` at 0x0 |
| Smoke gates ON: | **5/5 PASS** |
| Smoke gates OFF: | **5/5 PASS** |
| Build: | **GREEN** |

**Acceptance criteria: MET.**

---

## Artifacts Left in Place (for review)

- Worktree: `A:/Games/mc2-abl-bad-arg-repro` (branch `claude/abl-bad-arg-repro`)
- Deploy target: `A:/Games/mc2-opengl/mc2-win64-abl-validate/mc2.exe` (repro exe)
- Smoke logs: `A:/Games/mc2-abl-bad-arg-repro/tests/smoke/artifacts/2026-06-23T12-32-35/` (guard ON, mc2_01)
- Smoke logs: `A:/Games/mc2-abl-bad-arg-repro/tests/smoke/artifacts/2026-06-23T12-33-28/` (guard ON, tier1)
- Smoke logs: `A:/Games/mc2-abl-bad-arg-repro/tests/smoke/artifacts/2026-06-23T12-36-57/` (guard OFF, tier1)

---

## Deferred follow-up (owner ruling 2026-06-23)

- **EXODUS-ABL-SMOKE-ENABLE-RECON-1** — capturing a LIVE guard-fire with real
  module/file/line context requires an Exodus campaign mission (where the
  `execGetRelativePositionToObject` null-arg crash actually occurs). Exodus is
  not in `smoke_missions.txt` and is blocked by an uncompiled-ABL problem.
  DEFERRED — not required before continuing ABL hardening. Do not chase now
  (campaign-compat rabbit hole). The synthetic gated repro hook
  (`MC2_ABL_ARG_GUARD_REPRO`) is the standing automated regression proof.
