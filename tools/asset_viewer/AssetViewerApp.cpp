/***************************************************************
 * FILENAME: AssetViewerApp.cpp
 * DESCRIPTION: Top-level application class for mc2_asset_viewer.
 ***************************************************************/
#include "AssetViewerApp.h"
#include "SphereMesh.h"
#include "LocalPbrMaterialBackend.h"
#include "UiEditorImageCache.h"
#include "imgui.h"
#include "TextureExtensions.h"
#include "TextureDecoderRegistry.h"
#include "TextureMetadata.h"
#include "TexturePreview2D.h"
#include "Ktx2Decoder.h"
#include <SDL.h>
#include <GL/glew.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

// GUI mode: ctor/dtor own the cache lifetime. The static runSmoke() path does
// NOT construct an AssetViewerApp and does its own Initialize/Shutdown — the two
// paths are mutually exclusive, so there is no double-init.
AssetViewerApp::AssetViewerApp()  { UiEditorImageCache_Initialize(); }
AssetViewerApp::~AssetViewerApp() { UiEditorImageCache_Shutdown(); }

void AssetViewerApp::drawUi()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("MC2 Asset Viewer", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float sidebarW = 180.0f, browserW = 300.0f;
    ImGui::BeginChild("sidebar", ImVec2(sidebarW, 0), true);
    sidebar_.draw();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("browser", ImVec2(browserW, 0), true);
    browser_.draw();
    if (browser_.hasSelection())
        surface_.setSource(browser_.takeSelection());
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("inspector", ImVec2(0, 0), true);
    inspector_.draw(surface_);
    ImGui::EndChild();

    ImGui::End();
}

static int smokeFail(const char* msg) { std::fprintf(stderr, "[smoke] FAIL: %s\n", msg); return 1; }

