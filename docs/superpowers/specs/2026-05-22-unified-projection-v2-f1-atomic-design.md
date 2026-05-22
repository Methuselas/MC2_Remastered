# Unified projection v2 — F1 atomic META-FIX

<!-- POSTSCRIPT 2026-05-22 LATE (Task 7b): A-pre transport FINAL -- probe SSBO at binding=23.
Three attempts: (1) std140 UBO binding=0 -- AMD does not propagate UBO to TES; (2) flat
glUniformMatrix4fv after apply() -- glGetUniformfv readback confirmed non-zero but TES
read zeros (AMD TES stage isolation); (3) FINAL: matrix stored as float[16] at byte offset 32
in the existing probe SSBO (binding=23), written via glBufferSubData + re-issued
glBindBufferBase. TES reads via ssbo_readWorldToClipGL(). Transport proven: compared > 0
at every frame with MC2_TERRAIN_INDIRECT=0. Large max_delta expected (legacy vs direct
clip-space; converges at Stage A). Per-program flat-uniform approach in SS2.1.1 + SS5.1
SUPERSEDED. Addendum (authoritative):
docs/superpowers/plans/2026-05-22-unified-projection-v2-f1-atomic-plan-addendum-ubo-pivot.md -->

Date: 2026-05-22. Worktree: `claude/nifty-mendeleev` @ `c9b6406`.
Status: DESIGN v2.8 (greybeard skill verdict 2026-05-22: **META-FIX ready, no blocking findings**. All in-scope items ruled META-FIX; all 6 carve-outs ruled PATCH-with-justified-promotion-trigger; CullUBO field rename ruled PATCH-justified with explicit trigger. Substitutive-test discipline confirmed. v2.8 == v2.7 substantively; version bump records greybeard sign-off) — READY for writing-plans skill.
Supersedes: `docs/superpowers/specs/2026-05-20-unified-projection-meta-fix-design.md` (v1 DRAFT, 11 revision items in POSTSCRIPT).

## TL;DR

Collapse the 4 duplicate `axisSwap · worldToCamera · cameraToClip` compositions
into a single `Camera::worldToClipGL()` accessor (M3 META-FIX). Upload result
to a new `u_worldToClipGL` uniform slot. Migrate 14 vertex shaders + 3
compute/frag consumers to the clean GL convention (`gl_Position = mvp * vec4(world,
1.0)`, no `abs(clip.w)` round-trip). Retire `terrainMVP` slot + `gos_SetTerrainMVP`
API atomically. CPU `pz` gate in `quad.cpp`, `Camera::projectZ` body, all 8
wrappers, and instrumentation **stay alive** in F1 — they retire in follow-on
slices after empirical red-band verification.

F1 is **correctness + structural cleanup**, not perf. Per-actor CPU work is
already 0us (A2 + static-update-skip). The "GPU owns projection" framing remains
the long-term destination; F1 is its foundational matrix slice.

**M3 status:** F1 delivers M3 (single composition source) **for the runtime
GPU uniform path only**. `Camera::worldToClip` (pre-axisSwap, internal to
`projectZ`/wrappers), TGL class-statics, and MLR composition remain as
documented exceptions; they retire alongside their consumers in follow-on
arcs. See §9.

## Provenance

- v1 (`0befb97` + `09a47ac`) was DRAFT-downgraded with 11 revision items in
  POSTSCRIPT. v2 folds:
  - **Pivot finding** (`mlr-does-not-block-unified-projection` memory): under
    default `MC2_DISABLE_GOSFX=1`, MLR's `cameraToClip(2,2)/(3,2)` reads SET
    STATE (`farClipReciprocal`) that no downstream code consumes. R-MLR
    HIGH-RISK from v1 retires. Mitigation (c) accepted: dev-override path
    documented; not fixed in F1.
  - **F3 cost-split data** (commit `d064b77` + subsequent measurements):
    `tgl_transform = recalcBounds_perframe = skinning_chain = 0us` in stock
    workloads via `MC2_STATIC_UPDATE_SKIP=1` (default). `mlr_total` 408us →
    1.7us pre/post-A2. CPU budget goal already met. F1 = correctness, not perf.
  - **Track A1 ship** (`e66aee4`, 2026-05-06): `projectForObjectAdmission`
    already modernized to clip-space frustum predicate with
    `LegacyProjectionResult` dual-output discipline. F1 does NOT re-modernize
    object admission.
  - **Inverse-projection consumer-collapse Phases 1-5 ship**
    (`1b9a9e4`...`7ab53a3`, 2026-05-XX): `setInverseProject` + 4 scalar fields +
    `inverseProjectZ` + slimReduce RED reduction + water-block writers all
    deleted. F1 does NOT re-touch this surface.
  - **VPL retired, slimReduce live** (per `terrain.cpp:1670`). Per-vertex
    `projectForTerrainAdmission` call at `terrain.cpp:1783` is the current
    terrain admission hot path inside slimReduce, not the old vertexProjectLoop.
  - **glClipControl adoption shipped** (per `gameosmain.cpp:972-974`):
    `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` active. Engine already on
    [0,1] NDC depth. `cameraToClip` already has reverse-Z polarity
    (`camera.cpp:2086,:2145`: `-1.0f/(far-near)` and `-near*depth_range`). v1's
    C2 "perspectiveRH_ZO is not reverse-Z" is moot — we are not adopting glm;
    existing hand-built matrix is correct, we are only changing the upload path
    and consumer convention.
- v1 revisions retired by pivot:
  - **R-MLR (HIGH-RISK)** → retires under default gate. Mitigation (c) for
    `MC2_DISABLE_GOSFX=0` override.
  - **C2 (perspectiveRH_ZO polarity)** → retires. F1 does not introduce glm.
  - **G2/G3 (Stage 0 probe)** → retires. Replaced by build-time parity probe
    scaffold in `gos_terrain.tese` (per-vertex CPU-NDC vs new-NDC compare).
- v1 revisions carried forward:
  - **C3 (4 missed compute/frag shaders)** → folded into §4 component table.
  - **M1 (every file:line cite wrong)** → every cite in this v2 grep-verified
    at write-time 2026-05-22.
  - **G1 (7 stages → 2)** → F1 is the single atomic slice; one cleanup tail
    deferred to follow-up.
  - **G4/G5 (M2 + M3 with promotion triggers)** → M3 delivered by F1 (see §9).
    M2 (retire Stuff from camera path) named with promotion trigger.
- Pivot anti-pattern named in `mlr-does-not-block-unified-projection` memory:
  framing inheritance rot. Verified by re-grep of the FIRST dependency-chain
  link at session boundary. v2 maintains discipline by listing each carried/
  retired v1 finding explicitly above.

## 0. P0 invariants (must hold before Stage A-pre commits)

1. **axisSwap applied exactly once.** Verified at write-time 2026-05-22 via grep:
   `mclib/camera.cpp:2367` shows `worldToCameraMatrix.Invert(cameraToWorldMatrix)` — view-inverse only, axisSwap NOT baked into `worldToCameraMatrix`. Multiple
   upload-site comments (`code/gamecam.cpp:177`, `GameOS/gameos/gos_mech_batcher.cpp:1199`,
   `GameOS/gameos/gos_static_prop_batcher.cpp:3000`, `GameOS/gameos/gameos_graphics.cpp:5243`)
   confirm `terrainMVP = axisSwap * worldToClip` is applied at upload, not at
   `Camera::worldToClip` composition. Therefore:
   - `Camera::worldToClip` (existing, `mclib/camera.cpp:2369`) = `worldToCameraMatrix * cameraToClip` **without** axisSwap. Feeds `Camera::projectZ` body and 8 wrappers; their internal math operates in MC2-clip coords pre-axisSwap.
   - `Camera::worldToClipGL()` (new) = `axisSwap * worldToCameraMatrix * cameraToClip` **with** axisSwap baked in once. Matches the existing per-upload-site product, collapsed to one source.
   - Two compositions differ exactly by axisSwap. Not "same product." Stage A-pre basis-vector test (below) gates this invariant.
2. **`glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` clip-volume rule.** Per
   Khronos under `GL_ZERO_TO_ONE` depth mode, GPU clipping accepts vertices
   satisfying:
   ```
   x/y: -w <= x, y <= w
   z  :  0 <= z   <= w
   ```
   v1's `-w <= {x,y,z} <= w` formulation was the `GL_NEGATIVE_ONE_TO_ONE`
   rule; wrong under our adopted convention. This invariant propagates to
   every compute/frag admission predicate (§4.4).
3. **Stage A-pre parity probe in-front semantics.** Behind-camera vertices
   under F1 produce intentionally divergent NDC from the legacy
   `abs(clip.w)` kludge. Parity probe gates only on numerically-comparable
   in-front vertices (`old.w > epsilon AND new.w > epsilon AND finite(NDCs)`).
   Behind-camera + non-finite + near-zero-w vertices are counted separately.
4. **Compute/frag replacement semantics enumerated pre-Stage-A.** §4.4 commits
   per-site replacement decisions (pz/depth-only gate vs full clean-GL
   frustum cull) at design level. No "plan-stage will decide" for the 3
   compute/frag sites (post-ssao-drop); design commits or Stage A is not
   atomic.
5. **Shader-visible matrix orientation invariant** (per fourth-round reviewer P0.2).
   Three distinct matrix-views must be kept straight at design and plan-write:
   - **CPU mathematical matrix** = `axisSwap_MC2toGL * worldToCameraMatrix * cameraToClip` (the product Stuff::Matrix4D::Multiply produces).
   - **CPU memory layout** = column-major (Stuff::Matrix4D internal).
   - **GLSL-visible uniform matrix** = whatever `glUniformMatrix4fv(..., GL_FALSE, M)` produces given the C++ buffer `M` passed.

   The legacy upload manually repackages column-major Stuff bytes → row-major
   `M[16]` BEFORE `GL_FALSE` upload; `gos_SetWorldToClipGL` MUST do the same
   repackage (§2.1.1). Result: GLSL(`u_worldToClipGL`) must equal
   GLSL(legacy `terrainMVP`) for the same camera state. Verify in the
   probe by comparing `terrainMVP * world` against `u_worldToClipGL *
   world` directly in GLSL — see §5.1 revised probe. The basis-vector
   CPU test (invariant 6 below) compares shader-visible matrices, NOT raw
   Stuff::Matrix4D element layouts.

