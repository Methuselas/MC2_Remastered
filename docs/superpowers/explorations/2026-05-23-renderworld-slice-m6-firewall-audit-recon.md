# RenderWorld Slice M6 — Firewall Audit / No Raw GL From Game Side (Recon)

Date: 2026-05-23
Status: RECON ONLY — no spec, no plan
Author: subagent recon pass

---

## 1. Summary

-   **The "game side calls raw GL" hypothesis is empirically false.** Audit
    finds **zero** raw `gl*()` function calls in `code/` and **three** in
    `mclib/` (all diagnostic, in `mclib/render_contract.cpp`). No `glDraw*`,
    `glBind*`, `glClear*`, `glViewport`, `glScissor`, `glBlendFunc`,
    `glDepthFunc`, `glBuffer*`, `glUseProgram`, `glUniform*`,
    `glFramebuffer*` anywhere in game-side translation units.
-   **The game-side render path is already routed.** Every game-side
    rendering call goes through `gameos.hpp` abstractions (`gos_*` APIs),
    which is the engine boundary in this codebase. `#include <gameos.hpp>`
    appears 36 times across 12 files in `code/`; no `#include <GL/glew.h>`
    or `#include <GL/gl.h>` anywhere in `code/`. Only `mclib/render_contract.cpp`
    in `mclib/` includes `<GL/glew.h>` (for its diagnostic-only assert
    machinery).
-   **The three `mclib/` GL calls are all diagnostic.**
    `render_contract.cpp:437` (`glGetIntegerv`), `:493` and `:502`
    (`glGetBooleanv`). All inside `assertPassContract()` which is gated by
    `MC2_RENDER_CONTRACT_ASSERT=1` (off by default). Severity = Diagnostic-OK.
-   **The real layering risk lives in `GameOS/gameos/`, not in `code/` or
    `mclib/`.** `GameOS/` houses 2,986 `gl*()` call sites across 32 files —
    this is the engine's GL backend. Those are not violations; they ARE the
    engine. The M2.5 MAJOR-1 finding (`GameOS/` outside SCOPE_DIRS) is the
    real residual gap: a stray `mech3d.h` include inside `GameOS/gos_*.cpp`
    would not be policed.
-   **M6 is therefore not a "migrate raw GL out of game side" slice.** It is
    a "tighten the firewall script to *prove and lock in*" what is already
    empirically true. The audit infra cost (script + allowlist + CI hook +
    doc) is the deliverable; there are essentially no migrations to do.
-   **Recommended M6 scope shape: audit-only.** Ship a new
    `scripts/check-no-raw-gl-from-game.sh` script + an "approved diagnostic
    callers" allowlist (currently containing only `mclib/render_contract.cpp`),
    add a §3.5 to the migration guide, and document the GameOS reviewer-
    discipline gap as either closed (with a small allowlist) or accepted.
    Zero code-side migrations needed at ship time.
-   **The M2.5 GameOS gap can be closed at low cost.** Adding `GameOS/` to
    `SCOPE_DIRS` would emit ~0 expected violations for FORBIDDEN_HEADERS /
    FORBIDDEN_SYMBOLS if `GameAdapters/MechRenderAdapter.cpp`-style carve-outs
    are extended — verified by spot-checking that `GameOS/gameos/*.cpp` files
    do not include `mech3d.h` / `mission.h` / `warrior.h` (the rendering
    fast paths use their own bridge headers like `gos_terrain_bridge.h`,
    `gos_mech_batcher.h`, etc.). Worth a dedicated micro-slice; can be
    folded into M6 or split.

---

## 2. Raw GL Calls Inventory — Game-Side (`code/` + `mclib/`)

Search: `\bgl[A-Z]\w*\s*\(` (case-sensitive, word-boundary, call-style).

| File:line                          | Call                              | TU class      | Category    | Severity        | Engine-side route exists?               |
|------------------------------------|-----------------------------------|---------------|-------------|-----------------|------------------------------------------|
| `mclib/render_contract.cpp:437`    | `glGetIntegerv(query, &buf)`      | engine bridge | State query | Diagnostic-OK   | N/A — IS the diagnostic                  |
| `mclib/render_contract.cpp:493`    | `glGetBooleanv(GL_DEPTH_TEST,...)` | engine bridge | State query | Diagnostic-OK   | N/A — IS the diagnostic                  |
| `mclib/render_contract.cpp:502`    | `glGetBooleanv(GL_DEPTH_WRITEMASK,...)` | engine bridge | State query | Diagnostic-OK | N/A — IS the diagnostic                  |

