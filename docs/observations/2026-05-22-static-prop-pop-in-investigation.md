# Static-Prop Pop-in Regression — Root-Cause Investigation

**Date:** 2026-05-22
**Branch:** claude/nifty-mendeleev
**HEAD at investigation:** b30a994
**Status:** DONE

---

## 1. Summary

The static-prop pop-in regression is **not a matrix bug and not a missing
extent test on the CPU side**. It is a well-understood semantics mismatch: the
legacy `clipSpaceFrustumAdmit` predicate (operating on D3D-pixel-homogeneous
raw clip) and the new `clipSpaceFrustumAdmitGL` predicate (operating on
GL-NDC clip from `projectModernClipGL`) test different geometric quantities
for objects whose clip.w < 0. Legacy produces systematic false-positive admits
at screen edges by testing `rawClip.y*sign(w)` in `[-|w|, +|w|]` rather than
the true NDC.y. The GPU-cull path (which feeds the primary static-prop batcher)
uses `clipSpaceFrustumAdmitSphere` with `boundingRadius=200.0f` for all static
props and is unaffected by the predicate change. Pop-in therefore originates in
the GPU cull path receiving the correct-but-stricter GL matrix, not in any CPU
admission wrapper. The hardcoded `boundingRadius=200.0f` constant is the likely
pressure point: props with a footprint radius materially larger than 200 clip
units can be culled when their centroid exits the frustum by more than that
tolerance.

---

## 2. Symptom 1 — Static-prop pop-in (user-observed)

mc2_10 intro: ~2-3 of 200 props popped in pre-F1; ~5-6 post-F1+F2+F3. The
regression appeared after F1 Stage A (commit `59fae27`) switched terrain
shaders from the legacy viewport-scale chain to direct `gl_Position =
u_worldToClipGL * vec4(world,1)`.

---

## 3. Symptom 2 — F4 T1 Compare-mode disagreements

64+ disagreements per wrapper (`lighting_shadow`, `effect`) logged by
`logProjectZBypassDisagreement` (`mclib/object_admission_predicate.cpp:209`).
In every sampled case: Legacy admits, GL bypass rejects. The sample point is a
screen-edge or above-screen-top world position.

Sample disagreement (lighting_shadow):
- world = (-2709, 3136, 477)
- legacyRawClip = (-270, 586, -142, -3209) — legacy ADMITS
- bypassClipGL  = (-2669, 4382, 142, +3209) — GL REJECTS

---

## 4. Question 1 — static_prop_batcher cull approach

**CPU admission gate:** `BldgAppearance::recalcBounds` at
`mclib/bdactor.cpp:1181-1258` runs a **coarse angular sphere test** using
`bldgShape->GetExtentRadius()` and the per-camera clip constants
(`vClipConstant`, `hClipConstant`). The per-frame screen-projection body was
deleted 2026-05-18 (Task 2 per comment at `bdactor.cpp:1241-1244`).
`inView` is now coarse-angular-only. Props that pass angular test call
`GpuStaticPropRegistry::markVisible` (`bdactor.cpp:1403`).

**GPU admission gate (primary):** `gos_static_prop_registry.cpp:490-555`
submits a `GpuActorRecord` per prop to `gpu_cull::substrate`. The GPU cull
shader (`shaders/gpu_cull.comp:220-225`) runs
`clipSpaceFrustumAdmitSphere(clip, rec.boundingRadius)` when
`rec.boundingRadius > 0`. Every static prop record is written with
`boundingRadius = 200.0f` (hardcoded at `gos_static_prop_registry.cpp:539`)
and a matching cubic AABB (`gos_static_prop_registry.cpp:540-545`).

The matrix fed to the GPU cull's `viewProj` UBO is `gos_GetTerrainMVPMat4()`
(`gpu_cull_compute.cpp:831,839`), which returns the matrix written by
`gos_SetWorldToClipGL(eye->worldToClipGL())` at `code/gamecam.cpp:174` — the
correct GL-NDC matrix post-F1.

**Conclusion:** Static-prop cull is center-point + sphere-radius on the GPU,
using the GL-NDC matrix. It does NOT use any CPU `clipSpaceFrustumAdmit`
predicate. CPU wrappers (`projectForObjectAdmission`,
`projectForEffectAdmission`, `projectForLightingShadow`) are for non-static
objects and effect/light activation, not static-prop batcher admission.

---

## 5. Question 2 — `clipSpaceFrustumAdmit` verbatim + range semantics

Source: `mclib/object_admission_predicate.cpp:233-260`.

