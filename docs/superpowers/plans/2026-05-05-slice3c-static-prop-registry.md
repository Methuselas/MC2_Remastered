# Stage 3.C — StaticPropRegistry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate per-frame `CacheGpuLightData()` + `TransformMultiShape_PositionsOnly()` + `submitMultiShape()` for tree instances whose world-space lighting and position are stable, by caching `GpuStaticPropInstance` snapshots in a `GpuStaticPropRegistry` and replaying them each frame via a compact live-index list.

**Architecture:** Persistent CPU-side recipe table (`std::vector<GpuStaticPropInstance>`) + per-frame live-instance list populated by `markVisible()` in `TreeAppearance::render()`. Registry `flush()` injects cached instances into the batcher's `PerTypeBucket` queues via `submitCachedInstance()` **before** the batcher's own `flush()`, so all instances (dynamic + static) draw in one combined GPU pass with no extra SSBO. No new SSBO, no shader change, trees-only scope.

**Tech Stack:** C++14 STL (`<vector>`, `<cstring>`), OpenGL (no new calls), existing `GpuStaticPropBatcher` infrastructure.

**Env gate:** `MC2_STATIC_PROP_REGISTRY=1` (opt-in, default off for initial soak). Full Stage 3.C validation requires both `MC2_STATIC_PROP_REGISTRY=1` **and** `MC2_STATIC_UPDATE_SKIP=1` — the registry eliminates render-side `submitMultiShape()` cost, while the existing Stage 3.B skip gate eliminates update-side `CacheGpuLightData()` + `TransformMultiShape_PositionsOnly()` cost. Running `MC2_STATIC_PROP_REGISTRY=1` alone exercises only the render-side path and will not move the `TerrainObject::update appearanceUpdate` Tracy zone.

---

## Blocker resolutions (code-verified before plan-write)

These were raised by adversarial review. Each is resolved by grepping the actual source.

**B1 — fogRGB is stable for trees:** `TreeAppearance::update()` computes `fogRGB` based on `xlatPosition.y` (the tree's world elevation, not camera distance) vs `eye->fogStart`/`eye->fogFull` (fixed environment parameters in MC2's RTS mode). The result is cached on `TreeAppearance::fogRGB` and written to the shape via `treeShape->SetFogRGB(fogRGB)` (bdactor.cpp:4368). Recomputation is guarded by `!fogLightSet` (bdactor.cpp:4319) which is only cleared by reset of the flag — not by camera movement. For a tree at a fixed terrain position, `fogRGB` is constant. ✓

**B2 — submitMultiShape cardinality:** `submitMultiShape()` calls `submit()` once per eligible `SHAPE_NODE` leaf (gos_static_prop_batcher.cpp:1098–1136). A tree with N shape nodes produces N `GpuStaticPropInstance` entries in `PerTypeBucket::instances`. The plan uses a **batch capture** approach (`s_lastBuiltBatch` vector, cleared at `submitMultiShape()` start, populated per `submit()` call) to handle N≥1 correctly. ✓

**B3 — no early return from render():** Post-submit selection visualization (`drawBars`, `drawSelectBrackets`, `drawTextHelp` at bdactor.cpp:4141–4161) must still run. The static path sets `submittedToGpu = true` and falls through — it does NOT `return NO_ERR`. ✓

**B4 — readiness uses correct GPU-auth gate:** `IsStaticNow()` uses `g_useGpuObjects` (the actual submit gate at bdactor.cpp:4106), not `g_useGpuStaticProps` (the legacy bypass-cull flag). ✓

**B5 — flush integration:** Registry `flush()` injects via `submitCachedInstance()` before batcher `flush()`. Static instances share `s_bucketsByType`; batcher's `flush()` draws everything. No separate SSBO, no ring-buffer management in the registry. ✓

**MI-1 — abstraction-safe invalidation:** `terrobj.cpp` calls only `appearance->invalidateStaticRegistration()` (virtual default no-op). The `TreeAppearance` override handles `GpuStaticPropRegistry::invalidate(recipeIndex)` internally. ✓

**MI-2 — no duplicate live indices:** `TerrainObject::render()` is called at most once per visible frame per object by `ObjectManager::render()`. No idempotent guard needed; document assumption only. ✓

**Note on GpuStaticPropInstance size:** The struct is **112 bytes** (verified by `static_assert` at gos_static_prop_batcher.h:27). The spec said "96 bytes" — that was wrong. The plan uses the correct value.

**Note on aRGBHighlight:** `TG_Shape::aRGBHighlight` is initialized to 0 at shape allocation (tgl.cpp:251, 467) and has no setter call in `TreeAppearance::update()`. It is always 0 for trees = stable. ✓

**C1 — lightDataIndex is per-frame ephemeral (FIXED):** `resetLightData()` (called from MC_TextureManager each frame) clears `lightDataStructuresCount = 0`, invalidating all UBO slot indices. A `lightDataIndex` baked into `s_recipes` at registration time is stale from frame 2 onward. Fix: (a) `touch()` calls `treeShape->CacheGpuLightData()` each frame to refresh `TG_MultiShape::cachedGpuLightIndex_` (`uint32_t`, msl.h:275, `UINT32_MAX` = not yet gathered); (b) `RecipeRange` stores `TG_MultiShape* multi` (same pointer as `treeShape`, lifetime matches the tree); (c) `flush()` reads `multi->getCachedGpuLightIndex()` and patches a copy of each leaf's `inst.lightDataIndex` before `submitCachedInstance()`. New inline getter `getCachedGpuLightIndex()` added to `TG_MultiShape` in msl.h (returns the protected field). The live list changes to store `regIdx` values (renamed `s_liveRangeIndices`) so `flush()` can access the range and its `multi` pointer. ✓

**C2 — wrong file for init/destroy (FIXED):** `GpuStaticPropBatcher::onMapLoad()` is called from `code/mission.cpp:1644`, not from `gameosmain.cpp`. `onMapUnload()` is at `code/mission.cpp:3171`. Task 9 Step 5 is corrected accordingly, with include path `"../GameOS/gameos/gos_static_prop_registry.h"` matching the established relative-include pattern. ✓

