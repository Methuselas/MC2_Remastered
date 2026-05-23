# RenderWorld Slice M2 -- MechRenderAdapter Spec

- Status: DRAFT
- Date: 2026-05-23
- Type: contract / API boundary spec (DOC-ONLY; no code in this artifact)
- Relation to roadmap: `docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md`
  item 20 (Sequence A1); slice M2 in the adapter migration ladder
- Spec parent: `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`
  (boundary contract, Section 10 adapter deletion criteria, Section 12 firewall)
- Plan template: `docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`
  (format and task discipline reference)
- Greybeard ruling: PATCH (justified) -- see Section 12
- META-FIX paid: M2-pre (`tryGameplayPick` + `screenToFboPixel` extraction,
  shipped 2026-05-23 at HEAD `16e3d53`)
- Zero pixel delta required: yes
- Adversarial review: to be conducted at plan-stage before Task 1 executes

---

## 1. Status and metadata

```
Slice arc:
  M1        route-only StaticPropRenderAdapter        SHIPPED 2026-05-23
  M1.5      object-ID buffer substrate                SHIPPED 2026-05-23
  M1.6      static-prop Shift+click pick              SHIPPED 2026-05-23
  M2-pre    tryGameplayPick / screenToFboPixel META-FIX  SHIPPED 2026-05-23
  M2        route-only MechRenderAdapter              THIS SPEC
  M2.5      mech object-ID substrate (per-mech write to attachment-2)
  M2.6      mech pickup via extracted spine
  M3        TerrainRenderAdapter
  M4        VfxRenderAdapter
  M5        OverlayRenderAdapter
  M6        (verify) firewall locked; no raw GL access from game side
```

HEAD at spec-write time: `16e3d53` (M2-pre shipped).

---

## 2. Relation to slice arc

M2 mirrors M1 structurally: route-only, zero pixel delta, no new renderer
behavior, no new LOD decision, no new material model, no new visibility
model. Only a new boundary.

M1 proved the adapter shape for static props. M2 applies it to mechs.
M1.5 delivered the object-ID substrate (`R32_UINT` MRT at attachment-2,
`lookupAtPixel`, `setSceneDrawBuffers` C1 META-FIX). M1.6 wired the first
gameplay pick consumer (static props). M2-pre extracted the shared
gameplay-pick spine (`tryGameplayPick`, `screenToFboPixel`,
`GameplayPickRequest`/`Context`/`Result`) into `code/gameplay_pick.{h,cpp}`,
paying the M1.6 greybeard debt before any mech-specific work.

M2 does NOT write mech object IDs to attachment-2 (that is M2.5).
M2 does NOT integrate the gameplay-pick spine for mech pickup (that is M2.6).
M2 registers and destroys mechs in RenderWorld; no per-frame sync; no mech
picks; same pixels.

---

## 3. Purpose

### Purpose

`MechRenderAdapter` introduces the adapter module and the two RenderWorld
API additions (`registerMech` / `destroyMech`) needed to track every mech
spawn and destroy through the RenderWorld boundary. After M2 ships:

- Every live `Mech3DAppearance` instance has a `RenderObjectHandle` stored
  on it (accessed via public accessors), set at spawn, retired at destroy.
- `[RENDER_WORLD v1]` mission banner reports mech count separately from
  static-prop count (new `mechs=M` token; see Section 9.3).
- The M2.5 object-ID writer and M2.6 mech-pick consumer have a live handle
  to write against from day one.

### Non-goals (M2 scope only)

- No per-frame transform/animation sync (deferred to a future slice).
- No mech object-ID writes to the `R32_UINT` attachment (M2.5).
- No `tryGameplayPick` extension for mechs (M2.6).
- No new cull dispatch or visibility model.
- No `mc2_object_id_buffer` reads for mech pixels.
- No banner change to `[RENDER_PATH v1]` or `[VISIBILITY v1]` lines.
- No changes to `GpuMechBatcher` or `gpu_cull` dispatch.
- No changes to `mclib/mech3d.cpp` rendering methods.

### Open questions (carry to plan-stage)

- OQ-1: Does `BattleMech::init` ALWAYS construct a concrete
  `Mech3DAppearance` at the two assignment sites (`code/mech.cpp:1304`)?
  The cast `(Mech3DAppearanceTypePtr)mechAppearanceType` at line 1310
  implies yes, but verify at plan-stage: if any code path assigns a
  different `Appearance` subclass to `this->appearance` and then reaches
  the `syncSpawn` call site, `static_cast<Mech3DAppearance*>` is UB.
  Mitigation: if not provably always `Mech3DAppearance*`, use a
  type-discriminator check or `dynamic_cast` in debug. Full cast
  discipline in Section 7.

- OQ-2: The `appearance` field in `GameObj` is typed `AppearancePtr`
  (i.e. `Appearance*`, per `mclib/dappear.h:23`). The concrete type is
  `Mech3DAppearance` (verified by the assignment `appearance = new
  Mech3DAppearance` at `code/mech.cpp:1304`). Confirm that no other
  `BattleMech::init` overload assigns a different concrete type before
  reaching line 1310. Verify at plan-stage by grepping all BattleMech::init
  overloads for `appearance =` assignment.

- OQ-3: Is there a `BattleMech::destroy()` code path that deletes
  `appearance` before reaching line 3726 (e.g. early return, exception,
  or the existing `if (appearance) delete appearance; appearance = NULL;`
  guard at line 1302-1303 in a re-init scenario)? If yes, the `destroyMech`
  call site must also guard against double-destroy. Verify at plan-stage.

  NOTE: OQ-4 (mechTypeId source) is RESOLVED -- see resolved decisions below.

- OQ-5 (MINOR -- gameObjectId source):
  Determine the stable `uint32_t` for `gameObjectId` at the spawn call site.
  Options:
  - `static_cast<uint32_t>(getWatchID())` if `WatchID` is available at
    `BattleMech::init()` time (verify by grepping `BattleMech::init` body
    for WatchID assignment order vs the appearance init sequence).
  - `static_cast<uint32_t>(getObjectNumber())` if available.
  - 0 if no stable integer is derivable before full object init.
    M2 tolerates 0; M2.5 will refine when object-ID writes need correlation.

- OQ-6 (MINOR -- mission name for banner):
  Determine how to pass the mission name string to `RenderWorld::init()` or
  `GameAdapters::Mech::beginMission()` for the `mission=<name>` banner token.
  In M1, the banner used `mission=unknown` (placeholder). Grep
  `code/mission.cpp` for the mission name variable (likely `scenarioName` or
  similar) and pass it at `beginMission` time. Plan-stage resolves the string
  lifetime (const char* is safe if it outlives the mission; else pass by value).

- OQ-7 (MINOR -- CMakeLists update):
  `GameAdapters/CMakeLists.txt` currently lists only `StaticPropRenderAdapter.cpp`.
  Adding `MechRenderAdapter.cpp` to the `add_library(gameadapters STATIC ...)`
  source list is required. The include directories already present in the
  CMakeLists (mclib, GameOS, thirdparty) are sufficient for `mech3d.h`
  inclusion. Verify at plan-stage that no additional include directory is
  needed.

