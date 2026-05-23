# RenderWorld Boundary Spec

- Status: EXECUTABLE-READY (brainstorm pass + adversarial review applied;
  greybeard pass deferred to first slice execution per skill scope)
- Date: 2026-05-22 (brainstorm); 2026-05-22 (adversarial review applied)
- Relation to roadmap: item 20 (Sequence A1) of
  `docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md`
- Type: contract / API boundary spec (DOC-ONLY; no code in this artifact)
- Adversarial review:
  `docs/superpowers/reviews/2026-05-22-renderworld-boundary-spec-adversarial-review.md`
  (0 CRIT / 5 MAJOR / 6 MINOR; all 5 MAJORs resolved in this spec; MINORs
  m1/m2/m4/m5 documented for first-slice planning)
- Remaining follow-ups:
  - roadmap amendment (lines 482-503) to match Section 12 expanded list
  - codex sign-off on Section 12 forbidden-deps enforceability
  - convergence with Pass Contract Registry spec (advisor Edit 1)
  - greybeard pass at first slice execution (META-FIX vs PATCH ruling
    on the legacy-adapter pattern itself)

This document is the north-star-aligned NS3 deliverable that precedes
writing the `RenderWorld` type. The roadmap's sequencing note (item 20)
is explicit: design the boundary FIRST as a doc/spec, because writing the
`RenderWorld` type before the boundary is agreed produces the wrong type.

---

## 1. Purpose / non-goals

### Purpose

`RenderWorld` is the engine-facing scene API: the single boundary across
which game code hands rendering work to the engine. Today every appearance
class (`Appearance` and its subclasses including the building, tree, mech,
and gv variants) reaches directly into engine internals -- TG_MultiShape,
master node arrays, GOS texture handles, glDrawElementsIndirect helpers,
and named SSBO binding points. `RenderWorld` collapses those reach-throughs
into a small handle-based API so that:

- the engine has one source of truth for "what exists to be rendered"
- game code can be moved off direct engine state without rewriting it all at once
- the same API shape survives the OpenGL -> Vulkan backend swap
- every Track V / Track G / Track C feature enters through this boundary
  rather than carving its own access path

### Non-goals (explicit)

