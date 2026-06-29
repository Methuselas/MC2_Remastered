# TERRAIN-LEGACY-RETIRE-RECON-1
# Reachability + deletion plan for the deprecated terrain branches (MLR / Indirect / PatchStream)

> All `file:line` are nifty HEAD at recon time and DRIFT CHEAPLY. Re-run
> `repo_query.py slice-preflight` before any slice derived from this doc.
>
> Branch: `claude/nifty-mendeleev`

---

## TL;DR

The canonical terrain renderer since 2026-06-09 is **LODChunk** (default-ON, gated
`MC2_TERRAIN_LOD_CHUNK`). The other three branches — **LegacyMLR**, **IndirectBridge**,
**PatchStreamThin** — are all suppressed by that gate in the default configuration. They are
reachable only under explicit `MC2_TERRAIN_LOD_CHUNK=0` opt-out. None fires in default tier1
smoke or default capture. The Indirect bridge also requires the camera-windowed arm flag
(`IsFrameSolidArmed()`), which is off during deterministic bookmarks unless a special visual
fixture is loaded (`tests/visual/bookmarks/mc2_01_terrain_solid.json`).

Runtime telemetry counters exist (`RenderCore/terrain_path_telemetry.h`) and are wired to the
debug-state dump + MCP. **Deletion must be gated on counter evidence (Indirect=0, MLR=0,
PatchStream=0 across playtest + capture + editor runs), NOT on grep.** No such dump is available
in the artifact tree yet — the counters postdate the last persisted dump. The smoke telemetry
parity oracle (`TERRAIN_INDIRECT_PARITY`) confirms `legacy_solid_setup_quads=0` in the latest
inner-loop runs (mc2_01 + mc2_24, 2026-06-29T14-24-48), consistent with LODChunk-only draws.

**HIGH finding (retained from terrain-subpass-recon-1):** the IndirectBridge latch
(`markTerrainDrawn`) was missing but has since been fixed by `TERRAIN-INDIRECT-LATCH-FIX-1`
at `gos_terrain_indirect.cpp:3750`. `terrain_subpass_contract.h` now correctly reflects
`latchActuallyImplemented=true` for all four branches. This is now a regression guard.

---

## Per-branch Reachability Verdict

| Branch | Default? | Gameplay (default) | tier1 smoke (default) | Capture/baseline (default) | Editor / EditRel | Vestigial flag |
|---|---|---|---|---|---|---|
| **LODChunk** | YES | REACHABLE — draws every frame | REACHABLE — draws every frame | REACHABLE — draws every frame | REACHABLE (surface draw via GPU-indirect; chunk LOD mesh NOT submitted — editor lacks `flushDrawCommands()`) | — |
| **IndirectBridge** | NO | PROVABLY-DEAD-IN-DEFAULT (suppressed `txmmgr.cpp:3011`) | PROVABLY-DEAD-IN-DEFAULT | PROVABLY-DEAD-IN-DEFAULT | PROVABLY-DEAD-IN-DEFAULT (same gate applies; editor uses `mc2TerrainLodChunkEnabled()` = same env bool) | Reachable only under `MC2_TERRAIN_LOD_CHUNK=0` + camera armed |
| **PatchStreamThin** | NO | PROVABLY-DEAD-IN-DEFAULT (suppressed `txmmgr.cpp:3011-3016`) | PROVABLY-DEAD-IN-DEFAULT | PROVABLY-DEAD-IN-DEFAULT | PROVABLY-DEAD-IN-DEFAULT | Reachable only under `MC2_TERRAIN_LOD_CHUNK=0` + not armed + stream ready |
| **LegacyMLR** | NO | PROVABLY-DEAD-IN-DEFAULT (`txmmgr.cpp:3089` else-fallthrough only) | PROVABLY-DEAD-IN-DEFAULT | PROVABLY-DEAD-IN-DEFAULT | PROVABLY-DEAD-IN-DEFAULT | Reachable only under `MC2_TERRAIN_LOD_CHUNK=0` + armed fails + stream not ready; `[8Z_VESTIGIAL]` log at startup (`terrain.cpp:145`) |

