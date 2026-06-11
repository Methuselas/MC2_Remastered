//-------------------------------------------------------------------------------------------------
// EditorTaskRunner.h -- reusable async task runner for the MC2 Mission Editor (Phase 1).
//
// Runs a long external command (e.g. terrain_gen.py) on a worker thread so the
// MFC/GL editor UI thread never blocks.  The worker captures combined stdout/stderr,
// parses line-oriented "PROGRESS <pct> <stage> <message...>" output, and marshals
// completion back to the main thread.
//
// THREADING CONTRACT (load-bearing -- the editor is single-threaded GL/MFC):
//   * The worker thread ONLY touches this module's own state (under a mutex).
//   * It NEVER calls GL, MFC, or any editor mutation directly.
//   * Result callbacks (onSuccess/onFailure/onCancel) fire ONLY from
//     PumpMainThread(), which the caller must invoke from the editor main thread
//     (EditorInterface::update()).  Apply generated output inside those callbacks.
//
// Self-contained: no ImGui, no editor, no terrain dependency.  RenderImGui() is the
// only ImGui-touching entry point and is compiled only under MC2_IMGUI.
//-------------------------------------------------------------------------------------------------
#ifndef EDITOR_TASK_RUNNER_H
#define EDITOR_TASK_RUNNER_H

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace EditorTaskRunner
{
	using TaskId = uint64_t;
	static const TaskId kInvalidTask = 0;

	enum class Status { Pending, Running, Succeeded, Failed, Cancelled };

	// Snapshot passed to a completion callback (fired on the main thread).
	struct TaskResult
	{
		TaskId      id       = kInvalidTask;
		Status      status   = Status::Pending;
		int         exitCode = -1;
		std::string name;
		std::string log;     // full captured stdout+stderr tail
	};

	struct TaskSpec
	{
		std::string name;             // display label
		std::string commandLine;      // full command (launch python with -u!)
		std::string workingDirectory; // cwd for the child ("" = inherit)
		std::string cancelFile;       // optional: touched on cancel (cooperative kill)

		// Optional environment additions for the child.  When non-empty, the worker
		// builds a FULL ANSI environment block = parent env (GetEnvironmentStrings)
		// with these K=V pairs appended/overriding, and passes it to CreateProcessA.
		// When empty, the child inherits the parent env (env param = NULL), as before.
		std::vector<std::pair<std::string, std::string>> envExtra;

		// All three run on the MAIN thread, drained by PumpMainThread(), exactly once.
		std::function<void(const TaskResult&)> onSuccessMainThread; // exit code 0
		std::function<void(const TaskResult&)> onFailureMainThread; // nonzero / launch fail
		std::function<void()>                  onCancelMainThread;   // user cancelled
	};

	// Start a task.  Returns kInvalidTask if the process could not be launched.
	TaskId StartTask(const TaskSpec& spec);

	// Request cancel: writes cancelFile (if any) and terminates the child after a
	// short grace period.  The onCancel callback fires from PumpMainThread().
	void CancelTask(TaskId id);

	// Drain finished tasks and fire their callbacks.  MAIN THREAD ONLY.
	// Call once per frame from EditorInterface::update().
	void PumpMainThread();

	// True while any task is Pending or Running.
	bool HasActiveTasks();

#ifdef MC2_IMGUI
	// Draw the Task Monitor window.  Call from the ImGui render pass.
	void RenderImGui();
#endif
}

#endif // EDITOR_TASK_RUNNER_H
