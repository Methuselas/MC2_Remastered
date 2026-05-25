# Adversarial Plan Review — Step 5 (VPL cull-cascade slim pass, option 5B)

**Verdict: BLOCK**

**CRITICAL: 2 — MAJOR: 3 — MINOR: 2**

**Highest-risk finding (one sentence):** The design's central "conservative-superset / under-inclusion impossible" claim is provably backwards — the OLD `clipInfo`/`onScreen` test is *looser* than the exact frustum that `quadAabbInFrustum` implements (it unconditionally admits every vertex within 768 world-units of the camera including everything behind the near plane, dilates the lateral cone by 384u, and force-admits out-of-play-area vertices), so the slim pass is a strict **subset** of the old active set and will drop terrain quads and object blocks at normal oblique-camera zoom.

Scope: Step 5 of `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md` (v3), as described in the review brief. Ground truth: HEAD `2e11617`; shipped helper from `038d5e2`. Every citation below grep-verified at HEAD.

---

## CRITICAL-1 — The conservative-superset claim is inverted; the slim pass is a strict subset of the old active set (mass under-inclusion / object + terrain drop)

**Plan section faulted:** Step 5 "Claim: conservative-superset (never false-negative), so slim active-set superset of old active-set; over-inclusion safe, under-inclusion impossible." Parity assertion `legacyVis && !vplVis => superset_violation`, zero = pass.

**Ground truth (default path — `MC2_VERTEX_PROJECT_FAST` unset, legacy fallback `mclib/terrain.cpp:1589-1748`):**

The cull-cascade decision the slim pass replaces is `currentVertex->clipInfo`, written at `terrain.cpp:1710` for the default `usePerspective && Environment.Renderer != 3` case:

```
1708:		if (eye->usePerspective && Environment.Renderer != 3)
1710:			currentVertex->clipInfo = onScreen;
```

`onScreen` is computed `terrain.cpp:1599-1676`:

- `1603: onScreen = true;` — default admit.
- `1620: if (distanceToClip > CLIP_THRESHOLD_DISTANCE)` — the angular reject is **only entered when `distanceToClip > 768.0f`** (`CLIP_THRESHOLD_DISTANCE` = `768.0f`, `terrain.cpp:1362`). For any vertex within 768 units of the camera in the XZ-projected plane, `onScreen` stays `true` unconditionally. **There is no near-plane test anywhere in this path**; a vertex directly behind the camera but within 768u is admitted.
- `1625: float extent_angle = VERTEX_EXTENT_RADIUS / distanceToEye;` (`VERTEX_EXTENT_RADIUS` = `384.0f`, `terrain.cpp:1365`) and `1626/1634: object_angle > (vClipConstant + extent_angle)` — even past 768u the cone is **deliberately dilated** by adding `extent_angle` to the half-angle. This is looser than the true frustum lateral planes.
- `1660-1665: bool isVisible = Terrain::IsGameSelectTerrainPosition(vPos) || drawTerrainGrid; if (!isVisible) { currentVertex->hazeFactor = 1.0f; onScreen = true; }` — vertices **outside the play-area rectangle force `onScreen = true`** (`terrain.cpp:1664`). `IsGameSelectTerrainPosition` is the rectangle test at `terrain.cpp:687-700`.

