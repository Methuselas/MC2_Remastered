> **RETIRED 2026-05-17 (tautological — see DECISION.md "C6 FIX DETERMINED"). DO NOT EXECUTE.**
>
> The `needsFullBakeNextFrame` gate structurally guarantees the bake is primed before any touch(); this probe would measure a certainty. The C6 fix is directly determined by code-trace. Kept for audit trail only.

# C6 Bake-Priming Probe — Implementation Plan (Plan A-2)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** One env-gated probe that answers the single undetermined question blocking the C6 lighting-residual fix: is the 38d8720 persistent static-light bake map *populated* for the recipes that take the `touch()`/C6 path, or does the static-skip path never prime it?

**Architecture:** A once-per-recipe `[LIGHTBRIDGE v1] event=c6_bake_probe` stderr line emitted from `BldgAppearance::touch()` and `TreeAppearance::touch()` (the sole callers of the C6 path), gated by a new env var `MC2_C6_BAKE_PROBE`. It reports `mc2GetBakedStaticLight(staticReg.recipeIndex, &tmp)` per distinct recipe. Touches `mclib/bdactor.cpp` ONLY — deliberately zero contact with `gos_terrain_indirect.cpp` (which carries foreign uncommitted WIP).

**Tech Stack:** C++ (MSVC RelWithDebInfo), env-gated stderr marker + a `std::unordered_set<int>` dedup, per the project debug-instrumentation-rule.

---

## Why this slice (read first)

C6 recon (decision file `2026-05-17-c2-residual-fix-DECISION.md`, "C6 RECON OUTCOME") established: the dominant lighting cost is `TG_Shape::ResubmitCachedLightData` via `TG_MultiShape::ResubmitCachedGpuLightData()`, sole callers `BldgAppearance::touch()` (`bdactor.cpp:3046`) / `TreeAppearance::touch()` (`bdactor.cpp:5153`). The candidate substitutive fix is a pure repoint: make `touch()` use the existing 38d8720 persistent slot (keyed by `staticReg.recipeIndex`, already in scope at both sites) instead of the 1677x/frame FNV+memcmp resubmit.

**The one unknown that forks the fix:** the 38d8720 bake map fills *lazily* on the first `update()` MISS. A static that only ever takes the `touch()` skip-path may never have primed its slot. If `mc2GetBakedStaticLight(recipeIndex)` HITS for ~all touch() recipes -> pure repoint, near-zero-cost win. If it MISSES for most -> the slice must also prime the bake (first-frame gather or seed at registration) = a C6-specific bring-up, not a pure repoint. Static analysis cannot decide this; only runtime on the heaviest mission can.

**Gather-vs-submit concern: resolved by code-trace, not deferred.** The offload-expert flagged "is the cost in the `addLightDataStructure` submit body vs `lightData_` gather upstream of the committed C6 bracket?". Render-expert grep-verified the answer: on the C6 path `lightData_` is the already-cached member; `TG_Shape::ResubmitCachedLightData()` (`tgl.cpp:2883`) is `return mcTextureManager->addLightDataStructure(&lightData_);` with NO gather. The only upstream work is the cheap `firstShapeNodeLeaf` pointer-scan in `ResubmitCachedGpuLightData` (`msl.cpp:1994-2003`); the ~3.3M cyc is the 1792B FNV + 1792B memcmp dedup INSIDE the already-bracketed `addLightDataStructure` (`txmmgr.cpp:2886`). The committed C6 RDTSC bracket already isolates that submit cost. No second split probe is required for the repoint to be provably substitutive. (If adversarial-plan-review escalates this, the empirical leaf-scan-vs-submit split is a known, scoped add-on.)

## Verified anchors (grep-verified 2026-05-17; RE-GREP at execution — lines drift)
- `BldgAppearance::touch()` opens `mclib/bdactor.cpp:3037`; C6 call `bldgShape->ResubmitCachedGpuLightData();` at `:3046`; `staticReg.recipeIndex` in scope (used `:3057-3058`).
- `TreeAppearance::touch()` opens `mclib/bdactor.cpp:5144`; C6 call `treeShape->ResubmitCachedGpuLightData();` at `:5153`; `staticReg.recipeIndex` in scope (used `:5162-5163`).
- `bool mc2GetBakedStaticLight(int32_t recipeIndex, TG_HWLightsData& out)` defined `mclib/txmmgr.cpp:1197`. An `extern` forward-decl pattern already exists in bdactor.cpp at `:2221`: `extern bool mc2GetBakedStaticLight(int32_t, TG_HWLightsData&);` — reuse this exact extern form.
- `TG_HWLightsData` is the type passed; bdactor.cpp:2221-2230 shows the call idiom (`TG_HWLightsData baked; if (mc2GetBakedStaticLight(recipeIndex, baked)) ...`).

