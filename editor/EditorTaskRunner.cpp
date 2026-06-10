//-------------------------------------------------------------------------------------------------
// EditorTaskRunner.cpp -- see EditorTaskRunner.h for the threading contract.
//
// One worker std::thread per task.  The worker launches the child via CreateProcess
// with a combined stdout+stderr pipe, reads it line-by-line, parses PROGRESS lines,
// and stores everything under s_mutex.  On child exit it flags the task done; the
// main thread (PumpMainThread) fires the result callback and joins the worker.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"   // MFC / <windows.h>

#include "EditorTaskRunner.h"

#ifdef MC2_IMGUI
#include "imgui.h"
#endif

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace EditorTaskRunner
{
namespace {

// Max log characters retained per task (tail).  Keeps memory bounded for chatty gens.
static const size_t kMaxLogChars = 64 * 1024;

struct Task
{
	TaskId       id = kInvalidTask;
	TaskSpec     spec;

	// --- shared state (guard with s_mutex) ---
	Status       status   = Status::Pending;
	int          progress = 0;          // 0..100
	std::string  stage;                 // PROGRESS stage token
	std::string  message;               // PROGRESS trailing message
	std::string  log;                   // captured stdout+stderr (tail-trimmed)
	int          exitCode = -1;

	std::atomic<bool> cancelRequested{ false };
	std::atomic<bool> workerDone{ false };   // worker thread finished
	bool         callbackFired = false;      // main thread fired result once

	HANDLE       hProcess = NULL;            // valid while running (guard with s_mutex)
	std::thread  worker;
};

static std::mutex                         s_mutex;
static std::vector<std::shared_ptr<Task>> s_tasks;
static TaskId                             s_nextId = 1;

// Append to the task log under lock, trimming to the tail if oversized.
static void AppendLog(Task* t, const char* data, size_t len)
{
	t->log.append(data, len);
	if (t->log.size() > kMaxLogChars)
		t->log.erase(0, t->log.size() - kMaxLogChars);
}

// Parse one output line.  "PROGRESS <pct> <stage> <msg...>" updates progress fields;
// anything else is plain log.  Caller holds s_mutex.
static void HandleLine(Task* t, const std::string& line)
{
	AppendLog(t, line.data(), line.size());
	AppendLog(t, "\n", 1);

	if (line.compare(0, 9, "PROGRESS ") != 0)
		return;

	const char* p = line.c_str() + 9;
	while (*p == ' ') ++p;

	// pct
	int pct = 0;
	bool haveDigit = false;
	while (*p >= '0' && *p <= '9') { pct = pct * 10 + (*p - '0'); ++p; haveDigit = true; }
	if (!haveDigit) return;
	if (pct < 0) pct = 0; if (pct > 100) pct = 100;
	t->progress = pct;

	while (*p == ' ') ++p;
	// stage token
	const char* stageStart = p;
	while (*p && *p != ' ') ++p;
	t->stage.assign(stageStart, p);
	while (*p == ' ') ++p;
	// rest = message
	t->message.assign(p);
}

// Worker thread body.  Launches the child, pumps the pipe, sets terminal status.
static void WorkerMain(std::shared_ptr<Task> t)
{
	SECURITY_ATTRIBUTES sa{};
	sa.nLength        = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	HANDLE hRead = NULL, hWrite = NULL;
	if (!CreatePipe(&hRead, &hWrite, &sa, 0))
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		t->status = Status::Failed;
		t->message = "CreatePipe failed";
		t->workerDone = true;
		return;
	}
	// Child must not inherit the read end.
	SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOA si{};
	si.cb         = sizeof(si);
	si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = hWrite;
	si.hStdError  = hWrite;          // combine stderr into the same pipe
	si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION pi{};

	// CreateProcessA needs a mutable command buffer.
	std::vector<char> cmd(t->spec.commandLine.begin(), t->spec.commandLine.end());
	cmd.push_back('\0');
	const char* cwd = t->spec.workingDirectory.empty() ? NULL : t->spec.workingDirectory.c_str();

	BOOL ok = CreateProcessA(
		NULL, cmd.data(), NULL, NULL,
		TRUE,                              // inherit handles (the pipe write end)
		CREATE_NO_WINDOW, NULL, cwd, &si, &pi);

	// Parent never writes the pipe; close its copy so ReadFile sees EOF at child exit.
	CloseHandle(hWrite);

	if (!ok)
	{
		DWORD err = GetLastError();
		CloseHandle(hRead);
		std::lock_guard<std::mutex> lk(s_mutex);
		t->status  = Status::Failed;
		char buf[128];
		snprintf(buf, sizeof(buf), "CreateProcess failed (err=%lu). Is 'py'/python on PATH?", err);
		t->message = buf;
		AppendLog(t.get(), buf, strlen(buf));
		t->workerDone = true;
		return;
	}

	{
		std::lock_guard<std::mutex> lk(s_mutex);
		t->status   = Status::Running;
		t->hProcess = pi.hProcess;
	}

	// Read the combined pipe, splitting on newlines.
	std::string pending;
	char buf[4096];
	for (;;)
	{
		DWORD n = 0;
		BOOL r = ReadFile(hRead, buf, sizeof(buf), &n, NULL);
		if (!r || n == 0)
			break;                          // pipe closed (child exited) or error

		std::lock_guard<std::mutex> lk(s_mutex);
		for (DWORD i = 0; i < n; ++i)
		{
			char c = buf[i];
			if (c == '\r') continue;
			if (c == '\n') { HandleLine(t.get(), pending); pending.clear(); }
			else           { pending.push_back(c); }
		}
	}
	if (!pending.empty())
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		HandleLine(t.get(), pending);
	}

	CloseHandle(hRead);

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);

	{
		std::lock_guard<std::mutex> lk(s_mutex);
		t->exitCode = (int)code;
		if (t->cancelRequested.load())      t->status = Status::Cancelled;
		else if (code == 0)               { t->status = Status::Succeeded; t->progress = 100; }
		else                                t->status = Status::Failed;
		t->hProcess = NULL;
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	t->workerDone = true;
}

