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

## Manifest truth (TWO-MANIFEST TRAP — read this)

There are two manifest files in a deploy target and they are NOT interchangeable:

- **`.deployed_manifest.csv` is the VERIFIER TRUTH** — the manifest consumed by
  `deploy_payload.py --verify-only`. This is the one a staleness gate checks.
- **`.deploy-manifest.json` is legacy/auxiliary** unless explicitly consumed
  elsewhere (e.g. `check-deploy-coherence.py`). It does NOT drive `--verify-only`.

Rule: **any out-of-band deploy path** that copies `mc2.exe` / `mc2.pdb` / shaders
into a target (manual `cp`, a one-off script, an editor-only copy) MUST refresh
the CSV via:

```bash
py -3 "<worktree>/scripts/deploy_payload.py" "<target>" \
    --source-root "<worktree>" --build-dir "<build>" --write-manifest-only
```

Do **NOT** "fix" a `--verify-only` STALE report by editing the JSON — that is the
wrong manifest. The CSV is what `--verify-only` re-hashes against; editing the JSON
changes nothing the verifier reads. (Verified 2026-06-12: append a byte to a
deployed `mc2.exe` → `--verify-only --strict` exits 6; `--write-manifest-only`
re-hashes in place → `--verify-only --strict` clean again.)

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

6. **Write BOTH deploy manifests** (LAST step, after all copies verified).
   There are two manifest files and both must be refreshed or one goes
   stale behind the other:
   - `.deploy-manifest.json` — advisory coherence manifest (carries git
     metadata; read by `check-deploy-coherence.py`).
   - `.deployed_manifest.csv` — the manifest `deploy_payload.py --verify-only`
     checks. A copy that skips this leaves `--verify-only` reporting STALE.

```bash
# JSON coherence manifest:
py -3 "<worktree>/scripts/write-deploy-manifest.py" "A:/Games/mc2-opengl/mc2-win64-v0.4" \
    mc2.exe mc2.pdb --merge --worktree "<worktree>" \
    --glob "*.dll" --glob "shaders/*.vert" --glob "shaders/*.frag" \
    --glob "shaders/*.tesc" --glob "shaders/*.tese" --glob "shaders/include/*"

# CSV manifest that --verify-only checks (re-hashes files already deployed;
# no copy). Without this the manual recipe leaves the CSV stale:
py -3 "<worktree>/scripts/deploy_payload.py" "A:/Games/mc2-opengl/mc2-win64-v0.4" \
    --source-root "<worktree>" --write-manifest-only
```
   If you deploy to BOTH v0.4 and 0.4c, write both manifests into EACH target.
   `scripts/deploy-editor.sh` already refreshes both manifests automatically.

   **Better: just use `py -3 scripts/deploy_payload.py --target game` —
   the canonical one-command path copies AND writes the CSV manifest in one
   shot, so it never drifts. The manual recipe above is a fallback only.**
