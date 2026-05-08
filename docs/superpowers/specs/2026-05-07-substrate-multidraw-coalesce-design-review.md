# Adversarial review — substrate multi-draw coalesce design (2026-05-07)

**Subject:** `docs/superpowers/specs/2026-05-07-substrate-multidraw-coalesce-design.md`
**Reviewer stance:** adversarial, code-grounded per `.claude/skills/adversarial-plan-review.md` and worktree CLAUDE.md "Review Discipline."
**Worktree:** `claude/nifty-mendeleev`. All file:line citations resolve there at review time.

## Verdict

**STOP THE LINE.** 4 CRITICAL, 6 MAJOR, 4 MINOR. The design conflates two
separately-rejected architectural choices (bindless + `gl_DrawID` /
`ARB_shader_draw_parameters`), misses that `u_packetID` is consumed by the
fragment debug path (not just an SSBO index), introduces a per-frame audit
that itself is a perf regression, and provides no kill-switch for default-on
flip. The implementation plan cannot proceed against this design without a
revision pass.

---

## Cited-symbol verification (grep results)

| Claim in design | Verified | Notes |
|---|---|---|
| Loop head `gos_static_prop_batcher.cpp:1542` | **OK** | `for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID)` |
| Inner packet loop `:1652` | **OK** | `for (uint32_t p = 0; p < type.packetCount; ++p)` |
| `glDrawElementsIndirect` per-call `:1704` | **OK** | exact match |
| Indirect bind/unbind `:1702`/`:1706` | **OK** | per-iteration thrash confirmed |
| Per-packet texture bind `:1670–1671` | **OK** | `glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, glTexId);` |
| Per-packet `u_materialFlags` write `:1685` | **OK** | location-by-name lookup every iter (also a hidden cost) |
| Per-packet `u_packetID` write `:1687` | **OK** | but design's claim about its semantics is wrong — see CRITICAL-2 |
| Single `glUseProgram(s_staticPropProgram)` `:1433` | **OK** | shared |
| Single `glBindVertexArray(s_sharedVao)` `:1434` | **OK** | shared |
| `glEnableVertexAttribArray(0)` at VAO setup `:727` | **OK** | (also `:729 :731 :733 :739` — locations 1..4) |
| `DrawCmd` 5-field struct + 20-byte assert `gpu_cull_compute.cpp:507–514` | **OK** | exact match |
| `s_indirectCmdBuf` declaration `:81` | **OK** | exact match |
| Indirect alloc/upload `:551–555` | **OK** | exact match |
| Patch dispatch `:798/:805` | **OK** | binding 11 SSBO + glDispatchCompute |
| Post-patch barrier `SHADER_STORAGE | COMMAND` `:813` | **OK** | exact match |
| `compute_dispatch` ordering before `flush()` `txmmgr.cpp:1762–1766` | **OK** | (registry::flush -> compute_dispatch -> batcher::flush) |
| `STATIC_PROP_FLAG_ALPHA_TEST = 1u<<0` `gos_static_prop_batcher.h:58` | **OK** | exact match |
| `GpuStaticPropPacket` fields `gos_static_prop_batcher.h:38–48` | **OK** | exact match |
| `GpuStaticPropType` fields `gos_static_prop_batcher.h:87–92` | **OK** | exact match |
| `MC2_GPU_CULL_SUBSTRATE` env var `gpu_cull_substrate.cpp:53` | **OK** | exact match |
| **PR1 multi-draw call at `gameos_graphics.cpp:2410`** | **WRONG** | actual line is **`:2423`**: `glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, (GLsizei)cmdCount, 0);` Section 1.6 is off by 13 lines. |
| **AMD attr-0 banner at `gameos_graphics.cpp:2210–2213`** | **WRONG** | the banner block is at **`:2217–2230`** (the line range cited contains `glDepthFunc` / `glUseProgram` restoration code from a *different* function). |
| `u_packetID` "only used for indexing into a per-packet SSBO" (Section 2.1 row) | **WRONG** | grep shows `u_packetID` is consumed in `static_prop.frag:34` and `:62` for the RAlt+9 debug-mode hash (`hash_u(uint(u_packetID) * 2654435761u + v_localVertexID)`). Not an SSBO index. The proposed lift to "SSBO of-size-N" is mechanically wrong for this consumer. |

Bindless / `gl_DrawID` precedent grep:

