# VFX Oracle Coverage Matrix

Last updated: 2026-05-30 (VFX-ORIGINAL-LIVE-EFFECT-VALIDATION-1)

Branch: `claude/vfx-original-class-coverage-1`

## Class x Oracle Status

Class counts from `mc2srcdata/effects/mc2.fx` (920,265 bytes, version 17, 904 specs)
via aligned 4-byte ClassID scan.

| Class | ClassID | Spec count | Oracle status | Spawned in tier1? | Best validation mission | Notes |
|---|---|---|---|---|---|---|
| CardCloud | 1318 | 388 | Full oracle | Yes (~30/frame in mc2_10) | mc2_10, mc2_24 | Workhorse; spin+aspect+atlas wired (VFX-ORIGINAL-RENDER-ANIM-FIELDS-1) |
| ShardCloud | 1316 | 24 | Full oracle | Yes (combat) | mc2_24 | Fixed atlasIndex=0u; spin+aspect wired |
| Card | 1322 | 136 | Full oracle | Yes (combat, Flare) | mc2_10, mc2_24 | m_animated wired; uvRect sub-rect wired |
| PointCloud | 1314 | 2 | Oracle added (VFX-POINTCLOUD-ORACLE-HARVEST-1) | No — active=0 in tier1 mc2_10+mc2_24 (Missile_Flare births=0 in passive 30s smoke) | User-driven session with missile fire | No spin/aspect/atlas; position+color only; size constant 4.0f |
| Tube | 1324 | 31 | Profile oracle added (VFX-TUBE-PROFILE-ORACLE-1) | Yes — fires in mc2_24 passive smoke (PPC trail spec="core", harvestedTotal=4685+ at 30s). alpha=[0.000,0.000] throughout — profiles exist but are fully faded in passive smoke. | User-driven session with flamer mech firing for visible alpha | One billboard per active spine profile; real swept mesh is B2 polish debt; FX_FlameTrail + FX_SmokeTrail are Tube class |
| EffectCloud | 1320 | 39 | Composite container only | Yes (wraps CardCloud/ShardCloud/Card children) | mc2_10, mc2_24 | No direct GPU path; children handled individually by their own oracle |
| PertCloud | 1317 | 0 | Not present | No | N/A | 0 specs in mc2.fx; skip |
| ShapeCloud | 1319 | 9 | No GPU path | Unknown | Unknown | 3D mesh per particle; requires separate GPU primitive (not billboard) |
| Shape | 1323 | 10 | No GPU path | Unknown | Unknown | 3D singleton mesh |
| DebrisCloud | 1325 | 34 | No GPU path | Unknown | Unknown | Rigid-body 3D mesh pieces |
| PointLight | 1326 | 0 | Not present | No | N/A | 0 specs in mc2.fx; skip |

## Gate summary

- `MC2_VFX_ORACLE_RENDER=1` enables oracle render for all migrated classes (inside `MC2_GPU_PARTICLES=1` check).
- `MC2_GPU_PARTICLES_LOG=1` enables `[VFX_ORACLE v1]` per-class stderr output.
- Gate-OFF (no env vars) is byte-identical for all oracle classes.

## What fires in automated smoke (30s, no combat, camera pan only)

| Mission | VFX activity | Oracle classes confirmed |
|---|---|---|
| mc2_01 | VFX prohibited | none |
| mc2_03 | VFX prohibited (gosFX Draw called but emit=0, no combat) | none |
| mc2_10 | CardCloud active (harvestedTotal=31748+ over 30s, 1-6/call dust/smoke), ShardCloud (harvestedTotal=8334+), Card (Flare, 1 particle alpha=0.000 — born but faded) | CardCloud, ShardCloud, Card |
| mc2_17 | VFX prohibited | none |
| mc2_24 | Trail-heavy (emit_total ~850K/30s), EffectCloud children, PPC trail profiles | CardCloud, Card, ShardCloud, Tube |

Note: `vfx=prohibited` in `[VISIBILITY v1]` refers to the legacy gosFX display path being
blocked. The CPU oracle path (CardCloud/ShardCloud/Card/PointCloud/Tube `Draw()`) runs
independently via gosFX lifecycle and IS called even when `vfx=prohibited`; the oracle
harvests the CPU sim state and emits to the GPU billboard batcher directly.

