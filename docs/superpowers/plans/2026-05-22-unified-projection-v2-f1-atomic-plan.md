# Unified-projection F1 atomic implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the inline CPU `AW = axisSwap * worldToClip` composition at `code/gamecam.cpp:165-187` with a single `Camera::worldToClipGL()` accessor; the existing 1-composition + 1-cache (`terrain_mvp_`) + N-readers fan-out architecture stays intact (15+ consumers via `gos_GetTerrainMVPMat4()` + 10 flat-uniform bind sites). Producer change propagates transparently to all consumers via the cache. Retire `terrainMVP` uniform slot + screen→pixel→NDC round-trip kludge across 14 vertex shaders + 3 compute/frag consumers (rename to `u_worldToClipGL`); delete SSAO runtime path. Ships as Stage A-pre (additive parity probe; legacy authoritative) + Stage A (atomic flip).

**Architecture:** Producer-side composition unification feeding a renamed uniform slot (`u_worldToClipGL`); per-shader cleanup of `abs(clip.w)` packaging and pixel-homog round-trip; CPU `pz` gate in `quad.cpp` + `Camera::projectZ` body + 8 wrappers stay alive (red-band class blocker); MLR untouched (mitigation (c)). Single end-state, no parallel slot.

**Tech Stack:** C++17, OpenGL 4.5 (`glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` already adopted), GLSL 4.30+, Stuff::Matrix4D (column-major in memory, row-vector multiply convention), `gos_*` GOS API, CMake `--config RelWithDebInfo`, tier1 smoke harness.

**Spec:** `docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-atomic-design.md` (v2.8 greybeard-signed META-FIX).

---

## Pre-flight reading (engineer MUST read before Task 1)

- Spec v2.8 above — §0 invariants, §2.1 axisSwap literal, §2.1.1 transpose discipline, §4.1 CPU bind-site census (27 sites), §5.1 probe shader, §5.2 Stage A commit, §6 verification, §7 risks (especially R-clipw and R3).
- Worktree CLAUDE.md (`.claude/worktrees/nifty-mendeleev/CLAUDE.md`) — load-bearing rules: GL_FALSE for terrainMVP uploads, full relink before deploy, shader/exe lockstep deploy, AMD shader review mandatory.
- Memory: `memory/mlr_does_not_block_unified_projection.md`, `memory/static_update_skip_already_retired_per_actor_projection.md` for framing.

---

## File structure

**Created files:**
- `scripts/check-unified-projection-retirement.sh` — CI grep gate
- `scripts/check-unified-projection-retirement.allowlist` — bare-mvp HUD/shadow + non-projection `abs(.w)` exceptions

**Modified C++ files (Stage A-pre additive + Stage A atomic):**
- `mclib/camera.h` — decl `Camera::worldToClipGL()`
- `mclib/camera.cpp` — body `Camera::worldToClipGL()` + `axisSwap_MC2toGL` static const
- `GameOS/gameos/gameos_graphics.cpp` — `gos_SetWorldToClipGL` + `gos_SetWorldToClipGLProbeOnly` bodies (replace `gos_SetTerrainMVP`); cache write site; **5 `glGetUniformLocation` rename sites in this file** (`:1730, :1757, :3530, :3958, :6928`; total 10 across all files per spec §4.1 / Task 15 — remaining 5 live in `gos_mech_batcher.cpp`, `gos_static_prop_batcher.cpp`, `gos_particle_bridge.cpp`); 2 `setMat4Direct` rename sites (`:2177, :2337`)
- `GameOS/gameos/gameos_graphics.h` (or equivalent header) — API decl
- `GameOS/gameos/gpu_cull_compute.cpp` — no edit (cache reader at :831 inherits transparently)
- `code/gamecam.cpp:165-187` — replace inline `AW` composition + `gos_SetTerrainMVP(M)` with `gos_SetWorldToClipGL(prog, camera.worldToClipGL())`
- `GameOS/gameos/gos_mech_batcher.cpp:267` — `glGetUniformLocation` rename
- `GameOS/gameos/gos_static_prop_batcher.cpp:549,581,3002` — 3 `glGetUniformLocation` renames
- `GameOS/gameos/gos_particle_bridge.cpp:161` — rename (paired with particle_billboard.vert migration)
- `GameOS/gameos/gameosmain.cpp` — `[UNIFIED_PROJ_PARITY v1]` CPU readback (A-pre) + `MC2_DISABLE_GOSFX=0` runtime guard
- `GameOS/gameos/gos_postprocess.h` + `.cpp` — DELETE SSAO infrastructure (Stage A)
- `.claude/worktrees/nifty-mendeleev/CLAUDE.md` — add Known Issue entry for `MC2_DISABLE_GOSFX=0`
- `scripts/run_smoke.py` — add env assert blocking tier1 under `MC2_DISABLE_GOSFX=0`

**Modified shader files (14 vert + 3 compute/frag in Stage A; gos_terrain.tese also touched in A-pre):**
- `shaders/gos_terrain.tese` — A-pre adds probe scaffold + SSBO; Stage A removes probe + migrates to clean `u_worldToClipGL`
- `shaders/gos_terrain_surface.vert`, `gos_terrain_mask_solid.vert`, `gos_terrain_mask_water.vert`, `gos_terrain_thin.vert`, `gos_terrain_water_fast.vert`, `gos_terrain_water_fast_mdi.vert`, `gos_terrain_mine_static.vert`, `gos_grass.geom`, `static_prop.vert`, `terrain_overlay.vert`, `gos_tex_vertex_lighted.vert`, `mech.vert`, `particle_billboard.vert` — Stage A migration
- `shaders/gpu_driven_terrain_solid.comp`, `gpu_driven_water.comp` — Stage A pz/depth-only replacement (no x/y plane tests added)
- `shaders/ssao.frag` — DELETE in Stage A
- `shaders/gpu_cull_predicate.glsl` — no edit (helper; takes clip as input)
- `shaders/gpu_cull.comp` — no edit (`CullUBO.viewProj` inherits via cache)

---

## Phase 0 — CPU basis-vector smoke test (pre-Stage-A-pre)

### Task 1: Add `Camera::worldToClipGL()` declaration in header

**Files:**
- Modify: `mclib/camera.h` (insert near other matrix accessors; locate via `grep -n "worldToCameraMatrix\|cameraToClip\b" mclib/camera.h`)

- [ ] **Step 1: Locate insertion point**

```bash
grep -n "Stuff::Matrix4D" mclib/camera.h | head -10
```
Expected: list of existing matrix members. Insert decl after `cameraToClip` member declaration block.

- [ ] **Step 2: Insert declaration**

Add inside `class Camera { ... public: ... };` after existing matrix accessors:

```cpp
		// F1 unified-projection: single composition source for runtime GPU
		// uniform path. axisSwap * worldToCameraMatrix * cameraToClip,
		// post-axisSwap GL convention. Distinct from `Camera::worldToClip`
		// (pre-axisSwap; feeds projectZ body and 8 wrappers). See spec
		// 2026-05-22 §0.1 invariant.
		Stuff::Matrix4D worldToClipGL() const;
```

- [ ] **Step 3: Compile check (header-only)**

```powershell
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 20
```
Expected: build SUCCEEDS. A method declaration without callers does not create
an unresolved-symbol link error; the declaration is silently unreferenced.
Correctness signal at this point is "header parses + builds cleanly with the
new decl present." The actual TDD-equivalent failing test is Task 3 (basis-
vector test); the declaration alone is just header scaffolding.

- [ ] **Step 4: Commit**

```bash
git add mclib/camera.h
git commit -m "feat(unified-proj): declare Camera::worldToClipGL() accessor

F1 Stage A-pre Task 1: header declaration only. Body added in
Task 2. Compile fails by design at this checkpoint (linker
unresolved); resolves in Task 2.

Spec: docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-atomic-design.md §2.1"
```

### Task 2: Implement `Camera::worldToClipGL()` body + axisSwap_MC2toGL literal

**Files:**
- Modify: `mclib/camera.cpp` (add near `:2369` existing `worldToClip.Multiply` site)

- [ ] **Step 1: Locate insertion point**

```bash
grep -n "worldToClip.Multiply" mclib/camera.cpp
```
Expected: `:2369` (line number may drift; use grep output).

- [ ] **Step 2: Add file-scope `axisSwap_MC2toGL` static const**

Insert near top of `mclib/camera.cpp` (after includes, in anonymous namespace OR file-scope static):

