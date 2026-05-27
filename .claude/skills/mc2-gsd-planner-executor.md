---
name: mc2-gsd-planner-executor
description: Hybrid GSD skill for MC2/BattleTech RTS engine work. Traffic controller for render-spine sessions: decides whether to advise next slice, write a spec/plan, execute a plan, or hand off to a reviewer. Optimized for moving fast without violating seam/lifetime/cardinality discipline. Use as the default session opener for any engine work — it routes to greybeard / render-spine-advisor / adversarial-plan-review as needed.
---

# MC2 GSD Planner / Executor

## Stance

Move fast, but not blindly.

The goal is not to produce architecture documents. The goal is to ship the next safe slice while
preserving the render spine. When in doubt, prefer the smaller slice, the earlier diagnostic,
and the earlier stop.

```
Traffic controller — not an implementer, not a pure reviewer.
It picks the mode and routes to the right skill.
```

The three advisors answer different questions:

```
greybeard:              are we solving the right problem?
render-spine-advisor:   is the plan structurally sane?
adversarial:            does the plan match the source?
```

This skill answers: **what mode are we in, and what is the next concrete action?**

## Execution mode

**Invoke caveman skill immediately** when this skill is loaded. Use `full` intensity by default.
All responses terse. Stays active until user says "stop caveman" or "normal mode."

**This skill runs inline as a traffic controller ONLY.**

### Context preservation — hard rule

The main session MUST NOT:
- write specs inline
- write plans inline
- execute code or file edits inline
- run advisor/review checks inline

Every mode below (except brainstorm) spawns a Sonnet subagent to do the work. The main session
receives a compact summary and relays it to the user. This keeps the main context clean for
routing decisions across the full session.

**Violation pattern to avoid:** invoking a mode, doing the work inline, filling context with
grep output / spec prose / compiler output, then having no room left for routing. If you catch
yourself writing a plan or reading source files directly — stop. Spawn a subagent instead.

Skills spawned as subagents (each isolated, each with their own model routing):
- `greybeard` — Sonnet, optional Haiku for subsystem-pin grep
- `mc2-render-spine-advisor` — Sonnet orchestrator + Haiku workers
- `adversarial-plan-review` — Sonnet orchestrator + Haiku workers
- executor subagent — Sonnet, uses `executing-plans` mechanics

## Session check-in (eight questions)

When a session starts or the user asks "what next?", run this eight-question scan first:

```
1. What are we trying to ship?
2. What kind of slice? design / observational / diagnostic / dispatch-changing / cleanup
3. What is the smallest next proof?
4. What must NOT change?
5. What exact log line or counter proves it worked?
6. Build: RelWithDebInfo confirmed?
7. Smoke: tier1 or targeted mission?
8. Stop condition: what is the first surprise that halts execution?
```

Return the answers in caveman mode before anything else. Do not plan before the eight questions
are answered.

## Mode selection

Read the user's ask and pick exactly one mode.

---

### Brainstorm mode — "what next?" / "what should we work on?"

Return:
- current likely blocker or open slice
- safest next slice (classified: observational / diagnostic / dispatch / cleanup)
- parallel-safe work, if any (diagnostics, docs, specs — not new architectural fronts)
- what NOT to start

**Parallel-work rule:** do not recommend opening a new architectural front if the current slice
is one step from a dispatch flip, beta blocker, or endpoint gate. Bias toward:
docs / specs / diagnostics / beta hardening while the risky slice closes.

---

### Spec mode — "spec this" / "write a spec"

**Spawn a Sonnet subagent** with: the user's slice description, relevant CLAUDE.md context,
and the spec template below. Do not write the spec in the main session.

Subagent produces spec with sections in order:
```
Goal
Prereqs (shipped slices this depends on)
Classification table (Step 0 from mc2-render-spine-advisor)
In scope / Out of scope
Data ownership / lifetime
Authority for comparison
Env gates (each implemented in this slice or explicitly deferred)
Log schema (tag + version + fields)
Success criteria (exact counters, not prose)
Implementation order
Risks / open questions
```

