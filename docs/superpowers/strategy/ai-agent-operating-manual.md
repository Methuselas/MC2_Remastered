# AI-Agent Operating Manual

**Status:** Process codification (v1, 2026-06-11)
**Scope:** How AI models are dispatched on the MC2 OpenGL modernization project — which model class does what work, how dispatch prompts are built, what gates an agent must pass before claiming "done", and how sessions hand off. Grounded in observed practice; sections marked **(proposal)** generalize a single observation into a rule.

**Companion doc:** `docs/superpowers/strategy/release-train-governance-manual.md` owns branch/merge/baseline/flag-ladder/deploy/smoke governance. This manual does NOT restate it — where an agent rule depends on a governance rule, it cites the section (e.g. "governance §6 deploy split-brain").

Primary sources:
- `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md` ("Model routing", "Smoke gate", "Memory & CLAUDE.md discipline")
- `docs/disciplines.md` (review / meta-fix / advisor / subagent lean-intake disciplines)
- `docs/critical_inline_rules.md`
- `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` (handoff index — incident record)
- `C:/Users/Joe/.claude/CLAUDE.md` (local_llm delegation policy)
- `.claude/agents/DOMAINS.md` (12 MC2 advisor subagents)

---

## 1. North star

The right model for the right altitude, and **no model — at any altitude — gets to claim success without machine-checkable evidence.** Big models produce maps and decisions, mid models produce code slices, small models produce mechanical text — but every one of them is gated by the same physics: tier1 smoke exit codes, oracle counters, deployed-exe mtimes, and grep-verified citations. The project's worst incidents (stale-root edit, deploy split-brain, net-negative "fix") were all cases where an agent's *claim* substituted for a *measurement*. This manual exists to make that substitution structurally impossible.

---

## 2. Model role matrix

| Model | Capabilities used here | Dispatch when | NEVER |
|---|---|---|---|
| **Fable** (this class) | Overnight strategy/architecture docs (the `docs/superpowers/strategy/` corpus); messy cross-system ownership questions (engine-lane separation, data-ownership registry); orchestrating subagent fleets; producing maps, taxonomies, anti-goals, execution prompts | Question spans 3+ subsystems with no single owner; output is a decision document or a fleet of dispatch prompts; work is "figure out what the work IS" | Write production code directly; output is prose + prompts, not diffs. Don't dispatch for anything a spec already answers. |
| **Opus** | Complex implementation slices; debugging campaigns (crash families, split-brain projection bugs); code review of risky changes (SSBO schemas, legacy retirement, cull gates) | Slice touches load-bearing render/lifecycle code; bug is non-deterministic or multi-cause; review stakes per `docs/disciplines.md` review discipline ("architectural endpoints, legacy retirement, SSBO schemas, perf gates >=30%") | Routine lookups or well-specified mid-size edits (waste); per CLAUDE.md "Model routing": "opus: architecture, deep analysis only — give isolated context" |
| **Sonnet** | Mid-size well-specified implementation; recon (callgraph/classification docs like `.claude/terrain-update-*.md`); doc writing from a clear spec | Spec/plan exists and passed review; recon question has a defined deliverable shape; CLAUDE.md routing: "sonnet: impl, debug, review" | Open-ended architecture decisions; risky-slice final review without Opus escalation (§8) |
| **Haiku** | Trivial mechanical edits; log parsing (smoke artifact `ring_trace.log` triage); classification; summaries | Task is one file, one pattern, objectively checkable; CLAUDE.md routing: "haiku: lookups, summaries, simple edits" | Anything where a wrong answer is silently plausible — render code, packet formats, anything without a verification command |
| **Codex (external CLI)** | Well-specified isolated code tasks with crisp acceptance criteria; cross-AI plan review (`gsd-review` / `gsd-plan-review-convergence` skills exist for this) | Task is self-contained (one module, defined I/O, a test that proves it); useful as a second opinion engine on plans **(proposal — grounded in the gsd cross-AI review tooling, not yet a routine code-task channel)** | Anything needing live repo context beyond what the prompt carries; anything touching the smoke/deploy pipeline it cannot run |
| **local_llm** (Qwen3.6-27B, koboldcpp MCP) | Commit/PR titles, one-line summaries, rephrasing, trivial yes/no classification | Low-stakes text chore where a wrong answer is instantly visible and cheap (policy: `C:/Users/Joe/.claude/CLAUDE.md`) | **Never** code generation/edits, multi-step reasoning, correctness/security-critical text, anything needing repo context. If it returns `[local_llm unavailable: ...]`, do the chore yourself; do not retry-loop. |

