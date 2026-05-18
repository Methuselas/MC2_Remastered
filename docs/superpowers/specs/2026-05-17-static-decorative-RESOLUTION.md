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
