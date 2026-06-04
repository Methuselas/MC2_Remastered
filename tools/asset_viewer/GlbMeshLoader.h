// tools/asset_viewer/GlbMeshLoader.h
// Assimp glTF/GLB -> MeshData. No GL. Engine-faithful transform:
//   step1 glTF->Stuff: (mc2.x=-src.x, mc2.y=src.z, mc2.z=src.y)
//   step2 Stuff->GL:   (gl.x=-mc2.x, gl.y=mc2.z, gl.z=mc2.y)  + UV v->1-v.
#pragma once
#include "TglMeshLoader.h"
#include <string>
namespace GlbMeshLoader { MeshData load(const std::string& path); }
