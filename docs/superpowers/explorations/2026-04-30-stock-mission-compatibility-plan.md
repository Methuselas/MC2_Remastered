# Stock-Mission Compatibility Test Plan

**Date:** 2026-04-30
**Status:** Exploration / design only. No code lands from this doc.
**Audience:** engine engineers landing Tracks C/D/E/F; release maintainer.
**Predecessors:**
- [`memory/stock_install_must_remain_playable.md`](../../../../../../memory/stock_install_must_remain_playable.md) — the architectural rule this plan operationalizes.
- [`scripts/run_smoke.py`](../../../scripts/run_smoke.py) and [`tests/smoke/README.md`](../../../tests/smoke/README.md) — the existing tier1/2/3 backbone.
- [`docs/superpowers/explorations/2026-04-30-track-c-mod-test-harness-deep-dive.md`](2026-04-30-track-c-mod-test-harness-deep-dive.md) — the `mod-smoke` tier and `--test-mod` CLI we extend.
- [`docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §2 sidecar principle, §6 outcome gates.
- [`docs/superpowers/specs/2026-04-30-track-f-scalable-hierarchical-ai-design.md`](../specs/2026-04-30-track-f-scalable-hierarchical-ai-design.md) §11 stock-compatibility section, F-8 gate.

## Framing

Tracks C (Lua VM), D (Assimp mech import), E (JSON manifests), and F (hierarchical AI) each touch hot paths that stock missions exercise on every load. The current tier-1 smoke catches "engine doesn't crash and FPS doesn't tank with stock inputs and no mods loaded." It does **not** catch the cross-product that the modder-paradise roadmap creates: *stock mission, with Lua VM initialized; stock mission, with the JSON loader registered; stock mission, with brain registry populated but no mod-bound team; stock mission, with an empty `mods/` directory present; stock mission, with a no-op mod active.*

The stock-install rule says missing modern data must **degrade**, not **fail**. Tracks C–F invert that risk: the modern infrastructure is *always present in the binary* once shipped, and the question becomes "does the modern infrastructure correctly stay out of the way when no mod is bound?" That's the gap this plan fills.

---

## §1 — The Compatibility Matrix

Rows = environment state. Columns = the five tier-1 missions. Cells = expected verdict + which layer (defined in §3) executes the cell.

| Env-state \ Mission                              | mc2_01 | mc2_03 | mc2_10 | mc2_17 | mc2_24 | Layer | Existing coverage? |
|--------------------------------------------------|--------|--------|--------|--------|--------|-------|---------------------|
| **A.** Stock binary, no `mods/` dir              | PASS   | PASS   | PASS   | PASS   | PASS   | L1    | YES (current tier1) |
| **B.** Stock binary, empty `mods/` dir           | PASS   | PASS   | PASS   | PASS   | PASS   | L2    | NO (gap)            |
| **C.** Lua VM init, no mod loaded                | PASS   | PASS   | PASS   | PASS   | PASS   | L2    | NO (gap)            |
| **D.** `_compat_test` no-op mod active           | PASS   | PASS   | PASS   | PASS   | PASS   | L3    | NO (gap)            |
| **E.** `_compat_test` w/ event handler bound     | PASS   | PASS   | PASS   | PASS   | PASS   | L4    | NO (gap)            |
| **F.** Lua AI registered, **no team bound**      | PASS   | PASS   | PASS   | PASS   | PASS   | L5    | NO (Track-F F-8)    |
| **G.** Lua AI registered, **stock team bound**   | DEGRADE| DEGRADE| DEGRADE| DEGRADE| DEGRADE| L5    | NO (Track-F F-8)    |
| **H.** Multi-mod (3 no-ops) loaded               | PASS   | PASS   | PASS   | PASS   | PASS   | L6    | NO (gap)            |
| **I.** Assimp loader present, stock `.ase` only  | PASS   | PASS   | PASS   | PASS   | PASS   | L2    | NO (Track-D)        |
| **J.** JSON manifest loader present, no `.json`  | PASS   | PASS   | PASS   | PASS   | PASS   | L2    | NO (Track-E)        |
| **K.** ImGui bridge ON, no mod UI                | PASS   | PASS   | PASS   | PASS   | PASS   | L3    | NO (Track-B sibling)|
| **L.** Thin-record fastpath ON                   | PASS   | PASS   | PASS   | PASS   | PASS   | L1    | PARTIAL (env passthrough fix landed) |

**Legend.**
- **PASS** = identical to row A: same destroys count ±0, FPS within budget (§6), no `[GL_ERROR]`/`[TGL_POOL]`/`asset_oob` markers.
- **DEGRADE** = cell G is the *only* cell where stock is expected to deviate from row A (a stock team that has been opted into Lua AI is no longer running stock ABL). It is included to demonstrate the test harness can distinguish "intended deviation" from "regression"; it asserts the deviation matches the spec'd Lua AI behavior, not row A.

**Cell count:** 12 rows × 5 missions = 60 cells. Existing tier-1 covers row A only (5/60 = 8%). All other cells are gaps this plan closes.

---

## §2 — Per-Track Regression Risks

### Track C — Lua VM

- **VM init time inflates mission_load_start.** The `mc2x-import` and `Carver5O` work showed that subsystems initialized inside `InitializeGameEngine` cascade into mission load; a 200ms Lua VM warm-up adds 200ms to every stock mission.
- **Sandbox overhead even when no mod loaded.** If `pcall` boundaries wrap every event dispatch unconditionally, stock missions pay event-bus tax for capability they aren't using.
- **ABL stack pollution.** If Lua's reverse-direction event dispatch (Track-C reverse-direction doc) fires `MissionBegin`/`MechSpawned` events on stock missions when no listeners are bound, the event allocator + bus traversal cost is paid regardless. Assertion: empty event bus traversal is free at the dispatch site.
- **Tests that catch:** Layer 2 (empty-mod) compares mission_load_ms and steady-state FPS to row A baseline. Layer 4 (event handler bound) confirms that *adding* a no-op handler still costs <2% — exposes O(N²) dispatch bugs.

### Track D — Assimp / mech import

- **`TG_TypeMultiShape` allocation paths for stock `.ase`.** Adding a `.glb` codepath to the mech loader risks regressing `.ase` parsing (shared header decoder, shared material-ref resolution). Stock mechs are the bulk of every tier-1 mission; even a 1% per-shape allocation regression compounds.
- **Loader registry ordering.** If the Assimp registrar inserts itself ahead of the legacy `.ase` loader, the legacy path may stop being reached for `.ase` files when both can technically claim the extension.
- **Tests that catch:** Layer 1 with `--track-d-loader-active=1` env gate confirms loader presence ≠ behavior change. Compare destroys count and `[TGL_POOL] summary` line bytewise vs. baseline.

### Track E — JSON manifests

- **Loader presence affecting stock data load timing.** Adding a JSON manifest scanner that walks `mods/*/data/` on every boot adds I/O even when no mods exist. Filesystem walks on cold cache (CI fresh runner) are notoriously slow on Windows.
- **JSON parse errors as mission-load failures.** A typo in *one* mod's `mod.json` must never block stock missions from loading. Fail-closed on the offending mod, fail-open on the engine.
- **Tests that catch:** Layer 2 compares mission_load_ms with empty `mods/` to row A. Layer 6 includes one deliberately-malformed `mod.json` and asserts stock missions still pass while the bad mod is reported as failed-load.

### Track F — Hierarchical AI

- **`brainsEnabled[]` gate must default to ABL for stock teams.** Per Track-F §11 and the F-8 gate, the legacy ABL path runs unmodified when no mod registers a Lua brain for a team. A bug here means stock teams silently get an empty/no-op brain → mechs stand still.
- **Brain registry populated but unbound.** Even with the registry populated, stock teams must dispatch to ABL. The `brainsEnabled[teamId]` check at the per-warrior `brain->execute()` site is load-bearing.
- **Sensor batching / threat heatmap producers.** §11 calls out 1Hz heatmap generation as always-on infrastructure. Confirm this is gated `if (anyTeamUsesLuaBrain)` — otherwise stock missions pay heatmap CPU cost for nothing.
- **Tests that catch:** Layer 5 explicitly tests cells F (registered, unbound) and G (registered, bound to one stock team). Layer 5 asserts cell F's destroys count and FPS are byte-identical to row A; cell G is allowed to differ but is captured for the spec'd-deviation snapshot.

---

## §3 — Test Layers

Each layer runs the same five tier-1 missions and compares verdicts. Each layer adds *one new dimension* over the prior. CLI invocations assume `cd $WORKTREE`.

### Layer 1 — Stock-only smoke (existing)

The current tier-1 gate. Engine boots with no `mods/` directory present, no Lua, no manifest scanner, no Assimp loader registered.

```
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

**Exercises:** baseline. Establishes the reference numbers everything else compares to.

### Layer 2 — Empty-mod / loader-present smoke

Engine boots with `mods/` directory present but empty (no subdirs). All Track-C/D/E loaders are registered and initialized. No Lua chunks load, no JSON manifests parse, no `.glb` mechs imported.

```
py -3 scripts/run_smoke.py --tier tier1 --compat-tier 2 --kill-existing
```

**Exercises beyond Layer 1:** Lua VM warm-up cost, JSON-manifest filesystem scan on empty dir, Assimp registrar presence, brain-registry init. Surfaces "loader-present overhead" regressions.

### Layer 3 — Test-mod smoke

Bundled `mods/_compat_test/` (§4) with no-op `data.lua` and `control.lua` is loaded. No event handlers bound; the mod is *present and active* but does nothing.

```
py -3 scripts/run_smoke.py --tier tier1 --compat-tier 3 --kill-existing
```

**Exercises beyond Layer 2:** the full `mods/` ingestion path runs (manifest parse, dep graph, sandbox VM creation, `data.lua` execution, `control.lua` registration). Confirms the *act* of having an active mod doesn't regress stock.

### Layer 4 — Mod-active smoke (event handler bound)

Same `_compat_test` mod, but with a `control.lua` that registers a no-op counter on every event type the engine fires (`Mission.Begin`, `Mech.Spawned`, `Mech.Damaged`, `Mech.Destroyed`, `Tick`, ...). The handler increments a Lua local and returns; no engine state mutation.

```
py -3 scripts/run_smoke.py --tier tier1 --compat-tier 4 --kill-existing
```

**Exercises beyond Layer 3:** reverse-direction event dispatch overhead — every event the stock engine fires now crosses the C++→Lua boundary. Surfaces `pcall` cost, marshalling overhead, dispatch-table O(N) scans. Catches the "events get expensive when there are listeners" regression class.

### Layer 5 — AI-replacement smoke

Two sub-cells:
- **5a — registered-unbound:** Lua brain registry has one brain (`_compat_test/brains/null.lua` returning `Hold`); no team is bound to it. Stock teams run ABL. Asserts cell F of the matrix.
- **5b — registered-bound:** the same null brain bound to team 0. Stock teams 1+ run ABL. Asserts cell G.

```
py -3 scripts/run_smoke.py --tier tier1 --compat-tier 5a --kill-existing
py -3 scripts/run_smoke.py --tier tier1 --compat-tier 5b --kill-existing
```

**Exercises beyond Layer 4:** the Track-F F-8 gate. Layer 5a is the load-bearing assertion: stock missions with the Lua AI infrastructure *present* must be indistinguishable from Layer 1. Layer 5b confirms the engine doesn't crash when AI is genuinely replaced — no PASS/FAIL on gameplay outcome, just on engine stability.

### Layer 6 — Multi-mod smoke

Three test mods loaded: `_compat_test`, `_compat_test_b` (declares dep on `_compat_test`), `_compat_test_c` (declares conflicting version range with `_compat_test`). The engine resolves load order, refuses `_c`, and runs `a` + `b`.

```
py -3 scripts/run_smoke.py --tier tier1 --compat-tier 6 --kill-existing
```

**Exercises beyond Layer 5:** load-order resolution, dep-graph cycles, conflict detection, per-mod sandbox isolation. Asserts that one bad mod doesn't poison the others or stock.

---

## §4 — Bundled Test Mod Design

Lives at `mods/_compat_test/`, ships in-tree, never in release zips for end users. Modders never write something like this; it's an engine-side regression artifact, like a `tests/fixtures/` file.

```
mods/_compat_test/
  mod.json
  scripts/data.lua
  scripts/control.lua
  brains/null.lua            # used by Layer 5
  tests/manifest.json        # mod-smoke discovery (per Track-C harness doc)
```

**`mod.json`:**
```json
{
  "id": "_compat_test",
  "name": "Engine compatibility smoke harness",
  "version": "0.0.1",
  "mc2_api_version": 1,
  "depends": { "mc2": ">=0.2.0" },
  "entrypoints": {
    "data":    "scripts/data.lua",
    "control": "scripts/control.lua"
  },
  "internal": true
}
```

The `internal: true` field marks it as engine-shipped; the mod manager hides it from any user-facing mod list.

**`scripts/data.lua`** — empty body, just exists to prove the data-stage runs:
```lua
return {}
```

**`scripts/control.lua`** — Layer-3 default is also empty. Layer 4 swaps in a counter variant via env: `MC2_COMPAT_TEST_BIND_HANDLERS=1`:
```lua
if os.getenv("MC2_COMPAT_TEST_BIND_HANDLERS") then
    local n = 0
    for _, evt in ipairs(mc2.events.list()) do
        mc2.events.on(evt, function() n = n + 1 end)
    end
end
```

**`brains/null.lua`** — Layer 5 brain that always returns `Hold`:
```lua
return function(_lance) return { type = "Hold", priority = 50 } end
```

Mods three different paths through the harness with three small files; no engine-side "test mode" flag is needed beyond the env vars Layer-4/5 toggle.

---

## §5 — `run_smoke.py` Extensions

New CLI flags. Pseudocode showing how they integrate with existing `--tier`:

```python
ap.add_argument("--compat-tier", choices=["1","2","3","4","5a","5b","6"])
ap.add_argument("--compat-full", action="store_true")
ap.add_argument("--compat-baseline", action="store_true")
```

Behavior:
- `--compat-tier N` selects one of the §3 layers. The chosen layer determines env vars and active mods, then runs the same five tier-1 missions.
- `--compat-full` runs layers 1, 2, 3, 4, 5a, 5b, 6 sequentially; emits one combined report; exits nonzero if any layer fails.
- `--compat-baseline` diffs each layer's per-mission destroys / FPS against `tests/smoke/compat_baselines.json`, applying the §6 budgets.

Layer dispatch (sketch added inside `main()` after the existing tier branch):
```python
COMPAT_LAYER_ENV = {
    "1":  {},  # row A — no mods/, no Lua
    "2":  {"MC2_COMPAT_LAYER": "2", "MC2_MODS_ENABLE_EMPTY_DIR": "1"},
    "3":  {"MC2_COMPAT_LAYER": "3", "MC2_MODS_ACTIVE": "_compat_test"},
    "4":  {"MC2_COMPAT_LAYER": "4", "MC2_MODS_ACTIVE": "_compat_test",
           "MC2_COMPAT_TEST_BIND_HANDLERS": "1"},
    "5a": {"MC2_COMPAT_LAYER": "5a", "MC2_MODS_ACTIVE": "_compat_test",
           "MC2_LUA_BRAIN_REGISTERED": "1"},
    "5b": {"MC2_COMPAT_LAYER": "5b", "MC2_MODS_ACTIVE": "_compat_test",
           "MC2_LUA_BRAIN_REGISTERED": "1",
           "MC2_LUA_BRAIN_BIND_TEAM": "0"},
    "6":  {"MC2_COMPAT_LAYER": "6",
           "MC2_MODS_ACTIVE": "_compat_test,_compat_test_b,_compat_test_c"},
}
```

`RunConfig.env_extra` already accepts arbitrary env passthrough (see existing `MC2_MODERN_TERRAIN_*` block); this slots in. Reuse `_running_mc2()` guard, artifact-dir convention, baseline-update flag.

**Relationship to `--tier mod-smoke`** (from the Track-C harness doc): `mod-smoke` runs *modder authored* tests of mod logic; `--compat-tier` runs the *engine's stock-mission* tests with mods loaded. Different audiences, same backend. Both can be run together via:
```
py -3 scripts/run_smoke.py --compat-full --tier mod-smoke
```

---

## §6 — Performance Regression Guards

A stock mission's frame time must not regress beyond a per-layer budget. Numbers below are budgets *relative to Layer 1's per-mission baseline*; the absolute Layer-1 numbers come from `tests/smoke/baselines.json` and rotate via `--baseline-update`.

| Layer | What's running                                  | Steady-state FPS budget | mission_load_ms budget | destroys delta |
|-------|--------------------------------------------------|-------------------------|------------------------|----------------|
| 1     | Stock                                            | baseline (reference)    | baseline (reference)   | 0              |
| 2     | Empty mods/, all loaders init                    | ≥99% of baseline (-1%)  | +50 ms absolute        | 0              |
| 3     | `_compat_test` active, no handlers               | ≥98% of baseline (-2%)  | +100 ms absolute       | 0              |
| 4     | `_compat_test` w/ handlers on every event        | ≥95% of baseline (-5%)  | +100 ms absolute       | 0              |
| 5a    | Lua AI registered, unbound                       | ≥99% of baseline (-1%)  | +50 ms absolute        | 0              |
| 5b    | Lua AI bound to team 0                           | n/a (deviation expected)| +50 ms absolute        | snapshotted    |
| 6     | 3 mods, one rejected                             | ≥97% of baseline (-3%)  | +200 ms absolute       | 0              |

The runner asserts these by extending the existing `baselines.destroys_delta` logic to a `compat_perf_delta` checker that reads per-layer thresholds from `tests/smoke/compat_budgets.json`. Failure mode: report row marks the cell red with `perf_regression: layer=4 mission=mc2_17 fps=137 baseline=145 budget=-5% actual=-5.5%`.

The "destroys delta = 0" rule is the strongest signal: if a stock mission destroys a different number of objects with mods loaded vs. without, gameplay diverged. Cells F and 5a must hit zero exactly; G and 5b are exempt because gameplay is intentionally different.

---

## §7 — CI Integration

### PR gates (run on every push)
- Layer 1 (stock tier-1 smoke) — already exists.
- Layer 2 (empty-mod) — cheap, ~5 minutes added.
- Layer 3 (no-op mod active) — also cheap.

These three are the contract: if you're touching Track-C/D/E/F code, your PR either keeps Layers 1–3 green or it gets blocked.

### Nightly (cron-scheduled)
- Layer 4 (handler-bound).
- Layer 5a + 5b (AI registration / binding).
- Layer 6 (multi-mod).
- The Track-C-harness compatibility-matrix CI (top-N community mods, separate doc §5.3).

Nightly runtime budget: ~45 minutes total. Acceptable because failures here surface within 24 hours and don't gate individual commits.

### Failure surfacing
- GitHub Actions output (or whichever CI lives at the time): per-layer summary at the top of each job, full `report.md` as a build artifact.
- A persistent `BROKEN.md` artifact in the repo root (auto-rewritten by nightly): lists which layers/missions failed on the most recent main commit, with timestamp and commit SHA. Manual; if nightly is green the file says `# All compat layers green at <commit>`.
- For the community-mod compat matrix, a Discord webhook notification when a previously-green community mod goes red. (Aligns with the Track-C deep-dive's "Discord lights up after release" anti-goal.)

---

## §8 — Memory Entries to Update

After this plan ships:

1. **New memory file** — `compat_test_layers.md`. One-liner: "6-layer stock-mission compat gate, what each layer asserts, where bundled `_compat_test` mod lives." Links: this doc, `run_smoke.py`, `mods/_compat_test/`.
2. **Update** `stock_install_must_remain_playable.md` — append a "How to verify" subsection pointing to `--compat-full`.
3. **Update** the worktree `CLAUDE.md` "Smoke Gate" section — mention `--compat-tier 1..6` flag exists and Layers 1–3 are PR gates.
4. **Update** `MEMORY.md` index — add `compat_test_layers.md` under a new "## Test infrastructure" section (the existing "Workflow / feedback" section is for human-process notes).
5. **No update** to the per-track memory files (Track C / D / E / F don't exist as memories yet); their eventual memory entries should cite this plan.

---

## §9 — Deferred from v1

Explicit non-scope, with rationale:

- **MP determinism.** Out of scope until MP exists. Track-C harness §7 explicitly defers cross-platform bit-exact determinism; same applies here.
- **Cross-OS compatibility.** Windows-only for v1. No Linux/Mac runners; the engine only ships Win64 binaries today.
- **Specific community-mod regression catalogs.** Modders own their own mods' tests via the Track-C `mod-smoke` tier. The compat-matrix CI from the Track-C deep-dive covers community-mod *load-and-boot*; per-mod gameplay correctness is the modder's responsibility.
- **GPU driver coverage.** Tests run against whichever GPU the runner has. AMD-specific quirks (the RX 7900 XTX issues per `docs/amd-driver-rules.md`) are caught by the existing tier-1; this plan doesn't add per-vendor matrix axes.
- **Asset upscaler interactions.** The `art_4x_gpu/` and `tgl_4x_gpu/` paths are tested by tier-1 indirectly. This plan does not add explicit "with upscaled assets, with mod active" cells; if a regression appears, add a cell then.
- **MC2X / MCO content imports.** Those are covered by the manual `mc2x-import` and `omnitech-abl` worktree handoffs. Automating those is separately tracked in the Track-C harness §5.3 community-mod matrix.
- **Editor / Methuselas Editor integration.** Editor builds are versioned independently (`0.14.0-editor`). Editor compatibility with mods is out of scope for v1.
- **Tier-2 / tier-3 with mods loaded.** Only tier-1 missions get the cross-product. Tier-2 (24-mission stress) with all six layers would be 144 mission-runs, and tier-2 perf numbers are already non-comparable per `tests/smoke/README.md`. If a tier-1 layer passes and a tier-2 mission fails *only* with mods loaded, treat that as a separate investigation.
- **`--no-render` mode.** The Track-C harness uses `--no-render` for fast modder-authored tests. This plan stays in render mode; we're testing that *rendering* stock missions works with mods loaded, which is the user-visible regression class.

---

## §10 — Open Questions

1. **Where do `mods/` live?** Track-C lifecycle proposes `data/mods/`; the deploy at `A:/Games/mc2-opengl/mc2-win64-v0.2/` doesn't have one yet. This plan assumes whatever location lifecycle settles on; the runner reads it from a single constant.
2. **Should Layer 5b assert *anything* on gameplay?** Right now it only asserts "engine doesn't crash." A stronger version would snapshot the team-0 mech positions at t=10s and compare on subsequent runs. Risk: AI determinism (Track-F §3 contracts haven't been pinned for replay determinism yet).
3. **Bundled mod versioning.** `_compat_test/mod.json` declares `mc2_api_version: 1`. When the API bumps to v2, the bundled mod has to bump too — and the *previous* version of the bundled mod has to keep working against the *new* engine, since otherwise we're testing "mod author kept up" not "engine kept compat." Likely answer: ship `_compat_test_v1/`, `_compat_test_v2/`, run both. Decide before v2.
4. **Layer 4 event coverage completeness.** The handler binds to "every event the engine fires." The list is grown by Track C and Track F. Mechanism for keeping the list current: engine emits `[EVENT_REGISTRY v1] event=X` at boot, the harness greps the log to confirm it bound a handler to each one. If a new event lands without coverage, Layer 4 reports a soft warning.
5. **Asset-scale interaction.** When a community mod ships oversized icon TGAs (Magic), the worktree CLAUDE.md "Do Not Upscale These Art Assets" rule applies. Should Layer 6 include a known-bad mod that ships oversized icons, asserting the engine warns (per `[ASSET_SCALE v1]`) but stock missions still pass? Open — could be a Layer 7 down the road.
6. **Fail-open vs fail-closed on bad `mod.json`.** §2-Track-E says fail-closed on the offending mod, fail-open on the engine. Layer 6's `_compat_test_c` is the test of that — but the spec for *what* the engine does with a bad manifest hasn't been pinned in the lifecycle doc yet. Layer 6 will have to wait on or co-evolve with that decision.
7. **CI runner environment.** Tier-1 today runs locally on a developer box (the menu canary is desktop-bound per `tests/smoke/README.md`). Layers 2–6 don't need the menu canary, so they're CI-safe in principle — but they still need a real GL context. Whether the existing project CI has a GPU runner, or whether layers must run on a self-hosted Windows box with a real GPU, is unresolved.
8. **Snapshot of cell G's "spec'd deviation."** Cell G is the only place stock-mission output is *expected* to differ. What do we snapshot? Mech positions at t=10s? Destroy count? A `[BRAIN] decision=hold` log line per tick? Probably the latter — content-free signal that the Lua brain ran. Pin before Layer 5b lands.
