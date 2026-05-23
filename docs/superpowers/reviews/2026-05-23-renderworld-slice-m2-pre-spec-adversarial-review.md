# Adversarial review: M2-pre gameplay-pick extraction spec

- Target: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md`
  (DRAFT, ~1097 lines)
- Reviewer date: 2026-05-23
- Predecessor HEAD verified: `db25d67` + coord-fix `1853ad9`
- Method: every cited file:line + symbol grep'd at write time against
  worktree HEAD `code/missiongui.cpp`, `RenderWorld/RenderWorld.h`,
  `scripts/check-include-firewall.sh`, root `CMakeLists.txt`.

## Verdict: CONDITIONAL PROMOTE

Spec is internally consistent and the algorithm faithfully reproduces
M1.6. Conditions for promotion to plan-stage:

1. Fix the line-drift INSIDE the spec itself (Section 1 cites
   `:1460/1487/1690/1705`; HEAD is `:1476/1511/1740/1763`; only the
   Section 13 author note acknowledges drift).
2. Rewrite the firewall-concern paragraph in Section 7 + Q11 — the
   firewall script does NOT scope `code/`, so the concern is moot.
3. Concede that Gate 2 ("env-ON M1.6-reproduction canary") cannot be
   click-coord-exact and either downgrade its pass criterion or add
   a deterministic substrate-driven canary (analogous to M1.5
   `[RENDER_WORLD_SELFTEST v1]`).

Counts: CRITICAL=0, MAJOR=2, MINOR=5. Strengths confirmed=6.
User sign-off needed: 3 items (Q1, Gate-2 mechanism, Q9 unit test).

---

## CRITICAL

None. Spec proposes no new GL state, no shader edits, no SSBO schema
work, no env vars. The only new code is two free functions + three
POD types whose mechanical feasibility is grep-confirmed (see
"Strengths confirmed" below).

---

## MAJOR

### M-1. Gate 2 (env-ON M1.6-reproduction canary) is not actually verifiable as written

Spec Section 8 Gate 2 (lines 692-715) promises:

> "M2-pre and M1.6 emit the same log lines for the same screen
> coords on the same mission."

The M1.6 canary that this is compared against is the user-driven
26-hit / 11-miss sequence on `mc2_03` (CLAUDE.md "Active campaigns"
M1.6 entry). Reproducing it requires the user to click the SAME
screen pixels — but `getLastStaticPropPick.lastPickMouseX/Y` are
recorded in Win32 viewport-space and camera state drifts the on-
screen prop position between runs. A 1-pixel click drift can flip
hit -> miss at prop edges, breaking byte parity of the diagnostic
fields (`vMul`, `vAdd`, `draw=` survive but `handle`/`idx`/`gen`/
`recipe` do not).

The spec acknowledges this implicitly in Gate 3 ("+/- 1 count
tolerance for boot-frame timing variance") but holds Gate 2 to a
stricter standard than is mechanically achievable.

Recommendation: either
- (a) Replace Gate 2 with an automated substrate self-test in the
  style of M1.5 `[RENDER_WORLD_SELFTEST v1]` — call
  `tryGameplayPick` directly with a synthetic `GameplayPickRequest`
  at known FBO pixels chosen from `RenderWorld`'s populated record
  table (no UI click needed). Pass criterion: outcome enum +
  `lookup.handle` matches `lookupAtPixel` called inline; coord
  diagnostics in `ctx` match `screenToFboPixel` on the same inputs.
  This validates the extraction WITHOUT click-coord exactness.
- (b) Downgrade Gate 2 to "log SCHEMA byte-identical; handle/idx
  values may differ" and lean on Gate 4 (mechanical diff of the
  printf format strings) for byte parity.

Either is acceptable; the current Gate 2 wording over-promises and
will produce a "failed gate" event from camera jitter, not from a
real regression.

### M-2. Section 7 firewall concern + Q11 are based on a misreading of the firewall script

Spec lines 652-668 (Section 7 "Firewall consideration") + lines
1054-1063 (Q11) treat `scripts/check-include-firewall.sh` as if it
polices `code/`. Verified against HEAD:

`scripts/check-include-firewall.sh:22`:
```
SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer
            MaterialSystem DebugRenderer RenderDeviceGL"
```

Loop at `:72`: `for dir in $SCOPE_DIRS; do ... grep -rn ... "$dir"`.
The script ONLY recurses the seven directories listed. `code/` is
not in scope. Therefore `code/gameplay_pick.cpp` may include
`RenderWorld/RenderWorld.h` (or anything else) without any firewall
edit. Confirming pattern: `code/missiongui.cpp:33` already does
`#include "../RenderWorld/RenderWorld.h"` with no allowlist entry
and no violation.

