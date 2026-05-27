// RenderCore/RendererFeatureRegistry.h
//
// Header-only inventory of renderer feature gates and device capabilities.
// INVENTORY ONLY -- no feature decisions here. Each system's own env-flag
// accessor (e.g. RenderWorld::IsObjectIdBufferEnabled, g_useGpuMechs) remains
// authoritative for runtime decisions. This header is the single place to
// look up what env vars exist, what their defaults are, and what GL caps
// the engine requires per feature.
//
// Enforcement: scripts/check-env-registry.sh greps this file for all
// "MC2_[A-Z_]+" string literals. Any MC2_* var used in source that does not
// appear here AND is not in the script's ALLOWLIST triggers a CI failure.
// Adding a new env var: register it here first, then add the getenv() call.
//
// Usage:
//   DeviceCaps caps{};
//   fillDeviceCaps(caps);             // caller provides; needs GL context
//   debugDumpFeatureRegistry(caps);   // stderr banner at startup
//
// Firewall: MUST NOT include any GL header or game-side header.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace RenderCore {

// ---------------------------------------------------------------------------
// Device capabilities
// ---------------------------------------------------------------------------
// POD struct; caller fills it with GL queries after context creation.
// Zero-initialised default = "unknown / not queried yet".
// Nothing in this header calls GL.

struct DeviceCaps {
    int  glMajor              = 0;   // GL_MAJOR_VERSION
    int  glMinor              = 0;   // GL_MINOR_VERSION

    // Extension / promotion points
    bool hasSSBO              = false;  // GL 4.3 / ARB_shader_storage_buffer_object
    bool hasTessellation      = false;  // GL 4.0 / ARB_tessellation_shader
    bool hasComputeShaders    = false;  // GL 4.3 / ARB_compute_shader
    bool hasIndirectDraw      = false;  // GL 4.0 / ARB_draw_indirect
    bool hasBindlessTextures  = false;  // ARB_bindless_texture (AMD: driver-version-gated)
    bool hasClipControl       = false;  // GL 4.5 / ARB_clip_control (load-bearing for reverse-Z)
    bool hasDSA               = false;  // GL 4.5 / ARB_direct_state_access

    // Limits
    int  maxColorAttachments  = 0;  // GL_MAX_COLOR_ATTACHMENTS
    int  maxSSBOBindings      = 0;  // GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
    int  maxTessGenLevel      = 0;  // GL_MAX_TESS_GEN_LEVEL
};

// ---------------------------------------------------------------------------
// Env-var kind
// ---------------------------------------------------------------------------
// Used by EnvVarDesc to categorise entries in both tables below.

enum class EnvVarKind : uint8_t {
    Feature  = 0,  // gates a render code path (runtime behavior change)
    Trace    = 1,  // diagnostic output only; no correctness effect
    Retired  = 2,  // still in code; NO new consumers; superseded by RenderWorld API
};

// ---------------------------------------------------------------------------
// Env-var descriptor
// ---------------------------------------------------------------------------

struct EnvVarDesc {
    const char* featureId;   // "MC2_FEATURE_*" canonical name (or "MC2_TRACE_*" etc.)
    const char* envVar;      // env var string; nullptr = always-on (no runtime gate)
    EnvVarKind  kind;
    bool        defaultOn;   // true if active when env var is absent / unset
    const char* doc;         // one-line rationale / owner
};

// ---------------------------------------------------------------------------
// Feature enum
// ---------------------------------------------------------------------------
// Only Feature-kind and Retired-kind entries appear here (not Trace).
// Values are stable -- never renumber, only append before COUNT.
// Retired entries stay in the enum so existing code that indexes kFeatureTable
// by RendererFeature::GpuMechs etc. continues to compile.

