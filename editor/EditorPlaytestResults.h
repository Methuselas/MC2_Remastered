//-------------------------------------------------------------------------------------------------
// EditorPlaytestResults.h -- "Playtest Results" ImGui panel + log summary parser (v0).
//
// Turns an archived playtest stdout log (written by EditorPlaytest::OnFinished, next to the
// mission under playtest-logs/<stamp>.log or the mod's .modproject tree) into a readable
// summary: status chip (PASS/WARN/FAIL), child exit code, [SMOKE v1] result+reason, mission
// frame count, [PERF v1] p50/p99, [MOVER v1] line + unique-mover counts, and a warnings
// section (FATAL/ASSERT/Crash Report/[ESMOKE v1] event=modal markers).
//
// NO new engine telemetry: this is a pure post-hoc reader of stdout the engine already emits.
//
// HOOKS: EditorPlaytest::OnFinished calls ParseLog() on the just-archived log (live hand-off).
// On panel open (Draw, first frame with no parsed result) it falls back to parsing
// EditorPlaytest::LastLogPath() so a prior run's result is shown after an editor restart.
//
// THREADING: main (editor) thread only, same as the rest of the editor ImGui surface.
//-------------------------------------------------------------------------------------------------
#ifndef EDITOR_PLAYTEST_RESULTS_H
#define EDITOR_PLAYTEST_RESULTS_H

#include <string>
#include <vector>

namespace EditorPlaytestResults
{
	enum class Status { Unknown, Pass, Warn, Fail };

	// Parsed summary of one playtest log. All fields default to "not present" so the panel
	// can render partial results (e.g. an early-abort log with no [PERF] line).
	struct Summary
	{
		bool        valid       = false;   // a log was actually parsed
		std::string logPath;               // source log on disk

		Status      status      = Status::Unknown;
		std::string statusReason;          // short human reason for the chip

		// Child exit code: parsed from the "[playtest] exit code N (...)" tail line
		// EditorPlaytest appends. -1 = not found in the log.
		int         childExit   = -1;
		bool        haveExit    = false;

		// [SMOKE v1] event=summary result=<pass|fail> reason=<...>
		bool        haveSmoke   = false;
		std::string smokeResult;           // "pass" / "fail" / "early_exit" ...
		std::string smokeReason;           // optional reason= token

		// Frame / duration (from [SMOKE v1] frames= or [PERF v1] samples=).
		long        frames      = -1;

		// [PERF v1] p50_ms / p99_ms (engine-emitted; we only read them).
		bool        havePerf    = false;
		float       p50ms       = 0.f;
		float       p99ms       = 0.f;
		float       avgFps      = 0.f;

		// [MOVER v1] counts.
		int         moverLines  = 0;       // total [MOVER v1] lines
		int         uniqueMovers = 0;      // distinct id= values
		int         teamCount   = 0;       // distinct team= values

		// Warning / fatal markers (lines with FATAL / ASSERT / Crash Report /
		// [ESMOKE v1] event=modal). Stored verbatim (trimmed) for the warnings section.
		std::vector<std::string> warnings;
		bool        haveCrashMarker = false;   // a hard crash/fatal marker present
	};

	// Parse an archived log FILE on disk. Returns a Summary (valid=false if the file
	// could not be read). Pure; no global state touched.
	Summary ParseLogFile(const char* path);

	// Parse log TEXT already in memory (used by the OnFinished live hand-off so we do not
	// re-read the file we just wrote). logPath is recorded into Summary.logPath only.
	Summary ParseLogText(const std::string& text, const char* logPath);

	// Set the panel's current result (called by EditorPlaytest::OnFinished after a run).
	void SetResult(const Summary& s);

	// Panel visibility.
	void Open();
	void Close();
	void Toggle();
	bool IsOpen();

	// Draw the floating panel (no-op when closed). On first draw with no result yet,
	// lazily parses EditorPlaytest::LastLogPath() so a prior run is shown.
	void Draw();
}

#endif // EDITOR_PLAYTEST_RESULTS_H
