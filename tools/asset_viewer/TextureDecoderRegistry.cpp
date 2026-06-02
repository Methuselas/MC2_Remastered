// tools/asset_viewer/TextureDecoderRegistry.cpp
#include "TextureDecoderRegistry.h"
#include "LegacyImageDecoder.h"
#include <algorithm>
#include <cctype>

std::string TextureExtLower(const std::string& path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return ext;
}

void TextureDecoderRegistry::add(std::unique_ptr<ITextureDecoder> d)
{
    decoders_.push_back(std::move(d));
}

const ITextureDecoder* TextureDecoderRegistry::find(const std::string& path) const
{
    std::string ext = TextureExtLower(path);
    if (ext.empty()) return nullptr;
    for (const auto& d : decoders_)
        if (d->handles(ext)) return d.get();
    return nullptr;
}

DecodedTexture TextureDecoderRegistry::load(const std::string& path) const
{
    const ITextureDecoder* d = find(path);
    if (!d) { DecodedTexture r; r.error = "No decoder for this file type."; return r; }
    return d->load(path);
}

bool TextureDecoderRegistry::isSupported(const std::string& path) const
{
    return find(path) != nullptr;
}

std::vector<std::string> TextureDecoderRegistry::supportedExtensions() const
{
    // Union, in registration order. (Small N; linear is fine.)
    static const char* kProbe[] = {
        "png","jpg","jpeg","bmp","tga","ktx2","dds","basis"
    };
    std::vector<std::string> out;
    for (const char* e : kProbe) {
        std::string ext = e;
        for (const auto& d : decoders_)
            if (d->handles(ext)) { out.push_back(ext); break; }
    }
    return out;
}

// Default decoder set. Task 3 appends the Ktx2Decoder registration here.
static void buildDefaultRegistry(TextureDecoderRegistry& reg)
{
    reg.add(std::make_unique<LegacyImageDecoder>());
}

TextureDecoderRegistry& textureDecoderRegistry()
{
    static TextureDecoderRegistry reg = []{
        TextureDecoderRegistry r;
        buildDefaultRegistry(r);
        return r;
    }();
    return reg;
}
