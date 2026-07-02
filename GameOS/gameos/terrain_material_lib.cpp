// TERRAIN-MATERIAL-LIB-1: see terrain_material_lib.h for scope/precedence.
// Mirrors visual_tuning_profile.cpp's TinyJson reader 1:1 (fopen + minimal
// parser + gos_Set* setters); this file owns a SEPARATE JSON schema.

#include "terrain_material_lib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>

// Renderer setter/getter forward declarations (definitions in gameos_graphics.cpp).
extern void  gos_SetTerrainMatTiling(float rock, float grass, float dirt, float concrete, float snow);
extern void  gos_SetTerrainMatNormalBoost(float rock, float grass, float dirt, float concrete);
extern void  gos_SetTerrainTintRock(float r, float g, float b);
extern void  gos_SetTerrainTintGrass(float r, float g, float b);
extern void  gos_SetTerrainTintDirt(float r, float g, float b);
extern void  gos_SetTerrainTintConcrete(float r, float g, float b);
extern void  gos_SetTerrainTintSnow(float r, float g, float b);
extern void  gos_SetTerrainMatRoughness(float rock, float grass, float dirt, float concrete);
extern void  gos_SetTerrainMatAO(float rock, float grass, float dirt, float concrete);
extern void  gos_SetTerrainTintStrengthScale(float s);
extern void  gos_SetTerrainControlAlbedoStrength(float s);  // TERRAIN-CONTROLMAP-ALBEDO-1
extern void  gos_SetTerrainDetailParams(float tiling, float strength);
extern void  gos_SetTerrainSnowBrightnessDampen(float v);
extern void  gos_SetTerrainClassGrass(float gMinusRLo, float gMinusRHi, float gBrightLo, float gBrightHi);
extern void  gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi);

namespace {

// Minimal JSON parser for flat float-value objects, two levels deep.
// Same grammar as visual_tuning_profile.cpp's TinyJson (kept independent
// on purpose -- this is a separate, small reader, not worth sharing a TU).
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

    // Parse {"key": float, ...} — nested objects/arrays are skipped (v1 uses
    // flat keys exclusively; "layers" as an array is a v2 nicety per recon).
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
};

static bool envIsSet(const char* name) {
    const char* v = getenv(name);
    return v && v[0] != '\0';
}

static float getf(const std::map<std::string,float>& m, const char* key, float def) {
    auto it = m.find(key);
    return (it != m.end()) ? it->second : def;
}

} // namespace

// TERRAIN-CONTROLMAP-ALBEDO-1: independent gate from MC2_TERRAIN_MATERIAL_LIB
// (per USER RULING -- this slice ships its own killswitch). Applies the
// controlAlbedoStrength key from the SAME terrain_materials.json (or
// MC2_TERRAIN_MATERIAL_LIB_FILE override) even when MC2_TERRAIN_MATERIAL_LIB
// itself is off, so authors can turn on just the albedo-repaint knob without
// opting into the rest of the material-lib tuning surface. Precedence:
// env MC2_TERRAIN_CONTROLMAP_ALBEDO_STRENGTH wins over JSON, JSON wins over
// the shipped 0.7 default. Gate OFF -> member stays at its 0.0f C++
// initializer -> byte-identical.
static void applyControlAlbedoStrength(const std::map<std::string,float>& v, bool jsonLoaded) {
    if (!envIsSet("MC2_TERRAIN_CONTROLMAP_ALBEDO")) return;  // gate OFF -> no-op, member stays 0.0f

    float strength = jsonLoaded ? getf(v, "controlAlbedoStrength", 0.7f) : 0.7f;
    if (const char* envStrength = getenv("MC2_TERRAIN_CONTROLMAP_ALBEDO_STRENGTH")) {
        if (envStrength[0] != '\0') strength = (float)atof(envStrength);  // env wins over JSON
    }
    gos_SetTerrainControlAlbedoStrength(strength);
    fprintf(stderr, "[TerrainControlAlbedo] gate ON -- strength=%.3f\n", strength);
}

