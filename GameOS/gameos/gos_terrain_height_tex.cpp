// TERRAIN-NORMALS-FROM-HEIGHT-1: per-mission terrain height texture.
//
// Self-contained TU. Holds a single R32F texture + the world-space mapping
// metadata needed to convert a fragment's WorldPos.xy back into texel coords
// inside the shader. Visual-only — gameplay height stays in MapData.
//
// Trace logging is deliberately quiet (one stderr line per upload) so it can
// stay compiled-in even when the gate is OFF.

#include "gos_terrain_height_tex.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <GL/glew.h>

namespace {

GLuint g_handle              = 0;
int    g_side                = 0;
float  g_worldUnitsPerVertex = 128.0f;
float  g_mapTopLeftX         = 0.0f;
float  g_mapTopLeftY         = 0.0f;

}  // namespace

void __stdcall gos_uploadTerrainHeightTex(
    int          side,
    const void*  blocks,
    int          postcompVertexStride,
    int          elevationOffset,
    float        mapTopLeftX,
    float        mapTopLeftY,
    float        worldUnitsPerVertex)
{
    if (!blocks || side <= 0 || postcompVertexStride <= 0
        || elevationOffset < 0
        || elevationOffset + (int)sizeof(float) > postcompVertexStride) {
        fprintf(stderr, "[TERRAIN_HEIGHT_TEX] skip upload "
                "side=%d stride=%d elevOff=%d blocks=%p\n",
                side, postcompVertexStride, elevationOffset, blocks);
        return;
    }

    // Extract the elevation column into a packed float buffer.
    const size_t count = (size_t)side * (size_t)side;
    std::vector<float> elev(count);
    const uint8_t* base = static_cast<const uint8_t*>(blocks);
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(&elev[i],
                    base + i * (size_t)postcompVertexStride
                         + (size_t)elevationOffset,
                    sizeof(float));
    }

    if (g_handle == 0) {
        glGenTextures(1, &g_handle);
    }

    // Preserve current binding on unit 0 so we don't disturb the caller —
    // mclib/terrain.cpp is mid-mission-init and may have whatever was active
    // beforehand. glActiveTexture+restore costs one int.
    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    glBindTexture(GL_TEXTURE_2D, g_handle);
    // R32F single-channel float; no mipmaps (per-vertex authority value, no
    // need to filter across LODs). CLAMP_TO_EDGE so off-map fetches mirror
    // the nearest edge sample (matches gameplay clamp semantics on lookup).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F,
                 side, side, 0,
                 GL_RED, GL_FLOAT, elev.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    g_side                = side;
    g_worldUnitsPerVertex = (worldUnitsPerVertex > 0.0f) ? worldUnitsPerVertex
                                                         : 128.0f;
    g_mapTopLeftX         = mapTopLeftX;
    g_mapTopLeftY         = mapTopLeftY;

    fprintf(stderr,
        "[TERRAIN_HEIGHT_TEX] uploaded handle=%u side=%d wuPerVert=%.1f "
        "topLeft=(%.1f,%.1f) sampleBytes=%zu\n",
        (unsigned)g_handle, g_side, g_worldUnitsPerVertex,
        g_mapTopLeftX, g_mapTopLeftY, count * sizeof(float));
}

void __stdcall gos_resetTerrainHeightTex(void)
{
    if (g_handle != 0) {
        glDeleteTextures(1, &g_handle);
        g_handle = 0;
    }
    g_side                = 0;
    g_worldUnitsPerVertex = 128.0f;
    g_mapTopLeftX         = 0.0f;
    g_mapTopLeftY         = 0.0f;
}

uint32_t __stdcall gos_terrainHeightTexHandle(void)         { return (uint32_t)g_handle; }
int      __stdcall gos_terrainHeightTexSide(void)            { return g_side; }
float    __stdcall gos_terrainHeightWorldUnitsPerVertex(void){ return g_worldUnitsPerVertex; }
float    __stdcall gos_terrainHeightMapTopLeftX(void)        { return g_mapTopLeftX; }
float    __stdcall gos_terrainHeightMapTopLeftY(void)        { return g_mapTopLeftY; }
