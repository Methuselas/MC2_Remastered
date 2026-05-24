# RenderWorld Slice M6 — Firewall Audit Spec

**Date:** 2026-05-24
**Shape:** Audit-only 1-task slice. Codifies the empirical finding from
[M6 recon](../explorations/2026-05-23-renderworld-slice-m6-firewall-audit-recon.md)
into CI enforcement. No code migrations needed.

**Status:** READY FOR EXECUTE (user-resolved via external advisor 2026-05-24)

---

## 1. Purpose / non-goals

**Purpose:** Turn the "no raw GL from game side" discipline from
reviewer-memory into mechanical CI enforcement. The recon proved the
hypothesis is empirically false at HEAD (0 hits in `code/`, 3 diagnostic
gated hits in `mclib/render_contract.cpp`), so M6 is pure lockdown of
the clean state, not migration work.

**Non-goals:**
- NO source code migrations (none needed)
- NO GameOS coverage (deferred to M6.5 if user wants; see Resolved Q2)
- NO changes to existing `check-include-firewall.sh` (orthogonal axis)
- NO new env vars
- NO shader edits
- NO RenderObjectKind additions

---

## 2. Resolved decisions (per external advisor 2026-05-24)

| Q | Resolution | Source |
|---|---|---|
| Q1 — audit-only NOW vs defer | **Audit-only NOW** | Advisor: "M6 looks like the best next executable slice" |
| Q2 — GameOS gap close as M6.5 vs accept | **Defer as M6.5 (or accept)** | Advisor: "maybe expand firewall coverage toward GameOS reviewer discipline" — listed as optional follow-up. M6 ships without it; M6.5 spec spawnable later. |
| Q3 — migration-guide §3.5 add | **YES, ~20 lines** | Recon lean; advisor implicit confirm via "migration guide update" item |
| Q4 — schedule NOW vs LATER | **NOW** | Advisor: "lock in clean state before M3-M5 writers can drift it" |

---

## 3. Deliverables (one task)

### 3.1 New audit script

File: `scripts/check-no-raw-gl-from-game.sh`

Function-level grep (NOT include-level — includes can be there for
legitimate constants/types while CALLS are the actual violations).

Pattern: `\bgl[A-Z][a-zA-Z]+\s*\(`

Scope: `code/` and `mclib/` (game-side TUs)

Allowlist: one entry only — `mclib/render_contract.cpp` (diagnostic
state queries gated by `MC2_RENDER_CONTRACT_ASSERT=1`).

Output: zero hits = exit 0; any non-allowlisted hit = exit nonzero with
filename + line + matched function.

False-positive guard: pattern is anchored on word-boundary + GL naming
convention (`gl[A-Z]...`). Per recon, false-positive hits during the
naive audit included `glX/glY` field names (M2-pre `GameplayPickContext`),
`glTextureId` (mc2video struct field), `glTexId` (opaque-id local). The
function-call anchor `\s*\(` after the identifier eliminates these
false positives.

### 3.2 Allowlist file (or inline in script)

File: `scripts/check-no-raw-gl-from-game.allowlist`

```
# Diagnostic-only TU: gated by MC2_RENDER_CONTRACT_ASSERT=1; never executes rendering.
mclib/render_contract.cpp
```

If a future contributor adds a legitimate diagnostic GL query elsewhere,
they add the file + a justification comment line to the allowlist.

### 3.3 Migration guide §3.5 add

File: `docs/renderworld_migration_guide.md`

New section after the existing §3 ("What RenderWorld must never include"):

```
## 3.5 What game-side code must never CALL directly

Game-side code (`code/`, `mclib/`) must NOT call raw OpenGL functions
(`gl*()`). Rendering routes through engine abstractions:
MeshRenderer / MaterialSystem / RenderWorld / GpuStaticPropBatcher /
GpuMechBatcher / GameAdapters.

**Diagnostic exception:** `mclib/render_contract.cpp` may call read-only
GL state queries (`glGetIntegerv`, `glGetBooleanv`) inside the
`assertPassContract` machinery gated by `MC2_RENDER_CONTRACT_ASSERT=1`.
This is the ONLY exception, enforced by allowlist.

**Enforced by:** `scripts/check-no-raw-gl-from-game.sh` (CI / pre-commit).

**Why this matters:** Engine routing is what makes future Vulkan/Metal
migration feasible. Direct GL calls from game-side code couple game
logic to the GL API surface, defeating MeshRenderer / RenderDeviceGL
abstraction. The hypothesis was empirically verified clean at HEAD —
this section LOCKS that state.

**Adding a new diagnostic exception:** Add the file + 1-line
justification to `scripts/check-no-raw-gl-from-game.allowlist`. If the
exception is RENDERING (not just diagnostic), reject it: route through
an engine API instead.
```

