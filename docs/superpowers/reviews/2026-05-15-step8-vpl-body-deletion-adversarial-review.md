# Adversarial Review — Step 8 (8a/8b/8c) VPL-Body-Deletion Design

**Verdict: BLOCK**

**CRITICAL: 3   MAJOR: 4   MINOR: 2**

**slim⊇legacy proof (finding 1): CONDITIONALLY PROVEN — equal IFF the cull writes are placed BEFORE the slim loop's `terrain.cpp:1993 if (!clipR || !inViewR) continue;` and gated on `clipR` only; REFUTED (strict subset, objects vanish) if placed after that `continue` or gated on `inViewR`. The design text as written ("derived from its already-present onScreenR/clipR decision") is ambiguous on placement and the slim loop's current control flow forces the REFUTED branch unless restructured. The design is NOT safe as specified.**

Worktree: `A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/`, branch `claude/gpu-driven-rendering`, HEAD `40aec33`. Every citation grep-verified at HEAD `40aec33`.

---

## §0 Plan-of-record vs design-under-review divergence (CRITICAL framing issue)

Before the per-finding analysis: the design described in the review prompt does **not match the plan-of-record** at `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md` (HEAD `40aec33`), and does not match the code.

- Plan Step 8a (`plan:437`) states the post-8c cull source is "the **Step 5 slim pass**". **There is no Step 5 slim cull pass in the code.** Step 5 shipped instrumentation-only (the `MC2_VPL_CULL` probe) per the v3.2 correction (`plan:163-242`). Grep `terrain.cpp`: the cull cascade (`setObjBlockActive`/`setObjVertexActive`) is produced ONLY by the **VPL body** at `terrain.cpp:1790-1793`, gated `if (currentVertex->clipInfo)` where `clipInfo = onScreen` (`:1759`).
- The Step 6 slim reduce loop (`terrain.cpp:1923-2018`) is **already landed** and produces ONLY `leastZ/mostZ/leastW/mostW/leastWY/mostWY` (`:1996-2016`). It writes **no** `clipInfo`/`objBlockInfo`/`objVertexActive`.
- The prompt's "8c-part-1 cull-merge" (extend the Step 6 slim loop with cull writes) is a **new design not reflected anywhere in the plan document**. Plan Step 8a/8c text still describes a "Step 5 slim pass" cull producer that does not exist.

**CRITICAL-0.** The plan-of-record and the design-under-review disagree on the single most load-bearing fact in Step 8 (who produces the cull cascade post-8c). Plan Step 8a (`plan:437`), Step 8c carve-out (`plan:443`), and the consolidated gate (`plan:455-461`) must be rewritten to describe the actual mechanism (extend the Step 6 slim reduce loop with cull writes) before 8c executes. An executor following the plan text literally would look for a non-existent "Step 5 slim pass". Fix: amend the plan to v3.5 documenting the cull-merge-into-slim-reduce design, with the placement constraint from finding 1 as a load-bearing execution rule.

---

## §1 CRITICAL — slim⊇legacy: the catastrophic axis

### The two predicates, grep-traced

**Legacy VPL cull-write active-set** (the set that gets `setObjBlockActive`/`setObjVertexActive`):
- `terrain.cpp:1790`: `if (currentVertex->clipInfo)` → `:1792-1793` writes.
- `terrain.cpp:1757-1762`: stock branch `if (eye->usePerspective && Environment.Renderer != 3) currentVertex->clipInfo = onScreen;` else `= inView`.
- `Environment.Renderer` is assigned `0` and only `0` (`code/logmain.cpp:786`, `code/mechcmd2.cpp:2815`; no other assignment — opposite-direction grep clean). The `!= 3` branch is **always** taken in stock/smoke. `inView` never feeds `clipInfo` in stock.
- **Legacy active-set = { vertex : onScreen == true }.**
- `onScreen` (`terrain.cpp:1643-1725`): a deliberately LOOSE contract — `CLIP_THRESHOLD_DISTANCE=768` (`:1362`) proximity admit, `VERTEX_EXTENT_RADIUS=384` (`:1365`) dilated-cone (`:1674-1688`), and the `!IsGameSelectTerrainPosition` out-of-play **force-readmit** `onScreen = true` (`:1709-1714`). Matches the v3.2 loose-contract characterization (`plan:194-204`).