**M1 — shape-swap blocks re-registration (FIXED):** When `treeShape` is reassigned (LOD swap at bdactor.cpp:3984, damage at ~3596/3626), `IsStaticNow()`'s `staticReg.shape == treeShape` check correctly routes to the dynamic path, but `staticReg.registered=true` still blocks the registration block from firing for the new shape. Fix: in `render()`'s dynamic path (the `else if (treeShape)` branch), before `submitMultiShape()`, detect `staticReg.registered && staticReg.shape != treeShape` and call `invalidateStaticRegistration()` to clear the stale entry and enable fresh registration. ✓

---

## File map

| File | Action | Responsibility |
|---|---|---|
| `mclib/tgl.h` | Modify | Declare `TG_Shape::Touch()` |
| `mclib/tgl.cpp` | Modify | Implement `TG_Shape::Touch()` |
| `mclib/msl.h` | Modify | Declare `TG_MultiShape::Touch()`, `getCachedGpuLightIndex()` getter |
| `mclib/msl.cpp` | Modify | Implement `TG_MultiShape::Touch()` |
| `mclib/appear.h` | Modify | Add `virtual void touch() {}` and `virtual void invalidateStaticRegistration() {}` |
| `GameOS/gameos/gos_static_prop_batcher.h` | Modify | Declare `getLastBuiltBatch()`, `submitCachedInstance()` |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | Modify | Implement batch capture in `submitMultiShape()` + `submit()`; implement `submitCachedInstance()` |
| `GameOS/gameos/gos_static_prop_registry.h` | Create | Registry API namespace |
| `GameOS/gameos/gos_static_prop_registry.cpp` | Create | Registry implementation |
| `GameOS/gameos/CMakeLists.txt` | Modify | Add `gos_static_prop_registry.cpp` to gameos target |
| `mclib/bdactor.h` | Modify | `StaticRegistration` struct + `staticReg` member + method declarations on `TreeAppearance` |
| `mclib/bdactor.cpp` | Modify | `init()` zero-init, `touch()`, `IsStaticNow()`, `invalidateStaticRegistration()`, render() static branch + registration block |
| `code/terrobj.cpp` | Modify | `invalidateStaticRegistration()` on `ownerForcesDynamic`; `touch()` in skip branch |
| `code/gamecam.cpp` | Modify | `GpuStaticPropRegistry::frameBegin()` before `land->render()` |
| `mclib/txmmgr.cpp` | Modify | `GpuStaticPropRegistry::flush()` before `GpuStaticPropBatcher::instance().flush()` |
| `GameOS/gameos/gameosmain.cpp` | Modify | Extend `[INSTR v1]` banner with `static_prop_registry` field |

---

## Task 1: TGL Touch() layer

**Files:**
- Modify: `mclib/tgl.h` (near `lastTurnTransformed` declaration, line ~769)
- Modify: `mclib/tgl.cpp` (new function at end of file)
- Modify: `mclib/msl.h` (near TG_MultiShape public methods, ~line 281)
- Modify: `mclib/msl.cpp` (new function at end of file)

### What this does

`TG_Shape::Render()` has a staleness guard at tgl.cpp:2876: `if (lastTurnTransformed != turn) return`. When Stage 3.B's outer-skip gate skips `update()`, `TransformMultiShape_PositionsOnly()` never runs, so `lastTurnTransformed` goes stale. `Touch()` advances the stamp without touching vertex data. `timing.h` (which provides `extern long turn`) is already included in tgl.cpp at line 22.

- [ ] **Step 1: Add `Touch()` declaration to `TG_Shape` in tgl.h**

Find the `public:` section (line ~771, after the `static` data members block). Add immediately before the existing `void * operator new` or any public method:

```cpp
// Stage 3.C: advance lastTurnTransformed without running vertex transform.
// Called from TreeAppearance::touch() when the static registry is active.
void Touch();
```

- [ ] **Step 2: Implement `TG_Shape::Touch()` in tgl.cpp**

Add at the end of tgl.cpp (before the final `//` comment if any):

```cpp
void TG_Shape::Touch() {
    lastTurnTransformed = turn;
}
```

- [ ] **Step 3: Add `Touch()` declaration to `TG_MultiShape` in msl.h**

Find the public methods section of `TG_MultiShape` class (~line 281). Add:

```cpp
// Stage 3.C: propagate Touch() to all shape-node leaves.
void Touch();
```

- [ ] **Step 4: Implement `TG_MultiShape::Touch()` in msl.cpp**

Add at the end of msl.cpp:

```cpp
void TG_MultiShape::Touch() {
    for (long i = 0; i < numTG_Shapes; i++)
        if (listOfShapes[i].node)
            listOfShapes[i].node->Touch();
}
```

Note: `listOfShapes` is `TG_ShapeRecPtr` (array of `_TG_ShapeRec`). Each entry has a `node` field (`TG_Shape*`). Verified in msl.h:262.

- [ ] **Step 5: Add `getCachedGpuLightIndex()` getter to `TG_MultiShape` in msl.h**

Find the `public:` methods section of `TG_MultiShape` (~line 281, just after the `protected:` data block). Add the inline getter immediately after the `void init()` body:

```cpp
// Stage 3.C: expose the per-frame light-data UBO slot index for
// GpuStaticPropRegistry::flush() to patch into cached recipe copies.
// CacheGpuLightData() (called from TreeAppearance::touch() each frame)
// keeps this fresh. Returns UINT32_MAX if CacheGpuLightData() has not
// yet been called (first frame or non-GPU path) — submitMultiShape()
// already handles UINT32_MAX via gather-now fallback; flush() should
// treat UINT32_MAX as lightDataIndex=0 (no lighting data, safe default).
uint32_t getCachedGpuLightIndex() const { return cachedGpuLightIndex_; }
```

- [ ] **Step 6: Build and verify no errors**

