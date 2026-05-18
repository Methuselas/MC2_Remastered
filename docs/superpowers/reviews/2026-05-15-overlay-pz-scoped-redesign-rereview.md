# Adversarial Re-Review — M2d overlay-pz-gate scoped redesign (VPL Step 8b precursor)

**Verdict: BLOCK**

- CRITICAL: 1 (Finding 1 — the scoped redesign's §exact-edit code sample reads `vertices[c]->pz` directly; it does NOT achieve `cv->pz` independence and therefore does NOT unblock Step 8b. The precursor's entire purpose is defeated.)
- MAJOR: 0
- MINOR: 1 (Finding 5 — perf-bound is provable for the *corrected* design but must be stated as a pre-land gate, not assumed; carried forward from v1 Finding 5.)

**#1 coherence-defect resolution (one sentence):** DEFECT CONFIRMED — the redesign as written reads the VPL-written `vertices[c]->pz` directly (`quad.cpp:2107`, merely relocated), leaving `cv->pz` with a live reader so Step 8b stays blocked; **(i) corrected code is specified in Finding 1 §Corrected edit** — it is the synthesis of the v1 cv->pz-INDEPENDENT mechanism (on-site `eye->projectForTerrainAdmission` over the `(vx,vy,elevation)` triple + the mandatory `clipInfo==0` sentinel guard from v1 Finding 1) placed UNDER the v2 lazy compound-predicate scope.

**Perf-bound verdict:** sparse-proven **Y for the corrected design** (4 `projectForTerrainAdmission` x overlay-quads, with the `clipInfo==0` short-circuit further trimming off-cone corners; NOT x all-terrain-quads). The v1 146->42 regression was caused by *unconditional* re-projection in the per-terrain-quad `pzc` loop; the v2 lazy compound-predicate scope is what fixes that, and it is preserved in the corrected edit. The corrected design's cost is bounded; see Finding 4.

Worktree `.claude/worktrees/gpu-driven-rendering/`, branch `claude/gpu-driven-rendering`, HEAD `d7ff1c1` (the REVERT of `29ae435`; clean ~146fps baseline). Every citation grep-verified at this HEAD.

---

## Finding 1 — Redesign reads `cv->pz` directly; Step 8b stays blocked (CRITICAL; the #1 stress target, resolved definitively)

### 1a. What the redesign's §exact-edit code sample actually reads (grep-confirmed)

The redesign's §exact-edit prose says: "recompute pzc lazily, guarded by the compound predicate ... the `clipInfo==0` sentinel guard (Finding 1, still-correct) and the bit-identical pzc math are preserved verbatim." Its CODE SAMPLE computes:

```cpp
float pz_adj = vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
```

This is **byte-identical to the current pre-precursor baseline at HEAD** (`mclib/quad.cpp:2105-2109`, grep-verified at `d7ff1c1`):

```cpp
2105		    bool pzc[4];
2106		    for (int c = 0; c < 4; c++) {
2107		        float pz_adj = vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
2108		        pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
2109		    }
```

It reads `vertices[c]->pz` **directly** (the VPL-written field). It does NOT call `eye->projectForTerrainAdmission` and it does NOT contain any `clipInfo == 0` guard. The redesign's prose claim that "the clipInfo==0 sentinel guard ... [is] preserved verbatim" is **factually false against its own code sample**: there is no `clipInfo` token in the sample at all. The two halves of the redesign (prose vs. code) are contradictory, and the code is what an executor would write.

For contrast, the v1 bit-correct mechanism (the prior review proved this bit-exact, `2026-05-15-overlay-pz-precursor-adversarial-review.md:60-75`) was **on-site re-projection + the mandatory sentinel guard**:

```cpp
if (vertices[c]->clipInfo == 0) { pzc[c] = false; continue; }
Stuff::Vector3D ov3D(vertices[c]->vx, vertices[c]->vy, vertices[c]->pVertex->elevation);
Stuff::Vector4D osp;
eye->projectForTerrainAdmission(ov3D, osp);
float pz_adj = osp.z + TERRAIN_DEPTH_FUDGE;
pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
```

