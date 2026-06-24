// render_frame_driver.cpp — GAME-EDITOR-RENDER-FRAME-DRIVER-1 (Slice 6)
//
// See render_frame_driver.h for the full rationale and the inside-vs-outside
// scope contract. This TU lives in the SHARED gameos source list and therefore
// compiles into BOTH the `gameos` (game) and `gameos_editor` (EditRel) static
// libs — the same mechanism gos_render_context.cpp uses (Slice 1). That is what
// makes the seam genuinely shared rather than a game-only helper the editor
// re-copies.
//
// Deliberately NO GPU-backend includes here: the driver only references the
// renderer C entry points (extern "C"-style free functions) and the global
// Environment. Keeping it free of RenderCore/RenderWorld headers preserves the
// editor include firewall (the editor links this lib but must not gain
// forbidden GPU includes through it).

#include "render_frame_driver.h"

// The shared renderer frame entry points. Declared extern (no GPU headers) to
// match how both hosts forward-declare them at their call sites today
// (gameosmain.cpp / EditorGameOS.cpp both `extern void gos_Renderer*`).
extern void gos_RendererBeginFrame();
extern void gos_RendererEndFrame();

// The world/terrain/object render dispatch lives on the global Environment
// (Camera). Both hosts call Environment.UpdateRenderers() inside this bracket.
// We pull in the existing engine declaration rather than re-declaring the
// global, so the type stays in lockstep with the rest of the engine.
#include "gameos.hpp"   // brings in Environment (gos_Environment) + UpdateRenderers()

void RenderFrameDriver_RenderWorld(const RenderFrameDesc& desc)
{
    // INSIDE THE SEAM — the exact begin/UpdateRenderers/end bracket both hosts
    // run today, byte-for-byte. Nothing host-specific belongs here; desc is
    // available for future intra-seam branching but is intentionally not read
    // yet (today the three calls are identical for both hosts, which is the
    // whole point of formalizing them as one seam).
    (void)desc;

    gos_RendererBeginFrame();
    Environment.UpdateRenderers();
    gos_RendererEndFrame();
}
