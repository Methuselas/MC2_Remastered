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

## 2. The change (Strategy C - pure-delete; consumer stubbed)

**User-clarified context 2026-05-19:** the TacMap minimap viewport-rect
trapezoid is BROKEN today ("camera vertices are way off") and the user
authorized "kill it and reimplement later." So this effort does NOT add
a new plane-unproject helper. The broken consumer is removed; a future
separately-chartered effort reimplements the minimap viewport-rect with
correct visual/product requirements. This eliminates the reviewer's
underspecified "ground/water plane" must-fix by deleting the helper
from scope entirely.

**Stub the consumer FIRST** (compile-safe sequencing - all later
deletions become safe once nothing reads the chain):
- Delete the entire broken viewport-rect block in `GameTacMap::render`
  - `code/gametacmap.cpp:212-276` (the `// this is the little viewing
  rect` comment through the trapezoid's `gos_DrawQuads(&corners[0],
  4)`). This removes the 4 `inverseProjectForPicking` calls + the 4
  `worldToTacMap` calls + the trapezoid setup + draw, surgically.
- The minimap base bitmap (`GameTacMap::render` ~128-170, the
  `gos_DrawTriangles` calls for the underlying minimap) and the
  mover/team rendering (~278+) are NOT touched - they are independent
  and stay intact. The minimap still renders; it simply no longer
  draws the (currently-broken) viewport-rect overlay.

**Then delete the orphaned chain** (each commit compiles + bisectable;
opposite-direction grep before each deletion):
- `Camera::inverseProjectForPicking` (camera.h ~634-636) +
  `Camera::inverseProjectZ` (camera.cpp ~1941, decl camera.h:626
  `[[deprecated]]`). Confirm no remaining callers exist (the gametacmap
  block was the sole live one).
- `Camera::setInverseProject` (camera.h:1115) + the 4 scalar member
  fields `startZInverse/startWInverse/zPerPixel/wPerPixel`
  (camera.h:141-144). Confirm no remaining readers (the
  inverseProjectZ body was the sole reader).
- The `setInverseProject` call site at terrain.cpp:2119 + the
  `yzRange/ywRange` computation (~terrain.cpp:2051-2055) + the
  `slimReduce` RED reduction writing the 6-tuple (terrain.cpp
  ~1878-1898).
- The `quad.cpp` water-block 6-tuple reduction writers in the
  `CostSplitWaterVertProjScope` block (~quad.cpp:1101-1287). The
  Slice-2 DIVERGENT-water nemesis becomes MOOT here (no reduction ->
  nothing to diverge; no probe needed).
- The file-scope `extern float leastZ/leastW/mostZ/mostW/leastWY/mostWY`
  declarations (~quad.cpp:540-545) and their definition site, once no
  writers/readers remain.

`worldToTacMap`: keep if any other caller exists; otherwise delete in
the gametacmap commit (opposite-direction grep at plan time).

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
- TacMap MINIMAP STILL RENDERS (base bitmap + mover/team markers) -
  USER-confirmed; the only intentional behavioral change is the
  viewport-rect trapezoid no longer drawn (it was broken anyway -
  user-acknowledged). NO crash, no garbage corners, no unrelated UI
  regression. Per reviewer's parity-reframe: this is NOT pixel-identical
  reproduction; it is "stable + plausible behavior" - here, "stub
  renders no footprint, minimap otherwise unchanged." A future
  separately-chartered minimap-reimplementation effort restores the
  viewport-rect with a correct geometric inverse and clear product
  requirements.
- Reject capped-FPS / per-quad-chrono / cost-split absolutes
  (`memory/cost_split_instrumentation_is_observer_effect_dominated.md`).

## 5. Risk + discipline

Risk LOW and contained: the ONLY behavioral surface is the
already-broken TacMap viewport-rect (user-acknowledged); cursor/picking/
camera were verified non-consumers. But it touches camera
inverse-projection + is campaign-adjacent, and this session hit 6
clean-model-vs-code collisions - so full discipline:
- Spec -> adversarial-plan-review -> writing-plans -> subagent execution
  with atomic commits -> the substitutive Tracy + USER minimap-still-
  renders smoke gate. Each commit is bisectable AND compile-safe.
- `Camera::inverseProjectZ` is `[[deprecated]]`; confirm no external/
  mod ABI depends on it ([[stock-install-must-remain-playable]]).

### Mandatory grep checklist (reviewer guardrail; the implementer MUST run this BEFORE each deletion commit)

For each symbol about to be deleted, grep across ALL of code/ mclib/
GameOS/ shaders/ data/ docs/ - covering **declarations, definitions,
inline wrappers, member-field access, comments, external/mod headers**,
NOT just call sites. Bare-identifier `this->` member access is invisible
to `->member` patterns (this session's recurring trap); use both
patterns. Symbols to sweep, in order:
1. `inverseProjectForPicking` (camera.h:634-636 inline + 4 gametacmap
   call sites - confirm those 4 are the sole live callers at HEAD).
2. `inverseProjectZ` (camera.cpp:1941 def + camera.h:626 `[[deprecated]]`
   decl + the inverseProjectForPicking inline call - confirm no other
   readers; also grep for the symbol in comments/docs to update).
3. `setInverseProject` (camera.h:1115 def + terrain.cpp:2119 call -
   confirm sole call site).
4. The 4 scalar member fields: `startZInverse / startWInverse /
   zPerPixel / wPerPixel` (camera.h:141-144). Opposite-direction grep
   bare-identifier reads (NOT just `->startZInverse` - inside Camera
   member functions they're bare). Confirm only `inverseProjectZ` body
   reads them.
5. The 6 file-scope globals `leastZ / leastW / mostZ / mostW / leastWY /
   mostWY` (quad.cpp:540-545 decls). Opposite-direction grep across the
   tree: writers (slimReduce + the water block) and readers (terrain.cpp
   :2051-2055 yzRange/ywRange + :2119 setInverseProject call). Confirm
   no third site. If any unexpected reader/writer exists, STOP and
   escalate (premise change = 7th-collision guard).
6. `worldToTacMap` - confirm caller set; if the 4 gametacmap lines are
   sole callers, include its deletion in the same commit.

### Compile-safe deletion sequence (reviewer guardrail; no commit may leave dangling calls or dead fields still required by later code)

**Phase 1 (consumer stub):** Delete gametacmap.cpp:212-276 (the broken
viewport-rect block). The chain becomes orphaned-but-still-compiles.
Build + smoke + USER confirms minimap still renders (base + movers; no
viewport rect). Commit.

**Phase 2 (Camera inverse-proj inlines):** Delete
`Camera::inverseProjectForPicking` + `Camera::inverseProjectZ` (decls
camera.h:626/634-636, def camera.cpp:1941-...). Build + smoke. Commit.

**Phase 3 (Camera state):** Delete `Camera::setInverseProject` +
`startZInverse/startWInverse/zPerPixel/wPerPixel` member fields
(camera.h:141-144/1115-1121). Build + smoke. Commit.

**Phase 4 (terrain producer):** Delete the `setInverseProject` call
(terrain.cpp:2119) + the `yzRange/ywRange` computation (~2051-2055) +
the slimReduce RED reduction (~1878-1898). Build + smoke. Commit.

**Phase 5 (water producer + globals):** Delete the `quad.cpp` water-
block 6-tuple reduction writers (~1101-1287 inside the
`CostSplitWaterVertProjScope` block - the 6-global writes only; preserve
the other water-block residue per Slice-2 spec's residue rules) + the
6 file-scope `extern float` decls (~quad.cpp:540-545). Build + smoke +
USER worst-case zoomed-out-big-map Tracy = the substitutive proof
(slimReduce zone shrinks by the RED reduction's share; quadSetupTextures
shrinks by the water-block reduction's share; no displaced cost). Commit.

Invariant on every commit: builds clean, runs the 20s 1-2-mission
`--keep-logs` smoke, no `GL_INVALID_*`, USER confirms minimap renders.
Any phase that breaks any invariant: STOP, do not chain forward.

## 6. Verification items

1. Re-pin all file:line at plan time (drift; bare-identifier reads).
2. Run the §5 grep checklist before each phase. The 4 gametacmap corners
   being the sole live consumer at HEAD is the make-or-break / 7th-
   collision guard.
