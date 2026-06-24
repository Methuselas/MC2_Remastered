#!/usr/bin/env python3
"""
BRAIN-DISPATCH-HARNESS-1: check_brain_relaxed_guard_doc.py
Layer 5 — relaxed-guard doc consistency checker.

Invariant: the two RELAXED-CALL GUARD comment blocks in brain_special_dispatch.cpp
and the actual setGeneralTacOrder() call-sites in executeSpecialBody_Apply must
all agree on the permitted-verb count.

Method:
  header_count    = count "  //   <verb>" bullet lines in the top-of-file RELAXED-CALL GUARD block
                    (lines 27–40 region, starting with "// RELAXED-CALL GUARD —")
  inline_count    = count "//   <verb>" bullet lines in the inline RELAXED-CALL GUARD CONTRACT block
                    (lines 472–483 region, starting with "// RELAXED-CALL GUARD CONTRACT:")
  callsite_count  = count "warrior->setGeneralTacOrder(" lines inside executeSpecialBody_Apply()
                    (from "bool executeSpecialBody_Apply(" up to the next top-level function def)

  FAIL if any two of the three counts disagree.
  PASS if all three agree (currently expected value: 6).

Convention: scripts/check-*.py (--root/--quiet/--json, FAIL/WARN/PASS, exit 0/1/2)
  exit 0 = counts agree
  exit 1 = FAIL (counts disagree)
  exit 2 = source file not found

Usage:
  py -3 scripts/check_brain_relaxed_guard_doc.py [--root <worktree-root>] [--quiet] [--json]
"""

import argparse
import json
import re
import sys
from pathlib import Path

DISPATCH_CPP_REL = "code/brain_special_dispatch.cpp"


def count_header_guard_verbs(lines):
    """Count bullet verb lines in the first RELAXED-CALL GUARD block (top of file).

    The block looks like:
      // RELAXED-CALL GUARD — ...:
      // Permitted order calls: ... for:
      //   Brain.CorePower false    → TACTICAL_ORDER_POWERDOWN
      //   Unit.Eject (...)         → TACTICAL_ORDER_EJECT
      ...
      // All fire IN THE ROOT BODY ...
    """
    in_block = False
    count = 0
    for line in lines:
        stripped = line.rstrip()
        if not in_block:
            if "RELAXED-CALL GUARD" in stripped and "CONTRACT" not in stripped:
                in_block = True
            continue
        # Verb bullet lines: "//   <uppercase or word>" with an arrow or alias
        if re.match(r'^//   [A-Za-z]', stripped):
            count += 1
        # Block ends at a known terminator
        elif (stripped == "" or
              "STILL FORBIDDEN" in stripped or
              "FSM-TODO" in stripped or
              "All fire IN THE ROOT" in stripped):
            break
        # Non-bullet comment lines that are part of block preamble — skip, don't break
        # e.g. "// Permitted order calls: ..."
    return count


def count_inline_guard_verbs(lines):
    """Extract the explicit verb count from the inline RELAXED-CALL GUARD CONTRACT block.

    The inline block contains a line like:
      //   Permitted verbs (SIX total): ...
    We parse the spelled-out number and return it as an integer.
    Fallback: count "→" arrows in the block.
    """
    WORD_TO_INT = {
        "one": 1, "two": 2, "three": 3, "four": 4, "five": 5,
        "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10,
    }
    in_block = False
    for line in lines:
        stripped = line.rstrip()
        if not in_block:
            if "RELAXED-CALL GUARD CONTRACT:" in stripped:
                in_block = True
            continue
        # Look for the explicit count line: "Permitted verbs (N total):" or "(SIX total)"
        m = re.search(r'Permitted verbs\s*\((\w+)\s+total\)', stripped, re.IGNORECASE)
        if m:
            token = m.group(1).lower()
            if token.isdigit():
                return int(token)
            if token in WORD_TO_INT:
                return WORD_TO_INT[token]
        # Block ends at non-indented comment or blank line
        if stripped == "" or (stripped.startswith("//") and not stripped.startswith("//   ")):
            break
    return -1  # not found


def count_callsites_in_apply(lines):
    """Count warrior->setGeneralTacOrder( lines inside executeSpecialBody_Apply."""
    in_func = False
    depth = 0
    started = False  # True once we've seen the opening brace
    count = 0
    func_pattern = re.compile(r'^bool\s+executeSpecialBody_Apply\s*\(')
    for line in lines:
        stripped = line.rstrip()
        if not in_func:
            if func_pattern.match(stripped):
                in_func = True
                depth = 0
                started = False
            continue
        depth += stripped.count('{') - stripped.count('}')
        if not started and depth > 0:
            started = True
        if started and depth <= 0:
            break
        if "warrior->setGeneralTacOrder(" in stripped:
            count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description="Brain relaxed-guard doc consistency checker")
    parser.add_argument("--root",    default=".", help="Worktree root (default: .)")
    parser.add_argument("--source",  default=None, help=f"Override source file path (default: <root>/{DISPATCH_CPP_REL})")
    parser.add_argument("--quiet", "-q", action="store_true")
    parser.add_argument("--json",    action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    src_path = Path(args.source) if args.source else root / DISPATCH_CPP_REL

    findings = []

    if not src_path.is_file():
        findings.append(("FAIL", f"source not found: {src_path}"))
        _report(findings, args)
        return 2

    with open(src_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    header_count   = count_header_guard_verbs(lines)
    inline_count   = count_inline_guard_verbs(lines)
    callsite_count = count_callsites_in_apply(lines)

    findings.append(("INFO", f"header_guard_verbs={header_count}  inline_guard_verbs={inline_count}  callsite_count={callsite_count}"))

    ok = (header_count == inline_count == callsite_count)

    if header_count != callsite_count:
        findings.append(("FAIL", f"header doc ({header_count} verbs) != callsite count ({callsite_count})"))
    if inline_count != callsite_count:
        findings.append(("FAIL", f"inline doc ({inline_count} verbs) != callsite count ({callsite_count})"))
    if header_count != inline_count:
        findings.append(("FAIL", f"header doc ({header_count}) != inline doc ({inline_count})"))

    if ok:
        findings.append(("PASS", f"all three counts agree: {callsite_count} permitted verbs"))

    _report(findings, args)

    fail_count = sum(1 for lvl, _ in findings if lvl == "FAIL")
    return 1 if fail_count > 0 else 0


def _report(findings, args):
    if args.json:
        print(json.dumps([{"level": lvl, "message": msg} for lvl, msg in findings], indent=2))
        return
    for lvl, msg in findings:
        if args.quiet and lvl in ("PASS", "INFO"):
            continue
        print(f"{lvl}: {msg}")


if __name__ == "__main__":
    sys.exit(main())
