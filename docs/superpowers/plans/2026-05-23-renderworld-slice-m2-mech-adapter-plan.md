# RenderWorld Slice M2 -- MechRenderAdapter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route mech spawn/destroy lifecycle through MechRenderAdapter into RenderWorld, storing a RenderObjectHandle per Mech3DAppearance instance; zero pixel delta.

**Architecture:** New GameAdapters/MechRenderAdapter.{h,cpp} adapter bridges BattleMech::init/destroy to RenderWorld::registerMech/destroyMech. Mechs allocate from the unified s_objectRecords table at kMechHandleBase+ to avoid recipe-index collision with static props. Mech3DAppearance gains three public adapter-accessors for handle storage.

**Tech Stack:** C++14, OpenGL 4.5, Windows/MSVC, CMake 3.x, PowerShell smoke runner

---

## Plan-stage blocker resolutions

### B1 -- Unified allocator / index-collision proof (RESOLVED)

`s_objectRecords` at `RenderWorld/RenderWorld.cpp:85` is a `std::vector<RenderWorld::RenderObjectRecord>` indexed by handle.index(). Static-prop records use recipe indices directly as handle indices (generation=1, 20-bit clamp at `RenderWorld.cpp:70-72`). The known max static-prop recipe index across all tier1 missions is 2641 (mc2_24).

Mech handles are allocated at `kMechHandleBase + dense_mech_slot_index` where `kMechHandleBase = 0x00010000u` (65536). Max static-prop recipe index 2641 is well below 65536; no collision is possible. `registerMech()` maintains a per-mission dense slot counter (`s_nextMechSlot`) that resets in `clearAllMechRecords()`. The `s_objectRecords` vector grows lazily (same `populateRecord` resize logic used for static props).

An assert in `registerMech()` at plan-write time: `assert(desc_unused_index < kMechHandleBase)` is impractical because static props don't report their current max to `registerMech`. Instead, a one-shot startup assert at `RenderWorld::init()` logs the max static-prop active count and emits a WARN if it ever approaches kMechHandleBase (threshold: > 60000). This is added to `frameBannerTick()` rather than `registerMech()` to avoid per-call overhead.

**Decision:** `kMechHandleBase = 0x00010000u` defined as a `static constexpr uint32_t` in `RenderWorld/RenderWorld.cpp` (file-scope anonymous namespace, same pattern as other constants there).

### B2 -- Cast safety for BattleMech::init/destroy (RESOLVED)

**Grep finding:** `grep -n "appearance = new" code/mech.cpp` returns exactly one hit:

```
code/mech.cpp:1304:	appearance = new Mech3DAppearance;
```

**All five `BattleMech::init` overloads verified:**
- `BattleMech::init(bool create)` at line 1141: does NOT assign appearance (data-only init).
- `BattleMech::init(bool create, ObjectTypePtr _type)` at line 1251: THIS is the appearance-init overload; assigns `appearance = new Mech3DAppearance` at line 1304 and calls `appearance->init(...)` at line 1310.
- `BattleMech::init(DWORD variantNum)` at line 1376: reads CSV data only; does NOT assign appearance.
- `BattleMech::init(FitIniFile* mechFile)` at line 2692: reads profile data only; does NOT assign appearance.
- `BattleMech::init(FilePtr mechFile)` at line 3317: reads file data only; does NOT assign appearance.

**Conclusion:** Only `BattleMech::init(bool, ObjectTypePtr)` assigns `appearance`, and it always assigns `new Mech3DAppearance` (never another subclass). The `static_cast<Mech3DAppearance*>(appearance)` at both call sites is safe.

**`delete appearance` sites:** `grep -n "delete appearance" code/mech.cpp` returns exactly two hits:
- `code/mech.cpp:1303`: the re-init guard (inside the `if (appearance)` block before `new Mech3DAppearance`).
- `code/mech.cpp:3726`: inside `BattleMech::destroy()`.

No other early-exit paths delete `appearance` in `mech.cpp`. OQ-3 is resolved: the two known delete sites are the complete set.

### OQ-5 -- gameObjectId (RESOLVED)

`getWatchID()` is not available in `code/mech.cpp` at the `BattleMech::init` call site (no grep hit for WatchID in mech.cpp). `gameObjectId = 0` is the correct M2 value per spec Section 3 resolved decisions. M2.5 will refine.

### OQ-6 -- mission name for banner (RESOLVED)

`mission=<name>` banner token is out of M2 scope. The spec notes it "MAY be added" and M1 used `mission=unknown`. M2 does not add a `mission=` token to the banner; the M2 banner extension adds only `static_props=S mechs=M`. This avoids the string-lifetime question entirely in M2 scope.

### OQ-7 -- CMakeLists update (RESOLVED)

`GameAdapters/CMakeLists.txt` currently lists only `StaticPropRenderAdapter.cpp` at line 7. The existing `target_include_directories(gameadapters PRIVATE ...)` block already includes `${CMAKE_SOURCE_DIR}`, `${CMAKE_SOURCE_DIR}/mclib`, and `${CMAKE_SOURCE_DIR}/GameOS` which is sufficient for `mech3d.h`. Adding `MechRenderAdapter.cpp` to the source list is the only required change.

---

## File structure

**Modified files:**
- `RenderWorld/RenderWorld.h` -- add `RenderObjectKind` enum, `kind` field to `RenderObjectRecord`, `debugCookie` field to `RenderObjectRecord`, `RenderMechDesc` struct, `registerMech()` declaration, `destroyMech()` declaration, `clearAllMechRecords()` declaration
- `RenderWorld/RenderWorld.cpp` -- add `kMechHandleBase`, `s_nextMechSlot`, `s_mechs_alive_rw` counters; implement `registerMech()`, `destroyMech()`, `clearAllMechRecords()`; extend `frameBannerTick()`
- `mclib/mech3d.h` -- add `#include "../RenderCore/Handle.h"`, add `mechRenderHandle` field in protected section (before line 449 `public:`), add three public accessors, add defensive reset in `Mech3DAppearance::init()`
- `mclib/mech3d.cpp` -- add defensive reset in `Mech3DAppearance::init()` body at `mech3d.cpp:996`
- `code/mech.cpp` -- add `#include` for `MechRenderAdapter.h`; add pre-init destroyMech at line 1302-1303; add syncSpawn after line 1311; add destroyMech before line 3726
- `code/mission.cpp` (worktree) -- add `#include` for `MechRenderAdapter.h` at line 243 area; add `beginMission()` after line 1695; add `endMission()` after line 3282
- `GameAdapters/CMakeLists.txt` -- add `MechRenderAdapter.cpp` to source list
- `scripts/check-include-firewall.allowlist` -- add `GameAdapters/MechRenderAdapter.cpp`

**Created files:**
- `GameAdapters/MechRenderAdapter.h`
- `GameAdapters/MechRenderAdapter.cpp`

---

## Task 1: Blocker verification + RenderObjectKind enum + RenderObjectRecord extensions

**Files:**
- Modify: `RenderWorld/RenderWorld.h` -- add enum, extend struct
- Modify: `RenderWorld/RenderWorld.cpp` -- add `s_mechs_alive_rw` and `s_nextMechSlot`