static std::shared_ptr<Task> FindTask(TaskId id)
{
	for (auto& t : s_tasks)
		if (t->id == id) return t;
	return nullptr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------

TaskId StartTask(const TaskSpec& spec)
{
	auto t  = std::make_shared<Task>();
	t->spec = spec;

	std::lock_guard<std::mutex> lk(s_mutex);
	t->id     = s_nextId++;
	t->status = Status::Pending;
	if (t->spec.name.empty()) t->spec.name = "Task";
	s_tasks.push_back(t);
	t->worker = std::thread(WorkerMain, t);
	return t->id;
}

void CancelTask(TaskId id)
{
	HANDLE hProc = NULL;
	std::string cancelFile;
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		auto t = FindTask(id);
		if (!t) return;
		if (t->status != Status::Running && t->status != Status::Pending) return;
		t->cancelRequested.store(true);
		hProc      = t->hProcess;
		cancelFile = t->spec.cancelFile;
	}

	// Cooperative: touch the cancel file so a well-behaved generator can stop cleanly.
	if (!cancelFile.empty())
	{
		HANDLE f = CreateFileA(cancelFile.c_str(), GENERIC_WRITE, 0, NULL,
		                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
	}

	// Hard kill: terrain_gen.py does not poll a cancel file, so terminate it.
	// The child writes into a staging dir and we never run the success callback
	// on a cancelled task, so no partial output is ever applied.
	if (hProc)
		TerminateProcess(hProc, 1);
}

void PumpMainThread()
{
	// Collect callbacks to fire, then run them OUTSIDE the lock (they call into the
	// editor / GL and may take time).
	struct Pending { std::shared_ptr<Task> task; };
	std::vector<std::shared_ptr<Task>> ready;

	{
		std::lock_guard<std::mutex> lk(s_mutex);
		for (auto& t : s_tasks)
		{
			if (t->workerDone.load() && !t->callbackFired)
			{
				t->callbackFired = true;
				ready.push_back(t);
			}
		}
	}

	for (auto& t : ready)
	{
		if (t->worker.joinable())
			t->worker.join();

		// Snapshot result under lock.
		TaskResult res;
		Status st;
		std::function<void(const TaskResult&)> onSucc, onFail;
		std::function<void()> onCancel;
		{
			std::lock_guard<std::mutex> lk(s_mutex);
			res.id       = t->id;
			res.status   = t->status;
			res.exitCode = t->exitCode;
			res.name     = t->spec.name;
			res.log      = t->log;
			st       = t->status;
			onSucc   = t->spec.onSuccessMainThread;
			onFail   = t->spec.onFailureMainThread;
			onCancel = t->spec.onCancelMainThread;
		}

		if (st == Status::Succeeded)      { if (onSucc)   onSucc(res); }
		else if (st == Status::Cancelled) { if (onCancel) onCancel(); }
		else                              { if (onFail)   onFail(res); }
	}
}

bool HasActiveTasks()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	for (auto& t : s_tasks)
		if (t->status == Status::Pending || t->status == Status::Running)
			return true;
	return false;
}

