# Substrate Multi-Draw Coalesce — v2 Design

> **Status:** Stage 1 spec — **v2r20, implementation-ready**. v2r20 is a
> documentation-only cleanup pass after the fresh-session adversarial
> review against v2r19 / plan v3.5: 0 CRITICAL findings, the two big
> reverts (no slot-1 bind, no mid-mission indirect-buffer rebuild) were
> independently confirmed as the right calls. v2r20 changes are §11.X
> honesty rewrite (disarm fallback can corrupt all 5 indirect-command
> fields, not just `baseInstance`) and §5.5 added paragraph documenting
> the pre-existing `bucketCaps[]` staleness on legacy ring grow as
> deferred to a separate slice. No design changes. Stage 2+3
> implementation may begin per §13's single-PR rule.
>
> **Supersedes:** `2026-05-07-substrate-multidraw-coalesce-design.md` (v1).
>
> **Revision history:**
> - v2r20 (2026-05-09): documentation-only cleanup after fresh-session
>   adversarial review of v2r19 / plan v3.5. Two doc edits:
>   * §11.X first paragraph rewritten — disarm-fallback corruption is
>     ALL FIVE indirect-command fields (`count`, `instanceCount`,
>     `firstIndex`, `baseVertex`, `baseInstance`), not just stale
>     `baseInstance`. Same accept-and-document conclusion; honest about
>     scope.
>   * §5.5 adds a paragraph documenting the pre-existing
>     `bucketCaps[]` staleness on legacy ring grow as a known
>     limitation deferred to a separate slice. Explicitly forbids
>     adding a `compute_buildIndirectBuffer()` re-call hook into the
>     grow path here (rebuild paths are easy to get wrong; v3.4 → v3.5
>     burned that lesson recently).
>   No state-machine, layout, binding, or invariant changes. Plan v3.6
>   is the matching cleanup pass.
> - v2r19 (2026-05-09): adversarial pass against plan v3.4 caught that
>   the mid-mission rebuild mechanism (`s_coalesceNeedsNaturalIndirectRebuild`
>   + `compute_rebuildIndirectBufferNatural`) added in plan v3.4 had
>   a fatal blackout chain. `compute_dispatch()` runs BEFORE `flush()` and
>   has already patched `instanceCount` into `s_indirectCmdBuf` for this
>   frame's draws. The mid-flush rebuild called `glBufferData` which
>   orphans the buffer and re-uploads `cmds[]` with `instanceCount = 0`
>   — the legacy draw on the disarm frame would render every static prop
>   invisible. Both opus and sonnet adversarial reviews independently
>   traced the chain. v2r19 + plan v3.5 revert the rebuild mechanism:
>   runtime disarm via §5.1 (`type_overflow`) or §CRITICAL-B
>   (`tex_evicted`) ONLY flips `s_coalesceArmed = false` and logs.
>   The legacy `glDrawElementsIndirect` fallback path may render with
>   stale sorted-layout `cmd.baseInstance` values until the next mission
>   load — accepted as a documented limitation. Both disarm paths are
>   expected to be rare (the §5.1 cap formula is generous; texture
>   eviction is mitigated by §CRITICAL-B's refcount-aware
>   `pinNode()`). Operator can re-load the mission to recover. **Do
>   NOT add a mid-frame rebuild** — the spec explicitly forbids it.
> - v2r18 (2026-05-09): adversarial pass against plan v3.1 found that
>   v2r15+'s slot-1 invariant was based on a misreading of the live
>   shader. Verified by grep against `shaders/static_prop.vert` (line 56:
>   `colors_` declared at binding 1) and `shaders/static_prop.frag` (line
>   73: mode 4 reads `v_argb.rgb`, NOT `colors_.c[...]`). All eight
>   debug modes 0–7 read `v_argb` (vert-side computed lit color). No
>   live shader path reads slot 1. v2r15/v2r16/v2r17 §3.X.1's "load-
>   bearing for RAlt+9 mode 4 debug" claim was wrong. v2r18:
>   * §3.X.1 rewritten to acknowledge slot 1 is declared-but-unread.
>   * Coalesce branch must NOT bind slot 1 (the v2r15..v2r17-prescribed
>     `glBindBufferBase(1, s_colorSsbo)` whole-buffer bind would have
>     indexed the wrong frame slot AND wrong type-bucket base anyway —
>     dead-and-misleading rather than dead-but-defensive).
>   * Future-shader caveat preserved as a one-paragraph note: any future
>     slice that reintroduces `colors_.c[...]` reads owns either the
>     absolute-offset conversion OR forcing that path back to legacy.
>   * Plan v3.1's `forceLegacyForColorDebug = (debugAddrMode_ == 4)`
>     predicate and Step 17.5b canary are reverted in plan v3.2 — they
>     were downstream of the same misreading.
> - v2r17 (2026-05-09): editorial consistency pass after v2r16. No
>   design changes. Outside reviewer caught that v2r16's automated
>   "binding 14 → 15" replace had partially corrupted (a) the v2r16
>   revision-history paragraph that flipped its own narrative — saying
>   the diagnostic was at slot 15 and v2r15 chose 15, both wrong;
>   (b) the §3.X save/restore pseudo-code which still passed the
>   literal `14` to `glGetIntegeri_v` / `glBindBufferBase` despite
>   `prevSsbo15` naming; (c) the §3.X "new bindings" table row which
>   still listed slot 14; (d) the §5.6 heading "**Binding 14**". v2r17
>   restores historical accuracy in the revision history (v2r15 used
>   14, diagnostic at 14, v2r16 flipped to 15) and makes every
>   current-tense body reference consistent at binding 15. Also
>   rewrote §5.6's "Binding 15" header paragraph as a two-flip history
>   so future readers understand 13 → 14 → 15 in one place.
> - v2r16 (2026-05-09): adversarial pass on the v2r15 plan (opus +
>   sonnet + outside reviewer) found:
>   * **out4-CRIT-1 (§3.X.1):** v2r15 §3.X.1 said the legacy prologue
>     binds slot 1 (colors) AND slot 2 (per-type). Code reality: slot 2
>     IS in the prologue at `:1472–1474`, but slot 1 (`s_colorSsbo`) is
>     bound at `:1554–1559` INSIDE the legacy per-type loop —
>     per-type-relative `glBindBufferRange`. Coalesce branch skips the
>     loop, so slot 1 is unbound (or stale) under coalesce-armed mode.
>     Production impact today is masked: `static_prop.vert` (Stage 2.C.2
>     GPU-lighting flip) does NOT read `colors_.c[...]` in any live
>     code path — slot 1 is a legacy debug substrate only (RAlt+9 mode
>     4). But the binding is load-bearing for future shader edits and
>     for debug-mode robustness; the fix is cheap. v2r16 corrects
>     §3.X.1's wording (slot 1 is NOT inherited from prologue) and adds
>     an explicit whole-buffer bind in the coalesce branch.
>   * **out4-CRIT-2 (§5.5 + §5.6b clarification):** `batcher_getTypeDrawInfo`
>     at `gos_static_prop_batcher.cpp:1994–1999` returns
>     `s_instanceCapacity` (the legacy global) as per-type cap, NOT
>     `type.instanceCap`. Plan v2's sorted branch (Step 9.2) used
>     `batcher_getInstanceCap(typeID)` (correct), but the natural
>     branch (Step 9.3) was preserved verbatim and still calls
>     `batcher_getTypeDrawInfo`'s legacy cap. The two branches index
>     different-sized buffers; spec didn't pin down which cap each
>     branch uses. v2r16 adds explicit guidance: legacy/natural branch
>     keeps `batcher_getTypeDrawInfo`'s `s_instanceCapacity` cap;
>     coalesce/sorted branch uses `batcher_getInstanceCap(typeID)`.
>     The two branches index different SSBOs (`visibleIds[]` for
>     natural, `s_coalesceInstanceSsbo` for sorted), so the cap
>     mismatch is correct by design — but it must be documented so a
>     future "unify the caps" refactor doesn't break either path.
>   * **out4-CRIT-3 (§6 + §7 timing):** `loadProgramsIfNeeded()` is
>     guarded by `s_programLoadTried` at `:201` (one-shot per process)
>     and runs from `finalizeGeometry()` at `:707` — BEFORE v2r15's
>     §5.0 had `s_hasShaderDrawParams` probe ordering. Result: when
>     `loadProgramsIfNeeded()` runs, `s_hasShaderDrawParams` is still
>     false; the coalesce program never compiles even when the
>     extension is supported. v2r16 fix: extension probe moves INSIDE
>     `loadProgramsIfNeeded()` at the top of its body, before any
>     `makeProgram` decision. §5.0's pseudo-code re-resolved
>     accordingly.
>   * **out4-MAJ-1 (§5.6 binding flip 14 → 15):** `gpu_cull_compute.cpp:855`
>     has an existing env-gated diagnostic readback at slot **14**
>     expecting the readback SSBO. v2r15's choice of binding **14** for
>     permutation would clobber the diagnostic's expected state. v2r16
>     moves permutation to **binding 15** (also free; both 14 and 15
>     are declaration-free in `shaders/`, but 14 is read by the C++
>     diagnostic). This is the second binding flip in 24 hours
>     (13 → 14 → 15); the reason is each successive review found a new
>     conflict at the previous slot. Slot 15 is verified clean against
>     both shader declarations AND C++ binding readbacks.
>   * **out4-MAJ-2 (§3.X save/restore + plan):** spec already required
>     save/restore for the new bindings; the plan must rebind slot 15
>     in the coalesce branch (not rely on patch-dispatch persistence,
>     which doesn't run when GPU cull is disabled).
>   * **out4-MIN-1 (§5.3 / plan):** `u_maxLocalVertexID` is uploaded
>     per-TYPE at `:1567`, not per-packet at `:1685–1688`. PerDrawEntry
>     is the right destination either way (sorted-slot indexing matches
>     per-type cadence), but plan prose conflated the cadence. v2r16
>     spec note clarifies; plan must follow.
> - v2r15 (2026-05-09): outside review of the v2r14 implementation plan
>   caught 4 spec-level gaps (the plan-writer had to choose semantics the
>   spec didn't pin down):
>   * **out3-CRIT-1 (§5.0 + §5.5):** v2r14's "early-return on coalesce
>     build failure" cascaded into total static-prop blackout because the
>     existing legacy `finalizeGeometry()` body at `gos_static_prop_batcher
>     .cpp:709–795` (shared VAO/VBO/IBO + per-type SSBO + program load +
>     `s_geometryFinalized = true`) runs AFTER the insertion point the
>     spec described for coalesce work. Any coalesce-build failure path
>     that returns before `:795` leaves `s_geometryFinalized=false` and
>     `flush()` early-returns at `:1320` for the entire mission. v2r15
>     adds a new §5.0 "Legacy finalize must always complete" invariant
>     and rewrites §5.5's ordering: coalesce build is a side-attempt that
>     never aborts legacy finalize.
>   * **out3-CRIT-2 (§6 + §11.7):** v2r14 specified `s_staticPropProgram
>     Coalesce` link + `u_drawIDBase` / `u_texArr` location caching but
>     was silent on the FIVE shared uniforms (`terrainMVP`,
>     `u_terrainViewport`, `u_mvp`, `u_fogValue`, `u_debugAddrMode`) that
>     `flush()` uploads to the legacy program at `:1447–1466`. GL uniforms
>     are program-scoped: `glUseProgram(s_staticPropProgramCoalesce)`
>     starts with default zero state → identity `terrainMVP` → all static
>     props collapse to clip-space origin. v2r15 adds the
>     "Shared-uniform upload contract" subsection in §6 + spelled-out
>     re-upload (or per-program location cache) requirement in §11.7.
>   * **out3-CRIT-3 (§5.6):** v2r14 placed the patch-shader permutation
>     SSBO at binding 13. `gpu_cull_block_rollup.comp:58` already declares
>     `binding = 13` for `BlockVis` and `gpu_cull_compute.cpp:47`
>     constants `BLOCK_VIS_BINDING = 13u`. Dispatch order makes the slot
>     mechanically reusable (rollup at `:874` follows patch at `:835`),
>     but the spec's "binding 13 is free" claim is factually wrong, and a
>     future cleanup or dispatch reorder reintroduces silent corruption.
>     v2r15 moved permutation to **binding 14** (verified free at
>     v2r15 plan-write time). Old binding-13 references retained only
>     in revision history. (Note: v2r16 subsequently flipped this to
>     binding 15 — see the v2r16 entry above for the diagnostic-
>     readback conflict at slot 14 that prompted the second flip.)
>   * **out3-MAJ-1 (§7 + §9):** v2r14's `IsCoalesceEnabled()` checked
>     state-machine flags + extension + parity, but not the actual GL
>     handles (`s_staticPropProgramCoalesce`, `s_coalesceInstanceSsbo`,
>     `s_perDrawSsbo`). A coalesce program link failure leaves the flag
>     true but the handle 0 → `glUseProgram(0)` at draw time. v2r15
>     strengthens the predicate.
>   * **out3-MAJ-2 (§3 + §9 + §11):** v2r14 had no save/restore contract
>     for the new SSBO bindings (4 PerDraw, 14 Permutation). Existing
>     `flush()` save/restore at `:1422–1425, 1740–1743` covers slots 0–3
>     only. v2r15 adds the binding-hygiene invariant.
>   * **out3-MAJ-A (NEW, §3.X + §11.7):** outside reviewer's second pass
>     surfaced that the existing `flush()` prologue at `:1430–1474` binds
>     SSBO slot 2 to `s_perTypeSsbo` (per-type hot-color data consumed by
>     `static_prop.vert`'s `get_base_light()`). v2r14 was silent on
>     whether the coalesce branch in §11.7 inherits this prologue or
>     replaces it. v2r15 makes the inheritance explicit: the legacy
>     prologue (lines 1430–1474, including slot 1 colors and slot 2
>     per-type) runs UNCONDITIONALLY; the coalesce branch only adds
>     slot 0 / slot 4 / slot 15 binds on top. (Slot number reflects
>     the v2r16-current binding; v2r15 used slot 14 here.)
>   * **out3-MAJ-B (NEW, §3.Z):** v2r14 scattered the three-way
>     "group-relative addressing" invariant across §5.1b (CPU writes),
>     §5.3a (`u_drawIDBase` shift), §5.5 (per-group `baseInstance` in
>     `compute_buildIndirectBuffer()`), and §5.7 (`gl_BaseInstanceARB +
>     gl_InstanceID`). Each subsection is correct, but no single section
>     states the coupling. v2r15 adds §3.Z consolidating the invariant
>     so silent drift between any two of the three pivots is caught
>     immediately.
>   * **out3-MIN-C (NEW, §11 step 4):** v2r14's `event=ready` log line
>     was specified to fire "at first armed flush" via a process-static
>     `s_coalesceFirstFlushDone`. Per-process means it fires once per
>     run regardless of mission count — surprising for a tier1 5-mission
>     log. v2r15 specifies per-mission reset (`s_coalesceFirstFlushDone
>     = false` in `onMapLoad()`).
>   * **out3-MIN-D (NEW, §7):** v2r14 referenced `coalesce_resetEnvOnce()`
>     in §7's `IsCoalesceEnabled()` but never defined it. The env name
>     `MC2_SUBSTRATE_COALESCE_LEGACY` (used in §11 / §13 kill-switch
>     guidance) was likewise unwired to `s_coalesceEnvDisabled`. v2r15
>     §7 includes the explicit helper definition.
>   * **out3-MIN-E (NEW, §6):** `flushShadow()` is an empty stub today
>     (`gos_static_prop_batcher.cpp:1794–1796`); when it is filled by a
>     future slice (Task 13 marker), implementer must NOT auto-adopt the
>     coalesce program. v2r15 §6 adds an explicit forward-compat note.
>   * **out3-MIN-F (NEW, rollback):** v2r15 codifies the rollback
>     contract — any coalesce-build failure rolls back ITS resources
>     (pins, texture arrays, partial SSBOs) but never touches legacy
>     state. Generalizes the per-group rollback from §5.4 to all
>     failure paths.
> - v2r1: initial rewrite after v1 adversarial review.
> - v2r2: per-type granularity (v1-CRIT-1), parity guard function
>   (v1-CRIT-2), `s_instanceSsbo` lifecycle (v1-CRIT-3), txmmgr citation
>   (v1-MIN-1).
> - v2r3: ring-frame offset (r2-CRIT-A); `GL_BGRA` + eviction invariant
>   (r2-CRIT-B); slot-level `textureAlpha` (r2-CRIT-C); `gl_DrawIDARB`
>   vert→frag passthrough (r2-CRIT-E); two-program restructure (r2-MAJ-A);
>   substrate-OFF baseline canary (r2-MAJ-B); same-size assertion
>   (r2-MAJ-C); batcher exports enumerated (r2-MAJ-D); permutation SSBO
>   at binding 13 (r2-MAJ-E); `MIN_PER_TYPE_CAP` named.
> - v2r4 (2026-05-09): real eviction-pin API + correct field path
>   (r3-CRIT-1); removed fictional `isInitComplete()` assert (r3-CRIT-2);
>   `IsCoalesceEnabled()` early-out on `!s_geometryFinalized` (r3-CRIT-3);
>   `gl_DrawIDARB` consistency (r3-MAJ-1); `visibleIds[]` not used by
>   `static_prop.vert` clarified (r3-MAJ-2); removed unused
>   `batcher_getCoalesceFrameSlot()` (r3-MAJ-3); log-event table (r3-MIN-1).
> - v2r5 (2026-05-09): removed stale `touch()` call at §5.4 step 1
>   (r4-CRIT-1); switched to refcount-aware `pinNode()`/`unpinNode()`
>   per-mission and dropped static memo from `IsCoalesceEnabled()`
>   (r4-CRIT-2 + r4-MAJ-1); added §5.1b explicit per-frame CPU write
>   path into `s_coalesceInstanceSsbo` with fence-array decision
>   (r4-MAJ-2); added §5.1c `submit()`/`uploadAllBucketsIfNeeded`
>   interaction (r4-MAJ-3); fixed prefix concatenation pseudo-code
>   (r4-MIN-1); split per-stage shader prefixes (r4-MIN-2); explicit
>   pin-continuity note for PAUSE/UNPAUSE (r4-MIN-3).
> - v2r6 (2026-05-09): renamed `coalesceByteOffsetWithinFrame` →
>   `coalesceByteOffsetWithinGroup` and added group base in write loop
>   (r5-MAJ-1 — was a real data-corruption bug for alpha-ON instances);
>   confirmed `mcTextureNodeIndex` is set at register time via
>   `SetTextureHandle()` at `tgl.cpp:1551` (r5-MAJ-2 verified safe);
>   draw issue site explicit in §5.1b pseudo-code (r5-MIN-1);
>   re-arm buffer-freshness note (r5-MIN-2); partial-build GL cleanup
>   on size mismatch (r5-MIN-3); `finalizeGeometry()` wall-clock
>   measurement in parity gate (r5-MIN-4).
> - v2r7 (2026-05-09): added §5.3a per-group `u_drawIDBase` uniform —
>   `gl_DrawIDARB` resets per multi-draw call, so alpha-ON draws read
>   `perDraw_.entries[v_drawID + u_drawIDBase]` with `u_drawIDBase =
>   s_alphaOffCount` (r6-CRIT-1 — was a correctness bug for ALL
>   alpha-tested static props); removed dead
>   `batcher_getOffGroupTotalBytes()` export (r6-MAJ-1); added notes for
>   `s_bucketsByType` clear point (r6-MIN-1), multi-texture pin scope
>   (r6-MIN-3); confirmed `coalesceByteOffsetWithinFrame` no live
>   references (r6-MIN-4).
> - v2r8 (2026-05-09): editorial — fixed §5.1b stale reference to removed
>   `batcher_getOffGroupTotalBytes()` (r7-MIN-1); rewrote §5.3a code
>   blocks to use the actual `uniform int` + `glUniform1i` form (r7-MIN-2;
>   prose was correct but blocks showed `uniform uint` / `glUniform1ui`
>   which crashes shader_builder); added `s_locDrawIDBase` to §9 inventory
>   (r7-MIN-3).
> - v2r14 (2026-05-09): outside reviewer's second pass on v2r13 returned
>   PROCEED with 3 editorial cleanups; all fixed:
>   * out2-MIN-1: stale "per-type high-water tracker refines it" language
>     in §5.1 (and MINOR-H) — removed; cross-mission warm-start was
>     dropped in v2r10 per r9-MAJ-2 (ABA-vulnerable `TG_TypeShape*` key).
>   * out2-MIN-2: explicit implementation watchpoint added to §6 + §7 +
>     §11 — when `s_hasShaderDrawParams=false`, the early-return MUST
>     still allocate+bind the identity `s_permutationSsbo` before any
>     subsequent patch dispatch, or the patch shader reads an unbound
>     binding 13 and corrupts the legacy fallback.
>   * out2-MIN-3: stale "rev 4" status header — updated to "v2r14,
>     implementation-ready."
>
> - v2r13 (2026-05-09): round 12 (sonnet) returned PROCEED with 2 minors;
>   both fixed:
>   * r12-MIN-1: secondary illustrative `baseInstance` snippet used
>     `s_types[typeID].instanceCap` (file-scope, not exported) where
>     `gpu_cull_compute.cpp` can't see it; switched to
>     `batcher_getInstanceCap(typeID)` to match the authoritative §5.5
>     two-branch pseudo-code block.
>   * r12-MIN-2: `s_coalesceInstanceMap` (persistent-mapped write pointer
>     into `s_coalesceInstanceSsbo`) was missing from §9 inventory; added.
>
> **Ship-readiness reached:** rounds 11 (opus) and 12 (sonnet) both
> return 0 CRIT + 0 MAJ. The spec is implementation-ready as of v2r13.
>
> - v2r12 (2026-05-09): round 11 (opus) returned PROCEED but flagged 3
>   MIN editorial issues + 2 latent grep-verifications. v2r12 addresses
>   all five:
>   * r11-MIN-1: §5.6 "RE-CALLED" wording was misleading (implied a
>     second call). Rephrased to match r10-MAJ-1 single-call ownership.
>   * r11-MIN-2: added explicit two-branch pseudo-code for the refactored
>     `compute_buildIndirectBuffer()` body (sorted vs natural layout)
>     showing the distinct `baseInstance` semantics each branch needs.
>   * r11-MIN-3: removed the §5.6 end-of-function branch description
>     contradicting §5.5's early-return; §5.5 is authoritative.
>   * r11 latent-1 verified: legacy `baseInstance = cumBase` per
>     `gpu_cull_compute.cpp:564` confirmed; v2r12 pseudo-code preserves it.
>   * r11 latent-2 verified: `flush()` and `s_offGroupTotalBytes` both
>     live in `gos_static_prop_batcher.cpp` (same TU); direct file-scope
>     access confirmed valid.
> - v2r11 (2026-05-09): round 10 (sonnet) caught 1 MAJ + 3 MIN in v2r10:
>   * **r10-MAJ-1** v2r10 added `compute_buildIndirectBuffer()` calls
>     INSIDE `finalizeGeometry()` but the existing call site at
>     `mission.cpp:3094` was unaddressed → double-build with redundant GPU
>     buffer reallocation. Restructured: `finalizeGeometry()` does NOT
>     call `compute_buildIndirectBuffer()`; the existing post-finalize
>     call at `mission.cpp:3094` is the single call site, and it reads
>     `s_coalesceLayoutReady` to decide sorted vs natural layout.
>   * r10-MIN-1: option (a) text in §5.6 still described pre-r9 design
>     (`onMapLoad()` + `MAX_TYPES`) before the correction paragraph;
>     rewritten to match r9-MAJ-1.
>   * r10-MIN-2: §5.6 lifecycle prose said "created at `onMapLoad()`"
>     contradicting §9 table; corrected to "created inside `finalizeGeometry()`."
>   * r10-MIN-3: pseudo-code `compute_buildIndirectBuffer()` calls were
>     missing the `typeCount` argument required by its actual signature
>     at `gpu_cull_compute.h:53`.
> - v2r10 (2026-05-09): round 9 (opus) caught 2 MAJ + 1 MIN in v2r9's
>   fixes:
>   * `MAX_TYPES` was a fictional constant invoked 8x in pseudo-code
>     (r9-MAJ-1). Replaced with deferred allocation: `s_permutationSsbo`
>     is now allocated INSIDE `finalizeGeometry()` after `s_types.size()`
>     is known, sized exactly to that count. The patch shader doesn't
>     dispatch before `finalizeGeometry()` runs, so identity-at-onMapLoad
>     was unnecessary in the first place.
>   * `s_typeHighWater` keyed by raw `TG_TypeShape*` was ABA-vulnerable
>     (r9-MAJ-2 — `msl.cpp:373` does `delete listOfTypeShapes[i]` so heap
>     addresses can be reused). Removed cross-mission high-water tracking
>     entirely; use the global-divide estimate always. Type-overflow
>     runtime disarm is the safety net for under-estimation. Loss of
>     warm-start tuning is acceptable (first mission of every session
>     would have been cold-start anyway).
>   * Made the permutation SSBO overwrite the LAST step of build sequence
>     (r9-MIN-1) — if texture-array build fails AFTER permutation
>     overwrite, the legacy patch path would write to wrong slots.
>     Reordering eliminates the need for explicit rollback.
> - v2r9 (2026-05-09): outside review caught 5 CRIT + 5 MAJ + 3 MIN that
>   internal sonnet+opus rounds 7-8 missed:
>   * `gl_BaseInstance` → `gl_BaseInstanceARB` everywhere (out-CRIT-1;
>     same `*ARB` rule as `gl_DrawID` — under `#version 430` + extension,
>     the extension-suffixed name is required; unsuffixed is 4.6 core only).
>   * Patch shader needs identity-permutation fallback OR uniform branch
>     for the legacy/disarm path (out-CRIT-2 — coalesce-disarm at runtime
>     with sorted `cmds[]` writes would corrupt the legacy draw loop).
>   * Decoupled `s_coalesceLayoutReady` from `s_coalesceEnabled` so
>     `compute_buildIndirectBuffer()` build-mode is deterministic (out-CRIT-3).
>   * Made the §5.1b CPU-write overflow check authoritative on
>     `bucket.instances.size()`, not on patched `instanceCount` (out-CRIT-4
>     — prose was wrong; pseudo-code was right; conflict resolved).
>   * Empty-group handling: skip texture-array build, bind, draw entirely
>     when `s_alphaOffCount == 0` or `s_alphaOnCount == 0` (out-CRIT-5).
>   * §6 frag snippet updated to `perDraw_.entries[v_drawID + uint(u_drawIDBase)]`
>     (out-MAJ-1 — stale snippet still showed pre-r6-CRIT-1 form).
>   * Coalesce fence cleanup made disarm-independent (out-MAJ-2).
>   * Texture-pin transactional rollback on aborted build (out-MAJ-3).
>   * High-water tracker keyed by stable type identity (shape pointer +
>     first-packet texture node), not by transient type index (out-MAJ-4).
>   * Coalesce sampler `u_texArr` uniform setup specified (out-MAJ-5).
>   * State-machine renaming + clarifying notes for legacy patch path
>     and zero-group test (out-MIN-1..3).
>
> **Worktree:** `claude/nifty-mendeleev`. All cited line numbers verified
> at spec-write time (2026-05-08) against HEAD.

---

## 1. Problem statement

`GpuStaticPropBatcher::flush()` (`gos_static_prop_batcher.cpp:1542`) issues
one `glDrawElementsIndirect` per type per packet. The inner packet loop
(`:1652`) re-issues the same type-level draw command (`typeID * 20` byte
offset) for every packet of that type — a redundant overdraw for multi-packet
types. With 323 types and most having 1 packet, this produces ~323 separate
`glDrawElementsIndirect` calls per frame when `MC2_GPU_CULL_SUBSTRATE=1`.

GPU-pipeline serialization from 323 separate indirect draws raises
`Render.GpuStaticProps` from ~120 µs (legacy CPU path) to ~2 ms. This blocks
substrate from being default-on.

**Fix:** Replace the per-type draw loop with 2 `glMultiDrawElementsIndirect`
calls — one per alpha-test state group. Inner packet loop is eliminated
(one draw per type with the type's first-packet texture).

Expected: ≥90% zone reduction (≤200 µs at mc2_01 normal zoom).

---

## 2. Stage 0 recon findings (confirmed / corrected)

### 2.1 Draw granularity: one command per TYPE, not per packet

The indirect command buffer built by `compute_buildIndirectBuffer()` has
**one `DrawCmd` per type**:

- `gpu_cull_compute.cpp:543`: `std::vector<DrawCmd> cmds(typeCount);`
- `gpu_cull_compute.cpp:548-568`: one entry per type, filled by
  `batcher_getTypeDrawInfo(t, ...)`.
- `batcher_getTypeDrawInfo` at `:1963-2001`: sums all packets' indexCount into
  one `totalIndexCount`; comment at `:1973-1977` confirms "single draw command
  per type covering ALL packets as one contiguous draw." Packets are contiguous
  in the IBO (guaranteed by `registerType()`'s sequential append).
- `flush()` at `:1703`: `cmdOffset = typeID * 20` — per-type offset, same for
  every packet in the inner loop.

**Two paths exist today, with DIFFERENT correctness:**

- **C1b indirect path** (`useC1bIndirect=true` at `:1691`): issues
  `glDrawElementsIndirect(typeID * 20)` once per packet, all using the same
  type-level command (combined geometry). For multi-packet types this means N
  redundant draws; with `GL_LEQUAL` depth test, the last packet's texture wins.
  Pre-existing overdraw bug.
- **Non-C1b path** (`useC1bIndirect=false` at `:1707`): issues
  `glDrawElementsInstancedBaseVertex(... pkt.indexCount, ... pkt.firstIndex,
  ... pkt.baseVertex)` per packet — per-packet correct rendering with each
  packet's own texture.

V2 coalesce REPLACES only the C1b indirect path. The non-C1b path is unchanged.

**V2 behavior:** one draw per type (combined geometry) with the FIRST
packet's texture. For single-packet types: identical to today's correct path
(majority case). For multi-packet types: replaces "last wins overdraw" with
"first wins single-draw" — equivalent draw correctness, slightly different
texture choice (deterministic vs depth-race-dependent), and fewer overdraws.

The substrate-default-on flip is a SEPARATE slice; that flip's parity gate
must compare against the non-C1b baseline. See §11.

### 2.2 Line-number corrections (v1 design drift)

| Symbol | v1 cited | **Actual (2026-05-08)** |
|---|---|---|
| `DrawCmd` struct | `gpu_cull_compute.cpp:507–514` (file-scope) | **`:534–541`** (inside function) |
| Indirect buffer alloc | `:551–555` | **`:578–582`** |
| Patch dispatch | `:798/:805` | **`:828–835`** |
| Post-patch barrier | `:813` | **`:843`** |
| `cumBase` init | `:521` | **`:548`** |
| `cumBase += instanceCap` | `:541` | **`:568`** |
| PR1 `glMultiDrawArraysIndirect` | `:2410` / `:2423` | **`gameos_graphics.cpp:2468`** |
| AMD attr-0 banner | `:2210–2213` | **`:2262–2271`** |
| `batcher_getTypeDrawInfo` | `:1954` | **`:1963`** |
| Per-type count sum | `:1973–1980` | **`:1979–1988`** |
| Anticipatory coalesce comment | `txmmgr.cpp:1525` | **`txmmgr.cpp:1761`** |

### 2.3 SSBO offset invariant broken (v1 MAJOR-1)

Two independent cumulative pointers compute per-type SSBO byte offsets:

- `r.instanceByteOffset` (`:1290`): per-frame compacted layout, byte units,
  alignment-padded.
- `cmds[t].baseInstance` (`:548,568`): mission-static, instance-count units,
  global-pool capacity (`s_instanceCapacity`) for every type.

Structurally incompatible. V2 introduces a separate `s_coalesceInstanceSsbo`
with capacity-based layout (§5.1).

### 2.4 Late-registration alpha-class invariant: verified safe

`registerType()` (`:544`) checks `s_geometryFinalized` and rejects post-finalize
calls. Type set is locked before `finalizeGeometry()`. The "late registration"
in the code refers to per-frame INSTANCE submission skips, not type registration.

### 2.5 Ring-buffer mechanism (load-bearing for `glBindBufferRange`)

`gos_static_prop_batcher.cpp:62`: `RING_FRAMES = 3`. `:1266`:
```cpp
slotInstByteBase = s_frameSlot * s_instanceCapacity * sizeof(GpuStaticPropInstance);
```
The CPU writes the current frame's data to the slot at byte offset
`slotInstByteBase`. The GPU reads from the same offset (after a fence
synchronization). V2's `glBindBufferRange` calls MUST include this frame
offset. See §5.1 and §5.5.

### 2.6 `INITIAL_INSTANCES_PER_FRAME = 4096`

Confirmed at `:68`. Used as the floor for per-type capacity in §5.1.

### 2.7 Texture data format: BGRA on this engine

Per `memory/mc2_argb_packing.md` and the cement texture-array precedent at
`gos_terrain_indirect.cpp:1735-1755` (which uses `GL_BGRA, GL_UNSIGNED_BYTE`
for both `glGetTexImage` and `glTexSubImage3D`). MC2 textures stored on GPU
are BGRA byte order. V2 must use the same transfer format. See §5.4.

### 2.8 `pkt.materialFlags` does NOT capture per-slot `textureAlpha`

`registerType()` at `:640` sets `pkt.materialFlags` from
`typeShape->alphaTestOn` (shape-level `SetAlphaTest`). The per-slot
`textureAlpha` is set independently by `bdactor.cpp` during actor init
(after `registerType` returns). Comment at `:638-639` confirms:
"alphaTestOn captures shape-level alpha test (trees, via SetAlphaTest);
textureAlpha per-slot is resolved at draw time (after bdactor.cpp init
completes)."

`flush()` at `:1676-1680` re-checks `src->listOfTextures[slot].textureAlpha`
and OR's it into `effectiveMaterialFlags`. V2 must do the same at
`finalizeGeometry()` time — see §5.2.

---

## 3. Locked architectural decision

**2 `glBindBufferRange` + 2 `glMultiDrawElementsIndirect`**, one pair per
alpha-test state group. Per-type command granularity. Inner packet loop
eliminated.

Texture: one `GL_TEXTURE_2D_ARRAY` per group, layer = type's first packet
texture. Per-draw data (`packetID`, `materialFlags`, `texArrayLayer`,
`maxLocalVertexID`) in a per-draw SSBO indexed by `gl_DrawIDARB`.

`gl_BaseInstanceARB` and `gl_DrawIDARB` (both vertex-stage built-ins; the
latter is passed to fragment via `flat varying`) require
`GL_ARB_shader_draw_parameters`. In `#version 430` + `#extension` mode the
correct identifiers are the **`*ARB`-suffixed** names — both
`gl_DrawIDARB` and `gl_BaseInstanceARB`. The unsuffixed `gl_DrawID` /
`gl_BaseInstance` are the GL 4.6 core promotion names and are NOT defined
under `#version 430` + extension. (Out-CRIT-1: the same naming rule
applies to BOTH built-ins; v2r1..r8 inconsistently used `gl_BaseInstance`.)
See §6.

```
Per-frame setup:
  fr_off = s_frameSlot * per_frame_total_instance_bytes

Alpha-OFF group bind+draw (skip entire block if s_alphaOffCount == 0,
out-CRIT-5):
  glUniform1i(s_locDrawIDBase, 0)              // §5.3a: gl_DrawIDARB offset
  glBindBufferRange(0, s_coalesceInstanceSsbo,
                    fr_off + 0,                  // group_off = 0
                    off_total_bytes)
  glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff)
  glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                              (void*)(0),         // s_alphaOffCmdBufOffset = 0
                              s_alphaOffCount, 0)

Alpha-ON group bind+draw (skip entire block if s_alphaOnCount == 0,
out-CRIT-5):
  glUniform1i(s_locDrawIDBase, s_alphaOffCount) // §5.3a
  glBindBufferRange(0, s_coalesceInstanceSsbo,
                    fr_off + off_total_bytes,    // group_off = off_total
                    on_total_bytes)
  glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn)
  glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                              (void*)(s_alphaOffCount * 20),
                              s_alphaOnCount, 0)
```

---

### 3.X SSBO binding hygiene (out3-MAJ-2 invariant)

The existing `flush()` save/restore envelope at `gos_static_prop_batcher
.cpp:1422–1425, 1740–1743` covers SSBO bindings 0–3 only. v2 adds two
new bindings:

| Slot | Buffer | Read by |
|---|---|---|
| 4 | `s_perDrawSsbo` | coalesce frag (§5.3) |
| 15 | `s_permutationSsbo` | patch comp (§5.6) |

**Invariant:** the coalesce path MUST save the prior binding for each
new slot it touches before binding, and restore it before `flush()`
returns. This extends the existing 0–3 contract to 0–4 and 15.
Pseudo-code in §11.7:

```cpp
GLint prevSsbo4 = 0, prevSsbo15 = 0;
glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 4,  &prevSsbo4);
glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 15, &prevSsbo15);
// ... coalesce binds + draws ...
glBindBufferBase(GL_SHADER_STORAGE_BUFFER,  4, prevSsbo4);
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, prevSsbo15);
```

The existing `GL_DRAW_INDIRECT_BUFFER` unbind after multi-draw (Step
11.7.f / `:1706` precedent) is part of this contract — the coalesce
path must leave the indirect-buffer binding 0 on exit.

**Why this matters:** the patch path explicitly binds slot 15 (the
patch shader's permutation SSBO) before its compute dispatch, so a
leak of slot 15 out of `flush()` self-heals on the next frame's patch
dispatch. But binding 4 is owned only by the coalesce frag — leaking
it out of `flush()` exposes whatever passes run next (post-process,
shadow re-issue, particle systems) to a stale per-draw SSBO that they
neither expect nor declared. Conservative answer: restore both
explicitly.

### 3.X.1 Legacy prologue inheritance + slot 1 NOT bound (out3-MAJ-A + out4-CRIT-1, refined out5-CRIT-1 in v2r18)

The existing `flush()` prologue at `gos_static_prop_batcher.cpp:1430–1474`
binds:

- **Slot 2 only:** `s_perTypeSsbo` at `:1472–1474` (per-type hot-color
  data — `hotPinkRGB`, `hotYellowRGB`, `hotGreenRGB` — read by
  `get_base_light()` in `static_prop.vert`).

**Slot 1 is NOT in the prologue.** Out4-CRIT-1 found that v2r15's wording
("slot 1 colors and slot 2 per-type") was wrong: slot 1 (`s_colorSsbo`)
is bound at `:1554–1559` INSIDE the legacy per-type loop with
`glBindBufferRange` and a per-type-relative offset. The coalesce branch
skips that loop entirely, so the legacy per-type bind never happens
when coalesce is armed.

**Production reality (verified 2026-05-09 v2r18 grep pass):** `static_prop.vert`
declares `colors_` at binding 1 (line 56) but `colors_.c[...]` does NOT appear
anywhere in the live shader body. ALL debug modes 0–7 (`static_prop.frag:55–80`)
read `v_argb.rgb` (per-vertex lit color computed in the vert shader from
per-instance data) — including mode 4 at `:73`, which v2r15/v2r16/v2r17 had
mistakenly called out as a `colors_.c[...]` consumer. Mode 4 is `FragColor =
vec4(v_argb.rgb, 1.0)` — pure varying read, no SSBO 1 access. The legacy code
at `:1557–1559` still calls `glBindBufferRange(SLOT 1, s_colorSsbo, ...)`
per-type but no shader actually consumes the bound data; this is dead state
on the legacy side too.

**Invariant — v2r18 correction:** the coalesce branch in §11.7 MUST:
1. **Inherit slot 2** from the existing prologue (which runs unconditionally
   per §5.0's ordering). This part is unchanged from v2r15+.
2. **NOT bind slot 1.** v2r15..v2r17 mandated an explicit `glBindBufferBase(1,
   s_colorSsbo)` whole-buffer bind for "load-bearing debug-mode-4 + future-shader
   safety." Both rationales were based on misreadings:
   - Mode 4 reads `v_argb`, not `colors_` (verified frag:73).
   - "Future shader edit" is YAGNI — if/when a future slice introduces
     `colors_.c[...]` reads, that slice owns either (a) converting
     `firstColorOffset` from per-type-bucket-relative to absolute ring-frame
     offset for coalesce, or (b) forcing that read path back to legacy.
   The whole-buffer bind we previously prescribed would have indexed the
   wrong frame slot AND wrong type-bucket base anyway, so it was dead-and-
   misleading code rather than dead-but-defensive.

If a future slice reintroduces `colors_.c[...]` reads, it MUST decide
explicitly: convert `firstColorOffset` to absolute, OR force the consumer
back to legacy. v2r18 makes no provision for either.

(Plan v3.1's `forceLegacyForColorDebug = (debugAddrMode_ == 4)` predicate
and Step 17.5b canary, both downstream of v2r17's mistaken slot-1
rationale, are also reverted in plan v3.2.)

If a future revision restructures `flush()` so the coalesce branch
bypasses the prologue, the per-type hot-color SSBO at slot 2 carries
whatever the prior pass bound — `static_prop.vert`'s `get_base_light()`
then reads garbage hot-color magic and props render with random tints.
This is silent (no GL error, no shader compile failure).

### 3.Y `glBindBufferRange` offset alignment (out3-MAJ-2 sub-invariant)

`glBindBufferRange` requires `offset` divisible by
`GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT` (16 on AMD; up to 256 on
some platforms). `sizeof(GpuStaticPropInstance) == 112` is 16-aligned;
per-type capacities are uint counts, so per-type byte offsets stay
16-aligned today. But the spec must NOT assume — a future cap math
change (padding for new fields, struct size drift) would silently
break with `GL_INVALID_VALUE` at bind time. Required guards:

```cpp
GLint align = 0;
glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
MC2_ASSERT(align > 0 && (align & (align - 1)) == 0);
// At finalize:
MC2_ASSERT((sizeof(GpuStaticPropInstance) % align) == 0);
MC2_ASSERT((s_offGroupTotalBytes % align) == 0);
// At bind time, if the type's offset can drift due to runtime patches:
MC2_ASSERT((fr_off_bytes % align) == 0);
MC2_ASSERT((type.coalesceByteOffsetWithinGroup % align) == 0);
```

If `sizeof(GpuStaticPropInstance)` ever stops being a multiple of
`align`, pad the struct or round per-type capacities up to the
alignment boundary in instance units.

### 3.Z Group-relative addressing invariant (out3-MAJ-B)

The coalesce path has **three coupled pivots** that must agree on what
"group-relative" means. Drift between any two silently mis-renders.
v2r15 consolidates them here so the invariant lives in one place:

| Pivot | Set by | Read by | Semantics |
|---|---|---|---|
| `cmds[i].baseInstance` (sorted branch) | `compute_buildIndirectBuffer()` per §5.5 | vertex shader as `gl_BaseInstanceARB` | byte-offset / `sizeof(Instance)` from the start of the GROUP slice — accumulator resets to 0 for alpha-OFF and again for alpha-ON |
| `glBindBufferRange(0, s_coalesceInstanceSsbo, group_off, group_size)` | C++ in `flush()` per §3 / §11.7 | vertex shader (slot 0 = `instances_`) | bind window starts at `fr_off + group_off`; gl_BaseInstanceARB is RELATIVE to that window's base |
| `u_drawIDBase` | C++ before each multi-draw per §5.3a / §11.7 | fragment shader for `perDraw_.entries[v_drawID + uint(u_drawIDBase)]` | offset into the SORTED-order PerDraw SSBO; 0 for alpha-OFF, `s_alphaOffCount` for alpha-ON |

**Invariant:** for alpha-OFF group, all three pivots accumulate from 0;
for alpha-ON group, all three reset their group-relative origin so the
shader's reads land in the alpha-ON region of each buffer.

The PerDraw SSBO entries themselves are **layered group-relative**:
`PerDrawEntry.texArrayLayer` indexes into `s_texArrayOff` for alpha-OFF
entries and `s_texArrayOn` for alpha-ON entries. The shader binds the
correct array via the per-group `glBindTexture(GL_TEXTURE_2D_ARRAY, ...)`
call in §3 / §11.7 — texArrayLayer never crosses groups.

**Verification at finalize.** After `compute_buildIndirectBuffer()`'s
sorted branch builds, the implementation should `MC2_ASSERT` that:
1. The first alpha-ON `cmds[i].baseInstance` equals 0 (group reset).
2. The last alpha-OFF `cmds[i].baseInstance + cmds[i].instanceCap`
   equals `s_offGroupTotalBytes / sizeof(Instance)`.
3. `s_alphaOffCount` matches the number of `cmds[i]` with `i < N_off`
   (sanity: sort consistent with classification).

These are cheap startup-time asserts; they make Step 5.5 / 5.7 / 11.7
drift detectable.

---

## 4. CRITICAL resolutions

### CRITICAL-1 — Draw granularity (per-type)
Resolved §2.1, §3, §5.5. Per-type commands; inner packet loop eliminated.

### CRITICAL-2 — `u_packetID` debug-mode hash
Resolved §5.3. `packetID = type.firstPacket` lives in `PerDrawSsbo`; fragment
shader reads via `flat in v_drawID`. Debug-mode 2 hash preserved per type.

### CRITICAL-3 — `ARB_shader_draw_parameters` adoption
User explicitly approved 2026-05-08. See §6 for probe + fallback.

### CRITICAL-4 — `glGetBufferSubData` per-frame stall
Eliminated from production. Dev-only validation via
`MC2_SUBSTRATE_COALESCE_VALIDATE=1` (one-shot at first flush, uses C2 readback
ring; never per-frame). **Deferred to a future "coalesce validation hook"
slice (v2r20)** — not implemented in this slice; gates that previously
referenced this env var (§11 step 8 sub-step, step 11, step 12) have been
reframed as inspection-only against the gated `[COALESCE v1]
event=permutation_state` log line surfaced by plan Step group 12B.

### CRITICAL-A — Ring-frame offset in `glBindBufferRange`

`s_coalesceInstanceSsbo` is a 3-frame ring (`RING_FRAMES=3`, §2.5). Every bind
must include `s_frameSlot * per_frame_total_instance_bytes`. See §3 diagram
and §5.1 layout table. CPU writes to `s_instanceMap + (s_frameSlot *
per_frame_bytes) + group_off + type_cap_off`; GPU reads the same byte through
`glBindBufferRange(0, buf, fr_off+group_off, group_size)` followed by
`gl_BaseInstanceARB + gl_InstanceID` indexing within the bound range.

### CRITICAL-B — `GL_BGRA` pixel transfer; texture-eviction invariant

§5.4 step 5 uses `glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE,
buf)` and `glTexSubImage3D(... GL_BGRA, GL_UNSIGNED_BYTE, ...)`. Internal
format remains `GL_RGBA8` (driver-side; pixel ordering is set by transfer
format). Matches precedent at `gos_terrain_indirect.cpp:1735-1755`. The frag
shader's existing `tex_color = texture(u_tex, v_uv)` already returns RGBA in
the engine's expected channel order — no shader change needed.

**Texture-eviction invariant:** `mcTextureManager->update()` performs eviction
on inactive textures (`memory/pause_unpause_diagnostic_for_static_render_bugs.md`).
If a static-prop's source texture is evicted mid-mission, the cached array
layer becomes stale (the source GL ID may be reassigned to different content).

V2 mitigation: at `finalizeGeometry()`, every type's first-packet texture is
pinned against eviction via the **refcount-aware** API at `txmmgr.h:1279`:
```cpp
DWORD nodeId = typeShape->listOfTextures[firstPkt.textureSlot].mcTextureNodeIndex;
mcTextureManager->pinNode(nodeId);
```
This increments `masterTextureNodes[nodeId].pinRefCount` (`txmmgr.h:139`).
The eviction path excludes nodes with `pinRefCount > 0`. Refcounted: multiple
systems can independently pin/unpin the same node without clobbering each
other (the registry already pins via the same API at
`gos_static_prop_registry.cpp:237`). Once-per-unique-texture, not per-frame.

The non-refcounted `setTextureNeverFlush(N, 0x1)` API (`txmmgr.h:568`) was
considered and rejected — it overwrites the `neverFLUSH` DWORD as a SET, which
means a paired unpin (`setTextureNeverFlush(N, 0)`) silently destroys any other
system's pin on the same node. `pinNode/unpinNode` is the codebase's
established convention.

**Transactional pin rollback (out-MAJ-3):** the §5.4 texture-array build
pins every type's first-packet node sequentially. If the build aborts
partway through (size mismatch, GL error), nodes already pinned would
otherwise stay pinned for the entire mission — defeating the
"fallback is low-impact" goal. Use a temporary buffer:

```cpp
std::vector<DWORD> newlyPinnedThisBuild;
for (each type in group) {
    DWORD nodeId = ...;
    mcTextureManager->pinNode(nodeId);
    newlyPinnedThisBuild.push_back(nodeId);
    // ... attempt size assertion + glTexSubImage3D ...
    if (failure) {
        // Roll back THIS build's pins:
        for (DWORD n : newlyPinnedThisBuild) mcTextureManager->unpinNode(n);
        // (Pins from prior successful builds in the OTHER group remain
        // until onMapUnload — they're already in s_coalescePinnedNodes.)
        return false;
    }
}
// Success — promote temp pins into the persistent tracker:
for (DWORD n : newlyPinnedThisBuild) s_coalescePinnedNodes.push_back(n);
```

On `onMapUnload()`, every pinned node is released and any outstanding
coalesce fences are deleted (out-MAJ-2 cleanup safety net):
```cpp
for (DWORD nodeId : s_coalescePinnedNodes) mcTextureManager->unpinNode(nodeId);
s_coalescePinnedNodes.clear();
for (uint32_t i = 0; i < RING_FRAMES; ++i) {
    if (s_coalesceFence[i]) {
        glDeleteSync(s_coalesceFence[i]);
        s_coalesceFence[i] = 0;
    }
}
```
`s_coalescePinnedNodes` (new `std::vector<DWORD>`) tracks the set of
nodes pinned by the coalesce path. If the registry has already pinned the
same node, the refcount goes from 2 → 1 (still pinned).

**Multi-texture types — only first-packet slot is pinned (r6-MIN-3):**
`registerMultiShape()` (`gos_static_prop_batcher.cpp:675`) calls
`SetTextureHandle()` for ALL `j < numTxms`, so every slot has its
`mcTextureNodeIndex` set. The coalesce path pins ONLY the first packet's
slot, because §2.1 establishes that the multi-draw issues one draw per
type using the first packet's texture only. Other-slot textures aren't
read by the coalesce render path; eviction of those slots does not affect
the coalesce array's pixel content. Consistent with §5.4 step 3
deduplication. The pin is held
**continuously** between `finalizeGeometry()` and `onMapUnload()` — no
mid-mission release path. Mid-mission events that may trigger
`mcTextureManager->update()` (PAUSE/UNPAUSE, menu open) cannot evict pinned
nodes, so the coalesce array's source pixels stay valid.

If a type's source texture is rebound to a different GL ID (e.g., LOD swap or
hot-reload), the coalesce path's array layer contains stale pixels — visual
parity is broken. Detect and disable: at each `flush()`, compare
`type.lastSeenGosHandle` against `src->listOfTextures[firstPkt.textureSlot]
.gosTextureHandle`. On mismatch, log and disable coalesce for the rest of
the mission. This is a per-flush O(N_types) check; types is ~323; integer
compare; negligible cost.

### CRITICAL-C — Alpha-class sort must include slot-level `textureAlpha`

§2.8 confirms `pkt.materialFlags` is incomplete. Fence/gate types may have
`alphaTestOn=false` but `textureAlpha=true` per slot, leaving them in the
alpha-OFF group with the wrong texture array.

**Resolution — explicit ordering invariant + first-flush re-validation:**

`finalizeGeometry()` MUST run AFTER all `bdactor.cpp` init paths that call
`SetTextureAlpha` for static-prop types. Confirmed in `mission.cpp`: the call
order is (a) `objectManager->init()` which spawns actors and runs bdactor init
(setting `textureAlpha` per slot); (b) `finalizeGeometry()` at mission load
end (`mission.cpp:3088`); (c) first `flush()` at `:3094+`. Any future change
that moves `finalizeGeometry()` earlier breaks this.

Document the invariant in a comment block at the top of `finalizeGeometry()`:
```cpp
// INVARIANT: bdactor.cpp init must complete before this runs. Static-prop
// alpha-class classification reads `src->listOfTextures[s].textureAlpha`
// which bdactor sets after registerType. Reorder hazard: see CRITICAL-C
// of docs/superpowers/specs/2026-05-08-substrate-coalesce-design-v2.md.
// First-flush re-validation (below) is the runtime safety net.
```

The first-flush re-validation (next paragraph) is the actual runtime guard;
no fictional `isInitComplete()` API needed. If bdactor init somehow ran
incomplete, the re-walk catches the drift and disables coalesce.

`finalizeGeometry()` alpha-class determination (replaces v2r2 §5.2 step 1):

```cpp
for (uint32_t t = 0; t < s_types.size(); ++t) {
    const GpuStaticPropType& type = s_types[t];
    bool typeHasAlpha = false;
    for (uint32_t p = 0; p < type.packetCount; ++p) {
        const GpuStaticPropPacket& pkt = s_packets[type.firstPacket + p];
        if (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST) { typeHasAlpha = true; break; }
        if (type.source && type.source->listOfTextures &&
            pkt.textureSlot < type.source->numTextures &&
            type.source->listOfTextures[pkt.textureSlot].textureAlpha) {
            typeHasAlpha = true; break;
        }
    }
    type.alphaClass = typeHasAlpha ? 1 : 0;
}
```

**First-flush re-validation:** at the first coalesce flush, re-walk the
classification; if any type's resolved class differs from the one stored at
`finalizeGeometry()`, log `[COALESCE v1] event=alpha_class_drift type=N` and
disable coalesce for the rest of the mission. Belt-and-suspenders against
ordering regressions.

### CRITICAL-E — `gl_DrawID` is a vertex-stage built-in only

`GL_ARB_shader_draw_parameters` defines `gl_DrawID` (and `gl_DrawIDARB`) only
in the vertex stage. Fragment shader cannot read it directly.

**Resolution:** vertex shader passes the value through as a flat varying.

`static_prop.vert` additions:
```glsl
flat out uint v_drawID;
// ...
void main() {
    v_drawID = uint(gl_DrawIDARB);  // alias for gl_DrawID under ARB extension
    // ... existing body, with §5.7 instances_.i index change ...
}
```

`static_prop.frag` additions:
```glsl
flat in uint v_drawID;
// PerDrawSsbo lives in fragment stage; vertex stage doesn't need it.
struct PerDrawEntry { ... };
layout(std430, binding = 4) readonly buffer PerDrawData {
    PerDrawEntry entries[];
} perDraw_;
// Replace u_packetID / u_materialFlags / u_maxLocalVertexID reads:
//   perDraw_.entries[v_drawID].packetID etc.
```

This is the canonical pattern for `gl_DrawID` consumption in fragment shaders
under `GL_ARB_shader_draw_parameters` (the GL 4.6 core promotion lifts the
restriction; we are deliberately staying on the extension for compatibility).

---

## 5. Design details

### 5.0 Legacy finalize must always complete (out3-CRIT-1 invariant)

**Mandatory:** any coalesce-build failure path inside `finalizeGeometry()`
must NOT return before the existing legacy geometry-upload sequence has
completed. The legacy sequence at `gos_static_prop_batcher.cpp:709–795`
(shared `glGenVertexArrays` + VBO + IBO + per-type SSBO + legacy program
load + `s_geometryFinalized = true`) is load-bearing for both the legacy
draw path (RAlt+0 / kill-switch / parity / coalesce-disabled missions)
AND for `flush()`'s top-level guard at `:1320` (`if (!s_geometryFinalized
|| s_fatalRegistrationFailure) return;`). Any coalesce build failure
that returns before `:795` causes:

- `s_geometryFinalized = false` for the entire mission.
- `flush()` early-returns every frame; static props never draw.
- No legacy fallback — there is no legacy renderer to fall back TO.

V2r14 placed coalesce work BEFORE `loadProgramsIfNeeded()` at `:707` and
described early returns for mixed-alpha (§5.4 / §5.2), size-mismatch
(§5.10.c), and no-extension (§6) failures. That ordering causes total
static-prop blackout. **v2r15 corrects this:** coalesce build is a
side-attempt that runs alongside or after the legacy build; failures
disarm coalesce only.

**Ordering rule (authoritative — supersedes any contrary placement
language elsewhere in this spec):**

```
finalizeGeometry():
  if (s_geometryFinalized) return;                  // existing one-shot guard

  // ── Phase 1: legacy finalize (today's body, unchanged behavior) ──
  // Allocate s_sharedVao, s_sharedVbo, s_sharedIbo, s_perTypeSsbo;
  // upload geometry; loadProgramsIfNeeded() compiles legacy program.
  // No coalesce code in this phase. If legacy finalize itself fails,
  // it sets s_fatalRegistrationFailure (existing behavior) and returns.
  do_legacy_finalize_exactly_as_today();
  s_geometryFinalized = true;

  // ── Phase 2: coalesce side-attempt (v2 addition) ──
  // From this point on, every coalesce-build failure must DISARM
  // coalesce (clear all three state flags) but MUST NOT return before
  // the function ends. The legacy path is already valid; we are only
  // attempting to enable an optimization on top.
  coalesce_resetEnvOnce();   // out3-MIN-1 (v2r15 clarification): env
                             // decision MUST land before coalesce work
                             // begins, otherwise MC2_SUBSTRATE_COALESCE_LEGACY=1
                             // wastes finalize-time texture-array build.
  bool coalesceWanted = !s_coalesceEnvDisabled && s_hasShaderDrawParams;
  if (!coalesceWanted) {
      // No-extension or env-killed: still allocate identity permutation
      // (§5.6 / §6 watchpoint — patch shader binds slot 15 unconditionally).
      allocPermutationSsboAsIdentity(s_types.size());
      s_coalesceLayoutReady = false;
      s_coalesceEnabled     = false;
      s_coalesceArmed       = false;
      LOG("[COALESCE v1] event=disarmed reason=no_extension|env_killswitch");
      return;
  }

  // Always allocate identity permutation FIRST (legacy-safe state).
  allocPermutationSsboAsIdentity(s_types.size());

  // Run §5.2 alpha-class walk + mixed-alpha guard. On failure: log,
  // clear flags, RETURN (legacy is already done). DO NOT raise as a
  // legacy-side error.
  if (!classifyAlphaPerType()) {
      s_coalesceLayoutReady = false;
      s_coalesceEnabled     = false;
      s_coalesceArmed       = false;
      LOG("[COALESCE v1] event=disarmed reason=mixed_alpha type=N");
      return;
  }

  // Build sort + per-type caps + group totals. Set layout-ready.
  buildSortAndCaps();
  s_coalesceLayoutReady = true;

  // Allocate s_coalesceInstanceSsbo + map.
  if (!allocCoalesceInstanceSsbo()) {
      s_coalesceLayoutReady = false;       // disarm; legacy intact
      LOG("[COALESCE v1] event=disarmed reason=alloc_failed");
      return;
  }

  // Build per-group texture arrays (with same-size assertion +
  // transactional pin rollback per §5.4 + §CRITICAL-B). On failure:
  // disarm and return; legacy intact.
  if (!buildPerGroupTexArraysAndPerDrawSsbo()) {
      s_coalesceLayoutReady = false;
      // s_coalesceInstanceSsbo / s_coalesceInstanceMap stay allocated
      // — onMapUnload() will release them; mid-mission re-arm is not
      // supported (a single mission either ships armed or never).
      return;
  }

  // LAST step: overwrite identity permutation with sorted permutation.
  overwritePermutationSsboAsSorted();
  s_coalesceEnabled = true;
  s_coalesceArmed   = true;
  LOG("[COALESCE v1] event=armed types=N off=A on=B ...");
  // mission.cpp:3094's compute_buildIndirectBuffer() runs after return
  // and reads s_coalesceLayoutReady to pick sorted vs natural layout.
```

**State-flag invariant (out3-CRIT-1 corollary):** at every return from
`finalizeGeometry()` AFTER legacy finalize has completed successfully
(i.e. control reached past the existing `:795` `s_geometryFinalized =
true` write — does not apply to legacy-side fatal failures, which retain
their existing `s_fatalRegistrationFailure` semantics):
- `s_geometryFinalized == true` (legacy is always finalized in this branch).
- `s_permutationSsbo != 0` (identity OR sorted; patch shader reads
  binding 15 unconditionally — see §5.6).
- The three coalesce flags (`s_coalesceLayoutReady`, `s_coalesceEnabled`,
  `s_coalesceArmed`) are either all true (success) or all false (any
  failure path). No half-armed state.

**Rollback contract (out3-MIN-F):** any coalesce-build failure rolls
back ITS OWN resources without touching legacy state. Specifically:

- Pin operations are tracked in a temporary `newlyPinnedThisBuild`
  vector during the §5.4 texture-array build. On failure, all entries
  in that temp vector are `unpinNode`'d before return; the persistent
  `s_coalescePinnedNodes` (which tracks SUCCESSFULLY-PROMOTED pins
  from prior builds, e.g. when alpha-OFF group succeeded but alpha-ON
  fails) is NOT touched. Those persistent pins release at
  `onMapUnload()`.
- Texture arrays partially built (one group succeeded, the other
  failed) are deleted on the failure path: `glDeleteTextures(1,
  &s_texArrayOff)` and similarly for the other if it was created.
- Per-draw / instance / permutation SSBOs allocated before the failure
  point stay allocated; `onMapUnload()` releases them. Mid-mission
  re-arm is not supported (a single mission either ships armed at
  finalize time or never).
- Legacy state (`s_sharedVao`, `s_sharedVbo`, `s_sharedIbo`,
  `s_perTypeSsbo`, `s_staticPropProgram`, `s_geometryFinalized`) is
  NEVER touched by a coalesce-build rollback. The legacy renderer
  must be fully functional after any coalesce failure — that is the
  whole point of §5.0's "legacy first" ordering.

The implementation plan (Step group 5) MUST be written against this
ordering. Any §5.X subsection in this spec that contradicts §5.0 is
overridden by §5.0.

### 5.1 New SSBO: `s_coalesceInstanceSsbo`

Layout (mission-static, set at `finalizeGeometry()`):

```
Per-frame slot layout (× RING_FRAMES copies, total buffer = RING_FRAMES * per_frame_total):
  [alpha-OFF group: Σ(typeCap[t in offTypes]) instances]
  [alpha-ON  group: Σ(typeCap[t in onTypes])  instances]
                     ^^ "off_total_bytes" boundary
  per_frame_total = (off_total_count + on_total_count) * sizeof(GpuStaticPropInstance)
```

Storage: `glBufferStorage(... RING_FRAMES * per_frame_total, GL_MAP_WRITE_BIT
| GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT)` — same flags as the existing
`s_instanceSsbo` ring at `:267`.

Lifecycle: created at `finalizeGeometry()`, deleted at `onMapUnload()`.

**Per-type capacity:** new field `GpuStaticPropType::instanceCap` (added to
struct at `gos_static_prop_batcher.h:87-92`). Computed at `finalizeGeometry()`:

```cpp
constexpr uint32_t MIN_PER_TYPE_CAP = 32;  // small types still get a floor
const uint32_t globalCap = std::max<uint32_t>(s_instanceCapacity,
                                              INITIAL_INSTANCES_PER_FRAME);
type.instanceCap = std::max(MIN_PER_TYPE_CAP,
    (globalCap * 2u) / static_cast<uint32_t>(s_types.size()));
```

This estimate (2× average) over-provisions for hot types. The cross-mission
high-water tracker proposed in earlier revisions was REMOVED in v2r10 — see
the §5.1 "Per-type capacity" subsection below for the rationale (r9-MAJ-2:
ABA via `TG_TypeShape*` reuse).

**Per-type capacity (r9-MAJ-2 fix — cross-mission high-water removed):**

V2r9 proposed a cross-mission high-water tracker keyed by `TG_TypeShape*`.
That key is ABA-vulnerable: `msl.cpp:373` does `delete listOfTypeShapes[i]`
during `removeAppearance` (called from `bdactor.cpp:2840` and others), so
the same heap address can be returned to a different shape. Lookup with a
freed-and-reused pointer would seed the wrong type's cap. Content-derived
keys (texture slot + triangle/vertex counts + texture handle) were
considered but `gosTextureHandle` mutates per frame
(`mc2_texture_handle_is_live.md`), so any handle-based content hash is
also unstable.

Decision: drop cross-mission persistence entirely. Use the global-divide
estimate at every mission load:

```cpp
constexpr uint32_t MIN_PER_TYPE_CAP = 32;
const uint32_t globalCap = std::max<uint32_t>(s_instanceCapacity,
                                              INITIAL_INSTANCES_PER_FRAME);
type.instanceCap = std::max(MIN_PER_TYPE_CAP,
    (globalCap * 2u) / std::max<uint32_t>(1, s_types.size()));
```

At first mission: `s_instanceCapacity == INITIAL_INSTANCES_PER_FRAME = 4096`,
type count ~323, so `type.instanceCap = max(32, 8192/323) = max(32, 25) = 32`.
A single type with >32 instances disarms coalesce for that mission via the
type-overflow runtime guard (§5.1b).

For typical mc2_01 baseline (≈1500 active instances spread across many
types), most types average 4-5 instances; only outliers (e.g., heavy tree
clusters) push >32. The 2× headroom multiplier in the formula scales the
per-type allocation to handle wider distributions when `s_instanceCapacity`
grows mid-session via the existing legacy ring grow-path
(`uploadAllBucketsIfNeeded` at `:240-272`).

Type-overflow runtime disarm is the safety net for under-estimation.
Loss of warm-start tuning across missions is acceptable — first mission
of every session would have been cold-start anyway.

**Grow-path interaction:** `uploadAllBucketsIfNeeded()` (`:240-272`) can grow
and replace `s_instanceSsbo` when `s_instanceCapacity` is exceeded. If this
happens, the original per-type caps stored at `finalizeGeometry()` are NOT
invalidated (they're independent capacities for `s_coalesceInstanceSsbo`).
The two buffers are decoupled.

However: per-type submission can exceed `type.instanceCap`. The
**authoritative overflow guard** lives in the §5.1b CPU write loop, where it
checks `bucket.instances.size() > type.instanceCap` BEFORE the `memcpy`. This
is the memory-safety guard — the CPU writes `bucket.instances.size()` worth
of bytes, so the cap check must use that same value. (Out-CRIT-4 — a prior
draft suggested checking the patched GPU `instanceCount` instead, which is
the count the GPU will RENDER, not the count the CPU WRITES; using the GPU
count for the memory-safety guard could allow a CPU buffer overrun.)

The patched GPU `instanceCount` may be smaller (cull dropped some) — that's
fine, the GPU just renders fewer than the CPU wrote. It cannot be used as
the memory-safety guard.

Optional diagnostic (deferred to a future "coalesce validation hook"
slice per v2r20 — not implemented here): a separate one-shot dev path
(gated by `MC2_SUBSTRATE_COALESCE_VALIDATE=1`) could read the patched
indirect buffer via the C2 readback ring to confirm GPU-visible counts
match expectations.
That path is for parity verification, not memory safety.

### 5.1b Per-frame CPU write path into `s_coalesceInstanceSsbo`

(r4-MAJ-2: was undefined in v2r4.)

`s_coalesceInstanceSsbo` has its own dedicated fence array, separate from the
legacy `s_fence[]`:
```cpp
static GLsync s_coalesceFence[RING_FRAMES] = {};
```
Sharing fences with `s_instanceSsbo` would couple the two ring lifecycles and
defeat the coalesce/legacy-fallback decoupling. Memory cost: 3 × `GLsync`
(pointer-sized) — negligible.

The coalesce write path is invoked from `flush()` immediately after the
existing `uploadAllBucketsIfNeeded()` step at `:1255-1259`, but BEFORE the
legacy `slotInstByteBase` write loop at `:1281-1303`. Pseudocode:

```cpp
void flush() {
    // ... fence wait, alignment, etc. (existing, unchanged) ...

    // out-MAJ-2: coalesce fence cleanup runs unconditionally (not gated on
    // IsCoalesceEnabled()) so fences from prior frames get drained and freed
    // even after a runtime-disarm transition. Otherwise outstanding GLsync
    // objects leak until onMapUnload() — possibly forever if unload misses.
    if (s_coalesceFence[s_frameSlot]) {
        glClientWaitSync(s_coalesceFence[s_frameSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_coalesceFence[s_frameSlot]);
        s_coalesceFence[s_frameSlot] = 0;
    }

    if (IsCoalesceEnabled()) {

        // Per-frame ring offset.
        const size_t fr_off_bytes =
            s_frameSlot * batcher_getCoalescePerFrameInstanceBytes();
        auto* coalesceMapBase =
            static_cast<uint8_t*>(s_coalesceInstanceMap) + fr_off_bytes;

        // Iterate types in NATURAL typeID order (matches s_bucketsByType).
        // For each, copy the bucket's instance vector into the type's
        // CAPACITY-BASED slot within the bound group region.
        // Pre-computed at finalize (§5.2):
        //   s_offGroupTotalBytes = Σ(s_types[t].instanceCap * sizeof(Inst))
        //                          over all alpha-OFF types.
        // (File-scope variable in gos_static_prop_batcher.cpp; flush() lives
        // in the same TU so direct access — no exported accessor.)
        const size_t off_total_bytes = s_offGroupTotalBytes;
        for (auto& [typeID, bucket] : s_bucketsByType) {
            const auto& type = s_types[typeID];
            // coalesceByteOffsetWithinGroup is GROUP-relative (= 0 for the
            // first type in its group). Add the group base to get the
            // frame-relative offset. (r5-MAJ-1 fix: prior naming
            // "WithinFrame" + group-relative semantics caused alpha-ON
            // instances to overwrite alpha-OFF group memory.)
            const size_t groupBase_bytes =
                (type.alphaClass == 1) ? off_total_bytes : 0;
            uint8_t* dst = coalesceMapBase
                         + groupBase_bytes
                         + type.coalesceByteOffsetWithinGroup;

            // Per-type overflow check — set s_coalesceArmed=false if needed.
            if (bucket.instances.size() > type.instanceCap) {
                LOG_WARN("[COALESCE v1] event=disarmed reason=type_overflow "
                         "type=%u count=%zu cap=%u",
                         typeID, bucket.instances.size(), type.instanceCap);
                s_coalesceArmed = false;
                // Note: `break` here means types after `typeID` get stale
                // bytes in the coalesce buffer this frame. That's safe:
                // s_coalesceArmed=false routes the next flush to legacy.
                // The buffer is freshly allocated each mission (§9), so
                // stale-from-prior-mission data is never re-read on
                // subsequent re-arm. (r5-MIN-2.)
                break;
            }
            std::memcpy(dst,
                        bucket.instances.data(),
                        bucket.instances.size() * sizeof(GpuStaticPropInstance));
        }
    }

    // Legacy write path to s_instanceSsbo (existing, unchanged).
    // See §5.1c for why this still runs even when coalesce is armed.
    // ... existing code at :1281-1303 ...

    // ---- Issue draw calls (existing legacy + new coalesce per §3) ----
    // Legacy per-type per-packet glDrawElementsIndirect loop (existing
    // code at :1542-1716) runs when !IsCoalesceEnabled().
    //
    // Coalesce path (NEW, replaces the legacy loop when armed):
    if (IsCoalesceEnabled()) {
        // ... 2x glBindBufferRange + 2x glMultiDrawElementsIndirect per §3 ...
        // (Detailed bind+draw sequence shown in §3 ASCII diagram.)
    }

    // ---- Fence insertion (AFTER all draws issued) ----
    if (IsCoalesceEnabled()) {
        s_coalesceFence[s_frameSlot] =
            glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    // existing: s_fence[s_frameSlot] = glFenceSync(...) for legacy ring
}
```

**`type.coalesceByteOffsetWithinGroup`:** new per-type field, computed once
at `finalizeGeometry()` after the alpha-sort. Equals
`Σ(s_types[t].instanceCap * sizeof(GpuStaticPropInstance))` for all
preceding types in `s_sortedTypeOrder` within the SAME group. Stored in
`GpuStaticPropType` alongside `instanceCap`. The first type in each group
has `coalesceByteOffsetWithinGroup = 0`. (Field previously named
`coalesceByteOffsetWithinFrame` in v2r5; renamed in v2r6 because the
"WithinFrame" name conflicted with its group-relative semantics and
caused alpha-ON instances to overwrite alpha-OFF group memory in the
write loop.)

**`s_offGroupTotalBytes`:** module-scope `size_t` in
`gos_static_prop_batcher.cpp`, computed at `finalizeGeometry()`. Equals
`Σ(s_types[t].instanceCap * sizeof(...))` over all alpha-OFF types. Used
in §5.1b's write loop as the frame-relative base offset of the alpha-ON
group. **No exported accessor** — `flush()` is `GpuStaticPropBatcher::flush()`
and is defined in the same TU (`gos_static_prop_batcher.cpp`), so it reads
the file-scope variable directly. (See §5.6b removal note.)

The CPU write iterates `s_bucketsByType` (natural typeID order) but writes
to capacity-based slots — the typeID-to-slot mapping is via
`(group_base) + type.coalesceByteOffsetWithinGroup`, where `group_base`
is `0` for alpha-OFF types and `s_offGroupTotalBytes` for alpha-ON types.

### 5.1c Interaction with `submit()` and `uploadAllBucketsIfNeeded`

(r4-MAJ-3: was undefined in v2r4.)

**`submit()` (per-actor instance accumulation):** unchanged. Continues to
accumulate per-type buckets in `s_bucketsByType[typeID].instances`. The
data structure is read by both the legacy and coalesce write paths.

**`s_bucketsByType` clear point (r6-MIN-1):** existing code clears
`s_bucketsByType` at end of `flush()` (`gos_static_prop_batcher.cpp:1321`,
`:1326`, `:1335` exit paths). Both legacy and coalesce write paths
therefore see only this frame's submissions — no cross-frame accumulation.
The new coalesce write loop in §5.1b runs BEFORE these clear points and
benefits from the same invariant.

**`uploadAllBucketsIfNeeded()` (`:240-272`, legacy ring grow):** unchanged.
Continues to grow `s_instanceSsbo` when total submitted instances exceed
`s_instanceCapacity`. The coalesce buffer's size is a SEPARATE static
allocation set at `finalizeGeometry()`; it does NOT grow on demand. If
per-type submission exceeds `instanceCap`, the type-overflow guard in §5.1b
disarms coalesce for the rest of the mission (legacy fallback runs).

**Why the legacy write path still runs when coalesce is armed:**
1. **Parity / debug:** RAlt+9 frag debug-mode hotkeys, `MC2_OBJECT_PARITY_CHECK=1`,
   and the kill-switch `MC2_SUBSTRATE_COALESCE_LEGACY=1` all need an up-to-date
   `s_instanceSsbo`. If the legacy buffer were stale, hot-toggling any of
   these would render last frame's positions until the next frame.
2. **Type-overflow / eviction-detect runtime disarm:** when `s_coalesceArmed`
   transitions to false mid-frame, the next `flush()` falls through to legacy
   — that buffer must already have current-frame data.
3. **Cost:** the legacy write loop is pure CPU `memcpy` against a persistent-
   mapped buffer. The duplicate write is ~N_instances × 96 bytes; for typical
   mc2_01 (≈1500 active instances) this is ~140 KB of additional `memcpy` per
   frame — sub-microsecond overhead, well within the perf budget.

If a future slice can prove the duplicate cost is non-trivial, a follow-up
can gate the legacy write off when coalesce is armed AND no parity/debug/
kill-switch path is active. Out of scope for this slice.

### 5.2 Alpha-class sort and mixed-type guard

At `finalizeGeometry()` (after the ordering-assert and per-slot
`textureAlpha` walk per CRITICAL-C):

1. Each type's `alphaClass` is computed (CRITICAL-C resolution).
2. **Mixed-class assert (relaxed):** for the v1 review's MAJOR-3, check that
   all packets agree. If `pkt.materialFlags & ALPHA_TEST_BIT` differs across
   packets within a type after slot-level resolution, log
   `[COALESCE v1] event=mixed_alpha type=%u; disabling`, set
   `s_coalesceEnabled=false`. Mission falls back to legacy loop.
3. Build `s_sortedTypeOrder[N_types]`: alpha-off types first (stable in
   registration order), then alpha-on types. Record `s_alphaOffCount`,
   `s_alphaOnCount`.

### 5.3 Per-draw SSBO (`s_perDrawSsbo`, binding 4)

One entry per type, in sorted order across both groups. `GL_STATIC_DRAW`,
uploaded at `finalizeGeometry()`. Lifecycle via `onMapUnload()`.

```cpp
struct PerDrawEntry {        // 32 bytes std430-aligned
    int   packetID;          // = type.firstPacket; for debug-mode 2 hash
    int   materialFlags;     // STATIC_PROP_FLAG_ALPHA_TEST for shader branch
    int   maxLocalVertexID;  // for debug-mode 1 gradient
    int   texArrayLayer;     // layer in s_texArrayOff or s_texArrayOn (group-relative)
    float uvScaleX;          // always 1.0 in v2r3 (same-size assertion in §5.4)
    float uvScaleY;          // always 1.0 in v2r3
    int   _pad0;
    int   _pad1;
};
static_assert(sizeof(PerDrawEntry) == 32, "std430 alignment");
```

Fragment shader declaration (CRITICAL-E):
```glsl
flat in uint v_drawID;
struct PerDrawEntry { int packetID; int materialFlags; int maxLocalVertexID;
                      int texArrayLayer; float uvScaleX; float uvScaleY;
                      int _pad0; int _pad1; };
layout(std430, binding = 4) readonly buffer PerDrawData {
    PerDrawEntry entries[];
} perDraw_;
```

`u_packetID`, `u_materialFlags`, `u_maxLocalVertexID` uniforms are removed
from the coalesce-variant fragment shader. The `glUniform1i` call sites in
`flush()` are skipped on the coalesce path.

The PerDrawSsbo binds ONCE before both multi-draw calls (single
`glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, s_perDrawSsbo)`).

### 5.3a Per-group `gl_DrawIDARB` base offset

(r6-CRIT-1 fix.)

`gl_DrawIDARB` is the zero-based command index WITHIN the current
`glMultiDraw*Indirect` call — it RESETS to 0 for each new multi-draw
invocation. The spec issues two separate multi-draw calls (one per group);
without an offset, the alpha-ON group's `gl_DrawIDARB ∈ [0, s_alphaOnCount)`
would index into the alpha-OFF region of the per-draw SSBO (entries
[0..s_alphaOffCount-1]), reading the wrong `materialFlags` (no ALPHA_TEST
bit, no `discard` for transparent texels), wrong `texArrayLayer` (out of
bounds for `s_texArrayOn` if alpha-ON has fewer types, or pointing at the
wrong layer otherwise), and wrong `packetID`.

**Resolution:** add a uniform `u_drawIDBase` to the coalesce-variant frag
shader. Set per group before each multi-draw call.

`uniform uint` crashes this engine's `shader_builder`
(`memory/uniform_uint_crash.md`); declare as `uniform int` and cast in the
shader (matches the existing pattern at `static_prop.frag:34` for
`u_packetID` and `static_prop.vert:74` for `u_parityWrite`).

```cpp
// Before alpha-OFF multi-draw:
glUniform1i(s_locDrawIDBase, 0);
glMultiDrawElementsIndirect(... s_alphaOffCount ...);

// Before alpha-ON multi-draw:
glUniform1i(s_locDrawIDBase, (int)s_alphaOffCount);
glMultiDrawElementsIndirect(... s_alphaOnCount ...);
```

Frag shader:
```glsl
#ifdef MC2_COALESCE
uniform int u_drawIDBase;   // int (not uint) per uniform_uint_crash.md
flat in uint v_drawID;      // = uint(gl_DrawIDARB) from vert (declared per CRIT-E)
// ...
PerDrawEntry pd = perDraw_.entries[v_drawID + uint(u_drawIDBase)];
```

Cache the uniform location once at program creation:
```cpp
GLint s_locDrawIDBase =
    glGetUniformLocation(s_staticPropProgramCoalesce, "u_drawIDBase");
```

The uniform is per-draw-call cheap (`glUniform1i` is sub-microsecond) and
avoids the alternative — two SSBOs `s_perDrawSsboOff` / `s_perDrawSsboOn`
with a re-bind between groups (extra GL calls + lifecycle complexity).

### 5.4 Texture array per group — same-size assertion (replaces v2r2 pad-and-clamp)

One `GL_TEXTURE_2D_ARRAY` per alpha group. Built at `finalizeGeometry()`,
deleted at `onMapUnload()`.

**Empty-group handling (out-CRIT-5):** if `s_alphaOffCount == 0` or
`s_alphaOnCount == 0`, the corresponding group's texture array is NOT
created (`s_texArrayOff` or `s_texArrayOn` stays 0), the same-size
assertion is NOT run for that group (no textures to compare), and the
draw path skips the bind+draw block for that group entirely (per §3
diagram). `glTexImage3D` with `depth = 0` is undefined behavior on some
drivers; `glBindBufferRange` with `size = 0` is similarly fragile. Always
guard with `if (s_alphaXxxCount > 0)`.

**Build (precedent: `gos_terrain_indirect.cpp:1735-1755`):**

1. Walk each type in the group's sorted order. Resolve first packet's GL
   texture ID and pin the node against eviction (per §CRITICAL-B):
   ```cpp
   const auto& firstPkt = s_packets[type.firstPacket];
   const auto& slot     = type.source->listOfTextures[firstPkt.textureSlot];
   uint32_t gosHandle   = slot.gosTextureHandle;
   uint32_t glTexId     = gos_GetGLTextureId(gosHandle);
   type.lastSeenGosHandle = gosHandle;
   mcTextureManager->pinNode(slot.mcTextureNodeIndex);     // refcount-aware
   s_coalescePinnedNodes.push_back(slot.mcTextureNodeIndex);
   ```
2. Read dimensions: `glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
   GL_TEXTURE_WIDTH/HEIGHT, ...)`.
3. Deduplicate by `glTexId`. Assign per-unique `texArrayLayer`.
4. **Same-size assertion:** all unique textures in the group MUST have
   identical dimensions. If any differs, log
   `[COALESCE v1] event=disarmed reason=size_mismatch group=%s expected=%dx%d got=%dx%d`,
   set `s_coalesceEnabled=false`, **immediately delete any partial GL texture
   array** created in step 5/6 of the build (`glDeleteTextures`), and bail
   out of the texture-array build for both groups (legacy loop runs for the
   whole mission). The lifecycle table in §9 covers `onMapUnload()` cleanup
   for the SUCCESSFUL build path; this aborted-mid-build path needs the
   immediate delete or the texture leaks until process exit. (r5-MIN-3 fix.)
   Empirically, MC2 static-prop textures within a single shape are authored
   at one size; this assertion is expected to hold for stock data and most
   mods. The pad-and-clamp fallback from v2r2 is REMOVED — too many
   gotchas (mipmap seams, non-tiled UV bands).
5. `glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, W, H, numLayers, 0,
   GL_BGRA, GL_UNSIGNED_BYTE, nullptr)` — internal RGBA8, transfer BGRA.
6. For each unique texture: `glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA,
   GL_UNSIGNED_BYTE, buf)` then `glTexSubImage3D(... layer, 0, GL_BGRA,
   GL_UNSIGNED_BYTE, buf)`.
7. `glGenerateMipmap(GL_TEXTURE_2D_ARRAY)`.
8. Sampler params: `GL_REPEAT` wrap, `GL_LINEAR_MIPMAP_LINEAR` min,
   `GL_LINEAR` mag.

`PerDrawEntry::uvScaleX/Y` always `1.0f` in v2r3. Reserved for a future
follow-up if same-size assertion proves too restrictive.

### 5.5 Indirect command buffer — sorted order

**Pre-requisite clarification — `visibleIds[]` is not used by `static_prop.vert`:**

The `gpu_cull.comp:70-72` comment states "baseInstance in the indirect command
is bucketBase[b] so gl_BaseInstance addresses the correct range in the vertex
shader." This describes a design where the vertex shader reads
`visibleIds[gl_BaseInstance + gl_InstanceID]` to map from a draw-local instance
index to a "visible" actor index. **`static_prop.vert:112` does NOT do this** —
it reads `instances_.i[gl_InstanceID]` directly from the per-type
`glBindBufferRange`-bound subset of `s_instanceSsbo`. The `visibleIds[]` SSBO
is written by `gpu_cull.comp` but is currently **unused** by the static-prop
vertex path. The cull's effect on rendering reaches the vertex shader only via
the patch shader's `instanceCount` writes.

This means the v2 coalesce spec's `baseInstance` value is an offset into the
**per-frame instance SSBO** (`s_coalesceInstanceSsbo`), NOT into `visibleIds[]`.
The two are decoupled. `visibleIds[]` continues to be GPU-written by
`gpu_cull.comp` keyed on natural typeID; the coalesce path doesn't read it.

Whether the static-prop path SHOULD use `visibleIds[]` for true per-instance
GPU cull (allowing arbitrary visible-actor selection rather than just an
"render the first K of N submitted" count) is a separate question, out of
scope for this slice. Today's behavior: CPU submits all CPU-cull-passing
actors, GPU cull may reduce the count, and the rendered subset is "first K
in submission order" (a known approximation). Coalesce preserves this.

**`baseInstance` definition for v2:** offset into `s_coalesceInstanceSsbo`
within the bound `glBindBufferRange` (group-relative, instance units).
The `bucketBase[]` / `BucketCaps` SSBO and `visibleIds[]` are unrelated to
this value — `compute_buildIndirectBuffer()` may still write `bucketBase[]`
in natural typeID order for the cull shader's writes (independent of the
sorted-order `cmds[]` array).

**Refactor:** `compute_buildIndirectBuffer()` (`gpu_cull_compute.cpp:509`)
iterates `batcher_getSortedTypeOrder()` when **`s_coalesceLayoutReady`**,
falling back to `[0, typeCount)` otherwise (so the legacy path's `cmds[]`
keeps the natural-typeID layout it expects).

**Out-CRIT-3 state-machine separation:** the build-mode decision is keyed off
`s_coalesceLayoutReady` (set BEFORE buffer build, when sorted-order +
per-type caps + group totals are known) — NOT off `s_coalesceEnabled`
(set AFTER all GL resources succeed) and NOT off `s_coalesceArmed` (the
runtime-disarm flag).

**`compute_buildIndirectBuffer()` call ownership (r10-MAJ-1 fix):** the
existing call at `mission.cpp:3094` is the ONLY call site. It runs once
after `finalizeGeometry()` returns and reads `s_coalesceLayoutReady` to
choose sorted vs natural layout. `finalizeGeometry()` does NOT call
`compute_buildIndirectBuffer()` itself; the spec's earlier (v2r10)
double-call would have caused redundant GPU buffer reallocation on every
mission load.

Order of operations in `finalizeGeometry()`:

```cpp
// 1. Compute classification + sort + caps.
[walk types, classify alpha-class, sort, compute instanceCap, ...]
s_coalesceLayoutReady = true;   // sorted_order + caps + group totals valid

// 2. Allocate s_permutationSsbo at typeCount with identity content
//    (r9-MAJ-1: was "MAX_TYPES" — fictional. Defer to here, where
//    s_types.size() is known).
allocPermutationSsboAsIdentity(s_types.size());

// 3. Build texture arrays + per-draw SSBO. May fail (size mismatch,
//    GL error, capacity overflow). The permutation SSBO is NOT yet
//    overwritten with sorted values — still at identity from step 2.
[try build texture arrays / per-draw SSBO]
if (any_failure) {
    s_coalesceLayoutReady = false;
    // s_permutationSsbo stays at identity from step 2 → legacy patch
    // path is correct (writes to cmds[typeID]). No SSBO rollback needed.
    // mission.cpp:3094's call to compute_buildIndirectBuffer() will see
    // s_coalesceLayoutReady=false and build natural-typeID layout.
    return;
}

// 4. Overwrite permutation with sorted values (r9-MIN-1: this is now
//    the LAST step, so any earlier failure leaves identity intact).
overwritePermutationSsboAsSorted();
s_coalesceEnabled = true;
s_coalesceArmed   = true;
// finalizeGeometry() returns; mission.cpp:3094 calls
// compute_buildIndirectBuffer(s_types.size()) with s_coalesceLayoutReady=true
// → builds in sorted layout.
```

`compute_buildIndirectBuffer()` (`gpu_cull_compute.cpp:509`, signature
`bool compute_buildIndirectBuffer(uint32_t typeCount)`) keeps its existing
external signature; only its body changes to read `s_coalesceLayoutReady`
and pick sorted vs natural iteration. The two branches have **different
`baseInstance` semantics** — keep them separate:

```cpp
bool compute_buildIndirectBuffer(uint32_t typeCount) {
    // ... existing s_indirectCmdBuf / s_visibleIdsBuf alloc unchanged ...
    std::vector<DrawCmd> cmds(typeCount);

    if (batcher_isCoalesceLayoutReady()) {
        // ── SORTED layout for coalesce ──────────────────────────────
        // baseInstance = group-relative cumulative cap into
        // s_coalesceInstanceSsbo (read by coalesce vertex shader as
        // gl_BaseInstanceARB + gl_InstanceID).
        const uint32_t* sortedOrder = batcher_getSortedTypeOrder();
        const uint32_t  N_off       = batcher_getAlphaOffCount();
        uint32_t cumCapOff = 0, cumCapOn = 0;
        for (uint32_t i = 0; i < typeCount; ++i) {
            uint32_t typeID = sortedOrder[i];
            uint32_t indexCount, firstIndex; int32_t baseVertex;
            uint32_t legacyCap;  // ignored for coalesce — uses real per-type cap
            batcher_getTypeDrawInfo(typeID, &indexCount, &firstIndex,
                                    &baseVertex, &legacyCap);
            cmds[i].count         = indexCount;
            cmds[i].instanceCount = 0;        // patch shader writes per-frame
            cmds[i].firstIndex    = firstIndex;
            cmds[i].baseVertex    = baseVertex;
            const uint32_t typeCap = batcher_getInstanceCap(typeID);
            const bool isAlphaOn  = (i >= N_off);
            if (!isAlphaOn) {
                cmds[i].baseInstance = cumCapOff;
                cumCapOff += typeCap;
            } else {
                cmds[i].baseInstance = cumCapOn;
                cumCapOn += typeCap;
            }
        }
    } else {
        // ── NATURAL layout (legacy / coalesce-disabled fallback) ────
        // Existing behavior — preserved verbatim from gpu_cull_compute.cpp:548-572.
        // baseInstance = cumulative s_instanceCapacity (for visibleIds[] indexing,
        // even though static_prop.vert doesn't actually consume visibleIds[] today).
        uint32_t cumBase = 0;
        for (uint32_t t = 0; t < typeCount; ++t) {
            uint32_t indexCount = 0, firstIndex = 0, instanceCap = 0;
            int32_t  baseVertex = 0;
            if (!batcher_getTypeDrawInfo(t, &indexCount, &firstIndex,
                                         &baseVertex, &instanceCap)) {
                cmds[t] = {0, 0, 0, 0, cumBase};  // empty type
                continue;
            }
            cmds[t].count         = indexCount;
            cmds[t].instanceCount = 0;
            cmds[t].firstIndex    = firstIndex;
            cmds[t].baseVertex    = baseVertex;
            cmds[t].baseInstance  = cumBase;
            cumBase += instanceCap;            // global s_instanceCapacity
        }
    }

    // ... existing glBufferData(s_indirectCmdBuf, cmds.data()) unchanged ...
    return true;
}
```

The two `baseInstance` formulas differ because the buffers they index into
are different: sorted layout addresses `s_coalesceInstanceSsbo` (capacity-
based per-type slots within group ranges), natural layout addresses
`visibleIds[]` (sized to `Σ s_instanceCapacity`, the existing legacy
contract). Mixing the formulas would corrupt either the coalesce vertex
shader's instance reads or the legacy patch path's `cmds[]` layout.

**Pre-existing limitation — `bucketCaps[]` staleness on legacy ring
grow (v2r20 documentation):** `compute_buildIndirectBuffer()` populates
`bucketCaps[t] = legacyVisibleIdsCap` (`= s_instanceCapacity` at
finalize-time) and uploads to `s_bucketCapsBuf` once at mission load
(`mission.cpp:3094`). The legacy ring grow path at
`gos_static_prop_batcher.cpp:240–272` (`uploadAllBucketsIfNeeded`) can
double `s_instanceCapacity` mid-mission. After grow, `bucketCaps[t]` is
stale: cull shader's `cap = capsData[bucket]`
(`shaders/gpu_cull.comp:231`) reads the pre-grow value, so the
`if (slot >= cap)` overflow gate fires earlier than the actual buffer
capacity allows — silent under-cull, dropped instances. This is
inherited from the existing substrate path and is NOT introduced by
v2 coalesce. Both natural and sorted branches share the same
mission-static `bucketCaps[]`. **Fix is out of scope for this slice**
— deferred to a separate "bucketCaps refresh on legacy ring grow"
slice. Do NOT add a `compute_buildIndirectBuffer()` re-call hook into
the grow path here; we just learned that ad-hoc rebuild paths into
`s_indirectCmdBuf` are easy to get wrong (v3.4 → v3.5 mid-frame
rebuild blackout).

The state-machine flags (renaming + clarifying out-MIN-1):
- `s_coalesceLayoutReady` — sorted order, per-type caps, group totals
  computed; the indirect cmd buffer was built in sorted layout. Set after
  classification, cleared on failure-rollback.
- `s_coalesceEnabled` — all coalesce GL resources successfully created.
  Set last. Cleared on `onMapLoad()` and on §5.4 size-mismatch.
- `s_coalesceArmed` — runtime safety net. Cleared on type-overflow,
  eviction-detect, alpha-class-drift. Re-armed at next `onMapLoad()`.

`IsCoalesceEnabled()` returns true only when ALL three are true (plus env
+ extension). See §7.

Per-type `baseInstance` in the `DrawCmd` — high-level math (the full
implementable form is the two-branch `compute_buildIndirectBuffer()` body
above, which uses the exported accessors since `gpu_cull_compute.cpp`
cannot see file-scope `s_types`):

```cpp
uint32_t cumCapInGroup = 0;
for (uint32_t i = 0; i < N_off_types; ++i) {
    uint32_t typeID = sortedOrder[i];
    cmds[i].baseInstance = cumCapInGroup;  // group-relative
    cumCapInGroup += batcher_getInstanceCap(typeID);   // §5.6b export
    cmds[i].count = totalIndexCount;       // from batcher_getTypeDrawInfo
    cmds[i].firstIndex = firstPktFirstIndex;
    cmds[i].baseVertex = firstPktBaseVertex;
    cmds[i].instanceCount = 0;             // GPU-written by patch shader
}
// Reset cumCapInGroup = 0 for alpha-on group; same loop body.
```

The `glBindBufferRange` at flush time positions the SSBO at the group's first
byte (CRITICAL-A: includes `fr_off`); `gl_BaseInstanceARB + gl_InstanceID`
indexes within the bound range starting from 0.

Per-frame `instanceCount` written by the patch compute shader (§5.6).

### 5.6 Patch compute shader permutation

`gpu_cull_patch.comp` currently:
```glsl
uint b = gl_GlobalInvocationID.x;
if (b >= uint(u_nBuckets)) return;
cmds[b].instanceCount = bucketCountData[b];
```

After alpha-sort, command slot `b` corresponds to the SORTED position.
Update to write through a permutation:
```glsl
uint typeID = b;
uint sortedSlot = permutation[typeID];
cmds[sortedSlot].instanceCount = bucketCountData[typeID];
```

**Out-CRIT-2 fallback safety:** the legacy/disarm path needs the patch shader
to write `cmds[typeID].instanceCount` (natural order, no permutation),
because the legacy draw loop reads `cmdOffset = typeID * 20`. Two acceptable
mechanisms:

(a) **Identity permutation when coalesce layout not active.** Allocate
`s_permutationSsbo` inside `finalizeGeometry()` (after `s_types.size()` is
known) and fill it with the identity map `permutation[i] = i` for
`i ∈ [0, s_types.size())`. The patch shader then writes to the same slot
it would have under the old `cmds[b]` formula. The identity buffer is
overwritten with the sorted permutation only as the LAST step of
`finalizeGeometry()` after every other coalesce resource has built
successfully (see §5.5 step 4).

(b) **Patch-shader uniform branch.** Add `uniform int u_coalesceLayout` set
by C++ before each patch dispatch:
```glsl
uint sortedSlot = (u_coalesceLayout != 0)
                ? permutation[typeID] : typeID;
cmds[sortedSlot].instanceCount = bucketCountData[typeID];
```

V2 picks **(a)** — simpler, no shader-stage branch.

**Allocation timing (r9-MAJ-1 fix — `MAX_TYPES` was fictional):** the patch
shader runs only inside the cull dispatch, which is invoked from `flush()`,
which only runs after `finalizeGeometry()`. There is no need for an
identity-at-`onMapLoad()` allocation; the permutation SSBO is allocated and
populated inside `finalizeGeometry()` once `s_types.size()` is the known
bound. Order in `finalizeGeometry()` (precise ordering matters for the
state machine — see §5.5 step list):

```cpp
// (Inside finalizeGeometry(), after classification + sort + caps computed:)
const uint32_t typeCount = s_types.size();
std::vector<uint32_t> identity(typeCount);
for (uint32_t i = 0; i < typeCount; ++i) identity[i] = i;
if (!s_permutationSsbo) glGenBuffers(1, &s_permutationSsbo);
glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
glBufferData(GL_SHADER_STORAGE_BUFFER,
             typeCount * sizeof(uint32_t),
             identity.data(),                  // start as identity
             GL_DYNAMIC_DRAW);

// ... build texture arrays + per-draw SSBO ...
// (See §5.5 step 3 for the early-return-on-failure flow; if any earlier
// step failed, finalizeGeometry already returned and this overwrite is
// not reached. The SSBO stays at identity content, which is the
// legacy-safe state for the post-finalize compute_buildIndirectBuffer()
// call to read.)

// Sorted-permutation overwrite — LAST step in the success path:
std::vector<uint32_t> permutation = identity;  // start identity
for (uint32_t sortedSlot = 0; sortedSlot < typeCount; ++sortedSlot) {
    uint32_t typeID = s_sortedTypeOrder[sortedSlot];
    permutation[typeID] = sortedSlot;
}
glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                typeCount * sizeof(uint32_t), permutation.data());
```

Identity at allocation + overwrite-as-last-step means failure paths
automatically leave the permutation in legacy-safe state. No explicit
SSBO rollback needed.

**Binding 15** (verified free — final v2r16 choice after two slot
flips). The two-flip history:

- v2r2..v2r14: spec specified binding 13. `shaders/gpu_cull_block_rollup.
  comp:58` already declared `binding = 13` for `BlockVis`, with the
  C++ constant at `gpu_cull_compute.cpp:47` (`BLOCK_VIS_BINDING = 13u`).
  Dispatch ordering (rollup at `:874` runs AFTER patch at `:835`) made
  slot 13 mechanically reusable but the "free slot" claim was factually
  wrong; a future cleanup or dispatch reorder would reintroduce silent
  corruption. Out3-CRIT-3 (v2r15) flipped to binding 14.
- v2r15: chose binding 14 as "lowest free slot." But
  `gpu_cull_compute.cpp:855` has an existing env-gated diagnostic
  readback at slot 14 expecting the readback SSBO. Out4-MAJ-1 (v2r16)
  caught this; v2r16 flipped to binding 15.
- v2r16+ (current): **binding 15**. Verified free against both shader
  declarations AND C++ binding readbacks (no shader uses
  `binding = 15`; no C++ `glBindBuffer*(... 15, ...)` outside the new
  coalesce path).

Current `gpu_cull_*` slot map: 9 visibleIds, 10 BucketCounts, 11
IndirectCmds, 12 ActorVis, 13 BlockVis, 14 diagnostic readback (env-
gated), **15 permutation** (this spec). Patch shader declaration:
```glsl
layout(std430, binding = 15) readonly buffer PermutationBuf {
    uint permutation[];
};
```

Lifecycle: `s_permutationSsbo` created inside `finalizeGeometry()` (sized to
`s_types.size()`) with identity content, overwritten with sorted values
ONLY as the last step of `finalizeGeometry()` after all coalesce resources
build successfully, deleted at `onMapUnload()`. The patch shader binding
is **always** populated when running, so legacy/disarm paths are safe — on
build failure, the identity content stays in place and the legacy patch
write semantics (`cmds[typeID].instanceCount = bucketCountData[typeID]`)
hold automatically.

The cull shader's `bucketCountData` encoding stays keyed by natural typeID;
permutation only reorders the WRITE in the patch shader.

**Out-MIN-2 invariant:** when coalesce is not armed (or not even built —
identity permutation in effect), the patch shader writes
`cmds[typeID].instanceCount` (because `permutation[typeID] == typeID` for
the identity buffer). The legacy draw loop reads `cmdOffset = typeID * 20`,
so the legacy path sees its expected per-type counts. The permutation
mechanism is correctness-equivalent to the old direct write when
identity-loaded.

### 5.6a Cap-semantics naming convention (out4-CRIT-2 v2r16)

The two `compute_buildIndirectBuffer()` branches use **different** per-type
cap sources by design:

| Branch | Cap source | Buffer indexed | Purpose |
|---|---|---|---|
| Natural (legacy / coalesce-disabled) | `batcher_getTypeDrawInfo(...&legacyCap)` returns `s_instanceCapacity` (legacy global pool size) | `visibleIds[]` sized to `Σ s_instanceCapacity` | preserves legacy `cumBase += s_instanceCapacity` per-type stride |
| Sorted (coalesce) | `batcher_getInstanceCap(typeID)` returns `type.instanceCap` (per-type formula from §5.1) | `s_coalesceInstanceSsbo` sized to `Σ type.instanceCap` per group | per-type capacity-based packing |

**Do NOT change `batcher_getTypeDrawInfo` to return `type.instanceCap`.**
That would silently break the natural branch's `visibleIds[]` indexing
contract (the legacy patch-shader path still relies on `s_instanceCapacity`
stride). The two caps live separately on purpose.

**Local-variable naming convention** (executor-binding):
```cpp
uint32_t legacyVisibleIdsCap = 0;        // from batcher_getTypeDrawInfo
batcher_getTypeDrawInfo(t, ..., &legacyVisibleIdsCap);

uint32_t coalesceInstanceCap = batcher_getInstanceCap(typeID);  // sorted branch
```
Names like `instanceCap` (without qualifier) are forbidden — they invite
confusion between the two caps. The §5.5 pseudo-code uses `legacyCap`
and `typeCap` for this reason.

### 5.6b New batcher exports (declared in `gos_static_prop_batcher.h`)

```cpp
// Coalesce-related accessors used by gpu_cull_compute / gpu_cull_patch wiring.
const uint32_t* batcher_getSortedTypeOrder();   // size = batcher_getTypeCount()
uint32_t        batcher_getAlphaOffCount();
uint32_t        batcher_getAlphaOnCount();
uint32_t        batcher_getInstanceCap(uint32_t typeID);
GLuint          batcher_getCoalesceInstanceSsbo();   // for glBindBufferRange callers
GLuint          batcher_getPerDrawSsbo();
GLuint          batcher_getTexArrayOff();
GLuint          batcher_getTexArrayOn();
GLuint          batcher_getPermutationSsbo();
uint32_t        batcher_getCoalescePerFrameInstanceBytes();  // for fr_off math
bool            batcher_isCoalesceLayoutReady();             // §5.5: sorted vs natural layout in compute_buildIndirectBuffer()
bool            batcher_isCoalesceArmed();                   // §5.1 type-overflow / §CRIT-B / §5.2 mixed
```

(Two prior accessors removed for being dead exports: `batcher_getCoalesceFrameSlot()`
in v2r4 and `batcher_getOffGroupTotalBytes()` in v2r7 — both are file-scope
state in `gos_static_prop_batcher.cpp` accessed directly by `flush()`. No
external TU caller exists; an exported accessor adds surface area without
benefit.)

Also: `compute_buildIndirectBuffer()` keeps its existing signature; only its
body changes (iterate sorted order). No new compute exports.

### 5.7 Vertex shader change

`static_prop.vert:112`:
```glsl
// Before:
Instance inst = instances_.i[gl_InstanceID];
// After (requires ARB_shader_draw_parameters):
Instance inst = instances_.i[gl_BaseInstanceARB + gl_InstanceID];
```

Also, per CRITICAL-E:
```glsl
flat out uint v_drawID;
// in main():
v_drawID = uint(gl_DrawIDARB);
```

Both `gl_DrawIDARB` and `gl_BaseInstanceARB` are extension-suffixed names
that require the extension prefix (§6). Under `#version 430` + extension,
the unsuffixed `gl_DrawID` and `gl_BaseInstance` are NOT defined (those
are the GL 4.6 core promotion names). Use the `*ARB` form literally.

### 5.8 Parity harness exclusion

`static_prop.vert:263-303` writes parity readback indexed by per-type
uniforms (`u_parityVertsPerType`, `u_parityBaseVertex`). Incompatible with
multi-draw (uniforms can't vary per draw command).

**Resolution:** when `gos_object_parity::IsParityCheckEnabled()` returns true
(checks `MC2_OBJECT_PARITY_CHECK`, declared in `gos_object_parity.h`), the
coalesce path is forced off. Enforced at `flush()`:
```cpp
if (!IsCoalesceEnabled() || gos_object_parity::IsParityCheckEnabled()) {
    runLegacyPerBucketLoop();
    return;
}
```

---

## 6. Extension probe + dual-program restructure (MAJOR-A)

| Extension | Use | Fallback |
|---|---|---|
| `GL_ARB_shader_draw_parameters` | `gl_BaseInstanceARB` + `gl_DrawIDARB` in vert | Coalesce disarmed; legacy loop runs |

**Probe inside `loadProgramsIfNeeded()` at the top, before `s_programLoadTried` latches** (out4-CRIT-3 v2r16). v2r15's earlier wording said "probe at GL context create / persists for process life" — that was insufficient because `loadProgramsIfNeeded()` (`gos_static_prop_batcher.cpp:192`) runs from `finalizeGeometry()` at `:707`, which is the FIRST place the probe is needed AND is one-shot via `s_programLoadTried` at `:201`. If `s_hasShaderDrawParams` is set elsewhere (post-`:795` per a v2r15 misordering), the latch has already fired and the coalesce program never compiles.

Correct sequence inside `loadProgramsIfNeeded()`:

```cpp
static void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    s_programLoadTried = true;

    // out4-CRIT-3: probe + env decision MUST land BEFORE either
    // makeProgram call so the coalesce-program compile decision is
    // correct on the very first call.
    coalesce_resetEnvOnce();
    s_hasShaderDrawParams = glewIsSupported("GL_ARB_shader_draw_parameters");
    // glewIsSupported() pattern: gameosmain.cpp:930

    // Always compile legacy.
    s_staticPropProgramObj = glsl_program::makeProgram(
        "static_prop", ".../static_prop.vert", ".../static_prop.frag",
        kShaderPrefixLegacy);
    // ... existing legacy-link error handling, location cache fill ...

    // Compile coalesce only when extension present AND env not killed.
    if (s_hasShaderDrawParams && !s_coalesceEnvDisabled) {
        s_staticPropProgramCoalesce = glsl_program::makeProgram(
            "static_prop_coalesce", "...", "...",
            kShaderPrefixCoalesce);
        // ... coalesce-link error handling, location cache fill,
        //     u_texArr → unit 0 bind ...
    }
}
```

Plan-side: §5.0's "Phase 2 — coalesce side-attempt" pseudo-code no
longer needs to call `coalesce_resetEnvOnce()` itself; the probe + env
decision are baked into `loadProgramsIfNeeded()`. §5.0's pseudocode
should still call `coalesce_resetEnvOnce()` defensively (idempotent
memoized guard), but the binding decision in `loadProgramsIfNeeded()`
is now authoritative.

(Note: the v2r15 §5.0 ordering pseudo-code shows `coalesce_resetEnvOnce()`
before `coalesceWanted` — this stays. The fix is that
`loadProgramsIfNeeded()` ALSO calls it at the top, since the latch
gates the program-compile decision and the env decision must precede
the latch.)

**Two program variants** (replaces single `s_staticPropProgram`).

The shared `kShaderPrefix` mechanism is single-prefix-per-program. Since the
fragment stage doesn't reference `gl_DrawIDARB` directly (it reads
`v_drawID`, the flat varying), the `#extension : require` only needs to
appear in the vertex stage's compile. But `glsl_program::makeProgram()`
takes ONE prefix that applies to both stages. Two options:

(a) Use one shared prefix with the `#extension` line in both stages — the
    fragment stage simply ignores it (extension declared but unused is legal
    GLSL). Simpler.
(b) Switch to `glsl_program::makeProgram2()` or extend `makeProgram()` to
    take per-stage prefixes. More invasive.

V2r5 picks (a) — shared prefix, extension declared in both stages. The
`MC2_COALESCE` sentinel define is appended to the same prefix so both
stages see it.

```cpp
// File-scope statics in gos_static_prop_batcher.cpp:
static GLuint s_staticPropProgramLegacy   = 0;
static GLuint s_staticPropProgramCoalesce = 0;

// String-literal concatenation (compile-time, no runtime + or +=).
static const char* kShaderPrefixLegacy   = "#version 430\n";
static const char* kShaderPrefixCoalesce =
    "#version 430\n"
    "#extension GL_ARB_shader_draw_parameters : require\n"
    "#define MC2_COALESCE 1\n";
```

`loadProgramsIfNeeded()` compiles both variants with **different program
names** so the `glsl_program::makeProgram` cache (`shader_builder.cpp:611-614`)
treats them as distinct entries:

```cpp
s_staticPropProgramLegacy = glsl_program::makeProgram(
    "static_prop", "shaders/static_prop.vert", "shaders/static_prop.frag",
    kShaderPrefixLegacy);

if (s_hasShaderDrawParams) {
    s_staticPropProgramCoalesce = glsl_program::makeProgram(
        "static_prop_coalesce",
        "shaders/static_prop.vert",
        "shaders/static_prop.frag",
        kShaderPrefixCoalesce);
}
```

The coalesce variant compiles the SAME `.vert` / `.frag` source files; the
prefix's `#define MC2_COALESCE 1` switches behavior via preprocessor
`#ifdef` blocks inside the shaders:

```glsl
// In static_prop.vert:
#ifdef MC2_COALESCE
    Instance inst = instances_.i[gl_BaseInstanceARB + gl_InstanceID];
    v_drawID = uint(gl_DrawIDARB);
#else
    Instance inst = instances_.i[gl_InstanceID];
    v_drawID = 0u;  // unused in legacy path
#endif

// And in static_prop.frag — both branches read the SAME outputs but via
// different sources:
#ifdef MC2_COALESCE
    // out-MAJ-1: must add u_drawIDBase per §5.3a — gl_DrawIDARB resets
    // per multi-draw call, so without the offset the alpha-ON group's
    // draws would index into the alpha-OFF region.
    PerDrawEntry pd = perDraw_.entries[v_drawID + uint(u_drawIDBase)];
    int materialFlagsLocal   = pd.materialFlags;
    int packetIDLocal        = pd.packetID;
    int maxLocalVertexIDLocal = pd.maxLocalVertexID;
    int texLayerLocal        = pd.texArrayLayer;
#else
    int materialFlagsLocal   = u_materialFlags;
    int packetIDLocal        = u_packetID;
    int maxLocalVertexIDLocal = u_maxLocalVertexID;
    int texLayerLocal        = -1;  // sampler is u_tex (sampler2D), not array
#endif
```

The fragment shader's `u_tex` sampler stays `sampler2D` in legacy and becomes
`sampler2DArray u_texArr` in coalesce — gate via `#ifdef MC2_COALESCE`.

**Coalesce sampler binding (out-MAJ-5):** the `u_texArr` sampler defaults
to texture unit 0 only if uninitialized, which is implicit and fragile.
After linking `s_staticPropProgramCoalesce`, cache the location and
explicitly bind to texture unit 0 once:
```cpp
GLint s_locTexArr = glGetUniformLocation(s_staticPropProgramCoalesce, "u_texArr");
glUseProgram(s_staticPropProgramCoalesce);
glUniform1i(s_locTexArr, 0);  // texture unit 0
glUseProgram(0);  // restore
```
The `glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, ...)`
calls per group (§3) target the same unit. Add `s_locTexArr` to §9.

`flush()` selects which program to use:
```cpp
GLuint prog = IsCoalesceEnabled() ? s_staticPropProgramCoalesce
                                  : s_staticPropProgramLegacy;
glUseProgram(prog);
```

If `s_hasShaderDrawParams=false`, `s_staticPropProgramCoalesce=0` (never
compiled), `IsCoalesceEnabled()` returns false, legacy program used.

**Critical implementation watchpoint (out2-MIN-2):** the no-extension early-
return path inside `finalizeGeometry()` MUST still allocate and identity-fill
`s_permutationSsbo` before returning. The patch compute shader (`gpu_cull_patch.comp`)
binds binding 15 unconditionally (was binding 13 in v2r14; out3-CRIT-3
moved it) — it has no extension dependency. If the
early-return skips permutation allocation, the patch shader reads an unbound
or zero buffer at every dispatch and writes `cmds[0].instanceCount` for every
type, corrupting the legacy draw loop's natural-typeID layout. This is the
same class of bug as the original out-CRIT-2: the patch shader's view of
the world must be valid in EVERY code path that reaches it.

Sketch of the no-extension `finalizeGeometry()` early-return:
```cpp
if (!s_hasShaderDrawParams) {
    LOG("[COALESCE v1] event=disarmed reason=no_extension");
    // Still allocate identity permutation — patch shader binds it
    // unconditionally in subsequent flushes (legacy draw loop relies
    // on cmds[typeID].instanceCount being correctly written):
    allocPermutationSsboAsIdentity(s_types.size());
    s_coalesceLayoutReady = false;
    s_coalesceEnabled     = false;
    s_coalesceArmed       = false;
    return;
}
// ... rest of finalizeGeometry per §5.5 ...
```

The same invariant applies to ALL early-return paths in `finalizeGeometry()`
that can occur AFTER `s_types` is populated — e.g., a future check that
disarms coalesce based on pre-validation. Identity allocation must happen
before any such return.

Document the `#extension` adoption in `docs/architecture.md` and CLAUDE.md
"Critical Rules / Shader #version" as the FIRST `#extension` precedent in the
codebase. Future `#extension` use must remain probe-gated.

### 6.X Shared-uniform upload contract (out3-CRIT-2)

GL uniforms are **per-program**. Switching to `s_staticPropProgramCoalesce`
via `glUseProgram` does NOT carry uniform values from the legacy program.
The existing `flush()` body at `gos_static_prop_batcher.cpp:1447–1466`
uploads FIVE shared uniforms to the legacy program every frame:

| Uniform | Type | Source |
|---|---|---|
| `terrainMVP` | `mat4` (`GL_FALSE`) | CPU-composed `axisSwap * worldToClip` |
| `u_terrainViewport` | `vec4` | viewport |
| `u_mvp` | `mat4` (`GL_TRUE`) | secondary MVP for the post-projection chain |
| `u_fogValue` | `float` | fog scalar (currently 1.0) |
| `u_debugAddrMode` | `int` | `debugAddrMode_` (RAlt+9 cycle) |

V2r14 named only the coalesce-specific uniforms (`u_drawIDBase`, `u_texArr`)
in §5.3a / §6 / §9. v2r15 makes the contract explicit:

**Mandatory invariant:** for the coalesce program to render correctly, all
five shared uniforms above MUST be uploaded to `s_staticPropProgramCoalesce`
every frame `flush()` selects it (Step 11.7.a). Failure mode: identity
`terrainMVP` → all static props collapse to clip-space origin (silent —
visual regression only, no GL error).

**Implementation pattern.** Two acceptable shapes:

(a) **Per-program location cache.** At `loadProgramsIfNeeded()` time, after
each program links, cache its uniform locations into per-program `GLint`
statics. Recommended pattern — matches the existing `s_loc_u_parityWrite`
etc. caches at `:226–228`:
```cpp
struct ProgramLocs {
    GLint terrainMVP, terrainViewport, mvp, fogValue, debugAddrMode;
    GLint maxLocalVertexID, materialFlags, packetID;  // legacy-only
    GLint drawIDBase, texArr;                          // coalesce-only
};
static ProgramLocs s_locsLegacy;
static ProgramLocs s_locsCoalesce;
// fill at link time via glGetUniformLocation; legacy fields populated for
// both programs (they exist in legacy variant even when MC2_COALESCE
// is defined, because the #ifdef removes them from the coalesce frag —
// glGetUniformLocation returns -1, treat as "skip upload"; not an error).
```

(b) **Uniform-upload helper.** Single function that takes the program and
uploads all shared uniforms by name lookup. Works but does ~5
`glGetUniformLocation` calls per frame per program; (a) caches them once.

V2r15 picks (a). Step 7.4 of the implementation plan must extend its
location cache to include all five shared uniforms for the coalesce
program; Step 11.7 must upload them after `glUseProgram(s_staticPropProgram
Coalesce)` and before the multi-draw calls. The legacy program continues
to upload via the existing `:1447–1466` block, unchanged.

### 6.Y `flushShadow()` is legacy-only (out3-MIN-E forward-compat)

`gos_static_prop_batcher.cpp:1794–1796` is currently:

```cpp
void GpuStaticPropBatcher::flushShadow() {
    // Filled in Task 13.
}
```

When Task 13 (or any future shadow-side slice) fills this body, the
implementer MUST NOT auto-adopt `s_staticPropProgramCoalesce` by
copy-paste from `flush()`. The shadow pass has its own program
(`shadow_object` per the dynamic mech-shadow design) and runs with
different state (depth-only, no fragment shading). The coalesce path's
PerDraw SSBO + texture-array reads are fragment-stage; they do not
apply to shadow rendering.

**Forward-compat invariant for `flushShadow()`:** `IsCoalesceEnabled()`
returns false on the shadow path by construction (the shadow program
is selected before `flushShadow` is called; the legacy/coalesce switch
in `flush()` is irrelevant). If a future slice wants the multi-draw
shape for shadow casting, that is a separate design with its own
parity gate — do not assume v2r15's coalesce design generalizes.

---

## 7. Kill-switch

`MC2_SUBSTRATE_COALESCE_LEGACY=1` falls back to per-type/per-packet
`glDrawElementsIndirect` loop (today's path).

```cpp
// Module-scope:
static bool s_coalesceEnvDisabled = false;  // resolved once at startup
static bool s_coalesceEnabled     = false;  // set in finalizeGeometry(); cleared
                                            // at registration/build failures
static bool s_coalesceArmed       = false;  // set TRUE in finalizeGeometry();
                                            // cleared at runtime on overflow/
                                            // eviction. Reset on onMapLoad().
extern bool s_hasShaderDrawParams;          // probed at GL context create
extern bool s_geometryFinalized;            // existing flag in batcher

// out3-MIN-D (v2r15): the env name `MC2_SUBSTRATE_COALESCE_LEGACY` is
// the kill-switch. When set (any value), s_coalesceEnvDisabled is true
// for the entire process and IsCoalesceEnabled() short-circuits false.
// The lookup is process-once (memoized via the local static); subsequent
// calls are a single cached-bool read. First call comes from
// IsCoalesceEnabled(), which is invoked from finalizeGeometry()
// (Step 5.0 ordering rule) BEFORE any coalesce GL resources are
// allocated — so the env decision lands before any work is wasted.
inline void coalesce_resetEnvOnce() {
    static bool s_done = false;
    if (s_done) return;
    s_coalesceEnvDisabled = (getenv("MC2_SUBSTRATE_COALESCE_LEGACY") != nullptr);
    s_done = true;
}

static bool IsCoalesceEnabled() {
    coalesce_resetEnvOnce();
    if (s_coalesceEnvDisabled)    return false;
    if (!s_hasShaderDrawParams)   return false;
    if (!s_geometryFinalized)     return false;  // pre-finalize debug/hotkey path
    if (!s_coalesceLayoutReady)   return false;  // out-CRIT-3: layout-mode flag
    if (!s_coalesceEnabled)       return false;
    if (!s_coalesceArmed)         return false;
    // out3-MAJ-1 (v2r15): handle existence checks. State flags + extension
    // probe are insufficient — a coalesce-program LINK failure (or a
    // partially-built texture array path that escaped a return) leaves the
    // flags true but the GL handle 0. Without these guards, flush() does
    // glUseProgram(0) at the draw site and silently emits nothing (or
    // draws against the previously-bound program, which is worse).
    if (s_staticPropProgramCoalesce == 0) return false;
    if (s_coalesceInstanceSsbo == 0)      return false;
    if (s_perDrawSsbo == 0)               return false;
    if (s_permutationSsbo == 0)           return false;
    // Empty-group path: at least one group must have content. The
    // empty-group skip in §3 / §11 handles the "this group is empty"
    // case per-bind, but if BOTH groups are empty there's nothing to draw
    // and the legacy path is the cheap correct answer.
    if (s_alphaOffCount == 0 && s_alphaOnCount == 0) return false;
    if (gos_object_parity::IsParityCheckEnabled()) return false;  // §5.8
    return true;
}
```

(r4-CRIT-2 fix: removed the per-program `static int s_armed` memo. It
permanently latched the first call's classification result and could not
re-arm on a subsequent mission with clean data. The check is six bool
short-circuited reads — negligible hot-path cost. Only the env-var read is
memoized via `coalesce_resetEnvOnce()`, since `getenv` traverses the
environment block.)

`onMapLoad()` (`gos_static_prop_batcher.cpp:490-503`) extension:
```cpp
s_coalesceEnabled = false;
s_coalesceArmed   = false;
// s_geometryFinalized is already reset to false at :498
```
`finalizeGeometry()` sets both `s_coalesceEnabled` and `s_coalesceArmed` to
true after successful classification + texture array build + per-draw SSBO
upload. Runtime safety nets clear `s_coalesceArmed` on type-overflow or
eviction-detect (§5.1, §CRITICAL-B); `s_coalesceArmed` stays false for the
rest of the mission and re-arms fresh on the next `onMapLoad()`.

Also forced false when `gos_object_parity::IsParityCheckEnabled()` (§5.8).

---

## 8. MAJOR resolutions

- **MAJOR-1** (v1) — SSBO offset invariant: §5.1 capacity-based layout.
- **MAJOR-2** (v1) — Patch shader permutation cost: §5.6 single SSBO read,
  compute path only.
- **MAJOR-3** (v1) — Mixed-alpha types: §5.2 mixed-class assertion, fall back.
- **MAJOR-4** (v1) — Packet-per-type estimate: `[COALESCE v1]` log replaces
  guess.
- **MAJOR-5** (v1) — Kill-switch: §7.
- **MAJOR-6** (v1) — Bindless teardown: §9 (no bindless; standard delete).
- **MAJOR-A** (v2r2) — Two-program restructure: §6.
- **MAJOR-B** (v2r2) — Substrate-OFF baseline canary: §11 step 1c.
- **MAJOR-C** (v2r2) — Same-size assertion replaces pad-and-clamp: §5.4.
- **MAJOR-D** (v2r2) — New batcher exports enumerated: §5.6b.
- **MAJOR-E** (v2r2) — Permutation SSBO at binding 15 (was 13 in v2r2..v2r14
  — out3-CRIT-3 in v2r15): §5.6.

---

## 9. New GL resources (lifecycle table)

| Name | Type | Storage flags | Created | Destroyed |
|---|---|---|---|---|
| `s_coalesceInstanceSsbo` | SSBO instance ring | `glBufferStorage` `WRITE \| PERSIST \| COHERENT` | `finalizeGeometry()` | `onMapUnload()` |
| `s_texArrayOff` | `GL_TEXTURE_2D_ARRAY` | `glTexImage3D` (BGRA upload) | `finalizeGeometry()` | `onMapUnload()` |
| `s_texArrayOn` | `GL_TEXTURE_2D_ARRAY` | `glTexImage3D` (BGRA upload) | `finalizeGeometry()` | `onMapUnload()` |
| `s_perDrawSsbo` | SSBO per-draw entries | `GL_STATIC_DRAW` | `finalizeGeometry()` | `onMapUnload()` |
| `s_permutationSsbo` | SSBO patch permutation (binding **15** post-v2r15; was 13 in v2r2..v2r14) | `GL_STATIC_DRAW` | `finalizeGeometry()` (identity) + finalize last-step (sorted overwrite) | `onMapUnload()` |
| `s_staticPropProgramCoalesce` | GL program | (no buffer; cached by name) | first `loadProgramsIfNeeded()` | context destroy |

CPU-side data:
- `s_sortedTypeOrder` (`std::vector<uint32_t>`), `s_alphaOffCount`,
  `s_alphaOnCount`, `s_offGroupTotalBytes` — sort + group sizing
- (No cross-mission high-water tracker — r9-MAJ-2 dropped it. The
  v2r9 plan to seed `instanceCap` from prior-mission high-water marks
  was ABA-vulnerable via `TG_TypeShape*` reuse; no content-derived key
  is stable. Per-mission global-divide estimate is the only seed.)
- State machine flags (out-CRIT-3): `s_coalesceLayoutReady` (sorted layout
  built), `s_coalesceEnabled` (all GL resources built), `s_coalesceArmed`
  (runtime safety net)
- `s_hasShaderDrawParams` (probed at GL context create; persists)
- `s_coalescePinnedNodes` (`std::vector<DWORD>` for refcount-aware unpin)
- `s_coalesceFence[RING_FRAMES]` (per-frame fence array for the coalesce
  ring; cleanup is unconditional per out-MAJ-2)
- `s_coalesceInstanceMap` (`void*`, persistent-mapped pointer into
  `s_coalesceInstanceSsbo`; obtained via `glMapBufferRange(...
  GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT)` at
  `finalizeGeometry()` immediately after `glBufferStorage`; released via
  `glUnmapBuffer` at `onMapUnload()` before `glDeleteBuffers`)
- `s_locDrawIDBase` (`GLint`, cached at first `loadProgramsIfNeeded()`
  for `s_staticPropProgramCoalesce`; persists for program lifetime)
- `s_locTexArr` (`GLint`, cached at coalesce program link, set to
  texture unit 0 once via `glUniform1i`; out-MAJ-5)
- `GpuStaticPropType` new fields: `instanceCap`,
  `coalesceByteOffsetWithinGroup`, `lastSeenGosHandle`, `alphaClass`
- **SSBO save/restore contract (v2r15 out3-MAJ-2):** the coalesce path
  in `flush()` must save the prior binding for slots 4 (`s_perDrawSsbo`)
  and 15 (`s_permutationSsbo`) at entry and restore them at exit. The
  existing slot 0–3 envelope at `:1422–1425, 1740–1743` is preserved
  unchanged; v2r15 extends the discipline to the new slots.
- **Per-program uniform location caches (v2r15 out3-CRIT-2):**
  `s_locsLegacy` and `s_locsCoalesce` ProgramLocs structs (or equivalent)
  hold `glGetUniformLocation` results for both programs. Required
  fields: `terrainMVP`, `u_terrainViewport`, `u_mvp`, `u_fogValue`,
  `u_debugAddrMode` (shared) plus `u_maxLocalVertexID`, `u_materialFlags`,
  `u_packetID` (legacy-only, returns -1 in coalesce variant — skip
  upload on -1) plus `u_drawIDBase`, `u_texArr` (coalesce-only).

`onMapLoad()` clears: `s_sortedTypeOrder`, `s_alphaOffCount`,
`s_alphaOnCount`, `s_offGroupTotalBytes`, `s_coalesceLayoutReady = false`,
`s_coalesceEnabled = false`, `s_coalesceArmed = false`,
`s_coalesceFirstFlushDone = false` (out3-MIN-C: per-mission so the
`event=ready` log fires once per mission rather than once per process).
`s_permutationSsbo` is allocated and initialized to identity inside
`finalizeGeometry()` (r9-MAJ-1 fix — `MAX_TYPES` was fictional; defer
allocation until `s_types.size()` is known). No state persists across
missions besides `s_hasShaderDrawParams` (probed at GL context create
once).

`onMapUnload()` releases: pinned nodes via `unpinNode()`,
`s_coalesceFence[]` via `glDeleteSync()` (unconditional cleanup), all GL
resources per §9 lifecycle table.

Per-frame fence array: `s_coalesceFence[RING_FRAMES]` (separate from the
legacy `s_fence[]` to keep coalesce/legacy ring lifecycles independent).

---

## 10. MINOR resolutions

- **MINOR-1** (v1) — `txmmgr.cpp` line correction: §2.2.
- **MINOR-2** (v1) — AMD attr-0 banner line: §2.2.
- **MINOR-3** (v1) — Baseline measurement: `[COALESCE v1]` log + Tracy capture
  at implementation commit time.
- **MINOR-4** (v1) — Cull-cascade rule: spec runs after CPU cull gates,
  reorders draw commands only, no actor visibility change.
- **MINOR-D** (v2r2) — Fragment-side PerDrawSsbo declaration: §5.3 includes
  the GLSL block.
- **MINOR-E** (v2r2) — `GL_BGRA` transfer format: §5.4 + CRITICAL-B.
- **MINOR-F** (v2r2) — §11 step 7 restated: the test is "with extension
  probe forced false, `IsCoalesceEnabled()` returns false AND
  `s_staticPropProgramCoalesce` is never compiled," not just "legacy shader
  still compiles."
- **MINOR-G** (v2r2) — §2.1 explicitly distinguishes C1b (last-wins overdraw,
  bug) from non-C1b (per-packet correct).
- **MINOR-H** (v2r2) — `INITIAL_INSTANCES_PER_FRAME = 4096`, `MIN_PER_TYPE_CAP
  = 32`; per-mission global-divide estimate is the only seed (cross-
  mission high-water tracker dropped in v2r10 per r9-MAJ-2): §5.1.

---

## 11. Parity gate

Before merge to `claude/nifty-mendeleev`:

1. **Visual canary (three-way):**
   - (a) `MC2_GPU_CULL=1 MC2_GPU_CULL_SUBSTRATE=1 MC2_SUBSTRATE_COALESCE_LEGACY=1`
     vs (b) `MC2_GPU_CULL=1 MC2_GPU_CULL_SUBSTRATE=1` (coalesce armed) — must
     be visually identical on mc2_01 and mc2_03.
   - (c) **MAJOR-B**: `MC2_GPU_CULL=1 MC2_GPU_CULL_SUBSTRATE=0` (substrate-OFF
     baseline, the supported default) vs (b). Multi-packet types may show
     texture differences (first-wins vs per-packet-correct). Document any
     differences before substrate is flipped default-on. If significant on
     tier1, the substrate-default-on flip is BLOCKED on per-packet
     differentiation work — coalesce can still ship as-is, since substrate
     is opt-in; the comparison is just the explicit understanding.

2. **Tracy delta:** `Render.GpuStaticProps` ≤200 µs at mc2_01 normal zoom,
   coalesce armed. Legacy still ~2 ms. ≥90% reduction.

3. **DESTROY parity:** `[DESTROY v1]` count delta = 0 across tier1 5/5,
   coalesce ON vs OFF (both with substrate ON).

4. **Counter validation:** observe `[COALESCE v1]` events. `overflow=0` over
   600 frames. Event timing table:

   | Event | Logged at | Meaning |
   |---|---|---|
   | `event=armed types=N off_types=A on_types=B unique_tex_off=U unique_tex_on=V per_frame_inst_bytes=B` | `finalizeGeometry()` end | Coalesce successfully armed for this mission |
   | `event=disarmed reason=size_mismatch group=off expected=WxH got=WxH type=N` | `finalizeGeometry()` | Texture-array build failed §5.4 same-size assertion |
   | `event=disarmed reason=mixed_alpha type=N` | `finalizeGeometry()` | §5.2 mixed-class guard fired |
   | `event=disarmed reason=no_extension` | `finalizeGeometry()` | `s_hasShaderDrawParams==false` |
   | `event=ready buckets_off=N buckets_on=N elapsed_us=T` | First `flush()` after `armed` (per-mission; `s_coalesceFirstFlushDone` resets in `onMapLoad()` per out3-MIN-C) | Confirms first frame's two multi-draw issues completed |
   | `event=disarmed reason=alpha_class_drift type=N expected=C got=C` | First `flush()` (re-validation) | bdactor init ordering anomaly; coalesce off for rest of mission |
   | `event=disarmed reason=type_overflow type=N count=K cap=C` | Subsequent `flush()` | Per-type instance count exceeded `instanceCap` (§5.1) |
   | `event=disarmed reason=tex_evicted type=N old_handle=H new_handle=H` | Subsequent `flush()` | §CRITICAL-B eviction-detect fired |

5. **Kill-switch test:** force `s_hasShaderDrawParams=false` (e.g., add a
   debug-only env override). Confirm `IsCoalesceEnabled()` returns false,
   `s_staticPropProgramCoalesce==0`, legacy loop runs without crash.

6. **Mixed-alpha guard:** no `[COALESCE v1] event=mixed_alpha` warning on
   tier1 5/5.

7. **Same-size guard:** no `[COALESCE v1] event=size_mismatch` warning on
   tier1 5/5. (If any fires, document which group / which types — that's the
   first place to look for follow-up.)

8. **Extension-absent compile guard:** with `s_hasShaderDrawParams=false`,
   `loadProgramsIfNeeded()` skips the coalesce-variant compile. Verify by
   inspection (no compile error from `#extension : require`).

   **Sub-step (out2-MIN-2, v2r20 demoted from VALIDATE):** also verify that
   `s_permutationSsbo` is allocated with identity content even on the
   no-extension path. Use `MC2_COALESCE_FORCE_DISARM=no_extension` (plan
   Step group 12B) + the gated `[COALESCE v1] event=permutation_state
   ssbo=N typeCount=N first4=0,1,2,3` log line emitted at first armed
   flush (also gated on `MC2_COALESCE_FORCE_DISARM != none` so it doesn't
   pollute production runs). Confirm `[COALESCE v1] event=disarmed
   reason=no_extension` appears AND, on a separate run with the env
   unset, the permutation_state line shows the identity sequence. The
   `MC2_SUBSTRATE_COALESCE_VALIDATE=1` readback that prior revisions
   referenced is deferred to a future "coalesce validation hook" slice;
   not added in this slice.

9. **Mission-load wall-clock measurement (r5-MIN-4):** capture
   `finalizeGeometry()` total elapsed time (Tracy zone or `[COALESCE v1]
   event=armed elapsed_ms=N` log line). The texture-array build does ~N
   synchronous `glGetTexImage` calls (one per unique texture per group) at
   mission load — these are GPU readbacks and stall the pipeline. Acceptable
   ceiling for tier1: +200 ms over the legacy `finalizeGeometry()` baseline.
   If above ceiling, document and consider deferring the texture-array build
   to first flush (background-load while legacy renders frame 0).

10. **Empty-group synthetic test (out-MIN-3):** add a debug-forced or
    synthetic mission where one alpha group has zero types (e.g., a map
    with no alpha-tested static props at all, or an env override that
    suppresses one class). Confirm the coalesce path's empty-group skip
    (out-CRIT-5) doesn't crash on `glTexImage3D(depth=0)` or
    `glBindBufferRange(size=0)`. This is hard to hit organically because
    most missions have both trees (alpha-on) and buildings (alpha-off);
    the synthetic case must be deliberately constructed.

11. **Forced-disarm legacy fallback test (out3-CRIT-1, v2r15; executable
    via plan Step group 12B as of v2r20):** force each `finalizeGeometry()`
    disarm path via `MC2_COALESCE_FORCE_DISARM={mixed_alpha|size_mismatch|
    no_extension|alloc_failed}` and confirm: (a) `s_geometryFinalized ==
    true` after `finalizeGeometry()` returns, (b) the legacy draw path
    renders static props correctly for the entire mission (smoke exit 0
    is the gate), (c) `s_permutationSsbo` is non-zero with identity
    content (verify by inspecting the gated `[COALESCE v1]
    event=permutation_state` log line per Step 11 sub-step above —
    inspection-only; no `MC2_SUBSTRATE_COALESCE_VALIDATE` readback), (d)
    all three coalesce flags are false. This is the regression test for
    the v2r14 → v2r15 ordering bug; without it, blackout is a silent
    test gap.

12. **Shared-uniform coverage test (out3-CRIT-2, v2r15; v2r20 simplified):**
    on a normal armed run (no force-disarm env), confirm via screen
    capture on mc2_01 that static props render at correct world
    positions (`terrainMVP` took effect — props NOT collapsed to
    clip-space origin), with correct fog blend (`u_fogValue`), and that
    RAlt+9 cycles the debug modes correctly on coalesce-rendered props
    (`u_debugAddrMode` propagated to coalesce program). Verify
    `[COALESCE v1] event=armed` is in the log (without it, the canary
    would pass-by-coincidence on the legacy fallback). If any uniform
    is silently missing, the canary shows props at origin / no fog /
    stale debug mode — visible regression caught immediately. The
    `MC2_SUBSTRATE_COALESCE_VALIDATE` readback prior revs referenced is
    not needed for this gate.

---

## 11.X Runtime disarm fallback limitation (v2r19, scope clarified v2r20)

Runtime disarm via §5.1 type-overflow guard or §CRITICAL-B
texture-eviction detect AFTER `compute_buildIndirectBuffer()` ran with
sorted layout leaves `s_indirectCmdBuf` populated in sorted layout.
Subsequent legacy `glDrawElementsIndirect` calls
(`gos_static_prop_batcher.cpp:1703`) read the entire indirect command
struct (`count`, `instanceCount`, `firstIndex`, `baseVertex`,
`baseInstance`) at offset `typeID * stride` against natural-typeID
semantics — but those slots hold the SORTED-position type's values.
**All five fields can be wrong** until the next mission load: wrong
geometry, wrong vertex base, wrong index range, wrong instance count,
wrong base. Visible impact is likely "props show wrong geometry / wrong
textures / vanish or render at wrong scale," not a subtle offset drift.
v2r19 wording ("stale baseInstance") understated the scope; v2r20
corrects it. The accept-and-document conclusion is unchanged — operator
re-loads the mission to recover.

**Rationale for accepting this as documented limitation:**

- Both runtime-disarm events are expected to be rare. The §5.1
  per-type cap formula is `max(MIN_PER_TYPE_CAP=32, 2 × global / N_types)`
  — 2× headroom over the average. Texture eviction is mitigated by
  §CRITICAL-B's refcount-aware `pinNode()`; eviction-detect is a
  belt-and-suspenders safety net for code paths the pin doesn't cover
  (LOD swap, hot-reload).
- Both disarm paths log `[COALESCE v1] event=disarmed reason=...` —
  the operator sees the event in smoke-test artifacts and can re-load
  the mission. No silent corruption.
- A mid-frame rebuild would attempt `glBufferData(s_indirectCmdBuf, ...)`
  AFTER `compute_dispatch()` already wrote `instanceCount` for this
  frame's draws — orphaning the buffer and zeroing the freshly-uploaded
  `cmds[]` → blackout the disarm frame entirely. Plan v3.4 attempted
  this rebuild and reverted in v3.5 after both opus and sonnet
  adversarial reviews independently traced the chain. The minimum-
  blast-radius design is "flag-only disarm + accept stale baseInstance
  until next mission load."

**This is the explicit design — do NOT add a mid-frame rebuild in
follow-up slices.** A future slice that wants to make the disarm
fallback rendering-correct must address it via one of:
- (a) Defer the rebuild to next `onMapLoad` only (cleanest; same effect
  as current "limitation" but eliminates the rare incorrect-render
  window). Acceptable to spec.
- (b) Use `glBufferSubData` partial overwrite of just `cmd.baseInstance`
  fields, preserving the patch-written `instanceCount`. Requires
  careful per-frame ordering and a new memory barrier.
- (c) Eliminate the runtime-disarm class entirely (e.g., make per-type
  cap auto-grow on overflow rather than disarm). Larger redesign.

Out of scope for this slice.

---

## 12. Out of scope

- Per-packet texture differentiation in the multi-draw path (per-instance
  texture index, follow-up if §11 step 1c shows visible regressions).
- Default-on substrate flip — separate single-commit slice after 7-day soak.
- Lifecycle gate default-on flip — separate slice.
- Parity harness compatibility with multi-draw path.
- Track D GPU mech batcher — different render path, no overlap.
- Bindless texture adoption — deferred indefinitely (CRITICAL-3 from v1).

---

## 13. Cross-cutting constraint reminders

- **Single-PR ship rule:** Stage 2 (alpha-sort + capacity-layout + new SSBOs
  + texture arrays + per-draw SSBO + dual-program compile) and Stage 3
  (multi-draw issue + inner-packet-loop elimination + frame-offset bind +
  permutation patch) MUST land in one commit. Partial landing → visual
  breakage or perf-gate failure.
- **Build config:** RelWithDebInfo only.
- **Deploy:** `cp -f` + `diff -q` to BOTH v0.2 and v0.3 install dirs.
  No `cp -r`.
- **Adversarial review:** run `adversarial-plan-review` on this rev BEFORE
  implementation. Run again on the implementation commit diff before merge.
- **First `#extension` precedent:** document in `docs/architecture.md` and
  CLAUDE.md "Shader #version" rule.

---

*Spec author: Claude Code session 2026-05-08. Stage 0 recon + two adversarial
review passes (v2r1 → v2r2 → v2r3) performed same session. All citations
verified at write-time against `claude/nifty-mendeleev` HEAD. No source code
was modified.*