- `grep -r "ARB_shader_draw_parameters\|GetTextureHandleARB\|MakeTextureHandle" GameOS/` returns **NO matches**. The codebase has never used either extension.
- `grep gl_DrawID shaders/` returns one match: `gpu_cull.comp:71` — a *comment* about `gl_BaseInstance`, not actual usage. No shader currently uses `gl_DrawID` or `gl_DrawIDARB`.

---

## CRITICAL findings (block landing)

### CRITICAL-1 — `u_packetID` is consumed by frag debug-mode hash, not just an SSBO index

**Design (Section 2.1, row "u_packetID uniform"):** "only used for indexing
into a per-packet SSBO; can be lifted to SSBO of-size-N."

**Reality:** `shaders/static_prop.frag:34` declares `uniform int u_packetID;`
and `:62` uses it as the entropy source for the RAlt+9 debug-mode 2 hash:
```
uint h = hash_u(uint(u_packetID) * 2654435761u + v_localVertexID);
```
Per worktree CLAUDE.md hotkey table, RAlt+9 cycles GPU static-prop frag
debug-mode 0..7 and is a load-bearing diagnostic for this exact subsystem.
The design's proposed lift loses per-packet entropy (every packet inside the
multi-draw would receive the same `u_packetID` because there is no per-draw
uniform mechanism without `gl_DrawID`). RAlt+9 mode 2 visualization breaks.

**Fix options:**
- Move the per-packet entropy into the per-vertex stream (encode packetID into
  a new vertex attribute or repurpose `a_aRGBLight`) and rebuild the hash
  source. Mechanical, but a vertex-format change is a load-bearing schema
  change beyond the design's stated scope.
- Use `gl_DrawID` as the hash source — but see CRITICAL-3.
- Accept loss of the diagnostic and document it explicitly as a regression
  trade-off (escalate decision).

This is the kind of "looks fine in prose, fails on grep" issue the skill exists
to catch. **Section 2.1's table row must be rewritten.**

### CRITICAL-2 — `gl_DrawID` / `ARB_shader_draw_parameters` was explicitly rejected by sibling design; this design re-introduces it without acknowledging that decision

**Design (Section 3.4):** "the shader needs to know its absolute instance
index = `baseInstance + gl_InstanceID`. In GLSL, `baseInstance` is exposed as
`gl_BaseInstance` only if the `ARB_shader_draw_parameters` extension is
available."

**Sibling brainstorm `2026-05-01-detail-overlay-consolidation-scope.md:126`:**
"Decision: A(iii) single-command-per-bucket + texture array + per-quad layer
index in thin record. **Avoid `gl_DrawIDARB`**; defer bindless to a
hypothetical future slice if bucket count explodes."

`:135–139`: "introduces a load-bearing dependency on a non-core extension and
a `#version` bump (or `#extension GL_ARB_shader_draw_parameters : require`),
which would be the **first such extension in this codebase**. Adversarial
review..."

Worktree CLAUDE.md "Critical Rules": "Shader #version: Never in shader files.
Pass `"#version 430\n"` as prefix to `makeProgram()`." `ARB_shader_draw_parameters`
is GL 4.0+ extension but the in-shader pragma `#extension` would be the
first ever in this codebase, AND the extension is not part of the prefix
mechanism — there is no precedent path for adding it.

The design notes this in passing ("the codebase has not used before") but
makes it a hard dependency anyway, with the only fallback being "fall back
to current loop" (Section 3.4 last paragraph) — which means the
implementation collapses to single-bucket-per-bind = no perf gain at all if
the extension isn't available, or worse, a runtime fallback driver-by-driver
that explodes the test matrix.

**Fix:** the design must either (a) inherit the brainstorm's "avoid
`gl_DrawIDARB`" decision and find a different mechanism for per-bucket SSBO
indexing (e.g., texture-array layer index packed into the per-vertex stream,
following the brainstorm's A(iii) model), or (b) escalate the decision to
explicitly retire that prior rejection, with the same adversarial scrutiny
the brainstorm got. Sliding the dependency in via a multi-draw design that
doesn't cite the brainstorm is a violation of the worktree's "documentation
discipline" rule (root CLAUDE.md "supersede, don't append").

### CRITICAL-3 — Bindless texture is the first introduction in the codebase; lifecycle, residency, and AMD precedent unspecified

**Design (Section 4.2):** "Bind bindless texture handle table (new SSBO
populated at registration)." Section 2.2 #1 names bindless as the cleanest
solution. Section 7 R6 names mission-unload teardown but only at the
"don't leak handles" level.

