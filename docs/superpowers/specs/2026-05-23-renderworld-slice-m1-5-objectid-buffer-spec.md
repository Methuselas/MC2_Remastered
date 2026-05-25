# RenderWorld Slice M1.5 -- ObjectID Buffer Spec

- Status: EXECUTABLE-READY (brainstorm + scope narrowing + adversarial
  review pass applied 2026-05-23; greybeard pass deferred to first slice
  execution per skill scope)
- Adversarial review:
  `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-5-spec-adversarial-review.md`
  (1 CRIT / 3 MAJOR / 6 MINOR; all CRIT + MAJORs resolved in this spec;
  m1/m2/m5 documented inline; m3/m4/m6 noted for future slices)
- Remaining follow-ups before EXECUTING:
  - greybeard skill pass at first slice execution (META-FIX vs PATCH
    on the setSceneDrawBuffers helper centralization)
  - codex sign-off on the FBO MRT shape if changes from this revision
- Date: 2026-05-23
- Relation to roadmap: realization of item 10 (`MC2_OBJECT_ID_BUFFER`) of
  `docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md`;
  Section 11 (Debug / audit requirements) of
  `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`;
  Q11.2 resolution (M1.5 lands between M1 and M2).
- Parent spec: `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`
  (Sections 3 handle, 10 adapter migration, 11 debug/audit).
- Predecessor slice: RenderWorld Slice M1 SHIPPED 2026-05-23
  (`docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`).
- Scope narrowing 2026-05-23: M1.5 is a **render/debug substrate slice
  ONLY**. Picking integration (the missiongui.cpp click-handler wiring
  + hybrid selection lifecycle) is split out as a separate **M1.6**
  slice. Brainstorm discovered there is no CPU static-prop selection
  at M1 HEAD (existing selection is mover-only via 2D screen bounds in
  `code/missiongui.cpp`), so M1.5 does not "replace" anything; it
  introduces the substrate. Wiring is a follow-up, not part of this
  slice. Section 8 retains the lifecycle design for M1.6 reference but
  is explicitly OUT of scope for M1.5 execution.
- DOC-ONLY: no code in this artifact. Pseudocode for API signatures only.

This document specifies the **Tier 1.5 mandatory inspection substrate**
called out by the RenderWorld boundary spec advisor Simplification 3.
It promotes the parent spec's Section 11 "ObjectID buffer integration"
sketch to an executable slice plan input: pixel -> handle -> mesh /
material / LOD / pipeline / packet / path chain, recoverable for every
static-prop pixel in the scene by the end of M1.5.

---

## 1. Purpose / non-goals

### Purpose

Provide the smallest viable substrate that satisfies the boundary spec's
Section 11 promise: every `RenderObjectHandle` is recoverable from a
pixel. M1.5 delivers this **for static props only**. The FBO attachment,
shader-output contract, RenderWorld lookup table, and `lookupAtPixel`
debug API land in this slice so every later slice (M2 mechs, M3
terrain, M4 VFX, M5 overlays) extends a working substrate rather than
litigating it. Picking integration (gameplay-visible selection wiring)
is M1.6, separate slice — the substrate must work and be inspectable
via debug log without any UX behavior change in M1.5.

The slice is the inspection foundation that makes every later renderer
feature (PBR, dynamic shadows, decals, cluster-LOD) debuggable without
an editor. Without it, "why is this pixel wrong" remains a multi-hour
bisect; with it, the answer is a hover.

### Non-goals (explicit)

- **Not gameplay selection wiring (M1.6).** M1.5 does NOT modify
  `code/missiongui.cpp` click-handling or any existing selection code
  path. The substrate must be inspectable via debug API + log, but no
  user-visible click behavior changes in this slice. M1.6 is the
  separate slice that wires `lookupAtPixel` into the missiongui click
  path. Section 8 retains the design for M1.6 reference but is
  explicitly out of M1.5 execution scope.
- Not GPU picking for ALL passes. Mechs, terrain, VFX, immediate-mode
  passes do NOT write IDs in M1.5. They leave the attachment cleared to
  zero (`Handle::invalid()`). M2 extends; coverage closes by M5.
- Not async readback infrastructure. Single-pixel synchronous
  `glReadPixels` is acceptable for the debug API (and for M1.6 picking
  later). PBO double-buffer async path is mentioned as future work; not
  in this slice.
- Not drag-rectangle selection. Even M1.6 will be single-click only.
  Drag-rect stays on existing 2D screen-bounds path indefinitely.
- Not a generic "scene inspector" UI. The slice ships a programmatic
  `RenderWorld::lookupAtPixel(x, y)` API only; no in-engine HUD overlay
  beyond the existing `[RENDER_WORLD v1]` banner.
- Not a Vulkan port of the buffer. The attachment is GL-shaped; design
  notes call out Vulkan equivalence but the backend is GL.
- Not retirement of CPU-side picking. The existing mover-selection path
  in `missiongui.cpp` is untouched. No deletion criteria apply in M1.5.

### Open questions (carry to next pass)

See Section 14.

---

## 2. Relationship to M1, M2, and roadmap item 10

### M1 (SHIPPED 2026-05-23)

M1 introduced `RenderObjectHandle` and routed static-prop registration
through `RenderWorld::upsertStaticProp` -> `GameAdapters` ->
`RenderWorld/legacy/static_prop_backend.{h,cpp}` ->
`GpuStaticPropRegistry::registerRecipe`. M1 was strictly route-only: no
new renderer behavior, no FBO change, byte-identical pixels (tier1 5/5
PASS).

M1 reserved the handle shape (20-bit index, 12-bit generation; see
`RenderCore/Handle.h:28-59`) and the index<->slot mapping inside
RenderWorld. M1.5 builds on this: the same `Handle.raw()` value is what
gets emitted per pixel by the static-prop fragment shader.

### M1.5 (this slice)

- Adds a single MRT attachment (`R32_UINT` colour buffer) to the main
  scene FBO.
- Extends the static-prop fragment shader to emit `Handle.raw()` at a
  new `layout(location=2)` (location 1 is already in use by
  `GBuffer1` -- see Section 3).
- Adds a per-slot inspection table to RenderWorld (mesh / material /
  LOD / pipeline / packet / path / generation per handle index).
- Exposes a debug API `RenderWorld::lookupAtPixel(int x, int y)` ->
  populated `LookupResult` struct.
- Wires GPU pixel lookup as the FIRST CHOICE for static-prop selection;
  existing 2D screen-bounds / CPU-side fallback remains for unsupported
  passes.

M1.5 is a small but invasive slice: it touches one fragment shader, one
FBO setup site, and adds three new files (header for `LookupResult`,
header+TU for the per-slot record table). Tier1 5/5 must remain green
with the env var OFF; visual canary mission runs with env var ON to
confirm picking-replacement parity.

