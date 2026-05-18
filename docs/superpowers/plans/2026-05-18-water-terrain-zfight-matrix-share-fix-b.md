# Zoom Z-Fight Fix B (Matrix-Share + Two-Constant Bias) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. This is a load-bearing depth-pipeline change in an isolated worktree; per-task spec+quality review is mandatory (project discipline overrides any lighter default).

**Goal:** Eliminate the zoom-step depth pop / shoreline z-fight for GPU water AND live decals/cement by binding them to the exact matrix terrain projects from (symmetric mirror) and replacing the divergent per-consumer screen-z fudge with two single-sourced, oppositely-signed co-planar constants.

**Architecture:** Authoritative spec `docs/superpowers/specs/2026-05-18-AUTHORITATIVE-zfight-matrix-share-design.md` — **Section 12 is operative** (supersedes Sections 3/4 and the edit-site/probe text of 11). Re-adversarial #2 passed (opus+sonnet APPROVE-WITH-REQUIRED-EDITS); the CRITICAL two-constant edit, the user-approved Fa scope (all 3 overlay/decal callers take the mirror), the Q1 wording, and the Invariant-A re-characterization are folded into Section 12. CPU water + Sym1 are explicitly OUT.

**Tech Stack:** C++ (GameOS renderer), GLSL 4.30 vertex shaders, lockstep `.hglsl`/`.h` constant header pair, env-gated FNV probes, kill-aware mc2_01 smoke + USER visual gate.

**Verification model (project-mandated, not pytest):** each code task ends with a build-free static check (grep/compile-shape) where possible; the *behavioural* gates are the env-gated probes (`[DEPTH_TRANSITION v1]` already wired `f886b6c`; new `MC2_WATER_RENDERPROBE`) asserted in the Task 8 smoke, then the Task 9 USER visual gate. Do NOT invent unit tests.

**Isolation (load-bearing):** worktree `A:/Games/mc2-opengl-src/.claude/worktrees/water-material-v1/`, branch `claude/water-material-v1`. Deploy ONLY to `A:/Games/mc2-opengl/mc2-win64-water/`. NEVER `mc2-win64-v0.4` (concurrent priority session). No emoji in any file. Full relink before deploy (load-bearing C++).

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `shaders/include/terrain_depth_bias.hglsl` | GLSL side of the lockstep bias constants | Add `OVERLAY_DEPTH_FUDGE`; set the post-matrix-share water constant; update rationale comment |
| `mclib/terrain_depth_bias.h` | C++ side of the lockstep bias constants | Byte-equal mirror of the above (same commit) |
| `shaders/gos_terrain_water_fast_mdi.vert` | MDI GPU water VS | Point its water bias at the post-matrix-share water constant |
| `shaders/gos_terrain_water_fast.vert` | non-MDI GPU water VS | Same |
| `shaders/terrain_overlay.vert` | decal/cement/overlay VS (shared by overlayProg_ + decalProg_) | Add `OVERLAY_DEPTH_FUDGE` to `px.z` (replaces the removed host polygon-offset) |
| `GameOS/gameos/gameos_graphics.cpp` | renderer: water binds, overlay/decal uniform helper + 3 callers, polygon-offset sites | Symmetric-mirror at 2 water binds + per-caller arg threaded through 3 overlay/decal callers; remove 3 `glPolygonOffset(-1,-1)` |
| `GameOS/gameos/gos_terrain_water_stream.cpp` | water cull-feed (canonical mirror pattern lives here) + new render-path probe | Add `MC2_WATER_RENDERPROBE` Invariant A (tripwire) + B (release gate) |

Stable anchors are given as exact strings. **Rule 0: grep the anchor, do NOT trust any `:NNNN` — lines drift.**

---

## Task 0: Rule-0 re-grep gate (spec Section 6 V1-V10 + 12.6 V7-V10)

**Files:** none modified — this is the plan-stage grounding gate. Output a short ground-truth note the later tasks consume.

- [ ] **Step 1: Re-grep every anchor and record current line + surrounding shape**

