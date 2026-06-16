// render_contract.cpp — Render Contract Registry table (phase 2).
//
// Phase 2 changes from phase 1:
//   - All TODO_RENDER_CONTRACT rows filled in with audited values.
//   - PassStateContract gains attachmentCount + RequiredAttachments.
//   - ShaderOutputContract table added (output location declarations).
//   - assertPassContract() + initRenderContractAssert() added; gated by
//     MC2_RENDER_CONTRACT_ASSERT env var. Calls glGetIntegerv/glGetBooleanv
//     to verify live GL state; safe only after glewInit().
//
// Authoritative Phase-1 entries (TerrainBase, TerrainOverlay, TerrainDecal,
// Grass, StaticProp, PostProcess) are unchanged in semantics. New Phase-2
// audit entries (Water, OpaqueObject, AlphaObject, ParticleEffect, UI,
// DebugOverlay, ShadowCaster) reflect best-audit at commit time; mark any
// further findings with TODO_RENDER_CONTRACT_P3 for the next pass.
//
// See: docs/superpowers/specs/2026-04-26-render-contract-registry-design.md

#include "render_contract.h"

// GL access for assertPassContract. Only called from render paths,
// well after glewInit(); glGetIntegerv/glGetBooleanv extension pointers
// are guaranteed valid at that point.
#include <GL/glew.h>

#include <cstdio>   // fprintf
#include <cstdlib>  // getenv

namespace render_contract {

namespace {

using BM  = PassStateContract::BlendMode;
using RA  = RequiredAttachments;
using SOC = ShaderOutputContract;

// TODO_RENDER_CONTRACT sentinels — used for rows that remain unaudited.
constexpr ShadowContract kTodoShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kTodoState{
    /*requiresDepthTest*/    false,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          false,
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "TODO_RENDER_CONTRACT",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kTodoShaderOutput{
    /*writesLocation0*/ false,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// ---- Phase-1 authoritative entries (shadow + state contracts) ---------------

constexpr ShadowContract kTerrainBaseShadow{
    /*castsStaticShadow*/    true,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // gos_terrain.frag writes GBuffer1.a = 1.0
};
constexpr PassStateContract kTerrainBaseState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,      // min 2; 3 when MC2_OBJECT_ID_BUFFER=1 (safe superset)
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kTerrainBaseOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,   // rc_gbuffer1_shadowHandled() macro
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

constexpr ShadowContract kTerrainOverlayShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // terrain_overlay.frag writes GBuffer1.a = 1.0
};
constexpr PassStateContract kTerrainOverlayState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kTerrainOverlayOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

constexpr ShadowContract kTerrainDecalShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // decal.frag writes GBuffer1.a = 1.0
};
constexpr PassStateContract kTerrainDecalState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kTerrainDecalOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

constexpr ShadowContract kGrassShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // gos_grass.frag writes GBuffer1.a = 1.0
};
constexpr PassStateContract kGrassState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::AlphaTest,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kGrassOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

constexpr ShadowContract kStaticPropShadow{
    /*castsStaticShadow*/    true,
    /*castsDynamicShadow*/   true,
    /*skipsPostScreenShadow*/false,  // static_prop.frag writes GBuffer1.a = 0.0
};
constexpr PassStateContract kStaticPropState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          true,
    /*attachmentCount*/      3,      // writes objectId at location 2 (M1.5)
    /*attachments*/          RA{true, true, true},
    /*expectedFBO*/          "scene HDR FBO (MRT + objectId)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kStaticPropOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ true,   // layout(location=2) out uint v_objectId (M1.5)
    /*writesDepth*/     false,
};

constexpr ShadowContract kPostProcessShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,  // operates on G-buffer; not applicable
};
constexpr PassStateContract kPostProcessState{
    /*requiresDepthTest*/    false,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          false,
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "post FBO (single attachment)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kPostProcessOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// ---- Phase-2 audit entries --------------------------------------------------

// Water: renders into scene HDR FBO; uses MRT so shadow_screen.frag can
// sample GBuffer1. gos_terrain.frag water path writes GBuffer1.a =
// materialAlpha (documented §3.1 ambiguity — continuous, not boolean).
// Depth write ON: water surface claims depth to prevent terrain bleed-through.
constexpr ShadowContract kWaterShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,  // materialAlpha path; value depends on pixel
};
constexpr PassStateContract kWaterState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kWaterOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,   // materialAlpha — known §3.1 ambiguity
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// OpaqueObject (mechs): MRT scene FBO; writes objectId at location 2 (M2.5).
// No static shadow (MLR path; shadow atlas is terrain-only in current impl).
constexpr ShadowContract kOpaqueObjectShadow{
    /*castsStaticShadow*/    false,  // TODO_RENDER_CONTRACT_P3: confirm shadow atlas scope
    /*castsDynamicShadow*/   true,
    /*skipsPostScreenShadow*/false,  // mechs receive screen shadow
};
constexpr PassStateContract kOpaqueObjectState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          true,
    /*attachmentCount*/      3,      // writes objectId (M2.5)
    /*attachments*/          RA{true, true, true},
    /*expectedFBO*/          "scene HDR FBO (MRT + objectId)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kOpaqueObjectOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ true,   // mech.frag layout(location=2) out uint v_objectId (M2.5)
    /*writesDepth*/     false,
};