**`projectForTerrainAdmission` (= `inView` / `inViewR`)**, grep-traced:
- `camera.h:526-539`: returns `projectZ(point, screen)`.
- `camera.h:436-516` `projectZ`: returns `FALSE` when the projected screen point is **outside the screen rectangle**: `if ((screen.x < 0) || (screen.y < 0) || (screen.x > screenResolution.x) || (screen.y > screenResolution.y)) ... return FALSE;` (`:472-495`). Returns `TRUE` only for strict viewport-rect containment (`:515`).
- Therefore `inView` ⊊ `onScreen`: `inView` is strict screen-rect containment; `onScreen` is the loose proximity/dilated-cone/force-readmit superset. The legacy loop itself proves they diverge — it computes `bool inView=false` (`:1727`), projects (`:1734`), and the off-screen `else` (`:1746-1752`) shows `onScreen==true` vertices routinely have `inView==false` (near-edge, dilated-cone, and ALL `!IsGameSelectTerrainPosition` force-admits, which project off-rect by construction).

**Slim loop control flow** (`terrain.cpp:1923-2018`), grep-traced:
- `:1928-1982` computes `onScreenR`. **`onScreenR == onScreen` vertex-for-vertex**: the slim block is byte-identical to the legacy `eye->usePerspective` onScreen block MINUS the `hazeFactor` writes (which do not affect the onScreen decision). The legacy `else { hazeFactor=1.0 }` (`:1716-1719`) does not touch `onScreen`; the slim loop has no else (equivalent). The legacy `if(!isVisible){hazeFactor=1; onScreen=true}` (`:1709-1714`) force-trues onScreen; the slim `if(!isVisible){onScreenR=true}` (`:1972-1976`) is the same force-true (no-op inside that already-true block). Confirmed equivalent.
- `:1984` `if (!onScreenR) continue;`
- `:1989` `bool inViewR = eye->projectForTerrainAdmission(vertex3D,sp);`
- `:1991` `bool clipR = (eye->usePerspective && Environment.Renderer != 3) ? onScreenR : inViewR;` — **identical formula to legacy `clipInfo` (`:1757-1762`)**. In stock `clipR == onScreenR == onScreen`.
- `:1993` `if (!clipR || !inViewR) continue;` — discards every vertex where `inViewR == false`.
- `:1996-2016` reduction writes.

### The proof

The slim loop body **after `:1993`** is reached only for `{ onScreenR && clipR && inViewR }` = (stock) `{ onScreen && inView }`.

- **If the design places cull writes after `:1993`** (or anywhere gated on `inViewR`): slim active-set = `{ onScreen ∩ inView }` ⊊ `{ onScreen }` = legacy active-set. **REFUTED.** Every `onScreen==true, inView==false` vertex — near-screen-edge terrain, dilated-cone admits, and 100% of out-of-play `!IsGameSelectTerrainPosition` force-admits — is dropped from the cull cascade. The 9 `objmgr.cpp` consumers (`objmgr.cpp:1697/1704`, `:1828/1835`, `:2019/2027`, `:2610`, `:3060/3067`) gate object/mech iteration on `objBlockInfo[].active` AND `objVertexActive[vertexNum]`; a subset orphans them. This is the exact `cull_gates_are_load_bearing.md` cascade: objects/mechs on edge or partially-off-screen blocks vanish. Mechs are the canary (iterate last).
- **If the design places cull writes before `:1993`, after `:1984`, gated on `clipR` only** (not `inViewR`, not the `inViewR` arm of the `:1993` continue): slim active-set = `{ onScreenR }` = `{ onScreen }` = legacy active-set exactly. **PROVEN equal.** (`:1984 if(!onScreenR) continue` is harmless: `onScreenR==false` ⟹ legacy `clipInfo==false` ⟹ legacy writes nothing either.)

