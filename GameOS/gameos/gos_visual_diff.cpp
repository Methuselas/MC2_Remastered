// gos_visual_diff.cpp - Stage 2.E pinned-camera visual-diff harness.
// Phase 1 Step 1.2: pose-JSON parser only.
// State machine + env wiring + capture hook arrive in subsequent steps.
//
// This is a strict purpose-built parser for the v3 pose schema, not a
// general JSON library. Replaces the round-3 reviewer's suggested picojson
// vendoring per advisor decision (small fixed schema, no external dependency).
//
// Schema accepted (v3, MC2-native):
//   {
//     "version": 3,
//     "missions": {
//       "<missionKey>": {
//         "position":            [float, float],
//         "cameraRotation":      float,
//         "cameraRotationWorld": float,
//         "cameraTilt":          float,
//         "fov":                 float,
//         "frameN":              int,
//         "settle_frames":       int
//       },
//       ...
//     }
//   }
//
// Parser behavior:
//   - rejects missing required fields with ParseError + missing_field_<name>
//   - ignores unknown fields (forward-compat within v3)
//   - accepts arbitrary whitespace between tokens
//   - ASCII keys only; no Unicode/escape support beyond \" and \\
//   - numbers parsed via strtod; ints rounded
//
#include "gos_visual_diff.h"
#include "gos_screenshot.h"
#include "gos_smoke.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Forward declaration of the gameplay-layer camera bridge. Implementation at
// code/visual_diff_camera_bridge.cpp pulls in mclib/camera.h, gamecam.h,
// terrain.h — none of which the engine layer (this TU) is allowed to include
// per the gameos_graphics.cpp:38-42 layering convention. Linker connects the
// call. Hoisted to GLOBAL scope so the symbol matches the bridge's
// `::VisualDiffCameraBridge::applyPose` rather than getting nested in
// VisualDiff::anonymous::.
namespace VisualDiffCameraBridge {
    bool applyPose(float x, float y,
                   float cameraRotation,
                   float cameraRotationWorld,
                   float cameraTilt,
                   float fov);
}

