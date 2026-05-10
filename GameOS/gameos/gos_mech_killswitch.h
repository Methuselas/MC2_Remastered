// GameOS/gameos/gos_mech_killswitch.h
#pragma once
#include <cstdint>

// Runtime toggle for the GPU mech renderer.
// Default: enabled by env-var MC2_GPU_MECHS=1 at process start.
// Can also be toggled at runtime via RAlt+M (wire in gameosmain.cpp hotkey handler).
extern bool g_useGpuMechs;

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
const float* gos_GetTerrainViewportVec4();   // (vmx, vmy, vax, vay)
const float* gos_GetProj2ScreenMat4();       // screen-pixel -> NDC (upload GL_TRUE)
const float* gos_GetTerrainMVPMat4();        // axisSwap * worldToClip (upload GL_FALSE)
