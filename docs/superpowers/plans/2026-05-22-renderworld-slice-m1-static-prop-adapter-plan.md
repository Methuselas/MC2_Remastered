# RenderWorld Slice M1 — Static Prop Adapter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce the `RenderWorld` boundary as a *route-only* shim in front of `GpuStaticPropRegistry`, with a `GameAdapters::StaticPropRenderAdapter` bridging the 5 audited `Appearance`-side call sites (4 in `mclib/bdactor.cpp`, 1 in `code/warrior.cpp`); zero pixel delta, same indirect command stream, same `[STATIC_PROP_REGISTRY v1]` counts.

**Architecture:** Three new modules — `RenderCore/` (pure type vocabulary: `Handle<Tag>`, `RenderObjectDesc`, `DrawPacket`), `RenderWorld/` (engine API surface; thin forwarder into existing `GpuStaticPropRegistry` namespace), `GameAdapters/` (the only place that may include both game-side `mclib/appear.h` and engine-side `RenderWorld.h`). Section-12 firewall enforced by `scripts/check-include-firewall.sh` (Phase 1: grep, pre-commit; CI gate deferred). Sentinel translation `-1 <-> Handle::invalid()` happens at every game/engine seam (M1 has TWO: the GameAdapters seam and the RenderWorld/legacy backend seam) per M3 fix. New `[RENDER_WORLD v1]` frame banner mirrors prop count; opt-in `MC2_RENDER_WORLD_TRACE=1` for per-event traces. No new renderer behavior, no new LOD/material/visibility model, no FBO change.

**Tech Stack:** C++17, MSVC `--config RelWithDebInfo`, CMake `add_library(... STATIC)`, existing `GpuStaticPropRegistry` namespace API (signatures preserved bit-for-bit), tier1 smoke harness, `[SUBSYS v1]` instrumentation convention.

**Spec:** `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md` (EXECUTABLE-READY).
**Adversarial review applied:** `docs/superpowers/reviews/2026-05-22-renderworld-boundary-spec-adversarial-review.md` (0 CRIT, 5 MAJOR all resolved; MINORs m1/m2/m4/m5 documented).

---

## Decisions needed BEFORE Phase A executes

These are surfaced from the spec; they have *defaults* the plan assumes if the user does not override. The plan executor MUST get explicit user sign-off (or take the default explicitly) before Task 1.

**RESOLVED 2026-05-22 (all defaults confirmed by user):**
- D1: top-level dirs (`RenderCore/`, `RenderWorld/`, `GameAdapters/` at worktree root).
- D2: by-value `std::vector<GpuStaticPropInstance>` in `RenderObjectDesc`.
- D3: stateful TU with anon-namespace; required entry points:
  `beginMission()`, `endMission()`, `syncStaticProp(...)`,
  `syncStaticPropLateSpawn(...)`, `destroyStaticProp(...)`. State
  quarantined in adapter `.cpp`; banner counters + late-spawn accounting
  live there.
- D4: `StaticRegistration.recipeIndex` stays `int32_t` below the adapter.
  `RenderObjectHandle` lives at the boundary only. Adapter translates
  both directions; widening backend storage is a future slice.

**ADDITIONAL RESOLUTIONS 2026-05-22 (adversarial review pass 2):**
- C1: RenderCore stays pure. Introduce `RenderCore/StaticPropInstanceDesc.h`
  (POD mirror of `GpuStaticPropInstance`, no GL, no game headers). The
  adapter (game side) and a NEW private legacy-backend TU under
  `RenderWorld/legacy/static_prop_backend.cpp` (engine side) own the
  two-endpoint translation between `StaticPropInstanceDesc` and
  `GpuStaticPropInstance`. RenderCore public headers MUST NOT include
  `gos_static_prop_batcher.h`, `<GL/glew.h>`, `Stuff/Stuff.hpp`, `tgl.h`,
  `msl.h`, or any mclib/game header.
- C2: Tasks 8 and 10 PRESERVE the H4 `needsFullBakeNextFrame = true`
  follow-up block (bdactor.cpp:1475-1482 and :4273-4280). Failing to do
  so regresses the 2026-05-07 black-actor / black-tree fix.
- M1: Adapter entry points renamed to spec D3 names —
  `beginMission` / `endMission` / `syncStaticProp` /
  `syncStaticPropLateSpawn` / `destroyStaticProp`. `beginMission` /
  `endMission` wire at `code/mission.cpp:1693` and `:3279`
  (per-mission, NOT per-process — sibling of
  `GpuStaticPropRegistry::init/destroy`).
- M2: Firewall `SCOPE_DIRS` enumerates the full Section 12 module list
  with `[ -d ] || continue` guards: `RenderCore RenderWorld Visibility
  MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL`.
- M3: Sentinel translation lives at every game/engine seam (M1 has TWO:
  GameAdapters seam and RenderWorld legacy-backend seam). Spec amendment
  handled by a separate subagent; not duplicated in this plan.
- M4: `Handle<Tag>` uses explicit `uint32_t bits` + shift/mask packing,
  NOT C++ bitfields. Layout becomes well-defined across compilers /
  Vulkan / future ports.
- m1: `RenderWorld::init/destroy` and `GameAdapters::StaticProp::beginMission/
  endMission` are PER-MISSION at `code/mission.cpp:1693` / `:3279`,
  NOT at `GameOS/gameos/gameosmain.cpp`.
- m2: Frame banner emit goes immediately AFTER the `gos_RendererEndFrame()`
  call in `GameOS/gameos/gameosmain.cpp:541` (where the count is final).
- m4: `[RENDER_WORLD v1] objects=N` is sourced from a registry
  active-recipe accessor (`GpuStaticPropRegistry::getActiveCount()` to be
  added) rather than the adapter-side `s_upsertOk - s_destroyCalls`
  delta. Avoids drift when registry tombstones via paths the adapter
  doesn't see.
- m5: Late-spawn path produces a real `RenderObjectHandle`. New helper
  `GpuStaticPropRegistry::registerStaticPropAndReturnRecipe(Appearance*)`
  returns the recipe index; `RenderWorld::adoptStaticPropRecipe(int32_t)`
  wraps an existing slot without creating a new entry; adapter
  `syncStaticPropLateSpawn` returns the resulting handle.

### D1. Module physical layout (repo-root vs. under `mclib/`)

The spec says "modules" but does not commit to physical layout. Two options:

- **D1.A (default, plan assumes this):** New top-level directories under the worktree root —
  `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderCore/`,
  `.../RenderWorld/`, `.../GameAdapters/`. Each gets its own `CMakeLists.txt`,
  each `add_subdirectory()`'d from the root `CMakeLists.txt` alongside the
  existing `add_subdirectory("./mclib/...")` block (root `CMakeLists.txt:156-165`).
  Pros: makes the boundary visible at the directory level; the firewall script
  has trivially-named scope dirs; matches the spec's module-boundary language;
  symmetric with `GameOS/gameos/` already living at the root.
  Cons: introduces three new root-level dirs.
- **D1.B:** Nest under `mclib/renderworld/{core,world,adapters}/` or
  `GameOS/renderworld/...`. Pros: fewer root dirs. Cons: physically buries
  a boundary the spec wants visible; the firewall script must whitelist
  internal paths; risks the modules being treated as mclib-internal.

**Lean per spec Section 12 language:** D1.A. Confirm before Task 1.

### D2. `RenderObjectDesc` payload shape (by-value vs view)

Two options:

- **D2.A (default, plan assumes this):** `RenderObjectDesc` holds the full
  `std::vector<GpuStaticPropInstance>` batch *by value* (move-constructed
  from the caller). The adapter builds the batch then moves it into the
  desc. The forwarder in `RenderWorld.cpp` moves it onward into
  `GpuStaticPropRegistry::registerRecipe(...)`. One copy at most;
  Phase 1 documentary; matches the registry's existing `const std::vector<...>&`
  parameter signature.
  Pros: simple ownership; safe across the boundary; no lifetime puzzle.
  Cons: one allocation per registration (already exists in the legacy path;
  no regression).
- **D2.B:** `RenderObjectDesc` holds a `std::span<const GpuStaticPropInstance>`
  view. Caller owns the storage. Pros: zero copy. Cons: lifetime contract
  travels across a public boundary that crosses TUs; one stray temporary
  on the caller side is UB; adapter cannot move-promote.

**Lean per Vulkan-prep (descriptor-by-value matches Vulkan's `Vk*CreateInfo`
pattern):** D2.A. Confirm before Task 2.

### D3. Adapter lifecycle (free-function pair vs stateful TU vs per-mission instance)

Three options:

- **D3.A (default, plan assumes this):** Stateful TU with file-scope state
  inside `StaticPropRenderAdapter.cpp` (anonymous-namespace globals per
  spec §12 Q12.2 resolution). One adapter instance per process; reset hooks
  via `beginMission`/`endMission` callbacks. No header state.
  Pros: matches spec §10 lean ("real TU"); anonymous-namespace helpers
  cannot leak gameData types out of the TU; matches existing mission lifecycle.
  Cons: testability is harder than free functions (but Phase 1 has no unit
  tests anyway — slice gate is tier1 smoke + parity).
- **D3.B:** Pure free-function pair (`syncStaticProp`, `destroyStaticProp`)
  with no per-mission state. Pros: simplest. Cons: cannot hold per-mission
  counters for the `[RENDER_WORLD v1]` banner without a free-floating global.
- **D3.C:** `StaticPropRenderAdapter` instance held by `RenderWorld`,
  constructed in `RenderWorld::beginMission`. Pros: textbook ownership.
  Cons: forces `RenderWorld.h` to know about the adapter type;
  cross-references `GameAdapters` from `RenderWorld` — boundary failure.
  REJECT.

**Lean per spec §10 + §12 Q12.2:** D3.A. Confirm before Task 6.

### D4. Spec ambiguity hit during plan-write (flag for user)

- The spec at §13 says "the slice closes only when every production producer
  routes through the adapter" and lists 5 call sites — but it does not
  explicitly forbid `staticReg.recipeIndex` itself from continuing to be
  an `int32_t` in `StaticRegistration` (the struct that holds the bdactor
  side of the registration). This plan treats `staticReg.recipeIndex` as
  **storage-side, below the adapter, may stay `int32_t`** — the sentinel
  translation rule applies at the adapter boundary, not at the storage
  field. The `RenderObjectHandle` lives only above the adapter (in
  caller-visible API), and is *not* persisted to `StaticRegistration`.
  Rationale: M1 is route-only; widening `StaticRegistration` to a
  typed handle is a future-slice refactor. Confirm this reading.

---

## Pre-flight reading (engineer MUST read before Task 1)

1. Spec sections 3 (handle 20/12 split), 10 (adapter migration plan + sentinel
   translation), 12 (forbidden-deps + dependency shapes), 13 (first migration
   target + correctness gate). Adversarial review M1/M2/M4 resolutions.
2. Worktree CLAUDE.md — full file. Especially: NO emoji, grep-before-cite,
   build `--config RelWithDebInfo`, full-relink discipline, canonical smoke
   gate command, Vulkan-prep discipline, substitutive-not-additive rule.
3. Verify the 5 audit lines are still at the cited offsets BEFORE starting
   any task that touches them (Task 8-12). The audit pass was captured
   2026-05-22; if shipping HEAD has drifted, adjust line numbers in this
   plan and proceed.

---

## File structure

**Created directories (under worktree root, per Decision D1.A):**
- `RenderCore/` — pure types; no GL, no game headers
- `RenderWorld/` — engine API; no game headers (no forward-decls of game types either)
- `GameAdapters/` — the only module that may bridge both sides

**Created files:**
- `RenderCore/CMakeLists.txt`
- `RenderCore/Handle.h` — `Handle<Tag>` template (20-bit index, 12-bit generation; uint32_t shift/mask, NOT bitfields per M4 fix)
- `RenderCore/StaticPropInstanceDesc.h` — POD mirror of `GpuStaticPropInstance` (NEW per C1 fix; pure C++ standard types, no GL, no mclib)
- `RenderCore/RenderObjectDesc.h` — desc + archetype flags + tag types (uses `StaticPropInstanceDesc` and forward-declared `TG_MultiShape`; NO engine-side payload include per C1 fix)
- `RenderCore/DrawPacket.h` — packet struct + sort-key layout comment (documentary in M1)
- `RenderWorld/CMakeLists.txt`
- `RenderWorld/RenderWorld.h` — engine-facing API surface
- `RenderWorld/RenderWorld.cpp` — thin forwarder + banner emission
- `RenderWorld/legacy/static_prop_backend.h` — internal legacy-backend bridge declarations (NEW per C1 fix; not in public include path)
- `RenderWorld/legacy/static_prop_backend.cpp` — engine-side translation `StaticPropInstanceDesc` -> `GpuStaticPropInstance`; the ONLY engine TU that includes `gos_static_prop_batcher.h` and calls `GpuStaticPropRegistry::registerRecipe` (NEW per C1 fix)
- `GameAdapters/CMakeLists.txt`
- `GameAdapters/StaticPropRenderAdapter.h` — forward-decl `class Appearance;` allowed (spec §12 carve-out)
- `GameAdapters/StaticPropRenderAdapter.cpp` — includes `mclib/appear.h`, `bdactor.h`, real game-side headers
- `scripts/check-include-firewall.sh` — Phase 1 grep gate
- `scripts/check-include-firewall.allowlist` — known-legacy carve-outs

**Modified files:**
- `CMakeLists.txt` (root) — three new `add_subdirectory(...)` lines; one new `target_link_libraries(mc2 ...)` line
- `mclib/bdactor.cpp` — 4 call sites converted (lines 1471, 2802, 4269, 4855 per audit); H4 follow-up `needsFullBakeNextFrame = true` block PRESERVED at both Bldg (line 1475-1482) and Tree (line 4273-4280) sites per C2 fix
- `code/warrior.cpp` — 1 call site converted (line 7593 per audit)
- `code/mission.cpp` — add `RenderWorld::init()` / `GameAdapters::StaticProp::beginMission()` at line 1693 and matching destroy/end at line 3279 (per-mission, sibling of `GpuStaticPropRegistry::init/destroy`; per m1 fix)
- `GameOS/gameos/gos_static_prop_registry.h` — add `int32_t registerStaticPropAndReturnRecipe(Appearance*)` and `uint32_t getActiveCount()` accessors (per m5 and m4 fixes; signature-additive, existing surface preserved)
- `GameOS/gameos/gos_static_prop_registry.cpp` — implement the two new accessors (read-only counter + thin wrapper); banner integration into existing `[STATIC_PROP_REGISTRY v1]` summary remains optional
- `GameOS/gameos/gameosmain.cpp` — `RenderWorld::frameBannerTick()` immediately AFTER `gos_RendererEndFrame()` at line 541 (per m2 fix)

