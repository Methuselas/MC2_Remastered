// RenderCore/KtxLoader.h
// Minimal KTX2 loader for RGBA8 and stored BC7 textures.
// Supports VK_FORMAT_R8G8B8A8_UNORM (37), VK_FORMAT_R8G8B8A8_SRGB (43),
// VK_FORMAT_BC7_UNORM_BLOCK (145), and VK_FORMAT_BC7_SRGB_BLOCK (146).
// Returns false for supercompressed files, missing files, or invalid headers.
//
// BC7 support (COMPRESSION-BC7-STATICPROP-1): the loader stays GL-FREE and
// simply validates + concatenates the stored block-compressed mip chain. The
// consumer (gos_static_prop_batcher BC7 array path) uploads it verbatim via
// glCompressedTexSubImage3D. Per-level byteLength validation is format-aware.
//
// Phase 1+ (Basis UASTC/ETC1S transcoding) will extend this without
// changing the call site interface.

#pragma once

#include <cstdint>
#include <vector>

namespace RenderCore {

struct KtxImage {
    // Pixel data. Holds the FULL mip chain concatenated, level 0 first.
    // For RGBA8 this is raw RGBA bytes; for BC7 this is the stored block stream.
    // Legacy callers that read only mip 0 use pixels.data() (== offset 0),
    // width/height (mip-0 dims), and isSrgb — those remain valid unchanged.
    std::vector<uint8_t> pixels;
    int   width    = 0;            // mip-0 width
    int   height   = 0;            // mip-0 height
    bool  isSrgb   = false;        // vkFormat is an sRGB format (43 or 146)
    int   mipCount = 0;            // number of mip levels present in `pixels`
    std::vector<uint64_t> mipByteOffsets; // byte offset of each level in `pixels` (level 0 == 0)

    // COMPRESSION-BC7-STATICPROP-1 format metadata.
    uint32_t vkFormat       = 0;   // raw VK_FORMAT value from the file header
    bool     isCompressed   = false; // true for BC7 (145/146); false for RGBA8 (37/43)
    uint32_t blockSizeBytes = 0;   // 16 for BC7; 0 for uncompressed RGBA8
};

// Returns true and fills `out` on success.
// Returns false (silent) if file missing, bad magic, or format unsupported.
// Despite the historical name, this also accepts stored (non-supercompressed)
// BC7 KTX2 files — inspect out.isCompressed / out.vkFormat to dispatch upload.
bool ktxLoadRgba8(const char* path, KtxImage& out);

// CPU-side BC7 (BPTC) block decode (COMPRESSION-BC7-CPUDECODE-1).
// Decodes one mip level of an already-loaded BC7 KtxImage into a freshly
// allocated, tightly-packed RGBA8 buffer (4 bytes/pixel, top row first,
// no padding). This is the missing CPU-readable source that lets the engine
// source dimensions + texels from a .ktx2 alone (no companion .tga required).
//
//   img    : a KtxImage filled by ktxLoadRgba8 with img.isCompressed == true
//            and img.vkFormat 145/146 (BC7). Non-BC7 images return false.
//   level  : mip level to decode (0 == largest). Must be < img.mipCount.
//   outRgba: resized to mipWidth*mipHeight*4 and filled on success.
//   outW/outH (optional): receive the decoded level's dimensions.
//
// Returns false (and leaves outRgba untouched) on any precondition failure.
// GL-free, allocation only via the supplied vector, no global state.
bool ktxDecodeBc7ToRgba8(const KtxImage& img, int level,
                         std::vector<uint8_t>& outRgba,
                         int* outW = nullptr, int* outH = nullptr);

} // namespace RenderCore
