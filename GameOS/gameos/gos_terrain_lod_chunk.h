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

// Submit block draw commands for the current frame.
// count==0 is a strict no-op. mclib calls this via Terrain::flushDrawCommands() only.
void gos_TerrainLodChunk_SubmitDrawCommands(const TerrainDrawCommand* cmds, int count);

// Upload full heightfield to GPU SSBO at map load.
// elevations: float[mapSide*mapSide] row-major.
void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide);

// Patch a dirty block's heightfield rows after terrain edit.
// rowData: float[(quadCountY+1)*(quadCountX+1)] row-major.
void gos_TerrainLodChunk_UploadHeightPatch(
    const float* rowData,
    int originX, int originY,
    int quadCountX, int quadCountY,
    int mapSide);
