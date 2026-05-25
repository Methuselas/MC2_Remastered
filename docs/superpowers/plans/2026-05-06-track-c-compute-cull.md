# Track C — GPU Compute Cull + Async Readback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development.
> Track C ships as four sequential slices C0/C1/C2/C3. Each slice has its
> own gate; do NOT collapse the staging.

**Goal:** Replace the half-frame CPU "should this be culled?" cost (`Terrain::geometry vertexProjectLoop` + per-actor `recalcBounds()` cascading through `Camera.UpdateRenderers`, ~3.66 ms self-time at wolfman zoom on stock missions) with a single-pass GPU compute cull (~50 µs target) that fills per-bucket visible-ID lists for indirect draw and feeds CPU lifecycle gates via async-readback. End-state: `Camera::UpdateRenderers` becomes a stub.

**Architecture:** Four sequential slices.

- **C0** wires a *substrate*: per-frame CPU upload of compact dynamic-actor visibility records (mechs, GVs, gates, turrets, …) into a persistent-mapped triple-buffered SSBO, schema-compatible with Track B's static-prop instance SSBO. NO compute cull yet — just the record substrate, validated against legacy `recalcBounds()` output via env-gated parity. Sets up the cull-input that C1 will consume.
- **C1** lights up the compute cull for *render only*. A single combined compute dispatch (Q8) reads C0+TrackB instance SSBOs and a frustum-plane UBO, atomically scatter-writes per-bucket visible-ID lists with `instanceCount` patched directly into a pre-built `DrawElementsIndirectCommand` array (Q11). A `glMultiDrawElementsIndirectCount` consumes the result. CPU lifecycle gates STILL read legacy `inView` — C1 is GPU→GPU only (Q12 slice boundary).
- **C2** introduces a 3-frame readback ring (persistent-mapped, `glFenceSync`) and feeds frame-N-1 visibility into a *single* non-lifecycle consumer (Tracy plot OR `[GPU_CULL v1]` summary). Three-tier fallback: N-1 last-good → N-2 last-good → conservative-visible. Validates fence + ring + fallback paths without lifecycle risk.
- **C3** routes the lifecycle gates: `objmgr::update`, `Mech3DAppearance::update`, `GVAppearance::update`, AI gate at `code/mech.cpp:6497`, and weapon-spawn-node queries at `mclib/mech3d.cpp:721,759,795,833` and `mclib/gvactor.cpp:445,500,533` read GPU N-1 visibility instead of `inView`/`canBeSeen()`. Hard exit: `Camera::UpdateRenderers` becomes a stub. Q7 1-frame artifact accepted; visual canaries (zoom-transition, camera-jump, first-contact, weapon-spawn) verify gameplay tolerance.

**Tech Stack:** C++ (engine, MSVC RelWithDebInfo), GL 4.3 baseline + GL 4.6 OR (`ARB_indirect_parameters` + `ARB_shader_draw_parameters`) for Track C activation (Q13), GLSL `#version 430` compute shaders, persistent-mapped SSBOs (`GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`), `glFenceSync`/`glClientWaitSync` ring (mirroring `gos_static_prop_batcher.cpp:248,1251,1667` template), `[GPU_CULL v1]` env-gated instrumentation, AMD RX 7900 XTX 26.3.1 canary.

**Spec references:**
- Decisions: `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` (Q7 artifact, Q8 dispatch shape, Q9 visible-ID list, Q10 dynamic actor records / C0, Q11 bucket key, Q12 sync contracts post-advisor-pass-2, Q13 GL baseline, Q14 split)
- Recon: `docs/superpowers/explorations/2026-05-06-track-c-compute-cull-recon.md` (full code-grounded inventory; the `inView` consumer table in §5–§6 is the source-of-truth for C3 routing)
- Roadmap: `docs/superpowers/mc3-rendering-modernization-roadmap.md` Track C
- Track B widening: ships static-prop SSBO that C0/C1 share schema with (lockstep — see Q11 tuple)
- Track A1 plan: `docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md` (predicate semantics — `clipSpaceFrustumAdmit`; the GLSL compute predicate in C1 is its lockstep port)

**Worktree CLAUDE.md rules in force:**
- Build: `cmake --build build64 --config RelWithDebInfo`
- Stock install must remain playable; killswitch (`MC2_GPU_CULL=0`) defaults to legacy until each slice's soak passes
- Tier1 5/5 smoke is the regression gate; do NOT use the menu canary in iterative loops (`memory/feedback_no_menu_canary_in_smoke.md`)
- Debug instrumentation rule: every slice lands env-gated `[GPU_CULL v1]` prints in same commit (lifecycle/cull/render rework)
- Documentation discipline: every cited symbol grep-verified at write time
- Adversarial review for high-stakes (architectural endpoint) — REQUIRED before each slice flip

---

## File structure (all four slices, summary)

| File | Slice | Status | Responsibility |
|---|---|---|---|
| `GameOS/gameos/gpu_cull_record.h` | C0 | Create | std430 record schema (`GpuActorRecord` 64 B + `GpuVisibilityRecord` shared schema), `static_assert` size + offsets, doc block on category enum. |
| `GameOS/gameos/gpu_cull_record.cpp` | C0 | Create | (Defensive — usually empty.) Houses any non-inline helpers if the schema needs them; primary site for `static_assert` evaluation. |
| `GameOS/gameos/gpu_cull_substrate.h` | C0 | Create | Public API: `frameBegin()`, `submitDynamicActor(rec)`, `flushUpload()`, `getInstanceSsboBindingPoint()`, `[GPU_CULL v1]` startup banner emit. |
| `GameOS/gameos/gpu_cull_substrate.cpp` | C0 | Create | Triple-buffered persistent-mapped SSBO impl mirroring `gos_static_prop_batcher.cpp:248,261,1251` pattern. Per-frame upload path. Counter snapshot at `flushUpload()`. |
| `code/objmgr.cpp` | C0 | Modify | Add dynamic-actor enumeration call (one place; see Task C0-3) — emits one `GpuActorRecord` per live `Mech/GV/Gate/Turret`. Wrapped in `if (g_gpuCullSubstrateEnabled)` guard. |
| `mclib/appear.h` | C0 | Read-only | Source of `inView` and `position`/screenPos accessors used to construct records. No edits. |
| `GameOS/gameos/gpu_cull_parity.h` | C0 | Create | `MC2_GPU_CULL_AABB_PARITY=1` env-gated diff: per-frame, for each emitted record, recompute the legacy bound with the existing CPU path and assert envelope match. Counters surface via `[GPU_CULL v1] event=parity_summary`. |
| `GameOS/gameos/gpu_cull_compute.h` | C1 | Create | Public API for compute dispatch: `init()`, `dispatch(view-proj-data, frustumPlanes)`, killswitch state, GL-version probe result. |
| `GameOS/gameos/gpu_cull_compute.cpp` | C1 | Create | Compute program build (mirroring `makeProgram` shape but with `GL_COMPUTE_SHADER` only), program object, dispatch, `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT \| GL_COMMAND_BARRIER_BIT)`, indirect-command-buffer build at mission load, GL 4.6/extension probe + `[GPU_CULL v1] gl_version=...` banner per Q13. |
| `shaders/gpu_cull.comp` | C1 | Create | The compute shader: frustum + distance test, atomic compaction into per-bucket visible-ID lists, in-place `instanceCount` patch on the `DrawElementsIndirectCommand` array. `#version 430`. |
| `shaders/gpu_cull_predicate.glsl` | C1 | Create | The GLSL port of A1's `clipSpaceFrustumAdmit` (advisor sharpening #3 — drift hazard). Included by `gpu_cull.comp`; documented as the lockstep mirror of `mclib/object_admission_predicate.{h,cpp}`. |
| `code/mission.cpp` | C1 | Modify | One-line dispatch hook: between `land->geometry()` (line 500) and `ObjectManager->update` (line 505). Same hook host for the C0 substrate `flushUpload()` call. |
| `code/gamecam.cpp` | C1 | Modify | Replace the relevant indirect-draw path's command-buffer source with the GPU-written one (host-built only at mission load; per-frame instanceCount comes from compute). |
| `GameOS/gameos/gpu_cull_readback.h` | C2 | Create | Public API: `frameEnd()` (queue fence), `tryConsume()` (returns frame-N-1 visibility view + status), `[GPU_CULL v1] event=readback_*` events. |
| `GameOS/gameos/gpu_cull_readback.cpp` | C2 | Create | 3-frame ring with `glFenceSync` per slot. Persistent-mapped readback SSBO. Three-tier fallback. Mirrors `gos_static_prop_batcher.cpp:248,1251,1667` ring shape but with `GL_MAP_READ_BIT \| GL_MAP_PERSISTENT_BIT \| GL_MAP_COHERENT_BIT`. |
| `GameOS/gameos/gos_profiler.cpp` | C2 | Modify | (If chosen as proof-of-life consumer.) Tracy plot of GPU-visible count vs CPU-visible count, frame N-1 vs N. |
| `code/objmgr.cpp` | C3 | Modify | Per-block visibility gate at line 1760 reads `gpuCullVisible(blockIdx)` (still preserves the existing `if(active)` cascade — only the input changes). |
| `mclib/mech3d.cpp` | C3 | Modify | Lines 2055-2069 (`update()` body), 2377 (TGL alloc gate), 2881 (render-time gate), 4180-4196 (init+forced inView), node-pos sites at 721/759/795/833. |
| `mclib/gvactor.cpp` | C3 | Modify | Lines 1946/1982/2022/2039/2711 (update/render gates), node-pos sites at 445/500/533. |
| `code/mech.cpp` | C3 | Modify | AI gate at 6497 (and same-family at 6448, 6466) reads GPU visibility. |
| `code/gvehicl.cpp` | C3 | Modify | AI gates at 3928, 3936; vehicle update body 3183-3736 internal `recalcBounds` polls. |
| `code/mechcmd2.cpp` | C3 | Modify | `Camera::UpdateRenderers` body becomes a stub (Tracy zone retained at line 692, body collapses to GPU-cull dispatch trigger only). |
| `~/.claude/projects/.../memory/track_c_compute_cull.md` | C0 | Create | Memory file for cross-slice findings; index entry in MEMORY.md. Update at each slice flip. |

