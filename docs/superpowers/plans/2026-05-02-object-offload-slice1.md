# Object Offload — Slice 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire static-prop multishapes (buildings + trees + generics) through the existing `GpuStaticPropBatcher` infrastructure under a new env-gated flag `MC2_GPU_OBJECTS=1`, with NO cull bypass and NO update-time changes. Slice ships behind flag as substrate for slice 2 (GPU vertex lighting).

**Architecture:** Reuse `GameOS/gameos/gos_static_prop_batcher.{h,cpp}` (already in tree from prior killswitched attempt). Add a new global `g_useGpuObjects` distinct from legacy `g_useGpuStaticProps`. Make the two mutually exclusive (slice 1 wins). Modify only `*Appearance::render()` for the three populations to call `submitMultiShape()`. Add Gate F counters and late-registration accounting.

**Tech Stack:** C++14, OpenGL 4.3, SDL2 keybinding, MC2 engine (terrain-pbr-mod / nifty-mendeleev worktree). Build via `cmake --config RelWithDebInfo`. Deploy via `/mc2-deploy`. Test via `scripts/run_smoke.py`.

**Spec:** [`docs/superpowers/specs/2026-05-02-object-offload-slice1-design.md`](../specs/2026-05-02-object-offload-slice1-design.md)

**Brainstorm:** [`docs/superpowers/brainstorms/2026-05-02-object-offload-scope.md`](../brainstorms/2026-05-02-object-offload-scope.md)

**Worktree paths (load-bearing — do not deviate):**
- Source: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.3/`
- Build: `cmake --build build64 --config RelWithDebInfo` (NEVER `Release` — crashes with GL_INVALID_ENUM)
- Smoke harness: `py -3 scripts/run_smoke.py --tier tier1 --kill-existing`
- **Do NOT run the menu canary** (`--menu-canary` / `--with-menu-canary`). Project preference: smoke gates are tier1-only for this arc. Canary is desktop-bound and not needed for object-offload validation.

---

## Task 0: Pre-flight — verify clean baseline

**Purpose:** Establish a known-clean tier1 5/5 PASS state before touching code, so any later regression bisects cleanly.

**Files:** none modified.

- [ ] **Step 1: Verify worktree is clean**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
git status
```

Expected: `branch claude/nifty-mendeleev` (or similar), no uncommitted changes that could conflate with the slice. If staged/dirty, surface to user before proceeding.

- [ ] **Step 2: Build clean**

Run the project's `/mc2-build` skill OR:

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```

Expected: build succeeds, no warnings new vs. prior commit.

- [ ] **Step 3: Deploy**

Run `/mc2-deploy` skill (per-file `cp -f` + `diff -q`). NEVER `cp -r`.

- [ ] **Step 4: Run baseline smoke (fast iteration variant)**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
```

Expected: exit code `0`. If nonzero, inspect `tests/smoke/artifacts/<timestamp>/` and surface to user. The slice cannot proceed if baseline is red.

- [ ] **Step 5: Capture baseline artifacts**

Save the baseline run's `[INSTR v1]` banner + `[TGL_POOL v1] summary` line for later comparison:

```bash
ls tests/smoke/artifacts/ | tail -1   # latest run dir
```

Note the run-dir path. Gate E (TGL pool peak ≤ pre-slice peak) compares against this run's pool summary.

---

## Task 1: Stage 1.A — Add `g_useGpuObjects` flag (no behavior change)

**Purpose:** Plumb the new flag through declaration / definition / env-read / banner. No call site uses it yet, so behavior is bit-identical to baseline. This task is the canary for whether plumbing is correct in isolation.

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_killswitch.h:8` (add extern declaration after existing `g_useGpuStaticProps`)
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp:16` (add definition near `g_useGpuStaticProps`)
- Modify: `GameOS/gameos/gameosmain.cpp` (add env read + INSTR banner field; near line 615-637)

- [ ] **Step 1: Add extern declaration in `gos_static_prop_killswitch.h`**

Open `GameOS/gameos/gos_static_prop_killswitch.h`. After the existing line:

```cpp
extern bool g_useGpuStaticProps;
```

Add:

```cpp
// Slice 1 (object-offload arc) flag, env-gated via MC2_GPU_OBJECTS=1.
// Mutually exclusive with g_useGpuStaticProps at runtime — see spec
// docs/superpowers/specs/2026-05-02-object-offload-slice1-design.md R1.
extern bool g_useGpuObjects;
```

- [ ] **Step 2: Add definition in `gos_static_prop_batcher.cpp`**

Open `GameOS/gameos/gos_static_prop_batcher.cpp`. After the existing line:

```cpp
bool g_useGpuStaticProps = false;
```

Add:

```cpp
bool g_useGpuObjects = false;
```

- [ ] **Step 3: Add env read in `gameosmain.cpp`**

Find the block around line 615 where `MC2_RENDER_WATER_FASTPATH`, `MC2_VERTEX_PROJECT_FAST` etc. are read. Add right after the `vpPar` line (currently 618):

```cpp
        const bool gpuObj  = (getenv("MC2_GPU_OBJECTS")             != nullptr);
        // Slice 1 invariant: mutually exclusive with legacy killswitch.
        // Setting g_useGpuObjects at startup here happens before any code
        // path can read it; legacy g_useGpuStaticProps starts false.
        if (gpuObj) g_useGpuObjects = true;
```

- [ ] **Step 4: Extend the INSTR banner**

Find the `snprintf(_cbbuf, sizeof(_cbbuf), ...)` call at gameosmain.cpp ~631-637. Extend the format string and arg list to include `gpu_objects=%d`. Final form:

```cpp
        snprintf(_cbbuf, sizeof(_cbbuf),
            "[INSTR v1] enabled: tgl_pool=%d destroy=%d gl_error_print=%d "
            "smoke=%d water_fp=%d water_parity=%d vp_fast=%d vp_parity=%d "
            "terrain_indirect=%d terrain_indirect_parity=%d "
            "gpu_objects=%d build=%s",
            tgl ? 1 : 0, destr ? 1 : 0, glprint ? 1 : 0, smoke ? 1 : 0,
            waterFp ? 1 : 0, waterPc ? 1 : 0, vpFast ? 1 : 0, vpPar ? 1 : 0,
            tInd ? 1 : 0, tIndP ? 1 : 0,
            gpuObj ? 1 : 0, build);
```

(The existing 512-byte `_cbbuf` has headroom; if the banner overflows, increase to 640 in the same edit.)

- [ ] **Step 5: Build**

Run `/mc2-build` or:

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```

Expected: success, no new warnings.

- [ ] **Step 6: Deploy**

Run `/mc2-deploy`.

- [ ] **Step 7: Smoke (default, no env var) — fast tier1**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
```

Inspect any mission's log for the banner. Expected:

```
[INSTR v1] enabled: ... gpu_objects=0 build=...
```

`gpu_objects=0` confirms the flag is plumbed but inactive when env unset.

- [ ] **Step 8: Smoke (with env set) — fast tier1**

```bash
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
```

Expected: banner shows `gpu_objects=1`. No call site reads the flag yet, so visual / behavior is identical to Step 7. tier1 5/5 PASS unchanged.

- [ ] **Step 9: Confirm tier1 5/5 PASS triple gate**

