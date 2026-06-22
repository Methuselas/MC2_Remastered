# SMOKE-FINGERPRINT-FULL-COVERAGE-1

**Type:** Python (deploy_payload refactor + run_smoke advisory). Behavior-preserving
by default. **Production-touch:** `scripts/deploy_payload.py`, `scripts/run_smoke.py`.

## Gap closed

The exe `[BUILD_FINGERPRINT v1]` check only covers the **exe sha**. A **stale
shader or PDB** in the deploy target (e.g. a shader edit deployed without the
matching exe → a NaN/garbage frame that still renders) was invisible unless a
caller explicitly passed `--verify-preflight`. So smoke could pass green on a
drifted tree (the stale-shader fake-green class).

## What changed

1. **`deploy_payload.staleness_report(target_dir)`** — a PURE function extracted
   from `verify_only`: reads `.deployed_manifest.csv` and re-hashes every listed
   file with the existing `sha256_file` (single-source — no reimplemented
   hashing), returning `{has_manifest, version_ok, ok, stale[], missing[],
   src_commit, manifest_path}`. `verify_only` now delegates to it — output
   identical (behavior-preserving; covered by `test_verify_only_still_delegates`
   and the existing deploy-coherence suite).

2. **`run_smoke` default `[DEPLOY_STALENESS]` advisory** — on every run, before
   missions, the runner reports whether the deploy target matches its manifest:
   - match → `[DEPLOY_STALENESS] OK: N files match manifest (src_commit=…)`
   - drift → `WARNING: X stale, Y missing vs manifest` + the first 20 paths
   - no/old manifest → an ADVISORY note
   Default is advisory (never fails — other sessions legitimately smoke partial
   or externally-built trees). `MC2_SMOKE_REQUIRE_FRESH=1` promotes drift to a
   hard pre-mission abort (exit 7), mirroring `MC2_SMOKE_REQUIRE_FINGERPRINT`.

## Why it's safe

- Default behavior is unchanged (advisory only; no new failure unless the opt-in
  env is set). The deploy_payload change is a pure extraction with identical
  `verify_only` output.
- Reuses `sha256_file` — the hashing stays single-source, per the recon's
  "do NOT reimplement hashing" rule.

## Verification

- `scripts/tests/test_deploy_staleness.py` — 6 pure tests: fresh / stale-shader /
  missing-file / no-manifest / bad-version / verify_only-delegation.
- Regression: `scripts/tests/` 29/29, `tests/smoke/` 71/71.
- Real runs (Python-only change, no rebuild):
  - v0.4 (my build) tier1 **5/5**, advisory `OK: 123 files match`.
  - releases/mc2-win64-v0.4d-rc1 mc2_01 **1/1**, advisory `OK: 123 files match`.

## Deferred (not in scope)

Making `--verify-preflight` a hard default for tier2/tier3 capture matrices — the
default advisory already surfaces drift on every run; promoting it to a matrix
gate is a separate, opt-in policy change.
