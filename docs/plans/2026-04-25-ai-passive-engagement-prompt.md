# Next-session prompt: AI units don't engage until fired upon

**Priority: high.** Blocks playable release.

Paste everything below this header into a fresh focused session.

---

## Symptom

In v0.2 of the OpenGL port:
- Enemy mechs/vehicles stand passive and only return fire after the
  player shoots them.
- Turrets don't engage at all.
- Mission 3 HQ convoy: only one or two units spawn at the map edge, they
  don't move, and the rest of the convoy (including the Mobile HQ that's
  the mission objective) never appears.

Reporter says this worked in v0.1. **It did not** — see "What's already
been ruled out" below; the user's recollection of v0.1 is the suspect
data point, not v0.2.

## What's already been ruled out (do not re-derive)

`mc2.exe`, `data/missions/corebrain.abx`, and `data/warriors/*.abl`
(all 8 files) are **byte-identical** between
`A:\Games\mc2-opengl\mc2-win64-v0.1.1\` and
`A:\Games\mc2-opengl\mc2-win64-v0.2\`. Confirmed via `cmp`. The
hotfix `release_assets/mc2-hotfix-v0.1.2.zip` re-ships those same
files, so deploying it on top of v0.2 is a no-op. The bug is **not in
the .abl/.abx data files** — they haven't changed in either direction.

The bug is engine-side.

## Strong primary lead

In `docs/observations/2026-04-24-tier2-visual-observations.md` lines
58-65, there's a note that the 51 Omnitech ABL extension functions
registered in commits `7c852e2` (40 functions) and `db8c00a` (11 tier-2
FSM primitives: `magicattack`, `patrol`, `guard`, `escort`, `core*`,
`setwillrequesthelp`, `tdebugstring`, `isdeadorfled`, `printteamstatus`)
are **registered as no-op stubs** — they make the VM not crash, but
they don't actually do anything. The observations doc explicitly says:

> "a separate ABL-integration task is already in flight that has the
> **real implementations** for all 51 missing extensions (sourced from
> old MC2 documentation — these are not no-op stubs but actual
> functions to port)"

Symptom-to-cause mapping:
- **Passive enemies** — proactive target acquisition runs through these
  ABL primitives → no-op → no contact registered → no engagement. The
  *reactive* "took damage, return fire" path is deeper engine code that
  doesn't go through the stubs, which is why it still works.
- **Turrets don't engage** — same target-selection primitives.
- **Mission 3 HQ convoy partial spawn** — convoy mission script issues
  ABL `patrol` / `magicattack` orders to spawned units; the orders go
  into a black hole, so units neither move nor spawn-chain into the
  Mobile HQ.

## Suspect list (per user)

1. **`data/warriors/*.abl`** — already confirmed byte-identical to v0.1.1,
   so unlikely to be the regression — but if any of these scripts call
   ABL primitives that are registered-but-stubbed, the script appears
   to work and silently does nothing. Worth dumping the symbol calls
   in each .abl and cross-referencing against the registration table.
2. **GameLogic / mission-script tick path** — wherever `corebrain.abx`
   is interpreted per-frame and dispatches into ABL extensions.
3. **`aiBrain` code path** — `code/aibrain.*`, `code/team.*`, anything
   that drives team-level decisions and calls into the warrior brains.

## Investigation plan

1. **Locate the in-flight ABL-integration branch.** The observations
   doc says it's already in progress. Likely names:
   - `claude/omnitech-abl` (the worktree at
     `.claude/worktrees/omnitech-abl/` is the canonical staging area —
     check its HEAD and any uncommitted work)
   - Search across all worktrees in `.claude/worktrees/` for any
     branch with recent commits touching `code/abl*` or
     `code/abl/extensions*` or whatever houses the 51 stub bodies.
   - Grep all worktrees for the function names `magicattack`,
     `setwillrequesthelp`, `tdebugstring`, `isdeadorfled` —
     non-trivial bodies (>1 line) indicate real implementations.
2. **Confirm the symptom maps to the stubs.** Add an env-gated trace
   `MC2_ABL_EXT_TRACE=1` that logs every call into the 51 stubs with
   caller frame info. Run mission 1 / mission 3 with the trace on.
   Expect to see thousands of calls per second to the proactive
   primitives. If you don't, the problem is upstream of the stubs
   (e.g., the brain isn't even getting to those calls).
3. **If the in-flight branch exists**, integrate it, build, deploy,
   verify. If it doesn't or it's incomplete, scope the porting work.
   Do not attempt to write the 51 implementations from scratch in this
   session — that's a multi-day effort. Land what's mergeable, file the
   rest.
4. **Smoke-test the fix.** Use the AI-firing canary backlog entry I
   added to `tests/smoke/README.md` as the acceptance gate. Real
   verification: load mission 3, watch enemies engage proactively
   without being shot, watch the convoy spawn fully and Mobile HQ
   reach its waypoint.

## Useful greps to start

```
grep -rn "enterStandardRoutine" .claude/worktrees/*/code/ | head
grep -rn "magicattack\|setwillrequesthelp" .claude/worktrees/*/code/
grep -n "Unimplemented feature" .claude/worktrees/*/code/abl*
```

Also: the printf debug commit `1337876 debug(abl): printf every
enterStandardRoutine registration to stdout` adds visibility into what
the engine *thinks* is registered. Run with that build (or rebuild
with the printf re-enabled if it was demoted) and capture the
registration table at startup.

## Memory references

- `omnitech_abl_stubs_session.md` — 40 ABL extensions registered, mc2x-import HEAD fc64ee2
- `omnitech_abl_missing_names.md` — 11 tier-2 FSM primitives committed db8c00a
- `carver5_mission_playable.md` — MAX_STANDARD_FUNCTIONS 256→512, 51 ABL stubs total

## Constraints

- Per worktree CLAUDE.md: build `--config RelWithDebInfo`, deploy via
  `cp -f` + `diff -q` (use `/mc2-deploy`), never `cp -r`.
- Land debug instrumentation env-gated per "Debug Instrumentation Rule"
  in worktree CLAUDE.md; demote-don't-delete after verification.
- The deploy target for testing is
  `A:\Games\mc2-opengl\mc2-win64-v0.2\`, NOT `mc2-win64-v0.1.1\` or
  the source-tree `run/` dir (memory: `feedback_deploy_path.md`).

## Out of scope for this session

- Cursor-jump-near-HUD bug
- Rebind-to-occupied-key crash (separate prompt:
  `2026-04-25-rebind-crash-prompt.md`)
- Paint scheme color picker swap (separate prompt:
  `2026-04-25-paint-scheme-prompt.md`)
- AAR text overflow at 1440p / 4K
- Encyclopedia 3D unit preview missing
- Thin line on right edge of HUD
- Non-AI gameplay regressions

## Done = ready to ship as v0.1.3 or v0.3

Mission 1: enemies engage proactively without being shot first.
Mission 3: full HQ convoy spawns, Mobile HQ moves to its waypoint, and
the mission can be completed. AI-firing canary added to smoke tier1.
Commit on `claude/nifty-mendeleev`.