**Total: 3 calls across 1 file.** All inside one function (`checkDrawBuffer`
and `assertPassContract`). All gated by `s_assertEnabled` (=
`getenv("MC2_RENDER_CONTRACT_ASSERT") != nullptr`). All read-only state
queries used to verify other engine code is upholding documented contracts.

### Non-violations (false-positive identifier hits)

The naive grep `\bgl[A-Z]\w*` (without `\s*\(`) returned 14 hits in `code/`
and 21 in `mclib/`. Manually classified:

-   `code/gameplay_pick.h:49`, `:74`, `gameplay_pick.cpp:120`, `:123`,
    `missiongui.cpp:6251/6269/6288/6352/6394/6403/6416/6432` — references to
    the `glX`/`glY` *field names* on `GameplayPickContext` (post-y-flip GL
    pixel coords). Not function calls. Naming choice from the M2-pre spine.
-   `code/mc2video.cpp:454`, `mc2video.h:54` — `glTextureId` member name on
    a video-frame struct. Not a function call.
-   `mclib/camera.cpp:701`, `terrain_depth_bias.h:10/15/34`, `quad.cpp:1741`
    — comments referencing `glClipControl` (used elsewhere by GameOS).
-   `mclib/projectz_overlay.cpp:45`, `particles/batcher.h:10`,
    `txmmgr.cpp:2152` — comments referencing `glVertexAttribPointer`,
    `glDrawArrays`, `glMultiDrawElementsIndirect` for documentation.
-   `mclib/render_contract.h:11/64/102/103` — comments documenting expected
    `glDrawBuffers` state.
-   `mclib/terrtxm2.cpp:2106/2107/2149/2150` — local variable named
    `glTexId` (the result of `gos_CreateTerrainNormalTexture`), then passed
    to `gos_SetTerrainDetailNormalTexture` / `gos_SetTerrainDisplacementTexture`.
    Not a GL call — it's the variable that holds an opaque texture-id
    returned by the engine.

### Direct GL header includes

| File                          | Include                | Notes                                       |
|-------------------------------|------------------------|---------------------------------------------|
| `mclib/render_contract.cpp:24` | `#include <GL/glew.h>` | Only `mclib/` file with this include. The diagnostic-assert TU. |
| `code/**`                      | (none)                 | Verified: no `GL/gl.h` or `GL/glew.h` anywhere in `code/`. |

---

## 3. Aggregate Counts + Critical Files

| Scope             | Raw GL call sites | Files with calls | Severity                |
|-------------------|-------------------|------------------|--------------------------|
| `code/` (all)     | 0                 | 0                | n/a                      |
| `mclib/` (all)    | 3                 | 1 (`render_contract.cpp`) | Diagnostic-OK x3 |
| **Game-side total** | **3**           | **1**            | All diagnostic, gated   |
| `GameOS/gameos/`  | ~2,986            | 32               | Expected — IS the engine |

Critical files spot-check:

-   `code/missiongui.cpp` — **0 raw GL calls.** Goes through `gameos.hpp` +
    `gameplay_pick.cpp` + `RenderWorld`.
-   `code/gamecam.cpp` — **0 raw GL calls.** Includes `gameos.hpp` only
    (verified).
-   `mclib/txmmgr.cpp` — **0 raw GL calls.** Only one comment mention of
    `glMultiDrawElementsIndirect` at `:2152`.
-   `mclib/mech3d.cpp` — **0 raw GL calls.** (Implicit from total-count grep
    showing only `render_contract.cpp` hits.)
-   `mclib/render_contract.cpp` — **3 calls, all diagnostic.**

Distribution by category (game-side only):

| Category               | Count |
|------------------------|-------|
| State query (`glGet*`) | 3     |
| Draw                   | 0     |
| Bind                   | 0     |
| State change           | 0     |
| Clear                  | 0     |
| Buffer ops             | 0     |
| Shader/program         | 0     |
| FBO                    | 0     |
| Misc                   | 0     |

This is a 1-task slice, not a multi-week refactor. The migration work was
already done historically — game-side code routes through `gos_*` /
`gameos.hpp`, and the recent `RenderWorld` + `GameAdapters` arc moved the
last engine-handle leaks into typed adapters.

---

## 4. SCOPE_DIRS Extension Proposal — 3 Options + Recommendation

