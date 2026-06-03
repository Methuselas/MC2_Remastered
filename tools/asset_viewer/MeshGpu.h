// tools/asset_viewer/MeshGpu.h
// Upload a CPU MeshData to GL (VAO/VBO/EBO per submesh + per-submesh albedo).
// No engine headers; pure GL + C++17.
#pragma once
#include "TglMeshLoader.h"
#include <string>
#include <vector>
#include <cstdint>

struct GpuSubMesh {
    unsigned vao = 0, vbo = 0, ebo = 0;
    unsigned albedo = 0;
    int      indexCount = 0;
    bool     ownsAlbedo = false;
};

class MeshGpu {
public:
    // Upload all submeshes from MeshData to GL.
    // Resolves per-submesh albedo: looks for deployDir/data/tgl/{512,256,128}/<stem>.ktx2
    // via textureDecoderRegistry(). Falls back to albedo=0 (shader uses flat white).
    void upload(const MeshData& m, const std::string& deployDir);

    // Bind each submesh's albedo (unit 0) and draw elements.
    // For the basic (no-shader) path.
    void draw() const;

    // Draw with per-submesh u_hasAlbedo uniform (loc passed in).
    // Caller must have a program bound and u_albedo set to unit 0.
    void drawLit(int uHasAlbedoLoc) const;

    // Delete all GL objects.
    void destroy();

    bool valid() const { return !subs_.empty(); }

    const float* bmin() const { return bmin_; }
    const float* bmax() const { return bmax_; }

private:
    std::vector<GpuSubMesh> subs_;
    float bmin_[3]{};
    float bmax_[3]{};
};
