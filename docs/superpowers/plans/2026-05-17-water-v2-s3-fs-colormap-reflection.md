# Water v2 S3 - Pure-FS Reflected-Ray Colormap Reflection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a believable terrain-color reflection to the armed MDI water by marching a reflected view ray in the water fragment shader against the engine's whole-map world-addressable colormap atlas.

**Architecture:** Pure-fragment-shader reflection (zero new pass/FBO/CPU geometry, zero blocked ring/fence/dispatch contact). The water FS samples the existing `g_atlasGLTex` colormap atlas along `reflect(-vdir, waveNormal)` in world XY, Fresnel-weighted, mixed into the S1 color. C++ side adds 3 atlas uniforms + `reflTex`/`reflectionOn` + a conditional unit-2 texture bind in `gosRenderer::renderWaterFastPath()`. A mandatory `reflectionOn = IsFrameSolidArmed() && getAtlasGLTex()!=0` gate handles the atlas-absent-while-water-armed window (the load-bearing R1 fallback).

**Tech Stack:** OpenGL 4.3 GLSL, MSVC RelWithDebInfo, isolated worktree + dedicated `mc2-win64-water` deploy mirror, kill-aware `mc2_01` smoke harness.

**Spec:** `docs/superpowers/specs/2026-05-17-water-v2-s3-fs-colormap-reflection-design.md` (read it first; this plan implements it verbatim).

**Verification model (replaces TDD - no GLSL unit harness exists):** every code task ends by building/deploying to the **isolated** mirror and confirming an observable gate: shader compiles clean (`0(N): error` ABSENT from the engine log - hot-reload fails silently), then the `[WATER_REFL v1]` probe shows `reason=on`, then the user visual loop. Never claim a visual result; the smoke window is user-driven.

**Hard project rules (every task obeys):** no emoji in any file; no wall-clock projections; grep-verify every file:line at write-time (Rule 0); deploy ONLY to `A:/Games/mc2-opengl/mc2-win64-water/`, NEVER `mc2-win64-v0.4/`; build ALWAYS `--config RelWithDebInfo`; full relink when load-bearing C++ changes; branch stays isolated (user integrates separately).

---

### Task 0: Plan-stage Rule-0 grounding (close spec V1-V7)

Line numbers in the spec are advisor-grounded but drift. This task records the **current** exact symbols/lines so every later task edits the right place. No code change; produces a grounding addendum committed for the executor.

**Files:**
- Create: `docs/superpowers/plans/2026-05-17-water-v2-s3-GROUNDING.md`

- [ ] **Step 1: Re-grep the atlas accessors (V1)**

Run:
```
cd A:/Games/mc2-opengl-src/.claude/worktrees/water-material-v1
grep -nE "gos_terrain_indirect_getAtlas(GLTex|MapTopLeft[XY]|OneOverWorldUnits)" GameOS/gameos/gos_terrain_indirect.cpp GameOS/gameos/*.h
```
Record: each accessor's exact name + return type + the header (if any) it is declared in. If only defined in the .cpp, note that the C++ task must add a local `extern` block (sibling functions do this; the precedent bind sites are NOT in `renderWaterFastPath`).

- [ ] **Step 2: Re-grep the atlas sampler state + world-addr params (V1)**

Run:
```
grep -nE "g_atlasGLTex|BuildColormapAtlas|GL_CLAMP_TO_EDGE|GL_LINEAR|glTexImage2D|g_atlasMapTopLeft|g_atlasOneOverWorldUnits" GameOS/gameos/gos_terrain_indirect.cpp
```
Record: line of `g_atlasGLTex` decl, the `BuildColormapAtlas` body range, the exact min/mag filter + wrap calls (confirm `GL_LINEAR` + `GL_CLAMP_TO_EDGE`, no mip), and the three world-addr param assignments. Confirm teardown zeroes handle AND params in one call (R1).

- [ ] **Step 3: Re-grep the authoritative atlas-UV formula (V3)**

