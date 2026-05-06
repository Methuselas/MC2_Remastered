# Track A/B/C Brainstorm Decisions — 2026-05-06

> **Status:** decisions reached, pending outside-input review before plan-writing.
> **Scope:** consolidates 9 design decisions surfaced by the Track A/B/C
> recon-zero passes (recons at `docs/superpowers/explorations/2026-05-06-track-{a,b,c}-*-recon.md`).
> **Predecessor:** `docs/superpowers/mc3-rendering-modernization-roadmap.md`.
> **Successor:** implementation plan for Track A slice 1 (deferred until outside review).

---

## Framing

This doc resolves the design questions that the three parallel recons surfaced.
Decisions are weighted by two project memories:

- `mc3_modernization_philosophy.md` — GPU/modern by default; resource abundance is a tool, not a constraint.
- `open_rts_engine_framing.md` — engine and gameplay are separable layers; the boundary makes modernization easier, not harder.

Where a decision is contested, the framing memories are the tiebreaker.

---

## Decisions

### Q1 — Track A entry slice

**Resolved: object admission first, then effects, then re-evaluate terrain.**

Track A's roadmap default was "terrain first." The recon surfaced that the
project's own capture-replay data (`docs/superpowers/specs/projectz-capture-report.md` §5)
verdicts terrain admission with **High** confidence as the *worst* candidate
for predicate replacement: modern predicates over-cull terrain ~36% across
missions, and `rectGuard` permissive admissions concentrate at 74% on terrain
(the wedge-risk vector). Object admission has 1 site, smallest blast radius,
and is the lowest-risk validation of the swap pattern. Effects (7 sites)
follows once the pattern ships clean. Terrain becomes data-driven — re-run
captures with object+effects on the new predicate; only proceed to terrain
if the residual gap is worth the wedge risk.

### Q2 — Bool vs screen-oracle scope

**Resolved: dual-output wrapper. New predicate computes bool; legacy `projectZ`
computes `screen.x/y/z/w`. Slice scope stays narrow.**

The recon noted "all wedge-class callsites consume the screen output, not just
the bool" as an aggregate concern. A 5-minute grep at decision time confirmed
the single object admission callsite at `code/gameobj.cpp:2090` mutates
`screenPos` (a `GameObject` member) which is read across `mclib/mech3d.cpp`,
`mclib/gvactor.cpp`, `code/weaponbolt.cpp`, `code/actor.cpp`,
`mclib/bdactor.cpp`, `code/missiongui.cpp` and others. Pure bool-only is not
an option — the screen output is load-bearing for downstream HUD overlays
and gameplay code.

The dual-output wrapper preserves screen output via legacy `projectZ` math
while routing the bool through the new predicate. Compatible at the call
site; isolates the swap surface to the predicate decision only.

### Q3 — Track A parity definition

**Resolved (revised post-advisor-review):** dual-run legacy and modern
predicates with disagreement classification; failure is reserved for
unexpected lifecycle delta or out-of-envelope disagreements.

Earlier wording ("trace-counter parity = 0 disagreements") was wrong
on its face — a predicate replacement that produces zero disagreements
with the predicate it's replacing is a no-op. Disagreement is the *signal
of migration*, not the failure mode.

Operational definition:

- **Dual-run capture.** Run `MC2_PROJECTZ_TRACE` with both legacy and
  modern predicates active at the slice's site set. Record every
  disagreement (legacy admit / modern reject, and vice versa) for review.
- **Acceptance envelope.** A reviewed envelope of expected disagreements
  is established before flip — characterized by zoom level, camera angle,
  object class, distance band. Disagreements inside the envelope are
  expected migration signal, not failures.
- **Hard failure conditions:**
  - `[DESTROY v1]` count delta vs baseline (object lifecycle gate cascade).
  - Visual smoke regression on tier1 5/5 (HUD overlays misaligned, target
    reticles drifting, etc.).
  - Disagreements *outside* the reviewed envelope (e.g., at a zoom level
    or camera angle the envelope didn't account for).
- **Hard parity requirement (preserved):** `screen.x/y/z/w` output stays
  byte-identical to legacy via the dual-output wrapper (Q2). Bool
  disagreement is allowed; screen disagreement is not.

The `[DESTROY v1]` lifecycle gate cascade still anchors the cascade-safety
case (`memory/cull_gates_are_load_bearing.md`).

### Q4 — Track B registration timing

**Resolved: mission-load bulk register + register-on-spawn API for late types.
No first-render lazy fallback.**

The recon noted that artillery and vTOL units spawn at gameplay time, not at
mission load — they would miss a mission-load-only walk. The cleaner answer
under `open_rts_engine_framing.md` is that gameplay code (the spawn site)
tells the engine when a static prop appears, via an explicit
`registerStaticProp()` API. Mission-load enumeration becomes a bulk
optimization for known-at-load types; register-on-spawn handles late
arrivals.

This costs touching every spawn site for late types (artillery, vTOL,
mech-bay-mid-mission). The cost is paid willingly — it's the layer-separation
investment the framing memory says is aspirational today and operational
once TechScript or `UnitAppearance` unification lands. Front-loading it here
keeps the registry's semantics clean rather than entrenching first-render
lazy fallback as a permanent pattern.

### Q5 — Track B Generic scope

**Resolved: descope GenericAppearance from Track B. Two registration paths
preserved (world-static-prop registry vs HUD allowlist at commit `06ac847`).**

The only unregistered Generic instances across all 24 campaign missions are
`Cylinder01` (skybox) and `compassplane` — both HUD/non-world. Forcing them
into the same persistent instance buffer as world props would either violate
the registry's invariants (mission-stable transforms, sun/shadow lighting
eligibility, world-frustum cull) or require escape-hatch flags that
re-introduce the same complexity by another name. Cleaner: declare Track B's
scope as world-static-prop population explicitly. Two populations, two
registration paths, boundary documented.

