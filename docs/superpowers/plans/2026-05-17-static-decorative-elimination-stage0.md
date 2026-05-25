# Static Decorative Elimination - Stage 0 Resolution & Boundary-Proof Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve the five plan-resolution blockers and produce the `objList`
consumer boundary proof for the static-decorative-elimination spec, so a
faithful Stages 1-6 implementation plan can then be written.

**Architecture:** Stage 0 is investigation + decision, not code. Each task is
a grep-grounded analysis producing a recorded decision with a binary exit
criterion. The aggregate output (BOUNDARY-PROOF.md + RESOLUTION.md +
design-delta) goes through the first adversarial-review gate (design-delta,
opus). Stages 1-6 are out of scope for this plan and are contingent on these
outputs.

**Tech Stack:** C++ OpenGL engine (MC2), branch `claude/nifty-mendeleev`,
worktree `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`. Spec:
`docs/superpowers/specs/2026-05-17-static-decorative-elimination-design.md`
(commit `a971572`).

**Cross-branch facts established at plan-write (2026-05-17):**
- `substrate_writeRecord` does NOT exist on disk in
  `.claude/worktrees/gpu-driven-rendering/GameOS/gameos/gpu_cull_substrate.cpp`
  -- it is a proposed helper in the sibling design only. The
  single-choke-point obligation depends on an unbuilt interface.
- No static-world-fixed-shadow-map implementation exists in
  `GameOS/gameos/` -- it is a "design ready" known-issue, not code.
- `objList` is populated at `code/objmgr.cpp:504-631`; `terrainObjects[i]`
  enters at `:553`. Typed accessors begin `objmgr.cpp:797`.
- `ObjBlockInfo` (`mclib/terrain.h:106-107,179`): `numObjects` includes
  collidables; `firstHandle` = "collidables, followed by non"; collidables
  are a prefix of each block's handle range; `objBlockInfo` is a dynamically
  allocated array.

All `(A)`-marked spec anchors and any line cited below MUST be re-grepped at
task execution time (symbols stable, lines drift). No emoji. No wall-clock
projections.

## Hard constraints (from Task 1 boundary proof + user rulings 2026-05-17)

These override any conflicting candidate mechanism in the spec/plan:

- **HC-1 Save games are descoped, not patched.** The on-disk PacketFile
  save-game format, `objList` slot/index identity, the `watchSave`
  index map (`code/objmgr.cpp:3257-3283`, re-grep), and decorative
  destroyed/fallen-state persistence via the existing slotted
  `TerrainObject::Save` MUST remain byte-unchanged. This slice does NOT
  modify save/load. Any mechanism that compacts, reorders, or removes
  `objList` slots is forbidden.
- **HC-2 Sever at `objBlockInfo`, not `objList`.** Decoratives remain
  present in `objList` and the typed `terrainObjects[]` array (mission
  lifetime, static, cheap). Severance removes them only from the
  per-frame `objBlockInfo` block ranges (`firstHandle`/`numObjects`)
  that the per-frame walks iterate. This is what reconciles HC-1 with
  `seen -> 0`.
- **HC-3 Elimination = per-frame cost, not existence.** The decorative
  object still exists and is still resolvable on demand by infrequent
  consumers (ABL `getTerrainObject(i)` typed-array path
  `ablmc2.cpp:1469`; on-hit collision). These are not per-frame and are
  acceptable as-is. The boundary-proof Blockers #2/#3/#4 are addressed
  by "the object still exists, just not per-frame-iterated", NOT by
  deleting it from existence.
