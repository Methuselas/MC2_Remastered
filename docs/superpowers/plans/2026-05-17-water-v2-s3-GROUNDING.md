# Water v2 S3 -- Plan-stage Rule-0 Grounding Addendum (V1-V7 closed)

Status: V1-V7 CLOSED. Conflict resolved (shader advisor correct, adversarial sonnet wrong).

This document is the line-reference SOURCE OF TRUTH for plan Tasks 1-3. It supersedes the
spec/plan's `wave1`/`wave2`-based perturbation default (now known WRONG -- those symbols are
dead unreachable code for water). Task 1 MUST use this document's perturbation GLSL, not the
spec's.

All file:line citations below were grep-verified at write-time against the working tree at:

- Branch: `claude/water-material-v1`
- HEAD: `037665c4e089ecf5dfc98ff08e348d1494ac75c2`
- Worktree: `A:/Games/mc2-opengl-src/.claude/worktrees/water-material-v1/`

Line numbers drift; the symbols are stable. Every later task MUST re-grep the symbol before
editing -- the file:line values here are accurate as of the HEAD above only.

---

## V1 -- Step 1: Atlas accessor symbols (gos_terrain_indirect.cpp)

Defined ONLY in `GameOS/gameos/gos_terrain_indirect.cpp` (no header declaration -- grep of
`GameOS/gameos/*.h` for `gos_terrain_indirect_getAtlas` returned ZERO matches). The comment
at `gos_terrain_indirect.cpp:1022` explicitly states "Bridge accessors -- declared extern in
gameos_graphics.cpp."

| Symbol | Return type | Definition line | Header decl |
|---|---|---|---|
| `gos_terrain_indirect_getAtlasGLTex()` | `GLuint` | gos_terrain_indirect.cpp:1023 | NONE (.cpp only) |
| `gos_terrain_indirect_getAtlasMapTopLeftX()` | `float` | gos_terrain_indirect.cpp:1025 | NONE (.cpp only) |
| `gos_terrain_indirect_getAtlasMapTopLeftY()` | `float` | gos_terrain_indirect.cpp:1026 | NONE (.cpp only) |
| `gos_terrain_indirect_getAtlasOneOverWorldUnits()` | `float` | gos_terrain_indirect.cpp:1027 | NONE (.cpp only) |

(Also present, not required: `gos_terrain_indirect_getNumTexturesAcross()` -> float @ :1024.)

**Task 2 directive (V1):** these accessors are NOT in `gos_terrain_indirect.h`, so Task 2's
C++ work in `renderWaterFastPath` MUST add a local `extern` block declaring these four
functions. The existing precedent `extern` blocks are in SIBLING terrain-solid functions
in `gameos_graphics.cpp` at lines 2584-2588, 2877-2881, and 3100 -- NOT in
`renderWaterFastPath`. Mirror that exact extern-block idiom (e.g. lines 2584-2588):

```cpp
extern GLuint gos_terrain_indirect_getAtlasGLTex();
extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
```

NOTE: `gos_terrain_indirect.h` IS already `#include`d by `gameos_graphics.cpp` (line 33),
so `gos_terrain_indirect::IsFrameSolidArmed()` (declared in the header, see V7) is callable
WITHOUT an extern -- but the atlas accessors above are NOT in that header and DO need the
extern block.

---

## V1 -- Step 2: Atlas globals, build, filter/wrap, world-addr params, teardown

All in `GameOS/gameos/gos_terrain_indirect.cpp`.

- `g_atlasGLTex` declaration: line 664 (`static GLuint g_atlasGLTex = 0;`)
- `g_atlasMapTopLeftX` decl: line 667 ; `g_atlasMapTopLeftY` decl: line 668 ;
  `g_atlasOneOverWorldUnits` decl: line 669 (all `static float ... = 0.f;`)
- `BuildColormapAtlas()` body range: **lines 778-818** (signature at 778; closing brace
  after the `traceOn()` print block ending at 817).

Texture upload / filter / wrap (verbatim, lines 795-801):

