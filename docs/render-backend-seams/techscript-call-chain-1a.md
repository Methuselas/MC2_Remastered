# TECHSCRIPT-CALL-CHAIN-1A — Design-as-Built

Gate: `MC2_BRAIN_DISPATCH_CALL=1` (requires `MC2_BRAIN_DISPATCH=1`; warns + inert otherwise).

## Index shape

`SpecialIndex` = `std::vector<SpecialIndexEntry>`. One entry per TechSpecial brace-block found by `parseBrainSpecialBody_RawScan`. Each entry holds:
- `key` (std::string) — from the `key=` field in the TechSpecial block.
- `body` (BrainSpecialBody) — verbs from that block's `Body { DO ... }` section.

Stored on `MechBrainRuntime::specialIndex`. Mission-ephemeral (same lifecycle as `specialBody`). No STL hash; linear-scan lookup via `specialIndexFind()`. Populated unconditionally by the raw scanner regardless of `DISPATCH_CALL` gate state (gate controls runtime call resolution only).

**Entry-body selection rule** (for the existing `specialBody` member / root dispatch target):
1. First TechSpecial block whose `key` contains `"scenario_main"` (case-sensitive).
2. Else first block with `type = "MissionSpecial"`.
3. Else first block found.

## Visited-set scope

`std::vector<std::string>` created **per-dispatch-tick**, on the stack in `executeSpecialBody_TraceOnly` / `executeSpecialBody_Apply`. Cleared on entry. Passed by reference into `executeSpecialBody_TraceOnlyChained`. Not persistent across ticks.

Initial population: if `callerKey` is non-empty (root body has a known key), it is pushed at entry so a direct self-call from root is caught as a cycle.

## Depth limit

`kCallChainMaxDepth = 8` (hardcoded in `brain_special_dispatch.cpp`). Corpus max observed depth = 2 (per recon). The limit matches the recon recommendation with a 4× headroom for future content.

## Trace formats

| Condition | Format |
|-----------|--------|
| Successful call resolved | `[BRAIN_DISPATCH_CALL] from=<caller_key_or_root> to=<key> depth=<N> wid=<W>` |
| Verb inside called body (recognized) | `[BRAIN_DISPATCH] depth=<N> verb=<verbstr> wid=<W>` |
| Verb inside called body (unknown) | `[BRAIN_DISPATCH_UNKNOWN] depth=<N> verb=<verbstr> wid=<W>` |
| Target not in index | `[BRAIN_DISPATCH_CALL_UNKNOWN] from=<caller> target=<key> wid=<W>` |
| Cycle guard fired | `[BRAIN_DISPATCH_CALL_CYCLE] from=<caller> to=<key> depth=<N> wid=<W>` |
| Depth limit reached | `[BRAIN_DISPATCH_CALL_DEPTH] depth=<N> max=8 wid=<W>` |
| Gate-off: Call verb treated as unknown | `[BRAIN_DISPATCH_UNKNOWN] verb=TechSpecial.Call "..." wid=<W>` |

## 1A does NOT apply chained effects

`Brain.CorePower false` in a **called** body (via TechSpecial.Call) traces as `[BRAIN_DISPATCH] depth=N verb=Brain.CorePower false` but does NOT call `setGeneralTacOrder(POWERDOWN)`. The `executeSpecialBody_TraceOnlyChained` function is pure trace + recursion — no order functions called.

The existing 1B POWERDOWN fires only if `Brain.CorePower false` appears in the **root body** (selected by entry-body rule). Chained-effect execution is deferred to CALL-CHAIN-1B.

This is documented in `brain_special_dispatch.cpp` at the `executeSpecialBody_TraceOnlyChained` contract comment and in `executeSpecialBody_Apply` ("CALL-CHAIN-1A NOTE").

## Forbidden-call guard unchanged

Chaining is **pure verb-stream composition**. `executeSpecialBody_TraceOnlyChained` introduces no new order function calls. It calls only `fprintf` + `fflush` + recursive dispatch. The only order function in the whole dispatch TU remains `warrior->setGeneralTacOrder()` in `executeSpecialBody_Apply` for the root-body POWERDOWN verb (1B). Verified by inspection.

## Implementation files