### Resolved decisions (not open at plan-stage)

**RESOLVED -- mechTypeId (was OQ-4):**
`Mech3DAppearance::mechType` is a protected field; no public stable integer
accessor is known to exist in M2 scope. The adapter must NOT access
`mech.mechType` directly (it is protected; the adapter is non-member,
non-friend code). M2 ships:
```cpp
mechTypeId  = 0;              // M2: type identity deferred to M2.5
debugCookie = reinterpret_cast<uintptr_t>(&mech);  // opaque pointer echo for logs
```
Type identity correlation is deferred to M2.5. This is a deliberate design
decision, not a plan-stage open question.

**RESOLVED -- handle namespace:**
All `RenderObjectHandle` indices live in the unified `s_objectRecords` table
(confirmed: single `std::vector<RenderWorld::RenderObjectRecord> s_objectRecords`
in `RenderWorld/RenderWorld.cpp:85`). Mech records in M2 extend the same
unified table with `kind = RenderObjectKind::Mech`. There is no separate
zero-based mech record namespace. See Section 5 and Section 5.4 for details.

---

## 4. New files

### 4.1 `GameAdapters/MechRenderAdapter.h`

Location: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameAdapters/MechRenderAdapter.h`

Discipline:
- `#pragma once`
- Includes: `../RenderCore/Handle.h` only
- Forward-declares: `class Mech3DAppearance;` (Section 12 carve-out: adapter
  headers may forward-declare game-side types)
- No includes of `mech3d.h`, `appear.h`, `RenderWorld.h`, or any game-side
  or engine implementation header in the header file itself
- No `Mech3DAppearanceType*` in the public API

```cpp
// GameAdapters/MechRenderAdapter.h
//
// Slice M2 (route-only): mech spawn/destroy lifecycle adapter.
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md
//
// Include discipline (load-bearing):
//   - Header MAY include RenderCore/Handle.h only (RenderCore is pure types;
//     no GL, no game-side headers).
//   - Header MAY forward-declare class Mech3DAppearance.
//   - Header MUST NOT include mech3d.h, RenderWorld.h, or any game-side
//     or engine implementation header.
//   - The .cpp includes mech3d.h and RenderWorld.h (the only TU that may).
//   - Adapter is TEMPORARY per spec Section 10 deletion criteria.

#pragma once

#include <cstdint>
#include "../RenderCore/Handle.h"

// Forward-decl game-side mech appearance. Spec section 12 carve-out:
// adapter headers may forward-declare game-side types; the .cpp includes
// the real header.
class Mech3DAppearance;

namespace GameAdapters {
namespace Mech {

// Per-mission lifecycle. Wire alongside GpuMechBatcher::instance().onMapLoad()
// and GameAdapters::StaticProp::beginMission() at code/mission.cpp:1695
// (currently that line calls StaticProp::beginMission; Mech::beginMission
// is added adjacent, per the mission.cpp wiring spec in Section 7).
void beginMission();
void endMission();  // safety sweep / force-clear after warning; see Section 8.3

// Spawn hook. Call AFTER appearance->init() and appearance->initFX() succeed
// (code/mech.cpp:1310-1311). Takes a mutable reference so the adapter can
// call mech.setRenderWorldHandle() on the appearance instance.
// gameObjectId is an opaque engine-side cookie (typically the mech's
// GameObject index or WatchID; never dereferenced by RenderWorld).
//
// Returns invalid() on RenderWorld failure. The handle is also stored
// in mech via mech.setRenderWorldHandle(); caller should assert both are
// consistent in debug builds.
RenderCore::RenderObjectHandle syncSpawn(Mech3DAppearance& mech,
                                         uint32_t          gameObjectId);

// Destroy hook. Call BEFORE delete appearance (code/mech.cpp:3726).
// Retires the handle in RenderWorld and calls mech.clearRenderWorldHandle().
// No-op if mech.getRenderWorldHandle() is already invalid().
//
// THIS is the AUTHORITATIVE handle retirement path. endMission() is a
// safety sweep only and must not be relied upon for per-mech cleanup.
void destroyMech(Mech3DAppearance& mech);

} // namespace Mech
} // namespace GameAdapters
```

### 4.2 `GameAdapters/MechRenderAdapter.cpp`

Location: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameAdapters/MechRenderAdapter.cpp`

Discipline:
- This is the ONLY TU that may include both `mclib/mech3d.h` and
  `RenderWorld/RenderWorld.h`.
- Adapter state is quarantined in an anonymous namespace.
- Separate counters from static-prop counters (Section 9).
- The file includes both game-side (`mech3d.h`) and engine-side
  (`RenderWorld.h`) headers -- that is the definition of the adapter's
  job and the reason it exists.

Key include list (at plan-stage, verify each path resolves):
- `MechRenderAdapter.h`
- `../RenderWorld/RenderWorld.h`
- `../mclib/mech3d.h` (the only TU outside mclib/ that may include this)
- `<cstdio>`, `<cstdlib>`, `<cstdint>`

Adapter state (anonymous namespace, this TU only):
```cpp
namespace {
    // Separate from static-prop counters per spec Section 9.
    uint64_t s_mechs_registered   = 0;
    uint64_t s_mechs_alive        = 0;
    uint64_t s_mechs_destroyed    = 0;
    uint64_t s_mech_register_fail = 0;