**Untouched (load-bearing — confirm via grep, not assumption):**
- `GameOS/gameos/gos_static_prop_registry.h` — signature preserved; the
  existing `class Appearance;` forward-decl at line 9 stays (adversarial
  review M2 carve-out: forward-decls in legacy backend headers are
  grandfathered for Phase 1; the firewall script's symbol grep excludes
  this file via allowlist).
- `GameOS/gameos/gos_static_prop_batcher.{h,cpp}` — indirect path unchanged
- `shaders/*` — zero shader edits in M1
- All shaders that consume `GpuActorRecord` SSBO — unchanged

---

## Phase A — Scaffolding (build green, no behavior change)

**Phase A goal:** modules and types exist; `mc2.exe` links; no call site
yet routes through the adapter. After Phase A, `git diff HEAD~N --stat`
shows new files only; runtime behavior is bit-for-bit unchanged.

**Phase A gate (must pass before Phase B starts):** tier1 5/5 smoke pass
with `objects=0` reported by `[RENDER_WORLD v1]` banner (because no call
sites are wired yet). Banner emits, count is zero, no regression.

### Task 1: Create `RenderCore/Handle.h`

**Files:**
- Create: `RenderCore/Handle.h`

- [ ] **Step 1: Verify directory does not already exist**

```powershell
Test-Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderCore
```

Expected: `False`. If `True`, stop — directory was created by a previous
run; rebase or clean before proceeding.

- [ ] **Step 2: Create `RenderCore/Handle.h`**

```cpp
// RenderCore/Handle.h
//
// Slice M1 (route-only): type-safe opaque handle.
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 3 (handle model), 20/12 bit split (Q3.1 RESOLVED 2026-05-22).
//
// Invariants (load-bearing):
//   - Holder MUST NOT interpret `index` or `generation`.
//   - Handle::invalid() is the ONLY sentinel; do not overload with -1 / 0.
//   - Generation MUST be bumped when a slot is recycled.
//   - Equality is bitwise; trivially hashable by uint32_t pun.
//
// Phase 1 documentary: M1 does not yet recycle slots (recipe path is
// mission-lifetime). Generation defaults to 1 on first construction so
// that the canonical invalid() (index=0, generation=0) cannot collide
// with the first legitimate live handle.

#pragma once

#include <cstdint>

namespace RenderCore {

// M4 fix (adversarial review pass 2 2026-05-22): explicit uint32_t
// shift/mask, NOT bitfields. C++ bitfield layout is implementation-defined;
// future Vulkan / cross-compiler ports need the bit ordering to be
// well-defined. Wire encoding: bits[19:0] = index, bits[31:20] = generation.
template <typename Tag>
struct Handle {
    uint32_t bits = 0;  // [19:0] index, [31:20] generation

    static constexpr Handle make(uint32_t index, uint32_t generation) noexcept {
        Handle h;
        h.bits = (generation << 20) | (index & 0xFFFFFu);
        return h;
    }

    [[nodiscard]] constexpr uint32_t index() const noexcept {
        return bits & 0xFFFFFu;
    }
    [[nodiscard]] constexpr uint32_t generation() const noexcept {
        return bits >> 20;
    }

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return bits != 0;
    }

    [[nodiscard]] static constexpr Handle invalid() noexcept {
        return Handle{0u};
    }

    [[nodiscard]] constexpr uint32_t raw() const noexcept {
        return bits;
    }

    constexpr bool operator==(Handle o) const noexcept { return bits == o.bits; }
    constexpr bool operator!=(Handle o) const noexcept { return bits != o.bits; }
};

// Tag types — empty structs purely for nominal typing of Handle<>.
struct RenderObjectTag {};
struct ViewTag        {};
struct MeshTag        {};
struct MaterialTag    {};
struct TextureTag     {};

using RenderObjectHandle = Handle<RenderObjectTag>;
using ViewHandle         = Handle<ViewTag>;
using MeshHandle         = Handle<MeshTag>;
using MaterialHandle     = Handle<MaterialTag>;
using TextureHandle      = Handle<TextureTag>;

// Compile-time invariants.
static_assert(sizeof(RenderObjectHandle) == sizeof(uint32_t),
              "Handle must be 32 bits wide for hot-path passing.");

} // namespace RenderCore
```

- [ ] **Step 3: Verify no GL or game-side header is reachable**

```bash
grep -n "include" RenderCore/Handle.h
```

Expected output:

```
#include <cstdint>
```

Exactly one include. Any other line is a boundary failure.

- [ ] **Step 4: Commit**

```bash
git add RenderCore/Handle.h
git commit -m "feat(renderworld): add RenderCore/Handle.h (M1 Task 1)

20-bit index / 12-bit generation per boundary spec section 3.
Pure RenderCore: no GL, no game headers. Type-safe opaque
handle for the M1 route-only static-prop slice.

Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md"
```

### Task 2: Create `RenderCore/StaticPropInstanceDesc.h` and `RenderCore/RenderObjectDesc.h`

**Files:**
- Create: `RenderCore/StaticPropInstanceDesc.h` (NEW per C1 fix)
- Create: `RenderCore/RenderObjectDesc.h`

**C1 fix (adversarial review pass 2 2026-05-22):** RenderCore public
headers MUST NOT include `gos_static_prop_batcher.h`. That header
transitively pulls `<GL/glew.h>`, `Stuff/Stuff.hpp`, and other engine
types — anything that includes it inherits the entire GL+Stuff include
graph and the firewall promise that "RenderCore is pure" becomes a lie.
The fix: a POD mirror `StaticPropInstanceDesc` lives in RenderCore;
translation between it and `GpuStaticPropInstance` happens at the TWO
seams (game-side adapter and engine-side legacy backend).

- [ ] **Step 0a: Inspect the legacy struct shape**

```bash
grep -n "struct GpuStaticPropInstance" GameOS/gameos/gos_static_prop_batcher.h
grep -n "^[[:space:]]*\(float\|uint32_t\|int32_t\|vec\|mat\)" GameOS/gameos/gos_static_prop_batcher.h | head -40
```

Read the struct in full so the POD mirror matches field-for-field. The
mirror MUST contain identical scalar layout; translation at the two
seams is a `memcpy`-equivalent copy when sizes match, OR a field-by-field
assign when scalar types differ.

- [ ] **Step 0b: Create `RenderCore/StaticPropInstanceDesc.h` (POD mirror)**

```cpp
// RenderCore/StaticPropInstanceDesc.h
//
// Slice M1 (C1 fix): POD mirror of GpuStaticPropInstance.
// Pure C++ standard types only — no GL, no Stuff, no mclib.
// Translation between this and GpuStaticPropInstance happens at the
// two engine-game seams:
//   - Game-side seam: GameAdapters/StaticPropRenderAdapter.cpp
//   - Engine-side seam: RenderWorld/legacy/static_prop_backend.cpp
//
// Both seams must be updated together when the layout drifts. The
// firewall script enforces that no OTHER TU includes
// gos_static_prop_batcher.h from RenderCore/ or RenderWorld/.

#pragma once

#include <cstdint>

namespace RenderCore {

// Field layout: keep in lockstep with GpuStaticPropInstance. The
// legacy-backend TU and the adapter TU each `static_assert` size
// compatibility at compile time.
struct StaticPropInstanceDesc {
    // Mirror of the engine-side struct. Populate fields here as the
    // executor reads gos_static_prop_batcher.h at write time and fills
    // the POD copy. Use `float[N]` / `uint32_t` only; NO vec3, mat4, or
    // Stuff types.
    //
    // (Field-by-field listing is intentionally deferred to executor
    // step 0a above; the file write must reflect the actual current
    // GpuStaticPropInstance layout, not a stale snapshot. The two
    // translation seams compile-time check size match.)
    uint8_t opaque[ /* sizeof(GpuStaticPropInstance) at write time */ 1 ];
};

} // namespace RenderCore
```

NOTE: the executor populates the field list at write time from the
live struct definition. Do NOT carry forward stale field lists from
this plan. The `static_assert(sizeof(...) == sizeof(...))` at the two
translation seams (Tasks 7 and 6.5) is the load-bearing safety check.

- [ ] **Step 1: Confirm `GpuStaticPropInstance` exists (engine side)**

```bash
grep -rn "struct GpuStaticPropInstance" GameOS/gameos/ | head -3
```

Expected: a hit in `gos_static_prop_batcher.h`. This struct is NOT
reachable from RenderCore (the include would violate C1). It IS
reachable from the engine-side legacy-backend TU (Task 6.5) and from
the adapter TU (Task 7); both perform the translation.

- [ ] **Step 2: Create `RenderCore/RenderObjectDesc.h`**

```cpp
// RenderCore/RenderObjectDesc.h
//
// Slice M1: engine-side descriptor for the static-prop upsert path.
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 4 (render object lifecycle).
//
// M1 scope: this is the descriptor the StaticPropRenderAdapter builds
// from an Appearance and passes into RenderWorld::upsertStaticProp.
// Phase 1 documentary: only the fields the static-prop path actually
// consumes are populated. ArchetypeFlags and LayerMask are declared
// for shape correctness but ignored by the M1 forwarder.

#pragma once

#include <cstdint>
#include <vector>

#include "Handle.h"
#include "StaticPropInstanceDesc.h"  // C1 fix: pure POD mirror, NOT GpuStaticPropInstance.

// Forward-decl engine-side TG_MultiShape (C1 fix: prefer typed
// forward-decl over `void*`). Defined in mclib/tgl.h, but
// tgl.h is engine-side (not game-side: AI/mission/Appearance). Per
// spec Section 12 allowance, engine-side geometry forward-decls in
// RenderCore are permitted; the storage backend takes the pointer
// through and never dereferences it inside RenderCore.
class TG_MultiShape;

namespace RenderCore {

// Documentary-only in M1; M2+ consumers populate.
struct ArchetypeFlags {
    uint32_t castsShadow         : 1;
    uint32_t receivesShadow      : 1;
    uint32_t selectable          : 1;
    uint32_t usesImpostor        : 1;
    uint32_t hasClusterLod       : 1;
    uint32_t isSensorVisibleOnly : 1;
    uint32_t isOverlayOnly       : 1;
    uint32_t isStaticForMission  : 1;
    uint32_t reserved            : 24;
};

// Documentary-only in M1.
using LayerMask = uint32_t;
constexpr LayerMask kLayerMain   = 1u << 0;
constexpr LayerMask kLayerShadow = 1u << 1;
constexpr LayerMask kLayerAll    = 0xFFFFFFFFu;

// M1 static-prop descriptor.
//
// Per Decision D2.A (by-value payload): the adapter MOVES the batch
// into this desc; the legacy backend translates each element onward
// into the engine-side GpuStaticPropInstance. One owner at all times.
// No lifetime contract across TUs.
//
// C1 fix: `batch` is the POD-mirror type, not GpuStaticPropInstance.
struct StaticPropDesc {
    TG_MultiShape*                          shape    = nullptr;
    std::vector<StaticPropInstanceDesc>     batch;
    ArchetypeFlags                          archetype{};
    LayerMask                               layers   = kLayerAll;
    uint32_t                                gameObjectId = 0;  // opaque echo
};

} // namespace RenderCore
```

- [ ] **Step 3: Build check (header-only consumer)**

The header has no callers yet; verify it parses by including it from a
throwaway TU. Easiest: a one-line include-only smoke at the top of
`RenderCore/Handle.h`'s eventual companion `.cpp` (Task 4). For now,
just lint syntactically by reading it back.

- [ ] **Step 4: Commit**

```bash
git add RenderCore/StaticPropInstanceDesc.h RenderCore/RenderObjectDesc.h
git commit -m "feat(renderworld): add RenderCore desc + POD mirror (M1 Task 2)

C1 fix: RenderCore stays pure. StaticPropInstanceDesc is a POD
mirror of GpuStaticPropInstance with no GL / Stuff / mclib reach.
RenderObjectDesc consumes the mirror, plus a forward-declared
TG_MultiShape pointer (engine-side geometry; spec section 12
allowance). Decision D2.A: by-value batch payload.

Translation between mirror and legacy struct happens at the two
seams (GameAdapters and RenderWorld/legacy/static_prop_backend),
each with a static_assert size check.

Spec: 2026-05-22-renderworld-boundary-spec.md section 4"
```

### Task 3: Create `RenderCore/DrawPacket.h`

**Files:**
- Create: `RenderCore/DrawPacket.h`

- [ ] **Step 1: Create `RenderCore/DrawPacket.h`**

```cpp
// RenderCore/DrawPacket.h
//
// Slice M1: documentary only. The struct exists so future slices can
// land DrawPacket-based dispatch without re-litigating the type. The
// M1 backend continues to emit `glDrawElementsIndirect` via the
// existing GpuStaticPropBatcher::flush() path; no packets are
// dispatched at runtime in M1.
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 6 (draw packet model).

#pragma once

#include <cstdint>
#include "Handle.h"

namespace RenderCore {

// Sort-key layout per spec section 6 (proposed; documented for the next
// slice that actually sorts by it). NOT consumed in M1.
//
// [63:60]  pass priority   (opaque=0, alpha=8, overlay=12)
// [59:56]  view priority   (main=0, shadow=4, minimap=8)
// [55:32]  pipeline id     (24 bits, group key into PipelineDesc cache)
// [31:16]  material id     (16-bit hash bucket per spec MINOR m1)
// [15:0]   depth bucket    (alpha back-to-front; opaque inverted)
struct DrawPacket {
    uint32_t       pipelineId;    // PipelineDesc cache key (Phase 1: opaque)
    MeshHandle     mesh;
    MaterialHandle material;
    uint32_t       objectIndex;
    uint32_t       lightIndex;
    uint32_t       firstIndex;
    uint32_t       indexCount;
    uint32_t       instanceCount;
    uint64_t       sortKey;
};

static_assert(sizeof(DrawPacket) <= 64,
              "DrawPacket should fit in one cache line for hot-loop emission.");

} // namespace RenderCore
```