Both runs in Steps 7 and 8 are the gate. Verify exit `0` from both, +0 destroys delta vs Task 0 baseline (compare `[DESTROY v1]` summary if present, or any change in mission-specific destroy counters).

- [ ] **Step 10: Commit**

```bash
git add GameOS/gameos/gos_static_prop_killswitch.h GameOS/gameos/gos_static_prop_batcher.cpp GameOS/gameos/gameosmain.cpp
git commit -m "feat(objects): add g_useGpuObjects flag + MC2_GPU_OBJECTS env gate (no behavior change)

Stage 1.A of object-offload slice 1. Plumbs the new flag through
declaration / definition / env-read / [INSTR v1] banner. No call site
reads g_useGpuObjects yet; behavior is bit-identical to baseline.
Distinct from legacy g_useGpuStaticProps; mutual-exclusion enforced in
later stages.

Spec: docs/superpowers/specs/2026-05-02-object-offload-slice1-design.md
"
```

---

## Task 1.5: Stage 1.A.bis — Refactor batcher signature to accept population tag (no-op)

**Purpose:** Land the `GpuStaticPropPopulation` enum, the
`submitMultiShape(shape, pop)` two-arg signature, and empty
`recordEligibleActor` / `recordCpuFallback` stubs in a single commit.
**This is a no-op refactor** — counter bodies are still empty; the
single existing call site (`mclib/bdactor.cpp:1585`) is migrated to
pass `Legacy` since Task 2 hasn't replaced the wiring yet. Subsequent
Tasks 2/4/5 swap the tag from `Legacy` to slice-1 populations
(`Building` / `Tree` / `Generic`) when they introduce the new branch.

This sequencing prevents an intermediate broken-build state where
new call sites use a two-arg form the header doesn't declare.

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.h` (add enum + signature change + stubs)
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp:477` (signature change + empty stubs)
- Modify: `mclib/bdactor.cpp:1585` (single existing call site — migrate to two-arg with `Legacy` tag)

- [ ] **Step 1: Add enum + signature change to header**

Open `GameOS/gameos/gos_static_prop_batcher.h`. After the existing
`STATIC_PROP_FLAG_ALPHA_TEST` constant (around line 47), insert the
enum from Task 6 Step 1 verbatim:

```cpp
// Population tag — passed by caller so the batcher can split per-population
// counts in the [OBJBATCHER v1] summary. Not stored; consumed inside submit
// only.
//
// Legacy is the slice-1 fallback branch (g_useGpuStaticProps && !g_useGpuObjects)
// in *Appearance::render. Counted separately so Gate F's fallback-rate
// computation uses only slice-1 populations (Building/Tree/Generic).
enum class GpuStaticPropPopulation : uint8_t {
    Building = 0,
    Tree     = 1,
    Generic  = 2,
    Legacy   = 3,
};
```

Then change the `submitMultiShape` declaration from one-arg to two-arg
and add the accounting helpers. The current declaration at line ~93:

```cpp
[[nodiscard]] bool submitMultiShape(TG_MultiShape* multi);
```

Becomes:

```cpp
// Caller-side accounting. recordEligibleActor() is called by
// *Appearance::render BEFORE submit so caller-side bypasses
// (e.g., null shape) still count toward eligible_actors.
// recordCpuFallback() is called when no submit succeeded.
void recordEligibleActor(GpuStaticPropPopulation pop);
void recordCpuFallback(GpuStaticPropPopulation pop);

[[nodiscard]] bool submitMultiShape(TG_MultiShape* multi,
                                    GpuStaticPropPopulation pop);
```

- [ ] **Step 2: Empty-stub the helpers + extend submitMultiShape signature**

Open `GameOS/gameos/gos_static_prop_batcher.cpp`. Find the existing
implementation at line 477:

```cpp
bool GpuStaticPropBatcher::submitMultiShape(TG_MultiShape* multi) {
    // ... existing body ...
}
```

Change the signature only — body unchanged for now:

```cpp
bool GpuStaticPropBatcher::submitMultiShape(TG_MultiShape* multi,
                                            GpuStaticPropPopulation pop) {
    (void)pop;  // consumed by Task 6's counter additions; no-op here.
    // ... existing body — unchanged ...
}
```

Add the helper definitions in the same file, near the top (above
`submitMultiShape` is fine):

```cpp
void GpuStaticPropBatcher::recordEligibleActor(GpuStaticPropPopulation pop) {
    (void)pop;  // body filled in Task 6.
}

void GpuStaticPropBatcher::recordCpuFallback(GpuStaticPropPopulation pop) {
    (void)pop;  // body filled in Task 6.
}
```

- [ ] **Step 3: Migrate the existing single call site at bdactor.cpp:1585**

Open `mclib/bdactor.cpp:1585`. Current line:

```cpp
            submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(bldgShape);
```

Change to:

```cpp
            submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
                bldgShape, GpuStaticPropPopulation::Legacy);
```

(Tag as `Legacy` because this branch — gated by `g_useGpuStaticProps`
alone, no `g_useGpuObjects` term yet — is the legacy bypass-cull
path. Task 2 replaces this with the slice-1 mutual-exclusion form
that uses `Building` for the new path and `Legacy` for the legacy
fallback.)

Add `#include "gos_static_prop_batcher.h"` to `mclib/bdactor.cpp`
near the other includes if not already present (so the
`GpuStaticPropPopulation` enum is visible).

The two existing call sites at `mclib/bdactor.cpp:4004` (Tree) and
`mclib/genactor.cpp:802` (Generic) follow the same pattern: change
to two-arg with `GpuStaticPropPopulation::Legacy` tag. Add
`#include "gos_static_prop_batcher.h"` to `mclib/genactor.cpp` if
absent.

- [ ] **Step 4: Build, deploy**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```

Then `/mc2-deploy`. Build should succeed because the signature is
consistent across declaration, definition, and all three call sites.

- [ ] **Step 5: Smoke (default, RAlt+0 OFF)**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
```

Expected: tier1 PASS. The behavior is bit-identical to baseline because:
- `g_useGpuObjects` doesn't exist yet at this task (it does — added in
  Task 1 — but no call site reads it).
- The legacy `g_useGpuStaticProps` path is unchanged in behavior; only
  the call signature gained an unused-tagged parameter.
- The empty-stub helpers do nothing.

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp \
        mclib/bdactor.cpp mclib/genactor.cpp
git commit -m "refactor(objects): add GpuStaticPropPopulation tag + accounting helper stubs (no-op)

Stage 1.A.bis of slice 1. Establishes the two-arg
submitMultiShape(shape, pop) signature and empty-stub
recordEligibleActor / recordCpuFallback helpers so subsequent
slice-1 wiring (Tasks 2/4/5) lands without intermediate broken-build
states. The three existing call sites (bldg/tree/generic) tagged
Legacy because they're still on the legacy g_useGpuStaticProps path;
Tasks 2/4/5 retag them to Building/Tree/Generic when introducing the
slice-1 branch.

