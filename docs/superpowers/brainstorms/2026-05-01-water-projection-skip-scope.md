# Water vertex projection skip — brainstorm scope (2026-05-01)

> **⚠️ SUPERSEDED 2026-05-02 — slice premise INVALIDATED by Stage 0 M3 audit.**
> The legacy water-projection block at `quad.cpp:803-1124` is NOT stranded upstream.
> The fast-path thin-record builder at `gos_terrain_water_stream.cpp::UploadAndBindThinRecords()`
> consumes `q.waterHandle` (per-frame inclusion gate) and `q.vertices[i]->wz` (per-triangle
> pz validity check) — gating the legacy block silently drops quads from the thin record.
> Per `gpu_direct_renderer_bringup_checklist.md` trap #7 ("CPU pre-cull is THE frustum gate"):
> the projection block IS the CPU pre-cull for water rendering. Recon's Section C audited
> only `drawWater()` consumers without grep'ing the fast-path code; five review passes
> trusted Section C as ground truth. See orchestrator "Recently shipped / Closed" row +
> Update log entry 2026-05-02. **Do not act on this brainstorm.** Tracy hygiene bundle
> queued as the consolation deliverable. Process lesson: data-flow audits are asymmetric —
> grep the candidate consumer for negative claims, not just the source for positive claims.

> **Status:** SUPERSEDED. (Was: READY-FOR-SPEC.)
>
> **Slice in one paragraph.** The renderWater architectural slice (shipped 2026-04-30) retired the water DRAW via a post-`renderLists()` fast path, but the upstream water vertex projection in `mclib/quad.cpp:773-1100` still runs every frame for every water-bearing quad even when `MC2_RENDER_WATER_FASTPATH=1`. Its outputs (`vertices[i]->wx/wy/wz/ww`, the `addTriangleBulk` reservations at `quad.cpp:1087-1088`, the eight `wAlpha` writes scattered across the four per-corner sub-blocks) are consumed only by `TerrainQuad::drawWater()`, which the fast path bypasses entirely. This slice gates the upstream block behind the same armed predicate the fast path uses. Pattern is "fast-path with stranded upstream." Recon (`explorations/2026-04-30-water-projection-skip-recon.md`) confirms `wAlpha` has zero readers, modern-`terrainTextures2` precondition makes the legacy single-bitmap consumers of `clipInfo` unreachable, and the only architecturally live residual is `leastZ/mostZ/leastW/mostW` accumulator contributions feeding `Camera::inverseProjectZ()` for mouse-pick depth. Scope is closer to M2d-overlay than renderWater Stage 1+2+3.

---

## Q1. Gate placement — where exactly?

**Decision: (c) — extract `WaterStream::IsArmed()` helper, evaluate once-per-frame at top of `Terrain::geometry()`, gate the per-quad water block via a thin `BeginLegacyWaterCluster()` wrapper at `quad.cpp:773`.**

### Evidence

The current armed predicate is duplicated at two sites in `mclib/terrain.cpp`:

- **Site 1 — `terrain.cpp:1048-1051`** (legacy `renderWater()` early-return), confirmed:
  ```cpp
  static const bool s_fastPath =
      (getenv("MC2_RENDER_WATER_FASTPATH") != nullptr);
  if (s_fastPath
      && WaterStream::IsReady()
      && WaterStream::GetRecipeCount() > 0
      && Terrain::terrainTextures2 != nullptr)
  ```

- **Site 2 — `terrain.cpp:1118-1123`** (`renderWaterFastPath()` preflight), confirmed:
  ```cpp
  static const bool s_fastPath =
      (getenv("MC2_RENDER_WATER_FASTPATH") != nullptr);
  if (!s_fastPath) return;
  if (!WaterStream::IsReady()) return;
  if (WaterStream::GetRecipeCount() == 0) return;
  if (!Terrain::terrainTextures2) return;
  ```

Adding a third inlined copy at `quad.cpp:773` would create three sites that must stay in sync — already a maintenance hazard at two.

### Shipped precedent (load-bearing for this decision)

