# Track A1 — Object Admission Predicate Replacement

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the bool predicate at the single object-admission callsite (`code/gameobj.cpp:2090` → `Camera::projectForObjectAdmission` at `mclib/camera.h:541-554`) with a clip-space frustum test, keeping `screen.x/y/z/w` output byte-identical to legacy. Env-flag killswitch keeps default behavior unchanged during soak.

**Architecture:** Dual-output wrapper pattern. The wrapper declares a `LegacyProjectionResult result;` local, calls `projectZ(point, screen, &result)` (which populates `screen` byte-identically and captures `rawClip = xformCoords` in `result.rawClip`), then either returns the legacy bool (default) or computes a new clip-space frustum bool from `result.rawClip` (when env flag = modern). The existing `MC2_PROJECTZ_TRACE` + `MC2_PROJECTZ_HEATMAP` infrastructure provides per-callsite predicate-disagreement counters as the dual-run signal — Track A1 adds one new candidate predicate (`homogClipFull = homogClip && rectNearFar`) to make the comparison direct.

**Tech Stack:** C++ (engine, MSVC RelWithDebInfo), MC2 OpenGL renderer, existing `projectz_trace.{h,cpp}` infrastructure, `[DESTROY v1]` lifecycle instrumentation, `scripts/run_smoke.py` tier1 gate.

**Spec references:**
- Decisions: `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` (Q1, Q2, Q3)
- Roadmap: `docs/superpowers/mc3-rendering-modernization-roadmap.md` (Track A § A1)
- Wrapper substrate: `docs/superpowers/specs/projectz-policy-split-report.md`
- Trace infrastructure: `docs/superpowers/specs/2026-04-25-projectz-containment-design.md`

**Worktree CLAUDE.md rules in force:**
- Build: `cmake --build build64 --config RelWithDebInfo`
- Stock install must remain playable (default = legacy until soak passes)
- Tier1 smoke is the regression gate
- Debug instrumentation rule: lifecycle reworks land env-gated `[SUBSYS v1]` prints in same commit
- Documentation discipline: every cited symbol grep-verified

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `mclib/object_admission_predicate.h` | Create | Public declaration of `clipSpaceFrustumAdmit()` and `objectAdmissionPredicateMode()` accessor. |
| `mclib/object_admission_predicate.cpp` | Create | Predicate implementation, env-flag probe, startup banner emit, self-test runner. |
| `mclib/camera.h` | Modify | `projectForObjectAdmission` wrapper body — add `LegacyProjectionResult` capture and modern-predicate branch. |
| `mclib/projectz_trace.h` | Modify | Add `homogClipFull` field to `ProjectZPredicates` struct. |
| `mclib/projectz_trace.cpp` | Modify | Compute `homogClipFull` in trace dispatch; add to predicate menu, summary headers, heatmap output. |
| `CMakeLists.txt` | Modify | Add new `.cpp` to `mclib` library target. |
| `docs/superpowers/specs/2026-05-06-track-a1-acceptance-envelope.md` | Create | Operator-authored acceptance envelope (post-Task 8). |
| `~/.claude/projects/A--Games-mc2-opengl-src/memory/track_a1_object_admission_predicate.md` | Create | Memory file capturing the slice's flip-on date, smoke results, and any soak findings. |
| `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` | Modify | Index entry for the new memory file. |

**Decomposition rationale:** the predicate function is its own translation unit because it's the load-bearing testable surface — keeping it out of `camera.h` makes it self-test-able at startup and grep-able as a unit. The wrapper change is minimal (8 lines of body change). The trace addition is a one-field struct extension plus a one-line predicate evaluation.

---

