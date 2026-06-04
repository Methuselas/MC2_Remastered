#pragma once
#include "MaterialTextureLoader.h"
#include <string>

class MaterialPreviewPBR;

// Renders four slot rows (BaseColor / Normal / ORM / Emissive). Each row shows
// the assigned filename + a "Browse..." button that opens the Win32 picker
// (same path FileBrowser uses), loads via MaterialTextureLoader (slot-aware),
// and pushes the GL texture into the preview. Also renders light + camera controls.
class MaterialSlots {
public:
    void draw(MaterialPreviewPBR& preview);

    // Parse a .fit Material{} block and load its slots into `preview`. Each slot
    // path is resolved by trying, in order: <fitDir>/<rel>, <cwd>/<rel>, <rel>.
    // Returns the number of slots successfully loaded (0 if not found / none loaded).
    int loadFit(const std::string& fitPath, MaterialPreviewPBR& preview);

private:
    void slotRow(const char* label, MaterialSlotKind kind, MaterialPreviewPBR& preview);
    std::string paths_[4];     // indexed by (int)MaterialSlotKind
    std::string errors_[4];
};
