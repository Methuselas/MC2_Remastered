// assimp_importer.cpp — Track D Assimp-backed FBX/GLB mech importer.
//
// Geometry-only MVP slice. Populates TG_TypeMultiShape / TG_TypeShape from a
// .glb (preferred) or .fbx source, identically to what ParseASEFile produces
// for the same geometry. Animation, LOD swap, shadow mesh, palette swap, and
// .tglc cache are all M2 — see findings doc.
//
// Architectural invariant: import terminates at TG_TypeMultiShape. Renderer is
// downstream and unchanged. No Assimp types in any TG header.
//
// Spec: docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md
// Plan: docs/superpowers/plans/2026-04-27-assimp-mech-importer.md
// Findings: docs/superpowers/explorations/2026-05-02-track-d-mvp-adversarial-findings.md
#include "assimp_importer.h"

#ifdef ENABLE_ASSIMP_IMPORTER

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>

#include <vector>
#include <string>
#include <set>
#include <map>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdio>

#include "msl.h"
#include "tgl.h"
#include "mech_skel_import.h"  // BT2018-SKEL-ENGINE-1A: shared FK/skeleton math
#include "mech_anim_runtime.h" // BT2018-SKEL-ENGINE-1B-RUNTIME: per-frame re-bake API

// BT2018-SKEL-ENGINE-1A — bind-pose bake trace (default OFF). MC2_MECH_SKEL_TRACE=1.
static const bool s_mechSkelTrace = (getenv("MC2_MECH_SKEL_TRACE") != NULL);
#define MECH_SKEL_TRACE(fmt, ...) \
    do { if (s_mechSkelTrace) { \
        fprintf(stderr, "[MECH_SKEL] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); } } while (0)

// Track D — env-gated diagnostic trace (default OFF). Set MC2_ASSIMP_TRACE=1
// to emit per-import checkpoint lines. Convention matches existing
// [TGL_POOL v1] / [DESTROY v1] env-gated tracers
// (memory:debug_instrumentation_rule).
static const bool s_assimpTrace = (getenv("MC2_ASSIMP_TRACE") != NULL);
#define ASSIMP_TRACE(fmt, ...) \
    do { if (s_assimpTrace) { \
        fprintf(stderr, "[ASSIMP_TRACE] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); } } while (0)

namespace {

//-----------------------------------------------------------------------------
// Coordinate transforms (spec §6).
//
// glTF is Y-up, right-handed (X-right, Y-up, Z-toward-viewer). The engine's
// world-up in Stuff space is stuff.Y (stock ASE trees load their up axis into
// position.y; the static_prop shader maps stuff.z->GL.up via MC2). The previous
// mapping (mc2.y=src.z, mc2.z=src.y) was written for an ASE/Max Z-up source and
// put the glTF up-axis (Y) into stuff.z -> every imported override mesh rendered
// LYING ON ITS SIDE. Correct mapping for Y-up glTF: up (Y) -> stuff.y. We negate
// X AND Z (two axis flips) so triangle winding / handedness is preserved (a
// single flip would invert winding and backface-cull the mesh).
// MC2_GLTF_AXIS (0..3) selects the axis mapping at runtime so orientation can be
// dialed in-game without rebuilds; MC2_GLTF_YOFF nudges the up component (stuff.y)
// to lift a buried mesh (pivot-not-at-base). All four mappings are even-parity
// (winding preserved). Bake the winning combo as the default once confirmed.
static inline int s_gltfAxis() { const char* e=getenv("MC2_GLTF_AXIS"); return e?atoi(e):0; }
static inline float s_gltfYoff() { static float o=[]{const char*e=getenv("MC2_GLTF_YOFF"); return e?(float)atof(e):0.0f;}(); return o; }
static inline float s_gltfYawDeg() { const char* e=getenv("MC2_GLTF_YAW_DEG"); return e?(float)atof(e):0.0f; }
static inline void applyYaw(float& X, float& Z) {
	const float yaw = s_gltfYawDeg();
	if (fabsf(yaw) < 0.001f)
		return;
	const float r = yaw * 0.017453292519943295769f;
	const float c = cosf(r);
	const float s = sinf(r);
	const float x = X * c - Z * s;
	const float z = X * s + Z * c;
	X = x;
	Z = z;
}
static inline void axisMap(const aiVector3D& v, float& X, float& Y, float& Z) {
	switch (s_gltfAxis()) {
	default:
	case 0: X=-v.x; Y=-v.y; Z= v.z; break;  // -x,-y,z
	case 1: X=-v.x; Y= v.z; Z= v.y; break;  // -x,z,y (original 'on-side')
	case 2: X=-v.x; Y= v.y; Z=-v.z; break;  // -x,y,-z ('upside down')
	case 3: X=-v.x; Y=-v.z; Z=-v.y; break;  // -x,-z,-y
	}
	applyYaw(X, Z);
}
inline Stuff::Point3D toMC2Pos(const aiVector3D& v) {
	Stuff::Point3D p; float X,Y,Z; axisMap(v,X,Y,Z);
	p.x = X; p.y = Y + s_gltfYoff(); p.z = Z;
	return p;
}
inline Stuff::Vector3D toMC2Vec(const aiVector3D& v) {
	Stuff::Vector3D n; float X,Y,Z; axisMap(v,X,Y,Z);
	n.x = X; n.y = Y; n.z = Z;   // normals: no translation
	return n;
}
// UV V-flip (spec §6).
inline float toMC2V(float v) { return 1.0f - v; }

//-----------------------------------------------------------------------------
// BT2018-SKEL-ENGINE-1A — bind-pose bake context.
//
// When the source GLB is skinned (mech with a skeleton), each mesh-part's
// vertices live in bone-local space and only the skeleton assembles them. The
// rig stores a DIFFERENT inverse-bind per part for the same bone (proven in the
// harness), so we bake each part's OWN offset into its vertices at import, then
// apply the joint GLOBAL rest transform: v_world = restGlobal(bone) * offset * v
// (all in glTF space, BEFORE toMC2Pos). Result = the assembled bind pose, the
// same geometry the Python fk_bake static export produced. Animation is M2.
//
// Buildings/props/trees import with NO bones -> bake disabled -> unchanged path.
// BT2018-SKEL-ENGINE-1B-RUNTIME — per-part record for the per-frame re-bake.
// Captures, during the one-time merge, exactly what TickImportedMechs needs to
// re-pose this part from a clip global each frame: which source mesh, which bone
// (index into SkelBake::names), that part's bone offset, and where the part's
// vertices landed in the merged listOfTypeVertices. boneIndex < 0 => rigid stray
// (no bone) baked with scale only, mirroring PopulateMergedSkinnedShape.
struct ImportPartRec {
    unsigned meshIndex;
    int      boneIndex;
    aiMatrix4x4 off;
    unsigned vOff;
    unsigned vCount;
};

struct SkelBake {
    bool active = false;
    std::vector<std::string> names;
    std::vector<int> parents;
    std::vector<std::array<float, 16>> invBind;
    std::vector<mc2skel::GpuBone> rest;       // joint-global rest matrices, parallel to names
    std::map<std::string, int> nameIdx;
    float scale = 1.0f;                        // auto-scale bind pose to MC2 size
    std::string forcedClip;                    // 1B: posed-clip bake (empty = rest)
    int forcedFrame = 0;
    // 1B-RUNTIME: when non-null, PopulateMergedSkinnedShape records each part's
    // layout here so the import can register a per-frame re-bake (see below).
    std::vector<ImportPartRec>* recParts = nullptr;
    // counters (trace)
    int importedParts = 0, partsSingleBone = 0, partsOffsetBaked = 0, droppedParts = 0;
};

// Apply a row-major float[16] (joint global) to an Assimp point (w=1).
inline aiVector3D applyRowMajor16(const float* m, const aiVector3D& v) {
    return aiVector3D(
        m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
        m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
        m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]);
}
// Direction transform (3x3 part only; for normals).
inline aiVector3D rotRowMajor16(const float* m, const aiVector3D& v) {
    return aiVector3D(
        m[0] * v.x + m[1] * v.y + m[2] * v.z,
        m[4] * v.x + m[5] * v.y + m[6] * v.z,
        m[8] * v.x + m[9] * v.y + m[10] * v.z);
}

// Fixed mech orientation (glTF Y-up -> MC2), confirmed in-game (== MC2_GLTF_AXIS=2:
// X=-x, Y=+y, Z=-z) and baked as the skinned-mech default. Even parity (two flips)
// so triangle winding is preserved. Buildings/props keep the global axisMap default;
// the mech's own up-axis differs, so it carries its own fixed mapping here.
inline Stuff::Point3D mechToMC2Pos(const aiVector3D& v) {
    Stuff::Point3D p; p.x = -v.x; p.y = v.y; p.z = -v.z; return p;
}
inline Stuff::Vector3D mechToMC2Vec(const aiVector3D& v) {
    Stuff::Vector3D n; n.x = -v.x; n.y = v.y; n.z = -v.z; return n;
}

// Damage/destruction/UI parts excluded from the intact bind-pose import (they
// overlap the intact geometry; needed later for damage states — POC drops them).
inline bool skelMeshDropped(const char* n) {
    if (!n) return false;
    std::string s = n;
    for (char& c : s) c = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    return s.find("_explode") != std::string::npos || s.find("_dmg") != std::string::npos
        || s.find("blip") != std::string::npos || s.find("indc") != std::string::npos
        || s.find("uix") != std::string::npos;
}

//-----------------------------------------------------------------------------
// Validator. Returns -1 on hard error; 0 on pass.
// Hard errors abort the import (no cache write equivalent here for MVP):
//   - any node name longer than 24 chars (TG_NODE_ID-1 — silent truncation
//     would break animation binding; spec §5)
//   - any duplicate node name (would break node ID lookup)
//   - no renderable mesh (numMeshes == 0)
// Helper-object-style nodes (handle_*, World*) are not flagged for now —
// the importer only consumes scene->mMeshes anyway.
long ValidateScene(const aiScene* scene, const char* path) {
	if (!scene || !scene->mRootNode) {
		PAUSE(("[importer] %s: Assimp returned null scene", path));
		return -1;
	}
	if (scene->mNumMeshes == 0) {
		PAUSE(("[importer] %s: no meshes", path));
		return -1;
	}

	// Walk the scene graph; collect node names; flag length and duplicates.
	std::set<std::string> seen;
	std::vector<const aiNode*> stack;
	stack.push_back(scene->mRootNode);
	while (!stack.empty()) {
		const aiNode* n = stack.back();
		stack.pop_back();
		const char* name = n->mName.C_Str();
		const size_t len = strlen(name);
		// VALIDATE-SCENE-ASSIMP-NAME-LEN-1: GLB/FBX node names routinely exceed
		// the legacy TG_NODE_ID (24-char) field (e.g. BT2018 'mad_centre_torso_
		// pelvis_dmg' = 27). That length cap is an MC2/ASE runtime-field limit, not
		// a glTF constraint — rejecting valid GLB names here is wrong. Do NOT abort;
		// the importer truncates only where the name is written into the fixed-size
		// nodeId (TG_TypeShape::InitFromImportedMesh, which logs original->truncated).
		// Node names only drive animation binding (M2); bind-pose/static import does
		// not bind by name. Warn under trace and continue.
		if (len >= TG_NODE_ID) {
			ASSIMP_TRACE("  node name '%s' (%zu chars) > %d; nodeId will be truncated on import",
			             name, len, TG_NODE_ID - 1);
		}
		if (len > 0) {
			if (!seen.insert(name).second) {
				STOP(("[importer] %s: duplicate node name '%s' (breaks node-ID lookup)",
				      path, name));
				return -1;
			}
		}
		for (unsigned i = 0; i < n->mNumChildren; i++)
			stack.push_back(n->mChildren[i]);
	}

	return 0;
}

//-----------------------------------------------------------------------------
// Find the aiNode that references mesh index `meshIdx`. Walks the scene graph
// recursively; returns NULL if no node owns the mesh (rare but possible in
// malformed files — caller falls back to root-level identity).
const aiNode* FindNodeForMesh(const aiNode* node, unsigned meshIdx) {
	for (unsigned i = 0; i < node->mNumMeshes; i++)
		if (node->mMeshes[i] == meshIdx) return node;
	for (unsigned i = 0; i < node->mNumChildren; i++) {
		const aiNode* found = FindNodeForMesh(node->mChildren[i], meshIdx);
		if (found) return found;
	}
	return NULL;
}

//-----------------------------------------------------------------------------
// Strip leading directory components from a texture path. Assimp can return
// absolute Windows paths, relative paths, or bare filenames depending on the
// source authoring tool; MC_TextureManager looks up by base name.
const char* StripPath(const char* p) {
	const char* slash1 = strrchr(p, '/');
	const char* slash2 = strrchr(p, '\\');
	const char* base = (slash1 > slash2) ? slash1 + 1 : (slash2 ? slash2 + 1 : p);
	return base;
}

//-----------------------------------------------------------------------------
// MODEL-OVERRIDE texture binding: derive an MC2 texture NAME from a glTF/FBX
// material's base-color image so MC_TextureManager can resolve it by name to a
// loose data/tgl/<size>/<name>.tga (file.cpp strips the size subdir) or BC7
// .ktx2 sidecar. MC2 stores the diffuse texture name WITH its ".tga" extension
// (see msl.cpp ParseASEFile + the shadow-X strlen-4 logic). So:
//   1. resolve the material's base-color/diffuse image to a filename,
//   2. strip any directory, strip the source extension (.png/.jpg/...),
//   3. sanitize to MC2-safe chars and lowercase,
//   4. append ".tga", clamped to the TG_Texture::textureName[256] field.
// Returns false when the material has NO base-color image (caller keeps the
// "NULLTXM" sentinel — truly-untextured material).
//
// Embedded GLB images: Assimp reports the texture path as "*<index>" into
// scene->mTextures[]. We resolve that to the embedded image's mFilename so the
// derived name matches the authored image stem, not an opaque index.
bool DeriveMC2TextureName(const aiScene* scene, const aiMaterial* mat,
                          std::string& outName) {
	aiString path;
	bool have = false;
	// glTF baseColor lands on BASE_COLOR in newer Assimp, DIFFUSE in the
	// compatibility mapping. Try both.
	if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &path) == AI_SUCCESS && path.length > 0)
		have = true;
	else if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &path) == AI_SUCCESS && path.length > 0)
		have = true;
	if (!have)
		return false;

	const char* raw = path.C_Str();

	// Resolve embedded "*N" to the source image filename when available.
	std::string resolved;
	if (raw[0] == '*' && scene != NULL) {
		const unsigned idx = (unsigned)atoi(raw + 1);
		if (idx < scene->mNumTextures && scene->mTextures[idx] != NULL
		    && scene->mTextures[idx]->mFilename.length > 0) {
			resolved = scene->mTextures[idx]->mFilename.C_Str();
		}
	}
	const char* src = resolved.empty() ? raw : resolved.c_str();

	// Strip directory, then strip extension.
	std::string stem = StripPath(src);
	const size_t dot = stem.find_last_of('.');
	if (dot != std::string::npos)
		stem.erase(dot);

	// Sanitize: lowercase, keep [a-z0-9_-], map everything else to '_'.
	for (size_t i = 0; i < stem.size(); ++i) {
		char c = stem[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
		              || c == '_' || c == '-';
		stem[i] = ok ? c : '_';
	}
	if (stem.empty())
		return false;

	// Clamp stem so stem + ".tga" + NUL fits TG_Texture::textureName[256].
	const size_t kMaxName = 256;
	const size_t kExt = 4; // ".tga"
	if (stem.size() + kExt + 1 > kMaxName)
		stem.erase(kMaxName - kExt - 1);

	stem += ".tga";

	// Alpha-cutout detection. MC2's texture loader uses an "a_" name prefix as
	// the alpha-channel convention (bdactor.cpp LoadOverrideRenderShapeTextures:
	// names starting "a_" → gos_Texture_Alpha + SetTextureAlpha(true) → the
	// static-prop batcher flags STATIC_PROP_FLAG_ALPHA_TEST for the packet).
	// A glTF leaf-card material is alphaMode MASK/BLEND; prefix "a_" so the
	// deployed RGBA TGA is loaded with its alpha channel and cuts out. Detect
	// via the glTF alphaMode key, falling back to a foliage name heuristic.
	bool wantAlpha = false;
	aiString alphaMode;
	// glTF alphaMode is exposed as the importer string key "$mat.gltf.alphaMode".
	if (mat->Get("$mat.gltf.alphaMode", 0, 0, alphaMode) == AI_SUCCESS) {
		const char* am = alphaMode.C_Str();
		if (am && (strcmp(am, "MASK") == 0 || strcmp(am, "BLEND") == 0))
			wantAlpha = true;
	}
	if (!wantAlpha) {
		aiString matName;
		if (mat->Get(AI_MATKEY_NAME, matName) == AI_SUCCESS) {
			std::string mn = matName.C_Str();
			for (size_t i = 0; i < mn.size(); ++i)
				if (mn[i] >= 'A' && mn[i] <= 'Z') mn[i] = (char)(mn[i] - 'A' + 'a');
			if (mn.find("leaf") != std::string::npos ||
			    mn.find("leaves") != std::string::npos ||
			    mn.find("foliage") != std::string::npos)
				wantAlpha = true;
		}
	}
	if (wantAlpha && stem.compare(0, 2, "a_") != 0) {
		// keep within textureName[256] after the 2-char prefix
		if (stem.size() + 2 + 1 > 256)
			stem.erase(256 - 2 - 1);
		stem.insert(0, "a_");
	}

	outName.swap(stem);
	return true;
}

//-----------------------------------------------------------------------------
// Build the multi-shape's TG_Texture[] from scene materials. One slot per
// Assimp material (one-to-one mapping); per-material diffuse texture name is
// extracted via aiTextureType_DIFFUSE channel 0. Materials with no diffuse
// texture get "NULLTXM" (matches ParseASEFile's empty-name fallback).
//
// Note: MVP skips ASE's Nx-base + Nx-shadow-X texture-list doubling. The
// shadow-X variant is engaged when a separate shadow shape file is present
// (existing engine path); the GLB-embedded shadow node is M2.
void BuildTextureList(const aiScene* scene, TG_TypeMultiShape* out) {
	const DWORD count = scene->mNumMaterials;
	if (count == 0) {
		out->SetImportedTextures(0, NULL, NULL);
		return;
	}

	std::vector<std::string> names(count);
	std::vector<const char*> nameCStrs(count);

	for (DWORD i = 0; i < count; i++) {
		const aiMaterial* mat = scene->mMaterials[i];
		std::string derived;
		if (DeriveMC2TextureName(scene, mat, derived)) {
			names[i] = derived;
			ASSIMP_TRACE("  material %lu base-color tex -> '%s'", (unsigned long)i, names[i].c_str());
		} else {
			names[i] = "NULLTXM";
			ASSIMP_TRACE("  material %lu has no base-color image -> NULLTXM", (unsigned long)i);
		}
		nameCStrs[i] = names[i].c_str();
	}

	// alphas=NULL → all false; engine's MC_TextureManager toggles textureAlpha
	// later via SetTextureAlpha when the actual TGA loads.
	out->SetImportedTextures(count, nameCStrs.data(), NULL);
}

//-----------------------------------------------------------------------------
// Populate one TG_TypeShape from one aiMesh. Allocates vertex/triangle buffers
// from TG_Shape::tglHeap, writes them, then transfers ownership via the
// narrow construction API on TG_TypeShape.
//
// Returns 0 on success, -1 on failure.
long ImportShapeFromMesh(const aiScene* scene, unsigned meshIdx,
                         TG_TypeShape* outShape, TG_TypeMultiShape* outMulti,
                         SkelBake* bake = nullptr) {
	const aiMesh* mesh = scene->mMeshes[meshIdx];
	if (mesh->mNumVertices == 0 || mesh->mNumFaces == 0) {
		// Empty mesh — leave shape inited but do not populate. Engine treats
		// zero-vertex shapes as no-op renders (numVisibleFaces stays 0).
		return 0;
	}

	// BT2018-SKEL-ENGINE-1A: skinned-mech import. Drop damage/debris/UI parts,
	// then per-part decide the bind-pose bake (single bone, rigid).
	bool doMeshBake = false;
	const float* restM = nullptr;     // joint-global rest (row-major) for this part's bone
	aiMatrix4x4 partOffset;           // this part's own inverse-bind
	if (bake && bake->active) {
		if (skelMeshDropped(mesh->mName.C_Str())) {
			++bake->droppedParts;
			return 0;  // leave an empty shape (renders nothing)
		}
		++bake->importedParts;
		if (mesh->mNumBones >= 1) {
			if (mesh->mNumBones == 1) ++bake->partsSingleBone;
			const aiBone* bone = mesh->mBones[0];
			auto it = bake->nameIdx.find(bone->mName.C_Str());
			if (it != bake->nameIdx.end()) {
				restM = bake->rest[it->second].m;
				partOffset = bone->mOffsetMatrix;
				doMeshBake = true;
				++bake->partsOffsetBaked;
			}
		}
	}
	if (mesh->mNormals == NULL) {
		// Should not happen because we pass aiProcess_GenSmoothNormals, but
		// belt-and-braces — the engine's lighting kernel reads .normal.
		PAUSE(("[importer] mesh %u has no normals after Generate pass", meshIdx));
		return -1;
	}

	// Resolve node identity by walking the scene graph for the node that
	// references this mesh. Multi-mesh-per-node is allowed; we just take the
	// first hit (the renderer iterates shapes, not nodes).
	const aiNode* meshNode = FindNodeForMesh(scene->mRootNode, meshIdx);
	const char* nodeNm = (meshNode && meshNode->mName.length > 0)
	                     ? meshNode->mName.C_Str() : "";
	const char* parentNm = "None";
	if (meshNode && meshNode->mParent && meshNode->mParent != scene->mRootNode
	    && meshNode->mParent->mName.length > 0) {
		parentNm = meshNode->mParent->mName.C_Str();
	}

	// Node pivot: decompose the node's local transform; take the translation
	// component, flipped to MC2 space. (Rotation/scale are not propagated to
	// TG_TypeShape — those live in the per-instance TG_ShapeRec / animation
	// channels, which are M2.)
	Stuff::Point3D center;
	center.x = center.y = center.z = 0.0f;
	if (meshNode) {
		aiVector3D pos, scale;
		aiQuaternion rot;
		meshNode->mTransformation.Decompose(scale, rot, pos);
		center = toMC2Pos(pos);
	}

	// Allocate vertex + triangle buffers from the project's master allocator.
	TG_TypeVertexPtr verts = (TG_TypeVertexPtr)TG_Shape::tglHeap->Malloc(
		sizeof(TG_TypeVertex) * mesh->mNumVertices);
	TG_TypeTrianglePtr tris = (TG_TypeTrianglePtr)TG_Shape::tglHeap->Malloc(
		sizeof(TG_TypeTriangle) * mesh->mNumFaces);
	gosASSERT(verts != NULL && tris != NULL);
	memset(verts, 0, sizeof(TG_TypeVertex) * mesh->mNumVertices);
	memset(tris,  0, sizeof(TG_TypeTriangle) * mesh->mNumFaces);

	// Per-vertex: position + normal in MC2 space; aRGBLight init to opaque
	// black (matches ParseASEFile's default — engine's lighting kernel
	// overwrites this every frame anyway).
	for (unsigned v = 0; v < mesh->mNumVertices; v++) {
		if (doMeshBake) {
			// Bind-pose bake: v_world = restGlobal(bone) * (offset_part * v),
			// normal by the same rotation. All in glTF space; toMC2* applies the
			// MC2 axis flip after, identical to the static (pre-baked) GLB path.
			aiVector3D pLocal = partOffset * mesh->mVertices[v];
			verts[v].position = toMC2Pos(applyRowMajor16(restM, pLocal) * bake->scale);
			aiVector3D nLocal = aiMatrix3x3(partOffset) * mesh->mNormals[v];
			aiVector3D nWorld = rotRowMajor16(restM, nLocal);
			nWorld.Normalize();
			verts[v].normal = toMC2Vec(nWorld);
		} else {
			verts[v].position  = toMC2Pos(mesh->mVertices[v]);
			verts[v].normal    = toMC2Vec(mesh->mNormals[v]);
		}
		// Init OPAQUE WHITE, not black. Override render shapes (treeRenderShape/
		// bldgRenderShape) are drawn by the GPU static-prop batcher, which reads
		// a_aRGBLight from the type-level VBO (bdactor.cpp ~2698). The per-vertex
		// CPU lighting bake runs on the STOCK shape (treeShape), never on the
		// imported override shape, so its aRGBLight is never overwritten. Black
		// init => texture * 0 = solid-black tree. White init => the cooked albedo
		// shows (unlit, matching the "draw foliage UNLIT" prior art). When a bake
		// DOES run it overwrites this, so stock-lit shapes are unaffected.
		verts[v].aRGBLight = 0xffffffff;

		// MODEL-OVERRIDE / Track C: vertex-tight bounding box, mesh-local.
		// Accumulate over the SAME positions stored into the vertex buffer (no
		// nodeCenter applied): import sets a zero node pivot, so the renderer
		// draws these verts mesh-local. Baking center in would offset the box
		// from the rendered geometry for any non-zero node translation; the ASE
		// ref (msl.cpp ~300-323) keeps render-space and box-space in agreement.
		// Empty-mesh sentinel + extentRadius floor handled in ComputeBoundingBox.
		if (outMulti) {
			// Mesh-local box: matches the zero node-center render (no center pivot applied).
			const float wx = verts[v].position.x;
			const float wy = verts[v].position.y;
			const float wz = verts[v].position.z;
			if (wx < outMulti->minBox.x) outMulti->minBox.x = wx;
			if (wy < outMulti->minBox.y) outMulti->minBox.y = wy;
			if (wz < outMulti->minBox.z) outMulti->minBox.z = wz;
			if (wx > outMulti->maxBox.x) outMulti->maxBox.x = wx;
			if (wy > outMulti->maxBox.y) outMulti->maxBox.y = wy;
			if (wz > outMulti->maxBox.z) outMulti->maxBox.z = wz;
		}
	}

	// UV channel 0 is the standard diffuse channel. Mechs with UV1+ are out
	// of MVP scope; we drop the extra channels (renderer doesn't read them).
	const bool hasUV = mesh->HasTextureCoords(0);

	for (unsigned f = 0; f < mesh->mNumFaces; f++) {
		const aiFace& face = mesh->mFaces[f];
		// aiProcess_Triangulate guarantees 3-vertex faces; defensive check.
		if (face.mNumIndices != 3) {
			PAUSE(("[importer] mesh %u face %u has %u indices (expected 3)",
			       meshIdx, f, face.mNumIndices));
			TG_Shape::tglHeap->Free(verts);
			TG_Shape::tglHeap->Free(tris);
			return -1;
		}
		TG_TypeTriangle& t = tris[f];
		t.Vertices[0] = face.mIndices[0];
		t.Vertices[1] = face.mIndices[1];
		t.Vertices[2] = face.mIndices[2];

		// Material index → texture slot: 1:1 mapping per BuildTextureList
		// (MVP scope; mech materials don't share atlases).
		t.localTextureHandle = mesh->mMaterialIndex;
		t.renderStateFlags   = 0;  // backface bit 0 = front-facing default

		// Face normal: average the three vertex normals. Same approximation
		// the renderer uses for lighting; sufficient for MVP. Per-face
		// recompute via cross-product would be more accurate but the engine
		// re-uses faceNormal mainly for backface culling, which is sign-only.
		Stuff::Vector3D fn;
		fn.x = verts[face.mIndices[0]].normal.x
		     + verts[face.mIndices[1]].normal.x
		     + verts[face.mIndices[2]].normal.x;
		fn.y = verts[face.mIndices[0]].normal.y
		     + verts[face.mIndices[1]].normal.y
		     + verts[face.mIndices[2]].normal.y;
		fn.z = verts[face.mIndices[0]].normal.z
		     + verts[face.mIndices[1]].normal.z
		     + verts[face.mIndices[2]].normal.z;
		// Don't bother normalising — the renderer normalises on use.
		t.faceNormal = fn;

		if (hasUV) {
			t.uvdata.u0 = mesh->mTextureCoords[0][face.mIndices[0]].x;
			t.uvdata.v0 = toMC2V(mesh->mTextureCoords[0][face.mIndices[0]].y);
			t.uvdata.u1 = mesh->mTextureCoords[0][face.mIndices[1]].x;
			t.uvdata.v1 = toMC2V(mesh->mTextureCoords[0][face.mIndices[1]].y);
			t.uvdata.u2 = mesh->mTextureCoords[0][face.mIndices[2]].x;
			t.uvdata.v2 = toMC2V(mesh->mTextureCoords[0][face.mIndices[2]].y);
		}
	}

	// Hand ownership of the buffers to the shape (no copy).
	outShape->InitFromImportedMesh(nodeNm, parentNm, center,
	                               mesh->mNumVertices, mesh->mNumFaces,
	                               verts, tris);
	return 0;
}

//-----------------------------------------------------------------------------
// BT2018-SKEL-ENGINE-1A: skinned mech -> ONE merged, bind-pose-baked shape.
//
// A skinned GLB has many mesh-parts (69 for the marauder); importing them as
// per-mesh shapes left dropped parts as empty (NULL-vertex) shapes, which the
// mech GPU/recipe path chokes on (crash + no render). The proven-good static
// path is a SINGLE shape, so merge every intact baked part into one combined
// vertex/triangle buffer (per-triangle material index preserved), matching the
// fk_bake static export exactly. Caller has already done AllocateImportedShapes(1)
// + BuildTextureList + ResetBoundingBox.
long PopulateMergedSkinnedShape(const aiScene* scene, TG_TypeMultiShape* out, SkelBake& bake) {
	std::vector<unsigned> parts;
	DWORD totalV = 0, totalT = 0;
	for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
		const aiMesh* me = scene->mMeshes[m];
		if (me->mNumVertices == 0 || me->mNumFaces == 0) continue;
		if (skelMeshDropped(me->mName.C_Str())) { ++bake.droppedParts; continue; }
		if (me->mNumBones == 0) continue;  // skinned scene: skip any unrigged stray
		parts.push_back(m); totalV += me->mNumVertices; totalT += me->mNumFaces;
	}
	if (parts.empty() || totalV == 0) { PAUSE(("[importer] skinned merge: no intact parts")); return -1; }

	TG_TypeNodePtr slot = out->GetTypeNode(0);
	if (!slot || slot->GetNodeType() != SHAPE_NODE) return -1;
	TG_TypeShape* shape = static_cast<TG_TypeShape*>(slot);

	TG_TypeVertexPtr verts = (TG_TypeVertexPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeVertex) * totalV);
	TG_TypeTrianglePtr tris = (TG_TypeTrianglePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeTriangle) * totalT);
	gosASSERT(verts != NULL && tris != NULL);
	memset(verts, 0, sizeof(TG_TypeVertex) * totalV);
	memset(tris, 0, sizeof(TG_TypeTriangle) * totalT);

	DWORD vOff = 0, tOff = 0;
	for (unsigned m : parts) {
		const aiMesh* me = scene->mMeshes[m];
		++bake.importedParts;
		if (me->mNumBones == 1) ++bake.partsSingleBone;
		const aiBone* bone = me->mBones[0];
		auto it = bake.nameIdx.find(bone->mName.C_Str());
		const float* rm = (it != bake.nameIdx.end()) ? bake.rest[it->second].m : nullptr;
		aiMatrix4x4 off = bone->mOffsetMatrix;
		if (rm) ++bake.partsOffsetBaked;
		// 1B-RUNTIME: record this part's layout for the per-frame re-bake. The
		// vOff/vCount range is this part's slice of the merged listOfTypeVertices;
		// boneIndex indexes SkelBake::names (the clip-global array TickImportedMechs
		// re-evaluates each frame). -1 = no bone match (scale-only, as below).
		if (bake.recParts) {
			ImportPartRec pr;
			pr.meshIndex = m;
			pr.boneIndex = (it != bake.nameIdx.end()) ? it->second : -1;
			pr.off       = off;
			pr.vOff      = vOff;
			pr.vCount    = me->mNumVertices;
			bake.recParts->push_back(pr);
		}
		const bool hasUV = me->HasTextureCoords(0);

		for (unsigned v = 0; v < me->mNumVertices; ++v) {
			aiVector3D p, n;
			if (rm) {
				p = applyRowMajor16(rm, off * me->mVertices[v]) * bake.scale;
				n = rotRowMajor16(rm, aiMatrix3x3(off) * me->mNormals[v]);
			} else {
				p = me->mVertices[v] * bake.scale;
				n = me->mNormals[v];
			}
			n.Normalize();
			TG_TypeVertex& vt = verts[vOff + v];
			vt.position  = mechToMC2Pos(p);   // fixed mech axis (== MC2_GLTF_AXIS=2)
			vt.normal    = mechToMC2Vec(n);
			vt.aRGBLight = 0xffffffff;
			if (vt.position.x < out->minBox.x) out->minBox.x = vt.position.x;
			if (vt.position.y < out->minBox.y) out->minBox.y = vt.position.y;
			if (vt.position.z < out->minBox.z) out->minBox.z = vt.position.z;
			if (vt.position.x > out->maxBox.x) out->maxBox.x = vt.position.x;
			if (vt.position.y > out->maxBox.y) out->maxBox.y = vt.position.y;
			if (vt.position.z > out->maxBox.z) out->maxBox.z = vt.position.z;
		}
		for (unsigned f = 0; f < me->mNumFaces; ++f) {
			const aiFace& fc = me->mFaces[f];
			if (fc.mNumIndices != 3) continue;
			TG_TypeTriangle& t = tris[tOff + f];
			t.Vertices[0] = vOff + fc.mIndices[0];
			t.Vertices[1] = vOff + fc.mIndices[1];
			t.Vertices[2] = vOff + fc.mIndices[2];
			// Single skin slot 0 (the base atlas): resetPaintScheme binds the mech
			// skin from texture slot 0, and the GLB's material 0 is a blip atlas.
			t.localTextureHandle = 0;
			t.renderStateFlags = 0;
			Stuff::Vector3D fn;
			fn.x = verts[t.Vertices[0]].normal.x + verts[t.Vertices[1]].normal.x + verts[t.Vertices[2]].normal.x;
			fn.y = verts[t.Vertices[0]].normal.y + verts[t.Vertices[1]].normal.y + verts[t.Vertices[2]].normal.y;
			fn.z = verts[t.Vertices[0]].normal.z + verts[t.Vertices[1]].normal.z + verts[t.Vertices[2]].normal.z;
			t.faceNormal = fn;
			if (hasUV) {
				t.uvdata.u0 = me->mTextureCoords[0][fc.mIndices[0]].x;
				t.uvdata.v0 = toMC2V(me->mTextureCoords[0][fc.mIndices[0]].y);
				t.uvdata.u1 = me->mTextureCoords[0][fc.mIndices[1]].x;
				t.uvdata.v1 = toMC2V(me->mTextureCoords[0][fc.mIndices[1]].y);
				t.uvdata.u2 = me->mTextureCoords[0][fc.mIndices[2]].x;
				t.uvdata.v2 = toMC2V(me->mTextureCoords[0][fc.mIndices[2]].y);
			}
		}
		vOff += me->mNumVertices; tOff += me->mNumFaces;
	}

	Stuff::Point3D zero; zero.x = zero.y = zero.z = 0.0f;
	shape->InitFromImportedMesh("imported_mech", "None", zero, totalV, totalT, verts, tris);
	return 0;
}