**No legacy branch is reachable in editor or capture in the default configuration.** See §5 for
editor detail.

---

## 1. Reachability Per Branch (detailed)

### Gate decision tree

The terrain solid branch is selected at **two call sites**:

1. `code/gamecam.cpp:508` — `Terrain::flushDrawCommands()` — **LODChunk only**, BEFORE `renderLists()`.
   No-op unless `mc2TerrainLodChunkEnabled()` (i.e. `s_blockMeta != nullptr`).

2. `mclib/txmmgr.cpp:2999-3114` — inside `renderLists()` — the 3-way branch:

```
if (IsFrameSolidArmed()) {
    if (mc2TerrainLodChunkEnabled())
        modernHandled = true;           // LODChunk already drew; suppress Indirect
    else
        modernHandled = DrawIndirect(); // IndirectBridge path
} else if (isReady() && !isOverflowed())
    modernHandled = flush();            // PatchStreamThin path
// else: fall-through to masterVertexNodes loop = LegacyMLR
```

When LODChunk is ON (default), `txmmgr.cpp:3011-3012` sets `modernHandled=true` without calling
`DrawIndirect()`. The masterVertexNode loop at `:3057-3059` then skips terrain nodes
(`MC2_ISTERRAIN` flag) for modernHandled draws. **Under the default config the txmmgr 3-way is a
pure no-op for terrain.**

### Branch-specific activation requirements

**LegacyMLR** (`txmmgr.cpp:3089/3105`, `gos_RenderIndexedArray` per masterVertexNode):
- Requires: `MC2_TERRAIN_LOD_CHUNK=0` AND `IsFrameSolidArmed()=false` AND PatchStream not ready/overflowed
- Normal gameplay: `[8Z_VESTIGIAL]` log printed at init (`terrain.cpp:145`). `setupTextures` in game
  build is `#ifdef MC2_IS_EDITOR` only since commit `006800e5` — the non-editor build no longer
  populates the legacy quad geometry the MLR loop depends on.
- **In practice: a chunk-OFF game run with no armed camera and no PatchStream will hit this path,
  but the draw will produce empty/missing geometry** because `setupTextures` is editor-only.
- Verdict: **PROVABLY-DEAD-IN-DEFAULT; vestigial in non-editor builds.**

**IndirectBridge** (`txmmgr.cpp:3014` -> `gos_terrain_indirect.cpp:3686 DrawIndirect` ->
`gameos_graphics.cpp:4024 gos_terrain_bridge_drawIndirect`):
- Requires: `MC2_TERRAIN_LOD_CHUNK=0` AND `IsFrameSolidArmed()=true`
- `IsFrameSolidArmed()` is CAMERA-WINDOWED: set by `ComputePreflight` only when the camera frames
  a SOLID-terrain expanse. Deterministic capture/smoke poses usually do NOT arm it unless the
  `mc2_01_terrain_solid.json` visual bookmark is loaded.
- `MC2_TERRAIN_INDIRECT` (default ON) gates colormap-atlas + recipe build but is NOT the primary
  draw gate — the draw gate is `IsFrameSolidArmed()` AND chunk-OFF.
- Verdict: **REACHABLE-BUT-DEPRECATED** under explicit `MC2_TERRAIN_LOD_CHUNK=0` + armed camera;
  **PROVABLY-DEAD-IN-DEFAULT**.

**PatchStreamThin** (`txmmgr.cpp:3019` -> `gos_terrain_patch_stream.cpp:flush()`):
- Requires: `MC2_TERRAIN_LOD_CHUNK=0` AND `IsFrameSolidArmed()=false` AND
  `TerrainPatchStream::isReady() && !isOverflowed()`
- The "un-armed but stream-filled" middle state — requires frame where camera did not arm Indirect
  but PatchStream was pre-populated (transition state).
