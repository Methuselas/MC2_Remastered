// MISSION-VISUAL-TUNING-1: optional per-mission renderer tuning profiles.
// See docs/visual-tuning-profiles.md.

#include "visual_tuning_profile.h"
#include "ibl_sh_runtime.h"  // g_iblShStrength (extern float)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>

// Renderer function forward declarations.
// Definitions in gameos_graphics.cpp / gos_postprocess.cpp / gos_particle_bridge.cpp.
extern float gos_GetTerrainShadowSoftness();
extern void  gos_SetTerrainShadowSoftness(float s);
extern void  gos_SetTerrainLightingV1Strength(float s);
extern void  gos_SetTerrainLightingV2Floor(float f);
extern float gos_GetWaterSkyTintStrength();
extern void  gos_SetWaterSkyTintStrength(float v);
extern float gos_GetExposure();
extern void  gos_SetExposure(float v);
extern void  gos_SetBloomThreshold(float v);
extern void  gos_SetBloomIntensity(float v);
extern void  gos_SetSsaoRadius(float v);
extern void  gos_SetSsaoStrength(float v);
extern void  gos_SetSsaoBias(float v);
extern "C" void  gos_vfx_setBrightness(float v);
extern "C" void  gos_vfx_setAdditiveBrightness(float v);
extern "C" float gos_vfx_getBrightness(void);
extern "C" float gos_vfx_getAdditiveBrightness(void);
extern float     gos_GetTerrainLightingV1Strength();
extern float     gos_GetTerrainLightingV2Floor();

namespace {

// Minimal JSON parser for flat float-value objects, two levels deep.
// Handles {"key": 1.0, ...} and {"mission": {"key": 1.0, ...}, ...}.
struct TinyJson {
    const char* p;
    const char* e;

    void ws() { while (p < e && (unsigned char)*p <= 32) p++; }

    bool eat(char c) { ws(); if (p < e && *p == c) { p++; return true; } return false; }

    std::string str() {
        ws();
        if (p >= e || *p != '"') return {};
        p++;
        const char* s = p;
        while (p < e && *p != '"' && *p != '\\') p++;
        std::string r(s, (size_t)(p - s));
        if (p < e) p++;  // skip closing "
        return r;
    }

    float num() {
        ws(); char* ep = nullptr;
        float v = (float)strtod(p, &ep);
        if (ep != p) p = ep;
        return v;
    }

    void skipVal() {
        ws(); if (p >= e) return;
        if (*p == '{' || *p == '[') {
            char op = *p, cl = (op == '{') ? '}' : ']';
            int d = 0;
            do {
                if (p >= e) break;
                if (*p == '"') { str(); continue; }
                char c = *p++;
                if (c == op) d++;
                else if (c == cl && --d == 0) return;
            } while (true);
        } else if (*p == '"') {
            str();
        } else {
            char* ep = nullptr; strtod(p, &ep);
            if (ep != p) { p = ep; }
            else while (p < e && *p != ',' && *p != '}' && *p != ']' && (unsigned char)*p > 32) p++;
        }
    }

    // Parse {"key": float, ...} — nested objects are skipped.
    void floatObj(std::map<std::string,float>& out) {
        if (!eat('{')) return;
        for (;;) {
            ws(); if (p >= e || *p == '}') { if (p < e) p++; break; }
            auto k = str(); if (!eat(':')) break;
            ws();
            if (p < e && (*p == '{' || *p == '[')) skipVal();
            else if (!k.empty()) out[k] = num();
            else skipVal();
            eat(',');
        }
    }

    // Parse {"mission": {floatObj}, ...}
    void missionMap(std::map<std::string, std::map<std::string,float>>& out) {
        if (!eat('{')) return;
        for (;;) {
            ws(); if (p >= e || *p == '}') { if (p < e) p++; break; }
            auto k = str(); if (!eat(':')) break;
            if (!k.empty()) {
                std::map<std::string,float> obj;
                floatObj(obj);
                out[k] = std::move(obj);
            } else skipVal();
            eat(',');
        }
    }

