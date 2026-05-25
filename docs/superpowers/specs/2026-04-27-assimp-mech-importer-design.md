# Assimp-Backed Mech Asset Importer — Design Spec

**Date:** 2026-04-27  
**Status:** Approved  
**Scope:** Format ingestion only. No renderer changes. No visual modernization.

---

## Architectural goal

New asset format support must be independent from visual modernization.

The importer's sole job is to let the game ingest FBX and GLB mech assets and convert them into the same runtime data the legacy ASE pipeline already produces. Once a mech is imported, the rest of the game does not know or care whether it came from ASE, FBX, or GLB. It is simply a mech shape with nodes and animations.

This invariant must hold:

```
FBX/GLB import terminates at the existing TG runtime structures.
Rendering is downstream and unchanged.
```

---

## Layer diagram

```
Source formats
  FBX / GLB / legacy ASE

        ↓  (importer/cooking layer)

  format probe
  Assimp-backed loader
  node validator
  gesture mapper
  animation baker
  binary cache writer

        ↓  (engine runtime asset format)

  TG_TypeMultiShape
  TG_TypeShape
  TG_AnimateShape
  texture handles
  node names

        ↓  (renderer — unchanged by this work)

  legacy renderer
  updated render pipeline
  future graphics work
```

The importer layer is compile-time optional. See Section 2.

---

## Compatibility matrix

| Assets | Renderer | Result |
|---|---|---|
| Legacy ASE + legacy renderer | legacy | stock gameplay ✅ |
| Legacy ASE + updated renderer | updated | visual upgrade ✅ |
| FBX/GLB + legacy renderer | legacy | new mechs, old look ✅ |
| FBX/GLB + updated renderer | updated | new mechs, modern look ✅ |
| Pre-cooked cache + any renderer | any | fast load, no Assimp needed ✅ |

---

## Section 1 — Compile-time flag

```cmake
option(ENABLE_ASSIMP_IMPORTER "Enable FBX/GLB import via Assimp" ON)
```

**When ON:**
- Assimp linked (static, vcpkg or CMake FetchContent, version pinned)
- Game can cook FBX/GLB to native cache at runtime on cache miss
- Optional offline cook tool (`mc2_assetcook`) shares the same cooking code path

**When OFF:**
- Assimp not linked
- Game loads pre-cooked native cache files only
- Legacy ASE path still works regardless of this flag

This keeps the Assimp dependency from infecting the whole project and supports stripped-down or ported builds.

---

## Section 2 — File resolution and cache

### Resolution order

For any asset name (e.g. `"madcat"`), the loader proceeds as follows:

1. **Discover best available source:**
   - `madcat.glb` (preferred new format)
   - `madcat.fbx` (secondary new format)
   - `madcat.ase` (legacy fallback)
   - If none found: error

2. **Check whether a native cache exists and is fresh** for that source:
   - Cache file: `madcat.tgl` / `madcat.agl`
   - Cache is considered fresh only if **all** of the following hold:
     - Cache file exists
     - Cache modification time ≥ source file modification time
     - Cache contains a matching importer version stamp
     - Cache contains a matching cache format version stamp
   - If any condition fails, the cache is stale and must be re-cooked

3. **If cache is fresh:** load cache directly, skip import

4. **If cache is missing or stale and `ENABLE_ASSIMP_IMPORTER=ON`:**
   - Run importer/cooker on the source file
   - Write cache
   - Load from newly written cache

5. **If legacy ASE source and no cache:** use existing ASE parse path unchanged

### Version stamps

The cache binary must include:
- `uint32_t importer_version` — bumped on any change to import logic
- `uint32_t cache_format_version` — bumped on any structural change to TG binary layout
- `uint32_t coord_convention_version` — bumped if coordinate transform rules change
- `uint32_t anim_bake_version` — bumped if animation baking algorithm changes

Cache is unconditionally invalidated if any stamp does not match the current engine values.

### Cook-at-startup vs offline tool

