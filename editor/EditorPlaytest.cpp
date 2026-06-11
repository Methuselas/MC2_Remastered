//-------------------------------------------------------------------------------------------------
// EditorPlaytest.cpp -- see EditorPlaytest.h for the contract + the shadow-save compromise.
//
// Wraps EditorTaskRunner (process + combined stdout/stderr pipe + exit code) with the
// playtest-specific glue: save the mission in place, resolve/validate the game exe,
// stale-deploy mtime warning, launch with `-mission <stem>` (cwd = exe dir), and on exit
// archive the full captured log to <missionDir>/playtest-logs/<timestamp>.log.
//
// STEM + COPY BRIDGE CONTRACT (the load-bearing detail):
//   mc2.exe `-mission` takes a bare STEM, NOT a path.  The engine resolves the file as
//   `missionPath + stem + ".fit"/".pak"` where missionPath comes from system.cfg and is
//   effectively `data\missions\` relative to the game's cwd (mission.cpp:1500/1904/2166).
//   Passing a full saved path produced the mangled `data/missions/<abspath>.fit`, a missing
//   .fit, and the "Could not find Teams Block" fail-fast (mission.cpp:1985 -> exit 0xC0000409).
//   So we (1) derive the stem from the saved pak filename, (2) COPY both `<stem>.pak` and the
//   sibling `<stem>.fit` (they are saved next to each other) into the GAME install's
//   `<exeDir>\data\missions\` -- the game install may differ from the editor's, which is the
//   whole point of the bridge -- and (3) launch `-mission <stem>` with cwd = exeDir.
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

// Normalize a (possibly relative / `..`-laden) path to a full absolute path.
// On failure returns the input unchanged so callers still have something usable.
std::string AbsPath(const std::string& in)
{
	char buf[1024];
	DWORD n = GetFullPathNameA(in.c_str(), sizeof(buf), buf, NULL);
	if (n > 0 && n < sizeof(buf))
		return std::string(buf);
	return in;
}

// Case-insensitive path equality (after normalization the caller should already
// have run AbsPath on both sides; Windows paths are case-insensitive).
bool SamePath(const std::string& a, const std::string& b)
{
	return _stricmp(a.c_str(), b.c_str()) == 0;
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
		std::string abs = AbsPath(envBuf);
		strncpy(s_exePath, abs.c_str(), sizeof(s_exePath) - 1);
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
			std::string abs = AbsPath(candidates[i]);
			strncpy(s_exePath, abs.c_str(), sizeof(s_exePath) - 1);
			s_exePath[sizeof(s_exePath) - 1] = 0;
			s_exeResolved = true;
			return;
		}
	}

	// Nothing found: leave the default probe path so the user can see/edit it, but do NOT
	// latch s_exeResolved -- the exe may be deployed between now and the next Start(), so a
	// later call must re-probe instead of being short-circuited by the early-return above.
	if (!s_exePath[0])
	{
		strncpy(s_exePath, candidates[0], sizeof(s_exePath) - 1);
		s_exePath[sizeof(s_exePath) - 1] = 0;
	}
}