Specialized in-repo subagents (cut across model classes): the 12 **advisors** in `.claude/agents/DOMAINS.md` (domain Q&A with fresh context — advisor-first discipline, `docs/disciplines.md`), and the **cavecrew** investigator/builder/reviewer trio (compressed-output delegation for locate/small-edit/diff-review).

---

## 3. Dispatch decision tree

```
Is the task text-only, trivial, repo-context-free (title, one-liner, classify)?
├─ YES → local_llm (verify output; fall back silently if unavailable)
└─ NO
   Is it a substantive question in an advisor domain (.claude/agents/DOMAINS.md)?
   ├─ YES → spawn the advisor FIRST (disciplines.md: main-agent knowledge is stale by definition)
   └─ NO
      Is the deliverable a decision/map/strategy spanning systems, or a fleet of prompts?
      ├─ YES → Fable (overnight doc or orchestration session)
      └─ NO
         Does a reviewed spec/plan exist for it?
         ├─ NO → write the spec first (Fable or Sonnet recon); plans pass
         │        adversarial-plan-review before risky implementation (disciplines.md)
         └─ YES
            Is it risky (load-bearing render path, lifecycle, packet format,
            legacy retirement) or a debugging campaign?
            ├─ YES → Opus, isolated context
            └─ NO
               Is it fully isolated with crisp acceptance criteria + its own test?
               ├─ YES → Codex acceptable (proposal); Sonnet default
               └─ NO
                  Mid-size, well-specified → Sonnet
                  One-file mechanical / parse / summarize → Haiku (or cavecrew-builder)
```

Tie-breakers: cost of a wrong answer beats cost of the model. A "trivial" edit in `mclib/move.cpp` is not trivial — packet-format code routes to Sonnet minimum. When in doubt between Sonnet and Opus on implementation, Sonnet implements + Opus reviews (cheaper than Opus implementing).

---

## 4. Prompt anatomy per role

All dispatch prompts obey the **lean-intake rule** (`docs/disciplines.md`): ONE authoritative source file, everything else inlined, no "read these 5 docs for background" lists, and the analysis-paralysis guard verbatim in long-running dispatches: *"5+ consecutive Read/Grep/Glob without Edit/Write/Bash = STOP."* Every prompt contains the **absolute worktree path** (governance §7 — the stale-root incident).

**Fable (strategy/orchestration):**
- *Context grounding:* the question, the worktree CLAUDE.md pointer, the 2-4 source docs that constitute ground truth, instruction to mark inferences as proposals.
- *Constraints:* anti-goals up front (no code, no wall-clock estimates, no duplicating companion docs); citation discipline (every file:line grep-verified at write-time).
- *Deliverable:* named output file + required section list.
- *Verification:* self-check that cited paths exist; 3-line summary back to caller.

**Opus (risky slice / debug campaign):**
- *Context grounding:* the ONE authoritative spec/plan; inline the relevant trap-rulebook entries (governance §10) and load-bearing pointers; isolated context only ("give isolated context", CLAUDE.md).
- *Constraints:* "run the greybeard skill" verbatim (meta-fix ruling required before any fix, disciplines.md); env-gated debug instrumentation lands in the same commit (debug instrumentation rule); flag-ladder rung this slice occupies (governance §5).
- *Deliverable:* commits on the campaign branch in the assigned worktree, oracle counters wired before the fix (§7 oracle-first).
- *Verification:* canonical tier1 invocation copy-pasted verbatim from CLAUDE.md; PIPESTATUS on builds; deployed-exe mtime check; report counters, not adjectives.

**Sonnet (well-specified implementation / recon / docs):**
- Same skeleton as Opus, minus greybeard when the spec already names the meta-fix; recon dispatches additionally state the deliverable's *shape* (table/callgraph/classification with columns named) so output is mechanically consumable by the next dispatch.

**Haiku (mechanical):**
- *Context grounding:* exact file path(s), exact pattern, one example of correct output.
- *Constraints:* touch nothing else; no judgment calls — "if ambiguous, return the ambiguity, do not resolve it."
- *Deliverable + verification:* an objectively checkable artifact (grep count, diff line count, exit code) the orchestrator re-runs itself.