- [ ] **Step 2: Commit**

```bash
git add RenderCore/DrawPacket.h
git commit -m "feat(renderworld): add RenderCore/DrawPacket.h (M1 Task 3)

Documentary-only in M1. Struct exists so subsequent slices can
land packet-based dispatch without re-litigating the type. M1
continues to emit indirect commands through the existing
GpuStaticPropBatcher path.

Spec: 2026-05-22-renderworld-boundary-spec.md section 6"
```

### Task 4: Create `RenderCore/CMakeLists.txt` and wire to root

**Files:**
- Create: `RenderCore/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root) — add `add_subdirectory(...)` line

- [ ] **Step 1: Create `RenderCore/CMakeLists.txt`**

`RenderCore/` is header-only in M1. We still emit a CMake target so the
include path is centralized and the firewall script has a target to lint.

```cmake
# RenderCore/CMakeLists.txt
#
# Slice M1: header-only module. INTERFACE library exposes the public
# include directory; no compiled translation units yet.

add_library(rendercore INTERFACE)
target_include_directories(rendercore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(rendercore INTERFACE cxx_std_17)
```

- [ ] **Step 2: Locate insertion point in root `CMakeLists.txt`**

```bash
grep -n "add_subdirectory" CMakeLists.txt
```

Expected: hits at lines 156-165 (per audit 2026-05-22). Insert new lines
immediately AFTER line 156 (the `mclib` line) so RenderCore appears
adjacent to the existing module list.

- [ ] **Step 3: Insert `add_subdirectory` line**

```cmake
add_subdirectory("./RenderCore" "./out/RenderCore")
```

- [ ] **Step 4: Clean build**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
```

Expected: build succeeds. `rendercore` target is built (zero .obj
because INTERFACE) and mc2.exe links unchanged.

- [ ] **Step 5: Commit**

```bash
git add RenderCore/CMakeLists.txt CMakeLists.txt
git commit -m "build(renderworld): wire RenderCore INTERFACE module (M1 Task 4)

Header-only module; INTERFACE target carries include dir.
No compiled units in M1; sets up the include topology for
RenderWorld and GameAdapters consumers in Tasks 5-7.

Spec: 2026-05-22-renderworld-boundary-spec.md section 13"
```

### Task 5: Create `RenderWorld/RenderWorld.h`

**Files:**
- Create: `RenderWorld/RenderWorld.h`

- [ ] **Step 1: Create `RenderWorld/RenderWorld.h`**

```cpp
// RenderWorld/RenderWorld.h
//
// Slice M1: engine-facing scene API. The first surface in the
// RenderWorld boundary; takes engine types only.
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 4 (lifecycle), section 13 (first-slice scope).
//
// Firewall (spec section 12, load-bearing):
//   - This header MUST NOT include any game-side header.
//   - This header MUST NOT forward-declare any game-side type
//     (no `class Appearance;`, no `class BldgAppearance;`, ...).
//   - Adapters in GameAdapters/ may bridge both sides; this header
//     may not.

#pragma once

#include "../RenderCore/Handle.h"
#include "../RenderCore/RenderObjectDesc.h"

namespace RenderWorld {

// Lifecycle.
//
// Phase 1 / M1: a process-singleton implementation lives in
// RenderWorld.cpp. M1 does not yet model mission scope at the
// RenderWorld layer; static-prop slots are mission-lifetime by virtue
// of the underlying GpuStaticPropRegistry behavior. M2+ promotes
// begin/endMission to a real boundary call.
void init();
void destroy();

// Engine-facing upsert. Adapter calls this after building a StaticPropDesc
// from a game-side Appearance. The desc is MOVED in; ownership of the
// vector inside the desc transfers to the forwarder.
//
// Returns RenderObjectHandle::invalid() on failure. Caller MUST translate
// at the adapter boundary if a legacy sentinel (-1) is expected upward.
RenderCore::RenderObjectHandle upsertStaticProp(RenderCore::StaticPropDesc desc);

// m5 fix (adversarial review pass 2 2026-05-22): late-spawn path. The
// adapter's syncStaticPropLateSpawn calls the legacy
// GpuStaticPropRegistry::registerStaticPropAndReturnRecipe(Appearance*)
// which actually creates the recipe; THIS function wraps the already-
// created recipe index in a Handle so the counter and handle table stay
// honest. Does NOT create a new registry entry. Returns invalid() if
// recipeIndex < 0.
RenderCore::RenderObjectHandle adoptStaticPropRecipe(int32_t recipeIndex);

// Engine-facing destroy. Adapter calls this when a registration must be
// torn down (per-actor invalidate path). No-op on invalid() input.
void destroy(RenderCore::RenderObjectHandle h);

// Engine-facing visibility mark. M1: thin forwarder onto
// GpuStaticPropRegistry::markVisible. lightDataIndex and extentRadius
// match the existing registry signature; default args preserved.
void markVisible(RenderCore::RenderObjectHandle h,
                 uint32_t lightDataIndex = 0xFFFFFFFFu,
                 float    extentRadius  = 0.0f);

// Validity probe (engine-side; useful for debug asserts above the adapter).
bool isReady(RenderCore::RenderObjectHandle h);

// Frame banner emission. Called once per frame from gamecam.cpp's
// frame-end path (Task 14). Emits `[RENDER_WORLD v1]` with the current
// active prop count read from the underlying registry counters.
//
// Env-gated:
//   MC2_RENDER_WORLD_TRACE=1 -> per-frame banner
//   default                  -> monotonic 600-frame summary
void frameBannerTick();

} // namespace RenderWorld
```

- [ ] **Step 2: Verify no forbidden include**

```bash
grep -E "include.*(appear|bdactor|mech3d|objmgr|warrior|mission)" RenderWorld/RenderWorld.h
```

Expected: empty. Any hit is a boundary failure.

- [ ] **Step 3: Commit**

```bash
git add RenderWorld/RenderWorld.h
git commit -m "feat(renderworld): add RenderWorld API header (M1 Task 5)

Engine-facing surface. No game-side includes, no forward-decls of
game-side types. Consumes only RenderCore/Handle.h and
RenderObjectDesc.h. M1 surface: upsertStaticProp, destroy,
markVisible, isReady, frameBannerTick.

Spec: 2026-05-22-renderworld-boundary-spec.md section 4, 13"
```

### Task 6: Create `RenderWorld/RenderWorld.cpp` (thin forwarder)

**Files:**
- Create: `RenderWorld/RenderWorld.cpp`
- Create: `RenderWorld/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root) — `add_subdirectory(...)` + link

- [ ] **Step 1: Re-grep the registry signatures to confirm bit-for-bit match**

```bash
grep -n "registerRecipe\|invalidate\|markVisible\|isReady" GameOS/gameos/gos_static_prop_registry.h
```

Expected: 4 signatures present; signatures match the call sites in the
forwarder below. If a signature has drifted, adjust the forwarder body.

- [ ] **Step 2: Create `RenderWorld/RenderWorld.cpp`**

```cpp
// RenderWorld/RenderWorld.cpp
//
// Slice M1: thin forwarder. RenderWorld::upsertStaticProp routes into
// GpuStaticPropRegistry::registerRecipe; no new GPU behavior.
//
// Sentinel translation happens HERE on the engine side too: registry
// returns int32_t with -1 sentinel; we translate to RenderObjectHandle.
// The mirror translation (game-side -1 -> invalid()) happens in the
// adapter. Both endpoints translate so int32_t -1 cannot leak upward
// AND RenderObjectHandle::invalid() cannot leak downward.

#include "RenderWorld.h"

// C1 fix: this TU MUST NOT include gos_static_prop_batcher.h (it
// transitively pulls GL + Stuff). All translation between
// StaticPropInstanceDesc <-> GpuStaticPropInstance and all calls into
// GpuStaticPropRegistry::registerRecipe live in
// RenderWorld/legacy/static_prop_backend.cpp.
//
// We include the BACKEND-INTERNAL header, which exposes only the
// minimal bridge functions taking RenderCore types + int32_t recipe
// indices. The backend header itself does NOT include the batcher.
#include "legacy/static_prop_backend.h"

#include <atomic>
#include <cstdio>

namespace {

// Anonymous-namespace state per Decision D3.A. Adapter does NOT see
// these; the only public surface is RenderWorld:: free functions.
std::atomic<uint64_t> s_upsertOk{0};
std::atomic<uint64_t> s_upsertFail{0};
std::atomic<uint64_t> s_destroyCalls{0};
std::atomic<uint64_t> s_markVisibleCalls{0};
std::atomic<uint64_t> s_frameCounter{0};

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

uint32_t recipeIndexToHandleIndex(int32_t r) {
    // M1: generation is always 1 (no slot recycle yet). Index is the
    // raw recipe slot. -1 -> invalid (index=0, generation=0).
    if (r < 0) return 0;
    // 20-bit clamp; assert if registry ever overflows (it cannot under
    // current configuration; this is a future-proofing guard).
    return static_cast<uint32_t>(r) & 0x000FFFFFu;
}

int32_t handleToRecipeIndex(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return -1;
    return static_cast<int32_t>(h.index());
}

} // namespace

namespace RenderWorld {

void init() {
    s_upsertOk.store(0);
    s_upsertFail.store(0);
    s_destroyCalls.store(0);
    s_markVisibleCalls.store(0);
    s_frameCounter.store(0);
    std::fprintf(stderr, "[RENDER_WORLD v1] event=init\n");
}

void destroy() {
    std::fprintf(stderr,
        "[RENDER_WORLD v1] event=destroy upsert_ok=%llu upsert_fail=%llu "
        "destroy_calls=%llu mark_visible=%llu\n",
        (unsigned long long)s_upsertOk.load(),
        (unsigned long long)s_upsertFail.load(),
        (unsigned long long)s_destroyCalls.load(),
        (unsigned long long)s_markVisibleCalls.load());
}

RenderCore::RenderObjectHandle upsertStaticProp(RenderCore::StaticPropDesc desc) {
    // Delegate to the legacy backend TU. Backend performs translation
    // StaticPropInstanceDesc -> GpuStaticPropInstance and calls
    // GpuStaticPropRegistry::registerRecipe. RenderWorld.cpp itself
    // never includes the batcher header (C1).
    const int32_t r = legacy::registerStaticPropRecipe(std::move(desc));
    if (r < 0) {
        s_upsertFail.fetch_add(1, std::memory_order_relaxed);
        if (envFlag("MC2_RENDER_WORLD_TRACE")) {
            std::fprintf(stderr,
                "[RENDER_WORLD v1] event=upsert_fail recipe=-1\n");
        }
        return RenderCore::RenderObjectHandle::invalid();
    }
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(r), 1u);
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=upsert_ok recipe=%d handle.index=%u\n",
            r, (unsigned)h.index());
    }
    return h;
}

RenderCore::RenderObjectHandle adoptStaticPropRecipe(int32_t recipeIndex) {
    // m5 fix: wrap an existing registry slot in a Handle without
    // creating a new recipe entry. Counter is incremented so
    // [RENDER_WORLD v1] objects stays honest for the late-spawn path.
    if (recipeIndex < 0) {
        return RenderCore::RenderObjectHandle::invalid();
    }
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    return RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(recipeIndex), 1u);
}

void destroy(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return;
    legacy::invalidateStaticProp(handleToRecipeIndex(h));
    s_destroyCalls.fetch_add(1, std::memory_order_relaxed);
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=destroy handle.index=%u\n",
            (unsigned)h.index());
    }
}

void markVisible(RenderCore::RenderObjectHandle h,
                 uint32_t lightDataIndex, float extentRadius) {
    if (!h.isValid()) return;
    legacy::markVisibleStaticProp(handleToRecipeIndex(h),
                                  lightDataIndex, extentRadius);
    s_markVisibleCalls.fetch_add(1, std::memory_order_relaxed);
}

bool isReady(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return false;
    return legacy::isReadyStaticProp(handleToRecipeIndex(h));
}

void frameBannerTick() {
    const uint64_t f = s_frameCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool perFrame = envFlag("MC2_RENDER_WORLD_TRACE");
    const bool summary  = (f % 600u) == 0u;
    if (!perFrame && !summary) return;
    // m4 fix (adversarial review pass 2 2026-05-22): source the active
    // prop count from the registry's own active-recipe accessor, NOT
    // from the adapter-side upsert/destroy delta. The registry may
    // tombstone slots via code paths the adapter never sees; adapter
    // delta drifts; registry count is canonical.
    const uint64_t active = legacy::getStaticPropActiveCount();
    std::fprintf(stderr,
        "[RENDER_WORLD v1] frame=%llu objects=%llu visible=0 packets=0 views=1\n",
        (unsigned long long)f, (unsigned long long)active);
}

} // namespace RenderWorld
```

- [ ] **Step 3: Create `RenderWorld/CMakeLists.txt`**

```cmake
# RenderWorld/CMakeLists.txt
#
# Slice M1: static library. Engine API surface. Links against
# rendercore INTERFACE for types. NO game-side library link.

add_library(renderworld STATIC
    RenderWorld.cpp
)