6. **Basis-vector smoke test (CPU-side, pre-Stage-A-pre).** Per second-round
   reviewer P0.3: the oracle must compare the **old GPU upload product** vs
   `worldToClipGL()`, NOT `projectZ`. `projectZ` operates in pre-axisSwap
   MC2-clip coords; comparing it against post-axisSwap GL coords would
   trivially fail with delta ≈ axisSwap magnitude on most basis vectors.
   The correct CPU oracle composes the existing upload-site product:

   ```cpp
   // Reconstruct the existing axisSwap * worldToClip product the same way
   // code/gamecam.cpp:165-185 does (read worldToClip column-major, apply
   // row-permutation+negation per the axisSwap_MC2toGL literal in §2.1):
   Stuff::Matrix4D oldGpuUploadMVP = ... ; // copy of existing upload logic
   Stuff::Matrix4D newGpuUploadMVP = camera.worldToClipGL();
   ```

   Per fifth-round codex P1.3: compare **the GLSL-visible matrix
   representation** produced by `GL_FALSE` upload of each path, not the raw
   `Stuff::Matrix4D` storage. Both `oldGpuUploadMVP` and
   `newGpuUploadMVP = camera.worldToClipGL()` must be converted to their
   GLSL-visible form (apply the same column-major-to-row-major repackage
   `gos_SetTerrainMVP` and `gos_SetWorldToClipGL` apply) before
   element-wise comparison. Otherwise the test compares storage layouts
   rather than what shaders actually see.

   Compare GLSL-visible matrices element-by-element AND compare shader-
   equivalent NDC for 7 known points:
   - camera origin
   - +world-x unit
   - +ground-y unit (MC2 ground axis)
   - +elevation-z unit (MC2 elevation axis)
   - near-plane center point (in front)
   - far-plane center point (in front)
   - behind-camera point

   For in-front points (5 of 7), element-wise abs delta < 1e-5 and NDC
   delta < 1e-3. For behind-camera (1 of 7), delta is expected non-zero and
   counted, not gated. For camera-origin degenerate (1 of 7), w near zero,
   skipped per the parity-comparable rule in P0.3.

   Keep `projectZ` byte-identity as a SEPARATE wrapper-stability test
   (legacy wrapper remains unchanged in F1 — `projectZ` output before vs
   after F1 should match byte-identical because the `Camera::worldToClip`
   pre-axisSwap path is untouched). Do not conflate the two oracles.

   Test ships as gated dev tool (`MC2_UNIFIED_PROJECTION_BASIS_TEST=1`) or
   one-off scaffold deleted pre-Stage-A; plan-stage decides.

## 1. Scope

F1 retires the **CPU-composed pixel-homogeneous `terrainMVP` upload path** and
the **per-shader screen→pixel→NDC round-trip kludges**. It replaces both with
a single clean GL `u_worldToClipGL` matrix uploaded from a single CPU
composition site.

**F1 is correctness-only.** Per F3 data, every per-actor CPU bucket reads
0us in stock content. The "eliminate per-frame CPU work" campaign goal was
already met by A2 + static-update-skip. F1's value is:
- Single source of truth for projection composition (M3 META-FIX, §9)
- Coherent shader-side projection model (`clip.w == z_eye` monotonic; GPU
  clipper handles behind-camera rejection via `x/y: -w ≤ x,y ≤ w; z: 0 ≤ z ≤ w`
  under our adopted `GL_ZERO_TO_ONE` mode — see §0.2 invariant)
- Foundation for follow-up slices (CPU pz gate retire, wrapper modernization)

**F1 is NOT:**
- Not a CPU perf slice. No measurable CPU savings expected.
- Not a `projectZ` retirement. Body stays; wrappers stay; instrumentation
  stays. Each retires in its own follow-up arc.
- Not a CPU `pz` gate retirement. `mclib/quad.cpp` BoolAdmission machinery
  + the 5 `projectForTerrainAdmission` callsites stay. Red-band class
  verification post-F1 will gate the retirement slice.
- Not an MLR touch. Mitigation (c) accepted.
- Not an editor convergence slice. Editor lives in separate worktree.
- Not a parallel-slot strategy. `terrainMVP` slot retires in same atomic
  commit; F1 ships single end-state.

## 2. Math

The matrix product is unchanged from current production: all 4 existing CPU
composition sites already compose

```
worldToClip = axisSwap_MC2toGL · worldToCamera · cameraToClip
```

where:
- `axisSwap_MC2toGL`: permutation+negation matrix mapping MC2 world axes to
  the shader-clip orientation the legacy upload produces. Per the committed
  literal in §2.1: `GL.x = -MC2.x`, `GL.y = MC2.elevation` (was `z`),
  `GL.z = MC2.ground` (was `y`, POSITIVE forward sign — NOT stock OpenGL
  `-Z forward`; this engine's clip convention preserves `clip.w > 0` for
  in-front geometry via the perspective `cameraToClip` build, not via
  `-Z forward` semantics). Do not "correct" the sign to stock OpenGL during
  plan-write or implementation; the literal is transplanted from the
  existing upload site and must match byte-for-byte.
- `worldToCamera`: existing MC2 view matrix derived from `Camera::cameraOrigin`.
- `cameraToClip`: existing hand-built reverse-Z perspective matrix at
  `mclib/camera.cpp:2129-2151`. **Untouched.** Already correct for clean GL
  consumption: forward-axis row has `-near_clip * depth_range` polarity matching
  reverse-Z (near → w, far → 0).

What changes in F1 is the **upload path** and **shader consumption convention**,
not the matrix math.

### 2.1 `Camera::worldToClipGL()` body

Normal non-inline member: header decl in `mclib/camera.h` near other matrix
accessors; body in `mclib/camera.cpp` near the existing `Camera::worldToClip`
composition at `:2369`:

```cpp
// mclib/camera.h
class Camera {
    // ...
    Stuff::Matrix4D worldToClipGL() const;
    // ...
};

// mclib/camera.cpp
Stuff::Matrix4D Camera::worldToClipGL() const
{
    // Calling convention for Stuff::Matrix4D::Multiply is `dst.Multiply(a, b)`
    // = dst = a * b. Verified against existing site
    // mclib/camera.cpp:2369: worldToClip.Multiply(worldToCameraMatrix, cameraToClip);
    Stuff::Matrix4D viewClip;
    viewClip.Multiply(worldToCameraMatrix, cameraToClip);
    Stuff::Matrix4D out;
    out.Multiply(axisSwap_MC2toGL, viewClip);
    return out;
}
```

`axisSwap_MC2toGL` literal — transplanted from current upload site
`code/gamecam.cpp:168-175` per second-round reviewer P0.2. The existing
upload-time loop writes:

```cpp
// code/gamecam.cpp:168-175 (current upload-site logic, axisSwap composed at runtime)
AW[0][j] = -WTC(0,j);   // GL row 0 = -(worldToClip row 0)   -> MC2.x negated
AW[1][j] =  WTC(2,j);   // GL row 1 =  (worldToClip row 2)   -> elevation -> up
AW[2][j] =  WTC(1,j);   // GL row 2 =  (worldToClip row 1)   -> ground -> forward (POSITIVE)
AW[3][j] =  WTC(3,j);   // GL row 3 unchanged
```

Equivalent left-multiplication matrix:

```
axisSwap_MC2toGL =
| -1   0   0   0 |     // GL.x = -MC2.x          (negated)
|  0   0   1   0 |     // GL.y =  MC2.z          (elevation -> up)
|  0   1   0   0 |     // GL.z =  MC2.y          (ground -> forward, positive)
|  0   0   0   1 |     // homog
```

Verified against existing upload-site logic at write-time 2026-05-22. No
TODO. No "plan-stage verifies sign." This is the committed literal.

Stored as `static const Stuff::Matrix4D` in `mclib/camera.cpp` translation
unit; populated once at module init.

### 2.1.1 GL upload transpose discipline (per third-round reviewer P0.3)

CRITICAL — preserves the legacy GLSL matrix orientation across F1.

**Stuff::Matrix4D storage:** column-major in memory. Verified at write-time:
`code/gamecam.cpp:166` uses `#define WTC(r,c) W[(c)*4+(r)]` to access cells
of `Stuff::Matrix4D` via raw float pointer cast — `c*4 + r` = column-major.

**Stuff::Matrix4D::Multiply convention:** `dst.Multiply(S1, S2)` computes
`dst(i,j) = Σₖ S1(i,k) * S2(k,j)` — i.e. `dst = S1 * S2`. Verified at
`mclib/stuff/matrix.cpp:253-258`.

**Legacy upload protocol** (`code/gamecam.cpp:165-187`):

1. CPU composes `worldToClip = worldToCamera * cameraToClip` via Stuff's
   `Multiply` — `worldToClip` is column-major Stuff::Matrix4D.
2. CPU manually applies axisSwap AND repackages to row-major `float M[16]`:
   ```cpp
   AW[0][j] = -WTC(0,j);  AW[1][j] = WTC(2,j);  AW[2][j] = WTC(1,j);  AW[3][j] = WTC(3,j);
   // ...
   M[i*4+j] = AW[i][j];   // M is row-major
   gos_SetTerrainMVP(M);  // uploaded with GL_FALSE
   ```
3. `GL_FALSE` upload + row-major source ⇒ OpenGL reads each C++ row as a
   GLSL column ⇒ GLSL sees the **transpose of AW**, which under Stuff's
   row-vector convention is the form needed for `gl_Position = M_gl *
   vec4(world, 1)` to compute `M * world` correctly.

