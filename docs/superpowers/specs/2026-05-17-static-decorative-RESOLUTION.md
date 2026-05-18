# Static Decorative Elimination - Stage 0 Resolution

Date: 2026-05-17
Branch: claude/nifty-mendeleev
Stage: Stage 0 (pre-code investigation, no code changes)
Status: DONE_WITH_CONCERNS -- Blocker 1 PASS with one new OPEN BLOCKER recorded

All file:line citations were grep-verified at write time per Documentation Discipline.

---

## Blocker 1: objBlockInfo severance mechanism (HC-1..HC-4)

### Step 1: objBlockInfo construction sites and every block-range consumer

Grep run:
  rg -n "objBlockInfo|firstHandle|numObjects|numObjBlocks|getObjBlock"
     mclib/terrain.cpp code/objmgr.cpp code/mech.cpp code/gvehicl.cpp
     code/artlry.cpp code/carnage.cpp

#### Construction sites (mission load, non-per-frame)

1. `mclib/terrain.cpp:403` -- `numObjBlocks = blocksMapSide * blocksMapSide` (computed at terrain init)
2. `mclib/terrain.cpp:485-489` -- `objBlockInfo` array allocated and zeroed at terrain load
3. `code/objmgr.cpp:993-996` -- `countTerrainObjects`: assigns `firstHandle` contiguously per block
   (firstHandle[i] = firstHandle[i-1] + numObjects[i-1]), called once at mission load
4. `code/objmgr.cpp:1017,1021,1025,1035,1045` -- `countObject`: increments
   `numCollidableObjects` and `numObjects` per object class. TREE and TERRAINOBJECT
   are classified as collidable (lines 1017, 1045), making `numCollidableObjects` include
   decorative trees. Called once per terrain object at mission load.
5. `code/objmgr.cpp:3460,3463,3464,3499,3507,3510,3544,3547,3548,3582,3585,3586` --
   `GameObjectManager::Load` (savegame restore): rebuilds `objBlockInfo` in memory from
   saved `data.blockNumber` per object, replicating the same firstHandle/numObjects/
   numCollidableObjects layout as mission load. (See Step 2 for HC-1 analysis.)

#### getObjBlock* accessor implementations (code/objmgr.h:426-440)

  getObjBlockObject(blockNumber, objLocalIndex)
  -> returns objList[Terrain::objBlockInfo[blockNumber].firstHandle + objLocalIndex]

  getObjBlockNumObjects(blockNumber)
  -> returns Terrain::objBlockInfo[blockNumber].numObjects

  getObjBlockNumCollidables(blockNumber)
  -> returns Terrain::objBlockInfo[blockNumber].numCollidableObjects

  getObjBlockFirstHandle(blockNumber)
  -> returns Terrain::objBlockInfo[blockNumber].firstHandle

#### Block-range consumer inventory tagged per-frame vs non-per-frame

| Site | Function | Per-frame? | Field read | Notes |
|------|----------|-----------|-----------|-------|
| objmgr.cpp:1695,1699-1700 | `GameObjectManager::render` | YES (in scope) | numObjects, firstHandle | iterates ALL objects in active block |
| objmgr.cpp:1826,1830-1831 | `GameObjectManager::renderShadows` | YES (in scope) | numObjects, firstHandle | iterates ALL objects in active block |
| objmgr.cpp:2004,2009-2010 | `GameObjectManager::update` | YES (in scope) | numObjects, firstHandle | iterates ALL objects in active block |
| objmgr.cpp:3045,3049-3050 | `GameObjectManager::updateAppearancesOnly` | YES (in scope) | numObjects, firstHandle | called from mission.cpp:508 when paused |
| mech.cpp:1115-1116 | `BattleMech::handleStaticCollision` | YES (in scope) | numCollidableObjects, firstHandle | collidable prefix only; decoratives ARE in collidable prefix (see below) |
| gvehicl.cpp:800-801 | ground-vehicle static collision | YES (in scope) | numCollidableObjects, firstHandle | same pattern as mech |
| artlry.cpp:743,746 | artillery static collision | YES (in scope) | numObjects (ALL), getObjBlockObject | iterates full numObjects, not just collidables |
| carnage.cpp:533,536 | carnage static collision | YES (in scope) | numObjects (ALL), getObjBlockObject | iterates full numObjects, not just collidables |
| objmgr.cpp:2595,2599-2600 | `findTerrainObjectByMouse` | NOT per-frame | numObjects, firstHandle | mouse-click triggered only |
| objmgr.cpp:1085,1087-1088 | `loadTerrainObjects` | NOT per-frame | firstHandle, numCollidableObjects | mission-load only; builds sorted insertion order |

DECORATIVE COLLIDABLE CLASSIFICATION NOTE: `countObject` (objmgr.cpp:1015-1017) routes
TREE and TERRAINOBJECT to `numCollidableObjects++`. This means decorative trees appear in the
collidable prefix [firstHandle, firstHandle+numCollidableObjects). Mech and vehicle collision
walks (getObjBlockNumCollidables) DO reach decorative trees at present. Artillery and carnage
walks use getObjBlockNumObjects (full range) and also reach decoratives.

---

### Step 2: HC-1 proof -- objBlockInfo-only severance does not touch the save path

#### Sub-criterion: grep objBlockInfo INSIDE Save/Load

Grep: rg -n "objBlockInfo" code/objmgr.cpp (full file)

Result within `GameObjectManager::Save` (lines 3246-3289):
  ZERO hits. The Save function iterates `objList[0..getMaxObjects()]` directly (line 3257),
  calls `objList[i]->Save(file, packetNum)` per non-null slot (line 3261), and writes the
  `watchSave` index map keyed by slot index `i` (line 3272). It never reads or references
  `objBlockInfo`.

Result within `GameObjectManager::Load` (lines 3292-3896):
  PRESENT -- lines 3460, 3463, 3464, 3499, 3507, 3510, 3544, 3547, 3548, 3582, 3585, 3586.
  Load WRITES to objBlockInfo (rebuilds it in memory from data.blockNumber per object).

#### HC-1 coherence argument

HC-1 states the on-disk byte format is byte-unchanged. Specifically: objList slot identity,
watchSave index map, and TerrainObject::Save are untouched.

Save (disk writes): objBlockInfo is NEVER read. The bytes written to disk depend only on:
  - objList[i] pointer (slot identity, unchanged)
  - objList[i]->Save() call (per-object serialization, unchanged)
  - watchSave[j] = i (watchList-to-slot mapping, unchanged)

These three are pure functions of objList and watchList contents. An objBlockInfo-only
severance that DOES NOT modify objList, watchList, or slot assignments leaves all three
unchanged. HC-1 is satisfied for Save. The on-disk bytes are byte-identical before and after
a HC-2 severance.

Load (in-memory reconstruction): Load DOES rebuild objBlockInfo from saved data.blockNumber.
This means after a save-game Load, objBlockInfo is restored to the UNSEVERED state (full
numObjects/numCollidableObjects including decoratives). Therefore:

  ** OPEN BLOCKER OB-1: Severance must survive save-game restore. **
  An objBlockInfo severance applied at initial mission load would be UNDONE by
  GameObjectManager::Load (lines 3460-3586), which rebuilds objBlockInfo from the saved
  per-object blockNumber. The severance logic must be re-applied AFTER Load completes
  (i.e., as a post-load pass at the same point where the initial-load severance fires).
  This is a sequencing requirement for Stages 1-6, not a violation of HC-1 (the disk
  format remains unchanged), but it adds an execution site that must be identified and
  implemented in Plan 2.

HC-1 conclusion: SATISFIED for Save. Load satisfies HC-1 on-disk invariant but raises OB-1.

---

### Step 3: HC-4 proof -- all per-frame sites covered by one objBlockInfo severance

Per Step 1, every per-frame consumer reads block ranges via `objBlockInfo` fields or
`getObjBlock*` accessors that directly index `Terrain::objBlockInfo[]`:

- render (objmgr.cpp:1695-1706): reads `numObjects` and `firstHandle` from objBlockInfo.
- renderShadows (objmgr.cpp:1826-1837): same pattern.
- update (objmgr.cpp:2004-2017): same pattern.
- updateAppearancesOnly (objmgr.cpp:3040-3065): same pattern, called per-frame when paused.
- BattleMech::handleStaticCollision (mech.cpp:1115-1116): calls
  `getObjBlockNumCollidables` and `getObjBlockFirstHandle`, both of which read
  `objBlockInfo[blockNumber]` fields directly (objmgr.h:434-440).
- Ground vehicle collision (gvehicl.cpp:800-801): same getObjBlock* accessors.
- Artillery (artlry.cpp:743,746): calls `getObjBlockNumObjects` and `getObjBlockObject`,
  both reading `objBlockInfo` fields (objmgr.h:426-431).
- Carnage (carnage.cpp:533,536): same getObjBlock* accessors.

A single modification to the `objBlockInfo` block-range fields (numObjects, numCollidableObjects,
firstHandle as required) propagates to ALL per-frame consumers through the single data
structure. There is no per-frame consumer that bypasses objBlockInfo.

HC-4 conclusion: SATISFIED. All eight per-frame consumer sites are objBlockInfo-routed.
One severance drives `seen -> 0` for ALL.

IMPORTANT COMPLICATION -- decoratives in collidable prefix:
Decorative trees (TREE, TERRAINOBJECT) are classified as collidable in `countObject`
(objmgr.cpp:1017). They sit in the collidable prefix [firstHandle, firstHandle+numCollidableObjects).
The severance mechanism for the mech/gvehicl collision walk must account for this: either
(a) keep decoratives in the collidable prefix but have handleStaticCollision skip them on
type-check (already the case -- mech.cpp:1123-1126 has a switch over TREE/TREEBUILDING/
TERRAINOBJECT that sets isTangible), or (b) exclude decoratives from numCollidableObjects
as well. This is a design decision for Plan 2, not a blocker for Stage 0 analysis.

---

### Step 4: Exact edit site and collidable-prefix coherence proof

#### Where objBlockInfo block membership/ordering is built

`countTerrainObjects` (objmgr.cpp:927-1001) is the primary construction site:
  - Reads terrain object data from the terrain file
  - Calls `countObject(data)` per object (objmgr.cpp:1004-1046) which increments
    `numCollidableObjects` and `numObjects` per block
  - Assigns `firstHandle` contiguously: lines 992-996 iterate all blocks, setting
    firstHandle[i] = firstHandle[i-1] + numObjects[i-1]

`loadTerrainObjects` (objmgr.cpp:1073) follows countTerrainObjects and uses the same
block assignments to insert objects into objList at the computed positions.

The collidable prefix is defined by `numCollidableObjects` being filled first in the
handle range [firstHandle, firstHandle+numCollidableObjects), non-collidables fill
[firstHandle+numCollidableObjects, firstHandle+numObjects). This ordering is maintained by
the insertion logic in `loadTerrainObjects` (handles array built at lines 1083-1088).

#### Precise exclusion point

To exclude a decorative from per-frame iteration, the severance must:
  - Decrement `numObjects` for the block containing that decorative
  - Decrement `numCollidableObjects` if the decorative is in the collidable prefix
    (TREE/TERRAINOBJECT always are)
  - Adjust `firstHandle` for all subsequent blocks to maintain contiguity

The last requirement (adjusting firstHandle for subsequent blocks) CANNOT be done
without also reordering objList entries, because objList slot identity IS the handle
number and IS serialized. This is a fundamental constraint.

ALTERNATIVE EXCLUSION: Rather than modifying firstHandle/numObjects (which would require
objList reordering), the severance can be implemented as a PER-BLOCK DECORATIVE MASK or
by leaving numObjects/firstHandle intact and reducing only the effective count via a
separate parallel mechanism (e.g., a per-block `numDecorativesExcluded` counter, so that
effective_numObjects = numObjects - numDecorativesExcluded). The per-frame walks then
use the effective count. This avoids any reordering of objList.

