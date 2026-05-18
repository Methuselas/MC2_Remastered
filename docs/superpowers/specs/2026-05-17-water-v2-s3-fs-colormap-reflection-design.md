# Water v2 - Slice S3: Pure-FS Reflected-Ray Colormap Reflection (Option C)

**Date:** 2026-05-17
**Branch:** `claude/water-material-v1` (isolated worktree; keep-as-is, integrate later)
**Parent ruling:** `2026-05-17-water-v2-scope-and-decomposition.md` Section 3
(water base 100% camera-independent; S3 terrain reflection is the SOLE
camera-dependent term).
**Supersedes:** `2026-05-17-water-v2-s3-planar-reflection-design.md`
(in-pipeline second-dispatch design, BLOCKED x2 by dual adversarial - the
ring/fence/`g_dispatchMvp16` mutation findings). That document remains as the
historical record of why the in-pipeline approach is dead and why Option B
below is the deferred fidelity upgrade, NOT a revival of it.
**Status:** DESIGN - pending user spec review, then 2 adversarials.
**Grounding:** two code-grounded advisor passes this session (render-expert +
terrain-indirect-expert + shader-expert, Rule 0). All file:line below were
grep-verified 2026-05-17; symbols are stable, line numbers drift - the
plan-stage Rule-0 block (Section 7) re-confirms before implementation.

---

## 1. Goal and the lowered quality bar (user, 2026-05-17)

Add a believable terrain reflection in the armed MDI water. The user
explicitly set the bar: **"we don't need beautiful reflections, just a smudge
of 'oh hey it reflects the terrain a bit'."** Very few maps have terrain
adjacent to water; in most it is distant or off-screen. The reflection should
read correctly from any camera angle when the camera is settled, be fuzzed by
the existing S1 wave motion, and never regress S1 or the camera-independence
ruling.

This Fresnel-weighted terrain reflection is, per the ruling, the **ONLY**
camera-dependent term in the entire water material. S1's surface stays
`f(WorldPos, time)`.

**Non-goals (deferred, by design):**
- Crisp mirrored terrain geometry (silhouettes against sky, self-occlusion,
  parallax). This is Option B (Section 6) - the deferred fidelity upgrade.
- Reflecting mechs / vehicles / craters / dynamic shadows. That requires the
  per-frame second-geometry-pass machinery the dual adversarial BLOCKED;
  out of scope for water-v2 entirely.
- Wave-normal *distortion* beyond reusing the S1 normal to perturb the
  reflected ray (a later pure-FS refinement if wanted).
- S2 refraction stays deprioritised; S5 UBO demand-gated.

## 2. Why this is the smallest CORRECT slice (load-bearing)

Two reborn approaches were grounded against MC2's **oblique ~30deg cinematic
360 camera** (`memory/camera_model_oblique_cinematic.md`):

- **SSPR / screen-space (rejected):** at the oblique camera the terrain that
  should reflect sits beyond/behind the water plane, largely off-screen or
  occluded. Screen-space can only scatter on-screen pixels; it degrades to a
  thin near-shore smear with holes - "not a terrain reflection at all at this
  camera" (grounding verdict; matches the dead spec's own dual-adversarial
  note that screen-space "is viable only as a poor approximation, not the
  stated goal").
- **Option C (this spec):** the engine already builds a **single whole-map
  colormap atlas**, GPU-resident, addressable by world XY, valid for the
  whole mission. The water FS already has `WorldPos` and `cameraPos`. A short
  reflected-view-ray march in world XY sampling that atlas IS a
  camera-correct terrain-color reflection - computed per fragment, correct
  from every angle by construction, with ZERO new pass, ZERO FBO, ZERO new
  per-frame CPU geometry, ZERO invalidation logic, and ZERO contact with the
  blocked ring/fence/dispatch machinery. The only added CPU is a handful of
  uniform pushes + one texture bind in the existing `setM*` block (matching
  the `mapTopLeft`/`cameraPos` pushes already there - negligible, north-star
  compliant). Pure-FS reflection algorithm, hot-reloadable, same nature as S1.

Option C matches the user's stated bar exactly. It is a color wash, not a
crisp mirror - acceptable and intended.

## 3. The reflection source (grounded)

