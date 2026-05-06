# GPU Mech Offload Recon — 2026-05-03

**Branch:** `claude/nifty-mendeleev`
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Scope:** Mech-specific pipeline characterization for a future GPU mech draw arc.
**NOT in scope:** Slice 2 execution (buildings/trees/generics GPU lighting — already in flight);
vehicles (GVAppearance); Carver5O/Magic/MCO/Wolfman mod content.

---

## Slice summary

MC2 mechs use **rigid per-node animation** — each sub-mesh (torso, arms, legs, weapon
mounts) is a separate `TG_Shape` with its own `shapeToWorld` matrix composed each frame via
`TransformMultiShape`'s hierarchy traversal. This is NOT vertex-level skinning; the word
"joint" in `mech3d.cpp` refers only to scene-graph node-name strings, not vertex-level data.

The current pipeline has **zero vertex-stage skinning fields** (Section A, confirmed).
The skinning-ready vertex format (boneIdx + weights) for a future GPU mech draw arc requires
**no `.tgl` cache re-authoring for existing stock MC2 content** — bone indices can be
synthesized deterministically at registration time from the current per-node shape hierarchy,
and UV and tangent data can be derived at registration time without modifying the on-disk
format. However, this "no re-authoring" claim is bounded to the stock/rigid-degenerate path:
**future true-skinned content (Track D UE5/MW5 imports) still requires a defined persistent
representation** for bone indices, weights, and tangents — either a new GPU-side cache format
or a sidecar skinning-attribute file keyed to the TGL asset. The brainstorm/spec must choose
one of these options explicitly (see Section F — Persistent format decision).

Mechs are **completely outside** the current slice 1/slice 2 object-offload arc.
`Mech3DAppearance::render` reads only the legacy `g_useGpuStaticProps` flag (not `g_useGpuObjects`).
The slice 2 `TransformMultiShape_PositionsOnly` / `CacheGpuLightData` infrastructure is built
exclusively for buildings/trees/generics; mechs do not enter that path.

**Section G (slice 2 cross-cut) is the highest-leverage finding:** no blocking vertex-format
conflict with slice 2 was found. The mech arc designs a separate type VBO and batcher; the
two formats live in separate GL objects and are never mixed. The arcs can proceed without
blocking each other. However, slice 2 establishes shader-include conventions (`lighting.hglsl`,
`calc_light()` ABI), object-record layout patterns (SSBO per-instance struct), and texture
binding patterns that the mech arc should track to avoid a second divergent GPU-object
architecture. "No blocking conflict" is more precise than "fully independent."

---

## Section A: Negative-claim verification — "no vertex-stage skinning fields exist"

**Claim under review:** "Per-vertex transform happens on CPU each frame. Output is a
transformed vertex stream sent to GPU. The vertex format only needs to carry post-transform
position+normal — there's no concept of bone weights because skinning never reaches the
vertex stage."

Per `memory/feedback_data_flow_audit_asymmetry.md`, a negative claim requires grep'ing the
**candidate consumers**, not just the obvious source. The consumer search was:

### Consumers searched

| File | Pattern | Result |
|------|---------|--------|
| `mclib/mech3d.cpp` | `weight\|bone\|skin\|joint` (case-insensitive) | "joint" only, in node-name strings |
| `mclib/gvactor.cpp` | same | 0 matches |
| `mclib/tgl.cpp` | same | 0 matches |
| `mclib/tgl.h` | same | 0 matches |
| `mclib/msl.cpp` | same | "joint_xUARM" in one node-name-comparison comment |

### Vertex struct examination

**`TG_TypeVertex`** (`mclib/tgl.h:35-42`) — the per-type (static) vertex:
```c
typedef struct _TG_TypeVertex {
    Stuff::Point3D  position;  // local-space position
    Stuff::Vector3D normal;    // vertex normal
    DWORD           aRGBLight; // vertex light and alpha
} TG_TypeVertex;
```
Fields: position + normal + aRGBLight. No UV (UV is per-triangle in `TG_UVData`, tgl.h:104-116).
No bone indices. No weights. No tangent.

**`TG_HWTypeVertex`** (`mclib/tgl.h:49-58`) — hardware variant with UV:
```c
typedef struct _TG_HWTypeVertex {
    Stuff::Point3D  position;
    Stuff::Vector3D normal;
    DWORD           aRGBLight;
    float           u, v;
} TG_HWTypeVertex;
```
Still no bone data.

**`gos_VERTEX`** (`GameOS/include/gameos.hpp:2147-2152`) — the runtime pool vertex:
```c
{
    float x, y;    // screen coords
    float z;       // depth (0..0.99999)
    float rhw;     // 1/w
    DWORD argb;    // vertex color and alpha
    DWORD frgb;    // specular/fog
    float u, v;    // texture coordinates
}
```
Post-transform, post-lighting, screen-space. No bone data. 32 bytes.

**TGL cache serialization** (`mclib/tgl.cpp:619-623`):
```cpp
binFile.writeInt(numTypeVertices);
if (numTypeVertices)
    binFile.write((MemoryPtr)listOfTypeVertices, sizeof(TG_TypeVertex) * numTypeVertices);
```
Confirmed: `.tgl` files serialize `TG_TypeVertex` — position + normal + aRGBLight only.
No bone indices, no UV (UV lives in `TG_TypeTriangle::uvdata`).

### "joint" references in mech3d.cpp

All 12 matches of "joint" in `mech3d.cpp` are scene-graph node-name strings:
- `"joint_torso"` — SetNodeRotation call (torso twist)
- `"joint_ruarm"` — StopUsing/isChildOf/GetNodeNameId (right arm)
- `"joint_luarm"` — StopUsing/isChildOf/GetNodeNameId (left arm)
- `"joint_root"` — GetNodeNameId (root node for height calculation)

These are string identifiers for nodes in the scene-graph hierarchy — not vertex-level data
structures. They are used only for `SetNodeRotation`, `GetNodeNameId`, `StopUsing`, and
`isChildOf` operations that manipulate the scene-graph transform tree. They have no
vertex-format representation.

### Verdict: negative claim STANDS

The current pipeline has **zero vertex-stage skinning fields** at any level:
- `TG_TypeVertex` (static geometry): no bone data
- `gos_VERTEX` (runtime pool): no bone data
- `.tgl` cache format: no bone data
- All consumer files (mech3d.cpp, gvactor.cpp, tgl.cpp, tgl.h, msl.cpp): no weight/bone/skin
  vertex-stage symbols

The "joint" terminology in mech3d.cpp refers exclusively to scene-graph node names used for
setting per-node rotation quaternions on the rigid-body hierarchy. This is a software
scene-graph abstraction, not GPU skinning.

---

## Section B: Current CPU mech pipeline — per-frame data flow

### Entry point: update gate (mech3d.cpp:4183)

```cpp
if ((turn < 3) || inView || (currentGestureId == GestureJump) || g_useGpuStaticProps)
    updateGeometry();
```

`updateGeometry` at `mech3d.cpp:3000` runs **only for cull survivors** (inView=true) or on
the first 3 frames, or for the jump gesture (needs continuous update), or when the legacy
killswitch is on. Out-of-view mechs skip `updateGeometry` entirely → stale `listOfVertices`
→ `TG_Shape::Render` silently early-outs (tgl.cpp:2560-2567).

### `updateGeometry` body (mech3d.cpp:3000-3199)

Order of operations:
1. **Texture handle setup** (mech3d.cpp:3003-3010): sets `localTextureHandle` on `mechShape`,
   rightArm, leftArm — per-frame rewrite per `mc2_texture_handle_is_live.md`.