COHERENCE FOR SURVIVING NON-DECORATIVE OBJECTS: If the block range is not physically
reordered and only the effective count changes, all surviving non-decorative objects in the
same block remain at their original objList slots and are still reachable at the same
handles. Their render/update/collision behavior is unchanged. HC-1 is preserved because
objList slot identity is unchanged.

This design choice (effective count, not physical reorder) is the load-bearing coherence
ruling for Plan 2. See OPEN BLOCKER OB-2 for the decorative-position-in-range constraint.

---

### Step 5: Typed-array (HC-3) disposition

`GameObjectManager::getTerrainObject(long terrainObjectIndex)` (objmgr.cpp:797-803):
  - Returns `terrainObjects[terrainObjectIndex]` directly
  - Does NOT route through objBlockInfo
  - survives severance unchanged; decorative still in terrainObjects[i] and objList[slot]

ABL call sites (re-grepped):
  code/ablmc2.cpp:1469 -- `TerrainObjectPtr terObj = ObjectManager->getTerrainObject(i)`
  code/ablmc2.cpp:1530 -- `TerrainObjectPtr terObj = ObjectManager->getTerrainObject(i)`
Both iterate i=0..numTerrainObjects-1 using the typed array, not objBlockInfo.

On-hit collision resolution: mech.cpp:1121 and gvehicl.cpp:806 call
`ObjectManager->get(terrainObjHandle + i)` which returns `objList[handle]`. The decorative
still occupies its objList slot; the only question is whether the collision walk reaches it
(governed by numCollidableObjects, which is the severance target).

HC-3 conclusion: No code change required for the typed-array path.
  - `terrainObjects[]` array: untouched, decorative still accessible by index
  - `objList[slot]`: untouched, slot identity preserved (HC-1)
  - `getTerrainObject(i)`: returns decorative on demand (non-per-frame ABL/collision-hit)
  - This is acceptable because these callers are not per-frame

---

### Step 6: Exit criterion

#### Per-step verdicts

Step 2 -- HC-1: PASS with caveat.
  Save path: ZERO objBlockInfo references. Disk bytes unchanged by objBlockInfo severance.
  Load path: objBlockInfo IS rebuilt in Load. This does not violate HC-1 (disk unchanged)
  but creates OPEN BLOCKER OB-1 (severance must be re-applied after Load).

Step 3 -- HC-4: PASS.
  All eight per-frame consumer sites route through objBlockInfo or getObjBlock* accessors.
  No per-frame site bypasses objBlockInfo. One severance covers all.
  Additional site found: `updateAppearancesOnly` (objmgr.cpp:3040, per-frame when paused),
  also objBlockInfo-routed.

Step 4 -- edit site + coherence: PASS with design ruling.
  Primary construction: `countTerrainObjects` / `countObject` (objmgr.cpp:993-1046).
  HC-2 coherence mechanism: effective-count approach (no objList reorder required).
  Surviving non-decoratives remain at original slots; handles unchanged.

#### Overall verdict: PASS with two OPEN BLOCKERS

No per-frame site escapes objBlockInfo severance. HC-1 disk invariant preserved. HC-4 fully
covered. Steps 2-4 each have written grep-grounded proofs.

---

## OPEN BLOCKERS from this task

### OB-1: Severance must survive save-game restore

Discovered in Step 2.

`GameObjectManager::Load` (objmgr.cpp:3460-3586) rebuilds `objBlockInfo` from saved
`data.blockNumber` per object, restoring the pre-severance layout. A severance applied
only at initial mission load would be undone by a save-game restore.

Required: the severance pass must execute both at initial mission load AND immediately after
`GameObjectManager::Load` completes. Plan 2 must identify a single shared severance function
and call it from both sites.

This is a sequencing requirement only. It does not invalidate the HC-2 mechanism or change
the disk format. HC-1 is satisfied.

### OB-2: Decorative position within block range (collidable prefix constraint)

Discovered in Steps 1 and 4.

Decorative trees (TREE, TERRAINOBJECT) are classified as `numCollidableObjects` in
`countObject` (objmgr.cpp:1017). They currently sit in the collidable prefix.
The effective-count severance approach must decide whether to exclude decoratives from
`numCollidableObjects` as well as `numObjects`. If yes, the remaining collision walk for
mech/gvehicl must still correctly address surviving collidables (buildings, turrets, gates).
If no (decoratives remain in collidable prefix but iteration count reduced), the prefix
ordering constraint for non-decorative collidables must be maintained.

This requires a decision on decorative ordering within the block range at Plan 2 time.
It does not block the Stage 0 analysis. The effective-count mechanism satisfies HC-1 and
HC-4 regardless of which ordering sub-choice is made, as long as surviving objects remain
at stable handles.

---

## Blocker 2: handle-lookup invariant (HC-3-corrected)

Date: 2026-05-17
Status: PASS -- every resolver classified; corrected invariant stated; no unclassified consumer.

### Framing correction (HC-3)

The original spec framing for Blocker 2 was: "generic lookup MUST NOT materialize a severed
decorative." That framing assumes decoratives are removed from objList (i.e., severed from
existence). Under HC-2 and HC-3 (established Task 1/2), decoratives are NOT removed from
objList or terrainObjects[]. They still occupy their original objList slot and typed-array
index for the full mission lifetime. Severance is only from the per-frame objBlockInfo block
ranges. The framing is therefore wrong and the rule must be corrected.

### Step 1: Generic resolver inventory (re-grepped 2026-05-17)

Grep run:
  rg -n "getTerrainObject|getObject|objList\[|getByHandle|getObjectFromHandle|findByPartId"
     code/objmgr.cpp code/objmgr.h

Resolvers that can return a decorative (TerrainObject) on demand:

R-1. GameObjectManager::get(GameObjectHandle handle)
     -- code/objmgr.cpp:2181-2187
     -- Returns objList[handle] for handle in [1, getMaxObjects()].
     -- Decorative still occupies its objList slot (HC-2); returns it on any valid handle.

R-2. GameObjectManager::getTerrainObject(long terrainObjectIndex)
     -- code/objmgr.cpp:797-803 (declaration: code/objmgr.h:371)
     -- Returns terrainObjects[terrainObjectIndex] directly; does NOT route through objBlockInfo.
     -- Post-severance decoratives remain in terrainObjects[]; this accessor returns them
        unconditionally for any valid index.

R-3. GameObjectManager::findByPartId(long partId)
     -- code/objmgr.cpp:2363-2376 (declaration: code/objmgr.h, grep findByPartId)
     -- Linear scan over objList[1..getMaxObjects()], matching obj->getPartId(). NOT
        objBlockInfo-routed. HC-2 severance does NOT silence this path.
     -- With decorative still in objList (HC-2), finds it if its partId matches.
     -- VERIFIED PER-FRAME PATH (conditional): GameCamera::update() (per-frame
        frame loop) -> when lookTargetObject != -1 (gamecam.cpp:550) ->
        getCamObject(lookTargetObject,true) (gamecam.cpp:551; getCamObject def gamecam.cpp:522) ->
        ObjectManager->findByPartId(partId) (gamecam.cpp:528). This is a full-objList
        linear scan fired every frame while the camera is locked to a look-target.
        SCOPE RULING -- this path is:
          (a) PRE-EXISTING: not introduced by the decorative-severance slice;
          (b) NOT decorative-specific: it scans ALL objects (objList[1..getMaxObjects()]),
              not decoratives qua decoratives;
          (c) therefore NOT a violation of the corrected invariant, whose prohibition is
              "no consumer RE-INTRODUCING per-frame iteration OVER DECORATIVES" -- this
              path neither re-introduces anything nor targets decoratives.
        CONCLUSION: OUT OF SCOPE for the objBlockInfo decorative-severance slice. This
        is a separate, pre-existing engine inefficiency (O(N) linear findByPartId on the
        camera-lock path). Recorded as candidate future work, NOT a blocker or regression.

R-4. GameObjectManager::findByCellPosition(long row, long col)
     -- code/objmgr.cpp:2390-2411
     -- Linear scan over all objList entries matching cell coordinates.
     -- Source comment: "PLEASE DO NOT CALL EVERY FRAME."
     -- With decorative in objList, can return it if queried for its row/col.

