# RenderWorld Slice M2.6 -- Mech Pickup Plan Adversarial Review

REVIEW STATUS: CONDITIONAL-PASS

Executive summary (5 lines):
- Plan is structurally sound. All major file:line/symbol citations grep-verify: `LookupResult` at `RenderWorld.h:157`, M1.6 storage/setter at `RenderWorld.cpp:99/729-763`, M1.6 hit branch at `code/missiongui.cpp:6216-6242`, fog predicate at `code/missiongui.cpp:1272-1278`, boot banner at `RenderWorld.cpp:484`, adapter shape at `MechRenderAdapter.{h,cpp}`, `ObjectManager`/`getMover(long)`/`MoverPtr`/`isMech()` all present, `RenderCore::Handle::raw()`/`isValid()` present, `kMechHandleBase=0x00010000` per M2 ship.
- 0 CRITICAL findings; 4 MAJOR; 5 MINOR. The MAJOR set is recoverable with mechanical edits before executor handoff and does not require an architectural revision pass.
- MAJOR-A (Task 4 atomicity): Task 4's boot-banner rewrite emits the NEW `[GAMEPLAY_PICK v1] ...enabled=...` line yet Task 5's Gate 6 grep is `[STATIC_PROP_PICK v1]`-only -- but the static-prop hit/miss LITERALS at `missiongui.cpp:6229/6252` are still the old name at end of Task 4. Inter-commit state at HEAD-of-Task-4 is internally inconsistent (banner = new schema; hit/miss logs = old schema). This passes smoke (no clicks) but anyone bisecting will hit a single commit where the banner advertises one schema while emit sites use the other. Make Task 4 either keep the banner verbatim OR drop the Step 3 banner rewrite into Task 5.
- MAJOR-B (citation drift census incomplete): Plan enumerates 6 forgotten doc-comment / storage / lifecycle hits. Grep at write-time shows two more not in the drift table: `RenderWorld.cpp:93` ("set by setLastStaticPropPick from the gameplay-side tryStaticPropPick helper" -- doc comment; plan Section 5 lists `:93` only as part of spec's original five) and the `IsStaticPropPickEnabled` consumer-list doc comment at `RenderWorld.h:93` (line `"MissionInterfaceManager::tryStaticPropPick guard"`). These will trip Gate 6's strict `STATIC_PROP_PICK` literal-pattern grep only on `\[STATIC_PROP_PICK v1\]`, NOT on bare-symbol matches -- but Gate 6's FIRST and SECOND grep ARE bare-symbol matches (`setLastStaticPropPick|...|StaticPropSelectionDebugState`). Plan must also rename `tryStaticPropPick` consumer-list mentions in remaining doc comments OR exclude them explicitly from Gate 6.
- The plan's two design pivots from the spec (RunMechPickSelfTest moved to adapter TU; Task 4/5 split deferring mech-side setLastGameplayPick to Task 5) are both sound and explicitly defended. Approve with the corrections below; no architectural sign-off needed.

---

## CRITICAL

None.

---

## MAJOR

### MAJOR-A: Task 4 leaves an inter-commit state where the banner schema and the emit-site schema disagree

Plan line: Task 4 Step 3 (plan ~lines 657-681) replaces the M1.6 banner literal `[STATIC_PROP_PICK v1] enabled=%d debug=%d` with the new five-field `[GAMEPLAY_PICK v1] static_prop_enabled=...mech_pierce_fog=...` line.

Code reference: `RenderWorld/RenderWorld.cpp:484` -- current `std::fprintf(stderr, "[STATIC_PROP_PICK v1] enabled=%d debug=%d\n", ...)`. The other `[STATIC_PROP_PICK v1]` LITERALS at `code/missiongui.cpp:6229` (hit) and `:6252` (miss) are NOT renamed in Task 4 -- Task 4 deliberately defers schema-literal renames to Task 5 (per plan's own commit message at Task 4 Step 11: "M1.6 [STATIC_PROP_PICK v1] banner removed -- the existing M1.6 hit/miss schema literals are still in place; Task 5 retires them as part of the META-FIX commit").

Why it matters: end-of-Task-4 is a real commit. A user/agent bisecting at that SHA sees a boot banner saying "[GAMEPLAY_PICK v1] static_prop_enabled=1" but every hit/miss log still says "[STATIC_PROP_PICK v1] hit ...". For 30-second smoke runs with no clicks this is invisible. For a bisect over a future regression that involves the pick log it is actively confusing.

Recommendation (pick one):
- **Option 1 (smaller diff):** Move Step 3 (the banner rewrite) from Task 4 into Task 5. Task 4 keeps the existing banner literal `[STATIC_PROP_PICK v1] enabled=%d debug=%d`. Task 5's META-FIX commit then renames both the banner AND the hit/miss literals atomically.
- **Option 2 (preserves Task 4 banner-extension):** Keep the banner schema prefix `[STATIC_PROP_PICK v1]` in Task 4 while extending the field set (`enabled=%d debug=%d mech_enabled=%d mech_debug=%d mech_pierce_fog=%d`). Task 5 renames the prefix as part of the literal-rename sweep.

Option 1 is cleaner; both eliminate the inter-commit inconsistency.

### MAJOR-B: Citation drift census misses two doc-comment hits that Gate 6 will flag

Plan section: "Citation drift fixes" (plan ~lines 27-43) enumerates 7 drift hits beyond the spec's original 5. Spot-greps at write-time:

Code reference: `RenderWorld/RenderWorld.cpp:93` -- `// by setLastStaticPropPick from the gameplay-side tryStaticPropPick helper.` -- this IS one of spec Section 5's `:93/484/599/605` set so it WILL be hit by Task 5 Step 2 (the "doc comments at :93/:599/:605" generic instruction). Acceptable.

Code reference: `RenderWorld/RenderWorld.h:93` -- comment `// - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick guard` (above `bool IsStaticPropPickEnabled();`). Symbol `tryStaticPropPick` is INTENTIONALLY retained (M1.6 caller survives as the static-prop branch) but Gate 6's first grep is `setLastStaticPropPick|clearLastStaticPropPick|getLastStaticPropPick` -- `tryStaticPropPick` is NOT in the Gate 6 retired-symbol set per spec MAJOR-2(a) revision. SAFE; the symbol survives. False alarm; no fix needed for this one.

Code reference: `RenderWorld/RenderWorld.h:102` -- `// - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick miss branch` (above `bool IsStaticPropPickDebugEnabled();`). Same as above -- `tryStaticPropPick` retained. SAFE.

Real risk: there is a second `[STATIC_PROP_PICK v1]` literal in `RenderWorld/RenderWorld.cpp` doc comments beyond the spec's enumerated `:93/484/599/605` set. Confirmed at `RenderWorld.cpp:97-98` ("`[STATIC_PROP_PICK v1] miss` line is suppressed") in `IsStaticPropPickDebugEnabled` doc comment, NOT in the plan's drift table. Likewise the `RenderWorld.h:96-99` block.

Recommendation: extend Task 5's grep-then-rename pass to use the literal `Select-String` over BOTH `RenderWorld.cpp` AND `RenderWorld.h` for the pattern `\[STATIC_PROP_PICK v1\]` and rename every hit. The Gate 6 grep already covers `*.h` + `*.cpp` so if a hit is missed Gate 6 will reject the commit -- this is a discipline reminder, not a hard error.

### MAJOR-C: RenderWorld::destroyMech in init-time self-test will violate the "no real BattleMech in ObjectManager" assumption only if the synthetic handle bits collide with a real mech slot

Plan line: Task 6 Step 2 self-test impl (plan ~line 1741+) registers a synthetic mech via `RenderWorld::registerMech(desc)` at init-time, then calls `GameAdapters::Mech::findMechByHandle(h)`. The plan's comment block (~line 1768-1773) says: "Init-time has no real BattleMech in ObjectManager, so we expect nullptr."

Code reference: `RenderWorld/RenderWorld.cpp:792-815` (registerMech body, M2 ship): handles are allocated from `kMechHandleBase=0x00010000` upward, slot is added to `s_objectRecords`. At `RenderWorld::init()` time the table is empty so the FIRST synthetic registerMech returns index `0x00010000` with generation 1. `findMechByHandle` iterates `ObjectManager->getMover(i)` -- and the plan's resolver early-returns if `ObjectManager == nullptr`.

Verification grep needed (NOT in plan): is `ObjectManager` non-null at `RenderWorld::init()` time? `RenderWorld::init()` runs from `gameosmain.cpp` post-glewInit (per M1.5 wire). `ObjectManager` is a `GameObjectManagerPtr` defined in `code/objmgr.{h,cpp}` and constructed during mission load, not gameos init. The plan's resolver guards with `if (ObjectManager == nullptr) return nullptr;` -- safe.

But: the test then proceeds to call `RenderWorld::destroyMech(h)` (Task 6 Step 2 ~line 1783). `destroyMech` (engine-side, at `RenderWorld.cpp:825`) does NOT touch `ObjectManager` -- it only retires the record-table slot. Safe.

Subtler risk: the synthetic test's `RenderWorld::registerMech` allocates a REAL slot in `s_objectRecords`. Any subsequent mission's first mech registerMech then gets the NEXT slot (`0x00010001`). That increments `s_nextMechSlot` permanently across the process. M2.5 already runs `RunMechObjectIdSelfTest` which does the same thing and ships clean -- per M2.5 CLAUDE.md entry, `gpu_mech_id_writes=63836` on mc2_01 with the self-test active. So slot-counter consumption is not a real issue. Confirmed precedent.

Recommendation: no code change. Add a one-line comment to Task 6 Step 2 noting "shares M2.5 RunMechObjectIdSelfTest's slot-consumption discipline -- benign per M2.5 ship data." This is documentation hygiene only.

### MAJOR-D: Task 4 Step 1 doc comment claims to update the IsStaticPropPickDebugEnabled comment to reference `[GAMEPLAY_PICK v1]` schema BEFORE the literal exists in any emit site

Plan line: Task 4 Step 1 (plan ~line 580-585) replaces the `IsStaticPropPickDebugEnabled` doc comment to say `// the [GAMEPLAY_PICK v1] miss ...` BEFORE Task 5 actually renames the miss-emit literal at `code/missiongui.cpp:6252` from `[STATIC_PROP_PICK v1]` to `[GAMEPLAY_PICK v1]`.

Code reference: `code/missiongui.cpp:6252` -- current literal `"[STATIC_PROP_PICK v1] miss screen=..."`. Task 4 does NOT rename this. Task 4's doc-comment edit at `RenderWorld.h:96-99` describes a literal that does not exist in the binary until Task 5 lands.

Why it matters: same class of inter-commit inconsistency as MAJOR-A. End of Task 4 commits a header doc comment referencing a schema literal that no emit site uses.

Recommendation: defer the `IsStaticPropPickDebugEnabled` doc-comment rewrite to Task 5 (fold it into the same META-FIX commit that renames the miss-emit literal). Task 4 Step 1 then only adds the THREE new mech-pick decls without touching the M1.6 doc block.

---

## MINOR

### MINOR-1: Task 3 firewall claim "GameAdapters/ is NOT in SCOPE_DIRS so no firewall edit needed" -- correct but the forbidden-SYMBOLS list still bans `ObjectManager`

Code reference: `scripts/check-include-firewall.sh:32` -- `FORBIDDEN_SYMBOLS="Appearance BldgAppearance TreeAppearance GVAppearance Mech3DAppearance GenericAppearance ObjectAppearance ObjectManager Mission MechWarrior"`.

The forbidden-symbols sweep iterates over `SCOPE_DIRS` only (per script body), and `GameAdapters/` is excluded from `SCOPE_DIRS`. So the new `findMechByHandle` impl using `ObjectManager` is NOT flagged. Plan's claim holds.

Recommendation: no fix; the plan is right. Adding a one-line confirmation in Task 3 Step 5 ("forbidden-symbol scan does not iterate GameAdapters/") would make the audit explicit.

### MINOR-2: Plan's `findMechByHandle` linear-scan uses `static_cast<Mech3DAppearance*>(bm->getAppearance())` without isolating the `getAppearance() == nullptr` race

Plan line: Task 3 Step 3 impl (plan ~line 470-481).

Code reference: `code/mover.h:1129` defines `getAppearance()` as returning `AppearancePtr` (i.e. `Appearance*`). Pre-`syncSpawn` or post-`destroyMech` window: `getAppearance()` could be NULL. The plan guards with `if (app == nullptr) continue;` -- correct, but the comment block does not call out the race.

Recommendation: comment-only -- add "guards against pre-init or mid-destroy mech with NULL appearance (lifecycle race)."

### MINIR-3: Plan's `RunMechPickSelfTest` namespace placement justification is correct but the option-A/option-B language inside Task 6 Step 2 is contradictory

Plan text (Task 6 Step 2 ~lines 1793-1797): "Pick option B (file-scope free function)" but the code block above the choice block (~lines 1736-1740) ALREADY shows `} // namespace Mech` and `} // namespace GameAdapters` followed by the file-scope `void RunMechPickSelfTest() { ... }`. The "Re-check the file structure" subsection inserts AFTER both closing braces, which IS option B. The text is consistent but the option-A/option-B exposition reads as if the executor still needs to choose. Tighten to "use option B; option A is documented for reference only."

### MINOR-4: Three env-flag accessors in Task 4 Step 2 use lambda-init static idiom; plan instructs to "verify against existing M1.6 accessor"

Plan instruction (Task 4 Step 2 ~line 648-655) hedges by saying "if the existing pattern uses a different cached-getenv form, mirror it verbatim."

Code reference: grep at write-time for `bool IsStaticPropPickEnabled` impl would confirm whether the existing pattern is `static const bool kEnabled = [] { ... }()` or `static bool initialized; static bool cached; if (!initialized) ...`. Not blocking -- the plan's instruction to verify is correct. Just note that for executor predictability, plan could have pinned the exact M1.6 pattern at plan-write time rather than instructing the executor to re-derive.

Recommendation: optional plan tightening; executor will catch this in Step 2 verification.

### MINOR-5: Gate 6 grep exclusion clause says ".claude/worktrees/*/docs/superpowers/ MAY surface hits" but the actual exclusion regex is `\.claude` (catches the entire .claude tree)

Plan line: Task 5 Step 7 (plan ~line 1488) `Where-Object { $_.Path -notmatch '\\docs\\superpowers\\' -and $_.Path -notmatch '\\\.claude\\' }`.

The second clause `-and $_.Path -notmatch '\\\.claude\\'` excludes ALL of `.claude/` -- including `.claude/worktrees/nifty-mendeleev/CLAUDE.md` (which Task 7 Step 9 INTENTIONALLY updates to add a "renamed to [GAMEPLAY_PICK v1]" pointer to the old schema name -- per the M1.6-entry archaeology hint in Edit 9b). Gate 6 will trivially pass because CLAUDE.md is excluded; the intended pointer is preserved.

But: this means Gate 6 is structurally blind to violations inside `.claude/skills/`, `.claude/agents/`, and other `.claude/` subtrees. If an old `.claude/skills/*.md` happens to reference `[STATIC_PROP_PICK v1]`, the Gate 6 sweep will silently miss it.

Recommendation: tighten the exclusion to `-notmatch '\\\.claude\\worktrees\\[^\\]+\\(docs\\superpowers|CLAUDE\.md)'` so only the intended carve-outs are exempt; everything else under `.claude/` is still policed. This is hygiene, not a slice-blocker.

---

## Verification of named execution-time design decisions (from review prompt)

1. **RunMechPickSelfTest placement (adapter TU vs RenderWorld.cpp)**: VERIFIED CORRECT. Plan's option B (file-scope free function in `MechRenderAdapter.cpp` after both closing namespace braces) matches the existing `RunMechObjectIdSelfTest`/`RunGameplayPickSelfTest` forward-decl style at `RenderWorld.cpp:40-45`. `MechRenderAdapter.cpp` already includes `RenderWorld.h` and `mech3d.h`; Task 3 adds `code/objmgr.h`, `code/mover.h`, `code/mech.h` -- which transitively provides `BattleMech`, `Mech3DAppearance`, `ObjectManager`. No additional include needed for the test impl. Firewall script does not iterate `GameAdapters/` (verified at `scripts/check-include-firewall.sh:22`).

2. **7-task sequencing inter-task atomicity**: Task 2 committed BEFORE Task 5 means Task 2's diff uses the OLD setter names (`setLastStaticPropPick`/`StaticPropSelectionDebugState`/`[STATIC_PROP_PICK v1]`). Task 5 then renames these. Inter-task state at end of Task 2 compiles -- the old names still exist. The risk is documented under MAJOR-A (banner asymmetry) and MAJOR-D (doc-comment-vs-emit asymmetry); neither blocks compilation, only inter-commit semantic consistency.

3. **Task 4 banner rename before Task 5 META-FIX rename**: NOT COHERENT inter-commit. See MAJOR-A and MAJOR-D. Recommendation: defer Step 3 (banner) to Task 5.

4. **Citation drift fixes -- 6 additional doc-comment hits**: Spot-grep at write-time:
   - `code/gameplay_pick.h:42-45` "Field set mirrors the M1.6 [STATIC_PROP_PICK v1] hit/miss log printf args exactly." -- CONFIRMED, in scope.
   - `code/missiongui.h:265-269` "Emits [STATIC_PROP_PICK v1] hit/miss and updates RenderWorld debug state." -- CONFIRMED, in scope.
   - `code/missiongui.cpp:31-32` "// M1.6: IsStaticPropPickEnabled, lookupAtPixel, setLastStaticPropPick, // clearLastStaticPropPick, getLastStaticPropPick, IsObjectIdBufferEnabled." -- CONFIRMED, in scope.
   - `code/missiongui.cpp:6175` "(debug-state mutation + [STATIC_PROP_PICK v1] hit/miss logs)." -- CONFIRMED, in scope.
   - `RenderWorld/RenderWorld.cpp:99` storage var -- CONFIRMED, in scope.
   - `RenderWorld/RenderWorld.cpp:506` lifecycle clear -- CONFIRMED, in scope.
   All six listed in plan's drift table; all caught by Task 5 grep. Plan's drift census is accurate.

5. **Fog predicate verbatim diff against `code/missiongui.cpp:1272-1278`**: VERIFIED. Source reads:
   ```
   else if ( target->isMover() && !ShowMovers && !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]))
   {
       if ((target->getTeamId() != Team::home->getId()) && 
           !target->isDisabled() && 
           (((Mover *)target)->conStat < CONTACT_SENSOR_QUALITY_1))
           target = NULL;
   }
   ```
   Plan's mirrored predicate at Task 4 Step 6 (plan ~lines 826-835) decomposes into `showMovers`, `mpDefeat`, `hostile`, `disabled`, `sub_q1` and combines as `fogSuppresses = !showMovers && !mpDefeat && hostile && !disabled && sub_q1`. This matches the source semantics exactly (the outer carve-outs `!ShowMovers && !(MPlayer && ...)` gate the inner three-AND-clause; if either carve-out fires, fog does not suppress). Note plan drops `target->isMover()` -- safe because the caller has ALREADY kind-guarded on `r.lookup.kind == Mech` and resolved a `BattleMech*`, which IS a Mover. No operator-precedence drift. No paren drift. PASS.

6. **`findMechByHandle` resolver implementation**:
   - `ObjectManager::getMover(long)` -- VERIFIED at `code/objmgr.h:501`.
   - `ObjectManager::getNumMovers()` -- VERIFIED at `code/objmgr.h:450`.
   - `extern GameObjectManagerPtr ObjectManager` -- VERIFIED at `code/objmgr.h:588`.
   - `isMech()` on Mover -- VERIFIED at `code/mover.h:1751` (virtual).
   - Cast pattern `static_cast<BattleMech*>(mover)` after `isMech()` check -- matches M2 idiom (recon confirmed); semantically safe.
   - `Mech3DAppearance* app = static_cast<Mech3DAppearance*>(bm->getAppearance())` then `app->getRenderWorldHandle().raw() == target` -- VERIFIED `getRenderWorldHandle()` at `mclib/mech3d.h:487`; VERIFIED `raw()` at `RenderCore/Handle.h:53`.
   - Lifetime-stability claim (handle stable; mech survives unless destroyed; destroyMech idempotent per M2 `05f1f2d`) -- correct given M2 ship discipline.

7. **Three new env vars** -- Default-off documented for all three (plan Task 4 Steps 1-2 + boot banner emits both states). Banner field naming asymmetric (`static_prop_enabled` vs `mech_enabled`) -- intentional per spec MAJOR-2(b) "env-var name `MC2_STATIC_PROP_PICK` is INTENTIONALLY retained." `PIERCE_FOG` semantics short-circuit ONLY the fog predicate (not the kind-guard or stale-handle gate) -- VERIFIED at Task 4 Step 6 (plan line 822-825).

8. **Self-test init-time correctness**: Plan's option B places the test at file scope in `MechRenderAdapter.cpp` AFTER both closing namespace braces. Test:
   - Step 1: `RenderWorld::registerMech(desc)` -- works at init-time (record table exists from `RenderWorld::init()` callsite; per M2.5 precedent).
   - Step 2: `findMechByHandle(Handle::invalid())` -- guarded with early `if (!h.isValid()) return nullptr;` -- PASS expected.
   - Step 3: `findMechByHandle(h)` with synthetic handle -- `ObjectManager == nullptr` early-return likely (init runs pre-mission-load); returns nullptr; the "scan well-formed-ness" semantics hold because the test passes regardless of whether ObjectManager is null or empty.
   - Step 4: `RenderWorld::destroyMech(h)` -- per `RenderWorld.cpp:825` retires the synthetic slot. Clean.
   Plan correctly encodes "scan-validates-itself" pattern (test passes on either nullptr OR matching-bits non-null).

9. **CLAUDE.md M1.6 entry update**: Task 7 Step 9 Edit 9b instructs to replace literal `[STATIC_PROP_PICK v1]` with `[GAMEPLAY_PICK v1] kind=StaticProp` AND append a "(renamed to [GAMEPLAY_PICK v1] kind=StaticProp in M2.6)" note. Plan's archaeological-pointer discipline is correct. The M1.6 entry's load-bearing facts (handle ranges, gate sites at `code/missiongui.cpp:1460/1487/1690/1705`, user-driven mc2_03 canary "26 hits on distinct static-prop handles") would survive an in-place schema rename -- nothing in the M1.6 entry is structurally tied to the old schema literal beyond the literal mentions themselves.

---

## Architectural decisions that need user/advisor sign-off before revision pass

None. All findings are mechanical. The plan's two design pivots from the spec (RunMechPickSelfTest TU placement and Task 4/5 mech-side setter deferral) are explicitly documented and defended; both are correct.

---

## Recommended mechanical changes before executor handoff

1. **Pick MAJOR-A Option 1 or Option 2.** Either move the boot-banner rewrite from Task 4 Step 3 into Task 5, OR keep the `[STATIC_PROP_PICK v1]` prefix in Task 4 and rename it in Task 5.
2. **Address MAJOR-D.** Defer the `IsStaticPropPickDebugEnabled` doc-comment rewrite from Task 4 Step 1 to Task 5; Task 4 Step 1 should only add the three new mech-pick decls without touching M1.6 doc comments.
3. **Hygiene-only for MINOR-5.** Tighten Gate 6's `.claude/` exclusion so the sweep is not structurally blind to violations under `.claude/skills/` etc.
4. **Optional comment additions** for MINOR-2 (lifecycle race) and MINOR-3 (option-A/option-B language).

After these edits the plan is ready for executor handoff.

REVIEW STATUS: CONDITIONAL-PASS