### M2 (next; not this slice)

M2 adds `MechRenderAdapter`. The mech fragment shader gains the same
`out uint` declaration, mech-side `upsertMech` populates the inspection
table with mech-archetype fields, and picking-selection for mechs
flips from CPU 2D-bounds to GPU pixel-lookup. The attachment, clear
value, lookup API, and hybrid-fallback machinery are unchanged --
M1.5's investment pays out as the M2 implementation cost shrinks to
"write to attachment + populate table."

### Roadmap item 10

Item 10 of `docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md`
called for an `MC2_OBJECT_ID_BUFFER` env var. M1.5 is the realization
of that item; the env var name is preserved.

---

## 3. FBO architecture

### Current main-scene FBO layout (verified, grep at write-time)

The main scene FBO (`gosPostProcess::sceneFBO_`) currently has the
following attachments (created in `GameOS/gameos/gos_postprocess.cpp`
`createFBOs()`):

```
GL_COLOR_ATTACHMENT0 -> sceneColorTex_  (RGBA16F)  -- scene colour
GL_COLOR_ATTACHMENT1 -> sceneNormalTex_ (RGBA16F)  -- GBuffer1 (normal +
                                                     screen-shadow flag)
GL_DEPTH_STENCIL_ATTACHMENT -> sceneDepthTex_
```

`glDrawBuffers(2, {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1})` is
called in `createFBOs()` and re-asserted in the bind path; see
`gos_postprocess.cpp` around the `MRT remains bound for the entire scene
draw` comment.

**Implication for M1.5:** the ObjectID attachment is `GL_COLOR_ATTACHMENT2`,
NOT `GL_COLOR_ATTACHMENT1` as the user-resolved decision implied. The
parent spec's "2nd color attachment" wording was written before M1.5
authoring discovered GBuffer1 was already location 1. The attachment is
spelled "GBuffer2" in `[RENDER_CONTRACT]` comments for consistency with
the existing GBuffer1 naming.

### M1.5 FBO additions

- New texture: `sceneObjectIdTex_` (`GL_R32UI`, screen resolution,
  `GL_NEAREST` filter, `GL_CLAMP_TO_EDGE` wrap).
- New attachment slot: `GL_COLOR_ATTACHMENT2`.
- `glDrawBuffers` call list becomes
  `{GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2}`
  when `MC2_OBJECT_ID_BUFFER=1`; otherwise stays at the existing 2-buffer
  list (env-OFF leaves the attachment unbound and dormant).
- Per-frame clear: `glClearBufferuiv(GL_COLOR, 2, {0, 0, 0, 0})` runs at
  scene-FBO clear time (immediately after the existing colour/depth
  clear in `gameosmain.cpp`). `Handle::invalid().raw() == 0` matches
  this clear value exactly -- background pixels naturally resolve to
  "no object."

### Format choice rationale

- `GL_R32UI` (integer single-channel 32-bit unsigned) carries the full
  `Handle.bits` value without any float quantization. `Handle.raw()` is
  already `uint32_t`; no packing needed.
- Integer-format colour attachments require integer-format shader
  outputs (`out uint`, not `out vec4 ...`). Standard GL 4.3+. Vulkan
  equivalent: `VK_FORMAT_R32_UINT` with corresponding pipeline output.
- Bandwidth cost: 4 bytes/pixel at screen res. At 1920x1080: ~8 MB per
  frame attachment storage; per-pixel write cost is ~one extra raster
  store. See Section 10.

### Multi-pass interaction

The main-scene FBO is bound for the entire scene draw including
terrain, mechs, static props, water, VFX. The MRT attachment list is
set ONCE at FBO setup; all subsequent passes share the same bound
attachments. Passes that do not emit a `location=2` output leave it
undefined per the GL spec -- which is why **M1.5 mandates that the
attachment is CLEARED to zero at frame start AND that the static-prop
fragment shader is the only program declaring an output at location 2
in this slice**. Other passes' `gl_FragData[2]` is undefined; this is
acceptable because we clear-then-write-on-top. Section 6 makes this
explicit.

### CRITICAL: centralized setSceneDrawBuffers() helper (C1 resolution)

Multiple post-process bind sites in `gos_postprocess.cpp` re-issue
`glDrawBuffers(1, &singleBuf)` after a `glBindFramebuffer(...sceneFBO_)`.
These calls structurally DROP attachment-2 from the active write mask.
The adversarial-review C1 finding requires a centralized helper so the
main scene bind always re-asserts the full draw-buffer list (including
attachment-2 when object-ID is enabled) and the single-color
postprocess passes stay deliberately single-color.

The helper is `gos_postprocess.cpp`-local (file-scope `static`):

```cpp
// gos_postprocess.cpp-local helper
enum class SceneDrawBufferMode { MainSceneMRT, SingleColor };

static void setSceneDrawBuffers(SceneDrawBufferMode mode) {
    if (!IsObjectIdBufferEnabled()) {
        if (mode == SceneDrawBufferMode::SingleColor) {
            GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, bufs);
        } else {
            GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, bufs);
        }
        return;
    }

    if (mode == SceneDrawBufferMode::SingleColor) {
        // Postprocess single-color pass. Deliberately no object-ID writes.
        GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, bufs);
    } else {
        // Main scene draw. Object-ID active.
        GLenum bufs[3] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2
        };
        glDrawBuffers(3, bufs);
    }
}
```

**Important nuance:** the helper is NOT "always append C2 everywhere."
Single-color postprocess passes remain single-color. The helper
centralizes scene-FBO draw-buffer POLICY so the MAIN scene bind
always gets C2 when object-ID is on, and single-color passes stay
deliberately single-color. The decision lives in one place.

Required call-site routing (all `glBindFramebuffer.*sceneFBO_` sites
that issue `glDrawBuffers`):

```
createFBOs():     setSceneDrawBuffers(MainSceneMRT)   // was glDrawBuffers(2, {C0, C1}) at :274
beginScene():     setSceneDrawBuffers(MainSceneMRT)   // was glDrawBuffers(2, {C0, C1}) at :418
runScreenShadow:  setSceneDrawBuffers(SingleColor)    // was glDrawBuffers(1, &singleBuf) at :505
runGodRays:       setSceneDrawBuffers(SingleColor)    // was glDrawBuffers(1, &singleBuf) at :615
runShoreline:     setSceneDrawBuffers(SingleColor)    // was glDrawBuffers(1, &singleBuf) at :648
```

**Hard gate:** `grep -n "glDrawBuffers" GameOS/gameos/gos_postprocess.cpp`
MUST show ALL scene-FBO draw-buffer changes routing through
`setSceneDrawBuffers()` -- no raw `glDrawBuffers(...)` against
`sceneFBO_` outside the helper.