### Q6 — Track B first-frame race

**Resolved (revised post-advisor-review):** structural fix by default —
pre-populate `cachedFrame_` at mission-load registration. The
`[STATIC_FIRST_FRAME v1]` counter ships alongside, but as proof the
structural fix works, not as a decision-deferral gate.

Mission-load registration runs *before* the first frame's `update()`
populates `cachedFrame_`. The existing registry's `flush` skips
stale-stamped entries (the black-tree-bug fix). Under Q4's mission-load
registration, the first frame post-registration would silently drop
every static draw without handling.

The earlier "verify first, escalate if needed" framing under-weighted the
severity. "Every static draw silently dropped on frame 0" is not a
maybe-imperceptible artifact — it's a guaranteed-visible blank-world
flicker if it actually fires. The structural fix is small (~5 lines: at
registration time, set `cachedFrame_` to `currentFrame - 1` so the flush
invariant treats the entry as valid-this-frame), removes the uncertainty
entirely, and matches the user's expressed preference for structural
fixes once mechanism is understood.

Counter usage:

- `[STATIC_FIRST_FRAME v1]` env-gated counter logs any entry that the
  flush would have skipped due to stale-stamp on frame 0. With the
  structural fix in place, this should always be zero.
- A non-zero count indicates the structural fix's pre-populated stamp
  doesn't satisfy the flush invariant — escalate to option C
  (pre-populate `cachedGpuLightIndex_` baseline) or revisit the flush
  invariant itself.
- Counter satisfies the worktree's debug-instrumentation rule for
  lifecycle reworks regardless.

### Q7 — Track C AI 1-frame artifact

**Resolved: accept. Engine-supplied (1-frame-lagged) visibility flows to all
consumers including AI/weapon-spawn. `Camera.UpdateRenderers` becomes a stub.**

The recon found combat-AI gate (`code/mech.cpp:6497`) and weapon-spawn-node
queries (`mclib/mech3d.cpp:721,759,795,833`, `mclib/gvactor.cpp:445,500,533`)
read `inView`/`canBeSeen()` synchronously. With Track C's async-readback
feedback, a mech that just becomes visible doesn't fire back for 1 frame;
weapon bolts spawn at object root for 1 frame before snapping to muzzle
position. **Visual artifact only, not correctness.**

This is the most architecturally consequential decision of the brainstorm.
Under `open_rts_engine_framing.md`, gameplay rebuilds from engine-supplied
visibility — entrenching engine knowledge of gameplay timing (option B,
preserve CPU `recalcBounds` for AI consumers) is exactly what layer
separation rejects. The user's Q4 selection of D (register-on-spawn API)
already signaled willingness to pay layer-separation costs; A on Q7 is
consistent with that posture.

**Justification framing (revised post-advisor-review):** the artifact is
accepted as a **gameplay-tolerance tradeoff**, not because it's
categorically imperceptible. At 60Hz one frame is ~16.7ms; at 30Hz one
frame is ~33ms; during hitch-recovery frame intervals can be larger.
Earlier "below human-perception floor" framing overclaims. Validate via
explicit visual canaries on zoom-transition, camera-jump, first-contact,
and weapon-spawn scenarios before accepting the tradeoff in shipped
configuration.

The artifact is reversible. If shipped behavior turns out observably annoying,
gameplay code can request synchronous visibility through a separate engine
API at specific call sites — paying the cost only at those sites, as a
content-layer concern. That hedge is documented but not pre-emptively built.

### Q8 — Track C compute dispatch shape

**Resolved: single combined dispatch with bucket-keyed scatter-write via
atomic counters.**

For ~10K static instances across 20-50 material buckets, dispatch overhead
isn't the binding constraint either way. Combined dispatch with atomic
per-bucket counters is the modern reference pattern (UE5, Frostbite,
Doom Eternal). Single fence per frame keeps Track C's async-readback ring
buffer simple. Modder sidecar layers will add buckets over time; B's
shader-side scatter scales without per-bucket dispatch loops.

AMD-specific risk noted: atomic counters in compute shaders are well-trodden
on 7900 XTX-era drivers, but `docs/amd-driver-rules.md` should be checked
during canary build before flip.

### Q9 — Track C compute output

**Resolved: visible-ID list with atomic compaction. B (compute writes
`DrawElementsIndirectCommand` directly) documented as future escalation.**

A's host-assembly pattern matches existing `gos_terrain_indirect`
infrastructure — Track C extends rather than parallels. Compute-shader
complexity stays minimal (atomic counter + visible-ID write per bucket).
Visible-ID list is independently useful: async-readback path consumes it
for AI gating, modder sidecar layers can subscribe cheaply, debug overlays
can render visibility decisions directly.

