# Static-Prop ORM (runtime) Implementation Plan

> ⚠️ **SUPERSEDED by `2026-06-02-staticprop-orm-now-v2.md`** after adversarial review
> (`...-REVIEW.md`). v1 has architectural blockers (flat ORM array vs per-bucket; dead
> sidecar derivation; AO byte-identity breaks; broken verification commands). Kept for
> the review trail only — **do not execute v1.**

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sample an authored ORM map (R=AO, G=roughness, B=metallic) per static-prop material at runtime, behind a default-OFF gate, with byte-identical behavior for props that lack one.

**Architecture:** A parallel **linear** texture array (built by the existing BC7/RGBA8 bucket machinery, format chosen from `KtxImage.isSrgb`) holds ORM maps discovered as `<albedo>.orm.ktx2` sidecars. The material-table row gains the ORM layer index + `kMetallicRoughness` flag. The fragment shader multiplies sampled G/B into the existing per-fragment sun-specular roughness/metallic, and applies sampled AO (`.r`) to a newly-forwarded ambient/IBL varying. Everything is gated by `MC2_STATICPROP_MATERIAL_PBR_SLOTS` (compile-guard + runtime-force-zero, the `MC2_STATIC_PROP_PBR_V1` pattern), so gate-OFF is bit-identical.

**Tech Stack:** C++17 (GameOS static-prop batcher), GLSL 430 (`static_prop.{vert,frag}`), `RenderCore::ktxLoadRgba8`, Python cook (`tools/mc2texcook`), doctest (`mc2_tests`), `tools/shader_reflect` goldens, `scripts/capture_baseline.py` + `scripts/run_smoke.py`.

**Spec:** `docs/superpowers/specs/2026-06-02-staticprop-orm-now-spec.md`. **Recon:** `docs/staticprop-material-orm-normal-recon.md`.

**Testing reality:** the GL paths (array build, shader) have **no GL-free unit harness** — they are verified by reflect-golden regen (contract), `capture_baseline.py --verify` (pixel-invariance), and `run_smoke.py` (integration). TDD with real failing-first tests applies to the **cook** (`tools/mc2texcook/tests/`) and the **manifest validator** (python). Each GL task therefore has an explicit *verification* step (golden/capture/smoke) in place of a unit test, and a mandatory gate-OFF byte-identity check.

---

## File structure

| File | Responsibility | Action |
|---|---|---|
| `GameOS/gameos/gos_static_prop_batcher.cpp` | gate parse, ORM array build, sidecar feed, material-row ORM fields, sampler bind, uniform upload | Modify |
| `shaders/static_prop.frag` | sample ORM, multiply roughness/metallic, apply AO, AO debug view | Modify |
| `shaders/static_prop.vert` | forward ambient/IBL as a separable varying (gated) | Modify |
| `tools/mc2texcook/batch_cook.py` | slot-aware BC7 cook (linear for orm/normal) | Modify |
| `tools/mc2texcook/tests/test_mc2texcook.py` | cook tests | Modify |
| `data/visual_tuning.json` | `staticPropOrmStrength` / `staticPropAoStrength` knobs | Modify |
| `GameOS/gameos/visual_tuning_profile.cpp` | load the two new knobs | Modify |
| `scripts/run_smoke.py` | allowlist the new env gate | Modify |
| `tools/shader_reflect/expected/shaders__static_prop.frag__*.json` | regenerated goldens | Modify (generated) |
| `tests/fixtures/assets/orm_runtime/` | ORM `.ktx2` + manifest fixture | Create |

**Sequencing rationale:** cook (B) and gate (A) first — they have real tests and no rendering risk. Then array+feed (C), then shader (D), then soak (E). Roughness/metallic (D1) is the value; AO (D2) is plumbing that is a visual no-op until ambient/IBL is enabled — it is isolated so review can cut it.

---

## Task A1: Add the `MC2_STATICPROP_MATERIAL_PBR_SLOTS` gate

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (near the `s_pbrV1Enabled` block at `:498-501`)
- Modify: `scripts/run_smoke.py` (env allowlist near `:528-538`)

- [ ] **Step 1: Add the gate parse** after `s_pbrV1Enabled` (`gos_static_prop_batcher.cpp:501`):

