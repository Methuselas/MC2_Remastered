// MECH-IMPORT-HARNESS-1A — game-free Assimp inspector/validator.
//
// Runs the SOURCE-DATA half of the mech importer pipeline from the CLI without
// launching MC2: load a GLB/FBX via Assimp and report / validate skeleton,
// weights, bind matrices, and animation clips. Catches the no-skeleton /
// bad-weights / non-invertible-bind / NaN / missing-bone class of bugs in
// seconds, before any engine build.
//
//   mech_import_harness inspect  <model.glb>
//   mech_import_harness validate <model.glb>   (exit nonzero on any failure)
//
// 1A scope: Assimp source data only. NO MC2 coordinate conversion, NO FK, NO TG
// types, NO export. Those arrive in 1B/1C with the shared importer functions.

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

const char* kWeightSumTol = nullptr;  // doc anchor; tolerance is kWeightTol below
constexpr float kWeightTol = 0.02f;   // per-vertex weight sum must be within this of 1.0

bool isFinite4x4(const aiMatrix4x4& m) {
    const float* p = &m.a1;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

// Load with NO post-processing so we report the RAW authored skeleton/weights
// (Triangulate/JoinIdenticalVertices would mutate counts and weights). Validate
// inspects exactly what the importer's front end receives.
const aiScene* load(Assimp::Importer& imp, const char* path) {
    return imp.ReadFile(path, 0);
}

// Collect every bone (by name) referenced by any mesh, with its offset (inverse
// bind) matrix. Assimp stores bones per-mesh; the same bone can recur across
// meshes — last writer wins (offset matrices are identical across meshes).
struct BoneInfo { aiMatrix4x4 offset; bool seen = false; };

void gatherBones(const aiScene* s, std::map<std::string, BoneInfo>& out) {
    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        const aiMesh* mesh = s->mMeshes[m];
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            out[bone->mName.C_Str()] = BoneInfo{bone->mOffsetMatrix, true};
        }
    }
}

// Map node name -> parent node name across the whole node tree (for hierarchy
// printing + bad-parent / missing-bone-node checks).
void mapParents(const aiNode* n, const std::string& parent,
                std::map<std::string, std::string>& out) {
    out[n->mName.C_Str()] = parent;
    for (unsigned i = 0; i < n->mNumChildren; ++i)
        mapParents(n->mChildren[i], n->mName.C_Str(), out);
}

struct WeightStats {
    long weightedVerts = 0;
    long totalVerts = 0;
    int maxWeightsPerVert = 0;
    float minSum = 1e9f, maxSum = -1e9f;
    long badSumVerts = 0;
};

WeightStats weightStats(const aiScene* s) {
    WeightStats ws;
    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        const aiMesh* mesh = s->mMeshes[m];
        std::vector<float> sum(mesh->mNumVertices, 0.0f);
        std::vector<int> cnt(mesh->mNumVertices, 0);
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                const aiVertexWeight& vw = bone->mWeights[w];
                if (vw.mVertexId < mesh->mNumVertices) {
                    sum[vw.mVertexId] += vw.mWeight;
                    cnt[vw.mVertexId]++;
                }
            }
        }
        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            ws.totalVerts++;
            if (cnt[v] > 0) {
                ws.weightedVerts++;
                if (cnt[v] > ws.maxWeightsPerVert) ws.maxWeightsPerVert = cnt[v];
                if (sum[v] < ws.minSum) ws.minSum = sum[v];
                if (sum[v] > ws.maxSum) ws.maxSum = sum[v];
                if (std::fabs(sum[v] - 1.0f) > kWeightTol) ws.badSumVerts++;
            }
        }
    }
    return ws;
}

void geomTotals(const aiScene* s, long& verts, long& faces) {
    verts = faces = 0;
    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        verts += s->mMeshes[m]->mNumVertices;
        faces += s->mMeshes[m]->mNumFaces;
    }
}

int doInspect(const aiScene* s) {
    long verts, faces; geomTotals(s, verts, faces);
    std::printf("== GEOMETRY ==\n");
    std::printf("meshes      %u\n", s->mNumMeshes);
    std::printf("materials   %u\n", s->mNumMaterials);
    std::printf("vertices    %ld\n", verts);
    std::printf("faces       %ld\n", faces);
    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        const aiMesh* mesh = s->mMeshes[m];
        std::printf("  mesh[%u] %-28s v=%u f=%u bones=%u uv=%d\n", m,
                    mesh->mName.C_Str(), mesh->mNumVertices, mesh->mNumFaces,
                    mesh->mNumBones, mesh->HasTextureCoords(0) ? 1 : 0);
    }
    std::printf("materials:\n");
    for (unsigned i = 0; i < s->mNumMaterials; ++i) {
        aiString nm; s->mMaterials[i]->Get(AI_MATKEY_NAME, nm);
        std::printf("  [%u] %s\n", i, nm.C_Str());
    }

    std::map<std::string, BoneInfo> bones; gatherBones(s, bones);
    std::map<std::string, std::string> parents; mapParents(s->mRootNode, "", parents);

    std::printf("\n== SKELETON ==\n");
    std::printf("bones       %zu\n", bones.size());
    // Root bone = a bone whose parent chain leaves the bone set (topmost bone).
    std::string root;
    for (auto& kv : bones) {
        std::string p = parents.count(kv.first) ? parents[kv.first] : "";
        if (!bones.count(p)) { root = kv.first; break; }
    }
    std::printf("root bone   %s\n", root.empty() ? "<none>" : root.c_str());
    std::printf("parent hierarchy (bone -> parent):\n");
    int shown = 0;
    for (auto& kv : bones) {
        std::string p = parents.count(kv.first) ? parents[kv.first] : "<orphan>";
        std::printf("  %-28s -> %s\n", kv.first.c_str(), p.c_str());
        if (++shown >= 80) { std::printf("  ... (%zu total)\n", bones.size()); break; }
    }
    std::printf("inverse-bind matrices: %zu (det of first few)\n", bones.size());
    shown = 0;
    for (auto& kv : bones) {
        float det = kv.second.offset.Determinant();
        std::printf("  %-28s det=%.5f finite=%d\n", kv.first.c_str(), det,
                    isFinite4x4(kv.second.offset) ? 1 : 0);
        if (++shown >= 8) break;
    }

    std::printf("\n== ANIMATION ==\n");
    std::printf("clips       %u\n", s->mNumAnimations);
    for (unsigned a = 0; a < s->mNumAnimations; ++a) {
        const aiAnimation* an = s->mAnimations[a];
        double tps = an->mTicksPerSecond != 0.0 ? an->mTicksPerSecond : 1.0;
        std::printf("  [%2u] %-34s dur=%.1f tps=%.1f sec=%.2f channels=%u\n", a,
                    an->mName.C_Str(), an->mDuration, an->mTicksPerSecond,
                    an->mDuration / tps, an->mNumChannels);
    }

    WeightStats ws = weightStats(s);
    std::printf("\n== WEIGHTS ==\n");
    std::printf("weighted vertices   %ld / %ld\n", ws.weightedVerts, ws.totalVerts);
    std::printf("max weights/vertex  %d\n", ws.maxWeightsPerVert);
    if (ws.weightedVerts > 0)
        std::printf("weight-sum range    [%.4f .. %.4f]  (off-by->%.0ftol: %ld verts)\n",
                    ws.minSum, ws.maxSum, kWeightTol * 1000, ws.badSumVerts);
    return 0;
}

