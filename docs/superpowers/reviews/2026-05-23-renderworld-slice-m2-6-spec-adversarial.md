# Adversarial review — RenderWorld Slice M2.6 (mech-pickup) spec

- Reviewer pass: adversarial-plan-review skill
- Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-6-mech-pickup-spec.md`
- Resolutions: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-6-mech-pickup-spec-resolutions.md`
- Reviewed against HEAD source (M2.5 SHIPPED)

## REVIEW STATUS: CONDITIONAL-PASS

### Executive summary (5 lines)

1. Spec is well-grounded: all spot-checked file:line citations match HEAD verbatim (`RenderWorld.h:157-167`, `RenderWorld.cpp:717-726`, `missiongui.cpp:1273-1278`, `gameobj.h:409`, `mover.h:730`, `mech.cpp:1338`, `missiongui.cpp:6186-6271`, boot banner :484). META-FIX scope (Q3) is coherent and substitutive-proof gate is named.
2. **CRITICAL-1: `partId` is NOT lifetime-stable** — `setPartId` is called in `objmgr.cpp:804` BEFORE `init(true, objType)` runs (good), but `mission.cpp:2987` **re-assigns** `partId` to encode commander/group/mate AFTER mover->init has already run syncSpawn. The gameObjectId cookie stored in `s_objectRecords` is stale relative to final partId. Spec's claim "stable across the lifetime of the BattleMech instance" (Section 6.2) is wrong.
3. **MAJOR** issues: fog predicate has a `ShowMovers` / `allUnitsDestroyed` carve-out (`missiongui.cpp:1272`) the spec mech-fog-gate does not mirror; new `MechRenderAdapter.h` decl must use forward-decl for `BattleMech` (spec already addresses, but the firewall note should be promoted to a hard rule, not a "MAY"); META-FIX Gate 6 grep is incomplete (`tryStaticPropPick` method name itself survives the rename — should grep that too or rename it).
4. **MINOR** issues: missing in-repo consumer grep for `STATIC_PROP_PICK v1` schema before retirement (spec defers this to plan but the survey is mechanical and should be done at spec time); the 5-commit sequence (Section 17 step 5) bundles symbol rename + log rename + CLAUDE.md update — atomicity is fine but Gate 6 must run AFTER step 5 commit, not earlier.
5. Conditional on: (a) resolve partId-staleness (re-spec the cookie OR re-issue `setRenderWorldHandleForAdapter`-style cookie update after mission load OR pick a different identifier OR document that the cookie is "partId at spawn time, not necessarily current"); (b) extend mech-fog gate to honor `ShowMovers`/`allUnitsDestroyed`; (c) tighten Gate 6 grep set.

---

## CRITICAL findings

### CRIT-1. `partId` is not lifetime-stable — gameObjectId cookie can be stale

**Spec claim (Section 6.2, lines 789, 800):**
> "Stable across the lifetime of the BattleMech instance."
> "Already serialized into save-games."

**Evidence against:**
- `code/objmgr.cpp:804` (verified): `setPartId(mechs[numMechs], -1, -1, -1);` runs at `newMech()` pre-allocation time. `GameObjectManager::setPartId` (`objmgr.cpp:3243-3250`) computes a partId via `calcPartId` and assigns. OK so far.
- `code/mech.cpp:1338` (verified): `GameAdapters::Mech::syncSpawn(*m3d, 0u);` runs inside `BattleMech::init(bool, ObjectTypePtr)` at :1257. Mission flow: `mission.cpp:1033` calls `mover->init(true, objType)` which invokes this overload. At this point partId is the value newMech assigned (good).
- `code/mission.cpp:2987` (verified):
  ```
  ObjectManager->setPartId(ObjectManager->getByWatchID(parts[groupMates[curMate]].objectWID),
                            commandersToLoad[curCommanderId][0], numGroups, curMate);
  ```
  This RE-ASSIGNS partId for every group-roster mech AFTER `mover->init` has already run syncSpawn. So the `gameObjectId` baked into `RenderObjectRecord.gameObjectId` is the NEW-MECH partId, not the final commander-encoded partId.

