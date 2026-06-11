# MC2 Data Ownership & Registry Strategy

**Status:** Strategy / design doc (long-term), 2026-06-11.
**Scope:** Source-of-truth per data domain (mechs, vehicles, weapons, appearances, effects, terrain types, missions, manifests); ID allocation; the read-only generated registry index for tools; what stays distributed.
**Siblings:** `mod-packaging-deploy-architecture.md` (overlay + package layer), `asset-cook-pipeline-architecture.md` (source/cooked/cache taxonomy), `mc2-modding-toolchain-architecture.md` (tool ownership). This doc owns the **truth lane**: who owns each record, who may write it, and how tools query it without inventing a database.

---

## 1. North star

> **Every data domain keeps exactly one writable truth, and it is always a file the engine already loads. Tools never get a writable database; they get a read-only generated index built by scanning those files. The engine never reads the index. When the index and the files disagree, the files win and the index is stale by definition.**

Load-bearing consequences:

1. **The engine's per-format loaders are the constitution.** `FitIniFile`, `PacketFile`, `csv` parsing (`MasterComponent::initEXCEL`, `mclib/cmponent.cpp`), `AppearanceTypeList::getAppearance` (`mclib/apprtype.cpp:225`), `gosFX::EffectLibrary::Load` (`mclib/txmmgr.cpp:564-581`) are proven over 20+ years of content. No format change, no loader rewrite, no "registry pass" inserted in front of them.
2. **One queryable view for tools = a cache, not an authority.** Tools (Asset Viewer, editor pickers, `mc2mod check`, validators, CI) consume a generated `registry-index.json` rebuilt from the real files. It carries provenance + staleness markers and can be deleted at any time. This is the same trick as `.modindex-cache` (`mclib/file.cpp:161`) — proven, machine-local, self-invalidating.
3. **Mod overrides resolve at the file layer, never in the registry.** The canonical record under a mod is *whatever file the engine's first-wins overlay resolves* (`mclib/file.cpp`, `g_modIndex`, priority active mod > deps > base > FastFile > CD). The index *predicts* that resolution per mod-chain; the engine remains the oracle (`MC2_LOG_FILE_RESOLVE=1` parity, packaging doc §6).

---

## 2. Domain-by-domain ownership table

"Writer" = who may legitimately mutate the truth. "Canonical under mod" = which record wins when a mod overlays.

