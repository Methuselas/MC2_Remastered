#pragma once

struct RenderSnapshot;

namespace mc2_debug_state {

void maybeWriteRenderState(const RenderSnapshot& snap);
void writeShutdownState();          // "shutdown" dump_kind; uses cached last snapshot
const char* getSessionId();         // stable for process lifetime; shared with diagnostic_trace

} // namespace mc2_debug_state
