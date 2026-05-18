# C2 Armed-Attribution + Eligibility/Resume Disambiguation Probe — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land one env-gated instrumentation commit that (1) isolates the armed per-frame cost of the three `addLightDataStructure` callsites (C2-direct, C6-resubmit, C5-per-actor) that the existing Tracy `addLightDataStructure scan` zone conflates, and (2) disambiguates which resume path the heaviest mission uses and whether terrain-object `TG_TypeShape`s ever reach `registerType()` on it.

**Architecture:** A new RDTSC-based 3-bucket per-frame accumulator (NOT `std::chrono`, NOT a per-call Tracy zone — both falsified for this call density), gated by a NEW env var `MC2_LIGHT_COST_SPLIT` (deliberately NOT the falsified `MC2_TERRAIN_COST_SPLIT` gate), emitting one summary line per 600-frame cadence. Reuses the mechanical `CostSplit_RollFrame()` integration point but a distinct gate and distinct accumulators. The resume-path question reuses the existing `[MECHRESTORE v1] event=saveload_phase` markers (zero work) plus one new `[GPUPROPS v1]`-gated registration-attempt counter tagged by setup path.

**Tech Stack:** C++ (MSVC RelWithDebInfo), `__rdtsc()` intrinsic, existing `gos_terrain_indirect.{h,cpp}` CostSplit module pattern, env-gated stderr markers per the project debug-instrumentation-rule.

---

## Why this slice exists (read before touching code)

Both advisors converged: the C2 residual is fork **(a) mis-gating**, NOT an inherent class. But two hard gates block ANY C2 fix design or done-claim:

1. **Armed cost-attribution gate** (`memory/verify_producer_path_against_telemetry_before_substitution.md`): the `addLightDataStructure scan` Tracy zone conflates C2 (`mclib/tgl.cpp:3113`), C6 (`mclib/tgl.cpp:2865`), and C5 (`mclib/tgl.cpp:2860`). C2's armed self-cost must NOT be inferred from the legacy branch structure. No C2 slice may be designed or done-claimed until this lands a number.
2. **Resume-path / registration disambiguation gate** (`memory/parallel_mission_setup_paths_probe_which_one.md`): the mission-data recon refuted the simple "late-register on resume" hypothesis for terrain objects on the first-load and SP_LOGISTICS paths. The open pivot: `GpuStaticPropBatcher::registerType` (the static-prop/terrain-object batcher, `gos_static_prop_batcher.cpp:~1063`) is reached ONLY via `ObjectManager->registerStaticPropsForMissionLoad()` called at `code/mission.cpp:~2880` **inside `Mission::init`** (RE-GREP). `Mission::load` (the `.ims` path, `code/saveload.cpp`) does NOT call it; its respawn (`ObjectManager::Load` ~`saveload.cpp:1391` → `objmgr.cpp:~1362-1421`) is a distinct deserialization path. **Disambiguation precision (M-3):** `registerTypeLod` referenced in saveload.cpp is `GpuMechBatcher::registerTypeLod` (the MECH batcher, `gos_mech_batcher.cpp:~486`) — a DIFFERENT function/batcher, out of scope here; do not conflate with `GpuStaticPropBatcher::registerType`. The fix shape (recover-late-registration vs ensure-registration-on-resume) is undetermined until probe-confirmed: a path=2 (`.ims`) attempt count of 0 confirms `Mission::load` skips the static-prop walk → Plan B branch B2.

This slice is pure env-gated instrumentation: zero behavior change in stock builds, no shader edits (C++-only deploy), kill-switch is the env var being unset.

## Verified anchor points (grep-verified 2026-05-17 @ this worktree HEAD — RE-GREP at execution; lines drift)

