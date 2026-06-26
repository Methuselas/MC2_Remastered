# TERRAIN-EDGE-GEOMETRY-RECON-1 — water plane + horizon/backdrop slice

READ-ONLY recon. Companion to terrain-edge-feather-recon-1.md (color feather, SHIPPED) and the
terrain-skirt slice (separate). This slice: hide the EDGE GEOMETRY silhouette (water + a backdrop
that covers BOTH terrain and water edges) at all reachable camera angles.

## Why the silhouette survives the fog passes (THE root pin)

`edge_fog.frag` and `fog_oob.frag` are screen-space overlays composited AFTER the scene in
`gosPostProcess::runComposite` (runEdgeFog @ gos_postprocess.cpp:2463, runFogOob @ :2467). Both
gate on the DEPTH buffer:
- `fog_oob.frag:54` — `if (rawDepth > 0.0001) { outFog = vec4(0); return; }` → fires ONLY on
  far-plane / void pixels (no geometry).
- `edge_fog.frag:48,55,60` — for any pixel with geometry it reads geoZ; it returns early for water
  (`geoZ <= u_waterElevation+2` :55) and only fogs geometry ABOVE the water/below cloud-top band.

The terrain top surface at the map rim is OPAQUE geometry that DEPTH-WRITES (terrain_lod_chunk.frag
forces alpha 1.0, writes depth). So the hard terrain top-edge is a real geometric silhouette IN FRONT
of the void; the fog shaders paint the void BEHIND it but never overwrite the opaque rim pixel. Result:
fog feathers the sky behind the edge, the knife-edge geometry itself stays. Same for the water rim —
water depth-writes its own plane edge. Fog is a post-pass; it cannot fill a hole that opaque geometry
already closed. This is why the shipped COLOR feather only tints and does not move the silhouette.

## 1. WATER PLANE EXTENT — extend to horizon ocean?

Water geometry = per in-map water-quad recipes. `renderWaterFastPath` (terrain.cpp:2827) draws
`WaterStream::GetRecipeCount()` records (terrain.cpp:2895-2896) → only cells flagged water inside the
map. GPU draw is `gos_terrain_bridge_renderWaterFast` → `gosRenderer::renderWaterFastPath`
(gameos_graphics.cpp:3078), MDI over per-record commands. So water ALSO terminates at a hard map-internal
boundary (and most maps have no water touching the rim at all).

EXTEND FEASIBILITY: MODERATE. Adding one big surrounding ocean quad at `Terrain::waterElevation`
spanning e.g. ±(halfMap + N) is a single extra draw. Cleanest insertion = a dedicated quad appended in
`renderWaterFastPath` (terrain.cpp:2895 region) OR a new sub-call in `gosRenderer::renderWaterFastPath`
(gameos_graphics.cpp:3078) reusing the same water program/uniforms with a hand-built 4-vert quad. Caveats:
(a) the water shader UVs/tiling derive from `worldUnitsMapSide` (terrain.cpp:2869-2872) — an outer quad
needs its own tiling or it streaks; (b) only helps where the map edge IS at/below water level — a terrain
cliff rim sitting ABOVE sea level still silhouettes against the ocean+sky; (c) alpha-blended water over
the OOB cloud bank may double-tint. Net: kills the WATER edge cleanly, does NOT kill terrain rim that
stands above sea level.

## 2. HORIZON BACKDROP — what already exists

Skybox infra is LIVE: `skybox.{vert,frag}`, `hdri_skybox.{vert,frag}` compiled in gosPostProcess
(gos_postprocess.cpp:488, 509). Draw entries: `renderHdriSkybox` / `renderHdriSkyboxBasis` /
`renderHdriSkyboxInvVP` (gos_postprocess.cpp:2716/2960/3147) + `renderSkybox` (:2688). These are
ALSO depth-gated far-plane fills (same model as fog) — they paint the dome BEHIND geometry, so by
themselves they do not occlude the rim either.

DOME/RING OCCLUDER FEASIBILITY: HIGH and this is the strongest single move. A large distant horizon
RING/CYLINDER (real geometry, radius >> halfMap, centered on camera XY, vertical band from below sea
level up to a horizon height) drawn as actual depth-writing geometry would stand BETWEEN the camera and
the map rim at grazing angles → the rim silhouette is occluded by the ring, and the existing feather/fog
blends the ring into sky. Slot it in the SCENE pass (before post) so it depth-interacts with terrain —
i.e. alongside the terrain/water draws in the gamecam frame (gamecam.cpp ~522-530), NOT in the
post-process composite (post passes are 2D overlays and cannot occlude in depth). Build on the skybox
program for sky color, but it must be a real VBO ring drawn into the scene FBO with depth-write, not the
fullscreen skybox quad.

## 3. (folded into the root pin above)

## 4. CAMERA pitch bound — how reachable is the edge?

`Camera::projectionAngle` is the tilt. Clamp constants: `MIN_PERSPECTIVE = -20.0f`,
`MAX_PERSPECTIVE = 88.0f` (camera.cpp:138,140). Comment at :138: "allow looking up to 20deg above
horizontal." Default tilt = 30deg looking down (camera.h:375). So the player CAN tilt to 20deg ABOVE
horizontal → grazing/near-horizon views ARE reachable and the rim silhouette IS exposed. The edge fix
must therefore be robust to near-horizontal, not just top-down.

LOWER-PITCH OPTION (d): raising MIN_PERSPECTIVE from -20 back toward e.g. +5..+10 (camera.cpp:138) is a
ONE-LINE clamp change that removes most grazing exposure. But it's a gameplay/feel regression (removes
the dramatic low-angle look the -20 was deliberately added for) and a partial fix at high altitude.

## 5. RECOMMENDATION (ranked)

Lowest-risk REAL fix that kills the edge at all angles:

1. **(c) Horizon backdrop ring/dome occluder — BIGGEST single buy.** One distant depth-writing ring in
   the SCENE pass occludes BOTH terrain and water rims from every reachable angle, regardless of sea
   level, and reuses skybox color + existing edge feather to blend. Highest payoff, self-contained,
   gameplay-neutral, gated default-OFF. Risk = getting depth/scale/horizon-height right so it never
   clips real terrain; medium.
2. **(a) terrain perimeter skirt (other recon) — pairs with #1.** The skirt hides the terrain rim at
   moderate pitch and is the right tool if the ring's horizon line ever sits behind a tall rim. Together
   (1)+(a) are belt-and-suspenders.
3. **(b) extend water to horizon ocean plane — narrow.** Cheap, but only fixes water-level edges; a ring
   subsumes it for less map-specific risk. Do only if water-specific shimmer remains after #1.
4. **(d) lower max pitch — cheap mitigation, not a fix.** One-line clamp; use only as a stopgap or to
   bound how aggressive #1 must be.

SINGLE MOVE for most value: **(c) the horizon ring/dome occluder, drawn in the scene pass.** It is the
only approach that hides both edges at all angles in one slice. Combine with (a) skirt for tall rims.