- **Atlas texture:** `static GLuint g_atlasGLTex` (`gos_terrain_indirect.cpp`
  ~`:664`), created/populated in `BuildColormapAtlas()`
  (~`:778-818`): one `glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,
  cpuColorMapSize,cpuColorMapSize,...)` from
  `Terrain::terrainTextures2->cpuColorMap`. **Single whole-map atlas**, one
  square texture covering the entire map. Sampler `GL_LINEAR` +
  **`GL_CLAMP_TO_EDGE`**, **no mips**.
- **Lifecycle:** built once per mission from `BuildDenseRecipe()`
  (~`:1061`), torn down per-mission (~`:1151-1160`). Stable for the whole
  mission once built.
- **World-addressing params** captured alongside it:
  `g_atlasMapTopLeftX/Y = Terrain::mapTopLeft3d.x/y`,
  `g_atlasOneOverWorldUnits = Terrain::oneOverWorldUnitsMapSide`
  (~`:806-808`). Exposed via `gos_terrain_indirect_getAtlasGLTex()` /
  `...getAtlasMapTopLeftX()` / `...Y()` / `...getAtlasOneOverWorldUnits()`
  (~`:1023-1027`), already `extern`-declared and called in
  `gameos_graphics.cpp` (the bind sites at ~`:2584/2877/3100`).
- **Authoritative UV reconstruction** (the terrain frag, the convention the
  water FS MUST replicate verbatim - `shaders/gos_terrain.frag` ~`:344-346`):
  ```
  colormapUV.x = (WorldPos.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;
  colormapUV.y = (atlasMapTopLeftY - WorldPos.y) * atlasOneOverWorldUnits;
  ```
  **X is not flipped; Y is inverted.** Asymmetric. A symmetric guess mirrors
  the reflection vertically (Risk R2).

## 4. Design

### 4.1 C++ fold - `gosRenderer::renderWaterFastPath()` (`gameos_graphics.cpp`)

In the `mdiValid` branch (~`:2251-2254`), inside the existing `setM*`
uniform-set lambda block (~`:2275-2298`, which already pushes
`setMVec2("mapTopLeft",...)` ~`:2309` and `setMVec4("cameraPos",...)`
~`:2315`):

- `setMF("atlasMapTopLeftX", gos_terrain_indirect_getAtlasMapTopLeftX())`,
  same for `atlasMapTopLeftY`, `atlasOneOverWorldUnits`.
- `setMI("reflTex", 2)`.
- Resolve the atlas handle ONCE, gate, and **only bind on the armed path**:
  ```
  GLuint reflTexHandle = gos_terrain_indirect_getAtlasGLTex();
  int    reflOn = (gos_terrain_indirect::IsFrameSolidArmed()
                   && reflTexHandle != 0) ? 1 : 0;
  setMI("reflectionOn", reflOn);
  if (reflOn) { /* bind reflTexHandle on unit 2 (idiom below) */ }
  else        { /* do NOT bind anything to unit 2: leave the unit
                    restored/untouched by the save-restore path */ }
  ```
  Rule: when `reflOn == 0`, the C++ side must NOT issue a `glBindTexture` of
  handle `0` (or anything) to unit 2. The shader skip already prevents
  sampling; binding `0` would make a missing-atlas bug look intentional and
  hide R1. The skip is the single source of truth; the bind is conditional
  on it.
- When `reflOn`, bind `reflTexHandle` to **texture unit 2** using
  the exact unit-1 save/restore idiom already present (~`:2329-2353`:
  `glGetIntegeri_v(GL_SAMPLER_BINDING,...)` -> `glBindSampler` ->
  `glActiveTexture`+`glBindTexture` -> draw -> restore unit + sampler).
  Units 0/1 are occupied (tex1/tex2, ~`:2316-2317`/`:2336-2340`); unit 2 is
  free in `s_waterMdiProg` (water FS declares only `tex1`/`tex2`,
  `gos_terrain_water_mdi.frag` ~`:27-28`). Restore the unit-2 binding AND its
  sampler exactly as the unit-1 pattern does.

No VS change: `WorldPos` is already a varying in the water FS
(`gos_terrain_water_mdi.frag` ~`:22`, used ~`:75/82/83`).

### 4.2 Fragment shader fold - `shaders/gos_terrain_water_mdi.frag`

`o_isWater==1` branch ONLY, immediately before `FragColor = vec4(col, shore);`.
This is the SOLE block in the entire material allowed to read `cameraPos`.