    bool envFlag(const char* name) {
        const char* v = std::getenv(name);
        return v && v[0] && v[0] != '0';
    }
}
```

`beginMission()` resets all four counters to zero and emits
`[RENDER_WORLD v1] event=mech_begin_mission` when
`MC2_RENDER_WORLD_TRACE=1`.

`endMission()` behavior (definitive -- not deferred to plan-stage):
1. Emits a summary log line with the final counter values (always-on; see
   Section 9.2).
2. If `s_mechs_alive > 0`, emits `[RENDER_WORLD v1] WARN: event=mech_leaked_handles
   count=<N>` (always-on warning).
3. Force-clears ALL mech records in the unified table (marks their entries
   alive=false, bumps generation) after logging the warning, so stale handles
   cannot carry into the next mission.
4. Resets all four counters to zero.

`beginMission()` therefore always starts from clean empty mech state: counters
are zero and any mech slots from the previous mission were cleared by
`endMission()`.

`syncSpawn(mech, gameObjectId)`:
1. Asserts `mech.getRenderWorldHandle().isValid() == false` in debug
   (double-spawn guard; a valid handle before spawn means a prior destroy
   was missed).
2. Builds a `RenderWorld::RenderMechDesc` with:
   ```cpp
   mechTypeId  = 0;              // M2: type identity deferred to M2.5
   debugCookie = reinterpret_cast<uintptr_t>(&mech);  // opaque pointer echo for logs
   ```
   and `gameObjectId` as supplied.
3. Calls `RenderWorld::registerMech(desc)`.
4. On success: calls `mech.setRenderWorldHandle(returned_handle)`,
   increments `s_mechs_registered` and `s_mechs_alive`.
5. On failure (returns invalid()): increments `s_mech_register_fail`;
   emits `[RENDER_WORLD v1] event=mech_register_fail` unconditionally.
6. When `MC2_RENDER_WORLD_TRACE=1`: emits
   `[RENDER_WORLD v1] event=mech_register mech=%p handle.index=%u
   gameObjectId=%u` on success.
7. Returns the handle (also stored on mech via setRenderWorldHandle).

`destroyMech(mech)`:
1. If `mech.getRenderWorldHandle().isValid() == false`: no-op (already retired
   or never registered); returns immediately.
2. Calls `RenderWorld::destroyMech(mech.getRenderWorldHandle())`.
3. Calls `mech.clearRenderWorldHandle()`.
4. Increments `s_mechs_destroyed`; decrements `s_mechs_alive`.
5. When `MC2_RENDER_WORLD_TRACE=1`: emits
   `[RENDER_WORLD v1] event=mech_destroy handle.index=%u`.

---

## 5. RenderWorld API additions

These additions extend `RenderWorld/RenderWorld.h`. They follow the
firewall rule: no `Mech3DAppearance*`, no `Mech3DAppearanceType*`, and
no Mech3D game-side type in the public API.

### 5.1 `RenderObjectKind` enum

Add to `RenderWorld/RenderWorld.h` in the `RenderWorld` namespace, before
`RenderObjectRecord`:

```cpp
// M2: kind tag for the unified handle/record table. Every RenderObjectHandle
// issued by this module has an associated kind stored in the record.
// The kind disambiguates static props from mechs (and future kinds) when
// a caller examines a handle returned by lookupAtPixel or any other API.
//
// Values are stable across releases (never renumber; only append).
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    // Future: Terrain=2, Vfx=3, Overlay=4
};
```

### 5.2 `RenderObjectRecord` extension

`RenderObjectRecord` in `RenderWorld/RenderWorld.h` gains one field:

```cpp
// M2: kind tag. Populated by registerMech (kind=Mech) and upsertStaticProp
// (kind=StaticProp). lookupAtPixel callers MUST check kind before consuming
// kind-specific fields.
RenderObjectKind kind = RenderObjectKind::StaticProp;  // default for M1 legacy slots
```

Add this field to the existing `struct RenderObjectRecord` declaration.
Existing M1/M1.5 static-prop slots default to `StaticProp` via the struct
default-member-initializer; no migration of existing records is needed.

### 5.3 `RenderMechDesc` struct

Add to `RenderWorld/RenderWorld.h` in the `RenderWorld` namespace:

```cpp
// M2: mech spawn descriptor. Engine types only -- no Mech3DAppearance*,
// no Mech3DAppearanceType*. Firewall: spec section 12 + M2 spec section 10.
//
// mechTypeId: 0 in M2 by design (type identity deferred to M2.5).
//   RenderWorld MUST NOT dereference it.
//
// gameObjectId: opaque uint32_t; the engine-side echo of a game-side
//   identifier. Never dereferenced by RenderWorld. Used for future
//   object-ID correlation in M2.5.
//
// debugCookie: opaque uintptr_t; never dereferenced by engine. Carries
//   the raw Mech3DAppearance* echo for log output in MC2_RENDER_WORLD_TRACE
//   builds. RenderWorld stores it in the record but never casts or follows it.
struct RenderMechDesc {
    uint32_t  mechTypeId;    // 0 in M2; real value deferred to M2.5
    uint32_t  gameObjectId;
    uintptr_t debugCookie;
};
```

### 5.4 `registerMech` function

```cpp
// M2: register a mech with RenderWorld. Returns a new RenderObjectHandle
// on success; invalid() on failure (OOM or internal error).
//
// MUST NOT be called upsert-style (no overwrite of an existing handle).
// This is spawn-only: if the caller's handle is already valid, it means
// a prior destroyMech was missed. The adapter asserts on this in debug.
//
// Route-only in M2: no new GPU path. RenderWorld records the handle in
// the unified s_objectRecords table with kind=Mech; the handle is valid
// from this call until destroyMech.
RenderCore::RenderObjectHandle registerMech(RenderMechDesc desc);
```

### 5.5 `destroyMech` function

```cpp
// M2: retire a mech handle. No-op on invalid() input.
//
// AUTHORITATIVE handle retirement path. After this call the handle is
// invalid; any subsequent use of the old handle with lookupAtPixel or
// any future API returns invalid/false.
//
// endMission() force-clears remaining live mech records after logging
// a warning; correctness of per-mech destroy during normal play must
// not depend on endMission() being called.
void destroyMech(RenderCore::RenderObjectHandle h);
```

### 5.6 RenderWorld.cpp implementation notes

**Unified handle table (load-bearing):**
M2 mechs extend the EXISTING unified `s_objectRecords` table
(`std::vector<RenderWorld::RenderObjectRecord> s_objectRecords` at
`RenderWorld/RenderWorld.cpp:85`). There is NO separate `s_mechRecords`
table. Mech record slots are allocated from the same index space as static
props; all `RenderObjectHandle` indices are globally unique across all kinds.
This guarantees that a handle returned by `lookupAtPixel` or any other API
is unambiguous without partitioning.

`registerMech(desc)` in M2:
- Allocates the next available handle index from the unified `s_objectRecords`
  table (the allocator must be extended in M2 to support kind-tagged slots;
  see plan-stage). Generation is set to 1. Populates:
  ```
  rec.kind        = RenderObjectKind::Mech
  rec.generation  = 1
  rec.flags       = kRenderObjectFlagAlive
  rec.gameObjectId = desc.gameObjectId
  rec.debugCookie  = desc.debugCookie   // stored for log output; never cast
  ```
  (The `debugCookie` field is a new addition to `RenderObjectRecord` in M2;
  verify at plan-stage whether to add it or reuse an existing sentinel field.)
- Increments `s_mechs_alive_rw` (engine-side counter for the banner; keep
  separate from the adapter-side counter -- see Section 9 counter discipline).
- When `MC2_RENDER_WORLD_TRACE=1`: emits
  `[RENDER_WORLD v1] event=mech_register handle.index=%u mechTypeId=%u`.
- Returns `invalid()` only on OOM / slot exhaustion.

`destroyMech(h)` in M2:
- Looks up the record in `s_objectRecords` at `h.index()`. If out-of-range
  or kind != Mech, no-op (defensive; should not happen in correct usage).
- Marks the record: `rec.flags &= ~kRenderObjectFlagAlive`; bumps generation.
- Decrements `s_mechs_alive_rw`.
- No GPU work in M2 (the mech batcher path is unchanged).
- When `MC2_RENDER_WORLD_TRACE=1`: emits
  `[RENDER_WORLD v1] event=mech_destroy handle.index=%u`.

`endMission()` force-clear (engine-side implementation):
At mission end, after the adapter calls the engine-side mech record sweep,
any mech slots still marked alive in `s_objectRecords` with `kind=Mech` are
force-retired (alive=false, generation bumped) and `s_mechs_alive_rw`
decremented accordingly. This is triggered by `GameAdapters::Mech::endMission()`
via a new engine-side call `RenderWorld::clearAllMechRecords()` (or equivalent;
plan-stage names the function). The adapter logs the warning before calling.

`frameBannerTick()` extension (see Section 9.3 for full banner format):
- Reads `s_mechs_alive_rw` (registry-side live count) for the `mechs=M` token.
- Reads the existing `legacy::getStaticPropActiveCount()` for `static_props=S`.
- Total `objects=T` is `static_props + mechs`.

---

## 6. Mech3DAppearance changes

### 6.1 Handle field addition

Add to the `protected` section of `class Mech3DAppearance` in
`mclib/mech3d.h` (after the existing fields at approximately line 448,
before the `public:` block at line 449; verify at plan-stage):

```cpp
// M2 RenderWorld handle. Set by GameAdapters::Mech::syncSpawn() via
// setRenderWorldHandle() after appearance->init(). Cleared by
// GameAdapters::Mech::destroyMech() via clearRenderWorldHandle() before
// delete appearance. Default: invalid (never registered or already retired).
//
// IMPORTANT: Mech3DAppearance MUST NOT call the adapter or RenderWorld
// directly. The adapter is the bridge; this field is storage only.
// Firewall: mclib/mech3d.h may NOT include GameAdapters/MechRenderAdapter.h.
// The field type (RenderCore::RenderObjectHandle) is in RenderCore/Handle.h,
// which mclib/mech3d.h IS allowed to include (RenderCore is pure;
// no GL, no game headers -- see Section 10 firewall rule F-5).
#include "../RenderCore/Handle.h"  // add near top of mech3d.h instead if cleaner
RenderCore::RenderObjectHandle mechRenderHandle =
    RenderCore::RenderObjectHandle::invalid();
