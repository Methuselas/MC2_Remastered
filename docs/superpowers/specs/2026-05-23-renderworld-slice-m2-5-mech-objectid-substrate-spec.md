# RenderWorld Slice M2.5 — Mech ObjectID Substrate Spec

- SPEC STATUS: REVISED — adversarial CONDITIONAL-PASS findings applied + Q1-Q6 resolved
- Status: REVISED
- Date: 2026-05-23
- Author: spec-author session, post-M2 ship
- Predecessor slices (all SHIPPED 2026-05-23):
  - **M1.5** -- ObjectID buffer substrate (R32_UINT MRT attachment-2,
    setSceneDrawBuffers helper, static-prop fragment writes,
    `RenderWorld::lookupAtPixel`, `s_objectRecords` table).
    Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`
  - **M2** -- route-only MechRenderAdapter; every live `Mech3DAppearance`
    has a `RenderObjectHandle` in `mechRenderHandle`; unified table
    populated at `kMechHandleBase=0x00010000` with `kind=Mech`. No GPU
    shader writes yet.
    Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md`
  - **M2-pre** -- gameplay-pick META-FIX extraction
    (`tryGameplayPick(req)` + `screenToFboPixel(...)`); `RunGameplayPickSelfTest`
    validator wired into `RenderWorld::init()`.
- Recon (mandatory read; trust file:line, grep-verify load-bearing ones):
  `docs/superpowers/explorations/2026-05-23-renderworld-slice-m2-5-recon.md`
- Successor slice (informational):
  M2.6 -- mech pickup integration via `tryGameplayPick`.
- Relation to parent: realizes the Mech extension of the boundary spec's
  Section 11 ObjectID substrate. M1.5 shipped the substrate for static
  props; M2.5 closes the loop for mechs (the dominant gameplay-selectable
  class) by emitting `Handle.raw()` from `mech.frag` to
  `GL_COLOR_ATTACHMENT2`.

---

## 1. Purpose / closing the loop

### One-line framing

> **M2 stored the handle; M2.5 emits it to the GPU.**

M2 introduced `Mech3DAppearance::mechRenderHandle` and three public
`ForAdapter` accessors (`getRenderWorldHandle`, `setRenderWorldHandleForAdapter`,
`clearRenderWorldHandleForAdapter` at `mclib/mech3d.h:487-495`), routed
through `GameAdapters::Mech::registerMech` / `destroyMech` -> the unified
`s_objectRecords` table (`kind=Mech`). The handle was added but **never
written into the R32_UINT attachment**: `mech.frag` (`shaders/mech.frag`)
declares only `layout(location=0/1)` outputs; `RenderWorld::lookupAtPixel`
on a mech pixel returns `Handle::invalid()` (the per-frame clear value
that the static-prop path is symmetric with).

M2.5 ships the one symmetric write that closes the chain. After M2.5:

- `RenderWorld::lookupAtPixel(x, y)` on a mech-on-cursor pixel returns
  `LookupResult{isValid=true, handle=h, ...}` with `record.kind == Mech`.
- The handle round-trips: writing `handle.raw()` to attachment-2 and
  reading it back via `glReadPixels(GL_RED_INTEGER, GL_UNSIGNED_INT)`
  produces the same 32-bit value.
- M2.6 (next slice) flips the mech selection trigger from "static-prop
  pick only" to "any kind matching expected gesture-gate"; the spine and
  coord transform from M2-pre are reused unchanged.

### Predecessor work that M2.5 builds on (verified)

| Substrate piece | Location | Verified |
|---|---|---|
| FBO attachment-2 (`sceneObjectIdTex_`, R32_UINT) | `GameOS/gameos/gos_postprocess.cpp:324-333` | grep |
| MRT draw-buffer policy helper | `GameOS/gameos/gos_postprocess.cpp:31` (`setSceneDrawBuffers(mode, bool ready)`) | grep |
| Per-frame clear of attachment-2 | `setSceneDrawBuffers(MainSceneMRT, true)` then `glClearBufferuiv` at beginScene (`gos_postprocess.cpp:488-499`) | grep |
| `RenderWorld::IsObjectIdBufferEnabled()` env gate | `RenderWorld/RenderWorld.h:85` | grep |
| Static-prop GLSL prefix injection template | `GameOS/gameos/gos_static_prop_batcher.cpp:510-521` | grep |
| Static-prop fragment shader output @ loc 2 | `shaders/static_prop.frag:71` + `:178-181` | grep |
| `s_objectRecords` unified table | `RenderWorld/RenderWorld.h:132-150` | grep |
| Mech handle field | `mclib/mech3d.h:478` (private) | grep |
| Mech handle accessor (`getRenderWorldHandle`) | `mclib/mech3d.h:487` | grep |
| `lookupAtPixel` API | `RenderWorld/RenderWorld.h:176` | grep |
| `RunGameplayPickSelfTest` validator hook | `RenderWorld/RenderWorld.cpp:40,365` | grep |

### Non-goals (explicit)

- **Not the M2.6 trigger flip.** M2.5 ships substrate only. Shift+click
  on a mech still hits the static-prop pick spine first (mover-first
  gate already short-circuits in `tryGameplayPick`); the substrate
  produces a Mech handle the spine ignores in M2.5. M2.6 adds the
  `kind == Mech` branch.
- **Not MLR/`ShapeRenderer`/`gos_tex_vertex_lighted` extension.** The
  legacy CPU fallback path produces no ObjectID write; mechs that take
  the fallback that frame return `Handle::invalid()` from
  `lookupAtPixel`. Section 6 documents this asymmetry; it mirrors the
  M1.5 legacy-vs-coalesce asymmetry pattern.
- **Not a per-draw uniform.** `GpuMechBatcher::flush()` buckets many
  actors into one `glDrawElementsInstancedBaseVertex` call (one draw per
  packet+texture). A single per-draw uniform CANNOT carry per-instance
  data; the per-instance SSBO is the only correct vehicle. Section 3
  walks the rejected alternatives.
- **Not a new env var.** `MC2_OBJECT_ID_BUFFER` reused unchanged.
- **Not a shadow-pass write.** Shadow rendering targets a separate
  depth-only FBO (`shadow_static_prop.vert` family); no ObjectID work.
- **Not a `setSceneDrawBuffers` extension.** The mech batcher inherits
  the main-scene FBO bound by `gosPostProcess::beginScene()`; it does
  NOT call `glBindFramebuffer(sceneFBO_)` or `glDrawBuffers` (Section 4
  confirms via grep negative). The M1.5 helper covers the binding
  policy unchanged; no new call site routes through it.
- **Not an additional FBO attachment.** M2.5 reuses
  `GL_COLOR_ATTACHMENT2` as M1.5 set up. No `GL_COLOR_ATTACHMENT3` etc.

### Open questions / resolved decisions

All Q1-Q6 from the draft are RESOLVED -- see Section 12.

---

## 2. Relationship to M1.5 / M2 / M2-pre / M2.6

### From M1.5

M2.5 inherits the entire substrate stack: FBO attachment, draw-buffer
helper, env gate, per-frame clear, `s_objectRecords` table, `lookupAtPixel`
API, and the `[OBJECT_ID_SELFTEST v1]` passive validator that fires
once per startup on a known static-prop pixel.

M2.5 changes ZERO of these. No FBO change. No draw-buffer change. No
env-var add. No new `lookupAtPixel` overload. The single substrate
extension is "another fragment shader becomes a writer to the same
attachment slot," using the same GLSL macro gate.

### From M2

M2 added storage; M2.5 adds the emit. The chain after M2.5 (verified
file:line, all `_pad` fields rejected -- the recon's "lockstep warning"
applies):

```
Mech3DAppearance::mechRenderHandle                  [mech3d.h:478]
  via getRenderWorldHandle().raw()                  [mech3d.h:487]
  -> GpuMechSubmitDesc::objectIdRaw [NEW]           [gos_mech_batcher.h:88-106]
  -> PendingSubmit.desc.objectIdRaw                 [gos_mech_batcher.cpp:1095]
  -> GpuMechInstance::objectIdRaw [NEW]             [gos_mech_batcher.h:35-51]
  -> SSBO binding=0, read in mech.vert              [shaders/mech.vert:30-40]
  -> forwarded as `flat out uint v_objectIdRaw`     [shaders/mech.vert NEW]
  -> mech.frag: layout(location=2) out uint         [shaders/mech.frag NEW]
     under #ifdef MC2_OBJECT_ID_BUFFER
```

### From M2-pre

M2-pre extracted `tryGameplayPick(req)` and `screenToFboPixel(...)` as
the shared gameplay-pick spine. M2.5 does NOT touch this spine -- it
only causes the spine's `lookupAtPixel` call to start returning Mech
handles in addition to StaticProp handles. The spine's switch on
`Outcome::hit` is the M2.6 extension surface; M2.5 leaves it alone.

### Toward M2.6

After M2.5 ships, M2.6 becomes a small, additive slice:

1. Add a `MC2_MECH_PICK=1` env gate (mirrors `MC2_STATIC_PROP_PICK`).
2. In `tryGameplayPick`, on hit, branch on `record.kind`:
   - `StaticProp` -> existing M1.6 path (`[STATIC_PROP_PICK v1]` log,
     `setLastStaticPropPick`).
   - `Mech` -> new `[MECH_PICK v1]` log path, mech-side debug state.
3. User-driven canary: Shift+click on a mech now returns a Mech handle
   that round-trips to `Mech3DAppearance` (via the adapter's reverse
   lookup, if added) or simply logs the handle for inspection.

Substrate alone (M2.5) gives M2.6 a working pipeline. M2.5's success
gate is "the substrate is inspectable and round-trips," not "the user
can pick a mech."

### Roadmap item 10 status

After M2.5 ships, mech-ID coverage joins static-prop coverage under the
`MC2_OBJECT_ID_BUFFER` umbrella. Pass-list against the M1.5 forbidden-
layout-2 grep:

```
grep -rE 'layout\s*\(\s*location\s*=\s*2\s*\)\s*out' shaders/
```