**Consequence:**
- `LookupResult.gameObjectId` returned through M2.6 logs will not match `bm->getPartId()` for any mech that was placed into a group during mission load. The log line prints both (Section 6.3 hit format includes `gameObjectId=%u partId=%ld`), so they will visibly diverge in user-driven canary. Anyone diffing the two will think it's a bug.
- More importantly: any future M2.7 `select-by-handle` slice that uses the stored cookie to look up the BattleMech will look up the WRONG mech (or none, depending on whether partId is unique).

**Mitigation options:**
- (a) Use `getHandle()` (`gameobj.h:417`, returns `GameObjectHandle` — opaque struct; needs uint32_t packing inspection, but it IS the object-pool handle and IS lifetime-stable).
- (b) Use `getWatchID(false)` (`gameobj.h:421` — pass `assign=false` to avoid the side-effect spec correctly flagged; spec already rejected `assign=true` for the side effect, but `assign=false` is read-only and existing watch IDs are set by mission loader explicitly: see `mission.cpp:1032` `parts[partIndex].objectWID = mover->getWatchID();`). However, `newMech` (`objmgr.cpp:805`) sets `watchID = 0` explicitly, so this still needs verification that the mission loader has assigned by syncSpawn time.
- (c) Defer cookie population — add a `GameAdapters::Mech::updateCookie(Mech3DAppearance&, uint32_t)` that gets called from `Mover::setPartId` or from mission load tail (after all setPartId fan-out completes).
- (d) Document the cookie semantics honestly: "spawn-time partId; may diverge from current partId after mission group assignment." This is the minimal fix — pure spec change, no behavior delta — but the M2.6 log lines should then NOT print both side by side as if they should match.

**Recommendation:** Spec author re-resolves before plan. Option (d) is the smallest-delta path consistent with inspect-only v1; option (b) with verification is the cleanest path if it works.

---

## MAJOR findings

### MAJ-1. Fog predicate has a `ShowMovers`/`allUnitsDestroyed` carve-out the spec mech-fog gate ignores

**Spec claim (Section 6.3, lines 882-888):**
> "The CPU pick at `code/missiongui.cpp:1273-1278` nulls under-threshold hostile targets with `((Mover*)target)->conStat < CONTACT_SENSOR_QUALITY_1`. M2.6 mirrors this rule on the resolved BattleMech."

Implementation: `const bool visible = (bm->conStat >= CONTACT_SENSOR_QUALITY_1);`

**Evidence — full predicate at `code/missiongui.cpp:1267-1278` (verified):**
```cpp
target = ObjectManager->findObjectByMouse(mouseX, mouseY);
if ( target )
{
    if ( bGui )
        target = 0;
    else if ( target->isMover() && !ShowMovers && !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]))
    {
        if ((target->getTeamId() != Team::home->getId()) &&
            !target->isDisabled() &&
            (((Mover *)target)->conStat < CONTACT_SENSOR_QUALITY_1))
            target = NULL;
    }
    ...
}
```

The fog null-out is gated by an OUTER condition: `!ShowMovers && !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID])`. When `ShowMovers` is on (debug / cheat / spectator mode) OR all commander units are destroyed (end-of-game reveal), CPU pick ALREADY allows hostile mech selection through fog. The same pattern repeats at `mech.cpp:6332`, `gvehicl.cpp:3752/3898` for other fog-sensitive consumers — this is the engine-wide fog convention.

**Spec mech-fog gate as written:** unconditionally gates on `conStat`. Under ShowMovers, the user's CPU pick selects the hostile mech (legacy behavior preserved), but the M2.6 inspect log is SUPPRESSED — a user-visible inconsistency ("I can select it but inspect log is silent").

**Recommendation:** Mirror the OUTER predicate too:
```cpp
const bool fogApplies = !ShowMovers &&
                        !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]);
const bool visible = !fogApplies ||
                     bm->isDisabled() ||
                     (bm->conStat >= CONTACT_SENSOR_QUALITY_1) ||
                     (bm->getTeamId() == Team::home->getId());
```
(Note: spec also forgot that the CPU predicate is friendly-aware via `getTeamId() != Team::home->getId()` — friendly mechs are NEVER fog-nulled. M2.6 mech caller only ever sees hostile mechs reach it because friendly mechs are caught by mover-first short-circuit, so the friendly carve-out is benign by accident — but document it.)

