# Water vertex projection skip — design + plan (2026-05-01, rev2 2026-05-02)

> **Status:** READY-FOR-EXECUTOR. Two review passes applied: adversarial self-review (4 MAJOR + 5 MINOR) and external advisor review (6 blocking + 4 amendment findings). All 19 resolved inline; revision history at end of doc. User-confirmed decisions: Q2 = Option C (visual canary + mouse-pick manual), parity gate dropped (M1), per-quad `IsArmed()` call without cache (M4), Gate B firm at ≥50% with abandonment-acceptable below 200 µs baseline.
>
> **Predecessor dependency (advisor B2).** This slice's Stage 3 default-on flip requires `MC2_RENDER_WATER_FASTPATH` to be default-on FIRST. As of 2026-05-02 that flag is still default-off (renderWater shipped 2026-04-30, soaking). Stages 0-2 of THIS slice can ship before renderWater's promotion; Stage 3 cannot. See "Stage 3" section for the explicit gate.
>
> **Source artifacts.** Brainstorm: [`brainstorms/2026-05-01-water-projection-skip-scope.md`](../brainstorms/2026-05-01-water-projection-skip-scope.md). Recon: [`explorations/2026-04-30-water-projection-skip-recon.md`](../explorations/2026-04-30-water-projection-skip-recon.md). Orchestrator slot: "Water vertex projection skip — fast-path stranded-upstream cleanup."
>
> **Adversarial review:** see "Revision history" at end of doc for the 4 MAJOR + 5 MINOR findings and their resolutions.
>
> **One-paragraph summary.** The renderWater architectural slice (shipped 2026-04-30) retired the water DRAW via a post-`renderLists()` fast path. The legacy water vertex projection in `mclib/quad.cpp:773-1100` still runs every frame for every quad with at least one water-bit corner. Its outputs (per-corner `wx/wy/wz/ww`, the eight `wAlpha` writes, the two `addTriangleBulk` reservations at `quad.cpp:1087-1088`, the `waterHandle`/`waterDetailHandle` per-quad fields) are consumed only by `TerrainQuad::drawWater()`, which the fast path bypasses entirely. This slice gates the upstream block behind the same armed predicate the fast path uses, hoisted into a new `WaterStream::IsArmed()` namespace function. Stage 0 (scaffolding + audits) → Stage 1 (helper hoist, pure refactor) → Stage 2 (gate behind env flag) → Stage 3 (post-soak default-on flip).

---

## Brainstorm decisions (load-bearing — do not relitigate)

| ID | Decision | Source |
|---|---|---|
| **Q1** | `WaterStream::IsArmed()` helper + `BeginLegacyWaterCluster()` thin wrapper at `quad.cpp:773`. Mirrors shipped `gos_terrain_indirect::IsFrameSolidArmed()` precedent. Per-quad call, no frame-static cache (matches shipped pattern at `quad.cpp:106`). | Brainstorm Q1 + adversarial M4 |
| **Q2** | Option C — rely on existing `MC2_RENDER_WATER_PARITY_CHECK` (already-shipped, validates legacy-vs-fast-path equivalence) + visual canary on `mc2_01`/`mc2_17` + manual mouse-pick-on-water test. No new parity flag. | User spec prompt + adversarial M1 |
| **Q3** | **No new parity flag.** Coverage instead = (a) shipped `MC2_RENDER_WATER_PARITY_CHECK` for legacy-vs-fast-path GPU draw equivalence, and (b) one-time DEV-AUDIT step at Stage 0 instrumenting the legacy block's writes for one tier1 run, manually confirming the write set matches the recon's documented-waste set. | Adversarial M1 (overrides Brainstorm Q3) |
| **Q4** | M2d-overlay-shaped scope: now ~50-100 LOC main (parity flag dropped), 1-2 commits, 3 files (down from 4 — no `gos_terrain_water_stream.cpp` parity additions). | Brainstorm Q4 + M1 |
| **Q5** | Out of scope: `terrain.cpp:1611-1636` vertexProjectLoop accumulator (already-closed slice's territory), indirect-terrain consolidation, bucket reservation removal at `quad.cpp:1087-1088` (the skip path simply doesn't reach those lines — no source change), env-map-water `wAlpha` resurrection. **In scope (bonus):** consolidate the two existing armed-predicate sites at `terrain.cpp:1048-1051` and `terrain.cpp:1118-1123`. **Two pre-Stage-2 audits added by adversarial review:** `hazeFactor` reader audit (M2) and `waterHandle`/`waterDetailHandle` reader audit (M3). | Brainstorm Q5 + M2 + M3 |

---

## Architecture map (where the gate fits)

```
Terrain::geometry()
└── per-quad loop → setupTextures()  [mclib/quad.cpp:557 entry]
    ├── solid+detail+overlay path  [quad.cpp:557-772; unchanged]
    └── water projection block  [quad.cpp:773-1100]
        ├── outer water-bit OR gate  [quad.cpp:775-778]
        │    ↓ (NEW: BeginLegacyWaterCluster())
        │    ↓ if IsArmed → set sentinels at 1090-1099 path, skip block body
        │    ↓ else → run legacy block body
        ├── ZoneScopedN("setupTextures water vertex projection")  [quad.cpp:780]
        ├── per-corner projection sub-blocks  [quad.cpp:795-861, 863-931, 933-1001, 1003-1071]
        │   includes 8× wAlpha writes  [801, 806, 869, 874, 939, 944, 1009, 1014]
        ├── leastZ/mostZ/leastW/mostW accumulator updates  [838-858, 906-928, 976-998, 1046-1068]
        ├── waterHandle/waterDetailHandle resolve  [1083-1085 / sentinel else 1090-1094]
        └── 2× addTriangleBulk reservations  [1087-1088]

Terrain::renderWater()           [terrain.cpp:1027 — checked at terrain.cpp:1048-1051 armed predicate]
└── early-return when armed → bucket flush is no-op (totalVertices=0 path at txmmgr.cpp:1500)

Terrain::renderWaterFastPath()   [terrain.cpp:1116 — checked at terrain.cpp:1118-1123 armed predicate]
└── post-renderLists draw using static recipe SSBO + per-frame thin records
    └── renderLists hook order: gamecam.cpp calls land->renderWaterFastPath() AFTER mcTextureManager->renderLists()
```

The slice gates ONLY the per-quad `setupTextures` water block. The fast-path draw (`renderWaterFastPath()`) reads handle data from its **own** static recipe SSBO + per-frame thin records — NOT from the per-quad `waterHandle`/`waterDetailHandle` fields the legacy block writes. This is the residual claim flagged at the end of the brainstorm; it is verified below in the appendix (`gos_terrain_water_stream.h:46` recipe schema does not include `waterHandle`; `gos_terrain_water_stream.h:91-99` thin record does not include `waterHandle` either; the shader binds via `kWaterRecipeSsboBinding=5` and `kWaterThinSsboBinding=6` global state, not per-quad).

---

## Design decisions

### D1. `WaterStream::IsArmed()` helper

**Add free function at namespace scope in `GameOS/gameos/gos_terrain_water_stream.h`** (immediately after the existing `bool IsReady();` declaration at line 206):

```cpp
// True iff the renderWater fast path is enabled AND ready to draw this frame.
// Hoists the predicate currently inlined at terrain.cpp:1048-1051 (legacy
// renderWater early-return) and terrain.cpp:1118-1123 (renderWaterFastPath
// preflight). Pure read; no state mutation. Safe to call per-quad — see
// shipped precedent at gos_terrain_indirect::IsFrameSolidArmed() invoked
// per-quad from quad.cpp:106 without a cache.
bool IsArmed();
```

**Implementation in `gos_terrain_water_stream.cpp`** (mirrors the existing inline form at `terrain.cpp:1046-1051` exactly — including its raw-`getenv()` semantics for now):

```cpp
bool IsArmed() {
    // Stage 1 form: matches the legacy inline predicate at terrain.cpp:1046-1051
    // verbatim. Raw `getenv() != nullptr` means MC2_RENDER_WATER_FASTPATH=0
    // currently still arms the fast path — this is observable existing behavior;
    // changing it inside this slice would silently re-semantic the flag on
    // existing developer machines. See "Cross-slice contract" note below.
    static const bool s_fastPath =
        (getenv("MC2_RENDER_WATER_FASTPATH") != nullptr);
    return s_fastPath
        && IsReady()
        && GetRecipeCount() > 0
        && Terrain::terrainTextures2 != nullptr;
}
```