- C2-direct callsite: `mclib/tgl.cpp:3113` `rs.light_data_buffer_index_ = mcTextureManager->addLightDataStructure(&lightData_);` inside the `!eligibleForGpuObjects` legacy-leaf branch (gate at `mclib/tgl.cpp:2604`).
- C6-resubmit callsite: `mclib/tgl.cpp:2865` `return mcTextureManager->addLightDataStructure(&lightData_);` inside `TG_Shape::ResubmitCachedLightData()` (fn opens `mclib/tgl.cpp:2863`).
- C5-per-actor callsite: `mclib/tgl.cpp:2860` `return mcTextureManager->addLightDataStructureWithPerActorColor(&lightData_);` inside `TG_Shape::GatherGpuObjectLightDataOnly()` (fn opens `mclib/tgl.cpp:2858`).
- CostSplit module pattern: `GameOS/gameos/gos_terrain_indirect.h` — `void CostSplit_AddXxxNanos(long long n);` / `long long CostSplit_GetXxxNanosTotal();` / `void CostSplit_RollFrame();` / `bool IsCostSplitEnabled();` (gate `MC2_TERRAIN_COST_SPLIT`). Definitions in `GameOS/gameos/gos_terrain_indirect.cpp` (RE-GREP for the `CostSplit_RollFrame` body + summary emit site).
- `CostSplit_RollFrame()` per-frame integration boundary: documented at the `terrain.cpp:1684` boundary (RE-GREP `CostSplit_RollFrame` callsite in `terrain.cpp`).
- Existing resume-path markers (reuse, no new code): `code/saveload.cpp:702` `phase=post_destroy`, `:1398` `phase=post_objmgr_load`, `:1590` `phase=post_finalize`, all gated `MC2_MECH_RESTORE_TRACE`.
- Existing late-register marker: `GameOS/gameos/gos_static_prop_batcher.cpp:1078` `[GPUPROPS] late registerType for %p`. `registerType` body opens `:1063`; `s_geometryFinalized` gate `~:1075`; `registerMultiShape` `~:1191`; `eligibleForGpuObjects` free fn `:3806`; `isMultiShapeEligibleForGpuObjects` `:3778` (false-return `:3793`).
- Smoke env allowlist: `scripts/run_smoke.py` (RE-GREP for the env-passthrough list; new env vars must be added or the child process drops them).

---

## File Structure

- **Modify** `GameOS/gameos/gos_terrain_indirect.h` — declare the 3 new light-cost buckets (Add/Get nanos + Add/Get calls) and the new gate accessor `IsLightCostSplitEnabled()`.
- **Modify** `GameOS/gameos/gos_terrain_indirect.cpp` — define the RDTSC accumulators, the `MC2_LIGHT_COST_SPLIT` gate, the per-frame roll + summary-line emit, hooked off the existing `CostSplit_RollFrame()` path.
- **Modify** `mclib/tgl.cpp` — bracket the three callsites (`:3113`, `:2865`, `:2860`) with an RDTSC scope that feeds the matching bucket. RAII scope, cached-bool early-out, no work when gate off.
- **Modify** `GameOS/gameos/gos_static_prop_batcher.cpp` — add a `[GPUPROPS v1]`-gated registration-attempt counter in `registerType()` tagged with the current setup-path tag; emit a summary at `finalizeGeometry()`.
- **Modify** `code/saveload.cpp` + `code/mission.cpp` — set a global setup-path tag (`g_lightProbeSetupPath`) at each path entry so the registration counter can attribute attempts to first-load vs `.ims` vs SP_LOGISTICS. (Tag only — no logic change.)
- **Modify** `scripts/run_smoke.py` — add `MC2_LIGHT_COST_SPLIT` and ensure `MC2_MECH_RESTORE_TRACE` + `MC2_GPUPROPS_TRACE` pass through to the child.

No new files. All additions are env-gated and demote-not-delete per the debug-instrumentation-rule.

---

### Task 1: Declare the light-cost-split buckets and gate

**Files:**
- Modify: `GameOS/gameos/gos_terrain_indirect.h` (append near the existing `CostSplit_*` declarations, RE-GREP for the block ~lines 86-150)

- [ ] **Step 1: Add declarations**

Append to the CostSplit declaration block in `gos_terrain_indirect.h`:

```cpp
// --- [LIGHT_COST_SPLIT v1] -------------------------------------------------
// Separate gate from MC2_TERRAIN_COST_SPLIT (that gate's chrono scopes are
// observer-effect-dominated and the terrain-CPU campaign is terminal —
// memory/cost_split_instrumentation_is_observer_effect_dominated.md).
// These buckets use __rdtsc() and a distinct env var so they can run on a
// legacy-object mission without the falsified terrain scopes attached.
// Three buckets isolate the three addLightDataStructure callsites the Tracy
// "addLightDataStructure scan" zone conflates:
//   C2-direct  : mclib/tgl.cpp:3113  (!eligibleForGpuObjects legacy leaf)
//   C6-resubmit: mclib/tgl.cpp:2865  (TG_Shape::ResubmitCachedLightData)
//   C5-peractor: mclib/tgl.cpp:2860  (TG_Shape::GatherGpuObjectLightDataOnly)
bool IsLightCostSplitEnabled();              // MC2_LIGHT_COST_SPLIT
void LightCostSplit_AddC2DirectCycles(unsigned long long c);
void LightCostSplit_AddC6ResubmitCycles(unsigned long long c);
void LightCostSplit_AddC5PerActorCycles(unsigned long long c);
void LightCostSplit_AddC2DirectCall();
void LightCostSplit_AddC6ResubmitCall();
void LightCostSplit_AddC5PerActorCall();
void LightCostSplit_RollFrameAndMaybeEmit();  // call from CostSplit_RollFrame()
```

- [ ] **Step 2: Verify it compiles in isolation (header-only sanity)**

Run: `grep -n "LIGHT_COST_SPLIT v1" GameOS/gameos/gos_terrain_indirect.h`
Expected: the marker line prints; no other source references it yet (definitions land in Task 2).

- [ ] **Step 3: Commit**

```bash
git add GameOS/gameos/gos_terrain_indirect.h
git commit -m "instr: declare [LIGHT_COST_SPLIT v1] 3-bucket RDTSC light-cost gate"
```

---

### Task 2: Define the RDTSC accumulators + per-frame summary emit

**Files:**
- Modify: `GameOS/gameos/gos_terrain_indirect.cpp` (add a self-contained anonymous-namespace block; hook the emit into the existing `CostSplit_RollFrame()` body — RE-GREP `void CostSplit_RollFrame` for the exact function)

- [ ] **Step 1: Add the accumulator definitions**

Add near the other `CostSplit_*` definitions in `gos_terrain_indirect.cpp`:

```cpp
// [LIGHT_COST_SPLIT v1] — RDTSC, distinct gate. See header rationale.
#include <intrin.h>   // __rdtsc  (already included transitively in most TUs;
                       // keep explicit — this file may not pull it otherwise)
namespace {
    bool                g_lcsInit   = false;
    bool                g_lcsOn     = false;
    unsigned long long  g_lcsC2Cyc  = 0, g_lcsC6Cyc = 0, g_lcsC5Cyc = 0;
    unsigned long long  g_lcsC2Call = 0, g_lcsC6Call = 0, g_lcsC5Call = 0;
    unsigned long long  g_lcsFrames = 0;
}

bool IsLightCostSplitEnabled() {
    if (!g_lcsInit) {                       // cache once — getenv per-call is slow
        g_lcsOn   = (getenv("MC2_LIGHT_COST_SPLIT") != nullptr);
        g_lcsInit = true;
    }
    return g_lcsOn;
}

void LightCostSplit_AddC2DirectCycles(unsigned long long c){ g_lcsC2Cyc += c; }
void LightCostSplit_AddC6ResubmitCycles(unsigned long long c){ g_lcsC6Cyc += c; }
void LightCostSplit_AddC5PerActorCycles(unsigned long long c){ g_lcsC5Cyc += c; }
void LightCostSplit_AddC2DirectCall(){ ++g_lcsC2Call; }
void LightCostSplit_AddC6ResubmitCall(){ ++g_lcsC6Call; }
void LightCostSplit_AddC5PerActorCall(){ ++g_lcsC5Call; }

void LightCostSplit_RollFrameAndMaybeEmit() {
    if (!IsLightCostSplitEnabled()) return;
    ++g_lcsFrames;
    if (g_lcsFrames % 600ULL != 0ULL) return;
    // Cycles, not nanos: TSC->ns needs a calibrated invariant-TSC rate we do
    // not have here. Per-call cycle ratios + calls/frame are the decision
    // signal (which bucket dominates, and does it scale with object count) —
    // an absolute ns is not required to resolve the C2-vs-C5 fork.
    const double f = (double)600.0;
    std::fprintf(stderr,
        "[LIGHT_COST_SPLIT v1] event=summary frames=600 "
        "c2_cyc_per_frame=%.0f c2_calls_per_frame=%.1f "
        "c6_cyc_per_frame=%.0f c6_calls_per_frame=%.1f "
        "c5_cyc_per_frame=%.0f c5_calls_per_frame=%.1f\n",
        (double)g_lcsC2Cyc/f, (double)g_lcsC2Call/f,
        (double)g_lcsC6Cyc/f, (double)g_lcsC6Call/f,
        (double)g_lcsC5Cyc/f, (double)g_lcsC5Call/f);
    g_lcsC2Cyc=g_lcsC6Cyc=g_lcsC5Cyc=0;
    g_lcsC2Call=g_lcsC6Call=g_lcsC5Call=0;
}
```

