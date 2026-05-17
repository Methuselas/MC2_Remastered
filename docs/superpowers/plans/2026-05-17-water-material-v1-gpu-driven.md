# Water Material v1 (GPU-Driven, terrain-side thickness) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace flat textured water on the armed MDI path with a stylized material (depth absorption, Fresnel, procedural normals, continuous shoreline) driven entirely by terrain-side elevation data already GPU-resident.

**Architecture:** Water thickness = `max(0, waterElevation - velev)` computed in the MDI water vertex shader from the mission-static recipe SSBO (`.elev`) and the existing `waterElevation` uniform; emitted as a new varying. The fragment shader does Beer-Lambert absorption + Fresnel + procedural-normal specular on the base layer only. No depth-buffer read, no UBO, no reflection/refraction. The detail/spray layer and every band uniform are preserved byte-for-byte (the MDI program serves both base and detail draws with one uniform set).

**Tech Stack:** GLSL 4.30 (MDI, `GL_ARB_shader_draw_parameters`), OpenGL 4.3 core, C++ (gosRenderer), tier1 smoke harness, env-gated CPU probe.

**Spec:** `docs/superpowers/specs/2026-05-17-water-material-v1-gpu-driven-design.md` (rev 3, cleared 2 adversarial rounds).

**Codebase note:** This is a shader/render change. There is no unit-test framework for GLSL here; the project's regression gate is the tier1 smoke harness plus an env-gated CPU probe plus a manual visual checklist. Task verification steps reflect that, not pytest. All file:line below were grep-verified at write-time; grep the symbol if a line has drifted (Rule 0).

**Atomicity:** Task 1 (VS) and Task 2 (FS) change the VS/FS varying interface together. The program only links at runtime; do not run the game between Task 1 and Task 2. The build+smoke gate is Task 5, after both shader sides and both C++ changes are in.

---

### Task 1: Vertex shader - emit thickness + world position, move base alpha off the staircase

**Files:**
- Modify: `shaders/gos_terrain_water_fast_mdi.vert` (out varyings ~58-61; new emits after ~195; base argb ~233)

- [ ] **Step 1: Add the two new out varyings**

In `shaders/gos_terrain_water_fast_mdi.vert`, the current block (grep `flat out int o_isWater`):

```glsl
out vec4  Color;
out vec2  Texcoord;
out float FogValue;
flat out int o_isWater;
```

Change to:

```glsl
out vec4  Color;
out vec2  Texcoord;
out float FogValue;
flat out int o_isWater;
out float WaterThickness;   // water-v1: world-unit column (max(0, waterElevation - terrain floor))
out vec3  WorldPos;         // water-v1: wave-displaced surface position (Fresnel view vector ONLY)
```

- [ ] **Step 2: Emit WaterThickness and WorldPos right after the surface position is computed**

The current lines (grep `vec3 worldPos = vec3(vxy, wz);`):

```glsl
    float wz = waveOurCos(waterBits) + waterElevation;
    vec3 worldPos = vec3(vxy, wz);
```

Change to:

```glsl
    float wz = waveOurCos(waterBits) + waterElevation;
    vec3 worldPos = vec3(vxy, wz);

    // water-v1 LOAD-BEARING Z INVARIANT: thickness uses velev (terrain FLOOR);
    // WorldPos carries the wave-displaced SURFACE and is for the Fresnel view
    // vector ONLY. The two Z values are intentionally different - do not unify,
    // do not derive thickness from worldPos.z (this is the rev-1 trap class).
    WaterThickness = max(0.0, waterElevation - velev);
    WorldPos       = worldPos;
```

(`velev` is computed immediately above at `float velev = cornerElev(rec, cornerIdx);` - grep `float velev =` to confirm it precedes this point. It does in the current file.)

- [ ] **Step 3: Move the base-layer alpha off the elevation-band staircase (the rev-2 C1 fix)**

The current argb branch (grep `if (detailMode == 0) {`):

```glsl
    uint elevAlphaByte = elevAlphaBandByte(velev);
    uint argb;
    if (detailMode == 0) {
        uint lrgb = cornerLightRGB(trec, cornerIdx);
        argb = (lrgb & 0x00FFFFFFu) | (elevAlphaByte << 24);
    } else {
        argb = (elevAlphaByte << 24) | 0x00FFFFFFu;
    }
```

Change ONLY the base (`detailMode == 0`) alpha source to an opaque sentinel; leave `elevAlphaByte` computed and the `else` (detail) branch byte-for-byte unchanged:

```glsl
    uint elevAlphaByte = elevAlphaBandByte(velev);   // KEPT: detail branch still consumes it
    uint argb;
    if (detailMode == 0) {
        uint lrgb = cornerLightRGB(trec, cornerIdx);
        // water-v1: base-layer alpha is now owned by the FS shore term, not the
        // elevation-band staircase. Opaque sentinel; FS writes the real alpha.
        argb = (lrgb & 0x00FFFFFFu) | 0xFF000000u;
    } else {
        argb = (elevAlphaByte << 24) | 0x00FFFFFFu;  // DETAIL/SPRAY: UNCHANGED
    }
```

Do NOT remove `elevAlphaBandByte`, do NOT touch the `debugMode` branches below, do NOT remove any uniform.

- [ ] **Step 4: Sanity-check the edit (no build yet - FS not done)**

Run: `git -C A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering diff --stat shaders/gos_terrain_water_fast_mdi.vert`
Expected: one file changed, roughly `+9 -2` (2 varyings, 4 emit lines incl. comment, 1 argb line changed, comments).

- [ ] **Step 5: Commit**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
git add shaders/gos_terrain_water_fast_mdi.vert
git commit -m "feat(water-v1): VS emit WaterThickness+WorldPos; base alpha off staircase"
```

---

### Task 2: Fragment shader - stylized base-water material, detail path verbatim

**Files:**
- Modify: `shaders/gos_terrain_water_mdi.frag` (in varyings ~17-20; new uniform; const block; `main()` restructure)

- [ ] **Step 1: Add the two matching in varyings and the cameraPos uniform**

Current (grep `flat in int o_isWater;`):

```glsl
in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;
flat in int o_isWater;

layout (location=0) out PREC vec4 FragColor;
layout (location=1) out PREC vec4 GBuffer1;

uniform sampler2D tex1;
uniform sampler2D tex2;
uniform PREC vec4 fog_color;
uniform PREC float time;          // seconds — used for water animation
```

Change to:

```glsl
in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;
flat in int o_isWater;
in PREC float WaterThickness;     // water-v1: world-unit column from VS
in PREC vec3  WorldPos;           // water-v1: surface pos (Fresnel view vector)

layout (location=0) out PREC vec4 FragColor;
layout (location=1) out PREC vec4 GBuffer1;

uniform sampler2D tex1;
uniform sampler2D tex2;
uniform PREC vec4 fog_color;
uniform PREC float time;          // seconds — used for water animation
uniform PREC vec4 cameraPos;      // water-v1: MC2 world-space camera (Fresnel)

