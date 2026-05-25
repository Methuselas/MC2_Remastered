# Adversarial Plan Review — RenderWorld Slice M2.5 (mech ObjectID substrate)

**REVIEW STATUS: CONDITIONAL-PASS**

**Executive summary (5 lines):**
1. All 3 execution-time design decisions sanctioned by spec + verified against code: `event=mech_id_summary` is the right new line (existing `event=summary` is in fact gated by `MC2_MECH_BATCHER_STATS` per `gos_mech_batcher.cpp:1329`); atomic T1+T2+T3+T4 commit shape correctly closes the hot-reload-without-relink trap (build+smoke gates fire BEFORE the merged commit at T4 Step 10); `extern "C" mclib_consumeAndResetMlrMechDraws()` resolves cleanly cross-TU because both TUs link into `mc2.exe`.
2. ONE MAJOR finding: T3 Step 5 misclassifies the per-frame reset sites and **misses the third reset at `gos_mech_batcher.cpp:1365`** (the normal end-of-flush() path). Plan only patches lines 348 and 992. Result: `s_gpuMechIdWritesThisFrame` accumulates monotonically in the normal-flow path. Per-mission counter is unaffected (it accumulates explicitly), so the bug is cosmetic for M2.5 deliverables — but the per-frame counter is unused-on-purpose, which is a smell.
3. THREE MINOR findings: plan misnames the line-348 site as "likely `endFrame()`" when it is actually `onMapLoad()` (per-mission); `mclib_` snake-case prefix invented (no existing convention — sibling mclib APIs are `elfHash` lowerCamel or `fst_normalize_key`); `RenderObjectHandle::bits` is in fact **public** (`RenderCore/Handle.h:30`) so the T6 Step 3 fallback prose ("if private, use `make(idx, gen)`") is dead code that should be deleted to remove ambiguity.
4. Citation drift section is exemplary — every load-bearing claim re-grep-verified. The plan's own discovery that `[MECHBATCHER v1] event=summary` is gated is the kind of meta-finding that justifies the citation-drift discipline. Atomic commit shape, full-relink discipline (CMakeFiles delete), smoke command verbatim (`--keep-logs` present), and self-test wire-site (`RenderWorld::init()` after `RunGameplayPickSelfTest()`, NOT per-frame) all check out against `RenderWorld.cpp:361-365`.
5. Recommendation: **fix the MAJOR (add the line-1365 per-frame reset site, or remove the per-frame counter entirely since the per-mission counter is the actual emitted signal)**, accept the 3 MINORs as documentation-only revisions, then execute. No architectural decisions needed from advisor.

---

## CRITICAL findings

**None.**

---

## MAJOR findings

### M1 — T3 Step 5 misses the third per-frame reset site (`gos_mech_batcher.cpp:1365`)

**Plan claim (T3 Step 5, lines 494-501):**
> Expected: two hits -- one at line 348 (function-scope reset, likely `endFrame()`) and one at line 992 (inside `flush()` end-of-function cleanup).

**Code reality (grep `s_eligibleActorsThisFrame\s*=\s*0` against `gos_mech_batcher.cpp`):**
```
187:static uint32_t s_eligibleActorsThisFrame = 0;
348:    s_eligibleActorsThisFrame = 0;     <-- this is inside onMapLoad() (mission-load), NOT endFrame()
992:    s_eligibleActorsThisFrame = 0;     <-- this is the EARLY-OUT path in flush() (line 990-993 guards `g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed || s_pendingSubmits.empty()`)
1365:   s_eligibleActorsThisFrame = 0;     <-- this is the NORMAL end-of-flush reset; the plan misses it
```

