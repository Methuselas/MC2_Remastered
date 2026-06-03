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

### 3.0 Naming (disambiguation — used throughout)

Two distinct "build" concepts must not be conflated in code, logs, or docs:

| Term | Meaning |
|---|---|
| **snapshot extraction** | `ExtractRenderSnapshot()` gathers static props from RenderWorld into `snap` (upstream, `gameosmain.cpp`) |
| **snapshot_packet_build** | `flush()` builds dispatch arrays (`s_snapV6Packets`/`s_snapV6Meta`) *from* `snap` — gated by `s_snapshotBuildEnabled` (`MC2_SNAPSHOT_STATIC_PROP_BUILD`) |

All new logs/docs say **`snapshot_packet_build`** for the second; never bare
"snapshot build".

### 3.1 New gate — a default-off legacy/debug kill-switch

`MC2_STATIC_PROP_LIVE_BUILDER` is a **debug/legacy kill-switch**, not a feature flag:

```
unset / =0  → snapshot is the SOLE draw-packet owner (live builder + compare retired)
=1          → restore the v3-flip DUAL path: live build + snapshot_packet_build + compare
```

```cpp
// STATIC-PROP-SNAPSHOT-FINISH (v8): snapshot is the sole draw-packet owner.
// DEBUG/LEGACY kill-switch — default OFF (retired). =1 restores the v3-flip
// dual-build+compare path for regression bisect / A-B.
static const bool s_keepLiveBuilder = []() -> bool {
    const char* keep = std::getenv("MC2_STATIC_PROP_LIVE_BUILDER");
    return keep && keep[0] == '1';
}();
```

**Interaction guard (load-bearing):** if `MC2_SNAPSHOT_STATIC_PROP_BUILD=0`
(snapshot_packet_build disabled) the live builder MUST remain — retirement
requires the snapshot packet path active. Resolve the collision at startup:

```cpp
// Retire only when the kill-switch is unset AND snapshot_packet_build is enabled
// (nothing else produces the dispatch arrays). Otherwise keep the live builder.
const bool retireLiveBuilder = !s_keepLiveBuilder && s_snapshotBuildEnabled;
```

**Arm log (once at first flush), with resolved reason:**

```
[STATIC_PROP_PACKET_DISPATCH v8] event=arm
  live_builder_retired=<0|1>
  snapshot_packet_build=<0|1>
  live_builder_forced=<0|1>          # 1 when kept due to MC2_STATIC_PROP_LIVE_BUILDER=1
  reason=snapshot_sole_owner | snapshot_packet_build_disabled_keep_live | live_builder_forced
```

### 3.2 Flush restructure

```
flush(snap):
  IF retireLiveBuilder:
      // Sole path: snapshot builds the arrays, dispatch directly. No live build, no compare.
      build s_snapV6Packets / s_snapV6Meta from snap        (existing snapshot builder, L5444+)
      IF snapshotInvalid(snap):                              // see definition below — NOT "arrays empty"
          ++s_spBuildFallback ; fall back to LIVE build for THIS frame only (safety net)
      ELSE dispatch s_snapV6* arrays                         // zero packets → dispatch zero, no fallback
  ELSE:
      // Legacy debug path (MC2_STATIC_PROP_LIVE_BUILDER=1): today's v3-flip dual build + compare + select.
```

The existing snapshot builder loop (L5444+) and the existing dispatch loop are
reused unchanged. The change is: **skip the live builder loop (L5253+) and the
compare** when `retireLiveBuilder`, and route dispatch to the snapshot arrays.

**`snapshotInvalid(snap)` — precise definition (load-bearing).** Invalid means a
structural/extraction failure, NOT an empty result:

```text
INVALID (→ safety-net fallback):
    snap == nullptr
    snap->ok != 1
    snap->arenaOverflow                       (extraction arena overflowed)
    snapshot static-prop extraction marked invalid
    packet count disagrees with snapshot metadata   (malformed: snap->staticPropPackets.count
                                                      inconsistent with the built array length)

VALID, dispatch as-is (NO fallback):
    zero static-prop packets this frame       → dispatch zero props
```

