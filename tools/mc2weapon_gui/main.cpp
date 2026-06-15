// mc2weapon_gui — Dear ImGui (SDL2 + OpenGL3) weapon editor for MC2.
//
// Foolproof front-end over the same CSV model as tools/mc2weapon/mc2weapon.py:
// enum fields are dropdowns (can't typo a Type/Range/FX), numbers are validated
// live, and Save is disabled while anything is invalid. Save writes a loose
// mods/<id>/data/objects/compbas.csv overlay (+ mod.json) that the game resolves
// first via MC2_ACTIVE_MOD — no .pak repack. No engine/game code linked.
//
// Build: -DENABLE_MC2WEAPON_GUI=ON (guarded in top CMakeLists).
// --headless-smoke [--frames N]: load + render N frames (or core-only fallback
// if no display) then exit 0 — autonomous build/run verification.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "weapon_csv.h"

#include <GL/glew.h>
#include <SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

namespace {

const char* kDefaultCompbasCandidates[] = {
    "data/objects/compbas.csv",
    "A:/Games/mc2-opengl-src/mc2srcdata/objects/compbas.csv",
    "A:/Games/mc2-opengl/mc2-win64-v0.4/data/objects/compbas.csv",
};
const char* kDefaultEffectsCandidates[] = {
    "data/objects/effects.csv",
    "A:/Games/mc2-opengl/mc2-win64-v0.4/data/objects/effects.csv",
    "A:/Games/mc2-opengl-src/mc2srcdata/objects/effects.csv",
};

struct App {
    mc2w::Compbas cb;
    std::vector<mc2w::FxEntry> fx;
    char filter[64] = "";
    bool showAll = false;
    int selectedRow = -1;          // data-row index in cb.rows
    // edit buffers for the selected weapon
    char eName[128] = "";
    int eType = 0;                 // index into kWeaponTypes
    char eDamage[32] = "", eHeat[32] = "", eRecycle[32] = "", eTons[32] = "";
    char eSlots[32] = "";
    int eRange = 1;                // index into kRanges
    int eFx = 0;                   // index into fx palette
    char eMissileType[16] = "", eFields[16] = "", eAmmo[16] = "";
    char modId[64] = "my-weapons";
    char modRoot[256] = "mods";
    char newId[16] = "";
    std::string status;
};

bool fileExists(const char* p) { FILE* f = std::fopen(p, "rb"); if (f) { std::fclose(f); return true; } return false; }
const char* pickPath(const char* const* cands, int n, const char* override_) {
    if (override_ && *override_) return override_;
    for (int i = 0; i < n; ++i) if (fileExists(cands[i])) return cands[i];
    return cands[0];
}

void setBuf(char* dst, size_t cap, const std::string& s) {
    std::snprintf(dst, cap, "%s", s.c_str());
}

int findFxIndex(const std::vector<mc2w::FxEntry>& fx, const std::string& fxidStr) {
    long id = std::strtol(fxidStr.c_str(), nullptr, 10);
    for (size_t i = 0; i < fx.size(); ++i) if (fx[i].id == (int)id) return (int)i;
    return 0;
}
int comboIndex(const char* const* opts, int n, const std::string& v) {
    std::string lv;
    for (char c : v) lv += (char)tolower((unsigned char)c);
    for (int i = 0; i < n; ++i) {
        std::string o;
        for (const char* p = opts[i]; *p; ++p) o += (char)tolower((unsigned char)*p);
        if (o == lv) return i;
    }
    return -1;
}

void loadSelectionToBuffers(App& a) {
    int r = a.selectedRow;
    const auto& I = a.cb.idx;
    setBuf(a.eName, sizeof a.eName, a.cb.cell(r, I.name));
    setBuf(a.eDamage, sizeof a.eDamage, a.cb.cell(r, I.damage));
    setBuf(a.eHeat, sizeof a.eHeat, a.cb.cell(r, I.heat));
    setBuf(a.eRecycle, sizeof a.eRecycle, a.cb.cell(r, I.recycle));
    setBuf(a.eTons, sizeof a.eTons, a.cb.cell(r, I.tons));
    setBuf(a.eSlots, sizeof a.eSlots, a.cb.cell(r, I.slots));
    setBuf(a.eMissileType, sizeof a.eMissileType, a.cb.cell(r, I.missileType));
    setBuf(a.eFields, sizeof a.eFields, a.cb.cell(r, I.fields));
    setBuf(a.eAmmo, sizeof a.eAmmo, a.cb.cell(r, I.ammoMasterId));
    int ti = comboIndex(mc2w::kWeaponTypes, 3, a.cb.cell(r, I.type));
    a.eType = ti < 0 ? 0 : ti;
    int ri = comboIndex(mc2w::kRanges, 3, a.cb.cell(r, I.range));
    a.eRange = ri < 0 ? 1 : ri;
    a.eFx = findFxIndex(a.fx, a.cb.cell(r, I.fxid));
}

// apply edit buffers back into the selected row
void buffersToRow(App& a) {
    int r = a.selectedRow;
    const auto& I = a.cb.idx;
    a.cb.setCell(r, I.name, a.eName);
    a.cb.setCell(r, I.type, mc2w::kWeaponTypes[a.eType]);
    a.cb.setCell(r, I.damage, a.eDamage);
    a.cb.setCell(r, I.heat, a.eHeat);
    a.cb.setCell(r, I.recycle, a.eRecycle);
    a.cb.setCell(r, I.tons, a.eTons);
    a.cb.setCell(r, I.slots, a.eSlots);
    a.cb.setCell(r, I.range, mc2w::kRanges[a.eRange]);
    a.cb.setCell(r, I.missileType, a.eMissileType);
    a.cb.setCell(r, I.fields, a.eFields);
    a.cb.setCell(r, I.ammoMasterId, a.eAmmo);
    if (!a.fx.empty()) {
        char fxbuf[16];
        std::snprintf(fxbuf, sizeof fxbuf, "%d", a.fx[a.eFx].id);
        a.cb.setCell(r, I.fxid, fxbuf);
    }
}

// validated InputText: red border + returns whether valid
bool field(const char* label, char* buf, size_t cap, const char* kind,
           const std::vector<mc2w::FxEntry>& fx, int width = 120) {
    std::string err = mc2w::validateCell(kind, buf, fx);
    bool ok = err.empty();
    if (!ok) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.12f, 0.12f, 1.0f));
    ImGui::SetNextItemWidth((float)width);
    ImGui::InputText(label, buf, cap);
    if (!ok) {
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", err.c_str());
    }
    return ok;
}

