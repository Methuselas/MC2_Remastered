#include "TextureExtensions.h"
#include <algorithm>
#include <array>
#include <cctype>   // std::tolower

bool IsSupportedTextureFile(const std::string& path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    static const std::array<const char*, 5> kExts = {"png", "jpg", "jpeg", "bmp", "tga"};
    return std::any_of(kExts.begin(), kExts.end(),
                       [&](const char* e){ return ext == e; });
}