The firewall script polices the OPPOSITE direction (engine-side
modules including game-side headers). It does not police
gameplay-side TUs. The spec inverts this.

Recommendation: replace Section 7 lines 652-668 with a single
sentence — "`code/` is outside `SCOPE_DIRS` in
`scripts/check-include-firewall.sh:22`; new TUs under `code/` need
no firewall edit." Resolve Q11 to "NO firewall update required."

This is MAJOR rather than MINOR because the spec asks the
plan-stage author to verify a constraint that does not exist; a
diligent plan-stage author will waste cycles or, worse, propose a
spurious script edit.

---

## MINOR

### m-1. Line-drift inside the spec itself

Spec Section 1 (lines 9-13) cites the 4 mover-fallback sites at
`code/missiongui.cpp:1460/1487/1690/1705`. Section 13 author note
(lines 1078-1082) acknowledges the drift to
`:1476/:1511/:1740/:1763` but leaves Section 1 unfixed. Per worktree
CLAUDE.md "Grep before citing file:line": numbers verified at write
time MUST be in the executable spec body, not just a footnote.

Verified at write time (Grep on `moverSelectedThisFrame\s*=\s*true`
in `code/missiongui.cpp`):
- `:1476`
- `:1511`
- `:1740`
- `:1763`

Exactly 4 hits. Section 1 + Section 6 (line 591-593) + Section 9
(lines 780-781) all need the numbers updated. Tail call sites at
`:1538` and `:1781` and body at `:6168-6273` are correct as cited.

### m-2. Coord-translation contract loses a comment

M1.6 body at `code/missiongui.cpp:6197-6216` contains a ~20-line
inline comment deriving why `mouseX/Y` is "viewport-relative" and
not "FBO pixels" (the load-bearing explanation; the coord bug
`1853ad9` fixed turned on exactly this). The spec's
`screenToFboPixel` body (Section 5, lines 461-475) and contract
(lines 433-446) drop this derivation. A future reader of
`gameplay_pick.cpp` will lack the reasoning the M1.6 commit
encoded.

Recommendation: Section 5 should require the new function's
implementation to carry the M1.6 derivation comment verbatim, or
the spec should commit to copying it. The contract block alone
("vMulX, vMulY > 0; caller has already off-screen-guarded") does
not tell the maintainer WHY the scale formula is what it is.

### m-3. Gate 4 (log-string byte diff) cites a non-existent shell

Spec Section 8 Gate 4 (lines 736-740) proposes:
```
diff <(grep -h 'fprintf.*STATIC_PROP_PICK v1' \
        code/missiongui.cpp@db25d67) \
     <(grep -h ... code/missiongui.cpp@HEAD)
```

`code/missiongui.cpp@db25d67` is git pathspec syntax that works
with `git show db25d67:code/missiongui.cpp`, not as a path to
`grep`. As written, this command fails. Mechanical fix:
```
diff <(git show db25d67:code/missiongui.cpp | \
        grep 'fprintf.*STATIC_PROP_PICK v1') \
     <(git show HEAD:code/missiongui.cpp | \
        grep 'fprintf.*STATIC_PROP_PICK v1')
```

Trivial; flagging because the spec is the executable artifact.

### m-4. `_pad0`-style write-only verification mechanism is missing

The spec promises "preserves M1.6 behavior exactly" (line 82) but
provides no mechanism beyond user-driven canary + log-string diff.
A stronger no-behavior-change verification: capture stderr from
M1.6 HEAD on a tier1 env-ON run, capture from M2-pre HEAD on the
same run, `diff` the filtered `[STATIC_PROP_PICK v1]` lines. The
spec mentions tier1 5/5 PASS in Gate 3 but never names a
log-diff artifact.

Recommendation: add to Section 8 a "Gate 3.5: log-content diff"
that records the env-ON stderr capture as a plan-stage deliverable
and diffs it against the M1.6 ship's captured stderr.

### m-5. Q1 (helper location) — convention precedent exists and disambiguates

The brainstorm flagged Q1 open with a "lean Candidate A" answer.
Verified precedent in HEAD:
- `code/` has dozens of `.cpp/.h` pairs (`gameobj.{cpp,h}`,
  `objmgr.{cpp,h}`, etc.) but ALL are class declarations, not
  shared free-function utilities.
- `code/missiongui.h:57-58` contains exactly two free-function
  declarations: `void *gMalloc(long size)` and `void gFree(void
  *me)` — and those are leftovers, not a convention.
- There is NO existing `code/util.h` / `code/coord_util.h` /
  `code/gameplay_*.h` for shared free functions.

So Candidate A (new file) does NOT have a precedent to mirror; it
ESTABLISHES a precedent. That is fine — but the spec should be
honest that the "future M3/M4 will benefit" justification rests on
projected work, not on an existing pattern. Adjust Section 7's
"Why Candidate A wins" paragraph.

