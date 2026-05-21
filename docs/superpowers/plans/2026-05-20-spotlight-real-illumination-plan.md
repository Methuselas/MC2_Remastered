# (E) SpotLight_ → Real Illumination — Implementation Plan

- **Status:** DRAFT — ready to execute
- **Date:** 2026-05-20
- **Worktree:** `claude/nifty-mendeleev`
- **Spec:** [docs/superpowers/specs/2026-05-20-spotlight-real-illumination-design.md](../specs/2026-05-20-spotlight-real-illumination-design.md)
- **Goal:** Replace opaque cone billboard rendering for `SpotLight_*`-prefixed child shapes with real `TG_Light` registrations through the existing `addWorldLight` pipeline. Ship to all four affected populations (building static props, mech actors, both via the same retire-geometry + register-light pattern). Default-on after soak.
- **Substitutive test:** F3 RenderDoc capture shows zero cone draws for `SpotLight_*` children; `worldLights[2..N]` populated as expected; visible illumination spillage on terrain/props/mechs around lamp positions in mc2_04 (night) canary; Vedette/LRMC visual regression check on mc2_24 (the f77f135 revert canary class).

## Risk register before execution

- **R1.** Cross-mission lifecycle leak. Existing anubis `pointLight` is leaked at mech destruction in `mech3d.cpp` (advisor flagged). Don't inherit when generalising; pair every `addWorldLight` with a `removeWorldLight` in the destroy hook. Building-side bdactor.cpp:1967-1972 + :2754-2762 IS the canonical paired-cleanup pattern — mirror it for the new vectors.
- **R2.** `active=false` discipline. Off-screen mechs / culled buildings must set `active=false` or they consume per-shape best-16 slots from visible shapes. Mirror [mech3d.cpp:3382](mclib/mech3d.cpp).
- **R3.** addWorldLight is O(N) first-empty-slot scan. After 256→1024 bump, never call per-frame. One-shot at registration only.
- **R4.** Per-instance not per-type. Building instances have different world transforms; register at INSTANCE update (BldgAppearance::update where position is valid), NOT at init (where position is zero — see T1.4 C-r1 C1 fix) and NOT at TG_TypeShape construction (per-type, not per-instance).
- **R5.** Day/night gating. CPU path skips spotlight cones during day at [tgl.cpp:1728](mclib/tgl.cpp). Real lights follow the same convention — gate registration on `eye->isNight` AND active toggle on `visible` (per anubis at mech3d.cpp:3353; the inView gate was a mistake — see C-r1 C5).
- **R6.** Vedette/LRMC canary. The f77f135 revert was triggered by a state-leak that made Vedettes invisible. (E) does NOT add a new draw path, so the same class doesn't apply, but Stage 1 smoke MUST include mc2_24 visual confirmation as a paranoia check.
- **R7. (NEW from C-r1 C3+):** existing `BldgAppearance::pointLight` is the PER-BUILDING terrain ambient light (TG_LIGHT_TERRAIN, sourced from appearType->terrainLightRGB). The new `spotlightLights_` vector is PER-CHILD-SPOTLIGHT-NODE (TG_LIGHT_POINT). Distinct concerns; both coexist. Document in code comment at the vector declaration so future readers don't conflate. Same goes for mech3d anubis `pointLight` (the hardcoded SLCircle_anubis single light) vs the new `spotlightLights_` vector — coexist until T3.x consolidation.
- **R8. (NEW from C-r1 M2):** the existing CPU spotlight render at [tgl.cpp:1728](mclib/tgl.cpp) sets `listOfVertices = NULL; return;` when `isSpotlight && !isNight`. The downstream static-prop batcher at gos_static_prop_batcher.cpp:2627 then skips children with `!listOfVertices`. So in DAYTIME, the cone is already skipped without (E). T1.3's `continue` branch only meaningfully diverges from current behavior at NIGHT. Baseline measurements (T0.3) should be captured AT NIGHT to be meaningful. Day baselines will show ~zero spotlight submits regardless of env state.
- **R9. (NEW C-r2):** plan v2 adversarial-review round 1 introduced multiple file:line citations. **All v2 citations re-grep-verified against `nifty-mendeleev` worktree** before v3 commit. The reviewer's round-2 C3 finding (claiming systematic line-number rot) was REJECTED — reviewer was looking at a different fork/branch. The actual round-2 finds were C1 (missing childNodeIds_ on building side), C2 (night→day active stuck true), C4 (Mech3DAppearance not Mech3D), and several MAJORs — all fixed in v3.
- **R10. (NEW C-r2 M5):** in `BldgAppearance::destroy`, `bldgShape` is deleted before the cleanup block runs. T1.5's cleanup MUST use CACHED state vectors only — do NOT call `getNodeIdPosition` or any bldgShape method during destroy. Documented inline in T1.5.