B's direct DrawIndirect-write pattern is the modern endpoint shape but
provides no measurable win at our scale. Promote later if profiling shows
A's vertex-shader indirection costs anything; demoting from B to A is
harder.

---

## Post-advisor-review additions (2026-05-06)

These decisions were not in the original Q1-Q9 set but were surfaced by
outside-input review and absorbed into the design before plan-writing.

### Q10 — Track C dynamic-actor visibility records (new pre-slice)

**Resolved: Track C0 ships before C1. Per-frame CPU upload of compact
visibility records for dynamic actors (mechs, GVs, gates, turrets,
weapon emitters), feeding the same compute cull pass as Track B's static
instances.**

Track B is explicitly world-static-prop scope. Track C as originally drafted
claimed it would feed AI/Mech/GV gates with GPU-derived visibility — but
those are dynamic actors, not in Track B's persistent buffer. Real gap.

C0 closes the gap with a small CPU-side per-frame upload:

- One record per dynamic actor: `id`, `worldAABB` (or sphere), `category`
  (Mech / GV / Gate / Turret / Other), `prevVisibilityBit`, gate-consumer
  flags (`hasAIConsumer`, `hasWeaponSpawnConsumer`).
- Common cull-input schema with Track B's static records — compute shader
  doesn't distinguish; same frustum + distance test runs over both.