    // Parse root: {"defaults": {...}, "missions": {...}, unknown: skip}
    void root(std::map<std::string,float>& defs,
              std::map<std::string, std::map<std::string,float>>& missions) {
        if (!eat('{')) return;
        for (;;) {
            ws(); if (p >= e || *p == '}') break;
            auto k = str(); if (!eat(':')) break;
            if      (k == "defaults")  floatObj(defs);
            else if (k == "missions")  missionMap(missions);
            else                       skipVal();
            eat(',');
        }
    }
};

static bool s_hasFile     = false;
static int  s_appliedKeys = 0;
static char s_profilePath[256]  = "data/visual_tuning.json";
static char s_activeMission[80] = "";

static bool envIsSet(const char* name) {
    const char* v = getenv(name);
    return v && v[0] != '\0';
}

static void applyKey(const char* key, float val, int& count) {
    if (strcmp(key, "exposure") == 0) {
        gos_SetExposure(val);
        count++;
    } else if (strcmp(key, "shadowSoftness") == 0) {
        gos_SetTerrainShadowSoftness(val);
        count++;
    } else if (strcmp(key, "terrainLightingV1Strength") == 0) {
        gos_SetTerrainLightingV1Strength(val);
        count++;
    } else if (strcmp(key, "terrainLightingV2Floor") == 0) {
        gos_SetTerrainLightingV2Floor(val);
        count++;
    } else if (strcmp(key, "staticPropIblStrength") == 0) {
        if (!envIsSet("MC2_STATIC_PROP_IBL_SH_STRENGTH")) {
            g_iblShStrength = val < 0.0f ? 0.0f : (val > 3.0f ? 3.0f : val);
            count++;
        }
    } else if (strcmp(key, "waterSkyTintStrength") == 0) {
        if (!envIsSet("MC2_WATER_SKYTINT")) {
            gos_SetWaterSkyTintStrength(val);
            count++;
        }
    } else if (strcmp(key, "vfxBrightness") == 0) {
        if (!envIsSet("MC2_TUNE_VFX_BRIGHTNESS")) {
            gos_vfx_setBrightness(val);
            count++;
        }
    } else if (strcmp(key, "vfxAdditiveBrightness") == 0) {
        if (!envIsSet("MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS")) {
            gos_vfx_setAdditiveBrightness(val);
            count++;
        }
    } else if (strcmp(key, "bloomThreshold") == 0) {
        // BLOOM-MVP-1: per-mission bloom extract threshold. Only visible when
        // MC2_HDR_POST + MC2_BLOOM are on; harmless otherwise (writes member).
        gos_SetBloomThreshold(val);
        count++;
    } else if (strcmp(key, "bloomIntensity") == 0) {
        gos_SetBloomIntensity(val);
        count++;
    } else if (strcmp(key, "aoRadius") == 0) {
        // SSAO-GTAO-LITE-MVP-1: per-mission AO tunables. Only visible when
        // MC2_SSAO is on; harmless otherwise (writes member).
        gos_SetSsaoRadius(val);
        count++;
    } else if (strcmp(key, "aoStrength") == 0) {
        gos_SetSsaoStrength(val);
        count++;
    } else if (strcmp(key, "aoBias") == 0) {
        gos_SetSsaoBias(val);
        count++;
    } else {
        static std::map<std::string,bool> s_warned;
        if (!s_warned[key]) {
            s_warned[key] = true;
            fprintf(stderr, "[VisualTuning] unknown key '%s' -- ignored\n", key);
        }
    }
}

static void writeJsonFloatObj(FILE* f, const std::map<std::string,float>& obj, int depth) {
    const char* inner = (depth == 1) ? "    " : "      ";
    const char* close = (depth == 1) ? "  "   : "    ";
    fprintf(f, "{\n");
    bool first = true;
    for (auto& kv : obj) {
        if (!first) fprintf(f, ",\n");
        first = false;
        fprintf(f, "%s\"%s\": %g", inner, kv.first.c_str(), kv.second);
    }
    fprintf(f, "\n%s}", close);
}

static void writeJson(FILE* f,
                      const std::map<std::string,float>& defs,
                      const std::map<std::string, std::map<std::string,float>>& missions) {
    fprintf(f, "{\n  \"defaults\": ");
    writeJsonFloatObj(f, defs, 1);
    fprintf(f, ",\n  \"missions\": {\n");
    bool firstM = true;
    for (auto& mkv : missions) {
        if (!firstM) fprintf(f, ",\n");
        firstM = false;
        fprintf(f, "    \"%s\": ", mkv.first.c_str());
        writeJsonFloatObj(f, mkv.second, 2);
    }
    fprintf(f, "\n  }\n}\n");
}

} // namespace