2. **Status-based lightsOut** (mech3d.cpp:3012-3021): destroyed/disabled → LightsOut=true.
3. **Flash timer** (mech3d.cpp:3029-3042): damage/selection flash timing.
4. **Lighting parameters** (mech3d.cpp:3052-3102):
   - `land->getTerrainLight(position)` → per-mech light intensity
   - `eye->getLightRed/Green/Blue(intensity)` → RGB components
   - `eye->setLightColor(0, lightRGB)` + `setLightIntensity(0, 1.0)` — writes to the shared
     world-light list entry 0
   - Fog factor calculation from position.y vs fogStart/fogFull
   - `mechShape->SetFogRGB(fogRGB)` + `SetLightList(eye->getWorldLights(), eye->getNumLights())`
5. **Point light** (mech3d.cpp:3106-3157): night-mode spotlight attached to SLCircle_anubis node.
6. **Animation state** (mech3d.cpp:3164-3173):
   - `mechType->setAnimation(mechShape, currentGestureId)` — binds keyframe data to nodes
   - `mechShape->SetFrameNum(currentFrame)` — sets animation frame
   - Same for `mechShadowShape` if present
7. **Torso rotation** (mech3d.cpp:3176-3195):
   - `mechShape->SetNodeRotation("joint_torso", &torsoRot)` — per-frame torso twist quaternion
   - Same for mechShadowShape
8. **`TransformMultiShape`** (mech3d.cpp:3199):
   - Full hierarchy composition + per-vertex lighting bake
   - Output: per-node `shapeToWorld` matrices + `listOfVertices[j].argb` baked per-vertex

### `TransformMultiShape` internals (msl.cpp:1341-1730)

For each child node in `numTG_Shapes`:
1. **Texture handle rewrite** (msl.cpp:1378-1382): per-texture-per-child SetTextureHandle.
2. **Hierarchy composition** (msl.cpp:1384-1549): traverse parent chain via `childChain[]`,
   apply keyframe quaternion slerp + position data, build `shapeToWorld` per node.
   Keyframe data: `currentAnimation->quat[fNum]` + `currentAnimation->pos[fNum]` (frame-indexed).
3. **Lighting setup per node** (msl.cpp:1552-1694): per-light-type transform into shape space
   (AMBIENT, INFINITE, INFINITEWITHFALLOFF, POINT, SPOT, TERRAIN).
4. **Per-leaf dispatch** (msl.cpp:1705-1712):
   - `MultiTransformShape` (full path): calls `TG_Shape::TransformShape` per leaf
   - `MultiTransformShape_PositionsOnly` (positions-only path, flag-gated): skips lighting bake
5. **Shadow transform** (msl.cpp:1714-1717): `MultiTransformShadows` for shadow vertices.

### `TG_Shape::TransformShape` (per-vertex kernel)

Called per leaf TG_Shape. Per prior brainstorm (tgl.cpp:1687-1690):
- Allocates `listOfVertices`, `listOfColors`, `listOfShadowTVertices` from TGL pools
- Transforms each vertex: local → world via `shapeToWorld` → screen via `s_worldToClip`
- Per-vertex lighting evaluation against the active light list
- Bakes result into `listOfVertices[j].argb` (gos_VERTEX.argb)

### What flows to GPU each frame

Currently all per-mech rendering goes through `TG_Shape::Render` (legacy CPU submit path):
- Calls `mcTextureManager->addVertices` with the post-transform `gos_VERTEX` array
- `mcTextureManager` batches these for the `Render.3DObjects` pass in `txmmgr.cpp`

Static (uploaded once at mission load): nothing — mechs have no GPU-resident geometry today.
Dynamic (every frame via CPU path): the full post-transform `gos_VERTEX` array.

---

## Section C: Cull-cascade integration points

These are the exact gates a GPU mech draw path must preserve or explicitly handle.

### Gate 1: objBlockInfo activation (outer iteration loop)

**File:line:** `code/objmgr.cpp:1491` (render), `code/objmgr.cpp:1758` (update)
**What it does:** `Terrain::objBlockInfo[terrainBlock].active` gates whether the entire block
is iterated at all. Set by terrain-vertex cull at `mclib/terrain.cpp:1610-1611`:
```cpp
setObjBlockActive(currentVertex->getBlockNumber(), true);  // terrain.cpp:1629
objVertexActive[vertNum] = true;                           // terrain.cpp:1483
```
**Consequence of bypass:** Commit `b4cc927` tried to force all blocks active, causing
`Building::update`-style state checks to fail → `MC2_DESTROY(...)` at `objmgr.cpp:1775`.
Objects permanently destroyed for rest of mission.

**GPU path must:** NOT bypass the outer active-block iteration. `Mech3DAppearance::render`
is called from WITHIN this iteration loop — the GPU path inherits the same gate.

### Gate 2: inView (per-mech update gate)

**File:line:** `mclib/mech3d.cpp:4183`
```cpp
if ((turn < 3) || inView || (currentGestureId == GestureJump) || g_useGpuStaticProps)
    updateGeometry();
```
**What it gates:** `updateGeometry()` → `TransformMultiShape` → fresh `listOfVertices` + 
`shapeToWorld`.
**Consequence of bypass:** stale `listOfVertices` → `TG_Shape::Render` silent early-out.

**GPU path must:** Ensure `updateGeometry()` runs for all mechs that the GPU path draws.
For the per-node-rigid degenerate case, the GPU path needs per-node `shapeToWorld` matrices
freshly computed each frame. These come from `TransformMultiShape`. If the GPU path calls
`mechShape->Render(false)` for submission, inView being false would cause Render to early-out
before reaching the GPU submit path. The GPU path needs to gate on "was updateGeometry called
this frame" rather than "inView at render time".

**Current g_useGpuStaticProps bypass note:** The existing `|| g_useGpuStaticProps` clause
was added (commit `aef7e14`) to keep `updateGeometry` running even when the broken angular
cull sets inView=false. A GPU mech path would need a similar mechanism — either a
`g_useGpuMechs` clause OR separating `updateGeometry` from the inView gate entirely.

### Gate 3: canBeSeen / Building-render gate

**File:line:** `code/bldng.cpp:1071` (buildings), equivalent for mechs is the `inView` gate
at `mclib/mech3d.cpp:2377`:
```cpp
if (inView || g_useGpuStaticProps)
{
    // ... render mech
    mechShape->Render(true);
    // ...
}
```
**What it gates:** Whether `Render()` is even called for this mech.

