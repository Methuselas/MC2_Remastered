#include "TextureMetadata.h"
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