**Reality:** `grep -r "bindless\|GetTextureHandleARB\|MakeTextureHandleResident" GameOS/`
returns zero matches. The codebase has never:
- Probed `GL_ARB_bindless_texture` at context create.
- Made a single texture resident.
- Reasoned about the AMD driver's bindless residency limit (per
  `docs/amd-driver-rules.md` review tradition: AMD drivers diverge from
  NVIDIA on bindless residency in ways the brainstorm explicitly cited as
  "not yet vetted on RX 7900 XTX").
- Built any tooling for the residency lifecycle that
  `memory/gpu_direct_renderer_bringup_checklist.md` Trap 7 ("texture
  residency") and `memory/m2_thin_record_cpu_reduction_results.md`
  warn about.

The design also conflates "TG_TypeShape's texture" with "a stable bindless
handle" — but `memory/mc2_texture_handle_is_live.md` is load-bearing:
"MC2 texture handles mutate per-frame ... store slot index, resolve at
draw time, never cache handle." The handle table proposed in Section 4.2
must therefore either resolve every frame (defeating the multi-draw cost
saving) or break the load-bearing handle-mutation invariant.

The design must:
- Cite `mc2_texture_handle_is_live.md` and explain how the bindless handle
  survives mid-frame mutation, OR
- Pick the sampler-array fallback (Section 4.3) as primary, not as fallback.

The sampler-array path also has the load-bearing precedent (`cement_multi_sampler_v2`)
which is shipped, vetted, and uses the same `#version 430` core path. Bindless
does not.

### CRITICAL-4 — The audit layer (Section 5.2 layer (a)) calls `glGetBufferSubData` per-frame: itself a stall

**Design (Section 5.2 (a)):** "Add a debug-mode env (`MC2_GPU_CULL_MULTIDRAW_AUDIT=1`)
that, after the new multi-draw issue, walks the indirect command buffer with
`glGetBufferSubData` and asserts: Sigma instanceCount matches the legacy
loop's per-frame `s_counters.gpu_drawn_instances` total. ... If any
assertion fires, ... fall back to legacy loop for that frame."

**Reality:**
1. `glGetBufferSubData` on `GL_DRAW_INDIRECT_BUFFER` immediately after a
   compute-shader patch dispatch (which is what produces the
   instanceCounts) forces a full GPU-CPU sync. The Tracy zone target is
   <=200 us; a per-frame readback of even 8 KB of indirect data on AMD
   serializes the GPU pipeline at the cost of typical-frame magnitudes (this
   is exactly the lesson `memory/water_ssbo_pattern.md` and the Track C2
   readback ring spent multiple sessions designing around — see
   `gpu_cull_readback.cpp` for the ring infrastructure built specifically
   to AVOID `glGetBufferSubData` on the hot path).
2. "Fall back to legacy loop for that frame" — by the time the assertion
   fires, the multi-draw has already issued. Falling back to the legacy
   loop for the same frame would double-render the static props.
3. The audit can't be "default-on" as the design says (Section 5.2 (a)
   "cheap, default-on") because of (1).

**Fix:** rewrite the audit to use the existing C2 readback ring
(`gpu_cull_readback.cpp`) — i.e. write the assertion data into a ring SSBO
and consume RING_FRAMES later. Or restrict the audit to env-gated
slow-path mode and explicitly de-default it.

---

## MAJOR findings (block default-on flip)

### MAJOR-1 — Per-type SSBO range invariant assumption (Section 2.2 #3) is unverified and load-bearing

**Design:** "the same `baseInstance` value works for instance/color SSBO
indexing as long as the per-type ranges are placed at offsets matching
`baseInstance` in the shared SSBO. Verify that placement invariant in
slice 0... Currently the per-type ranges in `s_instanceSsbo` use a separate
`TypeRangeSsbo` map (looked up at `gos_static_prop_batcher.cpp:1543–1556`)
— its `r.instanceByteOffset` must equal `baseInstance * sizeof(instance)`
for every type, or the SSBO layout has to be re-packed at registration."