- [ ] **Step 2: Hook the roll into the existing per-frame boundary — FIRST statement, ABOVE the terrain-gate early-out (CRITICAL C-1)**

`CostSplit_RollFrame()`'s FIRST statement is `if (!IsCostSplitEnabled()) return;` (RE-GREP — review verified `gos_terrain_indirect.cpp:~281`; that gate is `MC2_TERRAIN_COST_SPLIT`). This slice deliberately uses a SEPARATE gate (`MC2_LIGHT_COST_SPLIT`) and the intended operating mode has `MC2_TERRAIN_COST_SPLIT` UNSET (its chrono scopes are falsified — `memory/cost_split_instrumentation_is_observer_effect_dominated.md`). Therefore the light-roll call MUST be placed as the **FIRST statement of `CostSplit_RollFrame()`, ABOVE the `if (!IsCostSplitEnabled()) return;` early-out** — otherwise it is unreachable in gate-only mode and the C2 number is NEVER emitted (the plan would self-review green and produce nothing):

```cpp
void CostSplit_RollFrame() {
    LightCostSplit_RollFrameAndMaybeEmit();   // [LIGHT_COST_SPLIT v1] — MUST be
                                              // above the next line; self-gates
                                              // on IsLightCostSplitEnabled().
    if (!IsCostSplitEnabled()) return;        // MC2_TERRAIN_COST_SPLIT (existing)
    ... // existing terrain-cost-split body unchanged
}
```

`LightCostSplit_RollFrameAndMaybeEmit()` self-gates on `IsLightCostSplitEnabled()`, so a stock frame (both gates off) pays one cached-bool branch and returns — inert.

- [ ] **Step 2b: Assert the summary actually emits in gate-only mode (CRITICAL C-1 verification)**

After Task 5 deploy, a smoke run with `MC2_LIGHT_COST_SPLIT=1` and `MC2_TERRAIN_COST_SPLIT` **unset** MUST show at least one `[LIGHT_COST_SPLIT v1] event=summary` line in the artifact log. If zero lines: the hook is below the terrain-gate early-out — C-1 regressed. This assertion is mandatory; without it the deliverable can be silently absent.

Note (CRITICAL C-2): `frames=600` counts `CostSplit_RollFrame` invocations (terrain-setup passes), NOT wall frames — terrain setup is skipped on cached/menu/load frames. The absolute `c2_calls_per_frame` is indicative only; the load-bearing signal is the **ratio** `c2_cyc / (c5_cyc + c6_cyc)` and whether c2 scales with object count. No C2 done-claim may rest on the absolute calls/frame.

- [ ] **Step 3: Build (full relink — load-bearing accumulator state)**

