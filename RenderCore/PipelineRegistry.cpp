// RenderCore/PipelineRegistry.cpp
//
// Static table of PipelineDesc entries, indexed by PipelineId.
// Add new rows here whenever PipelineRegistry.h gains a new PipelineId value;
// the static_assert below will fire if the table and enum fall out of sync.
//
// GL state encoded in this table (applied by pipeline_binder::applyPipeline):
//   depthTestEnable / depthWriteEnable / depthFunc=GreaterEqual (reverse-Z)
//   blend=Opaque or AlphaTest (GL_BLEND stays disabled for both)
//   cullMode=Back
// SSBO slots still bound explicitly in flush() (slot 2, 0, 1) — ssboBindingsMask
// documents them but does not drive binding; that is a future automation pass.
//
// AlphaTest note: static prop alpha is shader-discard (binary cutout).
// GL_BLEND stays disabled and GL_DEPTH_MASK stays GL_TRUE for both passes —
// the "alpha" split drives texture-array selection, not GL blend state.
//
// Object-ID note: color2 / objectIdWriteEnabled are false here. The static
// prop fragment shaders do not yet declare layout(location=2). Flip both
// flags when that shader output is added.

#include "PipelineRegistry.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <string>

namespace RenderCore {

// SSBO binding slots — bit N = slot N. See PipelineDesc.h for the full table.
static constexpr uint32_t kSsboInstances = 1u << 0;   // slot 0 — per-type instance data
static constexpr uint32_t kSsboColors    = 1u << 1;   // slot 1 — per-type color data
static constexpr uint32_t kSsboPerType   = 1u << 2;   // slot 2 — hot-color s_perTypeSsbo

static constexpr uint32_t kStaticPropSsbos = kSsboInstances | kSsboColors | kSsboPerType;

// Table indexed by static_cast<size_t>(PipelineId).
// Row 0 must be the Invalid sentinel (zeroed PipelineDesc).
static std::array<PipelineDesc, static_cast<size_t>(PipelineId::Count_)> s_descs = {{

    // [0] Invalid — zeroed sentinel; returned for bad lookups.
    PipelineDesc{},

    // [1] StaticPropOpaque
    // Renders the alpha-OFF texture-array group. Full depth, no blending.
    {
        /* glProgramName       */ 0u,              // filled by bindProgram()
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z throughout MC2
        /* cullMode            */ CullMode::Back,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,           // TODO: flip when shader adds loc=2
        /* frontFace           */ FrontFace::Ccw,  // GL default; explicit per row
        /* polygonOffsetEnable */ false,           // scene passes: no polygon offset
        /* ssboBindingsMask    */ kStaticPropSsbos,
    },

    // [2] StaticPropAlphaTest
    // Renders the alpha-ON texture-array group via shader discard.
    // GL blend stays disabled; depth write stays on (binary cutout writes depth).
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AlphaTest,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,            // intentional: discard ≠ blend
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::Back,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,  // GL default; explicit per row
        /* polygonOffsetEnable */ false,           // scene passes: no polygon offset
        /* ssboBindingsMask    */ kStaticPropSsbos,
    },

    // [3] MechOpaque — GPU mech batcher (GpuMechBatcher::flush). Mirrors the
    // fixed-function state the batcher previously set by hand: full depth
    // (test+write, reverse-Z GEQUAL), no blend, cull back. Color0 + GBuffer1
    // (normal/screen-shadow); object-ID (loc=2) is GLSL-macro-gated in mech.frag
    // (MC2_OBJECT_ID_BUFFER), not an applyPipeline attachment toggle, so
    // objectIdWriteEnabled stays false here (descriptive; applyPipeline does not
    // reconfigure draw buffers). ssboBindingsMask = 0: the mech batcher binds its
    // own SSBOs (instance/bone/material/lights) manually; the mask is metadata
    // and applyPipeline does not bind SSBOs. glProgramName filled by bindProgram()
    // at mech shader link (loadProgramsIfNeeded). The mech sampler bind stays
    // manual (no PipelineDesc sampler field).
    {
        /* glProgramName       */ 0u,              // filled by bindProgram()
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z
        /* cullMode            */ CullMode::Back,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,           // macro-gated in shader, not here
        /* frontFace           */ FrontFace::Ccw,  // GL default; explicit per row
        /* polygonOffsetEnable */ false,           // scene passes: no polygon offset
        /* ssboBindingsMask    */ 0u,              // mech binds its own SSBOs
    },

    // [4] StaticPropDepth — camera depth-prepass. Depth-only: GEQUAL + write so
    // it lays the nearest reverse-Z depth; alpha discard in static_prop_depth.frag.
    // Color writes are masked off by the caller (glColorMask), NOT by attachment
    // changes — same FBO stays bound. Shares the static-prop SSBOs (instance /
    // per-type / per-draw) because it reuses static_prop.vert.
    {
        /* glProgramName       */ 0u,              // filled by bindProgram()
        /* blend               */ BlendMode::AlphaTest, // discard path; GL_BLEND off
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z, lay nearest
        /* cullMode            */ CullMode::Back,
        // IMPORTANT-2: depth-only — no color attachments written. The caller masks
        // color via glColorMask (not attachment reconfig; same FBO stays bound), so
        // this row advertises the true write set: nothing color, depth only.
        /* colorAttachments    */ { false, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,  // GL default; explicit per row
        /* polygonOffsetEnable */ false,           // scene passes: no polygon offset
        /* ssboBindingsMask    */ kStaticPropSsbos,
    },

    // [5] ShadowTerrain — terrain -> static shadow map (shadow_terrain).
    // SHADOW-CASTER-PIPELINE-REGISTRATION-1: DESCRIPTIVE row — states the truth
    // but is NOT applyPipeline-driven (the pass bracket in gameos_graphics.cpp
    // sets this state by hand; glProgramName stays 0, bindProgram wiring deferred
    // to the active-routing slice). Forward-Z GL_LESS (opposite the scene's
    // reverse-Z), depth-only, cull DISABLED, no polygon offset.
    {
        /* glProgramName       */ 0u,              // descriptive; not yet wired
        /* blend               */ BlendMode::Opaque, // depth-only; GL_BLEND off
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::Less, // shadow map = forward-Z GL_LESS
        /* cullMode            */ CullMode::None,  // shadow bracket disables cull
        /* colorAttachments    */ { false, false, false }, // pure depth (dummy R8 never written)
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,  // no glFrontFace in shadow; ambient default
        /* polygonOffsetEnable */ false,           // terrain caster: no polygon offset
        /* ssboBindingsMask    */ 0u,              // descriptive; pass binds its own
    },

    // [6] ShadowStaticProp — static/dynamic props -> shadow map (shadow_static_prop).
    // The ONLY shadow caster that enables GL_POLYGON_OFFSET_FILL (gos_static_prop_
    // batcher.cpp:7639/7787, factor/units 2.0/4.0 set by hand, ImGui-mutable — the
    // magnitude is dynamic state, NOT modeled here). DESCRIPTIVE.
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::Less,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { false, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ true,            // prop caster: polygon offset ON
        /* ssboBindingsMask    */ 0u,
    },

    // [7] ShadowMech — mech -> dynamic shadow map (shadow_mech;
    // GpuMechBatcher::flushShadow). DESCRIPTIVE; no polygon offset.
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::Less,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { false, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,           // mech caster: no polygon offset
        /* ssboBindingsMask    */ 0u,
    },

    // [8] TerrainOverlay — cement/perimeter overlay (terrain_overlay.{vert,frag}).
    // TERRAIN-OVERLAY-DECAL-PIPELINE-REGISTRATION-1: DESCRIPTIVE — states the truth
    // (gosRenderer::drawTerrainOverlays/drawDecalStaticBatch set this by hand) but
    // is NOT applyPipeline-driven; glProgramName stays 0. Opaque, reverse-Z GEQUAL,
    // depth-write on, cull DISABLED (overlay tiles draw both faces). frag writes
    // color0 (FragColor) + color1 (GBuffer1).
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z (scene-space overlay)
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,           // overlay: no polygon offset (in-material blend)
        /* ssboBindingsMask    */ 0u,              // binds its own VBO/texture; no SSBO
    },

    // [9] TerrainDecal — bomb craters + mech footprints (terrain_overlay.vert +
    // decal.frag). DESCRIPTIVE; gosRenderer::drawDecals sets this by hand. The
    // FIRST AlphaBlend pipeline row: GL_BLEND on, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    // (BlendMode::AlphaBlend), depth-test on but depth-WRITE OFF (decals must not
    // occlude), reverse-Z GEQUAL, cull disabled. frag writes color0 + color1.
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AlphaBlend, // FIRST alpha-blend row
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,           // decals do not write depth
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },

    // [10] WaterArmed — the ARMED water fast-path base (renderWaterFastPath:
    // gos_terrain_water_fast.vert + gos_tex_vertex.frag). DESCRIPTIVE;
    // gosRenderer::renderWaterFastPath sets this by hand (and save/restores it).
    // Source-verified state (gameos_graphics.cpp:3275-3288): GL_CULL_FACE DISABLED
    // (CINEMATIC-WATER-CULL-1 — flat overlay mesh, must not be backface-culled),
    // AlphaBlend (SRC_ALPHA/ONE_MINUS_SRC_ALPHA), depth-test on + reverse-Z GEQUAL,
    // depth-WRITE ON by default (OOB-FOG-WATER-DEPTH-1; only MC2_WATER_NO_DEPTH_WRITE
    // debug-gate flips it off). frag writes color0 + color1. NOTE: the legacy quad
    // fallback and the GPU-driven MDI sub-variant are NOT modeled by this row.
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AlphaBlend, // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,            // default armed (MC2_WATER_NO_DEPTH_WRITE = debug A/B only)
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z (scene water)
        /* cullMode            */ CullMode::None,  // water explicitly disables cull
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,              // armed base binds its own buffers; MDI sub-path SSBOs not modeled
    },

    // [11-16] VFX family — VFX-PIPELINE-REGISTRATION-1. DESCRIPTIVE (glProgramName=0,
    // NOT routed; gos_particle_bridge.cpp / gos_vfx_mesh_bridge.cpp hand-set state).
    // Shared invariant (source-verified): depthTest on, depth-WRITE OFF, reverse-Z
    // GEQUAL, cull None (double-sided), frontFace Ccw, color0 only (all 3 VFX frags
    // write a single output; no objectID — see vfx_no_objectid contract), FUNC_ADD.
    // The ONLY variable is blend. BlendMode::Additive is COARSE: it cannot encode
    // the SRC_ALPHA/ONE (billboard/mesh) vs ONE/ONE (tube) divergence — the SCHEMA
    // blendState carries the exact src/dst and the checker enforces it. Until
    // BLENDMODE-ADDITIVE-VOCABULARY-1 splits the enum, do NOT route these.

    // [11] VfxBillboardAlpha — particle_billboard, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AlphaBlend,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },
    // [12] VfxBillboardAdditive — particle_billboard, SRC_ALPHA/ONE (schema-exact)
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AdditiveSrcAlphaOne, // SRC_ALPHA/ONE
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },
    // [13] VfxTubeAlpha — tube_ribbon, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AlphaBlend,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },
    // [14] VfxTubeAdditive — tube_ribbon, ONE/ONE (DIFFERS from billboard/mesh; schema-exact)
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AdditiveOneOne, // ONE/ONE (differs from billboard/mesh)
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },
    // [15] VfxMeshAlpha — vfx_mesh, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AlphaBlend,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },
    // [16] VfxMeshAdditive — vfx_mesh, SRC_ALPHA/ONE (schema-exact)
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AdditiveSrcAlphaOne, // SRC_ALPHA/ONE (same as billboard)
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },
    // [17] PostProcessComposite — endScene final composite (postprocess.frag).
    // Opaque (force no blend; fully overwrites the backbuffer), depth test+write
    // OFF, cull None, color0 only. glProgramName=0 -> the call site keeps binding
    // compositeProg_; applyPipeline only sets the fixed-function state.
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ false,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::Always,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },
    // [18] PostProcessEdgeFog — runEdgeFog (edge_fog.frag). AlphaBlend
    // (SRC_ALPHA/ONE_MINUS_SRC_ALPHA), depth test+write OFF, cull None, color0.
    // glProgramName=0 -> call site keeps binding edgeFogProg_.
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AlphaBlend,
        /* depthTestEnable     */ false,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::Always,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },
    // [19] PostProcessFogOob — runFogOob (fog_oob.frag). Same state as EdgeFog.
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AlphaBlend,
        /* depthTestEnable     */ false,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::Always,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },
    // [20] TerrainSolid — main solid terrain (live = GPU-indirect thin MDI,
    // gos_terrain_thin.vert + gos_terrain.frag). DESCRIPTIVE ONLY (glProgramName=0,
    // NOT routed — state still hand-set in gos_terrain_bridge_drawIndirect). Opaque,
    // depthTest+write ON, GEQUAL reverse-Z. cullMode=None / frontFace=Ccw are
    // EMPIRICALLY PROBED (TERRAIN-CULL-STATE-PROBE-1: cull DISABLED, CCW, 8 frames
    // unanimous at the dispatch chokepoint) — not guessed from water/prose. The frag
    // writes color0 (FragColor) + color1 (GBuffer1) -> {true,true,false}; objectId
    // (color2) NOT written.
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* ssboBindingsMask    */ 0u,
    },

}};

