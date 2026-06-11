//-------------------------------------------------------------------------------------------------
// EditorPlaytest.cpp -- see EditorPlaytest.h for the contract + the shadow-save compromise.
//
// Wraps EditorTaskRunner (process + combined stdout/stderr pipe + exit code) with the
// playtest-specific glue: save the mission in place, resolve/validate the game exe,
// stale-deploy mtime warning, launch with `-mission <pak>` (cwd = exe dir), and on exit
// archive the full captured log to <missionDir>/playtest-logs/<timestamp>.log.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"   // MFC / <windows.h>

#include "EditorPlaytest.h"
#include "EditorTaskRunner.h"
#include "EditorData.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace EditorPlaytest
{
namespace {

State        s_state = State::Idle;
EditorTaskRunner::TaskId s_task = EditorTaskRunner::kInvalidTask;
char         s_status[512] = "Idle -- load + save a mission, then Playtest.";
char         s_exePath[512] = "";
bool         s_exeResolved = false;

// --- helpers --------------------------------------------------------------------------

// Split a full file path into directory + filename-base (no extension).
void SplitPath(const std::string& full, std::string& dir, std::string& base)
{
	size_t slash = full.find_last_of("/\\");
	dir  = (slash != std::string::npos) ? full.substr(0, slash) : std::string(".");
	std::string fileNm = (slash != std::string::npos) ? full.substr(slash + 1) : full;
	size_t dot = fileNm.find_last_of('.');
	base = (dot != std::string::npos) ? fileNm.substr(0, dot) : fileNm;
}

bool FileExists(const char* p)
{
	DWORD a = GetFileAttributesA(p);
	return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Returns true if the exe was last written more than 24h ago (stale-deploy guard).
bool ExeIsStale(const char* p)
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fad))
		return false;
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER w, n;
	w.LowPart = fad.ftLastWriteTime.dwLowDateTime;  w.HighPart = fad.ftLastWriteTime.dwHighDateTime;
	n.LowPart = now.dwLowDateTime;                  n.HighPart = now.dwHighDateTime;
	const ULONGLONG kDay = 24ULL * 60ULL * 60ULL * 10000000ULL;   // 100ns ticks / day
	return (n.QuadPart > w.QuadPart) && ((n.QuadPart - w.QuadPart) > kDay);
}

// Resolve the game exe once: env MC2_PLAYTEST_EXE, then probe candidates relative to the
// editor cwd.  Stores into s_exePath.  Idempotent; safe to call repeatedly.
void ResolveExe()
{
	if (s_exeResolved && s_exePath[0])
		return;

	char envBuf[512] = "";
	DWORD n = GetEnvironmentVariableA("MC2_PLAYTEST_EXE", envBuf, sizeof(envBuf));
	if (n > 0 && n < sizeof(envBuf) && envBuf[0])
	{
		strncpy(s_exePath, envBuf, sizeof(s_exePath) - 1);
		s_exePath[sizeof(s_exePath) - 1] = 0;
		s_exeResolved = true;
		return;
	}

	// Probe candidates relative to the editor's working directory.
	const char* candidates[] = {
		"..\\mc2-win64-v0.4\\mc2.exe",
		".\\mc2.exe",
		"mc2.exe",
	};
	for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i)
	{
		if (FileExists(candidates[i]))
		{
			strncpy(s_exePath, candidates[i], sizeof(s_exePath) - 1);
			s_exePath[sizeof(s_exePath) - 1] = 0;
			s_exeResolved = true;
			return;
		}
	}

	// Nothing found: leave the default probe path so the user can see/edit it.
	if (!s_exePath[0])
	{
		strncpy(s_exePath, candidates[0], sizeof(s_exePath) - 1);
		s_exePath[sizeof(s_exePath) - 1] = 0;
	}
	s_exeResolved = true;
}

// Archive the captured log next to the mission.  Returns the written path (empty on fail).
std::string ArchiveLog(const std::string& missionPak, const std::string& log)
{
	std::string dir, base;
	SplitPath(missionPak, dir, base);

	std::string logDir = dir + "/playtest-logs";
	CreateDirectoryA(logDir.c_str(), NULL);   // ignore "already exists"

	time_t t = time(NULL);
	struct tm lt;
	localtime_s(&lt, &t);
	char stamp[32];
	snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
		lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);

	std::string logPath = logDir + "/" + stamp + ".log";
	FILE* f = NULL;
	if (fopen_s(&f, logPath.c_str(), "wb") == 0 && f)
	{
		fwrite(log.data(), 1, log.size(), f);
		fclose(f);
		return logPath;
	}
	return std::string();
}

