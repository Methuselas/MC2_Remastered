# Worktree discipline

Before editing ANY file, run:

```bash
sh scripts/check-worktree-context.sh
```

It tells you whether you are at the repo ROOT or inside a lane worktree, and warns
loudly (the stale-root-edit DANGER signature) when you are about to edit stale ROOT
copies while the active lane lives in `.claude/worktrees/<lane>/`. `--strict` exits 1
on the danger signature for use in hooks/CI.

NOTE: this 3-line "Before editing: run sh scripts/check-worktree-context.sh" reminder
should also be surfaced in the live lane CLAUDE.md
(`.claude/worktrees/nifty-mendeleev/CLAUDE.md`) — that file is untracked on this
branch (`.claude/worktrees/` is gitignored), so it must be added there manually.
