# `glClipControl` Adoption Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Adopt `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` to align the
hardware depth convention with the engine's existing D3D-style projection
matrices. Recovers 2× depth-buffer precision (engine currently uses only
window-space `[0.5, 1.0]` of the available `[0, 1]` range), simplifies four
shader-side workaround remaps, and aligns Track A1 / Track C cull
predicates with hardware-native semantics.

**Architecture:** Single atomic change set. ONE CPU edit (engine init) +
FOUR shader edits, all in one commit. The shader edits remove
`* 0.5 + 0.5` and `* 2.0 - 1.0` workaround remaps that compensated for
the half-precision depth chain. Half-applied = broken rendering; the
five edits MUST land together.

**Tech Stack:** OpenGL 4.5 core (`GL_ARB_clip_control`, promoted to core in
4.5). AMD RX 7900 XTX target supports natively. Engine baseline is GL 4.3
core; this adoption requires the 4.5 feature but keeps the rest of the
4.3 substrate.

**Spec references:**
- Q20 in `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` (recon-zero pass complete; this plan ships the result)
- Recon findings: session 2026-05-06 chat transcript
- Camera model context: `memory/camera_model_oblique_cinematic.md` (oblique 30° + cinematic — depth precision matters more here than top-down RTS)

**Worktree CLAUDE.md rules in force:**
- Build: `cmake --build build64 --config RelWithDebInfo`
- Stock install must remain playable (atomic commit ensures correctness; no half-state ships)
- Tier1 smoke is the regression gate
- Documentation discipline: every cited symbol grep-verified at write-time

---

## Background guardrails (5 advisor sharpenings inherited from A1)

1. **Lazy-init env probe:** N/A — the migration is unconditional once shipped. **No env-flag killswitch.** A CPU-side-only killswitch is unsafe because the shader edits in Task 1 remove depth-range workarounds unconditionally; gating only the `glClipControl` call without also gating the shader code (via variants or conditional compilation, neither of which this plan ships) would produce broken rendering whenever the killswitch is set to "off." The plan's atomic-commit invariant — five edits land together or none do — IS the safety mechanism. The fail-closed branch in Step 1.2 enforces it at runtime.
2. **Hard-fail self-tests:** N/A — no predicate to selftest. The visual depth-precision canary in Task 2 IS the test.
3. **No drift between trace and production:** N/A — no parallel implementations. Single source of truth for depth convention is `glClipControl`'s state.
4. **Single-run captures for parity-data:** Task 2 takes before/after screenshots in one session, side-by-side comparison.
5. **Identity diff for DESTROY:** Task 3 inherits A1's `[DESTROY v1]` count + identity diff pattern.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `GameOS/gameos/gameos_graphics.cpp` | Modify | Engine init: add `glClipControl` call + extension probe + `[INSTR v1]` banner field |
| `shaders/gos_terrain.tese` | Modify | Line 94: remove `* 0.5 + 0.5` from `UndisplacedDepth` assignment |
| `shaders/gos_terrain_thin.vert` | Modify | Line 179: remove `* 0.5 + 0.5` from `UndisplacedDepth` assignment |
| `shaders/shadow_screen.frag` | Modify | Line 120: change `depth * 2.0 - 1.0` to `depth` (passthrough) |
| `shaders/ssao.frag` | Modify | Line 43: change `depth * 2.0 - 1.0` to `depth` (passthrough) |
| `~/.claude/projects/A--Games-mc2-opengl-src/memory/clip_control_adopted.md` | Create | Post-ship memory file documenting the adoption |
| `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` | Modify | Index entry |

**Decomposition rationale:** the five edits are atomic — half-applied breaks rendering. They land in one commit. Memory + index land in a separate commit after soak.

---

## Task 1 — The atomic five-edit commit

All five edits must land together. Build + tier1 5/5 verify before commit.

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (engine init site)
- Modify: `shaders/gos_terrain.tese:94`
- Modify: `shaders/gos_terrain_thin.vert:179`
- Modify: `shaders/shadow_screen.frag:120`
- Modify: `shaders/ssao.frag:43`

- [ ] **Step 1.1 — Locate engine init site**

```bash
grep -rn "glewInit\|gladLoadGL\|wglCreateContext\|SDL_GL_CreateContext" \
  GameOS/ code/ 2>/dev/null | head -10
```

Expected: a single primary site where the GL context is fully initialized. The `glClipControl` call goes immediately after that site (the call requires an active GL context).

Likely candidate: `GameOS/gameos/gameos_graphics.cpp` near `gosRenderer::init()` (which Tracy-zones at line ~2475). Verify by reading the file around that area.