```cpp
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
             tcm->cpuColorMapSize, tcm->cpuColorMapSize,
             0, GL_BGRA, GL_UNSIGNED_BYTE, tcm->cpuColorMap);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
```

CONFIRMED: MIN/MAG = `GL_LINEAR` (lines 798-799), WRAP_S/T = `GL_CLAMP_TO_EDGE`
(lines 800-801). NO MIP -- there is no `glGenerateMipmap` for `g_atlasGLTex` anywhere; the
header-style comment at gos_terrain_indirect.cpp:833 explicitly notes "glGenerateMipmap
would bleed neighboring cells. Sampler is GL_LINEAR" (no mip by design).

Three world-addr param assignments (verbatim, lines 806-808):

```cpp
g_atlasMapTopLeftX       = Terrain::mapTopLeft3d.x;
g_atlasMapTopLeftY       = Terrain::mapTopLeft3d.y;
g_atlasOneOverWorldUnits = Terrain::oneOverWorldUnitsMapSide;
```

Teardown (R1 no-stale-window claim) -- in the per-mission Reset function, lines 1151-1160:

```cpp
if (g_atlasGLTex != 0) {
    glDeleteTextures(1, &g_atlasGLTex);
    g_atlasGLTex = 0;
}
g_atlasSize              = 0;
g_atlasNumTexturesAcross = 0.f;
g_atlasMapTopLeftX       = 0.f;
g_atlasMapTopLeftY       = 0.f;
g_atlasOneOverWorldUnits = 0.f;
```

CONFIRMED (R1): teardown zeroes the GL handle (lines 1152-1155) AND all three world-addr
params (lines 1158-1160) in the SAME function -- no stale window where the handle is gone
but params still point at a freed atlas. The accessors are read-only getters; a torn-down
atlas returns handle 0 + zeroed params atomically per mission.

---

## V3 -- Step 3: Colormap UV lines (shaders/gos_terrain.frag)

Uniform decls: `useAtlasColormap` (int) @ frag:61, `atlasMapTopLeftX` (float) @ frag:62,
`atlasMapTopLeftY` (float) @ frag:63, `atlasOneOverWorldUnits` (float) @ frag:64.

The two UV lines, verbatim character-for-character (`shaders/gos_terrain.frag` lines
345-346, inside the `if (useAtlasColormap != 0) {` block opened at line 344):

```glsl
        colormapUV.x = (WorldPos.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;
        colormapUV.y = (atlasMapTopLeftY - WorldPos.y) * atlasOneOverWorldUnits;
```

CONFIRMED: X is NOT flipped (`WorldPos.x - atlasMapTopLeftX`); Y IS inverted
(`atlasMapTopLeftY - WorldPos.y`). Surrounding context (lines 343-350):

```glsl
    PREC vec2 colormapUV;
    if (useAtlasColormap != 0) {
        colormapUV.x = (WorldPos.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;
        colormapUV.y = (atlasMapTopLeftY - WorldPos.y) * atlasOneOverWorldUnits;
    } else {
        colormapUV = Texcoord;
    }
    PREC vec4 texColor = texture(tex1, colormapUV);
```

**Task 1 directive (V3):** Task 1's FS colormap-sample block in
`gos_terrain_water_mdi.frag` MUST replicate these two lines byte-identically with `wp`
substituted for `WorldPos.xy` (i.e. `wp.x` for `WorldPos.x`, `wp.y` for `WorldPos.y`),
preserving the X-not-flipped / Y-inverted asymmetry exactly.

---

## V5 -- Step 4 (CRITICAL): Water surface shader control flow + CONFLICT RESOLUTION

File: `shaders/gos_terrain_water_mdi.frag`. `void main(void)` at line 70.

### Conflict resolution: SHADER ADVISOR (opus) was RIGHT; adversarial sonnet was WRONG.

Grep-verified control-flow evidence (all line numbers verified at write-time):

