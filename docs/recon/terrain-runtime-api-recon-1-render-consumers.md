# TERRAIN-RUNTIME-API-RECON-1 — Render-adjacent terrain consumers

Read-only recon (2026-06-25, branch claude/nifty-mendeleev). Scope: every render-adjacent consumer of terrain height/material/feature data OTHER than the core terrain mesh draw. Goal: map them ahead of a future TerrainRuntime compatibility API, flagging which break if VISUAL height diverges from GAMEPLAY height (the key Terrain 2.0 risk).

## The height API split (anchor)

- Terrain::getTerrainElevation(Vector3D&) -- mclib/terrain.cpp:3638 -> mapData->terrainElevation. AUTHORITATIVE / GAMEPLAY height. Most consumers below call this (or the (tileR,tileC) overload at terrain.cpp:3645).
- Terrain::waterElevation (static float) -- terrain.cpp:161, set from mapData->waterDepth at terrain.cpp:3880. A single flat plane, NOT per-vertex.
- Acknowledged divergence seam: terrain.cpp:711-716 comment (TERRAIN-NORMALS-FROM-HEIGHT) uploads an R32F height tex for the shader, "Visual-only; gameplay height remains authoritative." Per-vertex pVertex->elevation (the mesh) is the visual source, getTerrainElevation the gameplay source. Identical TODAY (one buffer); Terrain 2.0 splits them.

## 1. SHADOWS

### 1a. Baked terrain self-shadow (colormap burn-in) -- [visual-height / feature-derived]
- TerrainColorMap::burnInShadows -- mclib/terrtxm2.cpp:609. Builds per-pixel heightMap from land->getTerrainElevation(y,x) (terrtxm2.cpp:443,449,475), ray-marches sun, bakes light+shadow into colormap at LOAD time.
- RISK: LOW-MODERATE. Baked once from gameplay height; if visual diverges the baked shadow no longer matches the displaced mesh (painted on wrong hill). Cosmetic.

### 1b. Dynamic shadow map (caster -> terrain receiver) -- [visual-height]
- Terrain rendered into shadow depth FBO via its normal mesh (per-vertex elevation). Shadow uniforms gameos_graphics.cpp:2490,2554 (shadowMap, lightSpaceMatrix, enableShadows). Receiver depth IS the visual mesh.
- RISK: LOW (self-consistent). Hazard only via caster GROUNDING (sec 5): props grounded on gameplay height vs visual-height receiver -> shadow floats off feet.

## 2. DECALS (craters / roads / runways / cement)

### 2a. Crater decals -- [gameplay-height / legacy-direct-packet]
- mclib/crater.cpp:257-260: each quad corner z = land->getTerrainElevation(...). Enqueued into legacy masterVertexNodes packet stream (renderLists flush).
- RISK: HIGH. Corners snapped to GAMEPLAY height; mesh drawn at VISUAL height -> z-fight/float/sink on displaced cells.

### 2b. Dynamic decal ring (scorch/track) -- [gameplay-height]
- mclib/dynamic_decal_ring.cpp:95 (z = land->getTerrainElevation(p)); header note line 9.
- RISK: HIGH. Same z-fight/float class as 2a.

### 2c. Cement / road / runway transitions -- [material / feature-mask]
- CEMENT-TRANSITION-COMPOSITE-1: now in-material shader blend at mesh vertex (not coplanar decal) -> rides visual mesh. Roads/runways colormap-baked. No getTerrainElevation grounding -> SAFE.

## 3. WATER -- [gameplay-height threshold + flat visual plane]

- Flat plane at Terrain::waterElevation (terrain.cpp:161,3880); fast-path upload terrain.cpp:2859, packed uniforms terrain.cpp:2884.
- Shore/submerged tile selection compares per-vertex pVertex->elevation vs waterElevation + shoreExt: terrain.cpp:3539-3566 (fast-path + legacy); shore gather terrain.cpp:3544-3558.
- Terrain::getWater(worldPos) -- terrain.cpp:4074 -- gameplay query, getTerrainElevation vs waterElevation / shallowDepth.
- RISK: HIGH + SUBTLE. Visible waterline = MESH vertex vs flat plane; gameplay water class = getTerrainElevation vs same plane. If visual != gameplay these separate -> water covers/exposes ground gameplay disagrees with. Two heights straddle one flat constant.

