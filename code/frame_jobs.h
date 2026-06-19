#pragma once
//---------------------------------------------------------------------------
// frame_jobs.h — FRAME-JOBS-1 minimal stub.
//
// Task 3 expands this into the real gate (env-var parse, worker pool API).
// For now: frameJobsEnabled() returns false unconditionally so all Task-2
// cache-hit skip branches compile but are dead-code eliminated.
//---------------------------------------------------------------------------

inline bool frameJobsEnabled() { return false; }
