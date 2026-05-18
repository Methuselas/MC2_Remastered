# Adversarial review: VertexProjectLoop retirement plan v1

**Plan reviewed:** `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`
**Reviewer mode:** adversarial, code-grounded; all file:line cites grep-verified at write-time against worktree `A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/` HEAD `bd9dea3`.
**Date:** 2026-05-14

---

## Verdict: BLOCK

The plan misclassifies a load-bearing GPU consumer of `hazeFactor` as "read site not located," misframes the legacy SOLID `MC2_TERRAIN_INDIRECT=0` fallback as automatically dying (it is a default-on opt-out the user invokes for parity bisects, and the user must keep it playable), and the mouse-picking repoint strategy (step 3) is a non-equivalence — it replaces a tile-ordering policy (least-pz of overlapping candidates) with a single-ray ground-plane intersection that has no occlusion semantics for elevated/overlapping terrain. Step 8 (Fix A retirement) is also premature in light of the Fix-A code surface that still latently writes `g_thinSlotMVP` from two call sites; deleting Fix A while VPL is gone removes the only mitigation if a NEW one-frame-lag interaction emerges from the cmd-patch retirement (step 2, in a sibling plan). Net: step-by-step the sequence has correctness gaps that the plan's "Path C (dirty-flag skip)" escape hatch does not actually cover. Rework the consumer table for `hazeFactor`, rewrite step 3's strategy (or scope it as a known regression with a user-confirmed acceptance gate), defer step 8 to a separate post-soak slice, and re-issue as plan v2.

---

## §A High-severity findings (CRIT)

### CRIT-1: `hazeFactor` GPU consumer misclassified — terrain distance fog breaks if VPL writes retire

**Plan section:** "Adjacent fields written by VPL" table, row `vertex->hazeFactor` — "Per recon: read site not located. Suspected `quad.cpp` fog packing. Pre-commit: grep `hazeFactor` opposite-direction before retiring its write."

**Code at risk:**
- `GameOS/gameos/gos_terrain_lighting.cpp:549-553` — terrain-lighting pack writes `vi.hazeFactor = v.hazeFactor` per vertex into the `GpuTerrainVertexInput` SSBO.
- `shaders/gos_terrain_lighting.comp:276-278` — the lighting compute shader reads `v.hazeFactor`, computes `fogFactor = 1.0 - v.hazeFactor`, and applies distance fog.
- `shaders/include/terrain_lighting_shared.hglsl:7` — `GpuTerrainVertexInput` declares `float hazeFactor` as part of the cross-CPU/GPU struct.

**Failure scenario:** if step 7 (VPL body deletion) lands without first retiring or replacing the `cv->hazeFactor` write at `terrain.cpp:1556` and `:1646/:1654/:1663/:1669/:1674/:1702`, the terrain lighting compute shader reads garbage (uninitialised CPU memory) for `hazeFactor` each frame. Visually this manifests as random per-vertex fog intensity — flicker, banding, or distance fog "stuck on" or "stuck off." Tier1 visual smoke may catch this (mc2_24 reactor has long distance fog gradients) but per memory rule `parity_finds_gpu_substrate_bugs_visual_smoke_misses.md` the wrong-data case may pass visually while being incorrect.

**Recommended remediation:** add a new row to the consumer table:
> | `gos_terrain_lighting.cpp:549-553` (pack) -> `gos_terrain_lighting.comp:277` (consume) | (b) Survivor — GPU lighting compute consumes per-vertex fog | **Must port or retire FIRST.** Compute hazeFactor on GPU from per-vertex world-space distance to eye, OR keep VPL alive solely for hazeFactor until that port lands. |

Then renumber the steps so the hazeFactor port becomes a precondition for step 7. Also strike the "Pre-commit grep" language — the recon already missed this site; the executing session must not be allowed to discover it during a delete commit. Add to "Pending grep verifications" the explicit `gos_terrain_lighting.cpp:549-553` cite so the next session reads it before touching VPL.

---

### CRIT-2: Step 3 mouse-picking repoint is not equivalent — drops occlusion ordering

