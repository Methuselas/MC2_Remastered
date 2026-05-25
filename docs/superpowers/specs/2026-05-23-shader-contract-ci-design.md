# Shader Contract / Reflection CI — Design Spec

**Date:** 2026-05-23
**Status:** Approved (post-review C1/C2/C3/M1-M4 applied)
**Branch:** nifty-mendeleev
**Scope:** `scripts/shader_common.py`, `validate_shaders.py` (import refactor), `tools/shader_reflect/`

---

## 1. Problem

C++/GLSL binding drift is a silent failure class. When a C++ bind site uses
`glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, buf)` but the GLSL shader
declares `layout(binding=5)`, the driver silently reads stale data. No GL
error. No compile error. Symptoms are wrong rendering or black objects, not a
crash.

The M1.5 objectId buffer (`v_objectId` at location=2, `PerDrawData.objectIdRaw`
at SSBO binding=4) is the current load-bearing example. Future mech slices
M2.5/M3.5 will add more. Without automated contract checking, each new SSBO
or output location is a drift risk.

The existing Tier 1.1 gate (`scripts/validate_shaders.py`) catches GLSL
syntax and SPIR-V validity. It does NOT check binding numbers, output
locations, or UBO/SSBO member offsets against what the C++ side expects.

This spec adds a Tier 1.2 gate: **shader reflection CI**.

---

## 2. Non-goals

- No engine behavior change.
- No sampler/texture binding verification (runtime-assigned via `glUniform1i`; deferred to Vulkan-prep explicit-binding phase).
- No vertex attribute input location audit (runtime `glGetAttribLocation`; same deferral).
- No replacement of existing C++ `static_assert(sizeof == N)` discipline in `gos_static_prop_batcher.h`, `gos_mech_batcher.h`, etc. — that covers the C++ side; this spec covers the GLSL side.
- No change to `validate_shaders.py` behavior or exit semantics.

---

## 3. Repository layout

```
scripts/
  shader_common.py          <- new (Commit 1)
  validate_shaders.py       <- modified: imports shader_common (Commit 1, behavior unchanged)

tools/
  shader_reflect/
    reflect.py              <- new (Commit 2)
    expected/               <- new (Commit 2, populated by --update)
      shaders__gos_terrain.frag__default.json
      shaders__static_prop.frag__default.json
      shaders__static_prop.frag__coalesce.json
      shaders__static_prop.frag__objectid.json
      shaders__static_prop.frag__coalesce_objectid.json
      shaders__static_prop.vert__default.json
      shaders__static_prop.vert__coalesce.json
      shaders__static_prop.vert__objectid.json
      shaders__static_prop.vert__coalesce_objectid.json
      shaders__mech.frag__default.json
      shaders__mech.frag__objectid.json
      shaders__mech.vert__default.json
      shaders__mech.vert__objectid.json
      shaders__shadow_static_prop.vert__default.json
      shaders__shadow_static_prop.vert__coalesce.json
      ... (one file per shader x variant that has any contract surface)
```

Golden key = repo-relative path + variant, serialized as
`<path-with-slash-replaced-by-double-underscore>__<variant>.json`.
This avoids collisions between `static_prop.vert` and `static_prop.frag`,
and is future-proof for shaders in subdirs.

---

## 4. Commit 1 — `scripts/shader_common.py` (mechanical extraction)

Extract from `validate_shaders.py` verbatim, no logic changes:

- `_engine_get_path`, `_engine_find_next_include_directive`,
  `_engine_parse_include`, `_engine_get_num_lines`,
  `parse_includes_engine_style` — port of `shader_builder.cpp:262`
- Constants: `STAGE_BY_EXT`, `SKIP_SHADERS`, `SHADER_PREFIX_OVERRIDE`,
  `SHADER_TOKEN_REWRITES`, `VERSION_PREFIX`, `INCLUDE_EXTENSION`
- `find_tool(name) -> str | None` — Vulkan SDK + PATH lookup
- `discover_shaders() -> list[Path]` — walks `shaders/*.{vert,frag,...}`

New helper (does not exist in validate_shaders.py, added in shader_common.py):

```python
def build_shader_source(shader_path: Path,
                        extra_defines: list[str] = []) -> str:
    """Assemble full validated source: version prefix + extra defines +
    include-expanded body + token rewrites.

    Mirrors the makeProgram() preamble construction in
    GameOS/gameos/gameos_graphics.cpp. extra_defines are injected as
    #define lines between the version prefix and the body, matching the
    engine's runtime prefix-string composition for variant programs.

    Both validate_shaders.py and reflect.py call this so both gates
    see the exact same source string the engine compiles.
    """
```

`validate_shaders.py` after Commit 1:
- Imports `shader_common` instead of defining the above symbols.
- `validate_one()` calls `shader_common.build_shader_source(shader)` (no extra_defines; validate_shaders.py does not iterate variants).
- Output to stdout: byte-identical to before. Exit codes unchanged.
- Tier 1.1 gate result unchanged.

