# SMOKE-FIRST-LAUNCH-CRASH-SILENT-RECON-1 — verdict

**Status:** RECON COMPLETE — no fix in this slice. Classification + cheap repro recipe.
**Against:** nifty `97b80c1a`.

## Symptom
In full `--tier tier1` batch runs, exactly one mission intermittently reports
`crash_silent` (0 frames after a normal Load ms) **or** `heartbeat_freeze_play`
(loaded, played N frames, then stalled). The failing mission VARIES run-to-run
(observed mc2_17 and mc2_24 across this session's runs) and ALWAYS passes on an
isolated `--mission <id>` re-run with healthy frames/FPS. Seen on unmodified
baselines — independent of any code change under test.

## NOT these (excluded)
- **STARTUP-INIT-ASYNC-1** (`6307b382`, the async-init mission-select null-deref):
  its own commit notes "tier1 smoke missed it because it seeds missions directly,
  **bypassing MissionSelectionScreen**." The batch flake happens INSIDE tier1's
  direct-seed path, so it is a different failure. (That was the user-facing
  first-launch crash; now fixed.)
- **`--kill-existing` image-name taskkill race** (`run_smoke_kill_existing_causes_flaky_crash`,
  `check-smoke-matrices.py:159`): produces a false `crash_silent` for the
  in-flight mission. NOT applicable here — these runs never passed `--kill-existing`.
- **A runner detection artifact:** ruled out by the bucket definitions
  (`scripts/smoke_lib/gates.py`): `crash_silent` requires
  `smoke_summary_result is None AND exit_code != 0` — the mc2.exe process really
  exited nonzero. It is not a mislabel of a healthy run.
- **An engine-level crash:** ruled out by `crash_handler_hit` — when the engine's
  SEH crash handler fires, the bucket is `crash_no_summary`, not `crash_silent`.
  crash_silent means nonzero exit with NO crash-handler output, and NO
  minidump/crash bundle is written. That is not an app-level C++ fault.

## Classification (primary)
**GPU driver TDR / device-lost under contention from a concurrently-running game
instance** (the v0.4c soak lane, observed respawning mc2.exe throughout this
session). Mechanism:
- nonzero exit + no crash-handler + no dump == the OS/driver reset the GPU
  (TDR) and tore the GL context out from under the process -> `crash_silent`.
- the milder case is a GPU stall that doesn't TDR: the process stays alive but
  stops emitting heartbeats past the play timeout -> `heartbeat_freeze_play`.
- The smoke runner's concurrency lock (`smoke.lock`) prevents two SMOKE RUNS
  from colliding, but it does NOT stop a separate soak/game mc2.exe on the same
  GPU. So GPU contention is an open path even with a single smoke run.

Evidence: batch-only, mission-varies, passes-isolated, co-occurs with the live
v0.4c soak; no crash bundle/minidump produced; clean 5/5 batches occurred when
the soak was idle.

## Residual uncertainty
Cannot yet fully exclude a SECONDARY intrinsic cause: rapid GL-context
create/destroy churn between back-to-back missions (each mission spawns
launcher -> mc2 -> create+destroy a GL context) could occasionally TDR even with
no concurrent soak. Session evidence favors external contention (the clean
batches), but a solo control run was not isolated from the soak.

## Cheap repro / settle recipe (no fix, ~2 batches)
1. Confirm no other mc2.exe / soak is running:
   `Get-CimInstance Win32_Process -Filter "Name='mc2.exe'"` -> empty.
2. Run `--tier tier1` 3x back-to-back. If clean 15/15 -> external contention
   confirmed (the flake needs a concurrent GPU consumer).
3. If a solo batch still flakes -> intrinsic back-to-back context churn; then the
   fix is a small inter-mission settle in the runner (drain/await GL teardown +
   a short fixed delay before the next launch), NOT an engine change.

## Recommended handling (until fixed)
- Treat a SINGLE tier1 batch mission that reports `crash_silent` /
  `heartbeat_freeze_play` AND passes on isolated re-run as ENVIRONMENTAL — not a
  regression. (This session applied that rule correctly.)
- For a trustworthy clean signal, do not run tier1 concurrently with a soak on
  the same GPU.
- If it recurs in a verified-solo run, open SMOKE-INTER-MISSION-SETTLE-1 (runner
  only): await GL-context teardown + short settle between missions.

Related: [[smoke-runner-block-mechanisms]], `run_smoke_kill_existing_causes_flaky_crash`.