// AlphaObject: same FBO as opaque, depth-tested but no depth-write (standard
// alpha ordering). Does not write objectId (alpha geometry not picked).
constexpr ShadowContract kAlphaObjectShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kAlphaObjectState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kAlphaObjectOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// ParticleEffect: additive/alpha-blend forward pass INTO the scene FBO while
// MRT is still active. Only writes location 0; location 1 write would corrupt
// GBuffer1 shadow mask for additive transparent pixels.
constexpr ShadowContract kParticleEffectShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kParticleEffectState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::Additive,
    /*requiresMRT*/          false,  // doesn't rely on MRT; just needs color0
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "scene HDR FBO (color0 only needed)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kParticleEffectOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ false,  // must NOT write GBuffer1 (shadow mask corruption)
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// UI: screen-space, no depth, single attachment (backbuffer or blit target).
constexpr ShadowContract kUIShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kUIState{
    /*requiresDepthTest*/    false,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          false,
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "backbuffer / blit target",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kUIOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// DebugOverlay: overlaid on screen, must restore blend+depth state on exit.
constexpr ShadowContract kDebugOverlayShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kDebugOverlayState{
    /*requiresDepthTest*/    false,  // depth-aware debug vis enables this per-mode
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          false,
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "any (overlay renders into current target)",
    /*restoresStateOnExit*/  true,
};
constexpr SOC kDebugOverlayOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// ShadowCaster: depth-only pass; no color outputs. Separate depth-only FBO
// for the shadow atlas / dynamic shadow map. Explicit depth write is the
// entire purpose of the pass.
constexpr ShadowContract kShadowCasterShadow{
    /*castsStaticShadow*/    true,
    /*castsDynamicShadow*/   true,
    /*skipsPostScreenShadow*/false,  // not applicable
};
constexpr PassStateContract kShadowCasterState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          false,
    /*attachmentCount*/      0,      // depth-only FBO; no color attachments
    /*attachments*/          RA{false, false, false},
    /*expectedFBO*/          "shadow depth-only FBO",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kShadowCasterOutput{
    /*writesLocation0*/ false,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     true,   // depth-only; may use gl_FragDepth or implicit
};

// ---- Lookup helpers --------------------------------------------------------

const ShadowContract& lookupShadow(PassIdentity id) {
    switch (id) {
        case PassIdentity::TerrainBase:    return kTerrainBaseShadow;
        case PassIdentity::TerrainOverlay: return kTerrainOverlayShadow;
        case PassIdentity::TerrainDecal:   return kTerrainDecalShadow;
        case PassIdentity::Grass:          return kGrassShadow;
        case PassIdentity::Water:          return kWaterShadow;
        case PassIdentity::OpaqueObject:   return kOpaqueObjectShadow;
        case PassIdentity::AlphaObject:    return kAlphaObjectShadow;
        case PassIdentity::StaticProp:     return kStaticPropShadow;
        case PassIdentity::ParticleEffect: return kParticleEffectShadow;
        case PassIdentity::UI:             return kUIShadow;
        case PassIdentity::DebugOverlay:   return kDebugOverlayShadow;
        case PassIdentity::ShadowCaster:   return kShadowCasterShadow;
        case PassIdentity::PostProcess:    return kPostProcessShadow;
        case PassIdentity::Unknown:        return kTodoShadow;
    }
    return kTodoShadow;
}

const PassStateContract& lookupState(PassIdentity id) {
    switch (id) {
        case PassIdentity::TerrainBase:    return kTerrainBaseState;
        case PassIdentity::TerrainOverlay: return kTerrainOverlayState;
        case PassIdentity::TerrainDecal:   return kTerrainDecalState;
        case PassIdentity::Grass:          return kGrassState;
        case PassIdentity::Water:          return kWaterState;
        case PassIdentity::OpaqueObject:   return kOpaqueObjectState;
        case PassIdentity::AlphaObject:    return kAlphaObjectState;
        case PassIdentity::StaticProp:     return kStaticPropState;
        case PassIdentity::ParticleEffect: return kParticleEffectState;
        case PassIdentity::UI:             return kUIState;
        case PassIdentity::DebugOverlay:   return kDebugOverlayState;
        case PassIdentity::ShadowCaster:   return kShadowCasterState;
        case PassIdentity::PostProcess:    return kPostProcessState;
        case PassIdentity::Unknown:        return kTodoState;
    }
    return kTodoState;
}

const SOC& lookupShaderOutput(PassIdentity id) {
    switch (id) {
        case PassIdentity::TerrainBase:    return kTerrainBaseOutput;
        case PassIdentity::TerrainOverlay: return kTerrainOverlayOutput;
        case PassIdentity::TerrainDecal:   return kTerrainDecalOutput;
        case PassIdentity::Grass:          return kGrassOutput;
        case PassIdentity::Water:          return kWaterOutput;
        case PassIdentity::OpaqueObject:   return kOpaqueObjectOutput;
        case PassIdentity::AlphaObject:    return kAlphaObjectOutput;
        case PassIdentity::StaticProp:     return kStaticPropOutput;
        case PassIdentity::ParticleEffect: return kParticleEffectOutput;
        case PassIdentity::UI:             return kUIOutput;
        case PassIdentity::DebugOverlay:   return kDebugOverlayOutput;
        case PassIdentity::ShadowCaster:   return kShadowCasterOutput;
        case PassIdentity::PostProcess:    return kPostProcessOutput;
        case PassIdentity::Unknown:        return kTodoShaderOutput;
    }
    return kTodoShaderOutput;
}

// ---- Assert machinery -------------------------------------------------------

static bool s_assertEnabled = false;

static void fatalContractViolation(const char* passName, const char* hint,
                                   const char* msg)
{
    fprintf(stderr,
        "[RENDER_CONTRACT_ASSERT] FATAL pass=%s hint=%s: %s\n",
        passName, hint ? hint : "?", msg);
    fflush(stderr);
    std::abort();
}

static void checkDrawBuffer(GLenum query, bool required,
                            const char* slotName, const char* passName,
                            const char* hint)
{
    if (!required) return;
    GLint buf = 0;
    glGetIntegerv(query, &buf);
    if (buf == GL_NONE) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s required but draw buffer is GL_NONE", slotName);
        fatalContractViolation(passName, hint, msg);
    }
}