| File | Change |
|------|--------|
| `code/brain_special_dispatch.h` | Added `SpecialIndexEntry`, `SpecialIndex`, `specialIndexFind()`, `executeSpecialBody_TraceOnlyChained()` declaration; updated `executeSpecialBody_TraceOnly` + `executeSpecialBody_Apply` + `parseBrainSpecialBody` signatures with index + callerKey params. |
| `code/brain_special_dispatch.cpp` | `kCallChainMaxDepth=8`; `specialIndexFind()`; `s_dispatchCallGate()`; `parseCallVerbKey()`; `executeSpecialBody_TraceOnlyChained()`; updated `executeSpecialBody_TraceOnly` + `executeSpecialBody_Apply` for Call verb handling; extended `parseBrainSpecialBody_RawScan` to capture `key=`/`type=` fields and populate index; updated `parseBrainSpecialBody` to forward `outIndex`. |
| `code/mech_brain_runtime.h` | Added `SpecialIndex specialIndex` member to `MechBrainRuntime`. |
| `code/mission.cpp` | Loader passes `&brainRuntime->specialIndex` to `parseBrainSpecialBody`. |
| `code/warrior.cpp` | Dispatch seam passes index + callerKey; removed `bodyHasPowerdown` precondition on `executeSpecialBody_Apply` call (now dispatches all loaded bodies). |

## Dispatch hook location

`code/warrior.cpp` lines ~2284–2332 (Enhanced seam). The index is passed at:
- `executeSpecialBody_Apply(...)` call: warrior.cpp ~2300 (APPLY=1 path).
- `executeSpecialBody_TraceOnly(...)` call: warrior.cpp ~2330 (APPLY=0 path).

The `TechSpecial.Call` verb is intercepted inside `executeSpecialBody_TraceOnly` / `executeSpecialBody_Apply` at the verb loop, before the `isRecognizedVerb` check.

## Gate B evidence (CALL=0, TechSpecial.Call → UNKNOWN)

When `MC2_BRAIN_DISPATCH=1` but `MC2_BRAIN_DISPATCH_CALL=0`, the `callGate=false` branch skips the Call resolver. The verb falls through to `isRecognizedVerb()` (returns false → `BRAIN_DISPATCH_UNKNOWN`). Byte-identical to pre-1A behavior for non-Call verbs.

## Gate C verbatim traces (callchain fixture, CALL=1)

For `mission.test.scenario_main` root body calling init then update:

```
[BRAIN_DISPATCH_CALL] from= to=mission.test.special.init depth=1 wid=<W>
[BRAIN_DISPATCH] depth=1 verb=Brain.CorePower false wid=<W>
[BRAIN_DISPATCH] depth=1 verb=Brain.CorePower wid=<W>
[BRAIN_DISPATCH_UNKNOWN] depth=1 verb=Var.Set "init.done" 1 wid=<W>
[BRAIN_DISPATCH_CALL] from= to=mission.test.special.update depth=1 wid=<W>
[BRAIN_DISPATCH] depth=1 verb=OPORD.CoreGuard wid=<W>
[BRAIN_DISPATCH_UNKNOWN] depth=1 verb=Unit.InState "attacking" wid=<W>
[BRAIN_DISPATCH_UNKNOWN] depth=1 verb=UnknownVerb.DoSomething "arg1" wid=<W>
```

Note: `Brain.CorePower false` in the init body is traced at depth=1 but does NOT fire POWERDOWN (1A no-chained-effects rule). The root body (scenario_main) has no `Brain.CorePower false`, so 1B POWERDOWN does not fire at all in this fixture.

## Gate D cycle trace

For cycle A→B→A (cycle.b tries to call cycle.a which is in visited set):
```
[BRAIN_DISPATCH_CALL_CYCLE] from=mission.test.cycle.b to=mission.test.cycle.a depth=2 wid=<W>
```

## Gate E unknown-target trace

```
[BRAIN_DISPATCH_CALL_UNKNOWN] from= target=nonexistent.key wid=<W>
```

## Fixtures (normative — engine-owned per addendum #3)

| Fixture | Purpose |
|---------|---------|
| `tests/fixtures/brain_runtime/mc2_01_specials_callchain.fit` | Depth-2 chain: scenario_main → init + update |
| `tests/fixtures/brain_runtime/mc2_01_specials_callchain_cycle.fit` | Cycle guard: A→B→A |
| `tests/fixtures/brain_runtime/mc2_01_specials_callchain_unknown.fit` | Unknown-target soft-fail |
