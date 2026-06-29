# FRAME-GRAPH-TEX-LATCH-RECON-1

Read-only recon. Maps every texture-read / texture-unit latch hazard in the render
frame, BEFORE any executor modeling. Source-verified vs nifty-mendeleev worktree tip
(branch `claude/animated-prop-cook-recon-1`). No code changes. §6 is modeling-only.

**Texture latch = a pass that SAMPLES a texture it never bound, "working" only because a
prior pass left it on the unit.** That is where old GL renderers hide ghosts: making
bindings explicit (an executor's job) BREAKS the ghost. This recon gates whether a
texture-read ledger (mirror of `fbo_ledger.h`) is buildable.

Reuses two banked artifacts (do not re-derive): `texture-unit-ownership-recon-1.md` and
the generated `sampler-unit-occupancy.json` (from `scripts/check-sampler-bindings.py`).
This doc adds the **explicit-vs-inherited** axis those did not answer.

---

## 0. Headline (read this first)

- **No confirmed inherited-latch ghost on the default-on path.** The thing this recon was
  hunting — a geometry pass sampling the shadow map / height tex / IBL without binding it —
  **does not exist as feared**, because the renderer is a **deferred screen-shadow design**:
  terrain/mech/static-prop/grass/veg do NOT sample any shadow texture at geometry time. They
  write `GBuffer1.a = screenShadowEligible` and shadow is applied later in the PostProcess
  `screenShadowProg_` pass, which binds units 2/3/4 **explicitly** (`gos_postprocess.cpp:2128-2137`).
- **The `reads[]={ShadowDynamicMap}` rows on Terrain/Mech/StaticProp/Veg in
  `kRenderPassContracts[]` are an ORDER encoding, not a draw-time-sample claim.** No geometry
  fragment shader samples a shadow sampler (confirmed: `mech.frag`, `static_prop.frag` write
  `rc_gbuffer1_screenShadowEligible`, sample nothing). `frameBegin()` pre-seeds
  `ShadowDynamicMap` (`render_contract.cpp:777`) precisely because the only real consumer is post.
- **One real, confirmed residual latch** (vendor-divergent, default-OFF): the shadow-debug
  overlay leaves `GL_TEXTURE_2D_ARRAY` bound on unit 0 (`gos_postprocess.cpp:2698-2699`,
  never rebinds 0). Already cataloged as ledger row GLSTATE-SHADOWDEBUG-2DARRAY-1 / DEFERRED_LOW_RISK.
- **The genuine systemic hazard is `2D`-vs-`2D_ARRAY` target aliasing on multiplexed units**
  (units 3, 4, 5), which is safe TODAY only by the per-pass-rebind + `glActiveTexture(0)`-at-exit
  convention. There is no static check for a target-type mismatch on a unit. This is the one
  thing a texture-unit ledger would actually buy.
- **Verdict on a texture-read ledger: LOW-MEDIUM value, partially buildable.** The 4 id-backed
  textures are reverse-mappable cheaply (own GLuint variables exist). The atlases (terrain
  matNormal0-4, mech PBR, static-prop arrays, IBL, particle atlas) have NO `RenderResourceId`
  and would stay opaque. Concurs with the banked `texture-unit-ownership-recon-1.md` verdict.

---

## 1. Texture-unit binding map (per pass)

Per `kFramePassOrder[]` (`RenderPassContract.h:353`). Units are GL texture image units;
sampler-uniform→unit from `sampler-unit-occupancy.json`. "Binds" = a `glActiveTexture(GL_TEXTUREn)`
+ `glBindTexture`/`glBindTextureUnit` at draw time in that pass's owner TU.

| Pass (frame order) | Owner TU | Units bound (target) | Sampler→unit | SSBO binds |
|---|---|---|---|---|
| **Shadow** (caster) | `gos_postprocess.cpp` shadow render + per-lane | Depth-only FBO; writes `shadowDepthTex_`/`dynShadowDepthTex_`/`dynShadowArrayTex_`. Geometry re-uses its own albedo binds. | n/a (depth-only frags) | — |
| **StaticPropOpaque** | `gos_static_prop_batcher.cpp` | 0 albedo `2D_ARRAY` (`u_texArr`), 1 ORM `2D_ARRAY` (`u_ormTexArr`); 0/1 also 2D fallback (`u_tex`) | `u_texArr`/`u_ormTexArr` arr; `u_tex` 2D | yes (draw/meta SSBOs) |
| **Terrain** | `gos_terrain_indirect.cpp` / `gos_terrain_patch_stream.cpp` / quad.cpp | 0 colormap (`u_colormap`/`tex1`); 4 transition-mask `2D_ARRAY` (`u_transitionMaskArray`); 5-8 matNormal0-3 2D; 12 matNormal4/snow 2D; height-tex unit (see §2) | per occupancy json | recipe/thin SSBOs |
| **MechOpaque** | `gos_mech_batcher.cpp:2121-2144` | 0 base (`u_tex`); 1 PBR detail-normal; 2 PBR detail-ORM; 3 paint-normal; 4 paint-ORM; 6 imported-AO; 7 imported-normal — all `GL_TEXTURE_2D` | `u_pbrNormalTex`=1 … `u_pbrPaintOrmTex`=4 | SSBO slot 2 mech material (`:2092`) |
| **TerrainDecal** | quad.cpp / `gameos_graphics.cpp` decal batch | 0 decal albedo (`tex1`) | `tex1` 2D | — |
| **TerrainOverlay** | quad.cpp / `gameos_graphics.cpp:9617` drawDecalStaticBatch | 0 overlay/road/cement; +asphalt/gravel/matNormalArray on overlayProg_ (units 4-6) | `tex1`; `matNormalArray` arr | — |
| **Water** | `gameos_graphics.cpp:3500+` / renderWaterFastPath | 0 base; 1 detail; 2 reflRT; 3 HDRI (`u_hdri`) — units 1-3 saved/restored | `u_waterReflRT`, `u_hdri` (unit 3) | thin/solid ring SSBO |
| **VegetationCards** | `gos_vegetation.cpp` | 0 atlas (`u_atlas`) 2D | `u_atlas` 2D | blockVis SSBO |
| **VFX** (particles) | `gos_particle_bridge.cpp` / `gos_vfx_mesh_bridge.cpp` | 0 atlas (`uAtlas`); 1 depthCopy (soft); 2 sceneColorCopy (distort) | `uAtlas`, `u_sceneDepth` | SSBO 14/15/16 tube (sym unbind) |
| **UI** | GameOS 2D / HUD | 0 font/HUD atlas (`tex1`) via applyRenderStates re-bind | `tex1` 2D | — |
| **PostProcess** | `gos_postprocess.cpp` | rotates 0-4 per sub-pass: HZB 0=`uSrc`; SSAO 0=depth,1=normal; screenShadow 0=depth,1=normal,2=`shadowMap`,3=`dyn{Array,Map}`,4=`fullMap`; composite 0=sceneColor,2=objectId; skybox 0=`u_hdri`; shadowDebug 0=`shadowDebug{Map,Array}` | per `assignments[]` in json | — |

Notes: units 0-4 are **intentionally multiplexed** across sequential passes (re-bound every
pass). That is by-design, not a conflict. The hard sampler-NAME→unit drift class is already
governed by `scripts/check-sampler-bindings.py` (hard-anchors `uSrc`/`sceneTex`/`ssaoTex`→0,
0 anchor drift at last run).

---

## 2. EXPLICIT vs INHERITED reads (core deliverable)

For each texture a pass SAMPLES: does the pass bind it itself (explicit) or rely on a prior
pass having left it bound (inherited latch / ghost)?

### 2a. Shadow maps — the headline non-ghost

The four geometry contracts declare `reads[]={ShadowDynamicMap}`
(`RenderPassContract.h:174,188,203,288`). **No geometry shader samples a shadow sampler:**

- `mech.frag` — no shadow sampler; writes `rc_gbuffer1_screenShadowEligible(N_gbuf)`
  (`mech.frag:407`). Confirmed by grep: only comments mention shadow.
- `static_prop.frag:22-24` — `skipsPostScreenShadow=false`; writes
  `rc_gbuffer1_screenShadowEligible`; samples no shadow map.
- Terrain (`gos_terrain.frag`) / grass / veg — same deferred contract
  (`render_contract.cpp:70,92,136,187`: all set `skipsPostScreenShadow` true/eligible and
  write `GBuffer1.a`).

The ONLY pass that binds + samples shadow maps is **PostProcess → `screenShadowProg_`**, and
it binds them **EXPLICITLY** every invocation: unit 2 `shadowDepthTex_`, unit 3
`dynShadowArrayTex_` (CSM) or `dynShadowDepthTex_`, unit 4 full-map cascade
(`gos_postprocess.cpp:2128-2137`). Verdict: **EXPLICIT. No inherited shadow latch.** The
`reads[]` rows are a dependency-ORDER encoding (Shadow-before-geometry), paper-stamped by
`frameBegin()` pre-seeding `ShadowDynamicMap`/`TerrainHeightTexture` (`render_contract.cpp:777-778`).

> This is the single most important finding: the design that would normally hide a shadow
> ghost (geometry reading a leftover shadow bind) is structurally absent — shadow is a
> screen-space deferred pass, not a forward geometry sample.

### 2b. TerrainHeightTexture — owner-bound, NOT inherited

`gos_terrain_height_tex.cpp` UPLOADS the R32F height tex once at mission load and carefully
**restores the previous unit-0 binding** (`:163-188`) — it does not leave it latched. At
draw time the terrain shaders sample `terrainHeightTex` (`terrain_height_normal.hglsl:30`);
the bind to its sampling unit is owned by the terrain draw path each frame (the occupancy
json lists `terrainHeightTex` as an "UNKNOWN binder" = literal `glUniform1i` loc-cache /
dynamic helper, not a missing bind). Verdict: **NOT an inherited latch** (frameBegin pre-seeds
the resource as "always valid"; the GLuint is `gos_terrainHeightTexHandle()` and is bound by
the terrain TU, not inherited from another pass).

### 2c. MaterialGpuBuffer (SSBO) — explicit, fenced, symmetric

`MaterialGpuBuffer` is an SSBO, not a texture, but it is in `RenderResourceId`. Mech material
table is bound `glBindBufferBase(...,2,s_mechMaterialSsbo)` at mech flush
(`gos_mech_batcher.cpp:2092`) and the prior bindings on slots 0/1/2 are **saved and restored**
(`:2504-2506`). Verdict: **EXPLICIT + restored. No inherited SSBO latch.** (Consistent with
GPU-BINDING-SLOTS-LOCKSTEP-1 / particle slot-14 symmetric unbind already proven.)

### 2d. IBL / HDRI — explicit per consumer

`u_hdri` is bound explicitly by `hdriSkyboxProg_` (unit 0, `gos_postprocess.cpp:2554/2749/2932`)
and by the water MDI path (unit 3). Multiplexed across unit 0 (skybox) and unit 3 (water),
each binds its own. Verdict: **EXPLICIT.**

### 2e. Scene depth/normal/color (post chain) — explicit

Every post sub-pass binds `sceneDepthTex_`/`sceneNormalTex_` itself before its draw
(`:2125-2127`, `:2215`, `:2264-2266`, `:2321`). These are separate FBO attachments (no
read/write feedback loop, annotated `:2290`). Verdict: **EXPLICIT.**

### 2f. The residual question — composite unit-2 objectId

Composite conditionally binds `sceneObjectIdTex_` on unit 2 only if non-zero
(`gos_postprocess.cpp:2622-2625`); the `usampler2D` is "forced back to 0 above so it goes
unread" when absent (comment `:2620`). This is a **guarded conditional bind**, not an
inherited latch — the shader does not sample a leftover unit-2 texture. Verdict: **EXPLICIT
+ guarded.**

**§2 conclusion: zero confirmed inherited-binding (ghost) dependencies on the default-on
path.** Every sampled texture is bound by its own pass. The renderer's heavy use of
per-pass-rebind + manual save/restore epilogues (cataloged in the GL-correctness ledger
GLSTATE-TEXUNIT-LEAK-GUARDS-1 closure table) is exactly what prevents inherited latches.

---

## 3. NVIDIA-risky latches (vendor-divergent residuals)

| id | site | trigger | symptom | vendor split | default |
|---|---|---|---|---|---|
| **SHADOWDEBUG-2DARRAY** (confirmed) | `gos_postprocess.cpp:2698-2699` binds `GL_TEXTURE_2D_ARRAY` on unit 0, never `glBindTexture(target,0)` before exit (only compare-mode restored `:2706`) | toggle `showShadowDebug_` (ImGui) with CSM active + dynamic mode | unit 0 left holding a 2D_ARRAY for the next 2D consumer (HUD); incomplete-texture sampling is driver-defined | NVIDIA-leaning (AMD tolerant) | **OFF** (ImGui-only). Ledger: GLSTATE-SHADOWDEBUG-2DARRAY-1 / DEFERRED_LOW_RISK. 1-line fix iff ever default-on. |
| **2D-vs-2D_ARRAY unit aliasing** (systemic, suspected) | unit 3 = `2D_ARRAY` (CSM dyn shadow, screenShadow) vs `2D` (mech paint-normal / water HDRI); unit 4 = `2D_ARRAY` (fullMap shadow / terrain transition mask) vs `2D` (mech paint-ORM); unit 5 = `2D` matNormal0 vs `2D_ARRAY` mineSpriteArray | any frame: these passes co-occur in different draws on the same unit number | safe ONLY because each pass re-binds the correct target before its draw; a missed rebind → wrong-target sample (incomplete/driver-defined) | both, NVIDIA stricter on incomplete | on (latent; no static guard). occupancy.json `warns` rows for units 3/4/5 |
| **mineSprite unit-5 share** (suspected) | unit 5 shared terrain `matNormal0` (2D) and mine `mineSpriteArray` (2D_ARRAY); different shaders | mine-static draw co-occurring with terrain | wrong-target if passes interleave on unit 5 | both | on; "safe iff passes don't co-occur — unverified" per banked recon |
| empty shadow frags UB2-06/07; shadow PCF UB2-04 | shaders | — | (carried from GL-correctness ledger; NEEDS_VENDOR_TEST; empirically fine on dev 7900 XTX) | NVIDIA-unreproduced | n/a |

Only **SHADOWDEBUG-2DARRAY** is a *confirmed* unit-leak; it is default-OFF. The aliasing
class is *suspected*-but-real (the GL hazard exists; no failing repro because the rebind
convention holds). NVIDIA reproduction is **unavailable** (dev is AMD 7900 XTX) — all
NVIDIA rows stay NEEDS_VENDOR_TEST.

---

## 4. Reverse-mappable resources (can a texture ledger resolve GLuint→RenderResourceId?)

`RenderResourceId` (`RenderResourceRegistry.h`, enumerated in `render_contract.cpp:733-744`)
includes texture-backed ids. Which bound GLuints are knowable at bind time?

| RenderResourceId | owner GLuint variable | reverse-mappable? |
|---|---|---|
| `ShadowDynamicMap` | `dynShadowArrayTex_` / `dynShadowDepthTex_` (`gos_postprocess.cpp:401`, created `:3720/:3793`) | **YES** — register at creation |
| `ShadowStaticMap` | `shadowDepthTex_` (`:3392`) | **YES** |
| `TerrainHeightTexture` | `g_handle` (`gos_terrain_height_tex.cpp:157`, exported `gos_terrainHeightTexHandle()`) | **YES** |
| `MaterialGpuBuffer` | `s_mechMaterialSsbo` (`gos_mech_batcher.cpp:567`) — SSBO not texture | YES (but `glBindBufferBase` slot, not a tex unit — separate probe) |
| `WaterReflectionColor/Depth` | `waterReflFBO_` placeholder, no live producer (ledger POST-WATERREFL-PLACEHOLDER-1) | YES but reads black; low value |
| `MainColor`/`MainDepth` | `sceneColorTex_`/`sceneDepthTex_`/`sceneNormalTex_` | **YES** (already FBO-ledger-registered) |

**Textures with NO logical id (would stay opaque or need new ids):** terrain colormap atlas,
matNormal0-4, transition-mask array, cement atlas; mech base/PBR/paint/imported atlases;
static-prop albedo/ORM `2D_ARRAY`; IBL/HDRI; particle/veg atlases; mine sprite array. These
are content atlases with per-draw handles (`gos_GetTextureGLId`, 15 sites all PROVEN_COVERED
in ledger TEXHANDLE-1) — no stable logical identity.

**Feasibility of a `TextureLedger` mirroring `fbo_ledger.h`:** the FBO ledger pattern
(register GLuint→id at creation; probe samples `GL_DRAW_FRAMEBUFFER_BINDING`; resolve;
compare to declared) **ports cleanly for the 4-6 id-backed textures**. The probe would sample
`GL_TEXTURE_BINDING_2D` / `GL_TEXTURE_BINDING_2D_ARRAY` per active unit and resolve to a
RenderResourceId. **It cannot govern the atlases** (no id), so it would assert only "the
declared id-backed read is bound on SOME unit," not full occupancy. That is exactly the FBO
ledger's measure-first discipline (sparse `kPassFboTarget[]`, unknown→skip).

---

## 5. Paths needing instrumentation (static reads[] can't capture truth)

A static `reads[]` table cannot capture these; only a runtime probe at the `noteRenderPass`
seam can:

1. **Multiplexed-unit target identity** — which target (`2D` vs `2D_ARRAY`) is live on units
   3/4/5 at each pass begin. Probe: `glGetIntegerv(GL_TEXTURE_BINDING_2D[_ARRAY], …)` per
   active unit, compare to the sampler's declared target (from occupancy.json). Catches a
   missed rebind (the §3 aliasing class) — the one thing no static check covers.
2. **Per-pass active-unit count** — how many units a pass actually touches (the telemetry
   already samples drawbuffers `render_contract.cpp:687-691`; add a tex-unit equivalent:
   loop `GL_TEXTURE_BINDING_2D` over units 0..15).
3. **Sampler-type-vs-bound-target mismatch** — a `sampler2D` reading a unit holding a
   `2D_ARRAY` (or vice-versa). Static-detectable only by joining occupancy.json's sampler
   target to a runtime unit probe; the float-tex-into-usampler2D guard at `:2620` shows the
   team already hand-handles ONE such case — a probe would generalize it.
4. **Id-backed read presence** — for the 4-6 reverse-mappable textures: at a geometry/post
   pass begin, is the declared id-backed read actually bound on the unit the shader samples?
   (Mirror of the FBO ledger's `fboMismatch`.) Mostly a no-op given §2 (all explicit), but
   it would *enforce* that an executor keeps it explicit.

---

## 6. PROPOSAL (modeling only — NOT built)

Shape of an eventual texture-read ledger + guard, in increasing risk:

**Tier A — buildable-low-risk (the only tier worth scheduling now):**
- `RenderCore/tex_ledger.h` — a header-only singleton twin of `FboLedger`: owners call
  `texLedger().registerTex(glName, target, RenderResourceId)` at creation for the 4-6
  id-backed textures (`shadowDepthTex_`, `dynShadow*`, `g_handle`, `sceneDepth/Normal/Color`).
- A probe in `ambientProbeAtPassBegin()` (already the most-covered seam): loop active units,
  `glGetIntegerv(GL_TEXTURE_BINDING_2D[_ARRAY])`, resolve via ledger, and for each pass's
  id-backed `reads[]` assert it is bound on *some* unit. Unknown GLuint → skip (no false
  positive), identical to the FBO ledger discipline.
- Default-ON read-only counter (`g_texLatchMismatchCount`) exported `extern "C"` for the
  debug-state dump + a `scripts/check-tex-ledger.py` tier gate. Rendering stays byte-identical.
- Cost: ~1 header + ~6 register calls + ~30-line probe. Mirrors a pattern already shipped
  twice (ambient guard, FBO ledger). Risk: LOW.

**Tier B — modeling, defer:**
- Add an **expected-target column** to the occupancy story and a static lint for
  `2D`-vs-`2D_ARRAY` aliasing on multiplexed units (the §3 systemic hazard). This is the
  highest-value lint but needs the shader-variant work the banked recon said to defer behind.
- Per-pass `binds[]` vs `inherits[]` declaration in `RenderPassContract`. Given §2 found
  ZERO inherited reads, this would be all-`binds`, so it documents rather than catches —
  low marginal value until an executor actually starts reordering.

**Stays recon (not buildable as a guard):**
- The atlases (no `RenderResourceId`) — opaque; would need a whole id-assignment project
  (TEXHANDLE migration / GpuBuffer arc), out of scope.
- SHADOWDEBUG-2DARRAY NVIDIA confirm — needs a vendor run (NEEDS_VENDOR_TEST).

**Honest sizing/verdict:** Tier A is a clean ~1-day slice that *locks in* the (already-good)
explicit-binding state so an executor can't silently regress it — but it catches no current
bug (§2 is clean). Its value is **regression-prevention for the executor**, not bug-finding.
Tier B's aliasing lint is the only thing that would catch a *latent* hazard, and it is gated
behind shader-variant. Concurs with `texture-unit-ownership-recon-1.md`: **LOWER value than
the FBO/ambient seams** — build Tier A only if/when an executor slice is actually imminent.

---

## 7. EXECUTOR IMPACT

An executor that owns passes and makes all bindings explicit would **break any inherited-latch
ghost**. The finding here is the good news: **there are none on the default path** (§2). So the
executor's risk is not "discovering hidden ghosts" but "preserving the existing explicit
discipline + per-pass-rebind convention."

Specific things the executor MUST keep explicit (today they already are — do not regress):

1. **Shadow maps are PostProcess-only, bound explicitly on units 2/3/4** (`gos_postprocess.cpp:2128-2137`).
   The `reads[]={ShadowDynamicMap}` on Terrain/Mech/StaticProp/Veg is an ORDER constraint
   (Shadow-before-geometry), NOT a draw-time sample. An executor that "satisfies" that read by
   binding a shadow texture for the geometry pass would be WRONG (geometry shaders don't sample
   it; it would just waste a unit and risk the §3 aliasing on units 2-4). Keep the ordering edge;
   do not materialize it as a geometry texture bind.
2. **The per-pass-rebind on multiplexed units 0-5 must survive reordering.** If an executor
   reorders passes, the rebind-before-draw convention is what prevents the §3 `2D`/`2D_ARRAY`
   aliasing from becoming a live wrong-target sample. Any reorder MUST preserve "each pass
   re-binds its own units; nobody inherits."
3. **`MaterialGpuBuffer` slot-2 save/restore** (`gos_mech_batcher.cpp:2504-2506`) and the
   particle SSBO 14-16 symmetric unbind must survive — these are the SSBO analog of the same
   discipline.
4. **`glActiveTexture(GL_TEXTURE0)`-at-exit + `gos_InvalidateRenderStateCache()`** at the end
   of PostProcess (`:2632`, `:2646`) is the backstop that resets unit state for the HUD/UI pass.
   An executor that owns the post→UI boundary must keep an equivalent reset, or the
   SHADOWDEBUG-2DARRAY-class leak (and any future one) reaches the 2D HUD consumer.

No ghost must be "made explicit before an executor can proceed" — they are already explicit.
The executor's job is to NOT introduce the first one.
