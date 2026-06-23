#!/usr/bin/env python3
"""check-shader-injectors.py — SHADER-PERMUTATION-INVENTORY-1 governance gate

Turns the "runtime #define soup" into a GOVERNED list. The inventory
docs/render-backend-seams/shader-permutation-inventory.json is the allowlist:
every shader define INJECTED from C++ must have an entry there. A new injector
added without an inventory entry FAILS — so the shader-variant surface (the
biggest Vulkan PSO blocker) cannot grow ungoverned.

What it does:
  - collects C++-injected shader defines (literal '#define X' inside double-quoted
    strings) across the engine dirs;
  - collects the array-driven injects from g_shader_flags[] (the gosMaterialVariation
    path, whose macro names come from an array var, not a literal);
  - FAIL: an injected macro with no entry in the inventory JSON, or a g_shader_flags[]
    entry not in the inventory;
  - WARN (never fails): a GLSL guard macro with no injector and not marked
    DEAD_OR_STALE / SPECIALIZATION_CONSTANT-self in the inventory (mystery macro);
    an injected macro the inventory marks DEAD_OR_STALE (should be uninjected).

Read-only. No shader/runtime changes. Companion: shader-permutation-inventory.md.

Usage:
  py -3 scripts/check-shader-injectors.py [--root <repo>] [--json <out>] [--quiet]
"""
import argparse
import json
import os
import re
import sys

CPP_DIRS = ["GameOS", "RenderCore", "RenderWorld", "GameAdapters", "mclib", "code"]
GLSL_DIR = "shaders"
CPP_EXT = (".h", ".hpp", ".cpp", ".cc", ".cxx")
GLSL_EXT = (".vert", ".frag", ".comp", ".tesc", ".tese", ".geom", ".glsl", ".hglsl")
EXCLUDE = [os.path.join("tools", ""), os.path.join("build64", ""),
           os.path.join("3rdparty", ""), os.path.join(".claude", "")]

INVENTORY = "docs/render-backend-seams/shader-permutation-inventory.json"

# A literal '#define MACRO' that lives inside a C++ double-quoted string.
RE_CPP_INJECT = re.compile(r'"\s*#\s*define\s+([A-Za-z_]\w*)')
# g_shader_flags[] = { "ALPHA_TEST", "IS_OVERLAY" }; — array-driven injects.
RE_FLAGS_ARRAY = re.compile(
    r"g_shader_flags\s*\[\s*\]\s*=\s*\{(.*?)\}", re.S)
RE_QUOTED = re.compile(r'"([A-Za-z_]\w*)"')
# GLSL conditional guard tokens.
RE_GLSL_GUARD = re.compile(
    r"^\s*#\s*(?:ifdef|ifndef)\s+([A-Za-z_]\w*)"
    r"|^\s*#\s*(?:if|elif)\b.*?\bdefined\s*\(\s*([A-Za-z_]\w*)\s*\)"
    r"|^\s*#\s*(?:if|elif)\s+([A-Za-z_]\w*)\b")
RE_INCLUDE_GUARDISH = re.compile(r"HGLSL|^__\w+__$")


def walk(root, dirs, exts):
    for d in dirs:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dp, _, fns in os.walk(base):
            for fn in fns:
                if not fn.endswith(exts):
                    continue
                p = os.path.join(dp, fn)
                rel = os.path.relpath(p, root).replace("\\", "/")
                if any(s.replace("\\", "/") in rel + "/" for s in EXCLUDE):
                    continue
                yield p, rel


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def collect_cpp_injects(root):
    """macro -> [ 'rel:line', ... ] for literal and array-driven injects."""
    injects = {}
    for p, rel in walk(root, CPP_DIRS, CPP_EXT):
        text = read(p)
        for i, line in enumerate(text.splitlines(), 1):
            for m in RE_CPP_INJECT.finditer(line):
                injects.setdefault(m.group(1), []).append(f"{rel}:{i}")
        # array-driven (gosMaterialVariation flag names)
        am = RE_FLAGS_ARRAY.search(text)
        if am:
            line_no = text.count("\n", 0, am.start()) + 1
            for q in RE_QUOTED.finditer(am.group(1)):
                injects.setdefault(q.group(1), []).append(
                    f"{rel}:{line_no} (g_shader_flags[])")
    return injects


def collect_glsl_guards(root):
    """macro -> [ 'rel:line', ... ] for conditional guards (excl include-guards)."""
    guards = {}
    for p, rel in walk(root, [GLSL_DIR], GLSL_EXT):
        for i, line in enumerate(read(p).splitlines(), 1):
            m = RE_GLSL_GUARD.match(line)
            if not m:
                continue
            tok = m.group(1) or m.group(2) or m.group(3)
            if not tok or RE_INCLUDE_GUARDISH.search(tok):
                continue
            guards.setdefault(tok, []).append(f"{rel}:{i}")
    return guards


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    inv_path = os.path.join(root, INVENTORY)
    inv = json.load(open(inv_path, "r", encoding="utf-8"))
    inv_macros = {m["name"]: m for m in inv["macros"]}
    dead = {n for n, m in inv_macros.items() if m["class"] == "DEAD_OR_STALE"}
    self_const = {n for n, m in inv_macros.items()
                  if m["class"] == "SPECIALIZATION_CONSTANT" and not m.get("injected")}

    injects = collect_cpp_injects(root)
    guards = collect_glsl_guards(root)

    fails, warns = [], []

    # FAIL: injected macro with no inventory entry --------------------------
    for macro, sites in sorted(injects.items()):
        if macro not in inv_macros:
            fails.append(
                f"ungoverned injector: '{macro}' is injected from C++ "
                f"({sites[0]}) but has NO entry in {INVENTORY} "
                f"(add an inventory row classifying it)")
        elif macro in dead:
            warns.append(
                f"'{macro}' is marked DEAD_OR_STALE in the inventory but IS "
                f"injected ({sites[0]}) — reclassify or remove the injector")

    # WARN: GLSL guard with no injector and not known dead/self-const -------
    for macro, sites in sorted(guards.items()):
        if macro in injects:
            continue
        if macro in dead or macro in self_const:
            continue
        if macro in inv_macros:
            continue  # documented (e.g. covered elsewhere)
        warns.append(
            f"mystery guard: '{macro}' tested in GLSL ({sites[0]}) with no "
            f"C++ injector and no inventory entry")

    report = {
        "summary": {
            "verdict": inv["summary"]["verdict"],
            "injected_macros": len(injects),
            "glsl_guard_macros": len(guards),
            "inventory_macros": len(inv_macros),
            "fails": len(fails),
            "warns": len(warns),
        },
        "injected": {k: v for k, v in sorted(injects.items())},
        "fails": fails,
        "warns": warns,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

    if not args.quiet:
        print("[check-shader-injectors] SHADER-PERMUTATION-INVENTORY-1")
        print(f"  inventory verdict     : {inv['summary']['verdict']}")
        print(f"  C++-injected macros   : {len(injects)}")
        print(f"  GLSL guard macros     : {len(guards)}")
        print(f"  inventory entries     : {len(inv_macros)}")
        for w in warns:
            print(f"  WARN: {w}")
        for fl in fails:
            print(f"  FAIL: {fl}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(fails)} fail, {len(warns)} warn)")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
