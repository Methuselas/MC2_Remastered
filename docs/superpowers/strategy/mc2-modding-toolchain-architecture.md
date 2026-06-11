# MC2 Modding Toolchain — End-to-End Architecture

**Status:** Strategy synthesis, 2026-06-10. Recon-grounded (editor, asset viewer, runtime bridges, prior recon docs).
**Author:** Fable (orchestrated synthesis over 4 recon agents).

---

## 1. North Star

> **A modder goes from "I have a GLB and an idea" to "I'm playing my mission with my models in mc2.exe" without ever hand-editing a path, duplicating a file, or corrupting a save — and every step is validated before the next one runs.**

The architecture that gets there is **not new tools**. It is three existing tools — Asset Viewer (workbench), Mission Editor, mc2.exe — connected by **one shared, file-based contract: the mod directory** (`mods/<id>/` + `mod.json` + manifests). No live IPC, no shared in-process state, no second database. Every bridge is "write a file, launch a process, parse its output" — the pattern that already works (EditorTaskRunner, smoke harness, MC2_DEBUG_STATE_DUMP).

**The mod directory IS the project file.** This is the single most important decision in this document.

---

## 2. What Already Exists (the substrate)

| Capability | Owner today | Status |
|---|---|---|
| Mod mounting (`MC2_ACTIVE_MOD`, `mods/<id>/data/`, deps, first-wins priority, `.modindex-cache`) | `mclib/file.cpp:62-549` | **Shipped, proven** (mc2x-compat 6/6 campaign smokes) |
| Model override registry (`models.json`, key=`class:appearanceName`, renderOnly, fallback=stock) | `mclib/model_override_registry.cpp` | **Shipped** |
| Asset cook pipeline (G1 stage → G2 textures → G3b assemble; schema + coherence validation) | `tools/asset_cook/trackg_cook.py` | **Shipped** (2015 stock assets cooked, 0 errors) |
| Asset Viewer mod workbench S1–S5 (GLB import, side-by-side preview, validation, bundle export, reversible central-manifest merge) | `tools/asset_viewer/ModWorkbench.*` | **Shipped** |
| Editor mission save/load (.pak via PacketFile, MOVE packet 4 rebuild, .fit sidecars) | `editor/EditorData.cpp` | **Shipped, format frozen** |
| Editor ImGui panels (Outliner, Inspector, Asset Browser, Validator/Checklist, Command Palette, Undo History, Task Monitor) | `editor/EditorInterface.cpp:4843-4873` | **Shipped** |
| One-click playtest (`Launch Game on this Mission` → `mc2.exe -mission <pak>` via EditorTaskRunner/CreateProcess, stdout/stderr captured) | `editor/EditorInterface.cpp:5301-5326` | **Shipped, rough** |
| Build Mod Package button | `editor/EditorInterface.cpp:5330-5376` | **Shipped, rough** |
| Game structured stdout (`[SMOKE v1]` events, `[TIMING]`, `[PERF]` p50/p99) | `GameOS/gameos/gos_smoke.cpp` | **Shipped** |
| Render-state JSON dump + MCP reader (`MC2_DEBUG_STATE_DUMP` → `debug_state/latest_render_state.json` every ~5s) | `scripts/mcp/mc2_render_state_server.py` | **Shipped** |
| Editor render bridge v0 (read-only pick/visibility/overlay, env-gated) | `EditorBridge/EditorRenderBridge.*` | **Shipped** |
| Smoke harness (tiers, heartbeat, lock, artifacts) | `scripts/run_smoke.py` | **Shipped** |

**Conclusion: ~85% of the substrate exists. The missing 15% is cohesion** — the mod project abstraction, the telemetry return path into the editor, and making the editor mod-aware.

---

## 3. Tool Ownership Map

Each tool owns exactly one lifecycle stage. Nothing is owned twice.

