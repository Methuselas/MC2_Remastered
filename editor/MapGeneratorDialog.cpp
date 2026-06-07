/***************************************************************
* FILENAME: MapGeneratorDialog.cpp
* DESCRIPTION: ImGui Map Generator dialog for the MC2 Mission Editor.
*   Provides a single integrated window replacing the old MFC TerrainDlg +
*   MapSizeDlg flow.  Supports biome, size, height, material-mix controls,
*   a fast thumbnail preview, and deferred full generation.
*
* Biome labels map to terrain_gen.py recipe biome strings:
*   "Temperate Forest"  -> "swamp_forest"
*   "Desert"            -> "desert"
*   "Tundra / Snow"     -> "snow_mountain"
*   "Rocky Badlands"    -> "rocky_badlands"
*   "Volcanic"          -> "rocky_badlands"  (no dedicated volcanic biome yet)
*
* Map sizes are multiples of 20 (verticesBlockSide) per existing generateMission()
* convention: 60,80,100,120,260,520,1020.
*
* Recipe JSON is written to terrain_gen_out/genmap_recipe.json.
* Preview PNG is loaded from terrain_gen_out/genmap.preview.png into a GL
* texture and displayed via ImGui::Image.
*
* DATE: 2026-06-05
****************************************************************/

// Pull stdafx first (MFC / Windows headers)
#include "stdafx.h"

#ifdef MC2_IMGUI

// ImGui
#include "imgui.h"

// GL for preview texture
#include <GL/glew.h>

// stb_image — single-header PNG loader for the editor preview texture.
//
// burnin_jpeg_decode.cpp (mclib) owns STB_IMAGE_IMPLEMENTATION (JPEG-only,
// STBI_NO_STDIO) in a different archive.  We define our own isolated copy
// here using STB_IMAGE_STATIC so all stbi_* symbols become file-static,
// eliminating any ODR clash at the link step when the editor links mclib.
// STBI_ONLY_PNG: we only load the preview thumbnail (PNG), keeping the
// implementation small and avoiding JPEG codec duplication with mclib.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#pragma warning(push)
#pragma warning(disable: 4244 4996)
#include "../3rdparty/stb/stb_image.h"
#pragma warning(pop)

#include "MapGeneratorDialog.h"
#include "EditorData.h"    // EditorData::generateFromDialogParams declaration

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const char* k_biomeLabels[5] = {
    "Temperate Forest",
    "Desert",
    "Tundra / Snow",
    "Rocky Badlands",
    "Volcanic",
};

// terrain_gen.py recipe biome strings
static const char* k_biomeKeys[5] = {
    "swamp_forest",
    "desert",
    "snow_mountain",
    "rocky_badlands",
    "rocky_badlands",
};

