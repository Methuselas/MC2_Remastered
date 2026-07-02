# TECHSCRIPT-GAP-CLOSURE-1 — TechScript v1.5 (discussion #18) full-spec gap ledger + closure slices

**Lane:** BUILD — worktree `A:/Games/mc2-techscript`, branch `claude/techscript-gap`, base `c18618c4`.
**Mission:** close the gap between the engine's BrainSpecial runtime and Methuselas's TechScript v1.5 spec — implement the spec as written wherever sane.
**Inputs:** `A:/Games/mc2-controlmap-sample-1/.claude/BRAIN-DISCUSSION-18-INTEGRATE-1.md` (gap recon), `docs/MODDER-REPORT-techscript-brainspecial.md`, `docs/render-backend-seams/brain-arc-implementation-roadmap.md`, `code/brain_special_dispatch.{cpp,h}`, `code/mech_brain_runtime.h`, `tools/brain_dispatch_harness/`.

**Standing engine rulings honored (not re-litigated):**
1. Deterministic 7-tier task queue (`brain_task_queue.h`, key `(tier, frame_ms, seq, warrior)`) over an instruction-pointer VM with arbitrary `GOTO`.
2. Engine-owned spec authority: the normalized `BrainSpecialBody` DSL is canonical; `carver_v_enhanced` is a compatibility corpus, not law (roadmap Addendum #3).
3. APPLY-gate discipline: every behavior is default-OFF, gate-OFF byte-identical, forbidden-call guard enforced by `scripts/check_brain_relaxed_guard_doc.py`.
4. Name-collision ruling: internal label **BrainSpecial**; spec's `Runtime = "TechScript"` files work unchanged (MODDER-REPORT §Naming).

---

## Ledger — every spec feature → status

Statuses: **SHIPPED verbatim** / **SHIPPED adapted** (delta noted) / **PLANNED** (slotted slice) / **MISSING** (slice named) / **CONFLICT** (resolution honoring rulings). "THIS ARC" = built by this gap-closure branch (SHAs in §Closure log).

| # | Spec feature (v1.5) | Status | Detail / delta / slice |
|---|---|---|---|
| 1 | Native `TechSpecial { … Body { DO <verb> … } }` file format loaded+parsed | **SHIPPED verbatim** | Raw brace-block scanner (`parseBrainSpecialBody_RawScan`) + FitIni bracket fallback; `cacheMissionSpecialBody` at mission load (mission.cpp:3477/3517). Handles inline-quoted args (DISPATCH-LOADER-RAW-1). |
| 2 | Actions catalog (`Brain.*`, `OPORD.*`, `Unit.*`, …) | **SHIPPED adapted** | 17 verbs recognized; 6 GENERAL-slot effect verbs live (CorePower/Eject/Guard/MoveTo/Attack/Retreat) + ClearMoveOrders + SetTargetpriority as intents. Delta: effects land one-verb-per-slice on the APPLY ladder rather than all-at-once; remaining families (Audio/Video/Camera/Debug/String) = slice 1L-A/B/C (**PLANNED**, roadmap). |
| 3 | Conditions | **SHIPPED adapted** | `Unit.InState`/`NotInState`/`SetStateIf` (MC2_BRAIN_FSM). Var-equality condition primitive added by **WAIT_UNTIL (THIS ARC)**; generic condition vocabulary for IF = future `BRAINSPECIAL-IF-1` (**PLANNED**, S/M — reuses WAIT_UNTIL's evaluator). |
| 4 | Values (`Var.Get/Set`, symbols) | **SHIPPED adapted** | Per-unit auto-namespaced store (1D) + mission-scope store (1D-M, `MC2_BRAIN_VAR_MISSION`) + symbolic-arg resolver (`brain_symbol_resolve`). Delta: mission-scope flagged single-writer hazard — convergent with spec author's own concern. |
| 5 | Selectors (unit/target selector expressions) | **MISSING** | No selector grammar. Slice `BRAINSPECIAL-SELECTOR-1` (**M**) after verb families 1L; `UnitQuery.SetTargetpriority` + CONTACT_CRITERIA symbols are the seed. |
| 6 | Flow: `IF/ELSE/ENDIF` | **PLANNED (partial adapted)** | State-conditional gating (FSM stateGateOpen) covers the InState pattern; generic IF/ELSE/ENDIF = `BRAINSPECIAL-IF-1` (**S/M**) on top of THIS ARC's flow-gate + condition evaluator. |
| 7 | Flow: `WAIT`, `WAIT_UNTIL` | **SHIPPED adapted — THIS ARC** (`BRAINSPECIAL-FLOW-WAIT-1`, gate `MC2_BRAIN_FLOW`) | **SPEC-DELTA (deliberate):** NOT VM-blocking. The body re-executes every deterministic brain tick; `WAIT <sec>` closes a sequence gate for verbs after it until sim-time deadline (`getBrainTimeMs()`), then latches open. `WAIT_UNTIL Var "<k>" == <v> [scope=Mission]` gates until the condition holds, then latches. This is re-queue-at-tier semantics: no blocked VM, no saved instruction pointer — determinism keys hold. Root body only in v1 (chained bodies trace + skip, mirroring CALL-CHAIN-1A's effects-root-only precedent). |
| 8 | Flow: `STOP` | **SHIPPED adapted — THIS ARC** (same slice) | Gate OFF: parse-time sentinel skip (pre-existing). Gate ON: `STOP` ends body execution for the tick (verbs after it never run). Delta: "halt script" ≈ "halt this tick's re-execution" under the re-execution model; combined with latched WAITs this reproduces spec sequencing. |
| 9 | Flow: `LABEL` / `GOTO` | **CONFLICT — resolved: declined with substitute** | Ruling 1: arbitrary jumps can escape the 7-tier deterministic order and break MP/save-load replay. Resolution: structured flow only — latching WAIT/WAIT_UNTIL gates + FSM `SetState` transitions + `TechSpecial.Call` chaining express every corpus GOTO pattern (corpus max call depth = 2, zero cycles). GOTO will not be implemented; documented as spec-delta for the author. |
| 10 | Four scopes: `MissionSpecial` | **SHIPPED verbatim** | Entry-body selection rule (scenario_main → type=MissionSpecial → first); mission-level cache (GAP-A). |
| 11 | Four scopes: `UnitBrainSpecial` | **SHIPPED adapted** | Per-unit path = `<m>_ai.fit` Brain{} records + per-warrior `MechBrainRuntime` executing the specials body with unit context (wid, per-unit vars, FSM state). Delta: spec's per-unit *bodies* (distinct body per unit) arrive via `_ai.fit` unitRef → runtime; a per-unit `type=UnitBrainSpecial` block form can layer on later (**PLANNED**, S — parse rule only). |
| 12 | Four scopes: `GlobalSpecial` | **SHIPPED adapted — THIS ARC** (`BRAINSPECIAL-SCOPE-GLOBAL-1`, gate `MC2_BRAIN_SCOPE_GLOBAL`) | `global_specials.fit` (same directory as the mission specials) parsed at load; non-colliding blocks merged into the mission's special index. Mission-local wins on key collision (`[BRAIN_SCOPE_GLOBAL_SHADOWED]`). Delta: global specials are Call-targets + variantOf parents (a reusable library); they never provide a mission's root/entry body. |
| 13 | Four scopes: `CampaignSpecial` | **PLANNED (deferred)** | Deferred until campaign-progression need is real (recon §3 verdict). Slice `BRAINSPECIAL-SCOPE-CAMPAIGN-1` (**M**) — natural home after the campaign-generator arc lands emit seams. |
| 14 | `variantOf` inheritance | **SHIPPED adapted — THIS ARC** (`BRAINSPECIAL-VARIANTOF-1`, gate `MC2_BRAIN_VARIANTOF`) | `variantOf = "<parentKey>"` field on TechSpecial; resolution after global merge (a mission variant can inherit a global parent — "protected core Specials" as spec intends). Child with empty Body inherits parent verbs wholesale; child that re-declares Body overrides it. Chain depth ≤ 8, cycle guard. **SPEC-DELTA:** engine blocks have exactly one overridable section (Body), so override granularity = whole Body; per-section merge deferred until the engine spec grows more sections. Prereq `TECHSCRIPT-CALL-CHAIN-1A` verified SHIPPED at base (parse-time SpecialIndex exists). |
| 15 | Aliases (canonical key ↔ author shorthand) | **SHIPPED adapted — THIS ARC** (`BRAINSPECIAL-ALIAS-1`, gate `MC2_BRAIN_ALIAS`) | Pre-existing: 5 hardcoded aliases (coreEject/corePower/coreGuard/coreRetreat/Brain.CoreEject). THIS ARC: data-driven registry seeded with the built-ins + authorable `Aliases { <alias> = "<canonical>" }` block in the specials file + case-insensitive shorthand match (spec's `Tactic.Standard` → `tactic.standard`). Registry is first-token rewrite; args pass through. |
| 16 | Core Special Registry (40+ shipped Specials) | **PLANNED (content)** | Engine mechanism (global scope + variantOf + aliases) ships THIS ARC; the 40-special content pack is authoring work against the engine-owned spec — `BRAINSPECIAL-CORE-CONTENT-1` (**M/L**, content arc) once the 1L verb families land. |
| 17 | ServiceJobs (timed repair/reload/capture/sabotage state machine) | **MISSING — deferred by ruling** | Leaf feature, XL; off the foundation spine (recon §3, roadmap "Later"). `LOGISTICS-JOB-NETWORK-1` (**XL**, own arc). |
| 18 | Primitive domains beyond Movement/Combat/Brain-state | **PARTIAL / PLANNED** | Live: Movement, Combat, Brain-state, Sensor (engage/contact radius). Missing: Social/Help (RequestHelp switches parsed, runtime 1N), Logistics/Engineer/Capture/Sabotage (`OPORD.Capture/Repair` — MC2X-TCE stress, 1K addendum), Formation, Air ops, Debug (1L). Each slotted in roadmap. |
| 19 | Compatibility modes Legacy/Hybrid/Enhanced | **PARTIAL (shipped/planned)** | Field parsed (`brain_missionfit_oporbd.h:60`); Enhanced short-circuits ABL (RUNTIME-1B); Hybrid arbitration = slice 1M (**PLANNED**). |
| 20 | Deterministic runtime | **SHIPPED adapted** | 7-tier queue (`MC2_BRAIN_TASKQ`, tiers 0=Logistics..6=Background), fixed-tick option, intent queue + commit phase. Delta: determinism via stable-ordered queue + latched gates, not lockstep VM. |
| 21 | Save/Load (IP, wait-state, scoped vars, flags, ServiceJob refs) | **PLANNED** | Nothing serialized (mission-ephemeral policy, mech_brain_runtime.h header note). `BRAIN-SAVELOAD-1` (**M/L**) — scope now explicitly includes THIS ARC's wait-state latches + var stores. No instruction pointer exists to serialize (see #7) — the spec's hardest save/load item dissolves under the re-execution model. |
| 22 | Mission-editor authoring (BTCS-style line editor) | **PARTIAL + keystone THIS ARC** | Read-only Declarative Brain viewer + live waypoint/stance editor exist (`editor/UnitBrainPanel.cpp`). Keystone gap = raw brace-block writer (FitIniFile cannot write `Brain{}`/`TechSpecial{}` blocks) → **`FITBLOCK-WRITER-1` THIS ARC**. Then `EDITOR-BRAIN-PANEL-1` (**L**, editable panel) → `EDITOR-BRAINSPECIAL-LINEEDIT-1` (**L**, line editor). |
| 23 | ABL→AST→IR→FIT converter w/ provenance | **CONFLICT — resolved: declined-as-engine** | Ruling 2: engine owns the target spec, not the converter (Addendum #3). External tooling (the author's own converter included) emits the engine spec. Stays external. |
| 24 | Runtime architecture (POC 3 files ~3k LOC; VM loop) | **SHIPPED adapted** | Engine equivalent: `brain_special_dispatch.{cpp,h}` + `mech_brain_runtime.h` + `brain_missionfit_oporbd.{cpp,h}` (~3.5k LOC) with per-verb dispatch, expression/symbol eval, per-unit context binding. Delta: no IP-VM (see #7/#9/#20). |
| 25 | Infantry/power-armor patch (called-shot, mines, spotter) | **MISSING — deferred by ruling** | Leaf; `INFANTRY-CAPTURE-SABOTAGE-1` (**L**, later, needs infantry combat model touch-up). |
| 26 | Modding safety (protected core Specials, extend-not-overwrite) | **SHIPPED adapted — THIS ARC** | Delivered by the composition of #12 (global library) + #14 (variantOf, extend without overwrite) + #15 (stable canonical keys) + ruling 2 (engine-owned spec). |
| 27 | Naming: "TechScript" | **CONFLICT — resolved (standing ruling)** | Editor already ships an unrelated feature named TechScript (mission-trigger FSM, `editor_techscript.fit`). Internal label = BrainSpecial; `Runtime = "TechScript"` in content works unchanged. This ledger maps spec names → engine names throughout. |

### Spec-name → engine-name map (for the author)

| Spec (v1.5) | Engine |
|---|---|
| TechScript (the language) | BrainSpecial DSL (internal label only; files unchanged) |
| TechSpecial block | TechSpecial block (verbatim) |
| GlobalSpecial / CampaignSpecial / MissionSpecial / UnitBrainSpecial | `global_specials.fit` library / (deferred) / `<m>_specials.fit` entry body / `<m>_ai.fit` Brain{} + per-warrior runtime |
| WAIT / WAIT_UNTIL | `MC2_BRAIN_FLOW` latched sequence gates (re-queue semantics) |
| GOTO / LABEL | declined → `TechSpecial.Call` + `Unit.SetState` + latched gates |
| variantOf | `variantOf = "<key>"` (Body-level override) |
| Aliases | `Aliases { … }` block + built-in registry (`MC2_BRAIN_ALIAS`) |
| Compatibility Mode | `compatibilityMode` on Brain record (Legacy/Hybrid/Enhanced) |

---

## Gap arithmetic

Recon baseline (pre-arc): **~65% built/in-flight, ~20% planned-and-slotted, ~15% new.**

THIS ARC converts to shipped: alias registry (planned-S), GlobalSpecial scope (new), variantOf (new), WAIT/WAIT_UNTIL/STOP flow verbs (new/planned), FITBLOCK-WRITER keystone (planned-M, unblocks both editor items). Post-arc estimate: **~80% shipped, ~14% planned-and-slotted (IF-1, Hybrid 1M, 1L families, save/load, Campaign scope, editor panels, core-content pack), ~6% deferred-by-ruling leaves (ServiceJobs, infantry, converter-as-engine, GOTO).** 100% "as written" is intentionally unreachable on 3 items (#9 GOTO, #17 in-engine timeline, #23 converter) — each has a documented resolution honoring the standing rulings.

---

## Closure log (Phase B — one commit per slice)

| Slice | Gate | Commit | Proof |
|---|---|---|---|
| BRAINSPECIAL-ALIAS-1 (S) | `MC2_BRAIN_ALIAS` | (pending) | harness fixture `alias-registry` + A/B apply |
| BRAINSPECIAL-SCOPE-GLOBAL-1 (M) | `MC2_BRAIN_SCOPE_GLOBAL` | (pending) | harness fixture `scope-global-call` + shadow test |
| BRAINSPECIAL-VARIANTOF-1 (M) | `MC2_BRAIN_VARIANTOF` | (pending) | harness fixtures `variantof-inherit` / `variantof-override` |
| BRAINSPECIAL-FLOW-WAIT-1 (M) | `MC2_BRAIN_FLOW` | (pending) | harness `flowSequential` fixtures `flow-wait` / `flow-wait-until` / `flow-stop` |
| FITBLOCK-WRITER-1 (M) | (pure utility, zero engine callers) | (pending) | writer self-test in harness (`--fitwriter-selftest`): emit → reparse round-trip |

All slices: default-OFF, gate-OFF byte-identical, no new `setGeneralTacOrder` call-sites (relaxed-guard checker count stays 6), `check_brain_relaxed_guard_doc.py` + harness suite green per commit; smoke pair on a spare lane at arc end.