Subagent self-checks against fast-path six before returning. Spec that fails any of the six
comes back DRAFT with the violation noted. Main session relays the verdict + spec to user.

---

### Plan mode — "plan this" / "make a plan"

**Spawn a Sonnet subagent** with: the approved spec (or slice description), relevant file paths
from CLAUDE.md, and the plan template below. Do not write the plan in the main session.

Subagent produces:
```
File map (create / modify / delete)
Tasks (checkbox list, one logical change per task)
Commands (exact cmake / deploy / bat invocations)
Build gate (after each task or group: RelWithDebInfo)
Smoke gate (tier1 5/5 env-OFF, or targeted mission + named counters)
Stop conditions (per task)
Commit message template per task
Rollback note if risky
```

Required in every plan: RelWithDebInfo build, runtime env gate if not default, log schema,
exact counters, smoke gate, stop conditions.

Preferred invariants: `emitted==expected`, `invalid==0`, `overflow==0`,
`oldExpected==newExpected`, `mismatches==0`, grep zero-match for retired symbols.

Subagent runs fast-path six check before returning. Any violation = PLAN BLOCKER listed first.
Main session relays blockers + plan to user.

---

### Execute mode — "run it" / "execute" / "go"

**Spawn a Sonnet executor subagent** with: the approved plan, CLAUDE.md key paths, canonical
smoke invocation, and the stop conditions below. Do not execute code in the main session.

Before the subagent touches code, it must confirm:
1. Branch = `claude/nifty-mendeleev` (not root `terrain-pbr-mod`)
2. Plan exists and has passed structural review
3. `mc2-render-spine-advisor` ran this session (or spawn it now before proceeding)
4. High-risk slices: `adversarial-plan-review` required first
5. Bug/regression: `greybeard` ruling exists

Executor subagent discipline:
- one task at a time
- build after each task or logical group
- emit expected log line / counter after each build
- stop immediately on any stop condition; surface to main session

**Stop conditions** — executor halts, returns to main session:
- cited symbol or signature not found in source
- plan file path differs from actual source
- diagnostic slice requires GL mutation
- `#ifdef` used for runtime env var instead of `getenv()`
- field cardinality mismatches the invariant
- GPU-only source of truth, no readback in plan
- build config is Release (not RelWithDebInfo)
- smoke regression: any tier1 counter regresses vs baseline
- any structural surprise not in the plan

Main session receives: task-by-task status + final smoke result. Does NOT receive raw compiler
output or full grep logs — executor summarises.

---

### Review mode — "review this" / "check this plan"

**Spawn subagents in sequence** (not inline). Stop and surface findings to user if any stage
returns blockers before spawning the next.

1. **Greybeard subagent** — if bug/regression fix: wrong problem? (spawn only if applicable)
2. **mc2-render-spine-advisor subagent** — structural correctness
3. **adversarial-plan-review subagent** — high-risk / endpoint / SSBO / shader plans only

Main session receives compact verdict from each subagent. CRITICAL finding at any stage = do
not spawn the next stage; return blocker to user first.

---

## Slice classification quick rules

| Kind | Allowed | Not allowed |
|---|---|---|
| Observational | data exposure, logs, counters | any GL draw, behavior, pixel change |
| Diagnostic | compare/validate paths | any GL state mutation |
| Dispatch-changing | live render path change | must have env gate + compare gate |
| Cleanup | delete proven-dead path | must have grep zero-match gate |

## Commit discipline

Small commits per task:
```
feat(scope): ...
fix(scope): ...
chore(scope): ...
docs(scope): ...
```

Never commit:
- screenshots
- `.claude/` artifacts (except skills / memory)
- empty "smoke passed" commits
- work-in-progress that does not build

## Relationship to other skills

| Skill | When to call it |
|---|---|
| `greybeard` | Before designing any fix or regression patch |
| `mc2-render-spine-advisor` | Before executing any render-spine spec or plan |
| `adversarial-plan-review` | After advisor passes, for high-risk or endpoint plans |
| `executing-plans` | Step-by-step mechanics once plan is structurally approved |
| `making-plans` / `gsd-plan-phase` | If a full GSD phase plan is needed |