```
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
.claude/scripts/mc2-build.sh
```

Expected: zero new errors. No functional change yet.

- [ ] **Step 7: Commit**

```bash
git add mclib/tgl.h mclib/tgl.cpp mclib/msl.h mclib/msl.cpp
git commit -m "feat(3c): add TG_Shape::Touch() + TG_MultiShape::Touch() + getCachedGpuLightIndex()"
```

---

## Task 2: Appearance base virtuals

**Files:**
- Modify: `mclib/appear.h` (adjacent to existing `IsStaticNow()` at line ~138)

- [ ] **Step 1: Add two virtual no-ops to `Appearance` class in appear.h**

Locate the `IsStaticNow()` virtual (line ~138). Immediately after it, add:

```cpp
// Stage 3.C: stamp-advance path for static registry. Default no-op.
// Override in TreeAppearance calls TG_MultiShape::Touch() without
// running the full vertex transform.
virtual void touch() {}

// Stage 3.C: clear static registration when the owner transitions to
// dynamic (falling, damage, override). Default no-op. Override in
// TreeAppearance calls GpuStaticPropRegistry::invalidate() internally.
virtual void invalidateStaticRegistration() {}
```

- [ ] **Step 2: Build, verify no errors**

- [ ] **Step 3: Commit**

```bash
git add mclib/appear.h
git commit -m "feat(3c): add Appearance::touch() + invalidateStaticRegistration() virtual no-ops"
```

---

## Task 3: Batcher batch-capture and submitCachedInstance()

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.h` (public API section)
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (anonymous namespace + submit() + submitMultiShape() + new public method)

### What this does

Adds two things to the batcher:

1. **Batch capture (`s_lastBuiltBatch`):** A file-static vector, cleared at the start of each `submitMultiShape()` call, populated with the built `GpuStaticPropInstance` after each successful `submit()` leaf. After `submitMultiShape()` returns true, `getLastBuiltBatch()` returns this vector for snapshot registration.

2. **`submitCachedInstance()`:** Takes a pre-built `GpuStaticPropInstance`, looks up the appropriate `PerTypeBucket` by `typeID`, updates `firstColorOffset` to the current bucket color position, pushes the instance, and pushes `vertexCount` zero-bytes of color data (debug path only; normal render ignores the Colors SSBO).

- [ ] **Step 1: Add declarations to gos_static_prop_batcher.h**

In the `GpuStaticPropBatcher` public section, after the `wasLastFailureLateRegistration()` declaration (~line 188), add:

```cpp
// Stage 3.C: snapshot of all leaf instances built by the most recent
// successful submitMultiShape() call. Valid only until the next
// submitMultiShape() call. Used by GpuStaticPropRegistry::registerRecipe().
const std::vector<GpuStaticPropInstance>& getLastBuiltBatch() const;

// Stage 3.C: inject a pre-built instance into the per-type bucket
// without running the compute path. Called by GpuStaticPropRegistry::flush()
// before batcher flush. Updates firstColorOffset for this frame's bucket
// position; pushes zero-fill colors (debug only; normal render ignores
// the Colors SSBO binding 1).
void submitCachedInstance(const GpuStaticPropInstance& inst);
```

- [ ] **Step 2: Add `s_lastBuiltBatch` file-static in gos_static_prop_batcher.cpp**

In the anonymous namespace (near `s_bucketsByType` at line ~127), add:

```cpp
// Stage 3.C: per-submitMultiShape batch accumulator. Cleared at the
// start of each submitMultiShape(); populated by submit() per leaf.
std::vector<GpuStaticPropInstance> s_lastBuiltBatch;
```

- [ ] **Step 3: Populate `s_lastBuiltBatch` in `submit()`**

In `GpuStaticPropBatcher::submit()`, immediately after `bucket.instances.push_back(inst)` (line ~817), add:

```cpp
s_lastBuiltBatch.push_back(inst);
```

- [ ] **Step 4: Clear `s_lastBuiltBatch` at the start of `submitMultiShape()`**

In `GpuStaticPropBatcher::submitMultiShape()`, at the very top of the function body (before any loops), add:

```cpp
s_lastBuiltBatch.clear();
```

- [ ] **Step 5: Implement `getLastBuiltBatch()`**

Add to gos_static_prop_batcher.cpp (after the existing `submitMultiShape()` implementation):

```cpp
const std::vector<GpuStaticPropInstance>& GpuStaticPropBatcher::getLastBuiltBatch() const {
    return s_lastBuiltBatch;
}
```

- [ ] **Step 6: Implement `submitCachedInstance()`**

Add after `getLastBuiltBatch()`:

```cpp
void GpuStaticPropBatcher::submitCachedInstance(const GpuStaticPropInstance& inst) {
    if (inst.typeID >= s_types.size()) return;
    const GpuStaticPropType& type = s_types[inst.typeID];
    PerTypeBucket& bucket = s_bucketsByType[inst.typeID];

    // Capacity: PerTypeBucket::instances and colors are std::vector — they
    // grow on demand. No explicit per-frame limit in the batcher; same
    // dynamic growth applies here.
    //
    // Diagnostic counters: s_counters.submitted_children and
    // submitted_instances_by_pop are NOT incremented for registry-injected
    // instances. This is intentional — they measure the dynamic (compute)
    // path only. Registry-injected instances are a separate population;
    // their count is tracked inside GpuStaticPropRegistry (s_liveInstanceIndices.size()).
    //
    // Parity / ObserveSubmittedShape: not called for registry instances.
    // The original registration frame already went through the normal path;
    // subsequent static-replay frames are excluded by design (no new
    // per-actor snapshot is meaningful when no CPU compute ran).

    GpuStaticPropInstance updated = inst;
    updated.firstColorOffset = static_cast<uint32_t>(bucket.colors.size());
    bucket.instances.push_back(updated);
    // Push zero-fill colors. Normal render ignores the Colors SSBO;
    // debug addr-mode 4 shows black for static-registry instances.
    bucket.colors.insert(bucket.colors.end(), type.vertexCount, 0u);
}
```

- [ ] **Step 7: Build, verify no errors**

- [ ] **Step 8: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(3c): add batcher batch-capture (getLastBuiltBatch) + submitCachedInstance"
```

