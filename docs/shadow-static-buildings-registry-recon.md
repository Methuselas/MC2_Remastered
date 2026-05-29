# SHADOW-STATIC-BUILDINGS-REGISTRY-RECON-0

**Date:** 2026-05-29
**Scope:** Recon/plan only. One docs commit. No implementation.
**Why:** SHADOW-STATIC-BUILDINGS-1 (Option B) failed — it reused `flushShadow`'s per-frame `s_bucketsByType`/`s_typeRanges`, which only hold the **camera-visible** prop subset (live diag: `typeRanges=5`, `bldgTagged=121`). A world-fixed all-buildings static shadow map cannot come from per-frame visible buckets. This recon checks whether the **registry's full recipe data** (visibility-independent) can drive it.

## TL;DR — YES, Option C is feasible and self-contained

The registry **retains the baked world transform (`modelMatrix`) and `typeID` for every leaf of every registered prop**, forever, independent of visibility (`s_recipes[]` indexed by `s_recipeRanges[]`, `gos_static_prop_registry.cpp:112-140`). `GpuStaticPropInstance` is a **binary-exact match** for `shadow_static_prop.vert`'s instance block, and the shadow shader reads **only `modelMatrix`** — so registry records feed the depth draw with **zero conversion**. The baked per-type geometry (`s_sharedVao/Ibo`, `batcher_getTypeDrawInfo`) is resident after `finalizeGeometry()`. A **one-shot, default-OFF** pass that builds an all-buildings instance SSBO from the registry and draws it into the static map — entirely inside the existing `!gos_StaticLightMatrixBuilt()` build block — is ~70 lines, **no per-frame render-order change**.

## Answers to the recon questions

**Is Option C feasible?** Yes. All required data survives visibility culling in the registry.

**Exact data source that replaces buckets:**
- `s_recipes` — `std::vector<GpuStaticPropInstance>` (registry.cpp:139): all leaf instances of all registered props, each with baked `modelMatrix[16]` + `typeID` + flags. Set once at `registerStatic` (mission load) from `rec->shapeToWorld`; **not** re-derived per frame (the `TG_MultiShape* multi` in the range is only for per-frame `lightDataIndex` patching, not transforms).
- `s_recipeRanges` — `std::vector<RecipeRange>` (registry.cpp:140): per-recipe `{first, count, ...}` into `s_recipes`. `count==0` = tombstone (invalidated/destroyed). Iterate `0..size()`, skip `count==0`.
- `GpuStaticPropInstance` (gos_static_prop_batcher.h:21-32, 112 bytes): `modelMatrix[16], typeID, firstColorOffset, flags, lightDataIndex, aRGBHighlight[4], fogRGB[4]`.

**Shadow shader compatibility:** `shadow_static_prop.vert:11-20` declares an `Instance` struct **identical** to `GpuStaticPropInstance` (same 112-byte layout, std430 binding 0) and uses **only** `inst.modelMatrix` (depth-only; `lightDataIndex`/`firstColorOffset`/color fields untouched — their baked placeholders are harmless). Matrix uniform = `pp->getLightSpaceMatrix()` (static world-fixed). So `s_recipes[]` ranges upload directly as the instance SSBO.

**What new buffer/draw path is needed:**
1. A new GL SSBO (e.g. `s_staticBldgShadowInstanceSsbo`), distinct from the per-frame `s_instanceSsbo` (which is overwritten each frame). Built ONCE.
2. Iterate `s_recipeRanges[]`; for non-tombstoned **Building** recipes, copy `s_recipes[first..first+count)` grouped by `typeID`; concat into the SSBO; record a `typeID → (byteOffset, instanceCount)` map.
3. Draw: per typeID, `glBindBufferRange(...binding 0...)` + `glDrawElementsInstancedBaseVertex` using `batcher_getTypeDrawInfo(typeID,...)` geometry against `s_sharedVao`/`s_sharedIbo` with the `shadow_static_prop` program + `pp->getLightSpaceMatrix()` + polygon offset. (Same draw primitive as `flushShadow`, different instance source + matrix.)
4. Runs inside `gos_BeginShadowPrePass(false)` (append to terrain depth, no clear) / `gos_EndShadowPrePass()`.

