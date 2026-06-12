# Asset Viewer S5 Polish — Design

**Date:** 2026-06-10
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`
**Tool:** `tools/asset_viewer/` (`mc2_asset_viewer`)
**Predecessor:** MC2-ASSET-VIEWER-MOD-WORKBENCH-MVP-1 (shipped + merged to nifty `8a25a57c`)

## North star

> The asset viewer is already a functioning technical tool. S5 makes it
> modder-friendly by replacing manual string fields and one-off exports with
> discoverable rosters, validation-backed panels, and safe reversible manifest
> installation.

Philosophy (same as editor modernization work):
- Expose existing substrate. Do **not** invent a parallel asset system.
- Make manual string fields discoverable and validated.
- Keep export/merge safe and reversible.

## Scope

Five S5 polish items. **All Backend-B** (viewer-local Cook-Torrance preview); zero
engine render path. Backend-A v2 (engine-faithful `ModelPreviewRenderCore`) is a
**separate** sub-project with its own brainstorm/spec/plan after S5 ships.

## Resolved open question

The MVP handoff flagged the appearance-name source as "still open." Resolved:

The override lookup key `appearanceName` (key form `<class>:<appearanceName>`) =
the **`FileName` field inside `data/tgl/{X}.ini`**:
- staticProp: `bdactor.cpp:340` reads `FileName` → `bldgBaseName` → `resolve("staticProp", bldgBaseName)` at `bdactor.cpp:357`.
- tree: `bdactor.cpp:4087` reads `FileName` → `treeBaseName` → `resolve("tree", treeBaseName)` at `bdactor.cpp:4100`.
- Same `.ini` `FileName` source for both classes; class is decided by mission load type, not the appearance.

**Authoritative enumerable roster = unique `FileName` values across `<deploy>/data/tgl/*.ini`.**
This grounds the appearance picker instead of guessing.

## Existing substrate (reuse, do not duplicate)

- `ModelBrowser` / `TglMeshLoader::listTgl()` — filterable `.tgl` archive list (stock picker).
- `MaterialSlots` — texture slot assignment (texture-set panel).
- `OverrideManifest.{h,cpp}` — `WorkbenchOverride{overrideClass, appearanceName, appearanceVerified, sourceRelPath, scale, renderOnly, fallback, lods[]}`, `WorkbenchOverrideLod{lod, sourceRelPath, distance}`, `ValidateRecordRules`, `ToModelsJson`.
- `ModWorkbench` — `bindStock`, `loadOverride`, `record()`, `warnings()`, `revalidate(missingTextures)`, `exportBundle`, `hasBlocking`.
- `ModWorkbenchPanel` — current UI surface (manual `InputText` for stock + appearance key, draft-export button).
- `mclib/model_override_registry.{h,cpp}` — engine-faithful parse/validate/round-trip (central merge authority).
- `runSmoke*` / `--smoke-*` CLI idiom — GL-free backend smokes.

## The five items

### 1. Stock roster picker

Replace `InputText "Stock .tgl"` (`ModWorkbenchPanel.cpp:34`) with embedded
`ModelBrowser`. Click a row → `wb.bindStock(name)`.

- **Reuse** `ModelBrowser` / `TglMeshLoader::listTgl()`. No new scanner.
- UX: show display/base filename in the row; full relative path or source folder
  in a tooltip (disambiguates similarly named `.tgl`s).

### 2. Appearance-key roster

New unit `AppearanceRoster.{h,cpp}` — **read-only, cached, refreshable**.

Build rules:
- Scan `<deploy>/data/tgl/*.ini`.
- Parse `FileName=`; trim quotes + whitespace.
- Normalize case **for dedupe only**; preserve original spelling for display.
- Sorted unique list.
- Expose source `.ini` path + entry count in tooltip/debug.

Panel: filterable combo replaces the manual `InputText "Appearance key"`
(`ModWorkbenchPanel.cpp:72`). Keep free typing. Verified-state is explicit and
derived (no manual checkbox):
- Roster pick → `rec.appearanceVerified = true`.
- Free-typed value → `false`.
- Free-typed value that matches the roster (after refresh / case-normalized
  dedupe) → `true`.

The manual "Appearance key verified" checkbox is removed; state is computed.

### 3. LOD-chain panel

Editor over `rec.lods[]`. v1 table columns:

```
LOD | Source GLB | Distance | Status | Remove
```

Rules:
- Auto-index `lod` by row order.
- Validate distance ascending.
- Validate source exists.
- Validate first LOD distance == 0 (engine expectation) — **WARN if violated**.
- Only LOD0 present → **WARN, not BLOCK**.
- Reorder via Up/Down buttons. **No drag/drop** in v1 unless ImGui makes it trivial.

Per-row Browse selects the GLB; edits feed `ValidateRecordRules` via
`wb.revalidate()`.

### 4. Texture-set panel

Reuse `MaterialSlots` for slot assignment. Collect unresolved slot paths →
`wb.revalidate(missing)` (the `missingTextures` param already exists, currently
always `{}`). Turns the dead validation hook into a real `texture-missing` WARN.

Guardrail: **WARN, not BLOCK**, for missing textures — unless existing validator
rules already hard-block (engine would hard-fail).

### 5. Central manifest merge (last — riskiest)

New unit `CentralManifestMerge.{h,cpp}`. Engine-faithful via
`model_override_registry`.

Flow:
1. BLOCK pre-check (same gate as `exportBundle`).
2. Parse existing `<outRoot>/models.json` via the registry.
3. Splice this record by key: replace same key, preserve all other records.
4. Write `<outRoot>/models.json.bak` first.
5. Atomic write of merged `models.json`.
6. Post-write round-trip (re-parse via registry; refuse/rollback if `resolve()` null).

New button **"Append/Merge to Central models.json"** beside the existing
**"Export Draft Bundle"** button. **Both buttons stay** — draft export = safe
staging; central merge = deliberate install.

Strict acceptance:
```
Before: central has A, B, C
Merge D:           output A, B, C, D
Merge B (replace): output A, new-B, C
Failure:           original models.json intact; .bak exists; error visible
```

### Extra: Record Summary / Health strip

A strip at the top of the workbench that **aggregates existing validation state**
(not a new system):

```
Key: <class>:<appearance>   Stock: <name> ✓/—   Appearance: <name> ✓roster / ⚠free
LODs: N ✓/⚠                 Textures: M missing ⚠ / clean ✓
Validation: X block / Y warn   Export: draft clean / central dirty
```

Note: per the project emoji ban in `docs/critical_inline_rules.md`, the strip uses
ASCII status tokens (`ok` / `--` / `warn` / `[B]`/`[W]` counts), not emoji glyphs.
Helps modders see whether the override is ready.

## Component boundaries

| Unit | Type | Depends on | Responsibility |
|------|------|-----------|----------------|
| `AppearanceRoster` | new `.h/.cpp` | stdlib + filesystem | scan/parse/cache/dedupe `FileName` roster |
| `CentralManifestMerge` | new `.h/.cpp` | `model_override_registry`, `ToModelsJson` | safe reversible splice of central `models.json` |
| Stock picker | folds into `ModWorkbenchPanel` | `ModelBrowser` | pick stock `.tgl` |
| Appearance combo | folds into `ModWorkbenchPanel` | `AppearanceRoster` | pick/free-type appearance + derive verified |
| LOD panel | folds into `ModWorkbenchPanel` (split if file > ~250 lines) | `OverrideManifest` | edit `lods[]` |
| Texture panel | folds into `ModWorkbenchPanel` | `MaterialSlots`, `ModWorkbench::revalidate` | assign slots, feed missing-texture WARNs |
| Health strip | folds into `ModWorkbenchPanel` | `ModWorkbench::warnings/record/hasBlocking` | aggregate display |

If `ModWorkbenchPanel.cpp` grows past ~250 lines, split the LOD + texture panels
into their own `.cpp` units behind small draw functions.

## Error handling

- Roster scan: missing/unreadable `data/tgl` → empty roster + visible "no roster
  (deploy dir set?)" hint; never crash. Malformed `.ini` lines skipped.
- LOD/texture edits: validation surfaces as WARN/BLOCK through existing
  `revalidate`; never throws.
- Central merge: any parse/write/round-trip failure leaves original `models.json`
  untouched, `.bak` present, error string shown. Failure is never silent.

## Testing (GL-free `--smoke-*`)

```
--smoke-appearance-roster
  scan fixture deploy data/tgl/*.ini → assert FileName entries found + deduped
--smoke-lod-edit-validate
  add LOD rows → assert ascending passes; non-ascending warns/blocks as expected;
  first-LOD-distance!=0 warns; LOD0-only warns
--smoke-texture-missing-warn
  assign nonexistent texture → assert texture-missing WARN appears (not BLOCK)
--smoke-central-merge-preserve
  central A/B → merge C → assert A/B preserved + C added;
  merge B → assert B replaced, A/C preserved; round-trip parse succeeds;
  inject write failure → assert original intact + .bak exists
```

Fixtures extend `tests/fixtures/asset_viewer`. Stock/appearance smokes that need
`tgl.fst` / a populated `data/tgl` point at a deploy dir
(`A:/Games/mc2-opengl/mc2-win64-v0.4`) as the bind/stock MVP smokes already do.

## Build / run

Unchanged from MVP. VS2022 `-B build64`, configure with
`-DCMAKE_PREFIX_PATH=A:/Games/mc2-opengl-src/3rdparty/3rdparty`
`-DCMAKE_LIBRARY_PATH=$D/lib/x64` + GLEW dirs (see `tools/asset_viewer/README.md`).
Target `mc2_asset_viewer`; exe
`build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe`; run needs
`build64/RelWithDebInfo` on PATH. Update `README.md` for the new panels.

## Implementation order

Central merge writes shared state → do it after pickers/validation are stable.

```
1. AppearanceRoster backend + --smoke-appearance-roster
2. Replace appearance-key field with roster combo/free-type hybrid (derived verified)
3. Stock roster picker via ModelBrowser
4. Texture missing-warning plumbing (revalidate(missing)) + --smoke-texture-missing-warn
5. LOD-chain panel + --smoke-lod-edit-validate
6. Record/health strip (aggregate)
7. CentralManifestMerge + --smoke-central-merge-preserve (LAST)
```

## Out of scope (deferred)

- Backend-A v2 (engine-faithful render) — separate sub-project.
- UV-V convention empirical verify vs engine importer — needs textured preview
  (lands with Backend-A).
- DDS / Basis-supercompressed KTX2, CPU BC7 transcode, asset cooking.
- Drag/drop LOD reordering.

## Non-regression

10/10 existing smokes (6 workbench + 4 viewer) stay green. tier1 game smokes
unaffected (tool is standalone, links no game render path).
