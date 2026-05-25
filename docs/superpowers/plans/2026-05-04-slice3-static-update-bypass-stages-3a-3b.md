# Slice 3: Static-Object Update Bypass — Stages 3.A + 3.B Implementation Plan (v2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Skip the 1.55ms `appearance->update()` call inside `TerrainObject::update()` for objects whose appearance reports it has no per-frame work to do, starting with trees only.

**Architecture:** Add `virtual bool IsStaticNow() const` to the `Appearance` base class (default `false`). `TreeAppearance` overrides to return `true` **unconditionally** — trees-as-a-class have no appearance-internal dynamism. Owning-object dynamism (currently-falling) is composed at the call site in `code/terrobj.cpp` via `appearance->IsStaticNow() && !getFlag(OBJECT_FLAG_FALLING)`. Skip gated behind `MC2_STATIC_UPDATE_SKIP=1` (default-off in 3.B). Stage 3.A lands counter/Tracy/banner instrumentation with no behavior change; Stage 3.B adds the predicate + override + skip path.

**Tech Stack:** C++ (MSVC, RelWithDebInfo, MC2 engine), Tracy profiler, env-gated `[STATIC_UPDATE v1]` printf instrumentation matching the canonical `MC2_DESTROY_TRACE` / `MC2_TGL_POOL_TRACE` pattern. No new dependencies.

**Brainstorm/Recon:**
- Brainstorm: [docs/superpowers/brainstorms/2026-05-04-slice3-static-update-bypass-brainstorm.md](../brainstorms/2026-05-04-slice3-static-update-bypass-brainstorm.md)
- Recon: [docs/superpowers/explorations/2026-05-04-slice3-static-update-bypass-recon.md](../explorations/2026-05-04-slice3-static-update-bypass-recon.md)
- v1 plan + adversarial review (rejected): see git log for `2026-05-04-slice3-static-update-bypass-stages-3a-3b.md` v1 commit; review verdict was REJECTED on 3 CRITICAL findings — all folded into v2 below.

**v1 → v2 corrections (load-bearing — DO NOT regress):**
- **C1 (compile error):** v1 Task 6 Step 4 placeholder `return (pitchAngle == 0.0f);` would not compile. `TreeAppearance` has no `pitchAngle` or `fallRate` member. v2: `TreeAppearance::IsStaticNow()` returns `true` unconditionally; the falling-state check is composed at the call site only.
- **C2 (semantic bug):** v1 + brainstorm Q3 wrongly claimed `OBJECT_FLAG_FALLING` is only set inside `TerrainObject::update()`. Reality: it is set externally from collision callbacks at `code/terrobj.cpp:286-287` and `code/bldng.cpp:1453-1455`. v2 makes the call-site composition (`!getFlag(OBJECT_FLAG_FALLING)`) the **mandatory** dynamism check, not a "fallback." Without it, the impact frame is silently skipped.
- **C3 (stale citation):** v1 said `g_mc2FrameCounter` is "defined in GameOS/gameos/gameosmain.cpp:26." Actual definition is at `mclib/tgl.cpp:3718`. The misattribution was inherited from a stale comment in `code/gameobj.cpp:100-102` (which this plan also fixes). Per `memory/feedback_inherited_citations_must_regrep.md` — never copy a citation without re-grep'ing.

**Punts resolved at plan-write time (re-verified for v2):**
- `IsStaticNow()` lives on `Appearance` (`mclib/appear.h`, immediately after `virtual long update (bool animate = true)` at ~line 124). Confirmed: `code/terrobj.cpp:608` dispatches via `AppearancePtr` = `Appearance*` (`mclib/dappear.h:23`).
- `GateAppearance` is **not a class** — gates instantiate `BldgAppearance` (`code/gate.cpp:699`). Stage 3.B unaffected (`BldgAppearance` inherits the default `false`).
- GOSFX effect-pointer fields for the future Stage 3.D building disqualifier: `BldgAppearance::destructFX`, `::activity`, `::activity1` (`mclib/bdactor.h:220-222`); `TerrainObject::bldgDustPoofEffect` (`code/terrobj.h:149`). `TreeAppearance` has none — Stage 3.B safe.
- `g_mc2FrameCounter` is defined at `mclib/tgl.cpp:3718` (NOT `gameosmain.cpp`). Existing externs at `code/gameobj.cpp:102` and `GameOS/gameos/gos_crashbundle.cpp:47` resolve to that def; this plan adds another extern in `code/terrobj.cpp` and one in the new tiny header.
- Default smoke duration is 120s per `scripts/run_smoke.py:217` (each tier1 mission), so each mission produces ~7200 frames @ 60fps and the 600-frame summary fires ~12 times per mission. v1's "8s = ~480 frames" claim was wrong.