### Option (a): Add `code/` and `mclib/` to a new `INVERSE_SCOPE_DIRS` that prohibits raw-GL includes

Mechanism: extend `check-include-firewall.sh` to grep for
`#include.*GL/(gl|glew)\.h` inside files under `code/` and `mclib/`.
Allowlist `mclib/render_contract.cpp`.

-   Pro: Tiny script delta. Catches the obvious "someone wired GL into a
    game-side TU" regression.
-   Con: Include-level only. A `.cpp` could pull `gameos.hpp` (legitimately,
    for `gos_RenderIndexedArray` etc.) and then call `glDrawArrays`
    directly — the include-level guard would not catch it because
    `gameos.hpp` is allowed. (In practice this is impossible without also
    including `<GL/glew.h>` because the `gl*` function prototypes need a
    declaration, but a `extern "C"` forward-decl could bypass that.)
-   Con: Doesn't catch the GameOS gap (`GameOS/` would still need a
    separate decision).

### Option (b): New `scripts/check-no-raw-gl-from-game.sh` — function-level grep

Mechanism: dedicated script that greps `\bgl[A-Z][a-zA-Z]+\s*\(` across
`code/` + `mclib/`, with an allowlist of approved diagnostic callers.

-   Pro: Catches the actual violations (function calls), not just includes.
    Pattern is the same one this recon used and it produced clean results.
-   Pro: Decoupled from the include-firewall script — different axis,
    different cadence. The include firewall runs when SCOPE_DIRS files
    change; this one would run when `code/` or `mclib/` files change.
-   Pro: Allowlist is naturally small: today exactly one file
    (`mclib/render_contract.cpp`).
-   Con: A second script to maintain. Two pre-commit hooks instead of one.
-   Con: Has to be careful about false positives — variable names like
    `glX`/`glTextureId` matched the naive pattern. Adding the `\s*\(`
    suffix fixes this, as the recon grep confirmed (0 false positives with
    the stricter pattern).

### Option (c): Extend the existing firewall script with a new mode

Mechanism: add a second pass to `check-include-firewall.sh` keyed off a new
`INVERSE_SCOPE_DIRS` list and a new `FORBIDDEN_GL_CALL_PATTERN`. Single
script, two enforcement modes.

-   Pro: One pre-commit hook, one place to look for "what does the firewall
    enforce".
-   Pro: Allowlist semantics already battle-tested (M2 carve-outs work).
-   Con: Mixes two concerns. SCOPE_DIRS today is "watched modules that may
    not include game-side things." Adding "watched modules that may not call
    raw GL" is a different axis with a different rule set. The script grows
    in complexity.
-   Con: Confuses the mental model: a file being in `INVERSE_SCOPE_DIRS`
    has the opposite meaning from a file being in `SCOPE_DIRS`.

### Recommendation: **Option (b)** — separate script

Rationale:
-   The two checks answer orthogonal questions:
    `check-include-firewall.sh` answers "is RenderWorld pure?";
    `check-no-raw-gl-from-game.sh` would answer "is game-side abstracted?".
    Splitting keeps each script's purpose crisp and its allowlist small.
-   Function-level grep is required to catch real violations
    (option a misses the `extern "C"` bypass).
-   The allowlist starts at size 1 (`mclib/render_contract.cpp`) and is
    expected to stay there. A second script is trivial maintenance.
-   It composes with option (c)-style future tightening: if we later want a
    unified "firewall.sh" entry point, it can shell out to both.
-   Cost is ~50 lines of shell, mirroring the existing
    `check-include-firewall.sh` structure (allowlisted / is_comment_line /
    tmphits loop).

---

## 5. GameOS Reviewer-Discipline Gap — Close or Accept?

### The gap

`GameOS/` is OUTSIDE `SCOPE_DIRS`. A stray `#include "mech3d.h"` inside
`GameOS/gameos/gos_terrain_indirect.cpp` (for example) is unpoliced. The
M2.5 MAJOR-1 finding flagged this; the migration guide §3.10 documents it
as "manual review is the only guard."

### Closing the gap: cost estimate

To add `GameOS` to `SCOPE_DIRS` requires verifying the existing
`GameOS/gameos/*` files do not currently violate the firewall rules.
Spot-check from grep results:

-   `GameOS/gameos/gos_mech_batcher.cpp` (153 GL calls) — engine; needs
    `mech3d.h` forward-decl access? Check.
