#!/usr/bin/env python3
"""tools/shader_schema/validate.py — Shader ABI validation (SHADER-SCHEMA-1).

Reads manifest.json, validates each interface against:
  - Reflected GLSL struct size  (array_stride in shader_reflect golden JSON)
  - Required shader golden files present  (compilation passed)
  - GLSL source field presence and std430 layout
  - C++ static_assert size consistency

Exit 0 = all PASS.  Exit 1 = any FAIL.

Usage:
  python3 tools/shader_schema/validate.py           # run from repo root
  python3 tools/shader_schema/validate.py --quiet   # suppress PASS lines
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REFLECT_EXPECTED = Path("tools/shader_reflect/expected")
MANIFEST_PATH = Path("tools/shader_schema/manifest.json")
PREFIX = "[SHADER_SCHEMA v1]"

# std430 scalar/vector sizes (bytes) and base alignments.
# Arrays (e.g. float[4]) are not needed here; GLSL structs use vec types.
_STD430: dict[str, tuple[int, int]] = {
    "bool":  (4,  4),  "int":  (4,  4),  "uint": (4,  4),  "float": (4,  4),
    "double": (8, 8),
    "vec2":  (8,  8),  "ivec2": (8,  8),  "uvec2": (8,  8),
    "vec3":  (12, 16), "ivec3": (12, 16), "uvec3": (12, 16),
    "vec4":  (16, 16), "ivec4": (16, 16), "uvec4": (16, 16),
    "mat2":  (32,  8), "mat3":  (48, 16), "mat4":  (64, 16),
}


def _align(offset: int, alignment: int) -> int:
    return (offset + alignment - 1) & ~(alignment - 1)


def _golden_path(shader_path: str, variant: str) -> Path:
    key = shader_path.replace("/", "__") + f"__{variant}"
    return REFLECT_EXPECTED / f"{key}.json"


def _read_golden(shader_path: str, variant: str) -> tuple[dict | None, Path]:
    p = _golden_path(shader_path, variant)
    if not p.exists():
        return None, p
    with open(p, encoding="utf-8") as f:
        return json.load(f), p


def _golden_array_stride(golden: dict, expected_size: int) -> tuple[bool, str | None]:
    """Return (found, buffer_name) for first SSBO member with matching array_stride."""
    for ssbo in golden.get("ssbos", []):
        for member in ssbo.get("members", []):
            if member.get("array_stride") == expected_size:
                return True, ssbo["name"]
    return False, None


def _parse_glsl_struct(source_path: Path, glsl_type: str) -> list[tuple[str, str]] | None:
    """Extract ordered (name, glsl_type) pairs from a GLSL struct definition."""
    text = source_path.read_text(encoding="utf-8")
    m = re.search(
        r'\bstruct\s+' + re.escape(glsl_type) + r'\s*\{([^}]*)\}',
        text,
    )
    if not m:
        return None
    fields: list[tuple[str, str]] = []
    for line in m.group(1).splitlines():
        stripped = re.sub(r'//.*$', '', line).strip().rstrip(';').strip()
        if not stripped:
            continue
        parts = stripped.split()
        if len(parts) >= 2:
            ftype = parts[0]
            fname = parts[1].split('[')[0]
            fields.append((fname, ftype))
    return fields


def _parse_cpp_asserts(header_path: Path, cpp_type: str) -> tuple[int | None, dict[str, int]]:
    """Parse static_assert(sizeof) and static_assert(offsetof) from C++ header."""
    text = header_path.read_text(encoding="utf-8")
    type_pat = r'(?:\w+::)?' + re.escape(cpp_type)

    size_m = re.search(
        r'static_assert\s*\(\s*sizeof\s*\(\s*' + type_pat + r'\s*\)\s*==\s*(\d+)',
        text,
    )
    cpp_size = int(size_m.group(1)) if size_m else None

    offsets: dict[str, int] = {}
    for om in re.finditer(
        r'static_assert\s*\(\s*offsetof\s*\(\s*' + type_pat + r'\s*,\s*(\w+)\s*\)\s*==\s*(\d+)',
        text,
    ):
        offsets[om.group(1)] = int(om.group(2))

    return cpp_size, offsets


def _std430_layout(fields: list[tuple[str, str]]) -> tuple[dict[str, int], int] | None:
    """Compute std430 byte offsets and total size. Returns None on unknown type."""
    layout: dict[str, int] = {}
    offset = 0
    for name, ftype in fields:
        entry = _STD430.get(ftype)
        if entry is None:
            return None
        size, align = entry
        offset = _align(offset, align)
        layout[name] = offset
        offset += size
    # Final struct size is aligned to the largest member alignment (std430).
    max_align = max((_STD430[ft][1] for _, ft in fields if ft in _STD430), default=4)
    total = _align(offset, max_align)
    return layout, total


def validate_interface(iface: dict, quiet: bool) -> bool:
    name = iface["name"]
    expected_size = iface["expectedSize"]
    golden_shader = iface["goldenShader"]
    golden_variant = iface.get("goldenVariant", "default")
    required_shaders: list[str] = iface.get("requiredShaders", [])
    glsl_source = iface["glslSource"]
    glsl_type = iface["glslType"]
    cpp_header = iface["cppHeader"]
    cpp_type = iface["cppType"]

    fails: list[str] = []

    # --- 1. Golden size check ---
    golden, golden_p = _read_golden(golden_shader, golden_variant)
    if golden is None:
        fails.append(f"golden not found: {golden_p}")
    else:
        found, buf_name = _golden_array_stride(golden, expected_size)
        if not found:
            # Collect actual strides for diagnosis
            strides = [
                m.get("array_stride")
                for s in golden.get("ssbos", [])
                for m in s.get("members", [])
            ]
            fails.append(
                f"no SSBO with array_stride={expected_size} in {golden_p.name}"
                + (f" (found strides: {strides})" if strides else " (no SSBOs)")
            )

    # --- 2. Required shader goldens present ---
    for req in required_shaders:
        p = _golden_path(req, "default")
        if not p.exists():
            fails.append(f"required shader golden missing: {p.name}")

    # --- 3. Parse GLSL struct ---
    glsl_path = Path(glsl_source)
    if not glsl_path.exists():
        fails.append(f"GLSL source not found: {glsl_source}")
        _report(name, expected_size, 0, fails)
        return bool(fails)

    fields = _parse_glsl_struct(glsl_path, glsl_type)
    if fields is None:
        fails.append(f"struct {glsl_type} not found in {glsl_source}")
        _report(name, expected_size, 0, fails)
        return bool(fails)

    # --- 4. C++ static_assert consistency ---
    cpp_path = Path(cpp_header)
    cpp_size = None
    cpp_offsets: dict[str, int] = {}
    if cpp_path.exists():
        cpp_size, cpp_offsets = _parse_cpp_asserts(cpp_path, cpp_type)
        if cpp_size is not None and cpp_size != expected_size:
            fails.append(
                f"sizeof mismatch: manifest.expectedSize={expected_size} "
                f"cpp_assert={cpp_size}"
            )
    else:
        fails.append(f"C++ header not found: {cpp_header}")

    # --- 5. std430 layout cross-check ---
    layout_result = _std430_layout(fields)
    if layout_result is None:
        unknown = [ft for _, ft in fields if ft not in _STD430]
        fails.append(f"unknown GLSL type(s) in struct: {unknown}")
    else:
        glsl_offsets, glsl_total = layout_result
        if glsl_total != expected_size:
            fails.append(
                f"GLSL std430 computed size={glsl_total} != expectedSize={expected_size}"
            )
        for field, cpp_off in cpp_offsets.items():
            glsl_off = glsl_offsets.get(field)
            if glsl_off is None:
                fails.append(
                    f"field={field} in cpp offsetof but absent from GLSL struct"
                )
            elif glsl_off != cpp_off:
                fails.append(
                    f"field={field} cppOffset={cpp_off} glslOffset={glsl_off}"
                )

    _report(name, expected_size, len(fields), fails, quiet)
    return not fails


def _report(name: str, size: int, field_count: int, fails: list[str], quiet: bool = False) -> None:
    if fails:
        for f in fails:
            print(f"{PREFIX} FAIL interface={name} {f}", flush=True)
    elif not quiet:
        print(f"{PREFIX} {name} PASS size={size} fields={field_count}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--quiet", action="store_true", help="Suppress PASS output")
    args = parser.parse_args()

    if not MANIFEST_PATH.exists():
        print(f"{PREFIX} ERROR manifest not found: {MANIFEST_PATH}", flush=True)
        sys.exit(1)

    with open(MANIFEST_PATH, encoding="utf-8") as f:
        manifest = json.load(f)

    interfaces = manifest.get("interfaces", [])
    if not interfaces:
        print(f"{PREFIX} ERROR no interfaces in manifest", flush=True)
        sys.exit(1)

    passed = 0
    failed = 0
    for iface in interfaces:
        ok = validate_interface(iface, args.quiet)
        if ok:
            passed += 1
        else:
            failed += 1

    if failed == 0:
        print(f"{PREFIX} PASS interfaces={passed}", flush=True)
        sys.exit(0)
    else:
        print(f"{PREFIX} FAIL {failed}/{len(interfaces)} interfaces failed", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
