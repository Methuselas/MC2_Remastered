//==========================================================================//
// File:    gos_particle_bridge.cpp                                          //
// Contents: GameOS-side GL bridge for the GPU particle batcher. Owns the   //
//           SSBO (binding=14), the empty draw VAO, the billboard shader    //
//           program, and the per-group draw loop. Implements all 10 GPU-   //
//           direct bring-up traps per memory/gpu_direct_renderer_bringup_  //
//           checklist.md.                                                  //
//           FX-GPU-1 Phase 1: real per-effect texture binding.             //
//           FX-GPU-1 Phase 2: per-group UV sub-rect uniforms.              //
//===========================================================================//

#include "gos_particle_bridge.h"

#include "particles/spec.h"
#include "particles/batcher.h"  // GroupInfo

#include <gameos.hpp>
#include <GL/glew.h>
#include "utils/shader_builder.h"
#include "gos_postprocess.h"  // VFX-SOFT-PARTICLES-MVP-1: scene-depth copy + invViewProj
#include "../../mclib/camera.h"  // VFX-LIT-PARTICLES-MVP-1: eye->light*/ambient* (same source as terrain)

#include <cstdio>
#include <cstdlib>   // std::getenv, std::atoi (MC2_VFX_DEBUG_MODE)
#include <unordered_set>

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

// B2 P1: active camera basis — set by GameCamera::render() before flush.
// Defaults to identity (right=+X, up=+Y) so particles still appear if the
// caller forgets to call gos_SetActiveCamera (produces east-up orientation,
// same as the pre-B2 fixed-axis behaviour).
float g_cam_right[3]        = {1.0f, 0.0f, 0.0f};
float g_cam_up[3]           = {0.0f, 1.0f, 0.0f};
bool  g_cam_set_this_frame  = false;

GLuint s_ssbo          = 0;   // GpuParticle SSBO at binding=14
GLsizei s_ssboCapacity = 0;   // current GL buffer-data size in records
GLuint s_vao           = 0;   // empty VAO (gl_VertexID-driven draw)
GLuint s_sampler       = 0;   // CLAMP_TO_EDGE + LINEAR
const ::glsl_program* s_prog = nullptr;

bool s_initFailed = false;

// Lazy-init env gate for verbose group/texture diagnostics.
// MC2_GOSFX_GROUP_LOG=1 enables: UV rect dump (first flush) + missing-texture errors.
// Normal runs see only the first-flush banner; set this flag for texture debug sessions.
bool s_groupLog_initialized = false;
bool s_groupLog_value       = false;
bool groupLogEnabled() {
    if (!s_groupLog_initialized) {
        const char* v = std::getenv("MC2_GOSFX_GROUP_LOG");
        s_groupLog_value       = (v && v[0] == '1');
        s_groupLog_initialized = true;
    }
    return s_groupLog_value;
}

// VFX-DEBUG-VIEWS-1: particle billboard debug-mode selector.
// 0=Final (byte-identical default), 1=Albedo, 2=Alpha, 3=ParticleKind,
// 4=Overdraw proxy. Seeded once from MC2_VFX_DEBUG_MODE (clamped 0..4);
// diagnostic-only, no gameplay/emission/lifetime effect. Read-only getter
// gos_vfx_getDebugMode() surfaces the active value in the Object Inspector.
bool s_debugMode_initialized = false;
int  s_debugMode_value       = 0;
int  vfxDebugMode() {
    if (!s_debugMode_initialized) {
        const char* v = std::getenv("MC2_VFX_DEBUG_MODE");
        if (v && v[0] != '\0') {
            int m = std::atoi(v);
            if (m >= 0 && m <= 4) s_debugMode_value = m;
        }
        s_debugMode_initialized = true;
    }
    return s_debugMode_value;
}

