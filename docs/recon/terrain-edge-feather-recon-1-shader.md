# TERRAIN-EDGE-FEATHER-RECON-1 — shader/Option-A angle

READ-ONLY recon. Worktree: nifty-mendeleev. Slice goal: feather the HARD straight
map-perimeter silhouette (terrain-vs-water/void) into water/fog/void over a band,
gate-OFF byte-identical. Files read: terrain_lod_chunk.frag, gos_terrain.frag,
edge_fog.frag, include/edge_haze.hglsl, GameOS/gameos/gos_terrain_lod_chunk.cpp.

## HEADLINE — Option A already exists in legacy, MISSING in the live chunk path

shaders/include/edge_haze.hglsl IS the Option-A edge-distance color feather:

    const vec3 EDGE_HAZE_SKY = vec3(0.58, 0.65, 0.75);
    float edgeHazeAmount(vec2 worldXY, float halfMap) {
        return smoothstep(halfMap - 256.0, halfMap - 32.0, max(abs(worldXY.x), abs(worldXY.y)));
    }

- Legacy gos_terrain.frag USES it: #include at :9, applied :1054-1056:
  if (mapHalfExtent > 0.0) c.rgb = mix(c.rgb, EDGE_HAZE_SKY, edgeHazeAmount(WorldPos.xy, mapHalfExtent));
  Uniform mapHalfExtent declared gos_terrain.frag:126.
- The DEFAULT live renderer terrain_lod_chunk.frag does NOT — no #include, no fade
  (grep edge_haze/edgeHazeAmount/EDGE_HAZE matches only gos_terrain.frag + header).
  Final output terrain_lod_chunk.frag:701 fragColor = vec4(lit,1.0) writes colormap
  to the perimeter with no feather.

The hard perimeter line = the LOD-chunk path emitting un-hazed terrain to the
silhouette. The path that HAS the fix (legacy gos_terrain.frag) is not the one drawing.

## 1. EDGE-DISTANCE FEASIBILITY — YES, trivially. Zero new uniforms.

terrain_lod_chunk.frag already has both inputs:
- v_worldPos varying: declared :15 (in vec3 v_worldPos), used throughout.
- u_halfMap uniform: declared :64; DRIVER already uploads it —
  gos_terrain_lod_chunk.cpp:384 s_locHalfMap = glGetUniformLocation(...,"u_halfMap").
  Same half-side-length as legacy mapHalfExtent.

edgeDistance = u_halfMap - max(abs(v_worldPos.x), abs(v_worldPos.y));
fade = smoothstep(0, featherWidth, edgeDistance);
=> ZERO new uniforms, ZERO new C++ upload for the constant version. #include the
header + one mix near :701 = the whole change. Mirror in gos_terrain_lod_chunk.cpp
only if you want a tunable width/color (add s_locEdgeFeather* per the s_loc* pattern
at :381-423).

## 2. WHAT TO BLEND TO — fog/horizon color, NOT water

- EDGE_HAZE_SKY = vec3(0.58,0.65,0.75) (edge_haze.hglsl:8) — the constant legacy
  fades to. No uniform. Path-of-least-surprise (chunk == legacy). RECOMMENDED.
- edge_fog.frag u_fogColor (uniform :26) — the actual cloud-bank tint painted past
  the boundary, driven by MC2_EDGE_FOG C++. Match this only if EDGE_HAZE_SKY visibly
  mismatches the live cloud tint at the seam (then promote to a uniform fed from the
  same C++ source as u_fogColor).
- NOT water: perimeter is hidden by the cloud bank, not water; water is below sea
  level and edge_fog explicitly skips it (edge_fog.frag:55).

## 3. DEPTH-SILHOUETTE VERDICT — color feather SUFFICIENT; depth is not the problem

- terrain_lod_chunk.frag writes NO gl_FragDepth (:339-343, :77-80). Depth = rasterized
  geometry depth (vert pre-divide bias). So terrain DOES write a hard depth edge.
- BUT the silhouette is covered by a SCREEN-SPACE post pass: edge_fog.frag/fog_oob.frag
  read depthTex and paint over both edge terrain pixels AND void using a world-space
  ray-plane boundary INDEPENDENT of terrain depth (edge_fog.frag:62-69), blended
  GL_SRC_ALPHA over the scene. The hard terrain depth edge is COVERED by cloud alpha,
  not revealed. => color-only terrain feather leaves NO depth silhouette vs sky/void.
- Where depth WOULD matter is terrain-vs-WATER (real translucent geom), but at the
  perimeter water draws over and edge_fog skips water pixels — feather there is
  cosmetic, no skirt needed.
- Existing skirts (u_skirtDepth :29) are downward walls sampling the same colormap
  (:380-386) for inter-chunk seams — they do NOT fade to sky; wrong tool for perimeter.

## 4. WHY edge_fog IS INSUFFICIENT (the gap)

edge_fog/fog_oob aren't failing — they only paint the cloud BANK as a post overlay
keyed on the boundary plane, default u_fogStart=50 WU. Inside that 50 WU the cloud
alpha ramps from 0, so near the edge the terrain is still ~fully visible with its
authored colormap running to a HARD geometric edge. Legacy edgeHazeAmount fades the
TERRAIN COLOR to sky over a wider 256->32 WU band (edge_haze.hglsl:11) BEFORE the
cloud bank starts, so the colormap step is gone. The chunk path lost that terrain-side
fade => colormap reaches the silhouette hard and the 50 WU fog ramp is too narrow/late
to hide it. Gap = chunk frag has no terrain-side haze; post fog can only overlay cloud,
it cannot fade the terrain colormap itself.

## 5. RECOMMENDATION — Option A is enough. Do NOT add skirt geometry (Option B).

Option A (color edge-distance feather in terrain_lod_chunk.frag): SUFFICIENT.
Port the legacy line — #include <include/edge_haze.hglsl> + at ~:701:
  if (u_useEdgeFeather != 0)
      lit = mix(lit, EDGE_HAZE_SKY, edgeHazeAmount(v_worldPos.xy, u_halfMap) * strength);
Gate MC2_TERRAIN_EDGE_FEATHER default 0 -> block skipped -> byte-identical.
u_halfMap + v_worldPos already present; no new C++ upload for the constant version.

Option B (skirt geometry): NOT needed. Terrain writes hard depth but the post cloud
bank already covers the silhouette in screen space, so no depth-revealed gap a skirt
would fix; a sky-faded perimeter skirt only duplicates Option A in geometry.

Width: widen terrain-side haze to MEET the cloud bank (match legacy 256->32 WU, or
align inner edge to u_fogStart so terrain-haze and cloud ramp overlap).

## Byte-identical gate plan
- Env gate MC2_TERRAIN_EDGE_FEATHER; driver uploads int u_useEdgeFeather (+ optional
  width/strength/color). Default 0. Shader wraps the mix in if (u_useEdgeFeather != 0)
  -> pixel-invariant off, same discipline as useRockSlopeBias (:474) /
  macroVariationStrength (:612).
- Verify: A/B gate-off byte-identical (golden frame); gate-on shows perimeter colormap
  fading to sky over the band, no remaining hard line.
