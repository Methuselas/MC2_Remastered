# Object Offload — Slice 1 (Static-Prop Render Path) — Design

Date: 2026-05-02
Worktree: `nifty-mendeleev`
Author: ThranduilsRing + Claude (Opus 4.7, 1M context)
Brainstorm: [`brainstorms/2026-05-02-object-offload-scope.md`](../brainstorms/2026-05-02-object-offload-scope.md)
Arc: object offload (NEW); slice 1 of a 2-slice arc.
Status: design draft, awaiting review.

## Slice scope (single sentence)

Replace the per-actor CPU render emit (`bldgShape->Render(...)` and
analogues) for static-prop multishapes — buildings, trees, generics —
with a GPU-resident batched draw path, **without** bypassing the
existing CPU cull cascade and **without** moving any update-time
work to the GPU.

## What slice 1 explicitly does NOT do

These are slice 2 (gated on Recon Zero) or out-of-scope:

- Move per-vertex lighting bake to the GPU. CPU `TransformMultiShape`
  → `TransformShape` → `listOfVertices[j].argb` continues to run for
  every cull-survivor every frame. That's where the 2.4 ms lives;
  this slice does not move it.
- Touch the CPU shadow path. `bldgShape->RenderShadows()` and
  analogues continue to consume `listOfShadowTVertices` produced by
  `TransformShape`. Slice 1 does not affect shadows.
- Bypass `inView` / `canBeSeen` / `objBlockInfo.active` / `objVertexActive`.
  The cull cascade is unchanged.
- Bump TGL pool sizes. The 500K / 128MB pool footprint already in
  tree (set by the prior killswitched attempt) stays as-is. Slice 1
  consumes from existing pools because `TransformShape` is unchanged.
- Touch animated-mover populations (`Mech3DAppearance`, `GVAppearance`).
  Their per-node animation requires per-instance bone matrices; out
  of scope for the entire object-offload arc until a separate
  brainstorm.
- Default-on flip. Slice 1 ships behind `MC2_GPU_OBJECTS=1` (default
  off) and stays flagged until either render-zone Tracy delta is
  material OR slice 2 lands in the same arc.

## Problem statement

Per Tracy data (brainstorm Q0):

| Zone | Cost | Source |
|---|---|---|
| `TerrainObject::update appearanceUpdate` | ~2.4 ms | `code/terrobj.cpp:607` |
| All-objects render | ~900 µs | (user-supplied) |
| Other (mover update etc.) | ~700 µs | |
| **Total objects** | ~4 ms | |

Slice 1 targets the **render** zone: ~900 µs of per-actor `gVertex[3]`
build + per-triangle `mcTextureManager->addVertices` + downstream
`renderLists()` flush, scattered across `TG_Shape::Render` instances
called from `BldgAppearance::render` / `TreeAppearance::render` /
`GenericAppearance::render`.

The architectural justification for shipping slice 1 even though it
moves only ~900 µs is **substrate**: slice 2 (GPU vertex lighting,
the ~2 ms perf slice) needs the per-instance SSBO + shader + Layer
B + packet table infrastructure that slice 1 builds. They cannot
collapse into one slice because slice 2's parity surface (bytewise
lit ARGB) is harder than slice 1's (per-instance matrix + visibility)
and slice 2's perf bound is gated on Recon Zero, which slice 1 is
not.

## Architecture

The prior killswitched attempt (`docs/superpowers/specs/2026-04-19-gpu-static-prop-renderer-design.md`)
already shipped much of the infrastructure to tree as
`GameOS/gameos/gos_static_prop_batcher.{h,cpp}`. Slice 1 **reuses
that infrastructure** with the following changes:

1. New env-gated flag `MC2_GPU_OBJECTS=1` → new global `g_useGpuObjects`
   (default false), distinct from the legacy `g_useGpuStaticProps`.
2. The five cull-bypass sites (`code/bldng.cpp:1071`, `code/terrobj.cpp`,
   `code/gate.cpp`, `code/artlry.cpp`, `mclib/mech3d.cpp:4183`,
   `mclib/gvactor.cpp:2039`) are NOT touched by `g_useGpuObjects`.
   They remain gated only on `g_useGpuStaticProps` (legacy). Slice 1
   does not need them.
