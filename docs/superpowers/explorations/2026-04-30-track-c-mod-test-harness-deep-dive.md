# Track C — Mod Test Harness & CI Deep Dive

**Date:** 2026-04-30
**Mode:** Design only. No code changes.
**Predecessors:**
- [`2026-04-30-track-c-lua-trampolines-and-tests.md`](2026-04-30-track-c-lua-trampolines-and-tests.md) — test mission checklist (the engine-side smoke battery for the binding layer itself).
- [`2026-04-30-track-c-lua-loading-lifecycle.md`](2026-04-30-track-c-lua-loading-lifecycle.md) — mod discovery, dep graph, load order, fail-closed semantics.
- [`2026-04-30-track-c-lua-sandbox-and-errors.md`](2026-04-30-track-c-lua-sandbox-and-errors.md) — VM isolation, error envelope, `pcall` boundaries.
- [`scripts/run_smoke.py`](../../../scripts/run_smoke.py) — the existing tier-1/2/3 desktop smoke runner.
- [`tests/smoke/README.md`](../../../tests/smoke/README.md) — fail buckets, baseline rules, canary limitations.

**Audience:** a modder with a non-trivial mod (new mech class, new market UI, new AI behavior) who needs a fast feedback loop today and a credible "still works on tomorrow's engine release" gate. Also: an engine engineer who wants community mods to break loudly *before* merge, not after release. This is the test-harness sibling of the trampolines/tests doc, which covers engine-internal binding tests; this doc covers **mod author tests**.

---

## §1. CLI surface for headless mod testing

The full invocation:

```
mc2.exe --test-mod=mymod \
        --mission=mc2_01 \
        --duration=10s \
        --output-log=mod_test.log \
        --capture-events \
        [--scenario=path/to/scenario.lua] \
        [--snapshot=expected.json] \
        [--update-snapshots] \
        [--no-render] \
        [--seed=0xC0FFEE] \
        [--budget-strict] \
        [--filter=pattern]
```

Flag-by-flag:

- `--test-mod=NAME` — boot, load **only** the named mod and its declared deps (lifecycle doc §3); mask every other installed mod. Implies `MC2_SMOKE_MODE=1`.
- `--mission=STEM` — direct-start mission (same semantics as tier-1). Required unless `--scenario` declares its own via `ctx.load_mission(stem)`.
- `--duration=10s` — wall-time cap, or `--duration=ticks:600` for tick count (preferred under `--no-render`, where wall-time is meaningless).
- `--output-log=PATH` — full engine log + structured `[MODTEST v1]` lines. The `[MODTEST v1]` schema is grep-stable and bumps version on format changes (matches `[INSTR v1]`/`[SMOKE v1]`/`[ASSET_SCALE v1]`).
- `--capture-events` — record every fired/observed event as `[MODTEST v1] event=NAME args={…}` in canonical JSON.
- `--scenario=PATH` — explicit scenario file. Else auto-discover `mods/NAME/tests/*.lua`.
- `--snapshot=PATH` — compare structured output to a checked-in snapshot (§3); mismatch → exit 2 + diff to stderr.
- `--update-snapshots` — overwrite the snapshot. Refuses on a dirty tree unless `--force` (same instinct as `--baseline-update`).
- `--no-render` — engine without GL/audio/display (§6).
- `--seed=N` — pin RNG (§7).
- `--budget-strict` — perf-budget assertions become failures, not warnings. Off in author loop, on in CI.
- `--filter=PATTERN` — run only matching scenario names (Lua pattern, like Busted).

**Exit codes** (modeled on `run_smoke.py`):
- `0` — all scenarios passed, no errors logged.
- `1` — at least one scenario failed (assertion, perf budget, snapshot mismatch).
- `2` — engine reported a hard error (crash, GL_ERROR, pool null, missing file).
- `3` — harness usage error (bad flag, mod not found, snapshot file missing without `--update-snapshots`).
- `4` — existing mc2.exe already running (matches `run_smoke.py:171`).

Engine starts, loads only the named mod and its deps, runs the mission (or scenario), then **exits cleanly**. Non-zero exit if any error logged. The `[exit] gos_TerminateApplication called` clean-exit marker from the menu canary path (`run_smoke.py:98`) is reused as the cleanliness check.

