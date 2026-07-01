#!/usr/bin/env python3
"""check-gpu-buffer-owners.py -- GPU-BUFFER-OWNER-CI-GATE-1 static invariant gate.

Promotes the offline GpuBufferOwnerLifetimeCheck doctest (tests/unit/
test_gpu_buffer_owner.cpp) to a durable CI gate so a future code change can't
silently regress the completed GPU-buffer-owner arc. The doctest proves the
RUNTIME invariant (owners register with a non-Unset lifetime); this gate proves
the STATIC source invariant on every CI pass, no build required.

Every `RenderCore::GpuBufferOwner s_foo{ id, lifetime, name, glName }`
construction site (in GameOS/gameos/*.cpp + RenderCore/*.cpp) is parsed and the
4 positional initializer args are checked:

  1. ID REGISTERED  -- the RenderResourceId arg is a real enumerator (appears in
     `enum class RenderResourceId` in RenderResourceRegistry.h) AND has a
     toString() case in RenderResourceRegistry.cpp (no orphan id).
  2. CONCRETE LIFETIME -- the RenderResourceLifetime arg is present and is a
     concrete class (Persistent / Mission / FrameLocal / External), NOT a
     defaulted/Unset one. An owner constructed with ::Unset (or a missing
     lifetime arg) FAILs -- exactly the regression this gate blocks.
  3. NAME MATCHES ID -- the debugName string literal equals toString(id), so the
     registry's human label can't drift from the logical id.

Additionally asserts the runtime doctest suite still EXISTS so the runtime arm
of the invariant stays covered:

  4. DOCTEST PRESENT -- TEST_SUITE("GpuBufferOwnerLifetimeCheck") is present in
     tests/unit/test_gpu_buffer_owner.cpp.

Pure stdlib, grep/parse only. Exit 0 = all owners clean; nonzero + message on
any violation.
"""
import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

REGISTRY_H   = os.path.join(REPO_ROOT, "RenderCore", "RenderResourceRegistry.h")
REGISTRY_CPP = os.path.join(REPO_ROOT, "RenderCore", "RenderResourceRegistry.cpp")
DOCTEST      = os.path.join(REPO_ROOT, "tests", "unit", "test_gpu_buffer_owner.cpp")

# TUs that may host an owner construction site.
SCAN_DIRS = [
    os.path.join(REPO_ROOT, "GameOS", "gameos"),
    os.path.join(REPO_ROOT, "RenderCore"),
]

CONCRETE_LIFETIMES = {"FrameLocal", "Mission", "Persistent", "External"}

# GPU-BUFFER-EXCLUSION-LEDGER-1: valid TIER2 exclusion reasons.
TIER2_REASONS = {"ring", "substrate-gated", "dead-path"}

# A glGenBuffers target var name is in-scope for the owned-OR-excluded partition
# iff it looks like an SSBO or UBO (the frame-graph resource kinds we own). VBO /
# IBO / VB / IB / PBO / CmdBuf / generic `local`/`buffer`/`id` handles are out of
# scope by design (per-pass-rebind / vertex-index data, not owned resources).
SSBO_UBO_NAME = re.compile(r"[Ss][Ss][Bb][Oo]|[Uu][Bb][Oo]")

# `glGenBuffers(<n>, &<var>)` -- capture the target var name.
GEN_BUFFERS = re.compile(r"glGenBuffers\s*\(\s*[^,]+,\s*&\s*([A-Za-z_]\w*)\s*\)")

# `// TIER2-EXCLUDED: <reason>`
TIER2_TAG = re.compile(r"//\s*TIER2-EXCLUDED:\s*([A-Za-z0-9_-]+)")

# `RenderCore::GpuBufferOwner   s_name{` (var-decl construction, not a typedef/include/comment).
OWNER_DECL = re.compile(
    r"(?:RenderCore::)?GpuBufferOwner\s+[A-Za-z_]\w*\s*\{", re.M)


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_enum_ids():
    """Names in `enum class RenderResourceId` (excluding Unknown/Count)."""
    text = read(REGISTRY_H)
    m = re.search(r"enum\s+class\s+RenderResourceId\s*:[^\{]*\{(.*?)\};", text, re.DOTALL)
    if not m:
        return None
    body = re.sub(r"//.*$", "", m.group(1), flags=re.MULTILINE)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.DOTALL)
    ids = set()
    for raw in body.split(","):
        name = raw.split("=")[0].strip()
        if re.match(r"^[A-Za-z_]\w*$", name):
            ids.add(name)
    return ids


