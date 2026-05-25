# Adversarial Plan Review — M2d overlay-pz-gate repoint precursor

**Verdict: READY-WITH-FIXES**

- CRITICAL: 1 (Finding 1 — off-screen sentinel divergence; the design's pre-flagged HIGH item)
- MAJOR: 0
- MINOR: 1 (Finding 5 — cost bounding is adequate but should be stated as a gate, not assumed)

**Finding-1 sentinel verdict (one sentence):** REACHABLE for overlay quads and large-population at the engine's oblique camera angles — the edit MUST add a `vertices[c]->clipInfo == 0` short-circuit (`pzc[c] = false`) ahead of the re-projection so the new code reproduces VPL's `pz = -0.5f` sentinel co-decision bit-for-bit; ships only with guard-X below.

Worktree: `.claude/worktrees/gpu-driven-rendering/`, branch `claude/gpu-driven-rendering`, HEAD `5c22e28`. Every citation grep-verified at this HEAD.

---

## Finding 1 — Off-screen sentinel divergence (CRITICAL; resolves the design's open HIGH)

### The two VPL paths (grep-verified)

`Terrain::render`'s vertex-project loop (the slim VPL loop, `mclib/terrain.cpp:1491-1603`, and the legacy twin `:1638-1762`) decides per vertex:

- `terrain.cpp:1497-1558` — `onScreen` computed **non-projectively** (angular sphere-clip cone: `distanceToClip > CLIP_THRESHOLD_DISTANCE` then `object_angle > vClipConstant + extent_angle`). Constants grep-verified: `CLIP_THRESHOLD_DISTANCE = 768.0f` (`terrain.cpp:1362`), `VERTEX_EXTENT_RADIUS = 384.0f` (`terrain.cpp:1365`), `vClipConstant = eye->verticalSphereClipConstant` (`terrain.cpp:1421`).
- `terrain.cpp:1565-1573` — `projectForTerrainAdmission` runs **only if `onScreen`**; writes `pzL = screenPos.z`.
- `terrain.cpp:1574-1580` — the `else` (i.e. `onScreen==false`) branch: `pzL = -0.5f` (the sentinel), `pwL = 0.5f`.
- `terrain.cpp:1601` writes `cv->pz = pzL`; `terrain.cpp:1603` writes `cv->clipInfo = clipInfoFinal`, where `clipInfoFinal = vp_isPerspRenderer ? onScreen : inView` (`terrain.cpp:1582`). The legacy twin is identical: `pz = -0.5f` at `terrain.cpp:1749`, `clipInfo = onScreen` at `terrain.cpp:1759` (perspective renderer, `Environment.Renderer != 3` — grep `terrain.cpp:1757`).

So for the perspective GL renderer (the only renderer the TerrainPatchStream fast path drives), **`cv->pz`'s sentinel and `cv->clipInfo` are co-decided by the same `onScreen` bit**: `clipInfo==0 ⟺ onScreen==false ⟺ pz==-0.5f`.

Subtlety confirmed by reading `Camera::projectZ` (`camera.h:436-516`): when `onScreen==true` but the point is outside the screen rect, `projectZ` still writes `screen.z` (lines 457-468 run before the rect test at 472 and the `return FALSE`). So VPL stores the **true projected z** in that case, NOT the sentinel. The `-0.5f` sentinel is written **only** in the `onScreen==false` angular-cone-reject branch. This is what makes the `clipInfo` bit an exact proxy for the sentinel.

### Old `pzc` formula (grep-verified, `quad.cpp:2105-2109`)

```cpp
bool pzc[4];
for (int c = 0; c < 4; c++) {
    float pz_adj = vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
    pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
}
```

`#define TERRAIN_DEPTH_FUDGE 0.002f` (`mclib/quad.cpp:1954`). **Finding 3 resolved here**: the design's `TERRAIN_DEPTH_FUDGE` and the task prompt's `0.002f` are the **same token, same value**; boundary is `>= 0.0f && < 1.0f` exactly (`>=`/`<`, not `>`/`<=`). The `.codex_tmp_isolate/quad_head.cpp:1442` `0.001f` hit is a stale temp-isolate scratch file, NOT the build source — disregard. (`mclib/tgl.cpp:2868` `(0.000f)` is a different translation unit's macro, not in this path.)

When VPL sentinels (`pz=-0.5`): `pz_adj = -0.498 < 0` → `pzc=false`. Correct old behavior: sentinel corner is excluded.

### The divergence (the design's pre-flagged HIGH)

The recommended fix re-projects **unconditionally** (no `onScreen`/`clipInfo` guard) and computes `pzc` from the raw re-projected `osp.z`. Therefore for any corner where VPL set `onScreen==false` (sentinel, old `pzc=false`) but the true projected `osp.z + 0.002 ∈ [0,1)`, the new code yields `pzc=true`. Behavior change: emits decal triangles VPL dropped.

**Is this reachable for overlay quads? YES, and it is large-population, not a corner case.** Proof:

1. The M2d producer gate is `if (useOverlayTexture && overlayHandle != 0xffffffff)` (`quad.cpp:2260`) — a texture-property gate, fully orthogonal to per-corner `onScreen` geometry. Nothing about being a cement/runway/road quad excludes a corner from `onScreen==false`.
2. `onScreen==false` is the angular sphere-clip reject using `vClipConstant`/`hClipConstant` (`terrain.cpp:1421-1422`, `1523/1530`). `memory/clip_w_sign_trap.md` documents these exact constants as **geometrically broken at steep pitch** — they "dropped 87% of on-screen buildings in measured test." The engine's camera is oblique 30° + cinematic, NOT top-down RTS (`memory/camera_model_oblique_cinematic.md`). So `onScreen==false` with a true `projectZ` z still in `[0,1)` is not rare — it is the **routine off-rect dilated band** (768u threshold / 384u extent) the Step 8 review characterized, hit every frame at the engine's normal camera angles.
3. The legacy per-quad `clipInfo` rewrite at `quad.cpp:1023-1037` is gated inside the water block (`if ((vertices[..]->pVertex->water & 1)...)` at `quad.cpp:989` AND `if (!(vertices[0]->calcThisFrame & 2))` at `:1008`). Cement/runway/road overlay quads are **non-water**, so that block does not run for them; their `clipInfo` is exactly what VPL wrote = `onScreen`.

Consequence: the `MC2_M2D_PZ_PARITY` probe **will fire nonzero** (false-positives: raster-clipped, cost-only, NOT the `9964d5a` missing-decal class — but it breaks the bit-parity claim the entire design rests on and makes it un-shippable as "bit-identical").

### Verdict: (ii) REACHABLE → needs this exact guard

The edit MUST replicate VPL's sentinel co-decision by gating on the VPL-written `clipInfo` bit. `clipInfo` is `DWORD` on `Vertex` (`mclib/vertex.h:88`), and `vertices[]` are `Vertex*` carrying `vx,vy` (`vertex.h:75`), `pz` (`vertex.h:77`), `pVertex->elevation` (`PostcompVertex.elevation`, `vertex.h:43`). Exact replacement for `quad.cpp:2105-2109`:

```cpp
bool pzc[4];
for (int c = 0; c < 4; c++) {
    // VPL writes pz = -0.5f (sentinel) iff clipInfo == 0 (onScreen==false) for
    // the perspective renderer (terrain.cpp:1577/1601/1603, :1749/1759). Reproduce
    // that co-decision so the sentinel corners stay culled bit-for-bit; do NOT
    // re-project them (re-projection would yield pzc=true for off-cone corners
    // whose true projectZ z is still in [0,1) — the divergence this guard closes).
    if (vertices[c]->clipInfo == 0) { pzc[c] = false; continue; }
    Stuff::Vector3D ov3D(vertices[c]->vx, vertices[c]->vy, vertices[c]->pVertex->elevation);
    Stuff::Vector4D osp;
    eye->projectForTerrainAdmission(ov3D, osp);
    float pz_adj = osp.z + TERRAIN_DEPTH_FUDGE;
    pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
}
```

Bit-exactness proof:
- `clipInfo==0` branch: VPL set `pz=-0.5` → old `pzc = (-0.498 >= 0 && < 1) = false`. New code forces `pzc=false`. **Match.**
- `clipInfo!=0` branch (VPL `onScreen==true`): VPL stored `pz = screenPos.z` from `eye->projectForTerrainAdmission(Vector3D(cv->vx,cv->vy,cv->pVertex->elevation), screenPos)` at `terrain.cpp:1567-1568` (twin `:1731-1734`). New code re-runs the **identical pure function** (see Finding 2) on the **identical (vx,vy,elevation) triple** (see Finding 4) → identical `osp.z` → identical `pz_adj` → identical `pzc`. **Match.**

`pzTri1/pzTri2`, the `if (!pzTri1 && !pzTri2) return;` cull (`quad.cpp:2122`), and the entire M2d producer (`quad.cpp:2260-2316`) stay UNCHANGED, as the design specifies.

`Renderer==3` residual: VPL sets `clipInfo = inView` (not `onScreen`) when `Environment.Renderer == 3` (`terrain.cpp:1582`/`1762`), while the `pz` sentinel stays gated by `onScreen`. Under `Renderer==3` `clipInfo` and the sentinel could in principle disagree. This does not affect the verdict: the TerrainPatchStream fast path is the modern perspective-GL substrate (`Environment.Renderer != 3`); `Renderer==3` is the legacy path the GPU-driven terrain substrate does not drive (`terrain_mvp_gl_false.md` establishes the modern path is the perspective GL renderer). The guard is bit-exact for `Renderer != 3`, which is the only renderer the fast path executes under. **Recommend the plan state this scope assumption explicitly and have the `MC2_M2D_PZ_PARITY` probe assert `Environment.Renderer != 3` at probe-arm time** so a future `Renderer==3` reintroduction can't silently invalidate the parity claim.

---

## Finding 2 — `projectForTerrainAdmission` / `projectZ` purity (CONFIRMED PURE)

`projectForTerrainAdmission` (`camera.h:526-539`) delegates to `projectZ` (`camera.h:436-516`). `projectZ` reads only camera projection state — `worldToClip` (`camera.h:138`, used `camera.h:447`), `usePerspective`, `viewMulX/Y`, `viewAddX/Y`, `screenResolution`. It writes only the out-param `screen`, the (nullptr-here) `optionalResult`, and `g_pzTrace`-gated trace globals (debug-only, default-off). **No static, no member mutation, no frame counter.**

The task flagged `setInverseProject` at `terrain.cpp:2160` as a candidate order-dependency. Refuted: `setInverseProject` (`camera.h:1114-1120`) writes only `startZInverse`, `startWInverse`, `zPerPixel`, `wPerPixel`. `projectZ` reads **none of those four**. The VPL-loop-vs-M2d-site call order is therefore irrelevant to `projectZ` output. `worldToClip` is built once per frame in `camera.cpp:2325` (`worldToClip.Multiply(worldToCameraMatrix, cameraToClip)`), not mutated mid-`Terrain::render`. **"Same inputs ⇒ same output" holds. Purity claim PROVEN.**

---

## Finding 3 — `TERRAIN_DEPTH_FUDGE` constant (CONFIRMED BIT-IDENTICAL)

Resolved inside Finding 1: `#define TERRAIN_DEPTH_FUDGE 0.002f` (`mclib/quad.cpp:1954`), old formula `(pz+TERRAIN_DEPTH_FUDGE) >= 0.0f && < 1.0f` (`quad.cpp:2107-2108`), boundary `>=`/`<`. The design's `osp.z + TERRAIN_DEPTH_FUDGE` with the same `>=0 && <1` is bit-identical. No off-by-epsilon. (The `.codex_tmp_isolate` `0.001f` and `tgl.cpp` `0.000f` hits are unrelated scratch/other-TU and do not contradict this.)

---

## Finding 4 — `eye` symbol + the (vx,vy,elevation) triple (CONFIRMED)

- `eye` is `extern CameraPtr eye` (`camera.h:1124`) and is already referenced in `quad.cpp` in this same TU's legacy path: `if (eye->usePerspective)` / `eye->projectForTerrainAdmission(...)` at `quad.cpp:1026, 1034, 1101, 1168, 1235`. The design's `eye->projectForTerrainAdmission` at `quad.cpp:2103` is symbol-valid and matches the existing camera symbol. **Confirmed in scope.**
- VPL fed `Stuff::Vector3D vertex3D(cv->vx, cv->vy, cv->pVertex->elevation)` (`terrain.cpp:1567`, twin `:1731`). M2d builds corners from `vertices[c]->vx`, `->vy`, `->pVertex->elevation + OVERLAY_ELEV_OFFSET` (`quad.cpp:2275-2277`) — but the **projection input** the guard uses is the raw `(vx, vy, pVertex->elevation)` triple, with NO `OVERLAY_ELEV_OFFSET` and NO rotation/`+cameraOrigin` adjustment. This is the **identical triple** VPL projected. **Confirmed identical.** (Note for the executor: do NOT add `OVERLAY_ELEV_OFFSET` to the projection input — that offset is a render-time z-fight bias on the emitted vertex `wz`, not part of the admission projection; adding it would itself break parity.)

---

## Finding 5 — Cost / decal density (MINOR)

`Counters_AddM2dOverlayEmitQuad()` (`gos_terrain_indirect.cpp:135`) increments `s_m2d_overlay_emit_quads` (`:111`), already logged as `legacy_m2d_overlay_emit_quads` in the cost-split telemetry (`gos_terrain_indirect.cpp:335/386`). With the Finding-1 guard, the projection cost is **strictly bounded**: at most 4 `projectForTerrainAdmission` per overlay quad, and the `clipInfo==0` short-circuit skips projection for off-screen corners, so the real worst case is 4×(overlay quads with all corners on-screen) ≤ 4 × `legacy_m2d_overlay_emit_quads`. This count is already measurable per-mission from existing telemetry **before** flip. This is adequate — but the plan currently leaves it as an assumption. Make it a gate: add a one-line assertion to the plan that `legacy_m2d_overlay_emit_quads` per tier1 mission is read from the cost-split log and the 4×N projectZ is confirmed under the per-frame CPU budget before the precursor lands (cement/runway/road overlays are sparse on stock tier1, but a decal-dense mod mission is the unbounded case). No deep instrumentation needed; defer to mc2-render-perf-expert only if the existing counter shows >~2000 overlay quads/frame on any tier1 mission.

---

## §Confirmed-safe-after-scrutiny

- **Purity / call-order independence** (Finding 2): proven — `setInverseProject` does not feed `projectZ`; `worldToClip` is frame-stable.
- **Depth-fudge constant** (Finding 3): bit-identical, correct boundary operators.
- **`eye` symbol + projection-input triple** (Finding 4): in scope; identical `(vx,vy,elevation)`; `OVERLAY_ELEV_OFFSET` correctly excluded from the projection input.
- **Producer untouched**: `pzTri1/2`, the `!pzTri1 && !pzTri2` cull (`quad.cpp:2122`), and the M2d `gos_PushTerrainOverlay` block (`quad.cpp:2260-2316`, calls `:2289/2296/2305/2312`) are unchanged — no risk of the `9964d5a` conflated-overlay class from this edit.
- **Does not pre-empt the deferred sibling slice** (`docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`): this precursor only repoints the CPU pz-source; it does not move the producer to GPU, leaving the full GPU port slice intact.
- **Verification harness shape**: `MC2_M2D_PZ_PARITY` mirroring `MC2_VPL_REDUCE` (`terrain.cpp:2057-2067` family) is sound; with the guard, the dual-source bit-compare should yield zero mismatches. `parity_finds_gpu_substrate_bugs_visual_smoke_misses.md` applies — the decal-pixel canary (mc2_24 road / mc2_01 apron) alone would NOT have caught the Finding-1 divergence (it is a cost-only false-positive invisible to visual smoke); the numeric parity probe is the load-bearing validator. Keep the "stop on any nonzero mismatch" rule.

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **Confirm the guard field is `clipInfo`, not a recomputed onScreen.** I selected `vertices[c]->clipInfo == 0` because it is the exact bit VPL persisted alongside the `pz=-0.5` sentinel for the perspective renderer, it is already live for non-water overlay quads at draw time, and it requires no recomputation of the (broken-by-design) angular sphere-clip test. The alternative — re-deriving `onScreen` at the M2d site — would re-introduce the `vClipConstant`/`hClipConstant` math and is both costlier and more fragile. Recommend the executor + mc2-render-expert confirm `clipInfo` is the contract field before code; this is the single load-bearing decision in the revision.
2. **`Renderer==3` scope assertion.** Recommend adding `gosASSERT(Environment.Renderer != 3)` (or an env-gated probe warning) at `MC2_M2D_PZ_PARITY` arm-time so a future legacy-renderer reintroduction cannot silently break the parity claim (Finding 1 tail).
3. **Finding 5 budget gate** — make the 4×N projectZ cost a pre-land checklist item read from the existing cost-split counter, not an assumption.
