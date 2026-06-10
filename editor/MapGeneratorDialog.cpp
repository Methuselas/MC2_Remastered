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
* Map size labels and k_mapSizeCellSides use the CELL (quad) count per side — the
* user-facing dimension.  Vertex count = cellSide + 1.  The terrain generator recipe
* uses the vertex count so the generator produces the right grid size.  Use
* EditorData::MapSizeToCellSide() / MapSizeToVertexSide() for all conversions.
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
#include "EditorTaskRunner.h"  // async subprocess (Phase 1: no UI-thread blocking)
#include "FoliageRender.h"     // load generated foliage sidecar after apply

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

// MapSize index -> cell side (quads per side, user-facing label).  Always multiples of 20.
// Use EditorData::MapSizeToCellSide() / MapSizeToVertexSide() for conversions instead of
// indexing this array directly — it is kept here only for the preset dir name and recipe.
static const int   k_mapSizeCellSides[7] = { 60, 80, 100, 120, 260, 520, 1020 };
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
    float meanHeight     = 0.5f; // 0..1 slider: controls elevation baseline
    float variation      = 0.5f; // 0..1 slider: controls mountain + ridged amount
    float mountainAmount = 0.5f; // 0..1 slider; mapped to recipe [0.1..0.9]
    float ridgedAmount   = 0.3f; // 0..1 slider
    float grassLowland   = 0.5f;
    float snowLine       = 1.0f; // Default to 1.0 (snow only at very top)

    // Foliage / trees (PCG sidecar -> generator --generate-foliage). When on, the
    // recipe carries a "foliage" rules array and the generated genmap.foliage.json
    // is loaded into FoliageRender after the terrain applies.
    bool  generateFoliage = true;
    float treeDensity     = 0.5f; // 0..1 -> rule density
    float treeLine        = 0.85f;// 0..1 -> tree max_altitude (no trees above)
    bool  placeRocks      = true; // scatter rocks on slopes

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

// Async Generate hand-off (all touched on the main thread):
//   s_generateReady       -- task succeeded; the terrain apply is pending. The apply
//                            itself is deferred to EditorInterface::update() so it can
//                            run in the proven order eye->reset() -> apply (which primes
//                            the terrain face cache against the FINAL camera) -> setPos.
//   s_pendingApplySizeIdx  -- map-size index captured at task START (slider-change safe)
//   s_pendingApplyBiome    -- biome key captured at task START
static bool        s_generateReady       = false;
static int         s_pendingApplySizeIdx = 0;
static std::string s_pendingApplyBiome;

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

    // Recipe "size" is the VERTEX count so the generator produces the right grid.
    int sideN    = EditorData::MapSizeToVertexSide(s_state.mapSizeIndex);

    // Derive mountain and ridged amounts from variation slider
    float mnt    = SliderToMountainAmount(s_state.variation);
    float ridged = SliderToRidgedAmount(s_state.variation);

    // Derive minElevation from meanHeight slider (0..1 maps to 0..500)
    float minElev = s_state.meanHeight * 500.f;

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
        "  }",                       // NOTE: no top-level close yet (foliage may follow)
        sideN,
        k_biomeKeys[s_state.biomeIndex],
        seed,
        (double)s_state.maxElevation,
        (double)minElev,
        (double)mnt,
        (double)ridged,
        (double)s_state.grassLowland,
        (double)s_state.snowLine
    );

    std::string recipe(buf);

    // Optional foliage rules: trees on non-water below the tree line, plus rocks
    // on slopes. Density slider [0..1] -> rule density [0.004..0.030].
    if (s_state.generateFoliage) {
        float treeD = 0.004f + s_state.treeDensity * 0.026f;
        float treeMaxAlt = s_state.treeLine;
        char fol[1024];
        int n = snprintf(fol, sizeof(fol),
            ",\n  \"foliage\": [\n"
            "    { \"asset\": \"tree_pine_a\", \"kind\": \"tree\", \"density\": %.4f,\n"
            "      \"avoid_water\": true, \"max_altitude\": %.3f, \"max_slope\": 0.55,\n"
            "      \"min_spacing\": 256.0, \"scale_min\": 0.8, \"scale_max\": 1.25 }",
            (double)treeD, (double)treeMaxAlt);
        recipe += fol;
        if (s_state.placeRocks) {
            char rk[768];
            snprintf(rk, sizeof(rk),
                ",\n"
                "    { \"asset\": \"rock_a\", \"kind\": \"rock\", \"density\": %.4f,\n"
                "      \"avoid_water\": true, \"min_altitude\": 0.20, \"max_slope\": 1.0,\n"
                "      \"min_spacing\": 320.0, \"scale_min\": 0.7, \"scale_max\": 1.4 }",
                (double)(treeD * 0.3f));
            recipe += rk;
        }
        recipe += "\n  ]";
        (void)n;
    }

    recipe += "\n}\n";
    return recipe;
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