Expected match set after M2.5: `static_prop.frag`, `mech.frag` only. M3
(terrain), M4 (VFX), M5 (overlay) extend further; not in scope.

---

## 3. Architecture overview -- single META-FIX surface

### Why the SSBO (and NOT a per-draw uniform)

The mech batcher's central performance contract is that
`GpuMechBatcher::flush()` (`gos_mech_batcher.cpp:1289`) issues ONE
`glDrawElementsInstancedBaseVertex` call per (packet, texture, material)
bucket, carrying many actors per draw via the per-instance SSBO at
binding 0 (`gos_mech_batcher.cpp:1109-1111`). The instance index in the
vertex shader is `uint(u_instanceBase) + uint(gl_InstanceID)` (verified
`mech.vert:79`).

Any per-actor data MUST ride the SSBO. A per-draw uniform such as
`glProgramUniform1ui(s_mechProgram, loc, handle)` would apply to ALL
instances in the bucket -- every mech in the same draw call would get
the SAME ObjectID. That is wrong by definition: distinct mechs MUST
produce distinct pixel values for `lookupAtPixel` to identify them.

The static-prop legacy path uses a per-draw uniform (`u_objectIdRaw` at
`shaders/static_prop.frag:59`, upload at `gos_static_prop_batcher.cpp`)
precisely because it draws one instance at a time. The coalesce path
uses a `PerDrawEntry` SSBO field (`shaders/static_prop.frag:44` ->
`:179`) for bucketed draws. The mech batcher's shape MATCHES the
coalesce shape: per-instance SSBO field is the only correct vehicle.

### Why no `setSceneDrawBuffers` extension

