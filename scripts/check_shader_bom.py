#!/usr/bin/env python3
"""check_shader_bom.py -- detect byte-order marks in GLSL shader source files.

UTF-8 BOM (EF BB BF) silently kills shader compilation on AMD/Mesa drivers.
The driver reports "unexpected token" at position 0:1 with no file name,
making this extremely hard to diagnose without a preflight guard.

Checks: shaders/**/*.{vert,frag,geom,comp,hglsl}

Exit 0 = no BOMs.
Exit 1 = one or more files have a BOM.

Usage:
  py -3 scripts/check_shader_bom.py
  py -3 scripts/check_shader_bom.py --fix    # strip BOMs in-place (auto-fix)
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHADERS_DIR = ROOT / "shaders"
SHADER_GLOBS = ["*.vert", "*.frag", "*.geom", "*.comp", "*.hglsl"]

# (bytes_prefix, human_name, strip_length)
BOMS = [
    (b"\xEF\xBB\xBF", "UTF-8 BOM",    3),
    (b"\xFF\xFE",      "UTF-16 LE BOM", 2),
    (b"\xFE\xFF",      "UTF-16 BE BOM", 2),
]


def scan() -> list[tuple[Path, str, int]]:
    """Return list of (path, bom_name, strip_len) for every BOM-infected file."""
    hits: list[tuple[Path, str, int]] = []
    for glob in SHADER_GLOBS:
        for path in sorted(SHADERS_DIR.rglob(glob)):
            try:
                data = path.read_bytes()
            except OSError as e:
                print(f"[bom-check] WARN: cannot read {path.relative_to(ROOT)}: {e}",
                      file=sys.stderr)
                continue
            for bom_bytes, bom_name, strip_len in BOMS:
                if data[:strip_len] == bom_bytes:
                    hits.append((path, bom_name, strip_len))
                    break
    return hits


def main() -> int:
    fix_mode = "--fix" in sys.argv[1:]
    hits = scan()
    if not hits:
        print("[bom-check] OK: no BOMs found in shader sources.")
        return 0

    for path, bom_name, strip_len in hits:
        rel = path.relative_to(ROOT)
        if fix_mode:
            data = path.read_bytes()
            path.write_bytes(data[strip_len:])
            print(f"[bom-check] FIXED: stripped {bom_name} from {rel}")
        else:
            print(f"[bom-check] FAIL: {rel}: starts with {bom_name}", file=sys.stderr)

    if fix_mode:
        print(f"[bom-check] stripped {len(hits)} BOM(s); re-run without --fix to verify.")
        return 0

    print(f"[bom-check] FAIL: {len(hits)} shader file(s) with BOM. "
          "Run with --fix to strip in-place, or strip manually.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
