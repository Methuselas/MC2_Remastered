# Adversarial Review — Static-Prop ORM (runtime) plan

Review of `2026-06-02-staticprop-orm-now.md` by a 4-agent adversarial panel
(byte-identity, GL/shader, C++/batcher, cook/test/process lenses). Verdict:
**plan v1 would fail at execution** — two architectural blockers + a byte-identity
trio + every verification command wrong. Plan **v2** (`...-v2.md`) folds in all fixes.

## Severity-ranked findings

### BLOCKER-1 — ORM array/layer coordinate space (GL #1 + C++ #2, independent)
The albedo "array" is actually **N arrays** `s_bucketArrays[b]`, partitioned by
`(alphaGroup, dim)` (`gos_static_prop_batcher.cpp:2635-2699`). `albedoTex`/`texArrayLayer`
is **bucket-relative**; the draw loop rebinds `s_bucketArrays[b]` to the sampler per bucket
(`:5606-5610`); material rows dedup by `(bucketIdx<<32)|layer` (`:3136-3139`). Plan v1 built
a single flat ORM array and a flat layer → `metallicRoughnessTex` is in the wrong index
space → shader samples the wrong/out-of-range ORM layer. **Fix:** build `s_ormBucketArrays`
with the **identical (group,dim) partition**, one ORM array per `b`, ORM layer *k*
aligned to the same unique as albedo layer *k*, so the invariant is
`metallicRoughnessTex == albedoTex`. Bind `s_ormBucketArrays[b]` in lockstep at `:5606`.
Fill no-sidecar layers with a neutral 1.0 texel (dense array) but keep per-material
`kMaterialTexAbsent` so the shader's absent-guard still no-ops. Delete the flat helpers.

### BLOCKER-2 — uvScale aliasing (GL #2)
`uvSampled = fract(v_uv)*vec2(uvScaleX,uvScaleY)` (`static_prop.frag:181`); in the non-BC7
mixed-size lane uvScale<1 encodes the albedo sub-region (`:3022`). Reusing it for an ORM map
of different dims samples the wrong sub-rect. **Fix:** hard-constrain **ORM dims == albedo
dims** per unique; reject mismatches (treat as absent + log), mirroring the albedo
`dim_mismatch` discipline (`:2369-2371`). Document the constraint in the spec.

### BLOCKER-3 — sidecar derivation operates on the wrong string (C++ #3)
`mcTextureManager->getTextureName(nodeIdx)` returns the **full source path with its original
extension** (`.tga`/`.txm`), set via `strcpy(nodeName, textureFullPathName)`
(`txmmgr.cpp:3122-3125`, `txmmgr.h:563`). It is **never** `.ktx2`. Plan v1's
`.ktx2`→`.orm.ktx2` swap matches nothing → feed dead on arrival. **Fix:** `deriveOrmSidecar`
must strip the last extension *after the last path separator* and append `.orm.ktx2`,
mirroring the engine's own `.ktx2` derivation (`txmmgr.cpp:3449-3468`, guards `dot > lastSep`).
The raw-`fopen` feed via `ktxLoadRgba8` (`KtxLoader.cpp:55`) IS viable once the name is right.