## Per-task atomic commit rule

Every T-numbered task lands as one commit. Commit messages reference task number (e.g. `feat(spotlight-real T1.3): ...`). If a task surfaces an unexpected blocker, STOP, file the deviation, surface to user before continuing — do NOT bundle the workaround into the same commit.

---

## Stage 0 — Instrument (no behavior change)

### T0.1 — `MC2_SPOTLIGHT_REAL_TRACE` registration counter at static-prop batcher

**Files:** `GameOS/gameos/gos_static_prop_batcher.cpp`

**Changes:**
- Add env-gated bool `s_spotlightRealTrace` reading `MC2_SPOTLIGHT_REAL_TRACE`.
- At `submitMultiShape`, when `child->isSpotlight==true` (this site is INSIDE GpuStaticPropBatcher which IS a TG_MultiShape friend per msl.h:251-256, so direct field access compiles), increment monotonic + window counters. First-hit always-on (one-line stderr) so any operator sees confirmation without env.
- 600-flush summary print, env-gated, matches `[INSTR v1]` schema family.
- Tag: `[SPOTLIGHT_REAL_TRACE v1]`.

**Pattern to mirror:** the now-deleted `[SPOTLIGHT_TRACE v1]` block from f77f135 (which was reverted with d91e639 but is documented in `memory/spotlight_billboards_static_prop_opaque_bug.md`). Same shape, new tag.

**Verification:** smoke run with `MC2_SPOTLIGHT_REAL_TRACE=1` on mc2_04 + mc2_10 shows the counter incrementing. First-hit stderr line confirms first SpotLight_ child name.

### T0.2 — Same counter at mech batcher skip site

**Files:** `GameOS/gameos/gos_mech_batcher.cpp` around line 553-563.

**Important (per adversarial review C-r1 M1):** the existing `skip_spotlight` site at line 553-563 fires during RECIPE CONSTRUCTION (`numNodes` iteration in TG_TypeShape bring-up), once per type. It is NOT a per-frame counter. For per-frame mech-spotlight evidence, instrument the per-instance draw submission path instead. T0.2 instruments BOTH:
- (a) The existing recipe-build skip — to count how many type-recipes carry spotlight nodes. Tag: `[SPOTLIGHT_REAL_TRACE v1] event=type_spotlight_node`.
- (b) The per-frame mech draw submission for spotlight-carrying types — counter increment per drawn instance. Tag: `[SPOTLIGHT_REAL_TRACE v1] event=mech_spotlight_draw`.

**Verification:** mc2_24 smoke run shows BOTH counters; (a) reports type-count, (b) reports per-frame draws.

### T0.3 — Baseline measurement run

**Files:** none (data-gathering).

**Action:** Run `MC2_SPOTLIGHT_REAL_TRACE=1` smoke on mc2_04 (night, the visual proof case), mc2_10 (tier1 stress), mc2_24 (mech-heavy / Vedette/LRMC).

**Capture:** mission-load population count, per-frame submitMultiShape counts, mech-skip counts. Confirm user's "extremely low" expectation. If counts come back surprisingly high (>500 per mission), surface before Stage 1.

**Commit:** none — just artifact capture.

---

## Stage 1 — Gated implementation (`MC2_SPOTLIGHT_REAL=1`, default-off)

### T1.1 — Bump CPU world pool 256 → 1024

**Files:** `mclib/tgl.h`, `mclib/camera.cpp`, `mclib/tgl.cpp`