Cook-at-startup is the primary modder-friendly path. The cooking code lives in a shared library/module so an optional `mc2_assetcook <file>` CLI can call the same logic without running the game.

---

## Section 3 — Gesture mapping

### Source of truth

Gesture IDs are defined by the existing engine gesture table in `mclib/mech3d.cpp:143` (`MechAnimationNames[]`) and the comment-annotated gesture enum. The spec records the verified table below for reference only. The importer code must derive IDs from the engine table, not from this document.

### Verified gesture table

Source: `MechAnimationNames[MaxGestures+2]` at `mech3d.cpp:143`.

| Index | Engine name suffix | Canonical clip alias | Notes |
|---|---|---|---|
| 0 | `StandToPark` | `StandToPark` | GesturePark |
| 1 | `ParkToStand` | `ParkToStand` | |
| 2 | *(empty)* | `Stand` | No animation file; stand is a held pose |
| 3 | `STtoWK` | `StandToWalk` | |
| 4 | `Walk` | `Walk` | |
| 5 | `StandToPark` | `StandToPark` | GestureStandToPark — reuses gesture 0 file |
| 6 | `WKtoRN` | `WalkToRun` | |
| 7 | `Run` | `Run` | |
| 8 | `RNToWK` | `RunToWalk` | |
| 9 | `Walk` | `Reverse` | Reuses Walk file; played reverse via GestureData |
| 10 | `WKtoST` | `StandToReverse` | Reuses WKtoST file |
| 11 | `LimpLeft` | `LimpLeft` | |
| 12 | `LimpRight` | `LimpRight` | |
| 13 | `Idle` | `Idle` | |
| 14 | `FallBackward` | `FallBackward` | |
| 15 | `FallForward` | `FallForward` | |
| 16 | `HitFront` | `HitFront` | |
| 17 | `HitBack` | `HitBack` | |
| 18 | `HitLeft` | `HitLeft` | |
| 19 | `HitRight` | `HitRight` | |
| 20 | `Jump` | `Jump` | |
| 21 | `GetupBack` | `GetUpBack` | Getup from FallBackward |
| 22 | `GetupFront` | `GetUpFront` | Getup from FallForward |
| 23 | `FallForward` | `FallenForward` | Single-frame held pose; reuses FallForward file |
| 24 | `FallBackward` | `FallenBackward` | Single-frame held pose; reuses FallBackward file |
| +1 | `FallBackwardDam` | `FallBackwardDam` | Destroyed state; loaded separately |
| +2 | `FallForwardDam` | `FallForwardDam` | Destroyed state; loaded separately |

**Shared-file gestures (9, 10, 23, 24):** when importing GLB, these gestures reuse the animation data from their source gesture (Walk, WKtoST, FallForward, FallBackward respectively). The importer aliases the pointer rather than duplicating animation data. The `GestureData.reverse` flag in the INI controls reverse playback for gestures 9 and 10.

### Clip name matching rules

- Matching is **case-insensitive**
- **Aliases are supported.** Each canonical name has a set of accepted aliases:

  | Canonical | Accepted aliases |
  |---|---|
  | `StandToWalk` | `STtoWK`, `ST_TO_WK`, `Stand_To_Walk` |
  | `WalkToStand` | `WKtoST`, `WK_TO_ST`, `Walk_To_Stand` |
  | `WalkToRun` | `WKtoRN`, `WK_TO_RN`, `Walk_To_Run` |
  | `RunToWalk` | `RNToWK`, `RN_TO_WK`, `Run_To_Walk` |
  | `GetUpBack` | `GetupBack`, `Getup_Back` |
  | `GetUpFront` | `GetupFront`, `Getup_Front` |
  | `FallenForward` | `FallForwardPose` |
  | `FallenBackward` | `FallBackwardPose` |

  Additional aliases can be added without changing the importer version stamp, since they only affect clip discovery.

### Missing and unknown clips