---

## Task 4: GpuStaticPropRegistry new files + CMake

**Files:**
- Create: `GameOS/gameos/gos_static_prop_registry.h`
- Create: `GameOS/gameos/gos_static_prop_registry.cpp`
- Modify: `GameOS/gameos/CMakeLists.txt` (add new .cpp to gameos target)

### Data structures

```
s_recipes:         flat vector of GpuStaticPropInstance (all leaves of all registered trees)
s_recipeRanges:    per-registration RecipeRange {first, count, multi} — indexed by regIdx
s_liveRangeIndices: per-frame list of regIdx values (one per visible tree)
```

`registerRecipe(multi, batch)` appends N leaf instances as one range entry, stores `TG_MultiShape*`, returns regIdx.
`markVisible(regIdx)` appends regIdx to `s_liveRangeIndices` — no flat expansion yet.
`flush()` iterates regIdx values, reads `multi->getCachedGpuLightIndex()` (freshened by `touch()` this frame), patches `inst.lightDataIndex` in a stack copy, calls `batcher.submitCachedInstance()` per leaf.
`invalidate(regIdx)` zeroes the range's instance data, clears `multi`, sets count=0 (tombstone).

- [ ] **Step 1: Write `gos_static_prop_registry.h`**

Note: `<vector>` is required because the public API takes `std::vector<GpuStaticPropInstance>`. `gos_static_prop_batcher.h` already includes `<vector>` (line 5), `GpuStaticPropInstance`, and `msl.h` (for `TG_MultiShape`), so including it transitively covers all three. The explicit include is kept for clarity.

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include "gos_static_prop_batcher.h"

// TG_MultiShape forward-declared via gos_static_prop_batcher.h → msl.h.

namespace GpuStaticPropRegistry {

// One-time init / teardown.
void init();
void destroy();

// Returns true iff MC2_STATIC_PROP_REGISTRY=1 was set at startup.
bool isEnabled();

// Called once per frame from gamecam.cpp BEFORE land->render().
// Clears the per-frame live-range list.
void frameBegin();

// Called from TreeAppearance::render() after a successful first-time
// submitMultiShape(). Stores multi (for per-frame lightDataIndex patch)
// and snapshots the batch returned by GpuStaticPropBatcher::getLastBuiltBatch().
// Returns recipeIndex (>= 0) on success, -1 if disabled or OOM.
int32_t registerRecipe(TG_MultiShape* multi,
                       const std::vector<GpuStaticPropInstance>& batch);

// Called from TreeAppearance::render() when IsStaticNow() is true.
// Appends regIdx to the per-frame live list (no flat expansion here).
void markVisible(int32_t regIdx);

// Called when static registration must be cleared (fall, late-reg recovery,
// shape-pointer change). Sets the recipe range to count=0 (tombstone);
// caller must also clear TreeAppearance::staticReg.
void invalidate(int32_t regIdx);

// Returns true iff regIdx is valid and not invalidated (count > 0).
bool isReady(int32_t regIdx);

// Called from txmmgr.cpp BEFORE GpuStaticPropBatcher::instance().flush().
// For each live regIdx: reads multi->getCachedGpuLightIndex() (refreshed
// by touch() this frame), patches lightDataIndex in a stack copy of each
// leaf recipe, injects via submitCachedInstance(). Batcher flush() then
// draws everything in one combined GPU pass.
void flush();

} // namespace GpuStaticPropRegistry
```

- [ ] **Step 2: Write `gos_static_prop_registry.cpp`**

```cpp
#include "gos_static_prop_registry.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>   // UINT32_MAX

