# Slice 2 (object-offload arc) — Recon Zero prompt

> **Role for a fresh session reading this:** You are picking up the
> object-offload arc at slice 2. Slice 1 closed 2026-05-02 (substrate
> only — render-path replacement behind `MC2_GPU_OBJECTS=1`). Slice 2 is
> the actual perf slice — GPU vertex lighting for the same population
> (buildings + trees + generics). It is **gated on Recon Zero** below.
> Do NOT write a slice-2 spec until Recon Zero is complete.

---

## Worktree + branch

- **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Branch:** `claude/nifty-mendeleev` (the long-running nifty development branch)
- **Current tip (slice 1 close):** `dd8761a feat(objects): Gate F counters + summary emission + late-registration accounting`
- **Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.3/`

## What slice 1 shipped (2026-05-02)

Eight commits + Tracy cleanup, all on `claude/nifty-mendeleev`:

```
dd8761a Stage 1.D — counters + summary + late-reg + txmmgr flush gate fix + run_smoke fix
f911931 Stage 1.C — GenericAppearance::render
3e36aa5 Stage 1.C — TreeAppearance cull-bypass fix-up
327cf61 Stage 1.C — TreeAppearance::render initial
a81c28e Stage 1.B — RAlt+0 toggle guard
9677a15 Stage 1.B — BldgAppearance::render
48b3394 Tracy cleanup in quad.cpp (orthogonal)
574a5bb Stage 1.A.bis — submitMultiShape signature refactor (no-op)
75c8cf9 Stage 1.A — g_useGpuObjects flag plumbing
```

Slice 1 = **render-path replacement only**. The per-actor CPU `TG_Shape::Render`
emit is replaced for buildings/trees/generics under `MC2_GPU_OBJECTS=1`,
with NO cull bypass and NO update-time changes. Per-vertex lighting
bake (`TransformMultiShape` → `TransformShape`) continues to run on CPU
for every cull-survivor every frame. Slice 1 is substrate; perf is
deferred to slice 2.

## What slice 2 is supposed to do

Move per-vertex lighting bake to a GPU vertex shader. Per Q0 of the
brainstorm, that's the ~2.4 ms `TerrainObject::update appearanceUpdate`
zone — the dominant CPU cost for static-prop work. Replace
`TG_Shape::TransformShape`'s per-vertex lighting kernel with VS-side
lighting: read per-vertex normals + per-frame light list from SSBOs,
compute lit ARGB in the VS, drop the CPU lighting pass.

This is the actual perf slice. Bound above by ~2 ms recoverable;
realistic recovery TBD per Recon Zero results.

## REQUIRED READS (in order — non-skippable)

1. **Brainstorm:** `docs/superpowers/brainstorms/2026-05-02-object-offload-scope.md`
   — the full design context. Q1 (prior-attempt failure analysis),
   Q4 (population scoping), Q5 (parity), and Q7's Recon Zero discussion
   are the load-bearing parts for slice 2.

2. **Slice 1 spec:** `docs/superpowers/specs/2026-05-02-object-offload-slice1-design.md`
   — the substrate that slice 2 builds on. The SSBO + custom shader +
   Layer-B + Gate F infrastructure is reusable as-is.

3. **Worktree CLAUDE.md** — load-bearing project rules:
   - Documentation Discipline (grep at write-time)
   - Review Discipline (adversarial review for high-stakes plans)
   - Load-Bearing Cull Infrastructure (still applies)
   - Critical Rules (build / deploy / shader version)

4. **Memory files (load-bearing):**
   - `memory/cull_gates_are_load_bearing.md` ⭐
   - `memory/tgl_pool_exhaustion_is_silent.md` ⭐
   - `memory/mc2_texture_handle_is_live.md`
   - `memory/static_prop_projection.md`
   - `memory/gpu_direct_renderer_bringup_checklist.md`
   - `memory/feedback_offload_scope_stock_only.md`
   - `memory/feedback_smoke_no_canary.md` (do NOT run menu canary)
   - `memory/feedback_pool_peak_compare_same_mission.md` (Gate E methodology)
   - `memory/feedback_subagent_no_cmake_configure.md` (build env safety)
   - `memory/object_update_cost_baseline.md` if exists, else
     re-derive from brainstorm Q0 (2.4 ms appearanceUpdate / 900 µs render
     / 4 ms total objects)

5. **Adversarial review skill:** `.claude/skills/adversarial-plan-review.md`
   — slice 2's spec, when it lands, MUST go through this skill before
   plan write. Slice 2 qualifies as architectural-endpoint-class.

---

## Recon Zero (BLOCKING precondition for slice 2 spec)

**Question:** Can CPU `TransformShape` (per-vertex lighting bake) be
removed for buildings/trees/generics without breaking other systems
that consume what TransformShape produces?

**Initial grep evidence (from brainstorm):**

- `TG_Shape::TransformShape` at `mclib/tgl.cpp:1687-1690` allocates
  `listOfVertices`, `listOfColors`, AND `listOfShadowTVertices` from
  the TGL pools.
- `TG_Shape::RenderShadows` at `mclib/tgl.cpp:3262` consumes
  `listOfShadowTVertices`. References at `mclib/tgl.cpp:2562, 2808,
  2819, 2906, 3022`. The last writes
  `eye->projectForScreenXY(listOfShadowVertices[index].position,
   listOfShadowTVertices[index].transformedPosition)` — meaning the
  shadow path reads transformed positions that TransformShape produces.
- `BldgAppearance::renderShadows` at `mclib/bdactor.cpp:1924-1926`
  calls `bldgShape->RenderShadows()` directly. Same pattern at
  `TreeAppearance::renderShadows` (`mclib/bdactor.cpp:4144`) and
  `GenericAppearance::renderShadows` (`mclib/genactor.cpp:1015`).

**Implication:** CPU shadow path consumes `listOfShadowTVertices`.
Slice 2 cannot just delete `TransformShape` — that breaks shadows.

**Three branching answers (must pick one before spec):**

- **(2-a) Move shadows to GPU as part of slice 2.** The prior
  killswitched batcher had `flushShadow()` Task 13-14 stubbed — never
  shipped. This option subsumes that work into slice 2. Larger scope;
  perf win is preserved.
- **(2-b) Keep a reduced CPU `TransformShape` pass that produces only
  `listOfShadowTVertices` (no lighting bake).** Partial offload. Perf
  win shrinks proportional to how much of the 2.4 ms lives in
  positions vs. lighting. Recon must measure this split.
- **(2-c) Disable shadows for the GPU population temporarily.** Visual
  regression; not acceptable. Listed only for completeness — pick
  (2-a) or (2-b).

## Recon Zero work (this session's deliverable)

Produce a recon document at:
`docs/superpowers/explorations/<YYYY-MM-DD>-object-offload-slice2-recon-zero.md`

The recon must:

### 1. Fully enumerate consumers of state TransformShape produces

Grep ALL of:

- `listOfVertices` — both `TG_Shape::listOfVertices` and references
- `listOfColors` — `TG_Shape::listOfColors`
- `listOfShadowTVertices` — `TG_Shape::listOfShadowTVertices`
- `aRGBHighlight`, `fogRGB`, `lightsOut`, `isWindow`, `isSpotlight`
  (per-shape state read by render paths)
- Any per-shape state that is written inside `TransformShape` /
  `TransformMultiShape` and read outside them

For each consumer site, list: `file:line`, function/call site, what
it reads, and which lifecycle phase it's in (update / render /
renderShadows / selection / debug).

This step is the load-bearing one — DO NOT skip a class of consumers.
A consumer overlooked at recon time becomes a slice 2 regression at
default-on flip time. Use both forward grep (`TG_Shape::listOfVertices`)
and reverse grep (any `->listOfVertices`, `->listOfColors`,
`->listOfShadowTVertices` reads).

### 2. Decompose the 2.4 ms `appearanceUpdate` cost

Add ZoneScopedN sub-zones inside `BldgAppearance::update`,
`TreeAppearance::update`, `GenericAppearance::update`, and
`TransformMultiShape` to measure:

- `SetTextureHandle` per-frame rewrite (`mclib/msl.cpp:1365`)
- Hierarchy traversal + child shapeToWorld compute
- Per-leaf `TransformShape` invocations
- Within `TransformShape`: per-vertex transform vs. per-vertex
  lighting bake (the kernel slice 2 wants to move)
- `getVerticesFromPool` allocations + `multiSetLightList`
  (or whatever the lighting-list iteration shape is — grep at recon
  time; the brainstorm deferred this)

Goal: figure out what fraction of the 2.4 ms is lighting (slice 2
target) vs. machinery slice 2 doesn't move. Realistic perf bound for
slice 2 is the lighting fraction minus shadow-pass cost preserved.

Per Tracy zone-overhead memory rule: don't add zones with MTPC < 1µs;
use rdtsc accumulators for finer slices.

After measuring, **REMOVE the recon zones** (or leave gated behind a
`MC2_OBJECT_RECON_TRACY=1` env flag) — don't ship instrumentation
overhead in the default build.

### 3. Grep + read the active lighting model

`multiSetLightList` is named in the prior killswitched design's
deferred-lighting note but the brainstorm didn't grep its actual
location/shape. Recon must:

- Grep for the function definition.
- Read it: light types supported (directional, point, spot?), falloff
  math, light-list iteration shape, normal-vs-position dependence.
- Determine GLSL portability: which math ops? sqrt? per-vertex
  conditional branches? large light lists?
- Estimate per-vertex GLSL cost vs. per-vertex CPU cost.

Output: a "GPU lighting feasibility" subsection with the lighting
model summarized, GLSL port complexity estimated, and any showstopper
flagged (e.g., light counts > 16 not supported by simple per-vertex
loop).

### 4. SSBO budget

For slice 2 we need:

- Per-vertex normals — already on the prior batcher's type VBO via
  the registration pass. Verify by grep'ing
  `gos_static_prop_batcher.cpp` for the vertex layout — what
  attributes are there?
- Per-frame light list — bounded by `eye->getWorldLights()` count.
  Grep for the active light-list size; typical mission count?
- Per-instance state: matches what slice 1 already uploads. Verify.

Output: a "schema additions for slice 2" subsection — what new SSBO
fields, what binding slots, what byte layout.

### 5. Parity strategy for slice 2

Slice 1's parity surface was per-instance: matrices + visibility +
colors. Slice 2's parity surface is **bytewise lit ARGB** — comparing
CPU `listOfVertices[j].argb` against GPU-computed VS output. This is
harder because:

- FP ULP-tolerance compare needed (CPU and GPU FP units may differ
  by sub-ULP).
- The legacy CPU path doesn't expose its intermediate per-vertex
  ARGB at a clean comparable point — slice 2 may need to instrument
  the CPU lighting kernel.
- Pixel-level screenshot diff (Q5(iii) of brainstorm) is more robust
  but tooling-heavy.

Three options:

- (P1) ULP-tolerance bytewise: works but needs careful threshold.
- (P2) Pixel-level screenshot diff: robust, requires deterministic
  camera + animation phase + lighting state pinning. Same harness
  needed for slice 1 default-on (Stage 1.E) — could be built once
  and used for both.
- (P3) Dual-emit one frame at start of mission: CPU + GPU both run,
  bytewise-compare each pass-through, then disable CPU bake.
  Implementation cost: moderate.

Recon picks one with reasoning.

### 6. Branching answer decision (2-a vs 2-b)

Based on the consumer enumeration (step 1) AND the cost decomposition
(step 2):

- If shadow consumer count is small (e.g., only the three
  `*Appearance::renderShadows` paths) AND the shadow-pass cost
  preserved under (2-b) is small: **pick (2-b) partial offload**.
  Perf win is bounded but slice 2 stays scope-tractable.
- If shadow consumer count is large OR (2-b)'s preserved cost wipes
  out most of the perf win: **pick (2-a) move shadows to GPU too**.
  Larger scope but higher recoverable perf.

Recon picks one with explicit cost-benefit reasoning grounded in the
two prior steps' measurements.

### 7. Risk inventory

For each prior killswitched-attempt failure mode (Q1(b1-b4) of the
brainstorm), document slice 2's compensating mechanism. The slice 1
substrate already addresses (b1-b3); slice 2 needs to address:

- Behind-camera projection guard (b4): still applies. Slice 2's VS
  inherits the slice 1 guard.
- New: per-vertex lighting kernel divergence between CPU and GPU
  (the FP-tolerance question above). What's the parity-failure mode
  if VS produces sub-ULP-different lit values?
- New: light-list update cadence. CPU side updates lights at varying
  frequencies (per-mission, per-frame, per-tick). GPU SSBO needs the
  right cadence — too frequent and we waste bandwidth; too rare and
  lighting goes stale.

### 8. Output format

A recon doc with:

- **Front matter:** date, branch, slice number (2), prior commits.
- **Section 1:** consumer enumeration table (file:line + lifecycle
  phase + reads-what).
- **Section 2:** cost decomposition (Tracy data: percentages of
  `appearanceUpdate` attributable to each sub-zone).
- **Section 3:** lighting model (function name, file:line, math summary,
  GLSL feasibility verdict).
- **Section 4:** SSBO budget (new fields + binding slots + bytes).
- **Section 5:** parity strategy decision (P1/P2/P3 + reasoning).
- **Section 6:** branching decision (2-a or 2-b + reasoning).
- **Section 7:** risk inventory (table mirroring brainstorm Q8 +
  slice-2-specific additions).
- **Section 8:** code-grounding verification appendix (every cited
  symbol grep-confirmed at write time, file:line + matches/doesn't).
- **Closing:** ready-for-spec / blocked-on-X.

Length target: 600-1000 lines (it's a recon, not a brainstorm — depth
matters).

---

## After Recon Zero

If recon concludes "ready-for-spec":

- The next session writes the slice 2 design at
  `docs/superpowers/specs/<YYYY-MM-DD>-object-offload-slice2-design.md`.
- Spec MUST go through adversarial review (`.claude/skills/adversarial-plan-review.md`)
  before plan write. Slice 2 is architectural-endpoint-class.
- Then plan + subagent-driven execution per the slice 1 pattern.

If recon concludes "blocked-on-X":

- Surface to user. The most likely block is a consumer found in step 1
  that has no clean compensation path — at which point the arc may
  pause until a separate cleanup slice retires that consumer.

---

## Out of scope for slice 2

- Animated movers (mechs, GVs). Per-node animation requires per-instance
  bone matrices the prior batcher doesn't support. Separate brainstorm
  + arc.
- Removal of legacy `g_useGpuStaticProps` and the 5 cull-bypass sites.
  Post-arc cleanup; mechanical follow-up after default-on flip soaks.
- Stage 1.E pinned-camera screenshot diff harness. Gates default-on
  flip for slice 1, NOT slice 2. May land in parallel; if slice 2's
  parity strategy picks P2 (pixel-level diff), the harness becomes a
  shared dependency.

---

## Project constraints (load-bearing — read worktree CLAUDE.md too)

- **Stock missions only** for validation (`memory/feedback_offload_scope_stock_only.md`).
- **Never run the menu canary** (`memory/feedback_smoke_no_canary.md`).
- **Build:** `cmake --build build64 --config RelWithDebInfo --target mc2`
  ONLY. Do NOT run `cmake -B build64 -S .` or any configure variant —
  the SDL2/GLEW prefix paths get clobbered. See
  `memory/feedback_subagent_no_cmake_configure.md`.
- **Deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.3/`. Per-file `cp -f` +
  `diff -q`. NEVER `cp -r`.
- **Pool peak comparisons:** same-mission baseline-vs-test only
  (`memory/feedback_pool_peak_compare_same_mission.md`).
- **Git:** never push. HEREDOC for commit messages. NEVER amend.
- **Tracy zones with MTPC < 1µs:** don't add them; use rdtsc
  accumulators or simple timing deltas. See orchestrator's
  "Working principles" section.

---

## Handoff

If you (next session) approve this prompt: invoke `superpowers:brainstorming`
or proceed directly to recon — the recon is well-scoped enough that a
brainstorm phase may be redundant. Recon is a research deliverable;
brainstorm is for design questions. Pick whichever fits.

If recon turns up a load-bearing surprise (consumer not in the initial
grep, lighting model that GLSL can't port cleanly, parity strategy that
doesn't work): surface to user before plan write.

The slice 1 close is on `claude/nifty-mendeleev`. Slice 2 builds on top.
No rebase / cherry-pick needed — work continues on the same branch.
