#pragma once
#include <string>
// True if `path` ends in a stage-1 supported texture extension (case-insensitive):
// .png .jpg .jpeg .bmp .tga
bool IsSupportedTextureFile(const std::string& path);
