# Unified-Projection F1 Plan Addendum: R-clipw Polarity Investigation

**Date:** 2026-05-22 LATE
**Status:** INVESTIGATION COMPLETE. Candidate fix identified. User decision required before Task 7f.
**Trigger:** Task 7d transport-clean diagnostic. NDC products bit-identical CPU vs GPU; matrix produces
negative clip.w for in-front MC2 terrain. Hardware clip-volume test rejects negative-w verts pre-divide.
**Supersedes:** spec v2.8 §R-clipw risk row (partial). Decision matrix here takes precedence for Task 7f.
**Spec postscript:** `docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-atomic-design.md`

---

## 1. Finding

Task 7d proved transport bit-identical: `u_worldToClipGL * world` (new path) and
`terrainMVP * world -> screen-rhw -> mvp -> /w` (legacy round-trip) produce the same NDC.
`w_ratio = newW/oldW = 1.000` for sampled vertices.

However: `newClip.w = -1181.998` for an in-front vertex (world MC2.y = 5120 = forward into scene).
The legacy path writes `gl_Position = vec4(ndc.xyz * absW, absW)` using `abs(clip.w)`, forcing
`gl_Position.w > 0`. The new direct-emit path would write `gl_Position = newClip` verbatim, giving
`gl_Position.w = -1181.998`.

OpenGL hardware clip-volume test runs pre-perspective-divide using signed w. A vertex passes iff:
`-w <= x,y,z <= w`. With w = -1181.998: requires `1181.998 <= x,y,z <= -1181.998` -- impossible.
Every in-front vertex with negative clip.w would be discarded. Stage A direct emit is blocked.

---

## 2. Sample data (verbatim, Task 7d smoke, event=sample + event=ndc_compare)

```
world         = (-6144.083,  5120.010,  273.013,  1.000)
newClip       = (   509.182,  -923.042, -137.020, -1181.998)
legacyClip    = (   509.182,  -923.042, -137.020, -1181.998)
legacyGlPos   = ( -2200.362,  -664.085,  134.656,  1181.998)  <- abs(w) applied
oldNDC        = ( -0.430781,   0.780916,  0.115922)
newNDC        = ( -0.430781,   0.780917,  0.115922)
delta         ~= (0, 0, 0)
w_ratio (newW / oldW) = 1.000
```

MC2 world coord: x = -6144 (left), y = 5120 (forward, ground-plane), z = 273 (elevation).
axisSwap maps MC2 ground-forward (y) to GL forward (z), MC2 elevation (z) to GL up (y).
Both new and legacy clip chains produce the same signed clip.w = -1181.998.

---

## 3. Symbolic walk: why clip.w is negative for in-front geometry

### 3.1 Matrix build review

`Camera::worldToClipGL()` at `mclib/camera.cpp:2402-2411` (grep-verified):

```cpp
Stuff::Matrix4D viewClip;
viewClip.Multiply(worldToCameraMatrix, cameraToClip);  // dst = S1 * S2
Stuff::Matrix4D out;
out.Multiply(kAxisSwapMC2toGL, viewClip);
return out;
```

Stuff `Matrix4D::Multiply(S1, S2)` convention at `mclib/stuff/matrix.cpp:253-278`:
`dst(i,j) = Sum_k S1(i,k) * S2(k,j)` -- standard row-major matrix product.

Vector-matrix product at `mclib/stuff/vector4d.cpp:67-82`:
`result.w = v.x*m(0,3) + v.y*m(1,3) + v.z*m(2,3) + v.w*m(3,3)`
Row-vector convention: v multiplies from the left as a row.

GLSL `u_worldToClipGL * vec4(world, 1)` uses column-vector convention. The two conventions
agree when Stuff stores matrices row-major and GLSL uploads them with GL_FALSE (matching
the existing `terrainMVP` contract). Task 7d proves they agree empirically.

### 3.2 cameraToClip perspective build

Axis constants from `mclib/stuff/vector3d.hpp:53-58` (grep-verified):
`LEFT_AXIS = X_Axis = 0`, `UP_AXIS = Y_Axis = 1`, `FORWARD_AXIS = Z_Axis = 2`.

