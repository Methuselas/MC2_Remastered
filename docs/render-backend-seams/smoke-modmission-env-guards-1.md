# SMOKE-MODMISSION-ENV-GUARDS-1

**Branch:** `claude/smoke-modmission-env-guards`
**Worktree:** `A:/Games/mc2-smoke-modmission-env-guards`
**Date:** 2026-06-23
**Files changed:** `scripts/run_smoke.py`, `scripts/smoke_lib/manifest.py`, `scripts/smoke_lib/logparse.py`, `scripts/smoke_lib/gates.py`, `tests/smoke/smoke_missions.txt`
**No engine build/relink.**

---

## Root cause (diagnosed upstream)

A mod mission whose `.fit` cannot be resolved makes the engine call
`code/mission.cpp:2072 STOP("Unable to open Mission File")` → `std::abort()` →
exit `0xC0000409`. This bypasses the SEH crash-bundle, so the harness previously
bucketed it as `crash_silent` — visually identical to a real code crash. The
trigger is: `MC2_ACTIVE_MOD` wrong/unset, stale `mods/<mod>/.modindex-cache` (bakes
absolute mod root → copying a deploy breaks every path), or missing compat dep.

The manifest had NO structured `mod=` field; the mod name lived only in free-text
`reason=` (e.g. `"(set MC2_ACTIVE_MOD=MCO-ClanEagle)"` as a human hint).

---

## Guard 1 — MANIFEST SCHEMA (manifest.py + smoke_missions.txt)

Added two structured fields to `manifest.py Entry`:

| Field | Type | Meaning |
|-------|------|---------|
| `mod` | `str` | Exact `mods/<DirName>` for the mod this mission belongs to. Empty = stock. |
| `deps` | `str` | Comma-separated compat layer dir names (e.g. `mc2x-compat,mco-compat`). |

Both added to `VALID_KEYS`; parser fills them from `mod=` / `deps=` tokens.

Annotated rows in `smoke_missions.txt`:

| stem | mod | deps |
|------|-----|------|
| `poar_01` | `PicturesOfARebeliion` | `mc2x-compat` |
| `torrin`, `area16`, `coldstone` | `DarkRain` | `mc2x-compat` |
| `clearwater`, `ruins` | `TangoMaster` | `mc2x-compat` |
| `cfv2_mission1_escort` | `MCO-ClanEagle` | `mco-compat` |

Stock tier1/tier2 rows have no `mod=` and are unchanged.

---

## Guard 2 — AUTO-SET + VALIDATE (run_smoke.py `_modmission_env_guard`)

Called per-mission **before launch**, when `entry.mod` is non-empty.

**G1 — Auto-set:**
- Sets `MC2_ACTIVE_MOD=<entry.mod>` and `MC2_MOD_DEPS=<entry.deps>` in the child env.
- Does NOT depend on the caller having set them.
- If caller has them set AND they disagree with the manifest, **manifest wins** and
  a warning is logged: `[ENV-GUARD] WARNING: caller MC2_ACTIVE_MOD=... disagrees...`.

**G2 — Validate:**
- Resolves deploy dir = parent of `--exe`.
- Checks `mods/<mod>/` exists → else: clean single-line error + new bucket, no launch.
- Checks `mods/<mod>/data/missions/<stem>.fit` exists → else: same.
- Warns (does not abort) if any dep dir is absent.

Error format when `.fit` missing:
```
[runner] [ENV-GUARD] mission '<stem>' not found at <path> for mod=<mod> (deploy=<dir>) — check deploy/mod, NOT a code crash
```

New result bucket: **`env_mission_not_found`** (not `crash_silent`).
Exit code: 1 (nonzero), but no engine launched → immediate, no 30s wait.

---

## Guard 3 — STALE-CACHE (run_smoke.py `_modmission_env_guard`)

Before launch, deletes `mods/<mod>/.modindex-cache` AND each dep's `.modindex-cache`
in the deploy dir if present. The engine rebuilds it on next launch from the correct
absolute mod root.

Log line when cache existed:
```
[runner] [ENV-GUARD] cleared stale .modindex-cache for <mod>
```

This permanently kills the "copied deploy dir breaks mod path" class.

---

## Guard 4 — ABORT RECLASSIFICATION (logparse.py + gates.py)

`logparse.py`: new `MISSION_LOAD_FAIL_PATTERNS` regex (`Unable to open Mission File`,
`STOP\(.*Mission File`). Sets `LogSummary.mission_load_fail_seen=True` when matched
**before play phase** (`in_play_phase=False`).

`gates.py`: when `exit_code != 0` and no smoke summary, instead of always bucketing
`crash_silent`, now checks:

```
_load_phase_abort = s.mission_load_fail_seen AND s.mission_ready_ms is None
```

- Load phase + STOP line → **`mission_load_fail`** (not crash_silent)
- Mid-play (after `profile_ready`) → **`crash_silent`** unchanged

Detail: `"engine STOP(Unable to open Mission File) during load — check mod= / MC2_ACTIVE_MOD / .fit path, NOT a code crash"`

---

## New buckets

| Bucket | Meaning |
|--------|---------|
| `env_mission_not_found` | Pre-launch: `.fit` or mod dir absent in deploy. Engine never launched. |
| `mission_load_fail` | Post-launch: engine STOP'd in load phase with "Unable to open Mission File". |

---

## Test evidence

All tests run against `A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1`.

### POSITIVE (clearwater, cfv2_mission1_escort) — auto-set works end to end

```
[runner] [ENV-GUARD] cleared stale .modindex-cache for TangoMaster
[runner] [ENV-GUARD] cleared stale .modindex-cache for mc2x-compat
result=PASS (1/1 passed)  clearwater | PASS | ... | 4258 frames | Load 8323ms
```

```
[runner] [ENV-GUARD] cleared stale .modindex-cache for MCO-ClanEagle
[runner] [ENV-GUARD] cleared stale .modindex-cache for mco-compat
result=PASS (1/1 passed)  cfv2_mission1_escort | PASS | ... | 2770 frames | Load 11531ms
```

Neither run had `MC2_ACTIVE_MOD` set in the caller environment.

### NEGATIVE (bogus mod) — clean error, no engine launch

```python
e = Entry(tier='tier3', stem='clearwater', mod='BogusModXYZ', deps='mc2x-compat')
ok, env, err = _modmission_env_guard(e, deploy, {})
# ok=False
# err="[runner] [ENV-GUARD] mod dir not found: .../mods/BogusModXYZ ... NOT a code crash"
# bucket: env_mission_not_found
```

Fast (no 30s launch-into-abort). Distinct from crash_silent.

### STALE-CACHE auto-recover

Planted `TangoMaster/.modindex-cache` with junk content. Run clearwater:

```
[runner] [ENV-GUARD] cleared stale .modindex-cache for TangoMaster
[runner] [ENV-GUARD] cleared stale .modindex-cache for mc2x-compat
result=PASS (1/1 passed)  clearwater | PASS
```

### REGRESSION (mc2_01 stock) — unchanged

```
result=PASS (1/1 passed)  mc2_01 | PASS | ... | 1323 frames | Load 19105ms
```

No `mod=` on stock missions → guard no-ops, behavior identical to pre-patch.

### Unit tests

`tests/smoke/test_manifest.py + test_gates.py + test_logparse.py`: **23 passed**.

---

## Acceptance

- All 4 test cases pass.
- No engine code touched.
- Stock missions unaffected.
- `env_mission_not_found` / `mission_load_fail` are new distinct buckets, never
  produced on stock missions.
