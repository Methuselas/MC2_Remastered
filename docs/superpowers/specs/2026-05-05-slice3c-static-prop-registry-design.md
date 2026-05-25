# Slice 3.C — StaticPropRegistry Design

**Goal:** Eliminate per-frame `CacheGpuLightData()` + `TransformMultiShape_PositionsOnly()` + `submitMultiShape()` for tree instances whose world-space lighting and position are stable, by caching a per-instance GPU submission snapshot in a `GpuStaticPropRegistry` and replaying it each frame via a compact live-instance list.

**Scope:** Trees (`TreeAppearance`) only. Buildings, walls, and bridges are Stage 3.D candidates after this path soaks.

**Architecture:** Modeled after the water SSBO template — persistent recipe data (CPU-side snapshot of `GpuStaticPropInstance`) plus a per-frame live-instance list (compact index array). The existing per-frame instance SSBO format is reused unchanged. No new SSBO, no shader change.

**Performance target:** ~1.2ms/frame reduction in Tracy `TerrainObject::update appearanceUpdate` zone on mc2_01. Do not claim more than the measured zone total.

**Env gate:** `MC2_STATIC_PROP_REGISTRY=1` (opt-in). Default off for initial soak.

---

## Section 1 — What the recon changed

The earlier spec ("persistent static-color SSBO") was based on the premise that `CacheGpuLightData()` feeds a per-vertex color SSBO that the GPU reads every frame. Recon disproved this:

- The GPU static-prop shader (`static_prop.vert`) does **not** read from the Colors SSBO (binding 1) in the normal render path. Lighting is computed GPU-side via `calc_light()` using type-level `a_aRGBLight` vertex attributes plus per-instance `fogRGB`/`aRGBHighlight`/`lightDataIndex` from `GpuStaticPropInstance`.
- The Colors SSBO is declared and uploaded but read only in frag debug mode (`u_debugAddrMode == 4`).
- What `CacheGpuLightData()` actually produces are the per-instance fields in `GpuStaticPropInstance`: `fogRGB`, `aRGBHighlight`, `lightDataIndex`. These are stable for terrain objects whose lighting environment is stable.

**Therefore:** the optimization target is not a per-vertex color SSBO — it is the `GpuStaticPropInstance` struct (96 bytes) that is re-built and re-uploaded every frame for every visible tree. For a static tree at a fixed terrain position with stable lighting, the full struct (including `modelMatrix`) is constant between invalidation events. Caching it CPU-side and replaying it eliminates `CacheGpuLightData()`, `TransformMultiShape_PositionsOnly()`, and the full `submitMultiShape()` computation path.

### Frame lifecycle (from recon)

- `TerrainObject::update()` and `TerrainObject::render()` are **separate functions** in terrobj.cpp with separate callers and separate visibility guards (`inView` vs `canBeSeen() || g_useGpuStaticProps`). The outer-skip gate in Stage 3.B is in `update()` only; `render()` is always called independently.
- `submitMultiShape()` (bdactor.cpp:4114) is called from `TreeAppearance::render()`, not from `update()`.
- `flush()` is called from txmmgr.cpp:1475 inside `renderLists()`, after all render calls complete.

This means: skipping `update()` alone does not skip `submitMultiShape()`. To eliminate the per-frame submission cost, the static path must also intercept `TreeAppearance::render()`.

### Static-primary invariant (from recon Q3)

The GPU eligibility gate at tgl.cpp (`eligibleForGpuObjects()`) is **type-level and persistent** — it is based on type registration in `s_typeIndex`, not on per-frame submission. If a tree's type is registered but no instance is submitted this frame (neither via dynamic batcher nor via static registry), the CPU `addRenderShape` path is suppressed, producing a black silhouette or disappearance. The static path **must** emit a GPU instance record every visible frame, via `GpuStaticPropRegistry::markVisible()`.

---