//-----------------------------------------------------------------------------
// Multi-shape bounding box (min/max corner + extentRadius). Mirrors what the
// ASE path does in LoadBinaryCopy: vertex-tight over every transformed vertex
// (accumulated in ImportShapeFromMesh in multi-shape-local space), then
// finalized here for extentRadius. Sets `out->maxBox`, `minBox`, `extentRadius`.
//
// Reset the multi-shape box to "empty" extremes before the per-mesh vertex
// accumulation in ImportShapeFromMesh. Call once before the mesh loop.
void ResetBoundingBox(TG_TypeMultiShape* out) {
	out->maxBox.x = out->maxBox.y = out->maxBox.z = -1.0e9f;
	out->minBox.x = out->minBox.y = out->minBox.z =  1.0e9f;
}

// Finalize the multi-shape box after every mesh has expanded min/max over its
// vertices (vertex-tight, see ImportShapeFromMesh). Handles the empty-mesh
// sentinel and computes the bounding-sphere radius.
void ComputeBoundingBox(TG_TypeMultiShape* out) {
	// Sentinel if no vertices contributed (no mesh expanded the box).
	if (out->maxBox.x < out->minBox.x) {
		out->maxBox.x = out->maxBox.y = out->maxBox.z = 0.0f;
		out->minBox.x = out->minBox.y = out->minBox.z = 0.0f;
	}

	const float dx = out->maxBox.x - out->minBox.x;
	const float dy = out->maxBox.y - out->minBox.y;
	const float dz = out->maxBox.z - out->minBox.z;
	out->extentRadius = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
	if (out->extentRadius < 1.0f) out->extentRadius = 1.0f;  // defensive floor
}

} // anonymous namespace