Counter bodies are added in Task 6.
"
```

---

## Task 2: Stage 1.B — Wire `BldgAppearance::render` with mutual exclusion

**Purpose:** Make slice 1 active for buildings only. The new path submits via the existing batcher's `submitMultiShape`, with the legacy bypass-cull path gated off when slice 1 is on.

**Files:**
- Modify: `mclib/bdactor.cpp:1546-1597` (`BldgAppearance::render` body — replace existing `g_useGpuStaticProps`-only branch with the mutual-exclusion pattern)

- [ ] **Step 1: Locate the existing branch**

Read `mclib/bdactor.cpp:1546-1597`. Confirm the current shape matches the spec's "Files / Modified / `BldgAppearance::render`":

```cpp
bool submittedToGpu = false;
if (g_useGpuStaticProps && bldgShape)
{
    submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(bldgShape);
}
if (!submittedToGpu)
{
    if (appearType->spinMe)
        bldgShape->Render(false,0.00001f);
    else if (!depthFixup)
        bldgShape->Render();
    ...
}
```

- [ ] **Step 2: Replace with mutual-exclusion wiring**

Add `#include "gos_static_prop_batcher.h"` at the top of `mclib/bdactor.cpp`
if not already present (header gives `GpuStaticPropPopulation` enum). Then
edit the same block to:

```cpp
// Slice 1 path (g_useGpuObjects). No cull bypass; submitMultiShape is
// per-child Layer-B by construction. Returns false only when EVERY child
// is ineligible.
//
// Caller-side accounting: recordEligibleActor() fires unconditionally
// when slice 1 reaches this site (so a null shape or skipped submit
// still counts toward eligible_actors). recordCpuFallback() fires
// when no submit succeeded.
bool submittedToGpu = false;
if (g_useGpuObjects)
{
    GpuStaticPropBatcher::instance().recordEligibleActor(
        GpuStaticPropPopulation::Building);
    if (bldgShape)
    {
        submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
            bldgShape, GpuStaticPropPopulation::Building);
    }
    if (!submittedToGpu)
    {
        GpuStaticPropBatcher::instance().recordCpuFallback(
            GpuStaticPropPopulation::Building);
    }
}
// Legacy bypass-cull path (g_useGpuStaticProps). Mutually exclusive with
// slice 1 — gated on !g_useGpuObjects so the two paths cannot coexist.
// See spec R1. Tagged Legacy so Gate F's fallback-rate is computed only
// over slice-1 populations.
if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && bldgShape)
{
    submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
        bldgShape, GpuStaticPropPopulation::Legacy);
}
if (!submittedToGpu)
{
    if (appearType->spinMe)
        bldgShape->Render(false,0.00001f);
    else if (!depthFixup)
        bldgShape->Render();
    else if (depthFixup > 0)
        bldgShape->Render(false,0.9999999f);
    else if (depthFixup < 0)
        bldgShape->Render(false,0.00001f);
}
```

(Preserve the four `bldgShape->Render(...)` cases verbatim; the only
change is the gating before them.)

**Note on signature ordering:** the two-arg `submitMultiShape(shape, pop)`
form is established by **Task 1.5** below, which runs BEFORE Task 2
modifies call sites. Empty-stub `recordEligibleActor` /
`recordCpuFallback` also exist by then. So when Task 2 ships, the
build is consistent end-to-end. Task 6 later FILLS the counter
bodies but does not change the signatures.

- [ ] **Step 3: Build, deploy**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```

Then `/mc2-deploy`.

- [ ] **Step 4: Visual canary smoke (slice 1 ON)**

```bash
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 20 --kill-existing
```

Expected: mission loads cleanly, buildings visible, no streak artifacts, no missing textures, no console errors. Compare visual against Task 0 baseline screenshots (`tests/smoke/artifacts/<baseline>/` vs `<this run>/`). Per `memory/feedback_smoke_mission_filter.md`, mc2_01 first 20 s exposes terrain/building issues immediately.

- [ ] **Step 5: Tier1 5/5 PASS triple**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
```

Expected: both `0`. +0 destroys delta in either run.

- [ ] **Step 6: TGL pool peak gate (Gate E)**

Inspect `[TGL_POOL v1] summary` line at shutdown of the `MC2_GPU_OBJECTS=1` run. Expected: pool peak NOT higher than the baseline captured in Task 0 Step 5. Per spec: slice 1 does not change which actors call `TransformShape`, so pool peak should be unchanged. If higher, the slice has accidentally bypassed the cull cascade somewhere — investigate before continuing.

- [ ] **Step 7: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(objects): wire BldgAppearance::render through GPU batcher under MC2_GPU_OBJECTS

Stage 1.B of slice 1. Adds the new-path branch BEFORE the legacy
g_useGpuStaticProps branch, with mutual-exclusion gating
(!g_useGpuObjects on the legacy path) so the two flags cannot coexist.
No cull bypass added; the five legacy bypass sites (mech3d.cpp:4183
etc.) remain untouched and are unreachable while slice 1 is on.

Spec R1 invariant.
"
```

---

## Task 3: Stage 1.B — Add RAlt+0 toggle guard

**Purpose:** Prevent a mid-session keystroke (RAlt+0) from flipping `g_useGpuStaticProps` on while `g_useGpuObjects` is true, which would put the process into a hybrid cull-bypass state.

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp:304-309` (the `case SDLK_0:` block)

- [ ] **Step 1: Replace the toggle handler**

Open `gameosmain.cpp` and find:

```cpp
        case SDLK_0:
            if (alt_debug) {
                g_useGpuStaticProps = !g_useGpuStaticProps;
                fprintf(stderr, "GPU Static Props: %s\n",
                        g_useGpuStaticProps ? "ON" : "OFF");
            }
```

Replace with:

```cpp
        case SDLK_0:
            if (alt_debug) {
                if (g_useGpuObjects) {
                    // Slice 1 is active — legacy killswitch is mutually
                    // exclusive (spec R1). Ignore the toggle and log once.
                    static bool s_loggedBlock = false;
                    if (!s_loggedBlock) {
                        fprintf(stderr, "[OBJBATCHER v1] event=legacy_toggle_blocked "
                                        "reason=g_useGpuObjects_active\n");
                        fflush(stderr);
                        s_loggedBlock = true;
                    }
                } else {
                    g_useGpuStaticProps = !g_useGpuStaticProps;
                    fprintf(stderr, "GPU Static Props: %s\n",
                            g_useGpuStaticProps ? "ON" : "OFF");
                }
            }
            break;
```

(Note the `break;` — preserved from the original switch case.)

- [ ] **Step 2: Build, deploy, smoke (default)**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```

Then `/mc2-deploy`. Then:

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
```

Expected: tier1 5/5 PASS. The default path (slice 1 OFF) is unchanged — RAlt+0 toggle still works as before in baseline mode.

- [ ] **Step 3: Manual verification of toggle guard**

This step requires user interaction since smoke harness doesn't issue keystrokes meaningfully. Surface to user with the verification recipe:

> Launch with `MC2_GPU_OBJECTS=1`. Load any mission. Press RAlt+0. Expect stderr to show `[OBJBATCHER v1] event=legacy_toggle_blocked reason=g_useGpuObjects_active` exactly once (subsequent presses produce no log spam). `g_useGpuStaticProps` remains false. Verify by inspecting visual: no behavior change.

If the user can't run interactively, this is acceptable to defer to slice-1 default-on validation — the mutual-exclusion invariant is encoded in the wiring at Task 2 Step 2 already; the toggle guard is defense-in-depth.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(objects): guard RAlt+0 legacy toggle when slice 1 is active

