// BRAIN-MISSIONFIT-OPORD-CONSUMER-1: pure parser for carver mission.fit PrimaryOPORD blocks.
// See brain_missionfit_oporbd.h. Raw line scan — FitIniFile cannot see brace-blocks.
#include "brain_missionfit_oporbd.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

// Value token after "<key> =" on a line. Writes the trimmed token; returns true if non-empty.
bool lineKeyToken(const char* line, const char* key, char* out, size_t outsz) {
    const char* p = std::strstr(line, key);
    if (!p) return false;
    p += std::strlen(key);
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '=') return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'
           && *p != '}' && *p != ';' && i + 1 < outsz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

// Float after a single-char axis ('x'/'y') as in "x=3008.000". Boundary-aware: an axis char
// preceded by a letter is rejected (so "index=0" does NOT match 'x='). Returns true on success.
bool axisFloat(const char* line, char axis, float* out) {
    for (const char* p = std::strchr(line, axis); p; p = std::strchr(p + 1, axis)) {
        if (p > line && std::isalpha((unsigned char)p[-1])) continue;  // reject 'inde[x]='
        const char* q = p + 1;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != '=') continue;
        ++q;
        while (*q == ' ' || *q == '\t') ++q;
        char* end = nullptr;
        float v = std::strtof(q, &end);
        if (end == q) continue;
        *out = v;
        return true;
    }
    return false;
}

void setType(MissionFitOpord& r, const char* tok) {
    std::strncpy(r.primaryType, tok, sizeof(r.primaryType) - 1);
    r.primaryType[sizeof(r.primaryType) - 1] = '\0';
}

} // namespace

int parseMissionFitOpords(const char* text, MissionFitOpord* out, int maxOut) {
    if (!text || !out || maxOut <= 0) return 0;

    int count = 0;
    int curWarrior = -1;
    bool inPrimary = false;
    int depth = 0;
    MissionFitOpord cur;
    char buf[512];
    char tok[24];

    const char* line = text;
    while (*line) {
        const char* nl = line;
        while (*nl && *nl != '\n') ++nl;
        size_t len = (size_t)(nl - line);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        std::memcpy(buf, line, len);
        buf[len] = '\0';
        line = (*nl) ? nl + 1 : nl;

        const char* lb = buf;
        while (*lb == ' ' || *lb == '\t') ++lb;

        // Section header [WarriorN] (or any [Section]) — resets OPORD scope.
        if (lb[0] == '[') {
            inPrimary = false;
            depth = 0;
            curWarrior = (std::strncmp(lb, "[Warrior", 8) == 0)
                       ? (int)std::strtol(lb + 8, nullptr, 10) : -1;
            continue;
        }

        if (!inPrimary) {
            if (curWarrior >= 0 && std::strstr(buf, "PrimaryOPORD")) {
                cur.warriorIndex = curWarrior;
                cur.primaryType[0] = '\0';
                cur.loop = false;
                cur.waypointCount = 0;
                inPrimary = true;
                depth = 0;
                for (const char* q = buf; *q; ++q) { if (*q == '{') ++depth; else if (*q == '}') --depth; }
                if (lineKeyToken(buf, "type", tok, sizeof(tok))) setType(cur, tok);
                if (depth <= 0) {                         // single-line PrimaryOPORD { … }
                    if (count < maxOut) out[count++] = cur;
                    inPrimary = false;
                    depth = 0;
                }
            }
            continue;
        }

        // Inside a multi-line PrimaryOPORD body.
        for (const char* q = buf; *q; ++q) { if (*q == '{') ++depth; else if (*q == '}') --depth; }
        if (cur.primaryType[0] == '\0' && lineKeyToken(buf, "type", tok, sizeof(tok))) setType(cur, tok);
        if (lineKeyToken(buf, "loop", tok, sizeof(tok))) cur.loop = (std::strcmp(tok, "true") == 0);
        if (std::strstr(buf, "Waypoint") && cur.waypointCount < kMissionFitMaxWaypoints) {
            float x, y;
            if (axisFloat(buf, 'x', &x) && axisFloat(buf, 'y', &y)) {
                cur.waypoints[cur.waypointCount].x = x;
                cur.waypoints[cur.waypointCount].y = y;
                ++cur.waypointCount;
            }
        }
        if (depth <= 0) {                                 // PrimaryOPORD closing brace
            if (count < maxOut) out[count++] = cur;
            inPrimary = false;
            depth = 0;
        }
    }
    return count;
}
