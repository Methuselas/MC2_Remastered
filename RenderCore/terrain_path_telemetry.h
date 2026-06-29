#pragma once
// TERRAIN-PATH-TELEMETRY-1 — count which terrain-solid branch actually drew, per process.
//
// Terrain solid is path-variable (TERRAIN-SUBPASS-RECON-1): LOD-chunk (default), Indirect
// bridge, patch-stream-thin, legacy MLR. The retirement north-star is ONE terrain renderer
// (LOD-chunk); MLR + indirect are deprecated paths to be PROVEN UNUSED via runtime
// telemetry (not grep) before deletion. This is that telemetry: each branch bumps its
// counter at the point it commits a terrain draw (co-located with its markTerrainDrawn()
// call). Surfaced in the debug-state dump + MCP so "MLR used = 0 / indirect used = 0
// outside explicit diagnostic modes" becomes an observable fact.
//
// Header-only, GL-free, inline singleton (one instance across TUs). Pure counting; no
// behavior change.
#include <cstddef>

namespace RenderCore { namespace framegraph {

enum class TerrainPath : int {
    LODChunk = 0,       // default authority (MC2_TERRAIN_LOD_CHUNK on)
    IndirectBridge,     // deprecated transitional path
    PatchStreamThin,    // thin SSBO stream path
    LegacyMLR,          // deprecated legacy MLR path
    Count
};

inline unsigned long* terrainPathCounters() {
    static unsigned long c[static_cast<int>(TerrainPath::Count)] = { 0 };
    return c;
}
inline void noteTerrainPath(TerrainPath p) {
    terrainPathCounters()[static_cast<int>(p)]++;
}
inline unsigned long terrainPathCount(TerrainPath p) {
    return terrainPathCounters()[static_cast<int>(p)];
}

}} // namespace RenderCore::framegraph