**Cross-slice contract (advisor rev2 #1).** When renderWater's own Stage 4 default-on flip ships (currently soaking; tracked separately in orchestrator), THREE call sites must atomically switch from raw `getenv()` to the `EnvEnabled()` boolean parser:

1. `mclib/terrain.cpp:1046-1051` (legacy `renderWater` early-return — currently inline; will be replaced by `IsArmed()` per Stage 1)
2. `mclib/terrain.cpp:1118-1123` (`renderWaterFastPath` preflight — same)
3. `gos_terrain_water_stream.cpp` `IsArmed()` (created here in Stage 1)

After Stage 1 ships, sites 1 and 2 collapse into single `IsArmed()` calls — so the cross-slice contract reduces to "renderWater Stage 4 must update `IsArmed()` to use `EnvEnabled()`." The contract is documented here, in `IsArmed()`'s docstring, and must be surfaced in the renderWater Stage 4 plan as a precondition. **This slice's Stage 3 default-on flip cannot land while `IsArmed()` still uses raw `getenv()` for `MC2_RENDER_WATER_FASTPATH`** — the predecessor's mechanical contract has to land first or coupled.

**Note on namespace vs class.** `WaterStream` is a **namespace** (declared at `gos_terrain_water_stream.h:23` `namespace WaterStream {`), NOT a class. `IsReady()` and `GetRecipeCount()` are namespace-scope free functions, not class methods. The brainstorm and user spec-prompt both said "WaterStream class header"; that is loose terminology. Executor: declare as a free function inside the namespace, mirroring the existing 9 free functions in the header.

**Layering audit (advisor B5).** `IsArmed()`'s implementation references `Terrain::terrainTextures2`, which is declared in `mclib/terrain.h`. Verified at write-time: `gos_terrain_water_stream.cpp:28-31` already includes `mclib/terrain.h`, `mclib/quad.h`, `mclib/vertex.h`, `mclib/mapdata.h` — the `GameOS/gameos → mclib` dependency direction is established for this TU. Adding `Terrain::terrainTextures2` does NOT introduce a new layer crossing. Stage 1 acceptance verifies build clean.

If a future reorganization separates `GameOS/gameos` from `mclib` more strictly, the alternative is to define `IsArmed()` in `mclib/terrain.cpp` inside the `WaterStream` namespace (legal C++ — namespace bodies can span TUs). Not done in this slice; flagged here for the future reorg's authors.

**Replace the two existing inline predicate sites:**

- `mclib/terrain.cpp:1046-1051` (current 6-line inline): replace with:
  ```cpp
  if (WaterStream::IsArmed())
      return;  // Skip legacy loop entirely; renderWaterFastPath() does the work.
  ```
  Drop the `s_fastPath` static (now lives inside `IsArmed()`).

- `mclib/terrain.cpp:1118-1123` (current 5-line sequential checks): replace with:
  ```cpp
  if (!WaterStream::IsArmed()) return;
  ```

**Per-quad call pattern.** `BeginLegacyWaterCluster()` (D2) calls `IsArmed()` once per water-bearing quad — ~1.3K calls/frame on mc2_01, ~5K on mc2_17. The four-clause AND with three function calls is cheap enough at this rate to NOT need a frame-static cache. The shipped indirect-terrain precedent at `quad.cpp:106` calls `IsFrameSolidArmed()` per quad (no cache); the comment at `terrain.cpp:1704` describes per-frame stability as a CORRECTNESS property (safe to call repeatedly), not a perf optimization. We follow the same pattern: per-quad call, no cache.

**Stage 1 acceptance:** behavior-equivalent for the existing two call sites (`terrain.cpp:1046-1051` + `terrain.cpp:1118-1123`), verified by tier1 5/5 PASS in BOTH `MC2_RENDER_WATER_FASTPATH=1` and unset configs. (Function-static initialization order is well-defined; called once on first invocation, same as the existing inline statics.)

### D2. `BeginLegacyWaterCluster()` thin wrapper at `quad.cpp:773`

**Add namespace-scoped Begin/End helper pair near top of `mclib/quad.cpp`**, mirroring the shipped `BeginLegacySolidCluster()` / `EndLegacySolidCluster()` / `NoteLegacyDetailOverlayCluster()` pattern at `quad.cpp:105-113` (final adversarial review M1: splitting per shipped convention so Begin stays a pure predicate; End owns the counter side-effect):

```cpp
// Begin: pure predicate. True ⇒ legacy water-projection block runs.
// Stage 2: defaultValue=false (opt-IN via MC2_WATER_PROJECT_SKIP=1).
// Stage 3: defaultValue flips to true (kill-switch via =0 or =false).
static inline bool BeginLegacyWaterCluster() {
    static const bool s_skipEnabled =
        WaterStream::EnvEnabled("MC2_WATER_PROJECT_SKIP", /*default=*/false);  // Stage 3 flips
    return (!s_skipEnabled) || (!WaterStream::IsArmed());
}

// End: counter bump. Called once per water-bearing quad (per outer water-bit
// OR gate at quad.cpp:775-778), with the Begin return value to attribute the
// counter. Counter invariant `bearing == legacy + skipped` is enforced by
// construction: every End call increments WaterBearing once and exactly one
// of Legacy/Skipped.
static inline void EndLegacyWaterCluster(bool ranLegacy) {
    WaterStream::Counters_AddWaterBearingQuad();
    if (ranLegacy) WaterStream::Counters_AddLegacyProjectedQuad();
    else           WaterStream::Counters_AddSkippedProjectedQuad();
}
```

**Stage 0 stub form (final adversarial review M2 — disambiguating the stub semantics):** Stage 0 ships the End wiring REAL and the Begin predicate hardcoded to `return true`. End is unchanged across all stages. Stage 2 swaps only the Begin body to the real env+armed check. This mirrors how indirect-terrain Stage 0 (commit `9bfcddc`) shipped `Counters_Add*` setters that no caller invoked yet — the API was real, the wiring was deferred. Stage 0 stub:

```cpp
static inline bool BeginLegacyWaterCluster() {
    return true;  // Stage 0 stub — Stage 2 swaps to real env+armed check
}
// EndLegacyWaterCluster: ships REAL in Stage 0 (counter wiring real from Stage 0 onward).
```

**Why a real boolean parser?** Per advisor B1, `getenv() != nullptr` makes `MC2_WATER_PROJECT_SKIP=0` enable the skip — directly contradicting D6's rollback contract ("`MC2_WATER_PROJECT_SKIP=0` (or unset) returns to baseline behavior"). The `EnvEnabled` parser treats `=0` and `=false` as opt-out at all stages, so Stage 3's kill-switch semantics work without surprise. The helper lives in `WaterStream` (Stage 0 file list) so all three call sites (`BeginLegacyWaterCluster`, `gameosmain.cpp` banner, future `IsArmed()` update for `MC2_RENDER_WATER_FASTPATH`) share one definition.

**Wrap the block at `quad.cpp:773-1100`.** The current structure is:

```cpp
// quad.cpp:773-778
if ((vertices[0]->pVertex->water & 1) || ... || (vertices[3]->pVertex->water & 1))
{
    // ZoneScopedN, projection, 8× wAlpha, accumulator updates, addTriangleBulk
    // ... 320+ lines ...
}
else
{
    waterHandle = 0xffffffff;
    waterDetailHandle = 0xffffffff;
}
```

**New structure:**

```cpp
if ((vertices[0]->pVertex->water & 1) || ... || (vertices[3]->pVertex->water & 1))
{
    bool runLegacy = BeginLegacyWaterCluster();
    if (runLegacy)
    {
        // ZoneScopedN, projection, 8× wAlpha, accumulator updates, addTriangleBulk
        // ... existing 320+ lines unchanged ...
    }
    else
    {
        // Skip path: fast-path will draw water; the per-quad handle fields are
        // unused by the fast path (it reads from its own recipe SSBO at binding
        // 5 + thin SSBO at binding 6 — see gos_terrain_water_stream.h:106-107).
        // Sentinel mirrors the existing else at quad.cpp:1090-1099.
        waterHandle = 0xffffffff;
        waterDetailHandle = 0xffffffff;
    }
    EndLegacyWaterCluster(runLegacy);   // counter bump; mirrors EndLegacySolidCluster pattern
}
else
{
    waterHandle = 0xffffffff;
    waterDetailHandle = 0xffffffff;
    // No End* call here — only water-bearing quads enter the End counter set.
    // (NoteLegacyDetailOverlayCluster at quad.cpp:111-113 is the analogous "non-Begin"
    // counter site for indirect-terrain; we don't need an analog because the outer
    // water-bit OR gate already partitions water-bearing from non-water-bearing quads.)
}
```

**What the gate skips (verified against recon Section A + on-write grep here):**
- The `ZoneScopedN("setupTextures water vertex projection")` at `quad.cpp:780`.
- The four per-corner projection sub-blocks at `quad.cpp:795-861, 863-931, 933-1001, 1003-1071` (all per-vertex `wx/wy/wz/ww` writes, all `clipInfo` writes, all `calcThisFrame |= 2` writes).
- The eight `wAlpha` writes at `quad.cpp:801, 806, 869, 874, 939, 944, 1009, 1014` (verified dead-data — recon Section D.1: zero readers in active worktree).
- The four `hazeFactor = 1.0f` writes at `quad.cpp:821, 891, 961, 1031` (off-map fallback path). **Audit required at Stage 0 — see M2 below.**
- The `leastZ/mostZ/leastW/mostW/leastWY/mostWY` accumulator updates at `quad.cpp:838-858, 906-928, 976-998, 1046-1068` (per Q2/D6 — relying on terrain extrema being globally dominant).
- The two `addTriangleBulk` reservations at `quad.cpp:1087-1088`. The skip path simply doesn't reach those lines — no source-code change to lines 1087-1088 themselves; they remain in the un-armed legacy path.

**Bucket-flush behavior under skip.** With the entire block skipped, no `addTriangleBulk(waterHandle, ...)` runs. The `nextAvailableVertexNode` slot allocation does NOT happen for water buckets this frame; `renderLists()` finds no water bucket to iterate at `txmmgr.cpp:1466-1530`. This is a strict subset of the no-op behavior the recon analyzed at C.2 (where the reservation runs but `renderLists` computes `totalVertices=0` and skips the draw). Both cases produce zero draw work; the skip path saves the slot allocation too.

### D3. Coverage strategy (no new parity flag)

**Decision (revised per adversarial M1):** drop the brainstorm's proposed `MC2_WATER_PROJECT_SKIP_PARITY` flag. Two layers of coverage instead:

**Layer 1 — Existing `MC2_RENDER_WATER_PARITY_CHECK`** (already shipped, validated silent-on-pass tier1 5/5 with ~3.2M quads byte-checked / zero mismatches per `memory/renderwater_fastpath_stage2.md`). This validates that the renderWater fast-path's GPU draw is byte-equivalent to the legacy `drawWater()` emit. The slice's invariant — "the legacy water-projection block's outputs are wasted" — is implied: if the fast path produces byte-equivalent output without consuming the legacy block's per-quad writes, then those writes are unobserved.

**Layer 2 — One-time DEV-AUDIT at Stage 0.** Before Stage 1, instrument the `quad.cpp:773-1100` block with **counter-based per-write-site instrumentation** (NOT per-write `printf` — advisor A1 made the correct call: per-write log lines over a 30-second run produce enormous output and alter timing measurably). Shape:

```cpp
// Per-site bitset/counter. Bumped per call inside the water-projection block.
// Dumped ONCE at mission end (or every 600 frames if mission runs long).
// Gated by MC2_WATER_PROJECT_SKIP_AUDIT=1.
static int s_writeSiteCounters[/*~18 sites*/] = {0};
// At each write site, one increment:
if (s_audit) ++s_writeSiteCounters[kSiteId_quadcpp_801_wAlpha];
// On mission end / shutdown:
//   [WATER_AUDIT v1] event=summary site=quad.cpp:801 field=wAlpha hits=N
//   [WATER_AUDIT v1] event=summary site=quad.cpp:838 field=leastZ hits=N
//   ...
```

Run once on mc2_17 (water-heavy, longest renderWater Stage 3 baseline), capture per-site hit counts at mission end, manually inspect against recon Section C's documented waste set. Confirm:
- No write site outside `{wx, wy, wz, ww, clipInfo, calcThisFrame|=2, wAlpha, hazeFactor, waterHandle, waterDetailHandle, leastZ, mostZ, leastW, mostW, leastWY, mostWY}`.
- Any unexpected enumerated site is a STOP-THE-LINE — recon's analysis was incomplete, slice cannot proceed.

**Audit code is REMOVED (squashed out of the Stage 0 commit) before Stage 1 commits** per advisor B4 below. One-time — not per-frame, not shipped behind a permanent env flag.

**Why no per-frame parity?** Parity-checks of the form "compare two output streams" require two output streams. With Q2 = Option C, we trust accumulator-delta subdominance; we don't measure it. The legacy-block writes (other than accumulators) are wasted by definition once `MC2_RENDER_WATER_PARITY_CHECK` confirms fast-path byte-equivalence. A second per-frame parity flag would build redundant infrastructure for the same invariant.

**Why a one-time audit anyway?** Recon Section C built the waste set from grep evidence, not runtime observation. A 30-second audit converts the grep claim into runtime certainty before Stage 2 ships. Cheap, single-use.

### D4. Env gate

**Single new env flag: `MC2_WATER_PROJECT_SKIP=1`** (default off). Stage 3 promotes to default-on (kill-switch form: `MC2_WATER_PROJECT_SKIP=0` opts OUT), mirroring the `MC2_MODERN_TERRAIN_PATCHES` flip at commit `aee39cc`.

**Stage-0-only DEV-AUDIT flag: `MC2_WATER_PROJECT_SKIP_AUDIT=1`** (per D3 Layer 2). NOT shipped — removed before Stage 1 commits.

**run_smoke.py passthrough.** Add `MC2_WATER_PROJECT_SKIP` to the env-passthrough list at `scripts/run_smoke.py:232-253` (verified at write-time, line 246 passes `MC2_RENDER_WATER_FASTPATH`, line 247 passes `MC2_RENDER_WATER_PARITY_CHECK`). New entry lands alphabetically between `MC2_RENDER_WATER_PARITY_CHECK` and `MC2_VERTEX_PROJECT_FAST`:

```
"MC2_RENDER_WATER_PARITY_CHECK",
"MC2_WATER_PROJECT_SKIP",                 // NEW (Stage 0)
"MC2_VERTEX_PROJECT_FAST",
```

`MC2_WATER_PROJECT_SKIP_AUDIT` is dev-only — NOT added to passthrough.

**[INSTR v1] banner + per-mission summary log** (advisor A4 + advisor rev2 #3: banner timing fix).

The `[INSTR v1]` banner emits at engine startup, BEFORE any geometry runs. Reporting "effective state" in the startup banner would always read `na` and be useless (advisor rev2 #3 caught this). Solution: split the two pieces of information across TWO emit points.

**Startup banner field** at `gameosmain.cpp` (per `memory/renderwater_fastpath_stage2.md:127`):
- `water_skip_env=<0|1>` — env-flag REQUESTED state, set at startup from `EnvEnabled("MC2_WATER_PROJECT_SKIP", default)`. Reflects what the operator asked for.

**Per-mission summary log** emitted at mission teardown (and at app shutdown) by `gos_terrain_water_stream.cpp::ReleaseGlResources()` (advisor rev3 #1: counter ownership and emit site both in WaterStream namespace — no cross-TU read of file-scope statics):

```
[WATER_SKIP v1] event=summary water_skip_env=N
                water_bearing_quads_total=W
                legacy_projected_quads_total=L
                skipped_projected_quads_total=S
                effective=<0|1>
```

**Counter invariant (advisor rev3 #3 — naming precision):**
```
water_bearing_quads_total = legacy_projected_quads_total + skipped_projected_quads_total
```
i.e., every water-bearing quad takes exactly ONE of the two paths (legacy or skip). The invariant is asserted in the summary emit; if it fails, the counter wiring is wrong and the gate fails. `EndLegacyWaterCluster(ranLegacy)` enforces it by construction (it bumps `WaterBearing` always, and exactly one of `Legacy`/`Skipped` based on `ranLegacy`).

**`effective` derivation (final adversarial review m2 — drop redundant `is_armed_ever`):** `effective=1` iff `skipped_projected_quads_total > 0` — the engine genuinely skipped at least one water-bearing quad during the mission. `effective=0` otherwise (env was unset, OR env was set but the predicate never armed, OR no water-bearing quads). The earlier `is_armed_ever` field was redundant: `skipped > 0` already implies "armed at least once," and `skipped == 0 && water_skip_env=1` already encodes "operator asked but engine never armed." Two pieces of info (`water_skip_env` startup-banner + `effective` per-mission summary) cover the whole truth table.

The summary line is the runtime ground truth; the startup banner field is the operator-intent indicator. Together they answer "did the operator ask for skip" + "did the engine actually do it" — the question advisor A4 originally raised.

**Expected per-stage values (Gate 0e and Gate B reference):**

Stage 0 (`BeginLegacyWaterCluster()` is `return true` stub, both `MC2_WATER_PROJECT_SKIP` configs):
```
water_bearing_quads_total > 0
legacy_projected_quads_total == water_bearing_quads_total
skipped_projected_quads_total == 0
effective=0
```

Stage 2 with `MC2_RENDER_WATER_FASTPATH=1 MC2_WATER_PROJECT_SKIP=1`:
```
skipped_projected_quads_total ≈ water_bearing_quads_total  (≥ 99% on stable missions)
legacy_projected_quads_total ≈ 0  (only un-armed-frame outliers — e.g., first frame before recipe is built)
effective=1
```

Stage 2 with `MC2_RENDER_WATER_FASTPATH=1` and `MC2_WATER_PROJECT_SKIP` unset/`=0`:
```
legacy_projected_quads_total == water_bearing_quads_total
skipped_projected_quads_total == 0
effective=0
```

The two-field-but-emitted-at-different-times form was the right shape; the banner-only form (rev1 of rev2) had the wrong emit timing.

### D5. Visual canary at deterministic camera (Gate A)

**Mirror the renderWater Stage 2 canary** (`tests/smoke/artifacts/water-diff-1777563391/` cited in `memory/renderwater_fastpath_stage2.md:48-49`). Tier1 mission selection:

- **Primary canary: `mc2_01`** (water-heavy stock mission, also the renderWater Stage 2 canary).
- **Secondary canary: `mc2_17`** (the `lookup_miss` survivor mission from renderWater Stage 3 — water-heaviest in the tier1 set; if any mission would surface an accumulator-delta regression in mouse-pick, this is it).

**Camera coordinate citation.** The smoke runner uses `MC2_SMOKE_SEED=0xC0FFEE` (cited at `scripts/run_smoke.py:228` — verified at write-time). With this seed, the starting camera position is mission-deterministic — the seed gates AI/RNG state, not camera placement, but combined with the tier1 missions' fixed start states this produces a reproducible camera+entity layout. The "coordinate" is therefore "smoke-runner default starting camera + post-warmup frames 30-60" — deterministic-by-seed rather than world-space-cited. Reproducibility check: `py -3 scripts/run_smoke.py --tier tier1 --duration 8` twice on the same git revision should produce frame-30 screenshots that diff-quiet.

**Capture script:** reuse `scripts/water_visual_diff.py` (cited in `memory/renderwater_fastpath_stage2.md:75`); add `MC2_WATER_PROJECT_SKIP=1` to the matrix as a third config alongside legacy / fastpath.

**Mouse-pick augmentation.** The smoke runner is passive (no clicks); add a manual-mission-play step to the Stage 2 acceptance ladder: **load mc2_17, attempt unit selection on a unit standing in shallow water near a shoreline tile**. If selection picks correctly, mouse-pick depth is intact; if cursor depth drifts to terrain-elevation, the accumulator delta is biting.

**Repro location** (executor must record this in the commit message; spec does not pre-pin coordinates because mc2_17's mission start state determines unit placements). Procedure:
1. Boot mc2_17 with `MC2_RENDER_WATER_FASTPATH=1 MC2_WATER_PROJECT_SKIP=1` set.
2. Pan camera to a shoreline area where one of your friendly mechs has line-of-sight to a water tile (mc2_17 has a coastline along the southern map edge — reliable repro region).
3. Move a friendly unit so its world-position is on a shoreline tile (one foot in water, one foot on land — half-submerged).
4. Click on the unit. Selection succeeds → pass. Mis-click selects another unit, or cursor highlights a tile away → fail.
5. Record in commit message: world-coordinates of the unit, screen-coordinates of the click, screenshot of the moment-before and moment-after the click.

If a stable scripted version of this becomes valuable later, it can be added to the smoke runner via the existing menu-canary mechanism (which IS desktop-bound, but tier1's manual test is a one-time human gate, not a CI gate). For now the manual record is sufficient.

### D6. Mouse-pick residual handling (Q2 = Option C per spec prompt)

**Failure mode being trusted:** `leastZ/mostZ/leastW/mostW` accumulator contributions from water vertices feed `Camera::inverseProjectZ()` for mouse-pick depth (recon Section D.5; consumer at `mclib/terrain.cpp:1732` `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` — verified at write-time).

**Recon Section D.5 conjecture:** terrain elevation ≥ water elevation, so terrain-vertex extrema are globally dominant; water-only quads rarely contribute the global min/max. **Unmeasured.** Trusted on Option C basis.

**Tripwires** if conjecture is wrong:
- Manual mc2_17 unit-select-in-water test fails (D5 augmentation).
- User-reported mouse-pick drift on tier1 missions during Stage 2 soak.

**Rollback:** `MC2_WATER_PROJECT_SKIP=0` (or unset) returns to baseline behavior. Single env-var revert; no commit revert needed during soak.

---

## Plan stages

### Stage 0 — Scaffolding + audits (default-off, no behavior change)

**Files (code):**
- `GameOS/gameos/gos_terrain_water_stream.h` (declaration: `bool IsArmed();` at namespace scope; deferred to Stage 1 — Stage 0 may declare-only without definition)
- `mclib/quad.cpp` (add `BeginLegacyWaterCluster()` helper near top, in same anonymous namespace as `BeginLegacySolidCluster` at `quad.cpp:97-114`; `BeginLegacyWaterCluster()` returns `true` unconditionally for Stage 0 — wiring deferred to Stage 2)
- **Counters owned by `WaterStream` namespace** (advisor rev3 #1 — file-scope storage in `.cpp` cannot be read from another TU; move ownership). Extends the shipped `gos_terrain_indirect::Counters_*` accessor pattern from commit `9bfcddc` (verified at `gos_terrain_indirect.h:104-111`) — extends because the shipped pattern uses `long long` getters and has no Reset; this slice adds per-mission Reset semantics for the `[WATER_SKIP v1]` summary. New entries in `gos_terrain_water_stream.h`:
  ```cpp
  // Counter accessors — file-scope storage in gos_terrain_water_stream.cpp;
  // hot-loop callers in mclib/quad.cpp use the Add* setters via include.
  // Pattern source: gos_terrain_indirect.h:104-111. Type chosen long long
  // to match shipped sibling (final adversarial review m4 — atomic was
  // unnecessary since MC2 terrain processing is single-threaded).
  void      Counters_AddWaterBearingQuad();         // bumped per water-bearing quad observed
  void      Counters_AddLegacyProjectedQuad();      // bumped when EndLegacyWaterCluster(ranLegacy=true)
  void      Counters_AddSkippedProjectedQuad();     // bumped when EndLegacyWaterCluster(ranLegacy=false)
  long long Counters_GetWaterBearingQuads();
  long long Counters_GetLegacyProjectedQuads();
  long long Counters_GetSkippedProjectedQuads();
  void      Counters_Reset();                       // called at start of new mission via Build()
  ```
  Storage in `.cpp`: three `static long long` (no atomic — adversarial review m4: matches shipped indirect-terrain `long long` type at `gos_terrain_indirect.h:109-111`; MC2 terrain processing is single-threaded so atomicity is unnecessary).
- Counter cadence:
  - **Reset**: `WaterStream::Counters_Reset()` called from `WaterStream::Build()` (verified at `gos_terrain_water_stream.h:195`). Reset-at-new-mission-start sidesteps the ambiguity of which mission-unload hook (`Reset()` line 198 vs `ReleaseGlResources()` line 123) runs first — counters survive the unload→load gap so the per-mission summary in `ReleaseGlResources()` still has valid data, and only zero when the next mission's `Build()` runs.
  - **Emit (per-mission)**: at mission teardown — `WaterStream::ReleaseGlResources()` (verified at `gos_terrain_water_stream.h:123`) emits `[WATER_SKIP v1] event=summary` line via the `Counters_Get*` accessors before tearing down GL state. Counter ownership and emit site are both in WaterStream — no cross-TU read of file-scope statics.
  - **Emit (per-window, optional)**: every 600 frames if `MC2_WATER_DEBUG=1`, mirroring renderWater Stage 2's `[WATER_DEBUG v1] event=population` cadence at `terrain.cpp:1083-1107`. Default-off.
  - **Per-frame rate** (for Gate B's "comparable to baseline" check): derive from `(skipped_projected_quads_total - prev_total) / (frames_total - prev_frames)` between Tracy capture windows; not a separate counter.

- **Shared `EnvEnabled()` helper** (advisor rev3 #2 — quad.cpp's anonymous namespace cannot be reached by gameosmain.cpp or future renderWater Stage 4 update of `IsArmed()`). Declared in `gos_terrain_water_stream.h` as a namespace-scoped free function:
  ```cpp
  // Boolean env parser — treats =0 and =false as opt-out. Cross-slice contract:
  // renderWater Stage 4 must adopt this for MC2_RENDER_WATER_FASTPATH parsing
  // when the flag flips to default-on (see D1 cross-slice contract note).
  bool EnvEnabled(const char* name, bool defaultValue);
  ```
  Definition in `gos_terrain_water_stream.cpp` (single source of truth — no duplication). Callers: `BeginLegacyWaterCluster()` at `quad.cpp` (already includes `gos_terrain_indirect.h`'s sibling at `quad.cpp:43`, same dir-traversal pattern; new include of `gos_terrain_water_stream.h` is added in Stage 0 — final adversarial review m1), `gameosmain.cpp` for `water_skip_env` banner field (new include), future `IsArmed()` update for `MC2_RENDER_WATER_FASTPATH`.

- **Includes added in Stage 0** (final adversarial review m1, m3):
  - `mclib/quad.cpp`: add `#include "../GameOS/gameos/gos_terrain_water_stream.h"` for the `WaterStream::Counters_*` and `WaterStream::EnvEnabled` calls (paralleling `quad.cpp:43`'s `gos_terrain_indirect.h` include).
  - `GameOS/gameos/gameosmain.cpp`: add `#include "gos_terrain_water_stream.h"` for the `water_skip_env` banner field setup.
  - `gos_terrain_water_stream.cpp`: no new includes needed for `long long` counters (no `<atomic>` required since adversarial m4 dropped atomicity).
- `scripts/run_smoke.py:247-248` (add `MC2_WATER_PROJECT_SKIP` to passthrough between `MC2_RENDER_WATER_PARITY_CHECK` and `MC2_VERTEX_PROJECT_FAST`)
- `GameOS/gameos/gameosmain.cpp` (add `water_skip_env=<0|1>` to `[INSTR v1]` startup banner; per advisor rev2 #3 the per-mission `effective` value is emitted SEPARATELY at teardown via `[WATER_SKIP v1] event=summary` from `gos_terrain_water_stream.cpp::ReleaseGlResources()` — not in this banner)
- DEV-AUDIT instrumentation block in `mclib/quad.cpp:773-1100` per D3 — counter-based per advisor A1; landed in Stage 0 commit, REMOVED in Stage 1 commit (carve-out per worktree CLAUDE.md "Documentation Discipline" — DEV-AUDIT is intention-only, doesn't ship)

**Audits (Stage 0 deliverables, NO code changes — write findings into the spec's appendix or commit message):**

**M2 audit — `hazeFactor` reader/writer survey.** Required because the recon flagged `hazeFactor = 1.0f` writes at `quad.cpp:821, 891, 961, 1031` as "TBD if water-projection-specific reader." Procedure:
1. `Grep "hazeFactor" worktree-wide` (already done at adversarial review — 15 files match).
2. For each non-water-block reader site, determine: (a) does the reader execute reachably under `MC2_RENDER_WATER_FASTPATH=1 + MC2_WATER_PROJECT_SKIP=1`? (b) does it depend on the SKIPPED 1.0f write, or is the required value always written elsewhere before any reader observes it?
3. **Outcome A (refined per advisor A2) — pass condition:** "no reachable non-fast-path reader depends on the skipped writes, OR the required value is written elsewhere before any reader observes it." Sole-writer status alone is NOT sufficient; the reader-side analysis is required. Document in commit message: each non-water reader site, its reachability under skip, and either (a) "tolerates stale value" or (b) "value re-written by site X at line Y before reader."
4. **Outcome B — a non-water reader observes the skipped 1.0f write with no upstream write:** STOP-THE-LINE. Slice cannot proceed without preserving those writes (or migrating them to a non-skipped site).

**M3 audit — `waterHandle`/`waterDetailHandle` reader survey.** Required because recon Section D audited `clipInfo` and `wAlpha` exhaustively but did NOT exhaustively audit per-quad `waterHandle`/`waterDetailHandle` consumers outside `drawWater()`. Procedure:
1. `Grep '\.waterHandle|\.waterDetailHandle|->waterHandle|->waterDetailHandle' worktree-wide`.
2. For each match outside `mclib/quad.cpp:773-1100` and `TerrainQuad::drawWater` (which we already know uses them), determine: does the call site reach during `MC2_RENDER_WATER_FASTPATH=1 + MC2_WATER_PROJECT_SKIP=1`? If yes, does it tolerate `0xffffffff`/sentinel?
3. **Outcome A — `drawWater` is sole reader (or all other readers tolerate sentinel):** record in commit message; no further work.
4. **Outcome B — a non-`drawWater` reader breaks on sentinel:** STOP-THE-LINE. Slice needs to either preserve the handle writes (kept inside the gated block but executed unconditionally) or change the sentinel value.

**Audit deliverable:** the Stage 0 commit message includes the grep output (or a paste-link) + the audit conclusion (Outcome A/B). If both audits land Outcome A, Stage 1 starts. If either lands Outcome B, the slice goes back to brainstorm.

**Mandatory DEV-AUDIT instrumentation — WIP-then-squash workflow** (per D3 Layer 2; advisor B4 + rev2 #4 commit-boundary fix):

The DEV-AUDIT is mandatory as a **process step**, but the audit code MUST NOT ship in the landed Stage 0 commit. The clean workflow:

1. Apply a temporary WIP audit patch on top of the Stage 0 scaffolding work (counter-based instrumentation per the spec below). Do NOT commit yet — keep as `git stash` or `git add -p`-pending.
2. Build + run mc2_17 with `MC2_WATER_PROJECT_SKIP_AUDIT=1`. Capture per-site summary at mission end.
3. Manually inspect against the documented-waste set. If clean, proceed. If unexpected sites, STOP-THE-LINE.
4. **Discard the WIP audit patch** (`git stash drop` or reverse-apply).
5. Commit ONLY the retained Stage 0 scaffolding (no audit code, no `MC2_WATER_PROJECT_SKIP_AUDIT` env flag, no per-site counters).

Result: the landed Stage 0 commit contains scaffolding + WaterStream counter accessors (`Counters_AddWaterBearingQuad` / `Counters_AddLegacyProjectedQuad` / `Counters_AddSkippedProjectedQuad` and `Counters_Get*` / `Counters_Reset`) needed for Gate B but NO audit-specific code. The audit's evidence lives in the commit message (a paste of the per-site summary output + the executor's confirmation).

If a future regression suspect points at the same "what does the legacy block actually write?" question, the WIP patch can be re-applied from `git reflog` or re-derived from this spec section. It is intentionally not retained as a checked-in dev tool because per-site counters are hot-loop overhead and the audit is single-use.

The instrumentation shape (use as the WIP patch reference):

```cpp
// One bitset/counter per write site inside the water-projection block.
// Bumped per-call; dumped once at mission end (or per 600 frames if mission
// runs long). No per-write log lines.
static int s_writeSiteCounters[/*N=18ish*/] = {0};
// At each write site:
if (s_audit) ++s_writeSiteCounters[kSiteId_quadcpp_801_wAlpha];
// On mission end / shutdown, print one summary line:
//   [WATER_AUDIT v1] event=summary site=quad.cpp:801 field=wAlpha hits=N
//   [WATER_AUDIT v1] event=summary site=quad.cpp:838 field=leastZ hits=N
//   ...
```

Run once on mc2_17 (water-heavy, longest renderWater Stage 3 baseline), capture summary output, validate the per-site hit counts cover the recon's documented-waste set with no surprises, then **delete the audit code before Stage 1 commits** (the spec carves out this DEV-AUDIT as Stage-0-only — no demote-to-silent permanent residue, since the runtime cost is paid one time only).

**Stage 0 acceptance:**
- Build clean with `RelWithDebInfo`.
- tier1 5/5 PASS unset (regression baseline: identical to pre-Stage-0).
- `MC2_WATER_PROJECT_SKIP=1` set: tier1 5/5 PASS, behavior unchanged (helper returns `true` unconditionally — stub).
- `MC2_WATER_PROJECT_SKIP=0` set: tier1 5/5 PASS, behavior identical to unset (advisor B1 contract — `=0` and unset both opt OUT).
- `[INSTR v1]` startup banner shows `water_skip_env=<0|1>` reflecting the env state at boot.
- `[WATER_SKIP v1] event=summary` line emits at mission teardown with `water_bearing_quads_total > 0`, `legacy_projected_quads_total == water_bearing_quads_total`, `skipped_projected_quads_total == 0`, `effective=0` (Stage 0 helper is a stub that always returns true — no skip yet, so no skipped quads). Counter invariant `bearing == legacy + skipped` is asserted at emit time.
- Both audits (M2 + M3) land Outcome A; commit message documents the audit results per refined Outcome A pass condition.
- DEV-AUDIT (`MC2_WATER_PROJECT_SKIP_AUDIT=1`) run once on mc2_17; per-write-site counters dumped at mission end; verified write set covers recon's documented-waste set with no surprises. Audit code REMOVED before Stage 1 commits.
- **Tracy baseline established** (advisor rev2 cleanup #3 — pin the camera setup):
  - Mission: `mc2_17` (water-heavy) — chosen because it stresses the population that the slice retires; mc2_01 was less precise per recon Section C.3.
  - Camera: post-mission-load, mouse-wheel zoom OUT to maximum (the smoke-runner's default starting zoom is mid-range; max-zoom-out exposes the most water-bearing quads simultaneously). Position camera at the southern coastline (visible from default start by panning south ~3 screens).
  - Capture window: post-warmup frames 60-120 (renderWater Stage 3 used 30-60; this slice uses a wider window because per-quad rate matters more than per-frame jitter for hit-count comparison).
  - Tracy view: filter to zone `setupTextures water vertex projection` (`quad.cpp:780`); record mean + σ across the capture window; record `WaterStream::Counters_GetWaterBearingQuads()` value at frame 60 and frame 120 — the diff (divided by 60 frames) is the per-frame water-bearing-quad-count baseline. (Stage 0 has the helper as a stub returning true, so `WaterBearing == LegacyProjected` and `SkippedProjected == 0` — either suffices for the baseline since they're equal here.)
  - Both numbers (zone time + per-frame quad count) are recorded in the Stage 0 commit message and become the Stage 2 Gate B reference.

### Stage 1 — `WaterStream::IsArmed()` helper hoist (pure refactor)

**Files:**
- `GameOS/gameos/gos_terrain_water_stream.h` + `.cpp` (define `IsArmed()`; cite the existing predicate at `terrain.cpp:1046-1051` and `terrain.cpp:1118-1123` in the docstring as the consolidated source)
- `mclib/terrain.cpp:1046-1054` (replace inline with `if (WaterStream::IsArmed()) return;`)
- `mclib/terrain.cpp:1118-1123` (replace 5 sequential checks with `if (!WaterStream::IsArmed()) return;`)

**Acceptance:**
- tier1 5/5 PASS in `MC2_RENDER_WATER_FASTPATH=1` AND unset (behavior-equivalent for the existing two call sites — pure refactor).
- No new env flags activated. No new behavior.
- Diff stat: ~10 LOC added to `gos_terrain_water_stream.{h,cpp}`, ~10 LOC removed/changed in `terrain.cpp`.

### Stage 2 — Gate (default-off skip behavior)

**Files:**
- `mclib/quad.cpp:773-1100` (wrap inside the existing water-bit OR gate at `quad.cpp:775-778` with `if (BeginLegacyWaterCluster()) { ...legacy block... } else { waterHandle = waterDetailHandle = 0xffffffff; }` per D2)
- `mclib/quad.cpp` (top of file, anonymous namespace) — flip `BeginLegacyWaterCluster()` from Stage 0 stub (`return true`) to the real implementation per D2

**Acceptance ladder (mirrors renderWater Stage 2):**

A. **Visual canary** (D5):
- mc2_01 smoke camera, frames 30-60, side-by-side legacy / fastpath / fastpath+skip → no visible delta.
- mc2_17 smoke camera, same window → no visible delta.

B. **Tracy delta** in the named zone (`setupTextures water vertex projection` at `quad.cpp:780`):
- **Target: ≥50% reduction** vs the Stage 0 baseline. Firm target per user spec prompt. If Stage 0 baseline is below 200 µs (zone is below σ-noise), slice abandonment is the correct action — code-clarity alone isn't worth shipping a perf gate that can't pass. **In that case: do not land Stage 2** (Stage 0 audits + scaffolding stay; Stage 1 is a pure refactor and is independently shippable). Per advisor B6, "git revert Stage 1" was incorrect — Stage 1 doesn't carry the perf hypothesis.
- **Both time AND hit-count must be measured** (advisor A3). The named Tracy zone lives INSIDE the skipped block; a successful skip removes the zone samples entirely, which is correct but means the perf delta is "zone goes from N samples to ~0." Validation requires:
  - `WaterStream::Counters_GetSkippedProjectedQuads()` (bumped per quad inside `BeginLegacyWaterCluster()` when its return value is `false`) reports a nonzero per-frame rate that matches `Counters_GetWaterBearingQuads()` rate (≥99% on stable missions per the Stage 2 expected values in D4) — proves the skip is actually reducing work, not silently no-opping.
  - Tracy zone time reduction ≥50% vs Stage 0 baseline OR zone disappears with skipped-quad counter showing the displaced workload.

C. **Coverage** (per D3, no new parity flag):
- `MC2_RENDER_WATER_PARITY_CHECK=1` remains silent on the new config (`MC2_RENDER_WATER_FASTPATH=1 + MC2_WATER_PROJECT_SKIP=1`) — same target as renderWater Stage 3 (zero mismatches across tier1).
- **Vacuous-silence guard (advisor B3):** parity log MUST report a nonzero `quads_checked` count that is **comparable to the pre-skip baseline** (within ±10% of the SKIP-unset run's checked-quad count). A silent log because the legacy stream was bypassed (parity walks the legacy emit and finds nothing to compare) is a FAIL, not a PASS. The check shape is "comparable count with zero mismatches," not "any count with zero mismatches." Verify by running `MC2_RENDER_WATER_FASTPATH=1 MC2_RENDER_WATER_PARITY_CHECK=1` (without SKIP) first to establish the comparison baseline; then re-run with SKIP=1; checked-quad counts must agree.
- Stage 0 DEV-AUDIT result already validated the write set.

D. **tier1 5/5 PASS triple**:
1. unset (baseline)
2. `MC2_RENDER_WATER_FASTPATH=1` (post-2026-04-30 baseline)
3. `MC2_RENDER_WATER_FASTPATH=1 MC2_WATER_PROJECT_SKIP=1` (target)
- +0 destroys delta on every mission, every state.

E. **Mouse-pick manual canary**:
- Load mc2_17. Click on a unit standing in shallow water near a shoreline tile. Selection succeeds → pass. Selection misses or selects an adjacent unit → fail (D6 tripwire).

**Default-off — Stage 2 lands with `MC2_WATER_PROJECT_SKIP` defaulting to OFF.** Stage 2 ships the infrastructure; the user opts into the skip via env flag during soak.

### Stage 3 — Default-on flip (post-soak; gated on renderWater fast-path default-on)

**Hard predecessor (advisor B2):** Stage 3 of THIS slice CANNOT ship until `MC2_RENDER_WATER_FASTPATH` is itself default-on. `IsArmed()` returns false when the renderWater fast-path env var is unset — so flipping `BeginLegacyWaterCluster()` to "skip by default" while renderWater fast-path is still default-off would have NO effect at default config (legacy block runs as before). Worse, the `=0` opt-out path would also be no-op (block already runs). The rollback contract becomes vacuous.

The dependency chain is: **renderWater Stage 4** (default-on flip of `MC2_RENDER_WATER_FASTPATH`, currently soaking as of 2026-05-02 per orchestrator) → **this slice's Stage 3**. Two acceptable patterns:

- **(A) Sequential ship:** wait for renderWater's own promotion, then run this slice's Stage 3. Cleanest. Recommended.
- **(B) Coupled flip:** Stage 3 of this slice ALSO flips `MC2_RENDER_WATER_FASTPATH`'s default. Bundles two soak-validated promotions into one commit. Acceptable if renderWater's own soak gate has passed but the promotion commit hasn't landed yet. Risks: if either flag's soak isn't actually clean, both regress together.

Spec recommends (A). If (B) is chosen, the commit message must cite both slices' soak evidence.

**Soak duration:** ≥1 week of user-driven sessions with `MC2_WATER_PROJECT_SKIP=1` set AND renderWater fast-path armed. No mouse-pick complaints. No visual regressions reported.

**Mechanic:** flip `BeginLegacyWaterCluster()`'s `EnvEnabled` defaultValue from `false` to `true`. Mirror the `MC2_MODERN_TERRAIN_PATCHES` flip at commit `aee39cc` form. The `EnvEnabled` parser already handles `=0`/`=false` opt-out correctly (advisor B1) — no other code change needed.

**Acceptance:**
- tier1 5/5 PASS in 3 configs: unset (default-on skip), `MC2_WATER_PROJECT_SKIP=0` (legacy block runs), `MC2_RENDER_WATER_PARITY_CHECK=1` (renderWater Stage 3 parity still silent).
- Update `memory/renderwater_fastpath_stage2.md` index entry to reference this slice as the upstream-cleanup follow-up.
- Update orchestrator Status Board: move "Water vertex projection skip" from "Queued" to "Shipped."
- Add a memory file `memory/water_projection_skip_default_on.md` analogous to `memory/patchstream_shape_c.md`.

---

## Acceptance gates summary

| Gate | Stage | Mechanism | Target |
|---|---|---|---|
| 0a. M2 audit | 0 | `hazeFactor` reader/writer survey | Outcome A: no reachable non-fast-path reader depends on skipped writes (advisor A2 refinement) |
| 0b. M3 audit | 0 | `waterHandle`/`waterDetailHandle` reader survey | `drawWater` is sole reader (or all other readers tolerate sentinel) |
| 0c. Tracy baseline | 0 | Capture pre-skip zone time + water-bearing quad count on **mc2_17, max zoom-out, southern coastline, frames 60-120 post-warmup** (per detailed Stage 0 acceptance) | Used as Stage 2 Gate B reference (BOTH time AND hit-count) |
| 0d. DEV-AUDIT | 0 | Counter-based per-write-site instrumentation on mc2_17 | Captured write set covers recon's documented-waste with no surprises; code REMOVED before Stage 1 |
| 0e. Env semantics | 0 | tier1 5/5 PASS with `MC2_WATER_PROJECT_SKIP=0` set | Identical to unset (advisor B1 contract) |
| A. Visual canary | 2 | mc2_01 + mc2_17 frame-window screenshot diff (legacy/fastpath/fastpath+skip) | No visible delta |
| B. Tracy delta + hit-count | 2 | Zone time on **mc2_17 same camera setup as Gate 0c** AND `WaterStream::Counters_GetSkippedProjectedQuads()` rate | ≥50% time reduction OR zone-disappears-with-counter-displacement; abandon if Stage-0 baseline <200 µs |
| C. Coverage (vacuous-silence guard) | 2 | Existing `MC2_RENDER_WATER_PARITY_CHECK=1` silent AND `quads_checked` count comparable to pre-skip baseline (±10%) | Zero mismatches AND nonzero-comparable-count (advisor B3) |
| D. Smoke triple | 2 | tier1 5/5 PASS in 3 env configs (unset / FASTPATH=1 / FASTPATH=1+SKIP=1) | +0 destroys delta on every (mission × config) |
| E. Mouse-pick canary | 2 | mc2_17 manual unit-select-in-water | Selection succeeds on first click |
| F. Predecessor (advisor B2) | 3 | `MC2_RENDER_WATER_FASTPATH` is default-on | Sequential or coupled flip; cannot ship Stage 3 of THIS slice while renderWater fast path is default-off |
| G. Soak | 3 | ≥1 week user-driven with `MC2_WATER_PROJECT_SKIP=1` | No mouse-pick / visual regression reports |
| H. Default-on flip | 3 | tier1 5/5 PASS in 3 configs (default-on, opt-out, renderWater parity) | Same as D but with default flipped |

---

## Code-grounding verification appendix

Every cited symbol grep'd at write-time. Format: `<symbol>` — `<file:line>` — `<status>`.

| Symbol / claim | File:line | Status |
|---|---|---|
| `namespace WaterStream {` | `GameOS/gameos/gos_terrain_water_stream.h:23` | matches claim — namespace, NOT class |
| `bool IsReady();` | `gos_terrain_water_stream.h:206` | matches claim — namespace-scope free function |
| `uint32_t GetRecipeCount();` | `gos_terrain_water_stream.h:203` | matches claim |
| Existing armed predicate Site 1 (`s_fastPath && IsReady() && GetRecipeCount() > 0 && terrainTextures2 != nullptr`) | `mclib/terrain.cpp:1046-1051` | matches claim — verified by Read |
| Existing armed predicate Site 2 (`if (!s_fastPath) return; if (!IsReady()) return; if (GetRecipeCount() == 0) return; if (!terrainTextures2) return;`) | `mclib/terrain.cpp:1118-1123` | matches claim — verified by Read |
| Water projection block start (`// NEW(tm) water texture code here.`) | `mclib/quad.cpp:773-774` | matches claim — verified by Read |
| Outer water-bit OR gate | `mclib/quad.cpp:775-778` | matches claim |
| `ZoneScopedN("setupTextures water vertex projection")` | `mclib/quad.cpp:780` | matches claim (per recon Section A.1; not re-Read in this session — recon's verification stands) |
| `addTriangleBulk(waterHandle, MC2_ISTERRAIN \| MC2_DRAWALPHA \| MC2_ISWATER, 2)` | `mclib/quad.cpp:1087` | matches claim — verified by Read |
| `addTriangleBulk(waterDetailHandle, MC2_ISTERRAIN \| MC2_DRAWALPHA \| MC2_ISWATERDETAIL, 2)` | `mclib/quad.cpp:1088` | matches claim — verified by Read |
| Existing else branch sentinels `waterHandle = 0xffffffff; waterDetailHandle = 0xffffffff;` at `quad.cpp:1090-1094` AND `1096-1100` | `mclib/quad.cpp:1090-1099` | matches claim — verified by Read (TWO separate else blocks: 1090-1094 inside `if (clipped1\|\|clipped2)`-else; 1096-1100 outside the water-bit OR gate) |
| Eight `wAlpha` writes | `mclib/quad.cpp:801, 806, 869, 874, 939, 944, 1009, 1014` | matches claim (per recon Section A.2 Grep — not re-Grep'd here, recon's verification stands) |
| `wAlpha` declaration with comment "Used to environment Map Sky onto water." | `mclib/vertex.h:95` | matches claim — verified by Grep |
| `setInverseProject(mostZ, leastW, ...)` consumer | `mclib/terrain.cpp:1732` | matches claim — verified by Grep |
| `BeginLegacySolidCluster()` precedent | `mclib/quad.cpp:105-110` | matches claim — verified by Read |
| `IsFrameSolidArmed()` declaration | `GameOS/gameos/gos_terrain_indirect.h:161` | matches claim — verified by Grep |
| `IsFrameSolidArmed` per-frame stability comment | `mclib/terrain.cpp:1704` | matches claim — verified by Grep |
| `IsFrameSolidArmed` flush-side use | `mclib/txmmgr.cpp:1331` | matches claim — verified by Grep |
| `WaterRecipe` schema does NOT include `waterHandle` field | `gos_terrain_water_stream.h:47-70` | matches claim — verified by Read (fields: `v0x..v3y`, `v0e..v3e`, `quadIdx`, `flags`, `terrainTypes`, `waterBits`. No handle.) |
| `WaterThinRecord` schema does NOT include `waterHandle` field | `gos_terrain_water_stream.h:91-99` | matches claim — verified by Read (fields: `recipeIdx`, `flags`, `_pad0`, `_pad1`, `lightRGB0..3`, `fogRGB0..3`. No handle.) |
| `kWaterRecipeSsboBinding = 5` and `kWaterThinSsboBinding = 6` | `gos_terrain_water_stream.h:106-107` | matches claim — verified by Read |
| run_smoke.py env passthrough block | `scripts/run_smoke.py:232-253` | matches claim — verified by Read (32 lines, includes MC2_RENDER_WATER_FASTPATH at 246 and MC2_RENDER_WATER_PARITY_CHECK at 247; alphabetical insertion point for new flags is between 247 and 248) |
| `txmmgr.cpp:1466-1530` water-bucket flush loop | (per recon Section C.1 — not re-Read here) | recon's verification stands |
| `txmmgr.cpp:1500` underfill detection (`if (totalVertices && (totalVertices < MAX_SENDDOWN))`) | (per recon Section C.2 — not re-Read here) | recon's verification stands |
| `MC2_MODERN_TERRAIN_PATCHES` flip commit `aee39cc` | orchestrator Status Board | cited from orchestrator (authoritative for its log) |
| M2d-overlay shape (76 ins / 2 files / single quad.cpp site) | `git show --stat 258e584` | brainstorm Q4 verified |

**Divergent entries: ZERO.**

**Two recon-pinned claims NOT re-verified this session** (recon's verification stands per chained-document-trust pattern): the eight wAlpha write line numbers, and the `txmmgr.cpp:1466-1530`/`1500` flush logic. Both are non-load-bearing for the gate placement / API design — they're recon facts that inform what gets skipped, not what gets added. If executor needs to re-verify either at Stage 2, the recon's appendix lists the grep commands.

### Symbols proposed by this spec (no grep — they don't exist yet)

- `WaterStream::IsArmed()` — proposed namespace-scope free function. Header: `gos_terrain_water_stream.h` (after line 206). Definition: `gos_terrain_water_stream.cpp`.
- `EnvEnabled()` — proposed boolean env-parser helper in `mclib/quad.cpp` anonymous namespace. Treats `=0` and `=false` as opt-out (advisor B1).
- `BeginLegacyWaterCluster()` (predicate) + `EndLegacyWaterCluster(bool)` (counter bump) — proposed `mclib/quad.cpp` namespace-scope helper PAIR. Mirrors shipped `BeginLegacySolidCluster` / `EndLegacySolidCluster` at `quad.cpp:105-110` (final adversarial review M1: split per shipped pattern). Stage 0 ships End REAL + Begin as `return true` stub; Stage 2 swaps Begin to the real env+armed check; End is unchanged across stages.
- `WaterStream::Counters_AddWaterBearingQuad()`, `Counters_AddLegacyProjectedQuad()`, `Counters_AddSkippedProjectedQuad()`, `Counters_GetWaterBearingQuads()`, `Counters_GetLegacyProjectedQuads()`, `Counters_GetSkippedProjectedQuads()`, `Counters_Reset()` — proposed namespace-scope free functions extending shipped `gos_terrain_indirect::Counters_*` pattern (the shipped pattern has Add/Get; this slice adds per-mission Reset). File-scope `long long` storage in `gos_terrain_water_stream.cpp` (matching shipped sibling type at `gos_terrain_indirect.h:109-111`; non-atomic per adversarial m4). Counter invariant `WaterBearing == LegacyProjected + SkippedProjected` asserted at summary emit.
- `WaterStream::EnvEnabled(const char*, bool)` — shared boolean env-parser helper. Single source of truth for `MC2_WATER_PROJECT_SKIP` parsing AND (future, post renderWater Stage 4) `MC2_RENDER_WATER_FASTPATH` parsing AND `gameosmain.cpp`'s banner-field setup.
- `MC2_WATER_PROJECT_SKIP` — proposed env flag. Default off (Stage 2). Stage 3 promotes to default on (kill-switch form via `EnvEnabled` parser).
- `MC2_WATER_PROJECT_SKIP_AUDIT` — proposed Stage-0-only DEV-AUDIT flag (per D3 Layer 2). NOT shipped — squashed out of Stage 0 commit per advisor B4.
- `[WATER_SKIP v1]` log schema — proposed per-mission summary line emitted at teardown.
- `[WATER_AUDIT v1]` log schema — proposed Stage-0-only DEV-AUDIT summary; not shipped.
- Banner field in `[INSTR v1]`: `water_skip_env=<0|1>` (startup banner). Per-mission summary `water_skip_env / water_bearing_quads_total / legacy_projected_quads_total / skipped_projected_quads_total / effective` emitted via `[WATER_SKIP v1]` (NOT in `[INSTR v1]`) — split per advisor rev2 #3 + named per advisor rev3 #3. `is_armed_ever` field dropped per final adversarial review m2 (redundant with `effective`).

These are intentions per worktree CLAUDE.md "Documentation Discipline" carve-out.

---

## Revision history

**Rev 2026-05-02 — adversarial review applied.** 4 MAJOR + 5 MINOR findings, resolved as follows:

| Finding | Resolution |
|---|---|
| **CRITICAL — Q2 brainstorm-vs-prompt discrepancy** (initial draft only — resolved before review proper) | User confirmed Option C. Top-of-doc discrepancy callout removed; D5/D6 + Q2 row in decision table reflect Option C cleanly. |
| **M1 — `MC2_WATER_PROJECT_SKIP_PARITY` redundant with shipped `MC2_RENDER_WATER_PARITY_CHECK`** | Flag dropped. D3 reframed as two-layer coverage: (1) existing renderWater Stage 3 parity for legacy-vs-fast-path equivalence, (2) one-time Stage-0 DEV-AUDIT for write-set verification. ~30-50 LOC removed from Stage 0/2. |
| **M2 — `hazeFactor` audit gap** (4 writes at `quad.cpp:821, 891, 961, 1031`; 15 worktree-wide reader files) | Stage-0 audit added (Gate 0a). Outcome A (water-block-sole-writer) ships; Outcome B blocks the slice. |
| **M3 — `waterHandle`/`waterDetailHandle` reader audit gap** | Stage-0 audit added (Gate 0b). Same Outcome A/B gating as M2. |
| **M4 — D1 "frame-static cache is correct" rationale inconsistent with D2's per-quad call** | Rationale removed. D1 now explicitly states per-quad call without cache, citing the shipped `IsFrameSolidArmed()` precedent at `quad.cpp:106` as the equivalent pattern. |
| **m1 — D4 banner location hand-waving** | Replaced with direct `gameosmain.cpp` citation per `memory/renderwater_fastpath_stage2.md:127`. |
| **m2 — D5 missing camera coordinate** | D5 now cites `MC2_SMOKE_SEED=0xC0FFEE` at `scripts/run_smoke.py:228` as the deterministic-by-seed identifier; reproducibility check added. |
| **m3 — Gate B unilateral relaxation** | Reverted to firm ≥50% target. Conditional fallback dropped; if Stage-0 baseline <200 µs, slice abandonment is the documented response (cheap; code-clarity alone insufficient). |
| **m4 — Q5 "leave-as-no-op" framing for `addTriangleBulk`** | Reworded: "the skip path simply doesn't reach those lines — no source-code change to lines 1087-1088 themselves." |
| **m5 — Stage 1 "byte-identical behavior" claim too strong** | Reworded: "behavior-equivalent for the existing two call sites" with function-static initialization-order note. |

**Symbols grep'd at review time:** 24 in initial spec + 4 added (`hazeFactor` worktree-wide, `ParityCompareRecipeFrame` at `gos_terrain_indirect.cpp:875`, indirect-terrain Stage 0 commit `9bfcddc` diff stat, `MC2_SMOKE_SEED` at `run_smoke.py:228`). Total 28. **Divergent entries: ZERO.**

**Rev2 2026-05-02 — external advisor review applied.** 6 blocking + 4 amendment findings, resolved as follows:

| Finding | Resolution |
|---|---|
| **B1 — `MC2_WATER_PROJECT_SKIP=0` was contradictory** (raw `getenv() != nullptr` enables skip even at `=0`) | Added `EnvEnabled()` boolean parser to D2; treats `=0` and `=false` as opt-out at all stages. New Stage 0 acceptance test (Gate 0e) verifies `=0` ≡ unset. |
| **B2 — Stage 3 default-on under-specified** (depends on `MC2_RENDER_WATER_FASTPATH` also being default-on) | Stage 3 hard-gated on renderWater Stage 4 (predecessor). New Gate F. Two ship patterns documented (sequential vs coupled). Top-of-doc predecessor dependency callout added. |
| **B3 — Parity gate vacuous-silence** (silent log could mean "checker walked nothing") | Gate C now requires nonzero `quads_checked` count comparable (±10%) to pre-skip baseline. Pre-skip parity baseline run added as a setup step. |
| **B4 — DEV-AUDIT mandatory vs optional contradiction** | DEV-AUDIT is now MANDATORY in Stage 0, not optional. New Gate 0d requires audit run + write-set verification. Audit code REMOVED before Stage 1 commits. |
| **B5 — `IsArmed()` layering compile/linkage concern** | Verified at write-time: `gos_terrain_water_stream.cpp:28-31` already includes `mclib/terrain.h` etc. — no new layer crossing introduced. D1 documents the existing dependency direction. |
| **B6 — Gate B rollback named wrong stage** ("git revert Stage 1" — wrong; Stage 1 is pure refactor) | Reworded to "do not land Stage 2." Stage 0 audits + Stage 1 refactor remain shippable independently. |
| **A1 — Audit logging shape** (per-write `printf` produces enormous output, alters timing) | Counter-based per-write-site instrumentation; one summary dump per mission/shutdown. |
| **A2 — `hazeFactor` Outcome A under-specified** ("sole-writer" not safe) | Outcome A refined: "no reachable non-fast-path reader depends on skipped writes, OR required value written elsewhere before any reader." Reader-side analysis now required, not just writer-side. |
| **A3 — Gate B should measure both time AND hit count** | Gate B now requires both: zone time reduction OR zone-disappearance-with-counter-displacement. `WaterStream::Counters_GetSkippedProjectedQuads()` rate added in Stage 0. |
| **A4 — Banner field naming precision** | Single field `water_skip` split into `water_skip_env` (requested state) and `water_skip_effective` (actual per-frame state). |

**Total findings across both reviews:** 4 MAJOR + 5 MINOR (self) + 6 blocking + 4 amendment (advisor) = 19. All resolved inline.

**Rev2-2 2026-05-02 — second advisor pass.** Rev2 surfaced 4 more blockers + 4 cleanups from a second advisor read. Resolved as follows:

| Finding | Resolution |
|---|---|
| **R2-1 — `MC2_RENDER_WATER_FASTPATH` env still raw `getenv()` in `IsArmed()`** | D1 now documents the existing-behavior-match rationale + adds an explicit cross-slice contract: when renderWater's Stage 4 default-on flip ships, `IsArmed()` must atomically adopt `EnvEnabled()` parser. THIS slice's Stage 3 cannot land while `IsArmed()` still uses raw `getenv()`. Documented as a precondition surfaced in renderWater Stage 4 plan. |
| **R2-2 — D3 still said per-write `printf`** (contradicted Stage 0's counter-based shape) | D3 Layer 2 fully rewritten to counter-based instrumentation, matching Stage 0. Single source of truth. |
| **R2-3 — `water_skip_effective` banner timing impossible** (`[INSTR v1]` is startup banner, fires before any frame; field would always be `na`) | Split across two emit points: `water_skip_env` stays in `[INSTR v1]` startup banner (operator-intent); a NEW `[WATER_SKIP v1] event=summary` line emitted at mission teardown reports actual `effective` derived from counter values. |
| **R2-4 — Stage 0 audit-code commit boundary unclear** | DEV-AUDIT instrumented as a WIP-then-squash workflow: apply patch → run → discard patch → commit ONLY scaffolding. The landed Stage 0 commit contains no audit code, no `MC2_WATER_PROJECT_SKIP_AUDIT` env flag. Audit evidence lives in commit message. |
| **R2-5 (cleanup) — Symbols proposed list missed banner-field rename** | Updated to list `water_skip_env` (banner) + `[WATER_SKIP v1]` summary line + counter symbols + `EnvEnabled` helper. |
| **R2-6 (cleanup) — Counter reset/emit cadence undefined** | Stage 0 file list now specifies: reset at mission load (in/near `WaterStream::Build()`); emit at mission teardown (`[WATER_SKIP v1] event=summary`); optional 600-frame cadence under `MC2_WATER_DEBUG=1`; per-frame rate derived from delta between Tracy capture windows. |
| **R2-7 (cleanup) — "high-zoom" Tracy baseline ambiguous** | Pinned to: mission `mc2_17`, max zoom-out, southern coastline, frames 60-120 post-warmup. Both zone time AND per-frame quad count recorded. |
| **R2-8 (cleanup) — Mouse-pick canary too vague** | Procedure pinned: pan to mc2_17 southern coastline, position friendly unit half-submerged, click. Executor records world-coords + screen-coords + before/after screenshots in commit message. |

**Cumulative findings across all 3 review passes:** 4 MAJOR + 5 MINOR (self) + 6 blocking + 4 amendment (advisor rev1) + 4 blocking + 4 cleanup (advisor rev2) = 27 findings, all resolved.

**Rev3 2026-05-02 — third advisor pass.** 3 blockers + 1 cleanup. Resolved as follows:

| Finding | Resolution |
|---|---|
| **R3-1 — Counter ownership impossible as written** (anonymous-namespace symbols in `quad.cpp` cannot be read from `gos_terrain_water_stream.cpp` summary emit site) | Counters moved into `WaterStream` namespace with `Counters_Add*` / `Counters_Get*` / `Counters_Reset` accessors, mirroring shipped `gos_terrain_indirect::Counters_*` pattern from commit `9bfcddc`. File-scope `std::atomic<uint64_t>` storage in `gos_terrain_water_stream.cpp`. `BeginLegacyWaterCluster()` calls the setters. Summary emit in `WaterStream::ReleaseGlResources()` reads via getters. No cross-TU file-scope-static reads. |
| **R3-2 — `EnvEnabled()` scoped too narrowly** (in `quad.cpp` anonymous namespace; not reachable from `gameosmain.cpp` or future `IsArmed()` adoption per renderWater Stage 4 contract) | `EnvEnabled()` declared as a `WaterStream` namespace free function in `gos_terrain_water_stream.h`; defined once in `gos_terrain_water_stream.cpp`. Three callers share the single definition: `BeginLegacyWaterCluster()` in `quad.cpp` (already includes the sibling header at `quad.cpp:43`), `gameosmain.cpp` for the banner field, future `IsArmed()` update. |
| **R3-3 — Counter naming ambiguous** (Gate B denominator unclear: was `legacy_quads_total` "took the legacy path" or "all water-bearing quads observed"?) | Renamed to `water_bearing_quads_total` (denominator) + `legacy_projected_quads_total` + `skipped_projected_quads_total` with explicit invariant `bearing == legacy + skipped`. Per-stage expected values pinned in D4. `BeginLegacyWaterCluster()` enforces the invariant by construction. |
| **R3-4 (cleanup) — Gates table mc2_01 vs detailed mc2_17 mismatch** | Table updated: Gate 0c and Gate B now both cite `mc2_17, max zoom-out, southern coastline, frames 60-120` matching the detailed Stage 0 acceptance section. |

**Cumulative across 4 review passes:** 4 MAJOR + 5 MINOR (self) + 6 blocking + 4 amendment (advisor rev1) + 4 blocking + 4 cleanup (advisor rev2) + 3 blocking + 1 cleanup (advisor rev3) = 31 findings.

**Rev4 2026-05-02 — final adversarial pass before executor handoff.** 2 MAJOR + 5 MINOR. Resolved as follows:

| Finding | Resolution |
|---|---|
| **R4-M1 — `BeginLegacyWaterCluster` violated Begin/End naming convention** (bumping counters in Begin diverges from shipped `BeginLegacySolidCluster` at `quad.cpp:105-110` which is pure-predicate; counter bump lives in `EndLegacySolidCluster`) | Split into `BeginLegacyWaterCluster()` (pure predicate) + `EndLegacyWaterCluster(bool ranLegacy)` (counter bump). Usage updated in D2 example. |
| **R4-M2 — Stage 0 stub semantics ambiguous** (file list said "returns true unconditionally — wiring deferred"; acceptance test required `water_bearing_quads_total > 0`) | Disambiguated: Stage 0 ships End REAL with full counter wiring + Begin as `return true` stub. Stage 2 swaps only Begin's body. Mirrors how indirect-terrain Stage 0 (commit `9bfcddc`) shipped Counters_Add* with no callers — API is real, wiring deferred. |
| **R4-m1 — `quad.cpp` and `gameosmain.cpp` includes not explicit** | Stage 0 file list now lists exact `#include` lines for both TUs. |
| **R4-m2 — `is_armed_ever` field undocumented derivation** | Field DROPPED as redundant with `effective` (which is `skipped > 0`). Two pieces of info — `water_skip_env` (startup banner) + `effective` (per-mission summary) — cover the truth table. |
| **R4-m3 — `<atomic>` include unstated** | Moot after m4 (atomic dropped). No new include needed. |
| **R4-m4 — atomic counter rationale speculative** | Dropped atomic; counters now `long long` matching shipped sibling at `gos_terrain_indirect.h:109-111`. MC2 terrain processing is single-threaded; atomicity was unnecessary speculation. |
| **R4-m5 — `WaterStream::Build()` line citation off-by-one** | Citation corrected from line 194 to line 195. |
| **R4-m6 — Reset call-site choice rationale missing** | Now documents: Reset-from-Build()-at-mission-start sidesteps the `Reset()`-vs-`ReleaseGlResources()` mission-unload-order ambiguity. Counter values survive unload→load gap. |
| **R4-m7 — "mirrors shipped pattern" claim partial** (shipped indirect-terrain has no Reset) | Reworded as "extends shipped pattern with per-mission Reset semantics." |

**Cumulative across all 5 review passes:** 4 MAJOR + 5 MINOR (self) + 6 blocking + 4 amendment (advisor rev1) + 4 blocking + 4 cleanup (advisor rev2) + 3 blocking + 1 cleanup (advisor rev3) + 2 MAJOR + 7 MINOR (final adversarial rev4) = **40 findings, all resolved inline.** Symbols grep'd: 30 + 4 (`Counters_AddLegacySolidSetupQuad` re-verified at `gos_terrain_indirect.h:104`, `Counters_GetLegacySolidSetupQuads` at line 109, `Build` at line 195, `Reset` at line 198, `ReleaseGlResources` at line 123) = 34. **Divergent: ZERO.**

---

## Length: ~660 lines.

Status: **READY-FOR-EXECUTOR — final.** Five review passes resolved; user-confirmed decisions integrated; verification appendix clean across 34 grep'd symbols. No outstanding contradictions.

Executor invokes `superpowers:executing-plans` to walk Stage 0 → 1 → 2 → 3. Stage 0 carries 5 stop-the-line gates (0a-0e); if any lands Outcome B / fails, the slice goes back to brainstorm. Stage 3 is hard-gated on renderWater Stage 4 (predecessor) per advisor B2 + R2-1 cross-slice contract; the contract was tightened by R3-2's shared `EnvEnabled` helper (Stage 4's adoption is a one-line change in `IsArmed()`, not a parser duplication) and finalized by R4-M1's Begin/End split (matches shipped naming convention so future hands won't trap on counter side-effects).