### VFX-ORIGINAL-LIVE-EFFECT-VALIDATION-1 results (2026-05-30, passive 30s smoke)

Build deployed to `mc2-win64-v0.4`. Gates: `MC2_VFX_ORACLE_RENDER=1 MC2_GPU_PARTICLES_LOG=1`.

**mc2_10** (PASS, +0 destroys, 0 GL errors):
- CardCloud: FIRST_HARVEST fires (spec="Dust"), harvestedTotal≈31748+ at 30s, 1–6 particles/call, alpha varies 0..1. CONFIRMED working.
- ShardCloud: FIRST_HARVEST fires (spec="Shards"), harvestedTotal≈8334+ at 30s, 10/call. CONFIRMED working.
- Card: FIRST_HARVEST fires (spec="Flare"), alpha=0.000, 1 particle only — born but faded. No subsequent periodic calls. CONFIRMED oracle triggers but particle is invisible.
- PointCloud: 0 lines. No births in passive smoke. NEEDS INTERACTIVE.
- Tube: 0 lines. No Tube effects in mc2_10.

**mc2_24** (PASS, +0 destroys, 0 GL errors):
- CardCloud: FIRST_HARVEST fires (spec="Fireball"), harvestedTotal≈7176+ at 30s. CONFIRMED working.
- ShardCloud: FIRST_HARVEST fires (spec="Shards0008"), harvestedTotal≈8334+ at 30s. CONFIRMED working.
- Card: FIRST_HARVEST fires (spec="lozenge"), alpha=1.000. CONFIRMED working (but no periodic calls — single particle).
- Tube: FIRST_HARVEST fires (spec="core", PPC trail), harvestedTotal=4685+ at 30s, 2–7 profiles/call. alpha=[0.000,0.000] throughout. Oracle harvests but visually invisible in passive smoke.
- PointCloud: 0 lines. No births in passive smoke. NEEDS INTERACTIVE.

## Missing fields (deferred to follow-up arcs)

### Tier 1 — billboard representable now

- **age/lifetime upload**: `GpuParticle.lifetime` and `.age` stay 0 (not set by oracle). Shader alpha fade via age is not available yet. Deferred to VFX-AGE-LIFETIME-UPLOAD-1.
- **PointCloud size**: constant 4.0f world units. Spec has no per-particle size curve; no per-spec override without ABI bump.
- **Tube profile size**: `m_pScale` used as billboard radius. May not match tube width at all profile ages without spec tuning.

### Tier 2 — require struct or shader changes

- **PointCloud/Tube spin/aspect**: neither class has these concepts; skip.
- **Tube swept mesh**: one billboard per profile is an approximation. Real tube ribbon needs a separate GPU primitive (triangle strip per pair of adjacent profiles). Filed as B2 polish debt in `docs/vfx-originals-restoration-design.md`.

### Tier 3 — separate GPU primitive

- **ShapeCloud**: per-particle 3D mesh renders. No billboard path applicable.
- **DebrisCloud**: rigid-body 3D pieces. No billboard path applicable.
- **Shape**: 3D singleton mesh. No billboard path applicable.

## Next targets

1. **PointCloud visual validation**: user-driven session with missile fire (Missile_Flare is the only PointCloud spec with active births; confirmed births=0 in 30s passive smoke on mc2_10+mc2_24; need missile launch event).
2. **Tube alpha=0 investigation**: Tube oracle fires in mc2_24 passive smoke (harvestedTotal=4685+ at 30s) but alpha is 0.000 throughout. PPC trail "core" profiles are harvested but fully transparent. Possible causes: (a) PPC trails are GPU-owned/suppressed and alpha=0 is intentional suppression signal, (b) trail alpha depends on flight phase not present in 30s passive. User-driven session with flamer mech firing needed to see non-zero alpha (FX_FlameTrail, FX_SmokeTrail are Tube class).
3. **age/lifetime upload** (Tier 1): wire `GpuParticle.age` and `.lifetime` from CPU oracle so shader alpha fade works.
4. **ShapeCloud/DebrisCloud/Shape**: 3D mesh GPU primitives — separate arc, different substrate needed.
