# Track D — Assimp Mech Importer: Status Snapshot

**Date:** 2026-04-29
**Purpose:** Inventory + status of the modder's-paradise Track D ("Assimp mech importer") work. Read-only research pass; no code changes.

---

## 1. Inventory of all importer artifacts

Two pieces of authored material exist in tree, both in the `nifty-mendeleev` worktree under `docs/superpowers/`:

| Path | Role |
|---|---|
| `docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md` | Design spec, status **Approved** (483 lines). Defines compile-time flag, file resolution / cache, gesture mapping table, INI evolution, node conventions, geometry contract, animation baker contract, and resolved decisions (cache extensions, shared-gesture ownership, arm discovery). |
| `docs/superpowers/plans/2026-04-27-assimp-mech-importer.md` | Implementation plan (1547 lines, 12 tasks + self-review). Targets `mclib/assimp_importer.{h,cpp}`, `mclib/tgl_cook.{h,cpp}`, `tools/mc2_assetcook/`. Includes spec-coverage matrix and self-identified gaps. |
| `docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md` | Roadmap §6 Track D entry that gates this work on Phase B render headroom. |
| `~/.claude/projects/.../memory/modders_paradise_roadmap.md` | Strategic backdrop memory; one-line Track D entry "spec exists." |

There are no exploration notes specific to Track D in `docs/superpowers/explorations/` (the seven existing files are all terrain/shader topics). **There is no prototype branch and no prototype code.** No git branch contains "assimp", "importer", "glb", or "fbx" in its name; `git log --all` produces zero matching commits. No `assimp_importer.*`, `tgl_cook.*`, or `tools/mc2_assetcook/` files exist on disk. `3rdparty/` contains only `cmake/`, `include/`, `lib/` — no Assimp vendoring. No CMakeLists.txt mentions assimp.

State: **fully designed, fully planned, zero code.**

---

## 2. Existing .ase pipeline (the terminus)

Top-level entry points the importer must replace:

- `TG_TypeShape::ParseASEFile(BYTE*, const char*)` — `mclib/tgl.h:614`
- `TG_TypeShape::LoadTGShapeFromASE(const char*)` — `mclib/tgl.h:631`
- `TG_TypeMultiShape::LoadTGMultiShapeFromASE(const char*)` — declared in `mclib/msl.h`, defined in `mclib/msl.cpp`

Callers (every place a mech, building, vehicle, generic actor, or shadow shape is loaded):
- `mclib/mech3d.cpp:277` — mech LODs
- `mclib/bdactor.cpp` (lines 179, 197, 211, 227, 247, 3085, 3108, 3127, 3147, 3167) — buildings & trees, including damage and shadow variants
- `mclib/gvactor.cpp` (lines 147, 165, 179, 194, 346, 357) — vehicles, sensor circles
- `mclib/genactor.cpp:105, 127` — generic actors

Class hierarchy (target structures the importer must populate identically):

- `TG_TypeNode` — base, holds `nodeId[25]`, `parentId[25]`, `nodeCenter`, hierarchy
- `TG_TypeShape` (`mclib/tgl.h:528`) — per-mesh: `numTypeVertices`, `numTypeTriangles`, `numTextures`, `listOfTypeVertices`, `listOfTypeTriangles`, `listOfTextures`, `hotPink/Yellow/Green` palette swap colors, `alphaTestOn`, `filterOn`, plus rendering buffers (`vb_`, `ib_`, `vdecl_`)
- `TG_TypeMultiShape` (`mclib/msl.h:56`) — collection of TG_TypeShapes plus shape-list/parent-id table, the actual loaded model
- `TG_MultiShape` (`mclib/msl.h:232`) — per-instance runtime form
- `TG_AnimateShape` (`mclib/msl.h:570`) — animation: per-node `quat[]` and `pos[]` dense arrays at a fixed frameRate

Gesture table: `MechAnimationNames[MaxGestures+2]` at `mclib/mech3d.cpp:143-172`. 25 gestures + 2 destroyed-state appendices. Index→file-suffix mapping is the source of truth; the spec's table reproduces it for reference but the implementation must read from this engine table.

