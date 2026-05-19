# Inverse-Projection Consumer-Collapse - Design (approved scope)

Date: 2026-05-19. Worktree: `claude/nifty-mendeleev`, HEAD 9a2f67d.
Status: APPROVED scope (user: "Charter C alone, spec next"). This is the
honest meta-fix that the quadSetupTextures-retirement effort was groping
for. Strategy A (GPU-produce + readback) and Strategy B (analytic
camera/elevation-band bound) were both recon'd and KILLED (B:
oblique perspective-divide is non-monotone/pole-unbounded, terrain.cpp
:1629-1632 + camera.h:447-460 confirm). Inputs: two converged
fresh-context advisor recons + greybeard, all keystones grep-verified.
Re-pin every file:line at plan time.

## 1. The finding (verified)

The expensive per-frame per-vertex CPU terrain projection REDUCTION -
`slimReduce`'s `leastZ/mostZ/leastW/mostW/leastWY/mostWY` 6-tuple
(terrain.cpp ~1878-1898) PLUS the DIVERGENT `quad.cpp` water-block
re-projection reduction (~1101-1287) - exists SOLELY to feed
`Camera::setInverseProject` (called terrain.cpp:2119) -> the four
scalars `startZInverse/startWInverse/zPerPixel/wPerPixel`
(camera.h:1115-1120) -> `Camera::inverseProjectZ` (camera.cpp ~1941,
synthetic screen-Y-linear depth ramp ~1977-1986), which is
`[[deprecated]]` (camera.h:626).

**Sole live consumer (opposite-direction grep, all of code/ + mclib/):**
the 4 TacMap minimap viewport-footprint corners -
`gametacmap.cpp:225/232/239/246` `eye->inverseProjectForPicking(...)`
(each passes screen.z=screen.w=0 -> the synthetic-ramp branch).
NON-consumers (verified, correcting earlier recon):
`missiongui.cpp:765` uses the 2D `inverseProject` overload
(camera.cpp ~732; comment ~750-751 "never calls inverseProjectForPicking
/ inverseProjectZ"); `txmmgr.cpp:1887` does its OWN `clipToWorld`
inversion (~1902-1909). terrain.cpp:1549 itself annotates the reduction
`feeds dead inverseProjectZ`. Cursor->ground / unit picking NEVER
consume this (cross-ref `memory/mc2_selection_picking_model_water_
terrain_never_picked.md`).

## 2. The change (Strategy C - consumer collapse)

**Delete:**
- `Camera::setInverseProject` + the 4 scalars + `Camera::inverseProjectZ`
  (the deprecated synthetic-ramp) and its `inverseProjectForPicking`
  inline (camera.h ~634-636).
- The `slimReduce` RED reduction writing the 6-tuple (terrain.cpp
  ~1878-1898) and the `setInverseProject` call site (terrain.cpp:2119).
- The `quad.cpp` water-block 6-tuple reduction (the `CostSplitWaterVert
  ProjScope` writers of leastZ/mostZ/... ~1101-1287). The Slice-2
  DIVERGENT-water problem becomes MOOT (no reduction -> nothing to
  diverge; no probe needed).

**Repoint (same commit as the inverseProjectForPicking deletion - the 4
call sites must never dangle):** replace each of the 4
`eye->inverseProjectForPicking(nScreen, world)` calls at
gametacmap.cpp:225/232/239/246 with a call to the new geometric
`clipToWorld` plane-unproject helper against the ground/water plane.
Reuse / factor the EXISTING technique at `txmmgr.cpp:1902-1909` (it
already inverts NDC corners via `clipToWorld` with the Stuff->MC2
(-x,z,y) swizzle + the w<0 negate). Prefer extracting that into one
shared helper called by both txmmgr and the TacMap corners over
duplicating it. The result is geometrically MORE correct than the
regression-line ramp it replaces.

## 3. Scope boundary / carve-out (do NOT bundle)

The angular `onScreenR` object cull cascade (terrain.cpp ~1811 clipInfo
+ ~1813-1837 the objBlockInfo/objVertexActive set, gated by the pure
angular `onScreenR`, defs Terrain::setObjBlockActive/setObjVertexActive
~2268/2282) is OUT OF SCOPE. It is cheap (angular, zero projection),
separable (verified: produced by `onScreenR`, not the 6-tuple
projection), but it is consumed SAME-FRAME by the destroy-capable
`objmgr.cpp:2040` update loop (`if(!updateRet) MC2_DESTROY`) - a stale/
subset set destroys/leaks objects (mechs the canary, `memory/cull_gates
_are_load_bearing.md`). It is a separate, carefully-gated follow-on
slice (NEVER a naive N-1 readback). Bundling it would inject load-bearing
object-destruction risk into this otherwise low-risk decorative-consumer
change. Every bundling attempt this session failed; keep them apart.
Also untouched: the default-dead clipInfo->isTerrainQuadVisible->draw and
px/py/pz/pw raster paths (free, separate).

After C, what remains of the slimReduce/setupTextures per-frame walk is:
the cheap angular cull cascade (separate slice) + default-dead legacy.
The expensive projection reduction - the actual CPU cost - is gone.

## 4. Substitutive done-criterion (honest)

- The 6-tuple reduction (terrain.cpp ~1878-1898), `setInverseProject`,
  `inverseProjectZ`, and the `quad.cpp` water-block reduction are
  DELETED (not flagged/bypassed) - absent from the source and from a
  clean non-COST_SPLIT total-frame Tracy at WORST-CASE
  zoomed-out-big-map (tier1 default camera is structurally blind -
  `memory/zoomed_out_big_map...`). No displaced cost into draw /
  mission-load / the TacMap path.
- TacMap minimap viewport-footprint VISUAL PARITY across a zoom +
  pan + elevation-change sweep (USER-driven; the footprint trapezoid is
  the only behavioral surface and a wrong one is visually self-evident).
  The new plane-unproject should be equal-or-more-correct vs the old
  ramp; document any intentional difference.
- Reject capped-FPS / per-quad-chrono / cost-split absolutes
  (`memory/cost_split_instrumentation_is_observer_effect_dominated.md`).

## 5. Risk + discipline

Risk LOW and contained: the ONLY behavioral surface is the decorative
TacMap footprint; cursor/picking/camera were verified non-consumers.
But it touches camera inverse-projection + is campaign-adjacent, and
this session hit 6 clean-model-vs-code collisions - so full discipline:
- The implementer MUST re-run the opposite-direction grep of every
  reader of `inverseProjectZ`/`inverseProjectForPicking`/`setInverse
  Project`/the 4 scalars at HEAD and confirm the consumer set is EXACTLY
  the 4 gametacmap corners BEFORE deleting anything. If any other live
  consumer exists, STOP and escalate (premise change).
- Spec -> adversarial-plan-review -> writing-plans -> subagent execution
  with atomic commits -> the substitutive Tracy + USER TacMap visual
  gate. Each deletion is its own bisectable commit.
- `Camera::inverseProjectZ` is `[[deprecated]]`; confirm no external/
  mod ABI depends on it (grep + the stock-install-playable memory).

## 6. Verification items

1. Re-pin all file:line at plan time (drift; bare-identifier reads).
2. Confirm `gametacmap.cpp:225-246` is the sole live consumer at HEAD
   (the make-or-break; a 7th-collision guard).
3. Confirm the txmmgr `clipToWorld` plane-unproject is reusable for the
   TacMap corners (same clip space, same swizzle, same w<0 handling).