### m1 clear-order rule (load-bearing)

`glClearBufferuiv(GL_COLOR, 2, zero)` clears the draw-buffer at
INDEX 2 of the currently-bound draw-buffer list, NOT the attachment
named `GL_COLOR_ATTACHMENT2` directly. Therefore:

- The env-ON 3-entry `setSceneDrawBuffers(MainSceneMRT)` call MUST
  precede the `glClearBufferuiv(GL_COLOR, 2, zero)` at frame start.
- If the draw-buffer list is currently 2-entry (e.g. env-OFF, or a
  prior single-color pass that has not yet re-asserted the MRT list),
  the `glClearBufferuiv` index 2 clear either fails GL_INVALID_VALUE
  or clears the wrong slot. Both are bugs.
- Order at frame start (env-ON):
  1. `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)`
  2. `setSceneDrawBuffers(MainSceneMRT)` -- list becomes {C0,C1,C2}
  3. Existing color/depth clears
  4. `glClearBufferuiv(GL_COLOR, 2, {0,0,0,0})` -- now safe

Shadow-pass FBOs (`shadow_static_prop.vert` etc.) do NOT bind the
scene FBO -- they have their own depth-only render targets. No
attachment-2 work needed there.

### Resize behavior

`gosPostProcess` already destroys/recreates the scene FBO on window
resize. `sceneObjectIdTex_` follows the same lifecycle: destroyed in
`destroyFBOs()`, recreated in `createFBOs()` at the current `width_ x
height_`. No new code path needed; just one more texture in the
existing pair.

---

## 4. Handle table extension (per-slot inspection record)

### Motivation

A pixel value tells us `(index, generation)`. To answer "what mesh /
material / LOD / pipeline / packet / path produced this pixel?" we need
a side-channel table indexed by `handle.index()`. M1 did not need this;
M1.5 introduces it.

### Proposed structure (placement: RenderWorld internal)

```
// Conceptual: lives inside RenderWorld.cpp anonymous namespace or in
// a new RenderWorld/ObjectRecord.{h,cpp} internal-only header.
struct RenderObjectRecord {
    uint16_t        generation;       // mirrors Handle.generation() for
                                      // stale-handle detection
    uint16_t        flags;            // alive, has-lod-info, etc.
    MeshHandle      mesh;             // resolves to MeshDesc/TG_MultiShape
    MaterialHandle  material;         // resolves to material slot
    uint8_t         lodLevel;         // 0=highest; 0xFF=unknown
    uint8_t         _pad;
    uint16_t        pipelineId;       // M1.5: opaque sentinel (see below)
    uint32_t        drawPacketIndex;  // M1.5: 0xFFFFFFFF sentinel
    uint32_t        pathReasonCode;   // RenderPathDecision.reason index;
                                      // M1.5: 0=unknown
    uint32_t        gameObjectId;     // optional engine-side cookie
};

// Indexed by handle.index(); size = max prop count + headroom.
// std::vector<RenderObjectRecord> s_objectRecords;
```

### Sentinel values for un-materialized chain elements

Several chain elements named in the parent spec do not yet exist in
real code at M1.5 time:

- `PipelineId` is documentary only (no `PipelineDesc` cache exists yet).
  Record stores `pipelineId = 0` ("unknown"); `LookupResult` returns it
  as the literal `0` (PipelineId is documentary in M1.5; this field is
  forward-compatible). Debug print labels it `pipeline=<unknown>`.
- `DrawPacket` is documentary only (M1 emits no packets; the indirect
  command stream is the existing `GpuStaticPropBatcher::flush()` path).
  Record stores `drawPacketIndex = 0xFFFFFFFFu`; `LookupResult` labels
  it `packet=<unknown>`.
- `RenderPathDecision.reason` -- the capability resolver does not exist
  yet. Record stores `pathReasonCode = 0`; `LookupResult` returns it as
  `path=<m1.5-static-prop-indirect>` (the implicit current path).

This is deliberate: the API surface is shape-complete; the answers are
bounded by what M1 actually tracks. As later slices add real
PipelineId / DrawPacket / capability-resolver state, populating these
fields is a per-slice add, not a re-design.

### Storage discipline

- `std::vector<RenderObjectRecord>` indexed by `handle.index()` is the
  lean: M1 production producer audit shows the recipe count tops out
  near 3000 (largest tier1 mission mc2_24 = 2641). `vector` is fine;
  unordered_map adds hash cost on the hot picking path.
- Slot reuse follows the handle's generation-bump discipline: when
  `RenderWorld::destroy(h)` retires a slot, the record's
  `generation` is incremented and `flags &= ~alive`. A later
  `lookupAtPixel` returning a handle from a stale pixel (e.g. pixel
  read after a frame in which the prop was destroyed but the buffer was
  not yet re-cleared) checks `pixel.generation == record.generation`
  before claiming the lookup is valid.
- Populated on `RenderWorld::upsertStaticProp`. Cleared on
  `RenderWorld::destroy(h)`. M1's `adoptStaticPropRecipe` (late-spawn
  path) also populates the record at adoption time.

### Memory cost

`sizeof(RenderObjectRecord)` is ~32 bytes. Worst-case tier1 (2641 props)
-> ~85 KB. Per-mission. Trivial.

---

## 5. Fragment shader contract

### Required emission

The static-prop fragment shader (`shaders/static_prop.frag`) gains:

- An `out uint v_objectId` declaration at `layout(location=2)`. Gated
  by a new GLSL `#ifdef MC2_OBJECT_ID_BUFFER`.
- A uniform supplying the per-draw object handle value:
  `uniform int u_objectIdRaw` (per `memory/uniform_uint_crash.md`,
  declare `int`; reinterpret via `floatBitsToUint(intBitsToFloat(...))`
  or direct `uint(u_objectIdRaw)` cast -- needs C++-side bit pattern
  upload).
- Body: `v_objectId = uint(u_objectIdRaw);` (writes once per fragment;
  alpha-tested fragments that `discard` before the write naturally
  skip emission).

Under the `MC2_COALESCE` path (existing per-draw indirection via
`gl_DrawIDARB`), the object handle comes from a renamed slot in
the `PerDrawEntry` SSBO -- the packet-id namespace is NOT the handle
namespace. M1.5 RENAMES the existing `_pad0` field to `objectIdRaw`
on BOTH the C++ and GLSL sides (struct stays 32 bytes; offset 24
preserved):

```cpp
struct PerDrawEntry {
    int32_t packetID;          //  0
    int32_t materialFlags;     //  4
    int32_t maxLocalVertexID;  //  8
    int32_t texArrayLayer;     // 12
    float   uvScaleX;          // 16
    float   uvScaleY;          // 20
    int32_t objectIdRaw;       // 24  -- was _pad0
    int32_t _pad1;             // 28
};

static_assert(sizeof(PerDrawEntry) == 32, "PerDrawEntry std430 size");
static_assert(offsetof(PerDrawEntry, objectIdRaw) == 24, "objectIdRaw offset");
static_assert(offsetof(PerDrawEntry, _pad1) == 28, "_pad1 offset");
```

