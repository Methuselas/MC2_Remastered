// TERRAIN-NORMALS-FROM-HEIGHT-1: per-mission terrain height texture.
//
// One R32F GL texture sized realVerticesMapSide x realVerticesMapSide,
// uploaded once at mission load from MapData blocks (the gameplay-authoritative
// PostcompVertex.elevation field). Bound at sampler unit 11 by the terrain
// uniform-upload paths in gameos_graphics.cpp.
//
// Visual-only — gameplay height (Terrain::getTerrainElevation) is unchanged.
// Texture is empty/0 until uploadTerrainHeightTexture() is called; consumers
// must tolerate a 0 handle.
#ifndef GOS_TERRAIN_HEIGHT_TEX_H_
#define GOS_TERRAIN_HEIGHT_TEX_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Upload (or replace) the per-mission height texture.
//   side                : realVerticesMapSide (e.g. 100 or 120).
//   blocks              : pointer to MapData::getBlocks() — a side*side array
//                         of PostcompVertex records (32 bytes each).
//   postcompVertexStride: byte stride between records (always 32 for current
//                         MC2 build; pass as a guard against future drift).
//   elevationOffset     : byte offset of float elevation field within each
//                         record (12 in current MC2).
//   mapTopLeftX         : world-space top-left X (Terrain::mapTopLeft3d.x).
//   mapTopLeftY         : world-space top-left Y (Terrain::mapTopLeft3d.y).
//   worldUnitsPerVertex : Terrain::worldUnitsPerVertex (always 128.0 today).
//
// Safe to call repeatedly (re-uploads in place). No-op on null `blocks`.
// Logs once-per-call to stderr at trace level.
void __stdcall gos_uploadTerrainHeightTex(
    int          side,
    const void*  blocks,
    int          postcompVertexStride,
    int          elevationOffset,
    float        mapTopLeftX,
    float        mapTopLeftY,
    float        worldUnitsPerVertex);

// Drop the texture (mission unload / cleanup).
void __stdcall gos_resetTerrainHeightTex(void);

// Accessors used by gameos_graphics.cpp terrain uniform-upload paths.
// `Handle` returns 0 when no texture is uploaded.
uint32_t __stdcall gos_terrainHeightTexHandle(void);
int      __stdcall gos_terrainHeightTexSide(void);
float    __stdcall gos_terrainHeightWorldUnitsPerVertex(void);
float    __stdcall gos_terrainHeightMapTopLeftX(void);
float    __stdcall gos_terrainHeightMapTopLeftY(void);

#ifdef __cplusplus
}
#endif

#endif  // GOS_TERRAIN_HEIGHT_TEX_H_