## Section 2 — TGL layer: `TG_Shape::Touch()` / `TG_MultiShape::Touch()`

The staleness guard at tgl.cpp:2876 returns early if `lastTurnTransformed != turn`. When `TerrainObject::update()` skips `appearance->update()`, `TransformMultiShape_PositionsOnly()` never runs, so `lastTurnTransformed` goes stale. `Touch()` advances the stamp without touching vertex data.

**`TG_Shape::Touch()`** (new, ~2 lines in tgl.cpp, declared in tgl.h):
```cpp
void TG_Shape::Touch() {
    lastTurnTransformed = turn;
}
```

**`TG_MultiShape::Touch()`** (new, iterates child shapes):
```cpp
void TG_MultiShape::Touch() {
    for (long i = 0; i < numShapes; i++)
        if (shapeList[i])
            shapeList[i]->Touch();
}
```

---

## Section 3 — Appearance hierarchy: `touch()`, `IsStaticNow()`, `Appearance` base

**`Appearance::touch()`** (default no-op, added to appear.h adjacent to `IsStaticNow`):
```cpp
virtual void touch() {}
```

**`TreeAppearance::touch()`** (new override in bdactor.cpp):
```cpp
void TreeAppearance::touch() {
    if (treeShape)
        treeShape->Touch();
    // markVisible() is called from render(), not here.
    // touch() is only the timestamp maintenance path.
}
```

**Why markVisible() is NOT in touch():** `TerrainObject::update()` and `TerrainObject::render()` are separate functions. Putting `markVisible()` in touch() would advance the timestamp in update() but still leave `TreeAppearance::render()` calling `submitMultiShape()` every frame — the optimization would not fire. Instead, the render() path is the correct location for `markVisible()` because that is where all GPU submission decisions are made.

**`TreeAppearance::IsStaticNow()`** (re-added — removed in commit `09e32da`, now safe with registry as authoritative GPU path):
```cpp
bool TreeAppearance::IsStaticNow() const {
    // Tessellation guard: TreeAppearance::renderShadows (bdactor.cpp:4279)
    // returns NO_ERR immediately when tessellation is active. Without this
    // guard, a future tessellation disable could make touch() unsafe for shadows.
    return s_staticPropRegistryEnabled
        && g_useGpuStaticProps            // killswitch guard
        && gos_IsTerrainTessellationActive()
        && !needsFullBakeNextFrame
        && staticReg.registered && staticReg.recipeIndex >= 0
        && staticReg.shape == treeShape;  // shape-swap guard for fall detection
}
```

**`TreeAppearance::StaticRegistration`** (per-instance registration state — simplified per spec revision):
```cpp
struct StaticRegistration {
    bool             registered;
    TG_MultiShapePtr shape;        // snapshot at register time; detects shape swap
    int32_t          recipeIndex;  // -1 = not registered; index into registry recipe table
};
```

Added as a member to `TreeAppearance` (in bdactor.h), zero-initialized in `TreeAppearance::init()`:
```cpp
StaticRegistration staticReg;
// in init(): staticReg = {};
```

The recipe data (`GpuStaticPropInstance` snapshot) lives in the registry's recipe table, not on the appearance. `TreeAppearance` holds only the index.

---

## Section 4 — GpuStaticPropRegistry

New files: `GameOS/gameos/gos_static_prop_registry.{h,cpp}`.

The registry owns two data structures:

```cpp
// Persistent CPU-side recipe table (grows as instances register, shrinks only on destroy())
std::vector<GpuStaticPropInstance> s_recipes;   // full instance snapshots (including modelMatrix)
std::vector<uint32_t>              s_typeKeys;   // parallel: typeKey for each recipe entry

// Per-frame live list (cleared at frameBegin(), populated by markVisible() during render phase)
std::vector<uint32_t>              s_liveRecipeIndices;  // indices into s_recipes
```

