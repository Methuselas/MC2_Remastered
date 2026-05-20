// gos_rdoc_capture.cpp - Tier 5 in-process RenderDoc capture hook.
// See gos_rdoc_capture.h for the contract.
#include "gos_rdoc_capture.h"
#include "gos_terrain_indirect.h"  // WasEverFrameSolidArmed()

#include "../../3rdparty/renderdoc/renderdoc_app.h"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace RdocCapture {

namespace {

enum class Phase { Waiting, Armed, Triggered, Done };

struct State {
    bool                  readEnv             = false;
    bool                  enabled             = false;
    bool                  apiResolved         = false;
    RENDERDOC_API_1_5_0*  api                 = nullptr;
    int                   captureFrame        = -1;  // frames after intro-complete
    bool                  exitAfter           = false;
    int                   localFrame          = 0;
    int                   introCompleteFrame  = -1;
    bool                  introObserved       = false;
    Phase                 phase               = Phase::Waiting;
    int                   triggeredAt         = -1;
};
State s_state;

int envInt(const char* name, int dflt) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return dflt;
    return std::atoi(v);
}

void readEnvOnce() {
    if (s_state.readEnv) return;
    s_state.readEnv = true;
    const char* f = std::getenv("MC2_RDC_CAPTURE_FRAME");
    if (!f || !f[0]) { s_state.enabled = false; return; }
    int n = std::atoi(f);
    if (n < 0) { s_state.enabled = false; return; }
    s_state.enabled      = true;
    s_state.captureFrame = n;
    s_state.exitAfter    = (std::getenv("MC2_RDC_EXIT_AFTER") != nullptr);
}

void resolveApiOnce() {
    if (s_state.apiResolved) return;
    s_state.apiResolved = true;

#if defined(_WIN32)
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod) {
        // Fallback: try to load by name; succeeds if renderdoc.dll is on PATH.
        mod = LoadLibraryA("renderdoc.dll");
    }
    if (!mod) {
        std::fprintf(stderr,
            "[RDOC v1] event=api_unavailable reason=renderdoc_dll_not_found "
            "hint=launch_under_renderdoccmd_capture\n");
        std::fflush(stderr);
        return;
    }
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(mod, "RENDERDOC_GetAPI"));
#else
    void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (!mod) mod = dlopen("librenderdoc.so", RTLD_NOW);
    if (!mod) {
        std::fprintf(stderr,
            "[RDOC v1] event=api_unavailable reason=librenderdoc_not_found\n");
        std::fflush(stderr);
        return;
    }
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(
        dlsym(mod, "RENDERDOC_GetAPI"));
#endif
    if (!get_api) {
        std::fprintf(stderr,
            "[RDOC v1] event=api_unavailable reason=GetAPI_missing\n");
        std::fflush(stderr);
        return;
    }
    void* p = nullptr;
    int ok = get_api(eRENDERDOC_API_Version_1_5_0, &p);
    if (ok != 1 || !p) {
        std::fprintf(stderr,
            "[RDOC v1] event=api_unavailable reason=GetAPI_returned_%d\n", ok);
        std::fflush(stderr);
        return;
    }
    s_state.api = reinterpret_cast<RENDERDOC_API_1_5_0*>(p);

    if (const char* tpl = std::getenv("MC2_RDC_CAPTURE_PATH")) {
        if (tpl[0]) s_state.api->SetCaptureFilePathTemplate(tpl);
    }

    int major = 0, minor = 0, patch = 0;
    s_state.api->GetAPIVersion(&major, &minor, &patch);
    std::fprintf(stderr,
        "[RDOC v1] event=api_resolved version=%d.%d.%d capture_frame=%d "
        "exit_after=%d\n",
        major, minor, patch, s_state.captureFrame, s_state.exitAfter ? 1 : 0);
    std::fflush(stderr);
}

}  // namespace

bool isEnabled() {
    readEnvOnce();
    return s_state.enabled;
}

void onFrameTick() {
    readEnvOnce();
    if (!s_state.enabled) return;
    resolveApiOnce();
    if (!s_state.api) return;

    // Latch on the first frame that the engine reports intro-pan complete,
    // matching VisualDiff semantics so the captured EID layout is comparable.
    if (!s_state.introObserved) {
        if (gos_terrain_indirect::WasEverFrameSolidArmed()) {
            s_state.introObserved      = true;
            s_state.introCompleteFrame = s_state.localFrame;
            s_state.phase              = Phase::Armed;
            std::fprintf(stderr,
                "[RDOC v1] event=intro_armed local_frame=%d\n",
                s_state.localFrame);
            std::fflush(stderr);
        }
        ++s_state.localFrame;
        return;
    }

    const int sinceArm = s_state.localFrame - s_state.introCompleteFrame;

    if (s_state.phase == Phase::Armed && sinceArm == s_state.captureFrame) {
        // TriggerCapture marks the NEXT presented frame for capture. The
        // engine has not yet swapped (this hook is pre-SwapBuffers), so the
        // capture covers the frame whose hook we are inside now.
        s_state.api->TriggerCapture();
        s_state.phase       = Phase::Triggered;
        s_state.triggeredAt = s_state.localFrame;
        std::fprintf(stderr,
            "[RDOC v1] event=trigger_capture local_frame=%d since_arm=%d\n",
            s_state.localFrame, sinceArm);
        std::fflush(stderr);
    } else if (s_state.phase == Phase::Triggered &&
               s_state.localFrame >= s_state.triggeredAt + 2) {
        // Give RenderDoc one full present cycle to finalize the .rdc before
        // we exit. Two frames is conservative; one is the captured present,
        // the next ensures EndFrameCapture-equivalent state is flushed.
        std::fprintf(stderr,
            "[RDOC v1] event=capture_complete local_frame=%d\n",
            s_state.localFrame);
        std::fflush(stderr);
        s_state.phase = Phase::Done;
        if (s_state.exitAfter) {
            std::exit(0);
        }
    } else if (s_state.phase == Phase::Armed &&
               sinceArm > s_state.captureFrame + envInt("MC2_RDC_MAX_WAIT", 600)) {
        std::fprintf(stderr,
            "[RDOC v1] event=capture_timeout local_frame=%d\n",
            s_state.localFrame);
        std::fflush(stderr);
        s_state.phase = Phase::Done;
        if (s_state.exitAfter) std::exit(4);
    }

    ++s_state.localFrame;
}

}  // namespace RdocCapture