| Element | Line | Verbatim |
|---|---|---|
| `if (o_isWater == 1) {` (LIVE water branch open) | 72 | `    if (o_isWater == 1) {` |
| `return;` that ends the live water branch | 109 | `        return;` |
| closing `}` of the live water branch | 110 | `    }` |
| `if (o_isWater == 2) discard;` (legacy spray suppress) | 115 | `    if (o_isWater == 2) discard;` |
| `if (shore <= 0.0) discard;` (in-branch overdraw kill) | 90 | `        if (shore <= 0.0) discard;` |
| `FragColor = vec4(col, shore);` (INJECTION POINT, before this) | 107 | `        FragColor = vec4(col, shore);` |
| FIRST `wave1` appearance | 131 | inside a SECOND `if (o_isWater == 1)` at line 129 |
| FIRST `wave2` appearance | 133 | (same dead tail) |
| ONLY `waveNormal` appearance | 153 | inside the `else` (o_isWater==2 detail) of the dead tail |

**Verdict:** the live `o_isWater == 1` branch is lines 72-110 and ends in `return;` at
line **109**. The `wave1`/`wave2` symbols FIRST appear at lines **131/133**, which is
AFTER line 109 -- inside a SECOND, unreachable `if (o_isWater == 1)` at line 129 (after
the legacy `if (o_isWater == 2) discard;` at line 115 and a `PREC vec4 c = Color.bgra;`
verbatim-legacy block at 117-158). Since the live branch already executed `return;` at
line 109 for water, lines 117-165 are DEAD UNREACHABLE code for water. `109 < 131` is the
decisive ordering.

- Adversarial sonnet claimed `wave1`/`wave2` exist at ~132-136 INSIDE the reachable
  `o_isWater==1` branch and proposed
  `vec3 waveNormal = normalize(vec3(wave1*0.06, wave2*0.06, 1.0));`. **FALSE** -- those
  lines are at 131-138, after the `return;` at 109; they are in dead legacy code. The
  `vec3 waveNormal = normalize(vec3(wave1 * 0.06, wave2 * 0.06, 1.0));` they cite is
  literally line 153, inside the doubly-dead o_isWater==2 `else` branch.
- Shader advisor (opus) found the live branch is ~72-110 ending in `return;` at ~109,
  with `if (o_isWater==2) discard;` at ~115, and `wave1`/`wave2`/`waveNormal` only at
  ~131-155 (after the return, dead). The live branch's noise is a scalar fBm `nz` at ~84
  built from `q0`/`q1` (~82-83). **CONFIRMED** -- the only deltas vs the advisor's
  approximate line numbers are minor drift (return is :109 not :109 exactly as estimated;
  discard is :115; wave* :131-155). The reachability premise holds exactly.

### In-scope symbols at the injection point (immediately before `FragColor` @ line 107, inside `o_isWater==1`, before `return;` @ 109)

| Symbol | Type | Line | Meaning | Camera input? |
|---|---|---|---|---|
| `viewVec` | vec3 | 75 | `cameraPos.xyz - WorldPos` | YES (distance-only; feeds waveLOD only) |
| `waveLOD` | float | 76 | `1.0 - smoothstep(WAVE_FADE_NEAR, WAVE_FADE_FAR, length(viewVec))` distance fade | indirectly (distance, not angle) |
| `sc` | float | 81 | `time * WAVE_SPEED * WAVE_FREQ` (scroll amount) | no |
| `q0` | vec2 | 82 | `WorldPos.xy * WAVE_FREQ + vec2(1.00,0.60)*sc` | NO |
| `q1` | vec2 | 83 | `WorldPos.xy * WAVE_FREQ * 1.70 + vec2(-0.80,-1.10)*sc` | NO |
| `nz` | float | 84 | `(fbm3(q0) + fbm3(q1)) - 0.875` -- scalar zero-mean organic HEIGHT fBm | NO (f(WorldPos,time) only) |
| `trans` | float | 86 | absorption transmittance | no |
| `waterCol` | vec3 | 87 | depth-mixed water color | no |
| `shore` | float | 89 | shoreline smoothstep alpha | no |
| `vertexLightRGB` | vec3 | 92 | un-swizzled vertex light | no |
| `col` | vec3 | 93 | accumulating output color | no |
| `crest` | float | 100 | `max(nz, 0.0)` | no |
| `glint` | float | 102 | `smoothstep(GLINT_THRESH, 0.80, nz)` | no |
| `WorldPos` | in vec3 | 22 (varying decl) | surface world pos | n/a |
| `cameraPos` | uniform vec4 | 31 | MC2 world-space camera | n/a (uniform, in scope) |