void terrainMaterials_apply(const char* /*missionName*/) {
    const char* pathEnv = getenv("MC2_TERRAIN_MATERIAL_LIB_FILE");
    const char* path = (pathEnv && pathEnv[0]) ? pathEnv : "data/terrain_materials.json";
    bool materialLibGateOn = envIsSet("MC2_TERRAIN_MATERIAL_LIB");
    bool controlAlbedoGateOn = envIsSet("MC2_TERRAIN_CONTROLMAP_ALBEDO");

    if (!materialLibGateOn && !controlAlbedoGateOn) return;  // both gates OFF -> silent no-op

    FILE* f = fopen(path, "r");
    if (!f) {
        if (materialLibGateOn)
            fprintf(stderr, "[TerrainMaterialLib] gate ON but no file at '%s' -- no-op\n", path);
        applyControlAlbedoStrength({}, /*jsonLoaded=*/false);  // shipped-default strength
        return;  // missing file = silent no-op (matches visual_tuning.json convention)
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 512 * 1024) {
        fclose(f);
        applyControlAlbedoStrength({}, /*jsonLoaded=*/false);
        return;
    }

    std::string buf((size_t)sz, '\0');
    fread(&buf[0], 1, (size_t)sz, f);
    fclose(f);

    std::map<std::string,float> v;
    TinyJson jp{ buf.c_str(), buf.c_str() + buf.size() };
    jp.floatObj(v);

    applyControlAlbedoStrength(v, /*jsonLoaded=*/true);

    if (!materialLibGateOn) return;  // TERRAIN-CONTROLMAP-ALBEDO-1 gate handled above independently

    // --- Byte-identity defaults: EXACT current hardcoded constants (recon
    // table). If the JSON omits a key, the corresponding gos_Set* call below
    // still fires with this default, so gate-ON + a missing/partial JSON is
    // still byte-identical to gate-OFF. ---

    // Per-layer tiling (rock, grass, dirt, concrete) + snow tiling.
    gos_SetTerrainMatTiling(
        getf(v, "rock_tiling",     3.0f),
        getf(v, "grass_tiling",    2.0f),
        getf(v, "dirt_tiling",     1.0f),
        getf(v, "concrete_tiling", 6.0f),
        getf(v, "snow_tiling",     1.0f));

    // Per-layer normal boost.
    gos_SetTerrainMatNormalBoost(
        getf(v, "rock_normalBoost",     0.9f),
        getf(v, "grass_normalBoost",    0.5f),
        getf(v, "dirt_normalBoost",     1.1f),
        getf(v, "concrete_normalBoost", 2.5f));

    // Tints (rock/grass/dirt were already uniforms; concrete/snow are the
    // TWO promoted frag literals -- defaults MUST equal the exact literals).
    gos_SetTerrainTintRock(
        getf(v, "rock_tint_r", 0.36f), getf(v, "rock_tint_g", 0.37f), getf(v, "rock_tint_b", 0.40f));
    gos_SetTerrainTintGrass(
        getf(v, "grass_tint_r", 0.35f), getf(v, "grass_tint_g", 0.42f), getf(v, "grass_tint_b", 0.25f));
    gos_SetTerrainTintDirt(
        getf(v, "dirt_tint_r", 0.48f), getf(v, "dirt_tint_g", 0.42f), getf(v, "dirt_tint_b", 0.33f));
    gos_SetTerrainTintConcrete(
        getf(v, "concrete_tint_r", 0.55f), getf(v, "concrete_tint_g", 0.53f), getf(v, "concrete_tint_b", 0.50f));
    gos_SetTerrainTintSnow(
        getf(v, "snow_tint_r", 0.75f), getf(v, "snow_tint_g", 0.78f), getf(v, "snow_tint_b", 0.84f));

    // Per-layer roughness/AO (NEW; neutral 1.0 defaults).
    gos_SetTerrainMatRoughness(
        getf(v, "rock_roughness",     1.0f),
        getf(v, "grass_roughness",    1.0f),
        getf(v, "dirt_roughness",     1.0f),
        getf(v, "concrete_roughness", 1.0f));
    gos_SetTerrainMatAO(
        getf(v, "rock_ao",     1.0f),
        getf(v, "grass_ao",    1.0f),
        getf(v, "dirt_ao",     1.0f),
        getf(v, "concrete_ao", 1.0f));

    // Detail tiling/strength (global, not per-layer).
    gos_SetTerrainDetailParams(
        getf(v, "detail_tiling",   1.0f),
        getf(v, "detail_strength", 1.0f));

    // Global tint blend scalar.
    gos_SetTerrainTintStrengthScale(getf(v, "tintStrengthScale", 1.0f));

    // Snow brightness dampen: env wins over JSON (USER RULING). The renderer
    // member already initializes from MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN at
    // static-init time (gameos_graphics.cpp terrain_snow_brightness_dampen_);
    // only apply the JSON value when that env var is NOT set, so we never
    // clobber an explicit env override.
    if (!envIsSet("MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN")) {
        gos_SetTerrainSnowBrightnessDampen(getf(v, "snow_brightnessDampen", 0.78f));
    }

    // Classify thresholds (grass/dirt). Landmine (recon): Sand_M24 widens the
    // dirt gate IN-SHADER (g_terrainMaterialProfile) on top of whatever base
    // thresholds we set here -- JSON sets the base, the shader profile still
    // widens it afterward, so they compose without conflict.
    gos_SetTerrainClassGrass(
        getf(v, "classify_grass_gMinusRLo", -0.02f),
        getf(v, "classify_grass_gMinusRHi",  0.06f),
        getf(v, "classify_grass_gBrightLo",  0.22f),
        getf(v, "classify_grass_gBrightHi",  0.40f));
    gos_SetTerrainClassDirt(
        getf(v, "classify_dirt_rMinusGLo", -0.02f),
        getf(v, "classify_dirt_rMinusGHi",  0.06f),
        getf(v, "classify_dirt_rBrightLo",  0.22f),
        getf(v, "classify_dirt_rBrightHi",  0.45f));

    fprintf(stderr, "[TerrainMaterialLib] applied %d keys from '%s'\n", (int)v.size(), path);
}