def parse_tostring_cases():
    """{idName: literal} from toString(RenderResourceId) cases."""
    text = read(REGISTRY_CPP)
    out = {}
    for m in re.finditer(
            r"case\s+RenderResourceId::(\w+)\s*:\s*return\s+\"([^\"]*)\"", text):
        out[m.group(1)] = m.group(2)
    return out


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.DOTALL)
    s = re.sub(r"//.*$", "", s, flags=re.MULTILINE)
    return s


def owner_sites():
    """Yield (relpath, lineno, args[]) for each owner construction site.

    args = the 4 positional initializer tokens between the outermost braces:
    [id-expr, lifetime-expr, name-literal-or-expr, glName-expr].
    """
    for d in SCAN_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(d, fn)
            rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            text = read(path)
            for m in OWNER_DECL.finditer(text):
                brace_open = m.end() - 1  # index of the `{`
                lineno = text.count("\n", 0, m.start()) + 1
                # Find the matching close brace.
                depth = 0
                i = brace_open
                while i < len(text):
                    c = text[i]
                    if c == "{":
                        depth += 1
                    elif c == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    i += 1
                inner = strip_comments(text[brace_open + 1:i])
                # Split on top-level commas (no nesting expected in these PODs).
                args = [a.strip() for a in inner.split(",") if a.strip()]
                yield rel, lineno, args


def owner_aliased_names():
    """SSBO/UBO-named vars that ARE owned even though glGenBuffers writes them.

    Two owner patterns in the tree write the raw handle without a temp var:
      (a) `#define <var> (<owner>.glName)`  -- <var> is a macro alias for the
          owner's glName field (e.g. s_lightDataSsbo, s_dynamicPropShadowSsbo).
      (b) `<owner>.glName = <var>` near a glGenBuffers(&<var>) -- the freshly
          generated name is stored straight into an owner (e.g. view_uniforms
          `ubo`). Detected per-site in the partition scan, not here.
    Returns the set of macro-aliased names (pattern a).
    """
    aliased = set()
    for d in SCAN_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not (fn.endswith(".cpp") or fn.endswith(".h")):
                continue
            text = read(os.path.join(d, fn))
            for m in re.finditer(
                    r"#define\s+([A-Za-z_]\w*)\s*\(\s*[A-Za-z_]\w*\.glName\s*\)",
                    text):
                aliased.add(m.group(1))
    return aliased


