# Track B — ImGui Inspector + Hot-Reload Contract: Status Snapshot

**Date:** 2026-04-29
**Scope:** Status check feeding the §5.3 hot-reload contract decision in the modder's-paradise roadmap (`docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md`).
**Mode:** Read-only research.

## Current ImGui state

**ImGui is not vendored.** No `imgui` directory under `A:/Games/mc2-opengl-src/3rdparty/` (which contains only `3rdparty.zip`) nor under any worktree's `3rdparty/` (the live-tree vendor is in `nifty-mendeleev/3rdparty/`: `cmake`, `ffmpeg-lgpl-win64`, `include`, `lib`, `tracy`). `mod-profile-launcher/3rdparty/` adds only `json-vendor/include` (nlohmann/json v3.11.3, commit `d771b85`). A `git log --all --grep="imgui|ImGui|inspector|mission editor"` returns nothing — no ImGui has ever been committed.

The only matches for "imgui" in the worktree are inside the new modder roadmap and the older toolkit design (`docs/plans/2026-04-12-mc2-toolkit-design.md`, `docs/architecture.md`) — both aspirational, no integration code.

**There is no in-game ImGui UI.** Existing in-engine "dev tools" are env-gated `printf` instrumentation only:

- `[INSTR v1]` startup banner (`MC2_HEARTBEAT`, `MC2_TGL_POOL_TRACE`, `MC2_DESTROY_TRACE`, `MC2_GL_ERROR_DRAIN_SILENT`, `MC2_ASSET_SCALE_TRACE`/`SELFTEST`).
- Tracy GUI (vendored, `3rdparty/tracy/`) — external profiler, not in-game.
- Debug hotkeys (RAlt+0..9, F1..F12, [/]) toggling shader/render killswitches via global bools — no UI surface.
- Shadow debug visualizer is a fragment-shader debug-mode branch (RAlt+9), not an ImGui overlay.

## Collaborator WIP found

The `mod-profile-launcher` worktree (`.claude/worktrees/mod-profile-launcher`, branch `claude/mod-profile-launcher`, HEAD `dbbd245`) is the active collaborator track. Reading its log + design doc reveals:

- **Actual scope is profile selection, not ImGui or mission editing.** Plan 1 (foundation) shipped: `code/profile_manager.cpp`, `profile_manager.h`, `profile_manager_bind.cpp`. Profile descriptor JSON parsing (nlohmann/json), base-chain resolution + cycle detection, subdir resolver with overflow guard, path-global binding, `--profile` CLI flag, two stock descriptors (`stock`, `magic_corebrain_only`). Smoke wired with `--profile-cases`.
- **Plan 2 is in design.** Five revisions (latest `dbbd245`, "implementation prompt for fresh session"); topic is "ABL/ABX policy" and "replace stub semantics + production CMake," not UI. UI is explicitly out of scope per `2026-04-23-mod-profile-launcher-design.md` line 32 ("Explicitly out of scope for v1: in-game profile switching, profile download, per-profile saves").
- **No ImGui code, no mission editor code anywhere on this branch.** The collaborator handle in the roadmap text ("ImGui + mission editing") may be a forward-looking framing of future work — current commits do not reflect it.

The roadmap §6 Track B "in flight" status is therefore optimistic: the collaborator branch is shipping a launcher/profile-manager subsystem (highly modder-relevant, but pre-ImGui).

## Existing hot-reload paths

**Shader hot-reload — the only real existing reload path.** Implemented in `GameOS/gameos/utils/shader_builder.cpp`:

- `glsl_program::needsReload()` (line 1077): timestamp check `last_load_time_ < getModTimeMs()` walks every stage source + include.
- `glsl_program::reload()` (line 774): re-loads sources, compiles into a new program, links, and only swaps `shp_` on success. On compile/link failure: `printf("[SHADER] reload failed (compile|link); keeping previous program\n")`. Old program stays live — the "fails silently" caveat in CLAUDE.md is about the user not noticing the printf, not about state corruption.
- Driver: `gosRenderMaterial::checkReload()` in `gameos_graphics.cpp:319`, called per-material per-frame from the render loop at line 2378.

This is exactly the §5.3 contract shape, minus the `[<SUBSYS> v1] event=reload` log format and the bool return convention. It is the natural template for the hot-reload contract.

