/***************************************************************
 * FILENAME: TglMeshLoader.h
 * DESCRIPTION: CPU mesh data extraction from MC2 .tgl archives.
 *   Owns FastFile registration (tgl.fst), archive enumeration,
 *   and TG_TypeMultiShape -> MeshData conversion. No GL.
 *
 * Task 1 of the asset-viewer model-preview slice.
 ***************************************************************/
#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// CPU mesh data types (no GL, no engine headers in this public interface).
// ---------------------------------------------------------------------------

struct MeshVertex {
    float px, py, pz;   // position  (Stuff -> GL: x'=-x, y'=stuffZ, z'=stuffY)
    float nx, ny, nz;   // normal    (same swap)
    float u, v;         // UV (per-corner expanded)
};

struct SubMesh {
    std::vector<MeshVertex> verts;  // non-indexed: verts.size() == idx.size()
    std::vector<uint32_t>   idx;    // sequential: 0,1,2,3,4,5,...
    std::string textureName;        // basename from GetTextureName (may be empty)
    bool isSpotlight = false;       // true when shape node id starts with "SpotLight_"
};

struct MeshData {
    std::vector<SubMesh> submeshes;
    float bmin[3] = {0.0f, 0.0f, 0.0f};  // AABB over all transformed positions
    float bmax[3] = {0.0f, 0.0f, 0.0f};
    bool  ok      = false;
    std::string error;
};

// ---------------------------------------------------------------------------
// TglMeshLoader namespace
// ---------------------------------------------------------------------------
namespace TglMeshLoader {

    // One-time FastFile setup (maxFastFiles, fastFiles, systemHeap, Environment)
    // + FastFileInit(deployDir/"tgl.fst"). Idempotent after first success.
    // Returns true if ready to use.
    bool ensureFastFile(const char* deployDir);

    // Enumerate *.tgl names (case-insensitive) from the registered FST.
    // Sorted alphabetically. Must call ensureFastFile() first.
    std::vector<std::string> listTgl();

    // Load a .tgl by name (as stored in the FST, e.g. "data/tgl/2civliving.tgl").
    // Calls TG_TypeMultiShape::LoadBinaryCopy, walks all shapes, groups triangles
    // by localTextureHandle into SubMesh entries, expands per-corner UVs.
    // Applies Stuff->GL coordinate swap: x'=-stuffX, y'=stuffZ, z'=stuffY.
    // Returns ok=false + error string on failure.
    MeshData loadMesh(const std::string& tglName);

} // namespace TglMeshLoader
