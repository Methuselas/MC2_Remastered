# VFX-BLACKBODY-1

Gated, default-OFF LDR blackbody (temperature -> color) emissive tint for the
existing additive VFX particle path. Gives hot fire/plume/impact a "modern feel"
ramp (warm-white -> orange) with a tiny shader + material change. No HDR/bloom
dependency (that path is DELETED) — this is plain LDR multiply on the additive
emissive color.

## What it does

For **additive/emissive** particle groups only (explosions, impacts, muzzle,
plume), when the gate is ON, the fragment shader multiplies the already-computed
additive emissive color by an analytic blackbody (Planckian-locus) RGB derived
from the particle's own emissive brightness. Hotter (brighter) fragments read
whiter; cooler (dimmer) fragments read deeper orange — the way real
incandescent fire/impact light shifts with temperature.

The **alpha particle path** (smoke/dust), mech materials, decals, and the
`emissiveTex` material slot are **not touched**.

## The analytic blackbody function (AUTHORED, not imported)

`vec3 blackbodyRGB(float tempK)` in `shaders/particle_billboard.frag`.

This is an MC2-native, in-house closed-form approximation of the blackbody
(Planckian) chromaticity curve mapped to linear RGB, then normalized so the
brightest channel == 1.0 (a pure hue tint; the caller supplies intensity). It is:

- **NOT** a copied table, **NOT** a sampled LUT, **NOT** any third-party shader's
  code. It is a hand-tuned rational/exponential fit authored for this slice.
- Red: a saturating rational curve that rises fast and clamps, with a floor so a
  "hot" tint never loses its red component.
- Green: a logarithmic climb with temperature (cooler -> less green).
- Blue: a linear ramp that stays near zero while cool/orange and climbs in toward
  white at high temperature.

Verified qualitative shape (normalized RGB):

| Temp  | R    | G    | B    | reads as          |
|-------|------|------|------|-------------------|
| 1200K | 1.00 | 0.45 | 0.00 | deep red-orange   |
| 2000K | 1.00 | 0.48 | 0.00 | orange            |
| 3500K | 1.00 | 0.66 | 0.19 | warm white-orange |
| 5500K | 1.00 | 0.83 | 0.48 | near warm-white   |
| 6000K | 1.00 | 0.86 | 0.55 | faintly cool white |

Technique inspired by BattleTech's `VFX_Blackbody` concept, but all code here is
clean-room / written from scratch — no copied shader code, no imported
LUT/noise/texture assets.

## Temperature-derivation choice

v1 derives temperature **procedurally in-shader** from existing per-particle data —
**no new per-particle data field, no new art**:

```
luma  = dot(outColor.rgb, vec3(0.299, 0.587, 0.114))   // emissive brightness
tempK = mix(1200.0, 6000.0, clamp(luma, 0.0, 1.0))      // ~1200K..6000K hot range
```

A brighter (hotter) emissive fragment maps to a higher temperature -> whiter
tint; a dimmer one maps lower -> deeper orange. This reuses the additive group's
own per-particle color (already computed from the SSBO record + atlas texel +
head-brighten), so there is no ABI change and no new uniform feeding per-particle
heat.

## The gate

`MC2_VFX_BLACKBODY` — **default OFF** (`envFlagDefaultOff` pattern, mirrors
`MC2_VFX_LIT_PARTICLES` / `MC2_VFX_SOFT_PARTICLES`).

- Plumbed via int uniform `u_vfxBlackbody` from `gos_particle_bridge.cpp`, exactly
  like `u_vfxIsAdditive`.
- Gate OFF: the bridge uploads `u_vfxBlackbody = 0` once per flush and never
  raises it. The FS tint branch is guarded by `u_vfxBlackbody == 1` so it is
  dead code when OFF -> the additive path is **byte-identical to today** (no
  uniform effect, no math).
- Gate ON: the per-group draw loop raises `u_vfxBlackbody = 1` **only** for
  additive groups (`grp.blendMode == 1`). Alpha groups always get 0.
- The FS branch is additionally guarded `&& u_vfxIsAdditive == 1 && u_debugMode == 0`
  so it can never affect alpha groups or the debug visualization views.

## Files changed

- `shaders/particle_billboard.frag` — `u_vfxBlackbody` uniform + `blackbodyRGB()`
  function + the gated additive-only tint branch.
- `GameOS/gameos/gos_particle_bridge.cpp` — gate (`MC2_VFX_BLACKBODY`,
  `s_blackbody_enabled` + `vfxBlackbodyInitIfNeeded()`), `u_vfxBlackbody`
  location lookup, default-0 upload per flush, and per-group `=1` for additive
  groups when the gate is ON.
- `docs/VFX-BLACKBODY-1.md` — this doc.

## Validation (AMD RX 7900 XTX, RelWithDebInfo)

- **Build:** `mc2` + `mc2_launcher` RelWithDebInfo, clean (only pre-existing
  benign C4267/C4838/LNK4199 warnings). Deployed ISOLATED to
  `A:/Games/mc2-opengl/mc2-win64-0.4c` via `deploy_payload.py` (shaders in
  lockstep). No running instance was killed.
- **Shader compile:** clean — no `[SHADER WARN]` / compile error in the deployed
  run log; the particle program linked (mc2_24 rendered 1944 frames; a broken
  hot-reload would keep the old shader and log).
- **gate-OFF byte-identical:** tier1 (`mc2_01 mc2_03 mc2_10 mc2_17 mc2_24`)
  **PASS 5/5** with `MC2_VFX_BLACKBODY` unset. The OFF branch is provably dead
  (uniform == 0, shader branch skipped).
- **gate-ON clean:** `mc2_24` with `MC2_VFX_BLACKBODY=1` + `MC2_GL_DEBUG_FATAL=1`
  **PASS**, no GL errors (FATAL would abort on `GL_DEBUG_SEVERITY_HIGH`).
- **Additive blend not collapsed:** `scripts/check-vfx-blend-distinction.py`
  **PASS** (VfxBillboardAdditive == AdditiveSrcAlphaOne, distinct from alpha).
- **Honest visual-proof status:** the 30s headless auto-play of mc2_24 did not
  emit an additive particle group in-window (no `[PIPELINE_BIND] VfxBillboard*`
  row fired), so an actual on-screen explosion tinted by blackbody was **not**
  captured in this run. Pixel-exact ON proof is intrinsically nondeterministic
  (particle RNG) and is **not** claimed. The function's analytic ramp was
  verified numerically (table above); the additive path it tints is proven LIVE
  by the recon + blend-distinction check; gate-ON renders clean with no GL
  errors.

## Ledger

```yaml
VFX_BLACKBODY:
  status: BUILT_VISUAL_ORACLE_CONFIRMED
  gate_default: OFF
  gate_off: BYTE_IDENTICAL
  color: ANALYTIC_PLANCKIAN_NO_LUT
  target: ADDITIVE_EMISSIVE_PARTICLES
  hdr_bloom_dependency: NONE_DELETED
  pixel_exact_proof: BUILT_UNPROVEN_particle_RNG
```