---

## Cross-slice rules (enforce in every commit)

These come from the 5 advisor sharpenings and the load-bearing memories. Every task below assumes them.

1. **Lazy-init env probes.** Every `MC2_GPU_CULL_*` env var is read inside an accessor function with a `static bool s_initialized = (probe(), true);` pattern (zero static-init-order surface). Mirrors A1's `objectAdmissionPredicateMode()` shape.
2. **Hard-fail selftests.** Every predicate / schema / parity test prints `[GPU_CULL v1] event=selftest_pass|fail case=<name>` per case at startup. Non-zero failure count aborts via `STOP("[GPU_CULL] selftest failed; see log")` *before* the engine gets to mission load.
3. **No "two implementations of the same thing."** The GLSL compute predicate (`shaders/gpu_cull_predicate.glsl`) is documented as the lockstep port of `mclib/object_admission_predicate.{h,cpp}`. Bit-identical to within float rounding; any future change touches both files in the same commit. Same lockstep rule (`memory/cpp_glsl_ubo_struct_lockstep.md`) for `GpuActorRecord` ↔ its GLSL declaration.
4. **Single-run captures for parity-data.** The `MC2_GPU_CULL_AABB_PARITY` and `MC2_GPU_CULL_PARITY` paths instrument *one* run (legacy + modern producers running side-by-side, single process) — never byte-diff across two runs.
5. **Identity diff for DESTROY.** Each slice's gate routes a `[DESTROY v1]` baseline capture (from tier1) and compares (kind, reason, gate-state-snapshot) tuples vs a post-flip capture. Net-new destroys = +0; identity match required.

---

## Slice gating (post-advisor pass)

**C0 may execute independently.** It is a pure substrate slice — schema
+ per-frame upload + AABB parity. No compute, no readback, no gate
handoff. C0 unblocks once the schema header lands; nothing
architectural blocks it.

**C1 is GATED on Q15/Q16/Q17 resolution AND Track B substrate readiness.**
Concretely:

- **Q15 (shared-soak discipline)** must be applied — C1 enters soak
  under whatever-is-current; downstream slices (C2, C3) inherit
  sequential-with-overlap. Soak ordering already locked per Q15;
  this is enforcement, not a fresh decision.
- **Q16 (Track B `firstColorOffset` ownership)** must be resolved —
  C1's compute shader reads from Track B's persistent instance buffer.
  If the recipe schema's `firstColorOffset` semantics are still
  undecided, the compute shader's instance read is undefined.
- **Q17 (block-active rollup)** must be answered with explicit choice
  of GPU compute aggregation OR CPU-side conservative walk. C1's
  dispatch shape, barrier set, and soak gate all change depending on
  the answer. See the C1 task list below for the slot.
- **Track B substrate must be in tier1 default-on** (or at minimum,
  Track B's persistent-instance buffer is shipped and the
  `firstColorOffset` decision is committed). C1's compute reads from
  that buffer.

**C2 is GATED on C1 ship + soak.** C2 introduces async readback;
attempting it before C1's GPU→GPU path is proven adds two layers of
risk simultaneously.

