# C2 Residual Fix — Probe-Gated Decision Tree (Plan B)

> **STATUS: BLOCKED on Plan A.** This is deliberately NOT a concrete TDD plan.
> The mission-data recon (2026-05-17) **refuted** the simple
> "late-register-on-resume" hypothesis for terrain objects on the first-load
> and SP_LOGISTICS paths. The fix mechanism is genuinely undetermined and
> forks on Plan A's probe output. Writing concrete steps now would hard-code
> one of three mutually-exclusive mechanisms on an unconfirmed premise —
> exactly the guess-patching the handoff forbids ("the prior session was
> wrong TWICE") and the `parallel_mission_setup_paths_probe_which_one.md`
> memory blocks.
>
> When Plan A Task 6 lands a verdict, the matching branch below gets promoted
> to a full `writing-plans` plan. Until then, no fix code is written.

**Plan A gate file:** `docs/superpowers/plans/2026-05-17-c2-armed-attribution-and-eligibility-probe.md`

---

## Established by recon (treat as given — do NOT re-derive)

- Fork = **(a) mis-gating**, not (b) inherent. Every static-prop appearance
  class registers its leaves via `registerMultiShape` (`mclib/bdactor.cpp:~830`
  Bldg, Tree/Generic/GV analogues). No static class renders via
  `TG_Shape::Render` yet never registers. So a terrain object on C2
  (`mclib/tgl.cpp:3113`) has a leaf missing from
  `GpuStaticPropBatcher::s_typeIndex` for an object whose `init()` *should*
  have registered it.
- First-load (`Mission::init`) is **safe**: terrain-object register/prime
  (`code/mission.cpp:~2879`) runs strictly before `s_geometryFinalized=true`
  inside `finalizeGeometry()` (`code/mission.cpp:~3114` →
  `gos_static_prop_batcher.cpp:~1326`). The fix must NOT touch first-load.
- SP_LOGISTICS campaign-resume is **safe for terrain objects**:
  `Logistics::beginMission` → full `mission->init()` (registers + finalizes)
  → THEN `addMover` waves (player **mechs only**) →
  `GpuMechBatcher::finalizePending()` (`code/logistics.cpp:~813`). Terrain
  objects were registered inside `mission->init` before its finalize.
- `.ims` quicksave (`Mission::load`) ordering is **structurally safe**
  (respawn `code/saveload.cpp:~1391` precedes finalize `~:1583`), BUT the
  respawn uses a **distinct deserialization path**: `ObjectManager::Load` →
  `TerrainObject/Bldg/Gate/Turret::init` (`code/objmgr.cpp:~1362-1421`),
  which does NOT call `loadTerrainObjects`/`primeTerrainObjectsForMissionLoad`.
  **Whether that path drives `registerMultiShape` for terrain objects at all
  is the unverified pivot.**
- There is currently **no** `GpuStaticPropBatcher::finalizePending()` — only
  `onMapLoad`/`onMapUnload`/`finalizeGeometry`
  (`gos_static_prop_batcher.h:~131-151`). The mech analog
  (`gos_mech_batcher.cpp:~737-799`) is the reference pattern if a recovery
  branch is selected.
- Confounder cleared: mech `[MECHBATCHER v1] shader_fail` cannot abort the
  static `registerType` walk (separate TU/singleton, no cross-read). Stays a
  separate item.

---

## The fork — three candidate fixes, each gated on a Plan A Task 6 observation

### Branch B1 — "Port finalizePending recovery"
**Trigger:** Plan A `register_summary` shows `lateDrops[<active path>] > 0`
AND `[GPUPROPS] late registerType` lines fire for terrain `TG_TypeShape`s.
**Meaning:** types ARE registered but AFTER `s_geometryFinalized` →
permanently dropped into `s_failedTypes`.
**Fix shape:** add `GpuStaticPropBatcher::finalizePending()` mirroring
`gos_mech_batcher.cpp:~737-799` (append late-staged types, rebuild
VBO/IBO/VAO from retained staging, state-neutral bind save/restore), invoked
on the active path immediately adjacent to the proven no-render-gap hook
`GpuMechBatcher::instance().finalizePending()` at `code/logistics.cpp:~813`
(and/or the `.ims` finalize tail `saveload.cpp:~1583` if path=2).
**Done-criterion (substitutive):** Plan A re-capture shows
`c2_calls_per_frame` → ~0 for the terrain class AND total-frame Tracy drops
ON-vs-OFF. The existing 38d8720 bake then applies with zero new lighting code.
**Risk:** flipping a large static class onto the GPU-object path is a
draw-volume/LOD-bound change — MUST stress-test user-driven
zoomed-out-big-map, not tier1 (`memory/zoomed_out_big_map_*`,
`memory/distant_buildings_render_at_lower_lod_never_distance_culled.md`).

### Branch B2 — "Ensure registration on resume"
**Trigger:** Plan A `register_summary` shows `attempts[<active resume path>]
== 0` (or far below first-load) while terrain objects visibly render and hit
C2.
**Meaning:** the `ObjectManager::Load` deserialization path never calls
`registerMultiShape` for terrain objects at all — it is not a late-drop, it
is a missing registration.
**Fix shape:** drive the static-prop registration walk for the
deserialized terrain objects on the `.ims`/resume path BEFORE its
`finalizeGeometry()` (`saveload.cpp:~1583`), mirroring the first-load
`primeTerrainObjectsForMissionLoad` ordering (`mission.cpp:~2879`). This is
structurally different from B1 (add a registration call, not a recovery
append).
**Done-criterion:** same as B1 (C2 calls → ~0 + total-frame Tracy delta).
**Risk:** same zoomed-out stress requirement; PLUS ordering risk — the
registration walk must complete before finalize on the resume path without
double-registering first-load types (idempotent guard exists:
`gos_static_prop_batcher.cpp:~1074` `if (s_typeIndex.count(typeShape)) return;`).

### Branch B3 — "Eligibility predicate defect"
**Trigger:** Plan A shows `attempts > 0`, `lateDrops == 0`, yet terrain
objects still hit C2.
**Meaning:** registration succeeds but `eligibleForGpuObjects` /
`isMultiShapeEligibleForGpuObjects` still returns false — SHAPE_NODE-type
check (`gos_static_prop_batcher.cpp:~3810`) excludes their leaves, or
partial-leaf taint (`~:3786-3794` — one unregistered SHAPE_NODE leaf taints
the whole multishape to C2).
**Fix shape:** UNDETERMINED — requires a fresh narrow recon
(per-leaf eligibility dump for the offending multishape). Do NOT pre-design.
This branch re-enters recon, it does not promote to a plan directly.

### Null branch B0 — "C2 is not the cost"
**Trigger:** Plan A shows `c2_calls_per_frame` ~0 or `c2_cyc_per_frame`
small relative to C5/C6 despite the heavy Tracy zone.
**Meaning:** the dominant cost was C5/C6 all along; the handoff's C2
attribution (from user Tracy) was the conflated-zone artifact.
**Action:** do NOT plan any C2 slice. Re-open the fork; the 38d8720 C5 bake
proof path is the real lever. Update memory.

---

## Cross-branch invariants (apply to whichever branch promotes)

1. **C6 sibling MUST ship in the same commit.** `mclib/tgl.cpp:2865`
   `ResubmitCachedLightData` makes the *same* `addLightDataStructure` call as
   C2. Porting only `:3113` and leaving `:2865` on the legacy path is an
   additive half-fix that fails the done-governor
   (`memory/feedback_offload_must_be_substitutive_not_additive.md`). Plan A's
   probe already buckets C6 separately so its residual is visible.
2. **Substitutive done-governor, not zone-to-zero.** Done = C2 (and C6) armed
   calls/cycles → ~0 on the heaviest mission in a fresh capture AND
   total-frame Tracy drops ON-vs-OFF (anti-mirage — the drawPass precedent).
3. **Kill-switch gated, demote-not-delete.** The legacy C2 path stays
   present-but-gated, not deleted, per the debug-instrumentation-rule.
4. **Soak-waiver substitute** (`memory/feedback_soak_waiver_with_probes_and_reviews_validated.md`):
   env-gated parity probe + mandated adversarial implementation review for
   this endpoint slice + code-proof fallback. Do NOT propose a calendar soak.
5. **Mandatory adversarial-plan-review** of the promoted branch before
   execution (handoff methodology; architectural-endpoint stakes).

---

## Promotion procedure (when Plan A Task 6 lands)

1. Read the Plan A verdict appended to this file's bottom (Plan A Task 6 Step 3).
2. Select B1 / B2 / B3 / B0 from the trigger table.
3. If B1 or B2: dispatch `mc2-cpu-gpu-offload-expert` + `mc2-render-expert`
   to ground the Stage 0 consumer-repoint contract, then run
   `superpowers:writing-plans` to author the concrete plan, then
   `adversarial-plan-review`.
4. If B3: dispatch the narrow per-leaf eligibility recon FIRST; do not plan.
5. If B0: stop the C2 arc; update memory; redirect to the C5 bake proof.

<!-- Plan A verdict gets appended below this line by Plan A Task 6 Step 3 -->

## PLAN A VERDICT (2026-05-17, user-driven capture `~/Desktop/c2_probe.log`, 26475 lines)

**SELECTED BRANCH: B0 (reframed) — C2 is NOT the cost. The handoff's entire
C2 premise was a conflated-Tracy-zone misattribution. Do NOT plan a C2 slice.
Branches B1/B2/B3 and the (a)/(b) fork are MOOT (they were predicated on the
false C2 premise; registration/eligibility is healthy).**

### Evidence

Registration (`[GPUPROPS v1] event=register_summary`, one line, at finalize):
`path=1 regCalls[init=13408 ims=0 splog=0 unk=0] lateDrops[init=0 ims=0
splog=0 unk=0] typeIndexSize=548`. Zero `[MECHRESTORE v1] saveload_phase`
lines in the whole log -> mission did NOT use the `.ims` `Mission::load`
path; it is first-load / SP_LOGISTICS (path=1, `Mission::init`).
Registration is HEALTHY: 548 types registered, 13408 calls, 0 late-drops at
finalize. Only 2 post-finalize straggler types (`[GPUPROPS] late
registerType ... CPU-fallback`, lines 174-175) out of 548 — immaterial, and
they occur in the mech-batcher finalize_pending bring-up window, not the
cost path. There is NO registration fault and NO late-register trap. B1/B2
falsified. B3 (eligibility predicate) is moot — the cost is not a mis-gated
registered type at all (see below).

Armed cost split (`[LIGHT_COST_SPLIT v1] event=summary`, 21 windows):
- **C2 (`tgl.cpp:3113` direct legacy-leaf): NEGLIGIBLE.** Steady state
  `c2_calls_per_frame=1.0`, `c2_cyc_per_frame ~2600-3800`. Even the early
  camera peaked at only ~14 calls / ~23k cyc.
- **C6 (`TG_Shape::ResubmitCachedLightData`, `tgl.cpp:2883`; sole caller
  `mclib/msl.cpp:2006` `cachedGpuLightIndex_ =
  firstShapeNodeLeaf->ResubmitCachedLightData();`): THE DOMINANT PRODUCER.**
  Scales hard with camera/draw-volume: ~40-180 calls/frame at the initial
  camera, exploding to a **steady `c6_calls_per_frame=1677.0`,
  `c6_cyc_per_frame ~3.3M`** in the zoomed-out worst case. The handoff's
  user-Tracy "x1726/frame" attributed to C2 was C6 all along — C2 and C6
  both call `addLightDataStructure`, so the single Tracy zone could not
  separate them; the handoff guessed C2 from the legacy-leaf branch.
- **C5 (`GatherGpuObjectLightDataOnly`, `tgl.cpp:2858`): secondary.**
  ~24-51 calls/frame, ~60k-320k cyc/frame.

Ratio signal (m-2-valid; absolute cycles uncalibrated): zoomed-out
`c6_cyc / (c2_cyc + c5_cyc)` ~= 3.3M / ~0.3M ~= **~10x**, and C6 calls are
**~1677x** C2 calls. The camera-scaling (C6 40 -> 1677 as the user zoomed
out) is the `zoomed_out_big_map` stress path tier1 is structurally blind to,
exactly as the memory warns.

### Reframed next step (NOT a C2 slice)

The real lever is **C6 `TG_Shape::ResubmitCachedLightData()`** (sole caller
`msl.cpp:2006`), ~1677 calls/frame & ~3.3M cyc/frame zoomed-out. Any future
substitutive slice targets C6's per-leaf `addLightDataStructure(&lightData_)`
via the same persistent-slot/repoint pattern as 38d8720, keyed by stable
leaf/recipe identity. This is a NEW, telemetry-grounded slice — recon it
fresh (who/what `firstShapeNodeLeaf` is, why Resubmit fires 1677x, whether
the 38d8720 persistent table can serve it directly). Dispatch
`mc2-render-expert` + `mc2-cpu-gpu-offload-expert` before any plan. The
substitutive done-criterion: C6 `c6_calls/cyc_per_frame` -> ~0 zoomed-out in
a fresh capture AND total-frame Tracy drops. The C2 bracket stays as cheap
permanent telemetry (demote-not-delete).

## C6 RECON OUTCOME (2026-05-17, render-expert + offload-expert, parallel)

**Premise refined (favorable). The C6 fix is ~90% pre-wired; the residual is
a chicken-and-egg in the static-skip path.**

### Render mechanics (grep-verified)
- `msl.cpp:2006` is inside `TG_MultiShape::ResubmitCachedGpuLightData()`
  (`mclib/msl.cpp:1968`). `firstShapeNodeLeaf` = first SHAPE_NODE leaf of
  the multishape.
- Sole callers: `BldgAppearance::touch()` (`mclib/bdactor.cpp:3046`) and
  `TreeAppearance::touch()` (`mclib/bdactor.cpp:5153`).
- Dispatch: `code/terrobj.cpp:738-746` routes to `touch()` when
  `MC2_STATIC_UPDATE_SKIP` (default TRUE, `terrobj.cpp:92`) + gpuPath +
  appearanceClaimsStatic + !ownerForcesDynamic. So by default EVERY
  registered static bldg/tree takes `touch()` -> C6 every frame.
- 1677x / zoom-scaling: per-TerrainObject update loop walks every static
  the cull gate admits; zoomed-out admits whole map at LOD. ~3.3M cyc =
  1677 x the 1792B FNV + 1792B memcmp dedup inside `addLightDataStructure`
  (`tgl.cpp:2886`).
- A shipped fast-path (`MC2_LIGHTBRIDGE` default-on, guard `msl.cpp:1982-1992`)
  early-returns before :2006 when `cachedFrame_==g_mc2FrameCounter &&
  cachedGpuLightIndex_!=0xFFFFFFFF`. It does NOT fire on the touch() path:
  CHICKEN-AND-EGG -- touch() never calls the 38d8720 bake, falls to the
  FNV resubmit at :2006, and only THEN sets `cachedGpuLightIndex_`.
- Persistent-slot mechanism (`EmitBakedGpuLightData` `msl.cpp:1953`: pure
  `cachedGpuLightIndex_ = recipeIndex`) EXISTS, keyed by
  `staticReg.recipeIndex` (already in scope at both touch() sites). The C6
  callers are the SAME static recipes the `update()` branch already bakes.

### The candidate fix (substitutive, cheap)
Make `BldgAppearance::touch()` / `TreeAppearance::touch()` call
`EmitBakedGpuLightData(recipeIndex,...)` / `mc2CacheOrBakeStaticGpuLight`
with the `staticReg.recipeIndex` they already hold, WHEN
`mc2GetBakedStaticLight(recipeIndex)` hits -- a pure pointer assignment like
the update() HIT path. Repoint site = the two touch() bodies in
`bdactor.cpp` (NOT tgl.cpp:2883, NOT msl.cpp:2006). Legacy resubmit stays
present-but-gated as the MISS / kill-switch fallback. Stable key =
`staticReg.recipeIndex`.

### DOUBLE-BLOCK before any fix plan (both advisors, independently)
1. **Bake-priming probe.** Static analysis cannot prove the 38d8720 bake
   map is POPULATED for touch()-only recipes (bake fills lazily on first
   update() MISS; a skip-path static may never prime it). Probe: env-gated
   once-per-recipe in both touch() bodies -- `[LIGHTBRIDGE v1]
   event=c6_bake_probe recipe=%d baked=%d` where
   `baked=mc2GetBakedStaticLight(staticReg.recipeIndex,&tmp)`.
   baked=1 ~all -> pure repoint, near-zero-cost win. baked=0 most ->
   slice must ALSO prime the bake (first-frame gather or seed at
   registration) = C6-specific bring-up, not pure repoint.
2. **Gather-vs-submit split.** The committed C6 RDTSC bracket wraps the
   `addLightDataStructure(&lightData_)` submit only. Not proven whether
   ~3.3M cyc is the submit body (FNV+memcmp dedup -> repoint kills it) or
   `lightData_` gather upstream of the bracket (repoint = additive
   half-fix). Probe: tighter RDTSC split inside the C6 path (extend
   `MC2_LIGHT_COST_SPLIT` with c6_gather vs c6_submit sub-buckets,
   demote-not-delete).

Both are pure env-gated instrumentation, combinable into ONE micro-slice
(Plan A-2), user-driven zoomed-out capture -> branch selection -> then the
fix plan. Done-criterion (unchanged): C6 c6_calls AND c6_cyc -> ~0
zoomed-out in fresh MC2_LIGHT_COST_SPLIT capture + total-frame Tracy drop
ON-vs-OFF (anti-mirage) + named consumer repoint. Camera-scaled hot path:
user-driven zoomed-out-big-map validation mandatory, tier1 structurally
blind, gates the default-on flip. Plan A-2:
`docs/superpowers/plans/2026-05-17-c6-bake-priming-probe.md`

## C6 FIX DETERMINED (2026-05-17 adjudication — Plan A-2 RETIRED as tautological)

Adversarial-plan-review of Plan A-2 (bake-priming probe) returned STOP with a
recon-level finding; render-expert adjudication grep-verified it. Outcome:

- **Plan A-2 (`2026-05-17-c6-bake-priming-probe.md`) is RETIRED — do NOT
  execute it.** The `needsFullBakeNextFrame` lifecycle gate structurally
  guarantees a bake-populating `update()` before any `touch()` in the
  shipped config (registerStatic sets the flag -> IsStaticNow()=false ->
  dispatch routes to update() -> mc2CacheOrBakeStaticGpuLight populates the
  bake + clears the flag -> only then touch() engages; invalidate/LOD-swap
  re-sets the flag). The probe would report baked=1 by construction = a
  tautology that fabricates a "pure repoint" selection. The only un-primed
  path is the all-GPU-off non-default config where C6 is irrelevant.

- **Real binding constraint (grep-verified):** the C6 fast-path guard
  `msl.cpp:1982-1984` requires `cachedFrame_ == g_mc2FrameCounter`.
  `cachedFrame_` is stamped ONLY by update()/CacheGpuLightData/
  EmitBakedGpuLightData, never re-stamped on a steady-state touch() frame ->
  guard fails every frame -> control falls to msl.cpp:2006 ->
  addLightDataStructure 1792B FNV + 1792B memcmp dedup (tgl.cpp:2886,
  txmmgr.cpp:1251-1268) = C6's 3.3M cyc/frame.

- **The fix is directly determined (no probe needed):** in
  `BldgAppearance::touch()` (`bdactor.cpp:3037-3052`) and
  `TreeAppearance::touch()` (`bdactor.cpp:~5144-5153`) replace the
  `*Shape->ResubmitCachedGpuLightData()` call with
  `mc2CacheOrBakeStaticGpuLight(*Shape, staticReg.registered,
  staticReg.recipeIndex)` (the SAME primed-slot repoint update()'s
  else-branch already uses at bdactor.cpp:2574/4986). HIT branch ->
  `EmitBakedGpuLightData(recipeIndex, baked)` sets
  `cachedGpuLightIndex_=recipeIndex` + `cachedFrame_=g_mc2FrameCounter`
  with ZERO FNV/memcmp (msl.cpp:1955-1965). `recipeIndex` is a permanent
  shipped slot (bakeStaticLightSlot writes lightData_[recipeIndex]
  permanently + whole-buffer upload, txmmgr.cpp:1425-1436). C6 -> ~0.
  Safest concrete form: in touch(), `mc2GetBakedStaticLight(recipeIndex,
  baked)` -> if HIT `EmitBakedGpuLightData`; else KEEP current
  `ResubmitCachedGpuLightData()` as the MISS fallback (NOT
  `CacheGpuLightData()` — terrain-color-staleness, msl.cpp:1874-1887).

- **Residuals for the fix plan (not blockers):** MC2_LIGHTBAKE=0 kill-switch
  parity (reuse mc2LightBakeEnabled(); mc2CacheOrBakeStaticGpuLight already
  passes through at bdactor.cpp:2225-2228 — no new switch); map-miss-after-
  invalidate self-heals in 1 frame (keep ResubmitCachedGpuLightData as the
  MISS arm); sentinel/staleness safe (recipeIndex>=0 non-sentinel,
  cachedFrame_ freshly stamped); zoomed-out/cull-cascade safe (per-actor,
  strictly reduces work, no cull bypass) but STILL requires user-driven
  zoomed-out-big-map total-frame Tracy substitutive proof (C6 "scan" -> ~0,
  no displaced cost in EmitBakedGpuLightData/whole-buffer-upload, both
  cameras) per substitutive-not-additive.
- **Adjacent stale comment** `bdactor.cpp:3039-3040` claims touch() only
  fires when MC2_STATIC_UPDATE_SKIP=1 / default keeps update() — INVERTED
  (terrobj.cpp:92 defaults skip ON). Correct it IN the fix commit
  (minimal-touch), not separately.

Fix plan: `docs/superpowers/plans/2026-05-17-c6-touch-primed-slot-repoint.md`

## C6 SUBSTITUTIVE PROOF -- CONFIRMED (2026-05-17, user-driven)

DONE. The C6 lighting residual is RETIRED. Mechanism: BldgAppearance::touch()
/ TreeAppearance::touch() repoint to the primed 38d8720 slot
(`mc2GetBakedStaticLight` HIT -> `EmitBakedGpuLightData(recipeIndex, baked)`)
instead of the per-frame FNV+memcmp `ResubmitCachedGpuLightData()` resubmit;
legacy path retained as MISS/kill-switch fallback (`MC2_LIGHTBAKE=0`).
Commits `4fed1b6` (Bldg) + `7289eae` (Tree) on claude/gpu-driven-rendering.

Evidence (user-driven Tracy, heaviest mission, zoomed-out worst case):
- LEG A (legacy, both guards off): `addLightDataStructure scan` x1876 /
  932.12us / 41.38% of GameLogic.Units.TerrainObjects -- reproduces the
  Plan-A baseline (regime confirmed).
- LEG B (fix isolated, MC2_LIGHTBRIDGE=0 so ONLY the new touch() repoint can
  act): `addLightDataStructure scan` x18 / 8.67us / 0.57%. ~107x collapse,
  airtight attribution to THIS slice. The residual x18 is the expected
  bounded MISS-branch floor (transient un-baked/first-frame, self-heals) --
  not a leak.
- Full-frame view (fix): `addLightDataStructure scan` ABSENT from the top
  ~40 zones; no `EmitBakedGpuLightData` / lighting-upload zone appears
  (O(1) repoint; LightsData whole-buffer upload pre-paid by 38d8720 --
  ZERO displaced cost). TerrainObjects self ~980us -> 913us (flat, no
  displacement). Total frame ~10.47ms -> ~9.0ms (~14% at this camera).
- Visual: user confirms static buildings + trees render correct
  (no darkening/flicker/wrong-tint) zoomed-out.

Substitutive-not-additive done-governor SATISFIED: named CPU zone absent
from a fresh capture + total-frame dropped + no displaced cost + causal
attribution isolated (LEG B).

**38d8720 CO-CONFIRMED:** the persistent static-light table's correctness
was exercised at ~1677-recipe scale on the heaviest mission with clean
visuals; this capture doubles as 38d8720's pending substitutive/visual
proof (per the 2026-05-17 combined-capture user sign-off).

Confirmed substitutive wins now: minePass, drawPass, **C6
ResubmitCachedLightData retirement**, **38d8720 persistent static-light
table** (co-confirmed). The lighting arc (C2->C6 misattribution ->
chicken-and-egg -> touch() primed-slot repoint) is CLOSED.