// Rejects "0", "false", "off", "no"; accepts anything else (including "1").
// Matches the ParseEnvBool pattern in code/terrobj.cpp:79-85.
static bool parseEnvBool(const char* name) {
    const char* v = getenv(name);
    if (!v || !*v) return false;
    if (v[0] == '0' && !v[1]) return false;
    if (!_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no")) return false;
    return true;
}

// Parsed at file-scope (program start) so isEnabled() is valid before init()
// is called and before the [INSTR v1] banner fires.
static const bool s_enabled = parseEnvBool("MC2_STATIC_PROP_REGISTRY");
static const bool s_trace   = parseEnvBool("MC2_STATIC_PROP_TRACE");

#define SP_TRACE(fmt, ...) \
    do { if (s_trace) { printf("[STATIC_PROP] " fmt "\n", ##__VA_ARGS__); \
         fflush(stdout); } } while (0)

namespace {

struct RecipeRange {
    uint32_t       first;   // index into s_recipes
    uint32_t       count;   // 0 = invalidated (tombstone)
    TG_MultiShape* multi;   // pointer to the TG_MultiShape for this tree.
                            // Used by flush() to read getCachedGpuLightIndex()
                            // for per-frame lightDataIndex patching.
                            // NULL when count==0 (tombstone).
};

static std::vector<GpuStaticPropInstance> s_recipes;
static std::vector<RecipeRange>           s_recipeRanges;

// Per-frame list of regIdx values (not flat leaf indices).
// markVisible() appends one regIdx per visible tree.
// flush() expands to leaves at draw time, patching lightDataIndex.
static std::vector<uint32_t>              s_liveRangeIndices;

} // namespace

namespace GpuStaticPropRegistry {

bool isEnabled() { return s_enabled; }

void init() {
    // Env flags already parsed at file scope. init() only reserves memory.
    if (s_enabled) {
        s_recipes.reserve(20000);
        s_recipeRanges.reserve(15000);
        s_liveRangeIndices.reserve(15000);
        printf("[STATIC_PROP] registry init: memory reserved\n");
        fflush(stdout);
    }
}

void destroy() {
    s_recipes.clear();          s_recipes.shrink_to_fit();
    s_recipeRanges.clear();     s_recipeRanges.shrink_to_fit();
    s_liveRangeIndices.clear(); s_liveRangeIndices.shrink_to_fit();
}

void frameBegin() {
    if (!s_enabled) return;
    s_liveRangeIndices.clear();
}

int32_t registerRecipe(TG_MultiShape* multi,
                       const std::vector<GpuStaticPropInstance>& batch) {
    if (!s_enabled || batch.empty() || !multi) return -1;
    RecipeRange rng;
    rng.first = static_cast<uint32_t>(s_recipes.size());
    rng.count = static_cast<uint32_t>(batch.size());
    rng.multi = multi;
    s_recipes.insert(s_recipes.end(), batch.begin(), batch.end());
    const int32_t regIdx = static_cast<int32_t>(s_recipeRanges.size());
    s_recipeRanges.push_back(rng);
    SP_TRACE("register regIdx=%d first=%u count=%u", regIdx, rng.first, rng.count);
    return regIdx;
}

void markVisible(int32_t regIdx) {
    if (!s_enabled) return;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    if (s_recipeRanges[static_cast<uint32_t>(regIdx)].count == 0) return;  // tombstone
    // Append regIdx directly; flush() does the leaf expansion + lightDataIndex patch.
    s_liveRangeIndices.push_back(static_cast<uint32_t>(regIdx));
}

void invalidate(int32_t regIdx) {
    if (!s_enabled) return;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    RecipeRange& rng = s_recipeRanges[static_cast<uint32_t>(regIdx)];
    for (uint32_t i = 0; i < rng.count; ++i)
        s_recipes[rng.first + i] = GpuStaticPropInstance{};
    SP_TRACE("invalidate regIdx=%d (was count=%u)", regIdx, rng.count);
    rng.count = 0;
    rng.multi  = nullptr;
}

bool isReady(int32_t regIdx) {
    if (!s_enabled) return false;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return false;
    return s_recipeRanges[static_cast<uint32_t>(regIdx)].count > 0;
}

void flush() {
    if (!s_enabled || s_liveRangeIndices.empty()) return;
    GpuStaticPropBatcher& batcher = GpuStaticPropBatcher::instance();
    for (uint32_t regIdx : s_liveRangeIndices) {
        const RecipeRange& rng = s_recipeRanges[regIdx];
        if (rng.count == 0 || !rng.multi) continue;  // tombstone guard

        // Patch lightDataIndex from CacheGpuLightData() result gathered
        // by touch() earlier this frame. resetLightData() clears the UBO
        // index table each frame, so the baked value in s_recipes is stale;
        // the getter reads the freshly-gathered per-frame slot.
        // UINT32_MAX means CacheGpuLightData() hasn't run yet — treat as 0
        // (no lighting data), matching submitMultiShape's gather-now fallback.
        const uint32_t freshLightIdx = rng.multi->getCachedGpuLightIndex();
        const uint32_t patchedIdx = (freshLightIdx == UINT32_MAX) ? 0u : freshLightIdx;

        for (uint32_t i = 0; i < rng.count; ++i) {
            GpuStaticPropInstance inst = s_recipes[rng.first + i]; // stack copy
            inst.lightDataIndex = patchedIdx;
            batcher.submitCachedInstance(inst);
        }
    }
    // batcher.flush() is called by the caller (txmmgr.cpp) immediately after.
}

} // namespace GpuStaticPropRegistry
```

- [ ] **Step 3: Add to CMakeLists.txt**

Find the gameos library target in `GameOS/gameos/CMakeLists.txt`. Locate where `gos_static_prop_batcher.cpp` is listed and add `gos_static_prop_registry.cpp` immediately below it:

```cmake
    gos_static_prop_registry.cpp
```

- [ ] **Step 4: Build, verify**

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_static_prop_registry.h GameOS/gameos/gos_static_prop_registry.cpp GameOS/gameos/CMakeLists.txt
git commit -m "feat(3c): add GpuStaticPropRegistry (frameBegin/registerRecipe/markVisible/invalidate/flush)"
```

---

## Task 5: TreeAppearance — header additions

**Files:**
- Modify: `mclib/bdactor.h` (inside `TreeAppearance` class, starting ~line 471)

- [ ] **Step 1: Add `StaticRegistration` struct and `staticReg` member**

Inside the `TreeAppearance` class, immediately after the `needsFullBakeNextFrame` field (~line 480), add:

```cpp
struct StaticRegistration {
    bool             registered;   // true iff recipeIndex is valid
    TG_MultiShapePtr shape;        // treeShape at registration time; detects swap
    int32_t          recipeIndex;  // index into GpuStaticPropRegistry s_recipeRanges
};
StaticRegistration staticReg;
```

- [ ] **Step 2: Add method declarations**

In the `public:` section of `TreeAppearance`, add after the existing `virtual void init(...)` declaration:

```cpp
virtual void touch() override;
virtual bool IsStaticNow() const override;
virtual void invalidateStaticRegistration() override;
```

- [ ] **Step 3: Build (declarations only, expect linker errors on unresolved symbols)**

- [ ] **Step 4: Commit**

```bash
git add mclib/bdactor.h
git commit -m "feat(3c): TreeAppearance::StaticRegistration struct + method declarations"
```

---

## Task 6: TreeAppearance — method implementations

**Files:**
- Modify: `mclib/bdactor.cpp`

The file must include `gos_static_prop_registry.h`. Add at the top of bdactor.cpp include list if not present:

```cpp
#include "gos_static_prop_registry.h"
```

- [ ] **Step 1: Zero-init `staticReg` in `TreeAppearance::init()`**

Find `TreeAppearance::init()` in bdactor.cpp. After the line that initializes `needsFullBakeNextFrame` (or any suitable location in the init body), add:

```cpp
staticReg = {};  // Stage 3.C: zero-init StaticRegistration
```

- [ ] **Step 2: Implement `TreeAppearance::touch()`**

Add after the last existing `TreeAppearance` method implementation:

```cpp
void TreeAppearance::touch() {
    if (treeShape) {
        treeShape->Touch();             // advance lastTurnTransformed on all leaves
        treeShape->CacheGpuLightData(); // refresh cachedGpuLightIndex_ for flush()
    }
}
```

`treeShape->Touch()` calls `TG_MultiShape::Touch()` which iterates `listOfShapes[i].node->Touch()` on each leaf. `treeShape->CacheGpuLightData()` (msl.cpp:1765) sets `cachedGpuLightIndex_` from `GatherGpuObjectLightDataOnly()` — this is the per-frame UBO slot index that `flush()` reads via `getCachedGpuLightIndex()` to patch stale recipe entries. Must be called every frame because `resetLightData()` invalidates all UBO indices each frame.

- [ ] **Step 3: Implement `TreeAppearance::IsStaticNow()`**

```cpp
bool TreeAppearance::IsStaticNow() const {
    // g_useGpuObjects: same gate as the actual GPU submit block in render().
    // gos_IsTerrainTessellationActive(): shadow safety — renderShadows() returns
    // NO_ERR immediately when tessellation is active (bdactor.cpp:4279), so
    // skipping update() is safe for shadow stamping.
    extern bool g_useGpuObjects;
    return GpuStaticPropRegistry::isEnabled()
        && g_useGpuObjects
        && gos_IsTerrainTessellationActive()
        && !needsFullBakeNextFrame
        && staticReg.registered
        && staticReg.recipeIndex >= 0
        && GpuStaticPropRegistry::isReady(staticReg.recipeIndex)
        && staticReg.shape == treeShape;
}
```

- [ ] **Step 4: Implement `TreeAppearance::invalidateStaticRegistration()`**

```cpp
void TreeAppearance::invalidateStaticRegistration() {
    if (staticReg.registered && staticReg.recipeIndex >= 0) {
        GpuStaticPropRegistry::invalidate(staticReg.recipeIndex);
    }
    staticReg = {};
}
```

- [ ] **Step 5: Build, verify no linker errors**

- [ ] **Step 6: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(3c): TreeAppearance::touch/IsStaticNow/invalidateStaticRegistration implementations"
```

---

## Task 7: TreeAppearance::render() — static branch + registration block

**Files:**
- Modify: `mclib/bdactor.cpp` (inside `TreeAppearance::render()`, starting ~line 4087)

### Static-path design

The static path sets `submittedToGpu = true` and falls through to the selection-visualization code (drawBars/drawBrackets at lines 4141–4161). It does NOT early-return, because those UI elements must still run if `selected` is non-zero.

### Structure of changes inside `if (g_useGpuObjects)` block (bdactor.cpp:4106)

- [ ] **Step 1: Add static-path branch at the top of the `if (g_useGpuObjects)` block**

The current code (lines 4106–4129):
```cpp
if (g_useGpuObjects)
{
    GpuStaticPropBatcher::instance().recordEligibleActor(
        GpuStaticPropPopulation::Tree);
    if (treeShape)
    {
        const char* callerName = (appearType ? appearType->name : nullptr);
        submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
            treeShape, GpuStaticPropPopulation::Tree, callerName);
        if (!submittedToGpu &&
            GpuStaticPropBatcher::instance().wasLastFailureLateRegistration())
        {
            needsFullBakeNextFrame = true;
        }
    }
    if (!submittedToGpu)
    {
        GpuStaticPropBatcher::instance().recordCpuFallback(
            GpuStaticPropPopulation::Tree);
    }
}
```

Replace with:

```cpp
if (g_useGpuObjects)
{
    GpuStaticPropBatcher::instance().recordEligibleActor(
        GpuStaticPropPopulation::Tree);

    // Stage 3.C: static registry fast path. If this tree's instance was
    // previously registered and lighting/position are still stable, inject
    // it into the batcher via markVisible() (processed at registry flush)
    // instead of running the full submitMultiShape() compute path.
    // Sets submittedToGpu=true to suppress CPU Render() fallback below.
    // Does NOT return early — selection visualization (drawBars/drawBrackets)
    // at lines 4141-4161 must still run if selected is non-zero.
    if (IsStaticNow()) {
        GpuStaticPropRegistry::markVisible(staticReg.recipeIndex);
        submittedToGpu = true;
    } else if (treeShape)
    {
        // Stage 3.C / M1: shape-swap invalidation. IsStaticNow()'s
        // staticReg.shape==treeShape check routes us here when treeShape was
        // reassigned (LOD swap at bdactor.cpp:3984, damage at ~3596/3626), but
        // staticReg.registered==true still blocks the registration block below.
        // Invalidate the stale entry first so the new shape gets registered.
        if (staticReg.registered && staticReg.shape != treeShape)
            invalidateStaticRegistration();

        const char* callerName = (appearType ? appearType->name : nullptr);
        submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
            treeShape, GpuStaticPropPopulation::Tree, callerName);
        if (!submittedToGpu &&
            GpuStaticPropBatcher::instance().wasLastFailureLateRegistration())
        {
            needsFullBakeNextFrame = true;
            invalidateStaticRegistration();  // clear stale registration if any
        }
        // Stage 3.C: registration block. On the first successful full-bake
        // submission with no late-reg flag, snapshot the leaf batch into the
        // registry. Subsequent frames use the static path above.
        // Pass treeShape as multi so flush() can patch lightDataIndex each frame.
        if (submittedToGpu && !staticReg.registered
                && GpuStaticPropRegistry::isEnabled()
                && !needsFullBakeNextFrame) {
            const auto& batch =
                GpuStaticPropBatcher::instance().getLastBuiltBatch();
            staticReg.recipeIndex = GpuStaticPropRegistry::registerRecipe(
                treeShape, batch);
            staticReg.registered  = (staticReg.recipeIndex >= 0);
            staticReg.shape        = treeShape;
        }
    }
    if (!submittedToGpu)
    {
        GpuStaticPropBatcher::instance().recordCpuFallback(
            GpuStaticPropPopulation::Tree);
    }
}
```

- [ ] **Step 2: Build, verify no errors**

- [ ] **Step 3: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(3c): TreeAppearance::render() static-registry fast path + registration block"
```