- Verdict: **REACHABLE-BUT-DEPRECATED** under `MC2_TERRAIN_LOD_CHUNK=0`; **PROVABLY-DEAD-IN-DEFAULT**.

---

## 2. Telemetry Evidence

### Runtime path counters (TERRAIN-PATH-TELEMETRY-1)

Counter locations:
- `gos_terrain_lod_chunk.cpp:1162` — bumps `LODChunk` counter
- `gos_terrain_indirect.cpp:3751` — bumps `IndirectBridge` counter (co-located with latch fix)
- `gos_terrain_patch_stream.cpp:1503` — bumps `PatchStreamThin` counter
- `gameos_graphics.cpp:7294` — bumps `LegacyMLR` counter

Surfaced via `debug_state_dump.cpp:270-275` (JSON key `terrain_path`) and
`scripts/mcp/mc2_render_state_server.py:349-356` (MCP `get_render_state`).

**No persisted JSON dump with terrain_path block is available in the artifact tree** (the
telemetry counters postdate the last committed JSON dump in `debug_state/`). The
`diagnostic_trace.jsonl` file in `debug_state/` predates TERRAIN-PATH-TELEMETRY-1.

### Indirect parity oracle (existing smoke evidence)

The `TERRAIN_INDIRECT_PARITY` oracle in smoke telemetry (`MC2_TERRAIN_INDIRECT_PARITY_CHECK`)
reports `legacy_solid_setup_quads` — the count of quads that went through the legacy
`setupTextures` path. Latest run (2026-06-29T14-24-48, missions mc2_01 + mc2_24):

```
legacy_solid_setup_quads: 0  (mc2_01, 1800 frames)
legacy_solid_setup_quads: 0  (mc2_24, 1800 frames)
```

**`legacy_solid_setup_quads=0` confirms that no terrain quads used the legacy solid path in any
of the observed frames.** This is strong indirect evidence that LODChunk owns all terrain draws.
However it does NOT distinguish between LODChunk and IndirectBridge/PatchStream (both would show
0 here if LODChunk is on). The terrain_path counters from debug_state are the direct proof.

### What run produces definitive counter proof

To generate the counter evidence for deletion gating:

```powershell
$env:MC2_DEBUG_STATE_DUMP="1"
$env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
```

Then query `get_render_state()` via MCP (engine must be running with `MC2_DEBUG_STATE_DUMP=1`)
or parse the JSON dump for `terrain_path.lod_chunk`, `terrain_path.indirect`,
`terrain_path.patch_stream`, `terrain_path.legacy_mlr`. Deletion precondition: `indirect=0`,
`patch_stream=0`, `legacy_mlr=0` across full tier1 + playtest + editor session.

---

## 3. Gate Map

| Branch | Selecting gate(s) | Default | Env override to FORCE branch ON |
|---|---|---|---|
| **LODChunk** | `mc2TerrainLodChunkEnabled()` (`terrain.cpp:139`) = env `MC2_TERRAIN_LOD_CHUNK` not "0" | **ON** (default since 2026-06-09) | `MC2_TERRAIN_LOD_CHUNK=1` (already default; no override needed) |
| **IndirectBridge** | `MC2_TERRAIN_LOD_CHUNK=0` (chunk off) + `IsFrameSolidArmed()=true` (camera-windowed) | OFF (suppressed by chunk) | `MC2_TERRAIN_LOD_CHUNK=0` + run with a camera framing terrain |
| **PatchStreamThin** | `MC2_TERRAIN_LOD_CHUNK=0` + `IsFrameSolidArmed()=false` + `TerrainPatchStream::isReady() && !isOverflowed()` | OFF | `MC2_TERRAIN_LOD_CHUNK=0` + static camera bookmark (no arm) |
| **LegacyMLR** | `MC2_TERRAIN_LOD_CHUNK=0` + above both fail (else-fallthrough, `txmmgr.cpp:3089`) | OFF | `MC2_TERRAIN_LOD_CHUNK=0` + no camera arm + PatchStream empty/overflow |

