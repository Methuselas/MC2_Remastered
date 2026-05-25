# Static Decorative Elimination - objList Consumer Boundary Proof

Date: 2026-05-17
Branch: claude/nifty-mendeleev
Stage: Stage 0 (pre-code investigation)
Status: INCOMPLETE -- see CAN-REACH-UNCOVERED blockers below

All file:line citations were grep-verified at write time per Documentation Discipline.

---

## Step 1: Resolver Inventory

The following resolvers can return a TerrainObject (including decoratives) by
handle or index. Grepped with:

  rg -n "GameObjectManager::get|getObjectFromHandle|getObject\(|objList\[|
         getTerrainObject|getByHandle" code/objmgr.cpp code/objmgr.h

### Primary handle resolver

  GameObjectManager::get(GameObjectHandle handle)
  -- code/objmgr.cpp:2181-2187
  -- Returns objList[handle] for any valid handle 1..getMaxObjects().
  -- CAN reach a decorative if the decorative still occupies a slot in objList.

### Typed terrain-object accessor

  GameObjectManager::getTerrainObject(long terrainObjectIndex)
  -- code/objmgr.cpp:797-803
  -- Returns terrainObjects[index], NOT via objList.
  -- This accessor survives severance; it returns from the typed array.
  -- Post-severance decoratives removed from objList but still in terrainObjects[]
     would still be reachable here (see Consumer 4/ABL analysis below).

### Block-based resolvers (indirect via objList + ObjBlockInfo)

  GameObjectManager::getObjBlockObject(long blockNumber, long objLocalIndex)
  -- code/objmgr.h:426-428
  -- Returns objList[Terrain::objBlockInfo[blockNumber].firstHandle + objLocalIndex]
  -- CAN reach any object in the block range, including decorative non-collidables,
     if firstHandle/numObjects/numCollidableObjects are not adjusted for severance.

  GameObjectManager::get(terrainObjHandle + i) inside handleStaticCollision
  -- code/mech.cpp:1121, code/gvehicl.cpp:806
  -- Uses getObjBlockNumCollidables + getObjBlockFirstHandle.
  -- Decorative trees enter the collidable section at load time:
     objmgr.cpp:1366-1367 places TREE/TERRAINOBJECT at curCollidableHandle.
  -- CAN reach decorative trees via collision block traversal.

### Full-scan resolvers

  findByPartId(long partId)
  -- code/objmgr.cpp:2363-2376
  -- Linear scan over objList[1..getMaxObjects()], matching partId.
  -- CAN reach any decorative still in objList by its partId.

  findByCellPosition(long row, long col)
  -- code/objmgr.cpp:2390-2411
  -- Linear scan over objList matching cell coordinates.
  -- CAN reach decorative if still in objList.

  findObjectByMouse(long mouseX, long mouseY, ...)
  -- code/objmgr.cpp:2425-2485
  -- Linear scan over a searchList slice of objList.
  -- Explicitly excludes TREE class (objmgr.cpp:2463, 2471-2472) but still
     reads objList entries; TERRAINOBJECT subtype rocks are NOT excluded.

  Save() iteration
  -- code/objmgr.cpp:3257-3280
  -- Iterates objList[0..getMaxObjects()], saves every non-NULL slot.
  -- CAN reach any decorative still in objList.

