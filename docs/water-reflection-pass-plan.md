# Water Reflection Render-Pass Plan (WATER-REFLECTION-PASS-PLAN-1)

Phase C-pre: specs the first render-INTO-the-reflection-target pass before
implementation. Phase B built the 1/4-res RT substrate (FBO/resource/view +
black ImGui preview). Phase C renders **mirrored terrain into that RT** with a
mirror MVP, **no clip plane** yet. **Recon/spec only — no code.**

Lane: `claude/water-reflection`. HEAD `1dab976d`. Pairs with
[`water-reflection-plan.md`](water-reflection-plan.md) (arc plan) and the
shipped Phase A (`dad0cf9c`, SH sky reflection) / Phase B (`1dab976d`, RT
substrate).

## TL;DR — recommendation

**Split Phase C into two slices:**

- **C1 = WATER-TERRAIN-REFLECTION-1** (fill): render **mirrored terrain only**
  (no water, no props/mechs, no clip plane) into `WaterReflectionColor/Depth`.
  Clear color to `(0,0,0,0)` so off-terrain pixels have **alpha=0** (fallback
  marker). Gate `MC2_WATER_REFLECTION_RT` **default OFF**. **No water-shader
  change** — the only visible proof is the ImGui reflection thumbnail going
  non-black. Provably correct in isolation.
- **C2 = WATER-REFLECTION-SAMPLE-1** (consume): water FS samples the RT (reuse
  the now-dead unit-2 `reflTex` slot), screen-space projected + wave-normal
  distorted, and blends it **over the Phase-A SH sky term** using RT alpha as
  the terrain-vs-sky selector (RT.a>0 → terrain reflection; else → SH sky).
  Same `MC2_WATER_REFLECTION_RT` gate.

**Render terrain-only into the RT, not sky+terrain.** Phase A already supplies
the broad sky shape (SH-L2) cheaply in the water FS; the RT's unique value is
**near-shore terrain reflection**. Off-terrain reflected rays fall back to the
SH sky (RT alpha=0), so no sky needs to be drawn into the RT. (Optional: draw
the HDRI sky into the RT in a later polish slice if the SH fallback seam shows.)

**Keep Phase-A SH sky reflection as the fallback/blend layer.** **Defer the
clip plane** to WATER-REFLECTION-CLIP-1 (accept documented artifacts, §5).

Split rationale: C1 is self-contained and verifiable via the preview without
touching the water material; C2 is a pure shader+bind change. Splitting keeps
each diff small, each independently revertible, and isolates the (moderate)
mirror-pass risk from the (small) sampling risk.

## 1. Pass insertion point

Frame loop ([code/gamecam.cpp:284-378](../code/gamecam.cpp)):
`sky → land->render() (queue) → objects → renderLists() (terrain GPU draws fire,
:334) → renderWaterFastPath() (:345) → particles`.

**Insert the reflection pass between `renderLists()` (:334) and
`renderWaterFastPath()` (:345).** Rationale: terrain GPU data (recipes, indirect
buffers, atlas) is fully built by the main `renderLists()`, so the reflection
pass can re-dispatch the terrain SOLID path with a mirror MVP into the RT; and
it completes before the water draw samples it. Model the save/restore on the
dynamic-shadow pre-pass [`beginDynamicShadowPass`/`end`](../GameOS/gameos/gameos_graphics.cpp)
(gameos_graphics.cpp:~5186), which already binds an offscreen FBO, swaps
projection, and restores.

**GL / global state to save+restore (the whole reflection pass is bracketed):**
- The terrain MVP global (`gos_GetTerrainMVPMat4()` backing `terrain_mvp_`) — install mirror, restore real.
- Bound FBO + viewport (set to `waterReflFBO_` + quarter-res `waterReflW_/H_`, restore scene FBO + full viewport).
- `glCullFace` winding (reflection inverts triangle orientation → flip front-face or cull mode, restore after).
- Depth state (clear RT depth; restore).
- `gos_InvalidateRenderStateCache()` after the pass (existing remedy, used by the shadow pre-pass).
- Do **NOT** re-call `GpuStaticPropRegistry::frameBegin()` (resets live lists).

## 2. Mirrored camera / MVP