- **Missing gesture (no matching clip found):** allowed. `mechAnim[i]` is set to null. Engine already guards on null in animation playback. Log at info level: `[importer] gesture Walk: no clip found, mechAnim[4] = null`.
- **Unknown clip name (clip exists in file but matches no gesture):** emit a warning log. Do not silently discard. `[importer] warning: clip "ShoulderShrug" in madcat.glb does not match any known gesture alias — skipped`. This surfaces modder typos.

---

## Section 4 — INI evolution

The INI is retained for all gameplay metadata that cannot be derived from geometry. File references become implicit for new-format assets; gameplay bindings remain explicit.

### Fields kept for all assets

```ini
[Nodes]
NumSmoke, NumWeapon, NumJumpjet, NumFeet
SmokeNodeName0..N
WeaponNodeName0..N, WeaponType0..N
JumpNodeName0..N
FootNodeName0..N

[Gestures0..24]
StartVel, EndVel, StartFrame, Reverse
LeftFootDown0/1, RightFootDown0/1

[FootPrint]
LeftFootprintType, RightFootprintType

[Bounds]
UpperLeftX/Y, LowerRightX/Y
```

### Fields changed for new-format assets

**LOD distances — kept explicit, new optional section:**

```ini
[LOD]
Distance0=0
Distance1=250
Distance2=500
```

The engine cannot reliably infer LOD transition distances from geometry. These remain explicit. If `[LOD]` is absent, engine defaults apply.

**File references — implicit by convention, overrideable:**

For new-format assets (GLB/FBX), source file and mesh node names are discovered automatically. An optional `[Import]` section allows overrides for non-standard naming:

```ini
[Import]
Source=madcat.glb          ; explicit source file if name doesn't match mech name
ShadowNode=shadow_mesh     ; explicit shadow mesh node name
LOD0=MadCat_LOD0           ; explicit mesh node names per LOD level
LOD1=MadCat_LOD1
LOD2=MadCat_LOD2
```

If `[Import]` is absent, the importer uses conventions (see Section 5 — Node conventions).

**Legacy `[TGLData]` section:** used only for ASE assets. Ignored for GLB/FBX.

---

## Section 5 — Node conventions and validator

### Discovery conventions (GLB/FBX, no `[Import]` override)

| Asset | Convention |
|---|---|
| Primary mesh (LOD0) | Root-level mesh node or node named `{mechname}` or `{mechname}_LOD0` |
| LOD1, LOD2 | Nodes named `{mechname}_LOD1`, `{mechname}_LOD2` |
| Shadow mesh | Node named `shadow`, `{mechname}_shadow`, or `{mechname}X` |
| Arms | Nodes named `{mechname}_LeftArm`, `{mechname}_RightArm` |

### Hard validation errors (abort cook, do not write cache)

| Condition | Reason |
|---|---|
| No renderable mesh found | Nothing to import |
| Duplicate node names | Breaks node ID lookup and animation binding |
| Node name exceeds 24 characters | `TG_TypeShape::nodeId` is `char[25]`; truncation would silently break animation matching |
| Parent ID references non-existent node | Broken hierarchy |
| Cyclic hierarchy | Would infinite-loop during matrix chain traversal |
| Mesh has no vertices or no faces | Importer cannot recover |
| Mesh references material index with no texture data | Required for render |

Silent truncation of node names is explicitly prohibited. The importer must error, not truncate.

### Warnings (cook proceeds, issues logged)

| Condition |
|---|
| Missing `weapon_*` nodes — weapon fire points will not work |
| Missing `dust_lfoot` / `dust_rfoot` — footstep effects will not trigger |
| Missing `smoke_torso` — damage smoke will not appear |
| Missing `jumpjet_*` nodes — jump jet effects will not appear |
| Missing shadow mesh — existing engine behavior applies |
| Missing optional gestures — `mechAnim[i]` = null, already handled |
| Unknown animation clip names — surfaces modder typos (see Section 3) |
| Missing texture file on disk — logged with path, render will use placeholder |

