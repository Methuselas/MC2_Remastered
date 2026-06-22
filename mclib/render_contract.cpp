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

#include "RenderCore/RenderPassContract.h"
#include "RenderCore/RenderResourceRegistry.h"

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

// VegetationCards: instanced crossed-quad billboards, alpha-discard (v1).
// No shadows cast (v1 — no shadow atlas slot allocated).
// Depth test ON (must z-reject behind terrain/mechs); depth write OFF (alpha-discard
// geom without depth write avoids self-occlusion artefacts in crossed-quad pairs).
// AlphaTest blend = the frag shader discards fragments below threshold.
// skipsPostScreenShadow true: vegetation is already lit at draw time, same
// as grass; post-screen shadow darkening on top would double-shade.
constexpr ShadowContract kVegetationCardsShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,
};
constexpr PassStateContract kVegetationCardsState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaTest,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kVegetationCardsOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,   // writes GBuffer1.a = 1.0 to skip post-screen shadow
    /*writesLocation2*/ false,
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
        case PassIdentity::StaticProp:        return kStaticPropShadow;
        case PassIdentity::VegetationCards:   return kVegetationCardsShadow;
        case PassIdentity::ParticleEffect:    return kParticleEffectShadow;
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
        case PassIdentity::StaticProp:        return kStaticPropState;
        case PassIdentity::VegetationCards:   return kVegetationCardsState;
        case PassIdentity::ParticleEffect:    return kParticleEffectState;
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
        case PassIdentity::StaticProp:        return kStaticPropOutput;
        case PassIdentity::VegetationCards:   return kVegetationCardsOutput;
        case PassIdentity::ParticleEffect:    return kParticleEffectOutput;
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
        case PassIdentity::StaticProp:        return "StaticProp";
        case PassIdentity::VegetationCards:   return "VegetationCards";
        case PassIdentity::ParticleEffect:    return "ParticleEffect";
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

// ---- CONTRACT-3: per-frame resource-ordering audit ---------------------------
//
// MC2_RENDER_PASS_ORDER=1  -- check reads[] satisfied each beginPass
// MC2_RENDER_PASS_TELEMETRY (shared with [RENDER_PASS v1]) also drives endPass telemetry
//
// FrameResourceState is process-global static (no GL dependency; safe from
// any call site). forceEnabled is set by _test_forceOrderAudit() for CI use.

