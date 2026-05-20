#!/usr/bin/env python3
# scripts/validate_shaders.py
"""MC2 shader validator -- Tier 1.1 of docs/testing-strategy.md.

Walks shaders/*.{vert,frag,tesc,tese,comp,geom}, prepends the canonical
"#version 430" header (matching makeProgram() discipline -- shader files
themselves never carry a #version directive), and runs:

    glslangValidator --auto-map-locations -V -I<shaders/include> -S <stage> <tmpfile>
    spirv-val <emitted.spv>

Both must exit 0 for the shader to pass. Failure prints the offending
shader + tool diagnostic and accumulates; exit code is 0 if all pass,
1 if any failed.

Validator-prep prefix (path (a), per testing-strategy):
  * If the shader source contains a `#include` directive, the temp file
    additionally gets `#extension GL_GOOGLE_include_directive : require`
    after the #version line. The engine's makeProgram() tolerates the
    missing extension; glslangValidator does not. This injection keeps
    the validator-side gate matched to what the engine actually compiles.
  * `--auto-map-locations` is passed in both Vulkan and GL modes so
    glslangValidator auto-assigns the layout(location=) qualifiers that
    Vulkan/SPIR-V requires but the engine does not (engine uses
    glGetUniformLocation at runtime; OpenGL auto-assigns input/output
    varyings). Vulkan-strict tightening (explicit layout qualifiers on
    every uniform / varying) is deferred to when Vulkan-prep is on the
    active roadmap.

Skipped shaders:
  * `shaders/gos_terrain_lighting.comp` is programmatically stitched at
    runtime by tl_build_terrain_lighting_program (its include of
    terrain_lighting_shared.hglsl is intentionally commented out in the
    file on disk). Standalone validation cannot reach this shader
    without replicating the engine's include stitching, so it is
    explicitly skipped and counted separately.

Why prepend #version: shaders in this tree are loaded via
GameOS/gameos/gameos_graphics.cpp `makeProgram()`, which concatenates a
"#version 430\n" prefix before compile. Validating the file as-written
would fail every shader for the wrong reason.

Why -I shaders/include: the include/ subdir holds shared .hglsl snippets
referenced via #include directives. glslangValidator resolves them with
-I<dir> (note: NO space between -I and the path).

Tool discovery: prefers $VULKAN_SDK/Bin/{glslangValidator,spirv-val} on
Windows. Falls back to PATH. If neither resolves, exits with a clear
"install Vulkan SDK" error -- never silently skips.

Pre-commit wiring (optional, manual): add a one-liner to
.git/hooks/pre-commit:

    exec py -3 scripts/validate_shaders.py || exit 1

Modes:
    --vulkan   default; emits SPIR-V + runs spirv-val (Vulkan-prep gate)
    --gl       GL-semantics only, no SPIR-V emission
    --shader X validate only X (repeatable)

Exit code: 0 = all passed; 1 = any failed or environment broken.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "shaders"
INCLUDE = SHADERS / "include"

# Mirror of makeProgram() prefix in GameOS/gameos/gameos_graphics.cpp.
# If the prefix there ever changes (e.g. 4.4 / 4.5), this must follow.
VERSION_PREFIX = "#version 430\n"

# Path-(a) validator-prep: only injected when the source actually uses
# `#include`. Kept as a harmless safety net even though the engine-style
# Python pre-processor below flattens out every `#include` it understands
# before glslangValidator sees the source. If a future shader-include
# pattern slips past parse_includes_engine_style, the extension keeps
# glslang from rejecting the literal directive.
INCLUDE_EXTENSION = "#extension GL_GOOGLE_include_directive : require\n"
_INCLUDE_RE = re.compile(r"(?m)^\s*#\s*include\b")

# Engine path separator. The engine builds with -DLINUX_BUILD globally
# (see GameOS/gameos/utils/file_utils.cpp: kPathSeparator = "/"), so the
# port uses "/" too. Hardcoded — do NOT swap to os.sep.
ENGINE_PATH_SEPARATOR = "/"


def _engine_get_path(fname: str) -> str:
    """Port of GameOS/gameos/utils/file_utils.cpp:32 get_path. Returns dirname
    by stripping after the LAST kPathSeparator. Empty string if no separator.
    Engine's separator is "/" under LINUX_BUILD.
    """
    # Engine uses strrchr on kPathSeparatorAsChar ("/"). Accept both "/" and
    # "\\" defensively since Python on Windows may surface either when the
    # caller passed a native path; engine itself only sees "/".
    idx = max(fname.rfind("/"), fname.rfind("\\"))
    if idx < 0:
        return ""
    return fname[:idx]


def _engine_find_next_include_directive(p: str, start: int) -> int:
    """Port of GameOS/gameos/utils/shader_builder.cpp:231 find_next_include_directive.

    Returns index of next `#include` in `p` starting at `start`, skipping
    `//` line comments, `/* ... */` block comments, and `"..."` / `'...'`
    string literals. Returns -1 if none.
    """
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
    """Port of GameOS/gameos/utils/shader_builder.cpp:178 parse_include.

    Parses `<name>` directly after a `#include` token. Engine only supports
    angle-bracket form (uses strchr for '<' / '>'); quoted-string form is
    NOT handled. Returns (include_name, ieol_idx) on success, or None.
    `ieol_idx` is the index just past the newline ending the #include line
    (or len(s) if the directive was on the last line without trailing \n).
    """
    n = len(s)
    begin = s.find("<", start)
    end = s.find(">", start)
    eol = s.find("\n", start)
    if begin < 0 or end < 0:
        return None
    # If newline exists and `>` is past it, malformed (matches engine check).
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
    """Port of GameOS/gameos/utils/shader_builder.cpp:203 get_num_lines.
    Engine counts 1 + number of '\\n' chars.
    """
    if not text:
        return 0
    return 1 + text.count("\n")


def parse_includes_engine_style(source_text: str, source_path: str,
                                _visited: set | None = None) -> str:
    """Port of GameOS/gameos/utils/shader_builder.cpp:262 parse_includes.

    Engine semantics: base_path + path-separator + include-name; NO
    search-path resolution. Recursively inlines `#include <name>` directives
    (angle-bracket form only — engine's parse_include uses strchr('<','>')).
    Preserves line numbers via `#line N // fname` directives matching the
    engine's append_line_directive (shader_builder.cpp:219). Skips
    `#include` tokens inside // line comments, /* block comments */, and
    string literals (matches find_next_include_directive). Detects include
    cycles via a visited set and raises ValueError.

    If parse_includes evolves, update this port (the cited lines are the
    source of truth). Faithfulness over elegance: do NOT simplify.
    """
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

        # Append the pre-include code chunk with a #line directive.
        code = source_text[start:tok]
        out_parts.append(f"#line {current_line} // {source_path}\n")
        out_parts.append(code)
        current_line += _engine_get_num_lines(code)

        parsed = _engine_parse_include(source_text, tok + INCLUDE_LEN)
        if parsed is None:
            raise ValueError(f"malformed #include in {source_path} near offset {tok}")
        inc_name, ieol = parsed

        # Engine: base_path + kPathSeparator + inc_name (NO search paths).
        if base_path:
            include_path = base_path + ENGINE_PATH_SEPARATOR + inc_name
        else:
            include_path = inc_name

        try:
            included_src = Path(include_path).read_text(encoding="utf-8",
                                                        errors="replace")
        except OSError as e:
            raise ValueError(f"cannot open include {include_path} "
                             f"(from {source_path}): {e}")

        # Recurse — engine calls load_shader -> parse_includes.
        inlined = parse_includes_engine_style(included_src, include_path,
                                              _visited=_visited)
        out_parts.append(inlined)

        # Engine: `if(!start) break;` — if the include was on the final
        # line with no trailing data, we stop. _engine_parse_include sets
        # ieol = len(source_text) in that case; matching the engine, we
        # break when there's nothing left.
        if ieol >= n:
            start = n
            break
        start = ieol

    # Trailing tail after the last #include (or whole source if none).
    if start < n:
        out_parts.append(f"#line {current_line} // {source_path}\n")
        out_parts.append(source_text[start:])

    return "".join(out_parts)

# Per-shader prefix overrides. Some shaders are compiled by the engine with
# additional `#extension` / `#define` lines that follow `#version 430` in
# the makeProgram preamble; the shader file itself relies on those being
# present. Mirror them here so the validator sees the same prefix the
# engine compiles with. Keep this table in lockstep with the call sites
# in GameOS/gameos/gameos_graphics.cpp and gos_static_prop_batcher.cpp.
SHADER_PREFIX_OVERRIDE: dict[str, str] = {
    # gos_terrain_water_fast_mdi.vert -- gameos_graphics.cpp:2092 kWaterMdiPrefix.
    # Uses gl_DrawIDARB unconditionally (no #ifdef MC2_COALESCE guard).
    # Validator-only: bump to #version 460 so the core (unsuffixed) gl_DrawID
    # builtin is in scope; SHADER_TOKEN_REWRITES rewrites the ARB-suffixed
    # name in the source to the core builtin. glslang does not implement
    # gl_DrawIDARB even with GL_ARB_shader_draw_parameters; the driver runtime
    # accepts the ARB form. Engine still compiles the file with its own
    # 4.3 + ARB-ext preamble; this override only changes what the validator
    # sees.
    "gos_terrain_water_fast_mdi.vert": (
        "#version 460\n"
    ),
}

# Per-shader textual rewrites applied to the assembled source AFTER the
# prefix is prepended and BEFORE the temp file is written. Used to bridge
# tool-vs-engine gaps where glslangValidator does not implement a builtin
# the driver runtime accepts. Same shape as the parse_includes port: the
# engine source is unchanged; the validator compensates locally. Keep the
# scope of each rewrite as narrow as possible (one shader, one token).
SHADER_TOKEN_REWRITES: dict[str, list[tuple[str, str]]] = {
    # glslang does not implement ARB-suffixed gl_DrawIDARB even with
    # GL_ARB_shader_draw_parameters; driver runtime accepts. Validator-only
    # rewrite to the core unsuffixed builtin (paired with the per-shader
    # #version 460 override below; core gl_DrawID is core-4.6).
    # Engine source unchanged.
    "gos_terrain_water_fast_mdi.vert": [
        ("gl_DrawIDARB", "gl_DrawID"),
    ],
}

# Shaders that cannot be validated standalone because the engine
# programmatically stitches their source at runtime. See module docstring.
SKIP_SHADERS = {
    "gos_terrain_lighting.comp": (
        "programmatically stitched at runtime via "
        "tl_build_terrain_lighting_program; standalone validation cannot "
        "reach this shader without replicating the engine's include stitching"
    ),
}

# Map file extension to glslangValidator -S <stage> arg.
STAGE_BY_EXT = {
    ".vert": "vert",
    ".frag": "frag",
    ".tesc": "tesc",
    ".tese": "tese",
    ".geom": "geom",
    ".comp": "comp",
}


def _find_tool(name: str) -> str | None:
    """Locate a Vulkan SDK tool. Prefer $VULKAN_SDK/Bin, then PATH."""
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        candidate = Path(sdk) / "Bin" / (name + (".exe" if os.name == "nt" else ""))
        if candidate.exists():
            return str(candidate)
    # PATH fallback
    found = shutil.which(name)
    if found:
        return found
    return None


def _resolve_tools() -> tuple[str, str] | None:
    """Return (glslang, spirv_val) absolute paths, or None if missing."""
    glslang = _find_tool("glslangValidator")
    spirv_val = _find_tool("spirv-val")
    if not glslang or not spirv_val:
        return None
    return glslang, spirv_val


def discover_shaders() -> list[Path]:
    out: list[Path] = []
    for ext in STAGE_BY_EXT:
        out.extend(sorted(SHADERS.glob(f"*{ext}")))
    return out


def _decode(data: bytes | str) -> str:
    if isinstance(data, str):
        return data
    return data.decode("utf-8", errors="replace")


def _annotate(shader: Path, tmp: Path, stdout: str, stderr: str,
              phase: str = "glslang") -> str:
    """Format a diagnostic, rewriting tmp basename back to the real shader path."""
    body = (stdout + stderr).strip()
    body = body.replace(tmp.name, shader.name)
    body = body.replace(str(tmp), str(shader))
    rel = shader.relative_to(ROOT)
    return f"--- {rel} ({phase}) ---\n{body}"


def _first_error_line(diag: str) -> str:
    """Extract the first ERROR: line for terse summary."""
    for line in diag.splitlines():
        s = line.strip()
        if s.startswith("ERROR:") or "error" in s.lower():
            return s
    # fallback: first non-empty non-header line
    for line in diag.splitlines():
        s = line.strip()
        if s and not s.startswith("---"):
            return s
    return diag.strip()[:200]


def validate_one(shader: Path, vulkan: bool,
                 glslang: str, spirv_val: str) -> tuple[bool, str]:
    """Return (passed, diagnostic). diagnostic is empty on success."""
    stage = STAGE_BY_EXT[shader.suffix]
    try:
        src_text = shader.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        return False, f"--- {shader.relative_to(ROOT)} (read) ---\ncannot read shader: {e}"

    # Engine-style include pre-processor (option 1): flatten `#include`
    # directives the way GameOS/gameos/utils/shader_builder.cpp:262
    # parse_includes does, BEFORE handing the source to glslangValidator.
    # glslang's -I-based resolver uses search paths; the engine does not
    # (base_path + sep + name). Flattening here makes the validator see
    # the same source the engine compiles.
    try:
        flat_text = parse_includes_engine_style(src_text, str(shader))
    except ValueError as e:
        return False, f"--- {shader.relative_to(ROOT)} (include) ---\n{e}"

    # Pick the engine-matching prefix: a per-shader override (mirroring a
    # custom makeProgram preamble in C++) takes priority over the default.
    prefix = SHADER_PREFIX_OVERRIDE.get(shader.name, VERSION_PREFIX)

    # Keep the GL_GOOGLE_include_directive prefix as a harmless safety net
    # in case a future include pattern slips past the Python pre-processor
    # (the flattened text usually has no `#include` left at all).
    if _INCLUDE_RE.search(flat_text):
        src = prefix + INCLUDE_EXTENSION + flat_text
    else:
        src = prefix + flat_text

    # Per-shader textual rewrites (tool-vs-engine gap compensation). See
    # SHADER_TOKEN_REWRITES docstring above. Applied last so the rewrite
    # sees the final source the validator will compile.
    for old_tok, new_tok in SHADER_TOKEN_REWRITES.get(shader.name, ()):
        src = src.replace(old_tok, new_tok)

    # Write to a temp file in the shader dir so relative #include paths
    # (if any are not in -I) and error messages stay sensible. The dir
    # also makes -I<shaders/include> resolution natural.
    fd, tmp_name = tempfile.mkstemp(
        suffix=shader.suffix, prefix="_validate_", dir=str(SHADERS)
    )
    tmp = Path(tmp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(src)

        cmd = [
            glslang,
            "-S", stage,
            # NOTE: no -I flags. The Python pre-processor above
            # (parse_includes_engine_style) already flattens every
            # `#include` directive using engine semantics. glslang's
            # -I-based resolver uses search paths which the engine does
            # not, so feeding glslang the flattened source avoids the
            # divergence entirely.
            # Auto-assign layout(location=) on input/output varyings and
            # uniforms the engine does not declare explicitly. Engine
            # uses glGetUniformLocation at runtime; GL auto-assigns
            # varyings. Vulkan/SPIR-V demands them.
            "--auto-map-locations",
            # Auto-assign layout(binding=) on samplers/images. Engine
            # binds samplers via glUniform1i at runtime; Vulkan/SPIR-V
            # demands explicit bindings.
            "--auto-map-bindings",
        ]
        if vulkan:
            # -R = relaxed Vulkan rules: allow default uniforms (i.e.
            # `uniform mat4 mvp;` outside a block), atomic_uints, etc.
            # This matches what the engine compiles via makeProgram();
            # full Vulkan-strict (UBO blocks) is deferred to Vulkan-prep.
            cmd += ["-V", "-R"]
        else:
            # -G emits SPIR-V under OpenGL semantics (which permits the
            # same default-uniform style the engine relies on).
            cmd += ["-G"]
        cmd += [str(tmp)]

        # glslangValidator with -V or -G writes <input>.spv in CWD by
        # default; pass -o to keep it next to the temp file and easy to
        # clean. Both modes emit SPIR-V here (Vulkan via -V, GL via -G).
        spv_path = tmp.with_suffix(tmp.suffix + ".spv")
        cmd += ["-o", str(spv_path)]

        proc = subprocess.run(cmd, capture_output=True)
        stdout = _decode(proc.stdout)
        stderr = _decode(proc.stderr)
        if proc.returncode != 0:
            return False, _annotate(shader, tmp, stdout, stderr, phase="glslang")

        # Only spirv-val in Vulkan mode -- spirv-val targets the Vulkan
        # SPIR-V environment; GL-semantics SPIR-V can fail rules that
        # are GL-legal (and the engine doesn't consume the .spv anyway).
        if vulkan and spv_path.exists():
            v = subprocess.run([spirv_val, str(spv_path)], capture_output=True)
            v_stdout = _decode(v.stdout)
            v_stderr = _decode(v.stderr)
            if v.returncode != 0:
                return False, _annotate(shader, tmp, v_stdout, v_stderr,
                                        phase="spirv-val")

        return True, ""
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass
        try:
            spv_path.unlink()
        except (OSError, NameError, UnboundLocalError):
            pass


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Validate MC2 shaders with glslangValidator + spirv-val."
    )
    ap.add_argument("--shader", action="append",
                    help="Validate only this shader (repeatable). Default: all.")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--vulkan", action="store_true",
                      help="Emit SPIR-V + run spirv-val (default; Vulkan-prep).")
    mode.add_argument("--gl", action="store_true",
                      help="GL-semantics validation only (no SPIR-V).")
    args = ap.parse_args()

    tools = _resolve_tools()
    if tools is None:
        sdk = os.environ.get("VULKAN_SDK", "<unset>")
        print(
            "validate_shaders: cannot find glslangValidator and/or spirv-val.\n"
            f"  $VULKAN_SDK = {sdk}\n"
            "  Install the Vulkan SDK from https://vulkan.lunarg.com/ and ensure\n"
            "  $VULKAN_SDK/Bin/glslangValidator (+ spirv-val) exist, or place\n"
            "  them on PATH.",
            file=sys.stderr,
        )
        return 1
    glslang, spirv_val = tools

    if args.shader:
        targets = [Path(s).resolve() for s in args.shader]
        # Validate the user gave us recognised stages.
        for t in targets:
            if t.suffix not in STAGE_BY_EXT:
                print(f"validate_shaders: unknown shader stage for {t}",
                      file=sys.stderr)
                return 1
    else:
        targets = discover_shaders()

    if not targets:
        print("validate_shaders: no shaders found", file=sys.stderr)
        return 1

    vulkan = not args.gl   # default = Vulkan-prep mode
    passed = 0
    failed = 0
    skipped = 0
    failures: list[str] = []
    for sh in targets:
        rel = sh.relative_to(ROOT)
        # Skip shaders the engine stitches programmatically at runtime;
        # standalone validation cannot reach them. Counted separately.
        skip_reason = SKIP_SHADERS.get(sh.name)
        if skip_reason is not None:
            skipped += 1
            print(f"[SKIP] {rel}: {skip_reason}")
            continue
        ok, diag = validate_one(sh, vulkan=vulkan,
                                glslang=glslang, spirv_val=spirv_val)
        if ok:
            passed += 1
            print(f"[PASS] {rel}")
        else:
            failed += 1
            first = _first_error_line(diag)
            print(f"[FAIL] {rel}: {first}")
            failures.append(diag)

    total = passed + failed + skipped
    mode_label = "Vulkan/SPIR-V" if vulkan else "GL-only"
    print(f"\nvalidate_shaders: {passed} passed, {failed} failed, "
          f"{skipped} skipped (out of {total}) [{mode_label}]")

    if failures:
        print("\n" + "=" * 72)
        print("Full diagnostics:\n")
        for diag in failures:
            print(diag)
            print()
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