int doValidate(const aiScene* s) {
    int fails = 0;
    auto fail = [&](const char* msg) { std::printf("FAIL: %s\n", msg); ++fails; };
    auto ok = [&](const char* msg) { std::printf("ok:   %s\n", msg); };

    if (s->mNumMeshes == 0) fail("no meshes"); else ok("meshes present");

    std::map<std::string, BoneInfo> bones; gatherBones(s, bones);
    if (bones.empty()) fail("no skeleton (zero bones)"); else ok("skeleton present");

    std::map<std::string, std::string> parents; mapParents(s->mRootNode, "", parents);

    // Every bone must resolve to a node in the tree (bad/missing parent index).
    int missingNode = 0, nanMat = 0, singular = 0;
    for (auto& kv : bones) {
        if (!parents.count(kv.first)) ++missingNode;
        if (!isFinite4x4(kv.second.offset)) ++nanMat;
        else if (std::fabs(kv.second.offset.Determinant()) < 1e-8f) ++singular;
    }
    if (missingNode) fail("bone(s) reference a node not in the hierarchy"); else ok("all bones resolve to nodes");
    if (nanMat) fail("NaN/inf in an inverse-bind matrix"); else ok("inverse-bind matrices finite");
    if (singular) fail("non-invertible inverse-bind matrix (det~0)"); else ok("inverse-bind matrices invertible");

    // Weights: existence, per-vertex sum sanity.
    WeightStats ws = weightStats(s);
    if (ws.weightedVerts == 0) fail("no vertex weights"); else ok("vertex weights present");
    if (ws.weightedVerts > 0 && ws.badSumVerts > 0) {
        std::printf("FAIL: %ld vertices have weight sum outside 1.0+/-%.2f\n", ws.badSumVerts, kWeightTol);
        ++fails;
    } else if (ws.weightedVerts > 0) ok("weight sums ~= 1.0");

    // Clips must reference bones/nodes that exist.
    int badClipRefs = 0;
    for (unsigned a = 0; a < s->mNumAnimations; ++a) {
        const aiAnimation* an = s->mAnimations[a];
        for (unsigned c = 0; c < an->mNumChannels; ++c) {
            const char* nn = an->mChannels[c]->mNodeName.C_Str();
            if (!parents.count(nn)) ++badClipRefs;
        }
    }
    if (badClipRefs) fail("animation channel references a missing node/bone"); else ok("clip channels resolve");

    // Position finiteness + sane bounds (coarse coord-conversion sanity).
    float lo = 1e30f, hi = -1e30f; bool posNan = false;
    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        const aiMesh* mesh = s->mMeshes[m];
        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            const aiVector3D& p = mesh->mVertices[v];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) { posNan = true; continue; }
            for (float c : {p.x, p.y, p.z}) { if (c < lo) lo = c; if (c > hi) hi = c; }
        }
    }
    if (posNan) fail("NaN/inf vertex position"); else ok("vertex positions finite");
    if (std::isfinite(lo) && (hi - lo) > 1e6f) fail("extreme vertex bounds (>1e6 extent)"); else ok("vertex bounds sane");

    std::printf("\n%s (%d failure%s)\n", fails ? "VALIDATE FAILED" : "VALIDATE PASSED",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

void usage(const char* exe) {
    std::fprintf(stderr,
        "MECH-IMPORT-HARNESS-1A (Assimp-only)\n"
        "usage:\n"
        "  %s inspect  <model.glb|.fbx>\n"
        "  %s validate <model.glb|.fbx>   (exit nonzero on any failure)\n", exe, exe);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 2; }
    const std::string mode = argv[1];
    const char* path = argv[2];

    Assimp::Importer imp;
    const aiScene* scene = load(imp, path);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        std::fprintf(stderr, "ERROR: load failed for '%s': %s\n", path, imp.GetErrorString());
        return 1;
    }

    if (mode == "inspect")  return doInspect(scene);
    if (mode == "validate") return doValidate(scene);
    usage(argv[0]);
    return 2;
}
