#pragma once
#include <cstdint>
#include <vector>

// EditorNavLayer — terrain-cell passability mask built directly from land,
// with no dependency on GameMap or legacy MOVE data. Used as the overlay
// draw source when the map is too large for MOVE_buildData.
//
// Flag bits are defined in mclib/terrain.h (EditorNavFlags) so that
// mclib/quad.cpp can read gEditorNavFlags without including editor/ headers.

struct EditorNavLayer {
    int cellSide = 0;
    std::vector<uint8_t> flags;

    // Classify every terrain cell from land elevation/water. Populates
    // gEditorNavFlags / gEditorNavCellSide for the quad.cpp draw path.
    bool BuildFromTerrain();

    // Clear the mask and zero the mclib globals.
    void Clear();

    uint8_t GetCellFlags(int row, int col) const;

    static EditorNavLayer& Get();
};
