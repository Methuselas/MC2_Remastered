# RenderWorld Slice M2.6 -- Mech Pickup Plan (inspect-only v1)

> For agentic workers: REQUIRED SUB-SKILL: `superpowers:subagent-driven-development`.

**Goal:** Close the M1->M1.5->M1.6->M2->M2.5->M2.6 RenderWorld arc by making Shift+LMB on a visible hostile mech emit a `[GAMEPLAY_PICK v1] hit kind=Mech ...` inspect log (no selection, no attack routing), while retiring the per-kind log/state schema as the slice META-FIX.

**Architecture:** One substrate addition (`LookupResult.kind`) drives every consumer to be kind-aware at compile time, fixing M2.5's latent mislabel bug in M1.6's `tryStaticPropPick` wrapper. A new `tryMechPick` caller in `code/missiongui.cpp` mirrors `tryStaticPropPick`, dispatches through the unchanged `tryGameplayPick` spine, reverse-resolves the handle via a new linear-scan `GameAdapters::Mech::findMechByHandle`, applies the full fog-of-war predicate, and emits the unified log line. The M1.6 `StaticPropSelectionDebugState` / `[STATIC_PROP_PICK v1]` schema is renamed to `GameplaySelectionDebugState` / `[GAMEPLAY_PICK v1] kind=...` in the same slice (substitutive, not additive).

**Tech Stack:** C++14, OpenGL 4.5, Windows/MSVC, CMake 3.x, PowerShell smoke runner.

---

## External review fixes applied (EXECUTE-WITH-FIXES -> revised)

External greybeard review (post-adversarial-revision) returned EXECUTE WITH FIXES. All 6 findings applied:

| Finding | Class | Change |
|---|---|---|
| C1 | CRITICAL | Task 4 Step 5 documents that `code/missiongui.cpp:65` already directly `#include"mech.h"` (grep-verified at plan-write time), so the full `BattleMech` type is in scope for `tryMechPick`. No additional include needed; source-of-truth note added so a future header-cleanup pass does not remove it. Fallback path documented if grep returns zero hits at write time. |
| M1 | MAJOR | Task 4 Step 9 pass criterion + Step 11 commit subject + body reworded to match MAJOR-A: banner remains the M1.6 `[STATIC_PROP_PICK v1] enabled=...` form through Task 4; rename lands atomically in Task 5 Step 2a. Commit subject is `m2.6: add tryMechPick consumer + 3 env gates` (no "extended banner"). |
| M2 | MAJOR | Task 6 Step 4 adds link-direction sanity gate + fallback: if linker reports unresolved `RunMechPickSelfTest` (RenderWorld -> GameAdapters static-lib link order disagrees), move the CALL from `RenderWorld::init()` to `GameAdapters::Mech::beginMission()` (same TU as definition; no forward-decl needed). Explicit ban on "fixing" by including `mech.h` in `RenderWorld.cpp`. |
| m1 | MINOR | Task 3 Step 3 `findMechByHandle` body gains a cast-safety comment tying the `static_cast<BattleMech*>(m)` + `static_cast<Mech3DAppearance*>(bm->appearance)` chain to the M2 invariant (only `BattleMech::init` assigns `appearance = new Mech3DAppearance` at `code/mech.cpp:1304`). |
| m2 | MINOR | Task 6 self-test now snapshots `RenderWorld::getMechsAliveCount()` pre-register and asserts no drift post-destroy (Step 5 in the self-test body). New public accessor `RenderWorld::getMechsAliveCount()` added as Task 6 Step 2b prerequisite -- the file-scope `s_mechs_alive_rw` atomic at `RenderWorld.cpp:105` cannot be read from `MechRenderAdapter.cpp` (different TU) without an accessor. |
| m3 | MINOR (acknowledge) | External reviewer affirmed Task 5 META-FIX Gate 6 (zero-match grep for retired symbols) is correct as written. No change. |

---

## Adversarial findings applied (CONDITIONAL-PASS -> revised)

Per `docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-6-plan-adversarial.md` (0 CRIT + 4 MAJOR + 5 MINOR). All findings mechanical; no architectural revision needed.

| Finding | Class | Change |
|---|---|---|
| MAJOR-A | Inter-commit asymmetry | Banner rewrite moved from Task 4 Step 3 -> Task 5 Step 2a. Task 4 keeps the M1.6 banner literal verbatim. |
| MAJOR-B | Drift census incomplete | Two additional doc-comment hits (`RenderWorld.cpp:97-98`, `RenderWorld.h:96-99`) folded into Task 5 grep + the citation-drift table. |
| MAJOR-C | Self-test slot consumption | Doc-only note added to Task 6 acknowledging the slot-counter consumption mirrors M2.5 precedent (benign). |
| MAJOR-D | Doc-comment-vs-emit asymmetry | `IsStaticPropPickDebugEnabled` doc-comment rewrite deferred from Task 4 Step 1 to Task 5 Step 1; Task 4 keeps the M1.6 doc comment verbatim. |
| MINOR-1 | Firewall scope clarification | One-line confirmation added to Task 3 Step 5. |
| MINOR-2 | Lifecycle-race comment | Comment added to Task 3 Step 3 `findMechByHandle` impl. |
| MINOR-3 | Option-A/B language tightened | Task 6 placement text pinned to option B; option A documented as reference only. |
| MINOR-4 | Env-flag idiom pre-pinned | Task 4 Step 2 explicitly pins the lambda-init static idiom (still verify at write time, but no executor-time derivation). |
| MINOR-5 | Gate 6 `.claude/` exclusion too broad | Exclusion narrowed from `\\\.claude\\` to `docs\\superpowers\\` + `CLAUDE\.md` only (Task 5 Step 7 + Task 7 Step 6). |

---

## Plan-stage blocker resolutions

Confirmed:

1. **CRITICAL-1 fix landed.** `code/mech.cpp:1338` stays `GameAdapters::Mech::syncSpawn(*m3d, 0u);` byte-identical to M2 ship. No `partId` cookie populate (`partId` reassigned at `code/mission.cpp:2987` after `syncSpawn`). Reverse lookup is Option B alone (linear scan on handle). Confirmed in spec Sections 4.2 / 6.1 / 6.2 / 8 / 11 / 13 / 14 / 15. No task in this plan modifies `code/mech.cpp:1338` or `MechRenderAdapter::syncSpawn` body.

2. **Fog predicate FULL form per spec Section 6.3.** The mech-side predicate mirrors `code/missiongui.cpp:1272-1278` verbatim: fog suppresses ONLY when `!ShowMovers && !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]) && hostile && !disabled && conStat < CONTACT_SENSOR_QUALITY_1`. `MC2_MECH_PICK_PIERCE_FOG=1` short-circuits the whole predicate. Encoded verbatim in Task 4.

3. **Gate 6 grep set includes doc comments.** Gate 6 in Task 7 includes:
   - source greps on `RenderWorld/ code/ GameAdapters/`
   - header doc-comment greps `--include='*.h' --include='*.cpp' .`
   - in-repo tooling greps on `tests/ scripts/ .claude/`
   Per adversarial MAJOR-2(a) + Q1 RESOLVED full-retirement decision.

## Citation drift fixes

Spec Section 5 enumerated five doc-comment hits to rename in the META-FIX commit (`RenderWorld.h:97`, `RenderWorld.cpp:93/484/599/605`). Write-time re-grep shows three additional hits the spec missed; they MUST be renamed in the same commit or Gate 6 fails:

| Drift hit | File:line | Content |
|---|---|---|
| Forgotten doc comment | `code/gameplay_pick.h:45` | `// ... M1.6 [STATIC_PROP_PICK v1] hit/miss log printf args exactly.` |
| Forgotten doc comment | `code/missiongui.h:269` | `// ... Emits [STATIC_PROP_PICK v1] hit/miss and updates RenderWorld debug state.` |
| Forgotten include-comment | `code/missiongui.cpp:31-32` | `// M1.6: IsStaticPropPickEnabled, lookupAtPixel, setLastStaticPropPick, // clearLastStaticPropPick, getLastStaticPropPick, IsObjectIdBufferEnabled.` |
| Forgotten doc comment | `code/missiongui.cpp:6175` | `// ... (debug-state mutation + [STATIC_PROP_PICK v1] hit/miss logs).` |
| Forgotten storage var | `RenderWorld/RenderWorld.cpp:99` | `RenderWorld::StaticPropSelectionDebugState s_lastStaticPropPick;` -- the FILE-SCOPE static needs renaming alongside the public symbols. |
| Forgotten mutex partner | `RenderWorld/RenderWorld.cpp` near :99 | `s_lastStaticPropPickMutex` -- rename for consistency. |
| Forgotten lifecycle | `RenderWorld/RenderWorld.cpp:506` | `clearLastStaticPropPick();` in `RenderWorld::destroy()` -- spec Section 4.4 notes this site but does not enumerate the line. |
| Forgotten doc comment (MAJOR-B) | `RenderWorld/RenderWorld.cpp:97-98` | `[STATIC_PROP_PICK v1] miss` literal inside `IsStaticPropPickDebugEnabled` doc comment in the .cpp. |
| Forgotten doc comment (MAJOR-B) | `RenderWorld/RenderWorld.h:96-99` | `[STATIC_PROP_PICK v1] miss` literal inside `IsStaticPropPickDebugEnabled` doc comment in the header (the comment Task 4 USED to rewrite; per MAJOR-D it stays put in Task 4 and renames here in Task 5). |

All renames land in the Task 5 META-FIX commit; Gate 6 enforces zero leftover matches.

Also: spec Section 8 surfaces table cites `code/missiongui.cpp:1539` and `:1782` for the `tryStaticPropPick` call sites; both grep-confirmed at write time.

## File structure

**Modified files:**
- `RenderWorld/RenderWorld.h` (add `LookupResult.kind`; rename debug-state struct + decls; add `IsMechPickEnabled`/`IsMechPickDebugEnabled`/`IsMechPickPierceFogEnabled` decls)
- `RenderWorld/RenderWorld.cpp` (copy `kind` in `lookupAtPixel`; rename setter/getter/clear impls + storage + mutex; rename boot banner; add the three mech-pick env-flag accessors; wire `RunMechPickSelfTest` into `init()`)
- `GameAdapters/MechRenderAdapter.h` (forward-decl `class BattleMech`; add `findMechByHandle` decl)
- `GameAdapters/MechRenderAdapter.cpp` (add `code/` includes; implement `findMechByHandle` linear scan)
- `code/missiongui.h` (add `tryMechPick` member decl; update M1.6 doc comment to reference unified schema)
- `code/missiongui.cpp` (kind-guard the M1.6 `tryStaticPropPick` hit branch + rename log + rename setter; add `tryMechPick` body + add second tail call after each `tryStaticPropPick` call site; rename include-comment at top)
- `code/gameplay_pick.h` (doc-comment rename: M1.6 `[STATIC_PROP_PICK v1]` -> `[GAMEPLAY_PICK v1]`)
- `.claude/worktrees/nifty-mendeleev/CLAUDE.md` (update M1.6 entry log-schema reference; add M2.6 SHIPPED entry; add three new env vars to the `Tier-1 instrumentation env vars` section)

**Created files:** none. `RunMechPickSelfTest` lives in `RenderWorld/RenderWorld.cpp` next to `RunMechObjectIdSelfTest` (per M2.5 convention; matches "per-domain co-location" preference).

## Critical inline rules

- **BUILD DIR PIN:** always worktree `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/`. The root checkout's `build64/` is stale (terrain-pbr-mod branch).
- **ALWAYS `--config RelWithDebInfo`** for build and rebuild. Release crashes with `GL_INVALID_ENUM`.
- **Smoke verbatim (copy-paste; no flag substitution):**
  ```
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  ```
- **Deploy:** use `Copy-Item -Force` + `fc /B` per-file (never `cp -r`; `cp -r` silently fails on Windows/MSYS2). M2.6 has NO shader edits, so no `shaders/` redeploy required.
- **Full relink before any deploy** if a load-bearing function changed signature. M2.6 changes the `setLastStaticPropPick` symbol set (rename). Add `--clean-first` to the affected build step or delete `build64/RelWithDebInfo/mc2.exe` plus changed `.obj` files before `cmake --build`.
- **No emoji anywhere.** ASCII only in source, comments, logs, commit messages.
- **Grep every cited symbol at write-time.** Symbol stable, line numbers drift.
- **Commit one task per task.** HEREDOC commit messages. Co-author line required.

---

## Task 1: LookupResult.kind substrate change

**Files:** `RenderWorld/RenderWorld.h`, `RenderWorld/RenderWorld.cpp`

This is the load-bearing 2-line addition. ISOLATED commit; no consumer impact yet (existing consumers do not read `out.kind`).

- [ ] **Step 1: Verify CMakeCache + worktree build dir.**

  ```powershell
  Test-Path 'A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\CMakeCache.txt'
  ```

  Must return `True`. If `False`, generate first:
  ```powershell
  & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev -B A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64
  ```

- [ ] **Step 2: Add `kind` field at RenderWorld.h:157-167.**

  **Existing (`RenderWorld/RenderWorld.h:157-167`):**
  ```cpp
  struct LookupResult {
      bool                            isValid          = false;
      RenderCore::RenderObjectHandle  handle           = RenderCore::RenderObjectHandle::invalid();
      uint32_t                        meshHandleBits   = 0;
      uint32_t                        materialHandleBits = 0;
      uint8_t                         lodLevel         = 0xFFu;
      uint16_t                        pipelineId       = 0;
      uint32_t                        drawPacketIndex  = 0xFFFFFFFFu;
      uint32_t                        pathReasonCode   = 0;
      uint32_t                        gameObjectId     = 0;
  };
  ```

  **Replace with:**
  ```cpp
  struct LookupResult {
      bool                            isValid          = false;
      RenderCore::RenderObjectHandle  handle           = RenderCore::RenderObjectHandle::invalid();
      uint32_t                        meshHandleBits   = 0;
      uint32_t                        materialHandleBits = 0;
      uint8_t                         lodLevel         = 0xFFu;
      uint16_t                        pipelineId       = 0;
      uint32_t                        drawPacketIndex  = 0xFFFFFFFFu;
      uint32_t                        pathReasonCode   = 0;
      uint32_t                        gameObjectId     = 0;
      // M2.6: kind discriminator copied from RenderObjectRecord.kind.
      // Caller MUST check this before consuming kind-specific fields
      // (recipeIndex for StaticProp; BattleMech reverse-lookup for Mech).
      // Defaults to StaticProp to preserve M1.6 caller behavior on an
      // isValid=false return (callers should gate on isValid first
      // anyway; the default is only relevant for compile-time
      // initializer compatibility).
      RenderObjectKind                kind             = RenderObjectKind::StaticProp;
  };
  ```