### Absent resolvers

  getObjectFromHandle, handleToObject, resolveHandle, getByPartId, getByHandle:
  none of these symbol names appear in code/*.cpp or mclib/*.cpp (grep confirmed
  in Step 2, Consumer 13 below). The generic handle resolver is GameObjectManager::get.

---

## Step 2: Per-consumer classification (13 classes)

---

### Class 1: render/update

Grep command:
  rg -n "objList\[|->update\(\)|->render\(" code/objmgr.cpp code/gamecam.cpp

Hits (render path):
  code/objmgr.cpp:1695-1714 -- render() loop: iterates all numObjects per block
    via Terrain::objBlockInfo[terrainBlock].firstHandle and numObjects.
    objList[objIndex]->render() at objmgr.cpp:1706.
  code/objmgr.cpp:1830-1844 -- renderShadows() loop: same block-range iteration,
    objList[objIndex]->renderShadows() at objmgr.cpp:1837.

Hits (update path):
  code/objmgr.cpp:2005-2036 -- update() loop: same block-range iteration,
    objList[objIndex]->update() at objmgr.cpp:2017.
    MC2_DESTROY at objmgr.cpp:2023 on update_false.
    emitGpuCullRecord at objmgr.cpp:2031.

Verdict: CAN-REACH-UNCOVERED (OPEN HARD BLOCKER #1)

The render, renderShadow, and update loops all use
  Terrain::objBlockInfo[terrainBlock].firstHandle
  Terrain::objBlockInfo[terrainBlock].numObjects
to iterate. If decoratives remain in objList and their block ranges are
untouched, ALL three loops reach them every frame. This is exactly the
per-frame coupling the spec targets for elimination. Severance requires
removing decoratives from objList AND adjusting or replacing block range
metadata so these loops cannot resolve them.

---

### Class 2: collision

Grep command:
  rg -n "collid|OBB|lineOfFire|getCollision|terrainBlock.*Object"
     mclib/*.cpp code/*.cpp

Key hits:

  code/collsn.cpp:458 -- CollisionSystem::checkObjects uses
    ObjectManager->getCollidableList(objList).
    getCollidableList (objmgr.cpp:2797-2821) builds a separate list:
    mechs + vehicles + elementals + turrets + gates + carnage + artillery.
    TerrainObjects/trees are NOT in collidableList. CANNOT-REACH via this path.

  code/mech.cpp:1115-1139 -- BattleMech::handleStaticCollision:
    numCollidables = ObjectManager->getObjBlockNumCollidables(blockNumber)
    terrainObjHandle = ObjectManager->getObjBlockFirstHandle(blockNumber)
    for i in range(numCollidables):
      GameObjectPtr terrainObj = ObjectManager->get(terrainObjHandle + i)
    switch case TREE/TERRAINOBJECT at mech.cpp:1125-1127.
    Decorative trees are loaded into the collidable section of their block
    (objmgr.cpp:1366-1367: objList[curCollidableHandle] = obj for TREE/TERRAINOBJECT).

  code/gvehicl.cpp:800-825 -- GroundVehicle::handleStaticCollision: identical
    pattern using getObjBlockNumCollidables + getObjBlockFirstHandle + get().

Verdict: CAN-REACH-UNCOVERED (OPEN HARD BLOCKER #2)

handleStaticCollision (mech and vehicle) reaches decorative trees via
getObjBlockFirstHandle + ObjectManager->get(). The "collidable" section
of each block includes trees/terrain objects. Severance plan MUST either:
(a) move decoratives out of the collidable section and adjust
    numCollidableObjects so mech/vehicle handleStaticCollision cannot index them,
    or
(b) maintain a separate collision proxy for decoratives that does not route
    through objList.
This maps to spec Blocker 3 (collision proxy ownership).

---

### Class 3: damage

Grep command:
  rg -n "applyDamage|handleWeaponHit|takeDamage|destroy\("
     code/terrobj.cpp mclib/objappear*.cpp

Hits:
  code/terrobj.cpp:364 -- TerrainObjectType::handleCollision -> collidee->handleWeaponHit
  code/terrobj.cpp:412,416 -- TerrainObjectType::handleCollision wall-light branch
  code/terrobj.cpp:1045 -- TerrainObject::handleWeaponHit (the actual handler)
  code/terrobj.cpp:1072,1077 -- markLOS calls inside handleWeaponHit destruction path
  code/terrobj.cpp:1129,1134 -- TREE destruction branch in handleWeaponHit
  No hits in mclib/objappear*.cpp (no such files match the glob).

Analysis:
  handleWeaponHit on a TerrainObject is called from collision dispatch
  (which is already CAN-REACH-UNCOVERED in Class 2). The handler itself
  operates on the already-resolved TerrainObjectPtr -- it does NOT perform
  additional objList lookups. The damage path does not independently reach
  decoratives via objList; it is a downstream consequence of the collision
  reach.

Verdict: CAN-REACH-but-covered (conditional on collision being resolved)
  The damage path does not add an independent objList access beyond what
  collision already covers. Fixing Blocker #2 (collision) eliminates the
  damage reach for severed decoratives. The spec's deregister/re-admit model
  handles discrete damage events explicitly. No additional blocker.

---

### Class 4: script triggers (ABL)

Grep command:
  rg -n "getObject|ObjectHandle|partId|getPart" code/abl/*.cpp code/mission.cpp
  (code/abl/ glob returned no files; the ABL script is code/ablmc2.cpp)
  rg -n "TERRAINOBJECT|TERROBJ|getTerrainObject|TerrainObject" code/ablmc2.cpp

Key hits:
  code/ablmc2.cpp:340-353 -- getObject() helper calls ObjectManager->findByPartId(partId)
    which iterates objList (objmgr.cpp:2369-2374). If a decorative's partId is
    supplied to any ABL getObject call and the decorative is still in objList,
    it will be found.

  code/ablmc2.cpp:1467-1477 -- execGetObjects criteria=1 (TERRAIN OBJECTS):
    Calls ObjectManager->getTerrainObject(i) for i in 0..getNumTerrainObjects().
    This uses the typed terrainObjects[] array (objmgr.cpp:797-803), NOT objList.
    Post-severance: if decoratives remain in terrainObjects[] (they would unless
    explicitly removed), this returns their partIds. ABL scripts could then call
    getObject(partId) which routes to findByPartId -> objList scan. If decoratives
    are removed from objList, findByPartId returns NULL; ABL script gets NULL back.

  code/ablmc2.cpp:2882-2895 -- execGetTerrainObjectPartID: computes partId from
    row/col math only; no objList access. Does not reach the object.

  code/ablmc2.cpp:3307,3333 -- obj->isTerrainObject() checks on already-resolved
    objects; no independent objList access.

Verdict: CAN-REACH-UNCOVERED (OPEN HARD BLOCKER #3)
  findByPartId at objmgr.cpp:2369-2374 iterates objList and will find any
  decorative still in objList whose partId matches. ABL scripts use
  getObject(partId) which routes through findByPartId. If decoratives are
  removed from objList but still present in terrainObjects[], execGetObjects
  criteria=1 returns their partIds; getObject(partId) then returns NULL
  (safe). BUT if decoratives remain in objList during any transitional state,
  findByPartId CAN reach them.

  Additionally, ObjectManager->getTerrainObject (the typed accessor) will
  still return decoratives post-severance unless they are also removed from
  terrainObjects[]. The spec does not currently address whether terrainObjects[]
  is trimmed on severance. If scripts call getTerrainObject index directly
  (ablmc2.cpp:1469), they get the pointer regardless of objList removal.
  This is a gap requiring a plan ruling.

---

### Class 5: mission objectives

Grep command:
  rg -n "objective|trigger|getObject" code/mission*.cpp

Hits:
  code/mission.cpp:1001 -- ObjectManager->getObjectType (type lookup, not object)
  code/mission.cpp:3054,3057 -- commented-out getSpecificObjects calls (dead code)
  missiongui.cpp, missionbriefingscreen.cpp -- objective UI, no objList access;
    objectives reference movers and buildings via mover/team APIs, not terrain objects.

No mission objective code iterates objList for TerrainObjects or resolves
decoratives by handle. Objective status checks are on mover/building objects.

Verdict: CANNOT-REACH
  Mission objectives interact with movers and buildings, not terrain
  decoratives. No objList-sourced terrain-object lookups found in mission*.cpp.

---

### Class 6: save/load

Grep command:
  rg -n "save\(|load\(|Chunk|serialize" code/objmgr.cpp code/terrobj.cpp

Key hits:
  code/objmgr.cpp:3257-3280 -- Save() iterates objList[i] for i in 0..getMaxObjects().
    objList[i]->Save(file, packetNum) at objmgr.cpp:3261.
    If decoratives remain in objList, they are saved. If removed, their slots
    are NULL and a blank packet is written (the else branch at objmgr.cpp:3276-3279).

  code/terrobj.cpp:1045-1051 -- TerrainObject::handleWeaponHit uses
    MPlayer->addWeaponHitChunk (multiplayer chunk, not save file).

Verdict: CAN-REACH-UNCOVERED (OPEN HARD BLOCKER #4)
  The Save() loop at objmgr.cpp:3257-3261 iterates all objList slots. If a
  decorative is still in objList at save time, it is serialized. If removed,
  its slot is NULL and a blank packet is written (packetNum incremented at
  objmgr.cpp:3278). The corresponding Load() code must agree on packet layout
  or save files become incompatible.

  The severance plan MUST decide: (a) decoratives stay in objList but are
  flagged as "severed" and save/load treats them specially, OR (b) decoratives
  are removed from objList before any save can occur and blank slots are
  expected, with the loaded-back state re-registering them from the terrain
  file. This maps to spec Section 8 error-handling / killswitch path and the
  stock-install-must-remain-playable constraint. The plan has not specified
  the save/load packet-layout contract for removed slots.

---

### Class 7: selection/targeting

Grep command:
  rg -n "select|target|pickObject|getTarget" code/*.cpp

Key hits:
  code/objmgr.cpp:2425-2485 -- findObjectByMouse iterates a slice of objList.
    At objmgr.cpp:2463: TREE class explicitly excluded from selection
      (obj->getObjectClass() != TREE).
    At objmgr.cpp:2471-2472: TREE and ARTILLERY excluded in non-skipDisabled branch.
    Rock clumps (getDamageLevel() == 36000000) also excluded.
  code/missiongui.cpp:1235 -- calls ObjectManager->findObjectByMouse; result
    is only acted on if it passes the TREE/rock exclusion filters.

  ablmc2.cpp:791 -- getObject(targetId) -> findByPartId -> objList scan.
    Trees are a valid ABL target class in principle (no exclusion at findByPartId).

Verdict: CAN-REACH-but-covered for mouse selection (TREE/rock explicitly excluded).
         CAN-REACH-UNCOVERED via ABL target path (same as Blocker #3 above).
  The mouse-pick path explicitly excludes TREE class and rock clumps at
  objmgr.cpp:2463,2471-2472. No new blocker from selection for trees.
  TERRAINOBJECT subtype "none" (non-tree non-building) is NOT excluded; if any
  decoratives use that subtype they can appear as mouse targets. Requires
  plan confirmation of which subtypes are classified as decorative.
  The ABL target path shares Blocker #3.

---

### Class 8: pathing/blocking

Grep command:
  rg -n "blocked|passable|getMoveCost|setBlocked" mclib/move*.cpp code/*.cpp

Key hits:
  mclib/move.cpp:993,1046-1064 -- setPassable operates on cell data; does not
    access objList. The move map records passable/impassable flags per cell.
  mclib/move.cpp:1114-1153 -- placeTerrainObject under #if 0 (dead code).
  mclib/bdactor.cpp:3325 -- BldgAppearance::markMoveMap uses appearance geometry,
    not objList.
  mclib/bdactor.cpp:4946 -- TreeAppearance::markLOS: appearance-level LOS marking,
    called from terrobj.cpp::handleWeaponHit (damage path).

  No code/*.cpp pathing functions access objList for terrain objects.
  Move-map passability is baked into cell data at mission load via
  MissionMap::setPassable; trees mark impassable cells through their
  appearance's markMoveMap, which is called during init, not per-frame.

Verdict: CANNOT-REACH (per-frame pathing does not access objList for decoratives)
  Passability data is baked into cell data at mission load. Per-frame pathfinding
  reads cell data, not objList. The markMoveMap/markLOS calls are discrete
  (mission-load init and on-destruction). A severed decorative still has its
  cell passability baked; on deregister, the deregister path must call markMoveMap
  to clear the cell. This is a plan obligation, not a per-frame consumer problem.

---

### Class 9: fog/LOS/reveal

Grep command:
  rg -n "LOS|lineOfSight|reveal|fogOfWar|scenario" code/*.cpp mclib/*.cpp

Key hits:
  mclib/bdactor.cpp:3460 -- BldgAppearance::markLOS: appearance-level operation,
    called from bldng.cpp (building destruction/state change), not per-frame scan.
  mclib/bdactor.cpp:4946 -- TreeAppearance::markLOS: called from
    terrobj.cpp:1072,1077,1129,1134 (damage/destruction path only).
  code/gvehicl.cpp:3413 -- getTeam()->markSeen(position, LOSFactor):
    vehicle updating its own team's fog-of-war; position-based, no objList scan.
  code/gvehicl.cpp:3666 -- lineOfSight(curTarget): mover-to-target LOS check;
    operates on already-resolved GameObjectPtr, no objList iteration.

  No fog/LOS code iterates objList for decoratives. LOS cell-height marking
  for trees (bdactor.cpp:3124-3125, 4937-4938) is baked at mission load and
  on destruction via appearance->markLOS(), not per-frame.

Verdict: CANNOT-REACH (per-frame fog/LOS does not scan objList for decoratives)
  LOS contributions are baked into cell data. Per-frame LOS queries are
  position-based or resolve against already-held pointers. markLOS is a
  discrete event (init + destroy), not a per-frame consumer.

---

### Class 10: audio/event emitters

Grep command:
  rg -n "soundSystem|playSound|event.*Object" code/*.cpp

Key hits:
  code/terrobj.cpp:672-673 -- soundSystem->playDigitalSample(TREEFALL, ...) inside
    TerrainObject::update() (tree-fall animation path).
  code/terrobj.cpp:1159 -- soundSystem->playDigitalSample(BREAKINGFENCE, ...) inside
    handleWeaponHit.
  code/artlry.cpp, bldng.cpp, carnage.cpp -- soundSystem calls are on their own
    objects, not on decoratives.

Analysis:
  terrobj.cpp:672-673 is called from within TerrainObject::update(), which is
  reached via the update loop (Class 1, Blocker #1). It does not independently
  access objList; it fires from an already-resolved TerrainObjectPtr.
  Once decoratives are severed from the update loop, this path is unreachable
  by construction (the tree can only fall via the discrete deregister/re-admit
  event at that point).

Verdict: CAN-REACH-but-covered (gated on update loop; resolves with Blocker #1)
  No independent objList iteration for audio. Audio events on trees fire from
  within update() or handleWeaponHit (damage path, itself gated on collision).
  Fixing Blockers #1 and #2 eliminates this reach for severed decoratives.
  The deregister/re-admit path re-admits the object to the dynamic path for its
  fall/death, where audio fires normally.

---

### Class 11: cleanup/destruction

Grep command:
  rg -n "MC2_DESTROY|setExists|removeObject|freeHandle"
     code/objmgr.cpp code/terrobj.cpp

Key hits:
  code/objmgr.cpp:2023 -- MC2_DESTROY(objList[objIndex], "update_false") inside
    the update loop (Class 1). Decoratives reached via objList and destroyed if
    update() returns false.
  code/objmgr.cpp:507,523 -- MC2_DESTROY on pool-unused mechs/vehicles at init.
  code/terrobj.cpp:69 -- comment referencing MC2_DESTROY_TRACE pattern.
  code/terrobj.cpp has no MC2_DESTROY call (single grep hit is a comment).

Analysis:
  The destruction via MC2_DESTROY at objmgr.cpp:2023 is the update-loop path
  (Blocker #1). It would destroy a decorative if update() returns false, which
  is the existing lifecycle mechanism. Post-severance, decoratives must not enter
  the update loop, so this path is eliminated by construction. The spec (Section 5)
  states destruction is event-sourced via deregister, not poll-sourced.

Verdict: CAN-REACH-but-covered (gated on update loop; resolves with Blocker #1)
  MC2_DESTROY on decoratives only fires from the update loop. Severance from the
  update loop eliminates this reach. No independent cleanup path accesses objList
  for decoratives outside the update loop.

---

### Class 12: network/replay

Grep command:
  rg -n "replay|network|serializeFrame|lockstep" code/*.cpp

Hits:
  code/mechcmd2.cpp:1763 -- networking initialization comment only
  code/mech.cpp:3715,8740,8741 -- network play comments/assertions in weapon-fire
    paths (mover-only, not terrain objects)
  code/gvehicl.cpp:5259-5260 -- same pattern (mover weapon-fire)
  code/turret.cpp:1963 -- network play comment (turret-only)
  No hits for serializeFrame or lockstep.

Verdict: CANNOT-REACH
  No replay or frame-serialization infrastructure is present in the codebase
  that accesses objList for terrain objects. Network chunks for terrain objects
  (addWeaponHitChunk at terrobj.cpp:1051) are dispatched from within
  handleWeaponHit, which is already covered under Class 3 (damage).

---

### Class 13: handle-to-object helpers

Grep command:
  rg -n "getObjectFromHandle|handleToObject|resolveHandle|getByPartId"
     code/*.cpp mclib/*.cpp

Hits: No matches found in either code/*.cpp or mclib/*.cpp.

The named helper functions in the grep do not exist. The generic handle
resolver is GameObjectManager::get (objmgr.cpp:2181) and the partId resolver
is GameObjectManager::findByPartId (objmgr.cpp:2363). Both are covered in the
Resolver Inventory (Step 1) and in relevant consumer classes above.

Additional handle-usage hits from the cross-reference search (Step 2, consumers
4/7):
  code/ablmc2.cpp:207 -- ObjectManager->get(Team::sortList->getId(i)) returns
    a mover; cast to MoverPtr. No terrain object.
  code/ablmc2.cpp:584 -- ObjectManager->get(contactList[i]) returns a mover contact.
  code/contact.cpp:184,199,437,445,491 etc -- ObjectManager->get(contacts[i])
    all cast to MoverPtr; contact lists contain only movers.

Verdict: CANNOT-REACH (named helpers absent; generic helpers covered above)
  No independent handle-to-object helper class exists beyond get() and
  findByPartId(), which are already classified in Blockers #1 and #3.
  The generic get() is only a problem when a decorative handle is still in
  objList; removing decoratives from objList makes get() return NULL for their
  former handles.

---

## Step 3: Summary table

  Class                    Verdict                         Blocker
  -----------------------------------------------------------------
  1  render/update         CAN-REACH-UNCOVERED             HARD #1
  2  collision             CAN-REACH-UNCOVERED             HARD #2
  3  damage                CAN-REACH-but-covered           (gated on #2)
  4  script triggers       CAN-REACH-UNCOVERED             HARD #3
  5  mission objectives    CANNOT-REACH                    --
  6  save/load             CAN-REACH-UNCOVERED             HARD #4
  7  selection/targeting   CAN-REACH-but-covered (mouse);  (shares #3 for ABL)
                           CAN-REACH-UNCOVERED (ABL)       (shares Blocker #3)
  8  pathing/blocking      CANNOT-REACH                    --
  9  fog/LOS/reveal        CANNOT-REACH                    --
  10 audio/event emitters  CAN-REACH-but-covered           (gated on #1)
  11 cleanup/destruction   CAN-REACH-but-covered           (gated on #1)
  12 network/replay        CANNOT-REACH                    --
  13 handle helpers        CANNOT-REACH                    --

---

## Step 4: Verdict

INCOMPLETE

Four independent CAN-REACH-UNCOVERED blockers must be resolved before Stage 1
implementation begins. Listed by severity:

HARD BLOCKER #1 -- render/update loop (Class 1)
  objmgr.cpp:1695-1714 (render), 1830-1844 (renderShadows), 2005-2036 (update)
  All three iterate the block range firstHandle..firstHandle+numObjects which
  currently covers decoratives. Severance requires removing decoratives from
  objList slots AND adjusting or replacing block metadata (firstHandle,
  numObjects, numCollidableObjects) so loops cannot index them. This is spec
  Blocker 1 (objList/objBlockInfo removal mechanism) and is the primary
  structural change.

HARD BLOCKER #2 -- collision (Class 2)
  mech.cpp:1115-1139, gvehicl.cpp:800-825
  handleStaticCollision uses getObjBlockNumCollidables + getObjBlockFirstHandle +
  ObjectManager->get(). Decorative trees are in the collidable section of their
  block (objmgr.cpp:1366-1367). Severance must ensure decorative trees are
  removed from the collidable section and numCollidableObjects is adjusted, OR a
  separate collision proxy is used. This is spec Blocker 3 (collision proxy
  ownership). Note: the spec states trees CAN fall (discrete event), so a collision
  proxy for the severed state is required -- the block collidable range cannot
  simply omit trees entirely.

HARD BLOCKER #3 -- script triggers / findByPartId (Classes 4, 7-ABL)
  objmgr.cpp:2363-2376 (findByPartId), ablmc2.cpp:340-353 (getObject helper)
  ABL scripts can resolve any object by partId via findByPartId which scans
  objList. If decoratives are removed from objList, findByPartId returns NULL
  (safe). But the plan must also address whether terrainObjects[] is trimmed:
  ablmc2.cpp:1467-1477 (execGetObjects criteria=1) calls getTerrainObject(i)
  for all i in 0..getNumTerrainObjects() -- this bypasses objList and returns
  decorative pointers from the typed array regardless of objList severance.
  Decorative partIds are then passed to getObject which routes to findByPartId;
  with decoratives removed from objList this returns NULL, which is safe.
  The gap is: must terrainObjects[] also be trimmed to avoid leaking raw
  pointers to severed decoratives? The plan must rule on this. This is spec
  Blocker 2 (generic handle-lookup semantics).

HARD BLOCKER #4 -- save/load (Class 6)
  objmgr.cpp:3257-3280 (Save loop)
  The Save loop iterates objList[0..getMaxObjects()]. If decoratives are removed
  from objList before a save, blank packets are written (packetNum incremented,
  no data). The corresponding Load code must match this layout or save files
  become incompatible with pre-severance saves. The spec does not specify the
  save/load packet-layout contract for removed slots or how savegame backward
  compatibility is maintained. This must be resolved in the plan before Stage 3
  (severance flip). The stock-install-must-remain-playable constraint applies.

---

## Notes on additional partial-reach paths

- findObjectByMouse (objmgr.cpp:2425-2485) explicitly filters TREE class at
  lines 2463 and 2471-2472. Rock clumps are filtered by getDamageLevel check.
  However, TERRAINOBJECT subtype "none" objects are not filtered; if any
  decorative uses that subtype they can appear as mouse pick targets. The plan
  should confirm which object subtypes qualify as decorative.

- getSpecificObjects (objmgr.cpp:1057-1068) scans objList for class+subtype.
  Only call site in code/ is goal.cpp:440 for BUILDING/WALL, not terrain objects.
  Low risk but worth noting: any future caller could scan for TREE/TERRAINOBJECT.

- carnage.cpp:533-538, artlry.cpp:743-751 (getObjBlockNumObjects +
  getObjBlockObject): these use numObjects (all objects including non-collidables),
  not just collidables. They reach decorative trees if they are in the same block.
  This is a secondary collision-path issue within Blocker #2.
