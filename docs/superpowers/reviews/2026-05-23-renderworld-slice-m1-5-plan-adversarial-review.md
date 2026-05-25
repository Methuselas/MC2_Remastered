# Adversarial review: RenderWorld Slice M1.5 plan (2026-05-23)

- Target: `docs/superpowers/plans/2026-05-23-renderworld-slice-m1-5-objectid-buffer-plan.md`
- Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`
- Spec review: `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-5-spec-adversarial-review.md`
- M1 plan gold standard: `docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`
- Reviewer pass: code-grounded; every cited file:line re-grepped at write time.
- Mandate: try to BREAK the plan.

## Verdict

EXECUTE WITH FIXES. The plan has good structure, the C1 META-FIX is
correctly named, the 5 sceneFBO draw-buffer sites verify under fresh
grep, and the substitutive rename is correctly framed. However THREE
self-flagged ambiguities resolve to one CRITICAL (the producer per-type
handle accessor genuinely does not exist; the work is larger than the
plan footprints), Phase A is not actually build-green between Tasks 5
and 7 (linker dangler), and several rename-atomicity issues mirror M1
review C2 in spirit.

Counts: 3 CRITICAL, 4 MAJOR, 6 MINOR.

---

## CRITICAL findings

### C1. Producer per-type handle accessor does NOT exist; Task 10 silently expands scope

- Plan Task 10 Step 3 (plan lines 1582-1599) writes:
  `entry.objectIdRaw = handle.raw()` and footnotes "if the existing
  registry does NOT expose a handle-per-type accessor, this task
  EXPANDS to add one. Surface to user before implementation if so."
- Grep at write time confirms it does not:
  - `grep -n "RenderObjectHandle" GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp` returns ZERO hits.
  - `grep -n "RenderObjectHandle\|registerRecipe" GameOS/gameos/gos_static_prop_registry.h GameOS/gameos/gos_static_prop_registry.cpp`: registry returns `int32_t recipeIndex`, never a Handle.
- The PerDrawEntry producer at `GameOS/gameos/gos_static_prop_batcher.cpp:2007-2040` builds entries keyed by `s_sortedPacketOrder[i]` -> `owningTypeID`; there is no `handleForType` in scope. The plan's snippet `static_cast<int32_t>(handleForType.raw())` references a name that does not exist anywhere in the producer site.
- This is not a 1-line edit. The handle is created in `RenderWorld/RenderWorld.cpp:upsertStaticProp` (line 88-90) from a `recipeIndex` and is NEVER stored back. To make `objectIdRaw` correct per-packet, you need a `typeID -> recipeIndex -> handle` mapping somewhere in the batcher or registry, which means either:
  - Adding a parallel `s_typeIDToRecipeIndex` map in batcher (and wiring `registerType()` callers to pass it), OR
  - Adding `int32_t GpuStaticPropRegistry::getRecipeIndexForType(uint32_t typeID)` and reconstructing the handle bits at write time (`Handle::make(recipeIndex, 1u)` per M1's recipeIndexToHandleIndex), OR
  - Storing the `RenderObjectHandle` itself in `RecipeRange` at registerRecipe time.
- Worse: the plan's `recipeIndex -> handleIndex` translation at `RenderWorld.cpp:41-48` clamps to `0x000FFFFFu` (20 bits) and fixes generation = 1. The naive producer reconstruction must apply the SAME translation, or the per-pixel readback will not match `lookupAtPixel`'s record lookup. The plan does not specify which side owns this translation.
- Recommended fix BEFORE Task 1: resolve the ownership question explicitly. Lean: add `int32_t getRecipeIndexForType(uint32_t typeID)` to `gos_static_prop_registry.h` (the registry already maintains `s_recipeRanges` keyed by regIdx; it needs a type->regIdx side-index), then the producer calls it and reconstructs the handle via `RenderCore::RenderObjectHandle::make(recipeIndex & 0xFFFFFu, 1u)`. Make Task 10 explicit about this; do not let the executor improvise the translation.

### C2. Phase A is NOT build-green between Tasks 5 and 7 -- `lookupAtPixel` declaration is dangling for one commit

- Plan Task 5 Step 3 (line 884) claims "Build green. `lookupAtPixel` is undefined; the linker does NOT complain because there is no caller yet." That is only true if `lookupAtPixel` is DECLARED without ODR. The plan's Task 5 Step 2 (line 877) declares it as a non-inline free function: `LookupResult lookupAtPixel(int screenX, int screenY);`.
- An undefined free function with no caller compiles AND links cleanly only because the linker resolves on use; for a static-library target like `RenderWorld`, this is fine. But the dangling declaration is fragile: any TU that includes `RenderWorld.h` (Task 2 already does this at `gos_postprocess.cpp`!) does NOT trigger the linker error until first call -- which is Task 13's self-test, four commits later.
- The risk shape: between Task 5 (declaration) and Task 7 (definition), the only protection is "nobody calls it." Task 6's `populateRecord` / `retireRecord` are file-scope helpers in `RenderWorld.cpp` and do not reference `lookupAtPixel`, so the gap is real but inert. The plan should explicitly state that Tasks 5+6+7 are a tight triple and Phase A close is NOT at any task before Task 7. The current Phase A gate (line 142-149) requires "the attachment, helper, record table, and lookup API all exist" -- which only holds at Task 7 close.
- This mirrors the M1 plan review C2 lesson: "show full surrounding block, do not leave dangling references." Here it is a dangling SYMBOL not a dangling line.
- Recommended fix: combine Task 5 + Task 6 + Task 7 into a single Phase A "substrate-API" commit, OR explicitly mark Task 5's commit as "intentionally introduces an unresolved symbol; do not deploy this commit in isolation; Tasks 6+7 land in the same Phase A push."

### C3. Task 8 leaves the build INTENTIONALLY RED across the Task 8/9/10 boundary -- this violates Phase B's own gate ladder

- Plan Task 8 Step 3 (line 1417) acknowledges: "COMPILE FAIL on `gos_static_prop_batcher.cpp` if the producer writes `entry._pad0 = 0` anywhere. Use that signal to find the producer site for Task 10." Step 4 (line 1435): "Commit (even if RED -- the next two tasks are tight-coupled)."
- Grep at write time: `grep -n "_pad0\|PerDrawEntry" GameOS/gameos/gos_static_prop_batcher.cpp` returns hits at `:258, :1720, :2007, :2015, :2042, :3282, :3283, :3345`. The producer site at `:2007-2040` builds `PerDrawEntry e{}` via aggregate-init; it does NOT explicitly write `e._pad0`. So the build is GREEN, not RED, after Task 8. The plan's RED-build claim is wrong; the rename compiles cleanly because `_pad0` is implicitly zero-initialized.
- But that's not the critical issue. The critical issue is: the plan ALLOWS a RED commit (Task 8's commit). M1 plan review C2 explicitly flagged "leave the build broken between two tasks" as a hazard. Worktree CLAUDE.md's "full relink before deploy" and "build green between tasks" discipline is violated by design here.
- Even if the build is in fact GREEN (as my grep says), the plan's own framing of "may go RED until Tasks 9+10 land" sets the wrong expectation. An executor following the plan literally will commit a known-RED HEAD and proceed; if anything blocks Tasks 9+10 (greybeard escalation, unexpected shader content drift, CI timeout), the branch is left RED.
- Spec section 5 explicitly calls the C++/GLSL rename "atomic" -- the plan should follow that. The fact that the actual producer aggregate-inits `_pad0` is fortunate; the plan should not depend on that.
- Recommended fix: merge Task 8 + Task 9 + Task 10 into one commit titled "rename PerDrawEntry._pad0 -> objectIdRaw atomically (C++ + GLSL + producer)." The substitutive change is one logical edit; splitting it into three commits gates one risk (granular revert) against three real ones (RED HEAD, partial rename in flight, executor abandoning between commits). Spec's "atomic on both sides" wins.

---

## MAJOR findings

### M1. `lookupAtPixel` resolves the wrong handle bits if the producer applies a different recipe->handle translation than `RenderWorld` does

- `RenderWorld.cpp:41-48` (verified): `recipeIndexToHandleIndex(r) = (uint32_t)r & 0x000FFFFFu`. Generation is hard-coded to 1 in `make(idx, 1u)` at line 90.
- `lookupAtPixel` (Task 7 plan lines 1186-1265) does `h.bits = raw; if (rec.generation != (uint16_t)h.generation()) return invalid;`.
- If the producer (C1 above) writes `Handle::make(recipeIndex, 1u).raw()` while a different code path (the m5 late-spawn `adoptStaticPropRecipe` at `RenderWorld.cpp:99-109`) ALSO writes generation=1 but with a DIFFERENT internal slot index, two recipes can collide on bits.
- The plan's `s_objectRecords` table is keyed by `h.index()` (= recipeIndex's low 20 bits). The producer key (typeID -> recipeIndex) must match. There is no proof in the plan that `recipeIndex` is unique across all batcher write sites -- only that `int32_t recipeIndex` from `registerRecipe()` is unique within the registry's `s_recipeRanges` array.
- Recommended fix: the plan must enumerate the EXACT producer-side handle reconstruction, with the same `& 0xFFFFFu` clamp and generation=1, and confirm via test (substrate self-test Task 13) that round-trip works. Currently the substrate self-test sketch at plan lines 2018-2031 tests the record table directly, NOT the producer-shader-readback chain. Add a sub-case that exercises the producer write path.

### M2. shader makeProgram macro injection is wrong shape -- has two program builds, plan only edits one

- Plan Task 11 Step 2 (lines 1660-1683) shows pseudo-code `std::string prefix = "#version 430\n"; #ifdef MC2_COALESCE ... if (IsObjectIdBufferEnabled()) prefix += "#define MC2_OBJECT_ID_BUFFER 1\n";`. This is the documented C++-side macro injection.
- Grep at write time: `GameOS/gameos/gos_static_prop_batcher.cpp:504-572` shows TWO program builds:
  - `s_staticPropProgramObj` (legacy) built with `kShaderPrefixLegacy = "#version 430\n"` at line 511-514.
  - `s_staticPropProgramCoalesce` (coalesce) built with `kShaderPrefixCoalesce = "#version 430\n#extension GL_ARB_shader_draw_parameters : require\n#define MC2_COALESCE 1\n"` at line 506-509.