// water-v1 baked style constants (compile-time; tune via shader hot-reload;
// promote to a UBO only at per-biome per spec Section 8 TODO(water-v2)).
const vec3  SHALLOW_COLOR      = vec3(0.22, 0.45, 0.38);
const vec3  DEEP_COLOR         = vec3(0.02, 0.08, 0.10);
const float ABSORPTION_DENSITY = 0.15;   // 1/world-units
const float SHORE_BLEND_DEPTH  = 3.0;    // world-units to full opacity
const float NORMAL_STRENGTH    = 0.30;
const float FRESNEL_F0         = 0.02;
const float SUN_INTENSITY      = 1.0;
```

- [ ] **Step 2: Prepend the base-water branch to `main()`, keep the rest verbatim**

Current `main()` begins (grep `void main(void)`):

```glsl
void main(void)
{
    PREC vec4 c = Color.bgra;
    PREC vec4 tex_color = (o_isWater <= 1) ? texture(tex1, Texcoord) : texture(tex2, Texcoord);
    c *= tex_color;
```

Insert the new base branch immediately after `void main(void)\n{` and BEFORE `PREC vec4 c = Color.bgra;`, so the existing body becomes the `o_isWater != 1` fallthrough untouched:

```glsl
void main(void)
{
    if (o_isWater == 1) {
        // ---- water-v1 stylized base layer ----
        PREC vec2 wuv = Texcoord * 6.2831853;
        PREC vec2 w = vec2(sin(wuv.y * 3.0 + time * 0.50) + sin(wuv.y * 1.2 + time * 0.355),
                           sin(wuv.x * 3.0 + time * 0.355) + sin(wuv.x * 1.2 + time * 0.50));
        PREC vec3 wN = normalize(vec3(w * NORMAL_STRENGTH, 1.0));

        PREC float t        = clamp(WaterThickness * ABSORPTION_DENSITY, 0.0, 1.0);
        PREC vec3  waterCol = mix(SHALLOW_COLOR, DEEP_COLOR, t);

        PREC float shore = smoothstep(0.0, SHORE_BLEND_DEPTH, WaterThickness);
        if (shore <= 0.0) discard;            // kill invisible land-quad overdraw

        PREC vec3  viewDir = normalize(cameraPos.xyz - WorldPos);
        PREC float ct      = max(dot(wN, viewDir), 0.0);
        PREC float fres    = FRESNEL_F0 + (1.0 - FRESNEL_F0) * pow(1.0 - ct, 5.0);

        PREC vec3  reflCol = fog_color.rgb * 1.4;   // sky/fog approx (no reflection pass v1)

        PREC vec3  lightDir = normalize(vec3(0.3, 0.2, 1.0));  // existing FS constant light
        PREC vec3  halfV    = normalize(viewDir + lightDir);
        PREC float spec     = pow(max(dot(wN, halfV), 0.0), 64.0) * fres;

        PREC vec3  vertexLightRGB = Color.bgra.rgb;  // VS packs .bgra (~241); un-swizzle here
        PREC vec3  col = mix(waterCol, reflCol, fres);
        col *= vertexLightRGB;
        col += SUN_INTENSITY * spec;
        if (fog_color.x > 0.0 || fog_color.y > 0.0 || fog_color.z > 0.0 || fog_color.w > 0.0)
            col = mix(fog_color.rgb, col, FogValue);

        FragColor = vec4(col, shore);
        GBuffer1  = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
        return;
    }

    PREC vec4 c = Color.bgra;
    PREC vec4 tex_color = (o_isWater <= 1) ? texture(tex1, Texcoord) : texture(tex2, Texcoord);
    c *= tex_color;
```

Everything from `PREC vec4 c = Color.bgra;` to the end of `main()` (the `#ifdef ALPHA_TEST`, the `if (o_isWater > 0)` wave block, the fog blend, `FragColor = c;`, `GBuffer1 = ...`) is left exactly as-is. The original `if (o_isWater == 1)` sub-branch inside `if (o_isWater > 0)` becomes unreachable dead code (harmless; detail path is byte-for-byte preserved).

- [ ] **Step 3: Verify the edit shape (no build yet)**

Run: `git -C A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering diff --stat shaders/gos_terrain_water_mdi.frag`
Expected: one file changed, roughly `+45 -0` (varyings, uniform, const block, new branch; no deletions - the old body is retained).

- [ ] **Step 4: Commit**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
git add shaders/gos_terrain_water_mdi.frag
git commit -m "feat(water-v1): FS stylized base-water material; detail path verbatim"
```

---

### Task 3: C++ - push cameraPos to the MDI water program (one per-frame uniform)

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (MDI uniform-set block, after the `fog_color` setter ~2314)

- [ ] **Step 1: Add the cameraPos setter in the existing MDI lambda block**

Current (grep `setMVec4      ("fog_color", (const float*)&fog_color_);`):

```cpp
        setMF         ("time",  (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0));
        setMVec4      ("fog_color", (const float*)&fog_color_);
        setMI         ("tex1",  0);
        setMI         ("tex2",  1);
```

Change to:

```cpp
        setMF         ("time",  (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0));
        setMVec4      ("fog_color", (const float*)&fog_color_);
        setMVec4      ("cameraPos", (const float*)&terrain_camera_pos_);  // water-v1 Fresnel
        setMI         ("tex1",  0);
        setMI         ("tex2",  1);
```

`terrain_camera_pos_` is a `vec4` member of this class (grep `vec4 terrain_camera_pos_;`), MC2 world space, same space as the VS `worldPos`. The `(const float*)&` cast matches the existing `setMVec4` calls (e.g. `terrainViewport`, `fog_color`) and the codebase pattern at the other `glUniform4fv(.., (const float*)&terrain_camera_pos_)` sites. The `setMVec4` lambda no-ops if the uniform is absent (loc < 0), so this is safe even if a debug variant strips Fresnel.

- [ ] **Step 2: Commit (build happens in Task 5 with everything together)**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(water-v1): push cameraPos uniform to MDI water program"
```

---

### Task 4: CPU probe - prove the thickness data path is live ([WATER_MAT v1])

**Files:**
- Modify: `GameOS/gameos/gos_terrain_water_stream.cpp` (in `WaterStream::Build`, right after `g_ready = true;` ~314)

This probe is the project's positive-marker gate (a smoke PASS alone does not verify a GPU-driven shader change). It uses a NEW env var so the retained `[WATER_DEPTHPROBE v2]` MVP instrument on `MC2_WATER_DEPTHPROBE` is undisturbed.

- [ ] **Step 1: Add the env-gated probe**

Current (grep `g_ready = true;` in this file - it is immediately before `if (DebugOn()) {`):

```cpp
    g_ready = true;

    if (DebugOn()) {
```

Change to:

```cpp
    g_ready = true;

    // [WATER_MAT v1] positive-marker probe (env MC2_WATER_MATERIAL_PROBE; SEPARATE
    // from the retained [WATER_DEPTHPROBE v2] MVP instrument - do not share its env).
    // Recomputes the VS thickness formula CPU-side over the populated recipes so a
    // smoke can assert the elevation path is live (max > 0), not a flat unbound read.
    {
        static const bool s_waterMatProbe =
            (getenv("MC2_WATER_MATERIAL_PROBE") != nullptr);
        if (s_waterMatProbe && !g_recipes.empty()) {
            float tmin = 1e30f, tmax = -1e30f;
            for (const WaterRecipe& r : g_recipes) {
                float floorMin = r.v0e;
                floorMin = (r.v1e < floorMin) ? r.v1e : floorMin;
                floorMin = (r.v2e < floorMin) ? r.v2e : floorMin;
                floorMin = (r.v3e < floorMin) ? r.v3e : floorMin;
                float thick = (float)Terrain::waterElevation - floorMin;
                if (thick < 0.0f) thick = 0.0f;
                if (thick < tmin) tmin = thick;
                if (thick > tmax) tmax = thick;
            }
            fprintf(stderr,
                    "[WATER_MAT v1] event=summary recipes=%zu thickness_min=%.3f "
                    "thickness_max=%.3f waterElevation=%.3f\n",
                    g_recipes.size(), (double)tmin, (double)tmax,
                    (double)Terrain::waterElevation);
            fflush(stderr);
        }
    }

    if (DebugOn()) {
```

(`Terrain::waterElevation` is already referenced in this function's `DebugOn()` block just below - grep `(double)Terrain::waterElevation` to confirm it is in scope here. It is. `getenv` is already used in this TU - grep `getenv("MC2_WATER_UPLOAD_NARROW"` - so no new include is needed.)

- [ ] **Step 2: Commit**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
git add GameOS/gameos/gos_terrain_water_stream.cpp
git commit -m "feat(water-v1): [WATER_MAT v1] CPU thickness probe (distinct env)"
```

---

### Task 5: Build, deploy, gate (smoke + probe + visual)

**Files:** none modified. This is the regression gate for all of Tasks 1-4.

- [ ] **Step 1: Full-relink build (load-bearing functions changed in 2 TUs)**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
rm -f build64/RelWithDebInfo/mc2.exe
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```

Expected: build succeeds; `build64/RelWithDebInfo/mc2.exe` exists and is newly timestamped. If the build fails, fix the reported error before proceeding - do not deploy a stale exe.

- [ ] **Step 2: Deploy per-file (never cp -r)**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe
cp -f shaders/gos_terrain_water_fast_mdi.vert A:/Games/mc2-opengl/mc2-win64-v0.4/shaders/gos_terrain_water_fast_mdi.vert
cp -f shaders/gos_terrain_water_mdi.frag      A:/Games/mc2-opengl/mc2-win64-v0.4/shaders/gos_terrain_water_mdi.frag
diff -q shaders/gos_terrain_water_fast_mdi.vert A:/Games/mc2-opengl/mc2-win64-v0.4/shaders/gos_terrain_water_fast_mdi.vert
diff -q shaders/gos_terrain_water_mdi.frag      A:/Games/mc2-opengl/mc2-win64-v0.4/shaders/gos_terrain_water_mdi.frag
```

Expected: every `diff -q` prints nothing (files identical). Any difference = deploy failed; re-copy that file.

- [ ] **Step 3: Run the tier1 smoke with the probe enabled**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
MC2_WATER_MATERIAL_PROBE=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\gpu-driven-rendering\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing
```

Expected: process exit code `0`. Nonzero = inspect `tests/smoke/artifacts/<latest>/`.

- [ ] **Step 4: Verify the positive probe marker fired with a live thickness range**

Run (point at the newest artifact dir):

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
grep -h "\[WATER_MAT v1\]" tests/smoke/artifacts/*/mc2_*.log | tail -10
```

Expected: at least one line `[WATER_MAT v1] event=summary recipes=<N> thickness_min=... thickness_max=<M> ...` with `thickness_max` strictly > 0 and `recipes` > 0. `thickness_max == 0` or marker absent = the elevation path is dead (flat/unbound read) - FAIL, do not claim success; diagnose before continuing.

- [ ] **Step 5: Verify the MVP instrument is undisturbed**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
MC2_WATER_DEPTHPROBE=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\gpu-driven-rendering\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --fail-fast
grep -h "\[WATER_DEPTHPROBE v2\]" tests/smoke/artifacts/*/mc2_*.log | tail -5
```

Expected: smoke exit `0`; `[WATER_DEPTHPROBE v2]` lines still present and report MVP equal on motion frames (instrument behaves exactly as before this change).

- [ ] **Step 6: Manual visual checklist (tier1 PASS is necessary, not sufficient)**

The smoke window is user-driven and visible. Confirm by observation:
- Continuous shoreline fade; the 3-band staircase is gone on the base water layer; no waterline flicker on zoom/elevation change.
- Deep water visibly darker than shallow near-shore (absorption over real world-unit thickness).
- Cinematic low/grazing camera angle: Fresnel rim visibly brighter than at the oblique default.
- Detail/spray layer (`o_isWater==2`) looks unchanged versus baseline.
- Un-armed intro pan: legacy flat water still draws (no fallback regression).
- Zoomed-out big-map: capture a Tracy GPU water-zone sample; base FS is heavier per fragment (Fresnel pow5, sines, specular pow64) - confirm the water zone is not worse than ~2x the pre-change MDI water FS at the same camera. Measure; do not assume cost-neutral.

- [ ] **Step 7: Final gate commit (artifacts/notes only if the harness produces tracked output)**

If the smoke harness left tracked artifact summaries that the repo expects committed, stage and commit them:

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
git status --porcelain
# commit only intended tracked outputs; do NOT git add tests/smoke/artifacts if it is gitignored
```

If nothing tracked changed, no commit - the feature commits are Tasks 1-4.

---

## Self-Review

**1. Spec coverage:**
- Terrain-side thickness mechanic (spec Sec 3): Task 1 Steps 2-3, Task 2 Step 2. Covered.
- Floor-vs-surface Z invariant (spec Sec 3 LOAD-BEARING): Task 1 Step 2 comment + separate `velev`/`WorldPos` usage. Covered.
- Shading: absorption/Fresnel/normals/specular/reflective term (spec Sec 4): Task 2 Step 2. Covered.
- Composite + shared-program C1 fix, detail verbatim (spec Sec 5): Task 1 Step 3 (sentinel, band uniforms kept) + Task 2 Step 2 (prepend, fallthrough verbatim). Covered.
- VS/FS interface +2 varyings atomic (spec Sec 6): Task 1 Step 1 + Task 2 Step 1; atomicity note in header. Covered.
- Files changed (spec Sec 7): Tasks 1-4 match the spec's 4-file list exactly. Covered.
- Compile-time consts + UBO TODO (spec Sec 8): Task 2 Step 1 const block + TODO comment. Covered.
- Distinct-env probe + build/deploy/smoke gate (spec Sec 9): Task 4 + Task 5. Covered.
- Load-bearing constraints (spec Sec 10): band uniforms kept (Task 1 Step 3), MVP instrument undisturbed (Task 5 Step 5), no depth touch, no recipe field add. Covered.
- Plan-stage advisor tuning (spec Sec 11): constants ship at spec defaults; tuning of `ABSORPTION_DENSITY`/`SHORE_BLEND_DEPTH` is a hot-reload visual-iteration follow-up noted in Task 2 const comment and Task 5 Step 6. Covered (no code gap).

**2. Placeholder scan:** No TBD/TODO-as-work; the only `TODO(water-v2)` is an intentional in-code forward-compat seam per spec. All code steps contain complete code. No "similar to Task N".

**3. Type consistency:** `WaterThickness` (float), `WorldPos` (vec3), `cameraPos` (vec4, `.xyz` used) consistent across VS out / FS in / C++ setter. `o_isWater==1` base / `!=1` fallthrough consistent with the verified current FS selector `(o_isWater <= 1)`. `terrain_camera_pos_` cast matches existing `setMVec4` call sites. No mismatches.

No gaps found.