The matching GLSL struct in `shaders/static_prop.frag` (and any
coalesce shader file that mirrors the struct) gets the same rename.
Coalesce-path shader read:

```glsl
uint objectId = uint(perDraw_.entries[drawIndex].objectIdRaw);
```

Producer in `gos_static_prop_batcher.cpp` MUST fill
`objectIdRaw = handle.raw()` when object-ID is enabled, `0` otherwise.

### C++ upload contract

For each draw submitted by `GpuStaticPropBatcher::flush()`:

- Non-coalesce path: upload `u_objectIdRaw = (int)handle.raw()` via the
  explicit-program family (`glProgramUniform1i`, per
  `memory/glprogramuniform_vs_gluniform_explicit_program_trap.md` --
  the GOS uniform-upload helper goes through `glGetUniformLocation`
  with a named program, so explicit-program upload is mandatory).
- Coalesce path: write `handle.raw()` into the matching slot of the
  `PerDrawEntry` SSBO before the `glMultiDrawElementsIndirect` call.

The bit pattern of `handle.raw()` is preserved exactly. Pixel readback
returns the same 32-bit value.

### What about non-static-prop passes?

In M1.5, no other fragment shader declares an output at location=2.
GL spec allows missing outputs; the unwritten attachment retains the
clear value (0). This is the LOAD-BEARING reason the clear value MUST
be `Handle::invalid().raw() == 0`: background pixels (terrain, mechs,
VFX, water) read back as "no object" by design, not by accident.

### Conditional compilation discipline

Per `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`:
the GLSL `#ifdef MC2_OBJECT_ID_BUFFER` is gated by a C++-level
`prefix += "#define MC2_OBJECT_ID_BUFFER 1\n"` inside
`#ifdef MC2_OBJECT_ID_BUFFER` at `makeProgram()` call sites. Verify in
the plan by dumping the compiled shader source.

There is a subtler decision: do we make the GLSL `out uint` declaration
**always present** (so the shader unconditionally writes, but writes go
nowhere when no attachment is bound), or guard it behind the macro?
Lean: macro-gate the declaration AND the write. Reason: defensive --
keeping the existing shader byte-identical when env var is OFF makes
the env-OFF tier1 5/5 trivially safe.

This is Q2 in Section 14.

---

## 6. Background-pass behavior

The MRT attachment is bound for the ENTIRE main scene draw. Passes that
don't care about object ID -- terrain, mechs, VFX, water, particles,
immediate-mode HUD elements -- do nothing. The attachment retains
the per-frame clear value (`0` = `Handle::invalid()`).

### Required discipline

- Other Group I/II fragment shaders MUST NOT declare an `out` at
  `layout(location=2)`. Adding one would either crash the link (if the
  binding mismatches the FBO) or silently overwrite the static-prop
  IDs with garbage. M1.5 ships a tier-1 shader-build check that greps
  every shader for `layout(location = 2)` outside `static_prop.frag`
  and fails if found (until M2 explicitly opts mechs in).
- `gl_FragData[2]` writes are undefined for passes without a declared
  output. Per the GL spec the attachment is unmodified.
- **AMD / integer-MRT behavior is not assumed from documentation.**
  M1.5 validates behavior with runtime gates:
  1. attachment-2 clear stays zero where no writer exists
  2. static-prop shader writes expected `handle.raw()`
  3. non-static-prop pixels read back 0
  4. env-OFF has no attachment / no shader output / no readback result

  See Section 12 `OBJECT_ID_SELFTEST` canary -- mandatory on target
  AMD hardware (7900 XTX) before promoting default behavior.

### Pass-list audit (verified shaders/)

Existing fragment shaders that bind to the main scene FBO are
enumerated below; each must be confirmed NOT to declare a location=2
output. Audit predicate runs at slice plan time:

```
grep -rE 'layout\s*\(\s*location\s*=\s*2\s*\)\s*out' shaders/
```

Expected match set at M1.5 ship: `static_prop.frag` only.

### Shadow pass

Shadow rendering targets a separate depth-only FBO. No attachment-2
work. `shaders/shadow_static_prop.vert` (vertex only; no colour writes)
is unaffected.

---

## 7. Debug API surface

### Public lookup function

```cpp
// RenderWorld/Inspect.h  (new public header; engine-only)
namespace RenderWorld {

struct LookupResult {
    bool                isValid;          // false: pixel was background
                                          // or generation mismatch
    RenderCore::RenderObjectHandle handle;
    // Resolved chain (fields populated where the chain element exists
    // in M1.5; sentinel values otherwise -- see Section 4).
    RenderCore::MeshHandle      mesh;
    RenderCore::MaterialHandle  material;
    uint8_t                     lodLevel;          // 0xFF = unknown
    uint16_t                    pipelineId;        // 0 = unknown
    uint32_t                    drawPacketIndex;   // 0xFFFFFFFFu = unknown
    uint32_t                    pathReasonCode;    // 0 = unknown
    uint32_t                    gameObjectId;      // 0 = none
};

LookupResult lookupAtPixel(int screenX, int screenY);

} // namespace RenderWorld
```

### Semantics

1. Caller passes screen-pixel coordinates (origin at bottom-left to
   match GL convention; the API documents this and the caller is
   responsible for flipping Y if the input is window-top-left).
2. Implementation:
   - Bind the scene FBO for read.
   - `glReadBuffer(GL_COLOR_ATTACHMENT2)`.
   - `glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &raw)`.
   - Reconstruct handle: `Handle h; h.bits = raw;`.
   - If `h.bits == 0`: return `LookupResult{isValid=false, ...}`.
   - Index into `s_objectRecords[h.index()]`. If
     `record.generation != h.generation()`: return invalid (stale slot).
   - Populate `LookupResult` from the record.
3. The function is **synchronous** and stalls the GPU until the prior
   frame's writes for that pixel are visible. Acceptable for
   click-time selection (single pixel, <1ms typical on the hardware
   target). Documented as such.

### Failure modes

- Env var off: returns `LookupResult{isValid=false}` with a
  `[RENDER_WORLD v1] WARN: lookupAtPixel called with MC2_OBJECT_ID_BUFFER=0`
  once per session.
- FBO not initialized: same, with a `WARN: scene FBO not ready` once.
- glReadPixels driver failure (e.g. context lost): returns invalid;
  next frame may retry.

### Console / inspection use