**Codex (proposal):**
- *Context grounding:* fully self-contained — inline every type signature, format, and invariant; no repo assumptions.
- *Constraints:* acceptance test included in the prompt; output is a patch, applied and gated by the orchestrator (Codex never runs the smoke).

**local_llm:**
- One sentence of instruction + the raw input + small `max_tokens`. No context, no chains. Caller verifies output before use.

---

## 5. Branch discipline for agents

(Full topology: governance §2, §7. Agent-facing summary:)

1. Work happens **in the assigned worktree under `.claude/worktrees/`**, never the root checkout. Root `terrain-pbr-mod` and root `build64/` are STALE; root `CLAUDE.md` is a pointer (enforced by `scripts/check-claude-md-pointer.sh`).
2. Every dispatch prompt carries the absolute worktree path; the **orchestrator verifies the returned diff landed under the worktree** before accepting it (incident: subagent edited stale ROOT `mech3d.cpp` — dynamic-pipeline-oracle handoff).
3. Agents never merge into `claude/nifty-mendeleev` or deploy — they *propose*; the user owns merge windows and deploys (governance §3).
4. Build with the worktree's own `build64/`, config `RelWithDebInfo` always.

---

## 6. Verification gates before "done"

An agent may not report success until ALL applicable gates show evidence (not claims):

