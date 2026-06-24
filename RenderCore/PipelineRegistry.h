// RenderCore/PipelineRegistry.h
//
// Named pipeline IDs and their PipelineDesc lookup.
// Not consumed by live render paths yet — exists so DrawPacket emitters
// can carry a typed identity instead of pipelineId=0.
//
// Lifecycle:
//   1. Engine reads getPipelineDesc(id) for static GL-state contracts
//      (blend, depth, cull, attachments, ssboBindingsMask) at any time.
//   2. Renderer calls bindProgram(id, glName) once during GL init to wire
//      the actual program object into the table.
//   3. DrawPacket::pipelineId = static_cast<uint32_t>(PipelineId::Foo).
//
// Extending: add enum values before Count_, then add a matching row to
// the s_descs initializer in PipelineRegistry.cpp. The static_assert in
// that file will catch any mismatch at compile time.

#pragma once

#include <cstdint>
#include "PipelineDesc.h"

namespace RenderCore {

enum class PipelineId : uint32_t {
    Invalid             = 0,
    StaticPropOpaque    = 1,   // opaque geometry, alpha-off group
    StaticPropAlphaTest = 2,   // alpha-tested geometry, alpha-on group (shader discard)
    MechOpaque          = 3,   // GPU mech batcher opaque pass (reverse-Z, cull back)
    StaticPropDepth     = 4,   // camera depth-prepass (depth-only, alpha discard)
    // SHADOW-CASTER-PIPELINE-REGISTRATION-1: shadow-map caster passes. DESCRIPTIVE
    // ONLY — these rows state the truth (depth-only, GL_LESS forward-Z, cull off)
    // but are NOT routed through applyPipeline (pipelineDescRegistered stays false
    // in RenderPassContract). They exist so polygonOffsetEnable is an authoritative
    // per-pipeline fact. Active applyPipeline routing is a later, gated slice.
    ShadowTerrain       = 5,   // terrain -> static shadow map  (shadow_terrain)
    ShadowStaticProp    = 6,   // static/dynamic props -> shadow map (shadow_static_prop; polygon offset ON)
    ShadowMech          = 7,   // mech -> dynamic shadow map     (shadow_mech)
    // TERRAIN-OVERLAY-DECAL-PIPELINE-REGISTRATION-1: terrain overlay + decal
    // passes. DESCRIPTIVE ONLY (glProgramName=0, NOT routed through applyPipeline;
    // state still hand-set in gosRenderer::drawTerrainOverlays / drawDecals). The
    // rows state the truth so the pass-coverage ledger can move them forward.
    // TerrainDecal is the first AlphaBlend pipeline row.
    TerrainOverlay      = 8,   // terrain cement/perimeter overlay (terrain_overlay.frag, opaque)
    TerrainDecal        = 9,   // bomb craters + mech footprints (decal.frag, alpha blend, depth-write OFF)
    // WATER-ARMED-PIPELINE-REGISTRATION-1: the ARMED water fast-path base only
    // (gosRenderer::renderWaterFastPath, gos_terrain_water_fast.vert +
    // gos_tex_vertex.frag). DESCRIPTIVE (glProgramName=0, NOT routed). The legacy
    // quad fallback + the GPU-driven MDI sub-variant are deliberately NOT modeled.
    WaterArmed          = 10,  // armed water fast path (alpha blend, cull none, GEQUAL, depth-write)
    // VFX-PIPELINE-REGISTRATION-1: the finite particle/VFX family. DESCRIPTIVE
    // (glProgramName=0, NOT routed). 3 programs x AlphaBlend/Additive. Shared
    // invariant: depthTest on, GEQUAL, depth-write OFF, cull None, color0 only,
    // FUNC_ADD. The ADDITIVE factors DIVERGE per program — billboard/mesh use
    // SRC_ALPHA/ONE, tube uses ONE/ONE; BlendMode::Additive cannot distinguish
    // them, so the SCHEMA carries the exact src/dst and the checker treats those
    // as authoritative. Routing waits on BLENDMODE-ADDITIVE-VOCABULARY-1.
    VfxBillboardAlpha    = 11,  // particle_billboard, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    VfxBillboardAdditive = 12,  // particle_billboard, SRC_ALPHA/ONE
    VfxTubeAlpha         = 13,  // tube_ribbon, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    VfxTubeAdditive      = 14,  // tube_ribbon, ONE/ONE  (differs from billboard/mesh)
    VfxMeshAlpha         = 15,  // vfx_mesh, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    VfxMeshAdditive      = 16,  // vfx_mesh, SRC_ALPHA/ONE
    // POSTPROCESS-COMPOSITE-REGISTRATION-1: endScene final composite (fullscreen
    // quad, postprocess.frag). ROUTED via applyPipeline. Opaque (force no blend —
    // composite must fully overwrite the backbuffer; a gosFX additive leak would
    // otherwise saturate RGBA8 to white), depth test+write OFF, cull None, color0.
    PostProcessComposite = 17,  // endScene composite (postprocess.frag, opaque, no depth)
    // POSTPROCESS-FOG-REGISTRATION-1: the two fullscreen fog passes that blend into
    // the scene FBO before composite. Both AlphaBlend (SRC_ALPHA/ONE_MINUS_SRC_ALPHA),
    // depth test+write OFF, cull None, color0. ROUTED via applyPipeline. State-twins
    // (separate rows for per-pass [PIPELINE_BIND] trace identity).
    PostProcessEdgeFog   = 18,  // runEdgeFog (edge_fog.frag, alpha blend, no depth)
    PostProcessFogOob    = 19,  // runFogOob (fog_oob.frag, alpha blend, no depth)
    // Future: DebugWireframe, ...
    Count_               = 20,  // sentinel — do not use as an ID
};

// VERTEXLAYOUT-AUTHORITY-1: stable repo-owned vertex-input layout identities —
// the VAO half of a PSO key. Each value names a fixed, finite vertex layout used
// by a registered pipeline. The canonical layout-name token in the trailing
// "// layout: <name>" comment is AUTHORITATIVE: scripts/check-pipeline-key.py
// parses it and cross-checks it against the pipeline-key schema's per-pipeline
// vertexLayoutId/vertexLayout. Renaming a value or its layout string without
// updating the schema (or vice versa) FAILS the pipeline_key gate.
enum class VertexLayoutId : uint32_t {
    Invalid          = 0,
    StaticProp40B    = 1,   // layout: static_prop_40B (gos_static_prop_batcher.cpp:2310-2323)
    MechGpuVertex48B = 2,   // layout: mech_GpuMechVertex_48B (gos_mech_batcher.cpp:1376-1382)
    Count_           = 3,   // sentinel — do not use as an ID
};

// Canonical layout-name string for id (the same token recorded in the
// pipeline-key schema). Returns "" for Invalid / out-of-range.
const char* vertexLayoutName(VertexLayoutId id);

// Return the static GL-state contract for id.
// glProgramName is 0 until bindProgram() is called for that id.
// Out-of-range or Invalid returns a zeroed sentinel (all false, all 0).
const PipelineDesc& getPipelineDesc(PipelineId id);

// Wire in the actual GL program object name from the renderer at GL init.
// Must be called before any DrawPacket using this id is dispatched.
// No-op (with assert) if id is Invalid or out-of-range.
void bindProgram(PipelineId id, uint32_t glProgramName);

// SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1: record the selected logical
// shader-variant identity (the canonical define-set key, e.g.
// "mech|MC2_OBJECT_ID_BUFFER=1;MC2_USE_VIEW_UNIFORMS=1") for this pipeline id,
// into the pipeline-key path. Derived from the program PREFIX, so it is the SAME
// whether the stages were GLSL-compiled or SPIR-V-specialized (the SPIR-V branch
// lives per-stage BELOW the bound program and cannot fork this accounting).
// Empty until recorded; safe to call once at program build.
void recordPipelineVariantKey(PipelineId id, const char* variantKey);
const char* getPipelineVariantKey(PipelineId id);

// VERTEXLAYOUT-AUTHORITY-1: record the vertex-input layout identity bound for
// this pipeline id, into the pipeline-key path. Promotes vertexLayout from a
// descriptive doc string to a recorded + checked axis. Pure metadata — does NOT
// touch GL state, the VAO, or any draw. The MechOpaque PipelineKey is now
// (shaderVariantId + vertexLayoutId). Invalid until recorded.
void recordPipelineVertexLayout(PipelineId id, VertexLayoutId layout);
VertexLayoutId getPipelineVertexLayout(PipelineId id);

} // namespace RenderCore
