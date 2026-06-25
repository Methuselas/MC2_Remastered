#!/usr/bin/env python3
"""check-editor-pass-contract-parity.py - EDITOR-PASS-CONTRACT-PARITY-1 gate

Applies the project render-governance pattern to the EDITOR: for every governed
render pass (RenderPassId in RenderCore/RenderPassContract.h), the editor must
either render that pass through the SAME shared contract as the game, or declare
a scoped editor-override with rationale. Undeclared drift FAILs.

Why: the editor renders the world through the SAME shared path as the game
(editor/Editor.cpp UpdateRenderers -> editor->render(), bracketed by
editor/EditorGameOS.cpp pp_editor->beginScene()/endScene() with the object-id
MRT). If a future render feature adds, renumbers, or re-pipelines a governed
pass and the editor silently diverges (different PassId / pipeline / attachment),
the editor's authoring view drifts from the shipped game. This checker makes
that class of drift non-recurring by pinning a declared parity table to the
governed-pass enum and FAILing when they disagree.

Source of truth (the declared table this checker validates):
    docs/render-backend-seams/editor-pass-contract-parity.json
Governed-pass authority (cross-checked against):
    RenderCore/RenderPassContract.h  (enum RenderPassId)

FAILS when:
  1. The parity JSON is missing / unparseable.
  2. A governed RenderPassId (enum value, != None) has NO entry in the parity
     table  -> undeclared drift: a new governed pass appeared and nobody decided
     how the editor relates to it.
  3. A parity entry references a governed passId that does NOT exist in the enum
     -> stale entry (pass renamed/removed but table not updated).
  4. Any entry has an unknown `classification` (not shared/editor-only/game-only/
     override).
  5. An `override` or `game-only` or `editor-only` entry has empty `rationale`.
  6. A `shared` entry claims editor='absent' or game='absent'  -> contradiction
     (shared means BOTH render it through the same contract).
  7. An `editor-only` entry claims game='rendered', or a `game-only` entry claims
     editor='rendered'  -> classification contradicts declared status.

Editor-only synthetic passes (gizmos, selection overlay, brush preview,
object-id readback viz) use a `_editor_only:` passId prefix so they are NOT
expected to exist in the governed enum (check 3 skips that prefix).

Exit code: 0 on PASS, 1 on any FAIL. Grep/AST-lite, Windows py3 runnable.

Usage:
  py -3 scripts/check-editor-pass-contract-parity.py [--root <repo>] [--json <out>] [--quiet]
"""
import argparse
import json
import os
import re
import sys

PARITY_JSON = os.path.join(
    "docs", "render-backend-seams", "editor-pass-contract-parity.json")
PASS_H = os.path.join("RenderCore", "RenderPassContract.h")

VALID_CLASS = {"shared", "editor-only", "game-only", "override"}
EDITOR_ONLY_PREFIX = "_editor_only:"


def read(root, rel):
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_governed_passes(text):
    """Return the set of real RenderPassId names (excluding None and the
    _SentinelLast bookkeeping value) from the `enum class RenderPassId` block."""
    m = re.search(r"enum\s+class\s+RenderPassId\s*:\s*\w+\s*\{(.*?)\}", text, re.S)
    if not m:
        return None
    body = m.group(1)
    names = []
    for line in body.splitlines():
        code = line.split("//", 1)[0]
        em = re.match(r"\s*([A-Za-z_]\w*)\s*[,=]", code)
        if em:
            names.append(em.group(1))
    real = [n for n in names if n not in ("None", "_SentinelLast")]
    return real


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--json", default=None, help="optional path to write a result report")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    failures = []
    warnings = []

    def say(msg):
        if not args.quiet:
            print(msg)

    # ---- Load governed enum
    pass_h = read(root, PASS_H)
    if pass_h is None:
        print("FAIL: governed-pass header %s is missing" % PASS_H)
        return 1
    governed = parse_governed_passes(pass_h)
    if governed is None:
        print("FAIL: could not parse enum RenderPassId in %s" % PASS_H)
        return 1
    governed_set = set(governed)

    # ---- Load parity table
    raw = read(root, PARITY_JSON)
    if raw is None:
        print("FAIL: parity table %s is missing" % PARITY_JSON)
        return 1
    try:
        data = json.loads(raw)
    except Exception as e:  # noqa: BLE001
        print("FAIL: parity table %s is not valid JSON: %s" % (PARITY_JSON, e))
        return 1

    entries = data.get("passes", [])
    by_id = {}
    for e in entries:
        pid = e.get("passId", "")
        by_id[pid] = e

    # ---- Check 4/5/6/7: per-entry internal consistency
    for e in entries:
        pid = e.get("passId", "<missing>")
        cls = e.get("classification", "")
        game = e.get("game", "")
        editor = e.get("editor", "")
        rationale = (e.get("rationale") or "").strip()

        if cls not in VALID_CLASS:
            failures.append("%s: unknown classification %r (allowed: %s)"
                            % (pid, cls, ", ".join(sorted(VALID_CLASS))))
            continue

        if cls in ("override", "game-only", "editor-only") and not rationale:
            failures.append("%s: classification %r requires a non-empty rationale"
                            % (pid, cls))

        if cls == "shared" and (editor == "absent" or game == "absent"):
            failures.append("%s: classification 'shared' but game=%r editor=%r "
                            "(shared requires BOTH to render the pass)"
                            % (pid, game, editor))
        if cls == "editor-only" and game == "rendered":
            failures.append("%s: classification 'editor-only' but game='rendered'"
                            % pid)
        if cls == "game-only" and editor == "rendered":
            failures.append("%s: classification 'game-only' but editor='rendered'"
                            % pid)

        # Check 3: a non-synthetic passId must exist in the governed enum.
        if not pid.startswith(EDITOR_ONLY_PREFIX) and pid not in governed_set:
            failures.append("%s: parity entry references a passId not in enum "
                            "RenderPassId (stale entry?)" % pid)

    # ---- Check 2: every governed pass has a parity entry (no undeclared drift)
    for g in governed:
        if g not in by_id:
            failures.append("governed RenderPassId::%s has NO entry in %s "
                            "-> undeclared editor/game pass drift" % (g, PARITY_JSON))

    result = {
        "check": "EDITOR-PASS-CONTRACT-PARITY-1",
        "governed_pass_count": len(governed),
        "parity_entry_count": len(entries),
        "editor_only_count": sum(
            1 for e in entries if e.get("classification") == "editor-only"),
        "override_count": sum(
            1 for e in entries if e.get("classification") == "override"),
        "failures": failures,
        "warnings": warnings,
        "status": "FAIL" if failures else "PASS",
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)

    if failures:
        print("FAIL: editor/game pass-contract parity")
        for f in failures:
            print("  - " + f)
        return 1

    say("PASS: editor/game pass-contract parity "
        "(%d governed passes all declared; %d editor-only, %d override)"
        % (len(governed), result["editor_only_count"], result["override_count"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