### The break scenario (concrete)

Stock `mc2_10`, oblique cinematic camera (`camera_model_oblique_cinematic.md`), camera tilted so map-edge terrain is in the loose 384u-dilated cone but its vertices project just outside `screenResolution.x` (right viewport edge). Legacy: `onScreen==true` (dilated cone) → `clipInfo==true` → block active → edge mechs/buildings render. Slim-with-writes-after-`:1993`: `inViewR==false` (projects off-rect) → `continue` before cull write → block inactive → **edge mechs/buildings vanish**. Worst case is exactly the user-positioned corner+zoom capture (`feedback_cost_split_worst_case_camera.md`); the default-camera smoke will likely NOT catch it (most verts project on-rect at default zoom), making this a partial-landing hazard that passes the gate.

### Required fix (load-bearing, must be in the plan)

The slim reduce loop must be restructured so the cull-cascade writes (`clipInfo`/`setObjBlockActive`/`setObjVertexActive`) are emitted on the `{ onScreenR }`/`{ clipR }` decision **before and independent of** the `inViewR`/`:1993` reduction-admission `continue`. Concretely: do NOT `continue` on `!onScreenR` before the cull write; compute `clipInfo = clipR` (== onScreenR in stock) and emit the cull writes when `clipR` is true; THEN apply the reduction's stricter `!clipR || !inViewR` gate only for the `leastZ/mostZ/...` accumulation. The cull write and the reduction gate must be decoupled — the reduction may legitimately use a tighter set, the cull MUST use the loose `{ onScreen }` set. The `MC2_VPL_CULL` probe (finding 6) must compare the slim-produced `clipInfo` against the still-live VPL `clipInfo` during the 8c-part-1 co-production window — not against the slim loop's own output.

---

## §2 MAJOR — co-production window ordering

`mission.cpp:501-502` `clearObjBlocksActive()` / `clearObjVerticesActive()` run, then `:505` single `land->geometry()` call (grep-verified `code/mission.cpp:495-505`). Within that one `geometry()` call: VPL body (`terrain.cpp:1638-1834`) runs first and emits true-only cull writes (`:1790-1793`), then the slim reduce loop (`:1923-2018`) runs. During 8c-part-1 both produce cull writes; both true-only; the frame-start clear is the sole reset; no intervening reader of `objBlockInfo`/`objVertexActive` exists between the two producers inside `geometry()` (the `objmgr.cpp` consumers run later in the frame, outside `geometry()`). **The union-superset co-production argument holds for the array writes** PROVIDED finding 1's placement fix is applied (so the slim writes are a superset, not a subset). If finding 1 is violated, co-production does NOT save it: union of {legacy full set} and {slim subset} is still the full set during 8c-part-1 (VPL still live), but the instant 8c-part-2 deletes the VPL body the set collapses to the slim subset and objects vanish — the co-production window MASKS the regression until the body is deleted in the same arc. This makes finding 1 a latent partial-landing trap: 8c-part-1 smoke passes (VPL still feeding the full set), 8c-part-2 regresses. **The plan must require that the slim-only active-set (VPL body force-disabled, e.g. behind the existing `s_vpFast`/parity scaffolding or a temporary probe) is validated to equal the legacy set BEFORE 8c-part-2 deletes the body** — co-production alone is not a sufficient gate.

## §3 MAJOR — 8b consumer completeness (third unretired reader)

Opposite-direction grep of `->px|->py|->pz|->pw` across all `mclib/*.cpp`, `code/*.cpp`, `GameOS/gameos/*.cpp` (clouds excluded — distinct struct, out of scope per `plan:66`):