**GPU path must:** Call the GPU submit path unconditionally (or based on "was updateGeometry
called") rather than gating on `inView`. The submission itself must be independent of the
render-path cull.

### Gate 4: objVertexActive (pool budget proxy)

**File:line:** `code/objmgr.cpp:1491-1511`
**What it is:** Per-vertex active flag from the terrain-vertex activation cascade.
**Pool implication:** The 500K vertex pool is sized for the vanilla cull-survivor population.
A GPU path that calls `TransformMultiShape` for all mechs regardless of cull doubles the
pool consumption (only in-view mechs currently run `TransformShape`).

**GPU path must:** Either (a) use the same cull-survivor set (Q3=(a) from brainstorm) so
pool consumption is unchanged, or (b) own a separate pool not shared with the CPU path.

---

## Section D: TGL pool interaction

### Current allocation pattern (per brainstorm)

`TG_Shape::TransformShape` allocates from three pools per leaf:
- `vertexPool->getVerticesFromPool(numVertices)` — stores in `listOfVertices`
- `colorPool->getVerticesFromPool(numVertices)` — stores in `listOfColors`
- `shadowPool->getVerticesFromPool(numVertices)` — stores in `listOfShadowTVertices`

Current sizes (`code/mission.cpp:3140-3152`, verified in brainstorm):
```cpp
colorPool->init(500000);
vertexPool->init(500000);
facePool->init(200000);
shadowPool->init(500000);
trianglePool->init(200000);
```

### Per-mech pool cost

A typical mech has ~10-20 child TG_Shape nodes (torso + arms + legs + weapons). Each node
has its own `numVertices`. Ballpark: ~200 verts per mech (from brainstorm Q0: avg 200 verts
per instance). With 20-40 mechs per mission: ~4K-8K vertices from mechs alone.

### Exhaustion canary

From `memory/tgl_pool_exhaustion_is_silent.md`: `getVerticesFromPool` returns NULL silently.
`TG_Shape::Render` (tgl.cpp:2560-2567) early-outs on null `listOfVertices`. Render order:
buildings allocate first, mechs iterate last → mechs are the canary for pool exhaustion.

### GPU path pool design

Option A (Q3=(a) from brainstorm): CPU cull unchanged, GPU path draws exactly the cull-survivor
set. Pool consumption identical to today. Safest — pool peak gate (Gate E) passes unchanged.

Option B (separate GPU pool): GPU path allocates from its own pool, CPU path unchanged.
Allows rendering uncull'd mechs without exhausting the shared pool. Higher complexity;
requires a second pool for per-node matrix data or a different data structure entirely.

**Recommendation:** For the initial GPU mech arc, Q3=(a) — CPU cull unchanged — is the
correct seam choice, exactly as the static-prop brainstorm concluded. The GPU path draws the
same mechs the CPU path would have drawn; the difference is HOW they're drawn (SSBO-driven
instanced draw per node vs. per-vertex CPU submit). This sidesteps all pool-related risks.

---

## Section E: Texture binding for mechs

### Per-mech texture complexity

Mechs have a more complex texture story than buildings:

1. **localTextureHandle** (`mclib/mech3d.cpp:2369`): per-instance paint scheme texture handle,
   set on `mechShape` AND `rightArm` AND `leftArm` at the start of `updateGeometry` AND
   at the start of `render`. This is a per-instance, per-frame rewrite per
   `mc2_texture_handle_is_live.md`.

2. **SetTextureHandle per-child per-texture** (`mclib/msl.cpp:1378-1382`): inside
   `TransformMultiShape`, every child node gets every texture handle rewritten.

3. **ARM-specific handles** (mech3d.cpp:2369-2375): rightArm and leftArm share
   `localTextureHandle`. Detached arms (rightArmOff, leftArmOff) continue to render as
   separate instances.

### Implication for GPU path

The per-instance `localTextureHandle` is the paint scheme. A GPU mech renderer needs to:
- Store `textureSlot` (not handle) in the per-instance SSBO per the `mc2_texture_handle_is_live`
  fix pattern.
- Resolve handle at draw time: `type.source->listOfTextures[slot].gosTextureHandle`
- The multi-child-multi-texture pattern means each per-packet draw must resolve from the
  correct child node's texture slot at draw time.

### Texture array opportunity

With 20-40 mechs per mission, each with 1 paint texture (+ node-specific textures), a
texture array or atlas could reduce per-packet binds. However, paint schemes vary per mech
instance; an SSBO `textureLayer` index per instance is the natural extension of the cement
multi-sampler pattern established for terrain. This is an optimization for a later slice.

---

## Section F: `.tgl`/`.tglc` cache format

### Current serialized content per TG_TypeVertex (tgl.cpp:619-623)

```cpp
binFile.writeInt(numTypeVertices);
binFile.write((MemoryPtr)listOfTypeVertices, sizeof(TG_TypeVertex) * numTypeVertices);
```

`sizeof(TG_TypeVertex)` = `sizeof(Point3D) + sizeof(Vector3D) + sizeof(DWORD)`
= 12 + 12 + 4 = **28 bytes** per vertex.

UV is NOT in `TG_TypeVertex`. UV is per-triangle in `TG_TypeTriangle::uvdata`
(`TG_UVData`: u0,v0,u1,v1,u2,v2 = 6 floats = 24 bytes per triangle, tgl.h:104-116).

Also serialized per shape:
- `listOfTypeTriangles` (tgl.cpp:629-635): each `TG_TypeTriangle` = Vertices[3] + localTextureHandle +
  renderStateFlags + uvdata + faceNormal. ~68 bytes per triangle.

### Skinning-ready format additions

Target vertex format from user direction (48 B/vertex):
- position (12 B) — already in `TG_TypeVertex`
- normal (12 B) — already in `TG_TypeVertex`
- UV (8 B) — currently per-triangle; would need to be **per-vertex** in the GPU VBO
- 4×boneIndex (u8) (4 B) — new
- 4×boneWeight (u8 normalized) (4 B) — new
- tangent (oct-encoded) (8 B) — new

