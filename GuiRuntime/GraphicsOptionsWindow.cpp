#include "GraphicsOptionsWindow.h"
#include "imgui.h"

#include "../GameOS/gameos/gos_postprocess.h"
#include "../GameOS/gameos/view_uniforms_gl.h"
#include "../RenderCore/RenderResourceRegistry.h"
#include "gameos.hpp"
#include "../GameOS/gameos/gos_static_prop_killswitch.h"
#include "../GameOS/gameos/gos_mech_killswitch.h"
#include "../GameOS/gameos/ibl_sh_runtime.h"     // g_iblShStrength, g_pbrV1Strength, g_pbrV1RoughnessOverride*
#include "../mclib/projectz_overlay.h"
#include "draw_packet_emitter.h"       // DrawPacketsDebugSnapshot g_dpSnapshot
#include "StaticPropTypeDesc.h"        // RenderCore::StaticPropTypeDesc
// Forward-declare the two batcher accessors we need rather than pulling in
// gos_static_prop_batcher.h, which transitively requires Stuff.hpp / tgl.h.
bool     batcher_getStaticPropTypeDesc(uint32_t typeId, RenderCore::StaticPropTypeDesc* out);
uint32_t batcher_getStaticPropTypeDescCount();
// Terrain PBR parameter accessors (defined in gameos_graphics.cpp).
void  gos_SetTerrainMatNormalBoost(float rock, float grass, float dirt, float concrete);
void  gos_GetTerrainMatNormalBoost(float* rock, float* grass, float* dirt, float* concrete);
void  gos_SetTerrainTintStrengthScale(float s);
float gos_GetTerrainTintStrengthScale();
// TERRAIN-TUNING-UI-1 / TERRAIN-LIGHTING-1 — consolidated tunables (the
// Object Inspector "Terrain Pass" panel used to mirror these; they live
// here now alongside the rest of the terrain tuning stack).
void  gos_SetTerrainNormalsFromHeightStrength(float s);
float gos_GetTerrainNormalsFromHeightStrength();
void  gos_SetTerrainLightingV1Strength(float s);
float gos_GetTerrainLightingV1Strength();
void  gos_SetTerrainLightingV2Floor(float f);
float gos_GetTerrainLightingV2Floor();
// TERRAIN-RESAMPLE-1 — height-tex source/render/factor accessors + live
// factor setter. C-linkage (declared extern "C" in gos_terrain_height_tex.h).
extern "C" {
    int  __stdcall gos_terrainHeightSourceSide(void);
    int  __stdcall gos_terrainHeightTexSide(void);
    int  __stdcall gos_terrainHeightResampleFactor(void);
    void __stdcall gos_setTerrainHeightResampleFactor(int factor);
}

#include <cstdio>
#include <cmath>
#include <cstdlib>   // getenv

namespace GraphicsOptionsWindow {

static bool s_open = false;

void setOpen(bool open) { s_open = open; }
bool isOpen()           { return s_open; }

// ─────────────────────────────────────────────────────────────────────────────
// Terrain debug mode table — mirrors gos_terrain.frag branch logic exactly.
// ─────────────────────────────────────────────────────────────────────────────
struct TerrainMode {
    float       value;
    const char* name;
    const char* channels;   // null = no sub-line
};

static const TerrainMode kTerrainModes[] = {
    //  value    name                     channel breakdown
    {  0.0f, "OFF",               nullptr },
    // ── Visual modes ──────────────────────────────────────────────────────────
    {  1.0f, "Depth Comparison",  "R = rasterized depth  |  G = undisplaced depth  (range 0.85–1.0)" },
    {  2.0f, "Raw Colormap",      "terrain colormap tex1, unlit" },
    {  3.0f, "Blurred Colormap",  "LOD-tiered blur: far=1-tap, mid=5-tap, near=9-tap disc" },
    {  4.0f, "Material Weights",  "R = rock  |  G = grass  |  B = dirt  (classification weights)" },
    {  5.0f, "Normal Lighting",   "diffuse contribution (0.35–1.20 remapped to 0–1)" },
    {  6.0f, "Shadow Factor",     "PCF static shadow × dynamic shadow" },
    {  7.0f, "Cloud Shadow",      "FBM cloud attenuation (0.92–1.0 remapped to 0–1)" },
    // ── Diagnostic modes ──────────────────────────────────────────────────────
    {  8.0f, "Cement Diagnostic", "R = layer valid  |  G = layer index/255  |  B = useCementAtlas==0" },
    {  9.0f, "Thin-Record",       "R = recipeIdx/255  |  G = flags/255  |  B = terrainHandle/255" },
    { 10.0f, "Height Normal",     "TERRAIN-NORMALS-FROM-HEIGHT-1: derived normal as RGB (centered around 0.5)" },
    { 11.0f, "Hemi Additive",     "TERRAIN-LIGHTING-2: V1 hemi contribution after V2 modulation (×4 for visibility)" },
    { -1.0f, "Tess Alive Probe",  "solid red = tessellation frag shader is running" },
};
static const int kTerrainModeCount = (int)(sizeof(kTerrainModes) / sizeof(kTerrainModes[0]));
static const int kDiagnosticStart  = 8; // index into kTerrainModes where diagnostics begin

// ─────────────────────────────────────────────────────────────────────────────
// Env gate table — baked at startup, shown read-only.
// defaultOn=true means the feature is enabled when the var is absent/non-zero.
// ─────────────────────────────────────────────────────────────────────────────
struct EnvGate {
    const char* var;
    const char* desc;
    bool        defaultOn;
    bool        inverted;   // true = presence of var DISABLES the feature
};

static const EnvGate kEnvGates[] = {
    { "MC2_GPU_DRIVEN",             "GPU-driven rendering master gate",          false, false },
    { "MC2_GPU_OBJECTS",            "GPU static prop batcher",                   true,  false },
    { "MC2_GPU_MECHS",              "GPU mech batcher",                          true,  false },
    { "MC2_GPU_CULL",               "GPU frustum cull",                          false, false },
    { "MC2_GPU_CULL_SUBSTRATE",     "GPU substrate (default-on)",                true,  false },
    { "MC2_GPU_PARTICLES",          "GPU particle system",                       false, false },
    { "MC2_MATERIAL_GPU",           "MaterialGpu lookup path",                   false, false },
    { "MC2_RENDER_WATER_FASTPATH",  "Fast water render path",                    false, false },
    { "MC2_OBJECT_ID_BUFFER",       "Object-ID picking buffer",                  false, false },
    { "MC2_HDRI_SKY",               "HDRI skybox",                               false, false },
    { "MC2_DISABLE_GOSFX",          "FX/particles (var disables)",               false, true  },
    { "MC2_RENDER_CONTRACT_ASSERT", "Render-contract GL state validation",        false, false },
    { "MC2_IMGUI",                  "Dear ImGui overlay (default-on)",           true,  false },
    { "MC2_GL_DEBUG",               "GL debug callback",                         false, false },
    { "MC2_DEBUG_RENDERER",         "Debug primitive renderer",                  false, false },
};
static const int kEnvGateCount = (int)(sizeof(kEnvGates) / sizeof(kEnvGates[0]));

// ─────────────────────────────────────────────────────────────────────────────

static void drawTerrainSection() {
    // ── Draw + wireframe ──────────────────────────────────────────────────────
    bool terrainDraw = gos_GetTerrainDrawEnabled();
    if (ImGui::Checkbox("Terrain Draw", &terrainDraw))
        gos_SetTerrainDrawEnabled(terrainDraw);

    // No public getter for wireframe state — track locally.
    static bool s_terrainWire = false;
    if (ImGui::Checkbox("Terrain Wireframe", &s_terrainWire))
        gos_SetTerrainWireframe(s_terrainWire);

    // ── Debug mode scrollable child ───────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextUnformatted("Surface Debug Mode");
    ImGui::SameLine();
    if (ImGui::SmallButton("OFF##toff")) gos_SetTerrainDebugMode(0.0f);

    const float lineH = ImGui::GetTextLineHeight();
    const float pad   = ImGui::GetStyle().ItemSpacing.y;

    float childH = (lineH + pad) * 2.0f                                  // OFF row
                 + (lineH * 2.2f + pad) * (float)(kDiagnosticStart - 1)  // visual 1..7
                 + (lineH + pad) * 1.2f                                   // separator + label
                 + (lineH * 2.2f + pad) * (float)(kTerrainModeCount - kDiagnosticStart);
    if (childH < 120.0f) childH = 120.0f;
    if (childH > 420.0f) childH = 420.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::BeginChild("##terrain_modes", ImVec2(0, childH), ImGuiChildFlags_Border);
    ImGui::PopStyleVar();

    float    curMode = gos_GetTerrainDebugMode();
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    bool        sentDiagHeader = false;

    for (int i = 0; i < kTerrainModeCount; ++i) {
        const TerrainMode& m = kTerrainModes[i];

        if (i == kDiagnosticStart && !sentDiagHeader) {
            ImGui::Separator();
            ImGui::TextDisabled("  Diagnostics");
            sentDiagHeader = true;
        }

        bool  active = fabsf(curMode - m.value) < 0.4f;
        bool  hasSub = (m.channels != nullptr);
        float rowH   = hasSub ? lineH * 2.2f : lineH * 1.3f;

        ImGui::PushID(i);

        if (active)
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.40f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));