---

## File Structure
- **Modify** `mclib/bdactor.cpp` ONLY: a file-scope env-gated probe helper + one call in each of the two `touch()` bodies. No header changes, no other TU.

---

### Task 1: Add the c6_bake_probe helper + the two touch() call sites

**Files:** Modify `mclib/bdactor.cpp` (RE-GREP every anchor below).

- [ ] **Step 1: Add the probe helper at file scope**

RE-GREP an existing file-scope anonymous-namespace or static-helper region near the top of `bdactor.cpp` (after includes). Add:

```cpp
// [LIGHTBRIDGE v1] C6 bake-priming probe (MC2_C6_BAKE_PROBE). Answers whether
// the 38d8720 persistent bake map is primed for touch()-path recipes. Once
// per distinct recipe per kind; demote-not-delete.
namespace {
extern bool mc2GetBakedStaticLight(int32_t, TG_HWLightsData&);  // def txmmgr.cpp
inline void c6BakeProbe(const char* kind, int32_t recipeIndex) {
    static const bool on = (getenv("MC2_C6_BAKE_PROBE") != nullptr);
    if (!on) return;
    if (recipeIndex < 0) return;
    static std::unordered_set<long long> seen;   // key = (kind firstchar<<32)|recipe
    long long key = ((long long)(unsigned char)kind[0] << 32) | (unsigned)recipeIndex;
    if (!seen.insert(key).second) return;
    TG_HWLightsData tmp;
    bool baked = mc2GetBakedStaticLight(recipeIndex, tmp);
    std::fprintf(stderr,
        "[LIGHTBRIDGE v1] event=c6_bake_probe kind=%s recipe=%d baked=%d\n",
        kind, recipeIndex, baked ? 1 : 0);
}
} // namespace
```

Verify `<unordered_set>` and `<cstdio>`/`<stdlib.h>` are available in bdactor.cpp (RE-GREP includes; the file already uses `getenv` and `fprintf(stderr,...)` for other `[..]` markers — match that exact style, incl. whether `std::` is used). If `extern bool mc2GetBakedStaticLight` at the existing `:2221` is in a different scope, ensure this file-scope extern does not ODR-conflict — if the existing one is function-local, a second file-scope extern of the same signature is fine; if there is already a header decl, include that instead and drop the local extern. Do not introduce a conflicting declaration.

- [ ] **Step 2: Call from `BldgAppearance::touch()`**

RE-GREP `void BldgAppearance::touch()` and the `bldgShape->ResubmitCachedGpuLightData();` line. Immediately BEFORE that call add:

```cpp
		c6BakeProbe("bldg", staticReg.recipeIndex);
```
Match surrounding indentation (tabs).

- [ ] **Step 3: Call from `TreeAppearance::touch()`**

RE-GREP `void TreeAppearance::touch()` and `treeShape->ResubmitCachedGpuLightData();`. Immediately BEFORE that call add:

```cpp
		c6BakeProbe("tree", staticReg.recipeIndex);
```

- [ ] **Step 4: Build (full relink — bdactor.cpp is load-bearing)**

```
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/bdactor.obj
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```
Expected: links clean. Fix compile errors at root cause (likely a missing include or the extern ODR note above).

- [ ] **Step 5: Commit (per-hunk staging discipline)**

This worktree carries FOREIGN uncommitted WIP. `git status --short` first. `git diff mclib/bdactor.cpp` must contain ONLY your Task-1 edits; if any foreign hunk appears, STOP / report BLOCKED. Stage ONLY `mclib/bdactor.cpp`. `git show <sha> --stat` must = exactly that one file.
```
git add mclib/bdactor.cpp
git commit -m "instr: [LIGHTBRIDGE v1] C6 bake-priming probe in Bldg/Tree touch()"
```

---

### Task 2: Deploy + build-stability gate

**Files:** none (deploy + smoke only).

- [ ] **Step 1: Deploy (C++-only — exe only)**
```
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
powershell -Command "(Get-FileHash build64/RelWithDebInfo/mc2.exe).Hash -eq (Get-FileHash 'A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe').Hash"
```
Expected: `True`. Never `cp -r`; retry `cp -f` if not True.