```

Note: the `#include` directive belongs at the top of `mech3d.h`, not
inline in the class body. The `RenderCore/Handle.h` include is allowed
in `mech3d.h` because `RenderCore` is a pure-types module with no
game-side or GL headers (confirmed by reading `RenderCore/Handle.h`:
its only include is `<cstdint>`).

### 6.2 Public accessors (load-bearing -- C1 fix)

The `mechRenderHandle` field is PROTECTED. The adapter (`GameAdapters::Mech`
free functions) is non-member, non-friend code and cannot access protected
fields directly. Add the following narrow PUBLIC accessors to `class
Mech3DAppearance` in the `public:` section:

```cpp
// Public accessors -- used only by GameAdapters::Mech adapter functions.
// No other caller may use these outside the adapter.
RenderCore::RenderObjectHandle getRenderWorldHandle() const {
    return mechRenderHandle;
}
void setRenderWorldHandle(RenderCore::RenderObjectHandle h) {
    mechRenderHandle = h;
}
void clearRenderWorldHandle() {
    mechRenderHandle = RenderCore::RenderObjectHandle::invalid();
}
```

These are inline definitions in the header (the struct is already large;
adding three one-liner methods adds negligible compile-time cost). The
`mechRenderHandle` field stays protected. Only these three accessors are
public. No friendship is granted to the adapter.

All adapter code that reads or writes `mechRenderHandle` MUST go through
these accessors, never directly.

### 6.3 Defensive reset in init()

Add a defensive reset at the start of `Mech3DAppearance::init()`:
```cpp
// Near the start of Mech3DAppearance::init(), before any shape setup:
mechRenderHandle = RenderCore::RenderObjectHandle::invalid();
```
This guards against the re-init scenario (`code/mech.cpp:1302-1303`
deletes and re-news appearance before init, but if the object is ever
reused without delete, the reset ensures the handle is not stale).
Verify at plan-stage whether `Mech3DAppearance::init()` is called in
any path without a prior `delete appearance` + `new Mech3DAppearance`
(i.e., whether in-place re-init is possible).

### 6.4 Include graph impact

`mclib/mech3d.h` gains one new include: `../RenderCore/Handle.h`.
This is allowed by the firewall because `RenderCore` is the pure-types
module; it has no GL, no game-side, and no mclib headers (verified:
`RenderCore/Handle.h` includes only `<cstdint>`). The firewall forbids
mclib from including `GameAdapters/MechRenderAdapter.h` or `RenderWorld.h`.

Verify at plan-stage: run the firewall script after adding the include
to confirm no false-positive violation fires.

---

## 7. Call site specification

### 7.1 Spawn call site

File: `code/mech.cpp` (root tree, not worktree -- this is the shared source)
Verified line at spec-write time: `1310` (`appearance->init(...)`)
and `1311` (`appearance->initFX()`).

The adapter call goes AFTER `appearance->initFX()` at line 1311 and
BEFORE `appearance->setAlphaValue(alphaValue)` at line 1312:

```cpp
// M2 RenderWorld spawn route. Adapter writes the handle into the
// Mech3DAppearance instance via setRenderWorldHandle(). static_cast is
// safe here only if appearance is provably always Mech3DAppearance* at
// this site (see M2 spec OQ-1 and OQ-2; verify type invariant at
// plan-stage). If not guaranteed by construction, use a type-discriminator
// check or dynamic_cast in debug.
{
    Mech3DAppearance* m3d = static_cast<Mech3DAppearance*>(appearance);
    // gameObjectId derivation: verify at plan-stage. Options:
    //   (a) static_cast<uint32_t>(getWatchID()) if WatchID is stable
    //   (b) static_cast<uint32_t>(getObjectNumber()) if available
    //   (c) 0 if no stable integer is derivable before object is
    //       fully initialized (M2 tolerates 0; M2.5 will refine)
    const uint32_t goid = 0;  // VERIFY at plan-stage
    GameAdapters::Mech::syncSpawn(*m3d, goid);
}
```

Re-init guard: `code/mech.cpp:1302-1303` deletes and re-news `appearance`
before calling `init`. Because the new `Mech3DAppearance` is freshly
constructed (default member initializer sets `mechRenderHandle` to
`invalid()`), the `syncSpawn` double-spawn assert fires only if the OLD
appearance somehow survives into the new one -- which cannot happen after
`delete appearance; appearance = new Mech3DAppearance`. This is safe.

However: confirm that the `delete appearance` at line 1302-1303 is reached
AFTER `destroyMech` would have been called for the previous appearance
(i.e., `BattleMech::destroy()` was called before `BattleMech::init()` is
called again on the same object). If re-init can happen without a prior
destroy, add a pre-init `destroyMech` call at `mech.cpp:1302` before the
existing `delete appearance`.

### 7.2 Destroy call site

File: `code/mech.cpp`
Verified line at spec-write time: `3726` (`delete appearance`)
Context: `BattleMech::destroy()` at line 3722.

The adapter call goes BEFORE `delete appearance` at line 3726:

```cpp
void BattleMech::destroy (void)
{
    if (appearance)
    {
        // M2 RenderWorld destroy route. Must call BEFORE delete appearance
        // so the adapter can access the handle on the instance via
        // getRenderWorldHandle(). static_cast is safe here only if
        // appearance is provably always Mech3DAppearance* at this site
        // (see M2 spec OQ-1; verify at plan-stage). If not guaranteed,
        // use dynamic_cast in debug.
        {
            Mech3DAppearance* m3d = static_cast<Mech3DAppearance*>(appearance);
            GameAdapters::Mech::destroyMech(*m3d);
        }
        delete appearance;
        appearance = NULL;
    }
}
```