- **HC-4 Per-frame sites in scope are all three `objBlockInfo`-driven
  walks:** `objmgr.cpp:1706` render, `:1837` renderShadows, `:2017`
  update (all re-grep), plus the mech/vehicle/artillery/carnage
  collision block-walk (Blocker #2 sites). All must reach `seen -> 0`
  for decoratives via the single HC-2 `objBlockInfo` severance.

---

### Task 1: objList consumer boundary proof (Stage 0 hard gate, first deliverable)

**Files:**
- Create: `docs/superpowers/specs/2026-05-17-static-decorative-BOUNDARY-PROOF.md`

For each of the 13 dangerous consumer classes in spec Section 6, run the
opposite-direction grep (grep the consumer, not the obvious name, per
`feedback_data_flow_audit_asymmetry`), classify whether it can resolve a
decorative after severance, and record the verdict + the rule that keeps it
clean.

- [ ] **Step 1: Enumerate the typed/handle resolution surface**

Run:
```
rg -n "GameObjectManager::get|getObjectFromHandle|getObject\(|objList\[|getTerrainObject|getByHandle" code/objmgr.cpp code/objmgr.h
```
Record every resolver that can return a `TerrainObject*` (decorative) by
index/handle. Expected: `getTerrainObject` (~`objmgr.cpp:797`) and the
`objList[...]` build/scan sites (`:504-631`, `:2004-2036`).

- [ ] **Step 2: Per-class opposite-direction grep (13 classes)**

For each class run the listed grep against the worktree root and record
hits + a CAN-REACH / CANNOT-REACH verdict with file:line evidence:

1. render/update: `rg -n "objList\[|->update\(\)|->render\(" code/objmgr.cpp code/gamecam.cpp`
2. collision: `rg -n "collid|OBB|lineOfFire|getCollision|terrainBlock.*Object" mclib/*.cpp code/*.cpp`
3. damage: `rg -n "applyDamage|handleWeaponHit|takeDamage|destroy\(" code/terrobj.cpp mclib/objappear*.cpp`
4. script triggers: `rg -n "getObject|ObjectHandle|partId|getPart" code/abl/*.cpp code/mission.cpp`
5. mission objectives: `rg -n "objective|trigger|getObject" code/mission*.cpp`
6. save/load: `rg -n "save\(|load\(|Chunk|serialize" code/objmgr.cpp code/terrobj.cpp`
7. selection/targeting: `rg -n "select|target|pickObject|getTarget" code/*.cpp`
8. pathing/blocking: `rg -n "blocked|passable|getMoveCost|setBlocked" mclib/move*.cpp code/*.cpp`
9. fog/LOS/reveal: `rg -n "LOS|lineOfSight|reveal|fogOfWar|scenario" code/*.cpp mclib/*.cpp`
10. audio/event emitters: `rg -n "soundSystem|playSound|event.*Object" code/*.cpp`
11. cleanup/destruction: `rg -n "MC2_DESTROY|setExists|removeObject|freeHandle" code/objmgr.cpp code/terrobj.cpp`
12. network/replay: `rg -n "replay|network|serializeFrame|lockstep" code/*.cpp` (expected: none; record absence)
13. handle-to-object helpers: `rg -n "getObjectFromHandle|handleToObject|resolveHandle|getByPartId" code/*.cpp mclib/*.cpp`

- [ ] **Step 3: Write BOUNDARY-PROOF.md**

For each class: the grep run, hits, verdict, and the keep-clean rule (either
"never reaches decoratives by construction post-severance" with evidence, or
"reaches -> requires the Blocker-2 handle rule" naming the exact callsite).
Any CAN-REACH with no covering rule is a HARD BLOCKER recorded as OPEN.

- [ ] **Step 4: Exit criterion**

Exit PASS only if every one of the 13 classes is either CANNOT-REACH (with
opposite-direction grep evidence) or CAN-REACH-but-covered by a named rule
carried into Blocker 2. Otherwise the proof is INCOMPLETE and Stages 1-6
remain blocked.

- [ ] **Step 5: Commit**

```
git add docs/superpowers/specs/2026-05-17-static-decorative-BOUNDARY-PROOF.md
git commit -m "stage0(static-decor): objList consumer boundary proof (13 classes)"
```

---

### Task 2: Blocker 1 - objBlockInfo severance mechanism (HC-1..HC-4)

**Files:**
- Create: `docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md` (section: Blocker 1)
- Investigate: `code/objmgr.cpp` (objList build ~:504-631, save ~:3257-3283, per-frame walks :1706/:1837/:2017 -- re-grep all); `mclib/terrain.h:106-107,179`; `mclib/terrain.cpp` (objBlockInfo population); `code/ablmc2.cpp:1469` (typed-array path)

The mechanism is fixed by HC-2 (sever at `objBlockInfo`, leave `objList`
and `terrainObjects[]` intact). This task does not choose between candidate
mechanisms; it PROVES the HC-2 mechanism satisfies HC-1 and HC-4 and
records the exact edit sites.

- [ ] **Step 1: Map objBlockInfo construction and every block-range consumer**

Run:
```
rg -n "objBlockInfo|firstHandle|numObjects|numObjBlocks|getObjBlock" mclib/terrain.cpp code/objmgr.cpp code/mech.cpp code/gvehicl.cpp code/artlry.cpp code/carnage.cpp
```
Record who writes the `objBlockInfo` ranges and EVERY site that iterates
`[firstHandle, firstHandle+numObjects)` or uses `getObjBlock*`. Tag each as
per-frame (in scope, must reach `seen -> 0`) vs non-per-frame.

- [ ] **Step 2: Prove HC-1 (save untouched) by construction**

Re-grep `GameObjectManager::Save`/`Load` and the `watchSave` loop
(`objmgr.cpp` ~:3246-3299). Record the proof that an `objBlockInfo`-only
severance does not touch `objList[i]`, the packet/slot numbering, the
`watchSave` index map, or `TerrainObject::Save`. Exit sub-criterion: a
written argument that the save path never reads `objBlockInfo` for slot
identity (grep `objBlockInfo` inside Save/Load -- expect zero).

- [ ] **Step 3: Prove HC-4 (all per-frame sites covered by one severance)**

For each of `objmgr.cpp:1706` (render), `:1837` (renderShadows), `:2017`
(update), and the Blocker #2 collision block-walk
(`mech.cpp` handleStaticCollision, `gvehicl.cpp`, `artlry.cpp`,
`carnage.cpp` -- re-grep), confirm it iterates via `objBlockInfo`
ranges/`getObjBlock*` and therefore a single `objBlockInfo` decorative
exclusion drives `seen -> 0` for all of them. Record any per-frame site
that does NOT route through `objBlockInfo` (would be a new blocker).

- [ ] **Step 4: Record the exact edit site + collidable-prefix coherence**

Identify where `objBlockInfo` block membership/ordering is built
(collidable prefix per terrain.h:107) and the exact point a decorative
would be excluded from the block range while remaining in `objList`.
Record how `firstHandle`/`numObjects` and the collidable prefix stay
coherent for surviving non-decorative objects in the same block (this is
the load-bearing coherence proof).

- [ ] **Step 5: Record the typed-array (HC-3) disposition**

State explicitly: `terrainObjects[]` and the `objList` slot are NOT
trimmed; ABL `getTerrainObject(i)` (`ablmc2.cpp:1469`, re-grep) and on-hit
collision still resolve the object on demand; this is acceptable because
it is not per-frame. No code change required for the typed-array path;
record why.

- [ ] **Step 6: Exit criterion + commit**

Exit PASS only if Steps 2-4 each have a written, grep-grounded proof and
no per-frame site escapes the single `objBlockInfo` severance. If any
per-frame site is not `objBlockInfo`-routed, record it as a new OPEN
blocker (do not fabricate coverage).
```
git add docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md
git commit -m "stage0(static-decor): resolve Blocker 1 objBlockInfo severance (HC-1..HC-4)"
```

---

### Task 3: Blocker 2 - generic handle-lookup rule

**Files:**
- Modify: `docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md` (section: Blocker 2)

- [ ] **Step 1: Inventory every generic resolver that can return a decorative**

Run:
```
rg -n "getTerrainObject|getObject|objList\[|getByHandle|getObjectFromHandle" code/objmgr.cpp code/objmgr.h
```
Cross-reference Task 1 Step 1 output. List each resolver callsite that, post
severance, could be asked for a severed decorative handle.

- [ ] **Step 2: Specify the rule**

Write the rule verbatim into RESOLUTION.md: "Generic object lookup MUST NOT
silently materialize or return a severed decorative. It returns
not-object-resident; approved systems route through StaticDecorativeSet /
collision-proxy APIs." For each resolver from Step 1, state how it complies
(returns null/not-resident) and which callers must be updated.

- [ ] **Step 3: Exit criterion + commit**

Exit PASS only if every resolver from Step 1 has a stated compliant
behavior and every CAN-REACH consumer from Task 1 maps to this rule.
```
git add docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md
git commit -m "stage0(static-decor): resolve Blocker 2 handle-lookup rule"
```

---

### Task 4: Blocker 3 - collision proxy ownership

**Files:**
- Modify: `docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md` (section: Blocker 3)

- [ ] **Step 1: Trace the terrain-object spatial/collision index**

Run:
```
rg -n "objBlockInfo|collid|firstHandle|getObjBlock|terrainBlock" mclib/terrain.cpp code/objmgr.cpp code/terrobj.cpp
```
Determine whether the existing collidable-prefix index can answer a tree hit
WITHOUT routing back through `objList` (the Blocker-2 forbidden path).

- [ ] **Step 2: Decide ownership**

If and only if the existing index provably does not route through `objList`,
reuse it; record the non-routing proof. Otherwise specify a dedicated
decorative collision proxy keyed by `handle/generation/source-block/AABB`,
populated at mission load from the same data as the instance bake. Record
the decision, the data source, and the deregister hook.

- [ ] **Step 3: Exit criterion + commit**

Exit PASS only if the chosen owner has a written non-routing proof (reuse)
or a complete proxy spec (dedicated). Tie the deregister hook to the
collision/damage callbacks (`code/terrobj.cpp` fall path; re-grep the
`OBJECT_FLAG_FALLING` set site -- spec cites `terrobj.cpp:352-353 (A)`).
```
git add docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md
git commit -m "stage0(static-decor): resolve Blocker 3 collision proxy ownership"
```

---

### Task 5: Blocker 4 - static-shadow lifecycle (+ unbuilt-infra surfacing)

**Files:**
- Modify: `docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md` (section: Blocker 4)

- [ ] **Step 1: Confirm the static shadow map state**

Run:
```
rg -n "staticShadow|StaticShadow|worldFixedShadow|shadowMap" GameOS/gameos/gos_postprocess.cpp GameOS/gameos/gameos_graphics.cpp
rg -n "static.*shadow|shadow.*static" docs/ memory/ 2>/dev/null
```
Record whether the static world-fixed shadow map exists or is design-only
(plan-write finding: not implemented in `GameOS/gameos/`). If design-only,
flag a HARD DEPENDENCY: this slice's shadow bake requires that
infrastructure to exist first or to be in-scope.

- [ ] **Step 2: Decide lifecycle + destruction semantics**

Record explicit answers: static shadow map allocation time; decorative-bake
order vs terrain/building static bake; dynamic-caster compositing order;
mission-reload behavior; and the destruction rule -- a fallen/destroyed
decorative's baked static shadow either (a) persists for the mission
(explicitly accepted) or (b) is removed/patched on deregister. Pick one and
justify.

- [ ] **Step 3: Exit criterion + commit**

Exit PASS only if the static-shadow-map dependency status is recorded with a
go/no-go (build-first vs in-scope vs blocked) and all five lifecycle
questions answered with no TBD.
```
git add docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md
git commit -m "stage0(static-decor): resolve Blocker 4 static-shadow lifecycle + dependency"
```

---

### Task 6: Blocker 5 - same-frame deregister ordering

**Files:**
- Modify: `docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md` (section: Blocker 5)

- [ ] **Step 1: Establish the per-frame cull/draw order for the set**

Run:
```
rg -n "frameBegin|compute_dispatch|MultiDrawElementsIndirect|GpuStaticProp|markVisible" code/gamecam.cpp mclib/txmmgr.cpp GameOS/gameos/gos_static_prop_registry.cpp
```
Record the exact frame sequence: registry frameBegin (spec cites
`gamecam.cpp:201 (A)`) -> compute cull -> indirect draw (spec cites
`txmmgr.cpp` flush region; re-grep), relative to where collision/damage
callbacks fire in the frame.

- [ ] **Step 2: Define the no-double-tree sequence**

Write the exact ordered sequence into RESOLUTION.md: hit detected ->
tombstone write -> dynamic object spawn/re-admit -> compute cull observes
tombstone (or the prior-frame indirect command is invalidated) -> draw
shows exactly one representation. State which step guarantees the GPU does
not draw the static instance the same frame the dynamic replacement appears,
grounded in the Step 1 ordering.

- [ ] **Step 3: Exit criterion + commit**

Exit PASS only if the sequence has a written guarantee that, for any frame
where the hit lands, the draw shows exactly one of {static instance,
dynamic re-admit}, never both and never neither.
```
git add docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md
git commit -m "stage0(static-decor): resolve Blocker 5 same-frame deregister ordering"
```

---

### Task 7: Canonical parity-record layout + sibling-helper coordination

**Files:**
- Modify: `docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md` (section: Parity record + sibling coordination)

- [ ] **Step 1: Define the canonical packed parity record**

Specify the exact field list, byte widths, field order, and zeroed padding
for the parity record (spec Section 8: matrix / fog / highlight /
lightDataIndex + per-leaf world AABB). If any part is GPU-read, mark it for
shared C++/GLSL per `cpp_glsl_ubo_struct_lockstep`. No raw engine struct
compared; every source field initialized before compare.

- [ ] **Step 2: Coordinate the unbuilt sibling choke point**

Run (cross-branch):
```
rg -n "substrate_writeRecord|s_cpuVisibleCount" .claude/worktrees/gpu-driven-rendering/GameOS/gameos/gpu_cull_substrate.cpp
```
Plan-write finding: `substrate_writeRecord` is unbuilt (sibling design only).
Record the coordination requirement: Plan 2 (implementation) of THIS slice
must not route the fallback/legacy decorative path until the sibling helper
exists or its signature is frozen. Record the exact sibling spec section
reference
(`.claude/worktrees/gpu-driven-rendering/docs/superpowers/specs/2026-05-17-substrate-cpuvisible-writeside-accumulation-design.md`,
Counter-semantics contract) as the interface authority.

- [ ] **Step 3: Exit criterion + commit**

Exit PASS only if the parity record is fully specified (no TBD) and the
sibling-helper dependency is recorded as a Plan 2 sequencing precondition.
```
git add docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md
git commit -m "stage0(static-decor): canonical parity record + sibling-helper coordination"
```

---

### Task 8: Design-delta adversarial review (first review gate)

**Files:**
- Create: `docs/superpowers/specs/2026-05-17-static-decorative-DESIGN-DELTA-REVIEW.md`

- [ ] **Step 1: Assemble the design delta**

Summarize, in RESOLUTION.md, the net design changes the 5 resolutions +
boundary proof impose on the spec (the "design delta").

- [ ] **Step 2: Dispatch the adversarial review (opus)**

Dispatch a review subagent with model opus. The dispatch prompt MUST include
verbatim: "use the adversarial-plan-review skill". Scope: BOUNDARY-PROOF.md +
RESOLUTION.md against the spec and the cited code. Require CRITICAL / MAJOR /
MINOR findings with grep-verified file:line.

- [ ] **Step 3: Incorporate and re-gate**

Fix every CRITICAL and MAJOR inline in RESOLUTION.md / BOUNDARY-PROOF.md. If
any CRITICAL was found, re-dispatch one more round with the alternate model
(sonnet). Record the review outcome in DESIGN-DELTA-REVIEW.md.

- [ ] **Step 4: Exit criterion + commit**

Exit PASS only when the latest review round returns zero CRITICAL and zero
MAJOR. This is the Stage 0 completion gate; the implementation gate (second
adversarial gate, alternate model) belongs to Plan 2.
```
git add docs/superpowers/specs/2026-05-17-static-decorative-DESIGN-DELTA-REVIEW.md docs/superpowers/specs/2026-05-17-static-decorative-RESOLUTION.md docs/superpowers/specs/2026-05-17-static-decorative-BOUNDARY-PROOF.md
git commit -m "stage0(static-decor): design-delta adversarial review clean"
```

---

## Self-Review

- **Spec coverage:** Section 6 (boundary proof) -> Task 1. Section 10
  Blockers 1-5 -> Tasks 2-6. Section 8 parity record + Section 11
  single-choke-point -> Task 7. Section 9 first review gate / soak-substitute
  -> Task 8. Stages 1-6 explicitly deferred to Plan 2 (contingent).
- **Placeholder scan:** no TBD/TODO; every task has exact greps, binary exit
  criteria, and a commit. The two unbuilt dependencies (substrate_writeRecord,
  static shadow map) are surfaced as recorded findings, not placeholders.
- **Consistency:** RESOLUTION.md is the single decision doc across Tasks 2-7;
  BOUNDARY-PROOF.md is Task 1's; DESIGN-DELTA-REVIEW.md is Task 8's. Names
  consistent across tasks.

## Next plan (out of scope here)

Plan 2 (Stages 1-6 implementation) is written ONLY after this plan completes
(Task 8 clean). It is contingent on the 5 resolutions, and has a hard
sequencing precondition on the sibling `substrate_writeRecord` existing /
signature-frozen and the static shadow map dependency go/no-go.
