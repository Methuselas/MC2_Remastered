# Track A2 — Effects Admission Predicate Replacement Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the `Camera::projectForEffectAdmission` wrapper (`mclib/camera.h:557-570`) to the same `clipSpaceFrustumAdmit` predicate established in Track A1, behind a sibling env flag (`MC2_EFFECT_ADMISSION_PREDICATE=modern|legacy`, default legacy). Screen output stays byte-identical via `projectZ`'s own write at all 7 effect callsites (1× clouds, 4× craters, 2× weather raindrops).

**Architecture:** Fan-out slice over Track A1's substrate. The dual-output wrapper pattern (capture `LegacyProjectionResult result` → `projectZ(point, screen, &result)` → either return legacy bool or `clipSpaceFrustumAdmit(result.rawClip)`) is reused unchanged. Trace's `homogClipFull` candidate is already wired (A1 Task 3) and the per-callsite heatmap already counts disagreement at every effect site (`PROJECTZ_SITE` tagged: `cloud_vertex_screen`, `crater_corner0..3`, `weather_raindrop_top`, `weather_raindrop_bot`). The only new code is (a) a sibling env probe + accessor for the effect-side mode, (b) the wrapper body change, (c) a banner-line addition.

**Tech Stack:** C++ (engine, MSVC RelWithDebInfo), MC2 OpenGL renderer, existing `projectz_trace.{h,cpp}` infrastructure, `[DESTROY v1]` lifecycle instrumentation, `scripts/run_smoke.py` tier1 gate.

**Spec references:**
- Decisions: `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` (Q1, Q2, Q3 + post-advisor revisions)
- Roadmap: `docs/superpowers/mc3-rendering-modernization-roadmap.md` (Track A § A2)
- Recon: `docs/superpowers/explorations/2026-05-06-track-a-predicate-replacement-recon.md`
- Wrapper substrate: `docs/superpowers/specs/projectz-policy-split-report.md` (effect wrapper at row 58, 7 sites)
- Callsite inventory: `docs/superpowers/specs/projectz-callsite-inventory.md`
- **A1 plan (substrate inheritance):** `docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md`

**Inheritance from A1 (do NOT redo):**
- `clipSpaceFrustumAdmit(const Stuff::Vector4D&)` predicate function (A1 Task 1-2). Reused as-is.
- `LegacyProjectionResult` capture + `projectZ(.., LegacyProjectionResult*)` 3-arg signature (A1 Task 0 substrate, already in `mclib/camera.h:436`).
- `homogClipFull` trace candidate predicate, computed via `clipSpaceFrustumAdmit` (A1 Task 3). The trace already evaluates it on every projectZ call regardless of which wrapper invoked — including all 7 effect sites. **No trace changes for A2.**
- Eight-case selftest runner + `MC2_OBJECT_ADMISSION_SELFTEST=1` gate (A1 Task 2/4). The selftest exercises the predicate function itself; A2 reuses the same predicate so no new cases are needed.
- `[INSTR v1] object_admission_mode=...` startup banner pattern (A1 Task 4). A2 adds a sibling line.
- Acceptance-envelope authoring methodology + DESTROY count+identity diff (A1 Task 6/7).
- Soak + flip cadence (A1 Task 8/9).

**Worktree CLAUDE.md rules in force:**
- Build: `cmake --build build64 --config RelWithDebInfo`
- Stock install must remain playable (default = legacy until soak passes)
- Tier1 smoke is the regression gate
- Debug instrumentation rule: lifecycle reworks land env-gated `[SUBSYS v1]` prints in same commit
- Documentation discipline: every cited symbol grep-verified

**Five advisor sharpenings (inherited from A1, applied as background guardrails):**
1. Lazy-init env probe (already in A1 substrate; A2 mirrors).
2. Selftest hard-fail on any case failure (inherited; A2 adds no new cases — predicate is unchanged).
3. Trace and production share `clipSpaceFrustumAdmit` (already true post-A1 by construction).
4. Single-run heatmap capture for envelope authoring (not byte-diff across two runs).
5. DESTROY identity diff in addition to count parity (kind+reason+gate-state-snapshot tuple after pointer/frame normalization).

---

## Per-site grep findings (Task-zero recon — done at plan-write time)

The 7 effect-admission callsites, with screen-output consumption confirmed:

| # | File:Line | PROJECTZ_SITE id | Screen consumption | Implicit z-dependency? |
|---|-----------|------------------|--------------------|------------------------|
| 1 | `mclib/clouds.cpp:212` | `cloud_vertex_screen` | `cloudVertices[i].px/py/pz/pw = screenPos.x/y/z/w` | Yes — `pz` (screen.z) feeds depth-sort and rendering |
| 2 | `mclib/crater.cpp:323` | `crater_corner0` | `currCrater->screenPos[0].{x,y,z,w}` cached for later draw at lines 452-455 | Yes — `screen.z` written into `gVertex.z` |
| 3 | `mclib/crater.cpp:326` | `crater_corner1` | `currCrater->screenPos[1].{x,y,z,w}` cached at lines 461-464 | Yes — same as #2 |
| 4 | `mclib/crater.cpp:329` | `crater_corner2` | `currCrater->screenPos[2].{x,y,z,w}` cached at lines 470-473 | Yes — same as #2 |
| 5 | `mclib/crater.cpp:332` | `crater_corner3` | `currCrater->screenPos[3].{x,y,z,w}` cached at lines 479-482 | Yes — same as #2 |
| 6 | `code/weather.cpp:489` | `weather_raindrop_top` | `screen1.x/y/z/w` written to `gos_VERTEX[0]`. **Plus** `screen1.z` consumed at line 500: `amb = ambientFactor * (1.0f - screen1.z)` for fog/ambient color. | Yes — explicit screen.z fog |
| 7 | `code/weather.cpp:497` | `weather_raindrop_bot` | `screen2.x/y/z/w` written to `gos_VERTEX[1]`. | Yes — screen.z written through |