Run:
```bash
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/gos_terrain_indirect.obj
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```
Expected: links clean, `build64/RelWithDebInfo/mc2.exe` regenerated.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_terrain_indirect.cpp
git commit -m "instr: define [LIGHT_COST_SPLIT v1] RDTSC accumulators + 600-frame emit"
```

---

### Task 3: Bracket the three callsites in tgl.cpp

**Files:**
- Modify: `mclib/tgl.cpp` — three sites: `:3113` (C2), `:2865` (C6), `:2860` (C5). RE-GREP each symbol; lines WILL have drifted after Tasks 1-2 are unrelated TUs but tgl.cpp is untouched so far — still re-grep `addLightDataStructure` / `addLightDataStructureWithPerActorColor`.

**LOAD-BEARING (MAJOR M-1): bracket ONLY the three `TG_Shape` methods. Do NOT bracket `MC_TextureManager::addLightDataStructure*` in `mclib/txmmgr.cpp`** — `addLightDataStructureWithPerActorColor` (C5's impl) internally calls `addLightDataStructure` (C6's impl) at txmmgr.cpp:~1320/1362/1373 (RE-GREP). Bracketing at the wrapper level keeps C5 and C6 disjoint; bracketing in txmmgr would fold C5 cycles into C6 and destroy the isolation claim. All wrapper callers (`msl.cpp`, `genactor.cpp`, `gos_static_prop_batcher.cpp`) call THROUGH these three methods, so the in-method RAII scope still captures them — no leak, no miss.

- [ ] **Step 1: Add the RDTSC scope helper near the top of tgl.cpp**

After the existing `eligibleForGpuObjects` forward decl (RE-GREP `bool eligibleForGpuObjects` ~`:120`), add:

```cpp
#include <intrin.h>
#include "gos_terrain_indirect.h"   // LightCostSplit_* (RE-GREP include guard)
namespace {
// [LIGHT_COST_SPLIT v1] RAII cycle bracket. Cached-bool early-out; when the
// gate is off this is a predicted-not-taken branch + two __rdtsc reads at
// scope entry/exit only if enabled — below the 100ns Tracy floor concern
// because there is NO Tracy zone here by design.
struct LcsBucket { enum E { C2, C6, C5 }; };
template<int K> struct LcsScope {
    bool on; unsigned long long t0;
    LcsScope(): on(IsLightCostSplitEnabled()) { if (on) t0 = __rdtsc(); }
    ~LcsScope() {
        if (!on) return;
        unsigned long long d = __rdtsc() - t0;
        if (K==LcsBucket::C2){ LightCostSplit_AddC2DirectCycles(d);  LightCostSplit_AddC2DirectCall();  }
        if (K==LcsBucket::C6){ LightCostSplit_AddC6ResubmitCycles(d);LightCostSplit_AddC6ResubmitCall();}
        if (K==LcsBucket::C5){ LightCostSplit_AddC5PerActorCycles(d);LightCostSplit_AddC5PerActorCall();}
    }
};
} // namespace
```

- [ ] **Step 2: Bracket C5 (`TG_Shape::GatherGpuObjectLightDataOnly`)**

RE-GREP `GatherGpuObjectLightDataOnly`. Change the one-line body:

```cpp
uint32_t TG_Shape::GatherGpuObjectLightDataOnly()
{
	LcsScope<LcsBucket::C5> _lcs;
	return mcTextureManager->addLightDataStructureWithPerActorColor(&lightData_);
}
```

- [ ] **Step 3: Bracket C6 (`TG_Shape::ResubmitCachedLightData`)**

```cpp
uint32_t TG_Shape::ResubmitCachedLightData()
{
	LcsScope<LcsBucket::C6> _lcs;
	return mcTextureManager->addLightDataStructure(&lightData_);
}
```

- [ ] **Step 4: Bracket C2 (`:3113` legacy-leaf direct call)**

RE-GREP `rs.light_data_buffer_index_ = mcTextureManager->addLightDataStructure`. Wrap ONLY that statement (the scope must cover only the call, not the surrounding `rs` setup):

```cpp
			{
				LcsScope<LcsBucket::C2> _lcs;
				rs.light_data_buffer_index_ = mcTextureManager->addLightDataStructure(&lightData_);
			}
```

- [ ] **Step 5: Build (full relink — tgl.cpp is load-bearing, inline templates)**

Run:
```bash
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/tgl.obj
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```
Expected: links clean.

- [ ] **Step 6: Commit**

```bash
git add mclib/tgl.cpp
git commit -m "instr: bracket C2/C6/C5 addLightDataStructure callsites with [LIGHT_COST_SPLIT v1]"
```

---

### Task 4: Setup-path-tagged registration-attempt counter

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (`registerType` `~:1063`, `finalizeGeometry` — RE-GREP both; the existing `[GPUPROPS] late registerType` at `~:1078`)
- Modify: `code/mission.cpp` (Mission::init entry — RE-GREP `Mission::init`), `code/saveload.cpp` (`ObjectManager->Load` site `~:1391`, Mission::load entry)

- [ ] **Step 1: Add the global setup-path tag**

In `gos_static_prop_batcher.cpp` near the `s_typeIndex` definition (RE-GREP `s_typeIndex` decl ~`:176`), add:

```cpp
// [GPUPROPS v1] setup-path attribution for registerType attempts.
// 0=unknown 1=mission_init_firstload 2=ims_objmgr_load 3=sp_logistics
int g_lightProbeSetupPath = 0;
static bool s_gpuPropsTrace =
    (getenv("MC2_GPUPROPS_TRACE") != nullptr);