There are **THREE** reset sites. The plan enumerates only two. The line-348 site is per-mission, not per-frame (it's inside `onMapLoad()` at `gos_mech_batcher.cpp:338`, body shown 338-357). Line 992 only fires when flush early-outs. The **normal-flow reset is at line 1365**, which the plan does not touch.

**Consequence:** under normal play (gpu mechs eligible, geometry finalized, shader ok, non-empty submits), `s_eligibleActorsThisFrame` is reset at 1365 every frame, but `s_gpuMechIdWritesThisFrame` will accumulate monotonically across all frames in a mission. Per-mission emit reads only `s_gpuMechIdWritesThisMission` (T3 Step 6), so the visible counter is correct. **But** the plan's stated purpose of `s_gpuMechIdWritesThisFrame` is "per-frame writer counter" (comment in plan T3 Step 3), and if that counter is ever read for diagnostics it will be wildly wrong.

**Recommendation (mechanical):** either (a) add the per-frame reset at the line-1365 site (alongside `s_eligibleActorsThisFrame = 0;`), or (b) delete `s_gpuMechIdWritesThisFrame` entirely — `s_gpuMechIdWritesThisMission` is the only counter actually consumed by an emit, so the per-frame variable is dead state. Option (b) is preferred (simpler, removes the bug class).

---

## MINOR findings

### m1 — Misnaming of the line-348 site as "likely `endFrame()`"

**Plan T3 Step 5, line 501:**
> Expected: two hits -- one at line 348 (function-scope reset, **likely `endFrame()`**)...

**Code reality (`gos_mech_batcher.cpp:338-356`):**
```
void GpuMechBatcher::onMapLoad() {
    s_typeLodRecords.clear();
    ...
    s_eligibleActorsThisFrame = 0;     <-- line 348
    ...
}
```

The line-348 reset is in `onMapLoad()` — a per-MISSION hook, not a per-frame hook. The plan compounds this by then prescribing a per-frame counter reset adjacent to it ("// M2.5: per-frame writer counter; mission counter resets in onMapLoad."). Adding `s_gpuMechIdWritesThisFrame = 0;` at this site is harmless (per-mission reset implies per-frame reset is also clear) but the inline comment misleads the reader about what site this is.

**Recommendation:** rename the prose in T3 Step 5 to: "Expected: three hits — line 348 (per-mission reset inside `onMapLoad()`), line 992 (early-out reset in `flush()`), and line 1365 (normal end-of-flush reset). All three should receive the new counter reset, OR drop the per-frame counter as dead state." Tie this to the M1 finding above.

### m2 — `mclib_` snake-case prefix invented; no sibling convention in `mclib/`

**Plan T5 Step 4 (file-structure line 78 + body line 947):**
```cpp
extern "C" uint64_t mclib_consumeAndResetMlrMechDraws() { ... }
```

**Code reality (grep `extern "C"` across `mclib/`):**
```
mclib/fst_hash.cpp:15:  extern "C" unsigned long elfHash(const char* name)
mclib/fst_hash.cpp:28:  extern "C" void fst_normalize_key(char* dst, const char* src)
mclib/fastfile.h:24:    extern "C" DWORD elfHash (const char *name);
mclib/fst_hash.h:26:    extern "C" {
mclib/vfx.h:65:         extern "C" {
```

No `mclib_` prefix convention exists in the repo. Sibling cross-TU C-linkage symbols use either lowerCamel (`elfHash`) or a subsystem prefix (`fst_normalize_key`). The invented `mclib_` prefix is grep-discoverable but inconsistent.

**Recommendation:** rename to `mech_consume_and_reset_mlr_draws()` (snake_case, subsystem prefix, mirrors `fst_normalize_key` shape) OR `mechConsumeAndResetMlrDraws()` (lowerCamel, mirrors `elfHash`). Either is fine; both are more consistent with the file than `mclib_*`. Not load-bearing — this is purely a future-grep-discoverability concern.

### m3 — Dead-code fallback prose in T6 Step 3 (Handle::bits is public)

**Plan T6 Step 3, lines 1196-1212:**
> **Note on `h2.bits` access.** If `RenderObjectHandle::bits` is private, replace the round-trip with the canonical reconstruction API (`RenderObjectHandle::make(idx, gen)` mirrors the production path).

**Code reality (`RenderCore/Handle.h:29-30`):**
```cpp
template <typename Tag>
struct Handle {
    uint32_t bits = 0;  // [19:0] index, [31:20] generation
```

It's a `struct` (default public), and the `bits` field is at top-level. Plan's fallback branch never fires. The wider `make(idx, gen)` path is actually the cleaner one anyway (it's the production constructor), but the conditional framing creates ambiguity at execution time.

**Recommendation:** drop the conditional. Use the `make(idx, gen)` shape unconditionally:
```cpp
RenderCore::RenderObjectHandle h2 = RenderCore::RenderObjectHandle::make(idx, gen);
if (h2.raw() != raw) { ... FAIL ... }
```
Tighter, doesn't depend on direct field access, and matches how every other consumer in `RenderWorld.cpp` constructs handles.

---

## Re-validation of the 3 execution-time design decisions (per dispatch focus)

### 1. `event=mech_id_summary` as a NEW always-on line — VALIDATED

- Spec Q6 amendment 2 explicitly sanctions split-line shape (plan quotes verbatim).
- `event=summary` GATED at `gos_mech_batcher.cpp:1329` (`if (s_mechBatcherTrace)` enclosing 1336-1348). Confirmed by grep — `s_mechBatcherTrace` is `(getenv("MC2_MECH_BATCHER_STATS") != nullptr)` at line 967.
- `onMapUnload()` is the right hook: it already fires per-mission (grep confirms `event=map_unload` at line 377), and the function body (lines 359-378) is GL-resource teardown that runs once per mission. Emitting the counter at the TOP (before the GL teardown) ensures the counter is read before any subsequent state mutation.
- New line uses tag `[MECHBATCHER v1]` (same banner) + DIFFERENT `event=` key. Schema-versioning is by banner, not by event, so the new event key shares `v1`. **No conflict.**

### 2. Atomic T1+T2+T3+T4 commit — VALIDATED

- Plan T1 Step 4, T2 Step 4, T3 Step 7 all explicitly say "STAGE -- do NOT build/commit yet."
- T4 Step 7 deletes `mc2.exe` AND `CMakeFiles` (per `memory/feedback_class_layout_change_needs_clean_first.md`).
- T4 Step 8 (env-OFF) + Step 9 (env-ON) run smoke gates BEFORE Step 10 commits. So validation IS atomic with the merged stage.
- Commit message (T4 Step 10) is reviewable in one pass (~5 files: gos_mech_batcher.{h,cpp}, mech.{vert,frag}, mech3d.cpp). Five-file diff is large but within reviewable scope for a single conceptual change (struct grow + lockstep shader edit).
- **Trap closed:** because struct C++/GLSL grow lands in the same atomic commit + cmake clean before build, there is no window where the running binary has stale 48B stride while shader expects 64B.

### 3. `extern "C" mclib_consumeAndResetMlrMechDraws()` cross-TU counter — VALIDATED (with naming nit m2 above)

- Defined in `mclib/mech3d.cpp` (T5 Step 2/4); declared via `extern "C" ... ;` forward decl in `GameOS/gameos/gos_mech_batcher.cpp` (T5 Step 4). Both link into the same `mc2.exe`. No circular dependency: `mclib` is depended on by GameOS already (`mclib/mech3d.h` is included from `GameOS/gameos/gos_mech_batcher.h:11`), and the cross-call goes GameOS→mclib (the existing direction).
- Atomicity: the counter is single-thread (game logic), reset is per-mission single-thread (during teardown, after frame draws complete). Cross-frame increment race not possible. No lock needed. Plan correctly notes the per-mission lifecycle in T5 Step 2.
- **Naming convention is the only nit** — see Minor m2.

---

## Standard adversarial focus (per dispatch items 4-9)

### 4. Citation drift re-verified — PASS

Spot-checked load-bearing citations:
- `gos_mech_batcher.h:11 #include "mech3d.h"` — confirmed (line 11).
- `mech.frag:77` total line count — confirmed (`wc -l shaders/mech.frag` = 77).
- `gos_mech_batcher.cpp:1329` `s_mechBatcherTrace` gate around summary — confirmed (`if (s_mechBatcherTrace)` at 1329, summary at 1336-1340).
- `mech3d.cpp:2549,2586` submit-desc + submitActor — confirmed exactly.
- `mech3d.cpp:2608` `mechShape->Render(true)` — confirmed exactly.
- `mech3d.h:478,487-489` mechRenderHandle + getRenderWorldHandle accessor — confirmed (478 declaration, 487-489 accessor body).
- `RenderCore/Handle.h:30` `uint32_t bits` (public) — confirmed.
- `RenderWorld/RenderWorld.cpp:40,250,361,365` self-test forward decls + wire — confirmed all four.
- `gos_static_prop_batcher.cpp:510-521` GLSL prefix injection — confirmed exact shape.

### 5. `Existing:` / `Replace with:` spot-check — PASS

- T1 Step 2 `Existing:` (header lines 33-51) — **MATCHES** the actual file byte-for-byte.
- T3 Step 2 `Existing:` (mech3d.cpp lines 2582-2586) — **MATCHES**.
- T4 Step 4 `Existing:` (mech.frag lines 36-37) — **MATCHES**.
- T4 Step 5 `Existing:` (mech.frag lines 74-77) — **MATCHES**.

No line-drift between plan-write and execution-time observed in the spot-check sample.

### 6. Build/relink discipline — PASS

T4 Step 7 explicitly: `Remove-Item ... mc2.exe`, then `Remove-Item ... CMakeFiles -Recurse -Force`, then `cmake --build ... --config RelWithDebInfo`. Matches CLAUDE.md rule "Full relink before deploy when load-bearing functions change" + memory `feedback_class_layout_change_needs_clean_first.md`. T5 Step 5 also forces relink (good — covers the cross-TU `extern "C"` link).

### 7. Smoke command verbatim — PASS

Plan T7 Gate 1 Step 1 / Gate 2 Step 2 use exactly:
```
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```
Matches CLAUDE.md "Canonical invocation" section byte-for-byte. `--keep-logs` is present. No exotic flags introduced.

### 8. Self-test wire site — PASS

T6 Step 4 wires `RunMechObjectIdSelfTest()` into `RenderWorld::init()` immediately after `RunGameplayPickSelfTest()` (current call at `RenderWorld.cpp:365`). `init()` fires per-mission (not per-frame), so the self-test runs once per mission load — no per-frame log spam. Pattern mirrors the existing M2-pre wiring exactly.

### 9. T7 Gate 5 user-driven canary — PARTIAL CONCERN (not a finding)

T7 Gate 5 / Step 6 invokes `run_smoke.py --missions mc2_03 --duration 60`. Per CLAUDE.md "Smoke sessions are USER-DRIVEN", the user has visual observation through the live game window for the 60s window — that satisfies user-observability. **However**, Gate 5's user-action prose says:

> User actions during the 60s window:
> 1. Shift+click on a static prop. Expected: `[STATIC_PROP_PICK v1] hit ...` log line.
> 2. Shift+click on a mech. Expected: NO `[STATIC_PROP_PICK v1] hit ...` line.

This **does** ask the user to perform specific actions, but does NOT trigger the anti-pattern ("re-run with X env var") — it's first-person observation in the live smoke session. Acceptable per CLAUDE.md. Note: post-run inspection is done by the agent against the artifact log, not the user (correct).

---

## Architectural decisions that need user/advisor sign-off before revision pass

**None.** The M1 finding is mechanical (add a third reset site or drop the per-frame counter). All 3 MINOR findings are documentation/cosmetic. No architectural call needs human judgment.

---

## Verdict

**CONDITIONAL-PASS.** Land the M1 fix (per-frame reset at line 1365 or drop `s_gpuMechIdWritesThisFrame`); revise the 3 MINOR documentation items; then execute. The plan's atomic-commit shape, full-relink discipline, smoke gates, and lockstep struct+shader edit are sound. The 3 execution-time design decisions are all validated against code.

The citation-drift section is the strongest part of the plan — the discovery that `event=summary` was gated and the consequent pivot to a new `event=mech_id_summary` line is exactly the kind of plan-time recon work that prevents an executor session from shipping an always-on counter onto a silently-gated line.
