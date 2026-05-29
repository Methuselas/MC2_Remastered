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
    // V-AMBIENT-STATIC-1: hemisphere ambient fill on StaticPropOpaque lane.
    // Default-OFF; strength uniform = 0.0 when env unset/0 -> byte-identical
    // to pre-slice output.
    StaticPropAmbientV1      = 19,  // MC2_STATIC_PROP_AMBIENT_V1
    // V-MATERIAL-DEBUG-1: per-fragment material debug view (StaticPropOpaque).
    // Default-OFF; 0 = byte-identical to legacy output (shader short-circuit).
    StaticPropDebugMaterial  = 20,  // MC2_STATIC_PROP_DEBUG_MATERIAL
    // V-IBL-STATIC-1: SH-L2 image-based ambient on StaticPropOpaque lane.
    // Default-ON; MC2_STATIC_PROP_IBL_SH=0 uploads strength 0.0 so
    // shader `if (u_iblShStrength > 0.0)` short-circuits to byte-identical.
    // ImGui slider (g_iblShStrength) modulates strength when env gate is on;
    // the env var is the authoritative gate.
    StaticPropIblSh          = 21,  // MC2_STATIC_PROP_IBL_SH
    // V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel + power-lobe specular
    // on StaticPropOpaque lane. Default-OFF; strength uniform = 0.0 when
    // env unset/=0 -> shader `if (u_pbrV1Strength > 0.0)` short-circuits.
    // Uses MaterialGpu roughness/metallic when sampling is active, else
    // fallback metallic=0.0 roughness=0.6; F0 is albedo-tinted for metals.
    StaticPropPbrV1          = 22,  // MC2_STATIC_PROP_PBR_V1
    // TERRAIN-NORMALS-FROM-HEIGHT-1: gated macroscopic surface normal derived
    // from per-mission R32F height texture in gos_terrain.frag. Default-OFF.
    TerrainNormalsFromHeight = 23,  // MC2_TERRAIN_NORMALS_FROM_HEIGHT
    // TERRAIN-LIGHTING-1: hemisphere ambient fill on tessellated terrain
    // using terrain surface normal. Default-OFF.
    TerrainLightingV1        = 24,  // MC2_TERRAIN_LIGHTING_V1
    // TERRAIN-LIGHTING-2: shadow-aware modulation of the V1 hemisphere
    // fill — prevents over-bright shadows. Default-OFF (V1 unmodulated).
    TerrainLightingV2        = 25,  // MC2_TERRAIN_LIGHTING_V2
    // VFX-AGE-SAMPLE-1: sample GPU-particle spec curves at the effect's real
    // CPU-advanced m_age instead of the fixed 0.5 snapshot. Default-OFF.
    VfxAgeSample             = 26,  // MC2_VFX_AGE_SAMPLE
    COUNT                    = 27,
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
        "Dynamic object shadow caster pass (GpuStaticPropBatcher + GpuMechBatcher flushShadow). Opt-in MC2_SHADOW_ENABLE=1. VAO restore order fixed (SHADOW-DYNAMIC-RESTORE-1) but default-on causes prop regression; root cause under investigation."
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
        true,
        "Extraction v3: build v6Packets+meta from RenderSnapshot rows instead of live batcher state (gos_static_prop_batcher.cpp). Default-ON as of STATIC-PROP-V3-FLIP (2026-05-27). Kill-switch: =0 forces OFF and falls back to live builder authority."
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
        true,
        "ViewUniforms UBO at binding=3. Default ON. =0 reverts to legacy uniform. Shader consumption (static_prop.vert) requires restart."
    },
    // SnapshotMechExtract
    {
        "MC2_FEATURE_SNAPSHOT_MECH_EXTRACT",
        "MC2_SNAPSHOT_MECH_EXTRACT",
        EnvVarKind::Feature,
        false,
        "MECH-EXTRACTION-0: mech snapshot extraction (observe-only, no GL mutation). Extracts ExtractedMechPacket[] from RenderSnapshot. Default-off; =1 enables."
    },
    // StaticPropAmbientV1
    {
        "MC2_FEATURE_STATIC_PROP_AMBIENT_V1",
        "MC2_STATIC_PROP_AMBIENT_V1",
        EnvVarKind::Feature,
        false,
        "V-AMBIENT-STATIC-1: hemisphere ambient fill on StaticPropOpaque lane (static_prop.vert). Default-off; =1 enables (uniform u_ambientV1Strength=1.0). When OFF, strength=0.0 -> byte-identical to legacy output. Skips window-flag nodes."
    },
    // StaticPropDebugMaterial
    {
        "MC2_FEATURE_STATIC_PROP_DEBUG_MATERIAL",
        "MC2_STATIC_PROP_DEBUG_MATERIAL",
        EnvVarKind::Feature,
        false,
        "V-MATERIAL-DEBUG-1 / V-MATERIAL-PBR-1: per-fragment material debug view on StaticPropOpaque lane (static_prop.frag). Default 0 = OFF (byte-identical via `if (u_debugMaterialMode != 0) return;` short-circuit). Modes 1=albedo, 2=materialIdx-palette, 3=worldNormal, 4=texArrayLayer-palette, 5=roughnessFactor (grayscale), 6=metallicFactor (grayscale). Set MC2_STATIC_PROP_DEBUG_MATERIAL=N (1..6)."
    },
    // StaticPropIblSh
    {
        "MC2_FEATURE_STATIC_PROP_IBL_SH",
        "MC2_STATIC_PROP_IBL_SH",
        EnvVarKind::Feature,
        true,
        "V-IBL-STATIC-1: SH-L2 image-based ambient on StaticPropOpaque lane (static_prop.vert). Default-ON (flipped 2026-05-27 per V-STATICPROP-VISUAL-REVIEW-AUDIT); =0 is explicit kill-switch (byte-identical to pre-flip OFF). When OFF, u_iblShStrength uploads 0.0 -> shader short-circuits before evalShL2. Coefficients from RenderCore/IblShCoeffs.h (projected from data/hdr/DaySkyHDRI063B_4K.exr). ImGui slider g_iblShStrength modulates per-frame strength (default 0.5); env var is authoritative gate."
    },
    // StaticPropPbrV1
    {
        "MC2_FEATURE_STATIC_PROP_PBR_V1",
        "MC2_STATIC_PROP_PBR_V1",
        EnvVarKind::Feature,
        false,
        "V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel + power-lobe specular on StaticPropOpaque lane (static_prop.frag, inside `#if defined(MC2_USE_VIEW_UNIFORMS)`). Default-OFF; =1 enables. When OFF, u_pbrV1Strength uploads 0.0 -> shader `if (u_pbrV1Strength > 0.0)` short-circuits before any u_cameraWorldPos read. Uses MaterialGpu roughnessFactor/metallicFactor when MC2_MATERIAL_GPU_SAMPLE is active; otherwise falls back to metallic=0.0 and roughness=0.6. F0 is albedo-tinted for metallic materials. Window/hot-pink nodes bypass the PBR branch. Safety interlock: when MC2_VIEW_UNIFORMS=0, CPU force-zeroes strength and the shader compile-guard excludes the block. Sun direction accepts TG_LIGHT_INFINITE and TG_LIGHT_INFINITEWITHFALLOFF. ImGui slider g_pbrV1Strength modulates per-frame strength (default 1.0, range 0..3); env var is authoritative gate. Optional override MC2_STATIC_PROP_PBR_V1_STRENGTH (clamped 0..3) sets the default."
    },
    // TerrainNormalsFromHeight
    {
        "MC2_FEATURE_TERRAIN_NORMALS_FROM_HEIGHT",
        "MC2_TERRAIN_NORMALS_FROM_HEIGHT",
        EnvVarKind::Feature,
        false,
        "TERRAIN-NORMALS-FROM-HEIGHT-1: gated macroscopic surface normal derived from a per-mission R32F height texture (gos_terrain_height_tex.cpp; uploaded at mission load from MapData heightfield). Default-OFF; =1 enables. When OFF, useTerrainNormalsFromHeight uploads 0 and gos_terrain.frag skips the height-derived perturbation branch entirely → byte-identical legacy output. Visual-only: gameplay height (Terrain::getTerrainElevation) is unchanged; no geometry or vertex position is moved. CPU plumbing: env var read once-per-terrain-uniform-upload (no restart needed). Inspector mini-control in Terrain Pass panel displays current effective state. Debug visualization: MC2_TERRAIN_DEBUG_MODE=10 shows the height-derived normal as RGB (independent of this gate so the upload path can be diagnosed separately). Sampler unit 11. Texture not bound when no mission is loaded."
    },
    // TerrainLightingV1
    {
        "MC2_FEATURE_TERRAIN_LIGHTING_V1",
        "MC2_TERRAIN_LIGHTING_V1",
        EnvVarKind::Feature,
        false,
        "TERRAIN-LIGHTING-1: gated hemisphere ambient fill on the tessellated terrain. Default-OFF; =1 enables. When OFF, terrainLightingV1Strength uploads 0.0 and gos_terrain.frag skips the additive branch → byte-identical legacy output. Adds sky/ground tinted ambient that fills shadowed terrain (added AFTER shadow multiplication so direct sun stays shadowed but bounce light continues). Best paired with MC2_TERRAIN_NORMALS_FROM_HEIGHT=1 — ambient verticality is derived from the final per-fragment normal (sky term scales with N.z). Strength tunable in-engine via Terrain Pass inspector slider (default 1.0); env gate is authoritative on/off. Visual-only; no gameplay, geometry, or collision change."
    },
    // TerrainLightingV2
    {
        "MC2_FEATURE_TERRAIN_LIGHTING_V2",
        "MC2_TERRAIN_LIGHTING_V2",
        EnvVarKind::Feature,
        false,
        "TERRAIN-LIGHTING-2: gated shadow-aware modulation of the TERRAIN-LIGHTING-1 hemisphere fill. Default-OFF; =1 enables. When OFF, terrainLightingV2ShadowFillFloor uploads 1.0 → the shader expression mix(floor, 1.0, shadow) collapses to 1.0 → V1 behavior preserved (byte-equivalent to TERRAIN-LIGHTING-1 alone). When ON, the member floor (default 0.3, ImGui-tunable 0..1 via the Graphics Options Terrain section) scales the hemi additive in shadowed terrain so dark areas stay dark: floor=0.3 = 30% hemi in fully shadowed terrain, 100% in fully lit terrain. floor=0.0 makes hemi follow shadow exactly (lifeless shadows); floor=1.0 = V1 unmodulated. Debug mode MC2_TERRAIN_DEBUG_MODE=11 visualizes the hemi additive contribution as RGB (×4 for visibility). Visual-only; no gameplay, geometry, or collision change. Effective only when MC2_TERRAIN_LIGHTING_V1 is also ON (since the floor multiplies the V1 additive)."
    },
    // VfxAgeSample
    {
        "MC2_FEATURE_VFX_AGE_SAMPLE",
        "MC2_VFX_AGE_SAMPLE",
        EnvVarKind::Feature,
        false,
        "VFX-AGE-SAMPLE-1: sample GPU-particle spec curves (color/alpha/size/UV) at the routed effect's real CPU-advanced normalized age (gosFX Effect::m_age, threaded into mc2::particles::Spawn from each producer Draw) instead of the fixed 0.5 midpoint. Default-OFF; =1 enables. When OFF, resolveSampleAge() returns 0.5 → byte-identical to the pre-slice snapshot. When ON, particles regain fade-in/out + grow/shrink because each per-frame re-emit samples at the effect's current age. Read-only consumption of m_age (already advanced by gameplay) — NO emission/lifetime/spawn-rate/timing change; NO shader or GpuParticle-ABI change (curve eval stays CPU-side in spawn_*.cpp). Invalid/sentinel age (m_age=-1) or out-of-[0,1] falls back to 0.5. Affects only the 5 routed classes (Card/CardCloud/PointCloud/ShardCloud/Tube); unrouted CPU-only classes untouched. Object-ID invariant preserved. Min/max age summary logged under MC2_GPU_PARTICLES_LOG=1."
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
    {
        "MC2_TRACE_SHADOW_FRUSTUM",
        "MC2_SHADOW_FRUSTUM_DIAG",
        EnvVarKind::Trace,
        false,
        "SHADOW-FRUSTUM-AUDIT-1: read-only per-frame dynamic sun-shadow coverage probe in buildDynamicLightMatrix (sun dir, frustum XY, fit/xy radius, map clamp, texel WU, ortho WxH, depth). Default-off; =1 enables. No behavior change."
    },
    {
        "MC2_FEATURE_SHADOW_BOUNDED_NEAR_FIT",
        "MC2_SHADOW_BOUNDED_NEAR_FIT",
        EnvVarKind::Feature,
        false,
        "SHADOW-BOUNDED-NEAR-FIT-1: cap the dynamic sun-shadow frustum fit radius to a small camera-centered region (radius from MC2_SHADOW_BOUNDED_NEAR_RADIUS) for higher texel density. Default-off; =1 enables. Gate OFF = byte-identical full-frustum fit. Trades far-map coverage for crisp near shadows. Applied before pow-2/texel snap (snap preserved)."
    },
    {
        "MC2_TUNE_SHADOW_BOUNDED_NEAR_RADIUS",
        "MC2_SHADOW_BOUNDED_NEAR_RADIUS",
        EnvVarKind::Trace,
        false,
        "SHADOW-BOUNDED-NEAR-FIT-1 tunable: bounded near-fit radius in world units (default 2500, clamped 512..mapClampR). Only consulted when MC2_SHADOW_BOUNDED_NEAR_FIT=1. Resolved once at process start."
    },
    {
        "MC2_FEATURE_SHADOW_STATIC_BUILDINGS",
        "MC2_STATIC_PROP_BUILDING_SHADOW",
        EnvVarKind::Feature,
        false,
        "SHADOW-STATIC-BUILDINGS-2: replay ALL registered rigid-building recipes (full registry, visibility-independent) into the world-fixed static shadow map, once per mission at the static-map build. Trees excluded (population filter). Default-OFF; =1 enables, =2 enables+trace. Independent of MC2_SHADOW_ENABLE. Requires C-pre min-combine. Debt: destroyed buildings keep stale static shadow until mission reload."
    },
    {
        "MC2_FEATURE_SHADOW_DYNAMIC_PROP_CASTERS",
        "MC2_SHADOW_DYNAMIC_PROP_CASTERS",
        EnvVarKind::Feature,
        true,
        "SHADOW-DYNAMIC-PROP-CASTERS-1: feed the per-frame DYNAMIC sun-shadow caster pass from the full registry (registered props, visibility-independent) instead of the camera-visible s_typeRanges set, which only admitted props near the camera (so distant trees never cast into the now-correctly-camera-fit dynamic map). DEFAULT-ON; MC2_SHADOW_DYNAMIC_PROP_CASTERS=0 is the kill-switch (reverts to the legacy camera-visible flushShadow feed); =2 adds trace. Only takes effect when MC2_SHADOW_ENABLE is set (dynamic shadow pass). Buildings: excluded from this feed when MC2_STATIC_PROP_BUILDING_SHADOW is active (they cast via the world-fixed static map), else included so they still cast a dynamic shadow. Debt: no light-box cull yet (draws all map props every frame); HZB planned."
    },
    {
        "MC2_TUNE_STATIC_PROP_IBL_SH_SET",
        "MC2_STATIC_PROP_IBL_SH_SET",
        EnvVarKind::Trace,
        false,
        "V-IBL-STATIC-2: override the active SH coefficient set by name (e.g. 'default'). Optional dev/debug knob. Unset/empty/unknown -> registry-or-default fallback. Does not gate IBL itself (MC2_STATIC_PROP_IBL_SH remains the authoritative on/off gate)."
    },
    {
        "MC2_TUNE_STATIC_PROP_IBL_SH_STRENGTH",
        "MC2_STATIC_PROP_IBL_SH_STRENGTH",
        EnvVarKind::Trace,
        false,
        "V-IBL-STATIC-1: optional default-strength override for SH-L2 ambient (clamped 0..3). ImGui slider g_iblShStrength initial value. Only contributes when MC2_STATIC_PROP_IBL_SH is active; =0 on that gate uploads strength 0.0. Unset/empty -> default 0.5. Resolved once at process start."
    },
    {
        "MC2_TUNE_STATIC_PROP_PBR_V1_STRENGTH",
        "MC2_STATIC_PROP_PBR_V1_STRENGTH",
        EnvVarKind::Trace,
        false,
        "V-MATERIAL-PBR-3: optional default-strength override for per-fragment Schlick-Fresnel specular (clamped 0..3). ImGui slider g_pbrV1Strength initial value. Only meaningful when MC2_STATIC_PROP_PBR_V1=1 (env var is the authoritative gate). Unset/empty -> default 1.0. Resolved once at process start."
    },
    {
        "MC2_DIAG_STATIC_PROP_PBR_V1_SUNFOUND",
        "MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND",
        EnvVarKind::Trace,
        false,
        "V-MATERIAL-PBR-3-DIAG: diagnostic visualizer for the forwarded sunFound state. When ON together with MC2_STATIC_PROP_PBR_V1=1, static_prop.frag replaces the PBR result with cyan (sunFound=true) or magenta (sunFound=false), bypassing Schlick math. Diagnostic-only; never affects default-OFF behavior. Unset/'0' -> off."
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP",
        "MC2_DEBUG_STATE_DUMP",
        EnvVarKind::Trace,
        false,
        "DEBUG-STATE-DUMP-1: write a JSON render-state snapshot to debug_state/latest_render_state.json every 300 frames (and at frame 1). Snapshot includes feature gates, RenderSnapshot counters, EngineView state, and StaticPropOpaque visual globals. Read-only; no gameplay or renderer behavior changes. Default-OFF; =1 enables. Output dir override: MC2_DEBUG_STATE_DUMP_DIR. See docs/debug_state_dump.md."
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP_DIR",
        "MC2_DEBUG_STATE_DUMP_DIR",
        EnvVarKind::Trace,
        false,
        "DEBUG-STATE-DUMP-1: override output directory for MC2_DEBUG_STATE_DUMP. Defaults to debug_state/ relative to working directory. No effect when MC2_DEBUG_STATE_DUMP is not set."
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP_HISTORY",
        "MC2_DEBUG_STATE_DUMP_HISTORY",
        EnvVarKind::Trace,
        false,
        "DEBUG-STATE-DUMP-2: enable rolling 8-slot history ring alongside latest_render_state.json. When set with MC2_DEBUG_STATE_DUMP=1, each write also produces history_0.json..history_7.json in the same output directory (oldest slot overwritten in order). Bounded to 8 files; no unbounded growth. Default-OFF; =1 enables."
    },
    {
        "MC2_DIAG_TERRAIN_DEBUG_MODE",
        "MC2_TERRAIN_DEBUG_MODE",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DEBUG-VIEWS-1: terrain fragment-shader debug-mode selector for the tessellated terrain path (gos_terrain.frag tessDebug.x). Default unset = mode 0 (off, byte-identical to legacy output). Visual modes: 1=DepthComparison, 2=RawColormap, 3=BlurredColormap, 4=MaterialWeights (R=rock,G=grass,B=dirt), 5=NormalLighting, 6=ShadowFactor, 7=CloudShadow. Diagnostics: 8=CementDiag, 9=ThinRecordDiag, -1=TessAliveProbe. When set, env value overrides the runtime mode (gos_*TerrainDebugMode C-API, Surface Debug Mode picker in GraphicsOptionsWindow, and the Terrain Pass inspector mini-control all read it). Diagnostic-only; no gameplay, correctness, or default visual effect. Full mode list table: GuiRuntime/GraphicsOptionsWindow.cpp kTerrainModes."
    },
    {
        "MC2_TUNE_TERRAIN_HEIGHT_RESAMPLE_FACTOR",
        "MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR",
        EnvVarKind::Trace,
        false,
        "TERRAIN-RESAMPLE-1: CPU bilinear resample factor for the per-mission terrain height texture used by TERRAIN-NORMALS-FROM-HEIGHT-1. Accepted values 1, 2, 4 (anything else clamps to 1). Default 1 (byte-equivalent to pre-slice). Render texture side becomes (sourceSide-1)*factor + 1, with source samples preserved EXACTLY at corner positions (factor multiples). Bilinear interpolation between source taps fills intermediate render samples. Resample is read at every gos_uploadTerrainHeightTex() call (i.e. per mission load); toggling mid-mission does not re-upload. Only affects the height-derived normal path: gameplay height (Terrain::getTerrainElevation) is unchanged; no displacement, no geometry move. Memory: 4× factor on a 120² source = ~890 KB; bounded by source-grid * 16. Inspector shows source/render/factor."
    },
    {
        "MC2_DIAG_VFX_DEBUG_MODE",
        "MC2_VFX_DEBUG_MODE",
        EnvVarKind::Trace,
        false,
        "VFX-DEBUG-VIEWS-1: GPU particle billboard fragment-shader debug-mode selector (particle_billboard.frag u_debugMode, uploaded by gos_particle_bridge). Default unset = mode 0 (Final, byte-identical to default output). Modes: 1=Albedo (raw atlas texel rgb, no vertex-color tint), 2=Alpha (final alpha as grayscale), 3=ParticleKind (distinct hashed color per kind_flags kind), 4=Overdraw (constant additive proxy to visualize blend buildup). Seeded once at process start from this env var (clamped 0..4); the VFX Pass Object-Inspector panel shows the active mode read-only (gos_vfx_getDebugMode), and the Graphics Options 'VFX Tuning' combo overrides it live (gos_vfx_setDebugMode). Diagnostic-only; no gameplay, emission, lifetime, sorting, or default visual effect; VFX object-IDs remain prohibited. RenderDebugView canonical mapping (kDebugViewMask_Vfx): Final->0, Albedo->1; modes 2-4 are VFX-local (no canonical enum slot)."
    },
    {
        "MC2_TUNE_VFX_BRIGHTNESS",
        "MC2_TUNE_VFX_BRIGHTNESS",
        EnvVarKind::Trace,
        false,
        "VFX-TUNING-UI-1: startup-default for the global GPU-particle RGB brightness scale (particle_billboard.frag u_vfxBrightness, applied to ALL particles). Clamped 0..8. Unset/empty -> 1.0 (byte-identical no-op). Seeded once at process start; the Graphics Options 'VFX Tuning > Brightness' slider overrides at runtime (gos_vfx_setBrightness). Look-only — no emission/lifetime/sorting/timing change. No effect when MC2_GPU_PARTICLES=0."
    },
    {
        "MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS",
        "MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS",
        EnvVarKind::Trace,
        false,
        "VFX-TUNING-UI-1: startup-default for the additive-only GPU-particle RGB brightness scale (particle_billboard.frag u_vfxAdditiveBrightness, applied ONLY to additive draw groups via per-group u_vfxIsAdditive). Clamped 0..8. Unset/empty -> 1.0 (byte-identical no-op). Graphics Options 'VFX Tuning > Additive brightness' slider overrides at runtime (gos_vfx_setAdditiveBrightness). Highest-value lever for pre-bloom additive overdraw (see docs/vfx-overdraw-audit.md). Look-only. No effect when MC2_GPU_PARTICLES=0."
    },
    {
        "MC2_TUNE_VFX_ALPHA_SCALE",
        "MC2_TUNE_VFX_ALPHA_SCALE",
        EnvVarKind::Trace,
        false,
        "VFX-TUNING-UI-1: startup-default for the GPU-particle alpha (opacity) scale (particle_billboard.frag u_vfxAlphaScale, applied to ALL particles). Clamped 0..8. Unset/empty -> 1.0 (byte-identical no-op). Graphics Options 'VFX Tuning > Opacity' slider overrides at runtime (gos_vfx_setAlphaScale). Look-only — no emission/lifetime/timing change. No effect when MC2_GPU_PARTICLES=0."
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
