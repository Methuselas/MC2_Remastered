# Adversarial review -- RenderWorld Slice M1.5 (ObjectID Buffer) spec DRAFT

- Target: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md` (DRAFT, ~973 lines)
- Reviewer stance: actively break. Every cited symbol grep-confirmed at
  write-time against worktree HEAD `nifty-mendeleev/`.
- Skill: `.claude/skills/adversarial-plan-review.md` verbatim.
- Date: 2026-05-23.
- Decisions honored (not relitigated): static-prop indirect ONLY;
  attachment slot 2 (NOT 1); picking deferred to M1.6; Section 8 is
  M1.6 reference material.

---

## Verdict

**CONDITIONAL -- BLOCK on Q1.** The spec is structurally sound, the
scope narrowing (M1.6 split) closes a class of ambiguity, and Sections
2-7/9-13 are largely grep-verified true. However a single CRITICAL
finding (multiple post-scene-FBO bind sites that re-issue
`glDrawBuffers(1, &singleBuf)` and structurally DROP attachment-2
from the active draw-buffer write mask) means M1.5 cannot land
as-written without either (a) a defense-in-depth re-issue of the
3-buffer list on every `sceneFBO_` rebind, or (b) an explicit proof
that none of the four offending bind sites occurs between the
static-prop draw entry and the next full-FBO rebind. The spec's
own Q1 names this exact hazard as "load-bearing unverified"; this
review confirms it is in fact violated by shipped code. Two MAJORs
follow from the same root (env-OFF zero-delta claim contradicted by
Section 9, and unverified AMD integer-MRT slow-path claim in
Section 6). Promote to EXECUTABLE only after the Q1 mitigation is
written into Section 3 with concrete rebind discipline and the
env-OFF "table populated always" claim is reconciled with the
"zero CPU cost" claim.

**Counts:** CRITICAL = 1, MAJOR = 3, MINOR = 6.

---

## CRITICAL findings

### C1. `glDrawBuffers(1, &singleBuf)` at four post-process bind sites silently DROPS attachment-2 from the active write mask

**Spec line:** Section 3 ("Multi-pass interaction"), L202-212 and
Section 14 Q1 (L775-786, "load-bearing unverified").

**Spec claim:** "The MRT attachment list is set ONCE at FBO setup;
all subsequent passes share the same bound attachments. Passes that
do not emit a `location=2` output leave it undefined per the GL spec
-- which is why M1.5 mandates that the attachment is CLEARED to zero
at frame start AND that the static-prop fragment shader is the only
program declaring an output at location 2 in this slice."

**Grep verification of every `glBindFramebuffer.*sceneFBO_` site:**

| File:line | Context | `glDrawBuffers` call | Drops slot 2? |
|---|---|---|---|
| `gos_postprocess.cpp:239` | `createFBOs()` initial setup | `glDrawBuffers(2, {C0, C1})` at :274 | YES (correct shape at setup is still 2-list; M1.5 must promote to 3) |
| `gos_postprocess.cpp:405` | `beginScene()` per-frame MRT rebind | `glDrawBuffers(2, {C0, C1})` at :418, gated on `sceneNormalTex_` truthy | YES -- frame-entry rebind list is the live shape (2) |
| `gos_postprocess.cpp:503` | `runScreenShadow()` post-process pass | `glDrawBuffers(1, &singleBuf)` at :505 -- `singleBuf = GL_COLOR_ATTACHMENT0` | **YES, single-buf list, attachment-2 inactive after this call** |
| `gos_postprocess.cpp:613` | `runGodRays()` Pass 2 composite | `glDrawBuffers(1, &singleBuf)` at :615 | **YES, single-buf list, attachment-2 inactive after this call** |
| `gos_postprocess.cpp:646` | `runShoreline()` foam pass | `glDrawBuffers(1, &singleBuf)` at :648 | **YES, single-buf list, attachment-2 inactive after this call** |
| `gos_postprocess.cpp:965` | `endShadowPass()` restore-bind | no `glDrawBuffers` -- whatever list was last set persists | conditional; depends on prior call ordering |

Three of these (lines 503, 613, 648) are inside `endScene()` which
runs AFTER the scene draw (`runScreenShadow`, `runShoreline`,
`runGodRays`, `runBloom`, then default-FBO composite). So in
strict M1.5 frame ordering they fire *after* the static-prop pass
and do NOT affect the same-frame static-prop ID write. **But this
is precisely the latent hazard:** the next-frame `beginScene` at
:405-418 conditionally re-issues `glDrawBuffers(2,{C0,C1})` gated
on `if (sceneNormalTex_)`. That list is 2 entries, not 3. **Even
without any mid-frame rebind, the per-frame entry path re-asserts
a 2-entry list, NEVER a 3-entry list** -- so under the spec's
"add attachment-2 to `createFBOs()`" change alone, the per-frame
`beginScene` MRT rebind would actively REMOVE attachment-2 from
the write mask every frame.

This is not a theoretical Q1 audit deferred to adversarial review;
the offending call is in the same file the spec inspected (it cites
`gos_postprocess.cpp` 405-419 in Appendix A as the per-frame FBO
bind) and the comment on :416 explicitly enumerates the 2-buffer
list as the live frame-entry rebind. The spec's Section 3 wording
("set ONCE at FBO setup; subsequent passes share the same bound
attachments") is factually FALSE -- the codebase resets the
draw-buffer list at minimum once per frame, plus three times per
post-process pass.

**Code evidence:**

```
gos_postprocess.cpp:416-419
    if (sceneNormalTex_) {
        GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, drawBuffers);
    }