namespace VisualDiff {

namespace {

// --- Lexer over a NUL-terminated buffer ---

struct Lexer {
    const char* start;   // start of buffer
    const char* p;       // cursor
    const char* end;     // one past last byte
    bool        failed;
    char        errReason[64];   // one of: bad_token, missing_field_<name>,
                                 //         version_mismatch, bad_position_array, ...
    char        errDetails[192]; // human-friendly context
};

static void fail(Lexer& L, const char* reason, const char* details) {
    if (L.failed) return;  // first failure wins
    L.failed = true;
    snprintf(L.errReason, sizeof(L.errReason), "%s", reason);
    snprintf(L.errDetails, sizeof(L.errDetails), "%s", details ? details : "");
}

static void skipWs(Lexer& L) {
    while (L.p < L.end) {
        char c = *L.p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { L.p++; }
        else break;
    }
}

static bool peek(Lexer& L, char c) {
    skipWs(L);
    return L.p < L.end && *L.p == c;
}

static bool consume(Lexer& L, char c) {
    skipWs(L);
    if (L.p < L.end && *L.p == c) { L.p++; return true; }
    char buf[80];
    snprintf(buf, sizeof(buf), "expected '%c' at offset %ld",
             c, (long)(L.p - L.start));
    fail(L, "bad_token", buf);
    return false;
}

// Read a double-quoted ASCII string. Supports \" and \\ escapes only.
// Writes up to outCap-1 chars to out + NUL.
static bool readString(Lexer& L, char* out, size_t outCap) {
    skipWs(L);
    if (L.p >= L.end || *L.p != '"') {
        fail(L, "bad_token", "expected string literal");
        return false;
    }
    L.p++;  // opening "
    size_t i = 0;
    while (L.p < L.end && *L.p != '"') {
        char c = *L.p++;
        if (c == '\\' && L.p < L.end) {
            char e = *L.p++;
            if      (e == '"')  c = '"';
            else if (e == '\\') c = '\\';
            else { fail(L, "bad_token", "unsupported escape"); return false; }
        }
        if (i + 1 < outCap) out[i++] = c;
    }
    if (L.p >= L.end) {
        fail(L, "bad_token", "unterminated string");
        return false;
    }
    L.p++;  // closing "
    if (i < outCap) out[i] = '\0';
    else if (outCap > 0) out[outCap - 1] = '\0';
    return true;
}

// Read a number. strtod handles ints, floats, signs, scientific notation.
static bool readNumber(Lexer& L, double& out) {
    skipWs(L);
    char* endp = nullptr;
    // strtod stops at the first non-numeric char; safe because input is
    // NUL-terminated and we trust the upstream buffer lifecycle.
    double v = strtod(L.p, &endp);
    if (endp == L.p) {
        fail(L, "bad_token", "expected number");
        return false;
    }
    L.p = endp;
    out = v;
    return true;
}

// Skip a JSON value of any kind (used to ignore unknown fields).
// Recursive but bounded by document size.
static bool skipValue(Lexer& L) {
    skipWs(L);
    if (L.p >= L.end) { fail(L, "bad_token", "unexpected EOF"); return false; }
    char c = *L.p;
    if (c == '"') {
        char tmp[256];
        return readString(L, tmp, sizeof(tmp));
    }
    if (c == '{') {
        L.p++;
        skipWs(L);
        if (peek(L, '}')) { L.p++; return true; }
        for (;;) {
            char keyTmp[128];
            if (!readString(L, keyTmp, sizeof(keyTmp))) return false;
            if (!consume(L, ':')) return false;
            if (!skipValue(L)) return false;
            if (peek(L, ',')) { L.p++; continue; }
            if (peek(L, '}')) { L.p++; return true; }
            fail(L, "bad_token", "expected ',' or '}'");
            return false;
        }
    }
    if (c == '[') {
        L.p++;
        skipWs(L);
        if (peek(L, ']')) { L.p++; return true; }
        for (;;) {
            if (!skipValue(L)) return false;
            if (peek(L, ',')) { L.p++; continue; }
            if (peek(L, ']')) { L.p++; return true; }
            fail(L, "bad_token", "expected ',' or ']'");
            return false;
        }
    }
    // number / true / false / null — read greedily until non-token char.
    while (L.p < L.end) {
        char ch = *L.p;
        if (ch == ',' || ch == '}' || ch == ']' ||
            ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
        L.p++;
    }
    return true;
}

// Read [float, float] into out[2].
static bool readPositionArray(Lexer& L, float out[2]) {
    if (!consume(L, '[')) { fail(L, "bad_position_array", "expected '['"); return false; }
    double a = 0, b = 0;
    if (!readNumber(L, a)) { fail(L, "bad_position_array", "first element"); return false; }
    if (!consume(L, ','))  { fail(L, "bad_position_array", "expected ','"); return false; }
    if (!readNumber(L, b)) { fail(L, "bad_position_array", "second element"); return false; }
    if (!consume(L, ']'))  { fail(L, "bad_position_array", "expected ']'"); return false; }
    out[0] = (float)a;
    out[1] = (float)b;
    return true;
}

// --- Schema-aware walker ---

struct MissionFields {
    bool position = false;
    bool cameraRotation = false;
    bool cameraRotationWorld = false;
    bool cameraTilt = false;
    bool fov = false;
    bool frameN = false;
    bool settle_frames = false;

    bool allPresent() const {
        return position && cameraRotation && cameraRotationWorld &&
               cameraTilt && fov && frameN && settle_frames;
    }
    const char* firstMissing() const {
        if (!position)            return "position";
        if (!cameraRotation)      return "cameraRotation";
        if (!cameraRotationWorld) return "cameraRotationWorld";
        if (!cameraTilt)          return "cameraTilt";
        if (!fov)                 return "fov";
        if (!frameN)              return "frameN";
        if (!settle_frames)       return "settle_frames";
        return "(none)";
    }
};

// Parse one mission's object body { ... } into out. Caller has already
// consumed the opening `{`.
static bool parseMissionBody(Lexer& L, PoseData* out, MissionFields& seen) {
    if (peek(L, '}')) { L.p++; return true; }  // empty object — let caller decide
    for (;;) {
        char key[64];
        if (!readString(L, key, sizeof(key))) return false;
        if (!consume(L, ':')) return false;

        if      (strcmp(key, "position") == 0) {
            if (!readPositionArray(L, out->position)) return false;
            seen.position = true;
        }
        else if (strcmp(key, "cameraRotation") == 0) {
            double v; if (!readNumber(L, v)) return false;
            out->cameraRotation = (float)v; seen.cameraRotation = true;
        }
        else if (strcmp(key, "cameraRotationWorld") == 0) {
            double v; if (!readNumber(L, v)) return false;
            out->cameraRotationWorld = (float)v; seen.cameraRotationWorld = true;
        }
        else if (strcmp(key, "cameraTilt") == 0) {
            double v; if (!readNumber(L, v)) return false;
            out->cameraTilt = (float)v; seen.cameraTilt = true;
        }
        else if (strcmp(key, "fov") == 0) {
            double v; if (!readNumber(L, v)) return false;
            out->fov = (float)v; seen.fov = true;
        }
        else if (strcmp(key, "frameN") == 0) {
            double v; if (!readNumber(L, v)) return false;
            out->frameN = (int)(v + (v >= 0 ? 0.5 : -0.5)); seen.frameN = true;
        }
        else if (strcmp(key, "settle_frames") == 0) {
            double v; if (!readNumber(L, v)) return false;
            out->settle_frames = (int)(v + (v >= 0 ? 0.5 : -0.5)); seen.settle_frames = true;
        }
        else {
            // Forward-compat: ignore unknown fields silently.
            if (!skipValue(L)) return false;
        }

        if (peek(L, ',')) { L.p++; continue; }
        if (peek(L, '}')) { L.p++; return true; }
        fail(L, "bad_token", "expected ',' or '}' in mission body");
        return false;
    }
}

}  // anonymous namespace

PoseLoadResult loadPose(const char* jsonPath, const char* missionKey, PoseData* out) {
    if (!jsonPath || !missionKey) {
        fprintf(stderr, "[VISUAL_DIFF v1] event=pose_parse_failed reason=null_arg\n");
        return PoseLoadResult::ParseError;
    }

    FILE* f = fopen(jsonPath, "rb");
    if (!f) {
        fprintf(stderr, "[VISUAL_DIFF v1] event=pose_missing reason=file_not_found path=%s\n",
                jsonPath);
        return PoseLoadResult::FileNotFound;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > 1 << 20) {  // 1MB sanity cap
        fclose(f);
        fprintf(stderr, "[VISUAL_DIFF v1] event=pose_parse_failed reason=file_too_large size=%ld\n", sz);
        return PoseLoadResult::ParseError;
    }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "[VISUAL_DIFF v1] event=pose_parse_failed reason=oom\n");
        return PoseLoadResult::ParseError;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    Lexer L = {};
    L.start = buf;
    L.p = buf;
    L.end = buf + n;