**Stage 3.A → 3.B decision gate (load-bearing):** After Stage 3.A lands and produces real baseline numbers, **stop and evaluate before starting 3.B**. The decision input is the `[STATIC_UPDATE v1]` summary line from a tier1 forest mission (mc2_01) running for ~10 seconds:
- `seen` = total visible TerrainObject updates per frame interval — establishes the population the slice operates on.
- (3.A doesn't have a tree-only counter, but `seen` × estimated tree fraction ≈ skip volume.)

Decision rule: if estimated tree fraction × `seen` is below ~30% of total appearanceUpdate budget on mc2_01 (i.e., the skip volume is too small to justify the predicate dispatch overhead), **3.B is not worth doing** — close the slice as 3.A-only (instrumentation is still valuable as observability for future work). If it's above 30%, proceed to 3.B.

**Out of scope:** Stages 3.C (walls/fences/bridges), 3.D (BldgAppearance), 3.E (perf gate + default-on flip). Those become a separate plan once 3.A/B land and soak.

**Architectural contract (load-bearing for Stage 3.D — keep loud):**
> `Appearance::IsStaticNow()` is **appearance-internal only**. It must NOT inspect the owning `GameObject` / `TerrainObject` (the appearance has no back-pointer). Owner / game-object transient state — `OBJECT_FLAG_FALLING`, damage level, power-supply state, scripted flags — must be checked at the **call site** in `code/terrobj.cpp` and composed with `appearance->IsStaticNow()`. Stage 3.D will extend the call-site composition for buildings; do not be tempted to give appearances a back-pointer "for convenience."

---

## File structure

| File | Stage | Responsibility |
|---|---|---|
| `code/static_update_counters.h` (NEW) | 3.A | Tiny header: declares `g_staticUpdateRunCount()` and `g_staticUpdateSkipCount()` accessors used by Tracy plots. ~12 lines. |
| `code/terrobj.cpp` | 3.A, 3.B | File-private counters + env flags + skip gate at the `appearance->update()` call site. Defines the accessor functions. |
| `code/objmgr.cpp` | 3.A | One-per-frame summary emitter + Tracy plot calls, placed at end of TerrainObjects update block (alongside existing TracyPlots at line ~1786-1789). |
| `code/gameobj.cpp` | 3.A | Fix the stale `g_mc2FrameCounter` comment at line 100-102 (drive-by; tracks the same fix-as-found discipline). |
| `mclib/appear.h` | 3.B | Add `virtual bool IsStaticNow() const` to `Appearance`, default `return false;`. |
| `mclib/bdactor.h` | 3.B | Declare `IsStaticNow() const override` on `TreeAppearance`. |
| `mclib/bdactor.cpp` | 3.B | Define `TreeAppearance::IsStaticNow() { return true; }`. |
| `GameOS/gameos/gameosmain.cpp` | 3.A | Extend `[INSTR v1]` startup banner with `static_update_trace=N static_update_skip=N` fields. |

One new TU (the tiny header). All other files are existing.

---

## Pre-flight (must run before any task starts)

- [ ] **P1: Confirm name uniqueness — no symbol collision on `IsStaticNow`**

Run: `grep -rn "IsStaticNow" mclib/ code/ shaders/ GameOS/`

Expected: zero hits. If non-zero, the name collides — STOP and rename in the plan before proceeding.

- [ ] **P2: Enumerate all `Appearance` subclasses to confirm default-false coverage**

Run: `grep -rn "public Appearance\b\|public ObjectAppearance\b" mclib/ code/ | grep -v "//" | sort -u`

Expected: at minimum `BldgAppearance`, `TreeAppearance`, `GVAppearance`, `GenericAppearance`, `Mech3DAppearance`, `ObjectAppearance` itself. Confirm none of these has a member named `IsStaticNow` already (the P1 grep covers that). Default-false on the base means all of these inherit safe behavior in Stage 3.B.

- [ ] **P3: Audit `TreeAppearance::update()` return values**

Run: `sed -n '4295,4440p' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/bdactor.cpp` (the `// class TreeAppearance` section, `update()` body)

Inspect every `return` statement. Expected: all return `NO_ERR` (or equivalent success). If any return path signals a destroy / lifecycle event, the skip path **swallows it silently** — STOP and re-architect to preserve the return value (e.g., assign `appearance->update()` to a local and propagate).

- [ ] **P4: Confirm `g_mc2FrameCounter` definition site**

Run: `grep -rn "^uint32_t g_mc2FrameCounter\|^uint32_t\s*g_mc2FrameCounter" mclib/ code/ GameOS/`

Expected: exactly one line, `mclib/tgl.cpp:3718`. If it has moved, update Task 1 Step 2's comment and Task 9 (gameobj.cpp comment fix) accordingly.

---

## Stage 3.A — Instrumentation only (no behavior change)

### Task 1: Add file-private counters and env flags to terrobj.cpp

**Files:**
- Modify: `code/terrobj.cpp` — add a counters block after existing includes.

- [ ] **Step 1: Locate the include block**

Run: `grep -n "^#include" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/terrobj.cpp | head -10`

Identify the last `#include` line before code begins. Insertion point is the first blank line after that.

- [ ] **Step 2: Insert the counters block**

```cpp
//---------------------------------------------------------------------------
// Slice 3 Static-Update Bypass instrumentation (worktree CLAUDE.md
// §"Tier-1 Instrumentation Env Vars" pattern; mirrors MC2_DESTROY_TRACE in
// gameobj.cpp:104). Counters defined here because the gate lives in this TU;
// objmgr.cpp reads them via the accessors declared in static_update_counters.h.
//---------------------------------------------------------------------------
#include "static_update_counters.h"

extern uint32_t g_mc2FrameCounter;  // defined in mclib/tgl.cpp:3718

// Env bool parser: unset/"0"/"false"/"off" disable; "1"/"true"/"on" enable.
// Any other non-empty value enables (lenient default — better to over-trace
// on a typo than silently no-op). Matches the user-stated convention
// 2026-05-04 — do NOT regress to `getenv(...) != nullptr`, which would treat
// MC2_STATIC_UPDATE_SKIP=0 as ENABLED (the GPU_OBJECTS class of bug).
static bool ParseEnvBool(const char* name) {
    const char* v = getenv(name);
    if (!v || !*v) return false;
    if (v[0]=='0' && !v[1]) return false;
    if (!_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no")) return false;
    return true;
}

static const bool s_staticUpdateTrace = ParseEnvBool("MC2_STATIC_UPDATE_TRACE");
static const bool s_staticUpdateSkip  = ParseEnvBool("MC2_STATIC_UPDATE_SKIP");

namespace {
struct StaticUpdateCounters {
    uint32_t objects_seen;       // TerrainObject::update() entries with appearance && inView
    uint32_t updates_run;        // appearance->update() actually called
    uint32_t updates_skipped;    // appearance->update() short-circuited by IsStaticNow()
    uint32_t dyn_falling;        // appearance class said static, but OBJECT_FLAG_FALLING set
    uint32_t dyn_other;          // reserved for Stage 3.D building disqualifiers
};
StaticUpdateCounters g_staticUpdateCounters = {};
StaticUpdateCounters g_staticUpdateLastSummary = {};
uint32_t g_staticUpdateLastSummaryFrame = 0;
}  // namespace

// External accessors (declared in code/static_update_counters.h, called from
// code/objmgr.cpp for Tracy plots and the once-per-frame summary).
uint32_t g_staticUpdateRunCount()      { return g_staticUpdateCounters.updates_run; }
uint32_t g_staticUpdateSkipCount()     { return g_staticUpdateCounters.updates_skipped; }
uint32_t g_staticUpdateSeenCount()     { return g_staticUpdateCounters.objects_seen; }
uint32_t g_staticUpdateFallingCount()  { return g_staticUpdateCounters.dyn_falling; }
```

- [ ] **Step 3: Create the tiny header**

Create `code/static_update_counters.h`:

```cpp
#pragma once
#include <stdint.h>

// Slice 3 static-update counter accessors. Definitions live in code/terrobj.cpp.
// objmgr.cpp uses these to emit the once-per-frame [STATIC_UPDATE v1] summary
// and TracyPlot calls without taking a hard dependency on terrobj.cpp internals.
//
// All summary/emitter declarations live HERE — never re-declare these as
// `extern` inside a function body in a consuming TU. The header IS the
// contract (per v2 review feedback).
uint32_t g_staticUpdateRunCount();
uint32_t g_staticUpdateSkipCount();
uint32_t g_staticUpdateSeenCount();
uint32_t g_staticUpdateFallingCount();

// Summary emission (called from the per-frame hook in code/objmgr.cpp at the
// end of the TerrainObjects update sweep). State for the "last summary frame"
// guard lives in code/terrobj.cpp alongside the counters.
uint32_t g_staticUpdateLastSummaryFrame_get();
void     g_staticUpdateEmitSummary(uint32_t frame);
```

- [ ] **Step 4: Build**

Run the worktree build per CLAUDE.md "Build" rule (`--config RelWithDebInfo`). The `superpowers:executing-plans` skill should invoke `/mc2-build`.

Expected: clean build. If MSVC warns about unused `s_staticUpdateSkip`, prefix the static decl with `[[maybe_unused]]` (it's wired in Task 7).

- [ ] **Step 5: Commit**

```bash
git add code/terrobj.cpp code/static_update_counters.h
git commit -m "feat(slice3-3a): add static-update counter struct + env flags + accessor header

Counters defined; gate not yet wired (Task 2). Mirrors MC2_DESTROY_TRACE
pattern (gameobj.cpp:104). Tiny header isolates objmgr.cpp from terrobj.cpp
internals — same shape as the existing extern-uint32_t pattern but with a
proper declaration site."
```

---

### Task 2: Wire counter increments into TerrainObject::update() — no skip yet

**Files:**
- Modify: `code/terrobj.cpp:603-609` (the `if (inView)` block around `appearance->update()`).

- [ ] **Step 1: Read the exact current block**

Run: `sed -n '595,615p' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/terrobj.cpp`

Confirm the block ends with the `appearanceUpdate` ZoneScopedN and `appearance->update();`. Copy verbatim before editing.

- [ ] **Step 2: Edit to add counter increments (no skip)**

Find:
```cpp
		if (inView)
		{
			windowsVisible = turn;
			{
				ZoneScopedN("TerrainObject::update appearanceUpdate");
				appearance->update();
			}
```

Replace with:
```cpp
		if (inView)
		{
			windowsVisible = turn;
			{
				ZoneScopedN("TerrainObject::update appearanceUpdate");
				++g_staticUpdateCounters.objects_seen;
				// Stage 3.A: count only — no skip. Stage 3.B (Task 7) adds the gate.
				++g_staticUpdateCounters.updates_run;
				appearance->update();
			}
```

- [ ] **Step 3: Build + smoke**

Build, then:
```
py -3 .claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: exit 0. Tracy zone time on `GameLogic.Units.TerrainObjects` unchanged within ±1%. No new destroys. The `recalcBounds` and dust-effect blocks at `terrobj.cpp:599-601` and `:611-635` are untouched — confirm by re-reading the diff.

- [ ] **Step 4: Commit**

```bash
git add code/terrobj.cpp
git commit -m "feat(slice3-3a): increment objects_seen + updates_run counters

Stage 3.A counter wiring; updates_skipped stays 0 (no gate yet).
Verifies counter linkage against tier1 smoke before any path change."
```

---

### Task 3: Add once-per-frame summary emitter + Tracy plots in objmgr.cpp

**Background:** v1 placed the summary trigger inside `TerrainObject::update()`, which (per adversarial review M3) has three problems: starvation if no TerrainObject runs that frame, per-object firing on the trigger frame (only the first emits but the guard write is non-atomic), and `puts/fflush` from inside the hot path. v2 places the trigger in `objmgr.cpp` at the end of the TerrainObjects update block, alongside the existing `TracyPlot` calls — guaranteed to run exactly once per frame.

**Files:**
- Modify: `code/objmgr.cpp:1786-1799` (the existing TracyPlot block at the end of the TerrainObjects update sweep).

- [ ] **Step 1: Read the existing TracyPlot block**

Run: `sed -n '1780,1800p' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/objmgr.cpp`

Identify the block of `TracyPlot("TerrainObjects ...", ...)` lines around 1786-1789. Insertion point is immediately after the last existing TracyPlot in that block.

- [ ] **Step 2: Add include at top of objmgr.cpp**

Find the existing `#include` block in `code/objmgr.cpp`. Add `#include "static_update_counters.h"` alongside the other `code/` includes.

- [ ] **Step 3: Add the summary helper + plot calls**

Locate the end of the TracyPlot block from Step 1 (the line after `TracyPlot("TerrainObjects visible objects updated", int64_t(terrainObjectsUpdated));` or its actual neighbor). Insert immediately after:

```cpp
		// Slice 3 static-update visibility (always-on, paired with the
		// [STATIC_UPDATE v1] line below).
		TracyPlot("TerrainObjects dynamic updates", int64_t(g_staticUpdateRunCount()));
		TracyPlot("TerrainObjects static skipped",  int64_t(g_staticUpdateSkipCount()));

		// Once-per-600-frame summary line. Always-on (matches [TGL_POOL v1]
		// summary cadence — emits regardless of MC2_STATIC_UPDATE_TRACE so a
		// fresh operator sees baseline rates with no setup). This site runs
		// exactly once per frame at the end of the TerrainObjects update sweep.
		// g_mc2FrameCounter is the project-wide free extern (declared in
		// many TUs). The static-update accessors come from
		// code/static_update_counters.h (added at the top of this file in
		// Step 2 below — re-confirm before this hunk lands).
		extern uint32_t g_mc2FrameCounter;
		const uint32_t curFrame = g_mc2FrameCounter;
		if (curFrame > 0 && (curFrame % 600) == 0 &&
		    curFrame != g_staticUpdateLastSummaryFrame_get()) {
			g_staticUpdateEmitSummary(curFrame);
		}
```

- [ ] **Step 4: Add the matching emitter + accessor in terrobj.cpp**

In `code/terrobj.cpp`, add after the existing accessor functions from Task 1 Step 2:

```cpp
uint32_t g_staticUpdateLastSummaryFrame_get() { return g_staticUpdateLastSummaryFrame; }

void g_staticUpdateEmitSummary(uint32_t frame) {
    const StaticUpdateCounters& cur  = g_staticUpdateCounters;
    const StaticUpdateCounters& prev = g_staticUpdateLastSummary;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "[STATIC_UPDATE v1] frame=%u seen=%u run=%u skip=%u "
        "dyn_falling=%u dyn_other=%u "
        "delta_seen=%u delta_run=%u delta_skip=%u",
        frame,
        cur.objects_seen, cur.updates_run, cur.updates_skipped,
        cur.dyn_falling, cur.dyn_other,
        cur.objects_seen   - prev.objects_seen,
        cur.updates_run    - prev.updates_run,
        cur.updates_skipped - prev.updates_skipped);
    puts(buf);
    fflush(stdout);
    g_staticUpdateLastSummary = cur;
    g_staticUpdateLastSummaryFrame = frame;
}
```

Optionally add these two declarations to `code/static_update_counters.h` as well (cleaner than `extern` at call site). Either approach works; the `extern` form matches what objmgr.cpp already does for `g_mc2FrameCounter`.

- [ ] **Step 5: Build + smoke**

Build, then run smoke as in Task 2 Step 3. After completion, verify the summary line appears:

```bash
grep "STATIC_UPDATE v1" tests/smoke/artifacts/<latest-timestamp>/*/stdout.log | head -10
```

Expected: each tier1 mission runs ~120s ≈ 7200 frames @ 60fps, so ~12 summary lines per mission. Lines look like:
```
[STATIC_UPDATE v1] frame=600 seen=N run=N skip=0 dyn_falling=0 dyn_other=0 delta_seen=N delta_run=N delta_skip=0
```

`skip=0` and `dyn_falling=0` are the Stage 3.A correctness checks (no skip path exists yet, no falling-counter increments yet).

- [ ] **Step 6: Commit**

```bash
git add code/objmgr.cpp code/terrobj.cpp code/static_update_counters.h
git commit -m "feat(slice3-3a): emit [STATIC_UPDATE v1] summary every 600f + Tracy plots

Trigger placed at end of objmgr.cpp's TerrainObjects update block — runs
exactly once per frame regardless of TerrainObject population. Avoids
v1 reviewer's M3 starvation/race/hot-path-IO hazards (would have fired
from inside TerrainObject::update())."
```

---

### Task 4: Extend [INSTR v1] startup banner with static_update flags

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` — env-flag locals + snprintf format string + arg list.

- [ ] **Step 1: Locate the banner block**

Run: `sed -n '660,715p' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gameosmain.cpp`

Confirm the `const bool xxx = (getenv("MC2_XXX") != nullptr)` lines feed the `snprintf` building the `[INSTR v1] enabled:` line. Note the buffer size at line 701 (`char _cbbuf[640]`).

- [ ] **Step 2: Add the two env-flag locals**

Insert immediately after `const bool vpPar   = (getenv("MC2_VERTEX_PROJECT_PARITY") != nullptr);` (around line 670). **Use the same `ParseEnvBool` semantics as `code/terrobj.cpp` (Task 1) — NOT the `getenv(...) != nullptr` shortcut. Otherwise the banner will report `static_update_skip=1` for `MC2_STATIC_UPDATE_SKIP=0` while the actual gate (which uses `ParseEnvBool`) reports the opposite. The banner and the gate must agree.**

Local helper (file-private, mirrors the one in terrobj.cpp — duplicated rather than headerized because it's two-line code and gameosmain.cpp does not currently #include any code/ header):

```cpp
        auto suParseBool = [](const char* name) -> bool {
            const char* v = getenv(name);
            if (!v || !*v) return false;
            if (v[0]=='0' && !v[1]) return false;
            if (!_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no")) return false;
            return true;
        };
        const bool suTrace = suParseBool("MC2_STATIC_UPDATE_TRACE");
        const bool suSkip  = suParseBool("MC2_STATIC_UPDATE_SKIP");
```

- [ ] **Step 3: Extend the format string + args**

In the existing snprintf, append `static_update_trace=%d static_update_skip=%d ` immediately before `build=%s` in the format string, and add `suTrace?1:0, suSkip?1:0,` to the argument list immediately before `build`. Buffer math: the existing `[640]` has comfortable headroom (current worst-case ≈ 360 chars; the new fields add ~36 + 2 ints).

- [ ] **Step 4: Build + smoke**

Build, then run smoke + grep:

```bash
grep "INSTR v1" tests/smoke/artifacts/<latest-timestamp>/*/stdout.log | head -3
```

Expected: each banner ends with `... static_update_trace=0 static_update_skip=0 build=<hash>`.

Then verify env-on flips the field:
```bash
MC2_STATIC_UPDATE_TRACE=1 py -3 scripts/run_smoke.py --tier tier1 --duration 8 --kill-existing
grep "INSTR v1" tests/smoke/artifacts/<latest-timestamp>/*/stdout.log
```

Expected: `... static_update_trace=1 static_update_skip=0 ...`.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(slice3-3a): add static_update_trace + static_update_skip to [INSTR v1] banner

Operator can confirm at startup whether slice 3 env flags are set."
```

---

### Task 9 (drive-by, Stage 3.A): Fix stale `g_mc2FrameCounter` comment in gameobj.cpp

**Background:** Per adversarial review C3, `code/gameobj.cpp:100-102` has a comment claiming `g_mc2FrameCounter` is "Defined at file scope in GameOS/gameos/gameosmain.cpp:26 (Commit 1)". The actual definition is at `mclib/tgl.cpp:3718`. This stale comment caused v1 of this plan to inherit the wrong citation. Fix as found.

**Files:**
- Modify: `code/gameobj.cpp:100-102`.

- [ ] **Step 1: Read the current comment**

Run: `sed -n '98,105p' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/gameobj.cpp`

- [ ] **Step 2: Edit the comment**

Find:
```cpp
//---------------------------------------------------------------------------
// Tier-1 instrumentation (stability spec §3.1-3.4)
//---------------------------------------------------------------------------
// Defined at file scope in GameOS/gameos/gameosmain.cpp:26 (Commit 1).
extern uint32_t g_mc2FrameCounter;
```

Replace with:
```cpp
//---------------------------------------------------------------------------
// Tier-1 instrumentation (stability spec §3.1-3.4)
//---------------------------------------------------------------------------
// Defined at file scope in mclib/tgl.cpp:3718.
extern uint32_t g_mc2FrameCounter;
```

- [ ] **Step 3: Build (no behavior change, but link must still pass)**

Build clean.

- [ ] **Step 4: Commit**

```bash
git add code/gameobj.cpp
git commit -m "chore(slice3): fix stale g_mc2FrameCounter location comment

Was 'GameOS/gameos/gameosmain.cpp:26', actual def is 'mclib/tgl.cpp:3718'.
The stale citation in this comment was inherited by slice 3 plan v1 and
caught by adversarial review (C3). Fix-as-found per worktree CLAUDE.md
documentation discipline."
```

---

### Stage 3.A gate (must pass before starting Stage 3.B):
- Build clean RelWithDebInfo, no new warnings.
- Default-env smoke (tier1 + menu canary): exit 0, +0 destroys delta vs baseline.
- `[STATIC_UPDATE v1]` summary lines appear ~12× per tier1 mission with `skip=0` and `dyn_falling=0`.
- `[INSTR v1]` startup banner shows the two new fields.
- Tracy plot "TerrainObjects dynamic updates" populates with non-zero values; "TerrainObjects static skipped" stays 0.

If any gate fails, stop and root-cause before proceeding.

---

## Stage 3.B — `IsStaticNow()` predicate + tree-only override + env-gated skip

### Task 5: Add `IsStaticNow()` virtual to Appearance base

**Files:**
- Modify: `mclib/appear.h` — declare `virtual bool IsStaticNow (void) const` immediately after the existing `update()` virtual at ~line 124.

- [ ] **Step 1: Re-confirm pre-flight P1 grep result**

Run: `grep -rn "IsStaticNow" mclib/ code/ shaders/ GameOS/`

Expected: zero hits. If non-zero (e.g., a sibling worktree branch was merged in), STOP — name collision means rename.

- [ ] **Step 2: Read the Appearance class around the update virtual**

Run: `sed -n '120,135p' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/appear.h`

Confirm `virtual long update (bool animate = true)` at line ≈124.

- [ ] **Step 3: Insert the new virtual**

Find:
```cpp
		virtual long update (bool animate = true)
		{
			//Perform any frame by frame tasks.  Animations, etc.
			return NO_ERR;
		}
```

Insert directly after the closing brace:
```cpp

		// Slice 3 (static-update bypass): return true ONLY when the appearance
		// has no per-frame work this frame. Default false = always run update().
		// Override per class. The CALL SITE in code/terrobj.cpp:603-615 also
		// checks owning-object dynamism (e.g., OBJECT_FLAG_FALLING) — the
		// appearance does not have a back-pointer to its owner, so per-instance
		// dynamism rooted in the owning GameObject must be composed there.
		// Called once per frame per visible terrain object — keep it cheap.
		virtual bool IsStaticNow (void) const
		{
			return false;
		}
```

- [ ] **Step 4: Build**

Expected: clean build. Adding a new virtual with a default body doesn't break ABI here (no inheriting class overrides it yet).

- [ ] **Step 5: Commit**

```bash
git add mclib/appear.h
git commit -m "feat(slice3-3b): add Appearance::IsStaticNow() virtual, default false

Predicate for slice 3 static-update bypass. Default false = no behavior change.
Comment documents the call-site composition rule (owner-dynamism not visible
to the appearance — caller must check OBJECT_FLAG_FALLING etc. itself)."
```

---

### Task 6: Override `IsStaticNow()` on TreeAppearance — unconditional `return true`

**Background:** Per adversarial review C1, v1's attempt to read `pitchAngle` or `fallRate` inside the override does not compile — `TreeAppearance` has no such member. Per C2, the `OBJECT_FLAG_FALLING` flag is set externally from collision callbacks (`code/terrobj.cpp:286-287` and `code/bldng.cpp:1453-1455`), so the falling check must be at the call site regardless. Conclusion: `TreeAppearance::IsStaticNow()` returns `true` unconditionally, and Task 7 composes the falling check at the call site.

**Files:**
- Modify: `mclib/bdactor.h` — declare override on `TreeAppearance`.
- Modify: `mclib/bdactor.cpp` — define unconditional body.

- [ ] **Step 1: Read TreeAppearance class declaration around the existing virtuals**

Run: `sed -n '465,545p' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/bdactor.h`

Identify the existing `update` declaration (look for `virtual long update`).

- [ ] **Step 2: Add the override declaration**

Insert immediately after the existing `update()` declaration on `TreeAppearance`:

```cpp
		virtual bool IsStaticNow (void) const override;
```

If neighboring overrides omit the `virtual` keyword (some MC2 headers do), match local style: `bool IsStaticNow (void) const override;`.

- [ ] **Step 3: Define the body in bdactor.cpp**

In the `// class TreeAppearance` section starting at `mclib/bdactor.cpp:3389`, add:

```cpp
//----------------------------------------------------------------------------
// Slice 3 (static-update bypass) predicate.
//
// TreeAppearance has no appearance-internal dynamism: no per-frame animation
// state, no destructibility hook on the appearance itself, no scripted
// callback. The only dynamism that affects a tree is OBJECT_FLAG_FALLING on
// the owning TerrainObject — set EXTERNALLY by the collision callback at
// code/terrobj.cpp:286-287. The appearance has no back-pointer to its owner,
// so the falling check is composed at the call site in TerrainObject::update()
// (see code/terrobj.cpp around line 603-615). This function therefore returns
// true unconditionally; the call-site predicate composition is mandatory and
// the contract is asserted by Stage 3.B's pre-condition: TreeAppearance::update
// returns NO_ERR on every code path (verified pre-flight P3).
//----------------------------------------------------------------------------
bool TreeAppearance::IsStaticNow (void) const
{
	return true;
}
```

- [ ] **Step 4: Build**

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add mclib/bdactor.h mclib/bdactor.cpp
git commit -m "feat(slice3-3b): TreeAppearance::IsStaticNow() returns true unconditionally

TreeAppearance has no appearance-internal dynamism — fall state is owner-side
(OBJECT_FLAG_FALLING on TerrainObject) and is composed at the call site by
Task 7. Per adversarial review C1+C2: the appearance has no pitchAngle or
fallRate member, and OBJECT_FLAG_FALLING is set externally by collision
callbacks (terrobj.cpp:286-287, bldng.cpp:1453-1455), so a predicate-only
design would silently skip the impact frame. Composition is mandatory."
```

---

### Task 7: Add the env-gated skip with call-site falling check

**Files:**
- Modify: `code/terrobj.cpp:603-609` (the block from Task 2).

- [ ] **Step 1: Read the current block (post-Task 2)**

Run: `sed -n '603,615p' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/terrobj.cpp`

Confirm Task 2's `++g_staticUpdateCounters.objects_seen;` and `++g_staticUpdateCounters.updates_run;` are present.

- [ ] **Step 2: Replace with the gated skip**

Find:
```cpp
		if (inView)
		{
			windowsVisible = turn;
			{
				ZoneScopedN("TerrainObject::update appearanceUpdate");
				++g_staticUpdateCounters.objects_seen;
				// Stage 3.A: count only — no skip. Stage 3.B (Task 7) adds the gate.
				++g_staticUpdateCounters.updates_run;
				appearance->update();
			}
```

Replace with:
```cpp
		if (inView)
		{
			windowsVisible = turn;
			{
				ZoneScopedN("TerrainObject::update appearanceUpdate");
				++g_staticUpdateCounters.objects_seen;

				// Stage 3.B static-update bypass.
				//
				// Composition: appearance->IsStaticNow() (type-time claim) AND
				// !getFlag(OBJECT_FLAG_FALLING) (owner-side instance state). The
				// owner-side check is MANDATORY, not an optimization: the
				// FALLING flag is set EXTERNALLY by collision callbacks (see
				// code/terrobj.cpp:286-287 for tree-vs-mover collision and
				// code/bldng.cpp:1453-1455 for buildings), so a predicate-only
				// gate would silently skip the impact frame's animation setup.
				//
				// Stage 3.D will extend this composition with damage / power /
				// effect-active checks for BldgAppearance.
				const bool appearanceClaimsStatic = appearance->IsStaticNow();
				const bool ownerForcesDynamic     = getFlag(OBJECT_FLAG_FALLING);

				if (s_staticUpdateSkip && appearanceClaimsStatic && !ownerForcesDynamic) {
					++g_staticUpdateCounters.updates_skipped;
					if (s_staticUpdateTrace) {
						printf("[STATIC_UPDATE v1] event=skip frame=%u obj=%p\n",
							g_mc2FrameCounter, (void*)this);
						fflush(stdout);
					}
				} else {
					++g_staticUpdateCounters.updates_run;
					if (ownerForcesDynamic && appearanceClaimsStatic) {
						++g_staticUpdateCounters.dyn_falling;
					}
					appearance->update();
				}
			}
```

- [ ] **Step 3: Build**

Expected: clean build.

- [ ] **Step 4: Run scripts/check-destroy-invariant.sh**

Per worktree CLAUDE.md: "Before any commit that touches object lifecycle." Slice 3 doesn't add `setExists` sites but it gates `appearance->update()`, which is lifecycle-adjacent.

```bash
sh /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-destroy-invariant.sh
```

Expected: exit 0.

- [ ] **Step 5: Smoke test default-off — must be behavior-equivalent under default-off vs Stage 3.A**

```
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: exit 0, summary lines show `skip=0` (env unset → gate inert), +0 destroys delta vs Stage 3.A baseline. Frame count parity within ±1%. If `skip` is non-zero on default-off run, the env gate is broken — STOP.

- [ ] **Step 6: Smoke test with skip enabled — feature ON for the first time**

```
MC2_STATIC_UPDATE_SKIP=1 py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected:
- exit 0 (no crashes)
- `[STATIC_UPDATE v1]` summary lines show non-zero `skip` count, growing each 600-frame interval
- destroys delta vs default-off run = +0 (CRITICAL — any non-zero delta means the bypass is breaking lifecycle; revert and root-cause)
- on a forest mission like `mc2_01`, expect `skip / seen` ratio to be substantial (rough estimate: trees are typically 30-60% of TerrainObjects on tree-heavy maps)
- `dyn_falling` may be 0 if no trees fall during smoke (unattended smoke doesn't shoot trees) — that's OK, the field's coverage is verified by Step 7's manual check.

- [ ] **Step 7: Manual visual + counter check — load mc2_01, knock down a tree, confirm composition**

Per-author manual step. Launch the deployed exe with **both** trace AND skip enabled:

```
MC2_STATIC_UPDATE_SKIP=1 MC2_STATIC_UPDATE_TRACE=1 <deployed-exe>
```

Load mc2_01. Verify:

**Visual:**
- Standing trees render correctly (not invisible, not flickering)
- Pan camera around — terrain LOD doesn't strand trees
- Shoot a tree — fall animation plays smoothly through the full fall
- Fallen tree stays in fallen pose; no pop-back-to-standing

**Counter / trace expectations (the strong check — visual smoothness alone is not enough):**

Before knocking down any tree, watch a `[STATIC_UPDATE v1]` summary line. Expected:
```
[STATIC_UPDATE v1] frame=N seen=N run=N skip=N>0 dyn_falling=0 ...
```
(`skip` substantially non-zero proves the gate fires for static trees; `dyn_falling=0` proves no spurious falling-state.)

Knock down ONE tree. Within a few seconds, in the next summary line OR in `[STATIC_UPDATE v1] event=skip` per-frame trace lines, verify:

```
For the falling tree, while OBJECT_FLAG_FALLING is set:
  - Per-frame trace: NO `event=skip` line for that obj=<addr> (the gate composition correctly suppresses the skip)
  - Cumulative dyn_falling counter increments by 1 PER FRAME the tree is mid-fall
  - updates_run delta increases by ≥1 per frame for the falling tree
  - updates_skipped delta does NOT increase for that object on those frames
```

After the tree finishes falling (`OBJECT_FLAG_FALLING` cleared by `OBJECT_FLAG_FALLEN` transition at terrobj.cpp:587-588), the tree should re-enter the skip set:
```
- dyn_falling stops incrementing
- updates_skipped resumes incrementing for that object (now a fallen, static tree)
```

If `dyn_falling` stays at 0 throughout the fall → the call-site `getFlag(OBJECT_FLAG_FALLING)` check is broken or the flag is set on a different object than expected. Root-cause before continuing — likely the `OBJECT_FLAG_FALLING` set at terrobj.cpp:286-287 (collision callback) targets a different object pointer than the one being iterated.

If visual fall is smooth BUT counters show no `dyn_falling` increment → the call site is bypassing the gate entirely (not running it). Verify the patched block actually compiled in (check the binary timestamp, redeploy if needed).

- [ ] **Step 8: Commit**

```bash
git add code/terrobj.cpp
git commit -m "feat(slice3-3b): env-gated static-update bypass for trees (default-off)

MC2_STATIC_UPDATE_SKIP=1 enables; default-off equivalent to Stage 3.A.

Per adversarial review C2: the !getFlag(OBJECT_FLAG_FALLING) call-site
check is MANDATORY, not an optimization. OBJECT_FLAG_FALLING is set
externally from collision callbacks (terrobj.cpp:286-287 for trees,
bldng.cpp:1453-1455 for buildings), so a predicate-only gate would
silently skip the impact frame. The brainstorm Q3 self-consistency
claim was wrong on this point.

Tier1 smoke clean both default-off and skip-on; +0 destroys delta;
forest-heavy mc2_01 shows substantial skip ratio. Manual visual check
on mc2_01 confirms tree fall animation completes correctly."
```

---

### Stage 3.B gate (must pass before any later stage planning):
- Default-env smoke (skip flag unset): behavior-equivalent under default-off vs Stage 3.A — `skip=0` in summaries, +0 destroys delta, frame count parity ±1%.
- `MC2_STATIC_UPDATE_SKIP=1` smoke: exit 0, non-zero skip counts, +0 destroys delta vs default-off.
- Manual visual check on mc2_01 (Task 7 Step 7) clean — trees render, fall animations smooth, no pop.
- `scripts/check-destroy-invariant.sh` exit 0.
- No new TGL pool warnings (per `memory/tgl_pool_exhaustion_is_silent.md` — slice 3 doesn't touch allocation paths but verify).
- Tracy plot "TerrainObjects static skipped" populates with non-zero values when skip env is set; "TerrainObjects dynamic updates" drops by the corresponding amount.

---

## Self-review notes (v2)

- **Spec coverage:** Q1 (predicate placement) → Tasks 5/6, plus call-site composition documented inline. Q2 (allowlist scope) → Task 6 + brainstorm-grounded explicit "trees only" framing. Q3 (transition handling) → Task 7 Step 7 manual visual check + commit message documents the corrected story (call-site composition is mandatory, brainstorm Q3's "self-consistency" claim was wrong). Q4 (recalcBounds stays) → call-site edits ONLY touch the inner `appearance->update()`, recalcBounds at terrobj.cpp:599-601 is untouched and proven by code excerpt in Task 2 Step 1. Q5 (no texture-handle skipping) → not in scope. Q6 (counter design) → Tasks 1, 3, 4. Q7 (staging) → this plan = 3.A + 3.B (3.C/3.D/3.E deferred). Q8 (perf gate) → deferred to Stage 3.E. Q9 (known traps) → Stage 3.B gate covers cull-gate, pool-exhaustion verification, and texture-handle traps explicitly N/A.
- **Citation re-verification:** `mclib/appear.h` Appearance class around `virtual long update` at ~124; `mclib/dappear.h:23` AppearancePtr typedef; `mclib/bdactor.h:471` TreeAppearance class; `mclib/bdactor.h:188` BldgAppearance class; `mclib/bdactor.h:220-222` BldgAppearance GOSFX members; `code/terrobj.cpp:286-287` external OBJECT_FLAG_FALLING setter (tree-vs-mover collision); `code/bldng.cpp:1453-1455` external OBJECT_FLAG_FALLING setter (building); `code/terrobj.cpp:572-590` in-update fall logic; `code/terrobj.cpp:603-609` call site; `code/terrobj.h:149` bldgDustPoofEffect; `code/objmgr.cpp:1786-1789` existing TracyPlot site; `code/gameobj.cpp:100-104` MC2_DESTROY_TRACE pattern + the stale comment fixed by Task 9; `mclib/tgl.cpp:3718` g_mc2FrameCounter definition; `GameOS/gameos/gameosmain.cpp:660-715` INSTR banner site; `scripts/run_smoke.py:217` default duration 120s.
- **Pre-flight gates** (P1-P4) catch the structural failure modes the v1 review surfaced. P3 (TreeAppearance::update return-value audit) is the new safety net for the lifecycle-swallowing concern m5 raised.

---

## Adversarial review (v2)

Per worktree CLAUDE.md "Review Discipline" — re-run `adversarial-plan-review` against v2. Lighter pass expected since corrections are mechanical, but the dispatch must still be code-grounded. Specifically the v2 review should confirm:
- Pre-flight P1-P4 produce the expected results in the current tree.
- The call-site composition in Task 7 actually compiles and `getFlag(OBJECT_FLAG_FALLING)` is in scope (it's a `GameObject` method; `TerrainObject` inherits — verify).
- The new tiny header `code/static_update_counters.h` is picked up by the build system (CMake glob or explicit list — check `CMakeLists.txt` for `code/*.h` patterns; if explicit, add the file).
- No regression in v1's "MAJOR/MINOR" verified-clean items (banner buffer math, recalcBounds untouched, BldgAppearance member cites).