Spec R1 mutual-exclusion invariant. While g_useGpuObjects is true, RAlt+0
keystrokes do not flip g_useGpuStaticProps — they are silently ignored
and a one-time [OBJBATCHER v1] event=legacy_toggle_blocked stderr
message is emitted. Default path (slice 1 OFF) preserves the original
toggle behavior.
"
```

---

## Task 4: Stage 1.C — Wire `TreeAppearance::render`

**Purpose:** Extend the same mutual-exclusion wiring to trees. Same pattern as Task 2 — minimal change.

**Files:**
- Modify: `mclib/bdactor.cpp:3984-4045+` (`TreeAppearance::render` body)

- [ ] **Step 1: Locate and read the current branch**

Read `mclib/bdactor.cpp:3984+`. Confirm there's an existing `g_useGpuStaticProps` branch that calls `submitMultiShape` for `treeShape`. If the structure is identical to `BldgAppearance::render`, apply the same edit pattern.

- [ ] **Step 2: Apply mutual-exclusion wiring**

After Task 1.5, the existing pattern around `mclib/bdactor.cpp:4004` is:

```cpp
bool submittedToGpu = false;
if (g_useGpuStaticProps && treeShape)
{
    submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
        treeShape, GpuStaticPropPopulation::Legacy);
}
if (!submittedToGpu) { ... treeShape->Render(...) ... }
```

Replace with:

```cpp
bool submittedToGpu = false;
if (g_useGpuObjects)
{
    GpuStaticPropBatcher::instance().recordEligibleActor(
        GpuStaticPropPopulation::Tree);
    if (treeShape)
    {
        submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
            treeShape, GpuStaticPropPopulation::Tree);
    }
    if (!submittedToGpu)
    {
        GpuStaticPropBatcher::instance().recordCpuFallback(
            GpuStaticPropPopulation::Tree);
    }
}
// Legacy bypass-cull path. Mutually exclusive with slice 1 — gated on
// !g_useGpuObjects. Tagged Legacy so Gate F's fallback-rate is computed
// only over slice-1 populations.
if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && treeShape)
{
    submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
        treeShape, GpuStaticPropPopulation::Legacy);
}
if (!submittedToGpu)
{
    // legacy CPU treeShape->Render(...) — preserve existing branches verbatim
    ...
}
```

If the existing structure differs (e.g., wraps inside a different conditional), preserve the outer logic and only change the `g_useGpuStaticProps` gate to the mutual-exclusion shape. Surface to user if the structure diverges meaningfully.

- [ ] **Step 3: Build, deploy, smoke (default)**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```

Then `/mc2-deploy`.

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
```

Expected: tier1 PASS unchanged.

- [ ] **Step 4: Smoke with slice 1 ON, focus on tree-heavy mission**

mc2_03 and mc2_17 have visible foliage. Run:

```bash
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --mission mc2_17 --duration 20 --kill-existing
```

Expected: trees render correctly, no missing foliage, no streaks, no z-fighting at tree bases. Compare against baseline.

- [ ] **Step 5: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(objects): wire TreeAppearance::render through GPU batcher

Stage 1.C of slice 1, trees population. Same mutual-exclusion pattern as
BldgAppearance::render (Task 2). No new architectural surface.
"
```

---

## Task 5: Stage 1.C — Wire `GenericAppearance::render`

**Purpose:** Extend mutual-exclusion wiring to generics (the third static-prop population). Same pattern.

**Files:**
- Modify: `mclib/genactor.cpp:771+` (`GenericAppearance::render` body)

- [ ] **Step 1: Read current state**

Read `mclib/genactor.cpp:771+`. Confirm the existing `g_useGpuStaticProps` branch shape. Apply the same mutual-exclusion edit pattern.

- [ ] **Step 2: Apply edit**

After Task 1.5, the existing block in `GenericAppearance::render`
(around `mclib/genactor.cpp:802`) is:

```cpp
bool submittedToGpu = false;
if (g_useGpuStaticProps && genShape)
{
    submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
        genShape, GpuStaticPropPopulation::Legacy);
}
```

Replace with:

```cpp
bool submittedToGpu = false;
if (g_useGpuObjects)
{
    GpuStaticPropBatcher::instance().recordEligibleActor(
        GpuStaticPropPopulation::Generic);
    if (genShape)
    {
        submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
            genShape, GpuStaticPropPopulation::Generic);
    }
    if (!submittedToGpu)
    {
        GpuStaticPropBatcher::instance().recordCpuFallback(
            GpuStaticPropPopulation::Generic);
    }
}
if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && genShape)
{
    submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
        genShape, GpuStaticPropPopulation::Legacy);
}
```

Preserve the surrounding CPU-fallback logic verbatim.

- [ ] **Step 3: Build, deploy, smoke (default)**

Same commands as Task 4 Step 3. Expected: tier1 PASS unchanged.

- [ ] **Step 4: Smoke with slice 1 ON, generic-heavy mission**

mc2_10 and mc2_24 have generic-prop content (fences, debris, rocks). Run:

```bash
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --mission mc2_10 --duration 20 --kill-existing
```

Expected: generics render correctly, no regression.

- [ ] **Step 5: Commit**

```bash
git add mclib/genactor.cpp
git commit -m "feat(objects): wire GenericAppearance::render through GPU batcher

Stage 1.C of slice 1, generics population. All three populations
(buildings/trees/generics) now route through g_useGpuObjects when set.
"
```

---

## Task 6: Stage 1.D — Fill counter bodies + summary emission + late-registration

**Purpose:** Implement Gate F by filling the empty stubs from Task 1.5
with real counter logic, emitting `[OBJBATCHER v1] event=summary` every
600 frames AND on shutdown, and adding aggregate late-registration
accounting with allowlist support.

The signature work is already done (Task 1.5). All call sites already
pass a `GpuStaticPropPopulation` tag. This task only fills bodies.

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (fill stubs, add static counter state, increment at submit child sites + flush sites, emit summary at 600-frame cadence and at shutdown; add late-registration aggregate and allowlist load)
- Create: `data/objbatcher_late_register_allowlist.txt` (empty file, comment header only)

- [ ] **Step 1: (already done in Task 1.5 — no-op here)**

The `GpuStaticPropPopulation` enum and `submitMultiShape(shape, pop)`
signature were established in Task 1.5. Step skipped; proceed to Step 2.

Open `GameOS/gameos/gos_static_prop_batcher.h`. After the existing `STATIC_PROP_FLAG_ALPHA_TEST` constant, add:

```cpp
// Population tag — passed by caller so the batcher can split per-population
// counts in the [OBJBATCHER v1] summary. Not stored; consumed inside submit
// only.
//
// Legacy is the slice-1 fallback branch (g_useGpuStaticProps && !g_useGpuObjects)
// in *Appearance::render. Counted separately so Gate F's fallback-rate
// computation uses only slice-1 populations (Building/Tree/Generic).
enum class GpuStaticPropPopulation : uint8_t {
    Building = 0,
    Tree     = 1,
    Generic  = 2,
    Legacy   = 3,
};
```

