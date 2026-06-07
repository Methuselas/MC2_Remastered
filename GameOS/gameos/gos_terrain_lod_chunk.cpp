#include "gos_terrain_lod_chunk.h"
#include "utils/gl_utils.h"
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Static SSBO state — all GL objects live here, never in mclib/.
// ---------------------------------------------------------------------------

static GLuint s_heightSsbo = 0;   // GL handle; 0 = not yet allocated
static int    s_ssboMapSide = 0;  // mapSide stored at last UploadHeightFull

// ---------------------------------------------------------------------------
// Init / Destroy — called from gosRenderer::init / gosRenderer::destroy.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_Init()
{
    if (s_heightSsbo != 0)
        return; // idempotent

    glGenBuffers(1, &s_heightSsbo);
    if (s_heightSsbo == 0)
    {
        fprintf(stderr, "[TerrainLodChunk] glGenBuffers failed for height SSBO\n");
        fflush(stderr);
    }
}

void gos_TerrainLodChunk_Destroy()
{
    if (s_heightSsbo != 0)
    {
        glDeleteBuffers(1, &s_heightSsbo);
        s_heightSsbo  = 0;
        s_ssboMapSide = 0;
    }
}

// ---------------------------------------------------------------------------
// Submit draw commands — stub until the indirect draw shader is wired up.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_SubmitDrawCommands(const TerrainDrawCommand* /*cmds*/, int /*count*/)
{
    // Phase 4 will consume cmds[] to build an indirect draw buffer.
}

// ---------------------------------------------------------------------------
// Full heightfield upload — called once at map load.
// elevations: float[mapSide*mapSide] row-major.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide)
{
    if (s_heightSsbo == 0)
    {
        fprintf(stderr, "[TerrainLodChunk] UploadHeightFull called before Init\n");
        fflush(stderr);
        return;
    }
    if (!elevations || mapSide <= 0)
        return;

    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_heightSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, elevations, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, s_heightSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    s_ssboMapSide = mapSide;

#ifdef _DEBUG
    // First-frame readback verify: confirm that the GPU round-trips the first
    // float correctly. glGetBufferSubData is available on all desktop GL >=3.1.
    float firstSample = 0.0f;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_heightSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(float), &firstSample);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (firstSample != elevations[0])
    {
        fprintf(stderr,
            "[TerrainLodChunk] readback mismatch: wrote %.6f, got %.6f\n",
            elevations[0], firstSample);
        fflush(stderr);
    }
#endif
}

// ---------------------------------------------------------------------------
// Dirty-patch upload — called after setVertexHeight() modifies a block.
// rowData: compact float[(quadCountY+1)*(quadCountX+1)] row-major.
// The full SSBO is row-major with stride mapSide, so this MUST be row-by-row.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightPatch(
    const float* rowData,
    int originX, int originY,
    int quadCountX, int quadCountY,
    int mapSide)
{
    if (s_heightSsbo == 0 || !rowData || mapSide <= 0)
        return;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_heightSsbo);

    for (int row = 0; row <= quadCountY; ++row)
    {
        int        dstIdx    = (originY + row) * mapSide + originX;
        GLintptr   dstOffset = (GLintptr)dstIdx * sizeof(float);
        GLsizeiptr bytes     = (GLsizeiptr)(quadCountX + 1) * sizeof(float);
        const float* src     = rowData + row * (quadCountX + 1);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, dstOffset, bytes, src);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}