1. **Build exit code captured correctly** — `PIPESTATUS[0]`, never `cmake --build ... | tail` (trap: tail's exit status masked a failed build).
2. **tier1 5/5 PASS**, canonical invocation verbatim from worktree CLAUDE.md (`--keep-logs`, never `--kill-existing`, never concurrent). Inner-loop 2-mission subset is allowed mid-work but never as the done-gate.
3. **Oracle counters clean and NON-VACUOUS** — FN=0/dropped=0/mismatch=0, AND evidence the oracle exercised data (smoke missions are idle fly-throughs: FX counters legitimately read zero there; a zero from a path that never ran proves nothing).
4. **Deployed-exe freshness** — smoke runs the DEPLOYED exe; verify mtime ≥ fix commit time on every target (governance §6).
5. **The FIX is measured, not just the hotspot** — before/after numbers for the change itself (rect-prefilter incident, §7).
6. **Citations grep-verified** for any doc/recon deliverable (documentation discipline).
7. **User-gated items declared, not claimed** — visual confirms and interactive gameplay sanity are explicitly reported as PENDING; headless agents cannot see pixels.

Reporting format: counters and exit codes inline, artifact dir path, branch + HEAD sha, explicit PENDING list. "It should work now" is a gate failure.

---

## 7. Anti-pattern catalog

Each entry: the incident (cited), then the rule derived.

1. **Stale-root edit.** A subagent edited root `mech3d.cpp` instead of the worktree copy; the "completed" work was invisible to the build (dynamic-pipeline-oracle handoff). → *Rule:* absolute worktree path in every prompt; orchestrator verifies diff location before accepting (§5.2).
2. **Success claimed without running the gate.** Multiple handoffs record fixes "verified" against a stale deployed exe — a prior "fixed in 0.4c" left v0.4 stale and re-burned a full cycle on the already-fixed 0xC crash (2026-06-08b handoff). → *Rule:* no success report without the gate's literal output in the report; mtime check is part of the gate, not optional hygiene (§6.4).
3. **Unmeasured fix shipped, net-negative.** A rect-prefilter "optimization" of `findTerrainObjectByMouse` was REVERTED because the fix itself was never measured — only the hotspot was ("measure the FIX not just the hotspot", pick-path handoff). → *Rule:* every perf change reports before/after of the change in the same conditions the hotspot was measured; a fix without its own measurement is a hypothesis, not a slice.
4. **Pipe masks exit code.** `| tail` returned tail's status; a broken build read as green. → *Rule:* §6.1.
5. **False premise propagated through a plan.** Phase 8 plan asserted "Phase 4 suppresses geometry()" — it did not; geometry() ran unconditionally and was the sole producer of a load-bearing flag (terrain 8 handoff, corrected `a8697786`). → *Rule:* adversarial-plan-review before risky milestones grep-verifies every plan claim against code; "deletion" plans get a dependency-extraction audit first.
6. **Additive offload netting ~0ms.** History of slices that added a new path without retiring the old cost (`memory/feedback_offload_must_be_substitutive_not_additive.md`). → *Rule:* greybeard META-FIX ruling required before any fix; offloads must be substitutive.
7. **Vacuous oracle pass.** Counters read zero because the smoke never exercised the path (idle missions, FX=0). → *Rule:* §6.3 non-vacuousness evidence.
8. **Analysis paralysis in long dispatches.** Executor subagents looping reads without producing. → *Rule:* paralysis guard encoded verbatim in every long-running dispatch (§4).

---

## 8. Escalation rules

- **Haiku → Sonnet/Opus re-check:** any Haiku output that feeds a decision or a commit is re-verified mechanically by the orchestrator (re-run the grep, re-count). If Haiku output is *surprising* (a count that contradicts expectation, a classification with high stakes), escalate the same question to Sonnet with the Haiku answer withheld, and diff. **(proposal — generalizes the cavecrew-reviewer pattern.)**
- **Sonnet → Opus:** mandatory when a Sonnet slice touches anything on the review-discipline high-stakes list (architectural endpoints, legacy retirement, SSBO schemas, perf gates ≥30%) — Opus runs adversarial-plan-review / code review. Also escalate when Sonnet's second consecutive fix attempt fails the same gate: that is a debugging campaign now, not a slice.
- **Any model → advisor:** substantive domain question mid-task → stop and spawn the advisor (disciplines.md; advisors carry `<known_pitfalls>` not in MEMORY.md).
- **Any model → user:** visual confirms, interactive sanity, merge windows, deploys, and anything destructive outside the worktree. Agents never burn a user-gate by assuming it.
- **local_llm → self:** unavailable or wrong output → do it yourself, never retry-loop (user-global policy).
- **De-escalation:** Opus findings that reduce a task to mechanical edits hand back down to Sonnet/Haiku with the finding inlined — don't keep Opus on the keyboard for the cleanup.

---

## 9. Orchestration patterns

1. **Caveman-orchestrated slices.** Observed on the dynamic-pipeline oracle ("Caveman-orchestrated optimization", 2026-06-09 handoff): the orchestrator runs in compressed-token mode, dispatching cavecrew investigator/builder/reviewer subagents whose outputs are caveman-compressed (~60% smaller tool results), keeping a long session's context alive across many slices. Use for multi-slice campaigns where the orchestrator's context is the scarce resource.
2. **Adversarial-plan-review before risky milestones.** Mandated before GPU-cull M1 ("run adversarial-plan-review before coding M1", 2026-06-04 handoff) and encoded in disciplines.md: high-stakes plans get the full skill, dispatched without asking, prompt contains "use the adversarial-plan-review skill" verbatim, every cited symbol grep-verified, findings as CRITICAL/MAJOR/MINOR. The pattern exists because of incident §7.5.
3. **Oracle-first development: build the measurement before the fix.** Observed repeatedly: Step 1 of the dynamic-pipeline campaign was the *oracle* (`docs/oracle-dynamic-pipeline-gate.md`, `MC2_FX_COUNT_LOG`) before any optimization; terrain 8a/8b shipped shadow A/B producers (`MC2_TERRAIN_ACTIVE_AB`, `MC2_TERRAIN_SOLID_AB`) proving FN=0 before 8c rewired production. *Rule:* a campaign's first slice is the counter/parity probe that will later prove the change safe; an agent asked to "optimize X" first asks "what oracle gates X?" and builds it if missing.
4. **Recon → spec → review → implement pipeline.** Recon docs (`.claude/terrain-update-*.md`, `docs/superpowers/recon/`) are produced by Sonnet-class dispatches with fixed deliverable shapes; Fable consolidates into strategy/spec; adversarial review; then implementation dispatches consume ONE authoritative file (lean intake). Background recon is never re-attached to implementation prompts — its findings are baked into the spec.
5. **Fleet fan-out with mechanical join.** When Fable orchestrates parallel subagents, each dispatch has a deliverable the orchestrator can verify without trust (file exists, section list matches, grep passes) — the join step is verification, not synthesis-by-vibes.

---

## 10. Handoff and memory conventions

(Format owned by governance §9; agent-facing duties:)

- **Session end mid-arc** → write `HANDOFF_<date>_<topic>.md` in the memory dir + a one-block MEMORY.md index entry: branch + HEAD sha, SHIPPED (with shas), PENDING/BLOCKED with the blocking gate named, an explicit **"Do NOT yet"** list, and traps hit. Index entries ~200 chars; detail in the topic file (MEMORY.md is size-limited and currently over budget — long entries get truncated for future sessions, which defeats the handoff).
- **Session start** → read MEMORY.md handoff blocks first, then the matching `INDEX-{TOPIC}.md` for the domain (worktree CLAUDE.md topic tree).
- **New finding** → memory topic file + INDEX entry; `grep -i <keyword> memory/*.md` to dedupe first; superseded entries updated or deleted, never accreted.
- **No session narratives in CLAUDE.md**; ledger entries go to `docs/active_campaigns.md`; dated logs to commit messages or memory.
- **Traps are promoted**: any trap that cost a debug cycle gets a rulebook entry (governance §10) or an anti-pattern entry here — handoffs are the intake queue for this manual.

---

## 11. Anti-goals

- **No model self-grading.** No agent's "looks correct" substitutes for a gate output — including Opus.
- **No Fable writing production code**; outputs are docs and dispatch prompts.
- **No local_llm in any correctness path** — not even "just this once" for a trivial code edit.
- **No multi-doc reading lists in dispatch prompts** (lean intake; one authoritative file).
- **No re-litigating governance here** — merge windows, flag ladders, deploy targets live in the governance manual; this doc only routes agents through them.
- **No retry-loops on failing externals** (local_llm down, flaky tool) — diagnose or fall back.
- **No unmeasured perf "fixes"** and **no oracle-less campaigns** (§7.3, §9.3).
- **No model-prestige routing** — Opus on a trivial edit is as wrong as Haiku on a packet format.

---

## 12. First 5 process slices

1. **Dispatch-prompt linter** (`scripts/check-dispatch-prompt.py` **(proposal)**): given a prompt file, assert it contains the absolute worktree path, exactly one "authoritative file" declaration, the paralysis guard verbatim (for long-running), and the required skill incantations (greybeard / adversarial-plan-review) when the slice class demands them. Wire into the orchestrator's pre-dispatch step.
2. **Diff-location verifier** (`scripts/check-diff-in-worktree.sh`): post-dispatch, assert all changed paths are under `.claude/worktrees/<assigned>/`; nonzero exit on root-checkout edits. Retires anti-pattern §7.1 structurally.
3. **Done-report template** (`docs/superpowers/specs/_agent-done-report-template.md`): the §6 gate list as a literal fill-in form (build exit, tier1 result + artifact dir, counters + non-vacuousness evidence, deploy mtimes, PENDING user-gates); orchestrators reject reports that aren't the filled template.
4. **Escalation log** (`memory/escalations.md`): one line per Haiku/Sonnet→Opus escalation with cause; quarterly read decides whether routing defaults in CLAUDE.md "Model routing" need recalibration. **(proposal)**
5. **MEMORY.md size enforcement** (`scripts/check-memory-index-size.py`): fail when MEMORY.md exceeds its load budget or any index entry exceeds ~200 chars; the index is currently over limit and partially unloaded — the handoff convention is silently degrading (§10).

## 13. Three follow-up prompts

1. "In the nifty-mendeleev worktree, implement process slice 2: write `scripts/check-diff-in-worktree.sh` that takes a worktree path and a git ref-range and exits nonzero if any changed file lies outside that worktree; add a usage note to docs/disciplines.md subagent-dispatch section. Cite docs/superpowers/strategy/ai-agent-operating-manual.md §12.2."
2. "MEMORY.md at ~/.claude/projects/A--Games-mc2-opengl-src/memory/ is over its 24.4KB load budget and entries are being truncated. Compress every handoff index entry to one line under 200 chars, moving displaced detail into the linked topic files (create them where missing); verify nothing substantive is lost by diffing facts."
3. "Write `docs/superpowers/specs/_agent-done-report-template.md` per ai-agent-operating-manual.md §12.3: a fill-in done-report covering build exit code (PIPESTATUS), tier1 invocation + artifact dir, oracle counters with non-vacuousness evidence, deploy mtime checks per target, and a PENDING user-gates section; then update the dispatch guidance in docs/disciplines.md to require it."
