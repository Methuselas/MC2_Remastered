// assimp_importer.cpp — Track D Assimp-backed FBX/GLB mech importer.
//
// Stub file. ImportGeometryFromFile() body lands in commit 4 after the
// narrow construction API on TG_TypeShape and BuildTextureList helper.
//
// File compiles to nothing when ENABLE_ASSIMP_IMPORTER is OFF, so adding it
// unconditionally to mclib/CMakeLists.txt SOURCES is safe.
#include "assimp_importer.h"

#ifdef ENABLE_ASSIMP_IMPORTER

// Stub. Implementation in subsequent commits.
long ImportGeometryFromFile(const char* /*path*/, TG_TypeMultiShape* /*out*/)
{
    return -1;
}

#endif // ENABLE_ASSIMP_IMPORTER