No new GPU SSBO is introduced. `flush()` builds the per-frame `GpuStaticPropInstance` array by reading from `s_recipes` for each live index, then uploads to the **existing per-frame instance SSBO** alongside (or immediately after) the dynamic batcher's flush.

### API

```cpp
namespace GpuStaticPropRegistry {

// Called once per frame, from gamecam.cpp before land->render() (before any render calls).
void frameBegin();

// Called from TreeAppearance::render() after first successful submitMultiShape().
// Snapshots the fully-built GpuStaticPropInstance from the batcher.
// Returns the recipeIndex (>= 0 on success, -1 if registry is disabled or full).
int32_t registerRecipe(const GpuStaticPropInstance& snapshot, uint32_t typeKey);

// Called from TreeAppearance::render() when IsStaticNow() is true.
// Appends recipeIndex to the per-frame live list.
void markVisible(int32_t recipeIndex);

// Called when static registration must be cleared:
// - fall start (ownerForcesDynamic)
// - late-registration recovery
// - shape pointer change
// Sets s_recipes[recipeIndex] to a zeroed sentinel; caller must clear staticReg.
void invalidate(int32_t recipeIndex);

// Returns true iff recipeIndex is valid and not invalidated.
bool isReady(int32_t recipeIndex);

// Called from the post-renderLists hook (after dynamic batcher flush).
// Iterates s_liveRecipeIndices, uploads recipe data to per-frame instance SSBO,
// issues per-type glDrawElementsInstanced calls.
void flush();

void init();   // called at batcher init
void destroy(); // called at mission unload

} // namespace GpuStaticPropRegistry
```

### Snapshot capture

The `GpuStaticPropInstance` snapshot is captured from the batcher's internally-built instance data immediately after a successful `submitMultiShape()` call. The batcher exposes a new `getLastBuiltInstance() const → const GpuStaticPropInstance&` query method; this returns a const reference valid only until the next `submitMultiShape()` call. The plan will confirm the exact batcher-side implementation (likely a file-static cached-instance field written at the end of the successful submit path, around gos_static_prop_batcher.cpp:1145 where `ObserveSubmittedShape()` already fires).

### frameBegin() call site

`frameBegin()` is called **once per frame** from gamecam.cpp, immediately before the terrain/object render cycle begins (before land->render() which cascades to all TerrainObject::render() calls). This is around gamecam.cpp:200 per recon Q2. The plan will confirm the exact insertion point. It must not be inside the object update or render loops — exactly once per frame at the frame-render boundary.

### No double-draw guarantee

Registration frame (frame N=0 for any tree):
1. `IsStaticNow()` → false → full `update()` runs → `CacheGpuLightData()` + `TransformMultiShape_PositionsOnly()`
2. `render()` → `IsStaticNow()` → false → `submitMultiShape()` → success → `registerRecipe()` → `staticReg.registered=true`
3. Dynamic batcher `flush()` draws the instance. Registry live list is empty (no `markVisible()` was called). No static draw.

Stable frames (frame N≥1):
1. `update()` → `IsStaticNow()` → true → `touch()` → `TG_MultiShape::Touch()`
2. `render()` → `IsStaticNow()` → true → `markVisible(recipeIndex)` → returns (no `submitMultiShape()`)
3. Dynamic batcher has no instance for this tree. Registry `flush()` draws from cached recipe.

No double-draw in either phase.

---

## Section 5 — TreeAppearance::render() changes