### 7.3 Cast discipline (load-bearing)

At both call sites, the plan uses `static_cast<Mech3DAppearance*>(appearance)`.
This is safe IFF and ONLY IF:

1. `BattleMech::init()` (the overload that calls `appearance->init()` at
   line 1310) ALWAYS assigns a `Mech3DAppearance*` to `this->appearance`
   and there is no other path that assigns a different concrete type
   to `appearance` before the call site.
2. `BattleMech::destroy()` is only ever called after the corresponding
   `BattleMech::init()` completed successfully (i.e., `appearance` was
   set to a `Mech3DAppearance*` by that init, not a different subclass).

Verification required at plan-stage:
- Grep all `BattleMech::init` overloads for `appearance =` assignment to
  confirm the concrete type is always `Mech3DAppearance`.
- Grep for any `Mover::init` or `GameObj::init` override that assigns a
  non-`Mech3DAppearance` subclass.
- If the invariant cannot be proven by grep, substitute:
  ```cpp
  Mech3DAppearance* m3d = dynamic_cast<Mech3DAppearance*>(appearance);
  if (!m3d) { /* log + skip */ return; }
  ```
  and gate the `dynamic_cast` behind `#ifdef _DEBUG` if performance is
  a concern (debug only; release omits the check).

### 7.4 mission.cpp wiring

`GameAdapters::Mech::beginMission()` and `GameAdapters::Mech::endMission()`
are added at the worktree-side `code/mission.cpp` adjacent to the existing
`StaticProp::beginMission()` and `StaticProp::endMission()` calls.

Verified existing call sites (worktree `code/mission.cpp`):
- `mission.cpp:1695` -- `GameAdapters::StaticProp::beginMission()` (M1 Task 13)
- `mission.cpp:3282` -- `GameAdapters::StaticProp::endMission()` (M1 Task 13)

M2 adds immediately after each:
- After line 1695: `GameAdapters::Mech::beginMission();`
- After line 3282: `GameAdapters::Mech::endMission();`

Add `#include "../GameAdapters/MechRenderAdapter.h"` near the existing
`StaticPropRenderAdapter.h` include at `mission.cpp:243`.

---

## 8. Lifecycle contract

### 8.1 Spawn

```
BattleMech::init() called
  -> appearance = new Mech3DAppearance  [mech3d.h:455-457 ctor; mechRenderHandle = invalid()]
  -> appearance->init(mechAppearanceType, this)   [mech.cpp:1310]
  -> appearance->initFX()                          [mech.cpp:1311]
  -> GameAdapters::Mech::syncSpawn(*m3d, goid)    [M2 addition, AFTER initFX]
       -> RenderWorld::registerMech(desc)
            -> handle allocated in unified s_objectRecords table (kind=Mech)
            -> record populated, s_mechs_alive_rw++
       -> m3d->setRenderWorldHandle(returned_handle)
       -> s_mechs_registered++, s_mechs_alive++ (adapter-side)
```

After syncSpawn returns, `m3d->getRenderWorldHandle().isValid() == true` iff
registration succeeded. Failure path: handle stays invalid; mech renders
normally via the legacy batcher path; M2 does not regress rendering.

### 8.2 Destroy (authoritative)

```
BattleMech::destroy() called
  -> if (appearance):
       -> GameAdapters::Mech::destroyMech(*m3d)   [M2 addition, BEFORE delete]
            -> if (!m3d->getRenderWorldHandle().isValid()): no-op, return
            -> RenderWorld::destroyMech(m3d->getRenderWorldHandle())
                 -> record in unified s_objectRecords: alive=false, generation bumped
                 -> s_mechs_alive_rw--
            -> m3d->clearRenderWorldHandle()
            -> s_mechs_destroyed++, s_mechs_alive-- (adapter-side)
       -> delete appearance
       -> appearance = NULL
```

### 8.3 endMission (definitive behavior)

`GameAdapters::Mech::endMission()` is the safety net for handles not retired
during the mission. Its behavior is definitive (not deferred to plan-stage):

1. Emits the always-on summary log line with the four counters (see Section 9.2).
2. If `s_mechs_alive > 0`, emits `[RENDER_WORLD v1] WARN: event=mech_leaked_handles
   count=<N>` (always-on warning; alive>0 means some destroyMech calls were
   missed).
3. Force-clears ALL mech records in the unified table: calls
   `RenderWorld::clearAllMechRecords()` (or equivalent engine-side sweep) which
   marks every record with `kind=Mech` and `alive=true` as `alive=false` and
   bumps its generation, then decrements `s_mechs_alive_rw` for each.
4. Resets all four adapter-side counters to zero.

`beginMission()` therefore always starts from clean empty mech state.

Rationale: force-clear after warning prevents stale mech handles from
carrying into the next mission. The generation bump ensures that any pixel
written during the previous mission that somehow reads back after teardown
returns `isValid=false` from `lookupAtPixel`. This is the same discipline
applied to M1.5's per-slot generation tracking, extended to the mission
boundary.

The AUTHORITATIVE teardown path during normal play remains per-object
`destroyMech()`. `endMission()` is the safety net only; correctness of
in-mission handle state must not depend on endMission.

### 8.4 Re-init scenario

If `BattleMech::init()` is called on an object that already has a live
`appearance` (the `delete appearance; appearance = new Mech3DAppearance`
guard at `mech.cpp:1302-1303`), the PREVIOUS appearance is deleted without
`BattleMech::destroy()` being called. This means `destroyMech()` for the
previous appearance might not have fired.

Resolution: insert a `destroyMech` call at `mech.cpp:1302` BEFORE the
existing `if (appearance) delete appearance` block:

```cpp
if (appearance) {
    // M2: retire previous handle before the appearance is replaced.
    {
        Mech3DAppearance* m3d = static_cast<Mech3DAppearance*>(appearance);
        GameAdapters::Mech::destroyMech(*m3d);  // no-op if getRenderWorldHandle().isValid()==false
    }
    delete appearance;
}
appearance = new Mech3DAppearance;
```

This is correct because `destroyMech()` is a no-op when
`getRenderWorldHandle()` returns invalid (first-time init before any prior
spawn). Verify at plan-stage that this pattern is needed by checking whether
any production code path calls `BattleMech::init()` a second time on a live
mech without an intervening `BattleMech::destroy()`.

---

## 9. Instrumentation

### 9.1 Counters

Track four counters in the adapter TU anonymous namespace, separate from
static-prop counters:

```
s_mechs_registered    -- total registerMech() calls that returned a valid handle
s_mechs_alive         -- current live mech count (adapter-side; mirrors s_mechs_alive_rw)
s_mechs_destroyed     -- total destroyMech() calls on a valid handle
s_mech_register_fail  -- total registerMech() failures (returned invalid())
```