```cpp
// STATICPROP-MATERIAL-PBR-SLOTS: master gate for ORM (roughness/metallic/AO)
// texture sampling. Default-OFF; dual interlock (compile-guard in shader +
// runtime force-zero of u_ormSampleEnabled) so OFF is byte-identical.
static const bool s_ormSlotsEnabled = []() {
    const char* v = getenv("MC2_STATICPROP_MATERIAL_PBR_SLOTS");
    return v != nullptr && v[0] != '0' && v[0] != '\0';
}();
```

- [ ] **Step 2: Add the gate + its strength knobs to the smoke allowlist.** In `scripts/run_smoke.py`, find the `MC2_STATIC_PROP_PBR_V1` allowlist entries (~`:528-538`) and add alongside them:

```python
    "MC2_STATICPROP_MATERIAL_PBR_SLOTS",
    "MC2_STATICPROP_ORM_STRENGTH",
    "MC2_STATICPROP_AO_STRENGTH",
```

- [ ] **Step 3: Verify it compiles** (no behavior yet — `s_ormSlotsEnabled` is unused; expect an `-Wunused` only if `/WX`; the engine builds `/WX-`). Build the engine target per `docs/critical_inline_rules.md`. Expected: links clean.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp scripts/run_smoke.py
git commit -m "feat(staticprop): add MC2_STATICPROP_MATERIAL_PBR_SLOTS gate (no behavior)"
```

---

## Task B1: Slot-aware BC7 cook (linear for ORM)

**Files:**
- Modify: `tools/mc2texcook/batch_cook.py:52-97` (`_cook_one_bc7`)
- Test: `tools/mc2texcook/tests/test_mc2texcook.py`

- [ ] **Step 1: Write the failing test.** Add to `test_mc2texcook.py`:

```python
def test_bc7_orm_is_linear_unorm(tmp_path):
    # An ORM source cooked to BC7 must be a LINEAR (UNORM) BC7 KTX2 — never sRGB.
    src = _make_rgb_png(tmp_path / "thing.orm.png", (64, 64))  # existing helper
    out = tmp_path / "thing.orm.ktx2"
    batch_cook._cook_one_bc7(src, out, ktx_tool=_ktx_tool(), txm_size=0,
                             preset="orm")
    hdr = _read_ktx2_header(out)               # existing helper
    assert hdr.vkFormat == 145, "BC7 ORM must be VK_FORMAT_BC7_UNORM (145), not 146 (sRGB)"
```

- [ ] **Step 2: Run it, verify it fails.**

Run: `py -3 -m pytest tools/mc2texcook/tests/test_mc2texcook.py::test_bc7_orm_is_linear_unorm -v`
Expected: FAIL — `_cook_one_bc7()` has no `preset` parameter (TypeError) / output is vkFormat 146.

- [ ] **Step 3: Make `_cook_one_bc7` slot-aware.** Replace the signature and the create-step format selection:

```python
def _cook_one_bc7(src_path: Path, dst_path: Path, ktx_tool: str,
                  txm_size: int, gen_mips: bool = True,
                  preset: str = "albedo") -> tuple[int, int]:
    # ... docstring unchanged ...
    # Linear slots (normal/orm/mask) must NOT be sRGB-transcoded: cook as UNORM
    # so the GPU does no sRGB->linear on sample. Albedo/emissive stay sRGB.
    linear = preset in ("normal", "orm", "mask")
    create_fmt = "R8G8B8A8_UNORM" if linear else "R8G8B8A8_SRGB"
    assign_tf  = "linear" if linear else "srgb"
    # ... inside the temp dir, replace create_step: ...
        create_step = [ktx_tool, "create", "--encode", "uastc",
                       "--format", create_fmt, "--assign-tf", assign_tf]
