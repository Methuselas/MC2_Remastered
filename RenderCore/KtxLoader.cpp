// RenderCore/KtxLoader.cpp
// Minimal KTX2 RGBA8 loader -- Phase 0 (no Basis/supercompression transcoding).
// No GL headers. No external dependencies.

#include "KtxLoader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace RenderCore {

// KTX2 file magic: \xab KTX 20\xbb \r\n \x1a\n
static constexpr uint8_t kKtx2Magic[12] = {
    0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a
};

// VK_FORMAT constants for uncompressed RGBA8
static constexpr uint32_t kVkFormatR8G8B8A8Unorm = 37;
static constexpr uint32_t kVkFormatR8G8B8A8Srgb  = 43;
// VK_FORMAT constants for stored BC7 (BPTC) blocks
static constexpr uint32_t kVkFormatBc7UnormBlock = 145;
static constexpr uint32_t kVkFormatBc7SrgbBlock  = 146;

// KTX2 header fields we need (all uint32, little-endian, file offsets 12-44).
// We do NOT use the struct approach for the full header because mixing
// uint32_t and uint64_t fields causes compiler padding before the uint64_t
// members, making sizeof() != the on-disk layout.
// Strategy: read the 9 essential uint32 fields (36 bytes), then fseek to
// file offset 80 (start of level index) to skip the rest of the header.
// KTX2 level index starts at file offset 80 by spec:
//   12 (magic) + 9*4 + 4*4 + 2*8 = 12 + 36 + 16 + 16 = 80.
struct Ktx2Fields {
    uint32_t vkFormat;
    uint32_t typeSize;
    uint32_t pixelWidth;
    uint32_t pixelHeight;
    uint32_t pixelDepth;
    uint32_t layerCount;
    uint32_t faceCount;
    uint32_t levelCount;
    uint32_t supercompressionScheme;
};
static_assert(sizeof(Ktx2Fields) == 36, "Ktx2Fields must be 36 bytes (all uint32)");

// Helper: read exactly `n` bytes; return false on short read or error.
static bool readExact(std::FILE* f, void* buf, std::size_t n)
{
    return std::fread(buf, 1, n, f) == n;
}

bool ktxLoadRgba8(const char* path, KtxImage& out)
{
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;

    // ---- 1. Magic check ----
    uint8_t magic[12];
    if (!readExact(f, magic, 12) || std::memcmp(magic, kKtx2Magic, 12) != 0) {
        std::fclose(f);
        return false;
    }

    // ---- 2. Read the 9 essential uint32 fields (36 bytes) ----
    Ktx2Fields hdr{};
    if (!readExact(f, &hdr, sizeof(hdr))) {
        std::fclose(f);
        return false;
    }

    // ---- 3. Validate ----
    const bool isRgba8 = (hdr.vkFormat == kVkFormatR8G8B8A8Unorm ||
                          hdr.vkFormat == kVkFormatR8G8B8A8Srgb);
    const bool isBc7   = (hdr.vkFormat == kVkFormatBc7UnormBlock ||
                          hdr.vkFormat == kVkFormatBc7SrgbBlock);
    if (!isRgba8 && !isBc7) {
        std::fclose(f);
        return false;
    }
    // typeSize for block-compressed formats is 1 by KTX2 spec; for RGBA8 it is
    // the size of the channel type (1 byte). Either way we require 1.
    if (hdr.typeSize != 1 ||
        hdr.pixelDepth != 0 ||
        hdr.faceCount  != 1 ||
        hdr.levelCount  < 1 ||
        hdr.supercompressionScheme != 0) {   // stored only -- no transcoding
        std::fclose(f);
        return false;
    }

    // ---- 4. Seek to level index at file offset 80 ----
    // Skips dfdByteOffset/Length, kvdByteOffset/Length (4*4=16 bytes) and
    // sgdByteOffset/Length (2*8=16 bytes). Total header = 12+36+16+16 = 80.
    if (std::fseek(f, 80, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }

    // ---- 5. Level index: read ALL levelCount entries ----
    // Each entry: byteOffset (uint64), byteLength (uint64), uncompressedByteLength (uint64)
    // Spec: level index is ordered largest-mip-first (mip 0 = entry[0]).
    struct LevelEntry {
        uint64_t byteOffset;
        uint64_t byteLength;
        uint64_t uncompressedByteLength;
    };
    std::vector<LevelEntry> entries(hdr.levelCount);
    if (!readExact(f, entries.data(),
                   sizeof(LevelEntry) * static_cast<std::size_t>(hdr.levelCount))) {
        std::fclose(f);
        return false;
    }

    // ---- 6. Validate each level's byteLength against expected RGBA8 size,
    //         and accumulate the concatenated buffer size (level 0 first). ----
    out.mipByteOffsets.clear();
    out.mipByteOffsets.resize(hdr.levelCount);
    uint64_t totalBytes = 0;
    for (uint32_t lvl = 0; lvl < hdr.levelCount; ++lvl) {
        const uint32_t lw = (hdr.pixelWidth  >> lvl) ? (hdr.pixelWidth  >> lvl) : 1u;
        const uint32_t lh = (hdr.pixelHeight >> lvl) ? (hdr.pixelHeight >> lvl) : 1u;
        uint64_t expected;
        if (isBc7) {
            // BC7: 4x4 blocks, 16 bytes/block. ceil(lw/4)*ceil(lh/4)*16.
            const uint64_t bw = (static_cast<uint64_t>(lw) + 3u) / 4u;
            const uint64_t bh = (static_cast<uint64_t>(lh) + 3u) / 4u;
            expected = bw * bh * 16u;
        } else {
            expected = static_cast<uint64_t>(lw) * static_cast<uint64_t>(lh) * 4u;
        }
        if (entries[lvl].byteLength != expected) {
            std::fclose(f);
            return false;   // pre-baked mip chain mismatch -> fail; caller falls back.
        }
        out.mipByteOffsets[lvl] = totalBytes;   // level 0 -> offset 0
        totalBytes += expected;
    }

    // ---- 7. Read each level's bytes into the concatenated buffer ----
    out.pixels.resize(static_cast<std::size_t>(totalBytes));
    for (uint32_t lvl = 0; lvl < hdr.levelCount; ++lvl) {
        if (std::fseek(f, static_cast<long>(entries[lvl].byteOffset), SEEK_SET) != 0) {
            std::fclose(f);
            return false;
        }
        const std::size_t off = static_cast<std::size_t>(out.mipByteOffsets[lvl]);
        const std::size_t len = static_cast<std::size_t>(entries[lvl].byteLength);
        if (!readExact(f, out.pixels.data() + off, len)) {
            std::fclose(f);
            return false;
        }
    }

    std::fclose(f);

    // ---- 8. Fill output ----
    out.width          = static_cast<int>(hdr.pixelWidth);
    out.height         = static_cast<int>(hdr.pixelHeight);
    out.isSrgb         = (hdr.vkFormat == kVkFormatR8G8B8A8Srgb ||
                          hdr.vkFormat == kVkFormatBc7SrgbBlock);
    out.mipCount       = static_cast<int>(hdr.levelCount);
    out.vkFormat       = hdr.vkFormat;
    out.isCompressed   = isBc7;
    out.blockSizeBytes = isBc7 ? 16u : 0u;

    return true;
}

} // namespace RenderCore