// VFX-TUNING-UI-1: user intensity scales. All default 1.0 (= byte-identical
// no-op). Seeded once from MC2_TUNE_VFX_* (clamped 0..8); the Graphics Options
// "VFX Tuning" sliders override at runtime via the setters below. These tune
// LOOK only — no emission/lifetime/sorting/timing change.
bool  s_vfxTune_initialized   = false;
float s_vfxBrightness         = 1.0f;
float s_vfxAdditiveBrightness = 1.0f;
float s_vfxAlphaScale         = 1.0f;
static float clampVfxScale(float v) { return v < 0.0f ? 0.0f : (v > 8.0f ? 8.0f : v); }
static float envVfxScale(const char* name, float dflt) {
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0') return dflt;
    return clampVfxScale((float)std::atof(v));
}
void vfxTuneInitIfNeeded() {
    if (s_vfxTune_initialized) return;
    s_vfxBrightness         = envVfxScale("MC2_TUNE_VFX_BRIGHTNESS",          1.0f);
    s_vfxAdditiveBrightness = envVfxScale("MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS", 1.0f);
    s_vfxAlphaScale         = envVfxScale("MC2_TUNE_VFX_ALPHA_SCALE",         1.0f);
    s_vfxTune_initialized   = true;
}

// VFX-SOFT-PARTICLES-MVP-1: depth-fade enable + world-unit fade band. Gate
// MC2_VFX_SOFT_PARTICLES (default OFF -> byte-identical). When ON, the flush
// snapshots scene depth and the FS softens alpha where alpha particles meet
// opaque geometry. Distance is ImGui-tunable (gos_vfx_setSoftDistance); enable
// is ImGui-toggleable (gos_vfx_setSoftEnabled). No emission/lifetime/timing
// effect; alpha groups only.
bool  s_soft_initialized = false;
bool  s_soft_enabled     = false;
float s_softDistance     = 30.0f;   // world-unit fade band
static float clampSoftDist(float v) { return v < 0.0f ? 0.0f : (v > 500.0f ? 500.0f : v); }
void vfxSoftInitIfNeeded() {
    if (s_soft_initialized) return;
    const char* v = std::getenv("MC2_VFX_SOFT_PARTICLES");
    s_soft_enabled       = (v && v[0] == '1');
    s_soft_initialized   = true;
}