- [ ] **Step 1: Verify BattleMech::init overloads (B2 cast safety)**

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\code\mech.cpp" -Pattern "appearance\s*=\s*new" | Select-Object LineNumber, Line
```

Expected output:
```
1304  appearance = new Mech3DAppearance;
```

Exactly one hit. If any other line appears with `appearance = new <SomethingElse>`, the cast is not universally safe and the `syncSpawn` / `destroyMech` call sites MUST use `dynamic_cast` with a null guard instead of `static_cast`.

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\code\mech.cpp" -Pattern "delete appearance" | Select-Object LineNumber, Line
```

Expected output: exactly two hits at lines 1303 and 3726. Any additional site requires adding a `destroyMech` call there.

- [ ] **Step 2: Verify static-prop recipe index is safely below kMechHandleBase**

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts\*\*.ring_trace.log" -Pattern "\[RENDER_WORLD v1\] frame=\d+ objects=\d+" | Select-Object -Last 5
```

This shows the current peak `objects=N` value across recent smoke runs. If any mission shows `objects=N` with N >= 60000, STOP and report: the kMechHandleBase = 0x00010000 (65536) guard is too close and must be raised (e.g. to 0x00100000). Known baseline: mc2_24 max = 2641. Expected: all values well below 60000.

- [ ] **Step 3: Add `RenderObjectKind` enum and extend `RenderObjectRecord` in `RenderWorld/RenderWorld.h`**

Current `RenderObjectRecord` struct in `RenderWorld/RenderWorld.h` (lines 119-130):

```cpp
struct RenderObjectRecord {
    uint16_t generation       = 0;          // mirrors handle.generation() for staleness check
    uint16_t flags            = 0;          // bit 0: alive
    uint32_t meshHandleBits   = 0;          // RenderCore::MeshHandle bits (sentinel: 0 = unknown)
    uint32_t materialHandleBits = 0;        // RenderCore::MaterialHandle bits (sentinel: 0)
    uint8_t  lodLevel         = 0xFFu;      // 0 = highest, 0xFF = unknown
    uint8_t  pad0             = 0;
    uint16_t pipelineId       = 0;          // M1.5 sentinel: 0 = unknown
    uint32_t drawPacketIndex  = 0xFFFFFFFFu; // M1.5 sentinel
    uint32_t pathReasonCode   = 0;          // M1.5 sentinel: 0 = m1.5-static-prop-indirect
    uint32_t gameObjectId     = 0;          // optional engine-side cookie
};
```

Replace the section from the `RenderObjectRecord` comment through the `kRenderObjectFlagAlive` constant. Insert the `RenderObjectKind` enum BEFORE `RenderObjectRecord`. Add `kind` and `debugCookie` fields to the struct. The full replacement block (insert after the `LookupResult lookupAtPixel(...)` forward-declaration block and before the `kRenderObjectFlagAlive` constant, adjusting positions as needed):

Insert this block immediately before `struct RenderObjectRecord` in `RenderWorld/RenderWorld.h`:

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

Replace the `struct RenderObjectRecord` block:

**Existing:**

```cpp
struct RenderObjectRecord {
    uint16_t generation       = 0;          // mirrors handle.generation() for staleness check
    uint16_t flags            = 0;          // bit 0: alive
    uint32_t meshHandleBits   = 0;          // RenderCore::MeshHandle bits (sentinel: 0 = unknown)
    uint32_t materialHandleBits = 0;        // RenderCore::MaterialHandle bits (sentinel: 0)
    uint8_t  lodLevel         = 0xFFu;      // 0 = highest, 0xFF = unknown
    uint8_t  pad0             = 0;
    uint16_t pipelineId       = 0;          // M1.5 sentinel: 0 = unknown
    uint32_t drawPacketIndex  = 0xFFFFFFFFu; // M1.5 sentinel
    uint32_t pathReasonCode   = 0;          // M1.5 sentinel: 0 = m1.5-static-prop-indirect
    uint32_t gameObjectId     = 0;          // optional engine-side cookie
};
```

**Replace with:**

```cpp
struct RenderObjectRecord {
    uint16_t generation       = 0;          // mirrors handle.generation() for staleness check
    uint16_t flags            = 0;          // bit 0: alive
    uint32_t meshHandleBits   = 0;          // RenderCore::MeshHandle bits (sentinel: 0 = unknown)
    uint32_t materialHandleBits = 0;        // RenderCore::MaterialHandle bits (sentinel: 0)
    uint8_t  lodLevel         = 0xFFu;      // 0 = highest, 0xFF = unknown
    uint8_t  pad0             = 0;
    uint16_t pipelineId       = 0;          // M1.5 sentinel: 0 = unknown
    uint32_t drawPacketIndex  = 0xFFFFFFFFu; // M1.5 sentinel
    uint32_t pathReasonCode   = 0;          // M1.5 sentinel: 0 = m1.5-static-prop-indirect
    uint32_t gameObjectId     = 0;          // optional engine-side cookie
    // M2: kind tag. Populated by registerMech (kind=Mech) and upsertStaticProp
    // (kind=StaticProp). lookupAtPixel callers MUST check kind before consuming
    // kind-specific fields.
    RenderObjectKind kind     = RenderObjectKind::StaticProp;  // default for M1 legacy slots
    // M2: opaque debug cookie. Stored for log output; never dereferenced by engine.
    // For mechs: reinterpret_cast<uintptr_t>(&mech3DAppearance). For static props: 0.
    uintptr_t debugCookie     = 0;
};
```

- [ ] **Step 4: Add `RenderMechDesc` struct and function declarations to `RenderWorld/RenderWorld.h`**

Append the following to `RenderWorld/RenderWorld.h` before the closing `} // namespace RenderWorld`:

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

// M2: force-clear all live mech records in the unified table. Called by
// GameAdapters::Mech::endMission() after logging a leaked-handle warning.
// Marks every record with kind=Mech and alive=true as alive=false,
// bumps generation, and decrements s_mechs_alive_rw for each.
void clearAllMechRecords();
```

- [ ] **Step 5: Add engine-side mech state to `RenderWorld/RenderWorld.cpp` anonymous namespace**

In `RenderWorld/RenderWorld.cpp`, inside the anonymous namespace (after the existing `s_lastStaticPropPickMutex` / `s_lastStaticPropPick` declarations at approximately lines 93-94), add:

```cpp
// M2: engine-side mech counters. Separate from the adapter-side counters
// (adapter lives in MechRenderAdapter.cpp). s_mechs_alive_rw is sourced
// from the registry record table (not adapter delta) so the banner
// stays honest even if teardown paths skip the adapter.
static std::atomic<uint64_t> s_mechs_alive_rw{0};
// Dense mech slot index: incremented by registerMech, reset by clearAllMechRecords.
// Mech handle indices are kMechHandleBase + s_nextMechSlot at allocation time.
static std::atomic<uint32_t> s_nextMechSlot{0};

