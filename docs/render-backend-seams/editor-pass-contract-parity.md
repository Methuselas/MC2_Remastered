# Editor / Game Pass-Contract Parity (EDITOR-PASS-CONTRACT-PARITY-1)

Slice 5 of the editor/game parity arc. Applies the project render-governance
pattern to the **editor**: for every governed render pass, assert the editor
renders it through the same `PassId` / pipeline / attachment contract as the
game, or declares a scoped editor-override with rationale. Prevents quiet drift
after future render features.

- **Source of truth (declared table):** `editor-pass-contract-parity.json`
- **Checker:** `scripts/check-editor-pass-contract-parity.py` (registered in
  `scripts/check-contracts.sh` as `editor_pass_parity`)
- **Governed-pass authority:** `RenderCore/RenderPassContract.h` — `enum class
  RenderPassId` + `kRenderPassContracts[]` (11 real passes).

## How the editor renders

The editor does **not** have a private world renderer. Its frame is:

```
editor/EditorGameOS.cpp:
  pp_editor = getGosPostProcess()          // SAME gosPostProcess instance as the game
  pp_editor->beginScene()                  // binds sceneFBO_, MRT incl. COLOR_ATTACHMENT2 (object-id)
  pp_editor->clearGBuffer1()
  Environment.UpdateRenderers()            // editor/Editor.cpp -> editor->render()
                                           //   == the SAME shared world renderer the game runs
  pp_editor->endScene()                    // PostProcess composite (FBO0)
```

Because `editor->render()` drives the identical shared renderer, every governed
world pass (terrain / static-prop / mech / shadow / water / vegetation /
decal / overlay / vfx / UI) runs under the **same** `PassId`, pipeline, and
scene-FBO attachment contract as the game. The only host-level delta is the
PostProcess composite **target viewport** (editor render-to-texture region vs
full FBO0) — a blit destination, not a contract change (see PostProcess row).

## Parity table

| PassId | game | editor | classification | killswitch |
|---|---|---|---|---|
| StaticPropOpaque | rendered | rendered | shared | MC2_STATIC_PROP_REGISTRY |
| Terrain | rendered | rendered | shared | — |
| MechOpaque | rendered | rendered | shared | — |
| Shadow | rendered | rendered | shared | — |
| VFX | rendered | rendered | shared | — |
| Water | rendered | rendered | shared | — |
| PostProcess | rendered | rendered | shared (viewport-only host delta) | — |
| VegetationCards | rendered | rendered | shared | — |
| TerrainDecal | rendered | rendered | shared | — |
| TerrainOverlay | rendered | rendered | shared | — |
| UI | rendered | rendered | shared | — |
| _editor_only:Gizmos | absent | rendered | editor-only | — |
| _editor_only:SelectionOverlay | absent | rendered | editor-only | — |
| _editor_only:BrushPreview | absent | rendered | editor-only | — |
| _editor_only:ObjectIdReadbackViz | absent | rendered | editor-only | — |

**Shared (11):** every governed `RenderPassId` is shared — the editor renders
the world through the same path, so there is one contract, not two.

**Editor-only (4):** authoring affordances with no game frame equivalent —
manipulator gizmos (EditorInterface/Action), selection highlight
(EditorObjectMgr), brush footprint preview (BuildingBrush), and the object-id
**readback + pick visualization** (`lookupAtPixel`, Ctrl+Shift+LMB). Note the
object-id MRT **write** into `COLOR_ATTACHMENT2` is shared (covered by the
PostProcess/scene-FBO row in both game and editor); only the editor-side
readback consumer is editor-only. These are unguverned (no `RenderPassId`) and
thus have no game contract to drift against.

**Game-only (0):** none. No governed pass is skipped by the editor.

**Override (0):** none. No pass needed a divergent editor pipeline. If a future
render feature forces the editor onto a different pipeline/attachment for a
governed pass (e.g. a deferred-shading pass the editor can't afford), add an
`override` entry with rationale — the checker requires rationale on overrides.

## Checker mechanism

`check-editor-pass-contract-parity.py` reads the JSON table and the
`RenderPassId` enum, then FAILs (exit 1) on:

1. parity JSON missing/unparseable;
2. a governed `RenderPassId` (≠ `None`) with **no** parity entry → undeclared
   drift (a new governed pass appeared, nobody decided the editor relationship);
3. a parity entry referencing a non-`_editor_only:` passId absent from the enum
   → stale entry;
4. unknown `classification`;
5. `override` / `game-only` / `editor-only` entry with empty `rationale`;
6. a `shared` entry where game or editor is `absent` (contradiction);
7. `editor-only` claiming `game=rendered`, or `game-only` claiming
   `editor=rendered`.

It is grep/AST-lite (regex enum parse + JSON load), read-only, no GL, no build,
Windows `py -3` runnable.

### Negative test (verified)

Removing the `Water` entry from the JSON makes the checker FAIL with
`governed RenderPassId::Water has NO entry ... -> undeclared editor/game pass
drift` (exit 1). Restoring it returns exit 0. This confirms check (2) — the
core anti-drift assertion — actually fires.

## Maintenance contract

When a render slice **adds** a `RenderPassId`, it MUST also add a row here (the
checker fails CI otherwise). When a slice **renumbers/renames** a pass, update
the matching `passId` (check 3 fails on a stale name). When a slice forces the
editor onto a divergent pipeline for a governed pass, reclassify that row to
`override` with rationale rather than leaving it `shared`.