**For existing MC2 content:** bone indices can be **synthesized at registration time**
without modifying `.tgl` files. Since each `TG_Shape` leaf is a rigid body:
- Each vertex in a leaf node gets `boneIdx[0] = nodeIndex` (where nodeIndex is the
  leaf's index in `TG_MultiShape::listOfShapes[]`)
- Weights = (255, 0, 0, 0) (degenerate single-bone case)

UV must still be promoted from per-triangle to per-vertex in the GPU VBO. This is a
registration-time computation (per-triangle UVs → per-vertex by index remapping),
not a `.tgl` format change.

**Re-authoring cost for stock MC2 content:** ZERO. The GPU type VBO is built fresh at
registration time by the batcher reading `listOfTypeVertices` + `listOfTypeTriangles` from
the in-memory TGL structures (which are themselves loaded from `.tgl` cache). The batcher
synthesizes bone indices from the hierarchy during registration without any change to the
`.tgl` on-disk format. This claim is scoped to stock content only — see "Persistent format
decision" below for the future-import case.

**For future modder skinned content (UE5/MW5 via Track D Assimp):** the Assimp import
adapter (`mclib/assimp_tg_mesh.cpp` or equivalent) would write real bone indices, weights,
and tangents at import time. A persistent format decision is required — see below.

### .tglc cache (compiled variant)

Grep check: `.tglc` files use the same binary format as `.tgl` but are pre-compiled.
The `readBinFile`/`writeBinFile` paths (tgl.cpp:499-635) are the canonical serialization.
Re-authoring `.tglc` from `.tgl` is mechanical; any change to `TG_TypeVertex` size would
require regenerating all cached `.tglc` files from source `.tgl` files.

**This is NOT a concern for the proposed design** since bone indices are synthesized at
registration time by the GPU batcher, not serialized into `.tgl`/`.tglc`.

### Persistent format decision (required before spec)

For **stock content**, the GPU type VBO is a pure runtime artifact: regenerated at
registration from `.tgl` each time. No persistent GPU-side cache is needed because
`.tgl` → registration synthesis is cheap and deterministic.

For **future true-skinned imports** (Track D Assimp), the three viable options are:

| Option | Description | Tradeoffs |
|--------|-------------|-----------|
| **A — New GPU cache file** | Separate `.tglgpu` (or similar) sidecar written by the Assimp importer at import time; loaded by the mech batcher instead of the `.tgl` path | Cleanest runtime; adds a new serialization format to maintain; cache invalidation needed when importer changes |
| **B — Sidecar skinning-attribute file** | Separate file keyed to the TGL asset path storing only the new attributes (boneIdx, weight, tangent); merged with `.tgl` data at registration | Minimal `.tgl` disruption; requires merging two sources at load; file management overhead |
| **C — Extend `.tgl`/`.tglc` format** | Add version field to `TG_TypeVertex`; serialize bone/weight/tangent fields | Simplest single-file story; forces re-authoring ALL existing `.tgl` caches; breaks the "zero re-authoring" guarantee for stock content |

**Recommendation for the brainstorm to decide:** Option A (new `.tglgpu` sidecar cache
produced by the Assimp importer). Stock `.tgl` files are never touched, satisfying the
zero-re-authoring invariant. The Assimp import path writes `<meshname>.tglgpu` alongside
`<meshname>.tgl`; the batcher loads `.tglgpu` if present, otherwise falls back to the
degenerate-synthesis path.

**However: `.tglgpu` must be treated as a cache/schema decision, not an implementation
convenience.** The brainstorm must resolve all of the following before the spec locks this:

| Question | Options | Stakes |
|----------|---------|--------|
| **Version field** | 4-byte magic + 1-byte version at file header | Breaking schema change (new bone fields, layout change) must bump version; loader must reject mismatched version rather than silently misinterpreting data |
| **Invalidation key** | (a) hash of source `.tgl` content; (b) source `.tgl` mtime+size; (c) no automatic invalidation (importer re-runs explicitly) | Option (c) is simplest and correct for a modding pipeline where the importer is an explicit user action, not a hot-path |
| **Relationship to `.tglc`** | `.tglgpu` is independent — it is NOT a variant of `.tglc`; `.tglc` is a pre-compiled binary of `.tgl` geometry; `.tglgpu` carries GPU-specific attributes that have no `.tgl` analog | Must be documented in the format spec; avoid the temptation to extend `.tglc` because `.tglc` re-authoring would touch all existing caches |
| **Stock content: emit or synthesize?** | (a) Stock mechs never write `.tglgpu` — attributes synthesized in memory at every map load; (b) Stock mechs write `.tglgpu` on first load (cached for subsequent loads) | Option (a) is preferred for stock: synthesis is cheap (node index → boneIdx[0], no UV triangulation complexity since UVs are already per-triangle indexed), and it avoids a first-launch write that could fail on read-only installs. Option (b) makes sense only if synthesis becomes expensive (e.g., MikkTSpace tangent computation for large mech meshes) |

**Recommended defaults:** version field YES, invalidation = explicit (option c), stock content
synthesizes in memory (option a — never writes `.tglgpu`), `.tglgpu` exists only for Assimp-
imported skinned content. The brainstorm must explicitly confirm or override each row.

---

## Section G: Slice 2 vertex format coordination — CRITICAL FINDING

**Verdict: no blocking vertex-format conflict with slice 2. The mech arc does NOT need to
feed slice 2's format decision, and can start before slice 2 completes. However, the mech
arc should track slice 2's shader/lighting/SSBO-record conventions — see "Convention
alignment" below.**

### Current state of slice 2 (as observed in code)

Slice 2 (GPU vertex lighting for buildings/trees/generics) is substantially further along
than the recon prompt implies. The following stages have shipped:

| Stage | Status | Evidence |
|-------|--------|----------|
| 2.A (eligibility gate) | SHIPPED | `isMultiShapeEligibleForGpuObjects` in `gos_static_prop_batcher.cpp:1667` |
| 2.B (positions-only variant) | SHIPPED | `TransformMultiShape_PositionsOnly` at `msl.cpp:1740`; `CacheGpuLightData` at `msl.cpp:1765`; BldgAppearance gate at `bdactor.cpp:2239-2248`; TreeAppearance gate at `bdactor.cpp:4399+` |
| 2.C (GPU lighting kernel) | SHIPPED — `shaders/include/lighting.hglsl` present; actual `calc_light()` call at `shaders/static_prop.vert:199`; batcher lines 574/667 are comments only — searching batcher .cpp for calc_light finds no calls |
| 2.D.1 (parity SSBO) | SHIPPED | `gos_object_parity.cpp:8-11` |
| 2.D.1.1 (slot overflow) | SHIPPED | `gos_object_parity.cpp:12` |
| 2.D.2 (dual-emit parity) | SHIPPED | `gos_object_parity.cpp:13-14`; dual-emit in `bdactor.cpp:2249-2252` |

### Is slice 2 changing the vertex format?

Slice 2 does NOT change the serialized `.tgl` vertex format. It works by:
1. Using `TransformMultiShape_PositionsOnly` to run hierarchy animation (computing per-node
   `shapeToWorld`) while **skipping** the per-vertex lighting bake
2. The GPU shader (`static_prop.vert` / `static_prop.frag`) reads from the existing type VBO
   (which uses the Stage 1+2.C.2 vertex format: position + normal + UV + localVertexID + aRGBLight)
3. GPU lighting is computed in the VS by the `calc_light()` kernel (in `lighting.hglsl`)
   reading from a cached light-data SSBO

The **current** type VBO vertex layout (Stage 1 + Stage 2.C.2; from `shaders/static_prop.vert`):

```
layout(location = 0) in vec3  a_position;       // local-space position
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uint  a_localVertexID;  // shape-scope: 0..N-1
layout(location = 4) in uint  a_aRGBLight;      // Stage 2.C.2 — per-vertex hot-color tag (TG_TypeVertex::aRGBLight)
```

This is the format the static prop arc is operating on. It does NOT have bone indices.
**Adversarial revision note:** the prior description of this layout omitted location 4 (`a_aRGBLight`)
added by Stage 2.C.2. The mech arc VBO is fully independent (separate GL objects), so there is no
runtime conflict, but anyone writing the mech VS should cite this 5-attribute layout as context,
not the stale 4-attribute version.

### Mech arc vertex format is independent

A GPU mech draw arc would need a SEPARATE type VBO with a different (extended) vertex layout:
```
layout(location = 0) in vec3  a_position;       // local-space position
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uvec4 a_boneIndices;    // 4×u8 packed as uvec4
layout(location = 4) in vec4  a_boneWeights;    // 4×u8 normalized
layout(location = 5) in vec2  a_tangent;        // oct-encoded tangent (optional)
layout(location = 6) in uint  a_localVertexID;  // for color indexing
```

This format is **entirely separate** from the slice 2 static-prop format. The two arc's
type VBOs are different GL objects, potentially allocated by different subsystems
(the existing `GpuStaticPropBatcher` for buildings/trees/generics, a new `GpuMechBatcher`
for mechs).

### Can the two arcs proceed in parallel?

Yes. The mech arc:
- Uses a different `TG_MultiShape` subclass (mechs have `TG_AnimateShape` or use the same
  `TG_MultiShape` but with animation keyframes populated)
- Doesn't enter `isMultiShapeEligibleForGpuObjects` (builds don't register mech types)
- Has its own `localTextureHandle` per-instance paint scheme
- Needs per-node bone matrix SSBO (not per-instance color buffer)

The static-prop arc:
- Explicitly OUT-OF-SCOPES mechs in the brainstorm (`2026-05-02-object-offload-scope.md:600-633`)
- `isMultiShapeEligibleForGpuObjects` gates on node type == SHAPE_NODE; mech-specific nodes
  (helper nodes, bone nodes) would fail this check and fall to CPU