    // Top-level: must be `{ "version": 3, "missions": { ... } }`.
    int  version = -1;
    bool versionSeen = false;
    bool foundMission = false;
    PoseData scratch = {};
    MissionFields seen;

    if (!consume(L, '{')) goto parse_fail;
    if (peek(L, '}')) { L.p++; goto parse_done; }

    for (;;) {
        char key[64];
        if (!readString(L, key, sizeof(key))) goto parse_fail;
        if (!consume(L, ':'))                 goto parse_fail;

        if (strcmp(key, "version") == 0) {
            double v; if (!readNumber(L, v)) goto parse_fail;
            version = (int)(v + 0.5);
            versionSeen = true;
        }
        else if (strcmp(key, "missions") == 0) {
            if (!consume(L, '{')) goto parse_fail;
            if (peek(L, '}')) { L.p++; }
            else {
                for (;;) {
                    char mkey[64];
                    if (!readString(L, mkey, sizeof(mkey))) goto parse_fail;
                    if (!consume(L, ':'))                   goto parse_fail;
                    if (!consume(L, '{'))                   goto parse_fail;

                    bool isTarget = (strcmp(mkey, missionKey) == 0);
                    if (isTarget) {
                        if (!parseMissionBody(L, &scratch, seen)) goto parse_fail;
                        foundMission = true;
                    } else {
                        // Skip non-target mission's body: we already consumed `{`,
                        // so loop reading key:value pairs and skipping values.
                        if (peek(L, '}')) { L.p++; }
                        else {
                            for (;;) {
                                char dkey[64];
                                if (!readString(L, dkey, sizeof(dkey))) goto parse_fail;
                                if (!consume(L, ':'))                   goto parse_fail;
                                if (!skipValue(L))                      goto parse_fail;
                                if (peek(L, ',')) { L.p++; continue; }
                                if (peek(L, '}')) { L.p++; break; }
                                fail(L, "bad_token", "expected ',' or '}' in skipped mission");
                                goto parse_fail;
                            }
                        }
                    }

                    if (peek(L, ',')) { L.p++; continue; }
                    if (peek(L, '}')) { L.p++; break; }
                    fail(L, "bad_token", "expected ',' or '}' after mission entry");
                    goto parse_fail;
                }
            }
        }
        else {
            // Unknown top-level field — ignore for forward-compat.
            if (!skipValue(L)) goto parse_fail;
        }

        if (peek(L, ',')) { L.p++; continue; }
        if (peek(L, '}')) { L.p++; break; }
        fail(L, "bad_token", "expected ',' or '}' at top level");
        goto parse_fail;
    }

parse_done:
    free(buf);

