#pragma once
#include <cstdint>

// Terrain LOD chunk render system.
// mclib/ calls these functions. GameOS/gameos/ implements them.
// mclib/ must NEVER include GL headers or call gl*() directly.

struct TerrainDrawCommand {
    int blockOriginX;      // vertex-grid X origin = blockX * 20
    int blockOriginY;      // vertex-grid Y origin = blockY * 20
    int lodStep;           // one of {1, 2, 4, 5, 10, 20}
    int quadCountsPacked;  // (quadCountX & 0xFF) | ((quadCountY & 0xFF) << 8)
};
static_assert(sizeof(TerrainDrawCommand) == 16, "TerrainDrawCommand must be 16 bytes");

constexpr uint32_t TERRAIN_HEIGHT_SSBO_BINDING = 23u;
constexpr uint32_t TERRAIN_TYPE_SSBO_BINDING   = 24u;  // Step 5b: per-vertex terrainType (concrete)

// Submit block draw commands for the current frame.
// count==0 is a strict no-op. mclib calls this via Terrain::flushDrawCommands() only.
// skirtDepths: parallel float array [count], one depth value per command.
// skirtEdgeMasks: parallel uint8 array [count] (Phase 10.2b), bit 0=N,1=S,2=W,3=E
//   — draw a skirt only on edges whose neighbour LOD differs. nullptr -> all edges.
// edgeStitch: parallel uint32 array [count] (Phase 10.4). Packs the COARSER
//   neighbour's vertex stride per edge (N=bits0-7, S=8-15, W=16-23, E=24-31; 0 =
//   no stitch). Passed to the vert as u_edgeStitch; the fine edge's intermediate
//   verts snap onto the coarse edge line -> crack-free LOD seam. nullptr -> off.
void gos_TerrainLodChunk_SubmitDrawCommands(
    const TerrainDrawCommand* cmds,
    const float*              skirtDepths,
    const unsigned char*      skirtEdgeMasks,
    const unsigned int*       edgeStitch,
    int                       count);

// Upload full heightfield to GPU SSBO at map load.
// elevations: float[mapSide*mapSide] row-major.
void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide);

// Step 5b: upload per-vertex terrainType (0..N; cement/concrete ~3) to its SSBO.
// types: float[mapSide*mapSide] row-major (parallel to the heightfield).
void gos_TerrainLodChunk_UploadTerrainTypeFull(const float* types, int mapSide);

// Patch a dirty block's heightfield rows after terrain edit.
// rowData: float[(quadCountY+1)*(quadCountX+1)] row-major.
void gos_TerrainLodChunk_UploadHeightPatch(
    const float* rowData,
    int originX, int originY,
    int quadCountX, int quadCountY,
    int mapSide);

// Allocate the height SSBO. Call once after GL context is live (from gosRenderer::init).
void gos_TerrainLodChunk_Init();

// Free the height SSBO. Call at shutdown (from gosRenderer::destroy).
void gos_TerrainLodChunk_Destroy();
