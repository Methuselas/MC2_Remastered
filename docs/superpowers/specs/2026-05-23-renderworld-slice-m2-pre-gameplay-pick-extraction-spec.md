# RenderWorld Slice M2-pre -- Preemptive META-FIX refactor of M1.6 gameplay-pick machinery

- Status: EXECUTABLE-READY (adversarial review applied 2026-05-23;
  greybeard pass deferred to first slice execution per skill scope)
- Adversarial review:
  docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-pre-spec-adversarial-review.md
  (0 CRIT / 2 MAJOR / 5 MINOR; all resolved in this spec)
- Date: 2026-05-23
- Predecessor slice: RenderWorld Slice M1.6 SHIPPED 2026-05-23 at
  `db25d67`. Pick wiring lives in
  `MissionInterfaceManager::tryStaticPropPick`
  (`code/missiongui.cpp:6168-6273`), called from the tails of
  `updateOldStyle` (`:1538`) and `updateAOEStyle` (`:1781`), gated by
  the 4-site `moverSelectedThisFrame` observable at
  `code/missiongui.cpp:1476/1511/1740/1763` (verified by grep at
  adversarial-review apply time 2026-05-23; M1.6 spec cited
  `:1460/1487/1690/1705` -- symbol-stable, line-drift refresh).
- META-FIX trigger:
  `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-6-greybeard-ruling.md`
  -- Section 3 names `tryGameplayPick(request)` and
  `screenToFboPixel(...)` as the deferred META-FIX; Section 5
  trigger condition: "when M2 begins planning, the M2 spec MUST
  extract both before adding the mech-pick strategy, OR explicitly
  justify deferring to M3."