// Build the terrain-generator command line.  Launches Python UNBUFFERED (-u) so
// PROGRESS lines reach the task runner's pipe immediately instead of being held in
// stdio buffers until exit.  preview=true adds --preview (fast, ~2-3s).
static std::string BuildTerrainGenCmd(bool preview) {
    const char* recipeArg = "terrain_gen_out\\genmap_recipe.json";
    const char* outArg    = "terrain_gen_out";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "py -3 -u tools\\terrain_gen\\terrain_gen.py \"%s\" --out \"%s\"%s%s",
        recipeArg, outArg,
        preview ? " --preview" : "",
        // Foliage is a full-generate artifact only; skip it for the fast preview.
        (s_state.generateFoliage && !preview) ? " --generate-foliage" : "");
    return std::string(cmd);
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
    ImGui::SliderFloat("Mean Height (baseline)",  &s_state.meanHeight, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Peak Height (max)",  &s_state.maxElevation, 100.f, 600.f, "%.0f");

    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Variation (flat to mountainous)", &s_state.variation, 0.f, 1.f, "%.2f");
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
    // --- Foliage / Trees ---
    ImGui::Text("Foliage");
    ImGui::Indent();
    ImGui::Checkbox("Generate trees & rocks", &s_state.generateFoliage);
    if (s_state.generateFoliage) {
        ImGui::SetNextItemWidth(280.f * sc);
        ImGui::SliderFloat("Tree Density", &s_state.treeDensity, 0.f, 1.f, "%.2f");
        ImGui::SetNextItemWidth(280.f * sc);
        ImGui::SliderFloat("Tree Line (max altitude)", &s_state.treeLine, 0.f, 1.f, "%.2f");
        ImGui::Checkbox("Scatter rocks on slopes", &s_state.placeRocks);
        ImGui::TextDisabled("Trees apply on full Generate (not the fast Preview).");
    }
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
            EditorData::MapSizeToCellSide(s_state.mapSizeIndex));
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

    // Disable Preview/Generate while an async generation task is in flight so the
    // user cannot launch a second overlapping run (apply order would be ambiguous).
    const bool taskBusy = EditorTaskRunner::HasActiveTasks();
    if (taskBusy) ImGui::BeginDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Preview (~3s)", ImVec2(120.f * sc, 0))) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Queuing preview...");
        s_state.pendingAction = PendingAction::Preview;
    }
    ImGui::SameLine();
    if (ImGui::Button("Generate", ImVec2(100.f * sc, 0))) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Queuing generate...");
        s_state.pendingAction = PendingAction::Generate;
    }
    if (taskBusy) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80.f * sc, 0))) {
        Close();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Deferred execution (called from EditorInterface::update, not inside ImGui)
// ---------------------------------------------------------------------------

bool MapGeneratorDialog::IsTaskActive() {
    return EditorTaskRunner::HasActiveTasks();
}

bool MapGeneratorDialog::GenerateReady() {
    return s_generateReady;
}

// Apply the terrain generated by the just-finished async task. MAIN THREAD ONLY,
// and the caller MUST have already reset the camera (eye->reset()) so that the
// terrain face cache primed inside generateFromDialogParams culls against the final
// view -- otherwise the terrain renders empty/black. Returns true on a clean apply.
bool MapGeneratorDialog::ApplyPendingGenerate() {
    if (!s_generateReady)
        return false;
    s_generateReady = false;

    bool applied = EditorData::generateFromDialogParams(
        s_pendingApplySizeIdx, s_pendingApplyBiome.c_str());
    if (!applied) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate succeeded but terrain apply failed.");
        return false;
    }
    EditorData::MarkMoveDataDirty();

    // Load the generated foliage sidecar so trees/rocks show immediately. Load is
    // failure-tolerant (missing/empty -> 0 instances, no crash); force-visible.
    if (s_state.generateFoliage) {
        if (FoliageRender::Load("terrain_gen_out\\genmap.foliage.json")) {
            if (!FoliageRender::Visible())
                FoliageRender::Toggle();
        }
    }

    Close();
    return true;
}

