# Shader Contract / Reflection CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Tier 1.2 CI gate that compiles every shader variant to SPIR-V, reflects bindings/outputs/member offsets via spirv-cross, and fails on drift from committed golden JSON files.

**Architecture:** Two sequenced commits. Commit 1 extracts `scripts/shader_common.py` from `validate_shaders.py` (pure mechanical refactor, behavior identical). Commit 2 adds `tools/shader_reflect/reflect.py` which uses the shared source-construction logic, runs spirv-cross --reflect, compares against `expected/*.json` goldens, and enforces hardcoded invariants that cannot be bypassed by `--update`.

**Tech Stack:** Python 3.9+, glslangValidator (Vulkan SDK), spirv-cross (Vulkan SDK), no new Python deps.

**Spec:** `docs/superpowers/specs/2026-05-23-shader-contract-ci-design.md`

---

## File map

**Commit 1 — create + modify:**
- Create: `scripts/shader_common.py`
- Modify: `scripts/validate_shaders.py` (imports only, behavior unchanged)

**Commit 2 — create:**
- Create: `tools/shader_reflect/reflect.py`
- Create: `tools/shader_reflect/expected/*.json` (bootstrapped by `--update`)

---

## Task 0: Capture validate_shaders.py baseline (before any edits)

**Files:** none — read-only

This step must run before Task 1 touches any file, so the diff proves behavior
is unchanged by the extraction, not that the tool happens to work after.

- [ ] **Step 0.1: Capture baseline output**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
py -3 scripts/validate_shaders.py 2>&1 | Tee-Object baseline.txt
```

Expected: `validate_shaders: N passed, 0 failed, 1 skipped (out of N+1) [Vulkan/SPIR-V]`

Save `baseline.txt` — it is used in Task 2 Step 2.3.

---

## Task 1: Create `scripts/shader_common.py`

**Files:**
- Create: `scripts/shader_common.py`

This is a pure mechanical extraction from `validate_shaders.py`. Every function body is copied verbatim. One new function `build_shader_source` is added.

- [ ] **Step 1.1: Create shader_common.py with extracted symbols**

Create `scripts/shader_common.py` with this exact content:

```python
"""scripts/shader_common.py — shared shader source construction.

Extracted from validate_shaders.py. Both validate_shaders.py and
tools/shader_reflect/reflect.py import from here so both gates see
the exact same source string the engine compiles.

Do NOT import from this module outside scripts/ and tools/shader_reflect/.
"""
from __future__ import annotations

import os
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "shaders"
INCLUDE = SHADERS / "include"

VERSION_PREFIX = "#version 430\n"
INCLUDE_EXTENSION = "#extension GL_GOOGLE_include_directive : require\n"
_INCLUDE_RE = re.compile(r"(?m)^\s*#\s*include\b")
ENGINE_PATH_SEPARATOR = "/"

STAGE_BY_EXT: dict[str, str] = {
    ".vert": "vert",
    ".frag": "frag",
    ".tesc": "tesc",
    ".tese": "tese",
    ".geom": "geom",
    ".comp": "comp",
}

SKIP_SHADERS: dict[str, str] = {
    "gos_terrain_lighting.comp": (
        "programmatically stitched at runtime via "
        "tl_build_terrain_lighting_program; standalone validation cannot "
        "reach this shader without replicating the engine's include stitching"
    ),
}

SHADER_PREFIX_OVERRIDE: dict[str, str] = {
    "gos_terrain_water_fast_mdi.vert": "#version 460\n",
}

SHADER_TOKEN_REWRITES: dict[str, list[tuple[str, str]]] = {
    "gos_terrain_water_fast_mdi.vert": [
        ("gl_DrawIDARB", "gl_DrawID"),
    ],
}


def _engine_get_path(fname: str) -> str:
    idx = max(fname.rfind("/"), fname.rfind("\\"))
    if idx < 0:
        return ""
    return fname[:idx]


