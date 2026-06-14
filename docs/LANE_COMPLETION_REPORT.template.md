---
# LANE COMPLETION REPORT
# Fill every field. Parser: scripts/parse-done-reports.py
# Required fields: branch, commit, base_commit, files_changed, risk_class,
#                  build_run, smoke_run, deploy_state, merge_recommendation
# Copy this file to LANE_COMPLETION_REPORT.md at your worktree root.

branch: claude/<slug>-1
commit: <full sha>
base_commit: <full sha of the nifty commit this forked from>
files_changed: 0
risk_class: low          # low | medium | high
build_run: PASS          # PASS | FAIL | skipped
smoke_run: PASS          # PASS | FAIL | skipped
deploy_state: deployed   # deployed | not-deployed | N/A
merge_recommendation: READY   # READY | HOLD | NEEDS-REVIEW
---

## What landed

<!-- One paragraph. What the lane did. No bullet soup — a future session
     should be able to read this cold and understand the change. -->

## Files changed

<!-- Key files only, one line each, with a note on what changed.
     The parser uses `files_changed` for the count; this section is prose. -->

## Gates run

<!-- List every gate command + result verbatim, e.g.:
     - tier1 5/5 PASS (2026-06-13 sha abc1234)
     - oracle_report --strict exit 0
     - py -3 scripts/deploy_payload.py --target game --verify-only exit 0
-->

## Restore notes

<!-- How to undo this if it needs to be reverted. Name the kill-switch env var
     if one exists, or the specific commits to revert. If "just revert the merge
     commit", say so. If state was mutated outside git (deploy dirs, manifests),
     list what needs manual cleanup. -->

## Known follow-ups

<!-- Deferred items that this lane deliberately left open. Each item should
     have enough context for a fresh session to pick it up. -->

## Merge recommendation

<!-- Expand on the frontmatter merge_recommendation field.
     READY     — tier1 clean, oracle clean, no open blockers, safe to merge.
     HOLD      — passing but waiting on something (state it explicitly).
     NEEDS-REVIEW — specific concern that must be resolved before merge. -->
