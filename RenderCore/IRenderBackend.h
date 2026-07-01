// RenderCore/IRenderBackend.h
//
// Minimal, GL-FREE render-backend interface. First boundary slice
// (RENDER-BACKEND-IFACE-POSTPROCESS-1): only the ops the PostProcess composite
// output edge actually needs today. The interface GROWS one edge at a time as
// more render edges route through it — do NOT front-load the full op list here.
//
// HARD RULE: no GL includes, no GL types, no pass logic in this file. It is a
// pure abstract description of backend ops. The concrete GL implementation lives
// in GameOS/gameos/GLBackend.cpp (RenderCore stays GL-free by construction, same
// split as PipelineDesc -> pipeline_binder). A future VulkanBackend implements
// the same interface without any change here.
//
// Gate: MC2_RENDER_BACKEND_IFACE (default-OFF). OFF = the call site issues the
// direct GL it always has. ON = the call site routes the SAME work through this
// interface (GL-backed). Both are GL -> byte-identical output; the gate proves
// the routing seam works before any non-GL backend exists.

#pragma once

namespace RenderCore {

// Abstract render backend. Pure virtual; no state, no GL.
struct IRenderBackend {
    virtual ~IRenderBackend() = default;

    // Bind the window's default framebuffer (the backbuffer) as the current
    // render target for the composite output edge. GL impl == glBindFramebuffer
    // (GL_FRAMEBUFFER, 0). This is the single op the PostProcess composite edge
    // needs in this slice.
    virtual void bindBackbuffer() = 0;

    // Bind an arbitrary framebuffer object as the current render target. GL impl
    // == glBindFramebuffer(GL_FRAMEBUFFER, fbo). `fbo` is a plain uint handle so
    // this file stays GL-free (no GLuint / GL headers). Added by
    // RENDER-BACKEND-IFACE-FBO-1 to route one more PostProcess FBO-bind edge; the
    // interface grows one edge at a time.
    virtual void bindFramebuffer(unsigned int fbo) = 0;
};

}  // namespace RenderCore