**No other reload paths exist.**

- `texture manager.hpp` has `Reload(BYTE*, DWORD, bool)` — internal lifecycle for evicted/repopulated textures, not a disk-watch hot-reload.
- AssetScale (`GameOS/gameos/asset_scale.cpp`/`.h`) loads `data/art/asset_sizes.csv` at startup and exposes counters (`dumpCountersTo`); manifest is read-once. **No `reloadFromDisk()` exists** — but the subsystem already has `[ASSET_SCALE v1]` versioned logging and a startup banner, making it the easiest second adopter.
- ABL parser, FST loaders, mission CSV/FIT parsers, gosFX library, `EffectLibrary` — all load-once. Mod-content reloads today require process restart.
- `profile_manager` (collaborator branch) — bound at startup before `Environment.init()`; switching profiles requires restart per its own design.

## Proposed minimal hot-reload contract scope

The §5.3 contract should be specified in a single short follow-up doc with three load-bearing pieces:

1. **Signature.** `bool reloadFromDisk()` member on every modder-content subsystem. Returns true iff the new state is live; false iff the old state is preserved unchanged.
2. **Log line.** `[<SUBSYS> v1] event=reload status=<ok|failed> path=<...>` — matches the existing `[INSTR v1]` schema-versioned grep pattern (`\[SUBSYS v[0-9]+\]`) and the shader path's printf shape.
3. **Atomicity rule.** "On failure, leaves the previous state intact" — directly mirrors how `glsl_program::reload()` keeps `shp_` until the new program links cleanly. This is the precedent the contract should explicitly cite.

ImGui callsite design can defer until ImGui is actually vendored. The contract itself is independent of UI — engine work can adopt the API and Lua console can call it before any ImGui code lands.

## Subsystems that should adopt first

Two precedents, ordered by leverage:

1. **`glsl_program` — formalize the existing path.** Wrap `reload()` with the contract signature: log `[SHADER v1] event=reload status=<ok|failed> path=<frag/vert filename>`, return bool. Almost zero code change; establishes that "the contract" is "what shader reload already does, with one log-line change." This is the cheapest possible precedent and disarms the bikeshed about API shape.
2. **`AssetScale` manifest — the cleanest CSV adopter.** `data/art/asset_sizes.csv` is exactly the modder-edited content shape from roadmap §8.1 ("modder-facing data starts JSON" — but the existing CSV is engineering-internal so it stays CSV; that makes it a *safe* test bed without confusing the modder/engineer audience split). Add `AssetScale::reloadFromDisk()` that re-parses the manifest into a shadow table, atomically swaps on success, logs `[ASSET_SCALE v1] event=reload status=ok|failed path=data/art/asset_sizes.csv`. The subsystem already has versioned logging, a counter dump, and a self-test mode — the contract slots in cleanly.

These two together are the precedent: one renderer-side (timestamp-driven, frequent), one content-side (manual trigger, rare). Every later subsystem (weapon CSVs, pilot rosters, mod manifests) follows the AssetScale pattern.

What **not** to adopt first: ABL/`.abx` parser, FST loader, GameObject lifecycle. The cull-gate fusing rule (`memory/cull_gates_are_load_bearing.md`) means hot-reloading anything participating in object lifecycle is its own multi-week project.

## References

- Roadmap: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md` (§5.3, §6 Track B, §7).
- Shader reload: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/utils/shader_builder.cpp:774-825, 1077-1080`; driver `GameOS/gameos/gameos_graphics.cpp:319-327, 2378`.
- AssetScale: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/asset_scale.{h,cpp}`; spec `docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md`.
- Collaborator worktree: `A:/Games/mc2-opengl-src/.claude/worktrees/mod-profile-launcher/`; design `docs/plans/2026-04-23-mod-profile-launcher-design.md`; Plan 1 foundation closed at commit `4601525`; Plan 2 in design (HEAD `dbbd245`).
- nlohmann/json vendor: `A:/Games/mc2-opengl-src/.claude/worktrees/mod-profile-launcher/3rdparty/json-vendor/include/` (commit `d771b85`).
- Tracy (only vendored "tooling" library): `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/3rdparty/tracy/`.
- Instrumentation schema convention: `\[SUBSYS v[0-9]+\]` (worktree `CLAUDE.md` "Tier-1 Instrumentation Env Vars" section).
