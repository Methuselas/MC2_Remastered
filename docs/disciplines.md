# Disciplines (load-bearing)

Extracted from CLAUDE.md 2026-05-24. These are the procedural disciplines that
govern HOW work is done — when to spawn subagents, when to ask for adversarial
review, when META-FIX is required, when to grep-verify.

## Review discipline

When user asks for "review" / "second opinion": **adversarial, code-grounded by
default.** Read `.claude/skills/adversarial-plan-review.md`. High-stakes plans
(architectural endpoints, legacy retirement, SSBO schemas, perf gates >=30%)
get the full skill (grep every cited symbol; findings as CRITICAL/MAJOR/MINOR).
Dispatch prompt MUST include "use the adversarial-plan-review skill" verbatim.
Always dispatch without asking: `memory/feedback_always_dispatch_adversarial_review.md`.

## Meta-fix discipline

Before proposing/writing any fix: run the `greybeard` skill
(`.claude/skills/greybeard.md`). It forces an explicit `META-FIX` vs `PATCH
(justified)` ruling — the upstream change that retires the bug *class*, not
the local symptom patch. A patch with no named meta-fix and no debt
justification is not allowed. Documented history of additive slices netting
~0ms (`memory/feedback_offload_must_be_substitutive_not_additive.md`).
Dispatch prompts MUST include "run the greybeard skill" verbatim.

## Documentation discipline

Every cited symbol grep-verified AT WRITE-TIME. Applies at every stage.
Carve-out: intentions ("we will create X") need no grep.
Full: `memory/brainstorm_code_grounding_lesson.md`.

## Advisor invocation discipline

For any substantive question whose domain has a dedicated advisor per
`.claude/agents/DOMAINS.md`, **spawn the advisor first.** The advisor reads
MEMORY.md + topic files + current code with fresh context; main-agent compiled
knowledge is stale by definition.

**Applies to:** pipeline questions (rendering, shaders, mech, terrain-indirect,
GameOS), methodology (CPU→GPU offload, render-contract refresh, perf-slice
sizing, cull-cascade safety), asset-format (mission data, FST/.fit/.tga/.wav,
file IO, init order), build-system (CMake, vcpkg, FFmpeg delay-load,
full-relink).

**Does NOT apply to:** trivial lookups, follow-up clarification on something
this-session, or when user says "answer directly".

Why: advisors carry tacit knowledge in `<known_pitfalls>` that doesn't live in
MEMORY.md, and grep-verify file:line per their Rule 0. Answering from
main-agent context bypasses both protections.

## Debug instrumentation rule (for reworks)

Any rework touching object lifecycle, cull/visibility gates, render path,
resource lifetime, or cross-system control flow lands env-gated `[SUBSYSTEM]`
lifecycle prints in the same commit. Stays gated off; demote-don't-delete
after fix. Full rule: `memory/debug_instrumentation_rule.md`.

## Smoke sessions are USER-DRIVEN

**The user can see and control every smoke session.** `run_smoke.py` launches
mc2.exe in a real game window the user is watching live. Smoke feedback like
"I saw the triangle" is **first-hand visual observation.**

Anti-patterns: DO NOT ask user to "re-run manually to reproduce" or "confirm
with X env var"; they're already the visual observer. When user says "still
doing it" they mean the most recent smoke — find the latest artifact dir and
analyze its `ring_trace.log`.

After every smoke the user reports a bug in: read
`tests/smoke/artifacts/<latest>/{mission}.ring_trace.log` for that run's probe
events.

## Subagent dispatch discipline (lean intake)

Plan-writers and implementer subagents must have **lean intake** prompts —
one authoritative source file, inline everything else, no "read this as a
reference" lists. See `memory/dispatch_prompt_intake_budget.md` for the
pattern + the gsd-executor analysis-paralysis guard that should be encoded
in every long-running dispatch.

Key prompt-construction rules:
- ONE authoritative file per dispatch (typically the spec or plan)
- Inline all decisions / format / context from sidecars
- Eliminate background docs (recon, prior reviews) — their findings are
  already baked into the authoritative source
- Encode `analysis-paralysis-guard` verbatim in long-running dispatches:
  "5+ consecutive Read/Grep/Glob without Edit/Write/Bash = STOP"
