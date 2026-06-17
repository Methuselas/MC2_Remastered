// GameAdapters/VegetationAdapter.h
//
// Vegetation card placement adapter.
// Scans terrain on mission load, builds a GPU instance list of billboard
// grass/bush cards for green-zone tiles, and drives GosVegetation::flush()
// each frame.
//
// Caller responsibilities:
//   - Call init() once at startup.
//   - Call missionLoaded() after Terrain + MissionMap are initialised.
//   - Call flush() once per rendered frame.
//   - Call missionUnloaded() before the terrain is torn down.
//   - Call shutdown() at exit.

#pragma once

// Forward declarations only -- .cpp includes real headers.
namespace Stuff { class Matrix4D; class Vector3D; }
class Terrain;
class MissionMap;

namespace GameAdapters {
namespace Vegetation {

// One-time engine initialisation.  Must be called before any other entry.
void init();

// One-time engine shutdown.
void shutdown();

// Called once after Terrain and MissionMap are loaded for a mission.
// Scans qualifying cells, builds the instance list and uploads to GPU.
// gameMap may be nullptr if no MOVE packet is present.
void missionLoaded(Terrain* land, MissionMap* gameMap);

// Called on mission end / before terrain is torn down.
// Clears the GPU instance list.
void missionUnloaded();

// Called once per frame to submit vegetation draw commands.
//
// worldToClipGL      -- the GL-NDC world-to-clip matrix (Stuff column-major,
//                       same source as gos_SetWorldToClipGL).
// cameraPos          -- camera world position (ML/R space).
// terrainLightDir_4f -- vec4 light direction forwarded from terrain shaders.
// time               -- elapsed seconds (used for shader animation).
void flush(const Stuff::Matrix4D& worldToClipGL,
           const Stuff::Vector3D& cameraPos,
           const float*           terrainLightDir_4f,
           float                  time);

} // namespace Vegetation
} // namespace GameAdapters