// g_regCall counts EVERY registerType invocation after the null guard and
// BEFORE the idempotent `s_typeIndex.count` early-return — so 0 for a path
// means registerType was genuinely never invoked on it (M-4: this exact
// placement is what makes the "ims=0 -> B2" decision-table row robust).
static unsigned g_regCall[4]      = {0,0,0,0};
static unsigned g_regLateDrop[4]  = {0,0,0,0};
```

Declare `extern int g_lightProbeSetupPath;` in `gos_static_prop_batcher.h` (RE-GREP for a public decl block).

- [ ] **Step 2: Count in registerType (exact placement is load-bearing — M-4)**

In `registerType()` (RE-GREP `void GpuStaticPropBatcher::registerType`), place the call counter **after the null guard (`if (!typeShape) return;`) and BEFORE the idempotent guard (`if (s_typeIndex.count(typeShape)) return;`)** so it counts every genuine invocation, not just first-registrations:

```cpp
    if (!typeShape) return;                                  // existing null guard
    if (s_gpuPropsTrace) ++g_regCall[g_lightProbeSetupPath & 3]; // <-- HERE
    if (s_typeIndex.count(typeShape)) return;                // existing idempotent guard
```

At the existing late-drop branch (RE-GREP the `[GPUPROPS] late registerType` fprintf ~`:1078`), immediately after that fprintf:

```cpp
        if (s_gpuPropsTrace) ++g_regLateDrop[g_lightProbeSetupPath & 3];
```

- [ ] **Step 3: Emit summary at finalizeGeometry**

In `finalizeGeometry()` (RE-GREP `::finalizeGeometry`), just before it sets `s_geometryFinalized = true` (RE-GREP that assignment ~`:1326`):

```cpp
    if (s_gpuPropsTrace) {
        std::fprintf(stderr,
            "[GPUPROPS v1] event=register_summary path=%d "
            "regCalls[init=%u ims=%u splog=%u unk=%u] "
            "lateDrops[init=%u ims=%u splog=%u unk=%u] "
            "typeIndexSize=%zu\n",
            g_lightProbeSetupPath,
            g_regCall[1], g_regCall[2], g_regCall[3], g_regCall[0],
            g_regLateDrop[1], g_regLateDrop[2], g_regLateDrop[3], g_regLateDrop[0],
            s_typeIndex.size());
    }
```

- [ ] **Step 4: Tag the setup paths (tag-only, no logic change)**

`code/mission.cpp` — at the top of `Mission::init` (RE-GREP `Mission::init`), before `loadTerrainObjects`:

```cpp
    extern int g_lightProbeSetupPath; g_lightProbeSetupPath = 1; // [GPUPROPS v1]
```

`code/saveload.cpp` — at the top of `Mission::load` (RE-GREP the `.ims` load entry; same fn that reaches `ObjectManager->Load` ~`:1391` and `finalizeGeometry` ~`:1583`):

```cpp
    extern int g_lightProbeSetupPath; g_lightProbeSetupPath = 2; // [GPUPROPS v1]
