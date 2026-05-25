//==========================================================================//
// File:    gos_particle_bridge.cpp                                          //
// Contents: GameOS-side GL bridge for the GPU particle batcher. Owns the   //
//           SSBO (binding=14), the empty draw VAO, the billboard shader    //
//           program, and the per-texture draw loop. Implements all 10 GPU- //
//           direct bring-up traps per memory/gpu_direct_renderer_bringup_  //
//           checklist.md.                                                  //
//           FX-GPU-1 Phase 1: real per-effect texture binding.             //
//===========================================================================//

#include "gos_particle_bridge.h"

#include "particles/spec.h"

#include <gameos.hpp>
#include <GL/glew.h>
#include "utils/shader_builder.h"

#include <cstdio>

// terrainMVP getter — same accessor used by gos_terrain_bridge_renderWaterFast
// at gameos_graphics.cpp:2171. C linkage upstream.
extern const float* gos_GetTerrainMVPMat4();
// B1 C14: 3-step projection chain accessors (mirror static_prop bridge at
// gos_static_prop_batcher.cpp:3010/3013). terrainMVP alone is D3D pixel-
// homog clip; the shader needs viewport + pixel->NDC remap to land in GL
// clip space.
extern const float* gos_GetProj2ScreenMat4();

// VAO rebind helper (memory/projectz_overlay_findings.md trap #4) — AMD
// silently drops draws when VAO=0; rebind to a known-non-zero VAO before
// any glDrawArrays in a bridge path.
extern void gos_RendererRebindVAO();

// P1-1: narrow GL-name resolver declared in GameOS/include/gameos.hpp and
// implemented in GameOS/gameos/gameos_graphics.cpp.  gameos.hpp is already
// included above, so this is informational only.
// unsigned int gos_GetGLTextureName(DWORD handle); — see gameos.hpp