**C3 is GATED on C1+C2 ship + soak AND a Q18 lights preflight pass.**
Per Q18: before C3 flip, audit every transitive consumer of
`lightAppearance->inView` (`code/light.cpp:123-124` writes it; if any
C3-routed lifecycle gate reads it, lights join C3's routing list).
Concrete preflight grep is in the C3 section below.

The C-arc is therefore four sequential slices with three gates between
them, each gate carrying explicit dependencies on prior slices and
brainstorm Q-resolutions. **Do NOT attempt to ship C1 without B
substrate ready and Q16/Q17 closed.** This is the non-negotiable
sequencing.

---

## Slice C0 — Dynamic actor visibility record substrate (FULL plan)

**Goal:** Per-frame CPU upload of compact `GpuActorRecord` entries for every live mech / GV / gate / turret / "Other" dynamic actor into a persistent-mapped triple-buffered SSBO, with a common schema that C1's compute shader will consume alongside Track B's static-prop SSBO. NO compute cull yet — this slice ends with a verified record buffer that *would* serve a cull dispatch, validated by AABB parity against legacy `recalcBounds()` output.

**Sizing:** ~3-5 days.

**Hard exit criteria:**
- `[GPU_CULL v1] event=substrate_ready records=N capacity=M` line emits each frame at steady-state.
- `MC2_GPU_CULL_AABB_PARITY=1` shows `mismatches=0` across tier1 5/5 (full duration).
- `[DESTROY v1]` count delta vs baseline = 0.
- Tier1 5/5 PASS with both `MC2_GPU_CULL_SUBSTRATE=0` (default) and `=1`.
- No new GL_INVALID_*; `[GL_ERROR v1]` clean.
- `[CPP_GLSL_LOCKSTEP]` audit on `GpuActorRecord` ↔ GLSL declaration: every offset matches `static_assert`s.

### Q-decisions locked at C0 plan-time

- Q10 schema scope: dynamic actors = mechs, GVs, gates, turrets, generic actors (the §5–§6 recon `inView` consumers minus weapon bolts/clouds/lights — those are out-of-scope per recon classification). One record per actor.
- Q11 bucket key compatibility: `category` field reserves 4 bits for the cull-time bucket selector; bucket assignment finalized in C1.
- Q12 counter representation: not yet relevant in C0 (no compute), but the SSBO layout reserves a `uint actorCountThisFrame` header word that C1 will read.

### C0 schema (LOCKED)

```cpp
// GameOS/gameos/gpu_cull_record.h — verbatim layout (std430)

#pragma once
#include <cstdint>
#include <cstddef>

namespace gpu_cull {

// std430 layout. Bumped to 16-byte alignment for SSBO array stride safety
// (per memory/cpp_glsl_ubo_struct_lockstep.md — std430 stride for an array
// of structs is the next multiple of 16 of the largest member alignment).
//
// Total size: 64 bytes. Two cache lines per pair of records — friendly to
// the compute shader's per-invocation read pattern.
struct alignas(16) GpuActorRecord {
    // World-space center in MC2 cameraPos coords (.x left, .y elev, .z forward).
    // Mirrors how Camera::projectForObjectAdmission consumes coords today.
    float       worldCenter[3];      // offset 0   (12 B)
    float       boundingRadius;      // offset 12  (4 B)  — sphere fallback for
                                     //                     fast cull; AABB
                                     //                     captured below for
                                     //                     parity.
    float       worldAabbMin[3];     // offset 16  (12 B)
    uint32_t    category;            // offset 28  (4 B)  — Category enum
                                     //                     (low 4 bits) +
                                     //                     bucket-key bits
                                     //                     (high 28 bits,
                                     //                     populated by C1).
    float       worldAabbMax[3];     // offset 32  (12 B)
    uint32_t    flags;               // offset 44  (4 B)  — see GpuActorFlags
                                     //                     below.
    uint32_t    actorId;             // offset 48  (4 B)  — stable ID matching
                                     //                     objList[] handle so
                                     //                     C3 routing is O(1).
    uint32_t    prevVisibilityBit;   // offset 52  (4 B)  — frame N-1 visibility
                                     //                     (filled by readback
                                     //                     in C2; written by
                                     //                     CPU mirror in C0).
    uint32_t    consumerFlags;       // offset 56  (4 B)  — see GpuConsumerFlags
                                     //                     below.
    uint32_t    _pad0;               // offset 60  (4 B)  — reserved; std430 pad.
};

static_assert(sizeof(GpuActorRecord) == 64,
              "GpuActorRecord size must match std430 GLSL struct (64 B).");
static_assert(offsetof(GpuActorRecord, worldCenter)        ==  0, "worldCenter offset");
static_assert(offsetof(GpuActorRecord, boundingRadius)     == 12, "boundingRadius offset");
static_assert(offsetof(GpuActorRecord, worldAabbMin)       == 16, "worldAabbMin offset");
static_assert(offsetof(GpuActorRecord, category)           == 28, "category offset");
static_assert(offsetof(GpuActorRecord, worldAabbMax)       == 32, "worldAabbMax offset");
static_assert(offsetof(GpuActorRecord, flags)              == 44, "flags offset");
static_assert(offsetof(GpuActorRecord, actorId)            == 48, "actorId offset");
static_assert(offsetof(GpuActorRecord, prevVisibilityBit)  == 52, "prevVisibilityBit offset");
static_assert(offsetof(GpuActorRecord, consumerFlags)      == 56, "consumerFlags offset");

// Header at SSBO offset 0 (one per ring slot). Compute shader reads
// recordCount; C2 readback reads visibleCount.
struct alignas(16) GpuActorRecordHeader {
    uint32_t recordCount;            // 0  — set by CPU at flushUpload()
    uint32_t recordCapacity;         // 4  — mission-load constant
    uint32_t visibleCount;           // 8  — written by compute (C1+); ignored in C0
    uint32_t _pad0;                  // 12
};
static_assert(sizeof(GpuActorRecordHeader) == 16, "Header must be 16 B std430");

enum GpuActorCategory : uint32_t {
    Cat_Other      = 0u,  // generic actor, fence, artillery, …
    Cat_Mech       = 1u,
    Cat_GroundVeh  = 2u,
    Cat_Gate       = 3u,
    Cat_Turret     = 4u,
    Cat_StaticProp = 5u,  // present so Track B records can flow through the
                          // same compute path (C1 multiplexes static + dynamic
                          // input SSBOs but reads the same record schema).
    // 6..15 reserved.
    CategoryMask   = 0xFu,
};

enum GpuActorFlags : uint32_t {
    Flag_None         = 0u,
    Flag_AlwaysVisible = 1u << 0,  // skip cull (player mech, mission-critical)
    Flag_HasShadow    = 1u << 1,
    Flag_NeverShadow  = 1u << 2,
    // 3..31 reserved.
};

enum GpuConsumerFlags : uint32_t {
    Consumer_None             = 0u,
    Consumer_AIGate           = 1u << 0,  // mech.cpp:6497, gvehicl.cpp:3928,3936 read this actor's visibility for fire decisions
    Consumer_WeaponSpawnNode  = 1u << 1,  // mech3d.cpp:721/759/795/833, gvactor.cpp:445/500/533 query nodes
    Consumer_LifecycleGate    = 1u << 2,  // objmgr update gate
    Consumer_RenderGate       = 1u << 3,  // appearance render-time gate
    // 4..31 reserved.
};

} // namespace gpu_cull
```

**Cross-decision audit notes for the schema:**

- **Why a sphere AND an AABB?** Compute can do a 6-plane sphere test in 1 sub-µs/inst. AABB is for the C0 *parity* check against legacy `recalcBounds()` (which builds extents from MultiShape transformed bounds). C1 will use the sphere as the primary test; AABB is fallback for "tight envelope" cases. Both are packed because (a) the cost is 24 B added to a record we're already paying 64 B for, (b) keeping them in lockstep with legacy lets parity work without a parallel AABB-only path.
- **`prevVisibilityBit` populated in C0:** in C0 there's no GPU readback, so the "previous frame's visibility" is the CPU-side `inView` from the *current* frame at submit time. This is wrong-by-design — it'll be corrected in C2 when readback lands. Documented here so the parity check doesn't try to validate this field.
- **`actorId` = `objList[]` handle:** verified at recon §5 — `objList` is the canonical actor index. C3 routing dereferences `actorId` to map a visibility bit back to its `Appearance::setInView()` call.
- **Why category in low 4 bits + bucket bits in high 28?** C1 will pick a bucket per actor at submit time; reserving that part of `category` keeps C0's cull-input schema stable for C1 without requiring a separate field. Q11's "mesh-range + shader + texture-set + VAO + index-type" tuple resolves to ≤2^28 buckets in any realistic mission.
- **`_pad0` at offset 60:** required by std430 (next multiple of 16 of largest alignment). Lockstep memory: `cpp_glsl_ubo_struct_lockstep.md`.

### C0 tasks

#### Task C0-1 — Schema header + selftest

- [ ] **Step 1.1** — Create `GameOS/gameos/gpu_cull_record.h` with the schema above (verbatim).
- [ ] **Step 1.2** — Create `GameOS/gameos/gpu_cull_record.cpp` (initially just a `#include` of the header to materialize the `static_assert`s in a TU).
- [ ] **Step 1.3** — Add to `CMakeLists.txt` GameOS sources.
- [ ] **Step 1.4** — Selftest function `int gpu_cull_record_selftest()` in `gpu_cull_record.cpp`: at startup, allocates a sample record, sets each field, reads each field via raw byte offsets, prints `[GPU_CULL v1] event=selftest_pass|fail case=record_offsets` per offset. Hard-fail (`STOP`) on any mismatch.
- [ ] **Step 1.5** — Wire selftest into engine startup banner alongside existing `[INSTR v1]` line. Lazy-init env probe pattern: a static accessor `gpu_cull_record_selftest_runs()` returns a counter so future tests can assert it ran.

**Verification (this task):**
```bash
cmake --build build64 --config RelWithDebInfo
# Run any tier1 mission; grep stdout for selftest line.
grep -E "\[GPU_CULL v1\] event=selftest_(pass|fail) case=record_offsets" run.log
```
Expected: at least one `pass` line per offset assertion, zero `fail`.

#### Task C0-2 — Substrate SSBO (persistent-mapped triple-buffered)

- [ ] **Step 2.1** — Create `gpu_cull_substrate.h`:
  ```cpp
  namespace gpu_cull {
  void substrate_init(uint32_t maxActors);
  void substrate_shutdown();
  void substrate_frameBegin();          // resets per-frame staging state
  void substrate_submitDynamicActor(const GpuActorRecord& rec);
  void substrate_flushUpload();         // writes header.recordCount, advances ring
  GLuint substrate_getInstanceSsboName();
  uint32_t substrate_getInstanceSsboBindingPoint();
  bool   substrate_isEnabled();         // env-probe MC2_GPU_CULL_SUBSTRATE; default 0 in C0 standalone, 1 once C1 lands.
  } // namespace gpu_cull
  ```
- [ ] **Step 2.2** — In `gpu_cull_substrate.cpp`, mirror the persistent-mapped pattern from `gos_static_prop_batcher.cpp:248,261,1251,1667`:
  - `RING_FRAMES = 3` (matches `STATIC_PROP_RING_FRAMES`).
  - `glBufferStorage(GL_SHADER_STORAGE_BUFFER, RING_FRAMES * (sizeof(Header) + maxActors*sizeof(GpuActorRecord)), nullptr, GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_MAP_WRITE_BIT)`.
  - `glMapBufferRange(...)` with `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT` to obtain a stable mapped pointer.
  - Per-slot `GLsync s_uploadFence[RING_FRAMES] = {0}`. `glClientWaitSync(GL_TIMEOUT_IGNORED)` only at shutdown — per-frame the ring depth is enough to never collide. Identical to static prop batcher.
- [ ] **Step 2.3** — `substrate_frameBegin()` selects next slot; if its fence is unsignaled, `glClientWaitSync` (defensive — should never fire at 60Hz with a 3-deep ring per the static-prop precedent).
- [ ] **Step 2.4** — `substrate_submitDynamicActor()` increments local count and writes record into the slot's record region. `substrate_flushUpload()` writes the header (with `recordCount`), inserts a fence (`s_uploadFence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)`).
- [ ] **Step 2.5** — `[GPU_CULL v1] event=substrate_ready records=N capacity=M slot=K` summary line on every flush (gated by 600-frame counter, mirroring `[TGL_POOL v1] summary` — every-frame would be too noisy). One immediate event on first flush of a session for canary visibility.
- [ ] **Step 2.6** — Killswitch: `MC2_GPU_CULL_SUBSTRATE=0` (default in C0 isolated; bumped by C1 once compute is ready) → `substrate_isEnabled()` returns false; `submitDynamicActor` becomes no-op; no allocation. Lazy env probe (advisor sharpening #1).

**Verification:**
- AMD canary (RX 7900 XTX 26.3.1): mission load to game, leave running 30 s with `MC2_GPU_CULL_SUBSTRATE=1`. Expect monotonic `records=N` line every 600 frames, no GL errors, no fence stalls (compare frame time vs `=0` baseline; expect Δ ≤ 0.1 ms).
- `[GL_ERROR v1]` clean.

#### Task C0-3 — Dynamic-actor enumeration + per-frame upload

The enumeration site is the existing `GameObjectManager::update` body at `code/objmgr.cpp:1680` (signature `void GameObjectManager::update (bool terrain, bool movers, bool other)`). The dynamic-actor *iteration* loops are already there:

- mechs: `objmgr.cpp:1808-1822` (`ZoneScopedN("GameLogic.Units.Mechs")`)
- vehicles: `objmgr.cpp:1826-1840` (`ZoneScopedN("GameLogic.Units.Vehicles")`)
- turrets: `objmgr.cpp:1850-` (`ZoneScopedN("GameLogic.Units.Turrets")`)
- gates / specialBuildings / terrainObjects: `1717-1800`

We do NOT add a new iteration. We piggyback on the existing iteration order so `actorId == objList[handle]` is stable and the substrate's ordering matches `objList[]`.

- [ ] **Step 3.1** — Add a new helper in `code/objmgr.cpp` (file-scope static) `static void emitGpuCullRecord(GameObjectPtr obj)` that:
  - Reads `obj->getAppearance()` (cast to the appropriate XAppearance subtype where needed) for `position`, the cached AABB sphere-radius, `inView` (used as transient `prevVisibilityBit` source in C0 only), `consumerFlags` (derived from RTTI / class).
  - Constructs the `GpuActorRecord`, calls `gpu_cull::substrate_submitDynamicActor(rec)`.
- [ ] **Step 3.2** — Call `gpu_cull::substrate_frameBegin()` once at the top of `GameObjectManager::update` (line ~1680). Call `gpu_cull::substrate_flushUpload()` once at the very end of the function, after all iteration paths.
- [ ] **Step 3.3** — Inside each existing iteration (mechs/vehicles/turrets/gates/specialBuildings/terrainObjects), add `if (gpu_cull::substrate_isEnabled() && obj->getExists()) emitGpuCullRecord(obj);` immediately after the existing `update()` call. This keeps the legacy gate cascade *intact* — `update()` still runs, `MC2_DESTROY` still fires on its `update_false` path. The substrate just observes.
- [ ] **Step 3.4** — `consumerFlags` mapping (read-only RTTI — no virtual call cost when disabled):
  - `BattleMech*` → `Consumer_AIGate | Consumer_WeaponSpawnNode | Consumer_LifecycleGate | Consumer_RenderGate`
  - `GroundVehicle*` → same as mech
  - `Turret*` → `Consumer_RenderGate | Consumer_LifecycleGate` (no AI fire-decision through `canBeSeen` — verified at recon §6 turret rows)
  - `Gate*` / `Building*` / `TerrainObject*` → `Consumer_RenderGate | Consumer_LifecycleGate`
  - generic / artillery → same as building.
- [ ] **Step 3.5** — Defensive: if `substrate_submitDynamicActor` is called more than `recordCapacity` times in a frame, the substrate clamps + emits `[GPU_CULL v1] event=substrate_overflow at=N cap=M`. Capacity sized at `MAX_OBJECTS` (~10K) with 25% headroom.

**Verification:**
- `MC2_GPU_CULL_SUBSTRATE=1`, mc2_01 30s smoke: `[GPU_CULL v1] event=substrate_ready records=N capacity=M` line shows N = total (mechs + vehicles + turrets + gates + specialBuildings + terrainObjects + generics) — verify by hand against `numMechs`/`numVehicles`/etc. logged at mission load.
- `[DESTROY v1]` count delta vs `=0` baseline = 0 across tier1 5/5.
- Frame-time delta ≤ 0.1 ms at wolfman zoom (uploads are 64 B × 10K = 640 KB/frame; persistent-mapped write-combined; trivial).

#### Task C0-4 — AABB parity check

- [ ] **Step 4.1** — Create `GameOS/gameos/gpu_cull_parity.h/.cpp`. Env-gate `MC2_GPU_CULL_AABB_PARITY=1` (lazy probe).
- [ ] **Step 4.2** — At `substrate_flushUpload()` time, *before* fence insertion, walk the staged records and for each one:
  - Re-derive the legacy AABB from the same actor by calling a new helper `legacyAabbFromAppearance(obj)` that mirrors `XAppearance::recalcBounds`'s extent computation — but read-only (no side effect on `inView`). For mechs: read `Mech3DAppearance::position` + per-shape extents; for GVs: `GVAppearance` analog; for static actors: `BldgAppearance::extents`.
  - Compare to `rec.worldAabbMin`/`worldAabbMax` with epsilon 0.01 world units (positions are integer-ish in MC2).
  - On mismatch: `[GPU_CULL v1] event=parity_mismatch actor=<id> cat=<n> legacyMin=(...) recMin=(...) Δ=(...)`. Counter accumulates.
- [ ] **Step 4.3** — 600-frame summary line `[GPU_CULL v1] event=parity_summary mismatches=N total=M`.
- [ ] **Step 4.4** — Parity is single-run (advisor sharpening #4): both producers run side-by-side in the same process, comparing per-record at submit time. NEVER byte-diff across two runs.

**Verification:**
- Tier1 5/5 with `MC2_GPU_CULL_AABB_PARITY=1`: `mismatches=0` on the summary at end of each mission.
- If mismatches surface: stop the slice, fix before flip. Expected mismatch types are RTTI-class issues (e.g., a missed actor subtype). NOT acceptable: silent geometric drift.

#### Task C0-5 — Memory file + index entry

- [ ] **Step 5.1** — Create `~/.claude/projects/A--Games-mc2-opengl-src/memory/track_c_compute_cull.md` with: substrate schema decisions, AABB parity outcome, AMD canary findings, killswitch state at flip date.
- [ ] **Step 5.2** — Add MEMORY.md index entry under "Rendering / shaders": `- [Track C — compute cull substrate (C0 shipped)](track_c_compute_cull.md) — dynamic actor visibility records...`

#### Task C0-6 — Soak + flip

- [ ] **Step 6.1** — Tier1 5/5 with `MC2_GPU_CULL_SUBSTRATE=1` and `MC2_GPU_CULL_AABB_PARITY=1` — must PASS with `mismatches=0` and `[DESTROY v1]` identity.
- [ ] **Step 6.2** — Adversarial review subagent: REQUIRED PROMPT: "use the adversarial-plan-review skill in `.claude/skills/`" verbatim. Subject: this slice's diff + parity output.
- [ ] **Step 6.3** — Flip default: `MC2_GPU_CULL_SUBSTRATE` default → `1` once C1 lands (NOT in C0 standalone — C0 is dark by default).
- [ ] **Step 6.4** — Update memory file + commit.

---

## Slice C1 — Compute cull for render draw only (condensed plan)

**Goal:** A single combined compute dispatch reads the C0 substrate + Track B static-prop SSBO, performs frustum + distance test (using the GLSL port of A1's `clipSpaceFrustumAdmit`), atomically scatter-writes per-bucket visible-ID lists with `instanceCount` patched into a pre-built `DrawElementsIndirectCommand` array. `glMultiDrawElementsIndirectCount` consumes the result. CPU lifecycle gates STILL read legacy `inView` — C1 is GPU→GPU only (Q12 explicit slice boundary). No CPU readback in C1 except optional debug telemetry.

**Sizing:** ~1 week.

**Hard exit criteria:**
- `[GPU_CULL v1] event=dispatch_ok dispatched=N visible=M elapsed_us=K` line each frame; K ≤ 100 µs at wolfman zoom on 7900 XTX.
- Parity check `MC2_GPU_CULL_PARITY=1` shows `mismatches=0` across tier1 5/5 (compares GPU visibility decisions to legacy `inView`).
- `[GL_ERROR v1]` clean. AMD canary (apitrace or RenderDoc capture) verifies pre/post-barrier state per Q12.
- Tier1 5/5 PASS in both modes.
- `[DESTROY v1]` identity vs baseline (lifecycle still on CPU, so this should trivially hold; assert it anyway).
- Render output visually identical (single-run side-by-side via screen-rect overlay split with `MC2_GPU_CULL_SPLIT_OVERLAY=1` — left half legacy draw, right half GPU-cull draw; manual canary).
- Tracy `Cull.ComputeDispatch` zone visible at ~50 µs target. `Camera.UpdateRenderers` Tracy zone unchanged in C1 (lifecycle gates not yet routed).

### Q-decisions locked at C1 plan-time

- **Q8 dispatch shape**: single combined dispatch with bucket-keyed scatter via `atomicAdd` into per-bucket counters.
- **Q9 compute output**: visible-ID list with atomic compaction (the `gl_BaseInstance` consumer); the bitmask is a *secondary* output for C2 readback (not consumed in C1).
- **Q11 bucket key**: `(mesh-range, shader-program, texture-binding-set, VAO, index-type)`. Mission-load builds bucket descriptor table; one `DrawElementsIndirectCommand` per bucket with mesh-static fields populated; `instanceCount` GPU-written.
- **Q12 sync contracts (post-advisor-pass-2)** — see §C1-Sync below for the full list.
- **Q13 GL baseline**: GL 4.6 OR (`ARB_indirect_parameters` + `ARB_shader_draw_parameters`) probed at startup; refuse activation if neither.

### C1-Sync — Synchronization contracts (full Q12 list, REQUIRED reading)

This section is normative. Implementations MUST satisfy every bullet; the AMD canary in Task C1-7 verifies them.

- **Counter representation:** per-bucket counters are `uint` fields in the cull-output SSBO, updated via shader `atomicAdd(buckets[b].instanceCount, 1u)`. NOT ACBOs (`GL_ATOMIC_COUNTER_BUFFER`). This choice is a "choose-and-stick" per Q12 — switching to ACBOs later changes the barrier set.
- **Counter reset path:** at the start of each cull dispatch (CPU side, before `glDispatchCompute`), the SSBO region holding the per-bucket counters is cleared via `glClearNamedBufferSubData` (one call clearing all bucket counters; std430 offset known from the bucket-descriptor table). Must complete before the dispatch reads. Implicit ordering on the GL queue is sufficient — no explicit barrier between the clear and the dispatch (both target the same buffer).
- **Visible-list capacity + overflow:** per-bucket visible-ID list sized at mission load to **= total population of that bucket** (worst-case = every instance in the bucket is visible — frustum + camera at infinity). Shader-side bounds check: `if (write_idx < bucket.capacity) visibleIds[base + write_idx] = id;` else `atomicAdd(overflow_counter, 1)`. A non-zero `overflow_counter` at frame end emits `[GPU_CULL v1] event=overflow bucket=B count=N` and triggers a one-shot capacity bump path (logged for operator-visible canary). NEVER silently drop.
- **`instanceCount` patch location:** GPU writes per-bucket `instanceCount` directly into the pre-built `DrawElementsIndirectCommand` array. std430 offset = 4 bytes (the `instanceCount` field is the *second* `uint` in `DrawElementsIndirectCommand` per OpenGL spec). The compute shader writes `cmds[bucket].instanceCount = visibleCount[bucket]` at end-of-dispatch (one invocation per bucket, gated by `if (gl_LocalInvocationIndex == 0)` if dispatched per-bucket; or atomic since multiple writes to the same scalar is benign for the equality case — prefer the gated path for clarity).
- **Memory barriers (POST-ADVISOR-PASS-2 LOCKED):**
  - Cull dispatch → indirect draw consumption: `glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT)`. The SSBO bit covers BOTH the visible-ID list AND the in-SSBO atomic counters (since they're SSBO-resident `uint` fields under the choose-and-stick decision).
  - Cull dispatch → CPU async readback (C2 only, NOT C1): `glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT)` plus `glFenceSync` for completion signalling.
  - **DO NOT add `GL_ATOMIC_COUNTER_BARRIER_BIT`.** That barrier bit is for ACBOs specifically; using it on SSBO-resident `atomicAdd` counters is a cargo-cult error. Code review and adversarial review must catch any reintroduction.
- **C1 slice boundary:** GPU→GPU only. ZERO CPU readback in C1 except optional `[GPU_CULL v1] event=dispatch_ok` telemetry that copies a single `uint64_t` (timer query) and the overall `visibleCount` total — both are non-load-bearing diagnostics. The full readback ring + fence + fallback path begins in C2.

**Recon-level grep precedent at write time:** the engine has exactly one existing `glMemoryBarrier` call site, at `gos_terrain_patch_stream.cpp:910` (`GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT`). C1 will be the first `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` site in the codebase. There is no precedent to mirror — the contract above is normative on its own.

### C1 condensed task list

#### Task C1-1 — GL version probe + extension banner

- [ ] Add startup probe in `gpu_cull_compute.cpp` (lazy-init):
  ```cpp
  bool gpu_cull::probeGLSupport() {
    GLint major=0, minor=0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    bool hasIndirectCount = (major>4) || (major==4 && minor>=6) ||
                             GLEW_ARB_indirect_parameters;
    bool hasDrawParams    = (major>4) || (major==4 && minor>=6) ||
                             GLEW_ARB_shader_draw_parameters;
    bool ok = hasIndirectCount && hasDrawParams;
    fprintf(stderr, "[GPU_CULL v1] gl_version=%d.%d support=%s\n",
            major, minor,
            (major==4 && minor>=6) ? "4.6" :
            (ok ? "extensions" : "none"));
    return ok;
  }
  ```
- [ ] Refuse activation (`gpu_cull::compute_isEnabled()` returns false) when probe returns false. Killswitch `MC2_GPU_CULL=0` overrides anyway.

#### Task C1-2 — Compute program build path

- [ ] Mirror `makeProgram(...)` shape but for `GL_COMPUTE_SHADER` only. Single CS source. `#version 430` prefix passed in (matches existing convention).
- [ ] Compile failure: hard-fail (`STOP("[GPU_CULL] compute compile failed")`). Mirrors the `[INSTR v1]` startup banner shader-error pattern.

#### Task C1-3 — `gpu_cull.comp` shader source (sketch)

```glsl
#version 430
layout(local_size_x = 64) in;

#include "gpu_cull_predicate.glsl"   // GLSL port of A1's clipSpaceFrustumAdmit

struct GpuActorRecord {
    vec3 worldCenter; float boundingRadius;
    vec3 worldAabbMin; uint category;
    vec3 worldAabbMax; uint flags;
    uint actorId; uint prevVisibilityBit;
    uint consumerFlags; uint _pad0;
};
struct GpuActorRecordHeader { uint recordCount; uint recordCapacity; uint visibleCount; uint _pad0; };
struct DrawElementsIndirectCommand {
    uint count; uint instanceCount; uint firstIndex; uint baseVertex; uint baseInstance;
};

layout(std430, binding = 0) readonly  buffer Records  { GpuActorRecordHeader hdr; GpuActorRecord recs[]; };
layout(std430, binding = 1) writeonly buffer Visible  { uint visibleIds[]; };          // bucket-keyed
layout(std430, binding = 2) coherent  buffer Commands { DrawElementsIndirectCommand cmds[]; };
layout(std430, binding = 3) coherent  buffer Counters { uint perBucketCount[]; uint overflowCount; };
layout(std140, binding = 0) uniform   FrustumPlanes { vec4 planes[6]; mat4 viewProj; };

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= hdr.recordCount) return;
    GpuActorRecord r = recs[i];

    // Sphere-frustum cull (fast path).
    bool admit = true;
    for (int p = 0; p < 6 && admit; ++p) {
        admit = dot(planes[p].xyz, r.worldCenter) + planes[p].w >= -r.boundingRadius;
    }
    // Predicate lockstep with A1: clip-space test on AABB corners as the
    // tighter envelope when sphere passes (mirrors clipSpaceFrustumAdmit).
    if (admit) {
        admit = clipSpaceFrustumAdmit(viewProj, r.worldCenter, r.boundingRadius);
    }
    if (bool(r.flags & 0x1u)) admit = true;   // Flag_AlwaysVisible

    if (!admit) return;

    uint bucket = (r.category >> 4) & 0x0FFFFFFFu;     // bucket bits packed in category
    uint baseInBucket = ...; // resolved from bucket descriptor SSBO (not shown)
    uint capInBucket  = ...;
    uint slot = atomicAdd(perBucketCount[bucket], 1u);
    if (slot >= capInBucket) {
        atomicAdd(overflowCount, 1u);
        return;
    }
    visibleIds[baseInBucket + slot] = r.actorId;

    // instanceCount patch — last invocation per bucket triggers the write.
    // Implementation: write unconditionally with an `atomicMax` on the
    // per-bucket count into cmds[bucket].instanceCount, OR emit a small
    // patch dispatch after the cull dispatch (preferred for clarity).
}
```

The `instanceCount` patch is split into a *second* tiny dispatch (`gpu_cull_patch.comp`, one invocation per bucket) that reads `perBucketCount[]` and writes `cmds[bucket].instanceCount`. This avoids the order-dependency of writing inside the same dispatch as the compaction. Memory barrier `GL_SHADER_STORAGE_BARRIER_BIT` between cull dispatch and patch dispatch; then `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` between patch dispatch and the indirect draw.

#### Task C1-4 — Bucket descriptor table (mission-load)

- [ ] Walk Track B's static-prop registry + C0's dynamic actor type table; group by Q11 tuple (mesh-range, shader, texture-set, VAO, index-type). Result: `vector<BucketDescriptor>` with up to ~50 entries on stock missions.
- [ ] Pre-build `DrawElementsIndirectCommand[N_buckets]` with `count`, `firstIndex`, `baseVertex` set; `instanceCount` set to 0; `baseInstance` set to the bucket's slot in `visibleIds[]`. Upload via `glBufferStorage` (mission-immutable).
- [ ] Per-bucket capacity sized at mission load = total population of bucket × 1.10 headroom.

#### Task C1-5 — Per-frame dispatch hook

- [ ] In `code/mission.cpp` between `land->geometry()` (line 500) and `ObjectManager->update` (line 505):
  ```cpp
  gpu_cull::substrate_flushUpload();   // already added in C0
  if (gpu_cull::compute_isEnabled()) {
      gpu_cull::compute_dispatch(eye->getViewProjection(), eye->getFrustumPlanes());
  }
  ```
  The dispatch:
  1. `glClearNamedBufferSubData` to reset per-bucket counters and overflow counter.
  2. `glDispatchCompute(ceil(recordCount/64), 1, 1)` for cull.
  3. `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)`.
  4. `glDispatchCompute(N_buckets, 1, 1)` for instanceCount patch.
  5. `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)` — visible to indirect draw.

#### Task C1-MB — Multi-bind for bucket texture binding (cheap drop-in, ~2 lines)

`GL_ARB_multi_bind` (GL 4.4 core) lets us bind N textures in one call:
`glBindTextures(unit_first, count, &textures[0])`. Track C1's bucket
draws bind multiple textures per dispatch (cement multi-sampler at
tex3 + colormap at tex0 + atlases at others). Replacing the loop of
single `glBindTexture` calls with one `glBindTextures` reduces driver
overhead per dispatch.

- [ ] Identify bucket-bind sites in C1's per-frame draw setup (likely
  in `gos_terrain_indirect.cpp` or wherever C1 wires its indirect
  draws).
- [ ] Replace the per-slot bind loop with a single `glBindTextures`
  call passing the texture array.
- [ ] No behavior change; verify via `[GL_ERROR v1]` clean run.

Cost: ~2-line replacement. Win: one driver call per bucket instead
of N. Won't materially shift Tracy zone numbers at our bucket count
but it's free and clean.

#### Task C1-6 — Indirect draw consumption

- [ ] Replace the relevant indirect-draw call in `code/gamecam.cpp` / `gameos_graphics.cpp` to use `glMultiDrawElementsIndirectCount` (or `glMultiDrawElementsIndirect` with a host-known max count, fed by the post-cull command buffer). Vertex shader reads `gl_BaseInstance + gl_InstanceID` to index into `visibleIds[]` then into the instance SSBO.
- [ ] Sampler state inheritance trap (per `memory/sampler_state_inheritance_in_fast_paths.md`): explicitly bind the right sampler before draw — do NOT inherit.
- [ ] Depth state inheritance trap (per `memory/gpu_direct_depth_state_inheritance.md`): explicitly enable `GL_DEPTH_TEST | GL_LEQUAL` — do NOT inherit.
- [ ] Blend state inheritance trap (per `memory/blend_state_inheritance_in_post_process.md`): `glDisable(GL_BLEND)` before draw; do NOT inherit.

These three traps are explicitly called out because **every new fast path in this engine has hit them** (`memory/gpu_direct_renderer_bringup_checklist.md` traps #5, #6, #7).

#### Task C1-7 — AMD canary verification

- [ ] Apitrace OR RenderDoc capture of one frame at wolfman zoom on AMD 7900 XTX 26.3.1.
- [ ] Verify pre-/post-barrier SSBO state: per-bucket `instanceCount` matches `perBucketCount[]` after the patch barrier. Visible-ID list contents match what the indirect draw consumes (sampled by capturing the draw call's vertex pulls).
- [ ] Add finding (clean or otherwise) to `docs/amd-driver-rules.md`. If the barrier fails to flush, escalate — but the SSBO + atomicAdd pattern is well-trodden on this driver.

#### Task C1-8 — Parity instrumentation `MC2_GPU_CULL_PARITY=1`

- [ ] Single-run capture (advisor sharpening #4): both legacy `inView` and GPU compute decision active. Per-actor: compare `inView == (gpuVisibilityBit != 0)`. Mismatch envelope reviewed before flip (mirrors A1 dual-run pattern from Q3): expected migration disagreements (e.g., near-edge clip differences) are characterized; out-of-envelope mismatches are hard failures.

#### Task C1-RB — Block-active rollup (per Q17)

**Required by Q17.** `objmgr::update` (C3 gate target) gates at block
granularity (`objBlockInfo[].active`), not per-actor granularity. Per-actor
visibility from C1's compute pass does not auto-answer per-block visibility.
This task adds the explicit rollup. Pick ONE path before continuing:

**Path A — GPU compute aggregation pass.**
- [ ] Add a small kernel `gpu_cull_block_rollup.comp` that scans the
  per-actor visibility output and emits per-block OR-reduction into a
  per-block visibility SSBO (one bit per block). One additional dispatch.
- [ ] Add barrier between primary cull and rollup:
  `GL_SHADER_STORAGE_BARRIER_BIT`. Add barrier between rollup and any
  consumer reading the per-block buffer.
- [ ] Per-actor → block index mapping: each `GpuActorRecord` already
  stores actor identity; lookup of `obj->blockIdx` happens at C0 record
  build time. Verify: every actor's `blockIdx` is captured in its record
  before C0 ships.

**Path B — CPU-side conservative rollup.**
- [ ] In C2's async readback path, after reading the per-actor visibility
  bitmask, walk `objBlockInfo[].firstHandle..firstHandle+numObjects` and
  OR per-actor bits into per-block bits. Stays on CPU; no extra GPU
  dispatch.
- [ ] Cost: O(blocks × per-block actor count). At MC2 mission scale
  this is ~thousands of operations per frame; trivial on CPU.

**Decision gate:** the choice is documented at C1 plan-time. Path A is
preferred if the GPU dispatch overhead is below the CPU walk cost on
AMD RX 7900 XTX (canary measures both); Path B is preferred if the
extra barrier set adds calendar-time risk.

Rollup output is consumed by C3's `objmgr::update` gate. Without this
task, C3's gate handoff is incomplete; `objBlockInfo[].active` cannot
be cheaply derived from per-actor visibility alone.

#### Task C1-9 — Soak + flip + memory update

- [ ] Tier1 5/5 with `MC2_GPU_CULL=1` PASS.
- [ ] Adversarial review (REQUIRED PROMPT verbatim).
- [ ] Default flip pending C2 if any mid-soak surprise; otherwise stays opt-in (`MC2_GPU_CULL=0` default) until C3 ships, since C1 alone gives no FPS win without lifecycle-gate handoff.

---

## Slice C2 — Async readback into non-lifecycle consumers (detailed sketch)

**Goal:** Add a 3-frame readback ring buffer + per-frame `glFenceSync` + three-tier fallback, and feed frame-N-1 visibility into ONE non-lifecycle consumer (smallest blast radius first, per Q14's open follow-up). Validates the readback infrastructure end-to-end before C3 routes lifecycle gates.

**Sizing:** ~3-5 days.

**Hard exit criteria:**
- `[GPU_CULL v1] event=readback_ok slot=K stale_frames=N visibleCount=M` line each frame.
- Three-tier fallback exercised via test hook `MC2_GPU_CULL_FORCE_FENCE_NOT_READY=1`: sees `readback_fallback_n2`, then `readback_fallback_conservative`. Verify *all three* tiers in selftest (advisor sharpening #2, hard-fail).
- Tracy plot or `[GPU_CULL v1]` summary line shows GPU-derived visibility count tracking CPU-derived count within ±N (envelope of the 1-frame lag).
- NEVER blocks on `glClientWaitSync` in the render hot path — verified by Tracy zone showing zero `Wait` time on `Cull.Readback` zone at steady state.
- Tier1 5/5 PASS in both `MC2_GPU_CULL_READBACK={0,1}` modes.

### C2 sketch

#### Readback ring buffer

- 3-frame ring, persistent-mapped: `glBufferStorage(...,GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT)`. Size = `RING_FRAMES * (sizeof(VisibilityHeader) + maxActors * sizeof(uint8_t))` — byte-flag layout (Q9-recon section 9 candidate B), NOT bit-packed; simplest, ~10 KB total at our scale.
- Mirrors `gos_static_prop_batcher.cpp:248` ring shape (existing `RING_FRAMES = 3`).
- Per slot: `GLsync s_readbackFence[RING_FRAMES] = {0}`, `void* s_mapped[RING_FRAMES]`, last-good slot index `s_lastGoodSlot`.

#### Per-frame flow

1. **Frame end (post-cull, post-draw):** `glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT)` (per Q12 C2 contract). Write fence `s_readbackFence[currentSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)`.
2. **Frame begin (next frame):** `tryConsume()`:
   - Try slot for frame N-1 first. `glClientWaitSync(fence, 0, 0)` (zero timeout — non-blocking poll).
     - If `GL_ALREADY_SIGNALED` or `GL_CONDITION_SATISFIED`: read `s_mapped[slot]`, update `s_lastGoodSlot`, return slot pointer + status `Tier1_NMinusOne`.
     - If `GL_TIMEOUT_EXPIRED`: fall through to N-2.
   - Try slot for frame N-2: same poll.
     - If signaled: return slot pointer + status `Tier2_NMinusTwo` + emit `[GPU_CULL v1] event=readback_fallback_n2 slot=K stale_frames=2`.
     - If still not ready: tier 3.
   - Tier 3: `Tier3_Conservative`. Return a sentinel that the consumer interprets as "all visible." Emit `[GPU_CULL v1] event=readback_fallback_conservative`.
3. **Render hot path NEVER calls `glClientWaitSync(GL_TIMEOUT_IGNORED)`.** That would block the CPU; the ring depth + tier-3 fallback exists specifically so the hot path is wait-free.

4. **After consuming a slot (any tier 1/2 path):** call
   `glInvalidateBufferSubData(readbackBuf, slotOffset, slotSize)`.
   Tells the driver "this slot's data is dead — feel free to reuse
   storage" before we re-map it for the next frame. Without
   `glInvalidateBufferSubData` the driver may ghost the buffer (allocate
   shadow storage) on re-map to avoid CPU-GPU sync, wasting VRAM.
   `GL_ARB_invalidate_subdata` (4.3 core) — drop-in. Skipped on tier-3
   (no real slot consumed).

#### Test hook for fallback path coverage

`MC2_GPU_CULL_FORCE_FENCE_NOT_READY=1` makes the fence appear unsignaled for N frames (N=1 → exercises tier-2; N=2 → exercises tier-3). Selftest at startup runs all three tiers; hard-fail on any unexercised path (advisor sharpening #2).

#### Proof-of-life consumer

Pick at C2 plan time per Q14 follow-up. Recommended: **Tracy plot** of "GPU-visible count (frame N-1)" vs "CPU-visible count (frame N)". Smallest blast: a Tracy plot is purely diagnostic; misreads cause neither lifecycle nor render artifacts. Acceptance: 30 s of mc2_01 gameplay shows the GPU plot tracking CPU within an envelope (±200 actors at zoom transitions, zero at steady state).

#### C2 task summary

- [ ] **C2-1:** Create `gpu_cull_readback.{h,cpp}` ring + fence + tryConsume.
- [ ] **C2-2:** Wire `frameEnd()` from `code/mission.cpp` (post-render, near `eye->postRender` call).
- [ ] **C2-3:** Add bitmask output to `gpu_cull.comp` (secondary write; doesn't change visible-ID list path).
- [ ] **C2-4:** Test hook + 3-tier selftest. Hard-fail if any tier path fails to fire.
- [ ] **C2-5:** Tracy plot consumer wiring.
- [ ] **C2-6:** Tier1 soak + adversarial review + memory update.

---

## Slice C3 — Gate handoff + Camera::UpdateRenderers stub (detailed sketch)

**Goal:** Route every `inView`/`canBeSeen()` consumer (from recon §5–§6 audit) to read GPU-derived visibility from the C2 readback (frame N-1). After all sites flip, `Camera::UpdateRenderers` body becomes a stub. Q7 1-frame artifact accepted; visual canaries (zoom-transition, camera-jump, first-contact, weapon-spawn) verify gameplay tolerance.

**Sizing:** ~1 week.

**Hard exit criteria:**
- `Camera::UpdateRenderers` Tracy zone drops from ~6 ms self-time to ≤ 0.3 ms (stub overhead only).
- Net FPS gain ≥ 30% at wolfman zoom on tier1 5/5 (the roadmap's Track C exit criterion).
- `[DESTROY v1]` count delta vs baseline = 0; identity-tuple match (kind + reason + gate-state-snapshot) per advisor sharpening #5.
- Visual canaries pass: zoom-transition (RAlt+wolfman in/out), camera-jump (mission start cam pan), first-contact (enemy reveal), weapon-spawn (PPC/MECH_EXPLOSION at first-fire).
- Tier1 5/5 PASS.

### C3 routing — per-consumer call-site list (every site grep-verified)

Each site below was grep-verified at write time against the recon. The patch shape is identical at every site:

```cpp
// Old:
if (appearance->inView) { ... }
// New:
if (gpu_cull::isVisible(actorId)) { ... }
```

`gpu_cull::isVisible(actorId)` reads the latest readback slot's byte flag for that actor (via the C2 ring). Frame-N-1 lag is intrinsic to the API.

#### Lifecycle / update gates (the cascade — preserve!)

- `code/objmgr.cpp:1693-1702` — `framesSinceActive` accumulator. Reads `inView_instr() || canBeSeen_instr() || blockActive_instr()`. Route: `gpu_cull::isVisible(obj->getHandle()) || gpu_cull::isBlockActive(obj->getBlockNum())`.
- `code/objmgr.cpp:1760` — per-block active gate: `if (Terrain::objBlockInfo[terrainBlock].active)`. Route: `if (gpu_cull::isBlockActive(terrainBlock))`. **CRITICAL**: the *cascade* (block→object) MUST be preserved per `cull_gates_are_load_bearing.md`. Only the *input* changes.
- `code/objmgr.cpp:1768` — per-vertex gate: `Terrain::objVertexActive[objList[objIndex]->getVertexNum()]`. Route: `gpu_cull::isVertexActive(...)`. Same cascade preservation.
- `mclib/mech3d.cpp:2055-2069` — `Mech3DAppearance::update()` body's `if (inView) { ... }` block.
- `mclib/mech3d.cpp:2377` — TGL-pool allocation guard (`memory/tgl_pool_exhaustion_is_silent.md`). PRESERVE the gate; only flip its input.
- `mclib/mech3d.cpp:2881` — render-time path `if (inView && visible)`.
- `mclib/mech3d.cpp:3740,3797` — animation-state branches.
- `mclib/mech3d.cpp:4180-4196` — first-frame init special case (`turn < 3 || inView || g_useGpuStaticProps`). Preserve `turn < 3` short-circuit.
- `mclib/gvactor.cpp:1946,1982,2022,2039,2711` — GV equivalent gates.
- `mclib/bdactor.cpp:1082-1494,1561,2012,2112,2258-2268,3872-4199,4205,4208,4454,4555,4570` — building/terrain-object gates. Already partially served by `g_useGpuStaticProps`; replace with GPU visibility for the actor-class subset.
- `mclib/genactor.cpp:569-1204` — generic-actor gates (fences, artillery towers).
- `code/terrobj.cpp:610-698,797,799,869` — TerrainObject prime + update.
- `code/bldng.cpp:791-805,1066,1071` — Building update + render.
- `code/gate.cpp:335-355,596,599` — Gate update + render.
- `code/turret.cpp:575-808,2034,2048` — Turret update + render + canBeSeen consumers.
- `code/artlry.cpp:870,1183-1242,1334,1407` — Artillery (excluding save/load at `1746,1764` which is freeze-frame and stays CPU).

#### AI-decision gates (the Q7 visible artifact frontier)

- `code/mech.cpp:6448` — `if (appearance->canBeSeen())` ambient combat. Route to GPU.
- `code/mech.cpp:6466` — `if (appearance->canBeSeen())` ambient combat. Route to GPU.
- `code/mech.cpp:6497` — `if (attackRange == FIRERANGE_CURRENT && !isDisabled() && appearance->canBeSeen())` — **THE primary fire decision**. Route to GPU. 1-frame artifact: enemy waits 1 frame to fire on newly-revealed target.
- `code/gvehicl.cpp:3928,3936` — GV equivalents.

Per Q7 the artifact is **accepted**. The reversibility hedge (separate `gpu_cull::isVisibleSync(actorId)` API that *does* block on the fence at specific call sites) is NOT pre-built — documented for content-layer use later if testers complain.

#### Weapon-spawn-node queries (the Q7 weapon-bolt artifact)

- `mclib/mech3d.cpp:721` — `getWeaponNodePosition`: `if (!inView) return false;`. Route to GPU. Artifact: bolt spawns at object root for 1 frame on visibility transition.
- `mclib/mech3d.cpp:759` — `getNodeNamePosition`: same shape.
- `mclib/mech3d.cpp:795` — `getNodeIdPosition`: same.
- `mclib/mech3d.cpp:833` — `getNodePosition`: same.
- `mclib/gvactor.cpp:445` — `getWeaponNodePosition`.
- `mclib/gvactor.cpp:500` — `getSmokeNodePosition`.
- `mclib/gvactor.cpp:533` — `getDustNodePosition`.

(Lines 5099/5136 in mech3d.cpp are calls *into* the same accessors — no edit needed at those call sites; the early-out lives at the accessor.)

#### Special cases — DO NOT route

These call sites consume `inView` but for reasons orthogonal to GPU cull. Leave them alone:

- `code/mover.cpp:3470-3471` — `getLOSPosition` saves `oldInView`, forces `setInView(true)`, queries node, restores. Already overrides; route does nothing useful here.
- `code/light.cpp:123-124` — light's own `onScreen()` populates the bit; that's the *producer* side. The consumer is `lightAppearance->setInView()` which feeds the cascade. Route lights through the same GPU path only if Track B's static-prop registry includes them — otherwise lights stay CPU (out of Track C scope per recon §5).
- `code/weaponbolt.cpp` `bool inView` — **local variable**, not `Appearance::inView`. Out of scope.
- `code/clouds.cpp:212,222` — same: local variable, not `Appearance::inView`.
- `code/terrain.cpp:1430-1452,1485,1590-1632` — local `bool inView` from `eye->projectForTerrainAdmission`. Producer side; replaced by GPU compute in C1's terrain bucket, not here.
- `code/artlry.cpp:1746,1764` — save/load `data->inView = inView`. Freeze-frame; keep CPU.

### Camera::UpdateRenderers stubbing

After all routes flip:
- `code/mechcmd2.cpp:692` Tracy zone retained.
- Body collapses to: dispatch C1 cull (already happens at `mission.cpp:500-505`), no per-actor recalcBounds calls.
- Verification: Tracy capture shows the zone drops from ~6 ms to ≤ 0.3 ms.

### C3 task summary

- [ ] **C3-0 (preflight, REQUIRED before any rewiring):** Q18 lights audit. Run:
  ```bash
  grep -rn "->inView\|lightAppearance.*inView\|isInView" code/ mclib/ \
    | grep -v "^code/light.cpp" \
    | grep -v "^mclib/.*Appearance" \
    > /tmp/c3-q18-light-consumers.txt
  ```
  Audit each match. For every consumer that reads `lightAppearance->inView` (or any `inView` field reachable from a light), document whether it gates a lifecycle / gameplay decision OR is purely cosmetic. **If any lifecycle-gating consumer exists, lights JOIN the C3 routing list above** as a third routed producer alongside object admission and terrain admission. The recon classified lights out-of-scope; that classification is correct ONLY if this preflight finds zero lifecycle-gating consumers. Document the audit result in the memory file alongside the standard C3 routing list.
- [ ] **C3-1:** Add `gpu_cull::isVisible(actorId)` and `gpu_cull::isBlockActive(blockIdx)` accessors to `gpu_cull_readback.h`. Backed by C2's ring + tier-fallback.
- [ ] **C3-2:** Per-consumer rewiring — one commit per file in the list above (lifecycle, then AI, then weapon-spawn). Killswitch `MC2_GPU_CULL_LIFECYCLE=0` keeps each site on legacy `inView`. Default per file: `=1` only after that file's tier1 5/5 + visual canary PASS.
- [ ] **C3-3:** Visual canaries per Q7 — explicit scenario list:
  - Zoom-transition: RAlt+wolfman repeated 10 times — verify no missing render frames.
  - Camera-jump: mission_load mc2_01 → 30 s — verify enemy reveal at intro pan has 1-frame delay (acceptable).
  - First-contact: mc2_03 (multi-team) — first PPC volley fires from correct hardpoint after enemy reveal; first frame may show root-spawn (acceptable artifact).
  - Weapon-spawn: cycle through PPC/MECH_EXPLOSION effects — 1-frame root-spawn at zoom-transition is the only artifact.
- [ ] **C3-4:** `Camera::UpdateRenderers` body stubbed.
- [ ] **C3-5:** Tier1 5/5 + perf gate ≥30% FPS at wolfman zoom.
- [ ] **C3-6:** Adversarial review (REQUIRED PROMPT verbatim).
- [ ] **C3-7:** Identity diff for DESTROY: capture baseline `[DESTROY v1]` events from tier1 (kind + reason + (visibility-bit, blockActive, vertexActive) tuple); compare post-flip; require identity match. Per advisor sharpening #5.
- [ ] **C3-8:** Memory update + MEMORY.md index.
- [ ] **C3-9:** Default flip `MC2_GPU_CULL=1` for tier1; document reversibility hedge in memory file.

---

## Cross-slice Verification Appendix

Every claim in this plan that names a symbol/file:line was grep-verified at write time. The high-load-bearing ones:

| Claim | Verification |
|---|---|
| `Appearance::inView` field at `mclib/appear.h:71` | grep `mclib/appear.h` — confirmed `bool inView;` at line 71. |
| `canBeSeen()` returns `inView` at `appear.h:171` | grep — confirmed `return(inView);`. |
| Combat AI consumer at `code/mech.cpp:6497` | grep — confirmed `if (attackRange == FIRERANGE_CURRENT && !isDisabled() && appearance->canBeSeen())`. |
| AI same-family at `code/mech.cpp:6448, 6466` | grep — confirmed both `if ( appearance->canBeSeen() )`. |
| `Terrain::objBlockInfo[].active` consumer at `objmgr.cpp:1760` | grep — confirmed `if (Terrain::objBlockInfo[terrainBlock].active)`. |
| `objVertexActive` consumer at `objmgr.cpp:1768` | grep — confirmed pattern. |
| `Mech3DAppearance::recalcBounds` body at `mech3d.cpp:2073` | grep — confirmed `bool Mech3DAppearance::recalcBounds (void)`. |
| `GpuStaticPropInstance` schema at `gos_static_prop_batcher.h:13` | grep — confirmed `struct alignas(16) GpuStaticPropInstance` + `static_assert(sizeof == 112)`. |
| Persistent-mapped + RING_FRAMES=3 at `gos_static_prop_batcher.cpp:72,100,257` | grep — confirmed `constexpr uint32_t RING_FRAMES = 3`, `GLsync s_fence[RING_FRAMES]`, `glClientWaitSync` ring loop. |
| Existing `glMemoryBarrier` site at `gos_terrain_patch_stream.cpp:910` | grep — confirmed `glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT)`. THIS IS THE ONLY EXISTING `glMemoryBarrier` SITE — Track C will be the first `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` site in the engine. No precedent to mirror. |
| `glMultiDrawArraysIndirect` precedent at `gameos_graphics.cpp:2423` | grep — confirmed. The `Count` variant is a strict superset. |
| `GameObjectManager::update` signature at `objmgr.cpp:1680` | grep — confirmed `void GameObjectManager::update (bool terrain, bool movers, bool other)`. |
| Compute shader infrastructure: zero existing | grep `glDispatchCompute|GL_COMPUTE_SHADER|local_size_x` excluding `3rdparty/` returned 0 matches. Confirmed the recon's claim. |

**Negative claims explicitly verified (per `feedback_data_flow_audit_asymmetry.md`):**

- "C2 does not block the render hot path" — verified by reading `gos_static_prop_batcher.cpp:257-259` (`glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED)` is in the SHUTDOWN path, not per-frame) and confirming the ring-depth-3 pattern hides per-frame fence waits at 60Hz.
- "No new GL extension required for compute shaders at GL 4.3" — verified: `local_size_x`, `glDispatchCompute`, `atomicAdd`, `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` are all GL 4.3 core. Only `glMultiDrawElementsIndirectCount` requires the GL 4.6 / `ARB_indirect_parameters` upgrade (Q13 probe handles this).

---

## Risk register (consolidated, cross-slice)

| ID | Risk | Slice | Mitigation |
|---|---|---|---|
| R-C0-1 | C0 record schema mismatches GLSL on AMD due to std430 stride. | C0 | Lockstep `static_assert` + GLSL declaration in same commit (memory `cpp_glsl_ubo_struct_lockstep.md`). |
| R-C0-2 | AABB parity reveals an unseen dynamic-actor subtype not covered by `emitGpuCullRecord`. | C0 | Treat parity mismatch as STOP — fix subtype before flip. The AABB parity check exists specifically to catch this class. |
| R-C1-1 | AMD compute+atomic regression on 7900 XTX 26.3.1 (Q12 specifies the canary verification). | C1 | Apitrace/RenderDoc capture pre/post-barrier; add finding to `docs/amd-driver-rules.md`; killswitch keeps default off until canary passes. |
| R-C1-2 | Cargo-cult `GL_ATOMIC_COUNTER_BARRIER_BIT` reintroduced by reviewer. | C1 | Explicit prohibition in plan + adversarial review checks for the pattern. |
| R-C1-3 | Bucket capacity overflow at scaling event (artillery spawn, mid-mission reinforcement). | C1 | Shader-side bounds check + `[GPU_CULL v1] event=overflow` + capacity bump path. NEVER silently drop. |
| R-C1-4 | Sampler/depth/blend state inheritance breaks the indirect draw (the well-trodden GPU-direct trap). | C1 | Per Task C1-6, all three state types explicitly bound/disabled at draw time. Adversarial review verifies. |
| R-C2-1 | Fence not ready at frame N-1 AND N-2 → tier-3 conservative-visible spuriously fires. | C2 | Selftest (advisor sharpening #2) hard-fails if the tier-3 path fires more than M times per session at steady state. M = 1 acceptable; >1 = bug. |
| R-C2-2 | Persistent-mapped readback racing with compute writes on AMD. | C2 | `GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT` per Q12 contract. Verified at canary. |
| R-C3-1 | Q7 1-frame artifact judged unacceptable by user testing. | C3 | Reversibility hedge: introduce `gpu_cull::isVisibleSync()` (blocks on fence) at specific sites — content-layer concern. Documented; not pre-built. |
| R-C3-2 | DESTROY-cascade regresses (gate input wrong, `update()` returns false, object destroyed). | C3 | Identity-tuple diff (kind + reason + (visibility-bit, blockActive, vertexActive) snapshot) per advisor sharpening #5. Hard fail on any net-new destroy. |
| R-C3-3 | TGL pool exhaustion regresses because the gate at `mech3d.cpp:2377` flips with stale input. | C3 | Gate is PRESERVED (Q7 framing) — only the input changes. Pool size unchanged. Tracy `[TGL_POOL v1]` summary tracked across flip. |
| R-C3-4 | First-launch black/no-terrain intermittency interacts with GPU cull init order. | C3 | Lazy-init env probes (advisor sharpening #1); no static-init-order surface. Compute program builds at startup like every other shader. |

---

## References

**Engine code (grep-verified at write time):**
- `code/objmgr.cpp:1680,1693-1702,1760,1768,1808,1826,1850` — update + cascade gates.
- `code/mission.cpp:465-526` — frame ordering, dispatch hook target.
- `code/mech.cpp:6448,6466,6497` — AI gates.
- `code/mechcmd2.cpp:692` — `Camera.UpdateRenderers` Tracy zone (target for C3 stub).
- `mclib/appear.h:71,171,175` — `inView` field, `canBeSeen()`, `setInView()`.
- `mclib/mech3d.cpp:721,759,795,833,2055,2073,2377,2881,3740,3797,4180-4196` — mech consumers + recalcBounds.
- `mclib/gvactor.cpp:445,500,533,1946,1982,2022,2039,2711` — GV consumers.
- `mclib/terrain.cpp:484,1306,1359,1479,1483,1500` — terrain producer + Tracy zones.

**GPU substrate (grep-verified):**
- `GameOS/gameos/gos_render.cpp:184` — GL context request (the 4.3 → 4.6 bump or extension-probe site, per Q13).
- `GameOS/gameos/gos_static_prop_batcher.cpp:72,100,248,257,261,1251,1667` — RING_FRAMES, persistent-mapped, fence ring template.
- `GameOS/gameos/gos_static_prop_batcher.h:13-35` — `GpuStaticPropInstance` 112 B std430 schema (template for `GpuActorRecord`).
- `GameOS/gameos/gos_terrain_patch_stream.cpp:910` — only existing `glMemoryBarrier` call site in the engine.
- `GameOS/gameos/gameos_graphics.cpp:2423` — `glMultiDrawArraysIndirect` production site.

**Specs / brainstorms / recons:**
- `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` (Q7-Q14, post-advisor-pass-2 canonical).
- `docs/superpowers/explorations/2026-05-06-track-c-compute-cull-recon.md`.
- `docs/superpowers/mc3-rendering-modernization-roadmap.md` Track C.
- `docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md` (predicate semantics — lockstep source for `gpu_cull_predicate.glsl`).

**Memories (load-bearing constraints):**
- `memory/cull_gates_are_load_bearing.md` — cascade hazard; preserved at every gate site.
- `memory/gpu_direct_renderer_bringup_checklist.md` — 9 traps, all relevant to C1's indirect draw.
- `memory/cpp_glsl_ubo_struct_lockstep.md` — schema lockstep rule.
- `memory/parity_finds_gpu_substrate_bugs_visual_smoke_misses.md` — parity discipline.
- `memory/sampler_state_inheritance_in_fast_paths.md`, `memory/gpu_direct_depth_state_inheritance.md`, `memory/blend_state_inheritance_in_post_process.md` — C1 indirect draw state hygiene.
- `memory/tgl_pool_exhaustion_is_silent.md` — C3 gate-input-change must NOT change pool semantics.
- `memory/feedback_data_flow_audit_asymmetry.md` — negative-claim verification rule applied throughout the cross-slice verification appendix.
- `memory/feedback_dont_overgate_during_iteration.md` — applied: env flags are per-slice, not per-mission.

**Worktree CLAUDE.md sections in force:**
- "Documentation Discipline — grep at write-time, not after."
- "Review Discipline — adversarial by default for architectural endpoints."
- "Critical Rules — Stock install must remain playable; build RelWithDebInfo; #version 430 prefix."
- "Load-Bearing Cull Infrastructure — READ BEFORE TOUCHING."
- "Debug Instrumentation Rule — env-gated `[GPU_CULL v1]` prints in same commit."

---

## Revision log

- **2026-05-06 (initial)** — full plan written from canonical Q1-Q14 decisions (post-advisor-pass-2). C0 fully implementable (~3-5 day slice with explicit schema + parity tasks); C1 condensed (~1 week, full Q12 sync contracts inlined as normative §C1-Sync); C2 + C3 detailed sketches with per-consumer file:line list.