**Plan-stage:** add a Gate-8 user-driven canary row for `ShowMovers=1` + fog-of-war hostile mech click.

### MAJ-2. Gate 6 (META-FIX substitutive grep) is incomplete

**Spec Section 10 Gate 6 (verified, lines 1260-1264):**
```
grep -rnE 'setLastStaticPropPick|clearLastStaticPropPick|getLastStaticPropPick' \
    RenderWorld/ code/ GameAdapters/
grep -rnE 'StaticPropSelectionDebugState' RenderWorld/ code/ GameAdapters/
grep -rn '\[STATIC_PROP_PICK v1\]' RenderWorld/ code/ GameAdapters/
```

**Gap:**
- `tryStaticPropPick` (the method name itself on `MissionInterfaceManager`) survives the META-FIX as-spec'd. So does `IsStaticPropPickEnabled` / `IsStaticPropPickDebugEnabled` (spec Section 9 explicitly preserves them as the static-prop category gates — separate from the schema). This is INTENTIONAL — but the naming is now confused: the category gate keeps the "STATIC_PROP_PICK" string in its env-var name (`MC2_STATIC_PROP_PICK`) while the log/state slot retire that string. Greppers will find the env var and assume the schema lives.
- Verified: 20 files currently match `STATIC_PROP_PICK v1` (grep across worktree). Several are doc/spec/review files (those legitimately retain the historical reference). Source files that match: `RenderWorld/RenderWorld.cpp`, `RenderWorld/RenderWorld.h`, `code/missiongui.cpp`, `code/gameplay_pick.h`, `code/missiongui.h`. The header `RenderWorld.h` carries DOC COMMENTS that mention the schema (e.g. M1.6 doc block). Gate 6 will FAIL if the spec mechanically removes only emit sites — doc comments must be updated in lockstep.
- Also missing from Gate 6: the boot-banner string format `static_prop_enabled` / `static_prop_debug` (which the spec keeps but with the new `[GAMEPLAY_PICK v1]` prefix) — verify the grep doesn't accidentally catch the renamed banner.

**Recommendation:** Expand Gate 6 to:
```
# Forbidden: old symbol names
grep -rnE 'setLastStaticPropPick|clearLastStaticPropPick|getLastStaticPropPick|StaticPropSelectionDebugState' \
    RenderWorld/ code/ GameAdapters/
# Forbidden: old schema string in any form (including doc comments)
grep -rn '\[STATIC_PROP_PICK v1\]' RenderWorld/ code/ GameAdapters/
# Required: new schema present at every renamed site
grep -rn '\[GAMEPLAY_PICK v1\]' RenderWorld/ code/ GameAdapters/
```
And add an explicit "doc comments in `RenderWorld.h` line ~93 / ~97 / ~178-194 / ~196-213 must rename `[STATIC_PROP_PICK v1]` -> `[GAMEPLAY_PICK v1] kind=StaticProp` references" task in the plan.

### MAJ-3. In-repo consumer grep is deferred to plan but is a precondition for the META-FIX claim

**Spec Section 16 Q1 (verified, lines 1685-1696):** "Plan stage should grep `tests/` + `scripts/` + `.claude/` for the M1.6 symbol/schema strings to surface in-repo consumers."

**Why this is MAJOR not MINOR:** The META-FIX-vs-PATCH ruling (Section 13) hinges on full substitutive retirement. If even ONE in-repo consumer (e.g. a test harness `tests/smoke/*` log parser, a `.claude/` advisor, a `scripts/` post-smoke analyzer) consumes `[STATIC_PROP_PICK v1]` by name, then the slice must either (a) ship a one-release shim or (b) update the consumer in the same commit. The spec defers this survey to plan stage but the answer affects the META-FIX ruling itself — and the greybeard pass is supposed to rule on the META-FIX claim BEFORE plan.

**Recommendation:** Spec author runs the survey BEFORE the greybeard pass:
```
grep -rn '\[STATIC_PROP_PICK v1\]\|setLastStaticPropPick\|StaticPropSelectionDebugState' tests/ scripts/ .claude/
```
Findings either: (a) zero — META-FIX claim holds; (b) nonzero — list updated files in T-final or downgrade to PATCH with one-release shim.

### MAJ-4. `findMechByHandle` linear scan misses MLR-fallback mechs by construction