enum class RendererFeature : int {
    // [Retired] superseded by RenderWorld::registerMech; no new getenv consumers
    GpuMechs            = 0,   // MC2_GPU_MECHS
    // [Retired] superseded by RenderWorld::upsertStaticProp; no new getenv consumers
    StaticPropIndirect  = 1,   // MC2_GPU_OBJECTS
    // Feature gates
    ObjectIdBuffer      = 2,   // MC2_OBJECT_ID_BUFFER
    TerrainTessellation = 3,   // (no env var -- always-on TCS/TES)
    ReverseZ            = 4,   // (no env var -- glClipControl unconditional)
    ShadowMaps          = 5,   // MC2_SHADOW_ENABLE
    ImGui               = 6,   // MC2_IMGUI
    ImGuiInspector      = 7,   // MC2_IMGUI_INSPECTOR
    DebugRenderer       = 8,   // MC2_DEBUG_RENDERER
    MaterialGpu         = 9,   // MC2_MATERIAL_GPU
    MaterialGpuSample   = 10,  // MC2_MATERIAL_GPU_SAMPLE
    StaticPropRegistry  = 11,  // MC2_STATIC_PROP_REGISTRY
    MaterialKtx         = 12,  // MC2_MATERIAL_KTX
    // v6 DrawPacket+Meta dispatch. MC2_STATIC_PROP_LEGACY_DISPATCH=1 forces legacy fallback (kill-switch).
    StaticPropPacketDispatch = 13,  // MC2_STATIC_PROP_LEGACY_DISPATCH
    // [Retired] v6 opt-in gate removed in v7.1; setting this var is inert.
    DrawPacketStaticPropV6   = 14,  // MC2_DRAW_PACKET_STATIC_PROP_V6
    // Extraction v3: build v6Packets+meta from RenderSnapshot rows instead of live batcher state.
    RenderSnapshotBuild      = 15,  // MC2_SNAPSHOT_STATIC_PROP_BUILD
    // Extraction v2.3: skip prev-frame zero-instance snapshot slots (snap-cull opt-in).
    SnapCull                 = 16,  // MC2_SNAP_CULL
    // F1-3A ViewUniforms UBO upload (upload-only; F1-3B adds shader consumption).
    ViewUniforms             = 17,  // MC2_VIEW_UNIFORMS
    // MECH-EXTRACTION-0: mech snapshot extraction (observe-only).
    SnapshotMechExtract      = 18,  // MC2_SNAPSHOT_MECH_EXTRACT
    COUNT                    = 19,
};

// ---------------------------------------------------------------------------
// Feature table (kFeatureTable)
// ---------------------------------------------------------------------------
// Indexed by static_cast<int>(RendererFeature::*).
// static constexpr: one copy per TU (small struct; fine).