// Preview: run the generator (async) then load the thumbnail PNG on success.
// The success callback runs on the MAIN thread (glGenTextures is main-thread only).
void MapGeneratorDialog::ExecutePreview() {
    if (EditorTaskRunner::HasActiveTasks()) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "A generation task is already running.");
        return;
    }
    std::string json = BuildRecipeJSON(/*preview=*/true);
    if (!WriteRecipeFile(json)) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Preview failed: could not write recipe JSON");
        return;
    }

    EditorTaskRunner::TaskSpec spec;
    spec.name        = "Preview terrain";
    spec.commandLine = BuildTerrainGenCmd(/*preview=*/true);
    spec.onSuccessMainThread = [](const EditorTaskRunner::TaskResult&) {
        LoadPreviewPNG();   // main thread: creates GL texture, sets statusMsg
    };
    spec.onFailureMainThread = [](const EditorTaskRunner::TaskResult& r) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Preview failed (exit %d). See Task Monitor log.", r.exitCode);
    };
    spec.onCancelMainThread = []() {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Preview cancelled.");
    };
    EditorTaskRunner::StartTask(spec);
    snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
        "Running preview in background... (Task Monitor)");
}

// Generate: run the generator (async); on success apply terrain to the editor on
// the MAIN thread (generateFromDialogParams touches GL/editor state).  Outputs are
// applied ONLY when the subprocess exits 0 -- cancel/failure leaves the map untouched.
//
// TODO(phase1-atomicity): the generator writes directly into terrain_gen_out/.
// A cancel mid-write can leave partial files there, but they are never APPLIED (no
// success callback), and the next successful run overwrites them.  A future pass
// should generate into a temp dir and atomic-rename into terrain_gen_out/ on success.
void MapGeneratorDialog::ExecuteGenerate() {
    if (EditorTaskRunner::HasActiveTasks()) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "A generation task is already running.");
        return;
    }
    std::string json = BuildRecipeJSON(/*preview=*/false);
    if (!WriteRecipeFile(json)) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate failed: could not write recipe JSON");
        return;
    }

    // Snapshot the params NOW so later slider changes can't alter this run's apply.
    s_pendingApplySizeIdx = s_state.mapSizeIndex;
    s_pendingApplyBiome   = k_biomeKeys[s_state.biomeIndex];

    EditorTaskRunner::TaskSpec spec;
    spec.name        = "Generate terrain";
    spec.commandLine = BuildTerrainGenCmd(/*preview=*/false);
    spec.onSuccessMainThread = [](const EditorTaskRunner::TaskResult&) {
        // Do NOT apply here -- the apply primes the terrain face cache against the
        // current camera, so it must run AFTER eye->reset() in EditorInterface::update().
        // Just signal readiness; update() drives reset -> ApplyPendingGenerate() -> setPos.
        s_generateReady = true;
    };
    spec.onFailureMainThread = [](const EditorTaskRunner::TaskResult& r) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate failed (exit %d). See Task Monitor log.", r.exitCode);
    };
    spec.onCancelMainThread = []() {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate cancelled (map unchanged).");
    };
    EditorTaskRunner::StartTask(spec);
    snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
        "Generating in background... (Task Monitor)");
}

// Load a pre-baked flat preset produced by tools/terrain_gen/gen_presets.py.
// Copies {biome}_{sizeN}/genmap.elev.r32 and genmap.burnin.tga into
// terrain_gen_out/ (where generateFromDialogParams expects them) then calls
// generateFromDialogParams() -- no Python invocation.
void MapGeneratorDialog::ExecuteLoadPreset() {
    const char* biomeKey = k_biomeKeys[s_state.biomeIndex];
    // Preset dirs are named by CELL side (user-facing).  e.g. desert_60, not desert_61.
    const int   sizeN    = EditorData::MapSizeToCellSide(s_state.mapSizeIndex);

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
        // Build MOVE in-memory immediately so the passability grid works without
        // requiring a save/load round-trip.  land is ready at this point.
        // Mark MOVE dirty — rebuild fires in EditorInterface::update() after this
        // frame, once terrain/overlay/object state has fully settled.
        EditorData::MarkMoveDataDirty();
        Close();
    }
}

#endif // MC2_IMGUI
