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
    // RGBA8 pixel data. Holds the FULL mip chain concatenated, level 0 first.
    // Legacy callers that read only mip 0 use pixels.data() (== offset 0),
    // width/height (mip-0 dims), and isSrgb — those remain valid unchanged.
    std::vector<uint8_t> pixels;
    int   width    = 0;            // mip-0 width
    int   height   = 0;            // mip-0 height
    bool  isSrgb   = false;        // vkFormat == VK_FORMAT_R8G8B8A8_SRGB (43)
    int   mipCount = 0;            // number of mip levels present in `pixels`
    std::vector<uint64_t> mipByteOffsets; // byte offset of each level in `pixels` (level 0 == 0)
};

// Returns true and fills `out` on success.
// Returns false (silent) if file missing, bad magic, or format unsupported.
bool ktxLoadRgba8(const char* path, KtxImage& out);

} // namespace RenderCore