Additional gates for `MC2_TERRAIN_INDIRECT`:
- Default ON. Controls colormap-atlas skip, recipe build, overlay arming (`gos_terrain_indirect.cpp:85-91`).
- Setting `MC2_TERRAIN_INDIRECT=0` disables recipe build and overlays but does NOT alone select a
  different solid-draw branch — it is orthogonal to the LODChunk/Indirect/Patch/MLR choice.
- `editorData.h:32` notes: `MC2_TERRAIN_INDIRECT=0` does NOT expand editor visible region
  (the camera-window cap at `mapdata.cpp:1114-1117` is upstream of both paths).

---

## 4. Legacy Side-Effect Dependencies

The question is: if branch X is deleted, what breaks beyond the draw call itself?

### markTerrainDrawn latch (sceneHasTerrain_)

All four branches now set the latch (`terrain_subpass_contract.h:latchActuallyImplemented=true`
for all rows, post `TERRAIN-INDIRECT-LATCH-FIX-1`). Deleting a branch removes its latch call,
but since the branch is unreachable in default config, sceneHasTerrain_ is already set by LODChunk
(`gos_terrain_lod_chunk.cpp:1162`) before the other branches would have a chance to run.

**Deleting LegacyMLR/IndirectBridge/PatchStreamThin does NOT break the latch** — LODChunk already
owns it.

### MVP snapshot publish (dispatch-MVP ring-slot + g_viewContentEpoch)

The IndirectBridge is the ONLY branch that publishes the dispatch-MVP snapshot and bumps
`g_viewContentEpoch` BEFORE the object phase. Consumers: water fast path, decals, cull
(`gos_object_draw_mvp.h:45`).

**However:** the LODChunk branch also reads `IsFrameSolidArmed()` for its MVP selection
(`gos_terrain_lod_chunk.cpp:635`): `IsFrameSolidArmed() ? getDispatchMvp16() : live`. The
dispatch-MVP snapshot is PUBLISHED by the Indirect preflight (`ComputePreflight`), not the draw.
The preflight (`gos_terrain_indirect.cpp:~1780+`) runs regardless of whether LODChunk or Indirect
does the draw — it is a shared setup step. So the MVP snapshot is still published when LODChunk is
on; the IndirectBridge DRAW is suppressed but the PREFLIGHT still runs.

**Deleting the IndirectBridge DRAW code does not remove the dispatch-MVP publish** — that is in
`ComputePreflight` which must be retained.

**HIGH:** `IsFrameSolidArmed()` is also used by many non-terrain consumers to gate behaviors:
`gameos_graphics.cpp:3240,3447,3963,10075,10196,10270`, `gos_terrain_water_stream.cpp:1262,1583,
1627,1699`, `gos_object_draw_mvp.h:45`. These read the ARMING flag from the Indirect infrastructure.
**`gos_terrain_indirect::IsFrameSolidArmed()` is a LOAD-BEARING SYMBOL across the whole renderer,
not just the Indirect branch.** It must be retained even after the Indirect draw code is deleted.
The function lives in `gos_terrain_indirect.cpp:2507` — only its callers in the DRAW path can go.

### colormap/atlas state (gos_State_Terrain, TextureClamp)

LegacyMLR sets `gos_State_Terrain=1` and `gos_State_TextureAddress=gos_TextureClamp` per node
(`txmmgr.cpp:3063-3067`). These are not read by downstream passes in the default config —
LODChunk sets GL state directly and does not depend on these gos render-state slots for its
downstream consumers.

### MC2_TERRAIN_INDIRECT infrastructure (recipe build, atlas, overlay)