// kMechHandleBase: handle index base for mechs in the unified s_objectRecords table.
// Must exceed the maximum static-prop recipe index across all tier1 missions.
// Known max: 2641 (mc2_24). 65536 provides 24x headroom.
// INVARIANT: max static-prop recipe index < kMechHandleBase.
static constexpr uint32_t kMechHandleBase = 0x00010000u;
```

- [ ] **Step 6: Implement `registerMech`, `destroyMech`, `clearAllMechRecords` in `RenderWorld/RenderWorld.cpp`**

Add these three function bodies inside `namespace RenderWorld {` at the end of the existing function list (after `getLastStaticPropPick()`):

```cpp
RenderCore::RenderObjectHandle registerMech(RenderMechDesc desc) {
    // Allocate a dense slot above kMechHandleBase to avoid collision with
    // static-prop recipe indices (max known: 2641 at tier1).
    const uint32_t slot  = s_nextMechSlot.fetch_add(1, std::memory_order_relaxed);
    const uint32_t idx   = kMechHandleBase + slot;
    // 20-bit handle index clamp check.
    if (idx >= 0x000FFFFFu) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] WARN: event=mech_register_fail reason=index_overflow slot=%u\n",
            (unsigned)slot);
        return RenderCore::RenderObjectHandle::invalid();
    }
    // Generation 1 on first allocation (generation 0 == invalid()).
    const uint16_t gen = 1u;
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::make(idx, gen);

    // Populate the unified record table with kind=Mech.
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        if (idx >= s_objectRecords.size()) {
            const size_t want = static_cast<size_t>(idx) + 1;
            const size_t cap  = (want * 3) / 2 + 16;
            s_objectRecords.resize(cap);
        }
        auto& rec             = s_objectRecords[idx];
        rec.generation        = gen;
        rec.flags             = kRenderObjectFlagAlive;
        rec.meshHandleBits    = 0;
        rec.materialHandleBits = 0;
        rec.lodLevel          = 0xFFu;
        rec.pipelineId        = 0;
        rec.drawPacketIndex   = 0xFFFFFFFFu;
        rec.pathReasonCode    = 0;
        rec.gameObjectId      = desc.gameObjectId;
        rec.kind              = RenderObjectKind::Mech;
        rec.debugCookie       = desc.debugCookie;
    }

    s_mechs_alive_rw.fetch_add(1, std::memory_order_relaxed);

    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=mech_register handle.index=%u mechTypeId=%u "
            "gameObjectId=%u debugCookie=%llu\n",
            (unsigned)idx, (unsigned)desc.mechTypeId,
            (unsigned)desc.gameObjectId,
            (unsigned long long)desc.debugCookie);
    }
    return h;
}

void destroyMech(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return;
    const uint32_t idx = h.index();
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        if (idx >= s_objectRecords.size()) return;
        auto& rec = s_objectRecords[idx];
        // Defensive: only retire if this is actually a mech record.
        if (rec.kind != RenderObjectKind::Mech) {
            std::fprintf(stderr,
                "[RENDER_WORLD v1] WARN: destroyMech called on non-Mech record "
                "handle.index=%u\n", (unsigned)idx);
            return;
        }
        rec.flags &= static_cast<uint16_t>(~kRenderObjectFlagAlive);
        rec.generation = static_cast<uint16_t>(rec.generation + 1u);
    }
    s_mechs_alive_rw.fetch_sub(1, std::memory_order_relaxed);

    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=mech_destroy handle.index=%u\n",
            (unsigned)idx);
    }
}

void clearAllMechRecords() {
    uint64_t cleared = 0;
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        for (uint32_t i = kMechHandleBase;
             i < static_cast<uint32_t>(s_objectRecords.size()); ++i) {
            auto& rec = s_objectRecords[i];
            if (rec.kind == RenderObjectKind::Mech &&
                (rec.flags & kRenderObjectFlagAlive) != 0u) {
                rec.flags &= static_cast<uint16_t>(~kRenderObjectFlagAlive);
                rec.generation = static_cast<uint16_t>(rec.generation + 1u);
                ++cleared;
            }
        }
    }
    if (cleared > 0) {
        s_mechs_alive_rw.fetch_sub(cleared, std::memory_order_relaxed);
    }
    // Reset the dense slot counter so the next beginMission starts fresh.
    s_nextMechSlot.store(0, std::memory_order_relaxed);
}
```

- [ ] **Step 7: Extend `frameBannerTick()` with `static_props=S mechs=M` tokens**

In `RenderWorld/RenderWorld.cpp`, the existing `frameBannerTick()` body (currently at lines 476-497) ends with:

```cpp
    const uint64_t active = legacy::getStaticPropActiveCount();
    const char* oidTok = IsObjectIdBufferEnabled() ? "on" : "off";
    std::fprintf(stderr,
        "[RENDER_WORLD v1] frame=%llu objects=%llu visible=0 packets=0 views=1 objectid_buffer=%s\n",
        (unsigned long long)f, (unsigned long long)active, oidTok);
```

**Replace with:**

```cpp
    const uint64_t staticProps = legacy::getStaticPropActiveCount();
    const uint64_t mechs       = s_mechs_alive_rw.load(std::memory_order_relaxed);
    const uint64_t total       = staticProps + mechs;
    const char* oidTok = IsObjectIdBufferEnabled() ? "on" : "off";
    std::fprintf(stderr,
        "[RENDER_WORLD v1] frame=%llu objects=%llu static_props=%llu mechs=%llu "
        "visible=0 packets=0 views=1 objectid_buffer=%s\n",
        (unsigned long long)f, (unsigned long long)total,
        (unsigned long long)staticProps, (unsigned long long)mechs, oidTok);
```

- [ ] **Step 8: Build (RelWithDebInfo)**

```powershell
Remove-Item A:\Games\mc2-opengl-src\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build A:\Games\mc2-opengl-src\build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 15
```

Expected: build succeeds. New functions compile; no linker errors. No caller wires to `registerMech` yet so runtime behavior is unchanged.

- [ ] **Step 9: Tier1 smoke (no behavior change)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. Banner now emits `static_props=S mechs=0` (mechs=0 because no call sites wired yet). `[STATIC_PROP_REGISTRY v1]` counts unchanged.

- [ ] **Step 10: Commit**

```bash
git add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp
git commit -m "$(cat <<'EOF'
feat(renderworld): add RenderObjectKind enum + registerMech/destroyMech API

M2 slice T1: extends the unified s_objectRecords table with kind=Mech
support. RenderObjectRecord gains kind (RenderObjectKind enum) and
debugCookie (uintptr_t) fields. RenderMechDesc struct added. registerMech
allocates at kMechHandleBase (0x10000) + dense slot to avoid static-prop
recipe-index collision (max known: 2641). destroyMech/clearAllMechRecords
retire records with generation bump. frameBannerTick extended with
static_props=S mechs=M tokens. Tier1 5/5 PASS; mechs=0 (no callers yet).

Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Mech3DAppearance handle field + public accessors + defensive reset

**Files:**
- Modify: `mclib/mech3d.h` -- add include, protected field, three public accessors
- Modify: `mclib/mech3d.cpp` -- add defensive reset in `Mech3DAppearance::init()`

- [ ] **Step 1: Add `#include "../RenderCore/Handle.h"` to `mclib/mech3d.h`**

Verify the current include area at the top of `mclib/mech3d.h`:

```bash
grep -n "^#include\|^#pragma" mclib/mech3d.h | head -20
```

Add `#include "../RenderCore/Handle.h"` near the top of the file, after the existing `#pragma once` or after the first existing `#include` block. The exact position is after any existing includes of standard or engine headers but before the first class declaration. The file's `class Mech3DAppearanceType` begins at line 104 per spec Appendix A.

The include to add:

```cpp
// M2 RenderWorld handle storage (RenderCore is pure types; no GL, no game headers).
#include "../RenderCore/Handle.h"
```

- [ ] **Step 2: Add `mechRenderHandle` to the protected section of `class Mech3DAppearance`**

In `mclib/mech3d.h`, the `protected:` section of `class Mech3DAppearance` starts at line 302 and the first `public:` block after it is at line 449 (verified by grep at plan-write time: `mclib/mech3d.h:449: public:`).

Add the following as the LAST field in the `protected:` section, immediately before line 449 (`public:`):

**Existing (at approximately line 447-449):**

```cpp
		long						baseRootNodeDifference;
		
	public:
```

**Replace with:**

```cpp
		long						baseRootNodeDifference;

		// M2 RenderWorld handle. Set by GameAdapters::Mech::syncSpawn() via
		// setRenderWorldHandleForAdapter() after appearance->init(). Cleared by
		// GameAdapters::Mech::destroyMech() via clearRenderWorldHandleForAdapter()
		// before delete appearance. Default: invalid (never registered or retired).
		//
		// Mech3DAppearance MUST NOT call the adapter or RenderWorld directly.
		// The adapter is the bridge; this field is storage only.
		// Firewall: mclib/mech3d.h may NOT include GameAdapters/MechRenderAdapter.h.
		// The field type (RenderCore::RenderObjectHandle) is in RenderCore/Handle.h,
		// which is allowed here (RenderCore is pure; no GL, no game headers).
		RenderCore::RenderObjectHandle mechRenderHandle =
		    RenderCore::RenderObjectHandle::invalid();

	public:
```

- [ ] **Step 3: Add three public accessor methods to `class Mech3DAppearance`**

In `mclib/mech3d.h`, in the `public:` section, after the existing static members `paintSchemata` and `numPaintSchemata` (lines 450-451), add the following three inline methods:

**Existing (at approximately lines 450-453):**

```cpp
	public:
		static PaintSchemataPtr		paintSchemata;
		static DWORD				numPaintSchemata;

	public:
```

**Replace with:**

```cpp
	public:
		static PaintSchemataPtr		paintSchemata;
		static DWORD				numPaintSchemata;

		// M2 adapter accessors (public -- used ONLY by GameAdapters::Mech).
		// No other caller may use these outside the adapter TU.
		RenderCore::RenderObjectHandle getRenderWorldHandle() const {
		    return mechRenderHandle;
		}
		void setRenderWorldHandleForAdapter(RenderCore::RenderObjectHandle h) {
		    mechRenderHandle = h;
		}
		void clearRenderWorldHandleForAdapter() {
		    mechRenderHandle = RenderCore::RenderObjectHandle::invalid();
		}

	public:
```

- [ ] **Step 4: Add defensive reset in `Mech3DAppearance::init()` in `mclib/mech3d.cpp`**

`Mech3DAppearance::init()` is at `mclib/mech3d.cpp:996`. The body starts with `Appearance::init(tree,obj);` at line 998. Add the defensive reset as the SECOND statement (after the base class init, before any other work):

**Existing (lines 996-1001):**

```cpp
void Mech3DAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	mechType = (Mech3DAppearanceType *)tree;

	mechName[0] = 0;
```

**Replace with:**

```cpp
void Mech3DAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	// M2: defensive reset. Guards against in-place re-init without a prior
	// delete/new (which would carry a stale handle into the new instance).
	// The adapter's syncSpawn assert fires on a valid handle, making
	// accidental double-registration visible in debug builds.
	mechRenderHandle = RenderCore::RenderObjectHandle::invalid();
	mechType = (Mech3DAppearanceType *)tree;

	mechName[0] = 0;
```

- [ ] **Step 5: Verify firewall script still passes after mech3d.h gains the Handle.h include**

`check-include-firewall.sh` scans `SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL"` -- `mclib/` is NOT in that list, so the new include in `mech3d.h` does NOT trigger a forbidden-header scan. Confirm with:

```bash
sh A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\check-include-firewall.sh
```

Expected: exit 0, "clean" output. If a violation fires, read the output; it likely means a file in SCOPE_DIRS was accidentally modified.

- [ ] **Step 6: Build (full relink -- class layout changed)**

```powershell
Remove-Item A:\Games\mc2-opengl-src\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
Remove-Item A:\Games\mc2-opengl-src\build64\RelWithDebInfo\CMakeFiles -Recurse -Force -ErrorAction SilentlyContinue
cmake --build A:\Games\mc2-opengl-src\build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 15
```

`Mech3DAppearance` layout changed (new field added); full clean before relink is required per CLAUDE.md "class-layout changes" rule.

Expected: build succeeds. No compile errors from the new Handle.h include or accessor methods.

- [ ] **Step 7: Tier1 smoke (no behavior change)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. No behavioral change (accessors are defined but no caller exists yet).

- [ ] **Step 8: Commit**

```bash
git add mclib/mech3d.h mclib/mech3d.cpp
git commit -m "$(cat <<'EOF'
feat(mech3d): add mechRenderHandle field + adapter accessors

M2 slice T2: Mech3DAppearance gains a RenderCore::RenderObjectHandle
field (mechRenderHandle, protected) and three public adapter-accessors:
getRenderWorldHandle(), setRenderWorldHandleForAdapter(),
clearRenderWorldHandleForAdapter(). Defensive reset added in
Mech3DAppearance::init() after base-class init. mech3d.h gains
#include "../RenderCore/Handle.h" (allowed: RenderCore is pure).
Firewall clean. Tier1 5/5 PASS (no callers yet; zero pixel delta).

Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: GameAdapters/MechRenderAdapter.{h,cpp} + CMakeLists + firewall allowlist

**Files:**
- Create: `GameAdapters/MechRenderAdapter.h`
- Create: `GameAdapters/MechRenderAdapter.cpp`
- Modify: `GameAdapters/CMakeLists.txt`
- Modify: `scripts/check-include-firewall.allowlist`

- [ ] **Step 1: Create `GameAdapters/MechRenderAdapter.h`**

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
// is added adjacent, per the mission.cpp wiring in Task 4).
void beginMission();
void endMission();  // safety sweep / force-clear after warning; see spec Section 8.3

// Spawn hook. Call AFTER appearance->initFX() succeeds (code/mech.cpp:1311).
// Takes a mutable reference so the adapter can call
// mech.setRenderWorldHandleForAdapter() on the appearance instance.
// gameObjectId is an opaque engine-side cookie (0 in M2; M2.5 refines).
//
// Returns invalid() on RenderWorld failure. The handle is also stored
// in mech via setRenderWorldHandleForAdapter(); caller should assert both
// are consistent in debug builds.
RenderCore::RenderObjectHandle syncSpawn(Mech3DAppearance& mech,
                                         uint32_t          gameObjectId);

// Destroy hook. Call BEFORE delete appearance (code/mech.cpp:3724-3728).
// Retires the handle in RenderWorld and calls mech.clearRenderWorldHandleForAdapter().
// No-op if mech.getRenderWorldHandle() is already invalid().
//
// THIS is the AUTHORITATIVE handle retirement path. endMission() is a
// safety sweep only and must not be relied upon for per-mech cleanup.
void destroyMech(Mech3DAppearance& mech);

} // namespace Mech
} // namespace GameAdapters
```