The redesign relocated the cost (good — fixes the 146->42 regression) but **regressed the mechanism back to the cv->pz read** (fatal — defeats the precursor).

### 1b. Step 8b's documented intent: it DELETES the VPL `cv->pz` write (grep-confirmed)

`docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md:461` (grep-verified):

> **Step 8b — retire VPL's per-vertex writes (`px/py/pz/pw`).** Fields survive on the `cv` struct because non-VPL consumers ... still reference them; VPL simply no longer writes them. Downstream consumers see stale data from previous frames ...

Same plan, `:484-485` (the precursor dependency, grep-verified):

> 1. **Step 1 has landed.** Step 1 kills the `quad.cpp` thin-emit / indirect-pack `pz` consumers; **without it the VPL `pz` writes still have live readers.**

The two VPL `cv->pz` writes Step 8b retires, grep-verified at HEAD:
- Slim VPL: `mclib/terrain.cpp:1601` `cv->pz = pzL;` (sentinel `pzL = -0.5f` at `:1577`).
- Legacy twin VPL: `mclib/terrain.cpp:1738` `currentVertex->pz = screenPos.z;` (sentinel `currentVertex->pz = -0.5f;` at `:1749`).

After Step 8b deletes both, `vertices[c]->pz` is **stale from a previous frame**. The engine camera is oblique 30deg + 360deg + cinematic (`memory/camera_model_oblique_cinematic.md`) and moves essentially every frame, so a relocated `vertices[c]->pz` read produces a different (stale) admission decision every frame the camera moves — the M2d overlay would emit decal triangles against last frame's projection. **Any design that reads `vertices[c]->pz` at the M2d site does NOT unblock Step 8b** and additionally is a correctness bug the moment 8b lands (stale-pz decal flicker, 9964d5a-adjacent decal-set perturbation).

### 1c. The only fast-path-live `cv->pz` reader is exactly this block (grep-confirmed)

`grep '->pz\b' mclib/quad.cpp` at HEAD: every other `->pz` hit is either the `:371` debug-trace path (the plan itself calls this out at `:461`, "`quad.cpp` reads `pz` at ... debug-trace path") or in the legacy `!fastPathEligible` path (`:2341`, `:2700`, `:3707+` — all after `:2320 // === end M2 branch — legacy path continues below ===`, unreachable when `fastPathEligible`). **`quad.cpp:2107` is the sole fast-path-live VPL-`pz` consumer.** Repointing it correctly is precisely and only what unblocks Step 8b. The redesign's scoping analysis (M2d overlay is the only live-when-armed consumer) is correct; its *mechanism* is wrong.

### Verdict: (i) — DEFECT. Corrected edit specified.

The redesign must be corrected to the v1 cv->pz-INDEPENDENT mechanism placed UNDER the v2 lazy compound-predicate scope. This is the synthesis the redesign was supposed to be and is not.

#### §Corrected edit (the synthesis of v1-mechanism + v2-scoping)

Replace the unconditional `pzc` block at `quad.cpp:2105-2109` with the lazily-scoped, re-projecting, sentinel-guarded form. Insert at the current `:2103` site (immediately inside `if (fastPathEligible)`), keep the `if (!pzTri1 && !pzTri2) return;` cull immediately after the `pzTri` derivation, exactly where it is now (`:2122`):

