# TERRAIN-AA-RECON-1 — Anti-aliasing feasibility recon

Branch: `claude/nifty-mendeleev`. READ-ONLY recon (no edits). GL 4.3 Core / GLSL 4.30.
Symptom: jagged/crawling aliasing on straight geometric edges, worst at map edges.

## 1. Current AA state — NONE

- **MSAA at context: commented out.** `GameOS/gameos/gos_render.cpp:184-185` —
  `//SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS,1)` / `MULTISAMPLESAMPLES,4` disabled
  ("disable, and add as setting later"). Editor mirror also dead: `editor/EditorGosRender.cpp:406-407`.
- **Default framebuffer is single-sampled** (no MULTISAMPLE attribs requested).
- **Scene FBO is single-sampled** `GL_RGBA16F` `GL_TEXTURE_2D` (NOT `*_MULTISAMPLE`):
  `gos_postprocess.cpp:840-847` (color), depth at ~891. HDR-capable, no MSAA.
- **No `glEnable(GL_MULTISAMPLE)`, no `glRenderbufferStorageMultisample`, no
  `glTexImage2DMultisample`, no `glBlitFramebuffer` MSAA-resolve** anywhere in engine code.
- **FXAA was DELETED** — commit `9c2187d8` (DEAD-POST-FX-CLEANUP-1b "remove dead bloom/ACES/FXAA"),
  alongside `92d3a821` (god rays) + `3e1f9e0a` (docs). No FXAA/SMAA/TAA in `shaders/`
  (grep empty). Leftover hooks, both INERT:
  - `GameOS/gameos/gos_validate.cpp:22,39,45` — `s_config.fxaaOverride` parsed from a debug
    arg string but has NO consumer (dead stub).
- **No AA toggle in graphics options UI** (`GuiRuntime/GraphicsOptionsWindow.cpp` — grep empty).

## 2. Post-process chain / composite

Single fullscreen composite pass = `gosPostProcess::endScene()` `gos_postprocess.cpp:2378`.
- Scene rendered forward into `sceneFBO_` (RGBA16F). endScene runs HZB/cluster/lightgrid
  (all gated, inert) then the composite.
- **Composite**: `gos_postprocess.cpp:2482-2583`. `applyPipeline(PostProcessComposite)`
  (depth off, blend off, cull off) → `compositeProg_` = `shaders/postprocess.frag` →
  samples `sceneColorTex_` at unit 0 (`:2566`) → `glDrawArrays(GL_TRIANGLES,0,6)` (`:2579`)
  into **FBO 0** (backbuffer, bound earlier in endScene; MC2_FORCE_43 viewport at 2460-2467).
- `postprocess.frag` already does exposure + sunset grade + vignette + view-mode (thermal/
  nightvision/objectID) tonemap-style work. **This is the free fullscreen pass.**
- Edge fog / OOB fog (`edge_fog.frag`, `fog_oob.frag`) are SEPARATE earlier fullscreen passes
  that write into the scene before composite.

**FXAA/SMAA insertion point**: inside this composite. `sceneColorTex_` IS available as a
sampler2D here. Two clean options: (a) add an FXAA branch at the END of `postprocess.frag`
operating on the final graded color (cheapest — zero new passes/FBOs, but it AA's a luma that
includes vignette/grade), or (b) a dedicated FXAA pass reading `sceneColorTex_` → small LDR
FBO → composite samples that. UI draws AFTER composite, so post-AA never touches HUD (good —
HUD is 2D, must not be blurred).

## 3. Renderer type — FORWARD (hybrid MRT side-channel), NOT deferred

- Terrain frag writes `layout(location=1) GBuffer1` (`gos_terrain.frag:32`, guarded by
  `#ifdef MRT_ENABLED`) — but this is NOT a deferred G-buffer for lighting. Per the
  `[RENDER_CONTRACT]` header (`gos_terrain.frag:11-21`) GBuffer1 carries packed
  shadow-handled normals consumed by a **screen-space shadow pass** (`shadow_screen.frag`,
  compiled `gos_postprocess.cpp:552-553`). Lighting is computed IN the forward frag (PBR
  splat + shadow sample), color written to location 0.