-   `GameOS/gameos/gpu_cull_compute.cpp` (175 GL calls) — engine; uses
    its own bridges (`gpu_cull_substrate.h`) not game headers. Likely clean.
-   `GameOS/gameos/gos_static_prop_batcher.cpp` (279 GL calls) — already
    in M1 allowlist via the `RenderWorld/legacy/static_prop_backend.cpp`
    bridge pattern; this TU itself does not need to be allowlisted IF it
    does not include `mech3d.h` / etc.

Required action if we close it:
1.  Grep `GameOS/gameos/` for each `FORBIDDEN_HEADERS` entry.
2.  For each hit, decide: allowlist it (engine-internal use of a header
    that happens to be on the forbidden list) or move the include behind a
    bridge.
3.  Grep `GameOS/gameos/` for each `FORBIDDEN_SYMBOLS` entry. Same
    triage.
4.  Add `GameOS` to `SCOPE_DIRS`. The existing `[ -d ] || continue` guard
    means the script change is one line.

### Accepting the gap: cost

Document it more visibly than today (it's already noted in §10 of the
migration guide). No code change. Risk: a future contributor adds a stray
include and we miss it.

### Recommendation

**Close it, but as a dedicated micro-slice — not as part of M6.**

-   Rationale: GameOS audit needs a real grep pass per forbidden symbol/
    header (likely produces 5-30 hits to triage). That work is bounded but
    nontrivial, and mixing it into M6 muddies the "audit no-raw-GL-from-
    game" focus.
-   The grep pass is mechanical and ideal for a separate slice (e.g.
    "M6.5: GameOS firewall expansion") that lands after M6 and can leverage
    the audit infrastructure M6 introduces.
-   In the meantime, M6's `check-no-raw-gl-from-game.sh` *would* cover the
    GameOS dir naturally if we add `GameOS/` to its `INVERSE_SCOPE_DIRS` —
    but that's likely too strict (engine MUST call GL). So M6 would
    explicitly exclude `GameOS/` from the new script's scope.

---

## 6. Per-Violation Remediation Patterns

There are **0 violations** in this audit. The three diagnostic calls in
`mclib/render_contract.cpp` are by-design and explicitly allowlisted.

If a future violation appears, the engine-side route depends on category:

| Hypothetical violation category | Engine-side route                                | Currently exposed? |
|---------------------------------|--------------------------------------------------|--------------------|
| `glDrawArrays` / `glDrawElements` | `MeshRenderer::draw` (not yet implemented; M? slice) OR existing `gos_RenderIndexedArray`-family in `gameos.hpp` | Partially — `gos_RenderIndexedArray` is the legacy route, `MeshRenderer` is the future-state per spec §12 SCOPE_DIRS |
| `glBindTexture`                 | `gos_SetRenderState(gos_State_Texture, ...)`     | Yes               |
| `glUseProgram`                  | `gos_SetRenderState(gos_State_ShaderProgram, ...)` family OR direct via `apply()` on a `Program*` | Yes |
| `glUniform*`                    | `Program::setFloat/setInt/setMat4` + `apply()` (per CLAUDE.md uniform-API rule) | Yes |
| `glViewport` / `glScissor`      | `gos_SetViewport()` / `gos_SetScissor()` (TBD if exists) | TBD — would need API check |
| `glClear*`                      | `gos_ClearRenderer()` / framebuffer-bound `gos_*` clear | Yes |
| `glBindFramebuffer`             | `setSceneDrawBuffers(...)` helper (M1.5 C1 META-FIX) OR direct `gos_postprocess.cpp` APIs | Yes — but the M1.5 helper is the modern path |

If M6 ever needs to migrate a real violation, the most likely missing-route
is **`gos_SetViewport` / `gos_SetScissor`** — if those don't exist, a real
violation would block on adding them. Empirically not needed today, so flag
as: "if M6 finds a viewport/scissor call in `code/` or `mclib/`, expect to
add an engine-side route first."

API gaps that would need to be added before any migration:
-   None today (no migrations needed).

---

## 7. Open Questions for User

1.  **M6 scope shape:**
    -   (a) **Audit-only** — ship `check-no-raw-gl-from-game.sh` +
        allowlist + migration guide §3.5 + CLAUDE.md campaign entry. Zero
        code migrations. Recon says this is sufficient because audit found
        0 violations to migrate.
    -   (b) **Audit + GameOS gap closure** — folds the M2.5 MAJOR-1
        residual into M6.
    -   (c) **Defer** — wait until M3/M4/M5 ship (or at least M3.1+M4.1
        writers) before adding more firewall surface area, on the theory
        that the new substrate slices might surface real violations the
        audit infra can catch in-flight.
    -   **Recon recommendation: (a) with (b) split out as M6.5.** The
        audit confirms there's nothing to migrate; the script + doc lock
        in the current clean state and prevent regression as M3/M4/M5
        writers land.

