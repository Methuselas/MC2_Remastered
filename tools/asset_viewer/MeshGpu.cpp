// tools/asset_viewer/MeshGpu.cpp
// Uploads MeshData to GL and resolves albedo textures via the decoder registry.
#include "MeshGpu.h"
#include "TextureDecoderRegistry.h"
#include <GL/glew.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// Albedo resolution helpers
// ---------------------------------------------------------------------------

// Return the filename stem, lowercased ("A_2CivLiving.tga" -> "a_2civliving").
static std::string stemLower(const std::string& textureName) {
    namespace fs = std::filesystem;
    std::string s = fs::path(textureName).stem().string();
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Try tiers in order: 512, 256, 128. Return first existing path, or "".
static std::string resolveAlbedoPath(const std::string& deployDir, const std::string& stem) {
    static const char* tiers[] = { "512", "256", "128", nullptr };
    for (int i = 0; tiers[i]; ++i) {
        std::string p = deployDir + "/data/tgl/" + tiers[i] + "/" + stem + ".ktx2";
        // Check existence via fopen (avoids <filesystem> exception on invalid paths).
        if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return p; }
    }
    return {};
}

// ---------------------------------------------------------------------------
// MeshGpu::upload
// ---------------------------------------------------------------------------
void MeshGpu::upload(const MeshData& m, const std::string& deployDir) {
    destroy();

    for (const SubMesh& sub : m.submeshes) {
        if (sub.verts.empty()) continue;

        GpuSubMesh gs{};
        gs.indexCount = (int)sub.idx.size();

        // VAO
        glGenVertexArrays(1, &gs.vao);
        glBindVertexArray(gs.vao);

        // VBO — upload interleaved MeshVertex (stride 32)
        glGenBuffers(1, &gs.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gs.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(sub.verts.size() * sizeof(MeshVertex)),
                     sub.verts.data(), GL_STATIC_DRAW);

        // loc0 = position  (offset 0,  3 floats)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                              (void*)offsetof(MeshVertex, px));
        // loc1 = normal    (offset 12, 3 floats)
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                              (void*)offsetof(MeshVertex, nx));
        // loc2 = uv        (offset 24, 2 floats)
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                              (void*)offsetof(MeshVertex, u));

        // EBO
        glGenBuffers(1, &gs.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gs.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(sub.idx.size() * sizeof(uint32_t)),
                     sub.idx.data(), GL_STATIC_DRAW);

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        // Note: do NOT unbind GL_ELEMENT_ARRAY_BUFFER while the VAO is bound or after
        // unbinding VAO — EBO is part of VAO state. We unbound VAO first, so this is
        // a standalone unbind of the EBO from the current context (safe, no-op for state).
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        // Resolve albedo
        if (!sub.textureName.empty()) {
            std::string stem = stemLower(sub.textureName);
            std::string path = resolveAlbedoPath(deployDir, stem);
            if (!path.empty()) {
                DecodedTexture dt = textureDecoderRegistry().load(path);
                if (dt.glTexture) {
                    gs.albedo     = dt.glTexture;
                    gs.ownsAlbedo = dt.ownsGlTexture;
                }
            }
        }

        subs_.push_back(gs);
    }

    // Copy bounds
    for (int i = 0; i < 3; ++i) { bmin_[i] = m.bmin[i]; bmax_[i] = m.bmax[i]; }
}

// ---------------------------------------------------------------------------
// MeshGpu::draw
// ---------------------------------------------------------------------------
void MeshGpu::draw() const {
    for (const GpuSubMesh& gs : subs_) {
        // Bind albedo on unit 0 (or 0 for fallback → shader uses flat white)
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gs.albedo);

        glBindVertexArray(gs.vao);
        glDrawElements(GL_TRIANGLES, gs.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------------------
// MeshGpu::drawLit — draw with per-submesh u_hasAlbedo uniform
// ---------------------------------------------------------------------------
void MeshGpu::drawLit(int uHasAlbedoLoc) const {
    for (const GpuSubMesh& gs : subs_) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gs.albedo);
        if (uHasAlbedoLoc >= 0)
            glUniform1i(uHasAlbedoLoc, gs.albedo != 0 ? 1 : 0);

        glBindVertexArray(gs.vao);
        glDrawElements(GL_TRIANGLES, gs.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------------------
// MeshGpu::destroy
// ---------------------------------------------------------------------------
void MeshGpu::destroy() {
    for (GpuSubMesh& gs : subs_) {
        if (gs.vao) glDeleteVertexArrays(1, &gs.vao);
        if (gs.vbo) glDeleteBuffers(1, &gs.vbo);
        if (gs.ebo) glDeleteBuffers(1, &gs.ebo);
        if (gs.albedo && gs.ownsAlbedo) glDeleteTextures(1, &gs.albedo);
    }
    subs_.clear();
    for (int i = 0; i < 3; ++i) bmin_[i] = bmax_[i] = 0.0f;
}
