# TERRAIN-VISUAL-HEIGHT-SAMPLE-1 — CORE chunk geometry recon

Read-only. Goal: render terrain at a 4x-finer vertex grid driven by a precomputed
VISUAL heightfield (`visual_height_4x.r32`, V=(N-1)*4+1, corner-pinned) WITHOUT
moving gameplay height. Gate default-OFF, byte-identical OFF. This is the first
producer of a visual!=gameplay divergence. The live terrain path is the
LOD-chunk renderer (NOT tessellated).

## 1. GRID GEN (gos_terrain_lod_chunk.cpp)
- Map is divided into blocks of `verticesBlockSide = 20` quads (terrain.cpp:106).
  `worldUnitsPerVertex = 128`. blocksMapSide = realVerticesMapSide/20 (terrain.cpp:584).
- Per (qcX,qcY,lodStep) a `PatchShape` VBO+IBO is built once + cached in
  `s_patchCache` (getOrBuildPatch, :217-329). VBO = `int16_t[2]{lx,ly}` LOCAL grid
  offsets (:226-231). IBO = uint16 tris, 2 CCW per cell (:233-244).
- Sample positions: `makeSamplePositions(quadCount,lodStep)` (:207) = `0,step,2step,..`
  always appending far edge `quadCount`. So a full LOD0 block = 21x21 verts (441),
  400 quads, 800 tris.
- ONE global VAO `s_patchVao` (:335,461), attribs re-pointed per draw (:976-1001).
- (row,col)->world: vert (:42-82) `mapX=clamp(blockOriginX+localOffset.x)`,
  `worldX=mapX*128-halfMap`, `worldY=halfMap-mapY*128`. Map-stable index.

## 2. HEIGHT SOURCE
- SSBO binding 23 `TerrainHeightBuf heights[]` (h.h:16, vert:25). Allocated in
  Init (:346), filled by `UploadHeightFull` (:1054) glBufferData of
  `float[mapSide*mapSide]` row-major, GL_DYNAMIC_DRAW.
- Fill source = terrain.cpp:811 `elev[i]=blks[i].elevation` (PostcompVertex), i.e.
  ORIGINAL coarse resolution `realVerticesMapSide^2`.
- Vert reads `heights[mapX+mapY*mapSide]` (:45) — coarse-indexed, NO interpolation
  of height beyond the LOD-stitch mix (:73). terrainType SSBO @24 parallel (:30).
- For 4x: a coarse SSBO + denser VBO gains NOTHING (verts land on integer mapX/mapY
  and re-read the SAME coarse cells). 4x REQUIRES a 4x-resolution height buffer
  (V*V floats) AND finer vertices that sample it at fine index. The existing grid
  cannot "sample a finer height tex" because the vert addresses height by integer
  coarse (mapX,mapY); there is no sub-cell fractional sample today.

## 3. THE 4x INJECTION — concrete options
Live path is regular VAO, NOT tessellated. Two ways:
(a) SUBDIVIDE GRID 4x (16x verts): change `makeSamplePositions`/patch build so a
    block emits 80x80 quads at LOD0 (was 20x20). localOffset becomes fine units
    (0..80). Need a NEW 4x height SSBO (V*V floats) at a new binding, and the vert
    indexes `heightsFine[fineX+fineY*V]` where fineX=blockOriginX*4+localOffset.x.
    int16 localOffset still fits (max ~80). uint16 IBO still fits (81x81=6561<65535).
    VBO/IBO regen automatic (cache keyed on qc,lodStep). Buffers touched: NEW
    s_visualHeightSsbo (binding 26+), patch VBO/IBO sizes grow 16x.
(b) TESSELLATION: add a TCS/TES to the chunk program, displace by sampling the 4x
    SSBO. Avoids CPU vert blowup but is a NEW pipeline (the chunk path has none),
    and tess factors must align to LOD bands + neighbor — high risk. NOT recommended
    for v1.

## 4. LOD BANDS
- 6 levels, `LOD_STEPS` strides {1,2,4,5,10,20}, distance thresholds
  (terrain.cpp:197-212). chooseLodLevel by squared dist (:236, :1817/1859).
  lodStep = vertex stride baked into the patch (every step-th coarse vertex).
- 4x displacement must apply ONLY to fine bands (near). Far chunks (lodStep>=4
  already drop coarse verts) must keep coarse height or they re-introduce cracks +
  cost. Practical: gate 4x to lodLevel 0 (maybe 1). A 4x-fine near chunk meets a
  coarse neighbor exactly as today — via edge-stitch (sec 5), which must still
  evaluate on the COARSE grid line.

