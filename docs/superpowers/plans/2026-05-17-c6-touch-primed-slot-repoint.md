# C6 touch() Primed-Slot Repoint — Implementation Plan (Plan A-3)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** Eliminate the dominant lighting-arc CPU cost — C6 `addLightDataStructure` ~1677 calls/frame, ~3.3M cyc/frame zoomed-out — by making `BldgAppearance::touch()` / `TreeAppearance::touch()` repoint to the already-primed 38d8720 persistent light slot instead of re-running the per-frame 1792B FNV + 1792B memcmp dedup resubmit.

**Architecture:** Code-determined (no probe). The `needsFullBakeNextFrame` lifecycle gate structurally guarantees the 38d8720 bake is primed before any `touch()` (render-expert adjudication 2026-05-17). The C6 fast-path guard (`msl.cpp:1982-1984`) fails on the `touch()` path only because `cachedFrame_` is never re-stamped there. Fix: on `touch()`, when `mc2GetBakedStaticLight(recipeIndex)` HITS, call `EmitBakedGpuLightData` (the same primed-slot repoint `update()`'s else-branch already uses) which stamps `cachedGpuLightIndex_`+`cachedFrame_` with zero FNV/memcmp; keep the legacy `ResubmitCachedGpuLightData()` as the MISS / kill-switch fallback. Substitutive: the legacy path is gated, not deleted; the committed `MC2_LIGHT_COST_SPLIT` C6 bucket is the verification instrument.

**Tech Stack:** C++ (MSVC RelWithDebInfo). Modifies `mclib/bdactor.cpp` only.

---

## Why / full rationale (read before touching code)

Decision file `2026-05-17-c2-residual-fix-DECISION.md` -> "C6 FIX DETERMINED" section is the authoritative recon. Key grep-verified facts (RE-GREP all at execution; lines drift):

- C6 cost site: `TG_Shape::ResubmitCachedLightData()` `mclib/tgl.cpp:2883` (one-liner `return mcTextureManager->addLightDataStructure(&lightData_);`), reached via `TG_MultiShape::ResubmitCachedGpuLightData()` `mclib/msl.cpp:1968` (call ~:2006). Sole callers: `BldgAppearance::touch()` (`mclib/bdactor.cpp:3037`, C6 call ~:3046) and `TreeAppearance::touch()` (`mclib/bdactor.cpp:5144`, C6 call ~:5153).
- The primed-slot repoint already exists and is used by `update()`'s else-branch: `mc2CacheOrBakeStaticGpuLight(shape, staticReg.registered, staticReg.recipeIndex)` at `bdactor.cpp:2574` (Bldg) / `:4986` (Tree). Its HIT idiom is at `bdactor.cpp:2230-2231` (`mc2GetBakedStaticLight(recipeIndex, baked)` true -> `EmitBakedGpuLightData(recipeIndex, baked)`). `mc2CacheOrBakeStaticGpuLight` body: `bdactor.cpp:2217-2253`. `EmitBakedGpuLightData`: `mclib/msl.cpp:~1953` (sets `cachedGpuLightIndex_=recipeIndex`, `cachedFrame_=g_mc2FrameCounter`, no FNV/memcmp — msl.cpp:1955-1965).
- Kill-switch: `mc2LightBakeEnabled()` / `MC2_LIGHTBAKE` (`mclib/txmmgr.cpp:1191-1195`). `mc2GetBakedStaticLight(int32_t, TG_HWLightsData&)` def `txmmgr.cpp:1197` (pure const query, no side effects — adversarial-review-confirmed). Existing extern idiom `bdactor.cpp:2221`.
- **SECOND kill-switch (recon missed it; adversarial-review-flagged) — `MC2_LIGHTBRIDGE` / `mc2LightBridgeRepointEnabled()` (`txmmgr.cpp:1116-1141/1183-1187`, default ON).** `ResubmitCachedGpuLightData()` ALREADY contains an inner repoint early-return at `msl.cpp:1981-1992` gated on `mc2LightBridgeRepointEnabled() && cachedFrame_==g_mc2FrameCounter && cachedGpuLightIndex_!=0xFFFFFFFFu`. On the steady-state `touch()` path `cachedFrame_ != g_mc2FrameCounter` so this inner guard falls through to the FNV resubmit (the C6 cost) — the fix's correctness is unaffected. BUT it confounds the perf proof two ways: (1) a FIX-OFF run with only `MC2_LIGHTBAKE=0` does NOT reproduce the true legacy 1677-call baseline because the inner `MC2_LIGHTBRIDGE` guard is still ON inside the fallback; (2) a FIX-ON `c6->~0` cannot be attributed to THIS slice vs the pre-existing inner guard. Both are handled in Task 4's capture matrix below. Parity-relevant: index-space parity HOLDS — `resetLightData` (`txmmgr.cpp:1389`) rebases the dynamic allocator base to `S` when bake is on, so `recipeIndex in [0..S)` and dynamic slots `>=S` are the SAME `lightData_[]` SSBO space, consumed identically (`gos_static_prop_registry.cpp:402-405` -> `inst.lightDataIndex`). `EmitBakedGpuLightData` is O(1) (two scalar stores, `msl.cpp:1964-1965`); the per-frame whole-buffer LightsData upload scales with `s_staticLightHighWater` and is PRE-PAID by 38d8720, NOT introduced or displaced-into by this slice.
- `recipeIndex` is a permanent shipped slot: `bakeStaticLightSlot` writes `lightData_[recipeIndex]` permanently + bumps the per-frame whole-buffer upload count (`txmmgr.cpp:1425-1436`). So `EmitBakedGpuLightData` stamping `cachedGpuLightIndex_=recipeIndex` is a valid always-live downstream index.

**Why NOT just call `mc2CacheOrBakeStaticGpuLight` in touch():** its MISS arm calls `CacheGpuLightData()`, which has per-actor terrain-color-staleness when run outside the per-actor `worldLights[0]->aRGB` window (`msl.cpp:1874-1887`) — `touch()` runs outside that window. So the fix must take the HIT path explicitly and keep the *existing* `ResubmitCachedGpuLightData()` as the MISS fallback (NOT `CacheGpuLightData`). MISS is structurally near-impossible in the shipped config (invalidate re-sets `needsFullBakeNextFrame` -> routes to `update()` not `touch()`); it self-heals in one frame if it ever occurs.

## Verified anchors (RE-GREP every one at execution)
- `void BldgAppearance::touch()` — `mclib/bdactor.cpp:3037`; the `bldgShape->ResubmitCachedGpuLightData();` call ~`:3046`; `staticReg` (.registered, .recipeIndex) is a `StaticRegistration` member in scope (used at `bdactor.cpp:3050` `staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();`).
- `void TreeAppearance::touch()` — `mclib/bdactor.cpp:5144`; `treeShape->ResubmitCachedGpuLightData();` ~`:5153`; `staticReg` in scope (used `:5155`).
- The HIT idiom to mirror EXACTLY: `bdactor.cpp:2230-2231` inside `mc2CacheOrBakeStaticGpuLight` (`:2217-2253`).
- Stale comment to correct (minimal-touch, in THIS commit): `bdactor.cpp:3039-3040` claims touch() only fires when `MC2_STATIC_UPDATE_SKIP=1` and default keeps `update()` running — INVERTED; `code/terrobj.cpp:92` defaults skip ON so touch() IS the default path.

---

## File Structure
- **Modify** `mclib/bdactor.cpp` ONLY: the two `touch()` bodies + the one stale comment. No headers, no other TU. (`bdactor.cpp` is NOT in the foreign-WIP set — adversarial-review-confirmed; still use per-file staging.)

---

### Task 1: Repoint `BldgAppearance::touch()`

**Files:** `mclib/bdactor.cpp` (RE-GREP `void BldgAppearance::touch()`).

- [ ] **Step 1: Read the exact current touch() body + the HIT idiom**

RE-GREP and READ: `BldgAppearance::touch()` full body (~bdactor.cpp:3037-3052), `mc2CacheOrBakeStaticGpuLight` (~:2217-2253) to copy its HIT idiom verbatim, `EmitBakedGpuLightData` signature/class in `mclib/msl.cpp` (~:1953 — confirm it is a `TG_MultiShape` method matching `bldgShape`'s type, and its exact params). Confirm `TG_HWLightsData`, `mc2GetBakedStaticLight`, `mc2LightBakeEnabled` decls reachable from bdactor.cpp (extern idiom already at :2221; reuse the exact form, block-scoped or file-scoped consistent with existing usage — do NOT place an extern inside an anonymous namespace).

- [ ] **Step 2: Replace the C6 call with the guarded primed-slot repoint**

Replace the single line `bldgShape->ResubmitCachedGpuLightData();` (RE-GREP it; preserve tab indentation) with the HIT/MISS form below, adjusting symbol names/params to MATCH the verbatim idiom at `bdactor.cpp:2230-2231` and the real `EmitBakedGpuLightData` signature you just read:

```cpp
		// [LIGHTBRIDGE v1] C6 retirement: repoint to the primed 38d8720 slot
		// (cachedFrame_ stamped -> msl.cpp fast-path guard passes next frame,
		// zero FNV/memcmp). MISS keeps the legacy resubmit (NOT
		// CacheGpuLightData -- terrain-color-staleness, msl.cpp:1874-1887).
		// MC2_LIGHTBAKE=0 -> legacy path, bit-for-bit.
		TG_HWLightsData baked;
		if (mc2LightBakeEnabled()
		    && staticReg.registered && staticReg.recipeIndex >= 0
		    && mc2GetBakedStaticLight(staticReg.recipeIndex, baked)) {
			bldgShape->EmitBakedGpuLightData(staticReg.recipeIndex, baked);
		} else {
			bldgShape->ResubmitCachedGpuLightData();
		}
```

If the verbatim `:2230-2231` idiom differs (e.g. `EmitBakedGpuLightData` takes different args, or HIT uses a helper), mirror THAT exactly rather than this sketch — the proven update()-else-branch behavior is the contract, this block is its shape.

- [ ] **Step 3: Correct the inverted stale comment (same commit, minimal-touch)**

RE-GREP `bdactor.cpp:3039-3040` (the comment claiming touch() only fires under `MC2_STATIC_UPDATE_SKIP=1` / default keeps update()). Rewrite it to state the truth: `code/terrobj.cpp:92` defaults `s_staticUpdateSkip` ON, so `touch()` IS the default per-frame path for registered statics. Keep it one or two lines, no wall-clock, no emoji.

- [ ] **Step 4: Build (full relink — bdactor.cpp load-bearing)**
```
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/bdactor.obj
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```
Links clean; fix compile errors at root cause (symbol/signature mismatch -> re-read the real decls, do not cast around it).

- [ ] **Step 5: Commit (per-file staging)**

`git status --short`; `git diff mclib/bdactor.cpp` must contain ONLY Task-1 edits (Bldg touch repoint + the comment fix). If a foreign hunk appears, STOP / BLOCKED. Stage ONLY `mclib/bdactor.cpp`.
```
git add mclib/bdactor.cpp
git commit -m "perf(lighting): C6 retirement -- Bldg touch() repoints to primed 38d8720 slot"
```
`git show <sha> --stat` = exactly `mclib/bdactor.cpp`.

---

### Task 2: Repoint `TreeAppearance::touch()`

**Files:** `mclib/bdactor.cpp` (RE-GREP `void TreeAppearance::touch()`).

- [ ] **Step 1: Apply the identical transform to the Tree path**

RE-GREP `TreeAppearance::touch()` and `treeShape->ResubmitCachedGpuLightData();`. Replace with the SAME guarded form as Task 1 Step 2, substituting `treeShape` for `bldgShape` (and the Tree `staticReg`). Use the verbatim idiom; same comment header.

```cpp
		TG_HWLightsData baked;
		if (mc2LightBakeEnabled()
		    && staticReg.registered && staticReg.recipeIndex >= 0
		    && mc2GetBakedStaticLight(staticReg.recipeIndex, baked)) {
			treeShape->EmitBakedGpuLightData(staticReg.recipeIndex, baked);
		} else {
			treeShape->ResubmitCachedGpuLightData();
		}
```

- [ ] **Step 2: Build (full relink)**
```
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/bdactor.obj
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```
Links clean.

- [ ] **Step 3: Commit (per-file staging, same discipline as Task 1 Step 5)**
```
git add mclib/bdactor.cpp
git commit -m "perf(lighting): C6 retirement -- Tree touch() repoints to primed 38d8720 slot"
```
`git show <sha> --stat` = exactly `mclib/bdactor.cpp`.

---

### Task 3: Deploy + build-stability gate

- [ ] **Step 1: Deploy (C++-only, exe only)**
```
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
powershell -Command "(Get-FileHash build64/RelWithDebInfo/mc2.exe).Hash -eq (Get-FileHash 'A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe').Hash"
```
Expect `True`. Never `cp -r`.

- [ ] **Step 2: tier1 stability + default-camera regression gate**
```
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```
Expect exit 0, 5/5. This proves no crash/regression at the default-camera operating point AND (since the fix is default-on, not env-gated) that the repointed lighting renders correctly across 5 biomes. A mission FAIL or visual-wrong report = BLOCKED; inspect `tests/smoke/artifacts/<latest>/`, do not work around. tier1 is the *correctness* gate here; the *perf* proof is Task 4 (tier1 is structurally blind to the 1677-call zoomed-out regime).

- [ ] **Step 3: no commit (deploy/smoke only).**

---

### Task 4: User-driven substitutive proof (the done-gate)

USER-DRIVEN. The C6 fix is default-on; the committed `MC2_LIGHT_COST_SPLIT` RDTSC bucket (Plan A) is the verification instrument — demote-not-delete paying off.

- [ ] **Step 1: Hand the user the capture matrix (3 legs — MC2_LIGHTBRIDGE isolation, MAJOR-1/-2)**

The deployed exe already HAS the fix (default-on). The recon-missed inner `MC2_LIGHTBRIDGE` guard means a faithful baseline AND attribution require three legs, all same heaviest mission / same route / ~30s+ at the worst-case zoomed-out camera (the 1677-call regime):

LEG A — TRUE LEGACY BASELINE (both repoints off; reproduces the Plan-A 1677-call/3.3M-cyc number):
```
cd /d "A:\Games\mc2-opengl\mc2-win64-v0.4"
set MC2_LIGHT_COST_SPLIT=1
set MC2_LIGHTBAKE=0
set MC2_LIGHTBRIDGE=0
mc2.exe > "%USERPROFILE%\Desktop\c6_legA_baseline.log" 2>&1
```
LEG B — THIS FIX ISOLATED (inner LIGHTBRIDGE guard OFF so the only C6-killer is the new touch() repoint; this is the leg that attributes the win to THIS slice):
```
cd /d "A:\Games\mc2-opengl\mc2-win64-v0.4"
set MC2_LIGHT_COST_SPLIT=1
set MC2_LIGHTBRIDGE=0
mc2.exe > "%USERPROFILE%\Desktop\c6_legB_fixisolated.log" 2>&1
```
LEG C — PRODUCTION DEFAULT (both repoints on; the shipped state):
```
cd /d "A:\Games\mc2-opengl\mc2-win64-v0.4"
set MC2_LIGHT_COST_SPLIT=1
mc2.exe > "%USERPROFILE%\Desktop\c6_legC_default.log" 2>&1
```
Plus one LEG C run WITHOUT `MC2_LIGHT_COST_SPLIT` for the clean total-frame Tracy (CostSplit-on inflates absolutes — the anti-mirage instrument is the production-state total frame), captured at BOTH default and zoomed-out-big-map camera.

- [ ] **Step 2: Substitutive verdict (done-governor, attribution-correct)**

From the `[LIGHT_COST_SPLIT v1] event=summary` lines zoomed-out:
- **LEG A (true baseline):** `c6_calls_per_frame` ~1677, `c6_cyc` ~3.3M — reproduces the Plan-A number, proving both kill-switches off = legacy and that we measured the same regime. If LEG A does NOT show ~1677, the regime/route differs — STOP, do not compare.
- **LEG B (this fix isolated — THE attribution gate):** `c6_calls_per_frame` AND `c6_cyc_per_frame` -> ~0 (collapse toward the C2 floor ~1) with `MC2_LIGHTBRIDGE=0`. This is the leg that proves THIS slice retired C6 (the inner guard is off, so the only thing that can kill C6 is the new touch() repoint). **This leg going to ~0 is the binding done-criterion** — not LEG C.
- **LEG C (production default):** `c6 -> ~0` as well (consistent with B); confirms shipped state.
- Anti-mirage (LEG C non-COST_SPLIT total-frame Tracy): `addLightDataStructure scan` zone drops AND total frame drops, NO displaced cost in `EmitBakedGpuLightData` (O(1), confirmed) or the whole-buffer LightsData upload (pre-paid by 38d8720, scales with S not per-call — a flat upload cost is expected and is NOT displacement), in BOTH default and zoomed-out-big-map camera. Per substitutive-not-additive: LEG-B-zone->0 alone is NOT the proof; the LEG-C total-frame delta is.
- **Visual (BLOCKING, MAJOR-3):** user confirms static building/tree lighting is unchanged (no darkening/flicker/wrong-tint) specifically at the **user-driven zoomed-out-big-map** camera (where this fix engages 1677x and where 38d8720's persistent-table correctness is exercised at scale), AND at default camera. This slice's visual correctness is conditional on 38d8720's persistent-table being correct (itself not yet independently confirmed). **User sign-off 2026-05-17: COMBINED — this Task 4 zoomed-out capture is the single user-driven proof that confirms BOTH this C6 slice AND 38d8720's persistent-table (it is exercised at 1677x scale here). On DONE, record 38d8720 as co-confirmed.** (User also signed off: trust the empirical 3-leg matrix; no extra LIGHTBRIDGE advisor round.)

If LEG A ~1677 + LEG B c6->~0 + LEG C total-frame drop + zoomed-out visual-clean: **DONE — substitutive C6 retirement confirmed AND attributed to this slice.** Record in the decision file ("C6 SUBSTITUTIVE PROOF" heading) + update memory (supersede the C6-residual memory: retired, mechanism = touch() primed-slot repoint; note 38d8720 co-confirmed if applicable). If LEG B does NOT collapse (but LEG C does): the win is the pre-existing inner guard, NOT this fix — do NOT claim this slice; re-recon. If cost displaces into Emit/upload beyond the flat S-scaled baseline, or visuals regress: do NOT claim done; capture the anomaly, add a temporary gated HIT/MISS counter in touch() to diagnose (likely the MISS branch is being taken).

- [ ] **Step 3: finishing-a-development-branch**

On confirmed DONE, use superpowers:finishing-a-development-branch for the 7-commit arc (Plan A 5 + A-3 2) on `claude/gpu-driven-rendering`.

---

## Self-Review
- **Substitutive, not additive:** legacy `ResubmitCachedGpuLightData()` retained as MISS/kill-switch fallback (demote-not-delete); done-gate is c6->~0 + total-frame drop, not flag-on-parity. PASS.
- **Kill-switch:** `mc2LightBakeEnabled()` reused (no new switch); `MC2_LIGHTBAKE=0` -> bit-for-bit legacy. PASS.
- **Staleness trap avoided:** MISS keeps `ResubmitCachedGpuLightData()`, NOT `CacheGpuLightData()` (the per-actor terrain-color-staleness path). PASS.
- **Camera-scaled risk:** per-actor change, strictly reduces work, no cull/LOD/distance bound touched, no `inView`/`canBeSeen` bypass; Task 4 mandates user-driven zoomed-out proof tier1 cannot give (`zoomed_out_big_map` memory). PASS.
- **Foreign-WIP isolation:** only `bdactor.cpp` (not in foreign set); per-file staging + single-file commit verification each task. PASS.
- **Minimal-touch:** the inverted comment is corrected in the same commit (it directly concerns the path being changed), not as a separate cleanup slice. PASS.
- **Placeholder scan:** every step has exact code/commands/expected output; the one intentional flex ("mirror the verbatim :2230-2231 idiom if it differs") is a correctness safeguard, not a placeholder — the contract (HIT->Emit primed slot, MISS->legacy resubmit) is fully specified. PASS.

## Execution Handoff
Adversarial-plan-review (mandated — perf-gate >=30%, architectural endpoint, camera-scaled hot path) BEFORE execution. Then subagent-driven Tasks 1-3, user-driven Task 4.
