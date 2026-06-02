#pragma once
#include <string>
#include <cstdint>

struct TextureMetadata {
    int width = 0;
    int height = 0;
    int channels = 0;          // 0 if unknown
    std::uintmax_t fileBytes = 0;
    std::string formatLabel;   // "RGBA8", "BC7 (sRGB), 9 mips", ... ("" if unknown)
    int mipCount = 1;
};

std::string FormatDimensions(const TextureMetadata& m);   // "256 x 256"
std::string FormatFileSize(const TextureMetadata& m);     // "1.5 MB" / "812 KB" / "300 B"
std::string FormatChannels(const TextureMetadata& m);     // "RGBA"/"RGB"/"Gray+A"/"Gray"/"unknown"
std::string FormatTextureFormat(const TextureMetadata& m);// formatLabel or "unknown"

struct FitSize { float w = 0.0f; float h = 0.0f; };

// Largest aspect-preserving size of (texW x texH) fit into (availW x availH),
// then multiplied by zoom. Two textures with the SAME aspect ratio but different
// native resolutions yield the SAME result at the same zoom (constant on-screen
// size). Safe (finite, non-negative) for degenerate inputs (returns {0,0} if the
// texture has no area; clamps non-positive avail/zoom).
FitSize FitTextureDisplaySize(int texW, int texH, float availW, float availH, float zoom);