Then add caller-side accounting helpers AND change the `submitMultiShape`
declaration to take a `GpuStaticPropPopulation pop` parameter. The
helpers are called from `*Appearance::render` BEFORE submit so that
caller-side bypasses (e.g., `bldgShape == nullptr`) are still counted
as "actor reached render with slice 1 enabled."

```cpp
class GpuStaticPropBatcher {
public:
    // ... existing members ...

    // Caller-side accounting. Call recordEligibleActor() once per actor
    // that reaches *Appearance::render with g_useGpuObjects enabled (or
    // its g_useGpuStaticProps Legacy equivalent). If submit later
    // returns false OR is bypassed (e.g., null shape), the same caller
    // calls recordCpuFallback() with the same population tag. This
    // keeps fallback_rate honest even when a caller-side condition
    // skips the batcher entirely.
    void recordEligibleActor(GpuStaticPropPopulation pop);
    void recordCpuFallback(GpuStaticPropPopulation pop);

    [[nodiscard]] bool submitMultiShape(TG_MultiShape* multi,
                                        GpuStaticPropPopulation pop);
};
```

- [ ] **Step 2: Add counter state in batcher .cpp**

Open `GameOS/gameos/gos_static_prop_batcher.cpp`. Add the static counter
state near the top of the file (above the empty-stub helper bodies
established in Task 1.5 Step 2):

```cpp
namespace {
    // [OBJBATCHER v1] Gate F counters. Reset at frame boundary; aggregated
    // over the 600-frame summary window via the monotonic accumulators below.
    //
    // Population indexed by GpuStaticPropPopulation enum value: 0=Building,
    // 1=Tree, 2=Generic, 3=Legacy. Legacy is excluded from Gate F's
    // fallback-rate computation (slice-1 populations only).
    constexpr int kPopCount = 4;

    struct ObjBatcherCounters {
        // Per-frame, indexed by population (0=Building,1=Tree,2=Generic,3=Legacy):
        uint32_t eligible_actors_by_pop[kPopCount]     = {0};
        uint32_t submitted_instances_by_pop[kPopCount] = {0};  // successful submit only
        uint32_t cpu_fallback_by_pop[kPopCount]        = {0};
        uint32_t submitted_children                    = 0;
        uint32_t skipped_children                      = 0;
        // Per-flush (filled in by flush() before frame reset):
        uint32_t gpu_drawn_instances                   = 0;
        // Monotonic since process start, indexed same way:
        uint64_t mono_eligible_actors_by_pop[kPopCount]     = {0};
        uint64_t mono_submitted_instances_by_pop[kPopCount] = {0};
        uint64_t mono_cpu_fallback_by_pop[kPopCount]        = {0};
        uint64_t mono_submitted_children                = 0;
        uint64_t mono_skipped_children                  = 0;
        uint64_t mono_gpu_drawn_instances               = 0;
        uint64_t frame_count                            = 0;
    };
    ObjBatcherCounters s_counters;
    bool s_objbatcherTrace = false;  // set from MC2_OBJBATCHER_TRACE env at first call
    bool s_objbatcherTraceInit = false;

    inline int popIndex(GpuStaticPropPopulation pop) {
        return static_cast<int>(pop);  // {0,1,2,3}
    }

    inline void initTraceOnce() {
        if (!s_objbatcherTraceInit) {
            s_objbatcherTrace = (getenv("MC2_OBJBATCHER_TRACE") != nullptr);
            s_objbatcherTraceInit = true;
        }
    }
}
```

- [ ] **Step 3: Fill the empty-stub helpers from Task 1.5**

Locate the empty stubs added in Task 1.5 Step 2:

```cpp
void GpuStaticPropBatcher::recordEligibleActor(GpuStaticPropPopulation pop) {
    (void)pop;  // body filled in Task 6.
}

void GpuStaticPropBatcher::recordCpuFallback(GpuStaticPropPopulation pop) {
    (void)pop;  // body filled in Task 6.
}
```

Replace with:

```cpp
void GpuStaticPropBatcher::recordEligibleActor(GpuStaticPropPopulation pop) {
    initTraceOnce();
    s_counters.eligible_actors_by_pop[popIndex(pop)]++;
}

void GpuStaticPropBatcher::recordCpuFallback(GpuStaticPropPopulation pop) {
    s_counters.cpu_fallback_by_pop[popIndex(pop)]++;
}
```

- [ ] **Step 4: Weave per-child + per-instance counters into submitMultiShape body**

The existing `submitMultiShape` body at `gos_static_prop_batcher.cpp:477`
(now two-arg per Task 1.5) has the structure (verified by grep at plan
write-time):

- Lines 478-482: early returns on null/zero/program-fail conditions.
- Lines 497-518: **first pass** — pre-flight check for unregistered
  SHAPE_NODE types. `return false` at line 516 if any unregistered
  type found. (This is the CURRENT late-registration log site that
  Step 8 below replaces.)
- Lines 520-551: **second pass** — submit each child, with multiple
  `continue;` sites for ineligibility, and `return false` at line 549
  on submit-buffer-full.
- Line 552: `return true;` at end of function.

Add increments at these specific lines (DO NOT refactor the existing
control flow; just sprinkle counter increments):

1. **At line 524 (`if (!rec.processMe || !rec.node) continue;`):**
   change to:
   ```cpp
   if (!rec.processMe || !rec.node) { s_counters.skipped_children++; continue; }
   ```
2. **At line 526 (`if (!child->myType) continue;`):**
   ```cpp
   if (!child->myType) { s_counters.skipped_children++; continue; }
   ```
3. **At line 529 (`if (child->myType->GetNodeType() != SHAPE_NODE) continue;`):**
   ```cpp
   if (child->myType->GetNodeType() != SHAPE_NODE) {
       s_counters.skipped_children++;
       continue;
   }
   ```
4. **At line 535 (`if (!child->listOfVertices || !child->listOfColors) continue;`):**
   ```cpp
   if (!child->listOfVertices || !child->listOfColors) {
       s_counters.skipped_children++;
       continue;
   }
   ```
5. **After line 545's successful `submit(...)` call:** add
   ```cpp
   s_counters.submitted_children++;
   ```
   right before the closing brace of the second-pass loop iteration
   (after the `submit(...)` returned true).
6. **At line 552 (`return true;`):** prepend
   ```cpp
   s_counters.submitted_instances_by_pop[popIndex(pop)]++;
   ```
   This fires only on successful whole-multishape submit, never before
   the result is known.
7. **At line 549's `return false;`** (submit-buffer-full case): no
   counter change here. The CALLER's `recordCpuFallback()` covers it.
8. **At line 516's `return false;`** (unregistered-type whole-multishape
   fallback): no counter change here either — same reason.

`initTraceOnce()` is called at the top of the function; it's safe to
call repeatedly (idempotent guard).

Do NOT delete the `s_warned_unregistered` static or the `fprintf` at
lines 497-516 in this step — those are replaced in Step 8 with the
aggregate-counted form. Keeping them around for this step is fine.

- [ ] **Step 5: Add `gpu_drawn_instances` increment in `flush()`**

The `flush()` body (verified at `gos_static_prop_batcher.cpp:760-811`)
has a per-type outer loop AND a per-packet inner loop. We want
`gpu_drawn_instances` to count actors-that-drew-something, not
instance-times-packets. Add the increment at the **outer (per-type)**
level, AFTER the `if (r.instanceCount == 0 || type.packetCount == 0)
continue;` guard at line 765 and BEFORE the inner packet loop:

```cpp
        if (r.instanceCount == 0 || type.packetCount == 0) continue;

        // Gate F: count actors that produced ≥1 packet draw this frame.
        // Per-type increment, not per-packet — we want "actors drawn,"
        // not "draw-call count."
        s_counters.gpu_drawn_instances += r.instanceCount;
```

The variable is `r.instanceCount` (member of `TypeRangeSsbo` resolved
via `s_typeRanges.find(typeID)`), NOT `instanceCountForOwningType`
(which doesn't exist).

- [ ] **Step 6: Emit summary at end of flush every 600 frames**

At the END of `flush()`, after all draws, add a helper call that
both accumulates monotonic totals and (conditionally) emits the
summary line. Define the helper near the counter state in Step 2:

```cpp
namespace {
    void accumulateMonotonicAndMaybeEmit(bool forceEmit) {
        s_counters.frame_count++;
        for (int p = 0; p < kPopCount; ++p) {
            s_counters.mono_eligible_actors_by_pop[p]     += s_counters.eligible_actors_by_pop[p];
            s_counters.mono_submitted_instances_by_pop[p] += s_counters.submitted_instances_by_pop[p];
            s_counters.mono_cpu_fallback_by_pop[p]        += s_counters.cpu_fallback_by_pop[p];
        }
        s_counters.mono_submitted_children    += s_counters.submitted_children;
        s_counters.mono_skipped_children      += s_counters.skipped_children;
        s_counters.mono_gpu_drawn_instances   += s_counters.gpu_drawn_instances;

        const bool periodic = (s_counters.frame_count % 600 == 0
                               && s_counters.frame_count > 0);
        if (s_objbatcherTrace || periodic || forceEmit) {
            // Slice-1 fallback rate uses ONLY Building+Tree+Generic
            // populations (Legacy excluded — it's the prior killswitch path).
            uint64_t slice1_eligible = 0, slice1_fallback = 0;
            for (int p = 0; p <= 2; ++p) {  // Building, Tree, Generic
                slice1_eligible += s_counters.mono_eligible_actors_by_pop[p];
                slice1_fallback += s_counters.mono_cpu_fallback_by_pop[p];
            }
            const double fb_rate = (slice1_eligible > 0)
                ? (double)slice1_fallback / (double)slice1_eligible
                : 0.0;

            uint64_t total_submitted = 0;
            for (int p = 0; p < kPopCount; ++p) {
                total_submitted += s_counters.mono_submitted_instances_by_pop[p];
            }

            printf("[OBJBATCHER v1] event=summary frames=%llu "
                   "eligible_actors=%llu submitted_instances=%llu "
                   "submitted_children=%llu skipped_children=%llu "
                   "cpu_fallback=%llu gpu_drawn_instances=%llu "
                   "fallback_rate=%.4f "
                   "submit_buildings=%llu submit_trees=%llu submit_generics=%llu "
                   "submit_legacy=%llu\n",
                   (unsigned long long)s_counters.frame_count,
                   (unsigned long long)slice1_eligible,
                   (unsigned long long)total_submitted,
                   (unsigned long long)s_counters.mono_submitted_children,
                   (unsigned long long)s_counters.mono_skipped_children,
                   (unsigned long long)slice1_fallback,
                   (unsigned long long)s_counters.mono_gpu_drawn_instances,
                   fb_rate,
                   (unsigned long long)s_counters.mono_submitted_instances_by_pop[0],
                   (unsigned long long)s_counters.mono_submitted_instances_by_pop[1],
                   (unsigned long long)s_counters.mono_submitted_instances_by_pop[2],
                   (unsigned long long)s_counters.mono_submitted_instances_by_pop[3]);
            fflush(stdout);
        }

        // Reset per-frame counters for next frame.
        for (int p = 0; p < kPopCount; ++p) {
            s_counters.eligible_actors_by_pop[p]     = 0;
            s_counters.submitted_instances_by_pop[p] = 0;
            s_counters.cpu_fallback_by_pop[p]        = 0;
        }
        s_counters.submitted_children = 0;
        s_counters.skipped_children   = 0;
        s_counters.gpu_drawn_instances = 0;
    }
}
```

Then at the end of `GpuStaticPropBatcher::flush()`:

```cpp
    accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
```

- [ ] **Step 7: Add explicit shutdown summary**

Find the batcher's teardown / process-exit hook. The existing
`onMapUnload()` in the batcher header (line ~63) is per-mission, NOT
process-exit; that's wrong for shutdown summary. Use `atexit()` to
register a one-time hook from the batcher's first-use site
(`instance()` method or first `submitMultiShape` call):

```cpp
namespace {
    void emitFinalSummaryAtExit() {
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/true);
    }
    bool s_atexitRegistered = false;
}
```

In `initTraceOnce()`, after setting `s_objbatcherTraceInit = true`:

```cpp
        if (!s_atexitRegistered) {
            s_atexitRegistered = true;
            atexit(emitFinalSummaryAtExit);
        }
```

Result: regardless of frame count, the final `[OBJBATCHER v1] event=summary`
fires at process exit. Required for Task 7's gate-F validation when
smoke runs short.
    s_counters.mono_submit_trees           += s_counters.submit_trees;
    s_counters.mono_submit_generics        += s_counters.submit_generics;

    if (s_objbatcherTrace ||
        (s_counters.frame_count % 600 == 0 && s_counters.frame_count > 0))
    {
        const double fb_rate = (s_counters.mono_eligible_actors > 0)
            ? (double)s_counters.mono_cpu_fallback_instances /
              (double)s_counters.mono_eligible_actors
            : 0.0;
        printf("[OBJBATCHER v1] event=summary frames=%llu "
               "eligible_actors=%llu submitted_instances=%llu "
               "submitted_children=%llu skipped_children=%llu "
               "cpu_fallback_instances=%llu gpu_drawn_instances=%llu "
               "fallback_rate=%.4f "
               "submit_buildings=%llu submit_trees=%llu submit_generics=%llu\n",
               (unsigned long long)s_counters.frame_count,
               (unsigned long long)s_counters.mono_eligible_actors,
               (unsigned long long)s_counters.mono_submitted_instances,
               (unsigned long long)s_counters.mono_submitted_children,
               (unsigned long long)s_counters.mono_skipped_children,
               (unsigned long long)s_counters.mono_cpu_fallback_instances,
               (unsigned long long)s_counters.mono_gpu_drawn_instances,
               fb_rate,
               (unsigned long long)s_counters.mono_submit_buildings,
               (unsigned long long)s_counters.mono_submit_trees,
               (unsigned long long)s_counters.mono_submit_generics);
        fflush(stdout);
    }

    // Reset per-frame counters for next frame.
    s_counters.eligible_actors        = 0;
    s_counters.submitted_instances    = 0;
    s_counters.submitted_children     = 0;
    s_counters.skipped_children       = 0;
    s_counters.cpu_fallback_instances = 0;
    s_counters.gpu_drawn_instances    = 0;
    s_counters.submit_buildings       = 0;
    s_counters.submit_trees           = 0;
    s_counters.submit_generics        = 0;
```

- [ ] **Step 8: Add late-registration aggregate (replaces existing single-shot warning)**

The current code at `gos_static_prop_batcher.cpp:497-518` has a
single-shot `static bool s_warned_unregistered` + one-time
`fprintf(stderr, "[GPUPROPS] multi=%p child %d: unregistered type %p ...")`.
**Step 8 deletes that static + fprintf and replaces with the aggregate
form below.**

Add an aggregate `std::unordered_map<std::string, uint32_t>` in the
same anonymous namespace as the counter state from Step 2:

```cpp
    std::unordered_map<std::string, uint32_t> s_lateRegisterCounts;
    std::unordered_set<std::string> s_lateRegisterAllowlist;
    bool s_lateRegisterAllowlistLoaded = false;

    void loadLateRegisterAllowlistOnce() {
        if (s_lateRegisterAllowlistLoaded) return;
        s_lateRegisterAllowlistLoaded = true;
        FILE* f = fopen("data/objbatcher_late_register_allowlist.txt", "r");
        if (!f) return;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            // Strip the # comment delimiter and the trailing newline.
            char* p = line;
            while (*p && *p != '\n' && *p != '\r' && *p != '#') ++p;
            *p = '\0';
            // Trim leading whitespace.
            char* start = line;
            while (*start == ' ' || *start == '\t') ++start;
            // Trim trailing whitespace (after the comment trim).
            char* end = start + strlen(start);
            while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
                --end;
            }
            *end = '\0';
            if (*start == '\0') continue;
            s_lateRegisterAllowlist.insert(start);
        }
        fclose(f);
    }
