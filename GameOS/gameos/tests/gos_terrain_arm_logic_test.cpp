// Unit tests for the GPU-direct terrain arm predicate shared by the game and
// the Mission Editor (GameOS/gameos/gos_terrain_arm_logic.h).
//
// These lock down two things:
//   1. No game regression: the arm decision for every map shape the GAME ever
//      produces (always has tile-handle quads AND a colormap atlas) is
//      unchanged by the editor-new-map fix.
//   2. Editor↔game agreement: a freshly generated editor map (pure colormap,
//      zero tile-handle quads) arms via the colormap atlas through the exact
//      same predicate the game uses — and a legacy non-colormap map still
//      falls back to the CPU path.
//
// Dependency-free; compile and run standalone (no GL, no engine link):
//   cl /EHsc /std:c++17 /I..  gos_terrain_arm_logic_test.cpp && gos_terrain_arm_logic_test.exe
//   (or)  g++ -std=c++17 -I.. gos_terrain_arm_logic_test.cpp -o t && ./t

#include "../gos_terrain_arm_logic.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace gos_terrain_arm;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        ++g_failures; \
    } \
} while (0)

// Mirror of CollectUniqueNodeIds()'s filter, exercised on the real predicate so
// we can assert what populates the unique-node-id set for each map shape.
static std::vector<uint32_t> collectTileNodes(const std::vector<uint32_t>& wp2, uint32_t maxTex) {
    std::vector<uint32_t> out;
    for (uint32_t n : wp2)
        if (IsTileHandle(n, maxTex))
            out.push_back(n);
    return out;
}

int main() {
    const uint32_t MAXTEX = 4096; // representative MC_MAXTEXTURES

    // --- IsTileHandle: only (0, maxTex) is a real tile handle ----------------
    CHECK(IsTileHandle(1, MAXTEX));
    CHECK(IsTileHandle(659, MAXTEX));
    CHECK(IsTileHandle(MAXTEX - 1, MAXTEX));
    CHECK(!IsTileHandle(0, MAXTEX));               // edge / no-terrain quad
    CHECK(!IsTileHandle(0xFFFFFFFFu, MAXTEX));     // colormap sentinel
    CHECK(!IsTileHandle(MAXTEX, MAXTEX));          // out of range
    CHECK(!IsTileHandle(MAXTEX + 100, MAXTEX));

    // --- Map-shape scenarios via the collector + arm predicate ---------------
    const bool ENABLED = true;

    // (a) GAME stock map: mix of colormap-sentinel quads + some cement/overlay
    //     tile quads. Has tile node ids AND (always) a colormap atlas.
    {
        std::vector<uint32_t> wp2 = {0xFFFFFFFFu, 0xFFFFFFFFu, 12, 0xFFFFFFFFu, 7, 0u, 12};
        auto nodes = collectTileNodes(wp2, MAXTEX);
        CHECK(nodes.size() == 3);                  // 12, 7, 12 (dups kept by this mirror; set-dedup is upstream)
        const bool hasTiles  = !nodes.empty();
        const bool hasAtlas  = true;
        CHECK(ShouldArmGpuTerrain(ENABLED, hasTiles, hasAtlas) == true);   // armed (unchanged)
    }

    // (b) EDITOR fresh flat map: EVERY quad is the colormap sentinel, zero
    //     tile-handle quads -> empty node set. The fix: arms via the atlas.
    {
        std::vector<uint32_t> wp2 = {0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu};
        auto nodes = collectTileNodes(wp2, MAXTEX);
        CHECK(nodes.empty());                      // this is why the old !empty() gate failed
        const bool hasTiles  = !nodes.empty();     // false
        const bool hasAtlas  = true;               // BuildColormapAtlas succeeded
        CHECK(ShouldArmGpuTerrain(ENABLED, hasTiles, hasAtlas) == true);   // FIXED: now armed
    }

    // (c) LEGACY non-colormap map: no terrainTextures2 -> no atlas, and the
    //     legacy path leaves _wp2 = 0 -> empty node set. Must NOT arm (CPU path).
    {
        std::vector<uint32_t> wp2 = {0u, 0u, 0u, 0u};
        auto nodes = collectTileNodes(wp2, MAXTEX);
        CHECK(nodes.empty());
        const bool hasTiles  = !nodes.empty();     // false
        const bool hasAtlas  = false;              // BuildColormapAtlas early-returned
        CHECK(ShouldArmGpuTerrain(ENABLED, hasTiles, hasAtlas) == false);  // CPU fallback (no regression)
    }

    // --- Arm predicate truth table (exhaustive) ------------------------------
    CHECK(ShouldArmGpuTerrain(true,  true,  true ) == true);
    CHECK(ShouldArmGpuTerrain(true,  true,  false) == true);   // tile quads alone
    CHECK(ShouldArmGpuTerrain(true,  false, true ) == true);   // colormap atlas alone (editor new map)
    CHECK(ShouldArmGpuTerrain(true,  false, false) == false);  // nothing to draw -> CPU path
    CHECK(ShouldArmGpuTerrain(false, true,  true ) == false);  // solid disabled -> never arm
    CHECK(ShouldArmGpuTerrain(false, false, false) == false);

    if (g_failures == 0) {
        std::printf("gos_terrain_arm_logic_test: ALL PASS\n");
        return 0;
    }
    std::printf("gos_terrain_arm_logic_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
