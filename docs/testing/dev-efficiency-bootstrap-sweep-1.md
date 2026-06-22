# DEV-EFFICIENCY-BOOTSTRAP-SWEEP-1

**Status:** RECON ONLY — no code. Identifies small repo-local tools that cut
repeated setup, duplicate work, stale-branch mistakes, wrong deploy targets,
harness build-dir mistakes, and oversized handoff/context reports.
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).

## Existing tooling to BUILD ON (not duplicate)
| Existing | Covers | Gap |
|---|---|---|
| `tools/repo_intel/repo_query.py` (`preflight`/`dirty`/`harness`/`env`/`binding`) | branch/root/dirty guard | NO slice-collision, NO base-drift, NO duplicate-slice detection |
| `scripts/bootstrap_worktree_build.py` (`--build`) | worktree cmake configure + 3rdparty donor-copy + build **mc2** | NO worktree create/prune/status, NO harness build, NO foreign-WIP guard |
| `scripts/deploy_payload.py` (`verify_only`, presets, sha256, fingerprint) | deploy + verify a tree | NO read-only "which target / is-it-stale / is-it-running" resolver |
| `scripts/smoke_lib/{gates,crash_evidence,logparse,report,fingerprint}.py` | pure verdict/evidence parsers | NO single artifact-dir summarizer composing them |
| `run_contract_tests.py` (explicit registry) | runs harnesses if built | does NOT build them into canonical dirs |

## Per-tool classification

| Tool | Verdict | Why |
|---|---|---|
| **slice_preflight** (collision + base-drift) | **GREEN** (extend repo_query) | the branch/root/dirty half exists; collision (`git log --grep`/`-G symbols`) + base-drift is NEW. Highest leverage. |
| **build_contract_harnesses** | **GREEN** | new, trivial; reads the real registry, builds canonical dirs, runs suite. Kills the build-dir-mistake class. |
| **make_handoff** | **GREEN** | new; composes git + test results into the standard report shape. Cuts the biggest context tax. |
| **smoke_artifact_summarize** | **GREEN** | composes existing smoke_lib pure modules (gates/crash_evidence/logparse) — no logic duplication. |
| **resolve_mc2_target** | **YELLOW** (reuse deploy_payload) | overlaps deploy_payload presets/verify/fingerprint; thin read-only resolver on top. |
| **slice_worktree** (create/status/prune) | **YELLOW** | bootstrap_worktree_build already does the hard build-config part; create/prune/guard is convenience around `git worktree`. |
| **base_drift** (standalone) | **RED — fold into slice_preflight** | it IS the drift half of slice_preflight; a separate tool duplicates it. |

## Ranked top 5 (leverage × cheapness × clean-dedup)

### 1. `tools/repo_intel/repo_query.py slice-preflight` — GREEN, **MANDATORY preflight rule**
Extend repo_query with a new subcommand (reuse its dirty/branch logic):
```
py -3 tools/repo_intel/repo_query.py slice-preflight \
  --slice WATCHID-LOAD-GUARD-1 \
  --symbols "watchSave,nextWatchID,getByWatchID" \
  --paths code/objmgr.cpp code/objmgr.h \
  --base <branch-base-sha>
```
Checks (PASS/WARN/STOP + short markdown): current nifty HEAD; `git log --grep <slice>` (already-landed?); `git log -G <symbols>` since base (someone fixed it?); dirty files overlapping `--paths`; base-stale vs nifty; similar-name commits.
**Eliminates:** the single most expensive repeated failure this arc —
rediscovering already-fixed bugs (objmgr OBJMGR-WATCHID/WATCHID-LOAD, fit-parse,
the icon divisor) and trusting stale-base recon. Folds in base_drift (#6).
**Est:** ~1.5h (subcommand + 5 git checks + md report). **Mandatory before any recon-derived fix slice.**

### 2. `tools/build_contract_harnesses.py [--run]` — GREEN
```
py -3 tools/build_contract_harnesses.py --run
```
Reads `run_contract_tests.py`'s `REGISTERED_HARNESSES` + a name→build-dir map (or
derive `build64-<short>` consistently), configures+builds each into its CANONICAL
dir (the dir the runner searches), then runs the suite.
**Eliminates:** the "MISSING because I built into build64-contract_smoke / forgot
glew unzip / forgot a worktree's build" class (hit ≥3× this arc). Also the
per-merge "build all harnesses to prove the runner green" dance.
**Est:** ~1h. Not mandatory, but the default way to green the suite.

### 3. `tools/make_handoff.py` — GREEN
```
py -3 tools/make_handoff.py --slice ICON-ATLAS-HARNESS-1 --since HEAD~3
```
Emits the standard block: commits, files changed, tests/smoke run + result,
what's proven / not proven, next recommended slice, branches/worktrees to prune.
**Eliminates:** the giant bespoke prose report every slice + inconsistent memory
updates. Feeds straight into the memory file + the final message.
**Est:** ~1.5h (git plumbing + a fixed template; optional smoke-result scrape).

### 4. `tools/smoke_artifact_summarize.py <artifact-dir>` — GREEN
```
py -3 tools/smoke_artifact_summarize.py tests/smoke/artifacts/<ts>
```
Composes `smoke_lib` (logparse→gates→crash_evidence): bucket, exit code, summary
present?, crash-handler hit?, minidump present?, heartbeat phase, last N log
lines, concurrent mc2.exe if captured, probable classification.
**Eliminates:** speculative flake-vs-bug triage (esp. crash_silent ambiguity).
Reuses the now-tested gates/oracleparse logic — no new verdict logic.
**Est:** ~1h (it's orchestration over existing modules).

### 5. `tools/resolve_mc2_target.py` — YELLOW (reuse deploy_payload)
```
py -3 tools/resolve_mc2_target.py --target game   # or --release-root <path>
```
Read-only: prints actual exe path, release tree, basename-derived config, build
payload source, git sha/fingerprint if present, is-running?, is-stale-vs-HEAD?.
Reuses deploy_payload presets + fingerprint parser; no deploy.
**Eliminates:** v0.4/v0.4c/v0.4d target confusion (bitten repeatedly).
**Est:** ~1h. **Recommend mandatory before smoke/deploy** once it exists.

## Not in top 5
- **slice_worktree** (YELLOW) — convenience over `git worktree` + bootstrap_worktree_build; useful but lower leverage than 1–5. Do later if create/prune churn persists.
- **base_drift** — folded into slice_preflight (#1).

## Mandatory-rule recommendations (after build)
- **slice_preflight** → required before any recon-derived FIX slice (the anti-rediscovery gate).
- **resolve_mc2_target** → required before smoke/deploy (the anti-wrong-tree gate).
The other three are default-use conveniences, not gates.

## Recommended implementation order
1. `slice_preflight` (biggest repeated cost; mandatory gate)
2. `build_contract_harnesses` (cheap, removes a recurring stumble)
3. `make_handoff` (cuts context tax every slice)
Then 4 (smoke summarize) and 5 (target resolver) as the smoke/deploy lane needs them.

All five are script-only (tools/ + scripts/), no production touch, no game launch.