**Spec Section 4.2:** linear scan over `ObjectManager->getMover(i)` matching on `app->getRenderWorldHandle().raw() == h.raw()`.

**Trap:** Per M2.5 known-gap (CLAUDE.md "MLR-rendered mechs do not write object IDs"), an MLR-rendered mech's appearance DOES have a valid `mechRenderHandle` (M2 spawn-time registration), but the SUBSTRATE doesn't write that handle's raw bits to the pixel. So `lookupAtPixel` returns `outcome=miss`, the resolver is never called for MLR mechs. **This is correct** — but the spec's "Threat T3 MLR fallback ambiguity" defense (silent miss) is accurate only because of this construction.

**Subtle finding:** if `mlr_mech_draws>0` ever occurs (CLAUDE.md flags this as the trigger to re-spec M2.6), the resolver would still work correctly for the GPU-batched mechs while silent-missing on the MLR mechs — the gap is per-mech-instance, not per-mech-class. Spec should note this granularity: the "preserve mover-first legacy fallback" follow-on (per CLAUDE.md M2.5 known gap) needs to operate per-instance, not per-mission.

**Recommendation:** Add a Threat T3.5 to Section 14 noting that if MLR fallback path ever needs reintroduction, the gating is per-mech (check `mech.usesMlrFallback()` or equivalent), not per-mission. Document the per-instance trigger condition.

---

## MINOR findings

### MIN-1. Citation drift verification (skill step 2)

Spot-checked >5 citations across files. ALL MATCH HEAD:
- `RenderWorld.h:157-167` (LookupResult shape) — exact match.
- `RenderWorld.cpp:717-726` (lookupAtPixel copy site) — exact match.
- `RenderWorld.cpp:484` (boot banner) — exact match (`[STATIC_PROP_PICK v1] enabled=%d debug=%d`).
- `RenderWorld.h:185-194` (StaticPropSelectionDebugState) — exact match.
- `missiongui.cpp:6186-6271` (tryStaticPropPick body) — exact match.
- `missiongui.cpp:1273-1278` (fog predicate) — exact match.
- `mech.cpp:1338` (syncSpawn call site, `gameObjectId=0u`) — exact match.
- `mover.h:730` (conStat field, public scope) — exact match.
- `gameobj.h:409` (getPartId, returns `long`) — exact match.
- `gameobj.h:421` (getWatchID with `assign=true` default) — exact match.
- `gameobj.h:462` (isMech) — exact match.
- `mech3d.h:487` (getRenderWorldHandle) — exact match (recon cited :487-489).

No citation drift. Spec author's "every cited symbol grep-verified at write time" claim is accurate.

### MIN-2. "Existing/Replace" blocks accuracy

Spec Section 4.1 / 4.3 / 5 use Existing/Replace blocks for `LookupResult`, `lookupAtPixel` copy, `StaticPropSelectionDebugState`, `tryStaticPropPick` hit branch. Each verified against HEAD verbatim — match exactly. Section 6.1 Existing block for `MechRenderAdapter::syncSpawn` line range was simplified (omits the `s_mechs_registered++` / banner emit between lines 87 and 120 in the real source), but the replacement is for the CALLER at `mech.cpp:1338`, not the adapter body, so the simplification is benign.

### MIN-3. Two-call cost projection (skill focus area 4)

Spec Section 7 claims ~200µs total per click, 2ms/sec ceiling. The cost is dominated by `glReadPixels` synchronous stall (substrate documented "stalls the GPU to read the prior frame's attachment-2" at `RenderWorld.h:171`). The actual cost on the target driver is not benchmarked; "<100us" is a guess from the M1.5 spec. At 4 callers (M3+M4), 400us/click is plausible but unmeasured. Spec correctly flags the optimization deferral in Section 16 Q2.

**Recommendation:** Plan-stage Gate 4 self-test should record the measured `lookupAtPixel` duration once for posterity (single Tracy zone or stderr timing print under MC2_MECH_PICK_DEBUG=1) so the optimization trigger at M3/M4 is data-driven.

### MIN-4. Env var registry / CLAUDE.md "Tier-1 instrumentation env vars" section

