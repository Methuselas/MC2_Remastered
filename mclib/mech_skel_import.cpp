// mech_skel_import.cpp — shared skeleton/FK importer math (see header).
// The one place the FK chain lives; harness and game both link this TU.
#include "mech_skel_import.h"

#include <assimp/scene.h>
#include <assimp/anim.h>

#include <cstring>
#include <map>

namespace mc2skel {
namespace {

// Sample one node-animation channel at tick time t -> local transform.
aiMatrix4x4 sampleChannel(const aiNodeAnim* ch, double t) {
    auto lerpVec = [&](const aiVectorKey* keys, unsigned n) -> aiVector3D {
        if (n == 1) return keys[0].mValue;
        unsigned i = 0;
        while (i + 1 < n && keys[i + 1].mTime <= t) ++i;
        if (i + 1 >= n) return keys[n - 1].mValue;
        double t0 = keys[i].mTime, t1 = keys[i + 1].mTime;
        float f = (t1 > t0) ? (float)((t - t0) / (t1 - t0)) : 0.0f;
        return keys[i].mValue * (1.0f - f) + keys[i + 1].mValue * f;
    };
    aiVector3D pos = ch->mNumPositionKeys ? lerpVec(ch->mPositionKeys, ch->mNumPositionKeys) : aiVector3D(0, 0, 0);
    aiVector3D scl = ch->mNumScalingKeys ? lerpVec(ch->mScalingKeys, ch->mNumScalingKeys) : aiVector3D(1, 1, 1);
    aiQuaternion rot;
    if (ch->mNumRotationKeys == 1) {
        rot = ch->mRotationKeys[0].mValue;
    } else if (ch->mNumRotationKeys > 1) {
        unsigned i = 0;
        while (i + 1 < ch->mNumRotationKeys && ch->mRotationKeys[i + 1].mTime <= t) ++i;
        if (i + 1 >= ch->mNumRotationKeys) {
            rot = ch->mRotationKeys[ch->mNumRotationKeys - 1].mValue;
        } else {
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

// Map node name -> parent node name across the whole tree.
void mapParents(const aiNode* n, const std::string& parent,
                std::map<std::string, std::string>& out) {
    out[n->mName.C_Str()] = parent;
    for (unsigned i = 0; i < n->mNumChildren; ++i)
        mapParents(n->mChildren[i], n->mName.C_Str(), out);
}

void rowMajor(const aiMatrix4x4& m, float* out16) { std::memcpy(out16, &m.a1, 16 * sizeof(float)); }

}  // namespace

void BuildSkeleton(const aiScene* scene,
                   std::vector<std::string>& boneNames,
                   std::vector<int>& parentIndex,
                   std::vector<std::array<float, 16>>& inverseBind) {
    boneNames.clear(); parentIndex.clear(); inverseBind.clear();

    // Ordered de-dup of bones in first-encounter order (stable bone indices).
    std::map<std::string, int> idx;
    std::map<std::string, aiMatrix4x4> offsets;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            std::string nm = bone->mName.C_Str();
            if (!idx.count(nm)) {
                idx[nm] = (int)boneNames.size();
                boneNames.push_back(nm);
                std::array<float, 16> ib; rowMajor(bone->mOffsetMatrix, ib.data());
                inverseBind.push_back(ib);
            }
        }
    }

    // Parent index = nearest ancestor node that is also a bone.
    std::map<std::string, std::string> parents;
    mapParents(scene->mRootNode, "", parents);
    parentIndex.resize(boneNames.size(), -1);
    for (size_t i = 0; i < boneNames.size(); ++i) {
        std::string p = parents.count(boneNames[i]) ? parents[boneNames[i]] : "";
        while (!p.empty() && !idx.count(p))
            p = parents.count(p) ? parents[p] : "";
        parentIndex[i] = (!p.empty() && idx.count(p)) ? idx[p] : -1;
    }
}

bool EvaluateClipGpuBones(const aiScene* scene,
                          const std::string& clip,
                          float frame,
                          const std::vector<std::string>& boneNames,
                          std::vector<GpuBone>& out,
                          double* outTimeTicks,
                          double* outDurTicks) {
    const aiAnimation* anim = nullptr;
    for (unsigned a = 0; a < scene->mNumAnimations; ++a)
        if (clip == scene->mAnimations[a]->mName.C_Str()) { anim = scene->mAnimations[a]; break; }
    if (!anim) return false;

    double tps = anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 1000.0;
    double t = (double)frame / 30.0 * tps;
    if (t > anim->mDuration) t = anim->mDuration;
    if (t < 0) t = 0;
    if (outTimeTicks) *outTimeTicks = t;
    if (outDurTicks) *outDurTicks = anim->mDuration;

    std::map<std::string, const aiNodeAnim*> chans;
    for (unsigned c = 0; c < anim->mNumChannels; ++c)
        chans[anim->mChannels[c]->mNodeName.C_Str()] = anim->mChannels[c];

    std::map<std::string, aiMatrix4x4> globals;
    computeGlobals(scene->mRootNode, aiMatrix4x4(), chans, t, globals);

    // Return the joint GLOBAL (FK world) matrix per bone — NOT global*inverseBind.
    // The BT2018 rig has a DIFFERENT inverse-bind per mesh-part for the same bone
    // (19/28 bones carry 2 distinct offsets across skins), so inverse-bind cannot
    // be folded into one per-bone matrix. Instead the caller bakes each part's own
    // offset into its vertices at import; the per-frame GPU matrix is then just the
    // joint global. skin = global(bone) * (offset_part * vertex).
    out.assign(boneNames.size(), GpuBone{});
    for (size_t i = 0; i < boneNames.size(); ++i) {
        auto g = globals.find(boneNames[i]);
        rowMajor(g != globals.end() ? g->second : aiMatrix4x4(), out[i].m);
    }
    return true;
}

}  // namespace mc2skel
