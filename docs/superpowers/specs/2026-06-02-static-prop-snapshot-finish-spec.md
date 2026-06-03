# Static-Prop Snapshot Finish — Live-Builder Retirement (DrawPacket v8)

**Date:** 2026-06-02
**Slice kind:** behavior-changing draw-path consolidation (default-flip, kill-switched)
**Status:** SPEC — pending outside review
**Prerequisite HEAD:** `ccf208d8` (ExtractRenderSnapshot instrumentation) or later on `claude/nifty-mendeleev`
**Author context:** TRACKV-WHOLE-FRAME-CPU-OPTIMIZATION-AUDIT-1 → ExtractRenderSnapshot instrumentation → this slice

---

## 1. Purpose

Finish the static-prop extraction bridge by making the **snapshot the sole owner**
of static-prop draw-packet construction. Today both a *live* builder and a
*snapshot* builder run every `flush()`, plus a field-by-field compare between
them — the snapshot already drives the GPU draw (since the v3-flip, 2026-05-27),
but the live path is retained as compare-authority + fallback. This slice retires
the live builder and the per-flush compare, behind a kill-switch.

This realizes Engine Convergence Roadmap **item 3** (extraction phase — static-prop
axis) and the **P2 meta-fix principle** verbatim: *"Every time two systems know the
same fact, create one owner and make the other ask for it."* The draw-fact owner
becomes the snapshot; the live builder is deleted.

### Non-goals (explicit)

- **No `Extract.SP.Fill` optimization.** The `RenderWorld::fillStaticPropSlots`
  double full-scan (1.13ms, the dominant remaining cost) is a **separate follow-up
  slice** (`PERF-EXTRACT-SNAPSHOT-FILL-DIRTYLIST-1`). This slice removes the
  *flush-side* duplication only.
- **No shadow-pass change.** `flushShadow()` stays on the legacy per-type loop per
  v6-arch §7 (tracked as v6.1/v8-shadow). It does not consume DrawPackets.
- **No mech/terrain/VFX extraction.** Static-prop axis only.
- **No sort keys, no GPU-cull count integration, no struct/SSBO/shader changes.**

---

## 2. Background — current state (verified in code)

`GameOS/gameos/gos_static_prop_batcher.cpp::flush()`:

| Gate | Default | Kill-switch | Meaning |
|---|---|---|---|
| `s_v6Enabled` (L108) | ON | `MC2_STATIC_PROP_LEGACY_DISPATCH=1` | DrawPacket dispatch vs legacy multidraw |
| `s_snapshotBuildEnabled` (L127) | ON | `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` | snapshot builds v6 arrays + compare + dispatch-if-clean |

Per-flush sequence today (the v3-flip architecture, 2026-05-27):

```
flush(snap = getLastRenderSnapshot()):
  runV6 builder loop (L5253+)            → v6Packets / v6Meta   (LIVE: walks s_sortedPacketOrder + s_typeRanges)
  IF s_snapshotBuildEnabled (L5444+):
    snapshot builder loop                → s_snapV6Packets / s_snapV6Meta   (from snap rows)
    field-by-field compare               → spBuildPacketMismatch / spBuildMetaMismatch / spBuildCountMismatch
    dispatch: clean → snapshot arrays ; mismatch → live arrays (++spBuildFallback, ok=0 next frame)
  ELSE:
    dispatch live arrays
```

Upstream, once per frame between `DoGameLogic()` and `draw_screen()`
(`gameosmain.cpp:1201`): `ExtractRenderSnapshot()` gathers static props from
RenderWorld into `snap.staticProps` / `snap.staticPropPackets`.

### The duplication (Tracy, mc2_24, user-driven wolfman, 2026-06-02)

Same static-prop population walked three times per frame:

| Zone | Cost | Role |
|---|---|---|
| `GameLogic.Units.TerrainObjects` | 1.13ms | game-logic per-object update (separate; not in scope) |
| `Extract.SP.Fill` (`RenderWorld::fillStaticPropSlots`) | 1.13ms | snapshot gather (follow-up slice) |
| flush live-build + compare (inside `textureManagerRenderLists` 1.73ms) | portion | **THIS SLICE retires** |

`ExtractRenderSnapshot` mean = 1.84ms (median 1.79, P99 2.95, P99.9 10.07);
sub-zones `Extract.SP.Fill` 65% / self 25% / `WriteLoop` 10% / `Packets` 0.4%.

---

## 3. What changes

