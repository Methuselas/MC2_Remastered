#ifndef GL_STATE_GUARD_H
#define GL_STATE_GUARD_H
//
// gl_state_guard.h — GlStateGuard slice 1.
//
// A minimal RAII primitive for scoped OpenGL state ownership. The engine's
// passes historically inherit GL state implicitly and hand-roll save/restore
// (glGetIntegeri_v -> bind -> ... -> rebind-previous). That pattern is correct
// but easy to forget, and forgetting it has shipped real bugs (e.g. the GPU
// cull SSBO slots leaking into later passes -> "water disappears on camera
// move"; see gpu_cull_compute.cpp C1B-SSBO-CLEANUP).
//
// Slice 1 introduces ONE guard — GlScopedSsboBinding — and applies it to ONE
// contained, modern, timer+oracle-covered pass (GPU cull patch dispatch),
// replacing the hand-rolled prevSsbo7 / prevSsbo15 save/restore with no
// behavior change. This is the pattern-proving slice, not a renderer rewrite.
// It deliberately covers only SHADER_STORAGE_BUFFER binding-base slots — the
// states the target pass actually mutates. Depth/blend/cull/VAO/program guards
// are out of scope until a pass that mutates them is wrapped.
//
#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>

namespace mc2gl {

// Captures the buffer bound at SHADER_STORAGE_BUFFER binding-base `slot` on
// construction and restores it on destruction. Restore-previous semantics:
// whatever was bound before the guarded region is rebound after it. A rebind
// to the same value (slot never changed) is a harmless no-op.
//
// Leak visibility (opt-in, default-off): set MC2_GLSTATEGUARD_LOG=1 to print a
// one-line note whenever the guarded region actually changed the slot (i.e. the
// restore is load-bearing). Sampled once at process start; zero cost when off.
class GlScopedSsboBinding {
public:
    explicit GlScopedSsboBinding(GLuint slot) : slot_(slot) {
        GLint prev = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, slot_, &prev);
        prev_ = static_cast<GLuint>(prev);
    }

    ~GlScopedSsboBinding() {
        if (logEnabled()) {
            GLint cur = 0;
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, slot_, &cur);
            if (static_cast<GLuint>(cur) != prev_) {
                std::printf("[GLSTATEGUARD v1] ssbo slot=%u restored %u->%u\n",
                            slot_, static_cast<GLuint>(cur), prev_);
                std::fflush(stdout);
            }
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot_, prev_);
    }

    GlScopedSsboBinding(const GlScopedSsboBinding&) = delete;
    GlScopedSsboBinding& operator=(const GlScopedSsboBinding&) = delete;
    GlScopedSsboBinding(GlScopedSsboBinding&&) = delete;
    GlScopedSsboBinding& operator=(GlScopedSsboBinding&&) = delete;

private:
    static bool logEnabled() {
        static const bool on = (std::getenv("MC2_GLSTATEGUARD_LOG") != nullptr);
        return on;
    }

    GLuint slot_;
    GLuint prev_ = 0;
};

// --- Slice 2: composable render-state guards -------------------------------
//
// Slice 1 proved the RAII pattern on SSBO binding (a compute pass). Slice 2
// introduces the first RENDER-state vocabulary so a draw pass can OWN the
// depth/blend/cull state it depends on instead of inheriting it (the class of
// bug behind the terrain transparency / depth-mask saga: a prior pass left
// glDepthMask FALSE -> opaque terrain wrote color but no depth -> see-through).
// Each guard captures the previous value in its ctor and restores it in its
// dtor (restore-previous, exactly matching the hand-rolled save/restore it
// replaces). Guards are non-copyable/movable; declare them at the point the
// state is set and let scope exit restore. NOTE: if the guarded function ends
// with gos_InvalidateRenderStateCache(), the guard MUST be block-scoped to
// close BEFORE that call (GameOS re-reads raw GL there) — terrain has no such
// call, so function scope is correct for it.

// Enable or disable a GL capability for the scope; restore its prior
// enabled/disabled state on destruction.
class GlScopedCapability {
public:
    GlScopedCapability(GLenum cap, bool enable) : cap_(cap) {
        prev_ = glIsEnabled(cap_);
        if (enable) glEnable(cap_); else glDisable(cap_);
    }
    ~GlScopedCapability() {
        if (prev_) glEnable(cap_); else glDisable(cap_);
    }
    GlScopedCapability(const GlScopedCapability&) = delete;
    GlScopedCapability& operator=(const GlScopedCapability&) = delete;
    GlScopedCapability(GlScopedCapability&&) = delete;
    GlScopedCapability& operator=(GlScopedCapability&&) = delete;
private:
    GLenum    cap_;
    GLboolean prev_ = GL_FALSE;
};

// Set the depth write-mask and depth-func for the scope; restore both on
// destruction. (Depth-test enable is a capability -> use GlScopedCapability.)
class GlScopedDepthState {
public:
    GlScopedDepthState(GLboolean mask, GLenum func) {
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevMask_);
        GLint f = GL_LESS;
        glGetIntegerv(GL_DEPTH_FUNC, &f);
        prevFunc_ = static_cast<GLenum>(f);
        glDepthMask(mask);
        glDepthFunc(func);
    }
    ~GlScopedDepthState() {
        glDepthMask(prevMask_);
        glDepthFunc(prevFunc_);
    }
    GlScopedDepthState(const GlScopedDepthState&) = delete;
    GlScopedDepthState& operator=(const GlScopedDepthState&) = delete;
    GlScopedDepthState(GlScopedDepthState&&) = delete;
    GlScopedDepthState& operator=(GlScopedDepthState&&) = delete;
private:
    GLboolean prevMask_ = GL_TRUE;
    GLenum    prevFunc_ = GL_LESS;
};

}  // namespace mc2gl

#endif  // GL_STATE_GUARD_H
