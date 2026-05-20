// gos_visual_diff.cpp - Stage 2.E natural-mission-camera capture harness.
//
// Strips away the camera-teleport approach (Steps 1.2-1.5 architecture) in
// favor of capturing the engine's natural mission camera at a chosen frame.
// Rationale: each mission's intro pan + command-position settle already
// produces a repeatable, useful vantage on its own. Trying to teleport the
// camera and clear goal-tracking state ran into intro-pan / mission-script
// camera-driver fights that the goal-clearing block alone couldn't address.
//
// The engine's job here is now minimal: latch on the first frame where
// gos_terrain_indirect::WasEverFrameSolidArmed() returns true (the
// sticky-once cousin of IsFrameSolidArmed(); see header for why we need
// the sticky variant), count frames after that, capture at frame N, exit.
// MC2_VISUAL_DIFF_FRAME_N is then frames-since-intro-complete, so small
// values (60-90) suffice -- the intro-pan wall-clock jitter is upstream of
// the latch and cancels across runs. Per-mission frameN tuning lives in the
// Python harness.
#include "gos_visual_diff.h"
#include "gos_screenshot.h"
#include "gos_smoke.h"
#include "gos_terrain_indirect.h"

#include <cstdio>
#include <cstdlib>

namespace VisualDiff {

namespace {

// Lazy-cached env reads.
bool s_captureRead = false;
bool s_capture     = false;

enum class Phase { Waiting, Done };

struct State {
    int   localFrame            = 0;
    int   introCompleteFrame    = -1;
    bool  introCompleteObserved = false;
    Phase phase                 = Phase::Waiting;
    bool  paramsResolved        = false;
    int   frameN                = 90;
    int   maxFrames             = 150;
};
State s_state;

int envInt(const char* name, int dflt) {
    const char* v = getenv(name);
    if (!v || !v[0]) return dflt;
    return atoi(v);
}

void doCapture(int viewportW, int viewportH) {
    const char* outPath = getenv("MC2_VISUAL_DIFF_OUT");
    if (!outPath || !outPath[0]) {
        fprintf(stderr,
                "[VISUAL_DIFF v1] event=capture_skipped reason=no_out_path\n");
        fflush(stderr);
        return;
    }
    bool ok = gos::screenshot::writeTGA(outPath, viewportW, viewportH);
    if (ok) {
        const char* mission = getenv("MC2_VISUAL_DIFF_MISSION");
        fprintf(stderr,
                "[VISUAL_DIFF v1] event=capture_done out=%s w=%d h=%d mission=%s\n",
                outPath, viewportW, viewportH, mission ? mission : "(unset)");
    } else {
        fprintf(stderr,
                "[VISUAL_DIFF v1] event=capture_failed reason=write_tga out=%s\n",
                outPath);
    }
    fflush(stderr);
}

}  // anonymous namespace

bool isCaptureEnabled() {
    if (!s_captureRead) {
        const char* v = getenv("MC2_VISUAL_DIFF_CAPTURE");
        s_capture = (v && v[0] && v[0] != '0');
        s_captureRead = true;
    }
    return s_capture;
}

void onMissionLoad() {
    s_state = State{};
}

void onFrameTick(int viewportW, int viewportH) {
    if (!isCaptureEnabled()) return;
    if (s_state.phase == Phase::Done) return;

    s_state.localFrame++;

    // Resolve frameN + maxFrames from env on first tick. No JSON parsing,
    // no per-mission file lookup — Python harness sets these per launch.
    if (!s_state.paramsResolved) {
        s_state.frameN    = envInt("MC2_VISUAL_DIFF_FRAME_N",    90);
        s_state.maxFrames = envInt("MC2_VISUAL_DIFF_MAX_FRAMES", s_state.frameN + 60);
        s_state.paramsResolved = true;
        const char* mission = getenv("MC2_VISUAL_DIFF_MISSION");
        fprintf(stderr,
                "[VISUAL_DIFF v1] event=capture_armed mission=%s frameN=%d maxFrames=%d\n",
                mission ? mission : "(unset)",
                s_state.frameN, s_state.maxFrames);
        fflush(stderr);
    }

    // Snapshot intro-complete frame on first tick after the engine's own
    // "intro pan complete" signal: WasEverFrameSolidArmed() is the sticky
    // cousin of IsFrameSolidArmed() and is set on the first frame that
    // ComputePreflight() arms. We must use the sticky variant here because
    // gosRenderer::endFrame() resets the per-frame s_frameSolidArmed flag
    // BEFORE the VisualDiff post-PP hook fires, so the per-frame getter
    // always reads false from here. Latching on the sticky makes frameN
    // deterministic: intro-pan wall-clock jitter is upstream of the latch
    // and cancels across runs.
    if (!s_state.introCompleteObserved) {
        if (gos_terrain_indirect::WasEverFrameSolidArmed()) {
            s_state.introCompleteObserved = true;
            s_state.introCompleteFrame    = s_state.localFrame;
            fprintf(stderr,
                    "[VISUAL_DIFF v1] event=intro_pan_complete_observed local_frame=%d\n",
                    s_state.localFrame);
            fflush(stderr);
        }
        return;
    }

    int framesSinceStart = s_state.localFrame - s_state.introCompleteFrame;

    // Timeout — exit cleanly with code 4 so the Python harness can distinguish.
    if (framesSinceStart > s_state.maxFrames) {
        fprintf(stderr,
                "[VISUAL_DIFF v1] event=capture_timeout frames_since_start=%d max_frames=%d\n",
                framesSinceStart, s_state.maxFrames);
        fflush(stderr);
        fflush(stdout);
        exit(4);
    }

    // Capture at frameN.
    if (framesSinceStart >= s_state.frameN) {
        doCapture(viewportW, viewportH);
        s_state.phase = Phase::Done;
    }
}

}  // namespace VisualDiff