static constexpr EnvVarDesc kFeatureTable[] = {
    // GpuMechs [Retired]
    {
        "MC2_FEATURE_GPU_MECHS",
        "MC2_GPU_MECHS",
        EnvVarKind::Retired,
        true,
        "[RETIRED] GPU mech batcher (gos_mech_batcher). Superseded by RenderWorld::registerMech. No new getenv consumers."
    },
    // StaticPropIndirect [Retired]
    {
        "MC2_FEATURE_STATIC_PROP_INDIRECT",
        "MC2_GPU_OBJECTS",
        EnvVarKind::Retired,
        false,
        "[RETIRED] Indirect-draw static-prop batcher (gos_static_prop_batcher). Superseded by RenderWorld::upsertStaticProp."
    },
    // ObjectIdBuffer
    {
        "MC2_FEATURE_OBJECT_ID_BUFFER",
        "MC2_OBJECT_ID_BUFFER",
        EnvVarKind::Feature,
        false,
        "R32_UINT MRT attachment-2 with per-pixel object handles (RenderWorld M1.5). Default-off; =1 enables."
    },
    // TerrainTessellation
    {
        "MC2_FEATURE_TERRAIN_TESSELLATION",
        nullptr,
        EnvVarKind::Feature,
        true,
        "TCS/TES terrain tessellation (gameos_graphics.cpp). Always-on; no kill-switch yet."
    },
    // ReverseZ
    {
        "MC2_FEATURE_REVERSE_Z",
        nullptr,
        EnvVarKind::Feature,
        true,
        "Reverse-Z depth via glClipControl GL_ZERO_TO_ONE (gameosmain.cpp:992). Always-on; MC2_REVERSE_Z_TRACE for trace."
    },
    // ShadowMaps
    {
        "MC2_FEATURE_SHADOW_MAPS",
        "MC2_SHADOW_ENABLE",
        EnvVarKind::Feature,
        false,
        "Shadow map pre-pass and PCF sampling in mech/static-prop paths. Default-off; =1 enables."
    },
    // ImGui
    {
        "MC2_FEATURE_IMGUI",
        "MC2_IMGUI",
        EnvVarKind::Feature,
        false,
        "ImGui overlay (GuiRuntime/GuiRuntime.cpp). Default-off; =1 enables. Editor sets this automatically."
    },
    // ImGuiInspector
    {
        "MC2_FEATURE_IMGUI_INSPECTOR",
        "MC2_IMGUI_INSPECTOR",
        EnvVarKind::Feature,
        false,
        "ImGui inspector panel (GuiRuntime/EditorInspector.cpp). Default-off; =1 enables. Requires MC2_IMGUI."
    },
    // DebugRenderer
    {
        "MC2_FEATURE_DEBUG_RENDERER",
        "MC2_DEBUG_RENDERER",
        EnvVarKind::Feature,
        false,
        "Debug overlay renderer (GuiRuntime/EditorInspector.cpp). Default-off; =1 enables. Requires MC2_IMGUI_INSPECTOR."
    },
    // MaterialGpu
    {
        "MC2_FEATURE_MATERIAL_GPU",
        "MC2_MATERIAL_GPU",
        EnvVarKind::Feature,
        true,
        "GPU material table upload/bind/compare for static props (default-ON as of v5, 2026-05-26). Set =0 to disable."
    },
    // MaterialGpuSample
    {
        "MC2_FEATURE_MATERIAL_GPU_SAMPLE",
        "MC2_MATERIAL_GPU_SAMPLE",
        EnvVarKind::Feature,
        true,
        "GPU material albedo sampling in static_prop.frag (default-ON as of v7, 2026-05-26). Set =0 to fall back to texArrayLayer. Requires MC2_MATERIAL_GPU."
    },
    // StaticPropRegistry
    {
        "MC2_FEATURE_STATIC_PROP_REGISTRY",
        "MC2_STATIC_PROP_REGISTRY",
        EnvVarKind::Feature,
        true,
        "GpuStaticPropRegistry enable. Default-on; editor sets =0 (EditorMFC.cpp) to bypass registry for edit-time mutations."
    },
    // MaterialKtx
    {
        "MC2_FEATURE_MATERIAL_KTX",
        "MC2_MATERIAL_KTX",
        EnvVarKind::Feature,
        false,
        "KTX2 sidecar loader for static-prop textures (RenderCore/KtxLoader). Phase 0: RGBA8 only. Default-off; =1 enables. Requires MC2_COALESCE=1."
    },
    // StaticPropPacketDispatch
    {
        "MC2_FEATURE_STATIC_PROP_PACKET_DISPATCH",
        "MC2_STATIC_PROP_LEGACY_DISPATCH",
        EnvVarKind::Feature,
        true,
        "v6 DrawPacket+Meta dispatch (default-ON as of v7). MC2_STATIC_PROP_LEGACY_DISPATCH=1 forces legacy glMultiDrawElementsIndirect fallback. Kill-switch; env var absent = packet path active."
    },
    // DrawPacketStaticPropV6 [Retired]
    {
        "MC2_FEATURE_DRAW_PACKET_STATIC_PROP_V6",
        "MC2_DRAW_PACKET_STATIC_PROP_V6",
        EnvVarKind::Retired,
        false,
        "[RETIRED] v6 opt-in gate removed in v7.1. Gate plumbing deleted; setting this var has no effect. Kill-switch is MC2_STATIC_PROP_LEGACY_DISPATCH."
    },
    // RenderSnapshotBuild
    {
        "MC2_FEATURE_RENDER_SNAPSHOT_BUILD",
        "MC2_SNAPSHOT_STATIC_PROP_BUILD",
        EnvVarKind::Feature,
        false,
        "Extraction v3: build v6Packets+meta from RenderSnapshot rows instead of live batcher state (gos_static_prop_batcher.cpp). Default-off; =1 enables."
    },
    // SnapCull
    {
        "MC2_FEATURE_SNAP_CULL",
        "MC2_SNAP_CULL",
        EnvVarKind::Feature,
        false,
        "Extraction v2.3: skip prev-frame zero-instance slots during snapshot-based dispatch (snap-cull opt-in). Default-off; =1 enables. Requires snapshot path active."
    },
    // ViewUniforms
    {
        "MC2_FEATURE_VIEW_UNIFORMS",
        "MC2_VIEW_UNIFORMS",
        EnvVarKind::Feature,
        false,
        "ViewUniforms UBO at binding=3. Uploads per-frame view matrices (worldToClipGL, worldToViewGL, cameraWorldPos). F1-3A: upload only, no shader consumption. F1-3B shader consumption requires process restart (shaders compiled at startup). Default-off; =1 enables."
    },
    // SnapshotMechExtract
    {
        "MC2_FEATURE_SNAPSHOT_MECH_EXTRACT",
        "MC2_SNAPSHOT_MECH_EXTRACT",
        EnvVarKind::Feature,
        false,
        "MECH-EXTRACTION-0: mech snapshot extraction (observe-only, no GL mutation). Extracts ExtractedMechPacket[] from RenderSnapshot. Default-off; =1 enables."
    },
};