- Not a Render Graph. Pass scheduling, resource lifetime tracking, and
  barrier/dependency management are deferred to the Pass Contract Registry
  (roadmap item 2, advisor Edit 1: rename "Render graph" -> "Pass Contract
  Registry"). RenderWorld calls into that registry; it does not own it.
- Not an ECS. Renderable archetypes are designed (Section 4) but not as
  components of a generic entity system.
- Not a generic job system. RenderWorld participates in the named job
  graph (roadmap "Job graph direction") but does not define one.
- Not a Vulkan backend. Every API surface MUST be expressible in Vulkan
  primitives (handles / pipeline desc / pass desc / binding tables /
  commands) per `memory/vulkan_prep_explicit_device_discipline.md`, but
  the backend swap is a separate work item (Tier 5).
- Not a unified material system. `MaterialGpu` design lives in Tier 2 V1.
  RenderWorld references handles to it but does not specify its layout.
- Not a multi-view design. `EngineView` is sketched in Section 5 to the
  level needed to make handle and packet types coherent, but multi-view
  scheduling is roadmap item 7, deferred behind F1 ship.
- Not an asset-cook spec. Cook -> manifest -> capability flow is Sequence B.

### Open questions (carry to follow-up pass)

- Q1.1: Does `RenderWorld` own per-frame transient resource allocation, or
  delegate to a future `FrameAllocator` (roadmap item 13)? Lean: delegate.
- Q1.2: Threading model -- is `RenderWorld` mutation single-threaded from
  the extraction job, or does it accept concurrent `upsertX` calls?
  Lean: single-writer, multi-reader; extraction is the writer.
- Q1.3: Does `RenderWorld` own light table state, or does `LightsData` SSBO
  remain its own subsystem behind a thin handle wrapper?

---

## 2. Ownership boundary

### What RenderWorld owns

- The set of currently-registered render objects (handle table)
- The set of currently-registered views (handle table)
- The mapping from object handle -> backend resource slot
  (recipe index, ObjectData[] slot, MaterialGpu slot, etc.)
- Submission of work to the Pass Contract Registry
- Routing of capability/feature-flag decisions for which fallback path
  a given object uses (delegated to a capability resolver in Section 9)

### What RenderWorld does NOT own

- Raw GL buffer / texture / program lifetimes (owned by `RenderDeviceGL`)
- Material content / texture array layout (owned by `MaterialSystem`)
- Mesh data / cluster DAGs / meshlet layout (owned by `AssetCook` +
  `MeshRenderer` storage backends)
- Game-side object identity, transforms, animation state (owned by
  `gameData` and copied/extracted into RenderWorld each frame)
- The cull dispatch itself (owned by `Visibility`; RenderWorld asks for
  a `VisibilityResult` and forwards admitted handles to draw packet build)
- Pass scheduling, FBO ownership, blend/depth state (owned by Pass
  Contract Registry + `RenderDeviceGL`)
- Debug visualization (owned by `DebugRenderer`; RenderWorld feeds it
  with read-only handle data)

### One-owner-per-decision (advisor source-of-truth table)

Each rendering decision must have exactly one owner. RenderWorld is the
owner of "what exists." Other decisions route to:

```
What exists?              -> RenderWorld
From where are we drawing? -> ViewUniforms / EngineView
What is visible?           -> VisibilityRequest
What does it look like?    -> MaterialGpu + AssetManifest
How is it submitted?       -> DrawPacket + PipelineDesc
What rendered this pixel?  -> ObjectID + DebugRenderer
Is this feature allowed?   -> RendererFeatureRegistry + DeviceCaps
```

This table is the answer to the projection-matrix split-brain class
(`terrainMVP` / `projectZ` / per-program upload). RenderWorld is one row;
each row has one owner; every renderer asks the owner.

### Open questions

- Q2.1: How are mission load/teardown lifecycles modeled? Lean: mission
  is a scope; `RenderWorld::beginMission()` / `endMission()` reset
  Mission-lifetime resources (per P2-1 ResourceLifetime taxonomy).
- Q2.2: Does `RenderWorld` own the camera, or only consume an `EngineView`
  built by game-side `GameCamera`? Lean: consume.

---

## 3. Handle model

### Concrete signature (proposed)

```cpp
// In RenderCore (no game / no GL headers)
template <typename Tag>
struct Handle {
    uint32_t index      : 20;  // slot index in registry (1M live max)
    uint32_t generation : 12;  // bumped on destroy; 4096 reuses before wrap
    [[nodiscard]] bool isValid() const noexcept;
    static constexpr Handle invalid() noexcept;
    bool operator==(Handle) const noexcept = default;
};

struct RenderObjectTag {};
struct ViewTag {};
struct MeshTag {};
struct MaterialTag {};
struct TextureTag {};

using RenderObjectHandle = Handle<RenderObjectTag>;
using ViewHandle         = Handle<ViewTag>;
using MeshHandle         = Handle<MeshTag>;
using MaterialHandle     = Handle<MaterialTag>;
using TextureHandle      = Handle<TextureTag>;
```

### Invariants

- Handles are OPAQUE: the holder must not interpret `index` or `generation`.
- Handles are STABLE across the lifetime of the registration. The slot is
  recycled only after generation bump.
- Handles are NEVER game pointers. A renderer that holds
  `BldgAppearance*` or `Mech3DAppearance*` is a boundary failure.
- `Handle::invalid()` is the only sentinel; do not overload with -1 / 0.
- Equality is bitwise; hashability is required (used in object-ID maps).
- Generation MUST be incremented when the slot is freed; a stale handle
  to a recycled slot fails `isValid()` lookup with a [SUBSYS v1] warning.

### Prior art (verified)

The static-prop registry already proves this shape, just without the
type-safe wrapper. `GameOS/gameos/gos_static_prop_registry.h` exposes:

- `int32_t registerRecipe(TG_MultiShape*, const std::vector<...>& batch)`
  returning a `recipeIndex` (sentinel -1).
- `void invalidate(int32_t regIdx)` to tombstone a slot.
- `bool isReady(int32_t regIdx)` for validity check.

The `recipeIndex` is the monotonic-handle prototype called out in
roadmap item 1. Two gaps from `Handle<RenderObjectTag>`:

1. No generation byte -- recycled slots can collide with stale references.
   This is acceptable for static props (which are mission-lifetime and
   never invalidated mid-mission in practice) but not for general
   RenderObjects, where mid-mission destroy/create is normal.
2. `int32_t` is untyped: a `recipeIndex` can be silently passed where a
   `MaterialIndex` is expected. The `Handle<Tag>` wrapper closes that.

### Open questions

- Q3.1: RESOLVED 2026-05-22 -- 20-bit index / 12-bit generation. 1M live
  objects (well above RTS max), 4096 slot reuses before wraparound. Mech
  destroy/respawn churn under sustained combat empirically tops out near
  hundreds of reuses per mission; 4096 holds with safety margin. Hot
  paths keep 32-bit handle. Wraparound, if it ever fires, asserts in
  debug and bumps a `[RENDER_WORLD v1] event=gen_wrap` banner. No 64-bit
  fallback handle in this version.
- Q3.2: Should there be an aliased `weak_handle` vs `strong_handle`
  distinction (refcounted holders)? Lean: no -- RenderWorld is the only
  owner; everyone else holds weak handles. Refcount-style ownership is
  reserved for shared `MaterialHandle`/`MeshHandle`.
- Q3.3: Cross-mission persistence: do `MaterialHandle` / `MeshHandle`
  persist across `endMission`/`beginMission` if the asset is shared?
  Lean: yes for Persistent-lifetime resources (per P2-1).

---

## 4. Render object lifecycle

### Lifecycle states

```
Unregistered  -- no handle exists
Registered    -- handle valid, RenderWorld owns the slot
Visible       -- this frame's visibility set admitted it (per-frame transient)
Submitted     -- a DrawPacket has been built for it this frame
Retired       -- destroy() called; handle invalid; slot pending recycle
```

State transitions are driven by:

- Game-side (via adapter): `Unregistered` -> `Registered` via
  `RenderWorld::upsertX(desc)`; `Registered` -> `Retired` via `destroy(h)`.
- Per-frame (extraction): `Registered` -> `Visible` via the
  VisibilityRequest result (Section 7).
- Per-frame (packet build): `Visible` -> `Submitted` via DrawPacket
  emission (Section 6).
- `Retired` slots are recycled at frame boundary AFTER any in-flight
  GPU work referencing them has retired (CPU-side defer; GPU-side
  fence is handled by `RenderDeviceGL` resource manager).

### Object descriptor (sketch)

```cpp
struct RenderObjectDesc {
    MeshHandle      mesh;
    MaterialHandle  material;
    Transform       worldTransform;     // initial; setTransform updates
    ArchetypeFlags  archetype;          // see below
    LayerMask       visibilityLayers;   // shadow / main / minimap / picking
    AABB            localBounds;        // optional; cook manifest may provide
    uint32_t        gameObjectId;       // OPAQUE engine-side cookie; not a pointer
};
```

`gameObjectId` is the engine's read-only echo of a game-side identifier
(used for object-ID buffer correlation per advisor Simplification 3).
It is NOT a pointer and RenderWorld must not dereference it.

### Renderable archetypes

Per roadmap item 9, archetypes are policy bundles, not game types:

```cpp
struct ArchetypeFlags {
    bool castsShadow         : 1;
    bool receivesShadow      : 1;
    bool selectable          : 1;
    bool usesImpostor        : 1;
    bool hasClusterLod       : 1;
    bool isSensorVisibleOnly : 1;   // S1 ViewMode interaction
    bool isOverlayOnly       : 1;   // C-track tactical overlay
    bool isStaticForMission  : 1;   // hint to capability resolver / cook
};
```

The capability resolver (Section 9) combines `ArchetypeFlags` with
`MeshCapability` from the asset manifest to pick a path.

### Update API (sketch)

```cpp
RenderObjectHandle RenderWorld::upsertStaticProp(const RenderObjectDesc&);
RenderObjectHandle RenderWorld::upsertMech     (const RenderObjectDesc& /*+ anim ref*/);
RenderObjectHandle RenderWorld::upsertVfx      (const RenderObjectDesc& /*+ emitter ref*/);

void RenderWorld::setTransform   (RenderObjectHandle, const Transform&);
void RenderWorld::setMaterial    (RenderObjectHandle, MaterialHandle);
void RenderWorld::setVisibility  (RenderObjectHandle, LayerMask);
void RenderWorld::destroy        (RenderObjectHandle);
```

`upsertX` is preferred over `create + setX` calls because:

- it makes the call atomic from the adapter perspective
- it matches the actual game-side use (BldgAppearance constructs a fully
  populated record before handing it over; no incremental population)
- it avoids partially-initialized slots being visible to extraction

Specialized constructors (`upsertStaticProp` vs `upsertMech`) exist so the
archetype is known at registration time; the capability resolver can
select the storage backend (static prop registry, mech batcher, etc.).

### Open questions

- Q4.1: Should `Transform` carry both current and previous (TAA prep)?
  Lean: yes -- adds one mat4 to ObjectData but unlocks S5 Stage 1 cleanly.
- Q4.2: Dirty tracking -- does `setTransform` mark a dirty bit consumed by
  extraction, or does extraction always re-pull all registered objects?
  Lean: dirty bit for ObjectData upload; full re-pull for visibility set.
- Q4.3: Skinned mechs -- where does bone data live? Lean: separate
  `SkinnedPoseHandle` table; `RenderObjectDesc` for mech carries it.
- Q4.4: Decals are first-class RenderObjects or a separate
  `DecalGpu[]` storage? Lean: separate (per S4 binning), with their own
  thin upsert API that mirrors this shape.

---

## 5. View model

### EngineView descriptor

```cpp
struct ViewDesc {
    Mat4         worldToView;
    Mat4         viewToClipGL;        // F1 unified convention
    Viewport     viewport;
    Frustum      frustum;             // derived; cached
    LayerMask    visibilityLayers;
    ViewMode     mode;                // S1 enum -- Visual/Thermal/etc.
    ShadowConfig shadow;              // optional; CSM cascade if applicable
    Mat4         prevWorldToClipGL;   // S5 TAA prep; optional
};

ViewHandle RenderWorld::createView(const ViewDesc&);
void       RenderWorld::updateView(ViewHandle, const ViewDesc&);
void       RenderWorld::destroyView(ViewHandle);
```

### Invariants

- `viewToClipGL` is in the unified GL convention from F1
  (`u_worldToClipGL` campaign, currently EXECUTING). Legacy MC2-pixel
  homogeneous convention is forbidden in any new `RenderWorld` view.
- Each view has its own `ViewUniforms` UBO slot upload. Multi-view rendering
  binds different UBO ranges, never reuploads per-program uniforms.
- `LayerMask` is the cross-system layer enumeration: `Main`, `ShadowSun`,
  `Minimap`, `Picking`, `Portrait`, `Reflection`, `TacticalOverlay`, ...
  Object visibility is the intersection of its `desc.visibilityLayers`
  and the view's `desc.visibilityLayers`.
- `ViewHandle` is stable across frames; the main camera, shadow camera,
  minimap camera, and any portrait/mechbay views all live as long-lived
  view handles.

### Phase 1 scope

Phase 1 ships with exactly ONE active view (the main camera). The view
type exists so the API shape is correct; multi-view scheduling is
roadmap item 7 and lands after F1. The shadow pass continues to use its
existing lightSpaceMatrix path in Phase 1; promoting it to a second
`EngineView` is a follow-up slice.

### Open questions

- Q5.1: Where does `lightSpaceMatrix` for shadows live during the
  shadow-as-View migration? Lean: a `ShadowView` builds it from main
  view + sun direction; UBO contains it; CSM cascades are sub-views.
- Q5.2: Does each view own its own `VisibilityResult`, or do all views
  share a single dispatch? Lean: each view -> own request (Section 7).
- Q5.3: Picking view is conceptually a `View` with a 1x1 viewport; does
  the API support it cleanly? Probably yes; this falls out of layer masks.

---

## 6. Draw packet model

### DrawPacket struct (sketch)

```cpp
struct DrawPacket {
    PipelineId        pipeline;        // PipelineDesc cache key
    MeshHandle        mesh;
    MaterialHandle    material;
    uint32_t          objectIndex;     // ObjectData[] slot -> world matrix
    uint32_t          lightIndex;      // LightsData[] slot (per-instance light)
    uint32_t          firstIndex;      // mesh sub-range / LOD level
    uint32_t          indexCount;
    uint32_t          instanceCount;   // 1 for non-instanced; N for batches
    uint32_t          sortKey;         // packed; see below
};
```

### Sort key packing (proposed bit layout)

```
[63:60]  pass priority  (opaque=0, alpha=8, overlay=12)
[59:56]  view priority  (main=0, shadow=4, minimap=8)
[55:32]  pipeline id    (24 bits -- groups by PipelineDesc cache key)
[31:16]  material id    (16 bits -- groups by texture array slice)
[15:0]   depth bucket   (for alpha sort) or fragment cost hint (opaque)
```

Sort order: ascending sortKey. Opaque sorts front-to-back via depth
bucket inversion at packet build (cheaper overdraw). Alpha sorts
back-to-front via depth bucket. This matches the existing master-node
ordering implicitly used in `txmmgr.cpp::renderLists()` but makes the
key explicit.

### Packet ownership

- Packets are `Frame`-lifetime (P2-1). They are built in
  `BuildDrawPackets` (named job graph) and consumed by `Render`.
- Packets are NOT persistent. Re-ordering one frame's packets does not
  affect the next frame's order.
- The packet array is the input to `glMultiDrawElementsIndirect` once
  pipeline boundaries are detected (consecutive packets with the same
  `pipeline + mesh + material` collapse into one indirect submission).

### Phase 1 scope

Phase 1 ships packets as a documentary intermediate. The first slice
(Section 13) still emits the existing `GpuStaticPropBatcher::flush()`
indirect commands. Packets are constructed but then immediately
translated into the same indirect command stream -- no behavior change.
This is the "route-only" rule from advisor Edit 2.

### Open questions

- Q6.1: Pipeline id is 24 bits -- enough for the lifetime of the engine?
  Lean: yes; PipelineDesc is finite and grows slowly.
- Q6.2: How are decals / particles modeled as packets? They have variable
  per-frame counts, no persistent handles. Lean: a `Vfx` packet variant
  with its own indirect path; do not force them into the static-shape mold.
- Q6.3: Cross-pass packet reuse (main + shadow) -- emit once and tag with
  multiple pass priorities, or emit per pass? Lean: emit per pass for
  Phase 1; revisit when ShadowView is a first-class EngineView.

---

## 7. Visibility service

### API surface

```cpp
struct VisibilityRequest {
    ViewHandle   view;
    LayerMask    layers;
    ImportanceHints importance;        // S2 prep; optional
};

struct VisibilityResult {
    Span<const RenderObjectHandle> visibleStaticProps;
    Span<const RenderObjectHandle> visibleMechs;
    Span<const RenderObjectHandle> visibleVfx;
    Span<const TerrainChunkId>     visibleTerrainChunks;
    uint32_t                       version;
};

VisibilityResult RenderWorld::computeVisibility(const VisibilityRequest&);
```

### Invariants

- VisibilityRequest is the ONLY public way to ask "what is visible."
  No subsystem peeks into `gpu_cull` outputs directly.
- The request is dispatched to the existing subsystem culls
  (`gpu_cull.comp` for static props, `MC2_GPU_MECH_CULL` for mechs,
  `gpu_driven_terrain_solid.comp` for terrain). Phase 1 wraps these
  rather than unifying them.
- `LayerMask` controls which subsystems dispatch. A `Picking` request
  with no terrain layer skips the terrain cull dispatch.
- `version` increments only when the underlying admitted set changes,
  enabling consumer skip (mirrors the proposed S11
  `TacticalVisibilityResult.version` pattern -- sibling spec, not yet
  shipped; included here as design alignment, not as established
  precedent).

### Distance culling rule (load-bearing)

Per `memory/distant_buildings_render_at_lower_lod_never_distance_culled.md`:
**static props (buildings) must NEVER be distance-culled out of the visible
set.** They drop LOD instead (impostor / lower mesh). The VisibilityRequest
service must encode this: static-prop layer admission considers frustum +
occlusion only, never far-plane distance. LOD choice is downstream of
admission (Section 9 capability resolver).

### Phase 1 scope

A thin wrapper. Each existing cull dispatch keeps its current SSBO layout
and binding points. `computeVisibility` is the only entry point new code
should call; legacy paths continue to call subsystem culls directly until
their migration slice (Section 10).

### Open questions

- Q7.1: Does `computeVisibility` block on GPU readback? Lean: no -- the
  visibility set is consumed downstream of the cull dispatch in the same
  frame, indirect-style; no CPU readback.
- Q7.2: How are `Span` views into `VisibilityResult` invalidated when
  the next frame's cull dispatch runs? Lean: results are Frame-lifetime;
  holding them across frames is undefined behavior.
- Q7.3: S2 ImportanceHints -- when does this design land relative to V1
  baseline? Carry as an opt-in field; default-zero means "distance only."

---

## 8. Material / mesh / texture handles

### Handle wrappers

```cpp
MeshHandle     MaterialSystem::registerMesh   (const MeshDesc&);
MaterialHandle MaterialSystem::registerMaterial(const MaterialDesc&);
TextureHandle  MaterialSystem::registerTexture(const TextureDesc&);
```

These are NOT RenderWorld methods. RenderWorld consumes the handles but
does not create them; the asset-cook / material system owns the storage.

### Backend storage mapping

```
MeshHandle      -> { vbo offset, ibo offset, lod chain, meshlet chain, bounds }
MaterialHandle  -> MaterialGpu[]  slot (Tier 2 V1; KTX2 array indices)
TextureHandle   -> TextureArray slice (KTX2; or legacy GL texture binding
                                       in fallback path)
```

### Invariants

- Handles are stable across frames within their lifetime class
  (Persistent or Mission, per P2-1).
- The storage backend behind a handle is internal. RenderWorld does not
  know whether a `MeshHandle` resolves to a meshlet chain, a legacy VB,
  or an impostor atlas slice -- the capability resolver (Section 9) does.
- Texture binding is via TextureArray slice index, not raw GL texture ID.
  Per advisor Simplification 5, day-one V1 uses KTX2 texture arrays;
  bindless textures are deferred.
- LightsData SSBO slots stay handle-addressable but the per-instance
  light index is carried inside `DrawPacket.lightIndex`. The existing
  `LightsData` SSBO machinery (verified in `gameos_graphics.cpp` -- bound
  via `glGetProgramResourceIndex(shp, GL_SHADER_STORAGE_BLOCK,
  "LightsData")`) becomes RenderWorld-internal; consumers see only the slot.

### Phase 1 scope

`MaterialHandle` and `MeshHandle` are introduced as types in Phase 1,
but their backends remain the existing TG_MultiShape / GOS material
machinery. `MaterialSystem` is a thin facade. The full MaterialGpu SSBO
ships in Tier 2 V1; until then `MaterialHandle` resolves to the legacy
GOS material slot.

### Open questions

- Q8.1: Asset import order -- can a `MaterialHandle` be created before
  its underlying KTX2 texture is loaded? Lean: yes (Persistent slot
  reserved at import; resident bit cleared until texture upload).
- Q8.2: Material variants (faction colors, damage states) -- separate
  MaterialHandle per variant, or one handle with a per-instance
  variant index? Lean: per-instance variant index in `DrawPacket`.
- Q8.3: How is "missing material" represented in the fallback path
  (Section 9)? Lean: a reserved `MaterialHandle::missing()` sentinel
  resolves to a debug-magenta material; never crashes the draw.

---

## 9. Fallback and capability rules

### Capability resolver

```cpp
struct RenderPathDecision {
    RenderObjectHandle   object;
    MeshCapability       meshCaps;       // from asset manifest
    RenderableCapability renderCaps;     // from ArchetypeFlags
    PresentationBand     band;           // Track C zoom-band
    RendererFeatureMask  features;       // RendererFeatureRegistry
    DeviceCaps           device;         // RenderDeviceCaps
    ChosenPath           path;
    const char*          reason;         // for [RENDER_PATH v1] audit log
};

ChosenPath CapabilityResolver::resolve(const RenderPathDecision& in);
```

The resolver answers one question: "given this object's caps + the
runtime state, which render path do we use?" Per advisor Edit 4, every
decision is logged for audit; `[RENDER_PATH v1]` banners aggregate counts.

### Fallback paths

```
StaticPropIndirect     -- existing GpuStaticPropBatcher + indirect
StaticPropLegacy       -- master-node enqueue (pre-Track B path)
MechMeshletIndirect    -- Tier 3 cluster-LOD path (future)
MechGpuBatched         -- existing MC2_GPU_MECH_CULL path
MechLegacy             -- per-mech draw via mech3d.cpp legacy
ImpostorAtlas          -- Tier 3 impostor billboard (future)
IconOnly               -- Track C zoom-out icon (no 3D mesh)
MissingMaterial        -- magenta debug; never crashes
```

### Distance cull rule (verbatim from constraint)

```
Distant buildings must render at lower LOD; they must never be
distance-culled.
```

Source: `memory/distant_buildings_render_at_lower_lod_never_distance_culled.md`.

Implication: the resolver's distance branch ONLY chooses among LOD/impostor
paths. A `StaticPropIndirect` -> `ImpostorAtlas` -> `IconOnly` ladder; no
"do not draw" leaf. The renderer owns the fallback as a productized choice,
not a per-subsystem env-var gate (per roadmap item 19).

### Stock install compatibility (load-bearing)

Per `memory/stock_install_must_remain_playable.md`: every path in the
resolver MUST have a leaf that works on a stock asset install (no cooked
meshlets, no KTX2 texture arrays, no MaterialGpu SSBO). The
`StaticPropLegacy` / `MechLegacy` paths exist for this purpose and may
not be removed until the cook pipeline becomes the only supported path
(an explicit, future, gated decision -- not part of this spec).

### Feature flag relationship

Per advisor Simplification 4 + roadmap item 14: `MC2_*` env-vars are
overrides for the resolver, not the resolver. The resolver's path choice
is a function of (asset caps, device caps, feature registry). Setting
`MC2_GPU_MECH_CULL=0` forces `MechLegacy`; it does not BE the path
selection logic.

### Open questions

- Q9.1: Where does the resolver live -- in `RenderWorld` or in `MeshRenderer`
  / `MaterialSystem` modules? Lean: `RenderCore` (no game / no backend);
  RenderWorld holds an instance.
- Q9.2: How is the resolver tested? Lean: unit-test per (caps x feature
  x device) tuple; gated by P2-6 contract tests.
- Q9.3: Does the resolver memoize -- given the same inputs, same output --
  to avoid recomputing per-frame? Lean: yes; cache key is hash of inputs.

---

## 10. Legacy adapter migration plan

### Adapter pattern (advisor Addition 1, load-bearing)

Adapters are TEMPORARY bridges. They include both `gameData` headers
and `RenderWorld` headers; their job is to sync game-side appearance
state into the engine each frame, then disappear once the caller has
been moved off the legacy direct-into-engine path.

```cpp
// GameAdapters module (only place that may include both)
class StaticPropRenderAdapter {
public:
    RenderObjectHandle sync(RenderWorld&, const Appearance& app);
    void destroy(RenderWorld&, RenderObjectHandle h);
};

class MechRenderAdapter      { /* same shape, takes Mech3DAppearance */ };
class TerrainRenderAdapter   { /* same shape, takes Terrain chunk */ };
class VfxRenderAdapter       { /* same shape, takes VFX emitter */ };
class OverlayRenderAdapter   { /* HUD / decals / overlays */ };
```

### Sentinel translation at the boundary (load-bearing)

The Phase 1 first-slice path threads `RenderObjectHandle` through the
existing `GpuStaticPropRegistry` recipe path. The registry returns
`int32_t recipeIndex` with sentinel `-1`; `RenderObjectHandle` uses
`Handle::invalid()` as the only sentinel (Section 3 invariants). The
adapter MUST translate at the boundary, both directions:

```
-1 (recipeIndex)   <->   RenderObjectHandle::invalid()
```

`-1` MUST NOT leak upward past the adapter. `Handle::invalid()` MUST
NOT leak downward into legacy registry calls. The adapter is the only
place this translation is allowed; a free `int32_t` masquerading as a
handle anywhere above the adapter is a boundary failure.

### Migration order

```
Slice M1:    StaticPropRenderAdapter   (static props -- Section 13 first slice)
Slice M1.5: Object-ID buffer           (Section 11; inspection substrate)
Slice M2:    MechRenderAdapter         (mechs)
Slice M3:    TerrainRenderAdapter      (terrain chunks)
Slice M4:    VfxRenderAdapter          (particles, beams)
Slice M5:    OverlayRenderAdapter      (decals, HUD-adjacent overlays)
Slice M6:   (verify) no remaining game-side raw GL access; firewall locked
```

M1.5 sits between M1 and M2 by design (see Section 11 / Q11.2):
object-ID touches FBO attachments and pass behavior, which violates
M1's "route-only / no new renderer behavior" rule, but it must precede
M2 so MechRenderAdapter can use it for picking/debug/LOD diagnosis from
day one. M1 reserves the handle shape and object-index mapping that
M1.5 then writes to a real `R32_UINT` attachment.

Each slice ships as `route-only` per advisor Edit 2: no new renderer
behavior, no new LOD/material/visibility model, zero pixel delta.

### Adapter deletion criteria (substitutive-not-additive)

Per `memory/feedback_offload_must_be_substitutive_not_additive.md`,
adapters are not allowed to live forever. Each adapter's deletion
criteria is documented at write-time:

```
Adapter is deletable when:
  1. The corresponding game-side class has been refactored to call
     RenderWorld directly, OR
  2. The game-side class has been retired (the gameData type itself
     no longer exists in the migrated codebase), AND
  3. Tier1 smoke + parity probe confirm zero pixel delta for one
     full release without the adapter.

Adapter is NOT deletable just because RenderWorld now exists.
Removal is a separate explicit slice with the substitutive proof.
```

### Firewall rule (verbatim from roadmap)

```
GameAdapters may include gameData headers (BldgAppearance.h, mech3d.h, etc.).
RenderWorld and everything below may NOT include any gameData header.
Violation = boundary failure.
```

The include-graph checker enforces this. Phase 1 of the checker is a
shell script that greps for forbidden includes; Phase 2 is a CI gate.

### Open questions

- Q10.1: Where does the include-graph checker live and how is it run?
  Lean: `scripts/check-include-firewall.sh`; runs in pre-commit when any
  RenderWorld / GameAdapters file changes.
- Q10.2: Can an adapter be a header-only template, or must it be a real
  TU to avoid pulling gameData headers transitively? Lean: real TU.
- Q10.3: How do we prevent a future "convenience" adapter that grows into
  a parallel game-render system? Lean: adversarial review at each slice;
  every adapter must declare its deletion criteria up front.

---

## 11. Debug / audit requirements

### `[RENDER_WORLD v1]` frame summary banner

Per roadmap item 18 + advisor Edit 4. Once per frame:

```
[RENDER_WORLD v1] objects=N visible=M packets=P views=V
[RENDER_PATH v1]  StaticPropIndirect=A LegacyMesh=B MissingMaterial=C IconOnly=D
[VISIBILITY v1]   main_admitted=X shadow_admitted=Y picking_admitted=Z
```

These banners follow the established `[SUBSYS v1]` convention
(documented in MEMORY.md INDEX-SMOKE-TEST). They are env-gated:

- `MC2_RENDER_WORLD_TRACE=1` -> per-frame banners
- always-on -> monotonic 600-frame summary (matches TGL pool precedent)

### ObjectID buffer integration (advisor Simplification 3)

Per advisor: object-ID is promoted from "Tier 2 prerequisite" to
**Tier 1.5 mandatory inspection substrate**. The boundary spec must
require that every `RenderObjectHandle` is recoverable from a pixel:

```
Pixel -> ObjectID -> RenderObjectHandle
                     -> MeshHandle + MaterialHandle
                     -> LOD level
                     -> PipelineId
                     -> DrawPacket index
                     -> RenderPathDecision.reason
```

This is the inspection substrate that makes every later feature
(PBR, shadows, decals, LOD) debuggable without a real editor.

`MC2_OBJECT_ID_BUFFER=1` opts in. The buffer is an `R32_UINT` attachment
on the main FBO carrying the packed `(handle.index, handle.generation)`.

### Audit log discipline

Every `RenderPathDecision` is logged in debug builds. In release,
aggregate counts are emitted at frame end. This prevents fallback
behavior from becoming magical (the static-prop pop-in investigation
of 2026-05-22 took longer than necessary because the path choice was
implicit -- this fixes that class).

### Validation gates (Phase 1 -> Phase 2 -> Phase 3)

Mirrors the `render_contract.h` Phase 1/2/3 ladder:

- Phase 1 (documentary): types exist; no runtime enforcement.
- Phase 2 (debug assertions): debug builds verify boundary rules
  (no gameData header reached engine, handles validated on use, slot
  recycle generation gated).
- Phase 3 (enforced): release builds fail loudly on boundary violation;
  include-graph checker in CI.

### Open questions

- Q11.1: How verbose is the per-frame banner in release? Lean: monotonic
  summary only; per-frame requires env-gate.
- Q11.2: RESOLVED 2026-05-22 -- Object-ID lands as **Slice M1.5**,
  AFTER M1 (route-only static-prop migration) and BEFORE M2
  (`MechRenderAdapter`). M1 stays pure: no new renderer behavior, no
  FBO attachment change, same pixels. M1 DOES reserve the handle shape
  and the object-index mapping; M1.5 then adds the `R32_UINT`
  attachment, packs `(handle.index, handle.generation)` per pixel, and
  wires the picking/debug lookup. M2's `MechRenderAdapter` inherits the
  inspection substrate from day one. Substrate is **mandatory** by the
  end of M1.5; M1 alone does not satisfy "every handle recoverable
  from a pixel".
- Q11.3: How is the include-graph checker bootstrapped given the current
  worktree mixes new and legacy code? Lean: allow-list of legacy TUs;
  shrink allow-list as Slice M1..M5 land.

---

## 12. Forbidden dependencies (advisor Addition 2, expanded 2026-05-22)

These two lists are the load-bearing firewall. Any future session that
proposes violating either side is creating a boundary failure.

This section deliberately broadens the roadmap's Addition 2 list (see
roadmap lines 482-503) along two axes: it is **module / category-based**
rather than only named-type-based, and it covers **all dependency
shapes** (includes, forward-decls, signatures, fields, templates).
The roadmap entry is preserved as exemplar; the expansion is required
to match the actual class hierarchy and to prevent forward-decl creep
that an include-only checker would miss. The roadmap should be amended
in the same merge that promotes this spec to EXECUTABLE.

