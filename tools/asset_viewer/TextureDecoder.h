// tools/asset_viewer/TextureDecoder.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Result of decoding+uploading one texture file. glTexture==0 means failure
// (error holds a friendly message). ownsGlTexture decides who frees it:
//   true  -> the consumer (TexturePreview2D) must glDeleteTextures it.
//   false -> the GL texture is owned elsewhere (e.g. UiEditorImageCache); do NOT delete.
struct DecodedTexture {
    uint32_t    glTexture     = 0;
    int         width         = 0;
    int         height        = 0;
    int         mipCount      = 1;
    std::string formatLabel;            // "RGBA8", "RGBA8 (sRGB)", "BC7 (sRGB), 9 mips"
    bool        isCompressed  = false;
    bool        ownsGlTexture = false;
    std::string error;                  // empty on success
};

struct ITextureDecoder {
    virtual ~ITextureDecoder() = default;
    // extLower is the lowercased extension WITHOUT the dot, e.g. "ktx2".
    virtual bool          handles(const std::string& extLower) const = 0;
    virtual DecodedTexture load(const std::string& path) const = 0;
    // All lowercased extensions (without dot) this decoder handles.
    virtual std::vector<std::string> extensions() const = 0;
};

// Lowercased extension without the dot ("a/b.KTX2" -> "ktx2"; "noext" -> "").
std::string TextureExtLower(const std::string& path);
