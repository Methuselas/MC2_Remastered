# Static Decorative Elimination - Plan 2A: Preconditions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the three CP-3-independent preconditions for the
static-decorative-elimination slice (CP-1 per-mission static-shadow reset,
CP-2 decorative-mesh static-caster submission path, the DEDICATED decorative
collision proxy data structure + mission-load population) as additive,
independently verifiable infrastructure that changes no runtime behavior yet.

**Architecture:** Pure additive infrastructure. CP-1 adds a mission-start
reset of process-scoped static-shadow priming state. CP-2 routes the
existing `gos_DrawShadowObjectBatch` mesh-shadow primitive into the static
shadow FBO/light-matrix context during the `Shadow.StaticAccum` pass (the
primitive already exists for the dynamic pass; only the static-context entry
point is missing). The collision proxy is a mission-lifetime, handle-keyed
side structure built from the same `ObjData` walk that counts terrain
objects; it is populated but not yet queried (callsite repoint is Plan 2C).
Nothing in this plan severs anything or flips a killswitch.

**Tech Stack:** C++ OpenGL engine (MC2), branch `claude/nifty-mendeleev`,
worktree `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`.
Verification is the engine's established mechanism: env-gated `[SUBSYS v1]`
lifecycle probes + the tier1 smoke gate (no unit-test harness exists; do not
invent one - follow the existing probe+smoke pattern per worktree CLAUDE.md
"Debug instrumentation rule").

