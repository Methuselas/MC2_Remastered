---
name: mc2-deploy
description: Deploy mc2.exe and all shaders from current worktree to runtime directory, with diff verification
---

# MC2 Deploy

Deploy the built exe + PDB + ffmpeg DLLs + all shaders from the current worktree to a runtime install, with per-file hash verification and a deploy manifest.

**Canonical path: `scripts/deploy_payload.py`.** One command per target. It hard-fails on every documented stale-deploy trap (locked exe, silently-not-overwritten shaders, stale PDB, wrong target) and writes `<target>/.deployed_manifest.csv` automatically.

## One-command deploy

```bash
# Game install (A:/Games/mc2-opengl/mc2-win64-v0.4, mc2.exe, build64/RelWithDebInfo):
py -3 "<worktree>/scripts/deploy_payload.py" --target game

# Editor install (A:/Games/mc2-opengl/mc2-win64-0.4c, "Mission Editor.exe",
# build64/out/editor/RelWithDebInfo — the EditRel lane):
py -3 "<worktree>/scripts/deploy_payload.py" --target editor
```

Presets only set defaults — an explicit positional target dir, `--build-dir`, or `--exe-name` always overrides (e.g. deploy game exe to a scratch install: `py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-s4b-scratch --exe-name mc2.exe`).

## What the script guarantees

- **Lock refusal, never taskkill**: if the target exe is held by a running process, the deploy HARD FAILS (exit 2) and tells you to close the game/editor yourself. It will never kill processes.
- **Per-file copy + post-copy sha256 diff**: every file is copied individually (`cp -r` stale-shader trap eliminated) and re-hashed; any mismatch is a hard fail.
- **PDB freshness**: deployed PDB must hash-match the build's PDB (stale PDB = wrong Tracy symbols). Override only with `--allow-stale-pdb`.
- **Manifest written automatically** as the last step: `<target>/.deployed_manifest.csv` (relpath, sha256, bytes, src_commit, timestamp).
- **Refuses to create target dirs** (wrong-target trap): target must already exist.

## Staleness check (read-only)

```bash
py -3 "<worktree>/scripts/deploy_payload.py" --target game --verify-only
py -3 "<worktree>/scripts/deploy_payload.py" --target editor --verify-only
```

Re-hashes the target against its manifest, reports `N match, N stale, N missing`. No copying. If the target has no manifest yet (never deployed via this tool) it reports that gracefully and exits 0. Add `--strict` to make drift / missing manifest a nonzero exit (for gating).

## Extras not covered by deploy_payload.py

- **Mod tools** (gosFX effect tools, if built with `-DENABLE_MC2FX=ON -DENABLE_MC2FX_PREVIEW=ON`): still deploy via `bash "<worktree>/scripts/deploy-mc2fx-tools.sh"` (targets both v0.4 and 0.4c by default; override with `DEPLOY=...`).
- **Editor overlay assets** (Buildings.csv, esplash.bmp, tacsplash.bmp): `scripts/deploy-editor.sh` still owns those; `deploy_payload.py --target editor` covers exe/PDB/shaders.

## Critical Rules

- **NEVER taskkill** to free a locked exe — close the game/editor manually and re-run.
- Deploy target traps: game runs from **v0.4**, editor from **0.4c**. Deploying to one does NOT update the other — run the script once per target.
- Verify deployed exe mtime/manifest commit ≥ your fix commit before re-testing a "fixed" bug.

---

## Appendix: manual per-file recipe (FALLBACK ONLY)

Use only if `deploy_payload.py` is unavailable. This knowledge is still valid — it is what the script automates.

### Paths
- **Source worktree**: Auto-detect from CWD (or `A:/Games/mc2-opengl-src`)
- **Deploy target**: `A:/Games/mc2-opengl/mc2-win64-v0.4`

### Steps

1. **Detect worktree**: Same logic as mc2-build — find worktree root from CWD.

2. **Deploy exe + PDB** (PDB carries source paths; Tracy shows the wrong worktree if it's stale):
```bash
cp -f "<worktree>/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
cp -f "<worktree>/build64/RelWithDebInfo/mc2.pdb" "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.pdb"
```

3. **Deploy FFmpeg DLLs** (copy each DLL individually using `cp -f`, NEVER `cp -r`):
```bash
for dll in avcodec-61.dll avformat-61.dll avutil-59.dll swscale-8.dll swresample-5.dll; do
    cp -f "<worktree>/build64/RelWithDebInfo/$dll" "A:/Games/mc2-opengl/mc2-win64-v0.4/$dll"
    diff -q "<worktree>/build64/RelWithDebInfo/$dll" "A:/Games/mc2-opengl/mc2-win64-v0.4/$dll"
done
```

4. **Deploy shaders**: Copy EVERY shader file individually using `cp -f` (NEVER `cp -r` — it silently fails to overwrite on Windows/MSYS2):
   - All `*.vert`, `*.frag`, `*.tesc`, `*.tese` from `<worktree>/shaders/`
   - All files from `<worktree>/shaders/include/` to deploy `shaders/include/`
   - Create `shaders/include/` in deploy target if it doesn't exist

5. **Verify ALL deployed files**: Run `diff -q` on every DLL and shader file (source vs deployed). Report:
   - Files that were deployed successfully (match confirmed)
   - **ANY mismatches — flag loudly as errors**
   - Files that exist in source but not in deploy (new files that need copying)
   - Files that exist in deploy but not in source (stale files from old branches)

6. **Write the deploy-coherence manifest** (LAST step, after all copies verified):
```bash
py -3 "<worktree>/scripts/write-deploy-manifest.py" "A:/Games/mc2-opengl/mc2-win64-v0.4" \
    mc2.exe mc2.pdb --merge --worktree "<worktree>" \
    --glob "*.dll" --glob "shaders/*.vert" --glob "shaders/*.frag" \
    --glob "shaders/*.tesc" --glob "shaders/*.tese" --glob "shaders/include/*"
```
   If you deploy to BOTH v0.4 and 0.4c, write a manifest into EACH target.
   `scripts/deploy-editor.sh` already writes its own entry automatically.
