// tools/asset_viewer/Ktx2Decoder.cpp
#include "Ktx2Decoder.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

static const uint8_t kKtx2Magic[12] = {
    0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a
};

Ktx2DecodedImage loadKtx2Image(const std::string& path)
{
    Ktx2DecodedImage r;

    // Peek header ourselves to classify failures (ktxLoadRgba8 returns a bare bool).
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { r.error = "KTX2 file not found: " + path; return r; }
    uint8_t magic[12];
    uint32_t fields[9];   // vkFormat, typeSize, w, h, depth, layers, faces, levels, super
    bool readOk = std::fread(magic, 1, 12, f) == 12
               && std::fread(fields, 4, 9, f) == 9;
    std::fclose(f);
    if (!readOk || std::memcmp(magic, kKtx2Magic, 12) != 0) {
        r.error = "Not a KTX2 file: " + path;
        return r;
    }
    const uint32_t vkFormat = fields[0];
    const uint32_t superScheme = fields[8];
    if (superScheme != 0) {
        r.error = "Supercompressed KTX2 (Basis) not supported yet.";
        return r;
    }
    const bool known = (vkFormat == 37 || vkFormat == 43 || vkFormat == 145 || vkFormat == 146);
    if (!known) {
        r.error = "Unsupported KTX2 format (vkFormat=" + std::to_string(vkFormat)
                + "). Viewer supports RGBA8 and BC7.";
        return r;
    }

    // Real load via the engine parser (also handles BC7 despite the name).
    if (!RenderCore::ktxLoadRgba8(path.c_str(), r.img)) {
        r.error = "Corrupt KTX2: mip level size/offset out of range.";
        return r;
    }
    r.ok = true;
    return r;
}

bool Ktx2Decoder::handles(const std::string& extLower) const
{
    return extLower == "ktx2";
}

std::vector<std::string> Ktx2Decoder::extensions() const
{
    return {"ktx2"};
}

// GL upload implemented in Task 3. This stub keeps the file compiling for the
// parse smoke; it is replaced in Task 3.
DecodedTexture Ktx2Decoder::load(const std::string& path) const
{
    DecodedTexture d;
    d.error = "KTX2 GL upload not yet implemented.";
    (void)path;
    return d;
}