def check_partition(aliased):
    """Every SSBO/UBO-named glGenBuffers target is owned OR // TIER2-EXCLUDED.

    Returns (failures, n_owned, n_excluded).
    """
    failures = []
    n_owned = 0
    n_excluded = 0
    for d in SCAN_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(d, fn)
            rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            lines = read(path).splitlines()
            for i, line in enumerate(lines):
                gm = GEN_BUFFERS.search(line)
                if not gm:
                    continue
                var = gm.group(1)
                if not SSBO_UBO_NAME.search(var):
                    continue  # VBO/IBO/generic -- out of scope.
                where = "%s:%d %s" % (rel, i + 1, var)

                # OWNED via macro alias (#define var (owner.glName)).
                if var in aliased:
                    n_owned += 1
                    continue
                # OWNED via `<owner>.glName = <var>` within a few lines (the
                # temp-name-assigned-to-owner pattern, e.g. view_uniforms ubo).
                lookahead = "\n".join(lines[i:i + 4])
                if re.search(r"\.glName\s*=\s*(?:static_cast<[^>]+>\s*\(\s*)?"
                             + re.escape(var) + r"\b", lookahead):
                    n_owned += 1
                    continue

                # EXCLUDED via a // TIER2-EXCLUDED tag on this line or the 2
                # lines above it.
                tag = None
                for j in range(max(0, i - 2), i + 1):
                    tm = TIER2_TAG.search(lines[j])
                    if tm:
                        tag = tm.group(1)
                        break
                if tag is None:
                    failures.append(
                        "%s -- un-owned SSBO/UBO with NO // TIER2-EXCLUDED tag "
                        "(must be a GpuBufferOwner OR tagged ring/"
                        "substrate-gated/dead-path)" % where)
                elif tag not in TIER2_REASONS:
                    failures.append(
                        "%s -- // TIER2-EXCLUDED reason %r not in %s"
                        % (where, tag, sorted(TIER2_REASONS)))
                else:
                    n_excluded += 1
    return failures, n_owned, n_excluded


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="suppress OK chatter; offenders still printed")
    args = ap.parse_args()

    for p in (REGISTRY_H, REGISTRY_CPP, DOCTEST):
        if not os.path.exists(p):
            print("[check-gpu-buffer-owners] ERROR: missing %s"
                  % os.path.relpath(p, REPO_ROOT), file=sys.stderr)
            return 2

    enum_ids = parse_enum_ids()
    if not enum_ids:
        print("[check-gpu-buffer-owners] ERROR: could not parse "
              "enum class RenderResourceId", file=sys.stderr)
        return 2
    tostring = parse_tostring_cases()

    failures = []
    sites = list(owner_sites())
    if not sites:
        print("[check-gpu-buffer-owners] ERROR: parsed 0 GpuBufferOwner "
              "construction sites (parser broke?)", file=sys.stderr)
        return 2

    ok = []
    for rel, lineno, av in sites:
        where = "%s:%d" % (rel, lineno)
        if len(av) < 3:
            failures.append("%s -- only %d initializer args (need id, "
                            "lifetime, name, ...)" % (where, len(av)))
            continue

        id_arg, life_arg, name_arg = av[0], av[1], av[2]

        # 1. id registered (enum + toString case).
        idm = re.search(r"RenderResourceId::(\w+)", id_arg)
        if not idm:
            failures.append("%s -- arg0 is not a RenderResourceId:: enumerator: %r"
                            % (where, id_arg))
            continue
        idname = idm.group(1)
        if idname not in enum_ids:
            failures.append("%s -- RenderResourceId::%s not in enum (orphan id)"
                            % (where, idname))
        if idname not in tostring:
            failures.append("%s -- RenderResourceId::%s has no toString() case "
                            "(orphan id)" % (where, idname))

        # 2. concrete, non-Unset lifetime.
        lifem = re.search(r"RenderResourceLifetime::(\w+)", life_arg)
        if not lifem:
            failures.append("%s -- arg1 is not a RenderResourceLifetime:: "
                            "value: %r" % (where, life_arg))
        else:
            life = lifem.group(1)
            if life == "Unset":
                failures.append("%s -- owner %s constructed with Unset lifetime "
                                "(defaulted/regression)" % (where, idname))
            elif life not in CONCRETE_LIFETIMES:
                failures.append("%s -- unknown lifetime %s" % (where, life))

        # 3. debugName literal matches toString(id).
        nm = re.match(r'^\s*"([^"]*)"\s*$', name_arg)
        if not nm:
            failures.append("%s -- arg2 debugName is not a string literal: %r"
                            % (where, name_arg))
        elif idname in tostring and nm.group(1) != tostring[idname]:
            failures.append("%s -- debugName %r != toString(%s)=%r"
                            % (where, nm.group(1), idname, tostring[idname]))

        if not any(f.startswith(where) for f in failures):
            ok.append((where, idname,
                       lifem.group(1) if lifem else "?"))

    # 4. runtime doctest suite still present.
    if 'TEST_SUITE("GpuBufferOwnerLifetimeCheck")' not in read(DOCTEST):
        failures.append("tests/unit/test_gpu_buffer_owner.cpp -- runtime suite "
                        'TEST_SUITE("GpuBufferOwnerLifetimeCheck") missing '
                        "(runtime invariant arm gone)")

    # GPU-BUFFER-EXCLUSION-LEDGER-1: owned-OR-excluded partition over every
    # SSBO/UBO-named glGenBuffers target.
    part_failures, n_owned_gen, n_excluded = check_partition(owner_aliased_names())
    failures.extend(part_failures)

    if failures:
        print("[check-gpu-buffer-owners] FAIL:", file=sys.stderr)
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        return 1

    if not args.quiet:
        print("[check-gpu-buffer-owners] %d GpuBufferOwner construction site(s):"
              % len(sites))
        for where, idname, life in ok:
            print("  OK  %-30s %-12s %s" % (idname, life, where))
        print("[check-gpu-buffer-owners] SSBO/UBO partition: %d owned "
              "(GpuBufferOwner), %d excluded-by-tag, 0 unclassified."
              % (n_owned_gen, n_excluded))
        print("[check-gpu-buffer-owners] PASS -- all owners use a registered id "
              "+ concrete non-Unset lifetime + matching name; every SSBO/UBO "
              "glGenBuffers is owned OR // TIER2-EXCLUDED; runtime doctest present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
