# Modder Workflow — First Loop Implementation Program

Status: ACTIVE. Orchestrated 2026-06-11. Loop = Open/create mod project → edit mission → one-click Playtest in Game → capture results.

## Ground truth (verified 2026-06-11, supersedes strategy-doc claims)

- **NO playtest button exists.** `grep -i playtest editor/` = zero hits. Strategy doc claim "one-click launch button exists at EditorInterface.cpp:5301-5326" is FALSE/aspirational. Slice 1 is a BUILD, not a polish.
- **EditorTaskRunner EXISTS** (`editor/EditorTaskRunner.cpp/.h`, ~150 lines): `CreateProcessA` + combined stdout/stderr pipe, worker thread, line callback, exit code, 64KB log tail, status {Pending,Running,Succeeded,Failed,Cancelled}. Entry: `EditorTaskRunner::Launch(spec)`.
- **Game `-mission <path>` arg EXISTS** (`code/mechcmd2.cpp:2602-2634`): quoted paths OK, sets `justStartMission=true`.
- **Mod mounting EXISTS**: `MC2_ACTIVE_MOD` env read in `mclib/file.cpp:62-72`; priority active mod > deps > base > FastFiles. Editor-side `ModPicker.cpp`: `ScanMods()` finds `mods/<id>/`, `Activate()` sets env + `InitModSearchPaths`.
- **Smoke conventions EXIST**: `[SMOKE v1]`/`[TIMING v1]`/`[PERF v1]` stdout (`GameOS/gameos/gos_smoke.h/.cpp`), exit 0 = pass.
- **Editor save NOT mod-aware**: `EditorData.cpp:1487/1520/1557` hardcodes `data/missions/<name>.pak` + `.fit`.
- **Deploy-target trap**: game runs from `mc2-win64-v0.4/mc2.exe`; editor from `mc2-win64-0.4c/`. Playtest exe path must be explicit + mtime-checked.

## Shared invariants (hard constraints — all slices)

1. mc2.exe = runtime truth. No in-process PIE. Playtest = separate process.
2. No Mission/ObjectManager refactor. No PacketFile format change.
3. No second asset database. No editor rewrite. ImGui exposes existing systems.
4. `mods/<id>/` is the project. Engine scans `data/` only; `.modproject/` is tool-private (engine skips).
5. Bridges = files/env/process/stdout only. Launch via EditorTaskRunner; results via stdout pipe + exit code.
6. Mount priority lives ONLY in `mclib/file.cpp`. `MC2_ACTIVE_MOD=<id>` is the contract.
7. Slice-sized, reversible changes. Each slice = additive, own commit(s), revertable without touching others.

## Worktree decision

Implement in `.claude/worktrees/nifty-mendeleev` (branch `claude/terrain-gen-pcg`). Dirty WIP present in `GameOS/gameos/gos_mech_batcher.cpp`, `gos_mech_killswitch.h`, `code/simplecamera.cpp`, `mclib/mech3d.cpp`, `.gitignore` — **agents must NOT touch or commit those files**. Slice 1 touches editor/ only. `A:/Games/mc2-nifty-merge` (branch claude/nifty-mendeleev) left alone.

---

## Slice 1 — Playtest v0 (BUILD, was mislabeled "polish")

Goal: button in editor → auto-save shadow mission → launch mc2.exe → capture stdout + exit code → archive log. No mod awareness yet.

### Files to inspect/modify
- `editor/EditorTaskRunner.h/.cpp` — reuse `Launch()`; inspect TaskSpec shape (cmdline, cwd, line callback).
- `editor/EditorInterface.cpp` — ImGui toolbar (`renderToolbarImgui` area) + menu hook for "Playtest in Game" button.
- `editor/EditorData.cpp/.h` — `save()` path (1487/1520/1557), `getMapName()` (EditorData.h:186). Shadow save = save current mission to `<missionDir>/.playtest/<name>.pak` (+ `.fit`) WITHOUT changing current mission path state.
- New file pair `editor/EditorPlaytest.cpp/.h` — playtest state machine (Idle/Running), exe path resolve + persist, launch, log archive. Keeps EditorInterface diff small.
- Read-only reference: `code/mechcmd2.cpp:2602` (-mission contract), `GameOS/gameos/gos_smoke.h` (stdout grammar).

