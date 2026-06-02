// tools/asset_viewer/Ktx2Decoder.cpp
#include "Ktx2Decoder.h"
#include <GL/glew.h>
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

    // Reject KTX2 variants the viewer/loader does not support, with an accurate
    // message (otherwise these would fall through and be mislabeled "corrupt").
    const uint32_t typeSize    = fields[1];
    const uint32_t pixelDepth  = fields[4];
    const uint32_t faceCount   = fields[6];
    const uint32_t levelCount  = fields[7];
    if (faceCount != 1 || pixelDepth != 0 || typeSize != 1 || levelCount < 1) {
        r.error = "Unsupported KTX2 variant (cubemap, 3D, array, or non-byte channel type) not supported.";
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

DecodedTexture Ktx2Decoder::load(const std::string& path) const
{
    DecodedTexture d;
    Ktx2DecodedImage k = loadKtx2Image(path);
    if (!k.ok) { d.error = k.error; return d; }
    const RenderCore::KtxImage& img = k.img;

    d.width         = img.width;
    d.height        = img.height;
    d.mipCount      = img.mipCount;
    d.isCompressed  = img.isCompressed;

    GLuint tex = 0;
    while (glGetError() != GL_NO_ERROR) {}   // drain stale errors so our check only sees OUR upload
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    if (!img.isCompressed) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const GLenum internal = img.isSrgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;     // color-space per metadata
        glTexImage2D(GL_TEXTURE_2D, 0, internal, img.width, img.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE,
                     img.pixels.data() + img.mipByteOffsets[0]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);   // restore default
        d.formatLabel = img.isSrgb ? "RGBA8 (sRGB)" : "RGBA8";
        // mip-0 only — must NOT use a mipmap min-filter or the texture is mip-incomplete
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    } else {
        if (!GLEW_ARB_texture_compression_bptc) {
            glDeleteTextures(1, &tex);
            glGetError();   // consume any gen/bind error so it doesn't leak to the next load()
            d.error = "BC7 preview requires GL_ARB_texture_compression_bptc, "
                      "unavailable on this GPU/context.";
            return d;
        }
        const GLenum internal = img.isSrgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
                                           : GL_COMPRESSED_RGBA_BPTC_UNORM;
        for (int lvl = 0; lvl < img.mipCount; ++lvl) {
            const int lw = (img.width  >> lvl) ? (img.width  >> lvl) : 1;
            const int lh = (img.height >> lvl) ? (img.height >> lvl) : 1;
            const uint64_t off  = img.mipByteOffsets[lvl];
            const uint64_t next = (lvl + 1 < img.mipCount) ? img.mipByteOffsets[lvl + 1]
                                                           : (uint64_t)img.pixels.size();
            const GLsizei size  = (GLsizei)(next - off);
            glCompressedTexImage2D(GL_TEXTURE_2D, lvl, internal, lw, lh, 0,
                                   size, img.pixels.data() + off);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, img.mipCount - 1);
        // Full mip chain uploaded + MAX_LEVEL set — mip-complete, use mip-aware filter
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        img.mipCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "BC7%s, %d mip%s",
                      img.isSrgb ? " (sRGB)" : "", img.mipCount, img.mipCount > 1 ? "s" : "");
        d.formatLabel = buf;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "GL upload failed (0x%x) for %s", e, path.c_str());
        d.error = buf;
        return d;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    d.glTexture     = tex;
    d.ownsGlTexture = true;
    return d;
}