// MapSize index -> vertex side length (matches genMapSizeToN in EditorData.cpp)
static const int   k_mapSizeVertices[7] = { 60, 80, 100, 120, 260, 520, 1020 };
static const char* k_mapSizeLabels[7]   = {
    "60x60",
    "80x80",
    "100x100",
    "120x120 (large)",
    "260x260 (huge)",
    "520x520 (xl)",
    "1020x1020 (xxl)"
};
static const int k_numMapSizes = 7;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace {

struct DialogState {
    bool open            = false;

    int  biomeIndex      = 0;    // Temperate Forest default
    int  mapSizeIndex    = 3;    // 120x120 default

    float maxElevation   = 600.f;
    float minElevation   = 0.f;
    float mountainAmount = 0.5f; // 0..1 slider; mapped to recipe [0.1..0.9]
    float ridgedAmount   = 0.3f; // 0..1 slider
    float grassLowland   = 0.5f;
    float snowLine       = 0.7f;

    unsigned long seed   = 0;    // 0 = randomise each run
    bool  randomSeed     = true;

    // Preview texture (GL)
    GLuint previewTexID  = 0;
    int    previewW      = 0;
    int    previewH      = 0;

    // Pending deferred action (consumed once by update())
    MapGeneratorDialog::PendingAction pendingAction = MapGeneratorDialog::PendingAction::None;

    // Status string shown below buttons
    char statusMsg[256]  = "";
};

static DialogState s_state;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Map mountain_amount slider [0..1] to recipe value [0.1..0.9]
static float SliderToMountainAmount(float t) {
    return 0.1f + t * 0.8f;
}

// Same for ridged_amount
static float SliderToRidgedAmount(float t) {
    return 0.1f + t * 0.8f;
}

// Build the recipe JSON string for the current dialog params.
static std::string BuildRecipeJSON(bool preview) {
    unsigned long seed = s_state.seed;
    if (s_state.randomSeed || seed == 0) {
        seed = (unsigned long)time(nullptr) ^ (unsigned long)(uintptr_t)&s_state;
        // Store so Preview + Generate share the same seed in one session.
        if (!preview || s_state.seed == 0)
            s_state.seed = seed;
    }

    int sideN    = k_mapSizeVertices[s_state.mapSizeIndex];
    float mnt    = SliderToMountainAmount(s_state.mountainAmount);
    float ridged = SliderToRidgedAmount(s_state.ridgedAmount);

    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"genmap\",\n"
        "  \"size\": %d,\n"
        "  \"biome\": \"%s\",\n"
        "  \"seed\": %lu,\n"
        "  \"height\": {\n"
        "    \"max_elevation\": %.1f,\n"
        "    \"min_elevation\": %.1f,\n"
        "    \"mountain_amount\": %.3f,\n"
        "    \"ridged_amount\": %.3f\n"
        "  },\n"
        "  \"materials\": {\n"
        "    \"grass_lowland\": %.3f,\n"
        "    \"snow_line\": %.3f\n"
        "  }\n"
        "}\n",
        sideN,
        k_biomeKeys[s_state.biomeIndex],
        seed,
        (double)s_state.maxElevation,
        (double)s_state.minElevation,
        (double)mnt,
        (double)ridged,
        (double)s_state.grassLowland,
        (double)s_state.snowLine
    );
    return std::string(buf);
}

// Write recipe JSON to disk.  Returns false on error.
static bool WriteRecipeFile(const std::string& json) {
    const char* dir  = "terrain_gen_out";
    const char* path = "terrain_gen_out\\genmap_recipe.json";
    CreateDirectoryA(dir, NULL);
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fputs(json.c_str(), f);
    fclose(f);
    return true;
}

// Run the terrain generator (blocking).
// preview=true adds --preview flag (fast, ~2-3s).
// Returns true if the process exited 0.
static bool RunTerrainGen(bool preview) {
    const char* recipeArg = "terrain_gen_out\\genmap_recipe.json";
    const char* outArg    = "terrain_gen_out";
    char cmd[1024];
    if (preview) {
        snprintf(cmd, sizeof(cmd),
            "py -3 tools\\terrain_gen\\terrain_gen.py \"%s\" --out \"%s\" --preview",
            recipeArg, outArg);
    } else {
        snprintf(cmd, sizeof(cmd),
            "py -3 tools\\terrain_gen\\terrain_gen.py \"%s\" --out \"%s\"",
            recipeArg, outArg);
    }
    return (system(cmd) == 0);
}

// Free the existing preview texture (if any).
static void FreePreviewTexture() {
    if (s_state.previewTexID) {
        glDeleteTextures(1, &s_state.previewTexID);
        s_state.previewTexID = 0;
        s_state.previewW = 0;
        s_state.previewH = 0;
    }
}

// Load terrain_gen_out/genmap.preview.png into a GL texture.
static void LoadPreviewPNG() {
    FreePreviewTexture();
    const char* pngPath = "terrain_gen_out\\genmap.preview.png";
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(pngPath, &w, &h, &channels, 4 /*RGBA*/);
    if (!data) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Preview: failed to load %s", pngPath);
        return;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
    s_state.previewTexID = tex;
    s_state.previewW     = w;
    s_state.previewH     = h;
    snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
        "Preview ready (%dx%d)", w, h);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void MapGeneratorDialog::Open() {
    s_state.open = true;
    // Preserve params so the user keeps their last settings when reopening.
}