---

## §2. In-Lua test framework — `mc2.test`

A modder writes:

```lua
-- mods/mymod/tests/test_market.lua
local test = require("mc2.test")

test.scenario("market.buy_mech", function(ctx)
    ctx.spawn_mech("madcat", { x = 100, y = 100, team = 1 })
    ctx.fire_event("Market.Buy", "madcat")
    test.assert_eq(ctx.player.cash, 9000)        -- spent 1000 cr
    test.assert_eq(#ctx.team(1).mechs, 1)
end)

test.scenario("market.refund_on_undo", function(ctx)
    ctx.player.cash = 10000
    ctx.fire_event("Market.Buy", "madcat")
    ctx.fire_event("Market.Undo")
    test.assert_eq(ctx.player.cash, 10000)
end)
```

### `mc2.test` API

- `test.scenario(name, [opts,] fn)` — register a scenario. `opts`: `budget_ms = N` (§9), `mode = "no-render" | "render"` (default `no-render`, §6), `tags = {"slow","ui"}` (filterable).
- `test.before(fn)` / `test.after(fn)` — per-scenario hooks (pytest fixture semantics, not Busted's describe-scope). Rationale: §8's fresh-VM-per-scenario rule means file-scoped before/after only makes sense as "runs in the new VM before the body."
- `test.assert_eq(actual, expected, [msg])` — deep-equal for tables. Diff is structured: `expected={x=1,y=2}, actual={x=1,y=3}, diff={y: 2 != 3}`.
- `test.assert_true` / `assert_false` / `assert_lt` / `assert_lte` / `assert_gt` / `assert_gte` / `assert_near(a, b, eps)`.
- `test.assert_throws(fn, [pattern])` — borrowed from Busted's `assert.has.errors`.
- `test.assert_event(name, [matcher])` — was fired since last call or scenario start; matcher is predicate or partial-table.
- `test.skip([reason])` / `test.fail(msg)`.

### The `ctx` object

`ctx` is a sandboxed test harness that wraps the live engine state. It exposes:

- **Spawning:** `ctx.spawn_mech(type, opts)`, `spawn_vehicle`, `spawn_object`. Auto-cleaned at scenario end.
- **Time:** `ctx.tick()` (one engine tick); `ctx.advance_time(seconds)` deterministic; `ctx.advance_until(predicate, max_seconds)`.
- **Events:** `ctx.fire_event(name, ...)`, `ctx.events_since(t)`.
- **State:** `ctx.player`, `ctx.team(N)`, `ctx.mission`.
- **Mission:** `ctx.load_mission(stem)`, `ctx.set_objective_status(id, status)`.
- **Mocking:** `ctx.mock("mc2.market.buy", fn)` for one scenario only; auto-restored. Shape borrowed from pytest's `monkeypatch`.

`ctx` is **not** the global `mc2` module — the modder still calls `mc2.market.buy()` in the scenario. `ctx` is the *test* surface; `mc2` is the *runtime* surface. Separation prevents production code from reaching for test helpers.

---

## §3. Snapshot testing

For mods that produce structured output (a UI render tree, an audio queue, a mission log, an objective progression), snapshots beat assertion-by-assertion checking. Pattern:

1. The scenario runs and emits structured events via `ctx.snapshot.add(name, payload)` or implicitly via `--capture-events`.
2. At scenario end, the harness collects all recorded events into a deterministic JSON document.
3. The document is compared to `mods/MYMOD/tests/snapshots/SCENARIO_NAME.json`.
4. Mismatch → fail with a structured diff (line-level + JSON-path-level).
5. To accept a new snapshot: `mc2.exe --test-mod=mymod --update-snapshots`.

### Snapshot file format

```json
{
  "schema": "mc2-modtest-snapshot-v1",
  "mod": "mymod",
  "scenario": "market.buy_mech",
  "engine_version": "0.2.0",
  "engine_commit": "d5b5b4e",
  "seed": "0xC0FFEE",
  "events": [
    {"t": 0,    "name": "Market.Buy",    "args": {"chassis": "madcat"}},
    {"t": 0,    "name": "Player.Spent",  "args": {"amount": 1000, "balance_after": 9000}},
    {"t": 0,    "name": "Mech.Spawned",  "args": {"team": 1, "type": "madcat"}}
  ],
  "final_state": {
    "player.cash": 9000,
    "team[1].mech_count": 1
  }
}
```

**Determinism rules for serialization** (load-bearing — without these, snapshots diff for non-semantic reasons):

- Object keys sorted lexicographically.
- Floats formatted to 7 significant digits (`%.14g` is noisy across builds).
- Events ordered by tick index, then registration order within tick.
- `engine_version` / `engine_commit` are recorded but **not** part of the diff key — informational.
- World-position floats quantized to `1e-3` before compare; physics noise otherwise creates spurious diffs.

Schema bumps (`v1` → `v2`) follow the `[INSTR v1]` rule: bump on incompatible change, no shims.

---

## §4. `run_smoke.py` integration

New tier: `mod-smoke`. Borrows tier-1/2/3 plumbing.

```
py -3 scripts/run_smoke.py --tier mod-smoke
py -3 scripts/run_smoke.py --tier mod-smoke --mod mymod
py -3 scripts/run_smoke.py --tier mod-smoke --update-snapshots
```

### Discovery + manifest

Each mod ships `mods/MYMOD/tests/manifest.json`:

```json
{
  "schema": "mc2-modtest-manifest-v1",
  "mod": "mymod",
  "min_engine_version": "0.2.0",
  "scenarios": [
    {"file": "test_market.lua", "tags": ["fast"]},
    {"file": "test_ai_pathing.lua", "tags": ["slow", "perf"], "budget_ms": 5}
  ]
}
```

The engine doesn't strictly need this — it can glob `tests/*.lua` and let `test.scenario` self-register — but the manifest lets the smoke runner read per-mod metadata without spawning the engine.

### Pseudocode

```python
# scripts/run_smoke.py — new tier branch
if args.tier == "mod-smoke":
    mods_root = ROOT / "data" / "mods"   # or wherever mods live; lifecycle doc §2
    mods = [m for m in mods_root.iterdir()
            if (m / "tests" / "manifest.json").exists()]
    if args.mod:
        mods = [m for m in mods if m.name in set(args.mod)]

    rows = []
    for mod in mods:
        manifest = json.loads((mod / "tests" / "manifest.json").read_text())
        cfg = ModRunConfig(
            exe=[args.exe],
            mod=mod.name,
            mission=manifest.get("default_mission", "mc2_01"),
            duration_ticks=manifest.get("default_duration_ticks", 600),
            no_render=True,            # mod-smoke is fast tier
            seed="0xC0FFEE",
            update_snapshots=args.update_snapshots,
        )
        result = run_mod_one(cfg)      # parallel sibling of run_one()
        rows.append(report.ModRow(mod=mod.name,
                                   verdict=result.verdict,
                                   scenarios_pass=result.pass_count,
                                   scenarios_fail=result.fail_count,
                                   failed_names=result.failed))
        if args.fail_fast and not result.verdict.passed:
            break

    # Aggregate "12/14 mods pass, 2 failures listed below"
    md = report.render_mod_smoke_markdown(rows, ...)
    sys.exit(0 if all(r.verdict.passed for r in rows) else 1)
```

Report mimics tier-1/2 markdown. One artifact dir per run, `<mod>.log` per failing mod, plus `report.md` / `report.json`. Single-process-at-a-time by default (matches `_running_mc2()` guard at `run_smoke.py:38-51`); `--no-render` makes parallelizing across cores a future option.

---

## §5. CI patterns

Three distinct CI shapes, each useful, each cheap on its own:

### 5.1 Per-mod CI (modder owns it)

A modder ships their mod with `.github/workflows/test.yml`:

```yaml
name: mc2-mod-tests
on: [push, pull_request]
jobs:
  test:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - name: Download MC2 binary
        run: |
          curl -LO https://github.com/USER/mc2-opengl/releases/download/v0.2.0/mc2-headless.zip
          7z x mc2-headless.zip -ohelp
      - name: Run mod tests
        run: |
          cp -r . help/data/mods/mymod
          help/mc2.exe --test-mod=mymod --mission=mc2_01 --no-render --duration=ticks:600
```

The engine ships a headless build artifact (exe + minimal data, `--no-render` capable). This is the most important deliverable for community trust: a modder drops their mod into a fresh CI runner and gets a green check or useful failure log within a minute.

### 5.2 Engine-side CI (engine owns it)

The existing engine CI gains a `mod-smoke` job that runs against a curated bundle of stock mods checked into the repo at `data/mods/_bundled/`:

```yaml
- name: Mod-smoke gate
  run: py scripts/run_smoke.py --tier mod-smoke
```

This is the analog of tier-1 — fast, hand-curated, runs on every PR. New contributor adds a mod to `_bundled/`? CI auto-tests it next push.

### 5.3 Compatibility-matrix CI (separate periodic job)

A nightly (or weekly) job pulls the top-N community mods from a known registry (e.g. a checked-in `mods.csv` with `name,git_url,ref` columns), runs `mc2.exe --test-mod=NAME` against each, and posts a status report:

```
=== Compatibility matrix — engine d5b5b4e ===
PASS  cve-g           (12/12 scenarios)
PASS  omnitech        ( 8/ 8 scenarios)
FAIL  magic-expansion ( 5/ 7 scenarios — see attached log)
SKIP  wolfman         (manifest missing)
```

When the matrix turns red, an engine engineer sees "this commit broke 3 community mods" before release, not after Discord lights up. Mods don't need to live in the engine repo — the matrix clones at pinned ref. (Matches the spirit of `public_fork_and_release.md` and the `mc2x_integration_attempt` / `mco_omnitech_integration_attempt` notes — those handoffs are exactly the regression class this catches automatically.)

---

## §6. Mock vs real engine — `--no-render` mode

Most mod tests don't need pixels. They need: Lua VM, game logic, event bus, mission state, AI, pathfinding, sim. `--no-render` brings up the engine *without* GL context, audio, window, or display.

Stripped: SDL window, GL context, shader compile, FBOs; audio output (events still fire, mixer no-ops); tessellation, shadow maps, post-process — renderer short-circuits at the `Environment::doFrame()` boundary; frame timing — loop runs unbounded.

Kept: ObjectManager, MissionManager, AI, pathfinding, ABL VM, Lua VM, event bus, asset loaders.

Implementation: a `gosRenderMode` enum (`Full`, `Headless`) checked at every renderer entry — not a `#ifdef` (same binary serves both modes; build flavors would double CI cost). Same shape as the existing `g_useGpuStaticProps` killswitch: small flag, top-of-function early-out.

UI mods need `mode = "render"`. Those run via the screen-coord menu-canary path (desktop-bound), only for opt-in scenarios.

---

## §7. Determinism

For a test to be a regression gate, it must produce the same answer twice. Three knobs:

1. **RNG seed pinned** via `--seed=N`. Engine `Fastfile` RNG, AI decision RNG, and Lua's `math.random()` all read from one seedable stream. Each Lua VM is seeded with `--seed` xor a per-scenario salt. Partly in place: `MC2_SMOKE_SEED=0xC0FFEE` at `run_smoke.py:230`.
2. **`ctx.advance_time(s)` not wall-clock.** Harness drives `gameSystemManager->update(dt)` with fixed 1/30s ticks. No `Sleep()`, no frame-rate dependence.
3. **Lua 5.4 in IEEE-754 mode is sufficient for v1.** No SSE intrinsics or fast-math. Cross-platform MP requires bit-exact engine sim replay — **out of scope for v1**; single-player mod test on dev box + same-arch CI is enough for snapshot stability.

Anti-determinism gotchas to forbid: `os.time/clock` (use `ctx.now()` returning ticks-since-start); `io.*` outside the snapshot dir (use `ctx.fs.*` mocked FS); network (refused at sandbox layer per sandbox doc §4).

---

## §8. Test isolation

Each scenario starts with a **fresh Lua VM**:

- New `lua_State` per scenario (sub-millisecond on Lua 5.4).
- Mod's `init.lua` runs first.
- Test file runs to register scenarios; only the matching scenario body is invoked.
- `before` → body → `after` (always, even on error, in pcall) → VM destroyed.

No shared state between scenarios. Stricter than Busted's default (which reuses the VM between `it` blocks), matching pytest `--forked`. Rationale: scenario A leaving state behind makes scenario B spuriously fail; modders should debug N failures, not 1.

Engine state (ObjectManager, mission) — two flavors:

- **`mode = "no-render"` + `mission = "synthetic"`** — minimal mission stub (terrain, no objectives, empty teams). ~50ms startup.
- **`mode = "render"` or real mission stem** — re-load mission per scenario by default; manifest can opt into `share_mission = true` for tag-grouped runs, with `before`/`after` resetting state. Default is per-scenario reload: slow-and-correct beats fast-and-flaky.

`before`/`after` run in the scenario VM and can call `ctx.*`. No cross-scenario shared globals; use closures over `local fixture = {}` if setup repeats.

---

## §9. Performance regression detection

Modders annotate hot scenarios with budget assertions:

```lua
test.scenario("ai.pathfind.large_map", { budget_ms = 5 }, function(ctx)
    ctx.spawn_mech("madcat", { x = 0, y = 0, team = 1 })
    ctx.set_destination("madcat", { x = 4000, y = 4000 })
    ctx.advance_until(function() return ctx.team(1).mechs[1].at_destination end, 30)
end)
```

Measured: scenario wall-time (excluding fixture); per-binding total time (every `mc2.*` call wrapped via trampolines doc §5 trace hooks, aggregated as `sum(time_in_binding)`); engine tick count (for `--no-render` where wall-time is meaningless).

On overrun:
- `--budget-strict` off (author loop): `[MODTEST v1] event=budget_warn scenario=… ms=12 budget=5`. Exit unchanged.
- `--budget-strict` on (CI): `event=budget_fail`. Exit `1`.

This catches "I added a feature and broke perf" regressions that snapshots can't see. Reported next to snapshot diff in `report.md`: `mod-smoke: 14/14 pass; ai.pathfind 12ms (budget 5ms, +140%)`.

Tight budgets fluctuate on noisy CI. Mitigation: budgets set **2× typical** (modder runs locally 5x, takes max, doubles). Matrix CI also reports 7-day rolling p95 so a single noisy run doesn't fail the gate — borrowed from Spring's `recoil-engine` perf-CI stripe.

---

## §10. Best-practice borrows (with citations)

- **Busted (Lua test framework)** — <https://lunarmodules.github.io/busted/>. Borrowed: `assert.has.errors` (→ `assert_throws`), `--filter` flag, top-level test file convention. **Not borrowed:** the `describe/it` nesting (rejected because it pushes modders toward Behavior-Driven test names that don't grep well; flat `scenario("name", fn)` wins for log readability) and the cross-`it` VM reuse (rejected on isolation grounds, §8).
- **pytest** — fixture/parametrize patterns. Borrowed: `monkeypatch` (→ `ctx.mock`), `--forked`-style per-scenario isolation, the artifact-as-html-report habit. **Parametrization** would be a nice v2 addition (`test.scenario_each({...inputs...}, fn)`); deferred from v1 to keep the API small.
- **Factorio mod tests** — <https://wiki.factorio.com/Tutorial:Mod_settings> and the `factorio-data` repo organization (<https://github.com/wube/factorio-data>). Factorio doesn't ship a public test framework per se, but the *folder layout* (mod root has `info.json`, `data.lua`, `control.lua`, `tests/`) is widely understood by modders and is what we mirror with `manifest.json` + `tests/`. Their `factorio --benchmark <save> --benchmark-ticks N` is the spiritual ancestor of `--no-render --duration=ticks:N`.
- **`run_smoke.py` itself** — `RunConfig` shape, `_running_mc2` guard, fail-fast / continue-on-fail flags, baseline-update guards, artifact-dir-per-run convention. Mod-smoke is grafted onto exactly this skeleton; reuse is the point.
- **Tracy** — `gos_profiler.h` zones already exist (worktree CLAUDE.md "Profiling" section). Per-binding time accounting in §9 reuses these zones rather than adding a parallel timing system.

---

## §11. Failure modes

Match `tests/smoke/README.md` "Fail buckets" style — every mode has a name, a diagnosis hint, and a stable log marker.

- **`assertion_fail`** — `test.assert_*` failed. `[MODTEST v1] event=assert_fail scenario=NAME line=N expected=X actual=Y`. Exit `1`.
- **`snapshot_mismatch`** — captured events/final state diverged. JSON-path-keyed structured diff. Exit `1`. Re-run with `--update-snapshots` if intended.
- **`budget_fail`** — perf budget exceeded with `--budget-strict`. §9. Profile with Tracy; per-binding aggregate narrows it.
- **`engine_crash`** — engine exited non-zero or threw. Wraps engine subprocess in the `run_smoke.py:66-115` crash-detection pattern. Reported as containing scenario's failure. Exit `2`.
- **`engine_gl_error` / `engine_pool_null` / `engine_asset_oob`** — inherited from tier-1 instrumentation; no new code path. Exit `2`.
- **`scenario_timeout`** — didn't complete within `--duration`. Harness kills engine via `taskkill /F`. Exit `1`.
- **`no_scenarios_discovered`** — engine + mod loaded, but `test.scenario(...)` never called. Cause: typo in manifest, `mc2.test` not required, all scenarios filtered. Exit `3` — usage error, surfaced loudly so a modder doesn't mistake it for a green run.
- **`mod_load_fail`** — mod's `init.lua` errored (sandbox doc § error envelope). Exit `2`.
- **`unknown_mod`** — `--test-mod=NAME` matches no installed mod. Exit `3`.

Every fail bucket has a one-line `[MODTEST v1]` summary AND a structured artifact (diff/log/trace) on disk.

---

## §12. Open questions

1. **Where do `data/mods/` live in the deploy?** The lifecycle doc proposes `data/mods/`; the deploy at `A:/Games/mc2-opengl/mc2-win64-v0.2/` doesn't have such a dir today. Needs to land alongside or before this harness — otherwise `--test-mod` has nowhere to look.
2. **Mod manifest discovery vs. registration.** Should `manifest.json` be authoritative (engine refuses to load tests without it) or hint-only (engine globs `tests/*.lua`)? Authoritative is cleaner; hint-only is friendlier to ad-hoc local exploration. Lean: hint-only by default, authoritative under `--strict`.
3. **Snapshot format: JSON vs. JSON Lines.** A long event stream as one big JSON array is hostile to git diffs; one event per line is great for diffing but breaks deterministic key sorting at the document level. Likely answer: pretty-printed JSON with one event per array entry on its own line, sorted keys. Decide before first snapshot lands.
4. **Per-mod CI artifact distribution.** Releasing a "headless mc2.exe" artifact for community CI implies a stable distribution channel and a versioning policy. Coordinate with `public_fork_and_release.md` — the headless build is plausibly the *only* binary public CI ever needs.
5. **Cross-mod interaction tests.** v1 runs each mod in isolation. Real-world mods conflict (Magic + Wolfman); a "mod combinations" tier should exist eventually. Probably falls under the compatibility-matrix CI as a second axis (`for combo in itertools.combinations(mods, 2)`), not as a per-mod author concern.
6. **Render-mode scenarios in CI.** `--no-render` is CI-safe; `mode = "render"` scenarios are not (no display). Either (a) ship them as opt-in local-only, or (b) stand up a virtual display in CI (`Xvfb` on Linux runners, no clean Windows analog). v1: local-only with a clear warning in the report.
7. **Determinism of AI/pathfinding.** Some MC2 AI uses elapsed-time fields rather than tick counters. Audit needed before AI tests can rely on `ctx.advance_time` being equivalent to wall-clock-equivalent behavior.
8. **`ctx` API surface size.** The §2 `ctx` list is what *I think* a modder needs. The real list comes from writing 5 real test scenarios for actual mods (CVE-G, Omnitech, mymarket) and seeing what's missing. v1 ships the list above; v2 grows by demand, not speculation.

