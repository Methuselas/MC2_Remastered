#pragma once
#include <string>
// True if `path`'s extension is handled by some registered texture decoder
// (see TextureDecoderRegistry). Case-insensitive.
bool IsSupportedTextureFile(const std::string& path);
