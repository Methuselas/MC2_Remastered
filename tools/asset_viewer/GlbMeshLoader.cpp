#include "GlbMeshLoader.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cfloat>
namespace {
inline void srcToGl(float sx,float sy,float sz,float& gx,float& gy,float& gz){
    const float mx=-sx,my=sz,mz=sy;          // glTF -> Stuff
    gx=-mx; gy=mz; gz=my;                     // Stuff -> GL
}
std::string baseName(const std::string& p){ size_t s=p.find_last_of("/\\"); return s==std::string::npos?p:p.substr(s+1); }
}
MeshData GlbMeshLoader::load(const std::string& path){
    MeshData out;
    Assimp::Importer imp;
    const aiScene* sc = imp.ReadFile(path,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices);
    if (!sc || (sc->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !sc->mRootNode || sc->mNumMeshes==0){
        out.ok=false; out.error=imp.GetErrorString(); if(out.error.empty()) out.error="no meshes"; return out; }
    float lo[3]={FLT_MAX,FLT_MAX,FLT_MAX}, hi[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};
    for (unsigned mi=0; mi<sc->mNumMeshes; ++mi){
        const aiMesh* m=sc->mMeshes[mi];
        if (!m->mNumVertices || !m->HasFaces()) continue;
        SubMesh smsh;
        if (m->mMaterialIndex < sc->mNumMaterials){
            aiString tex;
            if (sc->mMaterials[m->mMaterialIndex]->GetTexture(aiTextureType_DIFFUSE,0,&tex)==AI_SUCCESS)
                smsh.textureName = baseName(tex.C_Str());
        }
        for (unsigned f=0; f<m->mNumFaces; ++f){
            const aiFace& fc=m->mFaces[f];
            if (fc.mNumIndices!=3) continue;
            for (int c=0;c<3;++c){
                unsigned vi=fc.mIndices[c];
                MeshVertex v{};
                srcToGl(m->mVertices[vi].x,m->mVertices[vi].y,m->mVertices[vi].z, v.px,v.py,v.pz);
                if (m->HasNormals())
                    srcToGl(m->mNormals[vi].x,m->mNormals[vi].y,m->mNormals[vi].z, v.nx,v.ny,v.nz);
                if (m->HasTextureCoords(0)){ v.u=m->mTextureCoords[0][vi].x; v.v=1.0f-m->mTextureCoords[0][vi].y; }
                smsh.verts.push_back(v); smsh.idx.push_back((uint32_t)smsh.idx.size());
                for (int k=0;k<3;++k){ float p=(&v.px)[k]; if(p<lo[k])lo[k]=p; if(p>hi[k])hi[k]=p; }
            }
        }
        if (!smsh.verts.empty()) out.submeshes.push_back(std::move(smsh));
    }
    if (out.submeshes.empty()){ out.ok=false; out.error="no triangulated geometry"; return out; }
    for (int k=0;k<3;++k){ out.bmin[k]=lo[k]; out.bmax[k]=hi[k]; }
    out.ok=true; return out;
}