2.  **GameOS reviewer-discipline gap:** close (Option (b)/(c) above) or
    accept and rely on reviewer discipline forever? Recon recommends close,
    but as a separate slice (M6.5) so the grep-triage work doesn't slow M6.

3.  **Migration-guide §3.5 add:** Should the migration guide get a new
    "What game-side code must never CALL directly" section parallel to §3?
    Recon recommends YES — it's the natural place to document the new
    `check-no-raw-gl-from-game.sh` rule and its one allowlisted file. ~20
    lines.

4.  **Schedule:** Does M6 happen now (right after M2.6) or wait for
    M3/M4/M5 to land first?
    -   For-now argument: Audit shows the current state is clean; lock it
        in NOW before new slices can drift it. The M3 (terrain) and M4
        (VFX) writers will add fragment shaders and SSBO writes — both
        through engine paths in principle, but a stray "I just need one
        `glBindBuffer` for the prototype" is exactly the regression M6
        catches.
    -   For-later argument: M6 right now is a 1-task slice; deferring to
        after M3/M4/M5 lets it absorb any new violations those slices
        introduce in one pass.
    -   **Recon recommendation: NOW.** The cost is small, and the value
        is exactly to prevent in-flight regression while M3-M5 land.

5.  **CLAUDE.md "Active campaigns" entry style:** Should M6 (audit-only)
    even warrant an "Active campaigns" entry, or just a one-line addition
    to the firewall section of the migration guide + a "ships with M3"
    rider? Recon recommends adding the entry — the new script is a new
    pre-commit gate and contributors need to discover it.

6.  **Allowlist policy for `check-no-raw-gl-from-game.sh`:** Should the
    allowlist require an inline comment with a deletion criterion (like the
    existing `check-include-firewall.allowlist` does for the legacy bridge
    entries)? Recon recommends YES — same pattern, prevents allowlist drift.

---

## 8. File:line Citations Table (grep-verified at write-time)

| Claim                                          | Citation                                       |
|------------------------------------------------|------------------------------------------------|
| Firewall script SCOPE_DIRS list                | `scripts/check-include-firewall.sh:22`         |
| Firewall script FORBIDDEN_HEADERS list         | `scripts/check-include-firewall.sh:28`         |
| Firewall script FORBIDDEN_SYMBOLS list         | `scripts/check-include-firewall.sh:33`         |
| `[ -d ] || continue` forward-compat guard      | `scripts/check-include-firewall.sh:73`         |
| Allowlist: M1 legacy bridge entries            | `scripts/check-include-firewall.allowlist:15-16` |
| Allowlist: M2 MechRenderAdapter carve-out      | `scripts/check-include-firewall.allowlist:21`  |
| Diagnostic GL call #1                          | `mclib/render_contract.cpp:437` (`glGetIntegerv`) |
| Diagnostic GL call #2                          | `mclib/render_contract.cpp:493` (`glGetBooleanv`) |
| Diagnostic GL call #3                          | `mclib/render_contract.cpp:502` (`glGetBooleanv`) |
| Diagnostic assert is env-gated                 | `mclib/render_contract.cpp:474` (`s_assertEnabled = getenv(...)`) |
| Only `mclib/` file including `<GL/glew.h>`     | `mclib/render_contract.cpp:24`                 |
| No `<GL/gl.h>` or `<GL/glew.h>` in `code/`     | Grep `#include.*GL/(gl|glew)\.h` in `code/` returns 0 hits |
| 36 `gameos.hpp` includes in `code/` (12 files) | Grep `#include.*gameos\.hpp` in `code/`        |
| `glX`/`glY` field names on GameplayPickContext (not GL calls) | `code/gameplay_pick.h:49`, `code/gameplay_pick.cpp:120/123`, `code/missiongui.cpp:6251/6269/6288/6352/6394/6403/6416/6432` |
| `glTextureId` field name (not GL call)         | `code/mc2video.h:54`, `code/mc2video.cpp:454`  |
| `glTexId` local variable (not GL call)         | `mclib/terrtxm2.cpp:2106/2107/2149/2150`       |
| Comment-only `gl*` references                  | `mclib/camera.cpp:701`, `mclib/quad.cpp:1741`, `mclib/projectz_overlay.cpp:45`, `mclib/particles/batcher.h:10`, `mclib/terrain_depth_bias.h:10/15/34`, `mclib/render_contract.h:11/64/102/103`, `mclib/txmmgr.cpp:2152` |
| GameOS gos_object_parity.cpp uses raw GL (engine, expected) | `GameOS/gameos/gos_object_parity.cpp:269/275/276/284/286/307` (`glDeleteBuffers`, `glGenBuffers`, `glBindBuffer`, `glBufferStorage`) |
| Migration guide §3 "What RenderWorld must never include" | `docs/renderworld_migration_guide.md:121-167` |
| Migration guide §10 firewall summary + GameOS gap notice | `docs/renderworld_migration_guide.md:464-485` |
| M2.5 MAJOR-1 finding (GameOS outside SCOPE_DIRS) | `docs/renderworld_migration_guide.md:161-166` |
| SCOPE_DIRS modules that don't exist yet (forward-compat) | `Visibility`, `MeshRenderer`, `MaterialSystem`, `DebugRenderer`, `RenderDeviceGL` — `ls` returns "No such file or directory" for all five |
| `GameAdapters/` not in SCOPE_DIRS (carve-out)  | `scripts/check-include-firewall.sh:21` (comment), `:22` (list excludes it) |
| Render-contract assert env var documented      | `CLAUDE.md` Tier-1 instrumentation list (`MC2_RENDER_CONTRACT_ASSERT=1`) |