**Plan section:** Step 3 "Mouse picking repoint" — "instead of walking every tile and reading `vertices[c]->pz`, project the cursor's screen-space ray into world space via `inverseProjectForPicking`... intersect with the terrain plane to get a world-space cursor point, then look up the tile under that point via `RecipeForVertexNum` or terrain mapdata."

**Code at risk:**
- `mclib/camera.cpp:749-832` — `Camera::inverseProject` (plan cites `:797-822`, the actual function starts at `:749`; `:797-822` is the inner loop where the function picks the **least-`pz`** tile when the cursor is over multiple overlapping tiles). Caller: `code/missiongui.cpp:742`.
- The function's contract: walk all tiles whose 4 vertices' `clipInfo` is true, find which ones the screen-cursor `(x,y)` is over via `overThisTile`, and if multiple, pick the one with the smallest pz (i.e. the front-most). This is a real occlusion test when terrain elevations vary (steep cliffs, layered ridges, mc2_17 water-shore overhangs).

**Failure scenario:** a screen ray intersected with a single canonical terrain plane has no concept of "which tile is in front" when the cursor projects onto multiple tiles at different elevations. On flat terrain the strategies are equivalent; on cliffs / ridge selection / shore picking they diverge. Specifically: clicking on a mech standing on a high mesa with another mesa behind it. The current code picks the near mesa (correct). Plane-intersection picks "wherever the ray hits z=ground_plane" which may be the far mesa or completely miss both. Off-by-one cursor positioning on these maps will be a real regression and the plan acknowledges "subtle... may only show on specific mission layouts."

**Recommended remediation:** rewrite step 3 strategy:

Option A (preferred): keep `inverseProject` as-is but feed it world-space tile positions from the recipe SSBO (which compute already has) rather than from `vertices[c]->pz`. This requires per-tile depth ranking via projection AT CLICK TIME (one projection per candidate tile, not per-frame for every vertex). Per-click cost: ~50 µs × N candidate tiles (typically <100). Preserves the least-pz tile-ordering policy.

Option B: explicitly scope step 3 as a known regression. Acknowledge in the plan body that picking-on-elevated-terrain will lose occlusion ordering; require user UAT on mc2_24 (cliffs), mc2_17 (water shore), and any mission with overlapping mesas BEFORE step 7. This is a stock-install-playability concern under `memory/stock_install_must_remain_playable.md` line 1 — picking is gameplay-critical.

Option C: defer step 3 to AFTER step 7 by accepting the GPU AABB hit-test downstream unlock as a prerequisite, not "informational." This re-orders the plan but keeps picking working until a known-better replacement lands.

Whichever option is chosen, update the verification text — "manual UAT on tier1 missions" is insufficient for a hidden occlusion semantic change. List the specific scenarios the user must drive (click on near mesa with far mesa behind, click on cliff edge, click on mech behind low ridge).

---

### CRIT-3: `MC2_TERRAIN_INDIRECT=0` legacy fallback retains VPL dependence; plan treats consumer (a) as dead but the user keeps invoking the env to debug

**Plan section:** consumer table — `Legacy SOLID/TES gVertex emit` at `quad.cpp:2301-2824` marked "(a) Chopping block. Dies with `MC2_TERRAIN_INDIRECT=0` legacy retirement." Load-bearing constraints — "No env-gated dead code... do NOT keep VPL alive behind an env var."

**Code at risk:**
- `GameOS/gameos/gos_terrain_indirect.cpp:54-66` — `IsEnabled()` checks `MC2_TERRAIN_INDIRECT`; literal `"0"` opts out, absent-or-anything-else returns true. This is documented at `:62` as the explicit fallback the team uses for "tier1 5/5 with both MC2_TERRAIN_INDIRECT=0 (legacy regression) and default-on" comparisons.
- `GameOS/gameos/gos_terrain_indirect.cpp:2593` — emits user-facing `advice=set MC2_TERRAIN_INDIRECT=0 to fall back to M2 legacy SOLID` on error. The legacy fallback is the documented escape valve.
- `.claude/agents/mc2-terrain-indirect-expert.md:56` — advisor brief: "`MC2_TERRAIN_INDIRECT=0` disables the indirect path entirely; legacy CPU walk runs. Useful as a discriminator."

