// visual_diff_camera_bridge.cpp - Stage 2.E camera-teleport bridge impl.
//
// Layering: this file lives in code/ (gameplay layer) and may freely include
// mclib/camera.h, gamecam.h, terrain.h. GameOS/gameos/gos_visual_diff.cpp
// calls into this namespace via forward decl, never via #include of this
// header — that keeps mclib types out of the engine layer.
//
// All cited camera APIs were grep-verified at round-5 plan revision and are
// reverified at the implementation site here:
//   - Camera::setPosition          mclib/camera.h:904  (in-mission, swoopy=false
//                                  branch overwrites z from terrain at
//                                  camera.cpp:2247,2269)
//   - Camera::setCameraRotation    mclib/camera.h:417
//   - Camera::cameraTilt           mclib/camera.h:312  (public static array)
//   - Camera::setFieldOfView       mclib/camera.h:952
//   - Camera::getPosition          mclib/camera.h:648
//   - Camera::setGoalPosition      mclib/camera.h:973
//   - Camera::setGoalPosTime       mclib/camera.h:993
//   - Camera::setFieldOfViewGoal   mclib/camera.h:962  (writes goalFOV+goalFOVTime)
//   - Camera::getRotation          mclib/camera.h:998-1005 (returns live state)
//   - Camera::setGoalRotation      mclib/camera.h:1022
//   - Camera::setGoalRotTime       mclib/camera.h:1032
// Consumer Camera::updateGoalRotation() at camera.cpp:1013-1040 gates on
// goalRotTime > 0.0; setting it to 0 stops the rotation animation.
#include "visual_diff_camera_bridge.h"

#include <cstdio>

#include "gamecam.h"   // declares CameraPtr eye
#include "camera.h"    // mclib Camera class with all setters/getters above
#include "terrain.h"   // declares TerrainPtr land

namespace VisualDiffCameraBridge {

bool applyPose(float x, float y,
               float cameraRotation,
               float cameraRotationWorld,
               float cameraTilt,
               float fov) {
    if (!eye) {
        fprintf(stderr,
                "[VISUAL_DIFF v1] event=teleport_skipped reason=no_eye\n");
        fflush(stderr);
        return false;
    }
    if (!land) {
        fprintf(stderr,
                "[VISUAL_DIFF v1] event=teleport_skipped reason=no_land\n");
        fflush(stderr);
        return false;
    }

    // Position. z=0 placeholder; setPosition's swoopy=false branch overwrites
    // position.z = land->getTerrainElevation(position) at camera.cpp:2247,2269.
    Stuff::Vector3D pos(x, y, 0.0f);
    eye->setPosition(pos, /*swoopy=*/false);

    // Rotation (two stored fields: cameraRotation at camera.h:124,
    // worldCameraRotation at :125).
    eye->setCameraRotation(cameraRotation, cameraRotationWorld);

    // Tilt (public static array — direct assignment per round-5 plan).
    Camera::cameraTilt[/*viewIdx=*/0] = cameraTilt;

    // FOV (instant variant — single arg).
    eye->setFieldOfView(fov);

    // Goal clearing — prevents smoothing away from pinned pose.
    // Position goal: lock to current, zero timer.
    eye->setGoalPosition(eye->getPosition());
    eye->setGoalPosTime(0.0f);

    // FOV goal: writes goalFOV=fov AND goalFOVTime=0 in one call.
    eye->setFieldOfViewGoal(fov, 0.0f);

    // Rotation goal: write LIVE rotation (not goal — round-4 NC6 lesson),
    // then zero the timer (the consumer at camera.cpp:1013 gates on
    // goalRotTime > 0).
    eye->setGoalRotation(eye->getRotation());
    eye->setGoalRotTime(0.0f);

    fprintf(stderr,
            "[VISUAL_DIFF v1] event=teleport_applied "
            "position=[%.2f,%.2f] cameraRotation=%.2f cameraRotationWorld=%.2f "
            "cameraTilt=%.2f fov=%.2f\n",
            x, y, cameraRotation, cameraRotationWorld, cameraTilt, fov);
    fflush(stderr);
    return true;
}

}  // namespace VisualDiffCameraBridge