The lookup function is the substrate. M1.5 ships ONE additional
diagnostic: a console / hotkey command that calls `lookupAtPixel` at
the current mouse position and emits a `[RENDER_WORLD_INSPECT v1]`
line with the full LookupResult. This is the smallest possible "you
can verify the substrate works" surface; richer HUD overlays defer to
later slices.

---

## 8. Picking integration (hybrid lifecycle) -- DEFERRED to M1.6

**M1.5 scope (2026-05-23 narrowing):** this section is **reference
material for M1.6**. M1.5 ships the FBO substrate, shader output,
handle table, and `lookupAtPixel` debug API only. M1.5 does NOT modify
`code/missiongui.cpp` or any other gameplay-selection code. The design
below describes how M1.6 will wire the substrate into click selection.

### Current state (M1 HEAD, verified by grep)

Picking / object selection in the current code base is NOT a 3D
raycast. The grep result of `mouseInBox` / `screenRayCast` returns no
matches; selection is dispatched through 2D screen-projected bounding
boxes maintained per-mover (`code/missiongui.cpp`'s `pTeam->getMover(i)`
iteration is the dominant pattern, working off projected screen
extents). Static props are NOT independently selectable in the current
game; only movers (mechs, vehicles) participate.

This is a gift: M1.5's "picking replacement" target is narrower than
the user's framing suggested. There is no static-prop CPU raycast to
replace yet -- the GPU buffer ENABLES static-prop selection that
previously did not exist. M2's mech extension is where existing 2D-bound
selection actually flips.

### Spec-mandated framing (preserved)

The parent spec frames this as hybrid because mechs and overlays DO
participate in picking-class logic (cursor tooltips, target acquisition).
M1.5 ships the hybrid plumbing even though the static-prop side has no
prior path to replace:

```
on click(screenX, screenY):
  result = RenderWorld::lookupAtPixel(screenX, screenY);
  if (result.isValid && result.handle resolves to a static prop):
      selection = static-prop result;       // GPU path
  else:
      selection = existing 2D-bounds path;  // CPU path (mechs etc.)
```

In M1.5 only the `result.isValid` static-prop branch fires; mechs
continue to flow through 2D-bounds in `missiongui.cpp`. M2 extends.

### Hybrid deletion criteria

Hybrid is TEMPORARY. CPU-side 2D-bounds picking is deleted when:

1. Every selectable class has an MRT-attached fragment shader writing
   a valid handle (M2 mechs, M3 terrain if selectable, M4 VFX if
   selectable, M5 overlays).
2. Tier1 5/5 + a dedicated picking-canary mission (designed during
   M2) show zero behavioural delta for one full release with the
   GPU path as sole source of truth.
3. The 2D-bounds path is removed in a follow-up slice with a written
   substitutive-not-additive justification.

This deletion lives **beyond** M5 in the timeline. M1.5 documents the
criteria so later slices have a target.

### Sub-pixel / multi-object aliasing

Single-pixel readback is a point sample. The user's click resolves to
exactly one prop (or none). Drag-rect selection (multi-pixel) is
out-of-scope for M1.5 per Section 1. If introduced later, it either:

- Reads a small NxN tile via `glReadPixels(...,N,N,...)` and unions
  the unique handles, or
- Switches to a compute-shader scan of the attachment region. Future
  work.

---

## 9. Env gating

### `MC2_OBJECT_ID_BUFFER` (load-bearing)

```
MC2_OBJECT_ID_BUFFER unset / 0  -> OFF (default)
                              1  -> ON
```

### OFF behavior (default; the production path)

- `sceneObjectIdTex_` is NOT created.
- `glDrawBuffers` list stays at 2 entries (existing M1 HEAD shape).
- Static-prop fragment shader is compiled WITHOUT the
  `#define MC2_OBJECT_ID_BUFFER 1\n` prefix; the `out uint` and the
  write do not exist in the linked program.
- `s_objectRecords` table is ALWAYS populated (mission/upsert-time
  RenderWorld metadata; useful for asserts / inspection prints even
  with the GPU buffer dormant). See "Runtime overhead when OFF" below
  for cost model.
- `RenderWorld::lookupAtPixel` returns `isValid=false` always (with
  the once-per-session WARN per Section 7).
- **Tier1 5/5 invariant:** byte-identical pixels vs M1 HEAD.

### ON behavior (opt-in)

- Attachment created at FBO setup.
- Shader recompiled with the macro defined.
- Per-frame clear of attachment-2 to zero.
- Per-draw `u_objectIdRaw` upload (or SSBO field write in coalesce
  path).
- `lookupAtPixel` returns populated results.
- `[RENDER_WORLD v1]` banner gains an `objectid_buffer=on` token.

### Runtime overhead when OFF

Goal: zero pixel delta, zero GL/FBO/shader delta.

```
MC2_OBJECT_ID_BUFFER=0:
  - no FBO attachment
  - no shader output
  - no GL readback path
  - no per-frame object-ID raster cost
  - s_objectRecords remains populated as RenderWorld metadata

Cost model:
  - one resize/write per static-prop upsert/adoption
  - mission/load-time bounded
  - not paid per frame
  - not claimed as zero CPU cost
```

`s_objectRecords` is always populated, even when
`MC2_OBJECT_ID_BUFFER=0`. It is mission/upsert-time RenderWorld
metadata, not per-frame raster cost. `lookupAtPixel` becomes a no-op
when env-OFF (returns invalid).

**Rationale for always-populated table:** env-gating the table would
spread `if (objectIdEnabled)` checks across every later lookup site
and record producer for very little savings (~85 KB peak at
`mc2_24`). One write per upsert is mission-load bounded and trivial.

Reads of `MC2_OBJECT_ID_BUFFER` cache at startup; the
`s_objectIdEnabled` boolean gates every conditional render code path;
the static-prop draw loop sees ONE extra branch-not-taken per draw.

This matches the `MC2_RENDER_WORLD_TRACE` discipline: env var read
once, banner-shape doesn't change, instrumentation off by default.

---

## 10. Performance budget

### Bandwidth (the dominant cost when ON)

Per-pixel write at the static-prop pass: `R32_UINT` is 4 bytes.
At 1920x1080 over the static-prop coverage region: bounded by the
ratio of static-prop pixels to total pixels. Worst case 100%: 8 MB/
frame extra writes. Modern AMD memory bandwidth (~512 GB/s on the
7900 XTX target hardware): negligible (~16 microseconds at full BW).

### Clear cost

`glClearBufferuiv` on the attachment: one full-screen integer clear.
~25us on the target hardware. Per frame. Acceptable.

### Per-draw uniform upload

Non-coalesce path adds one `glProgramUniform1i` per draw. With ~10-20
static-prop draws per frame (post-batching), well under 100us
combined. Coalesce path adds one `uint32_t` write per PerDrawEntry --
free.

### Readback