target_link_libraries(renderworld PUBLIC rendercore)
target_include_directories(renderworld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Reach the registry header. This is the load-bearing carve-out:
# RenderWorld.cpp includes gos_static_prop_registry.h, which itself
# forward-declares `class Appearance;`. The firewall script
# allowlists that one header (see scripts/check-include-firewall.allowlist).
target_include_directories(renderworld PRIVATE
    ${CMAKE_SOURCE_DIR}/GameOS
)

target_compile_features(renderworld PUBLIC cxx_std_17)
```

- [ ] **Step 4: Wire in root `CMakeLists.txt`**

Insert `add_subdirectory("./RenderWorld" "./out/RenderWorld")` immediately
after the `RenderCore` line from Task 4.

Then locate the `target_link_libraries(mc2 ...)` block at root line 274
(per audit 2026-05-22; re-grep):

```bash
grep -n "target_link_libraries(mc2" CMakeLists.txt | head -3
```

Add `renderworld` to the list (alphabetical position next to `rendercore`
is fine; do not reorder existing entries). Example minimal patch:

```cmake
target_link_libraries(mc2 mclib fx_trace particles gosfx mlr stuff gui gameos gameos_main windows ZLIB::ZLIB ${SDL2_LIBRARIES} GLEW::GLEW SDL2_mixer::SDL2_mixer ${ADDITIONAL_LIBS} OpenGL::GL rendercore renderworld)
```

- [ ] **Step 5: Clean build (new TU added)**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
```

Expected: `RenderWorld.cpp` compiles, mc2.exe links. No call sites yet
exercise it; the only runtime evidence is `RenderWorld::init/destroy` —
but `init()` is not yet called (Task 13). Banner has no caller. Build
green is the entire signal.

- [ ] **Step 6: Commit**

```bash
git add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp RenderWorld/CMakeLists.txt CMakeLists.txt
git commit -m "feat(renderworld): add RenderWorld static lib (M1 Task 6)

Thin forwarder onto GpuStaticPropRegistry. Sentinel translation
-1 <-> Handle::invalid() at the engine boundary. Counter state
in anonymous namespace per Decision D3.A. No init() caller yet
(Task 13); links cleanly, runtime unreferenced.

Spec: 2026-05-22-renderworld-boundary-spec.md section 4, 10, 13"
```

### Task 6.5: Create `RenderWorld/legacy/static_prop_backend.{h,cpp}` (C1 fix)

**Files:**
- Create: `RenderWorld/legacy/static_prop_backend.h`
- Create: `RenderWorld/legacy/static_prop_backend.cpp`
- Modify: `RenderWorld/CMakeLists.txt` (add the new TU + new helper accessor visibility)

**C1 fix:** this is the ONLY engine-side TU that includes
`gos_static_prop_batcher.h` and calls into `GpuStaticPropRegistry::*`.
The header exposes a narrow bridge surface that takes RenderCore types
+ int32_t recipe indices — nothing in the public include path drags
GL or Stuff.

- [ ] **Step 1: Create `RenderWorld/legacy/static_prop_backend.h`**

```cpp
// RenderWorld/legacy/static_prop_backend.h
//
// Slice M1 (C1 fix): internal bridge between RenderWorld and the
// legacy GpuStaticPropRegistry. Header-only surface; the .cpp owns
// every include of gos_static_prop_batcher.h and every call into
// GpuStaticPropRegistry::*.
//
// This header MUST NOT be included from RenderCore/, GameAdapters/,
// or anywhere outside RenderWorld/. The firewall script grandfathers
// the path RenderWorld/legacy/ to keep these reach-throughs scoped.

#pragma once

#include <cstdint>
#include "../../RenderCore/RenderObjectDesc.h"

namespace RenderWorld {
namespace legacy {

// Engine-side translation seam: StaticPropInstanceDesc -> GpuStaticPropInstance.
// Calls GpuStaticPropRegistry::registerRecipe. Returns recipe index
// (>= 0 on success, -1 on disabled / OOM / empty batch).
int32_t registerStaticPropRecipe(RenderCore::StaticPropDesc desc);

// Forwarders. recipe index is int32_t at this layer; sentinel
// translation happens at the seams.
void invalidateStaticProp(int32_t recipeIndex);
void markVisibleStaticProp(int32_t recipeIndex,
                           uint32_t lightDataIndex,
                           float extentRadius);
bool isReadyStaticProp(int32_t recipeIndex);

// m4 fix: registry active-recipe count for [RENDER_WORLD v1] objects=.
// Reads GpuStaticPropRegistry::getActiveCount() (new accessor; see
// Task 6.5 Step 0 below).
uint64_t getStaticPropActiveCount();

} // namespace legacy
} // namespace RenderWorld
```

- [ ] **Step 0: Add `getActiveCount()` accessor to the registry header (m4 fix)**

The registry currently has no live-count accessor. Add a thin one
(read-only; reads the existing internal counter — find it via grep):

```bash
grep -n "registered\|active\|recipe_count\|s_recipes" GameOS/gameos/gos_static_prop_registry.cpp | head -20
```

Pick the canonical "live recipe slot count" expression. Add to
`GameOS/gameos/gos_static_prop_registry.h` (after `getStaticFirstFrameSkipCount()`):

```cpp
// m4 fix (RenderWorld Slice M1): live recipe count for
// [RENDER_WORLD v1] objects=N. Read-only; reflects current tombstone-
// adjusted active recipe slot count.
uint32_t getActiveCount();
```

And implement in `gos_static_prop_registry.cpp` as a thin wrapper over
whatever the existing internal counter is. Verify the implementation
is non-locking / atomic-load-safe (the banner is called from the
frame-end path; do not introduce a new lock contention point).

- [ ] **Step 2: Create `RenderWorld/legacy/static_prop_backend.cpp`**

```cpp
// RenderWorld/legacy/static_prop_backend.cpp
//
// Slice M1 (C1 fix): the ONLY engine-side TU that touches
// gos_static_prop_batcher.h and GpuStaticPropRegistry::*.

#include "static_prop_backend.h"

#include "../../GameOS/gameos/gos_static_prop_registry.h"
#include "../../GameOS/gameos/gos_static_prop_batcher.h"  // GpuStaticPropInstance

#include <type_traits>
#include <vector>

namespace {

// Size compatibility check between the POD mirror and the legacy struct.
// If the legacy struct grows or shrinks, this check fires at compile
// time and the executor must update StaticPropInstanceDesc to match.
static_assert(sizeof(RenderCore::StaticPropInstanceDesc) ==
              sizeof(GpuStaticPropInstance),
              "StaticPropInstanceDesc and GpuStaticPropInstance "
              "size mismatch; update RenderCore/StaticPropInstanceDesc.h.");

// Translate one element. Implementation form depends on whether the
// mirror was laid out byte-identical (memcpy is legal) or field-by-
// field (executor writes explicit assignments). The executor MUST
// document which form was chosen in the commit message.
inline GpuStaticPropInstance toEngine(
    const RenderCore::StaticPropInstanceDesc& src) {
    GpuStaticPropInstance dst;
    static_assert(std::is_trivially_copyable<GpuStaticPropInstance>::value,
                  "GpuStaticPropInstance must be trivially copyable for "
                  "memcpy bridge.");
    static_assert(std::is_trivially_copyable<
                      RenderCore::StaticPropInstanceDesc>::value,
                  "StaticPropInstanceDesc must be trivially copyable.");
    std::memcpy(&dst, &src, sizeof(dst));
    return dst;
}

} // namespace

namespace RenderWorld {
namespace legacy {

int32_t registerStaticPropRecipe(RenderCore::StaticPropDesc desc) {
    if (desc.shape == nullptr || desc.batch.empty()) {
        return -1;
    }
    // Translate POD-mirror batch -> engine batch. One allocation; same
    // as the legacy path's allocation footprint.
    std::vector<GpuStaticPropInstance> engineBatch;
    engineBatch.reserve(desc.batch.size());
    for (const auto& src : desc.batch) {
        engineBatch.push_back(toEngine(src));
    }
    return GpuStaticPropRegistry::registerRecipe(desc.shape, engineBatch);
}

void invalidateStaticProp(int32_t recipeIndex) {
    GpuStaticPropRegistry::invalidate(recipeIndex);
}

void markVisibleStaticProp(int32_t recipeIndex,
                           uint32_t lightDataIndex,
                           float extentRadius) {
    GpuStaticPropRegistry::markVisible(recipeIndex,
                                       lightDataIndex,
                                       extentRadius);
}

bool isReadyStaticProp(int32_t recipeIndex) {
    return GpuStaticPropRegistry::isReady(recipeIndex);
}

uint64_t getStaticPropActiveCount() {
    return static_cast<uint64_t>(GpuStaticPropRegistry::getActiveCount());
}

} // namespace legacy
} // namespace RenderWorld
```

- [ ] **Step 3: Update `RenderWorld/CMakeLists.txt`**

Append the new TU:

```cmake
add_library(renderworld STATIC
    RenderWorld.cpp
    legacy/static_prop_backend.cpp
)
```

- [ ] **Step 4: Add `registerStaticPropAndReturnRecipe` to the registry (m5 fix)**

Append to `GameOS/gameos/gos_static_prop_registry.h`:

```cpp
// m5 fix (RenderWorld Slice M1): late-spawn path needs a real recipe
// index back so the adapter can produce a RenderObjectHandle.
// Returns the recipe index that registerStaticProp() created, or -1
// on failure / not eligible. registerStaticProp(Appearance*) is
// preserved for backward compat but no longer called from the
// worktree once Task 12 migrates warrior.cpp.
int32_t registerStaticPropAndReturnRecipe(Appearance* app);
```

And implement in `gos_static_prop_registry.cpp` (grep for the existing
`registerStaticProp(Appearance*)` body; the new function calls into the
same `Appearance::registerStatic()` path but reads back the recipe index
via `app->getStaticRecipeIndex()` or equivalent — confirm the accessor
name at write time).

- [ ] **Step 5: Clean build**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
```

Expected: build succeeds. `static_prop_backend.cpp` compiles cleanly;
the two `static_assert`s on size + trivially-copyable pass. mc2.exe
links unchanged (no callers exercise the legacy backend yet; Task 7's
adapter is the first consumer).

- [ ] **Step 6: Commit**

```bash
git add RenderWorld/legacy/static_prop_backend.h RenderWorld/legacy/static_prop_backend.cpp RenderWorld/CMakeLists.txt GameOS/gameos/gos_static_prop_registry.h GameOS/gameos/gos_static_prop_registry.cpp
git commit -m "feat(renderworld): legacy backend bridge + registry accessors (M1 Task 6.5)

C1 fix: the ONLY engine-side TU that touches
gos_static_prop_batcher.h and GpuStaticPropRegistry::*.
RenderWorld.cpp delegates upsert / destroy / markVisible /
isReady / active-count through this bridge.

m4 fix: getActiveCount() accessor added to registry; banner
sources count from registry, not adapter delta.

m5 fix: registerStaticPropAndReturnRecipe(Appearance*) added
for the late-spawn path so the adapter can produce a real
RenderObjectHandle.

Spec: 2026-05-22-renderworld-boundary-spec.md section 10 + amendment"
```

### Task 7: Create `GameAdapters/StaticPropRenderAdapter.{h,cpp}` (no callers yet)

**Files:**
- Create: `GameAdapters/StaticPropRenderAdapter.h`
- Create: `GameAdapters/StaticPropRenderAdapter.cpp`
- Create: `GameAdapters/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root)

- [ ] **Step 1: Create `GameAdapters/StaticPropRenderAdapter.h`**

```cpp
// GameAdapters/StaticPropRenderAdapter.h
//
// Slice M1: the ONLY module that may bridge gameData and engine sides.
// Per spec section 12 carve-out, an adapter header may forward-declare
// game-side types; the .cpp may include real game-side headers.
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 10 (legacy adapter migration), section 13 (M1 scope).
//
// Sentinel translation rule (load-bearing, spec section 10):
//   -1 (recipeIndex)  <->  RenderCore::RenderObjectHandle::invalid()
// MUST happen at this boundary, both directions. The int32_t MUST NOT
// leak upward; the Handle MUST NOT leak downward into the registry.

#pragma once

#include <cstddef>
#include <cstdint>

#include "../RenderCore/Handle.h"

// Forward-decl game-side base. Spec section 12 carve-out: adapter
// HEADERS may forward-decl; adapter .cpp may include real header.
class Appearance;
class TG_MultiShape;
struct GpuStaticPropInstance;  // adapter .cpp translates via the POD mirror

namespace GameAdapters {
namespace StaticProp {

// M1 fix (adversarial review pass 2): spec D3 entry-point names.
//
// Per-mission lifecycle. Wired at code/mission.cpp:1693 and :3279
// alongside GpuStaticPropRegistry::init/destroy (m1 fix).
void beginMission();
void endMission();

// M1 surface: building / tree first-render and bulk-register sites.
// Returns invalid() on failure (registry disabled, OOM, or batch empty).
//
// Caller MUST persist the returned handle alongside its existing
// staticReg.recipeIndex (M1 keeps the int32_t storage; see Plan
// Decision D4). The legacyRecipeIndexOut OUT parameter exists for the
// transitional period: bdactor.cpp's staticReg.recipeIndex field stays
// int32_t in M1 (slot-side storage; not a public boundary). The adapter
// writes the legacy sentinel value there; the Handle is returned
// for any future upward consumer.
RenderCore::RenderObjectHandle syncStaticProp(
    TG_MultiShape* shape,
    const GpuStaticPropInstance* batchData,
    size_t batchCount,
    int32_t* legacyRecipeIndexOut);  // may be nullptr

// m5 fix: late-spawn entry point (warrior.cpp:7593 path). Uses the
// new GpuStaticPropRegistry::registerStaticPropAndReturnRecipe() to
// obtain the recipe index, then calls
// RenderWorld::adoptStaticPropRecipe() to wrap it in a real Handle
// (so the [RENDER_WORLD v1] counter stays honest).
//
// Returns invalid() on failure. legacyRecipeIndexOut receives the
// raw recipe index (or -1) for any caller that still tracks the
// legacy storage type.
RenderCore::RenderObjectHandle syncStaticPropLateSpawn(
    Appearance* app,
    int32_t* legacyRecipeIndexOut);  // may be nullptr

// Destroy / invalidate. Mirrors GpuStaticPropRegistry::invalidate but
// goes through RenderWorld so counters and sentinel translation stay
// centralized. Currently unused in M1 (the bdactor sites that
// invalidate do so via separate paths; promotion to this API is a
// future-slice cleanup). Declared here so the surface is shape-complete.
void destroyStaticProp(RenderCore::RenderObjectHandle h);

} // namespace StaticProp
} // namespace GameAdapters
```

- [ ] **Step 2: Create `GameAdapters/StaticPropRenderAdapter.cpp`**

