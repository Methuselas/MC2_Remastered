# Slice 3.C — Persistent Static-Color SSBO Design

**Goal:** Eliminate per-frame `CacheGpuLightData()` + color staging upload for tree instances whose terrain lighting is stable, by moving per-vertex color data into a persistent GPU buffer that is only re-uploaded on explicit invalidation.

**Scope:** Trees (`TreeAppearance`) only. Buildings, walls, and bridges are Stage 3.D candidates after this path soaks.

**Architecture:** Three-layer split following the water SSBO template: (1) persistent color SSBO as the "static recipe" (populated once, lives until explicit invalidation); (2) per-frame thin matrix upload appended to the existing ring-buffered matrix SSBO as the "live filter"; (3) `touch()` in the outer-skip loop replaces full `update()` for registered static instances.

**Tech stack:** OpenGL 4.3 / SSBO / std430; existing `gos_static_prop_batcher`; `TreeAppearance` / `TG_MultiShape`; `terrobj.cpp` outer-skip gate (Stage 3.A/3.B infrastructure).

**Performance target:** ~1.2ms/frame reduction in Tracy `TerrainObject::update appearanceUpdate` zone on mc2_01. Do not claim more than the measured zone cost.

**Env gate:** `MC2_STATIC_COLOR_SSBO=1` (opt-in for the initial soak). Default off until tier1 soaks clean.

---

## Section 1 — Goal and scope

Stage 3.B established that outer-skip of `appearance->update()` is too coarse: it skips `CacheGpuLightData()`, `TransformMultiShape_PositionsOnly()`, and GPU batcher `submit()` in a single cut, producing black silhouettes and permanent disappearance. The root cause is that the per-frame color staging and the per-frame `lastTurnTransformed` stamp are both produced inside `update()` with no way to separate them at the call site.

Stage 3.C moves the color data to a persistent GPU buffer so it can survive the skip. The per-frame work that remains is: (a) advancing `lastTurnTransformed` via `Touch()`, and (b) appending a matrix update for the visible instance to the existing ring buffer. Everything else — vertex light gather, argb copy, color SSBO write — happens only on the frame the instance first registers, and again only if explicitly invalidated.

The outer-skip gate in `terrobj.cpp` (already wired) fires when `appearance->IsStaticNow()` returns true. On that path, `appearance->touch()` is called instead of `appearance->update()`.

### Static-primary invariant (load-bearing)

`IsStaticNow()` may return true **only** when:
1. The shape is registered in the persistent GPU static pass, AND
2. The primary color draw will be served by that pass.

This invariant is satisfied by construction: the GPU eligibility gate at `tgl.cpp:2816-2820` already strips `addRenderShape` for GPU-eligible shapes before `Render()` can add them to the CPU draw list. `Touch()` only advances `lastTurnTransformed` for any downstream staleness check — the CPU color path is suppressed by the eligibility gate, not by the stamp. The spec adds a comment at the `IsStaticNow()` call site to make this explicit.

### Shadow path (non-issue for Stage 3.C)

`TreeAppearance::renderShadows()` (`bdactor.cpp:4279`) returns `NO_ERR` immediately when `gos_IsTerrainTessellationActive()`. Tessellation is always active. Tree blob shadows are already unconditionally suppressed. Skipping `TransformMultiShape_PositionsOnly` has no effect on the shadow path.

---

## Section 2 — TGL layer: `TG_Shape::Touch()` / `TG_MultiShape::Touch()`

The staleness guard at `tgl.cpp:2876` returns early if `lastTurnTransformed != turn`. When `update()` is skipped, `TransformMultiShape_PositionsOnly` never runs, so `lastTurnTransformed` goes stale and any path that inspects the stamp sees the shape as not-this-frame.

**`TG_Shape::Touch()`** (new, ~2 lines in `tgl.cpp`, declared in `tgl.h`):
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

No vertex reads, no light gather, no SSBO write. This is the only new code in `tgl`.

---

## Section 3 — Appearance hierarchy: `touch()` virtual / `TreeAppearance` overrides

**`Appearance::touch()`** (default no-op, added to `appear.h` adjacent to `IsStaticNow`):
```cpp
virtual void touch() {}
```

**`TreeAppearance::touch()`** (new override in `bdactor.cpp`):
```cpp
void TreeAppearance::touch() {
    if (treeShape)
        treeShape->Touch();
}
```