def _engine_find_next_include_directive(p: str, start: int) -> int:
    INCLUDE = "#include"
    i = start
    n = len(p)
    while i < n:
        c = p[i]
        nxt = p[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and p[i] != "\n":
                i += 1
        elif c == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (p[i] == "*" and p[i + 1] == "/"):
                i += 1
            if i < n:
                i += 2
        elif c == '"' or c == "'":
            q = c
            i += 1
            while i < n and p[i] != q:
                if p[i] == "\\" and i + 1 < n:
                    i += 2
                else:
                    i += 1
            if i < n:
                i += 1
        elif c == "#" and p.startswith(INCLUDE, i):
            return i
        else:
            i += 1
    return -1


def _engine_parse_include(s: str, start: int):
    n = len(s)
    begin = s.find("<", start)
    end = s.find(">", start)
    eol = s.find("\n", start)
    if begin < 0 or end < 0:
        return None
    if eol >= 0 and end > eol:
        return None
    if end - begin <= 1:
        return None
    begin += 1
    while begin < end and s[begin] == " ":
        begin += 1
    while end > begin and s[end - 1] == " ":
        end -= 1
    name = s[begin:end]
    ieol = eol + 1 if eol >= 0 else n
    return name, ieol


def _engine_get_num_lines(text: str) -> int:
    if not text:
        return 0
    return 1 + text.count("\n")


def parse_includes_engine_style(source_text: str, source_path: str,
                                _visited: set | None = None) -> str:
    """Port of GameOS/gameos/utils/shader_builder.cpp:262 parse_includes."""
    if _visited is None:
        _visited = set()
    norm_self = os.path.normcase(os.path.normpath(source_path))
    if norm_self in _visited:
        raise ValueError(f"include cycle detected at {source_path}")
    _visited = _visited | {norm_self}

    base_path = _engine_get_path(source_path)
    out_parts: list[str] = []
    current_line = 1
    start = 0
    n = len(source_text)
    INCLUDE_LEN = len("#include")

    while True:
        tok = _engine_find_next_include_directive(source_text, start)
        if tok < 0:
            break
        code = source_text[start:tok]
        out_parts.append(f"#line {current_line} // {source_path}\n")
        out_parts.append(code)
        current_line += _engine_get_num_lines(code)
        parsed = _engine_parse_include(source_text, tok + INCLUDE_LEN)
        if parsed is None:
            raise ValueError(
                f"malformed #include in {source_path} near offset {tok}"
            )
        inc_name, ieol = parsed
        if base_path:
            include_path = base_path + ENGINE_PATH_SEPARATOR + inc_name
        else:
            include_path = inc_name
        try:
            included_src = Path(include_path).read_text(
                encoding="utf-8", errors="replace"
            )
        except OSError as e:
            raise ValueError(
                f"cannot open include {include_path} (from {source_path}): {e}"
            )
        inlined = parse_includes_engine_style(
            included_src, include_path, _visited=_visited
        )
        out_parts.append(inlined)
        if ieol >= n:
            start = n
            break
        start = ieol

    if start < n:
        out_parts.append(f"#line {current_line} // {source_path}\n")
        out_parts.append(source_text[start:])

    return "".join(out_parts)


def find_tool(name: str) -> str | None:
    """Locate a Vulkan SDK tool. Prefer $VULKAN_SDK/Bin, then PATH."""
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        candidate = Path(sdk) / "Bin" / (
            name + (".exe" if os.name == "nt" else "")
        )
        if candidate.exists():
            return str(candidate)
    return shutil.which(name)


def discover_shaders() -> list[Path]:
    """Return all shader files in shaders/, sorted by name within each extension group."""
    out: list[Path] = []
    for ext in STAGE_BY_EXT:
        out.extend(sorted(SHADERS.glob(f"*{ext}")))
    return out


def build_shader_source(
    shader_path: Path, extra_defines: list[str] | None = None
) -> str:
    """Assemble full shader source as the engine would compile it.

    Reads shader_path, expands #include directives using engine semantics
    (parse_includes_engine_style), prepends the version prefix (or per-shader
    override), injects extra_defines as #define lines, and applies per-shader
    token rewrites.

    extra_defines: list of "NAME=VALUE" or "NAME" strings. Each becomes a
    #define line injected between the version prefix and the body, matching
    the engine's runtime prefix-string composition for variant programs.

    Raises OSError if the file cannot be read.
    Raises ValueError if an #include cannot be resolved or forms a cycle.
    """
    if extra_defines is None:
        extra_defines = []

    src_text = shader_path.read_text(encoding="utf-8", errors="replace")
    flat_text = parse_includes_engine_style(src_text, str(shader_path))

    prefix = SHADER_PREFIX_OVERRIDE.get(shader_path.name, VERSION_PREFIX)
    parts: list[str] = [prefix]

    if _INCLUDE_RE.search(flat_text):
        parts.append(INCLUDE_EXTENSION)

    for d in extra_defines:
        if "=" in d:
            name, val = d.split("=", 1)
            parts.append(f"#define {name} {val}\n")
        else:
            parts.append(f"#define {d}\n")

    parts.append(flat_text)
    src = "".join(parts)

    for old_tok, new_tok in SHADER_TOKEN_REWRITES.get(shader_path.name, ()):
        src = src.replace(old_tok, new_tok)

    return src
```

---

## Task 2: Update `validate_shaders.py` to import `shader_common`

**Files:**
- Modify: `scripts/validate_shaders.py`

- [ ] **Step 2.1: Replace extracted symbols with imports**

Open `scripts/validate_shaders.py`. Make these changes:

**Remove** the following blocks (they now live in `shader_common.py`):
- The `ROOT`, `SHADERS`, `INCLUDE` constants
- `VERSION_PREFIX`, `INCLUDE_EXTENSION`, `_INCLUDE_RE`, `ENGINE_PATH_SEPARATOR`
- `_engine_get_path`, `_engine_find_next_include_directive`, `_engine_parse_include`, `_engine_get_num_lines`, `parse_includes_engine_style`
- `SHADER_PREFIX_OVERRIDE`, `SHADER_TOKEN_REWRITES`, `SKIP_SHADERS`, `STAGE_BY_EXT`
- `_find_tool` and `discover_shaders` (moved to shader_common)

**Keep** `_resolve_tools` in `validate_shaders.py` — it is validate-specific — but rewrite
it to call `shader_common.find_tool` instead of the removed `_find_tool`.

**Add** at the top (after `from __future__ import annotations` and stdlib imports):

```python
import sys
from pathlib import Path
# ensure scripts/ dir is importable when invoked as `py -3 scripts/validate_shaders.py`
sys.path.insert(0, str(Path(__file__).parent))
import shader_common

ROOT = shader_common.ROOT
SHADERS = shader_common.SHADERS
```

**Replace** `_resolve_tools` (keep it in validate_shaders.py since it's validate-specific):

```python
def _resolve_tools() -> tuple[str, str] | None:
    """Return (glslang, spirv_val) absolute paths, or None if missing."""
    glslang = shader_common.find_tool("glslangValidator")
    spirv_val = shader_common.find_tool("spirv-val")
    if not glslang or not spirv_val:
        return None
    return glslang, spirv_val
```

**Replace** `discover_shaders()` call sites with `shader_common.discover_shaders()`.

**Replace** the body of `validate_one` where it builds `src`:

Old (three blocks: read file, parse_includes, build src):
```python
try:
    src_text = shader.read_text(encoding="utf-8", errors="replace")
except OSError as e:
    return False, f"--- {shader.relative_to(ROOT)} (read) ---\ncannot read shader: {e}"
try:
    flat_text = parse_includes_engine_style(src_text, str(shader))
except ValueError as e:
    return False, f"--- {shader.relative_to(ROOT)} (include) ---\n{e}"
prefix = SHADER_PREFIX_OVERRIDE.get(shader.name, VERSION_PREFIX)
if _INCLUDE_RE.search(flat_text):
    src = prefix + INCLUDE_EXTENSION + flat_text
else:
    src = prefix + flat_text
for old_tok, new_tok in SHADER_TOKEN_REWRITES.get(shader.name, ()):
    src = src.replace(old_tok, new_tok)
```

New (single call):
```python
try:
    src = shader_common.build_shader_source(shader)
except OSError as e:
    return False, f"--- {shader.relative_to(ROOT)} (read) ---\ncannot read shader: {e}"
except ValueError as e:
    return False, f"--- {shader.relative_to(ROOT)} (include) ---\n{e}"
```

Also replace `STAGE_BY_EXT[shader.suffix]` with `shader_common.STAGE_BY_EXT[shader.suffix]`.
Replace `SKIP_SHADERS.get(sh.name)` with `shader_common.SKIP_SHADERS.get(sh.name)`.

- [ ] **Step 2.2: Run validate_shaders.py after the edit and diff against Task 0 baseline**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
py -3 scripts/validate_shaders.py 2>&1 | Tee-Object after.txt
Compare-Object (Get-Content baseline.txt) (Get-Content after.txt)
```

Expected: `Compare-Object` prints nothing (no differences). If any lines differ,
the extraction changed behavior — fix before proceeding. `baseline.txt` was
captured in Task 0 before any edits.

- [ ] **Step 2.4: Commit Commit 1**

```powershell
git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add scripts/shader_common.py scripts/validate_shaders.py
git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m "refactor(shaders): extract shader_common.py from validate_shaders.py

Mechanical extraction only. build_shader_source() new helper bundles
include-expansion + prefix + token-rewrites. validate_shaders.py
output is byte-identical before/after (verified by diff baseline).

reflect.py (Tier 1.2) imports shader_common so both gates compile
the exact same source string the engine compiles.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

- [ ] **Step 2.5: Clean up temp baseline files**

```powershell
Remove-Item A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\baseline.txt,
            A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\after.txt
```

---

## Task 3: Create `tools/shader_reflect/reflect.py` — constants and helpers

**Files:**
- Create: `tools/shader_reflect/reflect.py`

- [ ] **Step 3.1: Create tools/shader_reflect/ directory and reflect.py skeleton**

Create `tools/shader_reflect/reflect.py` with this content:

```python
#!/usr/bin/env python3
"""tools/shader_reflect/reflect.py — Shader contract reflection CI (Tier 1.2).

Compiles each shader+variant to SPIR-V via glslangValidator, runs
spirv-cross --reflect, normalizes the output, and compares against
expected/<golden_key>.json files.

Usage:
  py -3 tools/shader_reflect/reflect.py            # compare vs goldens
  py -3 tools/shader_reflect/reflect.py --update   # regenerate goldens
  py -3 tools/shader_reflect/reflect.py --shader shaders/static_prop.frag

Exit 0 = all PASS. Exit 1 = DRIFT / NEW / COMPILE_ERROR / CONTRACT_VIOLATION.
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# shader_common lives in scripts/; add it to path relative to repo root.
_REFLECT_DIR = Path(__file__).resolve().parent
ROOT = _REFLECT_DIR.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import shader_common

EXPECTED_DIR = _REFLECT_DIR / "expected"

# ---------------------------------------------------------------------------
# SHADER_VARIANTS
# Key: repo-relative posix path. All other shaders get [("default", [])].
# When adding a new #ifdef MC2_* to a shader, add an entry here.
# ---------------------------------------------------------------------------
SHADER_VARIANTS: dict[str, list[tuple[str, list[str]]]] = {
    "shaders/static_prop.frag": [
        ("default",           []),
        ("coalesce",          ["MC2_COALESCE=1"]),
        ("objectid",          ["MC2_OBJECT_ID_BUFFER=1"]),
        ("coalesce_objectid", ["MC2_COALESCE=1", "MC2_OBJECT_ID_BUFFER=1"]),
    ],
    "shaders/static_prop.vert": [
        ("default",           []),
        ("coalesce",          ["MC2_COALESCE=1"]),
        ("objectid",          ["MC2_OBJECT_ID_BUFFER=1"]),
        ("coalesce_objectid", ["MC2_COALESCE=1", "MC2_OBJECT_ID_BUFFER=1"]),
    ],
    "shaders/mech.frag": [
        ("default",  []),
        ("objectid", ["MC2_OBJECT_ID_BUFFER=1"]),
    ],
    "shaders/mech.vert": [
        ("default",  []),
        ("objectid", ["MC2_OBJECT_ID_BUFFER=1"]),
    ],
    "shaders/shadow_static_prop.vert": [
        ("default",  []),
        ("coalesce", ["MC2_COALESCE=1"]),
    ],
}

# Macros that gate variant-specific bindings. Used for coverage audit.
KNOWN_VARIANT_MACROS: frozenset[str] = frozenset(
    {"MC2_COALESCE", "MC2_OBJECT_ID_BUFFER"}
)

# ---------------------------------------------------------------------------
# REQUIRED_INVARIANTS — checked after golden comparison.
# CONTRACT_VIOLATION exits 1 even when --update is passed.
# To change a value here, update the spec and this table in the same commit.
# ---------------------------------------------------------------------------
REQUIRED_INVARIANTS: list[dict] = [
    # objectId MRT: static_prop.frag must write v_objectId at location=2.
    # M2: check BOTH object-ID variants (objectid AND coalesce_objectid).
    {
        "shader": "shaders/static_prop.frag",
        "variant": "objectid",
        "check": "output",
        "name": "v_objectId",
        "location": 2,
        "type": "uint",
    },
    {
        "shader": "shaders/static_prop.frag",
        "variant": "coalesce_objectid",
        "check": "output",
        "name": "v_objectId",
        "location": 2,
        "type": "uint",
    },
    # GBuffer1 normal: static_prop variants must write GBuffer1 at location=1.
    {
        "shader": "shaders/static_prop.frag",
        "variant": "default",
        "check": "output",
        "name": "GBuffer1",
        "location": 1,
        "type": "vec4",
    },
    # GBuffer1 normal: mech.frag must write GBuffer1 at location=1.
    {
        "shader": "shaders/mech.frag",
        "variant": "default",
        "check": "output",
        "name": "GBuffer1",
        "location": 1,
        "type": "vec4",
    },
    # objectIdRaw offset: PerDrawData.objectIdRaw must be at offset 24.
    # Value confirmed by --update bootstrap run 2026-05-23.
    {
        "shader": "shaders/static_prop.frag",
        "variant": "coalesce_objectid",
        "check": "ssbo_member",
        "block": "PerDrawData",
        "member": "objectIdRaw",
        "offset": 24,
    },
]


def golden_key(shader_rel: str, variant: str) -> str:
    """Collision-proof key: 'shaders/static_prop.frag' + 'default' ->
    'shaders__static_prop.frag__default'."""
    safe = shader_rel.replace("/", "__").replace("\\", "__")
    return f"{safe}__{variant}"


def golden_path(shader_rel: str, variant: str) -> Path:
    return EXPECTED_DIR / f"{golden_key(shader_rel, variant)}.json"


def _decode(data: bytes) -> str:
    return data.decode("utf-8", errors="replace")
```

---

## Task 4: Add compile and reflect pipeline to reflect.py

**Files:**
- Modify: `tools/shader_reflect/reflect.py` (append after Task 3 content)

- [ ] **Step 4.1: Add compile_to_spv and reflect_spv functions**

Append to `tools/shader_reflect/reflect.py`:

```python

def _find_tools() -> tuple[str, str] | None:
    """Return (glslang, spirv_cross) or None if either missing."""
    glslang = shader_common.find_tool("glslangValidator")
    spirv_cross = shader_common.find_tool("spirv-cross")
    if not glslang or not spirv_cross:
        return None
    return glslang, spirv_cross


def _print_tool_versions(glslang: str, spirv_cross: str) -> None:
    """Log tool versions to stdout (not included in goldens)."""
    for label, tool in (("glslangValidator", glslang), ("spirv-cross", spirv_cross)):
        try:
            r = subprocess.run([tool, "--version"], capture_output=True)
            ver = (_decode(r.stdout) + _decode(r.stderr)).strip().splitlines()
            print(f"  {label}: {ver[0] if ver else 'unknown'}")
        except OSError:
            print(f"  {label}: version check failed")


def compile_to_spv(
    shader: Path,
    extra_defines: list[str],
    glslang: str,
    vulkan: bool,
) -> tuple[bool, str, Path | None]:
    """Compile shader to SPIR-V. Returns (ok, diagnostic, spv_path_or_None).

    spv_path is a temp file the caller must delete. On failure, spv_path is None.
    reflect.py invokes glslangValidator only to obtain SPIR-V for spirv-cross.
    validate_shaders.py (Tier 1.1) remains the authoritative compile gate.
    """
    stage = shader_common.STAGE_BY_EXT[shader.suffix]
    try:
        src = shader_common.build_shader_source(shader, extra_defines)
    except OSError as e:
        return False, f"read error: {e}", None
    except ValueError as e:
        return False, f"include error: {e}", None

    fd, tmp_name = tempfile.mkstemp(
        suffix=shader.suffix, prefix="_reflect_", dir=str(shader_common.SHADERS)
    )
    tmp = Path(tmp_name)
    spv_path = tmp.with_suffix(tmp.suffix + ".spv")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(src)

        cmd = [
            glslang,
            "-S", stage,
            "--auto-map-locations",
            "--auto-map-bindings",
        ]
        if vulkan:
            cmd += ["-V", "-R"]
        else:
            cmd += ["-G"]
        cmd += [str(tmp), "-o", str(spv_path)]

        proc = subprocess.run(cmd, capture_output=True)
        if proc.returncode != 0:
            diag = (_decode(proc.stdout) + _decode(proc.stderr)).strip()
            diag = diag.replace(tmp.name, shader.name).replace(str(tmp), str(shader))
            # M4: clean up any partial .spv glslangValidator may have written.
            try:
                spv_path.unlink()
            except OSError:
                pass
            return False, diag, None
        return True, "", spv_path
    except Exception as e:
        try:
            spv_path.unlink()
        except OSError:
            pass
        return False, str(e), None
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass


def reflect_spv(spv_path: Path, spirv_cross: str) -> tuple[bool, str, dict]:
    """Run spirv-cross --reflect. Returns (ok, diagnostic, raw_json_dict)."""
    try:
        # C2: input file first, then --reflect flag (standard spirv-cross CLI shape).
        proc = subprocess.run(
            [spirv_cross, str(spv_path), "--reflect"],
            capture_output=True,
        )
    except OSError as e:
        return False, f"spirv-cross exec error: {e}", {}

    if proc.returncode != 0:
        diag = (_decode(proc.stdout) + _decode(proc.stderr)).strip()
        return False, diag, {}

    raw_text = _decode(proc.stdout)
    try:
        raw = json.loads(raw_text)
        return True, "", raw
    except json.JSONDecodeError as e:
        return False, f"spirv-cross JSON parse error: {e}\n{raw_text[:400]}", {}
```

---

## Task 5: Add normalize() to reflect.py

**Files:**
- Modify: `tools/shader_reflect/reflect.py` (append)

- [ ] **Step 5.1: Add normalization helpers**

Append to `tools/shader_reflect/reflect.py`:

```python

def _norm_type(t: str) -> str:
    """Normalize spirv-cross type aliases to canonical form."""
    return {"mat4x4": "mat4", "mat3x3": "mat3", "mat2x2": "mat2"}.get(t, t)


def _norm_stride(v) -> int | None:
    """spirv-cross uses 0 for 'not applicable'; map to null."""
    if v is None or v == 0:
        return None
    return int(v)


def _norm_member(m: dict) -> dict:
    return {
        "array_stride": _norm_stride(m.get("array_stride")),
        "matrix_stride": _norm_stride(m.get("matrix_stride")),
        "name": m["name"],
        "offset": int(m["offset"]),
        "type": _norm_type(m.get("type", "unknown")),
    }


def _norm_block(raw: dict, b: dict) -> dict:
    # C1: spirv-cross --reflect does NOT inline members on UBO/SSBO resources.
    # Members live in raw["types"][str(type_id)]["members"].
    type_id = b.get("type")
    members_list: list[dict] = []
    if type_id is not None:
        type_info = raw.get("types", {}).get(str(type_id), {})
        members_list = type_info.get("members", [])
    members = sorted(
        [_norm_member(m) for m in members_list],
        key=lambda m: (m["offset"], m["name"]),
    )
    return {
        "binding": int(b["binding"]),
        "members": members,
        "name": b["name"],
    }


def _norm_output(o: dict) -> dict:
    return {
        "location": int(o["location"]),
        "name": o["name"],
        "type": _norm_type(o.get("type", "unknown")),
    }


def normalize(
    raw: dict,
    shader_rel: str,
    stage: str,
    variant: str,
    defines: list[str],
) -> dict:
    """Normalize a spirv-cross --reflect JSON dict to our stable schema.

    Sorts all collections so golden diffs are stable across SPIR-V changes
    that reorder internal IDs. sort_keys=True on JSON serialization handles
    the rest.
    """
    ubos = sorted(
        [_norm_block(raw, b) for b in raw.get("ubos", [])],
        key=lambda b: (b["binding"], b["name"]),
    )
    ssbos = sorted(
        [_norm_block(raw, b) for b in raw.get("ssbos", [])],
        key=lambda b: (b["binding"], b["name"]),
    )
    outputs: list[dict] = []
    if stage == "frag":
        outputs = sorted(
            [_norm_output(o) for o in raw.get("outputs", [])],
            key=lambda o: (o["location"], o["name"]),
        )
    return {
        "defines": list(defines),
        "outputs": outputs,
        "shader": shader_rel,
        "ssbos": ssbos,
        "stage": stage,
        "ubos": ubos,
        "variant": variant,
    }


def serialize_contract(contract: dict) -> str:
    """Canonical serialization: sort_keys, 2-space indent, trailing newline."""
    return json.dumps(contract, indent=2, sort_keys=True) + "\n"


def diff_contracts(expected: dict, actual: dict) -> list[str]:
    exp_str = serialize_contract(expected)
    act_str = serialize_contract(actual)
    if exp_str == act_str:
        return []
    return list(
        difflib.unified_diff(
            exp_str.splitlines(keepends=True),
            act_str.splitlines(keepends=True),
            fromfile="expected",
            tofile="actual",
        )
    )
```

---

## Task 6: Add golden compare / update to reflect.py

**Files:**
- Modify: `tools/shader_reflect/reflect.py` (append)

- [ ] **Step 6.1: Add load_golden, save_golden, and check_variant_coverage**

Append to `tools/shader_reflect/reflect.py`:

```python

def load_golden(shader_rel: str, variant: str) -> dict | None:
    p = golden_path(shader_rel, variant)
    if not p.exists():
        return None
    return json.loads(p.read_text(encoding="utf-8"))


def save_golden(shader_rel: str, variant: str, contract: dict) -> None:
    p = golden_path(shader_rel, variant)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(serialize_contract(contract), encoding="utf-8")


def check_variant_coverage(
    shader_rel: str, shader: Path, strict: bool
) -> list[str]:
    """Warn if shader uses KNOWN_VARIANT_MACROS but has no SHADER_VARIANTS entry."""
    if shader_rel in SHADER_VARIANTS:
        return []
    try:
        # M3: use include-expanded source so macros in headers are caught.
        src = shader_common.build_shader_source(shader)
    except (OSError, ValueError):
        return []
    # Strip line comments, then look only inside preprocessor directive lines.
    src_no_comments = re.sub(r"//[^\n]*", "", src)
    found = {
        m for m in KNOWN_VARIANT_MACROS
        if re.search(
            rf"^\s*#\s*(?:if|ifdef|ifndef|elif)\b[^\n]*\b{re.escape(m)}\b",
            src_no_comments,
            re.MULTILINE,
        )
    }
    if not found:
        return []
    prefix = "FAIL" if strict else "WARNING"
    return [
        f"{prefix}: {shader_rel} uses {m} but has no SHADER_VARIANTS entry. "
        f"Add it or this variant's bindings will not be reflected."
        for m in sorted(found)
    ]
```

---

## Task 7: Add invariant checks to reflect.py

**Files:**
- Modify: `tools/shader_reflect/reflect.py` (append)

- [ ] **Step 7.1: Add check_invariants**

Append to `tools/shader_reflect/reflect.py`:

```python

def check_invariants(
    reflected: dict[str, dict],
) -> list[str]:
    """Check REQUIRED_INVARIANTS against reflected contracts.

    reflected: {f"{shader_rel}/{variant}": contract_dict}

    Returns list of CONTRACT_VIOLATION messages. An empty list = all pass.
    Invariant failures are NOT bypassable by --update.
    """
    violations: list[str] = []

    for inv in REQUIRED_INVARIANTS:
        shader_rel = inv["shader"]
        variant = inv["variant"]
        key = f"{shader_rel}/{variant}"
        contract = reflected.get(key)

        if contract is None:
            violations.append(
                f"CONTRACT_VIOLATION: {key} not in reflected results "
                f"(shader not compiled or skipped)"
            )
            continue

        check = inv["check"]

        if check == "output":
            matches = [
                o for o in contract.get("outputs", [])
                if o["name"] == inv["name"]
            ]
            if not matches:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: output '{inv['name']}' not found"
                )
                continue
            o = matches[0]
            if o["location"] != inv["location"]:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: output '{inv['name']}' "
                    f"location={o['location']}, expected {inv['location']}"
                )
            if o["type"] != inv["type"]:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: output '{inv['name']}' "
                    f"type={o['type']!r}, expected {inv['type']!r}"
                )

        elif check == "ssbo_member":
            ssbos = [s for s in contract.get("ssbos", []) if s["name"] == inv["block"]]
            if not ssbos:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: ssbo '{inv['block']}' not found"
                )
                continue
            members = [m for m in ssbos[0]["members"] if m["name"] == inv["member"]]
            if not members:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: "
                    f"ssbo member '{inv['block']}.{inv['member']}' not found"
                )
                continue
            m = members[0]
            if m["offset"] != inv["offset"]:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: "
                    f"ssbo member '{inv['block']}.{inv['member']}' "
                    f"offset={m['offset']}, expected {inv['offset']}"
                )

    return violations
```

---

## Task 8: Add main() and full CLI to reflect.py

**Files:**
- Modify: `tools/shader_reflect/reflect.py` (append)

- [ ] **Step 8.1: Add main()**

Append to `tools/shader_reflect/reflect.py`:

```python

# Result tags (printed per shader/variant).
_TAG_PASS    = "PASS"
_TAG_DRIFT   = "DRIFT"
_TAG_NEW     = "NEW"
_TAG_SKIP    = "SKIP"
_TAG_COMPILE = "COMPILE_ERROR"
_TAG_WARN    = "WARNING"
_TAG_FAIL    = "FAIL"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Shader contract reflection CI (Tier 1.2)."
    )
    ap.add_argument(
        "--update",
        action="store_true",
        help="Regenerate expected/*.json goldens. Rejected when $CI is set.",
    )
    ap.add_argument(
        "--shader",
        action="append",
        metavar="PATH",
        help="Reflect only this shader (repeatable). Implies all variants.",
    )
    ap.add_argument(
        "--variant",
        metavar="NAME",
        help="Restrict to this variant name (use with --shader).",
    )
    ap.add_argument(
        "--strict-variants",
        action="store_true",
        help="Promote variant-coverage WARNINGs to FAILs.",
    )
    ap.add_argument(
        "--gl",
        action="store_true",
        help="Use GL-semantics SPIR-V (-G) instead of Vulkan (-V -R).",
    )
    args = ap.parse_args()

    # CI guard: reject --update in CI environments.
    if args.update and os.environ.get("CI"):
        print(
            "reflect: --update is not allowed in CI environments. "
            "Run locally and commit the updated expected/*.json files.",
            file=sys.stderr,
        )
        return 1

    tools = _find_tools()
    if tools is None:
        sdk = os.environ.get("VULKAN_SDK", "<unset>")
        print(
            "reflect: cannot find glslangValidator and/or spirv-cross.\n"
            f"  $VULKAN_SDK = {sdk}\n"
            "  Install the Vulkan SDK and ensure $VULKAN_SDK/Bin/ is populated.",
            file=sys.stderr,
        )
        return 1
    glslang, spirv_cross = tools

    print("reflect: tool versions:")
    _print_tool_versions(glslang, spirv_cross)

    vulkan = not args.gl

    # Build target list.
    if args.shader:
        targets = [Path(s).resolve() for s in args.shader]
        for t in targets:
            if t.suffix not in shader_common.STAGE_BY_EXT:
                print(f"reflect: unknown stage for {t}", file=sys.stderr)
                return 1
    else:
        targets = shader_common.discover_shaders()

    passed = skipped = compile_errors = drifted = new_goldens = warnings = 0
    all_diffs: list[str] = []
    # reflected: keyed by "shader_rel/variant" -> normalized contract
    reflected: dict[str, dict] = {}
    # C4: collect --update writes; only commit to disk after invariant checks pass.
    pending_updates: list[tuple[str, str, dict]] = []

    for shader in targets:
        shader_rel = shader.relative_to(ROOT).as_posix()
        skip_reason = shader_common.SKIP_SHADERS.get(shader.name)
        if skip_reason:
            skipped += 1
            print(f"[{_TAG_SKIP}] {shader_rel}: {skip_reason}")
            continue

        # Variant coverage audit.
        coverage_msgs = check_variant_coverage(shader_rel, shader, args.strict_variants)
        for msg in coverage_msgs:
            print(msg)
            if msg.startswith("FAIL"):
                drifted += 1
            else:
                warnings += 1

        variants = SHADER_VARIANTS.get(shader_rel, [("default", [])])
        if args.variant:
            variants = [(n, d) for n, d in variants if n == args.variant]
            if not variants:
                print(
                    f"reflect: variant '{args.variant}' not found for {shader_rel}",
                    file=sys.stderr,
                )
                return 1

        for variant_name, extra_defines in variants:
            label = f"{shader_rel} [{variant_name}]"

            ok, diag, spv_path = compile_to_spv(
                shader, extra_defines, glslang, vulkan
            )
            if not ok:
                compile_errors += 1
                print(f"[{_TAG_COMPILE}] {label}")
                print(f"  {diag.splitlines()[0] if diag else '(no diagnostic)'}")
                all_diffs.append(f"--- {label} (COMPILE_ERROR) ---\n{diag}\n")
                continue

            assert spv_path is not None
            try:
                ok2, diag2, raw = reflect_spv(spv_path, spirv_cross)
            finally:
                try:
                    spv_path.unlink()
                except OSError:
                    pass

            if not ok2:
                compile_errors += 1
                print(f"[{_TAG_COMPILE}] {label}: spirv-cross failed")
                all_diffs.append(f"--- {label} (REFLECT_ERROR) ---\n{diag2}\n")
                continue

            stage = shader_common.STAGE_BY_EXT[shader.suffix]
            contract = normalize(raw, shader_rel, stage, variant_name, extra_defines)
            reflected[f"{shader_rel}/{variant_name}"] = contract

            if args.update:
                # C4: stash; write only after invariant checks pass.
                pending_updates.append((shader_rel, variant_name, contract))
                print(f"[UPDATE] {label}")
                passed += 1
                continue

            expected = load_golden(shader_rel, variant_name)
            if expected is None:
                new_goldens += 1
                print(
                    f"[{_TAG_NEW}] {label}: no golden found. "
                    f"Run --update to bootstrap."
                )
                continue

            diff = diff_contracts(expected, contract)
            if diff:
                drifted += 1
                first = next(
                    (l.strip() for l in diff if l.startswith(("+", "-"))
                     and not l.startswith(("---", "+++")))
                    , "(see diff)"
                )
                print(f"[{_TAG_DRIFT}] {label}: {first}")
                all_diffs.append(
                    f"--- {label} (DRIFT) ---\n" + "".join(diff) + "\n"
                )
            else:
                passed += 1
                print(f"[{_TAG_PASS}] {label}")

    # Invariant checks — run even on --update to catch bypasses.
    violations = check_invariants(reflected)
    for v in violations:
        print(v, file=sys.stderr)

    # C4: write goldens only when --update AND no invariant violations.
    # If violations exist, goldens are NOT written — bad values never land on disk.
    if args.update:
        if violations:
            print(
                f"\nreflect: {len(violations)} invariant violation(s) detected — "
                "goldens NOT written. Fix CONTRACT_VIOLATION(s) above before "
                "updating goldens.",
                file=sys.stderr,
            )
        else:
            for sr, vn, ct in pending_updates:
                save_golden(sr, vn, ct)

    total = passed + skipped + compile_errors + drifted + new_goldens
    mode = "Vulkan" if vulkan else "GL"
    action = "updated" if args.update else "checked"
    print(
        f"\nreflect: {passed} {action}, {drifted} drifted, "
        f"{new_goldens} new, {compile_errors} compile errors, "
        f"{skipped} skipped, {warnings} warnings [{mode}]"
    )

    if all_diffs and not args.update:
        print("\n" + "=" * 72)
        for d in all_diffs:
            print(d)

    if violations:
        print(f"\n{len(violations)} invariant violation(s) — see CONTRACT_VIOLATION above.")
        return 1
    if compile_errors or drifted or new_goldens:
        return 1
    if args.strict_variants and warnings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

---

## Task 9: Bootstrap goldens with --update and verify

**Files:**
- Create: `tools/shader_reflect/expected/*.json` (generated by --update)

- [ ] **Step 9.1: Run --update to bootstrap all goldens**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
py -3 tools/shader_reflect/reflect.py --update 2>&1
```

Expected output ends with:
```
reflect: N updated, 0 drifted, 0 new, 0 compile errors, 1 skipped, 0 warnings [Vulkan]
```

If any `[COMPILE_ERROR]` lines appear, those shaders failed glslangValidator.
Fix the source issue before proceeding (this should not happen on a clean tree).

- [ ] **Step 9.2: Verify objectIdRaw invariant offset**

```powershell
cat tools/shader_reflect/expected/shaders__static_prop.frag__coalesce_objectid.json |
    python -c "import sys,json; d=json.load(sys.stdin); [print(m) for s in d['ssbos'] for m in s['members'] if m['name']=='objectIdRaw']"
```

Expected output includes `"offset": 24`. The value 24 comes from the M1.5 spec
(`PerDrawEntry` struct in `gos_static_prop_batcher.h`: `objectIdRaw` field at
byte 24 per std430 layout — confirmed by recon on 2026-05-23 HEAD).

**If the actual offset differs from 24: STOP. Do not update REQUIRED_INVARIANTS.**
The invariant tracks the C++/GLSL contract. An unexpected offset means either:
(a) the GLSL `PerDrawData` struct drifted from the C++ `PerDrawEntry` definition,
or (b) the bootstrap compiled the wrong variant (check `MC2_OBJECT_ID_BUFFER=1`
is present). Investigate `gos_static_prop_batcher.h` and the GLSL struct before
changing the hardcoded offset — updating the invariant to match a wrong value
defeats the purpose of having the invariant.

- [ ] **Step 9.3: Inspect the golden count and spot-check two files**

```powershell
(Get-ChildItem tools/shader_reflect/expected/*.json).Count
```

Expected: roughly 40-60 JSON files (one per shader x variant). The exact count
depends on how many shaders compile cleanly.

Spot-check `static_prop.frag` coalesce_objectid golden:
```powershell
cat tools/shader_reflect/expected/shaders__static_prop.frag__coalesce_objectid.json
```

Verify it contains `"v_objectId"` at `"location": 2` and `"GBuffer1"` at `"location": 1`.

Spot-check `gpu_cull.comp` default golden:
```powershell
cat tools/shader_reflect/expected/shaders__gpu_cull.comp__default.json
```

Verify it contains `"CullUBO"` with `"binding": 2` in `"ubos"`.

- [ ] **Step 9.4: Run reflect.py without --update — must exit 0**

```powershell
py -3 tools/shader_reflect/reflect.py
echo "Exit code: $LASTEXITCODE"
```

Expected: all `[PASS]`, exit code 0.

- [ ] **Step 9.5: Commit Commit 2**

```powershell
git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add tools/
git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m "feat(ci): add shader reflection CI (Tier 1.2)

tools/shader_reflect/reflect.py: compiles each shader variant to SPIR-V
via glslangValidator, runs spirv-cross --reflect, normalizes to stable
JSON schema, compares against expected/*.json goldens.

SHADER_VARIANTS covers static_prop.frag/vert (x4), mech.frag/vert (x2),
shadow_static_prop.vert (x2). All other shaders get a default variant.

REQUIRED_INVARIANTS enforce v_objectId@location=2, GBuffer1@location=1,
PerDrawData.objectIdRaw@offset=N independently of goldens — CONTRACT_VIOLATION
exits 1 even during --update.

--update bootstrapped N goldens on 2026-05-23 from clean nifty-mendeleev HEAD.
--update rejected in CI ($CI env var). Tier 1.1 (validate_shaders.py)
ownership unchanged; reflect.py reports COMPILE_ERROR as precondition failure.

Closes drift-class: C++/GLSL binding numbers, output locations, and
UBO/SSBO member offsets now have CI coverage.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 10: Mutation verification and CMake target

**Files:**
- Modify: `shaders/static_prop.frag` (temporary — revert after test)
- Modify: `CMakeLists.txt` (append custom target)

- [ ] **Step 10.1: Mutation test — output location drift**

Temporarily edit `shaders/static_prop.frag`: change `layout(location=2) out uint v_objectId`
to `layout(location=3) out uint v_objectId`. Then run:

```powershell
py -3 tools/shader_reflect/reflect.py --shader shaders/static_prop.frag
echo "Exit code: $LASTEXITCODE"
```

Expected:
```
[DRIFT] shaders/static_prop.frag [objectid]: ...
[DRIFT] shaders/static_prop.frag [coalesce_objectid]: ...
CONTRACT_VIOLATION: shaders/static_prop.frag/coalesce_objectid: output 'v_objectId' location=3, expected 2
...
Exit code: 1
```

**Revert** the change:
```powershell
git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev checkout -- shaders/static_prop.frag
```

- [ ] **Step 10.2: Mutation test — --update cannot bypass CONTRACT_VIOLATION**

This test proves that `--update` cannot bless a bad shader change into a passing invariant.

Edit `shaders/static_prop.frag`: change `layout(location=2) out uint v_objectId`
to `layout(location=3) out uint v_objectId`. Then run `--update`:

```powershell
# (after editing static_prop.frag to use location=3)
py -3 tools/shader_reflect/reflect.py --shader shaders/static_prop.frag --update
echo "Exit code: $LASTEXITCODE"
```

Expected: `[UPDATE]` lines print but **goldens are NOT written to disk**
because invariant checks run before the write (C4 all-or-nothing). Exit
code is **1**. Output must include:

```
CONTRACT_VIOLATION: shaders/static_prop.frag/coalesce_objectid: output 'v_objectId' location=3, expected 2
```

Revert the shader edit (goldens were NOT written by the bad run — C4 — so no restore needed):
```powershell
git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev checkout -- shaders/static_prop.frag
```

Verify existing golden still has location=2 (was never overwritten):
```powershell
python -c "import json; d=json.load(open('tools/shader_reflect/expected/shaders__static_prop.frag__coalesce_objectid.json')); print([o for o in d['outputs'] if o['name']=='v_objectId'])"
```

Expected: `[{'location': 2, 'name': 'v_objectId', 'type': 'uint'}]`

- [ ] **Step 10.3: Variant coverage audit test**

Add a temporary `#ifdef MC2_COALESCE` block to a shader that has no SHADER_VARIANTS entry,
e.g. `shaders/postprocess.frag`. Append one line at the end:

```glsl
// test-only: #ifdef MC2_COALESCE
```

Then run:
```powershell
py -3 tools/shader_reflect/reflect.py --shader shaders/postprocess.frag
echo "Exit code: $LASTEXITCODE"
```

Expected: output includes
```
WARNING: shaders/postprocess.frag uses MC2_COALESCE but has no SHADER_VARIANTS entry. ...
```
and exit code **0** (WARNING doesn't fail by default).

Then run with `--strict-variants`:
```powershell
py -3 tools/shader_reflect/reflect.py --shader shaders/postprocess.frag --strict-variants
echo "Exit code: $LASTEXITCODE"
```

Expected: `FAIL: shaders/postprocess.frag uses MC2_COALESCE...` and exit code **1**.

Revert the edit:
```powershell
git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev checkout -- shaders/postprocess.frag
```

- [ ] **Step 10.5: CI rejection test**

```powershell
$env:CI = "true"
py -3 tools/shader_reflect/reflect.py --update
echo "Exit code: $LASTEXITCODE"
Remove-Item Env:CI
```

Expected: `reflect: --update is not allowed in CI environments.` and exit code 1.

- [ ] **Step 10.6: Add CMake custom target**

Find the CMakeLists.txt at the root of the nifty worktree:
```powershell
ls A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\CMakeLists.txt
```

Append to CMakeLists.txt (before the final closing paren if any):

```cmake
# -----------------------------------------------------------------------
# Shader contract reflection CI (Tier 1.2)
# Depends on Vulkan SDK tools: glslangValidator, spirv-cross.
# Run: cmake --build build64 --target shader_reflect
# -----------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(shader_reflect
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/shader_reflect/reflect.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Shader contract reflection CI (Tier 1.2)"
        VERBATIM
    )
    if(TARGET shader_validate)
        add_dependencies(shader_reflect shader_validate)
    endif()
endif()
```

- [ ] **Step 10.7: Verify CMake target configures without error**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
  -S A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev `
  -B A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64 `
  --fresh 2>&1 | Select-String -Pattern "shader_reflect|error|Error" | head -20
```

Expected: no errors, `shader_reflect` target appears if `Python3` found.

- [ ] **Step 10.8: Final commit**

```powershell
git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add CMakeLists.txt
git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m "feat(ci): add shader_reflect CMake target

cmake --build build64 --target shader_reflect runs Tier 1.2 gate.
Depends on shader_validate target if present. Python3 guard keeps
configure from failing when Python is absent.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Exit criteria checklist (from spec §12)

- [ ] `validate_shaders.py` stdout byte-identical after Commit 1 (Step 2.3)
- [ ] `reflect.py --update` populates `expected/` without errors (Step 9.1)
- [ ] `reflect.py` exits 0 on clean tree (Step 9.4)
- [ ] Mutating `location=2` in `static_prop.frag` causes exit 1 with DRIFT + CONTRACT_VIOLATION (Step 10.1)
- [ ] CONTRACT_VIOLATION fires during `--update` when shader has wrong location (Step 10.2)
- [ ] Variant coverage audit: shader with `#ifdef MC2_COALESCE` but no SHADER_VARIANTS entry emits WARNING (Step 10.3)
- [ ] `--strict-variants` promotes WARNING to exit 1 (Step 10.3)
- [ ] `reflect.py --update` rejected when `$CI=true` (Step 10.5)
- [ ] CMake `shader_reflect` target configures without error (Step 10.7)
- [ ] `objectIdRaw` offset in golden matches REQUIRED_INVARIANTS (Step 9.2)