void MapGeneratorDialog::Close() {
    s_state.open = false;
    FreePreviewTexture();
    s_state.statusMsg[0] = '\0';
}

bool MapGeneratorDialog::IsOpen() {
    return s_state.open;
}

MapGeneratorDialog::PendingAction MapGeneratorDialog::TakeAction() {
    PendingAction a = s_state.pendingAction;
    s_state.pendingAction = PendingAction::None;
    return a;
}

// Returns Windows DPI scale relative to 96 DPI baseline (1.0 = 100%, 2.0 = 200%).
// Cached after first call; uses GetDpiForSystem (Win8.1+) with fallback.
static float GetDpiScale() {
    static float s_scale = 0.0f;
    if (s_scale == 0.0f) {
#if defined(_WIN32)
        // GetDpiForSystem available since Win8.1 (always present on Win10+).
        typedef UINT (WINAPI *PFN_GetDpiForSystem)();
        HMODULE user32 = GetModuleHandleA("user32.dll");
        PFN_GetDpiForSystem fn = user32
            ? (PFN_GetDpiForSystem)GetProcAddress(user32, "GetDpiForSystem")
            : nullptr;
        UINT dpi = fn ? fn() : 96;
        s_scale = (float)dpi / 96.0f;
#else
        s_scale = 1.0f;
#endif
        if (s_scale < 1.0f) s_scale = 1.0f;  // never shrink below baseline
    }
    return s_scale;
}