## 5. STITCH + SKIRT
- Stitch (vert:47-75): a fine edge vertex whose neighbor is coarser snaps onto the
  coarse segment via `mix(sampleH(c0),sampleH(c1),t)` using `u_edgeStitch` packed
  per-edge coarse stride (h.h:25, cpp:967). Corners at offset 0 never move.
- Skirt (cpp:265-322): 4-edge vertical strips, `isSkirtFlag` pulls h down by
  u_skirtDepth (vert:78). Per-edge mask draws only differing edges (:1004-1012).
- If we 4x-subdivide the WHOLE chunk incl. edges: stitch math (operates in coarse
  `along%Sc` units) breaks — `along` is now in fine units, Sc in coarse — and skirt
  edge vertex lists change. Both would need fine-aware rework. This is the risk
  surface the kill-move (sec 6) avoids.

## 6. CORNER-PINNED KILL-MOVE (recommended)
Subdivide each chunk's INTERIOR only; keep the chunk's 4 corners AND all 4 edge
lines on the original coarse grid (sampled from coarse height @23). Only true
INTERIOR fine verts (not on any block edge) read the 4x visual height @new binding.
- Edges stay coarse -> stitch (which only touches edge verts, vert:52 `localOffset.y==0`
  etc.) is UNCHANGED. Skirts attach to coarse edges -> UNCHANGED. LOD seam UNCHANGED.
- Minimal change = (1) new patch builder variant that adds interior fine verts
  between the coarse 20x20 lines (only in LOD0 blocks); (2) tag interior verts so
  vert samples visual SSBO for them, coarse SSBO for edge verts; (3) one new SSBO
  + bind. No change to terrain.cpp LOD/stitch/skirt arrays. Feasible + lowest-risk.
  Interior-only fine count per LOD0 block ~= 19*19 added interior cols/rows -> still
  16x-ish verts but bounded to near chunks.

## 7. BAKE FEED
- Bake = tools/terrain_beautify/visual_heightfield.py: `upsample_corner_pinned`
  (:48), writes `<mission>.beauty/visual_height_<F>x.r32` float32 `<f4` row-major
  V*V (:199), V=(side-1)*factor+1, + report json (:216). Corner-pinned (bilinear,
  corners exact). Sidecar precedent = mission_sidecar.py baseHash gate.
- NO engine-side loader exists yet for visual_height_4x.r32 (grep: only python +
  editor BeautySidecarPreview which handles height_delta, not visual_height_4x).
  NEW work: a loader (mirror sidecar baseHash check) reads the .r32 at map load,
  calls a new `gos_TerrainLodChunk_UploadVisualHeightFull(float*, V)` -> new SSBO.
  Vert samples it at fine index for interior verts.

## 8. GATE + PERF
- Gate name suggestion `MC2_TERRAIN_VISUAL_HEIGHT` default-OFF. Byte-identical OFF
  mechanism = same pattern as MC2_TERRAIN_SLOPE_BIAS etc (cpp:849): when unset,
  do NOT upload visual SSBO / use coarse patch builder -> identical VBO/IBO/height
  -> identical pixels. (Confirmed pattern: gate read in SubmitDrawCommands, default 0.)
- PERF: full 4x = 16x verts + 16x index data per near block. LOD0 block 441->6561
  verts, 800->12800 tris. VRAM: patch cache VBO/IBO 16x (still small, int16/uint16).
  New visual SSBO = V*V*4 bytes = 16x the height SSBO (e.g. 481^2*4 ~= 925KB for a
  120-side map). Vertex-shader bound; only near (LOD0) chunks pay it.
  Interior-only kill-move ~ same vert count but avoids all edge/stitch/skirt rework.

## RECOMMENDED STAGED BUILD
1. Engine loader: read .beauty/visual_height_4x.r32 (baseHash gate) -> new SSBO
   binding 26, `UploadVisualHeightFull`. No render change yet (upload + log only).
2. Corner-pinned INTERIOR subdivision patch builder (LOD0 only), gated
   MC2_TERRAIN_VISUAL_HEIGHT, default-OFF byte-identical. Interior verts sample @26,
   edges/corners sample @23. Stitch/skirt/LOD untouched.
3. Verify byte-identical OFF (smoke + golden), then ON visual A/B on a baked mission.
4. (Later) extend to LOD1, or migrate to tess if vert cost bites.

DO NOT: feed visual height into gameplay (getTerrainElevation stays @ MapData);
touch stitch/skirt in the kill-move; subdivide edges; subdivide far LODs.
