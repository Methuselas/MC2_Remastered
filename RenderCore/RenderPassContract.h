// RenderCore/RenderPassContract.h
//
// Header-only DESCRIPTIVE registry of major render-pass lanes.
//
// Mirrors the table-of-facts style of RendererFeatureRegistry.h:
//   - feature registry = "what env-gated features exist"
//   - pipeline registry = "what GL state combos exist for a single draw"
//   - PASS contract (this file) = "what logical pass lanes exist, who owns them,
//     and how closed-up each one is on the snapshot / ViewUniforms / PipelineDesc
//     migration axes"
//
// NOT a scheduler. NOT a render graph. NO execute() callbacks. NO dispatch
// routing. The imperative frame loop continues to call each pass-owner's draw
// functions directly. This registry exists for inspection, audit, and
// migration tracking only.
//
// Firewall: header-only, no GL includes, no game-side includes. Pure POD +
// constexpr.
//
// Adding a pass:
//   1. Append a new RenderPassId enum value before COUNT (never renumber).
//   2. Append a kRenderPassContracts[] entry with the same id, in the same
//      order. The static_assert at the bottom enforces parity.
//   3. Fill all fields with current shipped state -- DO NOT aspirational-flag.
//      If PipelineDesc is not yet registered for the pass, write false.

#pragma once

#include <cstdint>

namespace RenderCore {

// ---------------------------------------------------------------------------
// Pass ids
// ---------------------------------------------------------------------------
// Values are stable -- never renumber, only append before COUNT.

enum class RenderPassId : uint32_t {
    StaticPropOpaque = 1,
    Terrain          = 2,
    MechOpaque       = 3,
    Shadow           = 4,
    VFX              = 5,
    COUNT            = 5,
};

// ---------------------------------------------------------------------------
// Contract entry (descriptive)
// ---------------------------------------------------------------------------

struct RenderPassContract {
    RenderPassId id;
    const char*  name;                      // human-readable
    const char*  ownerSubsystem;            // e.g. "GpuStaticPropBatcher"
    bool         viewUniformsBound;         // consumes binding=3 ViewUniforms UBO
    bool         pipelineDescRegistered;    // routes through PipelineDesc/Registry
    bool         snapshotRowAuthoritative;  // RenderSnapshot is the authority
    const char*  inspectorSectionId;        // ImGui CollapsingHeader label (incl. ##tag if any)
    const char*  killSwitchEnv;             // env var; nullptr if none
    const char*  notes;                     // optional one-liner
};

// ---------------------------------------------------------------------------
// Pass contract table
// ---------------------------------------------------------------------------
// Values reflect SHIPPED state at branch tip 1d7b9ea6. Update when a pass
// flips a closure axis (e.g. when terrain ViewUniforms ships, set
// viewUniformsBound=true here in the same slice).

static constexpr RenderPassContract kRenderPassContracts[] = {
    {
        RenderPassId::StaticPropOpaque,
        "StaticPropOpaque",
        "GpuStaticPropBatcher",
        /*viewUniformsBound*/        true,
        /*pipelineDescRegistered*/   true,
        /*snapshotRowAuthoritative*/ true,
        "StaticProp",
        "MC2_SNAPSHOT_STATIC_PROP_BUILD",
        "Reference path: snapshot-owned v6 DrawPacket+meta dispatch default-on (STATIC-PROP-V3-FLIP 2a88a5a8)."
    },
    {
        RenderPassId::Terrain,
        "Terrain",
        "TerrainPatchStream",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   false,
        /*snapshotRowAuthoritative*/ false,
        "Terrain Pass##tp",
        nullptr,
        "TerrainPassFacts row landed in RenderSnapshot at 1d7b9ea6 as passive recorder; not yet authoritative."
    },
    {
        RenderPassId::MechOpaque,
        "MechOpaque",
        "GpuMechBatcher",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   false,
        /*snapshotRowAuthoritative*/ true,
        "Mech",
        "MC2_SNAPSHOT_MECH_EXTRACT",
        "Mech rows extracted to snapshot (MECH-EXTRACTION-0); pipeline still legacy."
    },
    {
        RenderPassId::Shadow,
        "Shadow",
        "gosPostProcess + per-lane shadow programs",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   false,
        /*snapshotRowAuthoritative*/ false,
        "Shadow Pass##sp",
        nullptr,
        "Three shadow lanes (terrain/mech/static-prop); counters live-read from inspectors."
    },
    {
        RenderPassId::VFX,
        "VFX",
        "mc2::particles::Batcher",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   false,
        /*snapshotRowAuthoritative*/ false,
        "VFX Pass##vfx",
        nullptr,
        "Object-ID PROHIBITED. GpuTrailKind {None, MissileSmoke, PpcBolt}."
    },
};

static constexpr int kRenderPassContractCount =
    sizeof(kRenderPassContracts) / sizeof(kRenderPassContracts[0]);

static_assert(
    kRenderPassContractCount == static_cast<int>(RenderPassId::COUNT),
    "kRenderPassContracts length must match RenderPassId::COUNT");

} // namespace RenderCore
