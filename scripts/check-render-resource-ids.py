#!/usr/bin/env python3
"""check-render-resource-ids.py -- GPU-BUFFER-LEDGER-STRICT-1 id-allocation guard.

Parses `enum class RenderResourceId` (RenderResourceRegistry.h) + the
toString(RenderResourceId) switch (RenderResourceRegistry.cpp) and enforces that
resource-id ALLOCATION stays clean. Pure stdlib grep/parse; no build.

FAILs (exit nonzero, offender named) on:

  1. DUPLICATE VALUE -- two enumerators resolve to the same numeric value (the
     stale-worktree id collision this guard exists to catch). Values may be
     explicit (`Foo = 12`) or implicit (previous + 1); both are resolved.
  2. TOSTRING MISSING -- an enum id with no `case RenderResourceId::<id>:` in
     toString() (every id must be labelled; no silent "unknown" fallthrough).

WARNs (informational, exit unaffected) on:

  3. GAP -- non-contiguous id values (a hole in the [min..max] range). Gaps are
     legal but flagged so allocation stays dense/auditable.

NOTE (rule 4, lifetime): owner-registered ids should carry a lifetime, but
lifetime is a RUNTIME property (set at registerOrUpdateRenderResource call
sites, validated by validateRenderResourceLifetimes + the doctest), not
statically visible in this enum. SKIPPED here by design; covered by
check-gpu-buffer-owners.py (concrete-lifetime rule) + the runtime doctest.
"""
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY_H   = os.path.join(REPO_ROOT, "RenderCore", "RenderResourceRegistry.h")
REGISTRY_CPP = os.path.join(REPO_ROOT, "RenderCore", "RenderResourceRegistry.cpp")

# Sentinels that are not real allocatable resource slots.
SENTINELS = {"Unknown", "Count"}


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_enum():
    """Return ordered list of (name, value) for RenderResourceId, resolving
    implicit `previous + 1` values. Returns None if the enum can't be found."""
    text = read(REGISTRY_H)
    m = re.search(r"enum\s+class\s+RenderResourceId\s*:[^\{]*\{(.*?)\};",
                  text, re.DOTALL)
    if not m:
        return None
    body = re.sub(r"//.*$", "", m.group(1), flags=re.MULTILINE)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.DOTALL)
    out = []
    nextval = 0
    for raw in body.split(","):
        raw = raw.strip()
        if not raw:
            continue
        mm = re.match(r"^([A-Za-z_]\w*)\s*(?:=\s*(\w+))?$", raw)
        if not mm:
            continue
        name = mm.group(1)
        if mm.group(2) is not None:
            val = int(mm.group(2), 0)
        else:
            val = nextval
        out.append((name, val))
        nextval = val + 1
    return out


def parse_tostring_cases():
    """Set of enum names with a toString(RenderResourceId) case."""
    text = read(REGISTRY_CPP)
    return set(re.findall(
        r"case\s+RenderResourceId::(\w+)\s*:", text))


def main():
    for p in (REGISTRY_H, REGISTRY_CPP):
        if not os.path.exists(p):
            print("[check-render-resource-ids] ERROR: missing %s"
                  % os.path.relpath(p, REPO_ROOT), file=sys.stderr)
            return 2

    enum = parse_enum()
    if not enum:
        print("[check-render-resource-ids] ERROR: could not parse "
              "enum class RenderResourceId", file=sys.stderr)
        return 2
    tostring = parse_tostring_cases()

    failures = []
    warnings = []

    # Rule 1: duplicate value.
    by_val = {}
    for name, val in enum:
        by_val.setdefault(val, []).append(name)
    for val, names in sorted(by_val.items()):
        if len(names) > 1:
            failures.append("duplicate RenderResourceId value %d shared by: %s"
                            % (val, ", ".join(names)))

    # Rule 2: toString must cover every id (including sentinels; the switch is
    # exhaustive over the enum).
    for name, _ in enum:
        if name not in tostring:
            failures.append("RenderResourceId::%s has no toString() case" % name)

    # Rule 3 (warn): gap in the real (non-sentinel) id range.
    real = sorted(v for n, v in enum if n not in SENTINELS)
    if real:
        present = set(real)
        for v in range(real[0], real[-1] + 1):
            if v not in present:
                warnings.append("id value %d unused (gap in %d..%d)"
                                % (v, real[0], real[-1]))

    for w in warnings:
        print("[check-render-resource-ids] WARN: %s" % w)

    if failures:
        print("[check-render-resource-ids] FAIL:", file=sys.stderr)
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        return 1

    print("[check-render-resource-ids] PASS -- %d ids, no duplicate values, "
          "toString covers all, %d gap-warning(s). (lifetime = runtime, see "
          "doctest/check-gpu-buffer-owners.)" % (len(enum), len(warnings)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