### 3.4 CLAUDE.md M6 SHIPPED entry

Append to Active campaigns section after the most recent slice entry:

```
- **RenderWorld Slice M6** (SHIPPED 2026-05-24): firewall audit script —
  no raw GL from game side. Codifies the empirical finding (M6 recon)
  that `code/` has ZERO raw GL calls and `mclib/` has 3 diagnostic-only
  gated hits in `render_contract.cpp`. New
  `scripts/check-no-raw-gl-from-game.sh` (function-level grep, NOT
  include-level) with allowlist of exactly one TU
  (`mclib/render_contract.cpp`; gated by `MC2_RENDER_CONTRACT_ASSERT=1`).
  Migration guide §3.5 documents the rule. CI / pre-commit can wire the
  script into existing hook infrastructure. Tier1 5/5 PASS (no source
  changes). GameOS reviewer-discipline gap (M2.5 MAJOR-1 carry-over)
  deferred to optional M6.5. Turns the arc from "discipline by memory"
  to "discipline enforced by script." Spec:
  `docs/superpowers/specs/2026-05-24-renderworld-slice-m6-firewall-audit-spec.md`.
  Recon:
  `docs/superpowers/explorations/2026-05-23-renderworld-slice-m6-firewall-audit-recon.md`.
```

---

## 4. Validation

- Tier1 5/5 PASS (no source change → trivially passes; smoke just
  confirms no regression from doc updates)
- New audit script: `sh scripts/check-no-raw-gl-from-game.sh` exits 0
- Negative test: manually inject a `glClear(GL_COLOR_BUFFER_BIT);` line
  into a `code/` file, confirm script catches it with the file+line, then
  revert
- Existing `check-include-firewall.sh` continues to pass (no change)
- Migration guide section ordering: §3 followed by §3.5 followed by §4 (no
  orphan numbering)

---

## 5. Threat model

- **Trap 1: false positives from naming collisions.** The recon found 14
  naive hits were false positives (`glX/glY/glTextureId/glTexId` etc.).
  Mitigation: function-call anchor `\s*\(` in the pattern eliminates
  identifier-only matches. Spec mandates testing this on the recon's
  known false-positive set.
- **Trap 2: future GameOS contributor adds raw GL calls to code/ or
  mclib/.** Script catches it on next run. If the addition is legitimate
  diagnostic, allowlist it; if it's rendering, route through engine API.
- **Trap 3: M3/M4/M5 writers (if they ever ship) introduce game-side raw
  GL calls.** They shouldn't — substrate writes happen in engine TUs.
  But if they do, this script catches it AT THE COMMIT, not at runtime.
- **Trap 4: allowlist file format drift.** Keep it line-based, one path
  per line, `#`-comments allowed. Script parses with `grep -v '^#'` or
  similar.

---

## 6. Greybeard analysis

**Ruling: META-FIX.**

The slice converts a documented-but-unenforced contract ("no raw GL from
game side") into a mechanical CI gate. The recon proved the contract
already holds at HEAD; M6 is the lockdown that prevents drift.

Rejected alternatives:
- **Defer until M3/M4/M5 writers land:** advisor explicitly rejects —
  lock the clean state BEFORE any writer slice could drift it.
- **Hand-wave a CLAUDE.md note:** that's the "discipline by memory"
  shape M6 retires. The script is the META-FIX.
- **Use the existing `check-include-firewall.sh`:** orthogonal question
  (include direction vs function-call origin). The recon recommends a
  separate script for clarity.

---

## 7. Open questions for human

None at spec time. All 4 recon Qs resolved by external advisor 2026-05-24:

- Q1 (audit-only NOW vs defer): NOW
- Q2 (GameOS gap close vs accept): defer as optional M6.5
- Q3 (migration guide §3.5 add): YES, ~20 lines (encoded in §3.3 above)
- Q4 (schedule NOW vs LATER): NOW

---

## 8. Why this is the right shape (closing argument)

M6 is the slice that retires the bug class "discipline by reviewer
memory drifts as the team / archaeologist changes." Every other slice in
the arc (M1, M1.5, M1.6, M2-pre, M2, M2.5, M2.6) added code; M6 adds
*enforcement* of the rules those slices implicitly relied on. The
ROI is durability — future M3.1 / M4.1 writer slices (if/when they ship)
inherit the clean state automatically.

The smallest slice of the arc, with the highest leverage per line of code.

---

End of spec. SPEC STATUS: READY FOR EXECUTE.