- [ ] **Step 3: Copy `kind` from record at RenderWorld.cpp:717-726.**

  **Existing (`RenderWorld/RenderWorld.cpp:717-726`):**
  ```cpp
      out.isValid            = true;
      out.handle             = h;
      out.meshHandleBits     = rec.meshHandleBits;
      out.materialHandleBits = rec.materialHandleBits;
      out.lodLevel           = rec.lodLevel;
      out.pipelineId         = rec.pipelineId;
      out.drawPacketIndex    = rec.drawPacketIndex;
      out.pathReasonCode     = rec.pathReasonCode;
      out.gameObjectId       = rec.gameObjectId;
      return out;
  ```

  **Replace with:**
  ```cpp
      out.isValid            = true;
      out.handle             = h;
      out.meshHandleBits     = rec.meshHandleBits;
      out.materialHandleBits = rec.materialHandleBits;
      out.lodLevel           = rec.lodLevel;
      out.pipelineId         = rec.pipelineId;
      out.drawPacketIndex    = rec.drawPacketIndex;
      out.pathReasonCode     = rec.pathReasonCode;
      out.gameObjectId       = rec.gameObjectId;
      out.kind               = rec.kind;  // M2.6: kind discriminator
      return out;
  ```

- [ ] **Step 4: Build.**

  ```powershell
  & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64 --config RelWithDebInfo --target mc2
  ```

  Pass criterion: clean compile; no warnings on the touched files.

- [ ] **Step 5: Deploy + smoke tier1 env-OFF (verbatim).**

  Deploy mc2.exe:
  ```powershell
  Copy-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  fc /B A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  ```

  Smoke:
  ```powershell
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  ```

  Pass criterion: exit 0; tier1 5/5 PASS. (No env flags set; consumers do not yet read `out.kind`; expected pixel-parity with M2.5 HEAD.)

- [ ] **Step 6: Commit.**

  ```powershell
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m @'
  m2.6: add LookupResult.kind discriminator (substrate, no consumer impact)

  RenderObjectRecord already carries `kind` (populated by registerMech and
  upsertStaticProp); LookupResult did not expose it. Add the field and copy
  it in lookupAtPixel. Default = StaticProp (matches the M1.6 implicit
  invariant on a fresh-init LookupResult{}); callers always check
  isValid first, so the default only matters for type compatibility.

  Load-bearing for M2.6: every consumer now has a compile-aware
  discriminator to dispatch on. Tasks 2..7 fold the consumers.

  No behavior change in this commit -- M1.6 wrapper still ignores the
  new field. Tier1 5/5 PASS env-OFF.

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  '@
  ```

---

## Task 2: M1.6 wrapper kind-guard (latent-bug fix)

**Files:** `code/missiongui.cpp`

The latent mislabel bug armed by M2.5 lands here. After this task, a Shift+click on a mech pixel no longer poisons `StaticPropSelectionDebugState` with a -1 recipe. This task uses the OLD setter/getter/state names; Task 5 renames them.

- [ ] **Step 1: Verify the body shape at write-time.**

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\code\missiongui.cpp -Pattern 'case GameplayPickResult::Outcome::hit' -SimpleMatch
  ```

  Must return a line near `6216`. If drift, locate the case body before editing.

- [ ] **Step 2: Add kind guard to the hit branch.**

  **Existing (`code/missiongui.cpp:6216-6243`):**
  ```cpp
      case GameplayPickResult::Outcome::hit: {
          // Update RenderWorld debug state. Single-slot; latest wins.
          RenderWorld::setLastStaticPropPick(r.lookup,
                                             r.ctx.mouseX, r.ctx.mouseY,
                                             r.ctx.glX,    r.ctx.glY);
          // Sample back the debug-state struct so the log can include the
          // recipeIndex (LookupResult itself does not carry it; the
          // recipe lookup is done inside setLastStaticPropPick).
          const RenderWorld::StaticPropSelectionDebugState picked =
              RenderWorld::getLastStaticPropPick();
          // Unconditional hit log (spec Section 7); coord-diag fields
          // BYTE-IDENTICAL to M1.6 to keep user-driven canary stable.
          std::fprintf(stderr,
              "[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "
              "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
              "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
              r.lookup.handle.bits,
              (unsigned)r.lookup.handle.index(),
              (unsigned)r.lookup.handle.generation(),
              (int)picked.recipeIndex,
              r.ctx.mouseX, r.ctx.mouseY,
              r.ctx.glX,    r.ctx.glY,
              r.ctx.fboX,   r.ctx.fboY,
              r.ctx.vMulX,  r.ctx.vMulY,
              r.ctx.vAddX,  r.ctx.vAddY,
              r.ctx.drawableWidth, r.ctx.drawableHeight);
          break;
      }
  ```

  **Replace with:**
  ```cpp
      case GameplayPickResult::Outcome::hit: {
          // M2.6 latent-bug fix: post-M2.5, the substrate may return a
          // Mech handle. The static-prop caller MUST guard before
          // consuming -- a Mech handle has no recipe index, and the M2.6
          // mech caller (tryMechPick) owns mech-kind hits.
          if (r.lookup.kind != RenderWorld::RenderObjectKind::StaticProp) {
              // Not our category. Silent skip; the mech caller (or future
              // terrain/VFX caller) handles its own kinds.
              break;
          }
          // Update RenderWorld debug state. Single-slot; latest wins.
          RenderWorld::setLastStaticPropPick(r.lookup,
                                             r.ctx.mouseX, r.ctx.mouseY,
                                             r.ctx.glX,    r.ctx.glY);
          // Sample back the debug-state struct so the log can include the
          // recipeIndex (LookupResult itself does not carry it; the
          // recipe lookup is done inside setLastStaticPropPick).
          const RenderWorld::StaticPropSelectionDebugState picked =
              RenderWorld::getLastStaticPropPick();
          // Unconditional hit log (spec Section 7); coord-diag fields
          // BYTE-IDENTICAL to M1.6 to keep user-driven canary stable.
          std::fprintf(stderr,
              "[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "
              "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
              "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
              r.lookup.handle.bits,
              (unsigned)r.lookup.handle.index(),
              (unsigned)r.lookup.handle.generation(),
              (int)picked.recipeIndex,
              r.ctx.mouseX, r.ctx.mouseY,
              r.ctx.glX,    r.ctx.glY,
              r.ctx.fboX,   r.ctx.fboY,
              r.ctx.vMulX,  r.ctx.vMulY,
              r.ctx.vAddX,  r.ctx.vAddY,
              r.ctx.drawableWidth, r.ctx.drawableHeight);
          break;
      }
  ```

  Schema literal `[STATIC_PROP_PICK v1]` stays unchanged in this task; Task 5 renames it.

- [ ] **Step 3: Build (incremental OK, no class-layout change).**

  ```powershell
  & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64 --config RelWithDebInfo --target mc2
  ```

- [ ] **Step 4: Deploy + smoke tier1 env-OFF.**

  ```powershell
  Copy-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  fc /B A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  ```

  Pass criterion: tier1 5/5 PASS. (No clicks happen in tier1; the guard adds a single conditional branch on a dormant code path with `MC2_STATIC_PROP_PICK=0` default.)

- [ ] **Step 5: Commit.**

  ```powershell
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add code/missiongui.cpp
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m @'
  m2.6: kind-guard M1.6 wrapper hit branch (latent post-M2.5 bug)

  Without this guard, a Shift+click on a mech pixel (substrate-active
  post-M2.5) returned outcome=hit with a Mech handle, and the wrapper
  stored it as a static-prop pick: setLastStaticPropPick stamped a
  -1 recipeIndex, and the log line emitted `recipe=-1` on a real visible
  hit. Misleading and noisy.

  Fix: skip the hit branch when r.lookup.kind != StaticProp. The mech
  caller (Task 4) owns mech-kind hits; future terrain/VFX callers will
  own their kinds.

  Schema literal [STATIC_PROP_PICK v1] preserved here; Task 5 renames
  it as part of the META-FIX commit. Tier1 5/5 PASS env-OFF.

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  '@
  ```

---

## Task 3: findMechByHandle resolver in MechRenderAdapter

**Files:** `GameAdapters/MechRenderAdapter.h`, `GameAdapters/MechRenderAdapter.cpp`

Per CRITICAL-1: **NO** change to `MechRenderAdapter::syncSpawn` body and NO change to `code/mech.cpp:1338`. The resolver is the only new surface in the adapter.

- [ ] **Step 1: Verify worktree build dir + adapter shape.**

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\GameAdapters\MechRenderAdapter.h -Pattern 'void destroyMech' -SimpleMatch
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\GameAdapters\MechRenderAdapter.cpp -Pattern '#include "../mclib/mech3d.h"' -SimpleMatch
  ```

  Both must return matches.

- [ ] **Step 2: Add forward-decl + decl at GameAdapters/MechRenderAdapter.h.**

  **Existing (`GameAdapters/MechRenderAdapter.h:23` and `:43-52`):**
  ```cpp
  // Forward-decl game-side mech appearance. Spec section 12 carve-out:
  // adapter headers may forward-declare game-side types; the .cpp includes
  // the real header.
  class Mech3DAppearance;
  ```
  ```cpp
  // Destroy hook. Call BEFORE delete appearance (code/mech.cpp:3724-3728).
  // Retires the handle in RenderWorld and calls mech.clearRenderWorldHandleForAdapter().
  // No-op if mech.getRenderWorldHandle() is already invalid().
  //
  // THIS is the AUTHORITATIVE handle retirement path. endMission() is a
  // safety sweep only and must not be relied upon for per-mech cleanup.
  void destroyMech(Mech3DAppearance& mech);

  } // namespace Mech
  ```

  **Replace with (fwd-decl block):**
  ```cpp
  // Forward-decl game-side mech appearance. Spec section 12 carve-out:
  // adapter headers may forward-declare game-side types; the .cpp includes
  // the real header.
  class Mech3DAppearance;

  // M2.6: forward-decl game-side BattleMech (returned by findMechByHandle).
  // The .cpp includes code/mech.h to provide the full type; the header
  // stays firewall-clean per M2 adapter convention.
  class BattleMech;
  ```

  **Replace with (after destroyMech, before closing namespace):**
  ```cpp
  void destroyMech(Mech3DAppearance& mech);

  // M2.6: handle->BattleMech reverse lookup. Linear scan over
  // ObjectManager mover list; matches on
  //   mech.getAppearance()->getRenderWorldHandle().raw() == h.raw().
  // Returns nullptr on stale/unknown handle (M2.6 inspect-only path
  // treats this as outcome=miss for the click).
  //
  // O(N) where N = num movers per mission (<= ~50; tier1 max mc2_24
  // has 46 mechs). Cost negligible vs the lookupAtPixel readback that
  // produced the handle. NOT main-loop-safe to call per frame; intended
  // for one call per click (~10/sec max).
  //
  // MUST be called from the main thread (ObjectManager is not
  // thread-safe). The inspect path in tryMechPick satisfies this.
  BattleMech* findMechByHandle(RenderCore::RenderObjectHandle h);

  } // namespace Mech
  ```

- [ ] **Step 3: Add includes + implementation to GameAdapters/MechRenderAdapter.cpp.**

  Locate the include block at lines 10-21 of the file. Add the three `code/` includes after the `mech3d.h` include so the firewall comment still reads correctly.

  **Existing (`GameAdapters/MechRenderAdapter.cpp:10-21`):**
  ```cpp
  #include "MechRenderAdapter.h"

  // Engine side.
  #include "../RenderWorld/RenderWorld.h"

  // Game side. This is the ONLY TU outside mclib/ that may include mech3d.h.
  #include "../mclib/mech3d.h"

  #include <cassert>
  #include <cstdio>
  #include <cstdlib>
  #include <cstdint>
  ```

  **Replace with:**
  ```cpp
  #include "MechRenderAdapter.h"

  // Engine side.
  #include "../RenderWorld/RenderWorld.h"

  // Game side. This is the ONLY TU outside mclib/ that may include mech3d.h.
  #include "../mclib/mech3d.h"

  // M2.6: reverse-lookup needs ObjectManager + BattleMech + MoverPtr. These
  // includes extend the M2 bridging carve-out (this .cpp is the documented
  // exception). Firewall scope (scripts/check-include-firewall.sh SCOPE_DIRS)
  // does not police GameAdapters/, so no allowlist edit needed.
  #include "../code/objmgr.h"
  #include "../code/mover.h"
  #include "../code/mech.h"

  #include <cassert>
  #include <cstdio>
  #include <cstdlib>
  #include <cstdint>
  ```

  Then at end of `namespace Mech` (just before the `} // namespace Mech` closing brace -- locate it with grep), insert the implementation:

  ```cpp
  // M2.6: handle->BattleMech reverse lookup. Inspect-only path.
  BattleMech* findMechByHandle(RenderCore::RenderObjectHandle h) {
      if (!h.isValid()) return nullptr;
      if (ObjectManager == nullptr) return nullptr;
      const uint32_t target = h.raw();
      const long n = ObjectManager->getNumMovers();
      for (long i = 0; i < n; ++i) {
          MoverPtr m = ObjectManager->getMover(i);
          if (m == nullptr) continue;
          if (!m->isMech()) continue;
          // Cast safety (external-review m1): isMech() filter guarantees the
          // mover is a BattleMech. Verified in M2 spec: only BattleMech::init
          // assigns appearance = new Mech3DAppearance (at code/mech.cpp:1304),
          // and only BattleMech sets the isMech bit, so the static_cast pair
          // below is sound on any mover that passes isMech().
          BattleMech* bm = static_cast<BattleMech*>(m);
          // Lifecycle race (per adversarial MINOR-2): guards against
          // pre-init or mid-destroy mech with NULL appearance. A mech
          // exists in ObjectManager BEFORE syncSpawn populates its
          // appearance, and AFTER destroyMech runs but BEFORE the mover
          // slot is reclaimed. Both windows are single-threaded but real.
          Mech3DAppearance* app =
              static_cast<Mech3DAppearance*>(bm->getAppearance());
          if (app == nullptr) continue;
          if (app->getRenderWorldHandle().raw() == target) {
              return bm;
          }
      }
      return nullptr;
  }
  ```

  To locate the insertion point safely:
  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\GameAdapters\MechRenderAdapter.cpp -Pattern '} // namespace Mech' -SimpleMatch
  ```
  Insert the function body BEFORE that line.

- [ ] **Step 4: Build (full relink: function set on the adapter TU changed).**

  ```powershell
  Remove-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
  & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64 --config RelWithDebInfo --target mc2
  ```

  Pass criterion: clean compile; the `findMechByHandle` symbol exists in `MechRenderAdapter.obj` (unused so far; Task 4 wires the consumer).

- [ ] **Step 5: Run include-firewall check.**

  ```powershell
  bash A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-include-firewall.sh
  ```

  Pass criterion: exit 0. The new `code/` includes are in `GameAdapters/` which is NOT in SCOPE_DIRS; the script must not flag them.

  Confirmation (per adversarial MINOR-1): the script's forbidden-symbols sweep (`FORBIDDEN_SYMBOLS="... ObjectManager ..."` at `scripts/check-include-firewall.sh:32`) ALSO iterates SCOPE_DIRS only -- so the new `findMechByHandle` impl referencing `ObjectManager` is not flagged.

- [ ] **Step 6: Deploy + smoke tier1 env-OFF.**

  ```powershell
  Copy-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  fc /B A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  ```

  Pass criterion: tier1 5/5 PASS. (Symbol added but no caller; the binary's behavior is unchanged.)

- [ ] **Step 7: Commit.**

  ```powershell
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add GameAdapters/MechRenderAdapter.h GameAdapters/MechRenderAdapter.cpp
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m @'
  m2.6: add GameAdapters::Mech::findMechByHandle (linear-scan resolver)

  Reverse-resolves a RenderObjectHandle to its BattleMech* by linear
  scan over ObjectManager movers, matching on
  `app->getRenderWorldHandle().raw() == h.raw()`. Returns nullptr on
  stale or unknown handle (the M2.6 caller treats that as outcome=miss
  for the click).

  Per adversarial CRITICAL-1: NO change to syncSpawn or to
  code/mech.cpp:1338 -- partId is reassigned at code/mission.cpp:2987
  after syncSpawn fires, so a partId cookie would silently desync. The
  HANDLE itself is the only lifetime-stable identifier; linear scan is
  the resolver. Cost (~50 movers max) negligible vs the lookupAtPixel
  readback that produced the handle.

  Adapter .cpp now includes code/objmgr.h + code/mover.h + code/mech.h.
  GameAdapters/ is outside SCOPE_DIRS so no firewall allowlist edit.
  Header stays forward-decl-only (adds `class BattleMech;`).

  No consumer yet -- Task 4 wires tryMechPick. Tier1 5/5 PASS env-OFF.

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  '@
  ```

---

## Task 4: tryMechPick consumer + 3 env gates

**Files:** `RenderWorld/RenderWorld.h`, `RenderWorld/RenderWorld.cpp`, `code/missiongui.h`, `code/missiongui.cpp`

Header-include note (external-review C1): `code/missiongui.cpp` consumes `BattleMech` methods via `tryMechPick`. The full type is provided by the pre-existing `#include"mech.h"` at `code/missiongui.cpp:65` (grep-confirmed at plan-write time). If a future header-cleanup pass touches that include, audit `tryMechPick` callers + the existing fog-of-war gate at `:1272-1278`.