**Adversarial review C-r1 C4 — full census of MAX_LIGHTS_IN_WORLD readers (grep-verified against nifty-mendeleev):**
- [mclib/tgl.h:170](mclib/tgl.h) — the `#define` itself
- [mclib/tgl.h:790](mclib/tgl.h) — `static Stuff::LinearMatrix4D TG_Shape::s_lightToShape[MAX_LIGHTS_IN_WORLD];` (ONE LinearMatrix4D array — C-r2 M2 fix)
- [mclib/tgl.h:791](mclib/tgl.h) — `static Stuff::Vector3D TG_Shape::s_lightDir[MAX_LIGHTS_IN_WORLD];`
- [mclib/tgl.h:792](mclib/tgl.h) — `static Stuff::Vector3D TG_Shape::s_spotDir[MAX_LIGHTS_IN_WORLD];`
- [mclib/tgl.h:793](mclib/tgl.h) — `static Stuff::Vector3D TG_Shape::s_rootLightDir[MAX_LIGHTS_IN_WORLD];` (THREE Vector3D arrays)
- [mclib/tgl.cpp:78-81](mclib/tgl.cpp) — definitions of the four static arrays above
- [mclib/camera.cpp:411,:423,:430](mclib/camera.cpp) — the three pool allocations (worldLights / activeLights / terrainLights)

**Changes:**
- `mclib/tgl.h:170` — `#define MAX_LIGHTS_IN_WORLD 1024`.
- The four static arrays at tgl.h:790-793 grow automatically (BSS expansion at link time). **Corrected BSS arithmetic (C-r2 M2):** 1×1024×sizeof(LinearMatrix4D≈64) ≈ 64KB; 3×1024×sizeof(Vector3D≈12) ≈ 36KB; 3×1024×sizeof(TG_LightPtr=8) ≈ 24KB. **Total ≈ 124KB** (NOT 316KB as v2 incorrectly stated). Well below the project's RAM-cost-irrelevance threshold per [memory/feedback_ram_cost_not_a_concern_below_500mb.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_ram_cost_not_a_concern_below_500mb.md).
- No other `MAX_LIGHTS_IN_WORLD` readers found outside the cited sites. Shader-side `MAX_LIGHTS_IN_WORLD = 16` in [shaders/include/lighting.hglsl:23](shaders/include/lighting.hglsl) is a DIFFERENT scope (per-shape array) and DOES NOT change.
- Verify the `memset` calls at `:412`, `:424`, `:431` use the macro consistently (no hard-coded `256`).

**Verification:** clean build (RelWithDebInfo, `--clean-first`). **Reason for `--clean-first` (C-r2 M3 corrected rationale):** `MAX_LIGHTS_IN_WORLD` is a `#define` expanded at every use site (e.g. inline loop bounds in `camera.h` at `addWorldLight`, `removeWorldLight`, etc.). TUs that included the OLD value compile with the OLD loop bound; the BSS allocation is the NEW size. Stale `.obj` files would scan only 256 slots of a 1024-slot array. The static-array size change itself does NOT alter `sizeof(TG_Shape)` (those are class-level static members, not instance layout), so the class-layout rule isn't the primary trigger — the `#define` expansion is. Either way, `--clean-first` is correct per [memory/feedback_class_layout_change_needs_clean_first.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_class_layout_change_needs_clean_first.md). Smoke tier1 with no env vars: passes unchanged.

### T1.2 — Env-gate plumbing

**Files:** Wherever process-startup env reads live (likely `code/mechcmd2.cpp` or a startup module).

**Changes:**
- Add `static bool g_spotlightReal = false;` (file-scope) and read `MC2_SPOTLIGHT_REAL` once at startup. Default-off.
- Export via inline getter `bool isSpotlightRealEnabled()` from a shared header. No new symbol bloat.

**Verification:** startup banner `[INSTR v1] enabled: ...` includes `spotlight_real=0` when env unset.

### T1.3 — Static-prop batcher: skip cone for isSpotlight

**Files:** `GameOS/gameos/gos_static_prop_batcher.cpp` (submitMultiShape around the existing `flags |= (1u << 2)` site)