**Failure scenario:** the plan declares consumer (a) "dies with `MC2_TERRAIN_INDIRECT=0` legacy retirement." But that legacy retirement does not exist as a queued plan; the legacy fallback is a deliberately-preserved bisect tool. If step 7 deletes the VPL function body, the legacy SOLID path at `quad.cpp:2301-2824` is reading from `vertex->pz` that is no longer being written. Then `MC2_TERRAIN_INDIRECT=0` produces undefined output and the team loses its parity-bisect lever. The constraint "No env-gated dead code" is inverted here: VPL is the WRITER for an env-gated PATH that survives. Deleting the writer breaks the path.

**Recommended remediation:** either (a) explicitly retire the legacy SOLID/TES `gVertex` path FIRST in a separate commit that the orchestrator schedules before this plan's step 7, with its own soak, and accept that `MC2_TERRAIN_INDIRECT=0` becomes "non-functional bisect-only" (and document that loudly); or (b) keep VPL alive under `MC2_TERRAIN_INDIRECT=0` only, with an early-out at the top of the loop. Option (b) violates "no env-gated dead code" only if you consider the legacy fallback dead — the team's own commit log says it's used. Add a new step 4.5 to the plan explicitly answering the question "what does `MC2_TERRAIN_INDIRECT=0` do after step 7?" and pick a strategy.

---

### CRIT-4: file-scope reductions (`leastZ/mostZ/leastW/mostW/leastWY/mostWY`) have **no readers** in the source tree but plan §6 frames them as TBD

**Plan section:** Load-bearing constraints — "Shared file-scope reductions. VPL contributes to `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reductions per `terrain.cpp:1549-1552`... Three writers exist (water path in `quad.cpp`, non-water in `terrain.cpp`, legacy fallback)... Retiring VPL's contribution requires either..."  Step 6 — "Audit shape: grep every reader of these globals."

**Code at risk:**
- `mclib/terrain.cpp:1367-1368` — defines the globals at file scope.
- `mclib/quad.cpp:491-496` — declares `extern float leastZ/leastW/mostZ/mostW/leastWY/mostWY`.
- Writers: `terrain.cpp:1575-1578` (VPL fast path), `terrain.cpp:1730-1742` (VPL legacy fallback), `quad.cpp:1034-1054`, `:1101-1120`, `:1168-1187`, `:1235-1254` (four water/non-water blocks).
- Readers: I grep'd `leastZ|mostZ|leastW|mostW|leastWY|mostWY` across `*.{cpp,h}` in the worktree — the only hits are the writers above, the `extern` declarations in `quad.cpp:491-496`, the function-local `float leastZ` in `Camera::inverseProject` at `:793` (DIFFERENT variable, local scope shadows nothing because the function doesn't `extern` the global), and a comment in `gos_terrain_indirect.h:97`. **No code reads the globals.**

**Failure scenario:** the plan treats step 6 as a significant decision point ("If readers survive, keep the contribution alive (Path A partial)... they MUST then come from somewhere else"). This is wasted effort and structural cost. The simpler and correct path: the reductions are dead writes. Confirm with one more pass of grep, then retire all six writers as part of the VPL deletion commit. **The risk is opposite to what the plan flags**: the plan-author may execute step 6 audit, find readers in `quad.cpp` (which are themselves writers), be confused, and conclude the reductions must be kept alive. Then VPL is preserved by accident under Path A partial.

**Recommended remediation:** rewrite step 6 to:
> Pre-execution verification: grep every reader of `leastZ/mostZ/leastW/mostW/leastWY/mostWY` outside the writer files (`terrain.cpp`, `quad.cpp`). Distinguish file-scope writers from the function-local `Camera::inverseProject:793` `leastZ` (unrelated stack variable). If no readers found, retire all writers — including the water path in `quad.cpp:1034-1254` — as a precondition for step 7. If readers found, escalate to the orchestrator before continuing.

Move this verification to step 0 or step 1 — it's a 5-minute grep, not a "decision point."