`glReadPixels` of a single pixel from an integer attachment incurs a
GPU stall. On click only (not per-frame), so cost is paid at human
click rates (max ~10/sec); negligible.

### Tier1 budget invariant

`MC2_OBJECT_ID_BUFFER=1` must not regress tier1 5/5 frame time by more
than 0.5ms at the 99th percentile. Capture the delta in the M1.5
adversarial-review pass.

### Q1 (Section 14): does the budget hold across the full main scene?

Open. The dominant unknown is whether AMD's integer-format MRT
attachment is treated as a "slow path" in their driver. Flag for
adversarial-review and codex sign-off.

---

## 11. Forbidden behaviors

M1.5 explicitly does NOT do any of:

- Allocate a separate render pass for ID emission (doubles geometry
  submit cost; MRT is the whole point of the design).
- Write IDs from mechs, terrain, VFX, water, or HUD passes. Those
  are M2+ work.
- Use async PBO readback. Synchronous single-pixel only.
- Add a new SSBO for the inspection table. Keep the table CPU-side;
  it's already addressed by `handle.index()` directly.
- Modify the `[STATIC_PROP_REGISTRY v1]` banner shape or counters.
  M1.5 adds `[RENDER_WORLD v1] objectid_buffer=on|off` to the existing
  per-frame banner -- it does not invent new banner schemas.
- Touch the shadow pass. Shadow FBOs are separate; attachment-2 lives
  on the scene FBO only.
- Replace any portion of mech / vehicle / mover selection. Those
  remain on the 2D-bounds path until M2+.
- Promote `PipelineId` / `DrawPacket` / `RenderPathDecision.reason`
  from documentary to enforced. M1.5 returns sentinels for those
  fields; promoting them to real values is the work of later slices,
  not this one.

### Substitutive-not-additive check

Per `memory/feedback_offload_must_be_substitutive_not_additive.md`,
this slice CANNOT leave the 2D-bounds CPU path live for static props
in a region where the GPU path is also live -- two-truth split. The
mitigation: static props were not selectable on the CPU path
previously; M1.5 introduces a NEW capability with a single source of
truth (GPU). Mechs remain CPU-only in M1.5; M2 flips them and the
deletion criteria in Section 8 governs CPU 2D-bounds retirement.

---

## 12. Validation gates

### Tier1 smoke (mandatory)

- `MC2_OBJECT_ID_BUFFER` unset (default): tier1 5/5 PASS, BYTE-IDENTICAL
  pixels vs M1 HEAD. Captured via the standard tier1 invocation in
  `CLAUDE.md` "Smoke gate".
- `MC2_OBJECT_ID_BUFFER=1`: tier1 5/5 PASS, no visual delta beyond
  the documented attachment-2 writes (which are not displayed). Frame
  time delta <= 0.5ms p99.

### Visual canary (mandatory)

A dedicated short mission run with `MC2_OBJECT_ID_BUFFER=1` and the
inspect hotkey enabled. The user moves the cursor over multiple
static-prop categories (buildings, trees) and confirms the emitted
`[RENDER_WORLD_INSPECT v1]` line resolves to the expected mesh /
material handle. This is the substantive functional gate -- the only
end-to-end proof that the substrate works.

### ID-readback unit check (mandatory; in-binary)

A startup-only self-test gated by `MC2_OBJECT_ID_SELFTEST=1`:

1. Register a synthetic static prop.
2. Submit one draw via the static-prop path covering one pixel.
3. `glReadPixels` that pixel; assert it equals `handle.raw()`.
4. Destroy the prop; re-register; assert the new handle's generation
   has incremented; assert lookup with the OLD handle returns
   `isValid=false`.
5. After destroy + before re-register, assert
   `s_objectRecords[oldHandle.index()].alive == false`. (m5
   hardening: ensures the destroy bookkeeping ran on the record
   side, not just the GPU side.)

Self-test prints `[OBJECT_ID_SELFTEST v1] PASS/FAIL`. Tier1 perf
runs do NOT enable it (avoids startup hitch).

### OBJECT_ID_SELFTEST canary (mandatory before promotion; M2 resolution)

In addition to the in-binary ID-readback self-test above, M1.5
requires a runtime canary that exercises the full MRT path against
the target AMD hardware. Mandatory before promoting `MC2_OBJECT_ID_BUFFER=1`
default behavior or shipping any later slice (M2+) that relies on
attachment-2:

```
OBJECT_ID_SELFTEST gate (mandatory before promotion):
  - clear object-ID attachment to 0
  - draw a known static-prop pixel with handle H
  - read that pixel back; expect H.raw()
  - read nearby terrain/background pixel; expect 0
  - run on target AMD hardware (7900 XTX) before promoting default behavior
```

This replaces the previously-undocumented AMD integer-MRT claim with
runtime evidence on the actual driver. Failure modes (terrain pixel
returns nonzero, static-prop pixel reads wrong value) indicate the
write-mask or undefined-write-through behavior is not as the GL spec
states on this driver; investigate before promotion.

### Firewall check (already shipped in M1)

`scripts/check-include-firewall.sh` continues to pass. M1.5 adds no
new game-side dependencies; `Inspect.h` lives in `RenderWorld/`.

### Shader-output uniqueness check (new)

A grep gate in the build script:

```
grep -rE 'layout\s*\(\s*location\s*=\s*2\s*\)\s*out' shaders/
```

Expected: matches `shaders/static_prop.frag` only. Any other match
fails the build until M2 explicitly extends the allow-list.

### Render-contract registry coherence

Per `mclib/render_contract.*` and the matching docs, the static-prop
shader's `[RENDER_CONTRACT]` comment block (currently declares
`requiresMRT=true`) is extended with a `GBuffer2: rc_gbuffer2_objectIdU32`
field when the macro is on. Render-contract registry document mirrors
this. Coordinated in the M1.5 plan, not just the spec.

---

## 13. M2 extension path

The substrate is designed so M2 (mech adapter) plugs in cheaply.
Specifically:

- The MRT attachment stays at `GL_COLOR_ATTACHMENT2`. Mech fragment
  shaders gain a `#ifdef MC2_OBJECT_ID_BUFFER`-gated
  `layout(location=2) out uint v_objectId; v_objectId =
  uint(u_objectIdRaw);` -- the same three lines as the static-prop
  shader.
- The C++ upload site (`mclib/mech3d.cpp` per-mech draw call) adds one
  `glProgramUniform1i` call before each mech draw.
- `RenderWorld::upsertMech` populates a `RenderObjectRecord` in the
  same `s_objectRecords` table; mech-specific fields fold cleanly
  into the existing record schema.
- `lookupAtPixel` requires no API change.
- The picking integration (Section 8) hybrid flip happens at the
  mech-selection call site in `missiongui.cpp`: a GPU lookup attempt
  first, fallback to the existing 2D-bounds code if `isValid=false`
  (e.g. occluded mech behind a building -- GPU returns the building's
  handle, not the mech; CPU 2D-bounds correctly returns the mech).