- No vertex skinning dependency; bone matrices stay where they are
  (Track D's territory). C0 is purely a visibility substrate.

This makes Track C independent of Track D and preserves the "Camera::UpdateRenderers
becomes a stub" exit criterion — without C0, the criterion is unreachable
because actor visibility never moved off CPU.

### Q11 — Bucket key precision

**Resolved: bucket = mesh-range + shader-program + texture-binding-set +
VAO + index-type. One indirect command per mesh-range bucket, with
`instanceCount = visibleCount` from the GPU-written atomic. Material-only
bucketing is insufficient.**

Earlier wording ("one call per material bucket") was loose. OpenGL indirect
indexed commands carry per-mesh-geometry fields (`firstIndex`, `baseVertex`,
`count`); bucketing by material alone breaks unless every mesh in the
bucket shares pre-packed geometry tables.

Concrete shape (option A from advisor's enumeration):

- One bucket per `(mesh-range, shader, texture-set, VAO, index-type)` tuple.
- One `DrawElementsIndirectCommand` per bucket with mesh-static fields
  (`firstIndex`, `baseVertex`, `count`) populated at mission load.
- GPU writes per-bucket `instanceCount` from the atomic counter at compute
  time. Per-bucket `baseInstance` is also GPU-written if the visible-ID
  list is bucket-keyed (Q9 decision).

Options B (per mesh+material pair grouped for binding minimization) and
C (single material pass with shader-side mesh indirection) are NOT being
adopted in C1. They remain documented escalation paths if profiling
motivates.

### Q12 — Track C synchronization contracts

**Resolved: explicit barrier and reset/overflow/readback semantics
documented as part of Track C's plan.** This is not implementation detail
deferred to the plan — getting it wrong silently produces wrong-mesh /
stale-visibility / readback-stall failures that Tracy and parity checks
won't reliably catch.

Required contracts:

- **Counter representation (choose-and-stick).** Per-bucket counters are
  `uint` fields inside the cull-output SSBO, updated with shader
  `atomicAdd`. Not OpenGL atomic-counter buffer objects (ACBOs). This
  choice determines the barrier set below.
- **Counter reset path.** Per-bucket counters cleared at the start of
  every cull dispatch via `glClearNamedBufferSubData` on the SSBO region
  (or a small clear kernel). Reset must complete before the cull dispatch
  reads.
- **Visible-list capacity + overflow.** Per-bucket visible-ID list sized
  for worst-case (= total bucket population). Overflow handling: clamp
  the atomic counter via shader-side bounds check; emit `[GPU_CULL v1]
  overflow=` log entry; never silently drop. Overflow indicates registry
  population grew beyond capacity — escalate to capacity bump.
- **Per-bucket `instanceCount` patch location.** GPU-written into the
  pre-built `DrawElementsIndirectCommand` array's `instanceCount` field
  (offset = 4 bytes per std430 layout). Either compute writes the field
  directly, or a small "patch" kernel reads atomic counters into the
  command buffer.
- **Memory barriers (SSBO-atomics model).** Required between phases:
  - Cull dispatch → indirect draw consumption: `GL_COMMAND_BARRIER_BIT |
    GL_SHADER_STORAGE_BARRIER_BIT`. The SSBO bit covers both the
    visible-ID list AND the in-SSBO atomic counters (since they're
    SSBO-resident `uint` fields under the choose-and-stick decision).
  - Cull dispatch → CPU async readback (C2 only, not C1):
    `GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT` + `glFenceSync` for completion
    signalling.
  - **Do NOT add `GL_ATOMIC_COUNTER_BARRIER_BIT`** unless the
    implementation deliberately switches to ACBOs. The barrier bit is
    for ACBOs specifically; using it on SSBO-resident `atomicAdd`
    counters is a cargo-cult error.
- **Slice boundary (C1 vs C2).** C1 ships GPU→GPU only — one dispatch
  + required barriers + indirect draw per frame, no CPU readback except
  optional debug-only telemetry. The readback ring + fence + fallback
  paths begin in C2.
- **Async readback fallback (C2 contract).** CPU consumes only completed
  ring entries (frame N-1 with fence signaled). If frame N-1's readback
  isn't ready, fallback in priority order:
  1. Use frame N-2's last-good readback (still correct, slightly larger lag).
  2. Conservative-visible (assume all actors visible for that frame's gate
     decisions — pessimistic but never under-cull).
  3. Never block on `glClientWaitSync` in the render-loop hot path.

These contracts must be documented in the Track C C1 implementation plan
and verified at canary build time before any default-on flip.

### Q13 — GL version baseline vs Track C requirement

**Resolved: baseline GL 4.3 substrate preserved. Track C requires GL 4.6
OR (GL 4.3/4.4 + `ARB_indirect_parameters` + `ARB_shader_draw_parameters`)
verified at startup.**

The end-state document said "achievable in GL 4.3+ with extensions"; Track C
said "bump GL context from 4.3 to 4.6." Both true, but should be stated
together:

- Pre-Track-C engine baseline: GL 4.3 core context (matches `#version 430`
  shader prefix established in worktree CLAUDE.md).
- Track C requirement: `glMultiDrawElementsIndirectCount` (GL 4.6 core, or
  `ARB_indirect_parameters` extension on 4.3/4.4 drivers) and
  `gl_DrawID` / `gl_BaseInstance` access in vertex shader (GL 4.6 core,
  or `ARB_shader_draw_parameters` extension).
- Startup probe: at engine init, query GL version and extension support;
  refuse Track C activation if neither path is available; log
  `[GPU_CULL v1] gl_version=... support=4.6|extensions|none` for canary
  diagnosis.

AMD RX 7900 XTX supports GL 4.6 core directly, so the extension path is
mostly defensive. Stays useful for future hardware portability.

### Q14 — Track C split into C0/C1/C2/C3

**Resolved: Track C ships as four sequential slices, not one monolithic
~2-week chunk.**

- **C0 — GPU visibility record substrate.** Static registry records (from
  Track B) and dynamic actor visibility records (Q10) share a common
  cull-input schema. CPU per-frame upload of dynamic records. No compute
  cull yet; just the record substrate. Validates schema on AMD canary.
- **C1 — GPU cull for render draw only.** Compute shader fills per-bucket
  visible-ID lists and atomic counts. Indirect draw consumes them. CPU
  gates still use legacy `inView` — render path moves; lifecycle gates
  don't. This validates Q11 bucket layout, Q12 barrier contracts,
  capacity/overflow, and AMD compute-atomic behavior.
- **C2 — Async readback into non-lifecycle consumers.** CPU reads N-1
  visibility for debug overlays and non-destructive consumers (Tracy
  visibility plot, `[GPU_CULL v1]` summary). Validates the readback ring
  + fence + fallback paths without risking `setExists(false)` cascade.
- **C3 — Gate handoff / `Camera::UpdateRenderers` stub.** Only after C1+C2
  soak clean: route `objmgr::update`, `Mech3DAppearance::update`,
  `GVAppearance::update`, AI gate (`code/mech.cpp:6497`), and weapon-spawn
  queries to GPU-derived visibility. Then `Camera::UpdateRenderers`
  becomes a stub.

Sizing revised: C0 ~3-5 days, C1 ~1 week, C2 ~3-5 days, C3 ~1 week. Total
~3-4 weeks (up from monolithic ~2 weeks), but each slice has its own gate
and rollback. The big-bang risk of one-slice Track C is removed.

---

## Post-plan-pass-3 additions (2026-05-06)

These were not in the Q1-Q14 set; surfaced during post-plan-writing
advisor review of A2/B/C (after A1 had its own advisor pass-1).

### Q15 — Shared-soak discipline (cross-track ordering rule)

**Resolved:** isolate first, then soak the production-relevant joint
configuration. Sequential-with-overlap is the canonical pattern.

The pattern, written generally:

1. Slice X enters soak under all-other-slices-default.
2. Slice X passes its parity gate alone.
3. Slice X flips default-on.
4. Slice Y enters soak under X-already-modern (the production-relevant
   joint configuration).
5. Slice Y passes its own parity gate.
6. Slice Y flips default-on.

Why this and not parallel envelopes: regression attribution is the
load-bearing concern. If Slice Y fails under joint config but passed
isolated, Slice Y or the X-Y interaction is the culprit — clean
attribution. Parallel envelopes (both flags optional, both gates green)
look faster on calendar time but produce ambiguous "it passed alone"
claims if anything regresses jointly.

**Why this is brainstorm-worthy, not just plan-detail:** A2 surfaced it
explicitly; Track C's per-slice cadence (C0/C1/C2/C3) makes the rule
load-bearing because four sequential slices stack on each other. The
rule is now explicit so plan-writers don't have to re-derive it per slice.

**Concrete application:**

- A1 → A2: A1 soaks alone, flips, then A2 soaks under A1-modern, flips.
- A1+A2 → B: B soaks under A1+A2-modern joint baseline (the parallel
  alpha-test prep slice ships first per its own gate, separately).
- C0 → C1: C0 soaks under whatever's-current; C1 soaks under
  C0+B-modern joint baseline. Etc.
- The exception: independent prep slices (alpha-test prep, currently)
  may run on their own branch in parallel, with their own gate; they
  rejoin the main soak sequence on flip.

### Q16 — Track B `firstColorOffset` ownership

**Resolved:** Task 1's factoring spike CANNOT close without a written
decision on `firstColorOffset` ownership. The spike must commit to one
of three architectural answers, with rationale documented alongside the
spike outcome:

- **Bake-at-register (truly static).** Use only if Task 1 grep proves
  `firstColorOffset` is set at recipe build time and never mutated
  per-frame for any prop class. Track 3.C trees got away with this
  because they use fixed ARGB. **For wider Bldg population, prove this
  before commit** — building damage states / paint schemes / salvage
  states are candidates for per-frame mutation.
- **Patch-per-frame (explicit per-frame field).** `firstColorOffset`
  becomes a thin-record / per-frame SSBO field, patched in
  `GpuStaticPropRegistry::flush()`. Costs SSBO upload bandwidth per
  registered prop per frame; trivial at registry scale (~10K).
- **Recipe-field redesign (split static-base + per-frame-offset).**
  Recipe stores a static base; per-frame thin record stores a delta. Most
  invasive, but keeps the recipe genuinely immutable post-register.

**Why this is brainstorm-worthy, not just plan-detail:** the recon
flagged this as a "latent slice-3.C bug" but the plan-writer could not
commit to the resolution without architectural input. Wider Bldg
population may surface it as a correctness issue, not a polish issue.
The spike has no choice but to decide; making the decision-point
explicit prevents the spike from punting to "we'll patch it if it
breaks."

**How to apply:** Track B Task 1 (factoring spike) writes the decision
into the spike's outcome doc. Track B Task 2 implements per the chosen
answer. Reviewer at Task 1's PR-equivalent reviews the architectural
choice, not just the code change.

### Q17 — Track C block-active rollup

**Resolved:** per-actor visibility does NOT automatically answer
per-block visibility. Track C needs an explicit actor → block rollup
between the per-actor cull pass (C1) and the lifecycle gate handoff
(C3). Two implementation paths; pick at C1 plan-time:

- **GPU compute aggregation pass.** A small kernel that scans the
  visibility bitmask and emits a per-block `OR` reduction to a
  per-block visibility buffer. One additional dispatch + one additional
  barrier. Stays on GPU; consistent with the C-arc "GPU is the source
  of truth" frame.
- **CPU-side conservative rollup.** When CPU reads the per-actor
  visibility bitmask in C2's async readback, also walk
  `objBlockInfo[].firstHandle..firstHandle+numObjects` and OR the
  per-actor bits into per-block bits. Simpler, no extra GPU dispatch,
  but adds CPU work proportional to block count × per-block actor
  count.

**Why this is brainstorm-worthy:** `objmgr::update` (C3 gate target)
gates at block granularity (`objBlockInfo[].active`), not actor
granularity. Q11's bucket key is per-actor for draw purposes; the
visibility-feedback path needs an additional aggregation step nobody
called out at brainstorm time. Without it, C3's gate handoff is
incomplete.

**How to apply:** C1 plan must enumerate which path is being taken and
include the rollup kernel/CPU-walk in its sync-contract list (additional
dispatch in path 1 needs its own barrier; CPU-walk in path 2 needs to
happen before any C3-routed gate consumer reads).

### Q19 — DSA adoption arc (Track F candidate)

**Resolved (open future-arc):** Direct State Access (`ARB_direct_state_access`,
GL 4.5 core) replaces every `glBind*; glModify*` pattern with named-object
operations (`glNamedBufferSubData(buf, ...)` instead of
`glBindBuffer; glBufferSubData`). The codebase is currently 100%
legacy bind-style. Audit at session 2026-05-06 confirmed zero
production usage of `glCreateBuffers` / `glNamedBuffer*` /
`glCreateTextures` / `glTextureStorage*`.

**Why brainstorm-worthy:**

- Reduces driver state-tracking overhead per call.
- Eliminates a class of bind-leak bugs (the gosFX white-saturation
  bug from session log was a state-leak; bind-style operations are
  vulnerable).
- Code clarity — named-object calls are self-documenting.

**Why not a current-track concern:**

- Touches every buffer / texture / framebuffer / VAO operation in
  `GameOS/`, `mclib/`, `code/`. Multi-week mechanical refactor.
- Doing it during active modernization (Tracks A/B/C/D in flight)
  creates merge friction; better to land after the core arc ships.
- Zero behavior change; perf measurement is per-call-overhead which
  doesn't show up cleanly in Tracy until the arc is significant.

**How to apply:**

- Queue as **Track F: API surface modernization** for after Track E
  (legacy retirement).
- Track F is mechanical / file-by-file, no new design questions, but
  warrants a brainstorm before kickoff to enumerate scope (does it
  also fold in `glClipControl` Q20? `glMultiBind` audit? other GL 4.5+
  primitives?).
- Adversarial review applies (legacy retirement adjacent).

### Q20 — `glClipControl` adoption (oblique cull pays off here)

**Resolved (recommended now):** adopt `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`
at engine init alongside Track C work. Several active load-bearing
predicates assume D3D-style `[0, w]` depth convention as a *manual*
constraint that bleeds into shader code; clip control makes the
convention native and the manual handling drops out.

**Why this matters specifically for Track A/C cull work:**

The user-noted angle: the project's predicate replacement (Track A1's
`clipSpaceFrustumAdmit` and Track C0/C1's compute cull) all currently
test `0 <= rawClip.z <= rawClip.w` to enforce D3D-style depth
convention. Without `glClipControl`, the GL driver does an implicit
conversion from D3D-style to its native `[-1, 1]` somewhere in the
pipeline, AND our manual test assumes the pre-conversion convention.
The combination works but is fragile — every shader-side cull test has
to know which convention the matrix produces.

With `glClipControl(GL_ZERO_TO_ONE)` set once at engine init:

- The depth convention is `[0, 1]` natively at hardware level.
- Track A1 / Track C cull predicates' `0 <= z <= w` test is the
  hardware's native test, not a hand-applied constraint.
- `LegacyProjectionResult::rawClip` semantics align with what the
  hardware actually consumes.
- No projection-matrix gymnastics needed in shader code.

**The catch:** clip control is a global state, applied at the GL level
once. Switching it changes the depth convention for ALL subsequent
draws. Today's MC2 codebase passes D3D-style projection matrices
through but expects GL's `[-1,1]` Z convention in some legacy paths
(verify before flipping — a recon-zero pass would grep every shader
that touches `gl_Position.z` or reads depth-texture values, and audit
which convention each assumes). The **migration** is non-trivial
because behavior changes globally; the **steady state** is cleaner
than today.

**How to apply:**

- Queue as a recon-zero pass before Track C C0 (since C0 schema and
  C1 compute cull predicates are the biggest beneficiaries).
- Recon enumerates every depth-convention-touching shader and
  documents which convention each assumes.
- If recon finds a clean conversion path, ship `glClipControl` adoption
  as a small dedicated slice (pre-C0 prep, mirroring how alpha-test
  prep slice precedes Track B).
- If recon finds entrenched mixed-convention assumptions, defer to
  Track F.

**Estimated size:** recon ~3-5 days; if clean-path, slice ~3-5 days
including parity validation against legacy convention. Real
calendar-time win is the simplification it gives Track C compute
shader source.

### Q21 — HZB occlusion culling (camera model correction)

**Resolved (open candidate, brainstorm-worthy):** HZB (Hierarchical
Z-Buffer) occlusion culling is BACK on the candidate list after the
camera model correction (`memory/camera_model_oblique_cinematic.md`).
Earlier roadmap framing of "RTS top-down payoff too small" was
incorrect — the camera is oblique 30° + 360° + cinematic low-angle,
which surfaces real occlusion: mech behind mech, mech behind terrain
ridge, anything behind buildings, foreground-clip-out at low-angle
cinematics.

**What HZB adds beyond Track C frustum cull:**

Track C's compute cull tests bounding sphere/AABB against frustum
planes — admits anything inside the frustum regardless of whether
it's visible. HZB adds a depth-pyramid test:

- Build a depth-pyramid (mip chain of conservative max depth) once
  per frame from the depth buffer.
- In the cull pass, for each instance, project its bounding sphere
  to screen space, look up the appropriate mip level in the depth
  pyramid, and reject if the closest point is *farther* than the
  pyramid's max-depth at that screen region.
- Eliminates instances that are inside the frustum but occluded by
  closer geometry.

**For MC3 specifically:** at 30° elevation, ~30-50% of in-frustum
instances are typically occluded by foreground (mechs in front of
each other, buildings hiding mechs, terrain ridges hiding everything
behind them). HZB recovers that work. At cinematic low angles the
fraction can be higher.

**Two implementation shapes:**

- **Single-phase last-frame HZB.** Cull against last frame's depth
  pyramid. Simple. Catches steady-state occlusion. Misses
  disocclusions (newly-revealed geometry) for one frame; same
  1-frame-lag tolerance discipline as Track C async readback.
- **Two-phase HZB** (UE5 / Doom Eternal pattern). Phase 1 culls
  against last-frame's pyramid → renders that subset. Phase 2 uses
  current depth to refine, catches disocclusions before the same
  frame ships. Bigger correctness, more complexity.

**For MC3 scale, single-phase is probably enough.** The 1-frame
disocclusion lag is invisible at gameplay framerates and matches the
async-readback lag we already accept in Track C.

**Dependencies / sequencing:**

- HZB requires Track C C1 compute infrastructure (compute dispatch,
  SSBO substrate, depth-texture access). Belongs AFTER C1 ships at
  earliest.
- HZB requires depth pyramid construction — a separate compute pass
  (or fragment shader pass) that runs once per frame. Adds ~50-100µs
  GPU at 1080p.
- HZB is independent of C2/C3 (visibility readback / gate handoff)
  but stacks cleanly with them — the visibility bit could fold both
  frustum-cull and occlusion-cull results.

**How to apply:**

- Queue as **Track G** (or **C4** if framed as a Track C extension)
  for after C3 ships.
- Recon-zero pass enumerates: AMD RDNA3 depth-pyramid construction
  cost, depth-texture access in compute, two-phase vs single-phase
  decision, integration with C1's visible-ID list.
- Brainstorm specifically wants to answer: how much does HZB
  recover in our actual content? A measurement-first slice (instrument
  Track C C1's "in-frustum but occluded" count via a debug-mode
  depth-test, get a real number) would ground the cost-benefit
  analysis.

**Honest framing:** HZB is the natural follow-up after Track C
ships. Putting it on the candidate list now (vs. permanently skipping)
is the correction.

### Q18 — Lights as visibility producers (C3 preflight)

**Resolved:** before C3 gate handoff, audit every transitive consumer
of `lightAppearance->inView`. If any C3-routed lifecycle gate reads it,
include lights in the C3 routing list. Currently the Track C recon
classified lights out-of-scope; that's only correct if no lifecycle
gate transitively depends on them.

**Why this is brainstorm-worthy:** `code/light.cpp:123-124` writes
`lightAppearance->setInView()` from the light's own `onScreen()` —
making lights a *third* visibility producer alongside object admission
(`canBeSeen`) and terrain projection (`objVertexActive`). The recon
flagged this; C3's gate handoff is incomplete if any lifecycle gate
reads light visibility.

**How to apply:** C3 plan adds a preflight step:
```bash
grep -rn "->inView\|lightAppearance.*inView\|isInView" code/ mclib/ \
  | grep -v "^code/light.cpp" | grep -v "^mclib/.*Appearance"
```
Audit each match. Document any consumer that reads light visibility in
a way that affects lifecycle. If any exists, light routing joins C3
scope.

---

## Cross-cutting roadmap revisions

These flow from the decisions above and are applied to
`docs/superpowers/mc3-rendering-modernization-roadmap.md` as part of this
brainstorm's writeup:

1. Track A reorders to object → effects → (re-evaluate) terrain.
2. Track A's RAlt+P overlay GL state prereq is struck (already fixed in
   commit `dec89aa`).
3. Track A site count for terrain corrected to 6 (clone at
   `mclib/terrain.cpp:1438` joined original at `:1597` since the 2026-04-25
   inventory).
4. **Track A parity gate rewritten** as dual-run with reviewed acceptance
   envelope; `screen.x/y/z/w` byte-identical preserved as hard gate;
   `[DESTROY v1]` count delta + visual smoke + out-of-envelope
   disagreements are the only hard failures (per Q3 revision).
5. Track B's "static lighting input is also static" claim corrected — only
   genuinely static fields (`modelMatrix`, `firstColorOffset`, `flags`,
   `aRGBHighlight`, `fogRGB`) are mission-load-baked; per-frame
   `cachedFrame_`-stamped lighting cache preserved for dynamic lights.
6. Track B's "ever-visible pre-cull at mission load" descoped (~0% benefit
   on stock per recon §8) — also struck from end-state diagram for consistency.
