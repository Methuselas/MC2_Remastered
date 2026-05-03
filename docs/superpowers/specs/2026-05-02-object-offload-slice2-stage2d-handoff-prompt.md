# Slice 2 (object-offload) — Stage 2.D — Implementation hand-off prompt

> **Role for a fresh session reading this:** You are picking up the
> object-offload arc at **Stage 2.D — parity instrumentation**. Stages
> 2.A, 2.B, 2.C are COMPLETE behind `MC2_GPU_OBJECTS=1` flag. The
> late-reg allowlist is RESOLVED. Stage 2.D is unblocked from the
> late-reg side. Your job is to land the parity check (P3 dual-emit
> at first frame + P1 sampled bytewise in steady state) per the
> design spec, with the advisor's late-reg exclusion rule applied.
> Stage 2.E (pinned-camera diff) is a separate PR after 2.D.

---

## Current state (as of 2026-05-02)

**Slice 2 PR-ready checkpoint reached.** Stages 2.A + 2.B + 2.C +
late-reg instrumentation + allowlist all committed and green:

- `cdcdb7d` — Stage 2.A: substrate edits (no behavior change)
- `bd1bd25` — Stage 2.B: eligibility hoist + late-reg recovery (defensive
  flag only; falls through to legacy CPU `Render()`)
- `ad96c1f` — Stage 2.C.1: GLSL kernel + UBO schema lockstep + render-time
  gather + `TG_Shape::init()` static-state lifecycle fix
- `eb2a837` — Stage 2.C.2: flip static_prop draw to GPU lighting
- `47d9553` — late-reg type identification (nodeId + caller + one-shot
  registered_dump) + slice 2 spec/handoff status update
- `c82375b` — skybox vestigial note (memory file + cross-ref)
- `06ac847` — late-reg allowlist matcher fix (nodeId-keyed, was
  pointer-keyed silent-fail) + Cylinder01/compassplane allowlisted

**Tier1 5/5 PASS** in three configs (unset / `MC2_GPU_OBJECTS=1` /
`+MC2_OBJBATCHER_TRACE=1`), +0 destroys delta on every mission.
**Tier2 24/24 PASS** in both configs, +0 destroys delta everywhere.

**Verify these commits before starting Stage 2.D:**
```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
git log --oneline -10
# Expect: 06ac847 at HEAD (or HEAD^N)
git log --oneline | grep -E "stage 2\.[ABC]|allowlist|skybox vestigial" | head -8
# Expect: all 7 commits above present
```

If any is missing, escalate to user before doing anything — substrate
may have been reverted or the worktree may have drifted.

---

## Worktree + branch