    if (L.failed) {
        fprintf(stderr, "[VISUAL_DIFF v1] event=pose_parse_failed reason=%s details=\"%s\" path=%s\n",
                L.errReason, L.errDetails, jsonPath);
        return PoseLoadResult::ParseError;
    }
    if (!versionSeen) {
        fprintf(stderr, "[VISUAL_DIFF v1] event=pose_parse_failed reason=missing_field_version path=%s\n", jsonPath);
        return PoseLoadResult::ParseError;
    }
    if (version != 3) {
        fprintf(stderr, "[VISUAL_DIFF v1] event=pose_parse_failed reason=version_mismatch got=%d expected=3 path=%s\n",
                version, jsonPath);
        return PoseLoadResult::ParseError;
    }
    if (!foundMission) {
        fprintf(stderr, "[VISUAL_DIFF v1] event=pose_missing reason=mission_not_authored mission=%s path=%s\n",
                missionKey, jsonPath);
        return PoseLoadResult::MissionNotFound;
    }
    if (!seen.allPresent()) {
        fprintf(stderr, "[VISUAL_DIFF v1] event=pose_parse_failed reason=missing_field_%s mission=%s path=%s\n",
                seen.firstMissing(), missionKey, jsonPath);
        return PoseLoadResult::ParseError;
    }
    if (out) *out = scratch;
    return PoseLoadResult::Ok;

parse_fail:
    free(buf);
    fprintf(stderr, "[VISUAL_DIFF v1] event=pose_parse_failed reason=%s details=\"%s\" path=%s\n",
            L.errReason[0] ? L.errReason : "bad_token",
            L.errDetails,
            jsonPath);
    return PoseLoadResult::ParseError;
}

// =============================================================================
// State machine + lifecycle hooks (Phase 1 Step 1.3)
// =============================================================================
//
// Self-contained: VisualDiff owns its own frame counter, independent of
// SmokeMode::g_frameCount (which only ticks under MC2_SMOKE_MODE=1). The
// only SmokeMode coupling is reading missionHasStarted() to detect when
// "frame 0 of the mission" has occurred — that returns false on non-smoke
// runs too, so capture mode requires smoke mode in practice. Documented
// in the plan's Phase 1 verification section.

namespace {

// Lazy-cached env reads.
struct EnvCache {
    bool captureRead   = false;
    bool capture       = false;
    bool recordRead    = false;
    bool record        = false;
};
EnvCache s_env;

// State machine.
enum class Phase { Waiting, TeleportArmed, Done };

struct State {
    int   localFrame          = 0;
    int   missionStartFrame   = -1;
    bool  missionStartObserved = false;
    Phase phase               = Phase::Waiting;