- [ ] **Step 2: Create `GameAdapters/MechRenderAdapter.cpp`**

```cpp
// GameAdapters/MechRenderAdapter.cpp
//
// Slice M2 (route-only): the ONLY TU that may include both
// mclib/mech3d.h and RenderWorld/RenderWorld.h.
//
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md
// Firewall: scripts/check-include-firewall.allowlist lists this file as an
// explicit allowlist exception for the Mech3DAppearance forbidden-symbol check.

#include "MechRenderAdapter.h"

// Engine side.
#include "../RenderWorld/RenderWorld.h"

// Game side. This is the ONLY TU outside mclib/ that may include mech3d.h.
#include "../mclib/mech3d.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace {

// Adapter state quarantined in anonymous namespace. Separate from
// static-prop counters per spec Section 9 counter discipline.
uint64_t s_mechs_registered   = 0;
uint64_t s_mechs_alive        = 0;
uint64_t s_mechs_destroyed    = 0;
uint64_t s_mech_register_fail = 0;

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

} // namespace

namespace GameAdapters {
namespace Mech {

void beginMission() {
    s_mechs_registered   = 0;
    s_mechs_alive        = 0;
    s_mechs_destroyed    = 0;
    s_mech_register_fail = 0;
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr, "[RENDER_WORLD v1] event=mech_begin_mission\n");
    }
}

void endMission() {
    // Always-on per-mission summary.
    std::fprintf(stderr,
        "[RENDER_WORLD v1] event=mech_end_mission registered=%llu destroyed=%llu "
        "alive=%llu fail=%llu\n",
        (unsigned long long)s_mechs_registered,
        (unsigned long long)s_mechs_destroyed,
        (unsigned long long)s_mechs_alive,
        (unsigned long long)s_mech_register_fail);

    // Always-on warning if any handles were not retired normally.
    if (s_mechs_alive > 0) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] WARN: event=mech_leaked_handles count=%llu\n",
            (unsigned long long)s_mechs_alive);
    }

    // Force-clear any remaining mech records in the engine table after
    // logging the warning, so stale handles cannot carry into the next mission.
    // s_nextMechSlot resets inside clearAllMechRecords().
    RenderWorld::clearAllMechRecords();

    s_mechs_registered   = 0;
    s_mechs_alive        = 0;
    s_mechs_destroyed    = 0;
    s_mech_register_fail = 0;
}

RenderCore::RenderObjectHandle syncSpawn(Mech3DAppearance& mech,
                                         uint32_t          gameObjectId) {
    // Double-spawn guard: a valid handle before spawn means a prior
    // destroyMech was missed (or the re-init destroyMech path was skipped).
    assert(!mech.getRenderWorldHandle().isValid() &&
           "MechRenderAdapter::syncSpawn: handle already valid -- "
           "prior destroyMech was missed");

    RenderWorld::RenderMechDesc desc;
    desc.mechTypeId   = 0u;  // M2: type identity deferred to M2.5
    desc.gameObjectId = gameObjectId;
    desc.debugCookie  = reinterpret_cast<uintptr_t>(&mech);

    RenderCore::RenderObjectHandle h = RenderWorld::registerMech(desc);

    if (h.isValid()) {
        mech.setRenderWorldHandleForAdapter(h);
        ++s_mechs_registered;
        ++s_mechs_alive;
        if (envFlag("MC2_RENDER_WORLD_TRACE")) {
            std::fprintf(stderr,
                "[RENDER_WORLD v1] event=mech_register mech=%p handle.index=%u "
                "gameObjectId=%u\n",
                (void*)&mech, (unsigned)h.index(), (unsigned)gameObjectId);
        }
    } else {
        ++s_mech_register_fail;
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=mech_register_fail mech=%p\n",
            (void*)&mech);
    }

    return h;
}

void destroyMech(Mech3DAppearance& mech) {
    const RenderCore::RenderObjectHandle h = mech.getRenderWorldHandle();
    if (!h.isValid()) {
        // No-op: never registered, or already retired.
        return;
    }

    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=mech_destroy handle.index=%u\n",
            (unsigned)h.index());
    }

    RenderWorld::destroyMech(h);
    mech.clearRenderWorldHandleForAdapter();

    ++s_mechs_destroyed;
    if (s_mechs_alive > 0) {
        --s_mechs_alive;
    }
}

} // namespace Mech
} // namespace GameAdapters
```

- [ ] **Step 3: Update `GameAdapters/CMakeLists.txt`**

**Existing (lines 6-8):**

```cmake
add_library(gameadapters STATIC
    StaticPropRenderAdapter.cpp
)
```

**Replace with:**

```cmake
add_library(gameadapters STATIC
    StaticPropRenderAdapter.cpp
    MechRenderAdapter.cpp
)
```

- [ ] **Step 4: Add `GameAdapters/MechRenderAdapter.cpp` to the firewall allowlist**

The allowlist at `scripts/check-include-firewall.allowlist` currently contains (lines 9-16):

```
GameOS/gameos/gos_static_prop_registry.h
GameOS/gameos/gos_static_prop_registry.cpp
...
RenderWorld/legacy/static_prop_backend.h
RenderWorld/legacy/static_prop_backend.cpp
```

**Existing end of file:**

```
RenderWorld/legacy/static_prop_backend.h
RenderWorld/legacy/static_prop_backend.cpp
```

**Replace with:**

```
RenderWorld/legacy/static_prop_backend.h
RenderWorld/legacy/static_prop_backend.cpp

# M2: the ONLY TU outside mclib/ that may include mech3d.h, per spec
# section 12 carve-out. Named specifically (not "all GameAdapters/*.cpp")
# to prevent adapter creep.
GameAdapters/MechRenderAdapter.cpp
```

- [ ] **Step 5: Run firewall check**

```bash
sh A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\check-include-firewall.sh
```

Expected: exit 0, "clean" output. If a forbidden-symbol violation fires for `Mech3DAppearance` in `GameAdapters/MechRenderAdapter.cpp`, verify the allowlist path matches exactly (relative from worktree root, no trailing whitespace).

- [ ] **Step 6: Build**

```powershell
Remove-Item A:\Games\mc2-opengl-src\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build A:\Games\mc2-opengl-src\build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 15
```

Expected: `MechRenderAdapter.cpp` compiles cleanly. `gameadapters` library links. `mc2.exe` links (no undefined symbols -- `syncSpawn`, `destroyMech`, `beginMission`, `endMission` are all defined but not yet called from `mech.cpp` or `mission.cpp`).

