# TERRAIN-EDGE-FEATHER-RECON-1 — map-perimeter hard line (Option B / geometry slice)

> ## SHIPPED 2026-06-26 (Option A, both halves) — gated default-OFF, byte-identical
>
> The map edge is BOTH water and terrain (user). Two companion feathers, both color-fade
> the outer band to `EDGE_HAZE_SKY` over `smoothstep(halfMap-256, halfMap-32, dist)`:
> - **TERRAIN-EDGE-FEATHER-1** — `terrain_lod_chunk.frag` (port of legacy `gos_terrain.frag`
>   edge-haze the chunk path dropped). Gate `MC2_TERRAIN_EDGE_FEATHER` + `_STRENGTH`.
>   `lit = mix(lit, EDGE_HAZE_SKY, edgeHazeAmount(v_worldPos.xy, u_halfMap)*str)`, guarded
>   `u_halfMap>0` (greybeard footgun). C++ upload `gos_terrain_lod_chunk.cpp`.
> - **WATER-EDGE-FEATHER-1** — `gos_terrain_water_mdi.frag` (the live default water frag,
>   `renderWaterFastPath`). Gate `MC2_WATER_EDGE_FEATHER` + `_STRENGTH`. Because water is
>   alpha-blended it BOTH tints `col→EDGE_HAZE_SKY` AND **lifts alpha→0** (true dissolve, not
>   a hazed-opaque line). `u_halfMap = getMapHalfExtent()` (world half-extent vs WorldPos.xy,
>   the canonical value legacy terrain + edge_fog use). C++ upload `gameos_graphics.cpp` ~3424.
>
> **Set BOTH gates for the full water+terrain map-edge feather.**
>
> **Verification:** math-proven (at rim edgeHazeAmount→1 ⇒ terrain→sky / water→sky+alpha0; 224 WU
> smooth band; gate-OFF skipped = byte-identical). Adversarial (mc2-water-depth-expert) caught the
> water-only gap → companion built. Greybeard (mc2-terrain-indirect-expert) caught the `u_halfMap==0`
> footgun → guarded; confirmed faithful legacy port (legacy never feathered water either). Build clean,
> both shaders compile, interior water byte-identical with gates ON.
>
> **RESIDUALS (not blockers):** (a) `EDGE_HAZE_SKY` is a hardcoded grey, not the live fog tint
> (`edge_fog u_fogColor`) — promote to a uniform if the band reads as a wrong grey against the cloud
> bank. (b) `edge_fog.frag:55` skips water-level pixels, so terrain-feather *behind* water relies on the
> water companion (now built). (c) The perimeter isn't framable by the capture bookmarks — verify live
> by panning a camera to the actual map edge.


READ-ONLY recon. Worktree: nifty-mendeleev. Symptom (user real complaint): a HARD,
perfectly-straight MAP-PERIMETER line cutting the horizon/water — the terrain-vs-void/water
edge. FXAA only softens the staircase; goal is to FEATHER the knife edge so the perimeter
fades into water/fog/void over a band. Prior recon: docs/recon/terrain-aa-recon-1-fbo-edges.md.

## 1. ROOT CAUSE — what produces the hard straight line

Default production terrain is the LOD-chunk renderer (gos_terrain_lod_chunk.{cpp,vert,frag}, 8z
cutover). The map is a grid of chunks; geometry STOPS at the last map cell.

- Grid extent: mclib/terrain.cpp:1051 s_halfMap = mapSide*128*0.5; vert builds world XY from
  mapX/mapY clamped to [0, u_mapSide-1] and u_halfMap (terrain_lod_chunk.vert:43-44,80-81).
  Outermost surface vertices sit exactly on worldX/Y = +/-u_halfMap — a straight axis-aligned square.
- Map-boundary edges get NO skirt. Skirt edge-mask build skips boundary neighbours:
  mclib/terrain.cpp:2033-2035 (nx<0 || ny<0 || nx>=side || ny>=side -> continue; // map boundary ->
  no skirt). Comment at :2014 explicit: "map-boundary edges draw none". So the perimeter is a bare
  terminating triangle row — vertical silhouette of the terrain top surface against whatever is behind.
- Behind/below it: NO terrain water plane in this path; void/OOB pixels painted by fog_oob.frag
  (sea-of-clouds, smoothstep horizon) and the cloud bank by edge_fog.frag. Both ALREADY feathered
  (edge_fog.frag:77 innerRamp smoothstep). Terrain draws opaque (alpha forced 1.0,
  terrain_lod_chunk.frag:701) and writes depth (pre-divide bias, vert:86).

Pin: the hard line is (a) the terrain SILHOUETTE where geometry ends — a long, near-axis-aligned,
single-sampled opaque edge where terrain depth/color stops abruptly and the (feathered) OOB fog begins
behind it. NOT a terrain-color-vs-water hard transition (b), NOT terrain-vs-skybox (c); the fog shaders
that paint the void are already soft. The knife edge is the terrain own opaque border — nothing fades
the terrain TOWARD the void over a band. (Aliasing/crawl on that edge is the secondary FXAA-tier
symptom from recon-1; this slice targets the FEATHER, not the AA.)

## 2. EXISTING SKIRTS — generation + perimeter-skirt feasibility

- Skirt geometry built once per patch in buildPatchCache: four edge strips (N,S,W,E), each a quad-strip
  of {surface vert isSkirt=0} + {below vert isSkirt=1} pairs (gos_terrain_lod_chunk.cpp:264-308). Vert
  lowers the below vert: h -= float(isSkirtFlag) * u_skirtDepth (terrain_lod_chunk.vert:78).
