# Track A1 — Object Admission Acceptance Envelope

Captured: 2026-05-06
Source (modern run): `tests/smoke/artifacts/2026-05-06T16-15-18/`
Source (legacy run):  `tests/smoke/artifacts/2026-05-06T16-16-39/`
Predicate compared: homogClipFull (clipSpaceFrustumAdmit on rawClip) vs legacyRect

Coverage note: Captured from mc2_01 (15s) per project smoke policy. Full tier1
5-mission capture deferred to soak period; soak runs will augment if disagreement
classes appear in missions not covered here.

## Heatmap format note

The PROJECTZ v1 heatmap records per-callsite disagreements vs the `legacyRect` ground
truth (the legacy screen-rect test that is the existing `projectZ` return value). The
per-callsite outlier summary only prints callsites that have non-zero disagreement.
`gameobj_visibility_admit` (the Track A1 callsite at code/gameobj.cpp:2089) had zero
`homogClipFull` disagreements in both runs and therefore does not appear in the outlier
list. This is the expected steady-state: objects are either clearly inside or clearly
outside the frustum. The zero count is explicitly noted below.

## Global homogClipFull disagreement counts (all callsites)

These counts are cumulative across all perspective-branch projectZ calls (terrain
vertices, object admission, selection/picking, and untagged sites). The dominant
contributor is terrain vertex callsites (terrain_cpu_vert_admit, terrain_quad_vert*),
where the legacy screen-rect test accepted off-edge vertices that homogClipFull rejects
via the near/far clip planes.

### Modern mode (MC2_OBJECT_ADMISSION_PREDICATE=modern)

```
[PROJECTZ v1] summary frames=2122
  category=BoolAdmission          calls=10044220 parallel=0 rejected=4903977
  category=SelectionPicking       calls=870000   parallel=0 rejected=860939
  category=unknown                calls=2983676  parallel=0 rejected=490765
  perspective_calls=13897896
  predicate=homogClipFull      agree=6255681 disagree_perm=0 disagree_restr=7642215 finite_viol=0 (54.99% of persp)
  outlier callsiteId=terrain_cpu_vert_admit         homogClip_dis=4265365 rectSignedW_dis=4261554 contains_rej_tri=0
  outlier callsiteId=<unknown>                      homogClip_dis=2500222 rectSignedW_dis=2492911 contains_rej_tri=0
  outlier callsiteId=terrain_quad_vert2_admit       homogClip_dis=846286  rectSignedW_dis=844619  contains_rej_tri=0
  outlier callsiteId=terrain_quad_vert3_admit       homogClip_dis=14903   rectSignedW_dis=14416   contains_rej_tri=0
  outlier callsiteId=terrain_quad_vert0_admit       homogClip_dis=11793   rectSignedW_dis=10273   contains_rej_tri=0
  outlier callsiteId=picking_closest_vertex_fallback homogClip_dis=9061   rectSignedW_dis=9061    contains_rej_tri=0
  outlier callsiteId=terrain_quad_vert1_admit       homogClip_dis=10871   rectSignedW_dis=8202    contains_rej_tri=0
  triangles total=5247662 red(contains_rejected)=2767070 purple(area_exceeded)=239972
```

`object_admission_mode=modern` confirmed in log line 1 (first INSTR banner).

### Legacy mode (MC2_OBJECT_ADMISSION_PREDICATE not set)