**`TreeAppearance::IsStaticNow()`** (re-added — was removed in commit `09e32da` after outer-skip proved too coarse; now safe with persistent GPU pass as the authoritative draw path):
```cpp
bool TreeAppearance::IsStaticNow() const {
    // Static-primary invariant: tgl.cpp:2816-2820 already strips addRenderShape
    // for GPU-eligible shapes. Touch() is only valid here because the GPU static
    // pass is the authoritative color draw for registered instances.
    return !needsFullBakeNextFrame
        && staticReg.registered
        && staticReg.shape == treeShape;
}
```

The `staticReg.shape == treeShape` guard handles falling-tree shape swaps: if the instance rebuilds its `treeShape` pointer (e.g., pitch change), the pointer mismatch forces `IsStaticNow()` false on the next frame, `update()` runs in full, and the new shape is re-registered.

---

## Section 4 — Per-instance registration: `StaticRegistration` struct

Keying the persistent SSBO lookup by raw `TG_MultiShapePtr` alone is fragile: pointer reuse after dealloc could make a newly-allocated shape appear already registered. Instead, registration state is embedded directly in `TreeAppearance`:

```cpp
struct StaticRegistration {
    bool             registered;
    TG_MultiShapePtr shape;         // snapshot at register time
    uint32_t         typeKey;
    uint32_t         colorOffsetWords;  // index into uint32 persistent color SSBO
    uint32_t         vertexCount;
    uint32_t         instanceSlot;  // slot in s_staticInstancesByType[typeKey]
};
```

Added as a member to `TreeAppearance` (in `bdactor.h`):
```cpp
StaticRegistration staticReg;
```

Initialized in `TreeAppearance::init()`:
```cpp
staticReg = {};  // zero-init; registered=false
```

`isStaticRegistered()` disappears from the batcher public API. All registration state lives on the appearance.

**`colorOffsetWords` vs bytes:** the shader indexes into the persistent SSBO as `colorSsbo[colorOffsetWords + localVertIdx]` (uint32 array indexing). OpenGL `glBufferSubData` uses byte offsets. The implementation converts at upload time: `byteOffset = colorOffsetWords * sizeof(uint32_t)`. Never store or pass a byte offset to the shader.

---

## Section 5 — GpuStaticPropBatcher: persistent SSBO and `submitStatic()`

### New global state (alongside existing ring-buffer globals)

```cpp
// Persistent per-vertex color buffer — never rotated, cleared only on full deregister
GLuint   s_staticColorSsbo         = 0;
uint32_t s_staticColorSsboCapacity = 0;   // words allocated
uint32_t s_staticColorSsboUsed     = 0;   // words committed

struct StaticInstance {
    uint32_t colorOffsetWords;  // index into s_staticColorSsbo uint32 array
    uint32_t vertexCount;
    uint32_t baseVertex;        // for glDrawElementsInstancedBaseVertex
    uint32_t matrixSlotThisFrame; // assigned during flush; indexes ring-buffer matrix region
    bool     visibleThisFrame;
};

// Per-type persistent instance list — NOT cleared per frame
std::unordered_map<uint32_t, std::vector<StaticInstance>> s_staticInstancesByType;
```

The ring-buffered `s_bucketsByType` and `s_colorSsbo[RING_FRAMES]` are unchanged. Dynamic objects continue using them exactly as before.

### `submitStatic(TreeAppearance* appr, uint32_t typeKey, uint32_t baseVertex)`

Called at the end of the existing `submit()` path for GPU-eligible trees when `!appr->needsFullBakeNextFrame`:

1. If `appr->staticReg.registered && appr->staticReg.shape == appr->treeShape`: mark `visibleThisFrame = true`, assign `matrixSlotThisFrame` from the ring buffer tail pointer. Return — color data already in SSBO.
2. Otherwise (first registration or re-registration after invalidation):
   - Upload `shape->listOfVertices[0..vertexCount-1].argb` to `s_staticColorSsbo` via `glBufferSubData` at byte offset `s_staticColorSsboUsed * 4`.
   - Fill `appr->staticReg`: `registered=true`, `shape=treeShape`, `typeKey`, `colorOffsetWords=s_staticColorSsboUsed`, `vertexCount`, `instanceSlot=s_staticInstancesByType[typeKey].size()`.
   - Advance `s_staticColorSsboUsed += vertexCount`.
   - Append a `StaticInstance` to `s_staticInstancesByType[typeKey]`.