```cpp
// GameAdapters/StaticPropRenderAdapter.cpp
//
// Slice M1: the bridge .cpp. Includes BOTH sides.

#include "StaticPropRenderAdapter.h"

// Engine side.
#include "../RenderWorld/RenderWorld.h"
#include "../RenderCore/RenderObjectDesc.h"
#include "../RenderCore/StaticPropInstanceDesc.h"
// m5 fix: late-spawn path uses the new return-recipe accessor.
#include "../GameOS/gameos/gos_static_prop_registry.h"
// C1 fix: adapter is one of the two seams for POD-mirror translation;
// it MUST include the batcher to translate GpuStaticPropInstance ->
// StaticPropInstanceDesc.
#include "../GameOS/gameos/gos_static_prop_batcher.h"

// Game side. Per spec section 12 + adversarial M2 carve-out, this is
// the ONLY .cpp where game-side headers may be reached.
#include "../mclib/appear.h"

#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

} // namespace

namespace {

// C1 fix: game-side seam for POD-mirror translation. Engine-side seam
// lives in RenderWorld/legacy/static_prop_backend.cpp. Both perform
// the same translation; both have static_asserts guarding size match.
static_assert(sizeof(RenderCore::StaticPropInstanceDesc) ==
              sizeof(GpuStaticPropInstance),
              "StaticPropInstanceDesc / GpuStaticPropInstance size "
              "mismatch; update RenderCore/StaticPropInstanceDesc.h.");

inline RenderCore::StaticPropInstanceDesc toMirror(
    const GpuStaticPropInstance& src) {
    RenderCore::StaticPropInstanceDesc dst;
    static_assert(std::is_trivially_copyable<GpuStaticPropInstance>::value,
                  "GpuStaticPropInstance must be trivially copyable.");
    std::memcpy(&dst, &src, sizeof(dst));
    return dst;
}

} // namespace

namespace GameAdapters {
namespace StaticProp {

// Per-mission lifecycle. m1 fix: wired at mission.cpp:1693 / :3279.
// Sibling of GpuStaticPropRegistry::init/destroy (per-mission, NOT
// per-process). Currently a thin pair around RenderWorld::init/destroy;
// promoted to real boundary calls in M2+ (see spec section 4).
void beginMission() {
    RenderWorld::init();
}

void endMission() {
    RenderWorld::destroy();
}

RenderCore::RenderObjectHandle syncStaticProp(
    TG_MultiShape* shape,
    const GpuStaticPropInstance* batchData,
    size_t batchCount,
    int32_t* legacyRecipeIndexOut) {

    if (shape == nullptr || batchCount == 0) {
        if (legacyRecipeIndexOut) *legacyRecipeIndexOut = -1;
        return RenderCore::RenderObjectHandle::invalid();
    }

    // C1 fix: translate engine struct -> POD mirror. The desc going
    // across the RenderWorld boundary holds the mirror; the legacy
    // backend TU translates back when calling registerRecipe.
    RenderCore::StaticPropDesc desc;
    desc.shape = shape;
    desc.batch.reserve(batchCount);
    for (size_t i = 0; i < batchCount; ++i) {
        desc.batch.push_back(toMirror(batchData[i]));
    }

    // Route through RenderWorld (engine boundary). RenderWorld.cpp
    // performs sentinel translation -> handle.
    RenderCore::RenderObjectHandle h =
        RenderWorld::upsertStaticProp(std::move(desc));

    // Sentinel translation for the legacy out-parameter, both directions.
    // M3 (deferred to spec amendment): the boundary has TWO seams; this
    // one + the engine-side legacy backend each translate independently.
    if (legacyRecipeIndexOut) {
        *legacyRecipeIndexOut =
            h.isValid() ? static_cast<int32_t>(h.index()) : -1;
    }

    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=adapter_sync_static "
            "shape=%p batch=%zu handle.valid=%d legacy=%d\n",
            (void*)shape, batchCount, (int)h.isValid(),
            legacyRecipeIndexOut ? *legacyRecipeIndexOut : -2);
    }
    return h;
}

RenderCore::RenderObjectHandle syncStaticPropLateSpawn(
    Appearance* app,
    int32_t* legacyRecipeIndexOut) {

    if (!app) {
        if (legacyRecipeIndexOut) *legacyRecipeIndexOut = -1;
        return RenderCore::RenderObjectHandle::invalid();
    }

    // m5 fix: use the new return-recipe accessor so the adapter can
    // adopt the recipe into a real Handle. The legacy registerStaticProp
    // (bool) is preserved for backward compat but NOT used here.
    const int32_t recipe =
        GpuStaticPropRegistry::registerStaticPropAndReturnRecipe(app);

    RenderCore::RenderObjectHandle h =
        RenderWorld::adoptStaticPropRecipe(recipe);

    if (legacyRecipeIndexOut) {
        *legacyRecipeIndexOut = recipe;
    }

    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=adapter_sync_late "
            "app=%p recipe=%d handle.valid=%d\n",
            (void*)app, recipe, (int)h.isValid());
    }
    return h;
}

void destroyStaticProp(RenderCore::RenderObjectHandle h) {
    RenderWorld::destroy(h);
}

} // namespace StaticProp
} // namespace GameAdapters
```

- [ ] **Step 3: Create `GameAdapters/CMakeLists.txt`**

```cmake
# GameAdapters/CMakeLists.txt

add_library(gameadapters STATIC
    StaticPropRenderAdapter.cpp
)

target_link_libraries(gameadapters PUBLIC rendercore renderworld)

# Adapter .cpp reaches game-side headers. This is the ONLY library
# where the firewall script allows such reach-through.
target_include_directories(gameadapters PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/mclib
    ${CMAKE_SOURCE_DIR}/GameOS
)

target_include_directories(gameadapters PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(gameadapters PUBLIC cxx_std_17)
```

- [ ] **Step 4: Wire in root `CMakeLists.txt`**

Insert `add_subdirectory("./GameAdapters" "./out/GameAdapters")` after
the `RenderWorld` line. Add `gameadapters` to the `target_link_libraries(mc2 ...)`
list at root line 274 (re-grep). Resulting link line target list now ends:
`... rendercore renderworld gameadapters`.

- [ ] **Step 5: Clean build**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
```

Expected: gameadapters compiles (includes appear.h cleanly via the
PRIVATE mclib include dir). mc2.exe links unchanged. Runtime behavior
identical because no call sites use the adapter yet.

- [ ] **Step 6: Commit**

```bash
git add GameAdapters/StaticPropRenderAdapter.h GameAdapters/StaticPropRenderAdapter.cpp GameAdapters/CMakeLists.txt CMakeLists.txt
git commit -m "feat(renderworld): add StaticPropRenderAdapter (M1 Task 7)

The ONLY TU that bridges game-side mclib/appear.h with engine-side
RenderWorld. Sentinel translation -1 <-> Handle::invalid() exclusively
here. No call sites wired yet (Tasks 8-12); links clean, runtime
unreferenced.

Spec: 2026-05-22-renderworld-boundary-spec.md section 10, 12, 13"
```

### Phase A gate

After Task 7 (Tasks 1, 2, 3, 4, 5, 6, 6.5, 7 complete):

- [ ] **Run canonical smoke**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS, exit 0. No `[RENDER_WORLD v1]` lines yet
(init not wired). `[STATIC_PROP_REGISTRY v1]` counts unchanged vs.
pre-slice baseline (capture HEAD pre-Phase-A as `baseline.log` for
diffing; instructions in Task 16).

Phase A success criterion: zero pixel delta, zero log diff in
`[STATIC_PROP_REGISTRY v1]` summary, build clean. If any fails, STOP
and diagnose before Phase B.

---

## Phase B — Adapter wiring (one call site at a time)

**Phase B goal:** route each of the 5 audited call sites through
`GameAdapters::StaticPropRenderAdapter`. Each task is a single-site
swap with its own smoke gate. Half-migrated state IS allowed BETWEEN
tasks within Phase B (one site at a time); it is NOT allowed AT phase
close (all 5 sites must be wired before Phase C starts).

**Rollback per task:** each site is an isolated patch; reverting one
commit restores the legacy direct-into-registry path at that site
while leaving the other migrated sites intact. The plan is therefore
robust to per-site rollback.

### Task 8: Wire BldgAppearance first-render fallback (bdactor.cpp:1471)

**Files:**
- Modify: `mclib/bdactor.cpp` around line 1471

- [ ] **Step 1: Re-grep the call site**

```bash
grep -n "GpuStaticPropRegistry::registerRecipe" mclib/bdactor.cpp
```

Expected: 4 hits. The first one is at or near line 1471 (per audit
2026-05-22). If drift, update the line numbers below.

- [ ] **Step 2: Add include**

Near the top of `mclib/bdactor.cpp`, after the existing
`#include "../GameOS/gameos/gos_static_prop_registry.h"` (verify via
`grep -n "gos_static_prop_registry" mclib/bdactor.cpp`), add:

```cpp
#include "../GameAdapters/StaticPropRenderAdapter.h"
```

- [ ] **Step 3: Replace the call (C2 fix: preserve H4 follow-up)**

Read the FULL block at `mclib/bdactor.cpp:1465-1483` (verified at audit
write time 2026-05-22; re-grep at edit time):

Existing (per audit):

```cpp
if (submittedToGpu && !staticReg.registered
        && GpuStaticPropRegistry::isEnabled()
        && !needsFullBakeNextFrame
        && isStaticEligible()) {
    const auto& batch =
        GpuStaticPropBatcher::instance().getLastBuiltBatch();
    staticReg.recipeIndex = GpuStaticPropRegistry::registerRecipe(
        bldgShape, batch);
    staticReg.registered  = (staticReg.recipeIndex >= 0);
    staticReg.shape       = bldgShape;
    if (staticReg.registered) {
        // H4 follow-up (2026-05-07): per-frame re-registration
        // after damage/shape swap has the same lightData_ gap as
        // mission-load registerStatic(). Force one full update()
        // so touch() cannot resubmit default-zero lightData_.
        // Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
        needsFullBakeNextFrame = true;
    }
}
```

Replace with (M1 fix names; H4 follow-up PRESERVED — C2 fix):

```cpp
if (submittedToGpu && !staticReg.registered
        && GpuStaticPropRegistry::isEnabled()
        && !needsFullBakeNextFrame
        && isStaticEligible()) {
    const auto& batch =
        GpuStaticPropBatcher::instance().getLastBuiltBatch();
    // M1 RenderWorld route (Slice M1 Task 8). Adapter performs
    // sentinel translation; staticReg.recipeIndex remains int32_t
    // per plan Decision D4 (slot-side storage stays legacy in M1).
    int32_t legacyIdx = -1;
    (void)GameAdapters::StaticProp::syncStaticProp(
        bldgShape, batch.data(), batch.size(), &legacyIdx);
    staticReg.recipeIndex = legacyIdx;
    staticReg.registered  = (staticReg.recipeIndex >= 0);
    staticReg.shape       = bldgShape;
    if (staticReg.registered) {
        // H4 follow-up (2026-05-07) — PRESERVE. C2 fix: failing
        // to keep this block regresses the 2026-05-07 black-actor
        // fix. Per-frame re-registration after damage/shape swap
        // has the same lightData_ gap as mission-load registerStatic();
        // force one full update() so touch() cannot resubmit
        // default-zero lightData_.
        // Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
        needsFullBakeNextFrame = true;
    }
}
```

**PRESERVE H4 `needsFullBakeNextFrame = true` follow-up;** failing to
do so regresses the 2026-05-07 black-actor / black-tree fix. Grep-
verify the surrounding lines at edit time:

```bash
grep -n "needsFullBakeNextFrame\|H4 follow-up" mclib/bdactor.cpp | head -20
```

Expected: hits at the audit lines listed in the file structure section.

- [ ] **Step 4: Full relink (touching mclib/bdactor.cpp)**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
```

- [ ] **Step 5: Smoke gate**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: 5/5 PASS. `[STATIC_PROP_REGISTRY v1]` counts unchanged. If
`MC2_RENDER_WORLD_TRACE=1` re-run (optional dev-only confirmation): a
non-zero count of `event=adapter_register_recipe` lines appears.

- [ ] **Step 6: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(renderworld): route BldgAppearance first-render via adapter (M1 Task 8)

bdactor.cpp:1471 - first-render fallback path now routes
through GameAdapters::StaticProp::syncStaticProp. staticReg.recipeIndex
storage type unchanged (int32_t) per plan Decision D4; adapter
performs sentinel translation at the boundary.

Tier1 5/5 PASS. [STATIC_PROP_REGISTRY v1] counts unchanged.

Spec: 2026-05-22-renderworld-boundary-spec.md section 13"
```

### Task 9: Wire BldgAppearance bulk-register (bdactor.cpp:2802)

**Files:**
- Modify: `mclib/bdactor.cpp` around line 2802

- [ ] **Step 1: Re-grep**

```bash
grep -n "GpuStaticPropRegistry::registerRecipe(bldgShape, batch)" mclib/bdactor.cpp
```

Expected: one hit (the line-1471 site is now adapter-routed). If still
two hits, line 1471 conversion was incomplete; revisit Task 8.

- [ ] **Step 2: Replace the call (preserve H4 (2026-05-06) follow-up)**

Existing (per audit, full block at bdactor.cpp:2802-2819):

```cpp
const int32_t regIdx = GpuStaticPropRegistry::registerRecipe(bldgShape, batch);
if (regIdx >= 0) {
    staticReg.registered  = true;
    staticReg.shape       = bldgShape;
    staticReg.recipeIndex = regIdx;
    // H4 fix (2026-05-06): registerStatic only ran TransformMultiShape_BuildRecipe
    // (positions only); leaf TG_Shape::lightData_ is still default/zero.
    // ... (full comment preserved at edit time) ...
    needsFullBakeNextFrame = true;
}
```

Replace with (H4 block PRESERVED):

```cpp
int32_t regIdx = -1;
(void)GameAdapters::StaticProp::syncStaticProp(
    bldgShape, batch.data(), batch.size(), &regIdx);
if (regIdx >= 0) {
    staticReg.registered  = true;
    staticReg.shape       = bldgShape;
    staticReg.recipeIndex = regIdx;
    // H4 fix (2026-05-06) — PRESERVE verbatim per C2 fix. Removing
    // this block regresses black-actor / black-tree at mission load.
    // Spec: docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md
    needsFullBakeNextFrame = true;
}
```

- [ ] **Step 3: Full relink + smoke**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: 5/5 PASS. Counts unchanged.