**F1 upload protocol (`gos_SetWorldToClipGL`):** preserves the existing
GLSL-side orientation. Per Vulkan-prep discipline ("explicit device-mediated
binding; no implicit cross-call GL state" — `vulkan_prep_explicit_device_discipline.md`),
the API takes an explicit program/shader handle, NOT depending on the
ambient `glUseProgram` binding. The accessor returns `Stuff::Matrix4D`
(column-major in memory); the GOS API repackages to row-major before
`GL_FALSE` upload so GLSL sees the same orientation as before:

```cpp
// Explicit program/shader handle per Vulkan-prep "device-mediated binding"
// rule. Producer sites at gamecam.cpp:151 and simplecamera.cpp:168 already
// know which programs are live; threading the handle through is trivial
// today and prevents per-callsite retrofit during a future Vulkan port.
void gos_SetWorldToClipGL(ShaderProgram& prog, const Stuff::Matrix4D& mat)
{
    GLint loc = prog.getUniformLocation("u_worldToClipGL");
    if (loc == -1) return;  // see §5.1 location-binding behavior below
    const float* col = (const float*)&mat;
    #define WTC(r,c) col[(c)*4 + (r)]
    float M[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            M[i*4 + j] = WTC(i, j);  // column-major → row-major
    #undef WTC
    prog.uploadMatrix4(loc, M, /*transpose=*/false);  // GL_FALSE equivalent
}
```

(Exact `ShaderProgram` API surface is plan-stage detail — may be the
existing GOS shader-handle type or a new typed wrapper. Either way: the
function signature takes the program EXPLICITLY, never relies on whatever
`glUseProgram` happened to be last called.)

**Alternative considered + rejected:** upload `Stuff::Matrix4D` raw bytes
(column-major) with `GL_TRUE`. GLSL would see the matrix in the same
orientation as the repackage path. But this VIOLATES the worktree CLAUDE.md
load-bearing rule: "GL_FALSE for terrainMVP: direct-uploaded row-major
matrices use GL_FALSE. The gamecam.cpp comment claiming GL_TRUE is wrong;
do not 'fix' it." Convention preserved via repackage; not by changing
upload flag.

Stage A-pre basis-vector test (§0.5) catches any transposition mismatch
empirically before atomic flip lands.

### 2.2 Why this retires the per-shader kludge

Current path produces `vec4 pixelHomog = terrainMVP * vec4(world, 1.0)` whose
`.w` has been packaged for D3D pixel-homogeneous output. Every consumer shader
applies `abs(.w)` then re-derives NDC, destroying clip.w sign for behind-camera
verts.

After F1, `u_worldToClipGL * vec4(world, 1.0)` produces clip space with
`clip.w == z_eye > 0` for in-front, `< 0` for behind-camera. Under our
adopted `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` mode, GPU clipper's
acceptance rule is:

```
x/y: -w <= x, y <= w
z  :  0 <= z   <= w
```

(Note: this is the `GL_ZERO_TO_ONE` rule. v1 had `-w <= {x,y,z} <= w` —
that's `GL_NEGATIVE_ONE_TO_ONE` and would mis-spec compute admission
predicates if folded uncorrected.)

Behind-camera verts (`clip.w < 0`) violate the x/y rule and are rejected. No
`abs(.w)` workaround needed; no screen→pixel→NDC round-trip; no per-shader
guard for clip.w sign.

## 3. Data flow

**Producer side (1 composition + 1 cache + N readers per M4 corrected framing):**

```
Camera::worldToClipGL()  [single composition source for runtime GPU path]
    |
    \-- gamecam.cpp:165-187 (replaces inline AW composition + gos_SetTerrainMVP)
            \-- gos_SetWorldToClipGL(prog, mat)
                  \-- (a) writes terrain_mvp_ cache in gameos_graphics.cpp:1399
                  \-- (b) glUniformMatrix4fv(loc, ..., GL_FALSE, M_row_major)
                          for the EXPLICITLY-passed program

[15+ consumer bind sites read the cache via gos_GetTerrainMVPMat4() — see §4.1
 cache-reader list. They inherit the new cache value transparently. The 10
 glGetUniformLocation lookups for "terrainMVP" rename to "u_worldToClipGL"
 in same atomic Stage A commit.]
```

**`simplecamera.cpp` is NOT a current producer** (per adversarial M2 +
`docs/observations/2026-05-14-render-terrain-indirect-mvp-lag.md:12`). The
sole `gos_SetTerrainMVP` caller is `code/gamecam.cpp:187`. MechBay frames
inherit the stale gameplay-frame `terrain_mvp_` cache (today's behavior;
F1 does not change this).

`TG_Shape::s_cameraToClip` / `s_worldToClip` (existing class-statics at
`mclib/tgl.cpp:53, mclib/tgl.h:792`) — left alive but no longer the source of
truth for GPU upload. `mclib/camera.cpp:1737` `TG_Shape::SetCameraMatrices`
call kept (still populates the class-statics; downstream TGL CPU consumers
unchanged in F1; per F3, those buckets are 0us anyway).

`mclib/mlr/mlrclipper.cpp:321` MLR's own `worldToClipMatrix.Multiply(...)`
left untouched per mitigation (c).

**Consumer side (shader edit, 13 + 4 sites):**

Vertex shader pattern (replaces `terrainMVP` consumer pattern):
```glsl
uniform mat4 u_worldToClipGL;
// ...
gl_Position = u_worldToClipGL * vec4(world, 1.0);
```

Compute/frag pattern (admission/depth-test sites): see §4.4 + §4.4.1 for
per-site replacement decisions. Replacements split into three categories:
pz/depth-only (terrain_solid + water) and full clean-GL frustum cull
(gpu_cull_predicate). ssao dropped per §4.4. Plan-write splices the exact
replacements per §4.4 table.

## 4. Components + sites

All cites grep-verified 2026-05-22 (M1 discipline).

### 4.1 CPU producer + bind-site census (per M4 corrected framing + M5 enumeration)

Per adversarial M4 + M5: the actual shape is **1 composition site + 1 cache
+ N readers**, NOT "4 dup compositions." All consumers ultimately read the
same `terrain_mvp_` cache via `gos_GetTerrainMVPMat4()` or via flat uniform
`terrainMVP` resolved by `glGetUniformLocation`. F1 changes WHAT is written
into the cache; consumer-side bind sites all transparently inherit the new
value. The uniform NAME also renames from `terrainMVP` to `u_worldToClipGL`,
which DOES propagate to each `glGetUniformLocation` site listed below.

**Composition + producer sites:**

| # | Site | Current code | F1 disposition |
|---|---|---|---|
| 1 | `mclib/camera.h` + `mclib/camera.cpp` | n/a | **Add** new normal non-inline member `Camera::worldToClipGL()` (decl in header, body in cpp; per §2.1) |
| 2 | `mclib/camera.cpp:2369` | `worldToClip.Multiply(worldToCameraMatrix, cameraToClip);` (existing `Camera::worldToClip`; **pre-axisSwap, MC2 coords**) | **Keep** — feeds `projectZ` body and 8 wrappers, which operate in MC2-clip coords. Different product from `worldToClipGL()` by axisSwap factor. Per P0.1 invariant |
| 3 | `code/gamecam.cpp:165-187` | Inline `AW = axisSwap * worldToClip` composition + `gos_SetTerrainMVP(M)` upload | **Replace** the inline composition with `gos_SetWorldToClipGL(prog, camera.worldToClipGL())`. The inline `AW[4][4]` repackage + `M[16]` array + manual transpose logic ALL retire (the repackage moves into `gos_SetWorldToClipGL` body). Composition logic moves into `Camera::worldToClipGL` |
| 4 | `code/gamecam.cpp:149-151` | `theClipper->StartDraw(cameraOrigin, cameraToClip, ...); GOSVertex::farClipReciprocal = ...;` | Keep MLR `StartDraw` call alive per mitigation (c). NOT a producer site for the GPU uniform path |
| 5 | `code/simplecamera.cpp:168-169` | **NOT a current `gos_SetTerrainMVP` producer.** Per `docs/observations/2026-05-14-render-terrain-indirect-mvp-lag.md:12`: `gos_SetTerrainMVP` has exactly ONE caller, `code/gamecam.cpp:176/187`. simplecamera.cpp does NOT upload terrainMVP today. | **DROPPED from F1** per adversarial M2. MechBay either does not render the 14 migrated vert shaders OR uses the stale gameplay-frame `terrain_mvp_` cache value — same as today. No behavior change |
| 6 | `mclib/tgl.cpp:1620,1624` | `s_cameraToClip = camToClip; s_worldToClip.Multiply(s_worldToCamera, *s_cameraToClip);` | **Keep** (TGL class-statics still populated by `SetCameraMatrices`; downstream CPU consumers unchanged in F1) |

**Cache + accessor:**

| # | Site | Disposition |
|---|---|---|
| 7 | `GameOS/gameos/gameos_graphics.cpp:1399-1400` | `memcpy(&terrain_mvp_, m, 16 * sizeof(float)); terrain_mvp_valid_ = true;` — cache write inside `gos_SetTerrainMVP` body. **Replace** with cache write inside `gos_SetWorldToClipGL` body (same `terrain_mvp_` storage; rename optional, plan-stage decides) |
| 8 | `GameOS/gameos/gameos_graphics.cpp:7081` | `void __stdcall gos_SetTerrainMVP(const float* matrix16) { ... }` | **Retire** body; replaced by `gos_SetWorldToClipGL(ShaderProgram&, const Stuff::Matrix4D&)` per §2.1.1 |
| 9 | `GameOS/gameos/gameos_graphics.cpp:7661` | `const float* gos_GetTerrainMVPMat4()` accessor body | **Optionally rename** to `gos_GetWorldToClipGLMat4()`. If kept, all 15+ callers below stay un-renamed. Plan-stage picks |

**Consumer bind sites — flat uniform `terrainMVP` glGetUniformLocation lookups (10 sites, ALL rename to `u_worldToClipGL`):**

| # | Site | F1 disposition |
|---|---|---|
| 10 | `GameOS/gameos/gameos_graphics.cpp:1730` | `terrainLocs_.terrainMVP = glGetUniformLocation(shp, "terrainMVP")` → `"u_worldToClipGL"` |
| 11 | `GameOS/gameos/gameos_graphics.cpp:1757` | `thinTerrainLocs_.terrainMVP = ...` → rename string |
| 12 | `GameOS/gameos/gameos_graphics.cpp:1759` | `thinTerrainLocs_.mvp = glGetUniformLocation(shp, "mvp")` — **RETIRE** per M3 atomic bare-`mvp` cleanup. Field deleted from struct, lookup deleted |
| 13 | `GameOS/gameos/gameos_graphics.cpp:3530` | `glGetUniformLocation(prog, "terrainMVP")` → rename string |
| 14 | `GameOS/gameos/gameos_graphics.cpp:3958` | `locs.terrainMVP = ...` → rename string |
| 15 | `GameOS/gameos/gameos_graphics.cpp:3960` | `locs.mvp = ...` — **RETIRE** per M3 |
| 16 | `GameOS/gameos/gameos_graphics.cpp:6928` | `GLint mvpLoc = glGetUniformLocation(shp, "terrainMVP")` → rename string |
| 17 | `GameOS/gameos/gos_particle_bridge.cpp:161` | `glGetUniformLocation(s_prog->shp_, "terrainMVP")` → rename string. Note: this site is the producer-side bind for `particle_billboard.vert` (the 14th vertex shader per §4.3) |
| 18 | `GameOS/gameos/gos_static_prop_batcher.cpp:549` | `s_locsLegacy.terrainMVP = ...` → rename string |
| 19 | `GameOS/gameos/gos_static_prop_batcher.cpp:581` | `s_locsCoalesce.terrainMVP = ...` → rename string |
| 20 | `GameOS/gameos/gos_static_prop_batcher.cpp:3002` | `glGetUniformLocation(s_staticPropProgram, "terrainMVP")` → rename string |
| 21 | `GameOS/gameos/gos_mech_batcher.cpp:267` | `s_loc_terrainMVP = loc("terrainMVP")` → rename string |

**Bare `mvp` lookups (3 sites — RETIRE per M3 atomic):**

| # | Site | F1 disposition |
|---|---|---|
| 22 | `GameOS/gameos/gameos_graphics.cpp:1759` | `thinTerrainLocs_.mvp = glGetUniformLocation(shp, "mvp")` — retire (covered by #12 above; duplicated for clarity) |
| 23 | `GameOS/gameos/gameos_graphics.cpp:1795` | `shadowLocs_.mvp = glGetUniformLocation(shp, "mvp")` — **KEEP** (shadow shaders use legitimate screen-pixel→NDC `mvp` for shadow pass; NOT the round-trip kludge) |
| 24 | `GameOS/gameos/gameos_graphics.cpp:3960` | `locs.mvp = glGetUniformLocation(shp, "mvp")` — retire (covered by #15) |

**Cache readers (`gos_GetTerrainMVPMat4()` callers, 15 sites):**

All of these read whatever value the cache holds. F1 changes the cache CONTENTS via the producer change at #3; cache READERS need no edits UNLESS the accessor is renamed (decision at #9). If renamed, all 15 sites update the function-call name; if kept, all 15 stay unchanged.

Sites (verified at write-time 2026-05-22): `code/objmgr.cpp:314`, `gameos_graphics.cpp:2175,2335,3029,7403,7404,7515,7516,7581,7582`, `gos_mech_batcher.cpp:1204`, `gos_particle_bridge.cpp:159` (paired with #17), `gos_static_prop_batcher.cpp:3003,3337`, `gos_terrain_indirect.cpp:1816,1965,2598`, `gpu_cull_compute.cpp:831` (the M1 CullUBO upload site).

**`setMat4Direct(...)` / `setMMat4Direct(...)` calls binding terrainMVP (2 sites — RENAME):**

| # | Site | F1 disposition |
|---|---|---|
| 25 | `GameOS/gameos/gameos_graphics.cpp:2177` | `setMat4Direct("terrainMVP", wMvpWaterNonMdi)` → rename string |
| 26 | `GameOS/gameos/gameos_graphics.cpp:2337` | `setMMat4Direct("terrainMVP", wMvpWaterMdi)` → rename string |

**CullUBO upload site (M1 resolution):**

| # | Site | F1 disposition |
|---|---|---|
| 27 | `GameOS/gameos/gpu_cull_compute.cpp:831` | `const float* mvp = gos_GetTerrainMVPMat4();` — this is the CullUBO `viewProj` write source. UBO layout at binding 2 in `gpu_cull.comp:169-170` is `layout(std140, binding = 2) uniform CullUBO { mat4 viewProj; ... };`. Migration is transparent — cache value changes; UBO contents inherit. **No edit needed at this site** unless plan-stage renames the accessor (#9). The `gpu_cull_predicate.glsl` "helper" file has NO matrix uniform; spec's earlier §4.4 classification of that file was WRONG per adversarial M1. The matrix consumer is the UBO field, not the predicate helper |

### 4.2 GOS API + uniform slot

| Component | Disposition |
|---|---|
| `gos_SetTerrainMVP` (existing) | **Retire** in atomic flip commit |
| `u_terrainMVP` / `terrainMVP` / `u_mvp` uniform slots | **Retire** atomically (single end-state) |
| `gos_SetWorldToClipGL` (new) | **Add** with GL_FALSE transpose flag (matches existing convention at `gamecam.cpp:151` region — direct-uploaded row-major matrices use GL_FALSE per worktree CLAUDE.md "Key inline rules") |
| `u_worldToClipGL` uniform slot (new) | **Add** in shader common header / per-shader uniform decl |

### 4.3 Vertex shaders (13 consumers, grep-verified write-time 2026-05-22)

Verified by `grep "uniform mat4.*terrainMVP" shaders/` at write-time:

| Shader | Role | Live? |
|---|---|---|
| `shaders/gos_terrain.tese` | tessellated terrain | Yes (perspective `terrainMVP` consumer) |
| `shaders/gos_terrain_surface.vert` | solid terrain | Yes |
| `shaders/gos_terrain_mask_solid.vert` | mask pass solid | Yes |
| `shaders/gos_terrain_mask_water.vert` | mask pass water | Yes |
| `shaders/gos_terrain_thin.vert` | thin overlay | Yes |
| `shaders/gos_terrain_water_fast.vert` | water fast path | Yes |
| `shaders/gos_terrain_water_fast_mdi.vert` | water fast MDI | Yes |
| `shaders/gos_terrain_mine_static.vert` | mines | Yes |
| `shaders/gos_grass.geom` | grass billboarding | Yes |
| `shaders/static_prop.vert` | trees, buildings | Yes |
| `shaders/terrain_overlay.vert` | terrain decal overlay | Yes |
| `shaders/gos_tex_vertex_lighted.vert` | lit static overlay (has additional matrix uniforms `mvp_`, `world_`, `view_`, `wvp_`, `projection_` — plan-write audits which are dead) | Yes |
| `shaders/mech.vert` | mechs (dynamics) | Yes |

Migration pattern per §3.

**`shaders/particle_billboard.vert:33` is INCLUDED in F1** (committed
2026-05-22 per fifth-round gemini + codex). Shader was discovered as a
14th `terrainMVP` consumer (belongs to B1 GPU particle pipeline at
`c9b6406`). Atomic retirement of `terrainMVP` requires every live consumer
migrated in the same commit; no parallel slot carve-out per the spec's
single-end-state rule.

**F1 vertex shader count: 14.** All Stage A migration lists, verification
counts, deployment manifests, grep gates, and per-shader validation tables
use 14 vert + 3 compute/frag.

B1 spec at `c9b6406` may need a minor amendment to align its particle
pipeline expectations with the new `u_worldToClipGL` consumption convention
in `particle_billboard.vert` post-F1. Plan-write surfaces any B1-side
coordination required; expected impact is minimal because B1 has not
shipped any particle render path yet.

**Excluded (no migration):**
- `shaders/shadow_*.vert`, `shaders/shadow_terrain.tese` — already use clean GL
  convention (`lightSpaceMatrix * vec4(pos, 1.0)` direct to `gl_Position`).
- HUD/GUI/screen-space: `gos_text.vert`, `postprocess.vert`, skybox, etc. —
  projection-immune; verts written directly to clip space.

### 4.4 Compute/frag shaders (4 consumers, per v1 C3) — per-site replacement semantics

Per P0.4: design commits per-site replacement decisions. No "plan-stage will
audit" — the atomic terrainMVP retirement requires concrete commitments now.

Citations grep-verified at write-time 2026-05-22; line numbers carry from v1
C3 finding and must be re-grepped during plan-write per M1 discipline (numbers
may drift between v1 spec date 2026-05-20 and current HEAD).

Per second-round reviewer P0.5: **predicate substitution semantics differ
between admission tests and full frustum culls**. The terrain/water compute
sites use a pz / depth gate, NOT a full frustum cull. Adding x/y plane tests
to those replacements would over-cull edge-of-frustum geometry that hardware
clipping would have rasterized correctly, creating edge-of-screen holes.

**Per fifth-round gemini + codex + user decision 2026-05-22: ssao runtime
path is DELETED ENTIRELY in F1 Stage A.** Grep at write-time showed
`gosPostProcess::runSSAO()` is called every frame from
`GameOS/gameos/gos_postprocess.cpp:918` (postprocess pass) — ssao is
functionally live in shipped builds, NOT deprecated as initially framed.
User chose full retire over migration: accepted visual change (AO gone)
in exchange for smallest atomic surface.

Files retired in Stage A:
- `shaders/ssao.frag` (the shader; consumer of `u_terrainMVP` retires with it)
- `GameOS/gameos/gos_postprocess.h` ssao declarations (`runSSAO()`, SSAO FBO
  members, noise texture handle)
- `GameOS/gameos/gos_postprocess.cpp`:
  - `:164` shader load entry for `shaders/ssao.frag`
  - `:346-383` SSAO FBO + blur FBO allocation
  - `:383+` 4x4 SSAO noise texture
  - `:676-714` `gosPostProcess::runSSAO()` body
  - `:918` `runSSAO()` call site in main postprocess pass
- Any related uniform-binding code (plan-write greps `runSSAO|ssao_noise|
  ssaoFBO` for full callsite list)

No `gos_SetReverseZProjCoeffs` API needed; no scalar uniforms; no
inverse-projection derivation; ssao branch retires entirely.

The remaining 3 compute/frag sites split into two replacement categories:

| Site | Current shape | Replacement category | Detail |
|---|---|---|---|
| `gpu_driven_terrain_solid.comp` :194,:214,:226,:234 | Reads `u_terrainMVP`; per-vertex `pz` / depth gate (admission test, NOT full frustum) | **(I) pz/depth-only replacement** | Replace `u_terrainMVP` with `u_worldToClipGL` for matrix source. Keep ONLY the pre-existing depth gate semantic — replace `pz ∈ [0,1)` with the ZERO_TO_ONE-equivalent `clip.w > epsilon AND 0 <= clip.z <= clip.w`. Do NOT add `x/y: -w ≤ x,y ≤ w` checks; legacy did not have them; adding them over-culls edge-of-frustum quads. Plan-write verifies "legacy did not have x/y" by grep against current `gpu_driven_terrain_solid.comp` |
| `gpu_driven_water.comp` :137,:173 | Same pz/depth admission shape as terrain_solid | **(I) pz/depth-only replacement** | Same rule as terrain_solid. If plan-stage grep finds existing x/y checks, carry them forward unchanged but in the new clip-rule form |
| `gpu_cull_predicate.glsl` (helper) | **HELPER FILE — NO MATRIX UNIFORM.** Per adversarial M1 + grep at write-time 2026-05-22: this file is a stateless predicate helper (`clipSpaceFrustumAdmit(vec4 clip)`, `clipSpaceFrustumAdmitSphere`, `clipSpaceFrustumAdmitDilated`). Takes `clip` as input parameter. No `uniform mat4` declaration. v2.x spec's earlier classification was WRONG | **n/a — no edit at this file** | The real matrix consumer is `gpu_cull.comp:169-170` UBO `layout(std140, binding = 2) uniform CullUBO { mat4 viewProj; ... };`. The UBO is written from CPU side at `gpu_cull_compute.cpp:831` (`gos_GetTerrainMVPMat4()` cache read). Cache value changes via producer migration at §4.1 #3; UBO contents inherit transparently. The predicate's existing `sign(clip.w)` normalization stays load-bearing only if clip.w can be negative for in-front verts; see §7 R-clipw |

(Note: `ssao.frag` `viewProj` / `inverseViewProj` uniforms drop with the
shader. `shadow_screen.frag:66` also has `inverseViewProj`; plan-write
verifies whether that consumer survives or is part of the ssao chain.)

### 4.5 Components left alive (out of F1 scope)

| Component | Why stays |
|---|---|
| `Camera::projectZ` body (`mclib/camera.h:459`) | Consumed by 8 wrappers, including `projectForTerrainAdmission` which is red-band-class blocker. Body uses `Camera::worldToClip` (the existing class-internal `Matrix4D` at `mclib/camera.cpp:2369`) which is **pre-axisSwap MC2 clip space**, NOT the same product as `worldToClipGL()`. Body's math operates in MC2 coords and applies its own viewport scaling downstream. Body is correct under new convention without change because the pre-axisSwap path is untouched. |
| 8 wrappers in `mclib/camera.h` (`projectForTerrainAdmission`, `projectForObjectAdmission`, `projectForEffectAdmission`, `projectForLightingShadow`, `projectForSelectionPicking`, `projectForScreenXY`, `projectForDebugOverlay`, `inverseProjectForPicking`) | Each its own modernization arc. Track A1 modernized object admission. Others stay legacy. |
| CPU `pz` gate in `mclib/quad.cpp` (5 `projectForTerrainAdmission` callsites: `:1070,:1115,:1160,:1205,:2114`) | Red-band class blocker. Retirement deferred to follow-up slice after empirical post-F1 red-band verification. |
| `mclib/projectz_trace.{h,cpp}`, `mclib/projectz_overlay.{h,cpp}`, `mclib/cpu_proj_cost_split.{h,cpp}`, `mclib/object_admission_predicate.{h,cpp}` | Instrumentation tied to live `projectZ` body. Retires with body in follow-up. |
| `tests/unit/test_projection.cpp` | Tests legacy projectZ contract; retires with body in follow-up. |
| `mclib/mlr/mlrclipper.cpp:121,209,305,321,347` (MLR clipper composition + reads of `cameraToClip(2,2),(3,2)`) | Mitigation (c). MLR's reads are gate-dead under default `MC2_DISABLE_GOSFX=1`; stale convention reads harmless. |

## 5. Stages

### 5.1 Stage A-pre — parity probe (gated artifact, deleted before atomic flip)

**Stage A-pre commit artifact contents (explicit, per P0.3 + reviewer 3):**

The probe is NOT just shader scaffold; it requires producer + GOS API +
uniform + binding alive **concurrent with the old upload path**:

- `Camera::worldToClipGL()` body + header decl (`mclib/camera.cpp` + `.h`)
- `axisSwap_MC2toGL` constant (literal per §2.1)
- `gos_SetWorldToClipGL()` GOS API (declaration + body that calls
  `glUniformMatrix4fv` with `GL_FALSE`)
- `u_worldToClipGL` uniform slot registered in the shared shader uniform
  block / per-shader uniform decl (only in `gos_terrain.tese` for probe;
  other shaders unchanged until Stage A)
- Producer call additions at `code/gamecam.cpp:149-151` region and
  `code/simplecamera.cpp:168-169` region (uploads `worldToClipGL()` result
  via new GOS API every frame, adjacent to existing `gos_SetTerrainMVP`
  calls)
- `gos_SetTerrainMVP` + `u_terrainMVP` + `terrainMVP` + `u_mvp` all remain
  active and authoritative — every shader except `gos_terrain.tese` keeps
  reading legacy slot
- Probe-only dual-consumer modification to `gos_terrain.tese` (see code
  below)
- CPU-side per-frame readback + counter print
- Build flag `-DMC2_UNIFIED_PROJECTION_PARITY_PROBE` gates probe shader
  branch + CPU readback print

After Stage A-pre passes, Stage A commit deletes probe scaffold + retires
`terrainMVP` slot + migrates remaining 13 vertex shaders (the 14 vert list
in §4.3 minus the probe-host `gos_terrain.tese` which migrated in A-pre) + 3 compute/frag
sites.

**Probe shader (`shaders/gos_terrain.tese` — per second-round reviewer P0.1 +
P0.4 fixes):**

CRITICAL: the probe must wrap the **exact existing legacy gl_Position
computation**, not a synthesized model. Current legacy path at
`shaders/gos_terrain.tese:126-141` (write-time verified 2026-05-22):

```glsl
// EXACT legacy path — DO NOT re-derive, copy literally:
vec4 clip = terrainMVP * vec4(worldPos, 1.0);
float rhw = 1.0 / clip.w;
vec3 screen;
// ... screen.x/y from terrainViewport ...
screen.z = clip.z * rhw + TERRAIN_DEPTH_FUDGE;
vec4 ndc = mvp * vec4(screen, 1.0);
float absW = abs(clip.w);
vec4 legacyGlPosition = vec4(ndc.xyz * absW, absW);
```

Plan-write copies the actual file contents verbatim into the probe-wrap;
this spec does not re-state the exact lines. The synthesized `oldKludge =
vec4(clip.xy, clip.z, abs(clip.w))` from v2.1 was WRONG — it didn't model
the screen→pixel→NDC round-trip via `mvp * vec4(screen,1)`.

**Mechanical equivalence check** (per fourth-round reviewer P2):
plan-write commit lands a one-shot grep/diff that confirms
`computeLegacyGlPosition()`'s body is byte-equivalent to the pre-F1
`shaders/gos_terrain.tese:126-141` block. Specifically: extract the
post-`worldPos`-computation gl_Position-emitting lines from the pre-F1
file (git-blame anchored or by line range), extract the function body
from the probe shader, diff. Stage A-pre PR description carries the
diff output as evidence.

Probe wrapper:

```glsl
// SSBO declaration (per Vulkan-review §3: scaffolding still needs explicit
// layout + binding slot per Vulkan-prep cpp_glsl_ubo_struct_lockstep
// discipline). Plan-stage picks a free binding slot N (suggested: 7 or
// next-free per current binding-slot inventory) and documents it.
layout(std430, binding = N) buffer DebugSSBO {
    uint debugSSBO_counters[8];
};
// CPU side: glGenBuffers + glBindBufferBase(GL_SHADER_STORAGE_BUFFER, N, ssbo)
// at A-pre startup; clear counters at mission start per §5.1 lifecycle below;
// glDeleteBuffers + binding-slot freed in Stage A commit when probe retires.

// computeLegacyGlPosition() literally encapsulates the
// shaders/gos_terrain.tese:126-141 block above (plan-write splice).
vec4 legacyGlPosition = computeLegacyGlPosition(worldPos, undisplacedWorldPos);

#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE
    vec4 newClip = u_worldToClipGL * vec4(worldPos, 1.0);

    // epsilon discussion (per fifth-round gemini #1): MC2's perspective build
    // has near_clip > 0 (see camera.cpp:2129+ setPerspective). clip.w == z_eye
    // for in-front geometry, so clip.w values for in-front fall in
    // [near_clip, far_clip] world units (typical near_clip ~5-20 units;
    // far_clip ~ 1500-8000 units depending on mission). 1e-4 << near_clip
    // for any sane camera setup, so the gate correctly classifies "behind
    // or at camera plane" without flickering on near-plane verts. Plan-
    // write may bump to 1e-3 if AMD shader-review surfaces ULP drift, but
    // empirically 1e-4 is safe across observed camera setups.
    const float epsilon = 1e-4;
    bool oldBehind = (legacyGlPosition.w <= epsilon);
    bool newBehind = (newClip.w <= epsilon);

    // Compute NDC guarded against divide-by-zero. Values irrelevant for
    // behind-camera verts; behind-camera classification dominates.
    vec3 oldNDC = oldBehind ? vec3(0.0) : (legacyGlPosition.xyz / legacyGlPosition.w);
    vec3 newNDC = newBehind ? vec3(0.0) : (newClip.xyz       / newClip.w);

    // Per fifth-round gemini #4: isolate the math-hazard signal from the
    // behind-camera signal. "Hazard" means in-front-of-camera but NDC is
    // NaN/Inf — a real math bug. Using `hazard` (not `!finite`) avoids
    // classifying behind-camera verts as non-finite false-positives.
    // P2 GLSL-portability: avoid isfinite() (not in all GLSL versions); use
    // explicit !isnan && !isinf checks. Validate on NDC, not clip.
    bool oldHazard = !oldBehind && (any(isnan(oldNDC)) || any(isinf(oldNDC)));
    bool newHazard = !newBehind && (any(isnan(newNDC)) || any(isinf(newNDC)));
    bool comparable = !oldBehind && !newBehind && !oldHazard && !newHazard;

    if (comparable) {
        float d = max(abs(oldNDC.x - newNDC.x),
                  max(abs(oldNDC.y - newNDC.y),
                      abs(oldNDC.z - newNDC.z)));
        // Per fifth-round gemini #3: un-nest the bitcast inside atomicMax
        // to avoid AMD GLSL compiler miscompile of nested built-ins under
        // aggressive optimization. Cast to local uint first; then atomicMax.
        uint dBits = floatBitsToUint(d);
        atomicMax(debugSSBO_counters[0], dBits);                // max_delta_comparable
        atomicAdd(debugSSBO_counters[1], 1u);                   // count_compared
    } else {
        // Non-overlapping bucketing. Behind-camera classified first so a
        // vertex behind both cameras is NOT also counted as nonfinite
        // (fourth-round P1.8 + fifth-round gemini #4 over-block fix).
        if (oldBehind && newBehind) {
            atomicAdd(debugSSBO_counters[5], 1u);               // count_behind_both
        } else if (oldBehind) {
            atomicAdd(debugSSBO_counters[2], 1u);               // count_behind_old_only
        } else if (newBehind) {
            atomicAdd(debugSSBO_counters[3], 1u);               // count_behind_new_only
        } else {
            // Neither behind. At least one in-front vertex has nan/inf
            // NDC — a real math hazard. Bucket old-only/new-only/both so
            // pre-existing legacy debt (old-only) doesn't block flip.
            if (oldHazard && newHazard) {
                atomicAdd(debugSSBO_counters[6], 1u);           // count_nonfinite_both
            } else if (oldHazard) {
                atomicAdd(debugSSBO_counters[4], 1u);           // count_nonfinite_old_only
            } else /* newHazard */ {
                atomicAdd(debugSSBO_counters[7], 1u);           // count_nonfinite_new_only
            }
        }
    }
#endif

// CRITICAL per second-round reviewer P0.1: BOTH branches output legacy.
// Stage A-pre changes ZERO production behavior. A build without
// MC2_UNIFIED_PROJECTION_PARITY_PROBE behaves byte-identical to pre-F1.
gl_Position = legacyGlPosition;
```

**SSBO lifecycle (per fifth-round codex P1.4):**

- Clear `debugSSBO_counters[0..7]` to zero at mission start (`Mission::load`
  init mirror site or equivalent first-frame hook).
- Accumulate `max` and `count` values across the full mission run; do NOT
  reset per-frame (per-frame reset hides intermittent spikes).
- CPU side reads `debugSSBO_counters[0..7]` once per frame, prints a
  periodic snapshot every 60 frames (matches typical Tracy zone granularity)
  and a final summary at mission end.
- Stage A-pre gate (§6.1) evaluates the **final summary** values, not
  per-frame snapshots.

Snapshot/summary format:

```
[UNIFIED_PROJ_PARITY v1] max_delta_comparable=X.XXX compared=N
    behind_old_only=N behind_new_only=N behind_both=N
    nonfinite_old_only=N nonfinite_new_only=N nonfinite_both=N
```

to stderr (one line; line-wrapped here for readability). `floatBitsToUint`
chosen so `atomicMax` compares positive finite delta values bit-
monotonically (valid because deltas are guaranteed non-negative).

**`gos_SetWorldToClipGL` location-binding behavior (per fifth-round codex P1.5 + Vulkan-review §2):**

During Stage A-pre, only `gos_terrain.tese` declares `u_worldToClipGL`. Other
13 vertex shaders + 3 compute/frag still read legacy `terrainMVP`. The
uniform setter MUST tolerate "uniform not declared" on programs that haven't
migrated yet:

- `gos_SetWorldToClipGL(prog, mat)` resolves `u_worldToClipGL` location on
  the EXPLICITLY-PASSED program; if `-1`, upload silently no-ops (NOT fatal
  during A-pre).
- After Stage A flip, every migrated shader declares `u_worldToClipGL`. The
  silent-no-op behavior **retires as a hard gate** at Stage A (per Vulkan
  review §2: "make it a gate, not a footnote"). Specifically: Stage A
  commit replaces the `if (loc == -1) return;` line with `MC2_ASSERT(loc != -1, "u_worldToClipGL missing from program <name>");` OR moves the location resolution to a startup-time validation that hard-fails on any migrated program with missing uniform. Plan-stage picks; both are Vulkan-portable patterns.
- §6.2.2 per-shader post-flip validation enforces this via the per-program
  uniform-location probe at startup.

**Stage A-pre gate criterion (P0.3 + second-round reviewer 4):**
- `tier1 5/5` stock with probe build: `max_delta_comparable < 1e-3 NDC` across
  all 5 missions, 30s each.
- User-driven `mc2_10` wolfman zoom 60s: `max_delta_comparable < 1e-3 NDC`.
- `count_compared > 0` (probe actually firing, not all behind-camera/non-finite).
- `count_behind_old_only`, `count_behind_new_only`, `count_behind_both` MAY
  each be non-zero — that's the expected semantic change. NOT failure
  conditions. Plan-stage reviews the magnitudes for sanity.
- `count_nonfinite_new_only == 0` AND `count_nonfinite_both == 0`. New-path
  non-finite is a real math hazard and blocks the flip. `count_nonfinite_old_only`
  is logged but does NOT block — it represents pre-existing legacy debt
  unrelated to F1 (fourth-round P1.8 over-block fix).
- If `max_delta_comparable >= 1e-3` OR `count_nonfinite_new_only > 0` OR
  `count_nonfinite_both > 0`: math wrong; do NOT flip; debug
  `Camera::worldToClipGL()` body, axisSwap literal, or upstream matrix.

Stage A-pre is observation-only. Both `#ifdef`-branch and `#else`-fallback
emit legacy `gl_Position` (per second-round reviewer P0.1). Production GPU
output drives via legacy `terrainMVP` regardless of probe build flag. Once
gate passes, parity scaffold deleted, Stage A commit lands and migrates
`gos_terrain.tese` to clean `u_worldToClipGL` consumption along with all
other shaders.

### 5.2 Stage A — atomic flip commit

Producer + GOS API + uniform slot were added in Stage A-pre (alive
concurrently with legacy `terrainMVP`). Stage A is the **consumer migration
+ legacy retirement** commit.

Single commit lands:
- Delete Stage A-pre parity probe scaffold from `gos_terrain.tese`
- Migrate `gos_terrain.tese` from probe-mode (dual-consumer) to clean
  `gl_Position = u_worldToClipGL * vec4(world, 1.0)`
- Migrate remaining 13 vertex shaders to new pattern (full 14-shader list in §4.3 minus `gos_terrain.tese` which migrated in A-pre)
- **Per M3 atomic retire (NEW in v2.7):** delete `uniform mat4 mvp;` + `uniform vec4 terrainViewport;` from the 9+ terrain shaders that participated in the round-trip kludge (NOT shadow/HUD shaders — see §6.2.1 allowlist); delete screen.xyz / rhw math; delete `gl_Position = vec4(ndc.xyz * absW, absW)` packaging — all retired by the clean `gl_Position = u_worldToClipGL * vec4(world,1)` pattern
- Migrate 10 `glGetUniformLocation(_, "terrainMVP")` lookups to `"u_worldToClipGL"` per §4.1 #10-#21 census
- Retire 3 bare `mvp` glGetUniformLocation lookups in terrain set per §4.1 #12,#15 (shadow site #23 stays)
- Migrate 2 `setMat4Direct("terrainMVP", ...)` / `setMMat4Direct("terrainMVP", ...)` strings per §4.1 #25-#26
- CullUBO at `gpu_cull.comp:169-170` inherits via cache (no per-site edit needed; cache-reader at `gpu_cull_compute.cpp:831` per §4.1 #27)
- Migrate 3 compute/frag sites per §4.4 per-site replacement table
- Resolve `ssao.frag` per §4.4 path (a) delete OR path (b) carve-out
- Retire `gos_SetTerrainMVP` API + `u_terrainMVP`/`terrainMVP`/`u_mvp` uniform
  slots in same commit
- Retire `Camera::worldToClip` upload path (the upload route, not the
  internal `Camera::worldToClip` member which stays for `projectZ` body)
- Add CI script `scripts/check-unified-projection-retirement.sh` (per §6.2.1)
- Add runtime guard for `MC2_DISABLE_GOSFX=0` (per §10.2)
- Add `CLAUDE.md` "Known issues (current)" entry (per §10.1)

No env flag in production code. Rollback = `git revert <commit>` for Stage A,
plus a separate revert of Stage A-pre if rollback discipline requires it.

Reviewable as single diff. Per v1 POSTSCRIPT M4: Stage A ~12-16 files,
~300-800 LOC (lower than v1's 600-1500 because CPU pz gate + `Camera::projectZ`
body + wrappers stay). Stage A-pre is smaller, ~4-6 files (producer + GOS API
+ uniform + probe scaffold + CPU readback).

### 5.3 Stage B — cleanup (deferred, not part of F1 atomic)

Retired to follow-up slices, listed in §8 out-of-scope.

## 6. Verification

### 6.1 Pre-flip gates (Stage A-pre, probe phase)

| Gate | Pass criterion |
|---|---|
| `tier1 5/5` stock with `MC2_UNIFIED_PROJECTION_PARITY_PROBE` build | `max_delta_comparable < 1e-3 NDC` all 5 missions, 30s each; `count_compared > 0`; `count_nonfinite_new_only == 0`; `count_nonfinite_both == 0`. `count_nonfinite_old_only` logged but does not block. Standard `py scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs` |
| User-driven `mc2_10` wolfman zoom 60s with probe build | `max_delta_comparable < 1e-3 NDC`; `count_compared > 0`; `count_nonfinite_new_only == 0`; `count_nonfinite_both == 0` |
| Build clean | `--config RelWithDebInfo`, `--clean-first` per uniform-slot retire / GOS-API change |
| `MC2_GL_DEBUG_FATAL=1` | Clean across tier1 |

### 6.2 Post-flip gates (Stage A, the atomic commit)

| Gate | Pass criterion |
|---|---|
| `tier1 5/5` stock with parity probe deleted | Exit 0; no new visual regressions vs pre-F1 baseline |
| `/mc2-amd-shader-review` skill dispatched | Mandatory pre-deploy. Validates: (a) reverse-Z + ZERO_TO_ONE + ClipControl + cleaned shader interaction on AMD; (b) PER FIFTH-ROUND GEMINI #3: nested `floatBitsToUint` inside `atomicMax` is un-nested via local `uint` variable in the parity probe — verify AMD GLSL compiler does NOT drop the atomic write or corrupt the bitcast; (c) `glUniformMatrix4fv(..., GL_FALSE, ...)` upload produces the GLSL-visible matrix orientation expected by all 14 migrated vert shaders (test on AMD 7900 XTX target hardware) |
| User-driven `mc2_10` wolfman zoom 60s — red-band canary | Empirical: do red-diagonal-band terrain artifacts appear at extreme zoom-out with mechs below+behind camera? Pre-F1: caught by CPU pz gate. Post-F1: CPU pz gate still alive, so should remain caught. If bands appear, regression vs current behavior |
| Screen-spanning-triangle canary (`mc2_10`, mechs in worst-case pose transitions) | User-visual observation. If triangles disappear, F1 retired that class. If persist, separate dynamic-pose issue |
| Full relink discipline | `rm build64/RelWithDebInfo/mc2.exe` + changed .obj before build per `feedback_class_layout_change_needs_clean_first.md` |
| Shader/exe lockstep deploy | All 14 vert + 3 compute/frag shaders deploy with `mc2.exe` per `shader_exe_deploy_lockstep.md` |

### 6.2.1 Mechanical retirement gates (per P1 reviewer 8 — uniform-slot retirement)

After Stage A commit, the following greps MUST produce zero hits (or land on an
explicit allowlist, e.g., archived spec docs):

```
# Uniform name retirement (post-rename: terrainMVP → u_worldToClipGL):
grep -RE "terrainMVP|u_terrainMVP|gos_SetTerrainMVP" code GameOS mclib shaders

# Bare-mvp retirement (per M3 atomic) — bare `mvp` retired in terrain set only;
# shadow shaders legitimately keep their `mvp` (screen-pixel→NDC, NOT the
# round-trip kludge). Allowlist entries name the shadow set explicitly.
grep -RE "\buniform[[:space:]]+mat4[[:space:]]+u?_?mvp\b" shaders          # bare mvp uniform decl
grep -RE "u_mvp\b" code GameOS mclib shaders                              # u_mvp legacy slot

# Round-trip kludge retirement (per M3 atomic):
grep -RE "abs\(\s*[a-zA-Z_]+\.w\s*\)" shaders                              # legacy abs(clip.w) kludge
grep -RE "terrainViewport|u_terrainViewport" code GameOS mclib shaders     # screen-rhw multiplier
grep -RE "\*\s*rhw\s*[\+\-\*]"             shaders                          # screen.x/y = clip.* * rhw * ...
grep -E "pixelHomog|screenToPixel|pixelToNDC"  shaders                     # legacy round-trip helpers
```

CI script `scripts/check-unified-projection-retirement.sh` lands in Stage A
commit; runs all three greps; fails build on unexpected hit.

**Allowlist mechanism** (per fourth-round reviewer P2 + fifth-round codex P2):
the script reads `scripts/check-unified-projection-retirement.allowlist` —
newline-delimited list of file paths or extended-regex line-anchored exclusion
patterns (format: `path:linepattern`). Each allowlisted hit must include a
`# UNIFIED_PROJ_ALLOWLIST: <reason>` comment within 2 lines of the match in
the source file.

Grep scope is `code GameOS mclib shaders` (matches above command); allowlist
entries OUTSIDE that scope (e.g., docs/, memory/) are not needed and removed
from the initial list to keep allowlist and grep scope aligned. Initial
allowlist contents (per adversarial M3 + atomic bare-mvp retire):

- `mclib/mlr/**` (mitigation (c) — MLR's `cameraToClip` / `worldToClipMatrix`
  composition stays alive in F1)
- **Bare `mvp` legitimate consumers (HUD + shadow set):**
  - `shaders/gos_text.vert:8` (HUD text screen-pixel→NDC; not round-trip)
  - `shaders/gos_vertex.vert:8` (HUD)
  - `shaders/gos_vertex_lighted.vert:8` (HUD)
  - `shaders/gos_tex_vertex.vert:8` (HUD)
  - `shaders/gos_tex_vertex_lighted.vert:20` (`mvp_` with underscore;
    HUD-style usage — plan-write verifies it's NOT the round-trip)
  - `shaders/shadow_terrain.vert:8` (shadow screen-space fallback per
    comment; legitimate)
  - `shaders/particle_billboard.vert:43` (`u_mvp` for particle billboarding
    screen-space; plan-write verifies it's NOT the round-trip kludge —
    `particle_billboard` is in the migration set, but its `u_mvp` may
    legitimately persist for billboard math separate from world-space
    projection)
  - `GameOS/gameos/gameos_graphics.cpp:1795` (shadowLocs_.mvp lookup;
    paired with shadow_terrain.vert)
- Any shader file with `abs(...)` used in NON-projection math (e.g.,
  abs(normal.w) for normal length; abs(uv.w) for UV cycle). Plan-write
  audits each hit against the allowlist criterion "is this projection-
  related?" before permitting the entry.

### 6.2.1.5 Compute-culling parity scope (per second-round reviewer P1.10)

The Stage A-pre parity probe (§5.1) covers `gos_terrain.tese` per-vertex
output only. It does NOT validate the 3 compute/frag migrations
(terrain_solid + water admission, instance frustum cull). Compute-culling
correctness is validated by:

- **Tier1 5/5 + user-driven `mc2_10` wolfman zoom canary post-Stage-A** —
  any over-cull manifests as missing terrain/water quads, edge-of-screen
  holes, or popping at frustum edges. User-visual gate.
- **Golden-frame capture (§6.4)** once infra is settle-disciplined — durable
  regression gate covering all consumers, including compute.
- **Per-site plan-write grep** confirms each compute site's legacy
  predicate shape (pz/depth-only vs full-frustum-cull) matches the §4.4
  category assumed for its replacement. If grep finds a mismatch, plan-
  stage updates §4.4 before splicing the replacement.

Plan-stage MAY add a Stage A-pre-equivalent dual-run counter inside a
compute shader if a particular site warrants pre-flip empirical validation
(e.g., if its legacy predicate is unusually complex). Default = visual +
golden-frame post-flip is sufficient.

### 6.2.2 Per-shader post-flip validation (per P1 reviewer 9)

After Stage A commit, every migrated shader (14 vert + 3 compute/frag) MUST:

| Validation | Method |
|---|---|
| Compile + link cleanly | Standard build. AMD driver hot-reload silent-fail trap per worktree CLAUDE.md — check stderr after build |
| `u_worldToClipGL` uniform bound | Either: link-time fatal on unresolved uniform (preferred — fail loud), OR runtime startup probe that calls `glGetUniformLocation` per migrated shader and aborts on `-1` |
| No `terrainMVP` / `u_terrainMVP` / `u_mvp` references | §6.2.1 grep catches |
| No `abs(clip.w)` projection kludge | §6.2.1 grep catches |
| Visual smoke on first-frame render | Tier1 5/5 covers; AMD shader review confirms |

Probe scaffold deleted from `gos_terrain.tese` (§5.1 was the only shader
carrying it).

### 6.3 Adversarial review (mandatory pre-plan-write)

Per worktree CLAUDE.md discipline:
- Dispatch `adversarial-plan-review` skill on this design. Dispatch prompt MUST
  include "use the adversarial-plan-review skill" verbatim.
- Dispatch `greybeard` skill on this design. Dispatch prompt MUST include
  "run the greybeard skill" verbatim.
- Both in parallel.

### 6.4 Golden-frame regression gate (preferred verification path)

Golden-frame test infrastructure exists and is usable for F1, with two
disciplines required for reliable captures:

- **Disable cursor drift.** Cursor-driven camera nudges between captures
  produce frame-to-frame deltas unrelated to the change under test.
  Captures with cursor active are not comparable.
- **Wait for camera settle (`isArmed` / equivalent).** Capture only after the
  camera state has stabilized post-mission-load and post-input-quiesce.
  Pre-settle captures show transient state and produce false-positive deltas.

F1 verification:
- Capture golden-frame baseline pre-Stage-A on `claude/nifty-mendeleev @ c9b6406`
  (or whichever commit is immediately pre-F1) across `tier1` 5/5 missions.
- Post-Stage-A, re-capture under identical settle discipline.
- Delta within float-tolerance bounds (TBD per golden-frame infra's compare
  step; plan-stage commits the tolerance) = pass.
- Any non-tolerance delta = visual regression; investigate before declaring
  Stage A stable.

Golden-frame supersedes the user-visual canaries in §6.2 once captures pass
the settle discipline above. Tier1 + user-driven canaries remain as
complementary gates.

## 7. Risk surface

| # | Risk | Mitigation |
|---|---|---|
| R1 | `ddc173f` recurrence (delete CPU pz gate without matrix fix). | N/A — F1 does not retire CPU pz gate. Stays alive in `mclib/quad.cpp`. |
| R2 | Mixed-state during atomic flip. | Single commit, no production env flag, no half-flipped state ships. Rollback = `git revert`. |
| R3 | Parity probe fails (`max_delta_comparable >= 1e-3 NDC` OR `count_nonfinite_new_only > 0` OR `count_nonfinite_both > 0`) → math wrong in `Camera::worldToClipGL()` body, axisSwap literal, or upload transpose convention. | Probe gates flip. Do not ship Stage A until Stage A-pre passes per §5.1 + §6.1 criteria. |
| R4 | Red-diagonal-band class still present post-F1. | F1 keeps CPU pz gate alive in `mclib/quad.cpp`. Red-band stays caught. F1 success criterion = no NEW regressions, not red-band retired. |
| R5 | Screen-spanning-triangle class persists. | User-visual canary on `mc2_10` wolfman zoom. If persists, separate dynamic-pose issue (out of F1 scope). |
| R6 | `MC2_DISABLE_GOSFX=0` dev override renders gosFX wrong (MLR reads stale convention). | Mitigation (c) accepted. Triple-layer guard per §10: (1) `CLAUDE.md` "Known issues" entry, (2) runtime stderr warning once per startup, (3) smoke-script env assert blocking tier1 runs under the override |
| R7 | AMD driver quirk under new shader edits (`glClipControl` + reverse-Z + cleaned shaders interaction). | `/mc2-amd-shader-review` skill mandatory pre-deploy. |
| R8 | Editor worktree breakage. Editor builds own forked `terrainMVP`. | Out of F1 scope. Editor lives in separate worktree. Follow-on convergence slice per `feedback_editor_must_converge_with_runtime_paths.md` debt. |
| R9 | Compute migration over-culls edge-of-frustum geometry or under-replaces a real predicate. | §4.4 commits per-site replacement category at design level (pz/depth-only vs full clean-GL frustum cull, post-ssao-drop). Plan-write greps each current site to confirm legacy predicate shape matches assumed category before splicing the replacement. If grep shows a site with x/y checks that §4.4 categorized as pz-only, plan-write surfaces the discrepancy and revises the spec, not the replacement |
| R10 | TGL class-statics `s_cameraToClip` / `s_worldToClip` (kept alive in F1) become stale relative to new convention if some future TGL CPU consumer reads them. | F3 measurement says `tgl_transform = 0us`. Static-update-skip retires the per-actor CPU consumers. No live TGL CPU read of these statics in stock content. If a downstream slice surfaces a live consumer, retire those statics in same arc. |
| R-clipw | `gpu_cull_predicate.glsl:9-12` comment claims "MC2's Stuff worldToClip matrix produces clip.w of EITHER sign for visible vertices." If empirically true post-F1, deleting the legacy `abs(clip.w)` round-trip kludge would cause visible-but-clip.w-negative verts to be GPU-clip-rejected (visual regression). v1 POSTSCRIPT reconciliation prose argued mixed-sign is a round-trip artifact, not the matrix itself; F1 trusts that reading. | **Parity probe `count_behind_new_only` is the empirical falsifier.** Tightened gate interpretation (per fold-time analysis): `count_behind_new_only` from `gos_terrain.tese` MUST be near-zero on tier1 + wolfman zoom probe runs. The probe's behind-camera classification correctly counts vertices the legacy `abs(clip.w)` path would have rendered but new path drops. Non-zero count from visible terrain = v1 reconciliation was wrong, F1 must revisit the matrix before flip. Plan-stage adds an explicit sub-gate: "if `count_behind_new_only / count_compared > 0.001` AND visual canary shows missing terrain on wolfman zoom → BLOCK Stage A flip; matrix model needs revision before atomic ship." |

## 8. Out of scope (deferred to follow-up slices)

| Item | Why deferred | Owning slice |
|---|---|---|
| CPU `pz` gate retire in `mclib/quad.cpp` (5 sites) | Empirical red-band verification post-F1 needed first | Post-F1 slice 1 |
| `projectForTerrainAdmission` modernization | Red-band class predicate research | Independent arc |
| `projectForEffectAdmission` retire / modernize | Depends on weather + clouds + crater retirement decisions | Independent arc (or Track A2 plan at `docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md`) |
| Weather + clouds + crater rendering retirement | User-content cleanup; orthogonal | User-decision slice |
| Remaining 6 wrapper modernizations (`projectForLightingShadow`, `projectForSelectionPicking`, `projectForScreenXY`, `projectForDebugOverlay`, `inverseProjectForPicking`, plus the two above) | Each its own arc | Per-wrapper slices |
| `Camera::projectZ` body retire | Blocked on all 8 wrappers retiring | Post-wrapper-retirement cleanup |
| `projectz_trace`, `projectz_overlay`, `cpu_proj_cost_split`, `object_admission_predicate` instrumentation retire | Tied to live `projectZ` body | Post-`projectZ`-retire cleanup |
| `tests/unit/test_projection.cpp` retire / rewrite | Tied to live `projectZ` API | Post-`projectZ`-retire cleanup |
| MLR retirement (Slices 1-5 per pivot memory) | Mitigation (c). Independent | MLR-retirement arc |
| Editor worktree convergence | Editor lives in separate worktree | Editor-convergence slice |
| TGL CPU vertex transform retire (F2 framing from earlier session) | A2 + static-update-skip retired most CPU transforms; `tgl_transform = 0us`. No urgency | Deferred indefinitely; revisit if F3 ever shows non-zero TGL bucket |

## 9. M3 META-FIX delivered for the runtime GPU uniform path (greybeard G5 promotion trigger partially satisfied)

v1 POSTSCRIPT named M3 as a deferred META-FIX with promotion trigger:

> M3: retire projection-matrix-per-renderer pattern via shared
> `Camera::worldToClip()` accessor (currently 5+ independent composition sites).

**F1 delivers M3 for the runtime shader uniform producer path only.** After
Stage A atomic flip, the GPU-bound projection composition that feeds shaders
exists at exactly one site: `Camera::worldToClipGL()`. The 4 dup upload-site
compositions that previously fed `gos_SetTerrainMVP` collapse to this single
source.

**F1 does NOT retire all projection composition sites globally.** These
documented exceptions remain alive after F1:

- `Camera::worldToClip` at `mclib/camera.cpp:2369` — pre-axisSwap composition
  for `projectZ` body + 8 wrappers. Different product per P0.1; retires when
  wrappers + body retire in their own arcs.
- `TG_Shape::s_worldToClip` class-static at `mclib/tgl.cpp:1624` — TGL CPU-side
  composition for legacy mech transform consumers. `tgl_transform = 0us` per
  F3 data means consumers are dormant in stock content; static stays alive
  but unread. Retires alongside any future TGL CPU consumer revival OR
  unconditional cleanup slice.
- `MLRClipper::worldToClipMatrix` at `mclib/mlr/mlrclipper.cpp:321` —
  mitigation (c). MLR's composition stays for `MC2_DISABLE_GOSFX=0` dev
  override correctness (with the documented broken-under-unified-projection
  caveat). Retires with MLR retirement Slices 1-5.

**Promotion trigger:** PARTIALLY satisfied. Global M3 (all composition sites)
remains a deferred META-FIX whose remaining surface retires alongside `projectZ`
+ wrappers + body retirement arc.

**M2 (named deferred META-FIX, NOT delivered by F1):** retire Stuff from
camera path entirely (route Camera through glm). Promotion trigger: promote
to active when gosFX/MLR/editor matrix-convention bug recurs OR when a
future slice surfaces an empirical need to swap perspective math. Until
then, hand-built `cameraToClip` at `mclib/camera.cpp:2129-2151` is correct
and stays.

## 10. Mitigation (c) — documented regression + runtime guard for `MC2_DISABLE_GOSFX=0`

Per P1 reviewer 7: docs alone are weak. Add BOTH the doc entry AND a runtime
guard so dev-override users get a loud signal, not silent corruption.

### 10.1 Worktree `CLAUDE.md` "Known issues (current)" entry

Add in same atomic commit as Stage A:

> **gosFX dev-override broken under unified projection.** Running with
> `MC2_DISABLE_GOSFX=0` after F1 ship will render gosFX particles wrong:
> MLR's `mlrclipper.cpp:206-209,305,321,347` reads of `cameraToClip(2,2)`
> and `(3,2)` use stale MC2-pixel-homogeneous convention while the runtime
> uniforms drive shaders via the new GL convention. Default `MC2_DISABLE_GOSFX=1`
> (gate-dead MLR work-leaves) is unaffected. Dev-override path will be
> re-enabled when MLR retirement Slices 1-5 ship or when the dev-override
> use case demands a fix. **Runtime guard prints stderr warning once per
> startup; see §10.2.**

### 10.2 Runtime guard

Add once-per-startup stderr warning in `GameOS/gameos/gameosmain.cpp` near
existing env-var probe sites:

```cpp
if (getenv("MC2_DISABLE_GOSFX") != nullptr &&
    strcmp(getenv("MC2_DISABLE_GOSFX"), "0") == 0)
{
    fprintf(stderr,
        "[UNIFIED_PROJ v1] WARN: MC2_DISABLE_GOSFX=0 active under unified "
        "projection. gosFX particles will render incorrectly (MLR clipper "
        "uses stale convention). See CLAUDE.md Known Issues.\n");
}
```

Not fatal: dev users may legitimately want gosFX live for non-projection
debugging.

### 10.3 CI/smoke discipline

`scripts/run_smoke.py` already enforces default env (per env-allowlist memory).
Add explicit assert that `MC2_DISABLE_GOSFX` is unset OR `=1` in tier1 smoke
gates. Prevents accidental tier1 runs under the broken override.

## 11. References

### v1 spec and predecessor docs
- `docs/superpowers/specs/2026-05-20-unified-projection-meta-fix-design.md`
  (v1 DRAFT; this v2 supersedes)
- `docs/observations/2026-05-14-render-post-fix-b-retirement-chain.md`
  (8-wrapper inventory, `projectZ` deprecation status)
- `docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md`
  (Track A2 plan, unimplemented; may retire under weather+clouds+crater removal)
- `docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md`
  (F3 instrumentation context)

### Pivot memory and supporting evidence
- `memory/mlr_does_not_block_unified_projection.md` (framing pivot)
- `memory/static_update_skip_already_retired_per_actor_projection.md` (F3
  data; CPU budget already met)
- `memory/track_a1_object_admission_predicate.md` (A1 ship; dual-output
  wrapper discipline pattern)
- `memory/policy_split_wrapper_grep_trap.md` (8-wrapper inventory; effect
  admission ~950/frame attribution)
- `memory/projectz_overlay_findings.md` (red-band class; terrain admission
  must stay legacy until predicate research)
- `memory/vertexproject_loop_asymptotic.md` (VPL retired, slimReduce live)
- `memory/clip_w_sign_trap.md` (post-round-trip clip.w sign destruction;
  reconciled in v1 POSTSCRIPT prose)
- `memory/terrain_tes_projection.md` (terrain projection chain reference)

### Worktree CLAUDE.md rules folded into F1 discipline
- "No emoji in any file, ever"
- "No wall-clock time projections"
- "Grep before citing file:line" — every cite in this design grep-verified
  2026-05-22
- "Negative claims need opposite-direction grep" — pivot memory provides
- "Build: ALWAYS `--config RelWithDebInfo`"
- "Full relink before deploy when load-bearing functions change"
- "Deploy: NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q`"
- "Shaders deploy in lockstep with exe"
- "GL_FALSE for terrainMVP" / direct-uploaded row-major matrices — applies to
  `gos_SetWorldToClipGL` upload too

### Verified composition sites at write-time 2026-05-22
- `mclib/camera.cpp:2073-2093` (orthographic `cameraToClip` build)
- `mclib/camera.cpp:2129-2151` (perspective `cameraToClip` build, reverse-Z)
- `mclib/camera.cpp:2369` (existing `Camera::worldToClip.Multiply` site)
- `mclib/camera.cpp:1737` (`TG_Shape::SetCameraMatrices` pipe site)
- `mclib/tgl.cpp:53,1620,1624` (TGL class-statics + composition)
- `mclib/tgl.h:792` (TGL class-static decl)
- `code/gamecam.cpp:149-151` (gameplay producer + GOSVertex::farClipReciprocal write)
- `code/simplecamera.cpp:168-169` (MechBay producer)
- `code/mission.cpp:3308-3320` (TGL teardown NULL assigns)
- `GameOS/gameos/gameosmain.cpp:953-975` (`glClipControl` adoption; comment
  cites stale line numbers `mclib/camera.cpp:1942-1965` — actual at
  `:2073-2151`; fix as incidental in atomic commit)
- `mclib/mlr/mlrclipper.cpp:121,209,305,321,347` (MLR clipper, mitigation (c))

## 11.5 Orthographic camera note (third-round reviewer answered)

**Answer: no orthographic shaders migrate in F1.** Verified by grep
2026-05-22:

- `setOrthogonal()` (`mclib/camera.cpp:2049`) builds orthographic
  `cameraToClip` at `:2073-2093`. Called only from `calculateTopViewConstants`
  (`:2406+`) and one adjacent setup site at `:2400`. These feed alternate
  camera modes (top-view / non-gameplay), NOT the gameplay perspective
  path that uploads to `terrainMVP`.
- All 14 vertex shaders (including `particle_billboard.vert`) + 3 compute consumers (post-ssao-drop) listed in
  §4.3 + §4.4 read the perspective `terrainMVP` slot fed by
  `code/gamecam.cpp:151` region — driven by `Camera::setPerspective` build,
  not `setOrthogonal`.
- HUD / text / skybox shaders (`gos_text.vert`, `gos_vertex.vert`,
  `gos_vertex_lighted.vert`, `gos_tex_vertex.vert`) use their own
  screen-space `mvp` (a screen-pixel → NDC matrix uploaded via separate
  GOS API), NOT the orthographic `cameraToClip`. Projection-immune to F1.
- Shadow shaders (`shadow_*.vert`, `shadow_terrain.tese`) use
  `lightSpaceMatrix`; separate matrix chain.

Therefore the basis-vector smoke test (§0.6) needs perspective coverage
only. No parallel ortho test required.

If a future slice migrates tacmap (`gametacmap.cpp` consumers per grep) or
debug-overlay paths that feed orthographic-camera-derived matrices to
shaders, that slice carries the obligation to add ortho basis-vector
coverage — out of F1 scope.

## 12. What this design is NOT

- Not a single-session implementation. Stage A-pre + Stage A are independent
  shippable commits, each with its own gate.
- Not a complete `projectZ` retirement. Body + 8 wrappers + instrumentation
  stay alive in F1.
- Not a perf slice. CPU budget already met by A2 + static-update-skip.
- Not an MLR migration. Mitigation (c) accepted.
- Not an editor convergence slice. Separate worktree owns that.
- Not a Stuff library retirement. M2 promotion trigger documented; M2 not
  triggered by F1.
