#include "TextureExtensions.h"
#include "TextureDecoderRegistry.h"

bool IsSupportedTextureFile(const std::string& path)
{
    return textureDecoderRegistry().isSupported(path);
}