This is the largest task: adds the new caller body, three new cached env-flag accessors, the renamed boot banner, and the two new tail call sites. Uses the OLD `setLastStaticPropPick`/`StaticPropSelectionDebugState` names; Task 5 renames them. (Wait -- Task 5 renames AFTER this task, so this task's mech body MUST use the soon-to-be-renamed `setLastGameplayPick` form. RESOLUTION: Task 4 uses the FUTURE name `setLastGameplayPick` BUT does NOT call it yet -- the mech body emits the log line and skips the debug-state setter; debug-state population for mechs is added in Task 5 in the same commit that introduces the new setter. This keeps Task 4 buildable against the M1.6 surface and Task 5 the META-FIX atomic.)

Equivalent simpler shape: Task 4 wires the mech body to emit only the LOG LINE (no debug-state mutation for the Mech kind in this task). Task 5 retires the static-prop names AND adds the `setLastGameplayPick(Mech, ...)` call to the mech body in the same commit. This keeps Task 4's body building cleanly without forward references.

- [ ] **Step 1: Add 3 env-flag decls to RenderWorld/RenderWorld.h.**

  **Insertion point:** after `bool IsStaticPropPickDebugEnabled();` at `RenderWorld.h:103` and before the `objectIdRawForStaticPropRecipe` block at `:108`.

  **Existing (`RenderWorld/RenderWorld.h:96-108`):**
  ```cpp
  // M1.6: static-prop pick verbose-log enable. When this is OFF, the
  // `[STATIC_PROP_PICK v1] miss ...` line is suppressed (high-frequency
  // empty-Shift+click gesture would otherwise spam stderr). `hit` lines
  // fire unconditionally per spec Section 7.
  //
  // Process-lifetime cached. Consumed by:
  //   - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick miss branch
  bool IsStaticPropPickDebugEnabled();

  // M1.5 C1 fix: centralize Handle encoding. Returns 0 for invalid
  // recipeIndex (< 0). The producer in gos_static_prop_batcher.cpp
  // calls this with the result of GpuStaticPropRegistry::getRecipeIndexForType().
  uint32_t objectIdRawForStaticPropRecipe(int32_t recipeIndex);
  ```

  **Replace with (insert 3 decls between -- M1.6 doc comment kept VERBATIM per MAJOR-D; rename deferred to Task 5):**
  ```cpp
  // M1.6: static-prop pick verbose-log enable. When this is OFF, the
  // `[STATIC_PROP_PICK v1] miss ...` line is suppressed (high-frequency
  // empty-Shift+click gesture would otherwise spam stderr). `hit` lines
  // fire unconditionally per spec Section 7.
  //
  // Process-lifetime cached. Consumed by:
  //   - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick miss branch
  bool IsStaticPropPickDebugEnabled();

  // M2.6: master enable for the mech-pick wiring. When OFF, the
  // missiongui Shift+click mech wiring is dormant even if
  // MC2_OBJECT_ID_BUFFER=1 and MC2_STATIC_PROP_PICK=1. Default OFF.
  bool IsMechPickEnabled();

  // M2.6: mech-pick verbose-log enable. Gates the miss / gated /
  // stale-handle diagnostic logs from the mech caller. `hit` lines
  // always fire when MC2_MECH_PICK=1 and the fog gate passes.
  bool IsMechPickDebugEnabled();

  // M2.6: dev/debug override that allows inspect through sensor fog.
  // Default OFF preserves stock gameplay (cannot incidentally reveal
  // undetected enemy mechs via the inspect log).
  bool IsMechPickPierceFogEnabled();

  // M1.5 C1 fix: centralize Handle encoding. Returns 0 for invalid
  // recipeIndex (< 0). The producer in gos_static_prop_batcher.cpp
  // calls this with the result of GpuStaticPropRegistry::getRecipeIndexForType().
  uint32_t objectIdRawForStaticPropRecipe(int32_t recipeIndex);
  ```

  Note (per MAJOR-D): the `IsStaticPropPickDebugEnabled` doc comment is INTENTIONALLY left referencing `[STATIC_PROP_PICK v1]` in this task. End-of-Task-4 keeps doc comments and emit-site literals consistent (both still the M1.6 form). Task 5 atomically renames the doc comment AND every emit-site literal together.

- [ ] **Step 2: Add 3 env-flag impls to RenderWorld/RenderWorld.cpp.**

  Locate `bool IsStaticPropPickDebugEnabled()` impl with grep:

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern 'bool IsStaticPropPickDebugEnabled' -SimpleMatch
  ```

  Insert the three new impls IMMEDIATELY AFTER the closing `}` of `IsStaticPropPickDebugEnabled`:

  ```cpp
  // M2.6: cached env-flag accessors. Same lifetime/discipline as
  // IsStaticPropPickEnabled / IsStaticPropPickDebugEnabled. Process-
  // lifetime cached; restart required to flip.
  bool IsMechPickEnabled() {
      static const bool kEnabled = [] {
          const char* v = std::getenv("MC2_MECH_PICK");
          return v && v[0] && v[0] != '0';
      }();
      return kEnabled;
  }

  bool IsMechPickDebugEnabled() {
      static const bool kEnabled = [] {
          const char* v = std::getenv("MC2_MECH_PICK_DEBUG");
          return v && v[0] && v[0] != '0';
      }();
      return kEnabled;
  }

  bool IsMechPickPierceFogEnabled() {
      static const bool kEnabled = [] {
          const char* v = std::getenv("MC2_MECH_PICK_PIERCE_FOG");
          return v && v[0] && v[0] != '0';
      }();
      return kEnabled;
  }
  ```

  Per adversarial MINOR-4: idiom pre-pinned at plan-write time. The M1.6 `IsStaticPropPickEnabled` / `IsStaticPropPickDebugEnabled` accessors use the lambda-init `static const bool kEnabled = [] { ... }();` shape (verified at write time). The three new impls above mirror that shape verbatim; no executor-time re-derivation required.

  Quick write-time sanity grep (executor should still confirm the file has not drifted since plan-write):

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern 'bool IsStaticPropPickEnabled' -SimpleMatch
  ```

  If the M1.6 pattern has changed shape, fall back to mirroring the current form -- but at write time the lambda-init pattern is confirmed.

- [ ] **Step 3: Boot banner left unchanged in Task 4 (per MAJOR-A).**

  Per adversarial MAJOR-A: leaving the M1.6 banner literal `[STATIC_PROP_PICK v1] enabled=%d debug=%d` paired with the still-M1.6 hit/miss emit literals at `code/missiongui.cpp:6229/6252` keeps end-of-Task-4 internally consistent. The banner rewrite (with the extended five-field form including the three new mech-pick gates) is deferred to Task 5 Step 2a, which renames the banner ATOMICALLY with all emit-site literals + doc comments + storage var.

  No edit in this step. Verify the M1.6 banner is still in place:

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern '\[STATIC_PROP_PICK v1\] enabled=' -SimpleMatch
  ```

  Must return one match. (Task 4 adds the three new env-flag accessor SYMBOLS at Steps 1-2; the banner does not consume them yet -- it will after Task 5 Step 2a.)

- [ ] **Step 4: Add tryMechPick decl to code/missiongui.h.**

  Locate the `tryStaticPropPick` decl block at `code/missiongui.h:272-278` and add a sibling decl directly after.

  **Existing (`code/missiongui.h:272-278`):**
  ```cpp
  		void tryStaticPropPick(bool moverSelectedThisFrame,
  		                       bool shiftDn,
  		                       bool leftClicked,
  		                       bool bGui,
  		                       bool bLeftDouble,
  		                       int  mouseX,
  		                       int  mouseY);
  ```

  **Replace with:**
  ```cpp
  		void tryStaticPropPick(bool moverSelectedThisFrame,
  		                       bool shiftDn,
  		                       bool leftClicked,
  		                       bool bGui,
  		                       bool bLeftDouble,
  		                       int  mouseX,
  		                       int  mouseY);

  		// M2.6: mech-pick consumer. Mirrors tryStaticPropPick shape;
  		// dispatches through the SAME tryGameplayPick spine; kind-guards
  		// on r.lookup.kind == Mech; reverse-resolves to BattleMech via
  		// GameAdapters::Mech::findMechByHandle; applies fog-of-war
  		// predicate (mirrors code/missiongui.cpp:1272-1278); emits
  		// [GAMEPLAY_PICK v1] hit kind=Mech ... on a visible-hostile pick.
  		// Inspect-only v1 (no selection, no attack routing).
  		//
  		// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-6-mech-pickup-spec.md
  		void tryMechPick(bool moverSelectedThisFrame,
  		                 bool shiftDn,
  		                 bool leftClicked,
  		                 bool bGui,
  		                 bool bLeftDouble,
  		                 int  mouseX,
  		                 int  mouseY);
  ```

- [ ] **Step 5: Add the new include in code/missiongui.cpp.**

  Locate the existing include block near `code/missiongui.cpp:31-34`. Add the adapter include:

  **Existing:**
  ```cpp
  // M1.6: IsStaticPropPickEnabled, lookupAtPixel, setLastStaticPropPick,
  // clearLastStaticPropPick, getLastStaticPropPick, IsObjectIdBufferEnabled.
  #include "../RenderWorld/RenderWorld.h"
  #include "gameplay_pick.h"  // M2-pre: tryGameplayPick spine + GameplayPickRequest
  ```

  **Replace with:**
  ```cpp
  // M1.6 + M2.6: IsStaticPropPickEnabled, lookupAtPixel, setLastStaticPropPick,
  // clearLastStaticPropPick, getLastStaticPropPick, IsObjectIdBufferEnabled,
  // IsMechPickEnabled, IsMechPickDebugEnabled, IsMechPickPierceFogEnabled.
  #include "../RenderWorld/RenderWorld.h"
  #include "gameplay_pick.h"  // M2-pre: tryGameplayPick spine + GameplayPickRequest
  #include "../GameAdapters/MechRenderAdapter.h"  // M2.6: findMechByHandle (forward-decls BattleMech)
  ```

  **BattleMech complete-type include (external-review C1):** `tryMechPick` calls
  `bm->getTeamId()`, `bm->isDisabled()`, `bm->getAppearance()`, and casts
  `(Mover*)bm` -- all require the full `BattleMech` class definition.
  `MechRenderAdapter.h` only forward-declares `class BattleMech;` (per Task 3
  Step 2). Preflight grep (run at write time):

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\code\missiongui.cpp -Pattern '#include .*mech\.h'
  ```

  **Plan-write-time result (2026-05-23):** one hit at `code/missiongui.cpp:65`
  -- `#include"mech.h"` (no leading space; no `../` prefix; this TU is in
  `code/` so the bare path resolves directly). This direct include provides
  the full `BattleMech` type to the entire TU, including the `tryMechPick`
  body inserted at Step 6. **No additional `#include "mech.h"` required.**

  Source-of-truth note for the future header-cleanup pass: `code/missiongui.cpp:65`
  `#include"mech.h"` is what provides `BattleMech`, `Mover`, and the
  `target->isMover()` / `target->getTeamId()` / `target->isDisabled()` /
  `Mover::conStat` / `CONTACT_SENSOR_QUALITY_1` symbols consumed by both
  the pre-existing fog-of-war gate at `:1272-1278` and the new M2.6
  `tryMechPick` body. Do NOT remove the `mech.h` include without auditing
  these consumers.

  If write-time grep returns ZERO hits (file drift removed the include
  since plan write), add `#include "mech.h"` to the include block in this
  same step -- the M2.6 body cannot compile without the full type.

  Also ensure `MPlayer` + `ShowMovers` + `Team::home` + `CONTACT_SENSOR_QUALITY_1` are in scope. The fog predicate at `:1272-1278` already uses all four in this TU, so the includes that satisfy that predicate satisfy `tryMechPick` too. No additional includes required.

