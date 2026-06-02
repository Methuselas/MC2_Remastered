// tools/asset_viewer/LegacyImageDecoder.cpp
#include "LegacyImageDecoder.h"
#include "UiEditorImageCache.h"
#include <cstdint>

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
    d.glTexture     = (uint32_t)(intptr_t)tex->textureId;
    d.width         = tex->width;
    d.height        = tex->height;
    d.mipCount      = 1;
    d.formatLabel   = "RGBA8";
    d.isCompressed  = false;
    d.ownsGlTexture = false;   // cache-owned
    return d;
}
