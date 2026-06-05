# Angle-Dependent Shadow + Prop Disappear — Meta-Fix Recon (2026-06-05)

Status: recon + greybeard meta-fix proposal. No code. Cross-session coordination doc
(GPU-cull/M2b session + cook/model-override session share this bug family).

## Symptom
With the cook + M2a static-population merge (branch `claude/cook-m2a-merge`, exe deployed
to v0.4 `run_cooked_m2a.bat`): fast-rotate disappear is FIXED (M2a draws the whole static
store). But at **certain static camera angles**: (SS1) shadows present → (SS2, view barely
nudged) shadows gone → (SS3, rotated further) **all static props gone** (trees + buildings),
terrain + runway still render. User read: a **projection issue**; shadows + props share a
root cause; "is the unified view matrix on?".

## Verdict (short)
- **One bug FAMILY, two distinct defects.** Family = the engine's **D3D-pixel-homog ↔
  GL-NDC clip-space split-brain**: two projection conventions live side-by-side
  (`getWorldToClip`/`terrainMVP` = D3D, Y-down, `w<0` in front; `worldToClipGL` = GL-NDC,
  axis-swap baked, reverse-Z). The "correct only when frustum is centered, mirrors/vanishes
  at off-center pitch/yaw" signature is this family. Already bit shadow-lane, water-SH,
  water-reflection-clip.
- **Shadow vanish: PINPOINTED** (light-basis singularity, shadow-specific, not the matrix).
- **Prop vanish: NARROWED** — it is NOT the admission predicate (ruled out, see §4). It is
  in the GPU static-prop path; needs a one-run live diagnostic at the failing angle to nail.

## 1. Is the unified view matrix on? (direct answer)
| Thing | merge tree | nifty | consumed by the failing passes? |
|---|---|---|---|
| ViewUniforms (F1) UBO, `MC2_VIEW_UNIFORMS` | PRESENT, **default-ON** | same | **NO — only `static_prop.vert` (+opt-in mech) consume it.** Cull, shadow, terrain, water do NOT. |
| Shadow-projection fix (`worldToClipGL` frustum-corner unproject, `txmmgr.cpp:2285`) | PRESENT, ON | same (line offset) | yes (shadow corners) |
| EngineView multi-view | substrate only, `setCurrentView` store-only | same | no |