This is the *single most load-bearing* invariant in the design and it is
deferred to slice 0 with a "verify or re-pack" non-decision. If re-pack is
needed, the design's perf claim collapses (re-pack is mission-load cost,
not per-frame, but the entire indirect-buffer schema would also need to
move). Verified at write-time grep:

- `s_typeRanges` map: lookup at `gos_static_prop_batcher.cpp:1543` — confirmed.
- `r.instanceByteOffset` per-type — built independently from the cumulative
  `cumBase` in `gpu_cull_compute.cpp:521,541`. Two independent cumulative
  pointers means the invariant is **not** mechanically guaranteed today.

**Action required:** this verification must move OUT of "slice 0" into the
design pass, because if the invariant fails, the design's whole shape
(Section 4.2 "bind whole instance + color SSBOs") is wrong and a different
schema is needed.

### MAJOR-2 — Patch shader writes by typeID; permutation hand-wave

**Design (R7):** "the patch shader binds the indirect cmd buffer at SSBO
binding `INDIRECT_CMD_BINDING` and writes `cmds[t].instanceCount` for
`t in [0, typeCount)`. If we permute the indirect buffer by group, the
patch shader must use the permutation map..."

The design proposes "(b) re-permute the patch shader's input ordering at
registration time so the natural `t` index in the patch shader matches the
new layout." But the patch shader's input is **`perBucketCount[]`** (binding
10, written by the cull shader). The cull shader writes `perBucketCount[bucket]`
where `bucket` comes from the substrate record's category bits
(`Cat_StaticProp | (typeID<<4)` per `txmmgr.cpp:1751`). So permuting the
patch shader's input means permuting the cull shader's output binding —
which means permuting the *category encoding* in the substrate record —
which is a contract change in `gpu_cull_substrate.cpp` and every producer
that emits a `Cat_StaticProp` record.

Section 3.3 "(a) Sort at registration time" hand-waves this: "the shader
knows each input's groupID via the SSBO it reads from, so the same
permutation is supplied as a uniform array `permutation[N]` or, simpler,
baked into the SSBO indices used at registration." Neither variant is
trivial. Baking into SSBO indices means the substrate record's category
field must encode the *post-permutation* typeID, which means
`GpuStaticPropRegistry::flush()` (`txmmgr.cpp:1753`) must apply the
permutation at submit time on every frame for every actor. That is CPU
work, on the hot path, and is not in the perf-gate accounting.

**Fix:** the design must spell out exactly which path it takes (uniform
array vs. baked indices), and trace the cost of the chosen path through
the per-frame substrate-flush hot path before claiming the <=200 us perf
target.

### MAJOR-3 — Mixed-alpha types (R4) are deferred to slice 0 but block the entire group split

**Design (R4):** "Need to either split the type into two virtual buckets at
registration or assert at registration that no type is mixed (and fall back
to legacy loop if any is found). Verify cardinality at slice 0 by walking
`s_packets` per type and counting alpha-class distinctness."

If "split the type into two virtual buckets" is the chosen path, every
mention of `typeID` in the existing code (cull shader category, substrate
record, readback ring keying, parity SSBO cursor) gets a new dimension.
That is a far bigger surface than the design's scope. If "fall back to
legacy loop if any is found" is the chosen path, then on any map containing
even one mixed-alpha type the multi-draw is silently disabled and the perf
gate fails for that map — but the test matrix (tier1: mc2_01, mc2_03,
mc2_10, mc2_17, mc2_24) doesn't enumerate which of those maps hit this case.

**Fix:** answer the cardinality question at design time, not slice-0 time.
A 30-line C++ probe over `s_packets` is the same wall-clock cost whether
written into the design or written into slice 0.

### MAJOR-4 — Section 1.1's "1.x packets per type" estimate is unverified hand-waving

**Design (Section 1.1):** "With 323 types and an average ~1.x packet per
type (most static props have a single material), the practical draw count
is in the 300-500 range."

The "323 types" is itself unverified at write-time (the grep target is
`s_types.size()`, which is a runtime quantity). The "~1.x packet per type"
is asserted without grep. The cost analysis (Section 6 perf gate target
<=200 us) is derived from this draw-count estimate. If the actual mean
packet-per-type is 3+ on a real map (multi-material buildings or
animation-LOD damage variants), the 300-500 estimate is wrong by 3x and
the multi-draw payoff is bigger but the audit cost is also bigger.