The IndirectBridge DRAW depends on: recipe SSBO, thin-record SSBO, colormap atlas, cement atlas,
transition-mask 2D array, `GL_COMMAND_BARRIER_BIT` (`gos_terrain_indirect.cpp:3606`). These are
built by `ComputePreflight` even when LODChunk is on. Deleting the draw code does NOT remove the
recipe-build infrastructure, which is also used by overlays (`MC2_TERRAIN_INDIRECT_OVERLAY=1` is
the default-on decal/cement overlay path). **The overlay pipeline shares the recipe SSBO with the
solid bridge and must be considered separately.**

Specifically: `docs/known_issues.md:136` confirms `MC2_TERRAIN_INDIRECT_OVERLAY` is DEFAULT-ON
since Stage-6 flip `60f2ef8`. The overlay pipeline is LIVE and LOAD-BEARING. The IndirectBridge
SOLID draw is separate from the OVERLAY draw; deleting the solid draw code leaves the overlay
infrastructure intact.

### masterVertexNodes terrain loop (LegacyMLR specific)

`txmmgr.cpp:3052-3114` — the full masterVertexNodes `MC2_DRAWSOLID|MC2_ISTERRAIN` loop. Beyond
the terrain draw, this loop does node reset (`currentVertex = vertices`, `:3112`) which prevents
double-draw. Under `modernHandled=true` (default), terrain nodes are reset at `:3058-3060` via
early `continue` without drawing. So the reset is already handled. Deleting the MLR draw code
must preserve or consolidate the node-reset.

---

## 5. Editor / EditRel Caveat

`CLAUDE.md` states: "EditRel runs GPU-path-only — never add CPU fallbacks to editor TUs."

**Does the editor exercise IndirectBridge or LegacyMLR?** NO, under the default config.

Evidence from `docs/editor-chunk-path-parity.md:49,57,68,119`:

- `mc2TerrainLodChunkEnabled()` returns the same value in editor and game (it is a static-local
  bool from `terrain.cpp:139`; no editor/game branching).
- `makeLists` is skipped when chunk is on (`terrain.cpp:1523`).
- Legacy per-quad `draw()` loop is suppressed when chunk is on (`terrain.cpp:2221`).
- The GPU-indirect SOLID/OVERLAY path arms normally in the editor when recipes are ready.
- The chunk LOD MESH (`gos_TerrainLodChunk_SubmitDrawCommands`) is NOT submitted in the editor —
  the editor lacks `flushDrawCommands()` in its render chain. Terrain SURFACE is rendered by the
  GPU-indirect path (which IS armed via `IsFrameSolidArmed()`), not by the chunk mesh.

**Therefore: the editor exercises LODChunk gate (same env bool) + GPU-indirect SURFACE draw (the
overlay path), but NOT the IndirectBridge SOLID draw, NOT PatchStreamThin, NOT LegacyMLR.**

Note: `editorData.h:32` explicitly documents that `MC2_TERRAIN_INDIRECT=0` does NOT fix the
editor narrow-region rendering bug (upstream camera-window iterator at `mapdata.cpp:1114-1117`
is the real cause). The Indirect infrastructure is used for the SURFACE/OVERLAY path in the
editor, not as a fallback solid path.

**Implication for deletion:** IndirectBridge SOLID draw code can be deleted without breaking the
editor. The IndirectBridge OVERLAY infrastructure (recipe SSBO, cement atlas, overlay pipeline)
must be retained because the editor uses `MC2_TERRAIN_INDIRECT_OVERLAY` for terrain rendering.

---

## 6. Deletable-now vs Guard-first vs Load-bearing-keep

### Bucket (a): Safe to delete OUTRIGHT after runtime counter precondition is met

These are the draw-path code blocks that are unreachable in the default config.

