# Deploy/Run Correctness Recon — Modernization Acceleration

**Date:** 2026-06-11 | **Recon Agent 1** | **Caveman-compressed**

## 1. Repeated Deploy/Run Mistakes (Incident Evidence)

### Mistake A: Stale EXE after deploy copy
- **Root:** Hung game process holds lock on exe; cp silently succeeds but doesn't overwrite
- **Citation:** docs/HANDOFF-model-override-lod.md:3; model-override-mvp-notes.md
- **Cost:** Full debug cycle wasted

### Mistake B: Stale shaders deployed
- **Root:** cp -r does NOT overwrite on Windows; cp -f required per file
- **Citation:** mc2-deploy.md:32,52; critical_inline_rules.md:32
- **Cost:** Hours false-path investigation

### Mistake C: Wrong deploy target
- **Root:** Three targets exist (v0.3, v0.4, 0.4c); no exe identity marker
- **Citation:** HANDOFF-model-override-lod.md:34
- **Cost:** Intermittent hard-to-reproduce failures

### Mistake D: Stale PDB
- **Root:** PDB not redeployed; Tracy symbols point to old worktree
- **Citation:** mc2-deploy.md:18-22
- **Cost:** Can't click through profiler

### Mistake E: Env vars silently OFF in smoke
- **Root:** subprocess.Popen env replacement; allowlist manual → new gates vanish
- **Citation:** run_smoke.py:304-460, line 351-360 comment
- **Cost:** Merged code with zero regression test of actual feature

### Mistake F: Stale .obj after inline change
- **Root:** MSBuild incremental ignores inline/template edits
- **Citation:** critical_inline_rules.md:32
- **Cost:** Days of investigation before realizing edit never landed

## 2. Prevention Checks

| Check | Gate | Implement |
|---|---|---|
| Exe lock | HARD | mc2-deploy.md Step 0: tasklist before cp |
| Shader diff | HARD | Add \|\| exit 1 to diff -q |
| Exe identity hash | HARD | Post-copy hash verify + fail |
| PDB deploy | HARD | Already done (mc2-deploy.md:21) |
| Env allowlist | ADVISORY | Pre-commit hook: grep new MC2_* vs run_smoke.py:305-460 |
| Clean relink | ADVISORY | --clean-first for load-bearing changes |

## 3. S8 Pack/Install vs Deploy Chip

**S8 scope:** Exe + PDB + shaders + DLL versions + asset marker + env allowlist snapshot

**Separate chip (future):** Manifest format + coherence script + file-resolve trace + registry index

## 4. Minimal Manifest Fields

deployed_version | exe_path | exe_hash_sha256 | pdb_hash | shader_count | dll_versions | env_allowlist_commit

**Where:** A:/Games/mc2-opengl/mc2-win64-v0.4/.deployed_manifest.csv (post-deploy)

## 5. run_smoke.py Env Allowlist Audit

> **CORRECTION (2026-06-11):** The specific MC2_GL_DEBUG_FATAL silent-drop claim was false. It is already in passthrough as of c8b7ac03. runner.py inherits parent env with os.environ.copy(). The allowlist check remains useful as convention/drift tooling, not proof that gates are currently dropped.

**Current:** run_smoke.py:305-460 (~155 vars, hardcoded list)

**Silent drops:** MC2_GL_DEBUG_FATAL (set line 189, not in allowlist); any new MC2_* gate not pre-listed

**Gap:** New feature + MC2_NEW_FEATURE_GATE=1 works locally (gate ON), fails PR (allowlist miss), merged with zero regression test

**Fix:** Pre-commit hook scripts/check-env-allowlist.sh — grep new MC2_* vs allowlist; error on miss

**OK in allowlist:** MC2_*_TRACE, MC2_TERRAIN_*, MC2_SHADOW_*, MC2_GPU_* etc. (~155 lines 306–460)

## Summary

**Highest impact:** Exe lock check + taskkill (blocks stale-exe). Exe identity manifest + hash (catches wrong version).

**S8 owns:** Manifest format + coherence script. Deploy-payload (file-resolve, registry index) → separate chip.

**Advisory:** Env allowlist pre-commit audit + clean relink automation.