### Behavior spec
1. Button "Playtest" (toolbar + Tools menu). Disabled while a playtest task Running.
2. On click: shadow-save mission → resolve game exe path (config value in editor ini/`.modproject/editor.json` later; v0 = env `MC2_PLAYTEST_EXE` then fallback probe `..\mc2-win64-v0.4\mc2.exe` relative to editor cwd; warn in log if exe mtime older than 24h — split-brain guard).
3. Launch `"<exe>" -mission "<shadow pak abs path>"`, cwd = exe dir (game needs its deploy dir as cwd).
4. Pipe lines into task log (EditorTaskRunner already does). On exit: write full log to `playtest-logs/<timestamp>.log` next to mission; one-line ImGui status: exit code + pass/fail (exit 0 + saw `[SMOKE` or mission-ready marker = pass-ish; v0 just shows exit code + last lines).
5. Stop button: terminate child process (TerminateProcess via task handle if EditorTaskRunner supports Cancel; if not, add it).

### Smoke/test plan
- Build editor target (EditRel) green; build game unchanged (no game-side edits in slice 1).
- Compile-only gate sufficient + existing editor CLI smoke (`-mission ... -frames N`) must still pass.
- Manual visual gate (USER): click Playtest on loaded mission → game launches into that mission → close → log file appears, status shows exit code.

### Known issues
- **In-place save can lose campaign mission content (torrin incident 2026-06-11).** The Slice-1 compromise saves the mission IN PLACE (no shadow copy — see `EditorPlaytest::Start()` header). Clicking Playtest on `torrin.pak` (an original campaign mission) re-saved it with dropped content: `.fit` shrank 80074→56213 bytes, mech spawns + buildings gone (game A/B: destroyed 48→0, submit_buildings 9228→0). The `.pak.old` backup chain was overwritten by repeat saves; the original survived only via an unrelated mod copy. Mitigations added (`EditorPlaytest.cpp`): (1) **pristine snapshot** — one-time never-overwritten `<stem>.playtest-orig.pak/.fit` copied before the first in-place save; snapshot-copy failure ABORTS the playtest; (2) **save-shrink tripwire** — loud non-blocking warning if the new `.fit` is < 60% of the snapshot `.fit`; (3) **crash-log guarantee** — log archived on both PASS and FAIL exits, with the exit code appended into the archived log. **Real fix = shadow save (future slice)** so the original is never touched at all.

### Rollback
- Single commit (or 2: TaskRunner Cancel + Playtest feature). `git revert` clean — all new files + small ImGui hook. No engine/game changes.

### Risks
- Shadow save may have side effects in EditorData::save (path state mutation, MOVE rebuild cost). Mitigate: inspect save() first; if path-state entanglement, v0 saves over normal path + remembers (documented compromise).
- Game cwd assumptions (art files relative). Mitigate: cwd = exe dir.
- TerminateProcess mid-mission leaves no corruption risk (game writes nothing to mission).
- Wrong exe (stale deploy) — mtime warn only, not block.

### Stop conditions
- EditorData::save cannot produce shadow copy without >50-line refactor → STOP, report, downgrade to save-in-place.
- EditorTaskRunner lacks Cancel and adding it needs thread-model surgery → ship without Stop button, note known issue.
- Editor build break unrelated to changes → STOP, report.

---

## Slice 2 — Mod-aware playtest (QUEUED — do not start until Slice 1 smoke passes + user approves)

Goal: playtest inherits mod context. Detect mod root from mission path (`mods/<id>/data/missions/...`); pass `MC2_ACTIVE_MOD=<id>` + `MC2_DEBUG_STATE_DUMP=1` into child env; archive logs to `mods/<id>/.modproject/playtest/<timestamp>/` instead of playtest-logs/.

### Inspect
- `editor/ModPicker.cpp` (`ScanMods`, `Activate`), `mclib/file.cpp:62-72` (env contract), EditorTaskRunner env-block support (likely missing — CreateProcessA env param; add `envExtra` to TaskSpec).
- Engine `.modproject/` skip filter in file.cpp index walk — verify exists; if missing, add one-line filter (engine-side, tiny).

### Opus prompt (queued)
"In nifty worktree: extend EditorTaskRunner::TaskSpec with optional env additions (build env block = parent env + extras for CreateProcessA). In EditorPlaytest: derive mod id by walking mission path upward for `mods/<id>/` pattern; if found set MC2_ACTIVE_MOD=<id> and MC2_DEBUG_STATE_DUMP=1 in child env, archive log under mods/<id>/.modproject/playtest/<ISO-timestamp>/playtest.log; else previous behavior. Verify mclib/file.cpp index walk skips `.modproject` dirs — add filter if absent (one line, engine-side, separate commit). Do not touch dirty WIP files. Build editor + game green; tier1 smoke if engine touched."