        bool hit = ImGui::Selectable("##row", active, ImGuiSelectableFlags_AllowOverlap,
                                     ImVec2(0.0f, rowH));
        if (hit) gos_SetTerrainDebugMode(m.value);

        ImGui::PopStyleColor();
        if (active) ImGui::PopStyleColor();

        ImVec2 rmin = ImGui::GetItemRectMin();

        char heading[80];
        if (m.value < -0.5f)
            snprintf(heading, sizeof(heading), "[-1]  %s", m.name);
        else
            snprintf(heading, sizeof(heading), "[%d]  %s", (int)(m.value + 0.5f), m.name);

        ImU32 headCol = active ? IM_COL32(120, 255, 120, 255)
                               : IM_COL32(230, 230, 230, 255);
        dl->AddText(ImVec2(rmin.x + 6.0f, rmin.y + 3.0f), headCol, heading);

        if (hasSub)
            dl->AddText(ImVec2(rmin.x + 18.0f, rmin.y + lineH + 4.0f),
                        IM_COL32(140, 140, 140, 255), m.channels);

        ImGui::PopID();
    }

    ImGui::EndChild();
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawTerrainTuningSection() {
    // ── Tessellation ──────────────────────────────────────────────────────────
    // No public getters for tess params — mirror defaults here.
    static float s_tessLevel    = 4.0f;
    static float s_tessNear     = 200.0f;
    static float s_tessFar      = 2000.0f;

    ImGui::SeparatorText("Tessellation");
    bool tessChanged = false;
    tessChanged |= ImGui::SliderFloat("Level##tess",      &s_tessLevel, 1.0f, 16.0f, "%.1f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Inner/outer tess factor. Default: 4.0");
    tessChanged |= ImGui::SliderFloat("Near dist##tess",  &s_tessNear,  50.0f, 1000.0f, "%.0f wu");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance at which max tessellation begins. Default: 200");
    tessChanged |= ImGui::SliderFloat("Far dist##tess",   &s_tessFar,   500.0f, 8000.0f, "%.0f wu");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance at which tessellation falls off. Default: 2000");
    if (tessChanged)
        gos_SetTerrainTessParams(s_tessLevel, s_tessNear, s_tessFar);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tess")) {
        s_tessLevel = 4.0f; s_tessNear = 200.0f; s_tessFar = 2000.0f;
        gos_SetTerrainTessParams(4.0f, 200.0f, 2000.0f);
    }

    // ── Phong smoothing ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Phong Smoothing");
    float phong = gos_GetTerrainPhongAlpha();
    if (ImGui::SliderFloat("Alpha##phong", &phong, 0.0f, 1.0f))
        gos_SetTerrainPhongAlpha(phong);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = linear TES interpolation, 1 = full Phong smoothing. Default: 0.5\n"
                          "Higher values round off tessellated terrain vertices more aggressively.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##phong")) gos_SetTerrainPhongAlpha(0.5f);

    // ── Displacement ──────────────────────────────────────────────────────────
    ImGui::SeparatorText("Displacement");
    float disp = gos_GetTerrainDisplaceScale();
    if (ImGui::SliderFloat("Scale##disp", &disp, 0.0f, 8.0f))
        gos_SetTerrainDisplaceScale(disp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Vertex displacement amplitude (dirt material only). Default: 2.0\n"
                          "Acts on the dirt weight channel — non-dirt patches unaffected.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##disp")) gos_SetTerrainDisplaceScale(2.0f);

    // ── Normal / detail maps ──────────────────────────────────────────────────
    ImGui::SeparatorText("Normal Maps");

    float tiling   = gos_GetTerrainDetailTiling();
    static float s_strength = 4.0f;  // no public getter

    if (ImGui::SliderFloat("Tiling##nm", &tiling, 0.1f, 4.0f))
        gos_SetTerrainDetailParams(tiling, s_strength);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Global UV tiling multiplier on top of per-material constants.\n"
                          "Per-material: rock=3×, grass=12×, dirt=1×, concrete=6×. Default: 1.0");

    if (ImGui::SliderFloat("Strength##nm", &s_strength, 0.0f, 12.0f))
        gos_SetTerrainDetailParams(tiling, s_strength);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Global normal-map amplitude multiplier.\n"
                          "Per-material boost applied on top: rock=0.9×, grass/dirt=1.1×, concrete=2.5×. Default: 4.0");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##nm")) {
        s_strength = 4.0f;
        gos_SetTerrainDetailParams(1.0f, 4.0f);
    }

    // ── Per-material normal boost ─────────────────────────────────────────────
    ImGui::SeparatorText("Per-Material Normal Boost");
    ImGui::TextDisabled("Scales the shader's per-material normal strength (on top of global Strength above).");

    static float s_boostRock     = 0.9f;
    static float s_boostGrass    = 1.1f;
    static float s_boostDirt     = 1.1f;
    static float s_boostConcrete = 2.5f;
    static bool  s_boostInited   = false;
    if (!s_boostInited) {
        gos_GetTerrainMatNormalBoost(&s_boostRock, &s_boostGrass, &s_boostDirt, &s_boostConcrete);
        s_boostInited = true;
    }

    bool boostChanged = false;
    boostChanged |= ImGui::SliderFloat("Rock##nb",     &s_boostRock,     0.0f, 5.0f);
    boostChanged |= ImGui::SliderFloat("Grass##nb",    &s_boostGrass,    0.0f, 5.0f);
    boostChanged |= ImGui::SliderFloat("Dirt##nb",     &s_boostDirt,     0.0f, 5.0f);
    boostChanged |= ImGui::SliderFloat("Concrete##nb", &s_boostConcrete, 0.0f, 5.0f);
    if (boostChanged)
        gos_SetTerrainMatNormalBoost(s_boostRock, s_boostGrass, s_boostDirt, s_boostConcrete);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##nb")) {
        s_boostRock = 0.9f; s_boostGrass = 1.1f; s_boostDirt = 1.1f; s_boostConcrete = 2.5f;
        gos_SetTerrainMatNormalBoost(0.9f, 1.1f, 1.1f, 2.5f);
    }

    // ── POM ───────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Parallax Occlusion Mapping");

    static float s_pomScale = 0.02f;
    static float s_pomMin   = 8.0f;
    static float s_pomMax   = 32.0f;

    bool pomChanged = false;
    pomChanged |= ImGui::SliderFloat("Scale##pom",     &s_pomScale, 0.0f, 0.08f, "%.4f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Height-map depth scale. Dirt gets 2.5× internal boost. Default: 0.02");
    pomChanged |= ImGui::SliderFloat("Min layers##pom", &s_pomMin,  1.0f, 32.0f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Minimum ray-march samples at grazing angles. Default: 8");
    pomChanged |= ImGui::SliderFloat("Max layers##pom", &s_pomMax,  8.0f, 64.0f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Maximum ray-march samples looking straight down. Default: 32");
    if (pomChanged)
        gos_SetTerrainPOMParams(s_pomScale, s_pomMin, s_pomMax);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##pom")) {
        s_pomScale = 0.02f; s_pomMin = 8.0f; s_pomMax = 32.0f;
        gos_SetTerrainPOMParams(0.02f, 8.0f, 32.0f);
    }

    // ── Color tint ────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Color / Tinting");

    float tintScale = gos_GetTerrainTintStrengthScale();
    if (ImGui::SliderFloat("Tint strength##cs", &tintScale, 0.0f, 2.0f))
        gos_SetTerrainTintStrengthScale(tintScale);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = pure colormap passthrough (no material tint)\n"
                          "1 = default tint (0.18–0.50 blend by luminance, 0.85 for snow)\n"
                          "2 = double tint intensity");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##cs")) gos_SetTerrainTintStrengthScale(1.0f);

    // ── Shadow softness ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Shadow");

    float softness = gos_GetTerrainShadowSoftness();
    if (ImGui::SliderFloat("Shadow softness##ss", &softness, 0.0f, 1.0f))
        gos_SetTerrainShadowSoftness(softness);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PCF penumbra radius. 0=hard shadows, 1=maximum softness. Default: 0.9");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##ss")) gos_SetTerrainShadowSoftness(0.9f);

    // ── Cell-bomb noise ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Cell-Bomb Noise");
    ImGui::TextDisabled("Hash-based breakup applied on top of normal maps.");

    static float s_cellScale    = 8.0f;
    static float s_cellJitter   = 0.8f;
    static float s_cellRotation = 1.0f;

    bool cellChanged = false;
    cellChanged |= ImGui::SliderFloat("Scale##cb",    &s_cellScale,    1.0f, 20.0f);
    cellChanged |= ImGui::SliderFloat("Jitter##cb",   &s_cellJitter,   0.0f,  1.0f);
    cellChanged |= ImGui::SliderFloat("Rotation##cb", &s_cellRotation, 0.0f,  1.0f);
    if (cellChanged)
        gos_SetTerrainCellBombParams(s_cellScale, s_cellJitter, s_cellRotation);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##cb")) {
        s_cellScale = 8.0f; s_cellJitter = 0.8f; s_cellRotation = 1.0f;
        gos_SetTerrainCellBombParams(8.0f, 0.8f, 1.0f);
    }

    // ── Normals-from-Height + Lighting V1 ────────────────────────────────────
    // Consolidated home for the TERRAIN-NORMALS-FROM-HEIGHT-1 /
    // TERRAIN-RESAMPLE-1 / TERRAIN-LIGHTING-1 stack. Originally lived in the
    // Object Inspector "Terrain Pass" panel; moved here so all terrain
    // tuning is in one window. Env vars MC2_TERRAIN_NORMALS_FROM_HEIGHT and
    // MC2_TERRAIN_LIGHTING_V1 are authoritative on/off gates — sliders
    // tune strength only when the env gate is enabled.
    ImGui::SeparatorText("Normals-from-Height + Lighting V1");

    // Gate status — read env locally; the slider widgets stay enabled even
    // when the gate is off so the user can pre-arm a strength for the next
    // mission load.
    bool nfhGateOn = false;
    {
        const char* env = std::getenv("MC2_TERRAIN_NORMALS_FROM_HEIGHT");
        nfhGateOn = (env && env[0] && env[0] != '0');
        if (nfhGateOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "Normals-from-Height: ON  (MC2_TERRAIN_NORMALS_FROM_HEIGHT=%s)", env);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Normals-from-Height: OFF (set MC2_TERRAIN_NORMALS_FROM_HEIGHT=1)");
        }
    }

    // Height tex source/render readout + live resample combo.
    int srcSide = ::gos_terrainHeightSourceSide();
    int rndSide = ::gos_terrainHeightTexSide();
    int factor  = ::gos_terrainHeightResampleFactor();
    if (srcSide > 0) {
        ImGui::Text("Height tex: source %d^2 -> render %d^2", srcSide, rndSide);
    } else {
        ImGui::TextDisabled("Height tex: not uploaded (no mission loaded)");
    }
    const char* factorLabels[3] = { "1x", "2x", "4x" };
    int curIdx = (factor == 4) ? 2 : (factor == 2) ? 1 : 0;
    int newIdx = curIdx;
    if (ImGui::Combo("Resample##trf", &newIdx, factorLabels, 3)) {
        if (newIdx != curIdx) {
            const int f = (newIdx == 2) ? 4 : (newIdx == 1) ? 2 : 1;
            ::gos_setTerrainHeightResampleFactor(f);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("CPU bilinear resample factor for the height tex.\n"
                          "1x = source grid only; 4x = 4× sub-grid (finer slope normals).\n"
                          "env MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR sets startup default.");

    // Normals-from-Height strength.
    float nfh = ::gos_GetTerrainNormalsFromHeightStrength();
    if (ImGui::SliderFloat("NfH strength##tnfh", &nfh, 0.0f, 1.5f, "%.2f")) {
        ::gos_SetTerrainNormalsFromHeightStrength(nfh);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Multiplier on the additive height-derived normal term.\n"
                          "1.0 = full slope tilt (default). 0.0 = no slope contribution.\n"
                          "Only effective when MC2_TERRAIN_NORMALS_FROM_HEIGHT=1.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tnfh")) ::gos_SetTerrainNormalsFromHeightStrength(1.0f);
    if (!nfhGateOn) {
        ImGui::TextDisabled("(NfH strength only takes effect when env gate enabled)");
    }

    // Lighting V1 (hemisphere ambient) gate status + strength.
    bool litGateOn = false;
    {
        const char* env = std::getenv("MC2_TERRAIN_LIGHTING_V1");
        litGateOn = (env && env[0] && env[0] != '0');
        if (litGateOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "Lighting V1 (hemi ambient): ON");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Lighting V1 (hemi ambient): OFF (set MC2_TERRAIN_LIGHTING_V1=1)");
        }
    }
    float ambient = ::gos_GetTerrainLightingV1Strength();
    if (ImGui::SliderFloat("Ambient strength##tlv1", &ambient, 0.0f, 2.0f, "%.2f")) {
        ::gos_SetTerrainLightingV1Strength(ambient);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hemisphere sky/ground ambient fill on terrain.\n"
                          "Added AFTER shadow multiplication — shadowed terrain still receives\n"
                          "sky bounce, which is intentional. Default 1.0; 0.5 for subtler fill.\n"
                          "Only effective when MC2_TERRAIN_LIGHTING_V1=1.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tlv1")) ::gos_SetTerrainLightingV1Strength(1.0f);
    if (!litGateOn) {
        ImGui::TextDisabled("(slider has no effect until env gate enabled)");
    }

    // TERRAIN-LIGHTING-2: shadow-aware fill floor. Effective only when
    // both V1 and V2 env gates are ON. floor=1.0 → V1 unmodulated;
    // floor=0.3 = default; floor=0.0 → hemi follows shadow exactly.
    bool litV2GateOn = false;
    {
        const char* env = std::getenv("MC2_TERRAIN_LIGHTING_V2");
        litV2GateOn = (env && env[0] && env[0] != '0');
        if (litV2GateOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "Lighting V2 (shadow-aware fill): ON");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Lighting V2 (shadow-aware fill): OFF (set MC2_TERRAIN_LIGHTING_V2=1)");
        }
    }
    float v2Floor = ::gos_GetTerrainLightingV2Floor();
    if (ImGui::SliderFloat("Shadow fill floor##tlv2", &v2Floor, 0.0f, 1.0f, "%.2f")) {
        ::gos_SetTerrainLightingV2Floor(v2Floor);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hemi fill multiplier in fully shadowed terrain.\n"
                          "1.0 = V1 unmodulated (full hemi in shadows — too bright).\n"
                          "0.3 = default — 30%% hemi in shadows, 100%% in lit terrain.\n"
                          "0.0 = hemi follows shadow exactly (lifeless shadows).\n"
                          "Only effective when both MC2_TERRAIN_LIGHTING_V1=1 AND\n"
                          "MC2_TERRAIN_LIGHTING_V2=1 are set.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tlv2")) ::gos_SetTerrainLightingV2Floor(0.3f);
    if (!litV2GateOn) {
        ImGui::TextDisabled("(slider has no effect until MC2_TERRAIN_LIGHTING_V2=1)");
    } else if (!litGateOn) {
        // Reviewer-flagged UX gap: V2 ON but V1 OFF leaves the slider live
        // with no visible effect (V2 only modulates the V1 additive). Make
        // the combined-dependency explicit.
        ImGui::TextDisabled("(V2 floor only acts on the V1 hemi term — set MC2_TERRAIN_LIGHTING_V1=1 too)");
    }
    ImGui::TextDisabled("Debug Mode 10 = height-normal RGB; Mode 11 = hemi additive ×4");
}

