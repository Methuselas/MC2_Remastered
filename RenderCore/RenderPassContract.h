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
//   1. Append a new RenderPassId enum value BEFORE _SentinelLast (never
//      renumber). The sentinel auto-tracks the count -- no hand-set COUNT
//      to forget to update.
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
// Values are stable -- never renumber, only append before _SentinelLast.
//
// Sentinel-after-last pattern: the count is derived from _SentinelLast's
// position, so appending a new pass id cannot silently desync a hand-set
// COUNT. The static_assert below still enforces that kRenderPassContracts[]
// length matches; this enum change closes the orthogonal "enum-value drift"
// hole.

enum class RenderPassId : uint32_t {
    StaticPropOpaque = 1,
    Terrain          = 2,
    MechOpaque       = 3,
    Shadow           = 4,
    VFX              = 5,
    // KEEP _SentinelLast AT THE END. New pass ids must be added BEFORE it.
    _SentinelLast,
};

// Derived count of real (non-sentinel) pass ids. Pass ids start at 1, so
// subtract 1 from the sentinel's underlying value.
constexpr uint32_t kRenderPassIdCount =
    static_cast<uint32_t>(RenderPassId::_SentinelLast) - 1u;

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
//
// CONTRIBUTOR NOTE:
//   When a closure axis flips for an existing pass (e.g. terrain begins
//   consuming ViewUniforms, a pass gains a kill-switch env var, or a pass
//   migrates from live state to snapshot-authoritative dispatch), update
//   the corresponding row in kRenderPassContracts in the SAME commit that
//   makes the change. Stale booleans here will silently mis-report closure
//   state in the editor inspector and in docs/engine-closure-audit.md.
//   The static_assert below catches array-length drift but NOT field-value
//   staleness -- that is on you.

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
        /*viewUniformsBound*/        true,
        /*pipelineDescRegistered*/   true,
        /*snapshotRowAuthoritative*/ true,
        "Mech",
        "MC2_SNAPSHOT_MECH_EXTRACT",
        "Wired through PipelineId::MechOpaque (MECH-PIPELINEDESC-1, applyPipeline) "
        "+ ViewUniforms UBO consumer default-on (MECH-VIEWUNIFORMS)."
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
    kRenderPassContractCount == static_cast<int>(kRenderPassIdCount),
    "kRenderPassContracts length must match kRenderPassIdCount "
    "(append the new RenderPassContract row when you append a RenderPassId).");

} // namespace RenderCore