**Fix:** at design time, capture the actual `Sigma packetCount` count from one
of the tier1 missions via the existing batcher counters and paste it
in. This is a 5-line `[OBJBATCHER v1]` log read.

### MAJOR-5 — No kill-switch / opt-out env var defined for default-on flip

The design proposes `MC2_GPU_CULL_MULTIDRAW_AUDIT=1` for diagnostics but no
**production opt-out**. Per worktree convention (`memory/track_c_compute_cull.md`,
which the design cites as origin), every Track C ship was paired with an
env-gated revert path: `MC2_GPU_CULL=0`, `MC2_GPU_CULL_SUBSTRATE=0`,
`MC2_GPU_CULL_LIFECYCLE=0`. The substrate-multidraw path needs the same
lever: `MC2_GPU_CULL_SUBSTRATE_NOMULTIDRAW=1` -> falls back to today's
per-bucket `glDrawElementsIndirect` loop (the `useC1bIndirect` branch at
`gos_static_prop_batcher.cpp:1691–1716`, which is the perfect rollback
target — it already exists, gated by `compute_isEnabled()`).

If the substrate is later default-on'd (the soak gate), and a regression
slips through (a new map with a mixed-alpha type, a driver update, anything),
the only revert is to disable the entire substrate. That blast radius is
larger than necessary.

**Fix:** add a section "Kill-switch — `MC2_GPU_CULL_SUBSTRATE_NOMULTIDRAW`,
when set, falls through to the existing per-bucket loop. Default off.
Validates by env_var grep; matches Track C precedent."

### MAJOR-6 — Per-mission lifecycle: bindless handle table teardown not wired to onMapUnload

**Design (R6):** "the handle table SSBO itself must be deleted in
`onMapUnload` paths. Pattern reference: `onMapUnload` at
`gos_static_prop_batcher.h:99`."

But the cited file:line — `gos_static_prop_batcher.h:99` — only **declares**
`void onMapUnload();`. The actual implementation lives in the .cpp file
and the design does not enumerate which buffer-delete it would add. If the
implementation at slice time forgets the `glMakeTextureHandleNonResidentARB`
call (which is a lifecycle-lifetime constraint from
`memory/cull_gates_are_load_bearing.md` discipline applied to a new
resource), the handle table grows across mission cycles.

The design also doesn't address the existing `s_typeRanges`, `s_instanceSsbo`,
`s_colorSsbo` lifetime — does the multi-draw add a new SSBO (per Section
4.2 "bind bindless texture handle table") and where is that buffer
declared/created/destroyed? Each new buffer is a 5-line implementation but
the design must list them.

---

## MINOR findings (documentation gaps)

### MINOR-1 — Verification appendix's NF entries are useful but the line corrections themselves drift

The appendix calls out two NF entries (memory file's `:2321` superseded to
`:2410`, and "loop is in batcher not compute"). However, the design's own
PR1 citation in Section 1.6 (`:2410`) is itself stale — the actual line
at review time is **`:2423`**. Section 1.6 needs the same self-correction
the appendix already applies. (The appendix says "this design treats the
symbol, not the line, as authoritative" — fine, but then Section 1.6's
prose repeats `:2410` as if it were authoritative.)

### MINOR-2 — AMD attribute-0 banner cited at wrong line range

Section 1.6 cites `gameos_graphics.cpp:2210–2213`. Actual banner is at
`:2217–2230` (the lines `:2210–2213` are restoration code from the prior
function `gos_terrain_bridge_renderWaterFast` — entirely unrelated). The
banner moved with the cement-multi-sampler PR2 and the design's cite did
not update.

### MINOR-3 — Section 6 perf gate is a target without a baseline

"Baseline measurements at design-write time: NOT YET CAPTURED." The
implementation plan is told to capture baseline at slice 0. Per worktree
"Documentation Discipline" rule: numbers come from actual measurements
made at write-time, not from prior memory files. The design cites
`track_c_substrate_regression.md`'s "~120 us off, ~2 ms on" but flags
it `M` (not measured here). Capturing one Tracy zone before writing the
design is minutes of cost; deferring to slice 0 means the gate is
defended by un-verified prior-session numbers.

### MINOR-4 — No mention of cull-gate cascade rule

