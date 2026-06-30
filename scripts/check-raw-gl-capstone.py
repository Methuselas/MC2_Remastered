#!/usr/bin/env python3
"""check-raw-gl-capstone.py -- RAW-GL-BYPASS-CAPSTONE-1 (Phase 8 capstone).

ENFORCEMENT-COMPLETENESS META-GATE. This is NOT another per-axis raw-GL gate; it
is the gate that validates the raw-GL ENFORCEMENT SET ITSELF, so the set cannot
silently regress (a gate file deleted, an un-wired run_check, or a gate quietly
gutted to an empty stub). It also records the KNOWN-DEFERRED raw-GL axes as an
explicit ledger, so "no un-gated escape" is a documented decision rather than a
silent gap.

It asserts (Mode A, default, exit 1 on any failure):
  (a) each ENFORCED axis gate SCRIPT EXISTS in scripts/;
  (b) each is WIRED in scripts/check-contracts.sh (a run_check line invoking it);
  (c) each gate script actually CONTAINS its expected axis regex substring
      (a sanity check that it gates the right axis and is not an empty stub).

Mode B (--all) additionally prints the full enforced + known-deferred matrix.

MAINTENANCE RULE (the set stays self-describing only if you follow this):
  * Adding a NEW raw-GL axis gate => add an entry to ENFORCED_AXES below (name,
    gate_script, axis_substr) AND wire its run_check into check-contracts.sh.
    The capstone will then enforce that the new gate exists + is wired + is
    non-stub.
  * Promoting a DEFERRED axis to gated => MOVE it out of KNOWN_DEFERRED and into
    ENFORCED_AXES, and ship its gate script + run_check wiring.

Pure stdlib. No git, no build. Read-only.

Exit code: Mode A -> 1 if the enforcement set is incomplete/un-wired/stubbed,
           else 0. Mode B -> same exit semantics as Mode A (it still validates).
"""
import argparse
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS_DIR = os.path.join(REPO_ROOT, "scripts")
CONTRACTS_SH = os.path.join(SCRIPTS_DIR, "check-contracts.sh")

# ---------------------------------------------------------------------------
# ENFORCED_AXES -- the canonical raw-GL enforcement set. Each entry:
#   name        : human axis label (and the run_check label stem in the .sh)
#   gate_script : scripts/-relative gate filename that must exist
#   axis_substr : a literal substring that MUST appear in the gate script
#                 (the gated GL call name) -- proves the gate targets the right
#                 axis and is not an empty stub.
#
# Adding a new raw-GL axis gate? Add a row here AND wire its run_check into
# check-contracts.sh. (See module docstring MAINTENANCE RULE.)
# ---------------------------------------------------------------------------
ENFORCED_AXES = [
    {"name": "depthFunc",
     "gate_script": "check-raw-gl-depthfunc.py",
     "axis_substr": "glDepthFunc"},
    {"name": "depthMask",
     "gate_script": "check-raw-gl-depthmask.py",
     "axis_substr": "glDepthMask"},
    {"name": "colorMask",
     "gate_script": "check-raw-gl-colormask.py",
     "axis_substr": "glColorMask"},
    {"name": "blendFunc",
     "gate_script": "check-raw-gl-blendfunc.py",
     "axis_substr": "glBlend"},
    {"name": "fboBind",
     "gate_script": "check-raw-gl-fbobind.py",
     "axis_substr": "glBindFramebuffer"},
]

# ---------------------------------------------------------------------------
# KNOWN_DEFERRED -- raw-GL state axes that are INTENTIONALLY NOT gated. This is
# the "no SILENT escape" ledger: deferred is explicit + documented, not
# forgotten. The capstone does NOT enforce these (no gate to validate); it only
# records them so a future reader sees the COMPLETE map (enforced + deferred).
#
# Promoting one of these to gated => move it into ENFORCED_AXES + ship its gate.
# ---------------------------------------------------------------------------
KNOWN_DEFERRED = [
    {"name": "glViewport",
     "reason": "viewport is render-target-mode state owned by the executor "
               "applyTopLevel* render-target path; too many load-bearing "
               "save/restore brackets, no stable per-callsite contract yet."},
    {"name": "glActiveTexture / glBindTexture",
     "reason": "texture-unit latch is far too numerous (every sampler bind) and "
               "is covered descriptively by the sampler-occupancy manifest "
               "(check-sampler-bindings.py); no stable raw-callsite contract."},
    {"name": "glScissor / glEnable(GL_SCISSOR_TEST)",
     "reason": "scissor is used only in narrow UI/clip brackets; low blast "
               "radius, no migration chokepoint defined -- not worth a gate."},
    {"name": "glStencil* (Func/Op/Mask)",
     "reason": "stencil is effectively unused in the current frame; no backlog "
               "to freeze and no contracted owner -- gate would be empty."},
    {"name": "glCullFace / glFrontFace",
     "reason": "cull state is carried in PipelineDesc (CullMode) and emitted by "
               "the sanctioned applyPipeline; descriptive coverage already "
               "exists, raw-callsite gate would duplicate it."},
]


