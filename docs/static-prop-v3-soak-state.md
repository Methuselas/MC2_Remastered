# Static-Prop Extraction v3 — Soak State (pre-authority-flip)

**Date:** 2026-05-27
**Purpose:** Consolidate v2.1→v3 proof state before any default-ON flip. No code changes here.

---

## Arc Summary

| Slice | HEAD | What shipped |
|-------|------|--------------|
| v2.1 | `4245db18` | `ExtractedStaticPropPacket`, per-packet draw-slot snapshot, ok hard gate |
| v2.2 | `76f63b3f` | dispatch-fact compare (sortedSlot/globalPacketIdx/pipelineId/materialIdx/texArrayLayer) |
| v2.3 | `88379448` | snap-cull opt-in (`MC2_SNAP_CULL=1`), `spSnapCullSlotMismatch` in ok gate, all-zero warmup guard |
| v3 impl | `88d9668f` | snapshot-owned slot identity dispatch — `s_snapV6Packets`/`s_snapV6Meta`; compare; ref-swap dispatch |
| v3 soak (default) | `8ef2e8d2` | tier1 default, gate OFF → `spBuild*`=0, ok=1 5/5 PASS |
| v3 soak (opt-in) | `bb5356db` | tier1 `MC2_SNAPSHOT_STATIC_PROP_BUILD=1` → ok=1, fallback=0 5/5 PASS |
| v3 soak (collision) | `066b5b9d` | tier1 both `MC2_SNAPSHOT_STATIC_PROP_BUILD=1 MC2_SNAP_CULL=1` → collision guard fires, v3 falls back, ok=1 5/5 PASS |
| zombie-slot fix | `7bdbd1fd` | `invalidateStaticRegistration()` now calls `retireRecord`; mc2_10 sp_fail=1 eliminated |

---

## Behavior by Configuration

### Default (gate OFF)

`MC2_SNAPSHOT_STATIC_PROP_BUILD` unset (or `=0`).

- `s_snapshotBuildEnabled = false`
- All five `spBuild*` fields zero every flush
- ok gate: unaffected (zero satisfies all conditions)
- Live arrays dispatched unchanged
- **Proven:** `8ef2e8d2` tier1 5/5 PASS, all `spBuild*`=0

### Gate ON (`MC2_SNAPSHOT_STATIC_PROP_BUILD=1`)

- Activation guard runs each flush (Stage 0–4; see design doc)
- Snapshot arrays `s_snapV6Packets` / `s_snapV6Meta` built from snapshot rows
- Field-by-field compare runs after both arrays built
- Clean compare → snapshot arrays dispatched; `spBuildFallback=0`
- Mismatch → live arrays dispatched; `spBuildFallback=1`; ok=0 next frame; recovers frame after
- **Proven:** `bb5356db` tier1 5/5 PASS: ok=1, fallback=0, mismatch counters=0

### Snap-Cull Collision (`MC2_SNAP_CULL=1 MC2_SNAPSHOT_STATIC_PROP_BUILD=1`)

Both env vars set simultaneously.

- Stage 1 of activation guard detects collision
- Emits one log line: `[RENDER_SNAPSHOT v3] disabled — MC2_SNAP_CULL collision`
- Log latched (does not repeat per-frame)
- `spBuildAttempted=1`, `spBuildFallback` increments each frame
- Live arrays dispatched — no undefined behavior
- ok=1 because `spBuildFallback` is **informational only** (not in ok gate)
- `spBuildCountMismatch`, `spBuildPacketMismatch`, `spBuildMetaMismatch` remain 0 → ok unaffected
- **Proven:** `066b5b9d` tier1 5/5 PASS

---

## Authority Boundary

v3 makes the snapshot own draw-slot identity only. Current-frame execution facts
stay live.

**Snapshot owns (v3):**
- `sortedSlot` — draw-order index
- `globalPacketIdx` — packet index into `s_packets[]`
- `typeId` — owning type
- `pipelineId` / `group` — alpha split

