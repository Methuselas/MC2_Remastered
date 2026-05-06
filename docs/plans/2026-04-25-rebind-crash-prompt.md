# Next-session prompt: rebind-to-occupied-key crash

Paste everything below this header into a fresh session.

---

## Task

A v0.1.x playtester reports: in Options → Hotkeys, attempting to rebind a
command to a key that's **already used by another command** crashes the
game. This bug existed in v0.1 and survived v0.1.2. Find the crash and fix it.

## What I already know (do not re-derive)

- Suspect file: `code/optionsarea.cpp` — class `OptionsHotKeys`, methods
  `update()` and `end()`.
- Two relevant code paths in `update()`:
  - **Lines 1043-1099** — the YES-confirm branch of the conflict dialog,
    which performs the "swap" (gives the conflicting item the original
    item's key).
  - **Lines 1110-1158** — the keyboard-poll loop that detects the conflict,
    sets `bShowDlg = true`, and shows `LogisticsOKDialog` with
    `IDS_OPTIONS_HOTKEY_ERROR`.
- `MissionInterfaceManager::OldKeys[MAX_COMMAND]` is a static array of
  size 107 (`code/missiongui.h:60,111`). It's lazy-initialized in
  `loadHotKeys()` at `code/missiongui.cpp:5294-5302` (only when
  `OldKeys[0] == -1`).
- `HotKeyListItem::setCommand(i)` is only ever called with
  `i < MAX_COMMAND` (`optionsarea.cpp:1299`), so the
  `defaultKeys[pTmpItem->getCommand()]` read at line 1075 is bounds-safe
  on its face.
- Static analysis turned up no obvious NULL deref or OOB read in the
  YES-branch swap logic. The crash is **probably not** in
  `OptionsHotKeys::update()` itself — likely deeper in
  `LogisticsOKDialog` lifecycle, `HotKeyListItem` internals, or a corner
  case of `gos_GetKey()` / `makeInputKeyString()`.

## What to do

1. **Repro first.** Build + deploy (use `/mc2-build-deploy`), launch the
   game, go to Options → Hotkeys, pick any command, press a key already
   bound to something else. Click YES on the conflict dialog. Capture
   the crash.

2. **Get a stack trace.** Options in priority order:
   - Run under MSVC debugger (`mc2.exe` is built RelWithDebInfo, has PDBs).
   - Check for a crash bundle written by `gos_crashbundle.h` infrastructure
     (commits `c995ffc`, `2e10d55` added this — search for `crashbundle_append`
     output paths).
   - Enable any existing instrumentation env vars
     (`MC2_HEARTBEAT=1`, etc.) per the worktree CLAUDE.md "Tier-1
     Instrumentation" section.

3. **Once you have the stack**, fix the root cause. Do not paper over
   with a try/catch or a NULL guard added in optionsarea.cpp — that just
   hides whatever invariant is being violated. Find what's actually
   stale or freed and fix the lifecycle.

4. **Verify**: rebind to occupied key → swap → click YES → no crash;
   then rebind in the OTHER direction (the displaced binding) and
   confirm both keys do what they say. Then click NO on the dialog and
   confirm the original binding is preserved.

## Constraints

- **Do not** modify rendering, shadow, or terrain code paths.
- **Do not** change `MissionInterfaceManager::OldKeys[]` shape or sizing.
- Per worktree CLAUDE.md: build is `--config RelWithDebInfo`; deploy
  via `cp -f` per file with `diff -q` verification (use `/mc2-deploy`),
  not `cp -r`.
- Land any debug instrumentation env-gated (`MC2_HOTKEY_TRACE=1` style)
  per the "Debug Instrumentation Rule" section of worktree CLAUDE.md;
  demote-don't-delete after the fix.

## Out of scope

- The other v0.1.x bug reports from 2026-04-25 (cursor jumping near HUD,
  paint scheme colors, AAR text overflow at 1440p, encyclopedia 3D
  preview, thin line on right edge of HUD).
- AI/ABL passive-enemies bug — already fixed in `mc2-hotfix-v0.1.2.zip`
  (release_assets/).

## Done = ready to roll into v0.1.3

Crash gone, both swap directions verified, instrumentation left in
gated, commit on `claude/nifty-mendeleev`.