`APPLY_FORWARD_SIGN(z) = (z)`, `APPLY_LEFT_SIGN(x) = (x)`, `APPLY_UP_SIGN(y) = (y)`
(all identity, from `mclib/stuff/vector3d.hpp:68-93`, grep-verified).

`depth_range = APPLY_FORWARD_SIGN(1.0f) / (far_clip - near_clip) = 1/(far-near)`.

Perspective path at `mclib/camera.cpp:1930-1948` (grep-verified, perspective usePerspective branch):
```
cameraToClip(0, 0) = near * horizontal_range
cameraToClip(1, 1) = near * vertical_range
cameraToClip(2, 0) = -right_clip * horizontal_range
cameraToClip(2, 1) = -bottom_clip * vertical_range
cameraToClip(2, 2) = far * depth_range = far/(far-near)
cameraToClip(2, 3) = 1.0f                              <- w-source entry
cameraToClip(3, 0) = 0
cameraToClip(3, 1) = 0
cameraToClip(3, 2) = -far * near * depth_range = -far*near/(far-near)
cameraToClip(3, 3) = 0
```

### 3.3 w component derivation

For in-shader GLSL (column-vector): `clip = worldToClipGL * vec4(world, 1)`.
Equivalent to Stuff row-vector: `clip_row = world_row * worldToClipGL_transposed`.

Tracing clip.w through the matrix chain:

Step 1 -- worldToCamera transform: rotates + translates world coords to eye/camera space.
For the sample vertex at MC2.y = 5120 (well in front of camera), the eye-space z_eye is negative
(camera looks along -z in eye space, standard OpenGL convention). Sample: z_eye ~= -1182.

Step 2 -- cameraToClip applied (via viewClip = worldToCameraMatrix * cameraToClip):
Column-vector perspective w calculation uses row 3 of the column-vector matrix form.
In Stuff row-major storage with row-vector convention:
`clip.w = eye.x*cameraToClip(0,3) + eye.y*cameraToClip(1,3) + eye.z*cameraToClip(2,3) + eye.w*cameraToClip(3,3)`
= `0 + 0 + z_eye * 1.0f + 0`
= `z_eye`

With z_eye ~= -1182: clip.w = -1182. **This is the negative.**

Step 3 -- kAxisSwapMC2toGL at `mclib/camera.cpp:79-89` (grep-verified):
```
m(0,0) = -1.0f  m(1,1) = 0  m(1,2) = 1  m(2,2) = 0  m(2,1) = 1
all other diagonal = 1; off-diagonal = 0 except above; m(3,3) = 1
```
axisSwap does NOT touch column 3 / row 3: m(0,3)=m(1,3)=m(2,3)=m(3,3)=1 except m(3,3)=1.
Wait: axisSwap is applied as `out = kAxisSwapMC2toGL * viewClip` (S1=axisSwap, S2=viewClip).
For the w column of the result: `out(row, 3) = Sum_k axisSwap(row,k) * viewClip(k, 3)`.
axisSwap row 3 = [0, 0, 0, 1] (identity). So `out(3, 3) = viewClip(3, 3)`.
The effective w for a column-vector `out * vec4(world,1)` is computed via row 3 of `out`.
But clip.w from the GLSL perspective is the full dot product. The key invariant: axisSwap
has m(3,3) = 1 and m(3,0)=m(3,1)=m(3,2)=0, so it propagates clip.w unchanged.
axisSwap does NOT fix the sign of clip.w.

**Root cause: `cameraToClip(FORWARD_AXIS=2, col=3) = +1.0f` gives clip.w = +z_eye.
MC2 camera looks along -z_eye (in-front = z_eye < 0), so clip.w < 0 for in-front.**

Standard OpenGL perspective matrix has the perspective entry = -1.0 (column-vector form,
row 2 col 3 = -1, giving w = -z_eye > 0 for in-front z_eye < 0). MC2 uses +1.0, giving
w = +z_eye < 0 for in-front. The convention differs by one sign.