### RenderWorld must not depend on:

- gameData concrete types
- the legacy `Appearance` hierarchy (`Appearance`, `ObjectAppearance`,
  `BldgAppearance`, `TreeAppearance`, `GVAppearance`, `Mech3DAppearance`,
  `GenericAppearance`, and any future subclass)
- `Mission` or any mission / gameplay header
- `ObjectManager`, `warrior.*`, and any mission orchestration source
- legacy engine-side appearance classes -- e.g. `Mech3DAppearance` --
  EVEN when they historically lived near rendering code. These are
  migration inputs (consumed by adapters), not engine API types.
- OpenGL global state outside `RenderDevice` / `RenderContext`

### gameData must not depend on:

- GL buffer IDs
- shader program IDs
- SSBO/UBO binding points
- material table indices
- indirect draw command layout
- GPU sync primitives

### Dependency shapes (load-bearing)

Forward declarations count as architectural dependencies. The firewall
applies to all of:

```
includes, forward-declarations, function signatures, member fields,
template parameters, typedef/using aliases, friend declarations
```

Outside `GameAdapters`, the modules above (RenderCore, RenderWorld,
Visibility, MeshRenderer, MaterialSystem, DebugRenderer, RenderDeviceGL)
may not name a forbidden game-side type in any of those shapes.