**Verdict:** all 7 sites consume `screen.{x,y,z,w}` (including `.z`) downstream. Site #6 (`weather_raindrop_top`) has the most explicit z-dependency: it computes `amb = ambientFactor * (1.0f - screen1.z)`. **None requires special handling under the dual-output wrapper pattern**, because `projectZ` writes `screen.x/y/z/w` byte-identically regardless of which bool path the wrapper takes — the only thing the predicate change touches is the returned bool. The screen oracle is preserved by construction.

**Comparison with policy-split report's "Follow-Up Tickets" callsites:** the report flagged `weaponbolt_beam_*`, `mine_cell_corner*`, `actor_vfx_top_depth` as having screen.z fog/lighting consumers. Those route through `projectForLightingShadow` (not effect admission), so are not in A2's scope. The A2 sites' screen.z usage is identical-pattern (cosmetic/depth fog) and identical-safe under the wrapper.

**Sole nuance:** site #6 (raindrop_top) reads screen1.z BEFORE the bool is checked at line 490 — actually no, re-reading: line 490 is `if (onScreen)`, and line 500 is inside that `if`. So `screen1.z` is only consumed if the wrapper returned true. Under modern mode, fewer rejected drops will reach line 500 (or different ones will), which means **fewer/different raindrops get drawn**. This is the expected migration signal, not a bug. Counted in the acceptance envelope.

**Conclusion:** zero callsite code changes. The wrapper change is the entire production code delta.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `mclib/object_admission_predicate.h` | Modify | Extend with sibling enum `EffectAdmissionPredicateMode`, `effectAdmissionPredicate_init()`, `effectAdmissionPredicateMode()`. Co-locating in the same TU as A1 keeps the predicate function single-source-of-truth. |
| `mclib/object_admission_predicate.cpp` | Modify | Implement `effectAdmissionPredicate_init()` (probes `MC2_EFFECT_ADMISSION_PREDICATE`) and `effectAdmissionPredicateMode()` (lazy-init accessor). Emit `[INSTR v1] effect_admission_mode=<legacy|modern>` banner. |
| `mclib/camera.h:557-570` | Modify | `projectForEffectAdmission` wrapper body — mirror the A1 wrapper structure (`LegacyProjectionResult` capture + mode-gated branch). |
| `GameOS/gameos/gameosmain.cpp` (or whichever owns `[INSTR v1]` banner per A1 Task 4 grep) | Modify | Call `effectAdmissionPredicate_init()` next to the A1 init call. |
| `docs/superpowers/specs/2026-05-06-track-a2-acceptance-envelope.md` | Create | Operator-authored acceptance envelope (post-Task 5 capture). |
| `~/.claude/projects/A--Games-mc2-opengl-src/memory/track_a2_effect_admission_predicate.md` | Create | Memory file capturing the slice's flip-on date and soak findings. |
| `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` | Modify | Index entry for the new memory file. |

**Decomposition rationale:** the effect-mode probe is a sibling enum in the **same** translation unit as the A1 object-mode probe, not a separate file. Justification: the predicate function `clipSpaceFrustumAdmit` is shared; splitting the env probes into separate TUs would invite drift in the lazy-init pattern for no gain. The two enums + two accessors live side-by-side, share the same header guard, and use the same `[INSTR v1]` banner scheme. **Tradeoff considered:** a sibling header `effect_admission_predicate.h` would isolate the slice's name, but at the cost of duplicating the lazy-init boilerplate (~15 lines). The single-TU choice keeps both probes within ~50 lines of each other for grep-and-compare, which matters more for a fan-out slice.

---

> **Task ordering note:** Tasks proceed in the same order as A1 (header → impl → wrapper → smoke → envelope → DESTROY parity → soak → flip), but with several A1 tasks dropped (no new predicate function, no new selftest cases, no new trace candidate). The total task count is 8 versus A1's 10.

## Task 1 — Header extension: sibling enum, init, accessor

Extend `mclib/object_admission_predicate.h` with the effect-side declarations. The predicate function `clipSpaceFrustumAdmit` is unchanged — it's the same function used by both modes.

**Files:**
- Modify: `mclib/object_admission_predicate.h`

- [ ] **Step 1.1 — Append effect-mode declarations to the header**

After the existing object-mode declarations (after the `objectAdmissionPredicate_selftest` declaration), append:

```cpp
//---------------------------------------------------------------------------
// Track A2 — effect admission mode (sibling of the Track A1 object mode).
// Same clipSpaceFrustumAdmit predicate; separate env flag so the two
// slices can be flipped on independently during soak.
//---------------------------------------------------------------------------

enum class EffectAdmissionPredicateMode {
    Legacy,    // default: bool comes from projectZ's screen-rect test
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe. Idempotent. Called lazily from effectAdmissionPredicateMode().
void effectAdmissionPredicate_init();

// Read the active mode. Lazy-initializes — startup ordering is non-load-bearing.
EffectAdmissionPredicateMode effectAdmissionPredicateMode();
```