7. Track B Generic scope narrowed; two-population boundary documented.
8. **Track B first-frame race ships as structural fix** (pre-populate
   `cachedFrame_`) by default, not "verify-then-escalate" (per Q6 revision).
9. **Track C splits into C0/C1/C2/C3** sequential slices (per Q14).
   - C0: dynamic actor visibility record substrate (Q10).
   - C1: compute cull for render draw only (validates Q11 bucket layout +
     Q12 sync contracts).
   - C2: async readback into non-lifecycle consumers.
   - C3: gate handoff + `Camera::UpdateRenderers` stub.
10. **Track C bucket key sharpened** to mesh-range + shader + texture-set
    + VAO + index-type (per Q11). Material-only bucketing rejected.
11. **Track C synchronization contracts documented** (Q12): counter reset,
    overflow, instanceCount patch location, glMemoryBarrier bits, async
    readback fallback. These move into the C1 implementation plan.
12. **GL version language harmonized** (per Q13): baseline GL 4.3 preserved;
    Track C requires GL 4.6 OR `ARB_indirect_parameters` +
    `ARB_shader_draw_parameters` on 4.3/4.4. Startup probe required.
13. Track C exit criterion ("Camera.UpdateRenderers becomes a stub") is
    hard; option B (preserve `recalcBounds` for AI) rejected on framing
    grounds. Q7 justification reframed as gameplay-tolerance tradeoff,
    not "below human-perception floor."