**There is no vertex-format conflict.** The mech arc designs a new vertex format for a new
batcher; slice 2 extends the slice 1 format (which doesn't have bones). The two formats live
in separate VBOs and are never mixed.

### Convention alignment (non-blocking but load-bearing)

Slice 2 establishes project-level GPU-object conventions that the mech arc should inherit
rather than diverge from:

| Convention | Slice 2 establishes | Mech arc should |
|------------|---------------------|-----------------|
| GPU lighting kernel | `lighting.hglsl` / `calc_light()` ABI (4-param: lights_index, normal, vertex_world_pos, base_light); call site `static_prop.vert:199` | Reuse same kernel in mech VS — **but see LightsData[32] cap below** |
| Per-instance SSBO layout | `GpuStaticPropInstance` (112 B: `float modelMatrix[16]` + 4×uint32 + 2×float[4]; static_asserts enforce offsets at byte 0/64/68/72/76/80/96) | Follow std430/static_assert discipline; mech instance intentionally diverges — no inline mat4 (bone matrices go in separate `GpuMechBone` SSBO) |
| Texture binding | `textureSlot` index resolved at draw time (not handle cached) | Same pattern |
| SSBO binding slots | Slots 0-3 claimed by `gos_static_prop_batcher` (Instances=0, Colors=1, PerType=2, ParityOut=3) | Mech batcher must use binding slots 4+ or a completely separate SSBO layout |
| Parity infrastructure | `gos_object_parity.cpp` dual-emit harness | Mech arc should add its own parity gate using same harness |
| **LightsData[32] UBO cap** | `lighting.hglsl:41` hard-limits to 32 `ObjectLights` entries; static props consume one slot per unique light config via dedup cache | **OPEN — must decide before spec:** share UBO (capacity audit required: worst-case buildings+mechs ≤ 32), expand the array (lockstep GLSL+C++ change), or use separate per-mech lighting path that doesn't require `lightDataIndex` |
| **Sampler state inheritance** | Static prop batcher sets depth state explicitly (lines 1347-1349) but binds no sampler — inherits prior draw's state | Mech batcher must either (a) bind an explicit REPEAT/LINEAR sampler before draw, or (b) document that mech draw always precedes passes that mutate sampler state — see `sampler_state_inheritance_in_fast_paths.md` |

These are "how to build a GPU object pass in this project" conventions. Diverging from them
(e.g., using a different lighting model or a different per-instance struct layout) creates a
second divergent GPU-object architecture. The brainstorm should explicitly adopt or explicitly
deviate with justification.

---

## Section H: Joint matrix upload model

### Current per-node matrix state

`TransformMultiShape` computes per-node `shapeToWorld` matrices into
`listOfShapes[i].shapeToWorld` (`TG_ShapeRec.shapeToWorld`, a `Stuff::LinearMatrix4D`).
For a mech with N nodes, there are N such matrices, each a 4×4 float = 64 bytes.

A typical mech has ~10-20 visible nodes; with 40 mechs per mission = 400-800 matrices per
frame = 25-51 KB of matrix data.

**Bone index u8 limit:** The target format uses `u8` bone indices (4 bytes per vertex,
max 255 bones per mech draw record). `TG_MultiShape::numTG_Shapes` (= `numTG_TypeShapes` at
allocation time, `msl.cpp:134`) gives the per-type node count. The ~10-20 node range is
well within the u8 limit. The brainstorm should add an open measurement item: enumerate
actual `numTG_TypeShapes` across all stock mech types to confirm no type exceeds ~50 nodes.
If a future modded mech (Track D) has many sub-meshes, u8 remains safe up to 255; u16 is
the overflow option with 2 extra bytes per vertex.

### Upload shape options

| Option | Pros | Cons |
|--------|------|------|
| **Uniform array** | Simple, low overhead | std140 layout, max ~256 mat4 (16 KB) — fine for 20 bones × 40 mechs if batched by type; breaks for multi-type batching |
| **SSBO (std430)** | No size limit, matches established project pattern (water, terrain, indirect, objects) | Slightly more setup than uniforms |
| **TBO** | Pre-modern, widely supported | Unusual, non-standard layout, harder debugging |

**Recommendation: SSBO.** It's the established project shape for all prior GPU offload arcs.
Pattern: persistent coherent mapped ring (3 frames), one entry per (mesh_instance, node_index)
pair. Binding slot to coordinate with the existing batcher SSBOs (slots 0-3 are in use for
objects; mech batcher would use a separate binding block).

A concrete layout candidate:
```glsl
// Per-mech-instance: one entry per mech actor
struct GpuMechInstance {
    uint  mechTypeID;       // → type VBO geometry range
    uint  firstBoneOffset;  // into GpuMechBone[] SSBO
    uint  numBones;
    uint  firstColorOffset; // into per-instance color buffer (if retained)
    vec4  aRGBHighlight;
    vec4  fogRGB;
    uint  flags;            // lightsOut, isWindow, paint blend flags
    uint  textureSlot;      // localTextureHandle slot index (resolve at draw)
    uint  _pad0, _pad1;
};

// Per-node bone matrix SSBO (populated from shapeToWorld after TransformMultiShape)
struct GpuMechBone {
    mat4  boneMatrix;       // shapeToWorld for this node, Stuff row-major convention
};
```

`firstBoneOffset + boneIndex` indexing in the VS lets each vertex address its bone matrix
directly. For the degenerate case (one bone per mesh): `numBones = numVisibleNodes`,
`boneMatrix[a_boneIndices[0]]` is always bone 0 for that node's vertices.

---

## Section I: Animation FSM retention

### Current FSM structure (mech3d.cpp:3813-4158)

`Mech3DAppearance::update` (called from `TerrainObject::update` via `appearance->update()`)
runs a gesture FSM:
- `currentGestureId` tracks the animation state (walk, run, jump, torso twist, etc.)
- `currentFrame` is a float that advances per-frame
- Gesture transitions are computed from game state (velocity, weapon fire, damage, etc.)
- The FSM writes `currentGestureId`, `currentFrame`, `torsoRotation`, and optionally
  triggers gosFX particle effects (footprints, smoke, explosions)

### Relationship to GPU rendering

The FSM outputs are: `currentGestureId`, `currentFrame`, `torsoRotation`, arm-status flags.
`updateGeometry` (mech3d.cpp:3000) consumes these via:
- `mechType->setAnimation(mechShape, currentGestureId)` — binds keyframe tracks
- `mechShape->SetFrameNum(currentFrame)` — sets frame index
- `mechShape->SetNodeRotation("joint_torso", &torsoRot)` — applies torso twist

`TransformMultiShape` then CONSUMES the animation state to compute per-node matrices.

**The animation FSM stays ENTIRELY on the CPU.** The GPU mech draw arc does not move the
FSM or the keyframe sampling. What moves to GPU is only the per-vertex TRANSFORM AND RENDER,
not the per-node matrix computation. The pipeline becomes:

```
CPU: FSM → setAnimation/SetFrameNum/SetNodeRotation → TransformMultiShape
     → per-node shapeToWorld matrices
     ↓
GPU: mech_vertex_shader reads boneMatrix[a_boneIndices[0]] = shapeToWorld for this node
     applies paint texture, fog, highlight from per-instance SSBO
     computes gl_Position via worldToClip chain
```

The hierarchy chain composition (msl.cpp:1374-1724) runs CPU-side each frame, exactly as
today. This is correct: the FSM drives per-frame node rotations that feed the hierarchy; the
hierarchy output (per-node `shapeToWorld`) is what gets uploaded to the bone SSBO.

**The pasted analysis's claim "hierarchy chain composition is identical between current and
skinned"** is approximately correct: the CPU hierarchy traversal stays unchanged; only the
downstream per-vertex kernel (TransformShape → screen submit) moves to GPU.

### Implication for slice scope

The GPU mech draw arc does NOT require rewriting the animation FSM. It requires:
1. Extracting per-node `shapeToWorld` matrices after `TransformMultiShape` completes
2. Uploading them to the bone SSBO
3. Replacing `TG_Shape::Render` (per-vertex CPU submit) with a GPU instanced draw that
   reads the type VBO + bone SSBO + per-instance SSBO

---

## Section J: Slice scope estimate

### Reference points (from orchestrator)

| Slice | Work | Result |
|-------|------|--------|
| Static-prop substrate (slice 1) | 8 commits, ~1 week-equivalent | Substrate only, no perf |
| renderWater Stage 1+2+3 | ~3 weeks total | 78-85% GPU reduction, tier1 5/5 |
| Cement multi-sampler | ~1 week | Multi-layer seam + default-on flip |

### Mech arc structure

Unlike static-prop slice 1 (which could avoid touching the animation pipeline), mech work
requires engaging the animation pipeline from day 1:

**Mech slice A (substrate — per-node rigid degenerate):**
- Register mech type geometry into a skinning-ready type VBO at map load
  - Walk all `Mech3DAppearance` types at `primeMissionTerrainCache` time
  - Build per-node vertex ranges with synthesized boneIdx per vertex
  - Handle localTextureHandle per-instance paint (store slot index)
- Per-frame SSBO upload: per-instance `GpuMechInstance` + per-bone `GpuMechBone`
  - Extract `shapeToWorld` from each node AFTER `TransformMultiShape` runs
  - Upload to ring-buffered SSBO
- Replace `mechShape->Render(true)` with `g_mechBatcher.submit(...)`
- Draw at frame end: per-type instanced draw using the skinning-ready VS

**Complexity delta vs static-prop slice 1:**
- More complex registration (node hierarchy → per-vertex bone assignments)
- Per-bone matrix extraction step (new, reads from `TG_ShapeRec::shapeToWorld`)
- Per-mech paint texture slot per instance (similar to static-prop, but critical for
  mech identity)
- Multiple sub-shapes per mech (torso, arms, weapon mounts) need correct bone range

**Estimate: 1.5–2× static-prop slice 1** = approximately 12-18 commits, 1.5-2 week-equivalent.
The added complexity comes from the node→bone mapping and the per-frame matrix extraction,
not from new infrastructure concepts (the SSBO pattern, ring buffer, eligibility gate,
state save/restore are all established by prior slices).

**Mech slice B (GPU vertex lighting for mechs):**
Can use the `calc_light()` kernel being built for slice 2 (buildings). If slice 2's lighting
GLSL infrastructure is reusable (it should be — the lighting model is the same), mech slice B
is a smaller incremental on top of mech slice A + slice 2's lighting kernel. Estimated:
6-10 commits, ~1 week.

**Total (A+B): 2-3 weeks**, comparable to the renderWater arc.

---

## Section K: Risk inventory

### Prior-attempt failure modes that DO apply to mechs

| From brainstorm Q1 | How mech work intersects |
|---|---|
| **a1** Cull-bypass cascade | Mechs are the HIGHEST RISK population for this. `inView` gates both `updateGeometry()` AND `Render()`. Any GPU mech path must preserve the Q3=(a) seam: CPU cull unchanged, GPU draws cull survivors only. |
| **a2** Pool as policy | Mechs are the LAST allocators; they're the canary. Q3=(a) ensures pool consumption unchanged. If the mech arc calls `TransformMultiShape` on out-of-view mechs (to get matrices for uncull'd GPU draw), pool exhaustion re-emerges. |
| **a4** Cost blindspot | Mech cost is also in `TransformMultiShape` (animation + per-vertex bake), not just in `Render`. GPU mech draw (slice A) moves only `Render`; the full perf slice (slice B + lighting SSBO) is needed to move the 2.4 ms analog. |
| **b1** Cached texture handle | Per-mech `localTextureHandle` is THE canonical example of per-frame mutation. Must store `textureSlot` and resolve at draw time. |
| **b4** Behind-camera streak | Without CPU angular cull, behind-camera mechs produce screen-spanning streaks. Q3=(a) means the CPU cull still guards this. |
| **c2** objBlockInfo as terrain-vertex derivative | The outer iteration loop at `objmgr.cpp:1491-1511` gates mechs AND buildings. GPU mech draw still calls from within the managed iteration; the terrain-vertex angular cull issue at `mclib/terrain.cpp:1517-1532` remains uncorrected (87% false-negative at wolfman zoom). |