- [ ] **Step 1.2 — Add `glClipControl` call with extension probe (fail-closed on unsupported)**

After the GL context init line and BEFORE the first draw, add:

```cpp
// GL_ARB_clip_control: align hardware depth convention with engine's
// existing D3D-style projection matrices. Engine produces clip-space
// [0, w] (per cameraToClip at mclib/camera.cpp:1942-1945); without
// clip control, hardware default expects [-w, w] and compresses our
// output into window depth [0.5, 1.0] — half precision wasted.
// With ZERO_TO_ONE, hardware natively expects [0, w], NDC z is [0, 1],
// window depth uses full [0, 1] range.
//
// **Fail-closed contract:** the four shader edits in this slice
// (gos_terrain.tese, gos_terrain_thin.vert, shadow_screen.frag,
// ssao.frag) remove their depth-range workaround remaps unconditionally.
// Running without glClipControl(GL_ZERO_TO_ONE) would feed [0,1] NDC z
// to hardware expecting [-1,1] and produce garbage depth. So if the
// extension is somehow unavailable at runtime, we MUST refuse to start
// rather than ship broken rendering.
//
// Plan: docs/superpowers/plans/2026-05-06-clip-control-adoption.md
if (GLEW_ARB_clip_control || GLEW_VERSION_4_5) {
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    printf("[INSTR v1] clip_control=enabled origin=lower_left depth=zero_to_one\n");
    fflush(stdout);
} else {
    // Should never happen on AMD RX 7900 XTX (4.5 is core-supported).
    // If it does (very old driver, software fallback context, etc.),
    // fail loud and immediate. Half-applied state is the failure mode
    // we MUST refuse to ship.
    printf("[INSTR v1] clip_control=unsupported fatal=1\n");
    fflush(stdout);
    gosASSERT(false);
    std::abort();
}
```

(Verify exact GLEW symbol names with `grep -nE "GLEW_ARB_clip_control|GLEW_VERSION_4_5|glClipControl" 3rdparty/include/GL/glew.h | head -5` — adjust if names differ. `gosASSERT` is the project-native fatal pattern, used at multiple sites including the Track A1 selftest hard-fail; verify it's reachable in the TU you're editing — likely already included.)

- [ ] **Step 1.3 — Edit `shaders/gos_terrain.tese:94`**

Locate the line:

```glsl
    UndisplacedDepth = (uclip.z / uclip.w) * 0.5 + 0.5;
```

Replace with:

```glsl
    // glClipControl(ZERO_TO_ONE) makes NDC z native [0, 1]; no remap needed.
    UndisplacedDepth = uclip.z / uclip.w;
```

- [ ] **Step 1.4 — Edit `shaders/gos_terrain_thin.vert:179`**

Locate the line:

```glsl
    UndisplacedDepth = screen.z * 0.5 + 0.5;
```

Replace with:

```glsl
    // glClipControl(ZERO_TO_ONE) makes screen.z (D3D-style [0, 1]) native;
    // matches gl_FragCoord.z range without remap.
    UndisplacedDepth = screen.z;
```

- [ ] **Step 1.5 — Edit `shaders/shadow_screen.frag:120`**

Locate the line inside `reconstructWorldPos`:

```glsl
    float ndc_z = depth * 2.0 - 1.0;
```

Replace with:

```glsl
    // glClipControl(ZERO_TO_ONE) makes window depth and NDC z share [0, 1];
    // pass through. inverseViewProj inverts the D3D-style matrix natively now.
    float ndc_z = depth;
```

- [ ] **Step 1.6 — Edit `shaders/ssao.frag:43`**

Same edit as 1.5 — locate:

```glsl
    float ndc_z = depth * 2.0 - 1.0;
```

Replace with:

```glsl
    // glClipControl(ZERO_TO_ONE) — see shadow_screen.frag:120 for rationale.
    float ndc_z = depth;
```

- [ ] **Step 1.7 — Build**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: clean build. Shader hot-reload would apply, but a fresh build is safer to confirm nothing else regressed.

- [ ] **Step 1.8 — Tier1 5/5 smoke verify**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: 5/5 PASS. `[INSTR v1] clip_control=enabled origin=lower_left depth=zero_to_one` appears in each mission's log header.

**If any mission fails:**
- Look for visual artifacts in mission screenshots (terrain z-fighting, shadow blackout, SSAO white-out).
- Inspect log for `[GL_ERROR v1]` lines.
- Likely cause: a shader edit was missed or applied incorrectly. Revert all five edits, re-apply, retry. Half-applied state is the failure mode this task structure exists to prevent.

**Do NOT commit until tier1 5/5 PASSES.** This is the atomic-commit gate.