```cpp
bool clipSpaceFrustumAdmit(const Stuff::Vector4D& rawClip) {
    const float s  = (rawClip.w < 0.0f) ? -1.0f : 1.0f;
    const float cx = rawClip.x * s;
    const float cy = rawClip.y * s;
    const float cz = rawClip.z * s;
    const float cw = rawClip.w * s;  // always >= 0 after this
    if (cw < 1e-5f) return false;
    if (cx < -cw || cx > cw) return false;
    if (cy < -cw || cy > cw) return false;
    if (cz < 0.0f || cz > cw) return false;
    return true;
}
```

Range semantics:
- x, y: standard GL `[-w, +w]` test after sign-flip.
- z: D3D `[0, w]` range (matches `glClipControl(GL_ZERO_TO_ONE)`).
- No explicit radius tolerance. Center-point only.

The D3D comment in the shader (`gpu_cull_predicate.glsl:6`) refers only to the
z-range; x and y are already GL-style. The function does NOT test the D3D
pixel-homogeneous y range `[0, w]` on the y axis.

---

## 6. Question 3 — Was legacy permissive, and by what mechanism?

**Yes, legacy is systematically permissive for in-front objects (w < 0).**

MC2's `cameraToClip` matrix (built at `mclib/camera.cpp:1943` region) emits
clip.w < 0 for in-front vertices (the FORWARD_AXIS=2, col=3 entry is +1.0f
under a -z_eye convention; see `camera.cpp:101-102`). For such points,
`clipSpaceFrustumAdmit` flips all components by s=-1, giving:

```
cy_test = rawClip.y * (-1) = -(rawClip.y)
cw_test = rawClip.w * (-1) = |rawClip.w|
```

It then tests `-(rawClip.y) ∈ [-|w|, +|w|]`, i.e., `rawClip.y / |w| ∈ [-1, +1]`.

The true GL-NDC y for the same point, after `kPixelHomogToGLNDC` M_fix
(`camera.cpp:148-153`), is:

```
ndc_y = (2*rawClip.y - rawClip.w) / (-rawClip.w)
      = -2*(rawClip.y / rawClip.w) + 1
```

With `rawClip.w = -3209` and `rawClip.y = 586`:

```
legacy  test value = -(586) / 3209 = -0.183  → inside [-1,+1] → ADMITS
true GL ndc_y      = -2*(586/-3209)+1 = +0.365+1 = +1.366 → outside [−1,+1] → REJECTS
```

The point IS above the screen top in GL-NDC. Legacy's false admit is an
artifact of testing `rawClip.y * sign(w)` rather than the correct
`-2*(rawClip.y/rawClip.w)+1` NDC formula. For D3D-pixel-homog clip, the
quantity `rawClip.y / |w|` lives in `[0, 1]` for in-screen (D3D y-down), so
values near 0 pass the legacy `[-1,+1]` test even when GL NDC says above-top.

The discrepancy is bounded: it is only significant near the top of the screen,
where the D3D y range [0.0 .. ~0.5] maps to GL NDC y [1.0 .. ~0.0]. Any point
with D3D-pixel-homog `rawClip.y/|w| < ~0.5` will have GL NDC y > 0, but those
between 0.5 and 1.0 (near screen top in D3D) map to GL NDC y between 0 and 1
(still on screen). The ghost admissions come from D3D-pixel-homog y BELOW 0.0
(above the D3D screen top, which is y=0), which legacy passes because
`rawClip.y * s` is negative (inside [-cw, +cw]).

---

## 7. Question 4 — Prop radius/extent availability

**CPU side:** `bldgShape->GetExtentRadius()` is available at
`mclib/bdactor.cpp:1222` for the angular test. Units: world units. The actual
shape bounding sphere radius for buildings.

**GPU side:** `GpuActorRecord.boundingRadius` at `GameOS/gameos/gpu_cull_record.h:11`
is written as **a hardcoded constant `200.0f`** for all static props (registry:
`gos_static_prop_registry.cpp:539`). This is an engineering placeholder per
the comment at `gos_static_prop_registry.cpp:532-535`: "50.0f was too small:
strict point-in-frustum test even when most of the building was on screen."
200.0f was empirically increased to reduce false cull. The actual prop footprint
radius is NOT read from the shape; it is the same 200 for a small fence post
as for the LookoutTower.

---

## 8. Question 5 — Light/effect radius availability

**Lights:** `TG_Light.farDistance` (`mclib/tgl.h:211`) — world units; represents
the distance at which the light is fully attenuated. Also `closeDistance`
(inner falloff radius). For admission, `camera.cpp:2027` uses a simple center
point test (`projectForLightingShadow`) with no radius expand — per the
comment at `camera.cpp:2020`: "This is complicated because these lights have a
radius! For now, simple position check. Make complicated later!"

**Effects/billboards:** `gosFX::EffectCloud` writes `particle->m_radius` at
`mclib/gosfx/effectcloud.cpp:267` as `center.GetLength() + bounds.sphereRadius`.
However, `projectForEffectAdmission` at `mclib/camera.h:635` admits on center
point only — no extent expand.