"Might emit" inclusion: upload colors even if the instance is not in view this frame (i.e., `!inView`). The per-frame matrix upload is the live filter. Persistent color data covers all registered instances so a camera-return doesn't re-trigger an upload.

### SSBO allocation

Allocated at batcher init alongside the ring buffer:
```cpp
glGenBuffers(1, &s_staticColorSsbo);
glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_staticColorSsbo);
glBufferData(GL_SHADER_STORAGE_BUFFER,
             INITIAL_COLORS_PER_FRAME * sizeof(uint32_t),
             nullptr, GL_STATIC_DRAW);
s_staticColorSsboCapacity = INITIAL_COLORS_PER_FRAME;
```

If capacity is exceeded: double-and-realloc with a full re-upload. Acceptable because it happens at most once per session (bounded by unique-shape vertex count, not instance count).

### Matrix ring buffer reuse (Amendment 2)

Static instance matrices go into the **existing** ring-buffered matrix SSBO as a trailing region after the dynamic instance data, within the same `glBufferSubData` upload during `flush()`. `matrixSlotThisFrame` is assigned as `dynamicInstanceCount + staticInstanceIndex`. `glDrawElementsInstancedBaseInstance` uses this offset.

This reuses the existing fence/sync infrastructure with no new buffer.

### Modified `flush()`

After the existing per-type dynamic draw loop:
```cpp
// Static pass: bind persistent color SSBO
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, COLOR_SSBO_BINDING, s_staticColorSsbo);
for (auto& [typeKey, instances] : s_staticInstancesByType) {
    // Only draw instances visible this frame
    uint32_t visibleCount = buildStaticMatrixRegion(typeKey, instances);
    if (visibleCount == 0) continue;
    glDrawElementsInstancedBaseVertex(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT,
        indexOffset, visibleCount, instances[0].baseVertex);
}
// Restore ring-buffer color SSBO for next frame's dynamic flush
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, COLOR_SSBO_BINDING,
    s_colorSsbo[s_currentFrame % RING_FRAMES]);
```

One extra draw call per type with static instances. At ~30 active types, ~30 extra draw calls/frame — negligible at MC2's budget.

### State save/restore (Amendment 4, from `static_prop_projection.md`)

The static flush runs inside the existing `flush()` state envelope. The SSBO slot rebind (`s_staticColorSsbo` → restore to ring buffer) is added to the existing restore block at flush-exit. No second save/restore block.

### TERRAIN_DEPTH_FUDGE (Amendment 3)

The static pass reuses the same VS as the dynamic pass. That VS already applies `screen.z = clip.z * rhw + 0.001` to match `TERRAIN_DEPTH_FUDGE`. No shader change needed.

### Matrix convention (Amendment 1)

std430 SSBO does not transpose on upload. Shader uses `v * M` (not `M * v`). Already the convention in the dynamic pass. Static matrices follow the same rule.

---

## Section 6 — terrobj.cpp outer-skip changes

The existing outer-skip gate:
```cpp
if (!appearance->IsStaticNow())
    appearance->update(animate);
// else: nothing (Stage 3.B state — trees disappeared)
```

Changed to:
```cpp
if (!appearance->IsStaticNow())
    appearance->update(animate);
else
    appearance->touch();  // advance lastTurnTransformed; GPU static pass draws the color
```

That is the complete terrobj.cpp change.

---

## Section 7 — Registration and invalidation lifecycle (trees-only scope)

### First registration (frame N=0 for any tree)

`IsStaticNow()` → false (not yet registered) → `update()` runs → `TransformMultiShape_PositionsOnly` runs, `CacheGpuLightData()` runs → `submit()` called → batcher detects `!needsFullBakeNextFrame` + GPU eligible → calls `submitStatic()` → colors uploaded → `staticReg.registered = true`.

### Stable frames (frame N≥1, registered, no lighting change)

`IsStaticNow()` → true → `touch()` called → `lastTurnTransformed` advanced → `update()` body skipped → per-vertex color re-upload skipped → only matrix slot update touches GPU.

### Late-registration invalidation