Sub-finding: if Candidate A is chosen, `code/gameplay_pick.cpp`
must be added to the root `CMakeLists.txt` source list (verified
location: root `CMakeLists.txt:171-190` and beyond; `missiongui.cpp`
is sibling-listed there). Spec line 621 names this correctly
("CMakeLists.txt at plan time to verify exact list name") but the
list IS in the root CMakeLists, not under `code/` (there is no
`code/CMakeLists.txt`).

---

## Strengths confirmed under grep

1. `tryStaticPropPick` body at `code/missiongui.cpp:6168-6273` —
   verified. Spec algorithm Section 4 lines 348-398 reproduces the
   gate order exactly: env -> shift -> leftClick -> bGui ->
   bLeftDouble -> moverSelectedThisFrame -> off-screen ->
   gos_GetViewport -> screenToFboPixel -> lookupAtPixel. Matches
   M1.6 lines 6177-6237.
2. Tail call sites `code/missiongui.cpp:1538` (`updateOldStyle`)
   and `:1781` (`updateAOEStyle`) — verified.
3. Log format strings at `code/missiongui.cpp:6250-6258` (hit) and
   `:6267-6270` (miss) — verified; Section 6 lines 536-547 +
   553-560 mirror the format specifiers and field order exactly.
4. RenderWorld API surface — verified at `RenderWorld/RenderWorld.h`:
   `lookupAtPixel` at `:156`, `StaticPropSelectionDebugState` at
   `:165-174`, `setLastStaticPropPick` at `:181-183`,
   `clearLastStaticPropPick` at `:188`, `getLastStaticPropPick` at
   `:193`. The spec's caller pattern (Section 6) uses these
   correctly.
5. `tryStaticPropPick` signature (7 args: bool moverSelectedThisFrame,
   bool shiftDn, bool leftClicked, bool bGui, bool bLeftDouble, int
   mouseX, int mouseY) — verified at `code/missiongui.cpp:6168-6174`;
   matches Section 6 line 506-512 and Q8 "keep signature stable"
   commitment.
6. `screenToFboPixel` sub-extraction IS a pure function. Verified
   against M1.6 lines 6217-6233: the inline block reads
   `gos_GetViewport`/`Environment.drawableWidth`/`drawableHeight`
   (which the spec correctly moves into the CALLER), then performs
   only arithmetic. No GL calls, no global mutation. The "pure
   transform" contract in Section 5 is correct.

---

## Greybeard-verdict-prediction sanity check (spec Section 11)

Section 11 is honest about the sample-size-of-one critique. The
spec's response — "M2-pre is substitutive because the inline body
IS replaced; one-caller-now does not equal additive-forever, the
named M2.6 trigger establishes the second consumer" — is the right
framing. The spec correctly distinguishes M1.5's 5-site
`setSceneDrawBuffers` consolidation (5 simultaneous) from M2-pre
(1 caller now + 1 named future consumer in M2.6).

One soft caveat: Section 11 "Sub-judgment on screenToFboPixel"
(lines 947-959) overstates its case. The coord helper at M2-pre
close also has ONE caller (the new `tryGameplayPick`). It only
becomes META-FIX unambiguously when M2.6 / M3 / M4 consume it. As
written, the sub-judgment elevates it above the broader spine
without earning the distinction. Recommend softening to "the
coord helper's purity makes the extraction lower-risk regardless
of greybeard verdict on the spine."

---

## Decisions needing user/advisor sign-off before revision pass

1. **Q1 helper location (Candidate A new file vs B file-static).**
   Spec leans A; reviewer agrees A is preferable but notes no
   `code/` precedent exists for free-function utility headers.
   This is the establishing precedent, not a mirror of one. User
   sign-off: confirm A as the precedent-setter, or accept B as
   smaller-touch + revisit when M3 actually consumes the helper.

2. **Gate 2 mechanism (M-1 above).** Choose between (a) automated
   substrate self-test analogous to M1.5
   `[RENDER_WORLD_SELFTEST v1]`, or (b) downgrade Gate 2 to schema
   parity + Gate 4 byte parity. Both are sound; the user should
   pick which validation footprint M2-pre carries.

3. **Q9 (unit test for screenToFboPixel).** Spec leans optional.
   Reviewer notes: a 4-row table-driven test (full-canvas,
   sub-viewport, identity, Y-flip-only) would survive the M2.6
   spine-stability check and double as the deterministic
   alternative to user-driven Gate 2. Recommend escalating Q9 to
   "ship the test alongside the extraction" if the user picks
   option (a) for Gate 2 — the test is the deterministic gate.

---

End of adversarial review.