**Inputs (read before starting):**
- Spec: `docs/superpowers/specs/2026-05-17-static-decorative-elimination-design.md`
- Stage 0: `docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md`
  (Blocker 3 collision proxy spec; Blocker 4 CP-1/CP-2; the "Stage 0 Design
  Delta" section)
- Stage 0 passed its adversarial gate (commit chain through `f28d38e`).

**Discipline (hard, applies to every task):** no emoji; no wall-clock
projections; per-file `git add` by exact filename only (the branch carries
foreign uncommitted WIP - NEVER `git add -A`/`git add .`); grep-verify every
file:line at task execution time (all citations below are starting points,
re-grep - the prior phase saw ~1 week of drift); build
`--config RelWithDebInfo` with full relink when load-bearing functions
change (`rm build64/RelWithDebInfo/mc2.exe` + changed `.obj`, or
`--clean-first`); deploy `cp -f` per file + `diff -q` (NEVER `cp -r`);
env-gated `[SUBSYS v1]` probes land in the same commit as the code they
instrument and are demoted-not-deleted.

**VERIFICATION PROTOCOL OVERRIDE (user, 2026-05-18 - authoritative):**
Wherever a task's verify step says "tier1 smoke", instead run this exact
2-mission command (mc2_01 = mission 1, mc2_24 = mission 24), keeping logs
and enabling the cost-split path-activation counters:

```
set MC2_TERRAIN_COST_SPLIT=1   (plus the task's own probe env, e.g.
                                MC2_DECOR_SHADOW_TRACE / MC2_DECOR_PROXY_TRACE)
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --mission mc2_01 --mission mc2_24 --duration 30 --keep-logs --kill-existing
```

- Exit `0` on BOTH missions = pass. Nonzero => inspect the kept logs in
  `tests/smoke/artifacts/<latest>/`.
- The cost-split output is used HERE only to read CALL VOLUME / which paths
  are vs are not activated at each stage (path-activation evidence). Do
  NOT interpret cost-split ABSOLUTE timings as perf deltas: per
  `memory/cost_split_instrumentation_is_observer_effect_dominated.md`
  chrono scopes inflate ~3.5x and fabricate fake setup_total. Each
  task's PASS criterion stays the probe-counter assertion + smoke exit 0;
  call-volume is recorded as supporting evidence only.
- The user runs a clean Tracy capture at the END of the slice for real
  timing - no per-task Tracy, no per-task perf claim.

**Verified-at-plan-write targets (re-grep at task time):**
- CP-1: `s_terrainShadowPrimed` `mclib/txmmgr.cpp:1510-1518` (function-local
  static, never reset between missions); `staticLightMatrixBuilt_`
  `GameOS/gameos/gos_postprocess.cpp:60,1120,1198` (reset only in
  `initShadows`, process-lifetime); `gos_RequestFullShadowRebuild()`
  `GameOS/gameos/gameos_graphics.cpp:6279` (sets `s_shadowRebuildPending`).
- CP-2: `gos_DrawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib, ...)`
  `GameOS/gameos/gameos_graphics.cpp:6288` (mesh-shadow primitive, used by
  the dynamic pass); `gos_BeginShadowPrePass`/`gos_EndShadowPrePass`
  `:6269/:6272`; the static accum pass `Shadow.StaticAccum`
  `mclib/txmmgr.cpp:1525-1587` (terrain-only today via
  `gos_DrawShadowBatchTessellated`).
- Collision proxy: build source is the terrain-object count/registration
  walk `code/objmgr.cpp:993-1046` region (`countTerrainObjects`/
  `countObject`); the 4 query callsites (Plan 2C, not here) are
  `mech.cpp:1103`, `gvehicl.cpp:789`, `artlry.cpp:672`, `carnage.cpp:509`.

---

### Task 1: CP-1 - per-mission static-shadow reset hook

**Files:**
- Modify: `mclib/txmmgr.cpp` (the `s_terrainShadowPrimed` site, ~:1510-1518 - re-grep)
- Modify: `GameOS/gameos/gos_postprocess.cpp` (add a public reset entry; `staticLightMatrixBuilt_` ~:1120/:1198 - re-grep)
- Modify: `GameOS/gameos/gos_postprocess.h` (declare the reset entry)
- Modify: the per-mission init chokepoint - grep `gos_terrain_lighting::mission_init` / `gpu_cull::compute_init` in `code/mission.cpp` to find the existing per-mission init call site; the reset hook goes adjacent to it

- [ ] **Step 1: Re-grep and record the exact current state**

Run (Grep tool): `s_terrainShadowPrimed` in `mclib/txmmgr.cpp`;
`staticLightMatrixBuilt_` in `GameOS/gameos/gos_postprocess.{cpp,h}`;
`gos_RequestFullShadowRebuild` across the worktree; and
`mission_init|compute_init` in `code/mission.cpp`. Write the confirmed
file:line of each into the task notes. Expected: `s_terrainShadowPrimed`
is a function-local `static bool` never reset across missions;
`staticLightMatrixBuilt_` reset only in `initShadows`; a per-mission init
chokepoint exists in `mission.cpp`.

- [ ] **Step 2: Add the public reset entry to gos_postprocess**

In `gos_postprocess.h`, declare alongside the other `gos_*` shadow
prototypes:

```cpp
// CP-1: reset process-scoped static-shadow priming so the next mission
// rebuilds the world-fixed static shadow map and the static light matrix.
void gos_ResetStaticShadowPriming();
```

In `gos_postprocess.cpp`, implement (place near the `staticLightMatrixBuilt_`
definition; re-grep its class/owner first):

```cpp
void gos_ResetStaticShadowPriming() {
    // Static light matrix is rebuilt lazily when staticLightMatrixBuilt_
    // is false (see the early-out at the top of buildStaticLightMatrix()).
    if (g_postProcess) g_postProcess->staticLightMatrixBuilt_ = false;
    gos_RequestFullShadowRebuild();
}
```

If `staticLightMatrixBuilt_` is private, add a `void
resetStaticLightMatrix() { staticLightMatrixBuilt_ = false; }` member and
call that instead of touching the field directly. Re-grep the actual owner
symbol (`g_postProcess` vs other) and access level before writing.

- [ ] **Step 3: Make s_terrainShadowPrimed resettable**

The function-local `static bool s_terrainShadowPrimed` (txmmgr.cpp ~:1510)
cannot be reset from outside. Promote it to a file-scope static in
`txmmgr.cpp` and add a file-scope reset function:

```cpp
// CP-1: file-scope so a per-mission hook can re-prime the static
// terrain shadow accumulation for the new mission.
static bool s_terrainShadowPrimed = false;
void mc_ResetTerrainShadowPrimed() { s_terrainShadowPrimed = false; }
```

Replace the old function-local declaration with a reference to the
file-scope one (delete the `static` keyword at the old in-function site so
it uses the file-scope symbol). Declare `void mc_ResetTerrainShadowPrimed();`
in the appropriate mclib header that `mission.cpp` already includes (grep
which header declares the other `mc_*` txmmgr entry points; reuse it).

- [ ] **Step 4: Call the reset from the per-mission init chokepoint**

At the per-mission init site in `code/mission.cpp` identified in Step 1
(adjacent to the existing `mission_init`/`compute_init` calls), add:

```cpp
// CP-1: new mission - re-prime the world-fixed static shadow map and
// the static light matrix (both are process-scoped and otherwise leak
// the previous mission's state).
gos_ResetStaticShadowPriming();
mc_ResetTerrainShadowPrimed();
```

- [ ] **Step 5: Add the env-gated lifecycle probe (same commit)**

At the reset call site, add (matching the existing `MC2_DEBUG_SHADOW`
macro pattern - grep it for the exact form):

```cpp
if (getenv("MC2_DECOR_SHADOW_TRACE")) {
    printf("[DECOR_SHADOW v1] event=mission_reset_priming frame=%u\n",
           g_mc2FrameCounter); fflush(stdout);
}
```

- [ ] **Step 6: Build (full relink) and deploy**

Run:
```
rm -f build64/RelWithDebInfo/mc2.exe
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```
Expected: clean build, `mc2.exe` produced. Then deploy exe-only:
`cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe"` then `diff -q` the two.

- [ ] **Step 7: Verify via smoke + probe**

Run:
```
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing
```
Set `MC2_DECOR_SHADOW_TRACE=1` for the run. Expected: smoke exit `0`
(5/5); `[DECOR_SHADOW v1] event=mission_reset_priming` appears once per
mission in the artifact log (5 missions => >=5 occurrences). No shadow
regression vs the prior build (this is additive priming; the static map
content is unchanged, only its rebuild is re-triggered per mission). If
exit nonzero, inspect `tests/smoke/artifacts/<latest>/`.

- [ ] **Step 8: Commit**

```
git add mclib/txmmgr.cpp GameOS/gameos/gos_postprocess.cpp GameOS/gameos/gos_postprocess.h code/mission.cpp <the mclib header touched>
git commit -m "feat(static-decor): CP-1 per-mission static-shadow reset hook"
```

---

### Task 2: CP-2 - decorative-mesh static-caster submission entry point

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (add `gos_DrawShadowObjectBatchStatic`, near `gos_DrawShadowObjectBatch` ~:6288 - re-grep)
- Modify: `GameOS/gameos/gameos.hpp` or the header declaring `gos_DrawShadowObjectBatch` (grep which - declare the new entry there)
- Modify: `mclib/txmmgr.cpp` (the `Shadow.StaticAccum` pass ~:1525-1587 - re-grep - is where Plan 2C will call it; this task only builds and self-tests the entry point)

- [ ] **Step 1: Re-grep the static vs dynamic shadow submit paths**

Run (Grep): `gos_DrawShadowObjectBatch` and `gos_DrawShadowBatchTessellated`
and `gos_BeginShadowPrePass` and `staticLightSpaceMatrix_` /
`shadowFBO_` / `dynShadowFBO_` in `GameOS/gameos/gameos_graphics.cpp` and
`gos_postprocess.cpp`. Record: (a) how `gos_DrawShadowObjectBatch` binds
its FBO + light matrix (it targets the DYNAMIC FBO/matrix today), (b) how
the static accum pass (`Shadow.StaticAccum`, txmmgr.cpp) binds the STATIC
`shadowFBO_` + `staticLightSpaceMatrix_`. The delta between (a) and (b) is
exactly the new entry point's job.

- [ ] **Step 2: Add the static-context object-batch shadow submit**

In `gameos_graphics.cpp`, next to `gos_DrawShadowObjectBatch`, add a
sibling that binds the STATIC shadow target/matrix instead of the dynamic
one. Use the exact static-FBO/matrix binding the `Shadow.StaticAccum`
terrain path uses (copy that binding sequence, verified in Step 1 - do not
guess the uniform/matrix names):

```cpp
// CP-2: submit an arbitrary TG_Shape mesh (vb/ib/vdecl) as a STATIC
// shadow caster - bakes into the world-fixed static shadow map using
// staticLightSpaceMatrix_, parallel to gos_DrawShadowObjectBatch which
// targets the dynamic FBO. Caller must be inside the static shadow
// pre-pass (gos_BeginShadowPrePass already bound the static target by
// the Shadow.StaticAccum path).
void gos_DrawShadowObjectBatchStatic(HGOSBUFFER vb, HGOSBUFFER ib,
                                     /* match gos_DrawShadowObjectBatch's
                                        remaining signature exactly -
                                        re-grep it in Step 1 */ ...) {
    // Body: identical draw to gos_DrawShadowObjectBatch EXCEPT the
    // light-space matrix uniform is staticLightSpaceMatrix_ (not the
    // dynamic light matrix). Re-use the same shader/program the static
    // terrain accum path uses. Fill the parameter list and body from
    // the Step-1-verified signatures; no invented uniforms.
}
```

Note for the implementer: the signature/body MUST be filled from the
Step-1 grep of the real `gos_DrawShadowObjectBatch` and the static-accum
binding - this plan deliberately does not fabricate the GL uniform names;
copying the verified existing pattern is the task.

- [ ] **Step 3: Declare the entry point**

Add the prototype to the header that declares `gos_DrawShadowObjectBatch`
(grep which header; same file). Match the finalized Step-2 signature exactly
(C++/decl lockstep - a mismatch is a silent link error).

- [ ] **Step 4: Self-test harness (env-gated, additive, no behavior change)**

Add an env-gated one-shot self-test: when `MC2_DECOR_SHADOW_SELFTEST=1`,
after the static accum pass in `txmmgr.cpp` (re-grep `Shadow.StaticAccum`
end), submit ONE known decorative tree shape through
`gos_DrawShadowObjectBatchStatic` and emit:

```cpp
if (getenv("MC2_DECOR_SHADOW_SELFTEST")) {
    // one-shot: submit the first registered tree's shape as a static
    // caster, then read back whether the static depth tex changed in
    // its projected footprint (reuse any existing shadow-debug readback;
    // if none exists, gate the print on GL error state only).
    GLenum e = glGetError();
    printf("[DECOR_SHADOW v1] event=selftest_static_submit gl_err=0x%x\n", e);
    fflush(stdout);
}
```

This proves the entry point links, binds the static target without a GL
error, and is callable from the static accum context. It does NOT yet wire
real decorative shadows (that is Plan 2C).

- [ ] **Step 5: Build (full relink) + deploy**

Same build/deploy commands as Task 1 Step 6 (full relink: this adds an
exported function + touches txmmgr.cpp).

- [ ] **Step 6: Verify via smoke + selftest probe**

Run the tier1 smoke (Task 1 Step 7 command) with
`MC2_DECOR_SHADOW_SELFTEST=1`. Expected: exit `0` (5/5);
`[DECOR_SHADOW v1] event=selftest_static_submit gl_err=0x0` appears (zero
GL error => the static-context submit path is valid); NO visual shadow
regression (the self-test submits one extra caster into the static map -
acceptable transient; confirm tier1 still passes). If `gl_err` nonzero,
the static binding is wrong - fix before commit (do not commit a
GL-erroring path).

- [ ] **Step 7: Commit**

```
git add GameOS/gameos/gameos_graphics.cpp <the header touched> mclib/txmmgr.cpp
git commit -m "feat(static-decor): CP-2 static-context decorative-mesh shadow submit"
```

---

### Task 3: DEDICATED decorative collision proxy - structure + mission-load population

**Files:**
- Create: `GameOS/gameos/decor_collision_proxy.h` (the proxy type + API)
- Create: `GameOS/gameos/decor_collision_proxy.cpp` (implementation)
- Modify: `code/objmgr.cpp` (populate from the terrain-object walk ~:993-1046 - re-grep)
- Modify: `GameOS/gameos/CMakeLists.txt` or the relevant source list (add the new .cpp - grep how sibling GameOS .cpp files are listed)

This builds and populates the proxy. It is NOT queried yet (the 4
collision callsites are repointed in Plan 2C). Pure additive.

- [ ] **Step 1: Re-grep the population source + the proxy key fields**

Run (Grep): `countTerrainObjects|countObject` in `code/objmgr.cpp`;
`OBJECT_FLAG_TANGIBLE|setTangible` in `code/terrobj.cpp`; the decorative
classification (`TREE|TERRAINOBJECT` `numCollidableObjects++`) ~objmgr.cpp:1017.
Record the exact data available at registration: handle, blockNumber,
world AABB / position, the TREE/TERRAINOBJECT discriminant. The proxy key
per RESOLUTION.md Blocker 3 is `handle, blockNumber, AABB, tangible`.

- [ ] **Step 2: Define the proxy type**

`GameOS/gameos/decor_collision_proxy.h`:

```cpp
#pragma once
#include <vector>
#include <cstdint>

// DEDICATED decorative collision proxy (Stage 0 Blocker 3). Mission-
// lifetime, handle-keyed. Lets mech/vehicle/artillery/carnage collision
// still find decorative trees AFTER they are severed from the per-frame
// objBlockInfo block walk (Plan 2C repoints the 4 callsites here).
struct DecorProxyEntry {
    int32_t  handle;        // objList slot (unchanged by severance, HC-1)
    int32_t  blockNumber;   // terrain block, for per-block query
    float    aabbMin[3];
    float    aabbMax[3];
    bool     tangible;      // mirrors OBJECT_FLAG_TANGIBLE; cleared on fall
};

class DecorCollisionProxy {
public:
    static DecorCollisionProxy& instance();
    void clear();                                   // mission teardown
    void add(const DecorProxyEntry& e);             // mission-load populate
    void setTangible(int32_t handle, bool v);       // deregister hook (2C/OC-1)
    // Per-block query for the 4 collision callers (used in Plan 2C):
    const std::vector<int32_t>& blockHandles(int32_t blockNumber) const;
    size_t size() const { return entries_.size(); }
private:
    std::vector<DecorProxyEntry> entries_;
    std::vector<std::vector<int32_t>> byBlock_;     // blockNumber -> handles
    std::vector<int32_t> empty_;                    // returned for OOB block
};
```

- [ ] **Step 3: Implement the proxy**

`GameOS/gameos/decor_collision_proxy.cpp`: singleton; `clear()` empties
both vectors; `add()` pushes the entry and appends its handle to
`byBlock_[blockNumber]` (growing `byBlock_` as needed); `setTangible()`
linear-finds by handle (mission-lifetime, small N per block; not a
per-frame path) and sets the flag; `blockHandles()` bounds-checks
`blockNumber` and returns `empty_` for out-of-range (matches the existing
`(blockNumber>=0 && blockNumber<totalBlocks)` guard at artlry.cpp:741 /
carnage.cpp:531 noted in RESOLUTION.md Blocker 3). No per-frame iteration
anywhere in this file.

- [ ] **Step 4: Populate at mission load from the terrain-object walk**

In `code/objmgr.cpp`, at the terrain-object registration walk identified
in Step 1 (where each TREE/TERRAINOBJECT gets its handle/block), after the
object is registered add (re-grep the exact loop variable names):

```cpp
// Decorative collision proxy population (additive; queried in Plan 2C).
if (/* obj is TREE or TERRAINOBJECT - use the Step-1-verified discriminant */) {
    DecorProxyEntry e;
    e.handle      = /* the handle just assigned */;
    e.blockNumber = /* the block being filled */;
    /* fill aabbMin/Max from the object's bounds; tangible = true */
    DecorCollisionProxy::instance().add(e);
}
```

Call `DecorCollisionProxy::instance().clear()` at the matching mission
teardown (grep where the terrain-object arrays are freed / where
`GameObjectManager` resets for a new mission - co-locate the clear there).

- [ ] **Step 5: Env-gated population probe (same commit)**

After the populate loop completes:

```cpp
if (getenv("MC2_DECOR_PROXY_TRACE")) {
    printf("[DECOR_PROXY v1] event=populated count=%zu\n",
           DecorCollisionProxy::instance().size()); fflush(stdout);
}
```

- [ ] **Step 6: Add the new .cpp to the build**

Grep how other `GameOS/gameos/*.cpp` are listed in the build
(`CMakeLists.txt` or a source-list `.cmake`); add
`decor_collision_proxy.cpp` following that exact pattern.

- [ ] **Step 7: Build (full relink) + deploy**

Same as Task 1 Step 6 (new translation unit + objmgr.cpp change => full
relink mandatory).

- [ ] **Step 8: Verify via smoke + probe**

Run tier1 smoke (Task 1 Step 7 command) with `MC2_DECOR_PROXY_TRACE=1`.
Expected: exit `0` (5/5); `[DECOR_PROXY v1] event=populated count=N`
appears once per mission with `N` matching the registered
TREE/TERRAINOBJECT count for that mission (cross-check against the
`[STATIC_PROP_REG v1] event=mission_load` `terrainObjects=` count in the
same log - they should be equal or the proxy is mis-populated). Zero
behavior change (proxy is populated but not queried). If counts mismatch,
fix before commit.

- [ ] **Step 9: Commit**

```
git add GameOS/gameos/decor_collision_proxy.h GameOS/gameos/decor_collision_proxy.cpp code/objmgr.cpp <build list file>
git commit -m "feat(static-decor): DEDICATED decorative collision proxy (populate-only)"
```

---

## Self-Review

- **Spec/Delta coverage:** Plan 2A covers exactly the CP-3-independent
  preconditions from the RESOLUTION.md "Stage 0 Design Delta" precondition
  list: CP-1 (Task 1), CP-2 (Task 2), the DEDICATED collision proxy
  structure+population (Task 3). OB-1 (post-Load re-run), OC-1 (deregister
  ordering), the severance, the parity gate, the 4-callsite repoint,
  default-on/demote, and the CP-3-gated fallback are explicitly OUT of
  Plan 2A and belong to Plans 2B/2C/2D (stated in the orchestration note
  below). No Delta item is silently dropped: each non-2A delta is named as
  a downstream plan's responsibility.
- **Placeholder scan:** the only deliberately-deferred specifics are the
  GL uniform/signature bodies in CP-2 Task 2 Step 2, which the plan
  explicitly instructs to fill from a Step-1 grep of the real existing
  `gos_DrawShadowObjectBatch` + static-accum binding rather than fabricate
  invented uniform names - this is grounded-deferral (copy a verified
  pattern), not a TBD. Flagged inline as such.
- **Consistency:** type/symbol names used across tasks
  (`gos_ResetStaticShadowPriming`, `mc_ResetTerrainShadowPrimed`,
  `gos_DrawShadowObjectBatchStatic`, `DecorCollisionProxy`,
  `DecorProxyEntry`, probe schema `[DECOR_SHADOW v1]`/`[DECOR_PROXY v1]`)
  are defined once and referenced consistently.
- **Independent testability:** each task is additive, builds, and is
  verified by the engine's real mechanism (env probe + tier1 smoke); none
  changes runtime behavior, so Plan 2A is safe to land before 2B/2C/2D.

## Plan orchestration (downstream, out of scope here)

- **Plan 2B** (Stages 1-2): StaticDecorativeSet bake + 144-byte
  `DecorParityRecord` + `MC2_DECOR_PARITY` dual-output zero-mismatch gate;
  legacy authoritative. Contingent on Plan 2A interfaces.
- **Plan 2C** (Stage 3): `objBlockInfo`-count severance (HC-2),
  repoint the 4 collision callsites to `DecorCollisionProxy`, wire
  decorative shadow via `gos_DrawShadowObjectBatchStatic`, OB-1 post-Load
  re-run, OC-1 deregister at `terrobj.cpp:393`, killswitch
  `MC2_STATIC_DECOR_GPU` default-off, `decoratives_seen_in_objmgr_loop==0`
  gate. Contingent on 2A+2B.
- **Plan 2D** (Stages 5-6): default-on flip, demote-not-delete, and the
  fallback/legacy path through `substrate_writeRecord` - first step
  re-greps `substrate_writeRecord` on this branch and BLOCKS if absent
  (CP-3 single-choke-point; do not duplicate the sibling helper).
- The implementation adversarial-review gate (second of the two gates;
  design-delta gate already PASSED with opus) is run at the end of Plan 2C
  (the behavior-changing plan) with the model NOT used for the design-delta
  gate; dispatch prompt must say "use the adversarial-plan-review skill"
  verbatim. Soak waived per
  `feedback_soak_waiver_with_probes_and_reviews_validated` (env-gated
  parity probe + zero-counter substitute).
