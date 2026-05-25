# RenderWorld Slice M5 — OverlayRenderAdapter (clarification-pending)

- **Date:** 2026-05-23
- **Status:** DRAFT — CLARIFICATION-PENDING
- **Goal:** Clarify what "M5 / Overlay" actually means before any implementation
  scope is chosen. This spec is a **clarification request, not an
  implementation spec.** No code change ships from this spec.
- **Tech stack:** N/A (clarification document)
- **Predecessor arc:** M1, M1.5, M1.6, M2-pre, M2, M2.5, M2.6 SHIPPED 2026-05-23
- **Predecessor recon:**
  `docs/superpowers/explorations/2026-05-23-renderworld-slice-m5-overlay-recon.md`
- **Migration guide:** `docs/renderworld_migration_guide.md`
- **Enum reservation site:** `RenderWorld/RenderWorld.h:131-135`
  (`// Future: Terrain=2, Vfx=3, Overlay=4`)

---

## 0. Why this spec is a clarification request

The recon agent (2026-05-23, Opus 4.7) catalogued every in-tree use of the
word "overlay" and found **at least seven distinct, mostly-unrelated
systems** sharing the name. The strongest code-grounded reading of
`RenderObjectKind::Overlay = 4` is "terrain-overlay splat + decal pipeline,"
because those are the only world-space, depth-tested, engine-lit
rendering surfaces that the existing RenderWorld machinery is shaped to
absorb. Sections 5 and 6 of the recon then demonstrate that this scope has
**no identity-needing consumer** -- no code path queries a crater or
overlay tri by spatial click, and the producer APIs (`gos_PushTerrainOverlay`,
`gos_PushDecal`) take only `(verts3, texHandle)` with no source of identity
to fill an `objectIdRaw` from.

Issuing handles for a population that has no first consumer would be
**pre-speculative substrate without a named first user** -- the exact
anti-pattern the M2.6 META-FIX scope discipline pushed back against.

The recon recommends DEFER M5 pending user clarification, or rescope to a
narrower target with a real consumer. This spec exists to surface that
decision to the user with the trade-offs enumerated, not to commit code.

**M5 does NOT block any other slice.** M3 (terrain), M4 (VFX), and any
future kind can proceed without M5 clarification. The `Overlay=4`
reservation is an enum comment, not a load-bearing API contract; it can
remain unreserved indefinitely or be renumbered if pressure for the value
ever arises (unlikely -- the enum has 256 slots).

---

## 1. Purpose / non-goals

### Purpose

Force a single, explicit user decision before any implementation work
proceeds on slice M5. The decision space is enumerated in section 5
("The Q every user needs to answer first").

### Non-goals (load-bearing)

- **NOT an implementation spec.** No 5-questions answered. No handle
  range allocated. No shader plumb proposed. No tier1 gate defined.
- **NOT a plan.** A plan can only be written after the user picks one
  of the rescope options OR declines all of them. The plan author then
  produces a scope-narrowed spec first.
- **NOT a recon.** The recon already exists at
  `docs/superpowers/explorations/2026-05-23-renderworld-slice-m5-overlay-recon.md`.
  This spec consumes that recon and translates it into a decision
  document.
- **NOT a roadmap edit.** M3 / M4 ordering and prioritization is out of
  scope. This document only asks "what does M5 mean?"; M3/M4 sequencing
  is a separate conversation.

---

## 2. The seven in-tree meanings of "overlay"

Distilled from recon section 2. Each row is one distinct system that uses
the string "overlay" in its name, file, or enum. "Rendering surface?"
answers whether the meaning is a world-space draw pass that could
plausibly live behind a RenderWorld adapter.