```

At the late-registration detection site, the existing block:

```cpp
        if (s_typeIndex.find(ts) == s_typeIndex.end()) {
            if (!s_warned_unregistered) {
                std::fprintf(stderr,
                    "[GPUPROPS] multi=%p child %d: unregistered type %p -- "
                    "CPU-fallback whole multishape\n",
                    (void*)multi, i, (void*)ts);
                s_warned_unregistered = true;
            }
            return false;
        }
```

Becomes:

```cpp
        if (s_typeIndex.find(ts) == s_typeIndex.end()) {
            loadLateRegisterAllowlistOnce();
            // Resolve a stable name. TG_TypeShape doesn't expose a name
            // directly here; use the address as a stand-in and log the
            // child's source-file annotation if available. If a real
            // name accessor exists in TG_TypeShape (grep at
            // implementation time), prefer it.
            char addrBuf[32];
            snprintf(addrBuf, sizeof(addrBuf), "%p", (const void*)ts);
            const std::string typeName = addrBuf;
            auto& count = s_lateRegisterCounts[typeName];
            if (count == 0) {
                const bool allowed =
                    (s_lateRegisterAllowlist.find(typeName)
                     != s_lateRegisterAllowlist.end());
                printf("[OBJBATCHER v1] event=late_register type=%s allowed=%d\n",
                       typeName.c_str(), allowed ? 1 : 0);
                fflush(stdout);
            }
            ++count;
            return false;
        }
```

Also delete the now-unused `static bool s_warned_unregistered = false;`
declaration at line 497 (or just before — verify location at
implementation time).

**Note on type names:** the current code logs `(void*)ts` (pointer
address) because `TG_TypeShape` doesn't appear to have a `getName()`
accessor in the existing log. Pointer-stringification is functional
(addresses are stable per process) but not human-friendly. If the
implementation agent finds a real name accessor (e.g.,
`ts->source->name` or similar), prefer it. Otherwise the pointer
form is fine for slice-1 gates.

(Caller-side migration to two-arg `submitMultiShape` was already done
in Task 1.5 + Tasks 2/4/5; no additional caller updates needed in
this task.)

- [ ] **Step 9: Create empty allowlist file**

```bash
mkdir -p data
```

Then write `data/objbatcher_late_register_allowlist.txt` with content:

```
# Object batcher late-registration allowlist.
# One TG_TypeShape name (or pointer-form) per line. # for comments.
# Initial state: empty (strict mode). Add entries only after observing
# in [OBJBATCHER v1] event=late_register output AND confirming via
# grep that the type is genuinely out-of-scope (e.g., late-spawned
# artillery from code/artlry.cpp). Do not add speculatively.
```

**Deployment note:** the runtime CWD is the deploy directory
(`A:/Games/mc2-opengl/mc2-win64-v0.3/`), so `fopen("data/...")`
reads from `mc2-win64-v0.3/data/...`. The `/mc2-deploy` skill
already copies the source-tree `data/` → deploy `data/` (verify
by inspecting the skill if uncertain). If the deploy mechanism
doesn't copy this file, runtime fopen returns NULL and
`loadLateRegisterAllowlistOnce` treats the allowlist as empty —
which is exactly strict mode, so the failure is graceful and
matches intent. The Gate F log line is what matters.

- [ ] **Step 10: Build, deploy**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```

Then `/mc2-deploy`.

- [ ] **Step 11: Smoke — Gate F validation**

```bash
MC2_GPU_OBJECTS=1 MC2_OBJBATCHER_TRACE=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 20 --kill-existing
```

Inspect run artifact's stderr/stdout for `[OBJBATCHER v1] event=summary`. Expected (numbers will vary):

```
[OBJBATCHER v1] event=summary frames=600
  eligible_actors=NNNN submitted_instances=NNNN
  submitted_children=NNNN skipped_children=NN
  cpu_fallback_instances=NN gpu_drawn_instances=NNNN
  fallback_rate=0.0XXX
  submit_buildings=NNN submit_trees=NN submit_generics=NN
```

Gate (per spec):
- `gpu_drawn_instances > 0`
- `submitted_children > 0`
- `fallback_rate < 0.05`
- `submit_buildings > 0` AND `submit_trees > 0` AND `submit_generics > 0`

If any of these miss, surface to user — slice has a structural problem. If `fallback_rate ≈ 1.0`, the prior Layer-B failure has re-surfaced — investigate `submitMultiShape` per-child loop semantics.

- [ ] **Step 12: Smoke — late-registration coverage**

In the same run output, search for `event=late_register`. Expected: zero entries OR all entries' type names are in the (currently empty) allowlist (so `allowed=0` for every entry → fail the gate).

If any `event=late_register` entries appear with `allowed=0`, surface the type names to the user. They must either:
- be added to the allowlist with explicit reasoning (e.g., known late-spawned artillery), OR
- be fixed at the registration site (uncommon for slice 1)

- [ ] **Step 13: Tier1 5/5 PASS triple gate**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
MC2_GPU_OBJECTS=1 MC2_OBJBATCHER_TRACE=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 8 --fail-fast
```

All three: exit `0`. +0 destroys delta vs Task 0 baseline.

- [ ] **Step 14: TGL pool peak gate (Gate E)**

Compare `[TGL_POOL v1] summary` line at shutdown of the slice-1-ON run vs Task 0 baseline. Expected: same or lower peak. If peak rose, surface to user — the slice has accidentally bypassed cull somewhere. Investigation required before commit.

- [ ] **Step 15: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp \
        mclib/bdactor.cpp mclib/genactor.cpp \
        data/objbatcher_late_register_allowlist.txt
git commit -m "feat(objects): Gate F counters + late-registration accounting

Stage 1.D of slice 1. Adds [OBJBATCHER v1] event=summary every 600 frames
with eligible_actors / submitted_instances / submitted_children /
skipped_children / cpu_fallback_instances / gpu_drawn_instances /
fallback_rate / per-population counts. Late-registration accounting is
aggregate per-type with allowlist support
(data/objbatcher_late_register_allowlist.txt — strict-mode empty
initially). MC2_OBJBATCHER_TRACE=1 enables per-frame trace; default emits
600-frame summary regardless.

Gates spec section 'Test plan / gate ladder' Gate F + late-registration.
"
```