```cpp
namespace {
// F1 unified-projection axisSwap literal — transplanted from existing
// upload-site logic at code/gamecam.cpp:168-175. Per spec §2.1:
//   GL.x = -MC2.x          (negated)
//   GL.y =  MC2.elevation  (was z; elevation -> up)
//   GL.z =  MC2.ground     (was y; ground -> forward, POSITIVE)
//
// DO NOT "correct" the sign to stock OpenGL -Z forward; this matches the
// existing legacy upload bit-for-bit. Stage A-pre basis-vector test
// (Task 3) verifies empirically.
Stuff::Matrix4D makeAxisSwapMC2toGL()
{
    Stuff::Matrix4D m;
    // Stuff::Matrix4D uses (row, col) indexing; column-major in memory.
    // Initialize to identity then overwrite.
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m(r, c) = (r == c) ? 1.0f : 0.0f;
    // Row 0: GL.x = -MC2.x
    m(0, 0) = -1.0f;
    // Row 1: GL.y = MC2.z (elevation)
    m(1, 1) = 0.0f;
    m(1, 2) = 1.0f;
    // Row 2: GL.z = MC2.y (ground)
    m(2, 2) = 0.0f;
    m(2, 1) = 1.0f;
    return m;
}
const Stuff::Matrix4D kAxisSwapMC2toGL = makeAxisSwapMC2toGL();
} // namespace
```

- [ ] **Step 3: Add `Camera::worldToClipGL()` body**

Insert near `:2369` (after the existing `worldToClip.Multiply` site):

```cpp
//---------------------------------------------------------------------------
Stuff::Matrix4D Camera::worldToClipGL() const
{
    // Stuff::Matrix4D::Multiply convention: dst.Multiply(S1, S2) computes
    // dst = S1 * S2 (verified at mclib/stuff/matrix.cpp:253-258).
    Stuff::Matrix4D viewClip;
    viewClip.Multiply(worldToCameraMatrix, cameraToClip);
    Stuff::Matrix4D out;
    out.Multiply(kAxisSwapMC2toGL, viewClip);
    return out;
}
```

- [ ] **Step 4: Build clean**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
```
Expected: build succeeds. mc2.exe produced.

- [ ] **Step 5: Commit**

```bash
git add mclib/camera.cpp
git commit -m "feat(unified-proj): add Camera::worldToClipGL() body + axisSwap literal

F1 Stage A-pre Task 2: body implementation. axisSwap_MC2toGL
transplanted bit-for-bit from gamecam.cpp:168-175 (verified
2026-05-22). Stage A-pre basis-vector test (Task 3) validates
empirically against the existing terrain_mvp_ cache product.

Spec: 2026-05-22-unified-projection-v2-f1-atomic-design.md §2.1"
```

### Task 3: CPU basis-vector smoke test (one-off scaffold, gated by env)

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (add gated test block near other `MC2_*_SELFTEST` patterns)

**Why:** Spec §0.6 mandates comparing the GLSL-visible matrix orientation produced by the new accessor against the legacy upload-site product, before any shader work. 7 known points; in-front points must match within 1e-5 element-wise and 1e-3 NDC.

- [ ] **Step 1: Locate insertion point**

```bash
grep -n "_SELFTEST\|MC2_ASSET_SCALE_SELFTEST\|MC2_OBJECT_ADMISSION_SELFTEST" GameOS/gameos/gameosmain.cpp | head -10
```
Expected: existing selftest-gate sites; insert new gate adjacent.

- [ ] **Step 2: Add basis-vector test block — at first-camera-render hook, NOT startup**

CRITICAL per fourth-round codex P0.4: `eye` global may be null at startup-
selftest time. The basis test must run when `eye` is guaranteed initialized.
Best site: hook the FIRST `Camera::render()` call (or first call to whatever
populates `worldToCameraMatrix`) — `code/gamecam.cpp` `Camera::render()` body
near the existing `gos_SetTerrainMVP` upload site is a known-live point.

Add a static-once guard so the test runs exactly once per process:

```cpp
// In code/gamecam.cpp, near :165 inside Camera::render() after
// worldToCameraMatrix is guaranteed populated for this frame:
static bool s_unifiedProjBasisTestRan = false;
if (!s_unifiedProjBasisTestRan &&
    getenv("MC2_UNIFIED_PROJECTION_BASIS_TEST") != nullptr) {
    s_unifiedProjBasisTestRan = true;
    extern void unifiedProj_runBasisTest(Camera* eye);
    unifiedProj_runBasisTest(this);
}
```

Then declare + define the test body. Add to `gameosmain.cpp` (or a new
`unified_proj_basis_test.cpp`); test body is called with a guaranteed-live
camera pointer:

```cpp
// F1 unified-projection basis-vector smoke test (spec §0.6).
// Called from Camera::render() at first frame with guaranteed-live camera.
// Compares GLSL-visible matrix orientation produced by Camera::worldToClipGL()
// against the existing gamecam.cpp:165-187 upload product.
// One-off scaffold; deleted in Stage A.
void unifiedProj_runBasisTest(Camera* eye)
{
    // Eye is guaranteed non-null by caller (gamecam.cpp inside Camera::render).
    {
        // Build the new GPU-visible product (post-axisSwap, repackaged
        // column-major -> row-major same way gos_SetWorldToClipGL will).
        Stuff::Matrix4D newM = eye->worldToClipGL();

        // Build the legacy GPU-visible product the way gamecam.cpp:165-187
        // does today. Replicates the inline AW logic.
        Stuff::Matrix4D legacyWorldToClip;
        legacyWorldToClip.Multiply(eye->worldToCameraMatrix, eye->cameraToClip);
        float legacyM_rowmajor[16];
        {
            const float* W = (const float*)&legacyWorldToClip;
            #define WTC(r,c) W[(c)*4+(r)]
            float AW[4][4];
            for (int j = 0; j < 4; j++) {
                AW[0][j] = -WTC(0,j);
                AW[1][j] =  WTC(2,j);
                AW[2][j] =  WTC(1,j);
                AW[3][j] =  WTC(3,j);
            }
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    legacyM_rowmajor[i*4+j] = AW[i][j];
            #undef WTC
        }

        // Repackage newM the same way (column-major Stuff -> row-major M).
        float newM_rowmajor[16];
        {
            const float* col = (const float*)&newM;
            #define WTC(r,c) col[(c)*4 + (r)]
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    newM_rowmajor[i*4 + j] = WTC(i, j);
            #undef WTC
        }

        // Element-wise delta on the GLSL-visible representation.
        float maxElemDelta = 0.0f;
        for (int k = 0; k < 16; ++k) {
            float d = fabsf(newM_rowmajor[k] - legacyM_rowmajor[k]);
            if (d > maxElemDelta) maxElemDelta = d;
        }

        fprintf(stderr,
            "[UNIFIED_PROJ_BASIS] event=elementwise_compare max_delta=%.8f "
            "result=%s (threshold=1e-5)\n",
            maxElemDelta,
            (maxElemDelta < 1e-5f) ? "PASS" : "FAIL");

        // 7 known-point NDC comparison.
        struct TestPoint { const char* name; Stuff::Vector3D world; };
        Stuff::Vector3D camOrig = eye->getCameraOrigin();
        TestPoint pts[7] = {
            { "camera_origin",          camOrig },
            { "world_x_unit",           Stuff::Vector3D(camOrig.x + 1.0f, camOrig.y, camOrig.z) },
            { "ground_y_unit",          Stuff::Vector3D(camOrig.x, camOrig.y + 1.0f, camOrig.z) },
            { "elevation_z_unit",       Stuff::Vector3D(camOrig.x, camOrig.y, camOrig.z + 1.0f) },
            { "in_front_near",          Stuff::Vector3D(camOrig.x, camOrig.y + 10.0f, camOrig.z) },
            { "in_front_far",           Stuff::Vector3D(camOrig.x, camOrig.y + 1000.0f, camOrig.z) },
            { "behind_camera",          Stuff::Vector3D(camOrig.x, camOrig.y - 10.0f, camOrig.z) },
        };
        for (int p = 0; p < 7; ++p) {
            auto mul = [](const float M16[16], float vx, float vy, float vz) {
                // M16 is row-major; GLSL with GL_FALSE upload sees this as
                // column-major in GLSL space, i.e. transpose. To replicate
                // shader `gl_Position = M_glsl * vec4(world,1)`:
                //   M_glsl(i,j) = M16[j*4+i] (transposed access)
                float r[4];
                for (int i = 0; i < 4; ++i) {
                    r[i] = M16[0*4+i] * vx + M16[1*4+i] * vy +
                           M16[2*4+i] * vz + M16[3*4+i];
                }
                return Stuff::Vector4D(r[0], r[1], r[2], r[3]);
            };
            Stuff::Vector4D oldClip = mul(legacyM_rowmajor, pts[p].world.x, pts[p].world.y, pts[p].world.z);
            Stuff::Vector4D newClip = mul(newM_rowmajor,    pts[p].world.x, pts[p].world.y, pts[p].world.z);
            const float epsilon = 1e-4f;
            bool oldBehind = (oldClip.w <= epsilon);
            bool newBehind = (newClip.w <= epsilon);
            if (oldBehind || newBehind) {
                fprintf(stderr,
                    "[UNIFIED_PROJ_BASIS] event=point name=%s status=behind_or_degenerate "
                    "oldW=%.4f newW=%.4f\n", pts[p].name, oldClip.w, newClip.w);
                continue;
            }
            float oldNDC[3] = { oldClip.x/oldClip.w, oldClip.y/oldClip.w, oldClip.z/oldClip.w };
            float newNDC[3] = { newClip.x/newClip.w, newClip.y/newClip.w, newClip.z/newClip.w };
            float d = 0.0f;
            for (int k = 0; k < 3; ++k) {
                float dk = fabsf(oldNDC[k] - newNDC[k]);
                if (dk > d) d = dk;
            }
            fprintf(stderr,
                "[UNIFIED_PROJ_BASIS] event=point name=%s ndc_delta=%.8f "
                "result=%s (threshold=1e-3)\n",
                pts[p].name, d, (d < 1e-3f) ? "PASS" : "FAIL");
        }
    }
}
```

- [ ] **Step 3: Build clean**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
```
Expected: clean build.