### Asset Viewer ("Workbench") — owns **assets**
- Browse/preview stock + override assets (textures, TGL, GLB, materials, LOD chains).
- Import GLB → cook (drives `trackg_cook.py` stages) → validate (schema + semantic + screenshot side-by-side).
- Produce/merge `models.json` override manifests (reversible central merge — already S5).
- Owns the **appearance-key roster** view (derived from `data/tgl/*.ini` — engine data, never re-stored).
- **Does NOT own:** missions, placement, gameplay data, deploy.

### Mission Editor — owns **missions**
- Terrain, objects, objectives, MOVE packet 4, .pak/.fit save (format frozen).
- Modern panels = views over existing systems (EditorObjectMgr, ActionUndoMgr, EditorObjectCatalog).
- Owns **mission validation** (Mission Checklist) and **playtest orchestration** (launch, monitor, ingest telemetry).
- **Does NOT own:** asset cooking, override manifests (reads them; edits go through Workbench), runtime truth.

### mc2.exe — owns **runtime truth**
- Authoritative sim, render, mod mounting, asset resolution.
- Speaks outward only via: exit code, structured stdout (`[SMOKE v1]` etc.), `debug_state/*.json`, screenshots.
- Receives input only via: cmdline (`-mission`), env (`MC2_ACTIVE_MOD`, `MC2_*`), and files on disk.
- **Never embedded, never linked into a tool.**

### `trackg_cook.py` + validators — own **transformation correctness**
- Headless, scriptable, no UI. Both Workbench and CI call the same scripts. UI never reimplements cook logic.

### `mods/<id>/` directory — owns **the project state**
- The only thing all three tools read. See §5.

---

## 4. Bridge Map

All bridges are **files + process spawn + parsed output**. No sockets, no shared memory, no in-process embedding (matches PIE recon verdict).

```
                       ┌──────────────────────────────────────────┐
                       │            mods/<id>/  (PROJECT)         │
                       │  mod.json · models.json · missions/ ·    │
                       │  data/tgl/ · .modindex-cache             │
                       └──────────────────────────────────────────┘
                          ▲  R/W            ▲  R/W           ▲  R (mount)
            ┌─────────────┘                 │                └─────────────┐
            │                               │                              │
   ┌────────┴────────┐  B1: manifest files ┌┴───────────────┐   B3: env+cmdline   ┌──────────┐
   │  ASSET VIEWER   │ ───────────────────▶│ MISSION EDITOR │ ──────────────────▶ │ mc2.exe  │
   │  (workbench)    │                     │                │  CreateProcess      │ (runtime)│
   └────────┬────────┘                     └┬───────────────┘  MC2_ACTIVE_MOD     └────┬─────┘
            │ B0: spawns                    │       ▲           -mission <stem>        │
            ▼                               │       └──────────────────────────────────┘
   ┌─────────────────┐                      │         B4: telemetry return
   │ trackg_cook.py  │                      │         (stdout [SMOKE v1] pipe — already captured
   │ validators      │                      │          by EditorTaskRunner — + debug_state/*.json
   └─────────────────┘                      │          + exit code + screenshots)
                                            ▼
                                   B2: deploy/package
                                   (copy-if-diff into game deploy mods/, zip for share)
```