void visualTuning_applyProfile(const char* missionName) {
    s_hasFile    = false;
    s_appliedKeys = 0;
    s_activeMission[0] = '\0';
    if (missionName && missionName[0])
        strncpy(s_activeMission, missionName, sizeof(s_activeMission) - 1);

    const char* pathEnv = getenv("MC2_VISUAL_TUNING_FILE");
    const char* path = (pathEnv && pathEnv[0]) ? pathEnv : "data/visual_tuning.json";
    strncpy(s_profilePath, path, sizeof(s_profilePath) - 1);

    FILE* f = fopen(path, "r");
    if (!f) return;  // missing file = silent no-op

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); return; }

    std::string buf((size_t)sz, '\0');
    fread(&buf[0], 1, (size_t)sz, f);
    fclose(f);
    s_hasFile = true;

    std::map<std::string,float> defs;
    std::map<std::string, std::map<std::string,float>> missions;
    TinyJson jp{ buf.c_str(), buf.c_str() + buf.size() };
    jp.root(defs, missions);

    int count = 0;
    for (auto& kv : defs) applyKey(kv.first.c_str(), kv.second, count);

    if (missionName && missionName[0]) {
        auto it = missions.find(missionName);
        if (it != missions.end())
            for (auto& kv : it->second) applyKey(kv.first.c_str(), kv.second, count);
    }

    s_appliedKeys = count;
    fprintf(stderr, "[VisualTuning] mission='%s' applied %d keys from '%s'\n",
            missionName ? missionName : "(none)", count, path);
}

const char* visualTuning_getProfilePath()     { return s_profilePath; }
const char* visualTuning_getActiveMission()   { return s_activeMission; }
bool        visualTuning_hasProfileFile()     { return s_hasFile; }
int         visualTuning_getAppliedKeyCount() { return s_appliedKeys; }

bool visualTuning_saveCurrentToMission() {
    if (!s_activeMission[0]) return false;

    // Snapshot all current tunable values.
    std::map<std::string,float> current;
    current["exposure"]                  = gos_GetExposure();
    current["shadowSoftness"]            = gos_GetTerrainShadowSoftness();
    current["terrainLightingV1Strength"] = gos_GetTerrainLightingV1Strength();
    current["terrainLightingV2Floor"]    = gos_GetTerrainLightingV2Floor();
    current["staticPropIblStrength"]     = g_iblShStrength;
    current["waterSkyTintStrength"]      = gos_GetWaterSkyTintStrength();
    current["vfxBrightness"]             = gos_vfx_getBrightness();
    current["vfxAdditiveBrightness"]     = gos_vfx_getAdditiveBrightness();

    // Read existing file to preserve other missions and defaults.
    std::map<std::string,float> defs;
    std::map<std::string, std::map<std::string,float>> missions;
    FILE* f = fopen(s_profilePath, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f); rewind(f);
        if (sz > 0 && sz <= 512 * 1024) {
            std::string buf((size_t)sz, '\0');
            fread(&buf[0], 1, (size_t)sz, f);
            TinyJson jp{ buf.c_str(), buf.c_str() + buf.size() };
            jp.root(defs, missions);
        }
        fclose(f);
    }

    missions[s_activeMission] = current;

    FILE* out = fopen(s_profilePath, "w");
    if (!out) {
        fprintf(stderr, "[VisualTuning] cannot write '%s'\n", s_profilePath);
        return false;
    }
    writeJson(out, defs, missions);
    fclose(out);

    // Keep state consistent: next Reset to Profile will re-apply what we just saved.
    s_hasFile    = true;
    s_appliedKeys = (int)current.size();
    fprintf(stderr, "[VisualTuning] saved %d keys for mission '%s' to '%s'\n",
            (int)current.size(), s_activeMission, s_profilePath);
    return true;
}