3. `*Appearance::render()` is modified to check `g_useGpuObjects`
   FIRST and submit through the new path. Falls back to legacy CPU
   path on failure. Legacy `g_useGpuStaticProps` path stays as-is
   for backward compatibility but is marked deprecated.
4. New per-frame counters surface "GPU actually drew something"
   (Gate F per brainstorm), populating an `[OBJBATCHER v1]` log line
   gated by `MC2_OBJBATCHER_TRACE=1` plus an always-on 600-frame
   summary.
5. New aggregate late-registration accounting (per-type counts +
   allowlist), populating `[OBJBATCHER v1] event=late_register`.

### Data flow per frame

```
GameObjectManager::update            (objmgr.cpp:1756-1784, gated by objBlockInfo.active)
  └─ for each cull-surviving actor:
       └─ obj->update()
            └─ appearance->recalcBounds()           (computes inView, unchanged)
            └─ if (inView) appearance->update()     (terrobj.cpp:603-609)
                  └─ TransformMultiShape(...)       (bdactor.cpp:2171; bakes
                                                     listOfVertices[j].argb,
                                                     allocates listOfShadowTVertices)

Render phase
GameObjectManager::render           (objmgr.cpp:1487-1511, gated by objBlockInfo.active)
  └─ for each cull-surviving actor:
       └─ obj->render()
            └─ if (canBeSeen() || g_useGpuStaticProps)
                 └─ appearance->render(depthFixup)
                      └─ if (g_useGpuObjects && eligibleForBatcher(shape))
                           └─ batcher.submitMultiShape(shape)        ← slice 1 fast path
                                ├─ append per-instance record
                                ├─ memcpy listOfVertices[j].argb into per-instance color SSBO
                                └─ counters++
                      └─ else if (g_useGpuStaticProps)
                           └─ legacy bypass-cull GPU path (unchanged, deprecated)
                      └─ else
                           └─ shape->Render(...)                      ← legacy CPU path
            └─ shape->RenderShadows()                                  ← unchanged in slice 1

End-of-frame (after mcTextureManager->renderLists())
  └─ batcher.flush()
       ├─ upload thin records + color SSBO via persistent-coherent map
       ├─ for each type with submitted instances:
       │    bind type's textures (resolved at draw time per
       │    memory/mc2_texture_handle_is_live.md)
       │    glDrawElementsInstancedBaseVertex(...)
       └─ summary counters

Shadow phase                        (unchanged in slice 1)
  └─ legacy CPU shadow path consumes listOfShadowTVertices
```

### Eligibility predicate (`eligibleForBatcher`)

Eligibility is evaluated **per-child, with the multishape batchable
if at least one child is batchable.** Per-child failures skip the
child individually and are accounted as `skipped_children`; they do
NOT cause whole-multishape CPU fallback. (Whole-multishape fallback
firing on ~100% of inputs is the prior attempt's Layer-B failure
class — encoding it in the eligibility definition would re-introduce
the bug. The Layer-B per-child semantics already in
`gos_static_prop_batcher.cpp` (commit `1585db1`) are correct; this
spec preserves them.)

A multishape is batchable iff:

1. **Population gate.** The shape's lifecycle class is non-mover.
   Buildings, trees, generics pass; mechs and vehicles do not.
   Enforced by routing: slice 1 only modifies `BldgAppearance::render`,
   `TreeAppearance::render`, `GenericAppearance::render` to consult
   `g_useGpuObjects`.
2. **Type-registration gate.** At least one child shape's
   `TG_TypeShape*` (resolved via `shape->myType` at submit time per
   `gos_static_prop_batcher.cpp:414`) was registered at
   `finalizeGeometry()` time. If ALL children are unregistered (late
   registration or never registered), the multishape falls back to
   the CPU path for that frame and `[OBJBATCHER v1] event=late_register
   type=<typeName>` is logged on first occurrence per type. The
   single-child-unregistered case is per-child skip, not
   whole-multishape fallback.
3. **Per-child eligibility** (handled inside `submitMultiShape`):
   helper nodes, daytime spotlights (null `listOfVertices`), null
   `listOfColors` are skipped individually. Each skip increments
   `skipped_children`.