| What | Files / functions | Precondition |
|---|---|---|
| LegacyMLR terrain draw loop | `mclib/txmmgr.cpp:3052-3114` (the `MC2_ISTERRAIN` branch inside the masterVertexNodes loop) | `legacy_mlr` counter == 0 across tier1 + playtest + editor |
| PatchStreamThin flush call + TerrainPatchStream class | `mclib/txmmgr.cpp:3016-3019`; `GameOS/gameos/gos_terrain_patch_stream.cpp` (1552 lines) | `patch_stream` counter == 0 across tier1 + playtest + editor |
| IndirectBridge SOLID draw entry | `mclib/txmmgr.cpp:3013-3015` (`modernHandled = DrawIndirect()`); `GameOS/gameos/gos_terrain_indirect.cpp` `DrawIndirect()` function (~3686-3760) | `indirect` counter == 0 across tier1 + playtest + editor |
| `MC2_TERRAIN_LOD_CHUNK=0` opt-out branch in `terrain.cpp:143-146` (the `[8Z_VESTIGIAL]` log + vestigial path) | `mclib/terrain.cpp:138-150` | All three counters == 0 |

### Bucket (b): Reachable-but-deprecated (needs deprecation warning + counter watch before deletion)

These are currently reachable under `MC2_TERRAIN_LOD_CHUNK=0` but are deprecated paths.

| What | Status | Action before deletion |
|---|---|---|
| `MC2_TERRAIN_LOD_CHUNK=0` opt-out env var support | Vestigial (already warned at `terrain.cpp:145`); game non-editor build has no `setupTextures` | Add MCP-observable deprecation warning; watch counter for 2-4 weeks of normal use |
| `TerrainPatchStream` class in `gos_terrain_patch_stream.cpp` | Not hooked up in default build; only activated by `MC2_TERRAIN_LOD_CHUNK=0` + un-armed state | Add `static_assert`-style comment; coordinate with counter watch |
| `masterVertexNodes` MLR path in `txmmgr.cpp` for terrain specifically | Already suppressed by `modernHandled`; `[8Z_VESTIGIAL]` printed | Count watch; note that non-terrain masterVertexNodes loop is KEPT (other objects still use it) |

### Bucket (c): Load-bearing — KEEP regardless

| What | Files | Why kept |
|---|---|---|
| `gos_terrain_indirect::IsFrameSolidArmed()` | `gos_terrain_indirect.cpp:2507`; header `:443` | Used by water, decals, object MVP, LODChunk MVP selection — far wider than terrain draw |
| `ComputePreflight` / recipe SSBO build | `gos_terrain_indirect.cpp:~1780+` | Drives dispatch-MVP publish + `g_viewContentEpoch`; overlay pipeline; LODChunk reads dispatch-MVP via `getDispatchMvp16()` |
| Colormap atlas build + `MC2_TERRAIN_INDIRECT` infrastructure for overlays | `gos_terrain_indirect.cpp` + cement atlas path | `MC2_TERRAIN_INDIRECT_OVERLAY` is default-ON; overlay pipeline is LIVE |
| `gos_terrain_lod_chunk.cpp` + `terrain_lod_chunk.vert/frag` | Full LODChunk renderer | Production authority |
| `mc2TerrainLodChunkEnabled()` function | `mclib/terrain.cpp:139` | Single source of truth; read by 20+ call sites |
| masterVertexNodes loop for non-terrain objects | `mclib/txmmgr.cpp:3052+` | Static props, mechs, vehicles, VFX still use this |
| `terrain_path_telemetry.h` counters | `RenderCore/terrain_path_telemetry.h` | Required as precondition-proof instruments; delete LAST after all legacy paths gone |
| `terrain_subpass_contract.h` | `RenderCore/terrain_subpass_contract.h` | Regression guard + frame-graph modeling; update rows as branches delete |

---

## 7. Deletion Plan — Safe Chunks

### Recommended first slice: **LEGACY-MLR-DELETE-1**

This is the safest first cut because LegacyMLR is the most vestigial: it cannot produce geometry
in game builds (no `setupTextures` outside `#ifdef MC2_IS_EDITOR`), it is the last fallthrough,
and deleting it does NOT touch the Indirect infrastructure or `IsFrameSolidArmed`.

