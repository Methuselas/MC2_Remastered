# TORRIN ENHANCED-RUN — recon + first runs (2026-06-25)

Goal: wire + run the DarkRain mission **torrin** as a key multi-warrior AI example.
Recon (72 tool-uses) + my own verification runs. Several recon claims were wrong on
the crux (verified per memory `recon-agent-verify-crux-claims`).

## What torrin IS
- `mods/DarkRain/data/missions/torrin.fit` — **52 warriors** (50 AI, 2 player `PBrain`).
  Already in v0.5.0 (+ v0.4 decal-test). Registered in `tests/smoke/smoke_missions.txt`:
  `tier3 torrin allow_asset_oob=1 mod=DarkRain deps=mc2x-compat`. `run_smoke.py --mission
  torrin` works (mod-mission env guard auto-sets MC2_ACTIVE_MOD/MC2_MOD_DEPS).
- **100% legacy ABL FSM brains** (7): `torringuard`(10), `torrinmove33/34/35`(24),
  `torrinescort`(13), `torrinpat`(2), `torrinluciferwakeup`(1). NO Enhanced TechScript,
  NO `_ai.fit`, NO `_specials.fit`.

## VERIFIED RUNS (v0.5.0 exe `9a9faa5f` = GAP-A-ROBUSTNESS build)
- **torrin RUNS: result=PASS, +0 destroys, ~13s load.** It is a working multi-warrior AI
  mission on its native ABL brains.
- **24 ABL compile-failure lines.** Two brains fail: `torrin_script.abl` (lines 65/66:
  `noattackcode`, `wxmcode` undefined) and `torrinescort.abl` (line 324: `corerepair`
  undefined). The other 5 brains compile → ~37/52 warriors have working ABL behavior;
  the 13 escort warriors run brainless.

## CORRECTED RECON CLAIMS (verified wrong)
1. **"FORCE_MODE=enhanced gives ~37/52 Enhanced dispatch."** FALSE. Ran the full brain
   bundle + FORCE_MODE=enhanced + patrol/commit-phase → **0 Enhanced-dispatch wids**.
   Reason: Enhanced dispatch needs a specials BODY (verbs); torrin has none (no
   `_specials.fit`). FORCE_MODE just flips the mode flag — and on an ABL-brain warrior it
   SKIPS `brain->execute()` (the ABL FSM) and runs an empty Enhanced runtime → the warrior
   does NOTHING. So forcing enhanced on pure-ABL torrin is counterproductive without
   authored Enhanced content.
2. **"Add cveg as a dep → mc2xcore.abx resolves the missing symbols."** FALSE. Ran with
   `MC2_MOD_DEPS=mc2x-compat,cveg` → still 21 ABL errors, same `noattackcode`/`wxmcode`/
   `corerepair` undefined. The symbols live in cveg's compiled `mc2xcore.abx` + cveg mission
   scripts, NOT in the `.abi` headers torrin_script `#include_`s (`misconst.abi`/
   `sndconst.abi` — neither defines them). Mounting cveg on the search path does NOT make
   the ABL compiler link a dep's `.abx`.

## REAL STATE / what torrin actually needs
torrin was authored against the **MC2X core runtime** (`mc2xcore.abx` — the `coreRepair`/
`coreCapture`/`noAttackCode`/`wxmcode` symbols). To get all 52 warriors' ABL brains
compiling, the engine must LINK `mc2xcore.abx` as a core module for torrin (or torrin's
`.abl` must `#include` it). This is an MC2X-core-integration step, NOT an env/dep tweak.
Open question for a follow-up: how does the ABL loader mount a core `.abx` module — is
there an `MC2_ABL_CORE`/core-include mechanism, or does cveg's OWN missions resolve these
because they `#include` mc2xcore-providing headers torrin lacks? (cveg mission scripts
DEFINE the symbols locally; torrin USES them expecting a linked core.)

## Two distinct goals (don't conflate)
- **(A) torrin on native ABL, fully working:** fix the `mc2xcore.abx` linkage so escort +
  mission-script compile. Then torrin is a clean 52-warrior native-ABL AI example. This is
  the realistic near-term "key example."
- **(B) torrin under OUR Enhanced brain:** requires authoring `torrin_ai.fit` (Brain records
  for wid 3–52) + a `torrin_specials.fit` (Enhanced patrol/guard verbs per warrior). Only
  then does FORCE_MODE=enhanced + GAP-A do anything. Bigger content-authoring effort.
  GAP-A-ROBUSTNESS-1 (shipped nifty `6be6e1f7`) is a prerequisite IF MC2_TACTIC_WEIGHTS is
  also on.

## Cross-refs
- recon-verify lesson: memory `recon-agent-verify-crux-claims` (FORCE_MODE + cveg claims wrong).
- GAP-A-ROBUSTNESS-1: `torrin-multiwarrior-patrol-soak-1-findings.md` + nifty `6be6e1f7`.