### New risks not present in static-prop arc

| Risk | Description |
|------|-------------|
| **Per-instance paint scheme** | `localTextureHandle` is per-mech-actor, not per-type. All mechs of the same type can have different paint. The type VBO must expose a per-instance texture slot path; the existing `a_localVertexID` + color-buffer pattern may need extension for the texture dimension. |
| **Detached arms** | When `leftArmOff` or `rightArmOff`, the arm becomes a separate physics-simulated object (`leftArm`, `rightArm` pointers). These are additional `TG_MultiShape` instances with their own per-frame transforms. The registration pass must enumerate detached-arm variants. |
| **mechShadowShape** | Mechs have a separate `mechShadowShape` (`TG_MultiShape*`) for legacy blob shadows. GPU mech draw's shadow flush must handle this OR confirm that post-process shadow maps (which are working per `dynamic_shadow_status.md`) eliminate the need for it. |
| **inView gate + `turn < 3` term** | The `turn < 3` early-force in `mech3d.cpp:4183` ensures matrices are valid for the first 3 frames after load. A GPU path that queries `shapeToWorld` from a mech that hasn't yet run `updateGeometry` (frame 0 before `turn < 3` fires) gets uninitialized matrices. Must initialize per-node matrices to identity or detect `calcedThisFrame != turn`. |
| **Gesture GestureJump bypass** | `GestureJump` always forces `updateGeometry` regardless of `inView`. A GPU path that skips `updateGeometry` for out-of-view mechs must still handle the jump animation correctly. Under Q3=(a) this is a non-issue (CPU cull unchanged; jumping mechs that go off-screen still have inView handled by the cull). |
| **LOD swap** | ⚠ **ADVERSARIAL REVISION — prior "CONFIRMED NOT PRESENT" was wrong.** `recalcBounds` ends at line 2362 (just before `render` at 2366); the prior adversarial pass truncated its read at 2240. The LOD swap IS present at `mech3d.cpp:2299-2345`: when `selectLOD != currentLOD`, the code does `delete mechShape; mechShape = mechType->mechShape[currentLOD]->CreateFrom()`. This invalidates any cached per-instance `TG_MultiShape*`, node pointers, or per-node index state the GPU mech batcher holds. Same class of bug as `bldg_animation_lod_swap_unsafe.md`. **Required mitigation (choose one before spec):** (A) register all LOD variants per mech type at map load, select active VBO range by `currentLOD` at submit; (B) suppress LOD swap under `g_useGpuMechs` flag; (C) measure via `MC2_MECH_LOD_TRACE=1` smoke — if `selectLOD` never diverges from 0 for any stock mission, the swap is dead code and (C) is sufficient. Advisor preference: **option A**. |

### Failure modes that DO NOT apply to mechs (resolved by static-prop arc)

| From brainstorm | Why not a mech risk |
|---|---|
| **b3** Layer B false positive on ~100% of inputs | The batcher's eligibility gate now works (slice 1 shipped; Gate F counter proves GPU draws are happening). The mech batcher can inherit the same gate pattern. |
| **c1** TGL pool exhaustion silent | Instrumented by `MC2_TGL_POOL_TRACE=1` and the always-on pool summary. Visibility is there. |
| **b2** Wrong color stream | The ARGB stream (listOfVertices[j].argb) vs listOfColors distinction is now documented and fixed in the slice 1 batcher. Mech batcher can inherit the fix. |

---

## Section L: Open questions for brainstorm

1. **`Mech3DAppearance::recalcBounds` LOD swap — ADVERSARIAL REVISION: swap IS PRESENT.**
   `recalcBounds` (2073–2362) deletes and recreates `mechShape` at lines 2299-2345 when
   `selectLOD != currentLOD`. The prior pass read only to 2240 and missed it. **Required:
   measure via `MC2_MECH_LOD_TRACE=1` smoke whether selectLOD ever diverges from 0 for
   stock mechs, then choose mitigation A (register all LODs), B (suppress swap under GPU
   flag), or C (dead-code assumption validated by measurement).** Advisor preference: A.

