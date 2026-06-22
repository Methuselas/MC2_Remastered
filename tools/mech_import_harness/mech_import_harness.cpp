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

#include <cctype>
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

// ---------------------------------------------------------------------------
// 1B — rigid FK pose dump. Exploits the single-bone-per-vertex property found in
// 1A: each rendered part rigidly follows one bone, so skinned = global(bone) *
// offsetMatrix * vertex (no weight blend needed). Math lives HERE in the harness
// first (Assimp-only); engine/GPU integration comes after it's visually right.

bool nameDropped(const std::string& n) {
    std::string s = n;
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s.find("_explode") != std::string::npos || s.find("_dmg") != std::string::npos
        || s.find("blip") != std::string::npos || s.find("indc") != std::string::npos
        || s.find("uix") != std::string::npos;
}

const aiAnimation* findClip(const aiScene* s, const std::string& name) {
    for (unsigned a = 0; a < s->mNumAnimations; ++a)
        if (name == s->mAnimations[a]->mName.C_Str()) return s->mAnimations[a];
    return nullptr;
}

// Sample one node-animation channel at tick time t -> local transform.
aiMatrix4x4 sampleChannel(const aiNodeAnim* ch, double t) {
    auto lerpVec = [&](const aiVectorKey* keys, unsigned n) -> aiVector3D {
        if (n == 1) return keys[0].mValue;
        unsigned i = 0; while (i + 1 < n && keys[i + 1].mTime <= t) ++i;
        if (i + 1 >= n) return keys[n - 1].mValue;
        double t0 = keys[i].mTime, t1 = keys[i + 1].mTime;
        float f = (t1 > t0) ? (float)((t - t0) / (t1 - t0)) : 0.0f;
        return keys[i].mValue * (1.0f - f) + keys[i + 1].mValue * f;
    };
    aiVector3D pos = ch->mNumPositionKeys ? lerpVec(ch->mPositionKeys, ch->mNumPositionKeys) : aiVector3D(0, 0, 0);
    aiVector3D scl = ch->mNumScalingKeys ? lerpVec(ch->mScalingKeys, ch->mNumScalingKeys) : aiVector3D(1, 1, 1);
    aiQuaternion rot;
    if (ch->mNumRotationKeys == 1) rot = ch->mRotationKeys[0].mValue;
    else if (ch->mNumRotationKeys > 1) {
        unsigned i = 0; while (i + 1 < ch->mNumRotationKeys && ch->mRotationKeys[i + 1].mTime <= t) ++i;
        if (i + 1 >= ch->mNumRotationKeys) rot = ch->mRotationKeys[ch->mNumRotationKeys - 1].mValue;
        else {
            double t0 = ch->mRotationKeys[i].mTime, t1 = ch->mRotationKeys[i + 1].mTime;
            float f = (t1 > t0) ? (float)((t - t0) / (t1 - t0)) : 0.0f;
            aiQuaternion::Interpolate(rot, ch->mRotationKeys[i].mValue, ch->mRotationKeys[i + 1].mValue, f);
            rot.Normalize();
        }
    }
    return aiMatrix4x4(scl, rot, pos);
}

void computeGlobals(const aiNode* n, const aiMatrix4x4& parent,
                    const std::map<std::string, const aiNodeAnim*>& chans, double t,
                    std::map<std::string, aiMatrix4x4>& globals) {
    auto it = chans.find(n->mName.C_Str());
    aiMatrix4x4 local = (it != chans.end()) ? sampleChannel(it->second, t) : n->mTransformation;
    aiMatrix4x4 global = parent * local;
    globals[n->mName.C_Str()] = global;
    for (unsigned i = 0; i < n->mNumChildren; ++i)
        computeGlobals(n->mChildren[i], global, chans, t, globals);
}