static_assert(
    s_descs.size() == static_cast<size_t>(PipelineId::Count_),
    "s_descs row count must match PipelineId::Count_. "
    "Add a row for every new PipelineId value.");

// Returned for Invalid / out-of-range lookups.
static const PipelineDesc s_nullDesc{};

const PipelineDesc& getPipelineDesc(PipelineId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return s_nullDesc;
    return s_descs[idx];
}

void bindProgram(PipelineId id, uint32_t glProgramName) {
    const auto idx = static_cast<size_t>(id);
    assert(idx > 0u && idx < static_cast<size_t>(PipelineId::Count_) &&
           "bindProgram: invalid PipelineId");
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return;
    s_descs[idx].glProgramName = glProgramName;
}

// SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1: per-PipelineId logical variant key.
static std::array<std::string, static_cast<size_t>(PipelineId::Count_)> s_variantKeys;

void recordPipelineVariantKey(PipelineId id, const char* variantKey) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return;
    s_variantKeys[idx] = variantKey ? variantKey : "";
}

const char* getPipelineVariantKey(PipelineId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return "";
    return s_variantKeys[idx].c_str();
}

// VERTEXLAYOUT-AUTHORITY-1: canonical layout-name table, indexed by
// VertexLayoutId. MUST stay lockstep with the enum's "// layout:" comments and
// the pipeline-key schema (cross-checked by scripts/check-pipeline-key.py).
static constexpr const char* kVertexLayoutNames[] = {
    /* [0] Invalid          */ "",
    /* [1] StaticProp40B    */ "static_prop_40B",
    /* [2] MechGpuVertex48B */ "mech_GpuMechVertex_48B",
};
static_assert(
    sizeof(kVertexLayoutNames) / sizeof(kVertexLayoutNames[0]) ==
        static_cast<size_t>(VertexLayoutId::Count_),
    "kVertexLayoutNames must have one entry per VertexLayoutId value.");

const char* vertexLayoutName(VertexLayoutId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(VertexLayoutId::Count_))
        return "";
    return kVertexLayoutNames[idx];
}

// Per-PipelineId vertex-layout identity. Value-initialized to Invalid (0).
static std::array<VertexLayoutId, static_cast<size_t>(PipelineId::Count_)> s_vertexLayouts;

void recordPipelineVertexLayout(PipelineId id, VertexLayoutId layout) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return;
    s_vertexLayouts[idx] = layout;
}

VertexLayoutId getPipelineVertexLayout(PipelineId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return VertexLayoutId::Invalid;
    return s_vertexLayouts[idx];
}

} // namespace RenderCore
