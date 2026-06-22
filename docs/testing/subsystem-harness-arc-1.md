# SUBSYSTEM-HARNESS-ARC-1 — cheap contract/edge-case harness layer

**Status:** HARNESS-TEMPLATE-1 shipped (this doc + reference harness).
**Branch / worktree:** `claude/contract-harnesses-1` @ `A:/Games/mc2-contract-harnesses`
**Base:** nifty HEAD `4c177ea7`.

## Problem

Today every C++ correctness fix (UAF, OOB, watchID exhaustion, allocator
overflow, parser overrun) is first proved by the full 5-mission tier1 smoke.
Smoke is slow (~150s), launches the game, and is a *bad* way to force a rare
edge case — many crash paths are hard to hit naturally in a 30s mission.

**Goal:** a set of small standalone harness executables that link production
code where practical and force exact edge cases **without launching a mission**.
Smoke becomes the final integration gate, not the only proof.

> **Hard principle.** Every crash/corruption fix whose edge case is hard to hit
> naturally in tier1 smoke should get a cheap contract harness or forced
> micro-test.

## Model: `tools/mech_import_harness`

The existing `tools/mech_import_harness` is the proven pattern (do not rewrite):

- **Standalone CMake project** (`project(...)`, its own `build64-harness/` dir).
  It is **NOT** wired into the root `CMakeLists.txt`. Building a harness never
  touches `build64/` and never requires vcpkg.
- **Minimal linkage.** 1A linked only `assimp::assimp`; 1C added exactly one
  mclib TU (`mech_skel_import.cpp`) that includes only Assimp + std. *The linker
  is the proof it stays game-free.*
- **Reporting by exit code + printf.** `FAIL: ...` lines, a summary line, exit
  0 = pass / nonzero = fail. Mode dispatch off `argv[1]`.
- **No game launch, no GL, no gameos, no mission data.**

This arc generalizes that pattern with a shared header and a uniform CLI.

## Harness template convention

### Folder layout

```
tools/contract_harness_common/contract_harness.h   # header-only shared infra
tools/<name>_harness/<name>_harness.cpp            # one harness per subsystem
tools/<name>_harness/CMakeLists.txt                # standalone project
tools/<name>_harness/fixtures/                      # optional test data files
tools/run_contract_tests.py                        # discovers + runs harnesses
```

### CMake pattern

Copy `tools/contract_smoke_harness/CMakeLists.txt`. Each harness is a top-level
`project()` built into its own dir (`build64-contract`, or a per-harness dir).
It adds `tools/contract_harness_common` to its include path and links the
**smallest** production TU subset it needs (often zero). Do **not** add harnesses
to the root `CMakeLists.txt` — keeping them standalone is what guarantees "no
production behavior change."

### CLI convention (uniform, provided by `Harness::run`)

| Flag | Meaning |
|---|---|
| `--list` | list test names (human or `--json`) |
| `--test <name>` | run only the named test (overrides the default suite; can run an `inDefault=false` demo test) |
| `--json` | emit machine-readable JSON instead of human text |
| `--seed <n>` | seed for any randomized test (`TestCtx::seed`) |
| `-h`/`--help` | usage, exit 2 |

### Output format

Human:
```
  [PASS] foo (0 ms)
  [FAIL] bar (1 ms) - file.cpp:42 CH_CHECK(x == y)
my_harness: FAIL (2 tests, 1 failure, 1 ms)
```

JSON (per the arc spec):
```json
{"harness":"contract_smoke_harness",
 "tests":[{"name":"pass","status":"PASS","ms":1}],
 "status":"PASS","elapsed_ms":1}
```

### Exit-code policy

- `0` — all selected tests passed.
- `1` — at least one test failed.
- `2` — usage / unknown arg / no such test.

`run_contract_tests.py` treats the **exit code as authoritative** and also
honors `status:"FAIL"` in JSON.

### Writing a harness

```cpp
#include "contract_harness.h"
using namespace contract_harness;
static bool test_x(TestCtx& t) { CH_CHECK(t, cond); return true; }
int main(int argc, char** argv) {
    Harness h("x_harness");
    h.add("x", test_x);
    h.add("demo_fail", test_demo, /*inDefault=*/false); // only via --test
    return h.run(argc, argv);
}
```

