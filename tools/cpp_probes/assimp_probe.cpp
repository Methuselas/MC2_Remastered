// assimp_probe.cpp — ASSIMP-IMPORTER-PHASE-0
//
// OFFLINE host tool. Links ONLY assimp::assimp (no mclib, no gameos, no
// renderer). Imports a model via Assimp and emits a geometry-summary JSON
// matching the manifest `geometry` section that tools/asset_cook/validate_asset_manifest.py
// accepts.
//
// Usage:
//   assimp_probe <model-path> [--out <summary.json>]
//
// Output JSON keys (all under a single top-level object):
//   meshCount, vertexCount, indexCount, materialSlotCount : ints
//   hasNormals, hasTangents                               : bools
//   bounds { min:[x,y,z], max:[x,y,z], radius }
//
// bounds.radius is the HALF-DIAGONAL of the axis-aligned bounding box
// (0.5 * length(max - min)), i.e. the radius of the sphere centered at the
// AABB center that circumscribes the box.

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <cstdio>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const char* modelPath = nullptr;
    std::string outPath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--out") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[assimp_probe] ERROR: --out requires a path argument\n");
                return 2;
            }
            outPath = argv[++i];
        } else if (!modelPath) {
            modelPath = argv[i];
        } else {
            std::fprintf(stderr, "[assimp_probe] ERROR: unexpected extra argument '%s'\n", argv[i]);
            return 2;
        }
    }

    if (!modelPath) {
        std::fprintf(stderr, "Usage: assimp_probe <model-path> [--out <summary.json>]\n");
        return 2;
    }

    // Flags match the engine importer (mclib/mech3d.cpp); NOT aiProcess_CalcTangentSpace,
    // so hasTangents reflects the source.
    const unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_ValidateDataStructure |
        aiProcess_SortByPType;

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(modelPath, flags);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        std::fprintf(stderr, "[assimp_probe] ERROR: failed to import '%s': %s\n",
                     modelPath, importer.GetErrorString());
        return 1;
    }

    unsigned int meshCount = scene->mNumMeshes;
    unsigned int materialSlotCount = scene->mNumMaterials;
    unsigned long long vertexCount = 0;
    unsigned long long indexCount = 0;

    bool hasNormals = (meshCount > 0);
    bool hasTangents = (meshCount > 0);

    float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool sawVertex = false;

    for (unsigned int m = 0; m < meshCount; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        vertexCount += mesh->mNumVertices;

        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            indexCount += mesh->mFaces[f].mNumIndices;
        }

        if (!mesh->mNormals)  hasNormals = false;
        if (!mesh->mTangents) hasTangents = false;

        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            const aiVector3D& p = mesh->mVertices[v];
            const float pos[3] = { p.x, p.y, p.z };
            for (int a = 0; a < 3; ++a) {
                if (pos[a] < mn[a]) mn[a] = pos[a];
                if (pos[a] > mx[a]) mx[a] = pos[a];
            }
            sawVertex = true;
        }
    }

    if (!sawVertex) {
        for (int a = 0; a < 3; ++a) { mn[a] = 0.0f; mx[a] = 0.0f; }
    }

    // radius = half-diagonal of the AABB.
    double dx = (double)mx[0] - (double)mn[0];
    double dy = (double)mx[1] - (double)mn[1];
    double dz = (double)mx[2] - (double)mn[2];
    double radius = 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz);

    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "{\n"
        "  \"meshCount\": %u,\n"
        "  \"vertexCount\": %llu,\n"
        "  \"indexCount\": %llu,\n"
        "  \"materialSlotCount\": %u,\n"
        "  \"hasNormals\": %s,\n"
        "  \"hasTangents\": %s,\n"
        "  \"bounds\": {\n"
        "    \"min\": [%.6g, %.6g, %.6g],\n"
        "    \"max\": [%.6g, %.6g, %.6g],\n"
        "    \"radius\": %.6g\n"
        "  }\n"
        "}\n",
        meshCount,
        vertexCount,
        indexCount,
        materialSlotCount,
        hasNormals ? "true" : "false",
        hasTangents ? "true" : "false",
        (double)mn[0], (double)mn[1], (double)mn[2],
        (double)mx[0], (double)mx[1], (double)mx[2],
        radius);

    std::fputs(buf, stdout);

    if (!outPath.empty()) {
        FILE* f = std::fopen(outPath.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "[assimp_probe] ERROR: cannot open --out file '%s'\n", outPath.c_str());
            return 1;
        }
        std::fputs(buf, f);
        std::fclose(f);
    }

    return 0;
}