- => This is a **forward renderer with an MRT side-channel**. There is no deferred
  lighting/resolve stage, so hardware MSAA does NOT incur per-sample lighting cost. MSAA
  here is "ordinary" forward MSAA: multisample sceneFBO color+depth, resolve before post.
  The GBuffer1 MRT attachment would also need to become multisampled (or be split off).

## 4. Edge-specific diagnosis — TWO distinct phenomena

The "pixel jumps on straight lines at map edges" is most likely DUAL, and full-screen AA
only fixes part of it:

a. **Geometric aliasing of straight terrain/mech silhouette edges** — real subpixel
   aliasing + temporal crawl when the camera scrolls. FXAA/SMAA/MSAA help this.

b. **Hard-edged boundary masks at the map edge** — `edge_fog.frag` uses `smoothstep` ramps
   (soft) but `fog_oob.frag:62` does a hard `worldDir.z < -0.22` cutoff and the
   inner/outer fill in `edge_fog.frag:77-78` (`step(0.0,-distFromEdge)`) creates a
   sharp world-space boundary line. The **MC2_FORCE_43 pillarbox** (`gos_postprocess.cpp:
   2460-2467`) also draws hard black bars. These are screen-space hard edges that a
   post-AA pass WILL smooth, but they are not "geometry crawl" — they are authored hard
   transitions. If the user's complaint is specifically the fog/oob/letterbox boundary
   line shimmering, the targeted fix is to soften those masks (widen smoothstep, antialias
   the `step`), NOT a full AA pipeline.

**Recommend reproducing first** (which edge is jumping) before committing to a heavy AA path:
if it's silhouette crawl → AA; if it's the boundary mask line → edge-smoothing in the 2-3
named shaders is far cheaper and more correct.

## 5. Ranked AA recommendation (this renderer)

| Rank | Option | Insertion | Fixes | Risk | Map-edge symptom |
|---|---|---|---|---|---|
| 1 | **FXAA revive** | append branch to `postprocess.frag` composite (`gos_postprocess.cpp:2482-2583`); no new FBO | geometric edges + some crawl, cheap (~1 pass) | LOW — single shader, gate `MC2_FXAA`, no relink-heavy C++. Caveat: luma includes grade/vignette | YES for silhouette; YES for hard boundary lines (it blurs them) |
| 2 | **SMAA** | dedicated pass reading `sceneColorTex_` → edge/blend-weight/neighborhood (3 passes + 2 helper textures) | best edge quality, less texture-blur than FXAA | MED — 3 passes, area/search LUTs, more FBOs; more GL plumbing in endScene | YES, sharper than FXAA |
| 3 | **MSAA** | multisample `sceneFBO_` color+depth+GBuffer1 (`gos_postprocess.cpp:840-847`) + `glBlitFramebuffer` resolve before post; OR re-enable context MSAA `gos_render.cpp:184` (but post-FBO is the real target) | true geometric AA, no temporal crawl on statics, no texture blur | HIGH — sceneFBO is RGBA16F + MRT(GBuffer1) + screen-space shadow samples it; all must go multisampled or resolve-first. Reverse-Z depth + many fullscreen passes sample sceneDepthTex_ → each needs a resolved copy. Touches the whole post chain. VRAM 2-8x | YES geometric; does NOT fix authored hard fog/letterbox lines |
| 4 | **TAA** | needs per-pixel motion vectors + history buffer + jittered projection | best temporal (kills crawl) | VERY HIGH — **NO motion-vector buffer exists** (grep: no velocity/motion attachment; GBuffer1 is normals only). Would need new MRT, jitter, reprojection, disocclusion. Large build. | YES incl. crawl, but disproportionate effort |

**Recommendation**: Start with (1) FXAA revive behind `MC2_FXAA` gate IF repro shows silhouette
crawl; if repro shows the boundary mask line, fix the hard `step`/cutoff in `fog_oob.frag:62`
+ `edge_fog.frag:77-78` (+ pillarbox bar AA) first — cheaper and addresses the actual map-edge
symptom. SMAA (2) is the quality upgrade path if FXAA blur is objectionable. MSAA (3) only if
geometric correctness on statics matters more than effort. TAA (4) is out until motion vectors exist.