| Domain | Today's truth (format + location) | Loader code path | Writer(s) | Canonical under mod |
|---|---|---|---|---|
| **Mechs** (chassis/variants) | profile FITs in `data/objects/` + variant CSVs (`code/logisticsdata.cpp:302` `<variant>.csv`); object-type packets in `object2.pak` | `code/mech.cpp`, `code/logisticsdata.cpp`, `ObjectTypeManager::load` (`code/objtype.cpp:355` seekPacket by objTypeNum) | hand edit (FIT/CSV); purchasing/logistics UI writes variant CSVs at runtime (player-owned, not mod data) | mod's shadow file at same `data/...` relpath (first-wins overlay) |
| **Vehicles / buildings / turrets** | same shape: FITs + `buildings.csv` (`logisticsdata.cpp:227`), object-type packets | same `ObjectTypeManager` + `code/objtype.cpp` switch on type | hand edit; editor places *instances* (never edits type records) | shadow file |
| **Weapons / components** | `compbas.csv` (one row per master component) | `MasterComponent::initEXCEL` (`mclib/cmponent.cpp`), loaded by `mission.cpp:1970`, `saveload.cpp:842`, `logisticsdata.cpp:120` | hand edit ONLY (whole-file shadow; row-level merge is future work) | mod's whole `compbas.csv` shadows base — coarse, acknowledged (packaging doc §10 "monolith" risk) |
| **Appearances** | `.ini` (+ compiled `.tgl`, source `.ase`) in `data/tgl/`; name-keyed | `AppearanceTypeList::getAppearance(apprNum, apprFile)` (`mclib/apprtype.cpp:225`) → `Mech3DAppearanceType`/`GVAppearanceType`/`bdactor` init; `msl.cpp:563 LoadTGMultiShapeFromASE` compiles ASE→TGL | hand edit `.ini`; cook tools emit `.tgl`/ktx2 tiers (derived); Asset Viewer emits override GLB + `models.json` records (additive layer, never edits `.ini`) | shadow `.ini`/`.tgl`; model overrides resolve via `class:appearanceName` key in `model_override_registry.cpp:162` (mod's `models.json` over central) |
| **Effects** | `data/effects/mc2.fx` monolith (gosFX serialized stream, name-indexed inside library) | `gosFX::EffectLibrary::Instance->Load` (`mclib/txmmgr.cpp:581`; path set `code/mechcmd2.cpp:1673`) | NOTHING writes it today; mods shadow the whole file | whole-file shadow — finest grain available; per-effect split is engine work (out of scope, cook doc §8.5) |
| **Terrain types / textures** | per-tileset texture-list `.FIT` (`MaxTerrainTextures/Types/Overlays/Details`, `mclib/terrtxm.cpp:125-145`) + named `.tga`/`.ktx2` tiers in `data/textures/` | `TerrainTextures` ctor (`terrtxm.cpp`), colormap probe chain (`terrtxm2.cpp:1926` burnin.jpg→ktx2→tga) | hand edit FIT; texture cooks write sidecars only | shadow FIT and/or texture sidecars |
| **Missions / campaigns** | `data/missions/<name>.fit` (FitIniFile) + `<name>.pak` (PacketFile; packet 4 = MOVE, format frozen) + `data/campaign/*.fit` | `code/mission.cpp` init; `EnumerateModCampaignFiles` (`file.cpp:443`) | **Editor ONLY** for `.pak` (MOVE invariants: `moveSide ≤ 720`, `SECTOR_DIM` multiple — see mission.cpp synthesis fallback); hand edit `.fit` tolerated | shadow `.fit`/`.pak` pair (must ship together) |
| **Manifests** | `mod.json` (engine-parsed, `file.cpp:393`), `models.json` (engine-parsed, `model_override_registry.cpp:162`), `package.json` + per-asset `manifest.json` + `cook.json` (tool-only) | engine reads only the first two; others per cook/packaging docs | `mod.json` hand edit; `models.json` via `CentralManifestMerge.cpp` only; `package.json`/`cook.json` generated by pack/cook tools, never hand-maintained | mod's own `models.json` over central; `mod.json` is per-mod, no conflict |

Cross-cutting writer rules (binding):

- **Editor** writes mission/campaign data and nothing else. It never edits object types, CSVs, appearances, or manifests.
- **Asset Viewer / Workbench** writes override GLBs + generated manifests + merged `models.json` records (via the `.bak`+atomic+verify path). It never edits FITs, CSVs, `.pak`, or `mc2.fx`.
- **Cook tools** write derived sidecars only (`.ktx2`, `.burnin.jpg`, `.tgl`, `cook.json`); deleting their output loses nothing (cook doc §1.2).
- **Hand edit** remains a first-class writer for FIT/CSV/INI — these are the moddable text formats and we keep them human-writable forever.
- **mc2.exe** writes save data and player logistics state (variant CSVs, `save.fit`) into base `data/` only — never inside `mods/` (packaging doc §5.3).

---

## 3. ID allocation rules

Three ID spaces exist; each gets an explicit allocation + collision rule. **No central ID server** — rules are conventions enforced by the validator, with the index as the collision detector.

1. **Object type IDs (`objTypeNum` = packet index in `object2.pak`).** Dense integer = packet number; `ObjectTypeManager` table cap is 4096 (raised from 1024 for Omnitech content at ~2100, `code/objtype.h:228-232`; out-of-range or missing packet → clean NULL fallthrough). Allocation: **base game owns 0..1023; the 1024..2199 band is occupied by known mods (Omnitech); new mod content allocates from 2200 upward in blocks of 100 per mod**, recorded in the mod's `package.json` (advisory field `objTypeBands: [[2200,2299]]`). Collision check = registry index intersects bands across installed mods (`mc2mod check` extension). Hard ceiling 4095 until the table cap is revisited. Mods that need to *replace* a stock type shadow `object2.pak` wholesale today — coarse; per-packet override is future engine work, not a registry problem.
2. **Appearance numbers (`apprNum`).** Upper 8 bits = appearance class (`MECH_TYPE`, `GV_TYPE`, buildings...; `apprtype.h:104`, `apprtype.cpp:232`), lower 24 = type number, but **the de-facto key is the `.ini` file NAME** — `getAppearance` dedupes by `S_stricmp(name, apprFile)` (`apprtype.cpp:249`), not by number. Rule: **names are the allocated identity; appearance numbers must only be unique per (class, name) pair the engine actually compares**. Mods add appearances by adding uniquely-named `.ini` files (mc2x-compat's 13 tgl stubs are the precedent) and replace by shadowing the same name. Collision = two mods in one dep chain shipping the same `data/tgl/<name>.ini` with different content → the existing `[mod-dup]` first-wins detector covers it; the index surfaces it by name.
3. **Packet indices inside `.pak` files.** Frozen, positional, format-owned (mission paks: packet 4 = MOVE; `object2.pak`: packet = objTypeNum). **Never reallocated, never compacted, never registry-managed.** The editor is the only writer of mission packets and already owns the invariants; the registry records packet *counts* and known-slot presence (e.g. "packet 4 non-NUL") for validation only.
4. **Names everywhere else** (effects by name in the library, terrain types by FIT order within a tileset, component rows by CSV row order/ID column, override records by `class:appearanceName`). Rule: **name = identity; rename = new asset**. Terrain-type and component ordinals are positional within their file — a mod that shadows the file owns the whole ordinal space, which is why whole-file shadowing is the supported grain.

---

## 4. Generated registry index design

### What it is

`registry-index.json` (+ optional per-domain shards) under `<deploy>/.registry/` or a mod project's `out/` — a **read-only, regeneratable cache** built by `tools/registry/build_index.py` scanning the real files through the same resolution rules the engine uses. The engine **never** opens it (same hard rule as `package.json`, packaging doc §3). Tools (Viewer pickers, editor asset browser, `mc2mod check`, CI validators, future launcher) only ever read it or rebuild it.

### Schema (v1)

```json
{
  "schema": "mc2-registry-index/1",
  "builtAt": "2026-06-11T...",
  "toolVersion": "...",
  "deployRoot": "A:/Games/mc2-opengl/mc2-win64-v0.4",
  "modChain": ["mc2x-pbr", "mc2x-compat"],        // chain the index was resolved against ([] = base)
  "inputs": [ {"path": "data/objects/compbas.csv", "mtime": 133..., "size": 48211, "sha256?": "..."} ],
  "domains": {
    "objectTypes":  [ {"id": 2201, "name": "...", "sourcePak": "object2.pak", "packet": 2201, "providedBy": "base|<modId>"} ],
    "appearances":  [ {"name": "abuilding", "class": "BLDG", "ini": "data/tgl/abuilding.ini", "tgl": true, "overrideGlb": "mods/x/...", "providedBy": "..."} ],
    "components":   [ {"row": 17, "id": "...", "name": "...", "file": "data/objects/compbas.csv", "providedBy": "..."} ],
    "effects":      [ {"name": "...", "library": "data/effects/mc2.fx", "providedBy": "..."} ],
    "terrainTypes": [ {"tileset": "...", "ordinal": 3, "texture": "...", "fit": "...", "providedBy": "..."} ],
    "missions":     [ {"name": "mc2_01", "fit": "...", "pak": "...", "pakPackets": 3520, "movePacketPresent": true, "providedBy": "..."} ],
    "overrides":    [ {"key": "bldg:abuilding", "manifest": "...", "glb": "...", "providedBy": "...", "shadows": "central|<modId>"} ]
  },
  "conflicts": [ {"domain": "appearances", "key": "abuilding", "winner": "mc2x-pbr", "losers": ["mc2x-compat"], "kind": "mod-dup"} ]
}
```

Every record carries `providedBy` (resolution result for the given `modChain`) and the file path it was parsed from — `enumerate`, `validate`, `resolve` are then trivial queries over one JSON. CLI surface: `registry build / list <domain> / resolve <domain> <key> / check` (the latter folds into `mc2mod check`).

### Build

- Pure Python 3 stdlib, sibling of `tools/validate_asset_manifest.py` in style. Parsers are **read-only and minimal**: FIT block/id reads, CSV rows, `mod.json`/`models.json` JSON, PacketFile header walk (packet count + per-packet size only — never decompress payloads), `mc2.fx` effect-name table scan. Where a payload parse is risky (gosFX stream), degrade to "file present + hash" granularity rather than re-implementing the loader.
- Resolution mirrors `file.cpp` exactly: active mod > dep[0..N] > base, first-wins, `data/...` relpaths only. Parity oracle: a scripted `MC2_LOG_FILE_RESOLVE=1` run diffs `[mod-hit]/[mod-dup]` lines against the index's `providedBy`/`conflicts` (same gate as packaging doc slice 5). **Any divergence is an index bug.**

### Staleness / rebuild rules

1. **Self-invalidating:** every consumer first checks `inputs[]` mtimes+sizes against disk (the `.modindex-cache` 2-level-sweep lesson: do a *full* recorded-input check, not a directory-level heuristic). Any mismatch → index is stale → consumer either rebuilds inline (<2s target on a stock deploy) or proceeds with a loud "STALE INDEX" banner; tools MUST NOT silently serve stale data.
2. **Rebuild triggers:** `mc2mod install/uninstall/pack` and Viewer central-manifest merges call `registry build` as a post-step; editor rebuilds on mod-project switch; CI rebuilds fresh always. Dev loops with `--link` mods set `MC2_REBUILD_MOD_CACHE=1` anyway — the same launch script also touches a `registry build`.
3. **Deletable always:** removing `.registry/` breaks nothing; first tool query rebuilds. Never committed, never packaged (joins `.modindex-cache`/`.install-receipt.json` in the never-packaged set, packaging doc §2).
4. **Chain-keyed:** the index is valid for one `modChain`; switching active mod = different chain = rebuild (cheap because base-domain shards can be cached by input-hash and only the overlay diff re-resolves).

---

## 5. Mod override resolution order (canonical record)

One ordering, defined once, used by engine and predicted by tools:

1. **File-level:** active mod `data/...` > dependency[0]..[N] `data/...` > base `data/` > FastFiles > CD (`mclib/file.cpp`, `g_modIndex` first-wins). The canonical record for any path-keyed domain (CSVs, FITs, `.ini`, `.fx`, `.pak`) is the file this resolution returns — whole-file granularity.
2. **Record-level (model overrides only):** mod's `data/model_overrides/models.json` records > central `models.json` records > stock `.ase`/`.tgl` path, keyed `class:appearanceName` with `fallback:"stock"` semantics (`model_override_registry.cpp:162`). This is the ONLY record-grained override layer that exists; do not invent others until an engine seam ships (e.g. per-row compbas merge — explicitly deferred).
3. **Sidecar/cook level:** cooked sidecars probe within whatever directory step 1 resolved (cook doc §7.5) — a mod overrides a cook by shadowing the sidecar path like any file.

The index's `conflicts[]` is the static projection of this order; `MC2_LOG_FILE_RESOLVE=1` is the dynamic truth.

---

## 6. What stays distributed (must NOT be centralized)

- **All engine-read data files.** No domain migrates into a central store; the registry is a view, never a home.
- **Packet contents.** `.pak`/FST internals stay opaque to the registry beyond counts/presence; only `PacketFile` and the editor touch them.
- **`mod.json` identity + dependency lists** — per-mod files, engine-parsed; no global mod database or load-order file (packaging doc §6 "no lockfiles").
- **Per-asset manifests and `cook.json`** — sidecars next to their assets (cook doc §4 "per-asset manifests, not a central database"). The index *aggregates* them; it never replaces them.
- **Save/player data** (variant CSVs the logistics UI writes, `save.fit`) — runtime-owned, never indexed for tools, never inside mods.
- **ID allocation itself** — bands are declared in each mod's own `package.json`; the index only *detects* collisions. No issuing authority.

## 7. Anti-goals (binding)

- **No authoritative database** — no SQLite, no GUID table, no content-addressable store (reaffirms cook doc §9.1, packaging doc §9.1).
- **Engine never reads the index** — not even optionally, not even "just for the editor build". One consumer class: tools.
- **No write API on the registry** — mutations go through the domain's real writer (editor save, manifest merge, hand edit, cook); the index is rebuilt after.
- **No new ID namespaces** — reuse objTypeNum bands, appearance names, `class:appearanceName` keys; never mint parallel GUIDs that need mapping tables.
- **No loader changes in this lane** — if a domain's grain is too coarse (compbas rows, mc2.fx effects), the registry documents the coarseness; fixing it is a separately-gated engine slice.
- **No registry-driven load order** — layering is `MC2_ACTIVE_MOD` + dependency lists, period.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Index drifts from engine resolution (split-brain — the project's recurring failure class) | parity smoke: `MC2_LOG_FILE_RESOLVE=1` diff vs `providedBy`; index ships only with the parity test green |
| Tools start trusting a stale index (the `.modindex-cache` deep-edit miss, `file.cpp:170-204`) | full `inputs[]` mtime+size check on every read; loud STALE banner; rebuild-inline default |
| Mini-parsers (FIT/CSV) diverge from engine parsing quirks (e.g. `mod.json` flat-extractor precedent, packaging doc §10) | parsers extract identity fields only; anything ambiguous degrades to path+hash granularity; fixture corpus drawn from real stock files |
| objTypeNum band convention ignored by third-party mods | detector-not-enforcer: `mc2mod check` warns on band overlap and on IDs ≥4096; engine already NULL-falls-through safely |
| Whole-file shadow grain causes silent data loss (mod A's compbas row edits vanish under mod B's compbas) | conflicts[] makes it visible with winner/loser; row-level merge stays an explicit future engine slice, never a tool-side patcher |
| Index rebuild cost grows with content (2,947 ASE props, 2,015 cooked manifests) | per-domain shards cached by input-hash; PacketFile walk is header-only; target <2s, measured in CI |

## 9. Phased roadmap

- **P0 — Codify (this doc) + schema:** `docs/registry-index-format.md`, `mc2-registry-index/1` JSON schema, validator in the existing check-script family. No tool behavior change.
- **P1 — `tools/registry/build_index.py` for the path-keyed domains** (missions, appearances-by-name, terrain FITs, effects-file presence, manifests) over base data only.
- **P2 — Mod-chain resolution + conflicts[]** + `MC2_LOG_FILE_RESOLVE` parity smoke; fold into `mc2mod check`.
- **P3 — Deep-ID domains:** object2.pak packet walk → objectTypes shard; compbas.csv rows → components shard; band-overlap detection; `package.json` gains `objTypeBands`.
- **P4 — Tool consumption:** Asset Viewer picker + editor asset browser read the index (with staleness check); launcher mod list reads `conflicts[]`.
- **P5 — Grain improvements (engine-gated, separate lane):** per-packet object2.pak override, compbas row merge, mc2.fx split — each only if a real mod need appears.

## 10. First 5 implementation slices

1. **Schema + validator + fixture:** `docs/registry-index-format.md`, `tools/registry/registry_index.schema.json`, `tools/registry/validate_registry_index.py`, fixture `tests/fixtures/registry/minimal_index.json`; wire into `scripts/check-*` family. Python 3 stdlib only.
2. **`build_index.py` v1 (base, path-keyed domains):** scan a deploy root for missions (`.fit`+`.pak` pairing, packet count, packet-4 non-NUL), appearances (`data/tgl/*.ini` names + `.tgl` presence), terrain texture-list FITs (`MaxTerrainTypes` etc. via a minimal FIT reader), `mc2.fx` presence+hash, all manifests. Gate: index of stock v0.4 deploy validates; spot-check 10 records by hand.
3. **Mod-chain resolution + conflicts:** accept `--mod <id>` chain, mirror `file.cpp` first-wins, emit `providedBy` + `conflicts[]`. Gate: parity script launches mc2.exe with `MC2_LOG_FILE_RESOLVE=1` + two overlapping fixture mods and diffs `[mod-dup]` winners against `conflicts[]` (shares fixtures with packaging slice 5).
4. **Staleness contract:** `inputs[]` recording + `registry check --fresh` (exit nonzero on any mtime/size mismatch); rebuild hooks in `mc2mod install/uninstall` and the Viewer's central-merge post-step.
5. **objectTypes + components shards:** header-only PacketFile walk of `object2.pak` (packet index, size, nonzero) + `compbas.csv` row scan; `objTypeBands` in `package.json` + band-overlap warning in `mc2mod check`. Gate: stock pak yields the expected packet count; a fixture mod declaring an overlapping band triggers the warning.

## 11. Follow-up prompts (Opus/Codex)

1. *"Implement slices 1+2 of `docs/superpowers/strategy/data-ownership-registry-strategy.md` in worktree `.claude/worktrees/nifty-mendeleev`: write `docs/registry-index-format.md`, `tools/registry/{registry_index.schema.json, validate_registry_index.py, build_index.py}` and fixture `tests/fixtures/registry/minimal_index.json`. build_index scans a deploy root for the path-keyed domains (missions .fit/.pak pairing with header-only packet count, data/tgl/*.ini appearance names, terrain texture-list FITs via a minimal FitIni block reader, mc2.fx presence+sha256, mod.json/models.json/manifest.json aggregation). Python 3 stdlib only, read-only, no engine changes. Gate: validator exit 0 on the generated index for `A:/Games/mc2-opengl/mc2-win64-v0.4/`; both check scripts wired into the existing scripts/check-* family."*
2. *"Implement slice 3 (mod-chain resolution + parity) of data-ownership-registry-strategy.md: add `--mod <id>` chain resolution to `tools/registry/build_index.py` mirroring mclib/file.cpp first-wins ordering (active > dep[0..N] > base, data/ relpaths only), emit providedBy and conflicts[]. Add `tests/registry_resolve_parity.py` that runs mc2.exe with `MC2_LOG_FILE_RESOLVE=1` and two overlapping fixture mods (reuse/extend tests/fixtures/mods/) and asserts every [mod-dup] winner matches conflicts[]. Do NOT modify mclib/file.cpp."*
3. *"Implement slice 5 of data-ownership-registry-strategy.md: header-only PacketFile walk (read the packet table from `object2.pak` without decompressing payloads — see mclib/packet.cpp for the on-disk header layout) producing the objectTypes shard, a compbas.csv row scan producing the components shard, an `objTypeBands` advisory field in the mod package.json schema, and a band-overlap + id≥4096 warning in `mc2mod check`. Gate: stock object2.pak packet count matches ObjectTypeManager expectations (code/objtype.cpp:240 open path), and a fixture mod with an overlapping band triggers the named warning."*
