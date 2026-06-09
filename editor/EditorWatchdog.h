#pragma once
//============================================================================
// EditorWatchdog — TEMPORARY DIAGNOSTIC.
//
// Catches main-thread stalls that frame timers cannot see (the thread blocks
// fully — disk read, texture decode/upload, driver residency, paging — and
// never reaches a render tick or message pump). A watchdog thread suspends the
// stalled main thread, walks its stack, and logs it to editor-startup.log.
//
// Enable: env MC2_EDITOR_WATCHDOG=1. Zero effect when unset.
//
// Wiring:
//   * Call EditorWatchdog_Heartbeat() once per render tick (top of
//     RunGameOSLogic). First call records the main thread + starts the watcher.
//============================================================================

// Update the main-thread liveness timestamp. First call initializes (records
// the calling thread as "main" and starts the watcher if the env var is set).
void EditorWatchdog_Heartbeat();