2. **`lighting.hglsl` / `calc_light()` kernel shipping state. RESOLVED.** `shaders/include/lighting.hglsl`
   **exists** on the branch (confirmed 2026-05-03 adversarial pass). Stage 2.C is COMPLETE.
   Mech slice B CAN reuse the `calc_light()` kernel. Frame the mech arc as "new batcher +
   skinning-ready VBO + reuse existing lighting kernel."

3. **Per-mech shadow strategy.** Should GPU mech draw produce shadow data into the dynamic
   shadow FBO (as the static-prop design intended for `flushShadow()`)? Or does the existing
   post-process shadow pipeline (`dynamic_shadow_status.md` — working, 184 µs/frame) make
   this unnecessary? The static-prop slice 1 deferred shadows to CPU; mech arc should decide
   this explicitly.

4. **Wolfman-zoom coverage with Q3=(a).** Under Q3=(a), mechs that the cull marks as out-of-
   view still vanish at wolfman zoom (87% false-negative rate). This is the SAME behavior as
   today — no regression, no fix. The GPU mech arc should document this explicitly rather than
   implicitly promising "mechs always visible."

5. **GenericAppearance mech-like actors.** Some Generic actors may have animation (per the
   slice 1 exclusion list). Are they already handled by the slice 2 population, or do they need
   special treatment in a mech-arc brainstorm? The brainstorm classified them as "same as
   buildings" but verify whether any have the `inView || g_useGpuStaticProps` render gate
   rather than the buildings pattern.

6. **Type VBO registration timing for mechs.** Buildings are registered at `primeMissionTerrainCache`
   (map load). Mechs in MC2 can also spawn mid-mission (reinforcements). The late-registration
   soft-fail pattern from slice 1 must also apply to mechs. Reinforcement mechs arrive via
   `ObjectManager` allocation; the exact spawn site needs a fresh grep.

7. **Tangent source strategy for stock mechs.** The target vertex format includes an oct-encoded
   tangent (8 B). `TG_TypeVertex` has no tangent field; `tgl.cpp` has no tangent computation.
   Options for stock mechs: (a) generate at registration time from positions + normals + per-vertex
   UVs (Lengyel / MikkTSpace); (b) zero-fill for stock content since stock mechs use diffuse-only
   textures and have no normal maps; (c) defer tangent support behind a material/normal-map gate
   activated only when the mech type has a normal-map texture. Option (b) is safest for the initial
   arc: the field exists in the VBO layout, is zero-filled, and the VS ignores it for now. If a
   future mech skin adds a normal map, the tangent gets computed by the importer at that time.
   The brainstorm should decide this explicitly.

8. **Max `numTG_TypeShapes` across stock mech types — MUST MEASURE BEFORE SPEC LOCKS u8.**
   The u8 bone index limit (255 nodes per mech type) is almost certainly safe given the
   observed ~10-20 node range, but "almost certainly" is not the same as "measured once and
   written down." This is exactly the class of "obvious" invariant that should be a one-time
   measurement, not an assumption carried forward.

   **Measurement recipe:** Add a `MC2_MECH_NODE_TRACE=1` env-gated print in
   `Mech3DAppearance::init` (or wherever `mechShape` is first assigned) that logs:
   ```
   [MECH_NODE v1] type=<typeName> numTG_Shapes=<N>
   ```
   Run tier-1 smoke with `MC2_MECH_NODE_TRACE=1`. Collect the log. The max `N` across all
   types becomes the documented "stock max node count" and validates u8 is sufficient.

   **What to record:** max `numTG_TypeShapes` seen, the type name that produced it, and
   whether any type exceeds 50 (the comfortable u8 headroom threshold). If max > 100, revisit
   u16. Expected answer: max ~25-35, comfortably within u8. Write the result into the
   brainstorm/spec as a cited measurement, not an estimate.

9. **Scope boundary: stock validation vs. modern import readiness.** This recon covers the
   **stock/rigid-degenerate path only** (the GPU-direct rendering arc for existing MC2 mechs).
   The skinning-ready vertex format is architecturally motivated by Track D (UE5/MW5 imports),
   but the validation gate for the initial mech arc is tier-1 stock missions only (per
   `feedback_offload_scope_stock_only.md`). The brainstorm should be explicit about which
   questions are "does the stock arc work" vs. "are we forward-compatible with future imports."
   These are different conversations with different acceptance criteria.

---

## Code-grounding verification appendix

Every claim that names existing code, verified grep at write-time:

| Citation | Verified at | Status |
|----------|------------|--------|
| `TG_TypeVertex` struct definition | `mclib/tgl.h:35-42` | MATCHES — position + normal + aRGBLight only |
| `TG_HWTypeVertex` with UV | `mclib/tgl.h:49-58` | MATCHES |
| `gos_VERTEX` definition | `GameOS/include/gameos.hpp:2147-2152` | MATCHES — x,y,z,rhw,argb,frgb,u,v |
| `.tgl` serialization of TG_TypeVertex | `mclib/tgl.cpp:619-623` | MATCHES — binFile.write(listOfTypeVertices, sizeof(TG_TypeVertex)*N) |
| `TG_UVData` per-triangle UV | `mclib/tgl.h:104-116` | MATCHES — u0,v0,u1,v1,u2,v2 per triangle |
| "joint" in mech3d.cpp = node names only | `mclib/mech3d.cpp:740,776,812,850,1230,1235,2178,2316,2341,2530,2557,3179,3189` | MATCHES — all are string-arg calls to SetNodeRotation/GetNodeNameId/isChildOf/StopUsing |
| weight/bone/skin in tgl.cpp | grep result: 0 matches | CONFIRMED 0 matches |
| weight/bone/skin in tgl.h | grep result: 0 matches | CONFIRMED 0 matches |
| weight/bone/skin in gvactor.cpp | grep result: 0 matches | CONFIRMED 0 matches |
| joint/skin/bone in msl.cpp | grep result: "joint_xUARM" string literal only | CONFIRMED no vertex-stage fields |
| `updateGeometry` gate at mech3d.cpp | `mclib/mech3d.cpp:4183` | MATCHES |
| `mechShape->TransformMultiShape` at mech3d.cpp | `mclib/mech3d.cpp:3199` | MATCHES |
| `Mech3DAppearance::updateGeometry` at mech3d.cpp | `mclib/mech3d.cpp:3000` | MATCHES |
| `SetTextureHandle(0, localTextureHandle)` in updateGeometry | `mclib/mech3d.cpp:3004` | MATCHES |
| `mechShape->SetLightList(eye->getWorldLights(), eye->getNumLights())` | `mclib/mech3d.cpp:3198` | MATCHES |
| `Mech3DAppearance::render` at mech3d.cpp | `mclib/mech3d.cpp:2366` | MATCHES |
| `if (inView || g_useGpuStaticProps)` in mech render | `mclib/mech3d.cpp:2377` | MATCHES |
| `mechShape->Render(true)` in mech render | `mclib/mech3d.cpp:2406` | MATCHES |
| `TransformMultiShape` hierarchy loop | `mclib/msl.cpp:1341-1730` | MATCHES |
| SetTextureHandle per-child in TransformMultiShape | `mclib/msl.cpp:1378-1382` | MATCHES |
| keyframe quat slerp: `currentAnimation->quat[fNum]` | `mclib/msl.cpp:1426` | MATCHES |
| `s_multiShapePositionsOnly` flag | `mclib/msl.cpp:1339` | MATCHES |
| `TransformMultiShape_PositionsOnly` at msl.cpp | `mclib/msl.cpp:1740-1746` | MATCHES |
| `CacheGpuLightData` at msl.cpp | `mclib/msl.cpp:1765` | MATCHES |
| Slice 2 Stage 2.B in BldgAppearance::update | `mclib/bdactor.cpp:2239-2248` | MATCHES |
| `isMultiShapeEligibleForGpuObjects` in batcher | `GameOS/gameos/gos_static_prop_batcher.cpp:1667` | MATCHES |
| `g_useGpuObjects` bool in batcher.cpp | `GameOS/gameos/gos_static_prop_batcher.cpp:22` | MATCHES |
| Slice 2 Stage 2.D.2 shipped | `GameOS/gameos/gos_object_parity.cpp:13-14` | MATCHES |
| Pool sizes at mission.cpp | `code/mission.cpp:3140-3152` | Per brainstorm verification — line drift expected; shapes are the same |
| `objmgr.cpp` active-block gate | `code/objmgr.cpp:1491,1758` | Per brainstorm verification |
| mech3d.cpp `g_useGpuStaticProps` bypass | `mclib/mech3d.cpp:4183` (in inView gate) | MATCHES |
| bldg_animation_lod_swap_unsafe.md (LOD fix merged) | `mclib/bdactor.cpp:1336-1366` | Per brainstorm Q4 "already here on terrain-pbr-mod" |
| `TG_MultiShape::TransformMultiShape_PositionsOnly` declaration | `mclib/msl.h:352` | MATCHES |
| `TG_ShapeRec::shapeToWorld` as `Stuff::LinearMatrix4D` | `mclib/tgl.h:375` | MATCHES — confirmed field exists |
| `listOfShapes` as `TG_ShapeRecPtr` in `TG_MultiShape` | `mclib/msl.h:262` | MATCHES |
| `newShape->numTG_Shapes = numTG_TypeShapes` (instance = type count at alloc) | `mclib/msl.cpp:134` | MATCHES |
| `lighting.hglsl` exists on branch | `shaders/include/lighting.hglsl` | CONFIRMED — file present |
| `calc_light()` call site | `shaders/static_prop.vert:199` | CONFIRMED — batcher lines 574/667 are comments; actual call is in the shader |
| `Mech3DAppearance::recalcBounds` starts at | `mclib/mech3d.cpp:2073` | MATCHES |
| `recalcBounds` ends at | `mclib/mech3d.cpp:2362` (render at 2366) | CONFIRMED (full function boundary) |
| `recalcBounds` LOD swap | `mech3d.cpp:2299-2345` | ⚠ ADVERSARIAL REVISION — swap IS PRESENT; prior "CONFIRMED NOT PRESENT" was a truncated read (2073-2240 only) |
| `terrain.cpp` setObjBlockActive | line 1629 (not 1610-1611 as previously cited) | CORRECTED — `objBlockInfo[blockNum].active=true` at 1479/1903 |
| Pool sizes at `code/mission.cpp` | `mission.cpp:3140,3143,3146,3149` | CONFIRMED exact lines |