---

## Section 6 — Geometry importer contract

The importer must populate these structures identically to what `ParseASEFile` produces. The renderer must not be able to distinguish the source.

### Per `TG_TypeShape`

| Field | Type | Requirement |
|---|---|---|
| `nodeId` | `char[25]` | Node name, max 24 chars + null |
| `parentId` | `char[25]` | Parent node name; `"None"` if root |
| `nodeCenter` | `Point3D` | Node pivot in MC2 coordinate space |
| `listOfTypeVertices[n].position` | `Point3D` | Relative to nodeCenter, MC2 coords |
| `listOfTypeVertices[n].normal` | `Vector3D` | Vertex normal (from Assimp `aiProcess_GenSmoothNormals`) |
| `listOfTypeVertices[n].aRGBLight` | `DWORD` | Initialize to `0xff000000` |
| `listOfTypeTriangles[n].Vertices[3]` | `DWORD[3]` | Indices into vertex list |
| `listOfTypeTriangles[n].uvdata` | 6 floats | U0,V0,U1,V1,U2,V2 — V must be `1.0 - v` |
| `listOfTypeTriangles[n].localTextureHandle` | `DWORD` | Index into shape's texture list |
| `listOfTypeTriangles[n].faceNormal` | `Vector3D` | Face normal |
| `listOfTypeTriangles[n].renderStateFlags` | `DWORD` | Bit 0 = backface |
| `listOfTextures[n].textureName` | `char[]` | Filename without extension |
| `listOfTextures[n].textureAlpha` | `bool` | Has alpha channel |

### Coordinate transform

ASE files use 3DS Max convention (X-right, Y-forward, Z-up) and `ParseASEFile` applies:

```
mc2.x = -ase.x
mc2.y =  ase.z
mc2.z =  ase.y
```

The Assimp importer must apply the same transform to all positions and normals so the output is in MC2 space. Assimp import flags alone do not reliably produce this mapping — apply it explicitly in the loader code.

UV V-flip: `v_mc2 = 1.0 - v_assimp`

### Assimp post-process flags (recommended)

```cpp
aiProcess_Triangulate          // required: MC2 is triangles only
aiProcess_GenSmoothNormals     // if normals absent in source
aiProcess_CalcTangentSpace     // free; stored in normal field for future use
aiProcess_JoinIdenticalVertices
aiProcess_ValidateDataStructure
aiProcess_SortByPType          // isolate triangles from other primitive types
```

---

## Section 7 — Animation baker contract

### Input (Assimp)

`aiAnimation` contains `aiNodeAnim` channels per node. Each channel has sparse keyframe arrays:
- `mPositionKeys[k]` — `{time, aiVector3D}`
- `mRotationKeys[k]` — `{time, aiQuaternion}`

Keyframe times are in animation ticks. `aiAnimation::mTicksPerSecond` converts to seconds.

### Output (TG_Animation)

```cpp
struct TG_Animation {
    char   nodeId[25];
    DWORD  numFrames;
    float  frameRate;          // frames per second
    float  tickRate;           // ticks per frame
    UnitQuaternion *quat;      // dense array [numFrames], absolute orientations
    Point3D        *pos;       // dense array [numFrames], or NULL if no translation
};
```

### Baking algorithm

For each integer frame index `f` in `[0, numFrames)`:

1. Compute time `t = f / frameRate` in animation ticks
2. Find surrounding keyframes `k_lo`, `k_hi` such that `k_lo.time <= t <= k_hi.time`
3. Compute blend factor `alpha = (t - k_lo.time) / (k_hi.time - k_lo.time)`
4. Slerp: `quat[f] = slerp(k_lo.quat, k_hi.quat, alpha)`
5. Normalize: `quat[f].normalize()`
6. If position channel exists: `pos[f] = lerp(k_lo.pos, k_hi.pos, alpha)` in MC2 coords