---

## §B Medium-severity findings (HIGH)

### HIGH-1: Step 8 (Fix A retirement) is sequenced unsafely — Fix A's per-slot MVP infra also gets written from a SECOND site the plan does not enumerate

**Plan section:** Step 8 — "delete or demote to env-gated debug per the debug-instrumentation rule."

**Code at risk:**
- `GameOS/gameos/gos_terrain_indirect.cpp:1602-1603` — Fix-A copies `curMvp` into `g_thinSlotMVP[g_thinRingSlot]` in one site.
- `GameOS/gameos/gos_terrain_indirect.cpp:2484-2485` — Fix-A ALSO copies an `mvp` into `g_thinSlotMVP[g_thinRingSlot]` in a SECOND site. Plan enumerates only "per-slot MVP snapshot, `terrainOverrideThinMVP`, `gos_terrain_indirect_getRingSlotMvp`" — the dual-write surface is wider than the plan's three names.
- Memory rule `memory/ring_slot_state_must_travel_with_slot.md` lines 1-N: "ring-slot state must travel with the slot... any uniform a GPU producer used to write a ring slot must be stashed alongside the slot; consumer re-uploads it at draw time."

**Failure scenario:** Fix B (clipPos in thin record) removes Fix A's role as defense-in-depth FOR THE CURRENT PIPELINE. Step 2 of this plan's siblings (cmd-patch retirement) and step 5 (cull-cascade audit) both potentially introduce NEW dispatch / fence interactions. If either of those slices reintroduces a sub-frame state asymmetry (compute writes slot S using state X, bridge reads slot S using state Y), Fix A would have caught it — its `g_thinSlotMVP` capture is generic. Retiring Fix A in step 8 removes the safety net while two adjacent plans are still wet.

**Recommended remediation:** defer step 8 to a separate post-soak slice. Plan v2 ends at step 7 + step 9 (contract refresh). After cmd-patch retirement ships AND its 7-day soak completes AND step 5 cull-cascade audit ships AND its soak completes, then a follow-up plan retires Fix A. This costs three lines of code (the two memcpy sites + the getRingSlotMvp accessor) staying live for a few weeks — a trivial debt against the safety value.

---

### HIGH-2: Plan cites `terrain.cpp:1559` and `:1689` / `:1700` for VPL writes but the actual VPL fast-path writes are `:1556-1561` and legacy fallback writes are `:1646/:1654/:1663/:1669/:1674/:1687-1702`

**Plan section:** "Writers of `vertices[c]->pz`" table.

**Code at risk:**
- `mclib/terrain.cpp:1556-1561` — VPL fast-path live writes (`cv->hazeFactor = hazeL; cv->px = pxL; ... cv->pz = pzL; cv->pw = pwL; cv->clipInfo = clipInfoFinal;`). The plan's `:1559` cite lands on `cv->pz = pzL`, which is correct but elides the other five field writes around it.
- `mclib/terrain.cpp:1646, :1654, :1663, :1669, :1674` — legacy fallback `currentVertex->hazeFactor` writes. The plan-cited `:1689` lands on `currentVertex->pz = screenPos.z` and `:1700` on the off-screen fallback `currentVertex->pz = -0.5f`.

**Failure scenario:** drift from recon to plan to execution. Line numbers are stable enough that a careful executor will land in the right region, but the per-field enumeration is missing. An executor who searches for `vertices[c]->pz` and finds only those lines might miss the px/py/pw/clipInfo/hazeFactor writes that need to die in the same commit. The plan's own "interleaved sequencing" constraint demands that "every commit answers the question 'is the loop body provably smaller after this commit?'" — and that means every field write in the loop must be enumerated, not just `pz`.

**Recommended remediation:** in plan v2, rewrite the "Writers" table to enumerate per-field, per-line: `cv->hazeFactor` at `:1556` and `:1646/:1654/:1663/:1669/:1674`, `cv->px` at `:1557/:1687`, etc. Then in the "Pending grep verifications" section, list every cite as a write-time-verify-before-delete obligation, not a write-time-verify-after-recon.