So: **F1 is on but a dormant substrate for exactly the passes that fail.** The cull + shadow
still ride the legacy `gos_GetTerrainMVPMat4()` cache / CPU `worldToClipGL`. The shadow seam
that FIX-1 addressed is fixed here — the remaining shadow vanish is a *different* defect (§3).
The merge tree is at **parity with nifty** on both (model-override fork didn't lose the shadow
fix, didn't advance F1).

## 2. The shared producer (so they CAN fail together)
`code/gamecam.cpp:183 gos_SetWorldToClipGL(eye->worldToClipGL())` is the single per-frame
producer; the `terrain_mvp_` cache it writes is inherited by CullUBO, mech-batcher,
static-prop-batcher, particle-bridge (`gos_GetTerrainMVPMat4()`). The GPU prop cull
(`gpu_cull_compute.cpp:839` → frustum UBO `viewProj`) and the shadow corner unproject
(`txmmgr.cpp:2285 Invert(worldToClipGL())`) both consume it. One matrix, two consumers — a
single defect would hit both. **But terrain renders correctly at the failing angle (SS3), and
terrain uses the same matrix → the matrix is not globally wrong; the defects are
consumer-specific.**

## 3. Shadow vanish — PINPOINTED (`gos_postprocess.cpp`)
NOT the matrix (FIX-1 is in). Two shadow-specific angle defects:
- **Light-basis Gram-Schmidt singularity (`:2506-2517`):** basis built as `right = sun × up`,
  `rx /= len`. Up-vector pick flips at `|fz|>0.9`. When the sun nears the chosen up-axis,
  `len → 0` → `right` blows up → light matrix degenerates → shadows vanish/garbage. Sun-angle
  driven; a small camera nudge that changes the cascade fit can tip it.
- **Frustum-corner loss collapsing the light AABB (`:2530`):** corners below `wz < -200`
  (underground) are dropped; at certain camera pitch enough valid corners are lost that the
  light-space AABB collapses → empty/degenerate shadow volume.
- **Surgical fix:** robust orthonormal basis (pick up-axis as the world axis least parallel to
  the sun, or use a quaternion/`max-component` scheme — never normalize a near-zero cross),
  and clamp/fallback the light AABB to a minimum extent when corners are scarce.

## 4. Prop vanish — what it is NOT, and the live diagnostic
**Ruled out:**
- **Admission predicate mismatch.** The default object-admission path is `projectZBypassMode`
  = **Bypass** → `projectModernClipGL` (GL-NDC) → `clipSpaceFrustumAdmitGL` (GL) —
  *correct* GL→GL pairing (`camera.h:602-603`, `object_admission_predicate.cpp:163`). The D3D
  `clipSpaceFrustumAdmit` path runs only under `Compare`/`Off`. So no caller is feeding GL clip
  into the D3D predicate by default.
- **Per-object `inView` cull.** Static props **bypass `inView`** when GPU props are on:
  `bdactor.cpp:1362/2313/2459 if (inView || g_useGpuStaticProps)`. So per-object visibility
  does not gate them.
- **GPU static cull count.** M2a draws `instanceCount = store.size()` (the whole persistent
  store, registered once at mission load) from its own buffer; `reinjectPersistentStatic`
  early-returns under the gate. So the GPU cull count does not gate the M2a static draw.

**So the prop vanish is in the GPU static-prop path despite M2a drawing all.** Remaining
candidates (need the failing-angle repro to disambiguate):
1. **Projection sends props off-screen at angle** — props drawn but `worldToClipGL` (via
   ViewUniforms in `static_prop.vert`) maps them outside NDC at this pitch, while terrain (its
   own MVP consumption) survives. Test: capture a known-onscreen prop's clip coords at the
   failing angle; if `|x|>w` or `z∉[0,w]` for a prop that's clearly in frame → projection.
2. **A near-plane / reverse-Z interaction at steep pitch** clipping the prop band.
3. **The M2a static draw not actually emitting `store.size()` in the merged build** (cook
   LOD/override changes to the store population). Test: `MC2_STATIC_POP_SPLIT_CMD_DIAG=1` at
   the failing angle — `storeTotal` should be constant regardless of camera; if it drops →
   the store itself is being gated.

**Live diagnostic recipe (one run, at the failing angle):**
`MC2_STATIC_POP_SPLIT=1 MC2_GPU_CULL_STATIC_FROZEN_RECORDS=1 MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE=1 MC2_STATIC_POP_SPLIT_CMD_DIAG=1 MC2_PROJECTZ_BYPASS_MODE=Compare`
→ rotate to the SS3 angle, watch (a) `[STATIC_POP_SPLIT_CMD] storeTotal=` (constant? → not the
store), (b) `[OBJECT_ADMISSION_PREDICATE v1] event=disagree` (D3D vs GL disagreement spike at
angle → the family is live on the object path even if not gating static props). Then capture
`worldToClipGL` + one prop's clip at that frame to settle projection-off-screen vs not.

## 5. The greybeard meta-fix
Project already named it: **P2-5 "one-owner-per-decision" — kill the
terrainMVP / projectZ / worldToClipGL split-brain.** Realized as:
> **One clip-space convention (GL-NDC, via ViewUniforms), owned in one place, consumed by
> EVERY pass — cull, shadow, terrain, water, mech — and collapse the dual
> `clipSpaceFrustumAdmit` (D3D) / `clipSpaceFrustumAdmitGL` (GL) predicates into one.**

This dissolves the whole family **by construction**: no caller can pick the wrong convention
because there is only one, and no pass can ride a stale legacy matrix because there is only one
owner (ViewUniforms). It is *finishing F1* (the unified-view effort that stalled at one
consumer). The named meta-fix principle: *"every time two systems know the same fact, create
one owner and make the other ask for it."* The two-matrix split is the canonical violation.

**Hazard:** F1 pass-migration is non-trivial — the mech consumer was attempted and **reverted**
(`mech_viewuniforms_ubo_not_bound_at_flush.md`); terrain's D3D path has load-bearing quirks
(`abs(clip.w)` repackage, `terrainMVP` uploaded `GL_FALSE`, the `pz∈[0,1)` gate in `quad.cpp`)
that must be reconciled, not "cleaned up". This is an arc, adversarial-review-gated, one pass at
a time, behind a compare oracle (`vu.worldToClipGL == legacy MVP ≤1e-5` already exists).

## 6. Recommendation
- **Targeted unblock (fast, surgical, do first):**
  - **Shadows:** harden the light-basis singularity + AABB-corner fallback in
    `gos_postprocess.cpp:2506-2530`. Kills SS2.
  - **Props:** run the §4 live diagnostic at the SS3 angle to nail projection-off-screen vs a
    store/draw gate, then the fix follows from which candidate it is. (Can't pre-nail without
    the interactive failing-angle frame.)
- **Meta-fix arc (durable, the real answer):** P2-5 — finish F1 (all passes consume the single
  ViewUniforms matrix) + collapse the dual predicate. Recon-first + `adversarial-plan-review`,
  one pass per slice, behind the existing compare oracle.

## 7. Cross-session coordination
- M2b (GPU-cull session, `claude/perf-gpucull-ownership`) is partially landed + fixing cull
  bugs — it reclaims the M2a draw-all perf via `visibleIds`. The frustum UBO it culls with is
  the SAME `worldToClipGL`; if the prop-vanish turns out to be projection-at-angle, **M2b's
  visibleIds cull would re-expose it** (cull says not-visible because the matrix is wrong at
  angle). So the projection family must be settled before M2b flips static to GPU-culled draw.
- This recon lives on nifty (canonical) so both sessions see it. The merge with M2a is on
  `claude/cook-m2a-merge` (`5ecb3dc2`), parity-clean, gated default-off.

## 8. Progress log — 2026-06-05 (arc-owner session)

### Step 1 — Shadow light-basis fix: SHIPPED (nifty `f78d7973`, doc `beb73420`)
`SHADOW-ROBUST-BASIS-1`, gate `MC2_SHADOW_ROBUST_BASIS` default-ON (`=0` kill).
`gos_postprocess.cpp` — both build{Static,Dynamic}LightMatrix now share
`mc2ComputeLightBasis()`:
- **Singularity guard:** KEEPS the legacy up-hint pick as primary (byte-identical
  for normal suns — no texel-grid perturbation of working cases); re-picks up as
  the world axis least parallel to the sun ONLY when the legacy cross degenerates
  (`len < 1e-3`). Kills §3 bullet-1.
- **AABB corner-scarcity fallback:** rebuild the light AABB from all 8 corners when
  `validCorners < 4` (legacy fell back only at `==0`). Kills §3 bullet-2 (the
  sliver-AABB mis-center that slides shadow coverage off-view).
- **Validation** (mc2_24, `--validate --enable shadows`, isolated junction deploy off
  v0.4 since v0.4 mc2.exe was live in another session): default-ON and `=0` both
  render shadows **gl-clean** (shader_errors=[], gl_errors=[], exit 0, 90 frames).
  Default-ON frame-1 light-space AABB matches the `=0` AABB to camera-jitter
  (~5 WU / 70000) → basis path unchanged for normal suns; scarcity fallback inert
  (validCorners=8 in the flythrough). **Interactive SS2-angle confirmation on the
  cook+M2a merge remains user-gated** — the validate flythrough camera cannot be
  steered to the failing angle headless.

### Step 2 — SS3 prop diagnostic: PARTIAL (headless ceiling reached)
Ran the §4 recipe on the **nifty** exe (`MC2_PROJECTZ_BYPASS_MODE=Compare`, mc2_24,
120 frames). Findings:
- The cull/store diagnostics (`storeTotal`, `projectz` disagree, `frustum_admit`)
  **emit only on the merge branch** (gated by `MC2_STATIC_POP_SPLIT` et al.) — nifty
  has no M2a, so they were absent. The full §4 recipe MUST run on `claude/cook-m2a-merge`.
- The one Compare artifact nifty does emit, `screenxy_screen_delta`, is striking:
  **`legacyScreen=(0,0)` for every world point** while `bypassScreen` (modern GL) gives
  real coords (world origin → ~`(803,-317)`). The legacy D3D screen projection is
  effectively **dead/degenerate** here; only the default Bypass=GL path projects
  correctly. Corroborates §4 ("default GL→GL correct") AND the family verdict: any pass
  still riding the legacy convention is catastrophically wrong → reinforces P2-5.
- No prop-vanish reproduced in the flythrough (SS3 angle not reached), 0 gl_errors.

**Still BLOCKED / next (unchanged sequencing — first commandment still holds):** the
authoritative SS3 diagnostic needs the merge branch built + an **interactive** session
steered to the failing angle (capture `storeTotal` constant?, `projectz` disagree spike?,
one prop's clip coords). Do NOT touch M2b / visibleIds / static-pop draw until that frame
+ the P2-5 plan exist. Step 1's shadow fix should be merged forward into
`claude/cook-m2a-merge` so the merge deploy has SS2 fixed before the SS3 repro session.
</content>