    // Pose lookup result is sticky once attempted; -1=untried, 0=ok, 1=missing,
    // 2=parse-failed. The state machine inspects this to decide whether to
    // proceed with capture (only on 0).
    int   poseLookup          = -1;
    PoseData pose             = {};

    // Resolved per-mission frame parameters (env override or pose JSON or default).
    int frameN        = 90;
    int settleFrames  = 3;
    int maxFrames     = 150;  // frameN + 60 by default; recomputed when frameN known
};
State s_state;

// Resolve env-override-or-default for an int.
int envInt(const char* name, int dflt) {
    const char* v = getenv(name);
    if (!v || !v[0]) return dflt;
    return atoi(v);
}

// Cached missionPath / missionKey for record-pose flow (Step 1.6 uses them).
const char* envOrEmpty(const char* name) {
    const char* v = getenv(name);
    return v ? v : "";
}

// Wraps the bridge call so the call site reads naturally with PoseData.
// The bridge namespace is forward-declared at global scope (top of this file).
bool teleportCamera(const PoseData& p) {
    return ::VisualDiffCameraBridge::applyPose(
        p.position[0], p.position[1],
        p.cameraRotation, p.cameraRotationWorld,
        p.cameraTilt, p.fov);
}

void doCapture(int viewportW, int viewportH) {
    const char* outPath = envOrEmpty("MC2_VISUAL_DIFF_OUT");
    if (!outPath[0]) {
        fprintf(stderr, "[VISUAL_DIFF v1] event=capture_skipped reason=no_out_path\n");
        return;
    }
    bool ok = gos::screenshot::writeTGA(outPath, viewportW, viewportH);
    if (ok) {
        fprintf(stderr, "[VISUAL_DIFF v1] event=capture_done out=%s w=%d h=%d\n",
                outPath, viewportW, viewportH);
    } else {
        fprintf(stderr, "[VISUAL_DIFF v1] event=capture_failed reason=write_tga out=%s\n",
                outPath);
    }
    fflush(stderr);
}

}  // anonymous namespace

bool isCaptureEnabled() {
    if (!s_env.captureRead) {
        const char* v = getenv("MC2_VISUAL_DIFF_CAPTURE");
        s_env.capture = (v && v[0] && v[0] != '0');
        s_env.captureRead = true;
    }
    return s_env.capture;
}

bool isRecordEnabled() {
    if (!s_env.recordRead) {
        const char* v = getenv("MC2_VISUAL_DIFF_RECORD");
        s_env.record = (v && v[0] && v[0] != '0');
        s_env.recordRead = true;
    }
    return s_env.record;
}

void onMissionLoad() {
    // Reset state for a fresh mission. Currently uncalled from external code;
    // exposed for future engine-side wiring. Env caches are NOT reset (env
    // is process-static).
    s_state = State{};
}

void onHotkeyRecordPose() {
    // Step 1.6 implements the record body. Stub for Step 1.3.
    if (!isRecordEnabled()) return;
    fprintf(stderr, "[VISUAL_DIFF v1] event=record_pose_stub reason=step_1_3_skeleton\n");
    fflush(stderr);
}

void onFrameTick(int viewportW, int viewportH) {
    if (!isCaptureEnabled()) return;
    if (s_state.phase == Phase::Done) return;

    s_state.localFrame++;

    // Lazy pose load on first tick after capture enabled.
    if (s_state.poseLookup < 0) {
        const char* jsonPath = getenv("MC2_VISUAL_DIFF_POSES");
        if (!jsonPath || !jsonPath[0]) {
            jsonPath = "tests/smoke/visual_diff/mission_camera_poses.json";
        }
        const char* missionKey = envOrEmpty("MC2_VISUAL_DIFF_MISSION");
        if (!missionKey[0]) {
            fprintf(stderr, "[VISUAL_DIFF v1] event=pose_missing reason=no_mission_env\n");
            s_state.poseLookup = 1;
            s_state.phase = Phase::Done;
            return;
        }

        PoseLoadResult r = loadPose(jsonPath, missionKey, &s_state.pose);
        switch (r) {
            case PoseLoadResult::Ok:
                s_state.poseLookup = 0;
                // env override takes precedence; otherwise from pose JSON
                s_state.frameN       = envInt("MC2_VISUAL_DIFF_FRAME_N",       s_state.pose.frameN);
                s_state.settleFrames = envInt("MC2_VISUAL_DIFF_SETTLE_FRAMES", s_state.pose.settle_frames);
                s_state.maxFrames    = envInt("MC2_VISUAL_DIFF_MAX_FRAMES",    s_state.frameN + 60);
                fprintf(stderr,
                        "[VISUAL_DIFF v1] event=pose_loaded mission=%s "
                        "frameN=%d settle_frames=%d maxFrames=%d\n",
                        missionKey, s_state.frameN, s_state.settleFrames, s_state.maxFrames);
                fflush(stderr);
                break;
            case PoseLoadResult::FileNotFound:
            case PoseLoadResult::MissionNotFound:
                s_state.poseLookup = 1;
                s_state.phase = Phase::Done;
                return;
            case PoseLoadResult::ParseError:
                s_state.poseLookup = 2;
                s_state.phase = Phase::Done;
                return;
        }
    }
    if (s_state.poseLookup != 0) return;

    // Snapshot mission-start frame on first tick after missionHasStarted() goes true.
    if (!s_state.missionStartObserved) {
        if (SmokeMode::missionHasStarted()) {
            s_state.missionStartObserved = true;
            s_state.missionStartFrame    = s_state.localFrame;
            fprintf(stderr,
                    "[VISUAL_DIFF v1] event=mission_start_observed local_frame=%d\n",
                    s_state.localFrame);
            fflush(stderr);
        }
        return;
    }

    int framesSinceStart = s_state.localFrame - s_state.missionStartFrame;

    // Timeout — exit cleanly with code 4 so the Python harness can distinguish.
    if (framesSinceStart > s_state.maxFrames) {
        fprintf(stderr,
                "[VISUAL_DIFF v1] event=capture_timeout frames_since_start=%d max_frames=%d\n",
                framesSinceStart, s_state.maxFrames);
        fflush(stderr);
        fflush(stdout);
        // Direct exit; engine state at this point is irrecoverable for our
        // purposes (we've missed the capture window).
        exit(4);
    }

    // Teleport phase: at frameN - settle_frames, fire the bridge teleport.
    // Bridge returns false if eye/land aren't ready; in that case stay in
    // Waiting and retry next tick (the mission may still be loading).
    if (s_state.phase == Phase::Waiting &&
        framesSinceStart >= s_state.frameN - s_state.settleFrames) {
        if (teleportCamera(s_state.pose)) {
            s_state.phase = Phase::TeleportArmed;
        }
    }

    // Capture phase: at frameN, capture pre-HUD framebuffer.
    if (s_state.phase == Phase::TeleportArmed &&
        framesSinceStart >= s_state.frameN) {
        doCapture(viewportW, viewportH);
        s_state.phase = Phase::Done;
    }
}

}  // namespace VisualDiff