The engine-side `RenderWorld.cpp` maintains `s_mechs_alive_rw` separately
(sourced from the registry record table, not the adapter delta) for the
banner `mechs=M` token, mirroring the `m4 fix` from M1 (`getActiveCount()`
sourced from the registry rather than adapter delta). This prevents drift
when teardown paths skip the adapter.

### 9.2 Log format

All log lines use the `[RENDER_WORLD v1]` prefix per `[SUBSYS v1]`
convention.

Per-event lines (TRACE-GATED: require `MC2_RENDER_WORLD_TRACE=1`):
```
[RENDER_WORLD v1] event=mech_begin_mission
[RENDER_WORLD v1] event=mech_register mech=<ptr> handle.index=<N> gameObjectId=<N>
[RENDER_WORLD v1] event=mech_register_fail mech=<ptr>
[RENDER_WORLD v1] event=mech_destroy handle.index=<N>
```

NOTE: `event=mech_register` per-event lines are TRACE-GATED. They do NOT
fire unconditionally. The mech count canary in Gate 3 (Section 11) is
therefore run with `MC2_RENDER_WORLD_TRACE=1` to observe these lines.

Per-mission summary (ALWAYS-ON -- emitted by endMission() unconditionally):
```
[RENDER_WORLD v1] event=mech_end_mission registered=<N> destroyed=<N> alive=<N> fail=<N>
```

Warning on leaked handles (ALWAYS-ON -- emitted by endMission() when alive>0):
```
[RENDER_WORLD v1] WARN: event=mech_leaked_handles count=<N>
```

### 9.3 Banner extension

The existing `[RENDER_WORLD v1]` per-frame/per-600-frame banner in
`RenderWorld::frameBannerTick()` is extended with new tokens while
retaining the existing tokens for backward compatibility.

Current banner (M1 format, confirmed in `RenderWorld/RenderWorld.cpp:495`):
```
[RENDER_WORLD v1] frame=N objects=N visible=0 packets=0 views=1 objectid_buffer=<on|off>
```

New banner format (M2, backward-compatible addition):
```
[RENDER_WORLD v1] frame=N objects=T static_props=S mechs=M visible=0 packets=0 views=1 objectid_buffer=<on|off>
```

Changes from M1 to M2:
- `objects=T` is retained; T is now total live objects (static_props + mechs),
  preserving the same semantic as the old `objects=N` (which tracked only
  static props in M1; now expanded to cover all kinds).
- `static_props=S` is NEW: sourced from `legacy::getStaticPropActiveCount()`.
- `mechs=M` is NEW: sourced from `s_mechs_alive_rw` (engine-side registry
  counter, not adapter delta).
- `visible=0 packets=0 views=1` are RETAINED unchanged.
- `objectid_buffer=off|on` was already added in M1.5; retained.
- `mission=<name>` MAY be added (see OQ-6; plan-stage determines string source).

Banner schema cleanup (removing legacy tokens such as `visible=0 packets=0
views=1`) is a separate future logging slice. M2 only ADDS tokens; it does
NOT remove or rename any existing token. Any tooling grepping for `objects=N`,
`visible=0`, `packets=0`, or `views=1` continues to work unchanged.

---

## 10. Firewall rules

These rules are the load-bearing firewall for M2. All are
grep-expressible by `scripts/check-include-firewall.sh`.

### Allowed

```
F-1  GameAdapters/MechRenderAdapter.cpp includes mclib/mech3d.h
F-2  GameAdapters/MechRenderAdapter.cpp includes RenderWorld/RenderWorld.h
F-3  GameAdapters/MechRenderAdapter.h forward-declares class Mech3DAppearance
F-4  mclib/mech3d.h includes RenderCore/Handle.h (handle field storage)
F-5  code/mission.cpp includes GameAdapters/MechRenderAdapter.h (lifecycle wiring)
F-6  code/mech.cpp includes GameAdapters/MechRenderAdapter.h (call sites)
```

### Forbidden

```
F-FORBID-1  mclib/mech3d.cpp includes GameAdapters/MechRenderAdapter.h
            (appearance must NOT call the adapter; only mech.cpp call sites call it)

F-FORBID-2  RenderWorld/RenderWorld.h includes mclib/mech3d.h or any mech3d type
            (firewall: no game-side type in RenderWorld public API)

F-FORBID-3  RenderWorld/RenderWorld.h forward-declares Mech3DAppearance
            or Mech3DAppearanceType (forward-decl outside adapter is forbidden
            per boundary spec section 12)

F-FORBID-4  RenderWorld/RenderWorld.cpp includes mclib/mech3d.h
            (engine implementation must not see game-side headers)

F-FORBID-5  Any TU in RenderCore/, Visibility/, MeshRenderer/, MaterialSystem/,
            DebugRenderer/, RenderDeviceGL/ includes or forward-declares
            Mech3DAppearance, Mech3DAppearanceType, or any mclib mech type

F-FORBID-6  mclib/mech3d.h or mclib/mech3d.cpp includes RenderWorld/RenderWorld.h
            (appearance must not cross the engine API boundary)

F-FORBID-7  GameAdapters/MechRenderAdapter.h includes mech3d.h or RenderWorld.h
            (adapter HEADER is forward-decl only; real includes are in the .cpp)
```

### Firewall script update (load-bearing)

`scripts/check-include-firewall.sh` must be updated to add
`MechRenderAdapter.cpp` to the explicitly allowed-bridger list (named
specifically, NOT "all GameAdapters/*.cpp" -- the allowlist must be
per-file to prevent future adapter creep). The allowlist entry should be:

```
GameAdapters/MechRenderAdapter.cpp
```

added to `scripts/check-include-firewall.allowlist` alongside the existing
`GameAdapters/StaticPropRenderAdapter.cpp` entry. The allowlist format
(verified at `scripts/check-include-firewall.allowlist`) uses one bare
relative path per line, with `#`-prefixed comment lines; blank lines and
`#` lines are skipped by `allowlisted()` in the script.

The forbidden-symbol check in the script already covers `Mech3DAppearance`
in `FORBIDDEN_SYMBOLS` (line 33 of `scripts/check-include-firewall.sh`
reads: `FORBIDDEN_SYMBOLS="Appearance BldgAppearance ... Mech3DAppearance ..."`).
The allowlist exempts `GameAdapters/MechRenderAdapter.cpp` from that check.
`mclib/mech3d.h` gaining `#include "../RenderCore/Handle.h"` does NOT
trigger a forbidden-symbol violation because `RenderCore` is in the pure
module list, not in `SCOPE_DIRS`. No script edit needed for that include.

---

## 11. Validation gates

### Gate 1: Tier1 5/5 PASS env-OFF (zero pixel delta)

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS, exit 0. Pixel output identical to M2-pre HEAD
`16e3d53`. `[STATIC_PROP_REGISTRY v1]` counts unchanged vs M2-pre baseline.
`[RENDER_WORLD v1]` banner now includes `mechs=M` and `static_props=S`
tokens with M > 0 on missions that spawn mechs (mc2_01, mc2_03, mc2_10,
mc2_17, mc2_24 all have mechs).

### Gate 2: Tier1 5/5 PASS env-ON with MC2_RENDER_WORLD_TRACE=1