static_assert(
    sizeof(kFeatureTable) / sizeof(kFeatureTable[0]) == static_cast<int>(RendererFeature::COUNT),
    "kFeatureTable length must match RendererFeature::COUNT");

// ---------------------------------------------------------------------------
// Auxiliary env-var table (kAuxEnvVars)
// ---------------------------------------------------------------------------
// Trace, Infra, and other registered vars not indexed by RendererFeature.
// The enforcement script greps BOTH tables for "MC2_" string literals.

static constexpr EnvVarDesc kAuxEnvVars[] = {
    {
        "MC2_TRACE_RENDER_WORLD",
        "MC2_RENDER_WORLD_TRACE",
        EnvVarKind::Trace,
        false,
        "Per-frame [RENDER_WORLD v1] banner + per-event logs (RenderWorld.cpp). Default-off; =1 enables."
    },
};

// ---------------------------------------------------------------------------
// Debug dump
// ---------------------------------------------------------------------------
// Emits a [RENDERER_FEATURES v1] startup banner to stderr.
// Call once after fillDeviceCaps() and GL context creation.
// Prints Feature entries normally; Retired entries with [RETIRED] tag.
// Trace entries (kAuxEnvVars) are not printed here -- they are per-subsystem.

inline void debugDumpFeatureRegistry(const DeviceCaps& caps) {
    fprintf(stderr,
            "[RENDERER_FEATURES v1] gl=%d.%d"
            " ssbo=%d tess=%d compute=%d indirect=%d"
            " bindless=%d clipctrl=%d dsa=%d"
            " max_color_attach=%d max_ssbo_bind=%d max_tess_level=%d\n",
            caps.glMajor, caps.glMinor,
            (int)caps.hasSSBO, (int)caps.hasTessellation,
            (int)caps.hasComputeShaders, (int)caps.hasIndirectDraw,
            (int)caps.hasBindlessTextures, (int)caps.hasClipControl,
            (int)caps.hasDSA,
            caps.maxColorAttachments, caps.maxSSBOBindings, caps.maxTessGenLevel);

    for (int i = 0; i < static_cast<int>(RendererFeature::COUNT); ++i) {
        const EnvVarDesc& d = kFeatureTable[i];
        const char* envStr = d.envVar ? d.envVar : "(none)";
        const char* curStr;
        if (d.kind == EnvVarKind::Retired) {
            curStr = "[RETIRED]";
        } else if (!d.envVar) {
            curStr = "always-on";
        } else {
            const char* v = std::getenv(d.envVar);
            if (!v || !v[0]) {
                curStr = d.defaultOn ? "on(default)" : "off(default)";
            } else if (v[0] == '0') {
                curStr = "off(forced)";
            } else {
                curStr = "on(forced)";
            }
        }
        fprintf(stderr,
                "[RENDERER_FEATURES v1]   %-38s env=%-32s %s\n",
                d.featureId, envStr, curStr);
    }
}

} // namespace RenderCore