#ifdef MC2_IMGUI

void RenderImGui()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	if (s_tasks.empty())
		return;

	ImGui::SetNextWindowSize(ImVec2(420.f, 0.f), ImGuiCond_Once);
	ImGui::SetNextWindowPos(ImVec2(16.f, 320.f), ImGuiCond_Once);
	if (!ImGui::Begin("Task Monitor", nullptr, ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		return;
	}

	for (auto& t : s_tasks)
	{
		const char* statusStr = "?";
		ImVec4 col(1, 1, 1, 1);
		switch (t->status)
		{
			case Status::Pending:   statusStr = "pending";   col = ImVec4(0.7f,0.7f,0.7f,1); break;
			case Status::Running:   statusStr = "running";   col = ImVec4(0.6f,0.8f,1.0f,1); break;
			case Status::Succeeded: statusStr = "succeeded"; col = ImVec4(0.5f,0.9f,0.5f,1); break;
			case Status::Failed:    statusStr = "FAILED";    col = ImVec4(1.0f,0.5f,0.5f,1); break;
			case Status::Cancelled: statusStr = "cancelled"; col = ImVec4(0.9f,0.8f,0.4f,1); break;
		}

		ImGui::PushID((int)t->id);
		ImGui::TextColored(col, "%s  [%s]", t->spec.name.c_str(), statusStr);

		float frac = t->progress / 100.0f;
		char ov[64];
		snprintf(ov, sizeof(ov), "%d%%  %s", t->progress, t->stage.c_str());
		ImGui::ProgressBar(frac, ImVec2(-1.f, 0.f), ov);

		if (!t->message.empty())
			ImGui::TextWrapped("%s", t->message.c_str());

		if (t->status == Status::Running || t->status == Status::Pending)
		{
			if (ImGui::SmallButton("Cancel"))
			{
				// Defer the actual cancel: CancelTask re-locks s_mutex (we hold it).
				// Mark request now; PumpMainThread / next frame's CancelTask handles
				// the terminate.  Simpler: terminate inline since we hold the handle.
				t->cancelRequested.store(true);
				if (t->hProcess) TerminateProcess(t->hProcess, 1);
				if (!t->spec.cancelFile.empty())
				{
					HANDLE f = CreateFileA(t->spec.cancelFile.c_str(), GENERIC_WRITE, 0,
					                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
					if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
				}
			}
		}

		if (t->status == Status::Failed)
			ImGui::Text("exit code: %d", t->exitCode);

		if (ImGui::TreeNode("log"))
		{
			ImGui::BeginChild("logchild", ImVec2(0, 120.f), true,
			                  ImGuiWindowFlags_HorizontalScrollbar);
			ImGui::TextUnformatted(t->log.c_str());
			ImGui::EndChild();
			ImGui::TreePop();
		}

		ImGui::Separator();
		ImGui::PopID();
	}

	if (ImGui::SmallButton("Clear finished"))
	{
		// Erase only fully-finished tasks whose callbacks already fired.
		for (size_t i = s_tasks.size(); i-- > 0; )
		{
			auto& t = s_tasks[i];
			bool finished = (t->status == Status::Succeeded ||
			                 t->status == Status::Failed ||
			                 t->status == Status::Cancelled);
			if (finished && t->callbackFired && !t->worker.joinable())
				s_tasks.erase(s_tasks.begin() + i);
		}
	}

	ImGui::End();
}

#endif // MC2_IMGUI

} // namespace EditorTaskRunner