- **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Branch:** `claude/nifty-mendeleev`
- **Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.3/`

---

## REQUIRED READS (in order — non-skippable)

1. **Slice 2 design spec — Stage 2.D section:**
   `docs/superpowers/specs/2026-05-02-object-offload-slice2-design.md`
   — search for "Stage 2.D — Parity instrumentation". Architecture is
   locked. The spec covers: P3 dual-emit at first frame + P1 sampled
   bytewise in steady state, ULP tolerance ±2 LSB per channel,
   `listOfTriangles[].aRGBLight[i]` as the compare target (the FINAL
   render-equivalent color, NOT pre-face-additive `listOfVertices[].argb`),
   600-frame summary, mismatch logging format
   `[OBJECT_PARITY v1] event=lighting_mismatch ...`.

2. **Slice 2 design spec — "Compare-target caveat" subsection** — this
   is load-bearing. Why corner-granularity even though slice 2 is
   per-vertex-lit: in stock `useFaceLighting=false`, so corner color
   degenerates to per-vertex value modulo alpha/packing. Any mismatch
   indicates packing / fog / highlight / terrain-light / shader-math
   divergence, NOT missing per-face lighting.

3. **Slice 2 hand-off prompt** (the BIG one):
   `docs/superpowers/specs/2026-05-02-object-offload-slice2-handoff-prompt.md`
   — read the "RESOLVED 2026-05-02" section near the top for the
   campaign-wide late-reg inventory + the operational warnings carried
   forward (clean-rebuild rule, line-drift caveat, late-reg correction).
   This stage's discipline matches that document.

4. **Recon Zero Section 9** (post-revision view):
   `docs/superpowers/explorations/2026-05-02-object-offload-slice2-recon-zero.md`
   — Section 9 supersedes earlier sections. Item 4 is the eligibility-hoist
   design that you'll interact with at the per-actor level. Item 5 is
   the per-actor (NOT per-leaf) lightData_ identity that lets the parity
   check sample one leaf and trust it covers the multishape.

5. **Worktree CLAUDE.md** — `.claude/worktrees/nifty-mendeleev/CLAUDE.md`.
   Load-bearing project rules:
   - Documentation Discipline (grep at write-time)
   - Review Discipline (Stage 2.D is parity-instrumentation-class — NOT
     architectural-endpoint, NOT legacy-retiring; prose review is fine
     unless you propose a schema change)
   - Critical Rules (build / deploy / shader version)
   - Tier-1 Instrumentation Env Vars
   - Smoke Gate command (drop --menu-canary per
     memory/feedback_smoke_no_canary.md)

6. **Memory files (load-bearing):**
   - `memory/cull_gates_are_load_bearing.md`
   - `memory/tgl_pool_exhaustion_is_silent.md`
   - `memory/mc2_texture_handle_is_live.md`
   - `memory/mc2_argb_packing.md` ⭐ (parity hinges on the byte-order
     match between CPU writes and GPU readback — see Stage 2.C.2's
     B,G,R,A swizzle for precedent)
   - `memory/static_prop_projection.md`
   - `memory/render_order_post_renderlists_hook.md` (PBO readback must
     happen after the slice 1 batcher's draw — same hook timing rule)
   - `memory/feedback_offload_scope_stock_only.md`
   - `memory/feedback_smoke_no_canary.md`
   - `memory/feedback_pool_peak_compare_same_mission.md`
   - `memory/feedback_subagent_no_cmake_configure.md`
   - `memory/feedback_smoke_serial_only.md` ⭐ (NEW 2026-05-02 — never
     run two mc2.exe / smoke processes concurrently; serialize)
   - `memory/tg_shape_static_state_lifecycle_trap.md` ⭐ (NEW 2026-05-02
     — class-static fields cleared by per-instance init() must be
     reset together; rule applies if you touch any static-state init)
   - `memory/skybox_actor_vestigial_post_terrain_gpu.md` (context for
     why the skybox actor produces the always-allowed late-reg)

7. **Skills:**
   - `.claude/skills/mc2-build.md`, `mc2-deploy.md`, `mc2-build-deploy.md`,
     `mc2-check.md` — build/deploy cycle.
   - `.claude/skills/adversarial-plan-review.md` — apply if you propose
     a schema change beyond the spec; otherwise prose review is fine.

---

## Stage 2.D scope (single sentence)

Land the parity check (`MC2_OBJECT_PARITY_CHECK=1`): a one-frame
dual-emit at first eligible frame post-mission-start + per-frame
sampled bytewise compare in steady state, both writing
`[OBJECT_PARITY v1] event=lighting_mismatch ...` lines on per-corner
divergence > ±2 LSB per channel, with a 600-frame summary line.

---

## What Stage 2.D explicitly does NOT do

- **Default-on flip.** Stage 2.D ships behind `MC2_OBJECT_PARITY_CHECK=1`
  (default off) and stays flagged. Slice 2 PR-ready checkpoint is
  unaffected by 2.D's gate (it's a separate validation layer).
- **Pinned-camera screenshot diff.** That's Stage 2.E, separate PR.
- **Schema changes** to `GpuStaticPropInstance`, `TG_HWLightsData`,
  `ObjectLights`, the per-vertex VBO, or the per-type SSBO. Stage 2.C
  locked these. If parity surfaces evidence the schema is wrong,
  surface to user — do NOT modify.
- **Touch the late-reg path.** Allowed late-reg actors render via
  legacy CPU `Render()` and are documented out-of-scope for slice 2's
  GPU lighting. Per advisor 2026-05-02: **explicitly exclude them
  from parity accounting** — see "Advisor exclusion rule" below.
- **Touch animated movers.** Mech3D/GVAppearance are out of arc.
- **Bypass any cull infrastructure.**

---

## Stage 2.D-specific load-bearing rules

### 1. Advisor exclusion rule (from 2026-05-02 post-allowlist review)

> "in the Stage 2.D parity implementation, explicitly exclude allowed
> late-reg actors from parity accounting, or count them separately as
> parity_skipped_allowed_late_reg. They are intentionally CPU-rendered
> /out-of-scope, so including them in GPU-light parity would create noise."

**Implementation rule (encoded into the spec):** the parity sampler
must skip any actor whose owning multi-shape would have produced
`wasLastFailureLateRegistration() == true && allowed=1` at submit
time. Either:

- **Option A (preferred):** the sampler queries the slice 1 batcher's
  per-actor result before sampling. If the actor's submit failed and
  was allowed-late-reg, increment a separate counter
  `parity_skipped_allowed_late_reg` and do not compare. Only allowed
  late-reg actors take this path; non-allowed late-reg (real
  registration walk gaps) STILL count as mismatch / unexpected event.
- **Option B (fallback if A's plumbing is too invasive):** include
  late-reg-fallback actors in the compare bucket but tag them in the
  mismatch log so the operator can filter. Document the choice in the
  PR description and the 600-frame summary's accounting.

The 600-frame summary line MUST emit
`parity_skipped_allowed_late_reg=N` so the operator can confirm the
exclusion is working as intended.

### 2. Compare target — `listOfTriangles[].aRGBLight[i]` (FINAL emit)

Per spec line 327 ("Compare-target caveat"): use `listOfTriangles[].aRGBLight[i]`
on the CPU side, NOT `listOfVertices[].argb`. The Render kernel at
`mclib/tgl.cpp` (search for `.aRGBLight[i] =` writes) overwrites
gVertex's argb from this triangle-corner field at emit time. If you
compare `listOfVertices[].argb`, you're comparing the pre-face-additive
vertex stream — slice 2 ships per-vertex-lit so this would
match-by-accident on stock missions where `useFaceLighting=false`,
but the comparison is structurally wrong and would silently miss
divergence on hypothetical mod content.

### 3. PBO async readback — likely a sub-stage of its own

The spec mentions "via PBO async readback." Audit the tree first:
```bash
grep -rn "glMapBuffer\|GL_PIXEL_PACK\|PBO" GameOS/ mclib/ shaders/ 2>&1 | head -20
```
If no PBO infrastructure exists (likely), budget the PBO bring-up as
half of Stage 2.D's effort:

- Allocate a per-frame PBO sized for the per-actor-sample byte budget.
- After the slice 1 batcher's `flush()` (the `mcTextureManager->renderLists()`
  hook timing per `memory/render_order_post_renderlists_hook.md`),
  issue `glReadPixels` / texture-source readback into the PBO with a
  fence.
- One frame later, map the PBO and do the bytewise compare.

If existing terrain-indirect parity infra (commit history mentions
`MC2_TERRAIN_INDIRECT_PARITY=1`) has a similar PBO pattern, crib from
there. Otherwise this is greenfield work.

### 4. ULP tolerance ±2 LSB per channel

Per spec line 324. Each channel independently. If real divergence is
bounded under that, treat as PASS. If divergence is bounded but >
threshold, surface to user — the spec's tolerance is a starting
point, not absolute.

### 5. Stage 2.D gate

Per spec line 329:
> Zero mismatches across tier1 stock with `MC2_OBJECT_PARITY_CHECK=1`
> AND `parity_skipped_allowed_late_reg` matches the expected count
> (skybox + compass per mission, see campaign inventory in handoff).

If mismatches are nonzero but bounded (< 0.1% of compared corners) AND
visible only at extreme corner cases (lighting transitions, etc.),
surface to user with examples. Spec may need to widen ULP tolerance
or accept GPU as new ground truth.

### 6. Stage 2.D is NOT a slice 2 PR blocker (per spec line 333)

> Stages 2.A-2.C may merge behind `MC2_GPU_OBJECTS=1` flag once their
> respective gates pass. Stage 2.D parity is NOT a merge blocker for
> the slice 2 PR. It IS a hard pre-condition for either:
> (a) declaring slice 2 "validated" / done, OR
> (b) any default-on flip consideration.

Slice 2 PR-ready checkpoint already merged the substrate. Stage 2.D
adds the validation layer. The Stage 2.D PR can land separately and
its gate is "no mismatches found, sampler counted what we expected."

---

## Implementation stages

The spec doesn't pre-split 2.D into sub-stages. Suggested split for
clean bisection (decide based on PBO bring-up effort):

### 2.D.1 — PBO async readback harness (greenfield if no precedent)

Files:
- `GameOS/gameos/gos_object_parity.{h,cpp}` (NEW, sidecar — keeps the
  parity code out of the slice 1 batcher hot path) OR fold into
  `gos_static_prop_batcher.cpp` (less invasive but couples concerns).
- One PBO allocation, one fence per frame, one mapped read per frame.
- env-gated by `MC2_OBJECT_PARITY_CHECK=1`. Default-off = zero overhead.

**Gate:** with `MC2_OBJECT_PARITY_CHECK=1`, tier1 5/5 PASS, +0 destroys.
The PBO doesn't actually compare anything yet — it just runs the
readback infra to verify the pipeline doesn't crash or stall.

### 2.D.2 — P3 dual-emit at first frame + bytewise compare

- At first eligible frame post-mission-start: run BOTH
  `MultiTransformShape` (full-bake) AND `MultiTransformShape_PositionsOnly`
  for all actors. Compare CPU `listOfTriangles[].aRGBLight[i]` against
  GPU output (read back via PBO from 2.D.1).
- Mismatch log format per spec.
- This is one frame of overhead at mission start, then disabled.

**Gate:** zero mismatches across tier1 stock missions on this one
frame.

### 2.D.3 — P1 sampled bytewise in steady state

- Per-frame sampler: 1 actor per type per frame, round-robin. Compare
  GPU output (1-frame-stale OK via async PBO) against CPU
  `MultiTransformShape` recomputation on the sampled actor.
- Allowed-late-reg exclusion per advisor (see "Advisor exclusion rule"
  above).
- 600-frame summary line: counts of compared/passed/mismatched/
  skipped_allowed_late_reg.

**Gate:** zero mismatches across tier1 stock missions in P1 steady
state. `parity_skipped_allowed_late_reg` matches campaign inventory
expectations (skybox + compass per mission).

---

## Build / deploy / smoke discipline (carried forward)

**Build:** `cmake --build build64 --config RelWithDebInfo --target mc2`
ONLY. NEVER `cmake -B build64 -S .` or any configure variant. Per
`memory/feedback_subagent_no_cmake_configure.md`.

**Deploy:** per-file `cp -f` + `diff -q` to
`A:/Games/mc2-opengl/mc2-win64-v0.3/`. NEVER `cp -r`. Files for
Stage 2.D will likely be `mc2.exe` + `mc2.pdb` only (no shader
changes expected).

**Smoke:** `py -3 scripts/run_smoke.py --tier tier1 --duration 20
--fail-fast --kill-existing`. **Drop `--menu-canary`** per
`memory/feedback_smoke_no_canary.md`. Three configs:
- unset (default)
- `MC2_GPU_OBJECTS=1`
- `MC2_GPU_OBJECTS=1 MC2_OBJECT_PARITY_CHECK=1`

**Serial only.** Per `memory/feedback_smoke_serial_only.md`: NEVER run
two `mc2.exe` instances concurrently. NEVER kick off a smoke in the
background while doing direct-mc2 traces. Serialize.

**Clean rebuild after header changes.** Per the operational warning
in slice 2 hand-off: stale `.obj` artifacts can mask access-control
violations. If you edit a header that other translation units include,
verify the dependents recompiled from scratch — don't trust a
"no-op no-rebuild" link.

**Pool peak comparisons:** same-mission baseline-vs-test only per
`memory/feedback_pool_peak_compare_same_mission.md`. Trust +0
destroys delta as the primary Gate-E proxy.

**Stock missions only** for validation per
`memory/feedback_offload_scope_stock_only.md`. tier1 + tier2 campaign.
Mod content (Carver5O, Magic, MCO, Wolfman, MC2X) out of scope.

**Git:** never push. HEREDOC for commit messages. NEVER amend.

---

## When you finish

**Stage 2.D PR (the parity layer):**

1. tier1 5/5 PASS in three configs (unset / `MC2_GPU_OBJECTS=1` /
   `+MC2_OBJECT_PARITY_CHECK=1`).
2. tier2 24/24 PASS in same three configs (use
   `--duration 10 --kill-existing`, sequential, no menu canary).
3. P3 first-frame dual-emit: zero mismatches.
4. P1 steady-state sampled bytewise: zero mismatches.
5. `parity_skipped_allowed_late_reg` matches campaign inventory
   expectations (skybox always, compass in 22/24).
6. 600-frame summary line is grep-friendly and shipped with
   commit-tagged version.
7. PR description honestly notes that pinned-camera Tracy /
   screenshot validation is Stage 2.E, separate PR.

**If a stage gate fails:**
- Pool exhaustion / destroys delta != 0: revert and investigate per
  `memory/cull_gates_are_load_bearing.md` /
  `memory/tgl_pool_exhaustion_is_silent.md`.
- Parity mismatches > 0: surface to user with example mismatch lines.
  Investigate (probably packing / fog / shader-math). Fix-forward
  in 2.D PR; do NOT revert slice 2.A-2.C.

---

## Out of arc (do not pursue without separate brainstorm)

- Stage 2.E pinned-camera diff harness (separate PR, gates default-on).
- GPU shadow port for static props (separate slice or arc).
- Mover offload (Mech3D / GV).
- Default-on flip.
- Removing legacy `g_useGpuStaticProps` and the 5 cull-bypass sites.

---

## Hand-off

Stage 2.D is well-scoped: parity instrumentation behind a flag, with
a clean exclusion rule for the documented late-reg cases. The
substrate (Stages 2.A-2.C + allowlist) is settled and tested across
tier1+tier2. The PBO readback harness is the only piece of
greenfield infrastructure; everything else is comparing two byte
streams and counting.

Execute the 3 sub-stages in order. Surface real blockers to user;
never paper over them with hacks or skipped tests.
