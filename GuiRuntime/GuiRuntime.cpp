#include "GuiRuntime.h"
#include "EditorInspector.h"
#include "GraphicsOptionsWindow.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <cstdlib>   // getenv
#include <cstdio>    // fprintf

#include <GL/glew.h>  // glGetError, glGetIntegerv for Render() diagnostics

#include "../GameOS/gameos/gos_render.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#endif

bool g_imguiInitialized = false;

static bool isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_IMGUI");
        cached = (!v || v[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

void GuiRuntime::Init() {
    if (!isEnabled()) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(graphics::getSDLWindow(), graphics::getSDLGLContext());
    ImGui_ImplOpenGL3_Init("#version 430");

    g_imguiInitialized = true;
}

void GuiRuntime::Shutdown() {
    if (!g_imguiInitialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_imguiInitialized = false;
}

void GuiRuntime::NewFrame() {
    if (!g_imguiInitialized) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    // SDL_GL_GetDrawableSize returns 0 for SDL_CreateWindowFrom windows that
    // have no SDL GL context (editor uses WGL, not SDL_GL_CreateContext).
    // ImGui_ImplSDL2_NewFrame sets io.DisplayFramebufferScale = drawable/window,
    // which becomes (0,0) -> ImGui_ImplOpenGL3_RenderDrawData computes fb_width=0
    // and early-returns without issuing any draw calls.  Force scale=1 to fix.
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

#ifdef PLATFORM_WINDOWS
    // Win32 input polling
    // The GL child HWND uses a custom WndProc (MC2EditorGLChild) that SDL does
    // not subclass, so SDL never generates SDL_Events for mouse/keyboard input.
    // ImGui_ImplSDL2 therefore cannot feed the ImGui IO queue automatically.
    //
    // Solution: poll Win32 state here, directly before ImGui::NewFrame(), so
    // that when NewFrame runs it has accurate mouse position and can set
    // WantCaptureMouse correctly for this frame's message dispatch.
    {
        ImGuiIO& io = ImGui::GetIO();

        // Mouse position: convert cursor screen coords to GL child HWND client coords.
        // Those client coords ARE the ImGui display coords (1:1 mapping).
        SDL_Window* sdlWin = graphics::getSDLWindow();
        if (sdlWin) {
            SDL_SysWMinfo wmInfo;
            SDL_VERSION(&wmInfo.version);
            if (SDL_GetWindowWMInfo(sdlWin, &wmInfo)) {
                HWND glHwnd = wmInfo.info.win.window;
                POINT cursorPt;
                if (::GetCursorPos(&cursorPt) && ::IsWindow(glHwnd)) {
                    ::ScreenToClient(glHwnd, &cursorPt);
                    io.AddMousePosEvent((float)cursorPt.x, (float)cursorPt.y);
                }
            }
        }

        // Mouse buttons: NOT polled here via GetAsyncKeyState.
        //
        // Polling buttons every frame sets io.MouseDown[n]=true while any button
        // is physically held, which makes ImGui's internal mouse_any_down=true and
        // therefore forces WantCaptureMouse=true for ALL clicks -- including viewport
        // clicks that must reach the GameOS input system for inspector picks etc.
        //
        // Button injection is handled selectively in ForwardMouseToEditor:
        // only when WantCaptureMouse is already true (mouse hovering an ImGui window)
        // are events injected and the WM_BUTTON* message consumed.  Viewport clicks
        // fall through to the MFC/GameOS path unchanged.

        // Modifier keys: inject per-frame from Win32.
        // ImGui deduplicates state-unchanged key events, so unconditional calls
        // per frame are fine and match what the official SDL/Win32 backends do.
        io.AddKeyEvent(ImGuiMod_Ctrl,  (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Alt,   (::GetAsyncKeyState(VK_MENU)    & 0x8000) != 0);

        // Individual keys (edge-detect):
        // F  -> demo window toggle (via ImGui::IsKeyPressed in Render())
        // G  -> Ctrl+Shift+G opens Graphics Options (ImGui shortcut)
        static bool s_F = false, s_G = false;
        bool F_cur = (::GetAsyncKeyState('F') & 0x8000) != 0;
        bool G_cur = (::GetAsyncKeyState('G') & 0x8000) != 0;
        if (F_cur != s_F) { io.AddKeyEvent(ImGuiKey_F, F_cur); s_F = F_cur; }
        if (G_cur != s_G) { io.AddKeyEvent(ImGuiKey_G, G_cur); s_G = G_cur; }
    }
#endif

    ImGui::NewFrame();
}

void GuiRuntime::Render() {
    if (!g_imguiInitialized) return;

    // F toggles the ImGui demo window.
    static bool s_showDemo = false;
    if (ImGui::IsKeyPressed(ImGuiKey_F, /*repeat=*/false))
        s_showDemo = !s_showDemo;
    if (s_showDemo)
        ImGui::ShowDemoWindow(&s_showDemo);

    EditorInspector::drawImGui();
    GraphicsOptionsWindow::draw();

    // Force scale=1 at Render() time too -- some paths between NewFrame and
    // Render can reset it (SDL window event processing, etc.).
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    ImGui::Render();

    // One-shot diagnostic: dump ImDrawData stats and GL state to stderr.
    // editor-stderr.log captures this via run-editor.bat's 2> redirect.
    {
        static bool s_traced = false;
        if (!s_traced) {
            s_traced = true;
            ImDrawData* dd = ImGui::GetDrawData();
            GLint boundFbo = -1, boundProg = -1;
            GLint vp[4] = {};
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFbo);
            glGetIntegerv(GL_CURRENT_PROGRAM, &boundProg);
            glGetIntegerv(GL_VIEWPORT, vp);
            GLenum err = glGetError();
            if (dd) {
                fprintf(stderr,
                    "[GuiRuntime::Render] frame0 CmdLists=%d TotalVtx=%d TotalIdx=%d "
                    "DisplaySize=(%.0f,%.0f) FbScale=(%.3f,%.3f) "
                    "fbo=%d prog=%d vp=(%d,%d,%d,%d) glErr=0x%X\n",
                    dd->CmdListsCount, dd->TotalVtxCount, dd->TotalIdxCount,
                    dd->DisplaySize.x, dd->DisplaySize.y,
                    dd->FramebufferScale.x, dd->FramebufferScale.y,
                    boundFbo, boundProg,
                    vp[0], vp[1], vp[2], vp[3],
                    (unsigned)err);
            } else {
                fprintf(stderr, "[GuiRuntime::Render] frame0 GetDrawData() returned NULL\n");
            }
            fflush(stderr);
        }
    }

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    {
        static bool s_errTraced = false;
        if (!s_errTraced) {
            s_errTraced = true;
            GLenum err = glGetError();
            if (err != GL_NO_ERROR)
                fprintf(stderr, "[GuiRuntime::Render] glError after RenderDrawData: 0x%X\n", (unsigned)err);
            else
                fprintf(stderr, "[GuiRuntime::Render] RenderDrawData completed, no GL error\n");
            fflush(stderr);
        }
    }

    // Pixel probe (fires on 3rd Render call so terrain is in the buffer)
    {
        static int s_renderCallCount = 0;
        ++s_renderCallCount;
        if (s_renderCallCount == 3) {
            // Backend name: non-null only if ImGui_ImplOpenGL3_Init() succeeded
            const char* backendName = ImGui::GetIO().BackendRendererName;
            // Font texture handle: 0 if CreateDeviceObjects never ran
            ImTextureID fontTexID = ImGui::GetIO().Fonts->TexID;

            // Sample a pixel inside the "MC2 Editor" window title bar.
            // Window Pos=(10,10), Size=(220,60). Title bar centre ~ ImGui(120,19).
            // OpenGL origin is bottom-left, so gl_y = fb_height - 1 - imgui_y.
            ImDrawData* dd = ImGui::GetDrawData();
            int fbh = dd ? (int)(dd->DisplaySize.y * dd->FramebufferScale.y) : 0;
            int fbw = dd ? (int)(dd->DisplaySize.x * dd->FramebufferScale.x) : 0;
            GLubyte imguiPx[4] = {};
            GLubyte terrainPx[4] = {};
            if (fbh > 0 && fbw > 0) {
                int gl_y_imgui  = fbh - 1 - 19;   // ImGui y=19 (title bar)
                int gl_y_terrain = fbh / 2;         // screen centre
                glReadPixels(120,       gl_y_imgui,   1, 1, GL_RGBA, GL_UNSIGNED_BYTE, imguiPx);
                glReadPixels(fbw / 2,   gl_y_terrain, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, terrainPx);
            }

            fprintf(stderr,
                "[GuiRuntime PROBE] BackendRendererName=%s FontTexID=%p "
                "imgui_px(120,19)=RGBA(%d,%d,%d,%d) "
                "terrain_mid_px=RGBA(%d,%d,%d,%d)\n",
                backendName ? backendName : "NULL",
                (void*)(uintptr_t)fontTexID,
                imguiPx[0], imguiPx[1], imguiPx[2], imguiPx[3],
                terrainPx[0], terrainPx[1], terrainPx[2], terrainPx[3]);
            fflush(stderr);
        }
    }
}