- [ ] **Step 6: Add tryMechPick body to code/missiongui.cpp.**

  Insert AFTER the closing `}` of `tryStaticPropPick` (at `code/missiongui.cpp:6271`). Body is verbatim from spec Section 6.3, expanded into copy-paste form:

  ```cpp

  void MissionInterfaceManager::tryMechPick(bool moverSelectedThisFrame,
                                            bool shiftDn,
                                            bool leftClicked,
                                            bool bGui,
                                            bool bLeftDouble,
                                            int  mouseX,
                                            int  mouseY)
  {
      // M2.6: category gate. Master enable for mech pick wiring.
      if (!RenderWorld::IsMechPickEnabled())
          return;

      // Build the same shape of request as tryStaticPropPick. The spine
      // (tryGameplayPick) is shared and unchanged.
      GameplayPickRequest req;
      req.mouseX                  = mouseX;
      req.mouseY                  = mouseY;
      req.shiftDn                 = shiftDn;
      req.leftClicked             = leftClicked;
      req.bGui                    = bGui;
      req.bLeftDouble             = bLeftDouble;
      req.moverSelectedThisFrame  = moverSelectedThisFrame;

      const GameplayPickResult r = tryGameplayPick(req);

      switch (r.outcome) {
      case GameplayPickResult::Outcome::hit: {
          // Kind guard: M2.6 caller only handles Mech kind. Static-prop
          // and future terrain/VFX kinds are owned by their callers.
          if (r.lookup.kind != RenderWorld::RenderObjectKind::Mech)
              break;

          // Reverse-resolve to BattleMech via the M2.6 linear-scan resolver.
          BattleMech* bm =
              GameAdapters::Mech::findMechByHandle(r.lookup.handle);
          if (bm == nullptr) {
              // Stale handle race (mech destroyed between readback and
              // resolver) -- impossible in practice (single-threaded), but
              // defensive design treats it as a benign miss for the click.
              if (RenderWorld::IsMechPickDebugEnabled()) {
                  std::fprintf(stderr,
                      "[GAMEPLAY_PICK v1] miss kind=Mech reason=stale_handle "
                      "handle=%u idx=%u gen=%u screen=(%d,%d) gl=(%d,%d)\n",
                      r.lookup.handle.bits,
                      (unsigned)r.lookup.handle.index(),
                      (unsigned)r.lookup.handle.generation(),
                      r.ctx.mouseX, r.ctx.mouseY,
                      r.ctx.glX,    r.ctx.glY);
              }
              break;
          }

          // Fog-of-war gate. Mirrors the FULL CPU-pick predicate at
          // code/missiongui.cpp:1272-1278 verbatim:
          //   target->isMover() &&
          //   !ShowMovers &&
          //   !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]) &&
          //   target->getTeamId() != Team::home->getId() &&
          //   !target->isDisabled() &&
          //   ((Mover*)target)->conStat < CONTACT_SENSOR_QUALITY_1
          // i.e. fog SUPPRESSES the pick ONLY when all five hold; ShowMovers
          // (debug) and the multiplayer-defeat carve-out are the bypasses
          // the CPU pick honors and that M2.6 MUST mirror or the inspect
          // log will be silent on a mech the CPU pick successfully selects.
          //
          // MC2_MECH_PICK_PIERCE_FOG=1 short-circuits the whole predicate.
          const bool pierce = RenderWorld::IsMechPickPierceFogEnabled();
          bool visible;
          if (pierce) {
              visible = true;
          } else {
              const bool showMovers = (ShowMovers != 0);
              const bool mpDefeat   = (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]);
              const bool hostile    = (bm->getTeamId() != Team::home->getId());
              const bool disabled   = bm->isDisabled();
              const bool sub_q1     = (((Mover*)bm)->conStat < CONTACT_SENSOR_QUALITY_1);
              const bool fogSuppresses =
                  !showMovers && !mpDefeat && hostile && !disabled && sub_q1;
              visible = !fogSuppresses;
          }
          if (!visible) {
              // Fog-gated; silent on default path so the substrate cannot
              // become a sensor cheat. Diagnostic log under DEBUG=1.
              if (RenderWorld::IsMechPickDebugEnabled()) {
                  std::fprintf(stderr,
                      "[GAMEPLAY_PICK v1] gated kind=Mech reason=fog_of_war "
                      "handle=%u screen=(%d,%d) gl=(%d,%d)\n",
                      r.lookup.handle.bits,
                      r.ctx.mouseX, r.ctx.mouseY,
                      r.ctx.glX,    r.ctx.glY);
              }
              break;
          }

          // Visible hit. Inspect-only: log the resolved identity.
          // (Debug-state mutation lives in Task 5 META-FIX once
          // setLastGameplayPick exists.)
          //
          // Log line carries handle bits/index/generation + the resolved
          // BattleMech pointer (debug; not a stable cookie). NO
          // gameObjectId / partId fields per CRITICAL-1.
          std::fprintf(stderr,
              "[GAMEPLAY_PICK v1] hit kind=Mech handle=%u idx=%u gen=%u "
              "mech=%p screen=(%d,%d) gl=(%d,%d) "
              "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)%s\n",
              r.lookup.handle.bits,
              (unsigned)r.lookup.handle.index(),
              (unsigned)r.lookup.handle.generation(),
              (void*)bm,
              r.ctx.mouseX, r.ctx.mouseY,
              r.ctx.glX,    r.ctx.glY,
              r.ctx.fboX,   r.ctx.fboY,
              r.ctx.vMulX,  r.ctx.vMulY,
              r.ctx.vAddX,  r.ctx.vAddY,
              r.ctx.drawableWidth, r.ctx.drawableHeight,
              pierce ? " pierce_fog=1" : "");
          break;
      }
      case GameplayPickResult::Outcome::miss: {
          // Mech caller silent on plain miss (background-pixel Shift+click
          // is high-frequency for terrain but uninteresting for mechs).
          // Verbose miss only under MC2_MECH_PICK_DEBUG=1.
          if (RenderWorld::IsMechPickDebugEnabled()) {
              std::fprintf(stderr,
                  "[GAMEPLAY_PICK v1] miss kind=Mech screen=(%d,%d) gl=(%d,%d)\n",
                  r.ctx.mouseX, r.ctx.mouseY,
                  r.ctx.glX,    r.ctx.glY);
          }
          break;
      }
      case GameplayPickResult::Outcome::gated:
      case GameplayPickResult::Outcome::skipped:
          // Mover-first short-circuit consumed the click (gated) or a
          // gesture/substrate gate failed (skipped). No-op, no log.
          break;
      }
  }
  ```

- [ ] **Step 7: Add second tail call at both style bodies.**

  **Existing (`code/missiongui.cpp:1539-1545` -- updateOldStyle tail):**
  ```cpp
  		tryStaticPropPick(moverSelectedThisFrame,
  		                  shiftDn,
  		                  leftClicked,
  		                  bGui,
  		                  bLeftDouble,
  		                  mouseX,
  		                  mouseY);
  ```

  **Replace with:**
  ```cpp
  		tryStaticPropPick(moverSelectedThisFrame,
  		                  shiftDn,
  		                  leftClicked,
  		                  bGui,
  		                  bLeftDouble,
  		                  mouseX,
  		                  mouseY);
  		// M2.6: mech-pick consumer fires second. Both callers invoke the
  		// shared tryGameplayPick spine; each kind-guards on r.lookup.kind.
  		// At most one hit branch fires per click (handle ranges disjoint:
  		// static-prop indices < kMechHandleBase, mech indices >= 65536).
  		tryMechPick(moverSelectedThisFrame,
  		            shiftDn,
  		            leftClicked,
  		            bGui,
  		            bLeftDouble,
  		            mouseX,
  		            mouseY);
  ```

  **Existing (`code/missiongui.cpp:1782-1788` -- updateAOEStyle tail):**
  ```cpp
  		tryStaticPropPick(moverSelectedThisFrame,
  		                  shiftDn,
  		                  leftClicked,
  		                  bGui,
  		                  bLeftDouble,
  		                  mouseX,
  		                  mouseY);
  ```

  **Replace with:**
  ```cpp
  		tryStaticPropPick(moverSelectedThisFrame,
  		                  shiftDn,
  		                  leftClicked,
  		                  bGui,
  		                  bLeftDouble,
  		                  mouseX,
  		                  mouseY);
  		// M2.6: mech-pick consumer (mirrors updateOldStyle tail).
  		tryMechPick(moverSelectedThisFrame,
  		            shiftDn,
  		            leftClicked,
  		            bGui,
  		            bLeftDouble,
  		            mouseX,
  		            mouseY);
  ```

- [ ] **Step 8: Build (full relink: new public symbols on `RenderWorld` + new method on `MissionInterfaceManager`).**

  ```powershell
  Remove-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
  & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64 --config RelWithDebInfo --target mc2
  ```

- [ ] **Step 9: Deploy + smoke tier1 env-OFF.**

  ```powershell
  Copy-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  fc /B A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  ```

  Pass criterion: tier1 5/5 PASS. Banner remains the M1.6 `[STATIC_PROP_PICK v1] enabled=...` form (per MAJOR-A: rename deferred to Task 5 Step 2a). No `[GAMEPLAY_PICK v1]` banner present until Task 5. No `[GAMEPLAY_PICK v1] hit kind=Mech` lines either (no clicks during smoke; gate default OFF).

- [ ] **Step 10: Smoke tier1 env-ON banner check.**

  ```powershell
  $env:MC2_OBJECT_ID_BUFFER='1'
  $env:MC2_MECH_PICK='1'
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  Remove-Item Env:\MC2_OBJECT_ID_BUFFER
  Remove-Item Env:\MC2_MECH_PICK
  ```

  Pass criterion: tier1 5/5 PASS. Latest smoke artifact's per-mission log contains the M1.6 banner unchanged (Task 4 does NOT rewrite the banner per MAJOR-A):
  ```
  [STATIC_PROP_PICK v1] enabled=0 debug=0
  ```
  The new five-field `[GAMEPLAY_PICK v1] ... mech_enabled=1 ...` banner appears only after Task 5 Step 2a.

- [ ] **Step 11: Commit.**

  ```powershell
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp code/missiongui.h code/missiongui.cpp
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m @'
  m2.6: add tryMechPick consumer + 3 env gates

  New caller MissionInterfaceManager::tryMechPick mirrors tryStaticPropPick
  shape, dispatches through the unchanged tryGameplayPick spine, kind-guards
  on r.lookup.kind == Mech, reverse-resolves the handle via
  GameAdapters::Mech::findMechByHandle (Task 3), applies the FULL
  CPU-pick fog predicate from code/missiongui.cpp:1272-1278 (ShowMovers
  + MPlayer-defeat bypasses included per adversarial MAJOR-1), and emits
  [GAMEPLAY_PICK v1] hit kind=Mech ... on a visible hit.

  New env gates: MC2_MECH_PICK (master), MC2_MECH_PICK_DEBUG (verbose
  miss/gated/stale logs), MC2_MECH_PICK_PIERCE_FOG (dev override).
  All default OFF; process-lifetime cached. Boot banner UNCHANGED in
  this commit per adversarial MAJOR-A: keeping the M1.6 banner literal
  paired with the still-M1.6 hit/miss emit literals preserves
  end-of-commit internal consistency. Task 5 META-FIX atomically renames
  banner + emit literals + doc comments together (no "extended banner"
  in this commit -- that lives in Task 5 Step 2a).

  Tail call sites: tryMechPick(...) added after tryStaticPropPick(...) at
  updateOldStyle:1539 and updateAOEStyle:1782. Both callers invoke the
  spine on the same click; kind-guard at each ensures at most one hit
  branch fires.

  Debug-state mutation for kind=Mech deferred to Task 5 (folded into
  the setLastGameplayPick rename for atomicity). The hit log already
  carries the resolved BattleMech pointer + full handle so log-driven
  inspection works in this commit.

  Tier1 5/5 PASS env-OFF AND env-ON (banner verified).

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  '@
  ```

---

## Task 5: Schema + state retirement (META-FIX)

**Files:** `RenderWorld/RenderWorld.h`, `RenderWorld/RenderWorld.cpp`, `code/missiongui.h`, `code/missiongui.cpp`, `code/gameplay_pick.h`