//=============================================================================
// BT2018-SKEL-ENGINE-1B-RUNTIME — per-frame imported-mech animation.
//
// The merge above bakes ONE static pose into the shared TG type geometry. This
// re-poses that same merged `listOfTypeVertices` every frame from a looping clip
// (same FK/offset math, clip global per frame) so the imported mech MOVES. The
// CPU TransformMultiShape then re-reads the type verts into the instance each
// frame. Scope (1B): one forced clip, CPU mech path (MC2_GPU_MECHS=0), shared
// type → actors animate in lockstep. Per-actor + GPU path are later slices.
namespace {

struct ImportedAnimEntry {
    Assimp::Importer*   imp   = nullptr;   // owns scene; session-lifetime (never freed)
    const aiScene*      scene = nullptr;
    TG_TypeShape*       shape = nullptr;   // merged slot-0 shape (listOfTypeVertices)
    TG_TypeMultiShape*  multi = nullptr;   // for per-frame bbox refresh
    std::vector<std::string> names;        // skeleton, parallel to clip globals
    std::string         clip;
    float               scale    = 1.0f;
    float               groundDy = 0.0f;   // MC2-space Y offset applied at import
    float               durationSec = 1.0f;
    std::vector<ImportPartRec> parts;
    float               clipTimeSec = 0.0f;
    unsigned            lastFrame   = 0xFFFFFFFFu;
};
std::vector<ImportedAnimEntry> g_importedAnims;

bool mechImportAnimateEnabled() {
    // Opt-in, default OFF (static FORCE_CLIP bake stays the safe path).
    static int v = [](){ const char* e = getenv("MC2_MECH_IMPORT_ANIMATE");
                         return (e && e[0] == '1') ? 1 : 0; }();
    return v != 0;
}

// Keep a dedicated parse alive for the runtime re-bake: the import's own Importer
// is a stack local that frees its scene on return. Same flags → identical
// post-processed mesh ordering/vertices as the merge that recorded `parts`.
void RegisterImportedAnim(const char* path, TG_TypeMultiShape* out, const SkelBake& bake,
                          const std::vector<ImportPartRec>& parts, float groundDy) {
    TG_TypeNodePtr slot = out->GetTypeNode(0);
    if (!slot || slot->GetNodeType() != SHAPE_NODE) return;
    Assimp::Importer* imp = new Assimp::Importer();
    const aiScene* scene = imp->ReadFile(path,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_JoinIdenticalVertices | aiProcess_ValidateDataStructure | aiProcess_SortByPType);
    if (!scene) { delete imp; return; }
    float durSec = 1.0f;
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* an = scene->mAnimations[a];
        if (bake.forcedClip == an->mName.C_Str()) {
            double tps = an->mTicksPerSecond != 0.0 ? an->mTicksPerSecond : 1000.0;
            durSec = (float)(an->mDuration / tps);
            break;
        }
    }
    ImportedAnimEntry e;
    e.imp = imp; e.scene = scene;
    e.shape = static_cast<TG_TypeShape*>(slot);
    e.multi = out;
    e.names = bake.names;
    e.clip = bake.forcedClip;
    e.scale = bake.scale;
    e.groundDy = groundDy;
    e.durationSec = durSec > 1e-4f ? durSec : 1.0f;
    e.parts = parts;
    g_importedAnims.push_back(std::move(e));
    MECH_SKEL_TRACE("file='%s' 1B-RUNTIME registered clip='%s' dur=%.3fs parts=%zu",
                    path, bake.forcedClip.c_str(), durSec, parts.size());
}

} // namespace