// ─────────────────────────────────────────────────────────────────────────────

// Static-prop visual tuning. Previously lived in the Object Inspector PBR
// block (selection-driven); consolidated here so the controls are reachable
// without picking a prop. Env gates remain authoritative — sliders only
// modulate strength when the matching env gate is ON.
static void drawStaticPropTuningSection() {
    // IBL SH ambient (V-IBL-STATIC-1; default-ON env). Slider modulates
    // strength when MC2_STATIC_PROP_IBL_SH != 0 (default-on).
    ImGui::SeparatorText("IBL SH ambient");
    {
        const char* env = std::getenv("MC2_STATIC_PROP_IBL_SH");
        bool iblOn = !(env != nullptr && env[0] == '0');
        const char* setName = ibl_sh_runtime_currentSetName();
        if (iblOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "IBL SH: ON (set=%s, strength=%.2f)",
                setName ? setName : "default", g_iblShStrength);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "IBL SH: OFF (env-gated; MC2_STATIC_PROP_IBL_SH=0)");
        }
        ImGui::BeginDisabled(!iblOn);
        if (ImGui::SliderFloat("Strength##ibl_sh", &g_iblShStrength, 0.0f, 3.0f, "%.2f")) {}
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("V-IBL-STATIC-1 SH-L2 image-based ambient on the\n"
                              "StaticPropOpaque lane. Slider modulates strength\n"
                              "when env gate is on. Default 0.5.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##ibl_sh")) g_iblShStrength = 0.5f;
        ImGui::EndDisabled();
    }

    // PBR V1 specular (V-MATERIAL-PBR-3; default-OFF env). When gate ON,
    // slider controls strength. Default value 0.5 (dialled back from 1.0).
    ImGui::SeparatorText("PBR V1 specular");
    {
        const char* env = std::getenv("MC2_STATIC_PROP_PBR_V1");
        bool pbrOn = (env && env[0] && env[0] != '0');
        if (pbrOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "PBR V1: ON (strength=%.2f)", g_pbrV1Strength);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "PBR V1: OFF (set MC2_STATIC_PROP_PBR_V1=1)");
        }
        ImGui::BeginDisabled(!pbrOn);
        if (ImGui::SliderFloat("Strength##pbr_v1", &g_pbrV1Strength, 0.0f, 3.0f, "%.2f")) {}
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("V-MATERIAL-PBR-3 per-fragment Schlick-Fresnel +\n"
                              "power-lobe specular in static_prop.frag.\n"
                              "Default 0.5 (was 1.0 — full strength was too blunt\n"
                              "on flat-roofed legacy assets without material masks).\n"
                              "Env MC2_STATIC_PROP_PBR_V1_STRENGTH overrides startup.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbr_v1")) g_pbrV1Strength = 0.5f;
        ImGui::EndDisabled();
    }

    // PBR roughness override (V-MATERIAL-PBR-3-TUNE-UI). Forces roughness
    // to a fixed value across all materials when enabled. Default
    // enabled=true / value=0.95 (was disabled/0.6) — the 0.6 literal made
    // legacy assets too glossy.
    ImGui::SeparatorText("PBR roughness override");
    {
        const char* env = std::getenv("MC2_STATIC_PROP_PBR_V1");
        bool pbrOn = (env && env[0] && env[0] != '0');
        if (pbrOn) {
            ImGui::TextColored(g_pbrV1RoughnessOverrideEnabled
                                   ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                   : ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Override: %s (value=%.2f)",
                g_pbrV1RoughnessOverrideEnabled ? "ON" : "OFF",
                g_pbrV1RoughnessOverrideValue);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Override: inert (PBR V1 env gate OFF)");
        }
        ImGui::BeginDisabled(!pbrOn);
        ImGui::Checkbox("Enable##pbr_rough", &g_pbrV1RoughnessOverrideEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When enabled, overrides static_prop.vert's roughness\n"
                              "literal/MaterialGpu value with the slider value.\n"
                              "Default-ON at 0.95 (was OFF / 0.6 literal — too glossy).");
        ImGui::SameLine();
        ImGui::BeginDisabled(!g_pbrV1RoughnessOverrideEnabled);
        if (ImGui::SliderFloat("Value##pbr_rough", &g_pbrV1RoughnessOverrideValue,
                               0.05f, 1.0f, "%.2f")) {}
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbr_rough")) {
            g_pbrV1RoughnessOverrideEnabled = true;
            g_pbrV1RoughnessOverrideValue   = 0.95f;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawDrawPacketsSection() {
    // ── Frame health ──────────────────────────────────────────────────────────
    const bool anyError = (g_dpSnapshot.emitted != g_dpSnapshot.expected
                           || g_dpSnapshot.invalidRanges > 0
                           || g_dpSnapshot.overflow);
    if (anyError && g_dpSnapshot.frame > 0)
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "! packet integrity error");

    char buf[64];

    // Top table: frame + emitted vs expected
    if (ImGui::BeginTable("##dphealth", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##dk",  ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("##dv",  ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Frame");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)g_dpSnapshot.frame);
        ImGui::TextUnformatted(buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Emitted");
        ImGui::TableSetColumnIndex(1);
        {
            const bool ok = (g_dpSnapshot.emitted == g_dpSnapshot.expected);
            snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.emitted);
            ImGui::TextColored(ok ? ImVec4(0.3f,1.f,0.3f,1.f)
                                  : ImVec4(1.f,0.3f,0.3f,1.f), "%s", buf);
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Expected");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.expected);
        ImGui::TextUnformatted(buf);

        ImGui::EndTable();
    }

    ImGui::Separator();

    // Bottom table: diagnostic counters
    if (ImGui::BeginTable("##dpdiag", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##ddk", ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("##ddv", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Distinct types");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.distinctTypes);
        ImGui::TextUnformatted(buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Invalid ranges");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.invalidRanges > 0) {
            snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.invalidRanges);
            ImGui::TextColored(ImVec4(1.f,0.3f,0.3f,1.f), "%s", buf);
        } else {
            ImGui::TextUnformatted("0");
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Overflow");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.overflow)
            ImGui::TextColored(ImVec4(1.f,0.3f,0.3f,1.f), "YES");
        else
            ImGui::TextDisabled("no");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Skipped ranges");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.skippedRanges);
        ImGui::TextDisabled("%s", buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Mat mismatches");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.materialMismatches > 0) {
            snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.materialMismatches);
            ImGui::TextColored(ImVec4(1.f,0.8f,0.3f,1.f), "%s", buf);
        } else {
            ImGui::TextDisabled("0");
        }

        ImGui::EndTable();
    }

    // ── Type Inspector ────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Type Inspector");

    const uint32_t typeCount = g_dpSnapshot.typeDescCount;
    if (typeCount == 0) {
        ImGui::TextDisabled("(geometry not finalized — load a mission)");
        return;
    }

    static int s_selTypeId = 0;
    if (s_selTypeId >= (int)typeCount) s_selTypeId = 0;

    ImGui::SetNextItemWidth(110.f);
    ImGui::InputInt("TypeId##dp", &s_selTypeId, 1, 10);
    if (s_selTypeId < 0)             s_selTypeId = 0;
    if (s_selTypeId >= (int)typeCount) s_selTypeId = (int)typeCount - 1;
    ImGui::SameLine();
    ImGui::TextDisabled("/ %u", typeCount - 1u);

    RenderCore::StaticPropTypeDesc desc{};
    const bool descOk = batcher_getStaticPropTypeDesc((uint32_t)s_selTypeId, &desc);

    if (descOk) {
        if (ImGui::BeginTable("##dptype", 2,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("##dtk", ImGuiTableColumnFlags_WidthFixed, 110.f);
            ImGui::TableSetupColumn("##dtv", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("firstPacket");
            ImGui::TableSetColumnIndex(1);
            snprintf(buf, sizeof(buf), "%u", desc.firstPacket);
            ImGui::TextUnformatted(buf);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("packetCount");
            ImGui::TableSetColumnIndex(1);
            snprintf(buf, sizeof(buf), "%u", desc.packetCount);
            ImGui::TextUnformatted(buf);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("alphaClass");
            ImGui::TableSetColumnIndex(1);
            switch (desc.alphaClass) {
                case 0:  ImGui::TextUnformatted("OPAQUE");     break;
                case 1:  ImGui::TextUnformatted("ALPHA_TEST"); break;
                default:
                    snprintf(buf, sizeof(buf), "?(%u)", desc.alphaClass);
                    ImGui::TextColored(ImVec4(1.f,0.8f,0.3f,1.f), "%s", buf);
                    break;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("materialIdx");
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("per-instance");

            ImGui::EndTable();
        }

        if (ImGui::SmallButton("Copy CSV##dp")) {
            const char* alphaStr = desc.alphaClass == 0 ? "OPAQUE"
                                 : desc.alphaClass == 1 ? "ALPHA_TEST" : "?";
            snprintf(buf, sizeof(buf),
                "typeId,firstPacket,packetCount,alphaClass\n%u,%u,%u,%s",
                (uint32_t)s_selTypeId, desc.firstPacket, desc.packetCount, alphaStr);
            ImGui::SetClipboardText(buf);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy type descriptor row as CSV.");
    } else {
        ImGui::TextDisabled("(typeId %d out of range)", s_selTypeId);
    }

    // ── Packet range minimap ──────────────────────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Packet Range Minimap##dp")) {
        const uint32_t total  = g_dpSnapshot.expected > 0u ? g_dpSnapshot.expected : 1u;
        const float    availW = ImGui::GetContentRegionAvail().x;
        const float    barH   = 14.f;
        const ImVec2   p0     = ImGui::GetCursorScreenPos();
        ImDrawList*    dl     = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(p0, ImVec2(p0.x + availW, p0.y + barH),
                          IM_COL32(45, 45, 45, 255));

        // Selected type range in yellow
        if (descOk && desc.packetCount > 0u) {
            const float x0 = p0.x + availW * ((float)desc.firstPacket / (float)total);
            float       x1 = p0.x + availW * ((float)(desc.firstPacket + desc.packetCount) / (float)total);
            if (x1 < x0 + 2.f) x1 = x0 + 2.f;
            dl->AddRectFilled(ImVec2(x0, p0.y + 2.f), ImVec2(x1, p0.y + barH - 2.f),
                              IM_COL32(255, 220, 50, 200));
        }

        // Border
        dl->AddRect(p0, ImVec2(p0.x + availW, p0.y + barH),
                    IM_COL32(110, 110, 110, 255));
        ImGui::Dummy(ImVec2(availW, barH));

        snprintf(buf, sizeof(buf), "type %d: [%u, %u) of %u total",
            s_selTypeId,
            descOk ? desc.firstPacket : 0u,
            descOk ? (desc.firstPacket + desc.packetCount) : 0u,
            total);
        ImGui::TextDisabled("%s", buf);
    }

    // ── Selected Prop Packet Inspector ────────────────────────────────────────
    // Populated each frame by gameosmain after emitStaticPropDrawPackets.
    // Selection: Ctrl+Shift+Click a static prop in-game (Object Inspector).
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Selected Prop##dp", ImGuiTreeNodeFlags_DefaultOpen)) {
        const DrawPacketSelectedPropSnapshot& sel = g_dpSelProp;

        if (!sel.valid) {
            ImGui::TextDisabled("(no static prop selected)");
            ImGui::TextDisabled("Ctrl+Shift+Click a prop in-game to inspect.");
        } else {
            // Header line: shape | typeId | recipe
            const char* sname = sel.shapeName[0] ? sel.shapeName : "(unnamed)";
            ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.f),
                "%s", sname);
            ImGui::SameLine();
            ImGui::TextDisabled("| typeId: %u | recipe: %d",
                sel.typeId, sel.recipeIndex);

            // Summary row: instances + materialIdx + alphaClass
            if (ImGui::BeginTable("##selpropsummary", 2,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("##spsk", ImGuiTableColumnFlags_WidthFixed, 130.f);
                ImGui::TableSetupColumn("##spsv", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Instances (vis)");
                ImGui::TableSetColumnIndex(1);
                snprintf(buf, sizeof(buf), "%u", sel.instanceCount);
                ImGui::TextUnformatted(buf);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("materialIdx");
                ImGui::TableSetColumnIndex(1);
                if (sel.materialIdx == 0xFFFFFFFFu)
                    ImGui::TextDisabled("(sentinel)");
                else {
                    snprintf(buf, sizeof(buf), "%u", sel.materialIdx);
                    ImGui::TextUnformatted(buf);
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("alphaClass");
                ImGui::TableSetColumnIndex(1);
                switch (sel.alphaClass) {
                    case 0:  ImGui::TextUnformatted("OPAQUE");      break;
                    case 1:  ImGui::TextUnformatted("ALPHA_TEST");  break;
                    default:
                        snprintf(buf, sizeof(buf), "?(%u)", sel.alphaClass);
                        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", buf);
                        break;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("packetCount");
                ImGui::TableSetColumnIndex(1);
                snprintf(buf, sizeof(buf), "%u", sel.packetCount);
                ImGui::TextUnformatted(buf);

                ImGui::EndTable();
            }

            // Per-packet table
            if (sel.rowCount > 0) {
                ImGui::Spacing();
                ImGui::TextUnformatted("Packets:");
                if (ImGui::BeginTable("##selproppkts", 7,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg
                        | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("pktIdx",    ImGuiTableColumnFlags_WidthFixed, 48.f);
                    ImGui::TableSetupColumn("firstIdx",  ImGuiTableColumnFlags_WidthFixed, 64.f);
                    ImGui::TableSetupColumn("idxCount",  ImGuiTableColumnFlags_WidthFixed, 64.f);
                    ImGui::TableSetupColumn("baseVtx",   ImGuiTableColumnFlags_WidthFixed, 56.f);
                    ImGui::TableSetupColumn("pipeline",  ImGuiTableColumnFlags_WidthFixed, 74.f);
                    ImGui::TableSetupColumn("matFlags",  ImGuiTableColumnFlags_WidthFixed, 62.f);
                    ImGui::TableSetupColumn("baseInst",  ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (uint32_t r = 0; r < sel.rowCount; ++r) {
                        const DrawPacketPropRow& row = sel.rows[r];
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        snprintf(buf, sizeof(buf), "%u", row.globalPacketIdx);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(1);
                        snprintf(buf, sizeof(buf), "%u", row.firstIndex);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(2);
                        snprintf(buf, sizeof(buf), "%u", row.indexCount);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(3);
                        snprintf(buf, sizeof(buf), "%d", row.baseVertex);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(4);
                        switch (row.pipelineId) {
                            case 0:  ImGui::TextDisabled("invalid");      break;
                            case 1:  ImGui::TextUnformatted("opaque");    break;
                            case 2:  ImGui::TextUnformatted("alpha_test"); break;
                            default:
                                snprintf(buf, sizeof(buf), "?(%u)", row.pipelineId);
                                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", buf);
                                break;
                        }

                        ImGui::TableSetColumnIndex(5);
                        snprintf(buf, sizeof(buf), "0x%02X", row.materialFlags);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(6);
                        ImGui::TextDisabled("--");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("baseInstance populated at dispatch (v1+)");
                    }

                    ImGui::EndTable();
                }
            }

            if (ImGui::SmallButton("Copy CSV##selp")) {
                // Header
                int n = snprintf(buf, sizeof(buf),
                    "pktIdx,firstIndex,idxCount,baseVertex,pipeline,matFlags\n");
                ImGui::SetClipboardText(buf);
                // Rows: build into a larger buffer
                char cbuf[1024]; int cn = 0;
                cn += snprintf(cbuf+cn, sizeof(cbuf)-cn,
                    "pktIdx,firstIndex,idxCount,baseVertex,pipeline,matFlags\n");
                for (uint32_t r = 0; r < sel.rowCount && cn < (int)sizeof(cbuf)-80; ++r) {
                    const DrawPacketPropRow& row = sel.rows[r];
                    const char* plName = row.pipelineId == 1 ? "opaque"
                                       : row.pipelineId == 2 ? "alpha_test" : "invalid";
                    cn += snprintf(cbuf+cn, sizeof(cbuf)-cn,
                        "%u,%u,%u,%d,%s,0x%02X\n",
                        row.globalPacketIdx, row.firstIndex, row.indexCount,
                        row.baseVertex, plName, row.materialFlags);
                }
                ImGui::SetClipboardText(cbuf);
                (void)n;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawRenderPassesSection() {
    gosPostProcess* pp = getGosPostProcess();

    // ── Shadow pass ───────────────────────────────────────────────────────────
    if (pp) {
        ImGui::Checkbox("Shadow Pass##rp", &pp->shadowsEnabled_);
        ImGui::SameLine();
        ImGui::TextDisabled("(also in Post-Process)");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("GPU Geometry Passes");

    // Draw Mechs: hides mechs entirely (gates both GPU submit and CPU fallback).
    ImGui::Checkbox("Draw Mechs##rp", &g_drawMechs);
    ImGui::SameLine();
    // GPU Mechs: switches between GPU batcher and CPU legacy path (mechs still visible).
    ImGui::Checkbox("GPU path##rp", &g_useGpuMechs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("GPU Mechs ON = GPU batcher (PBR lit).\n"
                          "GPU Mechs OFF = CPU legacy path (older shading, dimmer).\n"
                          "Use 'Draw Mechs' to hide mechs entirely.");

    // GPU objects (full GPU-driven batcher)
    if (g_useGpuObjects) {
        ImGui::Checkbox("GPU Objects##rp",  &g_useGpuObjects);
    } else {
        ImGui::Checkbox("GPU Objects##rp",  &g_useGpuObjects);
        ImGui::SameLine();
        // Legacy static props only active when GPU Objects batcher is off
        ImGui::Checkbox("GPU Static Props##rp", &g_useGpuStaticProps);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("FX / Particles");

    bool fxDisabled = (getenv("MC2_DISABLE_GOSFX") != nullptr);
    if (fxDisabled) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "[off]  FX/Particles");
        ImGui::SameLine();
        ImGui::TextDisabled("MC2_DISABLE_GOSFX set — requires restart to re-enable");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "[ON]   FX/Particles");
        ImGui::SameLine();
        ImGui::TextDisabled("(set MC2_DISABLE_GOSFX=1 to disable)");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Static Props Debug Mode");

    static const char* kPropModes[] = {
        "0: Normal",
        "1: Addr-gradient",
        "2: Addr-hash",
        "3: WHITE",
        "4: ARGB only",
        "5: TEX only",
        "6: HIGHLIGHT only",
        "7: TEX+HIGHLIGHT",
    };
    int target = gos_GpuPropsGetDebugMode();
    if (ImGui::Combo("##propdbg", &target, kPropModes, IM_ARRAYSIZE(kPropModes))) {
        int cur   = gos_GpuPropsGetDebugMode();
        int steps = (target - cur + 8) % 8;
        for (int i = 0; i < steps; ++i)
            gos_GpuPropsCycleDebugMode();
    }
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawGBufferPreview() {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) {
        ImGui::TextDisabled("(post-process system unavailable)");
        return;
    }

    // Compute thumbnail width from available content region.
    // Aspect ratio: fixed 16:9 approximation.
    const float availW = ImGui::GetContentRegionAvail().x;
    const float thumbW = (availW * 0.5f) - ImGui::GetStyle().ItemSpacing.x;
    const float thumbH = thumbW * (9.0f / 16.0f);

    // UV flip: OpenGL textures have (0,0) at bottom-left; ImGui expects top-left.
    const ImVec2 uv0(0.0f, 1.0f);
    const ImVec2 uv1(1.0f, 0.0f);
    const ImVec2 sz(thumbW, thumbH);

    // ── Scene Color (RGBA16F HDR) ─────────────────────────────────────────────
    {
        unsigned int tex = pp->getSceneColorTexture();
        if (tex) {
            ImGui::TextUnformatted("Scene Color  (RGBA16F, HDR)");
            ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("HDR scene output before tone-mapping.\nValues above 1.0 are bloom sources.");
        } else {
            ImGui::TextDisabled("Scene Color: not allocated");
        }
    }

    ImGui::SameLine();

    // ── Scene Normals (RGBA16F world-space + shadow-skip) ─────────────────────
    {
        unsigned int tex = pp->getSceneNormalTexture();
        if (tex) {
            ImGui::TextUnformatted("Scene Normals  (RGBA16F)");
            // Column-layout: label is already on the SameLine'd column.
            // SetCursorPosY to align label with opposite column.
            ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("World-space normals (RGB = XYZ mapped 0-1).\n"
                                  "Alpha = shadow-skip flag (0=skip, 1=apply).\n"
                                  "Clear sentinel: (0.5, 0.5, 1.0, 0.0) = flat-up.");
        } else {
            ImGui::TextDisabled("Scene Normals: not allocated");
        }
    }

    // ── Shadow maps ───────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextUnformatted("Shadow Maps  (DEPTH_COMPONENT24)");
    ImGui::TextDisabled("  Depth textures need GL_TEXTURE_COMPARE_MODE=GL_NONE to preview.");
    ImGui::TextDisabled("  Use the shadow debug overlay in Post-Process instead.");

    // ── Object-ID buffer ──────────────────────────────────────────────────────
    {
        unsigned int tex = pp->getSceneObjectIdTex();
        if (tex) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Object-ID Buffer  (R32UI)");
            ImGui::TextDisabled("  Uint texture — needs a hash-to-RGB visualization shader.");
            ImGui::TextDisabled("  Use Ctrl+Shift+I + click to inspect individual pixels.");
        } else {
            ImGui::Spacing();
            ImGui::TextDisabled("Object-ID buffer: not active (set MC2_OBJECT_ID_BUFFER=1)");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawEnvGatesSection() {
    ImGui::TextDisabled("Baked at startup — changes require restart.");
    ImGui::Spacing();

    if (!ImGui::BeginTable("##envtbl", 3,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("##st",   ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("##var",  ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableSetupColumn("##desc", ImGuiTableColumnFlags_WidthStretch);

    for (int i = 0; i < kEnvGateCount; ++i) {
        const EnvGate& g = kEnvGates[i];
        const char* val  = getenv(g.var);

        bool enabled;
        if (g.inverted) {
            // Present → feature disabled.  Absent → feature enabled.
            enabled = (val == nullptr);
        } else if (g.defaultOn) {
            enabled = (!val || val[0] != '0');
        } else {
            enabled = (val && val[0] != '0');
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (enabled)
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "[ON] ");
        else
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[off]");

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(g.var);

        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("%s", g.desc);
    }

    ImGui::EndTable();
}

// ─────────────────────────────────────────────────────────────────────────────

void draw() {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G))
        s_open = !s_open;

    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(500, 700), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics Options  [Ctrl+Shift+G]", &s_open)) {
        ImGui::End();
        return;
    }

    // ── Post-Process ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Post-Process", ImGuiTreeNodeFlags_DefaultOpen)) {
        gosPostProcess* pp = getGosPostProcess();
        if (pp) {
            ImGui::Checkbox("Bloom", &pp->bloomEnabled_);
            if (pp->bloomEnabled_) {
                ImGui::Indent();
                ImGui::SliderFloat("Intensity##bloom", &pp->bloomIntensity_, 0.0f, 4.0f);
                ImGui::SliderFloat("Threshold##bloom", &pp->bloomThreshold_, 0.0f, 2.0f);
                ImGui::Unindent();
            }
            ImGui::Checkbox("FXAA",    &pp->fxaaEnabled_);
            ImGui::Checkbox("Tonemap", &pp->tonemapEnabled_);
            ImGui::Separator();
            ImGui::Checkbox("Shadows##pp",      &pp->shadowsEnabled_);
            ImGui::Checkbox("Shadow Debug##pp", &pp->showShadowDebug_);
            if (pp->showShadowDebug_) {
                ImGui::Indent();
                ImGui::RadioButton("Static##sd",  &pp->shadowDebugMode_, 0); ImGui::SameLine();
                ImGui::RadioButton("Dynamic##sd", &pp->shadowDebugMode_, 1);
                if (ImGui::TreeNodeEx("Registries##sd", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextDisabled("Views:");
                    uint32_t nv = RenderCore::getViewCount();
                    bool foundShadow = false;
                    for (uint32_t i = 0; i < nv; ++i) {
                        const RenderCore::EngineView* v = RenderCore::getViewByIndex(i);
                        if (!v) continue;
                        if (v->kind == RenderCore::ViewKind::ShadowStatic ||
                            v->kind == RenderCore::ViewKind::ShadowDynamic) {
                            ImGui::Text("  id=%u  %s  %dx%d",
                                v->id, v->debugName ? v->debugName : "?",
                                v->viewport[2], v->viewport[3]);
                            foundShadow = true;
                        }
                    }
                    if (!foundShadow)
                        ImGui::TextDisabled("  (none — load a mission first)");
                    ImGui::TextDisabled("Resources:");
                    for (auto rid : { RenderCore::RenderResourceId::ShadowStaticMap,
                                      RenderCore::RenderResourceId::ShadowDynamicMap }) {
                        const RenderCore::RenderResourceDesc* r =
                            RenderCore::getRenderResource(rid);
                        if (r)
                            ImGui::Text("  %s  %ux%u  %s",
                                r->debugName ? r->debugName : "?",
                                r->width, r->height,
                                RenderCore::toString(r->format));
                        else
                            ImGui::TextDisabled("  %s  (not registered)",
                                RenderCore::toString(rid));
                    }
                    ImGui::TreePop();
                }
                ImGui::Unindent();
            }
            // Shadow tuning — bias and softness. Always visible in shadow section.
            ImGui::SeparatorText("Shadow Tuning");
            ImGui::TextDisabled("Polygon offset (static shadow pass):");
            ImGui::SliderFloat("Factor##bias", &pp->shadowBiasFactor_, 0.0f, 8.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("lower = shadow acne risk\nhigher = peter-panning risk\ndefault 2.0");
            ImGui::SliderFloat("Units##bias",  &pp->shadowBiasUnits_,  0.0f, 20.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("offset units\ndefault 4.0");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##bias"))
                { pp->shadowBiasFactor_ = 2.0f; pp->shadowBiasUnits_ = 4.0f; }
            {
                float softness = gos_GetTerrainShadowSoftness();
                if (ImGui::SliderFloat("Softness##sdtune", &softness, 0.5f, 8.0f))
                    gos_SetTerrainShadowSoftness(softness);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("higher = blurrier shadows\ndefault 0.9 (also in Terrain Tuning)");
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##sdtune")) gos_SetTerrainShadowSoftness(0.9f);
            }
            ImGui::Separator();
            ImGui::Checkbox("Screen Shadows##pp", &pp->screenShadowEnabled_);
            if (pp->screenShadowEnabled_) {
                ImGui::Indent();
                ImGui::RadioButton("Normal##scr",    &pp->screenShadowDebug_, 0); ImGui::SameLine();
                ImGui::RadioButton("Visualize##scr", &pp->screenShadowDebug_, 1);
                ImGui::Unindent();
            }
            ImGui::Separator();
            ImGui::Checkbox("God Rays",   &pp->godrayEnabled_);
            ImGui::Checkbox("Shorelines", &pp->shorelineEnabled_);
        } else {
            ImGui::TextDisabled("(post-process system unavailable)");
        }
    }

    // ── Terrain ───────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen))
        drawTerrainSection();

    // ── Draw Packets ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Draw Packets", ImGuiTreeNodeFlags_DefaultOpen))
        drawDrawPacketsSection();

    // ── Render Passes ─────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Render Passes", ImGuiTreeNodeFlags_DefaultOpen))
        drawRenderPassesSection();

    // ── Terrain Tuning ────────────────────────────────────────────────────────
    // Heavy section (15 sliders) — collapsed by default so it doesn't push other
    // sections off-screen on first open.
    if (ImGui::CollapsingHeader("Terrain Tuning"))
        drawTerrainTuningSection();

    // ── Static Prop Tuning ────────────────────────────────────────────────────
    // IBL SH + PBR V1 strength + roughness override. Previously lived in
    // the Object Inspector PBR selection block; consolidated here so the
    // controls are reachable without picking a prop.
    if (ImGui::CollapsingHeader("Static Prop Tuning"))
        drawStaticPropTuningSection();

    // ── HUD ───────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("HUD")) {
        float scale = gos_GetHudScale();
        if (ImGui::SliderFloat("Scale##hud", &scale, 0.5f, 1.5f, "%.2f"))
            gos_SetHudScale(scale);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##hud")) gos_SetHudScale(1.0f);
    }

    // ── GBuffer Preview ───────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("GBuffer Preview"))
        drawGBufferPreview();

    // ── Env Gates ─────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Env Gates"))
        drawEnvGatesSection();

    // ── Debug Overlays ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Debug Overlays")) {
        if (ImGui::Button("Advance ProjectZ Overlay"))
            projectz_overlay_advance();
        ImGui::SameLine();
        ImGui::TextDisabled("(cycles predicates)");
    }

    ImGui::Separator();
    ImGui::TextDisabled("RAlt hotkeys still active.  Ctrl+Shift+G to close.");

    ImGui::End();
}

} // namespace GraphicsOptionsWindow