**B0 — Workbench → cook scripts.** Already exists. Workbench shells out; never re-implements.
**B1 — Workbench → Editor.** Pure file handoff: Workbench writes `mods/<id>/data/model_overrides/models.json` + cooked ktx2/glb; Editor only needs to *see* the mod mounted (gap: editor mod-awareness, slice 3).
**B2 — Editor → deploy.** "Build Mod Package" copies `mods/<id>/` into game deploy `mods/` (copy-if-diff like deploy scripts) and/or zips. Already exists rough.
**B3 — Editor → game.** Already exists: EditorTaskRunner + `mc2.exe -mission <pak>`. Gap: pass `MC2_ACTIVE_MOD=<id>` in child env, auto-save first, edit-lock during play (PIE recon's exact v0 list).
**B4 — Game → Editor.** The one genuinely new bridge — but both halves exist: game already emits structured stdout and EditorTaskRunner already captures it. Missing piece = a parser feeding the Gameplay Debugger / a Playtest Results panel. File-based, async, read-only.

**Bridge boundary rule:** a bridge may carry *data*, never *behavior*. If a tool needs engine logic (cull, pick, asset resolution), it either shells to the engine or uses an extracted static lib with stubs (the `mc2_tglloader` pattern the asset viewer already proves) — never a reimplementation.

---

## 5. The Mod Project Abstraction (minimum viable)

**Decision: there is no separate "project file." `mods/<id>/` + `mod.json` IS the project.** The engine already mounts it; the editor and workbench adopt the same root. One format, three readers, zero drift.

```
mods/<id>/
  mod.json                      # identity: id, name, version, dependencies[]   (ENGINE-OWNED schema)
  data/                         # everything below mirrors game data/ layout — engine first-wins overlay
    missions/<m>.pak            # editor-owned (PacketFile, frozen format)
    missions/<m>.fit            # editor-owned sidecar
    missions/<m>.foliage.json   # editor-owned sidecar
    model_overrides/models.json # workbench-owned (schema: asset_manifest.schema.json)
    tgl/<tier>/*.ktx2           # cook-owned outputs
    tgl/*.ini                   # appearance inis (stubs or overrides)
    campaign/*.fit              # optional campaign
  .modproject/                  # NEW, tool-only, engine-IGNORED, gitignore-able
    workbench.json              # workbench UI state: import source paths, last validation report
    editor.json                 # editor UI state: last open mission, game exe path, playtest prefs
    playtest/<timestamp>/       # B4 telemetry artifacts (stdout log, parsed events, screenshots)
```

Rules:
- Everything under `data/` is **engine-meaningful** and validated by existing validators. Tools never invent keys there.
- Everything tool-private goes under `.modproject/` which the engine's mod indexer must skip (one-line filter in `file.cpp` index walk — only engine change needed).
- `mod.json` grows at most additive optional fields (e.g. `displayVersion`, `author`); engine ignores unknown keys already.
- "Open mod project" in either tool = pick `mods/<id>/` directory. No registry, no global state.

---

## 6. What Must Never Be Duplicated

1. **Asset registry.** The appearance roster = `data/tgl/*.ini` scan + `tgl.fst` contents. Workbench *derives* its roster live (already does, `AppearanceRoster.cpp`); editor *derives* its catalog from CSV + .fit (already does). Neither persists a copy.
2. **Cook logic.** Only `trackg_cook.py` / `mc2texcook` transform assets. UI = thin driver.
3. **Manifest validation.** `asset_manifest.schema.json` + `validate_asset_manifest.py` + registry-load rules are the three layers; Workbench semantic validation calls into/replicates the documented rules from one place (`WorkbenchValidation.cpp`) — if a rule changes, it changes in schema first.
4. **Mission format.** PacketFile read/write lives in mclib, shared by editor and game already. No tool writes .pak any other way.
5. **Mod mount/priority semantics.** Only `mclib/file.cpp`. If the editor needs "which file wins," it asks via the same code (editor links mclib already) — never reimplements first-wins.
6. **Projection/pick math.** Editor render bridge (EditorBridge) is the only sanctioned path — the D3D↔GL split-brain history (shadow a365e6ad, props 09707cd8, frustum a280dde2) is the standing proof of what duplication costs.
7. **Telemetry schemas.** `[SMOKE v1]` line grammar and `MC2_DEBUG_STATE_V1` JSON are versioned engine-owned contracts; consumers parse, never fork.

---

## 7. Ideal Modder Workflow (end-to-end, text diagram)

```
 1. CREATE     Workbench: "New Mod" → scaffolds mods/<id>/ + mod.json + empty manifests
 2. IMPORT     Workbench: drop .glb → G1 stage → G2 textures → G3b assemble
               → schema+coherence validation → side-by-side visual check vs stock
 3. INSTALL    Workbench: "Add to mod" → writes models.json into mods/<id>/data/model_overrides/
               (reversible merge, S5 path) → round-trip registry validation
 4. AUTHOR     Editor: "Open Mod Project" → mounts mods/<id>/ read-aware
               → Asset Browser shows stock ∪ mod overrides (override badge)
               → place/inspect via existing panels; every edit through Action (undo)
 5. VALIDATE   Editor: Mission Checklist (existing) + mod-aware checks
               (missing appearance in mounted mod, MOVE-dirty, asset refs resolve under mount)
 6. PLAYTEST   Editor: [Play] → auto-save → edit-lock → EditorTaskRunner spawns
               mc2.exe -mission <stem> with MC2_ACTIVE_MOD=<id> MC2_DEBUG_STATE_DUMP=1
 7. OBSERVE    Game emits [SMOKE v1] events + debug_state/*.json + (optional) screenshots
               → Editor Playtest panel tails stdout live, archives to .modproject/playtest/<ts>/
               → on exit: parse summary (pass/fail, perf p50/p99, FATALs) → surface in panel
 8. ITERATE    fix → Play again (loop 4–7; each playtest is a fresh authoritative mc2.exe run)
 9. PACKAGE    Editor: "Build Mod Package" → validate manifest set → copy-if-diff into game
               deploy mods/ → optional zip <id>-<version>.zip for distribution
10. SHARE      recipient unzips into mods/, sets MC2_ACTIVE_MOD (or launcher picks) — engine
               mount system does the rest (deps, priority, cache)
```

---

## 8. Phased Roadmap (5–10 slices, each independently shippable)

Ordering principle: **close the loop end-to-end first (even rough), then thicken each segment.** Slices 1–3 make the loop real; 4–8 polish it.

| # | Slice | What ships | Risk | Builds on |
|---|---|---|---|---|
| **1** | **Playtest v0 polish** | Auto-save before launch; edit-lock during play (ObjectSelectOnlyMode); persist exe path; Stop button; stdout → log artifact | LOW | EditorTaskRunner, existing button (PIE recon's exact list) |
| **2** | **Mod-aware playtest** | Launch passes `MC2_ACTIVE_MOD=<id>` + `MC2_DEBUG_STATE_DUMP=1` in child env; mission stem resolved under mod mount | LOW | Slice 1, `file.cpp` mount |
| **3** | **"Open Mod Project" in editor** | Editor picks `mods/<id>/`; missions default-save under `mods/<id>/data/missions/`; Asset Browser marks overridden appearances; engine indexer skips `.modproject/` | MED | mod.json, EditorObjectCatalog |
| **4** | **Playtest Results panel (B4 v0)** | Parse captured `[SMOKE v1]`/`[exit]` lines post-run: pass/fail, mission_ready time, perf p50/p99, FATAL count; archive to `.modproject/playtest/<ts>/` | LOW-MED | Slices 1–2; gos_smoke grammar |
| **5** | **Workbench "New Mod" scaffold + project open** | Scaffold `mods/<id>/` (mod.json, dirs); Workbench S5 merge targets project's models.json instead of central deploy | LOW | ModWorkbench S5 |
| **6** | **Mod-aware mission validation** | Checklist additions: every placed appearance resolves under mod mount; MOVE not dirty; override manifest schema-valid (shell `validate_asset_manifest.py`) | MED | Slice 3, MissionValidator |
| **7** | **Package v1** | "Build Mod Package" → run full validation gate → copy-if-diff to deploy `mods/` → zip with version from mod.json; manifest of contents + hashes | LOW | Slice 5–6, existing button |
| **8** | **Live playtest tail (B4 v1)** | Stream stdout into panel during run (EditorTaskRunner already pipes); live event ticker; optional periodic `debug_state` JSON poll for render health | MED | Slice 4 |
| **9** | **Runtime gameplay telemetry opt-in** | New env-gated engine emitter (`MC2_PLAYTEST_TRACE=1`): selected-unit brain/path/combat lines on the same stdout grammar → feeds editor Gameplay Debugger with real run data | MED-HIGH (engine change) | Slice 8; PIE recon's "live debugger" recommendation |
| **10** | **Part-index ↔ EditorObject map** | Persist editor-object↔runtime-Part table at save; lets telemetry lines reference editor objects ("this gate, this turret") | HIGH | Slice 9; defer until 9 proves value |

Stop-line: slices 1–7 are committed direction. 8–10 re-evaluate after 7 ships.

---

## 9. Anti-Goals

- **No in-process PIE.** Sim/render entangled (BattleMech::update→render, 17-subsystem tick, unconditional HUD/camera). mc2.exe stays the runtime. (pie-feasibility-recon verdict, reaffirmed.)
- **No editor rewrite.** MFC dialogs stay; ImGui exposes, never replaces. No new editor framework.
- **No second asset database.** No SQLite, no asset GUID system, no central index file. Catalogs = live derivation from engine data (ini scan, fst, csv).
- **No PacketFile/.pak format changes.** New data = sidecar files or additive .fit blocks only.
- **No live editor↔game IPC** (sockets/shared-mem/COM). Files + process + stdout is the contract. Revisit only if slice 9 telemetry latency proves inadequate.
- **No mod manager UI** (load-order GUI, conflict resolver) in this arc. Engine first-wins + deps already define semantics; tooling for it is a later, separate product.
- **No reimplemented engine math in tools** (pick, cull, projection, mount priority). Bridge or extracted-lib only.

---

## 10. Risks & Traps

| Trap | Why it bites here | Mitigation |
|---|---|---|
| **Parallel state** (editor's idea of mod ≠ engine's) | Editor caches catalog/roster; mod content changes on disk (workbench merge mid-session) | Tools derive on open + explicit Refresh; never persist derived registries; `.modindex-cache` invalidation already content-driven |
| **Format drift** (`models.json` / `[SMOKE v1]` / `MC2_DEBUG_STATE_V1` consumers fork) | 3+ readers per format | Schemas are engine-owned + versioned; consumers fail loud on unknown version; validators shelled, not copied |
| **Editor/game divergence** (editor renders/validates differently than game) | Editor uses modern chain w/ editor gates (`MC2_IS_EDITOR` setupTextures, RTT, snapshot flags); game is chunk/GPU-only | Playtest IS the truth gate — keep loop cheap (slices 1–2) so "looks right in editor" never substitutes for a run; track editor-only gates in one doc |
| **Deploy-target trap** (stale exe / wrong dir) | Proven history: v0.4 vs 0.4c stale-exe cost a full debug cycle | Playtest launch verifies exe mtime ≥ newest build artifact, warns; exe path stored per-project in `.modproject/editor.json` |
| **Runtime embedding creep** | "Just link the sim for preview" temptation returns | Anti-goal §9; the `mc2_tglloader` stub-lib pattern is the only sanctioned static reuse |
| **Asset registry duplication** | Workbench roster, editor catalog, engine ini-scan could each become persisted copies | §6 rule 1; code review gate: no tool writes a derived asset list to disk |
| **MOVE packet staleness** | Oversized maps can never have legacy MOVE (>720 cells); blank-synthesis fallback is degraded | Validator (slice 6) surfaces MOVE state explicitly: ready / rebuilt / synthesized-blank / impossible-for-size |
| **Single-threaded editor + spawned processes** | UI freeze if any bridge call blocks | Everything through EditorTaskRunner (worker + PostMessage marshal); no synchronous waits on game |
| **`.modproject/` leaking into engine mount** | Engine indexes everything under mods/<id>/ | One-line skip filter in `file.cpp` index walk, with test |
| **Worktree confusion** | ~20 sibling worktrees; prior lost-work incident | All slices in nifty-mendeleev; absolute paths in prompts |

---

## 11. Concrete Next 3 Implementation Prompts (for Opus)

### Prompt 1 — Playtest v0 polish (Slice 1)
> In worktree `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev`, harden the editor's one-click playtest (Mission Tools panel, `EditorInterface.cpp:5301-5326`). (1) Before launch: if mission dirty or never saved, auto-save via existing `EditorData::save` path, honoring `RebuildMoveIfDirty` — abort launch with a clear panel message if save fails. (2) During run: enter edit-lock (selection-only mode), show a "Playing…" status with a Stop button that terminates the child process cleanly via EditorTaskRunner. (3) Persist the game exe path and last-used flags in a small hand-parsed JSON at `.modproject/editor.json` next to the mission's mod root (fall back to per-user file if no mod root). (4) Archive child stdout/stderr to a timestamped log file and surface its path in the Task Monitor. (5) Verify deployed exe mtime is newer than the newest local build artifact and warn in-panel if stale (deploy-trap guard). Constraints: no PacketFile format changes, all edits via existing Action/undo where applicable, single-threaded UI (EditorTaskRunner only), no nlohmann — hand-parse JSON like FoliageRender does. Add `-smoke-playtest` CLI smoke that exercises save-gate + launch-fail path headlessly. Read `docs/superpowers/strategy/mc2-modding-toolchain-architecture.md` §8 slice 1 first.

### Prompt 2 — Mod-aware playtest + `.modproject/` carve-out (Slices 2 + 3 prefix)
> Same worktree. (1) In `mclib/file.cpp` mod indexer, skip any `.modproject/` directory under `mods/<id>/` (tool-private data, engine must ignore); add a unit-style smoke proving a file under `.modproject/` is not resolvable via mod mount. (2) In the editor playtest launcher, when the open mission lives under `mods/<id>/data/missions/`, set `MC2_ACTIVE_MOD=<id>` and `MC2_DEBUG_STATE_DUMP=1` in the child environment (EditorTaskRunner env passthrough already inherits; add explicit overrides). Detect the mod root by walking up from the mission path to a directory containing `mod.json`. (3) Show the detected mod id in the Mission Tools panel ("Playtesting as mod: <id>" or "no mod — base game"). Constraints: do not change mount priority semantics or `.modindex-cache` format beyond the skip filter; tier1 smokes must stay 5/5; mc2x-compat tier3 smokes must stay green. Read architecture doc §5 (project layout) and §8 slice 2-3 first.

### Prompt 3 — Playtest Results panel (Slice 4, B4 v0)
> Same worktree. Build a post-run Playtest Results ImGui panel in the editor. After a playtest child exits, parse its captured stdout for the structured grammar in `GameOS/gameos/gos_smoke.cpp` (`[SMOKE v1] event=banner|mission_resolve|mission_ready|summary`, `[TIMING v1]`, `[PERF v1]` p50/p99, `[exit]` marker, plus FATAL lines). Render: result (pass/fail/crash via exit code + summary line), time-to-mission-ready, perf percentiles, FATAL/error count with first 5 lines expandable, and a button to open the full log. Archive the parsed summary + raw log under `<modroot>/.modproject/playtest/<timestamp>/` (or a temp dir if no mod root). Parser must be version-tolerant: unknown `[SMOKE v1]` keys ignored, unknown grammar versions → "raw log only" fallback, never a crash. Hand-parse, no nlohmann. Add a `-smoke-playtest-results` CLI flag that feeds a canned captured-log fixture through the parser headlessly and asserts the parsed fields. Read architecture doc §4 (bridge B4) and §8 slice 4 first; do not add any new engine-side emitters in this slice.

---

## 12. Source Recon Index

- Editor structure/panels/launch: agent recon over `editor/` (EditorInterface, EditorData, EditorObjectMgr, EditorTaskRunner, smoke CLI flags).
- Asset viewer/cook/override/mount: agent recon over `tools/asset_viewer/`, `tools/asset_cook/`, `mclib/model_override_registry.*`, `mclib/file.cpp`, `docs/asset-pipeline.md`.
- Runtime bridges/telemetry: agent recon over `gos_smoke.*`, `mc2_hitch_trace.*`, `scripts/run_smoke.py`, `scripts/mcp/mc2_render_state_server.py`, `EditorBridge/`.
- Prior strategy: `.claude/editor-modder-roadmap-recon.md`, `.claude/editor-modernization-recon.md`, `.claude/ue-like-editor-recon-1.md`, `.claude/pie-feasibility-recon.md`, `.claude/engine-lane-separation-strategy.md`.
