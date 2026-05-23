# Greybeard ruling -- RenderWorld Slice M1.5 setSceneDrawBuffers() helper

- Subject: `setSceneDrawBuffers(SceneDrawBufferMode, bool)` helper at
  `GameOS/gameos/gos_postprocess.cpp:31-56` and the routing of all 5
  scene-FBO `glDrawBuffers` call sites through it.
- Skill: `.claude/skills/greybeard.md` verbatim.
- Date: 2026-05-23.
- Reviewer stance: fresh ruling, no pre-judged verdict. Evidence
  grep-grounded against worktree HEAD `1b09b69`.

---

## 1. Subsystem pin

Scene-FBO MRT draw-buffer policy in `GameOS/gameos/gos_postprocess.cpp`.
First-hand evidence:
- `grep -n glDrawBuffers GameOS/gameos/gos_postprocess.cpp` returns 4
  hits, ALL inside the helper body (lines 38, 48, 54) or a stale
  comment at :566. Zero raw `glDrawBuffers` callers exist outside
  the helper in the production tree.
- `grep -rn glDrawBuffers --include=*.cpp` worktree-wide returns
  matches only in `gos_postprocess.cpp`. The helper is the sole
  policy owner across the engine.
- 5 scene-FBO `glDrawBuffers` call sites in spec C1 (createFBOs,
  beginScene, runScreenShadow, runGodRays, runShoreline) all route
  through the helper at lines 339, 488, 497, 586, 696, 729 (3 MRT
  sites + 3 SingleColor sites, including the explicit beginScene
  post-clear re-assert at :497).

## 2. Symptom vs cause

- **Proximate symptom (the bug C1 originally found):** static-prop
  fragment writes to `layout(location=2) out uint v_objectId` would
  be dropped because `beginScene()` and post-process passes re-issue
  `glDrawBuffers` with shorter lists, structurally removing
  attachment-2 from the active write mask. `lookupAtPixel` would
  return `Handle::invalid()` for every pixel.
- **Upstream condition:** the draw-buffer list is engine-wide GL
  state that any FBO bind site can freely re-issue. The codebase
  had 5 sites independently issuing `glDrawBuffers` against
  `sceneFBO_`, each carrying its own hand-written list shape.
  Policy drift was structural: adding a new MRT attachment required
  the author of the new slice to find and update every site, with
  no compile-time or grep-time enforcement that they had been
  exhaustive. M1 reviewer caught it for M1.5; nothing prevents M2,
  M3, M4, M5+ from re-introducing the same drift when they add
  their own attachments.

## 3. The meta-fix

**The single upstream change:** centralize scene-FBO draw-buffer
policy in one helper. Every site that targets `sceneFBO_` calls
`setSceneDrawBuffers(mode, objectIdReady)`. The list shape is
computed in one place from `(env flag, attachment-ready flag,
single-color-vs-MRT mode)`. No raw `glDrawBuffers` against
`sceneFBO_` exists anywhere else; the grep gate enforces this.

This change makes the "scattered draw-buffer policy drift" bug
class structurally hard to reintroduce:
- Future MRT attachments (M2 mech IDs, M3 terrain IDs, M4 VFX IDs,
  M5 overlay IDs) extend ONE function. The 5 call sites do not
  need to be re-audited.
- Mid-resize and env-on-no-texture states are handled by the
  `objectIdAttachmentReady` parameter (plan-review M3 fix), so the
  list shape is always consistent with what is actually attached.
- The grep gate `grep -n glDrawBuffers gos_postprocess.cpp` shows
  ALL production hits inside the helper body; new raw calls would
  be obvious in review.

This is "modernize the shim, do not correct the shim" applied to
GL state policy: the legacy mechanism (per-site hand-written
lists) is REPLACED, not patched.

## 4. Substitutive test

