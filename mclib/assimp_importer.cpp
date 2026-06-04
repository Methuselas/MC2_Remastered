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
#include <cstring>
#include <cmath>
#include <cstdio>

#include "msl.h"
#include "tgl.h"

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
inline Stuff::Point3D toMC2Pos(const aiVector3D& v) {
	Stuff::Point3D p;
	p.x = -v.x;
	p.y =  v.y;
	p.z = -v.z;
	return p;
}
inline Stuff::Vector3D toMC2Vec(const aiVector3D& v) {
	Stuff::Vector3D n;
	n.x = -v.x;
	n.y =  v.y;
	n.z = -v.z;
	return n;
}
// UV V-flip (spec §6).
inline float toMC2V(float v) { return 1.0f - v; }

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
		// 24 chars max + null terminator. Spec §5 forbids silent truncation.
		if (len >= TG_NODE_ID) {
			STOP(("[importer] %s: node name '%s' exceeds %d chars (silent truncation would break animation binding)",
			      path, name, TG_NODE_ID - 1));
			return -1;
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
                         TG_TypeShape* outShape, TG_TypeMultiShape* outMulti) {
	const aiMesh* mesh = scene->mMeshes[meshIdx];
	if (mesh->mNumVertices == 0 || mesh->mNumFaces == 0) {
		// Empty mesh — leave shape inited but do not populate. Engine treats
		// zero-vertex shapes as no-op renders (numVisibleFaces stays 0).
		return 0;
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
		verts[v].position  = toMC2Pos(mesh->mVertices[v]);
		verts[v].normal    = toMC2Vec(mesh->mNormals[v]);
		verts[v].aRGBLight = 0xff000000;

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
// Public entry point.
long ImportGeometryFromFile(const char* path, TG_TypeMultiShape* out) {
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
		long r = ImportShapeFromMesh(scene, i, static_cast<TG_TypeShape*>(slot), out);
		if (r != 0) {
			ASSIMP_TRACE("  ImportShapeFromMesh i=%u returned %ld", i, r);
			return r;
		}
	}

	ASSIMP_TRACE("  ComputeBoundingBox...");
	ComputeBoundingBox(out);

	ASSIMP_TRACE("  SUCCESS");
	SPEW(("ASSIMP", "%s: %u meshes, %u materials imported",
	      path, scene->mNumMeshes, scene->mNumMaterials));
	return 0;
}

#endif // ENABLE_ASSIMP_IMPORTER