> **Task ordering note (post-advisor-review):** Tasks were reordered so the
> predicate function (`clipSpaceFrustumAdmit`) lands BEFORE the trace
> addition, so both the trace's `homogClipFull` candidate and the production
> wrapper call into the SAME function. This eliminates the drift hazard
> (advisor sharpening #3): the trace is by-construction the same predicate
> the wrapper uses.

## Task 1 — Predicate function header

The new predicate is a free function over `Stuff::Vector4D` (the clip-space coordinate). Keeping it in its own header isolates the testable surface and prevents accidental inlining changes when wrapper code shifts.

**Files:**
- Create: `mclib/object_admission_predicate.h`

- [ ] **Step 1.1 — Create the header**

```cpp
// mclib/object_admission_predicate.h
#pragma once
//---------------------------------------------------------------------------
// Track A1 — object admission predicate (clip-space frustum).
//
// This is the modern bool decision used by Camera::projectForObjectAdmission
// when MC2_OBJECT_ADMISSION_PREDICATE=modern, AND the function the trace
// system uses to compute its homogClipFull candidate predicate. Both
// callers must use the same function — drift would invalidate the
// dual-run parity gate.
//
// Spec: docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md
// Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
//---------------------------------------------------------------------------

namespace Stuff { class Vector4D; }

// Mode probed at startup from MC2_OBJECT_ADMISSION_PREDICATE env var.
enum class ObjectAdmissionPredicateMode {
    Legacy,    // default: bool comes from projectZ's screen-rect test
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe (idempotent). Called lazily from objectAdmissionPredicateMode().
void objectAdmissionPredicate_init();

// Read the active mode. Lazily initializes — startup ordering is non-load-bearing.
ObjectAdmissionPredicateMode objectAdmissionPredicateMode();

// The predicate itself. Returns true iff the homogeneous clip-space point is
// inside the canonical clip volume:
//   rawClip.w >  0
//   |rawClip.x| <= rawClip.w   (left/right planes)
//   |rawClip.y| <= rawClip.w   (top/bottom planes)
//   0 <= rawClip.z <= rawClip.w (near/far planes; MC2 uses D3D-style [0,w])
//
// Note: rawClip is `xformCoords` from inside Camera::projectZ — the post-multiply,
// pre-divide clip-space coord. Always use the value captured via
// LegacyProjectionResult::rawClip; do NOT pass the post-divide screen vector.
bool clipSpaceFrustumAdmit(const Stuff::Vector4D& rawClip);

// Self-test entry point. Runs the unit cases listed in
// docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
// Task 3. Prints `[OBJECT_ADMISSION v1] event=selftest_pass|fail case=<name>`
// per case. Returns the number of failures (0 = all pass). Caller is
// expected to fail loudly on non-zero return — see Task 4.
int objectAdmissionPredicate_selftest();
```

- [ ] **Step 1.2 — Commit**

```bash
git add mclib/object_admission_predicate.h
git commit -m "feat(track-a1): add object admission predicate header

Declares clip-space frustum predicate, mode enum, lazy init/accessor,
selftest entry. No implementation yet (Task 2); no production callers
yet (Task 5).

Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
"
```

---

## Task 2 — Implement predicate, env-flag probe (lazy), self-test runner

The implementation sits in its own translation unit. The env probe is lazy — the accessor calls `init()` if not already initialized — so callers don't need to care about startup ordering (advisor sharpening #1).

**Files:**
- Create: `mclib/object_admission_predicate.cpp`
- Modify: `CMakeLists.txt` (add the new source)

- [ ] **Step 2.1 — Create the implementation file**

```cpp
// mclib/object_admission_predicate.cpp
#include "object_admission_predicate.h"
#include "stuff/stuff.hpp"   // Stuff::Vector4D
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool             s_initialized = false;
ObjectAdmissionPredicateMode s_mode = ObjectAdmissionPredicateMode::Legacy;

const char* modeLabel(ObjectAdmissionPredicateMode m) {
    return (m == ObjectAdmissionPredicateMode::Modern) ? "modern" : "legacy";
}

} // namespace

void objectAdmissionPredicate_init() {
    if (s_initialized) return;
    const char* env = std::getenv("MC2_OBJECT_ADMISSION_PREDICATE");
    if (env && std::strcmp(env, "modern") == 0) {
        s_mode = ObjectAdmissionPredicateMode::Modern;
    } else {
        s_mode = ObjectAdmissionPredicateMode::Legacy;
    }
    s_initialized = true;
    std::printf("[INSTR v1] object_admission_mode=%s\n", modeLabel(s_mode));
    std::fflush(stdout);
}

ObjectAdmissionPredicateMode objectAdmissionPredicateMode() {
    // Lazy init — startup ordering is non-load-bearing.
    objectAdmissionPredicate_init();
    return s_mode;
}

bool clipSpaceFrustumAdmit(const Stuff::Vector4D& rawClip) {
    // w must be in front of camera. Behind-camera vertices have rawClip.w <= 0
    // and the canonical clip-space tests below become meaningless.
    if (rawClip.w <= 0.0f) return false;
    const float w = rawClip.w;
    if (rawClip.x < -w || rawClip.x > w) return false;
    if (rawClip.y < -w || rawClip.y > w) return false;
    // MC2 uses D3D-style [0, w] depth range (matches existing `rectNearFar`
    // candidate at projectz_trace.cpp:154: `rawClip.z >= 0 && rawClip.z <= rawClip.w`).
    if (rawClip.z < 0.0f || rawClip.z > w) return false;
    return true;
}

int objectAdmissionPredicate_selftest() {
    int fails = 0;

    auto runCase = [&](const char* name, const Stuff::Vector4D& rawClip, bool expected) {
        bool actual = clipSpaceFrustumAdmit(rawClip);
        const char* result = (actual == expected) ? "pass" : "fail";
        if (actual != expected) ++fails;
        std::printf("[OBJECT_ADMISSION v1] event=selftest_%s case=%s expected=%d actual=%d "
                    "rawClip=(%.3f,%.3f,%.3f,%.3f)\n",
                    result, name, expected ? 1 : 0, actual ? 1 : 0,
                    rawClip.x, rawClip.y, rawClip.z, rawClip.w);
    };

    // Center of frustum, w=1, point fully inside.
    Stuff::Vector4D inside;       inside.x =  0.0f; inside.y =  0.0f; inside.z = 0.5f; inside.w = 1.0f;
    runCase("center_inside", inside, true);

    // Behind camera: w negative.
    Stuff::Vector4D behind;       behind.x =  0.0f; behind.y =  0.0f; behind.z = 0.5f; behind.w = -1.0f;
    runCase("behind_camera", behind, false);

    // Beyond left clip plane.
    Stuff::Vector4D leftOut;      leftOut.x = -1.5f; leftOut.y = 0.0f; leftOut.z = 0.5f; leftOut.w = 1.0f;
    runCase("left_outside", leftOut, false);

    // On left clip plane (x == -w): admitted by `<=` boundary semantics.
    Stuff::Vector4D leftEdge;     leftEdge.x = -1.0f; leftEdge.y = 0.0f; leftEdge.z = 0.5f; leftEdge.w = 1.0f;
    runCase("left_edge_inclusive", leftEdge, true);

    // Past far plane (z > w).
    Stuff::Vector4D far;          far.x =  0.0f; far.y =  0.0f; far.z = 1.5f; far.w = 1.0f;
    runCase("past_far", far, false);

    // Closer than near plane (z < 0).
    Stuff::Vector4D nearer;       nearer.x = 0.0f; nearer.y = 0.0f; nearer.z = -0.1f; nearer.w = 1.0f;
    runCase("before_near", nearer, false);

    // w == 0: pathological / degenerate. Reject (cannot represent a finite point).
    Stuff::Vector4D wZero;        wZero.x = 0.0f; wZero.y = 0.0f; wZero.z = 0.0f; wZero.w = 0.0f;
    runCase("w_zero_degenerate", wZero, false);

    // Far corner of frustum: x=w, y=w, z=w. All on inclusive boundaries.
    Stuff::Vector4D corner;       corner.x = 1.0f; corner.y = 1.0f; corner.z = 1.0f; corner.w = 1.0f;
    runCase("far_corner_inclusive", corner, true);

    return fails;
}
```

- [ ] **Step 2.2 — Add the source to CMakeLists.txt**

Find the existing list of `mclib/*.cpp` source files (search: `grep -n "mclib/.*\.cpp" CMakeLists.txt`). Add `mclib/object_admission_predicate.cpp` in alphabetical position. Preserve existing indentation/comma style.

- [ ] **Step 2.3 — Build**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: clean build. If linker complains about unresolved Stuff::Vector4D, verify `stuff/stuff.hpp` is the right include (cross-check with `mclib/camera.cpp`).

- [ ] **Step 2.4 — Commit**

```bash
git add mclib/object_admission_predicate.cpp CMakeLists.txt
git commit -m "feat(track-a1): implement clipSpaceFrustumAdmit + lazy env probe + selftest

MC2_OBJECT_ADMISSION_PREDICATE=modern|legacy probed lazily on first
accessor call. clipSpaceFrustumAdmit checks all six clip-space half-spaces
(w>0, |x|<=w, |y|<=w, 0<=z<=w). Self-test runner covers 8 boundary cases.

Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
"
```

---

## Task 3 — Add `homogClipFull` candidate predicate to the trace menu (calls `clipSpaceFrustumAdmit`)

The existing trace evaluates 5 candidate predicates per call (`legacyRect`, `legacyRectFinite`, `homogClip`, `rectSignedW`, `rectNearFar`, `rectGuard`). The Track A1 modern predicate is added as `homogClipFull` — and **its computation calls the same `clipSpaceFrustumAdmit` function the production wrapper will use** (advisor sharpening #3). This means the trace's per-callsite disagreement counter is, by construction, the disagreement between the legacy bool and what production will return when modern mode flips on. Drift is impossible because there's only one predicate function.

**Files:**
- Modify: `mclib/projectz_trace.h:24-32` (struct definition)
- Modify: `mclib/projectz_trace.cpp:58-61` (predicate index header), `:140-160` (computation), `:280-320` (output)

- [ ] **Step 3.1 — Add field to `ProjectZPredicates` struct**

In `mclib/projectz_trace.h`, locate the struct at line 24-32:

```cpp
struct ProjectZPredicates {
    bool legacyRect;
    bool legacyRectFinite;
    bool homogClip;
    bool rectSignedW;
    bool rectNearFar;
    bool rectGuard;
    bool isPerspective;
};
```

Add `homogClipFull` after `rectGuard`:

```cpp
struct ProjectZPredicates {
    bool legacyRect;
    bool legacyRectFinite;
    bool homogClip;
    bool rectSignedW;
    bool rectNearFar;
    bool rectGuard;
    bool homogClipFull;      // homogClip && rectNearFar — full clip-space frustum test (Track A1 modern predicate)
    bool isPerspective;
};
```

- [ ] **Step 3.2 — Compute the new field via `clipSpaceFrustumAdmit`**

Add include at the top of `mclib/projectz_trace.cpp`:

```cpp
#include "object_admission_predicate.h"
```

In `mclib/projectz_trace.cpp` at line ~158 (after the `rectGuard` computation), add:

```cpp
    // homogClipFull: full clip-space frustum (xy + near + far).
    // Calls the SAME function the production wrapper uses, so trace
    // disagreement counts are by-construction what production will see
    // when MC2_OBJECT_ADMISSION_PREDICATE=modern. Drift is impossible.
    p.homogClipFull = clipSpaceFrustumAdmit(rawClip);
```

- [ ] **Step 3.3 — Add the predicate to the index header and label menu**

In `mclib/projectz_trace.cpp` at lines 58-61, update the comment and the labels array:

```cpp
// Indices: 0=legacyRectFinite 1=homogClip 2=rectSignedW 3=rectNearFar 4=rectGuard 5=homogClipFull
```

```cpp
static const char* const PREDICATE_LABELS[] = {
    "legacyRectFinite", "homogClip", "rectSignedW", "rectNearFar", "rectGuard", "homogClipFull"
};
```

(The array used to have 5 entries; bump constant `NUM_PREDICATES` from 5 to 6 wherever it's defined. Search: `grep -n "NUM_PREDICATES\|sizeof(PREDICATE_LABELS)" mclib/projectz_trace.cpp` and update each site to match the new count.)

- [ ] **Step 3.4 — Add the new field to per-callsite disagreement output**

Around line 280 in `projectz_trace.cpp`, the output line lists all candidates. Add `homogClipFull=%s` to the format string and the corresponding `(preds.homogClipFull ? "T" : "F")` argument. Same for the structured emit at line 316:

```cpp
                preds.homogClip,
                preds.rectSignedW,
                preds.rectNearFar,
                preds.rectGuard,
                preds.homogClipFull
```

- [ ] **Step 3.5 — Build and verify the trace still emits**

Run:

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: build succeeds with no warnings about unused fields.

Then quick env-gated sanity check (no smoke yet — we're just confirming the trace still emits):

```bash
MC2_PROJECTZ_TRACE=1 MC2_PROJECTZ_HEATMAP=1 \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 10 --kill-existing
```

Expected: smoke passes. Search log for `homogClipFull` in trace output — must appear in per-call records. If not, the format-string update missed a site.

- [ ] **Step 3.6 — Commit**

```bash
git add mclib/projectz_trace.h mclib/projectz_trace.cpp
git commit -m "feat(projectz): add homogClipFull candidate predicate via shared function

Track A1 — adds homogClipFull to the predicate menu, computed via
clipSpaceFrustumAdmit() (the SAME function the production wrapper will
call). This guarantees trace disagreement counts equal production
behavior under MC2_OBJECT_ADMISSION_PREDICATE=modern; drift is impossible
because there's only one predicate function.

No behavior change: predicate is recorded in trace output only; no
production code path consumes it yet (Task 5).

Spec: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
"
```

---

## Task 4 — Wire startup probe + selftest entry

The probe runs once at engine init; the selftest runs once if `MC2_OBJECT_ADMISSION_SELFTEST=1`. Both happen before any rendering so the `[INSTR v1]` banner shows the active mode at the top of the log.

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (or whichever file owns the existing `[INSTR v1]` banner — confirm by grep)

- [ ] **Step 4.1 — Locate the existing `[INSTR v1]` banner**

```bash
grep -rn "\[INSTR v1\]" GameOS/ code/ mclib/ | head -10
```

Expected: there is one site that emits the startup banner with `enabled:` listing all the active env-gated subsystems. Track A1 piggybacks on that — adding `object_admission_mode=` to its output is the same pattern as the existing `static_update_trace`/`static_update_skip` fields.

- [ ] **Step 4.2 — Add the init call before banner emit**

Above the line that prints `[INSTR v1] enabled: ...`, add:

```cpp
#include "object_admission_predicate.h"
// ...
objectAdmissionPredicate_init();
```

(The init function is idempotent and emits its own `[INSTR v1] object_admission_mode=...` line. The existing `[INSTR v1] enabled:` aggregate banner does not need to be changed; adding a separate prefix-line is consistent with how `[INSTR v1] water_fp=...` etc. are emitted.)

- [ ] **Step 4.3 — Add the selftest gate (hard-fail on any failure)**

Right after the init call, add:

```cpp
if (const char* st = std::getenv("MC2_OBJECT_ADMISSION_SELFTEST")) {
    if (std::strcmp(st, "1") == 0) {
        int fails = objectAdmissionPredicate_selftest();
        std::printf("[OBJECT_ADMISSION v1] event=selftest_summary fails=%d\n", fails);
        std::fflush(stdout);
        if (fails != 0) {
            // Hard fail — selftest is opt-in (env-gated); when an operator
            // turned it on and a case failed, the predicate body has a real
            // boundary error and we must NOT continue into rendering.
            // Failing loudly here surfaces the bug; smoke runner reports
            // the abort as a failed mission.
            gosASSERT(false);
            std::abort();
        }
    }
}
```

(`#include <cstdlib>` and `#include <cstring>` if not already in the TU. `gosASSERT` is reachable via the existing project includes used elsewhere in `gameosmain.cpp`.)

- [ ] **Step 4.4 — Build, run selftest, verify**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
MC2_OBJECT_ADMISSION_SELFTEST=1 \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 5 --kill-existing
```

Expected log lines (8 cases listed in Task 3.1 + summary):

```
[INSTR v1] object_admission_mode=legacy
[OBJECT_ADMISSION v1] event=selftest_pass case=center_inside ...
[OBJECT_ADMISSION v1] event=selftest_pass case=behind_camera ...
... (6 more)
[OBJECT_ADMISSION v1] event=selftest_summary fails=0
```

If any case fails, the predicate body has a boundary error — fix in `object_admission_predicate.cpp`, rebuild, re-run.

- [ ] **Step 4.5 — Commit**

```bash
git add <gameosmain.cpp or whichever file was modified>
git commit -m "feat(track-a1): wire init probe + selftest at engine startup

MC2_OBJECT_ADMISSION_SELFTEST=1 runs eight boundary cases at startup and
prints [OBJECT_ADMISSION v1] event=selftest_pass|fail per case + summary.
[INSTR v1] object_admission_mode=legacy|modern emitted regardless.

Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
"
```

---

## Task 5 — Wire the wrapper to use the new predicate behind the env flag

**Files:**
- Modify: `mclib/camera.h:541-554` (the `projectForObjectAdmission` wrapper body)

- [ ] **Step 5.1 — Add the include**

Near the top of `mclib/camera.h`, after existing includes, add:

```cpp
#include "object_admission_predicate.h"
```

- [ ] **Step 5.2 — Replace the wrapper body**

Current body (lines 541-554):

```cpp
inline bool projectForObjectAdmission (Stuff::Vector3D& point,
                                       Stuff::Vector4D& screen) {
#pragma warning(push)
#pragma warning(disable: 4996)
    bool accepted = projectZ(point, screen);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
    if (accepted) {
        gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
                  isfinite(screen.z) && isfinite(screen.w));
    }
#endif
    return accepted;
}
```

Replace with:

```cpp
inline bool projectForObjectAdmission (Stuff::Vector3D& point,
                                       Stuff::Vector4D& screen) {
    LegacyProjectionResult result;
#pragma warning(push)
#pragma warning(disable: 4996)
    // projectZ writes screen byte-identically to legacy; we capture rawClip
    // via the optionalResult sidecar so the modern predicate can see it.
    bool legacyAccepted = projectZ(point, screen, &result);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
    // Invariant gates on legacy-rect-acceptance (the original semantics),
    // not on the bool we ultimately return — preserves the policy-split
    // contract from commit cc83857.
    if (result.acceptedByLegacyScreenRect) {
        gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
                  isfinite(screen.z) && isfinite(screen.w));
    }
#endif
    if (objectAdmissionPredicateMode() == ObjectAdmissionPredicateMode::Modern) {
        return clipSpaceFrustumAdmit(result.rawClip);
    }
    return legacyAccepted;
}
```

- [ ] **Step 5.3 — Build**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: clean build. If `LegacyProjectionResult` isn't visible, confirm its declaration is above the wrapper body in the same header (it is — verified at `mclib/camera.h:88` per Task 0 recon).

- [ ] **Step 5.4 — Tier1 smoke (legacy mode, sanity)**

The wrapper's behavior must be unchanged when env is unset (default = legacy). Confirm before flipping anything else:

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: tier1 5/5 PASS. `[INSTR v1] object_admission_mode=legacy` appears in each mission's log header.

- [ ] **Step 5.5 — Tier1 smoke (modern mode, first observation)**

```bash
MC2_OBJECT_ADMISSION_PREDICATE=modern \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: tier1 5/5 PASS, `[INSTR v1] object_admission_mode=modern` in each header. **If a mission fails:** capture artifact directory (`tests/smoke/artifacts/<timestamp>/`), inspect `[DESTROY v1]` line count delta and any visual smoke fail signal — these point at predicate-induced lifecycle differences. Do NOT proceed to Task 6 until tier1 passes under modern.

- [ ] **Step 5.6 — Commit**

```bash
git add mclib/camera.h
git commit -m "feat(track-a1): wire object admission wrapper to clip-space predicate

projectForObjectAdmission now captures rawClip via LegacyProjectionResult
and routes the bool decision through the env-flag-selected predicate.
Default (MC2_OBJECT_ADMISSION_PREDICATE unset) preserves legacy behavior
exactly — screen output is byte-identical via projectZ's existing path.

Tier1 5/5 PASS verified in both legacy (default) and modern modes.

Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
"
```

---

## Task 6 — Capture acceptance envelope from in-run dual-predicate trace

The Track A1 parity gate (per Q3 in the brainstorm decisions doc) is dual-run with a **reviewed acceptance envelope**: legacy and modern are expected to disagree at zoom-band / camera-angle / object-class boundaries; failure is unexpected disagreement, not non-zero disagreement.

**Methodology note (post-advisor-review):** the envelope is built from the per-callsite disagreement summaries inside a SINGLE smoke run (not from byte-comparing logs across two runs). The trace evaluates ALL 6 candidate predicates per `projectZ` call regardless of which bool the wrapper returned, so one run already gives the legacy-vs-modern comparison via the heatmap counters. A second run in the opposite mode is useful as a sanity-check (counts should be very close — the small drift comes from later-frame call counts shifting when the predicate changes admission flow), not as byte-identical evidence.

**Files:**
- Create: `docs/superpowers/specs/2026-05-06-track-a1-acceptance-envelope.md`

- [ ] **Step 6.1 — Capture single-run heatmap (tier1, modern mode)**

```bash
MC2_OBJECT_ADMISSION_PREDICATE=modern \
MC2_PROJECTZ_TRACE=1 MC2_PROJECTZ_HEATMAP=1 MC2_PROJECTZ_SUMMARY=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected output: per-mission `[PROJECTZ v1] callsite=gameobj_visibility_admit ... homogClipFull_disagreements=N total_calls=M` summary lines.

Capture the per-callsite summary subset for the envelope source:

```bash
grep "callsite=gameobj_visibility_admit" tests/smoke/artifacts/<latest>/*.log \
  > /tmp/track-a1-heatmap-summary.txt
cat /tmp/track-a1-heatmap-summary.txt
```

The envelope authoring uses these per-mission counts directly. Each line shows:
total `projectZ` calls at the callsite, plus `homogClipFull_disagreements` (where the modern predicate would have returned a different bool than legacy — i.e., the migration signal).

- [ ] **Step 6.2 — Sanity-check second run (tier1, legacy mode)**

```bash
MC2_PROJECTZ_TRACE=1 MC2_PROJECTZ_HEATMAP=1 MC2_PROJECTZ_SUMMARY=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
grep "callsite=gameobj_visibility_admit" tests/smoke/artifacts/<latest>/*.log \
  > /tmp/track-a1-heatmap-summary-legacy.txt
```

Compare the two summaries informally — they should be CLOSE but not byte-identical. The wrapper mode can affect later-frame `total_calls` because the predicate change shifts which objects hit `setExists(false)`/spawn/etc. on subsequent frames. Large counts drift (>20% per mission) indicate the predicate is causing lifecycle cascade — investigate before authoring the envelope.

```bash
# informal eyeball compare
paste /tmp/track-a1-heatmap-summary.txt /tmp/track-a1-heatmap-summary-legacy.txt
```

- [ ] **Step 6.3 — Author the acceptance envelope spec**

Write the disagreements observed at the `gameobj_visibility_admit` callsite, broken down by mission. Template:

```markdown
# Track A1 — Object Admission Acceptance Envelope

Captured: <date>
Source: tests/smoke/artifacts/<timestamp>/*.log
Predicate compared: homogClipFull (clipSpaceFrustumAdmit on rawClip) vs legacyRect

| Mission | Total calls | homogClipFull disagreements | %  |
|---------|-------------|------------------------------|-----|
| mc2_01  | <N>         | <D>                          | <P> |
| mc2_03  | <N>         | <D>                          | <P> |
| mc2_10  | <N>         | <D>                          | <P> |
| mc2_17  | <N>         | <D>                          | <P> |
| mc2_24  | <N>         | <D>                          | <P> |

## Candidate disagreement classes (hypotheses to confirm against capture data)

These are PREDICTED classes where the modern predicate should differ from
the screen-rect predicate by design. The envelope-authoring step is to
spot-check the captured disagreements against these hypotheses, refine
the list based on what's actually observed, and lock the refined list as
the envelope.

- **Behind-camera (rawClip.w ≤ 0):** modern rejects unconditionally; legacy
  accepts if the rhw-divided screen coord wraps into the viewport rect.
  Expect: modern_rejects-legacy_accepts disagreements concentrated here.
- **Inside frustum but rawClip.w very small:** modern accepts cleanly; legacy
  may reject if post-divide screen coord lands outside the viewport rect at
  extreme zoom-out. Expect: modern_accepts-legacy_rejects disagreements;
  the wolfman-zoom case for object admission specifically.
- **Far-plane boundary (rawClip.z ≈ rawClip.w):** modern admits inclusively;
  legacy depends on rect-overlap of the projected point. Smaller class than
  the above two.

After capture: confirm or refute each hypothesis. Add observed classes
that weren't predicted. Drop classes that didn't materialize.

## Out-of-envelope conditions (HARD FAILURE)

If any of the following appears in a future capture, Track A1 has a bug:

- Disagreement count delta >5x baseline at any single mission.
- Disagreements that don't fit into the (refined) class list above.
- `[DESTROY v1]` count delta >0 between legacy-mode and modern-mode tier1 runs
  (Task 7 — the cascade-safety check).

## Procedure for envelope refresh

Re-run Step 6.1 after any change to:
- The clipSpaceFrustumAdmit body
- Camera::projectZ (the legacy reference)
- The bucket of objects passing through gameobj_visibility_admit
  (e.g., a unit-roster change in stock missions)

If disagreement counts shift outside ±20% of envelope, gate the change.
```

Replace `<N>`, `<D>`, `<P>`, `<date>`, `<timestamp>` with values from the captures.

- [ ] **Step 6.4 — Commit the envelope**

```bash
git add docs/superpowers/specs/2026-05-06-track-a1-acceptance-envelope.md
git commit -m "docs(track-a1): author acceptance envelope from dual-run capture

Per Q3 of brainstorm-decisions: parity gate is dual-run with reviewed
envelope, not zero-disagreement. This doc captures the envelope at
post-Task-5 baseline; future Track A1-touching changes verify against it.

Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
"
```

---

## Task 7 — `[DESTROY v1]` parity verification (count + identity diff)

Object admission gates the `canBeSeen` chain which feeds `setExists(false)` lifecycle destruction (`memory/cull_gates_are_load_bearing.md`). Per Q3, `[DESTROY v1]` count delta is a **hard failure** — the modern predicate must not destroy more or fewer objects than legacy on tier1 stock content.

**Two-tier check (post-advisor-review):** count parity is the floor; identity parity (which specific objects destroyed) is the upper bound. Counts can match while different objects destroyed — same total, different victims. Identity diff catches that. The `[DESTROY v1]` line format from `code/gameobj.cpp:167,185` includes `obj=<ptr> kind=<kind> reason=<reason>`. Pointers are unstable across runs; `kind`+`reason`+per-mission ordering is the stable identity signal.

**Files:**
- Modify: `docs/superpowers/specs/2026-05-06-track-a1-acceptance-envelope.md` (append parity table)

- [ ] **Step 7.1 — Capture DESTROY events in legacy mode**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
# Per-mission count + per-mission normalized identity stream:
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log | wc -l \
    > /tmp/track-a1-destroy-legacy-${m}.count
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log \
    | sed -E 's/obj=0x[0-9a-fA-F]+/obj=PTR/; s/frame=[0-9]+/frame=N/' \
    > /tmp/track-a1-destroy-legacy-${m}.norm
done
```

The `sed` substitutions normalize away non-stable fields (heap pointer; frame number — frames can shift slightly between runs). What remains is `kind=`/`reason=`/`file=`/`line=` + the `in_view`/`can_be_seen`/`block_active`/`frames_since_active`/`last_update_ret` snapshot — those ARE stable across runs at the gameplay-state level and form the identity.

- [ ] **Step 7.2 — Capture DESTROY events in modern mode**

```bash
MC2_OBJECT_ADMISSION_PREDICATE=modern \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log | wc -l \
    > /tmp/track-a1-destroy-modern-${m}.count
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log \
    | sed -E 's/obj=0x[0-9a-fA-F]+/obj=PTR/; s/frame=[0-9]+/frame=N/' \
    > /tmp/track-a1-destroy-modern-${m}.norm
done
```

- [ ] **Step 7.3 — Compare counts (hard gate)**

```bash
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  diff /tmp/track-a1-destroy-legacy-${m}.count /tmp/track-a1-destroy-modern-${m}.count \
    && echo "${m}: count match" \
    || echo "${m}: COUNT MISMATCH"
done
```

Expected: every mission reports "count match." Any "COUNT MISMATCH" is a hard failure — Track A1 fails the parity gate; do NOT proceed to default-on.

- [ ] **Step 7.4 — Compare identity (sharper gate)**

```bash
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  diff /tmp/track-a1-destroy-legacy-${m}.norm /tmp/track-a1-destroy-modern-${m}.norm \
    > /tmp/track-a1-destroy-${m}.identity-diff
  if [ -s /tmp/track-a1-destroy-${m}.identity-diff ]; then
    echo "${m}: IDENTITY DIFF (count match but different victims) — review"
    head -20 /tmp/track-a1-destroy-${m}.identity-diff
  else
    echo "${m}: identity match"
  fi
done
```

Expected: every mission "identity match." If a mission has identity-diff with matching counts, it means the modern predicate caused the SAME number of objects to die but DIFFERENT objects — review the diff lines. This may indicate a real cascade-cousin: legacy admitted A and rejected B for some frame; modern admitted B and rejected A; same A-vs-B pair eventually both destroyed but via reversed pathways. Document the class in the acceptance envelope as a candidate disagreement class — if all observed identity-diffs fit the predicted classes from Task 6, this is acceptable; if novel destruction reasons appear, escalate.

**Limitation note:** if the project's `[DESTROY v1]` format gains stable per-object identity later (e.g., a `seq=<monotonic_id>` field), update the normalization sed to keep it. Currently the format-from-`code/gameobj.cpp:167,185` lacks a stable cross-run identity — the `kind+reason+gate-state-snapshot` tuple is the closest available proxy.

**If `[DESTROY v1]` is enriched in a future commit with stable identity, document the upgrade path:** add `seq=<id>` to the line format, drop the pointer-substitution from the sed, and the identity diff becomes line-exact. Until then, the kind+reason+gate-snapshot tuple is the documented limitation.

- [ ] **Step 7.5 — Document in the envelope**

If counts and identities match, append to `docs/superpowers/specs/2026-05-06-track-a1-acceptance-envelope.md`:

```markdown
## DESTROY parity (Task 7 capture, <date>)

| Mission | Legacy DESTROYs | Modern DESTROYs | Count? | Identity? |
|---------|-----------------|------------------|--------|-----------|
| mc2_01  | <N>             | <N>              | ✓      | ✓         |
| mc2_03  | <N>             | <N>              | ✓      | ✓         |
| mc2_10  | <N>             | <N>              | ✓      | ✓         |
| mc2_17  | <N>             | <N>              | ✓      | ✓         |
| mc2_24  | <N>             | <N>              | ✓      | ✓         |

Identity comparison: kind+reason+gate-state-snapshot tuple after pointer/frame
normalization. Format limitation noted: `[DESTROY v1]` line currently lacks
stable per-object cross-run identity; if a future commit enriches the line
format with a `seq=<id>` field, update the normalization sed and re-baseline.

DESTROY parity is the load-bearing cascade-safety check (per cull_gates_are_load_bearing.md).
Re-run on any Track A1-touching change.
```

```bash
git add docs/superpowers/specs/2026-05-06-track-a1-acceptance-envelope.md
git commit -m "docs(track-a1): record DESTROY count parity (legacy vs modern)

Tier1 stock missions show 0 lifecycle-cascade delta between legacy and
modern predicates — Track A1 parity gate's hard cascade-safety check
passes.

Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
"
```

---

## Task 8 — Soak observation period (no code change)

Per the modernization philosophy and the cull-cascade discipline, Track A1's flip-on does not happen until the modern path has been exercised under realistic gameplay for a meaningful interval. This task is a checklist, not code.

- [ ] **Step 8.1 — Run modern mode through one full mission start to mission end**

Pick `mc2_01` (shortest tier1 mission). Run with:

```bash
MC2_OBJECT_ADMISSION_PREDICATE=modern \
MC2_HEARTBEAT=1 \
  build64/RelWithDebInfo/mc2.exe -mission mc2_01
```

Play the mission to completion. Watch for:
- HUD overlays drifting on units (would indicate screen-output corruption — should be impossible since screen comes from legacy projectZ untouched)
- Units pop in/out at zoom transitions (modern predicate's expected behavior at frustum edges)
- Crashes, freezes, GL errors in console
- `[DESTROY v1]` lines for unexpected mech/building destruction

- [ ] **Step 8.2 — Wolfman-zoom canary**

Wolfman zoom is the worst-case for the legacy predicate (~87% false-negative on terrain admission per `memory/wolfman_is_max_zoom.md`). For object admission specifically, modern should keep more units visible at max zoom-out.

```bash
MC2_OBJECT_ADMISSION_PREDICATE=modern \
  build64/RelWithDebInfo/mc2.exe -mission mc2_24
```

In-mission: zoom out to wolfman level, observe whether buildings/objects that were missing under legacy now appear. Capture screenshot. Compare against an equivalent legacy-mode run.

- [ ] **Step 8.3 — Daily smoke for ≥3 days**

Run `MC2_OBJECT_ADMISSION_PREDICATE=modern py -3 scripts/run_smoke.py --tier tier1 --kill-existing` once per day for at least three days. Log results. Watch for non-deterministic regressions (e.g., a once-per-N-runs DESTROY count delta) — those indicate a race the predicate's behavior change exposed.

- [ ] **Step 8.4 — Consider AMD canary**

Per `docs/amd-driver-rules.md`, AMD RX 7900 XTX has had specific quirks with state changes. Run the soak builds on the AMD card explicitly. The predicate change shouldn't trigger any AMD-specific path, but cataloging this run is the discipline.

- [ ] **Step 8.5 — Decision point: flip default-on, or hold**

After 3+ days of clean soak:

- Tier1 5/5 PASS in both legacy and modern modes? → ✓
- DESTROY count delta = 0 across all soak runs? → ✓
- Acceptance envelope disagreement counts within ±20% of authored baseline? → ✓
- No visual regressions reported in any soak run? → ✓
- Wolfman canary shows at-least-equivalent visible-object count under modern? → ✓

**All five green:** proceed to Task 9 default-on flip.

**Any red:** hold the flip. Open a follow-up investigation. Update the acceptance envelope or fix the predicate, then restart Task 8 from Step 8.1.

---

## Task 9 — Default-on flip

After Task 8 soak passes, flip the default and remove the env flag's "legacy" path documentation (the env stays as a soak hedge for one more cycle).

**Files:**
- Modify: `mclib/object_admission_predicate.cpp` (default mode flips)

- [ ] **Step 9.1 — Flip default**

In `mclib/object_admission_predicate.cpp::objectAdmissionPredicate_init()`, change:

```cpp
const char* env = std::getenv("MC2_OBJECT_ADMISSION_PREDICATE");
if (env && std::strcmp(env, "modern") == 0) {
    s_mode = ObjectAdmissionPredicateMode::Modern;
} else {
    s_mode = ObjectAdmissionPredicateMode::Legacy;
}
```

To:

```cpp
const char* env = std::getenv("MC2_OBJECT_ADMISSION_PREDICATE");
if (env && std::strcmp(env, "legacy") == 0) {
    s_mode = ObjectAdmissionPredicateMode::Legacy;
} else {
    s_mode = ObjectAdmissionPredicateMode::Modern;  // default
}
```

- [ ] **Step 9.2 — Tier1 confirmation**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: 5/5 PASS, `[INSTR v1] object_admission_mode=modern` (no env set, modern is default).

- [ ] **Step 9.3 — Verify legacy still reachable as opt-out**

```bash
MC2_OBJECT_ADMISSION_PREDICATE=legacy \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 10 --kill-existing
```

Expected: passes, `[INSTR v1] object_admission_mode=legacy`.

- [ ] **Step 9.4 — Commit**

```bash
git add mclib/object_admission_predicate.cpp
git commit -m "feat(track-a1): flip object admission predicate default to modern

After 3+ days of clean soak with MC2_OBJECT_ADMISSION_PREDICATE=modern
across tier1 stock missions: zero DESTROY count delta, acceptance envelope
matched baseline, no visual regressions, wolfman zoom canary clean.

Modern is now default. MC2_OBJECT_ADMISSION_PREDICATE=legacy retained as
opt-out for at least one more soak cycle before Track E retirement.

Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
"
```

---

## Task 10 — Memory + index

**Files:**
- Create: `~/.claude/projects/A--Games-mc2-opengl-src/memory/track_a1_object_admission_predicate.md`
- Modify: `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

- [ ] **Step 10.1 — Write the memory file**

Path: `C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\track_a1_object_admission_predicate.md`

```markdown
---
name: track_a1_object_admission_predicate
description: Track A1 shipped — object admission uses clip-space frustum predicate; screen output remains byte-identical via legacy projectZ math; default-on after 3-day soak
type: project
---

Track A1 of the MC3 rendering modernization arc shipped on <date>. Replaces the
rect-screen-finite predicate at the single object-admission callsite
(`code/gameobj.cpp:2090` → `Camera::projectForObjectAdmission`) with a
clip-space frustum test. Screen output (`screen.x/y/z/w` consumed by HUD
overlays in `mclib/mech3d.cpp`, `mclib/gvactor.cpp`, `code/weaponbolt.cpp`,
`code/actor.cpp`, `mclib/bdactor.cpp`, `code/missiongui.cpp`) is preserved
byte-identical via `LegacyProjectionResult` capture from `projectZ`.

**Env flag (still present as opt-out):** `MC2_OBJECT_ADMISSION_PREDICATE=legacy`
restores the screen-rect predicate. Default = modern.

**Self-test:** `MC2_OBJECT_ADMISSION_SELFTEST=1` runs 8 boundary cases at startup,
prints `[OBJECT_ADMISSION v1] event=selftest_*` lines.

**Acceptance envelope:** documented at
`docs/superpowers/specs/2026-05-06-track-a1-acceptance-envelope.md` with
per-mission disagreement counts and DESTROY parity confirmation.

**Why this matters:** Track A1 was the validation slice for the dual-output
wrapper pattern. Track A2 (effects admission, 7 sites) inherits the same
pattern. Track A3 (terrain admission, 6 sites) is conditional on data review
post-A1+A2.

**Files touched:**
- `mclib/object_admission_predicate.{h,cpp}` — predicate + env probe + selftest
- `mclib/camera.h:541-554` — wrapper body change (LegacyProjectionResult capture)
- `mclib/projectz_trace.{h,cpp}` — added `homogClipFull` candidate
- `GameOS/gameos/gameosmain.cpp` (or equivalent) — startup probe call

**Roadmap:** `docs/superpowers/mc3-rendering-modernization-roadmap.md` (Track A § A1).
```

Replace `<date>` with the actual ship date.

- [ ] **Step 10.2 — Index in MEMORY.md**

Add to the "Rendering / shaders" section of `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`:

```
- ⭐ [Track A1 shipped — object admission clip-space frustum (<date>)](track_a1_object_admission_predicate.md) — first slice of MC3 modernization Track A; dual-output wrapper validates the swap pattern; default-on, env opt-out for soak
```

The index entry must stay under ~200 chars per the MEMORY.md rules.

- [ ] **Step 10.3 — Commit**

```bash
# Memory and MEMORY.md live outside the worktree, so commit must be from
# the appropriate repo if memory is git-tracked. If memory is unversioned
# (per project convention), this step is a save-only.
```

---

## Self-Review (run before declaring complete)

**Spec coverage:**
- Q1 (entry slice = object admission) → Tasks 5, 7, 8 (all touch `gameobj.cpp:2090`).
- Q2 (dual-output wrapper) → Task 5 specifically (LegacyProjectionResult capture).
- Q3 (parity definition: dual-run + envelope + DESTROY parity + screen byte-identity) → Tasks 6 (envelope) + 7 (DESTROY) + Task 5 (screen byte-identity preserved by `projectZ` reuse).
- Killswitch → Task 3 + Task 9 (default flip).

**Placeholder scan:** none — every step has exact code or exact commands. The acceptance envelope template (Task 6.3) has placeholders for measured values, which is correct (the values come from running the captures).

**Type consistency:** `ObjectAdmissionPredicateMode` (enum class) and `objectAdmissionPredicateMode()` (accessor) match between Task 2 (header), Task 3 (impl), and Task 5 (wrapper consumer). `clipSpaceFrustumAdmit(const Stuff::Vector4D&)` signature matches across files. `LegacyProjectionResult::rawClip` is the documented field per Task 0 recon (verified at `mclib/camera.h:91`).

**Open question reminder:** Task 6.3's acceptance envelope authoring is operator-side — the plan describes what to capture and how to format it. The actual numbers come from running the captures.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md`. Two execution options:

**1. Subagent-Driven (recommended)** — fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — execute tasks in this session using `executing-plans`, batch with checkpoints for review.

Which approach?