A frame with legitimately no static props (some missions/camera angles) MUST
dispatch zero, never fall back. Treating "arrays empty" as invalid is a defect.

**Per-frame safety net (survival only, NOT a steady state).** On `snapshotInvalid`,
that single frame falls back to a live build so a transient extraction failure
never drops static props. This is a runtime survival net, not accepted operation:

```text
fallback allowed:   isolated bad frame (runtime survives)
fallback in tier1:  spBuildFallback > 0  = VALIDATION FAILURE (do not merge)
fallback in soak:   spBuildFallback > 0  = do not merge / do not delete (Commit 2)
```

The merge gate (§5) requires `spBuildFallback == 0` across all validation. Any
fallback during validation means an extraction bug to fix before merge, not a
tolerated path.

### 3.3 Counters / ok-gate

**Add an explicit `spBuildRetired` field** to `RenderSnapshot` (resolves review
must-fix #1 / former open-Q3). Do NOT overload `spBuildAttempted==0` to mean
"retired" — that is ambiguous with "unvalidated".

```text
spBuildRetired = 1, spBuildAttempted = 0   → live compare INTENTIONALLY skipped (retired)
spBuildRetired = 0, spBuildAttempted = 0   → SUSPICIOUS / unvalidated (flag)
spBuildRetired = 0, spBuildAttempted = 1   → dual-build path ran compare (legacy/kill-switch on)
```

- `spBuildRetired`: set to 1 when `retireLiveBuilder` is in effect, else 0.
- `spBuildPacketMismatch` / `spBuildMetaMismatch` / `spBuildCountMismatch`: 0 by
  construction when retired (no compare). Meaningful only when `spBuildRetired==0`.
- `spBuildFallback`: retained and meaningful (per-frame safety-net trips); the
  merge gate requires it == 0 (§3.2, §5).
- The `render_snapshot.cpp` ok-gate (v4) must treat `spBuildRetired==1` as a clean
  state (compare deliberately absent), and must NOT flag `spBuildAttempted==0`
  alone as failure when `spBuildRetired==1`. When `spBuildRetired==0`, the existing
  gate semantics are unchanged.

`debug_state_dump.cpp` JSON + `MC2_RENDER_SNAPSHOT_LOG` line must emit
`spBuildRetired` so the soak can confirm the retired state per frame.

### 3.4 Supporting changes (non-behavioral)

- `RenderSnapshot` (`render_snapshot.h`): add `spBuildRetired` field (§3.3).
- `docs/tier1_env_vars.md`: register `MC2_STATIC_PROP_LIVE_BUILDER` as a
  **default-off legacy/debug kill-switch** (unset/0 = snapshot sole owner; 1 =
  dual-build) + add to `RendererFeatureRegistry.h` `kAuxEnvVars` (env_registry CI
  must stay green).
- `scripts/run_smoke.py`: add `MC2_STATIC_PROP_LIVE_BUILDER` to the propagation
  allowlist.
- `docs/active_campaigns.md`: DrawPacket v8 entry.

### 3.5 Mandatory pre-task — prove the live builder has no out-of-flush side effects

**BLOCKING. Must complete and be recorded before the dispatch change is written.**

The live builder loop (L5253+) does more than fill `v6Packets`/`v6Meta`: it resets
and increments per-frame diagnostic counters (`s_v6FrameDrawsIssued`,
`s_v6FrameZeroInstSkips`, `s_v6FrameSortedOob`, `s_v6FramePacketOob`,
`s_v6FrameTypeOob`, `s_v6FrameLockstepViolations`, `s_v6FrameGlErrors`,
`s_v6TotalFrameCount`) and the `s_spBuild*` reset. Prove, with file:line evidence:

1. `v6Packets` / `v6Meta` (L3 PassTransient static vectors) have **no reader**
   outside the flush dispatch/compare in the retired path.
2. The `s_v6Frame*` / `s_v6TotalFrameCount` counters' consumers (logs, ImGui,
   `batcher_get*`) either (a) are also driven by the snapshot dispatch path, or
   (b) tolerate being zero when retired. For any counter that would silently zero,
   **relocate its increment to the snapshot dispatch path** so diagnostics survive.