int doPose(const aiScene* s, const std::string& clipName, int frame,
           const std::string& outPath, bool compareRest) {
    const aiAnimation* clip = findClip(s, clipName);
    if (!clip) { std::fprintf(stderr, "ERROR: clip '%s' not found\n", clipName.c_str()); return 1; }
    double tps = clip->mTicksPerSecond != 0.0 ? clip->mTicksPerSecond : 1000.0;
    // --frame interpreted at 30 fps; clamp to clip duration.
    double t = (double)frame / 30.0 * tps;
    if (t > clip->mDuration) t = clip->mDuration;
    if (t < 0) t = 0;

    std::map<std::string, const aiNodeAnim*> chans;
    for (unsigned c = 0; c < clip->mNumChannels; ++c)
        chans[clip->mChannels[c]->mNodeName.C_Str()] = clip->mChannels[c];

    std::map<std::string, aiMatrix4x4> globals;          // posed
    computeGlobals(s->mRootNode, aiMatrix4x4(), chans, t, globals);
    std::map<std::string, aiMatrix4x4> restG;            // rest (no clip)
    if (compareRest) {
        std::map<std::string, const aiNodeAnim*> none;
        computeGlobals(s->mRootNode, aiMatrix4x4(), none, 0, restG);
    }

    std::printf("clip=%s frame=%d t=%.1f/%.1f ticks tps=%.0f bones=%zu\n",
                clipName.c_str(), frame, t, clip->mDuration, tps, globals.size());

    FILE* obj = nullptr;
    if (!outPath.empty()) { obj = std::fopen(outPath.c_str(), "w"); if (!obj) { std::fprintf(stderr, "ERROR: cannot write %s\n", outPath.c_str()); return 1; } }

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    long emittedV = 0, emittedTris = 0, voff = 0, droppedMeshes = 0, nanV = 0;
    int samplesShown = 0;

    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        const aiMesh* mesh = s->mMeshes[m];
        if (nameDropped(mesh->mName.C_Str()) || mesh->mNumBones == 0) { ++droppedMeshes; continue; }
        // Per-vertex rigid skin (single bone weight=1; general Σ kept for safety).
        std::vector<aiVector3D> out(mesh->mNumVertices, aiVector3D(0, 0, 0));
        std::vector<float> wsum(mesh->mNumVertices, 0.0f);
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            auto git = globals.find(bone->mName.C_Str());
            if (git == globals.end()) continue;
            aiMatrix4x4 skin = git->second * bone->mOffsetMatrix;
            for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                const aiVertexWeight& vw = bone->mWeights[w];
                if (vw.mVertexId >= mesh->mNumVertices) continue;
                aiVector3D p = skin * mesh->mVertices[vw.mVertexId];
                out[vw.mVertexId] += p * vw.mWeight;
                wsum[vw.mVertexId] += vw.mWeight;
            }
        }
        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            aiVector3D p = out[v];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) { ++nanV; continue; }
            for (int k = 0; k < 3; ++k) { float c = p[k]; if (c < lo[k]) lo[k] = c; if (c > hi[k]) hi[k] = c; }
            if (obj) std::fprintf(obj, "v %.5f %.5f %.5f\n", p.x, p.y, p.z);
            if (samplesShown < 5) { std::printf("  sample v[%u/%u] (%.3f, %.3f, %.3f)\n", v, m, p.x, p.y, p.z); ++samplesShown; }
            ++emittedV;
        }
        if (obj) for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& fc = mesh->mFaces[f];
            if (fc.mNumIndices == 3)
                std::fprintf(obj, "f %u %u %u\n", voff + fc.mIndices[0] + 1, voff + fc.mIndices[1] + 1, voff + fc.mIndices[2] + 1);
        }
        voff += mesh->mNumVertices; emittedTris += mesh->mNumFaces;
    }
    if (obj) std::fclose(obj);

    std::printf("root transform (j_Root global):\n");
    if (globals.count("j_Root")) { const aiMatrix4x4& r = globals["j_Root"];
        std::printf("  [% .3f % .3f % .3f % .3f]\n", r.a1, r.a2, r.a3, r.a4); }
    std::printf("emitted verts=%ld tris=%ld droppedMeshes=%ld nanVerts=%ld\n", emittedV, emittedTris, droppedMeshes, nanV);
    std::printf("world bbox X[% .3f..% .3f] Y[% .3f..% .3f] Z[% .3f..% .3f] dims(%.3f, %.3f, %.3f)\n",
                lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]);
    if (compareRest) {
        float maxDelta = 0.0f; std::string worst;
        for (auto& kv : globals) {
            auto r = restG.find(kv.first); if (r == restG.end()) continue;
            aiVector3D dp(kv.second.a4 - r->second.a4, kv.second.b4 - r->second.b4, kv.second.c4 - r->second.c4);
            float d = dp.Length(); if (d > maxDelta) { maxDelta = d; worst = kv.first; }
        }
        std::printf("compare-rest: max bone translation delta=%.4f at %s\n", maxDelta, worst.c_str());
    }
    if (!outPath.empty()) std::printf("wrote %s\n", outPath.c_str());
    if (nanV) { std::printf("FAIL: %ld NaN vertices in pose\n", nanV); return 1; }
    if (emittedV == 0) { std::printf("FAIL: no vertices emitted\n"); return 1; }
    return 0;
}

void usage(const char* exe) {
    std::fprintf(stderr,
        "MECH-IMPORT-HARNESS-1A/1B (Assimp-only)\n"
        "usage:\n"
        "  %s inspect    <model.glb|.fbx>\n"
        "  %s validate   <model.glb|.fbx>   (exit nonzero on any failure)\n"
        "  %s pose       <model.glb|.fbx> --clip <name> --frame <n> [--out <pose.obj>] [--compare-rest]\n", exe, exe, exe);
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
    if (mode == "pose") {
        std::string clip, out; int frame = 0; bool cmpRest = false;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--clip" && i + 1 < argc) clip = argv[++i];
            else if (a == "--frame" && i + 1 < argc) frame = std::atoi(argv[++i]);
            else if (a == "--out" && i + 1 < argc) out = argv[++i];
            else if (a == "--compare-rest") cmpRest = true;
        }
        if (clip.empty()) { std::fprintf(stderr, "ERROR: pose requires --clip <name>\n"); return 2; }
        return doPose(scene, clip, frame, out, cmpRest);
    }
    usage(argv[0]);
    return 2;
}