Commit 1 is a pure mechanical refactor. CI must pass before Commit 2 lands.

---

## 5. Commit 2 — `tools/shader_reflect/reflect.py`

### 5.1 Data flow

```
for shader_path in discover_shaders():
  variants = SHADER_VARIANTS.get(shader_path.relative_to(ROOT).as_posix(),
                                  [("default", [])])
  for (variant_name, extra_defines) in variants:
    if shader in SKIP_SHADERS: record as SKIPPED; continue
    src = build_shader_source(shader_path, extra_defines)
    write src to tempfile (same dir as shader, same suffix)
    run glslangValidator -V -R --auto-map-locations --auto-map-bindings
                         -o tmp.spv tmp.<ext>
    if nonzero: record COMPILE_ERROR; continue (do not fail yet)
    run spirv-cross --reflect tmp.spv -> raw JSON
    contract = normalize(raw_json, shader_path, variant_name, extra_defines)
    if --update:
      write contract to expected/<golden_key>.json
    else:
      load expected/<golden_key>.json
      if missing: record NEW (no golden yet; counts as failure unless --update)
      elif contract != expected: record DRIFT with unified diff
      else: record PASS
    cleanup tmp files
print summary
if any COMPILE_ERROR or DRIFT or NEW: exit 1
else: exit 0
```

### 5.2 reflect.py owns: COMPILE_ERROR reporting

`reflect.py` invokes `glslangValidator` only to obtain temporary SPIR-V for
reflection. It does NOT replace `validate_shaders.py` as the Tier 1.1 compile
gate. Compile errors are reported as `COMPILE_ERROR` / `REFLECT_PRECONDITION_FAILED`
and counted as reflect failures, but the Tier 1.1 gate remains authoritative
for GLSL validity.

### 5.3 SHADER_VARIANTS table

```python
# Key: repo-relative path (as_posix()). Use this key in SHADER_VARIANTS.
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
# All other shaders: [("default", [])] implied.
```

Known variant macros (authoritative list for M3 audit):
```python
KNOWN_VARIANT_MACROS = {"MC2_COALESCE", "MC2_OBJECT_ID_BUFFER"}
```

### 5.4 Variant coverage audit (M3)

On each run, for every shader not in SKIP_SHADERS:
- Grep the assembled source for `#if` / `#ifdef` / `#ifndef` / `defined(` tokens.
- Extract macro names. If any name is in `KNOWN_VARIANT_MACROS` AND the
  shader has no explicit entry in `SHADER_VARIANTS` (i.e., it gets the implied
  `[("default",[])]`), emit:
  ```
  WARNING: shaders/foo.frag uses MC2_COALESCE but has no SHADER_VARIANTS entry.
           Add it or this variant's bindings will not be reflected.
  ```
- In strict mode (`--strict-variants`), promote WARNING to FAIL.

---

## 6. Golden JSON schema

```json
{
  "shader": "shaders/static_prop.frag",
  "stage": "frag",
  "variant": "coalesce_objectid",
  "defines": ["MC2_COALESCE=1", "MC2_OBJECT_ID_BUFFER=1"],
  "ubos": [
    {
      "name": "CullUBO",
      "binding": 2,
      "members": [
        {"name": "viewProj", "offset": 0, "type": "mat4",
         "array_stride": null, "matrix_stride": 16}
      ]
    }
  ],
  "ssbos": [
    {
      "name": "PerDrawData",
      "binding": 4,
      "members": [
        {"name": "objectIdRaw", "offset": 24, "type": "int",
         "array_stride": null, "matrix_stride": null}
      ]
    }
  ],
  "outputs": [
    {"name": "FragColor",  "location": 0, "type": "vec4"},
    {"name": "GBuffer1",   "location": 1, "type": "vec4"},
    {"name": "v_objectId", "location": 2, "type": "uint"}
  ]
}
```

`size` is absent (C3 fix). Load-bearing fields: `offset`, `array_stride`,
`matrix_stride`, `type`, `binding`, `location`. `array_stride` and
`matrix_stride` are `null` for scalars/vectors.

Normalization rules for stable diffs (M4):
- JSON serialized with `indent=2, sort_keys=True`, trailing newline.
- `ubos` sorted by `binding` then `name`.
- `ssbos` sorted by `binding` then `name`.
- `members` sorted by `offset` then `name`.
- `outputs` sorted by `location` then `name`.
- Tool versions (`glslangValidator --version`, `spirv-cross --version`) logged
  to stdout during run, NOT included in golden JSON.

---

## 7. Hardcoded invariant checks (M1)

Goldens detect drift. Invariants enforce critical contracts regardless of
whether anyone ran `--update`. They fail even if the golden matches — if the
golden was updated to bless a bad value.