Run (worktree root `A:/Games/mc2-opengl-src/.claude/worktrees/water-material-v1/`):

```
grep -n 'setMat4Direct("terrainMVP"' GameOS/gameos/gameos_graphics.cpp          # site 1 non-MDI water
grep -n 'setMMat4Direct("terrainMVP"' GameOS/gameos/gameos_graphics.cpp         # site 2 MDI water
grep -n 'const bool mdiValid' GameOS/gameos/gameos_graphics.cpp                  # site-2 arming context
grep -n 'void gosRenderer::uploadOverlayUniforms_' GameOS/gameos/gameos_graphics.cpp
grep -n 'uploadOverlayUniforms_(' GameOS/gameos/gameos_graphics.cpp             # MUST be exactly: decl + 3 callers
grep -n 'glPolygonOffset(-1.0f, -1.0f)' GameOS/gameos/gameos_graphics.cpp       # MUST be exactly 3, all decal-family
grep -n 'WATER_DEPTH_FUDGE_FAST' shaders/gos_terrain_water_fast_mdi.vert shaders/gos_terrain_water_fast.vert
grep -n 'px.z' shaders/terrain_overlay.vert
grep -n 'IsFrameSolidArmed\|getDispatchMvp16\|gos_GetTerrainMVPMat4' GameOS/gameos/gos_terrain_water_stream.cpp | head
grep -n 'getTerrainMVP\|terrain_mvp_valid_' GameOS/gameos/gameos_graphics.cpp | head
```

- [ ] **Step 2: Assert the invariants the design depends on**

Confirm and write down PASS/FAIL for each:
- `uploadOverlayUniforms_` has exactly **one decl + 3 call sites** (`drawTerrainOverlays`, `drawDecalStaticBatch`, `drawDecals`); no 4th. (V7)
- `drawDecalStaticBatch` and `drawTerrainOverlays` both pass `overlayProg_->shp_, overlayLocs_`; `drawDecals` passes `decalProg_->shp_, decalLocs_`. (V7 — confirms per-locs dispatch impossible; per-caller arg mandatory.)
- Exactly **3** `glPolygonOffset(-1.0f, -1.0f)` sites, all inside the three overlay/decal functions (none in terrain/water/shadow paths). (V3/M3)
- The canonical symmetric-mirror expression already exists at the water cull-feed in `gos_terrain_water_stream.cpp` (`IsFrameSolidArmed() ? gos_terrain_indirect_getDispatchMvp16() : gos_GetTerrainMVPMat4()` with `if(!mvp) mvp=gos_GetTerrainMVPMat4()` safety). This is the pattern to copy verbatim. (V1)
- `getTerrainMVP()` returns `terrain_mvp_`; `gos_GetTerrainMVPMat4()` returns `&terrain_mvp_` (same storage the live overlay callers upload today → un-armed Fa arm is byte-identical). (V10/Q3)
- The `gamecam.cpp` "AW^T·(vx,vy,elev,1) = projectZ(...) exactly" comment is still present (Q1 un-armed co-planarity rests on it). Run: `grep -n 'projectZ' code/gamecam.cpp | head`. (V10)
- `gos_terrain_indirect` `getDispatchMvp16()` is populated when armed and documented "otherwise stale". Run: `grep -n 'getDispatchMvp16\|otherwise stale\|s_frameSolidArmed' GameOS/gameos/gos_terrain_indirect.cpp | head`. (V2)

- [ ] **Step 3: Commit the grounding note**

```bash
git add docs/superpowers/plans/2026-05-18-water-terrain-zfight-matrix-share-fix-b.md
git commit -m "docs: Fix B plan-stage Rule-0 grounding gate (V1-V10)"
```

If any Step-2 assertion FAILS, STOP and report — the spec's grounded premises drifted; do not proceed to code.

---

## Task 1: Lockstep two-constant header pair (the CRITICAL fold)

**Files:**
- Modify: `shaders/include/terrain_depth_bias.hglsl`
- Modify: `mclib/terrain_depth_bias.h`

