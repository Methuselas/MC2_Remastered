# TERRAIN-AA-RECON-1 — FBO architecture + map-edge rendering

READ-ONLY recon. Worktree: nifty-mendeleev. Symptom: "edges of the maps have weird
pixel diffs/jumps on straight lines."

## 1. FBO ARCHITECTURE — all single-sampled, NO MSAA

Created in `gosPostProcess::createFBOs` (`GameOS/gameos/gos_postprocess.cpp:833`):

| Target | Attach | Format | Filter | File:line |
|---|---|---|---|---|
| sceneColorTex_ (HDR scene) | COLOR0 | GL_RGBA16F | LINEAR | gos_postprocess.cpp:840-847 |
| sceneDepthTex_ | DEPTH_STENCIL | GL_DEPTH24_STENCIL8 | NEAREST | :878-886 |
| sceneNormalTex_ (GBuffer1: rgb=normal, a=shadowHandled) | COLOR1 | GL_RGBA16F | NEAREST | :889-896 |
| sceneObjectIdTex_ (opt, MC2_OBJECT_ID_BUFFER) | COLOR2 | GL_R32UI | NEAREST | :903-913 |
| ssaoColorTex_ (half-res) | COLOR0 | GL_R16F | LINEAR | :950-960 |
| waterReflColorTex_/Depth | COLOR0/DEPTH | RGBA16F / depth | — | :976-996 |
| shadowDepthTex_ + dummy color | DEPTH/COLOR0 | depth | — | :3337-3361 |
| dynShadow*, dynShadowArray, dynamicFullMap | DEPTH | depth | — | :3664-3820 |
| hzbFBO_ | — | RGBA16F | — | :1060-1116 |

**Sample count = 1 everywhere.** Zero `glRenderbufferStorageMultisample`,
`glTexImage2DMultisample`, `glBlitFramebuffer`, or `GL_MULTISAMPLE` in the whole
file (grep empty). The main scene FBO is a deferred-ish MRT (color + encoded
normal + shadowHandled flag + optional objectID), SINGLE-SAMPLED.

**Final resolve = fullscreen-quad composite, not a blit.** `endScene()`
(gos_postprocess.cpp:2378) binds default FB (`glBindFramebuffer(GL_FRAMEBUFFER,0)`
:2453), optional FORCE_43 pillarbox viewport (:2458-2468), then draws a fullscreen
quad with `postprocess.frag` sampling sceneColorTex_ (compositeProg_ :2489).
postprocess.frag (126 lines) does exposure/tonemap/color-grade/vignette only —
**no FXAA/SMAA/edge filtering**. `inverseScreenSize` is declared (:8) but used
only for vignette/grade, never for neighbor-tap AA.

**There is NO anti-aliasing pass anywhere.** No fxaa/smaa/taa shader files exist
(grep of shaders/ empty). The engine renders aliased single-sampled and never
resolves edges.

## 2. MSAA FEASIBILITY — verdict: post-AA is the sane path

Hardware MSAA would break / cost a lot:
- Scene FBO is MRT deferred-style (COLOR1 packs world normal + the GBuffer1
  `shadowHandled` alpha flag terrain writes; COLOR2 = R32UI objectID). MSAA on an
  **integer R32UI** target + per-sample shadow-flag semantics needs either
  per-sample shading or a custom resolve — a plain glBlitFramebuffer resolve
  averages the shadowHandled flag and the objectID (garbage for pick / shadow
  skip). NEAREST normal/id buffers are not average-resolvable.
- Terrain is SSBO/compute/tessellation-driven; lighting samples GBuffer1 — MSAA
  forces resolve-before-lighting or sample-rate lighting, a deferred rework.
- Depth is sampled in post (fog_oob, edge_fog, SSAO, HZB) as a plain TEXTURE_2D;
  multisample depth changes every consumer.

**Verdict: MSAA is NOT realistic without a deferred-resolve rework. Post-AA
(FXAA/SMAA on the composite, or supersample/render-scale) is the only sane near-
term path.** A render-scale (render at 1.5–2x, LINEAR downsample at composite) is
the cheapest robust win given the existing LINEAR sceneColorTex_ + fullscreen
composite already in place.

## 3. MAP-EDGE RENDERING — what draws the literal boundary

- **Terrain boundary:** outermost chunk row/col terminates; vertical **skirts**
  (terrain_lod_chunk.vert:2 isSkirtFlag, u_skirtDepth) fill inter-chunk seams.
  Production skirts sample the SAME colormap UV as the adjacent edge surface vertex
  → seamless, NOT a hard color edge (terrain_lod_chunk.frag:380-386, 450-452).
  The skirt is a vertical wall pulled down; debug-darken only when u_diag set.
- **OOB sky/void:** `fog_oob.frag` — 3D-FBM sea-of-clouds on void pixels
  (rawDepth~0). Smoothstep horizon fade (:65). No hard line.
- **Edge cloud bank:** `edge_fog.frag` — ray/height-plane intersection gives a
  world-fixed XY boundary; **inner ramp is smoothstep(u_fogStart,0,distFromEdge)**
  (:77) → FEATHERED. Default start=50WU. Outside = solid fill (:78). This edge is
  already soft; it is camera-scroll-stable by design.
- **Fog-of-war / unseen:** separate (not in this slice's files); axis-aligned.
- **Pillarbox:** FORCE_43 (default OFF) draws black bars via glClear + viewport
  inset (gos_postprocess.cpp:2458-2468) — a HARD axis-aligned black/scene edge at
  the pillarbox seam, but only when MC2_FORCE_43=1.

**Hard straight edges that alias/shimmer:** (a) the outermost terrain silhouette
against void/water — axis-aligned map perimeter, single-sampled, no AA → classic
geometric edge aliasing on the straight boundary; (b) FORCE_43 pillarbox seam IF
enabled. The two fog shaders are already feathered and are NOT the culprit.

## 4. DIAGNOSIS

Best read: **(a) geometric edge aliasing**, secondarily (c) temporal crawl.

Evidence: pipeline is fully single-sampled with zero AA pass (§1); the map
perimeter is the strongest set of long axis-aligned straight silhouette edges in
the scene (terrain-vs-void/water, and any building/cliff hard edges along the
border). "Pixel diffs/jumps on straight lines" under a scrolling RTS camera = the
textbook signature of un-antialiased near-axis-aligned edges crawling sample grid.
The fog edges are smoothstep-feathered (§3) so they are unlikely; a missing
feather is NOT the cause here — the boundary fogs already feather.

NOT likely (b) hard-mask-no-feather: the two boundary masks (edge_fog, fog_oob)
already smoothstep. Only FORCE_43 is an un-feathered hard edge, and it's default-OFF.

### Fix options
- **Fast targeted:** none that's truly targeted — the aliasing is the terrain/object
  silhouette itself, not one mask. (If FORCE_43 is on, feather the pillarbox seam,
  but that's a niche path.)
- **General (recommended):** add a post-AA stage on the composite. Cheapest =
  **render-scale / SSAA** (render sceneFBO at 1.5–2x, the existing LINEAR
  sceneColorTex_ + fullscreen composite downsamples for free). Next = **FXAA** in
  postprocess.frag (inverseScreenSize already plumbed → drop-in neighbor-tap).
  TAA would also kill temporal crawl but needs motion vectors / history buffer
  (none exist) — large lift.

Recommend: prototype FXAA in postprocess.frag (lowest integration cost, gate it),
fall back to render-scale if edge quality insufficient.