---

## Task 8: terrobj.cpp outer-skip gate changes

**Files:**
- Modify: `code/terrobj.cpp` (outer-skip gate at lines ~698–710)

### Two changes

1. **Before the skip gate:** call `appearance->invalidateStaticRegistration()` when `ownerForcesDynamic` so falling trees get a guaranteed full bake.

2. **In the skip branch:** call `appearance->touch()` to advance `lastTurnTransformed` so the staleness guard in `TG_Shape::Render()` doesn't trip.

- [ ] **Step 1: Add `invalidateStaticRegistration()` call before the gate**

Current code (line ~698):
```cpp
if (s_staticUpdateSkip && gpuPath && appearanceClaimsStatic && !ownerForcesDynamic) {
```

Add immediately BEFORE this line:
```cpp
// Stage 3.C: clear static registration on dynamic transition so the
// first post-fall frame gets a full bake and re-registers the final pose.
if (ownerForcesDynamic)
    appearance->invalidateStaticRegistration();
```

- [ ] **Step 2: Add `appearance->touch()` in the skip branch**

Current skip branch:
```cpp
if (s_staticUpdateSkip && gpuPath && appearanceClaimsStatic && !ownerForcesDynamic) {
    ++g_staticUpdateCounters.updates_skipped;
    if (s_staticUpdateTrace) {
        printf("[STATIC_UPDATE v1] event=skip frame=%u obj=%p\n",
            g_mc2FrameCounter, (void*)this);
        fflush(stdout);
    }
} else {
```

