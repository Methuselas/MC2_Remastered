// visual_diff_camera_bridge.h - Stage 2.E camera-teleport bridge.
//
// GameOS/gameos/gos_visual_diff.cpp drives the visual-diff state machine but
// must not include mclib/code headers (engine layer must not depend on
// gameplay layer; per gameos_graphics.cpp:38-42 convention). This bridge
// exposes a POD-only API that the engine layer can call via forward decl.
//
// Implementation in visual_diff_camera_bridge.cpp pulls in mclib/camera.h,
// gamecam.h, and terrain.h — all gameplay-layer headers, kept local to the
// bridge .cpp so the layering seam is explicit.
#pragma once

namespace VisualDiffCameraBridge {

// Apply a pinned camera pose with goal-clearing.
//
// Returns true on success; false if the runtime is not in a teleport-able
// state (eye == NULL means no Camera instance yet; land == NULL means terrain
// not loaded — Camera::setPosition's z-from-terrain branch wouldn't fire).
//
// On success, the camera is teleported to (x, y, terrain_z), rotated/tilted
// per the args, FOV set, and all goal-tracking timers zeroed so the next
// frame's update loop does not interpolate the camera away from the pinned
// pose.
bool applyPose(float x, float y,
               float cameraRotation,
               float cameraRotationWorld,
               float cameraTilt,
               float fov);

}  // namespace VisualDiffCameraBridge
