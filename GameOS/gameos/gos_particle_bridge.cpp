//==========================================================================//
// File:    gos_particle_bridge.cpp                                          //
// Contents: GameOS-side GL bridge for the GPU particle batcher. Owns the   //
//           SSBO (binding=14), the empty draw VAO, the billboard shader    //
//           program, and a 1x1 white test atlas. Implements all 10 GPU-    //
//           direct bring-up traps per memory/gpu_direct_renderer_bringup_  //
//           checklist.md.                                                  //
//           Plan v5 §5.4 B1 Stage 1' Commit 3.                             //
//===========================================================================//

#include "gos_particle_bridge.h"

#include "particles/spec.h"

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
extern const float* gos_GetTerrainViewportVec4();
extern const float* gos_GetProj2ScreenMat4();

// VAO rebind helper (memory/projectz_overlay_findings.md trap #4) — AMD
// silently drops draws when VAO=0; rebind to a known-non-zero VAO before
// any glDrawArrays in a bridge path.
extern void gos_RendererRebindVAO();

namespace {

GLuint s_ssbo          = 0;   // GpuParticle SSBO at binding=14
GLsizei s_ssboCapacity = 0;   // current GL buffer-data size in records
GLuint s_vao           = 0;   // empty VAO (gl_VertexID-driven draw)
GLuint s_sampler       = 0;   // CLAMP_TO_EDGE + LINEAR (Stage 1' atlas is small)
GLuint s_atlasTex      = 0;   // 1x1 white texture for the Card test effect
const ::glsl_program* s_prog = nullptr;

bool s_initFailed = false;

void ensureInitialized() {
    if (s_initFailed) return;
    if (s_vao != 0 && s_prog != nullptr && s_atlasTex != 0 && s_sampler != 0) {
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
    if (s_atlasTex == 0) {
        // 1x1 white pixel. The Card test effect multiplies this by particle
        // color so the output is just the particle color. Stage 2' replaces
        // this with a real atlas. MAX_LEVEL=0 + textureLod in FS keeps the
        // AMD auto-LOD trap (memory/amd_auto_lod_strict_fail.md) closed even
        // before the mip pyramid would be generated.
        glGenTextures(1, &s_atlasTex);
        glBindTexture(GL_TEXTURE_2D, s_atlasTex);
        const unsigned char white[4] = { 255, 255, 255, 255 };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  0);
        glBindTexture(GL_TEXTURE_2D, 0);
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
    if (s_initFailed || s_prog == nullptr || s_prog->shp_ == 0) return;

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

    // ── Upload particle records to the SSBO ───────────────────────────
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    (GLsizeiptr)(count * sizeof(mc2::particles::GpuParticle)),
                    records);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, /*binding=*/14, s_ssbo);

    // ── Bind our VAO (trap #4: AMD silently drops draws when VAO=0) ──
    glBindVertexArray(s_vao);

    // ── Bind program + uniforms ──────────────────────────────────────
    glUseProgram(s_prog->shp_);
    {
        // terrainMVP is row-major direct-upload (GL_FALSE) per
        // memory/terrain_mvp_gl_false.md.
        const float* mvp = gos_GetTerrainMVPMat4();
        if (mvp) {
            GLint loc = glGetUniformLocation(s_prog->shp_, "terrainMVP");
            if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, mvp);
        }
    }
    {
        // B1 C14: u_terrainViewport — pixel-space scale/offset for the
        // viewport divide step of the 3-step chain.
        const float* vp = gos_GetTerrainViewportVec4();
        if (vp) {
            GLint loc = glGetUniformLocation(s_prog->shp_, "u_terrainViewport");
            if (loc >= 0) glUniform4fv(loc, 1, vp);
        }
    }
    {
        // B1 C14: u_mvp — pixel-space -> GL NDC matrix. GL_TRUE (transpose)
        // matches the static_prop bridge at gos_static_prop_batcher.cpp:3014.
        const float* mm = gos_GetProj2ScreenMat4();
        if (mm) {
            GLint loc = glGetUniformLocation(s_prog->shp_, "u_mvp");
            if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_TRUE, mm);
        }
    }
    {
        GLint loc = glGetUniformLocation(s_prog->shp_, "uAtlas");
        if (loc >= 0) glUniform1i(loc, 0);
    }

    // ── Sampler + atlas on unit 0 (trap #5: sampler inheritance) ─────
    glBindSampler(0, s_sampler);
    glBindTexture(GL_TEXTURE_2D, s_atlasTex);

    // ── Depth + blend state (traps #9 depth, blend reset) ────────────
    // Particle billboards: alpha-blend, depth-test against scene, no write.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // reverse-Z convention (matches water fast path)
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ── The draw: 6 vertices per particle, gl_VertexID-driven ────────
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(count * 6u));

    // ── Restore state ────────────────────────────────────────────────
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
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}