**LEGACY-MLR-DELETE-1:**
```
What to delete:
  - mclib/txmmgr.cpp:3062-3114  (MC2_ISTERRAIN-flagged masterVertexNode draw block;
    NOT the loop head, NOT non-terrain nodes, NOT the reset-on-modernHandled at :3058)
  - The LegacyMLR TerrainPath::LegacyMLR counter bump in gameos_graphics.cpp:7294
    (or flip it to an assertion: FATAL if this counter ever > 0)
  - Update terrain_subpass_contract.h: remove LegacyMLR row (or mark DELETED)
  - Update kTerrainSubPassCount static_assert

Runtime counter precondition:
  - terrain_path.legacy_mlr == 0 across full tier1 (5 missions) + one free-play session
  - Confirm: legacy_solid_setup_quads == 0 in smoke oracle (already 0, shown above)

Deprecation slice that should precede it:
  - (already done): [8Z_VESTIGIAL] log at terrain.cpp:145 on MC2_TERRAIN_LOD_CHUNK=0
  - Add: fatal assert or MC2_LOG warning if noteTerrainPath(LegacyMLR) fires during tier1

Blast radius:
  - gos_State_Terrain, gos_State_TextureAddress per-node sets: gone (no downstream dep in default)
  - masterVertexNodes loop still exists for non-terrain objects: NOT affected
  - markTerrainDrawn at gameos_graphics.cpp:7292: can be deleted with the block (LODChunk owns latch)
  - gos_SetTerrainBatchExtras calls at txmmgr.cpp:3078-3083: remove this terrain-specific branch
```

### Second slice: **PATCHSTREAM-DELETE-1**

```
What to delete:
  - mclib/txmmgr.cpp:3016-3019 (the isReady/isOverflowed flush branch)
  - GameOS/gameos/gos_terrain_patch_stream.cpp (entire 1552-line file)
  - GameOS/gameos/gos_terrain_patch_stream.h (header)
  - Remove TerrainPatchStreamThin row from terrain_subpass_contract.h
  - Remove CMakeLists entry for gos_terrain_patch_stream.cpp

Precondition: terrain_path.patch_stream == 0 across tier1 + playtest + editor.

Blast radius:
  - gos_terrain_patch_stream is self-contained; quad.cpp emits to it when !IsFrameSolidArmed
    (quad.cpp:231 "return !IsFrameSolidArmed()"). With LODChunk default-on those emit calls
    are gated out. Verify: grep for TerrainPatchStream callers after LEGACY-MLR-DELETE-1.
  - markTerrainDrawn at gos_terrain_patch_stream.cpp:1500: gone (LODChunk owns latch)
```

### Third slice: **INDIRECT-SOLID-DELETE-1**

This is the most complex cut because IndirectBridge SOLID shares infrastructure with the
OVERLAY pipeline and `IsFrameSolidArmed()` is wide-impact.

```
What to delete:
  - mclib/txmmgr.cpp:3013-3015 (the IsFrameSolidArmed + !chunk branch calling DrawIndirect())
  - GameOS/gameos/gos_terrain_indirect.cpp: DrawIndirect() function body + supporting
    SOLID-specific setup (~3686-3760) — NOT ComputePreflight, NOT IsFrameSolidArmed,
    NOT overlay paths, NOT recipe/atlas build
  - gos_terrain_bridge.h: gos_terrain_bridge_drawIndirect() forward decl + callers
    IF no other callers remain (grep for gos_terrain_bridge_drawIndirect)
  - Remove IndirectBridge row from terrain_subpass_contract.h

Precondition: terrain_path.indirect == 0 across tier1 + editor (requires engine run with
  MC2_DEBUG_STATE_DUMP=1, query via MCP get_render_state() terrain_path block).

Blast radius (HIGH — read carefully):
  - IsFrameSolidArmed() MUST remain: 8+ non-terrain callers in gameos_graphics.cpp,
    gos_terrain_water_stream.cpp, gos_object_draw_mvp.h. The function is NOT deleted.
  - ComputePreflight / recipe SSBO / dispatch-MVP publish MUST remain: LODChunk reads
    dispatch-MVP via IsFrameSolidArmed + getDispatchMvp16() (gos_terrain_lod_chunk.cpp:635).
  - Overlay pipeline (MC2_TERRAIN_INDIRECT_OVERLAY) is DEFAULT-ON and LIVE — NOT touched.
  - Cement atlas, transition-mask 2D_ARRAY: kept by overlay path.
  - COMMAND barrier (gos_terrain_indirect.cpp:3606): lives in compute→dispatch chain,
    not in DrawIndirect() body — confirm placement before deleting draw code.
  - NOTE: requires audit of every line in DrawIndirect() before deletion to confirm
    no shared state mutations that the overlay path depends on.
```