---

## Task 7: Final validation gate ladder

**Purpose:** Run the full gate ladder per spec test plan section. Confirms slice 1 is ready for flagged merge.

**Files:** none modified.

- [ ] **Step 1: Visual canary at fixed camera**

Pick mc2_01 (airbase, building-heavy, mostly static). Capture screenshot at fixed camera with `MC2_GPU_OBJECTS=0` baseline and `MC2_GPU_OBJECTS=1`. Compare:

```bash
py -3 scripts/run_smoke.py --mission mc2_01 --duration 20 --kill-existing
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 20 --kill-existing
```

Diff screenshots in `tests/smoke/artifacts/<run>/`. Expected: visually equivalent. Per spec, the **pinned-camera screenshot diff harness** (Stage 1.E) is a separate PR and gates default-on, NOT flagged merge — visual comparison here is by-eye.

- [ ] **Step 2: Tracy delta on `Render.3DObjects` zone**

Launch with Tracy connected:

```bash
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 60 --kill-existing
```

Connect Tracy GUI, capture, inspect `Render.3DObjects` mean over the steady-state window. Compare against same recipe with `MC2_GPU_OBJECTS=0`.

Expected: **no regression** (didn't make it worse). A reduction is welcome but not gated. Per spec: slice 1's framing is substrate, not perf.

If `Render.3DObjects` regressed materially (>5% slower), surface to user. The slice does not pass the gate.

- [ ] **Step 3: Full tier1 5/5 PASS triple, long duration**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing
MC2_GPU_OBJECTS=1 MC2_OBJBATCHER_TRACE=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

(Note: no `--duration` override — full default 25s per mission.)

All three: exit `0`. Compare `[DESTROY v1]` summaries — +0 destroys delta on every mission, every state.

- [ ] **Step 4: Gate F final check**

From the third run's stdout, capture the final `[OBJBATCHER v1] event=summary` per mission. The summary is also force-emitted at process exit per Task 6 Step 7 (`atexit` hook), so it's available even on short runs. All five missions:
- `gpu_drawn_instances > 0`
- `submitted_children > 0`
- `fallback_rate < 0.05` (computed over slice-1 populations only — Legacy excluded by construction)
- `submit_buildings > 0 && submit_trees > 0 && submit_generics > 0`
- `submit_legacy == 0` (slice 1 wins; the legacy fallback path is unreachable when `MC2_GPU_OBJECTS=1`. If `submit_legacy > 0` here, the mutual-exclusion R1 invariant has leaked.)
- Sanity: `submit_buildings + submit_trees + submit_generics + submit_legacy == submitted_instances`

- [ ] **Step 5: Gate E final check**

Compare the `[TGL_POOL v1] summary` peak across the three runs. Slice-1-ON peaks must be ≤ baseline peak.

- [ ] **Step 6: Late-registration final check**

Search the third run's stdout for `event=late_register`. Either zero entries OR every entry has a typename in the allowlist (currently empty, so any entry fails strict mode).

If type names appear, surface to user with the recipe:

> [OBJBATCHER v1] event=late_register type=Foo allowed=0
>
> Foo is appearing late. Either add `Foo` to data/objbatcher_late_register_allowlist.txt (with reasoning in commit message) OR investigate the registration site. Slice 1 cannot ship if the gate is red.

- [ ] **Step 7: Slice-1 ready for flagged merge**

If all gates above pass, slice 1 is ready to merge. The PR description must:
- Frame slice 1 as substrate, not perf — no Tracy delta claim
- Cite spec R1 (mutual exclusion) as the central safety property
- Cite Gate F counters as the prior-Layer-B-failure-mode countermeasure
- Note that default-on flip is gated on Stage 1.E (pinned-camera diff harness, separate PR) OR slice 2 landing — neither in this PR

No additional commit at this task; just the validation summary written into the PR description.

---

## Out-of-scope for this plan (per spec)

- **Stage 1.E (pinned-camera screenshot diff harness):** separate PR.
  Gates default-on flip; does not gate slice-1 flagged merge.
- **Slice 2 (GPU vertex lighting):** gated on Recon Zero
  (`TG_Shape::listOfShadowTVertices` consumer enumeration). Not in
  this plan.
- **Default-on flip:** not in this plan. Requires either Stage 1.E
  or slice 2 to land first.
- **Removal of legacy `g_useGpuStaticProps` and the five cull-bypass
  sites:** post-arc cleanup, separate slice. Not in this plan.

---

## Self-review (against spec)

| Spec section | Plan task |
|---|---|
| Stage 1.A — Infrastructure rename (env flag) | Task 1 |
| Stage 1.A.bis — Refactor signature to take population tag (no-op) | Task 1.5 |
| Stage 1.B — Wire `BldgAppearance::render` | Task 2 |
| Stage 1.B — RAlt+0 toggle guard (R1) | Task 3 |
| Stage 1.C — Wire trees + generics | Tasks 4 + 5 |
| Stage 1.D — Counters + late-registration | Task 6 |
| Stage 1.E — Pinned-camera diff | Out of scope (separate PR per spec) |
| Eligibility predicate (per-child, multishape batchable iff ≥1 child) | Task 6 Step 4 (counter increments encode this) |
| Caller-side accounting (`recordEligibleActor`/`recordCpuFallback`) | Task 1.5 stubs + Task 6 Step 3 fills + Tasks 2/4/5 callers |
| Gate A (visual canary) | Task 7 Step 1 |
| Gate B (Tracy no-regression on render) | Task 7 Step 2 |
| Gate C (parity / Gate F counters) | Task 6 + Task 7 Step 4 |
| Gate D (tier1 5/5 PASS triple) | Task 7 Step 3 |
| Gate E (TGL pool peak ≤ baseline) | Task 2 Step 6 + Task 6 Step 14 + Task 7 Step 5 |
| Gate F (GPU drew something + fallback rate) | Task 6 Step 11 + Task 7 Step 4 |
| Late-registration coverage | Task 6 Step 12 + Task 7 Step 6 |
| Shutdown summary (atexit force-emit) | Task 6 Step 7 |
| R1 invariant (mutual exclusion) | Tasks 2 + 3 enforce it; Gate F's `submit_legacy == 0` verifies it; surfaced in PR description |

No gaps identified. No placeholders. Type signatures consistent
end-to-end (Task 1.5 establishes; Tasks 2/4/5 use; Task 6 fills
counter bodies):
- `submitMultiShape(TG_MultiShape*, GpuStaticPropPopulation)`
- `GpuStaticPropPopulation::{Building, Tree, Generic, Legacy}`
- `recordEligibleActor(GpuStaticPropPopulation)`
- `recordCpuFallback(GpuStaticPropPopulation)`