namespace {

GLuint s_ssbo          = 0;   // GpuParticle SSBO at binding=14
GLsizei s_ssboCapacity = 0;   // current GL buffer-data size in records
GLuint s_vao           = 0;   // empty VAO (gl_VertexID-driven draw)
GLuint s_sampler       = 0;   // CLAMP_TO_EDGE + LINEAR
const ::glsl_program* s_prog = nullptr;

bool s_initFailed = false;

// P0-4: Cached uniform locations — populated once in ensureInitialized()
// after the program links. -2 = not yet queried; -1 = not found (GLSL may
// strip unused uniforms); >= 0 = valid location.
GLint s_loc_worldToClipGL = -2;
GLint s_loc_mvp           = -2;
GLint s_loc_uAtlas        = -2;

void ensureInitialized() {
    if (s_initFailed) return;
    if (s_vao != 0 && s_prog != nullptr && s_sampler != 0) {
        return;
    }

    if (s_vao == 0) {
        glGenVertexArrays(1, &s_vao);
    }
    if (s_sampler == 0) {
        glGenSamplers(1, &s_sampler);
        glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (s_prog == nullptr) {
        // SSBO requires GL 4.3 + std430 → "#version 430\n" prefix per
        // CLAUDE.md "Shader #version" rule.
        static const char* kPrefix = "#version 430\n";
        s_prog = ::glsl_program::makeProgram(
            "particle_billboard",
            "shaders/particle_billboard.vert",
            "shaders/particle_billboard.frag",
            kPrefix);
        if (!s_prog || !s_prog->shp_) {
            s_initFailed = true;
            std::fprintf(stderr,
                         "[GPU_PARTICLES v1] event=prog_compile_fail — bridge disabled\n");
            std::fflush(stderr);
            s_prog = nullptr;
            return;
        }
        std::fprintf(stderr,
                     "[GPU_PARTICLES v1] event=prog_compiled prog=%u\n",
                     (unsigned)s_prog->shp_);
        std::fflush(stderr);

        // P0-4: cache uniform locations now that the program is linked.
        s_loc_worldToClipGL = glGetUniformLocation(s_prog->shp_, "u_worldToClipGL");
        s_loc_mvp           = glGetUniformLocation(s_prog->shp_, "u_mvp");
        s_loc_uAtlas        = glGetUniformLocation(s_prog->shp_, "uAtlas");
    }
}

void ensureSsboCapacity(GLsizei needRecords) {
    if (s_ssbo == 0) {
        glGenBuffers(1, &s_ssbo);
    }
    if (needRecords > s_ssboCapacity) {
        // Grow with headroom; Stage 1' budget peaks at 4096 (batcher
        // default). Reallocate via glBufferData (no orphan-on-each-frame).
        GLsizei newCap = (needRecords < 1024) ? 1024 : needRecords;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(newCap * sizeof(mc2::particles::GpuParticle)),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        s_ssboCapacity = newCap;
    }
}

}  // namespace

extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* records,
                                          unsigned int                       count) {
    if (count == 0 || records == nullptr) return;

    ensureInitialized();

    // P0-5: failure log — once only.
    if (s_initFailed) {
        static bool s_failLogEmitted = false;
        if (!s_failLogEmitted) {
            s_failLogEmitted = true;
            std::fprintf(stderr, "[GOSFX_GPU v1] ERROR init_failed\n");
            std::fflush(stderr);
        }
        return;
    }
    if (s_prog == nullptr || s_prog->shp_ == 0) return;

    // ── Collect unique texture handles (no heap allocation) ──────────
    // Linear scan over all records. At most kMaxTexGroups unique handles
    // per flush; any overflow entries are dropped with a one-time warning.
    static const int kMaxTexGroups = 64;
    uint32_t uniqueHandles[kMaxTexGroups];
    int      numUnique = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t h = records[i].atlasIndex;
        bool found = false;
        for (int j = 0; j < numUnique; ++j) {
            if (uniqueHandles[j] == h) { found = true; break; }
        }
        if (!found) {
            if (numUnique < kMaxTexGroups) {
                uniqueHandles[numUnique++] = h;
            } else {
                static bool s_overflowWarned = false;
                if (!s_overflowWarned) {
                    s_overflowWarned = true;
                    std::fprintf(stderr,
                        "[GOSFX_GPU v1] WARN tex_group_overflow max=%d — excess groups dropped\n",
                        kMaxTexGroups);
                    std::fflush(stderr);
                }
            }
        }
    }

    // P1-5: first-call banner — once only.
    {
        static bool s_bannerEmitted = false;
        if (!s_bannerEmitted) {
            s_bannerEmitted = true;
            std::fprintf(stderr,
                         "[GOSFX_GPU v1] enabled=1 sprites=%u draws=%d textures=%d blendMode=straight\n",
                         count, numUnique, numUnique);
            std::fflush(stderr);
        }
    }

    ensureSsboCapacity((GLsizei)count);

    // ── State save (trap #4 VAO, sampler, blend, depth, program) ──────
    GLint savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    GLint savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLint savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB, &savedSrcRGB);
    GLint savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB, &savedDstRGB);
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);
    GLint savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK, &savedDepthMask);
    GLint savedSampler   = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &savedSampler);
    GLint savedActiveTex = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    GLint savedTex2D0    = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D0);
    // P0-2 cull state: bridge must disable GL_CULL_FACE for the draw because
    // the particle shader is double-sided. Restore the caller's state after.
    GLboolean savedCullFace = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);

    // ── Bind our VAO (trap #4: AMD silently drops draws when VAO=0) ──
    glBindVertexArray(s_vao);

    // ── Bind program + uniforms (P0-4: use cached locations) ─────────
    glUseProgram(s_prog->shp_);
    {
        // terrainMVP is row-major direct-upload (GL_FALSE) per
        // memory/terrain_mvp_gl_false.md.
        const float* mvp = gos_GetTerrainMVPMat4();
        if (mvp && s_loc_worldToClipGL >= 0)
            glUniformMatrix4fv(s_loc_worldToClipGL, 1, GL_FALSE, mvp);
    }
    {
        // B1 C14: u_mvp — pixel-space -> GL NDC matrix. GL_TRUE (transpose)
        // matches the static_prop bridge at gos_static_prop_batcher.cpp:3014.
        const float* mm = gos_GetProj2ScreenMat4();
        if (mm && s_loc_mvp >= 0)
            glUniformMatrix4fv(s_loc_mvp, 1, GL_TRUE, mm);
    }
    {
        if (s_loc_uAtlas >= 0) glUniform1i(s_loc_uAtlas, 0);
    }

    // ── Sampler on unit 0 (trap #5: sampler inheritance) ─────────────
    glBindSampler(0, s_sampler);

    // ── Depth + blend state (traps #9 depth, blend reset) ────────────
    // Particle billboards: alpha-blend, depth-test against scene, no write.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // reverse-Z convention (matches water fast path)
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ── Per-texture draw loop ─────────────────────────────────────────
    // P1-3: For each unique texture handle group:
    //   1. Collect this group's records into a temporary SSBO upload.
    //   2. Resolve the gos handle to a GLuint via gos_GetGLTextureName.
    //   3. Bind the resolved texture; skip the group if not resident.
    //   4. Upload only this group's records to SSBO offset 0.
    //   5. Draw 6 vertices per particle (gl_VertexID-driven billboard).
    //
    // Using a static staging buffer (avoids large stack allocation).
    // Groups larger than kStagingMax are truncated; real particle counts
    // per effect are small (< 1024) so this is not a concern in practice.
    static const unsigned kStagingMax = 4096;
    static mc2::particles::GpuParticle stagingBuf[kStagingMax];

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, /*binding=*/14, s_ssbo);

    for (int gi = 0; gi < numUnique; ++gi) {
        const uint32_t gosHandle = uniqueHandles[gi];

        // Resolve handle to GL texture name.
        const GLuint glTex = (GLuint)gos_GetGLTextureName(gosHandle);
        if (glTex == 0) {
            std::fprintf(stderr,
                "[GOSFX_GPU v1] ERROR missing_texture handle=%u\n", gosHandle);
            std::fflush(stderr);
            continue;
        }

        // Collect records for this handle into the staging buffer.
        unsigned groupCount = 0;
        for (unsigned i = 0; i < count && groupCount < kStagingMax; ++i) {
            if (records[i].atlasIndex == gosHandle) {
                stagingBuf[groupCount++] = records[i];
            }
        }
        if (groupCount == 0) continue;

        // Upload this group's records to SSBO offset 0.
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        (GLsizeiptr)(groupCount * sizeof(mc2::particles::GpuParticle)),
                        stagingBuf);

        // Bind the resolved texture (state restore covers GL_TEXTURE_BINDING_2D
        // on unit 0 since savedTex2D0 was captured before this loop).
        glBindTexture(GL_TEXTURE_2D, glTex);

        // Draw: 6 vertices per particle, gl_VertexID-driven.
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(groupCount * 6u));
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // ── Restore state ────────────────────────────────────────────────
    if (savedCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D0);
    glBindSampler(0, (GLuint)savedSampler);
    if (savedActiveTex != GL_TEXTURE0) glActiveTexture((GLenum)savedActiveTex);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    if (!savedBlend) glDisable(GL_BLEND);
    glUseProgram((GLuint)savedProgram);
    glBindVertexArray((GLuint)savedVAO);
}