**Current mechanism:** [code/gamecam.cpp:180](../code/gamecam.cpp)
`gos_SetWorldToClipGL(eye->worldToClipGL())` builds the world→GL-clip matrix
(`axisSwap · worldToCamera · cameraToClip`) and writes the `terrain_mvp_` cache
([gameos_graphics.cpp:7529 `gos_SetWorldToClipGL`](../GameOS/gameos/gameos_graphics.cpp),
setter `setTerrainMVP` :1402). Every terrain consumer (CullUBO, indirect SOLID
compute, mech/static-prop batchers, water) reads `gos_GetTerrainMVPMat4()`.
The terrain SOLID path **bakes clip-space positions in its compute shader** from
this matrix — so installing a mirror matrix here is the entire lever; no shader
edit needed.

**Mirror matrix:** water is a horizontal plane at world Z = `waterElevation`
(MC2 Z-up; [Terrain::waterElevation](../mclib/terrain.cpp)). The reflection of a
world point across that plane is `z' = 2·waterElevation − z`, i.e. a 4×4
reflection `R = T(0,0,2·we)·diag(1,1,−1,1)`. The mirror clip matrix is
`worldToClipGL_mirror = worldToClipGL · R`. Safest install: compute `R` on CPU,
multiply into the current `worldToClipGL`, and call `gos_SetWorldToClipGL(mirror)`
at the start of the reflection pass; restore by re-calling
`gos_SetWorldToClipGL(eye->worldToClipGL())` (or caching the pre-pass matrix and
re-installing it) after.

**Winding:** `det(R) < 0` flips triangle orientation → the reflection pass must
flip the cull winding (`glFrontFace(GL_CW)` if scene is CCW, or disable cull),
restored after.

**Camera position:** the terrain TCS distance-LOD reads `gos_SetTerrainCameraPos`
(gamecam.cpp:257). Optionally mirror it (`camZ' = 2·we − camZ`) for correct LOD
in the reflection; at 1/4 res, using the unmirrored camera pos is acceptable —
**document as a minor approximation**, mirror it only if LOD popping shows.

**ViewUniforms / EngineView interaction:** terrain SOLID does **not** consume
the ViewUniforms UBO (it bakes from the MVP global), so the mirror install above
fully covers terrain. Do **not** call `uploadViewUniforms` with the mirror (it
would corrupt the binding=3 UBO that props/mechs read — but those aren't drawn
in C anyway). When ready, register a live `WaterReflectionView`
(`kWaterReflectionViewId`, `ViewKind::WaterReflection`) carrying the mirror
matrix + quarter-res viewport for **bookkeeping/debug only** (see §6) — it is
not the lever that drives the terrain draw.

## 3. Terrain-only rendering

- **Entry:** the terrain SOLID indirect path — `renderLists()`
  ([mclib/txmmgr.cpp:~2054](../mclib/txmmgr.cpp)) → `gos_terrain_indirect::DrawIndirect()`
  ([gos_terrain_indirect.cpp:~3307](../GameOS/gameos/gos_terrain_indirect.cpp)).
  `renderLists()` also drains non-terrain lists, so C1 should call the **terrain
  SOLID dispatch + DrawIndirect** directly (re-dispatch the SOLID compute with
  the mirror MVP, then the indirect draw), NOT the full `renderLists()`. This is
  the moderate-complexity piece: the SOLID ring buffers assume one dispatch/
  frame — a second dispatch needs its own ring slot or a re-pack (spec the
  implementer to add a dedicated reflection dispatch slot or reuse after the
  main draw has consumed its slot).
- **Without water:** trivially — the reflection pass calls only terrain; it does
  NOT call `renderWater()`/`renderWaterFastPath()`. Water draw is gated by
  `WaterFastPathOwnsArmedDraw()` and only invoked from the main sequence.
- **Decals/overlays:** excluded — call only the SOLID bucket, not overlay/decal
  dispatches.
- **Skybox:** `renderHdriSkybox(viewMat, projMat)`
  ([gos_postprocess.h:30](../GameOS/gameos/gos_postprocess.h)) writes color
  attachment 0 only and could draw into the RT with the mirror view/proj — but
  per §TL;DR, **omit it in C1**; the SH sky (Phase A) is the off-terrain
  fallback. Revisit only if the fallback seam is objectionable.

## 4. Recursion / exclusion

- **Water-in-reflection:** impossible — the reflection pass never calls a water
  draw; no frame-loop re-entry. No infinite recursion.
