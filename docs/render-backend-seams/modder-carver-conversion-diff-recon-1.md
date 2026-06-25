# MODDER-CARVER-CONVERSION-DIFF-RECON-1

**Read-only recon, 2026-06-25.** Compares the modder's "converted-to-new-brain"
`carver_v_enhanced` campaign against (a) our engine's actual brain DSL/dispatch
and (b) our reference carver content. No engine runs, no code, no builds.

Sources:
- **Modder content:** `C:\Users\Joe\Downloads\MC2R_campaign_carver_v_enhanced\data\campaigns\carver_v_enhanced`
- **Our reference carver:** `C:\Users\Joe\Downloads\GameAsset\carver_v_enhanced`
- **Engine authority:** `code/brain_special_dispatch.cpp` (`kRecognizedVerbs[]`, effect dispatch),
  `scripts/check-brain-fit-schema.py` (Brain{} schema), `docs/render-backend-seams/brain-*.md`.

---

## ⚠️ HEADLINE FINDING — they are byte-identical

```
diff -rq <GameAsset/carver_v_enhanced> <MC2R_campaign.../carver_v_enhanced>  → NO OUTPUT
```

Both trees = **1007 files**, same 46 missions, same `object_overrides` (28 .fit), same
`campaign.fit` (7053 B), `logistics.fit` (50 B), `roster.fit` (62 B). Ext census identical:
746 .abl, 169 .fit, 46 .pak, 46 .md.