bool mc2mechanim::AnyImportedAnim() { return !g_importedAnims.empty(); }

void mc2mechanim::TickImportedMechs(float dt, unsigned frameStamp) {
    if (g_importedAnims.empty()) return;
    for (ImportedAnimEntry& e : g_importedAnims) {
        if (e.lastFrame == frameStamp) continue;   // once per frame (double-update safe)
        e.lastFrame = frameStamp;
        e.clipTimeSec += dt;
        if (e.durationSec > 0.0f)
            while (e.clipTimeSec >= e.durationSec) e.clipTimeSec -= e.durationSec;
        const float frame = e.clipTimeSec * 30.0f;   // EvaluateClipGpuBones: t = frame/30*tps
        std::vector<mc2skel::GpuBone> globals; double tt = 0, dd = 0;
        if (!mc2skel::EvaluateClipGpuBones(e.scene, e.clip, frame, e.names, globals, &tt, &dd))
            continue;
        TG_TypeVertex* vt = e.shape->GetTypeVerticesMutable();
        if (!vt) continue;
        ResetBoundingBox(e.multi);
        for (const ImportPartRec& pr : e.parts) {
            const aiMesh* me = e.scene->mMeshes[pr.meshIndex];
            const float* rm = (pr.boneIndex >= 0 && pr.boneIndex < (int)globals.size())
                                  ? globals[pr.boneIndex].m : nullptr;
            for (unsigned v = 0; v < pr.vCount && v < me->mNumVertices; ++v) {
                aiVector3D p, n;
                if (rm) {
                    p = applyRowMajor16(rm, pr.off * me->mVertices[v]) * e.scale;
                    n = rotRowMajor16(rm, aiMatrix3x3(pr.off) * me->mNormals[v]);
                } else {
                    p = me->mVertices[v] * e.scale;
                    n = me->mNormals[v];
                }
                n.Normalize();
                TG_TypeVertex& d = vt[pr.vOff + v];
                d.position   = mechToMC2Pos(p);
                d.position.y += e.groundDy;          // preserve the import-grounded offset
                d.normal     = mechToMC2Vec(n);
                if (d.position.x < e.multi->minBox.x) e.multi->minBox.x = d.position.x;
                if (d.position.y < e.multi->minBox.y) e.multi->minBox.y = d.position.y;
                if (d.position.z < e.multi->minBox.z) e.multi->minBox.z = d.position.z;
                if (d.position.x > e.multi->maxBox.x) e.multi->maxBox.x = d.position.x;
                if (d.position.y > e.multi->maxBox.y) e.multi->maxBox.y = d.position.y;
                if (d.position.z > e.multi->maxBox.z) e.multi->maxBox.z = d.position.z;
            }
        }
        ComputeBoundingBox(e.multi);
    }
}