`CH_CHECK` records `file:line` and the expression, does not early-return (so all
checks in a test report). A test fails by returning false or recording any
`CH_CHECK`/`t.fail()`. Exceptions are caught and reported as failures.

## Runtime targets

- Individual harness: **< 1 s**.
- Whole non-GL harness suite: **< 10 s**.
- Keep linked production code minimal — slow harnesses defeat the purpose.

## Production linkage map (planned per harness)

| Harness | Links directly | Globals to stub | Entanglement risk |
|---|---|---|---|
| `contract_smoke_harness` (shipped) | none | none | none — pure template |
| `objmgr_contract_harness` | watch-list math from `objmgr` if extractable | ObjectManager singletons, GameObject ctor chain | **HIGH** — full ObjectManager pulls game globals. First slice may use a narrow extracted helper for watch-list math (no silent duplication). |
| `txmmgr_bounds_harness` | `getVertexBlock`/`getBlock` allocator from `txmmgr` | GL handles, texture upload, global tex manager init | **MED** — allocator cursor math may be extractable from GL. |
| `fit_parse_harness` | `.fit`/`.mdf`/inifile parser TU | file IO globals, FST archive | **MED** — parser may need temp fixture files only. |
| `shader_contract_harness` | none (filesystem inventory) | none | **LOW** — enumerates shader files + deploy manifest, no GL. |
| `render_state_contract_harness` | `RenderPassContract` logic | GL state, real renderer | **LOW** — contract begin/end nesting is mock-only. |

## How this fits with smoke

- **Harness** proves the *forced edge case* deterministically and cheaply.
- **tier1 smoke** proves *integration only* (real load, real GL, real frames).
- A correctness fix should land with a harness test for the edge case **and**
  pass tier1 for integration. The harness is the first proof; smoke is the gate.

## Risks (explicit)

1. **Over-stubbing hides bugs.** A stub that returns a "nice" value can mask the
   very corruption being tested. Prefer linking the real TU; stub only globals
   that are pure environment (no logic under test).
2. **Linking too much engine code makes harnesses slow.** Pulling a full
   subsystem (and its transitive game globals) blows the <1s budget and the
   game-free guarantee. Extract the smallest unit instead.
3. **Duplicating logic instead of calling it.** A reimplemented watch-list/
   allocator/parser in the harness tests the *copy*, not production. If a unit
   can't be linked, extract it into a shared TU that *both* the harness and
   production call — never silently duplicate.
4. **Fake tests that don't exercise the real bug path.** A test must drive the
   actual production code path of the fix. A green harness that doesn't touch the
   fixed code is worse than none.

## Seams: ready vs. needs-recon

- **Ready (low entanglement):** shader inventory/deploy-manifest checks;
  RenderPassContract nesting (mock-only, no GL).
- **Needs recon first (entangled):** `objmgr` watch-list (ObjectManager pulls
  game globals); `txmmgr` allocator (GL-coupled); `.fit`/`.mdf` parser (file IO
  + FST). For each, the first slice must determine whether the unit links
  directly, needs a small extracted shared helper, or is too entangled to test
  cheaply yet.

## Queue (scoped; not started — see Phase 3 in the arc brief)

1. `OBJMGR-CONTRACT-HARNESS-1` — watchID exhaustion, save-loop clamp, corrupt
   watchSave index rejection, `getByWatchID` range checks.
2. `TXMMGR-BOUNDS-HARNESS-1` — exact-fit vs overflow for `getVertexBlock`/
   `getBlock` (cursor unchanged on overflow), untextured add boundary cases.
3. `FIT-PARSE-HARNESS-1` — malformed `.fit`/`.mdf` fail-safe, OOB inventory
   index, `readIdIntArray`/`readIdFloatArray` capacity, oversize multiline.
4. `SHADER-CONTRACT-HARNESS-1` — expected shader file inventory, no removed
   bloom/ACES/FXAA/godray refs survive, include/prefix inventory.
5. `RENDER-STATE-CONTRACT-HARNESS-1` — pass begin/end nesting, missing-end,
   owner mismatch, declared read/write contract validation (mock-only).

**Recommended next:** `SHADER-CONTRACT-HARNESS-1` — lowest entanglement (pure
filesystem, no production link, no GL), so it validates the template against a
real subsystem with minimal risk before tackling the entangled seams.