```glsl
// S3: pure-FS reflected-ray terrain-colormap reflection.
// The ONLY camera-dependent term in the water material (v2 ruling).
if (reflectionOn == 1) {
    vec3  vdir = normalize(cameraPos.xyz - WorldPos);     // sole cam-dep input
    vec3  rdir = reflect(-vdir, waveNormal);              // S1 normal fuzzes it
    vec3  acc  = vec3(0.0);
    float wsum = 0.0;
    for (int i = 1; i <= REFL_STEPS; ++i) {
        vec2 wp = WorldPos.xy + rdir.xy * (float(i) * REFL_STEP_LEN);
        vec2 uv;
        uv.x = (wp.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;  // X: not flipped
        uv.y = (atlasMapTopLeftY - wp.y) * atlasOneOverWorldUnits;  // Y: inverted
        float inb = step(0.0, uv.x) * step(uv.x, 1.0)
                  * step(0.0, uv.y) * step(uv.y, 1.0);   // off-map -> 0, no edge streak
        acc  += inb * texture(reflTex, uv).rgb;
        wsum += inb;
    }
    vec3  refl = (wsum > 0.0) ? acc / wsum : col;          // all off-map -> no-op
    float fres = REFL_F0 + (1.0 - REFL_F0)
                 * pow(1.0 - max(vdir.z, 0.0), 5.0);
    col = mix(col, refl,
              clamp(fres * REFL_STRENGTH * waveLOD, 0.0, REFL_MAX));
}
```

- `reflectionOn`/`reflTex`/`atlasMapTopLeftX`/`atlasMapTopLeftY`/
  `atlasOneOverWorldUnits` are new uniforms on `s_waterMdiProg`.
- `waveNormal` and `waveLOD` are the existing S1 quantities (reused, not
  recomputed); `waveLOD` is distance-based (NOT angle) - it keeps the
  reflection a distant/peripheral smudge and anti-aliases it. Reusing it does
  not introduce a second camera-dependent term (it is distance, already
  present for S1 LOD).
- Tunable hot-reload consts at the top of the FS (same regime as the S1
  consts). The spec owns the first guess (conservative - this is a wash, not
  a mirror; cap strength low):
  ```glsl
  const int   REFL_STEPS    = 5;
  const float REFL_STEP_LEN = 96.0;   // ~one terrain-tile world distance
  const float REFL_F0       = 0.02;
  const float REFL_STRENGTH = 0.35;
  const float REFL_MAX      = 0.22;   // hard ceiling on the mix factor
  ```
  Numbers move during the visual loop; these are the starting point so
  implementation does not invent them. Kept as separate named constants
  (different regimes stay distinct per the scattered-tuning-constants
  ruling).
- **Reflected-ray sign acceptance criterion (do NOT "fix" by intuition):**
  the march direction is the mirror ray's XY projection
  (`reflect(-vdir, waveNormal).xy`), NOT "toward shore". Because the
  algorithm ignores terrain height and samples colormap XY only, the visual
  direction can feel counterintuitive during tuning. If the first visual
  pass appears directionally reversed, the **adversarial review must verify
  the sign against the incident/reflection convention before anyone changes
  constants or flips a sign**. The convention here
  (`vdir = cameraPos - WorldPos`, `reflect(-vdir, n)`) is the physically
  correct one for "what the water reflects".

### 4.3 Graceful fallback (load-bearing, not optional)

`reflectionOn == 0` -> the entire block is skipped -> pixel-identical to the
current shipped S1 water. Triggered whenever terrain-solid is not armed or
the atlas handle is 0 (see Risk R1: the water MDI path arms independently of
`IsFrameSolidArmed()`). S3 never hard-fails the water draw and never samples
an absent/0 atlas. A black/neutral fallback texture is explicitly NOT used (at
grazing angles a black mix darkens water; the skip is correct).

## 5. Load-bearing constraints / risks (adversarial focus)

- **R1 (CRITICAL) Atlas absent while water armed.** `mdiValid` depends on
  `gpuArmed = ComputeDispatchAndBindThinRecords()` (`gameos_graphics.cpp`
  ~`:2193`), whose water-readiness criteria
  (`gos_terrain_water_stream.cpp` ~`:1542-1543`: recipe buffer + lighting
  SSBO + window count) do NOT require terrain-solid to be armed; that
  function only *reads* `IsFrameSolidArmed()` to pick an MVP (~`:1410`). The
  atlas guard (`ResourcesReady()`, `gos_terrain_indirect.cpp` ~`:1645-1656`)
  only protects the *terrain* arm. Therefore the water draw CAN run armed
  while `g_atlasGLTex == 0`. The `reflectionOn` gate (Section 4.3) is the
  mandatory mitigation - never assume armed-water implies atlas-present. Same
  class as the water 1-frame-lag lesson
  (`memory/water_fastpath_interim_fixes_and_residuals.md`): never infer one
  subsystem's armed state from another's.