// VFX-LIT-PARTICLES-MVP-1: scene-lit alpha smoke/dust. Gate MC2_VFX_LIT_PARTICLES
// (default OFF -> byte-identical). Strength = startup default MC2_TUNE_VFX_LIT_
// STRENGTH (clamped 0..1) / per-mission "vfxLitStrength" profile key / ImGui
// slider. When OFF the bridge uploads strength 0 so the FS lit branch is inert.
// Alpha groups only; additive flashes stay emissive.
bool  s_lit_initialized = false;
bool  s_lit_enabled     = false;
float s_litStrength     = 0.7f;   // applied only while the gate is ON
static float clampLit(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
void vfxLitInitIfNeeded() {
    if (s_lit_initialized) return;
    const char* en = std::getenv("MC2_VFX_LIT_PARTICLES");
    s_lit_enabled  = (en && en[0] == '1');
    const char* st = std::getenv("MC2_TUNE_VFX_LIT_STRENGTH");
    if (st && st[0] != '\0') s_litStrength = clampLit((float)std::atof(st));
    s_lit_initialized = true;
}

// P0-4: Cached uniform locations — populated once in ensureInitialized()
// after the program links. -2 = not yet queried; -1 = not found (GLSL may
// strip unused uniforms); >= 0 = valid location.
GLint s_loc_worldToClipGL = -2;
GLint s_loc_mvp           = -2;
GLint s_loc_uAtlas        = -2;
// P2-1: UV sub-rect uniforms — set per draw group.
GLint s_loc_uvOffset      = -2;
GLint s_loc_uvSize        = -2;
// VFX-FLIPBOOK-ASSET-TABLE-1: atlas column count — set per draw group.
// 0 or 1 = non-animated (shader skips per-particle frame-offset path).
// >1 = animated; shader computes col=atlasIndex%columns, row=atlasIndex/columns.
GLint s_loc_atlasColumns  = -2;
// B2 P1: camera-basis uniforms — looked up once, bound per flush.
GLint s_loc_cameraRight   = -2;
GLint s_loc_cameraUp      = -2;
// VFX-DEBUG-VIEWS-1: particle debug-mode uniform.
GLint s_loc_debugMode     = -2;
// VFX-TUNING-UI-1: user intensity-scale uniforms.
GLint s_loc_vfxBrightness         = -2;
GLint s_loc_vfxAdditiveBrightness = -2;
GLint s_loc_vfxAlphaScale         = -2;
GLint s_loc_vfxIsAdditive         = -2;
// VFX-SOFT-PARTICLES-MVP-1: soft-particle depth-fade uniforms.
GLint s_loc_uSceneDepth     = -2;
GLint s_loc_invWorldToClip  = -2;
GLint s_loc_screenSize      = -2;
GLint s_loc_softDistance    = -2;
// VFX-LIT-PARTICLES-MVP-1: scene-lighting uniforms.
GLint s_loc_vfxLitStrength  = -2;
GLint s_loc_vfxSunColor     = -2;
GLint s_loc_vfxAmbientColor = -2;

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
        // P2-1: UV sub-rect uniforms.
        s_loc_uvOffset      = glGetUniformLocation(s_prog->shp_, "u_uvOffset");
        s_loc_uvSize        = glGetUniformLocation(s_prog->shp_, "u_uvSize");
        // VFX-FLIPBOOK-ASSET-TABLE-1: atlas column count per draw group.
        s_loc_atlasColumns  = glGetUniformLocation(s_prog->shp_, "u_atlasColumns");
        // B2 P1: camera-basis uniforms.
        // A -1 is a legitimate "not in the program" result (driver stripped a
        // uniform that's unused after dead-code elim). Do NOT retry the lookup
        // every frame — that hides shader bugs and wastes GL calls.
        s_loc_cameraRight   = glGetUniformLocation(s_prog->shp_, "u_cameraRight");
        s_loc_cameraUp      = glGetUniformLocation(s_prog->shp_, "u_cameraUp");
        // VFX-DEBUG-VIEWS-1: debug-mode selector (may be -1 if mode 0 dead-code
        // elim strips it; upload is guarded on >= 0).
        s_loc_debugMode     = glGetUniformLocation(s_prog->shp_, "u_debugMode");
        // VFX-TUNING-UI-1: intensity-scale uniforms.
        s_loc_vfxBrightness         = glGetUniformLocation(s_prog->shp_, "u_vfxBrightness");
        s_loc_vfxAdditiveBrightness = glGetUniformLocation(s_prog->shp_, "u_vfxAdditiveBrightness");
        s_loc_vfxAlphaScale         = glGetUniformLocation(s_prog->shp_, "u_vfxAlphaScale");
        s_loc_vfxIsAdditive         = glGetUniformLocation(s_prog->shp_, "u_vfxIsAdditive");
        // VFX-SOFT-PARTICLES-MVP-1: soft-particle depth-fade uniforms (may be
        // -1 when MC2_VFX_SOFT_PARTICLES is OFF and dead-code elim strips them).
        s_loc_uSceneDepth    = glGetUniformLocation(s_prog->shp_, "u_sceneDepth");
        s_loc_invWorldToClip = glGetUniformLocation(s_prog->shp_, "u_invWorldToClip");
        s_loc_screenSize     = glGetUniformLocation(s_prog->shp_, "u_screenSize");
        s_loc_softDistance   = glGetUniformLocation(s_prog->shp_, "u_softDistance");
        // VFX-LIT-PARTICLES-MVP-1: scene-lighting uniforms (may be -1 when the
        // gate is OFF and dead-code elim strips them).
        s_loc_vfxLitStrength  = glGetUniformLocation(s_prog->shp_, "u_vfxLitStrength");
        s_loc_vfxSunColor     = glGetUniformLocation(s_prog->shp_, "u_vfxSunColor");
        s_loc_vfxAmbientColor = glGetUniformLocation(s_prog->shp_, "u_vfxAmbientColor");
        if (s_loc_cameraRight < 0 || s_loc_cameraUp < 0) {
            if (groupLogEnabled())
                std::fprintf(stderr, "[B2] gos_particle_bridge: uniform locations missing — right=%d up=%d\n",
                             s_loc_cameraRight, s_loc_cameraUp);
        }
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

extern "C" void gos_SetActiveCamera(const float right_xyz[3], const float up_xyz[3])
{
    for (int i = 0; i < 3; ++i) {
        g_cam_right[i] = right_xyz[i];
        g_cam_up[i]    = up_xyz[i];
    }
    g_cam_set_this_frame = true;
}

extern "C" void gos_GetCameraRight(float out_xyz[3])
{
    for (int i = 0; i < 3; ++i) out_xyz[i] = g_cam_right[i];
}

extern "C" void gos_GetCameraUp(float out_xyz[3])
{
    for (int i = 0; i < 3; ++i) out_xyz[i] = g_cam_up[i];
}

extern "C" void gos_ClearActiveCamera(void)
{
    g_cam_set_this_frame = false;
}

extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* records,
                                          unsigned int                       count,
                                          const mc2::particles::GroupInfo*   groups,
                                          unsigned int                       numGroups) {
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

    // P1-5: first-call banner — once only.
    {
        static bool s_bannerEmitted = false;
        if (!s_bannerEmitted) {
            s_bannerEmitted = true;
            std::fprintf(stderr,
                         "[GOSFX_GPU v1] enabled=1 sprites=%u draws=%u textures=%u blendMode=straight\n",
                         count, numGroups, numGroups);
            std::fflush(stderr);
        }
    }

    // P2-3: per-group UV debug log on first flush — gated behind MC2_GOSFX_GROUP_LOG=1.
    // Shows UV rects and blend modes being propagated from spawn through to the bridge.
    if (groupLogEnabled()) {
        static bool s_uvDumpDone = false;
        if (!s_uvDumpDone && numGroups > 0) {
            s_uvDumpDone = true;
            for (unsigned gi = 0; gi < numGroups; ++gi) {
                const mc2::particles::GroupInfo& g = groups[gi];
                std::fprintf(stderr,
                    "[GOSFX_GPU v1] group %u: tex=%u uv=(%.2f,%.2f)+(%.2f,%.2f) count=%u blend=%s\n",
                    gi, g.handle, g.u0, g.v0, g.us, g.vs, g.count,
                    g.blendMode == 1 ? "additive" : "alpha");
            }
            std::fflush(stderr);
        }
    }

    // B2 P1: warn once if the caller never called gos_SetActiveCamera this frame.
    // Gated behind MC2_GOSFX_GROUP_LOG=1 to avoid log noise in normal runs.
    if (!g_cam_set_this_frame) {
        static bool warned = false;
        if (!warned) {
            if (groupLogEnabled())
                std::fprintf(stderr, "[B2] gos_particle_bridge: flush without gos_SetActiveCamera; using last-known basis\n");
            warned = true;
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
    // B2 P1: bind camera basis uniforms. g_cam_right/up are already in GL
    // world space (axis-swapped by gamecam.cpp before calling gos_SetActiveCamera).
    if (s_loc_cameraRight >= 0) glUniform3fv(s_loc_cameraRight, 1, g_cam_right);
    if (s_loc_cameraUp    >= 0) glUniform3fv(s_loc_cameraUp,    1, g_cam_up);
    // VFX-DEBUG-VIEWS-1: debug-mode selector (default 0 = byte-identical Final).
    if (s_loc_debugMode   >= 0) glUniform1i(s_loc_debugMode, vfxDebugMode());
    // VFX-TUNING-UI-1: per-flush intensity scales (defaults 1.0 = no-op).
    vfxTuneInitIfNeeded();
    if (s_loc_vfxBrightness         >= 0) glUniform1f(s_loc_vfxBrightness,         s_vfxBrightness);
    if (s_loc_vfxAdditiveBrightness >= 0) glUniform1f(s_loc_vfxAdditiveBrightness, s_vfxAdditiveBrightness);
    if (s_loc_vfxAlphaScale         >= 0) glUniform1f(s_loc_vfxAlphaScale,         s_vfxAlphaScale);
    // u_vfxIsAdditive defaults to alpha (0); set per-group in the draw loop.
    if (s_loc_vfxIsAdditive         >= 0) glUniform1i(s_loc_vfxIsAdditive, 0);

    // ── VFX-SOFT-PARTICLES-MVP-1: depth-fade setup ───────────────────
    // When the gate is OFF we upload u_softDistance=0 so the FS fade branch is
    // skipped -> byte-identical. When ON, snapshot scene depth (avoids the
    // FBO feedback loop), bind the copy on unit 1, and upload the inverse of
    // the SAME matrix the VS projects with (gosPostProcess::inverseViewProj_).
    GLint savedTex2D1 = 0;
    bool  softActive  = false;
    float softDist    = 0.0f;
    vfxSoftInitIfNeeded();
    if (s_soft_enabled && s_loc_softDistance >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        if (pp) {
            pp->copySceneDepthForParticles();
            const GLuint depthCopy = pp->getSceneDepthCopyTexture();
            if (depthCopy != 0) {
                softActive = true;
                softDist   = (s_softDistance > 0.0f) ? s_softDistance : 0.0f;
                glActiveTexture(GL_TEXTURE1);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D1);
                glBindTexture(GL_TEXTURE_2D, depthCopy);
                glBindSampler(1, 0);  // use the texture's own NEAREST params
                glActiveTexture(GL_TEXTURE0);
                if (s_loc_uSceneDepth    >= 0) glUniform1i(s_loc_uSceneDepth, 1);
                if (s_loc_invWorldToClip >= 0)
                    glUniformMatrix4fv(s_loc_invWorldToClip, 1, GL_FALSE, pp->getInverseViewProj());
                if (s_loc_screenSize     >= 0)
                    glUniform2f(s_loc_screenSize, (float)pp->getWidth(), (float)pp->getHeight());
            }
        }
    }
    if (s_loc_softDistance >= 0) glUniform1f(s_loc_softDistance, softDist);

    // ── VFX-LIT-PARTICLES-MVP-1: scene lighting for alpha groups ─────
    // When OFF, upload strength 0 -> FS lit branch inert -> byte-identical.
    // Sun/ambient come from the global camera (eye), the same source terrain
    // consumes (gos_terrain_lighting.cpp); 0..255 -> 0..1.
    vfxLitInitIfNeeded();
    {
        float litStrength = (s_lit_enabled && eye) ? s_litStrength : 0.0f;
        if (s_loc_vfxLitStrength >= 0) glUniform1f(s_loc_vfxLitStrength, litStrength);
        if (litStrength > 0.0f) {
            const float inv255 = 1.0f / 255.0f;
            if (s_loc_vfxSunColor >= 0)
                glUniform3f(s_loc_vfxSunColor,
                            (float)eye->lightRed   * inv255,
                            (float)eye->lightGreen * inv255,
                            (float)eye->lightBlue  * inv255);
            if (s_loc_vfxAmbientColor >= 0)
                glUniform3f(s_loc_vfxAmbientColor,
                            (float)eye->ambientRed   * inv255,
                            (float)eye->ambientGreen * inv255,
                            (float)eye->ambientBlue  * inv255);
        }
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

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, /*binding=*/14, s_ssbo);

    if (numGroups == 0 || groups == nullptr) {
        // Fallback: no group metadata — treat entire buffer as one group,
        // full UV rect, handle from first record. Should not occur after
        // Phase 2 callers always call BeginGroup; kept for robustness.
        const uint32_t gosHandle = records[0].atlasIndex;
        const GLuint   glTex     = (GLuint)gos_GetGLTextureName(gosHandle);
        if (glTex != 0) {
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                            (GLsizeiptr)(count * sizeof(mc2::particles::GpuParticle)),
                            records);
            glBindTexture(GL_TEXTURE_2D, glTex);
            if (s_loc_uvOffset     >= 0) glUniform2f(s_loc_uvOffset,    0.0f, 0.0f);
            if (s_loc_uvSize       >= 0) glUniform2f(s_loc_uvSize,      1.0f, 1.0f);
            if (s_loc_atlasColumns >= 0) glUniform1ui(s_loc_atlasColumns, 0u);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(count * 6u));
        } else {
            if (groupLogEnabled()) {
                std::fprintf(stderr,
                    "[GOSFX_GPU v1] ERROR missing_texture handle=%u\n", gosHandle);
                std::fflush(stderr);
            }
        }
    } else {
        // ── Per-group draw loop (P2-1) ────────────────────────────────
        // For each group produced by Batcher::BeginGroup:
        //   1. Resolve the gos handle to a GLuint; skip if not resident.
        //   2. Upload only this group's records to SSBO offset 0.
        //   3. Set the UV sub-rect uniforms for the billboard VS.
        //   4. Bind the resolved texture and draw.
        for (unsigned gi = 0; gi < numGroups; ++gi) {
            const mc2::particles::GroupInfo& grp = groups[gi];
            if (grp.count == 0) continue;

            // Resolve handle to GL texture name.
            const GLuint glTex = (GLuint)gos_GetGLTextureName(grp.handle);
            if (glTex == 0) {
                if (grp.handle == 0) {
                    // handle=0: B2 debt — point/shard/tube emitters not yet
                    // texture-wired via BeginGroup. Log once under MC2_GOSFX_GROUP_LOG=1.
                    if (groupLogEnabled()) {
                        static bool s_b2DebtWarned = false;
                        if (!s_b2DebtWarned) {
                            s_b2DebtWarned = true;
                            std::fprintf(stderr,
                                "[GOSFX_GPU v1] NOTE handle=0 groups present (B2 debt: point/shard/tube emitters not yet texture-wired)\n");
                            std::fflush(stderr);
                        }
                    }
                } else {
                    // Non-zero handle that failed to resolve — real error.
                    // Gated behind MC2_GOSFX_GROUP_LOG=1 to avoid spew on first
                    // frames before ForceLoadImages() completes.
                    if (groupLogEnabled()) {
                        static std::unordered_set<uint32_t> s_loggedMissingHandles;
                        if (s_loggedMissingHandles.insert(grp.handle).second) {
                            std::fprintf(stderr,
                                "[GOSFX_GPU v1] ERROR missing_texture handle=%u\n", grp.handle);
                            std::fflush(stderr);
                        }
                    }
                }
                continue;
            }

            // Upload this group's contiguous records to SSBO offset 0.
            const mc2::particles::GpuParticle* groupRecords = records + grp.start;
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                            (GLsizeiptr)(grp.count * sizeof(mc2::particles::GpuParticle)),
                            groupRecords);

            // P2-1: set UV sub-rect uniforms per group.
            if (s_loc_uvOffset >= 0) glUniform2f(s_loc_uvOffset, grp.u0, grp.v0);
            if (s_loc_uvSize   >= 0) glUniform2f(s_loc_uvSize,   grp.us, grp.vs);
            // VFX-FLIPBOOK-ASSET-TABLE-1: atlas column count (0 = non-animated).
            if (s_loc_atlasColumns >= 0)
                glUniform1ui(s_loc_atlasColumns, grp.atlasColumns);

            // Per-group blend mode from MLRState (saved state restored after loop).
            // 0 = standard alpha blend: GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
            // 1 = additive:             GL_SRC_ALPHA, GL_ONE
            if (grp.blendMode == 1) {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            } else {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            // VFX-TUNING-UI-1: tell the FS whether this group is additive so the
            // additive-brightness scale applies only to additive groups.
            if (s_loc_vfxIsAdditive >= 0)
                glUniform1i(s_loc_vfxIsAdditive, grp.blendMode == 1 ? 1 : 0);

            // Bind the resolved texture.
            glBindTexture(GL_TEXTURE_2D, glTex);

            // Draw: 6 vertices per particle, gl_VertexID-driven.
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(grp.count * 6u));
        }
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // ── Restore state ────────────────────────────────────────────────
    if (savedCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    // VFX-SOFT-PARTICLES-MVP-1: restore unit-1 binding used for the depth copy
    // (active texture is GL_TEXTURE0 here — the draw loop binds on unit 0).
    if (softActive) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D1);
        glActiveTexture(GL_TEXTURE0);
    }
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

