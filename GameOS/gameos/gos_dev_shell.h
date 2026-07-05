// gos_dev_shell.h - DEV-SHELL-1: localhost dev command socket.
// Remote-control spine for live engine iteration (write half of the
// MC2_DEBUG_STATE_DUMP read bridge). Gate: MC2_DEV_SHELL=1, default OFF —
// when unset both hooks are a single static bool check, no socket, no
// behavior change. Localhost-only (127.0.0.1), nonblocking; every command
// executes on the main thread at the poll point (which owns the GL context).
// Protocol: newline-delimited JSON request {"type":cmd,"params":{...}},
// reply {"ok":bool,"error":str|null,"version":1,"data":{...}}.
// Client: tools/dev_shell/mc2_cmd.py. Port: 9877 (MC2_DEV_SHELL_PORT).
#pragma once

#include <stdint.h>

namespace gos_dev_shell {

// Poll the socket and execute any complete pending commands. Called once per
// frame from the gameosmain loop (after gos_RendererHandleEvents). Returns
// true when a "quit" command was received — caller sets g_exit.
bool pollCommands(uint32_t frame);

// Execute a pending screenshot request, if any. Called at the post-render
// capture point (same site as [SCREENSHOT v1] / visual capture — sceneFBO is
// intact just before swap). No-op when nothing pending.
void capturePendingScreenshot(uint32_t frame);

}  // namespace gos_dev_shell