- [ ] **Step 1.9 — Commit**

```bash
git add GameOS/gameos/gameos_graphics.cpp \
        shaders/gos_terrain.tese \
        shaders/gos_terrain_thin.vert \
        shaders/shadow_screen.frag \
        shaders/ssao.frag
git commit -m "feat(gl): adopt glClipControl(LOWER_LEFT, ZERO_TO_ONE) for D3D-native depth

The engine's cameraToClip matrix at mclib/camera.cpp:1942-1965 produces
D3D-style clip-space [0, w] by deliberate design. Without glClipControl,
GL hardware default expects [-w, w] and compresses our output into
window depth [0.5, 1.0] — half the depth buffer precision wasted.

Adopting glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) tells hardware
to expect [0, w] natively, recovering full [0, 1] window depth precision.
Removes four shader-side workaround remaps that compensated for the
half-precision chain (gos_terrain.tese, gos_terrain_thin.vert,
shadow_screen.frag, ssao.frag).

CPU matrices need no changes — they were already producing/inverting
D3D-style coordinates. Track A1 clipSpaceFrustumAdmit predicate and
Track C0/C1 compute cull tests are now hardware-native semantics.

Tier1 5/5 PASS verified before commit.

Plan: docs/superpowers/plans/2026-05-06-clip-control-adoption.md
"
```

---

## Task 2 — Visual depth-precision canary

The 2× precision recovery is the load-bearing win. This task verifies it visibly.

**Files:** none (visual verification only)

- [ ] **Step 2.1 — Capture before-baseline screenshots (pre-merge)**

If the commit hasn't merged yet, this step is "screenshot from a build at the parent commit." If already merged, skip — baseline is on the prior tagged build.

Suggested scenes:
- `mc2_24` at wolfman zoom — distant terrain z-fighting / shadow acne if any.
- `mc2_01` mission intro pan (cinematic low angle) — far-distance terrain detail z-stability.
- Any cinematic kill-cam scene with deep frustum.

Capture: in-game F12 screenshot (or whatever screenshot mechanism the engine has — verify via grep `screenshot\|saveImage\|captureFrame` if needed) at identical camera positions. Save to `/tmp/clip-control-before/`.

- [ ] **Step 2.2 — Capture after screenshots (post-merge)**

Same scenes, same camera positions, same lighting state. Save to `/tmp/clip-control-after/`.

- [ ] **Step 2.3 — Side-by-side compare**

Look for:
- **Reduced z-fighting** at far distances (terrain LOD seams, overlay-on-terrain).
- **Reduced shadow acne** on terrain at extreme depth (the `gos_postprocess.cpp` polygon-offset sites stop fighting half-precision).
- **No new artifacts** at near depths.
- **No depth-related regressions** in shadows / SSAO (the inverseViewProj-using passes).

If after-screenshots show the depth-precision improvement: clip control is delivering its win.

If after-screenshots show NO visible difference: still ship — the precision win is real even if not always visually obvious; future depth-sensitive features (HZB, better shadows) will benefit. Document "no visible difference at typical scenes; precision improvement confirmed via depth-buffer inspection if needed."

If after-screenshots show NEW artifacts: regression. Investigate. Likely candidate: a shader path missed in the edit pass. Re-grep for any `* 0.5 + 0.5` or `* 2.0 - 1.0` adjacent to depth handling that wasn't caught.

- [ ] **Step 2.4 — Document in commit message addendum or memory file**

If visual difference observed: note specific scenes / cameras where it's visible. Helps future reviewers verify the change is still doing its job after subsequent commits.

If no visible difference: note that explicitly. Precision improvement is below visual-detection threshold at typical scenes; confirmation via `glReadPixels(GL_DEPTH_COMPONENT)` if rigor required.

---

## Task 3 — `[DESTROY v1]` parity (defense-in-depth)

Clip control should not affect lifecycle gates — it's purely a depth-convention change. But A1's discipline says any rendering-touching commit gets DESTROY parity verification. Cheap insurance.

**Files:** none (verification only)

- [ ] **Step 3.1 — Capture baseline (pre-merge or last-known-good)**

```bash
git checkout <pre-merge-tag-or-prior-commit>
cmake --build build64 --config RelWithDebInfo --target mc2
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log | wc -l \
    > /tmp/clipctrl-destroy-baseline-${m}.count
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log \
    | sed -E 's/obj=0x[0-9a-fA-F]+/obj=PTR/; s/frame=[0-9]+/frame=N/' \
    > /tmp/clipctrl-destroy-baseline-${m}.norm
done
git checkout <clip-control-merge>
cmake --build build64 --config RelWithDebInfo --target mc2
```