### BLOCKER-4 — D2 AO breaks byte-identity three ways (byte-identity #1, GL #7)
(a) Vert pre-sum `ambientIbl = ambient + ibl` then one `lit += ambientIbl` **reassociates**
floats vs today's two separate `+=` (`vert:355,364`) → non-identical when IBL>0.
(b) Frag `c.rgb += tex*v_ambientIbl*ao` is added **after** `c = tex*vec4(litRgb,a)`
(`frag:311`), bypassing the alpha-test `max(litRgb,0.5)` clamp (`:308`) and the `v_argb.a`
multiply, and distributing the float differently → breaks AC2 when ambient/IBL>0.
(c) The `#define` is added to `legacyPrefix` too (D2.3) but the re-add was inside
`#ifdef MC2_COALESCE` → the legacy lane **loses ambient/IBL entirely** when the gate is on.
All three are masked because every verify ran ambient/IBL=0. **Fix:** re-fold
`v_ambientIbl*ao` into `litRgb` **before** the alpha clamp and `tex*v_argb.a` multiply,
outside the `MC2_COALESCE` guard; make the vert `#else` reproduce the two original `+=`
verbatim; init `v_ambientIbl=vec3(0)` at top of `main()` (window/early-return paths leave it
unwritten → UB, process #4 M-3). **Recommendation: DEFER AO entirely from v1** — it is a
visual no-op until ambient/IBL strength>0 and is the sole source of these blockers. Ship
roughness/metallic (D1) now; AO as a follow-on with the re-fold design.

### BLOCKER-5 — every verification command is wrong (cook/process #4)
- Capture preset `staticprop_mc2_10` **does not exist**; mc2_10 static-prop preset is
  `staticprop_baseline_02` (`tests/visual/baselines/presets.json:13-18`).
- `capture_baseline.py --verify` re-runs the **same** capture twice (determinism check), and
  the OS-screenshot path is documented non-deterministic (`:528-530`). It does **not** compare
  gate-OFF vs gate-ON. AC1 needs an explicit two-capture PNG diff, not `--verify`.
- `reflect.py` selects files via `--shader PATH` (append), **no positional**
  (`reflect.py:680-710`). v1's `reflect.py --update shaders/...` errors.
- Cook test helpers `_make_rgb_png`/`_read_ktx2_header`/`_ktx_tool` **don't exist**; the suite
  is `unittest`, operates on uncompressed RGBA8, and **already** tests ORM-linear
  (`TestOrmPreset.test_vk_format_unorm`, `:220-227`). v1's BC7 145/146 assertion is in the
  wrong universe.
- Validator fixture (B2) has the wrong schema — needs `assetId`/`source`/`materials`/`lods`/
  `textureRefs`/`capabilities`, and `mips` must be a non-bool int (`validate_asset_manifest.py:260-285,172`).
  "Expected exit 0" was false. **Fix:** all corrected in v2; B1 BC7-linear test rewritten or
  dropped (RGBA8-linear already covered); B2 rebuilt from `material_validation_pass.json`.

### MAJOR-6 — D1 effect scope + debug views (GL #3)
The roughness/metallic multiply sits inside `#if defined(MC2_USE_VIEW_UNIFORMS)` and runs
only when `u_pbrV1Strength>0 && v_pbrV1SunFound!=0` (`frag:314,336-337`). Debug modes 5/6 read
scalars from a **different** block (`:230-234`) that v1 never multiplies → AC3 "modes 5/6
respond" is false. **Fix:** also multiply ORM into the debug-5/6 reads; document that
`MC2_VIEW_UNIFORMS=0` compiles the roughness/metallic path out (AO, if kept, is outside that
guard — asymmetry).

### MAJOR-7 — ORM array teardown missing (C++ #3)
`s_ormBucketArrays` owns GL handles but v1 never deletes them; `staticPropReleaseBuckets()`
(`:825-834`) handles albedo. → one `GL_TEXTURE_2D_ARRAY` leaked per ORM bucket per rebuild.
**Fix:** add the delete+clear loop to `staticPropReleaseBuckets()`.

### MAJOR-8 — ORM bound at only one draw bind-site (C++ #3)
Albedo binds at multiple sites (`:5347,5511,5606,5648,...`); v1 patched only `:5606`. Gate-ON
on any other path leaves `u_ormTexArr` bound to a stale texture with `u_ormSampleEnable=1`.
**Fix:** bind ORM in every albedo bind-site, or force `u_ormSampleEnable=0` outside the
bucket-multidraw path.

### MAJOR-9 — D1 not under the compile-define (C++ #5)
v1's frag ORM block keys off `u_ormSampleEnable` only, not `#ifdef MC2_STATICPROP_PBR_SLOTS`
→ violates the dual-interlock (compile-guard + runtime-force-zero). **Fix:** wrap the D1 block
in the define too.

### MINOR
- `kOrmTexUnit` undefined; no active-unit restore after bind; epilogue (`:5681`) won't unbind
  the ORM unit (GL #4). Define unit 1; reset active to 0; unbind in epilogue.
- ORM sampler params/mip-count unspecified (GL #6). Copy albedo's `CLAMP_TO_EDGE`+trilinear
  block; enforce consistent mipCount per ORM array; fall back to absent on mismatch.
- Undefined helper placeholders (`appendOrmLayer`, `ormLayerForUnique`, `ormLayerForMaterial`)
  — replaced by the per-bucket build in v2 (C++ #5).
- E1 over-scoped into one step (process M-2) — split in v2.
- B2 README cook CLI is directory-based `--src/--dst`, no `--out` (process M-1) — fixed.
- Legacy non-coalesce lane is ORM-less — documented (acceptable; ORM is coalesce-era).
- C3 uniform-loc anchor `:984-986` is off; real block `:1011-1028` (process #4).

## Confirmed sound (no change needed)
No `MaterialGpu` ABI churn (slots/flags/sentinel exist, reflect MaterialGpu invariants
untouched); gate-fully-OFF dual interlock; linear-format choice `bptcInternalFormatFor(false)`
+ cook UNORM; `ktxLoadRgba8` raw-fopen feed (no FastFile dependency); `run_smoke.py` env
allowlist is enforced (A1.2 load-bearing); `visual_tuning.json` ignores unknown keys; engine
builds after A1 alone; D2 cleanly cuttable (D1/D3/E1 don't depend on its varying).

## Net recommendation
1. **Re-architect the ORM array as a per-bucket sibling of `s_bucketArrays`** with the
   `metallicRoughnessTex == albedoTex` invariant (BLOCKER-1/2). This is the load-bearing fix.
2. **Fix the sidecar derivation** to strip the real source extension (BLOCKER-3).
3. **Cut AO from v1** (BLOCKER-4); ship roughness/metallic only. Re-introduce AO later with
   re-fold-before-clamp.
4. **Correct all verification commands** (BLOCKER-5).
5. Add teardown, all bind-sites, compile-guard, sampler params (MAJOR-7/8/9, MINORs).

v2 plan applies 1–5.