### Render order (CRITICAL)

Per `memory/render_order_post_renderlists_hook.md`: `batcher.flush()`
**must** be invoked AFTER `mcTextureManager->renderLists()` because
the legacy queue drains during `renderLists()` and re-overwrites the
GPU-direct depth buffer. The hook site is `mclib/txmmgr.cpp` (was
~1340 in prior handoff; verify at implementation time).

### Bridge state save/restore

Per `memory/static_prop_projection.md` and `memory/gpu_direct_renderer_bringup_checklist.md`,
`batcher.flush()` must save and restore:

- `GL_CURRENT_PROGRAM`, `GL_VERTEX_ARRAY_BINDING`, `GL_ARRAY_BUFFER_BINDING`,
  `GL_ELEMENT_ARRAY_BUFFER_BINDING`
- Per-unit `GL_TEXTURE_BINDING_2D`, `GL_SAMPLER_BINDING` (we touch unit 0)
- All SSBO bindings we use
- `GL_DEPTH_TEST` enable, `GL_DEPTH_WRITEMASK`, `GL_DEPTH_FUNC`
- `GL_CULL_FACE` enable + `GL_CULL_FACE_MODE`
- `GL_BLEND` enable

Per `memory/blend_state_inheritance_in_post_process.md` and
`memory/gpu_direct_depth_state_inheritance.md`: explicitly set the
state we need (don't trust inheritance):

- `glEnable(GL_DEPTH_TEST)`, `glDepthFunc(GL_LEQUAL)`, `glDepthMask(GL_TRUE)`
- `glDisable(GL_BLEND)` for opaque draws (alpha-test stays as
  `discard` in fragment shader)
- `glEnable(GL_CULL_FACE)`, `glCullFace(GL_BACK)`

VAO 0 trap: `gos_RendererRebindVAO()` at start of bridge per
`memory/projectz_overlay_findings.md`.

## Shaders

Reuse from prior attempt with verification:

- `shaders/static_prop.vert`: D3D-style projection chain per
  `memory/static_prop_projection.md`. Includes the `clip4.w < 0.1`
  behind-camera guard from commit `ea96c13` — kept as defense-in-depth
  even though slice 1's submit set is cull-survivor-only.
- `shaders/static_prop.frag`: alpha-test via `discard` gated by
  `packet.materialFlags & ALPHA_TEST_BIT`. Debug-mode cycle (RAlt+9)
  retained for diagnostic use during bring-up.

Shadow shaders (`static_prop_shadow.vert/.frag`) exist in tree but
are NOT used in slice 1; `flushShadow()` stays a no-op. Shadow path
remains CPU-side via `bldgShape->RenderShadows()`.

## AMD invariants (verbatim from prior design where still applicable)

1. Position at `layout(location = 0)` on every VAO.
2. `gl_FragDepth` written in any depth-only frag shader. (N/A
   for slice 1; depth-only path is shadow, which slice 1 does not
   use.)
3. Sampler unit 9 unbind discipline. (N/A for slice 1; we don't
   touch the shadow FBO.)
4. Per-batch program/material re-apply + direct-uniform re-upload.
   The bridge issues `glUseProgram` + sets uniforms on every
   `flush()`, never assumes prior-pass state survived.
5. Matrix transpose discipline: per-instance `modelMatrix` in SSBO is
   `v*M`, terrain-style `worldToClip` uniform uploaded with `GL_TRUE`.
   Mixing direct and deferred is banned by construction.
6. No `sampler2DArray` on this path (per-packet texture bind is the
   prior design's choice; not changed here).

## Killswitch / env gating

```
MC2_GPU_OBJECTS=1                 → g_useGpuObjects = true (default false)
MC2_OBJBATCHER_TRACE=1            → enables per-frame [OBJBATCHER v1]
                                     submit + fallback prints; default off
                                     (always-on 600-frame summary independent)
```

`g_useGpuObjects` is read once at startup from `getenv("MC2_GPU_OBJECTS")`,
exposed as `extern bool g_useGpuObjects` in `gos_static_prop_killswitch.h`
(extending the existing header).

The legacy `g_useGpuStaticProps` global, RAlt+0 toggle, and its
five cull-bypass sites are NOT touched by slice 1. Marked deprecated
in code comments; physical removal is post-arc cleanup, not slice 1.

The RAlt+9 fragment-debug-mode cycle is retained.

## Migration stages

Slice 1 is small enough to land in 2-3 commits:

### Stage 1.A — Infrastructure rename (no behavior change)

- Add `g_useGpuObjects` global to `gos_static_prop_killswitch.h` /
  `code/mechcmd2.cpp`.
- Read `MC2_GPU_OBJECTS` env var in `gameosmain.cpp` at startup.
- Add `[INSTR v1] enabled: ...` banner extension for `objbatcher`
  field.
- No call sites use `g_useGpuObjects` yet; CPU path is bit-identical
  to current behavior.

**Gate:** tier1 5/5 PASS, +0 destroys, +0 visual delta. Confirms
the new flag plumbing doesn't accidentally enable the legacy
killswitch path.

### Stage 1.B — Wire `BldgAppearance::render` to the new path

- In `mclib/bdactor.cpp:1546+` (`BldgAppearance::render`), add the
  **mutually-exclusive** wiring (slice 1 wins; legacy is fully
  disabled when slice 1 is on):

```cpp
bool submittedToGpu = false;
if (g_useGpuObjects && bldgShape) {
    // Slice 1 path. No cull bypass; submitMultiShape is per-child Layer-B
    // by construction. Returns false only when EVERY child is
    // ineligible (rare; logged via event=late_register).
    submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(bldgShape);
}
// Legacy bypass-cull path is gated on !g_useGpuObjects so the two
// flags are mutually exclusive at runtime. R1 in this spec.
if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && bldgShape) {
    submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(bldgShape);
}
if (!submittedToGpu) {
    // legacy CPU render path (unchanged)
    ...
}
```

- The RAlt+0 runtime toggle that flips `g_useGpuStaticProps` (in
  `GameOS/gameos/gameosmain.cpp`) gets a guard: if `g_useGpuObjects`
  is true, the toggle is ignored and `[OBJBATCHER v1]
  event=legacy_toggle_blocked` is logged once. This prevents a
  mid-session keystroke from putting the process into a hybrid
  cull-bypass state while slice 1 is active.

- `flush()` invocation at the existing post-`renderLists()` hook
  is unchanged.

- Counters per brainstorm Gate F (definitions tightened per advisor):
  - `eligible_actors` = actors of buildings/trees/generics whose
    `Appearance::render` was reached this frame WITH `g_useGpuObjects`
    enabled. (Actors gated out by `objBlockInfo.active` /
    `objVertexActive` upstream are NOT counted; only actors that
    actually arrived at the new wiring point.)
  - `submitted_instances` = multishape-level submits where the
    batcher accepted at least one child.
  - `submitted_children` = per-child submits that were packed into
    the SSBO.
  - `skipped_children` = per-child submits that were Layer-B-rejected
    (helper / spotlight / null colors).
  - `cpu_fallback_instances` = actors where every child was
    ineligible AND the actor fell back to CPU render (or to the
    legacy `g_useGpuStaticProps` path when both were live before
    R1's mutual-exclusion guard — under R1, this term reduces to
    the CPU-only path).
  - `gpu_drawn_instances` = instances actually issued via
    `glDrawElementsInstancedBaseVertex` in `flush()`.

  Surface via `[OBJBATCHER v1] event=summary` every 600 frames and
  on shutdown.

**Gate:** tier1 5/5 PASS in three configs (unset, `MC2_GPU_OBJECTS=1`,
`MC2_GPU_OBJECTS=1 + MC2_OBJBATCHER_TRACE=1`). Visual canary at
fixed camera with a building-heavy mission. Render-zone Tracy
delta on `MC2_GPU_OBJECTS=1`: target ≥30% reduction or "didn't make
it worse" (whichever fires depending on draw aggregation effectiveness).

### Stage 1.C — Same wiring for trees + generics

- Apply the same pattern to `TreeAppearance::render` (`mclib/bdactor.cpp:3984`)
  and `GenericAppearance::render` (`mclib/genactor.cpp:771`).
- Eligibility predicate is unchanged; the existing `submitMultiShape`
  works for all three populations.

**Gate:** tier1 5/5 PASS in same three configs. Per-population
counter (`gpu_drawn_instances_buildings`, `_trees`, `_generics`)
in summary line.

### Stage 1.D — Late-registration accounting (commit-with-1.C)

- Aggregate per-type late-registration counter table.
- `[OBJBATCHER v1] event=late_register type=<typeName> count=N`
  emitted on first occurrence per type and at 600-frame summary.
- Allowlist file: `data/objbatcher_late_register_allowlist.txt`,
  one type-name per line. **Initial allowlist is empty — strict
  mode.** Entries are added only after observing a name in the
  log, confirming via grep that the type is genuinely out-of-scope
  (e.g., late-spawned artillery), and explicitly approving the
  add. Adding to allowlist before observing is forbidden — the
  point of strict mode is to surface unknown registration paths,
  not paper over them.
- Gate in test plan: zero unexpected late registrations.

### Stage 1.E — Pinned-camera screenshot diff harness (separate PR, gates default-on)

This is **NOT** in the slice 1 merge PR. It's a separate piece of
test infrastructure that gates default-on flip:

- `tests/smoke/object_visual_diff.py`: deterministic camera-pin
  + screenshot capture + tolerance-based diff against a baseline
  reference.
- Baseline captured with `MC2_GPU_OBJECTS=0` (CPU path).
- Diff captured with `MC2_GPU_OBJECTS=1`.
- Gate: pixel-diff under tolerance threshold (TBD; estimate
  ≤0.5% pixels diffed by ≤2 LSB).

Slice 1 PR may merge before this exists; default-on flip cannot
happen before this passes.

## Files

### New

- (none — `gos_static_prop_batcher.h/.cpp` already in tree)
- `data/objbatcher_late_register_allowlist.txt` (empty initially)
- `tests/smoke/object_visual_diff.py` (Stage 1.E, separate PR)

### Modified

- `GameOS/gameos/gos_static_prop_killswitch.h`: add `extern bool g_useGpuObjects`
- `code/mechcmd2.cpp`: define `g_useGpuObjects`, read env at startup
- `GameOS/gameos/gameosmain.cpp`: `[INSTR v1]` banner extension
- `GameOS/gameos/gos_static_prop_batcher.cpp`:
  - Add `submitted_instances`, `gpu_drawn_instances`,
    `cpu_fallback_instances`, `eligible_actors` counters
  - `[OBJBATCHER v1] event=summary` 600-frame log + shutdown
  - Aggregate late-registration table + allowlist load
  - `[OBJBATCHER v1] event=late_register` log on first per-type
- `mclib/bdactor.cpp`:
  - `BldgAppearance::render` (line 1546): new `g_useGpuObjects`
    branch before existing `g_useGpuStaticProps` branch
  - `TreeAppearance::render` (line 3984): same pattern
- `mclib/genactor.cpp`:
  - `GenericAppearance::render` (line 771): same pattern

### Touched but not behavior-changed

- `mclib/mech3d.cpp:4183`, `mclib/gvactor.cpp:2039`, `code/bldng.cpp:1071`,
  `code/terrobj.cpp`, `code/gate.cpp`, `code/artlry.cpp`: all retain
  their existing `g_useGpuStaticProps` cull-bypass terms unchanged.
  Slice 1 does not bypass cull, so it does not add new bypass terms.
  Comments may be added marking the legacy bypass as deprecated.

## Test plan / gate ladder

### A. Visual canary

Side-by-side at fixed camera on a building-heavy mission (mc2_01
airbase region recommended). RAlt+0 retained for the legacy path;
new path toggle requires restart with `MC2_GPU_OBJECTS=1`.

Pass: no visible regression at fixed camera.

### A'. Pinned-camera screenshot diff

Per Stage 1.E. **Pre-condition for default-on flip, NOT for flagged
merge.** May lag the slice 1 PR.

### B. Tracy delta on render zone

Target on the `Render.3DObjects` zone with `MC2_GPU_OBJECTS=1`:
no regression; render-zone reduction acceptable but not gated.

Gate is "didn't make it worse." Reasoning: slice 1's value is
substrate, not perf. Honest framing per brainstorm Q1(a4) is to
state this in the PR description, not paper over it with a perf
target the architecture can't deliver.

### C. Parity (Gate F per brainstorm)

`MC2_OBJBATCHER_TRACE=1` summary every 600 frames AND at shutdown:

```
[OBJBATCHER v1] event=summary frames=N
  eligible_actors=E
  submitted_instances=S submitted_children=SC skipped_children=KC
  cpu_fallback_instances=F gpu_drawn_instances=G
  fallback_rate=F/E
  submit_buildings=B submit_trees=T submit_generics=GE
```

`eligible_actors` is defined precisely: actors of
buildings/trees/generics that reached `Appearance::render` this
frame with `g_useGpuObjects` enabled. (Cull-rejected actors gated
out upstream by `objBlockInfo.active` or `objVertexActive` are NOT
counted; the count is the population the batcher had a chance to
accept.)

`cpu_fallback_instances` is defined as: actors where every child was
ineligible (e.g., all children are unregistered types) AND the
actor fell back to CPU render. Per-child Layer-B skips show up as
`skipped_children`, NOT here.

Gate:
- `G > 0` (GPU actually drew something — catches the prior
  attempt's Layer-B failure mode).
- `F / E < 0.05` (whole-multishape fallback rate under 5%).
- `B > 0 && T > 0 && GE > 0` (all three populations active).
- `SC > 0` (at least one child shape is being submitted, in
  case multishape submits succeed structurally but every child
  is per-child-skipped — would otherwise hide as G == 0 with
  S > 0).

Slice 1 fails the gate if any of these miss.

### D. tier1 5/5 PASS triple

Configs: unset / `MC2_GPU_OBJECTS=1` / `MC2_GPU_OBJECTS=1 + MC2_OBJBATCHER_TRACE=1`.
+0 destroys delta on every mission, every state.

### E. TGL pool peak ≤ pre-slice peak

Per `MC2_TGL_POOL_TRACE` summary. Slice 1 does not change which
actors call `TransformShape`, so pool consumption should be
unchanged. If peak rises, the slice has bypassed cull somewhere it
shouldn't have — fail the gate.

### F. Late-registration

Zero unexpected late registrations OR all observed types in the
allowlist file (`data/objbatcher_late_register_allowlist.txt`).
The two known types per the prior handoff (suspected
artillery/bomber spawns) need to be either added to the allowlist
or fixed at the registration site.

### Triggers (slice will not merge behind flag)

- Visual canary regression.
- Tracy regression on render zone (slowdown).
- Gate F: `gpu_drawn_instances == 0` or fallback rate ≥ 5%.
- Destroys delta != 0 in any tier1 mission.
- Pool peak rises above pre-slice baseline.
- Late-registration without allowlist coverage.

## Risks and open questions

### R1. Mutual exclusion of `g_useGpuObjects` and `g_useGpuStaticProps` (HARD INVARIANT)

**Slice 1's central safety claim is "no cull bypass." The legacy
killswitch's five cull-bypass terms (`mclib/mech3d.cpp:4183`,
`mclib/gvactor.cpp:2039`, `code/bldng.cpp:1071`, `code/terrobj.cpp`,
`code/gate.cpp`, `code/artlry.cpp`) remain in tree gated on
`g_useGpuStaticProps`. If both flags are simultaneously live, those
five sites force pre-cull `TransformShape` while slice 1 also fires
its submit path — a hybrid state that violates the safety claim.**

**Hard rule (not advisory):** the two flags are mutually exclusive
by construction. Encoded in:

1. **Per-actor wiring (Stage 1.B/1.C pseudocode above):**
   the legacy `g_useGpuStaticProps` branch is gated on
   `!g_useGpuObjects`. When slice 1 is on, the legacy path is
   unreachable from `*Appearance::render`.
2. **Runtime toggle guard (`gameosmain.cpp` RAlt+0 handler):**
   if `g_useGpuObjects` is true, RAlt+0 is silently ignored and
   `[OBJBATCHER v1] event=legacy_toggle_blocked` is logged once.
3. **The five bypass terms themselves are unchanged.** They
   remain dead code under slice 1 because `g_useGpuStaticProps`
   cannot become true while `g_useGpuObjects` is true (the toggle
   guard) and starts false at process launch.

This makes "no cull bypass under slice 1" an enforceable
invariant, not a convention.

### R2. Render-zone Tracy delta might be near-zero

If the render zone's CPU cost is dominated by `mcTextureManager`
queue management rather than per-actor `gVertex[3]` build, then
substituting our own draw doesn't move the needle even on the
render zone. The "not worse" gate handles this — slice still
ships as substrate — but the PR description should not promise
a render-zone improvement.

### R3. Animated buildings under LOD swap

`BldgAppearance::recalcBounds` swaps `bldgShape` pointer when LOD
changes (`mclib/bdactor.cpp:1373-1377`). Animated buildings have
LOD swap suppressed (`mclib/bdactor.cpp:1336-1366`) but non-animated
buildings can swap mid-mission. The batcher resolves `typeID` at
submit time from `shape->myType` (`gos_static_prop_batcher.cpp:414`),
so LOD swap correctly causes the actor to be associated with a
different `typeID` from one frame to the next. **Verify in
implementation:** all LOD variants of every building type are
registered at map-load (per the prior design's Layer-A enumeration),
otherwise post-LOD-swap the `typeID` lookup misses and the actor
falls back to CPU silently.

### R4. Layer B fallback-rate threshold (5%)

The 5% threshold is a starting point informed by the prior
attempt's ~100% failure mode but otherwise arbitrary. May need
tuning. If real fallback rate during clean operation is e.g. 2%
(consistent with the known late-registration count of 2 types/mission),
5% is comfortably above the floor; if it's 8% (unexpected child
node patterns or shadow-only types), we surface the cause and
either add to allowlist or refactor the eligibility predicate.

### R5. Stale memory file

`memory/bldg_animation_lod_swap_unsafe.md` describes the
LOD-suppression fix as pending on `agile-hopper`. The fix is
already on this branch (`mclib/bdactor.cpp:1336-1366`). Update
memory after slice 1 lands. Cosmetic but worth noting.

## Reference docs

- Brainstorm: [`brainstorms/2026-05-02-object-offload-scope.md`](../brainstorms/2026-05-02-object-offload-scope.md)
- Prior attempt design: [`specs/2026-04-19-gpu-static-prop-renderer-design.md`](2026-04-19-gpu-static-prop-renderer-design.md)
- Prior attempt handoffs: [`plans/progress/2026-04-19-static-prop-handoff.md`](../plans/progress/2026-04-19-static-prop-handoff.md), [`...20-static-prop-handoff.md`](../plans/progress/2026-04-20-static-prop-handoff.md), [`...20-static-prop-handoff-part2.md`](../plans/progress/2026-04-20-static-prop-handoff-part2.md)
- Cull lessons: [`docs/gpu-static-prop-cull-lessons.md`](../../gpu-static-prop-cull-lessons.md)
- Memory:
  - `memory/cull_gates_are_load_bearing.md` ⭐
  - `memory/tgl_pool_exhaustion_is_silent.md` ⭐
  - `memory/mc2_texture_handle_is_live.md`
  - `memory/static_prop_projection.md`
  - `memory/gpu_direct_renderer_bringup_checklist.md` (the 9 traps)
  - `memory/render_order_post_renderlists_hook.md`
  - `memory/feedback_offload_scope_stock_only.md`

## Slice 2 dependency

Slice 2 (GPU vertex lighting for the same population — the perf slice)
is gated on **Recon Zero**: enumerate every consumer of
`TG_Shape::listOfVertices`, `listOfColors`, `listOfShadowTVertices`
after `appearance->update()`. Initial grep evidence in the brainstorm
shows `RenderShadows` reads `listOfShadowTVertices` (`mclib/tgl.cpp:2562, 2808, 3022`)
which is allocated by `TransformShape` itself — meaning slice 2
cannot remove `TransformShape` without one of:

- (2-a) move shadows to GPU concurrently
- (2-b) keep a reduced CPU `TransformShape` pass for shadow data only
- (2-c) accept smaller perf win

Recon Zero must run before slice 2 spec is written. Slice 1 may
merge independently; nothing in slice 1 depends on Recon Zero
completing.
