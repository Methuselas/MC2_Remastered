# Track B — TGL Pool Baseline Measurements

**Date:** 2026-05-06  
**Env:** `MC2_STATIC_PROP_MISSION_LOAD_REG=1 MC2_STATIC_PROP_LATE_SPAWN_REG=1`  
**Duration:** 60s per mission, tier1 (5/5 PASS)  
**Alert threshold:** 80% of current capacity

## Pool peaks by mission

| Mission | vertex (500K) | color (500K) | face (200K) | shadow (500K) | triangle (200K) |
|---------|--------------|-------------|------------|--------------|----------------|
| mc2_01  | 9973 (1%)    | 9973 (1%)   | 18320 (9%) | 9973 (1%)    | 9160 (4%)      |
| mc2_03  | 15564 (3%)   | 15564 (3%)  | 36110 (18%)| 15564 (3%)   | 18055 (9%)     |
| mc2_10  | 22073 (4%)   | 22073 (4%)  | 44022 (22%)| 22073 (4%)   | 22011 (11%)    |
| mc2_17  | 44215 (8%)   | 44215 (8%)  | 89606 (44%)| 44215 (8%)   | 44803 (22%)    |
| mc2_24  | 24502 (4%)   | 24502 (4%)  | 65388 (32%)| 24502 (4%)   | 32694 (16%)    |

**Worst case:** mc2_17, face pool at 44% (89606/200000)

## Verdict

All pools below 80% threshold across all tier1 missions. **No pool bump required.**

Pool sizes remain:
- vertex/color/shadow: 500K
- face/triangle: 200K

If a future mission exceeds 80%: bump to 1M (vertex/color/shadow) or 500K (face/triangle).  
User has confirmed pools can safely go to 5M if needed.

## Notes

- `MC2_STATIC_PROP_MISSION_LOAD_REG=1` enables the mission-load registration walk
- The `TransformMultiShape_BuildRecipe` path (introduced in Task 5) skips pool allocation during walk — pool peaks are from normal per-frame rendering only
- mc2_17 is the heaviest mission in tier1 (most terrain objects visible)