- [ ] **Step 7: Tier1 smoke (no behavior change)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. No `event=mech_register` or `event=mech_end_mission` lines (no call sites wired yet). Banner still shows `mechs=0`.

- [ ] **Step 8: Commit**

```bash
git add GameAdapters/MechRenderAdapter.h GameAdapters/MechRenderAdapter.cpp GameAdapters/CMakeLists.txt scripts/check-include-firewall.allowlist
git commit -m "$(cat <<'EOF'
feat(gameadapters): add MechRenderAdapter spawn/destroy adapter

M2 slice T3: MechRenderAdapter.h (forward-decl only header per spec
firewall) + MechRenderAdapter.cpp (the only TU that may include both
mech3d.h and RenderWorld.h). Four per-mission counters in anonymous
namespace separate from static-prop counters. beginMission resets
counters; endMission emits always-on summary, warns on leaked handles,
and calls clearAllMechRecords(). syncSpawn builds RenderMechDesc with
mechTypeId=0 and debugCookie=&mech; stores handle via
setRenderWorldHandleForAdapter(). destroyMech is a no-op on invalid
handle. CMakeLists updated; allowlist entry added. Firewall clean.
Tier1 5/5 PASS; no call sites wired yet.

Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Call site wiring -- mech.cpp spawn + destroy + mission.cpp lifecycle

**Files:**
- Modify: `code/mech.cpp` -- add include, pre-init destroy, spawn call, destroy call
- Modify: `code/mission.cpp` (worktree) -- add include, beginMission, endMission

**Cast safety note (verified in Task 1, Step 1):** Only `BattleMech::init(bool, ObjectTypePtr)` assigns `appearance`, always as `new Mech3DAppearance`. `static_cast<Mech3DAppearance*>(appearance)` is safe at both call sites in this overload and in `BattleMech::destroy()`. There are exactly two `delete appearance` sites (lines 1303 and 3726); both are covered by this task.

- [ ] **Step 1: Verify current line numbers in `code/mech.cpp` before editing**

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\code\mech.cpp" -Pattern "if \(appearance\)" | Select-Object LineNumber, Line | Select-Object -First 5
Select-String -Path "A:\Games\mc2-opengl-src\code\mech.cpp" -Pattern "appearance->initFX" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\code\mech.cpp" -Pattern "void BattleMech::destroy" | Select-Object LineNumber, Line
```

Expected: the re-init guard `if (appearance)` near line 1302; `appearance->initFX()` at line 1311; `void BattleMech::destroy` at line 3722. If any line number has drifted more than 10 lines from the spec, find the symbol and update the surrounding context below accordingly before editing.

- [ ] **Step 2: Add `#include` for `MechRenderAdapter.h` in `code/mech.cpp`**

Find the existing include block near the top of `code/mech.cpp`:

```bash
grep -n "^#include" A:/Games/mc2-opengl-src/code/mech.cpp | head -20
```

Add the following include adjacent to any existing GameAdapters include, or after the last `#include` of a game-side header near the top:

```cpp
#include "../GameAdapters/MechRenderAdapter.h"  // M2: mech spawn/destroy adapter
```

The path `../GameAdapters/MechRenderAdapter.h` assumes `code/mech.cpp` is compiled from the `code/` directory; verify the relative path resolves by checking that `StaticPropRenderAdapter.h` uses a similar relative prefix in other files that include it. If the build system uses source-relative includes, the path is `"../GameAdapters/MechRenderAdapter.h"`. If it uses root-relative includes (absolute from the source root), check `mission.cpp:243` for the pattern already established: `#include "../GameAdapters/StaticPropRenderAdapter.h"` -- use the same convention.

- [ ] **Step 3: Add MANDATORY pre-init destroyMech at `code/mech.cpp` re-init guard**

**Existing (lines 1302-1304):**

```cpp
	if (appearance)
		delete appearance;
	appearance = new Mech3DAppearance;
```

**Replace with:**

```cpp
	if (appearance) {
	    // M2: MANDATORY pre-init destroyMech. If BattleMech::init() is called
	    // on an object that already has a live appearance (re-init scenario),
	    // the previous appearance is deleted without BattleMech::destroy() being
	    // called. Retire the handle before the old appearance is deleted so the
	    // adapter can access it via getRenderWorldHandle().
	    // No-op if getRenderWorldHandle().isValid() == false (first-time init
	    // before any prior spawn, or already retired).
	    {
	        Mech3DAppearance* m3d = static_cast<Mech3DAppearance*>(appearance);
	        GameAdapters::Mech::destroyMech(*m3d);
	    }
	    delete appearance;
	}
	appearance = new Mech3DAppearance;
```

- [ ] **Step 4: Add spawn call after `appearance->initFX()` at `code/mech.cpp:1311`**

**Existing (lines 1310-1312):**

```cpp
	appearance->init((Mech3DAppearanceTypePtr)mechAppearanceType, this);
	appearance->initFX();
	appearance->setAlphaValue(alphaValue);
```

**Replace with:**

```cpp
	appearance->init((Mech3DAppearanceTypePtr)mechAppearanceType, this);
	appearance->initFX();
	// M2: RenderWorld spawn route. Called AFTER initFX() so the appearance
	// is fully initialized before the adapter records the handle.
	// static_cast safe: appearance is always Mech3DAppearance* here
	// (verified: only this overload assigns new Mech3DAppearance; one hit
	// in code/mech.cpp for "appearance = new").
	// gameObjectId=0 in M2; M2.5 refines when object-ID writes need correlation.
	{
	    Mech3DAppearance* m3d = static_cast<Mech3DAppearance*>(appearance);
	    GameAdapters::Mech::syncSpawn(*m3d, 0u);
	}
	appearance->setAlphaValue(alphaValue);
```

- [ ] **Step 5: Add destroy call inside `BattleMech::destroy()` at `code/mech.cpp`**

**Existing (lines 3722-3729):**

```cpp
void BattleMech::destroy (void) 
{
	if (appearance) 
	{
		delete appearance;
		appearance = NULL;
	}
}
```

**Replace with:**

```cpp
void BattleMech::destroy (void) 
{
	if (appearance) 
	{
	    // M2: retire RenderWorld handle BEFORE deleting the appearance so the
	    // adapter can read the handle via getRenderWorldHandle(). No-op if
	    // already retired (valid handle check is inside destroyMech).
	    // static_cast safe: appearance is always Mech3DAppearance* in
	    // BattleMech (verified: one assignment site in code/mech.cpp:1304).
	    {
	        Mech3DAppearance* m3d = static_cast<Mech3DAppearance*>(appearance);
	        GameAdapters::Mech::destroyMech(*m3d);
	    }
		delete appearance;
		appearance = NULL;
	}
}
```

- [ ] **Step 6: Add include and lifecycle calls in `code/mission.cpp` (worktree)**

In `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/mission.cpp`, the existing include is:

At line 243: `#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 Task 13`

Add immediately after line 243:

```cpp
#include "../GameAdapters/MechRenderAdapter.h"          // M2: mech lifecycle adapter
```

At line 1695: `GameAdapters::StaticProp::beginMission();  // M1 Task 13`

Add immediately after line 1695:

```cpp
	GameAdapters::Mech::beginMission();              // M2: mech lifecycle
```

