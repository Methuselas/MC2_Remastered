# MaterialGpu-1 Reflect/Contract Fixture — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lock the `MaterialGpu` GLSL struct layout into reflection CI by adding a non-runtime fixture shader, a recursive shader-discovery fix, an `array_stride` invariant extension, and nine layout invariants — so any C++/GLSL mirror drift is caught at build time before the first runtime upload slice.

**Architecture:** Four files change: `shader_common.py` gains recursive discovery; a new `shaders/fixtures/` fixture includes the GLSL mirror and forces spirv-cross to reflect all 8 struct fields; `reflect.py` gains one SHADER_VARIANTS entry, an `array_stride` field on the `ssbo_member` check, and nine `REQUIRED_INVARIANTS`; the golden is generated and committed. No production shaders, no runtime code.

**Tech Stack:** Python 3, glslangValidator (SPIR-V compile), spirv-cross (reflection), GLSL 430

---

## File map

| File | Change |
|---|---|
| `scripts/shader_common.py` | Edit `discover_shaders()`: flat glob → recursive |
| `shaders/fixtures/material_gpu_contract.frag` | Create: reflection fixture shader |
| `tools/shader_reflect/reflect.py` | Edit: SHADER_VARIANTS + ssbo_member extension + 9 invariants |
| `tools/shader_reflect/expected/shaders__fixtures__material_gpu_contract.frag__default.json` | Create: generated golden (via `--update`) |

---

### Task 1: Make `discover_shaders()` recursive

**Files:**
- Modify: `scripts/shader_common.py:191-196`

- [ ] **Step 1.1: Edit `discover_shaders()`**

In `scripts/shader_common.py`, replace lines 191-196:

```python
def discover_shaders() -> list[Path]:
    """Return all shader files in shaders/, sorted by name within each extension group."""
    out: list[Path] = []
    for ext in STAGE_BY_EXT:
        out.extend(sorted(SHADERS.glob(f"*{ext}")))
    return out
```

with:

```python
def discover_shaders() -> list[Path]:
    """Return all shader files under shaders/, sorted by extension then name.
    Recursive: includes shaders/fixtures/ and any future subdirectories.
    Convention: any file under shaders/**/*.frag|vert|... is part of shader
    validation unless explicitly listed in SKIP_SHADERS.
    """
    out: list[Path] = []
    for ext in STAGE_BY_EXT:
        out.extend(sorted(SHADERS.glob(f"**/*{ext}")))
    return out
```

- [ ] **Step 1.2: Verify discovery still works on existing shaders**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
python3 -c "
import sys; sys.path.insert(0,'scripts')
import shader_common
shaders = shader_common.discover_shaders()
print(f'{len(shaders)} shaders found')
for s in shaders: print(' ', s.relative_to(shader_common.ROOT))
"
```

Expected: same shaders as before (static_prop, mech, terrain, etc.) plus any in `shaders/fixtures/` once created. Count should be ≥ existing count. No `.hglsl` files should appear.

- [ ] **Step 1.3: Run `validate_shaders.py` to confirm recursive discovery causes no regressions**

Task 1 changes the global shader set. Run this before committing to catch any pre-existing nested test shaders that would now compile and fail:

```powershell
py -3 scripts/validate_shaders.py
```

Expected: same shaders as before pass, exit code 0. If any unexpected shader appears and fails, add it to `SKIP_SHADERS` in `shader_common.py` before proceeding.

- [ ] **Step 1.5: Commit**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
git add scripts/shader_common.py
git commit -m "fix(shader_common): recursive discover_shaders for fixtures/ subdirectory

Any file under shaders/**/*.frag|vert|... now participates in both
validate_shaders.py (Tier 1.1) and reflect.py (Tier 1.2) unless
explicitly listed in SKIP_SHADERS. Required for MaterialGpu-1 fixture.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

### Task 2: Create the fixture shader

**Files:**
- Create: `shaders/fixtures/material_gpu_contract.frag`

- [ ] **Step 2.1: Create `shaders/fixtures/` directory and fixture file**

Create `shaders/fixtures/material_gpu_contract.frag` with this exact content:

```glsl
// shaders/fixtures/material_gpu_contract.frag
//
// MaterialGpu-1 reflection fixture. NOT part of the game runtime.
// Never compiled into mc2.exe. Exists solely to produce a spirv-cross
// --reflect golden that validates MaterialGpu struct layout.
//
// Binding 5 is fixture/prototype-only — not a runtime allocation.
// Runtime binding is assigned in MaterialGpu-2.
//
// No #version directive: makeProgram() / reflect.py prepend "#version 430\n".

#include <include/material_gpu.hglsl>

layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
} materialTable_;

layout(location = 0) out vec4 o_color;