Run:
```
grep -nE "useAtlasColormap|colormapUV|atlasMapTopLeft|atlasOneOverWorldUnits" shaders/gos_terrain.frag
```
Record the EXACT two UV lines character-for-character (X = `(WorldPos.x - atlasMapTopLeftX) * atlasOneOverWorldUnits`, Y = `(atlasMapTopLeftY - WorldPos.y) * atlasOneOverWorldUnits` - **X not flipped, Y inverted**). Task 1's FS block must replicate these byte-identically with `wp` substituted for `WorldPos.xy`.

- [ ] **Step 4: Re-grep the water FS injection point + S1 symbols (V5)**

Run:
```
grep -nE "o_isWater|WorldPos|cameraPos|wave1|wave2|waveLOD|FragColor|uniform sampler2D|GBuffer1" shaders/gos_terrain_water_mdi.frag
```
Record: the exact `if (o_isWater == 1)` open line and its closing `return;`/brace, the line of `FragColor = vec4(col, shore);` inside that branch (the injection point is immediately before it), and confirm `wave1`, `wave2`, `waveLOD`, `WorldPos`, `cameraPos` all exist IN that branch. Confirm there is NO `waveNormal` in the `o_isWater==1` branch (only in the dead `==2` fallthrough). Record the existing sampler decls (confirm only `tex1`/`tex2` -> unit 2 free).

- [ ] **Step 5: Re-grep the C++ fold site (V1/V2/V4/V6/V7)**

Run:
```
grep -nE "renderWaterFastPath|mdiValid|setMF|setMI|setMVec2|setMVec4|GL_SAMPLER_BINDING|glBindSampler|IsFrameSolidArmed|invalidateRenderStateCache" GameOS/gameos/gameos_graphics.cpp
grep -nE "ComputeDispatchAndBindThinRecords|IsFrameSolidArmed" GameOS/gameos/gos_terrain_water_stream.cpp
```
Record: the `mdiValid` branch range in `renderWaterFastPath`, the `setM*` lambda definitions (confirm `if (loc >= 0)` location-tolerance - V6, no new guard needed), the exact unit-1 sampler save/restore + texture-force-0 lines (the idiom Task 2 mirrors for unit 2), and confirm `IsFrameSolidArmed()` + the atlas accessors are callable from that translation unit. Note the water-readiness criteria lines in `gos_terrain_water_stream.cpp` and that `IsFrameSolidArmed()` is read there only for MVP selection (V7 - the R1 independence premise).

- [ ] **Step 6: Route the perturbation-normal construction past the shader advisor (V5)**

Dispatch a general-purpose Agent told to: Read `.claude/agents/mc2-shader-expert.md` in full and adopt its Rule 0; then, given S1's `o_isWater==1` branch exposes scalar `wave1`/`wave2` (record their meaning/units from Step 4) but NO normal, decide whether the S3 reflected-ray perturbation normal should be `normalize(vec3(wave1*REFL_WAVE_SLOPE, wave2*REFL_WAVE_SLOPE, 1.0))` (spec default, mirrors dead `==2` line ~153) or reflect about flat `vec3(0,0,1)` then perturb `rdir.xy` post-hoc - on the criterion "which makes the wave fuzz read as a believable smudge without a second camera term." Record the verdict + the exact GLSL into the grounding addendum; Task 1 uses it.

- [ ] **Step 7: Write the grounding addendum + commit**

Write `docs/superpowers/plans/2026-05-17-water-v2-s3-GROUNDING.md` with every recorded actual (symbol, file:line, the verbatim UV lines, the chosen perturbation GLSL, the unit-1 idiom lines). This file is the source of truth for Tasks 1-3 line references.

```
git add docs/superpowers/plans/2026-05-17-water-v2-s3-GROUNDING.md
git commit -m "docs(water-v2/S3): plan-stage Rule-0 grounding addendum (V1-V7 closed)"
```

---

### Task 1: Fragment-shader reflection block

**Files:**
- Modify: `shaders/gos_terrain_water_mdi.frag` (the `o_isWater==1` branch, injection point + new uniforms + REFL_* consts; exact lines per GROUNDING Step 4)

