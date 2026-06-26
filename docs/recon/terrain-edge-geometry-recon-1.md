# TERRAIN-EDGE-GEOMETRY-RECON-1 — kill the perimeter SILHOUETTE (Option B / geometry)

READ-ONLY. Symptom: Option-A color feather (terrain+water rim tinted to sky) tints but the
terrain plane still ENDS at a hard geometric edge; at grazing camera angles the straight knife
silhouette persists against the sky. This slice = perimeter GEOMETRY.

## 1. PERIMETER SKIRT — emission feasibility

Machinery exists; emission is small. To emit a boundary skirt:
- mclib/terrain.cpp:2033-2035 `if (nx<0||ny<0||nx>=side||ny>=side) continue;` — the boundary
  `continue` skips setting the edge-mask bit. Replace with: set `edgeMask |= (1u<<e)` on the
  out-of-bounds (map-edge) case behind a new env gate (keep the LOD/cull logic untouched for in-map).
- depth: mclib/terrain.cpp:2057-2065 computes per-block skirt depth, capped 256
  (`MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX`). A perimeter skirt needs a DELIBERATELY large depth
  (own constant, bypass the elevRange-derived value), not the crack-sized 32..256.
- vert math already supports it: shaders/terrain_lod_chunk.vert:78 `h -= float(isSkirtFlag)*u_skirtDepth`.
- build: gos_terrain_lod_chunk.cpp:284-307 (buildEdge N/S/W/E quad-strips, SkirtVertex{lx,ly,isSkirt}).
- draw: gos_terrain_lod_chunk.cpp:985-993 (per-edge skirtDepths gate). Uniform feed terrain.cpp:2065.

VERDICT: skirt emission is FEASIBLE with ~3 edits (un-continue + custom depth + gate). BUT see §2 —
a skirt does NOT kill the silhouette at all angles.

## 2. DEEP / FAR — which actually removes the silhouette? (the crux)

Skirt verts share the boundary surface XY (vert:43-44,80-81 clamp mapX/mapY to [0,mapSide-1],
worldX/Y = mapX*128 - halfMap). isSkirt only subtracts from `h`. So a skirt is a STRICTLY VERTICAL
WALL dropping straight down at worldXY = ±halfMap. Options:
- (a) drop straight down (current skirt math): a vertical wall. Edge-on it is a thin line; it just
  MOVES the silhouette downward — does NOT remove it. **Insufficient.**
- (b) drape outward+down at a slope: requires an OUTWARD XY offset the current vert cannot produce
  (XY is pinned to in-bounds map cells). Needs new geometry (see §3).
- (c) extend far outward as an apron so the rim falls below the view frustum / into fog: also needs
  outward geometry (§3).

Camera pitch: code/mechcmd2.cpp:1271-1283 — `MAX_PERSPECTIVE` default 88deg (capped 90),
`MIN_PERSPECTIVE` default 18 (capped -89); `projectionAngle` (gamecam.cpp:1037) is pitch off-vertical.
At 88deg the camera looks NEARLY HORIZONTAL → nearly edge-on to the terrain plane. A vertical wall (a)
is therefore seen edge-on and remains a silhouette line. **Only an OUTWARD-draping apron (b/c) that
slopes/extends below the horizon line and fades into fog removes the silhouette at the 88deg extreme.**
Pure depth (drop) is not enough; you need horizontal extent so the rim is beyond/below frustum and
hazed before it ends.

## 3. APRON ALTERNATIVE (recommended geometry)

Extend the grid OUTWARD past ±halfMap with extra fading rings. The pins to change:
- vert:43-44 `mapX/mapY = clamp(origin+localOffset, 0, mapSide-1)` and vert:80-81
  `worldX = mapX*128 - halfMap`, `worldY = halfMap - mapY*128`. The clamp is LOAD-BEARING for the
  height-SSBO index (`heights[mapX+mapY*mapSide]`), so you CANNOT just widen the clamp. Add a separate
  apron attribute: a new vertex flag/offset (e.g. `aprOut` in localOffset.z or the spare isSkirt pad,
  gos_terrain_lod_chunk.cpp:272 SkirtVertex._pad) that the vert adds to worldXY AFTER the clamped
  height sample — sample height at the clamped edge cell, place vertex at edge + N*128 outward, and
  ramp `h` downward (and fade, §5). This makes an outward-draping apron skirt (case b/c).