R-5. GameObjectManager::findByBlockVertex(long blockNum, long vertex)
     -- code/objmgr.cpp:2380-2386
     -- Wrapper that calls calcPartId(TERRAINOBJECT, blockNum, vertex) then findByPartId().
     -- Can reach a decorative. Call sites: objmgr.cpp (definition only); zero callers found
        in code/*.cpp or mclib/*.cpp (grep confirmed).

R-6. ABL getObject(long partId) helper (inline)
     -- code/ablmc2.cpp:338-353
     -- Calls ObjectManager->findByPartId(partId) (R-3). All ABL "getObject" calls route here.

R-7. ABL getTerrainObject typed-array path (execGetObjects criteria=1)
     -- code/ablmc2.cpp:1469 and :1530
     -- Calls ObjectManager->getTerrainObject(i) (R-2) for i in 0..getNumTerrainObjects().
     -- Collects partIds from the typed array; subsequent getObject(partId) calls then route
        through R-3 / R-6.

R-8. GameObjectManager::getObjBlockObject(long blockNumber, long objLocalIndex)
     -- code/objmgr.h:426-428
     -- Returns objList[Terrain::objBlockInfo[blockNumber].firstHandle + objLocalIndex].
     -- Block-indexed resolver; per-frame consumers (render/update/collision) use this.
     -- This is the TARGET of HC-2 severance; post-severance the block ranges exclude
        decoratives so per-frame loops cannot index them. Classified separately from on-demand
        resolvers below.

Absent resolvers:
  getObjectFromHandle, handleToObject, resolveHandle, getByHandle: none of these names appear
  in code/*.cpp or mclib/*.cpp (confirmed in BOUNDARY-PROOF Task 1 Step 1 / Class 13).

---

### Step 2: Corrected invariant

CORRECTED INVARIANT (HC-3):

  Decorative objects remain valid, resolvable objects for the full mission lifetime (HC-2).
  On-demand resolution of a decorative via any of R-1 through R-7 is ALLOWED because these
  consumers are not per-frame. They fire only in response to discrete events (ABL script
  execution, mouse clicks, turret/building init, save-game restore).

  The PROHIBITED thing is any consumer RE-INTRODUCING per-frame iteration over decoratives.
  Specifically: no per-frame consumer may route through an objBlockInfo range that includes
  a decorative handle (the HC-2 severance removes decoratives from those ranges), and no
  new per-frame loop over terrainObjects[] or over objList filtered to TREE/TERRAINOBJECT
  may be added post-severance.

  The HC-2 objBlockInfo severance is the single enforcement point. It controls all eight
  per-frame sites identified in Blocker 1. No additional enforcement at the resolver level
  is required by this invariant.

Per-resolver classification:

| Resolver | Classification | Calling frequency | Evidence |
|----------|---------------|-------------------|---------|
| R-1 get(handle) | on-demand, ALLOWED | event-sourced (mech hit, ABL) | objmgr.cpp:2181; callers in contact.cpp/ablmc2.cpp are mover-targeted |
| R-2 getTerrainObject(i) | on-demand, ALLOWED | ABL script execution (non-per-frame) | objmgr.cpp:797; ablmc2.cpp:1469,1530 criteria cases |
| R-3 findByPartId(partId) | per-frame-BUT-OUT-OF-SCOPE (pre-existing, non-decorative-specific); on-demand callsites (ABL, turret) also ALLOWED | per-frame via gamecam.cpp:550->436 when lookTargetObject!=-1; event-sourced via ABL/init | objmgr.cpp:2098; gamecam.cpp:522,456-459; pre-existing path, not decorative-specific, not re-introduced by this slice; recorded as future-work |
| R-4 findByCellPosition(row,col) | on-demand, ALLOWED | mission-load + savegame-restore only | objmgr.cpp:2390; callers at bldng.cpp:737, gate.cpp:266, objmgr.cpp:1125,1150,3766,3789, turret.cpp:550 -- all init/load |
| R-5 findByBlockVertex(blockNum,v) | on-demand, ALLOWED | zero callers (dead/unused) | objmgr.cpp:2380; no callers in code/*.cpp or mclib/*.cpp |
| R-6 ABL getObject(partId) | on-demand, ALLOWED | ABL script execution (event-sourced) | ablmc2.cpp:338-353; wraps R-3 |
| R-7 ABL getTerrainObject loop | on-demand, ALLOWED | ABL script execution (event-sourced) | ablmc2.cpp:1469,1530; ABL scripts are not per-frame |
| R-8 getObjBlockObject(block,idx) | per-frame -- HC-2 SEVERANCE TARGET | per-frame in render/renderShadows/update/collision | objmgr.h:426; resolved in Blocker 1 |

R-8 is the only per-frame resolver targeting decoratives via block ranges. It is the direct
target of HC-2 severance and is fully covered by Blocker 1.

NOTE on R-3 per-frame path: findByPartId is called per-frame on the camera-lock path
(gamecam.cpp:550 -> gamecam.cpp:528). This path is pre-existing, not decorative-specific
(scans all objList entries), and is not re-introduced by this slice. It is out of scope for
the decorative-severance invariant (which prohibits only RE-INTRODUCTION of per-frame
iteration OVER DECORATIVES). It is recorded here as a separate, pre-existing engine
inefficiency (O(N) linear scan per frame on camera-lock). Candidate future work.
No on-demand resolver (R-1 through R-7) RE-INTRODUCES per-frame access to decoratives.

---

### Step 3: Reconciliation with BOUNDARY-PROOF blockers and OB-1/OB-2

BOUNDARY-PROOF HARD BLOCKER #1 (render/update, Class 1):
  Root cause is R-8 (getObjBlockObject) used by the per-frame render/renderShadows/update
  loops. Resolved by HC-2 objBlockInfo severance (Blocker 1). The object still exists in
  objList at the same slot (HC-3); R-8 simply no longer includes its slot in the iterated
  range. Consistent with "object still exists, not per-frame-iterated."

BOUNDARY-PROOF HARD BLOCKER #2 (collision, Class 2):
  mech.cpp:1115-1139, gvehicl.cpp:800-825 use getObjBlockNumCollidables + R-8. The per-frame
  collision walk is R-8-routed and is covered by HC-2 severance. On-hit dispatch (R-1 /
  get(terrainObjHandle+i)) is only reached inside the same collision loop; once the loop
  excludes decoratives via HC-2, on-hit dispatch for decoratives does not fire per-frame.
  Deferred to Blocker 3 / Task 4: the precise collision-proxy design for discrete hit events
  (tree falling) is out of scope here and does not affect this invariant.
  OB-2 (decorative position within collidable prefix) carries into Task 4 unchanged.

BOUNDARY-PROOF HARD BLOCKER #3 (script triggers / findByPartId, Class 4):
  ABL getObject (R-6 -> R-3) scans objList for any object by partId. Under HC-3 the
  decorative is still in objList (HC-2); findByPartId returns it. This is ALLOWED by the
  corrected invariant because: (a) the ABL callsite is script-sourced (event-driven, not
  per-frame); and (b) separately, the per-frame camera-lock path that also uses findByPartId
  (gamecam.cpp:550 -> gamecam.cpp:528) is a pre-existing path that scans ALL objects,
  not decoratives specifically, and is therefore out of scope -- it neither re-introduces
  anything nor targets decoratives as a class. The BOUNDARY-PROOF #3 dissolution rests on
  the correct basis: decorative in objList per HC-2/HC-3, ABL callsite event-sourced, and
  the camera-lock per-frame findByPartId path out-of-scope as recorded in R-3 above.
  No code change required here for the decorative-severance slice.
  ABL execGetObjects criteria=1 (R-7 -> R-2) also returns decoratives via the typed array;
  also on-demand, also ALLOWED. The BOUNDARY-PROOF classified this as CAN-REACH-UNCOVERED
  under the old framing; under HC-3 it is resolved -- no severance action needed.

BOUNDARY-PROOF HARD BLOCKER #4 (save/load, Class 6):
  The Save loop (objmgr.cpp:3257-3280) iterates objList[0..getMaxObjects()]. Under HC-2
  decoratives remain in objList; Save writes them normally, disk format unchanged. HC-1
  satisfied. OB-1 (severance must survive save-game restore) identified in Blocker 1
  remains open and carries to Plan 2 sequencing; it does not involve resolver semantics.
  The BOUNDARY-PROOF classified save/load as CAN-REACH-UNCOVERED under the old framing;
  under HC-3 the decorative being in objList at save time is the intended behavior, so
  there is no new blocker here for Blocker 2.

Task 2 OB-1 (severance must survive save-game restore):
  Not a resolver issue. GameObjectManager::Load rebuilds objBlockInfo (R-8's backing data),
  not the resolvers. OB-1 remains open, carried to Plan 2. Consistent with this invariant.

Task 2 OB-2 (decorative position within collidable prefix):
  Affects which objects R-8 returns during the per-frame collision loop. On-demand resolvers
  (R-1 through R-7) are unaffected by collidable-prefix ordering. OB-2 deferred to
  Blocker 3 / Task 4.

---

### Step 4: Exit criterion

PASS.

Evidence:
- All eight resolvers from Step 1 are classified in the table above.
- R-1, R-2, R-4 through R-7: on-demand, event-sourced, ALLOWED by corrected invariant.
  No code change.
- R-3 (findByPartId): correctly classified per-frame-but-out-of-scope. The per-frame path
  via gamecam.cpp:550 -> gamecam.cpp:528 is pre-existing, non-decorative-specific (scans
  all objList), and not re-introduced by this slice. The corrected invariant prohibits
  RE-INTRODUCTION of per-frame iteration OVER DECORATIVES; this path satisfies none of those
  conditions. Recorded as separate pre-existing engine inefficiency (O(N) camera-lock linear
  scan) and flagged as candidate future work. NOT a blocker and NOT a regression. The
  on-demand ABL callsites for findByPartId remain event-sourced and ALLOWED.
- R-8: per-frame, fully covered by HC-2 objBlockInfo severance (Blocker 1, Task 2).
- No resolver is per-frame-AND-decorative-specific AND unresolved after HC-2 severance.
- Corrected invariant stated unambiguously: decoratives remain resolvable on demand;
  prohibited thing is re-introduction of per-frame iteration over decoratives, not on-demand
  resolution, and not pre-existing non-decorative-specific per-frame paths.
- BOUNDARY-PROOF HARD BLOCKERs #1-#4 each reconciled: #1 and #2 covered by Blocker 1;
  #3 resolved by HC-3 framing on the correct basis (decorative in objList per HC-2/HC-3,
  ABL callsite event-sourced, camera-lock per-frame path out-of-scope pre-existing); #4
  resolved by HC-3 framing (no-op under corrected invariant).
- OB-1 and OB-2 from Blocker 1 carry to later tasks without conflict.
- One potential new OPEN blocker inspected and dismissed:
  ABL execGetObjects criteria=1 (ablmc2.cpp:1469, :1530) iterates ALL terrainObjects[]
  including decoratives and emits their partIds. Under the OLD framing this was a blocker
  (leaked raw pointers to "severed" objects). Under HC-3 this is by design -- the decorative
  exists, the partId is valid, and any subsequent getObject(partId) call returns the live
  object. No blocker.
- Pre-existing engine inefficiency noted and recorded: O(N) linear findByPartId on the
  gamecam.cpp camera-lock path (gamecam.cpp:550-551 -> getCamObject -> findByPartId).
  Not a regression from this slice; candidate future work outside this scope.

---

## Blocker 3: collision proxy ownership (MANDATORY post-severance)

Date: 2026-05-17
Status: PASS -- ownership DEDICATED; complete proxy spec recorded; all four collision
callsites identified; deregister hook tied; OB-1/OB-2 interactions explicit.

All file:line citations re-grepped at write time.

---

### Step 1: Trace the terrain-object collision / spatial index

#### Grep run

  rg -n "objBlockInfo|collid|firstHandle|getObjBlock|terrainBlock"
     mclib/terrain.cpp code/objmgr.cpp code/terrobj.cpp
  rg -n "objBlockInfo|collid|firstHandle|getObjBlock|numCollidable|handleStaticCollision"
     code/mech.cpp code/gvehicl.cpp code/artlry.cpp code/carnage.cpp

#### How each collision caller currently locates an in-block decorative

MECH (BattleMech::handleStaticCollision, code/mech.cpp:1103-1142):
  mech.cpp:1115 -- numCollidables = ObjectManager->getObjBlockNumCollidables(blockNumber)
  mech.cpp:1116 -- terrainObjHandle = ObjectManager->getObjBlockFirstHandle(blockNumber)
  mech.cpp:1119-1140 -- for i in [0, numCollidables):
    terrainObj = ObjectManager->get(terrainObjHandle + i)   // reads objList[handle+i]
    switch on terrainObj->getObjectClass() { TREE, TREEBUILDING, TERRAINOBJECT, BUILDING }
    isTangible = terrainObj->getTangible()
    if isTangible: ObjectManager->detectStaticCollision(this, terrainObj)
  The loop reads getObjBlockNumCollidables and getObjBlockFirstHandle, which are both
  direct reads of Terrain::objBlockInfo[blockNumber] fields (code/objmgr.h:434-440).
  TREE/TERRAINOBJECT are placed at curCollidableHandle (the collidable prefix) by
  addObject (code/objmgr.cpp:1366-1367), so they ARE reachable by this loop at present.
  The isTangible guard further filters: a fallen/falling tree has setTangible(false)
  called (terrobj.cpp:393), so it is walked but skipped by the isTangible check.

GROUND VEHICLE (GroundVehicle::handleStaticCollision, code/gvehicl.cpp:789-827):
  gvehicl.cpp:800 -- numCollidables = ObjectManager->getObjBlockNumCollidables(blockNumber)
  gvehicl.cpp:801 -- terrainObjHandle = ObjectManager->getObjBlockFirstHandle(blockNumber)
  gvehicl.cpp:804-825 -- identical iteration pattern to mech; same getObjectClass switch.
  TREE/TERRAINOBJECT reachable via same collidable-prefix path.

ARTILLERY (Artillery::handleStaticCollision, code/artlry.cpp:672-758):
  artlry.cpp:743 -- numObjectsInBlock = ObjectManager->getObjBlockNumObjects(currentBlockNumber)
  artlry.cpp:744-747 -- for objIndex in [0, numObjectsInBlock):
    obj = ObjectManager->getObjBlockObject(currentBlockNumber, objIndex)
    if obj->getExists(): ObjectManager->detectStaticCollision(this, obj)
  Artillery uses getObjBlockNumObjects (full range, NOT just collidables) and
  getObjBlockObject. It iterates a 3x3 block neighbourhood. ALL objects including
  decoratives are in scope.

CARNAGE (Carnage::handleStaticCollision, code/carnage.cpp:509-544):
  carnage.cpp:533 -- numObjectsInBlock = ObjectManager->getObjBlockNumObjects(currentBlockNumber)
  carnage.cpp:534-538 -- for objIndex in [0, numObjectsInBlock):
    obj = ObjectManager->getObjBlockObject(currentBlockNumber, objIndex)
    if obj->getExists() && class != GATE && class != TURRET:
      ObjectManager->detectStaticCollision(this, obj)
  Carnage also uses getObjBlockNumObjects (full range) and iterates a 3x3 neighbourhood.
  Decoratives in those blocks are reachable.

#### All four collision callers are objBlockInfo-routed

Every one of the four callers reads objBlockInfo fields through getObjBlock* accessors
(code/objmgr.h:426-440), which are direct struct-field reads of Terrain::objBlockInfo[].
There is no separate spatial index (k-d tree, grid, or per-cell pointer array) that any
of these callers consults. The objBlockInfo block range IS the spatial index.

#### Does the existing collidable index answer a decorative hit WITHOUT routing through the per-frame objBlockInfo walk?

No. The collidable-prefix mechanism IS objBlockInfo. It is not a separate structure:
  terrain.h:102-108 -- ObjBlockInfo { numCollidableObjects; numObjects; firstHandle }
  objmgr.h:434-440 -- getObjBlockNumCollidables / getObjBlockFirstHandle read these fields
  mech.cpp:1115-1116 / gvehicl.cpp:800-801 -- call those same accessors

There is no independent collision proxy, AABB tree, or cell-indexed decorative list.
Answering a "which decorative is at position X?" query requires either:
  (a) routing through objBlockInfo (the per-frame walk we are severing), or
  (b) a NEW structure not currently present.

REUSE DISQUALIFIED: A non-routing proof is impossible because the collidable-prefix
structure and the per-frame objBlockInfo walk are the same data structure. Any post-
severance use of getObjBlockNumCollidables or getObjBlockFirstHandle to find decoratives
would re-read exactly the block-range fields the HC-2 severance reduces -- re-introducing
the forbidden per-frame walk for decoratives via a different code path. This is the
Blocker-2 forbidden re-introduction.

Conclusion: DEDICATED proxy required.

---

### Step 2: Ownership decision -- DEDICATED

DECISION: DEDICATED decorative collision proxy.

JUSTIFICATION: The existing collidable-prefix index is not separable from the per-frame
objBlockInfo walk. REUSE would require reading the same Terrain::objBlockInfo fields that
HC-2 severs for decoratives. A dedicated proxy provides the collision service without
routing through the severed range.

#### Proxy data source

Mission load (countTerrainObjects / loadTerrainObjects path, code/objmgr.cpp:927-1113):
  For each TREE/TERRAINOBJECT object (objType->getObjectClass() == TREE or TERRAINOBJECT,
  code/objmgr.cpp:1015-1018 and 1356-1370):
  - GameObjectHandle handle (assigned at curCollidableHandle, objmgr.cpp:1367)
  - ObjectClass (TREE or TERRAINOBJECT)
  - World position (objData->vector, used at objmgr.cpp:1452)
  - AABB / extentRadius (from objType->getExtentRadius(), available post-init)
  - blockNumber (objData->blockNumber, available at count time)

These are the same fields the (future) instance bake uses. The proxy is populated in a
single pass over the ObjDataLoader array, in the same loop that currently calls
countObject / addObject.

#### Proxy keying

Key per proxy entry:
  - GameObjectHandle handle (the objList slot index assigned by addObject)
  - generation (a monotonic counter incremented when a handle slot is reused; prevents
    stale handle references from matching a newly-occupied slot; generation=0 at
    mission load, never incremented for static decoratives since they never leave objList)
  - blockNumber (the terrain block this decorative belongs to; enables O(1) block lookup)
  - AABB (world-space axis-aligned bounding box; used for broadphase rejection)

Alternative key sufficient for this slice (since decoratives never move and never leave
objList): handle alone is sufficient for validity checks. blockNumber and AABB enable
efficient spatial query. Generation is a defensive guard against future use; zero cost
to carry.

#### Collision callsites to repoint

All four callers (mech, gvehicl, artlry, carnage) must be updated. The proxy supports
two query shapes that match the existing caller patterns:

CALLSITE-1 (mech, gvehicl):
  Current: for i in [0, numCollidables): terrainObj = get(firstHandle + i)
  Post-severance: [0, numCollidables) no longer includes decoratives (HC-2 severance
  reduces numCollidableObjects to exclude them -- see Step 4/OB-2 ruling). Therefore
  the mech/gvehicl loop already CANNOT reach decoratives after severance; it still
  correctly finds non-decorative collidables (buildings, turrets, gates).
  HOWEVER: to preserve tree/terrain-object collision, the caller must ADDITIONALLY
  query the proxy for the same blockNumber. The proxy lookup returns only TANGIBLE
  decoratives in the block (proxy filters on tangible flag, see Step 3).
  Repoint: mech.cpp:1119-1140 and gvehicl.cpp:804-825 -- after the numCollidables loop,
  add a second loop over DecorativeCollisionProxy::getBlock(blockNumber), iterating
  only proxy entries where proxyEntry.tangible == true. Each entry yields a handle;
  ObjectManager->get(handle) returns the live TerrainObject* (HC-3 guarantees it).
  detectStaticCollision call is unchanged.

CALLSITE-2 (artlry, carnage):
  Current: for objIndex in [0, numObjects): obj = getObjBlockObject(block, objIndex)
  Post-severance: numObjects is reduced for the block (HC-2 severs decoratives from
  numObjects). This means artlry/carnage NO LONGER REACH decoratives via numObjects.
  To preserve artillery/explosion-vs-tree collision, the caller must query the proxy
  for each of the nine blocks in the 3x3 neighbourhood.
  Repoint: artlry.cpp:737-757 and carnage.cpp:527-544 -- for each currentBlockNumber
  in the 3x3 loop, after the numObjects loop, add a proxy query:
  DecorativeCollisionProxy::getBlock(currentBlockNumber) -> iterate tangible entries ->
  get(handle) -> detectStaticCollision. Same filter as CALLSITE-1.

SUMMARY of callers to repoint:
  code/mech.cpp:1115-1140      BattleMech::handleStaticCollision
  code/gvehicl.cpp:800-825     GroundVehicle::handleStaticCollision
  code/artlry.cpp:737-757      Artillery::handleStaticCollision (inner block loop)
  code/carnage.cpp:527-544     Carnage::handleStaticCollision (inner block loop)

No other collision caller was found in code/*.cpp or mclib/*.cpp that independently
iterates terrain objects by block range. (code/collsn.cpp:590 detectStaticCollision
is a dispatcher, not a locator; it operates on already-resolved pointers.)

---

### Step 3: Deregister hook -- tying proxy removal to OBJECT_FLAG_FALLING

#### OBJECT_FLAG_FALLING set site (re-grepped)

code/terrobj.cpp:385-393 -- TerrainObjectType::handleCollision, TERROBJ_TREE branch:
  if (!tree->getFlag(OBJECT_FLAG_FALLEN) && !tree->getFlag(OBJECT_FLAG_FALLING)):
    tree->setFlag(OBJECT_FLAG_FALLING, true)          // terrobj.cpp:386
    tree->setTangible(false)                           // terrobj.cpp:393
  "Tree has fallen. You may no longer collide with it." (comment at terrobj.cpp:392)

This is the canonical discrete event where a decorative tree transitions from collidable
to non-collidable. It fires exactly once per tree per mission (the outer flag check
ensures idempotence: already-fallen or already-falling trees skip the branch).

#### How the proxy is updated on this event

The proxy entry carries a `tangible` boolean mirroring OBJECT_FLAG_TANGIBLE. When
setTangible(false) is called at terrobj.cpp:393, the same call (or a wrapper called
immediately after) clears proxyEntry.tangible for the matching handle. The proxy's
getBlock() iterator filters on proxyEntry.tangible, so cleared entries are skipped.

Deregister hook design (consistent with HC-3):
  The decorative is NOT removed from the proxy. Its entry is tombstoned (tangible=false).
  This is consistent with HC-3: the object still exists in objList at its original slot.
  Only its per-frame iteration (severed by HC-2) and its proxy-side collidability change
  on the discrete event. The tombstone is read O(1) per proxy query.
  No entry is ever removed from the proxy during a mission; the proxy is mission-lifetime
  read-mostly after population.

setTangible-vs-proxy invariant: proxyEntry.tangible MUST be cleared in the same
callsite that clears OBJECT_FLAG_TANGIBLE for decoratives (terrobj.cpp:393). No other
path clears tangibility for a tree at the falling transition; this is the single hook
point. (Other setTangible(false) calls in the codebase are on buildings, vehicles, and
carnage; they do not affect decorative proxy entries.)

Separately, OBJECT_FLAG_FALLEN is set at terrobj.cpp:684 (end of fall animation in
TerrainObject::update). At that point tangibility is already false (set at fall start);
no additional proxy update is needed.

---

### Step 4: Reconcile with Task 2 OB-1 and OB-2

#### OB-2 (decorative position in collidable prefix -- interaction with proxy)

OB-2 asks: does HC-2 severance also exclude decoratives from numCollidableObjects, or
does only numObjects change?

The proxy design answers this definitively. The mech/gvehicl collision walk uses
getObjBlockNumCollidables (the collidable prefix). Post-severance, the proxy is the
replacement collision source for decoratives. Therefore:

RULING: HC-2 severance MUST exclude decoratives from BOTH numCollidableObjects AND
numObjects for the mech/gvehicl path to work correctly.

Rationale:
  - If decoratives remain in the collidable prefix (numCollidableObjects includes them),
    the mech/gvehicl loop still reaches them via the original objBlockInfo path.
    This re-introduces the per-frame decorative walk for collision, violating HC-2.
  - The proxy provides the collision service instead; the mech/gvehicl loop must use
    ONLY the proxy for decoratives.
  - Therefore numCollidableObjects must be reduced to exclude decoratives, and the proxy
    must be queried as a second pass (CALLSITE-1 above).

For artlry/carnage, numObjects must also be reduced (CALLSITE-2 above).

OB-2 ruling: the effective-count severance must reduce BOTH numCollidableObjects and
numObjects for blocks containing decoratives. Surviving non-decorative collidables
(buildings, turrets, gates) remain at their original slots in objList and their handles
are unchanged (HC-1 preserved). The ordering constraint in the collidable prefix (collidables
first, then non-collidables) is maintained for the surviving objects because their
handles are not moved; only the per-block counts change.

This is the load-bearing OB-2 resolution. It closes OB-2 as a design ruling.

#### OB-1 (severance must survive save-game restore -- interaction with proxy)

OB-1 states: GameObjectManager::Load (objmgr.cpp:3460-3586) rebuilds objBlockInfo from
saved data.blockNumber per object, restoring the pre-severance layout. The severance
pass must be re-applied after Load.

The proxy is subject to the same OB-1 sequencing requirement:
  - At initial mission load: proxy is populated from the ObjDataLoader array
    (same pass as countTerrainObjects/addObject).
  - After GameObjectManager::Load: objBlockInfo is rebuilt to the unsevered state AND
    proxy state (tangible flags) is reset to match the loaded game state. Specifically,
    if the loaded game has a tree with OBJECT_FLAG_FALLEN set, the proxy entry for that
    tree must have tangible=false after Load. Load writes OBJECT_FLAG_FALLEN state via
    the per-object data.damage field (re-check at Plan 2 time; the precise field is the
    damage byte encoding, objmgr.cpp:3355-3360 area).
  Plan 2 must identify a single shared (severance + proxy-rebuild) function called from
  both initial mission load and post-Load. OB-1 carries to Plan 2 unchanged but now
  includes the proxy rebuild requirement as well as the objBlockInfo severance.

---

### Step 5: Exit criterion

PASS.

Evidence:
1. All four collision callsites identified with grep-verified file:line:
   - BattleMech::handleStaticCollision: mech.cpp:1103-1142 (key lines 1115-1139)
   - GroundVehicle::handleStaticCollision: gvehicl.cpp:789-827 (key lines 800-824)
   - Artillery::handleStaticCollision: artlry.cpp:672-758 (key lines 743-751)
   - Carnage::handleStaticCollision: carnage.cpp:509-544 (key lines 533-538)

2. REUSE disqualified with airtight non-routing proof: the collidable prefix IS
   Terrain::objBlockInfo (terrain.h:102-108). Using it post-severance re-introduces the
   forbidden per-frame decorative walk. No separate spatial index exists.

3. DEDICATED proxy specified completely:
   - Data source: ObjDataLoader array at mission load (same pass as countTerrainObjects)
   - Key: handle, blockNumber, AABB, tangible flag (mirror of OBJECT_FLAG_TANGIBLE)
   - Population: initial mission load (same pass as countTerrainObjects / addObject,
     objmgr.cpp:927-1113) and post-GameObjectManager::Load (OB-1 sequencing).
   - Query shape CALLSITE-1 (mech/gvehicl): getBlock(blockNumber) -> filter tangible ->
     get(handle) -> detectStaticCollision. Added as second pass after numCollidables loop.
   - Query shape CALLSITE-2 (artlry/carnage): same getBlock query per block in 3x3 loop,
     added after the numObjects loop.
   - Deregister hook: clears proxyEntry.tangible at terrobj.cpp:393 (same callsite as
     setTangible(false) on tree fall). Idempotent by construction (OBJECT_FLAG_FALLING
     checked before entry).

4. OB-2 interaction: RULING RECORDED. HC-2 severance must reduce numCollidableObjects AND
   numObjects. Proxy provides replacement collision service for all four callers.
   OB-2 is closed as a design ruling.

5. OB-1 interaction: proxy rebuild requirement added to OB-1 scope. OB-1 carries to
   Plan 2 with expanded definition.

6. No collision caller is left unaddressed. No new OPEN blocker.

---

### New OPEN blocker from this task

None. All four collision callers have a stated proxy-repoint path. OB-2 is closed.
OB-1 is expanded but not a new blocker.

One implementation note for Plan 2 (not a blocker): the proxy's getBlock() API must
correctly handle block-boundary cases for the 3x3 neighbourhood (artlry/carnage) --
out-of-range blockNumbers must return an empty iterator, matching the existing
"(currentBlockNumber >= 0) && (currentBlockNumber < totalBlocks)" guard at
artlry.cpp:741 / carnage.cpp:531. This is a correctness requirement for Plan 2, not an
investigation blocker.

---

### HC-1 proof completion (CopyTo / ObjectManagerData discharge)

The Task 4 spec-compliance review flagged that the Blocker 1 HC-1 proof closed the
save-path question only by grepping the literal token `objBlockInfo` inside `Save()`
(zero hits), but `GameObjectManager::Save` serializes an `ObjectManagerData` struct via
`CopyTo(&data)` (objmgr.cpp:3251-3254) and the proof never discharged whether that
struct carries the per-block `numObjects` / `numCollidableObjects` / `firstHandle`
counts that the OB-2 ruling reduces. Because OB-2 mandates reducing those counts, this
vector is load-bearing for HC-1 (and for the user's explicit "descope save games"
ruling).

Discharged by direct grep this task:

- `_ObjectManagerData` (code/objmgr.h:171-188) contains ONLY global type counts
  (maxObjects, numElementals, numTerrainObjects, numBuildings, numTurrets, numWeapons,
  numCarnage, numLights, numArtillery, numGates, maxMechs, maxVehicles, numMechs,
  numVehicles, nextWatchId). It contains NO `objBlockInfo`, NO `numObjBlocks`, and NO
  per-block `numObjects` / `numCollidableObjects` / `firstHandle`.
- `objBlockInfo` is a `static ObjBlockInfo*` member of class `Terrain`
  (mclib/terrain.h:179); `numObjBlocks` is `static long` (terrain.h:178). The
  per-block counts the OB-2 ruling reduces live exclusively in `Terrain::objBlockInfo`,
  entirely outside `ObjectManagerData` / `CopyTo` / the save packet stream.

Conclusion: the OB-2 count reduction touches nothing in the on-disk save path. HC-1 is
fully discharged (CONFIRMED by grep, not "almost certainly"). The user's save-descope
ruling is preserved. Task 8 need not re-litigate this.

---

### Elimination-target baseline (supporting evidence, not the ship gate)

Current-build Tracy histogram for the `GameLogic.Units.TerrainObjects` bucket (the
per-frame static-object cost this slice eliminates), user-captured 2026-05-17:
mean 1.53 ms, median 1.43 ms, mode 1.41 ms, sigma 403 us, P75 1.63 ms, P90 1.97 ms,
P99 2.63 ms, P99.9 6.62 ms, max 15.19 ms, self-time 79.64%.

The fat upper tail (P99.9 6.62 ms vs 1.43 ms median) is consistent with the
`needsFullBakeNextFrame` / camera-move re-bake bursts the spec identifies. Per the
machine-contamination caveat this is SUPPORTING EVIDENCE only; the primary ship gate
remains the contamination-immune logic counter `decoratives_seen_in_objmgr_loop == 0`
(Stage 3) plus dual-output bit-identity parity. This baseline informs the Task 8
design-delta and the Plan 2 substitutive done-governor.

---

## Blocker 4: static-shadow lifecycle + dependency

Date: 2026-05-17
Status: PASS -- static-shadow dependency verdict BUILD-FIRST (FBO/matrix/consumer chain
exists; decorative-mesh static-caster submission path does not and must be built);
all five lifecycle questions answered with no TBD; two cross-slice prerequisites
recorded (CP-1 per-mission reset; CP-2 decorative static-caster submission).

Scope note: the elimination set for this slice is pure-static decoratives
TREE/TERRAINOBJECT only. Buildings (BldgAppearance) and turrets are decomposed out to
a follow-on spec (design Section 2). All shadow analysis below is scoped accordingly.

All file:line citations grep-verified at write time.

---

### Step 1: Confirm the static shadow map state

#### Grep runs executed

Broad shadow infrastructure search:
  rg -n "staticShadow|StaticShadow|worldFixedShadow|shadowMap|shadowFBO"
     GameOS/gameos/gos_postprocess.cpp
  rg -n "staticShadow|StaticShadow|worldFixed|static.*shadow|shadow.*static"
     GameOS/gameos/gos_postprocess.cpp
  rg -n "staticShadow|StaticShadow|worldFixed|static.*shadow|shadow.*static"
     GameOS/gameos/gameos_graphics.cpp
  rg -n "renderShadows|renderStaticShadows|staticShadow|static_shadow"
     mclib/txmmgr.cpp

Shadow lifecycle API:
  rg -n "gos_BeginShadowPrePass|gos_EndShadowPrePass|gos_BuildStaticLightMatrix|
     gos_MarkStaticLightMatrixBuilt|gos_StaticLightMatrixBuilt|gos_RequestFullShadowRebuild|
     gos_ShadowRebuildPending|gos_ClearShadowRebuildPending"
     GameOS/gameos/gameos_graphics.cpp

TreeAppearance renderShadows:
  rg -n "renderShadows" mclib/bdactor.cpp

SetAlphaTest calls in bdactor.cpp:
  rg -n "SetAlphaTest" mclib/bdactor.cpp

eligibleForDynamicShadow definition:
  rg -n "eligibleForDynamicShadow" mclib/tgl.cpp

Static shadow draw call:
  rg -n "gos_DrawShadowBatchTessellated" mclib/*.cpp GameOS/gameos/*.cpp

#### Findings

##### Static world-fixed shadow map: EXISTS AND IS FULLY OPERATIONAL

The plan-write finding ("not implemented in GameOS/gameos/") is REFUTED.

Evidence (opposite-direction grep -- producers and consumers of the FBO/texture/matrix):

PRODUCER SIDE (writes depth into the static shadow map):

1. `gosPostProcess::initShadows()` (gos_postprocess.cpp:1073) -- allocates `shadowFBO_`
   and `shadowDepthTex_` (a 4096x4096 GL_DEPTH_COMPONENT32F texture) at renderer
   creation time (called from gosPostProcess::init, line 188). This is the static shadow
   FBO, distinct from `dynShadowFBO_` which is allocated separately at line 189 via
   initDynamicShadows().

2. `gosPostProcess::buildStaticLightMatrix()` (gos_postprocess.cpp:1194) -- builds the
   world-fixed orthographic light-space matrix centered at map origin, covering the full
   map diagonal. Stores it in `staticLightSpaceMatrix_[16]` (gos_postprocess.h:153,
   comment: "world-fixed ortho, built once at map load"). Idempotent: early-returns if
   `staticLightMatrixBuilt_` is already set (line 1198).

3. `gosRenderer::beginShadowPrePass(bool clearDepth)` (gameos_graphics.cpp:3973) --
   binds `shadowFBO_`, configures GL depth state, binds the shadow shader, and uploads
   `getLightSpaceMatrix()` (the `staticLightSpaceMatrix_`). Called from txmmgr.cpp
   Shadow.StaticAccum path (see below).

4. `gos_DrawShadowBatchTessellated()` (gameos_graphics.cpp:6282) -- submits terrain
   vertex batches into the bound shadow FBO. ONLY ONE callsite in the entire codebase:
   `mclib/txmmgr.cpp:1575` inside the `Shadow.StaticAccum` scope.

5. `mclib/txmmgr.cpp:1525-1587` (Shadow.StaticAccum scope) -- per-frame conditional
   that fires when tessellation is active AND (camera has moved >100 units OR a full
   rebuild is pending). On each fire:
   - builds the static light matrix on first frame (txmmgr.cpp:1549-1553)
   - calls gos_BeginShadowPrePass(firstFrame) to bind the FBO and optionally clear
   - iterates masterVertexNodes[] filtering for MC2_DRAWSOLID | MC2_ISTERRAIN
     (txmmgr.cpp:1557-1558) and submits matching terrain patches via
     gos_DrawShadowBatchTessellated() (txmmgr.cpp:1575)
   - calls gos_EndShadowPrePass()

   This is the sole static shadow accumulation path. The loop filter (MC2_DRAWSOLID |
   MC2_ISTERRAIN) submits only terrain patch vertex nodes via
   gos_DrawShadowBatchTessellated, which takes terrain-specific vertex/extra arrays.
   There is NO existing path to submit a decorative TG_Shape mesh (vb/ib/vdecl)
   into the static map -- that submission entry point does not exist and must be built.

6. `mclib/txmmgr.cpp:1589-1621` (Shadow.DynPass scope) -- per-frame path that fires
   every frame when tessellation is active and g_numShadowShapes > 0. Writes into
   `dynShadowFBO_` / `dynShadowDepthTex_` (the DYNAMIC shadow map, separate FBO).
   Covers mechs and other dynamic movers. Completely separate from the static FBO.

CONSUMER SIDE (reads the static shadow depth texture):

7. `gosPostProcess::renderScreenSpaceShadow()` (gos_postprocess.cpp:648) -- binds
   `shadowDepthTex_` (the static shadow depth texture) to a sampler and renders the
   full-screen shadow composite. Uses `staticLightSpaceMatrix_` as `lightSpaceMatrix`.

8. Terrain draw in `gosRenderer` (gameos_graphics.cpp:4320, 4424, 4535) -- uploads
   `pp->getLightSpaceMatrix()` (== staticLightSpaceMatrix_) as `lightSpaceMatrix` uniform.

9. `shaders/include/shadow.hglsl:4` -- `uniform sampler2DShadow shadowMap` is the
   static map. `calcShadow()` (line 33) uses `lightSpaceMatrix` (the static world-fixed
   matrix). `calcDynamicShadow()` (line 91) uses `dynamicLightSpaceMatrix` and
   `dynamicShadowMap` (the dynamic FBO). These are two SEPARATE samplers.

IN-SCOPE DECORATIVE (TREE/TERRAINOBJECT) -- NO-SHADOW-TODAY PROOF (BOTH ROUTES):

The no-shadow-today conclusion for the in-scope set (TREE/TERRAINOBJECT) is proved via
two independent routes, both grep-verified.

Route A -- renderShadows tessellation early-return:

10. `TreeAppearance::renderShadows()` (mclib/bdactor.cpp:4703-4707):
    ```
    if (gos_IsTerrainTessellationActive())
        return NO_ERR;
    ```
    `gos_IsTerrainTessellationActive()` (gameos_graphics.cpp:6223) returns true on the
    modern render path. Under the modern path TreeAppearance::renderShadows is a
    complete NO-OP; no geometry is submitted to any shadow FBO via this route.

Route B -- render() -> TG_Shape::Render -> eligibleForDynamicShadow exclusion:

11. `TG_Shape::Render()` (mclib/tgl.cpp:3057-3062) evaluates eligibleForDynamicShadow:
    ```
    const bool eligibleForDynamicShadow =
        !isSpotlight && !isWindow && !isHudElement && !isClamped &&
        !theShape->alphaTestOn && (alphaValue == 0xff) && shapeToWorld;
    if (eligibleForDynamicShadow) {
        addShadowShape(theShape->vb_, theShape->ib_, theShape->vdecl_, shapeToWorld->entries);
    }
    ```
    For tree shapes: `TreeAppearance` calls `treeShape[i]->SetAlphaTest(true)` at
    bdactor.cpp:3627, bdactor.cpp:3650, and `treeShadowShape->SetAlphaTest(true)` at
    bdactor.cpp:3669. This sets `alphaTestOn = true` on every tree TG_Shape.
    The `!theShape->alphaTestOn` term in eligibleForDynamicShadow is therefore FALSE
    for all tree shapes, so `addShadowShape` is NEVER called. Trees cast no dynamic
    shadow via the per-frame render()->TG_Shape::Render path.

    `SetAlphaTest` is called ONLY for treeShape and treeShadowShape in bdactor.cpp
    (grep-verified: no `bldgShape->SetAlphaTest` exists anywhere in bdactor.cpp).

Combined: trees cast NO shadow today -- neither static (Route A) nor dynamic (Route B).
Severing them from both the objBlockInfo renderShadows walk and the per-frame
TG_Shape::Render path removes zero existing shadow contribution. The design proposal
to bake decorative shadows into the static map is ADDITIVE, not a replacement of
existing behavior. No shadow regression risk for the in-scope TREE/TERRAINOBJECT set.

OUT-OF-SCOPE NOTE -- BUILDINGS (FOLLOW-ON):

BldgAppearance::renderShadows() (bdactor.cpp:2112-2116) also early-returns under
tessellation (Route A, same NO-OP on the static renderShadows path). HOWEVER, grep
confirms there is NO `bldgShape->SetAlphaTest(true)` anywhere in bdactor.cpp. Opaque
decorative buildings therefore have `alphaTestOn = false` and DO satisfy
eligibleForDynamicShadow (tgl.cpp:3057-3059) when they pass the remaining predicates.
This means decorative buildings currently DO cast a real dynamic shadow via the
per-frame render()->TG_Shape::Render->addShadowShape route (Route B).

Buildings are decomposed out to a follow-on spec (design Section 2). That follow-on
slice MUST treat shadow severance as a regression risk -- severing building shapes
from the per-frame render path removes a real, currently-active dynamic shadow. It is
NOT additive for buildings. This is an explicit follow-on obligation; it does NOT
affect the in-scope TREE/TERRAINOBJECT no-regression conclusion above.

MISSION-RESET BEHAVIOR:

12. `gosPostProcess::initShadows()` sets `staticLightMatrixBuilt_ = false` and clears
    the depth texture (gos_postprocess.cpp:1117-1125). This runs ONCE at renderer
    creation (`new gosPostProcess()` at gameos_graphics.cpp:5175), which is
    process-lifetime, NOT mission-lifetime. The gosPostProcess is created and destroyed
    once per process startup/shutdown.

13. `s_terrainShadowPrimed` (txmmgr.cpp:1510) is a file-scope `static bool`,
    initialized to false at program start. It is NOT reset between missions. The
    same applies to `s_shadowRebuildPending` (gameos_graphics.cpp:6278).

    IMPLICATION: the static shadow map accumulates across missions within one process
    run. On a new mission load within the same process, the shadow map retains depth
    from the previous mission until the Shadow.StaticAccum path fires with a new
    terrain frame, which forces a full rebuild on first frame (`firstFrame = !gos_StaticLightMatrixBuilt()`).
    The `gos_RequestFullShadowRebuild()` call in the first-frame latch (txmmgr.cpp:1517)
    resets the accumulated depth on the first terrain submission of the new mission.

#### Dependency verdict: BUILD-FIRST

The static world-fixed shadow map infrastructure is:
  - ALLOCATED: shadowFBO_ + shadowDepthTex_ (4096x4096, process-lifetime)
  - OPERABLE: gos_BeginShadowPrePass / gos_EndShadowPrePass form the FBO write frame;
    gos_DrawShadowBatchTessellated is the existing terrain-specific submission call
  - DISTINCT from the dynamic shadow map (dynShadowFBO_ / dynShadowDepthTex_)
  - CONSUMED: by calcShadow() (shadow.hglsl) via shadowMap sampler (tex unit 9 per
    gameos_graphics.cpp:4323)

The plan-write claim "not implemented in GameOS/gameos/" was incorrect. The FBO,
matrix, and consumer chain exist and work. The CLAUDE.md "Known issues" stutter note
refers to the camera-move re-bake threshold, not to the FBO being absent.

HOWEVER: the Shadow.StaticAccum loop (txmmgr.cpp:1556-1581) filters exclusively for
MC2_DRAWSOLID | MC2_ISTERRAIN and submits only via gos_DrawShadowBatchTessellated,
which takes tessellated terrain vertex/extra arrays. There is NO existing path to
submit a decorative TG_Shape mesh (vb/ib/vdecl) into the static shadow map. A new
decorative-mesh static-caster submission entry point must be built -- this is new
infrastructure analogous to the dynamic pass's object-batch submission (addShadowShape
+ dynShadowFBO_). The existing FBO/matrix/consumer chain is reusable but the
decorative-caster write path does not exist.

VERDICT: BUILD-FIRST -- FBO/matrix/consumer EXISTS; decorative-mesh static-caster
submission path DOES NOT exist and must be built before this slice can bake decorative
shadows. Recorded as cross-slice prerequisite CP-2 below.

---

### Step 2: Lifecycle and destruction semantics (five questions)

All five questions are answered with no TBD.

#### Q1: Static shadow map allocation time

ANSWER: Process-lifetime, not mission-lifetime. The FBO and depth texture are allocated
once in gosPostProcess::initShadows() (gos_postprocess.cpp:1073), called from
gosPostProcess::init() (line 188), which is called when the gosPostProcess object is
created (gameos_graphics.cpp:5175, `new gosPostProcess()`). This happens once per
process startup. The FBO is not reallocated between missions.

Consequence: decorative shadow bake infrastructure is always available. No per-mission
allocation is required.

#### Q2: Decorative-shadow bake order relative to terrain static bake

ANSWER (scoped to in-scope TREE/TERRAINOBJECT set): The correct integration point is
inside the mclib/txmmgr.cpp Shadow.StaticAccum scope (txmmgr.cpp:1525-1587), AFTER the
existing terrain batch submission loop (txmmgr.cpp:1556-1581), BEFORE
gos_EndShadowPrePass(). Once CP-2 new infrastructure provides a decorative-mesh
submission entry point (a new function analogous to gos_DrawShadowBatchTessellated
but accepting TG_Shape vb/ib/vdecl), decorative trees can be submitted in the same
accumulation frame as terrain.

Order within the static bake frame: terrain first, decoratives second. Terrain depth
is the dominant occlusion; decorative depth composites additively into the same FBO
(glDepthFunc(GL_LESS) + no clear on the decorative step). This is the same accumulation
semantics the existing terrain multi-frame path uses for incremental updates (the
beginShadowPassNoClear path at gos_postprocess.cpp:1153).

Buildings are out of scope for this slice; the bake order question for buildings is
deferred to the follow-on spec.

#### Q3: Dynamic-caster (mech) compositing order relative to the static map

ANSWER: Dynamic shadows are written into `dynShadowFBO_` (a completely separate FBO
and texture from the static `shadowFBO_`). The dynamic shadow pass writes per-frame
(txmmgr.cpp:1589-1621) using gos_BeginDynamicShadowPass() / gos_EndDynamicShadowPass(),
which bind dynShadowFBO_, not shadowFBO_. The shader samples both independently
(calcShadow() from shadowMap on tex unit 9; calcDynamicShadow() from dynamicShadowMap
on tex unit 10 per gameos_graphics.cpp:4390). Compositing is additive in the fragment
shader: the minimum of both shadow terms is applied.

Adding decorative geometry to the STATIC pass does not affect the dynamic pass. No
ordering constraint is introduced; the two FBOs are independent.

#### Q4: Mission-reload behavior

ANSWER: On mission reload within the same process, the static shadow map is NOT reset
between missions at the gosPostProcess level (no per-mission initShadows call exists).
However, `gos_RequestFullShadowRebuild()` is called by the first-frame terrain latch
(txmmgr.cpp:1517) when `s_terrainShadowPrimed` is false. Since `s_terrainShadowPrimed`
is a file-scope static bool that starts false at process start and is never reset, it
does NOT re-trigger on mission reload within the same process.

IMPLICATION FOR DECORATIVE BAKE: the decorative-shadow bake must fire at the same
time as the static terrain first-frame bake. If decoratives are baked in the same
Shadow.StaticAccum conditional, they will bake on the first terrain accumulation
frame of the FIRST mission only. On subsequent missions within the same process, the
shadow map will retain terrain depth from the prior mission until the accumulation
threshold fires, but the static light matrix is not rebuilt (staticLightMatrixBuilt_
remains true after first mission, per gos_postprocess.cpp:1198 early-return).

CROSS-SLICE PREREQUISITE CP-1 (new, recorded here):
The Shadow.StaticAccum path's per-mission reset logic is incomplete: `s_terrainShadowPrimed`
and `staticLightMatrixBuilt_` are process-scoped statics that do not reset between
missions. For the decorative-shadow bake to work correctly on all missions (not just
the first), a per-mission shadow reset must fire on mission start. The existing
`gos_RequestFullShadowRebuild()` + `gos_ClearShadowRebuildPending()` mechanism already
provides a forced-rebuild path; what is missing is a mission-transition hook that calls
`gos_RequestFullShadowRebuild()` AND resets `staticLightMatrixBuilt_` (via
`initShadows()` or an explicit `staticLightMatrixBuilt_ = false` + shadowFBO clear)
at the start of each mission. This is a precondition for Plan 2's decorative-bake
implementation. It must be resolved before the decorative shadow bake is wired in.
Recorded as cross-slice prerequisite CP-1: "per-mission static shadow reset."

NOTE: this precondition may already be satisfied if the mission-start path calls
gos_RequestFullShadowRebuild() elsewhere; grep of gos_RequestFullShadowRebuild callers
in code/*.cpp and mclib/*.cpp was not performed in this task and must be done in Plan 2
before assuming CP-1 is open.

#### Q5: Destruction semantics -- fallen/destroyed decorative's baked shadow

ANSWER: PERSIST FOR MISSION. Explicit design choice, justified below.

A fallen decorative tree (OBJECT_FLAG_FALLING set at terrobj.cpp:386; tangibility
cleared at terrobj.cpp:393) has its baked shadow depth persist in the static shadow
map for the remainder of the mission. The shadow depth is NOT patched or removed on
deregister/fall.

JUSTIFICATION:
  (a) The static shadow map is a depth texture. Removing a single object's depth
      contribution requires re-rendering the full static pass without that object
      (no "erase" operation exists in standard OpenGL depth-texture semantics), or
      introducing a separate stencil/mask layer. This adds non-trivial complexity.

  (b) Trees (TREE/TERRAINOBJECT) cast NO shadow today via either route (grep-verified
      above: Route A renderShadows NO-OP; Route B SetAlphaTest(true) excludes from
      eligibleForDynamicShadow). Once the CP-2 BUILD-FIRST infrastructure exists and
      a decorative bake fires, the baked shadow corresponds to the tree's upright
      silhouette. After a tree falls, the static baked shadow for the upright tree
      becomes stale. However, the stale shadow is no worse than the current state
      (no shadow at all), and is only a subtle depth artifact at the former tree
      location. The fall-object re-admitted to the dynamic path (Blocker 5) casts no
      NEW dynamic shadow either (trees are alpha-test, excluded from addShadowShape).
      The overall shadow state at a fallen tree site: upright-bake orphan persists,
      dynamic path casts nothing -- which is better than the pre-bake state of no
      shadow at all.

  (c) Consistency with Blocker 3 tombstone design: the collision proxy entry is
      tombstoned (proxyEntry.tangible = false), not deleted. The same philosophy
      applies to shadow: the static shadow depth is tombstoned (orphaned) rather than
      actively removed. The object's shadow footprint is small and immobile.

  (d) The spec's static-elimination motivation is per-frame CPU cost, not pixel-
      perfect shadow correctness for falling trees. The stale baked shadow after tree
      fall is an explicitly accepted visual artifact for this design.

DESTRUCTION RULE: A fallen/destroyed decorative's baked static shadow PERSISTS for
the mission. The static shadow map FBO is not modified on fall/destroy. The per-frame
dynamic shadow pass (mclib/txmmgr.cpp:1589-1621) writes into dynShadowFBO_ and is
unaffected. No patch, no erase, no re-render. Explicit acceptance.

DEREGISTER HOOK INTERACTION (Blocker 3, terrobj.cpp:393):
The Blocker 3 deregister hook (clears proxyEntry.tangible at terrobj.cpp:393, same
callsite as setTangible(false)) has no shadow-map counterpart. It fires; the proxy
entry is tombstoned; the shadow bake is left intact. These are independent subsystems.
No conflict.

---

### Step 3: Exit criterion

PASS (with BUILD-FIRST verdict on shadow infrastructure dependency).

Evidence:

Step 1: Static shadow map infrastructure is confirmed present with opposite-direction
  grep evidence. The "does not exist" claim from plan-write is refuted by:
  - gosPostProcess::initShadows() (gos_postprocess.cpp:1073): FBO + 4096x4096 depth
    texture allocated at renderer creation.
  - gosPostProcess::buildStaticLightMatrix() (gos_postprocess.cpp:1194): world-fixed
    ortho matrix built once, stored in staticLightSpaceMatrix_[16].
  - gos_BeginShadowPrePass / gos_DrawShadowBatchTessellated / gos_EndShadowPrePass
    (gameos_graphics.cpp:6269-6273, 6282): complete write API, called from
    txmmgr.cpp:1525-1587 Shadow.StaticAccum scope every accumulation frame.
  - shaders/include/shadow.hglsl:4-77: calcShadow() consumes shadowMap (the static
    depth texture bound to unit 9), using staticLightSpaceMatrix_ as lightSpaceMatrix.
  HOWEVER: the Shadow.StaticAccum loop (txmmgr.cpp:1556-1581) filters MC2_DRAWSOLID |
  MC2_ISTERRAIN and submits only via gos_DrawShadowBatchTessellated (terrain vertex
  arrays). No path exists to submit decorative TG_Shape vb/ib/vdecl into the static
  map; that entry point must be built.
  VERDICT: BUILD-FIRST -- FBO/matrix/consumer chain EXISTS; decorative-mesh static-
  caster submission path DOES NOT exist (CP-2).

In-scope (TREE/TERRAINOBJECT) no-shadow-today proof (both routes grep-verified):
  - Route A: TreeAppearance::renderShadows() (bdactor.cpp:4703-4707) early-returns
    under gos_IsTerrainTessellationActive() (gameos_graphics.cpp:6223). NO-OP.
  - Route B: treeShape[i]->SetAlphaTest(true) at bdactor.cpp:3627, 3650, 3669 sets
    alphaTestOn=true on all tree TG_Shapes. eligibleForDynamicShadow (tgl.cpp:3057-3059)
    requires !alphaTestOn -> EXCLUDED. addShadowShape never called for trees.
  Both routes confirm: TREE/TERRAINOBJECT decoratives cast NO shadow today (static or
  dynamic). Shadow bake is ADDITIVE. No regression risk from severance.

Out-of-scope follow-on obligation (buildings): BldgAppearance has no SetAlphaTest(true)
  anywhere in bdactor.cpp (grep-verified). Opaque buildings satisfy eligibleForDynamicShadow
  and DO cast real dynamic shadow today via Route B. The building follow-on slice MUST
  treat shadow severance as a regression risk, not additive. Not relevant to this slice.

Step 2: All five lifecycle questions answered, no TBD.
  Q1 (allocation): process-lifetime (one FBO, not per-mission).
  Q2 (bake order): in-pass with terrain, same Shadow.StaticAccum frame, terrain first;
     scoped to TREE/TERRAINOBJECT; buildings deferred to follow-on.
  Q3 (dynamic compositing): independent FBO (dynShadowFBO_); no ordering constraint.
  Q4 (mission reload): CP-1 cross-slice prerequisite recorded (per-mission reset may
     be missing; grep of gos_RequestFullShadowRebuild callers deferred to Plan 2).
  Q5 (destruction): PERSIST FOR MISSION. Explicit acceptance. Justified above.

Step 3 exit verdict:
  - In-scope (TREE/TERRAINOBJECT) no-shadow-today: PROVED via both routes, grep-cited
  - Static shadow map infrastructure: EXISTS (FBO/matrix/consumer confirmed)
  - Decorative-mesh static-caster submission: DOES NOT EXIST -- CP-2 (BUILD-FIRST)
  - All five lifecycle questions: ANSWERED with no TBD
  - CP-1 recorded (per-mission static shadow reset)
  - CP-2 recorded (decorative-mesh static-caster submission entry point)
  - Out-of-scope follow-on obligation for buildings recorded
  - No fabricated clean answer: both prerequisites explicitly surfaced

---

### Cross-slice prerequisites from this task

#### CP-1: per-mission static shadow reset

Discovered in Step 2 Q4.

`s_terrainShadowPrimed` (mclib/txmmgr.cpp:1510) and `staticLightMatrixBuilt_`
(gosPostProcess, gos_postprocess.cpp:1198 early-return) are process-scoped statics
that do not automatically reset between missions. If a process runs two missions back-
to-back, the second mission's first-frame static shadow bake depends on whether the
forced-rebuild path fires. The existing `gos_RequestFullShadowRebuild()` mechanism
provides the forcing signal, but the CALLER that fires it on mission transition must
exist.

Required: before Plan 2 wires in the decorative shadow bake, confirm or add a
per-mission shadow reset that:
  (a) calls gos_RequestFullShadowRebuild() at mission load time
  (b) resets staticLightMatrixBuilt_ (via pp->initShadows() or direct field reset)
      so buildStaticLightMatrix() fires fresh with the new mission's map extent and
      sun direction

This is a Plan 2 sequencing precondition analogous to OB-1 (severance must survive
save-game restore). It does not invalidate the BUILD-FIRST verdict -- the FBO/matrix/
consumer infrastructure exists. It adds one confirmed implementation step to Plan 2:
verify/add mission-start shadow reset.

Grep to discharge at Plan 2 time:
  rg -n "gos_RequestFullShadowRebuild|staticLightMatrixBuilt\|initShadows" code/*.cpp
     mclib/*.cpp GameOS/gameos/*.cpp
If a mission-start caller already fires gos_RequestFullShadowRebuild + resets
staticLightMatrixBuilt_ in the mission load path, CP-1 is automatically discharged.
If not, add it to the same mission-load function that calls the objBlockInfo severance
pass (OB-1 shared function).

#### CP-2: decorative-mesh static-caster submission entry point

Discovered in Step 1 Shadow.StaticAccum analysis.

The Shadow.StaticAccum loop (txmmgr.cpp:1556-1581) submits only terrain patches via
`gos_DrawShadowBatchTessellated`, which is a terrain-specific API (tessellated vertex
node arrays, not generic TG_Shape vb/ib/vdecl). There is no existing path to submit
a decorative TG_Shape mesh into the static shadow FBO.

This is new infrastructure analogous to the dynamic pass's object-batch submission
(addShadowShape + dynShadowFBO_), but targeting the static FBO and light matrix. The
new entry point must:
  (a) Accept TG_Shape vb/ib/vdecl (or a decorative instance SSBO equivalent)
  (b) Bind the static shadowFBO_ (not dynShadowFBO_) and the staticLightSpaceMatrix_
  (c) Be callable from within the Shadow.StaticAccum accumulation frame, after terrain
      submission, before gos_EndShadowPrePass()
  (d) Not introduce a per-frame re-submission loop (decoratives bake once per
      mission-reset, not per camera-move frame)

This is a Plan 2 implementation prerequisite. The decorative shadow bake step in Plan 2
cannot be wired in until this entry point exists. It is characterized as a BUILD-FIRST
precondition (FBO/matrix/consumer EXISTS; submission path DOES NOT).

Recorded as the primary reason for the BUILD-FIRST verdict. CP-1 (per-mission reset)
is a secondary sequencing constraint that must also be resolved.

---

## Blocker 5: same-frame deregister ordering

Date: 2026-05-18
Status: PASS with one ordering constraint (OC-1) that Plan 2 must enforce.

All file:line citations grep-verified at write time.

---

### Step 1: Grep-verified per-frame sequence

#### Grep runs executed

Frame-loop entry points:
  rg -n "DoGameLogic|draw_screen|UpdateRenderers|Environment.DoGameLogic"
     GameOS/gameos/gameosmain.cpp

Mission logic sequence:
  rg -n "eye->update|ObjectManager->update|ObjectManager->updateCollisions|Mission::update"
     code/mission.cpp

Collision -> deregister path:
  rg -n "OBJECT_FLAG_FALLING|setTangible|handleCollision|TERROBJ_TREE"
     code/terrobj.cpp

Registry / cull / draw sequence:
  rg -n "frameBegin|GpuStaticPropRegistry::flush|compute_dispatch|GpuStaticPropBatcher"
     code/gamecam.cpp mclib/txmmgr.cpp

TerrainObject::update FALLING check:
  rg -n "OBJECT_FLAG_FALLING|ownerForcesDynamic|appearanceClaimsStatic|invalidateStaticRegistration"
     code/terrobj.cpp

#### Verified frame sequence (ordered, with file:line)

The game main loop (gameosmain.cpp main loop) calls two distinct phases per frame
in this order:

PHASE A -- DoGameLogic (gameosmain.cpp:1025, Environment.DoGameLogic = mechcmd2.cpp:2139):
  -> mission->update() (mechcmd2.cpp:2241, = mission.cpp:263 Mission::update)
     Inside Mission::update():
     A1. eye->update()                      (mission.cpp:476)
     A2. land->geometry()                   (mission.cpp:505)
     A3. ObjectManager->update(t,t,t)       (mission.cpp:510, = objmgr.cpp:1884
                                             GameObjectManager::update)
         For each terrain object in active blocks: TerrainObject::update()
         (code/terrobj.cpp:715-752). This checks OBJECT_FLAG_FALLING
         (terrobj.cpp:716 ownerForcesDynamic) and calls
         appearance->invalidateStaticRegistration() if falling (terrobj.cpp:728).
         Since severed decoratives are NOT in objBlockInfo ranges, this step
         does NOT run for severed decoratives.
     A4. ObjectManager->updateCollisions()  (mission.cpp:535)
         -> CollisionSystem::checkObjects() (collsn.cpp:445)
         -> objList[i]->handleStaticCollision() (collsn.cpp:468,505)
         -> BattleMech::handleStaticCollision (mech.cpp:1103) or other mover
         -> ObjectManager->detectStaticCollision (mech.cpp:1139, etc.)
         -> collisionSystem->detectStaticCollision (objmgr.cpp:2853)
         -> resolves to TerrainObjectType::handleCollision (terrobj.cpp:345)
         -> TERROBJ_TREE branch (terrobj.cpp:369):
              terrobj.cpp:385: guard (!FALLEN && !FALLING)
              terrobj.cpp:386: tree->setFlag(OBJECT_FLAG_FALLING, true)
              terrobj.cpp:393: tree->setTangible(false)
              [Plan 2 insertion point: StaticDecorativeSet::deregister(handle)
               MUST be called here, at terrobj.cpp:393, same callsite as
               setTangible(false) -- see OC-1 below]

PHASE B -- draw_screen (gameosmain.cpp:1042):
  -> UpdateRenderers() (mechcmd2.cpp:698)
  -> mission->render() (mechcmd2.cpp:738, = mission.cpp:770 Mission::render)
  -> eye->render() (mission.cpp:784, = GameCamera::render() gamecam.cpp)
     Inside GameCamera::render():
     B1. GpuStaticPropRegistry::frameBegin()   (gamecam.cpp:201)
         Clears s_liveRangeIndices (gos_static_prop_registry.cpp:227;
         :226 is the !s_enabled guard).
         [For StaticDecorativeSet: analogously resets per-frame GPU cull state]
     B2. land->render()                         (gamecam.cpp:202) -- terrain
     B3. ObjectManager->render(t,t,t)           (gamecam.cpp:213)
         For each object in active objBlockInfo ranges: calls render().
         Re-admitted fall objects (re-entered objBlockInfo via OC-1) render here
         via the dynamic path (OBJECT_FLAG_FALLING is set; IsStaticNow() returns
         false because ownerForcesDynamic is set; the registry entry is tombstoned
         so the static path is NOT taken). Severed decoratives that are NOT
         re-admitted are not in the ranges and do NOT render here.
     B4. mcTextureManager->renderLists()        (gamecam.cpp:245)
         Inside renderLists() (txmmgr.cpp:1332), Render.GpuStaticProps scope
         (txmmgr.cpp:1753):
         B4a. GpuStaticPropRegistry::flush()    (txmmgr.cpp:1763)
              Iterates s_liveRangeIndices; tombstoned entries (count==0) are
              skipped (gos_static_prop_registry.cpp:327 tombstone guard).
              [For StaticDecorativeSet: analogous flush checks instance SSBO
               liveness field; tombstoned instance is not submitted]
         B4b. batcher_prepareBaseInstanceTable() (txmmgr.cpp:1768)
         B4c. gpu_cull::compute_dispatch()       (txmmgr.cpp:1778)
              Cull compute shader reads the instance SSBO (including liveness).
              [For StaticDecorativeSet: cull shader reads liveness field; zero
               liveness = instance culled, writes instanceCount=0 in indirect
               command buffer]
         B4d. GpuStaticPropBatcher::instance().flush() (txmmgr.cpp:1781)
              Calls glMultiDrawElementsIndirect. Tombstoned/zero-count command
              draws nothing.

Phase A runs entirely before Phase B in the same frame (gameosmain.cpp:1025 before :1042).
Within Phase A, ObjectManager->updateCollisions() (A4) runs AFTER
ObjectManager->update() (A3) but entirely within DoGameLogic, before draw_screen begins.

---

### Step 2: No-double-tree / no-gap sequence

The no-double-tree guarantee is grounded in the Phase A / Phase B ordering above.

#### Exact ordered sequence on the hit frame

Step 1 (Phase A, A4, terrobj.cpp:393): Hit detected. OBJECT_FLAG_FALLING set
  (terrobj.cpp:386) and setTangible(false) (terrobj.cpp:393). These happen during
  ObjectManager->updateCollisions() in Phase A, before Phase B begins.

Step 2 (Plan 2 insertion, same callsite terrobj.cpp:393):
  StaticDecorativeSet::deregister(handle) is called immediately after
  setTangible(false). It writes the tombstone (liveness field = 0) into the
  CPU-side instance SSBO mapping for this instance. This is the ONLY write that
  must happen at this callsite (see OC-1 for SSBO visibility constraint).
  The object is simultaneously re-admitted to the dynamic path: its block-range
  effective count is restored (HC-2 severance reversed for this instance only),
  so ObjectManager->render() can reach it in Step 5.

Step 3 (Phase B, B3): ObjectManager->render() -- the re-admitted fall object
  renders via the dynamic path. OBJECT_FLAG_FALLING is set; IsStaticNow() returns
  false; the object's registry entry is tombstoned (count=0) so even if it were
  still in the registry, flush() would skip it. The dynamic render enqueues its
  mesh into the master arrays at the current (upright) pose. This is the one and
  only draw of this object this frame.

Step 4 (Phase B, B4a): GpuStaticPropRegistry::flush() / StaticDecorativeSet flush.
  The tombstoned instance (liveness=0) is skipped. It is NOT submitted to the
  batcher or the cull-compute substrate.

Step 5 (Phase B, B4c): compute_dispatch(). The cull compute shader reads the
  instance SSBO. The tombstoned instance has liveness=0; the shader writes
  instanceCount=0 for that slot in the indirect command buffer.

Step 6 (Phase B, B4d): glMultiDrawElementsIndirect. The tombstoned instance's
  indirect command has instanceCount=0 and draws nothing. Only the dynamic fall
  object (enqueued in Step 3) is visible.

#### Which step provides the guarantee

The guarantee is provided by the Phase A / Phase B ordering:
  - Steps 1-2 (tombstone write + re-admit) complete during Phase A
    (DoGameLogic -> ObjectManager->updateCollisions(), mission.cpp:535)
  - Steps 3-6 (dynamic draw + static skip) run during Phase B (draw_screen)
  - Phase A completes before Phase B begins (gameosmain.cpp:1025 before :1042)

Therefore: on the frame the hit lands, the tombstone is written in Phase A and is
visible to all of Phase B. The dynamic object is re-admitted in Phase A and rendered
in Phase B Step 3. The static indirect draw (Phase B Step 6) skips the tombstoned
instance. The screen shows exactly one representation -- the dynamic fall object at
its upright pose, identical to what the static path would have drawn, with the fall
animation starting on the following frame (when ObjectManager->update() processes
OBJECT_FLAG_FALLING for the re-admitted object).

Never both: the static instance is tombstoned (Steps 4-6 skip it) in the same Phase B
where the dynamic object draws (Step 3). They are mutually exclusive by construction.

Never neither: the dynamic fall object is re-admitted to objBlockInfo in Step 2 (Phase A)
before ObjectManager->render() runs in Step 3 (Phase B). It renders this frame.
The static instance would have drawn via the indirect path (Phase B Steps 4-6); since it
is now tombstoned, only the dynamic object draws. There is no frame where neither draws.

---

### Step 3: Tombstone/generation mechanics

#### Tombstone field

Name: liveness field (per-instance flag) within the instance SSBO.
Width: 1 byte (uint8 packed into the per-instance record, or 1 uint32 with value 0/1
for alignment; exact encoding is a Plan 2 C++/GLSL struct lockstep decision per
cpp_glsl_ubo_struct_lockstep).
Location: the instance SSBO (per design spec Section 4: "Liveness/generation/tombstone
state is the ONLY mutable GPU-resident field"). The SSBO layout places this field
adjacent to the immutable geometry/LOD/material fields baked at mission load. Only
deregister(handle) may write it; all other SSBO fields are immutable post-bake.

#### How the cull-compute reads it

The cull compute shader iterates over the instance SSBO. For each instance it checks
the liveness field. If liveness == 0, the instance is skipped: the shader writes
instanceCount = 0 for that slot in the indirect command buffer. This is the tombstone
predicate -- no geometry is dispatched for tombstoned instances.

#### Generation field

Width: 1 uint32 per instance slot, stored alongside liveness in the SSBO.
Purpose: prevents stale handle references after slot reuse. A deregister call
increments the generation for that slot; any subsequent collision-proxy lookup that
presents the old (handle, generation) pair gets rejected.
Initial value: 0 at mission load. For static decoratives (which never leave objList,
per HC-3), the generation is never incremented during a mission; the generation field
is a defensive guard for slot-reuse at mission reload or future feature work. It carries
zero per-frame cost.
Reuse policy: mission-lifetime. Slot assignments are fixed at mission load and never
compacted (HC-1 invariant). A deregistered decorative's slot is tombstoned (liveness=0)
and its generation is incremented; the slot is NOT reassigned during the mission.
The tombstoned slot persists as a zero-draw no-op for the remainder of the mission.
This is consistent with the HC-3 "object still exists in objList" policy: the SSBO
slot mirrors the objList slot's existence.

#### Tie to Blocker 3 deregister hook (terrobj.cpp:393)

Blocker 3 established: proxyEntry.tangible is cleared at terrobj.cpp:393, the same
callsite as setTangible(false), making collision-proxy deregister and render-instance
deregister atomic at one callsite.

The StaticDecorativeSet::deregister(handle) SSBO liveness write is added to the SAME
callsite at terrobj.cpp:393. The three operations at this single callsite become:
  (a) tree->setTangible(false)                   (existing, terrobj.cpp:393)
  (b) proxyEntry.tangible = false                (Blocker 3, same callsite)
  (c) StaticDecorativeSet::deregister(handle)    (this blocker, same callsite)
       -- writes liveness=0 in SSBO
       -- increments generation for that slot
       -- re-admits the object to dynamic objBlockInfo range

All three fire atomically at the same callsite, guarded by the existing
!FALLEN && !FALLING idempotence check (terrobj.cpp:385). The guard ensures
deregister is called exactly once per tree per mission.

SSBO visibility at deregister time: The liveness write in deregister(handle) writes
to the CPU-side view of the instance SSBO. This write must be GPU-visible before
compute_dispatch() (txmmgr.cpp:1778) reads the SSBO in Phase B. This is satisfied by
the Phase A / Phase B ordering (deregister fires in Phase A at mission.cpp:535; compute
fires in Phase B at txmmgr.cpp:1778, which is a separate draw_screen call after
DoGameLogic completes). The mechanism for visibility is OC-1 below.

---

### Step 4 (Exit criterion)

#### Ordering constraint OC-1 (Plan 2 must enforce)

CONSTRAINT: StaticDecorativeSet::deregister(handle) must write the instance SSBO
liveness field using a mechanism that guarantees GPU visibility BEFORE
compute_dispatch() (txmmgr.cpp:1778) reads that SSBO in the same frame.

INSERTION POINT: terrobj.cpp:393 (same callsite as setTangible(false), Phase A).

MECHANISM CHOICE (Plan 2 decision, two options):
  Option A (preferred): Use a coherent persistent-mapped BAR SSBO for liveness.
    CPU writes to the mapped liveness field are immediately visible to the GPU
    without an explicit glBufferSubData or glMemoryBarrier call. This is the
    lowest-latency path and matches the "no readback / no sync stall" design
    contract (spec Section 5: honors substrate_coalesce_sync_point_lesson).
  Option B (fallback): Use glBufferSubData for the liveness field in deregister().
    The upload must happen before compute_dispatch(). Since deregister fires in
    Phase A and compute_dispatch fires in Phase B, the Phase A / Phase B ordering
    already provides the necessary sequencing. An explicit glMemoryBarrier
    (GL_SHADER_STORAGE_BARRIER_BIT) before compute_dispatch() (txmmgr.cpp:1778)
    ensures coherence. Plan 2 must add this barrier if not already present.

Either option satisfies OC-1. The choice is a Plan 2 implementation decision.
The ordering guarantee (Phase A before Phase B) is structural and holds regardless
of which option is chosen.

DYNAMIC RE-ADMIT insertion point (Plan 2 must also enforce):
  Also at terrobj.cpp:393 callsite: StaticDecorativeSet::deregister(handle) must
  restore the object to the per-block effective count (reverse the HC-2 severance
  for this slot only) so ObjectManager->render() (B3) can reach it this frame.
  The re-admit must happen BEFORE Phase B begins, i.e., within Phase A at the
  deregister callsite. Since the callsite is terrobj.cpp:393 in Phase A, this
  constraint is satisfied by construction if the re-admit is part of deregister().

#### Verdict

PASS.

The Phase A / Phase B ordering provides a written, grep-verified guarantee that for
ANY frame where the hit lands:
- The tombstone write (liveness=0) happens in Phase A (mission.cpp:535 via terrobj.cpp:393)
  before the cull-compute and indirect draw in Phase B (txmmgr.cpp:1778, :1781).
- The dynamic fall object is re-admitted in Phase A (terrobj.cpp:393 deregister callsite)
  before ObjectManager->render() in Phase B (gamecam.cpp:213).
- The draw shows exactly one representation: the dynamic fall object (Phase B B3),
  not the static GPU-resident instance (skipped by tombstone in Phase B B4a-B4d).
- Never both: static instance is tombstoned before indirect draw.
- Never neither: dynamic fall object is re-admitted before render.

The only un-resolved item is the SSBO liveness write mechanism (OC-1, Option A vs B),
which is a Plan 2 implementation choice, not a blocker. The ordering guarantee holds
under both options. OC-1 is NOT an OPEN blocker; it is a specification precondition
for Plan 2.

No new OPEN blockers from this task.