void MapGeneratorDialog::Draw() {
    if (!s_state.open) return;

    const float sc = GetDpiScale();

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540.f * sc, 0), ImGuiCond_Appearing);

    bool windowOpen = true;
    if (!ImGui::Begin("Map Generator", &windowOpen,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!windowOpen) Close();
        return;
    }
    if (!windowOpen) {
        ImGui::End();
        Close();
        return;
    }
    ImGui::SetWindowFontScale(sc);  // scale text within this window

    // --- Biome ---
    ImGui::Text("Biome");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.f * sc);
    if (ImGui::BeginCombo("##biome", k_biomeLabels[s_state.biomeIndex])) {
        for (int i = 0; i < 5; ++i) {
            bool sel = (s_state.biomeIndex == i);
            if (ImGui::Selectable(k_biomeLabels[i], sel))
                s_state.biomeIndex = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // --- Map Size ---
    ImGui::Text("Size  ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.f * sc);
    if (ImGui::BeginCombo("##size", k_mapSizeLabels[s_state.mapSizeIndex])) {
        for (int i = 0; i < k_numMapSizes; ++i) {
            bool sel = (s_state.mapSizeIndex == i);
            if (ImGui::Selectable(k_mapSizeLabels[i], sel))
                s_state.mapSizeIndex = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    ImGui::Text("Height");
    ImGui::Indent();

    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Min Elevation",  &s_state.minElevation, 0.f, 500.f, "%.0f");
    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Max Elevation",  &s_state.maxElevation, 0.f, 500.f, "%.0f");
    // Keep min <= max
    if (s_state.minElevation > s_state.maxElevation)
        s_state.minElevation = s_state.maxElevation;

    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Flat <-> Mountainous", &s_state.mountainAmount, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Ridged Noise",   &s_state.ridgedAmount,   0.f, 1.f, "%.2f");
    ImGui::Unindent();

    ImGui::Separator();
    ImGui::Text("Materials");
    ImGui::Indent();
    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Grass Lowland",  &s_state.grassLowland,  0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Snow Line",      &s_state.snowLine,       0.f, 1.f, "%.2f");
    ImGui::Unindent();

    ImGui::Separator();
    // --- Seed ---
    ImGui::Checkbox("Random seed", &s_state.randomSeed);
    if (!s_state.randomSeed) {
        ImGui::SameLine();
        int seed_i = (int)(s_state.seed & 0x7fffffff);
        ImGui::SetNextItemWidth(120.f * sc);
        if (ImGui::InputInt("##seed", &seed_i))
            s_state.seed = (unsigned long)(seed_i < 0 ? 0 : seed_i);
    }

    ImGui::Separator();
    // --- Preview thumbnail ---
    const float previewDisplayW = 256.f * sc;
    if (s_state.previewTexID) {
        ImGui::Text("Preview:");
        float aspect = (s_state.previewH > 0)
                       ? (float)s_state.previewW / (float)s_state.previewH
                       : 1.f;
        float dh = previewDisplayW / aspect;
        ImGui::Image((ImTextureID)(uintptr_t)s_state.previewTexID,
                     ImVec2(previewDisplayW, dh));
    } else {
        ImGui::Dummy(ImVec2(previewDisplayW, previewDisplayW * 0.4f));
        ImGui::SameLine();
        ImGui::TextDisabled("(click Preview)");
    }

    // Status line
    if (s_state.statusMsg[0]) {
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.f), "%s", s_state.statusMsg);
    }

    ImGui::Separator();
    // --- Buttons ---

    // "Load Preset" -- instant load from pre-baked flat terrain files (no Python).
    // Three states based on manifest.json (written last during generation = completion flag):
    //   1. Folder absent          -> preset missing
    //   2. Folder present, no manifest -> incomplete / interrupted generation
    //   3. manifest.json present  -> preset complete, button enabled
    {
        char presetDir[512];
        snprintf(presetDir, sizeof(presetDir),
            "terrain_gen_presets\\%s_%d",
            k_biomeKeys[s_state.biomeIndex],
            k_mapSizeVertices[s_state.mapSizeIndex]);
        char presetManifest[640];
        snprintf(presetManifest, sizeof(presetManifest), "%s\\manifest.json", presetDir);

        bool dirExists      = (GetFileAttributesA(presetDir)      != INVALID_FILE_ATTRIBUTES);
        bool presetComplete = (GetFileAttributesA(presetManifest) != INVALID_FILE_ATTRIBUTES);
        bool presetPartial  = dirExists && !presetComplete;

        if (!presetComplete) ImGui::BeginDisabled();
        if (ImGui::Button("Load Preset", ImVec2(110.f * sc, 0))) {
            snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Loading preset...");
            s_state.pendingAction = PendingAction::LoadPreset;
        }
        if (!presetComplete) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(presetPartial
                    ? "Preset folder exists but is incomplete (generation may have been interrupted).\n"
                      "Re-run: py tools/terrain_gen/gen_presets.py"
                    : "No pre-baked preset for this biome+size.\n"
                      "Run: py tools/terrain_gen/gen_presets.py\n"
                      "Or use Generate to run the full generator.");
            }
        } else {
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Load pre-baked flat terrain instantly (no Python run).\n"
                    "Preset: %s", presetDir);
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Preview (~3s)", ImVec2(120.f * sc, 0))) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Running preview...");
        s_state.pendingAction = PendingAction::Preview;
    }
    ImGui::SameLine();
    if (ImGui::Button("Generate", ImVec2(100.f * sc, 0))) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Generating (please wait)...");
        s_state.pendingAction = PendingAction::Generate;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80.f * sc, 0))) {
        Close();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Deferred execution (called from EditorInterface::update, not inside ImGui)
// ---------------------------------------------------------------------------

void MapGeneratorDialog::ExecutePreview() {
    std::string json = BuildRecipeJSON(/*preview=*/true);
    if (!WriteRecipeFile(json)) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Preview failed: could not write recipe JSON");
        return;
    }
    bool ok = RunTerrainGen(/*preview=*/true);
    if (!ok) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Preview failed: terrain_gen.py returned error "
            "(needs py -3 + tools/terrain_gen/ available)");
        return;
    }
    LoadPreviewPNG();
}

void MapGeneratorDialog::ExecuteGenerate() {
    std::string json = BuildRecipeJSON(/*preview=*/false);
    if (!WriteRecipeFile(json)) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate failed: could not write recipe JSON");
        return;
    }
    bool ok = RunTerrainGen(/*preview=*/false);
    if (!ok) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate failed: terrain_gen.py returned error");
        return;
    }
    // Apply to editor via EditorData.  mapSizeIndex maps 1:1 with
    // genMapSizeToN() indices in EditorData.cpp; terrain=0 is unused when
    // s_genColormapName is set (the generator writes its own burnin colormap).
    bool applied = EditorData::generateFromDialogParams(
        s_state.mapSizeIndex,
        k_biomeKeys[s_state.biomeIndex]);
    if (!applied) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate succeeded but terrain apply failed.");
    } else {
        Close();
    }
}

