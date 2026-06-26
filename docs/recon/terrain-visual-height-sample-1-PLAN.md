# TERRAIN-VISUAL-HEIGHT-SAMPLE-1 — synthesis + staged build plan

Geometry displacement: render the live terrain at a 4x-finer VISUAL grid driven by a
precomputed heightfield, gameplay height untouched. "THE big one." Gate default-OFF.
Sub-recons: `-core.md` (geometry), `-shader.md` (vert/normals/shadow/depth), `-desync.md`
(consumers). This doc = the synthesis + plan.

## The shape of it
- **Live terrain = `terrain_lod_chunk`** (regular CPU VAO, NOT tessellated). Vertex Z from
  **SSBO binding 23** (`heights[mapX+mapY*mapSide]`, integer coarse index — no sub-cell
  sample today). This is the ONLY renderer that moves geometry.
- A separate stack (`gos_terrain` tess + TERRAIN-RESAMPLE-1 R32F tex@11) resamples height for
  **normals only** — never geometry. Don't conflate.
- The bake EXISTS: `tools/terrain_beautify/visual_heightfield.py` →
  `<mission>.beauty/visual_height_4x.r32` (float32, V*V, V=(side-1)*4+1, corner-pinned). **No
  engine loader yet.**

## The kill-move: corner-pinned interior subdivision (LOW RISK)
Subdivide only each near-LOD chunk's **interior**; keep the 4 corners + all 4 edge LINES on
the coarse grid sampling SSBO@23. Interior verts sample the visual 4x field (new SSBO).
- Stitch only touches EDGE verts → **UNCHANGED**. Skirts attach coarse edges → **UNCHANGED**.
  LOD seam → **UNCHANGED**. **No `terrain.cpp` LOD/stitch/skirt rework.**
- Divergence `A_int` is ZERO at corners, bounded by the bake's interior amplitude → bounds
  every desync below.
- LOD0 (near band) only for v1. Far LODs stay coarse.

## What MUST be done in the slice
1. **Geometry**: corner-pinned interior subdivision patch builder (LOD0), new visual height
   SSBO (binding 26), baseHash-gated loader for `visual_height_4x.r32`.
2. **Normals in lockstep**: feed the SAME 4x bake to the normal tex (`resample_bilinear`
   already makes a finer grid) — else lighting reads the old slope and mismatches the new shape.
3. **Shadow caster in lockstep**: the terrain shadow caster is a SEPARATE coarse CPU path
   (`MapData::renderStaticTerrainShadowFullMap` mapdata.cpp:1294, from `blocks[].elevation`).
   If geometry displaces but the caster stays coarse, the cast shadow shape ≠ visible shape.
   Feed 4x height into the shadow emit (watch the 16-bit / 59994 vertex-chunk cap → ~16x quads
   = more flushes). For small `A_int` this is a bounded mismatch; do it in lockstep to be safe.
4. **Decals**: `decalElevation` seam is already plumbed to `sampleVisualHeight` but that fn
   still returns gameplay Z. **Wire `sampleVisualHeight` to the 4x field + enable the decal
   gate in lockstep**, or decals z-fight/float `A_int` on every displaced cell. (Cheap.)

## Gate
`MC2_TERRAIN_VISUAL_HEIGHT` default-OFF. Vert: `uniform float u_visualDisplace` (default 0);
`==0` → exact original `h` path = byte-identical. ONE gate must zero geometry AND normal-4x
AND shadow displacement together — a partial gate = self-desync.

## Defer (bounded by corner-pin)
- **Object/unit grounding** visual-foot-snap (units hover/sink ≤ `A_int`, zero at corners):
  optional later slice — `sampleVisualHeight` for RENDER-transform Z only, gameplay Z for all
  else; shadow-caster object grounding folds in.
- **Picking** visual-height raycast variant (cursor offset ≤ `A_int` on peaks).
- **Far-LOD** displacement; eventual tessellation migration.

## Do NOT
- Feed visual height into `getTerrainElevation` (gameplay stays MapData-authoritative).
- "Fix water to follow terrain Z" — REINTRODUCES the zoom z-fight Fix B killed. Water is a flat
  plane, insulated; leave it.
- Touch stitch/skirt/far-LOD in the kill-move.

## Staged build
1. baseHash-gated loader → upload `visual_height_4x.r32` to new SSBO (binding 26) + 4x normal
   tex; log only, no geometry change. Verify load.
2. Corner-pinned interior subdivision patch builder, LOD0, gated default-OFF. Verify OFF
   byte-identical (smoke + golden), then ON A/B (graphical).
3. Shadow caster 4x lockstep + normals 4x lockstep (under same gate).
4. Decal feed: wire `sampleVisualHeight` → 4x field + decal gate lockstep.
5. (Defer) grounding visual-foot-snap, picking, far-LOD, tess.

## Risk
HIGH-but-bounded. The corner-pin contains divergence and avoids LOD/stitch/skirt rework
(the scariest part). Main remaining risk: shadow-caster lockstep (separate path, vertex cap)
and graphical verification at the LOD0/coarse interior seam. Dedicated session, graphical
verify, gate default-OFF throughout.