14. **Shared-soak discipline (Q15)** — sequential-with-overlap is the
    canonical cross-track soak pattern. Slice X soaks alone → flips →
    Slice Y soaks under X-already-modern. No parallel envelopes for
    sequential slices. Independent prep slices (e.g. alpha-test prep)
    may run on their own branch and rejoin on flip.
15. **Track B `firstColorOffset` ownership (Q16)** — Task 1 spike must
    commit to one of three architectural answers (bake-at-register /
    patch-per-frame / recipe-field redesign) with written rationale
    before Task 2 begins. No "we'll patch it if it breaks" punt.
16. **Track C block-active rollup (Q17)** — per-actor visibility does
    not auto-answer per-block visibility. C1 plan must enumerate
    which rollup path (GPU compute aggregation OR CPU-side conservative
    walk) it takes; C3 gate handoff is incomplete without it.
17. **Track C lights preflight audit (Q18)** — before C3 flip, audit
    every transitive consumer of `lightAppearance->inView`. Add lights
    to C3 routing if any lifecycle gate reads them. Recon classified
    lights out-of-scope; that's correct only conditional on the audit.
18. **DSA adoption arc — Track F candidate (Q19)** — every buffer /
    texture / framebuffer / VAO operation in the codebase is currently
    legacy bind-style. Track F (post-Track-E) is a mechanical refactor
    to named-object operations. Brainstorm before kickoff to enumerate
    scope.