// Load a pre-baked flat preset produced by tools/terrain_gen/gen_presets.py.
// Copies {biome}_{sizeN}/genmap.elev.r32 and genmap.burnin.tga into
// terrain_gen_out/ (where generateFromDialogParams expects them) then calls
// generateFromDialogParams() -- no Python invocation.
void MapGeneratorDialog::ExecuteLoadPreset() {
    const char* biomeKey = k_biomeKeys[s_state.biomeIndex];
    const int   sizeN    = k_mapSizeVertices[s_state.mapSizeIndex];

    char srcDir[512];
    snprintf(srcDir, sizeof(srcDir), "terrain_gen_presets\\%s_%d", biomeKey, sizeN);

    // Verify manifest (written last during generation — presence guarantees data files intact)
    char srcManifest[640], srcElev[640], srcBurnin[640], srcPreview[640];
    snprintf(srcManifest, sizeof(srcManifest), "%s\\manifest.json",      srcDir);
    snprintf(srcElev,     sizeof(srcElev),     "%s\\genmap.elev.r32",    srcDir);
    snprintf(srcBurnin,   sizeof(srcBurnin),   "%s\\genmap.burnin.tga",  srcDir);
    snprintf(srcPreview,  sizeof(srcPreview),  "%s\\genmap.preview.png", srcDir);

    if (GetFileAttributesA(srcManifest) == INVALID_FILE_ATTRIBUTES) {
        bool dirExists = (GetFileAttributesA(srcDir) != INVALID_FILE_ATTRIBUTES);
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            dirExists
                ? "Preset incomplete — re-run tools/terrain_gen/gen_presets.py"
                : "Preset not found: %s", srcDir);
        return;
    }

    // Ensure output directory exists
    CreateDirectoryA("terrain_gen_out", NULL);

    // Atomic copy: write both files to .tmp names first, then rename when both succeed.
    // This guarantees terrain_gen_out/ never holds a mixed old+new terrain pair.
    const char* dstElev   = "terrain_gen_out\\genmap.elev.r32";
    const char* dstBurnin = "terrain_gen_out\\genmap.burnin.tga";
    const char* tmpElev   = "terrain_gen_out\\genmap.elev.r32.tmp";
    const char* tmpBurnin = "terrain_gen_out\\genmap.burnin.tga.tmp";

    DeleteFileA(tmpElev);   // clear any stale temps from a prior interrupted copy
    DeleteFileA(tmpBurnin);

    bool ok = CopyFileA(srcElev,   tmpElev,   FALSE)
           && CopyFileA(srcBurnin, tmpBurnin, FALSE);

    if (!ok) {
        DeleteFileA(tmpElev);
        DeleteFileA(tmpBurnin);
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Failed to copy preset files from %s", srcDir);
        return;
    }

    // Both temp files written successfully — commit by replacing the live files.
    SetFileAttributesA(dstElev,   FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesA(dstBurnin, FILE_ATTRIBUTE_NORMAL);
    MoveFileExA(tmpElev,   dstElev,   MOVEFILE_REPLACE_EXISTING);
    MoveFileExA(tmpBurnin, dstBurnin, MOVEFILE_REPLACE_EXISTING);

    // Preview is optional; don't fail if absent
    if (GetFileAttributesA(srcPreview) != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesA("terrain_gen_out\\genmap.preview.png", FILE_ATTRIBUTE_NORMAL);
        CopyFileA(srcPreview, "terrain_gen_out\\genmap.preview.png", FALSE);
        LoadPreviewPNG();
    }

    // Hand off to editor (same path as ExecuteGenerate, no Python needed)
    bool applied = EditorData::generateFromDialogParams(s_state.mapSizeIndex, biomeKey);
    if (!applied) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Preset files copied but terrain apply failed.");
    } else {
        Close();
    }
}

#endif // MC2_IMGUI