def check_script_exists(entry):
    path = os.path.join(SCRIPTS_DIR, entry["gate_script"])
    return os.path.isfile(path), path


def check_wired(entry, contracts_text):
    """A run_check line in check-contracts.sh must invoke this gate script."""
    return entry["gate_script"] in contracts_text


def check_non_stub(entry, path):
    """The gate script must contain its expected axis substring."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return False
    return entry["axis_substr"] in text


def validate(quiet=False):
    """Returns (ok, failures[]). Asserts existence + wiring + non-stub."""
    failures = []

    try:
        with open(CONTRACTS_SH, "r", encoding="utf-8", errors="replace") as f:
            contracts_text = f.read()
    except OSError as e:
        failures.append("check-contracts.sh unreadable: %s" % e)
        contracts_text = ""

    for entry in ENFORCED_AXES:
        name = entry["name"]
        exists, path = check_script_exists(entry)
        if not exists:
            failures.append("axis '%s': gate script MISSING (%s)"
                            % (name, entry["gate_script"]))
            continue  # can't check wiring-substr usefully without the file
        if not check_wired(entry, contracts_text):
            failures.append("axis '%s': gate '%s' NOT WIRED into "
                            "check-contracts.sh (no run_check invokes it)"
                            % (name, entry["gate_script"]))
        if not check_non_stub(entry, path):
            failures.append("axis '%s': gate '%s' missing expected axis token "
                            "'%s' (empty stub or wrong axis?)"
                            % (name, entry["gate_script"], entry["axis_substr"]))

    ok = not failures
    if not quiet:
        print("[check-raw-gl-capstone] enforcement-set completeness check -- "
              "%d enforced axes, %d deferred axes"
              % (len(ENFORCED_AXES), len(KNOWN_DEFERRED)))
        if ok:
            print("[check-raw-gl-capstone] OK: all %d raw-GL axis gates exist, "
                  "are wired into check-contracts.sh, and are non-stub"
                  % len(ENFORCED_AXES))
    if failures:
        for f in failures:
            print("RAW-GL-CAPSTONE-FAIL %s" % f)
        print("[check-raw-gl-capstone] FAIL: raw-GL enforcement set is "
              "incomplete -- the meta-gate detected %d problem(s). A new gate "
              "must be added to ENFORCED_AXES + wired into check-contracts.sh; "
              "promoting a deferred axis means moving it out of KNOWN_DEFERRED."
              % len(failures), file=sys.stderr)
    return ok, failures


def print_matrix():
    print("")
    print("===== RAW-GL ENFORCEMENT MATRIX =====")
    print("")
    print("ENFORCED (gated -- new gates register here):")
    for entry in ENFORCED_AXES:
        exists, path = check_script_exists(entry)
        try:
            with open(CONTRACTS_SH, "r", encoding="utf-8", errors="replace") as f:
                wired = entry["gate_script"] in f.read()
        except OSError:
            wired = False
        non_stub = exists and check_non_stub(entry, path)
        flags = "exists=%s wired=%s non-stub=%s" % (
            "Y" if exists else "N",
            "Y" if wired else "N",
            "Y" if non_stub else "N")
        print("  %-12s  %-32s [%s]" % (entry["name"], entry["gate_script"], flags))
    print("")
    print("KNOWN-DEFERRED (intentionally NOT gated -- explicit, not forgotten):")
    for entry in KNOWN_DEFERRED:
        print("  %s" % entry["name"])
        print("      reason: %s" % entry["reason"])
    print("")
    print("=====================================")
    print("")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true",
                    help="Mode B: also print the full enforced + deferred matrix")
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="suppress OK chatter; failures still printed")
    args = ap.parse_args()

    if args.all:
        print_matrix()

    ok, _ = validate(quiet=args.quiet)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