- Required follow-ups before EXECUTABLE:
  - adversarial-plan-review pass (per worktree CLAUDE.md "Review
    discipline")
  - greybeard skill pass (per worktree CLAUDE.md "Meta-fix
    discipline") -- M2-pre is explicitly drafted AS the META-FIX;
    the greybeard pass should rule whether the extraction at
    sample-size-one satisfies the "substitutive central spine"
    criterion or whether deferral to M2.6 remains the right call
- DOC-ONLY: no code in this artifact. Pseudocode for the request /
  result types + the algorithm only.

---

## Scope (user-resolved, BAKE INTO SPEC verbatim)

```
M2-pre = preemptive META-FIX refactor only.

Do the pure extraction before any mech-specific work:
  extract tryGameplayPick(request)
  extract screenToFboPixel(...)
  preserve current M1.6 behavior exactly
  no mech IDs
  no mech adapter
  no shader changes
  no selection behavior changes

Reason: M1.6 already created the second-consumer trigger. If M2 starts
adding mech-specific pickup without extracting the shared pick flow
first, you will immediately bake static-prop assumptions into the mech
slice. The greybeard debt was explicit: when mech pickup wires up,
extract shared helpers before adding mech-specific gate semantics.
```

User-chosen sequencing after M2-pre:

- **M2:** route-only `MechRenderAdapter` (mirrors M1's
  `StaticPropRenderAdapter` shape; no IDs, no picking).
- **M2.5:** mech object-ID substrate (extends attachment-2 writes
  to the mech fragment path; mirrors M1.5 for mechs).
- **M2.6:** gameplay pickup integration (becomes the SECOND
  consumer of the M2-pre shared helpers; adds mech-specific gate
  semantics atop the unchanged spine).

---

## 1. Purpose / non-goals

### Purpose

Pay the M1.6 greybeard debt by extracting two reusable helpers from
the inline body of `tryStaticPropPick` BEFORE any second-consumer
work begins. The two extractions:

1. `tryGameplayPick(request) -> result` -- the shared gameplay-side
   dispatcher (substrate env gate + gesture gates + mover-first
   fallback gate + off-screen guard + coord scaling + lookupAtPixel
   call). Category-agnostic.
2. `screenToFboPixel(...)` -- the pure coord transform (UI-canvas
   viewport-space mouse coords + `gos_GetViewport` deltas + GL
   y-flip -> FBO pixel + GL pixel). No GL state mutation.

M2-pre is a pure refactor. Env-ON behavior MUST reproduce M1.6 hit /
miss results exactly on the same screen coords. Env-OFF behavior is
bit-identical (no new per-frame work on the default path).

### Non-goals (explicit)

- **Not a new feature.** No env-var additions. No new log lines. No
  changes to log schemas (`[STATIC_PROP_PICK v1] hit/miss` field
  order and meanings preserved verbatim).
- **Not mech work.** Zero edits to `mech3d.cpp` / `mech.cpp` /
  `bdmech.cpp`. No `MechRenderAdapter`. No mech-side attachment-2
  writes.
- **Not shader work.** Zero edits to `shaders/*.frag`,
  `shaders/*.vert`, `shaders/include/*.hglsl`, or any compute
  shader. `layout(location=2) out uint v_objectId` stays as M1.5
  shipped it.
- **Not selection-behavior changes.** Mover-first fallback ordering
  preserved. Shift+LMB on a friendly mover still toggles selection
  via legacy path with zero `[STATIC_PROP_PICK v1]` log emission.
  The four `moverSelectedThisFrame` writer sites stay put.
- **Not a substrate change.** `RenderWorld::lookupAtPixel`,
  `setLastStaticPropPick`, `clearLastStaticPropPick`,
  `getLastStaticPropPick`, `IsObjectIdBufferEnabled`,
  `IsStaticPropPickEnabled`, `IsStaticPropPickDebugEnabled` are all
  untouched.
- **Not a new env var.** The opt-in stack from M1.6 Section 9
  remains the only knob set.
- **Not a deletion slice.** `tryStaticPropPick` keeps existing as a
  thin caller. Its body shrinks; its signature, call sites
  (`code/missiongui.cpp:1538` + `:1781`), and observable side
  effects (log line + debug-state update) are identical.
- **Not a coord-utility consolidation.** Other UI-canvas-to-
  drawable translations elsewhere in the codebase (e.g.
  `mclib/mouse.cpp:225` Y-flip use) are NOT visited or unified.
  M2-pre extracts the helper used by gameplay pick; broader coord
  utilities are out of scope.

### Why "preemptive" matters

The M1.6 greybeard ruling explicitly identified the additive-slice
anti-pattern: if M2 ships a route-only `MechRenderAdapter` and M2.5
ships mech IDs and M2.6 then copy-pastes the `tryStaticPropPick`
body into `tryMechPick` with its own inline `gos_GetViewport` block,
the bug class re-emerges. M2-pre exists so the spine is in place
BEFORE M2.6 has a chance to fork it.

The user explicitly chose this sequencing: extract first, then
build M2 / M2.5 / M2.6 on the extracted spine. See the scope block
above.

### Open questions

See Section 12.

---

## 2. Relationship to M1.6 (debt) + M2.6 (future consumer)

### The M1.6 debt (what is being paid)

The M1.6 greybeard ruling at
`docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-6-greybeard-ruling.md`
named two specific extractions as the deferred META-FIX:

- `tryGameplayPick(request)` -- the shared substrate-consumer spine.
- `screenToFboPixel(...)` -- the pure coord transform.

Ruling Section 5 trigger condition (paraphrased): "when M2 begins
planning, the M2 spec MUST reuse / extract both, OR explicitly
justify deferring to M3." The user's chosen path is **extract
NOW, in M2-pre, before M2 lands.** The justification is in the
scope block above: extracting first prevents M2.6 from baking
static-prop assumptions into the mech slice.

### Why the helper has 1 caller at M2-pre close (and that's OK)

At M2-pre close, `tryGameplayPick` has exactly ONE caller: the
refactored `tryStaticPropPick`. This looks like "additive
single-instance extraction" -- the exact pattern the M1.6
greybeard ruling Section 5 warned against ("premature
abstraction would bake in static-prop assumptions").

The distinction: M2-pre is a REFACTOR slice, not a feature slice.
The extraction is preparation for a NAMED second consumer (M2.6).
The greybeard pass at M2-pre close should rule on whether:

- the extracted shape is **category-agnostic enough** that M2.6
  will use it unchanged, or
- the extraction is **shaped by static-prop assumptions** and
  needs revisiting at M2.6.

This spec proposes a category-agnostic shape (Section 4 +
Section 7) and flags the parts most likely to need revisiting in
Section 12.

### M2.6 extension contract (what the future consumer must conform to)

Once M2.5 lands and mech pixels write to attachment-2 with proper
depth ordering, M2.6 adds a new caller (e.g. `tryMechPick`) that:

- Checks its own category env var (`MC2_MECH_PICK=1` strawman),
  separate from `MC2_STATIC_PROP_PICK`.
- Builds a `GameplayPickRequest` from missiongui state (same fields
  as the static-prop path).
- Calls `tryGameplayPick(req)` -- unchanged spine.
- Switches on `result.outcome`:
  - `hit`: M2.6 owns the "what does it mean that we hit handle H?"
    semantics -- mech vs prop discrimination via the M2.5 type-tag
    on attachment-2, log schema `[MECH_PICK v1]`, mech-specific
    debug-state struct.
  - `miss` / `gated` / `skipped`: M2.6 owns its own miss handling.

Crucially: M2.6 does NOT change the spine. If M2.6 finds it MUST
change `tryGameplayPick`'s signature or body, that is the signal
the M2-pre extraction was premature and the greybeard ruling's
sample-size-of-one risk materialized.

Spine stability under M2.6 is the validation the M2-pre
extraction was correctly shaped. M2.6's spec MUST grep-cite
unchanged tryGameplayPick signature against the M2-pre HEAD.

### Mover-fallback ordering: still in place, will flip at M2.5+

Per M1.6 Section 2: "until M2 writes mech IDs into attachment-2,
GPU pickup MUST be a fallback after legacy mover picking." The
`moverSelectedThisFrame` observable + the helper's gated short-
circuit on that bool encodes this rule today.

M2.5 lands mech IDs in attachment-2. At that point, the fallback-
vs-preempt ordering can be revisited -- but NOT in M2-pre. The
M2-pre helper preserves the M1.6 fallback shape exactly. Any
ordering flip is M2.6-or-later work, with its own spec and
greybeard pass.

The `moverSelectedThisFrame` field name in `GameplayPickRequest`
is candidate-for-rename at M2.6 (it generalizes to "did the
legacy non-substrate path consume this click"). Section 12 Q5.

---

## 3. Shared types

### Type sketches (illustrative pseudocode, NOT the final patch)

Three new POD types, one new function pair. All new symbols live in
the file proposed in Section 6.

```cpp
// Input gesture + missiongui state. Mirrors the 7 args
// tryStaticPropPick already takes, plus the drawable / viewport
// state the inline body queries.
struct GameplayPickRequest {
    // Viewport-space mouse coords (Win32 convention: origin
    // top-left, Y grows down). Match userInput->getMouseX/Y().
    int  mouseX;
    int  mouseY;

    // Edge-triggered gesture flags from missiongui input gather.
    bool shiftDn;            // userInput->shift() this frame
    bool leftClicked;        // (!isLeftDrag && !isRightDrag && isLeftClick)
    bool bGui;               // cursor in HUD/control region
    bool bLeftDouble;        // double-click in progress

    // Mover-first fallback observable -- caller sets to true when
    // the legacy click-driven selection path consumed THIS click
    // (see M1.6 Section 4 + Q6 four-site instrumentation map).
    bool moverSelectedThisFrame;
};

// Diagnostic context propagated to caller for logging. All fields
// echo the inputs + the intermediate coord-translation results so
// caller logs can show the full transform on one line.
struct GameplayPickContext {
    int   mouseX, mouseY;        // echo of request
    int   fboX,   fboY;          // post-scale FBO pixel (top-left origin)
    int   glX,    glY;           // post-y-flip GL pixel (bottom-left origin)
    float vMulX,  vMulY;         // gos_GetViewport scale (FBO-pixel units)
    float vAddX,  vAddY;         // gos_GetViewport offset
    int   drawableWidth;
    int   drawableHeight;
};

// Outcome enum + result. Caller switches on outcome.
struct GameplayPickResult {
    enum class Outcome {
        skipped, // env / gesture / off-screen gates failed; no lookup ran
        gated,   // moverSelectedThisFrame fired; legacy path won
        miss,    // lookupAtPixel ran; pixel was background (raw=0)
        hit,     // lookupAtPixel ran; valid handle returned
    };
    Outcome                  outcome;
    RenderWorld::LookupResult lookup; // valid iff outcome == hit
    GameplayPickContext      ctx;
};

// The two new functions.
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req);

void screenToFboPixel(int mouseX, int mouseY,
                      float vMulX, float vMulY,
                      float vAddX, float vAddY,
                      int drawableWidth, int drawableHeight,
                      int* outFboX, int* outFboY,
                      int* outGlX,  int* outGlY);
```

### Why these fields and no others

The request struct intentionally carries ONLY the seven values
already passed to `tryStaticPropPick` plus nothing. It is NOT a
god-bag of "everything missiongui might want." If M2.6 needs a new
field (e.g. category enum -- see Section 12 Q3), that addition
lands in M2.6's spec with a justification.

The result struct intentionally splits "what happened" (outcome
enum) from "the data on hit" (lookup) from "diagnostic" (ctx).
Caller code that only cares about the hit handle does
`if (r.outcome == hit) use(r.lookup);`. Caller code that wants to
log on miss reads `r.ctx`. No conditional-validity contracts on
unrelated fields.

### Outcome semantics (precise)

- `skipped` = any of: substrate env off, wiring env off, !shiftDn,
  !leftClicked, bGui, bLeftDouble, off-screen mouse. The helper
  did NOT call `lookupAtPixel`. `lookup.isValid` is false; `ctx`
  is populated only with the echoed inputs (coord fields may be
  zero if the gate fired before scaling).
- `gated` = all gates passed AND moverSelectedThisFrame == true.
  The helper did NOT call `lookupAtPixel`. The intent is to give
  the caller a distinct signal from `skipped` (the gesture WAS
  intended; the legacy path just won).
- `miss` = `lookupAtPixel` ran and returned `isValid == false`.
  `ctx` is fully populated.
- `hit` = `lookupAtPixel` ran and returned `isValid == true`.
  `ctx` is fully populated. `lookup` carries the valid handle +
  generation + alive.

Caller-side: a caller that wants to log a "miss" diagnostic only
when the gesture was INTENDED checks `outcome == miss`, not
`outcome != hit`. The `gated` and `skipped` cases are silent by
design (the legacy path or the input filter handled it).

---

## 4. tryGameplayPick() contract + algorithm

### Contract

```
Input:  const GameplayPickRequest& req
Output: GameplayPickResult { outcome, lookup, ctx }
Side effects: NONE on outcome in { skipped, gated, miss }.
              ONE synchronous glReadPixels via RenderWorld::lookupAtPixel
              when all gates pass and moverSelectedThisFrame is false.
              The readback path is the same one M1.5 shipped; no new
              GL state is bound or mutated by this function.
Logging: NONE. The helper emits zero stderr lines. Caller owns logs.
Debug-state mutation: NONE. The helper does not touch
              RenderWorld::setLastStaticPropPick / clearLastStaticPropPick.
              Caller owns category-specific debug-state.
Thread-safety: main thread only (same as M1.6).
```

### Algorithm (pseudocode; matches M1.6 ordering)

```text
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req) {
    GameplayPickResult r{};
    r.ctx.mouseX = req.mouseX;
    r.ctx.mouseY = req.mouseY;
    r.outcome    = GameplayPickResult::Outcome::skipped;

    // Engine substrate gate. Cheap cached bool (M1.5).
    if (!RenderWorld::IsObjectIdBufferEnabled())
        return r;

    // Gesture gates (M1.6 Section 3 detection condition #3-#5).
    if (!req.shiftDn)       return r;
    if (!req.leftClicked)   return r;
    if (req.bGui)           return r;
    if (req.bLeftDouble)    return r;

    // Mover-first fallback gate (M1.6 Section 4).
    if (req.moverSelectedThisFrame) {
        r.outcome = GameplayPickResult::Outcome::gated;
        return r;
    }

    // Viewport state for coord translation.
    gos_GetViewport(&r.ctx.vMulX, &r.ctx.vMulY,
                    &r.ctx.vAddX, &r.ctx.vAddY);
    r.ctx.drawableWidth  = Environment.drawableWidth;
    r.ctx.drawableHeight = Environment.drawableHeight;

    // Off-screen guard (viewport-space bounds, M1.6 inline).
    if (req.mouseX < 0 || req.mouseY < 0
        || req.mouseX >= (int)r.ctx.vMulX
        || req.mouseY >= (int)r.ctx.vMulY)
        return r;

    // Coord translation: viewport-space -> FBO pixel -> GL pixel.
    screenToFboPixel(req.mouseX, req.mouseY,
                     r.ctx.vMulX, r.ctx.vMulY,
                     r.ctx.vAddX, r.ctx.vAddY,
                     r.ctx.drawableWidth, r.ctx.drawableHeight,
                     &r.ctx.fboX, &r.ctx.fboY,
                     &r.ctx.glX,  &r.ctx.glY);

    // Synchronous single-pixel readback (M1.5 substrate).
    r.lookup = RenderWorld::lookupAtPixel(r.ctx.glX, r.ctx.glY);

    r.outcome = r.lookup.isValid
        ? GameplayPickResult::Outcome::hit
        : GameplayPickResult::Outcome::miss;
    return r;
}
```

### What is NOT in this body (and why)

- **No category-specific env check.** `IsStaticPropPickEnabled` /
  the future `IsMechPickEnabled` are caller concerns. The spine
  cannot know which category to gate on.
- **No category-specific log emission.** Caller emits
  `[STATIC_PROP_PICK v1]` / `[MECH_PICK v1]` / etc.
- **No category-specific debug-state mutation.** Caller calls
  `setLastStaticPropPick` / future `setLastMechPick`.
- **No fallback inversion.** The mover-first gate stays. M2.5+
  ordering changes are out of scope.

### Reachability of all four outcomes

A caller that wants to verify the spine's branch coverage:

- `skipped`: env off OR any gesture filter fails OR off-screen
  mouse. Multiple paths reach this; the result struct does not
  currently distinguish which gate fired. Section 12 Q6 covers
  whether a "skip reason" enum should be added.
- `gated`: shiftDn + leftClicked + !bGui + !bLeftDouble + mover
  was selected this frame. Verified by friendly-mover canary.
- `miss`: gates pass, mover-not-selected, lookup returns invalid.
  Verified by Shift+click on terrain/sky.
- `hit`: gates pass, mover-not-selected, lookup returns valid.
  Verified by Shift+click on static prop.

---

## 5. screenToFboPixel() contract (pure transform)

### Contract

```
Input:  mouseX, mouseY (viewport-space, Win32 top-left origin)
        vMulX, vMulY    (FBO-pixel viewport extent from gos_GetViewport)
        vAddX, vAddY    (FBO-pixel viewport offset from gos_GetViewport)
        drawableWidth, drawableHeight (Environment.*; FBO pixel units)
Output (via out params):
        outFboX, outFboY -- FBO pixel (top-left origin)
        outGlX,  outGlY  -- GL pixel  (bottom-left origin; Y flipped)
Side effects: NONE. Pure function. No GL calls. No I/O. No global state.
Preconditions: drawableHeight > 0. vMulX, vMulY > 0 (caller has
        already off-screen-guarded; this helper does NOT re-check).
Thread-safety: pure; safe anywhere.
```

### Coord derivation (restored from M1.6 commit `1853ad9`)

The chain from raw Win32 mouse pixel to FBO pixel to GL pixel:

```
mouseXPosition = mouseX_pixel / drawableWidth   (normalized [0,1])
getMouseX()    = mouseXPosition * viewMulX      (scaled to viewport)
fbo_x          = mouseX + viewAddX              (viewport-relative + offset)
scaleX         = drawableWidth / viewMulX       (undo viewport scaling)
fboX           = vAddX + mouseX * scaleX        (FBO-pixel)
glX            = fboX                           (X is identity)
glY            = drawableHeight - 1 - fboY      (Win32 -> GL y-flip)
```

The scale factor `drawableWidth / vMulX` exists because the caller
passes viewport-space mouse coords (already normalized then re-scaled
to viewport extents), and the helper must back the value out to true
drawable-pixel coords so `lookupAtPixel` can index the FBO correctly.

### Algorithm (matches M1.6 inline coord scaling at `:6228-6233`)

```text
void screenToFboPixel(int mouseX, int mouseY,
                      float vMulX, float vMulY,
                      float vAddX, float vAddY,
                      int drawableWidth, int drawableHeight,
                      int* outFboX, int* outFboY,
                      int* outGlX,  int* outGlY)
{
    // Undo viewport scaling: mouseX/Y is in viewport-relative coords
    // (0..vMulX, 0..vMulY) but is also normalized to drawable -- see
    // M1.6 inline comments at code/missiongui.cpp:6197-6216 for the
    // full derivation. The scale factor converts back to drawable-
    // pixel coords; vAddX/Y handle sub-viewport offset.
    const float scaleX = (vMulX > 0.0f)
        ? ((float)drawableWidth  / vMulX) : 1.0f;
    const float scaleY = (vMulY > 0.0f)
        ? ((float)drawableHeight / vMulY) : 1.0f;

    *outFboX = (int)(vAddX + (float)mouseX * scaleX);
    *outFboY = (int)(vAddY + (float)mouseY * scaleY);

    // GL convention: origin bottom-left.
    *outGlX  = *outFboX;
    *outGlY  = drawableHeight - 1 - *outFboY;
}
```

### Why a pure function, not a method

- Pure functions are trivially unit-testable. A future M2-pre plan
  may add a small unit test (table-driven: viewport (0,0,800,600)
  full-canvas case + sub-viewport case + identity case).
- No `this`. The helper does not need RenderWorld or missiongui
  state.
- M2.6 can call it from a different gameplay class without
  inheritance gymnastics.

### What is NOT in this body (and why)

- **No `gos_GetViewport` call.** Caller queries; helper receives
  the values. Keeps the helper pure and lets a unit test pass in
  synthetic viewport values without mocking GOS.
- **No off-screen check.** Caller does this against vMulX/Y before
  calling. Putting bounds checks here would require the helper to
  signal failure, which violates the "pure transform" contract.
- **No clamping.** If caller passes garbage (e.g. mouseX > vMulX),
  the result is garbage. The caller's responsibility to guard.

---

## 6. Caller pattern (tryStaticPropPick refactored)

### The shape of the refactored caller

```text
void MissionInterfaceManager::tryStaticPropPick(
    bool moverSelectedThisFrame,
    bool shiftDn,
    bool leftClicked,
    bool bGui,
    bool bLeftDouble,
    int  mouseX,
    int  mouseY)
{
    // Category gate (static-prop wiring opt-in).
    if (!RenderWorld::IsStaticPropPickEnabled())
        return;

    GameplayPickRequest req{};
    req.mouseX                  = mouseX;
    req.mouseY                  = mouseY;
    req.shiftDn                 = shiftDn;
    req.leftClicked             = leftClicked;
    req.bGui                    = bGui;
    req.bLeftDouble             = bLeftDouble;
    req.moverSelectedThisFrame  = moverSelectedThisFrame;

    GameplayPickResult r = tryGameplayPick(req);

    switch (r.outcome) {
    case GameplayPickResult::Outcome::hit: {
        RenderWorld::setLastStaticPropPick(r.lookup,
                                           r.ctx.mouseX, r.ctx.mouseY,
                                           r.ctx.glX,    r.ctx.glY);
        const RenderWorld::StaticPropSelectionDebugState picked =
            RenderWorld::getLastStaticPropPick();
        std::fprintf(stderr,
            "[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "
            "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
            "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
            r.lookup.handle.bits,
            (unsigned)r.lookup.handle.index(),
            (unsigned)r.lookup.handle.generation(),
            (int)picked.recipeIndex,
            r.ctx.mouseX, r.ctx.mouseY, r.ctx.glX, r.ctx.glY,
            r.ctx.fboX,   r.ctx.fboY,
            r.ctx.vMulX,  r.ctx.vMulY, r.ctx.vAddX, r.ctx.vAddY,
            r.ctx.drawableWidth, r.ctx.drawableHeight);
        break;
    }
    case GameplayPickResult::Outcome::miss: {
        RenderWorld::clearLastStaticPropPick();
        if (RenderWorld::IsStaticPropPickDebugEnabled()) {
            std::fprintf(stderr,
                "[STATIC_PROP_PICK v1] miss screen=(%d,%d) gl=(%d,%d) "
                "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) "
                "draw=(%d,%d)\n",
                r.ctx.mouseX, r.ctx.mouseY, r.ctx.glX, r.ctx.glY,
                r.ctx.fboX,   r.ctx.fboY,
                r.ctx.vMulX,  r.ctx.vMulY, r.ctx.vAddX, r.ctx.vAddY,
                r.ctx.drawableWidth, r.ctx.drawableHeight);
        }
        break;
    }
    case GameplayPickResult::Outcome::gated:
    case GameplayPickResult::Outcome::skipped:
        // No-op. Legacy path consumed the click (gated) or the
        // gesture filter rejected it (skipped).
        break;
    }
}
```

### Log-byte parity (load-bearing)

The hit / miss log strings above MUST be byte-identical to M1.6's
emission at `code/missiongui.cpp:6249-6258` (hit) and `:6266-6270`
(miss). Same field order, same printf format specifiers, same
spacing. Plan-stage author MUST diff the literal format strings
against HEAD M1.6 (commit `db25d67`).

Reason: M2-pre is a refactor. Any byte-level drift in the log
string is an observable behavior change that breaks Section 8
Gate 2 (env-ON reproduction).

### Call sites untouched

- `code/missiongui.cpp:1538` (updateOldStyle tail call) -- argument
  list unchanged.
- `code/missiongui.cpp:1781` (updateAOEStyle tail call) -- argument
  list unchanged.
- The 4 `moverSelectedThisFrame = true` writers at `:1476`,
  `:1511`, `:1740`, `:1763` -- untouched.

The signature of `tryStaticPropPick` is preserved as-is. Only its
body changes.

---

## 7. Helper location (open question Q1)

### Two candidate placements

**Candidate A (preferred lean): new dedicated file**

```
code/gameplay_pick.h
code/gameplay_pick.cpp
```

Contents:
- `GameplayPickRequest`, `GameplayPickContext`, `GameplayPickResult`
  types
- `tryGameplayPick(const GameplayPickRequest&)` free function
- `screenToFboPixel(...)` free function

`code/missiongui.cpp` includes `code/gameplay_pick.h`. Caller pattern
in Section 6 lives in `tryStaticPropPick` (still in `missiongui.cpp`).

CMake: add the new `.cpp` to the `code/` source list (the engine-
side game-logic target; same target `missiongui.cpp` already lives
in -- grep `CMakeLists.txt` at plan time to verify exact list name).

**Candidate B: file-static in `missiongui.cpp`**

Types + `tryGameplayPick` + `screenToFboPixel` live in an anonymous
namespace at the top of `code/missiongui.cpp`. Public surface: just
the existing `tryStaticPropPick` method. M2.6's mech-pick caller
would have to live in `missiongui.cpp` too, OR M2.6 would have to
promote the helper out at that point.

### Why Candidate A wins (lean)

- Future M2.6 mech-pick caller likely also lives in `missiongui.cpp`
  (selection logic clusters there), but the helper's reusability
  reads more clearly when it sits in its own file with a clear
  header surface.
- A dedicated file signals "this is shared gameplay-pick
  infrastructure," not "this happens to live in missiongui.cpp."
- Future M3 (terrain pick) and M4 (VFX pick) get a clear home
  without needing to repeat the same promotion-out decision.
- The header is small (the three POD structs + two function
  declarations). Inclusion cost in `missiongui.cpp` is negligible.

### Why Candidate B has merit

- Smaller touch. No new files, no CMake edit, no firewall script
  update.
- If `missiongui.cpp` is the ONLY caller and stays that way,
  Candidate B keeps the surface area localized.
- M2.6 may co-locate naturally.

### Includes / firewall (RESOLVED -- no edit needed)

The new file lives in `code/gameplay_pick.{h,cpp}`. This directory
is NOT in `scripts/check-include-firewall.sh`'s `SCOPE_DIRS`
(verified at `scripts/check-include-firewall.sh:22` --
`SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer
MaterialSystem DebugRenderer RenderDeviceGL"` -- `code/` is not
listed). The firewall only constrains those scoped directories.
Therefore the new TU may freely include
`../RenderWorld/RenderWorld.h` and `../RenderCore/Handle.h` via
the same reach-in pattern that `code/missiongui.cpp:33` already
uses for `RenderWorld/RenderWorld.h` (see M1.6 ship commit
`32ee49b` introducing that include). No firewall edit is required.

The adversarial review pass 2026-05-23 confirmed this by grepping
the firewall script directly; what was Q11 is now closed.

### Decision (RESOLVED): Candidate A

Helper lives in NEW FILE `code/gameplay_pick.{h,cpp}`. Rationale:
- Establishes the game-side shared-utility convention; M2.6+
  mech-pickup will share the file as its second caller.
- Decouples spine identity from `missiongui.cpp`'s primary role
  (UI/HUD glue, not gameplay infrastructure).
- Header surface is small (3 PODs + 2 free functions); inclusion
  cost in `missiongui.cpp` is negligible.

### Precedent note for `code/` free-function headers

Current `code/*.h` headers are predominantly class-method
declarations, not free-function utility headers. `code/gameplay_pick.h`
introduces a new convention in `code/`: a pure free-function utility
header for game-side shared logic. M2-pre is establishing the
precedent; future M3 (terrain pick) / M4 (VFX pick) extensions are
expected to follow the same shape, and M2.6 (mech pickup) will be
the first non-static-prop caller.

Future readers grep'ing `code/*.h | head` to infer "what shape do
headers here take?" should land on the class-method-decl norm; the
gameplay_pick header is the documented exception, justified by the
named multi-consumer trajectory (Section 2 + Section 10).

---

## 8. Validation gates

### Gate 1: env-OFF parity (tier1 5/5)

With `MC2_OBJECT_ID_BUFFER=0` and `MC2_STATIC_PROP_PICK=0` (defaults):

- Run `tier1` (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`).
- Verification:
  - Tier1 5/5 PASS.
  - `grep '\[STATIC_PROP_PICK v1\]' <log>` returns ZERO matches in
    every run (matches M1.6 env-OFF behavior).
  - No fps delta vs M1.6 HEAD baseline (no new per-frame work).

Pass criterion: bit-for-bit identical click behavior vs M1.6 ship.

### Gate 2 (REVISED): MC2_GAMEPLAY_PICK_SELFTEST=1 automated self-test

The load-bearing post-refactor proof is an automated substrate
self-test analogous to M1.5's `[RENDER_WORLD_SELFTEST v1] step=all`.
The M1.6 user-driven Shift+click canary is RETAINED as a
supplementary visual verification but is NOT the load-bearing gate
(user clicks are not reproducible at byte parity; a self-test is).

**Design.** A new `RunGameplayPickSelfTest()` function is added next
to the helper TU (Section 7) and invoked from `RenderWorld::init()`
when `getenv("MC2_GAMEPLAY_PICK_SELFTEST")` returns `"1"` AND
`RenderWorld::IsObjectIdBufferEnabled()` is true (the self-test
exercises the substrate; substrate-off makes the test vacuous).

The self-test calls `tryGameplayPick(req)` directly with synthetic
`GameplayPickRequest` inputs at known pixel positions; each step
asserts `result.outcome` matches expected:

```
step=1 shiftDn=false                                -> skipped
step=2 leftClicked=false                            -> skipped
step=3 bGui=true                                    -> skipped
step=4 bLeftDouble=true                             -> skipped
step=5 moverSelectedThisFrame=true (all else clean) -> gated
step=6 all gates clean + mouseX=-1                  -> skipped (off-screen)
step=7 all gates clean + mouseY=INT_MAX             -> skipped (off-screen)
step=8 all gates clean + center-FBO pixel during
       stable frame -> hit OR miss (NOT skipped/gated).
       Record which; assert outcome IN {hit, miss}.
```

Step 8 is the "spine reached `lookupAtPixel`" assertion. The exact
hit-vs-miss disposition depends on what the scene rendered at the
synthetic pixel; the load-bearing claim is that the spine did not
gate or skip prematurely.

**Result lines** (stderr, mirroring `[RENDER_WORLD_SELFTEST v1]`):

```
[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all
[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=N expected=<...> actual=<...>
```

`FAIL` is a STOP. It indicates the extracted helper has diverged
from M1.6 gate semantics -- the refactor regressed the gate
ladder. This is exactly the regression class the M1.6 user-driven
canary was designed to catch; the self-test catches it
reproducibly with no human in the loop.

**Pass criterion.** `result=PASS step=all` on first launch under
`MC2_GAMEPLAY_PICK_SELFTEST=1 MC2_OBJECT_ID_BUFFER=1` on
`mc2_03` (any tier1 mission also works; mc2_03 is the highest-
prop choice to maximize hit-vs-miss diversity at step 8).

**Substitutive-not-additive.** Post-refactor, the inline gate
machinery in `tryStaticPropPick` is GONE -- the body is just
`if (!IsStaticPropPickEnabled()) return; req = ...; r =
tryGameplayPick(req); switch (r.outcome) {...}`. The self-test
verifies the extracted helper handles the same input space the
inline body did; the M1.6 inline ladder is retired, not duplicated.

**Supplementary visual canary (NOT load-bearing).** With
`MC2_OBJECT_ID_BUFFER=1` and `MC2_STATIC_PROP_PICK=1`, a smoke
session on `mc2_03` where the user Shift+left-clicks on static
props, empty terrain, and a friendly mover should produce log
lines structurally matching M1.6 (same field order, same hit /
miss outcome shape per gesture category). Per MINOR m4 (Section 8
Gate 6), a `tests/m2_pre_log_diff.sh` artifact captures the SHAPE
check.

### Gate 3: env-ON tier1

With both env vars on:

- Run tier1 5/5.
- Verification:
  - Tier1 5/5 PASS.
  - `[STATIC_PROP_PICK v1] hit` matches M1.6 baseline counts per
    mission (within +/- 1 to allow for boot-frame timing
    variance; the canary is the env-ON behavior, not exact
    auto-click counts).
  - 0 fps avg delta vs M1.6 env-ON baseline.

### Gate 4: log-string byte diff

Plan-stage executable verification (NOT a smoke gate; mechanical
diff):

```
diff <(git show db25d67:code/missiongui.cpp \
        | grep -h 'fprintf.*STATIC_PROP_PICK v1') \
     <(git show HEAD:code/missiongui.cpp \
        | grep -h 'fprintf.*STATIC_PROP_PICK v1')
```

(Proper `git show <sha>:<filepath>` extraction; the `<sha>@<path>`
form is invalid git syntax.)

Pass criterion: identical (modulo the new caller's `r.ctx.*` /
`r.lookup.*` field accesses, which yield byte-identical printf
output by Section 6 contract).

### Gate 6: tests/m2_pre_log_diff.sh -- backup observability check

Named artifact: `tests/m2_pre_log_diff.sh`. Runs the env-ON canary
on `mc2_03` against M2-pre HEAD and structurally compares
`[STATIC_PROP_PICK v1] hit/miss` lines from a pre-refactor reference
log (captured at M1.6 ship `db25d67`) and a post-refactor log.

Full byte reproducibility is NOT feasible (user clicks differ
between sessions). The script therefore performs a SHAPE check
per matched line:

- Same fields present.
- Same field order.
- Each field's value RANGE is sane (e.g. `handle=` parses as
  uint; `gl=(X,Y)` is within drawable bounds; `vMul=` is finite).

The automated self-test (Gate 2) is the PRIMARY proof. Gate 6 is
a backup observability check: it would catch a drift in the log
format string that the self-test (which does not emit logs)
cannot detect.

Pass criterion: shape-diff returns zero structural divergences.

### Gate 5: spine-stability assertion (deferred to M2.6)

Not run at M2-pre close; recorded here as the M2.6 validation
hook. M2.6's spec MUST grep-cite that
`tryGameplayPick(const GameplayPickRequest&) -> GameplayPickResult`
signature matches M2-pre HEAD exactly. If M2.6 changes the
signature, that is the signal the M2-pre extraction was premature.

---

## 9. Forbidden behaviors

### No mech work

- No edits to `mech3d.cpp`, `mech.cpp`, `bdmech.cpp`,
  `code/warrior.cpp`, or any mech-side TU.
- No `MechRenderAdapter`. No new `GameAdapters/` files.
- No mech-side writes to `GL_COLOR_ATTACHMENT2`.
- No new RenderWorld API for mechs.

### No shader work

- Zero edits under `shaders/`.
- The fragment-shader `layout(location=2) out uint v_objectId`
  stays exactly as M1.5 shipped it.
- No new uniforms, no new SSBOs, no new compute kernels.

### No selection-behavior changes

- Plain LMB still selects movers via the legacy path.
- Shift+LMB on a friendly mover still toggles via the legacy
  additive-select gesture (M1.6 Section 11 invariant preserved).
- Shift+LMB on a static prop still emits `[STATIC_PROP_PICK v1]
  hit` with M1.6 byte-identical formatting.
- The 4 `moverSelectedThisFrame` writer sites at `:1476`, `:1511`,
  `:1740`, `:1763` are untouched (no add, no move, no delete).
- The sibling `setSelected(false)` sites (M1.6 cited
  `:1462/:1483/:1692/:1701`; current HEAD drift mirrors the writer
  shift; grep at plan-execution time) remain explicitly NOT
  instrumented (M1.6 Section 11 + Q6 invariant).

### No env-var additions

- The opt-in stack (`MC2_OBJECT_ID_BUFFER`, `MC2_STATIC_PROP_PICK`,
  `MC2_STATIC_PROP_PICK_DEBUG`) stays as M1.6 shipped it.
- No new `MC2_*` env var. (The user-chosen sequencing puts the
  hypothetical `MC2_MECH_PICK` at M2.6, not M2-pre.)

### No log-schema changes

- `[STATIC_PROP_PICK v1] hit` and `miss` schemas remain at `v1`.
- Field order, field names, printf format specifiers all preserved.
- No new fields. No new log lines. No new schema versions.

### No substrate API additions

- `RenderWorld::lookupAtPixel`, `setLastStaticPropPick`,
  `clearLastStaticPropPick`, `getLastStaticPropPick`,
  `IsObjectIdBufferEnabled`, `IsStaticPropPickEnabled`,
  `IsStaticPropPickDebugEnabled` -- all untouched.
- No new accessors on RenderWorld. No new debug-state struct.

### No coord-utility consolidation beyond pick

`screenToFboPixel` is introduced as the helper used by
`tryGameplayPick`. Other coord transforms elsewhere in the
codebase (e.g. `mclib/mouse.cpp:225` Y-flip) are NOT visited.
Consolidating those is a separate slice with its own greybeard
pass.

### Edit budget

The patch should comprise:

- One new header file (Candidate A: `code/gameplay_pick.h`).
- One new source file (Candidate A: `code/gameplay_pick.cpp`).
- One CMakeLists edit (add the new `.cpp` to the source list).
- Possibly one firewall-script edit (allow-list the new TU; verify
  at plan stage).
- One body-only edit to `MissionInterfaceManager::tryStaticPropPick`
  (`code/missiongui.cpp:6168-6273`) -- signature unchanged.

If the patch grows beyond this, it has stopped being M2-pre.

---

## 10. M2.6 extension contract (what the future mech-pickup caller must conform to)

### Required shape

When M2.6 lands, the new mech-pick caller (e.g.
`tryMechPick(moverSelectedThisFrame, shiftDn, leftClicked, bGui,
bLeftDouble, mouseX, mouseY)`) MUST:

1. Check its own category env var (`MC2_MECH_PICK` strawman).
2. Build a `GameplayPickRequest` from missiongui state with the
   SAME field values the static-prop caller uses (the gesture is
   identical; only the category differs).
3. Call `tryGameplayPick(req)` -- unchanged spine.
4. Switch on `result.outcome`:
   - `hit`: M2.6 owns category resolution. The `LookupResult` carries
     a handle from attachment-2; M2.5 will have established whether
     mech pixels and prop pixels are distinguishable by handle range
     / type tag / separate attachment. M2.6's spec resolves how the
     caller decides "this is a mech pick" vs "this is a prop pick
     showing through where M2.5 left a coverage gap." If the M2.5
     substrate makes the pixel unambiguously a mech, M2.6 emits
     `[MECH_PICK v1] hit ...` and updates a mech-specific debug
     state. Otherwise M2.6 falls through to the static-prop caller
     (or drops the click silently -- M2.6 decides).
   - `miss` / `gated` / `skipped`: M2.6 owns its own no-op or
     diagnostic handling.

### Required NOT to do

- M2.6 MUST NOT change `GameplayPickRequest`'s field set without an
  amendment to this spec.
- M2.6 MUST NOT add side effects to `tryGameplayPick` (no new
  category-specific env checks; no new log lines).
- M2.6 MUST NOT bypass the spine and call `lookupAtPixel` directly
  with its own inline coord-translation block. (That re-creates
  the bug class.)

### Spine-stability check at M2.6 plan stage

The M2.6 plan MUST grep the M2-pre HEAD for `tryGameplayPick`
signature + `GameplayPickRequest` field list and assert they are
unchanged. Any required change is a M2-pre redesign signal.

### What M2.6 MAY do without amending this spec

- Add new fields to `GameplayPickRequest` IF those fields are
  category-agnostic (e.g. "the click came from a UI event vs an
  AI agent" -- if such a distinction emerges).
- Add new outcome enum cases IF the spine truly needs to signal a
  new disposition (unlikely; the four current outcomes cover the
  gate ladder exhaustively).
- Add a new caller in `code/missiongui.cpp` or elsewhere that uses
  the existing spine.

### What triggers an M2-pre redesign

If M2.6 finds it needs:

- A category enum on the request (e.g. to short-circuit lookup if
  attachment-2 won't carry that category's IDs).
- A different gesture set (e.g. Ctrl-click instead of Shift-click
  for mechs).
- A different fallback observable (e.g. "did the static-prop pick
  consume this click").

...then the M2-pre extraction was premature, and an M2.6-pre
slice (analogous to M2-pre) is needed to reshape the spine. This
is the explicit fallback the user's sequencing accepts.

---

## 11. Greybeard expectation

### Lean: META-FIX

The M1.6 greybeard ruling at Section 3 named exactly this
extraction as the META-FIX. M2-pre IS that extraction. Even though
the spine has ONE caller at M2-pre close, the extraction:

- Satisfies the named trigger condition (Section 5 of the M1.6
  ruling).
- Provides a single substitutive central spine that M2.6 will use
  unchanged (Section 10 contract).
- Centralizes the coord-translation policy (no second site will
  ever need to know about `gos_GetViewport` -> drawable-pixel
  scaling for gameplay pick).
- Establishes the contract that prevents copy-paste of the M1.6
  inline body into a future mech-pick caller.

### Anticipated greybeard counter

A fair greybeard counter: "you have ONE caller. The M1.5
`setSceneDrawBuffers` META-FIX had FIVE simultaneous call sites
consolidated. M2-pre is additive single-instance. Defer to M2.6."

The response: M2-pre is preparation for a named, planned second
consumer. The user's sequencing puts the extraction BEFORE M2 /
M2.5 / M2.6 specifically to prevent the additive-anti-pattern
documented in `memory/feedback_offload_must_be_substitutive_not_additive.md`.
Deferring the extraction to M2.6 risks the M1.6 ruling's exact
warning: M2.5 lands first, M2.6 starts under M2.5's coverage
shape, and the spine never gets extracted because "now there is a
working flow, don't refactor."

### Substitutive-not-additive proof (load-bearing for META-FIX rule)

Per worktree CLAUDE.md "Meta-fix discipline" and
`memory/feedback_offload_must_be_substitutive_not_additive.md`, the
greybeard pass must verify the inline machinery in `tryStaticPropPick`
is GONE post-refactor, not merely complemented by the new helper.
Section 8 Gate 2 (`MC2_GAMEPLAY_PICK_SELFTEST=1` automated self-test)
is the load-bearing substitutive proof: it exercises
`tryGameplayPick` directly through the full gate ladder, and the
caller is reduced to category-env gate + `tryGameplayPick` call +
outcome-switch. If the inline gate code survives in
`tryStaticPropPick`, the self-test still passes (it tests the
helper) but the substitutive criterion fails -- the greybeard pass
should grep the refactored `tryStaticPropPick` body to confirm
zero residual inline `gos_GetViewport` / off-screen / gesture-gate
calls.

### What the greybeard pass at M2-pre close should rule on

- Is the extracted shape **category-agnostic enough**? (Sections
  3 + 4 propose a category-agnostic shape; reviewer may identify
  static-prop leaks.)
- Is the helper location decision (Section 7 Candidate A vs B)
  justified at this slice size?
- Is the spine-stability assertion in Section 10 + Gate 5 strong
  enough to detect a premature extraction at M2.6 time?

### Sub-judgment on `screenToFboPixel`

The coord helper is the lower-risk extraction. It is a pure
function with a single grep-able call site today. Its extraction
is unambiguously META-FIX:

- Centralizes the viewport-to-FBO translation policy.
- Pure function -- no inheritance, no state.
- Single grep target for future "where do we do this?" audits.
- Author's own M1.6 commit message at `1853ad9` flagged the
  extraction.

The greybeard pass should rule this sub-extraction as META-FIX
even if the broader spine is ruled additive.

---

## 12. Open questions

- **Q1. RESOLVED (2026-05-23 adversarial-review apply):** Helper
  lives in NEW FILE `code/gameplay_pick.{h,cpp}` (Candidate A).
  Rationale: establishes the game-side shared-utility convention
  ahead of the named second caller (M2.6 mech-pickup). See
  Section 7 "Decision" + "Precedent note." Q11 firewall concern
  also resolved (firewall does not scope `code/`).

- **Q2.** Should `screenToFboPixel` live next to `tryGameplayPick`
  (same file) or in a more generic coord-utility file (e.g.
  `code/coord_util.{h,cpp}`)?
  - **Lean: same file.** Single consumer for now. Promoting it to
    a generic coord-utility file invites scope creep (audit of
    other coord transforms). Re-evaluate if M3 / M4 add new coord
    consumers.

- **Q3.** Should `GameplayPickRequest` carry a category enum
  (StaticProp / Mech / Terrain / VFX) so the spine can pre-filter
  by what M2.5+ attachment-2 will return?
  - **Lean: category-agnostic for M2-pre.** Today only the
    static-prop fragment path writes to attachment-2; the spine
    has nothing to pre-filter against. Add a category enum when
    M2.5 mech IDs land AND the spine genuinely needs to gate on
    "would this category's pixel even be readable?" Adding the
    enum now bakes in assumptions about M2.5's representation
    (handle range? type tag? separate attachment?) that have not
    been decided.

- **Q4.** How should the spine signal "lookupAtPixel returned
  valid but the handle has been retired since"?
  - **Lean: not the spine's job.** M1.5's
    `RenderWorld::lookupAtPixel` already validates against
    `s_objectRecords` with generation + alive check; the helper
    just propagates `result.lookup.isValid`. Verified by reading
    `RenderWorld/RenderWorld.cpp:448-510` per M1.6 spec Section
    2. If M1.5's contract changes, this propagation continues to
    work.

- **Q5.** Naming -- `tryGameplayPick` vs `dispatchGameplayPick` vs
  `runGameplayPick`?
  - **Lean: `tryGameplayPick`.** Matches the greybeard ruling's
    named symbol AND M1.6's `tryStaticPropPick` shape. Same
    semantic family: "try the operation; return what happened."

- **Q6.** Should `GameplayPickResult` carry a "skip reason" enum
  alongside the outcome enum, so callers (or tests) can
  distinguish "off-screen mouse" from "shift not held" from
  "substrate env off"?
  - **Lean: no.** Adds surface area for a use case (debugging
    "why didn't my click register?") that the existing
    `MC2_STATIC_PROP_PICK_DEBUG` miss-line already covers
    indirectly. If users genuinely want this, add it in M2.6
    when the second consumer concretizes the need.

- **Q7. RESOLVED (2026-05-23 adversarial-review apply):** KEEP the
  name `moverSelectedThisFrame` in `GameplayPickRequest`. Smaller
  diff, and the variable's semantic IS mover-only today --
  renaming to a general term (e.g. `legacyPathConsumedThisFrame`)
  preempts a generalization before any non-mover legacy path
  exists. Reconsider at M2.6 if the second consumer introduces a
  different "legacy path" (likely: mech selection that competes
  with M2.5 substrate).

- **Q8.** Should `tryStaticPropPick` itself be renamed (e.g. to
  `staticPropPickCallback` or moved out of
  `MissionInterfaceManager`)?
  - **Lean: no.** Keep the method signature stable across this
    refactor. Call sites at `code/missiongui.cpp:1538` + `:1781`
    stay byte-identical; ownership of the caller does not change.
    The body shrinks; the surface stays.

- **Q9.** Should the M2-pre slice ship a unit test for
  `screenToFboPixel`?
  - **Lean: optional.** A small table-driven test (full-canvas
    viewport, sub-viewport, identity case, Y-flip case) would
    provide a regression gate that survives the M2.6 spine-
    stability check. Test infrastructure for the engine code/
    target is light; plan-stage author decides whether a
    parameterized test fits the existing harness or is itself an
    out-of-scope addition.

- **Q10.** Does the per-mission `RenderWorld::destroy()` hook need
  to clear any new state introduced by M2-pre?
  - **Lean: no.** M2-pre introduces zero new state. The
    `StaticPropSelectionDebugState` lifecycle is unchanged. The
    spine helper is stateless. Q4 propagation means no caching of
    handles in the spine.

- **Q11. RESOLVED (2026-05-23 adversarial-review apply): MOOT.**
  `scripts/check-include-firewall.sh:22` SCOPE_DIRS lists only
  `RenderCore RenderWorld Visibility MeshRenderer MaterialSystem
  DebugRenderer RenderDeviceGL`. `code/` is not scoped, so
  `code/gameplay_pick.cpp` including `RenderWorld/RenderWorld.h`
  is unconstrained by the firewall (same reach-in pattern as
  `code/missiongui.cpp:33`). Zero firewall edit needed. See
  Section 7 "Includes / firewall" for the verified citation.

---

## 13. Spec author notes (not part of the contract)

- This spec was written with M1.6 SHIPPED at `db25d67` and
  verified live (tier1 5/5 PASS env-ON + env-OFF; user-driven
  canary on mc2_03 with 26 hits + 11 misses + mover-toggle
  preservation). Every cited M1.6 symbol grep-verified at write
  time:
  - `MissionInterfaceManager::tryStaticPropPick` body at
    `code/missiongui.cpp:6168-6273` (verified).
  - Call sites at `code/missiongui.cpp:1538` and `:1781` (verified).
  - 4 `moverSelectedThisFrame = true` writers at `:1476`, `:1511`,
    `:1740`, `:1763` -- note: M1.6 spec Section 11 cites these as
    `:1460/:1487/:1690/:1705`; the actual HEAD lines at M2-pre
    write time are `:1476/:1511/:1740/:1763` per grep. The spec
    will be amended with current line numbers at plan stage; the
    behavior is unchanged. Numbers drift; symbols are stable.
- The biggest open ambiguities are Q1 (helper location), Q3
  (category enum), and Q11 (firewall script). All are plan-stage
  resolvable.
- The spec is intentionally smaller than M1.6 (~600 lines vs
  M1.6's ~861) because M2-pre is a pure refactor with no new
  gameplay behavior, no new env vars, no new log lines, no new
  substrate API.
- No code in this artifact. Pseudocode in Sections 3, 4, 5, 6 is
  illustrative.
- ASCII only; no emoji.

---

End of M2-pre DRAFT spec.