// VFX-SPINE-0: read-only accessors for the Object Inspector. Pure getters
// over file-static state (SSBO capacity, linked program id, init-failure flag,
// per-frame camera-basis "set" flag). No GL calls, no mutation.
extern "C" unsigned int gos_vfx_getParticleProgramId()
{
    return (s_prog && s_prog->shp_) ? (unsigned int)s_prog->shp_ : 0u;
}
extern "C" unsigned int gos_vfx_getSsboCapacity()
{
    return (unsigned int)s_ssboCapacity;
}
extern "C" int gos_vfx_getInitFailed()
{
    return s_initFailed ? 1 : 0;
}
extern "C" int gos_vfx_getCameraSetThisFrame()
{
    return g_cam_set_this_frame ? 1 : 0;
}
// VFX-DEBUG-VIEWS-1: active particle debug mode (0..4), seeded from
// MC2_VFX_DEBUG_MODE. Read-only; resolves the env lazily on first call.
extern "C" int gos_vfx_getDebugMode()
{
    return vfxDebugMode();
}
// VFX-TUNING-UI-1: runtime debug-mode override (Graphics Options combo).
// Clamped 0..4; marks initialized so it wins over a later env read. Look-only.
extern "C" void gos_vfx_setDebugMode(int m)
{
    s_debugMode_initialized = true;
    s_debugMode_value = (m >= 0 && m <= 4) ? m : 0;
}