| # | Meaning | Rendering surface? | File:line evidence | Notes |
|---|---------|--------------------|--------------------|-------|
| 1 | Terrain-overlay splat (cement perimeter, runway transitions, road decals around buildings) | YES | `shaders/terrain_overlay.{vert,frag}`; `mclib/quad.cpp:1678,1685,1694,1701,1859,1947,2107,2193` producers; `GameOS/gameos/gameos_graphics.cpp:1481,7308` push; batch members at `:1808-1848` | Mission-static. Lit inline (no deferred shadow). |
| 2 | Decal splat (bomb craters, mech footprints) | YES | `shaders/decal.frag`; `mclib/crater.cpp:563,572` producers; `gameos_graphics.cpp:1482,7313` push | Fixed ring buffer: `craterManager->init(1000, ...)` (`code/mission.cpp:2211`); 64 footprint slots (`mclib/crater.cpp:46`). |
| 3 | Map-tile semantic overlay (road / bridge / runway tile classifier) | NO | `mclib/mapdata.h:39-60` (`enum Overlays { DIRT_ROAD..NUM_OVERLAY_TYPES=17 }`); consumed at `code/goal.cpp:223,232,255,266` for pathfinding | Pure data. Already baked into terrain texture via #1. |
| 4 | `MC_OverlayType` atlas record (per-overlay texture pages + transitions) | NO | `mclib/terrtxm.h:87-95,102,122,135,152-162,184-185` | Build-time descriptor for #1. |
| 5 | `MC2_TERRAIN_INDIRECT_OVERLAY` env (kill switch for the indirect bake of #1's producer output) | NO | `GameOS/gameos/gos_terrain_indirect.cpp:215`; CLAUDE.md "Known issues" entry | Default-ON since `60f2ef8`. Toggles a fast-path, not a surface. |
| 6 | `Mover::overlayWeightClass` (AI weight-class metadata) | NO | `code/mover.h:712,910,1157-1162` | Gameplay state. Not visual. |
| 7 | Debug visualization overlays | YES (debug only) | `mclib/projectz_overlay.{h,cpp}`; `GameOS/gameos/gos_postprocess.cpp:829-845` (`drawShadowDebugOverlay`); `:607` (`overlayPass` uniform = pass discriminator); `code/missiongui.cpp:255,2815-2818` (Ctrl+Alt+O `drawTerrainOverlays` toggle); `:3150` (`eye->projectForDebugOverlay`) | Hotkey-toggled diagnostics. No identity. |

Categories the original M5 framing might suggest but that do not exist as
separate render surfaces in MC2:

- **HUD overlays** (health/ammo/reticle) live in HUD infra
  (`gos_State_IsHUD`, `flushHUDBatch` at `gameos_graphics.cpp:1305`) with
  its own command buffer at `:1617`. Screen-space; consumed by hardcoded
  UI click handlers, not pixel readback. Not a RenderWorld candidate.
- **In-world unit labels / billboards:** none beyond HUD-2D. Mech
  selection rings draw inline with the mech path.
- **Minimap markers:** tactical map renders to its own framebuffer; the
  viewport-rect overlay block at `code/gametacmap.cpp:206-210` was already
  deleted in the Phase-1 carve-out 2026-05-19.
- **Mission-script / cinematic / editor overlays:** not found as render
  surfaces in the runtime path. (Mission editor `Viewer/` is a separate
  target outside the recon scope.)

### Why the strongest candidate (#1 + #2) still fails

Recon section 4 documents that `terrain_overlay.frag` and `decal.frag`
both write only `layout(location=0) out FragColor` and (under
`MRT_ENABLED`) `layout(location=1) out GBuffer1`. Neither shader writes
`layout(location=2) out uint v_objectId` -- the M1.5 ObjectID substrate
slot. So `RenderWorld::lookupAtPixel(x, y)` cannot return a decal or
overlay handle today.

Recon section 5 then documents the consumer audit (verifiable via the
opposite-direction grep rule in CLAUDE.md):

- `getCrater`, `craterAt`, `pickCrater`, `decalAt` -- **zero hits**
  repo-wide.
- `code/mover.cpp:23` mentions of "overlay" are all `overlayWeightClass`
  (category #6 -- AI, not visual).
- The Ctrl+Alt+O `drawTerrainOverlays` toggle at `code/missiongui.cpp:255`
  is a **global show/hide flag**, not a per-tri pick.

There is no consumer to satisfy. Issuing handles would be substrate
without a first user.

---

## 3. Three plausible rescopes for M5

If the user does not want to defer M5 outright, three rescopes have a
real consumer. Each is much smaller than the M2/M2.5/M2.6 trio.

### Rescope A: terrain-overlay + decal GPU port (perf migration)

**Pre-existing stub spec:**
`docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`
(~100 lines, predates the RenderWorld arc).

- **Scope:** CPU -> GPU port of `gos_PushTerrainOverlay` /
  `gos_PushDecal`. Producers in `mclib/quad.cpp` and `mclib/crater.cpp`
  emit through a GPU-indirect path instead of the per-frame
  `gosRenderer::pushTerrainOverlayTri` / `pushDecalTri` immediate-mode
  submission.
- **Type:** **perf migration** (CPU-side cost reduction), NOT an
  identity slice. No handle issuance. No RenderWorld API surface
  involvement.
- **Lean against M5 framing:** This work lives naturally under the
  `MC2_TERRAIN_INDIRECT_OVERLAY` story (already default-ON since the
  Stage-6 flip `60f2ef8`; remaining endpoint is a Tracy substitutive
  proof per CLAUDE.md "Known issues"). It is **misframed** as a
  RenderWorld slice -- there's no identity, no handle, no `lookupAtPixel`
  consumer.
- **If chosen:** rename to "M5-perf: overlay/decal GPU port" to
  distinguish from identity slices, and route through the existing stub
  spec rather than the RenderWorld migration guide's 5-questions
  template (which assumes identity issuance).
- **Greybeard ruling required:** likely PATCH (justified) because it
  perpetuates the legacy producer API rather than retiring its bug class.

### Rescope B: cursor-hover kind indicator

- **Scope:** Hook into the existing `tryGameplayPick` spine
  (`code/gameplay_pick.cpp`) on hover (not just on Shift+click), then
  surface the kind-tag of what's under the cursor (`StaticProp` / `Mech`
  / future `Terrain` / `Vfx`) into the HUD or a debug text overlay.
- **Type:** **consumer of M2.6 / M3 substrate**, not a peer slice. No
  new RenderWorld kind. No handle range. No shader plumb.
- **Surface area estimate:** ~200 lines of code. A small HUD/debug text
  draw + a `MissionInterfaceManager` hover hook that calls
  `tryGameplayPick` at hover-cadence (~10 Hz, not per-frame).
- **Trade-off:** Useful for QA and dev iteration; trivial pixel cost.
  Not a substantive "slice" in the M2 sense -- could ship as a
  single-PR enhancement under any milestone.
- **Why this is M5-eligible:** because the recon notes "the implicit
  roadmap may have placed Overlay=4 where a UX/dev tool would more
  naturally fit." If the user's mental model of M5 was "give me a way to
  see what RenderWorld thinks is under the cursor," this is the
  smallest answer.

### Rescope C: debug visualization adapter

- **Scope:** Render an overlay that visualizes pick results -- e.g. a
  kind-color-coded heatmap of recent picks, or a screen-space label
  drawn next to whatever the cursor is over showing kind / index /
  generation. Env-gated `MC2_RENDER_WORLD_DEBUG_OVERLAY=1`.
- **Type:** **consumer of M2.6 substrate**, no new substrate. Could
  reach into the M1.5 ObjectID R32_UINT attachment-2 to visualize the
  whole frame's handle distribution.
- **Surface area estimate:** ~400 lines (a small fullscreen-pass
  shader + CPU draw + env gate + HUD integration). Higher than Rescope
  B because it includes a real shader pass.
- **Trade-off:** Greybeard debugging value is high; ship cost moderate.
  Could be the canonical "show me what RenderWorld owns" tool the arc
  has been missing -- analogous to `MC2_TGL_POOL_TRACE` for pools.

### Rescope C variant: spawn at greybeard request

If user picks (d), worth considering whether the debug viz lives in
`RenderWorld/debug_viz.{h,cpp}` (engine-side; reaches into the substrate
directly) or `GameAdapters/RenderWorldDebugAdapter.{h,cpp}` (boundary-
respecting; only consumes public API). Spec section 11 of the M2.6
boundary discipline favors the latter; recon section 7b sketched the
former. Decision deferred to the rescope spec.

---

## 4. Why this matters to schedule

- **M3 (terrain pickup), M4 (VFX), and any future kind can proceed
  without M5 clarification.** The `RenderObjectKind` enum has 256 slots
  and per the CLAUDE.md entry "values are stable across releases --
  never renumber; only append" simply leaving the `Overlay=4`
  reservation unused costs nothing.
- **M5 should not block other slices.** If the user picks (a) full
  deferral, M3 / M4 plans land first and reference M5 as "reserved
  pending consumer."
- **The `Overlay=4` enum value can be un-reserved if pressure ever
  exists.** Comment in `RenderWorld/RenderWorld.h:134` is the only
  artifact; deleting the comment requires no API change. The recon notes
  this is unlikely to matter -- enum slot allocation is not a scarce
  resource.
- **Naming hygiene:** if Rescope A is chosen, renaming the slice from
  "M5 / Overlay" to "M5-perf: overlay/decal GPU port" prevents future
  confusion between identity slices and perf slices. The same risk
  applies if Rescope B or C is chosen -- "cursor-hover kind indicator"
  and "debug viz adapter" should NOT inherit the M5 / Overlay name
  because they are consumers, not new kinds.

---

## 5. The Q every user needs to answer first

### Q1 (LOAD-BEARING) -- What does M5 mean to you?

Pick one. All five options are acceptable; the recon-recommended lean is
**(a) defer indefinitely** OR a tie between **(c) cursor-hover indicator**
and **(d) debug viz adapter** (both are tiny and have real consumers).

| Option | Action | Recon lean |
|--------|--------|------------|
| (a) | **Defer M5 indefinitely.** No consumer exists today. Reclaim the implicit roadmap slot for whatever surfaces later. M3 / M4 proceed unaffected. | **PREFERRED** if no concrete use case exists. |
| (b) | **Rescope A: terrain-overlay + decal GPU port.** Promote the pre-existing stub spec (`docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`) and rename to "M5-perf." Recognized as perf migration, not identity slice. | Acceptable IF the user explicitly wants the CPU->GPU perf work now AND accepts the misframing tax. |
| (c) | **Rescope B: cursor-hover kind indicator.** Tiny consumer of M2.6 / M3 substrate. ~200 lines. Ship as a UX enhancement under a separate name. | Acceptable; very low cost; concrete value. |
| (d) | **Rescope C: debug viz adapter.** Tiny consumer of M2.6 substrate + small shader pass. ~400 lines. Ships the canonical "show me RenderWorld" tool. | Acceptable; moderate cost; high greybeard value. |
| (e) | **Other.** User has a specific use case the recon missed. Surface it and a new recon will be produced scoped to that use case. | Acceptable; produces a new recon, not this spec. |

---

## 6. If user picks (a) -- full deferral

- **No code change.** Spec lands as historical artifact.
- **Update sites:**
  - `docs/renderworld_migration_guide.md` slice table: add row "M5 ---
    DEFERRED (no first consumer; clarification doc 2026-05-23)".
  - `RenderWorld/RenderWorld.h:134` enum comment: leave as-is OR change
    to `// Future: Terrain=2, Vfx=3, (Overlay reserved -- deferred,
    see clarification 2026-05-23-renderworld-slice-m5-overlay-spec.md)`.
  - CLAUDE.md "Active campaigns" section: NO new entry (deferred
    work is not an active campaign).
  - MEMORY.md INDEX-RENDERING.md: add a one-line pointer to this spec
    so future Claude sessions land on the clarification before
    re-deriving the recon.
- **Future re-scope:** spawns a new slice named "M5-redux" or similar,
  with its own recon scoped to the named consumer.

---

## 7. If user picks (b) -- perf migration rescope

- **Promote pre-existing stub:**
  `docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`
  becomes the basis for the M5-perf spec. The stub predates the
  RenderWorld arc and uses different language; it needs a refresh pass
  to integrate Tracy proof methodology and substitutive-vs-additive
  greybeard rulings.
- **Rename slice:** "M5-perf: overlay/decal GPU port" -- the "-perf"
  suffix signals this is not an identity slice and does not follow the
  5-questions template.
- **Greybeard ruling required:** likely PATCH (justified). The named
  META-FIX would be retiring `gos_PushTerrainOverlay` /
  `gos_PushDecal` entirely in favor of a producer-side GPU bake -- this
  may be larger scope than the current stub.
- **Tracy gate:** per the CLAUDE.md `MC2_TERRAIN_INDIRECT_OVERLAY`
  entry, the remaining endpoint for that story is "user-driven
  substitutive non-COST_SPLIT Tracy proof + decal visual canary."
  Rescope A inherits that gate.
- **5-questions template:** does NOT apply. This is a perf slice, not a
  RenderObjectKind slice. The migration guide section 4 questions are
  about identity allocation; perf slices have a different shape.

---

## 8. If user picks (c) -- cursor-hover kind indicator

- **Substrate:** none needed. Consumes M2.6's `tryGameplayPick` spine
  directly.
- **Shader plumb:** none needed. ObjectID buffer is already written by
  static-prop (M1.5) and mech (M2.5) shaders under env gate.
- **Surface area:** ~200 lines:
  - HUD/debug-text draw call (or a minimal label widget).
  - `MissionInterfaceManager` hover hook -- call `tryGameplayPick` at
    ~10 Hz (NOT per-frame -- pixel readback stalls the GPU per the
    migration guide section 1 note on `lookupAtPixel`).
  - Env gate `MC2_HOVER_KIND_INDICATOR=1` (or equivalent name) to keep
    it off by default.
- **Greybeard ruling required:** likely PATCH (justified) -- additive
  UX feature with no obvious bug class to retire. Or arguably no
  ruling needed because it's a pure consumer of existing META-FIX'd
  substrate.
- **Name:** SHOULD NOT inherit "M5 / Overlay." Suggested name:
  "HoverKindIndicator (consumer slice under M2.6 substrate)."

---

## 9. If user picks (d) -- debug viz adapter

- **Substrate:** none needed. Consumes M1.5 ObjectID
  `GL_COLOR_ATTACHMENT2` directly, plus M2.6's
  `GameplaySelectionDebugState` for the most-recent-pick visualization.
- **Shader plumb:** one new fullscreen-pass fragment shader (likely
  `shaders/render_world_debug.frag`) that reads the R32_UINT attachment
  and produces a kind-color visualization. NO new writes to the
  ObjectID buffer.
- **Surface area:** ~400 lines:
  - Fullscreen quad draw infrastructure (may already exist as
    `gos_postprocess.cpp` helper).
  - New shader pair (vert is trivial; frag does the kind-color lookup
    + label rendering).
  - Env gate `MC2_RENDER_WORLD_DEBUG_OVERLAY=1`.
  - HUD integration for the per-pick label (kind + index + generation
    + debugCookie if it's a known type).
- **Greybeard ruling required:** likely META-FIX of the
  greybeard-debugging bug class -- "no way to see what RenderWorld
  thinks is under each pixel" is a real diagnostic gap that this slice
  retires.
- **Name:** SHOULD NOT inherit "M5 / Overlay." Suggested name:
  "RenderWorldDebugOverlay (debug viz consumer of M1.5 + M2.6
  substrate)." Note that this slice uses the word "Overlay" in its name
  -- but in the **debug-overlay** sense (category #7 in section 2), not
  the **decal/terrain-overlay** sense (categories #1 + #2). If chosen,
  spec should explicitly disambiguate in its header.

---

## 10. Handle range -- no allocation yet

Per the migration guide section 3 (kHandleBase strides), M5's slot
would naturally fall at `kOverlayHandleBase = 0x00040000u` (262144) on
the 64K-stride schema. But:

- **Rescope A** is a perf migration; allocates no handles.
- **Rescope B** consumes existing kinds (StaticProp + Mech + future
  Terrain); allocates no new handles.
- **Rescope C** consumes existing kinds; allocates no new handles.
- **Full deferral (a)** leaves the slot reserved with no allocation.

In **all** five Q1 outcomes, no handle range allocation happens at this
spec's level. The partitioning decision is deferred to whichever
rescope (if any) ships, and only if that rescope actually issues new
handles. If the answer is "none of (a)..(e) issue handles," the
allocation conversation never needs to happen.

---

## 11. The five questions (deliberately unanswered)

The migration guide section 4 mandates that every new `RenderObjectKind`
spec answer five questions verbatim:

1. What creates/destroys the handle?
2. What kind does it report?
3. Does it write object ID? (M1.5 substrate yes/no, and via which
   mechanism)
4. How does lookup/pick/debug consume it? (`findXByHandle` semantics)
5. What legacy fallback remains?

**This spec deliberately does not answer them.** Each rescope in
section 3 implies a different set of answers (Rescope A answers all
five with "N/A -- this is a perf slice"; Rescopes B and C answer them
with "consumes existing kinds, no new kind issued"). The answers
crystallize only after Q1 is resolved.

If Q1 is answered with (e) and a new substantive RenderObjectKind
emerges, a new recon + spec will produce the five answers. Until then,
the questions are intentionally TBD because the spec is intentionally
not at execute-phase entry.

---

## 12. Resolved decisions

| # | Decision | Status | Notes |
|---|----------|--------|-------|
| D1 | M5 is a clarification request, not an implementation. | RESOLVED | Per recon recommendation. No code change ships from this spec. |
| D2 | M5 does not block M3 / M4 / M6. | RESOLVED | Per CLAUDE.md "values are stable across releases -- never renumber; only append" -- Overlay=4 can remain unallocated indefinitely. |
| D3 | Handle range allocation is deferred. | RESOLVED | No rescope option requires allocation at this spec's level. |
| D4 | The five-questions template is deferred. | RESOLVED | Applies at the rescope spec level (or not at all if Rescope A is chosen). |
| D5 | If a rescope is chosen, the slice should be renamed away from "M5 / Overlay." | RESOLVED | Prevents future name collision (e.g. "M5-perf" / "HoverKindIndicator" / "RenderWorldDebugOverlay"). |
| **Q1** | **What does M5 mean?** | **OPEN -- load-bearing for user review** | See section 5. |

---

## 13. Open questions for human

### Q1 (LOAD-BEARING) -- What does M5 mean to you?

Recon-recommended lean: **(a) defer indefinitely** is preferred because no
consumer exists today and substrate-without-consumer is the anti-pattern
the M2.6 META-FIX discipline pushed back against. Tied second: **(c)
cursor-hover kind indicator** (~200 lines) and **(d) debug viz adapter**
(~400 lines), both of which have real consumers and modest cost.

Options:

- **(a)** **Defer M5 indefinitely.** No consumer exists; reclaim the
  slot. M3 / M4 proceed unaffected. Update migration guide + enum
  comment; no code change.
- **(b)** **Rescope A: terrain-overlay + decal GPU port.** Promote the
  pre-existing stub spec at
  `docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`.
  Rename to "M5-perf." Recognized as perf migration, not identity.
- **(c)** **Rescope B: cursor-hover kind indicator.** Tiny consumer of
  M2.6 / M3 substrate. ~200 lines. Ship under a name like
  "HoverKindIndicator," not "M5 / Overlay."
- **(d)** **Rescope C: debug viz adapter.** Tiny consumer of M2.6
  substrate + small shader pass. ~400 lines. Ship under a name like
  "RenderWorldDebugOverlay" with explicit disambiguation of which
  "overlay" sense applies.
- **(e)** **Other.** User has a specific use case the recon missed.
  Surface it; a new recon scoped to that use case will be produced
  before any spec/plan work.

### Q2 (consequential of Q1) -- If (a), should the enum comment be updated?

- **(a.i)** Leave `RenderWorld/RenderWorld.h:134` as
  `// Future: Terrain=2, Vfx=3, Overlay=4` -- preserves implicit
  roadmap optionality.
- **(a.ii)** Change to `// Future: Terrain=2, Vfx=3, (Overlay reserved
  -- deferred 2026-05-23, see spec)` -- explicitly records the
  deferral.

Recon lean: (a.ii) for grep-discoverability from future sessions
landing on the enum.

### Q3 (consequential of Q1=(b)) -- If perf rescope, who runs the Tracy proof?

The pre-existing stub spec at
`docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`
predates the substitutive-proof methodology. The rescope spec must add
a substitutive non-COST_SPLIT Tracy proof gate (parallel to the one in
`MC2_TERRAIN_INDIRECT_OVERLAY` story per CLAUDE.md Known Issues entry)
and identify who runs it (user-driven smoke is the standard).

---

SPEC STATUS: DRAFT -- clarification-pending; 1 load-bearing Q for user

**Q1:** What does M5 mean to you?
- **(a)** Defer M5 indefinitely
- **(b)** Rescope A: terrain-overlay + decal GPU port (perf migration)
- **(c)** Rescope B: cursor-hover kind indicator (~200 LOC consumer)
- **(d)** Rescope C: debug viz adapter (~400 LOC consumer)
- **(e)** Other -- user has a specific use case the recon missed

No plan will be written until Q1 is answered. If Q1 resolves to (a),
the artifact is a one-line migration guide update + (optional) enum
comment edit, no plan. If Q1 resolves to (b)/(c)/(d), a scope-narrowed
spec is produced first, then a plan against that spec. If Q1 resolves
to (e), a new recon scoped to the named use case is produced first.