- [ ] **Step 4: Run basis test on tier1 mc2_01**

```powershell
$env:MC2_UNIFIED_PROJECTION_BASIS_TEST = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --missions mc2_01 --duration 10 --kill-existing --keep-logs
$env:MC2_UNIFIED_PROJECTION_BASIS_TEST = $null
```

Inspect the produced log (`tests/smoke/artifacts/<latest>/mc2_01.log`) and grep `[UNIFIED_PROJ_BASIS]`. Expected:
- `event=elementwise_compare result=PASS` (max_delta < 1e-5)
- 6 of 7 `event=point` lines with `result=PASS` (5 in-front + camera_origin degenerate)
- 1 `event=point name=behind_camera status=behind_or_degenerate` (expected)

If any `result=FAIL`: STOP. Math is wrong. Debug `Camera::worldToClipGL()` body or `kAxisSwapMC2toGL` literal before proceeding.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "test(unified-proj): CPU basis-vector smoke test (one-off scaffold)

F1 Stage A-pre Task 3: validates GLSL-visible matrix orientation
produced by Camera::worldToClipGL() against legacy gamecam.cpp:165-187
upload product. 7 known points, element-wise + NDC comparison.

Gate: MC2_UNIFIED_PROJECTION_BASIS_TEST=1. Deleted in Stage A.