`GameAdapters` is the ONLY module that may bridge both sides. Adapter
**headers** may forward-declare game-side types; adapter **.cpp files**
may include the real game-side headers. No other module may do either.

Example:

```cpp
// OK -- in GameAdapters/StaticPropRenderAdapter.h
class Appearance;  // forward-decl in adapter is allowed

class StaticPropRenderAdapter {
public:
    RenderObjectHandle sync(RenderWorld&, const Appearance&);
};
```

```cpp
// NOT OK -- in RenderWorld/RenderWorld.h
class Appearance;  // forward-decl outside adapter is forbidden
RenderObjectHandle upsertStaticProp(const Appearance&);
```

The engine-facing `RenderWorld::upsertStaticProp` MUST take a
`RenderObjectDesc` value (or equivalent engine type), never a
game-side appearance pointer or reference.

### Enforcement plan

1. Phase 1: document this list in the spec (this section) + adversarial
   review at each migration slice greps for violations.
2. Phase 2: `scripts/check-include-firewall.sh` -- forbid-list of
   **headers** that RenderWorld/* may not include, PLUS a forbid-list of
   **symbol names** (case-sensitive grep across `RenderCore/`,
   `RenderWorld/`, `Visibility/`, `MeshRenderer/`, `MaterialSystem/`,
   `DebugRenderer/`, `RenderDeviceGL/`) that catches forward-decls,
   typedef bridges, and signature uses an include-only checker misses.
   Checked pre-commit.
3. Phase 3: CI gate. Build fails on violation in non-adapter TUs.

### Vulkan-prep restatement

Each forbidden-dep above is also Vulkan-shape negative space. Vulkan
does not have "GL global state"; if a game-side caller required global
GL state, it would not survive the backend swap. The forbidden-dep list
is therefore a forward-compatibility list, not a stylistic preference.

### Open questions

- Q12.1: How are inline templates handled (do they count as "depending
  on" a header)? RESOLVED 2026-05-22: yes; templates that name forbidden
  types in their signature are dependencies. Adapter-only.
- Q12.2: Adapter-internal helpers (a free function in
  `StaticPropRenderAdapter.cpp` that takes `const Appearance&` and a
  `RenderObjectDesc*`) -- where do they live? RESOLVED 2026-05-22: in
  the adapter TU itself; not in RenderCore. Anonymous-namespace helpers
  inside the adapter `.cpp` are preferred over header helpers so the
  game-side type never leaks into any header.

---

## 13. First migration target (advisor Addition 3)

### The slice

```
all current GpuStaticPropRegistry client Appearance subclasses
    -> StaticPropRenderAdapter::sync(RenderWorld&, const Appearance&)
    -> RenderWorld::upsertStaticProp(desc)
    -> MeshHandle + MaterialHandle resolved (Phase 1: legacy backends)
    -> DrawPacket emitted (Phase 1: documentary; immediately translated
       to existing indirect command stream)
    -> existing GpuStaticPropRegistry path (recipeIndex, markVisible)
    -> existing GpuStaticPropBatcher::flush() -> same glDrawElementsIndirect
    -> zero pixel delta vs legacy
```

Slice scope is **all production producers of `GpuStaticPropRegistry`
state**, not buildings alone, and not "Appearance subclasses" alone.
Production producers include every live call site that creates,
registers, invalidates, marks-visible, or destroys a registry record.
Half-migrated state is explicitly disallowed; the slice closes only
when every production producer routes through the adapter.

### Required-in-M1 audit predicate

A call site is required-in-M1 if it is a production producer of static
prop registry state. Concretely:

```
registerRecipe       producers (creates a recipe)
registerStaticProp   producers (late-spawn entry point; calls registerRecipe)
invalidate           producers (tombstones a slot)
markVisible          producers (per-frame visibility marking, if coupled to
                                ownership; otherwise demoted to follow-up)
destroy / unregister producers
```

### Confirmed in-scope call sites (audited 2026-05-22)

- `mclib/bdactor.cpp:1471` -- `BldgAppearance` first-render fallback
- `mclib/bdactor.cpp:2802` -- `BldgAppearance` bulk-register path
- `mclib/bdactor.cpp:4269` -- `TreeAppearance` first-render fallback
- `mclib/bdactor.cpp:4855` -- `TreeAppearance` bulk-register path
- `code/warrior.cpp:7593` -- late-spawn `registerStaticProp` entry point

`warrior.cpp:7593` is the late-spawn registration entry point and is
**inside** M1. Allowing it to bypass the adapter would create a
two-truth split (`bdactor` props go through `RenderWorld`, late-spawned
warrior props go direct to the registry) and violate the no-half-migrated
rule above. M1 closes only when all five sites route through
`StaticPropRenderAdapter` (or a sibling helper in `GameAdapters`).

### Audit exemptions

A call site may be exempted from M1 only with grep evidence that it is:

- dead code (unreferenced),
- test-only code (under `tests/`), or
- diagnostic-only code (gated by an `MC2_*` env-var that is OFF by
  default and the documented graduation contract excludes it).

Verify each exemption at slice start and record it in the slice plan.

### Audit instruction

Re-grep `GpuStaticPropRegistry::register` and `registerRecipe` at slice
plan time. The audit list above was captured 2026-05-22; if call sites
have changed at execution time, the slice scope adjusts accordingly
(but cannot shrink below "all production producers").

### Why static props first (verbatim from roadmap)

1. `GpuStaticPropRegistry` already proves the handle model -- routing
   through `RenderWorld` is an API-shape exercise, not a new GPU path.
2. Buildings are less animation-heavy than mechs.
3. They are already the cluster-LOD PoC target (Sequence B).
4. They exercise: handles, materials, draw packets, visibility, fallback.

### Rules for the first slice (verbatim from advisor Edit 2)

```
No new renderer behavior.
No new LOD decision.
No new material model.
No new visibility model.
Only a new boundary.
```

### Correctness gate

```
Tier1 smoke: 5/5 pass, byte-identical or visually-identical frames vs HEAD
Parity probe: existing static-prop probes show zero admission delta
[STATIC_PROP_REGISTRY v1]: counts unchanged vs pre-slice baseline
[RENDER_WORLD v1]: emits with objects=N matching prop count
Adversarial review: include-graph clean; no gameData header in RenderWorld TU
```

### Out of scope for this slice

- Mechs, terrain, VFX, decals (deferred to Slice M2..M5)
- Object-ID buffer (Tier 1.5, can run in parallel)
- MaterialGpu SSBO (Tier 2 V1)
- New cull dispatch (existing gpu_cull.comp unchanged)
- ShadowView promotion (Phase 1 view scope -- one view only)
- Adapter deletion (criteria documented; deletion is a future slice)

### Slice deliverables

```
1. RenderCore/Handle.h               -- Handle<Tag> template
2. RenderCore/RenderObjectDesc.h     -- desc + archetype flags
3. RenderCore/DrawPacket.h           -- packet struct + sort key fields
4. RenderWorld/RenderWorld.h         -- API surface (this spec, in code)
5. RenderWorld/RenderWorld.cpp       -- thin forwarder to existing backends
6. GameAdapters/StaticPropRenderAdapter.{h,cpp}
7. scripts/check-include-firewall.sh -- Phase 1 grep enforcement
8. Tier1 smoke + parity probe verification before merge
9. [RENDER_WORLD v1] banner; opt-in MC2_RENDER_WORLD_TRACE
10. Adversarial review report (referenced; not part of code)
```

### Open questions

- Q13.1: RESOLVED 2026-05-22 -- slice scope is the FULL set of Appearance
  subclasses currently registering with `GpuStaticPropRegistry`. No
  half-migrated state. Audit step: enumerate `registerRecipe` call sites
  at slice start and commit the adapter to covering all of them.
- Q13.2: How is the adapter wired into each Appearance subclass's
  current `render()` -> `registerRecipe()` path? Lean: the
  adapter calls into the same `GpuStaticPropRegistry` namespace; only
  the call SITE moves into the adapter TU. One adapter TU may
  internally fan out per Appearance subclass.
- Q13.3: Cluster-LOD PoC overlap -- does this slice block, or land
  alongside, the cluster-LOD PoC Phase 0? Lean: alongside; PoC consumes
  the new boundary once it exists.

---

## Appendix A. Verified prior art (grep-confirmed 2026-05-22)

- `GameOS/gameos/gos_static_prop_registry.h` -- `GpuStaticPropRegistry`
  is a **namespace** (not a class). Exposes `registerRecipe(...)` returning
  `int32_t recipeIndex` (sentinel -1), `invalidate(int32_t)`, `isReady(...)`,
  `markVisible(int32_t, uint32_t lightDataIndex, float extentRadius)`.
  This is the monotonic-handle prototype that informs Section 3.

- `mclib/render_contract.h` -- `enum class PassIdentity : std::uint8_t`
  and `struct PassStateContract`. Phase 1 documentary; informs the
  pass-side analogue for the boundary contract pattern in Section 11.

- `GameOS/gameos/gameos_graphics.cpp` -- `LightsData` SSBO bound via
  `glGetProgramResourceIndex(shp, GL_SHADER_STORAGE_BLOCK, "LightsData")`.
  Per-instance light index is the model for Section 6 `DrawPacket.lightIndex`.

- `mclib/appear.h` -- base `Appearance` class (the actual base that
  building / tree / gv subclasses derive from). The "BldgAppearance"
  name used in the roadmap is the conceptual building subclass; the
  actual class hierarchy uses `Appearance` as the polymorphic base.
  Section 10 adapter signatures take `const Appearance&` accordingly.

- `mclib/mech3d.h` -- `Mech3DAppearance` engine-side mech appearance class (line 299; derives from `ObjectAppearance`); not a game
  AI type. Section 10 `MechRenderAdapter` consumes this.

- The `GpuStaticPropBatcher` and `gpu_mech_batcher` symbols are referenced
  by name in `gameos_graphics.cpp`, `gos_static_prop_batcher.cpp`, and
  `gos_mech_batcher.cpp`. These are the indirect-draw backends behind
  the existing fast paths; Section 13's first-slice translation lands
  them as the storage backends for `MeshHandle`/`MaterialHandle` in Phase 1.

Line numbers intentionally omitted from this draft; the adversarial-plan-review
pass will re-grep all symbol references at write-time and add them or flag
drift.

---

## Appendix B. Glossary

- **Adapter** -- a temporary GameAdapters-module class that bridges a
  gameData appearance class to RenderWorld. Includes both sides.
- **ArchetypeFlags** -- policy bits (shadow/selectable/impostor) carried
  in `RenderObjectDesc`. Drives the capability resolver.
- **DrawPacket** -- frame-lifetime unit of work; the renderer-facing
  equivalent of a draw call.
- **EngineView** -- a `ViewHandle` + `ViewUniforms`. Multi-view foundation.
- **Handle** -- opaque `(index, generation)` pair. Never a pointer.
- **LayerMask** -- cross-system layer enumeration for visibility.
- **PresentationBand** -- Track C zoom-band input to the capability
  resolver (advisor Edit 3).
- **RenderObject** -- the engine-side projection of a game entity for
  rendering purposes. Owned by RenderWorld.
- **RenderPathDecision** -- logged record of (caps + features + device)
  -> chosen path. Audit substrate.
- **VisibilityRequest / VisibilityResult** -- the only public API for
  "what is visible." Wraps existing cull dispatches in Phase 1.

---

## Appendix C. Cross-spec references

- `docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md`
  -- parent roadmap; this spec is item 20.
- `docs/superpowers/specs/2026-04-26-render-contract-registry-design.md`
  -- Pass Contract Registry; sibling spec referenced by Section 6 + 11.
- `docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-atomic-design.md`
  -- F1 ViewUniforms UBO; prerequisite for Section 5 view model.
- `docs/superpowers/specs/2026-05-19-static-prop-cluster-lod-poc-design.md`
  -- cluster-LOD PoC; Section 9 capability resolver consumer.
- `docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md`
  -- asset cook; provides Section 8 MeshHandle backends in Tier 3.
- `memory/distant_buildings_render_at_lower_lod_never_distance_culled.md`
  -- Section 9 distance-cull rule source.
- `memory/stock_install_must_remain_playable.md`
  -- Section 9 stock-compat fallback constraint.
- `memory/vulkan_prep_explicit_device_discipline.md`
  -- backend-shape discipline; informs every section.
- `memory/feedback_offload_must_be_substitutive_not_additive.md`
  -- Section 10 adapter deletion criteria rationale.

---

## Appendix D. Brainstorm-pass aggregated open questions

Compiled for the follow-up adversarial-review / greybeard pass. Resolve
each before promoting this spec from DRAFT to EXECUTABLE.

```
Q1.1  FrameAllocator ownership boundary
Q1.2  Threading model (single-writer vs concurrent)  -- DEFERRED 2026-05-22, revisit when extraction phase (roadmap item 3) lands
Q1.3  Light table ownership relationship
Q2.1  Mission lifecycle scope semantics
Q2.2  Camera ownership (RenderWorld vs GameCamera)
Q3.1  Handle bit-split + generation wraparound plan  -- RESOLVED 2026-05-22 (20/12)
Q3.2  Weak vs strong handle distinction
Q3.3  Cross-mission asset handle persistence
Q4.1  prevTransform in ObjectData (TAA prep)
Q4.2  Dirty bit vs full re-pull semantics
Q4.3  Skinned pose handle table
Q4.4  Decals as RenderObjects vs separate
Q5.1  ShadowView lightSpaceMatrix migration
Q5.2  Per-view vs shared visibility dispatch
Q5.3  Picking view as 1x1 viewport
Q6.1  Pipeline-id bit width
Q6.2  Decal/particle packet variant
Q6.3  Cross-pass packet reuse
Q7.1  CPU readback blocking
Q7.2  Span lifetime across frames
Q7.3  ImportanceHints landing relative to V1
Q8.1  Asset import ordering (Material before Texture)
Q8.2  Material variants (per-handle vs per-instance)
Q8.3  MissingMaterial sentinel design
Q9.1  Capability resolver module placement
Q9.2  Resolver unit-test discipline
Q9.3  Resolver memoization
Q10.1 Include-graph checker location and trigger
Q10.2 Adapter as template vs TU
Q10.3 Adapter creep prevention
Q11.1 Banner verbosity in release
Q11.2 Object-ID buffer timing relative to first slice  -- RESOLVED 2026-05-22 (Slice M1.5)
Q11.3 Include-checker allow-list bootstrap
Q12.1 Inline-template dependency semantics  -- RESOLVED 2026-05-22 (templates count; adapter-only)
Q12.2 Adapter-internal helper placement  -- RESOLVED 2026-05-22 (anonymous-namespace in adapter .cpp)
Q13.1 First-slice scope (buildings-only vs all static props)  -- RESOLVED 2026-05-22 (all GpuStaticPropRegistry clients)
Q13.2 TreeAppearance wiring detail
Q13.3 Cluster-LOD PoC overlap
```

End of brainstorm draft.