19. **`glClipControl` adoption (Q20)** — recon-zero pass before Track C
    C0; if recon shows clean migration, ship as a pre-C0 prep slice.
    Cleans up Track A1 / C0 / C1 cull predicates' D3D-style depth
    convention from "manual constraint" to "hardware native."
20. **HZB occlusion culling (Q21)** — back on the candidate list after
    camera model correction. Earlier "RTS top-down" rationale was wrong;
    oblique 30° + cinematic surfaces real occlusion. Queue as Track G
    or C4 after C3 ships; recon-zero pass first to measure
    "in-frustum but occluded" fraction in actual content.

---

## Open follow-ups

- **Q6 counter validation.** `[STATIC_FIRST_FRAME v1]` counter must read
  zero with the structural fix (pre-populated `cachedFrame_` at registration)
  in place. Non-zero indicates the flush invariant isn't satisfied by the
  pre-populated stamp; escalate to baseline-lighting fallback or revisit
  flush invariant directly.
- **Q1 terrain re-evaluation.** After Track A slices 1 (object) and 2
  (effects) ship and soak, re-run capture-replay corpus with the new
  predicate active on those wrappers; re-assess whether terrain admission
  is worth the wedge risk the capture data flagged.
- **Q2 effects scope.** Track A slice 2 (effects, 7 sites) inherits the
  dual-output wrapper pattern from slice 1. Per-site grep at slice-2 plan
  time will determine whether any of the 7 effect sites need different
  treatment than the single object site.