---

### HIGH-3: Plan declares Path A vs Path B/C in §11 but Path C (dirty-flag skip) is correctness-unsafe across camera rotations

**Plan section:** Step 5 — "fall back to Path C (dirty-flag skip) for VPL: keep the loop alive but skip it when `terrainMVP` hasn't changed since last frame. Saves ~475 µs on static-camera frames; preserves correctness on moving frames."

**Code at risk:**
- VPL writes `objBlockInfo[blockNum].active` and `objVertexActive[vertNum]` per `terrain.cpp:1565-1571` only when `clipInfoFinal == true`.
- Path C says "skip the loop when MVP unchanged." But `objBlockInfo[].active` and `objVertexActive[]` are PER-FRAME state used by downstream cull cascades that gate `update()` AND allocation AND lifecycle per `memory/cull_gates_are_load_bearing.md:N`. They are NOT idempotent — if VPL skips a frame, the previous frame's `active` values persist.
- This is precisely the bug class `memory/cull_gates_are_load_bearing.md` is named for: "bypass cascades into streaks, destruction, silent shape drop-outs."

**Failure scenario:** consider a frame N with camera mid-rotation (MVP changing) — VPL runs, sets `objBlockInfo[k].active=true` for visible blocks, sets others false (well, it sets only true; the false-reset is implicit in the per-frame init at the loop top — actually it ISN'T in this code — looking at `terrain.cpp:1408-1410` only `leastZ/leastW/mostZ/mostW/leastWY/mostWY` reset; `objBlockInfo[].active` does NOT reset). Then frame N+1 camera stops; MVP unchanged; Path C skips VPL. `objBlockInfo[]` carries frame N's `active` flags. Cull-cascade consumers see stale visibility for one frame — usually OK. BUT: at the next camera nudge (a mouse jiggle, a heartbeat-triggered MVP tweak from animations), VPL runs again and writes new `active=true` bits — and the OLD true bits from N are NEVER explicitly cleared by VPL (VPL only sets to true, never to false). So Path C compounds with VPL's already-additive `active` semantics into stale visibility that doesn't decay.

**Recommended remediation:** if Path C is genuinely needed, the plan must specify HOW the cull-cascade arrays reset. Add to step 5: "Before considering Path C, confirm that `objBlockInfo[].active` and `objVertexActive[]` are reset elsewhere in the frame (currently uncertain — needs grep of `setObjBlockActive(...,false)` and `memset(objBlockInfo`...)`). If not, Path C is unsafe; pick (a) or (b) instead." Without that confirmation, Path C is BLOCK-grade.

---

### HIGH-4: Step 4 mask-dispatch reachability under-stated — the path IS opt-in (`MC2_TERRAIN_MASK_DISPATCH` env literal != "0") but plan treats it as uncertain

**Plan section:** Step 4 — "Pre-commit grep: confirm mask-dispatch's actual reachability. If it's reached only under env-gated paths (`MC2_TERRAIN_MASK_DISPATCH=1`?), document and either gate-default-off → unreachable, or include the retirement."

**Code at risk:**
- `GameOS/gameos/gos_terrain_mask_dispatch.cpp:78-85` — `MasterEnabled()` returns false if env unset or `"0"`. Default-off.
- `GameOS/gameos/gos_terrain_mask_dispatch.cpp:211-223` — pz cull lambda; grep-verified, matches the plan's cite.

**Failure scenario:** the plan flags this as "uncertain reachability" but the grep is a 30-second check. Mask-dispatch is default-off. Its `pz` read at `:211` is unreachable in default config, but the orchestrator has been using `MC2_TERRAIN_MASK_DISPATCH=1` as a parity-bisect tool. Same legacy-fallback concern as CRIT-3.

**Recommended remediation:** state in plan v2: "mask-dispatch is default-off. Under the env-on configuration the pz cull at `gos_terrain_mask_dispatch.cpp:211` reads `vertex->pz`; the env-on path is a bisect tool, not production. Retirement strategy: include the pz-read retirement in the same commit that retires the analogous indirect-dispatch pz read (`gos_terrain_indirect.cpp:1639-1656`), so the bisect tool stays self-consistent." Then state explicitly that the bisect tool is retained as opt-in. Sibling to CRIT-3.

---

### HIGH-5: Cross-plan integration risk — CPU pack retirement, cmd-patch retirement, and Fix A retirement are sequenced as "queued / independent" but they share thin-record / ring-slot state

**Plan section:** Steps 1, 2, 8.

**Code at risk:**
- `gos_terrain_indirect.cpp:1455-1456, :1602-1603, :2484-2485, :2844-2848` — `g_thinSlotMVP[]`, `g_thinSlotMVPValid[]`, the two write sites, and `getRingSlotMvp()`.
- `gos_terrain_indirect.cpp:1674-1769` — thin-record write loop, including Fix-B's clipPos compute at `:1749-1769`.
- CPU pack retirement (step 1) deletes `s_shadow` (the thin-record CPU staging array per `gos_terrain_indirect.cpp:1674`) and `PackThinRecordsForFrame`. Cmd-patch retirement (step 2) deletes the second `glDispatchCompute` and bucket header dependency.

**Failure scenario:** order-sensitive overlap. If step 1 (CPU pack retire) lands before step 2 (cmd-patch retire), the thin-record producer changes mid-arc, and Fix-B's CPU-side mirror at `:1739-1769` must shift from CPU-staged writes to GPU-direct emission. If step 2 lands first, the indirect cmd-buffer becomes atomicAdd-driven, but `s_shadow` is still used to stage records, so consumer-side semantics change. Step 8 (Fix A retire) depends on both 1 AND 2 having stabilized.

The plan states "Step 1 and Step 2 ship independently as separate plan docs" — but the sequence matters and the plan doesn't specify which order. Worse, step 8 is gated on "after VPL is gone" but the explicit conditional is missing — what if step 1 ships first, breaks something, gets reverted, and step 8 lands anyway?

**Recommended remediation:** add a top-level "Step order graph" section to plan v2:
- Step 1 (CPU pack retire) BEFORE step 2 (cmd-patch retire). Justification: cmd-patch retirement needs the atomicAdd-driven cmd.count to NOT have CPU staging in parallel.
- Step 3 (picking) and Step 4 (mask-dispatch) can land in either order.
- Step 5 (cull-cascade) AFTER steps 1, 2, 3, 4. Justification: §B brainstorm.
- Step 6 (reduction audit) can run any time; recommend before step 5 to avoid wasted state-preservation work (see CRIT-4).
- Step 7 (VPL delete) AFTER 1-6.
- Step 8 (Fix A retire) deferred to follow-up plan, AFTER 7-day soak of step 7. (See HIGH-1.)
- Step 9 (contract refresh) immediately after step 7.

---

### HIGH-6: Step 5 cull-cascade audit punted to "separate recon agent" but the plan doesn't define what the audit's pass/fail criteria are

**Plan section:** Step 5 — "Required sub-audit (spawn as a separate recon agent, do NOT do inline)... If any consumer is (b), this plan blocks."

**Failure scenario:** the recon agent's deliverable is not specified. A future session reads "spawn the audit" and produces a report; the orchestrator reads it and judges. Subjective. Risk: the recon classifies all consumers as (a)/(c)/(d) optimistically, the orchestrator approves, step 5 ships, regression appears. Per memory rule `parity_finds_gpu_substrate_bugs_visual_smoke_misses.md`, structural verification beats visual.

**Recommended remediation:** specify the audit deliverable shape in plan v2:
- Markdown table with one row per `objBlockInfo[].active` read site and one row per `objVertexActive[]` read site (full grep, including transitive callers through pointer fields).
- For each site: file:line cite (grep-verified), classification (a/b/c/d), rationale (1-2 sentences), and a parity-check proposal IF the site is (c) re-derivable.
- An explicit "if any site is (b) survivor, this plan v2 blocks until plan v3 (re-pointing) is drafted" gate.
- Sign-off from at least one of `mc2-render-expert` / `mc2-terrain-indirect-expert` advisors per worktree CLAUDE.md advisor-invocation discipline.

---

## §C Low-severity findings (MED/LOW)

### MED-1: Plan's line cites have drifted

- "VPL writes per `terrain.cpp:1547-1571`" — actual is `:1556-1571` (parity vs live branch starts at `:1542`).
- "writers `terrain.cpp:1549-1552`" for `leastZ/mostZ/...` — actual writer is `:1575-1578` (this is a parity-vs-live drift; the parity SNAPSHOT block at `:1542-1552` does not touch the reductions).
- "`camera.cpp:797-822`" for `inverseProject` — function starts at `:749`; `:797-822` is the inner least-pz loop.
- "`mclib/camera.h:422-622`" for "8 categorical wrappers" — `inverseProjectForPicking` is actually at `mclib/camera.h:634`. The 422-622 range is forward-projection wrappers; inverse-projection wrappers are further down. Picking the right wrapper is non-trivial; the plan should name it explicitly: `Camera::inverseProjectForPicking` at `mclib/camera.h:634-637`.

**Recommendation:** re-grep all cites at plan v2 write time. Stable symbols, not lines.

### MED-2: Plan doesn't address `mclib/clouds.cpp` `Clouds::vertex.pz` write at `:216` other than "out of scope, separate struct"

Spot-check: `clouds.cpp:147-164` has its own local `hazeFactor` computation. So clouds are self-contained for this state. Confirmed safe — but the plan's "out of scope" carve-out should add a one-line "verified no shared state with terrain vertex struct" note so a future reader doesn't re-investigate.

### LOW-1: Plan's "Expected findings density: 2-4 CRITICAL, 4-6 MAJOR, 5-8 MINOR" pre-prediction is fine but borderline gaming the reviewer

Project rule does not constrain this, but the plan-author setting findings expectations before review is mildly anti-pattern. Note for plan v2.

---

## §D Confirmed-safe-after-scrutiny

### Step 9: Render contract refresh

The contract document at `docs/render-contract.md:115-125` (D1 bucket: "Terrain world-space submission gated by projected depth") explicitly identifies the `pz`-as-correctness-input as "active, highest-priority contract violation, required cleanup: remove projected-depth correctness dependence." After step 7, the synthesizer will mark D1 as resolved — there is no ambiguity in the contract about what step 9 must do. **Robust.**

### Step 7 itself, given preconditions

The VPL function body has been verified to be:
- Self-contained scope (single function body in `Terrain::geometry`).
- Two-branch (fast path / legacy fallback) gated by `s_vpFast`.
- Bookended by a Tracy zone `"Terrain::geometry vertexProjectLoop"` (so post-delete Tracy zone disappearance is a clean PASS gate).
- No early returns, no thrown exceptions, no global heartbeat / counter increments visible in the code grep.

I stress-tested the seed concern "are there counter increments / debug logging other code uses as a heartbeat?" Grep'd `s_vp` and `MC2_*_STAT` macro patterns in `terrain.cpp:1370-1820`. Found `s_vpFrame`, `s_vpVertsChecked`, `s_vpVertsMismatch` — all self-contained to the VPL parity infra and die with VPL deletion. No external counter / heartbeat dependency. **The body itself is delete-safe once the preconditions land.**

### Path B (GPU port + readback) rejection rationale

The plan rejects Path B citing `memory/substrate_coalesce_sync_point_lesson.md`. Cross-checked: that memory file explicitly identifies a `glGetBufferSubData` readback as a 2x perf regression. A GPU-port-of-VPL approach would either (a) need readback for the cull-cascade CPU consumers — reintroducing the exact same class of stall, or (b) require porting the consumers too — which is what Path A is doing anyway. **Plan-author's Path B rejection is correct.**

### `vertexNum` carve-out

The plan flags `vertexNum` retirement as out-of-scope ("Init-time only at mapdata.cpp:1114-1119; permanent state, not per-frame transient"). Cross-checked: `vertexNum` is read in VPL but written once at init. **Correct to scope out.**

---

## §E Cross-plan integration risk

The CPU-pack-retirement and cmd-patch-retirement plan docs are referenced by this plan but were not on disk under `docs/superpowers/plans/` at review time (grep'd `docs/superpowers/plans/*.md` — only this plan and prior-arc plans). **This section is conditional on those plan docs landing.**

| This plan step | Sibling slice (assumed) | Risk if siblings land out-of-sync |
|---|---|---|
| Step 1 (assumes CPU pack retire shipped) | CPU pack retirement plan doc | If sibling slips, VPL's `pz` writes still have consumer at `quad.cpp:2089-2094`. Step 7 cannot land. |
| Step 2 (assumes cmd-patch retire shipped) | Cmd-patch retirement plan doc | If sibling slips, second `glDispatchCompute` and bucket header still consume `cmd.count` from CPU path. Step 7's correctness unaffected (cmd-patch is orthogonal to VPL) but the Fix-A retirement (step 8) is at risk because cmd-patch retire reshapes the dispatch flow. |
| Step 3 (picking repoint) | Independent | None. |
| Step 5 (cull-cascade audit) | Track D (GPU mech batcher) per `render-perf-snapshot.md:40` | Track D is "PARITY SIGN-OFF" — independent. But if Track D ships during VPL's soak, it shifts the bucket map and the `vertexProjectLoop` ~475 µs measurement may need re-baselining. **Low risk.** |
| Step 6 (reduction audit) | Per CRIT-4, likely a no-op | None. |
| Step 7 (VPL delete) | Depends on 1-6 | Aforementioned. |
| Step 8 (Fix A retire) | Cmd-patch retire + step 5 | Per HIGH-1, this needs explicit gating. |

**Recommendation:** plan v2 should declare an explicit "preconditions section" listing the sibling plan docs by filename and the commit SHAs that must be merged before each of this plan's steps. If a sibling plan is not yet drafted, the dependent step is "BLOCKED — awaiting sibling plan v1 draft."

---

## §F Recommended ordering changes

1. Promote step 6 (reduction audit) to step 0 — it's a 5-minute grep and likely retires the reductions as part of the VPL delete commit (per CRIT-4). Doing it early eliminates a "decision point" that doesn't exist.
2. Add an explicit step "0.5: `hazeFactor` GPU consumer port" before step 1 — CRIT-1 makes this load-bearing for step 7.
3. Keep step 1 (CPU pack) before step 2 (cmd-patch) — preserves the natural data-flow simplification ordering and removes Fix-B's CPU mirror before the dispatch infra is reshaped.
4. Move step 3 (mouse picking) to AFTER step 5 (cull-cascade audit) OR rescope per CRIT-2 — currently step 3 is positioned as a low-risk repoint but the analysis above shows it has hidden gameplay-correctness debt.
5. Defer step 8 (Fix A retire) to a separate plan, post-step-7 soak. (HIGH-1.)
6. Step 9 (contract refresh) immediately after step 7 — already correct, no change.

The 9-step sequence becomes 8 steps in this plan + 1 deferred follow-up plan. Cumulative correctness risk drops.

---

## Notes on review limits

- I did not invoke `mc2-terrain-indirect-expert` / `mc2-cpu-gpu-offload-expert` / `mc2-render-expert` advisors for this review — the worktree's advisor-invocation discipline (CLAUDE.md "Advisor invocation discipline") would call for that on substantive domain work. If the orchestrator wants a second-pass opinion on CRIT-3 (legacy `MC2_TERRAIN_INDIRECT=0` fallback semantics) and HIGH-3 (Path C cull-cascade stale-reset), those two advisors are the right consults.
- I treated the absence of `docs/superpowers/plans/cpu-pack-retirement-*.md` and `docs/superpowers/plans/cmd-patch-retirement-*.md` on disk as "not yet drafted" rather than searching exhaustively for alternate names. **Uncertain — needs orchestrator confirmation** that those sibling plans exist or are queued.
- The `.codex_tmp_isolate/quad_head.cpp` and `quad_desired.cpp` files in the worktree contain pre-/post-state snapshots of `quad.cpp` from a prior session; I deliberately did not cite them as authority because they are scratch artifacts. All cites are against `mclib/quad.cpp` proper.