namespace {

struct FrameResourceState {
    uint32_t writtenBits    = 0;
    uint32_t violationCount = 0;
    int      telemetryTick  = 0;
    bool     orderEnabled   = false;
    bool     telEnabled     = false;
    bool     forceEnabled   = false; // test-only override
};
static FrameResourceState s_frame;

static constexpr int kPassOrderTelemetryInterval = 300;

bool isResourceWritten(RenderCore::RenderResourceId id) {
    if (id == RenderCore::RenderResourceId::Unknown) return true;
    return (s_frame.writtenBits & (1u << static_cast<uint32_t>(id))) != 0;
}

void markResourceWritten(RenderCore::RenderResourceId id) {
    if (id == RenderCore::RenderResourceId::Unknown) return;
    s_frame.writtenBits |= (1u << static_cast<uint32_t>(id));
}

const char* resourceName(RenderCore::RenderResourceId id) {
    switch (id) {
        case RenderCore::RenderResourceId::Unknown:              return "Unknown";
        case RenderCore::RenderResourceId::MainColor:            return "MainColor";
        case RenderCore::RenderResourceId::MainDepth:            return "MainDepth";
        case RenderCore::RenderResourceId::ShadowStaticMap:      return "ShadowStaticMap";
        case RenderCore::RenderResourceId::TerrainHeightTexture: return "TerrainHeightTexture";
        case RenderCore::RenderResourceId::MaterialGpuBuffer:    return "MaterialGpuBuffer";
        case RenderCore::RenderResourceId::ShadowDynamicMap:     return "ShadowDynamicMap";
        case RenderCore::RenderResourceId::WaterReflectionColor: return "WaterReflectionColor";
        case RenderCore::RenderResourceId::WaterReflectionDepth: return "WaterReflectionDepth";
        default:                                                  return "???";
    }
}

const RenderCore::RenderPassContract* findPassContract(RenderCore::RenderPassId id) {
    for (const auto& c : RenderCore::kRenderPassContracts) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

} // anonymous namespace (CONTRACT-3 helpers)

void initRenderPassOrder() {
    const char* v = getenv("MC2_RENDER_PASS_ORDER");
    s_frame.orderEnabled = s_frame.forceEnabled || (v && v[0] == '1');
    const char* t = getenv("MC2_RENDER_PASS_TELEMETRY");
    s_frame.telEnabled = s_frame.forceEnabled || (t && t[0] == '1');
}

void _test_forceOrderAudit(bool enabled) {
    s_frame.forceEnabled = enabled;
    if (enabled) {
        s_frame.orderEnabled = true;
        s_frame.telEnabled   = true;
    }
}

void frameBegin() {
    s_frame.writtenBits    = 0;
    s_frame.violationCount = 0;
    // Pre-seed resources that are always valid (not produced per-frame):
    //   ShadowStaticMap: built once per mission at renderLists() preamble
    //   TerrainHeightTexture: uploaded at mission load
    markResourceWritten(RenderCore::RenderResourceId::ShadowStaticMap);
    markResourceWritten(RenderCore::RenderResourceId::TerrainHeightTexture);
    ++s_frame.telemetryTick;
}

void beginPass(RenderCore::RenderPassId id) {
    if (!s_frame.orderEnabled) return;
    const RenderCore::RenderPassContract* c = findPassContract(id);
    if (!c) return;
    for (int i = 0; i < 4; ++i) {
        RenderCore::RenderResourceId r = c->reads[i];
        if (r == RenderCore::RenderResourceId::Unknown) break;
        if (!isResourceWritten(r)) {
            ++s_frame.violationCount;
            fprintf(stderr,
                "[RENDER_PASS_ORDER v3] VIOLATION: pass %s reads %s but no writer seen this frame\n",
                c->name, resourceName(r));
        }
    }
}

void endPass(RenderCore::RenderPassId id) {
    // Early-out on default path (both gates off) -- skip table scan.
    if (!s_frame.orderEnabled && !s_frame.telEnabled) return;

    const RenderCore::RenderPassContract* c = findPassContract(id);
    if (!c) return;

    // ORDER MATTERS: inspect read status BEFORE marking writes complete.
    // 1. Capture read ok/missing status for telemetry
    bool readOk[4] = {};
    int  readCount = 0;
    for (int i = 0; i < 4; ++i) {
        if (c->reads[i] == RenderCore::RenderResourceId::Unknown) break;
        readOk[i] = isResourceWritten(c->reads[i]);
        ++readCount;
    }

    // 2. Mark this pass's writes complete
    for (int i = 0; i < 4; ++i) {
        RenderCore::RenderResourceId w = c->writes[i];
        if (w == RenderCore::RenderResourceId::Unknown) break;
        markResourceWritten(w);
    }

    // 3. Emit telemetry (rate-limited)
    if (!s_frame.telEnabled) return;
    if (s_frame.telemetryTick % kPassOrderTelemetryInterval != 0) return;

    fprintf(stderr, "[RENDER_PASS_TELEMETRY v3] pass=%s", c->name);
    for (int i = 0; i < readCount; ++i) {
        fprintf(stderr, " read=%s:%s",
            resourceName(c->reads[i]),
            readOk[i] ? "ok" : "MISSING");
    }
    for (int i = 0; i < 4; ++i) {
        if (c->writes[i] == RenderCore::RenderResourceId::Unknown) break;
        fprintf(stderr, " write=%s", resourceName(c->writes[i]));
    }
    fprintf(stderr, "\n");
}

uint32_t getFrameViolationCount() {
    return s_frame.violationCount;
}

// ---- RENDER-PASS-CONTRACT-ENFORCEMENT-1: pass-scope tracking -----------------
//
// A bounded scope stack. begin pushes, end pops-and-checks, the frame boundary
// flushes any still-open scopes. Diagnostics only: every anomaly is logged to
// stderr and counted; nothing aborts and no GL/render state is touched. The
// stack is process-global (no GL dependency) and only mutated from the render
// thread, matching the existing CONTRACT-3 / telemetry single-thread model.

namespace {

constexpr int kPassScopeMaxDepth = 8;   // slice 1 stays at depth 1; cap guards runaway begins

struct PassScopeEntry {
    PassIdentity id;
    const char*  hint;   // borrowed literal / __func__; never owned
};

struct PassScopeState {
    bool           traceEnabled    = false;
    bool           assertEnabled   = false;
    int            depth           = 0;
    bool           overflowed      = false;  // latched once depth cap hit, until frame boundary
    uint32_t       violationCount  = 0;       // cumulative since process start
    PassScopeEntry stack[kPassScopeMaxDepth] = {};
};
static PassScopeState s_scope;

static inline bool passScopeActive() {
    return s_scope.traceEnabled || s_scope.assertEnabled;
}

static void passScopeViolation(const char* what, PassIdentity id, const char* hint) {
    ++s_scope.violationCount;
    if (!s_scope.assertEnabled) return;   // TRACE-only run: track silently, no verdict noise
    fprintf(stderr,
        "[RENDER_PASS_CONTRACT v1] VIOLATION(%s) pass=%s hint=%s depth=%d\n",
        what, passIdentityName(id), hint ? hint : "?", s_scope.depth);
    fflush(stderr);
}

} // anonymous namespace (scope-tracking helpers)

void initRenderPassScope() {
    const char* t = ::getenv("MC2_RENDER_PASS_CONTRACT_TRACE");
    const char* a = ::getenv("MC2_RENDER_PASS_CONTRACT_ASSERT");
    s_scope.traceEnabled  = (t && t[0] == '1');
    s_scope.assertEnabled = (a && a[0] == '1');
    s_scope.depth         = 0;
    s_scope.overflowed    = false;
    if (passScopeActive()) {
        fprintf(stderr, "[RENDER_PASS_CONTRACT v1] scope tracking ACTIVE (trace=%d assert=%d)\n",
                s_scope.traceEnabled ? 1 : 0, s_scope.assertEnabled ? 1 : 0);
        fflush(stderr);
    }
}

void beginPassScope(PassIdentity id, const char* hint) {
    if (!passScopeActive()) return;
    if (s_scope.depth >= kPassScopeMaxDepth) {
        if (!s_scope.overflowed) {
            s_scope.overflowed = true;
            passScopeViolation("depth-cap", id, hint);   // counts once per overflow episode
        }
        return;   // refuse to push past the cap; do not corrupt the stack
    }
    if (s_scope.traceEnabled) {
        fprintf(stderr, "[RENDER_PASS_CONTRACT v1] BEGIN depth=%d pass=%s hint=%s\n",
                s_scope.depth, passIdentityName(id), hint ? hint : "?");
        fflush(stderr);
    }
    s_scope.stack[s_scope.depth].id   = id;
    s_scope.stack[s_scope.depth].hint = hint;
    ++s_scope.depth;
}

void endPassScope(PassIdentity id, const char* hint) {
    if (!passScopeActive()) return;
    if (s_scope.depth <= 0) {
        passScopeViolation("end-without-begin", id, hint);
        return;
    }
    const PassScopeEntry& top = s_scope.stack[s_scope.depth - 1];
    if (top.id != id) {
        // Owner mismatch: closing a pass that is not the innermost open scope.
        if (s_scope.assertEnabled) {
            fprintf(stderr,
                "[RENDER_PASS_CONTRACT v1] VIOLATION(owner-mismatch) end=%s hint=%s "
                "but innermost open=%s (openedBy=%s) depth=%d\n",
                passIdentityName(id), hint ? hint : "?",
                passIdentityName(top.id), top.hint ? top.hint : "?", s_scope.depth);
            fflush(stderr);
        }
        ++s_scope.violationCount;
        // Recovery: pop the innermost so the stack still drains by frame end.
    }
    --s_scope.depth;
    if (s_scope.traceEnabled) {
        fprintf(stderr, "[RENDER_PASS_CONTRACT v1] END   depth=%d pass=%s hint=%s\n",
                s_scope.depth, passIdentityName(id), hint ? hint : "?");
        fflush(stderr);
    }
}

void renderPassScopeFrameBoundary() {
    if (!passScopeActive()) return;
    if (s_scope.depth != 0) {
        for (int i = s_scope.depth - 1; i >= 0; --i) {
            passScopeViolation("missing-end", s_scope.stack[i].id, s_scope.stack[i].hint);
        }
    }
    s_scope.depth      = 0;
    s_scope.overflowed = false;
}

uint32_t getPassScopeViolationCount() {
    return s_scope.violationCount;
}

} // namespace render_contract