Re-run tier1 with the trace env var set. Verify:
- `ring_trace.log` (or stderr capture) contains `event=mech_register` lines
  for each mission.
- `event=mech_end_mission` fires at mission teardown (always-on).
- No `event=mech_register_fail` lines (healthy mission load).
- Banner format matches Section 9.3 (contains `static_props=S mechs=M
  objectid_buffer=off`, and retains `objects=T visible=0 packets=0 views=1`).

### Gate 3: Mech count canary on mc2_03

Run with `MC2_RENDER_WORLD_TRACE=1` (required -- event=mech_register is
TRACE-GATED):

```powershell
# After running tier1 with MC2_RENDER_WORLD_TRACE=1:
Select-String -Path "tests\smoke\artifacts\<latest>\mc2_03.ring_trace.log" `
    -Pattern "event=mech_register " | Measure-Object | Select-Object Count
```

Expected: Count > 0. mc2_03 is a combat mission with multiple mechs;
the count must be non-zero to prove the spawn hook fired.

### Gate 4: Firewall clean

```bash
sh scripts/check-include-firewall.sh
```

Expected: exit 0, "clean" output. Run this after adding the
`MechRenderAdapter.cpp` allowlist entry and before merge.

### Gate 5: handle validity check at destroy

In a debug build (`MC2_RENDER_WORLD_TRACE=1`), verify that no
`event=mech_register` line appears for a mech that has already emitted
`event=mech_destroy` (no double-register after destroy). This is
observable from the `ring_trace.log` handle.index values.

---

## 12. Greybeard ruling

**Ruling: PATCH (justified)**

The `MechRenderAdapter` is a TEMPORARY bridge per boundary spec Section 10
deletion criteria (substitutive-not-additive rule, per
`memory/feedback_offload_must_be_substitutive_not_additive.md`).

Justification for PATCH (not META-FIX):
- The adapter introduces NO new behavior, NO new GPU path, NO new renderer
  feature. It is a pure routing slice.
- The META-FIX for the pickup machinery was paid by M2-pre (`tryGameplayPick`
  extraction), which shipped 2026-05-23 at HEAD `16e3d53`.
- M2 adapter deletion criteria are documented in Section 13. Deletion is
  a future explicit slice with a substitutive proof; M2 alone does not
  retire any legacy code.
- The adapter is analogous to M1's `StaticPropRenderAdapter` (greybeard
  ruling 2026-05-23: PATCH justified), which set the precedent for this
  pattern in this codebase.

META-FIX debts named (deferred):
- M2.5: mech object-ID writes to attachment-2 (each live mech handle writes
  its `Handle.raw()` to the `R32_UINT` attachment, analogous to the
  static-prop C1 META-FIX in M1.5).
- M2.6: mech pickup via the extracted `tryGameplayPick` spine (Shift+click
  mech selection using the M2 handle as the lookup key).

---

## 13. Deletion criteria

The `MechRenderAdapter` is deletable when ALL of the following are true:

1. `class Mech3DAppearance` (or its refactored successor) has been updated
   to call `RenderWorld::registerMech` / `RenderWorld::destroyMech` directly
   without going through the adapter, OR `Mech3DAppearance` has been fully
   retired from the codebase (the gameData type no longer exists in the
   migrated codebase).

2. The `mechRenderHandle` field on `Mech3DAppearance` is either removed
   (if the class is retired) or moved to a direct-RenderWorld call path
   (if the class is refactored).

3. Tier1 5/5 smoke + parity probe confirm zero pixel delta for at least
   one full release cycle without the adapter (the adapter is absent, not
   just disabled).

The adapter is NOT deletable just because `RenderWorld::registerMech` and
`RenderWorld::destroyMech` exist. Removal is a separate explicit slice with
the substitutive proof documented at that time.

Source: boundary spec `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`
Section 10 (adapter deletion criteria).

---

## 14. Open questions (carry to plan-stage)

OQ-1 through OQ-3 and OQ-5 through OQ-7 carry to plan-stage. OQ-4 is
RESOLVED (see Section 3 resolved decisions). Summarized here for the plan
author:

**OQ-1 (CRITICAL -- cast safety):**
Prove by grepping all `BattleMech::init` overloads that `this->appearance`
is always `Mech3DAppearance*` at the two call sites (`mech.cpp:1310` and
`mech.cpp:3726`). If not provable, use `dynamic_cast` in debug. See
Section 7.3.

**OQ-2 (MAJOR -- re-init scenario):**
Determine whether `BattleMech::init()` can be called on an object that
already has a live `appearance` without a prior `BattleMech::destroy()`.
If yes, add the pre-init `destroyMech` call per Section 8.4. Grep for
callers of `BattleMech::init()` that do NOT call `BattleMech::destroy()`
first.

**OQ-3 (MAJOR -- destroy guard):**
Verify `BattleMech::destroy()` at `mech.cpp:3722` is the ONLY site where
`appearance` is deleted. If there are other early-exit paths that delete
`appearance` without calling `BattleMech::destroy()`, add `destroyMech`
calls there too. Grep for `delete appearance` in `code/mech.cpp` (currently
two known sites: `mech.cpp:1302` re-init guard and `mech.cpp:3726` destroy).

**OQ-5 (MINOR -- gameObjectId source):**
Determine the stable `uint32_t` for `gameObjectId` at the spawn call site.
Options:
- `static_cast<uint32_t>(getWatchID())` if `WatchID` is available at
  `BattleMech::init()` time (verify by grepping `BattleMech::init` body
  for WatchID assignment order vs the appearance init sequence).
- `static_cast<uint32_t>(getObjectNumber())` if available.
- 0 if no stable integer is derivable before full object init.
  M2 tolerates 0; M2.5 will refine when object-ID writes need correlation.

**OQ-6 (MINOR -- mission name for banner):**
Determine how to pass the mission name string to `RenderWorld::init()` or
`GameAdapters::Mech::beginMission()` for the `mission=<name>` banner token.
In M1, the banner used `mission=unknown` (placeholder). Grep
`code/mission.cpp` for the mission name variable (likely `scenarioName` or
similar) and pass it at `beginMission` time. Plan-stage resolves the string
lifetime (const char* is safe if it outlives the mission; else pass by value).

**OQ-7 (MINOR -- CMakeLists update):**
`GameAdapters/CMakeLists.txt` currently lists only `StaticPropRenderAdapter.cpp`.
Adding `MechRenderAdapter.cpp` to the `add_library(gameadapters STATIC ...)`
source list is required. The include directories already present in the
CMakeLists (mclib, GameOS, thirdparty) are sufficient for `mech3d.h`
inclusion. Verify at plan-stage that no additional include directory is
needed.

---

## Appendix A. Verified file:line references (grep-confirmed 2026-05-23)

All line numbers below were verified by grep at spec-write time.
Re-grep before writing the plan (line numbers drift; symbols are stable).

```
mclib/mech3d.h:298        class Mech3DAppearance: public ObjectAppearance
mclib/mech3d.h:104        class Mech3DAppearanceType: public AppearanceType
mclib/mech3d.h:304        Mech3DAppearanceTypePtr  mechType;  (protected field)
mclib/mech3d.h:449        public: (first public: after protected block)
mclib/mech3d.h:242        typedef Mech3DAppearanceType *Mech3DAppearanceTypePtr;
mclib/dappear.h:23        typedef Appearance *AppearancePtr;
code/gameobj.h:327        AppearancePtr  appearance;
code/mech.cpp:1302        if (appearance)
code/mech.cpp:1303            delete appearance;
code/mech.cpp:1304        appearance = new Mech3DAppearance;
code/mech.cpp:1310        appearance->init((Mech3DAppearanceTypePtr)mechAppearanceType, this);
code/mech.cpp:1311        appearance->initFX();
code/mech.cpp:3722        void BattleMech::destroy (void)
code/mech.cpp:3726            delete appearance;
code/mech.h:340           class BattleMech : public Mover {
code/mech.h:438           virtual void destroy (void);
RenderCore/Handle.h:1     // RenderCore/Handle.h
GameAdapters/StaticPropRenderAdapter.h:1   // GameAdapters/StaticPropRenderAdapter.h
GameAdapters/CMakeLists.txt:6              add_library(gameadapters STATIC
GameAdapters/CMakeLists.txt:7                  StaticPropRenderAdapter.cpp
scripts/check-include-firewall.sh:22       SCOPE_DIRS="RenderCore RenderWorld ..."
scripts/check-include-firewall.sh:33       FORBIDDEN_SYMBOLS="... Mech3DAppearance ..."
scripts/check-include-firewall.allowlist:9    GameOS/gameos/gos_static_prop_registry.h
scripts/check-include-firewall.allowlist:10   GameOS/gameos/gos_static_prop_registry.cpp
scripts/check-include-firewall.allowlist:15   RenderWorld/legacy/static_prop_backend.h
scripts/check-include-firewall.allowlist:16   RenderWorld/legacy/static_prop_backend.cpp
RenderWorld/RenderWorld.cpp:84-85          s_objectRecordsMutex / s_objectRecords (unified table)
RenderWorld/RenderWorld.cpp:495            frameBannerTick existing format
worktree code/mission.cpp:243      #include "../GameAdapters/StaticPropRenderAdapter.h"
worktree code/mission.cpp:1695     GameAdapters::StaticProp::beginMission();
worktree code/mission.cpp:3282     GameAdapters::StaticProp::endMission();
```

Symbols with "verify at plan-stage" annotation (not yet grep-confirmed):
- `Mech3DAppearance::mechRenderHandle` -- does not yet exist; field to be added
- `Mech3DAppearance::getRenderWorldHandle` -- does not yet exist; accessor to be added
- `Mech3DAppearance::setRenderWorldHandle` -- does not yet exist; accessor to be added
- `Mech3DAppearance::clearRenderWorldHandle` -- does not yet exist; accessor to be added
- `RenderWorld::registerMech` -- does not yet exist; function to be added
- `RenderWorld::destroyMech` -- does not yet exist (name collision check:
  `RenderWorld.h` already has `void destroy(RenderObjectHandle h)` for
  static props; `destroyMech` is a new overload-by-name, not overload-by-signature)
- `RenderWorld::RenderMechDesc` -- does not yet exist; struct to be added
- `RenderWorld::RenderObjectKind` -- does not yet exist; enum to be added
- `RenderWorld::RenderObjectRecord::kind` -- does not yet exist; field to be added
- `GameAdapters::Mech::syncSpawn` -- does not yet exist
- `GameAdapters::Mech::destroyMech` -- does not yet exist
- `GameAdapters::Mech::beginMission` / `endMission` -- do not yet exist
- `scripts/check-include-firewall.allowlist` -- format confirmed (bare relative
  paths, one per line, `#` comment prefix; verified at spec-write time)

---

## Appendix B. Slice deliverables checklist

```
1. mclib/mech3d.h
     - Add #include "../RenderCore/Handle.h" near top
     - Add RenderCore::RenderObjectHandle mechRenderHandle = invalid() in protected section
     - Add public accessors getRenderWorldHandle() / setRenderWorldHandle() /
       clearRenderWorldHandle() in public section (Section 6.2)
     - Add defensive reset in Mech3DAppearance::init() (Section 6.3)

2. GameAdapters/MechRenderAdapter.h
     - New file per Section 4.1

3. GameAdapters/MechRenderAdapter.cpp
     - New file per Section 4.2

4. GameAdapters/CMakeLists.txt
     - Add MechRenderAdapter.cpp to add_library(gameadapters STATIC ...) list

5. RenderWorld/RenderWorld.h
     - Add RenderObjectKind enum (Section 5.1)
     - Add kind field to RenderObjectRecord (Section 5.2)
     - Add RenderMechDesc struct (Section 5.3)
     - Add registerMech() declaration (Section 5.4)
     - Add destroyMech() declaration (Section 5.5)

6. RenderWorld/RenderWorld.cpp
     - Implement registerMech() and destroyMech() using unified s_objectRecords
       table (Section 5.6)
     - Add s_mechs_alive_rw counter
     - Add clearAllMechRecords() (or equivalent) for endMission force-clear
       (Section 8.3)
     - Extend frameBannerTick() with static_props=S and mechs=M tokens while
       retaining objects=T and all existing tokens (Section 9.3)

7. code/mech.cpp
     - Add #include for MechRenderAdapter.h (near top)
     - Add syncSpawn call at line 1311+ (Section 7.1)
     - Add destroyMech call at line 3725 (Section 7.2)
     - Optionally add pre-init destroyMech at line 1302 (Section 8.4;
       confirm necessity at plan-stage per OQ-2)

8. worktree code/mission.cpp
     - Add #include for MechRenderAdapter.h at line 243 area
     - Add GameAdapters::Mech::beginMission() after line 1695
     - Add GameAdapters::Mech::endMission() after line 3282

9. scripts/check-include-firewall.allowlist
     - Add GameAdapters/MechRenderAdapter.cpp entry

10. Tier1 5/5 PASS env-OFF (gate 1)
11. Tier1 5/5 PASS env-ON MC2_RENDER_WORLD_TRACE=1 (gate 2)
12. Mech count canary mc2_03 > 0 with MC2_RENDER_WORLD_TRACE=1 (gate 3)
13. Firewall script clean (gate 4)
14. Adversarial review report (at plan-stage before execution)
```

---

## Appendix C. Cross-spec references

- `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`
  -- parent boundary contract; Section 10 (deletion criteria),
  Section 12 (forbidden deps), Section 13 (M1 format template)

- `docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`
  -- M1 plan; formatting reference for task structure

- `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`
  -- M1.5 object-ID substrate; M2.5 will extend for mechs

- `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md`
  -- M2-pre META-FIX; `tryGameplayPick` spine that M2.6 will consume

- `memory/feedback_offload_must_be_substitutive_not_additive.md`
  -- greybeard rationale; adapter deletion discipline

- `memory/brainstorm_code_grounding_lesson.md`
  -- grep-before-cite discipline; all file:line in this spec verified at
  write time per Appendix A

End of spec.