- [ ] **Step 4: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(renderworld): route BldgAppearance bulk-register via adapter (M1 Task 9)

bdactor.cpp:2802 - mission-load bulk-register path routes through
GameAdapters::StaticProp::syncStaticProp. H4 2026-05-06 follow-up
(needsFullBakeNextFrame=true) preserved per C2 fix. Counts unchanged.

Spec: 2026-05-22-renderworld-boundary-spec.md section 13"
```

### Task 10: Wire TreeAppearance first-render fallback (bdactor.cpp:4269)

**Files:**
- Modify: `mclib/bdactor.cpp` around line 4269

- [ ] **Step 1: Re-grep the FULL block at bdactor.cpp:4264-4281 (C2 fix)**

```bash
grep -n "needsFullBakeNextFrame\|H4 follow-up" mclib/bdactor.cpp | head -20
```

Confirm the H4 follow-up surrounding lines are at audit positions
4273-4280 (TreeAppearance equivalent of Task 8's BldgAppearance
follow-up).

Existing (per audit):

```cpp
if (submittedToGpu && !staticReg.registered
        && GpuStaticPropRegistry::isEnabled()
        && !needsFullBakeNextFrame) {
    const auto& batch =
        GpuStaticPropBatcher::instance().getLastBuiltBatch();
    staticReg.recipeIndex = GpuStaticPropRegistry::registerRecipe(
        treeShape, batch);
    staticReg.registered  = (staticReg.recipeIndex >= 0);
    staticReg.shape        = treeShape;
    if (staticReg.registered) {
        // H4 follow-up (2026-05-07): per-frame re-registration
        // after LOD/shape swap has the same lightData_ gap as
        // mission-load registerStatic(). Force one full update()
        // so touch() cannot resubmit default-zero lightData_.
        // Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
        needsFullBakeNextFrame = true;
    }
}
```

Replace with (M1 names; H4 follow-up PRESERVED — C2 fix):

```cpp
if (submittedToGpu && !staticReg.registered
        && GpuStaticPropRegistry::isEnabled()
        && !needsFullBakeNextFrame) {
    const auto& batch =
        GpuStaticPropBatcher::instance().getLastBuiltBatch();
    int32_t legacyIdx = -1;
    (void)GameAdapters::StaticProp::syncStaticProp(
        treeShape, batch.data(), batch.size(), &legacyIdx);
    staticReg.recipeIndex = legacyIdx;
    staticReg.registered  = (staticReg.recipeIndex >= 0);
    staticReg.shape       = treeShape;
    if (staticReg.registered) {
        // H4 follow-up (2026-05-07) — PRESERVE verbatim per C2 fix.
        // Removing this block regresses 2026-05-07 black-tree fix.
        // Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
        needsFullBakeNextFrame = true;
    }
}
```

**PRESERVE H4 `needsFullBakeNextFrame = true` follow-up;** failing to
do so regresses the 2026-05-07 black-tree fix.

- [ ] **Step 2: Full relink + smoke**

(Same commands as Task 9.)

- [ ] **Step 3: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(renderworld): route TreeAppearance first-render via adapter (M1 Task 10)

bdactor.cpp:4269. Counts unchanged.

Spec: 2026-05-22-renderworld-boundary-spec.md section 13"
```

### Task 11: Wire TreeAppearance bulk-register (bdactor.cpp:4855)

**Files:**
- Modify: `mclib/bdactor.cpp` around line 4855

- [ ] **Step 1: Re-grep and replace**

Existing:

```cpp
const int32_t regIdx = GpuStaticPropRegistry::registerRecipe(treeShape, batch);
if (regIdx >= 0) {
    staticReg.registered  = true;
    staticReg.shape       = treeShape;
    staticReg.recipeIndex = regIdx;
    // H4 fix (2026-05-06) ... needsFullBakeNextFrame = true;
}
```

Replace pattern identical to Task 9, with `treeShape`, using
`GameAdapters::StaticProp::syncStaticProp(...)`. PRESERVE the H4
follow-up block verbatim — C2 fix.

- [ ] **Step 2: Full relink + smoke**

(Same commands.)

- [ ] **Step 3: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(renderworld): route TreeAppearance bulk-register via adapter (M1 Task 11)

bdactor.cpp:4855. With this commit, all 4 bdactor sites are
adapter-routed. Counts unchanged.

Spec: 2026-05-22-renderworld-boundary-spec.md section 13"
```

### Task 12: Wire warrior.cpp late-spawn (warrior.cpp:7593)

**Files:**
- Modify: `code/warrior.cpp` around line 7593

- [ ] **Step 1: Re-grep**

```bash
grep -n "GpuStaticPropRegistry::registerStaticProp" code/warrior.cpp
```

Expected: one hit at line 7593 (per audit). Verify.

- [ ] **Step 2: Add include**

```bash
grep -n "gos_static_prop_registry" code/warrior.cpp
```

The existing include at `code/warrior.cpp:110` for the registry header
stays. ADD immediately below it:

```cpp
#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 Task 12
```

- [ ] **Step 3: Replace the call**

Existing (per audit):

```cpp
// Task 6 (Track B): late-spawn registration. Waypoint marker has position
// set and full init completed - eligible for early static-prop registration.
// Gated on MC2_STATIC_PROP_LATE_SPAWN_REG; no-op when disabled.
GpuStaticPropRegistry::registerStaticProp(appearance);
```

Replace with:

```cpp
// Task 6 (Track B): late-spawn registration. M1 Slice (Task 12)
// routes through GameAdapters::StaticProp::syncStaticPropLateSpawn.
// m5 fix: adapter returns a real RenderObjectHandle (via
// adoptStaticPropRecipe) so the [RENDER_WORLD v1] objects counter
// stays honest. Handle is unused at this call site; legacy recipe
// index propagated via OUT param for any diagnostic that needs it.
int32_t lateRecipe = -1;
(void)GameAdapters::StaticProp::syncStaticPropLateSpawn(
    appearance, &lateRecipe);
```

- [ ] **Step 4: Full relink + smoke**

(Same commands as Task 9. Note: `code/warrior.cpp` is large; relink
takes time. Do not skip the `rm mc2.exe` step — warrior.cpp object
churn is a documented full-relink trigger in CLAUDE.md.)

- [ ] **Step 5: Commit**

```bash
git add code/warrior.cpp
git commit -m "feat(renderworld): route warrior late-spawn via adapter (M1 Task 12)

warrior.cpp:7593 - waypoint-marker late-spawn registration now
routes through GameAdapters::StaticProp::syncStaticPropLateSpawn. With
this commit, all 5 audited M1 call sites are adapter-routed.
Phase B closed. Counts unchanged.

Spec: 2026-05-22-renderworld-boundary-spec.md section 13"
```

### Phase B gate

After Task 12:

- [ ] **Verify zero direct-into-registry callers remain (outside the adapter and Phase 1 grandfathered TUs)**

```bash
grep -rn "GpuStaticPropRegistry::registerRecipe" --include="*.cpp" --include="*.h" \
    --exclude-dir=GameAdapters --exclude-dir=RenderWorld --exclude-dir=GameOS \
    A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/
```

Expected: ZERO hits in `mclib/` or `code/`. Any remaining hit is a
missed call site; do not advance to Phase C until resolved.

```bash
grep -rn "GpuStaticPropRegistry::registerStaticProp" --include="*.cpp" --include="*.h" \
    --exclude-dir=GameAdapters --exclude-dir=RenderWorld --exclude-dir=GameOS \
    A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/
```

Expected: ZERO hits.

- [ ] **Negative-direction grep (per CLAUDE.md "negative claims need opposite-direction grep")**

To prove "all bdactor producers are routed through the adapter," grep
for the adapter symbol itself in bdactor:

```bash
grep -n "GameAdapters::StaticProp::syncStaticProp" mclib/bdactor.cpp
```

Expected: 4 hits (one per site). If fewer, a site was missed.

H4-preservation grep (C2 fix): verify both follow-up blocks survived:

```bash
grep -n "H4 follow-up\|H4 fix" mclib/bdactor.cpp
```

Expected: at least 3 hits at lines near 1475 (Task 8 BldgAppearance
first-render), 2807 (Task 9 BldgAppearance bulk), 4274 (Task 10
TreeAppearance first-render); plus any others in TreeAppearance bulk.
ZERO hits = C2 regression, STOP and restore the H4 blocks.

- [ ] **Smoke gate (full tier1)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: 5/5 PASS. Phase B is closed only when this passes.

---

## Phase C — Include firewall enforcement

**Phase C goal:** lock in the boundary by adding a grep-based pre-commit
check. Phase 1 enforcement only (per spec): the script flags violations;
CI gate (Phase 3) is a future slice. No runtime assertions in M1.

### Task 13: Wire `GameAdapters::StaticProp::beginMission/endMission` (per-mission lifecycle)

**Files:**
- Modify: `code/mission.cpp` (at audit lines 1693 and 3279, sibling of `GpuStaticPropRegistry::init/destroy`)

**m1 fix (adversarial review pass 2 2026-05-22):** lifecycle is
PER-MISSION, NOT per-process. Hook at `code/mission.cpp:1693`
(beginMission, next to `GpuStaticPropRegistry::init()`) and
`code/mission.cpp:3279` (endMission, next to
`GpuStaticPropRegistry::destroy()`). DO NOT wire at
`GameOS/gameos/gameosmain.cpp` — the earlier draft hint was incorrect.

- [ ] **Step 1: Re-grep the hook sites**

```bash
grep -n "GpuStaticPropRegistry::init\|GpuStaticPropRegistry::destroy" code/mission.cpp
```

Expected: two hits at lines 1693 and 3279 (per audit 2026-05-22).
If drift, update offsets but keep the symbol-adjacency rule (the new
calls go on the line immediately following the existing init/destroy).

- [ ] **Step 2: Add include and calls**

Near the top of `code/mission.cpp` (grep for existing
`gos_static_prop_registry` include first to confirm the right include
block):

```cpp
#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 Task 13
```

At line 1693 (immediately AFTER `GpuStaticPropRegistry::init()`):

```cpp
GpuStaticPropRegistry::init();   // Stage 3.C
GameAdapters::StaticProp::beginMission();  // M1 Task 13
```

At line 3279 (immediately AFTER `GpuStaticPropRegistry::destroy()`):

```cpp
GpuStaticPropRegistry::destroy(); // Stage 3.C
GameAdapters::StaticProp::endMission();    // M1 Task 13
```

The adapter's `beginMission`/`endMission` forwards to
`RenderWorld::init()`/`destroy()` per Task 7 .cpp body.

- [ ] **Step 2: Full relink + smoke**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: 5/5 PASS. Check artifacts log for `[RENDER_WORLD v1] event=init`
exactly once per mission and `event=destroy` exactly once.

- [ ] **Step 3: Commit**

```bash
git add code/mission.cpp
git commit -m "feat(renderworld): wire begin/endMission (M1 Task 13)

m1 fix: per-mission lifecycle wired at code/mission.cpp:1693 and
:3279 (sibling of GpuStaticPropRegistry::init/destroy), NOT in
gameosmain.cpp. Adapter forwards to RenderWorld::init/destroy.
[RENDER_WORLD v1] event=init/destroy banners now emit per mission.

Spec: 2026-05-22-renderworld-boundary-spec.md section 11"
```

### Task 14: Wire `frameBannerTick` into the frame loop

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (immediately AFTER `gos_RendererEndFrame()` at line 541; m2 fix)

**m2 fix (adversarial review pass 2 2026-05-22):** the banner emit
goes immediately AFTER the `gos_RendererEndFrame()` call in
`GameOS/gameos/gameosmain.cpp:541` — that is where the per-frame
count is final. Do NOT "mirror frameBegin"; frame-end is the only
correct emit site.

- [ ] **Step 1: Re-grep the hook site**

```bash
grep -n "gos_RendererEndFrame" GameOS/gameos/gameosmain.cpp
```

Expected: declaration at line 149, single call site at line 541
(per audit 2026-05-22; re-verify at edit time).

- [ ] **Step 2: Add include and call**

Near the existing `extern void gos_RendererEndFrame();` declaration
(line 149), add:

```cpp
#include "../../RenderWorld/RenderWorld.h"  // M1 Task 14
```

At line 541 (immediately AFTER the `gos_RendererEndFrame()` call):

```cpp
{ ZoneScopedN("Camera.UpdateRenderers gos_RendererEndFrame"); gos_RendererEndFrame(); }
RenderWorld::frameBannerTick();  // M1 Task 14 (m2 fix: post-EndFrame)
```

- [ ] **Step 3: Full relink + smoke**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: 5/5 PASS. Artifacts log shows `[RENDER_WORLD v1] frame=600 objects=N`
at roughly the 10-second mark (assuming ~60fps; 30s tier1 yields one
or two summary lines per mission). `N` (the objects count) MUST be
nonzero and stable across missions that load static props.

Optional confirmation: re-run with `MC2_RENDER_WORLD_TRACE=1`:

```powershell
$env:MC2_RENDER_WORLD_TRACE = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
$env:MC2_RENDER_WORLD_TRACE = $null
```

Expected: many `event=adapter_register_recipe` and `event=upsert_ok`
lines; the per-frame banner now fires every frame.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(renderworld): emit [RENDER_WORLD v1] frame banner (M1 Task 14)

m2 fix: frameBannerTick wired immediately AFTER gos_RendererEndFrame
in gameosmain.cpp:541 (where the per-frame count is final).
Default emits the 600-frame summary; MC2_RENDER_WORLD_TRACE=1
enables per-frame.

m4 fix: objects=N sources from GpuStaticPropRegistry::getActiveCount()
(via the legacy backend bridge), NOT the adapter-side upsert/destroy
delta. visible=, packets=, views= are 0/0/1 in M1; promoted in M2+.

Spec: 2026-05-22-renderworld-boundary-spec.md section 11"
```

### Task 15: Create `scripts/check-include-firewall.sh` and allowlist

**Files:**
- Create: `scripts/check-include-firewall.sh`
- Create: `scripts/check-include-firewall.allowlist`

- [ ] **Step 1: Create the allowlist file**