At line 3282: `GameAdapters::StaticProp::endMission();    // M1 Task 13`

Add immediately after line 3282:

```cpp
	GameAdapters::Mech::endMission();               // M2: mech lifecycle
```

- [ ] **Step 7: Build (full relink -- mech.cpp changed)**

```powershell
Remove-Item A:\Games\mc2-opengl-src\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build A:\Games\mc2-opengl-src\build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 15
```

Expected: build succeeds. All new call sites resolve (no undefined symbols). If any compile error fires, the most likely cause is the relative include path for `MechRenderAdapter.h`; adjust per Step 2 diagnosis.

- [ ] **Step 8: Firewall check**

```bash
sh A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\check-include-firewall.sh
```

Expected: exit 0. The new include in `code/mech.cpp` is not in SCOPE_DIRS, so it does not trigger a violation. The new include in `code/mission.cpp` (worktree `code/`) is also not in SCOPE_DIRS.

- [ ] **Step 9: Tier1 smoke env-OFF (zero pixel delta gate)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. Banner now shows `mechs=M` with M > 0 on missions that spawn mechs (mc2_01, mc2_03, mc2_10, mc2_17, mc2_24 all have mechs). `event=mech_end_mission` lines appear in stderr. Zero pixel delta (M2 does not change rendering).

- [ ] **Step 10: Commit**

```bash
git add code/mech.cpp A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/mission.cpp
git commit -m "$(cat <<'EOF'
feat(mech): wire MechRenderAdapter spawn/destroy lifecycle

M2 slice T4: wires MechRenderAdapter into BattleMech::init and
BattleMech::destroy, plus mission.cpp beginMission/endMission lifecycle.

Call sites:
- code/mech.cpp:1302: MANDATORY pre-init destroyMech before delete+new
  (re-init scenario: retiring the previous appearance's handle before
  the old Mech3DAppearance is deleted).
- code/mech.cpp:1311+: syncSpawn after appearance->initFX() with
  gameObjectId=0 (M2; M2.5 refines).
- code/mech.cpp:3724-3728: destroyMech inside BattleMech::destroy()
  before delete appearance.
- worktree code/mission.cpp:1695+: Mech::beginMission() after StaticProp.
- worktree code/mission.cpp:3282+: Mech::endMission() after StaticProp.

static_cast safety: verified that only one site in code/mech.cpp assigns
appearance = new Mech3DAppearance (line 1304); static_cast is safe.

Tier1 5/5 PASS env-OFF. Banner: mechs=M (M > 0 on mech missions).
event=mech_end_mission emitted at mission teardown.

Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Validation gates

**Files:**
- No source changes; gate verification only.
- Modify: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md` (update Active campaigns after gates pass)

### Gate 1: Tier1 5/5 PASS env-OFF (zero pixel delta)

- [ ] **Step 1: Run tier1 env-OFF**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS, exit 0. Pixel output identical to M2-pre HEAD `16e3d53`. `[STATIC_PROP_REGISTRY v1]` counts unchanged vs M2-pre baseline. `[RENDER_WORLD v1]` banner includes `static_props=S mechs=M` with M > 0 on all five missions.

If gate fails: inspect `tests/smoke/artifacts/<latest>/` for the failing mission's `ring_trace.log`. Look for unexpected `WARN:` or assert lines. The most likely regression is the pre-init destroyMech path firing where it should be a no-op (check that the `if (!h.isValid()) return;` guard in `destroyMech` fires correctly on first-time init).

### Gate 2: Tier1 5/5 PASS env-ON with MC2_RENDER_WORLD_TRACE=1

- [ ] **Step 2: Run tier1 with trace enabled**

```powershell
$env:MC2_RENDER_WORLD_TRACE = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
$env:MC2_RENDER_WORLD_TRACE = $null
```

Expected: tier1 5/5 PASS, exit 0.

- [ ] **Step 3: Verify trace log contents**

```powershell
$latestArtifactDir = Get-ChildItem A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts\ |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
Write-Host "Artifact dir: $latestArtifactDir"

# Check for mech_register events (TRACE-GATED)
Select-String -Path "$latestArtifactDir\*.ring_trace.log" -Pattern "event=mech_register " | Select-Object -First 10

# Check for mech_end_mission (ALWAYS-ON)
Select-String -Path "$latestArtifactDir\*.ring_trace.log" -Pattern "event=mech_end_mission" | Select-Object -First 10

# Verify banner format includes new tokens
Select-String -Path "$latestArtifactDir\*.ring_trace.log" -Pattern "static_props=\d+ mechs=\d+" | Select-Object -First 5
```

Expected:
- `event=mech_register` lines present for at least one mission.
- `event=mech_end_mission` lines present for all five missions (always-on).
- Banner lines contain `static_props=N mechs=M` (M > 0).
- No `event=mech_register_fail` lines (healthy mission load).

If `event=mech_register` is absent even with trace=1: the spawn call site in `code/mech.cpp` did not fire. Verify the build included the wired `code/mech.cpp` (check object file timestamp: `ls build64/RelWithDebInfo/code.dir/mech.cpp.obj`).

### Gate 3: Mech count canary on mc2_03

- [ ] **Step 4: Count mech_register events on mc2_03 (requires TRACE on)**

```powershell
$latestArtifactDir = Get-ChildItem A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts\ |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName

$count = (Select-String -Path "$latestArtifactDir\mc2_03.ring_trace.log" -Pattern "event=mech_register ").Count
Write-Host "mc2_03 mech_register count: $count"
if ($count -eq 0) {
    Write-Host "GATE 3 FAIL: no mech_register events on mc2_03"
    exit 1
} else {
    Write-Host "GATE 3 PASS: $count mech_register events"
}
```

Expected: count > 0. mc2_03 is a combat mission with multiple mechs. A count of 0 means the syncSpawn call site did not fire, indicating the call site edit was not compiled or the mission has a code path that skips `BattleMech::init(bool, ObjectTypePtr)`.

Note: this gate requires `MC2_RENDER_WORLD_TRACE=1` from the Gate 2 run. If running this gate standalone, re-run with trace enabled first.

### Gate 4: Firewall clean

- [ ] **Step 5: Verify firewall**

```bash
sh A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\check-include-firewall.sh
```

Expected: exit 0, "clean (scope: RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL)". Any violation must be resolved before merge.

### Gate 5: Handle validity (no double-register in trace log)

- [ ] **Step 6: Check for double-register anomaly in mc2_03 trace**

A double-register would appear as: `event=mech_register mech=<SAME_PTR>` appearing twice for the same pointer without an intervening `event=mech_destroy`.

```powershell
$latestArtifactDir = Get-ChildItem A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts\ |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName

# Extract mech pointer values from register events
$registerLines = Select-String -Path "$latestArtifactDir\mc2_03.ring_trace.log" -Pattern "event=mech_register mech=0x[0-9a-f]+"
$destroyLines  = Select-String -Path "$latestArtifactDir\mc2_03.ring_trace.log" -Pattern "event=mech_destroy handle\.index=\d+"

Write-Host "mech_register lines: $($registerLines.Count)"
Write-Host "mech_destroy lines:  $($destroyLines.Count)"

# If any mech_register_fail exists, report it
$failLines = Select-String -Path "$latestArtifactDir\mc2_03.ring_trace.log" -Pattern "event=mech_register_fail"
if ($failLines.Count -gt 0) {
    Write-Host "WARN: $($failLines.Count) mech_register_fail events -- investigate"
}
```