gos_postprocess.cpp:503-506
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
    ...
```

There is no contention that the static-prop draw happens before
`endScene()` -- it does. The hazard is the per-frame `beginScene`
re-assertion of a 2-entry list. Without an explicit fix, attachment-2
will receive ZERO writes from the static-prop fragment shader
because GL's draw-buffer write mask masks the `out uint v_objectId`
write to a slot index (2) that is not in the active list.

**Recommendation (mechanical):**

The spec MUST require, with file:line specificity:

1. **`createFBOs()` (`gos_postprocess.cpp:272-274`):** when env-ON,
   `drawBuffers` extends to 3 entries `{C0, C1, C2}`. Env-OFF: stays
   at 2. (Already present in spec Section 3 L179-182.)

2. **`beginScene()` (`gos_postprocess.cpp:416-419`):** when env-ON,
   the per-frame rebind issues `glDrawBuffers(3, {C0, C1, C2})`.
   The current 2-entry list is INSUFFICIENT. **MISSING from the
   spec.**

3. **`runScreenShadow` / `runGodRays` / `runShoreline` (`:505/:615/:648`)**
   currently issue `glDrawBuffers(1, &singleBuf)` to disable
   GBuffer1 writes during single-attachment composite. These run
   AFTER static-prop draw, so they don't affect same-frame writes,
   but they DO leak state into the next frame's `beginScene`
   rebind path. The spec must add an explicit "after the
   single-buf pass restores the scene FBO list" rule -- or rely
   on `beginScene` re-asserting the full list. If the latter,
   per (2) above the `beginScene` list MUST be the 3-entry shape
   when env-ON.

4. **Defense-in-depth (recommended):** every site that issues
   `glDrawBuffers(N, list)` against `sceneFBO_` MUST do so via a
   single helper in `gos_postprocess.cpp` (call it
   `setSceneDrawBuffers(int n_buffers_excluding_objectid)`) that
   appends `GL_COLOR_ATTACHMENT2` to the tail when env-ON. This
   collapses the 4-site audit predicate into one source of truth
   and survives future passes added by later slices. Without this,
   the M2 plan adds the same hazard for mech IDs.

**Severity: CRITICAL.** Without the `beginScene` fix, the spec's
own "byte-identical when env-ON minus debug writes" promise is
unreachable for the static-prop draw -- attachment-2 stays cleared
to zero across the entire frame because no write mask permits it,
and `lookupAtPixel` returns `Handle::invalid()` for every static
prop. The visual canary gate (Section 12) catches this; the
adversarial review catches it BEFORE writing the plan that fails it.

This is also the exact failure mode of `cull_gates_are_load_bearing`
applied to MRT: state that "is set once" in spec prose is in
practice "re-asserted in N places" in code, and a slice that
inherits the prose's mental model gets bitten.

---

## MAJOR findings

### M1. Section 9 contradicts itself on env-OFF runtime cost: "table populated always" vs "zero pixel delta vs M1 HEAD"

**Spec lines:** Section 9 L568-572 ("`s_objectRecords` table is still
populated"); Section 9 L573 ("**Tier1 5/5 invariant:** byte-identical
pixels vs M1 HEAD"); Section 9 L586-590 ("Goal: zero pixel delta, zero
measurable CPU cost").

The two statements coexist. Pixel-identical is satisfied trivially
(no shader recompile, no attachment created, no write). But the
"zero measurable CPU cost" claim is FALSE on its face: every
`upsertStaticProp` call now does a `s_objectRecords.resize()` /
`s_objectRecords[idx] = record` write. At mc2_24 = 2641 props per
mission, this is a non-trivial CPU walk over the per-prop
production path.

The contradiction is partly cosmetic ("trivial != zero"), but the
M1 spec used "byte-identical" to mean "tier1 PASS plus zero
behavioral delta" and the same standard should apply here. Either:

- The spec acknowledges the small CPU cost (and the M1.5 plan adds
  a tier1 perf delta capture step with an explicit budget for the
  always-on table population), OR

- The table population is also env-gated, in which case **every
  caller path that assumes `s_objectRecords[h.index()]` is valid
  must check the env flag first**, including the `lookupAtPixel`
  no-op return.

**Recommendation:** pick one. The clean answer is "table always
populated; the cost is bounded by upsert-rate not by frame-rate;
document it explicitly." But the spec MUST state which side.
Currently L568-572 ("still populated") and L590 ("one extra
branch-not-taken per draw") fight each other -- per-draw branches
are paid per-frame, table population is paid per-mission, and the
spec collapses both into "zero" without distinguishing them.

**Severity: MAJOR.** Surface to user; mechanical choice.

### M2. Section 6 ("AMD does not write-through-undefined on integer formats") is uncited and stays an UNVERIFIED ASSUMPTION

**Spec lines:** Section 6 L389-391 ("Verified acceptable: AMD driver
does not write-through-undefined on integer formats per
`docs/amd-driver-rules.md` (no known counterexample; flag for
adversarial-review pass).")

`grep -in 'integer|R32|MRT.*uint|uint.*MRT' docs/amd-driver-rules.md`
returns **NO matches** at worktree HEAD. The cited rule does not
exist in `docs/amd-driver-rules.md`. The spec is hand-waving a
load-bearing AMD behavior claim against an empty source.

This is the canonical "depth-flip review missed two writers" trap
(`brainstorm_code_grounding_lesson.md`) at a different level: the
spec's mitigation cites a doc that does not contain the cited rule.
Verify-cited-symbols (step 2 of the skill) caught it; step 9
exhaustive-census is not applicable (this is single-property AMD
behavior, not multi-site state) but the writer would still be
required to *produce* the rule or remove the cite.

**Recommendation:** Either (a) the M1.5 plan adds a startup
self-test that writes a known sentinel from a non-static-prop
shader at unbound location=2 and asserts the attachment retains
the clear value, OR (b) the spec adds the runtime measurement to
the visual canary mandatory gate (Section 12). Citing a rule that
doesn't exist is worse than acknowledging unknown -- the latter
permits caution, the former invites future readers to skip the
verification because "it's in the AMD rules doc."

**Severity: MAJOR.** Flag for codex sign-off (the user's listed
required follow-up).

### M3. Coalesce-path `PerDrawEntry` `_pad0` reuse breaks std430 layout commitments

**Spec lines:** Section 5 L325-329, Section 14 Q8 L843-852 ("Lean:
reuse `_pad0` (rename to `objectIdRaw`) when MC2_COALESCE is on; the
struct layout stays the same size.").

**Grep verification at `gos_static_prop_batcher.h:46-64`:**

```
46:struct PerDrawEntry {
47:    int32_t packetID;          //  0
48:    int32_t materialFlags;     //  4
49:    int32_t maxLocalVertexID;  //  8
50:    int32_t texArrayLayer;     // 12
51:    float   uvScaleX;          // 16
52:    float   uvScaleY;          // 20
53:    int32_t _pad0;             // 24
54:    int32_t _pad1;             // 28
56:static_assert(sizeof(PerDrawEntry) == 32, "PerDrawEntry std430 size");
57-64: offset asserts for EVERY field, including _pad0 at offset 24 and _pad1 at offset 28.
```

**The hazard:** `_pad0` and `_pad1` are NOT unused dead-space. They
satisfy `static_assert(sizeof(PerDrawEntry) == 32, ...)` AND
`static_assert(offsetof(PerDrawEntry, _pad0) == 24, "_pad0 offset")`
on lines 56 and 63. Renaming `_pad0` -> `objectIdRaw` changes the
field name but NOT the size or offset, so the static_asserts that
reference `_pad0` by name (`offsetof(PerDrawEntry, _pad0)`) will
FAIL to compile after the rename.

The shader-side `PerDrawEntry` in `static_prop.frag:36-46`
references `_pad0` symbolically; that side breaks the same way.

**Recommendation:** Spec Q8 must be hardened from "lean: reuse
`_pad0`" to a concrete refactor: either (a) rename `_pad0` -> `objectIdRaw`
on the C++ side AND in the GLSL struct AND in the offset-assert,
OR (b) keep `_pad0` as padding and add a separate `int32_t
objectIdRaw` field at offset 24 (push `_pad0` to offset 28,
delete `_pad1`, keep total size 32; updates ALL offset asserts and
the shader struct). Either is fine; the spec needs to commit.

This is also a *substitutive-not-additive* check: option (a) is
substitutive (rename), option (b) is substitutive (replace one
pad with content). Neither expands the struct -- good.

**Spec gap:** Section 14 Q8 invites the plan author to "verify
at plan time by grepping every site that writes `PerDrawEntry`"
without naming the producer (`gos_static_prop_batcher.cpp`, the
`materialFlags|texArrayLayer|uvScale*` write site -- per the
header comment at :22 about prior Stage 2.A `_pad0` repurpose).
The spec should at minimum cite Stage 2.A as prior art for what
"repurpose `_pad0`" means concretely (header comment at L22 says
"Stage 2.A repurposes the prior _pad0 slot at offset 76" -- this
references a different struct or stage; needs disambiguation).

**Severity: MAJOR.** Without explicit struct-layout commitment,
the M1.5 plan will compile-fail at static_assert.

---

## MINOR findings

### m1. `glClearBufferuiv(GL_COLOR, 2, {0,0,0,0})` correctness

Section 3 L185-187 says "Per-frame clear: `glClearBufferuiv(GL_COLOR,
2, {0, 0, 0, 0})` runs at scene-FBO clear time." This is correct GL,
but: `glClearBufferuiv` clears DRAW-BUFFER index 2, not
attachment-2. If the draw-buffer list at clear time is `{C0, C1, C2}`,
then index 2 maps to `C2` and the clear works. If the list is
`{C0, C1}` at that moment (e.g. env-OFF), index 2 is out of range
and the call is a no-op (or silent error). Defensive: the spec
should explicitly require clear-after-`glDrawBuffers(3, ...)`
ordering, mirroring the existing `clearGBuffer1()` discipline at
`gos_postprocess.cpp:484-488` which has a similar comment block.

**Recommendation:** Section 3 add: "`glClearBufferuiv` is called
ONLY after the env-ON 3-entry `glDrawBuffers` list is bound;
clear-list-index 2 maps to attachment-2."

### m2. `LookupResult::pipelineId` is `uint16_t` but Section 4 record has it as `uint16_t pipelineId` and Section 7 fields claim "0 = unknown"

Section 4 L249 declares `uint16_t pipelineId; // M1.5: opaque sentinel`.
Section 7 L431 `uint16_t pipelineId; // 0 = unknown`. The spec is
internally consistent but `PipelineId::unknown()` (mentioned at L267)
implies a strong type. If `PipelineId` is a wrapper type with
`unknown()` static factory, the `LookupResult` field should be
`PipelineId`, not raw `uint16_t`. If `PipelineId` is "documentary
only" (per L264, the type doesn't exist yet), then `uint16_t` is
correct -- but the API contract should not advertise a type name
(`PipelineId::unknown()`) for a type that doesn't compile.

**Recommendation:** strike the `PipelineId::unknown()` reference at
L267-268. Replace with "literal `0`". Same for `RenderPathDecision.reason`
(L273, "the capability resolver does not exist yet") -- spec text
referencing `pathReasonCode = 0` is correct, but consider
explicitly noting the field shape is forward-compatible.

### m3. `glReadPixels(GL_RED_INTEGER, GL_UNSIGNED_INT)` per Section 7 L450 -- portability

`GL_RED_INTEGER` + `GL_UNSIGNED_INT` is the correct format/type pair
for reading from a `GL_R32UI` integer-format attachment. NVIDIA and
AMD both support this since GL 3.0. The spec is correct, but it
should explicitly note that the standard `GL_RED` + `GL_FLOAT` pair
will silently fail (or worse, reinterpret bits) on integer-format
attachments. No code change; documentation hardening only.

### m4. `glTexStorage2D` vs `glTexImage2D` (Section 14 Q7, L835-841)

The spec L840 ("Lean: `glTexStorage2D` -- it is immutable-storage
style, future-proof, and Vulkan-shaped") is correct, but the actual
M1 `sceneNormalTex_` creation at `gos_postprocess.cpp:265` uses
`glTexImage2D`, NOT `glTexStorage2D`. The spec is implicitly
advocating that M1.5's new texture diverge from M1's pattern. This
is a code-style inconsistency, not a bug, but should be resolved
either by:

- Using `glTexImage2D` for `sceneObjectIdTex_` to match the other
  scene FBO textures (existing pattern), or
- Migrating ALL scene FBO textures to `glTexStorage2D` (a side
  scope creep -- needs its own justification per
  `memory/minimal_touch_modern_when_touched.md`).

**Recommendation:** Default to `glTexImage2D` for M1.5 (minimal
touch); defer `glTexStorage2D` migration to a separate slice that
covers all four scene FBO textures together.

### m5. Section 12 ID-readback unit check is good, but missing one assertion

The L695-704 self-test:
1. Register prop, draw, readback pixel -- assert == handle.raw().
2. Destroy prop; re-register; assert new handle.generation > old;
   assert lookup with OLD handle returns invalid.

Missing assertion: **after destroy, before re-register, the OLD
handle.index() slot's record.alive flag should be cleared.** The
generation check alone is sufficient at the API surface, but the
record state is internal and the self-test is in-binary -- it
should verify the internal flag too (defense-in-depth). Add a
step 5: after destroy, before re-register, assert
`s_objectRecords[oldHandle.index()].alive == false`.

### m6. Vulkan-prep claim Q9 is mechanically right but glosses sync

Section 14 Q9 L854-861: "`glReadPixels` synchronous readback ->
`vkCmdCopyImageToBuffer` + fence-wait." Correct shape but skips the
multi-frame buffering Vulkan needs to avoid stalling the entire
queue. The M1.5 sync `glReadPixels` is fine for GL (driver handles
the fence implicitly); the Vulkan port needs an explicit fence and
a host-readable staging buffer. Not in scope for M1.5; flag for the
Vulkan-port slice spec when that's authored.

---

## Strengths confirmed under grep

- **Attachment slot 2 is free.** `grep 'layout\s*\(\s*location\s*=\s*2\s*\)\s*out' shaders/` returns NO matches (zero shader currently declares an output at location 2). The spec's Section 6 audit predicate is correct.
- **`GL_COLOR_ATTACHMENT1` is `sceneNormalTex_` (GBuffer1).** Verified at `gos_postprocess.cpp:262-274`. The spec's correction of the user's "2nd color attachment" wording (Section 3 L165-172) is accurate.
- **No deferred-render path hides a second consumer of slot 2.** Single-FBO scene draw at HEAD; no other attachment competes for slot 2 in `createFBOs()`.
- **`RenderObjectRecord` / `s_objectRecords` symbols do NOT collide with existing RenderWorld names.** Confirmed by grep across `RenderWorld/RenderWorld.{h,cpp}` -- no current declarations of either symbol.
- **`Handle::invalid().raw() == 0` matches the per-frame clear value.** Verified at `RenderCore/Handle.h` per spec Appendix A L876-878.
- **`out uint` declaration is safe when MC2_OBJECT_ID_BUFFER=0** (Q2). The macro-gate approach (L797-799) is the conservative choice; AMD does silently accept fragment outputs that map to no attachment in the GL spec, but the spec's lean to "macro-gate the declaration AND the write" eliminates that variable. Good defensive choice.
- **`s_objectRecords` sizing.** 2641-prop worst-case (mc2_24) at 32 bytes/record = ~85KB. Trivial; the `std::vector` lean (Section 4 L284-288) is correct.
- **PerDrawEntry static_asserts exist and enforce the layout.** This is what makes M3 above a CRITICAL-adjacent finding rather than a silent corruption.
- **Section 8 deferral is consistent.** Sections 5, 7, 12 do NOT require missiongui.cpp changes. The deletion-criteria text at L524-537 is documentary; no code gate references missiongui.cpp in the M1.5 mandatory list.
- **Render-contract registry coherence step (Section 12 L725-731)** is well-scoped; the `requiresMRT=true` flag already in `mclib/render_contract.h:79` will accept the GBuffer2 addition cleanly.

---

## Decisions needing user / advisor sign-off before revision pass

1. **C1 mitigation shape.** Pick (a) per-site re-issue of `glDrawBuffers(3, {C0,C1,C2})` after every `sceneFBO_` rebind, (b) single `setSceneDrawBuffers()` helper consolidating all 4+ sites, OR (c) leave the per-site sites alone and rely on the next-frame `beginScene` re-assertion of the full list (requires updating `beginScene` to the 3-entry list when env-ON). Option (b) is the cleanest substitutive-not-additive shape and survives M2+. **Defer to user.**

2. **M1 reconciliation.** Pick: env-OFF table population is "always on" (cost acknowledged in Section 10) vs "env-gated" (lookup API stays consistent, but all callers of `s_objectRecords[h.index()]` need the env check). **Defer to user.**

3. **M2 mitigation.** Pick: add startup self-test for AMD integer-MRT write-through behavior, OR add a doc rule to `docs/amd-driver-rules.md` that codifies the observation (with the canary mission as the durable test). **Defer to codex per spec's own required-follow-up list.**

4. **M3 struct-layout choice.** Pick: rename `_pad0` -> `objectIdRaw` (single offset asserts updated), OR insert new field at offset 24 and shift padding (multiple offset asserts updated). **Defer to user; cleaner option is the rename.**

5. **m4 cosmetic.** Either `glTexImage2D` (M1 pattern) or `glTexStorage2D` (Vulkan-prep). Lean: match M1.

---

## Cross-checks against load-bearing memory

- `cull_gates_are_load_bearing.md`: Not a cull change. N/A.
- `gpu_direct_renderer_bringup_checklist.md`: M1.5 is not a new GPU-direct renderer; it extends an existing one (static-prop batcher). No new bring-up surface.
- `glprogramuniform_vs_gluniform_explicit_program_trap.md`: Spec L335-339 correctly uses `glProgramUniform1i` for `u_objectIdRaw` upload. **Confirmed grep-grounded.**
- `uniform_uint_crash.md`: Spec L315-318 correctly declares `uniform int u_objectIdRaw` and casts to `uint` in shader. **Confirmed grep-grounded.**
- `glsl_preprocessor_does_not_inherit_cpp_build_flags.md`: Spec L353-360 correctly addresses macro propagation via `makeProgram()` prefix string. **Confirmed grep-grounded.**
- `feedback_offload_must_be_substitutive_not_additive.md`: Spec Section 11 "Substitutive-not-additive check" L662-670 correctly observes that static-prop selection did not exist on CPU previously; M1.5 introduces a NEW capability with single source of truth. **Confirmed grep-grounded** -- the spec's framing is the correct application of this discipline.
- `shader_exe_deploy_lockstep.md`: The M1.5 shader change to `static_prop.frag` triggers full shader-tree redeploy. Spec doesn't mention this; the M1.5 plan must.

---

## Census-style sanity (step 9 of the skill)

This is not a cross-cutting global-convention change (no depth-func
flip, no blend convention change, no clip-control flip). Step 9 does
not apply in the exhaustive-census sense -- the change is localized
to one FBO, one attachment, one shader, one upload site. However the
*spirit* of step 9 applies: the spec's "set ONCE at FBO setup"
mental model is the same failure mode as the reverse-Z depth-func
case. C1 above is the analog finding: the spec verified the cited
file (`gos_postprocess.cpp:405-419`) and missed the four sibling
bind sites in the SAME FILE that re-issue `glDrawBuffers` with
shorter lists. The fix is one helper function; the lesson is the
same.

---

## End of review

This is a CONDITIONAL review. The spec is well-structured and the
scope-narrowing (M1.6 split) is a good simplification. The single
CRITICAL is addressable by adding three lines to Section 3 plus an
explicit Section 12 gate ("self-test that exercises the per-frame
`beginScene` rebind and confirms attachment-2 is in the active draw
mask"). The three MAJORs are choose-one decisions; the six MINORs
are documentation hardening.

Recommended path: revise Section 3 (C1 fix), reconcile Section 9
(M1), strengthen Section 6 cite or remove it (M2), commit to
struct-layout option (M3), then promote to EXECUTABLE for the
codex / greybeard passes called out in spec L24-29.