- [ ] **Step 3.2 — Capture post-clip-control**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log | wc -l \
    > /tmp/clipctrl-destroy-after-${m}.count
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log \
    | sed -E 's/obj=0x[0-9a-fA-F]+/obj=PTR/; s/frame=[0-9]+/frame=N/' \
    > /tmp/clipctrl-destroy-after-${m}.norm
done
```

- [ ] **Step 3.3 — Diff**

```bash
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  diff /tmp/clipctrl-destroy-baseline-${m}.count /tmp/clipctrl-destroy-after-${m}.count \
    && echo "${m}: count match" \
    || echo "${m}: COUNT MISMATCH"
  diff /tmp/clipctrl-destroy-baseline-${m}.norm /tmp/clipctrl-destroy-after-${m}.norm \
    > /tmp/clipctrl-destroy-${m}.identity-diff
  if [ -s /tmp/clipctrl-destroy-${m}.identity-diff ]; then
    echo "${m}: IDENTITY DIFF — review"
    head -20 /tmp/clipctrl-destroy-${m}.identity-diff
  else
    echo "${m}: identity match"
  fi
done
```

Expected: every mission reports both "count match" and "identity match." Clip control should not perturb lifecycle.

If counts diverge: clip control is somehow affecting cull (theoretically possible if a far-clip-plane case shifts under the new convention). Investigate — likely indicates the depth precision change is exposing a previously-masked cull-edge case. Revert if blocking.

---

## Task 4 — Soak observation (≥3 days)

Clip control is a small change but it touches a load-bearing convention. Soak it before declaring done.

**Files:** none (passive observation)

- [ ] **Step 4.1 — Daily smoke for ≥3 days**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Run once per day for at least three days. Log results. Watch for:
- Non-deterministic z-fighting that wasn't visible before (clip control changes precision distribution; some near-far ratios may newly z-fight if marginal).
- Shadow / SSAO artifacts at unusual camera angles.
- Any `[GL_ERROR v1]` from the new state.

- [ ] **Step 4.2 — Cinematic / oblique-canary scenes**

Per `memory/camera_model_oblique_cinematic.md`, the oblique 30° + 360° + cinematic shape exercises depth precision more than top-down RTS would. Specifically test:
- Mission intro pan with low-angle camera moves (mc2_01 specifically).
- Wolfman zoom on mc2_24 (max zoom-out, deep frustum).
- Any kill-cam scenarios that pull the camera close to a mech and look outward.

Visual canary: terrain detail at far distances should look CLEANER (less z-fighting between detail layers) under clip control vs. baseline.

- [ ] **Step 4.3 — Decision: declare done or revert**

After 3+ days clean:
- Tier1 5/5 every day? → ✓
- DESTROY parity every day? → ✓
- No visual regressions in cinematic scenes? → ✓
- `[INSTR v1] clip_control=enabled` appears in every log? → ✓

**All four green:** proceed to Task 5.

**Any red:** investigate. The change is small enough to revert cleanly (single commit). If revert is needed, the predicate-already-correct work in Track A1 / C0 / C1 is unaffected — those plans remain shipped on the existing convention regardless of clip control state.

---

## Task 5 — Memory + index

**Files:**
- Create: `~/.claude/projects/A--Games-mc2-opengl-src/memory/clip_control_adopted.md`
- Modify: `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

- [ ] **Step 5.1 — Write memory file**

Path: `C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\clip_control_adopted.md`