// Returns the GL depth func expected for this pass, or 0 meaning "don't check."
// Opaque scene passes use reverse-Z (GL_GEQUAL); shadow caster uses forward-Z (GL_LESS).
static GLenum expectedDepthFuncFor(PassIdentity id)
{
    switch (id) {
        case PassIdentity::TerrainBase:
        case PassIdentity::Grass:
        case PassIdentity::StaticProp:
        case PassIdentity::OpaqueObject:
            return GL_GEQUAL;
        case PassIdentity::ShadowCaster:
            return GL_LESS;
        default:
            return 0; // not checked for this pass
    }
}

} // namespace

// ---- Public API ------------------------------------------------------------

const ShadowContract&       shadowContractFor(PassIdentity id)      { return lookupShadow(id); }
const PassStateContract&    stateContractFor(PassIdentity id)       { return lookupState(id); }
const ShaderOutputContract& shaderOutputContractFor(PassIdentity id){ return lookupShaderOutput(id); }

const char* passIdentityName(PassIdentity id) {
    switch (id) {
        case PassIdentity::Unknown:        return "Unknown";
        case PassIdentity::TerrainBase:    return "TerrainBase";
        case PassIdentity::TerrainOverlay: return "TerrainOverlay";
        case PassIdentity::TerrainDecal:   return "TerrainDecal";
        case PassIdentity::Grass:          return "Grass";
        case PassIdentity::Water:          return "Water";
        case PassIdentity::OpaqueObject:   return "OpaqueObject";
        case PassIdentity::AlphaObject:    return "AlphaObject";
        case PassIdentity::StaticProp:     return "StaticProp";
        case PassIdentity::ParticleEffect: return "ParticleEffect";
        case PassIdentity::UI:             return "UI";
        case PassIdentity::DebugOverlay:   return "DebugOverlay";
        case PassIdentity::ShadowCaster:   return "ShadowCaster";
        case PassIdentity::PostProcess:    return "PostProcess";
    }
    return "Unknown";
}

void initRenderContractAssert() {
    const char* v = ::getenv("MC2_RENDER_CONTRACT_ASSERT");
    s_assertEnabled = (v && v[0] != '\0' && v[0] != '0');
    if (s_assertEnabled)
        fprintf(stderr, "[RENDER_CONTRACT v2] assert mode ACTIVE\n");
}

