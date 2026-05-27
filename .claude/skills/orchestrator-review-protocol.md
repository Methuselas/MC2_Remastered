---
name: orchestrator-review-protocol
description: Three-phase subagent dispatch pattern (implementer → reviewer → committer) that recovers 4-eyes review when the implementer is itself a subagent and cannot dispatch siblings. Invoke when a slice touches shared structs, contract registries, multi-file refactors, closure-audit work, or anything where a stale boolean or mis-named symbol would silently mis-report state downstream. Skip for read-only inspectors, trivial doc edits, or single-file localized fixes where main-thread inline review is cheaper.
---

# Orchestrator Review Protocol (Diff-Ready Halt + Independent Reviewer)

Three-phase subagent dispatch pattern used to recover 4-eyes review when the
implementer is itself a subagent and cannot dispatch sibling agents.

## When to use

USE for slices that touch shared structs, contract registries, multi-file
refactors, closure-audit work, or anything where a stale boolean or a
mis-named symbol would silently mis-report state downstream.

SKIP for read-only inspector additions, trivial doc edits, comment-only
changes, or single-file localized fixes where main-thread inline review is
cheaper than an extra round-trip.

## The pattern

### Phase 1 — Implementer subagent

Dispatched by main thread with a fully self-contained brief. Performs:

1. Recon (grep / read consumers / verify assumptions).
2. Plan (state the decision explicitly in the report — e.g. "rename
   consumers" vs "backward-compat alias").
3. Implement (Edit / Write tool calls — only the files in scope).
4. Build (RelWithDebInfo if any code changed; skip if doc-only).
5. Smoke (tier1 5/5 or minimal subset, per brief).
6. Self-pre-review checklist (typically a/b/c/d/e/f bullets the brief lists).
7. **STOP AT DIFF-READY.** Do NOT commit. Return a terse report including
   `DIRTY_NOW: <git status --short>` and `HEAD_STILL: <pre-slice SHA>` so
   the main thread can verify nothing was committed prematurely.

The implementer's report is the handoff. It must include the diff summary
(one line per file) so the reviewer can be dispatched without re-reading
the full diff first.

### Phase 2 — Reviewer subagent

Dispatched by main thread *after* Phase 1 returns. Given:

- The list of files touched (absolute paths).
- The original Phase 1 brief (so the reviewer knows the intended scope).
- An explicit review checklist matching the slice's risk surface (e.g.
  "verify no closure-axis bool flipped", "verify no shader changes", "verify
  enum-value stability for serialized data").
- Explicit instruction: do NOT edit, do NOT commit. Read-only diff review.

Returns one of: **PASS**, **LOW_ONLY** (nits, safe to ship), **HIGH**
(blocker — back to implementer or main thread for rework).

### Phase 3 — Committer subagent (or main thread)

If verdict is PASS or LOW_ONLY: stage the files explicitly (NEVER
`git add -A`), commit with a message that cites the implementer + reviewer
phase SHAs or subagent IDs in the trailer, and return the new HEAD SHA.

If verdict is HIGH: back to Phase 1 with the reviewer's findings as
additional context.

## Why it exists

Subagents currently cannot dispatch sibling Agent / Task calls. An
implementer subagent that self-reviews is structurally forced to mark its
own homework. The 3-phase split recovers independent review at the cost of
one extra dispatch and one extra main-thread round-trip per slice.

The diff-ready halt is the load-bearing primitive: as long as the
implementer reliably stops without committing, the main thread retains the
choice of who reviews and when to commit.

## Cost

Per slice (vs. single-subagent implement-and-commit):

- +1 subagent dispatch (the reviewer).
- +1 main-thread round-trip (dispatch reviewer, dispatch committer).
- ~0 implementation time (the implementer's work is unchanged; it just
  stops one step earlier).

Acceptable for medium/high-risk slices. Overkill for trivial slices.

## Examples (batch 3, branch tip `b7987b70`)

All three slices in batch 3 used this protocol cleanly:

- **TERRAIN-PASS-PACKET-0** — passive TerrainPassFacts row in
  RenderSnapshot. Reviewer verdict PASS clean. Final SHA `1d7b9ea6`.
- **RENDERPASS-CONTRACT-2.5** — pass-contract registry expansion.
  Reviewer verdict PASS clean. Final SHA `828432b6`.
- **ENGINE-CLOSURE-AUDIT** — `docs/engine-closure-audit.md` write-up.
  Reviewer verdict PASS, 8 citation spot-checks all verified against
  source. Final SHA `b7987b70`.

Citing commit SHAs (not subagent IDs) is the durable evidence trail —
subagent IDs are session-local and drop out of context, while SHAs persist
in `git log`.

## Anti-patterns

- **Implementer self-reviews and skips the reviewer dispatch.** Was
  structurally forced in earlier sessions; surfaced as risk in batch 1 and
  batch 2 closure reports. The diff-ready halt exists specifically to
  prevent this.
- **Reviewer not given the actual diff content or file list.** Must pass
  absolute file paths and an explicit review checklist. A reviewer told
  "review the recent change" without scope will either over-read (waste
  context) or under-read (miss the slice).
- **Bundling multiple slices into one diff-ready halt.** Review one slice
  at a time so a HIGH verdict on slice 2 doesn't trash slice 1's clean
  implementer work. Batch commit is fine *after* each slice independently
  passes its reviewer.
- **Reviewer subagent given write permissions.** The reviewer must be
  read-only. If it edits, the protocol collapses back to single-agent
  self-review.

## Cross-references

- `docs/engine-closure-audit.md` (SHA `b7987b70`) — first audit document
  produced under this protocol, with reviewer spot-check trail in the
  commit message.
- `docs/superpowers/cpu-to-gpu-offload-orchestrator.md` — the broader
  orchestrator context this protocol slots into.
