# PLAN — harness isolation meta-fix (HARNESS-ISOLATION-1)

Supersedes HANDOFF Step-1's "4 capture-infra bugs" framing. Recon (this session)
proves bugs **1, 3, 4 collapse to ONE root**: the capture harness has no
per-invocation isolation on two axes. Bug **2 is engine-side**, kept separate.

## ROOT CAUSE (recon-confirmed)
Two coarse "all-or-nothing" isolation gaps in `scripts/run_visual_capture.py`:

- **Axis A — process ownership** (`:79-89`). Binary: blanket `taskkill /F /IM mc2.exe`
  (kills FOREIGN) **or** `--no-kill` (kills NOTHING; own children leak). No "own only
  mine". → bug 1 (own child not reaped) + bug 4 (leftover OWN child contends next run
  → `present=False`). HANDOFF concedes bug4 "mostly consequence of (1)".
- **Axis B — output ownership** (`:507` vs `pipeline_visual_gate.py:114,124`). Multi-run
  writes `base_out.parent / f"{mission}_r{i}"`. Gate passes `base/"after"` and
  `base/"before"`; both `.parent` → `base` → both clobber `base/mc2_01_rN`. **Leaf
  dropped.** → bug 3. Side effect: `_row_drew` rglob (`pipeline_visual_gate.py:88`)
  scans `after/` but logs land at `base/mc2_01_rN/` → finds nothing.

## RECON FACTS (banked, verified this session)
- Launcher never spawns under capture: `gameosmain.cpp:759` skip-guard includes
  `MC2_SMOKE_SEED`, which capture always sets (`run_visual_capture.py:182`). Child =
  a SINGLE mc2.exe, no grandchildren → **PID-tracking is sufficient; no Job Object**.
- `_row_drew` is recursive rglob → tolerant of any new nesting; the out-dir fix also
  un-breaks it.
- 2nd consumer `scripts/run_visual.py` calls `run_one()` directly with prebuilt dirs;
  it does NOT use `main()`'s multi-run `:507` path → the fix is localized + safe.
- No other caller depends on the `.parent/_rN` layout.

## FIX (one isolation primitive, dependency-free, ~30 lines + 1 line)

### Part A — own-only-your-children (kills bug 1 + bug 4)
`run_visual_capture.py`:
1. Module-level `_ACTIVE: list[subprocess.Popen] = []`.
2. In `run_one`, after `Popen(...)`, append `proc` to `_ACTIVE`.
3. New `_reap_active()`: for each still-alive proc in `_ACTIVE`, `proc.terminate()`
   then short `wait`; on timeout `proc.kill()`. Clear the list. Register via `atexit`
   (next to the existing `_restore_cursor` atexit, `:471`).
4. Call `_reap_active()` at the START of `run_one` (reap MY prior children before the
   next launch) — replaces the role of the blanket kill WITHOUT touching foreign.
5. **Delete the global-kill branch** in `_kill_existing_mc2` (`:87-89`); keep only the
   `--no-kill`-style no-op log. Foreign mc2.exe becomes structurally untouchable;
   `--no-kill` is now the only behavior (flag retained as a deprecated no-op for
   callers, or dropped — pick at edit time; gate passes it today at `:70`).

### Part B — honor full out-dir (kills bug 3)
`run_visual_capture.py:507`: change
`out_i = base_out.parent / f"{args.mission}_r{i}"`
→ `out_i = base_out / f"r{i}"`.
Now gate `after/` + `before/` each own `after/r1..rN`, `before/r1..rN`. No collision;
`_row_drew` rglob finds the logs.

### Out of scope (separate slice/ticket) — bug 2
Single-pose bookmark silent no-fire (`engine_capture_fired=True, present=False`) is in
the engine bookmark/capture iterator (`gos_visual_capture.cpp` /
`MC2_VISUAL_BOOKMARK_CAPTURE`). **Harness guard meanwhile:** in `run_one` (or
`main`), if a bookmark file has <2 poses, WARN loudly (or `return` non-zero) until the
iterator is fixed. File a `HARNESS-SINGLEPOSE-2` engine ticket.

## ACCEPTANCE
- `pipeline_visual_gate.py --dry-run` still prints the plan, no launch.
- A real water capture (≥2 poses, routed build, single foreign-free run) reproduces
  sha `cb5a700ebd00dd0b`.
- No foreign mc2.exe killed (verify a concurrent `releases/mc2-win64-v0.5.0` instance
  survives a gate run — `Get-CimInstance Win32_Process -Filter "name='mc2.exe'"`).
- No leftover child mc2.exe after the run (`tasklist | grep mc2.exe` clean post-run).
- Cursor restored.
- Multi-run `after/` + `before/` write to distinct `rN` dirs; `_row_drew` > 0.

## DISCIPLINE
Isolated-worktree build only. Stage ONLY `scripts/run_visual_capture.py`,
`scripts/pipeline_visual_gate.py` (if flag touched), this doc, the HANDOFF update.
Verify foreign-WIP md5 unchanged (mech3d.cpp, txmmgr.h, ablmc2.cpp, golden-sets.json,
assimp_importer.cpp, mech_skel_import.cpp). The 4 pre-existing check-contracts fails
(env_registry, include_firewall, no_raw_gl_from_game, render_contract_gbuf1) are
foreign-WIP — ignore.

## SEQUENCE
1. Part A (process isolation) + Part B (out-dir) in `run_visual_capture.py`.
2. `--dry-run` gate sanity.
3. Foreign-free water capture → confirm sha `cb5a700e` → STEP 2 (water byte-A/B) is
   then immediate (BEFORE = revert water route per HANDOFF, build, deploy, capture).
4. File bug-2 engine ticket; add the ≥2-pose harness guard.