### NOT in scope at the injection point (DEAD tail, after the `return;` at line 109)

`wave1` (first at :131), `wave2` (first at :133), `waveNormal` (only at :153),
`wuv` (:127), `c` (:117), `tex_color` (:118). These are ALL in dead legacy code for water
and MUST NOT be referenced by Task 1's injected block.

### Confirmations

- `WorldPos` in-scope: YES (in vec3, decl line 22; used at 75/82/83).
- `cameraPos` in-scope: YES (uniform vec4, decl line 31).
- `waveLOD` in-scope: YES (line 76) -- usable to scale the reflection contribution.
- Sampler decls: ONLY `uniform sampler2D tex1;` (line 27, unit 0) and
  `uniform sampler2D tex2;` (line 28, unit 1). NO `sampler2D` bound to unit 2.
  **Texture unit 2 is FREE** for the colormap atlas in Task 1/2.

---

## V5 -- Step 6: Perturbation normal (FINAL AUTHORITATIVE GLSL)

### Independent verification of the advisor verdict's reachability premise

The advisor verdict relies on the in-scope scalar fBm `nz`. Grep-verified by control flow:

- `nz` is defined at `gos_terrain_water_mdi.frag:84` as `(fbm3(q0) + fbm3(q1)) - 0.875`.
- `q0` (:82) = `WorldPos.xy * WAVE_FREQ + vec2(1.00,0.60) * sc`.
- `q1` (:83) = `WorldPos.xy * WAVE_FREQ * 1.70 + vec2(-0.80,-1.10) * sc`.
- `sc` (:81) = `time * WAVE_SPEED * WAVE_FREQ`.
- `fbm3` (decl ~:64-67) consumes only its vec2 arg; `h21`/`vnoise` (~:52-61) likewise.

`q0`/`q1`/`nz` are functions of `WorldPos.xy` and `time` ONLY. NO `cameraPos`, NO
`viewVec`, NO view/projection term feeds `q0`/`q1`/`nz`. `viewVec` (:75) feeds ONLY
`waveLOD` (:76) -- a distance term -- and is NOT consumed by `q0`/`q1`/`nz`. The file
header comment at line 41 independently asserts "f(WorldPos,time) only" for this block,
and line 80 asserts "Camera-INDEPENDENT." Both are corroborated by the control-flow grep.

CONCLUSION: The advisor's reachability + camera-independence premise is CONFIRMED by
independent grep. NOT contradicted. The verdict is adopted. NOT BLOCKED.

### FINAL authoritative perturbation GLSL (verbatim -- Task 1 uses THIS, not the spec's)

```glsl
const float REFL_WAVE_SLOPE = 0.05;   // tunable; visual-loop range 0.02-0.10, start 0.05
vec2  nzGrad     = clamp(vec2(dFdx(nz), dFdy(nz)), -2.0, 2.0);  // clamp prevents zoom-out over-distortion
vec3  waveNormal = normalize(vec3(nzGrad * REFL_WAVE_SLOPE, 1.0));
vec3  vdir       = normalize(cameraPos.xyz - WorldPos);
vec3  rdir       = reflect(-vdir, waveNormal);
```

Rationale (from the advisor verdict, verified): `nz` is a scalar HEIGHT field, not a slope
pair; its screen-space gradient (`dFdx`/`dFdy`) is the true tangent-plane perturbation,
producing a soft fuzzed smudge (the S3 goal) rather than a crisp sliding mirror.
`dFdx`/`dFdy(nz)` is screen-FOOTPRINT dependence (same benign class as the existing
`waveLOD = f(length(viewVec))` at line 76), NOT a camera-ANGLE term -- so it does not
violate the camera-independence ruling. Camera-independence PASS: `q0`/`q1`/`nz` have no
`cameraPos`/`viewVec` input (grep-confirmed above).

