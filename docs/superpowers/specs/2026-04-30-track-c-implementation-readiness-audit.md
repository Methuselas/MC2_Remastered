# Track C — Implementation Readiness Audit

> **⚠️ STATUS UPDATE (2026-04-30, post-publication):** This audit assumes **Lua-primary scripting**. That assumption was superseded later the same day by [`2026-04-30-techscript-primary-lua-sidecar-design.md`](2026-04-30-techscript-primary-lua-sidecar-design.md), which adopts Methuselas's TechScript proposal ([Discussion #18](https://github.com/ThranduilsRing/mc2-opengl-remastered/discussions/18)) as the primary scripting architecture and demotes Lua to a deferred sidecar capability ("add as needed" per user direction). **This audit remains valid as the implementation-readiness reference for the Lua sidecar IF/WHEN it lands**, but it does NOT apply to TechScript implementation. Read the techscript-primary spec FIRST; treat this audit as Lua-side reference. Do not begin C-1 without confirming with Methuselas that Lua sidecar is being activated rather than deferred.

**Date:** 2026-04-30
**Mode:** Cross-cutting readiness audit. No code or implementation changes; this is the master pre-coding document for Track C M0 (Sol2 + Lua wiring).
**Predecessors (the design corpus):**
- `specs/2026-04-29-modders-paradise-roadmap-design.md` — roadmap §3, §6 Track C, §5.5, §8.2, §8.3.
- `explorations/2026-04-29-track-c-lua-scripting-status.md` — greenfield baseline + ABL surface map.
- `explorations/2026-04-30-track-c-lua-implementation-shape.md` — VM class shape (note: §5 ABI superseded; lifecycle §4 still valid).
- `explorations/2026-04-30-track-c-lua-api-surface-catalog.md` — STABLE/EXPERIMENTAL/INTERNAL tier list.
- `explorations/2026-04-30-track-c-lua-sandbox-and-errors.md` — stdlib whitelist, instruction/memory caps.
- `explorations/2026-04-30-track-c-lua-loading-lifecycle.md` — Kahn's BFS, `mc2.persist`, hot-reload.
- `explorations/2026-04-30-track-c-lua-trampolines-and-tests.md` — perf budget §4, trace §5, test checklist §6, doc-gen §7 (note: §1–§2 ABI superseded).
- `explorations/2026-04-30-track-c-blocking-questions-resolution.md` — **AUTHORITATIVE: `*_impl` extraction pattern, namespace lock, magic-FSM gating.**
- `explorations/2026-04-30-track-c-build-integration-deep-dive.md` — final CMake / vendoring shape.
- `explorations/2026-04-30-track-c-modder-tooling-deep-dive.md` — LuaLS stubs, `tools/new-mod`.
- `explorations/2026-04-30-track-c-modder-dx-deep-dive.md` — REPL, profiler hooks, debug overlays.
- `explorations/2026-04-30-track-c-abl-to-lua-reverse-direction.md` — `mc2luadispatch_*` primitives.
- `explorations/2026-04-30-track-c-openra-factorio-spring-borrowing.md` — borrow / adapt / reject table.
- `explorations/2026-04-30-track-c-mod-boundaries-deep-dive.md` — 12 NO-list categories.
- `explorations/2026-04-30-track-c-mod-test-harness-deep-dive.md` — `--test-mod`, snapshot, `--no-render`.
- `explorations/2026-04-30-track-c-battletech-mw5-mod-scene-research.md` — Track F (AI) gap.

**Post-audit additions (2026-04-30, after this audit was first written):**
- `explorations/2026-04-30-track-c-modifier-registry-decision.md` — **D4 answered: hybrid v1.** Single primitive serves quirks, abilities, weapon tags, pilot personalities, chassis affinities, faction passives. Reframes Track F §4 and §5 as modifier-registry consumers.
- `explorations/2026-04-30-stock-mission-compatibility-plan.md` — 60-cell test matrix; bundled `mods/_compat_test/`; per-layer perf budgets; `destroys-delta = 0` as the load-bearing gameplay-equivalence assertion.
- `explorations/2026-04-30-cross-track-perf-budget-audit.md` — All-up CPU/memory/disk budget. Tracks C+E+F at 5 active mods consume ~10–13% of the post-M2 freed 24 ms/frame. Comfortable headroom.
- `specs/2026-04-30-pre-flight-and-risk-map.md` — Pre-flight checklist + 23-row risk map + consolidated NOT-in-v1 list. Read this BEFORE C-1 starts.
- `specs/2026-04-30-battletech-modder-conventions-design.md` — Stub artifact for the RogueTech-veteran collaborator (art-side authoritative; systems-side research-seeded for validation).
- `specs/2026-04-30-track-f-ai-replacement-design.md` — Track F foundational inventory (3-options analysis; 0.44 Hz tick rate finding).
- `specs/2026-04-30-track-f-scalable-hierarchical-ai-design.md` — Track F architecture (4-layer hierarchy, modifier-registry-consuming pilot/chassis systems).

**Small correction:** `brain->execute()` is at `code/warrior.cpp:2160`, not `:2155` (cited at one place in this doc and in pre-flight). Cosmetic; fix on next audit revision.

---

## Executive summary

Track C M0 is **implementation-ready** for the C-1 (vendoring) and C-2 (LuaVM skeleton) commits. The two design questions that historically blocked C-3 — the ABL stack reentrancy hazard and the namespace lock-in — are resolved in `blocking-questions-resolution.md` (the `*_impl` extraction pattern + locked `mc2.<sub>.<verb>` tree). Build integration is fully specified down to per-source-file warning suppression. Sandbox stdlib whitelist, instruction caps, and per-mod memory caps are locked. The 16 components below break out as:

- **Locked / code-ready (8):** vendoring + build, `*_impl` extraction, LuaVM class + sandbox, VM lifecycle wiring, mod loader (Kahn's BFS), stage mechanics (single VM with view-table swap), STABLE bindings (~85 via `*_impl`), reverse direction (`mc2luadispatch_*` 6 typed primitives).
- **Locked design / requires source-code investigation before code lands (4):** `mc2.persist` save/load (savefile chunk insertion site), hot-reload (`ReadDirectoryChangesW` integration with engine main loop), ActionRegistry + DataSourceRegistry (Track B coupling), modder DX (REPL/profiler require Track B ImGui or stdio fallback).
- **Architecturally sketched, design pass needed before implementation (3):** modder tooling (`tools/lua_api_doc_gen` Python — schema OK, generator unwritten), boundaries enforcement (per-mod budgets — accounting code unwritten), test harness (`--test-mod` CLI — flag set defined, runner missing).
- **Open gap (1):** Track F (AI replacement). Out of Track C scope. The BattleTech research surfaces a real, sized hole — RogueTech / BTA / MercTech-class mods replace AI behavior wholesale, and the Lua dispatch hooks of Track C give *triggers*, not *replacement*. See "Track F gap analysis" §below.

The headline: **a session can begin C-1 tomorrow with zero design questions** in the vendoring / VM / namespace / `*_impl` slice. C-2 through C-5 follow with at most 1–2 small clarifications per commit (most flagged in the per-component "pitfalls" column). The first ~10-file change recipe is in §"Day-1-of-C-1 recipe."

---

## Per-component readiness table

Status legend: **LOCKED** (design final, ready to implement) / **PROPOSED** (design complete, one open Q) / **CONTESTED** (two designs, decision pending) / **TBD** (architectural sketch only) / **GAP** (intentionally out of Track C M0).

| # | Component | Status | Locked-by (single source of truth) | Code-vs-design split | Leakage risks | Pitfalls |
|---|-----------|--------|-------------------------------------|----------------------|---------------|----------|
| a | **Vendoring + build** (Sol2, Lua 5.4, lua_static, CMake) | LOCKED | `build-integration-deep-dive.md` (§2 option b, §10 fragments, §12 sequence) | Code-ready: vendor 32 of 35 Lua TUs (skip `lua.c`, `luac.c`, `onelua.c`); `add_library(lua_static STATIC ...)`; Sol2 header-only via `THIRDPARTY_INCLUDE_DIRS`; `add_subdirectory("./modding")`; per-target `/wd4334 /wd4146 /wd4244 /wd4267 /wd4310 /wd4324`. | None — Lua/Sol2 link static into `mc2.exe`, no DLL surface, no symbol export. | Sol2 includes only in `modding/lua_vm.cpp` + `modding/lua_bindings_mc2.cpp` (forward-decl `namespace sol { class state; }` in `lua_vm.h`); otherwise +5–10s per engine TU. `LINUX_BUILD` global is harmless to Lua per §9. |
| b | **`*_impl` extraction in `code/ablmc2.cpp`** | LOCKED | `blocking-questions-resolution.md` §Q1 (authoritative) | Code-ready for the M0 ten: `mc2_get_time_impl`, `mc2_object_status_impl`, `mc2_object_exists_impl`, `mc2_set_timer_impl`, `mc2_check_timer_impl`, `mc2_set_objective_status_impl`, `mc2_check_objective_status_impl`, `mc2_play_sound_impl`, `mc2_set_global_impl`, `mc2_get_global_impl`. Each is ~30 LoC pure refactor: pop ABL args at top of original `execXxx`, body becomes a call to `*_impl`, push return at bottom. **Net behavior change to ABL: zero.** Header at `modding/lua_abl_shim.h` (`extern "C"` block). | None at runtime — `*_impl` is a normal C function, no globals. The danger is **build-time**: extracting a body that secretly mutates `CurWarrior` / `CurFSM` and breaks ABL. Mitigation: per-binding regression smoke (call from ABL test mission, compare result). | Some `execXxx` close over `CurFSM` / `CurWarrior` globals (`code/ablmc2.cpp` >5000 LoC of these patterns). The first non-trivial impl extraction must verify those globals are read-only inside the body, OR pass them as `*_impl` parameters. The 10 M0 picks were chosen for triviality — none touch warrior globals — but verify before each promotion. |
| c | **`LuaVM` class + sandbox** (sol::state, stage gates, instr caps, mem caps, file I/O shim) | LOCKED | `lua-implementation-shape.md` §2 (header), `lua-sandbox-and-errors.md` §1 (stdlib whitelist + caps) | Code-ready: pimpl shape, `Init()`/`Shutdown()`, `LoadDataStage`/`LoadControlStage`, `Tick(dt)`, `CallEvent`, `DispatchAction`. Stdlib: `base, math, string, table, utf8, coroutine`. Strip `dofile/load/loadfile/loadstring/setmetatable/getmetatable/rawget/rawset/collectgarbage`. `io`/`os`/`package`/`debug` not opened; replace with `mc2.io.read` shim, re-export `os.time/clock/date/difftime` only, `debug.traceback` only. `SOL_ALL_SAFETIES_ON=1`, `SOL_PRINT_ERRORS=0`. | Bytecode-via-`load` (mitigation: reject any chunk whose first byte is `0x1B` before `luaL_loadbuffer`). Symlink/junction escape on Windows for `mc2.io.read` (mitigation: realpath + prefix check; reject NUL and drive letters). | Sol2's `lib::debug` is all-or-nothing — opening it loads everything we don't want. The fix (sandbox doc §1): expose only `debug.traceback` as a plain function in the sandbox env, never call `state.open_libraries(sol::lib::debug)`. Custom `require` resolver must not match `..` traversal nor absolute paths, even when normalized through `/` separators (recall: `LINUX_BUILD` makes engine path globals use `/`). |
| d | **VM lifecycle wiring** (3 init sites, 2 teardown sites) | LOCKED | `lua-implementation-shape.md` §4 (file:line citations verified) | Code-ready, 5 single-line additions:<br>- `code/mission.cpp:1705` — `mc2lua::initLuaVM();` after `initABL();`<br>- `code/mission.cpp:3336` — `mc2lua::closeLuaVM();` BEFORE `closeABL();`<br>- `code/missionbegin.cpp:122` — `mc2lua::initLuaVM();` after `initABL();`<br>- `code/missionbegin.cpp:450` — `mc2lua::closeLuaVM();` BEFORE `closeABL();`<br>- `code/saveload.cpp:704` — `mc2lua::initLuaVM();` after `initABL();` (no co-located teardown — rides `Mission::~Mission`).<br>**Tear order: Lua before ABL** at every site — final pcalls during teardown may reference ABL state. | If `Tick()` runs during ABL execution (cull / brain mid-tick), event drains can fire Lua handlers that call into `*_impl` which is fine, but anything that re-enters `ABLi_execute` corrupts the global stack (see §Q1 reentrancy analysis). Mitigation: `g_ablInTick` flag set/cleared by ABL execute frame; forward bindings check it. | Per-frame `Tick()` site choice. Recommended: end of `Mission::update()`, AFTER `objMgr->update()` returns and BEFORE render submit. This guarantees no ABL frame is in flight when Lua callbacks fire (deferred-event drain happens here). Picking a different site silently breaks the deferral guarantee. |
| e | **Mod loader** (Kahn's BFS, manifest, `?`/`!` operators, profile-launcher integration) | LOCKED | `lua-loading-lifecycle.md` §1 (Kahn's), §7 (profile-launcher coupling) | Code-ready: `modding/mod_loader.cpp` parses `mods/*/mod.json` via vendored `nlohmann/json` (already in mod-profile-launcher worktree). `depends` is `{modid: constraint}` map; constraint grammar `>=`/`<=`/`=`/`<`/`>` with conjunction; optional via `?` prefix on key. Topo sort uses sortedByModId tiebreak for determinism. Cycle detector returns the offending path. Profile filter via `profile_manager::getActiveModList()` (returns `optional<set<string>>`; `nullopt` = scan all). | Manifest field that is a path string (e.g. `"scripts": {"data": "scripts/data.lua"}`) — a malicious mod could put `..` in the value. Mitigation: same path validator as the file I/O shim (§c). | Borrowing-doc finding: add `!` (incompatibility) operator NOW even if no mod uses it yet. Cheap to add, expensive to retrofit semantics for after mods exist (`borrowing.md` §Q4 open Q). Defer `~` (no-load-order edge). |
| f | **Stage mechanics** (data → freeze → control via destroy-and-recreate per Factorio borrowing) | LOCKED (with one borrow-doc note) | `lua-loading-lifecycle.md` §2–§3, `lua-implementation-shape.md` §7 | Code-ready: single `sol::state`. View-table swap exposes data-stage bindings during `Stage::Data`, switches to control-stage view at `Stage::Control`. Prototype tables are frozen via `__newindex` raise + `__metatable = "locked"` to prevent unfreeze trick. `Stage::DataFrozen` is observable in instrumentation. | A mod's data-stage script keeps a `local handle = mc2.spawn_object` reference before stage transition — at control stage, that handle is to a binding that errors when the stage gate disagrees. Mitigation: per-binding gate check is defense-in-depth (lifecycle §2.3); the view swap is the primary. | **Borrowing-doc note:** Factorio actually destroys and recreates the VM between stages. This audit follows lifecycle §2 (single-VM with view swap + freeze) — simpler, no userdata reification needed. The implementer should NOT misread "destroy-and-recreate per Factorio borrowing" as license to actually destroy the VM. The freeze approach is the chosen adaptation; see `borrowing.md` Q2 "What we adapt" — single-VM is the locked decision. |
| g | **`mc2.persist` save/load** (versioned LUA1 JSON chunk, mod-fatal-tolerant) | PROPOSED | `lua-loading-lifecycle.md` §4 | Architectural sketch ready: serializer walks `mc2.persist`, emits deterministic JSON (sorted keys, skip functions/threads/userdata with warning), writes versioned `LUA1 + uint32 + bytes` chunk to savegame stream. Backward compat: missing chunk → `mc2.persist = {}`. **Requires source investigation:** the savegame stream is in `code/saveload.cpp` — exact insertion site for the LUA1 chunk needs a read of the existing serialization helpers and a new `MissionData::saveLuaState()` companion to the existing per-subsystem savers. | Save bloat — a misbehaving mod could write a large persist table; mitigation: per-mod persist size cap (M1; not M0). Mod-set drift on load (save written with `[A,B,C]`, loaded with `[A,B,D]`) — `B`'s persist survives, `C`'s silently drops, `D` starts empty. Lifecycle §9.2 says "warn-only in M0; refuse-by-default with `--force` is M1 hardening." | The `LUA1` magic + versioning is correct; the open question is whether mod-set drift should fail-closed by default. Currently warn-only. Decide before saves with mod content ship. |
| h | **Hot-reload** (file watch via ReadDirectoryChangesW/inotify, F5/inspector/REPL triggers, end-of-frame drain) | PROPOSED | `lua-loading-lifecycle.md` §5, `modder-dx-deep-dive.md` §3-ish | Architectural: `LuaVM::reloadFromDisk(path)` discriminates `data.lua` (dev-only, blocked unless `MC2_LUA_DEV=1`) vs `control.lua` (safe path: `clearHandlersFor(mod) → LoadControlStage`). `control.lua` reload is idempotent if mods write `state = state or {}` at top. **Requires source investigation:** Windows file watching needs `ReadDirectoryChangesW` integration with the engine's main loop — investigate where the existing TODO infrastructure for live shader reload lives (root CLAUDE.md notes "Shader hot-reload fails silently"); reuse that pattern if possible. | Mid-mission references into prototypes (cached HP from earlier `mc2_loadFromPrototypes`) keep the OLD values until next mission load — documented as known caveat (lifecycle §5), not a bug. Hot-reload while events queued: drop the deferred queue at hot-reload (matches "drop user functions" rule per blocking-questions §Open 4). | Module reload graph: a `require`'d sub-module like `mods/X/scripts/util.lua` should trigger `control.lua` re-run for X. Lifecycle §9.6 leaves this open. Pick "any change under `mods/X/` re-runs X's control.lua" for M0; refine later. |
| i | **STABLE-tier bindings** (~85 trampolines via `*_impl`; ABL stack never touched from Lua) | LOCKED | `blocking-questions-resolution.md` Q2 (locked namespace), `api-surface-catalog.md` §3 | Code-ready for the M0 ten (per §b above). The full ~85 STABLE list is enumerated in `blocking-questions-resolution.md` Q2 namespace tree. Each binding is a Sol2 lambda that calls the matching `*_impl` directly: `mc2.object.status` → `mc2_object_status_impl`. Defense-in-depth: stage check at top of each lambda. | Per `mod-boundaries-deep-dive.md` §1.9 — every forward binding checks `g_ablInTick`. If true and the binding would re-enter ABL, error with `[LUA v1] event=abl_reentry_reject ...`. M0 ten do not re-enter; this guard is for future bindings. | Variadic ABL signatures (`*` and `?` type letters) are deferred — no M0 STABLE binding uses them per `blocking-questions.md` §Open 1. Promotion to STABLE for any variadic binding requires shape decision (`std::variant`, ABLStackItem passthrough, or Lua-side type-tagged table). |
| j | **`mc2luadispatch_*` reverse-direction primitives** (6 typed primitives + auto-disable on N failures) | LOCKED | `abl-to-lua-reverse-direction.md` §1, §3, §10 | Code-ready: 6 ABL primitives registered in `code/ablmc2.cpp` near line 6693+: `mc2luadispatch` (key only), `mc2luadispatch_i`, `mc2luadispatch_ii`, `mc2luadispatch_r`, `mc2luadispatch_ir`, `mc2luadispatch_c`. Each is ~5 LoC pop-call-push. `LuaVM::Dispatch(key, args...)` short-circuits on first non-zero return, swallows pcall errors. Built-in events (engine-side Option B): `warrior.HpThresholdCrossed`, `mission.ObjectiveComplete`, `mech.Destroyed`, `team.Eliminated`. M0 ships 3–5 such Option B sites; **Option A (corebrain.abx patching) deferred to M1 per magic-ABL contamination rule.** | Reentry: handler runs *during* ABL execution (the calling primitive is mid-frame). `in_abl_dispatch_` flag set; forward bindings flagged `requires_no_dispatch` error if called during dispatch. M0 ten are all read-side — none flagged; M1 promotions need annotation. | `mclib/ablsymt.h` `MAX_STANDARD_FUNCTIONS` — this worktree should already have it bumped to 512 from carver5 stability session (`memory/carver5_mission_playable.md`); 6 new primitives won't push the limit, but **verify `nifty-mendeleev` HEAD has the bump** before C-3 lands. |
| k | **ActionRegistry + DataSourceRegistry** (string-dispatch, RegisterCpp + RegisterLua from day 1) | PROPOSED | `lua-implementation-shape.md` §8, roadmap §5.5 (Track B) | Architectural — Track C contributes only the Lua side (`mc2.register_action(key, fn)` and `LuaVM::DispatchAction(key)`); the C++ `ActionRegistry::RegisterLua` API is owned by Track B. **Requires source investigation:** Track B has not landed; the C++ registry shape in lifecycle §8 is a sketch. Coordinate with Track B owner before C-5 to confirm signatures. | None at runtime if Track B hasn't landed; the registration is a no-op when `ActionRegistry` is null. | Layering: implementation-shape §8 stores opaque `g_LuaVM` as the handle in `RegisterLua` because there is exactly one VM per mission. If multi-VM ever ships (out of M0 scope), this breaks. Document the assumption in the registration API. |
| l | **Modder tooling** (`tools/lua_api_doc_gen/` Python, BindingRegistry → JSON → mc2-api.lua + lua-api.md, `tools/new-mod` scaffolder) | PROPOSED | `modder-tooling-deep-dive.md` §1–§7 | Architectural: `BindingRegistry` is a C++ singleton populated at registration time (each binding emits a `BindingSpec{name, sig, kind, doc}`). `mc2.exe --dump-lua-api` walks the registry and emits JSON to stdout. Python `tools/lua_api_doc_gen/main.py` reads that JSON and emits LuaLS stubs (`tools/lua-meta/library/mc2-api.lua` with `---@meta` header) + Markdown (`docs/modding/lua-api.md`). `tools/new-mod` is a Python scaffolder that drops `mod.json` + `scripts/data.lua` + `scripts/control.lua` + `meta/.luarc.json` per the deep-dive's templates. | Doc drift if generator skipped — modders see autocomplete that doesn't match runtime. Mitigation: regenerate stubs in CI on every binding-touching commit; diff vs committed stubs and fail PR if mismatch. | Schema versioning: the JSON dump format itself needs a version field so future stubs can target older engine builds. Currently unspecified. Add `"schema_version": 1` to the dump root. |
| m | **Modder DX** (LuaConsole REPL, mc2.profiler, debug overlays, mod-manager UI, state inspector) | PROPOSED | `modder-dx-deep-dive.md` §1–§5 | Architectural: REPL is ImGui-backed (gated on Track B bridge) with stdio fallback (worker thread reading stdin) for pre-Track-B builds. Auth gate: `MC2_LUA_REPL=1` || `--enable-repl` || `s_devModeFlag`. `mc2.profiler.start/stop/dump_csv` instrument per-binding call counts + total time; modders read CSV externally. State inspector and mod-manager UI are Track B/E concerns; Track C contributes only the data hooks. **Requires source investigation:** Track B ImGui bridge land status (it has not at HEAD). | REPL allows arbitrary code in dev builds — auth gate must be three-pronged (env, CLI, dev flag) and OFF by default. Otherwise a player who got an `MC2_LUA_REPL` env var from somewhere could exfil. M0: ship with `MC2_LUA_REPL` UNDOCUMENTED for shipping users; only modder docs reference it. | Stdio fallback REPL is two pages of code; **ship it day one** so REPL is not blocked on Track B. |
| n | **Boundaries** (12 NO-list categories, experimental opt-in, per-mod budgets, sandbox escape tests) | LOCKED (NO list) / TBD (per-mod budgets) | `mod-boundaries-deep-dive.md` §1 (NO list), §2 (experimental opt-in), §3 (budgets) | Code-ready (NO list): the bindings simply aren't in the namespace tree, so most categories enforce themselves by absence. Networking, threading, FFI, filesystem-outside-mod, OS shell, cross-mod state, save-format manipulation, ABL reentry, debug introspection, sandbox bypass — none have any code path. Experimental opt-in: `mod.json` field `experimental_features: ["raw_metatables", ...]`; engine logs `event=experimental_opt_in`. **TBD:** per-mod resource accounting (instruction count, memory) needs accounting code unwritten. Boundaries §3 sketches "VM-wide cap with per-mod attribution via `lua_setallocf` shim" but the shim is unwritten. | The bytecode-injection attack via `string.dump` then `load` — explicitly closed by stripping `load`/`loadstring`/`loadfile` AND rejecting any chunk starting with `0x1B`. Verify both belt-and-braces. | Per-mod budgets: M0 ships VM-wide cap only; per-mod attribution is M1. Document this in modder-facing docs so authors don't expect "my mod gets its own 64MB" when it's actually shared. |
| o | **Test harness** (`mc2.exe --test-mod`, `mc2.test` API, snapshot, `--no-render`, `mod-smoke` tier in run_smoke.py) | PROPOSED | `mod-test-harness-deep-dive.md` §1 (CLI), §2-§7 | Architectural: `--test-mod=NAME --mission=STEM --duration=10s --no-render --seed=...`. `[MODTEST v1]` log schema. Snapshot mode emits structured JSON of mod-observable events; diff against `expected.json`. `--no-render` skips GL submit / shader bind. `mod-smoke` tier is a new entry in `scripts/run_smoke.py` that boots one stock mission per mod with a 10s `--duration`. **Requires source investigation:** `--no-render` needs an early-return at the gpfx submit layer; tracing whether the existing `--menu-canary` path can be the precedent. | Snapshot non-determinism — mod test output must be stable across runs. `--seed=` covers RNG; the harness must also pin animation frame timing (use `--duration=ticks:600`, not wall-time). | Existing `run_smoke.py` matrix is per-mission; mod-smoke tier multiplies by per-mod (every mod × every mission would explode). Pick representative pairings: `mod-smoke = {(mod, default_mission)}` only. |
| p | **Track F (AI replacement) — cross-cutting BattleTech finding** | GAP (out of Track C scope) | `battletech-mw5-mod-scene-research.md` (Track F bullet), `blocking-questions.md` Q3 (magic* not exposed in v1) | Architectural sketch only. magicpatrol/magicguard/magicescort are no-op stubs in stock content; not exposed in v1. magicAttack/coreGuard/corePatrol/coreWait are `#if 0` — the four corebrain shadow names that contaminated Magic v0.2. Lua dispatch hooks (Option B engine events from `j` above) give *triggers*, not *replacement*. RogueTech-class AI mods replace behavior wholesale via direct C++ DLL. **No path to that in M0 or M1.** | A mod that calls `mc2.ai.magic_patrol(...)` on stock content gets a no-op trace + a one-time `[LUA WARN] mc2.ai.magic_patrol called in stock profile — Omnitech FSM primitives have no effect`. The pre-commit hook `git grep -E '^void execMagicAttack|^void execCoreGuard|^void execCorePatrol|^void execCoreWait' code/ablmc2.cpp` must FAIL the commit if those names re-enter (per `blocking-questions.md` §Open 6). | The gap is real and sized (see "Track F gap analysis" §below). Acknowledge it in modder docs: "Lua mods can hook AI events; they cannot replace AI behavior in v1." |

---

## Dependency graph

```
                       ┌──────────────────────────┐
                       │ a. Vendoring + build     │  C-1
                       │  (CMake, lua_static,     │
                       │   sol/, modding/)        │
                       └────────────┬─────────────┘
                                    │
                                    ▼
        ┌───────────────────────────────────────────────┐
        │ c. LuaVM class + sandbox    │ b. *_impl      │  C-2 + C-3 (parallel)
        │   (sol::state, stdlib       │   extraction   │
        │    whitelist, instr/mem     │   (M0 ten in   │
        │    caps, FS shim)           │   ablmc2.cpp)  │
        └────────────┬────────────────┴────────┬───────┘
                     │                         │
                     ▼                         ▼
        ┌────────────────────────┐  ┌──────────────────────────┐
        │ d. VM lifecycle wiring │  │ i. STABLE bindings       │  C-3 + C-4
        │   (5 +1 lines in       │  │   (Sol2 lambdas calling  │
        │    mission/missionbeg/ │  │    *_impl; namespace     │
        │    saveload)           │  │    locked)               │
        └─────────┬──────────────┘  └─────────┬────────────────┘
                  │                           │
                  ▼                           ▼
        ┌─────────────────────┐   ┌───────────────────────────┐
        │ f. Stage mechanics  │   │ j. mc2luadispatch_*       │  C-3 + C-4
        │   (single VM,       │   │   (6 ABL primitives,      │
        │    view swap,       │   │    in_abl_dispatch flag)  │
        │    freeze)          │   └─────────┬─────────────────┘
        └────────┬────────────┘             │
                 ▼                          │
        ┌─────────────────┐                 │
        │ e. Mod loader   │  ◄──────────────┘
        │   (Kahn, manifest,
        │    profile filter)
        └────────┬────────┘
                 ▼
        ┌──────────────────┐    ┌────────────────────────┐
        │ g. mc2.persist   │    │ h. Hot-reload          │  C-5+ (post-M0)
        │   (LUA1 chunk)   │    │   (file watch, drain)  │
        └──────────────────┘    └────────────────────────┘

Parallel non-blocking (informs design but not blocked by code above):
  k. ActionRegistry coupling (Track B owner-coordinated)
  l. tools/lua_api_doc_gen + tools/new-mod (Python; depends on BindingRegistry from C-3)
  m. Modder DX — REPL stdio fallback ships day-one; ImGui REPL gated on Track B
  n. Boundaries enforcement — NO list is automatic (absence); per-mod budgets are M1
  o. Test harness — depends on at least one shipped binding (so post C-3)
  p. Track F (AI) — out of Track C scope
```

**Critical path:** a → c → d/i → e → f. C-1 unblocks C-2 (LuaVM); C-2 + b unblock C-3 (bindings); C-3 + d unblock C-4 (lifecycle wiring); C-5 demos.

---

## Day-1-of-C-1 recipe

The literal first ten file changes. Each step is independently reviewable; after step 10 the build is green and a hello-world Lua chunk loads.

1. **`3rdparty/lua/`** — drop the unmodified Lua 5.4.7 distribution (32 `.c` + 5 `.h`) plus `LICENSE`, plus `README` with `Lua 5.4.7, dropped 2026-04-30, sha256 <hash>` provenance. Skip `lua.c` / `luac.c` / `onelua.c` (they have `main()`).

2. **`3rdparty/sol/`** — drop the Sol2 single-file amalgamation `sol.hpp` + `forward.hpp` (generated upstream via `single/single.py`; do not vendor the script). Plus `LICENSE.txt` with the Sol2 commit hash in a leading comment.

3. **`THIRD_PARTY_LICENSES.md`** — append two lines referencing Lua MIT and Sol2 MIT (`memory/public_fork_and_release.md` already accommodates third-party MIT).

4. **`CMakeLists.txt`** — insert after line 154 (Tracy include) the §10 fragment from `build-integration-deep-dive.md`:
   - `file(GLOB LUA_SOURCES ...)`; `list(REMOVE_ITEM ... lua.c luac.c onelua.c)`
   - `add_library(lua_static STATIC ${LUA_SOURCES})`
   - `target_compile_definitions(lua_static PUBLIC LUA_COMPAT_5_3)`
   - MSVC warning suppression block: `/wd4334 /wd4146 /wd4244 /wd4267 /wd4310 /wd4324`
   - Append Sol2 + Lua include dirs to `THIRDPARTY_INCLUDE_DIRS`.
   - After line 160: `add_subdirectory("./modding" "./out/modding")`.
   - On the `target_link_libraries(mc2 ...)` line ~272: add `modding` near the front.

5. **`modding/CMakeLists.txt`** (NEW) — declare `add_library(modding STATIC lua_vm.cpp lua_bindings_mc2.cpp)`. Include `${COMMON_INCLUDE_DIRS}`, `${THIRDPARTY_INCLUDE_DIRS}`, `${CMAKE_SOURCE_DIR}/code`, `${CMAKE_SOURCE_DIR}/mclib`. `target_link_libraries(modding PUBLIC lua_static)`. `target_compile_definitions(modding PRIVATE SOL_ALL_SAFETIES_ON=1 SOL_PRINT_ERRORS=0)`.

6. **`modding/lua_vm.h`** (NEW) — the full header from `lua-implementation-shape.md` §2. Forward-declare `namespace sol { class state; }`. Pimpl `Impl* pimpl_`. Public methods: `Init/Shutdown/LoadDataStage/LoadControlStage/Tick/CallEvent/DispatchAction`. Free functions `initLuaVM/closeLuaVM`. Extern `g_LuaVM`.

7. **`modding/lua_vm.cpp`** (NEW) — skeleton from `implementation-shape.md` §3. Empty bodies with `// TODO:` placeholders, but `Init()` opens the whitelisted libs (`base, math, string, table, utf8, coroutine`), strips `dofile/load/loadfile/loadstring/setmetatable/getmetatable/rawget/rawset/collectgarbage`, and creates the `mc2` named table. `printf("[LUA v1] event=init status=ok\n")` fires from `initLuaVM()`.

8. **`modding/lua_bindings_mc2.cpp`** (NEW) — empty TU initially. C-3 fills it; for C-1 it's just a `void mc2lua_register_bindings_placeholder(void) {}` to give the lib at least one symbol.

9. **`modding/lua_abl_shim.h`** (NEW) — empty `extern "C"` header. C-3 fills it with the M0 ten declarations; for C-1 it's just the include guards.

10. **Smoke verification** — clean build per build-integration §10:
    - `cmake --build build64 --config RelWithDebInfo --target lua_static`
    - `cmake --build build64 --config RelWithDebInfo --target modding`
    - `cmake --build build64 --config RelWithDebInfo --target mc2`
    - Confirm `lua_static.lib`, `modding.lib`, `mc2.exe` exist.
    - `dumpbin /DEPENDENTS build64/RelWithDebInfo/mc2.exe | grep -i lua` — expect empty.
    - Run tier-1 smoke: `py -3 scripts/run_smoke.py --tier tier1 --kill-existing --with-menu-canary`. Should pass identically to pre-C-1 (no engine path touched). C-1 commits land; nothing in mc2.exe behavior changes — the `modding` lib has only one stub function and is never called.

After step 10: C-1 lands. C-2 wires `g_LuaVM = new LuaVM()` and gives it to no one yet (still no bindings). C-3 lands the M0 ten `*_impl` extractions in `code/ablmc2.cpp` and the matching Sol2 lambdas in `lua_bindings_mc2.cpp`. C-4 adds the 5 lifecycle one-liners. C-5 drops `mods/test/mod.json` + `scripts/{data,control}.lua` and runs an end-to-end smoke.

---

## Free wins consolidated (~20 items)

Drawn from `borrowing.md` and the BattleTech research, these are concrete patterns we can adopt verbatim or adapt cheaply.

1. **Kahn's BFS with sortedByModId tiebreak** for mod load order. Verbatim from Factorio. (`borrowing.md` Q4; `lifecycle.md` §1.)
2. **`?` (optional) + `!` (incompat) prefix operators** in `depends`. Adopt `?` from day 1, `!` before any "replaces stock" mod ships. Defer `~`. (`borrowing.md` Q4 open Q.)
3. **`data:extend{}` registration shape**, validation pass (type+name required), first-write-wins with logged collision. Verbatim from Factorio `dataloader.lua`. (`borrowing.md` Q2; `lifecycle.md` §3.)
4. **DFS with immutable-visited-set cycle detection** for prototype `Inherits:` chain (when override policy resolves toward inheritance). Verbatim from OpenRA `MiniYaml.cs`. (`borrowing.md` Q1.)
5. **`__metatable = "locked"` on frozen prototype tables** to prevent `setmetatable(P, nil)` unfreeze trick. Standard Lua hardening. (`lifecycle.md` §3.)
6. **Strip the debug lib down to just `traceback`** by NOT opening `sol::lib::debug` and exposing `debug.traceback` as a plain function in the sandbox env. Spring/BAR pattern. (`borrowing.md` Q3; `sandbox-and-errors.md` §1.)
7. **Custom file-IO shim, don't open Lua's `io`** — single `mc2.io.read(path_relative_to_mod)` is enough for M0. Spring/BAR pattern minus archive-mounting. (`borrowing.md` Q3.)
8. **Reject any chunk whose first byte is `0x1B`** before `luaL_loadbuffer` to close the bytecode-via-`load` channel. (`mod-boundaries.md` §1.3; `sandbox-and-errors.md`.)
9. **Many-handlers-per-event + pcall-each + auto-disable on N consecutive failures** for `mc2.on_event`. Adopt Spring's widget pattern. Threshold: 3 errors in 60 frames for `brain_tick`; never for one-shots. (`borrowing.md` Q5; `abl-to-lua-reverse-direction.md` §11 open Q.)
10. **Frame-end-queue dispatch via `Mission::update` end-of-frame slot.** Adopt OpenRA's `World.AddFrameEndTask` pattern. Resolves the ABL reentrancy hazard by guaranteeing Lua handlers fire only when no ABL frame is in flight. (`borrowing.md` Q5; `blocking-questions.md` Q1 option (a).)
11. **`@meta` LuaLS stub format with `---@class`/`---@alias`/`---@enum`** — copy the OpenRA `[ScriptGlobal]` mental model into a Sol2-friendly emit pipeline. (`modder-tooling-deep-dive.md` §1–§2.)
12. **`${3rd}/mc2/library` LuaLS third-party-library convention** — modders type `mc2.` and LuaLS prompts to enable the library, identical to Love2D / OpenResty UX. (`modder-tooling-deep-dive.md` §1.)
13. **Per-mod `meta/.luarc.json` + workspace `library` covering all loaded mods' `meta/`** so cross-mod autocomplete works without engine intervention. (`modder-tooling-deep-dive.md` §1.)
14. **Backtick (`` ` ``) hotkey for `LuaConsole`** — Quake/Source/Factorio muscle memory. Stdio fallback in a worker thread for pre-Track-B builds. (`modder-dx-deep-dive.md` §1.)
15. **`mc2.persist[mod_id]` namespaced JSON-serializable table** — Factorio's `global` table re-skinned. Mods MUST tolerate empty persist on load. (`lifecycle.md` §4; `borrowing.md` Q2.)
16. **`script.on_event(event_id, handler)` mental model** for engine-emitted hooks but with many-handlers semantics from Spring. (`borrowing.md` Q5; `abl-to-lua-reverse-direction.md` §4.)
17. **Object-shape `depends: {modid: constraint}`** rather than Factorio's array-of-prefixed-strings — easier to parse with vendored nlohmann/json. (`borrowing.md` Q4 "What we adapt"; `lifecycle.md` §1.)
18. **Versioned `LUA1` savefile chunk** — variants on Factorio's mod-data save format, sized by uint32 length, JSON inside. Backward compat: missing chunk → empty persist. (`lifecycle.md` §4.)
19. **`tools/new-mod` scaffolder** that drops `mod.json` + `scripts/data.lua` + `scripts/control.lua` + `meta/.luarc.json` from templates. Strong DX win for first-mod onboarding. (`modder-tooling-deep-dive.md` §6.)
20. **YAML-style mod compatibility pack pattern** — when two mods conflict, a third "compat" mod patches one to play nice with the other. Adopt the convention; modders know it from MW5 (`mw5_mod_compatibility_pack`) and HBS BT. (`battletech-mw5-mod-scene-research.md` §1.)
21. **Reject sym-link / junction escape on Windows** for `mc2.io.read` paths via realpath + prefix check. Standard practice; cheap to add. (`mod-boundaries.md` §1.5; `sandbox.md` §8 open Q 6.)
22. **Pre-commit hook `git grep -E '^void execMagicAttack|^void execCoreGuard|^void execCorePatrol|^void execCoreWait' code/ablmc2.cpp` failing the commit** to prevent re-enabling the four corebrain shadow names. (`blocking-questions.md` §Open 6.)

---

## Open questions consolidated (severity-tagged)

### Blocking (resolve before C-3 commit)

- **None.** All Q1/Q2/Q3 from `blocking-questions-resolution.md` are closed. The `*_impl` extraction pattern, the locked namespace tree, and the magic-FSM gating policy are settled.

### Important (resolve before C-5 / first content commit)

- **`*_impl` extraction with stateful globals** (component b pitfall). The 10 M0 picks were chosen for triviality; verify each body is truly side-effect-free against `CurWarrior` / `CurFSM` before extraction. If any is not, pass the global as a `*_impl` parameter rather than reaching for it.
- **`Tick()` site choice in `Mission::update`** (component d pitfall). Recommended end-of-update + before render-submit. Picking a different site silently breaks the ABL-reentrancy deferral guarantee.
- **`MAX_STANDARD_FUNCTIONS` bump in `mclib/ablsymt.h`** (component j pitfall). Verify `nifty-mendeleev` HEAD has the carver5 bump (256 → 512). Otherwise 6 new ABL primitives push the limit.
- **`!` (incompat) operator in mod loader manifest** (component e pitfall). Cheap to add now, expensive after mods exist.
- **Profile-launcher coupling shape** (component k). The `ActionRegistry::RegisterLua` API is owned by Track B. Coordinate with Track B owner before C-5.

### Nice-to-have (resolve at M1)

- **Prototype override policy** (lifecycle §9.1). First-write-wins or layered-with-load-order or explicit `replace()` API? Wait until the first override-needing mod ships.
- **Mod-set drift on save load** (lifecycle §9.2). Currently warn-only; refuse-by-default with `--force` is M1 hardening.
- **Per-mod resource budgets** (component n). M0 ships VM-wide cap only; per-mod attribution via `lua_setallocf` shim is M1.
- **Module reload graph** (component h). "Any change under `mods/X/` re-runs X's control.lua" for M0; finer control later.
- **Variadic ABL signatures** (component i). No M0 STABLE binding uses `*` or `?`. Decide shape (`std::variant`, ABLStackItem, or Lua-table) when first promotion needs it.
- **Auto-disable threshold for `mc2.on_event` handlers** (free win 9). 3-in-60 default for M1.
- **Async / queued `mc2.events.emit`** (`abl-to-lua-reverse-direction.md` §11 open Q 2). M0 says synchronous; M1 maybe.
- **Coroutine instruction-cap interaction** (boundaries §2 `coroutine_unbounded`). Experimental opt-in flag; not M0.
- **String interning for dispatch keys** (`abl-to-lua-reverse-direction.md` §11 open Q 1). Profile first; ~100ns lookup is fine.
- **Cross-mod priority / ordering control** for stacked event handlers (`abl-to-lua-reverse-direction.md` §11 open Q 8). Stable registration order works until two mods fight; then add `priority` arg.

### Future (post-M1; explicitly out of Track C scope)

- **Track F (AI replacement).** RogueTech-class mods replace AI behavior wholesale via DLL. Lua dispatch hooks of Track C give triggers, not replacement. See gap analysis below.
- **Multiplayer determinism** (lifecycle §9.5). `pairs()` non-determinism, RNG sharing, lockstep dispatch order. Defer until MP is on roadmap.
- **Save format migration tooling** (g pitfall). Versioned LUA1 chunks support migration but the tooling is unwritten.
- **Option A corebrain.abx patching** (component j). Engine-side Option B sites cover M0; Option A gated on a stable corebrain `.abl` source build pipeline.
- **C extension loading** (`build-integration.md` §11 risk 7). Mods shipping `.dll` is a category-error vs. sandbox guarantees; would require flipping Lua to DLL build. Never in M0/M1.
- **`mc2.net.fetch` allowlist** (boundaries §1.1 escape hatch). v2+ candidate; player-consent gated.

---

## Track F (AI replacement) gap analysis

The BattleTech research surfaces a real gap that Track C does NOT close.

**What we know.** The dominant BattleTech-IP mods (RogueTech, BTA3062, MercTech, YAML, vonBiomes) replace AI behavior wholesale, not just augment it. ModTek (HBS BT) ships a JSON-merge layer plus a C# DLL hook surface that lets mods inject `IModInit`-style classes that intercept `BehaviorTreeFactory.GetBehavior` and substitute mod-supplied tree nodes. RogueTech ships `BattleTechPerformanceFix` plus thousands of behavior-tree nodes implemented in mod DLLs. MercTech replaces the entire `MoveDecisionTreeNode` hierarchy.

**What Track C provides.** Triggers and reads, not replacement.
- Lua mods can subscribe to engine-emitted events (`warrior.HpThresholdCrossed`, `mech.Destroyed`) and react with side effects (heal a mech, spawn an effect, log an achievement).
- Lua mods can call read-side bindings to query AI state (`mc2.object.status`, `mc2.object.position`).
- Lua mods CANNOT substitute `corebrain.abx`'s decision logic. The closest path is Option A — patch stock `corebrain.abx` at well-known decision points to emit `mc2luadispatch_*` calls, and have Lua handlers short-circuit by returning non-zero. This still leaves the surrounding ABL bytecode intact; the Lua mod is a *guard* on the existing tree, not a replacement.

**What we don't know.** The full enumeration of corebrain decision points worth exposing. The ABL source for stock corebrain is not in the worktree (only the compiled `.abx`). To enumerate hooks, we'd need either:
1. The Microsoft-era `.abl` source recovered from somewhere (unknown availability), OR
2. A decompiler pass over `corebrain.abx` to identify decision branches.

**What scoping Track F would entail.**
- **Track F.1 — corebrain `.abl` source recovery.** Locate or decompile. Out of Track C; arguably a separate exploration.
- **Track F.2 — decision-point enumeration.** Walk the recovered source. Identify "behavior cliff" branches (engage/disengage, target selection, retreat threshold, group cohesion). For each, define the Option A patch site and the dispatch-key contract.
- **Track F.3 — corebrain patch build pipeline.** A reproducible build of patched `corebrain.abx` from the source + a versioned diff against stock. Feeds into the magic-ABL contamination rule (`memory/magic_abl_contamination_rule.md`) — the patched corebrain rides only with the mod-loader package, never the stock-only release branch.
- **Track F.4 — behavior-tree replacement substrate.** If decision-point hooks aren't enough, the next step is letting Lua mods replace whole sub-trees. This requires a proper behavior-tree formal model in the engine; today there is none (corebrain is hand-written ABL bytecode). This is the "BT/MW5 mod-scene parity" target and is a multi-month engineering effort.
- **Track F.5 — DLL hook surface.** RogueTech-class. Out of Track C entirely; would require ABI stability commitments we cannot make in v0.x.

**Recommendation.** Acknowledge the gap in modder-facing docs. Land Track C M0 with the dispatch-trigger model. Schedule Track F.1–F.3 as a separate exploration after C-5 ships.

---

## Single source of truth registry

When two docs disagree about the same component, the column below wins. Older docs are kept for historical context but should not be cited as authoritative.

| Component | Authoritative doc | Doc(s) explicitly superseded |
|-----------|-------------------|------------------------------|
| ABL → Lua calling convention (`*_impl`) | `blocking-questions-resolution.md` §Q1 | `lua-implementation-shape.md` §5; `lua-trampolines-and-tests.md` §1–§2 |
| Namespace tree (`mc2.<sub>.<verb>`) | `blocking-questions-resolution.md` §Q2 | `lua-api-surface-catalog.md` §3 (still useful for binding count + ABL signatures, but the names + tree structure are locked by Q2) |
| magicpatrol / magicguard / magicescort exposure | `blocking-questions-resolution.md` §Q3 | `lua-api-surface-catalog.md` §11 open Q1 |
| CMake target shape (`lua_static` STATIC) | `build-integration-deep-dive.md` §2 option (b) | `lua-implementation-shape.md` §1 (general layout still valid; the target shape is locked by build-integration §2) |
| Stage mechanics (single VM with view-table swap) | `lua-loading-lifecycle.md` §2; `lua-implementation-shape.md` §7 | (no contradiction; the borrowing-doc Q2 "Factorio destroys the VM" is informational, not a recommendation — see component f pitfall) |
| Sandbox stdlib whitelist | `lua-sandbox-and-errors.md` §1 | `lua-implementation-shape.md` §6 (sketch superseded by spec) |
| Reverse-direction (ABL → Lua) primitives | `abl-to-lua-reverse-direction.md` §1, §3 | (no predecessor; this is the canonical doc) |
| `mc2.persist` save/load | `lua-loading-lifecycle.md` §4 | (no predecessor) |
| Hot-reload | `lua-loading-lifecycle.md` §5; `modder-dx-deep-dive.md` §3 | (complementary, not contradictory) |
| Mod load order (Kahn's BFS) | `lua-loading-lifecycle.md` §1 | (no predecessor) |
| 12-category NO list | `mod-boundaries-deep-dive.md` §1 | `lua-sandbox-and-errors.md` §1 (sandbox doc is whitelist; boundaries doc is blacklist; both are authoritative for their lens) |
| Test harness CLI | `mod-test-harness-deep-dive.md` §1 | (no predecessor) |
| Modder editor experience (LuaLS, stubs) | `modder-tooling-deep-dive.md` §1–§7 | (no predecessor) |
| Modder DX in-game (REPL, profiler, overlays) | `modder-dx-deep-dive.md` §1–§5 | (no predecessor) |
| BattleTech mod-scene context (Track F gap) | `battletech-mw5-mod-scene-research.md` | (no predecessor) |
| Build / CI integration | `build-integration-deep-dive.md` | `lua-implementation-shape.md` §1 (older sketch; build-integration is the spec) |

---

## References

Read-only design corpus (this document is a synthesis; primary sources above):

- **Spec:** `specs/2026-04-29-modders-paradise-roadmap-design.md` — §3 reference stack, §6 Track C, §5.4 ABI, §5.5 ActionRegistry, §8.2 two-stage, §8.3 sandboxing.
- **Authoritative resolutions:** `explorations/2026-04-30-track-c-blocking-questions-resolution.md` (Q1 `*_impl`, Q2 namespace, Q3 magic).
- **Build:** `explorations/2026-04-30-track-c-build-integration-deep-dive.md`.
- **VM + sandbox:** `explorations/2026-04-30-track-c-lua-implementation-shape.md` (§4 lifecycle still valid), `explorations/2026-04-30-track-c-lua-sandbox-and-errors.md`.
- **Lifecycle + loading:** `explorations/2026-04-30-track-c-lua-loading-lifecycle.md`.
- **Bindings + tests:** `explorations/2026-04-30-track-c-lua-trampolines-and-tests.md` (§4–§7 valid), `explorations/2026-04-30-track-c-lua-api-surface-catalog.md`.
- **Reverse direction:** `explorations/2026-04-30-track-c-abl-to-lua-reverse-direction.md`.
- **Borrowing:** `explorations/2026-04-30-track-c-openra-factorio-spring-borrowing.md`.
- **Boundaries:** `explorations/2026-04-30-track-c-mod-boundaries-deep-dive.md`.
- **Tooling:** `explorations/2026-04-30-track-c-modder-tooling-deep-dive.md`, `explorations/2026-04-30-track-c-modder-dx-deep-dive.md`.
- **Test harness:** `explorations/2026-04-30-track-c-mod-test-harness-deep-dive.md`.
- **Field research:** `explorations/2026-04-30-track-c-battletech-mw5-mod-scene-research.md`.

Memory entries load-bearing for Track C:

- `stock_install_must_remain_playable.md` — Lua additions are sidecar; legacy `.abx` path untouched; no savegame depends on Lua state correctness.
- `magic_abl_contamination_rule.md` — never ship modified `corebrain.abx` in stock distribution; Option A patches ride only with the mod-loader package.
- `cull_gates_are_load_bearing.md` — Lua mods get NO direct access to `inView`/`canBeSeen`/`objBlockInfo`; the supported surface is the BindingRegistry only.
- `mc2_path_separator_linux_build.md` — `LINUX_BUILD` makes path globals use `/`; the Lua sandbox file resolver must not assume Windows separator.
- `carver5_mission_playable.md` — `MAX_STANDARD_FUNCTIONS` bump 256 → 512; verify nifty-mendeleev HEAD has it before C-3.
- `omnitech_abl_stubs_session.md`, `omnitech_abl_missing_names.md` — context for magicpatrol/guard/escort treatment in Q3.
- `debug_instrumentation_rule.md` — `[LUA v1] event=...` lifecycle prints land in same commit as the rework; demote-don't-delete after the bug is fixed.

End of audit.