---

## 9. Hypothesis verdict

**Two separate issues; the primary regression is center-only GPU cull with
hardcoded conservative radius:**

**A. Static-prop pop-in regression (Symptom 1):** The GPU cull path uses the
correct GL-NDC matrix from `gos_GetTerrainMVPMat4()` (post-F1). The predicate
`clipSpaceFrustumAdmitSphere(clip, 200.0f)` is applied. With 200.0f, buildings
whose centroid exits the frustum by > 200 clip units are culled. Pre-F1, the
D3D-pixel-homog matrix fed to CullUBO had a systematically looser effective
frustum (the same `clipSpaceFrustumAdmitSphere` operating on D3D-pixel-homog
coords produces a subtly different admit envelope than on correct GL-NDC).
The regression is NOT about missing extent data per se — 200.0f is generous —
but about the **clip-space extent of 200.0 clip-units representing a smaller
world-space cone** under the correct GL-NDC matrix vs the old D3D-homog matrix.
This is a radius-calibration issue, not a code-path bug.

**B. Effect/lighting-shadow disagreements (Symptom 2):** These are center-point
admits using `clipSpaceFrustumAdmit(rawClip)` vs `clipSpaceFrustumAdmitGL(clipGL)`.
The legacy predicate is permissive for in-front objects due to the sign-flip
artifact described in Section 6. The `projectModernClipGL` path is correct; it
is the legacy path that was wrong. The disagreements are legacy false-admits
being correctly rejected by GL bypass — not a GL regression.

**Matrix bug verdict: NONE.** `kPixelHomogToGLNDC` at `mclib/camera.cpp:148-156`
is correct. The M_fix row-1 = +2 is mathematically correct for the intended
conversion (verified by checking `bypassClipGL` = M_fix * `legacyRawClip`
for the sample point).

---

## 10. Recommendation

### For Symptom 1 (static-prop pop-in)

**Root cause:** Hardcoded `boundingRadius=200.0f` in `gos_static_prop_registry.cpp:539`
is not calibrated to world-prop extent. Pre-F1 the D3D-pixel-homog matrix gave
a fortuitously looser sphere test; GL-NDC is tighter.

**Fix options (two-pronged):**

**(a) Short-term — increase or per-prop-calibrate the tolerance:**
Read `bldgShape->GetExtentRadius()` (already available at CPU side,
`bdactor.cpp:1222`) and write it into `GpuActorRecord.boundingRadius` instead
of 200.0f. This requires threading the per-actor radius through to
`gos_static_prop_registry.cpp:539`. A safe interim is to raise the hardcoded
constant to 400.0f as a regression gate; prop geometry is typically 50-300 units
radius, so 400 would admit all edge cases. Change is one line.

**(b) Longer-term (preferred, meta-fix class):**
Pass the actual `GetExtentRadius()` value at `GpuStaticPropRegistry::markVisible`
callsites (`bdactor.cpp:1403`, `bdactor.cpp:4226`) and store it in
`RecipeRange`. Flush uses the per-actor radius as `boundingRadius` in
`GpuActorRecord`. This is the substitutive fix that retires the constant-radius
class of pop-in permanently.

**Scope:**
- `mclib/bdactor.cpp` — two `markVisible` callsites
- `GameOS/gameos/gos_static_prop_registry.cpp` — `markVisible()` signature + `RecipeRange` layout + flush emit
- `GameOS/gameos/gos_static_prop_registry.h` — `markVisible()` declaration
- Single-slice landing; no shader changes needed.

### For Symptom 2 (effect/lighting-shadow disagreements)

The legacy admits are false positives. If `MC2_PROJECTZ_BYPASS_MODE=Bypass`
is not yet default, there is no visible user regression — the legacy path is
correctly admitting in-screen objects and only misses the above-screen-edge
ones that legacy falsely admits. When the bypass is made default (F4), those
false-admit objects (screen-edge effects/lights) will correctly be rejected.
No fix needed unless the user explicitly wants to backfill effect-radius
tolerance for screen-edge billboards, which would require passing
`gosFX::EffectCloud.m_radius` through the admission site.

---

## 11. Scope of fix

**Primary (static-prop pop-in):**
- One-slice landing.
- Files: `mclib/bdactor.cpp`, `GameOS/gameos/gos_static_prop_registry.{cpp,h}`
- No shader changes.
- Option (a) is one line; option (b) is ~20 lines across 3 files + signature change.

**Secondary (effect/light admission):**
- Not regressions; deferred to F4 bypass default rollout.
- Optional hardening: extend `projectForEffectAdmission` /
  `projectForLightingShadow` with radius parameter when `farDistance` or
  `m_radius` is available at callsite. Low priority.