- **UI / debug / post-process:** excluded — the pass runs before post-process
  and draws only terrain SOLID.
- **Static props / mechs:** **excluded in Phase C** (they consume ViewUniforms,
  add draws + state to mirror, and aren't the near-shore signal). A future
  slice could add them once the terrain RT proves useful.

## 5. Clip-plane deferral (accepted artifacts)

No `gl_ClipDistance` infra exists (confirmed shader-wide); the terrain
projection is baked in the SOLID **compute** shader, so a water-height clip is a
larger change. **Defer to WATER-REFLECTION-CLIP-1.** Without clipping, expect:
- **Below-waterline terrain** (lakebed/banks under the surface) renders into the
  reflection and appears in the reflected image where it shouldn't.
- **Shoreline / near-plane artifacts:** geometry straddling the water plane
  reflects with a seam at the waterline.
- **Reflection leakage:** terrain slightly below the plane bleeds into grazing-
  angle reflections.

**Why acceptable for Phase C:** the RT is 1/4-res and blended at a low,
Fresnel/grazing-weighted strength under the SH sky; the dominant read is "the
shore/terrain is reflected," and the artifacts are subtle at that res+weight and
behind a default-OFF gate. C ships the *mechanism*; CLIP-1 refines fidelity.
**WATER-REFLECTION-CLIP-1 (future):** add `gl_ClipDistance[0]` emit from the
terrain SOLID compute/thin-VS at `z − waterElevation`, `glEnable(GL_CLIP_DISTANCE0)`
during the reflection pass — or an oblique-near-plane trick on the mirror matrix.

## 6. Resource / view / debug

- **Live WaterReflectionView:** register it (with the mirror matrix + quarter-
  res viewport) at the start of the C1 pass — **bookkeeping/debug only**; it does
  not drive the terrain draw (the MVP global does). Lets the inspector show a
  real reflection view.
- **debug-state JSON:** optional — add a small "waterReflection" section
  (enabled, RT dims, lastFrameDrawn, drawCount) if cheap; not required for C1.
- **ImGui preview:** already wired (Phase B thumbnail). C1's success = the
  thumbnail shows mirrored terrain instead of black.
- **Counters/logs (proof the pass ran):** emit `[WATER_REFL_RT v1]
  event=pass frame=N draws=K mirror_ok=1 fbo=complete` once per N frames (like
  the DRAW_PACKET_V6 summary). C1 acceptance reads this + the non-black preview.

## 7. Sampling in water (C2)

- **Texture delivery:** bind `waterReflColorTex_` to the **dead unit-2 slot**
  (the old `reflTex` atlas sampler — now unused after Phase A). Re-purpose
  `uniform sampler2D reflTex` → the reflection RT (rename to
  `u_waterReflRT` for clarity). C++ binds `getWaterReflectionTexture()` to unit 2
  in the MDI bind block.
- **UV / projection:** standard planar-reflection screen-space sample — the RT
  was rendered with the mirror MVP at the same screen projection, so sample it
  at the water fragment's **screen UV** (`gl_FragCoord.xy / screenSize`),
  perturbed by the existing fBm wave normal (reuse the Phase-A `waveNormal`
  gradient as a small UV offset). Add a `screenSize` uniform (or pass via an
  existing one). No view matrix needed in the FS.
- **Blend:** `reflColor = (rt.a > 0.0) ? rt.rgb : skyReflCol;` then the existing
  Phase-A mix `col = mix(col, reflColor, reflMix)` (Fresnel·strength·waveLOD).
  RT terrain reflection thus layers over the SH sky exactly where terrain was
  reflected; sky shows elsewhere. Phase-A SH path stays as the fallback.
- **Gate / env:** **`MC2_WATER_REFLECTION_RT`** (new, default OFF), distinct
  from `MC2_WATER_REFLECTION` (the SH sky term). Documented interaction:
  `MC2_WATER_REFLECTION` = SH sky reflection (cheap, always-available shape);
  `MC2_WATER_REFLECTION_RT` = adds the terrain RT layer (needs the C1 pass +
  `MC2_GPU_DRIVEN_WATER`). RT implies/should-coexist-with the SH term as
  fallback. Both default OFF.

## 8. Validation plan

**C1 (fill):** build; tier1 5/5 gate OFF (RT pass not run → byte-identical);
mc2_24 `MC2_WATER_REFLECTION_RT=1 MC2_GPU_DRIVEN_WATER=1` → `[WATER_REFL_RT]`
log shows draws>0, FBO complete, no GL errors; **ImGui reflection thumbnail
non-black** (mirrored terrain visible) — primary proof. No water-shader change,
so the water surface itself is unchanged in C1.

**C2 (sample):** build; shader_reflect golden refresh (reflTex→RT rename / new
`screenSize`); env_registry for `MC2_WATER_REFLECTION_RT`; tier1 5/5 gate OFF
(byte-identical); mc2_24 gate ON → water shows terrain reflection near shore,
gl_errors=0, no crash; debug mode 7 still shows the SH term; user visual A/B
(orbit — terrain reflection should track the shore, sky elsewhere). Foreground-
race caveat applies to pixel captures → rely on logs + user eyeball. **No
`--kill-existing`.**

## Recommended implementation slice (next)

**WATER-TERRAIN-REFLECTION-1 (C1):** mirrored terrain-only fill of the 1/4-res
RT behind `MC2_WATER_REFLECTION_RT` (default OFF), bracketed save/restore of
MVP global + FBO + viewport + cull winding (modeled on `beginDynamicShadowPass`),
own SOLID dispatch slot, `[WATER_REFL_RT]` counter, debug preview as the proof.
**No water-shader change, no clip plane, no props/mechs.** Then
**WATER-REFLECTION-SAMPLE-1 (C2)** wires water FS sampling + blend.

Rollback: `MC2_WATER_REFLECTION_RT` unset → C1 pass skipped + C2 sample skipped
→ byte-identical; SH sky reflection (`MC2_WATER_REFLECTION`) unaffected.

## Known artifacts accepted (Phase C, pre-clip)
Below-waterline terrain in the reflection, shoreline seam, grazing leakage —
all subtle at 1/4 res + low Fresnel-weighted blend, behind a default-OFF gate;
resolved by the future WATER-REFLECTION-CLIP-1.

---

**Status:** docs/spec only. Recommends C1 (mirrored terrain-only RT fill, gated
default-OFF, preview-proven) then C2 (water FS sample + blend over Phase-A SH).
Clip plane deferred to WATER-REFLECTION-CLIP-1.

---

## POSTSCRIPT (2026-05-29) — shipped state + corrected CLIP-1 root cause

C1 (`b09da4be`) and C2 (`1fb8731d`) shipped as planned and the whole reflection
arc is **merged to nifty** (merge `68343329`), all gates default-OFF. The
SH-L2 sky reflection is the working primary at all cameras (sun-azimuth +
cameraPos-frame + dispatch-MVP-restore fixes landed: `615865d6`/`d027d6a9`/`d671343e`).

**Correction to this plan's "marginal payoff / camera-regime" framing:** the
MC2 gameplay camera is **~20° oblique across cliffs**, NOT steep/top-down, so
terrain reflection SHOULD be visible there — it is a real bug, not physics.
Root cause (investigated): the terrain SOLID compute depth gate `pzOk`
([gpu_driven_terrain_solid.comp:213-215](../shaders/gpu_driven_terrain_solid.comp))
culls the mirrored quads. Reflecting world-Z and re-projecting through the
**real** camera shifts each above-water hill's camera-forward depth by
`Δs ≈ 2·eye_height·sin(pitch)`; the high RTS eye makes `Δs` large even at 20°,
pushing mirrored corners past the reverse-Z far plane → 0 instances → empty RT.
The simple world-mirror reusing the real camera's depth gate is the wrong tool.

**WATER-REFLECTION-CLIP-1 (the remaining slice):** build a reflected projection
with a **Lengyel oblique near-plane** at the water plane so mirrored geometry
stays in the reverse-Z band at all angles AND below-water geometry is excluded
without an FS discard. (Interim hack: a `u_reflectionPass` flag relaxing `pzOk`'s
z-range + `GL_DEPTH_CLAMP` — but RT depth ordering then approximate.) Same
pixel-homog(`getWorldToClip`)-vs-GL-NDC(`worldToClipGL`) convention class as the
cameraPos-frame fix and the shadow-lane txmmgr unprojection fix. Memory:
`water-reflection-clip1-followup`.