```
# scripts/check-include-firewall.allowlist
#
# Phase 1 grandfathered carve-outs per adversarial review M2.
# Each line: relative-path-from-worktree-root
#
# Grandfathered: the registry header forward-declares `class Appearance;`
# at line 9 for the legacy registerStaticProp(Appearance*) signature.
# Spec section 12 carve-out applies for the duration of Phase 1.
GameOS/gameos/gos_static_prop_registry.h
GameOS/gameos/gos_static_prop_registry.cpp

# C1 fix: the legacy backend bridge TU is the ONLY engine-side TU
# allowed to include gos_static_prop_batcher.h and call
# GpuStaticPropRegistry::*. Confined to RenderWorld/legacy/ subdir.
RenderWorld/legacy/static_prop_backend.h
RenderWorld/legacy/static_prop_backend.cpp
```

- [ ] **Step 2: Create the script**

```bash
#!/usr/bin/env sh
# scripts/check-include-firewall.sh
#
# Phase 1 (M1): grep-based include + symbol firewall for the
# RenderWorld boundary. Run pre-commit when any RenderCore/,
# RenderWorld/, or GameAdapters/ file changes.
#
# Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
#       section 12.
#
# Exit 0  = clean
# Exit 1  = at least one violation

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# M2 fix (adversarial review pass 2 2026-05-22): enumerate the full
# Section 12 module list. The `[ -d ] || continue` guard below makes
# the script forward-compatible without per-slice script edits.
# GameAdapters is the carve-out module: it MAY include both sides.
# Do NOT add it to SCOPE_DIRS.
SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL"

# Forbidden headers (any include of these from SCOPE_DIRS is a violation).
# C1 fix: RenderCore must stay pure — gos_static_prop_batcher.h pulls
# <GL/glew.h> + Stuff/Stuff.hpp transitively; that violates Section 12
# even though the header is engine-side. Same for tgl.h / msl.h.
FORBIDDEN_HEADERS="appear.h bdactor.h mech3d.h objectappearance.h objmgr.h mission.h warrior.h gos_static_prop_batcher.h tgl.h msl.h GL/glew.h Stuff/Stuff.hpp"

# Forbidden symbol names (case-sensitive). Catches forward-decls,
# function-signature uses, typedef/using aliases that an include-only
# checker misses.
FORBIDDEN_SYMBOLS="Appearance BldgAppearance TreeAppearance GVAppearance Mech3DAppearance GenericAppearance ObjectAppearance ObjectManager Mission MechWarrior"

VIOLATIONS=0

allowlisted() {
    # $1 = path
    while IFS= read -r line; do
        # skip blanks and comments
        case "$line" in
            ""|"#"*) continue ;;
        esac
        if [ "$1" = "$line" ]; then
            return 0
        fi
    done < scripts/check-include-firewall.allowlist
    return 1
}

for dir in $SCOPE_DIRS; do
    if [ ! -d "$dir" ]; then continue; fi
    # Headers
    for hdr in $FORBIDDEN_HEADERS; do
        if grep -rn "include.*${hdr}" "$dir" 2>/dev/null; then
            echo "VIOLATION: forbidden include of ${hdr} in ${dir}/" >&2
            VIOLATIONS=$((VIOLATIONS+1))
        fi
    done
    # Symbols (word-boundary case-sensitive grep)
    for sym in $FORBIDDEN_SYMBOLS; do
        # Use -w; this catches forward-decls (`class Appearance;`),
        # parameter types (`const Appearance&`), and typedef uses.
        hits="$(grep -rwn "${sym}" "$dir" 2>/dev/null || true)"
        if [ -n "$hits" ]; then
            # Filter allowlist (very small list; line-by-line)
            while IFS= read -r line; do
                file="$(printf '%s' "$line" | cut -d: -f1)"
                if allowlisted "$file"; then
                    continue
                fi
                echo "VIOLATION: forbidden symbol '${sym}' in ${line}" >&2
                VIOLATIONS=$((VIOLATIONS+1))
            done <<EOF
$hits
EOF
        fi
    done
done

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "" >&2
    echo "scripts/check-include-firewall.sh: ${VIOLATIONS} violation(s)" >&2
    echo "Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md section 12" >&2
    exit 1
fi
echo "scripts/check-include-firewall.sh: clean (scope: ${SCOPE_DIRS})"
exit 0
```

- [ ] **Step 3: Make executable + run**

```bash
chmod +x scripts/check-include-firewall.sh
sh scripts/check-include-firewall.sh
```

Expected: prints `clean (scope: RenderCore RenderWorld Visibility
MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL)`. Only
`RenderCore` and `RenderWorld` actually exist in M1; the rest are
guarded by `[ -d ] || continue` and silently skipped. If any
violation reported, STOP — there is a real boundary failure in the
M1 modules that needs fixing before commit.

- [ ] **Step 4: Self-test with a synthetic violation (proves the script catches breakage)**

Temporarily add a single line to `RenderWorld/RenderWorld.h`:

```cpp
class Appearance;  // synthetic violation - DO NOT COMMIT
```

Re-run:

```bash
sh scripts/check-include-firewall.sh
```

Expected: exit 1 with `VIOLATION: forbidden symbol 'Appearance' in
RenderWorld/RenderWorld.h:<N>`. The script works.

REMOVE the synthetic line:

```bash
git checkout -- RenderWorld/RenderWorld.h
sh scripts/check-include-firewall.sh
```

Expected: clean again. Confirm with `grep -n "class Appearance" RenderWorld/RenderWorld.h`
returning empty.

- [ ] **Step 5: Commit**

```bash
git add scripts/check-include-firewall.sh scripts/check-include-firewall.allowlist
git commit -m "build(renderworld): add include-firewall grep gate (M1 Task 15)

Phase 1 enforcement: grep-based pre-commit check for forbidden
headers and forbidden symbol names in RenderCore/ and RenderWorld/.
GameAdapters/ is the bridge module and is OUT of scope.
Allowlist documents the gos_static_prop_registry.h carve-out
(adversarial review M2 resolution).

Phase 2 (debug assertions) and Phase 3 (CI gate) are future slices.

Spec: 2026-05-22-renderworld-boundary-spec.md section 12"
```

---

## Phase D — Verification

**Phase D goal:** prove M1 success criteria against HEAD. Capture the
baseline-vs-post diff in commit artifacts; document the slice in the
worktree CLAUDE.md "Active campaigns" section; greybeard pass on the
adapter pattern itself.

### Task 16: Capture parity baseline and post-slice diff

**Files:**
- (Read-only) `tests/smoke/artifacts/<timestamp>/`

- [ ] **Step 1: Capture pre-slice baseline (IF NOT ALREADY DONE)**

If a Phase-A-pre baseline was not captured, do it now from the merge
base or from the most recent pre-M1 commit:

```bash
git rev-parse HEAD > /tmp/m1_post_sha.txt
git log --oneline -1 HEAD
# checkout pre-M1 (use the SHA of the commit immediately before Task 1):
git stash
git checkout <pre-M1-sha>
```

Build + smoke:

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Copy the artifacts dir aside:

```bash
cp -r tests/smoke/artifacts/<latest>/ /tmp/m1_baseline_artifacts/
git checkout $(cat /tmp/m1_post_sha.txt)
git stash pop
```

- [ ] **Step 2: Re-smoke at HEAD (post-M1)**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

- [ ] **Step 3: Diff the registry counter banners**

```bash
grep -h "STATIC_PROP_REGISTRY v1" /tmp/m1_baseline_artifacts/*.log | sort > /tmp/baseline_counters.txt
grep -h "STATIC_PROP_REGISTRY v1" tests/smoke/artifacts/<latest>/*.log | sort > /tmp/post_counters.txt
diff /tmp/baseline_counters.txt /tmp/post_counters.txt
```

Expected: ZERO functional diff. The only acceptable diff is in
timing-related fields (frame numbers, allocation-order counters
already known to be nondeterministic; if such exist, exclude them
from the diff with a -I regex on the diff command). Any drift in
event counts (`registered=`, `invalidated=`, `markVisible=`) is a
SLICE FAILURE — investigate before proceeding.

- [ ] **Step 4: Verify `[RENDER_WORLD v1]` banner is present and consistent**

```bash
grep -h "RENDER_WORLD v1" tests/smoke/artifacts/<latest>/*.log | head -20
```

Expected: at least one `event=init`, one `event=destroy`, and N
`frame=...` lines per mission. `objects=` is nonzero and stable across
the run.

Cross-check: `objects=` count matches the registry's own active count.
```bash
grep -h "STATIC_PROP_REGISTRY v1" tests/smoke/artifacts/<latest>/*.log | grep -i "active\|registered"
```

Confirm the numbers agree (within +/- 1 for race-free closure).

- [ ] **Step 5: Run the firewall script one more time**

```bash
sh scripts/check-include-firewall.sh
```

Expected: exit 0.

### Task 17: Greybeard pass — META-FIX vs PATCH ruling on the adapter pattern itself

**This task does NOT modify code.** It is a written ruling that the
plan executor MUST perform before declaring M1 complete.

- [ ] **Step 1: Run the greybeard skill against the M1 outcome**

Per CLAUDE.md: "Before proposing/writing any fix: run the greybeard
skill (`.claude/skills/greybeard.md`)." M1 is a structural change, not
a bug fix, but the skill still applies because the adapter pattern is
explicitly TEMPORARY (spec section 10 deletion criteria). The greybeard
pass must rule: is the adapter a META-FIX, or a PATCH (justified)?

Dispatch a fresh greybeard subagent with:

```
run the greybeard skill. Target: GameAdapters/StaticPropRenderAdapter
introduced in RenderWorld Slice M1. Source spec:
docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
section 10. Question: is this adapter a META-FIX (the upstream
change that retires a bug class) or a PATCH (justified) with a
named follow-up?
```

- [ ] **Step 2: Record the ruling**

Per CLAUDE.md, the answer MUST be either META-FIX or PATCH (justified)
with debt. The PATCH ruling is the expected outcome here because the
spec explicitly documents adapter-deletion criteria. The deletion
criteria from spec section 10 ARE the named follow-up; recording them
here closes the greybeard requirement:

> **Greybeard ruling (record verbatim in the M1 closing commit):**
>
> PATCH (justified). The adapter is a temporary bridge per spec
> section 10. Named follow-up: adapter deletion lands when (1) the
> corresponding game-side class has been refactored to call RenderWorld
> directly OR has been retired, AND (2) tier1 5/5 + parity probe
> confirm zero pixel delta for one full release without the adapter.
> The adapter is NOT deletable just because RenderWorld now exists;
> removal is a separate explicit slice with substitutive proof per
> `memory/feedback_offload_must_be_substitutive_not_additive.md`.

If the greybeard subagent rules META-FIX instead, record that ruling
verbatim and revise the spec section 10 deletion criteria in a
follow-up commit.

### Task 18: Document the slice in worktree CLAUDE.md

**Files:**
- Modify: `.claude/worktrees/nifty-mendeleev/CLAUDE.md` ("Active campaigns" section)

- [ ] **Step 1: Locate insertion point**

```bash
grep -n "## Active campaigns" .claude/worktrees/nifty-mendeleev/CLAUDE.md
```

Expected: one section header. Insert a new bullet at the END of that
section (do not displace the existing Unified-projection F1 bullet).

- [ ] **Step 2: Add bullet**

```markdown
- **RenderWorld Slice M1** (SHIPPED <date>): static-prop adapter routes
  5 audited call sites (`mclib/bdactor.cpp:1471,2802,4269,4855`,
  `code/warrior.cpp:7593`) through `GameAdapters::StaticPropRenderAdapter`
  -> `RenderWorld::upsertStaticProp` -> `GpuStaticPropRegistry::registerRecipe`.
  Three new modules at repo root: `RenderCore/`, `RenderWorld/`,
  `GameAdapters/`. `[RENDER_WORLD v1]` banner; opt-in `MC2_RENDER_WORLD_TRACE=1`.
  Firewall: `scripts/check-include-firewall.sh` (Phase 1 grep, pre-commit).
  Adapter is TEMPORARY per spec section 10 deletion criteria (greybeard
  PATCH ruling 2026-05-22). M1.5 next (object-ID buffer); M2..M5 follow.
  Spec: `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`.
  Plan: `docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`.
```

- [ ] **Step 3: Commit and close M1**

```bash
git add .claude/worktrees/nifty-mendeleev/CLAUDE.md
git commit -m "docs(renderworld): mark Slice M1 shipped in CLAUDE.md (M1 Task 18)

5 audited call sites routed; firewall script clean; tier1 5/5;
[RENDER_WORLD v1] banner emitting; greybeard PATCH (justified)
with named deletion criteria.

Spec: 2026-05-22-renderworld-boundary-spec.md
Plan: 2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md"
```

---

## Goal-backward verification (slice success proof)

How does the final state prove M1 succeeded? Each item must hold AFTER
Task 18 commits.

1. **tier1 5/5 pass:**
   `py -3 .../scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs`
   exits 0.
2. **Parity probe zero admission delta:** Task 16 diff of
   `[STATIC_PROP_REGISTRY v1]` counters shows zero functional drift
   vs pre-M1 baseline.
3. **`[STATIC_PROP_REGISTRY v1]` counts unchanged:** same as (2).
4. **`[RENDER_WORLD v1]` emits with matching count:** Task 16 Step 4
   confirms `objects=` matches the registry's active count.
5. **Include-firewall script clean:** `sh scripts/check-include-firewall.sh`
   exits 0 (Task 15 Step 5 / Task 16 Step 5).
6. **Greybeard pass on adapter pattern:** Task 17 recorded ruling
   (expected: PATCH justified with named deletion criteria).
7. **No direct registry callers remain outside the adapter:** Phase B
   gate greps return zero hits.
8. **Build clean on `--config RelWithDebInfo`:** every task ran the
   build; the final mc2.exe is the post-Task-18 binary.

---

## Risks and mitigations

### R1. Sentinel translation leak

**Risk:** an `int32_t -1` from the registry leaks above the adapter,
or a `RenderObjectHandle::invalid()` leaks below into a registry call.
Either is a Section 10 violation and silently breaks the boundary.