// VFX-TUNING-UI-1: runtime intensity-scale get/set (Graphics Options sliders).
// Clamped 0..8; default 1.0 = byte-identical no-op. Look-only.
extern "C" float gos_vfx_getBrightness()         { vfxTuneInitIfNeeded(); return s_vfxBrightness; }
extern "C" float gos_vfx_getAdditiveBrightness() { vfxTuneInitIfNeeded(); return s_vfxAdditiveBrightness; }
extern "C" float gos_vfx_getAlphaScale()         { vfxTuneInitIfNeeded(); return s_vfxAlphaScale; }
extern "C" void  gos_vfx_setBrightness(float v)         { vfxTuneInitIfNeeded(); s_vfxBrightness = clampVfxScale(v); }
extern "C" void  gos_vfx_setAdditiveBrightness(float v) { vfxTuneInitIfNeeded(); s_vfxAdditiveBrightness = clampVfxScale(v); }
extern "C" void  gos_vfx_setAlphaScale(float v)         { vfxTuneInitIfNeeded(); s_vfxAlphaScale = clampVfxScale(v); }

// VFX-SOFT-PARTICLES-MVP-1: enable + fade-band accessors (ImGui + inspector).
extern "C" int   gos_vfx_getSoftEnabled()         { vfxSoftInitIfNeeded(); return s_soft_enabled ? 1 : 0; }
extern "C" void  gos_vfx_setSoftEnabled(int e)    { vfxSoftInitIfNeeded(); s_soft_enabled = (e != 0); }
extern "C" float gos_vfx_getSoftDistance()        { return s_softDistance; }
extern "C" void  gos_vfx_setSoftDistance(float v) { s_softDistance = clampSoftDist(v); }

// VFX-LIT-PARTICLES-MVP-1: enable + strength accessors (ImGui + profile + inspector).
extern "C" int   gos_vfx_getLitEnabled()         { vfxLitInitIfNeeded(); return s_lit_enabled ? 1 : 0; }
extern "C" void  gos_vfx_setLitEnabled(int e)    { vfxLitInitIfNeeded(); s_lit_enabled = (e != 0); }
extern "C" float gos_vfx_getLitStrength()        { vfxLitInitIfNeeded(); return s_litStrength; }
extern "C" void  gos_vfx_setLitStrength(float v) { vfxLitInitIfNeeded(); s_litStrength = clampLit(v); }