```

Then thread `preset` from the `batch_cook` call sites (the per-file loop that already knows each file's slot) into `_cook_one_bc7(..., preset=slot)`.

- [ ] **Step 4: Run the test, verify it passes.**

Run: `py -3 -m pytest tools/mc2texcook/tests/test_mc2texcook.py -v`
Expected: PASS, including the existing albedo BC7 test still vkFormat 146.

- [ ] **Step 5: Commit**

```bash
git add tools/mc2texcook/batch_cook.py tools/mc2texcook/tests/test_mc2texcook.py
git commit -m "feat(cook): slot-aware BC7 — linear UNORM for orm/normal/mask"
```

---

## Task B2: ORM runtime fixture + validator pass

**Files:**
- Create: `tests/fixtures/assets/orm_runtime/manifest.json`, `tests/fixtures/assets/orm_runtime/README.md`
- Test: reuse `tools/validate_asset_manifest.py`

- [ ] **Step 1: Write the fixture manifest** referencing albedo + ORM with correct colorspaces (mirror `tests/fixtures/assets/material_validation_pass.json`):

```json
{
  "schemaVersion": 1,
  "asset": "orm_runtime_prop",
  "textureRefs": [
    { "slot": "albedo", "path": "prop.ktx2",     "colorSpace": "srgb",   "vkFormat": 146, "mips": true, "dims": [256,256] },
    { "slot": "orm",    "path": "prop.orm.ktx2", "colorSpace": "linear", "vkFormat": 145, "mips": true, "dims": [256,256] }
  ],
  "pbr": { "baseColorFactor": 1.0, "metallicFactor": 1.0, "roughnessFactor": 1.0 }
}
```

- [ ] **Step 2: Run the validator, verify PASS.**

Run: `py -3 tools/validate_asset_manifest.py tests/fixtures/assets/orm_runtime/manifest.json`
Expected: exit 0 (validator already knows `orm`→linear→145, `tools/validate_asset_manifest.py:79-85`).

- [ ] **Step 3: README documents** how to regenerate the `.ktx2` binaries (they are gitignored per `docs/asset-manifest-schema.md:8`):

```
# Regenerate (KTX-Software CLI on PATH):
py -3 tools/mc2texcook/batch_cook.py --bc7 prop.png       --preset albedo --out prop.ktx2
py -3 tools/mc2texcook/batch_cook.py --bc7 prop.orm.png   --preset orm    --out prop.orm.ktx2
```

- [ ] **Step 4: Commit**

```bash
git add tests/fixtures/assets/orm_runtime/
git commit -m "test(staticprop): ORM runtime manifest fixture (validator pass)"
```

---

## Task C1: Build a linear ORM texture array

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (buckets + array build, mirror `buildBucketArray` `:2337-2494`; format selection `:2376/2789-2795`)

**Design:** add a second array set `s_ormBucketArrays` built exactly like the albedo BC7 array, with two differences: (a) the internalformat is chosen from `KtxImage.isSrgb` (ORM is always UNORM/linear), (b) it is built only when `s_ormSlotsEnabled` and at least one ORM sidecar was found (Task C2).

- [ ] **Step 1: Add the parallel array storage** next to `s_bucketArrays` (`:613-625`):

```cpp
// STATICPROP-MATERIAL-PBR-SLOTS: parallel ORM array, linear internalformat.
// One layer per unique ORM sidecar; layer index stored in MaterialGpu.metallicRoughnessTex.
static std::vector<GLuint> s_ormBucketArrays;       // owns GL handles
static std::vector<int32_t> s_packetOrmLayer;       // per global packet; -1 = no ORM
```

- [ ] **Step 2: Add a format helper** (the load-bearing fix — never sRGB for ORM):

```cpp
// COMPRESSION-BC7-STATICPROP: pick the BPTC internalformat by colorspace.
// Albedo: SRGB (GPU does sRGB->linear). ORM/normal: UNORM (linear data).
static GLenum bptcInternalFormatFor(bool isSrgb) {
    return isSrgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
                  : GL_COMPRESSED_RGBA_BPTC_UNORM;
}
```
Then in the ORM array build (copied from `buildBucketArray`), call
`bptcInternalFormatFor(img.isSrgb)` with `img.isSrgb == false` for ORM, instead of the
hardcoded `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM` at `:2376`. RGBA8 fallback uses `GL_RGBA8`
(already linear — fine for ORM).

- [ ] **Step 3: Verify gate-OFF byte-identity.** Build engine. Run the soak preset twice and diff:

Run: `MC2_STATICPROP_MATERIAL_PBR_SLOTS=0 py -3 scripts/capture_baseline.py --preset staticprop_mc2_10 && py -3 scripts/capture_baseline.py --preset staticprop_mc2_10 --verify`
Expected: 0-pixel diff (no ORM array built when gate off / no sidecars).

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(staticprop): linear ORM texture-array scaffold (gate-OFF no-op)"
```