- Build: extend buildEdge (gos_terrain_lod_chunk.cpp:284-307) to emit 1..K extra outward rings on
  boundary edges only, or add a dedicated apron strip generator gated by env.
APRON is more code than the §1 skirt but is the ONLY form that actually hides the edge at 88deg.

## 4. SAFETY — visual-only confinement (CONFIRMED extends to perimeter)

The chunk renderer is a pure draw consumer; gameplay reads the CPU heightfield (Terrain/MapData), not
chunk geometry:
- Pathing (packet-4), getTerrainElevation, object placement, picking/objectID — all CPU-side; terrain
  frag writes only fragColor + GBuffer1 (terrain_lod_chunk.frag:698-701,718), NOT the R32UI objectID
  GBuffer. A downward/outward skirt moves NO pickable object and changes NO sampled elevation.
- Skirt/apron verts are DOWNWARD-only (and OUTWARD past the play area) — never raise terrain, never
  alter the in-bounds surface. Same visual-only contract as existing skirts holds.
- ONE caveat (geometry, not Option A): skirt/apron verts WRITE DEPTH. A perimeter skirt dropped to fog
  height OCCLUDES the OOB cloud-bank fog (edge_fog/fog_oob) behind it and becomes the new visible
  surface — must be depth/blend-audited + faded (§5), else it is a grey wall in front of the fog.

VERDICT: visual-only is achievable; depth-occlusion-of-fog is the only new hazard.

## 5. DEPTH / FADE — make the skirt dissolve, not be a grey wall

Skirt frags currently shade IDENTICALLY to surface (terrain_lod_chunk.frag:383-389 sample same
colormap via shared worldPos.xy; :501-502 force flat-up normal so the wall is not dark). They are NOT
darkened/faded — opaque, depth-writing. They then FALL THROUGH to the TERRAIN-EDGE-FEATHER-1 block
(frag:714-717): `haze = edgeHazeAmount(v_worldPos.xy, u_halfMap)*strength; lit = mix(lit, EDGE_HAZE_SKY, haze)`.
Because a perimeter skirt vert shares boundary XY (= exactly ±halfMap), `edgeHazeAmount → 1` already, so
**the existing color feather ALREADY fully tints a perimeter skirt to sky** with NO new frag code — IF the
feather gate is on. An outward apron's outer rings sit BEYOND halfMap; `edgeHazeAmount` must be extended
to ramp past halfMap (currently smoothstep(halfMap-256, halfMap-32, dist)) so apron rings keep fading,
or drop their alpha. Caveat: the pass is opaque (alpha forced 1.0, frag:718) and writes depth, so the
hazed-grey skirt still OCCLUDES the cloud fog behind it — to truly dissolve, the perimeter skirt needs
either (i) alpha-blend to 0 toward the rim (new blend state for the skirt draw) OR (ii) accept the
sky-tinted opaque rim reading as horizon. (i) is the clean dissolve; (ii) is zero-extra-code if
EDGE_HAZE_SKY matches the fog tone.

## RECOMMENDATION (caveman)

- Vertical skirt (§1) = cheap but DOES NOT kill silhouette at 88deg pitch (vertical wall seen edge-on).
- Silhouette only dies with an OUTWARD-draping APRON (§3): new outward vertex offset (clamp stays for
  height index), extra fading rings past ±halfMap, ramp h down + fade haze past halfMap, alpha→0 at rim.
- Reuse: existing edge-feather (frag:714-717) already tints boundary-XY skirt verts to sky for free;
  extend edgeHazeAmount to ramp past halfMap for apron rings.
- Safety: visual-only holds (no pathing/elev/pick/objectID). ONLY hazard = skirt depth occludes OOB
  fog → must alpha-fade or sky-tint to match the cloud bank.
- Gate default-OFF (e.g. MC2_TERRAIN_EDGE_APRON): u_apronRings=0 → no extra geom, byte-identical.
