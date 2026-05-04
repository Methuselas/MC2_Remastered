// gos_visual_diff.h - Stage 2.E pinned-camera visual-diff harness.
// Phase 1 Step 1.2: pose-data struct + strict pose-JSON parser.
// State machine, env wiring, and capture lands in subsequent Phase 1 steps.
#pragma once

namespace VisualDiff {

// Per-mission pinned camera pose. Schema v3 (MC2-native; see Stage 2.E
// Phase 1 plan, "Pose schema" section).
struct PoseData {
    float position[2];          // x, y in cameraPos space; z is recomputed
                                // from terrain by Camera::setPosition.
    float cameraRotation;       // first arg to Camera::setCameraRotation
    float cameraRotationWorld;  // second arg
    float cameraTilt;           // assigned to Camera::cameraTilt[viewIdx]
    float fov;                  // first arg to Camera::setFieldOfView
    int   frameN;               // frames-since-mission-ready at capture
    int   settle_frames;        // teleport fires at frameN - settle_frames
};

// Result of attempting to load a pose for a given mission key.
//
// File-not-found and mission-not-found are distinct from parse-failed:
// the former indicate "pose authoring not yet done for this mission" and
// are recoverable (harness logs pose_missing and exits 3); the latter
// indicates a malformed file and is a real error (harness logs
// pose_parse_failed and exits 3 as well, but with a different reason
// string for diagnosis).
enum class PoseLoadResult {
    Ok,
    FileNotFound,     // jsonPath does not exist or could not be opened
    MissionNotFound,  // file parsed, but missions[missionKey] absent
    ParseError,       // file present but malformed / missing required field
};

// Load a single mission's pose from the v3 JSON schema.
//
// On Ok: *out is populated.
// On FileNotFound: emits [VISUAL_DIFF v1] event=pose_missing reason=file_not_found
// On MissionNotFound: emits [VISUAL_DIFF v1] event=pose_missing reason=mission_not_authored mission=<key>
// On ParseError: emits [VISUAL_DIFF v1] event=pose_parse_failed reason=<r> details=<d>
//
// out may be NULL only when the caller is probing for existence; if non-NULL
// it is written iff the return is Ok.
PoseLoadResult loadPose(const char* jsonPath, const char* missionKey, PoseData* out);

}  // namespace VisualDiff
