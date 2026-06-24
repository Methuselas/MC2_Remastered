// GameOS/gameos/gos_mech_killswitch.h
#pragma once
#include <cstdint>

// =============================================================================
// DEFAULT-ON FLIP (2026-05-09)
// =============================================================================
// All GPU mech killswitches in this header now default to ON. The historical
// `(getenv("X") != nullptr)` opt-in pattern was replaced by `default-on, "0"
// opts out` after the slice campaign accumulated empirical soak (tier1 5/5,
// 90s mc2_10 Tracy clean, mouse-pick + sensor canaries verified, combined
// Mech3D.UpdateGeometry stack -80%).
//
// To OPT OUT of any individual flag: set the env var to literal "0".
//   e.g.  MC2_GPU_MECHS=0  →  legacy CPU mech path
//
// Any other value (including unset) keeps the new default-on behavior.
// See `envFlagDefaultOn` in gos_mech_batcher.cpp for the canonical pattern.
//
// Inter-flag dependencies still apply — most downstream flags only take
// effect when MC2_GPU_MECHS=1 (now default). Opting out of MC2_GPU_MECHS
// effectively opts out of the entire GPU mech path.
// =============================================================================

// ImGui "Draw Mechs" visibility kill-switch. DEFAULT-ON.
// When false, both the GPU batcher submit AND the CPU Render(true) fallback
// are suppressed, so mechs are invisible.  Independent of g_useGpuMechs —
// set this to false to hide mechs entirely; set g_useGpuMechs to false to
// force the CPU rendering path while still drawing mechs.
extern bool g_drawMechs;

// Runtime toggle for the GPU mech renderer. DEFAULT-ON.
// Opt-out: MC2_GPU_MECHS=0 → legacy CPU mech path.
// Can also be toggled at runtime via RAlt+M (wire in gameosmain.cpp hotkey handler).
extern bool g_useGpuMechs;

// Preview-render context depth. >0 while a SimpleCamera UI preview render is
// in flight (Mech Bay / Mech Purchase rotating mech). The GPU mech batcher
// flushes with the WORLD render snapshot / terrain MVP, not the SimpleCamera
// UI projection, so a submitted preview mech draws off-screen / blank. While
// this depth is nonzero, Mech3DAppearance::render bypasses the GPU mech submit
// and forces the legacy CPU MLR draw, which honors the SimpleCamera. Tactical /
// world mech rendering (depth == 0) is unaffected. Set via the RAII scope below.
extern int g_mechPreviewRenderDepth;
struct MechPreviewRenderScope {
	MechPreviewRenderScope()  { ++g_mechPreviewRenderDepth; }
	~MechPreviewRenderScope() { --g_mechPreviewRenderDepth; }
};

// Slice B1: gates VS-side calc_light() in mech.vert. Independent of
// g_useGpuMechs so an operator can keep GPU mech rendering on while
// flipping lighting off if a B1 regression surfaces. No effect when
// g_useGpuMechs is off (the entire batcher path skips).
extern bool g_useGpuMechLighting;

// Slice C1: render-side mech cull. Skips submitActor for mechs the
// GPU frustum/visibility shader marks invisible. RENDER-ONLY — does
// NOT cascade into update/AI/lifecycle. No effect when g_useGpuMechs
// is off. Independent of MC2_GPU_CULL_LIFECYCLE.
extern bool g_useGpuMechCull;

// Slice C2: weighted multi-bone skinning in mech.vert. Off = rigid
// per-bone (single boneIndices.x lookup, Slice A behavior). On =
// weighted blend across all 4 bone slots. Stock data is byte-
// identical between the two modes (boneWeights = 1,0,0,0 collapses);
// the difference is only meaningful for imported meshes.
extern bool g_useGpuMechSkin;

// Slice C3-revised (2026-05-09): wire TransformMultiShape_PositionsOnly
// for the GPU mech body. Skips the per-vertex CPU lighting kernel that
// consumes ~65µs/mech and whose output (listOfVertices[j].argb) is only
// consumed by the legacy CPU Render(true) path that Slice A bypasses.
// Independent of g_useGpuMechs / g_useGpuMechLighting for bisect
// granularity. Requires g_useGpuMechs=true to take effect (when GPU
// mech path is off, the legacy path NEEDS the lighting bake's output).
// BODY-ONLY: arms (mech3d.cpp:4459, :4543) and shadow (mech3d.cpp:3377)
// stay full TransformMultiShape; their Render(true) callers still need
// listOfVertices[*].argb populated.
extern bool g_useGpuMechFastTransform;