Edge cases:
- `t` before first keyframe: use first keyframe value
- `t` after last keyframe: use last keyframe value
- Single keyframe: replicate across all frames

**Note:** This replaces the current ASE path's cumulative delta-quaternion encoding with absolute orientations baked at load time. The output `quat[]` array is identical in layout; only the values differ (absolute vs. the ASE path's accumulated product). Both produce the same result at runtime because `TransformMultiShape` consumes absolute quaternions.

---

## Section 8 — What is explicitly out of scope

The following are not part of this importer work and must not be introduced by it:

- Changes to `TG_Shape::Render()` or any render path
- New shader features, uniforms, or render state
- PBR, normal mapping, or lighting model changes
- GPU skinning or GPU-side vertex transform
- Changes to the shadow pipeline
- Changes to particle effects or gosFX
- Changes to the existing ASE/TGL binary cache format version (unless forced by a bug)
- Generated simplified shadow meshes from main geometry (not in scope; existing engine behavior applies when shadow mesh is absent)

---

## Section 9 — Resolved decisions

### 9.1 Cache file extensions

**Decision: separate cooked cache extensions `.tglc` / `.aglc`.**

Legacy ASE-generated caches retain `.tgl` / `.agl` unchanged. New Assimp-cooked caches use `.tglc` / `.aglc`. Both load into the same `TG_TypeMultiShape` / `TG_AnimateShape` runtime structures — the extension difference is filesystem-visible only.

Cooked cache header (`.tglc` / `.aglc`):

```
uint32  magic                    (distinct value from .tgl/.agl)
uint32  cache_format_version
uint32  importer_version
uint32  source_format            (GLB=1, FBX=2)
char[]  source_path
uint64  source_timestamp
uint8[16] source_hash            (optional MD5/xxHash of source file)
uint32  assimp_version
uint32  coord_conversion_version
uint32  anim_baker_version
```

This makes bug reports actionable: "cooked from `MadCat.glb`, importer version 3, coord-convention v2."

### 9.2 Shared-file gesture ownership

**Decision: copy into owned runtime slots. No pointer aliasing at cache/runtime layer.**

Gestures 9 (Reverse), 10 (StandToReverse), 23 (FallenForward), and 24 (FallenBackward) reuse animation data from other gesture slots in the ASE path. The cooker may recognize these as aliases internally, but the cache and runtime always materialize each populated gesture slot as its own fully owned `TG_AnimateShape` with its own `quat[]` / `pos[]` arrays.

Rationale: aliasing infects ownership semantics (who frees the arrays? can the reverse-playback flag mutate shared state?), complicates cache serialization, and saves negligible memory on 25-year-old animations. Copy is safe, simple, and consistent with what the ASE path already does implicitly via separate files.

### 9.3 Arm mesh discovery

**Decision: GLB embeds arms as named nodes (preferred); separate files supported for FBX and optional GLB compatibility.**

**GLB preferred convention — single-file package:**

```
madcat.glb
  root
    body / torso / legs / ...
    left_arm                  (discovered by name or [Import] LeftArmNode=)
    right_arm
    shadow
    LOD0 / LOD1 / LOD2 nodes
    animation clips
```

**FBX compatibility — separate arm files (existing community assets):**
```
MadCatLeftArm.FBX
MadCatRightArm.FBX
```

**Optional GLB override — separate arm files:**
```
madcat_leftarm.glb
madcat_rightarm.glb
```

INI `[Import]` section supports explicit overrides for all cases:

```ini
[Import]
Source=madcat.glb
LeftArmNode=left_arm          ; embedded node name
RightArmNode=right_arm
ShadowNode=shadow
; or, for separate file overrides:
LeftArmSource=madcat_leftarm.glb
RightArmSource=madcat_rightarm.glb
```

If neither `LeftArmNode` nor `LeftArmSource` is specified, the importer tries conventional node names (`left_arm`, `{mechname}_LeftArm`) in the primary source file before looking for a separate file.
