# Execution prompt — GL Hygiene Drop-Ins

**Paste this into a fresh session to execute.** Self-contained briefing.

---

You are executing the **GL Hygiene Drop-Ins** slice of the MC3 rendering modernization. Three small, independent OpenGL feature adoptions bundled into one commit set. Pre-Track-A1 priority — ships first.

## Worktree

`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. All paths below are relative.

## Required reading (in order)

1. `CLAUDE.md` — worktree rules (build with `--config RelWithDebInfo`; deploy via `cp -f`; documentation discipline).
2. `docs/superpowers/plans/2026-05-06-gl-hygiene-dropins.md` — the plan itself. Tasks 1-4. **This is your spec.**
3. `memory/camera_model_oblique_cinematic.md` — explains why anisotropic filter is high-priority for this camera (oblique 30°, NOT top-down).

## Skill to invoke

Use **`superpowers:subagent-driven-development`** for task-by-task execution. Three tasks are independent — fresh subagent per task is appropriate, or batch all three inline since they're small. Your call based on session capacity.

## Scope summary

- **Task 1:** `glMaxShaderCompilerThreadsARB` at engine init (one-line ARB extension call). Fastest startup wins; no-op fallback.
- **Task 2:** Anisotropic filter on terrain textures (oblique camera high-impact). Scope: terrain textures only first — expand to objects in a follow-up if smoke clean.
- **Task 3:** `glPolygonOffsetClamp` audit. Capture before-baseline screenshots of shadow scenes; only replace if visual baseline shows acne/peter-panning. Skip the replacement if shadows already clean.
- **Task 4:** Memory file + MEMORY.md index entry.

## Gates

- Each task can land in a separate commit. No atomicity requirement across tasks (unlike clip-control).
- Tier1 5/5 PASS after each task is the regression gate. Visual smoke for Task 2 (terrain detail at oblique angle) is the hero check.
- For 15-second visual canaries: `py -3 scripts/run_smoke.py --mission mc2_01 --duration 15 --kill-existing`. Full tier1 only at slice close.

## Coordination

- **No cross-track gates.** Independent of A1/A2/B/C.
- Watch for working-tree state: there may be uncommitted changes from other sessions touching `gameos_graphics.cpp` or shaders. Check `git status` first; coordinate or wait if conflicts surface.
- Build verified clean as of HEAD `4c8f9a4` (clip-control adoption). Can ship right now.

## When blocked

- **Build broken at HEAD before you start:** check `git log` — there may be a half-applied commit. Document the broken commit's SHA and stop; do NOT attempt to fix unrelated breakage.
- **AF visual difference imperceptible at standard zoom:** still ship (Task 2 success criterion explicitly accepts "no visible difference at typical scenes"). The win is real even when subtle, and oblique cinematic shots are where it matters most.
- **Polygon-offset-clamp baseline shows clean shadows:** SKIP Task 3 entirely. Don't fix what isn't broken. Document baseline-clean in the memory file and move on.
- **Tier1 fails on any task:** revert the task's commit; do NOT investigate further unless you have a clear hypothesis. Hand back to user.

## Deliverable

Two-three commits (Task 1 alone, Task 2 alone, optional Task 3) plus memory file at end. Build clean, tier1 5/5 PASS, visual canary clean for Task 2.

## What's already shipped (don't re-do)

- `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` — committed `4c8f9a4` 2026-05-06. Hardware depth convention is now D3D-native; full `[0, 1]` window depth precision.

After this slice ships, hand back to user. Track A1 is the next slice.
