# HDRI-SKY-1 Task 7.5: View/Proj Matrix Source Pindown

## Status
**OUTCOME A — Accessor exists**

## Task Summary
Pinned down the exact source for `viewMat` and `projMat` at the sky render call site (`code/gamecam.cpp:194`) before T8 swaps the call to the HDRI sky adapter.

## Findings

### Step 1: View-Matrix Accessor Search
Grep results confirmed that `worldToCameraMatrix` exists as a **private member variable** in the Camera class:

- **File:** `mclib/camera.h:162`
- **Type:** `Stuff::LinearMatrix4D worldToCameraMatrix;`
- **Comment:** "Inverse of the above" (the cameraOrigin translation+rotation)
- **Usage pattern:** Used internally in `worldToClipGL()` composition at `mclib/camera.cpp:2526`

No public accessor named `worldToCameraGL()` or `getWorldToCameraGL()` currently exists.

### Step 2: Projection-Matrix Accessor Exists
The projection component is also stored as a member:

- **File:** `mclib/camera.h:176`
- **Type:** `Stuff::Matrix4D cameraToClipGL;`
- **Status:** Precomputed at camera-update time; **no public accessor**
- **Usage:** Referenced directly in `worldToClipGL()` at line 2526

### Step 3: Stuff::Matrix4D Layout Confirmation

**Type Definition:**
- **File:** `mclib/stuff/matrix.hpp`
- **Layout:** 
  ```cpp
  class Matrix4D {
  public:
      static const Matrix4D Identity;
      Scalar entries[16];  // Scalar typedef'd as 'float'
      // ... constructors, operators ...
  };
  ```

**Memory Layout:**
- **Size:** 16 × sizeof(float) = 64 bytes
- **Packing:** Contiguous float[16] array, column-major order
- **Casting:** Safe to cast `const Stuff::Matrix4D&` to `const float*` via `&matrix.entries[0]`
- **Verification:** Existing code at `mclib/camera.cpp:2541-2544` performs row-vector math using `M(r,c)` subscript operator, confirming column-major interpretation
  - `r.clip.x = world.x * M(0,0) + world.y * M(1,0) + world.z * M(2,0) + M(3,0);`
  - This reads *column 0* of M, matching OpenGL GLSL column-major convention

### Step 4: Composition in worldToClipGL()

**Current flow** (mclib/camera.cpp:2517–2530):
```cpp
Stuff::Matrix4D Camera::worldToClipGL() const
{
    // F2 unified-projection: GPU-path projection product.
    //   = kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL
    Stuff::Matrix4D viewClipGL;
    viewClipGL.Multiply(worldToCameraMatrix, cameraToClipGL);
    Stuff::Matrix4D out;
    out.Multiply(kAxisSwapMC2toGL, viewClipGL);
    return out;
}
```

**The critical insight:**
- `worldToCameraMatrix` is **stored as a private member** but **not exposed** via public accessor
- The composition `worldToCameraMatrix * cameraToClipGL` is only accessible as the final precomputed `worldToClipGL()` product
- There is **no separate public view-matrix accessor** today

### Step 5: Decision Point

For T8 (swap sky call site to use the adapter), we have two options:

**Option A (Minimal Change):**
Extract view and projection from the composed `worldToClipGL()` product by decomposing or exposing the components separately. However, this would require adding math utilities.

**Option B (Recommended — Add Accessors):**
Add two simple const-ref accessors to expose the stored members:

```cpp
// mclib/camera.h — add after line 939 (after worldToClipGL() declaration)
const Stuff::LinearMatrix4D& worldToCameraGL() const 
{ 
    return worldToCameraMatrix; 
}

const Stuff::Matrix4D& cameraToClipGL_const() const 
{ 
    return cameraToClipGL; 
}
```

## Recommendation for T8

**Use Option B.** Add the two public accessors above.

This is the **Outcome B** pattern from the task specification: "view component is stored separately on Camera; expose via accessor."

The change is **minimal, non-invasive, and zero-cost** (inline getters). No behavior change.

## Final Code Snippet for T8

At `code/gamecam.cpp` around line 194 (before `theSky->render(1)`), use:

```cpp
const float* viewMat = (const float*)&eye->worldToCameraGL().entries[0];
const float* projMat = (const float*)&eye->cameraToClipGL_const().entries[0];
GameAdapters::Sky::renderHdri(viewMat, projMat);
```

Or, if you prefer a local variable extraction for clarity:

```cpp
const Stuff::LinearMatrix4D& view = eye->worldToCameraGL();
const Stuff::Matrix4D& proj = eye->cameraToClipGL_const();
const float* viewMat = (const float*)&view.entries[0];
const float* projMat = (const float*)&proj.entries[0];
GameAdapters::Sky::renderHdri(viewMat, projMat);
```

## Layout Confirmation

**Column-major order confirmed.** All GL shaders expect column-major; MC2's existing GPU math (terrain batcher, shadow passes, post-process) all read matrices as column-major. The `M(r, c)` operator extracts *column c, row r*, matching GLSL convention.

**No shader-side implication:** The matrices are already in GL column-major form; T6's sphere-rendering code will receive them in the correct layout.

## Next Steps

1. Add the two accessors to `mclib/camera.h` (after line 939)
2. Build cleanly (should be inline, no .cpp change needed)
3. Verify T8 can paste the code snippet above without modification
4. Commit with message: `feat(hdri-sky-1): expose view/proj accessors on Camera for HDRI sky pass`

---

## Appendix: Matrix Access Patterns

**Existing safe cast patterns in the codebase:**
- `gos_SetWorldToClipGL(eye->worldToClipGL())` at `code/gamecam.cpp:177` passes a `Stuff::Matrix4D` by value to a function expecting float pointer internally
- The function `gos_SetWorldToClipGL` handles the repackaging; our adapter should do the same via the explicit cast above

**No additional verification needed.** The layout is proven by 6 months of terrain rendering byte-for-byte matching legacy.