**The "modder's converted content" IS our reference carver, copied verbatim.** It is the
same conversion *we* produced/adopted. There is no independent modder conversion to diff
against — the delta is zero. Everything below therefore describes **the carver conversion
format itself** measured against OUR current engine dispatch (which is the real question:
does this content load against our loader, and what's the gap).

---

## 1. FILE INVENTORY + DELTA

| | Modder | Reference (GameAsset) |
|---|---|---|
| Total files | 1007 | 1007 |
| Missions | 46 | 46 (same names) |
| object_overrides | 28 .fit | 28 (identical) |
| Content diff | — | **none (`diff -rq` clean)** |

Per-mission layout (e.g. `missions/mc2_01/`):
- `mission.fit` — bracket-section FitIni; carries **inline `Brain{}` + `Tactics{}` blocks** per `[WarriorN]`.
- `mission_specials.fit` — brace-block **`TechSpecial{}`** bodies (the per-unit brain ticks + scenario script).
- `gamesys.fit`, `mc2_01.pak`, `warriors/README.md`.
- `legacy_source/mission.abl` + `legacy_source/warriors/*.abl` — **original ABL preserved alongside**, audit-only (per README).

So: original `.abl` are NOT removed/renamed — they are retained under `legacy_source/`. Converted
brain lives in two places: inline `Brain{}` in `mission.fit` + `TechSpecial{}` in `mission_specials.fit`.
**No `mission_ai.fit` file exists anywhere in the tree (0 found).**

---

## 2. BRAIN FORMAT THEY USED

**Brace-block dialect** (our RawScan format), NOT the legacy `[BrainSpecial]/[Body]/st DO0=` bracket form.

**(a) Per-unit brain tick — `mission_specials.fit`:**
```
TechSpecial {
    key  = "mission.mc2_01.unit.mc2_01_pat1.tick"
    alias = "UnitBrain.mc2_01_Pat1.Tick"
    type = "UnitBrainSpecial"
    sourceABLBrain = "mc2_01_Pat1"
    Body {
        IF objectClass(-1) == 2
        DO Brain.CoreEject
        ELSE
        DO Brain.CorePower false
        ENDIF
        DO OPORD.CorePatrol startBase1PatrolState startBase1PatrolPath AttackStateHandle
        DO Unit.ClearMoveOrders 1
        DO Brain.SetPilotState self "start"
        DO Brain.AttackTactic 0 TACORDER_PARAM_NONE TACTIC_RIGHT_FLANK tacticState
        DO OPORD.CoreGuard startPosition -1 AttackStateHandle
        STOP
    }
}
```

**(b) Scenario script — `mission_specials.fit`** (`type="MissionSpecial"`), thousands of
`DO Var.Set "..." ... scope=Mission` plus `Camera.*`, `PlayMusic`, `PlayWave`, `PlayVideo`.

**(c) Inline `Brain{}` per warrior — `mission.fit`:**
```
[Warrior1]
st Brain = ""
st LegacyBrain = "PBrain"
Brain {
    sourceABLBrain = "PBrain"
    compatibilityMode = "Enhanced"
    archetype = "Archetype.InnerSphere.Standard"
    RequestHelp = false
    PrimaryOPORD { type = Guard }
    Tactics {
        Standard   = 1.00
        Flank      = ...
        Suppress   = ...
    }
}
```

---

## 3. VERB-LEVEL DIFF vs OUR DSL

`kRecognizedVerbs[]` (engine, `brain_special_dispatch.cpp:168`) — the FULL set:
`Brain.CorePower`, `Brain.CoreAttack`, `OPORD.CoreAttack`, `OPORD.CoreGuard`, `OPORD.CorePatrol`,
`OPORD.CoreMoveTo`, `Unit.Retreat`, `Unit.Eject`, `HOLD`, `Unit.SetState`, `Unit.SetStatePrev`,
`Unit.InState`, `Unit.NotInState`, `Unit.SetStateIf`. Plus aliases (`aliasToCanonical`):
`coreEject→Unit.Eject`, `corePower→Brain.CorePower`, `coreGuard→OPORD.CoreGuard`,
`coreRetreat→Unit.Retreat`. Plus `Var.Set`/`Var.Get` (intercepted when `MC2_BRAIN_DISPATCH_VAR=1`)
and `TechSpecial.Call` (when call-chain gate on). **"Recognized" ≠ "has effect":** only a
subset actually fires a `TacticalOrder` (effect branches at lines 754–1135).

Verbs the modder content actually uses (census across all `mission_specials.fit`), mapped:

| Modder verb | count | Our equivalent | Recognized? | Has runtime EFFECT? | Notes |
|---|---|---|---|---|---|
| `Var.Set` | 5455 | `Var.Set` | Y (gated `DISPATCH_VAR`) | store-only | needs `MC2_BRAIN_DISPATCH_VAR=1` else UNKNOWN |
| `Debug.SetDebugstring` | 2445 | — | **N** | no | trace-only `[BRAIN_DISPATCH_UNKNOWN]` |
| `Unit.ClearMoveOrders` | 1577 | — | **N** | no | UNKNOWN |
| `Brain.CorePower` | 801 | `Brain.CorePower` | Y | **POWERDOWN** (on `false`) | works |
| `Brain.CoreEject` | 657 | `coreEject`→`Unit.Eject`? | **N** | no | alias is `coreEject`, NOT `Brain.CoreEject` → UNKNOWN |
| `OPORD.CorePatrol` | 656 | `OPORD.CorePatrol` | Y (gated patrol) | PATROL | works when patrol gate on |
| `Brain.CoreAttack` | 571 | `OPORD.CoreAttack` | Y (listed) but **no effect branch for `Brain.CoreAttack`** | **no** | listed in array, but effect dispatch only matches `OPORD.CoreAttack`; `Brain.CoreAttack` recognized→silently no-op |
| `OPORD.CoreGuard` | 345 | `OPORD.CoreGuard` | Y | GUARD | works |
| `Brain.SetPilotState` | 222 | — (FSM is `Unit.SetState`) | **N** | no | UNKNOWN; our FSM verb is `Unit.SetState`, not `Brain.SetPilotState` |
| `PlayMusic` | 217 | — | **N** | no | UNKNOWN |
| `PlayWave` | 184 | — | **N** | no | UNKNOWN |
| `Debug.Mcprint` | 177 | — | **N** | no | UNKNOWN |
| `UnitQuery.SetTargetpriority` | 125 | — | **N** | no | UNKNOWN |
| `Brain.AttackTactic` | 90 | — (tactic weights are data, not a verb) | **N** | no | UNKNOWN; we express tactics as `Tactics{}` weights, not a verb |
| `Unit.SetMoveArea` | 73 | — | **N** | no | UNKNOWN |
| `TechSpecial.Call` | 62 | `TechSpecial.Call` | Y (gated) | trace/chain only | recurses, trace-only in current slice |
| `PlaySound` | 62 | — | **N** | no | UNKNOWN |
| `PlayVideo` | 44 | — | **N** | no | UNKNOWN |
| `Flow.Return` | 41 | — | **N** | no | UNKNOWN |
| `Camera.*` (SetGoalPosition/Rotation/SetPosition/FadeToColor/EndMovieMode/…) | ~150 | — | **N** | no | UNKNOWN (cinematic verbs) |
| `Brain.CoreEscort` | 24 | — | **N** | no | UNKNOWN (Escort OPORD type exists in schema, but not as a dispatch verb) |
| `OPORD.CoreMoveTo` | 20 | `OPORD.CoreMoveTo` | Y | MOVETO_POINT | works (needs 3 numeric args; carver passes symbolic names → parse-fail) |
| `TriggerArea.Add`, `Object.*`, `Audio.*`, `Unit.TacAttack`, `Unit.TacMoveToObject`, `OPORD.CoreMoveToObject`, `Brain.GetPilotState`, `Brain.CoreWait`, `Unit.OrderPatrol`, `UnitQuery.SetPilotstate` | <20 ea | — | **N** | no | UNKNOWN |

**Core takeaway:** of ~40 distinct verbs the conversion emits, only **5** dispatch to a real
effect today — `Brain.CorePower`, `OPORD.CoreGuard`, `OPORD.CorePatrol` (gated), `OPORD.CoreMoveTo`,
`Unit.Eject` (and only via `coreEject` alias, which the content does NOT use — it uses `Brain.CoreEject`).
Everything else is `[BRAIN_DISPATCH_UNKNOWN]` trace-only and produces no behavior.

---

## 4. FSM / STATE

The conversion did **NOT** use our flat-guard FSM (`Unit.SetState`/`InState`/`NotInState`/`SetStateIf`).
Instead the ABL state machine was **flattened into a linear verb stream** and the original FSM
structure left as **untranslated comments**:
```
; TODO: manual ABL line: state attack;
; TODO: manual ABL line: integer tacticState;
; TODO: manual ABL line: update;
; TODO: manual ABL line: transBack;
; TODO: manual ABL line: endstate;
```
Every state body's verbs are emitted back-to-back inside ONE `Body{}` with no guard wrapping —
so at runtime all states' orders would issue every tick (last-write-wins), not a real FSM.
State intent survives only as `Brain.SetPilotState self "start"` / `Brain.GetPilotState` calls —
**neither of which we recognize** (our verb is `Unit.SetState`). They did NOT invent verbs we lack
*for FSM* per se; they left FSM semantics in `; TODO` comments + the unsupported `Brain.*PilotState` pair.

`IF/ELSE/ENDIF` guards ARE present (raw ABL condition text, e.g. `IF objectClass(-1) == 2`),
but the conditions are raw ABL expressions our RawScan loader does not evaluate.

---

## 5. TACTIC WEIGHTS / SWITCHES / PILOT-STAT

They DID use `Tactics{}` weight tables and `archetype=` inside inline `Brain{}` (mission.fit),
and `PrimaryOPORD{ type = ... }`. Shape vs `check-brain-fit-schema.py`:

- **OPORD types used:** `Guard` (2147), `Patrol` (1607), `PlayerControlled` (277), `Escort` (8),
  `Sentry` (1), plus blank `type =` (2424, = OPORD slot with no type set).
  - In schema `OPORD_TYPES`: Guard ✓, Patrol ✓, Escort ✓, Sentry ✓ (composition).
  - **`PlayerControlled` is NOT in `OPORD_TYPES`** → schema rule **R3 FAIL**.
- **Tactic names used:** `Standard`, `Flank`, `HitAndRun`, `Suppress` (+ `AttackerHelpRadius`/
  `DefenderHelpRadius`/`EngageRadius` numeric knobs).
  - In schema `TACTIC_NAMES`: `HitAndRun` ✓, `Flank` ✓. **`Standard` and `Suppress` are NOT** →
    rule **W3 WARN** (unknown tactic, forward-compat) for each.
- **archetypes:** `Archetype.InnerSphere.Standard` (1971), `Archetype.Clan.Standard` (228),
  `Archetype.PlayerControlled` (217), `Archetype.Mercenary.Standard` (8). `archetype=` present →
  rule **W4 WARN** (resolver deferred to BRAIN-ARCHETYPE-FIT-1; not yet implemented).
- **60+ GDD switches / pilot-stat config:** **NOT used.** No `combat.*`/`doctrine.*`/`sensor.*`
  switch keys, no pilot-stat aliases. Only `RequestHelp`, `compatibilityMode="Enhanced"`,
  `sourceABLBrain`, `LegacyBrain` appear.
- **CRITICAL TOOLING GAP:** the schema checker scans `*_ai.fit`. These `Brain{}` blocks live in
  `mission.fit` (and `TechSpecial{}` in `mission_specials.fit`) — files the checker does NOT match.
  So `check-brain-fit-schema.py` would report PASS (no `_ai.fit` = legacy fallback) and never see
  the R3/W3/W4 issues above. The Brain{} data is effectively unvalidated by our current tooling.

---

## 6. GAP LIST — their constructs OUR engine does NOT recognize/load

Trace-only (no effect) verbs they emit that we do not handle at all:
- `Debug.SetDebugstring`, `Debug.Mcprint` (debug only — harmless)
- `Unit.ClearMoveOrders`, `Unit.SetMoveArea`, `Unit.TacAttack`, `Unit.TacMoveToObject`, `Unit.OrderPatrol`
- `Brain.CoreEject` (we only alias `coreEject`), `Brain.AttackTactic`, `Brain.SetPilotState`,
  `Brain.GetPilotState`, `Brain.CoreEscort`, `Brain.CoreWait`
- `OPORD.CoreMoveToObject`
- `UnitQuery.SetTargetpriority`, `UnitQuery.SetPilotstate`
- `PlayMusic`, `PlayWave`, `PlaySound`, `PlayVideo`, `Audio.Stopmusic`, `Audio.Stopvoiceover`
- `Camera.*` (SetGoalPosition, SetGoalRotation, SetPosition, SetRotation, GetPosition, GetRotation,
  FadeToColor, SetMovieMode, EndMovieMode)
- `TriggerArea.Add`, `Object.ConvertCoords`, `Object.Status`, `Object.Remove`,
  `Object.SetInvulnerable`, `Object.SetObjectdamage`, `Flow.Return`

Recognized-but-no-effect (silent no-op): `Brain.CoreAttack` (the most-used combat verb, 571×).

Format / data gaps:
- `Brain.CoreAttack` listed in `kRecognizedVerbs[]` but no effect branch (only `OPORD.CoreAttack` fires).
- OPORD type `PlayerControlled` not in schema enum.
- Tactic names `Standard`, `Suppress` not in schema enum.
- `archetype=` resolver unimplemented (W4).
- `OPORD.CorePatrol`/`CoreGuard`/`CoreMoveTo` args are **symbolic** (`startBase1PatrolPath`,
  `AttackStateHandle`, `startPosition`) — our parsers expect bare numerics; `OPORD.CoreMoveTo`
  in particular would hit `[BRAIN_DISPATCH_MOVETO_PARSE_FAIL]`.
- Raw ABL `IF` condition expressions (`checkObjectiveStatus(0) == 1`, `objectClass(-1) == 2`) are
  not evaluated by the RawScan loader.
- Brain{} blocks live in `mission.fit`, outside `check-brain-fit-schema.py`'s `*_ai.fit` glob.

---

## 7. WHERE WE ARE AHEAD (DSL features they did NOT use)

- **Flat-guard FSM** (`Unit.SetState/InState/NotInState/SetStateIf`) — they left FSM as `; TODO`
  comments + unsupported `Brain.SetPilotState`. Our FSM verbs are unused.
- **`Tactics{}` weight schema** with the registered tactic enum + OPORD slot model — they emit a
  `Brain.AttackTactic` *verb* per tick instead of leaning on the weight table.
- **60+ GDD switches + pilot-stat aliases** — entirely unused.
- **`OPORD.CoreMoveTo` numeric-arg + NaN-guarded path** — they pass symbolic names, never numerics.
- **`TechSpecial.Call` chaining with cycle/depth guards** — used only 62× for scenario init/update
  fan-out, not for brain composition.

---

## 8. "OURS TAKES PRIORITY" — RECONCILIATION TABLE

Since the modder changed NOTHING engine-side (and the content is literally our own reference copy),
every divergence must resolve against OUR loader as-is or via our-side aliases.

| Divergence | Class | Resolution |
|---|---|---|
| `Brain.CoreEject` (657×) vs our alias `coreEject` | **B (cheap alias)** | add `Brain.CoreEject→Unit.Eject` to `aliasToCanonical` — one line; high value (657 uses) |
| `Brain.CoreAttack` listed but no effect (571×) | **A or B** | either route `Brain.CoreAttack→OPORD.CoreAttack` in alias map, OR conform content to `OPORD.CoreAttack`. Cheap alias wins (most-used combat verb) |
| `Var.Set` needs `MC2_BRAIN_DISPATCH_VAR=1` | **A (config)** | enable the gate; already supported, just off by default |
| `OPORD.CorePatrol` needs patrol gate | **A (config)** | enable `MC2_BRAIN_…` patrol gate |
| FSM flattened + `Brain.SetPilotState`/`GetPilotState` | **A (conform)** | content must re-emit as `Unit.SetState`/guards; we will not add a parallel PilotState FSM |
| `Brain.AttackTactic` verb | **A (conform)** | express as `Tactics{}` weights (data), not a per-tick verb; we won't add the verb |
| Symbolic OPORD args (`startPosition`, `…PatrolPath`) | **A (conform)** | content must pass numerics our parsers accept, or we add a symbol-resolution layer (larger; not cheap) |
| Raw ABL `IF` conditions | **C (out of scope)** | RawScan does not evaluate ABL expressions; either conform to our guard verbs or accept trace-only |
| `Camera.*`, `Play*`, `Audio.*`, `TriggerArea.*`, `Object.*`, `Flow.Return`, `Debug.*` | **C (out of scope as brain verbs)** | these are scenario/cinematic ops, not brain orders; out of scope for the brain dispatcher (handled — or not — by mission scripting elsewhere). Harmless as trace-only |
| OPORD `type=PlayerControlled` | **B (schema add)** | add to `OPORD_TYPES` (player units are real); cheap |
| Tactics `Standard`,`Suppress` | **B (schema add)** | add to `TACTIC_NAMES`; cheap (today only W3 warn) |
| `archetype=` unresolved | **C/deferred** | resolver is BRAIN-ARCHETYPE-FIT-1 future work |
| Brain{} in `mission.fit` outside checker glob | **B (tooling)** | extend `check-brain-fit-schema.py` to also scan `mission.fit`/`mission_specials.fit`, or accept it's unvalidated |

---

## SUMMARY (caveman-terse)

- **File delta = ZERO.** Modder's `carver_v_enhanced` is `diff -rq`-identical to our reference
  GameAsset copy. 1007 files, 46 missions, same object_overrides. There is no separate modder
  conversion — it's our own carver conversion, verbatim. Original `.abl` preserved under
  `legacy_source/`.
- **Format = brace-block.** `TechSpecial{ Body{ DO … } }` (our RawScan) in `mission_specials.fit`
  + inline `Brain{}`/`Tactics{}` per `[WarriorN]` in `mission.fit`. **No `mission_ai.fit` anywhere.**
- **Verb gap is large.** ~40 distinct verbs; only **5 actually dispatch an effect** today
  (`Brain.CorePower`, `OPORD.CoreGuard`, `OPORD.CorePatrol`-gated, `OPORD.CoreMoveTo`, `Unit.Eject`).
  Biggest live misses: `Brain.CoreEject` (657, wrong alias name), `Brain.CoreAttack` (571,
  recognized-but-no-effect), `Brain.AttackTactic`/`Brain.SetPilotState`/`UnitQuery.*`/`Unit.ClearMoveOrders`
  (all UNKNOWN). Cinematic/audio/object verbs (`Camera.*`,`Play*`,`Object.*`) are out-of-scope-as-brain.
- **FSM not converted.** States flattened into one linear body; FSM left as `; TODO: manual ABL line:`
  comments + unsupported `Brain.SetPilotState`. Our `Unit.SetState` FSM unused.
- **Tactics/switches partial.** Used `Tactics{}` weights + `archetype=` + `PrimaryOPORD{type}`; did
  NOT use the 60+ GDD switches or pilot-stat. Schema would flag R3 (`type=PlayerControlled`),
  W3 (`Standard`/`Suppress`), W4 (`archetype=`) — but the checker only scans `*_ai.fit`, so the
  Brain{} blocks are currently UNVALIDATED by our tooling.
- **Recommended reconciliation:** cheap our-side aliases for `Brain.CoreEject→Unit.Eject` and
  `Brain.CoreAttack→OPORD.CoreAttack` (covers 1228 uses), schema adds for `PlayerControlled`/
  `Standard`/`Suppress`, extend the checker to scan `mission.fit`/`mission_specials.fit`. CONFORM
  (content-side) for FSM (`Unit.SetState`), tactic-as-data, and numeric OPORD args. OUT OF SCOPE:
  raw ABL `IF` evaluation and cinematic/audio/object verbs as brain orders.