**Live remains authoritative:**
- `instanceCount` — from `s_typeRanges`, built fresh each flush
- `baseInstance` — from `prepareBaseInstanceTable()`, per-frame ring-buffer
- Geometry (`firstIndex`, `indexCount`, `baseVertex`) — via `s_packets[row.globalPacketIdx]` (static after `finalizeGeometry()`; indexing via snapshot row is equivalent)

Fields not compared (tautological — identical source in both paths): `instanceCount`, `baseInstance`.

---

## mc2_10 Zombie-Slot Fix (Prerequisite — COMPLETE)

**Commit:** `7bdbd1fd`

`invalidateStaticRegistration()` tombstoned the registry recipe but did not call
`retireRecord()` on the matching `s_objectRecords` slot. The slot remained
`alive=true` with a tombstoned recipe, causing `staticPropValidationFail` (`sp_fail=1`)
from frame ~1706 onward in mc2_10. `sp_fail=1` sets `ok=0` for the rest of the
mission, masking the ok gate for the entire v3 soak.

Fix: `retireRecord()` call added in `invalidateStaticRegistration()`. Committed by
other worktree session.

**Re-validation pending:** Run mc2_10 alone with `MC2_RENDER_SNAPSHOT_LOG=1` and
confirm ok=1 throughout. Command:

```powershell
$env:MC2_RENDER_SNAPSHOT_LOG="1"
py -3 scripts/run_smoke.py --missions mc2_10 --duration 30 --keep-logs
```

Expect: no `sp_fail=1` in log; ok=1 throughout mission (frame 1706–3326 range clean).

---

## Authority-Flip Acceptance Gates

Conditions that must ALL be true before flipping `MC2_SNAPSHOT_STATIC_PROP_BUILD`
to default-ON:

| Gate | Status | Evidence |
|------|--------|----------|
| Tier1 5/5 gate-ON PASS | **PROVEN** | `bb5356db` — ok=1, fallback=0, mismatch counters=0 |
| Tier1 5/5 default PASS (no regression) | **PROVEN** | `8ef2e8d2` — ok=1, `spBuild*`=0 |
| Collision guard clean | **PROVEN** | `066b5b9d` — ok=1, no crash |
| `spBuildFallback=0` under gate-ON | **PROVEN** | `bb5356db` — confirmed 0 across all 5 missions |
| mc2_10 ok=1 post zombie-slot fix | **PENDING** | Requires re-run with `MC2_RENDER_SNAPSHOT_LOG=1` after `7bdbd1fd` |
| Visual identity (gate-ON identical to gate-OFF) | **PENDING** | No visual diff run yet; terrain + props look identical to casual inspection but not frame-compared |
| Extended soak (60s per mission or full tier1 at 60s) | **PENDING** | All gate-ON soak runs were 30s; longer run would exercise more mission-end teardown cycles |

**Blocking gates for flip:** mc2_10 re-validation + visual identity.
Extended soak is advisory (run before announcing default-ON in release notes).

---

## What Changes at Default-ON Flip

When flip ships:
1. Remove `if (!s_snapshotBuildEnabled) return false;` early-return (or set default to `true`)
2. Rename env var guard semantics: `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` becomes the kill-switch
3. Update `docs/tier1_env_vars.md` — mark default-ON; document `=0` revert
4. Smoke gate: add kill-switch (`=0`) run to tier1 to confirm live path still clean
5. `spBuildFallback` counter: promote to ok gate when default-ON (currently informational; a sustained fallback under default-on is a signal, not noise)

Not changed at flip: dispatch loop, compare logic, auth boundary, shadow pass.

---

## Related Files

- Design doc: `docs/superpowers/specs/2026-05-26-extraction-v3-design.md`
- Gate vars: `docs/tier1_env_vars.md` (`MC2_SNAPSHOT_STATIC_PROP_BUILD`, `MC2_SNAP_CULL`)
- Known issues: `docs/known_issues.md` (mc2_10 zombie-slot tombstoned as FIXED `7bdbd1fd`)
- Next step handoff: `memory/HANDOFF_2026_05_26_extraction_v3_next_slice.md`