```cpp
if (fastPathEligible)
{
    // pz-validity gate. v2-scoping: only the M2d overlay (live-when-armed)
    // and the not-armed thin-emit consume pzc/pzTri; gate the (re-)compute
    // behind the union of their producer predicates so the cost is bounded
    // to the sparse overlay-quad set (NOT every terrain quad — the v1
    // 146->42 regression was unconditional re-projection here).
    // v1-mechanism: re-project on-site from the SAME (vx,vy,elevation)
    // triple VPL used, and reproduce VPL's pz=-0.5 off-screen sentinel via
    // the clipInfo==0 short-circuit (v1 Finding 1, proven bit-exact). This
    // makes the gate INDEPENDENT of vertices[c]->pz so Step 8b can delete
    // the VPL cv->pz write (terrain.cpp:1601 / :1738).
    bool pzc[4] = { false, false, false, false };
    const bool pzNeeded =
        (useOverlayTexture && overlayHandle != 0xffffffff)
        || (terrainHandle != 0 && !gos_terrain_indirect::IsFrameSolidArmed());
    if (pzNeeded)
    {
        for (int c = 0; c < 4; c++) {
            // VPL writes pz=-0.5 (sentinel) iff clipInfo==0 (onScreen==false)
            // for the perspective renderer (terrain.cpp:1577/1601/1603,
            // :1749/1759). Reproduce that co-decision; do NOT re-project the
            // sentinel corners (re-projection of an off-cone corner whose true
            // projectZ z is still in [0,1) would flip pzc false->true — the
            // divergence v1 Finding 1 closes).
            if (vertices[c]->clipInfo == 0) { pzc[c] = false; continue; }
            Stuff::Vector3D ov3D(vertices[c]->vx, vertices[c]->vy,
                                 vertices[c]->pVertex->elevation);
            Stuff::Vector4D osp;
            eye->projectForTerrainAdmission(ov3D, osp);
            float pz_adj = osp.z + TERRAIN_DEPTH_FUDGE;
            pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
        }
    }

    bool pzTri1, pzTri2;
    if (uvMode == BOTTOMLEFT) {
        pzTri1 = pzc[0] && pzc[1] && pzc[3];
        pzTri2 = pzc[1] && pzc[2] && pzc[3];
    } else {
        pzTri1 = pzc[0] && pzc[1] && pzc[2];
        pzTri2 = pzc[0] && pzc[2] && pzc[3];
    }

    if (!pzTri1 && !pzTri2) return;  // both culled — skip entirely
    // ... thin-emit block (:2124-2223) and M2d block (:2260-2316) UNCHANGED ...
```

Notes binding the executor:
- `pzc` is **zero-initialized** (`= { false, ... }`). When `pzNeeded==false`, all four are `false`, so `pzTri1==pzTri2==false` and `:2122` returns early. This is correct and is *exactly* the behavior the v2 scoping intends: if neither the overlay nor the not-armed thin-emit will consume pzTri, the quad has no fast-path decal/thin work and the early return is the right outcome. (Proof it cannot drop live work: Finding 3.)
- `eye` is `extern CameraPtr eye` (`camera.h:1124`), already used in this TU's legacy path (`quad.cpp:1026/1034/...`). In scope. (v1 Finding 4.)
- `projectForTerrainAdmission` is pure w.r.t. frame-stable camera state (`camera.h:526-539` -> `projectZ`; v1 Finding 2, re-confirmed pure at HEAD `camera.h:530`). `setInverseProject` does not feed `projectZ`; `worldToClip` is frame-stable. Call-order vs. the VPL loop is irrelevant.
- Do NOT add `OVERLAY_ELEV_OFFSET` to the projection input — VPL projected the raw `(vx, vy, pVertex->elevation)` triple (`terrain.cpp:1567`, twin `:1731`). The `+ OVERLAY_ELEV_OFFSET` at `quad.cpp:2277` is a render-time z-bias on the emitted vertex `wz`, not part of the admission projection. (v1 Finding 4.)
- After this lands, `quad.cpp:2107` no longer reads `vertices[c]->pz`; the only remaining fast-path `->pz` is the `:371` debug-trace path the plan already accounts for. **Step 8b's `quad.cpp` thin-emit/pz consumer is retired -> Step 8b unblocked.**

---

## Finding 2 — Decal-set identity: corrected math is byte-identical to v1 bit-correct behavior (CONFIRMED SAFE, with the corrected edit)

This holds **only for the §Corrected edit**, not the redesign as written.

