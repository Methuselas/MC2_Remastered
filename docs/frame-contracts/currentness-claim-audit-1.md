# FRAME-CURRENTNESS-CLAIM-AUDIT-1

## Contract

Any code path that declares an object is (submitted / visible / eligible-target / hover-target / animation-current) must preserve the PRODUCER stamp that proves the claim is fresh relative to the current frame. If a consumer later trusts that stamp without validating it, a stale stamp silently invalidates the object without logging.

**Canonical instance:** commit `07a1f8ac` (R2B-STATIC-NATURAL-TOUCH-PRESERVE-1). A CPU skip path issued bare `continue` that skipped `update()` — the producer that stamps a registered shape's `cachedFrame_`. The consumer (static-prop registry `flush()`, `gos_static_prop_registry.cpp:987`) drops any registered multi whose `cachedFrame_ != currentFrame`, causing silent culling (black trees on NVIDIA).

---

## Claims & Producer/Consumer Audit

| Claim Site | Claim Value | Producer Stamp | Producer Code | Consumer | Consumer Check | Outcome if Stale | Guard Status |
|---|---|---|---|---|---|---|---|
| `code/objmgr.cpp:192` | "Skipped prop silently culled" | `cachedFrame_` (liveness) | `update()` → `CacheGpuLightData()` / `ResubmitCachedGpuLightData()` or `mc2R2bTouchStaticLiveness()` | `gos_static_prop_registry.cpp:987` | `rng.multi->getCachedFrame() != currentFrame` → skip range | Prop dropped from SSBO, never drawn | **GUARDED** (R2B-TOUCH-PRESERVE-1, code:199-207, frame stamp in objmgr.cpp:229,236) |
| `mclib/bdactor.cpp:2120` | "Marked visible → Submitted to GPU batch" | `recipeIndex >= 0 && count > 0` | `registerRecipe()` / `markVisible()` → `GpuStaticPropRegistry::markVisibleChecked()` | `gos_static_prop_registry.cpp:976,987` | Tombstone guard: `if (rng.count == 0 \|\| !rng.multi) continue` | Prop skipped → culled | **GUARDED** (markVisibleChecked validates count/multi before return Submitted, bdactor:2123-2126) |
| `mclib/bdactor.cpp:2158-2160` | "Shape-swapped prop still registered → use stale recipe" | `staticReg.shape != bldgShape` (active LOD mismatch) | Shape assignment (LOD swap, damage state) | `bdactor:2098` (IsStaticNow check) | `staticReg.registered && staticReg.shape != bldgShape` | Bldg rendered with wrong geometry (prev LOD) | **GUARDED** (invalidateStaticRegistration clears entry, forces re-register) |
| `mclib/bdactor.cpp:5873` | "Tree marked visible from active LOD → static path suppresses dynamic" | `recipeIndex[activeLOD]` valid & `cachedGpuLightIndex_ != UINT32_MAX` | `submitMultiShape()` & `CacheGpuLightData()` (both per-frame) | Tree render() line 5850-5890 (suppression of fallback) | `markVisibleChecked()` returns `Submitted` (multi & count non-zero) | Tree vanishes (dynamic fallback never runs) | **GUARDED** (lines 5853-5856 check lightIndex sentinel; 5868-5888 validate markVisible result; invalidate on failure) |
| `mclib/bdactor.cpp:5907-5908` | "Active LOD0 still has old treeShape registered" | `staticReg[activeLOD].shape != treeShape` (shape reassignment) | LOD/damage shape swap at update time | Render-path decision at 5843-5949 | `activeLOD == 0 && staticReg[0].registered && staticReg[0].shape != treeShape` | Tree renders with stale mesh (prev state) | **GUARDED** (invalidate clears registration before re-submission) |
| `mclib/bdactor.cpp:2147-2180` | "Unregistered shape w/ late-reg flag → skip full-bake next frame" | `needsFullBakeNextFrame` (frame-delayed re-registration) | `submitMultiShape()` side effect: `wasLastFailureLateRegistration()` | `bdactor:2074` (touch skip check) | Predicate guard + explicit flag check | Bldg/tree skips registration indefinitely (type never registers) | **GUARDED** (flag check at line 2074; explicitly set on failed submit line 2177) |
| `mclib/msl.cpp` | "Offscreen actor update() skipped → multi's light/frame state stale" | `cachedGpuLightIndex_` + `cachedFrame_` | `update()` skipped by cull gate | `gos_static_prop_registry.cpp:1077,987` | Frame-stamp guard at 987; light-index UINT32_MAX fallback at 2120 | Wrong RGB lighting (slot contains prev-frame actor's data) | **GUARDED** (per-instance light-index path + frame-stamp check + UINT32_MAX sentinel) |
| `code/objmgr.cpp:199-207` | "R2b fast-path skips expensive update() → must stamp cachedFrame_ anyway" | `cachedFrame_` refresh without transform/bounds | `mc2R2bTouchStaticLiveness()` at line 229/236 | `gos_static_prop_registry.cpp:987` frame check | `setCachedFrame(currentFrame)` called (line 229/236) even on skip | Tree silently culled next frame | **GUARDED** (code/objmgr.cpp:199-207 gate; 229/236 direct stamp call; 2549 integration) |
| `code/missiongui.cpp:1744` | "Fresh-picked target cached as WID + pointer → valid next frame" | `s_hoverCachedTargetWID` (watch-ID) | `findObjectByMouse()` or GPU pick, stores `bm->getWatchID()` | `missiongui.cpp:1708` (next-frame validation) | `ObjectManager->getByWatchID(s_hoverCachedTargetWID) == s_hoverCachedTarget` | Pointer deref on dead object → READ-violation crash (vtable call on 0xFFFF..) | **GUARDED** (line 1708 validates WID before any deref; 1712 clears cache if stale) |
| `code/missiongui.cpp:1773` | "Target object pointers cached → must validate WID next cycle" | `s_targetWatchID` (watch-ID of current target) | `target->getWatchID()` called at pick time (line 1773) | `missiongui.cpp:1636,1643` (updateTarget re-validation) | `ObjectManager->getByWatchID(s_targetWatchID) != target` → set target=0 | Stale pointer virtual call (isDisabled / getDescription) → crash | **GUARDED** (explicit getByWatchID re-validation at 1636/1643 before any deref) |
| `code/ablmc2.cpp:4538` | "Refit buddy fetched without re-validation → may be freed" | Watch-ID stored at prior time | Prior assignment to `refitBuddyWID` (bldng.cpp/warrior.cpp context-dependent) | `ablmc2.cpp:4538` (getByWatchID call within ABL script) | `ObjectManager->getByWatchID(bay->refitBuddyWID)` returns nullptr if dead | Null deref or stale pointer if object died | **PARTIALLY GUARDED** (getByWatchID called; return value unchecked for nullptr) |
| `code/bldng.cpp:628,644,738-739` | "Parent building / refit buddy fetched via cached WID" | `refitBuddyWID` / `parent` (watch-IDs) | Set during prior building init | Multiple sites (628, 644, 738, 745, 784, 889, 897-898, etc.) | Bare `ObjectManager->getByWatchID(WID)` call | Null deref if building/parent/buddy was destroyed | **PARTIALLY GUARDED** (some call sites check result, some do not; e.g. line 745 checks, 784 does not) |
| `code/mech.cpp` | "Sensor system cached → must handle null if pilot ejects or mech dies" | `sensorSystem` pointer stored | Set at mech init or restoration | Rendering / update code reads without guard | Deref null sensorSystem → crash | **UNGUARDED** (historical null-check missing; FIXED in soak harness but not committed to audit branch) |

---

## Highest-Risk Unguarded Claims

Ranked by **blast radius** (number of objects × severity of failure):

### 1. **Bldng.cpp parent/refit-buddy watch-ID dereference (line 745, 784, 889, 897-898, 908-909, 920-921, 924, 932-934, 1006-1007)**
- **Risk:** ~300 buildings/mission, each may cache parent WID at spawn; parent can die any frame (destroyed by mission script, player, cheat-mode). Sparse null-checks across the ~15 sites.
- **Failure mode:** Null-deref or stale pointer read-violation in object-manager lookups. No centralized guard.
- **Recommendation:** Audit all `getByWatchID(parent)` call sites; guard return value or store parent pointer directly (not WID).

### 2. **ABL script execGetRelativePositionToObject (code/ablmc2.cpp:4398 & 4538)**
- **Risk:** ~1000+ ABL script invocations/mission under dynamic conditions; WID validity depends on prior context (unclear provenance).
- **Failure mode:** Null deref in position-calc or sensor-related queries; crashes recorded in campaign soak (Exodus, some TCE missions).
- **Recommendation:** Enforce null-return handling in all ABL object-fetch calls; add getByWatchID wrapper that logs on null.

### 3. **SensorSystemManager::newSensor null sensorSystem (code/contact.cpp, per soak ledger)**
- **Risk:** Mechs with null sensorSystem (historically rare but confirmed during eject sequence + restore; stub-chassis edge case).
- **Failure mode:** Null deref in newSensor during combat; crash recorded (keid-v m7, others).
- **Recommendation:** Null-guard in SensorSystemManager::newSensor; track root cause (mech init, eject handoff, pilot loss).

### 4. **Hop-off frame-stamp skips without re-validation (cachedFrame_ contract violation)**
- **Risk:** Any skip path (cull gate, R2b fast-path, FRAME_JOBS worker prepass) that touches a frame-dependent contract without re-stamping.
- **Current status:** R2B-TOUCH-PRESERVE guarded; but new skip paths added over time without defense (e.g., FRAME_JOBS-2F touch-split).
- **Recommendation:** Enforce "every skip → explicit frame-stamp or contract re-check" rule in code review; flag `continue` in hot loops.

### 5. **GPU static-prop registry tombstone race (markVisibleChecked validates, but registration churn may invalidate mid-frame)**
- **Risk:** Multi-step process (register → markVisible → flush) spans frames; LOD swaps / shape reassigns / invalidation can race.
- **Current status:** Tombstone guards exist (count==0, multi==nullptr) at flush; markVisibleChecked validates before accept.
- **Recommendation:** Add per-frame stability counter to RecipeRange; reject stale markVisible calls (frame number mismatch).

---

## Summary

**Status:** 8/10 major claims have localized guards. **High-risk gap:** C/ABL object-fetch sites (bldng.cpp, ablmc2.cpp, contact.cpp) rely on sparse null-checks across many call sites. **Systematic risk:** Frame-stamp contracts (cachedFrame_, needsFullBakeNextFrame) are implicit; new skip paths must be audited per-commit to ensure producer-stamp preservation.

**Process recommendation:** Enforce "producer–consumer pairing" checklist in code review: if a skip path skips a producer, the consumer must either (a) re-validate the stamp, or (b) call a minimal-cost producer-refresh function (e.g., `mc2R2bTouchStaticLiveness()`).