Spec: 2026-05-22-unified-projection-v2-f1-atomic-design.md §0.6"
```

---

## Phase 1 — Stage A-pre additive (legacy authoritative)

### Task 4: Add `gos_SetWorldToClipGL` GOS API + `u_worldToClipGL` uniform slot

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (add new function near `gos_SetTerrainMVP` at `:7081`)
- Modify: `GameOS/gameos/gos_mech_killswitch.h` OR equivalent GOS header that exposes `gos_SetTerrainMVP` (locate via grep)

- [ ] **Step 1: Locate `gos_SetTerrainMVP` for adjacency**

```bash
grep -n "void __stdcall gos_SetTerrainMVP\|gos_SetTerrainMVP(" GameOS/gameos/gameos_graphics.cpp
```

- [ ] **Step 2: Add `gos_SetWorldToClipGLProbeOnly` (A-pre setter, no cache write)**

CRITICAL per fourth-round codex P0.1 + P0.2:
- Use `glProgramUniformMatrix4fv` (explicit program target), NOT `glUniformMatrix4fv`
  (which would upload to the currently-bound program — silent wrong-shader bug).
- A-pre setter does NOT write `terrain_mvp_` cache. Stage A-pre must leave
  legacy `gos_SetTerrainMVP` authoritative; cache stays under legacy ownership
  until the atomic flip in Task 14.

```cpp
// F1 unified-projection — A-PRE PROBE-ONLY setter (Task 4).
// Uploads u_worldToClipGL to the explicitly-passed program; does NOT write
// terrain_mvp_ cache. Legacy gos_SetTerrainMVP remains authoritative for
// all consumers until Stage A (Task 14) promotes a cache-writing variant.
//
// Uses glProgramUniformMatrix4fv (GL 4.1+) for explicit-program upload —
// glUniformMatrix4fv would upload to currently-bound program, which is
// not what we want here.
void __stdcall gos_SetWorldToClipGLProbeOnly(GLuint program, const Stuff::Matrix4D& mat)
{
    if (program == 0) return;
    GLint loc = glGetUniformLocation(program, "u_worldToClipGL");
    if (loc == -1) {
        // Stage A-pre tolerance: not-yet-migrated shaders silently no-op.
        return;
    }
    const float* col = (const float*)&mat;
    #define WTC(r,c) col[(c)*4 + (r)]
    float M[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            M[i*4 + j] = WTC(i, j);  // column-major -> row-major
    #undef WTC
    glProgramUniformMatrix4fv(program, loc, 1, GL_FALSE, M);
    // NO terrain_mvp_ write — A-pre is observation-only. See Task 14
    // for the Stage A promotion that adds cache write.
}
```

(Note: requires `glProgramUniformMatrix4fv` from GL 4.1+ or `GL_ARB_separate_shader_objects`. Engine uses 4.5 already — available. If toolchain rejects, alternative: `GLint prev; glGetIntegerv(GL_CURRENT_PROGRAM, &prev); glUseProgram(program); glUniformMatrix4fv(...); glUseProgram(prev);` — explicit bind+restore.)

- [ ] **Step 3: Declare in header**

```bash
grep -rn "void __stdcall gos_SetTerrainMVP\|gos_SetTerrainMVP(const float" GameOS/gameos/*.h
```

Insert decls alongside existing `gos_SetTerrainMVP` decl:

```cpp
// A-pre setter (probe only, no cache write). Retired in Stage A.
void __stdcall gos_SetWorldToClipGLProbeOnly(GLuint program, const Stuff::Matrix4D& mat);
// Stage A setter (explicit-program upload + cache write). Added in Task 14.
void __stdcall gos_SetWorldToClipGL(GLuint program, const Stuff::Matrix4D& mat);
```

- [ ] **Step 4: Build clean**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
```
Expected: clean build (function defined but uncalled = OK).

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gameos_graphics.cpp GameOS/gameos/gos_mech_killswitch.h
git commit -m "feat(unified-proj): gos_SetWorldToClipGL GOS API + column-major repackage

F1 Stage A-pre Task 4: explicit-program signature per Vulkan-prep
discipline. Column-major Stuff -> row-major repackage preserves
GLSL-visible orientation. -1 location silent no-op for A-pre;
becomes fatal in Stage A.

Spec: §2.1.1 + §5.1 location-binding behavior"
```

### Task 5: Add producer call at `code/gamecam.cpp` (parallel to existing `gos_SetTerrainMVP`)

**Files:**
- Modify: `code/gamecam.cpp:165-187` region (add new call adjacent to existing upload)

**A-pre rule:** ADDITIVE only. Legacy `gos_SetTerrainMVP(M)` STAYS active and authoritative. New `gos_SetWorldToClipGL` is called alongside.

- [ ] **Step 1: Locate current upload code**

```bash
grep -n "gos_SetTerrainMVP(M);" code/gamecam.cpp
```
Expected: `:187`.

- [ ] **Step 2: Add new producer call immediately after `gos_SetTerrainMVP(M);`**

```cpp
            gos_SetTerrainMVP(M);

            // F1 Stage A-pre: parallel probe-only upload via new accessor.
            // Legacy gos_SetTerrainMVP(M) above stays AUTHORITATIVE; new
            // path uploads u_worldToClipGL only to the gos_terrain.tese
            // program (the only declarant in A-pre per Task 7) and DOES
            // NOT write terrain_mvp_ cache.
            //
            // Plan-stage decision: source of the gos_terrain.tese program
            // handle. Options (pick at implementation time by grep):
            //   (a) `gos_GetCurrentTerrainProgram()` accessor if it exists
            //   (b) directly query the cached terrain program member that
            //       gameos_graphics.cpp already maintains (search:
            //       `s_terrainProgram`, `terrainLocs_.program`, etc.)
            //   (c) add a thin getter `gos_GetTerrainTeseProgram()` in
            //       gameos_graphics.cpp that returns the live handle for
            //       the program containing the tessellated-terrain pipeline
            // Verify by grep BEFORE writing the code; do NOT invent an
            // accessor that doesn't exist.
            GLuint terrainProg = /* SEE PLAN-STAGE NOTE ABOVE */;
            if (terrainProg != 0) {
                gos_SetWorldToClipGLProbeOnly(terrainProg, eye->worldToClipGL());
            }
```

(If `gos_GetCurrentTerrainProgram()` doesn't exist, plan-stage adds a thin accessor in `gameos_graphics.cpp` returning the live terrain program handle. Locate via `grep -n "s_terrainProgram\|terrainProg" GameOS/gameos/gameos_graphics.cpp`.)

- [ ] **Step 3: Build clean + run mc2_01 30s with default env**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 5
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --missions mc2_01 --duration 30 --kill-existing --keep-logs
```
Expected: exit 0; rendering byte-identical to pre-change (legacy path still active).

- [ ] **Step 4: Commit**

```bash
git add code/gamecam.cpp
git commit -m "feat(unified-proj): parallel A-pre producer call alongside legacy

F1 Stage A-pre Task 5: adds gos_SetWorldToClipGL adjacent to
gos_SetTerrainMVP. Legacy authoritative. New uniform populated
only where declared (gos_terrain.tese in Task 7).

Spec: §5.1 Stage A-pre artifact contents"
```

### Task 6: Add SSBO + probe scaffold to `gos_terrain.tese`

**Files:**
- Modify: `shaders/gos_terrain.tese`

- [ ] **Step 1: Locate next free SSBO binding slot**

```bash
grep -nE "layout\(std430, binding = " shaders/*.{comp,frag,vert,tese} 2>$null | awk -F'binding = ' '{print $2}' | awk -F')' '{print $1}' | sort -nu
```
Expected: list of used bindings. Pick next free slot (suggested: 14 if not used; verify against grep output). Plan-stage commits this slot number; use `BIND_PROBE = N` placeholder below replaced with the chosen number.

- [ ] **Step 2: Declare `u_worldToClipGL` uniform + SSBO in `gos_terrain.tese`**

Locate the existing uniform block at `:22-26` (`uniform mat4 terrainMVP; uniform vec4 terrainViewport; uniform mat4 mvp;`). Add immediately after:

```glsl
// F1 Stage A-pre: parallel uniform for parity probe (spec §5.1).
// Stage A retires terrainMVP/terrainViewport/mvp and keeps only this one.
// During A-pre, u_worldToClipGL is declared but NOT consumed for emission —
// only read inside the probe block. Default-build (no probe macro) treats
// it as dead uniform; AMD driver may warn-on-unused-uniform (acceptable).
uniform mat4 u_worldToClipGL;

// F1 Stage A-pre parity probe SSBO (spec §5.1 SSBO declaration).
// Per fourth-round codex P1: guard SSBO declaration behind the same macro
// as the probe block, so default-build is byte-identical to pre-F1 (no
// SSBO binding, no dead-uniform-decl noise).
#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE
// Counter layout (uint32 each):
//   [0] = max_delta_comparable (bit-cast float, atomicMax)
//   [1] = count_compared
//   [2] = count_behind_old_only
//   [3] = count_behind_new_only
//   [4] = count_nonfinite_old_only (hazard: in-front but NaN/Inf)
//   [5] = count_behind_both
//   [6] = count_nonfinite_both
//   [7] = count_nonfinite_new_only
// Cleared at mission start; accumulated across run.
layout(std430, binding = 14) buffer DebugSSBO {
    uint debugSSBO_counters[8];
};
#endif // MC2_UNIFIED_PROJECTION_PARITY_PROBE
```

(Replace `14` with the slot chosen in Step 1.)

- [ ] **Step 3: Wrap existing `gl_Position` emission with probe**

Locate the legacy emission at `:126-141` (`vec4 clip = terrainMVP * vec4(worldPos, 1.0); ... gl_Position = vec4(ndc.xyz * absW, absW);`).

Wrap entire block in `computeLegacyGlPosition` semantic, then add probe + ensure both branches still output legacy:

```glsl
    // --- Projection of DISPLACED position (visual rendering) ---
    // Begin computeLegacyGlPosition equivalent — EXACT existing block:
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    screen.z = clip.z * rhw + TERRAIN_DEPTH_FUDGE;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    vec4 legacyGlPosition = vec4(ndc.xyz * absW, absW);
    // End computeLegacyGlPosition equivalent.

#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE
    vec4 newClip = u_worldToClipGL * vec4(worldPos, 1.0);

    const float epsilon = 1e-4;
    bool oldBehind = (legacyGlPosition.w <= epsilon);
    bool newBehind = (newClip.w <= epsilon);

    vec3 oldNDC = oldBehind ? vec3(0.0) : (legacyGlPosition.xyz / legacyGlPosition.w);
    vec3 newNDC = newBehind ? vec3(0.0) : (newClip.xyz       / newClip.w);

    // Hazard = in-front AND has NaN/Inf NDC. Isolates math hazard from
    // behind-camera classification (spec gemini #4 fix).
    bool oldHazard = !oldBehind && (any(isnan(oldNDC)) || any(isinf(oldNDC)));
    bool newHazard = !newBehind && (any(isnan(newNDC)) || any(isinf(newNDC)));
    bool comparable = !oldBehind && !newBehind && !oldHazard && !newHazard;

    if (comparable) {
        float d = max(abs(oldNDC.x - newNDC.x),
                  max(abs(oldNDC.y - newNDC.y),
                      abs(oldNDC.z - newNDC.z)));
        // Un-nest bitcast inside atomicMax per AMD GLSL compiler quirk
        // (spec gemini #3). Cast to local uint first.
        uint dBits = floatBitsToUint(d);
        atomicMax(debugSSBO_counters[0], dBits);
        atomicAdd(debugSSBO_counters[1], 1u);
    } else {
        if (oldBehind && newBehind) {
            atomicAdd(debugSSBO_counters[5], 1u);  // count_behind_both
        } else if (oldBehind) {
            atomicAdd(debugSSBO_counters[2], 1u);  // count_behind_old_only
        } else if (newBehind) {
            atomicAdd(debugSSBO_counters[3], 1u);  // count_behind_new_only
        } else {
            if (oldHazard && newHazard) {
                atomicAdd(debugSSBO_counters[6], 1u);  // count_nonfinite_both
            } else if (oldHazard) {
                atomicAdd(debugSSBO_counters[4], 1u);  // count_nonfinite_old_only
            } else /* newHazard */ {
                atomicAdd(debugSSBO_counters[7], 1u);  // count_nonfinite_new_only
            }
        }
    }
#endif

    // CRITICAL: BOTH branches output legacy. Stage A-pre changes ZERO
    // production behavior. A build without
    // MC2_UNIFIED_PROJECTION_PARITY_PROBE behaves byte-identical to pre-F1.
    gl_Position = legacyGlPosition;
```

- [ ] **Step 4: Deploy shaders + run mc2_01 30s with default env (no probe flag yet)**

```powershell
# Per CLAUDE.md shader/exe lockstep deploy rule:
sh A:/Games/mc2-opengl/mc2-win64-v0.4/deploy-shaders.sh   # or equivalent project script
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --missions mc2_01 --duration 30 --kill-existing --keep-logs
```
Expected: exit 0; rendering byte-identical to pre-change. Probe is `#ifdef`-gated off in default build.

- [ ] **Step 5: Commit**

```bash
git add shaders/gos_terrain.tese
git commit -m "feat(unified-proj): A-pre parity probe scaffold in gos_terrain.tese

F1 Stage A-pre Task 6: SSBO declaration + probe wrapper + dual-
consumer math. Both #ifdef branches output legacy gl_Position.
MC2_UNIFIED_PROJECTION_PARITY_PROBE gate controls compile-time
inclusion of the comparison logic.

Spec: §5.1 probe shader"
```

### Task 7: Add CPU-side SSBO setup + readback in `gameosmain.cpp`

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (SSBO alloc at startup, clear at mission start, periodic readback + final summary)
- Modify: `code/mission.cpp` (clear-at-mission-start hook; locate per spec §5.1 SSBO lifecycle)

- [ ] **Step 1: Locate existing SSBO setup patterns**

```bash
grep -n "glGenBuffers\|glBufferStorage.*UNIFORM\|GL_SHADER_STORAGE_BUFFER" GameOS/gameos/gameos_graphics.cpp GameOS/gameos/gpu_cull_compute.cpp | head -10
```

- [ ] **Step 2: Add SSBO alloc + glBindBufferBase at startup, gated by probe build flag**

Add to `gameosmain.cpp` near other GL setup, inside `#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE`:

```cpp
#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE
static GLuint s_unifiedProjProbeSSBO = 0;
static const GLuint kUnifiedProjProbeBinding = 14;  // matches gos_terrain.tese

void unifiedProj_probeInit()
{
    glGenBuffers(1, &s_unifiedProjProbeSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_unifiedProjProbeSSBO);
    uint32_t zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(zeros), zeros,
                    GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kUnifiedProjProbeBinding,
                     s_unifiedProjProbeSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    fprintf(stderr, "[UNIFIED_PROJ_PARITY v1] event=ssbo_init binding=%u\n",
            kUnifiedProjProbeBinding);
}

void unifiedProj_probeReset()
{
    if (!s_unifiedProjProbeSSBO) return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_unifiedProjProbeSSBO);
    uint32_t zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void unifiedProj_probeSnapshot(const char* tag)
{
    if (!s_unifiedProjProbeSSBO) return;
    uint32_t counters[8] = {0};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_unifiedProjProbeSSBO);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    float maxDelta = 0.0f;
    memcpy(&maxDelta, &counters[0], sizeof(float));  // bit-cast back
    fprintf(stderr,
        "[UNIFIED_PROJ_PARITY v1] tag=%s max_delta_comparable=%.6f compared=%u "
        "behind_old_only=%u behind_new_only=%u behind_both=%u "
        "nonfinite_old_only=%u nonfinite_new_only=%u nonfinite_both=%u\n",
        tag, maxDelta, counters[1], counters[2], counters[3], counters[5],
        counters[4], counters[7], counters[6]);
}
#endif
```

Call `unifiedProj_probeInit()` once after GL context creation. Call `unifiedProj_probeSnapshot("frame_N")` every 60 frames in the main render loop. Call `unifiedProj_probeReset()` at mission start.

- [ ] **Step 3: Hook reset in `code/mission.cpp`**

Locate `Mission::load` or first-frame init site:
```bash
grep -n "Mission::load\|MissionInit\|onMissionLoad" code/mission.cpp | head -5
```

Add inside the function body:

```cpp
#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE
    extern void unifiedProj_probeReset();
    unifiedProj_probeReset();
#endif
```

- [ ] **Step 4: Build clean with probe flag (BOTH C++ AND GLSL macro paths)**

CRITICAL per fourth-round codex P0.3: a CMake C++ flag does NOT reach GLSL
preprocessor by default. Two propagation steps required:

1. **C++ side:** add `-DMC2_UNIFIED_PROJECTION_PARITY_PROBE` to `CMAKE_CXX_FLAGS`
   (e.g. via build profile or one-off cmake invocation).
2. **GLSL side:** locate the shader-prefix system (engine uses
   `makeProgram(prefix, ...)` per CLAUDE.md "Shader `#version`" rule —
   `"#version 430\n"` is passed as prefix). Extend the prefix at probe-build
   time to include `"#define MC2_UNIFIED_PROJECTION_PARITY_PROBE 1\n"`. Likely
   in `GameOS/gameos/shader_loader.cpp` or wherever the prefix lives:
   ```cpp
   std::string prefix = "#version 430\n";
   #ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE
       prefix += "#define MC2_UNIFIED_PROJECTION_PARITY_PROBE 1\n";
   #endif
   ```
3. **Verification:** after build, dump a compiled shader source or log to
   confirm `#define MC2_UNIFIED_PROJECTION_PARITY_PROBE` is present:
   ```bash
   # Add temporary debug print of full shader source before glCompileShader
   # for gos_terrain.tese — verify the define line appears
   ```

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-DMC2_UNIFIED_PROJECTION_PARITY_PROBE %CMAKE_CXX_FLAGS_RELWITHDEBINFO%" -B build64 -S .
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 5
```

If `compared=0` shows up in Task 7 Step 5 output, the GLSL macro propagation
is broken — go back and add the shader-prefix extension explicitly.

- [ ] **Step 5: Smoke run mc2_01 30s with probe build**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --missions mc2_01 --duration 30 --kill-existing --keep-logs
```

Inspect log for `[UNIFIED_PROJ_PARITY v1]` lines. Expected: `event=ssbo_init`, then periodic `tag=frame_N` snapshots. `max_delta_comparable < 1e-3`. `nonfinite_new_only == 0`. `nonfinite_both == 0`.

If gate fails: STOP. Investigate `Camera::worldToClipGL()` body or axisSwap literal.

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp code/mission.cpp
git commit -m "feat(unified-proj): A-pre probe CPU side (SSBO + readback + reset)

F1 Stage A-pre Task 7: gated SSBO at binding 14, init at startup,
reset at mission load, periodic readback every 60 frames.
[UNIFIED_PROJ_PARITY v1] log lines structured per spec.

Spec: §5.1 SSBO lifecycle"
```

---

## Phase 2 — Stage A-pre validation gates

### Task 8: Tier1 5/5 with probe build

- [ ] **Step 1: Run tier1 5/5 with probe build (30s each)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

- [ ] **Step 2: Verify gate criteria for each mission**

Grep each mission log for `[UNIFIED_PROJ_PARITY v1]` final-frame snapshot:

```powershell
$artifacts = (Get-ChildItem tests\smoke\artifacts | Sort-Object Name -Descending)[0]
foreach ($m in @("mc2_01", "mc2_03", "mc2_10", "mc2_17", "mc2_24")) {
    Write-Host "=== $m ==="
    Select-String "[UNIFIED_PROJ_PARITY v1]" "$($artifacts.FullName)\$m.log" | Select-Object -Last 1
}
```

For each mission, verify:
- `max_delta_comparable < 1e-3`
- `compared > 0` (probe firing)
- `nonfinite_new_only == 0`
- `nonfinite_both == 0`

If ANY mission fails: STOP. Debug per spec §6.1 fallback.

- [ ] **Step 3: Document gate-pass evidence**

Create `docs/observations/2026-05-22-unified-proj-apre-gate.md` with the per-mission counters captured. Reference in Stage A commit message.

- [ ] **Step 4: Commit observation doc**

```bash
git add docs/observations/2026-05-22-unified-proj-apre-gate.md
git commit -m "docs(unified-proj): A-pre tier1 gate evidence

Per-mission probe counters from tier1 5/5 probe build.
All missions pass: max_delta_comparable < 1e-3,
nonfinite_new_only = 0, nonfinite_both = 0.

Spec: §6.1"
```

### Task 9: User-driven `mc2_10` wolfman canary with probe build

- [ ] **Step 1: User-driven canary 60s**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_10 --duration 60 --kill-existing --keep-logs
```

User drives mc2_10 mission to wolfman zoom + worst-case below+behind-camera angles. Probe accumulates counters across the run.

- [ ] **Step 2: Verify gate criteria (per Task 8)**

Same criteria as Task 8 for `mc2_10` log.

**Additional check per R-clipw (spec §7):** verify `count_behind_new_only / max(count_compared, 1) < 0.001`. Division guard prevents div-by-zero if probe didn't fire (the main gate from Task 8 requires `count_compared > 0`, but defensive guard here costs nothing). Non-zero `behind_new_only` is expected for legitimately behind-camera verts. If ratio exceeds 0.001 AND user reports missing terrain at wolfman zoom: STOP. Matrix model needs revision before Stage A.

- [ ] **Step 3: Document canary result**

Append to `docs/observations/2026-05-22-unified-proj-apre-gate.md`.

- [ ] **Step 4: Commit**

```bash
git add docs/observations/2026-05-22-unified-proj-apre-gate.md
git commit -m "docs(unified-proj): A-pre wolfman canary gate evidence

mc2_10 user-driven canary 60s with probe build. R-clipw gate
verified: count_behind_new_only/count_compared < 0.001.

Spec: §6.1 + §7 R-clipw"
```

---

## Phase 3 — Stage A atomic flip (single commit)

Stage A is a SINGLE atomic commit. All sub-tasks below produce changes in the working tree; the final commit lands them together. Per spec §5.2.

### Task 10: Delete A-pre probe scaffold + migrate `gos_terrain.tese` to clean consumption

**Files:**
- Modify: `shaders/gos_terrain.tese`

- [ ] **Step 1: Replace probe block + legacy `gl_Position` math with clean emit**

Delete lines added in Task 6 (SSBO decl + probe wrapper + dual-consumer block). Delete the legacy `terrainMVP` / `terrainViewport` / `mvp` consumption and screen→pixel→NDC round-trip. Replace with:

```glsl
    // F1 unified-projection: clean GL convention. clip.w = z_eye for
    // in-front geometry under hand-built reverse-Z + ZERO_TO_ONE
    // perspective. GPU clipper rejects behind-camera via standard
    // x/y: -w <= x,y <= w; z: 0 <= z <= w.
    gl_Position = u_worldToClipGL * vec4(worldPos, 1.0);
    // UndisplacedDepth still needs computing for the bisect math:
    vec4 uclip = u_worldToClipGL * vec4(undisplacedWorldPos, 1.0);
    UndisplacedDepth = uclip.z / uclip.w;
```

Also delete:
- `uniform mat4 terrainMVP;` (line 24)
- `uniform vec4 terrainViewport;` (line 25)
- `uniform mat4 mvp;` (line 26)
- the SSBO declaration block from Task 6
- the `#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE` wrapper (Stage A retires the probe)

### Task 11: Migrate remaining 13 vertex shaders

**Files:** 13 vertex/geometry shaders per spec §4.3:
- `shaders/gos_terrain_surface.vert`
- `shaders/gos_terrain_mask_solid.vert`
- `shaders/gos_terrain_mask_water.vert`
- `shaders/gos_terrain_thin.vert`
- `shaders/gos_terrain_water_fast.vert`
- `shaders/gos_terrain_water_fast_mdi.vert`
- `shaders/gos_terrain_mine_static.vert`
- `shaders/gos_grass.geom`
- `shaders/static_prop.vert`
- `shaders/terrain_overlay.vert`
- `shaders/gos_tex_vertex_lighted.vert`
- `shaders/mech.vert`
- `shaders/particle_billboard.vert`

**Pattern per shader (canonical template):**

Replace:
```glsl
uniform mat4 terrainMVP;        // ...
uniform vec4 terrainViewport;   // ...
uniform mat4 mvp;               // screen pixels -> NDC
// ...
vec4 clip = terrainMVP * vec4(world, 1.0);
float rhw = 1.0 / clip.w;
vec3 screen;
screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
screen.z = clip.z * rhw + TERRAIN_DEPTH_FUDGE;
vec4 ndc = mvp * vec4(screen, 1.0);
float absW = abs(clip.w);
gl_Position = vec4(ndc.xyz * absW, absW);
```

With:
```glsl
uniform mat4 u_worldToClipGL;
// ...
gl_Position = u_worldToClipGL * vec4(world, 1.0);
```

**Allowlisted bare-`mvp` retentions (spec §6.2.1 allowlist):** do NOT delete `mvp` in shadow shaders (`shadow_terrain.vert:8` etc) or HUD shaders. `particle_billboard.vert:43` (`u_mvp`) needs case-by-case review — if it's used for billboard screen-space math (not the projection round-trip), keep it. Plan-stage decision based on its actual usage.

- [ ] **Step 1: Migrate `gos_terrain_surface.vert`**

```bash
grep -n "terrainMVP\|terrainViewport\|abs.*clip.*w\|gl_Position" shaders/gos_terrain_surface.vert
```
Apply canonical template. Read the file, identify the round-trip block, replace with clean emit.

- [ ] **Step 2: Migrate `gos_terrain_mask_solid.vert`** — same canonical template
- [ ] **Step 3: Migrate `gos_terrain_mask_water.vert`** — same canonical template
- [ ] **Step 4: Migrate `gos_terrain_thin.vert`** — same canonical template
- [ ] **Step 5: Migrate `gos_terrain_water_fast.vert`** — same canonical template
- [ ] **Step 6: Migrate `gos_terrain_water_fast_mdi.vert`** — same canonical template
- [ ] **Step 7: Migrate `gos_terrain_mine_static.vert`** — same canonical template
- [ ] **Step 8: Migrate `gos_grass.geom`** — same canonical template (note: geometry shader; may have additional billboard logic to preserve)
- [ ] **Step 9: Migrate `static_prop.vert`** — same canonical template
- [ ] **Step 10: Migrate `terrain_overlay.vert`** — same canonical template
- [ ] **Step 11: Migrate `gos_tex_vertex_lighted.vert`** — special case: has `mvp_`, `world_`, `view_`, `wvp_`, `projection_` uniforms plus `terrainMVP`. Plan-stage audits which are dead. Conservatively: replace `terrainMVP` consumption with `u_worldToClipGL`; mark `mvp_`/`projection_` for separate cleanup if they participate in the round-trip; leave `world_`/`view_`/`wvp_` if they serve other purposes (lighting etc).
- [ ] **Step 12: Migrate `mech.vert`** — same canonical template (dynamics shader; per `cull_gates_are_load_bearing.md` "mechs are the canary"; per spec the screen-spanning-triangle class should retire here)
- [ ] **Step 13: Migrate `particle_billboard.vert`** — same canonical template for `terrainMVP`; preserve `u_mvp` only if it serves billboard screen-space math distinct from projection round-trip. Plan-stage audit required.

### Task 12: Migrate compute/frag pz/depth gates (3 sites per spec §4.4)

**Files:**
- Modify: `shaders/gpu_driven_terrain_solid.comp:194,214,226,234`
- Modify: `shaders/gpu_driven_water.comp:137,173`
- NO EDIT: `shaders/gpu_cull_predicate.glsl` (helper, no matrix uniform)

- [ ] **Step 1: Migrate `gpu_driven_terrain_solid.comp`**

Replace `uniform mat4 u_terrainMVP;` with `uniform mat4 u_worldToClipGL;`. Per spec §4.4: replace `pz ∈ [0,1)` admission tests with `clip.w > epsilon AND 0 <= clip.z <= clip.w` (ZERO_TO_ONE rule). **Do NOT add x/y plane tests** that legacy didn't have (spec §4.4 over-cull warning).

Read each site at `:194,:214,:226,:234` and apply the replacement. Show diff in commit description.

- [ ] **Step 2: Migrate `gpu_driven_water.comp`**

Same pattern as terrain_solid. Sites at `:137,:173`. If grep reveals existing x/y checks, carry them forward in the new clip-rule form (do not delete; do not add).

- [ ] **Step 3: Verify `gpu_cull_predicate.glsl` requires no edit**

```bash
grep -n "uniform mat4\|terrainMVP" shaders/gpu_cull_predicate.glsl
```
Expected: zero hits. File is a stateless helper. Matrix consumer is `CullUBO.viewProj` UBO at `gpu_cull.comp:169-170`, written from cache at `gpu_cull_compute.cpp:831` — inherits transparently from producer change in Task 14.

### Task 13: Delete SSAO runtime path entirely

**Files:**
- Delete: `shaders/ssao.frag`
- Modify: `GameOS/gameos/gos_postprocess.h` — remove `void runSSAO();` decl + SSAO FBO members
- Modify: `GameOS/gameos/gos_postprocess.cpp` — delete `:164` shader load, `:346-383` FBO allocation, `:383+` noise texture, `:676-714` `runSSAO()` body, `:918` call site
- Modify: `GameOS/gameos/gameosmain.cpp:284-285` — update the SSAO-toggle comment (key now repurposed; SSAO infrastructure gone)

- [ ] **Step 1: Locate all SSAO infra sites**

```bash
grep -rn "ssao\|SSAO\|runSSAO\|s_ssaoNoise\|ssaoFBO\|ssaoBlur" GameOS/gameos/gos_postprocess.{cpp,h} shaders/ssao.frag 2>$null | head -40
```

- [ ] **Step 2: Delete `shaders/ssao.frag`**

```bash
git rm shaders/ssao.frag
```

- [ ] **Step 3: Delete SSAO members + decls from `gos_postprocess.h`**

Read the file, locate `runSSAO()` decl and SSAO FBO/noise members under `// SSAO` comment block. Delete.

- [ ] **Step 4: Delete SSAO bodies + caller from `gos_postprocess.cpp`**

- Delete shader-load at `:164` (`"shaders/postprocess.vert", "shaders/ssao.frag", kShaderPrefix`)
- Delete FBO allocation block `:346-383`
- Delete noise texture allocation
- Delete `void gosPostProcess::runSSAO() { ... }` body (`:676-714`+)
- Delete `runSSAO();` call at `:918` (main postprocess pass)
- Verify no other references remain via post-edit grep

- [ ] **Step 5: Update SSAO-toggle comment at `gameosmain.cpp:284-285`**

Replace:
```
// Repurposed from SSAO toggle to GPU static prop frag debug
// cycle. SSAO infrastructure is preserved in code; rebind
```
With:
```
// Repurposed from SSAO toggle to GPU static prop frag debug cycle.
// SSAO infrastructure removed entirely in F1 unified-projection
// retirement (2026-05-22 spec). Key no longer toggles SSAO.
```

### Task 14: Producer + GOS API + cache migration

**Files:**
- Modify: `code/gamecam.cpp:165-187` — replace inline `AW` composition + `gos_SetTerrainMVP(M)` with single `gos_SetWorldToClipGL(prog, eye->worldToClipGL())` call
- Modify: `GameOS/gameos/gameos_graphics.cpp` — retire `gos_SetTerrainMVP` body at `:7081`; promote `gos_SetWorldToClipGL` to fatal-on-`-1`-location per spec §5.1; rename `gos_GetTerrainMVPMat4` accessor if plan-stage chose to (see §4.1 #9 — decision: KEEP name to avoid 15+ caller renames)

- [ ] **Step 1: Replace gamecam.cpp:165-187 inline composition**

Delete lines 165-187 (the entire `AW[4][4]` repackage block + `gos_SetTerrainMVP(M)` call + the parallel A-pre `gos_SetWorldToClipGL` call). Replace with:

```cpp
            // F1 unified-projection: single producer call.
            // gos_SetWorldToClipGL composes axisSwap*worldToCamera*cameraToClip
            // (via Camera::worldToClipGL) and uploads with the column-major
            // -> row-major repackage internally. terrain_mvp_ cache is
            // written by the same call; existing gos_GetTerrainMVPMat4()
            // callers (CullUBO, mech-batcher, static-prop-batcher, etc.)
            // inherit the new value transparently.
            extern GLuint gos_GetCurrentTerrainProgram();
            GLuint terrainProg = gos_GetCurrentTerrainProgram();
            gos_SetWorldToClipGL(terrainProg, eye->worldToClipGL());
```

- [ ] **Step 2: Add Stage A `gos_SetWorldToClipGL` (cache-writing, fatal-on-`-1`)**

Stage A introduces the production `gos_SetWorldToClipGL` next to the A-pre
probe-only setter from Task 4. The probe-only setter is DELETED in Stage A
(no longer needed once consumers migrate). Production setter writes the
cache for the 15+ existing readers via `gos_GetTerrainMVPMat4()`:

```cpp
// F1 unified-projection — STAGE A production setter (cache-writing).
// Uploads to the explicitly-passed program AND writes the terrain_mvp_
// cache that all gos_GetTerrainMVPMat4() callers read. This is what
// makes the producer change propagate to CullUBO, mech-batcher,
// static-prop-batcher, etc. transparently.
void __stdcall gos_SetWorldToClipGL(GLuint program, const Stuff::Matrix4D& mat)
{
    if (program == 0) {
        fprintf(stderr,
            "[UNIFIED_PROJ v1] FATAL: gos_SetWorldToClipGL called with program=0\n");
        abort();
    }
    GLint loc = glGetUniformLocation(program, "u_worldToClipGL");
    if (loc == -1) {
        // Stage A: every migrated program MUST declare u_worldToClipGL.
        // -1 location indicates a missed migration; fatal per spec §5.1.
        fprintf(stderr,
            "[UNIFIED_PROJ v1] FATAL: u_worldToClipGL missing from program %u\n",
            program);
        abort();
    }
    const float* col = (const float*)&mat;
    #define WTC(r,c) col[(c)*4 + (r)]
    float M[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            M[i*4 + j] = WTC(i, j);  // column-major -> row-major
    #undef WTC
    glProgramUniformMatrix4fv(program, loc, 1, GL_FALSE, M);

    // Stage A cache write — all gos_GetTerrainMVPMat4() readers inherit
    // via the same cache that gos_SetTerrainMVP used to write. Bit-
    // identical M layout preserves consumer expectations.
    memcpy(&terrain_mvp_, M, 16 * sizeof(float));
    terrain_mvp_valid_ = true;
}
```

DELETE the A-pre `gos_SetWorldToClipGLProbeOnly` function and its header
declaration in the same atomic commit.

(Note: `terrain_mvp_` member-access syntax depends on whether this body
lives inside the renderer class; locate the existing `gos_SetTerrainMVP`
body's cache-write pattern at `gameos_graphics.cpp:1399-1400` and mirror
exactly.)

- [ ] **Step 3: Retire `gos_SetTerrainMVP` body at `:7081`**

Delete the function body and its header declaration. Any remaining callers (should be ZERO after Task 14 Step 1) will fail to link, surfacing missed migrations.

### Task 15: Rename `glGetUniformLocation` strings (10 sites per spec §4.1 #10-#21)

**Files:** Per spec §4.1 census — apply each rename:

- [ ] **Step 1: Rename `gameos_graphics.cpp:1730`** — `"terrainMVP"` → `"u_worldToClipGL"`
- [ ] **Step 2: Rename `gameos_graphics.cpp:1757`** — same
- [ ] **Step 3: Rename `gameos_graphics.cpp:3530`** — same
- [ ] **Step 4: Rename `gameos_graphics.cpp:3958`** — same
- [ ] **Step 5: Rename `gameos_graphics.cpp:6928`** — same
- [ ] **Step 6: Rename `gos_particle_bridge.cpp:161`** — same
- [ ] **Step 7: Rename `gos_static_prop_batcher.cpp:549`** — same
- [ ] **Step 8: Rename `gos_static_prop_batcher.cpp:581`** — same
- [ ] **Step 9: Rename `gos_static_prop_batcher.cpp:3002`** — same
- [ ] **Step 10: Rename `gos_mech_batcher.cpp:267`** — same

Each rename: read the file, locate the line, change the literal string, verify no other consumer expects the old name.

### Task 16: Retire bare-`mvp` lookups in terrain set (3 sites per spec §4.1 #12,#15,#22-24)

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp:1759` — delete `thinTerrainLocs_.mvp = glGetUniformLocation(shp, "mvp");`
- Modify: `GameOS/gameos/gameos_graphics.cpp:3960` — delete `locs.mvp = glGetUniformLocation(shp, "mvp");`
- KEEP: `gameos_graphics.cpp:1795` — `shadowLocs_.mvp` (legitimate shadow consumer, allowlisted per §6.2.1)

- [ ] **Step 1: Delete terrain bare-mvp lookups**

Read each site, delete the lookup line. Also delete the corresponding struct member `mvp` from `thinTerrainLocs_` and `locs` (terrain locs) if no other consumer.

### Task 17: Rename `setMat4Direct` / `setMMat4Direct` strings (2 sites per spec §4.1 #25-26)

- [ ] **Step 1: Rename `gameos_graphics.cpp:2177`** — `setMat4Direct("terrainMVP", wMvpWaterNonMdi)` → `setMat4Direct("u_worldToClipGL", wMvpWaterNonMdi)`
- [ ] **Step 2: Rename `gameos_graphics.cpp:2337`** — `setMMat4Direct("terrainMVP", wMvpWaterMdi)` → `setMMat4Direct("u_worldToClipGL", wMvpWaterMdi)`

### Task 18: Add `MC2_DISABLE_GOSFX=0` runtime guard + CLAUDE.md known issue + smoke env assert

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` — add runtime warning per spec §10.2
- Modify: `.claude/worktrees/nifty-mendeleev/CLAUDE.md` — Known issues entry per spec §10.1
- Modify: `scripts/run_smoke.py` — env assert per spec §10.3

- [ ] **Step 1: Add runtime guard to gameosmain.cpp**

Add near other env-var probes:
```cpp
{
    const char* gosfxEnv = getenv("MC2_DISABLE_GOSFX");
    if (gosfxEnv != nullptr && strcmp(gosfxEnv, "0") == 0) {
        fprintf(stderr,
            "[UNIFIED_PROJ v1] WARN: MC2_DISABLE_GOSFX=0 active under unified "
            "projection. gosFX particles will render incorrectly (MLR clipper "
            "uses stale convention). See CLAUDE.md Known Issues.\n");
    }
}
```

- [ ] **Step 2: Add Known Issues entry to worktree CLAUDE.md**

Append to "Known issues (current)" section:

```markdown
- **gosFX dev-override broken under unified projection.** Running with
  `MC2_DISABLE_GOSFX=0` after F1 ship will render gosFX particles wrong:
  MLR's `mlrclipper.cpp:206-209,305,321,347` reads of `cameraToClip(2,2)`
  and `(3,2)` use stale MC2-pixel-homogeneous convention while the runtime
  uniforms drive shaders via the new GL convention. Default
  `MC2_DISABLE_GOSFX=1` (gate-dead MLR work-leaves) is unaffected.
  Runtime guard prints `[UNIFIED_PROJ v1] WARN:` stderr once per startup.
  Dev-override path re-enabled when MLR retirement Slices 1-5 ship.
```

- [ ] **Step 3: Add smoke env assert to run_smoke.py**

Locate env-allowlist setup in `scripts/run_smoke.py`. Add:
```python
# F1 unified-projection: forbid MC2_DISABLE_GOSFX=0 in tier1 smoke.
# Visual regression accepted only in dev-override sessions; tier1 must
# represent shipped default state.
if os.environ.get("MC2_DISABLE_GOSFX") == "0":
    print("[run_smoke] FATAL: MC2_DISABLE_GOSFX=0 conflicts with unified "
          "projection; smoke would record regressed visuals. Unset or set =1.")
    sys.exit(2)
```

### Task 19: Add CI grep gate script + allowlist

**Files:**
- Create: `scripts/check-unified-projection-retirement.sh`
- Create: `scripts/check-unified-projection-retirement.allowlist`

- [ ] **Step 1: Write grep gate script**

```bash
#!/bin/sh
# F1 unified-projection retirement gate (spec §6.2.1 + §6.2.2).
# Fails CI if legacy projection scaffolding survives Stage A.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ALLOWLIST="$SCRIPT_DIR/check-unified-projection-retirement.allowlist"

# Uniform name retirement (terrainMVP renamed to u_worldToClipGL).
echo "Checking terrainMVP retirement..."
HITS_UNIFORM=$(grep -RE "terrainMVP|u_terrainMVP|gos_SetTerrainMVP" code GameOS mclib shaders 2>/dev/null || true)
# Filter allowlist
HITS_UNIFORM_FILTERED=$(echo "$HITS_UNIFORM" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)

# Round-trip kludge retirement.
echo "Checking abs(clip.w) kludge retirement..."
HITS_ABS=$(grep -RE "abs\(\s*[a-zA-Z_]+\.w\s*\)" shaders 2>/dev/null || true)
HITS_ABS_FILTERED=$(echo "$HITS_ABS" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)

echo "Checking terrainViewport retirement..."
HITS_VIEWPORT=$(grep -RE "terrainViewport|u_terrainViewport" code GameOS mclib shaders 2>/dev/null || true)
HITS_VIEWPORT_FILTERED=$(echo "$HITS_VIEWPORT" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)

echo "Checking screen-rhw round-trip retirement..."
HITS_RHW=$(grep -RE "\*\s*rhw\s*[\+\-\*]" shaders 2>/dev/null || true)
HITS_RHW_FILTERED=$(echo "$HITS_RHW" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)

# Bare mvp retirement in terrain set.
echo "Checking bare mvp uniform retirement (terrain set only)..."
HITS_MVP=$(grep -RE "\buniform[[:space:]]+mat4[[:space:]]+u?_?mvp\b" shaders 2>/dev/null || true)
HITS_MVP_FILTERED=$(echo "$HITS_MVP" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)

FAIL=0
for hits in "$HITS_UNIFORM_FILTERED" "$HITS_ABS_FILTERED" "$HITS_VIEWPORT_FILTERED" "$HITS_RHW_FILTERED" "$HITS_MVP_FILTERED"; do
    if [ -n "$hits" ]; then
        echo "FAIL: unexpected legacy projection scaffolding:"
        echo "$hits"
        FAIL=1
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "Either: (a) finish migration of the flagged sites, or (b) add to"
    echo "$ALLOWLIST with a # UNIFIED_PROJ_ALLOWLIST: <reason> marker."
    exit 1
fi

echo "PASS: unified-projection retirement complete."
```

- [ ] **Step 2: Write allowlist file**

Create `scripts/check-unified-projection-retirement.allowlist` with the initial entries from spec §6.2.1:

```
# F1 unified-projection retirement allowlist.
# Format: substring patterns to filter from grep hits.
# Each entry must have a documented reason.

# Mitigation (c) — MLR's cameraToClip/worldToClipMatrix composition stays:
mclib/mlr/

# Bare mvp legitimate consumers (HUD + shadow set):
shaders/gos_text.vert
shaders/gos_vertex.vert
shaders/gos_vertex_lighted.vert
shaders/gos_tex_vertex.vert
shaders/shadow_terrain.vert
shaders/particle_billboard.vert:43
GameOS/gameos/gameos_graphics.cpp:1795
```

(Plan-stage refines if `particle_billboard.vert:43` ends up NOT retained.)

- [ ] **Step 3: Make script executable + test it succeeds on current working tree**

```bash
chmod +x scripts/check-unified-projection-retirement.sh
sh scripts/check-unified-projection-retirement.sh
```
Expected: PASS (after Tasks 10-17 land in working tree).

If FAIL: investigate the flagged hits. Either migrate the file or add to allowlist with justified reason.

### Task 20: Build clean + tier1 smoke + AMD shader review

- [ ] **Step 1: Full clean build**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
# Per CLAUDE.md: when class layout / static state changes, --clean-first
cmake --build build64 --config RelWithDebInfo --target mc2 --clean-first -- /m 2>&1 | Select-Object -Last 10
```
Expected: clean build. Any link error indicates a missed migration (caller of retired `gos_SetTerrainMVP` etc.).

- [ ] **Step 2: Deploy shaders + exe lockstep**

```powershell
# Per CLAUDE.md shader/exe lockstep rule:
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe
# Deploy all shader files (NOT cp -r per CLAUDE.md):
foreach ($f in (Get-ChildItem shaders -Recurse -File)) {
    $rel = $f.FullName.Substring((Resolve-Path shaders).Path.Length + 1)
    $dst = "A:/Games/mc2-opengl/mc2-win64-v0.4/data/shaders/$rel"
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    Copy-Item -Force $f.FullName $dst
}
```

- [ ] **Step 3: Run tier1 5/5 stock (no probe build flag)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```
Expected: exit 0. No new visual regressions vs pre-F1 baseline.

- [ ] **Step 4: Run CI grep gate**

```bash
sh scripts/check-unified-projection-retirement.sh
```
Expected: PASS.

- [ ] **Step 5: Dispatch `/mc2-amd-shader-review` skill**

Run the AMD shader review skill verbatim per worktree CLAUDE.md. The skill validates: reverse-Z + ZERO_TO_ONE + ClipControl interaction; nested `floatBitsToUint`/`atomicMax` AMD-compiler quirk (Task 6 un-nested per spec gemini #3); `GL_FALSE` upload + repackage on AMD 7900 XTX.

- [ ] **Step 6: User-driven canaries**

User drives mc2_10 wolfman zoom 60s. Verify:
- Red-diagonal-band class still caught (CPU pz gate in quad.cpp stays alive in F1)
- Screen-spanning-triangle class status (may retire under new convention; user reports)
- No new visual regressions

- [ ] **Step 7: Commit Stage A atomic flip**

```bash
git add -A
git commit -m "feat(unified-proj): F1 atomic flip — single composition + clean GL convention

ATOMIC: collapses 4 dup compositions to Camera::worldToClipGL();
renames terrainMVP -> u_worldToClipGL across 14 vert + 3 compute
shaders + 10 CPU bind sites; retires screen->pixel->NDC round-trip
+ abs(clip.w) kludge + bare-mvp + terrainViewport from terrain set;
deletes SSAO runtime entirely; adds MC2_DISABLE_GOSFX=0 triple-guard;
adds CI grep gate.

Out of F1 (per spec §8):
- Camera::projectZ body + 8 wrappers (own retirement arcs)
- CPU pz gate in quad.cpp (red-band class blocker, deferred)
- MLR (mitigation (c) — dev-override regression accepted)
- TGL class-statics (0us per F3; cleanup if surfaces)
- Editor (separate worktree)

Verified:
- Stage A-pre tier1 5/5: max_delta_comparable < 1e-3,
  nonfinite_new_only = 0, nonfinite_both = 0
- Stage A-pre mc2_10 wolfman canary: gate pass; R-clipw ratio < 0.001
- Stage A tier1 5/5 post-flip: exit 0
- AMD shader review: pass
- User canary mc2_10: no new regressions
- CI grep gate: pass (allowlist documented)

Spec: docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-atomic-design.md (v2.8 greybeard-signed META-FIX)"
```

---

## Phase 4 — Post-flip validation

### Task 21: Tier1 verification + golden-frame capture

- [ ] **Step 1: Tier1 5/5 30s post-commit**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```
Expected: exit 0. No regressions.

- [ ] **Step 2: Golden-frame capture per spec §6.4**

User runs golden-frame infra with cursor-drift disabled + camera-settle (isArmed) discipline across tier1 5/5 missions. Compare against pre-F1 baseline.

- [ ] **Step 3: Document Stage A evidence**

Create `docs/observations/2026-05-22-unified-proj-stage-a-shipped.md` with:
- Stage A commit hash
- Tier1 result
- Golden-frame delta summary
- AMD shader review verdict
- User canary results

- [ ] **Step 4: Commit observation doc**

```bash
git add docs/observations/2026-05-22-unified-proj-stage-a-shipped.md
git commit -m "docs(unified-proj): Stage A ship evidence

Tier1 5/5 pass, golden-frame delta within tolerance,
AMD shader review pass, user canaries clean.

Spec: §6.2 + §6.4"
```

---

## Self-review

**Spec coverage:** Every spec section maps to a task —
- §0 invariants → Tasks 1-3 (basis-vector test) + §2.1.1 transpose preserved by Task 4 repackage
- §2.1 axisSwap literal → Task 2
- §2.2 GL convention → Stage A canonical template (Task 11)
- §3 data flow → Tasks 4-7 (producer + cache + bind sites)
- §4.1 27-site census → Tasks 10-17
- §4.3 14 vert shaders → Tasks 10-11
- §4.4 3 compute/frag → Task 12
- §4.4 ssao delete → Task 13
- §5.1 probe → Tasks 6-7
- §5.2 Stage A atomic → Tasks 10-20
- §6.1 + §6.2 verification → Tasks 8-9, 20
- §6.2.1 + §6.2.2 grep + per-shader validation → Task 19 + Task 20 Step 4
- §6.4 golden-frame → Task 21 Step 2
- §7 R-clipw → Task 9 Step 2 explicit sub-gate
- §10 mitigation (c) → Task 18

**Placeholder scan:** No "TBD" / "implement later" / "add appropriate error handling" / etc. Every step has executable content. Two intentional plan-stage decisions remain (SSBO binding slot number in Task 6 Step 1; particle_billboard.vert `u_mvp` retention in Task 11 Step 13) — both documented as "plan-stage decides" with explicit criteria. NOT placeholders; they are bounded design decisions the implementer makes by grep evidence.

**Type consistency:** `gos_SetWorldToClipGL` signature `(GLuint program, const Stuff::Matrix4D& mat)` consistent across Tasks 4, 5, 14. `u_worldToClipGL` GLSL uniform name consistent across Tasks 6, 10, 11, 12. `kAxisSwapMC2toGL` consistent in Task 2.

Ready for execution.

---

**Plan complete and saved to `docs/superpowers/plans/2026-05-22-unified-projection-v2-f1-atomic-plan.md`.**
