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
    if (hdr.vkFormat != kVkFormatR8G8B8A8Unorm &&
        hdr.vkFormat != kVkFormatR8G8B8A8Srgb) {
        std::fclose(f);
        return false;
    }
    if (hdr.typeSize != 1 ||
        hdr.pixelDepth != 0 ||
        hdr.faceCount  != 1 ||
        hdr.levelCount  < 1 ||
        hdr.supercompressionScheme != 0) {
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

    // ---- 5. Level index: read entry[0] (mip 0, largest level) ----
    // Each entry: byteOffset (uint64), byteLength (uint64), uncompressedByteLength (uint64)
    // Spec: level index is ordered largest-mip-first (mip 0 = entry[0]).
    struct LevelEntry {
        uint64_t byteOffset;
        uint64_t byteLength;
        uint64_t uncompressedByteLength;
    };
    LevelEntry entry0{};
    if (!readExact(f, &entry0, sizeof(entry0))) {
        std::fclose(f);
        return false;
    }

    // ---- 6. Seek and read pixel data ----
    if (std::fseek(f, static_cast<long>(entry0.byteOffset), SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }

    // ---- 7. Validate byte length matches RGBA8 expectation ----
    const uint64_t expectedBytes =
        static_cast<uint64_t>(hdr.pixelWidth) *
        static_cast<uint64_t>(hdr.pixelHeight) * 4u;
    if (entry0.byteLength != expectedBytes) {
        std::fclose(f);
        return false;
    }

    out.pixels.resize(static_cast<std::size_t>(expectedBytes));
    if (!readExact(f, out.pixels.data(), static_cast<std::size_t>(expectedBytes))) {
        std::fclose(f);
        return false;
    }

    std::fclose(f);

    // ---- 8. Fill output ----
    out.width  = static_cast<int>(hdr.pixelWidth);
    out.height = static_cast<int>(hdr.pixelHeight);
    out.isSrgb = (hdr.vkFormat == kVkFormatR8G8B8A8Srgb);

    return true;
}

} // namespace RenderCore