int AssetViewerApp::runSmoke(const char* fixtureDir)
{
    if (!IsSupportedTextureFile("a.PNG"))  return smokeFail("PNG should be supported");
    if ( IsSupportedTextureFile("a.dds"))  return smokeFail("dds should NOT be supported");
    if ( IsSupportedTextureFile("noext"))  return smokeFail("extensionless should NOT be supported");
    if (FormatDimensions(TextureMetadata{256,128,4,0}) != "256 x 128") return smokeFail("dims format");
    if (FormatChannels(TextureMetadata{0,0,4,0})       != "RGBA")      return smokeFail("channels format");
    { TextureMetadata m; m.fileBytes = 1572864; if (FormatFileSize(m) != "1.5 MB") return smokeFail("size format"); }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError(); // consume glew's spurious error

    int rc = 0;
    {
        UiEditorImageCache_Initialize();
        TexturePreview2D surface;
        std::string path = (std::filesystem::path(fixtureDir) / "test_rgba.png").string();
        surface.setSource(path);
        if (surface.hasError())               rc = smokeFail("fixture failed to load");
        else if (surface.metadata().width  != 4) rc = smokeFail("fixture width != 4");
        else if (surface.metadata().height != 2) rc = smokeFail("fixture height != 2");
        UiEditorImageCache_Shutdown();
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS\n");
    return rc;
}

int AssetViewerApp::runSmokeKtxParse(const char* dir)
{
    namespace fs = std::filesystem;
    auto p = [&](const char* f){ return (fs::path(dir) / f).string(); };

    Ktx2DecodedImage rgba = loadKtx2Image(p("tex_rgba8.ktx2"));
    if (!rgba.ok)                       return smokeFail("rgba8 ktx2 should parse");
    if (rgba.img.width != 4 || rgba.img.height != 2) return smokeFail("rgba8 dims");
    if (rgba.img.isCompressed)          return smokeFail("rgba8 should not be compressed");

    // Validate the hand-written BC7 fixture against the REAL loader here (no GL).
    // The GL smoke's BC7 path is BPTC-gated and may SKIP, so this is the only
    // guard that a malformed BC7 fixture cannot ship green. (review fix: KTX2 #1)
    Ktx2DecodedImage bc7 = loadKtx2Image(p("tex_bc7.ktx2"));
    if (!bc7.ok)                        return smokeFail("bc7 ktx2 should parse");
    if (!bc7.img.isCompressed)          return smokeFail("bc7 should be compressed");
    if (bc7.img.width != 4 || bc7.img.height != 2) return smokeFail("bc7 dims");

    Ktx2DecodedImage sup = loadKtx2Image(p("tex_super.ktx2"));
    if (sup.ok || sup.error.find("Supercompressed") == std::string::npos)
        return smokeFail("supercompressed should be classified");

    Ktx2DecodedImage bad = loadKtx2Image(p("tex_badfmt.ktx2"));
    if (bad.ok || bad.error.find("Unsupported KTX2 format") == std::string::npos)
        return smokeFail("bad format should be classified");

    Ktx2DecodedImage missing = loadKtx2Image(p("does_not_exist.ktx2"));
    if (missing.ok || missing.error.find("not found") == std::string::npos)
        return smokeFail("missing file should be classified");

    std::printf("[smoke] PASS ktx parse+classify\n");
    return 0;
}

int AssetViewerApp::runSmokeDecoder()
{
    auto& reg = textureDecoderRegistry();
    if (!reg.isSupported("a.PNG"))      return smokeFail("png should be supported via registry");
    if (!reg.isSupported("a.tga"))      return smokeFail("tga should be supported via registry");
    if ( reg.isSupported("a.dds"))      return smokeFail("dds should NOT be supported");
    if ( reg.isSupported("noext"))      return smokeFail("extensionless should NOT be supported");
    // IsSupportedTextureFile must now agree with the registry.
    if (!IsSupportedTextureFile("a.png")) return smokeFail("IsSupportedTextureFile png");
    if ( IsSupportedTextureFile("a.dds")) return smokeFail("IsSupportedTextureFile dds");
    if (TextureExtLower("X/Y.KTX2") != "ktx2") return smokeFail("TextureExtLower");
    if (!reg.isSupported("a.ktx2"))     return smokeFail("ktx2 should be supported now");
    {
        auto exts = reg.supportedExtensions();
        bool hasKtx2 = false; for (auto& e : exts) if (e == "ktx2") hasKtx2 = true;
        if (!hasKtx2) return smokeFail("supportedExtensions missing ktx2");
    }
    std::printf("[smoke] PASS decoder registry\n");
    return 0;
}

int AssetViewerApp::runSmokeKtx(const char* dir)
{
    namespace fs = std::filesystem;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke-ktx", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError(); // consume glew's spurious error

    int rc = 0;
    auto& reg = textureDecoderRegistry();

    // RGBA8 path
    {
        DecodedTexture d = reg.load((fs::path(dir) / "tex_rgba8.ktx2").string());
        if (!d.error.empty())           rc = smokeFail("rgba8 ktx upload error");
        else if (d.glTexture == 0)      rc = smokeFail("rgba8 ktx no texture");
        else if (d.width != 4 || d.height != 2) rc = smokeFail("rgba8 ktx dims");
        else if (d.formatLabel.rfind("RGBA8", 0) != 0) rc = smokeFail("rgba8 label");
        else if (!d.ownsGlTexture)      rc = smokeFail("rgba8 should be owned");
        else if (glGetError() != GL_NO_ERROR) rc = smokeFail("rgba8 glGetError");
        if (d.ownsGlTexture && d.glTexture) { GLuint t = d.glTexture; glDeleteTextures(1, &t); }
    }

    // BC7 path (skip cleanly if no BPTC)
    if (rc == 0) {
        if (!GLEW_ARB_texture_compression_bptc) {
            std::printf("[smoke] SKIP bc7: no BPTC\n");
        } else {
            DecodedTexture d = reg.load((fs::path(dir) / "tex_bc7.ktx2").string());
            if (!d.error.empty())       rc = smokeFail("bc7 upload error");
            else if (d.glTexture == 0)  rc = smokeFail("bc7 no texture");
            else if (!d.isCompressed)   rc = smokeFail("bc7 should be compressed");
            else if (glGetError() != GL_NO_ERROR) rc = smokeFail("bc7 glGetError");
            if (d.ownsGlTexture && d.glTexture) { GLuint t = d.glTexture; glDeleteTextures(1, &t); }
        }
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS ktx upload\n");
    return rc;
}

int AssetViewerApp::runSmokePreview(const char* dir)
{
    namespace fs = std::filesystem;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke-preview", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError();

    int rc = 0;
    {
        UiEditorImageCache_Initialize();
        TexturePreview2D surface;

        surface.setSource((fs::path(dir) / "tex_rgba8.ktx2").string());
        if (surface.hasError())                                  rc = smokeFail("ktx2 preview load");
        else if (surface.metadata().formatLabel.rfind("RGBA8", 0) != 0) rc = smokeFail("ktx2 format label");

        // Replace with a PNG (legacy, not owned) -> must free the prior KTX texture, no crash/leak-error.
        if (rc == 0) surface.setSource((fs::path(dir) / "test_rgba.png").string());
        if (rc == 0 && surface.hasError())                      rc = smokeFail("png after ktx2");

        // Replace with another KTX2 -> exercises owned->owned replacement path.
        if (rc == 0) surface.setSource((fs::path(dir) / "tex_rgba8.ktx2").string());
        if (rc == 0 && surface.hasError())                      rc = smokeFail("ktx2 after png");

        if (rc == 0 && glGetError() != GL_NO_ERROR)             rc = smokeFail("preview glGetError");
        UiEditorImageCache_Shutdown();
    }   // ~TexturePreview2D here must free its owned KTX texture without error

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS preview ownership+metadata\n");
    return rc;
}

int AssetViewerApp::runSmokeTiers(const char* dir)
{
    namespace fs = std::filesystem;
    auto p = [&](const char* rel){ return (fs::path(dir) / rel).make_preferred().string(); };

    // Select sample.ktx2 in the 128 tier (selectFile sets folder = parent).
    FileBrowser fb;
    fb.selectFile(p("tiers/128/sample.ktx2"));
    if (fb.CurrentTier() != "128")            return smokeFail("CurrentTier should be 128");
    auto tiers = fb.SiblingTiers();
    if (tiers.size() != 2 || tiers[0] != "128" || tiers[1] != "256")
        return smokeFail("SiblingTiers should be {128,256}");

    // Switch to 256: same filename exists -> selection preserved, now under tiers/256.
    fb.SwitchTier("256");
    if (fb.CurrentTier() != "256")            return smokeFail("CurrentTier should be 256 after switch");
    if (!fb.hasSelection())                   return smokeFail("selection should persist (sample exists in 256)");
    if (fb.selectionPath().find("256") == std::string::npos ||
        fb.selectionPath().find("sample.ktx2") == std::string::npos)
        return smokeFail("selectionPath should point at tiers/256/sample.ktx2");

    // Missing tier -> no-op (folder unchanged).
    fb.SwitchTier("999");
    if (fb.CurrentTier() != "256")            return smokeFail("missing tier should be a no-op");

    // Switch to a tier lacking the selected file -> folder switches, no selection.
    fb.selectFile(p("tiers/128/only128.ktx2"));
    fb.SwitchTier("256");
    if (fb.CurrentTier() != "256")            return smokeFail("should switch folder even when file absent");
    if (fb.hasSelection())                    return smokeFail("selection should drop when file absent in new tier");

    std::printf("[smoke] PASS tiers (detect/switch/continuity)\n");
    return 0;
}

int AssetViewerApp::runSmokeSphere()
{
    // CPU-only: no GL context required.
    SphereMesh m;
    m.generate(1.0f, 32, 64);
    const auto& v = m.vertices();
    const auto& idx = m.indices();
    if (v.empty() || idx.empty()) { printf("[smoke] FAIL: empty mesh\n"); return 1; }
    if (idx.size() % 3 != 0)      { printf("[smoke] FAIL: index count not triangulated\n"); return 1; }

    for (const auto& sv : v) {
        // position is on the unit sphere
        float pr = std::sqrt(sv.px*sv.px + sv.py*sv.py + sv.pz*sv.pz);
        if (std::fabs(pr - 1.0f) > 1e-3f) { printf("[smoke] FAIL: vertex off sphere (r=%f)\n", pr); return 1; }
        // normal is unit length and equals the position direction (unit sphere)
        float nl = std::sqrt(sv.nx*sv.nx + sv.ny*sv.ny + sv.nz*sv.nz);
        if (std::fabs(nl - 1.0f) > 1e-3f) { printf("[smoke] FAIL: non-unit normal\n"); return 1; }
        // tangent is unit length, perpendicular to the normal, handedness is +/-1
        float tl = std::sqrt(sv.tx*sv.tx + sv.ty*sv.ty + sv.tz*sv.tz);
        if (std::fabs(tl - 1.0f) > 1e-3f) { printf("[smoke] FAIL: non-unit tangent\n"); return 1; }
        float ndott = sv.nx*sv.tx + sv.ny*sv.ty + sv.nz*sv.tz;
        if (std::fabs(ndott) > 1e-2f) { printf("[smoke] FAIL: tangent not perpendicular to normal (%f)\n", ndott); return 1; }
        if (std::fabs(std::fabs(sv.tw) - 1.0f) > 1e-3f) { printf("[smoke] FAIL: handedness not +/-1\n"); return 1; }
    }
    printf("[smoke] PASS sphere verts=%zu tris=%zu\n", v.size(), idx.size()/3);
    return 0;
}

int AssetViewerApp::runSmokeBackend()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-backend", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context (need GL 3.3)"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError(); // consume glew's spurious error

    LocalPbrMaterialBackend b;
    bool ok = b.init();
    GLenum e = glGetError();
    int rc = 0;
    if (!ok)             rc = smokeFail("backend init/compile failed");
    else if (e != GL_NO_ERROR) rc = smokeFail("glGetError non-zero after init");
    else {
        b.shutdown();
        std::printf("[smoke] PASS backend=%s approximate=%d\n", b.name(), (int)b.isApproximate());
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

int AssetViewerApp::runSmokeFit()
{
    auto approx = [](float a, float b){ return std::fabs(a - b) < 0.01f; };

    // Same aspect, different native res, same avail+zoom -> SAME display size (the caveat).
    FitSize a = FitTextureDisplaySize(128, 128, 400.0f, 400.0f, 1.0f);
    FitSize b = FitTextureDisplaySize(256, 256, 400.0f, 400.0f, 1.0f);
    FitSize c = FitTextureDisplaySize(512, 512, 400.0f, 400.0f, 1.0f);
    if (!approx(a.w, b.w) || !approx(a.h, b.h)) return smokeFail("128 vs 256 differ in display size");
    if (!approx(a.w, c.w) || !approx(a.h, c.h)) return smokeFail("128 vs 512 differ in display size");

    // zoom 1 fits inside the avail area (square -> 400x400).
    if (a.w > 400.01f || a.h > 400.01f || a.w < 1.0f) return smokeFail("fit overflows or empty");

    // Aspect preserved for non-square (256x128 -> w == 2*h, fits).
    FitSize r = FitTextureDisplaySize(256, 128, 400.0f, 400.0f, 1.0f);
    if (!approx(r.w, 2.0f * r.h)) return smokeFail("aspect not preserved");
    if (r.w > 400.01f || r.h > 400.01f) return smokeFail("non-square overflows");

    // zoom 2 == exactly 2x zoom 1.
    FitSize z = FitTextureDisplaySize(128, 128, 400.0f, 400.0f, 2.0f);
    if (!approx(z.w, 2.0f * a.w) || !approx(z.h, 2.0f * a.h)) return smokeFail("zoom not linear");

    // Degenerate inputs: finite, no divide-by-zero.
    FitSize d0 = FitTextureDisplaySize(0, 0, 400.0f, 400.0f, 1.0f);
    if (d0.w != 0.0f || d0.h != 0.0f) return smokeFail("zero-dim texture should yield {0,0}");
    FitSize d1 = FitTextureDisplaySize(128, 128, 0.0f, 0.0f, 1.0f);
    if (!std::isfinite(d1.w) || !std::isfinite(d1.h) || d1.w < 0.0f) return smokeFail("zero-avail not finite");
    FitSize d2 = FitTextureDisplaySize(128, 128, 400.0f, 400.0f, 0.0f);
    if (!std::isfinite(d2.w) || d2.w <= 0.0f) return smokeFail("zero-zoom not handled");

    std::printf("[smoke] PASS fit (size@1=%.1fx%.1f)\n", a.w, a.h);
    return 0;
}