---

## 4. Candidate localized fix

**Fix location:** `Camera::worldToClipGL()` only (`mclib/camera.cpp:2402-2411`).
No change to `cameraToClip` build. No change to `worldToClip`. No change to GLSL.

After computing `out`, negate the entire matrix (scale by -1):

```cpp
Stuff::Matrix4D Camera::worldToClipGL() const
{
    // ... existing code ...
    Stuff::Matrix4D viewClip;
    viewClip.Multiply(worldToCameraMatrix, cameraToClip);
    Stuff::Matrix4D out;
    out.Multiply(kAxisSwapMC2toGL, viewClip);

    // Polarity fix: cameraToClip produces clip.w = z_eye < 0 for in-front
    // geometry (MC2 camera looks -z, standard OpenGL convention).
    // Negating the full matrix preserves NDC (xyz/w unchanged: both flip)
    // and makes w = -z_eye > 0 for in-front. Hardware clip-volume test
    // requires w > 0. See addendum-rclipw-polarity.md for derivation.
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out(r, c) = -out(r, c);

    return out;
}
```

### 4.1 Effect on GLSL usage

GLSL: `gl_Position = u_worldToClipGL * vec4(world, 1)` produces `-old_clip`.
- `gl_Position.w = -old_clip.w = -z_eye > 0` for in-front. Hardware clip-volume test passes.
- `gl_Position.xyz = -old_clip.xyz`. NDC = (-clip.xyz) / (-clip.w) = clip.xyz / clip.w = same NDC.
- Stage A direct emit `gl_Position = u_worldToClipGL * vec4(world, 1)` is safe. No abs(w) needed.

---

## 5. Per-axis impact evaluation

### A. Predicted clip.w sign for in-front (VERDICT: SAFE)

After fix: clip.w = -z_eye. For in-front z_eye ~= -1182, clip.w = +1182 > 0.
Hardware clip-volume test `-w <= x,y,z <= w` passes. Stage A direct emit enabled.

### B. Reverse-Z preservation (VERDICT: SAFE)

NDC.z = (-clip.z) / (-clip.w) = clip.z / clip.w = old_NDC.z. Unchanged.
`MC2_REVERSE_Z_TRACE` instrumentation reads `worldToClip` (not `worldToClipGL`) for
`inverseProjectZ` fence-seam. `worldToClip` is NOT modified by this fix.
Shadow maps, depth-prepass, GPU cull predicate all unchanged.

cameraToClip entries `(2,2)` and `(3,2)` that drive depth range are untouched.
Reverse-Z depth convention preserved.

### C. Legacy CPU wrappers preserved (VERDICT: SAFE)

`Camera::projectZ()` at `mclib/camera.h:459-540` uses `worldToClip` (grep-verified at line 478:
`xformCoords.Multiply(coords, worldToClip)`). `worldToClip` is the pre-axisSwap member built
at `mclib/camera.cpp:2144`: `worldToClip.Multiply(worldToCameraMatrix, cameraToClip)`.
This fix does NOT touch `worldToClip` or `cameraToClip`.

The 8 wrappers at `mclib/camera.h:550-696`
(`projectForTerrainAdmission`, `projectForObjectAdmission`, and 6 others)
all delegate to `projectZ()`, which uses `worldToClip`. All unaffected.

`projectZ` already handles sign via `screen.w = fabs(rhw)` at `mclib/camera.h:491`. Even if
this fix were somehow propagated (it is not), that fabs guard would absorb it. Belt + suspenders.

### D. Legacy terrainMVP consumers preserved (VERDICT: SAFE)

`terrainMVP` is uploaded from `code/gamecam.cpp:165-187` as `axisSwap * worldToClip` (the
AW matrix, NOT `worldToClipGL()`). F1 does not change the gamecam.cpp upload site until
Stage A. The legacy `terrainMVP * world -> screen-rhw -> mvp -> /w` round-trip path is
completely unaffected by any change inside `Camera::worldToClipGL()`.

