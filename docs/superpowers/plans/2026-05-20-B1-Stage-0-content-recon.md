# B1 Stage 0' — Content Recon

Sibling artifact to `2026-05-20-integrated-gosfx-retirement-gpu-particles-plan.md`
§5.4 Stage 0'. Per-mission per-spec spawn / draw histogram captured under A2
default (gosFX still gated OFF for the rest of the game) with an opt-in
`MC2_DISABLE_GOSFX=0` set FOR THIS TRACE RUN ONLY to revive the legacy
path for histogram baseline purposes (per Stage 0' MINOR-2 fold-in).

## Capture details

- HEAD: `c5553131086e1be7464004ccccb6ade57e192afc` (post-spotlight-real arc).
- Smoke: `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs`.
- Env: `MC2_FX_TRACE=1 MC2_DISABLE_GOSFX=0`.
- Artifact dir: `tests/smoke/artifacts/2026-05-21T06-15-00/`.
- Result: PASS 5/5 (mc2_01 / mc2_03 / mc2_10 / mc2_17 / mc2_24, 4178-4271 frames/mission @ ~141 FPS avg, ~115 p1%).
- Tooling auto-fix landed alongside: `scripts/run_smoke.py` allowlist now propagates `MC2_FX_TRACE`, `MC2_DISABLE_GOSFX`, `MC2_GPU_PARTICLES` to the engine subprocess (was being silently stripped by the `subprocess.Popen env=` replacement; one-line allowlist add, no behavior change for default smoke runs).

## Histogram (legacy-gosFX baseline)

Three counter tables per `mclib/fx_trace/fx_trace.h`:

- `spawn` = `EffectLibrary::Instance->Find` invocations (one per producer-site spawn event).
- `draw` = `gosFX::Effect::Draw` entries (per-frame per-live-effect; oracle for B1 Stage 3' equivalence).
- `mlr_enqueue` = entries into the 4 gated MLR work-leaves PRE-gate (under `MC2_DISABLE_GOSFX=0` this matches the legacy load).

### mc2_01 — light combat (4233 frames)

| Counter | Total | Unique |
|---|---|---|
| spawn | 88 | 22 |
| draw | 34,245 | 46 |
| mlr_enqueue | 22,770 (all `DrawEffect`) | 1 |

Top spawn names: `Vehicle_Dust_Cloud` (23), `large_poof` (12), `Ground_Hit_Water` (8), `Jump_Jets` / `Mech_Smoking` (6 each), `Missile_flare` / `lrm_trail` / `Missile_Miss` (3 each).

Top draw names: `large_poof` (6495), `LRM_*` quadlet (4×1606), `Fireball` (1412), `Initial_Smoke` (1352), `Smoke` (1298), `AC_10_Flare` (1326), `Missile_Flare` (1071), `PPC_Hit` / `Missile_hit` / `Generic_Hit` / `Large_Explosion` / `Explosion_Handle` (~500-900).

### mc2_03 — calm patrol (4178 frames)

| Counter | Total | Unique |
|---|---|---|
| spawn | 96 | 5 |
| draw | 58 | 2 |
| mlr_enqueue | 0 | 0 |

Spawn: `large_poof` (38), `Jump_Jets` / `Mech_Smoking` / `Vehicle_Dust_Cloud` (19 each), `Steam` (1).
Draw: `Steam` (30), `SteamClouds` (28). NO mlr enqueues — these two specs do not exercise the gated work-leaves (continuous environment effects whose Draw runs but does not enter `MLRClipper::Draw*`).

### mc2_10 — heavy combat (4271 frames)

| Counter | Total | Unique |
|---|---|---|
| spawn | 225 | 17 |
| draw | 134,807 | 27 |
| mlr_enqueue | 63,972 (all `DrawEffect`) | 1 |

Top spawn: `large_poof` (45), `Jump_Jets` / `Mech_Smoking` / `Vehicle_Dust_Cloud` (~19-29 each), `Ground_Hit_Water` (19), `Missile_flare` / `Missile_Miss` / `missile_hit` (12 each).

Top draw: `Vehicle_Dust_Cloud` (24611), `Dust` (14790), `smoke` (13588), `mg_handle` (13382), `Sparks` (7844), `Missile_hit` (6468), `Smoke` (5529), `Generic_Hit` (5168), `LRM_*` quadlet (4×4740), `Flare` (4953), `Missile_Flare` (4341), `MG_hit` (3087).

### mc2_17 — water-heavy traversal (4228 frames)

| Counter | Total | Unique |
|---|---|---|
| spawn | 203 | 6 |
| draw | 2,072 | 2 |
| mlr_enqueue | 972 (all `DrawEffect`) | 1 |

Spawn: `large_poof` (90), `Jump_Jets` / `Mech_Smoking` (45 each), `Vehicle_Dust_Cloud` (16), `Hovercraft_Wake` (6), `Steam` (1).
Draw: `Steam` (1038), `SteamClouds` (1034). `Hovercraft_Wake` spawn registers but no Draw counter fires for it under capture window.

### mc2_24 — combat-heaviest (4251 frames)

| Counter | Total | Unique |
|---|---|---|
| spawn | 358 | 21 |
| draw | 187,829 | 47 |
| mlr_enqueue | 90,676 (all `DrawEffect`) | 1 |

Top spawn: `large_poof` (96), `Jump_Jets` / `Mech_Smoking` (46 each), `Vehicle_Dust_Cloud` (24), `Ground_Hit_Water` (20), `MG_Miss` / `mg_flare` / `mg_hit` (15 each), `Missile_flare` / `Missile_Miss` / `missile_hit` / `lrm_trail` (12 each).

Top draw: `smoke` (33585), `mg_handle` (33401), `Sparks` (16779), `Missile_hit` (8592), `MG_hit` (7702), `Flare` (7292), `LRM_*` quadlet (4×5626), `AC_10_Hit` / `PPC_Hit` (5728 each), `Smoke` (5128), `Missile_Flare` (4308), `MG_flare` (4305).

## Aggregate (tier1 5/5)

| Counter | Total | Unique-across-tier1 |
|---|---|---|
| spawn | 970 | ~28 (set union of 22+5+17+6+21 minus overlap) |
| draw | 359,011 | ~60 (set union with overlap) |
| mlr_enqueue | 178,390 (all `DrawEffect`) | 1 |

**Observation:** all 178,390 `mlr_enqueue` events under the trace run hit the
`DrawEffect` leaf only. None of the 4 gated leaves except `DrawEffect`
fired — confirms audit doc §3 caller cross-reference: `DrawShape` and
`DrawScreenQuads` have zero live callers under tier1, and `DrawScalableShape`
callers (Shape / ShapeCloud / DebrisCloud) did not spawn in this capture
window. **The combined Shape / ShapeCloud / DebrisCloud / PertCloud / EffectCloud
class never appeared in the spawn counter at all across all 5 tier1 missions**
— consistent with the (B) §1.4 ¶3 "v2 scope" prediction that these primitive
types are rare and can be deferred. v1 (Card / Point / Shard / Tube) covers
the entire observed tier1 spawn surface.

## v1-vs-v2 split recommendation (from data, not class-name guesses)

Per-spec primitive-type classification requires parsing the binary `mc2.fx`
asset (`#XFG`/`#RLM` markers; not text-greppable). For the v1 scope decision
the data-driven signal is sufficient:

- **v1 (must-ship in B1 Stage 2'):** the 28 spawn-names observed across
  tier1 5/5. Every name in the spawn counter above. The producer-site grep
  proves these all flow through one of the four primitive types in the
  v1 plan (Card / Point / Shard / Tube) because **no `Shape::Draw`,
  `ShapeCloud::Draw`, `DebrisCloud::Draw`, `PertCloud::Draw`, or
  `EffectCloud::Draw` call ever fired** (would have shown a separate
  `mlr_enqueue` distribution touching `DrawScalableShape`; none did).
- **v2 (filed as B2 debt at Stage 5'):** the Pert/Shape/Debris/EffectCloud
  primitive types. Defer per plan; tier1 spawn data confirms zero exercise
  in standard play.

**Heavy-combat pick for Stage 4' (per plan §5.4 default):** **mc2_24**
confirmed combat-heaviest (358 spawn / 187,829 draw / 90,676 mlr_enqueue
— 1.6× mc2_10 across all three counters).

## Coverage equivalence target for Stage 3'

For each spawn name above, Stage 3' requires the new `mclib::particles::Spawn`
counter to land within ±10% of the legacy `EffectLibrary::Find` count
in the matched mission. mc2_03 / mc2_17 are calm baselines; mc2_01 /
mc2_10 / mc2_24 are the carry-load stress samples.

## A2-default-on rest-state confirmation

Outside of this Stage 0' opt-in trace run, the A2 default (`MC2_DISABLE_GOSFX=1`
implicit) remains in effect — Stage 1' / 2' / 3' / 4' smoke runs do NOT
need to reset the default. Per plan §5.4 Stage 0' MINOR-2 fold-in: the
`MC2_DISABLE_GOSFX=0` set was scoped to this single trace invocation.