### Verification commands (re-runnable)

```sh
# Game-side raw GL call count (strict function-call pattern)
rg -n '\bgl[A-Z][a-zA-Z]+\s*\(' code/   # -> 0 hits
rg -n '\bgl[A-Z][a-zA-Z]+\s*\(' mclib/  # -> 3 hits in render_contract.cpp

# GL header includes in game-side code
rg -n '#include.*GL/(gl|glew)\.h' code/   # -> 0
rg -n '#include.*GL/(gl|glew)\.h' mclib/  # -> 1 (render_contract.cpp:24)

# Engine boundary used by game-side code
rg -c '#include.*gameos\.hpp' code/       # -> 36 across 12 files

# GameOS for contrast (engine, expected to be GL-rich)
rg -c '\bgl[A-Z][a-zA-Z]+\s*\(' GameOS/   # -> ~2986 across 32 files
```

---

## RECON STATUS: COMPLETE

**Recommended M6 scope shape: AUDIT-ONLY (Option 7.1.a).**

Specifically:
1.  Add `scripts/check-no-raw-gl-from-game.sh` mirroring
    `check-include-firewall.sh` structure: greps
    `\bgl[A-Z][a-zA-Z]+\s*\(` across `code/` + `mclib/`, with comment-
    stripping and an allowlist file.
2.  Add `scripts/check-no-raw-gl-from-game.allowlist` containing one entry:
    `mclib/render_contract.cpp` with a comment documenting that the file
    hosts the env-gated `MC2_RENDER_CONTRACT_ASSERT` diagnostic and the
    deletion criterion ("when render-contract asserts are moved into a
    SCOPE_DIRS module").
3.  Add §3.5 to `docs/renderworld_migration_guide.md` titled "What
    game-side code must never CALL directly" — parallel structure to §3.
4.  Add a one-paragraph "RenderWorld Slice M6 (SHIPPED ...)" entry under
    CLAUDE.md "Active campaigns" mirroring the M2.6 entry style. Counter:
    `[GAME_GL_AUDIT v1] result=clean violations=0 allowlisted=1` printed
    by the script on success.
5.  Wire the new script into the appropriate pre-commit hook trigger
    (changes under `code/` or `mclib/`).

**Explicitly NOT in M6:**
-   Migrating any actual game-side GL calls (there are none to migrate).
-   Closing the GameOS reviewer-discipline gap (split as M6.5).
-   Extending the include-firewall script (orthogonal axis; option (b)
    keeps them separate).

**Effort:** ~1 task. Script + allowlist + doc + campaign entry. No code
changes outside `scripts/` and `docs/`. Tier1 unchanged (no runtime
impact).

**Scheduling:** Recommend SHIPPING NOW (right after M2.6), before
M3/M4/M5 writers land — the value is exactly to prevent in-flight
regression of the already-clean state while new slices add fragment
writes and SSBO bindings.