Tuning caveats recorded (authoritative): `REFL_WAVE_SLOPE` starts at **0.05** (NOT 0.06 --
the dead legacy 2-sine field had different noise statistics, so its 0.06 does not transfer
to the fBm `nz`). Too high -> per-pixel speckle (over-perturbed SSR failure). Too low ->
crisp mirror. The reflection contribution is already `*waveLOD`-scaled in the spec/plan mix
so it calms at zoom-out.

NOTE on the spec/plan superseded default: the spec/plan show
`vec3 waveNormal = normalize(vec3(wave1*0.06, wave2*0.06, 1.0));`. That is WRONG --
`wave1`/`wave2` are dead unreachable symbols for water (lines 131/133, after the `return;`
at 109). Task 1 MUST use the `nzGrad`-based GLSL above. This document supersedes the spec.

---

## V1/V2/V4/V6/V7 -- Step 5: renderWaterFastPath + stream readiness

File: `GameOS/gameos/gameos_graphics.cpp`. `gosRenderer::renderWaterFastPath` signature
at lines 2047-2067 (definition body opens at 2068; early-out at 2069:
`if (!water_fast_prog_ || !water_fast_prog_->shp_ || recordCount == 0) return;`).

### mdiValid branch (V2/V4)

- `mdiValid` computed: lines 2251-2254 (`gpuArmed && s_waterMdiProg != 0 && s_waterMdiProg
  != ~0u && s_perCmdSsbo != 0 && baseTex != 0`).
- `if (mdiValid) {` opens at line **2256**. The MDI block (uniform setup, SSBO binds,
  texture binds, draw, restore) runs to ~line 2365 (the `glBindSampler(0, savedSampler);`
  + depth restore at 2363-2365 inside the branch). The `glMultiDrawArraysIndirect` water
  draw call is at line **2346** (Task 2 brackets this call).
- Per-draw SSBO upload (WaterPerCmd cmds[2]) at lines 2264-2269.

### setM* lambda definitions (V6 location-tolerance)

All defined inside the `if (mdiValid)` block, lines 2275-2298:

```cpp
auto setMF = [&](const char* name, float a) {
    GLint loc = glGetUniformLocation(s_waterMdiProg, name);
    if (loc >= 0) glUniform1f(loc, a);
};                                              // lines 2275-2278
auto setMI = [&](const char* name, int a) {
    GLint loc = glGetUniformLocation(s_waterMdiProg, name);
    if (loc >= 0) glUniform1i(loc, a);
};                                              // lines 2279-2282
auto setMVec4 = [&](const char* name, const float* v) {
    GLint loc = glGetUniformLocation(s_waterMdiProg, name);
    if (loc >= 0) glUniform4fv(loc, 1, v);
};                                              // lines 2291-2294
auto setMVec2 = [&](const char* name, float a, float b) {
    GLint loc = glGetUniformLocation(s_waterMdiProg, name);
    if (loc >= 0) glUniform2f(loc, a, b);
};                                              // lines 2295-2298
```

(Also `setMMat4Direct` @ 2283-2286 and `setMMat4Std` @ 2287-2290.)

CONFIRMED (V6): every `setM*` lambda guards `if (loc >= 0)` before the `glUniform*` call.
**Task 2/3 add NO extra location guard** -- adding a uniform that the shader does not yet
declare is location-tolerant; the lambda silently skips an absent uniform (loc == -1).

### Unit-1 sampler save/bind/post-draw force-0 idiom (V4 -- the verbatim idiom Task 2 mirrors for unit 2)

The unit-1 sampler save (lines 2329-2334):

```cpp
GLuint savedSampler1 = 0;
{
    GLint q = 0;
    glGetIntegeri_v(GL_SAMPLER_BINDING, 1, &q);
    savedSampler1 = (GLuint)q;
}
```

