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

}  // namespace mc2gl

#endif  // GL_STATE_GUARD_H
