# RenderWorld Slice M5 - "Overlay" Recon

Date: 2026-05-23
Author: recon agent (Opus 4.7)
Status: RECON ONLY -- no spec proposed.
Predecessor arc: M1..M2.6 SHIPPED (static-prop pickup + mech pickup). M3 (terrain) and M4 (VFX) implicitly slotted as "next" per `RenderObjectKind` enum comment at `RenderWorld/RenderWorld.h:134` (`// Future: Terrain=2, Vfx=3, Overlay=4`).

---

## 1. Summary

- The token "overlay" in this codebase covers **at least seven distinct, mostly-unrelated systems**. The MEMORY index pointer "MC2_TERRAIN_INDIRECT_OVERLAY, decals, drawPass-retirement decal static-bake" is the strongest signal that the intended M5 scope is the **terrain-overlay / decal pipeline** (cement perimeter + craters/footprints) -- not HUD, not minimap, not editor.
- That candidate exists as a real rendering surface (`shaders/terrain_overlay.{vert,frag}` and `shaders/decal.frag`, both routed through `gosRenderer::drawTerrainOverlays` / `drawDecals` via the `WorldOverlayVert` batch in `GameOS/gameos/gameos_graphics.cpp:1808-1848`).
- **But it has effectively zero identity worth handing out.** Terrain overlays are mission-static splat geometry (cement perimeter tiles around buildings, baked once at mission load); decals are a fixed-size ring buffer of 1000 craters + 64 footprints, recycled FIFO. No game-side caller asks "which crater was clicked"; no AI or selection code dereferences a decal by id.
- The shaders ALREADY opt out of post-process shadow via `rc_gbuffer1_shadowHandled_flatUp()` writing GBuffer1, and neither shader writes `layout(location=2) out uint v_objectId` -- the M1.5 ObjectID MRT slot. So they're invisible to `RenderWorld::lookupAtPixel` even today, and giving them a handle would force a third shader-side MRT plumb for no consumer.
- The other "overlay" categories the prompt enumerates either don't exist in MC2 as separate render surfaces (no in-world unit billboards beyond HUD-2D; no map-symbol render layer beyond the tactical map's screen-space draw) or are already covered by sibling systems (HUD has `gos_State_IsHUD` + `flushHUDBatch`; debug overlays like `projectz_overlay` are dev-only diagnostics).
- **Recommendation: DEFER M5, or RESCOPE to either a debug visualization adapter or the cursor/reticle target indicator.** The substantive RenderWorld arc that should ship before M5 is **M3 (terrain pickup)** -- terrain has real identity (quad coords), real consumers (artillery targeting, drop zones), and is already on the implicit roadmap per the enum.
- The "overlay" naming in the prompt may be an artifact of the enum comment ordering rather than a substantive next-slice plan. Worth confirming with the user before doing more work.
- The one **substantive** future RenderWorld touch in the overlay/decal area is GPU-port of `gos_PushTerrainOverlay` (per stub spec `docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`) -- but that is a CPU->GPU producer-side perf migration, NOT an identity/picking slice, and does not need M5 framing.

---

## 2. "Overlay" definitional audit

Every distinct in-tree meaning of the string "overlay", with whether it constitutes a rendering surface:

| # | Meaning | Rendering surface? | Lives in | Notes |
|---|---------|--------------------|----------|-------|
| 1 | Terrain-overlay splat (cement perimeter, runway transitions, road decals around buildings) | YES - `terrain_overlay.{vert,frag}` | `mclib/quad.cpp` producer; `GameOS/gameos/gameos_graphics.cpp:1481,7308` push; `gameos_graphics.cpp:1480-1848` batch members | Mission-static. Lit inline (no deferred shadow). |
| 2 | Decal splat (bomb craters, mech footprints) | YES - `decal.frag` | `mclib/crater.cpp:563,572` producer; `gameos_graphics.cpp:1482,7313` push | Fixed ring buffer (`craterManager->init(1000, ...)` at `code/mission.cpp:2211` / `code/saveload.cpp:1106`). 64 footprint slots `mclib/crater.cpp:46`. |
| 3 | Map-tile semantic overlay (road / bridge / runway tile classifier) | NO - enum drives texture selection, not its own pass | `mclib/mapdata.h:39-60` (`enum Overlays { DIRT_ROAD..NUM_OVERLAY_TYPES=17 }`); consumed by `code/goal.cpp:223,232,255,266` for pathfinding | Pure data; already baked into terrain texture via #1. |
| 4 | MC_OverlayType atlas record (per-overlay texture pages + transitions) | NO - data descriptor for #1's textures | `mclib/terrtxm.h:87-95,102,122,135,152-162,184-185` | Build-time data. |
| 5 | `MC2_TERRAIN_INDIRECT_OVERLAY` env (gates the indirect bake of #1 producer's output) | NO - it's a kill switch, not a surface | `GameOS/gameos/gos_terrain_indirect.cpp:215`; default-ON since `60f2ef8` per `CLAUDE.md:157` | Toggles the M2d fast-path. |
| 6 | Mover `overlayWeightClass` (AI weight-class metadata) | NO - gameplay state | `code/mover.h:712,910,1157-1162` | Not visual. |
| 7 | Debug-visualization overlays | YES (debug-only) - `projectz_overlay.h:27-33`; shadow debug at `GameOS/gameos/gos_postprocess.cpp:829-845` (`drawShadowDebugOverlay`); `screenShadowProg_->setInt("overlayPass", 0)` at `gos_postprocess.cpp:607` (pass discriminator, not user content); `eye->projectForDebugOverlay` at `code/missiongui.cpp:3150`; `Ctrl+Alt+O drawTerrainOverlays` toggle at `code/missiongui.cpp:255,2815-2818` (debug visibility flag for #1 not a new pass) | `mclib/projectz_overlay.{h,cpp}`; `gos_postprocess.cpp:829-845` | Hotkey-toggled diagnostics. No identity. |

Categories from the prompt that **do not exist** in MC2 as separate render surfaces:

- **HUD overlays** (health bars / ammo / reticle): yes they exist, but they live in HUD infra (`gos_State_IsHUD`, `flushHUDBatch` at `gameos_graphics.cpp:1305`, dedicated HUD command buffer at `:1617`). Not the same surface as "overlay" #1/#2. Already has its own pipeline. No object-ID buffer participation by design (screen-space; consumed by hardcoded UI click handlers, not by pixel readback).
- **In-world icons / labels above units**: MC2 does not render world-space text labels or unit billboards beyond the HUD-2D path. Unit selection rings (`code/mech.cpp` selection circle) draw inline with the mech path.
- **Map symbols / minimap markers**: tactical map renders to its own framebuffer; the live "viewport-rect overlay block" at `code/gametacmap.cpp:206-210` is **already deleted** (commented-out dead code from Phase-1 carve-out 2026-05-19). No active surface.
- **Mission-script overlays (objective arrows / tutorial highlights)**: not found. ABL/`code/ablmc2.cpp:` "overlay" mention is metadata, not a draw call.
- **Cinematic overlays (letterbox bars)**: not found as a discrete pass.
- **Editor overlays**: MC2's mission editor (`Viewer/`) is a separate target; no in-engine editor overlay surface in the runtime path.

---

## 3. Strongest candidate for M5 RenderWorld adapter

Of the seven categories, only #1 (terrain-overlay) and #2 (decals) are world-space, lit-by-engine, depth-tested rendering surfaces that the existing RenderWorld machinery is shaped to absorb.

Within those two, **#2 decals** has a slight edge over #1 terrain-overlays for "looks like a RenderWorld adapter" because:

- Decals are dynamic / lifecycle-bearing (spawn at weapon impact, age, get reclaimed when the 1000-slot ring wraps). Static terrain-overlay tiles are bake-once.
- Decals already have a per-element identity in `CraterManager::craterList[]` indexed by slot.
- Decals have a `craterShapeId` enum and a `craterTextureIndices[handleOffset]`, both stable per-mission.

But this edge is **cosmetic, not load-bearing**, because of the killer counterargument in section 4: **neither shader writes ObjectID**, and **no consumer asks "what decal is at this pixel?"**.

---

## 4. Rendering write paths for the candidate

`shaders/terrain_overlay.frag:32-35`:
```
layout(location=0) out PREC vec4 FragColor;
#ifdef MRT_ENABLED
layout(location=1) out PREC vec4 GBuffer1;
#endif
```

`shaders/decal.frag:38-41`:
```
layout(location=0) out PREC vec4 FragColor;
#ifdef MRT_ENABLED
layout(location=1) out PREC vec4 GBuffer1;
#endif
```

Both shaders write to color attachments 0 and 1 only -- NO `layout(location=2) out uint v_objectId` (the M1.5 ObjectID substrate slot at `RenderWorld/RenderWorld.h:79-85`). So `RenderWorld::lookupAtPixel(x, y)` cannot ever return a decal or terrain-overlay handle today.

State contracts at `terrain_overlay.frag:18-25` and `decal.frag:23-31`:
- depthTest=true, depthWrite=false (overlays / decals are alpha-blended on top of terrain).
- AlphaBlend (terrain_overlay binary-alpha discard; decal SRC_ALPHA classic blend).
- `castsStatic=false, castsDynamic=false, skipsPostScreenShadow=true` -- both opt out of post-process screen shadow, handling cloud + static + dynamic shadow inline.

Both surfaces would need new fragment outputs gated behind the existing `MC2_OBJECT_ID_BUFFER` macro pattern (mirror of `shaders/mech.frag` per M2.5 spec) to participate at all. Plus a producer fill on the CPU side that has no source of identity to fill it from -- `gos_PushTerrainOverlay` / `gos_PushDecal` take only `(verts3, texHandle)`. No object id flows in today.

---

## 5. Identity question for the candidate

| Question | Terrain-overlay (#1) | Decal (#2) |
|----------|----------------------|------------|
| Unit of identity | None per-tri. Tile-coord could be re-derived from world position. | `CraterManager::craterList[i]` index. |
| Lifetime | Mission-static (baked at load). | Recycled FIFO; one slot lives ~game-event-duration. |
| Cardinality | Thousands per mission (every building footprint perimeter; runway tiles). | 1000 craters + 64 footprints, hardcoded at `crater.cpp:46` + init at `mission.cpp:2211`. |
| Existing consumer that CARES which one was clicked | None found. `Ctrl+Alt+O` (`missiongui.cpp:255,2815`) is a global show/hide of all terrain-overlays. | None found. No code path queries a crater by spatial click. Craters are visual residue only. |

Negative-claim verification (per CLAUDE.md "Negative claims need opposite-direction grep" rule): grepped `getCrater`, `craterAt`, `pickCrater`, `decalAt` repo-wide -- zero hits. Grepped `mover.cpp / missiongui.cpp / tacordr.cpp / warrior.cpp` for any reference to crater/decal handles or indices in click handling paths -- zero hits in handlers. The "overlay" hits in `mover.cpp:23` are all `overlayWeightClass` (AI category #6).

Conclusion: neither surface has an existing in-engine consumer that would benefit from a `RenderObjectHandle`. Issuing handles would be **pre-speculative substrate without a named first consumer** -- the exact anti-pattern the M2.6 META-FIX scope discipline pushed back against.

---

## 6. Handle range proposal (if M5 ships)

If the user does decide to proceed, the obvious slot consistent with the existing scheme at `RenderWorld/RenderWorld.cpp:121`:

| Kind | Base | Max population observed |
|------|------|------------------------|
| `StaticProp` | 0 | tier1 max 2641 (mc2_24) per CLAUDE.md M1 entry |
| `Mech` | `0x00010000` (65536) | tier1 max likely <2k |
| `Terrain` (M3, hypothetical) | `0x00020000` (131072) | depends on grid; ~64x64 quads = ~4k? |
| `Vfx` (M4, hypothetical) | `0x00030000` (196608) | depends |
| `Overlay` (M5, hypothetical) | `0x00040000` (262144) | ~3000 (1064 decals + ~thousands of mission-static overlay tris) |

20-bit index field (`RenderCore/Handle.h:32-43`) caps total population at `0x100000 = 1048576`. Bases at 64K stride leave 15 free regions; comfortable.

**But:** even the proposal exposes the problem -- there's no natural per-decal-tri identity. The `pushDecalTri` call takes 3 verts and a texHandle; one logical "crater" produces 2 triangles (`crater.cpp:563,572`). Would the handle key on `craterList[i]` index, or on per-tri? The fact that this is unclear is itself a signal: **the data model is being asked to invent identity it doesn't naturally have**.

---

## 7. Possible alternative scopes for M5

Listed roughly in increasing-substance order. The first three are "rescope to give M5 a real consumer"; the last is "drop M5 entirely":

### 7a. Cursor / reticle target indicator
Use the existing `tryGameplayPick` spine (`code/gameplay_pick.cpp`) on hover (not just on click), then surface the kind-tag of what's under the cursor (StaticProp / Mech / Terrain / ...) into the HUD. This is a **consumer of M3 (terrain pickup)** more than a new slice. Doesn't justify M5 as its own arc.

### 7b. Debug visualization adapter
Wire a new `MC2_RENDER_WORLD_DEBUG_OVERLAY=1` env that draws a small text label next to whatever the cursor is over, using the picked handle to look up `RenderObjectRecord` and print kind + index + generation. Useful for greybeard debugging but small-stakes. Could be a 1-2 day MR not a full slice.

### 7c. Selection box / drag-rect
Almost certainly should NOT be RenderWorld -- it's screen-space 2D UI. Sits in HUD path. Excluded.

### 7d. Defer M5 entirely (RECOMMENDED)
Ship M3 (terrain pickup -- has real identity from quad coords, real consumers in artillery / drop-zone targeting) and M4 (VFX -- explosions / muzzle flashes, has potential identity for cinematic camera focus). Reevaluate "overlay" framing after those land; by then the residual scope for M5 may be obvious or may have evaporated.

The CPU->GPU port of `gos_PushTerrainOverlay` is a **perf migration** worth doing for terrain modernization, but it lives under the `MC2_TERRAIN_INDIRECT_OVERLAY` story (already DEFAULT-ON, just needs Tracy proof per `CLAUDE.md:157`), NOT under the M5 RenderWorld arc.

---

## 8. Open questions for user (IMPORTANT)

**Q1.** Does the user have a specific overlay use case in mind that drove the M5 naming? Possible interpretations:
- (a) literal `RenderObjectKind::Overlay=4` (per the enum comment at `RenderWorld/RenderWorld.h:134`) meaning "the decal/cement-perimeter splat pipeline"
- (b) HUD / in-world unit labels (would be a totally different slice -- screen-space, not RenderWorld)
- (c) a debug visualization use case (cursor-hover-shows-kind, etc.)
- (d) something I haven't enumerated

This is the load-bearing question. Without an answer, the most code-grounded reading is (a) -- terrain-overlay + decals -- but section 5 shows that scope has no identity consumer.

**Q2.** Is the implicit roadmap (M3 terrain, M4 VFX, M5 overlay -- in that order, per the enum comment) load-bearing, or just a placeholder ordering from M2 spec time? If the latter, **M5 should be deferred or dropped and M3/M4 prioritized**.

**Q3.** Is there an editor / mission-script / cinematic overlay use case that wasn't obvious from CLAUDE.md? The `Viewer/` target was not scanned (out of scope of this recon); if the editor has world-space overlays needing picking, that's a non-trivial expansion of M5 scope.

**Q4.** If the answer to Q1 is "yes (a) -- I want the decal pipeline to be RenderWorld-aware", what is the consumer? Is it cinematic-focus on a specific crater? Modding hook (mod can attach behavior to a specific decal)? AI cover-finding using craters as terrain features? Without a named consumer M5 is pure substrate.

---

## 9. File:line citations

All grep-verified 2026-05-23.

| Subject | File:line | Snippet/role |
|---------|-----------|--------------|
| RenderObjectKind enum + Overlay=4 comment | `RenderWorld/RenderWorld.h:131-135` | `// Future: Terrain=2, Vfx=3, Overlay=4` |
| 20-bit handle index cap | `RenderCore/Handle.h:32-43` | `((generation & 0xFFFu) << 20) | (index & 0xFFFFFu)` |
| kMechHandleBase | `RenderWorld/RenderWorld.cpp:121` | `static constexpr uint32_t kMechHandleBase = 0x00010000u;` |
| MC2_TERRAIN_INDIRECT_OVERLAY default ON | `GameOS/gameos/gos_terrain_indirect.cpp:215`; `CLAUDE.md:157` | Stage-6 flip `60f2ef8` 2026-05-17 |
| terrain_overlay frag MRT outputs | `shaders/terrain_overlay.frag:32-35` | `layout(location=0) out FragColor` + `(location=1) out GBuffer1` |
| decal frag MRT outputs | `shaders/decal.frag:38-41` | same shape; no location=2 |
| WorldOverlayVert batch members | `GameOS/gameos/gameos_graphics.cpp:1808-1848` | `OverlayBatch_ terrainOverlayBatch_; OverlayBatch_ decalBatch_;` |
| pushTerrainOverlayTri / pushDecalTri impl | `GameOS/gameos/gameos_graphics.cpp:7308,7313` | gosRenderer methods |
| gos_PushTerrainOverlay / gos_PushDecal C wrappers | `GameOS/gameos/gameos_graphics.cpp:7640,7648` | __stdcall entry points |
| gos_PushTerrainOverlay producers in quad.cpp | `mclib/quad.cpp:1678,1685,1694,1701,1859,1947,2107,2193` | M2d fast-path emit sites |
| gos_PushDecal callers (crater + footprint) | `mclib/crater.cpp:563,572` | two tris per crater (gWov, sWov) |
| crater pool size | `mclib/crater.cpp:46` + `code/mission.cpp:2211` + `code/saveload.cpp:1106` | `MAX_FOOTPRINTS=64`; `craterManager->init(1000, ...)` |
| MC_OverlayType atlas | `mclib/terrtxm.h:87-95,102,121-122,135,152-162` | Overlay texture-page descriptor |
| Overlays enum (semantic map-tile types) | `mclib/mapdata.h:39-60` | `DIRT_ROAD..NUM_OVERLAY_TYPES=17` |
| Overlay enum consumers (pathfinding) | `code/goal.cpp:223,232,255,266` | `GameMap->getOverlay(...)` |
| overlayWeightClass (AI) | `code/mover.h:712,910,1157-1162` | not visual |
| drawTerrainOverlays Ctrl+Alt+O toggle | `code/missiongui.cpp:182,255,2815-2818` | global show/hide debug flag |
| projectForDebugOverlay | `code/missiongui.cpp:3150` | debug-only |
| projectz_overlay debug viz | `mclib/projectz_overlay.h:27-33` | `PZ_OVERLAY_OFF..` enum |
| shadow debug overlay | `GameOS/gameos/gos_postprocess.cpp:829-845` | `drawShadowDebugOverlay()` |
| screen shadow overlayPass uniform | `GameOS/gameos/gos_postprocess.cpp:607` | pass discriminator, not content |
| tactical map overlay block (DEAD) | `code/gametacmap.cpp:206-210` | Phase-1 carve-out 2026-05-19 deleted; commented |
| HUD batch infra | `GameOS/gameos/gameos_graphics.cpp:1305 (flushHUDBatch), 1617 (HUD command buffer), 4353 (gos_State_IsHUD)` | separate from "overlay" |
| Overlay/decal GPU port stub spec | `docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md` | perf migration, not identity slice |
| RenderWorld lookupAtPixel | `RenderWorld/RenderWorld.h:192-199` | requires shader-side write at location=2 |
| tryGameplayPick spine | `code/gameplay_pick.cpp:79-128` | shared gate ladder; consumed by M1.6 + M2.6 |

---

## 10. Recommendation

**DEFER M5 pending user clarification on Q1.** Strongest probability is that "Overlay=4" in the M2 enum comment was placeholder ordering not a committed slice. The decal/terrain-overlay surface has no identity consumer; pushing it through the RenderWorld machinery would be substrate without a first user -- the exact anti-pattern flagged in M2.6's META-FIX discipline review.

If the user confirms M3 (terrain) and M4 (VFX) are the next-priority slices, M5 should be removed from the implicit roadmap and the `Overlay=4` enum comment should be revised to reflect that (or held until a real consumer surfaces, e.g. a modding hook or a campaign mission system that needs to address specific battlefield scars).

If the user has a specific use case in mind (Q1), produce a new recon scoped to THAT use case -- not to the generic "overlay" word -- to avoid scoping by lexical accident.

RECON STATUS: COMPLETE
