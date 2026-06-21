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

// Save/restore glClipControl (clip origin + depth-range mode). ARB_clip_control
// is required at engine startup (gameosmain.cpp); this guard is safe to
// instantiate whenever GL is live. Use at the boundary of any pass that could
// change clip semantics, or defensively in endXxxPass() to re-assert the scene
// expectation (GL_LOWER_LEFT + GL_ZERO_TO_ONE) if an NVIDIA driver reset occurs
// on FBO switch. Not usable across function call pairs — use manual member-var
// save/restore instead (see gosPostProcess::shadowSaved* pattern).
class GlScopedClipControl {
public:
    GlScopedClipControl() {
        GLint origin = GL_LOWER_LEFT;
        GLint depth  = GL_NEGATIVE_ONE_TO_ONE;
        glGetIntegerv(GL_CLIP_ORIGIN,     &origin);
        glGetIntegerv(GL_CLIP_DEPTH_MODE, &depth);
        prevOrigin_ = static_cast<GLenum>(origin);
        prevDepth_  = static_cast<GLenum>(depth);
    }
    ~GlScopedClipControl() {
        glClipControl(prevOrigin_, prevDepth_);
    }
    GlScopedClipControl(const GlScopedClipControl&) = delete;
    GlScopedClipControl& operator=(const GlScopedClipControl&) = delete;
    GlScopedClipControl(GlScopedClipControl&&) = delete;
    GlScopedClipControl& operator=(GlScopedClipControl&&) = delete;
private:
    GLenum prevOrigin_ = GL_LOWER_LEFT;
    GLenum prevDepth_  = GL_ZERO_TO_ONE;
};

// --- Slice 3: texture-unit save/restore ------------------------------------
//
// Slice 2 covered depth/blend/cull states. Slice 3 adds the texture-unit
// leak class: legacy decal/overlay paths call glActiveTexture(GL_TEXTURE0)
// and glBindTexture(GL_TEXTURE_2D, ...) inside their draw loops without
// restoring the previous active unit or the previous unit-0 binding.
// gos_InvalidateRenderStateCache() does NOT track texture unit state, so
// after one of these paths the engine cache believes unit 0 is bound to
// whatever applyRenderStates() last set, while GL actually holds the
// decal/overlay texture — exactly the vendor-visibility gap (AMD tolerates
// it; NVIDIA exposes wrong textures or intermittent corruption).
//
// Usage: declare at the point where the raw binds begin; the guard captures
// the current active-texture unit and the GL_TEXTURE_2D binding on `unit`
// (default 0), then restores both on destruction.  If the enclosing function
// ends with gos_InvalidateRenderStateCache(), use a block scope so the guard
// closes (restores) BEFORE that call — the cache-invalidation path re-reads
// raw GL state and must see the restored binding, not the mutated one.
class GlScopedTextureUnit {
public:
    explicit GlScopedTextureUnit(GLuint unit = 0) : unit_(unit) {
        GLint active = static_cast<GLint>(GL_TEXTURE0);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        prevActiveTex_ = static_cast<GLenum>(active);
        glActiveTexture(GL_TEXTURE0 + unit_);
        GLint binding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        prevBinding_ = static_cast<GLuint>(binding);
    }
    ~GlScopedTextureUnit() {
        glActiveTexture(GL_TEXTURE0 + unit_);
        glBindTexture(GL_TEXTURE_2D, prevBinding_);
        glActiveTexture(prevActiveTex_);
    }
    GlScopedTextureUnit(const GlScopedTextureUnit&) = delete;
    GlScopedTextureUnit& operator=(const GlScopedTextureUnit&) = delete;
    GlScopedTextureUnit(GlScopedTextureUnit&&) = delete;
    GlScopedTextureUnit& operator=(GlScopedTextureUnit&&) = delete;
private:
    GLuint unit_;
    GLenum prevActiveTex_ = GL_TEXTURE0;
    GLuint prevBinding_   = 0;
};

}  // namespace mc2gl

#endif  // GL_STATE_GUARD_H
