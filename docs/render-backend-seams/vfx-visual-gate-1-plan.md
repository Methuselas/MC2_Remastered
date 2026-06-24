# PLAN — VFX-VISUAL-GATE-1 (full scope)

Supersedes the tube-only promotion (`ede06c78`, partial). Meets the operator's
acceptance gates: 3-run stable, perceptual compare, oracle confirms tube coverage
AND additive-non-collapse, the BLEND-MODE-DISTINCTION trap caught by a checker,
honest ledger promotion. No game launch without asking.

## THE TRAP (must be caught)
billboard/mesh additive = SRC_ALPHA/ONE (`AdditiveSrcAlphaOne`); tube additive =
ONE/ONE (`AdditiveOneOne`). Registry rows 11-16 encode this correctly. But:
- `check-pipeline-desc.py` only checks the `blend` FIELD EXISTS (completeness), not
  its VALUE → does NOT catch a tube->billboard collapse. **Add a value checker.**
- `[PIPELINE_BIND]` trace (pipeline_binder.cpp:128) omits blend → runtime oracle
  can't confirm non-collapse. **Extend the trace to emit blend.**

## DELIVERABLES
### C++ (requires build)
1. `GameOS/gameos/pipeline_binder.cpp` — add `blend=%s` to the `[PIPELINE_BIND]`
   line (AdditiveOneOne / AdditiveSrcAlphaOne / AlphaBlend / ...). Runtime oracle
   for non-collapse. Default-OFF gate unchanged (MC2_PIPELINE_BIND_TRACE).

### Python (no build)
2. `scripts/check-vfx-blend-distinction.py` — parse `s_descs` rows; FAIL unless
   VfxTubeAdditive==AdditiveOneOne, Vfx{Billboard,Mesh}Additive==AdditiveSrcAlphaOne,
   Vfx{Tube,Billboard,Mesh}Alpha==AlphaBlend. The trap-catcher. Register in
   check-contracts.sh as `vfx_blend_distinction`.
3. `scripts/pipeline_visual_gate.py` vfx profile:
   - runs=3, warmup=1 (sim-freeze makes VFX deterministic → byte-stable; det=True
     already observed). Drop the runs=1 weakening.
   - assert all three additive rows bind (VfxTubeAdditive + VfxBillboardAdditive +
     VfxMeshAdditive binds>0).
   - parse the NEW blend field from [PIPELINE_BIND]; assert tube lines = AdditiveOneOne
     and billboard/mesh lines = AdditiveSrcAlphaOne (runtime non-collapse oracle).
   - perceptual compare via visual_compare.py + visual-tolerance-policy.json across
     the 3 runs.

### Build + deploy
4. Full build green (cold; CMAKE_PREFIX_PATH → nifty worktree 3rdparty). Deploy to
   `mc2-win64-v0.4` (operator-chosen). md5 deployed==built.

### Proof + ledger
5. ASK before launch. Run gate. All gates green → VFX proofStatus=VISUAL_PROVEN
   (oracle_coverage + perceptual_ab), evidence recorded honestly.

## ACCEPTANCE (operator)
full build green · gate-OFF unchanged · 3-run stable · warmup handled · fixture
deterministic · visual_compare passes · oracle confirms tube coverage · oracle
confirms additive not collapsed · no deploy-lease collision · no global kill ·
cursor restored · ledger honest. Checker MUST catch tube ONE/ONE vs billboard
SRC_ALPHA/ONE.

## STOP (not in scope)
new VFX art/particles, blend-mode rewrites, collapsing tube->billboard, weakening
the test to dodge nondeterminism, broad render-state refactors, launching unasked.
