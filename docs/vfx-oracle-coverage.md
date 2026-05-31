# VFX Oracle Coverage Matrix

Last updated: 2026-05-30

Branch: `claude/vfx-original-class-coverage-1`

## Class x Oracle Status

Class counts from `mc2srcdata/effects/mc2.fx` (920,265 bytes, version 17, 904 specs)
via aligned 4-byte ClassID scan.

| Class | ClassID | Spec count | Oracle status | Spawned in tier1? | Best validation mission | Notes |
|---|---|---|---|---|---|---|
| CardCloud | 1318 | 388 | Full oracle | Yes (~30/frame in mc2_10) | mc2_10, mc2_24 | Workhorse; spin+aspect+atlas wired (VFX-ORIGINAL-RENDER-ANIM-FIELDS-1) |
| ShardCloud | 1316 | 24 | Full oracle | Yes (combat) | mc2_24 | Fixed atlasIndex=0u; spin+aspect wired |
| Card | 1322 | 136 | Full oracle | Yes (combat, Flare) | mc2_10, mc2_24 | m_animated wired; uvRect sub-rect wired |
| PointCloud | 1314 | 2 | Oracle added (VFX-POINTCLOUD-ORACLE-HARVEST-1) | No — active=0 in tier1 (Missile_Flare exhausts in <1s) | User-driven session with missile fire | No spin/aspect/atlas; position+color only; size constant 4.0f |
| Tube | 1324 | 31 | Profile oracle added (VFX-TUBE-PROFILE-ORACLE-1) | No — no combat in 30s smoke; FX_FlameTrail confirmed ClassID 1324 | User-driven session with flamer mech firing | One billboard per active spine profile; real swept mesh is B2 polish debt; FX_FlameTrail + FX_SmokeTrail are Tube class |
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
| mc2_10 | CardCloud active (~6/frame dust/smoke), Card (Flare) | CardCloud, Card |
| mc2_17 | VFX prohibited | none |
| mc2_24 | Trail-heavy (emit_total ~900K/30s), EffectCloud children | CardCloud, Card, ShardCloud |

Note: `vfx=prohibited` in `[VISIBILITY v1]` refers to the legacy gosFX display path being
blocked. The CPU oracle path (CardCloud/ShardCloud/Card/PointCloud/Tube `Draw()`) runs
independently via gosFX lifecycle and IS called even when `vfx=prohibited`; the oracle
harvests the CPU sim state and emits to the GPU billboard batcher directly.

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

1. **PointCloud visual validation**: user-driven session with missile fire (Missile_Flare is the only PointCloud spec with active births; need missile launch to see particles).
2. **Tube visual validation**: user-driven session with flamer mech firing (FX_FlameTrail, FX_SmokeTrail).
3. **age/lifetime upload** (Tier 1): wire `GpuParticle.age` and `.lifetime` from CPU oracle so shader alpha fade works.
4. **ShapeCloud/DebrisCloud/Shape**: 3D mesh GPU primitives — separate arc, different substrate needed.