- Both prefixes are `static const char*` string LITERALS, not `std::string` accumulators. The plan's "prefix += ..." pattern does not compile against either. The executor must either (a) convert both literals to runtime-built `std::string`s, or (b) build a third runtime variant for env-ON.
- Worse: the coalesce program is gated on `s_hasShaderDrawParams && !s_coalesceEnvDisabled` (line 567). Under env-ON-but-coalesce-disarmed, the legacy program needs the macro; under env-ON-coalesce-armed, BOTH need it. The plan's snippet does not address this.
- Recommended fix: rewrite Task 11 Step 2 with the actual code shape -- two `std::string` builders, both conditionally appending `#define MC2_OBJECT_ID_BUFFER 1\n`. Grep-confirm the line numbers (504-514 for legacy program, 567-580 for coalesce program) before editing.

### M3. Task 4's clear-order rule is correct BUT the env-OFF byte-identical claim is contingent on `sceneObjectIdTex_` being 0 -- which the plan does not assert across all destruction paths

- Plan Task 4 Step 5 (lines 731-740) guards `glClearBufferuiv` with `if (RenderWorld::IsObjectIdBufferEnabled() && sceneObjectIdTex_)`. Correct.
- But `sceneObjectIdTex_` is added as a class member with default-init `= 0` (Task 4 Step 2, line 621). After `destroyFBOs()` (Task 4 Step 4, line 696-700) it is reset to 0. Good.
- The hazard: between FBO recreate cycles (e.g. resolution change triggers destroyFBOs() + createFBOs()), env-ON path will create+destroy attachment-2. If a code path between destroyFBOs() and createFBOs() calls `beginScene()` (unlikely but possible during mode switch), the `glClearBufferuiv` is guarded and safe; the `setSceneDrawBuffers(MainSceneMRT)` call inside `beginScene()` returns the env-OFF 2-entry list (because `sceneObjectIdTex_` is 0... no wait, the helper does NOT check `sceneObjectIdTex_`, only `IsObjectIdBufferEnabled()`).
- That means: in env-ON mid-resize state, `setSceneDrawBuffers(MainSceneMRT)` returns the 3-entry list including `GL_COLOR_ATTACHMENT2` -- but the attachment slot is empty (no texture attached). GL behavior on `glDrawBuffers` listing an unattached slot is implementation-defined; AMD typically logs `GL_INVALID_OPERATION` on the next draw or silently no-ops the write.
- Recommended fix: helper signature should accept the actual attachment count, OR helper should read `sceneObjectIdTex_` directly (i.e. live in postprocess class, not file-scope), OR add an explicit `sceneObjectIdTex_ != 0` AND-clause to the env-ON branch. Adversarial review's C1 says "centralized helper"; that's a positive, but the helper still needs to know whether the texture exists.

