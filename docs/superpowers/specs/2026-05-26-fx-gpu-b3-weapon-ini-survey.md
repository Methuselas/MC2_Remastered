# FX-GPU-1 B3b-3 Weapon INI Survey

Survey of bolt-type weapon definitions in `mc2srcdata/objects/` for GPU trail kind mapping.
Source files: `*.fit` in `mc2srcdata/objects/` and `mc2srcdata/objects/effects.csv`.

## .fit File Survey

Weapon definition format: each `.fit` file under `mc2srcdata/objects/` defines one weapon projectile type.
The `[BoltProjectileData]` section marks it as a projectile (vs `[ProjectileLaserData]` = hit-scan).
The `GosEffectId` field (when present) names the gosFX trail effect from `effects.csv`.

| FIT file     | Section              | GosEffectId    | Projectile | Notes |
|--------------|----------------------|----------------|------------|-------|
| nerppc.fit   | BoltProjectileData   | ppc_trail      | Y          | ER PPC (non-clan) |
| nppc.fit     | BoltProjectileData   | ppc_trail      | Y          | nPPC (novel PPC variant) |
| erppc.fit    | BoltProjectileData   | (none: "NONE") | Y          | ER PPC (clan) -- no CPU trail |
| ppc.fit      | ProjectileLaserData  | (n/a)          | N (hitscan)| Classic PPC laser bolt |
| lrm.fit      | BoltProjectileData   | lrm_trail      | Y          | LRM missile |
| srm.fit      | BoltProjectileData   | (none: 0)      | Y          | SRM missile |
| swarmlrm.fit | BoltProjectileData   | (none: 0)      | Y          | Swarm LRM |
| thunderbolt.fit | BoltProjectileData| (none: 0)      | Y          | Thunderbolt missile |
| gauss.fit    | BoltProjectileData   | (none: 0)      | Y          | Gauss slug |
| gauss_hvy.fit| BoltProjectileData   | (none: 0)      | Y          | Heavy Gauss |
| gauss_lt.fit | BoltProjectileData   | (none: 0)      | Y          | Light Gauss |
| ac.fit       | BoltProjectileData   | (none: 0)      | Y          | AC (base) |
| ac5.fit      | BoltProjectileData   | (none: 0)      | Y          | AC/5 |
| ac10.fit     | BoltProjectileData   | (none: 0)      | Y          | AC/10 |
| ac20.fit     | BoltProjectileData   | (none: 0)      | Y          | AC/20 |
| lbxhvy.fit   | BoltProjectileData   | (none: 0)      | Y          | LBX (heavy) |
| lbxmed.fit   | BoltProjectileData   | (none: 0)      | Y          | LBX (medium) |
| lbxlt.fit    | BoltProjectileData   | (none: 0)      | Y          | LBX (light) |
| lbxhvy_clan.fit | BoltProjectileData| (none: 0)     | Y          | Clan LBX heavy |
| lbxmed_clan.fit | BoltProjectileData| (none: 0)     | Y          | Clan LBX medium |
| lbxlt_clan.fit  | BoltProjectileData| (none: 0)     | Y          | Clan LBX light |
| mg.fit       | BoltProjectileData   | (none: 0)      | Y          | Machine gun |
| snipercannon.fit | BoltProjectileData| (none: 0)     | Y          | Sniper cannon |
| flamer.fit   | BoltProjectileData   | lrm_trail      | Y          | Flamer bolt |
| plaser.fit   | BoltProjectileData   | (none: "NONE") | Y          | Pulse laser bolt |
| laser.fit    | BoltProjectileData   | (none: "NONE") | Y          | Laser bolt |
| longtom.fit  | BoltProjectileData   | (none: 0)      | Y          | Long Tom |

## effects.csv Mapping (weapon trail effect names)

The `effects.csv` maps `effectId` -> `effectName` (the gosFX trail). WeaponBolt::init uses
`weaponEffects->GetEffectName(effectId)` to retrieve the trail name at runtime.

| effectId | effectName       | Weapon label          | GpuTrailKind assigned |
|----------|------------------|-----------------------|-----------------------|
| 4        | ppc_trail        | PPC                   | PpcBolt               |
| 5        | ppc_trail        | ER PPC                | PpcBolt               |
| 8        | srm_trail        | SRM                   | MissileSmoke          |
| 9        | lrm_trail        | LRM                   | MissileSmoke          |
| 74       | Swarm_lrm_trail  | Swarm LRM             | MissileSmoke          |
| 27       | TBolt_trail      | Thunderbolt           | MissileSmoke          |
| 3        | flamer_trail     | Flamer                | MissileSmoke          |
| 1,2      | *_ClanER_hit     | Clan ER Laser         | None (hit-scan)       |
| 7        | AC_5_Trail       | AC 5                  | None (no smoke trail) |
| 10       | gauss_trail      | Gauss                 | None                  |
| 11       | NONE             | Machine Gun           | None                  |
| 12-17    | *_las_hit etc    | Various lasers        | None (hit-scan)       |
| 18,19    | ac_10/20_Trail   | AC 10/20              | None                  |
| 28       | Light_gauss_trail| Light Gauss           | None                  |
| 31       | long_tom_trail   | Long Tom              | None                  |

## Mapping implementation

In `code/weaponbolt.cpp`, helper `gpuTrailKindFromEffectId()` maps `effectId` -> `GpuTrailKind`
via `weaponEffects->GetEffectName(effectId)` string comparison. Unmapped weapons return
`GpuTrailKind::None` (CPU gosFX path unchanged, zero regression).

## Smoke verification

Tier1 run with `MC2_GPU_PARTICLES=1` after B3b-3 deploy:

| Mission | trail_spawn | trail_head | result |
|---------|-------------|------------|--------|
| mc2_01  | 0           | 0          | PASS   |
| mc2_03  | 0           | 0          | PASS   |
| mc2_10  | 406665      | 0          | PASS   |
| mc2_17  | 0           | 0          | PASS   |
| mc2_24  | 469177      | 147315     | PASS   |

`trail_head=147315` on mc2_24 confirms PpcBolt head sprites firing correctly.
No overflow events on any mission.
