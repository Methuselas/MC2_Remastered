# Track A — Predicate Replacement: Recon Zero

**Date:** 2026-05-06
**Status:** read-only recon, no code changes
**Roadmap track:** [Track A](../mc3-rendering-modernization-roadmap.md#track-a--predicate-replacement-fixes-wolfman-no-cascade--start-here)
**Branch:** `terrain-pbr-mod` (worktree `nifty-mendeleev`)

---

## 1. TL;DR

- **The Track A hypothesis as written has a serious tension with already-captured data.** The 2026-04-26 capture report (`projectz-capture-report.md` § 3) explicitly verdicts "**terrain `BoolAdmission` must remain on the legacy screen-rect contract**" because every modern predicate (`homogClip`, `rectSignedW`, `rectNearFar`, `rectGuard`) over-culls or over-admits in measured-and-converged ways at exactly the camera configurations Track A targets. A frustum+distance predicate is a fifth candidate — but the report's per-callsite data (mc2_03 baseline rectSignedW dis% = 50.91%; cross-mission stress terrain_cpu_vert_admit ~36% restrictive) is the empirical floor any new candidate has to clear before it can be a drop-in, and that data was collected over the same `mclib/projectz_trace.cpp` infrastructure Track A's exit gate proposes to reuse.
- **The "RAlt+P overlay GL state bug" prereq is stale.** `dec89aa "projectz: fix overlay draw state"` already landed: `mclib/projectz_overlay.cpp:226` rebinds the GameOS VAO before the overlay's draws and `gos_DrawTriangles`. The roadmap text and the policy-split report both still cite it as a Track A prereq; this recon recommends striking it.
- **The wedge-class wrappers' interface is a drop-in surface in principle:** `bool projectFor*(Vector3D&, Vector4D&)` in `mclib/camera.h:525-570`. **In practice** at least the 4 terrain quad sites + 1 terrain.cpp loop site write screen.x/y/z/w into per-vertex storage (`mclib/quad.cpp:804,871,938,1005`; `mclib/terrain.cpp:1438,1597`) and the GPU terrain pipeline consumes those values. The bool gates submission, but the screen quartet must remain valid for legacy-path consumers. So a frustum+distance predicate **must continue to populate screen.{x,y,z,w}** with byte-identical-to-legacy values regardless of how the bool is computed. This is doable but is not strictly "predicate replacement" — it is "predicate gate replacement, screen oracle preserved."
- **Track A as scoped is plausibly ~1 week IF** the slice is reframed as "swap the bool computation, keep the screen oracle, parity-gate via existing trace infra." If the slice tries to discard or alter the screen oracle alongside, it cascades into the territory that already produced "Outcome C" rejection.

## 2. Current predicate body — verified

The wrapper(s) live in `mclib/camera.h:517-606`. All three wedge-class wrappers
(`projectForTerrainAdmission`, `projectForObjectAdmission`,
`projectForEffectAdmission`) at `mclib/camera.h:525-570` are inert
forwarders to `projectZ()` plus an `MC2_PROJECTZ_FINITE_CHECK`-gated
`gosASSERT` on the four screen components.

Underlying `projectZ()` body at `mclib/camera.h:435-515`:

1. **Axis swap** — `coords.x = -point.x; coords.y = point.z; coords.z = point.y;`
   (MC2 world → Stuff camera-space). `mclib/camera.h:442-444`.
2. **Single composite matrix multiply** — `xformCoords.Multiply(coords, worldToClip);`
   `mclib/camera.h:446`.
3. **Perspective branch** (`usePerspective`):
   - `rhw = (xformCoords.w != 0.0f) ? 1/xformCoords.w : 1.0f;` — note this
     does **not** fix the sign; if w<0, rhw<0.
   - `screen.x = xformCoords.x*rhw*viewMulX + viewAddX;`
     `screen.y = xformCoords.y*rhw*viewMulY + viewAddY;`
     `screen.z = xformCoords.z*rhw;`
   - **`screen.w = fabs(rhw);`** — the smoking-gun line at
     `mclib/camera.h:459`. The W sign is destroyed before any caller can
     read it.
4. **Parallel branch** (`!usePerspective`) at `mclib/camera.h:463-469`:
   different math path (orthographic), `screen.w = 0.000001f;`.
5. **Pixel-rectangle admission test** at `mclib/camera.h:471`:
   `if ((screen.x < 0) || (screen.y < 0) || (screen.x > screenResolution.x) || (screen.y > screenResolution.y)) return FALSE;`
6. **Otherwise** `return TRUE;`

So "rect-screen-finite" is a misnomer for the legacy predicate. There is
no finite-check in the legacy bool — the finite-check is only in the
**wrapper-side gosASSERT** under `MC2_PROJECTZ_FINITE_CHECK`. The legacy
admission is a **post-divide pixel-rectangle test** with the W sign
destroyed by `fabs(rhw)`. That is the predicate Track A is actually
proposing to replace.

## 3. Why it fails at wolfman zoom — concrete math

**Note on terminology:** "wolfman zoom" is just maximum zoom-out; not a
separate code path (`memory/wolfman_is_max_zoom.md`). The bug shape is
documented in `docs/gpu-static-prop-cull-lessons.md:8-18`: at extended
camera distance, "most buildings and half the mechs don't render"
because the `inView` angular cull (`mclib/terrain.cpp:1029-1054`,
`mclib/bdactor.cpp:1090-1167`) reports a "~87% false-negative rate."

Important distinction: the 87% number comes from **`inView`**, which is
the angular-cull pre-filter feeding `projectZ`, not from the `projectZ`
admission bool itself. Following the chain at
`mclib/terrain.cpp:1429-1438` and `:1590-1597`:

```
if (onScreen)                                        // angular pre-cull
    inView = eye->projectForTerrainAdmission(...)    // post-divide rect
currentVertex->clipInfo = inView                     // gates submission
```

The 87% number is on the angular `onScreen` pre-cull; the `projectZ`
rect test runs only on already-admitted vertices, but its rejections
also feed `clipInfo`. Both layers cull more than they should at far
distance.

**Where the rect test goes wrong at far zoom:**
- `screenResolution` is fixed (e.g. 1920×1080). At extreme zoom-out the
  visible terrain spans many world units that all map to a small
  fraction of the screen rect. Vertices at the periphery (still
  in-frustum) project to screen.x/y values that fall just outside
  `[0, 1920]` × `[0, 1080]` because the math has no margin: pixel-
  rectangle clipping is binary.
- **rectGuard with GUARD_PX=64 over the same captures admitted 4.49–4.57%
  more vertices** (`projectz-capture-report.md` § 4) — the strict-superset
  property says these are vertices the angular cull let through but the
  pixel-rect rejected. 74% of those extras concentrate on terrain
  admission sites — exactly the wedge-risk vector. Whether they are
  "harmless near-edge recovery" or "wedge-producers" is precisely what
  the still-open visual-overlay disambiguation was supposed to answer.
- The W-sign destruction at `mclib/camera.h:459` is a separate hazard:
  behind-camera vertices whose post-divide xy lands inside the rect
  return TRUE with garbage screen.z/w. This is the documented "wedge"
  failure mode and is independent of distance — it bites at any zoom
  where the camera grazes an obstacle.

**A frustum-plane + distance test would fix both layers simultaneously:**
near/far/left/right/top/bottom plane tests give a true frustum gate
(no W-sign bug, no off-by-pixel near rect edges); a distance test caps
at the engine's view distance. The hard part is **screen.x/y/z/w must
still be populated with the legacy values** for callers that use them
post-admission.

## 4. Replacement candidate — prose sketch

### 4.1 Frustum representation

The Camera class today exposes:
- `worldToClip` — `Stuff::Matrix4D`, `mclib/camera.h:137`. The composite
  world→clip matrix used inside `projectZ`.
- `cameraOrigin` — `Stuff::LinearMatrix4D`, `mclib/camera.h:129`.
- `cameraAltitude` — float, `mclib/camera.h:274`.
- `camera_fov`, `cosHalfFOV` — `mclib/camera.h:203-204`.
- `projectionAngle` — float, `mclib/camera.h:113`.

**There are no precomputed frustum-plane fields.** A grep for
`frustum|FrustumPlane|nearPlane|farPlane|hither|yon` in `mclib/camera.h`
and `mclib/camera.cpp` returns no matches.

A replacement predicate would need to either:
- **(a) Gribb-Hartmann extract** the 6 planes from `worldToClip` once per
  frame (24 floats; trivial; well-known formula). Cache on Camera.
- **(b) Test directly against `xformCoords` (the pre-divide clip vector)**
  with `homogClip`-style inequalities: `-w ≤ x ≤ w; -w ≤ y ≤ w; 0 ≤ z ≤ w`
  (D3D depth convention; the `MC2_PROJECTZ_TRACE` predicate set already
  measured `homogClip` and `rectNearFar` separately).

Option (b) is cheaper per-call (no plane extraction, reuses the
`worldToClip` multiply already in `projectZ`'s body) but is a known
quantity from the capture data: `homogClip` was 9–51% disagreement at
baseline, mostly **permissive** disagreement (`disagree_perm` non-zero
for mc2_10, mc2_17 only) — i.e. it admits vertices the legacy rect
rejects. The capture report classifies this as the "behind-camera-but-
in-front-of-frustum" case the rect rejects but homogClip accepts. **A
frustum-plane test gives the same answer as `homogClip` with extra
floating-point cost.** That is not a fix; that is rectGuard's superset
property in different clothes.

### 4.2 Distance metric

- **View-space z** (i.e. `cameraOrigin.trans_to_frame(point).z`) is what
  the existing angular cull at `mclib/terrain.cpp:1029-1054` already
  uses (variable `distanceToEye`).
- **World-space distance** is what `mclib/bdactor.cpp:1090-1167`
  recalcBounds uses.
- A distance gate added on top of frustum-plane gating could match
  whichever layer it is replacing.

### 4.3 Drop-in interface

The wrapper signature `inline bool projectFor*Admission(Stuff::Vector3D&,
Stuff::Vector4D&)` at `mclib/camera.h:525-570` returns a bool and
populates the second param with screen.{x,y,z,w}. The signature stays.

The body becomes (sketch, no code):

1. Compute `xformCoords` via `worldToClip` multiply (same as legacy).
2. Compute `screen.{x,y,z,w}` via the same perspective/parallel branches
   as legacy — **including** the `fabs(rhw)` line. Callers depend on
   these values; do not change.
3. Replace the `(screen.x < 0) || (screen.y < 0) || ...` admission bool
   with: `xformCoords.w > 0` (signed-W check) AND frustum-plane test on
   `xformCoords` AND view-distance test.
4. `MC2_PROJECTZ_FINITE_CHECK` `gosASSERT` stays.

This is a drop-in for the bool, **with the screen oracle preserved
byte-identical**. That preserves the consumers documented in § 5.

## 5. Wedge-class callsite assumptions — what consumes the bool, what consumes the screen

| Site ID | file:line | bool consumed → | screen consumed → | Drop-in safe? |
|---|---|---|---|---|
| terrain_quad_vert0_admit | mclib/quad.cpp:804 | `vertices[0]->clipInfo` (gates per-vertex leastZ/mostZ/leastW/mostW tracking and the per-quad clipped1/clipped2 sums at quad.cpp:622-625, which gate triangle submission) | `vertices[0]->wx/wy/wz/ww` (consumed by GPU terrain pipeline) | YES if screen unchanged |
| terrain_quad_vert1_admit | mclib/quad.cpp:871 | same | `vertices[1]->wx/wy/wz/ww` | YES |
| terrain_quad_vert2_admit | mclib/quad.cpp:938 | same | `vertices[2]->wx/wy/wz/ww` | YES |
| terrain_quad_vert3_admit | mclib/quad.cpp:1005 | same | `vertices[3]->wx/wy/wz/ww` | YES |
| terrain_cpu_vert_admit | mclib/terrain.cpp:1597 | `inView` → `currentVertex->clipInfo` → `setObjBlockActive` + `setObjVertexActive` (cull cascade input) | `currentVertex->px/py/pz/pw` | YES if cull cascade is stable under the new bool |
| (terrain.cpp:1438 — second site, not in original 5; may be a new clone since the inventory) | mclib/terrain.cpp:1438 | local `inView`, written to `pxL/pyL/pzL/pwL` (different storage path; see "Surprises" below) | `pxL/pyL/pzL/pwL` | needs audit |
| gameobj_visibility_admit | code/gameobj.cpp:2090 | `isVisible` → `windowsVisible = turn` → feeds `canBeSeen()` → cull/lifecycle (cull_gates_are_load_bearing) | `screenPos` (member; HUD position) | YES if cull cascade is stable |
| cloud_vertex_screen | mclib/clouds.cpp:212 | `clipInfo = onScreen && inView` → cloud submission | screen.x/y/z/w → vertex.px/py/pz/pw | YES |
| crater_corner0..3 | mclib/crater.cpp:323,326,329,332 | `onScreen1..4` → crater quad submission | `currCrater->screenPos[0..3]` (vertex/UV gen) | YES |
| weather_raindrop_top | code/weather.cpp:489 | `onScreen` → outer rain gate | `screen1.x/y/z/w` (z feeds `unsigned char amb` cast) | YES, but z must remain in legacy [0,1] range |
| weather_raindrop_bot | code/weather.cpp:497 | `onScreen` → inner rain draw gate | `screen2.x/y` for line endpoint | YES |

**Total wedge-class admission sites today: 12 unique active callsites**
(4 terrain quad + 2 terrain.cpp + 1 gameobj + 1 cloud + 4 crater +
2 weather, with the sixth terrain.cpp site at line 1438 being either a
clone added after the 2026-04-25 inventory or a duplicate that the
policy-split report missed when it cited "5 terrain admission sites").

The grep in this recon found **6** terrain admission callers
(`mclib/quad.cpp:804,871,938,1005` + `mclib/terrain.cpp:1438,1597`).
The policy-split report at `projectz-policy-split-report.md:56` says 5.
This is a documentation drift Track A's verification appendix should
address.

**Critical: every site populates the second-param `screen` and reads
it.** No site treats the bool independently of the screen output. A
predicate replacement that changes only the bool while preserving
screen population is a clean substitution; one that also changes
screen output is a much wider change (and is likely what the capture
report's "Outcome C" was rejecting in the first place).

## 6. Capture-replay infrastructure status

**File:** `mclib/projectz_trace.cpp` (~430 lines), `mclib/projectz_trace.h`.
Initialization at `:167-190` reads three env vars:
- `MC2_PROJECTZ_TRACE` — per-vertex records to `mc2_projectz.log`
  (~3 us/vertex, drops 150→35 FPS by design; banner at `:184-186`).
- `MC2_PROJECTZ_HEATMAP` — per-callsite × per-predicate counters
  (no perf hit).
- `MC2_PROJECTZ_SUMMARY` — global per-predicate counters
  (no perf hit).
- `MC2_PROJECTZ_GUARD_PX` — int — adjusts `rectGuard` viewport expansion;
  default 0 makes rectGuard ≡ legacyRect.

**Per-callsite × per-predicate × disagreement counters** are emittable,
shown at `mclib/projectz_trace.cpp:218-241` (top-10 outlier loop).
Per-callsite heatmap recorded at `:330-339`. The aggregator
(`scripts/projectz_aggregate.py`, mentioned at
`projectz-capture-report.md:80`) ingests both summary blocks and
trace records and emits CSV.

**The five existing predicates** (`mclib/projectz_trace.cpp:60-62`):
`legacyRectFinite`, `homogClip`, `rectSignedW`, `rectNearFar`,
`rectGuard`. **A frustum+distance candidate would be a sixth predicate.**
Adding it requires:
1. Extending `ProjectZPredicates` (`projectz_trace.h`) with one bool
   field.
2. Computing it in `compute_predicates()` at
   `mclib/projectz_trace.cpp:123-162` from the same inputs.
3. Bumping `PRED_COUNT` from 5 → 6 and extending `s_predNames`,
   `s_predStat`, and the per-callsite arrays (`pred_agree[PRED_COUNT]`
   etc., `:74-79`).
4. The aggregator already iterates all predicates by name; it auto-picks
   up the new one.

This is **mechanical, not architectural**. ~1 day of careful edits +
re-running the existing tier1 baseline + 4 stress captures (the
`projectz-capture-report.md` matrix). The exit gate
"capture-replay disagreement count ≤ legacy baseline" is **directly
measurable** with the existing infra; what is missing is the
disambiguation step (visual overlay confirms whether the new
predicate's permissive disagreements are over-cull-fixes vs
wedge-producers).

The visual overlay (`mclib/projectz_overlay.cpp`) supports cycling
between candidate predicates via RAlt+P (5 modes today; would be 6).
Adding the 6th color-coded mode is also mechanical. **The overlay
itself is functional** — see § 7.

## 7. RAlt+P overlay GL state bug — STALE

The capture report `projectz-capture-report.md:371-378` and the
policy-split report `projectz-policy-split-report.md:136-138` both
list "RAlt+P overlay GL state bug" as a Track A prereq. The capture
report describes it as "`GL_INVALID_OPERATION` at
`gameos_graphics.cpp:577`," likely cause: VAO not bound when the
overlay calls `gos_DrawTriangles`.

**This bug was fixed in commit `dec89aa "projectz: fix overlay draw state"`**
on `terrain-pbr-mod`. The fix lives at `mclib/projectz_overlay.cpp:226`:

> `gos_RendererRebindVAO();`
> `// pp->endScene() leaves VAO 0 bound; rebind so attribute 0 is active`
> `// (AMD silently drops draws when attribute 0 is unbound).`

The same commit also corrected the byte-order on overlay color
constants and converted legend quads to triangle pairs (since
`gos_DrawQuads` is globally disabled).

**Recommendation:** strike "RAlt+P overlay GL state bug" from Track A's
prereqs in the roadmap. Verification: a fresh tier1 + 30s `mc2_01`
visual smoke with `RAlt+P` cycled through all 5 modes confirms it
draws the predicate-disagreement overlay without GL errors. The
`MC2_GL_ERROR_DRAIN_SILENT=0` default-on instrumentation (worktree
CLAUDE.md) makes any regression visible.

## 8. Mod-content / non-stock risk

Track A is gated stock-only per `memory/feedback_offload_scope_stock_only.md`.
The wedge-class wrapper math runs in **every** content set because the
predicate is in `Camera::projectForTerrainAdmission` — there is no
content-conditional dispatch. So a change to the bool computation will
also affect Carver5O / Magic / Wolfman content. Two specific risks:

1. **Custom missions with extreme camera altitudes / FOVs** — Wolfman's
   `cameraAltitude=6000` and `GameVisibleVertices=200` (CLAUDE.md
   "Wolfman mode status") drives 200×200=40000 vertex projections per
   frame. A frustum-plane test on those vertices with the wrong near
   plane definition will over-cull harder than the legacy rect at far
   zoom. Today wolfman zoom is the broken case for the legacy rect; a
   modern predicate could trade one regression for another.
2. **Magic / Carver5O custom-content hand-tuned camera setups** — Magic
   Unofficial Expansion ships custom cameras and might exercise edge-of-
   frustum cases the stock content doesn't. The capture matrix didn't
   include custom-content missions (`projectz-capture-report.md:389-392`,
   open question 4). Stock-only validation is a weaker exit gate than
   the roadmap implies; user-report regressions on mod content are a
   real possibility post-flip.

Neither of these blocks Track A. They argue for an env-flag killswitch
(`MC2_PREDICATE_REPLACEMENT=0` opts back to legacy) so user reports of
"X stopped rendering on mod Y" can be A/B-tested in seconds.

## 9. Open questions for brainstorm

1. **Does Track A replace the bool only, or also the screen oracle?**
   Capture-report Outcome C strongly implies bool-only. The roadmap
   text ("Same gate, better math") is consistent with bool-only. But
   "rect-screen-finite" terminology in the roadmap is ambiguous; this
   should be made explicit in the spec.

2. **All three wedge wrappers in one slice, or stagger?** The capture
   report's per-category verdict (§ 5.1) says **terrain BoolAdmission
   keep legacy** (high confidence). Track A proposes terrain first,
   which contradicts the capture report's recommendation. The other
   two wedge wrappers (object, effect) have less data — object has 1
   site (`code/gameobj.cpp:2090`) feeding the cull cascade; effects
   are 7 sites (cloud + crater + weather) where weather already shows
   99.9% over-cull on rectSignedW. **Why terrain first if it has the
   strongest "keep legacy" signal in the data?**

3. **What baseline does the disagreement-count gate use?** The capture
   report has 5 baseline missions × 60s + 4 stress captures + 1 GUARD64
   capture. Is "≤ legacy baseline" comparing against legacyRect's
   trivial 0% disagreement (impossible — anything different from legacy
   is by definition disagreement) or against the rectGuard /
   homogClip / rectSignedW / rectNearFar disagreement floors? The exit
   criterion as stated needs operationalization.

4. **Is the visual-overlay disambiguation step still required?** The
   capture report (`§ 5 conditions for verdict tightening`) said
   rectGuard could not be admitted without visual proof its terrain
   permissive admissions were over-cull-fixes vs wedge-producers. The
   overlay GL bug is fixed (§ 7); the disambiguation step is now
   unblocked. Should Track A's exit gate require operator-side visual
   inspection at wolfman zoom + 4 stress missions before any flip?

5. **Is the 1438 / 1597 terrain.cpp split a clone or a real second
   codepath?** The 2026-04-25 inventory listed 1 terrain.cpp admission
   site; today there are 2 (lines 1438 and 1597). If both are
   simultaneously active under different gates, replacement of one but
   not the other introduces a divergent admission policy inside the
   same loop.

6. **Killswitch shape.** Roadmap doesn't specify. Recommended: env-flag
   default-on legacy, `MC2_PREDICATE_REPLACEMENT=1` opts in (during
   soak); flip default at a separate commit after tier1 + 4-stress
   convergence. This matches the established pattern
   (`MC2_MODERN_TERRAIN_PATCHES`, `g_useGpuStaticProps`, etc.).

## 10. Sizing — is Track A still ~1 week?

**If the slice is bool-only with screen oracle preserved:**
- Add 6th predicate to `projectz_trace.{cpp,h}` + overlay color: ~1 day.
- Re-run tier1 + 4-stress captures (~2 hours wall-clock per capture +
  aggregator runs): ~1 day.
- Implement frustum+distance bool inside `projectForTerrainAdmission`
  body; preserve screen oracle: ~1 day.
- Killswitch + parity-mode (run both predicates, log disagreement) +
  visual overlay validation at wolfman zoom + 4-stress: ~2 days.
- Spec write-up + adversarial-plan-review pass: ~1 day.

That's ~5–6 days, consistent with the ~1 week roadmap estimate. **The
sizing is plausible IF item 2 (terrain first) is the right starting
wrapper; if the brainstorm decides effects or objects are a safer first
wedge, the sizing is similar but the wrapper changes.**

**If the slice tries to replace the screen oracle alongside:** sizing
balloons to multi-week because (a) every consuming site needs an audit
of which screen.{x,y,z,w} fields are load-bearing for downstream math
(notably `weather.cpp:498` `unsigned char amb` cast on screen.z), and
(b) the capture report's already-rejected modern predicates produced
the screen oracle differently — the verdict was driven exactly by that
divergence. This is the trap.

## 11. References — verified file:line citations

- `mclib/camera.h:435-515` — `projectZ()` body (legacy implementation).
- `mclib/camera.h:459` — `screen.w = fabs(rhw);` (W-sign destruction).
- `mclib/camera.h:471` — pixel-rectangle admission test.
- `mclib/camera.h:525-538` — `projectForTerrainAdmission` wrapper.
- `mclib/camera.h:541-554` — `projectForObjectAdmission` wrapper.
- `mclib/camera.h:557-570` — `projectForEffectAdmission` wrapper.
- `mclib/camera.h:137` — `worldToClip` matrix field.
- `mclib/camera.h:129` — `cameraOrigin` field.
- `mclib/camera.h:203-204` — `camera_fov`, `cosHalfFOV` fields.
- `mclib/camera.h:274` — `cameraAltitude` field.
- `mclib/quad.cpp:804,871,938,1005` — terrain quad admission callers
  (4 sites, vert0..vert3).
- `mclib/terrain.cpp:1438,1597` — terrain.cpp admission callers
  (2 sites; inventory listed only 1 — drift).
- `code/gameobj.cpp:2090` — object admission caller (1 site).
- `mclib/clouds.cpp:212` — cloud effect admission (1 site).
- `mclib/crater.cpp:323,326,329,332` — crater effect admission (4 sites).
- `code/weather.cpp:489,497` — weather effect admission (2 sites).
- `mclib/projectz_trace.cpp:60-62` — predicate name array (5 today).
- `mclib/projectz_trace.cpp:74-79` — `HeatmapEntry::pred_agree[PRED_COUNT]`
  arrays.
- `mclib/projectz_trace.cpp:123-162` — `compute_predicates()` body.
- `mclib/projectz_trace.cpp:167-190` — env var initialization.
- `mclib/projectz_trace.cpp:218-241` — top-10 outlier emission.
- `mclib/projectz_overlay.cpp:226` — `gos_RendererRebindVAO()` (overlay
  GL fix from `dec89aa`).
- `mclib/projectz_overlay.h:27-35` — overlay mode enum (5 active modes).
- `docs/superpowers/specs/projectz-callsite-inventory.md:32-37,141-203` —
  6 BoolAdmission sites detail.
- `docs/superpowers/specs/projectz-policy-split-report.md:56-66` — wedge-
  class wrapper distribution.
- `docs/superpowers/specs/projectz-capture-report.md:23-34,138-142,
  176-198,254-269,371-378` — Outcome C verdict, baseline data,
  stress convergence, GUARD=64 superset, RAlt+P bug.
- `docs/superpowers/explorations/2026-04-27-modern-terrain-surface-findings.md:100-130`
  — vertex-loop walkthrough with line-anchored `projectForTerrainAdmission` call.
- `docs/gpu-static-prop-cull-lessons.md:8-18` — wolfman zoom 87% bug shape.
- `memory/wolfman_is_max_zoom.md` — wolfman terminology.
- `memory/cull_gates_are_load_bearing.md` — cull cascade hazard.
- Commit `dec89aa "projectz: fix overlay draw state"` — overlay GL fix.