`gos_GetTerrainMVPMat4()` reads the cached `terrain_mvp_` float array written from the
AW matrix path. This path remains unchanged through Stage A-pre and Stage A-pre gate period.

### E. MLR untouched (VERDICT: SAFE)

MLR reads `cameraToClip(2,2)` and `(3,2)` at:
- `code/gamecam.cpp:151` (grep-verified): `(1.0f-cameraToClip(2,2))/cameraToClip(3,2)`
- `mclib/mlr/mlrclipper.cpp:209` (grep-verified): `sorter->SetFarClipReciprocal(...)`
- `mclib/mlr/mlrclipper.cpp:305,321,347` (grep-verified): `GetPerspective`, `worldToClipMatrix.Multiply`

None of these read `worldToClipGL()`. `cameraToClip` is NOT modified. MLR is fully isolated.

---

## 6. Resolution order

### Primary path: Task 7f -- apply candidate fix

1. In `Camera::worldToClipGL()` body, add the per-element negation loop after the axisSwap multiply.
2. Re-run Task 7d smoke diagnostic. Expected: `w_ratio = -1.000` (newClip.w flipped vs legacy),
   `newNDC` still matches `oldNDC` (delta near zero), `count_behind_new_only` near zero.
3. If gate passes: proceed to Task 8 (Stage A-pre parity probe armed with corrected matrix).
4. Stage A canonical template becomes clean `gl_Position = u_worldToClipGL * vec4(world, 1)` --
   no abs(w), no round-trip kludge. Tasks 10-12 proceed as spec v2.8 §5.2 describes.

### Fallback A: preserve abs(w) in Stage A canonical template

If Task 7f candidate fix is rejected (breaks something not covered above) OR if polarity fix
is deferred: Stage A canonical template uses the 2-line pattern:
```glsl
vec4 raw = u_worldToClipGL * vec4(world, 1.0);
gl_Position = vec4(raw.xyz, abs(raw.w));  // preserve legacy polarity
```
This is the existing legacy pattern already proven working. No matrix change required.
Tasks 10-12 update the template comment to document the abs(w) intent explicitly.

### Fallback B: narrow F1 to upload centralization only

If polarity is proven non-localized and Fallback A also deemed unacceptable: scope F1 to
centralizing the `worldToClipGL()` upload path only (Tasks 1-9). Defer shader clean-emit
migration (Tasks 10-19) to a follow-on arc with dedicated matrix-convention rework. No
visible change to any shader; Stage A-pre becomes the final deliverable of F1.

---

## 7. Decision matrix

| Scenario | Recommended path |
|---|---|
| Task 7f passes: w_ratio=-1, NDC match, behind_new_only~0 | Primary: proceed to Task 8 with clean emit |
| Task 7f breaks reverse-Z in probe | Investigate which depth entry changed; may need additional correction |
| Task 7f: NDC mismatch (delta > 1e-3) | Root-cause before proceeding; may indicate additional sign coupling |
| Task 7f: behind_new_only > 1% of compared | Polarity fix insufficient; matrix model still wrong; escalate |
| Any Task 7f failure with user time pressure | Fallback A: abs(w) in template; ship Stage A |
| User prefers minimal-scope F1 | Fallback B: upload centralization only |

---

## 8. Promotion

This addendum supersedes spec v2.8 only for:
- Risk row R-clipw interpretation: "non-zero `count_behind_new_only` = matrix needs revision"
  is NOW confirmed true. Task 7f is the revision vehicle.
- Stage A canonical template in §5.2: clean emit `gl_Position = u_worldToClipGL * vec4(w,1)`
  is conditional on Task 7f passing. If Task 7f fails, Fallback A template applies.

All other spec v2.8 sections, including §4 (census), §6 (gates), §7 (reverification),
and §10 (F2/F3 triggers), remain authoritative and unchanged.

Plan v1.1 task list (Tasks 1-21) unchanged. Task 7f is a NEW pre-requisite inserted between
Task 7e (this investigation) and Task 8 (Stage A-pre parity probe armed run).