- **R2 (MAJOR) Asymmetric atlas-UV convention.** Y is inverted, X is not
  (`gos_terrain.frag` ~`:344-346`). The FS must replicate the exact terrain
  formula; a symmetric form flips the reflection vertically.
- **R3 (MAJOR) `GL_CLAMP_TO_EDGE` off-map streak.** A reflected ray marched
  in world XY frequently leaves the map rectangle. Clamped sampling returns
  the colored map-edge texel - an edge smear at every off-map-facing
  shoreline. The in-bounds `step()` mask (Section 4.2) zeroes off-map taps
  so they contribute nothing. Verify the mask, not just the clamp.
- **R4 (MINOR) `GL_LINEAR`/no-mips shimmer.** Distant/oblique reflection
  taps will shimmer (no mip filtering on the atlas). Acceptable for an
  intentional smear; noted so it is not later mis-rooted as a new bug.
- **R5 (MINOR, named limitation) Source is albedo-like terrain colormap,
  NOT lit scene color.** The atlas is static terrain color - it carries no
  dynamic lighting, no sun direction, no shadows, no battle-damage/scorch.
  The reflection will therefore NOT match the lit/shadowed appearance of the
  terrain it mirrors. This is intentional and accepted for S3 (the whole
  point of the cheap slice). Named here so a later session does not
  mis-triage "reflection doesn't match sun/shadows/damage" as a bug and try
  to wire lit scene color (that is the feedback-loop-blocked SSPR path).
- **R6 (MINOR, perf-only) Off-map taps still fetch.** The in-bounds
  `step()` mask multiplies by zero but GLSL arithmetic does not
  short-circuit, so `texture(reflTex, uv)` is still sampled for off-map
  steps (clamped, so correctness is fine - only the edge texel is fetched
  then zeroed). Cost only, not a correctness issue; do not "optimize" it
  into a branch that breaks uniform control flow.
- **Camera-independence named contract:** the `vdir`/`reflect`/`fres` block
  is the ONLY `cameraPos`-dependent code added; S1 wave/glint/color stay
  `f(WorldPos,time)`. No `SKY_TINT`/`fres`/`spec` sky terms reintroduced
  (those were deleted in `8ee5d12` for this reason).
- **No regression:** z-bias untouched (`WATER_DEPTH_FUDGE_FAST`; the on-screen
  zoom z-fight is the pre-existing depth-fudge residual, NOT S3 scope - do
  not attempt it here); `[WATER_MAT v1]` / `[WATER_DEPTHPROBE v2]` probes
  untouched; `GBuffer1` MRT (loc 1) untouched; no GPU readback / no
  `glClientWaitSync`-after-write (sync-stall lesson); no AMD feedback loop
  (the atlas is a static resident texture, NOT the bound scene-color
  attachment - structurally feedback-loop-free, unlike SSPR).

## 6. Option B - the deferred fidelity upgrade (documented, NOT in scope)

If a specific hero location (e.g. a cliff with water on three sides) ever
needs crisp mirrored geometry rather than a color wash: a **dedicated,
low-rate offscreen render** of the mirrored static terrain (from the
surviving CPU heightfield `Terrain::vertexList` / `MapData::makeLists`),
re-rendered only on a quantized camera-region-key change, cached, sampled in
the SAME water-FS consumer site as Option C. It uses its OWN minimal shader +
FBO + mirrored MVP and touches NONE of
`g_thinRingSlot`/`g_thinRingFences`/`g_indirectCmdBuffer`/`g_dispatchMvp16`
nor `gos_terrain_bridge_drawIndirect`. Central risk: the region-key
invalidation (too coarse -> pop/lag on fast camera motion; too fine ->
defeats low-rate). This is a separate spec -> 2 adversarials -> plan when/if
the fidelity is demanded. It is NOT a revival of the BLOCKED in-pipeline
second-dispatch design.

## 7. Plan-stage Rule-0 verifications (MUST close in the plan)

- **V1:** re-grep and confirm the four `gos_terrain_indirect_getAtlas*()`
  accessor names + signatures and that they are `extern`-visible in
  `gameos_graphics.cpp` at `renderWaterFastPath`.
