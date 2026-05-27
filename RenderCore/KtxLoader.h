// RenderCore/KtxLoader.h
// Minimal KTX2 loader for RGBA8 textures (Phase 0 -- no Basis transcoding).
// Supports VK_FORMAT_R8G8B8A8_UNORM (37) and VK_FORMAT_R8G8B8A8_SRGB (43).
// Returns false for compressed formats, missing files, or invalid headers.
//
// Phase 1+ (Basis UASTC/ETC1S transcoding) will extend this without
// changing the call site interface.

#pragma once

#include <cstdint>
#include <vector>

namespace RenderCore {

struct KtxImage {
    std::vector<uint8_t> pixels;   // RGBA8, mip 0 only (level 0)
    int   width    = 0;
    int   height   = 0;
    bool  isSrgb   = false;        // vkFormat == VK_FORMAT_R8G8B8A8_SRGB (43)
};

// Returns true and fills `out` on success.
// Returns false (silent) if file missing, bad magic, or format unsupported.
bool ktxLoadRgba8(const char* path, KtxImage& out);

} // namespace RenderCore