- Skirt verts share the edge surface vertex worldPos.xy -> sample the SAME colormap -> seamless, NOT
  darkened (terrain_lod_chunk.frag:380-386); skirt normal forced flat-up so it is not a dark wall
  (frag:498-499). u_skirtDepth chosen per block, capped 256 (terrain.cpp:2057-2065).
- Per-edge mask + per-edge depth uploaded via gos_TerrainLodChunk_SubmitDrawCommands(... skirtDepths,
  skirtEdgeMasks ...) (gos_terrain_lod_chunk.cpp:515-521, draw at :963-989).

Perimeter skirt feasibility (Option B): YES, machinery exists. To emit a perimeter skirt you would
(a) REMOVE the boundary continue at terrain.cpp:2034-2035 for the outer edge so boundary chunks set the
map-edge mask bit, and (b) give that perimeter skirt a deliberately LARGE depth (drop to/below fog
height) + a fade. But the existing skirt is colormap-seamless and flat-up-lit; to make it READ as a
fade-to-fog you must additionally darken/alpha it toward fog colour along its drop (new frag branch
keyed on a perimeter-skirt flag, since current skirts intentionally do NOT darken). That is a new vertex
flag + new frag path + blend state (skirts currently opaque). Non-trivial; it also changes the
silhouette (skirt wall becomes the new visible edge lower down).

## 3. MAP BOUNDS availability to the chunk system

Fully available, both stages:
- u_mapSide + u_halfMap are linked-program uniforms on BOTH vert and frag (terrain_lod_chunk.vert:6-7,
  terrain_lod_chunk.frag:63-64); driver sets s_halfMap=mapSide*128*0.5 (gos_terrain_lod_chunk.cpp:1051,
  uploaded :642).
- Frag already has v_worldPos (world XY) -> edge-distance d = u_halfMap - max(abs(wx),abs(wy)) is a
  one-liner, computable per-pixel with ZERO new uniforms.
- Same metric edge_fog.frag:71-72 already uses (u_halfExtent - max(abs(planeXY.x),abs(planeXY.y))),
  so an Option-A feather is visually consistent with the existing cloud-bank ramp.

## 4. SAFETY — visual-only confinement

A perimeter feather/skirt CAN be visual-only. The chunk render path is a pure draw consumer; does not
feed gameplay:
- Pathing / MOVE (packet-4), object placement, getTerrainElevation read the CPU heightfield
  (Terrain/MapData), NOT chunk geometry. Option A (frag colour blend) touches no geometry at all.
  Option B adds DOWNWARD-only skirt verts sharing surface XY — never raise terrain, never change sampled
  elevation, below the play surface.
- Picking / objectID (R32UI GBuffer, COLOR2) is written by object draws; terrain frag writes only
  fragColor + GBuffer1 (terrain_lod_chunk.frag:74-75,701-702). A perimeter band that only blends terrain
  colour toward fog, or a downward skirt, moves no pickable object.
- Caveat (Option B only): skirt verts DO write depth. A perimeter skirt dropping to fog height would
  occlude OOB fog behind it and become the new silhouette — must be depth/blend-audited. Option A writes
  the same terrain fragments it already does -> no depth change.

Verdict: visual-only is achievable. Option A is inherently safe (colour-only, no geometry, no depth
delta). Option B needs a depth/blend audit but still does not perturb pathing/elevation/picking.

## 5. RECOMMENDATION — Option A (shader edge-distance feather), gated default-OFF

Lower-risk first slice = Option A (frag edge-distance feather in terrain_lod_chunk.frag):

- Compute float edgeD = u_halfMap - max(abs(v_worldPos.x), abs(v_worldPos.y)); (no new uniform; same
  metric as edge_fog.frag).
- float feather = smoothstep(0.0, u_edgeFeatherBand, edgeD); blend final lit toward the fog/void colour
  (reuse u_fogColor plumbing or the fog_oob tone) as edgeD -> 0, so terrain DISSOLVES into the cloud bank
  over a band instead of ending on a knife edge. Place just before the fragColor = vec4(lit,1.0) write
  (frag:701). Optionally also drop alpha at the very edge if the pass is composited over fog.

Why A over B:
- A is COLOUR-ONLY: no new vertex attribute, no skirt rebuild, no depth/blend state change, no
  winding/cull concern, no silhouette-shape change -> trivially byte-identical when gated OFF.
- A reuses the exact edge-distance + smoothstep idiom already proven in edge_fog.frag, so it composites
  consistently with the existing feathered cloud bank that already paints the void behind the edge.
- B fights the existing skirt contract (skirts deliberately seamless + opaque + flat-lit); making them
  fade needs a new flag/branch/blend and re-introduces a (lower) silhouette. Higher blast radius for the
  SAME perceived result on a far, low-detail border.

Gate design (default-OFF, byte-identical): MC2_TERRAIN_EDGE_SKIRT (or MC2_TERRAIN_EDGE_FEATHER) ->
driver reads env once, uploads int u_edgeFeather (0 = skip whole block -> pixel-invariant) + float
u_edgeFeatherBand (WU, e.g. 256-768). When 0 the new frag block is not entered -> output unchanged ->
passes the byte-exact visual gate. If Option B is later wanted, gate it on the same env behind the
boundary-continue removal so the default skirt build is untouched.

Follow-up if A insufficient: pair with the recon-1 FXAA/render-scale AA pass to also kill residual edge
crawl (orthogonal: A removes the hard line, AA removes the staircase shimmer).