- **Q3 acceptance envelope authoring.** Before A1 flip, the reviewed
  envelope of expected disagreements must be written down (which zoom
  bands, camera angles, object distance bands are expected to differ).
  Operator-side step; documented in the A1 plan.
- **Q12 barrier validation.** AMD canary build must verify
  `GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT` post-cull
  produces correct indirect-draw consumption. Apitrace or RenderDoc
  capture compares pre/post-barrier. Docs/amd-driver-rules.md updated if
  any barrier-bit pathologies surface.
- **Q14 C2 readback consumer choice.** Which non-lifecycle consumer reads
  N-1 first? Candidates: Tracy visibility plot, `[GPU_CULL v1]` summary,
  debug overlay. Pick at C2 plan time; smallest-blast first.

---

## References

- Recons: `docs/superpowers/explorations/2026-05-06-track-{a,b,c}-*-recon.md`
- Roadmap: `docs/superpowers/mc3-rendering-modernization-roadmap.md`
- Framing memories: `mc3_modernization_philosophy.md`, `open_rts_engine_framing.md`
- Cull cascade constraint: `memory/cull_gates_are_load_bearing.md`
- Capture report (Track A reordering basis): `docs/superpowers/specs/projectz-capture-report.md`
- Black-tree-bug fix (Q6 cachedFrame_ pattern): `memory/black_tree_bug_investigation_state.md`
- Late-register inventory: commit `06ac847`
- Worktree CLAUDE.md documentation discipline.
- AMD driver rules (Q12 barrier validation context): `docs/amd-driver-rules.md`.

## Revision log

- **2026-05-06 (initial)** — Q1-Q9 resolved; doc written.
- **2026-05-06 (post-advisor-review)** — Q3 parity wording rewritten as
  dual-run + envelope; Q6 first-frame race promoted to structural-by-default;
  Q7 reasoning reframed from "below perception floor" to gameplay-tolerance
  tradeoff; new sections Q10 (dynamic actor records / Track C0), Q11
  (bucket key precision), Q12 (sync contracts), Q13 (GL version
  harmonization), Q14 (Track C split into C0/C1/C2/C3) added.
  Cross-cutting revisions list and open follow-ups updated to match.
- **2026-05-06 (canonical, post-advisor-pass-2)** — Q12 sharpened: C1
  slice boundary explicit (GPU→GPU only, no readback in C1); counter
  representation choose-and-stick (SSBO `uint` + `atomicAdd`, not ACBOs);
  barrier set narrowed to `GL_SHADER_STORAGE_BARRIER_BIT |
  GL_COMMAND_BARRIER_BIT` (plus `GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT` in
  C2 only); explicit "do NOT add `GL_ATOMIC_COUNTER_BARRIER_BIT`" warning
  to prevent cargo-culting. Greenlit for Track A1 plan-writing.
- **2026-05-06 (post-plan-pass-3, post-advisor-on-A2/B/C)** — A1 approved
  execution-ready. A2 soak ordering locked as sequential-with-overlap.
  Track B given three hard constraints before execution. Track C C1+
  gated on Q15/Q16/Q17 resolution. Three new sections added below
  (Q15/Q16/Q17). Cross-cutting revisions extended.