**Single file (behavior): `GameOS/gameos/gos_static_prop_batcher.cpp`.**

### 3.1 New gate

Add a kill-switch that, when *unset* (default), retires the live builder + compare:

```cpp
// STATIC-PROP-SNAPSHOT-FINISH (v8): snapshot is the sole draw-packet owner.
// Default ON (live builder + per-flush compare retired). Kill-switch restores
// the v3-flip dual-build+compare+fallback path for debugging / regression bisect.
// Requires s_snapshotBuildEnabled (snapshot build is the sole path when this is ON).
static const bool s_liveBuilderRetired = []() -> bool {
    const char* keep = std::getenv("MC2_STATIC_PROP_LIVE_BUILDER");  // =1 keeps live build+compare
    return !(keep && keep[0] == '1');
}();
```

**Interaction guard (load-bearing):** if `MC2_SNAPSHOT_STATIC_PROP_BUILD=0`
(snapshot build disabled) the live builder MUST remain — retirement requires the
snapshot path active. Resolve the collision at startup:

```cpp
// If snapshot build is force-disabled, the live builder cannot be retired
// (nothing else produces the dispatch arrays). Keep live build; log once.
const bool retireLiveBuilder = s_liveBuilderRetired && s_snapshotBuildEnabled;
```

Log the resolved state once at first flush:
`[STATIC_PROP_PACKET_DISPATCH v1] event=arm live_builder_retired=%d snapshot_build=%d`.

### 3.2 Flush restructure

```
flush(snap):
  IF retireLiveBuilder:
      // Sole path: snapshot builds the arrays, dispatch directly. No live build, no compare.
      build s_snapV6Packets / s_snapV6Meta from snap        (existing snapshot builder, L5444+)
      IF snapshot invalid (snap==nullptr || snap->ok!=1 || arrays empty):
          ++s_spBuildFallback ; fall back to LIVE build for THIS frame only (safety net)
      ELSE dispatch s_snapV6* arrays
  ELSE:
      // Legacy debug path (MC2_STATIC_PROP_LIVE_BUILDER=1): today's v3-flip dual build + compare + select.
```

The existing snapshot builder loop (L5444+) and the existing dispatch loop are
reused unchanged. The change is: **skip the live builder loop (L5253+) and the
compare** when `retireLiveBuilder`, and route dispatch to the snapshot arrays.

**Per-frame safety net (keep):** even with the live builder retired, if the
snapshot is structurally invalid for a given frame (`snap==nullptr`, `ok!=1`,
zero-length, arena overflow), that single frame falls back to a live build so a
transient extraction failure never drops static props. This preserves the
fail-safe the v3-flip relied on, at the cost of an occasional live build on a bad
frame (counted by `spBuildFallback`). Sustained `spBuildFallback>0` is a
regression signal, not normal operation.

### 3.3 Counters / ok-gate

- `spBuildAttempted` / `spBuildPacketMismatch` / `spBuildMetaMismatch` /
  `spBuildCountMismatch`: when `retireLiveBuilder`, there is no compare, so these
  are 0 by construction. Keep the fields; set `spBuildAttempted=0` to denote
  "compare not run (live builder retired)". Document in `render_snapshot.h` ok-gate
  comment that 0/0 here means *retired*, not *unvalidated* — distinguished by the
  `live_builder_retired=1` arm log.
- `spBuildFallback`: retained and meaningful (per-frame safety-net trips).
- The `render_snapshot.cpp` ok-gate (v4) must not flag `spBuildAttempted==0` as a
  failure when the live builder is retired.

### 3.4 Supporting changes (non-behavioral)

- `docs/tier1_env_vars.md`: register `MC2_STATIC_PROP_LIVE_BUILDER` (default OFF =
  retired) + add to `RendererFeatureRegistry.h` `kAuxEnvVars` (env_registry CI must
  stay green).
- `scripts/run_smoke.py`: add `MC2_STATIC_PROP_LIVE_BUILDER` to the propagation
  allowlist.
- `docs/active_campaigns.md`: DrawPacket v8 entry.

---

## 4. Two-commit shape (retire, then delete)

**Commit 1 — retire (this spec, reversible):** add the gate; skip live build +
compare by default; dispatch from snapshot; keep the live-builder code in place
(reachable via `MC2_STATIC_PROP_LIVE_BUILDER=1`). Fully reversible.