- **V2:** re-grep `renderWaterFastPath` `setM*` lambda names
  (`setMF`/`setMI`/`setMVec2` etc.) + the unit-1 sampler save/restore block;
  confirm unit 2 is unused by `s_waterMdiProg` (water FS sampler
  declarations).
- **V3:** re-grep the terrain-frag atlas-UV formula (`gos_terrain.frag`) and
  confirm the X-not-flipped / Y-inverted asymmetry is current; the FS S3
  block must match it character-for-character.
- **V4:** confirm `IsFrameSolidArmed()` + `getAtlasGLTex()` are both callable
  from `renderWaterFastPath`'s translation unit at the water draw site
  (the `reflectionOn` gate depends on both).
- **V5:** confirm `waveNormal` and `waveLOD` symbols exist in the current
  `o_isWater==1` branch (S1 as-built, commit `89d329b`) with the meanings
  reused here; if renamed, bind to the current symbols.
- **V6 (carry into plan):** the `setM*` uniform setter must tolerate a
  missing/optimized-out uniform location the same way the existing helpers
  do (a tuning pass that compiles out `reflectionOn`/`REFL_*` must not
  crash/assert). Confirm the existing `setM*` path is location-tolerant; if
  not, the plan adds the guard.

## 8. Gates

- **Pure-FS = shader-only, hot-reloadable. NO relink.** But the C++ uniform
  binds (Section 4.1) ARE a `gameos_graphics.cpp` change -> this slice DOES
  need a full relink + exe deploy for the uniform/texture-bind plumbing, then
  the `.frag` is hot-reloadable for the visual tuning loop. (Only the
  per-const visual iteration is hot-reload; first wiring needs the exe.)
- Build RelWithDebInfo, full relink (`gameos_graphics.cpp` is load-bearing).
  Deploy exe + `.frag` to the isolated `A:/Games/mc2-opengl/mc2-win64-water`
  ONLY - never `mc2-win64-v0.4` (concurrent priority session).
- After redeploy, grep the engine log for `0(N): error` / link error
  (shader hot-reload fails silently on bad compile).
- Add an env-gated `[WATER_REFL v1]` probe, logged once per arm transition,
  carrying BOTH the raw atlas handle AND the final `reflectionOn` state AND
  a cheap reason enum: `reason=solid0` (`!IsFrameSolidArmed()`),
  `reason=atlas0` (`getAtlasGLTex()==0`), or `reason=on` (`reflectionOn==1`).
  This makes silent-fallback diagnosis immediate. A kill-aware `mc2_01` 30s
  smoke (`--keep-logs --exe`, marker-gated, exit-code-agnostic) must show
  `reason=on` to prove the path is live and NOT in silent fallback.
- Real gate = USER visual tuning loop on the hot-reload consts: terrain
  colors visibly wash into the water from terrain-adjacent shorelines,
  correct from each camera angle when settled, fuzzed by S1 waves, fades to
  clean S1 water when distant/zoomed/un-armed, no edge streak off-map, no
  S1/camera-independence regression. Smoke window is user-driven; do not
  claim a visual result or ask for re-runs.

## 9. Files

```
MODIFIED  GameOS/gameos/gameos_graphics.cpp     -- renderWaterFastPath mdiValid:
                                                   3 atlas uniforms + reflTex/reflectionOn
                                                   via existing setM* block; bind
                                                   getAtlasGLTex() on unit 2 with the
                                                   existing unit-1 save/restore idiom;
                                                   reflOn = IsFrameSolidArmed() &&
                                                   getAtlasGLTex()!=0
MODIFIED  shaders/gos_terrain_water_mdi.frag    -- o_isWater==1: reflected-ray atlas
                                                   march, in-bounds mask, Fresnel-
                                                   weighted mix; REFL_* hot consts
(REUSED, unmodified)  the colormap atlas + its accessors + S1 waveNormal/waveLOD.
```

## 10. Discipline

This spec -> user spec review -> 2 adversarials (opus + sonnet,
adversarial-plan-review skill, code-grounded, CRITICAL/MAJOR/MINOR) -> fold
findings -> implementation plan (writing-plans) -> subagent-driven execute ->
isolated build/deploy -> kill-aware `mc2_01` marker-gated smoke + `[WATER_REFL
v1]` probe -> user visual tuning loop. Branch stays isolated; the user
integrates separately.
