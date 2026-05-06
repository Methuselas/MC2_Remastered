# Execution prompt — Track B: Widen Static-Prop Registry

**Paste this into a fresh session to execute.** Self-contained briefing.

---

You are executing **Track B** of the MC3 rendering modernization arc. Promote `GpuStaticPropRegistry` from "fast-path replay of cull-approved instances" to "single source of truth for ALL world-static-prop geometry in the mission." ~3 weeks code + ≥1 week soak. Three hard constraints from advisor pass.

## Worktree

`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. All paths below are relative.

## Required reading (in order)

1. `CLAUDE.md` — worktree rules. ⚠️ "Load-Bearing Cull Infrastructure" section is mandatory.
2. `docs/superpowers/plans/2026-05-06-track-b-widen-registry.md` — **this is your plan.** 10 tasks. Hard constraints HC-1/HC-2/HC-3 in the "Hard constraints before execution" section are non-negotiable.
3. `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` — Q4 (mission-load + register-on-spawn API), Q5 (Generic descoped), Q6 (first-frame race structural fix by default), **Q15 (shared-soak discipline)**, **Q16 (firstColorOffset ownership — Task 1 spike must commit before Task 2)**.
4. `docs/superpowers/specs/2026-05-06-static-prop-alpha-test-self-awareness.md` — **MUST READ.** Parallel prep slice. Track B's `registerType()` calls inherit the post-prep 2-arg signature `(TG_TypeShape*, TG_TypeMultiShape*)` — already in HEAD per commits `5327c4b` + `9e45718`. Plan's coordination section documents handoff.
5. `docs/superpowers/explorations/2026-05-06-track-b-widen-registry-recon.md` — recon backing.
6. `memory/cull_gates_are_load_bearing.md` — cascade hazard. Track B widens the population but MUST NOT widen any cull bypass.
7. `memory/black_tree_bug_investigation_state.md` — the `cachedFrame_` pattern Q6 structural fix piggybacks on.

## Skill to invoke

**`superpowers:subagent-driven-development`** with adversarial review at architectural inflection points (Task 1 spike outcome especially).

## Critical prerequisite — DO NOT START UNTIL TWO GATES CLEAR

1. **Alpha-test prep slice has shipped + soaked default-on.** Per the spec at `docs/superpowers/specs/2026-05-06-static-prop-alpha-test-self-awareness.md`. The prep slice's `event=warn` instrumentation becomes Track B's CI/soak signal.
2. **Track A1 has shipped default-on** (per A1 plan Task 9). Q4's register-on-spawn API depends on the substrate validated by A1 in production.

If either gate is not clear: **stop, hand back to user.**

## Three hard constraints (advisor pass)

These block Track B from proceeding unless honored. From the plan's "Hard constraints before execution" section:

- **HC-1: No `void*` cast-compat for Bldg/Tree registration.** Use typed setter callbacks OR a shared `IStaticRegistrable` interface. Pick at Task 2 plan-time.
- **HC-2: `firstColorOffset` ownership decided at Task 1 spike, not deferred.** Three valid answers (bake-at-register / patch-per-frame / recipe-field redesign); spike commits to one with written rationale before Task 2 begins. "We'll patch it if it breaks" is NOT acceptable resolution.
- **HC-3: First-render fallback retirement gated on invariant proof.** Task 9.6 retires the lazy fallback only after `[STATIC_PROP_REG v1] event=type_unknown_at_late_spawn` reads zero across stress soak (specifically including a "spawn one of every late-type during a single mission" test).

## Scope summary — 3-week arc

- **Week 1 (Tasks 1-4):** factoring spike (HC-2 decided), `buildRecipeFromShape` extraction, structural `cachedFrame_` fix, `[STATIC_FIRST_FRAME v1]` counter. Week 1's risk = the factoring spike itself (recon noted `submitMultiShape` reads `listOfVertices`/`listOfColors` which are NULL pre-update — three branching outcomes A/B/C documented in plan Step 1.1).
- **Week 2 (Tasks 5-7):** mission-load walk at `objmgr.cpp:1132 addObject`, `registerStaticProp()` API + late-spawn site wiring (artillery `code/artlry.cpp:1577,1645,1713`, vTOL, mech-bay refits), TGL pool sizing audit per tier1 mission.
- **Week 3 (Tasks 8-10):** parity verification (`submit_legacy=0`, `[DESTROY v1]` count + identity per advisor sharpening #5, `[STATIC_FIRST_FRAME v1]=0`, `[REG_TYPE v1] event=warn` count from prep slice), ≥1 week soak, memory + index.

## Gates

- **Build per task:** clean.
- **Visual canary per task touching the registry:** 15s mc2_01.
- **Cascade-safety hard gate at Task 8:** `[DESTROY v1]` count + identity parity vs baseline. If counts diverge OR identity diverges (even at matching counts), Track B fails. Investigate; do NOT proceed to flip.
- **Pool sizing gate (Task 7):** `[TGL_POOL v1]` peak per tier1 mission must stay below 80% of current 500K. If any mission exceeds, bump pool or descope.
- **Soak ≥1 week** under joint A1+A2+B-modern + alpha-test-prep-modern config (Q15).

## Coordination

- **Track A2 may be in soak or shipped** by the time you start. Either is fine; Track B's soak inherits whatever's-current as default.
- **Track C0 may already be running** as an independent slice. Track B and C0 don't overlap files significantly; check `git status` if unsure.
- **Track C1 is GATED on Track B substrate ready + Q16 closed + Q17 chosen.** Don't block on C — but be aware C1 starts after B ships.

## When blocked

- **Alpha-test prep hasn't shipped:** stop. Wait. Don't start Track B in parallel — the prep slice's `registerType` signature change must be in HEAD before Track B's mission-load walk can use the API correctly.
- **Task 1 spike returns Outcome C** (interstitial-slice fallback): scope reduces to register-on-spawn API + structural `cachedFrame_` fix only. Mission-load enumeration deferred to a follow-up. Document and ship the smaller slice.
- **HC-1 conflict** — Bldg and Tree `StaticRegistration` structs DON'T have matching field order: scope the typed-setter approach (HC-1 option (a)) and proceed.
- **HC-2 spike inconclusive:** escalate to user. Don't pick by coin flip; the consequence is a correctness bug under wider Bldg population.
- **Late-spawn type registration `event=type_unknown` non-zero:** keep the first-render fallback indefinitely (HC-3). Document and ship Track B without retiring the fallback. Acceptable end-state.
- **Registry pool peaks above 80% on any tier1 mission:** bump pool to next tier (1M, 2M, 5M) or descope Generic-class registration further.

## Soak window

Tasks 8-9 require ≥1 week of joint default-on soak before Task 10 (memory + retire fallback per HC-3 invariant proof).

## Deliverable

10 commits + memory + acceptance docs. After soak, Track B is shipped. **Track C1 unblocks** at this point (provided Q16/Q17/Q18 are also closed).

Track A2 and Track C0 are independent of B and may already have shipped.