This is a substitutive change, not additive:
- 5 call sites that previously held `glDrawBuffers(N, list)` calls
  now hold `setSceneDrawBuffers(mode, ready)` calls. The raw calls
  are GONE from those sites (verified by `grep -n glDrawBuffers
  gos_postprocess.cpp` showing matches only inside the helper body
  at :38, :48, :54 and a stale comment at :566).
- No old path remains alongside the new one. The helper is the
  ONLY producer of `glDrawBuffers` against `sceneFBO_` in the
  worktree.
- The OLD per-site policy zone is GONE from the file, not merely
  bypassed.

Substitutive criterion: PASS.

One residual: `endShadowPass()` at :1046 rebinds `sceneFBO_`
without re-asserting draw-buffers. This is correct (no policy
change at that site, the prior helper-set list persists), but it
is a sibling site that does not route through the helper. It does
not violate the META-FIX because it does not issue
`glDrawBuffers`; it only rebinds the FBO. Flag as documentary
follow-up: if a future slice adds attachment work at that site,
it must go through the helper.

## 5. Verdict

**META-FIX.**

### Upstream change
Centralized scene-FBO draw-buffer policy. The helper
`setSceneDrawBuffers(SceneDrawBufferMode, bool)` at
`GameOS/gameos/gos_postprocess.cpp:31-56` is the sole producer of
`glDrawBuffers` against `sceneFBO_`. The list shape is computed
from `(IsObjectIdBufferEnabled(), objectIdAttachmentReady, mode)`
in one place.

### Bug class retired
"Scattered draw-buffer policy drift across post-process bind
sites." Concretely:
- The M1.5 C1 finding (5 sites independently maintaining list
  shapes) is unrepresentable: there is one shape, computed once.
- The mid-resize env-ON-no-texture race (plan-review M3) is
  handled by the `objectIdAttachmentReady` parameter; no caller
  can issue a 3-entry list when attachment-2 is unallocated.
- Future MRT attachments (M2..M5+) extend the helper, not the 5
  bind sites. The bug class "M2 mech ID writes dropped because
  beginScene re-asserts the M1.5 3-entry list instead of a new
  4-entry list" is prevented by construction.
- The `endShadowPass()` rebind-without-redraw-buffer pattern is
  safe because the prior helper-set list persists across the
  bind; no new sibling-site drift is introduced.

### Prevented bug instances
- **This slice (M1.5):** attachment-2 writes dropped in 5 places.
- **M2 (mech ID):** if mech adds attachment-3, the 5 sites do not
  need re-auditing; one helper edit suffices.
- **M3/M4/M5+:** each future MRT extension is a single-site change.
- **Mid-resize race:** env-ON destroyFBOs->createFBOs window
  cannot produce an invalid 3-entry list against unallocated
  attachment-2.

### Blast radius
- File: `GameOS/gameos/gos_postprocess.cpp` only.
- Symbols: 1 new helper + 5 routed call sites + 1 `#include
  RenderWorld/RenderWorld.h` for `IsObjectIdBufferEnabled()`.
- Grep gate: `grep -n glDrawBuffers gos_postprocess.cpp` is the
  one-line review check that future slices preserve the META-FIX.

### Verdict line
META-FIX. The setSceneDrawBuffers helper retires the
"scattered scene-FBO draw-buffer policy drift" bug class for
M1.5 AND every M2..M5+ slice that adds MRT attachments. Record
verbatim per worktree CLAUDE.md "Meta-fix discipline."

---

## Closing recommendation

Adopt as META-FIX. Add to slice closing notes:
- Grep gate `grep -n glDrawBuffers GameOS/gameos/gos_postprocess.cpp`
  expected to show matches only inside the helper body.
- M2 plan must extend the helper, not the 5 call sites, when adding
  attachment-3 for mech IDs.
- `endShadowPass()` rebind at :1046 is intentionally bare; if a
  future slice needs to re-assert draw-buffers there, route through
  the helper.

End of ruling.