Negative grep confirmed at write-time (recon §"setSceneDrawBuffers
extension surface"):

```
grep -n glDrawBuffers GameOS/gameos/gos_mech_batcher.cpp     -> no matches
grep -n glBindFramebuffer GameOS/gameos/gos_mech_batcher.cpp -> (no scene-FBO bind)
```

The mech batcher inherits the FBO + draw-buffer list set up by
`gosPostProcess::beginScene()` at `gos_postprocess.cpp:488-499`. That
call already routes through `setSceneDrawBuffers(MainSceneMRT,
sceneObjectIdTex_ != 0)` -- attachment-2 is in the write mask for the
entire scene pass, including the mech-batcher draw. M2.5 needs zero new
helper sites.

The M1.5 helper's bug-class retirement claim ("scattered glDrawBuffers
policy drift") survives M2.5 unchanged.

### Why a clean META-FIX and not a PATCH

The per-instance SSBO field add is **strictly substitutive** against
the M1.5 coalesce-path precedent: same shape (per-instance ObjectID
field), same shader-output declaration, same env-gate macro, same
fragment-shader emit line. M2.5 retires the "mechs invisible to picking"
bug class with one symmetric edit; the next bug class (any subsequent
batched-instanced renderer that wants ObjectID coverage) can be
addressed by the same recipe.

Per the greybeard discipline (`.claude/skills/greybeard.md`):
META-FIX retires the **bug class** by adding the missing per-instance
ObjectID emission path. PATCH would add a separate mech-ID FBO or a
per-actor uniform (rejected below).

Full greybeard analysis: Section 9.

### Architectural diagram

```
                    +------------------------------+
                    | Mech3DAppearance             |
                    |  - mechRenderHandle (M2)     |
                    +------------------------------+
                                  |
                                  v
                    +------------------------------+
                    | mech3d.cpp render path       |
                    |  GpuMechSubmitDesc desc{};   |
                    |  desc.objectIdRaw =          |   <-- M2.5 add
                    |    getRenderWorldHandle()    |
                    |    .raw();                   |
                    |  submitActor(desc);          |
                    +------------------------------+
                                  |
                                  v
                    +------------------------------+
                    | GpuMechBatcher::flush()      |
                    |  per-instance fill:          |
                    |  inst.objectIdRaw =          |   <-- M2.5 add
                    |    ps.desc.objectIdRaw;      |
                    |  glDrawElementsInstanced...  |
                    +------------------------------+
                                  |
                                  v        SSBO binding=0
                    +------------------------------+
                    | mech.vert                    |
                    |  GpuMechInstance struct      |   <-- M2.5 add field
                    |  flat out uint v_objectIdRaw |   <-- M2.5 add
                    |    = inst.objectIdRaw;       |
                    +------------------------------+
                                  |
                                  v
                    +------------------------------+
                    | mech.frag                    |
                    |  #ifdef MC2_OBJECT_ID_BUFFER |   <-- M2.5 add
                    |  layout(location=2)          |
                    |    out uint v_objectId;      |
                    |  v_objectId = v_objectIdRaw; |
                    +------------------------------+
                                  |
                                  v
                    +------------------------------+
                    | GL_COLOR_ATTACHMENT2         |
                    |  (R32_UINT, M1.5 substrate)  |
                    +------------------------------+
                                  |
                                  v
                    +------------------------------+
                    | RenderWorld::lookupAtPixel   |
                    |  -> LookupResult             |
                    |  record.kind == Mech         |
                    +------------------------------+
```

---

## 4. File changes

Every change below is gated by `#ifdef MC2_OBJECT_ID_BUFFER` on the
GLSL side (via the C++-injected prefix per `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`).
C++ struct fields are unconditional (additive; default-zero leaves
env-OFF byte-identical -- see Section 7 cost analysis).

### 4.1 `GameOS/gameos/gos_mech_batcher.h` -- two struct field adds

#### 4.1.1 `GpuMechSubmitDesc` (CPU carrier; lines 88-106)

**Existing:**

```cpp
// All per-actor context needed at submit / flush time.
struct GpuMechSubmitDesc {
    TG_MultiShape*              mechShape;
    const Mech3DAppearanceType* mechType;
    int                         currentLOD;
    uint32_t                    slot0TexHandle;
    uint32_t                    lightDataIndex;
    uint32_t                    renderFlags;
    uint32_t                    highlightARGB;
    uint32_t                    fogARGB;
};
```

**Replace with:**

```cpp
// All per-actor context needed at submit / flush time.
struct GpuMechSubmitDesc {
    TG_MultiShape*              mechShape;
    const Mech3DAppearanceType* mechType;
    int                         currentLOD;
    uint32_t                    slot0TexHandle;
    uint32_t                    lightDataIndex;
    uint32_t                    renderFlags;
    uint32_t                    highlightARGB;
    uint32_t                    fogARGB;
    // M2.5: RenderObjectHandle.raw() for this actor's mech handle
    // (M2 storage). 0 = Handle::invalid() = no ObjectID write at this
    // pixel (treated identically to legacy-path fallback). The CPU-side
    // carrier is unconditional; the consumer is gated by
    // MC2_OBJECT_ID_BUFFER at GLSL prefix time.
    // Source: mech3d.cpp submit site reads
    //   appearance.getRenderWorldHandle().raw()  [mech3d.h:487].
    uint32_t                    objectIdRaw;
};
```

#### 4.1.2 `GpuMechInstance` (std430 SSBO; lines 35-51)

**Existing (48-byte std430 record; `static_assert(sizeof == 48)`):**

```cpp
struct alignas(16) GpuMechInstance {
    uint32_t typeLodRecordIndex;  //  0
    uint32_t baseBoneOffset;      //  4
    uint32_t lightDataIndex;      //  8
    uint32_t renderFlags;         // 12
    float    aRGBHighlight[4];    // 16
    float    fogRGB[4];           // 32
};                                // sizeof = 48
static_assert(sizeof(GpuMechInstance) == 48, ...);
static_assert(offsetof(GpuMechInstance, typeLodRecordIndex) ==  0);
static_assert(offsetof(GpuMechInstance, baseBoneOffset)     ==  4);
static_assert(offsetof(GpuMechInstance, lightDataIndex)     ==  8);
static_assert(offsetof(GpuMechInstance, renderFlags)        == 12);
static_assert(offsetof(GpuMechInstance, aRGBHighlight)      == 16);
static_assert(offsetof(GpuMechInstance, fogRGB)             == 32);
```

**Replace with (64-byte std430 record; lockstep with `mech.vert`):**

```cpp
// Per-instance GPU record -- std430, 64 bytes (M2.5: was 48 bytes).
// CHANGING THIS STRUCT REQUIRES CHANGING mech.vert IN LOCKSTEP.
struct alignas(16) GpuMechInstance {
    uint32_t typeLodRecordIndex;  //  0
    uint32_t baseBoneOffset;      //  4
    uint32_t lightDataIndex;      //  8
    uint32_t renderFlags;         // 12
    float    aRGBHighlight[4];    // 16
    float    fogRGB[4];           // 32
    // M2.5: RenderObjectHandle.raw() emitted by mech.frag as
    //   layout(location=2) out uint v_objectId
    // under #ifdef MC2_OBJECT_ID_BUFFER. 0 = Handle::invalid()
    // (clear-value match -- background read by lookupAtPixel).
    uint32_t objectIdRaw;         // 48
    uint32_t _pad1;               // 52 -- reserved; std430 vec4 alignment
    uint32_t _pad2;               // 56 -- reserved
    uint32_t _pad3;               // 60 -- reserved
};                                // sizeof = 64
// Layout: 16 (4*uint32) + 16 (vec4) + 16 (vec4) + 16 (uint32 + 3*pad) = 64
static_assert(sizeof(GpuMechInstance) == 64,
              "GpuMechInstance size must match std430 GLSL struct");
static_assert(offsetof(GpuMechInstance, typeLodRecordIndex) ==  0);
static_assert(offsetof(GpuMechInstance, baseBoneOffset)     ==  4);
static_assert(offsetof(GpuMechInstance, lightDataIndex)     ==  8);
static_assert(offsetof(GpuMechInstance, renderFlags)        == 12);
static_assert(offsetof(GpuMechInstance, aRGBHighlight)      == 16);
static_assert(offsetof(GpuMechInstance, fogRGB)             == 32);
static_assert(offsetof(GpuMechInstance, objectIdRaw)        == 48);
```

**Padding rationale.** std430 alignment for a `vec4`-preceded uint is
naturally 4 bytes -- a single `uint32_t objectIdRaw` at offset 48
satisfies alignment with NO trailing padding required (the next struct
in an SSBO array starts at the alignment of the struct's strictest
member, which is `vec4` = 16 bytes; std430 trailing-pad to 16 gives a
56 -> 64 bump but no GLSL-side fields need to exist for the pad bytes).

We choose to be **explicit**: three trailing `_padN` fields named and
asserted at C++ level so any future field add (M3+: e.g. terrain chunk
ID, VFX particle ID) takes a named slot rather than silently consuming
pad. The GLSL struct mirrors with one `uint objectIdRaw` and three
`uint _padN` fields (Section 4.3); the static_assert chain prevents
accidental layout drift. Per Q2 resolved, names are GENERIC
(`_pad1/_pad2/_pad3`) -- only `objectIdRaw` is named; future slices
that consume a pad slot rename it in place at that time.

Alternative considered: omit the explicit pad fields on the C++ side
and let the compiler + std430 trail-pad implicitly. **Rejected:**
would diverge from the explicit-padding pattern that `GpuMechVertex`
follows (each field offset asserted at lines 25-31), and would leave
a layout-drift trap when M3 adds a new field.

### 4.2 `GameOS/gameos/gos_mech_batcher.cpp` -- two edits

#### 4.2.1 Per-instance SSBO fill (lines 1093-1104)

**Existing:**

```cpp
for (uint32_t si : subs) {
    const PendingSubmit& ps   = s_pendingSubmits[si];
    const GpuMechSubmitDesc& d = ps.desc;
    GpuMechInstance inst{};
    inst.typeLodRecordIndex = ps.typeLodIdx;
    inst.baseBoneOffset     = actorBoneBase[si];
    inst.lightDataIndex     = d.lightDataIndex;
    inst.renderFlags        = d.renderFlags;
    unpack(d.highlightARGB, inst.aRGBHighlight);
    unpack(d.fogARGB,       inst.fogRGB);
    instDst[instHead++]     = inst;
}
```

**Replace with:**

```cpp
for (uint32_t si : subs) {
    const PendingSubmit& ps   = s_pendingSubmits[si];
    const GpuMechSubmitDesc& d = ps.desc;
    GpuMechInstance inst{};
    inst.typeLodRecordIndex = ps.typeLodIdx;
    inst.baseBoneOffset     = actorBoneBase[si];
    inst.lightDataIndex     = d.lightDataIndex;
    inst.renderFlags        = d.renderFlags;
    unpack(d.highlightARGB, inst.aRGBHighlight);
    unpack(d.fogARGB,       inst.fogRGB);
    // M2.5: emit handle bits to attachment-2 via mech.frag.
    // 0 = Handle::invalid() when env-OFF (carrier remains zero; FS
    // path is macro-gated out anyway) or when the actor has no live
    // handle (legacy fallback / pre-register frame).
    inst.objectIdRaw        = d.objectIdRaw;
    instDst[instHead++]     = inst;
}
```

#### 4.2.2 GLSL prefix injection at program load (around `loadProgramsIfNeeded`, lines 218-233)

**Existing:**

```cpp
static void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    s_programLoadTried = true;

    s_mechProgramObj = glsl_program::makeProgram(
        "mech", "shaders/mech.vert", "shaders/mech.frag", "#version 430\n");

    if (!s_mechProgramObj || !s_mechProgramObj->is_valid()) {
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=shader_fail -- GPU mech path disabled\n");
        s_mechProgramObj    = nullptr;
        s_mechProgram       = 0;
        s_programLoadFailed = true;
        return;
    }
    s_mechProgram = s_mechProgramObj->shp_;
    // ...
}
```

**Replace with (mirrors `gos_static_prop_batcher.cpp:510-521`):**

```cpp
static void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    s_programLoadTried = true;

    // M2.5: GLSL preprocessor does not inherit C++ build flags
    // (memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
    // Build the prefix as a std::string and append the MC2_OBJECT_ID_BUFFER
    // macro definition when the env gate is on, mirroring
    // gos_static_prop_batcher.cpp:510-521.
    std::string mechPrefix = "#version 430\n";
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        mechPrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
    }

    s_mechProgramObj = glsl_program::makeProgram(
        "mech", "shaders/mech.vert", "shaders/mech.frag", mechPrefix.c_str());

    if (!s_mechProgramObj || !s_mechProgramObj->is_valid()) {
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=shader_fail -- GPU mech path disabled\n");
        s_mechProgramObj    = nullptr;
        s_mechProgram       = 0;
        s_programLoadFailed = true;
        return;
    }
    s_mechProgram = s_mechProgramObj->shp_;
    // ...
}
```

**Add include (top of file):**

```cpp
#include "../../RenderWorld/RenderWorld.h"  // M2.5: IsObjectIdBufferEnabled
```

**Discrepancy with recon:** the recon report's firewall section
("`gos_mech_batcher.cpp` already includes `RenderWorld.h` for
`IsObjectIdBufferEnabled()` (verify via grep)") was wrong at write-time.
Grep confirms NO existing `#include` of `RenderWorld.h` /
`RenderWorld/` from `GameOS/gameos/gos_mech_batcher.cpp`. The
sibling file `gos_static_prop_batcher.cpp` does include it (line 3).
M2.5 adds the include to `gos_mech_batcher.cpp` as part of the slice.

**No firewall-script edit required.** `scripts/check-include-firewall.sh:22`
defines `SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer
MaterialSystem DebugRenderer RenderDeviceGL"`. `GameOS/` is NOT in
SCOPE_DIRS, so includes originating from `GameOS/gameos/*.cpp` are
NOT policed by the script. The sibling include at
`gos_static_prop_batcher.cpp:3` (shipped under M1.5) is also
unpoliced — it works by reviewer discipline, not by allowlist.

Reviewer must visually verify that the include direction is
engine -> engine: `RenderWorld/RenderWorld.h` is a pure-types public
header (it exposes `IsObjectIdBufferEnabled()`, `Handle`,
`LookupResult`, kind enum). Confirming visually that the include does
not pull game-side (`code/`, `mclib/`) dependencies back into GameOS
is the only gate. No allowlist line to add; no script change.

### 4.3 `shaders/mech.vert` -- struct add + forward varying

#### 4.3.1 `GpuMechInstance` GLSL struct (lines 30-37)

**Existing:**

```glsl
struct GpuMechInstance {
    uint  typeLodRecordIndex;
    uint  baseBoneOffset;
    uint  lightDataIndex;
    uint  renderFlags;
    vec4  aRGBHighlight;
    vec4  fogRGB;
};
layout(std430, binding=0) readonly buffer InstanceBuffer {
    GpuMechInstance instances[];
};
```

**Replace with:**

```glsl
struct GpuMechInstance {
    uint  typeLodRecordIndex;
    uint  baseBoneOffset;
    uint  lightDataIndex;
    uint  renderFlags;
    vec4  aRGBHighlight;
    vec4  fogRGB;
    // M2.5: RenderObjectHandle.raw() emitted to attachment-2 via
    // mech.frag. Mirrors C++ GpuMechInstance at gos_mech_batcher.h:48.
    // Three trailing uint pad fields keep std430 layout explicit so
    // M3+ adds (terrain chunk, VFX) take named slots.
    uint  objectIdRaw;
    uint  _pad1;
    uint  _pad2;
    uint  _pad3;
};
layout(std430, binding=0) readonly buffer InstanceBuffer {
    GpuMechInstance instances[];
};
```

#### 4.3.2 Forward varying (after the existing `out` declarations, lines 67-76)

**Add (gated):**

```glsl
// M2.5: forward per-instance ObjectID to FS for the
// layout(location=2) out uint emission under
// #ifdef MC2_OBJECT_ID_BUFFER. `flat` qualifier mandatory because
// integers cannot be linearly interpolated (GL spec).
#ifdef MC2_OBJECT_ID_BUFFER
flat out uint v_objectIdRaw;
#endif
```

#### 4.3.3 Body write (inside `main()`, after the existing `inst` fetch at line 80)

**Add (gated; placement: at the end of main, after the existing
varying assignments at lines 165-173):**

```glsl
#ifdef MC2_OBJECT_ID_BUFFER
    // M2.5: emit per-instance ObjectID through the existing
    // per-instance SSBO read; no extra memory traffic.
    v_objectIdRaw = inst.objectIdRaw;
#endif
```

**Note on macro discipline.** Per Section 5 below and
`memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`, the
`#ifdef MC2_OBJECT_ID_BUFFER` macro is defined only by the C++-injected
prefix in 4.2.2. Without the env var, the `flat out uint` declaration
AND the body write are absent from the linked program. Env-OFF
behavior is byte-identical (Section 7).

### 4.4 `shaders/mech.frag` -- one `out` decl + one body line

#### 4.4.1 Add the gated `flat in` + `out` declarations (after existing `layout(location=0/1)` at lines 36-37)

**Existing:**

```glsl
layout(location=0) out vec4 FragColor;
layout(location=1) out vec4 GBuffer1;
```

**Replace with:**

```glsl
layout(location=0) out vec4 FragColor;
layout(location=1) out vec4 GBuffer1;
#ifdef MC2_OBJECT_ID_BUFFER
// M2.5: per-pixel mech ObjectID. Emitted to GL_COLOR_ATTACHMENT2
// (R32UI; M1.5 substrate). `flat in` matches mech.vert's
// `flat out uint v_objectIdRaw`.
flat in uint v_objectIdRaw;
layout(location=2) out uint v_objectId;
#endif
```

#### 4.4.2 Body write (at the end of `main()`, after `GBuffer1 = ...` at line 76)

**Existing (lines 75-77):**

```glsl
    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
}
```

**Replace with:**

```glsl
    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
#ifdef MC2_OBJECT_ID_BUFFER
    // M2.5: emit per-pixel RenderObjectHandle.raw(). Alpha-tested
    // fragments that discard() above (line 56) skip this write
    // naturally -- the attachment-2 pixel retains the clear value
    // (0 = Handle::invalid()), correctly classifying that fragment as
    // background under lookupAtPixel.
    v_objectId = v_objectIdRaw;
#endif
}
```

### 4.5 `mclib/mech3d.cpp` -- one assignment at the submit site (line ~2585)

**Existing (lines 2549-2586):**

```cpp
GpuMechSubmitDesc desc{};
desc.mechShape      = mechShape;
desc.mechType       = mechType;
desc.currentLOD     = (int)currentLOD;
desc.slot0TexHandle = (uint32_t)localTextureHandle;
// ... lightDataIndex, renderFlags, highlightARGB, fogARGB ...
desc.fogARGB        = (uint32_t)hazeByte << 24;

gpuMechSubmitted = GpuMechBatcher::instance().submitActor(desc);
```

**Replace with (insert before `submitActor`):**

```cpp
GpuMechSubmitDesc desc{};
desc.mechShape      = mechShape;
desc.mechType       = mechType;
desc.currentLOD     = (int)currentLOD;
desc.slot0TexHandle = (uint32_t)localTextureHandle;
// ... lightDataIndex, renderFlags, highlightARGB, fogARGB ...
desc.fogARGB        = (uint32_t)hazeByte << 24;

// M2.5: forward the RenderWorld handle to the GPU. M2 stored the
// handle on Mech3DAppearance::mechRenderHandle via the
// GameAdapters::Mech::registerMech path; M2.5 emits the bits to
// attachment-2 via mech.frag under MC2_OBJECT_ID_BUFFER.
//
// Handle::invalid().raw() == 0 by definition, so any pre-register
// frame or actor that missed registration writes 0 -- correctly
// classified as "background" by RenderWorld::lookupAtPixel.
desc.objectIdRaw    = getRenderWorldHandle().raw();

gpuMechSubmitted = GpuMechBatcher::instance().submitActor(desc);
```

**Note on accessor scope.** `mclib/mech3d.cpp` is inside
`Mech3DAppearance` member functions at this site (the surrounding
context shows `mechShape`, `hazeFactor`, etc. accessed as `this->`
members). `getRenderWorldHandle()` is the public M2 accessor on
`Mech3DAppearance` itself (`mech3d.h:487-489`); no adapter call, no
header changes. The M2 firewall ("Mech3DAppearance MUST NOT call the
adapter or RenderWorld directly") is preserved -- this is a pure read
of a `RenderCore` POD field.

---

## 5. Gating

### Env variable

`MC2_OBJECT_ID_BUFFER` (M1.5 substrate gate) reused with NO change.

```
MC2_OBJECT_ID_BUFFER unset / 0  -> OFF (default)
                              1  -> ON
```

No new env var. M2.6 introduces `MC2_MECH_PICK` (mirrors
`MC2_STATIC_PROP_PICK`); not in M2.5 scope.

### GLSL macro discipline (critical inline rule)

Per `CLAUDE.md` critical inline rules and
`memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`:

> GLSL macros do NOT inherit C++ build flags. `-DMY_FLAG` in
> `CMAKE_CXX_FLAGS` reaches only `.cpp` compilation. To gate a GLSL
> `#ifdef`, extend the `makeProgram()` prefix at C++ level.

Section 4.2.2 implements this: the C++ side reads
`RenderWorld::IsObjectIdBufferEnabled()` (process-lifetime cached) and
APPENDS `"#define MC2_OBJECT_ID_BUFFER 1\n"` to the
`makeProgram("mech", ...)` prefix string when env-ON. Without the env
var the macro is undefined in the linked program; the `out uint`
declaration AND the body write are absent.

Verification at slice plan execution time:

1. Dump compiled shader source before `glCompileShader` in env-ON build.
   Expect `#define MC2_OBJECT_ID_BUFFER 1` in the first ten lines of
   both `mech.vert` and `mech.frag` (the prefix passes through both
   stages via `makeProgram`).
2. Env-OFF: dump the same source; expect the macro absent.

### Lifecycle (per-process)

`RenderWorld::IsObjectIdBufferEnabled()` is cached at process start
(verified `RenderWorld/RenderWorld.h:74-78`). A process restart is
required to flip the gate. M2.5 inherits this discipline unchanged.

### Forbidden behaviors (M2.5-specific)

- Per-draw uniform upload of `objectIdRaw`. Rejected; see Section 3.
- Per-bucket uniform upload of `objectIdRaw`. Wrong by definition --
  per-bucket buckets contain many actors with different handles.
- Separate `MC2_MECH_OBJECT_ID_BUFFER` env var. Rejected; the
  attachment is shared, the gate is shared.
- Replacing static-prop ObjectID writes (M1.5 paths remain intact).
- Touching the MLR/CPU-fallback path (`mclib/txmmgr.cpp` ShapeRenderer;
  see Section 6).

---

## 6. MLR fallback gap (acknowledged asymmetry)

### The gap

Path B in the recon -- legacy MLR / `ShapeRenderer` /
`gos_tex_vertex_lighted` -- is the CPU fallback that fires only when
`GpuMechBatcher::submitActor` rejects an actor:

| Fallback trigger | Path B fires |
|---|---|
| `g_useGpuMechs = 0` (env override) | All mechs |
| Late type registration | First-frame on that type |
| Ring overflow | Above-ring-capacity actors |
| Tgl/GPU init failure | All mechs that frame |
| `u8` bone overflow (>255 bones) | That actor |
| Shader-program link failure | All mechs |

When Path B fires, the actor renders through `mclib/txmmgr.cpp:1868`
(`ShapeRenderer::render(rs->vb_, rs->ib_, ...)`) using the
`gos_tex_vertex_lighted` material. That shader pair has no ObjectID
output declaration; M2.5 does NOT add one.

### Impact

A mech that took Path B that frame writes NOTHING to attachment-2 at
its on-screen pixels. The pixels retain the per-frame clear value (0 =
`Handle::invalid()`). `RenderWorld::lookupAtPixel` on those pixels
returns `LookupResult{isValid=false}` -- same as background terrain.

User-visible effect: M2.6 Shift+click on a Path-B mech misses (no
selection); the click "falls through" to the CPU 2D-bounds spine.

### Why we accept the asymmetry

- **Default-build incidence is near-zero.** `g_useGpuMechs` defaults ON
  since 2026-05-09 (`gos_mech_batcher.cpp:45`). Ring capacity is 512
  instances; per-mission max mech count is 46 (mc2_24). Late
  registration fires once per typeXLOD on first-spawn; rare on a fully
  loaded mission. The dual-queue retirement campaign
  (`memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`)
  is closing the legacy path over multiple slices.
- **M1.5 set the same precedent.** Static-prop coalesce-path actors
  emit ObjectID via `PerDrawEntry.objectIdRaw`; static-prop legacy-path
  actors emit via `u_objectIdRaw` only when the producer fills it.
  M1.5 ships with both paths gated by `MC2_OBJECT_ID_BUFFER` but the
  recon-time grep showed the legacy-path upload as M1.6+ work (M1.5
  spec Section 5: "non-coalesce path... PLAN-TIME DECISION"). M2.5's
  Path-B gap is structurally identical.
- **The substrate degrades gracefully.** Background pixel = no
  selection = caller falls back to 2D-bounds. User experience: a
  fallback-frame mech is briefly un-pickable; subsequent frames where
  the actor takes Path A pick correctly. At 60 Hz this is invisible.
- **Closing the gap is a substitutive future slice.** Either:
  (a) Drive `gos_tex_vertex_lighted` through a per-draw `u_objectIdRaw`
      upload (M2.5 successor; one shader output add + one uniform-upload
      site), or
  (b) Wait for the MLR retirement campaign to delete Path B entirely
      (preferred per greybeard discipline).

### Documentary log (Q4 + Q6 resolved -- in-scope for M2.5)

Per Q4 resolution: the per-mission `[MECHBATCHER v1] event=summary`
banner gains a `gpu_mech_id_writes=N` counter (count of Path-A
submits with non-zero `desc.objectIdRaw`) so the substrate's writer
volume is measurable.

Per Q6 resolution amendment 2: an always-on companion counter
`mlr_mech_draws=M` is added alongside, surfacing the size of the
Path-B fallback per mission. Either both counters appear on the same
banner line, or split across two log lines if the MLR draw site lives
in a different TU (Section 12 Q6 amendment 2). The MLR counter is
NOT env-gated -- the M2.6 readiness decision depends on its value
even when `MC2_OBJECT_ID_BUFFER` is off.

### Verbatim documentation language (Q6 amendment 1)

The following paragraph MUST appear:
(a) in this section (here), and
(b) in CLAUDE.md "Known issues (current)" when M2.5 ships:

> MLR-rendered mechs do not write object IDs in M2.5.
> M2.6 pickup works only for GPU-batched mech pixels.
> If tier1 exercises MLR, pickup must fall back to legacy mover selection
> for those mechs and cannot claim full mech GPU-pick coverage.

The M2.6 plan-writer reads tier1 `mlr_mech_draws` and applies the
decision rule recorded in Section 12 Q6 amendment 3.

---

## 7. Cost analysis

### Memory

`GpuMechInstance` grows from 48 to 64 bytes (one uint slot + three
explicit pad uints; see Section 4.1.2 padding rationale).

Ring storage growth (corrected — instance capacity is dynamic):

```
ring frames                 = MECH_RING_FRAMES (= 3; gos_mech_batcher.h:119)
instance capacity per frame = s_instanceCapacity (dynamic; doubles on
                              overflow at gos_mech_batcher.cpp:283-298;
                              initial seed ~512)
old per-instance size       = 48 B
new per-instance size       = 64 B
per-instance delta          = 16 B
```

Instance capacity is NOT capped -- it grows dynamically by doubling
when a frame exceeds the current allocation
(`gos_mech_batcher.cpp:283-298`). The size-grow code path absorbs the
M2.5 struct grow with no code change: `sizeof(GpuMechInstance)` is
used everywhere, so the doubled allocation simply moves to
64-byte stride automatically. No new allocation site, no new buffer
class, no API change.

Per-mission live working set at max-mech mc2_24 (46 mechs):
`46 * 64 = 2944 bytes` per frame -- below cache-line noise floor.
Ring delta at the same load: `3 * 46 * 16 = 2208 B` (~2 KB across
the whole ring). At the initial seed capacity, ring delta is
`3 * 512 * 16 = 24 KB` -- still negligible against the multi-MB
shader-storage + vertex-buffer budgets already in flight (the bone
SSBO at `MECH_RING_FRAMES * s_boneCapacity * 64` typically dominates
by orders of magnitude). Either ring size figure (24 KB at initial
seed, ~2 KB at mc2_24 live load) is small enough to ignore against
the broader budget.

### CPU

- One `uint32_t` load + one `uint32_t` store per actor per frame at
  the submit site (`desc.objectIdRaw = getRenderWorldHandle().raw();`).
  `getRenderWorldHandle()` is a trivial inline accessor over a POD
  field on `Mech3DAppearance`, but under `RelWithDebInfo` we do NOT
  assume the call is inlined -- realistic cost is one load + one store
  per instance, < 10 ns per instance even with a function-call frame.
  No virtual dispatch, no allocation.
- One `uint32_t` assignment from `desc.objectIdRaw` to
  `inst.objectIdRaw` in `flush()`.
- The CPU-side fill is UNCONDITIONAL (Q3 resolved A). Env-OFF still
  runs the load + store at submit time and the SSBO-fill assignment
  at flush time; both are << 10 ns each per instance. The "off
  switch" lives at the GLSL macro gate (the shader output declaration
  is gone, so the SSBO field is never read by the GPU). This keeps
  instance data shape stable across env states and avoids a CPU
  branch at submit time. Total per-frame CPU cost at mc2_24
  (46 mechs): < 1 us.

### GPU

- One `flat` varying carry-through per vertex (mech.vert -> mech.frag).
  Drivers do not interpolate `flat` integer varyings; carry-through is
  one register write per provoking-vertex.
- One R32_UINT raster store per fragment (the static-prop M1.5 path
  already pays this cost; M2.5 is additive to fragment count, not
  bandwidth per fragment). M1.5 budget allocation: ~16 us per frame
  at full coverage on 7900 XTX (M1.5 spec Section 10). Mechs cover
  far less screen area than static props (max 46 actors, small
  silhouettes); additional cost < 1 us.

### Per-frame clear

Unchanged from M1.5. `glClearBufferuiv(GL_COLOR, 2, {0,0,0,0})` at
`beginScene()` already clears the whole attachment to zero each frame
(`gos_postprocess.cpp:495-499`).

### Total budget

M2.5 fits inside the M1.5 0.5ms p99 budget without additional headroom
analysis. The dominant cost remains the per-fragment raster store on
attachment-2, which static-prop M1.5 already validated on the 7900 XTX
target.

### Verification gate (Section 8)

Tier1 5/5 env-ON must show 0 fps regression beyond the M1.5 baseline.
Captured as part of the M2.5 plan-time perf snapshot.

---

## 8. Validation strategy

### Tier1 5/5 env-OFF (mandatory; zero pixel delta gate)

```
MC2_OBJECT_ID_BUFFER unset
```

Expected: byte-identical pixels vs M2 HEAD. The additive `objectIdRaw`
field on both `GpuMechSubmitDesc` and `GpuMechInstance` is zero-defaulted
and silently consumed; the GLSL macro gate ensures no shader-output
addition.

Standard invocation:

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

### Tier1 5/5 env-ON (mandatory; substrate active)

```
MC2_OBJECT_ID_BUFFER=1
```

Expected: substrate active, no visual delta beyond M1.5 baseline.
Frame-time delta <= 0.5ms p99 vs env-OFF baseline (inherits M1.5 budget;
mech contribution should be <<1 us per Section 7).

### Mech ObjectID self-test (mandatory; in-binary)

**Per Q1 resolved (Section 12):** ship a SEPARATE
`[MECH_OBJECT_ID_SELFTEST v1]` canary, NOT an extension of M1.5's
`[OBJECT_ID_SELFTEST v1]`. Wire it into `RenderWorld::init()` after
`runSubstrateSelfTest()` and `RunGameplayPickSelfTest()` (mirrors the
M2-pre pattern where `[GAMEPLAY_PICK_SELFTEST v1]` is its own
log lane separate from `[RENDER_WORLD_SELFTEST v1]`). Naming
suggestion: `RunMechObjectIdSelfTest()`.

Procedure:

1. Wait for mission to reach a stable frame (M1.5 already does this).
2. Sample one pixel known to land on a mech (mc2_03 has multiple early-
   spawn mechs at known map coordinates; pick a screen-space pixel from
   the camera bind state).
3. `glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &raw)`.
4. Decode: `Handle h; h.bits = raw;`.
5. Look up `s_objectRecords[h.index()]`.
6. Assert: `record.alive == true`, `record.generation == h.generation()`,
   `record.kind == RenderObjectKind::Mech`.
7. Round-trip: assert `h.raw() == raw`.
8. Emit `[MECH_OBJECT_ID_SELFTEST v1] result=PASS|FAIL kind=N gen=N
   handle=0xNN screen=(x,y)`.

If the chosen pixel falls on background (mech moved, camera shifted),
emit `result=SKIPPED reason=background_pixel` -- not a failure (mirrors
M1.5 SKIPPED path at `RenderWorld.cpp:161`).

### User-driven canary (mandatory; substrate inspectability)

After Tier1 passes:

1. Launch mc2_03 with `MC2_OBJECT_ID_BUFFER=1 MC2_STATIC_PROP_PICK=1`.
2. Shift+click on a static prop. Expected: `[STATIC_PROP_PICK v1] hit ...`
   line emits (M1.6 behavior; M2.5 does NOT alter this).
3. Shift+click on a mech. Expected: NO `[STATIC_PROP_PICK v1] hit ...`
   line (the spine's mover-first gate short-circuits per M2-pre; mech
   selection falls to legacy 2D-bounds path). M2.5 does not flip this.
4. With `MC2_MECH_OBJECT_ID_SELFTEST=1` set: on mission load, the
   self-test emits `[MECH_OBJECT_ID_SELFTEST v1] result=PASS` -- this
   is the substrate inspectability proof.

The shift-click flip on mechs is M2.6 work. M2.5 ships the substrate
and proves it round-trips via the in-binary self-test.

### Greybeard / adversarial-review canary (mandatory before promotion)

Run `.claude/skills/greybeard.md` against this spec at plan time;
expect the META-FIX vs PATCH ruling (Section 9 below) to survive
review. Run `.claude/skills/adversarial-plan-review.md` against the
follow-up plan; expect 0 CRIT and any MAJORs resolved before EXECUTING.

### Firewall check (no changes needed)

`scripts/check-include-firewall.sh` already passes for M2 HEAD. M2.5's
sole include add (`gos_mech_batcher.cpp` -> `RenderWorld/RenderWorld.h`)
is NOT policed by the script -- `GameOS/` is outside `SCOPE_DIRS`
(`scripts/check-include-firewall.sh:22`). The sibling include at
`gos_static_prop_batcher.cpp:3` shipped under M1.5 without a script
edit for the same reason. M2.5 adds nothing the script checks for.
Reviewer-discipline verification of the include direction
(engine -> engine, public header only) is the gate. See §9.

### Shader-output uniqueness check (M1.5 grep gate, extended)

The M1.5 grep gate:

```
grep -rE 'layout\s*\(\s*location\s*=\s*2\s*\)\s*out' shaders/
```

Expected match set after M2.5: `static_prop.frag` AND `mech.frag` only.
Plan-time gate updates the allowlist comment in the build script.

### Render-contract registry update

`shaders/mech.frag`'s `[RENDER_CONTRACT]` comment block (lines 11-15)
gains `GBuffer2: rc_gbuffer2_objectIdU32` when the macro is on:

```
//   [RENDER_CONTRACT]
//     Pass:           Mech
//     Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//     GBuffer1:       rc_gbuffer1_screenShadowEligible
//     GBuffer2:       rc_gbuffer2_objectIdU32  // M2.5 (#ifdef MC2_OBJECT_ID_BUFFER)
//     StateContract:  depthTest=true, depthWrite=true, blend=Opaque, requiresMRT=true
```

Coordinate with `mclib/render_contract.h` (M1.5 added the GBuffer2
contract; M2.5 extends to a second shader).

---

## 9. Firewall section

### What this slice touches

| Path | Type | New dep? |
|---|---|---|
| `GameOS/gameos/gos_mech_batcher.h` | struct schema | no |
| `GameOS/gameos/gos_mech_batcher.cpp` | impl + new `RenderWorld.h` include | **yes** (unpoliced — see below) |
| `shaders/mech.vert` | GLSL | no |
| `shaders/mech.frag` | GLSL | no |
| `mclib/mech3d.cpp` | one assignment at submit site | no (uses existing M2 accessor) |

### Firewall direction

The single new dependency: `gos_mech_batcher.cpp` -> `RenderWorld/RenderWorld.h`.

- `GameOS/` -> `RenderWorld/` was opened in M1.5 by
  `gos_static_prop_batcher.cpp:3` (`#include
  "../../RenderWorld/RenderWorld.h"`). M2.5 adds a SECOND GameOS-side
  includer of the same header for the same reason
  (`IsObjectIdBufferEnabled` gate).
- **The firewall script does NOT police this include.**
  `scripts/check-include-firewall.sh:22` defines
  `SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer
  MaterialSystem DebugRenderer RenderDeviceGL"`. `GameOS/` is outside
  SCOPE_DIRS, so neither the M1.5 sibling include nor this M2.5 add
  produces a script-side gate. Discipline is reviewer-enforced, not
  script-enforced.
- **Reviewer-discipline contract.** Reviewer must visually verify:
  (a) the include target is the public header `RenderWorld/RenderWorld.h`,
      NOT `RenderWorld/legacy/*` or any private surface;
  (b) the include direction is engine -> engine -- `RenderWorld.h`
      exposes only pure types and free functions, no transitive reach
      into `code/` / `mclib/` (game-side) headers;
  (c) the consumer (`gos_mech_batcher.cpp`) uses only
      `IsObjectIdBufferEnabled()` from the header -- no broader API
      surface adoption.
- `RenderWorld/RenderWorld.h` is the public header -- GameOS-side
  reaches MUST stop at this surface (no `RenderWorld/legacy/*` reach).
  M2.5 honors this.
- `mclib/mech3d.cpp` does NOT gain a new include. It uses
  `Mech3DAppearance::getRenderWorldHandle()` which returns
  `RenderCore::RenderObjectHandle` (pure POD in `RenderCore/Handle.h`).
  `RenderCore` is already in scope for `mech3d.h` via the M2 field
  declaration (`mech3d.h:478`).

### What this slice does NOT touch

- `GameAdapters/` -- no new bridge code. The handle is already on
  `Mech3DAppearance` from M2; M2.5 reads it from inside the appearance
  class itself.
- `RenderWorld/legacy/` -- the legacy backend for static props is M1
  / M1.5 territory; mechs do not have a legacy backend in this scope
  (M2 introduced `clearAllMechRecords` directly in `RenderWorld.cpp`).
- `RenderCore/` -- handle type is consumed via existing accessor; no
  new POD types added.
- `mclib/` outside `mech3d.cpp` -- no other mclib TU touches the
  submit site.
- `code/` -- no game-side TU edits; `tryGameplayPick` and missiongui
  spine are M2.6 territory.
- `mclib/txmmgr.cpp` -- the MLR/legacy path (Section 6) is explicitly
  out of scope.
- `shaders/shadow_*` -- shadow shaders are separate FBO; no work.

### Firewall-script change set

**None.** `scripts/check-include-firewall.sh` does not police
`GameOS/`-originating includes (GameOS not in `SCOPE_DIRS` at
`scripts/check-include-firewall.sh:22`). The M1.5 sibling include
shipped without a script edit; M2.5's add is symmetric and likewise
requires no script edit and no allowlist line.

The discipline this slice relies on is reviewer-enforced (see
"Reviewer-discipline contract" above). If a future hardening pass
adds `GameOS/` to `SCOPE_DIRS`, both the M1.5 sibling include AND
M2.5's add would need allowlist lines in the same patch.

---

## 10. Greybeard analysis -- META-FIX vs PATCH ruling

### Claim

M2.5 is a **META-FIX**.

### Argument

**Bug class being retired:** "mech fragments do not produce
ObjectID writes, so `lookupAtPixel` cannot identify a mech pixel."

**The fix:** add per-instance `objectIdRaw` to the existing
`GpuMechInstance` SSBO; forward through `mech.vert` flat varying;
emit at `mech.frag` `layout(location=2)` under the existing GLSL gate.

**Symmetric to M1.5 coalesce path.** The static-prop coalesce path
shipped exactly this shape: per-instance ObjectID in a per-draw SSBO
record, gated by `MC2_OBJECT_ID_BUFFER`, emitted at `location=2`.
M2.5 is the second user of the same recipe -- and the recipe
generalizes to any future batched-instanced renderer that wants
ObjectID coverage (M3 terrain chunks, M4 VFX particles).

**One symmetric edit retires the bug class.** Concretely:

- One field add per struct (CPU `GpuMechSubmitDesc` + std430
  `GpuMechInstance`; lockstep with `mech.vert`).
- One submit-site assignment (`mech3d.cpp` reads `getRenderWorldHandle()`).
- One per-instance fill (`gos_mech_batcher.cpp` `flush()`).
- One vert-shader forward varying.
- One frag-shader output declaration + write.
- One GLSL macro prefix injection (mirrors `gos_static_prop_batcher.cpp:510-521`).

Zero new files. Zero new env vars. Zero new APIs. Zero FBO changes.
Zero `setSceneDrawBuffers` extensions. Zero shadow-pass changes.

### Alternatives considered (rejected)

#### A. Separate mech-ID FBO attachment

Add `GL_COLOR_ATTACHMENT3` and a second R32_UINT texture for mech IDs;
`lookupAtPixel` reads both and merges by priority.

**Reject:** doubles FBO memory at the attachment-2 slot, doubles
readback indirection, splits picking results across two textures,
forces every M2.6+ consumer (`tryGameplayPick`) to merge two lookups.
Negates the M1.5 unified-table win.

#### B. Per-draw `glProgramUniform1ui` upload at each bucket draw

Upload the ObjectID once per bucket via a uniform.

**Reject:** per-bucket buckets contain MANY actors. A single uniform
cannot carry per-instance data -- every mech in the bucket would get
the same ObjectID, breaking the per-pixel disambiguation contract.
Would force one draw per actor, undoing Track D's primary win.

#### C. CPU-side per-mech screen-rect cache

Maintain a CPU-side map of mech screen bounds per frame; resolve
clicks against the cache.

**Reject:** skips the GPU readback entirely but loses per-pixel
precision (mechs occlude each other and have non-rectangular
silhouettes), duplicates the cull/visibility/projection chain on
the CPU. Strict regression vs. M1.5's correctness contract.

#### D. Wait for MLR retirement, do nothing now

Defer M2.5 until the legacy CPU fallback path is retired so the
"Path-B mechs invisible to picking" gap closes by deletion.

**Reject:** MLR retirement is a multi-slice campaign without a clean
endpoint; default-build incidence of Path B is near-zero (Section 6);
waiting blocks M2.6 (the M2.5+M2.6 pair is the actual
"select-a-mech-with-the-mouse" user value).

### Ruling

**META-FIX.** Recipe matches the M1.5 coalesce-path precedent;
generalizes to M3+. Bug class "batched-instanced renderer cannot
participate in ObjectID picking" is retired by introducing the
canonical per-instance SSBO-field pattern, with M2.5 as the second
documented user.

### Debt acknowledgment

The MLR/CPU fallback gap (Section 6) is a known asymmetry, not new
debt -- it mirrors the M1.5 legacy-path gap and resolves by either
(a) one substitutive successor slice on `gos_tex_vertex_lighted` OR
(b) MLR retirement campaign deletion. Documented in Section 6;
not a blocker for M2.5 ship.

---

## 11. Threat model / known gotchas

### AMD integer-MRT trap (M1.5 canary; inherited)

The M1.5 spec Section 6 documented AMD's integer-format MRT-attachment
behavior as a runtime canary requirement (`OBJECT_ID_SELFTEST`)
because the GL spec's "undefined writes to unbound output locations"
clause has historically driven AMD-side fallback paths. M1.5 shipped a
passive validator (`[OBJECT_ID_SELFTEST v1] result=PASS sampled=10
valid_hits=4 invalid_hits=6` on mc2_03 / 7900 XTX) that proved
clear-to-zero + write + non-writer-pixel behavior is correct on the
target driver.

**M2.5 inherits this proof.** M2.5 adds a SECOND writer to the same
attachment slot; if the M1.5 canary passed, M2.5's add is structurally
covered by the same proof. The recommended `[MECH_OBJECT_ID_SELFTEST v1]`
extension (Section 8) provides explicit mech-pixel coverage as
defense-in-depth.

### Generation / staleness implications of new M2 handles

M2 added a chip fix (`05f1f2d`): `RenderWorld::destroyMech()` is
idempotent (re-destroy is a no-op). This means a mech may be destroyed
mid-frame between the CPU submit (`mech3d.cpp:2586`) and the GPU draw
(`gos_mech_batcher.cpp:1289`). The substrate handles this correctly:

- `inst.objectIdRaw` is captured at submit time. If the mech is
  destroyed between submit and draw, the GPU still writes the OLD
  handle bits to attachment-2.
- `lookupAtPixel` reads back the handle, looks up
  `s_objectRecords[h.index()]`. If `destroyMech` ran, the slot's
  `generation` has bumped (M2 path: `RenderWorld.cpp` destroy bumps
  generation and clears `alive`).
- Generation check: `record.generation != h.generation()` ->
  `isValid=false`. The stale pixel is correctly classified as
  background.

This matches the M1.5 spec Q4 staleness semantics; M2.5 inherits with
no new behavior.

### Lockstep edit risk

The M2.5 edit touches three artifacts that MUST agree on the
`GpuMechInstance` std430 layout:

1. `GameOS/gameos/gos_mech_batcher.h` -- C++ struct + `static_assert`
   chain at lines 44-51.
2. `shaders/mech.vert` -- GLSL struct at lines 30-37.
3. `GameOS/gameos/gos_mech_batcher.cpp` -- per-instance fill at lines
   1093-1104.

Failure mode: edit 2 lands without 1 (or vice versa) -> GLSL reads
garbage at `inst.objectIdRaw`, mech.frag writes garbage to
attachment-2, `lookupAtPixel` returns nonsensical handles, M2.6
selection fires on wrong mechs (or crashes on out-of-range index
into `s_objectRecords`).

**Mitigation:** all three artifacts MUST be in the same commit. Plan
step (M2.5 plan-time gate) requires a single-commit diff covering
items 1-3 with the `static_assert` updated to `sizeof == 64`. The
GLSL struct comments cross-reference `gos_mech_batcher.h:48`.

**Hot-reload-without-relink trap (specific failure mode).** If the
shader is hot-reloaded after the C++ struct grows to 64 B but BEFORE
a full relink, the running exe still holds the OLD 48 B stride for
its SSBO uploads while the freshly loaded shader expects the NEW 64 B
layout. The shader then reads `objectIdRaw` at offset 48 from the
OLD instance stride -- which in the OLD 48 B layout is the NEXT
instance's first field (`typeLodRecordIndex`). The visible symptom
is WRONG-MECH MESHES (not wrong handles, and not garbage): every
instance reads the next instance's mesh ID, so mechs render with
their neighbor's geometry. Picking still "works" in the sense that
`lookupAtPixel` returns a valid handle, but it returns the wrong
mech's handle for the pixel.

**Detection rule.** If the user-driven canary or visual smoke shows
wrong meshes (not wrong handles, not garbage) after a shader edit in
this slice, suspect this trap. Full-relink the exe BEFORE further
debugging -- per the `CLAUDE.md` "Full relink before deploy" rule,
class-layout changes require `--clean-first` or manual `.obj` /
`mc2.exe` deletion before `cmake --build`. This M2.5 lockstep section
exists to restate that rule with the specific symptom-to-cause
mapping; do not skip the relink and chase the symptom in shader code.

### `flat` qualifier mandatory for integer varyings

`v_objectIdRaw` is `uint`. GL spec FORBIDS linear interpolation of
integer varyings; the qualifier MUST be `flat` on BOTH `out` (vert)
and `in` (frag). A missing `flat` produces a GLSL link error at
program-load time; the M1.5 program-load failure path
(`[MECHBATCHER v1] event=shader_fail`) catches this -- the GPU mech
path goes silently dormant. Validator: env-ON tier1 must show
`event=shader_ok` in the log (`gos_mech_batcher.cpp:274`).

### Existing M1.5 `[OBJECT_ID_SELFTEST v1]` passive canary

The M1.5 passive canary fires at mission start in env-ON builds and
samples N pixels from the attachment, asserting clear-value + write
behavior. M2.5 does NOT need to extend this canary -- it samples
attachment-2 generically without knowing which writer produced each
pixel. After M2.5, the canary's `valid_hits` / `invalid_hits` ratio
will shift toward more `valid_hits` (mechs add writers); this is
expected and not a regression.

### shader_builder `uniform uint` crash

Per `memory/uniform_uint_crash.md`, the project's shader compiler
crashes on `uniform uint`. M2.5 declares NO new uniforms (the
ObjectID rides the SSBO via `inst.objectIdRaw` -> `flat out uint`
varying); the trap does not apply.

### shader hot-reload silent failure

Per `CLAUDE.md`: bad GLSL compile leaves old shader active. After
editing `mech.vert` / `mech.frag`, console output MUST show
`event=shader_ok prog=N` (`gos_mech_batcher.cpp:274`); absence
indicates the env-ON shader build failed and mechs may be using a
stale program. Validator: tier1 env-ON expects this log line.

### Full relink discipline

Per `CLAUDE.md` ("Full relink before deploy when load-bearing
functions change"): `GpuMechInstance` size change is a class-layout
change touching multiple TUs. The plan-time deploy step MUST use
`--clean-first` or manually delete `mc2.exe` + the changed
`.obj` files before `cmake --build`.

### Path separator + line endings

Per `CLAUDE.md`: engine uses `PATH_SEPARATOR=/`; no hardcoded `\\`.
M2.5 touches no path strings. Shader files are LF-only (Git config);
no CRLF concerns.

---

## 12. Resolved decisions (Q1-Q6)

All open questions from the draft spec were resolved by the user
(`docs/superpowers/specs/2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-spec-resolutions.md`).
This section records the binding decisions; the plan-writer and
reviewer treat these as fixed, NOT as open leans.

### Q1 (RESOLVED). Self-test canary shape

**Decision:** B -- separate `[MECH_OBJECT_ID_SELFTEST v1]` canary.

Static props and mechs use different producer surfaces (M1.5
coalesce/legacy paths vs M2.5 `GpuMechBatcher` SSBO). Separate
failure signals are worth the extra log line. Matches M2-pre
precedent (`[GAMEPLAY_PICK_SELFTEST v1]` separate from
`[RENDER_WORLD_SELFTEST v1]`).

**Spec body implication.** Section 8's "extend existing canary"
recommendation is OVERRIDDEN: M2.5 ships a NEW canary
`RunMechObjectIdSelfTest()` (or similarly named) wired into
`RenderWorld::init()` AFTER `runSubstrateSelfTest()` and
`RunGameplayPickSelfTest()`. Log schema:
`[MECH_OBJECT_ID_SELFTEST v1] result=PASS|FAIL kind=N gen=N
handle=0xNN screen=(x,y)`. SKIP path inherits M1.5 shape
(`result=SKIPPED reason=background_pixel`).

### Q2 (RESOLVED). Pad-field naming in `GpuMechInstance`

**Decision:** A -- generic `_pad1/_pad2/_pad3` (only `objectIdRaw`
is named).

Do NOT pre-name future terrain/VFX/overlay fields before those specs
exist. Rename only the one field actually consumed now.

**Spec body implication.** Section 4.1.2 (C++) and 4.3.1 (GLSL) use
`_pad1/_pad2/_pad3` -- the resolution-preferred naming. Plan-writer
and implementor follow this so the rename in a future slice is
unambiguous (`_padN` -> `<consumer>Id`). Struct still grows 48 B ->
64 B by adding ONE consumed field plus three generic pads.

### Q3 (RESOLVED). Submit-time gating of `desc.objectIdRaw`

**Decision:** A -- unconditional CPU fill.

`desc.objectIdRaw = mech.getRenderWorldHandle().raw();` always fires
regardless of `MC2_OBJECT_ID_BUFFER`. The env var gates the SHADER
OUTPUT and the FBO ATTACHMENT, NOT CPU-side data preparation.
Keeps instance data shape stable so env-ON is a pure render-path
toggle.

**Spec body implication.** Section 4.5's submit-site code shows the
unconditional assignment correctly (no `if (envFlag)` wrapper). The
realistic cost (per §7 CPU): one load + one store per instance,
< 10 ns each under `RelWithDebInfo` even without guaranteed inlining
of `getRenderWorldHandle()`. Total per-frame at mc2_24 (46 mechs):
< 1 us.

### Q4 (RESOLVED). `mech_id_writes=N` counter timing

**Decision:** A -- ship in M2.5.

M2.5 owns the mech writer, so M2.5 owns writer observability. Counter
on the existing `[MECHBATCHER v1] event=summary` line:

```
[MECHBATCHER v1] event=summary ... mech_id_writes=N
```

Gives an immediate substrate gate before M2.6 (pickup) depends on it;
M2.6 can then assert non-zero `mech_id_writes` before testing picks.

**Spec body implication.** Thread one `uint64_t` counter through
`GpuMechBatcher::flush()` (or equivalent state struct). Increment on
each instance fill when `inst.objectIdRaw != 0`. Emit on summary.
Plan-writer treats this as in-scope for M2.5, NOT a deferrable
sub-task.

### Q5 (RESOLVED). Recon-vs-code discrepancy on `RenderWorld.h` include

**Decision:** include add is fine; NO firewall-script change is
needed (correcting the original lean).

Per MAJOR M1 in the adversarial review:
`scripts/check-include-firewall.sh:22` defines `SCOPE_DIRS` without
`GameOS`, so the new include from `gos_mech_batcher.cpp` is unpoliced
by the script -- exactly like the M1.5 sibling include at
`gos_static_prop_batcher.cpp:3`. No allowlist line to add, no script
edit in the plan. Sections 4.2.2, 8, and 9 reflect this.

**Spec body implication.** Reviewer verifies the include is to the
PUBLIC `RenderWorld/RenderWorld.h` header only, that it does not
pull `code/` or `mclib/` through, and that the consumer uses only
`IsObjectIdBufferEnabled()`. No script change is part of the M2.5
slice.

### Q6 (RESOLVED). MLR/CPU-fallback gap

**Decision:** A -- accept the gap. **Spec amendments required** (below).

Do NOT block M2.5 or M2.6 on MLR retirement. Document the gap hard
in the spec, add a CLAUDE.md known-issues flag, AND add a
**measurable** MLR-side counter so the "MLR is rare" assumption
becomes verifiable rather than asserted.

#### Spec amendment 1 (verbatim documentation language)

The following paragraph MUST appear verbatim in:
(a) this spec (Section 6 "MLR fallback gap"), and
(b) the CLAUDE.md "Known issues (current)" section once M2.5 ships:

> MLR-rendered mechs do not write object IDs in M2.5.
> M2.6 pickup works only for GPU-batched mech pixels.
> If tier1 exercises MLR, pickup must fall back to legacy mover selection
> for those mechs and cannot claim full mech GPU-pick coverage.

Section 6 below already covers the technical content; the verbatim
sentence is repeated there in a dedicated "documentation language"
sub-block to keep it copy-pasteable.

#### Spec amendment 2 (always-on `mlr_mech_draws` counter)

Add a SECOND always-on counter alongside the M2.5 `mech_id_writes`:

```
[MECHBATCHER v1] event=summary ... gpu_mech_id_writes=N mlr_mech_draws=M
```

(or split across two log lines if MLR draws live in a different TU --
either shape is acceptable as long as both counters surface per
mission.) The counter is ALWAYS-ON (NOT env-gated) because it informs
the M2.5 / M2.6 coverage decision; gating it behind
`MC2_OBJECT_ID_BUFFER` would lose the very signal that drives the
M2.6 readiness check.

Locate MLR mech draw sites by grep over `mclib/mlr/` and the
CPU-fallback render path in `mclib/mech3d.cpp` (the recon-identified
`mechShape->Render(true)` site at `mclib/mech3d.cpp:2608` is the
primary candidate -- plan-writer confirms at plan time). Increment
once per MLR mech draw call. The bookkeeping for the new counter
is in `GpuMechBatcher` (or a new tiny shared diagnostic singleton) so
the `[MECHBATCHER v1] event=summary` emit site reads both values
together.

#### Spec amendment 3 (pre-defined M2.6 decision rule)

The M2.6 plan-writer (next slice) MUST consult the tier1 `mlr_mech_draws`
value before deciding scope. The rule, defined NOW:

- **If tier1 ever shows `mlr_mech_draws > 0` on any mission:**
  - M2.6 MUST preserve the mover-first legacy fallback for those
    mechs (no behavior change for MLR-rendered actors).
  - M2.6 CANNOT claim "full mech GPU-pick coverage" in
    CLAUDE.md / handoffs / shipped summaries.
  - The MLR path becomes a NAMED BLOCKER for the next slice that
    wants full coverage (candidate slices:
    "M2.7 MLR-mech object-ID write" or "M2.7 MLR retirement").

- **If tier1 consistently shows `mlr_mech_draws == 0` across all 5
  missions for ~3 ship cycles:**
  - The gap is provably-rare-in-practice.
  - M2.6 can ship without the conditional fallback warning.
  - The "M2.7 MLR work" candidate can be deprioritized to backlog.

This decision rule is captured here (not deferred to M2.6 spec) so
the M2.6 plan-writer does not re-derive it.

#### Spec amendment 4 (CLAUDE.md known-issues flag at ship time)

When M2.5 ships, the CLAUDE.md "Known issues (current)" list MUST
gain a bullet using the verbatim language from amendment 1 above.
This is a ship-step requirement, not a plan-step requirement.

---

## Appendix A. Verified prior art (grep-confirmed 2026-05-23)

| Citation | Verified |
|---|---|
| `mech3d.h:478` `mechRenderHandle` field | grep |
| `mech3d.h:487-489` `getRenderWorldHandle()` accessor | grep |
| `gos_mech_batcher.h:35-51` `GpuMechInstance` 48B std430 + static_asserts | read |
| `gos_mech_batcher.h:88-106` `GpuMechSubmitDesc` | read |
| `gos_mech_batcher.h:119` `MECH_RING_FRAMES = 3u` | read |
| `gos_mech_batcher.cpp:45` `g_useGpuMechs` default-on | read |
| `gos_mech_batcher.cpp:222-233` `makeProgram("mech", ...)` shader load | read |
| `gos_mech_batcher.cpp:1093-1104` per-instance fill loop | read |
| `gos_mech_batcher.cpp:1109-1111` SSBO binding 0 bind range | read |
| `gos_mech_batcher.cpp:1289` `glDrawElementsInstancedBaseVertex` in flush() | grep |
| `gos_mech_batcher.cpp` does NOT include `RenderWorld.h` -- recon was wrong; M2.5 adds it | grep negative |
| `gos_mech_batcher.cpp` no `glDrawBuffers` call -- inherits from beginScene | grep negative |
| `gos_static_prop_batcher.cpp:3` `#include "../../RenderWorld/RenderWorld.h"` | grep |
| `gos_static_prop_batcher.cpp:510-521` GLSL prefix injection pattern | read |
| `shaders/mech.vert:30-40` GLSL `GpuMechInstance` struct + SSBO binding | read |
| `shaders/mech.vert:79-80` `instIdx` + `inst` fetch | read |
| `shaders/mech.vert:165-173` end-of-main varying assignments | read |
| `shaders/mech.frag:36-37` `layout(location=0/1)` outputs | read |
| `shaders/mech.frag:75-77` end-of-main color writes | read |
| `shaders/static_prop.frag:44` `PerDrawEntry.objectIdRaw` (coalesce) | read |
| `shaders/static_prop.frag:56-60` legacy-path `u_objectIdRaw` | read |
| `shaders/static_prop.frag:68-72` `layout(location=2) out uint v_objectId` decl | read |
| `shaders/static_prop.frag:174-183` `MC2_OBJECT_ID_BUFFER` body write | read |
| `mclib/mech3d.cpp:2549-2586` `GpuMechSubmitDesc desc{}` + `submitActor(desc)` | read |
| `mclib/mech3d.cpp:2608` MLR fallback `mechShape->Render(true)` | grep |
| `mclib/txmmgr.cpp:1499` `class ShapeRenderer` (legacy path) | recon |
| `mclib/txmmgr.cpp:1868` `ShapeRenderer::render(...)` (legacy submit) | recon |
| `RenderWorld/RenderWorld.h:74-85` `IsObjectIdBufferEnabled()` API + cache contract | read |
| `RenderWorld/RenderWorld.h:108` `objectIdRawForStaticPropRecipe(...)` | read |
| `RenderWorld/RenderWorld.h:116-120` `RenderObjectKind` enum (StaticProp, Mech) | read |
| `RenderWorld/RenderWorld.h:132-150` `RenderObjectRecord` (kind + debugCookie) | read |
| `RenderWorld/RenderWorld.h:154-167` `LookupResult` | read |
| `RenderWorld/RenderWorld.h:176` `lookupAtPixel(int, int)` | read |
| `RenderWorld/RenderWorld.cpp:40,365` `RunGameplayPickSelfTest` validator hook | grep |
| `RenderWorld/RenderWorld.cpp:233` `[OBJECT_ID_SELFTEST v1]` line emit | grep |
| `GameOS/gameos/gos_postprocess.cpp:31` `setSceneDrawBuffers(mode, bool ready)` | read |
| `GameOS/gameos/gos_postprocess.cpp:324-333` `sceneObjectIdTex_` allocation | grep |
| `GameOS/gameos/gos_postprocess.cpp:339-340` createFBOs MRT bind | read |
| `GameOS/gameos/gos_postprocess.cpp:488-499` beginScene MRT bind + clear | read |

---

## Appendix B. Glossary

- **Attachment-2** -- `GL_COLOR_ATTACHMENT2`, M1.5's R32_UINT MRT slot
  on the main scene FBO. Shared by static-prop (M1.5) and mech (M2.5)
  writers.
- **GBuffer2** -- contract name for attachment-2; carries the packed
  `Handle.raw()` value.
- **`GpuMechInstance`** -- std430 per-instance SSBO record consumed by
  `mech.vert`. M2.5 grows from 48B to 64B with one `uint32_t
  objectIdRaw` field + three pad uints.
- **`GpuMechSubmitDesc`** -- CPU-side per-actor carrier filled by
  `mech3d.cpp` and consumed at `flush()` time. M2.5 gains one
  `uint32_t objectIdRaw` field; mirrors the SSBO field.
- **Path A / Path B** -- recon-defined labels for the GPU mech batcher
  (A; M2.5 writes ObjectID) vs. legacy MLR/ShapeRenderer CPU fallback
  (B; M2.5 leaves alone).
- **META-FIX vs PATCH** -- greybeard discipline term. M2.5 is the
  former because it retires the bug class via the canonical
  per-instance SSBO-field recipe established by M1.5 coalesce.

---

## Appendix C. Cross-spec references

- `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`
  -- the substrate this slice consumes.
- `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md`
  -- first gameplay-side ObjectID consumer; M2.5 enables the mech
  successor.
- `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md`
  -- introduced `Mech3DAppearance::mechRenderHandle`; M2.5 consumes the
  field.
- `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md`
  -- shared `tryGameplayPick(req)` + `screenToFboPixel` spine; M2.6
  extends to mechs.
- `docs/superpowers/explorations/2026-05-23-renderworld-slice-m2-5-recon.md`
  -- recon-only source for this spec. M2.5 grep-verified all
  load-bearing file:line at write-time and corrected one discrepancy
  (Section 4.2.2 / Q5).
- `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`
  -- macro injection at C++ level; load-bearing for Section 4.2.2.
- `memory/uniform_uint_crash.md` -- not applicable (M2.5 adds no
  uniforms).
- `memory/feedback_offload_must_be_substitutive_not_additive.md`
  -- substitutive-not-additive discipline; Section 9 META-FIX argument.
- `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`
  -- the MLR retirement campaign that eventually closes Section 6's
  gap.
- `memory/vulkan_prep_explicit_device_discipline.md` -- M2.5's SSBO
  field add is Vulkan-prep clean (no implicit cross-call GL state;
  the per-instance write rides the existing SSBO bind).

---

SPEC STATUS: REVISED — adversarial CONDITIONAL-PASS findings applied + Q1-Q6 resolved

## Resolved decisions summary

See Section 12 for the full text. All Q1-Q6 are RESOLVED (binding,
not open):

1. **Q1 -> B.** Separate `[MECH_OBJECT_ID_SELFTEST v1]` canary
   (NOT extend M1.5's `[OBJECT_ID_SELFTEST v1]`).
2. **Q2 -> A.** Generic `_pad1/_pad2/_pad3` pad-field names; only
   `objectIdRaw` is named.
3. **Q3 -> A.** Unconditional CPU fill of `desc.objectIdRaw`. Env
   gates the shader output / FBO, NOT CPU prep.
4. **Q4 -> A.** Ship `gpu_mech_id_writes=N` counter on
   `[MECHBATCHER v1] event=summary` in M2.5.
5. **Q5 -> resolved.** Include add to `gos_mech_batcher.cpp` is fine;
   firewall script does NOT police GameOS includes
   (`scripts/check-include-firewall.sh:22` SCOPE_DIRS excludes GameOS).
   No allowlist line, no script change. Reviewer-discipline gate only.
6. **Q6 -> A with SPEC AMENDMENT.** Accept the MLR/CPU-fallback gap;
   ship verbatim documentation language in §6 + CLAUDE.md known-issues;
   add always-on `mlr_mech_draws=M` counter; pre-define the M2.6
   readiness decision rule based on tier1 `mlr_mech_draws` (Section 12).

## Adversarial findings applied

This revision pass applied the four findings from
`docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-5-spec-adversarial.md`:

- **MAJOR M1** -- firewall-allowlist claim corrected. `GameOS/` is not
  in SCOPE_DIRS; the include add is unpoliced. Sections 4.2.2, 8, 9
  reframed to reviewer-discipline.
- **MINOR m1** -- instance-capacity framing corrected. Capacity is
  dynamic (doubles on overflow at `gos_mech_batcher.cpp:283-298`);
  ring delta at mc2_24 live load is ~2 KB, at initial seed is 24 KB.
  Section 7 rewritten.
- **MINOR m2** -- hot-reload-without-relink trap added to Section 11
  "Lockstep edit risk" with the wrong-mech-meshes detection rule.
- **MINOR m3** -- Q3 cost re-framed. "One load + one store per
  instance, < 10 ns under `RelWithDebInfo` without guaranteed
  inlining" replaces the "one mov; trivial" framing. Sections 7
  CPU and 12 Q3 updated.

End of spec.