### Slice ordering dependency graph

```
LEGACY-MLR-DELETE-1   (safest, lowest blast radius, delete first)
       |
PATCHSTREAM-DELETE-1  (self-contained TU, easy)
       |
INDIRECT-SOLID-DELETE-1  (careful — IsFrameSolidArmed + overlay infrastructure retained)
       |
MC2_TERRAIN_LOD_CHUNK=0 opt-out removal  (LAST — remove the escape hatch only after all
                                           legacy branches are gone and counters confirm 0)
```

---

## 8. Telemetry Precondition Procedure (for each deletion slice)

Before each deletion:

1. Run engine with `MC2_DEBUG_STATE_DUMP=1` + full tier1 smoke:
   ```powershell
   $env:MC2_DEBUG_STATE_DUMP="1"
   py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
   ```
2. Query MCP: `get_render_state()` → check `terrain_path` block.
3. Run one free-play / editor session with the relevant `MC2_TERRAIN_LOD_CHUNK=0`
   opt-out to confirm the target branch DOES increment its counter (to prove the
   counter is wired), then confirm it reads 0 on default runs.
4. For INDIRECT-SOLID-DELETE-1: also confirm `indirect=0` in editor via EditRel run
   with `MC2_DEBUG_STATE_DUMP=1`.

All counter checks must read:
- `legacy_mlr: 0` (for LEGACY-MLR-DELETE-1 precondition)
- `patch_stream: 0` (for PATCHSTREAM-DELETE-1)
- `indirect: 0` (for INDIRECT-SOLID-DELETE-1)

---

## Appendix: File / Symbol Reference (all drift-prone — re-grep before coding)

| Symbol / file | Location | Role in deletion |
|---|---|---|
| `mc2TerrainLodChunkEnabled()` | `mclib/terrain.cpp:139` | Gate for all branching; retained |
| `IsFrameSolidArmed()` | `gos_terrain_indirect.cpp:2507` | LOAD-BEARING; retained |
| `DrawIndirect()` | `gos_terrain_indirect.cpp:3686` | IndirectBridge draw; target of slice 3 |
| `TerrainPatchStream::flush()` | `gos_terrain_patch_stream.cpp:~1500` | PatchStream draw; target of slice 2 |
| masterVertexNodes MLR block | `mclib/txmmgr.cpp:3062-3114` | LegacyMLR draw; target of slice 1 |
| `noteTerrainPath()` | `RenderCore/terrain_path_telemetry.h:30` | Counters; delete LAST |
| `terrain_subpass_contract.h` | `RenderCore/terrain_subpass_contract.h` | Update each slice |
| `kTerrainSubPasses[]` | `RenderCore/terrain_subpass_contract.h:42` | Remove rows as branches delete |
| `allDeclaredLatchProducersImplemented()` | `RenderCore/terrain_subpass_contract.h:~125` | Regression guard; update each slice |
| `gos_terrain_bridge_drawIndirect` | `gameos_graphics.cpp:4024` + `gos_terrain_bridge.h` | Bridge entry; target of slice 3 |
| `[8Z_VESTIGIAL]` log | `mclib/terrain.cpp:145` | Already present; extend for slice 1 |
| `MC2_TERRAIN_LOD_CHUNK=0` support | `mclib/terrain.cpp:142-146` | Opt-out escape hatch; remove LAST |