### M4. The visual canary mission lean (`mc2_24`) is not grep-verified for static-prop coverage

- Plan O2 (lines 64-79) leans `mc2_24` because "2641 props -- the heaviest static-prop mission in tier1." Number traced to worktree CLAUDE.md "Active campaigns" RenderWorld Slice M1 bullet which lists `mc2_24=2641` static-prop registry counts.
- Worktree CLAUDE.md verified at write time: "objects counts: mc2_01=997, mc2_03=2552, mc2_10=2611, mc2_17=1521, mc2_24=2641."
- Concern: mc2_24's prop count is recipe registrations, not on-screen pixel coverage. A canary mission needs (a) many DIFFERENT prop categories on screen (buildings vs trees) so the user can sweep the cursor across them, and (b) a clean camera position where props are unoccluded by other ID writers. mc2_24's 2641 props could be 99% buildings; the canary user "moves the cursor over multiple static-prop categories (buildings, trees)" presumes both exist.
- Recommended fix: confirm mc2_24 has at least 100+ trees AND 100+ buildings visible in the spawn-camera position. Quickest verification: grep tier1 artifact logs for the per-category breakdown (if any) or eyeball a screenshot. If mc2_24 turns out to be predominantly one category, switch to whichever mission has the mixed coverage -- mc2_10 (2611 props) is a close runner-up.