**Mitigation:** the translation lives in TWO seams —
`RenderWorld.cpp::recipeIndexToHandleIndex/handleToRecipeIndex`
(engine seam; the legacy backend bridge takes the int32_t) and
`StaticPropRenderAdapter.cpp::syncStaticProp / syncStaticPropLateSpawn`
(game seam). The OUT-parameter pattern in
`syncStaticProp(..., int32_t* legacyRecipeIndexOut)` is the ONLY allowed
path for the legacy sentinel to reach the bdactor
`staticReg.recipeIndex` field. M3 (deferred to spec amendment 2026-05-22)
formalizes the two-seam rule. If a future PR adds a third translation
site, the firewall script's symbol grep for `int32_t.*recipeIndex` in
`RenderCore/` and `RenderWorld/` (excluding the legacy backend allowlist)
will not catch it; this is a known gap of Phase 1 enforcement. The
Phase 2 follow-up tightens to AST-aware checking.

### R2. Half-migrated state during Phase B

**Risk:** between Task 8 and Task 12, a subset of producers is routed
through the adapter while others still call the registry directly.
The `[STATIC_PROP_REGISTRY v1]` counter banner stays balanced because
both paths terminate in the same backend, BUT the `[RENDER_WORLD v1]`
banner shows a partial `objects=` count. If a crash bisect happens
mid-Phase-B, the diagnostic count is misleading.

**Mitigation:** each Phase B task is its own commit with its own smoke
gate (Task 8-12). Bisect lands on a known-clean commit. The Phase B
gate (post-Task-12) explicitly verifies "zero direct callers remain"
via grep before Phase C starts. If a user STOPs Phase B partway, the
worktree is still smoke-green at each commit boundary; the partial
`objects=` count is acceptable.

### R3. Forward-decl creep above the adapter

**Risk:** a future change adds `class Appearance;` to `RenderWorld.h`
"for one signature" — a Section 12 violation that the firewall script
catches but a human reviewer might wave through.

**Mitigation:** Task 15 builds the script and Task 15 Step 4 self-tests
that the script catches exactly this case (`class Appearance;` synthetic
violation). The script runs pre-commit when any RenderCore/RenderWorld
file changes; CLAUDE.md mention in Task 18 ensures the next agent reads
about it.

### R4. Smoke regression in unrelated mission

**Risk:** the slice is route-only, but a hidden ordering change in the
adapter (e.g., when sentinel translation happens vs when
`staticReg.registered` is set) causes a mission-specific regression
that only appears in tier1's mc2_24 (heaviest static-prop mission).

**Mitigation:** each Phase B task runs the FULL tier1, not just a
2-mission subset. mc2_24 is in tier1 by selection. The visual-iteration
60s mode is NOT used here because route-only does not warrant it.

### R5. CMake link-order or include-dir collision

**Risk:** the `target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR})`
line in `GameAdapters/CMakeLists.txt` could shadow an unexpected header
elsewhere if the include topology has hidden duplicates.

**Mitigation:** Task 7 Step 5 is a CLEAN build (`rm mc2.exe` first).
If a duplicate-header issue exists, the clean build surfaces it as a
compile error, not as a silent header swap.

### R6. `code/warrior.cpp` relink time

**Risk:** `code/warrior.cpp` is the largest TU in the codebase; Task 12
full-relink takes time and may tempt the executor to skip the
`rm mc2.exe` step.

**Mitigation:** CLAUDE.md's full-relink rule is stated verbatim in
Task 12 Step 4. No skip. The plan does not promise a time budget.

### R7. Re-grep drift in audit lines

**Risk:** by the time the plan executes, the cited line numbers
(1471, 2802, 4269, 4855, 7593) may have drifted from edits in other
slices.

**Mitigation:** every Phase B task starts with a `grep` re-locate
step. The plan is symbol-anchored, not line-anchored, and the
symbol-anchor is `GpuStaticPropRegistry::registerRecipe` /
`registerStaticProp`. Even if line numbers move, the four-vs-one
count and the calling class (Bldg / Tree / warrior) remain stable.

---

## Rollback strategy per phase

- **Phase A (Tasks 1, 2, 3, 4, 5, 6, 6.5, 7):** all-new files. Revert
  by `git revert` of those task commits (or `git reset --hard` to
  pre-M1). No game-side code touched; rollback is unconditionally safe.
- **Phase B (Tasks 8-12):** five independent single-site commits. Each
  is independently revertable; reverting one site restores the direct
  call to `GpuStaticPropRegistry::registerRecipe` at that site only.
  Half-rolled-back state is supported and tested by the per-task smoke
  gate (since "half migrated" was the in-progress state during Phase B
  itself).
- **Phase C (Tasks 13-15):** Task 13/14 wire lifecycle calls; reverting
  either drops the `[RENDER_WORLD v1]` banner but does NOT regress
  runtime behavior. Task 15 is script-only; revertable in isolation.
- **Phase D (Tasks 16-18):** verification + docs. No code paths to
  revert.

**Full M1 rollback:** `git revert` Tasks 18..1 (in reverse), or
`git reset --hard <pre-M1-sha>`. All slice changes are confined to
the listed files; no dotfiles or build-config files outside
`CMakeLists.txt` are touched.

---

## Build + smoke gate per phase (canonical commands)

**Build (every task that compiles code):**

```powershell
Remove-Item build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 10
```

(Skip the `Remove-Item` line only on header-only changes that touch
zero `.cpp`. Phase A Tasks 1-3 are header-only; Tasks 4-18 require
the full relink.)

**Smoke (every phase boundary; every Phase B task):**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

This is the canonical CLAUDE.md form; use verbatim. Subagents that
deviate are violating the worktree rule.

---

## Out of scope for M1 (explicit)

Per spec section 13 + Out-of-scope rules from the prompt:

- Mechs (M2), terrain (M3), VFX (M4), decals (M5)
- Object-ID buffer (M1.5)
- MaterialGpu SSBO (Tier 2 V1)
- New cull dispatch (`gpu_cull.comp` unchanged)
- ShadowView promotion (Phase 1 = single view)
- Adapter deletion (criteria documented; future slice)
- Phase 2 (debug assertions) and Phase 3 (CI gate) firewall
- Pass Contract Registry integration (separate spec)
- `DrawPacket` runtime dispatch (struct exists, backend unchanged)
- Widening `staticReg.recipeIndex` from int32_t to a typed handle
  (slot-side storage; future refactor; see Decision D4)

---

## Adapter deletion criteria (recorded here for the future slice)

Per spec section 10. The `StaticPropRenderAdapter` introduced in M1
becomes deletable when:

1. The corresponding game-side classes (`BldgAppearance`,
   `TreeAppearance`, `MechWarrior::createWaypointMarker` etc.) have
   been refactored to call `RenderWorld::upsertStaticProp` directly
   with engine-side types only — i.e. the Appearance hierarchy itself
   has been retired or the call sites no longer touch
   `GpuStaticPropInstance` / `TG_MultiShape` at the game-side layer; OR
2. The Appearance hierarchy has been retired from the migrated codebase; AND
3. Tier1 smoke + parity probe confirm zero pixel delta for one full
   release without the adapter.

The adapter is NOT deletable just because `RenderWorld` exists.
Removal is its own slice (call it Slice M-Adapter-Retire-Static when
it lands) with the substitutive proof from
`memory/feedback_offload_must_be_substitutive_not_additive.md`.

---

## Vulkan-prep restatement (load-bearing per CLAUDE.md)

Every new type added in M1 was checked against the "is this expressible
in Vulkan?" test:

- `Handle<Tag>` -> Vulkan handle (opaque uint64; M1 uses 32-bit but
  the shape is identical).
- `RenderObjectDesc` / `StaticPropDesc` -> Vulkan `Vk*CreateInfo`
  (by-value descriptor, takes raw payload, no implicit state).
- `DrawPacket` -> Vulkan command-buffer entry (pipeline + bindings +
  vertex/index range).
- `RenderWorld::upsertStaticProp` / `destroy` / `markVisible` -> command
  recording onto a per-frame command builder.

The adapter pattern itself is a porting-layer concept; it has no
Vulkan equivalent and is explicitly TEMPORARY (deletion criteria above).
This is acceptable because the adapter does not appear in any Vulkan-
era API surface; it is bridge scaffolding only.

---

## Self-review against spec (writing-plans skill mandate)

- Spec section 3 (handle 20/12): Task 1 implements. Covered.
- Spec section 4 (lifecycle + StaticPropDesc): Task 2 implements desc.
  Lifecycle states (Visible/Submitted/Retired) NOT modeled in M1 (per
  route-only rule); deferred to M2+. Coverage: M1 scope items only.
- Spec section 6 (DrawPacket documentary): Task 3 implements. Covered.
- Spec section 10 (adapter pattern + sentinel translation): Tasks 7-12.
  Covered. Sentinel translation rule applied in both
  `RenderWorld.cpp` and `StaticPropRenderAdapter.cpp`; verified by
  inspection at write time.
- Spec section 11 (banner + audit log): Task 6 (banner state), Task 13
  (init/destroy lifecycle), Task 14 (frame tick). M1.5 substrate
  (object-ID buffer) explicitly out of scope.
- Spec section 12 (forbidden deps + dependency shapes): Task 15
  implements grep enforcement; Phase 2/3 deferred.
- Spec section 13 (first-slice scope + 5 audited call sites): Tasks
  8-12. All 5 sites wired.
- Spec section 13 correctness gate: Task 16 verifies all 5 items
  (tier1 5/5, counts unchanged, banner emitting, include-firewall clean,
  no game header in RenderWorld TU).

**No placeholders.** Every task has explicit files, exact code, exact
commands. Per the writing-plans skill self-review checklist, no `TBD`,
no "implement later", no "add appropriate error handling."

**Type consistency:** `RenderObjectHandle` is used identically across
Tasks 1, 2, 5, 6, 6.5, 7. `StaticPropDesc` is the only desc variant
in M1. `GameAdapters::StaticProp::syncStaticProp` has a consistent
signature in `StaticPropRenderAdapter.h` (Task 7) and at every Bldg/
Tree call site (Tasks 8-11); `syncStaticPropLateSpawn` is the
warrior.cpp consumer (Task 12).

**Spec ambiguities flagged for user (Decision section):** D1 (module
physical layout), D2 (desc by-value vs view), D3 (adapter lifecycle),
D4 (`staticReg.recipeIndex` storage type during M1).

---

## Pre-execution / must-pass before merge gates

Per user resolution 2026-05-22 (adversarial review pass 2): every M1
merge candidate MUST satisfy ALL nine of the gates below. Failing any
one is a slice failure; do not merge until restored.

1. **RenderCore public headers grep clean — no GL, no mclib, no
   game-side headers.**

   ```bash
   grep -rn "include" RenderCore/ | \
     grep -E "GL/|mclib/|appear|bdactor|mech3d|warrior|mission|Stuff|gos_static_prop_batcher|tgl\.h|msl\.h"
   ```

   Expected: ZERO hits. Any hit is a C1 regression.

2. **`GameAdapters/` is the ONLY new module that names `Appearance`.**

   ```bash
   grep -rn "Appearance" RenderCore/ RenderWorld/
   ```

   Expected: ZERO hits. Adapters may name Appearance; engine boundary
   may not.

3. **All 5 audited registration producers route through the adapter
   (including warrior.cpp:7593 late-spawn).**

   ```bash
   grep -n "GameAdapters::StaticProp::syncStaticProp" mclib/bdactor.cpp
   grep -n "GameAdapters::StaticProp::syncStaticPropLateSpawn" code/warrior.cpp
   ```

   Expected: 4 hits in bdactor.cpp + 1 hit in warrior.cpp = 5 total.
   ZERO direct calls to `GpuStaticPropRegistry::registerRecipe` or
   `GpuStaticPropRegistry::registerStaticProp` outside the legacy
   backend TU and the registry's own files.

4. **Late-spawn path produces / adopts a `RenderObjectHandle`
   (not just a bool wrapper).**

   ```bash
   grep -n "adoptStaticPropRecipe\|registerStaticPropAndReturnRecipe" \
       GameAdapters/StaticPropRenderAdapter.cpp \
       RenderWorld/RenderWorld.cpp \
       GameOS/gameos/gos_static_prop_registry.h
   ```

   Expected: at least one hit in each file. The adapter must return
   a real handle from `syncStaticPropLateSpawn`.

5. **H4 `needsFullBakeNextFrame = true` preservation grep-verified
   at both Bldg and Tree sites.**

   ```bash
   grep -n "needsFullBakeNextFrame = true" mclib/bdactor.cpp
   ```

   Expected: hits at all original audit positions (1481, 2818, 4279,
   plus any in Task 11 site). Compare line-by-line against
   pre-M1 baseline `grep` output captured at Task 16 Step 1. ZERO
   removed lines.

6. **`Handle` uses explicit uint32_t shift/mask packing, NOT bitfields.**

   ```bash
   grep -n "uint32_t.*:.*[0-9]" RenderCore/Handle.h
   ```

   Expected: ZERO hits (no bitfield syntax `name : N;` in the file).
   The `Handle` template must use `uint32_t bits;` with `make(...)`,
   `index()`, `generation()` accessors.

7. **Firewall script scans full future module set with `[ -d ]` guards.**

   ```bash
   grep -n "SCOPE_DIRS=" scripts/check-include-firewall.sh
   ```

   Expected: `SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer
   MaterialSystem DebugRenderer RenderDeviceGL"`. The directory-existence
   guard `[ -d ... ] || continue` must precede each scan loop.

8. **`[STATIC_PROP_REGISTRY v1]` counts unchanged vs HEAD baseline.**

   See Task 16 Step 3. Diff result MUST be empty (modulo nondeterministic
   timing fields). ANY drift in `registered=`, `invalidated=`,
   `markVisible=` is a slice failure.

9. **`[RENDER_WORLD v1] objects=` count sourced from registry
   active-count (m4 fix) or clearly labeled adapter-only.**

   Default path: sourced via `legacy::getStaticPropActiveCount()` ->
   `GpuStaticPropRegistry::getActiveCount()`. Cross-check at Task 16
   Step 4: `objects=` value matches the registry-side active count
   within +/- 1 for race-free closure.

If a gate cannot be satisfied, the plan executor MUST file a deviation
note in the M1 closing commit and the gate must be addressed by a
follow-up patch BEFORE the slice is declared shipped. Skipping a gate
silently is the same class of error this review pass exists to catch.

End of plan.