The slim pass replaces this with `eye->quadAabbInFrustum(vplPlanes, wp, wp)` after `eye->extractFrustumPlanes(vplPlanes)`. The shipped helper (`038d5e2`, `mclib/camera.cpp` `Camera::extractFrustumPlanes` / `Camera::quadAabbInFrustum`) is a Gribb-Hartmann 6-plane test against `worldToClip`, with the projection swizzle `s=(-wx,wz,wy)` folded into the planes and `near = rZ` (native [0,1]). The degenerate `mn==mx==wp` p-vertex test collapses to an exact point-in-frustum test (for `mn==mx`, every plane's p-vertex is `wp` itself, so `quadAabbInFrustum` returns `dot(plane, wp) >= 0` for all 6 planes — a true point-in-frustum predicate, near plane included). It is conservative **with respect to the true frustum** (never rejects an AABB that truly intersects the frustum) — but that is NOT the same property the plan needs.

**The break:** The plan needs `oldOnScreen ⊆ trueFrustum` for "slim ⊇ old". The reverse holds:

- (A) Any terrain vertex within 768u of the camera, including everything behind the near plane: `oldOnScreen = true`, `quadAabbInFrustum = false` (fails near plane / side planes). `legacyVis && !vplVis` — the plan's own `superset_violation`, every frame, at normal zoom. MC2's camera is oblique 30°-cinematic per `memory/camera_model_oblique_cinematic.md`; near-camera and behind-near-plane terrain vertices are the common case, not an edge case.
- (B) Cone-dilation margin (384u angular pad): vertices just outside the true lateral planes but inside the padded cone: `old = true`, frustum `= false`.
- (C) Out-of-play-area vertices: `terrain.cpp:1664` forces `old = true`; frustum rejects any that are also outside the view volume.

The plan conflated "helper is conservative vs. the true frustum" with "slim set ⊇ old set." Those are different sets. The design's own parity probe (`legacyVis && !vplVis => superset_violation`, zero = pass) is not a passing guard — run honestly it FIRES every frame and is itself the proof the retirement is unsound as specified. Treating zero-violations as the expected outcome is the tell that the probe was reasoned about but not mentally executed against the real `onScreen`.

**Consequence (load-bearing):** `clipInfo` gates `setObjBlockActive`/`setObjVertexActive` (`terrain.cpp:1715-1718`). Per `memory/cull_gates_are_load_bearing.md` these gates control object `update()`, object lifecycle (`setExists(false)`), TGL pool budget, and per-instance refresh — bypass/under-inclusion cascades into Fix-A-class ghost-mech / streak / silent shape drop. Under-inclusion here de-activates blocks whose only-admitted vertices were near-camera / behind-near-plane / out-of-play, dropping their objects.

**Required design change (architectural — needs sign-off):** The slim pass MUST reproduce the OLD admission predicate, not the true frustum. Either:
1. Port `onScreen` verbatim (the dilated angular sphere-cone test at `terrain.cpp:1620-1640` using `vClipConstant`/`hClipConstant`/`VERTEX_EXTENT_RADIUS`/`CLIP_THRESHOLD_DISTANCE` + the `IsGameSelectTerrainPosition` force-true clause) as the per-vertex cull, NOT a Gribb-Hartmann frustum test; OR
2. Dilate the frustum-AABB test so it is provably a superset of `onScreen` (push all 6 planes out by a margin ≥ the worst-case `onScreen` slack, AND drop the near plane entirely since `onScreen` has none, AND OR-in `!IsGameSelectTerrainPosition`). This is no longer "the shared helper from Step 3" — picking wants the tight test, cull wants the loose one. They are NOT the same primitive; the B3/B5 "shared helper" premise is unsound for Step 5.
3. Surface to advisor: whether the OLD `onScreen` over-admission is itself load-bearing for downstream consumers (it is — see CRITICAL-2). If so, option 1 is mandatory and the "eliminate per-vertex projection" goal for Step 5 is only partially achievable (the angular test is per-vertex but cheap; the `projectForTerrainAdmission` call is what gets retired).

---

## CRITICAL-2 — `clipInfo` is read by `isTerrainQuadVisible` (terrain draw gate) BEFORE quad.cpp overwrites it; the narrowed slim value drops terrain quads independent of the objmgr cascade

**Plan section faulted:** Step 5 / brief stress-point 6 — design claims Step 8b retires the `clipInfo` readers and frames the only risk as write-ordering vs. the quad.cpp second writer.

**Ground truth:**

- VPL writes `clipInfo` at `terrain.cpp:1710/1713` inside `Terrain::geometry`, called from `code/mission.cpp:505`.
- `Terrain::geometry` later calls `quadSetupTextures` (plan's own Step 8c note cites `terrain.cpp:1809`). `TerrainQuad::setupTextures` is `mclib/quad.cpp:685`.
- Inside `setupTextures`, `isTerrainQuadVisible(*this)` is called at `quad.cpp:908` and **reads `vertices[*]->clipInfo`** (`quad.cpp:401-408`) to compute `quadVisible` — the gate for whether the quad emits at all:

```
quad.cpp:908:			quadVisible = isTerrainQuadVisible(*this);
quad.cpp:401:		long clipped1 = quad.vertices[0]->clipInfo + quad.vertices[1]->clipInfo + quad.vertices[2]->clipInfo;
quad.cpp:403:		return (clipped1 || clipped2) != 0;
```

- THEN `setupTextures` overwrites `clipInfo` at `quad.cpp:1035/1037` (and `:1102/1169/1236`) with its own `projectForTerrainAdmission`-derived `clipData`.

So there IS a consumer that reads the VPL-written `clipInfo` between the VPL write and the quad.cpp overwrite: `isTerrainQuadVisible` at `quad.cpp:908`, in the same `Terrain::geometry` invocation. It is not a debug/parity reader (the plan's table classifies `clipInfo` consumers as "inverseProject (b) + parity (a) + debug (d)" — this production terrain-draw-gate consumer is **missing from the plan's consumer table entirely**).

**The break:** With the slim pass narrowing `clipInfo` to the frustum value (same under-inclusion as CRITICAL-1), `isTerrainQuadVisible` returns `false` for near-camera / out-of-play / cone-margin quads the stock build draws → **terrain holes / missing terrain quads** at normal zoom — a visible regression in the default config, in the same commit, that the objmgr-side parity assertion does not even cover (different consumer). Step 8b has not run at Step 5; the brief explicitly notes "it hasn't yet."

**Required design change:** The plan's consumer table for `clipInfo` must add `quad.cpp:908 isTerrainQuadVisible` (production terrain-draw gate, class (a)/survivor-until-Step-8b). Step 5's slim `clipInfo` value MUST remain a superset of the old `onScreen` (folds into CRITICAL-1's fix) OR Step 5 must not write `clipInfo` at all and instead keep the old `onScreen` computation alive solely for the `clipInfo` write while retiring only the `projectForTerrainAdmission` call and the `px/py/pz/pw` writes. Decouple the cull-cascade-bit retirement from the `clipInfo` retirement; they have different consumers with different lifetimes.

---

## MAJOR-1 — Moving the reduction guard from `clipInfo` to `vplVis` is NOT verbatim; it shrinks the `leastZ/mostZ/leastW/mostW` accumulation set and changes `setInverseProject` extents

**Plan section faulted:** Step 5 / Step 6 — "reduction stays verbatim"; brief stress-point 4.

**Ground truth:** the reduction at `terrain.cpp:1720-1743` is nested inside `if (currentVertex->clipInfo)` (`terrain.cpp:1715`) AND `if (inView)` (`terrain.cpp:1720`). `inView` is the return of `eye->projectForTerrainAdmission(vertex3D, screenPos)` at `terrain.cpp:1685`; `screenPos.z/.w/.y` feed `leastZ/mostZ/leastW/mostW/leastWY/mostWY`. Those feed `eye->setInverseProject(...)` at `terrain.cpp:1891`, consumed by `Camera::inverseProjectZ` and the tacmap viewport callsites (`memory`/plan Step 6 chain; the plan's own A4 note confirms this chain is live).

The reduction's OUTER guard is `clipInfo`. The slim pass redefines `clipInfo`/`vplVis` to a strictly smaller set (CRITICAL-1). Even if the inner `if (inView)` projection is kept alive (the plan keeps `:1685` + `:1722-1742` "verbatim" until Step 6), the SET of vertices that reach the accumulator changes because the gating `clipInfo` shrank. Therefore the min/max extents change, and `setInverseProject` gets different arguments → tacmap F-key viewport + picking unprojection drift. The plan's claim that the reduction is "verbatim" is false: the body is verbatim, the gating predicate is not.

**Required design change:** Step 5 must explicitly state the reduction's outer guard remains the OLD `onScreen`/old-`clipInfo` value (not `vplVis`) until Step 6 retires the chain, OR fold this into CRITICAL-1's "keep old predicate" fix so `clipInfo` is unchanged. The Step 6 `setInverseProject` byte-compare gate would catch the drift after the fact, but the plan should not ship a commit it knows changes the reduction set while claiming it doesn't.

---

## MAJOR-2 — Slim pass replaces only the legacy block `:1705-1718`; the fast-path block `:1561-1571` is left on the OLD cull-cascade, creating a stale-contradiction dead branch for `MC2_VERTEX_PROJECT_FAST=1` developer runs

**Plan section faulted:** brief description — "Slim pass ... Replaces ONLY the cull-cascade decision at legacy `:1705-1718`."

**Ground truth:** `s_vpFast = getenv("MC2_VERTEX_PROJECT_FAST") != nullptr` (`terrain.cpp:1425`, default-off, `terrain.cpp:1378`). `scripts/run_smoke.py:251` lists `MC2_VERTEX_PROJECT_FAST` in the env-scrub list, so the fast path is provably unreachable in the tier1 smoke gate (Finding 5 mitigated *for smoke*). BUT the fast path block at `terrain.cpp:1563-1571` writes the same `objBlockInfo[].active`/`objVertexActive[]`/reduction via its own inlined `clipInfoFinal`. If Step 5 modifies only the legacy block, a developer running `MC2_VERTEX_PROJECT_FAST=1` (the entire reason that path exists) gets the OLD un-slimmed cull-cascade with no slim pass and no parity — a stale branch that silently contradicts the shipped semantics. This is the additive-debt anti-pattern the plan's own "No env-gated dead code" load-bearing constraint forbids.

**Required design change:** Step 5 must either (a) apply the identical slim replacement to BOTH the fast-path inline (`terrain.cpp:1561-1571`) and the legacy block, or (b) delete the fast path in this commit (it is D1-closed dead weight per `memory/vertexproject_loop_asymptotic.md` — ~0% mean improvement), or (c) explicitly state the fast path is retired in Step 8c and gate it `false` now so it cannot be flipped on into a contradictory state. The plan must not leave two divergent cull-cascade writers.

---

## MAJOR-3 — Parity probe `MC2_VPL_CULL` as specified cannot pass; it is a bug-detector being treated as a gate

**Plan section faulted:** brief — "Parity assertion: env-gated `MC2_VPL_CULL`, `legacyVis && !vplVis` => `superset_violation`, zero = pass."

Given CRITICAL-1, `legacyVis && !vplVis` is `true` for a large fraction of vertices every frame at normal zoom. The probe is correctly *designed* (it asserts the superset property) but the design assumes it passes. It does not. This is MAJOR not CRITICAL only because the probe, if actually run before commit, correctly blocks the bad commit — but the plan presents zero-violations as the expected steady state, which means the plan was written without executing the probe's logic against the real `onScreen`. Recommendation: keep the probe; change the plan's stated expectation to "the probe MUST be run and show zero violations across all 5 tier1 missions at multiple zoom levels INCLUDING near-camera ground before the commit lands; a nonzero count is a BLOCK, not a tuning signal." After CRITICAL-1 is fixed (old predicate preserved), the probe legitimately reads zero.

---

## MINOR-1 — Plan's `clipInfo` consumer table is incomplete (missing the production terrain-draw-gate reader)

The Step 1 scope note and the consumer table enumerate `clipInfo` consumers as the per-corner clip logic in `quad.cpp` (`:1021/1023/...`) plus `inverseProject` + parity + debug. The production reader `isTerrainQuadVisible` (`quad.cpp:397`, called `quad.cpp:908`) that gates terrain-quad emission is absent. Even independent of CRITICAL-2, the table should list it so Step 8b's "retire clipInfo readers" scope is correct. Recommendation: add row `quad.cpp:908 isTerrainQuadVisible` to the Adjacent-fields `clipInfo` consumer list.

## MINOR-2 — `getBlockNumber()`/`vertexNum` bounds in the design match the engine guards (no off-by-one) — documented for the record

Grep-verified: `Terrain::setObjBlockActive` guard is `(blockNum >= 0) && (blockNum < numObjBlocks)` (`terrain.cpp:2042`); `Terrain::setObjVertexActive` guard is `(vertexNum >= 0) && (vertexNum < (realVerticesMapSide * realVerticesMapSide))` (`terrain.cpp:2056`). The fast-path inline uses `vp_numObjB = numObjBlocks` (`terrain.cpp:1444/1566`) and `vp_numActiveVerts = realVerticesMapSide * realVerticesMapSide` (`terrain.cpp:1445/1570`) — exact match, no off-by-one. The legacy block calls the guarded setters directly (`terrain.cpp:1717-1718`). The brief's stress-point 3 concern does not reproduce; recorded as MINOR only so the next reviewer does not re-investigate. Note: this safety is irrelevant if CRITICAL-1 drops the block before the setter is reached — the bounds are not the failure surface here.

---

## §Confirmed-safe-after-scrutiny

Stress-tested and held:

- **Frame-start clear ordering (brief stress-point 2 / external clear).** `code/mission.cpp:501-502` (`clearObjBlocksActive`/`clearObjVerticesActive`) run before `land->geometry()` at `mission.cpp:505`, within the same `Mission::update`. `clearObjVerticesActive` is a `memset` (`terrain.cpp:2063`). Frame-start clear is external and correctly sequenced as the brief states. No double-clear, no missing-clear.

- **`worldToClip` validity at slim-pass execution time (brief stress-point 2 / camera-not-ready).** `eye->update()` runs at `mission.cpp:476`, strictly before `land->geometry()` at `mission.cpp:505` in the same tick. `Camera::update` (`camera.cpp:1609`) has no early return between entry and `calculateProjectionConstants()` at `camera.cpp:1752` except the hard `INVALID_CAMERA` failure at `camera.cpp:1621` (non-`BASE_CAMERA` class — not a first-frame transient). `calculateProjectionConstants` calls `setCameraOrigin()` at `camera.cpp:2358`, which computes `worldToClip` at `camera.cpp:2325`. So `worldToClip` is freshly recomputed every frame before the slim pass; there is no stale/zero-`worldToClip` first-frame window. Moreover the OLD `onScreen` test reads camera state (`cameraFrame` `camera.cpp:2350-2353`, `vClipConstant`/`hClipConstant` `camera.cpp:2347-2348`) set in the SAME `calculateProjectionConstants` call, so old and new read camera state refreshed at the same point — no first-frame asymmetry. The `turn < 4` early-out the brief references is inside `Camera::inverseProject` (`camera.cpp:771`), NOT in `Terrain::geometry`'s VPL path; it does not apply to Step 5. (This safety is conditional on the slim pass living inside `Terrain::geometry` at/after the current VPL block as the plan states; if a future revision hoists the slim pass before `eye->update()` this reopens.)

- **Degenerate point-AABB collapse (brief stress-point 1, helper mechanics).** For `mn==mx==wp`, `Camera::quadAabbInFrustum` (`038d5e2`, `camera.cpp`) selects p-vertex `= wp` for every plane regardless of sign, so it computes `dot(plane_p, wp) >= 0` for all 6 planes — an exact point-in-frustum predicate including the `near = rZ` plane. The p/n-vertex selection does nothing pathological at `mn==mx` (the ternaries just all resolve to the single point). The helper math itself is sound; the bug is NOT in the helper — it is that this *exact* predicate is the wrong (tighter) set vs. the old loose `onScreen` (CRITICAL-1). The swizzle fold (`worldPlane = [-a, c, b, d]`) is an exact signed-axis permutation per the commit's own derivation; no approximation introduced.

- **Bounds guards (MINOR-2).** Design inline bounds exactly match engine guards; no edge-block off-by-one.

- **Fast-path smoke reachability (brief stress-point 5, smoke scope only).** `MC2_VERTEX_PROJECT_FAST` is in the `run_smoke.py:251` env-scrub list, so the fast path cannot be flipped on by a tier1 mission/env during the smoke gate. (The non-smoke developer-run hazard is the separate MAJOR-2.)

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **CRITICAL-1 resolution path.** Whether Step 5's slim pass must reproduce the OLD `onScreen` predicate verbatim (dilated angular cone + no near plane + `!IsGameSelectTerrainPosition` force-admit) rather than a true frustum test. If yes, the "shared frustum-AABB helper with Step 3" (B3/B5) premise is invalid for Step 5 — picking needs the tight frustum, cull needs the loose legacy predicate; they are different primitives. This changes the plan's "eliminate per-vertex projection entirely / 5B rationale (d)" claim: the cheap per-vertex angular test stays, only `projectForTerrainAdmission` + `px/py/pz/pw` retire. Needs `mc2-terrain-indirect-expert` + `mc2-cpu-gpu-offload-expert` sign-off — this partially walks back the stated Step 5 goal.

2. **Is the OLD over-admission load-bearing?** `onScreen`'s 768u/cone-dilation/out-of-play over-admission feeds BOTH the objmgr cull cascade AND `isTerrainQuadVisible` (terrain draw). Confirm with `mc2-render-perf-expert` whether the over-admission is intentional slack (triangles whose verts are off-screen but whose interior is visible — the `:1622` comment "close enough to screen that its triangle MAY be visible" says it is) before any tightening. If load-bearing, option 1 above is mandatory.

3. **Decouple `clipInfo` retirement from cull-cascade-bit retirement (CRITICAL-2).** `clipInfo` has a production terrain-draw consumer (`isTerrainQuadVisible`) that outlives Step 5. Decide whether Step 5 writes `clipInfo` at all, or keeps the old `onScreen` solely for `clipInfo`/`isTerrainQuadVisible` while retiring only the projection. Affects Step 8b scope.

4. **Fast-path disposition (MAJOR-2).** Delete the D1-closed fast path now, or mirror the slim pass into it, or hard-gate it false. User decision per the plan's own "no env-gated dead code" constraint.