---

## MINOR findings

### m1. Five sceneFBO_ glDrawBuffers sites: cited line numbers drifted but content matches

- Plan Task 3 Step 1 (line 322) cites approximate lines `:274, :418, :505, :615, :648`.
- Grep at write time on `GameOS/gameos/gos_postprocess.cpp`:
  - `:274` glDrawBuffers(2, drawBuffers) (createFBOs MRT setup) -- MATCH
  - `:418` glDrawBuffers(2, drawBuffers) (beginScene rebind) -- MATCH (plan says `:416-419`)
  - `:486` is a COMMENT mentioning glDrawBuffers (in `clearSceneNormalSentinel`) -- not a real site
  - `:505` glDrawBuffers(1, &singleBuf) (runScreenShadow) -- MATCH (plan says `:503-506`)
  - `:615` glDrawBuffers(1, &singleBuf) (runGodRays Pass 2) -- MATCH (plan says `:613-616`)
  - `:648` glDrawBuffers(1, &singleBuf) (runShoreline) -- MATCH (plan says `:646-649`)
- FIVE real sites confirmed, all line numbers within 1-2 of plan citation. No drift requiring plan revision.
- One concern: the comment at `:486` mentions `glDrawBuffers(2, {COLOR0, COLOR1})` literally -- after the rename to helper, this comment becomes stale. Task 3 should add a comment-update step.

### m2. `extern gosPostProcess* g_pp` is the WRONG accessor symbol

- Plan Task 7 Step 3 (line 1200) writes `extern gosPostProcess* g_pp; // confirm symbol via grep at write time`.
- Grep at write time: no `g_pp` symbol exists. The actual accessor is `gosPostProcess* getGosPostProcess()`. Confirmed at `GameOS/gameos/gos_postprocess.cpp:988`, `GameOS/gameos/gameosmain.cpp:193,202,220,250,318,327,457,520`, `GameOS/gameos/gameos_graphics.cpp:241,1404,4694,...` (20+ call sites).
- The plan flag acknowledges the symbol is unconfirmed but leans `g_pp`. Wrong lean; the correct accessor is `getGosPostProcess()`. This is a 1-line fix in the plan; the implementation pattern remains identical.

### m3. Task 11 references a `#else` block in static_prop.frag at line 52-56 that does not exist as cited