---

## Task C2: ORM sidecar source feed → material-table fields

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (unique-texture resolve `:2520-2617`; material-row build `:3144-3153`)

**Design:** for each unique albedo texture, derive `<albedoName>.orm.ktx2`, probe disk via `RenderCore::ktxLoadRgba8`. If present, upload into the ORM array (Task C1), record its layer, set the material row's `metallicRoughnessTex` + `kMetallicRoughness` flag. Else leave `kMaterialTexAbsent`. Sidecar-by-naming needs no manifest/ABI change.

- [ ] **Step 1: In the unique-texture loop** (`:2611` where `uniques.push_back` records the albedo), resolve the ORM sidecar name from the albedo node name and probe it:

```cpp
// STATICPROP-MATERIAL-PBR-SLOTS: probe an ORM sidecar for this unique albedo.
// Naming convention: "<albedoBase>.orm.ktx2" next to the albedo asset.
int32_t ormLayer = -1;
if (s_ormSlotsEnabled && mcTextureManager && nodeIdx != 0xFFFFFFFFu) {
    const char* albedoName = mcTextureManager->getTextureName(nodeIdx);
    if (albedoName && *albedoName) {
        std::string ormPath = deriveOrmSidecarPath(albedoName); // ".ktx2"->".orm.ktx2"
        RenderCore::KtxImage ormImg{};
        if (RenderCore::ktxLoadRgba8(ormPath.c_str(), ormImg)) {
            ormLayer = appendOrmLayer(ormImg);  // upload into s_ormBucketArrays, return layer
        }
    }
}
ormLayerForUnique.push_back(ormLayer);  // parallel to `uniques`
```
`deriveOrmSidecarPath` and `appendOrmLayer` are small new static helpers in this TU
(string swap; KTX upload mirroring the albedo BC7 upload but via `bptcInternalFormatFor(false)`).

- [ ] **Step 2: In the material-row build** (`:3144-3153`), set the ORM fields when a layer exists:

```cpp
    RenderCore::MaterialGpu m = {};
    m.albedoTex            = static_cast<uint32_t>(layer);
    m.normalTex            = RenderCore::kMaterialTexAbsent;
    const int32_t ormLayer = s_ormSlotsEnabled ? ormLayerForMaterial(globalPktIdx) : -1;
    if (ormLayer >= 0) {
        m.metallicRoughnessTex = static_cast<uint32_t>(ormLayer);
        m.flags               |= RenderCore::MaterialFlags::kMetallicRoughness;
    } else {
        m.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
    }
    m.emissiveTex          = RenderCore::kMaterialTexAbsent;
    m.baseColorFactor      = 1.0f;
    m.metallicFactor       = 0.0f;
    m.roughnessFactor      = 1.0f;
```

- [ ] **Step 3: Verify** gate-OFF still byte-identical (Step C1.3 rerun) AND gate-ON-no-sidecar byte-identical:

Run: `MC2_STATICPROP_MATERIAL_PBR_SLOTS=1 py -3 scripts/capture_baseline.py --preset staticprop_mc2_10 --verify`
Expected: 0-pixel diff vs gate-OFF (no fixture sidecars on stock props → all `kMaterialTexAbsent` → shader unchanged once Task D lands; here the shader doesn't yet read ORM, so still identical).

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(staticprop): ORM sidecar feed -> metallicRoughnessTex + flag (gated)"
```

---

## Task C3: Bind the ORM array sampler

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (per-bucket bind `:5606-5610`; uniform-location cache `:984-986`/coalesce equiv; upload near `:4779`)

- [ ] **Step 1: Cache the new sampler + sample-enable uniform locations** (coalesce program), alongside the PBR locs:

```cpp
s_locsCoalesce.ormTexArr      = glGetUniformLocation(prog, "u_ormTexArr");
s_locsCoalesce.ormSampleEnable = glGetUniformLocation(prog, "u_ormSampleEnable"); // int (uniform-uint crash trap)
```

- [ ] **Step 2: Bind the ORM array to a dedicated texture unit and upload the enable flag** (mirror the albedo bind at `:5606`, use a fresh unit e.g. `GL_TEXTURE0 + kOrmUnit`):

```cpp
if (s_locsCoalesce.ormTexArr >= 0 && b < s_ormBucketArrays.size() && s_ormBucketArrays[b] != 0) {
    glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
    glUniform1i(s_locsCoalesce.ormTexArr, kOrmTexUnit);
}
if (s_locsCoalesce.ormSampleEnable >= 0) {
    glUniform1i(s_locsCoalesce.ormSampleEnable, s_ormSlotsEnabled ? 1 : 0);  // force 0 when gate off
}
```

- [ ] **Step 3: Verify** no new `glGetError` in the static-prop pass (gate ON, fixture present), and gate-OFF byte-identity holds.

Run: `MC2_STATICPROP_MATERIAL_PBR_SLOTS=1 py -3 scripts/run_smoke.py --mission mc2_10 --keep-logs`
Expected: exit 0; logs show no GL error.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(staticprop): bind ORM array sampler + sample-enable uniform"
```

---

## Task D1: Shader — sample roughness/metallic from ORM (× scalars), gated

**Files:**
- Modify: `shaders/static_prop.frag` (declarations after `:96`; PBR block `:346-352`)

- [ ] **Step 1: Declare the ORM sampler + enable** under `MC2_COALESCE`, after `u_materialGpuSample` (`:96`):

```glsl
// STATICPROP-MATERIAL-PBR-SLOTS: ORM array (linear). R=AO G=roughness B=metallic.
uniform sampler2DArray u_ormTexArr;
uniform int            u_ormSampleEnable;  // 0 = scalar-only (byte-identical), 1 = sample ORM
```

- [ ] **Step 2: In the PBR specular block** (`:346-352`), after the scalar metallic/roughness are read from MaterialGpu, multiply by the sampled ORM G/B when enabled and present:

```glsl
                float metallic  = 0.0;
                float roughness = 0.6;
#ifdef MC2_COALESCE
                if (u_materialGpuSample != 0) {
                    metallic  = materialTable_.materials[materialIdx].metallicFactor;
                    roughness = materialTable_.materials[materialIdx].roughnessFactor;
                }
                // STATICPROP-MATERIAL-PBR-SLOTS: ORM map multiplies the scalars.
                if (u_ormSampleEnable != 0) {
                    uint ormTex = materialTable_.materials[materialIdx].metallicRoughnessTex;
                    if (ormTex != kMatTexAbsent) {
                        vec3 orm = texture(u_ormTexArr, vec3(uvSampled, float(ormTex))).rgb;
                        roughness *= orm.g;
                        metallic  *= orm.b;
                    }
                }
#endif
```

- [ ] **Step 3: Regenerate the frag reflect goldens.**

Run: `py -3 tools/shader_reflect/reflect.py --update shaders/static_prop.frag`
Then inspect the diff: only the `static_prop.frag__coalesce*` goldens gain `u_ormTexArr`/`u_ormSampleEnable`; MaterialGpu offsets unchanged.

- [ ] **Step 4: Verify** gate-OFF byte-identity (`u_ormSampleEnable=0` forced → block skipped) and gate-ON roughness/metallic debug views respond:

Run: `MC2_STATICPROP_MATERIAL_PBR_SLOTS=1 MC2_STATIC_PROP_PBR_V1=1 MC2_STATIC_PROP_DEBUG_MATERIAL=5 py -3 scripts/run_smoke.py --mission mc2_10 --duration 30 --keep-logs`
Expected: exit 0; roughness view (mode 5) shows fixture prop's roughness texture variation.

- [ ] **Step 5: Commit**

```bash
git add shaders/static_prop.frag tools/shader_reflect/expected/
git commit -m "feat(staticprop): sample ORM roughness/metallic x scalars (gated)"
```

---

## Task D2: Shader — AO on ambient/IBL (vert→frag varying forward), gated

**Files:**
- Modify: `shaders/static_prop.vert` (ambient `:342-355`, IBL `:357-365`, output `:426`)
- Modify: `shaders/static_prop.frag` (composition `:304-311`)

**Note:** ambient + IBL are summed into `v_argb` at the vertex stage and are 0 by default. To apply AO to only those terms, forward them as a separate varying `v_ambientIbl` and add them in the fragment **after** AO scaling — only under the gate, so OFF stays bit-identical. With default ambient/IBL strengths (0.0) this is a visual no-op; it wires AO for when those terms are enabled.

- [ ] **Step 1: In the vert, accumulate ambient+IBL into a separate local** instead of folding into `lit`, guarded by the new compile define. Add a varying `out vec3 v_ambientIbl;` and:

```glsl
        // STATICPROP-MATERIAL-PBR-SLOTS: keep ambient+IBL separable for per-fragment AO.
        vec3 ambientIbl = ambient_v1;           // from :353
        if (u_iblShStrength > 0.0) {
            ambientIbl += evalShL2(normalize(worldNormal)) * u_iblShStrength;  // was added to lit at :364
        }
#ifdef MC2_STATICPROP_PBR_SLOTS
        v_ambientIbl = ambientIbl;              // forwarded; NOT added to lit here
#else
        lit += ambientIbl;                      // legacy: folded in (byte-identical)
        v_ambientIbl = vec3(0.0);
#endif
```

- [ ] **Step 2: In the frag composition** (`:311`), add the AO-scaled ambient/IBL under the gate:

```glsl
    vec4 c = tex_color * vec4(litRgb, v_argb.a);
#if defined(MC2_COALESCE)
    if (u_ormSampleEnable != 0) {
        float ao = 1.0;
        uint ormTex = materialTable_.materials[materialIdx].metallicRoughnessTex;
        if (ormTex != kMatTexAbsent) ao = texture(u_ormTexArr, vec3(uvSampled, float(ormTex))).r;
        c.rgb += tex_color.rgb * v_ambientIbl * ao;   // ambient/IBL only, AO-darkened
    }
#endif
    c.rgb += v_highlight.rgb * v_highlight.a;
```

- [ ] **Step 3: Add the `#define MC2_STATICPROP_PBR_SLOTS 1`** to BOTH shader prefixes when `s_ormSlotsEnabled`, in `loadProgramsIfNeeded` (`gos_static_prop_batcher.cpp:896-924`):

```cpp
    if (s_ormSlotsEnabled) {
        legacyPrefix   += "#define MC2_STATICPROP_PBR_SLOTS 1\n";
        coalescePrefix += "#define MC2_STATICPROP_PBR_SLOTS 1\n";
    }
```

- [ ] **Step 4: Regenerate goldens** (vert gains `v_ambientIbl`; both prefixes gain a variant). Run the reflect update for both `static_prop.vert` and `.frag`; inspect diff.

- [ ] **Step 5: Verify** the critical byte-identity: with the gate OFF the `#define` is absent → the `#else` path folds ambient/IBL into `lit` exactly as today.

Run: `MC2_STATICPROP_MATERIAL_PBR_SLOTS=0 py -3 scripts/capture_baseline.py --preset staticprop_mc2_10 --verify`
Expected: 0-pixel diff. Then with gate ON and IBL enabled (`MC2_*_IBL...`), AO darkens creases on the fixture prop only.

- [ ] **Step 6: Commit**

```bash
git add shaders/static_prop.vert shaders/static_prop.frag GameOS/gameos/gos_static_prop_batcher.cpp tools/shader_reflect/expected/
git commit -m "feat(staticprop): AO on ambient/IBL via separable varying (gated)"
```

---

## Task D3: AO debug view (mode 7)

**Files:**
- Modify: `shaders/static_prop.frag` (debug block `:246-252`)

- [ ] **Step 1: Add mode 7 = AO** to the debug ladder (after mode 6 at `:251`), and update the uniform comment block (`:115-127`):

```glsl
        else if (u_debugMaterialMode == 6) dbg = vec3(dbgMetallic);
        else if (u_debugMaterialMode == 7) {                       // AO (ORM .r)
            float ao = 1.0;
#ifdef MC2_COALESCE
            if (u_ormSampleEnable != 0) {
                uint ormTex = materialTable_.materials[materialIdx].metallicRoughnessTex;
                if (ormTex != kMatTexAbsent) ao = texture(u_ormTexArr, vec3(uvSampled, float(ormTex))).r;
            }
#endif
            dbg = vec3(ao);
        }
        else                               dbg = vec3(1.0, 0.0, 1.0);
```

- [ ] **Step 2: Regenerate goldens; verify mode 7 renders AO grayscale** on the fixture (`MC2_STATIC_PROP_DEBUG_MATERIAL=7`).

- [ ] **Step 3: Commit**

```bash
git add shaders/static_prop.frag tools/shader_reflect/expected/
git commit -m "feat(staticprop): debug material view mode 7 = AO"
```

---

## Task E1: visual_tuning knobs + soak

**Files:**
- Modify: `data/visual_tuning.json`, `GameOS/gameos/visual_tuning_profile.cpp`
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (strength upload)

- [ ] **Step 1: Add the knobs** to `data/visual_tuning.json` (mirror `staticPropIblStrength`):

```json
  "staticPropOrmStrength": 1.0,
  "staticPropAoStrength": 1.0,
```

- [ ] **Step 2: Load them** in `visual_tuning_profile.cpp` next to `staticPropIblStrength`, and upload as `u_ormStrength`/`u_aoStrength` (forced to 1.0 baseline; these scale the ORM/AO effect for tuning). Wire env overrides `MC2_STATICPROP_ORM_STRENGTH`/`_AO_STRENGTH` (already allowlisted in Task A1). Apply in the frag (`roughness/metallic` lerp toward map by ormStrength; `ao = mix(1.0, ao, aoStrength)`).

- [ ] **Step 3: Soak** in `mc2_10` and capture before/after:

```bash
MC2_STATICPROP_MATERIAL_PBR_SLOTS=1 py -3 scripts/capture_baseline.py --preset staticprop_mc2_10
py -3 scripts/run_smoke.py --tier tier1 --keep-logs
```
Expected: tier1 5/5; capture shows ORM effect on fixture; stock props unchanged.

- [ ] **Step 4: Commit**

```bash
git add data/visual_tuning.json GameOS/gameos/visual_tuning_profile.cpp GameOS/gameos/gos_static_prop_batcher.cpp shaders/static_prop.frag tools/shader_reflect/expected/
git commit -m "feat(staticprop): ORM/AO tuning knobs + mc2_10 soak"
```

---

## Self-review (against the spec)

- **AC1 (gate-OFF byte-identical):** Tasks C1.3, C2.3, D1.4, D2.5 each gate-OFF `capture --verify`. The `#else` fold in D2.1 preserves vert composition; `u_ormSampleEnable=0` forced in C3.2 skips all frag sampling. ✓
- **AC2 (gate-ON, no map):** `metallicRoughnessTex==kMatTexAbsent` → D1/D2/D3 all branch to scalar/AO=1. ✓ (verified C2.3)
- **AC3 (gate-ON, with map):** D1 roughness/metallic, D2 AO, D3 debug-7. ✓
- **AC4 (linear):** `bptcInternalFormatFor(false)` (C1.2), cook UNORM (B1). ✓
- **AC5 (no ABI churn):** no MaterialGpu/mirror change; only frag/vert goldens regen (D1.3/D2.4/D3.2). ✓
- **AC6 (cook linear ORM):** B1 + B2. ✓
- **AC7 (gates green):** reflect update steps + firewall unaffected (no scope-dir edits). ✓
- **AC8 (smoke):** C3.3, D1.4, E1.3. ✓

**Open risk flagged for review:** the pre-existing `roughness=0.6` literal vs table `1.0` default is intentionally left unchanged (spec constraint); D1 multiplies the map onto whichever scalar path runs. **AO requires a vert→frag shader restructure (D2)** — the one non-trivial shader change; it is a visual no-op until ambient/IBL strength > 0, so it can be deferred without losing the roughness/metallic value if review prefers a smaller first cut.

---

## Execution handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-02-staticprop-orm-now.md`.

**Before execution:** this plan goes through adversarial review next (per the user's spec→plan→review request). Do not start implementation until review findings are folded in.
