# Oracle Gate — Legacy Dynamic Pipeline (mech/vehicle/turret/effects)

Regression gate for the dynamic-object render pipeline work (slices S2 TG render-path,
S4 material→SSBO, S5 HZB dynamic cull, S6 gesture table). Establishes the **baseline** that
every later slice must not regress, and splits checks into **agent-checkable** (counter-based,
verifiable headless from smoke logs) vs **user-visual** (needs eyes / screenshots).

> Step 1 of the caveman orchestration plan. No S2+ implementation until this gate exists. ✅ it now does.

## Baseline refs

| Item | Value |
|------|-------|
| Branch | `claude/nifty-mendeleev` |
| HEAD commit | `43e22401` |
| Dirty-tree snapshot (dangling) | `fe4e55e4` (`git stash create`, non-destructive — captures the exact working tree incl. the load-bearing GpuMechBatcher LOD re-register fix in `mech3d.cpp::init`) |
| Baseline capture | `tests/smoke/artifacts/2026-06-09T19-27-36/` (mission mc2_01, 30s, PASS) |

**Why a snapshot, not clean HEAD:** clean HEAD lacks the uncommitted `mech3d.cpp` GpuMechBatcher
LOD re-registration fix → *invisible mechs on any map after the first session* → would poison the
mech-paint/faction oracle. Baseline MUST be the dirty tree. Reproduce it with
`git stash apply fe4e55e4` (or just compare against the live working tree).

