# M3 + M4 + M5 — Spec Q Resolutions

**Date:** 2026-05-24
**Resolves:** open questions in
- [M3 terrain spec](2026-05-23-renderworld-slice-m3-terrain-spec.md)
- [M4 VFX spec](2026-05-23-renderworld-slice-m4-vfx-spec.md)
- [M5 overlay spec](2026-05-23-renderworld-slice-m5-overlay-spec.md)

User decisions captured 2026-05-24. Plan-writers + implementers MUST
read this sidecar alongside the specs. Resolutions override spec
"leans" where they differ.

---

## M3 — Terrain (reservation/deferral only)

| Q | Resolution |
|---|---|
| M3-Q1 — identity unit | **NONE for v1.** No writer ships. |
| M3-Q2 — picking semantic | **NONE for v1.** CPU terrain pick (`worldToTile`) remains canonical. |
| M3-Q3 — editor use case | **NONE current.** If editor UI lands later with a need for per-quad GPU identity, M3.1 ships then. |
| M3-Q4 — water/decal/mine sub-kind split | **Single Terrain kind.** Future M3.1 uses `subKind = Base / Water / Decal / Mine` payload, NOT separate `RenderObjectKind` values. |
| M3-Q5 — retire `Terrain::IsGameSelectTerrainPosition` | **NO.** CPU path is correct, used by multiple call sites; retiring is a much bigger gameplay/input rewrite out of scope. |
| M3-bonus — handle-base value | **`kTerrainHandleBase = 0x40000`** (per recon). Update migration guide handle-base table accordingly. |

**M3 ship scope:**
- Enum value `RenderObjectKind::Terrain = 2`
- Constant `kTerrainHandleBase = 0x40000`
- Defensive tripwire branch in `lookupAtPixel` (warn if `kind=Terrain` ever returned)
- Migration guide handle-base table update (0x40000)
- CLAUDE.md M3 SHIPPED entry

**Forward-compat note encoded in spec/migration guide:** if M3.1 ever
ships per-quad terrain identity (editor-driven), use the `subKind =
Base/Water/Decal/Mine` payload pattern. Do NOT proliferate `RenderObjectKind`
values for terrain variants.

---

## M4 — VFX (prohibition + scaffold)

| Q | Resolution |
|---|---|
| M4-Q1 — prohibit attachment-2 writes | **YES.** Load-bearing decision. VFX is click-through; pick the object underneath. |
| M4-Q2 — scaffold now vs wait for gate flip | **Ship scaffold NOW.** Lock the prohibition before any VFX writer could drift in. |
| M4-Q3 — per-emitter identity | **NONE.** No current use case. |
| M4-Q4 — per-source-game-object lookup | **Game-logic owns it.** Source mech is known at weapon/fire-event time; GPU should not rediscover provenance via a particle pixel. |
| M4-Q5 — gosFX dev-override blocker | **Acknowledge.** Document that `MC2_DISABLE_GOSFX=0` is NOT a valid substrate proof path until MLR projection fix lands. |
| M4-Q6 — encode corrected handle-base chart | **YES.** Encode the partitioning in migration guide §12 inline. |

**M4 ship scope:**
- Enum value `RenderObjectKind::Vfx = 3`
- Constant `kVfxHandleBase = 0x80000` (reserved, unused)
- New `scripts/check-vfx-no-objectid.sh` firewall grep gate
  (forbids `layout(location=2) out` in VFX shaders)
- Migration guide prohibition note + corrected handle-base chart
- CLAUDE.md M4 SHIPPED entry + gosFX dev-override caveat

**NO:** adapter, registerEffect, per-emitter handles, objectIdRaw fields,
shader writes, runtime gates, env vars.

---

## M5 — Overlay (defer indefinitely)

| Q | Resolution |
|---|---|
| M5-Q1 — what does M5 mean? | **(a) Defer indefinitely.** No identity consumer exists in-tree. |
| M5-Q2 — update enum comment with deferral note | **YES.** Future archaeology needs the pointer. |
| M5-Q3 — Tracy proof owner if (b) shipped | **N/A** (Q1 = defer). |

**M5 ship scope (TINY — folds into M3 commit since both touch the same enum block):**
- Enum block comment update:
  ```cpp
  // Future: Terrain=2 (reserved in M3), Vfx=3 (reserved in M4),
  //         Overlay reserved/deferred (see M5 spec for clarification rationale)
  ```
- CLAUDE.md M5 deferral note (1 line under Active campaigns, NOT a SHIPPED entry — nothing implemented)

**Rescopes for future work** (do NOT ship as "M5 Overlay"):
- `HoverKindIndicator` (consumer of M2.6 / M3)
- `RenderWorldDebugOverlay` (debug viz)
- `M5-perf: overlay/decal GPU port` (perf migration, separate from RenderWorld identity arc)

---

## Execution plan

Two implementer dispatches:

1. **M3 + M5 atomic commit** (single enum block edit covers both):
   - M3 substantive: enum value + constant + tripwire + migration guide table
   - M5 fold-in: enum block comment update
   - Both shipped in one commit (acknowledged as M3 + M5 in commit message)
   - CLAUDE.md gets M3 SHIPPED entry + M5 deferred note

2. **M4 standalone commit**:
   - Enum value + constant + firewall script + allowlist + migration guide prohibition + handle-base chart
   - CLAUDE.md gets M4 SHIPPED entry + gosFX dev-override caveat note

Each shippable as a single implementer dispatch (~one-shot like M6 was).
No separate plan/spec review needed — slices are tight and resolutions
are already pinned.

---

## Order of operations

1. Land this resolutions sidecar (docs commit, alongside any spec status updates)
2. M3+M5 atomic commit
3. M4 commit
4. Done. Arc post-M6 + M3 + M4 + M5 reaches steady state.