---

## Cross-cutting with slice 2

**Summary:** No blocking vertex-format conflict with slice 2. Slice 2 is already substantially
implemented (Stages 2.A, 2.B, 2.C, 2.D.1, 2.D.1.1, 2.D.2 shipped). The mech arc uses a
separate batcher, separate type VBO, and separate vertex shader — the two formats live in
separate GL objects and are never mixed.

"Fully independent" is too strong: slice 2 establishes conventions (lighting kernel ABI,
per-instance SSBO layout, texture binding pattern, SSBO binding slots) that the mech arc
should **inherit, not diverge from** (Section G — Convention alignment). This is a design
constraint for the brainstorm, not a schedule blocker.

**Frame the mech arc as:** "new `GpuMechBatcher` + skinning-ready type VBO +
per-mech/per-bone matrix SSBO + reuse `calc_light()` from `lighting.hglsl` + adopt slice 2
SSBO-record conventions." The persistent format decision for future skinned imports (Section F)
is a first-class brainstorm decision point.

---

## Closing: Ready-for-brainstorm / needs-more-recon / blocked

### Status: NEEDS REVISION — adversarial review found 1 CRITICAL + 4 MAJOR items (2026-05-03)

**Not ready for executor handoff or brainstorm start.** The required patch set below must be
applied to the findings doc, and the LOD mitigation strategy must be decided before the spec
can be written. Most important decision first: **choose LOD handling** — registration
lifetime, VBO ownership, bone-index mapping, and per-instance submit all depend on it.

#### Required patches before brainstorm handoff

| Priority | Finding | Required action |
|---|---|---|
| **CRITICAL** | `recalcBounds` LOD swap is present (2299-2345); "RESOLVED" claim was a truncated read to 2240 | Measure via `MC2_MECH_LOD_TRACE=1` smoke; pick mitigation A/B/C; advisor preference: A (register all LODs) |
| **MAJOR** | `LightsData[32]` UBO hard cap not in design (lighting.hglsl:41) | Decide: share UBO (capacity audit), expand, or separate per-mech lighting path |
| **MAJOR** | Stage 2.C cited at batcher lines 574/667 (comments); actual call is static_prop.vert:199 | Fixed in doc; no design change needed |
| **MAJOR** | Slice 1+2.C.2 VBO layout is 5 attributes (0-4), not 4 (0-3) | Fixed in doc; mech VBO is independent so no runtime conflict |
| **MAJOR** | Sampler state inheritance not addressed — batcher binds no sampler | Add to spec: bind explicit REPEAT/LINEAR sampler before mech draw |
| **MINOR** | `GpuObjectInstance` in convention table is a fictional struct name | Fixed — correct name is `GpuStaticPropInstance` |
| **MINOR** | SSBO convention inheritance vs. divergence not enumerated | Add to spec: list inherited (std430, static_asserts, ring buffer, textureSlot, parity) vs. divergent (no inline mat4) |

#### Sections verified clean (no revision required)

- **Section A (negative claim):** VERIFIED. Zero vertex-stage skinning fields exist.
- **Section B (pipeline map):** Complete and verified.
- **Section C (cull-cascade):** Four integration points correct. Q3=(a) is mandatory.
- **Section D (pool interaction):** Q3=(a) sidesteps pool risks.
- **Section E (texture binding):** Per-mech paint via `localTextureHandle` — store textureSlot, resolve at draw.
- **Section F (.tgl format):** No re-authoring for stock content. **Persistent format decision OPEN.**
- **Section H (joint matrix upload):** SSBO recommended. u8 sufficient. **Max node count OPEN measurement.**
- **Section I (FSM retention):** FSM stays CPU-side.
- **Section J (scope):** ~12-18 commits for mech slice A; ~6-10 for slice B.

#### Four first-class decisions for the brainstorm (after revision pass)

1. **LOD mitigation** (CRITICAL, Section K): A (register all LODs), B (suppress under GPU flag),
   or C (measurement-validated dead-code assumption). Everything else in the design depends on this.
2. **LightsData[32] capacity strategy** (MAJOR, Section G): Shared UBO, expanded UBO,
   or separate per-mech lighting pathway.
3. **Persistent format for future skinned imports** (Section F): `.tglgpu` sidecar vs. extend `.tgl` vs. sidecar skinning attributes.
4. **Tangent strategy for stock mechs** (Section L, item 7): zero-fill, generate at registration, or gate behind normal-map material.

**Resolved verifications (adversarial pass 2026-05-03):**
- `lighting.hglsl` CONFIRMED SHIPPED — Stage 2.C complete; call site at `static_prop.vert:199`.
- `recalcBounds` CONFIRMED TO CONTAIN LOD SWAP at 2299-2345 — prior "safe" claim OVERTURNED.
- Pool sizes confirmed at `mission.cpp:3140,3143,3146,3149,3152`.
- `GpuStaticPropInstance` layout confirmed: 112 B, static_asserts at gos_static_prop_batcher.h:27-35.
- `LightsData[32]` cap confirmed: `lighting.hglsl:41`.