This is the load-bearing META-FIX commit. Substitutive rename of every M1.6 symbol/schema; Gate 6 enforces zero matches afterward. The mech-side debug-state mutation also lands here (in `tryMechPick`'s hit branch) so the new setter has both consumers from commit 1.

- [ ] **Step 1: Rename struct + 3 decls in RenderWorld/RenderWorld.h.**

  **Existing (`RenderWorld/RenderWorld.h:178-213`):**
  ```cpp
  // M1.6: most-recent static-prop pick debug state. Updated by
  // MissionInterfaceManager::tryStaticPropPick on a successful Shift+click
  // pick. Single-slot (latest wins); not a selection list. Not serialized.
  // Cleared on per-mission RenderWorld::destroy() so a stale handle does
  // not survive mission-load boundaries.
  //
  // Spec: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md sec 6.
  struct StaticPropSelectionDebugState {
      bool                            valid              = false;
      RenderCore::RenderObjectHandle  handle             = RenderCore::RenderObjectHandle::invalid();
      int32_t                         recipeIndex        = -1;
      int32_t                         lastPickMouseX     = 0;  // Win32 origin top-left
      int32_t                         lastPickMouseY     = 0;
      int32_t                         lastPickGlX        = 0;  // GL origin bottom-left
      int32_t                         lastPickGlY        = 0;
      uint64_t                        lastPickFrameIndex = 0;  // mirrors s_frameCounter at pick time
  };

  // M1.6: populate from a valid LookupResult. Asserts internally that
  // res.isValid == true (callers must filter; debug build only).
  // mouseX/Y are Win32-convention click coords; glX/Y are the post-y-flip
  // GL coords passed to lookupAtPixel. lastPickFrameIndex is sampled from
  // the internal frame counter at call time.
  void setLastStaticPropPick(const LookupResult& res,
                             int32_t mouseX, int32_t mouseY,
                             int32_t glX,    int32_t glY);

  // M1.6: reset to default (valid=false). Idempotent. Called on
  // (a) empty Shift+click (Q1 lean: clear on miss), and
  // (b) per-mission RenderWorld::destroy() lifecycle hook (Q2 lean).
  void clearLastStaticPropPick();

  // M1.6: read-only access. Caller MUST check .valid before consuming any
  // other field. Returns a copy (the struct is tiny; avoids exposing
  // internal mutex state to callers).
  StaticPropSelectionDebugState getLastStaticPropPick();
  ```

  **Replace with:**
  ```cpp
  // M2.6 (META-FIX of M1.6 StaticPropSelectionDebugState): most-recent
  // gameplay pick debug state. Single mutex-guarded slot; latest pick
  // across all kinds wins. Retires the per-kind state-slot pattern that
  // would otherwise multiply at M3 (terrain) and M4 (VFX).
  //
  // Kind-specific payload:
  //   kind == StaticProp -> recipeIndex carries the recipe (M1.6 semantic)
  //   kind == Mech       -> handle alone carries the identity. Callers
  //                          re-resolve via GameAdapters::Mech::findMechByHandle
  //                          to avoid stale-pointer dereference after destroyMech.
  //                          (Adversarial CRITICAL-1: no stable cookie at
  //                          syncSpawn -- partId reassigned post-init at
  //                          code/mission.cpp:2987. Handle IS the identity.)
  //   future kinds       -> add a tagged-union payload field at that
  //                          slice; do NOT widen the struct prematurely.
  //
  // Cleared on per-mission RenderWorld::destroy() (same lifecycle as the
  // retired StaticPropSelectionDebugState).
  //
  // Spec: 2026-05-23-renderworld-slice-m2-6-mech-pickup-spec.md sec 4.3.
  struct GameplaySelectionDebugState {
      bool                            valid              = false;
      RenderObjectKind                kind               = RenderObjectKind::StaticProp;
      RenderCore::RenderObjectHandle  handle             = RenderCore::RenderObjectHandle::invalid();
      int32_t                         recipeIndex        = -1;  // kind==StaticProp only; -1 otherwise
      int32_t                         lastPickMouseX     = 0;   // Win32 origin top-left
      int32_t                         lastPickMouseY     = 0;
      int32_t                         lastPickGlX        = 0;   // GL origin bottom-left
      int32_t                         lastPickGlY        = 0;
      uint64_t                        lastPickFrameIndex = 0;
  };

  // M2.6: populate from a valid LookupResult. Caller passes the
  // pre-checked kind (must match res.kind). Kind-specific payload
  // extracted from the appropriate source:
  //   kind==StaticProp: recipeIndex sampled from handleToRecipeIndex
  //                     (existing M1.6 path)
  //   kind==Mech:       no kind-specific payload; handle alone identifies
  //                     the mech (CRITICAL-1 note above).
  void setLastGameplayPick(RenderObjectKind kind,
                           const LookupResult& res,
                           int32_t mouseX, int32_t mouseY,
                           int32_t glX,    int32_t glY);

  // M2.6: reset to default (valid=false). Idempotent. Called on
  // (a) empty Shift+click from the static-prop caller, and
  // (b) per-mission RenderWorld::destroy() lifecycle hook.
  void clearLastGameplayPick();

  // M2.6: read-only access. Caller MUST check .valid before consuming
  // any other field; then dispatch on .kind for payload semantics.
  GameplaySelectionDebugState getLastGameplayPick();
  ```

  Also update the `IsStaticPropPickDebugEnabled` doc comment in the HEADER (per MAJOR-D: Task 4 INTENTIONALLY left this verbatim M1.6 to keep end-of-Task-4 internally consistent; the rename lives here atomically with the emit-site rename below).

  **Existing (`RenderWorld/RenderWorld.h:96-103`):**
  ```cpp
  // M1.6: static-prop pick verbose-log enable. When this is OFF, the
  // `[STATIC_PROP_PICK v1] miss ...` line is suppressed (high-frequency
  // empty-Shift+click gesture would otherwise spam stderr). `hit` lines
  // fire unconditionally per spec Section 7.
  ```

  **Replace with:**
  ```cpp
  // M1.6 + M2.6: static-prop pick verbose-log enable. When this is OFF,
  // the `[GAMEPLAY_PICK v1] miss kind=StaticProp ...` line is suppressed
  // (high-frequency empty-Shift+click gesture would otherwise spam
  // stderr). `hit` lines fire unconditionally per spec Section 7.
  ```

  Update `IsObjectIdBufferEnabled` consumer-list doc comment if it references the retired schema. Re-grep at write time:

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.h -Pattern 'STATIC_PROP_PICK|StaticPropSelectionDebugState|setLastStaticPropPick|clearLastStaticPropPick|getLastStaticPropPick' -SimpleMatch
  ```

  Every remaining hit in the header must be either renamed or removed in this step.

- [ ] **Step 2a: Rewrite boot banner at RenderWorld.cpp:484 (deferred from Task 4 per MAJOR-A).**

  This rewrite lives here so banner + emit-site literals + doc comments rename in one atomic META-FIX commit.

  **Existing (`RenderWorld/RenderWorld.cpp:482-486`):**
  ```cpp
      // M1.6: pick-wiring banner. Always emitted (both 0/0 and 1/1 states
      // useful to log readers diagnosing "why did Shift+click do nothing").
      std::fprintf(stderr, "[STATIC_PROP_PICK v1] enabled=%d debug=%d\n",
                   IsStaticPropPickEnabled() ? 1 : 0,
                   IsStaticPropPickDebugEnabled() ? 1 : 0);
  ```

  **Replace with:**
  ```cpp
      // M1.6 + M2.6: pick-wiring banner. Always emitted; useful to log
      // readers diagnosing "why did Shift+click do nothing". M2.6 extends
      // the line with the three mech-pick gate states under the unified
      // [GAMEPLAY_PICK v1] schema name.
      std::fprintf(stderr,
                   "[GAMEPLAY_PICK v1] static_prop_enabled=%d static_prop_debug=%d "
                   "mech_enabled=%d mech_debug=%d mech_pierce_fog=%d\n",
                   IsStaticPropPickEnabled() ? 1 : 0,
                   IsStaticPropPickDebugEnabled() ? 1 : 0,
                   IsMechPickEnabled() ? 1 : 0,
                   IsMechPickDebugEnabled() ? 1 : 0,
                   IsMechPickPierceFogEnabled() ? 1 : 0);
  ```

  Also: update the `IsStaticPropPickDebugEnabled` doc comment INSIDE the .cpp (MAJOR-B drift hit at `RenderWorld.cpp:97-98` if a sibling doc comment exists there). Grep:

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern '\[STATIC_PROP_PICK v1\]' -SimpleMatch
  ```

  Rename every remaining `[STATIC_PROP_PICK v1]` literal in the .cpp doc comments to `[GAMEPLAY_PICK v1]` (or `[GAMEPLAY_PICK v1] kind=StaticProp` where the comment specifically describes the static-prop branch).

- [ ] **Step 2: Rename impls + storage + mutex + lifecycle in RenderWorld/RenderWorld.cpp.**

  Rename the file-scope static and its mutex:

  **Existing (`RenderWorld/RenderWorld.cpp:99` plus the sibling mutex line nearby):**
  ```cpp
  RenderWorld::StaticPropSelectionDebugState       s_lastStaticPropPick;
  ```

  **Replace with:**
  ```cpp
  RenderWorld::GameplaySelectionDebugState         s_lastGameplayPick;
  ```

  Locate the sibling mutex with grep at write time:
  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern 's_lastStaticPropPickMutex' -SimpleMatch
  ```
  Rename every hit to `s_lastGameplayPickMutex`.

  Rename the three impls at `:729-764`. Locate with grep:
  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern 'setLastStaticPropPick|clearLastStaticPropPick|getLastStaticPropPick' -SimpleMatch
  ```

  **Existing impl block (`RenderWorld/RenderWorld.cpp:729-764`):**
  ```cpp
  void setLastStaticPropPick(const LookupResult& res,
                             int32_t mouseX, int32_t mouseY,
                             int32_t glX,    int32_t glY)
  {
      // Callers MUST filter on res.isValid before calling. We do not assert
      // here (release-mode safety) but a misuse populates a "valid pick"
      // with an invalid handle, which the next get() consumer will see
      // and either skip or log-spam. Filter at the call site.
      std::lock_guard<std::mutex> lk(s_lastStaticPropPickMutex);
      s_lastStaticPropPick.valid              = res.isValid;
      s_lastStaticPropPick.handle             = res.handle;
      // recipeIndex: project handle -> recipe index via the existing
      // inverse mapper handleToRecipeIndex (declared in this TU; takes a
      // full RenderObjectHandle and returns int32_t). Per CRIT C1 of
      // plan-review: the correct symbol is handleToRecipeIndex, NOT
      // handleIndexToRecipeIndex.
      s_lastStaticPropPick.recipeIndex        = res.isValid
          ? handleToRecipeIndex(res.handle)
          : -1;
      s_lastStaticPropPick.lastPickMouseX     = mouseX;
      s_lastStaticPropPick.lastPickMouseY     = mouseY;
      s_lastStaticPropPick.lastPickGlX        = glX;
      s_lastStaticPropPick.lastPickGlY        = glY;
      s_lastStaticPropPick.lastPickFrameIndex =
          s_frameCounter.load(std::memory_order_relaxed);
  }

  void clearLastStaticPropPick() {
      std::lock_guard<std::mutex> lk(s_lastStaticPropPickMutex);
      s_lastStaticPropPick = StaticPropSelectionDebugState{};
  }

  StaticPropSelectionDebugState getLastStaticPropPick() {
      std::lock_guard<std::mutex> lk(s_lastStaticPropPickMutex);
      return s_lastStaticPropPick;  // copy out; struct is tiny
  }
  ```

  **Replace with:**
  ```cpp
  void setLastGameplayPick(RenderObjectKind kind,
                           const LookupResult& res,
                           int32_t mouseX, int32_t mouseY,
                           int32_t glX,    int32_t glY)
  {
      // Callers MUST filter on res.isValid before calling. We do not assert
      // here (release-mode safety) but a misuse populates a "valid pick"
      // with an invalid handle, which the next get() consumer will see
      // and either skip or log-spam. Filter at the call site.
      //
      // M2.6: kind drives payload semantics. recipeIndex is set only when
      // kind==StaticProp; mech kind leaves recipeIndex at -1 (the handle
      // identifies the mech; callers re-resolve via findMechByHandle).
      std::lock_guard<std::mutex> lk(s_lastGameplayPickMutex);
      s_lastGameplayPick.valid              = res.isValid;
      s_lastGameplayPick.kind               = kind;
      s_lastGameplayPick.handle             = res.handle;
      s_lastGameplayPick.recipeIndex        =
          (res.isValid && kind == RenderObjectKind::StaticProp)
              ? handleToRecipeIndex(res.handle)
              : -1;
      s_lastGameplayPick.lastPickMouseX     = mouseX;
      s_lastGameplayPick.lastPickMouseY     = mouseY;
      s_lastGameplayPick.lastPickGlX        = glX;
      s_lastGameplayPick.lastPickGlY        = glY;
      s_lastGameplayPick.lastPickFrameIndex =
          s_frameCounter.load(std::memory_order_relaxed);
  }

  void clearLastGameplayPick() {
      std::lock_guard<std::mutex> lk(s_lastGameplayPickMutex);
      s_lastGameplayPick = GameplaySelectionDebugState{};
  }

  GameplaySelectionDebugState getLastGameplayPick() {
      std::lock_guard<std::mutex> lk(s_lastGameplayPickMutex);
      return s_lastGameplayPick;  // copy out; struct is tiny
  }
  ```

  Update the lifecycle hook at `RenderWorld.cpp:506`:

  **Existing:**
  ```cpp
      clearLastStaticPropPick();
  ```

  **Replace with:**
  ```cpp
      clearLastGameplayPick();
  ```

  Update doc comments at `:93`, `:599`, `:605`:

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern 'setLastStaticPropPick|\[STATIC_PROP_PICK v1\]' -SimpleMatch
  ```

  Each remaining doc-comment hit must be renamed to the new symbol/schema. (The boot banner at `:484` is renamed in Step 2a above.)

- [ ] **Step 3: Rename M1.6 static-prop hit/miss/setter+log in code/missiongui.cpp.**

  Locate with grep:
  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\code\missiongui.cpp -Pattern 'setLastStaticPropPick|clearLastStaticPropPick|getLastStaticPropPick|StaticPropSelectionDebugState|\[STATIC_PROP_PICK v1\]' -SimpleMatch
  ```

  Patch the hit branch updated in Task 2:

  **Existing (`code/missiongui.cpp:6218-6242` post Task 2):**
  ```cpp
          RenderWorld::setLastStaticPropPick(r.lookup,
                                             r.ctx.mouseX, r.ctx.mouseY,
                                             r.ctx.glX,    r.ctx.glY);
          // Sample back the debug-state struct so the log can include the
          // recipeIndex (LookupResult itself does not carry it; the
          // recipe lookup is done inside setLastStaticPropPick).
          const RenderWorld::StaticPropSelectionDebugState picked =
              RenderWorld::getLastStaticPropPick();
          // Unconditional hit log (spec Section 7); coord-diag fields
          // BYTE-IDENTICAL to M1.6 to keep user-driven canary stable.
          std::fprintf(stderr,
              "[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "
              "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
              "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
  ```

  **Replace with:**
  ```cpp
          RenderWorld::setLastGameplayPick(RenderWorld::RenderObjectKind::StaticProp,
                                           r.lookup,
                                           r.ctx.mouseX, r.ctx.mouseY,
                                           r.ctx.glX,    r.ctx.glY);
          // Sample back the unified debug-state struct so the log can
          // include the recipeIndex (LookupResult itself does not carry
          // it; the recipe lookup is done inside setLastGameplayPick).
          const RenderWorld::GameplaySelectionDebugState picked =
              RenderWorld::getLastGameplayPick();
          // Unified hit log (META-FIX of M1.6 [STATIC_PROP_PICK v1]);
          // coord-diag fields BYTE-IDENTICAL to M1.6 to keep the
          // user-driven canary semantically stable across rename.
          std::fprintf(stderr,
              "[GAMEPLAY_PICK v1] hit kind=StaticProp handle=%u idx=%u gen=%u "
              "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
              "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
  ```

  Patch the miss branch (`code/missiongui.cpp:6244-6263`):

  **Existing:**
  ```cpp
      case GameplayPickResult::Outcome::miss: {
          // Q1 lean: clear the debug-state struct on empty Shift+click so
          // a stale prior pick does not survive an empty-click gesture.
          RenderWorld::clearLastStaticPropPick();
          // Verbose miss log only when MC2_STATIC_PROP_PICK_DEBUG=1;
          // coord-diag BYTE-IDENTICAL to M1.6.
          if (RenderWorld::IsStaticPropPickDebugEnabled()) {
              std::fprintf(stderr,
                  "[STATIC_PROP_PICK v1] miss screen=(%d,%d) gl=(%d,%d) "
                  "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) "
                  "draw=(%d,%d)\n",
  ```

  **Replace with:**
  ```cpp
      case GameplayPickResult::Outcome::miss: {
          // Clear the unified debug-state struct on empty Shift+click so
          // a stale prior pick does not survive an empty-click gesture.
          RenderWorld::clearLastGameplayPick();
          // Verbose miss log only when MC2_STATIC_PROP_PICK_DEBUG=1;
          // coord-diag BYTE-IDENTICAL to M1.6 modulo the schema prefix.
          if (RenderWorld::IsStaticPropPickDebugEnabled()) {
              std::fprintf(stderr,
                  "[GAMEPLAY_PICK v1] miss kind=StaticProp screen=(%d,%d) gl=(%d,%d) "
                  "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) "
                  "draw=(%d,%d)\n",
  ```

  Add the debug-state mutation to the mech hit branch (right after the visible-hit log emit). Locate the mech hit log:
  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\code\missiongui.cpp -Pattern '\[GAMEPLAY_PICK v1\] hit kind=Mech' -SimpleMatch
  ```

  **Existing (the mech-hit emit added in Task 4 step 6; minus the trailing `break;` line):**
  ```cpp
          // Visible hit. Inspect-only: log the resolved identity.
          // (Debug-state mutation lives in Task 5 META-FIX once
          // setLastGameplayPick exists.)
          //
          // Log line carries handle bits/index/generation + the resolved
          // BattleMech pointer (debug; not a stable cookie). NO
          // gameObjectId / partId fields per CRITICAL-1.
          std::fprintf(stderr,
              "[GAMEPLAY_PICK v1] hit kind=Mech handle=%u idx=%u gen=%u "
              "mech=%p screen=(%d,%d) gl=(%d,%d) "
  ```

  **Replace with (insert setLastGameplayPick call BEFORE the fprintf; update the comment block):**
  ```cpp
          // Visible hit. Inspect-only: update unified debug state + log.
          RenderWorld::setLastGameplayPick(RenderWorld::RenderObjectKind::Mech,
                                           r.lookup,
                                           r.ctx.mouseX, r.ctx.mouseY,
                                           r.ctx.glX,    r.ctx.glY);
          // Log line carries handle bits/index/generation + the resolved
          // BattleMech pointer (debug; not a stable cookie). NO
          // gameObjectId / partId fields per CRITICAL-1.
          std::fprintf(stderr,
              "[GAMEPLAY_PICK v1] hit kind=Mech handle=%u idx=%u gen=%u "
              "mech=%p screen=(%d,%d) gl=(%d,%d) "
  ```

  Also remove the M1.6 schema literal from the include comment at top of file:

  **Existing (`code/missiongui.cpp:31-32`):**
  ```cpp
  // M1.6 + M2.6: IsStaticPropPickEnabled, lookupAtPixel, setLastStaticPropPick,
  // clearLastStaticPropPick, getLastStaticPropPick, IsObjectIdBufferEnabled,
  // IsMechPickEnabled, IsMechPickDebugEnabled, IsMechPickPierceFogEnabled.
  ```

  **Replace with:**
  ```cpp
  // M1.6 + M2.6: IsStaticPropPickEnabled, lookupAtPixel, setLastGameplayPick,
  // clearLastGameplayPick, getLastGameplayPick, IsObjectIdBufferEnabled,
  // IsMechPickEnabled, IsMechPickDebugEnabled, IsMechPickPierceFogEnabled.
  ```

  Also update the wrapper-doc comment at `code/missiongui.cpp:6175`:

  **Existing:**
  ```cpp
  //      (debug-state mutation + [STATIC_PROP_PICK v1] hit/miss logs).
  ```

  **Replace with:**
  ```cpp
  //      (debug-state mutation + [GAMEPLAY_PICK v1] kind=StaticProp hit/miss logs).
  ```

- [ ] **Step 4: Rename doc comment in code/missiongui.h.**

  **Existing (`code/missiongui.h:265-269`):**
  ```cpp
  		// both updateOldStyle and updateAOEStyle when leftClicked && shiftDn
  		// && !bGui. Short-circuits when moverSelectedThisFrame == true so
  		// the legacy Shift+LMB additive-select gesture on a friendly mover
  		// is preserved verbatim (no M1.6 log line in that case). Emits
  		// [STATIC_PROP_PICK v1] hit/miss and updates RenderWorld debug state.
  ```

  **Replace with:**
  ```cpp
  		// both updateOldStyle and updateAOEStyle when leftClicked && shiftDn
  		// && !bGui. Short-circuits when moverSelectedThisFrame == true so
  		// the legacy Shift+LMB additive-select gesture on a friendly mover
  		// is preserved verbatim (no log line in that case). Emits
  		// [GAMEPLAY_PICK v1] hit kind=StaticProp / miss and updates the
  		// unified RenderWorld debug state.
  ```

- [ ] **Step 5: Rename doc comment in code/gameplay_pick.h.**

  **Existing (`code/gameplay_pick.h:42-46`):**
  ```cpp
  // Diagnostic context propagated to caller for logging. All fields echo
  // the inputs + the intermediate coord-translation results so caller
  // logs can show the full transform on one line. Field set mirrors the
  // M1.6 [STATIC_PROP_PICK v1] hit/miss log printf args exactly.
  struct GameplayPickContext {
  ```

  **Replace with:**
  ```cpp
  // Diagnostic context propagated to caller for logging. All fields echo
  // the inputs + the intermediate coord-translation results so caller
  // logs can show the full transform on one line. Field set mirrors the
  // M2.6 [GAMEPLAY_PICK v1] hit/miss log printf args exactly.
  struct GameplayPickContext {
  ```

- [ ] **Step 6: Build (full relink: signature change on RenderWorld setter family).**

  ```powershell
  Remove-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
  & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64 --config RelWithDebInfo --target mc2
  ```

- [ ] **Step 7: Gate 6 substitutive proof (load-bearing).**

  All four greps must return ZERO hits across code+headers+test/script tooling. If any returns a hit, fix it before commit.

  ```powershell
  Push-Location A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev
  Select-String -Path RenderWorld\*,code\*,GameAdapters\* -Pattern 'setLastStaticPropPick|clearLastStaticPropPick|getLastStaticPropPick' -List
  Select-String -Path RenderWorld\*,code\*,GameAdapters\* -Pattern 'StaticPropSelectionDebugState' -List
  Select-String -Path RenderWorld\*,code\*,GameAdapters\* -Pattern '\[STATIC_PROP_PICK v1\]' -List
  Select-String -Path tests\*,scripts\* -Pattern 'setLastStaticPropPick|getLastStaticPropPick|clearLastStaticPropPick|StaticPropSelectionDebugState|\[STATIC_PROP_PICK v1\]' -List
  Pop-Location
  ```

  Pass criterion: all four return no matches. Per adversarial MINOR-5: dated SPEC/PLAN/REVIEW/RECON/HANDOFF artifacts under `.claude/worktrees/*/docs/superpowers/` and the worktree `CLAUDE.md` archaeology pointer ARE legitimate retired-schema references; they are the ONLY .claude/ carve-outs. Everything else under `.claude/` (skills, agents, settings) IS policed.

  Re-grep with `--include='*.h' --include='*.cpp'` discipline as well. Per adversarial MINOR-5: narrow the `.claude/` exclusion to ONLY `docs/superpowers/` planning artifacts + the worktree CLAUDE.md (which legitimately reference the retired schema as archaeology pointers). Everything else under `.claude/` (skills, agents, etc.) IS policed.

  ```powershell
  Get-ChildItem -Recurse -Include *.h,*.cpp,*.md A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev | Select-String -Pattern 'setLastStaticPropPick|StaticPropSelectionDebugState|\[STATIC_PROP_PICK v1\]|clearLastStaticPropPick|getLastStaticPropPick' | Where-Object { $_.Path -notmatch '\\docs\\superpowers\\' -and $_.Path -notmatch '\\CLAUDE\.md$' }
  ```

  Pass criterion: empty result set.

- [ ] **Step 8: Deploy + smoke tier1 env-OFF AND env-ON.**

  ```powershell
  Copy-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  fc /B A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  ```

  Then env-ON:
  ```powershell
  $env:MC2_OBJECT_ID_BUFFER='1'
  $env:MC2_MECH_PICK='1'
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  Remove-Item Env:\MC2_OBJECT_ID_BUFFER
  Remove-Item Env:\MC2_MECH_PICK
  ```

  Pass criterion: tier1 5/5 PASS in both runs. Latest env-ON artifact contains the new banner; no `[STATIC_PROP_PICK v1]` lines.

- [ ] **Step 9: Commit.**

  ```powershell
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp code/missiongui.h code/missiongui.cpp code/gameplay_pick.h
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m @'
  m2.6 META-FIX: retire StaticPropSelectionDebugState + [STATIC_PROP_PICK v1]

  Substitutive (not additive) rename across the four surfaces:
   - struct StaticPropSelectionDebugState -> GameplaySelectionDebugState
     (+ kind discriminator field)
   - setLastStaticPropPick/clear/get -> setLastGameplayPick(kind, ...)/clear/get
   - log schema [STATIC_PROP_PICK v1] -> [GAMEPLAY_PICK v1] kind=...
   - file-scope storage s_lastStaticPropPick + sibling mutex renamed

  Gate 6 substitutive proof: grep -E 'setLastStaticPropPick|
  clearLastStaticPropPick|getLastStaticPropPick|
  StaticPropSelectionDebugState|\[STATIC_PROP_PICK v1\]' returns ZERO
  matches across RenderWorld/ + code/ + GameAdapters/ + header doc
  comments + tests/ + scripts/ (excluding .claude/worktrees dated
  planning artifacts, which legitimately describe the retired state).

  Mech-side debug-state mutation (setLastGameplayPick(Mech, ...))
  added to tryMechPick's hit branch in the same commit so the new
  setter has both consumers atomically.

  Citation drift fixed: doc comments at code/gameplay_pick.h:45,
  code/missiongui.h:269, code/missiongui.cpp:31-32/6175, plus
  RenderWorld/RenderWorld.cpp:99 (storage) and :506 (lifecycle).

  Tier1 5/5 PASS env-OFF AND env-ON (MC2_OBJECT_ID_BUFFER=1
  MC2_MECH_PICK=1; new schema banner verified; no legacy schema
  literals in any log line).

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  '@
  ```

---

## Task 6: RunMechPickSelfTest() + RenderWorld::init wire

**Files:** `RenderWorld/RenderWorld.cpp` (host the test alongside `RunMechObjectIdSelfTest`)

Per spec Section 10 Gate 4 + Q1 MINOR-clarification (the scan-validates-itself form; init-time has no real BattleMechs, so the test validates scan well-formed-ness, not data state).

Note (per adversarial MAJOR-C, documentation-only): the self-test calls `registerMech(desc)` once per process invocation, which allocates a slot in `s_objectRecords` and advances `s_nextMechSlot` by one. The slot is then retired via `destroyMech(h)`, but slot-counter advancement is permanent for the process. This mirrors the M2.5 `RunMechObjectIdSelfTest` precedent (already shipping with the same pattern; per CLAUDE.md M2.5 entry `gpu_mech_id_writes=63836` on mc2_01 with the self-test active proves the consumption is benign in practice). No code change required.

- [ ] **Step 1: Verify wire-up slot exists.**

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern 'RunMechObjectIdSelfTest\(\);' -SimpleMatch
  ```

  Must return at least the forward-decl line and the call site at `:496`.

- [ ] **Step 2: Add the test impl + a fwd-decl at the top of RenderWorld.cpp.**

  Forward-decl block (top of file, where other RunXxx decls live around `:40-45`):

  **Existing:**
  ```cpp
  void RunGameplayPickSelfTest();
  ```
  (and below)
  ```cpp
  void RunMechObjectIdSelfTest();
  ```

  **Insert after `RunMechObjectIdSelfTest` decl:**
  ```cpp
  void RunMechPickSelfTest();
  ```

  Implementation body (place AFTER `RunMechObjectIdSelfTest` impl; locate with grep):
  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern '^void RunMechObjectIdSelfTest\(\)' -SimpleMatch
  ```

  Insert (must include `<cstdlib>` for getenv, already included by the TU; and the adapter header for `findMechByHandle`):

  At the top includes (just below `#include "../GameAdapters/MechRenderAdapter.h"` if present; if not present, add it):

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern 'MechRenderAdapter.h' -SimpleMatch
  ```

  If not present, add the include in the engine-side section. Otherwise reuse it.

  ```cpp
  // M2.6: end-to-end mech pickup self-test. Gated on
  // MC2_MECH_PICK_SELFTEST=1 AND IsObjectIdBufferEnabled() (substrate
  // must be on; otherwise the test is vacuous). Mirrors the
  // RunMechObjectIdSelfTest shape; co-located in this TU per M2.5
  // per-domain co-location preference.
  //
  // Validates:
  //   1. registerMech yields a fresh handle with kind=Mech.
  //   2. lookupAtPixel-equivalent record fetch round-trips kind=Mech
  //      via the existing s_objectRecords table (reuses substrate proof).
  //   3. findMechByHandle SCAN itself is well-formed (per MINOR-clarif:
  //      init-time has no real BattleMechs, so the test validates the
  //      SCAN's invariants -- invalid input -> nullptr; synthetic-handle
  //      input -> nullptr (no real mech matches); both prove the
  //      implementation does not crash or false-positive).
  //   4. destroyMech retires the handle (post-destroy lookup is invalid).
  //
  // Emit:
  //   [MECH_PICK_SELFTEST v1] result=PASS step=all
  //   [MECH_PICK_SELFTEST v1] result=FAIL step=N reason=...
  void RunMechPickSelfTest() {
      const char* envSelftest = std::getenv("MC2_MECH_PICK_SELFTEST");
      const bool enabled = envSelftest && envSelftest[0] && envSelftest[0] != '0';
      if (!enabled) return;
      if (!IsObjectIdBufferEnabled()) {
          std::fprintf(stderr,
              "[MECH_PICK_SELFTEST v1] result=SKIP reason=substrate_off\n");
          return;
      }

      // Step 1: register a synthetic mech.
      RenderMechDesc desc{};
      desc.mechTypeId    = 0u;
      desc.gameObjectId  = 0xC0FFEEu;
      desc.debugCookie   = 0u;
      RenderCore::RenderObjectHandle h = registerMech(desc);
      if (!h.isValid()) {
          std::fprintf(stderr,
              "[MECH_PICK_SELFTEST v1] result=FAIL step=1 reason=register_failed\n");
          return;
      }

      // Step 2: round-trip kind via the record table (synthetic; no
      // pixel readback -- gate 4 in the spec already covers the
      // pixel round-trip via RunMechObjectIdSelfTest).
      {
          std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
          if (h.index() >= s_objectRecords.size()) {
              std::fprintf(stderr,
                  "[MECH_PICK_SELFTEST v1] result=FAIL step=2 reason=index_oob\n");
              destroyMech(h);
              return;
          }
          const RenderObjectRecord& rec = s_objectRecords[h.index()];
          if (rec.kind != RenderObjectKind::Mech) {
              std::fprintf(stderr,
                  "[MECH_PICK_SELFTEST v1] result=FAIL step=2 reason=kind_mismatch got=%u\n",
                  (unsigned)rec.kind);
              destroyMech(h);
              return;
          }
      }

      // Step 3a: scan with invalid handle -> nullptr.
      if (GameAdapters::Mech::findMechByHandle(RenderCore::RenderObjectHandle::invalid()) != nullptr) {
          std::fprintf(stderr,
              "[MECH_PICK_SELFTEST v1] result=FAIL step=3a reason=invalid_returned_nonnull\n");
          destroyMech(h);
          return;
      }
      // Step 3b: scan with synthetic handle. Either nullptr (no real
      // BattleMech in ObjectManager at init-time; expected) OR a
      // non-null BattleMech whose getRenderWorldHandle().raw() matches
      // (would only happen if a real mech has been registered with the
      // same raw bits, which is impossible at init-time). Both outcomes
      // prove the scan well-formed; neither is a fail.
      BattleMech* found = GameAdapters::Mech::findMechByHandle(h);
      if (found != nullptr) {
          // Sanity: if the scan returned non-null, the bits must match.
          Mech3DAppearance* app = static_cast<Mech3DAppearance*>(found->getAppearance());
          if (app == nullptr || app->getRenderWorldHandle().raw() != h.raw()) {
              std::fprintf(stderr,
                  "[MECH_PICK_SELFTEST v1] result=FAIL step=3b reason=scan_bits_mismatch\n");
              destroyMech(h);
              return;
          }
      }

      // Step 4: destroyMech retires the handle.
      destroyMech(h);
      // Post-destroy: synthetic handle now stale (generation bump). The
      // record-table fetch must show the slot retired (or, if recycled,
      // a fresh generation). We do not try to round-trip lookupAtPixel
      // here (no GPU state at init-time).

      std::fprintf(stderr,
          "[MECH_PICK_SELFTEST v1] result=PASS step=all\n");
  }
  ```

  Note: the test reads `s_objectRecords` and `s_objectRecordsMutex` from the same TU (both file-scope statics in `RenderWorld.cpp`). The test reaches into `GameAdapters::Mech::findMechByHandle`, `Mech3DAppearance::getRenderWorldHandle`, and `BattleMech::getAppearance` -- so the TU must include `../GameAdapters/MechRenderAdapter.h` AND have visibility on `BattleMech` + `Mech3DAppearance` types. Since the test is INSIDE `RenderWorld.cpp` and RenderWorld is firewall-clean (must not include `mech3d.h` or `code/mech.h`), this is a firewall violation if done naively.

  **Resolution:** host `RunMechPickSelfTest` in `GameAdapters/MechRenderAdapter.cpp` instead (which already includes the relevant headers + has access to `findMechByHandle` directly). Forward-declare `RunMechPickSelfTest` from `RenderWorld/RenderWorld.cpp` so the init() wire still calls it cleanly.

  Revised placement: implement in `GameAdapters/MechRenderAdapter.cpp` under a free `RunMechPickSelfTest` symbol (extern "C" linkage NOT needed; ordinary external linkage is fine). Forward-decl in `RenderWorld/RenderWorld.cpp` near the existing `RunMechObjectIdSelfTest` forward-decl.

  **Forward-decl block in RenderWorld/RenderWorld.cpp (around line 40-45):**

  **Existing:**
  ```cpp
  void RunMechObjectIdSelfTest();
  ```

  **Replace with:**
  ```cpp
  void RunMechObjectIdSelfTest();
  void RunMechPickSelfTest();
  ```

  **Implementation in GameAdapters/MechRenderAdapter.cpp** (insert at end of the namespace block, after `findMechByHandle`):

  ```cpp
  // M2.6: end-to-end mech-pick self-test. Hosted here (not in
  // RenderWorld.cpp) because the test exercises findMechByHandle, which
  // reaches into game-side BattleMech / Mech3DAppearance -- RenderWorld
  // must not include those headers. Forward-decl'd from RenderWorld.cpp;
  // wired into RenderWorld::init() after RunMechObjectIdSelfTest.
  //
  // Gated on MC2_MECH_PICK_SELFTEST=1 AND substrate enabled. Validates:
  //  1. registerMech yields a fresh handle.
  //  2. findMechByHandle(invalid) -> nullptr.
  //  3. findMechByHandle(synthetic_handle) -> nullptr at init-time
  //     (no real BattleMech in ObjectManager), or non-null with
  //     matching bits if a real mech somehow occupies the same slot
  //     (impossible in practice). Either is a pass; the test proves
  //     scan well-formed-ness, not data state (per MINOR-clarification).
  //  4. destroyMech retires the handle.
  } // namespace Mech
  } // namespace GameAdapters

  // Free function in the global namespace so RenderWorld.cpp can
  // forward-declare and call it without a header dependency.
  void RunMechPickSelfTest() {
      const char* envSelftest = std::getenv("MC2_MECH_PICK_SELFTEST");
      const bool enabled = envSelftest && envSelftest[0] && envSelftest[0] != '0';
      if (!enabled) return;
      if (!RenderWorld::IsObjectIdBufferEnabled()) {
          std::fprintf(stderr,
              "[MECH_PICK_SELFTEST v1] result=SKIP reason=substrate_off\n");
          return;
      }
      // External-review m2: sample live-mech count BEFORE the synthetic
      // register/destroy pair so we can assert no drift afterwards.
      // RenderWorld::getMechsAliveCount() is the public accessor added
      // alongside this self-test (see "live-count accessor" sub-step below).
      const uint64_t mechsAliveBefore = RenderWorld::getMechsAliveCount();

      // Step 1: register a synthetic mech.
      RenderWorld::RenderMechDesc desc{};
      desc.mechTypeId    = 0u;
      desc.gameObjectId  = 0xC0FFEEu;
      desc.debugCookie   = 0u;
      RenderCore::RenderObjectHandle h = RenderWorld::registerMech(desc);
      if (!h.isValid()) {
          std::fprintf(stderr,
              "[MECH_PICK_SELFTEST v1] result=FAIL step=1 reason=register_failed\n");
          return;
      }
      // Step 2: scan with invalid handle -> nullptr.
      if (GameAdapters::Mech::findMechByHandle(RenderCore::RenderObjectHandle::invalid()) != nullptr) {
          std::fprintf(stderr,
              "[MECH_PICK_SELFTEST v1] result=FAIL step=2 reason=invalid_returned_nonnull\n");
          RenderWorld::destroyMech(h);
          return;
      }
      // Step 3: scan with synthetic handle. Init-time has no real
      // BattleMech in ObjectManager, so we expect nullptr.
      BattleMech* found = GameAdapters::Mech::findMechByHandle(h);
      if (found != nullptr) {
          // If a real mech happens to occupy this slot, sanity-check
          // the bits. (Init-time should never hit this path.)
          Mech3DAppearance* app = static_cast<Mech3DAppearance*>(found->getAppearance());
          if (app == nullptr || app->getRenderWorldHandle().raw() != h.raw()) {
              std::fprintf(stderr,
                  "[MECH_PICK_SELFTEST v1] result=FAIL step=3 reason=scan_bits_mismatch\n");
              RenderWorld::destroyMech(h);
              return;
          }
      }
      // Step 4: destroyMech retires the handle.
      RenderWorld::destroyMech(h);

      // Step 5 (external-review m2): assert live-mech count returned to
      // baseline. A drift means destroyMech did not balance registerMech
      // and the slot leaked (would compound across init-time invocations).
      const uint64_t mechsAliveAfter = RenderWorld::getMechsAliveCount();
      if (mechsAliveAfter != mechsAliveBefore) {
          std::fprintf(stderr,
              "[MECH_PICK_SELFTEST v1] result=FAIL step=5 reason=live_count_drift "
              "before=%llu after=%llu\n",
              (unsigned long long)mechsAliveBefore,
              (unsigned long long)mechsAliveAfter);
          return; // do not emit PASS
      }

      std::fprintf(stderr,
          "[MECH_PICK_SELFTEST v1] result=PASS step=all\n");
  }

  // Restore the namespace nesting closed above so subsequent code in
  // this TU (if any) stays in GameAdapters::Mech. There currently is
  // no further code; this comment documents the structure.
  ```

  File structure (per adversarial MINOR-3: option B is THE choice; option A documented for reference only -- do not implement it): locate the original `} // namespace Mech` and `} // namespace GameAdapters` closing braces, place the new `findMechByHandle` impl (Task 3) BEFORE them, then place the self-test impl at FILE SCOPE AFTER the closing namespace braces. Forward-decl in `RenderWorld.cpp` is a single `void RunMechPickSelfTest();` with no namespace declaration; matches the existing `RunGameplayPickSelfTest`/`RunMechObjectIdSelfTest` forward-decl style at `RenderWorld.cpp:40-45`.

  Option A (namespaced) is documented in the code block above only because the impl block opens inside `namespace GameAdapters::Mech` for `findMechByHandle`; do NOT keep the self-test inside that namespace. The closing `} // namespace Mech` and `} // namespace GameAdapters` must precede the `void RunMechPickSelfTest() { ... }` definition exactly as shown.

  Plan-stage executor MUST verify the existing pattern with:

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern '^void Run' -SimpleMatch
  ```

  Then mirror exactly.

- [ ] **Step 2b: Add `RenderWorld::getMechsAliveCount()` public accessor (external-review m2 prerequisite).**

  `s_mechs_alive_rw` is a file-scope `std::atomic<uint64_t>` in
  `RenderWorld/RenderWorld.cpp:105` with no public accessor. The
  self-test lives in `GameAdapters/MechRenderAdapter.cpp` (different TU)
  and so cannot read the file-scope static directly. Add a thin
  accessor.

  Verify the storage shape at write time:

  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp -Pattern 's_mechs_alive_rw' -SimpleMatch
  ```

  Must show the `static std::atomic<uint64_t> s_mechs_alive_rw{0};`
  definition (verified at plan-write time: `RenderWorld.cpp:105`).

  **Header decl** -- insert near the other RenderWorld free-function
  decls in `RenderWorld/RenderWorld.h` (locate `IsObjectIdBufferEnabled`
  or `objectIdRawForStaticPropRecipe` as anchor):

  ```cpp
  // M2.6: read-only accessor for the engine-side live-mech counter
  // (sourced from MechRenderAdapter via registerMech/destroyMech). Used
  // by RunMechPickSelfTest in GameAdapters/MechRenderAdapter.cpp to
  // assert no drift across a synthetic register+destroy pair. Returns
  // a relaxed-load uint64_t snapshot; no synchronization across
  // multiple consumers.
  uint64_t getMechsAliveCount();
  ```

  **Impl** -- insert in `RenderWorld/RenderWorld.cpp` near the file-scope
  static definition or near `registerMech`/`destroyMech`:

  ```cpp
  uint64_t getMechsAliveCount() {
      return s_mechs_alive_rw.load(std::memory_order_relaxed);
  }
  ```

  Add both files to Task 6 `git add` in Step 6. Update commit message
  to mention the new accessor.

- [ ] **Step 3: Wire RunMechPickSelfTest into RenderWorld::init.**

  **Existing (`RenderWorld/RenderWorld.cpp:492-496`):**
  ```cpp
      RunGameplayPickSelfTest();
      // M2.5 (Q1): mech-substrate self-test (gated by
      // MC2_MECH_OBJECT_ID_SELFTEST=1). Validates registerMech / destroyMech
      // / record-table generation + kind plumbing. Synthetic; no GL state.
      RunMechObjectIdSelfTest();
  ```

  **Replace with:**
  ```cpp
      RunGameplayPickSelfTest();
      // M2.5 (Q1): mech-substrate self-test (gated by
      // MC2_MECH_OBJECT_ID_SELFTEST=1). Validates registerMech / destroyMech
      // / record-table generation + kind plumbing. Synthetic; no GL state.
      RunMechObjectIdSelfTest();
      // M2.6: mech-pick self-test (gated by MC2_MECH_PICK_SELFTEST=1).
      // Validates findMechByHandle scan well-formed-ness; hosted in
      // GameAdapters/MechRenderAdapter.cpp because it reaches into
      // game-side BattleMech (RenderWorld must not include code/mech.h).
      RunMechPickSelfTest();
  ```

- [ ] **Step 4: Build (full relink: new symbol on adapter TU + new call site).**

  ```powershell
  Remove-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
  & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64 --config RelWithDebInfo --target mc2
  ```

  Pass criterion (link-direction sanity, external-review M2): build completes
  with NO unresolved-external error on `RunMechPickSelfTest`. The forward-decl
  in `RenderWorld.cpp` creates a RenderWorld static lib -> GameAdapters static
  lib link dependency for the self-test symbol. If `cmake --build` reports
  `unresolved external symbol "void __cdecl RunMechPickSelfTest(void)"` (or
  the linker dies with LNK2019 on that symbol), the static-lib link order
  disagrees with the call direction.

  **Fallback (do NOT include mech.h or any game header in RenderWorld.cpp
  to "fix" the unresolved external -- that violates the firewall):** move
  the self-test CALL out of `RenderWorld::init()` into
  `GameAdapters::Mech::beginMission()` (already lives in
  `GameAdapters/MechRenderAdapter.cpp` -- same TU as the definition; no
  forward-decl needed; no link-direction concern). Steps:

  1. Revert Step 3 of this task (remove the `RunMechPickSelfTest();` call
     and the forward-decl from `RenderWorld.cpp`).
  2. Add the call to `GameAdapters::Mech::beginMission()` in the same TU
     where the definition lives. Locate with:
     ```powershell
     Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\GameAdapters\MechRenderAdapter.cpp -Pattern 'void beginMission' -SimpleMatch
     ```
     Insert `::RunMechPickSelfTest();` after any existing init body in
     `beginMission` (or at the top of the function body if no body exists yet).
  3. Re-build; the symbol now resolves within a single TU.
  4. Smoke gate: the self-test still fires once per mission (beginMission
     is called per mission load, equivalent to init-time observability for
     the synthetic registerMech+destroyMech check). Expected log line
     `[MECH_PICK_SELFTEST v1] result=PASS step=all` still appears, just
     during per-mission init instead of process init.

  Document the fallback as taken (or NOT taken) in the Task 6 commit message
  so future maintainers know which placement is live.

- [ ] **Step 5: Deploy + env-ON smoke with selftest gate.**

  ```powershell
  Copy-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  fc /B A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
  $env:MC2_OBJECT_ID_BUFFER='1'
  $env:MC2_MECH_PICK_SELFTEST='1'
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  Remove-Item Env:\MC2_OBJECT_ID_BUFFER
  Remove-Item Env:\MC2_MECH_PICK_SELFTEST
  ```

  Pass criterion: tier1 5/5 PASS. Latest artifact contains `[MECH_PICK_SELFTEST v1] result=PASS step=all` on each mission's log.

- [ ] **Step 6: Commit.**

  ```powershell
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp GameAdapters/MechRenderAdapter.cpp
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m @'
  m2.6: add RunMechPickSelfTest end-to-end validator

  Gated on MC2_MECH_PICK_SELFTEST=1 AND substrate enabled. Hosted in
  GameAdapters/MechRenderAdapter.cpp (file scope) because the test
  reaches into game-side BattleMech / Mech3DAppearance via
  findMechByHandle -- RenderWorld must not include code/mech.h.
  Forward-declared from RenderWorld.cpp matching the existing
  RunGameplayPickSelfTest / RunMechObjectIdSelfTest pattern.

  Validates:
   1. registerMech yields a fresh handle.
   2. findMechByHandle(invalid) -> nullptr.
   3. findMechByHandle(synthetic) -> nullptr at init-time (well-formed
      scan over an empty ObjectManager). Per MINOR-clarif: validates
      the SCAN itself, not init-time mech data state.
   4. destroyMech retires the handle.
   5. live-mech count returned to baseline (external-review m2: catches
      register/destroy imbalance via the new getMechsAliveCount accessor).

  New public accessor RenderWorld::getMechsAliveCount() exposes the
  file-scope s_mechs_alive_rw atomic for cross-TU consumption by the
  self-test (the test cannot read the file-scope static directly).

  Emits [MECH_PICK_SELFTEST v1] result=PASS|FAIL|SKIP. Wired into
  RenderWorld::init after RunMechObjectIdSelfTest (or relocated to
  GameAdapters::Mech::beginMission if the link-direction fallback fired
  per Step 4 of this task; note which placement landed). Tier1 5/5 PASS
  env-ON MC2_OBJECT_ID_BUFFER=1 MC2_MECH_PICK_SELFTEST=1 with
  PASS on every mission log.

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  '@
  ```

---

## Task 7: Validation gates + CLAUDE.md M2.6 SHIPPED

**Files:** `.claude/worktrees/nifty-mendeleev/CLAUDE.md`

Run every numbered validation gate from spec Section 10, then write the M2.6 SHIPPED entry + update the M1.6 entry log-schema reference + add the three new env vars to the instrumentation section.

- [ ] **Step 1: Gate 1 -- env-OFF tier1 5/5.**

  ```powershell
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  ```

  Pass: exit 0; zero `[GAMEPLAY_PICK v1]` lines in logs.

- [ ] **Step 2: Gate 2 -- env-ON substrate-only.**

  ```powershell
  $env:MC2_OBJECT_ID_BUFFER='1'
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  Remove-Item Env:\MC2_OBJECT_ID_BUFFER
  ```

  Pass: exit 0; banner emits `mech_enabled=0`; no `[GAMEPLAY_PICK v1] hit kind=Mech`.

- [ ] **Step 3: Gate 3 -- env-ON mech wiring.**

  ```powershell
  $env:MC2_OBJECT_ID_BUFFER='1'
  $env:MC2_MECH_PICK='1'
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  Remove-Item Env:\MC2_OBJECT_ID_BUFFER
  Remove-Item Env:\MC2_MECH_PICK
  ```

  Pass: exit 0; banner emits `mech_enabled=1`.

- [ ] **Step 4: Gate 4 -- selftest PASS.**

  Already covered by Task 6 Step 5. Re-run if commits have landed since.

- [ ] **Step 5: Gate 5 -- env-ON mech + pierce-fog.**

  ```powershell
  $env:MC2_OBJECT_ID_BUFFER='1'
  $env:MC2_MECH_PICK='1'
  $env:MC2_MECH_PICK_PIERCE_FOG='1'
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  Remove-Item Env:\MC2_OBJECT_ID_BUFFER
  Remove-Item Env:\MC2_MECH_PICK
  Remove-Item Env:\MC2_MECH_PICK_PIERCE_FOG
  ```

  Pass: exit 0; banner emits `mech_pierce_fog=1`.

- [ ] **Step 6: Gate 6 substitutive proof (META-FIX load-bearing).**

  Re-run the grep set from Task 5 Step 7. ALL must return zero matches (excluding `.claude/worktrees/*/docs/superpowers/` planning artifacts).

  Per adversarial MINOR-5: exclusion narrowed to `docs/superpowers/` + `CLAUDE.md` only (everything else under `.claude/` is policed).

  ```powershell
  Get-ChildItem -Recurse -Include *.h,*.cpp,*.md A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev | Select-String -Pattern 'setLastStaticPropPick|StaticPropSelectionDebugState|\[STATIC_PROP_PICK v1\]|clearLastStaticPropPick|getLastStaticPropPick' | Where-Object { $_.Path -notmatch '\\docs\\superpowers\\' -and $_.Path -notmatch '\\CLAUDE\.md$' }
  ```

  Pass: empty result. If non-empty, return to Task 5 Step 3 / Step 4 / Step 5 and fix.

- [ ] **Step 7: Gate 7 -- new schema appearance check.**

  ```powershell
  Get-ChildItem -Recurse -Include *.h,*.cpp A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld,A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\code,A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\GameAdapters | Select-String -Pattern '\[GAMEPLAY_PICK v1\]'
  ```

  Pass: matches found at the new emit sites: banner (RenderWorld.cpp), static-prop hit/miss (missiongui.cpp), mech hit/miss/gated/stale-handle (missiongui.cpp).

- [ ] **Step 8: Gate 9 -- log byte-shape grep (post env-ON smoke).**

  Locate latest smoke artifact dir and grep:
  ```powershell
  $latest = Get-ChildItem A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  Get-ChildItem $latest.FullName -Filter '*.ring_trace.log' | ForEach-Object {
      Select-String -Path $_.FullName -Pattern '\[GAMEPLAY_PICK v1\] (hit|miss|gated)'
  }
  ```

  Pass: any `hit kind=(StaticProp|Mech)` line found matches the field set documented in spec Section 11. (Tier1 has no clicks; lines may be absent. The smoke gate is the env-ON banner appearance only; the user-driven canary at Gate 8 is for the human.)

- [ ] **Step 9: Update CLAUDE.md.**

  Three discrete edits to `.claude/worktrees/nifty-mendeleev/CLAUDE.md`:

  **Edit 9a:** add the three new env vars to the `Tier-1 instrumentation env vars` section (under the line listing `MC2_RENDER_CONTRACT_ASSERT`).

  Locate:
  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\CLAUDE.md -Pattern 'MC2_RENDER_CONTRACT_ASSERT' -SimpleMatch
  ```

  Insert three bullet lines AFTER the `MC2_RENDER_CONTRACT_ASSERT` bullet:

  ```
  - `MC2_MECH_PICK=1` -- M2.6 mech-pick wiring master enable. Requires `MC2_OBJECT_ID_BUFFER=1` substrate. Default OFF.
  - `MC2_MECH_PICK_DEBUG=1` -- M2.6 verbose miss/gated/stale-handle diagnostic logs from `tryMechPick`. Default OFF.
  - `MC2_MECH_PICK_PIERCE_FOG=1` -- M2.6 dev/debug override: allow inspect through sensor fog. Default OFF preserves stock gameplay.
  ```

  **Edit 9b:** update the M1.6 entry log-schema reference. Locate:
  ```powershell
  Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\CLAUDE.md -Pattern 'RenderWorld Slice M1.6' -SimpleMatch
  ```

  Within that entry, change every literal `[STATIC_PROP_PICK v1]` to `[GAMEPLAY_PICK v1] kind=StaticProp` and append a "(renamed to [GAMEPLAY_PICK v1] kind=StaticProp in M2.6)" note next to the schema description so future archaeologists find the rename.

  **Edit 9c:** add the M2.6 SHIPPED entry to the `Active campaigns` section. Insert AFTER the M2.5 entry and BEFORE the section-end. Template:

  ```
  - **RenderWorld Slice M2.6** (SHIPPED 2026-05-23): mech pickup integration (inspect-only v1) -- closes the RenderWorld arc. Shift+LMB on a visible hostile mech now emits `[GAMEPLAY_PICK v1] hit kind=Mech ...` via a new `MissionInterfaceManager::tryMechPick` caller dispatching through the unchanged M2-pre `tryGameplayPick` spine. Handle->BattleMech reverse-resolution via new `GameAdapters::Mech::findMechByHandle` linear scan (Option B alone per adversarial CRITICAL-1: `partId` is NOT lifetime-stable, reassigned post-`syncSpawn` at `code/mission.cpp:2987`; `code/mech.cpp:1338` stays byte-identical to M2 ship). Fog-of-war predicate mirrors the FULL CPU-pick gate at `code/missiongui.cpp:1272-1278` including ShowMovers + MPlayer-defeat carve-outs (per MAJOR-1). Three new env gates: `MC2_MECH_PICK`, `MC2_MECH_PICK_DEBUG`, `MC2_MECH_PICK_PIERCE_FOG`. META-FIX: `LookupResult.kind` added as substrate; M1.6 wrapper kind-guards on `r.lookup.kind == StaticProp` (fixes the latent post-M2.5 mislabel bug -- mech handle had been silently stamped as `recipe=-1` static-prop pick); `StaticPropSelectionDebugState` renamed to `GameplaySelectionDebugState` with `RenderObjectKind kind` discriminator; `[STATIC_PROP_PICK v1]` schema retired in favor of unified `[GAMEPLAY_PICK v1] kind=...`. Gate 6 substitutive proof: zero matches across `RenderWorld/` + `code/` + `GameAdapters/` + header doc comments + `tests/` + `scripts/` for the retired symbols/schema (excluding dated planning artifacts under `.claude/worktrees/*/docs/superpowers/`). New `[MECH_PICK_SELFTEST v1] result=PASS step=all` self-test gated on `MC2_MECH_PICK_SELFTEST=1`; validates `findMechByHandle` scan well-formed-ness at init-time (per MINOR-clarif: not init-time data state). Hosted in `GameAdapters/MechRenderAdapter.cpp` (firewall: RenderWorld must not include `code/mech.h`). Tier1 5/5 PASS env-OFF AND env-ON for substrate-only / mech-only / mech+pierce-fog combinations. Greybeard ruling 2026-05-23: META-FIX (Q3 retire-now-not-later: deferring to M3/M4 would multiply the parallel schemas from 1 to 3-4 with linearly higher retirement cost). Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-6-mech-pickup-spec.md`. Plan: `docs/superpowers/plans/2026-05-23-renderworld-slice-m2-6-mech-pickup-plan.md`.
  ```

- [ ] **Step 10: Commit docs.**

  ```powershell
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev add .claude/worktrees/nifty-mendeleev/CLAUDE.md
  git -C A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev commit -m @'
  m2.6 docs: add SHIPPED entry + M1.6 schema-rename pointer + env vars

  Three discrete CLAUDE.md updates:
   - Active campaigns: add M2.6 SHIPPED entry (per Section 9c template).
   - Active campaigns: M1.6 entry now references the renamed log schema
     [GAMEPLAY_PICK v1] kind=StaticProp so anyone grepping the old name
     after M2.6 ships gets a pointer to the rename instead of zero
     matches (defends against the T6 code-archaeology hazard).
   - Tier-1 instrumentation env vars: add MC2_MECH_PICK,
     MC2_MECH_PICK_DEBUG, MC2_MECH_PICK_PIERCE_FOG (per adversarial
     MINOR -- CLAUDE.md env-vars).

  All M2.6 SHIPPED gates have passed: tier1 5/5 env-OFF + env-ON
  (substrate-only / mech-only / mech+pierce-fog); selftest PASS;
  Gate 6 substitutive proof zero-match; Gate 7 new-schema appearance
  found at all expected emit sites.

  User-driven canary (Gate 8 in spec Section 10) is the final
  post-ship verification step for the human; gestures + expected
  log lines documented in spec Section 10.

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  '@
  ```

- [ ] **Step 11: Verify pointer-CLAUDE.md sanity check.**

  ```powershell
  bash A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\check-claude-md-pointer.sh
  ```

  Pass: exit 0. (M2.6 only touches the worktree CLAUDE.md, not the root pointer.)

---

## Self-review checklist

Run BEFORE handing the plan off:

### Spec coverage

| Spec section | Addressed by |
|---|---|
| 4.1 LookupResult.kind | Task 1 |
| 4.2 findMechByHandle | Task 3 |
| 4.3 GameplaySelectionDebugState (rename + setters) | Task 5 |
| 4.4 RenderWorld::destroy lifecycle rename | Task 5 Step 2 (`:506`) |
| 5 M1.6 wrapper kind-guard + doc comments + boot banner | Task 2 (guard) + Task 5 Step 2a (banner; moved from Task 4 per MAJOR-A) + Task 5 (all rename hits incl. cpp:93/484/599/605 + h:96-99/97 + cpp:31/32/6175 + gameplay_pick.h:45 + missiongui.h:269 + cpp:99 storage + cpp:506 lifecycle + cpp:97-98 MAJOR-B) |
| 5 CLAUDE.md M1.6 entry + env-vars section | Task 7 Step 9 |
| 6.1 NO change to syncSpawn / mech.cpp:1338 | Confirmed in "Plan-stage blocker resolutions" + Task 3 commentary |
| 6.2 NO cookie identifier choice | Confirmed in Task 3 commit message |
| 6.3 tryMechPick consumer body (full fog predicate) | Task 4 Step 6 (verbatim from spec) |
| 6.4 tail call additions (both styles) | Task 4 Step 7 |
| 6.5 3 new env vars | Task 4 Steps 1-2 (decls + impls) |
| 7 caller pattern (two callers, two spine calls) | Task 4 Step 6 + Step 7 (parallel callers) |
| 9 Gating five-gate stack + naming asymmetry | Task 4 Steps 1-3 (decls + banner) |
| 10 Gates 1-9 | Task 7 Steps 1-8 |
| 11 Log schema | Task 5 Step 2a (banner; moved from Task 4 per MAJOR-A) + Task 4 Step 6 (mech hit/miss/gated/stale) + Task 5 Step 3 (StaticProp branch rename) |
| 12 Firewall (no SCOPE_DIRS change, no allowlist edit) | Task 3 Step 5 (firewall script run) |
| 13 Greybeard META-FIX ruling | Carried in commit messages + Gate 6 substitutive proof |
| 14 Threat model T1-T9 | T1 stale-handle: Task 4 Step 6 stale-handle branch; T2 latent bug: Task 2; T3 MLR fallback: no code change required (spec confirms); T4 coord-space: caller does no coord math (spec confirms); T6 archaeology: Task 7 Step 9 Edit 9b; T9 docs drift: Task 7 Step 9 Edits 9a + 9b |

### Placeholder scan

```powershell
Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\docs\superpowers\plans\2026-05-23-renderworld-slice-m2-6-mech-pickup-plan.md -Pattern 'TODO|TBD|XXX|FIXME|PLACEHOLDER|\.\.\.'
```

Carries-acceptable hits: the literal three dots `...` may appear inside spec quotation blocks. Reject any actual TODO/TBD/XXX/FIXME tokens.

### Type consistency

- `RenderObjectKind` enum referenced consistently as `RenderWorld::RenderObjectKind::Mech` / `::StaticProp` in `code/missiongui.cpp` callers (qualified) and `RenderObjectKind::Mech` / `::StaticProp` inside `namespace RenderWorld` (unqualified). Plan checked.
- `BattleMech*` resolved via `static_cast<BattleMech*>(MoverPtr m)` only after `m->isMech()` (matches engine idiom). Plan checked.
- `Mech3DAppearance*` resolved via `static_cast<Mech3DAppearance*>(bm->getAppearance())` matches the engine idiom used in M2 adapter and M2.5 batcher. Plan checked.

### Citation drift coverage

- Spec-cited lines (`RenderWorld.h:157-167`, `:185-194`, `:201`, `:208`, `:213`, `:96`, `:103`, `:108`, `:116-120`, `:146`; `RenderWorld.cpp:484`, `:488-496`, `:506`, `:599`, `:605`, `:717-726`, `:729-754`, `:756-759`, `:761-764`; `code/missiongui.cpp:1267`, `:1272-1278`, `:1539`, `:1782`, `:6186-6271`; `code/missiongui.h:272`; `code/gameplay_pick.h:78`; `GameAdapters/MechRenderAdapter.h:52`; `GameAdapters/MechRenderAdapter.cpp:3-7, 10-21, 87-120`; `code/mech.cpp:1338`; `code/mech.h:340`; `code/mover.h:730, 951, 1264`; `code/objmgr.h:450, 501`; `code/gameobj.h:462-464`; `mclib/mech3d.h:478, 487-489`) -- all spot-verified at plan-write time.

- Drift hits found and folded into Task 5: `code/gameplay_pick.h:45` doc comment, `code/missiongui.h:269` doc comment, `code/missiongui.cpp:31-32/6175` doc comments, `RenderWorld/RenderWorld.cpp:99` storage var, `:506` lifecycle clear. Documented in "Citation drift fixes" section at top.

---

PLAN STATUS: READY FOR EXECUTE -- external-review fixes applied (C1+M1+M2+m1+m2+m3)

Commit-message-ready summary (5 lines):

```
m2.6: close RenderWorld arc -- mech pickup inspect-only + META-FIX
Substrate adds LookupResult.kind; M1.6 wrapper kind-guards (retires latent
post-M2.5 mislabel); new tryMechPick caller mirrors tryStaticPropPick
shape, dispatches through unchanged tryGameplayPick spine, resolves via
GameAdapters::Mech::findMechByHandle linear scan (Option B alone per
CRITICAL-1 -- partId not lifetime-stable); full fog predicate from
missiongui.cpp:1272-1278 (ShowMovers + MPlayer-defeat bypasses);
StaticPropSelectionDebugState + [STATIC_PROP_PICK v1] retired
substitutively in favor of GameplaySelectionDebugState +
[GAMEPLAY_PICK v1] kind=...; three env gates added; new
RunMechPickSelfTest validates scan well-formed-ness at init.
```