3. No live-builder output feeds the **shadow path** (`flushShadow`), **culling**,
   **material binding**, or **post-flush diagnostics**.

If any side effect exists, move it to the snapshot dispatch path **before** retiring
the loop. Record findings in the implementation plan's pre-task output.

### 3.6 Live-build / snapshot-build Tracy subzones (instrument before measuring)

Add scoped Tracy zones so the perf proof can distinguish "work disappeared" from
"work moved" (per 100ns-floor rule — per-loop, not per-element):

- `ZoneScopedN("StaticProp.LiveBuild")` around the live builder loop (L5253+).
- `ZoneScopedN("StaticProp.SnapshotBuild")` around the snapshot builder loop (L5444+).
- `ZoneScopedN("StaticProp.BuildCompare")` around the compare block (if separable).

These land in the same commit (observational; gateless Tracy zones are standard
here) so the before/after Tracy (kill-switch on vs off) attributes the delta.

---

## 4. Two-commit shape (retire, then delete)

**Commit 1 — retire (this spec, reversible):** add the gate; skip live build +
compare by default; dispatch from snapshot; keep the live-builder code in place
(reachable via `MC2_STATIC_PROP_LIVE_BUILDER=1`). Fully reversible.

**Commit 2 — delete (follow-up, after soak):** delete the live builder loop, the
compare code, and the `MC2_STATIC_PROP_LIVE_BUILDER` gate. Separate slice
(`STATIC-PROP-LIVE-BUILDER-DELETE`) so the retirement bakes behind the kill-switch
first. **A single tier1 is NOT sufficient to delete.** Required soak before delete:
- tier1 5/5;
- mc2_24 wolfman/combat soak;
- one terrain-object-dense mission;
- one session exercising the `MC2_STATIC_PROP_LIVE_BUILDER` kill-switch A/B;
- **`spBuildFallback == 0` across the entire soak window** (any fallback blocks the
  delete) and visual identity throughout.

This spec covers **Commit 1**. Commit 2 is scheduled, not executed here.

---

## 5. Validation / flip gate

Reuse the v3-flip doc's prerequisite pattern (`2026-05-27-static-prop-v3-flip.md`).
All BLOCKING before merge:

### P1 — Zero mismatch AND zero fallback (strict)
The snapshot path has *been* the dispatcher since the v3-flip with mismatch
counters zero. Re-confirm at current HEAD: tier1 5/5 with
`MC2_RENDER_SNAPSHOT_LOG=1`:
- all `spBuild*Mismatch == 0` and `ok=1` every frame, including mc2_10 frames
  1706–3326 (the historical `sp_fail` window);
- **`spBuildFallback == 0` across all of tier1.** Any fallback = an extraction bug
  to fix before merge, NOT a tolerated path (§3.2). `spBuildRetired == 1` every
  frame (confirms the retired path is actually active).

### P2 — Visual identity (same-camera A/B)
On a prop-heavy mission (mc2_24 + one terrain-object-dense mission), capture
same-camera frames retired (default) vs kept (`MC2_STATIC_PROP_LIVE_BUILDER=1`).
Pixel-identical static props expected (same indirect commands; only the *source*
of the identical arrays changed). Per substitutive-proof discipline.

### P3 — Substitutive perf proof (measure the right zones)
User-driven wolfman Tracy, mc2_24 combat. Report, before (kill-switch on) vs
after (retired, default):
- `GpuStaticProps.Flush` self-time
- `textureManagerRenderLists`
- `StaticProp.LiveBuild` + `StaticProp.BuildCompare` subzones (§3.6) — must go to
  ~0 when retired