- Plan Task 11 Step 4 (lines 1721-1730) shows "Existing (verbatim, `:52-56`)" with a `#else` / `uniform sampler2D u_tex;` block.
- Grep at write time on `shaders/static_prop.frag`: line 36-37 = `layout(location=0) out vec4 FragColor; layout(location=1) out vec4 GBuffer1;` (the EARLY occurrence the plan also cites). Line 61-62 = same outputs in the late occurrence (under MC2_COALESCE branch).
- The plan cites both lines 37 ("Existing") for the early occurrence AND line 61-62 ("Replace with" in Task 11 Step 3) for the same edit. There are TWO occurrences of `layout(location = 0) out vec4 FragColor;` in this shader file -- one in the legacy branch (~line 36) and one in the coalesce branch (~line 61). The plan's edit only modifies one; both need the location=2 declaration if both program builds are used.
- Recommended fix: edit both occurrences OR move the `#ifdef MC2_OBJECT_ID_BUFFER` guard above both. Verify by re-grepping `layout(location` in the final file -- both `out` declarations should be siblings of `v_objectId`.

### m4. Task 11 Step 5 fragment shader debug-mode early-return omission is documented but is a partial-landing hazard

- Plan Task 11 Step 5 (lines 1751-1782) acknowledges "the early-return debug-mode branches (modes 1-8) all `return` before reaching the post-`FragColor` lines; they will NOT emit `v_objectId`. In M1.5 this is acceptable -- debug modes are not the production path."
- Concern: an executor (or a future debugger) who flips MC2_OBJECT_ID_BUFFER=1 AND a debug-mode env at the same time will see attachment-2 stay 0 for debug-mode pixels -- but the plan doesn't promise that. `lookupAtPixel` will return invalid for those pixels (raw==0 short-circuits).
- This is correct behavior, but undocumented. Add a one-line note in the slice CLAUDE.md bullet: "debug-mode pixels return Handle::invalid() from lookupAtPixel by design (early-return skips emit)."

### m5. greybeard ruling at Task 15 pre-judges to META-FIX without a real adversarial framing

- Plan Task 15 Step 2 (lines 2169-2184) writes: "Expected outcome: META-FIX." with full justification. The dispatch prompt at Step 1 is well-formed, but the "expected outcome" leak means the executor will not freshly run the greybeard skill -- they'll copy the expected verdict.
- Worktree CLAUDE.md "Meta-fix discipline (load-bearing)" requires the greybeard ruling be EXPLICIT, not assumed. Pre-judging defeats the purpose.
- The META-FIX framing is probably correct (the helper does retire the MRT-drift bug class), but the plan should not put the verdict in the executor's hand pre-ruling. Move the "expected outcome" paragraph to "if greybeard rules META-FIX, record verbatim..." and let the actual subagent rule.

### m6. CLAUDE.md "Active campaigns" bullet is 25 lines (Task 16 Step 2, plan lines 2208-2234)

- Worktree CLAUDE.md discipline at "Memory & CLAUDE.md discipline" section: "Keep this file under 200 lines. If it grows past 200, extract to memory and link."
- M1's bullet is currently ~17 lines (verified in CLAUDE.md "Active campaigns" section). Adding 25 lines pushes total file growth past the soft cap. Should compress to ~10 lines following M1's style -- the 25-line draft reads like a commit message, not a CLAUDE.md bullet.
- Recommended fix: trim the M1.5 bullet to one-paragraph summary + spec/plan paths only; delete the per-task narrative.

---

## Strengths confirmed under grep

