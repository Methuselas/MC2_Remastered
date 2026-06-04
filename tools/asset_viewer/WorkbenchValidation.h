#pragma once
#include "TglMeshLoader.h"
#include "OverrideManifest.h"
#include "WorkbenchWarning.h"
#include <string>
#include <vector>
struct SemanticInputs {
    const MeshData* overrideMesh=nullptr; const MeshData* stockMesh=nullptr;
    float maxFootprintRatio=1.0f, pivotOffsetXZ=0.0f, pivotOffsetY=0.0f;
    std::vector<std::string> missingTextures; bool hasImpostorLod=false;
};
std::vector<Warning> ValidateSemantics(const WorkbenchOverride& rec, const SemanticInputs& in);
