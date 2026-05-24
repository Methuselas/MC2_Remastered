# MaterialGpu-1: Reflect/Contract Fixture

**Date:** 2026-05-24 (rev 2 — reviewer blockers addressed)
**Status:** Approved for planning
**Slice:** MaterialGpu-1 (reflect/contract only — no runtime code)
**Predecessor:** MaterialGpu-0 (schema + offline prototype, shipped 2026-05-24)
**Successor:** MaterialGpu-2 (first runtime upload slice, gated on design decision)

---

## 1. Goal

Lock the `MaterialGpu` struct layout into the shader reflection CI so that any
C++/GLSL mirror drift is caught at build time, before the first runtime upload
slice touches it. No game code changes. No runtime behavior changes.

This is a minimal static-prop material contract fixture, not the final
material model. The 8-field struct is intentionally minimal for MaterialGpu-0;
future fields (damage variants, emissive scale, etc.) will extend it.

---

## 2. Scope

### In scope
- `scripts/shader_common.py` — make `discover_shaders()` recursive
- `shaders/fixtures/material_gpu_contract.frag` — new fixture shader
- `tools/shader_reflect/reflect.py` — SHADER_VARIANTS entry + 9 invariants + ssbo_member array_stride support
- `tools/shader_reflect/expected/shaders__fixtures__material_gpu_contract.frag__default.json` — generated golden

### Out of scope
- No changes to `static_prop.frag`, `mech.frag`, or any production shader
- No SSBO binding allocated in the runtime path
- No `PerDrawEntry.materialIdx` field added
- No `MaterialGpu` table upload in C++
- No texture binding behavior changed

---

## 3. C1 — Recursive shader discovery

**Problem:** `discover_shaders()` in `scripts/shader_common.py` uses a flat
glob (`SHADERS.glob(f"*{ext}")`). `shaders/fixtures/*.frag` is not found by
the full reflect suite unless `--shader` is passed explicitly.

**Fix:** Change to recursive glob in `discover_shaders()`:

```python
def discover_shaders() -> list[Path]:
    """Return all shader files under shaders/, sorted by extension then name.
    Recursive: includes shaders/fixtures/ and any future subdirectories."""
    out: list[Path] = []
    for ext in STAGE_BY_EXT:
        out.extend(sorted(SHADERS.glob(f"**/*{ext}")))
    return out
```

`STAGE_BY_EXT` keys (`.vert`, `.frag`, etc.) do not match `.hglsl`, so
`shaders/include/` header files are never returned.

**New convention (document in `shader_common.py`):** Any file under
`shaders/**/*.frag|vert|...` is part of shader validation and reflection unless
explicitly listed in `SKIP_SHADERS`. Future backup or test shaders added under
`shaders/fixtures/` must either compile cleanly or have a `SKIP_SHADERS` entry.

`validate_shaders.py` also calls `discover_shaders()` — the fixture must be a
valid, standalone-compilable GLSL shader. M3 resolves this: the fixture is
written to compile cleanly. No `SKIP_SHADERS` entry needed.

---

## 4. Fixture shader

**Path:** `shaders/fixtures/material_gpu_contract.frag`

A minimal, non-runtime fragment shader whose sole purpose is to produce a
spirv-cross `--reflect` golden that validates the `MaterialGpu` struct layout.
It is never compiled into `mc2.exe`.

### Design constraints

- `#include <include/material_gpu.hglsl>` — exercises the actual include path
- `layout(std430, binding=5) readonly buffer MaterialTable { MaterialGpu materials[]; }` — binding 5 matches the reservation in `tools/material_cook/reflect_expected_layout.md`
- **Binding 5 is fixture/prototype-only.** No runtime binding is allocated until MaterialGpu-2 explicitly assigns it in the runtime binding registry. "Fixture uses binding 5" is NOT evidence that runtime binding 5 is live.
- `main()` uses an accumulator pattern (see §4.1) to read all 8 fields — defeats constant-folding
- Single `layout(location=0) out vec4 o_color` output — minimum valid fragment shader
- No `#version` directive — prepended by the reflect tool / `makeProgram()` convention

### 4.1 Keep-alive accumulator pattern

Spirv-cross reflects only live resources. The accumulator defeats compiler
constant-folding by producing an output that depends on all 8 fields:

```glsl
MaterialGpu m = materialTable_.materials[0];
float acc = 0.0;
acc += float(m.albedoTex);
acc += float(m.normalTex);
acc += float(m.metallicRoughnessTex);
acc += float(m.emissiveTex);
acc += float(m.flags);
acc += m.baseColorFactor;
acc += m.metallicFactor;
acc += m.roughnessFactor;
o_color = vec4(fract(acc * 0.001), 0.0, 0.0, 1.0);
```

`fract(acc * 0.001)` ensures the value is always in [0, 1] (valid fragment
output) while being data-dependent on all 8 fields — the compiler cannot fold
any of them away.

---

## 5. SHADER_VARIANTS entry

Add to `SHADER_VARIANTS` dict in `tools/shader_reflect/reflect.py`:

```python
"shaders/fixtures/material_gpu_contract.frag": [
    _v("default", []),
],
```

One variant. No defines. No ARB rewrite needed.

---

## 6. C2 — `type_member` check: verify type name survives spirv-cross

`type_member` invariant support already exists in `reflect.py` (lines ~512-553),
used by the existing `PerDrawEntry.objectIdRaw` invariant. This slice reuses
the same mechanism.

**Bootstrap step (required during implementation):** After running
`reflect.py --update`, inspect the raw spirv-cross JSON to confirm the type
name is preserved as exactly `"MaterialGpu"`. The check scans:

```python
for tinfo in raw.get("types", {}).values():
    if tinfo.get("name") == type_name:  # "MaterialGpu"
        found_type = tinfo
        break
```

If spirv-cross emits the type under a mangled name (e.g. `"_MaterialGpu"` or
an empty string), the lookup will fail and the invariants will fire with
`CONTRACT_VIOLATION: type 'MaterialGpu' not found`. In that case, inspect the
raw JSON, identify the actual emitted name, and adjust the invariant
`type_name` field accordingly before committing the golden.

Procedure:
1. Compile the fixture to SPIR-V manually:
   ```powershell
   # glslangValidator is in tools/shader_reflect/ or on PATH after Tier 1.2 setup
   glslangValidator -G -S frag shaders/fixtures/material_gpu_contract.frag -o /tmp/mat.spv
   spirv-cross /tmp/mat.spv --reflect | python3 -c "import sys,json; d=json.load(sys.stdin); [print(v.get('name','?'), list(v.get('members',[]))[:1]) for v in d.get('types',{}).values()]"
   ```
2. Confirm `"MaterialGpu"` appears verbatim in the output.
3. If spirv-cross emits a different name, update the `type_name` field in the
   invariants accordingly before committing the golden.
4. No edits to `check_invariants` are needed for this inspection.

---

## 7. REQUIRED_INVARIANTS entries

Nine invariants total: 8 `type_member` field-offset checks + 1 extended
`ssbo_member` array-stride check.

### 7.1 Eight `type_member` field-offset invariants

```python
# MaterialGpu field offsets — lock struct layout independently of golden.
# type_name confirmed by bootstrap inspection step (§6).
*[
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": member,
        "offset": offset,
    }
    for member, offset in [
        ("albedoTex",            0),
        ("normalTex",            4),
        ("metallicRoughnessTex", 8),
        ("emissiveTex",         12),
        ("flags",               16),
        ("baseColorFactor",     20),
        ("metallicFactor",      24),
        ("roughnessFactor",     28),
    ]
],
```

### 7.2 One `ssbo_member` array-stride invariant

The existing `ssbo_member` check only validates `offset`. This slice extends
it to also check `array_stride` when the invariant specifies it:

```python
{
    "shader": "shaders/fixtures/material_gpu_contract.frag",
    "variant": "default",
    "check": "ssbo_member",
    "block": "MaterialTable",
    "member": "materials",
    "offset": 0,
    "array_stride": 32,
}
```

**reflect.py extension required:** In the `elif check == "ssbo_member":` branch,
after the `offset` check, add:

```python
if "array_stride" in inv:
    if m.get("array_stride") != inv["array_stride"]:
        violations.append(
            f"CONTRACT_VIOLATION: {key}: "
            f"ssbo member '{inv['block']}.{inv['member']}' "
            f"array_stride={m.get('array_stride')}, expected {inv['array_stride']}"
        )
```

This is additive — existing invariants without `array_stride` are unaffected.

---

## 8. M3 — validate_shaders.py compatibility

`validate_shaders.py` also calls `discover_shaders()` and will pick up the
fixture after the recursive change. The fixture must compile cleanly as a
standalone shader. No `SKIP_SHADERS` entry needed. This is the preferred
approach: reflection fixtures also guard the source-construction path.

---

## 9. Golden file

Generated by:

```powershell
py -3 tools/shader_reflect/reflect.py --update --shader shaders/fixtures/material_gpu_contract.frag
```

Expected shape (the `type` ref is an opaque spirv-cross ID; shown illustratively):

```json
{
  "defines": [],
  "outputs": [{"location": 0, "name": "o_color", "type": "vec4"}],
  "shader": "shaders/fixtures/material_gpu_contract.frag",
  "ssbos": [
    {
      "binding": 5,
      "members": [
        {"array_stride": 32, "matrix_stride": null, "name": "materials", "offset": 0, "type": "<opaque spirv-cross type ref>"}
      ],
      "name": "MaterialTable"
    }
  ],
  "stage": "frag",
  "ubos": [],
  "variant": "default"
}
```

The `type` field value in the real golden may be a spirv-cross internal type
reference string. It is included for diagnostic value only; correctness is
enforced by the `type_member` and `array_stride` invariants, not by assuming
the opaque ref is semantically stable.

---

## 10. Verification

After implementation:

```powershell
# Full suite — fixture must appear and PASS
py -3 tools/shader_reflect/reflect.py
```

Expected: all existing goldens PASS + fixture PASS + 9 new invariants PASS.

Tier1 smoke is not part of this slice (no runtime changes).

---

## 11. Invariant coverage summary (MaterialGpu-0 + MaterialGpu-1)

| What drifts | How caught |
|---|---|
| `MaterialGpu` field renamed or reordered | `check-material-gpu-mirror.sh` (order) + 8 `type_member` invariants (offsets) |
| Struct grows past 32 bytes | `static_assert(sizeof == 32)` in C++ |
| `array_stride` changes from 32 | `ssbo_member` `array_stride` invariant + golden |
| GLSL mirror diverges from C++ | `check-material-gpu-mirror.sh` |
| Golden diverges from reflection | `reflect.py` golden comparison |
| Binding 5 changes in fixture | golden comparison |

---

## 12. Files changed

| File | Change |
|---|---|
| `scripts/shader_common.py` | EDIT — `discover_shaders()` recursive glob |
| `shaders/fixtures/material_gpu_contract.frag` | CREATE — fixture shader |
| `tools/shader_reflect/reflect.py` | EDIT — SHADER_VARIANTS + 9 invariants + ssbo_member array_stride check |
| `tools/shader_reflect/expected/shaders__fixtures__material_gpu_contract.frag__default.json` | CREATE — generated golden |