This belongs in the batcher (owns `s_sharedVao/Ibo` + the shadow program); it pulls instance data from the registry via a small iterator/accessor (Option B framing — registry exposes "for each building recipe, here are its leaves").

**Building vs tree distinction:** NOT stored per-recipe today (population is only a per-frame counter input). For building-only, add a `uint8_t population` (or `bool isBuilding`) to `RecipeRange`, set at `registerRecipe` time — the caller (`BldgAppearance::registerStatic` bdactor.cpp:2538 / `TreeAppearance::registerStatic` :4413) knows which it is. ~0 cost. Trees stay dynamic (excluded).

**Gated default-OFF + self-contained?** Yes. Single env `MC2_STATIC_PROP_BUILDING_SHADOW=1` checked inside the one-shot `!gos_StaticLightMatrixBuilt()` block. Zero code-path impact when off. No per-frame work, no render-order change (unlike Option B's per-frame latch).

**Timing:** mission load order (mission.cpp): `onMapLoad` → `registerStaticPropsForMissionLoad` (fills `s_recipes`/`s_recipeRanges`) → `finalizeGeometry` (uploads `s_sharedVbo/Ibo`, fills `s_types`). The static-map build fires frame-1 in `renderLists` (txmmgr ~1916) — by then both registry leaves AND baked geometry are ready. Safe for a one-shot build.

**Invalidation debt:**
- **Building destruction/damage:** the registry tombstones the recipe (`count=0`) on `invalidate`, but the one-shot static SSBO is built once → destroyed buildings keep casting a stale static shadow until mission reload. **Primary new debt.** Same class as the existing terrain static shadow / the deferred destruction-invalidation. Acceptable first-cut; a follow-up could rebuild the SSBO on destroy (re-trigger the one-shot) if it matters.
- **Sun-direction change:** static matrix frozen at build (pre-existing terrain-static debt; MC2 sun is fixed per mission).
- **Mission reload:** `gos_ResetStaticLightMatrix()` (Terrain::destroy) re-arms the latch → rebuild next mission. Free the SSBO at `onMapUnload`/rebuild at `onMapLoad`.

**Worth doing before CSM?** Yes. It leverages 100% existing infrastructure (registry data, baked geometry, shadow shader, static matrix, prepass), is ~one session, default-OFF, and delivers stable far-field building shadows that the bounded-near dynamic fit (visible-only) can't. CSM is a much larger architecture effort; this is independent and complementary (CSM later supersedes the dynamic near map, not this static-building map).

## Recommended implementation slice — SHADOW-STATIC-BUILDINGS-2

1. **Population on `RecipeRange`** (`uint8_t population`), set at `registerRecipe` from the registerStatic caller (Building/Tree). Additive.
2. **Registry iterator/accessor** for "all non-tombstoned Building recipes → their `s_recipes` leaf ranges" (read-only; visibility-independent).
3. **Batcher one-shot builder + draw** `GpuStaticPropBatcher::buildAndDrawStaticBuildingShadows(const float* staticMatrix)`: build `s_staticBldgShadowInstanceSsbo` (grouped by typeID) from the iterator, draw per-type into the bound (static) FBO with `shadow_static_prop` + polygon offset. Build the SSBO once (cache); the draw is one-shot inside the latch.
4. **txmmgr one-shot call** inside the `!gos_StaticLightMatrixBuilt()` block (gate `MC2_STATIC_PROP_BUILDING_SHADOW=1`, default OFF), using `gos_BeginShadowPrePass(false)` (append) — only when the prepass actually activates (reuse the `bool gos_BeginShadowPrePass` return idea from the reverted WIP).
5. **Lifecycle:** free/rebuild SSBO at `onMapUnload`/`onMapLoad`; latch re-arm via `gos_StaticLightMatrixBuilt()`.
6. **Counters + `=2` trace** (recipes scanned, building leaves uploaded, types drawn) — proves it from logs.
7. **Reuses C-pre** (`7ea32b83`): a building in both the static map and the dynamic bounded-near map no longer double-darkens terrain.
8. **Debt filed:** destroyed-building stale shadow until mission reload; sun-change frozen.

Deferred (unchanged): foliage shadow shape (SHADOW-FOLIAGE-ALPHA-DISCARD-1), CSM (E), self-shadow bias tuning.
