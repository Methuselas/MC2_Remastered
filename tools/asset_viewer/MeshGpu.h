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
    // Lowercased stem of the source texture name (no extension, no path).
    // e.g. "a_2civliving" or "a_2civlivingx".
    std::string texStem;
    // True when texStem ends with 'x' — emissive/glow overlay submesh.
    bool isLights = false;
};

class MeshGpu {
public:
    // Upload all submeshes from MeshData to GL.
    // Resolves per-submesh albedo at the preferred tier first, then falls back.
    // preferredTier must be one of {128, 256, 512, 1024}; default 512.
    void upload(const MeshData& m, const std::string& deployDir, int preferredTier = 512);

    // Reload albedo textures at a new resolution tier (hot-swap).
    // VAO/VBO/EBO are untouched; only textures are freed (if owned) and re-resolved.
    void reloadAlbedo(const std::string& deployDir, int preferredTier);

    // Bind each submesh's albedo (unit 0) and draw elements.
    // For the basic (no-shader) path.
    void draw() const;

    // Draw with per-submesh u_hasAlbedo uniform (loc passed in).
    // Caller must have a program bound and u_albedo set to unit 0.
    // showLights: if false, submeshes with isLights==true are skipped entirely.
    void drawLit(int uHasAlbedoLoc, bool showLights = true) const;

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