Change the skip branch body to:
```cpp
if (s_staticUpdateSkip && gpuPath && appearanceClaimsStatic && !ownerForcesDynamic) {
    ++g_staticUpdateCounters.updates_skipped;
    if (s_staticUpdateTrace) {
        printf("[STATIC_UPDATE v1] event=skip frame=%u obj=%p\n",
            g_mc2FrameCounter, (void*)this);
        fflush(stdout);
    }
    appearance->touch();  // Stage 3.C: advance lastTurnTransformed
} else {
```

- [ ] **Step 3: Build, verify no errors**

- [ ] **Step 4: Commit**

```bash
git add code/terrobj.cpp
git commit -m "feat(3c): terrobj.cpp — invalidateStaticRegistration on fall + touch() in skip branch"
```

---

## Task 9: Call-site wiring — gamecam.cpp + txmmgr.cpp

**Files:**
- Modify: `code/gamecam.cpp` (line ~200, before `land->render()`)
- Modify: `mclib/txmmgr.cpp` (line ~1474, before `GpuStaticPropBatcher::instance().flush()`)

- [ ] **Step 1: Add `#include "gos_static_prop_registry.h"` to gamecam.cpp**

Add near the top of gamecam.cpp include block (after existing `#ifndef GAMECAM_H` etc.):

```cpp
#include "gos_static_prop_registry.h"   // Stage 3.C: frameBegin()
```

- [ ] **Step 2: Add `GpuStaticPropRegistry::frameBegin()` in gamecam.cpp**

Find the `ZoneScopedN("GameCamera::render terrain")` block (line ~198–201):

```cpp
{
    ZoneScopedN("GameCamera::render terrain");
    land->render();
}
```

Change to:

```cpp
{
    ZoneScopedN("GameCamera::render terrain");
    GpuStaticPropRegistry::frameBegin();  // Stage 3.C: reset live-instance list
    land->render();
}
```

`frameBegin()` clears `s_liveInstanceIndices` before any `render()` calls, so no instances from the previous frame linger into the `markVisible()` accumulation phase.

- [ ] **Step 3: Add `#include` of `gos_static_prop_registry.h` to txmmgr.cpp**

txmmgr.cpp is in `mclib/`; the header is in `GameOS/gameos/`. The file already uses the explicit relative pattern (see `"../GameOS/gameos/gos_static_prop_batcher.h"` in the existing includes). Add near the top of txmmgr.cpp include block:

```cpp
#include "../GameOS/gameos/gos_static_prop_registry.h"   // Stage 3.C: flush()
```

- [ ] **Step 4: Add `GpuStaticPropRegistry::flush()` in txmmgr.cpp before batcher flush**

Find the existing batcher flush block (line ~1470–1476):

```cpp
{
    ZoneScopedN("Render.GpuStaticProps");
    TracyGpuZone("Render.GpuStaticProps");
    extern bool g_useGpuStaticProps;
    extern bool g_useGpuObjects;
    if (g_useGpuStaticProps || g_useGpuObjects) {
        GpuStaticPropBatcher::instance().flush();
    }
}
```

Change to:

```cpp
{
    ZoneScopedN("Render.GpuStaticProps");
    TracyGpuZone("Render.GpuStaticProps");
    extern bool g_useGpuStaticProps;
    extern bool g_useGpuObjects;
    if (g_useGpuStaticProps || g_useGpuObjects) {
        // Stage 3.C: inject static-registry instances into batcher buckets
        // BEFORE flush(), so they're drawn in the same combined GPU pass.
        GpuStaticPropRegistry::flush();
        GpuStaticPropBatcher::instance().flush();
    }
}
```

- [ ] **Step 5: Add `GpuStaticPropRegistry::init()` and `destroy()` call sites**

`GpuStaticPropBatcher::onMapLoad()` is called from **`code/mission.cpp:1644`** (not gameosmain.cpp — that file has zero results for `onMapLoad`). `onMapUnload()` is at **`code/mission.cpp:3171`**.

Add `#include "../GameOS/gameos/gos_static_prop_registry.h"` to the include block of `code/mission.cpp`.

At line 1644, immediately after `GpuStaticPropBatcher::instance().onMapLoad()`:
```cpp
GpuStaticPropRegistry::init();   // Stage 3.C
```

At line 3171, immediately after `GpuStaticPropBatcher::instance().onMapUnload()`:
```cpp
GpuStaticPropRegistry::destroy(); // Stage 3.C
```

