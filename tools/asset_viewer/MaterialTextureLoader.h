// tools/asset_viewer/MaterialTextureLoader.h
#pragma once
#include <cstdint>
#include <string>

enum class MaterialSlotKind { BaseColor, Normal, Orm, Emissive };

// Decodes an image file (PNG/JPG/BMP/TGA via stb_image) and uploads it to a GL
// texture with a SLOT-AWARE internalformat:
//   BaseColor/Emissive -> GL_SRGB8_ALPHA8 (GL linearizes on sample)
//   Normal/Orm         -> GL_RGBA8        (linear)
// Returns the GL texture id, or 0 on failure (errorOut set). The caller owns
// the texture and must glDeleteTextures it.
uint32_t MaterialTextureLoader_Load(const std::string& path,
                                    MaterialSlotKind kind,
                                    std::string* errorOut);