- `quad.cpp:2107` `vertices[c]->pz` — LIVE, `fastPathEligible` block; **Step 1 not landed** (confirmed BLOCKER).
- `gos_terrain_indirect.cpp:1684` `q.vertices[c]->pz` — LIVE; **Step 1 not landed** (confirmed BLOCKER).
- `gos_terrain_mask_dispatch.cpp:210` — now a comment ("Was: per-corner q.vertices[c]->pz"); Step 4 effectively done. Not a blocker.
- `camera.cpp:586-587` `topVertex->px/py` in `Camera::getClosestVertex` (`camera.cpp:568`, decl `camera.h:661`) — **the third reader the advisor flagged as unresolved.** Grep-verified: `getClosestVertex` has **ZERO callers** anywhere in `mclib/`+`code/`+`GameOS/` (only the definition + the header decl). It is dead code. 8b deleting `px/py` writes is runtime-safe for it (never called), but the advisor's "unresolved BLOCKER" framing is correct in spirit: leaving a dead function that reads about-to-be-stale `px/py` is the additive-debt anti-pattern (`mc_texture_manager_dual_queue_legacy_retirement_debt.md`). **8b/8c must DELETE `Camera::getClosestVertex` (`camera.cpp:568-...` + `camera.h:661`) in the same arc**, not just leave it reading stale state.
- `camera.cpp:717/780` — comments only ("stale per-frame px/py"), Step 3 repoint already landed (`Camera::inverseProject` body `camera.cpp:768+` no longer reads vertex VPL fields — grep-verified). Not a blocker.
- `quad.cpp:2339-2865` (legacy SOLID/TES `gVertex`) and `quad.cpp:3667-3805` (debug-line draws) — class (a)/(d), unreachable under `MC2_TERRAIN_INDIRECT=1` default / not production. Per `plan:433` these are CPU-pack-plan scope, not this plan. Acceptable PROVIDED Step 1 lands first (gate condition 1).
- `quad.cpp:368-371` `pzv[i].wx/wy/wz = verts[i]->px/py/pz` — `projectz_overlay_record_tri` debug trace, class (d), env-gated (`g_pzTrace`, `:362 if(!g_pzTrace) return;`). Not production.

**Verdict:** the advisor's table is exhaustive for live PRODUCTION readers (the two Step-1 blockers are real and correctly identified). The `getClosestVertex` reader is correctly flagged but the design must escalate it from "tracked unresolved" to "delete in 8b/8c" — a dead function reading stale per-frame state is a latent re-activation hazard. MAJOR because the design's stated SURVIVORS/deletion list does not name `getClosestVertex` for deletion.

## §4 MAJOR — clipInfo survivors

8b/8c KEEP clipInfo writes per `plan:439`/`:443`. Grep of clipInfo readers:

- `quad.cpp:401-408` `isTerrainQuadVisible` (sums `quad.vertices[*]->clipInfo`); caller `quad.cpp:908` `quadVisible = isTerrainQuadVisible(*this)` inside `setupTextures`/the visibility path.
- `quad.cpp:733-734/817-818/997-1003` per-corner clip-decision sites; `quad.cpp:368` `pzv[i].legacyAccepted = (verts[i]->clipInfo != 0)` (debug).
- **Second clipInfo writer**: `quad.cpp:1035/1037` (corner 0), `:1102/1104` (c1), `:1169/1171` (c2), `:1236/1238` (c3), each `vertices[N]->clipInfo = clipData;` where `clipData = eye->projectForTerrainAdmission(vertex3D,screenPos)` (`:1026/1093/1160/1227`) with a `!IsGameSelectTerrainPosition → clipData=false` (`:1030-1034` etc.). This is a **projection-derived, water-elevation-adjusted, per-corner** value — semantically DIFFERENT from VPL's `onScreen`-derived `clipInfo`.

Ordering: VPL body (`terrain.cpp:1638-1834`) → slim reduce (`:1923-2018`) → `quadSetupTextures` loop calling `setupTextures()` (`terrain.cpp:2101-2131`), all inside one `geometry()` call. The `quad.cpp` second writer runs AFTER VPL every frame and **OVERWRITES** VPL's `clipInfo` for every quad whose `setupTextures` runs. Therefore:

1. `isTerrainQuadVisible` (`quad.cpp:908`) and the per-corner clip sites read the **`quad.cpp` projection-derived clipInfo**, NOT VPL's. Deleting/retiring VPL's `clipInfo` write in 8b/8c does **not** change what these readers see (they are downstream of the `quad.cpp` overwrite). PROVEN safe — but for a different reason than the design states. The design's claim "the slim loop's clipInfo value equals what those readers got from the VPL body pre-8c" is **false and irrelevant**: those readers never got VPL's clipInfo; they got `quad.cpp`'s. The plan must correct this rationale (it currently implies the slim loop must match VPL clipInfo for `isTerrainQuadVisible` — it does not, because `isTerrainQuadVisible` reads the post-`setupTextures` overwrite).
2. The ONLY consumer of VPL's `onScreen`-derived clipInfo (read before the `quad.cpp` overwrite) is the cull cascade at `terrain.cpp:1790-1793` itself, in the same loop. This is exactly finding 1's set. So finding 4 collapses into finding 1: the only thing that must reproduce VPL's `clipInfo`-as-`onScreen` is the cull-cascade write, and that is the §1 placement requirement.

MAJOR: the plan's clipInfo-survivor rationale is wrong (cites `isTerrainQuadVisible` as needing VPL-clipInfo parity; it reads the `quad.cpp` overwrite instead). Not a correctness break for `isTerrainQuadVisible` (it is safe), but the wrong rationale would let an executor "preserve VPL clipInfo for isTerrainQuadVisible" and miss that the real (and only) clipInfo invariant is the cull-cascade set in finding 1. Fix: rewrite `plan:439` clipInfo survivor analysis to state the sole VPL-clipInfo consumer is the in-loop cull write; `quad.cpp:1035..` overwrites clipInfo before any quad-visibility reader.

## §5 MAJOR — legacy-lighting death under-specified (parity-mode entry missed)

`quad.cpp:1319-1321`: `s_lightingGpuAuth = gos_terrain_lighting::IsEnabled() && !gos_terrain_lighting::IsParityCheckEnabled();` then `if (!s_lightingGpuAuth && terrainHandle != 0xffffffff)` reads `vertices[N]->hazeFactor` at `quad.cpp:1471/1473/1625/1627/1779/1781/1931/1933`.

- `IsEnabled()` (`gos_terrain_lighting.cpp:90-96`): false iff `MC2_TERRAIN_LIGHTING_GPU` set to `"0"`.
- `IsParityCheckEnabled()` (`:98-101`): true iff `MC2_TERRAIN_LIGHTING_PARITY` set (any value).
- `s_lightingGpuAuth` is false — i.e. the legacy CPU-lighting path that reads `hazeFactor` is **reachable** — under **EITHER** `MC2_TERRAIN_LIGHTING_GPU=0` **OR** `MC2_TERRAIN_LIGHTING_PARITY=<set>`.

Grep of ALL terrain `Vertex::hazeFactor` writers: ONLY `mclib/terrain.cpp` VPL fast (`:1598`, snapshot `:1593`) and legacy fallback (`:1695/:1699/:1703/:1712/:1718/:1723`). `mapdata.cpp` does NOT initialize `hazeFactor` (grep: no hits). The other `hazeFactor =` hits (`bdactor.cpp`, `clouds.cpp`, `crater.cpp`, `genactor.cpp`, `gvactor.cpp`, `mech3d.cpp`) are different structs, not terrain `Vertex`.

After 8c deletes the VPL `hazeFactor` writes (`plan:446-447`), the terrain `Vertex::hazeFactor` has **no writer**. With `MC2_TERRAIN_LIGHTING_GPU=0` OR `MC2_TERRAIN_LIGHTING_PARITY=1`, `quad.cpp:1471` reads uninitialized/stale `hazeFactor` → wrong/garbage fog. The plan's Step 8c (`plan:447`) acknowledges the `MC2_TERRAIN_LIGHTING_GPU=0` path must die in the 8c window, but its prose only names `MC2_TERRAIN_LIGHTING_GPU=0` — it **does not mention the `IsParityCheckEnabled()` entry** into the same `!s_lightingGpuAuth` branch. `stock_install_must_remain_playable.md`: the failure must DEGRADE, not render-wrong. Two MAJOR sub-issues:

(a) 8c's "remove/guard the legacy path" must cover BOTH `IsEnabled()==false` AND `IsParityCheckEnabled()==true`. If 8c only guards the `MC2_TERRAIN_LIGHTING_GPU=0` documented path and leaves the parity-mode entry, anyone running `MC2_TERRAIN_LIGHTING_PARITY=1` (the lighting parity probe — a debug aid the team uses) post-8c reads stale hazeFactor and sees wrong fog, silently. Fix: 8c must delete the entire `!s_lightingGpuAuth` `hazeFactor`-reading block (`quad.cpp:1321`-scoped reads at `:1471..:1933`) OR neutralize the `hazeFactor` reads to a constant, covering the parity-mode entry explicitly.

(b) The design summary's phrase "no surviving non-VPL hazeFactor source" is correct but the consequence is stronger than stated: there is no DEFENSIVE init either (`mapdata.cpp` never sets it). Recommend the keep-field-stop-reading lockstep (`plan:331`) additionally zero-initialize the terrain `Vertex::hazeFactor` at vertex construction (`mapdata.cpp` vertex init) so a stray surviving reader degrades to "no fog" (stock-playable) rather than garbage. This is the `stock_install_must_remain_playable` degradation requirement.

## §6 MAJOR — probe relocation is a tautology after 8c

`MC2_VPL_CULL` probe today (`terrain.cpp:1772-1788`): asserts `(DWORD)onScreen != currentVertex->clipInfo`, INSIDE the VPL body, where `clipInfo` was set from `onScreen` 13 lines earlier (`:1759`, stock branch). It is **already near-tautological in the stock branch** — `clipInfo == onScreen` by direct assignment; the only non-corruption way to differ is the dead `Renderer==3` branch. Its real value today is exactly as the comment says (`:1764-1771`): a tripwire for *future* hidden projection→clipInfo coupling, and a co-production cross-check.

The design relocates the probe to the slim loop after the VPL body dies (`plan:443`/`:461`, "the relocated `MC2_VPL_CULL`"). If relocated to assert `slim onScreenR == slim-written clipInfo`, that compares the slim loop's output to itself — a **pure tautology**, zero diagnostic value, at the exact step that most needs a real tripwire. A tautological probe that prints "0 violations" gives false confidence that finding 1's subset bug did not happen.

Fix: the surviving probe must assert a MEANINGFUL invariant. Two options: (i) during the 8c-part-1 co-production window, the probe compares the **slim-produced** cull `clipInfo`/active-set against the **still-live VPL-body** `clipInfo`/active-set per vertex, asserting slim ⊇ legacy (`!(legacyActive && !slimActive)`) — the exact §1 invariant, the only check that catches the subset bug; (ii) post-8c-part-2 (VPL body gone) the probe must be retired or demoted to `MC2_VPL_REDUCE`-style, because there is no longer an independent producer to compare against and a self-comparison is a tautology. The plan's "probe survives 8c as the tripwire" (`plan:461`) is only valid for variant (i) during co-production; it CANNOT be a meaningful tripwire after the VPL body is deleted. Plan must specify the probe asserts slim-vs-VPL during co-production and is explicitly retired/demoted at 8c-part-2, not "survives as tripwire" indefinitely.

## §7 MINOR

- **M1.** Plan Step 8c deletion list (`plan:441`/`:446`) cites line numbers at HEAD `55b1167`/`d520967`; HEAD is now `40aec33` and they have drifted (e.g. fast-path final `hazeFactor` write is `terrain.cpp:1598` not `:1573`/`:1556`; legacy fallback `:1695-1723` not `:1661-1692`). The plan already says "grep-verify at write-time" — acceptable, but the 8c executor checklist should pin a re-grep gate for every deletion range as the first 8c step.
- **M2.** `Camera::getClosestVertex` dead-code deletion (finding 3) and the §0 plan/code divergence both touch the same `mclib/camera.*` / plan-doc surface; recommend one v3.5 plan amendment commit lands the corrected cull-merge design + the explicit getClosestVertex/legacy-lighting deletion list together, so the executor works from a single coherent contract.

---