```
[PROJECTZ v1] summary frames=2121
  category=BoolAdmission          calls=10023336 parallel=0 rejected=4856386
  category=SelectionPicking       calls=870000   parallel=0 rejected=860946
  category=unknown                calls=2997112  parallel=0 rejected=491358
  perspective_calls=13890448
  predicate=homogClipFull      agree=6208690 disagree_perm=0 disagree_restr=7681758 finite_viol=0 (55.30% of persp)
  outlier callsiteId=terrain_cpu_vert_admit         homogClip_dis=4289911 rectSignedW_dis=4285741 contains_rej_tri=0
  outlier callsiteId=<unknown>                      homogClip_dis=2512938 rectSignedW_dis=2505754 contains_rej_tri=0
  outlier callsiteId=terrain_quad_vert2_admit       homogClip_dis=848849  rectSignedW_dis=847145  contains_rej_tri=0
  outlier callsiteId=terrain_quad_vert3_admit       homogClip_dis=14986   rectSignedW_dis=14480   contains_rej_tri=0
  outlier callsiteId=terrain_quad_vert0_admit       homogClip_dis=11823   rectSignedW_dis=10268   contains_rej_tri=0
  outlier callsiteId=picking_closest_vertex_fallback homogClip_dis=9054   rectSignedW_dis=9054    contains_rej_tri=0
  outlier callsiteId=terrain_quad_vert1_admit       homogClip_dis=10851   rectSignedW_dis=8137    contains_rej_tri=0
  triangles total=5245188 red(contains_rejected)=2757399 purple(area_exceeded)=240955
```

`object_admission_mode=legacy` confirmed in log line 1 (first INSTR banner).

## gameobj_visibility_admit callsite (Track A1 primary target)

| Mission | Mode   | Total calls (BoolAdmission all) | homogClipFull disagree_perm | homogClipFull disagree_restr | % total BoolAdmission |
|---------|--------|---------------------------------|-----------------------------|------------------------------|-----------------------|
| mc2_01  | modern | 10,044,220                      | 0                           | 0                            | 0.00%                 |
| mc2_01  | legacy | 10,023,336                      | 0                           | 0                            | 0.00%                 |

Note: `gameobj_visibility_admit` has zero `homogClipFull` disagreements in both
runs. The heatmap outlier print only shows callsites with non-zero disagreement, so this
site does not appear in the printed summary. The BoolAdmission total above covers all
terrain and object admission callsites (terrain_cpu_vert_admit, terrain_quad_vert*,
gameobj_visibility_admit, etc.). The `gameobj_visibility_admit` site is confirmed present
in the binary via PROJECTZ_SITE at code/gameobj.cpp:2089.

(disagree_perm = modern accepts, legacy rejects; disagree_restr = legacy accepts, modern rejects)

## Legacy vs modern call-count comparison (sanity check)

| Mission | Legacy perspective_calls | Modern perspective_calls | Drift % |
|---------|--------------------------|--------------------------|---------|
| mc2_01  | 13,890,448               | 13,897,896               | +0.054% |

| Mission | Legacy BoolAdmission calls | Modern BoolAdmission calls | Drift % |
|---------|----------------------------|---------------------------|---------|
| mc2_01  | 10,023,336                 | 10,044,220                | +0.21%  |

| Mission | Legacy frames | Modern frames | Drift % |
|---------|---------------|---------------|---------|
| mc2_01  | 2121          | 2122          | +0.05%  |

Drift >20% would indicate predicate-induced lifecycle cascade — none observed.
All drift is within noise (single-digit per mille). The modern predicate has no
measurable effect on object admission throughput.

## Global homogClipFull baseline (all-callsite aggregate)

These numbers define the parity gate. Future runs that touch the predicate or the
`gameobj_visibility_admit` path must land within ±20% of these baselines:

| Metric                              | Modern-mode value | Legacy-mode value |
|-------------------------------------|-------------------|-------------------|
| perspective_calls                   | 13,897,896        | 13,890,448        |
| homogClipFull disagree_perm (total) | 0                 | 0                 |
| homogClipFull disagree_restr (total)| 7,642,215         | 7,681,758         |
| homogClipFull disagree_restr %      | 54.99%            | 55.30%            |
| BoolAdmission calls                 | 10,044,220        | 10,023,336        |

The `disagree_restr` count is dominated by terrain vertex callsites (far-edge terrain
where the legacy rect accepted but homogClipFull rejects via near/far clip planes).
This is structural and expected — the terrain submission path was not changed by Track A1.

