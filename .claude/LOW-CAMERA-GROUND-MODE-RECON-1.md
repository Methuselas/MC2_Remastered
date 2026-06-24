# LOW-CAMERA-GROUND-MODE-RECON-1

Worktree: `A:/Games/mc2-low-camera` @ `claude/low-camera-ground-mode-1` (off nifty tip `6fbf5604`).
Read-only recon complete. Math-proven plan below. No gameplay change yet.

## Camera identity model (canonical = `Camera`, mclib/camera.cpp/.h)

| Concept | Source | Note |
|---|---|---|
| eye / position | `physicalPos = translation` camera.cpp:2751; `cameraOriginGL()` :2806 | true eye, MC2 world |
| pivot / orbit center | `cameraShift` (x,y) camera.h:166 + `cameraShiftZ` | set each frame to camera XY :2015 |
| look dir | `cameraDirection` (range/pitch/yaw) spherical → translation :2688 | |
| pitch / yaw | `projectionAngle` h:145, `cameraRotation` h:156 | |
| render VP matrix | `worldToClipGL()` :2768 = `kAxisSwapMC2toGL · worldToCameraMatrix · cameraToClipGL` | reverse-Z, ZERO_TO_ONE, near→NDCz=1 far→z=0 |
| `getPosition()` | returns GROUND-FOCUS point, **NOT eye** | documented trap (camera.cpp:961) |

## THE keystone bug (root cause of pick + zoom-anchor failures)

`Stuff::Matrix4D::Invert` (mclib/stuff/matrix.cpp:860) is a correct **full 4×4 cofactor inverse**, BUT the singularity guard at line 921:

```cpp
if (det < Stuff::SMALL)   // Stuff::SMALL = 1e-4f  (style.cpp:21)
    oneOverDet = 1e30f;   // -> returns GARBAGE (all entries ×1e30)
```

Two stacked defects on a **projective** matrix:
1. **Sign**: a reverse-Z + x-flip (`x'=-x`) clip matrix has a **negative** determinant. `det < 1e-4` is TRUE for any negative det → flagged singular.
2. **Magnitude**: a perspective clip matrix maps huge world extents into [-1,1]³, so even a valid `|det|` is far below `1e-4` → flagged singular regardless of sign.

Result: `worldToClipGL().Invert()` yields garbage. camera.h:1027 admits "Matrix4D::Invert(worldToClipGL()) is UNRELIABLE in the game." Every inverse-VP unproject inherits this.

**Downstream casualties (all currently broken/worked-around because of this):**
- `inverseProject` raycast picker (camera.cpp:977) — disabled; "X response collapses, far point above camera" = classic garbage-inverse signature.
- `screenToGroundPlaneApprox` (:831) and `screenToTerrainApprox` (:879) — the correct inverse-VP-ray + heightfield-march helpers — call `Invert(M)` → also corrupted. Used per-frame for cursor preview (missiongui.cpp:1118, controlgui.cpp:506).
- Active move-order pick falls back to legacy forward-projection quad-scan (`inverseProject` Stage 1/2, :1142) — works at high angle, ambiguous at grazing angle (screen-distance tiebreak can't disambiguate depth along a near-horizontal ray, and picks nearest-center quad even if occluded by a closer hill).

### FIX-0 (keystone): `lowCamInvert4x4` — REVISED to ZERO global change
Original plan was to fix the global `Matrix4D::Invert` guard (`det < SMALL` → `fabsf`).
**REVISED (user note: top-down picking finally works via the default forward-projection
path; this arc has been fraught — do NOT risk the working baseline):**
- Global `Matrix4D::Invert` is LEFT BYTE-IDENTICAL (reverted). Nothing engine-wide changes.
- Added a file-local guard-free Gauss-Jordan inverse `lowCamInvert4x4` (camera.cpp, before
  `screenToGroundPlaneApprox`) used ONLY by the inverse-VP unproject sites:
  `screenToGroundPlaneApprox`, `screenToTerrainApprox`, and FIX-1's primary ray.
- These three helpers were ALREADY returning garbage (they called the broken global Invert),
  so routing them through the correct local inverse is strictly an improvement with no
  regression to the working forward-projection move-order path.
- Proof harness retained: `MC2_INVERSE_DET_PROOF=1` (one-shot) compares the shipped (still
  buggy) `Stuff::Invert` vs a guard-free Gauss inverse on the real `worldToClipGL()` —
  prints `resid_stuffInvert` (huge) vs `resid_gauss` (~0), documenting WHY we route around it.

## Per-symptom plan (each proven before user tests)

### FIX-1 `LOW-CAMERA-PICK-RAY-1` (move orders at grazing angle)
With FIX-0, the inverse ray is valid. Route the **move-order** pick through a true ray:
- build world ray from `worldToClipGL()^-1` unproject of NDC(x,y,z=1 near)→(z=0 far) → MC2 (glToMC2);
- march the heightfield for FIRST hit (`Terrain::raycastTerrain`, terrain.h:327) → correct occlusion + grazing robustness;
- fallback: `screenToTerrainApprox` fixed-point (3-iter) if raycast misses; final fallback z=0 plane.
- Origin MUST be eye (`physicalPos`), never `getPosition()`.
Acceptance: order lands under cursor at low pitch; high-angle unchanged; horizon soft-fails.

### FIX-2 `LOW-CAMERA-TERRAIN-CULL-1` (near terrain dropped)
`NearPlaneDistance = -400` (camera.cpp:161) baked into proj → frustum near plane (camera.cpp:691) used by terrain block cull (terrain.cpp:1755 `quadAabbInFrustum`). At low pitch, terrain within ~400u of eye is clipped. Plan: shrink near plane for the cull frustum and/or widen the near apron at low pitch (existing `MC2_TERRAIN_LOD_CHUNK_NO_APRON` knob @ terrain.cpp:1781). Gate `MC2_LOWCAM_TERRAIN_NEAR`.
Acceptance: no near-terrain drop low pitch; chunk path still governed; high-angle unchanged.

### FIX-3 `LOW-CAMERA-OBJECT-CULL-1` (mechs/buildings drop when near)
`object_admission_predicate.cpp:330` `if (cz < 0.0f || cz > cw) return false;` rejects an object whose **center** crosses the near plane, ignoring bounds radius. Plan: pad near/far admit by the object's clip-space radius (sphere-vs-near-plane, not point) — accept if `cz + rClip >= 0`. Origin already eye. Gate `MC2_LOWCAM_OBJ_NEARPAD`.
Acceptance: near objects stay visible while in front; no overdraw blow-up; pick/editor untouched.

### FIX-4 `MOUSE-ANCHORED-ZOOM-1` (zoom around cursor)
Zoom changes altitude around fixed `cameraShift` (=camera XY, camera.cpp:2015; ZoomIn :3168). Plan (depends on FIX-0/1 ray): preZoom raycast cursor→world A; apply zoom; postZoom raycast→B; `cameraShift += (A−B)` (XY). Fallback: last valid cursor hit, else current pivot. Gate `MC2_LOWCAM_ZOOM_ANCHOR`.
Acceptance: world point under cursor stays put across wheel; works low+normal pitch; no jump at horizon.

## Order
FIX-0 (keystone, empirical det gate) → FIX-1 → FIX-2 → FIX-3 → FIX-4. All default-OFF gates except FIX-0 (correctness, but behind det-proof). User tests all at end. Deploy folder: `A:/Games/mc2-opengl/mc2-win64-v0.4`.
</content>
