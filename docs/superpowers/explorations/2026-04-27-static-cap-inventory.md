# Static Cap Inventory — MC2 OpenGL (2026-04-27)

Branch surveyed: `terrain-pbr-mod` (root repo + worktrees excluded from search).
Search scope: `mclib/`, `code/`, `Viewer/`, `GameOS/`, `shaders/` under repo root only.
This is a read-only audit. No source files were modified.

---

## Summary

**71 hard-coded capacity limits** catalogued across six subsystem families: texture/geometry, terrain/map, unit/team/game objects, ABL scripting, renderer (MLR), and audio/radio. Class distribution:

| Class | Count | Meaning |
|-------|-------|---------|
| A | 28 | Runtime-only; safe to raise after memory/perf check |
| B | 18 | Pool or queue; instrument before raising |
| C | 7 | GPU/shader/index-width; needs GL audit |
| D | 18 | File-format/savegame/network-packet; do NOT raise without migration |

Key concern: the D-class limits cluster around the **unit/mover ID space** (unitdesg.h). These are baked into all savegame files, network packets, and .miz mission files. Raising them requires a versioned migration path. Stock campaign compatibility would break immediately without it.

Three B-class pool limits (TGL face/shadow/triangle pools) have **no telemetry** — exhaustion is silent and manifests as shapes vanishing mid-frame. Instrumenting these is the highest-value near-term action.

---

## Top 5 Caps to Instrument or Raise (Rationale)

1. **`facePool` / `shadowPool` / `trianglePool` (mission.cpp:3096-3102, B)** — 40K/30K/20K face/shadow/triangle pools. No peak-usage telemetry. Exhaustion is completely silent: `getVerticesFromPool` returns NULL and `TG_Shape::Render` silently early-outs. In Wolfman zoom mode (visibleVertices=200, 40K rendered tiles) the face pool is within a factor of 2 of its limit. **Action:** add a frame-peak counter and `fprintf(stderr, "[TGL] facePool peak: %u/%u\n", ...)` log on mission unload.