```cpp
// At start of the inView/g_useGpuStaticProps-gated block (after bdactor.cpp:4092 guard):

if (IsStaticNow()) {
    // Static registry path: emit markVisible, skip full submitMultiShape().
    // GPU eligibility gate (tgl.cpp eligibleForGpuObjects) suppresses CPU addRenderShape
    // for registered types; markVisible() here ensures a GPU instance is still emitted.
    GpuStaticPropRegistry::markVisible(staticReg.recipeIndex);
    // Non-batcher render work (selection bars, brackets, debug) may still run here.
    // DO NOT call submitMultiShape() — that would submit to the dynamic batcher and
    // double-draw the instance.
    return NO_ERR;
}

// Normal dynamic path: submitMultiShape() as before.
// ...
bool submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(...);

// NEW: snapshot registration after first successful full-bake submission.
if (submittedToGpu && !needsFullBakeNextFrame
        && !staticReg.registered && s_staticPropRegistryEnabled) {
    const GpuStaticPropInstance& snap =
        GpuStaticPropBatcher::instance().getLastBuiltInstance();
    staticReg.recipeIndex = GpuStaticPropRegistry::registerRecipe(snap, typeKey);
    staticReg.registered  = (staticReg.recipeIndex >= 0);
    staticReg.shape        = treeShape;
}
```

---

## Section 6 — terrobj.cpp changes

### update() outer-skip (existing Stage 3.B gate, minimal change)

```cpp
// Existing from Stage 3.B (terrobj.cpp ~line 698):
if (ownerForcesDynamic) {
    // NEW: invalidate static registration so fall gets a guaranteed full bake.
    if (staticReg(appearance).registered)
        GpuStaticPropRegistry::invalidate(staticReg(appearance).recipeIndex);
    appearance->invalidateStaticRegistration();  // clears staticReg.registered
}

if (!appearance->IsStaticNow())
    appearance->update(animate);
else
    appearance->touch();   // advance lastTurnTransformed; markVisible() in render()
```

`invalidateStaticRegistration()` is a new virtual on `Appearance` (default no-op, `TreeAppearance` override clears `staticReg.registered = false` and `staticReg.recipeIndex = -1`).

### render() — no changes to terrobj.cpp

The static/dynamic branch is handled inside `TreeAppearance::render()` (Section 5). `TerrainObject::render()` continues to call `appearance->render()` unconditionally (for visible objects). This keeps the terrobj change minimal.

---

## Section 7 — Registration and invalidation lifecycle (trees-only scope)

### First registration

`IsStaticNow()` → false (not registered) → full `update()` → `render()` → `submitMultiShape()` → success → `registerRecipe()` → `staticReg.registered = true`.

### Stable static frames

`IsStaticNow()` → true → `touch()` advances stamp → `render()` calls `markVisible()` → registry emits instance from cached recipe. No `CacheGpuLightData()`, no `TransformMultiShape_PositionsOnly()`, no `submitMultiShape()`.

### Camera exit / re-entry

When tree leaves view (`canBeSeen()` = false), `render()` is not called → `markVisible()` not called → tree absent from live list → not drawn. Recipe data persists. On camera return: `render()` called → `markVisible()` → drawn from same cached recipe. No re-upload.

### Late-registration invalidation

Batcher late-registration path already sets `needsFullBakeNextFrame = true` (bdactor.cpp:4118-4122). Add: call `GpuStaticPropRegistry::invalidate(recipeIndex)` and `staticReg.registered = false`. Next frame: `IsStaticNow()` → false → full `update()` + `render()` → re-register.

### Falling tree (guaranteed final-pose bake)

Owner-side `OBJECT_FLAG_FALLING` check in terrobj.cpp sets `ownerForcesDynamic = true`. This triggers `invalidateStaticRegistration()` at the outer-skip gate before the flag is evaluated, clearing `staticReg.registered`. For the duration of the fall, `IsStaticNow()` returns false, full `update()` + `render()` run each frame. After the fall flag clears: first `render()` after a successful `submitMultiShape()` with `!needsFullBakeNextFrame` re-registers the final pose. No manual re-registration step is needed.

### Shape pointer change

`IsStaticNow()` checks `staticReg.shape == treeShape`. If `treeShape` is rebuilt (e.g., a path that swaps the shape pointer), the mismatch forces `IsStaticNow()` false, full `update()` + `render()` run, and the new shape is registered.

