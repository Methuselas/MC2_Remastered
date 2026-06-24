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

// MECH-KILLSWITCH-LIGHTING-RETIRE-1 (2026-06-23): MC2_GPU_MECH_LIGHTING (Slice
// B1, default-ON) RETIRED to constant. u_lightingMode is now always 1 (calc_light).
// Safe: the low-UBO hardware guard that once forced this off was REMOVED by
// [LIGHTSSBO v1] (LightsData is now an unbounded std430 SSBO, binding 20 —
// gos_mech_batcher.cpp:616-642 "Gate removed"), so no runtime writer survived and
// the flag was pure env-config. OFF was a visual degrade (unlit white mechs), never
// shipped. Audit: MECH-KILLSWITCH-AUDIT-1. NOTE: if LightsData ever reverts to a UBO,
// the hardware guard must return and this goes back to a runtime bool (default-true).

// Slice C1: render-side mech cull. Skips submitActor for mechs the
// GPU frustum/visibility shader marks invisible. RENDER-ONLY — does
// NOT cascade into update/AI/lifecycle. No effect when g_useGpuMechs
// is off. Independent of MC2_GPU_CULL_LIFECYCLE.
extern bool g_useGpuMechCull;

// MECH-KILLSWITCH-SKIN-RETIRE-1 (2026-06-23): MC2_GPU_MECH_SKIN (Slice C2,
// default-ON) RETIRED to constant. u_skinningMode is now always 1 (weighted).
// Proven no-op: stock data is (1,0,0,0) → the weighted sum collapses to the
// rigid single-bone result (mech.vert:156-169), and ALL 59 BT2018 imports are
// single-bone-per-vertex at source (3.27M verts, 0 blended — verified). The
// weighted branch stays as latent forward capacity for a future blend-skinned
// importer (do NOT delete it). Audit: MECH-KILLSWITCH-AUDIT-1.

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

// Slice D-sensor-skip (2026-05-09): skip the sensor block in
// Mech3DAppearance::updateGeometry (sensorTriangleShape + sensorSquareShape
// SetFogRGB + SetLightList + TransformMultiShape, plus sensorSpin animation
// update) when sensorLevel ∈ {0, 5}. Sensor SHAPES render only when
// sensorLevel ∈ [1,4] (mech3d.cpp:2948-2960); for player mechs (sensorLevel=5)
// and undetected enemies (sensorLevel=0) the transform work has no consumer.
// Skip gate is exact INVERSE of Render gate — strict no-op semantics.
//
// Independent of all other mech killswitches for bisect granularity.
// Requires g_useGpuMechs=true to take effect (consistent with prior slices'
// gating discipline; no tessellation gate needed since sensor render path
// is legacy CPU regardless).
extern bool g_useGpuMechSensorSkip;

// Resolve a gosTextureHandle to the underlying raw GL texture name.
// Returns 0 if handle is INVALID_TEXTURE_ID or gosTexture is gone.
// Implemented in gameos_graphics.cpp (same as gos_static_prop_killswitch.h).
uint32_t gos_GetGLTextureId(uint32_t gosHandle);

// Terrain projection chain uniforms — same as gos_static_prop_killswitch.h.
const float* gos_GetProj2ScreenMat4();       // screen-pixel -> NDC (upload GL_TRUE)
const float* gos_GetTerrainMVPMat4();        // axisSwap * worldToClip (upload GL_FALSE)