Bind + texture set (lines 2335-2341):

```cpp
glBindSampler(1, s_waterFastSampler);
glActiveTexture(GL_TEXTURE0);
glBindTexture(GL_TEXTURE_2D, baseTex);
glActiveTexture(GL_TEXTURE1);
GLuint detailOrBase = (detailTex != 0) ? detailTex : baseTex;
glBindTexture(GL_TEXTURE_2D, detailOrBase);
glActiveTexture(GL_TEXTURE0);
```

Post-draw texture-force-0 + sampler restore (lines 2350-2353):

```cpp
glActiveTexture(GL_TEXTURE1);
glBindTexture(GL_TEXTURE_2D, 0);
glActiveTexture(GL_TEXTURE0);
glBindSampler(1, savedSampler1);
```

**Task 2 directive (V4):** mirror this exact idiom for texture unit **2** (the free unit
per V5 Step 4): `glGetIntegeri_v(GL_SAMPLER_BINDING, 2, &q)` save, `glBindSampler(2, ...)`,
`glActiveTexture(GL_TEXTURE2)` + `glBindTexture(GL_TEXTURE_2D, atlasGLTex)`, and the
post-draw `glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
glActiveTexture(GL_TEXTURE0); glBindSampler(2, savedSampler2);` restore. The unit-0 save
idiom precedent is also at lines 2237-2243 (`glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q)`).

### V1 callability + namespace qualification

- `IsFrameSolidArmed()` is declared in `GameOS/gameos/gos_terrain_indirect.h` at line 414
  (`bool IsFrameSolidArmed();`) inside `namespace gos_terrain_indirect {` (opened at
  header line 49, closed at line 445).
- Exact qualification: **`gos_terrain_indirect::IsFrameSolidArmed()`**.
- `gameos_graphics.cpp` already `#include`s `gos_terrain_indirect.h` at line 33, so
  `gos_terrain_indirect::IsFrameSolidArmed()` is directly callable from
  `renderWaterFastPath` with NO extern. (The atlas accessors -- V1 Step 1 -- are NOT in
  that header and still require the local extern block.)

### V7 -- water-readiness criteria + R1 independence premise

File: `GameOS/gameos/gos_terrain_water_stream.cpp`. `IsFrameSolidArmed` is read here at:

- Line 1410: `gos_terrain_indirect::IsFrameSolidArmed() ? gos_terrain_indirect_getDispatchMvp16() : gos_GetTerrainMVPMat4()`
  -- MVP SELECTION ONLY (dispatch MVP vs live MVP, the 2026-05-17 water-consistency fix).
- Line 1454: `const bool armed = gos_terrain_indirect::IsFrameSolidArmed();` -- inside the
  env-gated `MC2_WATER_DEPTHPROBE` discriminator (probe-only, silent by default).

`ComputeDispatchAndBindThinRecords(float frameCos)` is defined at line 1226.

CONFIRMED (V7 / R1): in `gos_terrain_water_stream.cpp`, `IsFrameSolidArmed()` is read ONLY
for (a) MVP selection (line 1410) and (b) the env-gated probe (line 1454). It is NOT a
water-readiness gate. Water readiness is `IsReady()` (line 1455) / `GetRecipeCount()`
(line 1456) -- independent of solid-armed. The R1 independence premise (water producer
readiness does not depend on terrain-solid being armed) HOLDS.

### Probe print style (Task 3 must match)

`gameos_graphics.cpp` uses RAW `printf(...)` + `fflush(stdout)` for normal lifecycle
events and `fprintf(stderr, ...)` + `fflush(stderr)` for errors/first-draw diagnostics.
NO project log macro. Verbatim examples:

- Normal event (line 2088-2089):
  ```cpp
  printf("[WATER_MDI v1] event=prog_compiled prog=%u\n", (unsigned)s_waterMdiProg);
  fflush(stdout);
  ```