```python
REQUIRED_INVARIANTS = [
    # objectId MRT output: static_prop.frag must emit v_objectId at location=2.
    {
        "shader": "shaders/static_prop.frag",
        "variant": "coalesce_objectid",
        "check": "output",
        "name": "v_objectId", "location": 2, "type": "uint",
    },
    # GBuffer1 normal channel: all variants that write GBuffer1 must be at location=1.
    {
        "shader": "shaders/static_prop.frag",
        "variant": "default",
        "check": "output",
        "name": "GBuffer1", "location": 1, "type": "vec4",
    },
    {
        "shader": "shaders/mech.frag",
        "variant": "default",
        "check": "output",
        "name": "GBuffer1", "location": 1, "type": "vec4",
    },
    # objectIdRaw member offset: PerDrawData.objectIdRaw must remain at offset 24.
    {
        "shader": "shaders/static_prop.frag",
        "variant": "coalesce_objectid",
        "check": "ssbo_member",
        "block": "PerDrawData", "member": "objectIdRaw", "offset": 24,
    },
]
```

Invariant check runs after golden comparison. A failing invariant is a
`CONTRACT_VIOLATION` — exits 1 regardless of `--update`. To change an
invariant, the spec and the code must be updated in the same commit with a
justification in the commit message.

---

## 8. CLI interface

```
py -3 tools/shader_reflect/reflect.py [options]

Options:
  --update            Regenerate all expected/*.json goldens. Exit 0 on success.
                      Rejected (exit 1) when $CI env var is set.
  --shader PATH       Reflect only this shader (repeatable). Implies all variants.
  --variant NAME      Further restrict to this variant name (paired with --shader).
  --strict-variants   Promote variant-coverage WARNINGs to FAILs.
  --gl                Use GL-semantics SPIR-V (-G) instead of Vulkan (-V -R).

Exit codes:
  0 = all PASS (or --update succeeded)
  1 = any DRIFT, NEW, COMPILE_ERROR, CONTRACT_VIOLATION, or environment broken
```

---

## 9. CMake integration

```cmake
find_package(Python3 REQUIRED)

add_custom_target(shader_reflect
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/shader_reflect/reflect.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Shader contract reflection CI (Tier 1.2)"
    VERBATIM
)

# Optional: make shader_reflect depend on shader_validate so both run in order.
if(TARGET shader_validate)
    add_dependencies(shader_reflect shader_validate)
endif()
```

---

## 10. Relationship to existing grep gates

| Gate | Catches |
|------|---------|
| `check-render-contract-gbuffer1.sh` | Which `rc_*` helper function wrote GBuffer1 (GLSL code pattern) |
| `check-unified-projection-retirement.sh` | Forbidden `axisSwap*` call sites (code pattern) |
| `check-include-firewall.sh` | Forbidden C++ includes across module boundaries |
| `validate_shaders.py` (Tier 1.1) | GLSL syntax, SPIR-V validity, compile errors |
| **reflect.py (Tier 1.2, this spec)** | Binding numbers, output locations, UBO/SSBO member offsets, variant coverage |

Grep gates = GLSL code-pattern contracts.
Reflection CI = structural/binary layout contracts.
Orthogonal. Both stay.

---

## 11. Implementation plan shape (for writing-plans)

Two commits, sequenced:

**Commit 1 — shader_common extraction**
- Create `scripts/shader_common.py`
- Modify `validate_shaders.py` to import it
- Run `validate_shaders.py` — must produce byte-identical output
- Commit only when Tier 1.1 still passes

**Commit 2 — reflect.py + goldens**
- Create `tools/shader_reflect/reflect.py`
- Run `py -3 tools/shader_reflect/reflect.py --update` to bootstrap goldens
- Review `git diff tools/shader_reflect/expected/` (should show new .json files)
- Commit `reflect.py` + all `expected/*.json`
- Run `py -3 tools/shader_reflect/reflect.py` (no --update) — must exit 0
- Verify invariant checks fire correctly by temporarily mutating one field

Subagent dispatch recommended for Commit 2 implementation: reflect.py is
self-contained enough to execute in an isolated worktree.

---

## 12. Exit criteria

- [ ] `validate_shaders.py` behavior unchanged after Commit 1 (byte-identical stdout)
- [ ] `reflect.py --update` populates `expected/` without errors
- [ ] `reflect.py` (no flags) exits 0 on clean tree
- [ ] Mutating `layout(location=2)` in `static_prop.frag` causes `reflect.py` to exit 1
- [ ] Mutating `objectIdRaw` offset in `static_prop.frag` SSBO causes exit 1
- [ ] REQUIRED_INVARIANTS fire independently of golden: manually set golden to bad value, confirm CONTRACT_VIOLATION still exits 1
- [ ] `reflect.py --update` rejected in `CI=true` environment
- [ ] Variant coverage audit: adding `#ifdef MC2_COALESCE` to a shader without a VARIANTS entry emits WARNING
- [ ] CMake target `shader_reflect` succeeds from clean build dir