- The five sceneFBO_ glDrawBuffers sites are real, at the cited line numbers (off by 1-2), and the helper-routes-all-of-them claim is mechanically achievable. C1 META-FIX framing holds.
- PerDrawEntry layout at `GameOS/gameos/gos_static_prop_batcher.h:46-65` matches plan verbatim: `_pad0` at offset 24, `_pad1` at offset 28, struct size 32. Substitutive rename is structurally clean.
- shaders/static_prop.frag PerDrawEntry mirror at lines 37-46 also matches plan verbatim. The `_pad0` field appears exactly once in the shader.
- `Handle::raw()` at `RenderCore/Handle.h:53-55` exists, returns `uint32_t bits`. Layout `[19:0] index, [31:20] generation` matches all plan claims about translation.
- `StaticPropDesc::gameObjectId` exists at `RenderCore/RenderObjectDesc.h:63` as `uint32_t gameObjectId = 0;` matching plan Task 6 Step 4 (line 1016).
- No other shader file declares `layout(location=2) out` -- exhaustive grep `layout\(\s*location\s*=\s*2\s*\)\s*out` returns zero hits across all shaders. Plan's "uniqueness" gate (Phase D verification #6 / Pre-execution gate #9) is satisfiable.
- `RenderWorld::envFlag()` helper exists at `RenderWorld/RenderWorld.cpp:36-39`. Task 1 Step 3's `readObjectIdBufferEnv()` reusing it is mechanically sound.
- `legacy::getStaticPropActiveCount()` (the M1 m4 fix) is intact at `RenderWorld/RenderWorld.cpp:144` and Task 7's banner extension (Step 4, plan lines 1268-1284) preserves it.
- M1 firewall `scripts/check-include-firewall.sh` will pass: no new game-side dependencies are introduced.

---

## Decisions needing user sign-off before Task 1

These are issues the plan did NOT flag that the user needs to rule on:

1. **C1: Per-type handle accessor scope.** The plan assumes Task 10 is a 1-line edit; in reality it requires either a new `getRecipeIndexForType()` registry accessor OR a side-mapping in the batcher. This is its own 1-task addition. Decide before Task 1: (a) add accessor as Task 10a, (b) collapse into Task 10 with explicit scope acknowledgement, or (c) make M1.5 ship with `objectIdRaw=0` for all packets and defer producer fill to M1.6.

2. **C3 / Task 8-9-10 atomicity.** Spec says the rename is atomic. Plan splits across 3 commits with explicit RED-build allowance. Rule: collapse into one commit, OR accept the RED window as documented risk.

3. **M2 makeProgram shape.** Two program builds, two static `const char*` prefixes, plan's "prefix += " pattern does not compile. Rule whether to (a) rewrite prefixes as `std::string` accumulators (slight cost: programs rebuilt every restart instead of init-once), or (b) build a third runtime variant just for env-ON.

4. **M3 setSceneDrawBuffers attachment-existence vs env-flag.** Helper currently keyed on `IsObjectIdBufferEnabled()` only. Mid-resize cycles can produce env-ON-no-texture state. Decide: (a) helper additionally reads `sceneObjectIdTex_` (becomes member fn or takes ptr), (b) helper trusts the flag and accepts the mid-resize GL_INVALID_OPERATION as harmless, or (c) `createFBOs/destroyFBOs` are atomic enough that mid-resize beginScene cannot fire.

5. **M4 canary mission.** Confirm mc2_24 has both buildings AND trees visible at spawn camera. If not, swap to mc2_10 or another tier1 mission.

6. **O1 / plan-flagged accessor location.** Plan leans `RenderWorld/RenderWorld.h`. Lean accepted; no countervailing evidence. Surface for user ack only.

---

## Apply-before-Task-1 fix list (summary)

CRITICAL (block):
- C1: define producer-side handle source (registry accessor or batcher side-map).
- C2: collapse Tasks 5-6-7 into one Phase A close OR mark Task 5 commit "do-not-deploy-alone."
- C3: collapse Tasks 8-9-10 into one atomic commit per spec.

MAJOR (must fix Task 1-prep):
- M1: state producer-handle reconstruction formula explicitly in Task 10.
- M2: rewrite Task 11 Step 2 with actual two-program shape (`kShaderPrefixLegacy/Coalesce` are string literals, not std::string).
- M3: helper guards on attachment-texture existence, not just env flag.
- M4: grep-verify mc2_24 has mixed prop categories OR switch canary mission.

MINOR (defer to first commit or docs):
- m1: cited line numbers off by 1-2; update on first task touching each site.
- m2: change `g_pp` to `getGosPostProcess()` in Task 7 Step 3.
- m3: edit BOTH `layout(location=0) out` occurrences in static_prop.frag, not just one.
- m4: document debug-mode early-return semantics in slice CLAUDE.md bullet.
- m5: remove "Expected outcome: META-FIX" pre-judgment from Task 15.
- m6: compress Task 16 CLAUDE.md bullet to ~10 lines matching M1 style.

---

End of review.