2. **`MAX_STANDARD_FUNCTIONS = 256` (ablsymt.h:161, A→B)** — ABL function registration table. When it hits 256, `ablsymt.cpp:522` logs and stops registering. Omnitech content (51 stubs registered) consumes 20% of the table. Future mod content or additional stub layers could hit this in a single session. **Action:** add a startup log of how many slots are consumed; raise to 512 after verifying stack/heap impact (it's two parallel flat arrays, not a tree).

3. **`MAX_MC2_GOS_TEXTURES = 750` (txmmgr.h:46, B)** — GPU texture cache slots. This branch still has 750 (mod-profile-launcher raised it to 3000). With 498 art + 1112 tgl upscaled textures deployed as loose files, a single complex mission can easily evict-and-reload more than 750 unique textures. Failure mode: `flushCache()` returns false → load silently abandons the texture. **Action:** raise to 3000 (already proven in mod-profile-launcher), add a per-mission high-water mark log.

4. **`GameVisibleVertices` default = 30/60, Wolfman = 200 (Viewer/View.cpp:41, mission.cpp:299, A)** — terrain vertex grid side. Terrain allocs `visibleVertices²` Vertex + TerrainQuad structs at init. At 200: 200²=40K Vertex + 40K TerrainQuad. At 300: 90K each — still heap, but the face pool (40K) becomes the binding constraint first. **Action:** verify face pool peak in Wolfman before raising visibleVertices further; document the face-pool dependency explicitly.

5. **`MAX_SENSORS = 150` / `MAX_CONTACTS_PER_SENSOR = 200` (dcontact.h:14-16, B/D)** — sensor pool and per-sensor contact list. MAX_CONTACTS_PER_SENSOR is a fixed-size `unsigned short contacts[200]` inside a struct that is saved/loaded; overflow at `contact.cpp:394` is a silent `numContacts` clamp, not an assert. In a dense multiplayer match with 8 teams × 12 units, total contacts across all sensors can exceed the per-sensor cap. **Action:** add a `numContacts >= MAX_CONTACTS_PER_SENSOR` assert-log; classify MAX_CONTACTS_PER_SENSOR as D pending savegame format audit.

---

## Full Inventory

| Constant | Value | File:Line | What It Caps | Allocation Sites | Key Write/Read Sites | Failure Mode | Silent? | File-Format? | Class | Next Action |
|---|---|---|---|---|---|---|---|---|---|---|
| `MC_MAXTEXTURES` | 4096 | mclib/txmmgr.h:44 | Texture node table size | txmmgr.cpp:131 `Malloc(4096 * sizeof(MC_TextureNode))` + two vertex-node tables | txmmgr.h:500,507,559,642,694,778,1043 (all nodeId < guard); txmmgr.cpp:2028 `gosASSERT(texNodeID < MC_MAXTEXTURES)` | gosASSERT crash on nodeId ≥ 4096 | No — asserted | No | A | Instrument current-node high-water mark at mission unload |
| `MAX_MC2_GOS_TEXTURES` | 750 | mclib/txmmgr.h:46 | GPU-resident texture cache slots | Runtime counter `currentUsedTextures`; no fixed array | txmmgr.cpp:455 check; txmmgr.cpp:2061 `if (currentUsedTextures >= MAX && !flushCache())` aborts load | Silent texture load failure when flush fails; object renders with old/wrong texture | Yes | No | B | Raise to 3000 (proven in mod-profile-launcher); add high-water log |
| `MAX_LZ_BUFFER_SIZE` | 8,389,632 (~8 MB) | mclib/txmmgr.h:50 | Decompression staging buffer for texture loads | txmmgr.cpp:1803,1945 `Malloc(MAX_LZ_BUFFER_SIZE)` (×2 buffers) | txmmgr.cpp:1958 `gosASSERT(txmSize <= MAX_LZ_BUFFER_SIZE)` on every texture load | gosASSERT crash if any texture decompresses > 8 MB | No — asserted | No | A | Adequate for current 4× upscales; monitor if 8× ever attempted |
| `facePool` init | 40,000 | code/mission.cpp:3096 | Per-frame face/index entries for TGL shapes | `new TG_DWORDPool` + `init(40000)` at mission start | tgl.cpp every shape render; reset at mission.cpp:802 | Silent early-out in `TG_Shape::Render` — shape disappears | Yes | No | B | Add peak counter; log on mission unload |
| `shadowPool` init | 30,000 | code/mission.cpp:3099 | Per-frame shadow vertex temp entries | `new TG_ShadowPool` + `init(30000)` | tgl.cpp:1668 `getShadowsFromPool(numVertices)` | Silent shape drop (same as facePool) | Yes | No | B | Same as facePool |
| `trianglePool` init | 20,000 | code/mission.cpp:3102 | Per-frame triangle records | `new TG_TrianglePool` + `init(20000)` | tgl.cpp triangle iteration | Silent shape drop | Yes | No | B | Same as facePool |
| `startVertices(500000)` | 500,000 | Viewer/View.cpp:582 | gos_VERTEX pool entries (main game) | `gos_VERTEXManager::init(500000)` in txmmgr.cpp:107 | All TGL shape renders via `getVerticesFromPool`; returns NULL when exhausted | NULL return → shape silently skipped (see TGL pool exhaustion memory note) | Yes | No | B | Add pool-usage log; mission2.cpp uses only 100K — verify they never coexist |
| `startVertices(100000)` | 100,000 | code/mission2.cpp:121 | gos_VERTEX pool for mission2 context | Same `gos_VERTEXManager::init` | Same readers | Same silent drop | Yes | No | B | Verify mission2 path is never hit with Wolfman zoom enabled |
| `startShapes(50000)` | 50,000 | Viewer/View.cpp:583 | Render shape manager capacity | `gos_RenderShapeManager::init(50000)` | Shape render pipeline | Unknown — likely assert or crash | E | No | B | Audit TG_RenderShape overflow path |
| `MAX_LIGHTS_IN_WORLD` | 256 | mclib/tgl.h:169 | Light entries in world/active/terrain light arrays | camera.cpp:393,405,412 `Malloc(256 * sizeof(TG_LightPtr))` (×3 arrays) | camera.h:532-561 iteration; tgl.cpp:63 static arrays | Array overrun (no guard visible) | E | No | A | Add bounds guard before raising |
| `MAX_SHADOWS` | 1 | mclib/tgl.h:177 | Shadow entries per TG_Shape | tgl.h:723 `bool shadowsVisible[1]` | tgl.h:797 init loop | Only one caster per shape; silently ignores extras | Yes | No | C | Evaluate per-shape multi-shadow cost vs. GPU shadow system benefit |
| `MAX_MAP_CELL_WIDTH` | 720 | mclib/terrain.h:58 | Map width/height in terrain cells | move.cpp:255 `long tileMulMAPCELL_DIM[720]`; trigger.h:49 `unsigned char map[240][240]` | move.cpp:814 init loop; trigger.cpp:37-40 gosASSERT bounds | gosASSERT crash on out-of-bounds tile; move.cpp overrun if map larger | No — asserted | Yes — .map file encodes cell count | D | Do not raise; audit .map format before any change |
| `MAX_TERRAIN_BLOCKS` | 144 | code/unitdesg.h:86 | Terrain block count for part-ID generation | Part-ID arithmetic only; no fixed array of this size found | unitdesg.h:68 `MAX_TERRAIN_PART_ID` formula | Part-ID collision if block count exceeds 144 | Silent ID collision | Yes — part IDs in savegame | D | Do not raise without full ID-space migration |
| `MAX_TERRAIN_BLOCK_VERTICES` | 400 | code/unitdesg.h:87 | Vertices per terrain block (ID math) | Part-ID arithmetic | Same as above | Same | Silent ID collision | Yes | D | Do not raise without migration |
| `MAX_TERRAIN_TILE_ITEMS` | 8 | code/unitdesg.h:88 | Items per terrain tile (ID math) | Part-ID arithmetic | Same | Same | Silent | Yes | D | Audit only |
| `MIN_UNIT_PART_ID`/`MAX_UNIT_PART_ID` | 1–511 | code/unitdesg.h:55-56 | Unit object ID space (static buildings etc.) | objmgr.cpp:139 `MoverRoster[MAX_MOVER_PART_ID - MIN_MOVER_PART_ID + 1]` | ID assignment at spawn; all savegame/network refs | ID space exhaustion → spawn fails or collides | Silent collision possible | Yes — savegame/network | D | Do not raise; 511 units is stock limit |
| `MIN_MOVER_PART_ID`/`MAX_MOVER_PART_ID` | 512–4095 | code/unitdesg.h:61-64 | Mover (mech/vehicle) object ID space | `MoverRoster[3584]` array | ID assignment, all savegame serialization, network packets | Collision or roster array overrun | Silent corruption | Yes — savegame/network | D | HARD LIMIT; do not raise without full savegame migration |
| `MIN_REINFORCEMENT_PART_ID`/`MAX_REINFORCEMENT_PART_ID` | 2050–3550 | code/unitdesg.h:62-63 | Reinforcement ID sub-range | Part of mover ID space | Reinforcement spawn | Max 1500 reinforcements across all teams | Silent cap | Yes | D | Do not raise without ID migration |
| `MAX_COMMANDERS` | 8 | code/unitdesg.h:81 | Max players/commanders (= NetworkMaxPlayers) | Static ID ranges; mechcmd2.cpp:2682 sets NetworkMaxPlayers | mechcmd2.cpp:2682; ID range OBJ_ID_FIRST_COMMANDER=492..499 | Network lobby limited to 8 | Logged/rejected by network layer | Yes — .miz format | D | Do not raise; modifying requires network and mission format changes |
| `MAX_MOVERGROUPS` | 16 | code/unitdesg.h:82 | Mover groups per commander | Part-ID arithmetic for mover IDs | ID generation formula | Part-ID corruption if exceeded | Silent | Yes | D | Audit only |
| `MAX_MOVERGROUP_COUNT_START` | 12 | code/unitdesg.h:83 | Initial movers per group | Part-ID formula | ID math | ID overrun → part-ID corruption | Silent | Yes | D | Audit only |
| `MAX_TRAIN_CARS` | 100 | code/unitdesg.h:85 | Cars per train | Part-ID range MIN_TRAIN_PART_ID..MAX_TRAIN_PART_ID | Train spawn | Train-ID overrun | E | Yes | D | Audit only |
| `MAX_CAMERA_DRONES` | 1000 | code/unitdesg.h:89 | Camera drone ID space | Part-ID range | Drone spawn | Drone-ID overrun | E | Yes | A | Safe to raise; runtime only |
| `MAX_TEAMS` | 8 | code/dteam.h:16 | Teams in a mission | team.h:81 `TeamPtr teams[8]`; team.h:58-59 `char relations[8][8]`; warrior.h:789,935 | team.cpp:73,83,87 static init with explicit 8-element arrays; move.cpp:303 TeamRelations init | Array overrun; static inits hardcode 8 elements | No — explicit 8-element static initializers will compile-error | Yes — team IDs in .miz + savegame | D | Do not raise without touching all static initializers and save format |
| `MAX_MOVERS` | 255 | mclib/dmovemgr.h:16 | Total movers (mechs + vehicles) across all teams | objmgr.h:237-239 `moverList[255]`, `goodMoverList[255]`, `badMoverList[255]` | objmgr.cpp:1767 `removeList[255]`; warrior.cpp:508,557,764 `contactList[255]` | Array overrun | No — arrays will overrun silently at index 255 | Yes — mover count in savegame/network | D | HARD LIMIT; do not raise without auditing all 255-sized arrays and save format |
| `MAX_MOVERS_PER_TEAM` | 120 | code/dteam.h:17 | Movers on one team's roster | team.h:51 `GameObjectWatchID roster[120]`; team.h:70 same in save struct | team.cpp:1315,1350 memcpy(roster, MAX_MOVERS_PER_TEAM); savegame serialize | Array overrun → savegame corruption | Yes | Yes — roster is serialized in team save blocks | D | Do not raise without bumping save struct version |
| `MAX_WARRIORS` | 120 | code/warrior.h:90 | MechWarrior pilot objects | warrior.h:943 `MechWarrior* warriorList[120]` (static) | warrior.cpp:287,4500,7515,7529,7561 full-array loops | Silent NULL warrior; loops will skip NULL entries | Partial | No | A | Raise with care — static array; audit all loop bounds |
| `MAX_MULTIPLAYER_MOVERS` | 96 | code/multplyr.h:104 | Movers tracked in multiplayer packets | multplyr.h:782 `unsigned char moverData[96]`; multplyr.h:1111-1112 chunk count arrays | multplyr.cpp:607 init loop; gameobj.cpp:536,1293 Assert bounds checks | Assert crash in debug; silent corruption in release | Partial (Assert in debug) | Yes — network packets | D | Do not raise; protocol change required |
| `MAX_MULTIPLAYER_MECHS_IN_LOGISTICS` | 36 | code/multplyr.h:105 | Mechs in MP logistics screen | controlgui.cpp:1820, forcegroupbar.cpp:317, logisticsdata.cpp:1865 | UI display only | UI truncates | Yes | No | A | Safe to raise for UI only |
| `MAX_MULTIPLAYER_TURRETS` | 128 | code/multplyr.h:107 | Turrets tracked in MP packets | multplyr.h:1049 loop bound | Packet serialization | Packet truncation | Yes | Yes — network | D | Do not raise without protocol change |
| `MAX_REINFORCEMENTS_PER_TEAM` | 16 | code/objmgr.h:142 | Reinforcement waves per team | objmgr.cpp:316-317 `maxMechs = numMechs + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM` | Spawn budget calculation | Budget undercount → spawn fails | Yes | No | A | Safe to raise; only used in spawn budget math |
| `MAX_CAPTURES_PER_TEAM` | 30 | code/objmgr.h:164 | Capture objectives tracked per team | objmgr.h:258 `captureList[8][30]` | objmgr.cpp:2597 iteration | Array overrun if > 30 captures per team | Silent | Yes — capture state in savegame | D | Do not raise without savegame audit |
| `MAX_MOVER_BODY_LOCATIONS` | 8 | code/mover.h:586 | Mech body sections (CT, LA, RA, etc.) | mover.h:617 `BodyLocation body[8]`; mover.h:778 save struct same | mover.cpp:7407 `memcpy(body, …, 8)` | Architecture hardcoded on 8 body sections | No — compile-time struct | Yes — serialized in mech save data | D | HARD LIMIT; architecture assumes exactly 8 |
| `MAX_MOVER_ARMOR_LOCATIONS` | 11 | code/mover.h:587 | Armor sections (includes rear torsos) | mover.h:623 `ArmorLocation armor[11]`; mover.h:787 save struct | mover.cpp:7414 memcpy | Same | Same | Yes | D | Do not raise |
| `MAX_MOVER_INVENTORY_ITEMS` | 72 | code/mover.h:588 | Equipment slots per mech | mover.h:627 `InventoryItem inventory[72]`; mover.h:792 save struct | mover.cpp:7420 memcpy; mover.h:1407,1415 gosASSERT guard | gosASSERT crash if slot ≥ 72 | No — asserted | Yes — serialized | D | Do not raise without save format version |
| `MAX_ANTI_MISSILE_SYSTEMS` | 16 | code/mover.h:90 | AMS systems per mech | mover.h:652 `unsigned char antiMissileSystem[16]` | mech.cpp:3396 `write(antiMissileSystem, MAX_ANTI_MISSILE_SYSTEMS)` | Array overrun + corrupt mech file | Yes | Yes — written to mech file | D | Audit only; "Way more than needed" (comment) |
| `MAX_WEAPONS_PER_MOVER` | 32 | code/dmover.h:16 | Weapons per mech/vehicle | warrior.h:731 `weaponsStatus[32]`; warrior.h:873 save struct | warrior.cpp:933,1452 init loops | Array overrun → save corruption | Yes | Yes — savegame | D | Do not raise without save format version |
| `MAX_RADIO_CHUNKS` | 7 | code/mover.h:83 | Radio message chunks per frame per slot | mover.h:877 `unsigned char radioChunks[2][7]` | mover.cpp:3883 cap check, silent drop if full | Silent message drop | Yes | Yes — network packet (2 slots × 7 chunks) | D | Audit network packet format before raising |
| `MAX_ATTACK_CELLRANGE` | 30 | code/mover.h:592 | Attack range in map cells | mover.h:937 `optimalCells[30][MAX_ATTACK_INCREMENTS][2]` (static) | mover.cpp:4489-4499 explicit clamp to MAX_ATTACK_CELLRANGE-1 | Clamped — no crash, shorter attack range | No — explicitly clamped | No | A | Safe to raise; audit optimalCells static array size |
| `MAX_SENSORS` | 150 | code/dcontact.h:14 | Sensor objects per mission | contact.cpp:1158 `Malloc(150 * sizeof(SensorSystemPtr))` | contact.cpp:724 cap check; contact.cpp:1022,1161 loops | Cap check hits → no new sensor allocated; unit has no sensor | Yes — silently no sensor | No | B | Add log when cap hit; instrument peak usage |
| `MAX_CONTACTS_PER_SENSOR` | 200 | code/dcontact.h:16 | Contacts tracked per sensor | contact.h:152 `unsigned short contacts[200]`; contact.cpp:886,890 stack arrays | contact.cpp:394 `if (numContacts < MAX_CONTACTS_PER_SENSOR)` clamp | Silent contact drop — unit invisible to sensor when over cap | Yes | Likely yes — sensor state in savegame (needs audit) | B→D | Add assert-log at clamp; audit savegame format |
| `MAX_STANDARD_FUNCTIONS` | 256 | mclib/ablsymt.h:161 | ABL built-in function table entries | ablsymt.cpp:63-65 `FunctionInfoTable[256]`, `FunctionCallbackTable[256]` (file-scope arrays) | ablsymt.cpp:522 `if (NumStandardFunctions == MAX_STANDARD_FUNCTIONS)` — logs and stops | Log message; further registrations silently dropped | Yes (log, not crash) | No | A | Raise to 512; audit stack impact of two 256-entry flat arrays |
| `MAX_FUNCTION_PARAMS` | 20 | mclib/ablsymt.h:162 | Parameters per ABL function | ablsymt.h:167 `FunctionParamType params[20]` per entry × 256 entries | ablsymt.cpp:541 gosASSERT if exceeded | gosASSERT crash | No — asserted | No | A | Raise with care; FunctionInfoTable is contiguous |
| `MAX_LIBRARIES_USED` | 25 | mclib/ablenv.h:114 | ABL library imports per module | ablsymt.cpp:46 `ABLModulePtr LibrariesUsed[25]` | ablsymt.cpp:214 error if exceeded | Error + module fails to load | No — errors | No | A | Safe to raise; small flat array |
| `MAX_SOURCE_FILES` | 256 | mclib/ablenv.h:113 | ABL source files per module | ablscan.cpp:279 `char SourceFiles[256][MAXLEN_FILENAME]` | ablscan.cpp:1294 error if exceeded | Error + compile fails | No — errors | No | A | Raise if MCO/Omnitech module count approaches limit |
| `MAX_ABLMODULE_NAME` | 5 | mclib/ablenv.h:112 | ABL module name length (chars) | ablenv.h:161 `char name[5]` per module | ablenv.cpp:724 `strncpy(name, _name, 5)` | Name silently truncated to 4 chars (null term) | Yes | No | A | Raise if longer module names needed; trivial |
| `MLR::Max_Number_Vertices_Per_Frame` | 32,768 (=8192×4) | Viewer/View.cpp:529 (call site) | Max GOSVertex entries across all MLR drawcalls per frame | mlr.hpp:120 runtime parameter; GOSVertexPool allocation at init | mlr/mlrsortbyorder.cpp:450,536 Verify() assert (2× cap) | Verify() abort (debug) / silent overrun (release) | Partial | No | C | Audit GOSVertexPool overflow path; check GL buffer backing size |
| `MLR::Max_Number_Primitives_Per_Frame` | 1,024 | Viewer/View.cpp:529 | Primitive (draw-call batch) entries per frame | mlr.hpp:121; DynamicArrayOf allocs in mlrprimitivebase.cpp, mlrsortbyorder.cpp | mlrsortbyorder.cpp:55 `priorityBuckets[i].SetLength(1024+0)` | DynamicArrayOf resize → heap alloc | No | No | B | Instrument primitive count per frame; likely fine |
| `MLR::Max_Number_Vertices_Per_Mesh` | 1,024 | mclib/mlr/mlr.hpp:103 (enum) | Vertices per single MLR mesh | DynamicArrayOf allocs in mlrcardcloud.cpp, mlreffect.cpp | Multiple clipExtra arrays of this size | DynamicArrayOf resize | No | No | B | Audit gosFX particle cards — may be approaching for large effects |
| `MLR::Max_Number_Vertices_Per_Polygon` | 32 | mclib/mlr/mlr.hpp:104 (enum) | Vertices per polygon (clipping scratch) | mlreffect.cpp:15-18 stack arrays of this size | Clipping code | Array overrun | E | No | C | Do not raise without auditing all clipping scratch arrays |
| `MLR::Max_Number_Of_Lights_Per_Primitive` | 16 | mclib/mlr/mlr.hpp:109 (enum) | Active lights per MLR primitive | mlrclipper.cpp:410 clamp | mlrclipper.cpp:474 Verify() | Clamped silently to 16 | Yes | No | A | Safe to raise if more lights needed |
| `MLR::Max_Number_Of_Multitextures` | 8 | mclib/mlr/mlr.hpp:108 (enum) | Multitexture layers per material | Static enum; usage E | E | E | E | No | C | Audit before raising; likely a GL texture unit cap |
| `MLR::Max_Number_Textures` | 16,384 (=1<<14) | mclib/mlr/mlr.hpp:106 (enum) | MLR internal texture ID space | Enum; texture ID masking | Texture lookup | ID wrap-around | Yes | No | C | Verify no texture IDs collide with MC_MAXTEXTURES=4096 node table |
| `gosPostProcess::shadowMapSize_` | 2,048 | GameOS/gameos/gos_postprocess.cpp:414 | Overlay/object shadow map resolution (px²) | `glTexImage2D(…, 2048, 2048, …)` | beginShadowPass viewport; shadow_terrain.frag sampler | Lower resolution = shadow acne/aliasing | No — GPU creates texture | No | C | Make configurable via prefs; 4096 safe on modern GPUs |
| `MAX_MC2_TRANSITIONS` | 8,192 | mclib/terrtxm.cpp:54 | Terrain texture transitions (blend zones) | terrtxm.cpp:140 `numTransitions = 8192` | terrtxm.cpp:1098 `if (nextTransition < 8192)` guard | Silent stop adding transitions when full | Yes | No | B | Add telemetry for transition usage per map load |
| `systemHeapSize` | 8,192,000 (~8 MB) | code/mechcmd2.cpp:136 | GameOS system heap size | GameOS heap init | All systemHeap->Malloc calls | Out-of-memory crash when heap exhausted | No — will crash | No | A | Raise if heap exhaustion observed (Tracy can profile) |
| `MAX_RADIOS` | 256 | code/radio.h:34 | Radio/voice message objects | radio.cpp:34 `RadioPtr radioList[256]` (static); radio.cpp:39 `PacketFilePtr messagesFile[256]` | gamesound.cpp:59 loop | Array overrun if > 256 radio objects created | Silent if no bounds check visible | No | A | Audit creation path for bounds guard before raising |
| `MOVEPARAM_ESCAPE_TILE` | 8,192 | mclib/move.h:130 | Escape tile sentinel value in pathfinding | Pathfinding param checks | Pathfinding | Sentinel collision if tile index ≥ 8192 | E | No | A | Verify no map can have ≥ 8192 tiles (MAX_MAP_CELL_WIDTH=720 → 518400 cells, so sentinel is safe) |
| `MAX_MOVERGROUPS` | 16 | code/unitdesg.h:82 | Mover groups per commander | Part-ID math | ID generation | ID overrun | Silent | Yes | D | Audit only |
| `MAX_MOVERGROUP_COUNT` | 16 | code/unitdesg.h:84 | Max movers per group (active cap) | Part-ID math | Group assignment | Overrun | Silent | Yes | D | Audit only |
| `OBJ_ID_FIRST_COMMANDER`/`LAST` | 492–499 | code/unitdesg.h:16-17 | Commander object ID range (8 slots) | Static ID range | Commander spawn | ID exhaustion → spawn fails | E | Yes | D | Tied to MAX_COMMANDERS=8; do not change independently |
| `OBJ_ID_FIRST_TEAM`/`LAST` | 500–508 | code/unitdesg.h:19-20 | Team object ID range (9 slots) | Static ID range | Team spawn | ID exhaustion | E | Yes | D | Same |
| `RANGED_CELLS_DIM` | 3,721 (=61×61) | code/mover.h:594 | Precomputed range-cell grid size | mover.h:937 `short rangedCellsIndices[30][2]` (outer only) | mover.cpp attack range calc | Implicit from MAX_ATTACK_CELLRANGE | No | No | A | Follows MAX_ATTACK_CELLRANGE automatically |
| `MAX_SENSORS_PER_TEAM` | 255 (=MAX_MOVERS) | code/contact.h:259 | Sensor objects per team | contact.h:274 `SensorSystemPtr sensors[255]` | contact.cpp:724 | Array overrun | Silent | No | A | Follows MAX_MOVERS; both must change together |
| `gosPostProcess::bloomFBO_[2]` | 2 (ping-pong) | GameOS/gameos/gos_postprocess.h:60 | Bloom downscale stages | 2 FBOs allocated at init | bloom pipeline | Fixed 2-pass blur | N/A | No | C | Raising requires bloom shader restructure |
| `MLR::Max_Number_Of_FogStates` | 4 | mclib/mlr/mlr.hpp:110 (enum) | Fog LUT table count | gosvertex.cpp:17 `BYTE fogTable[4][1024]` | Fog interpolation | Table overrun | E | No | A | Unused in GL path; safe to ignore |
| `MLR::Max_Number_ScreenQuads_Per_Frame` | 0 (disabled) | Viewer/View.cpp:529 | Screen-space quad batching | Set to 0 at call; no pool | N/A | Disabled | N/A | No | A | Unused; was D3D-era UI batching |

---

## Notes and Unknowns

### Caps confirmed NOT present on this branch (but in memory notes as existing elsewhere)

- **Static terrain shadow map 8192×8192**: Present in nifty-mendeleev worktree's rendering stack; not visible in this branch's GameOS/gameos/gos_postprocess.cpp. The 2048 shadow map above is the only shadow texture on this branch.
- **MAX_MC2_GOS_TEXTURES = 3000**: Present in mod-profile-launcher worktree; this branch still has 750. Cherry-pick needed.

### Ambiguous / needs owner audit

- **`startShapes(50000)` overflow path**: `gos_RenderShapeManager` overflow behavior not determined; no NULL-check visible in tgl.cpp. Mark E until audited.
- **`MAX_ANTI_MISSILE_SYSTEMS = 16` write path**: `mech.cpp:3396` writes exactly 16 bytes to mech file regardless of actual count — this is a fixed-width record, not a length-prefixed array. Do not raise without bumping mech file format version.
- **`MAX_LIGHTS_IN_WORLD = 256` guard**: camera.h:549 checks `lightNum < MAX_LIGHTS_IN_WORLD` before writing, but the overrun path if `numLights` counter itself exceeds 256 was not found. Needs audit.
- **`MAX_CONTACTS_PER_SENSOR` savegame**: contact state is loaded/saved in sensor blobs; whether `contacts[200]` is in the savegame struct was not confirmed. Treat as D until format confirmed.
- **Mission save packet counts**: No explicit `MAX_SAVES` or packet-count limit found. Saveload.cpp reads mission data from PacketFile; limits are implicit in file format. Needs a separate PacketFile format audit.
- **gosFX particle limits**: No `MAX_EFFECTS`, `MAX_PARTICLES`, or pool size for gosFX was found in this branch's code. The effect system appears to use dynamic DynamicArrayOf allocation. Mark for follow-up.

### Branch divergence notes

`terrain-pbr-mod` (root) diverges from nifty-mendeleev in at least:
- `MAX_MC2_GOS_TEXTURES`: 750 here vs. 3000 in mod-profile-launcher (nifty likely has 3000 too)
- Shadow infrastructure: 2048 overlay map only here; static 8192 map exists in nifty

When sizing Shape B/C work, use **nifty-mendeleev's values as the baseline**, not this branch.