Batcher's late-registration path already sets `needsFullBakeNextFrame = true`. Add: `appr->staticReg.registered = false`. Next frame: `IsStaticNow()` → false → full `update()` runs → new colors gathered → `submitStatic()` re-registers. Dead color words in SSBO are treated as wasted space (bounded by session-total late-reg events, which are rare).

### Falling tree

Owner-side `OBJECT_FLAG_FALLING` check in `terrobj.cpp` (already in the outer-skip gate from Stage 3.A) forces `IsStaticNow()` false for the duration of the fall. The appearance runs full `update()` each frame while falling. After the final pose is settled, the instance is eligible for re-registration on the next `update()` call.

Final-fall-pose verification: after the fall completes, one full `update()` runs (flag cleared), `TransformMultiShape_PositionsOnly` bakes the final orientation, `submitStatic()` re-registers with the correct final-pose colors. No manual deregister-then-re-register is required.

### Camera return after out-of-view

Persistent color data remains in the SSBO. When the camera returns, `inView` becomes true, `IsStaticNow()` → true (still registered), `touch()` runs, matrix slot updated — no color re-upload.

### Not in Stage 3.C scope

- Building damage / destruction transitions
- Destructible building shape swaps  
- Power-state / scripted effect changes on buildings
- Any `BldgAppearance` path

---

## Section 8 — Memory and performance

### Memory

- Persistent color SSBO: `INITIAL_COLORS_PER_FRAME × 4 bytes = 4 MB`
- Typical MC2 scene: ~15K tree instances × ~50 vertices = 750K vertices → ~3 MB used
- `s_staticInstancesByType` metadata: ~15K entries × ~80 bytes = ~1.2 MB CPU-side
- `StaticRegistration` per `TreeAppearance`: 24 bytes × instance count — negligible

### Performance target

Expected win: reduce Tracy `TerrainObject::update appearanceUpdate` contribution by ~1.2ms/frame on mc2_01. Measured before/after on that zone; do not claim more than the measured zone total. Tracy `GameLogic.Units.TerrainObjects` is the enclosing zone.

Validation ladder (matches water SSBO pattern):
- A. Visual canary: trees visible on load, survive camera pan, survive pan-away+return
- B. Tracy delta on `TerrainObject::update appearanceUpdate` before/after
- C. `MC2_STATIC_UPDATE_SKIP=1` + `MC2_STATIC_COLOR_SSBO=1` tier1 5/5 PASS, +0 destroys delta

---

## Section 9 — Debug instrumentation

Follow the CLAUDE.md debug instrumentation rule:

```cpp
static const bool s_staticColorTrace = (getenv("MC2_STATIC_COLOR_TRACE") != nullptr);
#define SC_TRACE(fmt, ...) \
    do { if (s_staticColorTrace) { printf("[STATIC_COLOR] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)
```

Log at: first registration of each shape (colorOffsetWords, vertexCount, typeKey), late-reg invalidation, SSBO realloc (if triggered). Never per-frame.

`[INSTR v1]` banner extension: add `static_color_ssbo` field alongside existing `static_update_trace` / `static_update_skip` fields in `gameosmain.cpp`.

---

## Files to create / modify

| File | Change |
|---|---|
| `mclib/tgl.h` | Declare `TG_Shape::Touch()`, `TG_MultiShape::Touch()` |
| `mclib/tgl.cpp` | Implement both Touch() methods |
| `mclib/bdactor.h` | Add `StaticRegistration` struct; add `staticReg` member to `TreeAppearance`; declare `touch()` override, re-declare `IsStaticNow()` override |
| `mclib/bdactor.cpp` | Implement `TreeAppearance::touch()`, `TreeAppearance::IsStaticNow()` (re-add); init `staticReg = {}` in `init()` |
| `mclib/appear.h` | Add `virtual void touch() {}` default no-op |
| `code/terrobj.cpp` | Change else-nothing to `else appearance->touch()` |
| `GameOS/gameos/gos_static_prop_batcher.h` | Add `StaticInstance`, `submitStatic()` declaration; remove `isStaticRegistered()` |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | Add `s_staticColorSsbo`, `s_staticInstancesByType`; implement `submitStatic()`; modify `flush()` for static pass; modify `init()` for SSBO allocation; add SC_TRACE instrumentation |
| `GameOS/gameos/gameosmain.cpp` | Extend `[INSTR v1]` banner with `static_color_ssbo` field |