Binary cache: existing `.tgl` / `.agl` files (legacy ASE-cooked); `SaveBinaryCopy` / `LoadBinaryCopy` at `msl.cpp:182-320` per the plan's task notes.

INI file: `Mech3DAppearanceType::init` (`mech3d.cpp:187`) parses `[Nodes]`, `[Gestures0..24]`, `[FootPrint]`, `[Bounds]`, plus the existing `[TGLData]` (ASE-only, retained as legacy).

---

## 3. Prototype code state

**None.** No headers, no source files, no CMake plumbing, no vendored library, no test asset. The entire Track D body of work is documents.

The implementation plan is task-by-task ready for `superpowers:executing-plans` or `subagent-driven-development` — File Map and per-task checklists are written, including the self-identified gaps in §Self-review (notably the missing `TG_AnimateShape::Clone()` for shared-gesture copy, and missing `BuildTextureList` helper).

---

## 4. Format gotchas — .ase vs .glb / .fbx

These are the load-bearing details the spec already addresses; modders authoring .glb in Blender will not naturally produce them:

1. **Coordinate transform.** ASE is 3DS Max convention (X-right, Y-forward, Z-up); MC2 needs `mc2.x = -ase.x; mc2.y = ase.z; mc2.z = ase.y`. Assimp's built-in axis flags are unreliable — the spec mandates explicit transform in importer code, applied to both positions *and* normals.
2. **UV V-flip.** `v_mc2 = 1.0 - v_assimp`.
3. **Node names ≤ 24 chars.** `TG_TypeShape::nodeId` is `char[25]`. Silent truncation breaks animation binding because `TG_AnimateShape` matches by node name. Spec mandates **error, not truncate**.
4. **Gesture-bearing clip names.** Animations must match the engine's `MechAnimationNames` strings (or one of the documented aliases — `STtoWK` / `Stand_To_Walk` / etc.); a Blender-authored `.glb` with action named "ShoulderShrug" gets dropped with a warning. Case-insensitive but alias-strict.
5. **Hardpoint / FX anchor nodes.** Weapon, smoke, jumpjet, footprint nodes are referenced *by name* from the INI (`WeaponNodeName0..N` etc.). These are not in the geometry file's gift to define — the modder must name nodes in Blender to match the INI section, or edit both. The spec's validator emits warnings (not errors) when these are missing.
6. **Damage states.** `FallBackwardDam` / `FallForwardDam` are loaded as separate animation files in the ASE path (`MaxGestures+2` slots). For .glb, they must be embedded clips with matching names.
7. **Shared-gesture aliasing.** Gestures 9 (Reverse), 10 (StandToReverse), 23 (FallenForward), 24 (FallenBackward) reuse another gesture's animation data. Spec §9.2 resolves this as **owned copy in cache + runtime**, not pointer aliasing — this requires a `TG_AnimateShape::Clone()` method that does not yet exist (gap called out in plan §Self-review).
8. **Hot Pink / Yellow / Green palette swap.** ASE pipeline preserves these as `0x00cbf0ff` / `0x00FEfF91` / `0x000081b6` magic colors used for mech paint. Importer must preserve them; modder textures using these exact RGB values will get swapped at runtime.
9. **Arm meshes.** ASE community convention is separate `<Mech>LeftArm.ASE` / `RightArm.ASE` files. Spec §9.3 prefers embedded `left_arm` / `right_arm` nodes inside one .glb but supports both.
10. **LOD distances.** Cannot be inferred from geometry; remain explicit `[LOD] Distance0/1/2` in INI.
11. **Per-face `renderStateFlags` (bit 0 = backface).** ASE encodes; .glb has no equivalent — spec defaults to 0; double-sided materials in .glb need a convention.

What .glb has that .ase doesn't and that the importer **does not propagate**: skeletal skinning weights, PBR materials, tangent space (computed but stored in normal field), morph targets. Spec §8 explicitly excludes these.

---

## 5. Open design questions

From the spec's resolved/deferred sections plus the plan's self-review:

- **`TG_AnimateShape::Clone()` not yet declared** — needed for shared-gesture owned copy (plan §Self-review).
- **Texture list population** in `ImportGeometryFromFile` — plan task 6 omits the `BuildTextureList(scene, multishape)` helper that walks `aiMaterial::DIFFUSE` to populate `listOfTextures`.
- **Hot-color preservation policy.** Not addressed: does the importer pass through the magic RGB triplet untouched, or remap? Spec implies pass-through but does not state it.
- **Double-sided material → renderStateFlags bit 0.** No convention defined.
- **Skeletal-skinning ingestion.** A modern .glb mech rig will use joints/skin; the spec models per-node animation with absolute quaternions. The conversion (skin → per-node bake) is glossed.
- **Cache invalidation if INI changes.** Cache freshness checks source-file mtime; not the .ini mtime.
- **Mod-tree vs stock-tree resolution.** Roadmap spec puts modder mechs at `mods/<id>/assets/mechs/*.glb`; importer spec uses `tglPath` resolution. Bridging these (file resolver search order across mod overlay) is implicit.
- **Per-mission asset budget.** TGL pool is 500K; new-format mechs may have higher poly counts than ASE — interaction with `cull_gates_are_load_bearing.md` not analyzed.

---

## 6. Proposed minimum-viable first slice

Goal: one MadCat .glb renders in-engine, indistinguishable from stock ASE MadCat.

Scope (a strict subset of the 12-task plan):

1. **Task 1** — CMake plumbing, ENABLE_ASSIMP_IMPORTER option, FetchContent assimp 5.3.1 with FBX+GLTF importers only.
2. **Task 6** (geometry only, no LOD, no shadow mesh, no arms) — `ImportGeometryFromFile` for the primary mesh: vertex+triangle+UV+texture-name extraction, coordinate transform, V-flip, validator (24-char limit + duplicate-name + cyclic-hierarchy hard errors).
3. **Task 9 partial** — parse `[Import] Source=` only.
4. **Task 10 partial** — wire `mech3d.cpp:277` `mechShape[0]` (LOD 0) to call new `LoadFromFile` when a `.glb` exists alongside the `.ini`. Stock ASE path stays default; new path is opt-in by file presence.
5. **No animation, no cache, no LODs, no arms, no shadow mesh.** Leave `mechAnim[i]` null across the board (engine guards). Mech stands frozen at `Stand` pose. This is the proof the geometry pipeline reaches `TG_TypeMultiShape` and renders.

Verification: deploy, load `mc2_01` with a hand-authored MadCat.glb dropped next to `madcat.ini`, see the mech render in stand pose with correct textures and node hierarchy. Then unblock animation work (Tasks 7, 8, plan §Self-review gap fixes).

Defer to slice 2: animation baker, shared-gesture clone, .tglc/.aglc cache, LODs, arms (separate files OR embedded), shadow mesh, offline `mc2_assetcook` tool, `[LOD]` INI section, modder mod-tree resolution.

This first slice lands one renderable .glb mech without any rendering, animation, or cache complexity to debug — minimum viable proof that the format-independence invariant holds.

---

## 7. References

- Spec: `docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md`
- Plan: `docs/superpowers/plans/2026-04-27-assimp-mech-importer.md`
- Roadmap: `docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md` (§6 Track D)
- Engine terminus: `mclib/tgl.h:528` (TG_TypeShape), `mclib/msl.h:56` (TG_TypeMultiShape), `mclib/msl.h:570` (TG_AnimateShape)
- Gesture source of truth: `mclib/mech3d.cpp:143-172` (`MechAnimationNames`)
- ASE call sites to replace: `mclib/mech3d.cpp:277`, `mclib/bdactor.cpp` (10 sites), `mclib/gvactor.cpp` (6 sites), `mclib/genactor.cpp` (2 sites)
- Memory: `~/.claude/projects/A--Games-mc2-opengl-src/memory/modders_paradise_roadmap.md`
- Architectural rule: `~/.claude/projects/A--Games-mc2-opengl-src/memory/stock_install_must_remain_playable.md` (sidecar pattern Track D inherits)
- Cull/pool constraints: `~/.claude/projects/A--Games-mc2-opengl-src/memory/cull_gates_are_load_bearing.md`, `tgl_pool_exhaustion_is_silent.md`