Verify the include path is reachable: `mission.cpp` is in `code/`; `GameOS/gameos/` is a sibling directory from the repo root, so the relative path `"../GameOS/gameos/gos_static_prop_registry.h"` matches the established pattern (same as txmmgr.cpp's batcher include).

- [ ] **Step 6: Build, verify**

- [ ] **Step 7: Commit**

```bash
git add code/gamecam.cpp mclib/txmmgr.cpp code/mission.cpp
git commit -m "feat(3c): wire frameBegin()/flush()/init()/destroy() registry call sites"
```

---

## Task 10: gameosmain.cpp [INSTR v1] banner extension

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (banner at line ~716)

- [ ] **Step 1: Add `static_prop_registry` field to banner**

Find the `snprintf(_cbbuf, ...)` at line ~715. The current last variable is `static_update_skip=%d`. The buffer is 640 bytes.

Add `static_prop_registry=%d ` to the format string and pass `GpuStaticPropRegistry::isEnabled() ? 1 : 0` as the corresponding argument.

Current (end of format string and args, line ~720):
```cpp
"static_update_trace=%d static_update_skip=%d build=%s",
...
suTrace ? 1 : 0, suSkip ? 1 : 0, build);
```

Change to:
```cpp
"static_update_trace=%d static_update_skip=%d "
"static_prop_registry=%d build=%s",
...
suTrace ? 1 : 0, suSkip ? 1 : 0,
GpuStaticPropRegistry::isEnabled() ? 1 : 0, build);
```

Add `#include "gos_static_prop_registry.h"` to gameosmain.cpp if not already present from Task 9.

- [ ] **Step 2: Build, verify banner compiles**

Run mc2.exe with `MC2_STATIC_PROP_REGISTRY=1`, check log for `[INSTR v1] ... static_prop_registry=1`.

- [ ] **Step 3: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(3c): extend [INSTR v1] banner with static_prop_registry field"
```

---

## Task 11: Build and validation gate ladder

**No new files. Run in order.**

- [ ] **Step 1: Full build**

```
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
.claude/scripts/mc2-build.sh
```

Expected: zero errors, zero new warnings.

- [ ] **Step 2: Deploy**

```
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
.claude/scripts/mc2-deploy.sh
```

- [ ] **Step 3: Visual canary A — basic visibility**

Launch with `MC2_STATIC_PROP_REGISTRY=1 MC2_STATIC_UPDATE_SKIP=1`. Load mc2_01. Verify:
- Trees visible on load
- Camera pan: trees remain visible throughout pan
- No black silhouettes

- [ ] **Step 4: Visual canary B — camera exit + re-entry**

Pan camera away until trees are off-screen. Pan back. Verify:
- Trees reappear correctly (no missing trees, no position errors)
- Receipt: `[STATIC_PROP] register` lines in log once per tree type, not every frame

- [ ] **Step 5: Visual canary C — fall/destruction**

Shoot trees until one falls. Verify:
- Fall animation plays correctly (tree is dynamic during fall)
- Resting pose is stable after fall completes
- On camera exit + return: fallen tree still visible in final pose

- [ ] **Step 6: Tracy delta measurement**

Run mc2_01 with Tracy connected, `MC2_STATIC_PROP_REGISTRY=1 MC2_STATIC_UPDATE_SKIP=1` (both required for full Stage 3.C benefit). Measure `TerrainObject::update appearanceUpdate` zone. Compare to baseline (neither env var set). Expected: ~1.2ms reduction. Record actual delta.

**Partial-mode note:** `MC2_STATIC_PROP_REGISTRY=1` alone (without `MC2_STATIC_UPDATE_SKIP=1`) exercises only the render-side cached-submission path and will not move the `TerrainObject::update` Tracy zone, because `TreeAppearance::update()` still runs on every frame for every tree.

Also check: no regression in `Render.GpuStaticProps` GPU zone (static instances draw in the same batcher pass, no extra draw calls).

- [ ] **Step 7: Tier-1 smoke**

```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1
```

Expected: 5/5 PASS, `+0 destroys` delta on all missions.

- [ ] **Step 8: Commit smoke result**

If all pass, commit the smoke gate record:

```bash
git commit --allow-empty -m "chore(3c): tier1 smoke PASS (5/5, +0 destroys) — static_prop_registry soak begins"
```

---

## Self-review checklist

**Spec coverage:**
- TGL Touch() layer ✓ (Task 1)
- Appearance base virtuals ✓ (Task 2)
- Batcher batch capture + submitCachedInstance ✓ (Task 3)
- Registry implementation ✓ (Task 4)
- TreeAppearance declarations ✓ (Task 5)
- TreeAppearance implementations ✓ (Task 6)
- render() static branch + registration ✓ (Task 7)
- terrobj.cpp outer-skip changes ✓ (Task 8)
- Call sites (frameBegin/flush/init/destroy) ✓ (Task 9)
- [INSTR v1] banner ✓ (Task 10)
- Build + smoke gate ✓ (Task 11)

**Placeholder scan:** none found.

**Type consistency across tasks:**
- `int32_t recipeIndex` used uniformly (Tasks 4, 5, 6, 7)
- `std::vector<GpuStaticPropInstance>` batch type used in Task 3 `getLastBuiltBatch()` and Task 4 `registerRecipe()` ✓
- `TG_MultiShapePtr` for `staticReg.shape` matches `treeShape` field type ✓
- `uint32_t` for `getCachedGpuLightIndex()` return and `lightDataIndex` field (both verified against msl.h:275 and gos_static_prop_batcher.h:18) ✓
- `TG_MultiShape*` (raw pointer) for `RecipeRange::multi` — safe because the registry's `invalidate()` is called by `invalidateStaticRegistration()` before any tree is destroyed; pointer lifetime matches tree lifetime ✓

**Blocker resolutions referenced:** B1–B5, MI-1, MI-2, C1, C2, M1 — all addressed in the "Blocker resolutions" section above.