**Changes:**
- Wrap the existing isSpotlight packet emission in `if (isSpotlightRealEnabled() && child->GetIsSpotlight()) { /* skip — light is handled at BldgAppearance::update */ continue; }`. Effectively: when gate ON, the cone is NOT submitted. (Note: the static-prop batcher's existing direct-field access works because `GpuStaticPropBatcher` IS in the TG_MultiShape friend list at msl.h:251-256; but using the public `GetIsSpotlight()` accessor here is forward-compatible.)
- Keep the existing `flags |= (1u << 2)` path for the gate-off case (no behavior change when default-off).

**Verification:** With `MC2_SPOTLIGHT_REAL=1`, RenderDoc capture on mc2_04 shows no static-prop packets with the spotlight bit set in the coalesce multidraw. With env unset: behavior unchanged.

### T1.4 — Building-side TG_Light registration in `BldgAppearance::update`

**Adversarial review C-r1 C1 fix:** `BldgAppearance::init` at [bdactor.cpp:612](mclib/bdactor.cpp) sets `position.Zero()` at line 652. World position is (0,0,0) at init time. Registering there places EVERY building spotlight at world origin — substitutive completion criterion #5 (lamps illuminate ground) would fail by construction. Correct hook is `BldgAppearance::update` at [bdactor.cpp:1910](mclib/bdactor.cpp), mirroring the existing per-building terrain pointLight pattern at lines 1933-1973 (which is itself a different concern — TG_LIGHT_TERRAIN building-ambient — and continues to coexist; see C-r1 C3 note below).

**Adversarial review C-r1 C3 disambiguation:** `BldgAppearance::pointLight` ([bdactor.h:259](mclib/bdactor.h), allocated at [bdactor.cpp:1937](mclib/bdactor.cpp)) is the PRE-EXISTING per-building TERRAIN illumination (TG_LIGHT_TERRAIN type, sourced from `appearType->terrainLightRGB`). It is a SEPARATE concern from SpotLight_ child nodes. (E) adds a vector of NEW lights (TG_LIGHT_POINT type, per-SpotLight_-child); existing `pointLight` is untouched. Document this in code comment at the new vector declaration so future readers don't conflate.

**Files:** `mclib/bdactor.cpp` (`BldgAppearance::update` at line 1910), `mclib/bdactor.h` (add fields)

**Changes:**
- In `BldgAppearance` header: add FOUR members (paired arrays + node-index vector + init flag):
  ```cpp
  // (E) SpotLight_-child illumination. NOT the same as `pointLight`/`lightId`
  // above (which is per-building terrain ambient, TG_LIGHT_TERRAIN).
  std::vector<TG_LightPtr>  spotlightLights_;
  std::vector<DWORD>        spotlightSlotIds_;
  std::vector<int>          spotlightNodeIds_;  // C-r2 C1 fix: indices into bldgShape->listOfShapes
  bool                      spotlightsRegistered_;  // per-instance lazy-init key
  ```
- Initialize `spotlightsRegistered_ = false` in the existing init block at bdactor.cpp:645-674.
- In `BldgAppearance::update` (starts at [bdactor.cpp:1910](mclib/bdactor.cpp), pointLight pattern at 1933-1972), AFTER the existing terrain pointLight block:
  ```cpp
  if (isSpotlightRealEnabled()) {
      // Lazy first-night register (mirrors anubis lazy pattern).
      // World position is valid in update() (not in init()) per C-r1 C1.
      // eye->isNight is a BARE FIELD at camera.h:272 — no parens (C-r3 C2 fix).
      if (!spotlightsRegistered_ && eye->isNight) {
          // C-r4 C1: numTG_Shapes / listOfShapes are PROTECTED on TG_MultiShape
          // (msl.h:260-263) and BldgAppearance is not a friend. Use public
          // accessors GetNumShapes() at msl.h:431 + GetShapeRec(int) at msl.h:438
          // (returns const TG_ShapeRec*). The static-prop batcher uses direct
          // field access ONLY because it's in the friend list at msl.h:251-256.
          for (int i = 0; i < bldgShape->GetNumShapes(); ++i) {
              const TG_ShapeRec* recp = bldgShape->GetShapeRec(i);
              if (!recp) continue;
              const TG_ShapeRec& rec = *recp;
              TG_Shape* child = rec.node;
              // C-r3 M1: mirror canonical batcher guards at
              // gos_static_prop_batcher.cpp:2477 — skip helper nodes / inactive.
              if (!child || !rec.processMe) continue;
              if (!child->GetIsSpotlight()) continue;  // C-r4 M1: TG_Shape::isSpotlight field is protected; use accessor at tgl.h:951

              // C-r3 C1 fix: resolve node-NAME id (NOT listOfShapes index).
              // getNodeIdPosition takes a name-id from GetNodeNameId per the
              // anubis pattern at mech3d.cpp:3336-3338 and bdactor.cpp:574/1610.
              // Chain: TG_Shape::getNodeName() at tgl.h:964 → bldgShape->GetNodeNameId(name).
              const char* nodeName = child->getNodeName();
              if (!nodeName) continue;
              long nodeId = bldgShape->GetNodeNameId(nodeName);
              if (nodeId == -1) continue;

              TG_LightPtr light = (TG_LightPtr)malloc(sizeof(TG_Light));   // m3 fix: plain malloc, mirror :1937
              light->init(TG_LIGHT_POINT);                                  // OQ2 v1 — POINT not SPOT
              light->SetaRGB(0xffe8c870);                                    // OQ3 v1 warm hardcoded
              light->SetIntensity(0.5f);                                     // OQ4 v1 initial
              light->SetFalloffDistances(20.0f, 80.0f);                      // OQ4 v1
              long slotId = eye->addWorldLight(light);  // camera.h:805 returns long; -1 on overflow
              if (slotId < 0) { free(light); continue; }
              // Cast to DWORD for storage to match existing lightId convention
              // (bdactor.h:260 lightId is DWORD; removeWorldLight takes DWORD).               // pool overflow
              spotlightLights_.push_back(light);
              spotlightSlotIds_.push_back(static_cast<DWORD>(slotId));
              spotlightNodeIds_.push_back(nodeId);                           // C-r3 C1: store NAME-id, not index
          }
          spotlightsRegistered_ = true;
      }

      // Per-frame in-place update. Runs UNCONDITIONALLY once registered
      // (NOT inside the isNight gate) — C-r2 C2 fix: day→night→day transitions
      // toggle active via the gate below; lights stay allocated.
      // R3: no remove/add per frame, just SetPosition + active toggle.
      for (size_t k = 0; k < spotlightLights_.size(); ++k) {
          // getNodeIdPosition takes a name-id (resolved at registration above
          // per anubis pattern at mech3d.cpp:3338).
          Stuff::Vector3D childPos = getNodeIdPosition(spotlightNodeIds_[k]);
          spotlightLights_[k]->SetPosition(&childPos);
          // C-r3 C2: eye->isNight (bare field, no parens) per camera.h:272 + canonical mech3d.cpp:3333.
          spotlightLights_[k]->active = (eye->isNight && visible && !forceLightsOut);
          // 'visible' matches anubis at mech3d.cpp:3353 (C-r1 C5)
          // !forceLightsOut matches existing pointLight gate at :1933
      }
  }
  ```
- Guard the entire block on `isSpotlightRealEnabled()` so env-off behavior is unchanged.

**Verification:** With env on AND mc2_04 (night mission), mission load → first night-frame update calls `addWorldLight` N times where N matches the T0.3 baseline count. Subsequent frames show only SetPosition / active-toggle (no further addWorldLight). Day-test: ensure if mission ever transitions to day, active flips to false (no spillage) but no alloc churn.

### T1.5 — Building destroy hook: removeWorldLight

**Files:** `mclib/bdactor.cpp` (`BldgAppearance::destroy` at [:2726](mclib/bdactor.cpp); existing `pointLight` removal at [:2756](mclib/bdactor.cpp))

**Changes:**
- Add a parallel cleanup block alongside the existing `pointLight` removal at [bdactor.cpp:2756](mclib/bdactor.cpp):
  ```cpp
  for (size_t k = 0; k < spotlightLights_.size(); ++k) {
      if (eye) eye->removeWorldLight(spotlightSlotIds_[k], spotlightLights_[k]);
      free(spotlightLights_[k]);  // m3 fix: plain free to match malloc
  }
  spotlightLights_.clear();
  spotlightSlotIds_.clear();
  spotlightNodeIds_.clear();
  spotlightsRegistered_ = false;
  ```
- **Destroy ordering note (C-r2 M5):** `BldgAppearance::destroy` deletes `bldgShape` BEFORE freeing pointLight in the existing pattern. The new cleanup block above must use CACHED state (spotlightLights_/spotlightSlotIds_/spotlightNodeIds_) only — do NOT call `getNodeIdPosition` or any bldgShape method during destroy.
- **No day-tear-down required (C-r2 C2 fix is at T1.4):** unlike the existing pointLight (which alloc/frees on night/day boundaries), (E)'s spotlightLights_ stay allocated for the building's lifetime and toggle `active` per-frame. So this destroy hook is the ONLY cleanup site needed.

**Verification:** Smoke run loads mc2_04 twice in sequence (mission reload). Mission 2's `addWorldLight` call count matches mission 1's (no slot leak from previous). Verify via the trace counter and a one-line stderr at `removeWorldLight`.

### T1.6 — Mech-side: generalize anubis pattern from single node to all SpotLight_

**Files:** `mclib/mech3d.cpp` (anubis block at [:3333-3383](mclib/mech3d.cpp)), `mclib/mech3d.h` (`Mech3DAppearance` class at [:298](mclib/mech3d.h) — C-r2 C4 fix: NOT Mech3D, NOT MechAppearance)

**Adversarial review C-r1 M3 fix:** the existing anubis `lightCircleNodeIndex == -1` lazy-init key is a per-NODE state, not per-vector. Generalising naively to `spotlightNodeIds_.empty()` would skip re-registration after the first walk completes (since vector is non-empty thereafter, no further nodes would ever register). Correct lazy-init: a SEPARATE `bool spotlightsRegistered_` flag set true after the walk completes.

**Pre-task design decision needing user sign-off (C-r2 M6):** existing anubis uses **TG_LIGHT_SPOT** with `spotDir` + `maxSpotLength` populated ([mech3d.cpp:3373-3377](mclib/mech3d.cpp)). T1.6 chooses **TG_LIGHT_POINT** for v1 consistency with T1.4 building registration. This is a known v1 visual tradeoff (omnidirectional bloom instead of forward cone for mech spotlights). Surface to user before T1.6 lands. If user wants SPOT preserved for mechs, the path is: T1.6 uses SPOT, derive `spotDir` from child node local transform, set `maxSpotLength = falloff-outer`.

**Changes:**
- In `Mech3DAppearance` ([mclib/mech3d.h:298](mclib/mech3d.h)) header: add THREE members alongside the existing `pointLight` / `lightId` / `lightCircleNodeIndex`:
  ```cpp
  // (E) generalised SpotLight_ children. NOT the same as `pointLight`
  // above (anubis hard-coded SLCircle_anubis searchlight). Different
  // node-name match; coexist until T3.x decides on consolidation.
  std::vector<long>         spotlightNodeIds_;
  std::vector<TG_LightPtr>  spotlightLights_;
  std::vector<DWORD>        spotlightSlotIds_;
  bool                      spotlightsRegistered_;
  ```
- Initialize `spotlightsRegistered_ = false` and clear vectors in the existing init path alongside `lightCircleNodeIndex = -1`.
- Replace the anubis `if (!pointLight && eye->isNight)` lazy-init block with:
  ```cpp
  if (isSpotlightRealEnabled() && !spotlightsRegistered_ && eye->isNight) {  // C-r3 C2: no parens
      // Walk mechShape children using the same canonical pattern as T1.4
      // (msl.h:431 GetNumShapes + msl.h:438 GetShapeRec + tgl.h:951 GetIsSpotlight).
      // Same C-r4 C1/M1 protected-access fix as T1.4 — Mech3DAppearance is NOT
      // in the TG_MultiShape friend list (msl.h:251-256).
      for (int i = 0; i < mechShape->GetNumShapes(); ++i) {
          const TG_ShapeRec* recp = mechShape->GetShapeRec(i);
          if (!recp) continue;
          const TG_ShapeRec& rec = *recp;
          TG_Shape* c = rec.node;
          if (!c || !rec.processMe || !c->GetIsSpotlight()) continue;

          const char* nodeName = c->getNodeName();
          if (!nodeName) continue;
          long nodeId = mechShape->GetNodeNameId(nodeName);
          if (nodeId == -1) continue;

          TG_LightPtr light = (TG_LightPtr)malloc(sizeof(TG_Light));
          light->init(TG_LIGHT_POINT);            // v1 — POINT not SPOT (M6 sign-off)
          light->SetaRGB(0xffffff00);              // anubis-equivalent default
          light->SetIntensity(0.15f);              // anubis default
          light->SetFalloffDistances(50.0f, 250.0f); // anubis default
          long slotId = eye->addWorldLight(light);  // camera.h:805 returns long; -1 on overflow
          if (slotId < 0) { free(light); continue; }
          spotlightNodeIds_.push_back(nodeId);
          spotlightLights_.push_back(light);
          spotlightSlotIds_.push_back(static_cast<DWORD>(slotId));
      }
      spotlightsRegistered_ = true;  // M3 fix: register-once flag, not vector.empty()
  }
  ```
- LEAVE the existing anubis `lightCircleNodeIndex == -1` block IN PLACE — the "SLCircle_anubis" node-name is a HARDCODED single-node lookup that may or may not also be tagged `isSpotlight` (verify at write-time; SLCircle prefix vs SpotLight prefix). Don't break the existing anubis behavior. The two paths may double-register if SLCircle_anubis also has isSpotlight=true; check the node-name match at tgl.cpp:278.

**Verification:** mc2_24 with `MC2_SPOTLIGHT_REAL=1` shows first-night-visibility lazy allocation; further frames show only per-frame in-place updates (T1.7); registration count matches mech instance × spotlight-children-per-mech.

### T1.7 — Mech per-frame in-place update

**Files:** `mclib/mech3d.cpp` (UpdateGeometry, after the lazy-init block)

**Adversarial review C-r1 C5 fix:** anubis uses `visible && (sensorLevel > 4) && !InEditor` at [mech3d.cpp:3353](mclib/mech3d.cpp), NOT `inView`. `visible` is the "rendered-last-frame" semantic; `inView` is the "frustum-included-this-frame" semantic. Per [memory/cull_gates_are_load_bearing.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\cull_gates_are_load_bearing.md), inView gates UpdateGeometry execution — if inView==false the update doesn't run, so checking inView inside UpdateGeometry is partially redundant AND introduces a behavioral fork from the existing anubis pattern. Match anubis exactly.

**Member-scoping verified (C-r3 round-3 grep):** `visible` is on base `ObjectAppearance` ([appear.h:66](mclib/appear.h)); `sensorLevel` is on `Mech3DAppearance` ([mech3d.h:271](mclib/mech3d.h)); `InEditor` is the existing-scope identifier the anubis block already uses. All resolve correctly inside `Mech3DAppearance::UpdateGeometry`.

**Changes:**
- For each registered spotlight light (after init):
  - Look up the spotlight child node's current world position via `getNodeIdPosition(spotlightNodeIds_[k])`.
  - `light->SetPosition(&pos);`
  - `light->SetLightToWorld(&lightToWorldMatrix);` (per the anubis pattern at mech3d.cpp:3378)
  - `light->active = (eye->isNight && visible && (sensorLevel > 4) && !InEditor);` — verbatim anubis gate semantics + isNight (C-r3 C2: bare field, no parens), no `inView` addition.
- NO `removeWorldLight` / `addWorldLight` per frame (R3).

**Verification:** Tracy capture on mc2_24 shows no per-frame pool churn. `addWorldLight` first-hit prints fire only at first-night-visibility per mech.

### T1.8 — Mech destroy hook: removeWorldLight

**Files:** Mech destruction site (grep `MechAppearance::destroy` or equivalent).

**Changes:** mirror T1.5 for the mech path. Walk vectors, removeWorldLight + free, clear.

**Verification:** Smoke run with multiple mech deaths (mc2_24 combat) doesn't leak slots. Confirm via trace counter and stderr at removeWorldLight.

### T1.9 — Stage 1 visual canary

**Files:** none (validation).

**Action:**
1. Smoke `mc2_04 --duration 30 --keep-logs` with `MC2_SPOTLIGHT_REAL=1` — expect: lamps no longer render as opaque triangles; surrounding terrain/props/mechs show illumination spillage.
2. Smoke `mc2_10` — expect: no regression; spotlight counts low.
3. Smoke `mc2_24` (Vedette/LRMC canary) — expect: Vedettes and LRMCs render correctly (the f77f135 revert canary). If any actor goes invisible, STOP and surface.
4. RenderDoc capture on one mc2_04 frame: confirm no cone draws from static_prop batcher with the spotlight bit; confirm `worldLights[2..N]` populated.

**Commit:** none (validation gate); if all four steps pass, proceed to Stage 2.

---

## Stage 2 — Default-on flip + soak

### T2.1 — Flip the env-gate default

**Files:** wherever T1.2 reads `MC2_SPOTLIGHT_REAL`.

**Changes:** invert the default. New behavior: `MC2_SPOTLIGHT_REAL=0` is the opt-out; default behaves as if `=1`. Update startup banner.

**Verification:** smoke tier1 with no env vars passes (matches T1.9 with env on). Smoke with `MC2_SPOTLIGHT_REAL=0` passes (matches pre-T1.1 behavior).

### T2.2 — Soak

**Action:** User-driven canary across all tier1 + mc2_04 + mc2_24. Multi-day soak in master session. Watch for: visual regressions during mission transitions, save/load edge cases, day/night transition behavior, Vedette/LRMC integrity.

**Commit:** none (validation gate).

---

## Stage 3 — Delete the gated code (substitutive completion)

Only after Stage 2 soak passes.

### T3.1 — Remove env-gate

**Files:** T1.2 + T1.3 + T1.4 + T1.5 + T1.6 + T1.7 + T1.8 callsites.

**Changes:**
- Delete the env var read.
- Delete `isSpotlightRealEnabled()` guards. The new code paths become unconditional.
- Keep the trace counter (gated on `MC2_SPOTLIGHT_REAL_TRACE`) for future diagnostics per the Debug Instrumentation Rule (demote-not-delete).

### T3.2 — Delete the dead static-prop spotlight bit

**Files:** `GameOS/gameos/gos_static_prop_batcher.cpp` (submitMultiShape), `shaders/static_prop.vert`

**Changes:**
- Remove the `flags |= (1u << 2)` branch for spotlight children entirely (cone is never drawn now).
- In `static_prop.vert`, remove the bit-2 read path for kFlagIsSpotlight if it's no longer referenced by any live draw.

### T3.3 — Mech batcher skip cleanup

**Files:** `GameOS/gameos/gos_mech_batcher.cpp:553-563`

**Changes:**
- Update the comment block to reflect the new architecture (spotlights are now handled as TG_Lights via MechAppearance, not skipped here). Keep the geometry skip (spotlight children should never emit cone geometry through any path).

### T3.4 — Final smoke + Tracy capture

**Action:**
- mc2_04 + mc2_10 + mc2_24 smoke after deletion.
- Tracy capture: no `cone_emit` / spotlight packet drawn anywhere.
- RenderDoc: `worldLights` populated; static_prop draws contain no spotlight packets; mech draws contain no skipped spotlight children.

**Commit:** none — Stage 3's value is the deletion commits T3.1-T3.3, not the validation run.

---

## Out-of-scope items (file as follow-ups, not blockers)

- **OQ2 / TG_LIGHT_SPOT directional cones:** v2 work if v1 POINT illumination feels too omnidirectional.
- **OQ3 / per-light color from texture sampling:** v2 if all-warm-yellow lamps don't fit mission moods.
- **OQ4 / falloff distances from cone shape bbox:** v2 if hardcoded distances look wrong on specific shapes.
- **OQ7 / day-night transition mid-mission:** if MC2 supports time-of-day transitions, the `active` flag re-eval is per-frame so it should already work; not a separate task.
- **LIGHTBAKE v2 baked path:** only if dynamic pool overflows OR per-shape best-16 truncates visibly. With 1024 pool and "extremely low" actual population, neither is expected.
- **Vedette/LRMC anubis-leak audit:** the existing anubis path has a documented `removeWorldLight` gap (advisor flagged). (E) doesn't make it worse but doesn't fix it. File as standalone follow-up.

---

## Substitutive completion criteria (the spec's §5 step 4)

`(E)` is "done" when ALL of these hold:

1. Build under default flags (no `MC2_SPOTLIGHT_REAL` env set) shows the new behavior.
2. RenderDoc capture on mc2_04: zero static-prop draws contain spotlight-tagged packets.
3. RenderDoc capture on mc2_24: zero mech-batcher skip-spotlight events; mechs with spotlight nodes show illumination spillage on surrounding terrain/buildings.
4. `worldLights[2..N]` populated; `addWorldLight` first-hit count matches T0.3 baseline (within combat variance).
5. Visual canary: mc2_04 night-time lamp posts illuminate the ground around them (NOT just the building they sit on).
6. mc2_24 Vedette/LRMC integrity unchanged from baseline.
7. Mission reload (load mc2_04 twice in sequence): no slot leak in worldLights.
8. T3.1-T3.3 deletion commits have landed; env gate is gone from code.

## Cross-references

- Spec: [docs/superpowers/specs/2026-05-20-spotlight-real-illumination-design.md](../specs/2026-05-20-spotlight-real-illumination-design.md)
- Pattern source: [mclib/mech3d.cpp:3333-3383](../../mclib/mech3d.cpp) (anubis searchlight)
- Memory: [feedback_static_prop_subpass_program_switch.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_static_prop_subpass_program_switch.md) — why we don't repeat f77f135
- Memory: [spotlight_billboards_static_prop_opaque_bug.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\spotlight_billboards_static_prop_opaque_bug.md) — the original bug analysis
- Memory: [mission_load_inits_mirror_init_per_subsystem.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\mission_load_inits_mirror_init_per_subsystem.md) — mission init/teardown discipline
- Memory: [cpp_glsl_ubo_struct_lockstep.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\cpp_glsl_ubo_struct_lockstep.md) — for any future shader-array bump
- Memory: [feedback_class_layout_change_needs_clean_first.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_class_layout_change_needs_clean_first.md) — T1.1 build discipline