The indirect-terrain SOLID-only PR1 (commit `f221570`) chose option (c) for the same problem. `gos_terrain_indirect::IsFrameSolidArmed()` is declared at `GameOS/gameos/gos_terrain_indirect.h:161` and used from three sites:
- `mclib/quad.cpp:106` — wrapped in a thin `BeginLegacySolidCluster()` namespace-scoped helper:
  ```cpp
  static inline bool BeginLegacySolidCluster() {
      return !gos_terrain_indirect::IsFrameSolidArmed();
  }
  ```
- `mclib/terrain.cpp:1704` — comment confirms per-frame stability: *"IsFrameSolidArmed() is stable for all setupTextures() calls."*
- `mclib/txmmgr.cpp:1331` — flush-side gate.

This slice mirrors that exact shape: `WaterStream::IsArmed()` returning the four-clause AND, called once at the top of `Terrain::geometry()` to populate a frame-static, then a thin `BeginLegacyWaterCluster()` wrapper at `quad.cpp:773`.

### Alternatives rejected

- **(a) Inline the compound predicate at `quad.cpp:773`.** Recomputes the predicate per-quad (cheap but wasteful), and creates the third synchronization site. Rejected.
- **(b) Hoist a frame-static into `Terrain::geometry()` without an `IsArmed()` helper.** Same flag-cache shape but inlines the four-clause AND in `Terrain::geometry`. Rejected because the indirect-terrain precedent demonstrates that the helper-in-namespace form is the codebase convention; inventing a sibling form would fragment the pattern.

### Per-frame stability

`s_fastPath` and `WaterStream::IsReady()` are mission-static post-init; `GetRecipeCount()` is mission-static post-`primeMissionTerrainCache`; `Terrain::terrainTextures2` flips only at mission load/teardown. All four are stable for the duration of `Terrain::geometry()`. A frame-static cache is correct.

---

## Q2. Mouse-pick residual risk handling

**Decision: (A) — ship a dev-only side-channel diagnostic (`MC2_WATER_PROJECT_SKIP_PARITY=1`) that records `leastZ/mostZ/leastW/mostW` deltas (skip vs no-skip) for one development cycle, then drop before promoting the gate to default-on if deltas are consistently zero on tier1.**

### Evidence

Recon Section D.5 establishes the accumulators are written from the water block at `mclib/quad.cpp:838-858, 906-928, 976-998, 1046-1068` and consumed at `mclib/terrain.cpp:1732` via `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)`. `Camera::inverseProjectZ()` then uses these for mouse-pick depth on stock missions. This is gameplay-correctness state, not visual-only.

The recon's Section D.5 conjectures terrain-vertex extrema are always the global min/max because water elevation is below or equal to terrain elevation, so water-only quads rarely contribute the global extreme — but this conjecture is unmeasured.

### Why not (C) — visual canary alone?

Three reasons:

1. **`MC2_RENDER_WATER_PARITY_CHECK` covers the GPU output stream**, not the upstream CPU accumulators. It cannot signal a regression in `inverseProjectZ()` consumers.

2. **Mouse-pick is not visual**. A 1-pixel cursor depth drift on water tiles is invisible in screenshot diff and tier1 smoke (`tests/smoke/README.md` shows tier1 is passive — no mouse-pick exercised).

3. **Stage 3 of renderWater caught three real bugs that would have shipped silently** (recon Section §Stage 3 closeout in `memory/renderwater_fastpath_stage2.md`). The pattern is: a measurement gate on the actual residual state catches what visual gates miss. The accumulator delta is this slice's analogous residual.

### Why not full-time always-on diagnostic?

Once tier1 has shown the delta is zero for one development cycle, the diagnostic is dead weight. Drop it before default-on promotion. The conjecture promotes to a verified fact at that point; the slice carries no ongoing instrumentation cost.

### Scope alignment