(No new selftest entry — A1's `objectAdmissionPredicate_selftest()` exercises `clipSpaceFrustumAdmit`, which is the function A2 reuses.)

- [ ] **Step 1.2 — Commit**

```bash
git add mclib/object_admission_predicate.h
git commit -m "feat(track-a2): add effect admission mode declarations

Sibling of Track A1's object-mode enum/init/accessor. Same
clipSpaceFrustumAdmit predicate, separate env flag
(MC2_EFFECT_ADMISSION_PREDICATE) so the slices flip independently.
No selftest extension — predicate function unchanged from A1.

Plan: docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md
"
```

---

## Task 2 — Implement effect-mode probe + accessor

**Files:**
- Modify: `mclib/object_admission_predicate.cpp`

- [ ] **Step 2.1 — Append effect-mode implementation**

In `mclib/object_admission_predicate.cpp`, after the object-side namespace and accessor (right before the predicate body or selftest, but in the same anonymous namespace if applicable), add:

```cpp
namespace {
bool                          s_effectInitialized = false;
EffectAdmissionPredicateMode  s_effectMode = EffectAdmissionPredicateMode::Legacy;

const char* effectModeLabel(EffectAdmissionPredicateMode m) {
    return (m == EffectAdmissionPredicateMode::Modern) ? "modern" : "legacy";
}
} // namespace

void effectAdmissionPredicate_init() {
    if (s_effectInitialized) return;
    const char* env = std::getenv("MC2_EFFECT_ADMISSION_PREDICATE");
    if (env && std::strcmp(env, "modern") == 0) {
        s_effectMode = EffectAdmissionPredicateMode::Modern;
    } else {
        s_effectMode = EffectAdmissionPredicateMode::Legacy;
    }
    s_effectInitialized = true;
    std::printf("[INSTR v1] effect_admission_mode=%s\n", effectModeLabel(s_effectMode));
    std::fflush(stdout);
}

EffectAdmissionPredicateMode effectAdmissionPredicateMode() {
    // Lazy init — startup ordering is non-load-bearing.
    effectAdmissionPredicate_init();
    return s_effectMode;
}
```

(The anonymous-namespace duplicate-name guard: if the existing object-side namespace already defines `s_initialized`, the effect-side variables must be uniquely named — `s_effectInitialized`/`s_effectMode` per above. The label helper has a different name `effectModeLabel` to avoid overload ambiguity.)

- [ ] **Step 2.2 — Build**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: clean build. If the linker complains about duplicate `modeLabel`, rename one. If `<cstdlib>`/`<cstring>` aren't already included from A1's TU header, add them.

- [ ] **Step 2.3 — Commit**

```bash
git add mclib/object_admission_predicate.cpp
git commit -m "feat(track-a2): implement effect admission probe + lazy accessor

MC2_EFFECT_ADMISSION_PREDICATE=modern|legacy probed lazily on first
accessor call. [INSTR v1] effect_admission_mode=<mode> banner emitted
exactly once. Mirrors A1's object-mode probe pattern.

Plan: docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md
"
```

---

## Task 3 — Wire startup probe call

The init function is idempotent and emits its own banner; the existing `[INSTR v1]` aggregate doesn't need to change.

**Files:**
- Modify: whichever file owns A1's `objectAdmissionPredicate_init()` call (per A1 Task 4 grep — likely `GameOS/gameos/gameosmain.cpp`)

- [ ] **Step 3.1 — Locate the A1 init call**

```bash
grep -rn "objectAdmissionPredicate_init" GameOS/ code/ mclib/ | head -5
```

Expected: one site, near other `[INSTR v1]` banner-emitting init calls.

- [ ] **Step 3.2 — Add the effect-side init call directly below**

Right after the line `objectAdmissionPredicate_init();`, add:

```cpp
effectAdmissionPredicate_init();
```

(The header `object_admission_predicate.h` is already included from A1 Task 4; no new include needed.)

- [ ] **Step 3.3 — Build, run smoke (legacy mode, banner check)**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 10 --kill-existing
```

Expected log lines at startup:

```
[INSTR v1] object_admission_mode=legacy
[INSTR v1] effect_admission_mode=legacy
```

Both banners present, both default-legacy. No behavior change yet.

- [ ] **Step 3.4 — Commit**

```bash
git add <gameosmain.cpp or whichever file>
git commit -m "feat(track-a2): wire effect admission startup probe

effectAdmissionPredicate_init() called next to objectAdmissionPredicate_init()
at engine startup. [INSTR v1] effect_admission_mode=legacy emitted in
banner header. No behavior change — wrapper still uses legacy bool.

Plan: docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md
"
```

---

## Task 4 — Wire the wrapper to use the new predicate behind the env flag

**Files:**
- Modify: `mclib/camera.h:557-570` (the `projectForEffectAdmission` wrapper body)

- [ ] **Step 4.1 — Verify the include is already present**

A1 Task 5 added `#include "object_admission_predicate.h"` near the top of `mclib/camera.h`. Confirm:

```bash
grep -n "object_admission_predicate.h" mclib/camera.h
```

Expected: one match. If absent, add it (this would only happen if A1 was reverted; not expected).

- [ ] **Step 4.2 — Replace the wrapper body**

Current body (lines 557-570):

```cpp
inline bool projectForEffectAdmission (Stuff::Vector3D& point,
                                       Stuff::Vector4D& screen) {
#pragma warning(push)
#pragma warning(disable: 4996)
    bool accepted = projectZ(point, screen);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
    if (accepted) {
        gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
                  isfinite(screen.z) && isfinite(screen.w));
    }
#endif
    return accepted;
}
```

Replace with:

```cpp
inline bool projectForEffectAdmission (Stuff::Vector3D& point,
                                       Stuff::Vector4D& screen) {
    LegacyProjectionResult result;
#pragma warning(push)
#pragma warning(disable: 4996)
    // projectZ writes screen byte-identically to legacy; we capture rawClip
    // via the optionalResult sidecar so the modern predicate can see it.
    bool legacyAccepted = projectZ(point, screen, &result);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
    // Invariant gates on legacy-rect-acceptance (the original semantics),
    // not on the bool we ultimately return — preserves the policy-split
    // contract from commit cc83857. Same as Track A1's object wrapper.
    if (result.acceptedByLegacyScreenRect) {
        gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
                  isfinite(screen.z) && isfinite(screen.w));
    }
#endif
    if (effectAdmissionPredicateMode() == EffectAdmissionPredicateMode::Modern) {
        return clipSpaceFrustumAdmit(result.rawClip);
    }
    return legacyAccepted;
}
```

- [ ] **Step 4.3 — Build**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: clean build. `LegacyProjectionResult`, `clipSpaceFrustumAdmit`, `effectAdmissionPredicateMode`, `EffectAdmissionPredicateMode::Modern` are all already in scope via the A1 substrate + Task 1.

- [ ] **Step 4.4 — Tier1 smoke (legacy mode, sanity)**

The wrapper's behavior must be unchanged when env is unset (default = legacy). Confirm before flipping anything else:

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: tier1 5/5 PASS. Both `[INSTR v1]` banner lines appear at log header. No visual regressions in any of the 5 stable missions.

- [ ] **Step 4.5 — Tier1 smoke (modern mode, first observation)**

```bash
MC2_EFFECT_ADMISSION_PREDICATE=modern \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: tier1 5/5 PASS, `[INSTR v1] effect_admission_mode=modern` in each header (object_admission stays whatever its default is at the time of A2 — possibly modern post-A1 flip, possibly legacy mid-soak; either is fine).

**If a mission fails:** capture artifact directory (`tests/smoke/artifacts/<timestamp>/`), inspect `[DESTROY v1]` line count delta and any visual smoke fail signal. Do NOT proceed to Task 5 until tier1 passes under modern.

**Watch effect-specific signals:**
- `mc2_03` is a rain mission — observe whether raindrops still appear (they may shift count slightly under modern; complete absence is a regression).
- `mc2_24` is the wolfman/explosions canary — craters from explosions should still appear; cloud shadows should still cover the map.

- [ ] **Step 4.6 — Commit**

```bash
git add mclib/camera.h
git commit -m "feat(track-a2): wire effect admission wrapper to clip-space predicate

projectForEffectAdmission now captures rawClip via LegacyProjectionResult
and routes the bool through the env-flag-selected predicate. Default
(MC2_EFFECT_ADMISSION_PREDICATE unset) preserves legacy behavior — screen
output is byte-identical via projectZ's existing path. Identical-shape
swap as Track A1's object wrapper.

7 callsites consume screen.{x,y,z,w} downstream (cloud_vertex_screen,
crater_corner0..3, weather_raindrop_top/bot); none requires special
handling — screen oracle is preserved by construction.

Tier1 5/5 PASS verified in both legacy (default) and modern modes.

Plan: docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md
"
```

---

## Task 5 — Capture acceptance envelope from in-run dual-predicate trace

The trace already evaluates `homogClipFull` per call at every effect callsite (A1 Task 3 wired this for ALL `PROJECTZ_SITE`-tagged sites including the 7 effect ones). One smoke run yields per-callsite `homogClipFull_disagreements` directly. Same methodology as A1 Task 6.

**Methodology note:** envelope built from per-callsite disagreement summaries inside a SINGLE smoke run (advisor sharpening #4). A second run in the opposite mode is a sanity check, not a byte-diff source.

**Files:**
- Create: `docs/superpowers/specs/2026-05-06-track-a2-acceptance-envelope.md`

- [ ] **Step 5.1 — Capture single-run heatmap (tier1, modern mode)**

```bash
MC2_EFFECT_ADMISSION_PREDICATE=modern \
MC2_PROJECTZ_TRACE=1 MC2_PROJECTZ_HEATMAP=1 MC2_PROJECTZ_SUMMARY=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected output: per-mission `[PROJECTZ v1] callsite=<id> ... homogClipFull_disagreements=N total_calls=M` summary lines for each of the 7 effect callsites.

Capture:

```bash
LATEST=$(ls -td tests/smoke/artifacts/*/ | head -1)
for site in cloud_vertex_screen crater_corner0 crater_corner1 crater_corner2 crater_corner3 weather_raindrop_top weather_raindrop_bot; do
  echo "=== $site ==="
  grep "callsite=$site" "$LATEST"/*.log
done > /tmp/track-a2-heatmap-summary.txt
cat /tmp/track-a2-heatmap-summary.txt
```

- [ ] **Step 5.2 — Sanity-check second run (tier1, legacy mode)**

```bash
MC2_PROJECTZ_TRACE=1 MC2_PROJECTZ_HEATMAP=1 MC2_PROJECTZ_SUMMARY=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
LATEST=$(ls -td tests/smoke/artifacts/*/ | head -1)
for site in cloud_vertex_screen crater_corner0 crater_corner1 crater_corner2 crater_corner3 weather_raindrop_top weather_raindrop_bot; do
  echo "=== $site ==="
  grep "callsite=$site" "$LATEST"/*.log
done > /tmp/track-a2-heatmap-summary-legacy.txt
```

Compare the two summaries informally. The wrapper-mode change can affect later-frame `total_calls` per site (e.g., `crater_corner*` counts shift if modern admits/rejects differently which affects whether crater submission happens at all — though the 4 corners are evaluated before the OR-combine, so corner counts should be stable). Large drift (>20%) at any site indicates predicate behavior cascading; investigate before envelope authoring.

- [ ] **Step 5.3 — Author the acceptance envelope spec**

Path: `docs/superpowers/specs/2026-05-06-track-a2-acceptance-envelope.md`

Template:

```markdown
# Track A2 — Effect Admission Acceptance Envelope

Captured: <date>
Source: tests/smoke/artifacts/<timestamp>/*.log
Predicate compared: homogClipFull (clipSpaceFrustumAdmit on rawClip) vs legacyRect
Sites: cloud_vertex_screen, crater_corner0..3, weather_raindrop_top, weather_raindrop_bot

## Per-site disagreement counts (modern run)

| Mission | Site | Total calls | homogClipFull disagreements | %  |
|---------|------|-------------|------------------------------|-----|
| mc2_01  | cloud_vertex_screen | <N> | <D> | <P> |
| mc2_01  | crater_corner0      | <N> | <D> | <P> |
| ...     | ...                 | ... | ... | ... |
| mc2_03  | weather_raindrop_top | <N> | <D> | <P> |
| mc2_03  | weather_raindrop_bot | <N> | <D> | <P> |
| mc2_24  | crater_corner0..3   | <N> | <D> | <P> |

(Fill in all 5 missions × 7 sites = 35 rows. Sites that report 0 calls in
a mission — e.g., no rain in mc2_01 — are flagged as "n/a".)

## Candidate disagreement classes (hypotheses)

These classes are PREDICTED to differ between modern and legacy by design.
Confirm or refute against captured data; refine the list.

- **Behind-camera (rawClip.w ≤ 0):** modern rejects unconditionally; legacy
  accepts if the rhw-divided screen coord wraps into viewport rect (sign-
  destruction by `screen.w = fabs(rhw)`). Concentrated at: cloud vertices
  beyond camera (CLOUD_ALTITUDE high above eye), raindrops behind frustum
  near far plane.
- **Tiny-w near-camera:** modern admits cleanly; legacy may reject when
  the post-divide screen coord lands outside the viewport rect at extreme
  near-plane positioning. Rarer for effects (raindrops drop down past
  player camera, craters are ground-pinned).
- **Frustum-corner inclusive boundary:** modern admits at `|x|=w` etc.
  inclusively; legacy rect-test admits at viewport-edge inclusively. Tiny
  delta — likely <1% of total disagreements.

## Out-of-envelope conditions (HARD FAILURE)

- Disagreement count delta >5x baseline at any single mission/site.
- Disagreements at sites that don't fit the (refined) class list.
- `[DESTROY v1]` count delta >0 between legacy-mode and modern-mode tier1 runs (Task 6 — cascade-safety check). **Note for A2:** effect admission does NOT directly feed object lifecycle (clouds/craters/raindrops are not GameObjects in the cull-cascade sense; they don't `setExists(false)`). DESTROY parity is therefore expected to be trivially zero, but we still verify it as a defense against unforeseen cross-system coupling.
- Visual regression at the effect-specific canaries (Task 7 soak): missing rain in mc2_03, missing craters from mech-death explosions in mc2_24, cloud shadows missing or jittering at zoom transitions.

## Procedure for envelope refresh

Re-run Step 5.1 after any change to:
- The clipSpaceFrustumAdmit body (which would also affect Track A1 — coordinate)
- Camera::projectZ (legacy reference)
- Effect-emitting code that adds/removes callsites (e.g., a new weather kind, a different crater grid)

If disagreement counts shift outside ±20% of envelope, gate the change.
```

Replace `<N>`, `<D>`, `<P>`, `<date>`, `<timestamp>` with values from the captures.

- [ ] **Step 5.4 — Commit the envelope**

```bash
git add docs/superpowers/specs/2026-05-06-track-a2-acceptance-envelope.md
git commit -m "docs(track-a2): author acceptance envelope from dual-run capture

Per Q3 of brainstorm-decisions: parity gate is dual-run with reviewed
envelope, not zero-disagreement. This doc captures the envelope at
post-Task-4 baseline; future Track A2-touching changes verify against it.

Plan: docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md
"
```

---

## Task 6 — `[DESTROY v1]` parity verification (count + identity diff)

Effect admission does not directly destroy GameObjects (clouds/craters/raindrops are not in the cull-cascade), so DESTROY count delta is expected to be **trivially zero**. The check is a defense-in-depth against unforeseen coupling — e.g., a future change that ties effect emission to mech-state lifecycle. Same methodology as A1 Task 7.

**Two-tier check:** count parity is the floor; identity parity is the upper bound. Same `[DESTROY v1]` line normalization as A1 (advisor sharpening #5).

**Files:**
- Modify: `docs/superpowers/specs/2026-05-06-track-a2-acceptance-envelope.md` (append parity table)

- [ ] **Step 6.1 — Capture DESTROY events in legacy mode**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
LATEST=$(ls -td tests/smoke/artifacts/*/ | head -1)
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "\[DESTROY v1\]" "$LATEST"/${m}*.log | wc -l \
    > /tmp/track-a2-destroy-legacy-${m}.count
  grep "\[DESTROY v1\]" "$LATEST"/${m}*.log \
    | sed -E 's/obj=0x[0-9a-fA-F]+/obj=PTR/; s/frame=[0-9]+/frame=N/' \
    > /tmp/track-a2-destroy-legacy-${m}.norm
done
```

- [ ] **Step 6.2 — Capture DESTROY events in modern mode**

```bash
MC2_EFFECT_ADMISSION_PREDICATE=modern \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
LATEST=$(ls -td tests/smoke/artifacts/*/ | head -1)
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "\[DESTROY v1\]" "$LATEST"/${m}*.log | wc -l \
    > /tmp/track-a2-destroy-modern-${m}.count
  grep "\[DESTROY v1\]" "$LATEST"/${m}*.log \
    | sed -E 's/obj=0x[0-9a-fA-F]+/obj=PTR/; s/frame=[0-9]+/frame=N/' \
    > /tmp/track-a2-destroy-modern-${m}.norm
done
```

- [ ] **Step 6.3 — Compare counts (hard gate)**

```bash
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  diff /tmp/track-a2-destroy-legacy-${m}.count /tmp/track-a2-destroy-modern-${m}.count \
    && echo "${m}: count match" \
    || echo "${m}: COUNT MISMATCH"
done
```

Expected: every mission "count match." Any "COUNT MISMATCH" is a hard failure — Track A2 fails the parity gate.

- [ ] **Step 6.4 — Compare identity (sharper gate)**

```bash
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  diff /tmp/track-a2-destroy-legacy-${m}.norm /tmp/track-a2-destroy-modern-${m}.norm \
    > /tmp/track-a2-destroy-${m}.identity-diff
  if [ -s /tmp/track-a2-destroy-${m}.identity-diff ]; then
    echo "${m}: IDENTITY DIFF — review"
    head -20 /tmp/track-a2-destroy-${m}.identity-diff
  else
    echo "${m}: identity match"
  fi
done
```

Expected: every mission "identity match." For A2 specifically, even count-and-identity matches that are non-zero are surprising — effect admission shouldn't touch GameObject lifecycle. If non-zero matches appear, document them in the envelope as "unexpected coupling" and investigate before flip.

- [ ] **Step 6.5 — Document in the envelope**

Append to `docs/superpowers/specs/2026-05-06-track-a2-acceptance-envelope.md`:

```markdown
## DESTROY parity (Task 6 capture, <date>)

| Mission | Legacy DESTROYs | Modern DESTROYs | Count? | Identity? |
|---------|-----------------|------------------|--------|-----------|
| mc2_01  | <N>             | <N>              | ✓      | ✓         |
| mc2_03  | <N>             | <N>              | ✓      | ✓         |
| mc2_10  | <N>             | <N>              | ✓      | ✓         |
| mc2_17  | <N>             | <N>              | ✓      | ✓         |
| mc2_24  | <N>             | <N>              | ✓      | ✓         |

Identity comparison: kind+reason+gate-state-snapshot tuple after pointer/frame
normalization (same scheme as A1 envelope).

DESTROY parity is expected trivially-zero for A2 (effect admission does
not feed GameObject lifecycle). Recording it anyway as defense-in-depth
against unforeseen cross-system coupling.
```

```bash
git add docs/superpowers/specs/2026-05-06-track-a2-acceptance-envelope.md
git commit -m "docs(track-a2): record DESTROY count parity (legacy vs modern)

Tier1 stock missions show 0 DESTROY count delta between legacy and
modern effect-admission predicates — expected (effect admission does
not feed GameObject lifecycle), recorded as defense-in-depth.

Plan: docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md
"
```

---

## Task 7 — Soak observation period (sequential-with-overlap, no code change)

**Soak ordering locked per Q15 (`docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md`):**
**sequential-with-overlap.** A1 must already be flipped default-on
*before* A2 enters soak. The soak then runs under A1-already-modern,
which IS the production-relevant joint configuration. This is the
canonical Q15 pattern across the whole rendering modernization arc.

**Why this and not parallel envelopes:** if A2-under-A1 fails, A2 or
the A1-A2 interaction is the culprit — clean attribution. If A1+A2
both passed alone but the joint config fails, we'd be re-running both
soaks individually anyway to attribute. Sequential-with-overlap is
cheaper on calendar time over the full arc.

**Prerequisite for entering this task:** A1 has shipped, soaked clean
for ≥3 days, and flipped default-on (A1's Task 9 complete). If A1 is
still in soak, A2 Task 7 does not start — wait for A1 flip.

Same cadence as A1 Task 8: ~3 days of clean modern-mode runs, with
effect-specific visual canaries. This task is a checklist, not code.

- [ ] **Step 7.1 — Run modern mode through one full mission (rain canary)**

`mc2_03` exercises raindrops (the most z-sensitive effect site, screen.z feeds ambient). Run with:

```bash
MC2_EFFECT_ADMISSION_PREDICATE=modern \
MC2_HEARTBEAT=1 \
  build64/RelWithDebInfo/mc2.exe -mission mc2_03
```

Play the mission. Watch for:
- Rain still falls (most are admitted; subtle count shift OK).
- No raindrop "lines" with one endpoint stuck at screen origin (would indicate bool-true with screen.x/y missing — should be impossible since `projectZ` writes screen first).
- No raindrop color flicker (`amb = ambientFactor * (1.0 - screen.z)`; screen.z stable across modes by construction).
- No crashes, freezes, GL errors.

- [ ] **Step 7.2 — Run modern mode through `mc2_24` (crater + cloud canary)**

`mc2_24` has heavy mech combat (crater spawning) and is the wolfman canary mission. Run with:

```bash
MC2_EFFECT_ADMISSION_PREDICATE=modern \
MC2_HEARTBEAT=1 \
  build64/RelWithDebInfo/mc2.exe -mission mc2_24
```

In-mission:
- Trigger mech destruction (craters spawn). Observe corner clipping behavior — modern should preserve the OR-of-4-corners admission that legacy uses (line 368: `if (onScreen1 || onScreen2 || onScreen3 || onScreen4)`).
- Zoom out to wolfman level. Check cloud shadows still cover the map. Compare against an equivalent legacy-mode run.
- Capture screenshots at the same camera positions.

- [ ] **Step 7.3 — Daily smoke for ≥3 days under joint A1+A2-modern**

Per Q15 sequential-with-overlap discipline, A1 has already flipped
default-on by this point. Run A2-modern soak with the joint config:

```bash
MC2_EFFECT_ADMISSION_PREDICATE=modern \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

(`MC2_OBJECT_ADMISSION_PREDICATE` is unset because A1 default-on means
its modern path is already active. Setting it to `modern` explicitly is
a no-op but harmless.)

If a regression appears under joint A1+A2-modern that did not appear
during A1-isolated soak, it's an A1-A2 interaction. Bisect by setting
`MC2_OBJECT_ADMISSION_PREDICATE=legacy` to revert A1 only — if the
regression disappears with A1=legacy + A2=modern but reappears with
both modern, the predicates interact and require diagnosis.

- [ ] **Step 7.4 — Decision point: flip default-on, or hold**

After 3+ days of clean soak:

- Tier1 5/5 PASS under joint A1-modern + A2-modern? → ✓
- DESTROY count delta = 0 across all joint-config soak runs? → ✓ (defense-in-depth — effects are not lifecycle-cascade producers, but parity check confirms no surprise)
- Acceptance envelope per-site disagreement counts within ±20% of authored baseline? → ✓
- No visual regressions in rain (mc2_03), craters (mc2_24), or cloud shadows under joint config? → ✓
- Wolfman canary shows at-least-equivalent visible-effect count under joint modern? → ✓

**All five green:** proceed to Task 8 default-on flip (A2 joins A1 as default).

**Any red:** hold. Open follow-up investigation. Update envelope or fix predicate, then restart Task 7 from Step 7.1.

---

## Task 8 — Default-on flip + memory + index

After Task 7 soak passes, flip the default and write the memory file.

**Files:**
- Modify: `mclib/object_admission_predicate.cpp` (default mode flips)
- Create: `~/.claude/projects/A--Games-mc2-opengl-src/memory/track_a2_effect_admission_predicate.md`
- Modify: `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

- [ ] **Step 8.1 — Flip default**

In `mclib/object_admission_predicate.cpp::effectAdmissionPredicate_init()`, change:

```cpp
const char* env = std::getenv("MC2_EFFECT_ADMISSION_PREDICATE");
if (env && std::strcmp(env, "modern") == 0) {
    s_effectMode = EffectAdmissionPredicateMode::Modern;
} else {
    s_effectMode = EffectAdmissionPredicateMode::Legacy;
}
```

To:

```cpp
const char* env = std::getenv("MC2_EFFECT_ADMISSION_PREDICATE");
if (env && std::strcmp(env, "legacy") == 0) {
    s_effectMode = EffectAdmissionPredicateMode::Legacy;
} else {
    s_effectMode = EffectAdmissionPredicateMode::Modern;  // default
}
```

- [ ] **Step 8.2 — Tier1 confirmation**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: 5/5 PASS, `[INSTR v1] effect_admission_mode=modern` (no env set, modern is default).

- [ ] **Step 8.3 — Verify legacy still reachable as opt-out**

```bash
MC2_EFFECT_ADMISSION_PREDICATE=legacy \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 10 --kill-existing
```

Expected: passes, `[INSTR v1] effect_admission_mode=legacy`.

- [ ] **Step 8.4 — Write the memory file**

Path: `C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\track_a2_effect_admission_predicate.md`

```markdown
---
name: track_a2_effect_admission_predicate
description: Track A2 shipped — effect admission (clouds/craters/weather) uses clip-space frustum predicate; same shared clipSpaceFrustumAdmit as A1; default-on after 3-day soak
type: project
---

Track A2 of the MC3 rendering modernization arc shipped on <date>. Wires the
7 effect-admission callsites (1× clouds, 4× craters, 2× weather raindrops)
to the same `clipSpaceFrustumAdmit` predicate Track A1 ships, behind a
sibling env flag `MC2_EFFECT_ADMISSION_PREDICATE=modern|legacy`. Screen
output (`screen.x/y/z/w` consumed by `cloudVertices[].px/py/pz/pw`,
`currCrater->screenPos[].{x,y,z,w}`, raindrop `gos_VERTEX[].{x,y,z,rhw}` +
the explicit `1.0-screen.z` ambient term in weather raindrop_top) is
preserved byte-identical via `LegacyProjectionResult` capture from
`projectZ`.

**Env flag (still present as opt-out):** `MC2_EFFECT_ADMISSION_PREDICATE=legacy`
restores the screen-rect predicate. Default = modern.

**Acceptance envelope:** documented at
`docs/superpowers/specs/2026-05-06-track-a2-acceptance-envelope.md` with
per-callsite disagreement counts and trivial DESTROY parity confirmation.

**Why this matters:** Track A2 was the fan-out validation that A1's
dual-output wrapper pattern scales to multiple wrappers without per-site
code changes when the screen oracle is preserved. Track A3 (terrain
admission, 6 sites) is conditional on data review post-A1+A2.

**Files touched:**
- `mclib/object_admission_predicate.{h,cpp}` — sibling enum + lazy probe
- `mclib/camera.h:557-570` — wrapper body change (LegacyProjectionResult capture)
- `GameOS/gameos/gameosmain.cpp` (or equivalent) — startup probe call

**Roadmap:** `docs/superpowers/mc3-rendering-modernization-roadmap.md` (Track A § A2).
```

Replace `<date>` with the actual ship date.

- [ ] **Step 8.5 — Index in MEMORY.md**

Add to the "Rendering / shaders" section of `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`:

```
- ⭐ [Track A2 shipped — effect admission clip-space frustum (<date>)](track_a2_effect_admission_predicate.md) — fan-out of A1 pattern across 7 cloud/crater/weather callsites; screen oracle preserved; default-on
```

The index entry must stay under ~200 chars per the MEMORY.md rules.

- [ ] **Step 8.6 — Commit the flip**

```bash
git add mclib/object_admission_predicate.cpp
git commit -m "feat(track-a2): flip effect admission predicate default to modern

After 3+ days of clean soak with MC2_EFFECT_ADMISSION_PREDICATE=modern
across tier1 stock missions: zero DESTROY delta, acceptance envelope
matched baseline, no visual regressions in rain/crater/cloud canaries.

Modern is now default. MC2_EFFECT_ADMISSION_PREDICATE=legacy retained as
opt-out for at least one more soak cycle before Track E retirement.

Plan: docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md
"
```

---

## Self-Review (run before declaring complete)

**Spec coverage:**
- Q1 (entry slice = effect admission) → Tasks 4, 6, 7 (all touch the 7 callsites' shared wrapper).
- Q2 (dual-output wrapper) → Task 4 specifically (LegacyProjectionResult capture, mirror of A1).
- Q3 (parity = dual-run + envelope + DESTROY parity + screen byte-identity) → Tasks 5 (envelope) + 6 (DESTROY) + Task 4 (screen byte-identity preserved by `projectZ` reuse).
- Killswitch → Task 2 + Task 8 (default flip).

**Per-site grep findings:** 7 sites confirmed at `mclib/clouds.cpp:212`, `mclib/crater.cpp:323/326/329/332`, `code/weather.cpp:489/497`. All consume `screen.{x,y,z,w}`, including explicit `screen.z` ambient at weather_raindrop_top:500. Zero callsite changes needed — wrapper preserves screen byte-identical.

**Inheritance verified:** `clipSpaceFrustumAdmit`, `LegacyProjectionResult`, 3-arg `projectZ`, `homogClipFull` trace candidate, selftest, and DESTROY-parity methodology all come from A1 unchanged.

**Placeholder scan:** none — every step has exact code or exact commands. The acceptance envelope template (Task 5.3) has placeholders for measured values, which is correct (the values come from running the captures).

**Type consistency:** `EffectAdmissionPredicateMode` (enum class, sibling of `ObjectAdmissionPredicateMode`), `effectAdmissionPredicateMode()` accessor, `effectAdmissionPredicate_init()` lifecycle entry — all match between Task 1 (header), Task 2 (impl), Task 3 (startup wire), Task 4 (wrapper consumer).

**Open question reminder:** Task 5.3's acceptance envelope authoring is operator-side — the plan describes what to capture and how to format it. The actual numbers come from running the captures.

---

## Execution Handoff

Plan complete. Two execution options:

**1. Subagent-Driven (recommended)** — fresh subagent per task, review between tasks, fast iteration. Eight tasks, several are trivial (Task 1, 3) so subagent overhead is well-amortized.

**2. Inline Execution** — execute tasks in this session using `executing-plans`, batch with checkpoints for review.

Which approach?