For M3 (terrain), the same pattern extends to the terrain fragment
shader. Terrain "objects" are chunk IDs; record fields `mesh`,
`material`, `lodLevel` are populated with terrain-shaped values.

For M4 (VFX), particles emit handles; the bandwidth cost compounds but
remains far below the budget envelope.

For M5 (overlays), HUD-adjacent overlays mostly don't need IDs. The
slice opts most overlay fragment shaders OUT (preserving the cleared-
to-zero default), and opts IN only those that are user-selectable.

---

## 14. Open questions

Surface these for the next-pass resolution by the user / adversarial
review / greybeard. Each is load-bearing for the M1.5 EXECUTABLE
promotion.

### Q1. MRT attachment persistence across multi-pass-in-frame

The main-scene FBO is shared across shadow-post, terrain, mech, static
prop, water, VFX, post-process readout passes. The clear value of
attachment-2 is set at scene-FBO clear time, but the engine has
multiple `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)` calls per
frame (post-bloom write-back, godray composite, etc., per
`gos_postprocess.cpp` 400-650). Do any of these passes either (a)
re-clear, (b) re-issue `glDrawBuffers` in a way that omits
attachment-2, or (c) write to attachment-2 with a non-uint shader?
Audit predicate: trace every `glBindFramebuffer(sceneFBO_)` call and
verify post-clear attachment-2 state at the static-prop pass entry.

### Q2. `out uint` declaration when no attachment is bound

In env-OFF mode, the static-prop fragment shader's
`#ifdef MC2_OBJECT_ID_BUFFER` macro is undefined and the `out uint`
declaration is absent. But: should we instead keep the declaration
always present (writes to "nowhere" when no attachment-2 is bound)?
Pros of always-present: no shader recompile gating; tier1 OFF still
exercises the write path so OFF/ON divergence is smaller. Cons: extra
shader hot-reload path; potential AMD driver fallback on integer
writes to unbound MRT slots (refer `docs/amd-driver-rules.md` for
related history). Lean: macro-gate; revisit if hot-reload pain proves
acute.

### Q3. Inspection table data structure -- vector vs unordered_map

The 20-bit index space is 1M slots. Worst-case mission has ~3000 live
props. A direct `std::vector<RenderObjectRecord>` indexed by
`handle.index()` is fine if growth is bounded; an `unordered_map`
introduces hash cost on the hot picking path. Open: do we size the
vector to mission max + headroom (cheap) or to the full 20-bit space
(profligate)? Lean: mission max + headroom at `init()`, grow on
upsert.

### Q4. Generation-check semantics under buffer staleness

The attachment carries last-rendered pixel state. If frame N draws
prop with handle (idx=42, gen=3), then frame N+1 destroys it and
re-creates a different prop at idx=42 with gen=4, then `lookupAtPixel`
between draw-of-N and clear-of-N+1 returns gen=3 -- but the table
already says gen=4. The generation check correctly invalidates this.
Is that the desired behavior, or should the lookup speculatively
return the "last-known" mesh for the stale pixel? Lean: strict
invalidation. Document.

### Q5. Picking-replacement scope -- single-click only?

Per Section 8, drag-rect is deferred. Confirm with user that single-
click is the M1.5 scope and drag-rect is not litigated as
"unfinished" at the M1.5 ship gate.

### Q6. Tier1 perf delta when ON -- is 0.5ms p99 the right budget?

The budget in Section 10 is suggested; needs a real measurement on
the target hardware. The M1.5 plan adds a tier1 perf-comparison step
between env-OFF and env-ON; the budget firms up there.

### Q7. Compute-shader readability

Future cull / inspector / replay tools may want to read the
attachment from a compute shader (e.g. screen-space stats over an
ID region). Do we declare the attachment `glTexStorage2D` upfront so
it can also be bound as a shader-image, or does GL3.3-style
`glTexImage2D` suffice?

**RESOLVED 2026-05-23:** M1.5 `sceneObjectIdTex_` uses `glTexImage2D`,
matching the existing scene-FBO pattern (`sceneNormalTex_` at
`gos_postprocess.cpp:265`). Migrating all scene-FBO textures to
`glTexStorage2D` is a separate modernization slice with its own
justification per `memory/minimal_touch_modern_when_touched.md`.

### Q8. Coalesce-path PerDrawEntry layout impact

The existing `PerDrawEntry` struct in `shaders/static_prop.frag`
already has two `_pad0` / `_pad1` int fields. Can we reuse one of
them for `objectIdRaw`, or do we need to extend the struct (and the
matching C++ producer in `gos_static_prop_batcher.cpp`)?

**RESOLVED 2026-05-23:** rename `_pad0` -> `objectIdRaw` on both C++
and GLSL sides. Update `static_assert(offsetof(_pad0))` to
`offsetof(objectIdRaw)`. Single grep-and-replace across
`gos_static_prop_batcher.h`, `gos_static_prop_batcher.cpp`,
`shaders/static_prop.frag`, and any coalesce shader file that mirrors
the struct. Struct stays 32 bytes; `objectIdRaw` stays at offset 24.
See Section 5 for the full struct layout + static_assert set.

### Q9. Vulkan-prep restatement

`R32_UINT` attachment -> `VK_FORMAT_R32_UINT`.
Fragment-shader `out uint` -> SPIR-V `OpTypeInt 32 0` output.
`glReadPixels` synchronous readback -> `vkCmdCopyImageToBuffer` +
fence-wait. All shapes survive the backend swap; the abstraction layer
in `RenderWorld/Inspect.cpp` is internal and can be ported in lockstep
with the device. Document.

### Q10. RenderPathDecision.reason population timeline

The parent spec's Section 9 capability resolver does not exist yet.
Spec section 4 above documents `pathReasonCode = 0` ("unknown") in
M1.5. When does this field gain real values? Bound: when Section 9's
capability resolver lands (not on the M1.5/M2 path; later in the
roadmap). Confirm the LookupResult API is stable in the meantime --
the field exists, it just resolves to sentinel.

---

## Appendix A. Verified prior art (grep-confirmed 2026-05-23)

- `RenderCore/Handle.h:28-59` -- `Handle<Tag>` with 20-bit index +
  12-bit generation, `raw()` returns the full 32-bit packing.
  `invalid()` is `bits == 0`. M1.5 emits `raw()` directly into the
  attachment.
- `RenderWorld/RenderWorld.h:30-71` -- M1 lifecycle + upsertStaticProp
  + adoptStaticPropRecipe + destroy + markVisible + frameBannerTick.
  M1.5 extends with `lookupAtPixel` in a new `RenderWorld/Inspect.h`
  header.
