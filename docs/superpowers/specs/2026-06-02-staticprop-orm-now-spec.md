# Static-Prop ORM (runtime) — Spec

**Slice family:** "ORM now" = recon slices 1–4 of
`docs/staticprop-material-orm-normal-recon.md`. Normal mapping, tangents, and emissive are
explicitly **deferred** (their own later arc).

**Branch basis:** `claude/nifty-mendeleev`.

---

## Problem

Static props can only vary albedo. Roughness and metallic are global scalars
(`MaterialGpu.metallicFactor/roughnessFactor`, default 0.0/1.0) and there is no ambient
occlusion. Authored ORM maps (R=AO, G=roughness, B=metallic) exist in the manifest/validator
vocabulary but are never loaded or sampled at runtime — `metallicRoughnessTex` is always
`kMaterialTexAbsent` and the shader never reads it.

## What this delivers

Runtime sampling of an authored **ORM** texture per static-prop material, behind a
default-OFF gate, with byte-identical behavior for stock/mod props that have no ORM map:

1. A per-usage **linear** texture array for ORM (parallel to the albedo array), built by the
   existing BC7/RGBA8 bucket machinery with the format chosen from the source KTX colorspace.
2. A per-type **source feed** so a material can name its ORM texture, populating
   `MaterialGpu.metallicRoughnessTex` + the `kMetallicRoughness` flag.
3. Gated shader consumption in `static_prop.frag`:
   - **roughness/metallic**: sampled `metallicRoughnessTex.g/.b` **multiplied** by the scalar
     `roughnessFactor/metallicFactor`, feeding the existing per-fragment sun-specular block.
   - **AO** (`.r`): multiplies **only** the indirect/ambient terms (hemisphere ambient +
     SH-L2 IBL). Never direct `calc_light` diffuse, never raw albedo.
4. A soak/tuning pass in `mc2_10` with `visual_tuning.json` knobs, ending in a default-ON
   recommendation (the flip itself is out of scope for this arc).

## Acceptance criteria

- **AC1 — Gate OFF byte-identical.** With `MC2_STATICPROP_MATERIAL_PBR_SLOTS` unset/0, the
  rendered output and GL state are identical to pre-change (verified by
  `scripts/capture_baseline.py --verify` on the soak preset). The new shader block is both
  compile-guarded and runtime-forced to a no-op.
- **AC2 — Gate ON, no ORM map = no visual change.** A material with
  `metallicRoughnessTex == kMaterialTexAbsent` renders identically to gate-OFF: AO=1,
  roughness=`roughnessFactor`, metallic=`metallicFactor` (scalar-only path, the current PBR-V1
  behavior). Old mods and stock props are unaffected.
- **AC3 — Gate ON, with ORM map = correct sampling.** A fixture prop with an authored ORM
  map shows: roughness/metallic driven by G/B (× scalars) in the sun-specular response, and
  AO from R darkening only ambient/IBL. Verified by the `MC2_STATIC_PROP_DEBUG_MATERIAL`
  roughness(5)/metallic(6) views plus a new AO debug view.
- **AC4 — Linear colorspace.** The ORM array is uploaded with a **linear** internalformat
  (UNORM BC7 / RGBA8), never sRGB. Verified by the array build choosing format from
  `KtxImage.isSrgb == false`.
- **AC5 — No ABI/contract regressions.** `MaterialGpu` struct, offsets, GLSL mirror,
  `scripts/check-material-gpu-mirror.sh`, and `tools/shader_reflect` MaterialGpu invariants
  are unchanged. Only `static_prop.frag` reflect goldens change (new sampler + sampling),
  regenerated and committed.
- **AC6 — Cook produces linear ORM.** `tools/mc2texcook` emits a linear ORM `.ktx2` (BC7
  UNORM or RGBA8) via a slot-aware path; `tools/validate_asset_manifest.py` passes the ORM
  fixture (orm slot, colorSpace=linear).
- **AC7 — Unit/contract gates green.** `mc2_tests` builds and runs; new shader goldens match;
  `check-material-gpu-mirror.sh` and the firewall stay clean.
- **AC8 — Smoke.** `tier1` (esp. `mc2_10`) passes gate-OFF and gate-ON-with-fixture; no new
  `glGetError` in the static-prop pass.

## Out of scope (deferred)

- Normal mapping, tangent generation, per-fragment diffuse move (the split-granularity arc).
- Emissive sampling.
- Flipping the gate default-ON (a soak outcome, separate change).
- Mech/terrain materials.
- Any change to `MaterialGpu` layout or the asset-manifest schema.

## Key constraints

- `uniform uint` crashes this engine's shader_builder — declare any new uint-typed uniform as
  `int` (see `memory/uniform_uint_crash.md`).
- Gate must use the `MC2_STATIC_PROP_PBR_V1` dual-interlock pattern (compile-guard +
  runtime-force-zero) so OFF is byte-identical.
- Reconcile the pre-existing fallback mismatch: frag literal `roughness=0.6` vs table default
  `roughnessFactor=1.0`. Spec choice: the scalar-only path keeps current PBR-V1 behavior;
  this arc does **not** change the existing default to avoid an unrelated visual delta —
  document the mismatch, do not "fix" it inside this arc unless a test forces it.
- New texture array reuses the existing bucket machinery; it must **not** force ORM into the
  sRGB albedo array.

## QA / soak

Primary mission `mc2_10` (urban/buildings/windows). Harness `scripts/capture_baseline.py`
(deterministic camera) + `mc2-render-state` MCP. Add `staticPropAoStrength` /
`staticPropOrmStrength` to `data/visual_tuning.json` (mirrors `staticPropIblStrength`). Add
the gate to the `run_smoke.py` env allowlist.