- [ ] **Step 2: tier1 stability gate (probe OFF)**
```
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```
Expected: exit 0, 5/5. (`MC2_C6_BAKE_PROBE` is forwarded by the allowlist? It is NOT — this probe uses a fresh env var. The Task 3 capture is run by the USER directly, not via run_smoke, so the smoke allowlist does not need it. Stability gate runs with the probe OFF, proving inertness.) If a mission FAILS: inspect `tests/smoke/artifacts/<latest>/`, report BLOCKED with the failing mission + log tail. Do not work around.

- [ ] **Step 3: No commit (no files changed in Task 2).**

---

### Task 3: User-driven capture + verdict (the deliverable)

USER-DRIVEN per CLAUDE.md. Agent prepares the command + interprets; user runs the heaviest mission.

- [ ] **Step 1: Hand the user the capture command**
```
cd /d "A:\Games\mc2-opengl\mc2-win64-v0.4"
set MC2_C6_BAKE_PROBE=1
set MC2_LIGHT_COST_SPLIT=1
mc2.exe > "%USERPROFILE%\Desktop\c6_probe.log" 2>&1
```
Instruct: same heaviest legacy-object mission as the C2 capture, same route. Play ~30s+ at worst-case zoomed-out camera, exit.

- [ ] **Step 2: Interpret against the decision table**

Extract all `[LIGHTBRIDGE v1] event=c6_bake_probe` lines (one per distinct recipe per kind) and the `[LIGHT_COST_SPLIT v1] event=summary` C6 numbers (sanity: c6_calls still ~1677 zoomed-out, confirming the same regime).

| Observation | Conclusion -> Plan A-3 (fix) branch |
|---|---|
| `baked=1` for ~all c6_bake_probe recipes | **Branch R (pure repoint).** The bake is primed. Fix = `touch()` reads the persistent slot when `mc2GetBakedStaticLight(recipeIndex)` hits, legacy resubmit gated as MISS fallback. Cheapest; no new table. |
| `baked=0` for most/all recipes | **Branch P (prime + repoint).** Skip-path statics never prime the bake. Fix must drive one priming gather (first-frame `update()`/`CacheGpuLightData`, or seed the slot at `registerRecipe`) THEN repoint. C6-specific bring-up. |
| Mixed (some 1, some 0) | **Branch P-partial.** Characterize which recipe class misses (by kind / by registration timing); the prime step targets only the missing set. Fresh narrow recon before the fix plan. |
| `c6_bake_probe` count ~0 but C6 calls ~1677 | Probe mis-placed (touch() not the live path in this mission) — re-open recon, do NOT plan the repoint. |

- [ ] **Step 3: Record the verdict**

Append the captured `c6_bake_probe` summary + selected branch (R / P / P-partial) to the decision file `2026-05-17-c2-residual-fix-DECISION.md` under a new "C6 BAKE-PRIMING VERDICT" heading. Update memory if a durable fact emerges (e.g. "touch()-path statics do/don't prime the 38d8720 bake"). The fix itself (Plan A-3) is authored only after this verdict, per the promotion procedure (dispatch render-expert + cpu-gpu-offload-expert, then writing-plans, then adversarial-plan-review).

---

## Self-Review

**Question coverage:** Task 1 emits `baked` per distinct touch()-path recipe; Task 3 maps it to the R / P / P-partial fork. The single undetermined question is answered. COVERED.

**Foreign-WIP isolation:** only `mclib/bdactor.cpp` is touched (not the foreign-WIP `gos_terrain_indirect.cpp`); Task 1 Step 5 enforces per-hunk staging + single-file commit verification. PASS.

**Placeholder scan:** every code step has exact code; commands have expected output; no TBD. PASS.

**Stock-equivalence:** the probe self-gates on a cached `getenv` (`static const bool on`); probe OFF = one predicted branch, inert. Task 2 Step 2 proves tier1 5/5 with probe off. Satisfies `stock_install_must_remain_playable`. PASS.

**Discipline:** no emoji, no wall-clock, RE-GREP mandated on every anchor, demote-not-delete. PASS.

---

## Execution Handoff
Pure single-file instrumentation. Still gets adversarial-plan-review before execution (it is the gate selecting the C6 fix branch; a mis-placed or mis-keyed probe fabricates the wrong fork — the `matrix_index_convention` failure class). Then subagent-driven execution (Tasks 1-2), then user-driven Task 3.