void main() {
    // Read all 8 fields through a data-dependent accumulator so spirv-cross
    // cannot optimize the struct away before reflection.
    // fract(acc * 0.001) keeps the output in [0, 1] for a valid fragment.
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
}
```

- [ ] **Step 2.2: Verify the fixture compiles cleanly via `validate_shaders.py`**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
py -3 scripts/validate_shaders.py
```

Expected: all existing shaders PASS + `shaders/fixtures/material_gpu_contract.frag` PASS. No FAIL or COMPILE_ERROR lines.

If the fixture produces a compile error, fix the GLSL before proceeding. Common causes:
- `uint` cast: `float(m.albedoTex)` — valid in GLSL 430 ✓
- `layout(std430)` — requires GLSL 430 which is the project default ✓
- `#include` path: resolved relative to `shaders/` root ✓

- [ ] **Step 2.3: Commit**

```powershell
git add shaders/fixtures/material_gpu_contract.frag
git commit -m "feat(fixtures): MaterialGpu-1 reflection fixture shader

Non-runtime fragment shader. Includes material_gpu.hglsl and declares
MaterialTable SSBO (binding=5, prototype-only). Accumulator pattern
forces spirv-cross to reflect all 8 MaterialGpu struct fields.

Compiles cleanly under validate_shaders.py (Tier 1.1).

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

### Task 3: Add SHADER_VARIANTS entry and bootstrap the golden

**Files:**
- Modify: `tools/shader_reflect/reflect.py:63-90` (SHADER_VARIANTS dict)

- [ ] **Step 3.0: Preflight — confirm `_v` helper exists in `reflect.py`**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
python3 -c "
import sys; sys.path.insert(0,'tools/shader_reflect')
import importlib.util, pathlib
src = pathlib.Path('tools/shader_reflect/reflect.py').read_text(encoding='utf-8')
if 'def _v' in src or '_v(' in src:
    print('_v helper found — use _v(\"default\", []) in SHADER_VARIANTS entry')
else:
    print('_v helper NOT found — use (\"default\", []) tuple directly')
"
```

If `_v` is absent, the SHADER_VARIANTS entry in Step 3.1 becomes a plain tuple — check how existing entries are structured and match that shape exactly.

- [ ] **Step 3.1: Add fixture to SHADER_VARIANTS**

In `tools/shader_reflect/reflect.py`, find the `SHADER_VARIANTS` dict (ends around line 90 with `}`). Add the fixture entry at the end of the dict, before the closing `}`:

```python
    "shaders/fixtures/material_gpu_contract.frag": [
        _v("default", []),
    ],
```

The dict should now end:

```python
    "shaders/shadow_static_prop.vert": [
        _v("default",  []),
        _v("coalesce", ["MC2_COALESCE=1"]),
    ],
    "shaders/fixtures/material_gpu_contract.frag": [
        _v("default", []),
    ],
}
```

- [ ] **Step 3.2: Run `--update` to generate the golden and bootstrap-inspect the raw types**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
py -3 tools/shader_reflect/reflect.py --update --shader shaders/fixtures/material_gpu_contract.frag
```

Expected: command exits 0 and creates `tools/shader_reflect/expected/shaders__fixtures__material_gpu_contract.frag__default.json`. The exact log wording ([NEW] / [UPDATE] / written) may vary; confirm by checking the file exists, not by matching log text.

Then inspect the generated golden to confirm binding, array_stride, and SSBO name:

```powershell
cat tools/shader_reflect/expected/shaders__fixtures__material_gpu_contract.frag__default.json
```

Verify:
- `"ssbos"` contains one entry with `"name": "MaterialTable"` and `"binding": 5`
- The `"members"` array has one entry with `"name": "materials"` and `"array_stride": 32`

- [ ] **Step 3.3: Bootstrap-inspect the MaterialGpu type name in raw spirv-cross JSON**

Run spirv-cross manually on the compiled SPIR-V to confirm the type name survives reflection.
Use Python to write the temp file (avoids PowerShell BOM/newline issues) and `-V -R` for Vulkan
mode so the SPIR-V profile matches `reflect.py`'s own compile path:

```powershell
# Write the prepended shader source using Python (matches reflect.py exactly, no BOM risk)
py -3 -c "
import sys, pathlib
sys.path.insert(0, 'scripts')
import shader_common
src = shader_common.build_shader_source(shader_common.SHADERS / 'fixtures/material_gpu_contract.frag')
out = pathlib.Path(r'$env:TEMP\mat_contract.frag')
out.write_text(src, encoding='utf-8')
print('wrote', out)
"

# Compile to SPIR-V using Vulkan mode (-V -R) — must match reflect.py's compile path
glslangValidator -V -R -S frag "$env:TEMP\mat_contract.frag" -o "$env:TEMP\mat_contract.spv"