// Archive the captured log next to the mission.  Returns the written path (empty on fail).
// `prefix` is prepended to the timestamped filename (e.g. "stopped-") to distinguish a
// user-terminated run from a natural exit.
std::string ArchiveLog(const std::string& missionPak, const std::string& log,
	const char* prefix = "")
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

	std::string logPath = logDir + "/" + (prefix ? prefix : "") + stamp + ".log";
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

	// --- STEM + COPY BRIDGE (see file header) -----------------------------------------
	// The engine resolves `-mission <stem>` as data\missions\<stem>.fit/.pak relative to the
	// game's cwd (= exeDir).  Derive the stem and bridge BOTH the .pak and its sibling .fit
	// into the game install's data\missions\, since the game install may differ from ours.
	std::string srcDir, srcBase;
	SplitPath(savedPak, srcDir, srcBase);
	std::string stem = srcBase;

	std::string srcPak = srcDir + "\\" + stem + ".pak";
	std::string srcFit = srcDir + "\\" + stem + ".fit";
	if (!FileExists(srcPak.c_str()))
		srcPak = savedPak;   // fall back to whatever save() actually wrote
	if (!FileExists(srcFit.c_str()))
	{
		snprintf(s_status, sizeof(s_status),
			"Mission .fit not found next to pak (%s) -- cannot bridge to game; aborting.",
			srcFit.c_str());
		return;
	}

	std::string dstDir = exeDir + "\\data\\missions";
	// Create data\ then data\missions (CreateDirectoryA is single-level only).
	CreateDirectoryA((exeDir + "\\data").c_str(), NULL);
	CreateDirectoryA(dstDir.c_str(), NULL);

	// Normalize all four paths to absolute so the same-file check is reliable (the mission
	// may have been opened FROM the game install, making src == dst via a relative `..\`
	// dest) and so status/launch never leak confusing relative paths.
	srcPak = AbsPath(srcPak);
	srcFit = AbsPath(srcFit);
	std::string dstPak = AbsPath(dstDir + "\\" + stem + ".pak");
	std::string dstFit = AbsPath(dstDir + "\\" + stem + ".fit");

	// Per-file: if src and dst resolve to the same file, it is already in place -- skip the
	// copy silently (copying a file onto itself fails with sharing-violation err 32).
	if (!SamePath(srcPak, dstPak) && !CopyFileA(srcPak.c_str(), dstPak.c_str(), FALSE))
	{
		snprintf(s_status, sizeof(s_status),
			"Failed to copy pak into game (%s -> %s), err %lu; aborting.",
			srcPak.c_str(), dstPak.c_str(), GetLastError());
		return;
	}
	if (!SamePath(srcFit, dstFit) && !CopyFileA(srcFit.c_str(), dstFit.c_str(), FALSE))
	{
		snprintf(s_status, sizeof(s_status),
			"Failed to copy fit into game (%s -> %s), err %lu; aborting.",
			srcFit.c_str(), dstFit.c_str(), GetLastError());
		return;
	}

	std::string stalePrefix;
	if (ExeIsStale(s_exePath))
		stalePrefix = "WARNING: game exe is >24h old (possible stale deploy).\n";

	// Quote the stem only when it contains spaces (engine treats the token as the bare stem).
	bool stemHasSpace = (stem.find(' ') != std::string::npos);

	EditorTaskRunner::TaskSpec spec;
	spec.name = "Playtest";
	char cmd[1200];
	snprintf(cmd, sizeof(cmd), stemHasSpace ? "\"%s\" -mission \"%s\"" : "\"%s\" -mission %s",
		s_exePath, stem.c_str());
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
	snprintf(s_status, sizeof(s_status), "%sRunning: -mission %s  (bridged to %s)",
		stalePrefix.c_str(), stem.c_str(), dstDir.c_str());
}

void Stop()
{
	if (!IsRunning() || s_task == EditorTaskRunner::kInvalidTask)
		return;

	// The worker's captured process log is private to EditorTaskRunner and there is no
	// public accessor (and we must not modify EditorTaskRunner here), so the natural-exit
	// archive in OnFinished cannot run for a cancelled task -- PumpMainThread routes cancels
	// to OnCancelMainThread (which carries no TaskResult/log).  Archive a "stopped-" marker
	// note now, BEFORE CancelTask, so a user-terminated run still leaves a breadcrumb next
	// to the mission instead of dropping the record entirely.
	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : 0;
	if (missionPath && missionPath[0])
	{
		std::string note = "Playtest STOPPED by user (process terminated).\n";
		note += "Status at stop: ";
		note += s_status;
		note += "\n(Full process stdout/stderr is not retained on user-stop.)\n";
		ArchiveLog(missionPath, note, "stopped-");
	}

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