// Slice C3-shadow (2026-05-09): wire TransformMultiShape_PositionsOnly
// for the mech SHADOW shape callsite (mech3d.cpp:3377). Recon proved
// RenderShadows hardcodes gVertex.argb (tgl.cpp:3636/3645/3654) and uses
// listOfShadowTVertices populated by MultiTransformShadows (which still
// dispatches at msl.cpp:1765 in both branches); the per-vertex CPU
// lighting bake's output is unused by the shadow render path. Independent
// of g_useGpuMechs / g_useGpuMechFastTransform for bisect granularity.
// Requires g_useGpuMechs=true to take effect (mirrors C3-revised body
// slice gating discipline).
extern bool g_useGpuMechShadowFastTransform;

// Slice D-shadow-skip (2026-05-09): skip mechShadowShape->TransformMultiShape*
// entirely when modern engine has terrain tessellation active. Recon proved
// every byte produced by that call has zero consumer in modern + GPU mech
// path: Mech3DAppearance::renderShadows early-returns on tessellation
// (mech3d.cpp:3054), and modern dynamic shadows use g_shadowShapes[]
// (txmmgr.cpp:130, 1589-1620), a separate data path. Independent of
// g_useGpuMechs / fast-transform flags for bisect granularity. Requires
// g_useGpuMechs=true AND gos_IsTerrainTessellationActive() to take effect.
extern bool g_useGpuMechShadowSkip;

// Slice D-shadow-state-strip (2026-05-09): skip the four state setters on
// mechShadowShape (setAnimation/SetFrameNum/SetNodeRotation/SetLightList
// at mech3d.cpp:3351-3355 and :3368-3376) when modern engine + GPU mech
// path is engaged. Recon proved no consumer of these setters' effects
// exists in this configuration: instance-state setters feed only
// TransformMultiShape (already retired by D-shadow-skip), and SetLightList's
// global-static side effect (s_listOfLights) is overwritten by mechShape's
// identical call at mech3d.cpp:3407 before any consumer reads it.
// Independent of g_useGpuMechs / fast-transform / shadow-skip flags for
// bisect granularity. Requires g_useGpuMechs=true AND
// gos_IsTerrainTessellationActive() to take effect (mirrors D-shadow-skip).
extern bool g_useGpuMechShadowStateStrip;

// Slice D-leaf-skip-v2 (2026-05-09): strip per-leaf body of mechShape->
// TransformMultiShape* (per-leaf pool alloc + per-vertex screen-space
// projection + per-face backface cull + MultiTransformShadows dispatch)
// when modern engine + GPU mech path is engaged. Recon proved every
// per-leaf field on mechShape has zero practical consumer in this
// configuration: Slice A bypasses mechShape->Render(true); RenderShadows
// unreachable on tessellation; PerPolySelect-on-mechs is theoretical via
// fallback findObjectByMouse but findMoverByMouse rect-only test catches
// all real mech selection (objmgr.cpp:2506 "// Movers are NOT per poly!!").
// submitActor + getNodePosition read only listOfShapes[i].shapeToWorld
// (hierarchy-level), preserved by leaf-skip.
//
// Independent of g_useGpuMechs / fast-transform / shadow-skip / state-strip
// flags for bisect granularity. Requires g_useGpuMechs=true AND
// gos_IsTerrainTessellationActive() to take effect. NOT compatible with
// MC2_MECH_GPU_PARITY=1 — disable LEAF_SKIP if running parity diagnostic.
extern bool g_useGpuMechLeafSkip;

// MECH-KILLSWITCH-SENSORSKIP-RETIRE-1 (2026-06-23): the former
// MC2_GPU_MECH_SENSOR_SKIP killswitch (Slice D-sensor-skip, default-ON) is
// RETIRED to a constant. The skip — bypass the sensor-marker transform when
// sensorLevel ∈ {0,5}, where the marker never renders (render gate is the exact
// inverse [1,4], mech3d.cpp:2948-2960) — is a strict no-op and was always on by
// default. The behavior now lives inline in updateGeometry, still guarded by
// g_useGpuMechs (CPU path transforms sensors regardless). Audit:
// MECH-KILLSWITCH-AUDIT-1; MC2_MECH_GPU_PARITY harness never built.

// Resolve a gosTextureHandle to the underlying raw GL texture name.
// Returns 0 if handle is INVALID_TEXTURE_ID or gosTexture is gone.
// Implemented in gameos_graphics.cpp (same as gos_static_prop_killswitch.h).
uint32_t gos_GetGLTextureId(uint32_t gosHandle);

// Terrain projection chain uniforms — same as gos_static_prop_killswitch.h.
const float* gos_GetProj2ScreenMat4();       // screen-pixel -> NDC (upload GL_TRUE)
const float* gos_GetTerrainMVPMat4();        // axisSwap * worldToClip (upload GL_FALSE)