### Mission unload

`GpuStaticPropRegistry::destroy()` clears recipe table and live list. `TreeAppearance::destroy()` zeroes `staticReg` (already called on mission unload via existing cleanup path).

### Not in Stage 3.C scope

- `BldgAppearance` static path (Stage 3.D)
- Building damage/destruction transitions
- Power-state / scripted effect changes
- Per-instance lighting epoch invalidation (day/night changes)

---

## Section 8 — Memory, performance, and validation

### Memory

- `s_recipes`: ~15K tree instances × 96 bytes = ~1.4 MB CPU-side. Bounded by object count.
- `s_liveRecipeIndices`: ~15K uint32 = ~60 KB per frame.
- No new GPU memory.

### Performance target

Reduce Tracy `TerrainObject::update appearanceUpdate` zone by ~1.2ms/frame on mc2_01. Measured before/after on that zone; do not claim more than the measured zone total. Also check Tracy `TreeAppearance::render` zone.

### Validation gate ladder

- **A. Visual canary:** trees visible on load, survive camera pan, survive pan-away + camera return, no black silhouettes.
- **B. Fall canary:** one tree falls (animation plays), rests in final pose, remains visible on camera return.
- **C. Tracy delta:** `TerrainObject::update appearanceUpdate` zone before/after with `MC2_STATIC_PROP_REGISTRY=1`.
- **D. Tier1 5/5 PASS:** `+0 destroys` delta on all five missions.

---

## Section 9 — Debug instrumentation

```cpp
static const bool s_staticPropTrace = ParseEnvBool("MC2_STATIC_PROP_TRACE", false);
#define SP_TRACE(fmt, ...) \
    do { if (s_staticPropTrace) { printf("[STATIC_PROP] " fmt "\n", ##__VA_ARGS__); \
         fflush(stdout); } } while (0)
```

Log at: first registration (recipeIndex, typeKey), invalidation (reason string), SSBO capacity growth. Never per-frame.

`[INSTR v1]` banner extension: add `static_prop_registry` field to the existing banner in gameosmain.cpp.

---

## Files to create / modify

| File | Change |
|---|---|
| `mclib/tgl.h` | Declare `TG_Shape::Touch()`, `TG_MultiShape::Touch()` |
| `mclib/tgl.cpp` | Implement both Touch() methods |
| `mclib/appear.h` | Add `virtual void touch() {}`, `virtual void invalidateStaticRegistration() {}`, re-declare `IsStaticNow()` default |
| `mclib/bdactor.h` | Add `StaticRegistration` struct; add `staticReg` member to `TreeAppearance`; declare `touch()`, `IsStaticNow()`, `invalidateStaticRegistration()` overrides |
| `mclib/bdactor.cpp` | Implement `TreeAppearance::touch()`, `IsStaticNow()`, `invalidateStaticRegistration()`; add static-path branch + registration to `render()`; zero-init `staticReg` in `init()` |
| `code/terrobj.cpp` | Add `invalidateStaticRegistration()` call before `ownerForcesDynamic` skip; change else-nothing to `else appearance->touch()` |
| `GameOS/gameos/gos_static_prop_batcher.h` | Declare `getLastBuiltInstance()` query method |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | Implement `getLastBuiltInstance()` (cache last-built instance at success path ~line 1145) |
| `GameOS/gameos/gos_static_prop_registry.h` | New file: registry API, `StaticPropRecipe` type alias |
| `GameOS/gameos/gos_static_prop_registry.cpp` | New file: `frameBegin()`, `registerRecipe()`, `markVisible()`, `invalidate()`, `isReady()`, `flush()`, `init()`, `destroy()` |
| `code/gamecam.cpp` | Add `GpuStaticPropRegistry::frameBegin()` call before land->render() (~line 200) |
| `GameOS/gameos/gameosmain.cpp` | Extend `[INSTR v1]` banner with `static_prop_registry` field |