//=============================================================================
// Public entry point.
long ImportGeometryFromFile(const char* path, TG_TypeMultiShape* out, bool autoGround) {
	if (!path || !out) return -1;

	ASSIMP_TRACE("ImportGeometryFromFile path='%s'", path);

	Assimp::Importer imp;
	ASSIMP_TRACE("  calling Assimp::Importer::ReadFile...");
	const aiScene* scene = imp.ReadFile(path,
		aiProcess_Triangulate           |
		aiProcess_GenSmoothNormals      |
		aiProcess_JoinIdenticalVertices |
		aiProcess_ValidateDataStructure |
		aiProcess_SortByPType);
	ASSIMP_TRACE("  ReadFile returned scene=%p", (const void*)scene);

	if (!scene) {
		ASSIMP_TRACE("  ERROR: %s", imp.GetErrorString());
		PAUSE(("[importer] %s: Assimp ReadFile failed: %s",
		       path, imp.GetErrorString()));
		return -1;
	}

	ASSIMP_TRACE("  scene meshes=%u materials=%u animations=%u",
	             scene->mNumMeshes, scene->mNumMaterials, scene->mNumAnimations);

	if (ValidateScene(scene, path) != 0) {
		ASSIMP_TRACE("  ValidateScene rejected");
		return -1;
	}

	// BT2018-SKEL-ENGINE-1A: if the scene is skinned (a mech with a skeleton),
	// build the shared skeleton + rest-pose joint globals so each mesh-part can
	// be baked to the assembled bind pose below. Static GLB/props (no bones) skip
	// this entirely -> unchanged import path.
	SkelBake bake;
	// 1B-RUNTIME state (used only on the skinned mech path).
	std::vector<ImportPartRec> animParts;
	bool animateImport = false;
	float groundDyApplied = 0.0f;
	bool sceneHasBones = false;
	for (unsigned m = 0; m < scene->mNumMeshes; ++m)
		if (scene->mMeshes[m]->mNumBones > 0) { sceneHasBones = true; break; }
	if (sceneHasBones) {
		mc2skel::BuildSkeleton(scene, bake.names, bake.parents, bake.invBind);
		// BT2018-SKEL-ENGINE-1B: by default bake the REST (bind) pose. If
		// MC2_MECH_IMPORT_FORCE_CLIP=<clipName> is set (+ optional
		// MC2_MECH_IMPORT_FORCE_FRAME=<n>, default 0), bake that clip frame's pose
		// instead — a STATIC posed import (no per-frame runtime yet). Same joint-
		// global path (mc2skel::EvaluateClipGpuBones) the harness oracle uses, so
		// mech_bone_parity --clip/--frame proves they match. Unknown clip -> rest.
		if (const char* fc = getenv("MC2_MECH_IMPORT_FORCE_CLIP")) {
			int ff = 0;
			if (const char* fe = getenv("MC2_MECH_IMPORT_FORCE_FRAME")) ff = atoi(fe);
			double bt = 0, bd = 0;
			if (mc2skel::EvaluateClipGpuBones(scene, fc, (float)ff, bake.names, bake.rest, &bt, &bd)) {
				bake.forcedClip = fc;
				bake.forcedFrame = ff;
				MECH_SKEL_TRACE("file='%s' FORCE_CLIP='%s' frame=%d (t=%.1f/%.1f) -> posed bake", path, fc, ff, bt, bd);
			} else {
				mc2skel::EvaluateRestGpuBones(scene, bake.names, bake.rest);
				MECH_SKEL_TRACE("file='%s' FORCE_CLIP='%s' NOT FOUND -> rest pose", path, fc);
			}
		} else {
			mc2skel::EvaluateRestGpuBones(scene, bake.names, bake.rest);
		}
		for (size_t i = 0; i < bake.names.size(); ++i) bake.nameIdx[bake.names[i]] = (int)i;
		bake.active = !bake.names.empty();
		// 1B-RUNTIME: animate only with a valid forced clip (it names the loop) and
		// the opt-in gate. Otherwise the static bind/posed bake stands.
		animateImport = mechImportAnimateEnabled() && !bake.forcedClip.empty();

		// Auto-scale the assembled bind pose to MC2 mech size: the skinned GLB is
		// authored at Unity scale (~0.15u tall); stock-equivalent mechs are ~25u.
		// Measure the intact bind-pose extent (glTF space) and scale to target
		// height (matches the Python fk_bake --target_height). Override via
		// MC2_MECH_SKEL_HEIGHT. Single-pass measure over intact, bone-bound meshes.
		if (bake.active) {
			float targetH = 50.0f;  // confirmed in-game (marauder); override MC2_MECH_SKEL_HEIGHT
			if (const char* e = getenv("MC2_MECH_SKEL_HEIGHT")) targetH = (float)atof(e);
			float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
			for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
				const aiMesh* me = scene->mMeshes[m];
				if (me->mNumBones == 0 || skelMeshDropped(me->mName.C_Str())) continue;
				auto it = bake.nameIdx.find(me->mBones[0]->mName.C_Str());
				if (it == bake.nameIdx.end()) continue;
				const float* rm = bake.rest[it->second].m;
				const aiMatrix4x4& off = me->mBones[0]->mOffsetMatrix;
				for (unsigned v = 0; v < me->mNumVertices; ++v) {
					aiVector3D p = applyRowMajor16(rm, off * me->mVertices[v]);
					float c[3] = {p.x, p.y, p.z};
					for (int k = 0; k < 3; ++k) { if (c[k] < lo[k]) lo[k] = c[k]; if (c[k] > hi[k]) hi[k] = c[k]; }
				}
			}
			float ext = 0.0f;
			for (int k = 0; k < 3; ++k) ext = (hi[k] - lo[k] > ext) ? (hi[k] - lo[k]) : ext;
			if (ext > 1e-6f) bake.scale = targetH / ext;
			MECH_SKEL_TRACE("file='%s' SKINNED: bones=%zu bindExtent=%.4f scale=%.2f -> targetH=%.1f",
			                path, bake.names.size(), ext, bake.scale, targetH);
		}

		// MECH-BONE-PARITY-GATE-1: dump the engine's REST joint-global bone
		// matrices (the same mc2skel output the bake consumes) as JSON when
		// MC2_MECH_SKEL_BONE_DUMP=<path> is set. Same shape as the harness
		// `gpu-bones --rest` output, so a diff script can prove the engine and the
		// CLI oracle compute identical bones (and, in 1B, identical per-frame).
		if (const char* dumpPath = getenv("MC2_MECH_SKEL_BONE_DUMP")) {
			if (FILE* bf = fopen(dumpPath, "w")) {
				double sum = 0.0;
				for (const auto& b : bake.rest) for (int k = 0; k < 16; ++k) sum += b.m[k];
				const char* clipLabel = bake.forcedClip.empty() ? "rest" : bake.forcedClip.c_str();
				fprintf(bf, "{\n  \"clip\": \"%s\",\n  \"frame\": %d,\n  \"boneCount\": %zu,\n  \"checksum\": %.6f,\n  \"bones\": [\n",
				        clipLabel, bake.forcedFrame, bake.names.size(), sum);
				for (size_t i = 0; i < bake.names.size(); ++i) {
					fprintf(bf, "    {\"index\": %zu, \"name\": \"%s\", \"m\": [", i, bake.names[i].c_str());
					for (int k = 0; k < 16; ++k) fprintf(bf, "%s%.6f", k ? ", " : "", bake.rest[i].m[k]);
					fprintf(bf, "]}%s\n", i + 1 < bake.names.size() ? "," : "");
				}
				fprintf(bf, "  ]\n}\n");
				fclose(bf);
				MECH_SKEL_TRACE("bone dump -> %s (boneCount=%zu checksum=%.6f)",
				                dumpPath, bake.names.size(), sum);
			}
		}
	}

	if (bake.active) {
		// SKINNED MECH: merge every intact baked part into ONE shape (the proven
		// static-GLB structure). Per-mesh shapes would leave dropped parts as empty
		// NULL-vertex shapes that crash the mech GPU/recipe path.
		out->AllocateImportedShapes(1);
		// Mech skin = texture SLOT 0 (resetPaintScheme binds slot 0). The skinned
		// GLB lists blip atlases at material 0/1, so build a single-slot list with
		// the BASE atlas (the intact body's material) at slot 0; all merged tris
		// reference slot 0. (Weapons/second-atlas mechs are a later slice.)
		std::string baseTex = "NULLTXM";
		for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
			const aiMesh* me = scene->mMeshes[m];
			if (me->mNumVertices == 0 || me->mNumBones == 0 || skelMeshDropped(me->mName.C_Str())) continue;
			std::string nm;
			if (DeriveMC2TextureName(scene, scene->mMaterials[me->mMaterialIndex], nm)) { baseTex = nm; break; }
		}
		const char* texNames[1] = { baseTex.c_str() };
		out->SetImportedTextures(1, texNames, NULL);
		MECH_SKEL_TRACE("file='%s' mech skin slot0 tex='%s'", path, baseTex.c_str());
		ResetBoundingBox(out);
		// 1B-RUNTIME: when animating, capture each part's layout so the merge can be
		// registered for per-frame re-bake (requires a valid forced clip).
		if (animateImport) bake.recParts = &animParts;
		long r = PopulateMergedSkinnedShape(scene, out, bake);
		if (r != 0) { ASSIMP_TRACE("  PopulateMergedSkinnedShape returned %ld", r); return r; }
	} else {
	// Allocate the multi-shape's listOfTypeShapes[] and per-slot TG_TypeShape
	// instances. One Assimp mesh → one TG_TypeShape (MVP; LODs not embedded).
	ASSIMP_TRACE("  AllocateImportedShapes(%d)", (int)scene->mNumMeshes);
	out->AllocateImportedShapes((int)scene->mNumMeshes);

	// Build the multi-shape's TG_Texture[] before populating shapes — the
	// per-shape TG_TinyTexture wiring (CreateListOfTextures) reads from the
	// multi-shape table.
	ASSIMP_TRACE("  BuildTextureList...");
	BuildTextureList(scene, out);
	ASSIMP_TRACE("  BuildTextureList done");

	// Vertex-tight box: reset to empty extremes, then each ImportShapeFromMesh
	// expands min/max over its vertices; ComputeBoundingBox finalizes below.
	ResetBoundingBox(out);

	// Populate each shape from its mesh.
	for (unsigned i = 0; i < scene->mNumMeshes; i++) {
		ASSIMP_TRACE("  ImportShapeFromMesh i=%u verts=%u faces=%u matIdx=%u",
		             i, scene->mMeshes[i]->mNumVertices, scene->mMeshes[i]->mNumFaces,
		             scene->mMeshes[i]->mMaterialIndex);
		TG_TypeNodePtr slot = out->GetTypeNode((long)i);
		if (!slot || slot->GetNodeType() != SHAPE_NODE)
			continue;
		long r = ImportShapeFromMesh(scene, i, static_cast<TG_TypeShape*>(slot), out,
		                             bake.active ? &bake : nullptr);
		if (r != 0) {
			ASSIMP_TRACE("  ImportShapeFromMesh i=%u returned %ld", i, r);
			return r;
		}
	}
	}  // end else (non-skinned per-mesh path)

	if (bake.active) {
		MECH_SKEL_TRACE("file='%s' bones=%zu imported_parts=%d single_bone=%d "
		                "offset_baked=%d dropped=%d selected_clip=bindpose",
		                path, bake.names.size(), bake.importedParts,
		                bake.partsSingleBone, bake.partsOffsetBaked, bake.droppedParts);
	}

	ASSIMP_TRACE("  ComputeBoundingBox...");
	ComputeBoundingBox(out);

	// AUTO-GROUND: imported override meshes are often centered on their pivot, so
	// the placement (pivot at terrain elevation) puts terrain through the MIDPOINT.
	// GL-up = -stuff.y (canopy at the most-negative y), so the BASE is at the
	// largest stuff.y; translate that to 0 so the base sits on the ground.
	// MC2_GLTF_GROUND=2 grounds the opposite end; =0 disables.
	{
		static const int s_ground = [](){ const char* e=getenv("MC2_GLTF_GROUND"); return e?atoi(e):2; }();
		if (s_ground && autoGround) {
			const float dy = (s_ground==2) ? -out->minBox.y : -out->maxBox.y;
			for (unsigned si=0; si<scene->mNumMeshes; ++si) {
				TG_TypeNodePtr nd = out->GetTypeNode((long)si);
				if (nd && nd->GetNodeType()==SHAPE_NODE)
					static_cast<TG_TypeShape*>(nd)->TranslateTypeVerticesY(dy);
			}
			out->minBox.y += dy; out->maxBox.y += dy;
			groundDyApplied = dy;  // 1B-RUNTIME: re-bake must preserve this offset
		}
	}

	// 1B-RUNTIME: register the merged skinned mech for per-frame re-bake. After
	// auto-ground so the stored offset matches the static pose. Reuses the forced
	// clip as the loop. Keeps its own Assimp scene alive (session lifetime).
	if (animateImport && bake.active && !animParts.empty())
		RegisterImportedAnim(path, out, bake, animParts, groundDyApplied);

	ASSIMP_TRACE("  SUCCESS");
	SPEW(("ASSIMP", "%s: %u meshes, %u materials imported",
	      path, scene->mNumMeshes, scene->mNumMaterials));
	return 0;
}

#endif // ENABLE_ASSIMP_IMPORTER
