# Static-Prop ORM (runtime) Implementation Plan — v2 (post-review)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.
> **Supersedes** `2026-06-02-staticprop-orm-now.md` (v1). Folds in `...-REVIEW.md` (all BLOCKER/MAJOR/MINOR fixes).

**Goal:** Sample an authored ORM map's **roughness (G) and metallic (B)** per static-prop material at runtime, multiplied into the existing per-fragment sun-specular, behind a default-OFF gate, byte-identical when off or when a material has no ORM map.

**Scope change from v1 (per review BLOCKER-4):** **AO is CUT from this arc.** AO requires moving ambient/IBL out of the vertex `lit` accumulation, which breaks byte-identity in three ways and is a visual no-op until ambient/IBL strength>0. Roughness/metallic needs **no** vertex/ambient change — it only adds gated sampling inside the existing PBR specular block, so gate-OFF identity is trivial. AO is deferred to a follow-on (`STATICPROP-ORM-AO-1`) with the re-fold-before-clamp design from the review.

**Architecture:** A **per-bucket sibling** array `s_ormBucketArrays[b]`, built with the *same* `(group,dim)` partition and layer ordering as `s_bucketArrays[b]`, so the invariant `metallicRoughnessTex == albedoTex` holds and the existing bucket-relative `uvSampled`/layer indexing is reused unchanged. ORM maps are discovered as `<sourceBase>.orm.ktx2` sidecars (derivation mirrors the engine's own `.ktx2` derivation). Internalformat is **linear** (UNORM BC7 / RGBA8). Gated by `MC2_STATICPROP_MATERIAL_PBR_SLOTS` (compile-define + runtime `u_ormSampleEnable`), forced off so OFF is bit-identical.

**Tech Stack:** C++17 (static-prop batcher), GLSL 430 (`static_prop.frag` only — no vert change), `RenderCore::ktxLoadRgba8`, Python cook (`tools/mc2texcook`), `tools/shader_reflect`, `scripts/capture_baseline.py`, `scripts/run_smoke.py`.

**Spec:** `docs/superpowers/specs/2026-06-02-staticprop-orm-now-spec.md` (AC-AO items now deferred). **Review:** `...-REVIEW.md`.

---

## Hard constraints carried from review
- **ORM array is per-bucket, layer-aligned to albedo** (`metallicRoughnessTex == albedoTex`). Never a flat array.
- **ORM dims MUST equal the albedo dims** for the same unique; mismatches are rejected (treated absent + logged), mirroring the albedo `dim_mismatch` rule (`gos_static_prop_batcher.cpp:2369-2371`).
- **Sidecar name** = engine `.ktx2` derivation applied to `<sourceBase>.orm.ktx2` (strip last ext after last path sep; mirror `txmmgr.cpp:3449-3468`).
- **No vertex shader change**, no ambient/IBL restructure (AO deferred).
- Gate dual-interlock: frag block under `#ifdef MC2_STATICPROP_PBR_SLOTS` **and** runtime `u_ormSampleEnable` forced 0 when off.
- No `MaterialGpu` / mirror / manifest-schema change.

---

## File structure
| File | Responsibility | Action |
|---|---|---|
| `GameOS/gameos/gos_static_prop_batcher.cpp` | gate, per-bucket ORM array build, sidecar feed, material ORM field, all-site bind, teardown, uniform upload | Modify |
| `shaders/static_prop.frag` | gated ORM roughness/metallic sample × scalars; debug 5/6 reflect the map | Modify |
| `tools/mc2texcook/batch_cook.py` | slot-aware BC7 (linear UNORM for orm) | Modify |
| `tools/mc2texcook/tests/test_mc2texcook.py` | BC7-linear cook test (real unittest helpers) | Modify |
| `tests/fixtures/assets/orm_runtime/manifest.json` | validator fixture (real schema) | Create |
| `data/visual_tuning.json` + `visual_tuning_profile.cpp` | `staticPropOrmStrength` knob | Modify |
| `scripts/run_smoke.py` | allowlist the gate + strength env | Modify |
| `tools/shader_reflect/expected/shaders__static_prop.frag__*.json` | regenerated goldens | Modify (generated) |

No vert golden changes (no vert edit). No `tests/unit` change (ssboBindingsMask unchanged — ORM adds sampler uniforms, not SSBO bindings).

---

## Task A1: Gate + smoke allowlist
**Files:** `gos_static_prop_batcher.cpp` (after `s_pbrV1Enabled` `:498-501`), `scripts/run_smoke.py` (allowlist `~:528-538`)

- [ ] **Step 1:** add gate parse:
```cpp
static const bool s_ormSlotsEnabled = []() {
    const char* v = getenv("MC2_STATICPROP_MATERIAL_PBR_SLOTS");
    return v != nullptr && v[0] != '0' && v[0] != '\0';
}();
```
- [ ] **Step 2:** add to the `run_smoke.py` env allowlist (next to `MC2_STATIC_PROP_PBR_V1`):
```python
    "MC2_STATICPROP_MATERIAL_PBR_SLOTS",
    "MC2_STATICPROP_ORM_STRENGTH",
```
- [ ] **Step 3:** build engine (per `docs/critical_inline_rules.md`). File-scope `static const` unused is not a `-Werror` (only `-Werror=array-bounds`; `-Wunused-const-variable` not set) → builds clean. A1 is independently committable.
- [ ] **Step 4:** commit
```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp scripts/run_smoke.py
git commit -m "feat(staticprop): add MC2_STATICPROP_MATERIAL_PBR_SLOTS gate (no behavior)"
```

---

## Task B1: Slot-aware BC7 cook (linear UNORM for ORM)
**Files:** `tools/mc2texcook/batch_cook.py:52-97` + its call site; `tools/mc2texcook/tests/test_mc2texcook.py` (unittest, real helpers)

- [ ] **Step 1: failing test** using the suite's REAL helpers (`_make_rgb_image`, `_parse_ktx2_header`→dict key `vk_format`; BC7 needs the KTX CLI — guard with skip if absent). Add a BC7 reader or assert via the CLI's output header:
```python
import shutil, subprocess, tempfile
@unittest.skipUnless(shutil.which("ktx"), "KTX-Software CLI not on PATH")
def test_bc7_orm_is_linear_unorm(self):
    import batch_cook
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "thing.orm.png"
        _make_rgb_image((64, 64)).save(src)            # existing helper returns a PIL image
        out = Path(td) / "thing.orm.ktx2"
        batch_cook._cook_one_bc7(src, out, ktx_tool="ktx", txm_size=0, preset="orm")
        # ktx info reports the vkFormat; BC7 UNORM = 145, BC7 SRGB = 146.
        info = subprocess.run(["ktx", "info", str(out)], capture_output=True, text=True).stdout
        self.assertIn("BC7_UNORM", info)
        self.assertNotIn("BC7_SRGB", info)
```
- [ ] **Step 2: run, verify fail.** Run: `py -3 -m unittest tools.mc2texcook.tests.test_mc2texcook -k bc7_orm -v` → FAIL (TypeError: no `preset` param, or SRGB output).
- [ ] **Step 3: make `_cook_one_bc7` slot-aware** (`batch_cook.py:52`):
```python
def _cook_one_bc7(src_path, dst_path, ktx_tool, txm_size, gen_mips=True, preset="albedo"):
    ...
    linear = preset in ("normal", "orm", "mask")
    create_fmt = "R8G8B8A8_UNORM" if linear else "R8G8B8A8_SRGB"
    assign_tf  = "linear" if linear else "srgb"
    create_step = [ktx_tool, "create", "--encode", "uastc",
                   "--format", create_fmt, "--assign-tf", assign_tf]
```
Thread `preset` from the `--bc7` CLI branch (`batch_cook.py:~187`) so `_cook_one_bc7(..., preset=args.preset)`.
- [ ] **Step 4: run, verify pass.** `py -3 -m unittest tools.mc2texcook.tests.test_mc2texcook -v` (existing albedo BC7 test, if any, still SRGB; the new ORM test passes or skips if no CLI).
- [ ] **Step 5: commit**
```bash
git add tools/mc2texcook/batch_cook.py tools/mc2texcook/tests/test_mc2texcook.py
git commit -m "feat(cook): slot-aware BC7 — linear UNORM for orm/normal/mask"
```

> Note: the uncompressed RGBA8 ORM-linear path is **already** correct + tested (`TestOrmPreset.test_vk_format_unorm`). This task fixes only the `--bc7` path.

---

## Task B2: Validator fixture (real schema)
**Files:** Create `tests/fixtures/assets/orm_runtime/manifest.json` (copy the SHAPE of `tests/fixtures/assets/material_validation_pass.json` — `assetId`/`source`/`kind`/`materials`/`lods`/`textureRefs`/`capabilities`; `mips` must be a non-bool int; do not invent `schemaVersion`/`pbr` flat keys).

- [ ] **Step 1:** author the fixture by copying `material_validation_pass.json` and ensuring it has an albedo + an `orm` (colorSpace `linear`) textureRef and `hasTangents` consistent with no normal map (ORM needs no tangents).
- [ ] **Step 2: verify PASS.** Run: `py -3 tools/validate_asset_manifest.py tests/fixtures/assets/orm_runtime/manifest.json` → exit 0. (Validator enforces `orm`→`linear`; it does **not** assert vkFormat==145.)
- [ ] **Step 3:** add a README noting regeneration via the real directory-based CLI:
```
py -3 tools/mc2texcook/batch_cook.py --src <dir> --dst <dir> --preset orm --bc7
```
- [ ] **Step 4: commit**
```bash
git add tests/fixtures/assets/orm_runtime/
git commit -m "test(staticprop): ORM runtime manifest fixture (validator pass)"
```

---

## Task C1: Per-bucket ORM array + sidecar feed (the core fix)
**Files:** `gos_static_prop_batcher.cpp` — storage near `s_bucketArrays` (`:621`); build inside the per-bucket build loop (`:2664-2685`); teardown (`:825-834`); material row (`:3144-3153`); helpers.

- [ ] **Step 1: storage + format helper + sidecar-name helper.**
```cpp
static std::vector<GLuint> s_ormBucketArrays;   // parallel to s_bucketArrays; [b] aligns to albedo bucket b
static GLenum bptcInternalFormatFor(bool isSrgb) {
    return isSrgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM;
}
// Mirror txmmgr.cpp:3449-3468: strip the last extension AFTER the last path
// separator, append ".orm.ktx2". getTextureName() returns the SOURCE path
// (.tga/.txm), never .ktx2 — so we strip whatever ext is there.
static std::string deriveOrmSidecar(const char* srcName) {
    std::string s(srcName ? srcName : "");
    size_t lastSep = s.find_last_of("/\\");
    size_t dot     = s.find_last_of('.');
    if (dot != std::string::npos && (lastSep == std::string::npos || dot > lastSep))
        s.erase(dot);
    return s + ".orm.ktx2";
}
```
- [ ] **Step 2: build `s_ormBucketArrays[b]` inside the albedo bucket build** (`:2664-2685`). For each bucket `b`, allocate an ORM `GL_TEXTURE_2D_ARRAY` with the **same dims and layerCount** as the albedo bucket. For each albedo unique at bucket-relative layer `k` (dims W×H), derive its ORM sidecar, `ktxLoadRgba8` it; if present **and dims == W×H**, upload to ORM layer `k` via `bptcInternalFormatFor(ormImg.isSrgb /*=false for ORM*/)` (BC7) or `GL_RGBA8`; else upload a **1×1-replicated neutral texel (255,255,255,255)** into layer `k` so the array stays dense and bindable. Copy the albedo sampler-param block verbatim (`GL_CLAMP_TO_EDGE`, `GL_LINEAR_MIPMAP_LINEAR`, `GL_TEXTURE_MAX_LEVEL`) and enforce consistent mipCount (fall back to neutral on mismatch). Record per-bucket-layer "had real ORM" in a `std::vector<std::vector<bool>> s_ormLayerHasMap` for the material-row flag.
- [ ] **Step 3: material row** (`:3144-3153`): keep `metallicRoughnessTex == albedoTex` (the layer) and set the flag only when the layer had a real map:
```cpp
    m.albedoTex = static_cast<uint32_t>(layer);
    if (s_ormSlotsEnabled && bucketLayerHasOrm(bucketIdx, layer)) {
        m.metallicRoughnessTex = static_cast<uint32_t>(layer);   // SAME index space as albedoTex
        m.flags               |= RenderCore::MaterialFlags::kMetallicRoughness;
    } else {
        m.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
    }
```
- [ ] **Step 4: teardown.** In `staticPropReleaseBuckets()` (`:825`):
```cpp
    for (GLuint a : s_ormBucketArrays) if (a) glDeleteTextures(1, &a);
    s_ormBucketArrays.clear();
    s_ormLayerHasMap.clear();
```
- [ ] **Step 5: verify gate-OFF byte-identity** (real two-capture diff — `--verify` is only a determinism check, do NOT use it for OFF-vs-ON):
```bash
MC2_STATICPROP_MATERIAL_PBR_SLOTS=0 py -3 scripts/capture_baseline.py --preset staticprop_baseline_02   # -> off.png
MC2_STATICPROP_MATERIAL_PBR_SLOTS=1 py -3 scripts/capture_baseline.py --preset staticprop_baseline_02   # -> on.png  (no sidecars on stock props)
py -3 -c "from PIL import Image,ImageChops,sys; d=ImageChops.difference(Image.open('off.png').convert('RGB'),Image.open('on.png').convert('RGB')); print('bbox',d.getbbox()); sys.exit(0 if d.getbbox() is None else 1)"
```
Expected: `bbox None` (identical) — stock props have no `.orm.ktx2`, all `kMaterialTexAbsent`, shader unchanged (Task D not yet landed → ORM never sampled here anyway). Use the deterministic camera preset; if the OS-screenshot path shows sub-pixel noise, capture the offscreen target instead (see `capture_baseline.py` header) and document the tolerance.
- [ ] **Step 6: commit**
```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(staticprop): per-bucket linear ORM array + sidecar feed (layer-aligned)"
```

---

## Task C2: Bind ORM array at ALL albedo bind-sites
**Files:** `gos_static_prop_batcher.cpp` — uniform-loc cache (`~:1011-1028`), every albedo bind site (`:5347,5511,5606,5648,...`).

- [ ] **Step 1: constant + loc cache.**
```cpp
static constexpr GLuint kOrmTexUnit = 1;   // unit 0 = albedo; no other sampler bound in this pass
// in coalesce loc cache:
s_locsCoalesce.ormTexArr       = glGetUniformLocation(prog, "u_ormTexArr");
s_locsCoalesce.ormSampleEnable = glGetUniformLocation(prog, "u_ormSampleEnable"); // int
```
- [ ] **Step 2: at the bucket-multidraw bind (`:5606`)** bind ORM in lockstep and restore active unit:
```cpp
if (s_locsCoalesce.ormTexArr >= 0 && b < s_ormBucketArrays.size() && s_ormBucketArrays[b]) {
    glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
    glUniform1i(s_locsCoalesce.ormTexArr, kOrmTexUnit);
    glActiveTexture(GL_TEXTURE0);   // restore so the next albedo bind lands on unit 0
}
```
- [ ] **Step 3: force the enable uniform** once per program use: `glUniform1i(s_locsCoalesce.ormSampleEnable, s_ormSlotsEnabled ? 1 : 0);` In **non-bucket-multidraw** draw paths that do NOT bind an ORM array, upload `u_ormSampleEnable=0` so a stale unit is never sampled (review MAJOR-8).
- [ ] **Step 4: epilogue** — unbind the ORM unit next to the unit-0 restore (`:5681-5685`):
```cpp
glActiveTexture(GL_TEXTURE0 + kOrmTexUnit); glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
glActiveTexture(GL_TEXTURE0);
```
- [ ] **Step 5: verify** no new GL error gate-ON (fixture present):
```bash
MC2_STATICPROP_MATERIAL_PBR_SLOTS=1 py -3 scripts/run_smoke.py --mission mc2_10 --keep-logs   # exit 0, no GL error in static-prop pass
```
- [ ] **Step 6: commit**
```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(staticprop): bind ORM array at all albedo bind-sites + enable uniform"
```

---

## Task D1: Shader — sample ORM roughness/metallic × scalars (gated, compile-guarded)
**Files:** `shaders/static_prop.frag` — decls after `:96`; PBR scalar read `:346-352`; debug reads `:233-234`.

- [ ] **Step 1: declarations** under `MC2_COALESCE`, gated by the compile-define (review MAJOR-9):
```glsl
#ifdef MC2_STATICPROP_PBR_SLOTS
uniform sampler2DArray u_ormTexArr;
uniform int            u_ormSampleEnable;  // int (uniform-uint crash trap); 0 = byte-identical
uniform float          u_ormStrength;      // tuning; 1.0 = full map
#endif
```
- [ ] **Step 2: multiply ORM into the specular scalars** (`:346-352`), inside the existing `MC2_USE_VIEW_UNIFORMS`+`sunFound` block, fully compile-guarded:
```glsl
                if (u_materialGpuSample != 0) {
                    metallic  = materialTable_.materials[materialIdx].metallicFactor;
                    roughness = materialTable_.materials[materialIdx].roughnessFactor;
                }
#if defined(MC2_STATICPROP_PBR_SLOTS) && defined(MC2_COALESCE)
                if (u_ormSampleEnable != 0) {
                    uint ormTex = materialTable_.materials[materialIdx].metallicRoughnessTex;
                    if (ormTex != kMatTexAbsent) {
                        vec3 orm = texture(u_ormTexArr, vec3(uvSampled, float(ormTex))).rgb;
                        roughness = mix(roughness, roughness * orm.g, u_ormStrength);
                        metallic  = mix(metallic,  metallic  * orm.b, u_ormStrength);
                    }
                }
#endif
```
- [ ] **Step 3: make debug-5/6 reflect the map** (review MAJOR-6) so AC3's stated verification is true (`:233-234`): apply the same `orm.g/.b` multiply to `dbgRoughness`/`dbgMetallic` under the same compile+enable guard.
- [ ] **Step 4: add the compile-define** in `loadProgramsIfNeeded` (`:896-924`) to BOTH prefixes when `s_ormSlotsEnabled`:
```cpp
if (s_ormSlotsEnabled) { legacyPrefix += "#define MC2_STATICPROP_PBR_SLOTS 1\n";
                         coalescePrefix += "#define MC2_STATICPROP_PBR_SLOTS 1\n"; }
```
(Legacy lane has no `materialTable_`, so its `MC2_STATICPROP_PBR_SLOTS && MC2_COALESCE` guard is false → no effect there. Documented: legacy non-coalesce lane does not sample ORM.)
- [ ] **Step 5: regenerate frag goldens** (correct invocation):
```bash
py -3 tools/shader_reflect/reflect.py --update --shader shaders/static_prop.frag
```
Inspect: only `static_prop.frag__coalesce*` gain the three `u_orm*` uniforms; MaterialGpu offsets/stride unchanged (CONTRACT invariants not tripped — uniforms aren't struct fields).
- [ ] **Step 6: verify** — gate-OFF identity (rerun C1.5 PIL diff: still `bbox None` since `u_ormSampleEnable=0` forced and block compiled out when gate off) AND gate-ON debug-5 shows ORM roughness on the fixture prop:
```bash
MC2_STATICPROP_MATERIAL_PBR_SLOTS=1 MC2_STATIC_PROP_PBR_V1=1 MC2_STATIC_PROP_DEBUG_MATERIAL=5 \
  py -3 scripts/run_smoke.py --mission mc2_10 --duration 30 --keep-logs
```
Expected: exit 0; fixture prop's roughness view varies with its ORM.g. (Note: roughness/metallic only affect sun-lit fragments — `v_pbrV1SunFound!=0` — and `MC2_VIEW_UNIFORMS=0` compiles the block out.)
- [ ] **Step 7: commit**
```bash
git add shaders/static_prop.frag GameOS/gameos/gos_static_prop_batcher.cpp tools/shader_reflect/expected/
git commit -m "feat(staticprop): sample ORM roughness/metallic x scalars (gated, compile-guarded)"
```

---

## Task E1: ORM strength knob + soak
**Files:** `data/visual_tuning.json`, `visual_tuning_profile.cpp` (mirror `staticPropIblStrength` `:160-177`), `gos_static_prop_batcher.cpp` (global+setter+loc+upload mirroring `g_iblShStrength`).

- [ ] **Step 1:** add `"staticPropOrmStrength": 1.0` to `data/visual_tuning.json` (unknown keys are ignored elsewhere — safe).
- [ ] **Step 2:** load it in `visual_tuning_profile.cpp` next to `staticPropIblStrength`; add a `g_ormStrength` global + env override `MC2_STATICPROP_ORM_STRENGTH` (allowlisted in A1); cache `s_locsCoalesce.ormStrength`; upload `u_ormStrength` (forced 1.0 baseline; 0.0 when gate off → no-op).
- [ ] **Step 3: soak** mc2_10 + tier1:
```bash
MC2_STATICPROP_MATERIAL_PBR_SLOTS=1 py -3 scripts/capture_baseline.py --preset staticprop_baseline_02
py -3 scripts/run_smoke.py --tier tier1 --keep-logs    # 5/5
```
Expected: tier1 5/5; ORM visible on fixture; stock props unchanged. Default-ON decision is a separate follow-on, not this arc.
- [ ] **Step 4: commit**
```bash
git add data/visual_tuning.json GameOS/gameos/visual_tuning_profile.cpp GameOS/gameos/gos_static_prop_batcher.cpp shaders/static_prop.frag tools/shader_reflect/expected/
git commit -m "feat(staticprop): ORM strength tuning knob + mc2_10 soak"
```

---

## Deferred to follow-on `STATICPROP-ORM-AO-1`
AO (`.r`) on ambient/IBL. Requires the review's correct design: forward an ambient-only
varying from the vert; re-fold `v_ambientIbl * ao` into `litRgb` **before** the alpha-test
`max(litRgb,0.5)` clamp and the `tex_color * v_argb.a` multiply, outside the `MC2_COALESCE`
guard; init `v_ambientIbl=vec3(0)` at top of vert `main()`; verify byte-identity with
**IBL/ambient ENABLED** (the v1 blind spot). Visual no-op until ambient/IBL strength>0.

## Self-review vs spec (AO items deferred)
- AC1 (gate-OFF identical): C1.5/D1.6 PIL two-capture diff (correct method). ✓
- AC2 (gate-ON no map identical): C1.5 (no sidecars) + D1 compile-guard + `u_ormSampleEnable=0`/`kMatTexAbsent` branch. ✓
- AC3 (gate-ON with map): D1.2 specular + D1.3 debug-5/6 now reflect the map. ✓
- AC4 (linear): `bptcInternalFormatFor(false)` + cook UNORM (B1). ✓
- AC5 (no ABI churn): only frag goldens regen; MaterialGpu invariants untouched. ✓
- AC6 (cook linear ORM): B1+B2. ✓  AC7 (gates green): D1.5 + firewall (no scope-dir edits). ✓  AC8 (smoke): C2.5/D1.6/E1.3. ✓
- **AC-AO items: explicitly DEFERRED** (documented above), per review BLOCKER-4.

## Execution handoff
Plan v2 complete. Two execution options when you're ready: **(1) Subagent-Driven** (fresh subagent per task, review between) or **(2) Inline** (executing-plans, checkpoints). Which?
