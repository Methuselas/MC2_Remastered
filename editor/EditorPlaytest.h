//-------------------------------------------------------------------------------------------------
// EditorPlaytest.h -- "Playtest in Game" controller for the MC2 Mission Editor (Slice 1).
//
// One-click loop: save the current mission -> launch mc2.exe as a child process with
// `-mission <pak>` -> capture combined stdout/stderr + exit code (via EditorTaskRunner)
// -> archive the captured log next to the mission.  Editor-only; no game/engine edits.
//
// THREADING: launches via EditorTaskRunner (worker thread + pipe).  All state mutation
// and the log-archive callback run on the MAIN (editor) thread, driven by
// EditorTaskRunner::PumpMainThread().  Do NOT call any method here off the main thread.
//
// DESIGN COMPROMISE (Slice 1): EditorData::save() mutates editor path state (calls
// setMapName + writes .old backups) -- a non-mutating "shadow save" to .playtest/
// would require a >50-line EditorData refactor, exceeding the slice budget.  Per the
// plan's STOP-DOWNGRADE rule we save IN PLACE (normal save) and launch that pak.
//-------------------------------------------------------------------------------------------------
#ifndef EDITOR_PLAYTEST_H
#define EDITOR_PLAYTEST_H

#include <string>

namespace EditorPlaytest
{
	enum class State { Idle, Running };

	// True when a mission is loaded/saved-able AND no playtest is currently running.
	bool CanPlaytest();

	// True while a launched playtest child process is still alive.
	bool IsRunning();

	// True when no playtest child is running (the inverse of IsRunning()).
	// The headless `-playtest` CLI smoke polls this after Start() to detect
	// completion (the child exited and OnFinished/OnCancelled has run).
	bool IsIdle();

	// Exit code of the most recently completed playtest child, or -1 if none has
	// completed yet (or the launch was cancelled/never started). Valid once
	// IsIdle() is true after a Start(). Set on the main thread by OnFinished.
	int LastExitCode();

	// Archived log path of the most recently completed playtest child ("" if the
	// archive write failed or no run has completed). Valid once IsIdle() is true.
	const char* LastLogPath();

	// Detected mod id for the most recent run, or "" when the mission is not in a
	// mod. Valid once Start() has run.
	const char* LastModId();

	// SMOKE CHILD ENV (headless `-playtest` CLI only -- interactive playtest is
	// unchanged). When enabled, the next Start() appends gos_smoke's argv
	// (`--mission <stem> --duration <sec>`) AND sets MC2_SMOKE_MODE=1 in the
	// child env so the launched game auto-quits cleanly after <sec> seconds
	// instead of running forever. `seconds` clamps to [1, 600]; default 30.
	void SetSmokeChildEnv(bool enabled, int seconds);

	// Begin a playtest: save mission in place, resolve game exe, launch child.
	// On any pre-launch failure (no mission, exe missing) sets an error status and
	// returns without launching.  Safe to call only when CanPlaytest() is true.
	void Start();

	// Terminate the running playtest child (TerminateProcess via EditorTaskRunner).
	// No-op when idle.
	void Stop();

	// One-line status for the toolbar UI (state, exit code, log path, last line).
	const char* StatusLine();

	// Resolved/overridable game exe path (env MC2_PLAYTEST_EXE or probe fallback).
	// Returned buffer is editable in place by the UI (fixed-size).
	char* GameExePathBuffer();
	int   GameExePathBufferSize();
}

#endif // EDITOR_PLAYTEST_H