- [ ] **Step 1: Add the new uniforms + REFL_* consts**

Near the existing `uniform sampler2D tex1; tex2;` block and the S1 const block (lines per GROUNDING Step 4), add:

```glsl
uniform sampler2D reflTex;            // unit 2: whole-map colormap atlas
uniform int   reflectionOn;           // 0 -> skip entire S3 block
uniform float atlasMapTopLeftX;
uniform float atlasMapTopLeftY;
uniform float atlasOneOverWorldUnits;

const int   REFL_STEPS      = 5;
const float REFL_STEP_LEN   = 96.0;   // ~one terrain-tile world distance
const float REFL_F0         = 0.02;
const float REFL_STRENGTH   = 0.35;
const float REFL_MAX        = 0.22;   // hard ceiling on the mix factor
const float REFL_WAVE_SLOPE = 0.05;   // nz-gradient -> perturbation slope (GROUNDING; range 0.02-0.10)
```

> **GROUNDING supersedes the old `wave1`/`wave2` default.** Task 0 proved
> (`2026-05-17-water-v2-s3-GROUNDING.md`, commit `e8332ff`) the live
> `o_isWater==1` branch ends `return;` at `:109`; `wave1`/`wave2`/`waveNormal`
> are DEAD code at `:131-153` and will not compile in scope. The in-scope
> noise is scalar fBm `nz` (`:84`, `f(WorldPos,time)`, no camera). Use the
> `nz`-gradient construction below (the addendum's authoritative GLSL), NOT
> any `wave1`/`wave2` form.

- [ ] **Step 2: Insert the S3 reflection block at the injection point**

Immediately BEFORE `FragColor = vec4(col, shore);` inside the `o_isWater==1` branch (exact line per GROUNDING Step 4), insert (using the perturbation-normal GLSL chosen in GROUNDING Step 6 - the default is shown):

```glsl
// S3: pure-FS reflected-ray terrain-colormap reflection.
// The ONLY camera-dependent term in the water material (v2 ruling).
if (reflectionOn == 1) {
    // S1 has no normal in this branch (scalar fBm). Derive the perturbation
    // from the screen-space gradient of the in-scope scalar fBm nz (:84,
    // f(WorldPos,time)). clamp() prevents zoom-out over-distortion. This is
    // the GROUNDING-authoritative construction (supersedes wave1/wave2).
    vec2  nzGrad     = clamp(vec2(dFdx(nz), dFdy(nz)), -2.0, 2.0);
    vec3  waveNormal = normalize(vec3(nzGrad * REFL_WAVE_SLOPE, 1.0));
    vec3  vdir       = normalize(cameraPos.xyz - WorldPos); // sole cam-dep input
    vec3  rdir       = reflect(-vdir, waveNormal);
    vec3  acc  = vec3(0.0);
    float wsum = 0.0;
    for (int i = 1; i <= REFL_STEPS; ++i) {
        vec2 wp = WorldPos.xy + rdir.xy * (float(i) * REFL_STEP_LEN);
        vec2 uv;
        uv.x = (wp.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;  // X: not flipped
        uv.y = (atlasMapTopLeftY - wp.y) * atlasOneOverWorldUnits;  // Y: inverted
        float inb = step(0.0, uv.x) * step(uv.x, 1.0)
                  * step(0.0, uv.y) * step(uv.y, 1.0);
        acc  += inb * texture(reflTex, uv).rgb;
        wsum += inb;
    }
    vec3  refl = (wsum > 0.0) ? acc / wsum : col;        // all off-map -> no-op
    float fres = REFL_F0 + (1.0 - REFL_F0)
                 * pow(1.0 - max(vdir.z, 0.0), 5.0);
    col = mix(col, refl,
              clamp(fres * REFL_STRENGTH * waveLOD, 0.0, REFL_MAX));
}
```

GROUNDING (commit `e8332ff`) recorded the `nz`-gradient construction shown above as authoritative; use it verbatim. Confirm `nz` is in scope at the injection point from the addendum's V5 record before inserting.

- [ ] **Step 3: Deploy the shader to the isolated mirror**

```
cp -f shaders/gos_terrain_water_mdi.frag A:/Games/mc2-opengl/mc2-win64-water/shaders/gos_terrain_water_mdi.frag
diff -q shaders/gos_terrain_water_mdi.frag A:/Games/mc2-opengl/mc2-win64-water/shaders/gos_terrain_water_mdi.frag
```
Expected: `diff -q` prints nothing (files identical).

- [ ] **Step 4: Compile-gate (the "test" - shader hot-reload fails silently)**

The shader cannot be exercised until Task 2 wires the uniforms (without them `reflectionOn` is 0 -> block skipped -> identical to S1, which is the correct safe state). The gate for THIS task is that it compiles. Run the existing kill-aware smoke once so the engine compiles the shader, then grep the log:

```
# wait if any *v0.4* mc2.exe is running (priority session); do NOT --kill-existing it
MC2_SMOKE_MODE=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
LOG=$(ls -dt tests/smoke/artifacts/*/ | head -1)
grep -nE "0\([0-9]+\): error|gos_terrain_water_mdi" "$LOG"mc2_01*.log
```
Expected: a `gos_terrain_water_mdi` compile/link line present, and **NO** `0(N): error`. If an error appears, fix the GLSL and redeploy before proceeding (a silently-failed compile means the OLD shader is live).

- [ ] **Step 5: Commit**

```
git add shaders/gos_terrain_water_mdi.frag
git commit -m "feat(water-v2/S3): FS reflected-ray colormap reflection block (gated, no-op until uniforms wired)"
```

---

### Task 2: C++ uniform + conditional unit-2 texture fold

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (the `mdiValid` branch of `gosRenderer::renderWaterFastPath()`; exact lines per GROUNDING Step 5)

- [ ] **Step 1: Add the local extern accessor decls**

At the top of the `mdiValid` branch (per GROUNDING Step 5/V1), add a local `extern` block using the EXACT accessor signatures recorded in GROUNDING Step 1, e.g.:

```cpp
extern GLuint gos_terrain_indirect_getAtlasGLTex();
extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
```
(Use the recorded return types verbatim; if a header already exposes them at this site per GROUNDING Step 1, include that header instead and skip the redundant externs.)

- [ ] **Step 2: Resolve the handle once + compute the R1 gate**

In the same branch, before the `setM*` push block:

```cpp
GLuint reflTexHandle = gos_terrain_indirect_getAtlasGLTex();
int    reflOn = (gos_terrain_indirect::IsFrameSolidArmed()
                 && reflTexHandle != 0) ? 1 : 0;
```
(Use the exact `IsFrameSolidArmed` qualification recorded in GROUNDING Step 5.)

- [ ] **Step 3: Push the uniforms via the existing setM\* lambdas**

Alongside the existing `setMVec2("mapTopLeft",...)` / `setMVec4("cameraPos",...)` pushes (exact lambda names per GROUNDING Step 5):

```cpp
setMI("reflectionOn", reflOn);
setMI("reflTex", 2);
setMF("atlasMapTopLeftX",       gos_terrain_indirect_getAtlasMapTopLeftX());
setMF("atlasMapTopLeftY",       gos_terrain_indirect_getAtlasMapTopLeftY());
setMF("atlasOneOverWorldUnits", gos_terrain_indirect_getAtlasOneOverWorldUnits());
```
(`setM*` is location-tolerant per GROUNDING Step 5/V6 - no extra guard.)

- [ ] **Step 4: Conditional unit-2 bind mirroring the unit-1 idiom**

Using the EXACT unit-1 sampler-save / texture-force-0 lines recorded in GROUNDING Step 5 as the template, wrap the unit-2 bind so it only happens when `reflOn`:

```cpp
GLint savedSampler2 = 0;
if (reflOn) {
    glGetIntegeri_v(GL_SAMPLER_BINDING, 2, &savedSampler2);
    glBindSampler(2, 0);                       // use atlas per-texture-object state
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, reflTexHandle);
}
// ... existing water MDI draw ...
if (reflOn) {
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, 0);           // force-clear, mirrors unit-1 idiom
    glBindSampler(2, savedSampler2);           // restore the sampler only
}
```
When `reflOn == 0`: do NOT bind anything to unit 2 and do NOT issue `glBindTexture(...,0)` on it - leave it untouched (the shader skip is the single source of truth; binding 0 would mask a missing-atlas R1 bug). Place the pre-draw block immediately before, and the post-draw block immediately after, the existing `glMultiDrawArraysIndirect` water draw call (exact draw line per GROUNDING Step 5).

- [ ] **Step 5: Full relink build (load-bearing C++ change)**

`gameos_graphics.cpp` is load-bearing - incremental builds leak stale linkage. If `build64/` is not yet configured, copy the exact configure command verbatim from `docs/superpowers/specs/2026-05-17-water-material-v1-gpu-driven-design.md` (per handoff Section 2; build64 is normally already configured). Then:

```
rm -f build64/RelWithDebInfo/mc2.exe
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -30
```
Expected: build succeeds, `build64/RelWithDebInfo/mc2.exe` regenerated. If link errors reference the atlas accessors, the extern signatures in Step 1 do not match GROUNDING Step 1 - fix and rebuild.

- [ ] **Step 6: Deploy exe + shader to the isolated mirror**

```
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
cp -f shaders/gos_terrain_water_mdi.frag A:/Games/mc2-opengl/mc2-win64-water/shaders/gos_terrain_water_mdi.frag
diff -q shaders/gos_terrain_water_mdi.frag A:/Games/mc2-opengl/mc2-win64-water/shaders/gos_terrain_water_mdi.frag
```
Expected: both `diff -q` print nothing. NEVER deploy to `mc2-win64-v0.4/`.

- [ ] **Step 7: Commit**

```
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(water-v2/S3): wire colormap-atlas uniforms + conditional unit-2 bind in renderWaterFastPath (R1-gated)"
```

---

### Task 3: `[WATER_REFL v1]` env-gated probe with edge-detect latch

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (same `mdiValid` branch, right after the `reflOn` computation from Task 2 Step 2)

- [ ] **Step 1: Add the latched, reason-tagged probe**

Immediately after `reflOn` is computed (Task 2 Step 2), following the codebase's existing env-gated `[SUBSYS vN]` print pattern (match the macro/style recorded near other probes in this file):

```cpp
if (getenv("MC2_WATER_REFL_TRACE")) {
    static int s_lastReflOn = -1;            // mandatory edge-detect latch
    if (reflOn != s_lastReflOn) {
        const char* reason =
            !gos_terrain_indirect::IsFrameSolidArmed() ? "solid0" :
            (reflTexHandle == 0)                        ? "atlas0" : "on";
        printf("[WATER_REFL v1] event=state reflectionOn=%d atlas=%u reason=%s\n",
               reflOn, (unsigned)reflTexHandle, reason);
        fflush(stdout);
        s_lastReflOn = reflOn;
    }
}
```
(If the file uses a project logging macro rather than raw `printf` for `[SUBSYS vN]` probes, use that macro instead - per GROUNDING Step 5 record the existing probe style.)

- [ ] **Step 2: Full relink + deploy (per Task 2 Steps 5-6)**

```
rm -f build64/RelWithDebInfo/mc2.exe
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe && diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
```
Expected: clean build, identical deploy.

- [ ] **Step 3: Commit**

```
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(water-v2/S3): [WATER_REFL v1] env-gated probe (latched, reason enum)"
```

---

### Task 4: Integrated smoke gate (the slice's acceptance gate)

No code change. Proves the path is LIVE (not silent-fallback) before the user visual loop.

- [ ] **Step 1: Kill-aware probed smoke on the heavy water map**

Confirm no `*v0.4*` `mc2.exe` is running (priority session). If it is, WAIT (poll in the background); do NOT `--kill-existing` it and do NOT run concurrently. Then:

```
cd A:/Games/mc2-opengl-src/.claude/worktrees/water-material-v1
MC2_SMOKE_MODE=1 MC2_WATER_REFL_TRACE=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
```

- [ ] **Step 2: Marker-gate the latest artifact (exit-code-agnostic - a killed run can exit 0)**

```
LOG=$(ls -dt tests/smoke/artifacts/*/ | head -1)
grep -nE "\[SMOKE v1\] event=summary result=pass" "$LOG"mc2_01*.log
grep -nE "\[WATER_REFL v1\].*reason=on" "$LOG"mc2_01*.log
grep -nE "0\([0-9]+\): error" "$LOG"mc2_01*.log
```
Expected: line 1 present (run completed), line 2 present (`reason=on` -> S3 path live, atlas bound, NOT silent fallback), line 3 **absent** (clean shader compile). If `reason=on` is missing but `solid0`/`atlas0` is present, S3 fell back - investigate the R1 gate / atlas lifecycle before proceeding; do NOT tune constants on a fallen-back path.

- [ ] **Step 3: Hand off to the user visual tuning loop**

Report the three marker results to the user. The real gate is the USER's visual judgment in the user-driven smoke window: terrain colors visibly wash into the water from terrain-adjacent shorelines, correct from each camera angle when settled, fuzzed by S1 waves, fading to clean S1 water when distant/zoomed/un-armed, no off-map edge streak, S1 + camera-independence unregressed. The `REFL_*` consts (incl `REFL_WAVE_SLOPE`) are hot-reloadable: redeploy only the `.frag` between tweaks and re-grep `0(N): error` each redeploy. **If the reflection looks directionally reversed, the adversarial sign criterion applies: verify the `reflect(-vdir, waveNormal)` convention BEFORE flipping any sign or const (spec Section 4.2).** Never claim a visual result yourself; never ask the user to re-run.

---

## Self-Review

**1. Spec coverage:**
- Spec S4.1 C++ fold (extern, handle-once, reflOn gate, setM* uniforms, conditional unit-2 bind w/ sampler save + force-tex-0) -> Task 2 Steps 1-4. Covered.
- Spec S4.2 FS block (REFL_* incl REFL_WAVE_SLOPE, local waveNormal from wave1/wave2, asymmetric UV, in-bounds mask, Fresnel mix, reflectionOn guard) -> Task 1 Steps 1-2. Covered.
- Spec S4.3 graceful fallback (reflOn=0 -> skip, no unit-2 bind) -> Task 2 Step 4 + Task 1 guard. Covered.
- Spec S5 R1 (atlas absent while water armed) -> Task 2 Step 2 gate; verified in Task 0 Step 5 / Task 4 Step 2. R2 (asymmetric UV) -> Task 0 Step 3 + Task 1 Step 2. R3 (off-map streak) -> in-bounds mask Task 1 Step 2. R5/R6 named limitations -> no code, carried in spec. Covered.
- Spec S7 V1-V7 -> Task 0 Steps 1-6. Covered.
- Spec S8 gates (full relink, isolated deploy, compile-grep, latched probe w/ reason enum, marker-gated smoke) -> Tasks 1-4. Covered.
- Spec S6 Option B -> documentation-only, correctly NOT in any task. Covered.

**2. Placeholder scan:** No TBD/TODO. The one conditional ("if GROUNDING Step 6 chose the flat-normal variant") names the exact alternative and where its GLSL is recorded - a grounded branch, not a placeholder. The configure-command pointer is an exact reference to an existing verbatim command (handoff-sanctioned), not a fill-in.

**3. Type consistency:** `reflTexHandle` (GLuint), `reflOn` (int), uniform names `reflectionOn`/`reflTex`/`atlasMapTopLeftX`/`atlasMapTopLeftY`/`atlasOneOverWorldUnits` and consts `REFL_STEPS/STEP_LEN/F0/STRENGTH/MAX/WAVE_SLOPE` are identical across Task 1 (decl), Task 2 (push), Task 3 (probe). `setMF`/`setMI`/`setMVec2` lambda names are deferred to GROUNDING Step 5 actuals (not invented). Accessor signatures deferred to GROUNDING Step 1 actuals. Consistent.