## 4. VEGETATION (GosVegetation cards) -- [material / feature-mask / gameplay-height]

GameAdapters/VegetationAdapter.cpp -- richest multi-signal consumer:
- Card/instance grounding: getTerrainElevation at 264, 312, 435, 464.
- Water-floor suppression: <= Terrain::waterElevation at 265, 312-313, 400, 451.
- Material classification: land->getTerrain(tileR,tileC) at 253, 323, 470 (classifyTerrainType density tier; comments 66/89/213/250).
- Overlay/feature mask: land->getOverlay(...) at 338.
- Slope gate: land->getTerrainAngle(instPos,&normal) DEGREES, 344-351.
- Tile map: land->worldToTileCell(...) 246, 320, 333, 466.
- RISK: HIGH (placement) + MODERATE (gates). Cards PLACED at gameplay height, RENDERED vs visual mesh -> trees float/sink on displaced cells. Slope gate from getTerrainAngle (gameplay) -> dense veg on visually-steep faces. Material/overlay masks height-independent -> safe.

## 5. OBJECT / STATIC-PROP / BUILDING grounding -- [gameplay-height]  ** LOAD-BEARING **

- Buildings re-ground Z EVERY FRAME when in view: code/bldng.cpp:825-827 (zPos = land->getTerrainElevation(position); position.z = zPos; setPosition).
- Generic objects: code/objmgr.cpp:4667,4695,4716,4743 (pos.z = land->getTerrainElevation(pos)).
- Game-object base + LOS footing: code/gameobj.cpp:371,443,1866,2098 (getLOSPosition footing at 2098).
- RISK: HIGH, load-bearing. Props/buildings/mechs at GAMEPLAY height while mesh under feet draws at VISUAL height -> every prop/building floats/sinks, and shadow caster (1b) grounded on wrong surface. Largest Terrain 2.0 break surface: hundreds of props x per-frame regrounding all assume mesh height == getTerrainElevation.

## 6. EDITOR + runtime picking -- [gameplay-height]

- Runtime ray pick: Camera::screenToTerrainApprox -- mclib/camera.cpp:927; fixed-point ray-plane iteration intersecting land->getTerrainElevation(hit) (camera.cpp:979). Surface grounding camera.cpp:1388,1421,1439,1459,1477; camera Z follow 2883,3081.
- Editor rect-select: mapData->terrainElevation(j,i) at terrain.cpp:3951.
- Editor brushes ground placement on getTerrainElevation (editor/*).
- RISK: MODERATE. Pick converges to GAMEPLAY height; clicking VISUAL mesh returns gameplay surface -> cursor/placement offset on displaced cells. Editor correctness, not crash.

## Divergence-risk summary (Terrain 2.0: visual != gameplay)

| # | Consumer | Class | Break if visual!=gameplay? |
|---|----------|-------|----------------------------|
| 1a | Baked colormap self-shadow | visual/feature | LOW (cosmetic) |
| 1b | Dynamic shadow map receiver | visual-height | LOW (risk via 5) |
| 2a | Crater decals | gameplay + legacy-packet | HIGH (z-fight) |
| 2b | Dynamic decal ring | gameplay-height | HIGH (z-fight) |
| 2c | Cement/road/runway | material/feature | SAFE |
| 3 | Water plane + shoreline | gameplay-threshold + flat plane | HIGH+SUBTLE |
| 4 | Vegetation cards | material+feature+gameplay | HIGH placement |
| 5 | Prop/building/object grounding | gameplay-height | HIGH -- LOAD-BEARING |
| 6 | Editor + runtime picking | gameplay-height | MODERATE |

KEY TAKEAWAY: every grounding consumer (craters, decals, vegetation, props, buildings, picking) calls getTerrainElevation = GAMEPLAY height, while mesh, shadow receiver, and waterline test ride the VISUAL per-vertex elevation. Identical TODAY (one buffer). A TerrainRuntime API must expose BOTH gameplayHeight() and visualHeight() and force each call-site to declare which; silent equality is the trap. Highest-impact break = sec 5 (prop/building grounding): per-frame, ubiquitous, feeds shadow casters.
