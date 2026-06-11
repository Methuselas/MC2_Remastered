/***************************************************************
 * FILENAME: main.cpp
 * DESCRIPTION: Entry point for mc2_asset_viewer (stage 1).
 ***************************************************************/
#include <cstdio>
#include <cstring>
#include <SDL.h>
#include <GL/glew.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#include "AssetViewerApp.h"

int main(int argc, char* argv[])
{
    // --smoke <fixtureDir> dispatch (Task 8 smoke test hook)
    if (argc >= 2 && strcmp(argv[1], "--smoke") == 0)
    {
        const char* fixtureDir = (argc >= 3) ? argv[2] : ".";
        return AssetViewerApp::runSmoke(fixtureDir);
    }

    if (argc >= 2 && strcmp(argv[1], "--smoke-decoder") == 0)
        return AssetViewerApp::runSmokeDecoder();

    if (argc >= 2 && strcmp(argv[1], "--smoke-ktx-parse") == 0)
        return AssetViewerApp::runSmokeKtxParse(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-ktx") == 0)
        return AssetViewerApp::runSmokeKtx(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-preview") == 0)
        return AssetViewerApp::runSmokePreview(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-fit") == 0)
        return AssetViewerApp::runSmokeFit();

    if (argc >= 2 && strcmp(argv[1], "--smoke-tiers") == 0)
        return AssetViewerApp::runSmokeTiers(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-sphere") == 0)
        return AssetViewerApp::runSmokeSphere();

    if (argc >= 2 && strcmp(argv[1], "--smoke-backend") == 0)
        return AssetViewerApp::runSmokeBackend();

    if (argc >= 2 && strcmp(argv[1], "--smoke-texload") == 0)
        return AssetViewerApp::runSmokeTexLoad(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-render") == 0)
        return AssetViewerApp::runSmokeRender(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-tangent") == 0)
        return AssetViewerApp::runSmokeTangent(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-fit-material") == 0)
        return AssetViewerApp::runSmokeFitMaterial(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-fit-load") == 0)
        return AssetViewerApp::runSmokeFitLoad(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-tgl-load") == 0)
        return AssetViewerApp::runSmokeTglLoad(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-mesh-build") == 0)
        return AssetViewerApp::runSmokeMeshBuild(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-mesh-render") == 0)
        return AssetViewerApp::runSmokeMeshRender(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-mesh-orient") == 0)
        return AssetViewerApp::runSmokeMeshOrient(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-spotlight") == 0)
        return AssetViewerApp::runSmokeSpotlight(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-link") == 0)
        return AssetViewerApp::runSmokeWorkbenchLink();

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-glb") == 0)
        return AssetViewerApp::runSmokeWorkbenchGlb(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-bind") == 0)
        return AssetViewerApp::runSmokeWorkbenchBind(argc >= 3 ? argv[2] : ".", argc >= 4 ? argv[3] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-validate") == 0)
        return AssetViewerApp::runSmokeWorkbenchValidate(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-export") == 0)
        return AssetViewerApp::runSmokeWorkbenchExport(
            argc >= 3 ? argv[2] : ".",
            argc >= 4 ? argv[3] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-reload") == 0)
        return AssetViewerApp::runSmokeWorkbenchReload(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-appearance-roster") == 0)
        return AssetViewerApp::runSmokeAppearanceRoster(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-texture-missing-warn") == 0)
        return AssetViewerApp::runSmokeTextureMissingWarn(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-lod-edit-validate") == 0)
        return AssetViewerApp::runSmokeLodEditValidate();

    if (argc >= 2 && strcmp(argv[1], "--smoke-central-merge-preserve") == 0)
        return AssetViewerApp::runSmokeCentralMergePreserve(
            argc >= 3 ? argv[2] : ".", argc >= 4 ? argv[3] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-shader-include") == 0)
        return AssetViewerApp::runSmokeShaderInclude(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-backend-a-compile") == 0)
        return AssetViewerApp::runSmokeBackendACompile(argc >= 3 ? argv[2] : "shaders");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        printf("SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);  // PBR shaders use #version 330
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(
        "MC2 Asset Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window)
    {
        printf("SDL_CreateWindow error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context)
    {
        printf("SDL_GL_CreateContext error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    glewExperimental = GL_TRUE;
    GLenum glew_err = glewInit();
    if (glew_err != GLEW_OK)
    {
        printf("glewInit error: %s\n", glewGetErrorString(glew_err));
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // Consume any spurious GL error that glewInit may have generated
    glGetError();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    {
        AssetViewerApp app;

        bool running = true;
        while (running)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT)
                    running = false;
                if (event.type == SDL_WINDOWEVENT &&
                    event.window.event == SDL_WINDOWEVENT_CLOSE &&
                    event.window.windowID == SDL_GetWindowID(window))
                    running = false;
                if (event.type == SDL_DROPFILE){ app.onFileDropped(event.drop.file); SDL_free(event.drop.file); }
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();

            app.drawUi();

            ImGui::Render();

            int display_w, display_h;
            SDL_GL_GetDrawableSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClearColor(0.06f, 0.065f, 0.075f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            SDL_GL_SwapWindow(window);
        }
    }   // app destroyed here — GL context still current, glDeleteTextures is valid

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