- `RenderWorld/legacy/static_prop_backend.cpp` -- the only engine TU
  that touches `gos_static_prop_batcher.h`. M1.5's per-draw object-id
  upload either lives here or in `gos_static_prop_batcher.cpp` itself
  (which is GameOS-side, not RenderWorld). Plan-time decision.
- `GameOS/gameos/gos_postprocess.cpp:238-279` -- main scene FBO setup
  with `GL_COLOR_ATTACHMENT0` (sceneColorTex_) and
  `GL_COLOR_ATTACHMENT1` (sceneNormalTex_, GBuffer1).
  M1.5 inserts attachment-2 here.
- `GameOS/gameos/gos_postprocess.cpp:332-349` -- FBO teardown; M1.5
  adds `sceneObjectIdTex_` destruction here.
- `GameOS/gameos/gos_postprocess.cpp:405-419` -- per-frame FBO bind +
  `glDrawBuffers` call. M1.5 extends the buffer list to 3 entries
  when env var is ON.
- `shaders/static_prop.frag` -- the fragment shader getting the
  `out uint` extension. Already declares
  `layout(location = 0) out vec4 FragColor` and
  `layout(location = 1) out vec4 GBuffer1`. M1.5 adds
  `layout(location = 2) out uint v_objectId` under
  `#ifdef MC2_OBJECT_ID_BUFFER`.
- `mclib/render_contract.h` -- contract block in shader comments
  carries `requiresMRT=true`; M1.5 extends with `GBuffer2` field when
  env var is ON.
- `memory/glprogramuniform_vs_gluniform_explicit_program_trap.md` --
  the explicit-program uniform upload rule. M1.5's
  `u_objectIdRaw` upload MUST use `glProgramUniform1i`, not the
  state-bound `glUniform1i`.
- `memory/uniform_uint_crash.md` -- the project's shader builder
  crashes on `uniform uint`. M1.5 declares the upload uniform as
  `uniform int u_objectIdRaw;` and casts to `uint` in the shader body.
- `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md` --
  C++ `-D` flags do not reach GLSL. M1.5 must extend the
  `makeProgram()` prefix string at the C++ call site under
  `#ifdef MC2_OBJECT_ID_BUFFER`.
- `code/missiongui.cpp` -- per-team mover iteration drives current
  selection. No static-prop selection exists at M1 HEAD. Section 8
  hybrid framing is preserved nonetheless.

Line numbers verified 2026-05-23 against worktree HEAD; the
adversarial-plan-review pass will re-grep at execution time.

---

## Appendix B. Glossary

- **Attachment-2** -- `GL_COLOR_ATTACHMENT2`, M1.5's new MRT slot on
  the main scene FBO. Format `GL_R32UI`.
- **GBuffer2** -- contract name mirroring GBuffer1's role; carries the
  packed `Handle.raw()` value.
- **LookupResult** -- struct returned by `RenderWorld::lookupAtPixel`;
  fully populated where chain elements exist, sentinel otherwise.
- **RenderObjectRecord** -- internal RenderWorld struct holding the
  inspection chain data per handle.index().
- **Inspection substrate** -- the minimal end-to-end pipeline (FBO
  attachment + shader emit + RenderWorld table + lookup API + picking
  hook) that makes every static-prop pixel introspectable. Required
  by the boundary spec Section 11; promoted to mandatory by M1.5.
- **Hybrid picking lifecycle** -- the transition state where some
  classes resolve via GPU pixel lookup and others via legacy CPU
  paths; ends when M5 closes and the substitutive deletion slice
  retires CPU 2D-bounds.

---

## Appendix C. Cross-spec references

- `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`
  -- parent spec; Section 11 mandates this substrate.
- `docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md`
  -- item 10 (`MC2_OBJECT_ID_BUFFER`); this spec is its realization.
- `docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`
  -- M1 plan (executed; this slice consumes its artifacts).
- `docs/superpowers/reviews/2026-05-22-renderworld-boundary-spec-adversarial-review.md`
  -- adjudicated MAJORs from the parent spec; this draft inherits the
  resolutions.
- `docs/superpowers/reviews/2026-05-22-renderworld-slice-m1-plan-adversarial-review.md`
  -- M1 adversarial review; documents legacy-seam treatment and the
  D4 sign-off that shaped Section 4.
- `docs/render-contract.md` + `mclib/render_contract.*` -- shader
  contract registry; M1.5 amends the static-prop block.
- `docs/amd-driver-rules.md` -- AMD driver behaviour log; flag
  integer-MRT writes for the adversarial-review pass.
- `memory/glprogramuniform_vs_gluniform_explicit_program_trap.md`
- `memory/uniform_uint_crash.md`
- `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`
- `memory/feedback_offload_must_be_substitutive_not_additive.md`
- `memory/vulkan_prep_explicit_device_discipline.md`

---

## Appendix D. Resolved decisions (2026-05-23)

Adversarial review of this spec produced 1 CRIT + 3 MAJOR + 6 MINOR.
All CRIT and MAJORs are resolved as follows; user sign-off recorded
verbatim.

### C1: setSceneDrawBuffers() helper

Use a `gos_postprocess.cpp`-local `setSceneDrawBuffers()` helper.
All `sceneFBO_` `glDrawBuffers` calls route through it.
Main scene bind emits {C0, C1, C2} when `MC2_OBJECT_ID_BUFFER=1`.
Single-color postprocess passes remain {C0}.
`glClearBufferuiv(GL_COLOR, 2, zero)` is called only after the env-ON
3-entry list is active.

### M1: s_objectRecords always populated

`s_objectRecords` is always populated, even when
`MC2_OBJECT_ID_BUFFER=0`. This is mission/upsert-time RenderWorld
metadata, not per-frame raster cost. Remove "zero CPU cost"; claim
only zero pixel delta and zero GL/FBO/shader delta when env-OFF.

### M2: AMD claim removed; runtime canary added

Remove uncited AMD integer-MRT claim.
Add `OBJECT_ID_SELFTEST` / canary proving clear-to-zero, static-prop
write, and non-writer pixel remains zero on target hardware
(7900 XTX).

### M3: PerDrawEntry _pad0 -> objectIdRaw rename

Rename `PerDrawEntry._pad0 -> objectIdRaw` on C++ and GLSL sides.
Update `static_assert` offset checks and all producer writes.
Struct remains 32 bytes; `objectIdRaw` stays at offset 24.

### m4: glTexImage2D (not glTexStorage2D)

M1.5 `sceneObjectIdTex_` uses `glTexImage2D` for M1.5; defer
`glTexStorage2D` migration to a whole-FBO modernization slice.

### Final gate

Do not promote to EXECUTING until C1 is in the spec as concrete
helper discipline. The rest are normal plan-shaping decisions.

---

End of spec.
