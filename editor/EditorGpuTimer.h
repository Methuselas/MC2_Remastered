#pragma once
//============================================================================
// EditorGpuTimer — per-pass GPU timing for the editor render chain.
//
// Diagnostic for the "slow editor pan on oversized (1k) maps" investigation:
// frames were ~600ms with the CPU idle (~37%) and NOTHING in Tracy's CPU
// zones — i.e. GPU-bound, blocking at SwapBuffers. Tracy's CPU timeline can't
// see it. These GL_TIMESTAMP queries attribute GPU wall-time to each pass of
// EditorCamera::render so we can name the hot pass without RGP.
//
// Enable: set env MC2_EDITOR_GPU_TIMERS=1 before launching the editor.
// Output: one "[EDGPU f=N] sky=.. terrain=.. ... TOTAL=.." line per frame to
// stdout (editor-startup.log). Zero cost when the env var is unset.
//
// Implementation: ping-pong double-buffered timestamp queries. Each frame
// writes into the active buffer and reads back the PREVIOUS frame's buffer
// (a full frame + swap has elapsed, so glGetQueryObjectui64v never stalls).
//============================================================================

// Call once at the very start of the timed render section. Prints the previous
// frame's per-pass deltas, then issues the baseline ("start") timestamp.
void EditorGpuTimer_Begin();

// Call at each pass boundary. `name` labels the segment that ran since the
// previous mark (must be a stable string literal; same call order every frame).
void EditorGpuTimer_Mark(const char* name);

// Call once after the last pass. Flips the ping-pong buffer.
void EditorGpuTimer_End();

//----------------------------------------------------------------------------
// CPU-only phase timer (no GL) for the PRE-render work in Editor::update
// (land->update / land->geometry / etc.). Emits "[EDPRE f=N] ..." lines.
// Same env gate (MC2_EDITOR_GPU_TIMERS=1). Use independently of the GPU timer.
//----------------------------------------------------------------------------
void EditorCpuPhase_Begin();
void EditorCpuPhase_Mark(const char* name);
void EditorCpuPhase_End();

//----------------------------------------------------------------------------
// Whole-frame phase timer (CPU wall-clock) for RunGameOSLogic: partitions the
// full frame period (BeginFrame/DoGameLogic/UpdateRenderers/EndFrame/endScene/
// gui/swap) so we can find time spent OUTSIDE the render passes (e.g. a
// SwapBuffers GPU-wait stall). Emits "[EDFRM f=N dt=..] ..." lines.
//----------------------------------------------------------------------------
void EditorFramePhase_Begin();
void EditorFramePhase_Mark(const char* name);
void EditorFramePhase_End();