void drawList(App& a) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 780), ImGuiCond_FirstUseEver);
    ImGui::Begin("Weapons");
    ImGui::InputText("filter", a.filter, sizeof a.filter);
    ImGui::SameLine();
    ImGui::Checkbox("all components", &a.showAll);
    ImGui::Separator();
    ImGui::BeginChild("list", ImVec2(0, 0), false);
    std::string flt;
    for (char* p = a.filter; *p; ++p) flt += (char)tolower((unsigned char)*p);
    for (int i = 0; i < (int)a.cb.rows.size(); ++i) {
        std::string type = a.cb.cell(i, a.cb.idx.type);
        if (!a.showAll && !mc2w::isWeaponType(type)) continue;
        std::string name = a.cb.cell(i, a.cb.idx.name);
        std::string mid = a.cb.cell(i, a.cb.idx.masterID);
        if (!flt.empty()) {
            std::string nl;
            for (char c : name) nl += (char)tolower((unsigned char)c);
            if (nl.find(flt) == std::string::npos) continue;
        }
        char label[160];
        std::snprintf(label, sizeof label, "[%s] %s##%d", mid.c_str(), name.c_str(), i);
        if (ImGui::Selectable(label, a.selectedRow == i)) {
            a.selectedRow = i;
            loadSelectionToBuffers(a);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void drawEditor(App& a) {
    ImGui::SetNextWindowPos(ImVec2(400, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(860, 780), ImGuiCond_FirstUseEver);
    ImGui::Begin("Editor");

    // create-new section
    ImGui::TextUnformatted("New weapon (unused masterID):");
    ImGui::SetNextItemWidth(80);
    ImGui::InputText("masterID##new", a.newId, sizeof a.newId);
    ImGui::SameLine();
    if (ImGui::Button("Create")) {
        int r = a.cb.findByMasterId(a.newId);
        if (r < 0) a.status = std::string("masterID ") + a.newId + " not in compbas";
        else {
            a.selectedRow = r;
            loadSelectionToBuffers(a);
            // seed as a weapon if currently not one
            if (!mc2w::isWeaponType(a.cb.cell(r, a.cb.idx.type))) {
                a.eType = 0;
                setBuf(a.eName, sizeof a.eName, "New Weapon");
                setBuf(a.eDamage, sizeof a.eDamage, "1");
                setBuf(a.eHeat, sizeof a.eHeat, "1");
                setBuf(a.eRecycle, sizeof a.eRecycle, "3");
                setBuf(a.eTons, sizeof a.eTons, "1");
                setBuf(a.eSlots, sizeof a.eSlots, "1");
                a.eRange = 1;
            }
            a.status = std::string("editing masterID ") + a.newId;
        }
    }
    ImGui::Separator();

    if (a.selectedRow < 0) {
        ImGui::TextUnformatted("Select a weapon from the list, or Create one.");
        ImGui::End();
        return;
    }

    ImGui::Text("masterID %s", a.cb.cell(a.selectedRow, a.cb.idx.masterID).c_str());

    bool ok = true;
    ok &= field("name", a.eName, sizeof a.eName, "str", a.fx, 200);

    ImGui::SetNextItemWidth(160);
    ImGui::Combo("type", &a.eType, mc2w::kWeaponTypes, 3);

    ok &= field("damage", a.eDamage, sizeof a.eDamage, "ufloat", a.fx);
    ok &= field("heat", a.eHeat, sizeof a.eHeat, "ufloat", a.fx);
    ok &= field("recycle (s)", a.eRecycle, sizeof a.eRecycle, "ufloat", a.fx);
    ok &= field("tons", a.eTons, sizeof a.eTons, "ufloat", a.fx);
    ok &= field("slots", a.eSlots, sizeof a.eSlots, "uint", a.fx);

    ImGui::SetNextItemWidth(120);
    ImGui::Combo("range", &a.eRange, mc2w::kRanges, 3);

    // FX picker
    if (!a.fx.empty()) {
        std::string preview;
        {
            char b[96];
            std::snprintf(b, sizeof b, "%d: %s", a.fx[a.eFx].id, a.fx[a.eFx].name.c_str());
            preview = b;
        }
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("FX", preview.c_str())) {
            for (int i = 0; i < (int)a.fx.size(); ++i) {
                char b[96];
                std::snprintf(b, sizeof b, "%d: %s", a.fx[i].id, a.fx[i].name.c_str());
                if (ImGui::Selectable(b, a.eFx == i)) a.eFx = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("hit=%s miss=%s", a.fx[a.eFx].hit.c_str(), a.fx[a.eFx].miss.c_str());
    }

    ok &= field("missileType", a.eMissileType, sizeof a.eMissileType, "int", a.fx, 60);
    ok &= field("fields", a.eFields, sizeof a.eFields, "int", a.fx, 60);
    ok &= field("ammoMasterId", a.eAmmo, sizeof a.eAmmo, "int", a.fx, 60);

    ImGui::Separator();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("mod id", a.modId, sizeof a.modId);
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("mod root", a.modRoot, sizeof a.modRoot);

    if (!ok) ImGui::BeginDisabled();
    if (ImGui::Button("Save overlay")) {
        buffersToRow(a);
        std::string outPath, err;
        if (mc2w::writeOverlay(a.modRoot, a.modId, a.cb, outPath, err))
            a.status = "saved " + outPath + "  (run MC2_ACTIVE_MOD=" + a.modId + ")";
        else
            a.status = "save FAILED: " + err;
    }
    if (!ok) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "fix invalid fields to save");
    }

    if (!a.status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", a.status.c_str());
    }
    ImGui::End();
}

int coreOnly(App& a, const char* cbp, const char* efp) {
    std::printf("[mode] core-only (no display)\n");
    std::printf("loaded %zu components from %s\n", a.cb.rows.size(), cbp);
    int weapons = 0;
    for (int i = 0; i < (int)a.cb.rows.size(); ++i)
        if (mc2w::isWeaponType(a.cb.cell(i, a.cb.idx.type))) ++weapons;
    std::printf("  %d weapons; %zu FX entries from %s\n", weapons, a.fx.size(), efp);
    return 0;
}

}  // namespace

// Exercise the C++ load -> edit -> writeOverlay -> reload round-trip (no SDL).
int selftest(App& a, const char* cbp) {
    int r = -1;
    for (int i = 0; i < (int)a.cb.rows.size(); ++i)
        if (mc2w::isWeaponType(a.cb.cell(i, a.cb.idx.type))) { r = i; break; }
    if (r < 0) { std::printf("SELFTEST FAIL: no weapon found\n"); return 1; }
    a.selectedRow = r;
    loadSelectionToBuffers(a);
    std::string mid = a.cb.cell(r, a.cb.idx.masterID);
    setBuf(a.eDamage, sizeof a.eDamage, "42");
    buffersToRow(a);
    std::string outPath, err;
    if (!mc2w::writeOverlay("build64/out/tools/mc2weapon_gui/selftest", "st", a.cb, outPath, err)) {
        std::printf("SELFTEST FAIL: write: %s\n", err.c_str()); return 1;
    }
    mc2w::Compbas re;
    if (!re.load(outPath, err)) { std::printf("SELFTEST FAIL: reload: %s\n", err.c_str()); return 1; }
    int rr = re.findByMasterId(mid);
    std::string got = re.cell(rr, re.idx.damage);
    bool ok = (rr >= 0 && got == "42") && (int)re.rows.size() == (int)a.cb.rows.size();
    std::printf("SELFTEST %s: masterID %s damage='%s' (rows %zu==%zu), overlay=%s\n",
                ok ? "PASS" : "FAIL", mid.c_str(), got.c_str(),
                re.rows.size(), a.cb.rows.size(), outPath.c_str());
    (void)cbp;
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    bool headless = false, doselftest = false;
    int frames = 30;
    const char* cbOverride = nullptr;
    const char* efOverride = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--headless-smoke") headless = true;
        else if (s == "--selftest") doselftest = true;
        else if (s == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (s == "--compbas" && i + 1 < argc) cbOverride = argv[++i];
        else if (s == "--effects" && i + 1 < argc) efOverride = argv[++i];
    }
    const char* cbp = pickPath(kDefaultCompbasCandidates, 3, cbOverride);
    const char* efp = pickPath(kDefaultEffectsCandidates, 3, efOverride);

    App app;
    std::string err;
    if (!app.cb.load(cbp, err)) { std::fprintf(stderr, "load compbas: %s\n", err.c_str()); return 2; }
    if (!mc2w::loadEffects(efp, app.fx, err)) { std::fprintf(stderr, "load effects: %s\n", err.c_str()); return 2; }
    std::printf("mc2weapon_gui: %zu components, %zu FX from %s / %s\n",
                app.cb.rows.size(), app.fx.size(), cbp, efp);

    if (doselftest) return selftest(app, cbp);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        if (headless) return coreOnly(app, cbp, efp);
        return 3;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    Uint32 winFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (headless) winFlags |= SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow("mc2weapon — weapon editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800, winFlags);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit();
                   if (headless) return coreOnly(app, cbp, efp); return 3; }
    SDL_GLContext glc = SDL_GL_CreateContext(window);
    if (!glc) { std::fprintf(stderr, "GL context: %s\n", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit();
                if (headless) return coreOnly(app, cbp, efp); return 3; }
    SDL_GL_MakeCurrent(window, glc);
    SDL_GL_SetSwapInterval(headless ? 0 : 1);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { std::fprintf(stderr, "glewInit failed\n");
        SDL_GL_DeleteContext(glc); SDL_DestroyWindow(window); SDL_Quit();
        if (headless) return coreOnly(app, cbp, efp); return 3; }
    while (glGetError() != GL_NO_ERROR) {}

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, glc);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool running = true;
    int frame = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window)) running = false;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        drawList(app);
        drawEditor(app);

        ImGui::Render();
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        ++frame;
        if (headless && frame >= frames) running = false;
    }
    if (headless) std::printf("rendered %d frames, exiting cleanly\n", frame);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(glc);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
