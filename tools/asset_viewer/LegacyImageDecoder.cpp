// tools/asset_viewer/LegacyImageDecoder.cpp
#include "LegacyImageDecoder.h"
#include "UiEditorImageCache.h"
#include <cstdint>
#include <vector>

std::vector<std::string> LegacyImageDecoder::extensions() const
{
    return {"png","jpg","jpeg","bmp","tga"};
}

bool LegacyImageDecoder::handles(const std::string& extLower) const
{
    return extLower == "png" || extLower == "jpg" || extLower == "jpeg"
        || extLower == "bmp" || extLower == "tga";
}

DecodedTexture LegacyImageDecoder::load(const std::string& path) const
{
    DecodedTexture d;
    const UiEditorImageTexture* tex = UiEditorImageCache_Get(path.c_str());
    if (!tex || !tex->loaded) {
        d.error = (tex && tex->unavailable)
            ? "Image format not supported or file unreadable."
            : "Failed to load image (not found or decode error).";
        return d;
    }
    d.glTexture = static_cast<uint32_t>(static_cast<uintptr_t>(tex->textureId));  // ImU64 -> GLuint (GL ids fit in 32 bits)
    d.width         = tex->width;
    d.height        = tex->height;
    d.mipCount      = 1;
    d.formatLabel   = "RGBA8";
    d.isCompressed  = false;
    d.ownsGlTexture = false;   // cache-owned
    return d;
}