- `clipInfo==0` branch: VPL set `pz=-0.5` -> old `pz_adj = -0.498`, `(-0.498>=0 && <1)==false`. Corrected code forces `pzc=false`. **Match.**
- `clipInfo!=0` branch (VPL `onScreen==true`): VPL stored `pz = screenPos.z` from `eye->projectForTerrainAdmission(Vector3D(cv->vx,cv->vy,cv->pVertex->elevation), screenPos)` (`terrain.cpp:1567-1571`, twin `:1731-1738`). Corrected code re-runs the **identical pure function** on the **identical `(vx,vy,elevation)` triple** -> identical `osp.z` -> identical `pz_adj` -> identical `pzc`. **Match.**
- `TERRAIN_DEPTH_FUDGE` = `0.002f` (`quad.cpp:1954`, grep-verified at HEAD); boundary `>= 0.0f && < 1.0f` (`quad.cpp:2108`). Corrected `osp.z + TERRAIN_DEPTH_FUDGE` with the same `>=0 && <1` is bit-identical. No off-by-epsilon. (v1 Finding 3.)
- Overlay gate expression bit-for-bit: the M2d producer gate `useOverlayTexture && overlayHandle != 0xffffffff` (`quad.cpp:2260`) is **textually unchanged** and the corrected edit does NOT touch `pzTri1/pzTri2`, the `:2122` cull, or the M2d producer (`:2260-2316`). The `pzNeeded` predicate's overlay disjunct `(useOverlayTexture && overlayHandle != 0xffffffff)` is the **same two operands in the same order** as `:2260` — so any quad that reaches the M2d producer had `pzNeeded==true` and got a fully-computed `pzc`. No 9964d5a-class divergence in which-tris-emit.

**The redesign-as-written FAILS this finding**: relocating the `cv->pz` read does not reproduce the sentinel co-decision once Step 8b removes the VPL write; `vertices[c]->pz` becomes stale, so `pzc` (hence which decal tris emit) diverges every frame the camera moves. This is the 9964d5a-class hazard the prior review's Finding 1 was created to prevent — the redesign reintroduced it by dropping the guard+reprojection.

---

## Finding 3 — Thin-emit not-armed coverage: `pzNeeded` covers every config where `:2209-2211` runs (CONFIRMED SAFE)

Thin-emit pzTri use, grep-verified:
- `quad.cpp:2209-2211`: `tr.flags = (uvMode==BOTTOMLEFT?1u:0u) | (pzTri1?2u:0u) | (pzTri2?4u:0u);`
- Gating scope: `if (terrainHandle != 0 && !thinEmitArmed)` at `quad.cpp:2154`, where `const bool thinEmitArmed = gos_terrain_indirect::IsFrameSolidArmed();` (`quad.cpp:2142`).

`IsFrameSolidArmed()` = `s_frameSolidArmed && !s_processArmingDisabled` (`gos_terrain_indirect.cpp:1913-1915`). The `pzNeeded` second disjunct is `(terrainHandle != 0 && !gos_terrain_indirect::IsFrameSolidArmed())` — **the exact same two-operand predicate** as the thin-emit gate at `:2154` (`terrainHandle != 0 && !thinEmitArmed`, `thinEmitArmed` being literally `IsFrameSolidArmed()`). Therefore in every config where `:2209-2211` executes, `pzNeeded` is true and `pzc` is fully computed before the `pzTri` derivation. No not-armed config gets stale/zeroed pzTri.

Exhaustive consumer enumeration inside `if (fastPathEligible)` (`grep 'fastPathEligible\|pzTri1\|pzTri2\|\bpzc\b' mclib/quad.cpp`, range `:2101`-`:2318`): only three pzc/pzTri consumers — (1) the `:2122` early-return cull, (2) thin-emit `:2210-2211` [gated `:2154`], (3) M2d overlay `:2284-2307` [gated `:2260`]. All `pzTri` hits at `:2371+`/`:2632+`/`:2987+` are in the legacy `!fastPathEligible` path (after `:2320`), which derives its own `gVertex[].z` and is unreachable when `fastPathEligible`. The `pzNeeded` predicate is the exact union of (2)'s and (3)'s producer gates; (1) is satisfied-by-construction because when `pzNeeded==false` both pzTri are false and the early return is the correct no-fast-work outcome (the quad emits neither thin nor overlay nor reaches any other live consumer).