`memory/cull_gates_are_load_bearing.md` warns that bypassing CPU cull gates
cascades into pool exhaustion, lifecycle destruction, and stale matrices.
The substrate path is already past the CPU cull gate (GPU-authority indirect
draws), so the multi-draw coalesce itself doesn't reintroduce a CPU cull
cascade. But the design should explicitly state that — adversarial readers
will look for the cascade analysis and not finding it suggests the design
didn't consider it. One sentence in Section 7 suffices.

---

## Cross-cutting questions

1. **Stage A vs Stage B partial-landing hazard.** Design (Section 2.3) says
   "Slice the work as Stage A (multi-draw with bindless textures or sampler
   array) and Stage B (alpha-test split into two groups). Stage A is the
   perf payoff; Stage B is a mechanical follow-up." If Stage A ships the
   multi-draw without the alpha-test group split, then Stage A's single
   multi-draw must dispatch ALL 323 types under one `u_materialFlags` value,
   silently breaking alpha-tested trees/fences. **The design needs an
   explicit "Stage A and Stage B land in the same PR or Stage B precedes
   Stage A" rule.** As written, the staging is partial-landing-hazard.

2. **Texture-binding strategy is undefined.** Section 2.2 #1 lists three
   candidates (bindless / array / atlas) and Section 2.3 picks bindless,
   then Section 4.3 defines a "fallback" sampler-array path. Pick one as
   primary at design time. Recommend sampler-array given precedent
   (cement-multi-sampler v2) and lack of bindless tooling — but the
   adversarial reviewer can't make this call.

3. **`u_packetID` debug-mode regression.** CRITICAL-1. Either find a way to
   preserve RAlt+9 mode 2 entropy under multi-draw, or document the
   regression as accepted.

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **Bindless vs sampler-array vs atlas for the per-packet texture
   variation problem.** This is the single biggest architecture decision in
   the design. CRITICAL-3 argues against bindless on absence-of-precedent
   grounds; the cement-multi-sampler v2 precedent argues for sampler-array.
   Atlas is rejected by the design as "much larger PR" (Section 2.2 #1) but
   that reasoning isn't adversarially defended — atlasing 323 types may
   actually be smaller than introducing two GL extensions to the codebase.

2. **`gl_DrawID` / `ARB_shader_draw_parameters` adoption.** CRITICAL-2 notes
   the sibling brainstorm rejected this. The user/advisor must explicitly
   sign off either "yes we are now adopting it, accepting the
   `#extension`/`#version 460` violation of CLAUDE.md's '#version 430'
   rule" or "no, find a different mechanism" (e.g., layer-index in the
   per-vertex stream).

3. **Mixed-alpha type handling at registration time.** MAJOR-3. Either
   accept the cardinality probe must happen before design freeze, or
   commit to "split into virtual buckets at registration" as the path —
   with all the surface that implies.

4. **`u_packetID` debug-mode preservation vs accepted regression.**
   CRITICAL-1. The RAlt+9 mode 2 hash is a documented diagnostic; losing
   it is a contract change for the user.

5. **Audit ring vs `glGetBufferSubData`.** CRITICAL-4. If the audit must
   not stall, it has to use the C2 readback ring. That's another
   subsystem to extend.

---

## Summary

- **CRITICAL: 4** (frag debug-hash usage misread; ARB extension conflict
  with sibling brainstorm; bindless lifecycle/precedent missing; audit
  layer is itself a perf regression).
- **MAJOR: 6** (SSBO offset invariant deferred; patch-shader permutation
  cost untraced; mixed-alpha probe deferred; packet-per-type estimate
  unverified; no production kill-switch; bindless teardown spec missing).
- **MINOR: 4** (line drifts; missing baseline; missing cascade-rule
  acknowledgement).

**Verdict: STOP THE LINE.** Revise before any implementation slice opens.
The grep'd evidence shows the design has structural issues beyond the
mechanical line-shift kind — it conflates two pre-rejected
architectural choices, misses one consumer of `u_packetID`, and proposes
an audit mechanism that contradicts the project's own readback-ring
discipline. None of this is reachable by prose-only review against the
brainstorm; only by grepping `static_prop.frag` for `u_packetID`,
`shaders/*` for `gl_DrawID`, `GameOS/` for `bindless`, and the sibling
brainstorm at `:126`.

---

*Reviewer: subagent under adversarial-plan-review skill, dispatched per
worktree CLAUDE.md "Review Discipline." All citations verified at review
time against `claude/nifty-mendeleev` worktree HEAD. No source touched.*