**Constraint (spec 12.1 + the header's own hard bound):** `TERRAIN_DEPTH_FUDGE=0.002` UNCHANGED (terrain VS is the reference, never edited). The water constant must be `> 0.002` and `< 0.004` (loses the shoreline LEQUAL tie → terrain occludes water at coast; never `>=0.002` *delta* or lakebed punch-through). The overlay constant must be `>= 0` and `< 0.002` (wins the tie → decals/cement render over terrain, replacing the removed `glPolygonOffset(-1,-1)`). CPU-raster `WATER_DEPTH_FUDGE_RASTER` stays untouched (out-of-scope CPU-water path). **Exact magnitudes are decided by Step 1, not guessed.**

- [ ] **Step 1: Get the two magnitudes from mc2-shader-expert + /mc2-amd-shader-review (spec V6/V9)**

Dispatch the `mc2-shader-expert` advisor (and run `/mc2-amd-shader-review` discipline) with this exact question, code-grounded: *"Post matrix-share, GPU water + decals project through the bit-identical baked terrain MVP as terrain. In the `glClipControl(ZERO_TO_ONE)` [0,1] forward-Z LEQUAL regime with depth-write ON and draw order terrain→decals→water: (a) the smallest reliable post-divide screen-z constant `> TERRAIN_DEPTH_FUDGE (0.002)` that makes water deterministically LOSE the shoreline coast tie (preserve the `gos_terrain_water_fast.vert` scar invariant ~lines 322-368) yet keeps the residual world-depth gap sub-resolvable across the full zoom sweep; (b) the constant in `[0, 0.002)` (candidate 0.0 vs a small epsilon) for `terrain_overlay.vert` that makes decals/cement deterministically WIN the tie over terrain, equivalent to the removed `glPolygonOffset(-1,-1)`, with no shimmer. Give exact float values."* Record the returned `WATER` and `OVERLAY` values; if the expert says water can safely drop to a smaller value than the legacy 0.003, use the expert's value (the legacy 0.003 was the divergent-projection era; matrix-share changes the calculus).

- [ ] **Step 2: Edit the GLSL header** `shaders/include/terrain_depth_bias.hglsl`

Replace the file body's constant block + rationale so it reads (substitute `<WATER_VAL>` / `<OVERLAY_VAL>` from Step 1; keep RASTER as-is):

```glsl
// GLSL sibling of mclib/terrain_depth_bias.h -- keep LOCKSTEP (byte-equal
// values, same commit). Full rationale in that C++ header.
//   TERRAIN              = 0.002   (terrain VS, reference, UNCHANGED)
//   WATER_DEPTH_FUDGE_FAST = <WATER_VAL>  (gos_terrain_water_fast.vert/_mdi.vert)
//                          post matrix-share: a co-planar epsilon on the
//                          SHARED baked MVP (NOT a divergence compensator);
//                          > TERRAIN so water loses the shoreline LEQUAL tie.
//   OVERLAY_DEPTH_FUDGE  = <OVERLAY_VAL>  (terrain_overlay.vert) < TERRAIN so
//                          decals/cement win the tie over terrain; replaces
//                          the removed host glPolygonOffset(-1,-1).
//   WATER_DEPTH_FUDGE_RASTER = 0.0025  (CPU raster path; OUT OF SCOPE here)
#ifndef TERRAIN_DEPTH_BIAS_HGLSL
#define TERRAIN_DEPTH_BIAS_HGLSL
const float TERRAIN_DEPTH_FUDGE      = 0.002;
const float WATER_DEPTH_FUDGE_FAST   = <WATER_VAL>;
const float OVERLAY_DEPTH_FUDGE      = <OVERLAY_VAL>;
const float WATER_DEPTH_FUDGE_RASTER = 0.0025;
#endif
```

- [ ] **Step 3: Byte-mirror the C++ header** `mclib/terrain_depth_bias.h`

In the `namespace mc2depth` block, keep `TERRAIN_DEPTH_FUDGE`, `WATER_DEPTH_FUDGE_RASTER`, and the `WATER_DEPTH_FUDGE` RASTER back-compat alias UNCHANGED. Set `WATER_DEPTH_FUDGE_FAST = <WATER_VAL>f;` (drop the now-misleading `TERRAIN + DELTA` derivation if the expert chose a value not expressible as the old delta — make it a direct literal with a comment). Add `constexpr float OVERLAY_DEPTH_FUDGE = <OVERLAY_VAL>f;`. Update the file's top rationale comment to add the post-matrix-share explanation (co-planar epsilons on a shared projection; two oppositely-signed constants; `feedback_single_source_scattered_tuning_constants` — single-source the mechanism, regimes stay separate named values). Add a `static_assert(OVERLAY_DEPTH_FUDGE < TERRAIN_DEPTH_FUDGE && TERRAIN_DEPTH_FUDGE < WATER_DEPTH_FUDGE_FAST && WATER_DEPTH_FUDGE_FAST < 2.0f*TERRAIN_DEPTH_FUDGE, "Fix B depth ordering invariant");` (V8).

- [ ] **Step 4: Verify lockstep + ordering**

Run:
```
grep -n 'DEPTH_FUDGE\|DEPTH_BIAS' shaders/include/terrain_depth_bias.hglsl mclib/terrain_depth_bias.h
```
Confirm the three live constants (`TERRAIN`, `WATER_FAST`, `OVERLAY`) are byte-equal across both files and satisfy `OVERLAY < 0.002 < WATER_FAST < 0.004`.

- [ ] **Step 5: Commit**

```bash
git add shaders/include/terrain_depth_bias.hglsl mclib/terrain_depth_bias.h
git commit -m "feat: Fix B two-constant co-planar depth bias (lockstep header pair)"
```

---

## Task 2: Point both GPU water vertex shaders at the post-matrix-share water constant

**Files:**
- Modify: `shaders/gos_terrain_water_fast_mdi.vert`
- Modify: `shaders/gos_terrain_water_fast.vert`

The water VS already adds `WATER_DEPTH_FUDGE_FAST`. If Step 1 kept the same symbol name (recommended — minimal touch), these shaders need **no source change** beyond confirming they include the lockstep header and reference `WATER_DEPTH_FUDGE_FAST` (its value changed in Task 1). If the expert renamed the symbol, update the reference.

- [ ] **Step 1: Confirm the include + reference**

```
grep -n 'terrain_depth_bias\|WATER_DEPTH_FUDGE_FAST' shaders/gos_terrain_water_fast_mdi.vert shaders/gos_terrain_water_fast.vert
```
Expected: each `#include`s `include/terrain_depth_bias.hglsl` (or the project's include idiom) and applies `+ WATER_DEPTH_FUDGE_FAST` to the post-divide screen z. If the symbol is unchanged, no edit needed — the constant's new value flows through.

- [ ] **Step 2: (Only if the expert renamed the symbol) update the reference**

Replace `WATER_DEPTH_FUDGE_FAST` with the new symbol in both files. Do NOT change the surrounding screen-z math, the scar block (`gos_terrain_water_fast.vert` ~322-368), or `absW`/signed-w packaging (3 falsified attempts — memory `water_fastpath_interim_fixes_and_residuals`).

- [ ] **Step 3: Verify no other water-fudge drift**

```
grep -rn 'WATER_DEPTH_FUDGE\|0.003\|DEPTH_FUDGE_FAST' shaders/gos_terrain_water_fast*.vert
```
Confirm the only depth-bias term is the single shared constant; no stray literal.

- [ ] **Step 4: Commit (skip if Task 2 produced no file change)**

```bash
git add shaders/gos_terrain_water_fast_mdi.vert shaders/gos_terrain_water_fast.vert
git commit -m "feat: Fix B water VS uses post-matrix-share co-planar water constant"
```

---

## Task 3: Add the overlay constant to `terrain_overlay.vert` (replaces removed polygon-offset)

**Files:**
- Modify: `shaders/terrain_overlay.vert`

This VS is shared by `overlayProg_` (cement/overlays) AND `decalProg_` (bomb craters/footprints). Today `px.z = clip4.z * rhw` with NO bias; ordering came entirely from the host `glPolygonOffset(-1,-1)` (removed in Task 4). The overlay constant is now MANDATORY (spec 12.1 / M1 — without it the live overlay loses its ONLY depth ordering).

- [ ] **Step 1: Add the include + the bias term**

Find the screen-z line (`grep -n 'px.z' shaders/terrain_overlay.vert` — currently `px.z = clip4.z * rhw;`). Ensure the lockstep header is included (match the include idiom the water verts use). Change the term to:

```glsl
px.z = clip4.z * rhw + OVERLAY_DEPTH_FUDGE;
```

Do not change the matrix multiply, `clip4`, `rhw`, or any other component.

- [ ] **Step 2: Verify**

```
grep -n 'terrain_depth_bias\|OVERLAY_DEPTH_FUDGE\|px.z' shaders/terrain_overlay.vert
```
Expected: header included; exactly one `+ OVERLAY_DEPTH_FUDGE` on the `px.z` line.

- [ ] **Step 3: Commit**

```bash
git add shaders/terrain_overlay.vert
git commit -m "feat: Fix B overlay/decal VS co-planar bias (replaces host polygon-offset)"
```

---

## Task 4: `uploadOverlayUniforms_` per-caller MVP override + Fa (all 3 callers mirror) + remove 3 polygon-offsets

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (helper signature + decl, 3 call sites, 3 `glPolygonOffset` removals)

This is the user-approved Fa scope (spec 12.3): all three callers pass the symmetric-mirror so the **default-play** live decal/cement pop is actually fixed. The `nullptr` default is a safety no-op only — no caller uses it.

- [ ] **Step 1: Add the override parameter to the declaration**

Find `void uploadOverlayUniforms_(GLuint shp, const OverlayUniformLocs_& L, float elapsed);` (the in-class decl) and the out-of-line definition `void gosRenderer::uploadOverlayUniforms_(GLuint shp, const OverlayUniformLocs_& L, float elapsed)`. Add a trailing parameter to BOTH: `, const float* terrainMvpOverride = nullptr` (default only on the decl).

- [ ] **Step 2: Use the override in the helper body**

The current body is:

```cpp
    if (L.terrainMVP >= 0)
        glUniformMatrix4fv(L.terrainMVP, 1, GL_FALSE, (const float*)&getTerrainMVP());
```

Replace with (nullptr → unchanged live behaviour; non-null → the caller-supplied symmetric-mirror; GL_FALSE row-major preserved per `terrain_mvp_gl_false.md`):

```cpp
    if (L.terrainMVP >= 0) {
        const float* tmvp = terrainMvpOverride
                                ? terrainMvpOverride
                                : (const float*)&getTerrainMVP();
        glUniformMatrix4fv(L.terrainMVP, 1, GL_FALSE, tmvp);
    }
```

- [ ] **Step 3: Define a local symmetric-mirror helper at each of the 3 call sites**

The canonical expression (copy verbatim from `gos_terrain_water_stream.cpp`, the proven 926/0 pattern; include whatever header declares `gos_terrain_indirect::IsFrameSolidArmed` / `gos_terrain_indirect_getDispatchMvp16` / `gos_GetTerrainMVPMat4` — grep an existing user of those in `gameos_graphics.cpp` to confirm visibility):

```cpp
    const float* fixBMvp =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()
            : gos_GetTerrainMVPMat4();
    if (!fixBMvp) fixBMvp = gos_GetTerrainMVPMat4();  // safety: pre-arm/first frame
```

Insert this immediately before each of the three `uploadOverlayUniforms_(...)` calls and pass `fixBMvp` as the new last argument:
- in `drawTerrainOverlays` — call becomes `uploadOverlayUniforms_(overlayProg_->shp_, overlayLocs_, elapsed, fixBMvp);`
- in `drawDecalStaticBatch` — `uploadOverlayUniforms_(overlayProg_->shp_, overlayLocs_, elapsed, fixBMvp);`
- in `drawDecals` — `uploadOverlayUniforms_(decalProg_->shp_, decalLocs_, elapsed, fixBMvp);`

- [ ] **Step 4: Remove the 3 `glPolygonOffset(-1,-1)` (all decal-family, all-or-nothing same-PR)**

Delete the line `glPolygonOffset(-1.0f, -1.0f);` in each of `drawTerrainOverlays`, `drawDecalStaticBatch`, `drawDecals`. If a paired `glEnable(GL_POLYGON_OFFSET_FILL)` / `glDisable(...)` brackets it in any of the three, remove the now-dead enable/disable too (grep around each site; do not leave an orphan enable that biases nothing or a disable of an un-enabled state). Ordering is now carried by `OVERLAY_DEPTH_FUDGE` (Task 3).

- [ ] **Step 5: Verify**

```
grep -n 'uploadOverlayUniforms_\|terrainMvpOverride\|glPolygonOffset\|GL_POLYGON_OFFSET_FILL\|fixBMvp' GameOS/gameos/gameos_graphics.cpp
```
Expected: decl+def have the new param; exactly 3 `fixBMvp` blocks; 3 calls pass it; **zero** `glPolygonOffset(-1.0f, -1.0f)` remain; no orphan polygon-offset enable/disable.

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat: Fix B Fa - symmetric-mirror MVP for all 3 overlay/decal callers; drop polygon-offset"
```

---

## Task 5: Symmetric-mirror the two GPU-water `terrainMVP` binds (spec 11 sites 1+2)

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (non-MDI water bind + MDI water bind)

- [ ] **Step 1: Site 1 — non-MDI water (shared pre-amble of `renderWaterFastPath`)**

Find `setMat4Direct("terrainMVP",      (const float*)&terrain_mvp_);`. Immediately before it, add the same canonical mirror block (reuse a local name that doesn't collide in this scope, e.g. `wMvp`):

```cpp
    const float* wMvp =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()
            : gos_GetTerrainMVPMat4();
    if (!wMvp) wMvp = gos_GetTerrainMVPMat4();
```

Change the bind to `setMat4Direct("terrainMVP", wMvp);`.

- [ ] **Step 2: Site 2 — MDI water (inside `if (mdiValid)`)**

Find `setMMat4Direct("terrainMVP",      (const float*)&terrain_mvp_);`. Add the same mirror block immediately before (local name e.g. `wMvpMdi` to avoid shadowing), and change the bind to `setMMat4Direct("terrainMVP", wMvpMdi);`. Confirm `mdiValid` / `gpuArmed` context is unchanged (do not touch the arming logic — only the matrix source).

- [ ] **Step 3: Verify byte-shape**

```
grep -n 'setMat4Direct("terrainMVP"\|setMMat4Direct("terrainMVP"\|wMvp\|wMvpMdi' GameOS/gameos/gameos_graphics.cpp
```
Expected: both binds now take the mirror local, not `&terrain_mvp_`; the mirror blocks are byte-identical in structure to the 926/0 pattern and to Task 4's `fixBMvp`.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat: Fix B symmetric-mirror terrainMVP for non-MDI + MDI GPU water binds"
```

---

## Task 6: `MC2_WATER_RENDERPROBE` — Invariant A (tripwire) + Invariant B (release gate)

**Files:**
- Modify: `GameOS/gameos/gos_terrain_water_stream.cpp` (next to the existing `[WATER_DEPTHPROBE v1]` FNV idiom)

Per spec 12.4: Invariant A is a future-regression tripwire (currently trivially-true — single per-frame MVP writer); Invariant B (latched arming-transition frame: water/decal render-bind FP == terrain's this-frame source FP) is the actual release gate RenderDoc cannot catch. Env-gated, silent default, demote-not-delete (debug-instrumentation rule). Reuse the FNV-1a-over-first-12-floats idiom verbatim from the existing `[WATER_DEPTHPROBE v1]` block.

- [ ] **Step 1: Add the probe block**

Adjacent to the existing `s_waterDepthProbe` FNV code, add an `MC2_WATER_RENDERPROBE`-gated block that, per armed frame: computes FNV of the matrix actually uploaded at the render binds, compares to FNV of the cull-feed matrix (`[WATER_RENDERPROBE] invA equal=<0|1>` — Invariant A tripwire); and latches the first arming-transition frame (armed-state changed vs previous frame) and prints `[WATER_RENDERPROBE] invB transition frame=<f> water_fp=<...> terrain_src_fp=<...> equal=<0|1>` exactly once (Invariant B release gate). Use the existing `gos_terrain_indirect_getDispatchMvpFp()` / `IsFrameSolidArmed()` accessors; mirror the byte-layout of the v1 FNV (`2166136261u` seed, `16777619u` prime, 12 floats). Document in the comment that invA is a tripwire (trivially-true under the single `gamecam.cpp` per-frame `gos_SetTerrainMVP` writer) and invB on a captured transition frame is the release gate.

- [ ] **Step 2: Verify it is silent by default and compiles in shape**

```
grep -n 'MC2_WATER_RENDERPROBE\|WATER_RENDERPROBE' GameOS/gameos/gos_terrain_water_stream.cpp
```
Expected: a single `static const bool ... = (getenv("MC2_WATER_RENDERPROBE") != nullptr);` gate; no output unless the env var is set; demote-not-delete comment present.

- [ ] **Step 3: Commit**

```bash
git add GameOS/gameos/gos_terrain_water_stream.cpp
git commit -m "feat: Fix B MC2_WATER_RENDERPROBE invA tripwire + invB arming-transition release gate"
```

---

## Task 7: Isolated full-relink build + deploy to mc2-win64-water ONLY

**Files:** none (build/deploy).

- [ ] **Step 1: Full relink (load-bearing C++ changed — inline/static linkage hazard)**

```
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/**/gameos_graphics*.obj build64/RelWithDebInfo/**/gos_terrain_water_stream*.obj
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```
ALWAYS `--config RelWithDebInfo` (Release crashes `GL_INVALID_ENUM`). Expected: clean link, `build64/RelWithDebInfo/mc2.exe` rebuilt. If shader hot-reload errors appear later, recall: bad compile = old shader silently stays — Task 8 must check the console for `0(N): error`.

- [ ] **Step 2: Deploy per-file to the ISOLATED water dir ONLY**

`cp -f` each changed artifact (exe + the 4 modified shaders) into `A:/Games/mc2-opengl/mc2-win64-water/` and `diff -q` each. NEVER `cp -r`. NEVER `mc2-win64-v0.4`. Verify the deployed shader files match the worktree byte-for-byte.

- [ ] **Step 3: Commit (no-op if nothing tracked changed) / note the deployed build hash**

Record the commit SHA deployed so the smoke artifact can be traced.

---

## Task 8: Kill-aware mc2_01 smoke + probe gates

**Files:** none (verification).

- [ ] **Step 1: Kill-aware single-mission smoke**

If a `*v0.4*` mc2.exe is running, WAIT — never `--kill-existing` against the concurrent priority session. Otherwise run the kill-aware mc2_01 30s smoke against the isolated water deploy with both probes enabled:

```
set MC2_DEPTH_TRANSITION_PROBE=1
set MC2_WATER_RENDERPROBE=1
set MC2_WATER_DEPTHPROBE=1
py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 30 --kill-existing
```
(Use the project's exact single-mission smoke invocation; `MC2_SMOKE_MODE=1` if `--mission` requires it per `smoke_autonomous_run_pattern`.)

- [ ] **Step 2: Assert the probe gates in `tests/smoke/artifacts/<latest>/mc2_01.*`**

PASS requires ALL:
- `[DEPTH_TRANSITION v1]`: `dz_gpuw` and `dz_decal` collapse to ~0 (or exactly the chosen `OVERLAY`/`WATER` constant delta) AND stay so across the zoom sweep INCLUDING the transition frame — NOT the pre-fix flat `+0.0010000` / `-0.0020001` with drifting `z_terr`.
- `[WATER_RENDERPROBE] invB`: on the captured arming-transition frame, `equal=1` (water/decal render-bind FP == terrain's this-frame source FP). This is the release gate.
- `[WATER_RENDERPROBE] invA`: `equal=1` every armed frame (tripwire — a `0` means a regression introduced a mid-frame MVP mutation).
- `[WATER_DEPTHPROBE v1]`: still `equal=1` (now trivially-true; a `0` means the MVP-consistency contract broke).
- Zero `0(N): error` shader-compile lines in the mc2_01 log (hot-reload silent-fail guard).
- Smoke exit 0.

- [ ] **Step 3: On any gate FAIL — stop, diagnose, do not advance**

Read the latest `ring_trace.log` / `mc2_01.log`. A `[DEPTH_TRANSITION v1]` still-flat-with-drift = the matrix-share didn't take (re-check Task 5 binds / the arming gate). `invB equal=0` on transition = the symmetric-mirror arms aren't matching terrain's regime that frame (re-check the ternary + un-armed `gos_GetTerrainMVPMat4()` arm). Fix root cause; do not silence the probe.

---

## Task 9: USER visual gate (the authority)

**Files:** none.

- [ ] **Step 1: Present the smoke for USER visual observation**

The user drives the smoke window live (smoke is USER-DRIVEN — do not ask them to re-run manually). State explicitly what to look for and what is EXPECTED to persist:
- **Must be GONE:** the zoom-step jump + shoreline z-fight for **GPU water AND decals/cement**, on in AND out zoom steps, at all zoom levels; decals still correctly OVER terrain at all zooms (polygon-offset removed → `OVERLAY_DEPTH_FUDGE` must hold the ordering); no deep-water lakebed punch-through; S1/S6/transparency water visually unchanged.
- **EXPECTED to persist (NOT failures):** the CPU-water (un-armed intro/deploy-pan) jump (separate ~50x projection-path sub-root, no `terrainMVP` to repoint — probe-proven OUT); Sym1 constant water-sits-low (separate `waterElevation` baseline).

- [ ] **Step 2: On USER approval — finishing**

This branch is ISOLATED; the user integrates separately (the cross-branch push is the user's explicit deliberate step — confirm scope, do NOT blind-merge an experimental branch). Update the living-record memory (`water_v2_s1_shipped_s3_blocked_reborn.md`) from "PLAN-READY" to "SHIPPED + user-approved" with the deployed commit SHA. On rejection: capture the first-hand visual evidence (it outranks any probe — recurred 3x this effort), re-ground, do not re-derive blindly.

---

## Self-Review (run against the spec)

- **Spec coverage:** §12.1 two-constant CRITICAL → Task 1+3 (+Task 2 ref). §12.3 Fa (all-3-callers mirror) → Task 4. §11 sites 1+2 water binds → Task 5. §12.4 Invariant A/B probe → Task 6. §11 site-4 polygon-offset removal → Task 4 Step 4. Q1 un-armed (V10) → Task 0 Step 2. Build/deploy/smoke/visual discipline (§7/§9) → Tasks 7-9. No spec requirement left unmapped; CPU water + Sym1 explicitly out (stated in Task 9 Step 1).
- **Placeholder scan:** the only deferred values are the two depth-bias magnitudes — these are *correctly* deferred to mc2-shader-expert per spec V6/V9 (Task 1 Step 1 makes the decision a concrete dispatched action with exact constraints, not a "TBD"). No other placeholders.
- **Type/symbol consistency:** the symmetric-mirror local is named distinctly per scope (`fixBMvp` Task 4, `wMvp`/`wMvpMdi` Task 5) to avoid shadowing; `terrainMvpOverride` is the single helper param name used in decl, def, and body; `OVERLAY_DEPTH_FUDGE` / `WATER_DEPTH_FUDGE_FAST` are the single constant names across `.hglsl`, `.h`, and all consuming `.vert`. Consistent.