## §8 Confirmed-safe-after-scrutiny

- **`onScreenR == onScreen` byte-for-byte.** Stress-tested the slim loop's onScreen copy (`terrain.cpp:1928-1982`) against the legacy block (`:1643-1725`) branch by branch: same `usePerspective` gate, same `CLIP_THRESHOLD_DISTANCE`/`VERTEX_EXTENT_RADIUS` cone math, same `IsGameSelectTerrainPosition` force-readmit, the only deltas are the `hazeFactor` writes (do not feed the onScreen decision). Held. This is what makes finding 1's PROVEN branch achievable — the slim loop CAN reproduce `{ onScreen }` exactly, it just must emit the cull write at the right point.
- **`Environment.Renderer` is only ever 0.** Opposite-direction grep across `code/`+`mclib/`+`GameOS/`: two assignments, both `= 0` (`logmain.cpp:786`, `mechcmd2.cpp:2815`). The stock `clipInfo = onScreen` / `clipR = onScreenR` branch is unconditional in production; the projection-dependent `else` (`clipInfo = inView`) is dead. The §1 proof rests on this and it is solid.
- **Co-production array mechanics (finding 2).** `mission.cpp:501-502` clear → single `:505 geometry()`; VPL body and slim loop both inside it, true-only writes, no intervening `objBlockInfo`/`objVertexActive` reader. The union-superset is mechanically sound *for the array writes* — the hazard is purely the subset-vs-superset semantics of finding 1, not aliasing/ordering.
- **Step 3 (picking repoint) already landed.** `Camera::inverseProject` (`camera.cpp:768+`) is the recursion-free frustum-AABB + forward-projection refine path; reads no vertex VPL field. Gate condition 2's consumer (`inverseProject` clipInfo/pz/px/py) is genuinely retired. Only the Step-1 blockers and dead `getClosestVertex` remain.
- **`isTerrainQuadVisible` is safe under VPL clipInfo retirement** — but because `quad.cpp:1035..` overwrites clipInfo with its own projection-derived value before any quad-visibility reader, NOT because the slim loop matches VPL clipInfo (see finding 4; the plan's stated rationale is wrong but the outcome is safe).

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **Cull-merge design must be written into the plan (v3.5).** The prompt's "8c-part-1 cull-merge into the Step 6 slim reduce loop" is not in the plan-of-record; plan Step 8a/8c still describe a non-existent "Step 5 slim pass". Orchestrator + `mc2-terrain-indirect-expert` must amend the plan with the cull-merge design AND the finding-1 placement constraint as a load-bearing execution rule. 8c CANNOT execute against the current plan text.
2. **Finding 1 placement rule is the gate.** The slim loop's cull write must be on the `{ onScreenR/clipR }` decision BEFORE the `:1993 if(!clipR||!inViewR) continue;` reduction gate. This is a code-structure decision (decouple cull-write from reduction-admission inside one loop) that the execution session must implement exactly; the plan must specify it, not leave it to the executor.
3. **Slim-only validation before 8c-part-2.** Co-production masks a finding-1 subset bug until the VPL body is deleted. Decide the mechanism to validate the slim-only active-set == legacy active-set with the VPL body disabled (not just co-producing) before 8c-part-2 lands — worst-case camera (`feedback_cost_split_worst_case_camera.md`), not default-camera smoke.
4. **Legacy-lighting death must cover the parity-mode entry.** 8c must neutralize/delete the `quad.cpp:1321` `!s_lightingGpuAuth` hazeFactor reads for BOTH `MC2_TERRAIN_LIGHTING_GPU=0` and `MC2_TERRAIN_LIGHTING_PARITY=1`, plus zero-init terrain `Vertex::hazeFactor` for stock-degradation. Confirm with `mc2-shader-expert`.
5. **Probe semantics post-8c.** Decide: `MC2_VPL_CULL` asserts slim-vs-VPL during 8c-part-1 co-production (meaningful), then is retired/demoted at 8c-part-2 (no independent producer ⟹ self-comparison is a tautology). The plan's "survives 8c as the tripwire" is invalid past body deletion.
