---
name: mc2-build-deploy
description: Build mc2.exe then deploy exe + shaders to runtime directory with verification — the full cycle
---

# MC2 Build + Deploy

The complete build-deploy-verify cycle in one command.

## Steps

1. **Invoke `/mc2-build`** — build mc2.exe in the current worktree
2. **If build succeeds, deploy via the canonical script** (one command per target):
```bash
py -3 "<worktree>/scripts/deploy_payload.py" --target game    # v0.4, mc2.exe
py -3 "<worktree>/scripts/deploy_payload.py" --target editor  # 0.4c, Mission Editor.exe (EditRel)
```
   The script verifies every copied file by sha256, writes the deploy manifest automatically, and HARD FAILS if the target exe is locked by a running process — close the game/editor yourself; it never taskkills. See `/mc2-deploy` for details, overrides, and the manual fallback recipe.
3. **If build fails, stop** — don't deploy a stale exe
4. **Staleness check anytime** (read-only): `py -3 "<worktree>/scripts/deploy_payload.py" --target game --verify-only`

This is the most common workflow. Use this instead of running build and deploy separately. Remember the two-target trap: game runs from v0.4, editor from 0.4c — deploy each separately.