// Pull the last non-empty line of the captured log for the status string.
std::string LastLine(const std::string& log)
{
	size_t end = log.find_last_not_of("\r\n");
	if (end == std::string::npos) return std::string();
	size_t start = log.find_last_of('\n', end);
	start = (start == std::string::npos) ? 0 : start + 1;
	return log.substr(start, end - start + 1);
}

// Shared completion handler (success or failure both archive the log + report exit code).
void OnFinished(const EditorTaskRunner::TaskResult& res)
{
	s_state = State::Idle;
	s_task  = EditorTaskRunner::kInvalidTask;

	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : 0;
	std::string logPath = (missionPath && missionPath[0])
		? ArchiveLog(missionPath, res.log)
		: std::string();

	const bool pass = (res.exitCode == 0);
	std::string last = LastLine(res.log);

	snprintf(s_status, sizeof(s_status),
		"%s (exit %d). Log: %s%s%s",
		pass ? "PASS" : "FAIL",
		res.exitCode,
		logPath.empty() ? "(write failed)" : logPath.c_str(),
		last.empty() ? "" : "  | ",
		last.empty() ? "" : last.c_str());
}

void OnCancelled()
{
	s_state = State::Idle;
	s_task  = EditorTaskRunner::kInvalidTask;

	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : 0;
	(void)missionPath;
	snprintf(s_status, sizeof(s_status), "Stopped (playtest terminated).");
}

} // anonymous namespace

// --- public API -----------------------------------------------------------------------

bool IsRunning()
{
	return s_state == State::Running;
}

bool CanPlaytest()
{
	if (IsRunning())
		return false;
	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : 0;
	return (missionPath && missionPath[0]);
}

void Start()
{
	if (IsRunning())
		return;

	if (!EditorData::instance)
	{
		snprintf(s_status, sizeof(s_status), "No editor data loaded.");
		return;
	}

	const char* missionPath = EditorData::instance->getMapName();
	if (!missionPath || !missionPath[0])
	{
		snprintf(s_status, sizeof(s_status), "No mission loaded -- save a mission first.");
		return;
	}

	// COMPROMISE (see header): save IN PLACE.  EditorData::save() mutates path state
	// (setMapName) + writes .old backups; a non-mutating shadow copy to .playtest/ would
	// need a >50-line EditorData refactor -- out of slice budget.  We launch the saved pak.
	std::string savedPak = missionPath;
	if (!EditorData::instance->save(savedPak.c_str()))
	{
		snprintf(s_status, sizeof(s_status), "Mission save failed -- aborting playtest.");
		return;
	}
	// save() may rewrite mapName; re-read to launch exactly what was written.
	if (EditorData::instance->getMapName())
		savedPak = EditorData::instance->getMapName();

	ResolveExe();
	if (!FileExists(s_exePath))
	{
		snprintf(s_status, sizeof(s_status),
			"Game exe not found: %s  (set MC2_PLAYTEST_EXE or edit the path).", s_exePath);
		return;
	}

	std::string exeDir, exeBase;
	SplitPath(s_exePath, exeDir, exeBase);

	std::string stalePrefix;
	if (ExeIsStale(s_exePath))
		stalePrefix = "WARNING: game exe is >24h old (possible stale deploy).\n";

	EditorTaskRunner::TaskSpec spec;
	spec.name = "Playtest";
	char cmd[1200];
	snprintf(cmd, sizeof(cmd), "\"%s\" -mission \"%s\"", s_exePath, savedPak.c_str());
	spec.commandLine     = cmd;
	spec.workingDirectory = exeDir;
	spec.onSuccessMainThread = &OnFinished;
	spec.onFailureMainThread = &OnFinished;
	spec.onCancelMainThread  = &OnCancelled;

	s_task = EditorTaskRunner::StartTask(spec);
	if (s_task == EditorTaskRunner::kInvalidTask)
	{
		s_state = State::Idle;
		snprintf(s_status, sizeof(s_status), "Failed to launch game process.");
		return;
	}

	s_state = State::Running;
	snprintf(s_status, sizeof(s_status), "%sRunning: %s -mission %s",
		stalePrefix.c_str(), s_exePath, savedPak.c_str());
}

void Stop()
{
	if (!IsRunning() || s_task == EditorTaskRunner::kInvalidTask)
		return;
	EditorTaskRunner::CancelTask(s_task);
	// State transition happens in OnCancelled via PumpMainThread.
}

const char* StatusLine()
{
	return s_status;
}

char* GameExePathBuffer()
{
	ResolveExe();
	return s_exePath;
}

int GameExePathBufferSize()
{
	return (int)sizeof(s_exePath);
}

} // namespace EditorPlaytest