```

SP_LOGISTICS: `Logistics::beginMission` calls `mission->init()` which sets path=1; after `mission->init()` returns (RE-GREP the `mission->init(` callsite in `logistics.cpp` ~`:598`), overwrite to 3 to catch post-init stragglers (late actor spawns):

```cpp
    extern int g_lightProbeSetupPath; g_lightProbeSetupPath = 3; // [GPUPROPS v1] post mission->init
```

**M-2 — read before interpreting:** the static-prop registration walk (`registerStaticPropsForMissionLoad`) runs INSIDE `mission->init`, so under SP_LOGISTICS terrain-object `registerType` calls are attributed to **path=1 (init)**, NOT path=3. path=3 only captures genuinely-post-init late registrations. Consequently the path tag does NOT distinguish first-load from SP_LOGISTICS (both flow through `Mission::init`/path=1). The first-load-vs-SP_LOGISTICS-vs-`.ims` discriminator is the **presence/absence of `[MECHRESTORE v1] event=saveload_phase` markers** (only the `.ims` `Mission::load` path emits them). The path tag's load-bearing value is isolating the **`.ims` path=2** case: `regCalls[ims]==0` confirms `Mission::load` never runs the static-prop walk.

- [ ] **Step 5: Build (full relink)**

Run:
```bash
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/gos_static_prop_batcher.obj build64/RelWithDebInfo/mission.obj build64/RelWithDebInfo/saveload.obj build64/RelWithDebInfo/logistics.obj
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```
Expected: links clean.

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp GameOS/gameos/gos_static_prop_batcher.h code/mission.cpp code/saveload.cpp code/logistics.cpp
git commit -m "instr: [GPUPROPS v1] setup-path-tagged registerType attempt/late-drop counters"
```

---

### Task 5: Smoke env passthrough + regression gate

**Files:**
- Modify: `scripts/run_smoke.py` (RE-GREP the env-passthrough list / `os.environ` copy / `Popen(env=...)`)

- [ ] **Step 1: Add the new env vars to the passthrough allowlist**

Add `MC2_LIGHT_COST_SPLIT`, `MC2_GPUPROPS_TRACE`, `MC2_MECH_RESTORE_TRACE` to the **allowlist tuple used by the mission-launch `Popen`** (M-3/m-3: review found it at `scripts/run_smoke.py:~235-298`, the `**{k:v for k,v in os.environ.items() if k in (...)}` set — `MC2_TERRAIN_COST_SPLIT` is in it ~line 270; add the three adjacent to it. RE-GREP for the tuple). Do NOT add to the small line-62 `for var in [...]` loop — that does not reach the mission child, so the Task 5 Step 3 regression gate would prove nothing about the gated path.

- [ ] **Step 2: Deploy the rebuilt exe (C++-only slice — exe only, no shaders)**

Run (per-file `cp -f` + verify, NEVER `cp -r`):
```bash
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
powershell -Command "(Get-FileHash build64/RelWithDebInfo/mc2.exe).Hash -eq (Get-FileHash 'A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe').Hash"
```
Expected: `True` (deployed exe SHA256 == build64).

- [ ] **Step 3: Regression gate — tier1 smoke, gate OFF (stock-equivalence proof)**

Run:
```bash
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```
Expected: exit 0, 5/5 missions pass. With no env vars set, the build is byte-behaviourally identical to pre-slice (all probes self-gate off). This proves the instrumentation is inert in stock builds.

- [ ] **Step 4: Commit**

```bash
git add scripts/run_smoke.py
git commit -m "instr: forward MC2_LIGHT_COST_SPLIT/GPUPROPS_TRACE/MECH_RESTORE_TRACE to smoke child"
```

---

### Task 6: User-driven armed capture (the actual deliverable)

This task produces the numbers that unblock the C2 fix. It is USER-DRIVEN (smoke sessions are user-observed per CLAUDE.md) — the agent prepares the command and interprets the resulting log; the user runs their heaviest mission.

- [ ] **Step 1: Hand the user the capture command**

```
cd /d "A:\Games\mc2-opengl\mc2-win64-v0.4"
set MC2_LIGHT_COST_SPLIT=1
set MC2_GPUPROPS_TRACE=1
set MC2_MECH_RESTORE_TRACE=1
mc2.exe > "%USERPROFILE%\Desktop\c2_probe.log" 2>&1
```
Instruct: load the *heaviest legacy-object mission, via the same route normally used* (campaign-resume vs quicksave matters — that is exactly what we are disambiguating). Play ~30s at the worst-case zoomed-out camera (`memory/feedback_cost_split_worst_case_camera.md`), then exit.

- [ ] **Step 2: Interpret the log against the decision table**

Read `%USERPROFILE%\Desktop\c2_probe.log`. Extract:
- `[LIGHT_COST_SPLIT v1] event=summary` lines (steady-state, not first frame).
- `[GPUPROPS v1] event=register_summary` line(s).
- presence/absence of `[MECHRESTORE v1] event=saveload_phase` lines.
- any `[GPUPROPS] late registerType` lines.

**m-2 caveat (read first):** the RDTSC counters yield valid RATIOS only. Do NOT quote `c2_cyc_per_frame` as a ns-equivalent and do NOT compare absolute cycles across captures or machines (uncalibrated, non-invariant TSC). The load-bearing signal is the **ratio** `c2_cyc / (c5_cyc + c6_cyc)` and whether c2 scales with object count — quoting a fabricated absolute is the `matrix_index_convention_*` failure class. Per C-2, `*_per_frame` is per `CostSplit_RollFrame` invocation (terrain-setup pass), not per wall frame.

Decision table (this is the gate output — feeds Plan B):

| Observation | Conclusion |
|---|---|
| `c2_cyc/(c5_cyc+c6_cyc)` >> 1 and c2 scales with object count | C2 armed cost CONFIRMED dominant — Plan B is justified. |
| `c2_calls_per_frame` ~0 but Tracy zone still heavy | C2 is NOT the cost — the zone is C5/C6; re-open the fork, do NOT plan a C2 slice (Plan B branch B0). |
| zero `[MECHRESTORE v1] saveload_phase` lines | Heaviest mission did NOT go through `.ims` `Mission::load` (first-load or SP_LOGISTICS — both flow through `Mission::init`, so terrain regs sit in the **path=1** column; path=3 = post-init stragglers only). |
| `[MECHRESTORE v1] saveload_phase` lines present | Heaviest mission used **`.ims` `Mission::load`**. The **path=2 (ims)** column is the relevant one. |
| `regCalls[ims]==0` (`.ims` case) but terrain objects render & hit C2 | `Mission::load` never runs the static-prop walk → Plan B branch **B2** ("ensure registration on resume"). |
| `lateDrops[<active path>]` > 0 AND `[GPUPROPS] late registerType` fires | Late-register trap CONFIRMED → Plan B branch **B1** ("port finalizePending recovery"). |
| `regCalls[<active path>]` > 0, `lateDrops` = 0, but objects still C2 | Registration succeeds yet eligibility fails elsewhere (SHAPE_NODE check / partial-leaf taint) → Plan B branch **B3** ("eligibility predicate", new recon). |

- [ ] **Step 3: Record the verdict**

Append the captured summary lines + the selected Plan B branch to `docs/superpowers/plans/2026-05-17-c2-residual-fix-DECISION.md` (Plan B's gate file). Update `memory/` per the memory discipline if a durable fact emerged (e.g. "heaviest mission uses SP_LOGISTICS path"). No code change in this step.

---

## Self-Review (run against the two gate requirements)

**Gate 1 (armed C2 attribution):** Tasks 1-3 produce `c2_cyc_per_frame` / `c2_calls_per_frame` isolated from C5/C6 in the same conflated zone, via RDTSC (not chrono — observer-effect memory) and no Tracy zone (100ns floor). Task 6 captures it user-driven worst-case-camera. COVERED.

**Gate 2 (resume-path + registration disambiguation):** Task 4 tags every `registerType` attempt by setup path and counts late-drops; Task 6 reads it alongside the existing `[MECHRESTORE v1] saveload_phase` markers to pin which path the heaviest mission uses and whether terrain objects register on it. COVERED.

**Placeholder scan:** every code step shows the exact code; every command has expected output; no "TBD"/"handle edge cases". PASS.

**Type consistency:** `g_lightProbeSetupPath` (int, 0-3) declared in Task 4 Step 1, defined once, referenced via `extern` in Tasks 4.4 — consistent. `LcsScope<LcsBucket::E>` template defined Task 3.1, used 3.2-3.4 — consistent. `LightCostSplit_*` symbols declared Task 1, defined Task 2, called Task 3 — consistent. PASS.

**Stock-equivalence:** Task 5 Step 3 proves the gate-OFF build is behaviourally identical (all probes self-gate on cached env reads). Satisfies `memory/stock_install_must_remain_playable.md`. PASS.

---

## Execution Handoff

This is a pure instrumentation slice (no behavior change, no shader, C++-only deploy). Per the handoff methodology it still gets adversarial-plan-review before execution because it is the load-bearing gate for the entire C2 arc and a wrong probe formula fakes a confirmed root cause (`memory/matrix_index_convention_verify_before_trusting_cited_index.md` class of failure). After review:

1. **Subagent-Driven (recommended)** — fresh subagent per task, two-stage review between tasks.
2. **Inline Execution** — batch with checkpoints.