```markdown
---
name: clip_control_adopted
description: glClipControl(LOWER_LEFT, ZERO_TO_ONE) shipped — engine now uses hardware-native D3D-style depth convention; recovers 2× depth precision; removes 4 shader-side workaround remaps
type: project
---

Adopted on <date>. The engine's cameraToClip matrix
(`mclib/camera.cpp:1942-1965`) was always D3D-style by deliberate design
(comment: "valid X, Y, and Z axis values when divided by W will all be
between 0 and 1"). Without glClipControl, GL hardware default expected
[-w, w] clip-space and compressed our [0, w] output into window depth
[0.5, 1.0] — half the depth buffer precision wasted.

Now: glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) at engine init makes
hardware natively expect [0, w]. Window depth uses full [0, 1] range.
2× depth precision recovered.

**Removed shader-side workaround remaps:**
- `gos_terrain.tese:94` — was `UndisplacedDepth = (uclip.z / uclip.w) * 0.5 + 0.5`, now passthrough.
- `gos_terrain_thin.vert:179` — same shape, same fix.
- `shadow_screen.frag:120` (reconstructWorldPos) — was `ndc_z = depth * 2.0 - 1.0`, now `ndc_z = depth`.
- `ssao.frag:43` — same shape, same fix.

**No CPU matrix changes:** cameraToClip, projection_, terrain_mvp_, and inverseViewProj all unchanged — they were already producing/inverting D3D-style coordinates.

**Track A1 / C0 / C1 alignment:** `clipSpaceFrustumAdmit` predicate and
Track C compute cull tests use `0 <= rawClip.z <= rawClip.w`. After clip
control, this is hardware-native — no longer a manually-maintained
convention. Predicate body unchanged; semantics now align with hardware
expectation.

**Diagnostic:** `[INSTR v1] clip_control=enabled origin=lower_left depth=zero_to_one` at startup confirms the call succeeded. `unsupported gl_version_too_old` indicates fallback (would only happen on pre-4.5 hardware).

**Future implications:**
- HZB occlusion culling (Q21 candidate) benefits from full depth precision when it ships.
- Shadow z-fighting / acne issues that may have been worsened by half-precision are now operating with 2× headroom.
- Track F (DSA adoption arc) inherits the cleaner depth convention; Track F doesn't need to re-evaluate.
```

- [ ] **Step 5.2 — Index in MEMORY.md**

Add to the "Rendering / shaders" section of MEMORY.md (one line, ≤200 chars):

```
- ⭐ [glClipControl adopted (<date>)](clip_control_adopted.md) — D3D-native depth convention; recovers 2× depth precision; removes 4 shader workaround remaps; A1/C cull predicates now hardware-native
```

- [ ] **Step 5.3 — Commit memory updates**

```bash
# If memory is git-tracked locally; otherwise this is a save-only step.
```

---

## Self-Review

**Spec coverage:**

- Q20 (clip control adoption) → all five edits covered (engine init + 4 shaders).
- Atomic-commit requirement → Task 1 structures all five as one commit; tier1 5/5 gate before commit.
- Track A1 / C alignment → documented in commit message + memory file; no plan changes for A1/C needed (already verified during recon — predicate body is already in post-clip-control form).
- 2× depth precision win → primary justification, documented in commit + memory.

**Placeholder scan:** none — every step has exact code or commands. The screenshot paths use `/tmp/` placeholder which is the standard convention.

**Type consistency:** N/A — no new types defined.

**Open questions for executor:**
- Step 1.1's grep for engine init site: if the result is ambiguous (multiple plausible candidates), pick the one that runs AFTER `glewInit()` succeeds. `glClipControl` requires GLEW to have loaded the function pointer.
- Step 2.1: if the engine's screenshot mechanism is unfamiliar, grep `screenshot\|saveImage\|captureFrame\|F12` to find the binding. If none exists, RenderDoc capture is an acceptable substitute.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-06-clip-control-adoption.md`. Two execution options:

**1. Subagent-Driven** — fresh subagent per task. Probably overkill for a 5-edit slice; subagent dispatch overhead is comparable to the actual work.

**2. Inline Execution (recommended)** — execute Task 1 in one session (atomic commit), Task 2/3 verification immediately after, Task 4 soak runs as needed, Task 5 memory at any time post-soak.

**Sizing:** ~1 day code (Task 1) + ~1 day verification (Tasks 2/3) + ≥3 days soak (Task 4) + ~30min memory (Task 5). Total ≥5 days calendar-time, dominated by soak window.

**Coordination with active tracks:** none. Clip control touches files no active track touches. Can ship in parallel with Track A1 execution (A1 touches `mclib/camera.h` + `mclib/object_admission_predicate.cpp`; clip control touches `gameos_graphics.cpp` + 4 shaders). Recommended order: ship clip control FIRST so Track A1 / C0 / C1 cull predicates land on hardware-native semantics from day 1, but the predicate body is already correct for either convention so order doesn't affect correctness.

---

## Revision log

- **2026-05-06 (initial):** Plan written from clip-control recon findings.
- **2026-05-06 (post-advisor-pass):** Two corrections per outside-input
  review. (1) Step 1.2 fallback branch changed from "engine continues at
  half precision" (incorrect — would ship broken rendering since shader
  edits are unconditional) to fail-closed via `gosASSERT(false);
  std::abort();` matching the project's Track A1 selftest hard-fail
  pattern. The atomic-commit invariant requires that running without
  clip control with the new shaders is impossible. (2) Removed the
  "MC2_CLIP_CONTROL killswitch" speculation from Background guardrails
  — a CPU-side-only killswitch is unsafe without shader variants, which
  this plan does not ship. The atomic-commit invariant + fail-closed
  branch IS the safety mechanism.