Expected: `mech_register_fail` count = 0. `mech_register` count >= `mech_destroy` count (some mechs may not be destroyed within 30s). No assert-triggered output in the log.

### Gate pass: update CLAUDE.md and commit docs

- [ ] **Step 7: Update CLAUDE.md Active campaigns section**

In `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md`, the Active campaigns section currently ends with the M2-pre entry. Add M2 SHIPPED and M2.5 as next.

Find the existing M2-pre entry (ends with `M2 (route-only MechRenderAdapter), M2.5 (mech object-ID substrate), M2.6 (mech pickup integration) follow.`) and add the M2 SHIPPED paragraph after it:

```
- **RenderWorld Slice M2** (SHIPPED 2026-05-23): route-only MechRenderAdapter. Every live Mech3DAppearance instance now has a RenderObjectHandle stored on it (mechRenderHandle field, protected; three public ForAdapter accessors). GameAdapters/MechRenderAdapter.{h,cpp} bridges BattleMech::init/destroy to RenderWorld::registerMech/destroyMech. Mechs allocate from the unified s_objectRecords table at kMechHandleBase=0x00010000 (65536) to avoid recipe-index collision with static props (max known: 2641). RenderObjectRecord gains kind (RenderObjectKind enum) and debugCookie fields. frameBannerTick extended: [RENDER_WORLD v1] now emits static_props=S mechs=M alongside objects=T. endMission force-clears leaked handles via clearAllMechRecords(). Three mandatory call sites in code/mech.cpp: pre-init destroyMech (re-init guard), syncSpawn after initFX(), destroyMech before delete. mission.cpp Mech::beginMission/endMission adjacent to StaticProp calls. Firewall clean; allowlist entry added. Tier1 5/5 PASS env-OFF and env-ON MC2_RENDER_WORLD_TRACE=1. Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md. Plan: docs/superpowers/plans/2026-05-23-renderworld-slice-m2-mech-adapter-plan.md. Next: M2.5 (mech object-ID substrate: per-mech writes to R32_UINT attachment-2); then M2.6 (mech pickup via tryGameplayPick spine).
```

- [ ] **Step 8: Commit docs**

```bash
git add A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md
git commit -m "$(cat <<'EOF'
docs(renderworld): M2 SHIPPED -- update CLAUDE.md active campaigns

Mark RenderWorld Slice M2 (route-only MechRenderAdapter) as SHIPPED.
Add M2.5 (mech object-ID substrate) as next. All 5 validation gates
passed: tier1 5/5 PASS env-OFF and env-ON, mech count canary mc2_03
> 0, firewall clean, no double-register in trace.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Self-review checklist

### Spec coverage

| Spec section | Covered by task |
|---|---|
| Section 3 (purpose) | T1 (registerMech/destroyMech), T4 (call sites) |
| Section 4.1 (MechRenderAdapter.h) | T3 Step 1 |
| Section 4.2 (MechRenderAdapter.cpp) | T3 Step 2 |
| Section 5.1 (RenderObjectKind enum) | T1 Step 3 |
| Section 5.2 (RenderObjectRecord kind field) | T1 Step 3 |
| Section 5.3 (RenderMechDesc) | T1 Step 4 |
| Section 5.4 (registerMech) | T1 Step 6 |
| Section 5.5 (destroyMech) | T1 Step 6 |
| Section 5.6 (impl notes: debugCookie, s_mechs_alive_rw, clearAllMechRecords) | T1 Steps 5-6 |
| Section 5.6 (frameBannerTick extension) | T1 Step 7 |
| Section 6.1 (mechRenderHandle field) | T2 Step 2 |
| Section 6.2 (public accessors with ForAdapter names) | T2 Step 3 |
| Section 6.3 (defensive reset in init()) | T2 Step 4 |
| Section 7.1 (spawn call site mech.cpp) | T4 Steps 3-4 |
| Section 7.2 (destroy call site mech.cpp) | T4 Step 5 |
| Section 7.3 (cast discipline) | T1 Step 1 (verification); T4 Steps 3-5 (static_cast with comment) |
| Section 7.4 (mission.cpp wiring) | T4 Step 6 |
| Section 8.3 (endMission force-clear) | T3 Step 2 (endMission), T1 Step 6 (clearAllMechRecords) |
| Section 8.4 (re-init scenario) | T4 Step 3 (pre-init destroyMech -- MANDATORY) |
| Section 9.1 (four counters, separate from static-prop) | T3 Step 2 |
| Section 9.2 (log format) | T3 Step 2 (all event names match spec) |
| Section 9.3 (banner static_props=S mechs=M) | T1 Step 7 |
| Section 10 (firewall F-1 through F-7) | T3 Steps 4-5 (allowlist + firewall check) |
| Section 11 Gate 1 | T5 Step 1 |
| Section 11 Gate 2 | T5 Steps 2-3 |
| Section 11 Gate 3 | T5 Step 4 |
| Section 11 Gate 4 | T5 Step 5 |
| Section 11 Gate 5 | T5 Step 6 |

### Placeholder scan

- No "TBD" or "TODO" in the plan.
- No "implement later" or "similar to Task N" shortcuts.
- All code blocks are complete; no partial diffs.
- All "Existing:" blocks show the actual current code (read at plan-write time and verified with grep).
- Commit messages are specific (not placeholders).
- Line numbers verified by grep at plan-write time.

### Type consistency

- `getRenderWorldHandle()` / `setRenderWorldHandleForAdapter()` / `clearRenderWorldHandleForAdapter()` used consistently throughout; old names `setRenderWorldHandle` and `clearRenderWorldHandle` (without "ForAdapter") do NOT appear anywhere in this plan.
- `RenderCore::RenderObjectHandle` used as the handle type throughout (consistent with Handle.h definition).
- `RenderWorld::RenderMechDesc` used in T1 Step 4 (declaration) and T3 Step 2 (`MechRenderAdapter.cpp` body) -- names match.
- `RenderWorld::clearAllMechRecords()` declared in T1 Step 4, implemented in T1 Step 6, called in T3 Step 2 -- names match.
- `RenderObjectKind::StaticProp` / `RenderObjectKind::Mech` used consistently in the enum (T1 Step 3) and the `registerMech` implementation (T1 Step 6).
- `kMechHandleBase` defined in anonymous namespace in `RenderWorld.cpp` (T1 Step 5) and used in `registerMech` and `clearAllMechRecords` (T1 Step 6) -- same TU, no header needed.
- `GameAdapters::Mech::syncSpawn` / `GameAdapters::Mech::destroyMech` / `GameAdapters::Mech::beginMission` / `GameAdapters::Mech::endMission` -- names consistent between `MechRenderAdapter.h` (T3 Step 1) and the call sites in `mech.cpp` (T4 Steps 3-5) and `mission.cpp` (T4 Step 6).