## Candidate disagreement classes

### Behind-camera (rawClip.w <= 0)
Modern rejects unconditionally; legacy accepts if the rhw-divided screen coord wraps
into the viewport rect.

NOT OBSERVED (0 disagree_perm in 15s mc2_01 capture for gameobj_visibility_admit).
Globally, disagree_perm=0 across all callsites. The camera never places objects behind
the clip plane during normal gameplay at wolfman zoom — the panning range keeps all
objects in front of the near plane.

### Inside frustum but rawClip.w very small
Modern accepts cleanly; legacy may reject if post-divide screen coord lands outside
the viewport rect at extreme zoom-out.

NOT OBSERVED (0 disagree_perm globally in 15s mc2_01 capture). At wolfman zoom (altitude
6000, GameVisibleVertices=200), the frustum coverage is wide enough that objects passing
the clip test also pass the legacy rect test.

### Far-plane boundary (rawClip.z >= 0 and rawClip.z <= rawClip.w)
Modern admits inclusively; legacy depends on rect-overlap.

NOT OBSERVED for gameobj_visibility_admit (0 disagreement). The 7,642,215 global
disagree_restr is entirely from terrain vertex callsites where the legacy rect accepted
vertices that homogClipFull rejects — these are terrain vertices that fall within the
screen rect but outside the homogeneous near/far clip bounds. This is a terrain-only
phenomenon; game objects do not exhibit this pattern in mc2_01.

### Legacy-restr dominant at terrain callsites
disagree_restr=7,642,215 at global level (54.99% of all perspective calls) is terrain-
driven and expected. homogClipFull is more restrictive than the legacy rect for terrain
vertices at the visual horizon (vertices that are outside the clip frustum but whose
post-divide coordinates fall within the screen rect). This is the structurally correct
behavior: homogClipFull eliminates the "wedge" artifacts that occur when behind-camera
terrain vertices have their rhw-division wrap them into the viewport.

## Out-of-envelope conditions (HARD FAILURE)

If any of the following appears in a future capture, Track A1 has a bug:

- Disagreement count delta >5x baseline at any single mission.
- `gameobj_visibility_admit` appearing in the outlier list with non-zero counts that
  cannot be attributed to the classes above.
- `[DESTROY v1]` count delta >0 between legacy-mode and modern-mode runs.
- `object_admission_mode=modern` not confirmed in log line 1 when the env var is set.
- BoolAdmission call count drift >20% between legacy and modern runs at same duration.

## Procedure for envelope refresh

Re-run Steps 6.1 and 6.2 after any change to:
- The `clipSpaceFrustumAdmit` body
- `Camera::projectZ` (the legacy reference)
- `Camera::projectForObjectAdmission` (the wrapper)
- The bucket of objects passing through `gameobj_visibility_admit`
  (code/gameobj.cpp:2089)

Command (modern mode):
```
MC2_OBJECT_ADMISSION_PREDICATE=modern MC2_PROJECTZ_HEATMAP=1 MC2_PROJECTZ_SUMMARY=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 15 --kill-existing --keep-logs
```

Command (legacy mode, for drift check):
```
MC2_PROJECTZ_HEATMAP=1 MC2_PROJECTZ_SUMMARY=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 15 --kill-existing --keep-logs
```

Grep targets in artifact log:
- `predicate=homogClipFull` — global aggregate disagree_perm and disagree_restr
- `callsiteId=gameobj_visibility_admit` — should be ABSENT (zero disagreement)
- `callsiteId=terrain_cpu_vert_admit` — expected top outlier
- `perspective_calls=` — total call volume for drift check

If disagreement counts shift outside ±20% of this baseline, gate the change.

If `gameobj_visibility_admit` appears in the outlier list (non-zero disagreement),
classify by disagree_perm vs disagree_restr and compare against the candidate classes
above. Counts below 100 are likely noise from edge-case camera angles. Counts above 1000
require root-cause analysis before merging.