CLAUDE.md has a "Tier-1 instrumentation env vars" section. Adding 3 new env vars (MC2_MECH_PICK / _DEBUG / _PIERCE_FOG) — spec mandates CLAUDE.md M1.6 entry update (T6) but does NOT mention extending the env-var section. Greppers asking "what env vars exist?" will get an out-of-date answer.

**Recommendation:** Add to T-final: extend CLAUDE.md "Tier-1 instrumentation env vars" with the 3 new entries (and the M1.6 ones if they were also missing — verify; M1.5/M1.6 may have skipped this).

### MIN-5. Greybeard "sample-size-of-two" defense is well-argued but optimistic

Section 13 anticipates the greybeard counter ("sample-size-two") and responds with "M3/M4 will add 3rd/4th." This is correct reasoning but optimistic: M3 (terrain) and M4 (VFX) are not yet specced. If their final scope ends up NOT adding gameplay-pick consumers (e.g. terrain might never be inspect-pickable, VFX is unlikely to be pickable at all), the META-FIX retires a bug class that wouldn't have multiplied.

**Recommendation:** Greybeard pass should rule explicitly on whether M3/M4 are committed to extending the gameplay-pick surface. If yes, META-FIX-now is correct. If no, downgrade to PATCH and ship the kind guard + per-kind schema retention; rename to GAMEPLAY_PICK only when the second pickable category genuinely lands.

### MIN-6. Spec asks for `findMechByHandle` in `GameAdapters::Mech::` namespace — declaration site

Section 4.2 says "Add to `GameAdapters/MechRenderAdapter.h` after `destroyMech` declaration around line 52". Verified HEAD MechRenderAdapter.h ends at line 56 (`} // namespace GameAdapters` at 55, EOF). Adding after `destroyMech` (line 52) inside namespace is mechanically fine. Spec says header MAY forward-declare BattleMech; should be MUST (the firewall comment at lines 1-13 forbids `mech.h` from the header, only the `.cpp` may include it).

**Recommendation:** Change "MAY forward-declare" to "MUST forward-declare; MUST NOT include mech.h".

### MIN-7. Self-test step 6 invariant inversion ambiguity

Spec Section 10 Gate 4 step 6: "With the synthetic handle from step 2: expect `nullptr` (the synthetic mech is a RenderWorld-only record; no BattleMech instance exists in ObjectManager)."

This test depends on `ObjectManager` being initialized at self-test time. `RenderWorld::init()` runs early (before mission load). At that point `ObjectManager` may be `nullptr`. Spec resolver code (Section 4.2) handles this: `if (ObjectManager == nullptr) return nullptr;`. But then step 6 passes trivially (nullptr returned) even if the resolver loop was wrong. The self-test is vacuous in that case.

**Recommendation:** Either (a) move the `findMechByHandle` self-test to a later gate that runs post-mission-load (e.g. integrate into `[MECHBATCHER v1] onMapLoad` sequence), or (b) document that step 6 is "early-init vacuous pass" and add a stronger gate that runs during mission with at least one mech alive.

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **partId stability (CRIT-1).** Pick one: (a) accept stale cookie + document semantics honestly; (b) use a different identifier (handle / watchID-assign-false); (c) add a post-mission-load cookie-update path. Affects: Section 6.2 rationale; hit-log field semantics; future M2.7 design.

2. **MAJ-1 fog carve-out scope.** Mirror the engine-wide `ShowMovers`/`allUnitsDestroyed` carve-out in the mech-fog gate (recommend yes — preserves engine-wide invariant) or leave the M2.6 inspect log strictly conservative (silent under fog even when ShowMovers exposes the mech CPU-side). Affects: Section 6.3 fog gate code; Gate 8 canary table.

3. **MAJ-3 in-repo consumer survey.** Run BEFORE greybeard pass so the META-FIX claim is sound. If any consumer found, shim-or-update decision lands in spec, not plan.

4. **MIN-5 META-FIX-now vs defer.** Greybeard pass should rule on M3/M4 gameplay-pick commitment. If non-committal, demote to PATCH (kind guard only; schema rename deferred until second pickable category genuinely materializes).

5. **MAJ-2 Gate 6 expansion.** Mechanical fix but the doc-comment update in `RenderWorld.h` is non-trivial in surface area and must be in the same commit as the symbol rename, or Gate 6 fails on doc strings.

REVIEW STATUS: CONDITIONAL-PASS
