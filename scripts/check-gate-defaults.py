#!/usr/bin/env python3
"""check-gate-defaults.py -- MC2_* env-gate DEFAULT-behavior auditor.

Sweeps every `getenv("MC2_...")` site across the source and heuristically
classifies the ABSENT-env (default) behavior of each gate:

  DEFAULT-OFF  (good) -- unset env branches to the OFF / legacy path. The
                         common safe patterns:
                           getenv(x) != nullptr            -> absent = off
                           getenv(x) && v[0] != '0'        -> absent = off
                           getenv(x) && v[0] == '1'        -> absent = off
                           v==nullptr || strcmp(v,"0")==0  -> absent = off
  DEFAULT-ON   (flag) -- unset env branches to the NEW / enabled path. Some
                         are legitimately default-on shipped features
                         (envFlagDefaultOn / "if (v==nullptr) return true").
                         LISTED FOR REVIEW, not auto-failed -- the value is
                         catching a NEW *accidental* default-on.

Cross-references docs/flags.auto.yaml + docs/tier1_env_vars.md (reused, not
rebuilt) to mark each gate UNDOCUMENTED when it is neither registered nor in
tier1_env_vars.md.

Exit 0 (report) always, EXCEPT with --strict-undocumented, which exits 1 if
any undocumented gate exists (future gate for new gates).

Usage:
  py -3 scripts/check-gate-defaults.py
  py -3 scripts/check-gate-defaults.py --strict-undocumented
  py -3 scripts/check-gate-defaults.py --only MC2_VULKAN,MC2_DETERMINISTIC_RNG
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAML_PATH = os.path.join(REPO_ROOT, "docs", "flags.auto.yaml")
DOCS_PATH = os.path.join(REPO_ROOT, "docs", "tier1_env_vars.md")

SWEEP_DIRS = ["mclib", "GameOS", "code", "RenderCore", "RenderWorld",
              "GameAdapters", "editor", "GuiRuntime", "gui"]
SRC_EXT = (".cpp", ".h", ".hpp", ".cxx", ".cc", ".inl")

# getenv("MC2_...") -- allow std:: prefix, whitespace, single/double colons.
GETENV_RE = re.compile(r'(?:std\s*::\s*)?getenv\s*\(\s*"(MC2_[A-Z0-9_]+)"')

# Helpers whose whole job is "absent env -> ON".
DEFAULT_ON_HELPER_RE = re.compile(r'\b\w*[eE]nvFlagDefaultOn\s*\(\s*"(MC2_[A-Z0-9_]+)"')


def iter_src_files():
    for d in SWEEP_DIRS:
        base = os.path.join(REPO_ROOT, d)
        if not os.path.isdir(base):
            continue
        for root, _dirs, files in os.walk(base):
            for fn in files:
                if fn.endswith(SRC_EXT):
                    yield os.path.join(root, fn)


def classify(window):
    """Given a small text window around a getenv site, return one of
    DEFAULT-OFF / DEFAULT-ON / UNKNOWN. Heuristic, conservative:
    only report DEFAULT-ON when a positive absent->on signal is present."""
    w = window

    # Strong DEFAULT-ON signals (absent env yields the new/enabled path).
    on_signals = [
        r'if\s*\(\s*\w+\s*==\s*(?:nullptr|NULL|0)\s*\)\s*return\s+true',
        r'if\s*\(\s*!\s*\w+\s*\)\s*return\s+true',
        r'==\s*(?:nullptr|NULL)\s*\)\s*\?\s*true',
        r'\?\s*true\s*:',        # (v ? cmp : true) style below handled loosely
    ]
    # unset -> on: `v == nullptr || strcmp(v,"0")==0 ... ` guarding legacy
    # is DEFAULT-OFF; but `v == nullptr` alone selecting new path is ON.
    for pat in on_signals:
        if re.search(pat, w):
            return "DEFAULT-ON"

    # Strong DEFAULT-OFF signals (absent env yields off/legacy).
    off_signals = [
        r'!=\s*(?:nullptr|NULL|0)\b',          # getenv(x) != nullptr
        r'==\s*(?:nullptr|NULL)\s*\)\s*\?\s*false',
        r"\[\s*0\s*\]\s*(?:!=|==)\s*'[01]'",   # v[0] != '0' / == '1'
        r"\bstrcmp\s*\([^)]*\"1\"\s*\)\s*==\s*0",
        r'if\s*\(\s*\w+\s*==\s*(?:nullptr|NULL|0)\s*\)\s*return\s+false',
        r'if\s*\(\s*!\s*\w+\s*\)\s*return\s+false',
        r'\bgetenv\s*\(\s*"MC2_[A-Z0-9_]+"\s*\)\s*\)',  # bare `if (getenv(x))`
        r'v\s*&&\s*v\s*\[',                    # (v && v[0]...) truthy-guard
    ]
    for pat in off_signals:
        if re.search(pat, w):
            return "DEFAULT-OFF"

    return "UNKNOWN"


def sweep_gates(only=None):
    """Return {name: {'default': str, 'sites': [(relpath,line)]}}."""
    gates = {}
    for path in iter_src_files():
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
        except OSError:
            continue
        text = "".join(lines)

        # Names via the default-on helper are DEFAULT-ON regardless of window.
        helper_on = set(DEFAULT_ON_HELPER_RE.findall(text))

        for i, line in enumerate(lines):
            for name in GETENV_RE.findall(line):
                if only and name not in only:
                    continue
                rel = os.path.relpath(path, REPO_ROOT).replace("\\", "/")
                site = (rel, i + 1)
                # window: this line + a few after (branch usually follows).
                window = "".join(lines[max(0, i - 1):i + 4])
                verdict = "DEFAULT-ON" if name in helper_on else classify(window)

                g = gates.setdefault(name, {"default": "UNKNOWN", "sites": []})
                g["sites"].append(site)
                # Resolve across multiple sites: any ON wins (worst case for
                # audit), else any OFF, else UNKNOWN.
                prev = g["default"]
                if verdict == "DEFAULT-ON" or prev == "DEFAULT-ON":
                    g["default"] = "DEFAULT-ON"
                elif verdict == "DEFAULT-OFF" or prev == "DEFAULT-OFF":
                    g["default"] = "DEFAULT-OFF"
    return gates


def load_documented():
    """Names that are registered OR present in tier1_env_vars.md."""
    documented = set()
    if os.path.isfile(YAML_PATH):
        with open(YAML_PATH, "r", encoding="utf-8") as f:
            cur = None
            for line in f:
                m = re.match(r"^  - name: (\S+)\s*$", line)
                if m:
                    cur = m.group(1)
                    continue
                if cur and re.match(r"^    registered: yes\s*$", line):
                    documented.add(cur)
                if cur and re.match(
                        r"^    documented_in_tier1_env_vars: yes\s*$", line):
                    documented.add(cur)
    if os.path.isfile(DOCS_PATH):
        with open(DOCS_PATH, "r", encoding="utf-8") as f:
            documented |= set(re.findall(r"MC2_[A-Z0-9_]+", f.read()))
    return documented


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict-undocumented", action="store_true",
                    help="exit 1 if any swept gate is undocumented")
    ap.add_argument("--only", default="",
                    help="comma-separated gate names to restrict the sweep to")
    args = ap.parse_args()

    only = {s.strip() for s in args.only.split(",") if s.strip()} or None
    gates = sweep_gates(only=only)
    documented = load_documented()

    names = sorted(gates)
    off = [n for n in names if gates[n]["default"] == "DEFAULT-OFF"]
    on = [n for n in names if gates[n]["default"] == "DEFAULT-ON"]
    unknown = [n for n in names if gates[n]["default"] == "UNKNOWN"]
    undocumented = [n for n in names if n not in documented]

    print("== MC2_* gate-default audit (report) ==")
    print("total gates swept: %d" % len(names))
    print("  DEFAULT-OFF (good):  %d" % len(off))
    print("  DEFAULT-ON  (review): %d" % len(on))
    print("  UNKNOWN (unclassified heuristic): %d" % len(unknown))
    print("  UNDOCUMENTED (not registered / not in tier1_env_vars.md): %d"
          % len(undocumented))
    print()

    print("DEFAULT-ON gates (review -- some legitimately default-on features):")
    for n in on:
        s = gates[n]["sites"][0]
        print("   ON  %-40s %s:%d" % (n, s[0], s[1]))
    if not on:
        print("   (none)")
    print()

    if unknown:
        print("UNKNOWN gates (heuristic could not classify -- inspect):")
        for n in unknown:
            s = gates[n]["sites"][0]
            print("   ??  %-40s %s:%d" % (n, s[0], s[1]))
        print()

    print("UNDOCUMENTED gates: %d" % len(undocumented))
    for n in undocumented:
        print("   UNDOC %-38s [%s]" % (n, gates[n]["default"]))
    print()

    if args.strict_undocumented and undocumented:
        print("STRICT FAIL: %d undocumented gate(s) -- register or document "
              "in docs/tier1_env_vars.md" % len(undocumented))
        return 1

    print("OK (report-only; --strict-undocumented to gate undocumented gates)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