# Inspect types for MaterialGpu name
spirv-cross "$env:TEMP\mat_contract.spv" --reflect | py -3 -c "
import sys, json
d = json.load(sys.stdin)
for tid, t in d.get('types', {}).items():
    name = t.get('name', '')
    members = [m['name'] for m in t.get('members', [])]
    if members:
        print(f'  type_id={tid} name={name!r} members={members}')
"
```

Expected: one type entry with `name='MaterialGpu'` and `members=['albedoTex', 'normalTex', 'metallicRoughnessTex', 'emissiveTex', 'flags', 'baseColorFactor', 'metallicFactor', 'roughnessFactor']`.

If the type name is different (e.g. `'_MaterialGpu'` or `''`), note the actual value — you will use it as `"type_name"` in Task 5's invariants instead of `"MaterialGpu"`.

- [ ] **Step 3.4: Commit SHADER_VARIANTS entry and golden**

```powershell
git add tools/shader_reflect/reflect.py
git add "tools/shader_reflect/expected/shaders__fixtures__material_gpu_contract.frag__default.json"
git commit -m "feat(reflect): add MaterialGpu fixture to SHADER_VARIANTS + bootstrap golden

Generates golden for shaders/fixtures/material_gpu_contract.frag (default).
Golden confirms: binding=5, MaterialTable SSBO, array_stride=32, o_color output.
Invariants added in next task.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

### Task 4: Extend `ssbo_member` check to validate `array_stride`

**Files:**
- Modify: `tools/shader_reflect/reflect.py:504-510` (ssbo_member elif block)

- [ ] **Step 4.1: Add `array_stride` check to the `ssbo_member` branch**

In `tools/shader_reflect/reflect.py`, find the `elif check == "ssbo_member":` block.
The block currently ends at approximately line 510:

```python
            m = members[0]
            if m["offset"] != inv["offset"]:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: "
                    f"ssbo member '{inv['block']}.{inv['member']}' "
                    f"offset={m['offset']}, expected {inv['offset']}"
                )
```

Add the following immediately after (before `elif check == "type_member":`):

```python
            if "array_stride" in inv:
                if m.get("array_stride") != inv["array_stride"]:
                    violations.append(
                        f"CONTRACT_VIOLATION: {key}: "
                        f"ssbo member '{inv['block']}.{inv['member']}' "
                        f"array_stride={m.get('array_stride')}, "
                        f"expected {inv['array_stride']}"
                    )
```

The full `ssbo_member` block should now read:

```python
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
            if "array_stride" in inv:
                if m.get("array_stride") != inv["array_stride"]:
                    violations.append(
                        f"CONTRACT_VIOLATION: {key}: "
                        f"ssbo member '{inv['block']}.{inv['member']}' "
                        f"array_stride={m.get('array_stride')}, "
                        f"expected {inv['array_stride']}"
                    )
```

- [ ] **Step 4.2: Verify existing invariants still pass (no regressions)**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
py -3 tools/shader_reflect/reflect.py
```

Expected: all existing goldens PASS, no CONTRACT_VIOLATION lines. The new `array_stride` path is only triggered when an invariant includes the `"array_stride"` key — no existing invariants have it, so nothing changes for them.

- [ ] **Step 4.3: Commit**

```powershell
git add tools/shader_reflect/reflect.py
git commit -m "feat(reflect): extend ssbo_member invariant check with array_stride field

Existing invariants without 'array_stride' key are unaffected.
Required for MaterialGpu.materials[] stride=32 hard gate.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

### Task 5: Add nine `REQUIRED_INVARIANTS` entries

**Files:**
- Modify: `tools/shader_reflect/reflect.py:151-152` (end of REQUIRED_INVARIANTS list)

**Prerequisite:** If Task 3 bootstrap found a type name other than `"MaterialGpu"`, substitute that name for `"MaterialGpu"` in the invariants below.

- [ ] **Step 5.1: Append nine invariants to `REQUIRED_INVARIANTS`**

In `tools/shader_reflect/reflect.py`, find line 151-152:

```python
    },
]
```

This is the end of the `REQUIRED_INVARIANTS` list (after the `PerDrawEntry.objectIdRaw` entry). Replace the closing `]` with the following, then close:

```python
    },
    # MaterialGpu field offsets — lock struct layout independently of golden.
    # type_name "MaterialGpu" confirmed by bootstrap spirv-cross inspection (Task 3).
    # These survive --update: CONTRACT_VIOLATION fires even when regenerating goldens.
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "albedoTex",
        "offset": 0,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "normalTex",
        "offset": 4,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "metallicRoughnessTex",
        "offset": 8,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "emissiveTex",
        "offset": 12,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "flags",
        "offset": 16,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "baseColorFactor",
        "offset": 20,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "metallicFactor",
        "offset": 24,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "roughnessFactor",
        "offset": 28,
    },
    # MaterialGpu array stride — hard gate: --update cannot bless a stride change.
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "ssbo_member",
        "block": "MaterialTable",
        "member": "materials",
        "offset": 0,
        "array_stride": 32,
    },
]
```

- [ ] **Step 5.2: Run the full reflect suite — all invariants must pass**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
py -3 tools/shader_reflect/reflect.py
```

Expected output (key lines):
```
[PASS] shaders/fixtures/material_gpu_contract.frag / default
```
And at the end: exit code 0, no `CONTRACT_VIOLATION` lines.

If you see `CONTRACT_VIOLATION: ... type 'MaterialGpu' not found`:
- The spirv-cross type name differs from `"MaterialGpu"`.
- Re-run the bootstrap command from Task 3.3 to find the actual name.
- Update `"type_name"` in all 8 `type_member` invariants to match.
- Re-run the suite.

If you see `CONTRACT_VIOLATION: ... array_stride=<N>, expected 32`:
- The struct layout has drifted. Do NOT proceed. Investigate `material_gpu.hglsl` for unexpected padding or field order.

- [ ] **Step 5.3: Run the mirror gate to confirm C++/GLSL consistency**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
sh scripts/check-material-gpu-mirror.sh
```

Expected: `OK: MaterialGpu field order matches (8 fields)`

- [ ] **Step 5.4: Commit**

```powershell
git add tools/shader_reflect/reflect.py
git commit -m "feat(reflect): MaterialGpu-1 — 9 layout invariants for MaterialGpu struct

8 type_member invariants lock field offsets (0/4/8/12/16/20/24/28).
1 ssbo_member array_stride invariant locks stride=32.
All survive --update: CONTRACT_VIOLATION fires even on golden regeneration.
Full suite PASS confirmed.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

### Task 6: Final verification

- [ ] **Step 6.1: Run full reflect suite clean (no --update)**

```powershell
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
py -3 tools/shader_reflect/reflect.py
```

Expected:
- All pre-existing goldens: `PASS`
- `shaders/fixtures/material_gpu_contract.frag / default`: `PASS`
- No `DRIFT`, `NEW`, `COMPILE_ERROR`, `CONTRACT_VIOLATION` lines
- Exit code: `0`

- [ ] **Step 6.2: Run validate_shaders.py to confirm fixture compiles**

```powershell
py -3 scripts/validate_shaders.py
```

Expected: fixture appears and PASS. Existing shaders unchanged.

- [ ] **Step 6.3: Run the validator fixture tests**

```powershell
sh tools/material_cook/tests/run_tests.sh
```

Expected: `Results: 2 passed, 0 failed`

- [ ] **Step 6.4: Run the mirror gate**

```powershell
sh scripts/check-material-gpu-mirror.sh
```

Expected: `OK: MaterialGpu field order matches (8 fields)`

- [ ] **Step 6.5: Mutation test — prove the invariants are live (not just present)**

Temporarily swap two adjacent fields in `shaders/include/material_gpu.hglsl` to make offsets wrong — e.g. swap `metallicFactor` and `roughnessFactor`. Do **not** commit this change.

```powershell
# In material_gpu.hglsl, swap these two lines (edit temporarily):
#   float    metallicFactor;      // 24
#   float    roughnessFactor;     // 28
# to:
#   float    roughnessFactor;     // 24
#   float    metallicFactor;      // 28

py -3 tools/shader_reflect/reflect.py --shader shaders/fixtures/material_gpu_contract.frag
```

Expected: `CONTRACT_VIOLATION` for `metallicFactor` or `roughnessFactor` offset.

Then revert:

```powershell
git checkout shaders/include/material_gpu.hglsl
```

Run the full suite one final time to confirm clean:

```powershell
py -3 tools/shader_reflect/reflect.py
```

Expected: exit 0, no violations.

- [ ] **Step 6.7: Confirm invariant coverage summary**

At this point the following are hard build-gate violations:

| What drifts | Caught by |
|---|---|
| Any `MaterialGpu` field renamed/reordered | `check-material-gpu-mirror.sh` + 8 `type_member` invariants |
| Struct grows past 32 bytes | `static_assert(sizeof == 32)` in C++ |
| `array_stride` changes from 32 | `ssbo_member` `array_stride` invariant |
| GLSL mirror field order diverges from C++ | `check-material-gpu-mirror.sh` |
| Golden diverges from reflection | `reflect.py` golden comparison |
| Binding 5 changes in fixture | golden comparison |