---

## Finding 4 — Perf bound: corrected design is provably sparse (CONFIRMED; non-negotiable gate met)

The v1 146->42 (~71%) regression root cause: re-projection ran **unconditionally** in the per-terrain-quad `pzc` loop (the `for c in 0..4` at the old `:2105`, executed for EVERY admitted terrain quad — tens of thousands/frame). That is `4 x projectForTerrainAdmission x ALL-terrain-quads`.

The §Corrected edit gates the entire re-projection loop behind `if (pzNeeded)`. Cost proof:
- Overlay disjunct fires only for `useOverlayTexture && overlayHandle != 0xffffffff` quads — cement/runway/road decals, sparse on stock tier1. Bounded by `legacy_m2d_overlay_emit_quads` (telemetry: `Counters_AddM2dOverlayEmitQuad()` -> `s_m2d_overlay_emit_quads`, `gos_terrain_indirect.cpp:135/111`, grep-verified at HEAD).
- Thin-emit disjunct fires only when `!IsFrameSolidArmed()` — i.e. the GPU-solid substrate is NOT armed. In the **default-armed** config (substrate default-on, `s_frameSolidArmed` set when GPU solid dispatch runs, `gos_terrain_indirect.cpp:2006/2036`) this disjunct is FALSE, so the only re-projection is the sparse overlay set. When the substrate is *disarmed* (non-default), the cost reverts to the CPU thin-emit population — but that is the legacy CPU path's pre-existing cost envelope, not a new per-quad re-projection on top of the GPU path; and even there the `clipInfo==0` short-circuit skips projection for off-cone corners.
- Worst case (armed, default): `4 x (overlay quads with >=1 on-screen corner) <= 4 x legacy_m2d_overlay_emit_quads`. This count is already measurable per-mission from existing cost-split telemetry **before** flip.

So the corrected design's projection cost is bounded to the sparse overlay-quad set — NOT x all-terrain-quads. **Sparse-proven: Y.** The 146->42 regression cannot recur with the lazy `pzNeeded` gate in place.

**Gate (carry-forward, MINOR, see Finding 5):** make this a pre-land checklist item — read `legacy_m2d_overlay_emit_quads` per tier1 mission from the cost-split log, confirm `4 x N` projectZ is under the per-frame CPU budget, before the precursor lands. Decal-dense mod missions are the unbounded tail; defer to mc2-render-perf-expert only if the counter shows >~2000 overlay quads/frame on any tier1 mission.

---

## Finding 5 — Probe re-author + perf-gate-as-checklist (MINOR)

`MC2_M2D_PZ_PARITY` does **not** exist at HEAD `d7ff1c1` (`grep -rn MC2_M2D_PZ_PARITY mclib/ GameOS/` -> zero hits); it was carried in the reverted commit `29ae435`. Confirms the revert was clean. The redesign's §probe-fix points are sound and should be implemented as specified, with these grep-grounded constraints:

- **No `Renderer != 3` arm-guard in the probe-arm condition** is correct per the redesign — that guard sat in the dead legacy arm; the modern fast path is the perspective GL renderer (`Environment.Renderer != 3`). HOWEVER (v1 Finding 1 tail, still applies): VPL sets `clipInfo = inView` (not `onScreen`) when `Environment.Renderer == 3` (`terrain.cpp:1582` slim, `:1762` twin), while the `pz` sentinel stays gated by `onScreen` (`:1577`/`:1749`). Under `Renderer==3` the `clipInfo==0` guard and the sentinel could disagree, so the corrected edit's bit-exactness is proven **only for `Renderer != 3`** (the only renderer the TerrainPatchStream fast path drives). The probe should therefore **assert/log `Environment.Renderer != 3` at arm time** (not gate arming on the dead legacy arm, but a one-line scope assertion) so a future `Renderer==3` reintroduction cannot silently invalidate the parity claim. This is a scope assertion, NOT the dead-arm guard the redesign correctly removes — they are different and both required.
- First-N-mismatch emit (not frame-modulo), static `getenv` at file scope, summary line with a `checked > 0` assertion: implement as the redesign specifies. The corrected edit will produce **zero mismatches** (Finding 2 bit-exactness); the `checked > 0` assertion is the load-bearing guard against a silent no-op probe (the probe must prove it actually exercised the M2d path under the smoke wrapper env). Confirm the smoke wrapper sets the env (`scripts/run_smoke.py`) and that tier1 missions with cement/runway/apron decals (mc2_24 road, mc2_01 apron) drive `legacy_m2d_overlay_emit_quads > 0` so `checked > 0` holds — otherwise the probe passes vacuously.
- Keep "stop on any nonzero mismatch." `parity_finds_gpu_substrate_bugs_visual_smoke_misses.md` applies: the decal-pixel canary alone would NOT catch a sentinel divergence (cost-only false-positive, invisible to visual smoke); the numeric probe is the load-bearing validator.

---

## §Confirmed-safe-after-scrutiny (what held)

- **v2 scoping analysis is correct.** M2d overlay (`quad.cpp:2260`) + not-armed thin-emit (`:2154`) are the only live pzc/pzTri consumers in `if (fastPathEligible)`; `:2122` only gates the cull; legacy `pzTri` uses are post-`:2320` `!fastPathEligible` and unreachable. pzc IS lazily-scopable. (Finding 3 enumeration.)
- **`:2122` return relocation safety.** Nothing between the old `:2122` and the fast-path `return` at `:2318` consumes pzc/pzTri except the dead-when-armed thin-emit (`:2154-2223`) and the M2d overlay (`:2260-2316`); `pzNeeded` is the exact union of their gates, so the early return at `:2122` never drops live work. (Finding 3.)
- **`projectForTerrainAdmission` purity** re-confirmed at HEAD (`camera.h:526-539` -> `projectZ`; frame-stable `worldToClip`; `setInverseProject` does not feed `projectZ`). Call-order vs. VPL irrelevant. (v1 Finding 2.)
- **`TERRAIN_DEPTH_FUDGE` constant** = `0.002f` (`quad.cpp:1954`), boundary `>=0 && <1` (`:2108`) — bit-identical in corrected edit. (v1 Finding 3.)
- **`eye` symbol + `(vx,vy,elevation)` triple + `OVERLAY_ELEV_OFFSET` exclusion** all confirmed at HEAD (`camera.h:1124`; `vertex.h:75/77/43/88`; `terrain.cpp:1567`/`:1731`). (v1 Finding 4.)
- **Revert is clean.** HEAD `d7ff1c1` reverts `29ae435`; `quad.cpp:2105-2109` is the original pre-precursor `cv->pz` read; `MC2_M2D_PZ_PARITY` absent. Baseline is the ~146fps clean state.

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **The redesign must be rewritten to the §Corrected edit (Finding 1).** This is not a mechanical tweak of the redesign — the redesign's *mechanism* is wrong (reads `cv->pz`) even though its *scoping* is right. The corrected edit is the v1-mechanism (re-projection + `clipInfo==0` guard, proven bit-exact) under the v2 lazy `pzNeeded` scope. Confirm with mc2-render-expert that `clipInfo` is the contract field (not a recomputed `onScreen`) before code — single load-bearing decision, carried from v1.
2. **`Renderer != 3` scope assertion in the probe (Finding 5)** — distinct from the dead-arm guard the redesign correctly removes. Both are required: remove the dead-arm guard, add a one-line `Renderer != 3` scope assertion at probe-arm.
3. **Finding 4/5 perf-bound as a pre-land gate**, not an assumption: read `legacy_m2d_overlay_emit_quads` from the cost-split log per tier1 mission and confirm `4 x N` projectZ under per-frame CPU budget before the precursor lands.
