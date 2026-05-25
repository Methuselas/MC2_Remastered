# Overlay-pz v2 — Bit-Identity-by-Construction Proof (Step-8b-unblock evidence)

Status: PROVEN. HEAD 206990f (overlay-pz v2). mc2-terrain-indirect-expert,
2026-05-15. Substitutes for the runtime MC2_M2D_PZ_PARITY probe (which is
runtime-broken; control experiment proved env delivery fine + probe silent =
2nd-failure code bug, decoupled from the critical path as a deferred cleanup).

## Why a proof instead of the probe
v1 (29ae435) reverted for a ~71% perf regression. v2 (0ce2737/206990f) is
perf-clean (147fps, lazy pzNeeded scope, regression provably cannot recur) but
its bit-parity probe never fired. Control: MC2_VPL_CULL fired + MC2_M2D_PZ_PARITY
silent in the SAME run => env delivery fine, v2 probe runtime-broken. Rather
than a 3rd probe attempt against a path tier1 may not exercise, the user ruled
"code-proof pivot": prove bit-identity by construction.

## The four claims (all PROVEN, grep-verified at 206990f)

1. Identical-projection: v2 (quad.cpp:2149-2152) and both VPL pz-write sites
   (terrain.cpp:1567-1571 slim, :1731-1738 legacy) construct
   Vector3D(.vx,.vy,.pVertex->elevation) from the same Vertex* and call the
   identical eye->projectForTerrainAdmission. No offset/rotation/OVERLAY_ELEV_OFFSET
   in the admission input. PROVE.
2. Sentinel-co-decision (load-bearing): in BOTH paths, pz=-0.5f is written ONLY
   in the else of if(onScreen) (terrain.cpp:1577 slim / :1749 legacy); for the
   perspective renderer clipInfo=onScreen (:1582 / :1757-1759). So
   pz==-0.5 <=> onScreen==false <=> clipInfo==0, single branch, no divergence.
   Old code: pz=-0.5 -> pz_adj=-0.498 -> pzc=false. v2 guard
   if(clipInfo==0){pzc=false;continue;} -> pzc=false. Bit-identical. PROVE.
3. Purity/call-order: projectZ pure, frame-stable (worldToClip built once/frame);
   setInverseProject writes 4 fields projectZ never reads. VPL-loop vs
   quad.cpp-draw same-frame -> osp.z==screenPos.z. PROVE.
4. Boundary-math-identical: same TERRAIN_DEPTH_FUDGE (0.002f, quad.cpp:1973),
   same +, same >=0 && <1. Only operand differs (osp.z vs vertices[c]->pz);
   by 1+3 equal on-screen, by 2 equal off-screen. PROVE.

## Synthesis verdict
BIT-IDENTICAL-BY-CONSTRUCTION PROVEN. pzc[]/pzTri1/pzTri2/the !pzTri cull/the
entire M2d decal-emission set are bit-identical to pre-precursor, independent of
mission/probe coverage. v2 SHIPS; probe demoted-not-deleted (gated for first
M2d-exercising mod/non-default run, asserts Renderer!=3 at arm). Step 8b's
cv->pz deletion (terrain.cpp:1601/:1738) is SAFE.

## Residual (LOW, scoped-out)
Renderer==3 legacy path: clipInfo=inView while sentinel stays on onScreen ->
the clipInfo==0<=>pz==-0.5 equivalence holds only for Renderer!=3. The
TerrainPatchStream fast path runs ONLY the perspective GL renderer
(Environment.Renderer!=3), so the proof is airtight for the only renderer this
code executes under. The demoted probe logs Renderer!=3 at arm so a future
legacy-renderer reintroduction cannot silently invalidate the claim.