`memory/feedback_offload_scope_stock_only.md`: validation is stock missions only. The diagnostic runs on tier1 (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`); `mc2_17` is water-heavy and is the canary that surfaced the recipe-coverage and blank-vertex bugs in renderWater Stage 3.

### Alternatives rejected

- **(C) Trust the conjecture, ship without measurement.** Plausible but creates a class-of-bug-Stage-3-catches-but-this-slice-doesn't. Cheap insurance to add the diagnostic for one cycle.

---

## Q3. Parity gate shape

**Decision: (i) — byte-compare per-quad CPU output: `wx/wy/wz/ww` writes per corner + `addTriangleBulk` reservation arguments + `clipInfo` writes + `calcThisFrame |= 2` writes. Field-level mismatch printer + 600-frame summary, mirroring `MC2_RENDER_WATER_PARITY_CHECK` shape.**

### Evidence

- Recon Section C.2 establishes that with the fast path on, the bucket-queue contents at `mclib/txmmgr.cpp:1466-1530` are post-renderLists no-ops (`totalVertices = 0` → `gos_RenderIndexedArray` not called). Comparing the bucket-queue state (option ii) compares an artifact whose downstream consumer is dead. Adds infra without adding signal.

- Per-quad CPU output (option i) compares the actual writes the gate skips: 4× `wx/wy/wz/ww` + 4× `clipInfo` + 4× `calcThisFrame |= 2` + 2× `addTriangleBulk(handle, flags, 2)`. These are the load-bearing outputs whose absence the slice is betting on. If a future change makes any of them load-bearing again, this gate catches the regression.

- Visual-canary-only (option iii) is too weak. Renderwater Stage 3 explicitly proved this: three real bugs surfaced through byte-compare that tier1 visual canary did not flag (recon Section §Stage 3 closeout in `memory/renderwater_fastpath_stage2.md`).

### Mechanics

The byte-compare is naturally implementable as a "run the projection block AND record what it would have produced; then compare to the no-op state we should have left." The skipped path produces no writes; the parity branch runs the legacy block, captures into a side buffer, asserts side-buffer-equals-zero. With `MC2_WATER_PROJECT_SKIP_PARITY=1`, the slice is effectively in a 3-state matrix:

| `MC2_RENDER_WATER_FASTPATH` | `MC2_WATER_PROJECT_SKIP` | `MC2_WATER_PROJECT_SKIP_PARITY` | Behavior |
|---|---|---|---|
| 0 | (any) | (any) | Legacy water draw + projection (baseline) |
| 1 | 0 | (any) | Fast-path draw + projection still runs (current 2026-04-30 state) |
| 1 | 1 | 0 | Fast-path draw + projection skipped (target) |
| 1 | 1 | 1 | Fast-path draw + projection runs into side buffer + assert no-op |

This matches the `MC2_RENDER_WATER_FASTPATH=1+PARITY_CHECK=1` validation tier1 5/5 PASS triple from the renderWater closeout (`memory/renderwater_fastpath_stage2.md`).

### Output convention

`[WATER_PROJ_SKIP_PARITY v1] event=mismatch frame=N quad=Q corner=C field=<name> legacy=0xHEX skipped=0xHEX` (throttled, e.g. 16/frame). 600-frame summary `event=summary frames=N quads_checked=Q total_mismatches=K`. Naming mirrors `[WATER_PARITY v1]` from renderWater Stage 3.

### Alternatives rejected

- **(ii) Bucket-queue compare.** Compares state whose downstream is dead. Wasted infra.
- **(iii) Visual canary only.** Too weak per Stage 3 evidence.

---

## Q4. Slice-shape estimate vs M2d-overlay

**Decision: closer to M2d-overlay than renderWater Stage 1+2+3. ~1-2 commits, 2-3 files touched, 2 env flags, tier1 5/5 single-pass with parity tier appended.**

### Evidence

M2d-overlay shipped in commit `258e584` (`feat(m2d): land overlay fast path`):
- 2 files changed, 76 insertions(+), 8 deletions(-)
- Single LOC site (`mclib/quad.cpp`) + design doc bump
- Single tier1 5/5 PASS gate
- No new headers, no new SSBO, no new shader, no new bridge

This slice's expected diff:
- `mclib/quad.cpp` — wrap `quad.cpp:773-1100` in `if (BeginLegacyWaterCluster()) { … }` + add an `EndLegacyWaterCluster()` counter bump after the closing brace. ~5 LOC for the wrapper, ~optional ~30-50 LOC for the parity side-buffer capture.
- `GameOS/gameos/gos_terrain_water_stream.h` — declare `WaterStream::IsArmed()`. ~2 LOC.
- `GameOS/gameos/gos_terrain_water_stream.cpp` — implement `IsArmed()` (the four-clause AND already exists at `terrain.cpp:1048-1051` and `terrain.cpp:1118-1123`; this consolidates them). ~10 LOC. Optionally update both call sites in `terrain.cpp` to use `WaterStream::IsArmed()`. ~4 LOC delta.
- `GameOS/gameos/gameosmain.cpp` — extend `[INSTR v1]` banner with `water_skip` field. ~1 LOC.

Total: ~50 LOC main + ~30-50 LOC optional parity. **Single commit if `IsArmed()` extraction is folded in; two commits if it lands as a refactor precursor.** Recon recommended either; this brainstorm picks single commit (lower churn, M2d shape).

### Env flags

Two new (recon Section H.3 picked separate flag for bisection clarity):
- `MC2_WATER_PROJECT_SKIP=1` — arms the gate. Default off until promoted.
- `MC2_WATER_PROJECT_SKIP_PARITY=1` — arms the byte-compare diagnostic for one development cycle. Default off; expected to be removed before default-on promotion.

Single-flag option `MC2_RENDER_WATER_FASTPATH=1` arms-both was considered and rejected: cannot bisect "fast-path draw without skip" vs "fast-path draw with skip" if one breaks. Two flags trade one env-knob for one bisection axis; the bisection axis is more valuable mid-development.

### Validation tier

Tier1 5/5 PASS quintuple:
1. unset (legacy baseline)
2. `MC2_RENDER_WATER_FASTPATH=1` (post-2026-04-30 baseline)
3. `MC2_RENDER_WATER_FASTPATH=1 MC2_WATER_PROJECT_SKIP=1` (target)
4. `MC2_RENDER_WATER_FASTPATH=1 MC2_WATER_PROJECT_SKIP=1 MC2_WATER_PROJECT_SKIP_PARITY=1` (parity diagnostic)
5. `MC2_RENDER_WATER_FASTPATH=1 MC2_WATER_PROJECT_SKIP=1 MC2_RENDER_WATER_PARITY_CHECK=1` (compose with renderWater parity to confirm no cross-effect)

+0 destroys delta on every state. Mouse-pick visual canary on tier1 mc2_17 (water-heavy) added to the manual-spot-check list.

---

## Q5. Out-of-scope flagging

The recon explicitly enumerated several "while we're in here" temptations. Confirming each as out of scope:

### Out of scope (do NOT bundle)

- **`mclib/terrain.cpp:1611-1636` accumulator update.** Recon Section B confirmed this is `vertexProjectLoop`'s **terrain** (not water) accumulator. Owned by the closed vertexProjectLoop D1 slice (`memory/vertexproject_loop_asymptotic.md`). **Different population, different consumer, different data field.** Touching it pollutes scope.

- **Indirect-terrain detail/overlay/mine consolidation.** Listed in orchestrator status board as a separate queued slice with its own brainstorm precondition. Has its own multi-bucket draw question (`gl_DrawIDARB` vs separate indirect calls vs single-command-with-per-quad-texture) that this slice does not need to settle. Keep separate.

- **Bucket reservation removal at `quad.cpp:1087-1088`.** Section H.5 of recon flags as a follow-up cleanup. Recon Section C.2 confirmed the reservations are harmless when the fast path is on (`renderLists` computes `totalVertices=0` and skips the draw call). **Recommendation: leave as-is in this slice.** Removing them is mechanical and can land as a post-soak cleanup commit alongside the legacy water draw retirement (when `MC2_RENDER_WATER_FASTPATH` itself promotes to default-on or gets removed). Mixing it into this slice means the parity diagnostic must distinguish "block ran but produced no bucket reservation" from "block was skipped" — an asymmetry that adds parity-check complexity for zero functional gain.

- **`wAlpha` env-map-water resurrection.** Recon Section H.6 — `wAlpha` is documented as "environment Map Sky onto water" (declaration at `mclib/vertex.h:95`) but has zero readers in the active codebase. Slice's removal of the writes is safe today; if a future env-map-water shader pulls it, that shader's design must source from the SSBO recipe instead. **Document via commit message, do not re-wire here.**

- **`WaterStream::IsArmed()` consolidation of `terrain.cpp:1048-1051` and `terrain.cpp:1118-1123` call sites.** Optional bundle. Recommended IN-scope as a 4-LOC delta because it eliminates the third synchronization site this slice would otherwise create. Does not expand the slice meaningfully.

### In scope (stays bounded)

- The `quad.cpp:773-1100` block gate.
- The `WaterStream::IsArmed()` helper + namespace-scoped `BeginLegacyWaterCluster()` wrapper.
- The two new env flags + parity diagnostic.
- The `[INSTR v1]` banner extension.
- The two-call-site update in `terrain.cpp` to use the new helper (optional but recommended for consistency).

---

## Code-grounding verification appendix

Every cited symbol grep'd at write-time. Format: `<symbol>` — `<file:line>` — `matches claim / divergent`.

| Symbol | File:line | Status |
|---|---|---|
| `s_fastPath && WaterStream::IsReady() && WaterStream::GetRecipeCount() > 0 && Terrain::terrainTextures2 != nullptr` | `mclib/terrain.cpp:1046-1051` | matches claim (predicate verified by Read) |
| Second armed-check site (`renderWaterFastPath()` four sequential `if (!…) return;`) | `mclib/terrain.cpp:1118-1123` | matches claim (verified by Read) |
| Water projection block start `// NEW(tm) water texture code here.` | `mclib/quad.cpp:773-774` | matches claim (verified by Read) |
| Outer water-bit OR gate `if ((vertices[0]->pVertex->water & 1) || …)` | `mclib/quad.cpp:775-778` | matches claim (verified by Read) |
| `addTriangleBulk(waterHandle, MC2_ISTERRAIN \| MC2_DRAWALPHA \| MC2_ISWATER, 2)` | `mclib/quad.cpp:1087` | matches claim (verified by Read) |
| `addTriangleBulk(waterDetailHandle, MC2_ISTERRAIN \| MC2_DRAWALPHA \| MC2_ISWATERDETAIL, 2)` | `mclib/quad.cpp:1088` | matches claim (verified by Read) |
| `wAlpha` declaration `float wAlpha; //Used to environment Map Sky onto water.` | `mclib/vertex.h:95` | matches claim (cited from recon, recon verified by Grep) |
| `IsFrameSolidArmed` declaration | `GameOS/gameos/gos_terrain_indirect.h:161` | matches claim (verified by Grep) |
| `BeginLegacySolidCluster` thin wrapper `return !gos_terrain_indirect::IsFrameSolidArmed();` | `mclib/quad.cpp:106` | matches claim (verified by Read) |
| `IsFrameSolidArmed()` per-frame stability comment | `mclib/terrain.cpp:1704` | matches claim (verified by Grep — comment text *"IsFrameSolidArmed() is stable for all setupTextures() calls"*) |
| `IsFrameSolidArmed` flush-side use | `mclib/txmmgr.cpp:1331` | matches claim (verified by Grep) |
| M2d-overlay commit shape (76 ins / 2 files, single quad.cpp site) | `git show --stat 258e584` | matches claim (verified by Bash) |
| `setObjBlockActive`/`setObjVertexActive` are NOT in the water block (recon Section G "Cull gates" entry) | implicit from recon's cited file:line ranges in Section A | matches claim (recon's Read of `quad.cpp:773-1100` verified) |
| `Camera::inverseProjectZ()` consumer at `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` | `mclib/terrain.cpp:1732` (cited by recon) | matches claim (recon Section D.5 verified by Grep on prior pass; not re-grep'd here) |
| Indirect-terrain SOLID-only PR1 commit `f221570` | orchestrator Status Board | matches claim (cited in orchestrator doc, not separately verified — orchestrator is authoritative for its own log) |
| `MC2_RENDER_WATER_PARITY_CHECK` declared in `gos_terrain_water_stream.{h,cpp}` | files-with-matches Grep returned both | matches claim |
| M2d-overlay diff stat (`mclib/quad.cpp \| 76 ++++++++++++++++++++--`) | `git show --stat 258e584` | matches claim |

**No divergent entries.** Two entries marked "verified by recon's prior Grep, not re-grep'd here": `mclib/vertex.h:95` (`wAlpha` declaration) and `mclib/terrain.cpp:1732` (`setInverseProject` call). Recon's verification appendix at its Section §Appendix has these as grep-confirmed; this brainstorm relies on the recon's verification, which is the documented compositional pattern (recon establishes ground truth; brainstorm references recon claims plus adds its own grep at decision sites).

### Symbols proposed by this brainstorm (no grep possible — they don't exist yet)

- `WaterStream::IsArmed()` — proposed helper. To be declared in `gos_terrain_water_stream.h`, defined in `.cpp`. Mirrors `gos_terrain_indirect::IsFrameSolidArmed()`.
- `BeginLegacyWaterCluster()` / `EndLegacyWaterCluster()` — proposed namespace-scoped helpers in `quad.cpp`, mirroring `BeginLegacySolidCluster()` at `quad.cpp:105-110`.
- `MC2_WATER_PROJECT_SKIP` env flag — proposed.
- `MC2_WATER_PROJECT_SKIP_PARITY` env flag — proposed (one-cycle development tool, drop before default-on promote).
- `[WATER_PROJ_SKIP_PARITY v1]` log schema — proposed; mirrors `[WATER_PARITY v1]`.

These are intentions, not claims about existing code, per the worktree CLAUDE.md "Documentation Discipline" carve-out.

---

## Closing — ready-for-spec

All five questions answered with grep-cited evidence. No "needs-more-recon-on-X" residuals. Spec session has:

- A clean gate placement (Q1 → option c, with shipped indirect-terrain precedent).
- A measurement strategy for the residual mouse-pick risk (Q2 → one-cycle parity diagnostic).
- A parity-check shape that mirrors renderWater Stage 3 (Q3 → option i, byte-compare per-quad CPU output).
- A bounded slice scope (Q4 → ~80-150 LOC, 1-2 commits, 4 files).
- Explicit out-of-scope list (Q5 → vertexProjectLoop accumulator, indirect-terrain consolidation, bucket reservation removal, env-map-water resurrection).

The spec writer should produce a plan that:
1. Adds `WaterStream::IsArmed()` helper consolidating `terrain.cpp:1048-1051` and `terrain.cpp:1118-1123`.
2. Adds `BeginLegacyWaterCluster()` thin wrapper at `quad.cpp` namespace scope.
3. Wraps `quad.cpp:773-1100` in `if (BeginLegacyWaterCluster()) { … } else { /* sentinel waterHandle/waterDetailHandle = 0xffffffff per existing else branches at 1090-1099 */ }` — the sentinel-on-skip is required because `drawWater()` is called in the legacy path and reads these fields.
4. Implements parity diagnostic with side-buffer capture + assert-zero.
5. Validates against the 5-state tier1 matrix in Q4.

Spec writer should also confirm at plan-write time that `waterHandle`/`waterDetailHandle` sentinel-on-skip is sound (i.e., the fast path's downstream is unaffected by these fields being `0xffffffff` instead of the real handles — the fast path looks them up from the recipe SSBO, not from the per-quad fields). Recon Section C did not explicitly grep the fast path's handle-source; this is the one residual claim worth re-grep-verifying at plan-write time.