void assertPassContract(PassIdentity id, const char* hint) {
    if (!s_assertEnabled) return;
    const PassStateContract& state = lookupState(id);
    const char* passName = passIdentityName(id);

    // --- draw-buffer attachment checks ---
    checkDrawBuffer(GL_DRAW_BUFFER0, state.attachments.color0,
                    "COLOR_ATTACHMENT0", passName, hint);
    checkDrawBuffer(GL_DRAW_BUFFER1, state.attachments.color1,
                    "COLOR_ATTACHMENT1", passName, hint);
    checkDrawBuffer(GL_DRAW_BUFFER2, state.attachments.color2,
                    "COLOR_ATTACHMENT2", passName, hint);

    // --- depth test ---
    if (state.requiresDepthTest) {
        GLboolean dt = GL_FALSE;
        glGetBooleanv(GL_DEPTH_TEST, &dt);
        if (!dt)
            fatalContractViolation(passName, hint,
                "depth test required but GL_DEPTH_TEST disabled");
    }

    // --- depth write ---
    if (state.requiresDepthWrite) {
        GLboolean dw = GL_FALSE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dw);
        if (!dw)
            fatalContractViolation(passName, hint,
                "depth write required but GL_DEPTH_WRITEMASK=FALSE");
    }

    // --- depth func: GL_GEQUAL for opaque scene, GL_LESS for shadow ---
    const GLenum wantDF = expectedDepthFuncFor(id);
    if (wantDF != 0) {
        GLint df = 0;
        glGetIntegerv(GL_DEPTH_FUNC, &df);
        if ((GLenum)df != wantDF) {
            const char* msg =
                (wantDF == GL_GEQUAL) ?
                    "depth func must be GL_GEQUAL (reverse-Z scene pass)" :
                    "depth func must be GL_LESS (shadow forward-Z pass)";
            fatalContractViolation(passName, hint, msg);
        }
    }

    // --- blend disabled for opaque passes ---
    if (state.blend == BM::Opaque) {
        GLboolean blendOn = GL_FALSE;
        glGetBooleanv(GL_BLEND, &blendOn);
        if (blendOn)
            fatalContractViolation(passName, hint,
                "GL_BLEND enabled but pass blend contract is Opaque");
    }
}

// ---- [RENDER_PASS v1] telemetry (slice D1) ----------------------------------
//
// Advisory, log-only, env-gated (MC2_RENDER_PASS_TELEMETRY=1). When OFF the
// hot-path cost is a single cached-bool branch. When ON, emission is
// rate-limited to one line per pass per sampled frame; a frame is sampled
// every kTelemetryFrameInterval frames (300, matching debug_state_dump
// cadence). GL queries (FBO binding, viewport, draw-buffer count) run only
// on sampled frames at pass-begin points.
//
// D2 (deferred): per-pass draw-call/triangle/texture-bind counters + GPU
// timing — those need counter feeds at centralized draw-submission sites.

namespace {

constexpr std::uint64_t kTelemetryFrameInterval = 300;

bool          s_telemetryEnabled = false;
std::uint64_t s_telemetryFrame   = 0;
std::uint64_t s_telemetryLastEmitFrame[16] = {};  // index = (uint8)PassIdentity; 0 sentinel handled below
bool          s_telemetryEverEmitted[16]   = {};

} // namespace

void initRenderPassTelemetry() {
    const char* v = ::getenv("MC2_RENDER_PASS_TELEMETRY");
    s_telemetryEnabled = (v != nullptr && v[0] == '1');
    if (s_telemetryEnabled) {
        printf("[RENDER_PASS v1] telemetry ACTIVE (sample every %u frames)\n",
               (unsigned)kTelemetryFrameInterval);
        fflush(stdout);
    }
}

void renderPassTelemetryFrameTick() {
    if (!s_telemetryEnabled) return;
    ++s_telemetryFrame;
}

void noteRenderPass(PassIdentity id, const char* callerHint) {
    if (!s_telemetryEnabled) return;
    // Sample frame 1 (first full frame) then every kTelemetryFrameInterval.
    if (s_telemetryFrame % kTelemetryFrameInterval != 1) return;

    const unsigned idx = static_cast<unsigned>(id) & 15u;
    if (s_telemetryEverEmitted[idx] && s_telemetryLastEmitFrame[idx] == s_telemetryFrame)
        return;  // already emitted for this pass this sampled frame
    s_telemetryEverEmitted[idx]   = true;
    s_telemetryLastEmitFrame[idx] = s_telemetryFrame;

    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    int drawBuffers = 0;
    for (int i = 0; i < 8; ++i) {
        GLint buf = 0;
        glGetIntegerv(GL_DRAW_BUFFER0 + i, &buf);
        if (buf != GL_NONE) ++drawBuffers;
    }

    printf("[RENDER_PASS v1] frame=%llu pass=%s fbo=%d viewport=%d,%d,%d,%d drawbuffers=%d phase=begin hint=%s\n",
           (unsigned long long)s_telemetryFrame, passIdentityName(id), (int)fbo,
           (int)vp[0], (int)vp[1], (int)vp[2], (int)vp[3], drawBuffers,
           callerHint ? callerHint : "?");
    fflush(stdout);
}

} // namespace render_contract
