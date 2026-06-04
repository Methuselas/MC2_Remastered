#include "TextureMetadata.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

std::string FormatDimensions(const TextureMetadata& m) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d x %d", m.width, m.height);
    return buf;
}

std::string FormatFileSize(const TextureMetadata& m) {
    char buf[64];
    double b = (double)m.fileBytes;
    if (b >= 1024.0 * 1024.0) std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
    else if (b >= 1024.0)     std::snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    else                      std::snprintf(buf, sizeof(buf), "%ju B", (std::uintmax_t)m.fileBytes);
    return buf;
}

std::string FormatChannels(const TextureMetadata& m) {
    switch (m.channels) {
        case 1: return "Gray";
        case 2: return "Gray+A";
        case 3: return "RGB";
        case 4: return "RGBA";
        default: return "unknown";
    }
}

std::string FormatTextureFormat(const TextureMetadata& m) {
    return m.formatLabel.empty() ? "unknown" : m.formatLabel;
}

FitSize FitTextureDisplaySize(int texW, int texH, float availW, float availH, float zoom)
{
    FitSize r;
    if (texW <= 0 || texH <= 0) return r;                 // {0,0}: nothing to show
    float aw = availW > 1.0f ? availW : 1.0f;             // clamp non-positive avail
    float ah = availH > 1.0f ? availH : 1.0f;
    float z  = zoom   > 0.0f ? zoom   : 1.0f;             // clamp non-positive zoom
    float scale = std::min(aw / (float)texW, ah / (float)texH);   // aspect-preserving fit
    if (!(scale > 0.0f) || !std::isfinite(scale)) scale = 1.0f;   // guard NaN/inf
    r.w = (float)texW * scale * z;
    r.h = (float)texH * scale * z;
    return r;
}