### Smoke: tier1 5/5 if file.cpp touched; editor build green; manual: playtest mission in mods/<id>/ → game log shows mod mounted (`MC2_LOG_FILE_RESOLVE=1` spot check).
### Rollback: revert slice commits; env additions additive.
### Risks: env block construction bugs (CreateProcessA needs full block, not deltas — must copy parent env); .modproject filter touching hot file.cpp path (tier1 gates).
### Stop: file.cpp filter causes any tier1 regression → drop engine commit, keep editor side.

---

## Slice 3 — Open Mod Project skeleton (QUEUED)

Goal: File > Open Mod Project picks `mods/<id>/` folder; editor session binds to mod (save default = `mods/<id>/data/missions/`); New Mod Project scaffolds `mod.json` + `data/missions/` + `.modproject/editor.json`.

### Inspect
- `editor/EditorData.cpp` save paths (1487/1520/1557, `saveMissionFitFileStuff:2494`) — parameterize base dir, default unchanged.
- `editor/ModPicker.cpp` — reuse scan; add project-bind state (active project != active runtime mod; same id usually).
- File dialogs in editor MFC (existing open/save dialogs as pattern).
- `mod.json` minimal schema: `{id, name, version, dependencies[]}` per mod-packaging doc.

### Opus prompt (queued)
"In nifty worktree: add Mod Project concept to editor. New EditorModProject.cpp/.h: state {root, id, loaded}. Menu: File>Open Mod Project (folder picker → validate mod.json or offer create), File>New Mod Project (name → scaffold mods/<id>/{mod.json, data/missions/, .modproject/editor.json}). When project active: EditorData save dialogs default to <root>/data/missions/; playtest (slice 2) uses project id for MC2_ACTIVE_MOD. Persist last project in .modproject/editor.json. NO engine changes. Editor build green; existing save/load smoke unaffected."

### Smoke: editor build + editor CLI smoke; manual: create project, save mission inside, playtest end-to-end loop.
### Rollback: revert; save-path default change is the only behavior touch — guard behind project-active.
### Risks: MFC dialog plumbing; save-path regressions for non-project users (guard: zero behavior change when no project open).
### Stop: EditorData save path parameterization needs refactor >100 lines → STOP, redesign as wrapper.

---

## Sequencing gate

Slice 1 → build+smoke pass → USER visual approval → Slice 2 → tier1 gate if engine touched → USER approval → Slice 3. One slice in flight at a time.

---

## Bridge v0 — mover.state (SHIPPED)

First runtime-bridge slice: the editor now shows LIVE mover state from a running playtest, over the existing stdout pipe (no sockets, no engine refactor). Ref: `docs/superpowers/strategy/runtime-bridge-architecture.md` Phase 3 §11.

### Game side (env-gated, zero cost off)
- `code/mission.cpp` `Mission::update()`: when `MC2_BRIDGE_MOVER_STATE=1`, emit a burst once per `MC2_BRIDGE_MOVER_PERIOD_SEC` seconds (default 2, min 0.25), throttled on `scenarioTime`. One `[MOVER v1 begin] t=.. count=..` header then up to 64 `[MOVER v1] ...` lines. Strictly read-only — only getters, every deref null-guarded. Off = one `getenv` at startup + a bool/float compare per frame.
- Fields per mover: `id` (partId), `name`, `team` (teamId), `pos=x,y,z`, `hp` (0..1 from `getDamageLevel`), `pilot` (warrior name), `order=val/name` (warrior status enum), `target` (pilot's current-target partId, or -1).

### Editor side
- `editor/EditorTaskRunner.{h,cpp}`: new optional `TaskSpec::onLineMainThread` callback. Worker queues raw lines under the mutex (bounded 4096, oldest-drop); `PumpMainThread()` drains + fires the callback on the MAIN thread — keeps the parser off the GL/MFC thread per the runner's threading contract.
- `editor/EditorPlaytest.{h,cpp}`: always sets `MC2_BRIDGE_MOVER_STATE=1` in the child env; registers a tolerant `[MOVER v1]` key=value parser. Burst-published ring (`s_burstMovers` accumulates, published to `s_liveMovers` on each `begin`). Accessors `LiveMovers()` + `LiveMoversStamp()`.
- `editor/GameplayDebugger.cpp`: when a playtest is Running and `LiveMovers()` is non-empty, renders a live table (id/name/team/hp/pilot/order/target) under a green "LIVE from playtest" banner; otherwise keeps the static-placement view (now with a "waiting for telemetry" hint while running).

### Verification
- Game + editor build exit 0; mc2.exe + editor deployed to v0.4 and 0.4c.
- tier1 5/5 (`--duration 30`).
- Playtest smoke (`run_editor_playtest_smoke.py`, torrin) green; archived playtest log contains `[MOVER v1]` lines (E2E bridge proof).