- Error (line 2094-2095):
  ```cpp
  fprintf(stderr, "[WATER_MDI v1] event=prog_compile_fail — MDI path disabled\n");
  fflush(stderr);
  ```
- First-draw diagnostic (lines 2401-2411): `fprintf(stderr, "[WATER_FAST v1]
  event=first_draw prog=%u recipeBuf=%u ... key=value\n", ...);` + `fflush(stderr);`

Marker schema: `[SUBSYS v<N>] event=<name> key=value ...`. Task 3 MUST use raw
`printf`/`fprintf` + explicit `fflush`, NOT a log macro, and a `[WATER_*  v1]`-style
schema-versioned marker, gated by a `static const bool` env guard
(`getenv("MC2_...") != nullptr`) as the existing probes do.

---

## Summary of directives carried into Tasks 1-3

1. **Task 1 (FS):** add the colormap-sample block to `gos_terrain_water_mdi.frag` INSIDE
   the `o_isWater == 1` branch, before `FragColor = vec4(col, shore);` at line 107 (before
   the `return;` at 109). Replicate `gos_terrain.frag:345-346` byte-identically with `wp`
   for `WorldPos.xy`. Use the `nzGrad`-based perturbation GLSL from V5 Step 6 (NOT the
   spec's dead `wave1`/`wave2`). Sample the atlas via a new `uniform sampler2D` on unit 2.
2. **Task 2 (C++):** in `renderWaterFastPath`, add the local `extern` block for the four
   atlas accessors (precedent: gameos_graphics.cpp:2584-2588). Bind the atlas GL texture +
   set the three world-addr uniforms + the `useAtlasColormap`-equivalent for the water
   shader. Mirror the unit-1 sampler save/bind/force-0 idiom (lines 2329-2353) for unit 2,
   bracketing the `glMultiDrawArraysIndirect` at line 2346. No extra `loc >= 0` guard
   (the setM* lambdas already guard, lines 2275-2298).
3. **Task 3 (probe):** raw `printf`/`fprintf` + `fflush`, `static const bool` env guard,
   `[WATER_* v1] event=... key=value` schema. No log macro.

CONFLICT OUTCOME: shader advisor (opus) correct. The S3 perturbation uses the screen-space
gradient of the in-scope scalar fBm `nz`, NOT the dead `wave1`/`wave2` sine field. NOT
BLOCKED -- the reachability + camera-independence premise was independently grep-confirmed.

## S6 prep recon

A water-specific arm predicate DOES exist and water arms independently of the
solid path. `IsFrameMaskWaterArmed()` is declared at
`GameOS/gameos/gos_terrain_mask_dispatch.h:55` and defined at
`GameOS/gameos/gos_terrain_mask_dispatch.cpp:170-175`; it returns
`s_readyThisFrame && env(MC2_TERRAIN_MASK_DISPATCH_WATER) != "0"`. Its solid
sibling `IsFrameMaskSolidArmed()` (`gos_terrain_mask_dispatch.cpp:163-168`)
gates on the separate `MC2_TERRAIN_MASK_DISPATCH_SOLID` killswitch -- the two
buckets share only the per-frame `s_readyThisFrame` mask-dispatch readiness
flag (set by `BuildAndUploadMasksForFrame`, reset by `BeginFrame`) and are
otherwise independently env-gated, so water can arm while solid is disabled
and vice-versa. This is a DIFFERENT subsystem from `IsFrameSolidArmed()`
(`gos_terrain_indirect.cpp:2203-2205`, `s_frameSolidArmed &&
!s_processArmingDisabled`), which is the indirect-SOLID arming used by the
`quadSetupTextures` loop's water-narrow append predicate
(`mclib/terrain.cpp:1790`). The water-fast-path narrow walk gates on
`WaterStream::NarrowEnabled()` (`gos_terrain_water_stream.cpp:156-157`), a
third independent env predicate. Net: S6 has a real per-bucket water arm
predicate available (`IsFrameMaskWaterArmed`) and need not overload
`IsFrameSolidArmed`; this informs (does not gate) the eventual S6 design.