## Capture command (canonical)

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --keep-logs
```

- **NEVER** `--kill-existing` (taskkills concurrent mc2.exe → false `crash_silent`; run_smoke holds its own lock).
- **NEVER** run concurrent with another smoke (tier1 or otherwise) — the lock will reject the second run; if you see a stale `tests/smoke/artifacts/smoke.lock`, verify the named PID is dead before removing.
- For broad coverage use the canonical tier1 gate instead (`--tier tier1`, 5 missions mc2_01/03/10/17/24).
- Artifacts land in a fresh `tests/smoke/artifacts/<timestamp>/` (`mc2_01.log`, `report.json`, `report.md`). **No screenshots** — this path emits counters/logs only.

## Agent-checkable gate (from `mc2_01.log` + `report.json`)

All values below are the mc2_01 baseline. A later slice **PASSES** only if every one still holds.

| Oracle item | Counter (log tag) | Baseline | Pass criterion |
|-------------|-------------------|----------|----------------|
| Mech paint / team color | `[MECH_MATERIAL_GPU v1] mechs=12 mismatches=0` | mismatches=0 | **mismatches == 0** (mission-independent — covers faction coloring too) |
| Material / texture bind | `[MATERIAL_GPU v4] emitted=136 mismatches=0` | mismatches=0 | **mismatches == 0** |
| Texture resolve | `[TEX_RESOLVE v1] mismatches=0 oob=0` | 0 / 0 | **mismatches == 0 AND oob == 0** |
| Render snapshot correctness | `[RENDER_SNAPSHOT v3] count_mismatch=0 pkt_mismatch=0 meta_mismatch=0 fallback=0` | all 0 | **all four == 0, using_snapshot=1** |
| Visibility validity | `[VISIBILITY v1] mech_valid=1 sp_valid=1 view_valid=1` | valid=1 | **all _valid flags == 1** |
| Terrain parity (unrelated, must not break) | `[TERRAIN_INDIRECT_PARITY v1] total_mismatches=0` | 0 | **total_mismatches == 0** |
| Effect draw counts | `[FX_COUNT v1]` (env `MC2_FX_COUNT_LOG=1`) | see below | counts in expected band, no site drops to 0 unexpectedly |
| Stability | `report.json destroys_delta` | 0 | **== 0**, result == PASS, buckets empty |
| FPS (informational) | `report.json avg_fps` | 80.9 (window minimized, ~100fps SDL cap — NOT GPU throughput) | not a hard gate; track for gross regressions |

### Effect-count oracle — `MC2_FX_COUNT_LOG`

Added in `mclib/mech3d.cpp` (env-gated, default OFF, zero behavior change when unset). Counts
draw attempts at the **11 GOSFX `->Draw()` sites** in `Mech3DAppearance::render()`. Emits a
`[FX_COUNT v1] event=summary ...` line to stderr every 9000 ticks.

```bash
MC2_FX_COUNT_LOG=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --keep-logs
```

11 sites: `rDust lDust jump lBoom rBoom critSmoke smoke wake heliDust rArmSmoke lArmSmoke`.

Emits two line types to **stdout** (NOT stderr — smoke logs capture stdout only):
- `[FX_COUNT v1] event=init flag=<0|1>` once at first `render()` (proves env reach + render runs).
- `[FX_COUNT v1] event=shutdown total=N rDust=.. lDust=.. ...` once at clean exit (atexit, mirrors `shutdownTexResolveTable`).

Implementation notes (load-bearing, learned the hard way):
- Must use `printf`/stdout, not `fprintf(stderr)` — the smoke harness captures stdout only.
- atexit is registered from inside `render()` (a guaranteed-run path), NOT a static-init object — an anonymous-namespace static with only a registration side effect can be elided by MSVC `/OPT:REF`.
- `MC2_FX_COUNT_LOG` had to be added to the `run_smoke.py` env-propagation whitelist (~line 285) — Popen replaces the child env, so non-whitelisted vars never reach `mc2.exe`.

**Baseline under smoke = all zeros, and that is expected:** the smoke missions are scripted
idle fly-throughs — player mechs do not walk, jump, or fire, so none of the 11 effect sites
trigger. Verified mc2_01 AND mc2_24 both → `total=0`. **The FX counter is therefore an oracle for
INTERACTIVE / combat sessions, not headless smoke.** To capture a non-zero effect baseline, run an
interactive game session (or a mission script that drives movement + weapons fire) with
`MC2_FX_COUNT_LOG=1` and read the `event=shutdown` line. The headless smoke value (all-zero) still
serves as a no-effect regression tripwire on the idle path.

## User-visual gate (NOT agent-checkable — needs eyes; smoke emits no screenshots)

Capture these by eye on the baseline before touching S2+, re-check after. No counter exists.

| # | Item | What to look for |
|---|------|------------------|
| 1 | Selection highlight | click a mech → correct ARGB highlight overlay; clears on deselect |
| 2 | Turret/barrel rotation | smooth interpolation to heading, no snapping/jitter |
| 3 | Effect *positions* (count is agent-checkable; placement is not) | weapon-fire muzzle origin, jump-jet at foot/leg nodes, dust at foot contact, wake at waterline |
| 4 | Faction color *appearance* (parity is agent-checkable; absolute correctness once) | IS vs Clan vs merc visually distinct + correct team colors |

## Headless limitations

- The smoke path produces **counters/logs only, no pixel screenshots** (the `water-diff` artifact set's PNGs were a special visual-diff run, not the default path). Agent cannot see pixels.
- `mc2-render-state` MCP `get_latest_artifact_paths` / `summarize_latest_capture` can point at a **stale** set — trust the on-disk newest `tests/smoke/artifacts/<timestamp>/` with a `report.json`.
- `run_capture_baseline` MCP tool self-caps at 150s; a 30s mission + build/launch overhead exceeds it and the underlying `run_smoke.py` + `mc2.exe` keep running orphaned after the MCP gives up. Prefer launching `run_smoke.py` directly via shell for captures.
- FPS from minimized-window runs reflects the SDL ~100fps minimized cap, not GPU throughput — never read it as a perf result.

## Hard rule

No S2 (TG render-path) / S4 / S5 / S6 implementation lands without re-running this gate and
confirming **all agent-checkable criteria hold** + the relevant user-visual items eyeballed.