- **`Extract.SP.Fill` — must NOT increase.** Confirm the retired cost
  *disappeared*, did not *move* into the gather. (Fill stays the follow-up slice's
  target; it must be unchanged here.)

This is the load-bearing perf bar
(`feedback_offload_must_be_substitutive_not_additive.md`).

### P4 — Snapshot-packet-build-disabled interaction
Run `MC2_SNAPSHOT_STATIC_PROP_BUILD=0`. Expected:
- live builder **remains active** (retirement disabled);
- no static props vanish; GL-clean;
- arm log: `live_builder_retired=0 reason=snapshot_packet_build_disabled_keep_live`.

### P5 — Gates green
`scripts/check-contracts.sh` 8/8 (esp. env_registry with the new var registered);
tier1 5/5 `+0 destroys` GL-clean; kill-switch `MC2_STATIC_PROP_LIVE_BUILDER=1`
restores byte-identical legacy behavior (same indirect commands).

---

## 6. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Snapshot structurally invalid on a frame → static props vanish | HIGH | Per-frame safety-net fallback to live build on `snapshotInvalid` only (§3.2, NOT on empty arrays); `spBuildFallback` counter; **merge gate requires fallback==0** so a real extraction bug blocks merge rather than silently degrading |
| Empty-arrays mistaken for invalid → needless fallback on legitimately prop-free frames | MED | §3.2 `snapshotInvalid` excludes zero-packets; zero props dispatch zero |
| Loss of per-frame compare hides a future divergence | MED | Compare retained behind `MC2_STATIC_PROP_LIVE_BUILDER=1` for CI/bisect; P1 zero-mismatch+zero-fallback gate; Commit 2 delete only after multi-mission soak (§4) |
| ok-gate misreads retired state as failure | MED | §3.3 explicit `spBuildRetired` field (not overloaded `spBuildAttempted==0`); arm-log + per-frame log emit it |
| Interaction with `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` | MED | §3.1 startup collision guard keeps live builder when snapshot_packet_build disabled; P4 validates |
| Hidden live-build side effect consumed elsewhere | MED→gated | **Mandatory pre-task §3.5** (BLOCKING): prove no out-of-flush reader of `v6Packets`/`v6Meta`; relocate any silently-zeroed `s_v6Frame*` counter increment to the snapshot dispatch path before retiring |

---

## 7. Resolved review decisions (outside review, 2026-06-02)

1. **Safety-net fallback vs hard-fail:** KEEP the per-frame fallback as a runtime
   survival net, but make it a strict merge gate — `spBuildFallback == 0` required
   across all validation/soak; any fallback during validation blocks merge (§3.2,
   §5 P1). Runtime survives; steady-state fallback is not accepted.
2. **Commit 2 timing:** NOT a single tier1. Requires tier1 + mc2_24 wolfman/combat
   soak + terrain-object-dense mission + kill-switch A/B session + zero fallback
   over the whole window (§4).
3. **Counter semantics:** RESOLVED — add explicit `spBuildRetired` field; do not
   overload `spBuildAttempted==0` (§3.3).
4. **`s_v6Frame*` diagnostic counters:** RESOLVED into the mandatory pre-task
   (§3.5) — prove no out-of-flush side effect; relocate any silently-zeroed counter
   increment to the snapshot dispatch path before retiring the loop.

---

## 8. References

- Arc: `docs/superpowers/specs/2026-05-26-drawpacket-v6-arch.md` (§7 shadow exclusion),
  `2026-05-26-drawpacket-v7-spec.md`, `2026-05-27-static-prop-v3-flip.md`
- Roadmap: `docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md` (items 3, 11, 13; P2 meta-fix)
- Perf: `docs/render-perf-snapshot.md`, `docs/trackv-whole-frame-cpu-optimization-audit.md`
- Discipline: `memory/feedback_offload_must_be_substitutive_not_additive.md`
- Follow-up: `PERF-EXTRACT-SNAPSHOT-FILL-DIRTYLIST-1` (the `fillStaticPropSlots` double-scan dirty-list)
