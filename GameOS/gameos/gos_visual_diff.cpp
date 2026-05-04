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

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

}  // namespace VisualDiff