**Commit 2 — delete (follow-up, after soak):** once a soak confirms zero
`spBuildFallback` and visual identity over the soak window, delete the live builder
loop, the compare code, and the `MC2_STATIC_PROP_LIVE_BUILDER` gate. Separate slice
(`STATIC-PROP-LIVE-BUILDER-DELETE`) so the retirement can bake behind the kill-switch
first.

This spec covers **Commit 1**. Commit 2 is scheduled, not executed here.

---

## 5. Validation / flip gate

Reuse the v3-flip doc's prerequisite pattern (`2026-05-27-static-prop-v3-flip.md`).
All BLOCKING before merge:

### P1 — Snapshot already the active dispatcher, zero mismatch (evidence)
The snapshot path has *been* the dispatcher since the v3-flip with mismatch
counters zero. Re-confirm at current HEAD: tier1 5/5 with
`MC2_RENDER_SNAPSHOT_LOG=1`, all `spBuild*Mismatch==0` and `ok=1` every frame,
including mc2_10 frames 1706–3326 (the historical `sp_fail` window).

### P2 — Visual identity (same-camera A/B)
On a prop-heavy mission (mc2_24 + one terrain-object-dense mission), capture
same-camera frames with the live builder retired (default) vs kept
(`MC2_STATIC_PROP_LIVE_BUILDER=1`). Pixel-identical static props expected (same
indirect commands; only the *source* of the identical arrays changed). Per the
project substitutive-proof discipline.

### P3 — Substitutive perf proof
User-driven wolfman Tracy, mc2_24 combat: `textureManagerRenderLists` /
`GpuStaticProps.Flush` self-time must DROP by the retired live-build+compare cost
(not merely move). Record before/after. This is the load-bearing perf bar
(`feedback_offload_must_be_substitutive_not_additive.md`).

### P4 — Gates green
`scripts/check-contracts.sh` 8/8 (esp. env_registry with the new var registered);
tier1 5/5 `+0 destroys` GL-clean; kill-switch `MC2_STATIC_PROP_LIVE_BUILDER=1`
restores byte-identical legacy behavior.

---

## 6. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Snapshot produces wrong/empty arrays on some frame → static props vanish | HIGH | Per-frame safety-net fallback to live build on invalid snapshot (§3.2); `spBuildFallback` counter; kill-switch |
| Loss of per-frame compare hides a future divergence | MED | Compare retained behind `MC2_STATIC_PROP_LIVE_BUILDER=1` for CI/bisect; P1 soak before flip; Commit 2 delete only after clean soak |
| ok-gate misreads `spBuildAttempted==0` as failure | MED | §3.3 ok-gate update; arm-log distinguishes retired from unvalidated |
| Interaction with `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` | MED | §3.1 startup collision guard forces live build when snapshot build disabled |
| Hidden live-build side effect consumed elsewhere | MED | Recon task in plan: confirm `v6Packets`/`v6Meta` (L3 PassTransient, static-vector) have no reader outside flush dispatch; confirm `s_v6Frame*` counters' only consumers tolerate retirement |

---

## 7. Open questions for outside review

1. **Safety-net fallback vs hard-fail:** keep the per-frame live-build fallback on
   invalid snapshot (§3.2), or hard-fail (skip static props that frame) to surface
   extraction bugs louder? Spec assumes keep-fallback.
2. **Commit 2 timing:** how long a soak before deleting the live builder — one
   tier1 cycle, or a multi-session wolfman soak?
3. **Counter semantics:** is `spBuildAttempted=0`-means-retired acceptable, or add
   an explicit `spBuildRetired=1` field to avoid overloading?
4. **`s_v6Frame*` diagnostic counters:** several per-frame v6 counters
   (`s_v6FrameDrawsIssued`, `s_v6FrameSortedOob`, …) are reset/incremented in the
   live builder loop. Confirm they are still populated by the snapshot dispatch
   path, or relocate their increments, so diagnostics don't silently zero.

---

## 8. References

- Arc: `docs/superpowers/specs/2026-05-26-drawpacket-v6-arch.md` (§7 shadow exclusion),
  `2026-05-26-drawpacket-v7-spec.md`, `2026-05-27-static-prop-v3-flip.md`
- Roadmap: `docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md` (items 3, 11, 13; P2 meta-fix)
- Perf: `docs/render-perf-snapshot.md`, `docs/trackv-whole-frame-cpu-optimization-audit.md`
- Discipline: `memory/feedback_offload_must_be_substitutive_not_additive.md`
- Follow-up: `PERF-EXTRACT-SNAPSHOT-FILL-DIRTYLIST-1` (the `fillStaticPropSlots` double-scan dirty-list)
