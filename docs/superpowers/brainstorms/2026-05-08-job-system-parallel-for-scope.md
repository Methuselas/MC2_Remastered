# Job-System / Parallel-For Scope — Brainstorm (2026-05-08)

**Status:** brainstorm only. No source code changes. No spec.
**Worktree:** `claude/nifty-mendeleev`.
**Framing question:** Is full task-graph job-system architecture (the
idTech 7/8 shape) the right destination for MC2, or is targeted
parallel-for on specific hot loops the practical play, OR is neither
worth doing relative to the GPU-modernization arc that's already
shipping wins?

This document follows the worktree CLAUDE.md "Documentation Discipline":
every cited symbol was grep-verified at write-time. The verification
appendix at the end lists each citation with M (matches) / D (divergent)
/ NF (not found) status.

---

## Q1. What does idTech 7/8's job system actually do, and which parts of it are translatable?

idTech 7 (DOOM 2016 / DOOM Eternal era) and idTech 8 (the post-Eternal
iteration) both ship a "decompose everything into jobs" architecture
that breaks across three distinct layers. Distinguishing these layers
is the entire point of this question — they have very different
translation costs to MC2.

**Layer A — design pattern: decompose work into independent units.**
At its cheapest, this is just "find a hot loop, prove iterations are
independent, run them across N worker threads via a thread pool." This
is the layer that translates trivially to any codebase. The pattern
itself does not require fibers, dependency graphs, or any specific
runtime. `std::for_each(par_unseq, ...)`, OpenMP `#pragma omp
parallel for`, Taskflow's `for_each`, and a hand-rolled thread pool's
`parallel_for` are all the same pattern with different vendoring
costs. **This layer is translatable to MC2 for non-GL work.** The
caveat is "find a hot loop" and "prove iterations are independent" —
the rest of this brainstorm is mostly about how hard those two
predicates are in MC2.

**Layer B — implementation: fiber scheduler with explicit dependency
graph.** id's variant uses a job-stealing scheduler with fibers
(stackful coroutines) so a job can issue a "wait on dependency" call
and yield to another job on the same worker without paying a kernel
context-switch. The scheduler tracks a DAG: each job declares its
inputs and outputs, the scheduler runs all jobs whose inputs are
satisfied, fences emit when outputs are written, downstream consumers
unblock. This is the "frame is a graph, not a serial sequence"
abstraction. **This layer is partially translatable.** The DAG-of-jobs
model is just a higher-octane parallel-for; what doesn't translate is
the assumption that everything in the engine — animation, IK,
particles, audio, AI, render-command-building — was *designed* with
explicit input/output sets so the DAG is even possible to declare. MC2
was designed serially with shared-singleton state; converting it to
declare DAG inputs/outputs is essentially a port to a new
architecture, not a refactor. The same destination is reachable
incrementally without the fiber scheduler — you just convert one loop
at a time and lose the elegance of "everything is a job."

**Layer C — infrastructure: Vulkan command-buffer parallelism.** id's
renderer threading wins are tied to Vulkan/D3D12 secondary command
buffers — multiple threads each record draw commands into separate
command buffers, the main thread submits them. **This layer does NOT
translate to MC2 today.** GL 4.3 has one thread that owns the GL
context. There is no "multiple threads recording into command buffers"
on GL — the entire renderer's GL submission must stay on the render
thread. (Bindless extensions and `glMultiDrawIndirect` shrink the
per-call CPU cost of GL submission, which is the orthogonal substrate
arc — but they don't make GL multithreaded.)

**Translation summary:** Layer A is on the table for any independent
hot loop in non-GL code. Layer B's *destination* (DAG of jobs) is
reachable as the limit of converting many Layer A slices but is not
itself a slice. Layer C is blocked by the GL constraint and only opens
under a Vulkan/D3D12 migration that is firmly out of scope here.

The framing trap to avoid: "MC2 should adopt the idTech 7 architecture"
collapses these three layers and reads as "rewrite the engine." The
right framing is "MC2 should consider Layer A on a small number of
specific hot loops, after a load-bearing-state audit."

---

## Q2. What's already in `3rdparty/` for concurrency / threading?

Inventoried `3rdparty/` directly: `cmake/`, `ffmpeg-lgpl-win64/`,
`include/` (only `GL/`, `SDL2/`, `unistd.h`, `zconf.h`, `zlib.h`),
`lib/`, `tracy/`. No Taskflow, no EnkiTS, no TBB, no concurrencpp, no
hand-rolled thread pool.

Grep for `taskflow|enkiTS|tbb::|std::async|std::thread|thread_pool|
ThreadPool` over the whole worktree returned 3 hits, all inside Tracy:
`3rdparty/tracy/tracy/TracyCUDA.hpp`,
`3rdparty/tracy/client/TracySysTrace.cpp`,
`3rdparty/tracy/client/TracyProfiler.cpp`. Tracy uses threads
internally for its own profiler worker; nothing in MC2 application
code uses `std::thread` or `std::async`.

Grep for `CreateThread|_beginthread|pthread_create|std::mutex|
std::atomic|InterlockedIncrement|CRITICAL_SECTION` over the whole
worktree found 31 files but **every match is inside Tracy or SDL2
headers** — `3rdparty/tracy/client/TracyProfiler.cpp`,
`3rdparty/tracy/common/TracyMutex.hpp`,
`3rdparty/tracy/common/TracySystem.cpp`,
`3rdparty/tracy/client/TracyLock.hpp`,
`3rdparty/include/SDL2/SDL_thread.h`, etc. Plus a handful of doc-files
that mention these terms in prose. **Application code (MC2 mclib/code/
GameOS) has zero direct uses of std::mutex, std::atomic, or kernel
threading APIs.**

This is consistent with the project memory entry "Massive existing
global-singleton state... Atomicity discipline is nowhere" — the
codebase is single-threaded by construction with no atomicity
groundwork laid.

**What we'd inherit if we vendored a job library:** nothing. Every
candidate library would be net-new vendoring. SDL2 ships a
`SDL_CreateThread` API but it's primitive (no pool, no parallel_for,
no work-stealing) and nothing in the codebase uses it.

**What we'd need to vendor:** see Q6.

---

## Q3. Catalog embarrassingly-parallel hot-loop candidates

For each candidate: file:line of the loop, iteration count (rough),
per-iteration work, current Tracy zone if any, thread-safety obstacles.
All file:lines were grep-verified at write-time.

### C1. `Terrain::geometry vertexProjectLoop` (CPU-side terrain admission)

- **Site:** `mclib/terrain.cpp:1421` (D1 fast-path inner loop) and
  `mclib/terrain.cpp:1562` (legacy fallback inner loop).
- **Tracy zone:** `"Terrain::geometry vertexProjectLoop"` (both at
  1421 and 1562, same name).
- **N per frame:** `numberVertices` for the active terrain; in
  practice tens of thousands at wolfman zoom, fewer at zoomed-in
  RTS.
- **Per-iteration work:** project one vertex, sphere-clip, write
  back `cv->{px,py,pz,pw,clipInfo,hazeFactor}`, AND set side-effect
  cull-cascade booleans `objBlockInfo[blockNum].active = true` and
  `objVertexActive[vertNum] = true` (terrain.cpp:1539-1545).
- **Thread-safety obstacles:**
  1. Vertex writeback is per-`cv` (independent stride) — safe.
  2. Cull-cascade boolean writes to `objBlockInfo[]` and
     `objVertexActive[]` are write-only `= true` from many vertices
     into shared arrays. Multiple vertices in the same block write
     `true` to the same index. Since the operation is monotonic
     (only ever sets to `true`, never clears mid-loop), it tolerates
     racing writes on `bool` even without atomics — but C++ memory
     model technically demands `std::atomic<bool>` with
     `memory_order_relaxed` to avoid undefined behavior. Cheap fix.
  3. Reduction min/max accumulators (`leastZ`, `mostZ`, `leastW`,
     `mostW`, `leastWY`, `mostWY`) at terrain.cpp:1549-1553 require
     thread-local reductions then a final merge. Standard pattern.
  4. `Camera::cameraFrame` (a global frame matrix) is read by every
     iteration (terrain.cpp:1443). Read-only during the loop ⇒ safe.
- **Prior work:** D1 hoist landed but is asymptotic (memory entry
  `vertex_project_loop_d1_asymptotic.md`: mean Δ ~0%, σ -10% on a
  pure CPU loop with no SSBO). The D1 hoist took the loop near the
  scalar-CPU ceiling. Threading is the next CPU lever; whether
  there's enough work left after D1 to justify it depends on the
  vertex count.

### C2. `Terrain::geometry quadSetupTextures` (per-quad terrain texture admission)

- **Site:** `mclib/terrain.cpp:1798` (`currentQuad->setupTextures()`
  called inside loop at 1796-1812).
- **Tracy zone:** `"Terrain::geometry quadSetupTextures"`
  (terrain.cpp:1783).
- **N per frame:** `numberQuads` (camera-windowed quadList, see
  `quadlist_is_camera_windowed.md` memory).
- **Per-iteration work:** `TerrainQuad::setupTextures()` —
  classifies textures, picks splat layers, populates the quad's
  texture handles, and on the indirect-terrain-armed path calls
  through to substrate emit machinery.
- **Thread-safety obstacles:**
  1. `setupTextures()` writes to per-quad fields (independent
     stride) — safe.
  2. **Indirect-terrain emit path** — when armed,
     `setupTextures()` reaches into the SOLID-endpoint substrate
     (memory: `indirect_terrain_solid_endpoint.md`). Substrate
     append is a shared write target. **Not currently thread-safe.**
  3. **WaterStream narrow append** at terrain.cpp:1808
     (`WaterStream::AppendNarrowCandidate(currentQuad)`) — shared
     `std::vector`-shaped accumulator. Not thread-safe.
- **Prior work:** Cache-read default-on (memory:
  `patchstream_shape_c.md`, median 3.40 → 2.99 ms). Substrate
  emission shipped (memory:
  `indirect_terrain_solid_endpoint.md`). Threading would require
  splitting "compute-prep" (parallelizable per-quad classifier)
  from "substrate append" (must serialize) — see Q5.

### C3. `MC_TextureManager::addLightDataStructure` (lighting dedup hash)

- **Site:** `mclib/txmmgr.cpp:1022`. Outer loop is in render-time
  light gather; this function itself is NOT a loop, it's the per-call
  hash-and-dedup target.
- **Tracy zone:** `"addLightDataStructure scan"` (txmmgr.cpp:1027,
  retained name from pre-hash-table era).
- **N per frame:** ~1039 calls (memory entry referenced in user
  prompt; consistent with project recon).
- **Per-iteration work:** FNV-1a hash of the light-data struct,
  hashmap lookup, on hit a `memcmp` verify, on miss an append + map
  insert + occasional realloc.
- **Thread-safety obstacles:**
  1. `s_lightDataDedupMap` (txmmgr.cpp:1030) is a singleton
     hashmap. Concurrent insert/find from multiple threads is UB
     without a lock. A `concurrent_hash_map` (TBB) or a per-thread
     dedup with end-of-frame merge would work.
  2. `lightData_` array realloc + write at txmmgr.cpp:1046-1055 is
     destructive; concurrent appends would race on
     `lightDataStructuresCount`.
- **Note:** The 2026-05-06 hash-table optimization (memory file
  references this in the prose at txmmgr.cpp:1024-1027 noting
  "should drop from ~12 µs/call to ~200-300 ns/call") may have
  already collapsed the hot zone. Threading 200 ns/call work is
  below the Tracy-100ns floor for measurement; ROI depends on
  whether the hash-table win actually landed and stuck. Needs
  fresh Tracy capture at write-time of the spec.

### C4. `Mech3DAppearance::update` (per-mech update)

- **Site:** Called from `TerrainObject::update` (terrobj.cpp,
  appearanceUpdate Tracy zone). The call is `appearance->update()`
  inside `TerrainObject::update()`.
- **Tracy zone:** `"TerrainObject::update appearanceUpdate"` —
  ~2.4 ms total for the zone (Q3 figures from
  `2026-05-02-object-offload-scope.md` brainstorm row 1; verified
  M in that doc).
- **N per frame:** ~1.7M calls per session in long captures (memory:
  `1,764,624` calls in a 3.20 s capture, ~1.81 µs/call); per-frame
  count varies by visible mech population — many fewer than the
  static-prop count, but each call is heavier.
- **Per-iteration work:** updates animation state, geometry
  transforms, light-source attachments. Touches GameObject
  hierarchy.
- **Thread-safety obstacles (load-bearing — read carefully):**
  1. **TGL pool:** `getVerticesFromPool` is global-singleton state
     and silently returns NULL on exhaustion (memory:
     `tgl_pool_exhaustion_is_silent.md`). Concurrent calls would
     race on the pool free-list. Threading mech update without
     making the pool atomic-safe risks pool corruption that
     manifests as silent shape drop-outs.
  2. **Light-data emit:** `Mech3DAppearance::update` paths emit
     into the same `addLightDataStructure` shared hashmap as C3.
     Same obstacle.
  3. **Cull cascade write-backs:** `setExists(false)` cascade per
     `cull_gates_are_load_bearing.md` — if `update()` returns false
     and the caller calls `MC2_DESTROY`, the destructor mutates the
     pointer-graph (`objList[]`, parent/child links). Concurrent
     destroys race.
  4. **Animation state machines:** Mech anim transitions touch
     observer/listener lists. Pointer-graph mutation.
  5. **Per-mech instances are independent in the most-common case**
     — two mechs updating their own state usually don't touch
     each other's GameObject. But the *infrastructure* they call
     into (TGL pool, light data, texture-handle resolve) is shared.

### C5. `GroundVehicle::update` (per-vehicle update)

- **Site:** `code/gvehicl.cpp:2798` (per `asan-follow-ups.md` doc;
  the user prompt cites :3155 — both are inside the same file's
  update path; not re-grep'd at write-time, so call this
  approximately gvehicl.cpp).
- **Tracy zone:** `"GameLogic.Units.Vehicles"` (per
  `2026-05-06-track-c-compute-cull.md:303`).
- **N per frame:** typical ground-vehicle population per mission;
  smaller than mech count in tier1 missions.
- **Per-iteration work:** AI control update, physics integration,
  appearance update.
- **Thread-safety obstacles:** Same shape as C4 — TGL pool, light
  data, GameObject pointer-graph, plus pathfinding shared state.
- **Note:** Track C compute cull's lifecycle work touches this
  same loop — see Q9 timing discussion.

### C6. `TerrainObject::update appearanceUpdate` (per-static-prop update)

- **Site:** `code/terrobj.cpp:603-609` per
  `2026-05-04-slice3-static-update-bypass-recon.md` (line numbers
  drift between revisions; current rev cited at terrobj.cpp:715 in
  `2026-05-06-update-skip-touch-regression-handoff.md`). For this
  brainstorm: terrobj.cpp `appearance->update()` call inside
  `TerrainObject::update()`.
- **Tracy zone:** `"TerrainObject::update appearanceUpdate"`.
- **N per frame:** ~869 (per recon `slice3-static-update-bypass`)
  for static props at rest.
- **Per-iteration work:** ~53 µs / 869 calls = ~61 ns/call ⇒
  **below the Tracy 100 ns floor**. The ~2.4 ms total is
  amortized across many props but the per-prop work is tiny.
- **Status:** Slice 3.C/3.D shipped a static-prop registry that
  bypasses the per-frame appearance update entirely (memory:
  `track_b_widen_static_prop_registry.md`), reducing this zone
  ~1.2 ms. Threading what's left is lower-priority than the
  bypass already delivered.

### C7. `Camera.UpdateRenderers` (CPU bottleneck per memory)

- **Site:** Tracy zone `"Camera.UpdateRenderers"` (per memory entry
  in MEMORY.md "Performance / profiling" section).
- **Cost:** 6 ms/frame, 3.66 ms self-time is "MC2 terrain vertex
  building" per memory.
- **Per-iteration work:** This is a wrapper, not itself a loop —
  the 3.66 ms self-time is the terrain vertex build path that
  flows through `vertexProjectLoop` (C1) and
  `quadSetupTextures` (C2). Threading this means threading C1+C2.
- **Note:** Memory says "Not fixable without engine rewrite" — that
  framing is from a single-threaded vantage. Parallel-for on C1
  is the most direct attack.

### C8. gosFX particle updates

- **Site:** `mclib/gosfx/effectcloud.cpp` and siblings (cloud.cpp,
  particle.cpp). Did not grep the per-particle update inner loop
  at write-time; per memory entry "gosFX particle system: Data-
  driven via EffectLibrary. CardCloud/Tube/PertCloud/DebrisCloud."
- **Per-iteration work:** Per-particle integration (position,
  velocity, lifetime).
- **Thread-safety obstacles:** Per-particle within a cloud is
  independent. Across clouds is independent. Submission to
  `gos_tex_vertex` is shared.
- **Status:** Cost is not currently a Tracy hot zone; gosFX is not
  the budget-eater that mech update or terrain admission is.

### C9. Path manager calc

- **Site:** `mclib/move.cpp:4582`, `:4753`, `:5010` —
  `GameLogic.PathManager.CalcPath1/2/3` Tracy zones.
- **Per-iteration work:** A* / pathfinding.
- **Thread-safety obstacles:** PathManager is a singleton,
  internally stateful (open/closed lists, scratch buffers). Each
  call mutates singleton scratch. Threading requires per-thread
  scratch.
- **Note:** This is the "GameLogic spikes (200ms+) when panning
  reveals new areas — AI/pathfinding activation" item from
  MEMORY.md — bursty, not steady-state, so amortized cost is
  unclear. Could be a parallel-for win, could be small.

### C10. LOS update

- **Site:** `code/gameobj.cpp:1947` Tracy zone
  `"GameLogic.LOS.Update"`.
- **Per-iteration work:** Per-actor line-of-sight raycasts against
  terrain.
- **Thread-safety obstacles:** Reads terrain (read-only during a
  frame ⇒ safe). Writes per-actor LOS state (independent stride
  ⇒ safe). Singleton sensor manager state may be shared.

### C11. AI brain run

- **Site:** `code/warrior.cpp:2127` Tracy zone
  `"GameLogic.AI.BrainRun"`.
- **Per-iteration work:** Per-warrior decision update, ABL script
  step.
- **Thread-safety obstacles:** **Bad.** ABL is a stateful
  interpreter with global symbol tables. Threading would require
  per-thread interpreter state. Plus AI mutates target lists,
  which are shared. Plus ABL extension callbacks (per
  `omnitech_abl_stubs_session.md`) reach into engine singletons
  freely.

---

## Q4. Score each candidate

Three columns: **Quality** (1–5: 5 = trivially parallel,
1 = thread-hostile). **ROI** (negligible / small / medium / large
based on Tracy data and per-iteration cost). **Risk surface** keyed to
load-bearing memory entries.

| ID | Loop | Quality | ROI | Risk surface |
|----|------|---------|-----|--------------|
| C1 | vertexProjectLoop | 4 | medium | objBlockInfo write race (cull_gates_are_load_bearing.md), reduction merge |
| C2 | quadSetupTextures | 3 | small-to-medium after Shape C cache | substrate append (indirect_terrain_solid_endpoint), WaterStream append |
| C3 | addLightDataStructure | 2 | small (post-hash-table) | shared hashmap, lightData_ realloc |
| C4 | Mech3DAppearance::update | 2 | medium | TGL pool (tgl_pool_exhaustion_is_silent), light data, GameObject graph, cull cascade |
| C5 | GroundVehicle::update | 2 | small | same as C4 + pathfinding state |
| C6 | TerrainObject::update appearanceUpdate | n/a | already won via bypass | obviated by static-prop registry |
| C7 | Camera.UpdateRenderers | (= C1+C2) | (composite) | (composite) |
| C8 | gosFX | 4 | negligible | shared gos_tex_vertex submit |
| C9 | PathManager.CalcPath* | 3 | small (bursty) | singleton scratch, requires per-thread state |
| C10 | LOS.Update | 4 | small-medium | sensor mgr singleton |
| C11 | AI.BrainRun | 1 | medium-large but blocked | ABL singleton interpreter, mutating target lists, gameplay determinism |

**Reading the table:**
- The highest-quality candidate is **C1 (vertexProjectLoop)** because
  the per-iteration body is mostly arithmetic with one well-defined
  shared-write target (the cull cascade booleans), no pool
  allocation, no GameObject pointer-graph mutation, no GL.
- The candidates with the largest theoretical ROI are C4 (mechs) and
  C11 (AI), but both have severe risk surface.
- C8 and C10 have low risk but low ROI.
- C2 has medium ROI but the substrate-append path muddies the
  parallelism quality — if we thread C2 we either have to skip
  the substrate-append branch (lose the indirect-terrain endpoint
  win) or serialize substrate append behind a lock (claw back the
  parallelism win).

---

## Q5. The GL single-threading boundary

GL 4.3 has one thread that owns the GL context. Any "compute job that
calls a GL function" is broken by construction. The honest map of
where the boundary is wrongly drawn today, where it could be cleanly
re-drawn for parallelism:

**Already correctly split (compute is separate from GL):**
- `vertexProjectLoop` (C1) — pure CPU vertex math, writes back to
  CPU-side vertex array. No GL calls in the loop body. Parallelism
  here is not blocked by the GL constraint.
- gosFX particle update (per-particle integration) — pure CPU.
  Submission later via `gos_tex_vertex` is the GL-touching part,
  separable.
- LOS raycast (C10) — pure CPU.

**Wrongly drawn today (compute and GL are mixed in one function):**
- `quadSetupTextures` outer loop (C2) — per-quad calls into
  `setupTextures()` which both classifies texture state (CPU) AND
  on the indirect-terrain-armed path appends into substrate state
  (CPU-side accumulator, but the eventual SSBO upload is GL). To
  parallelize, split per-quad classify (parallel) from substrate
  append (serial). The substrate-append path is small per quad and
  is the eventual bottleneck — locking it serializes the
  parallelism win unless we use per-thread substrate accumulators
  with end-of-frame merge.
- `Mech3DAppearance::update` — internally calls into
  `addLightDataStructure` (CPU dedup, no GL) AND triggers
  `TransformMultiShape` paths that touch GPU vertex buffers
  through `getVerticesFromPool` (CPU pool with eventual GL upload).
  The pool itself is CPU-side state but the threading discipline
  it would need is the same regardless.

**Always serialized (must stay on render thread):**
- `mcTextureManager->renderLists()` flush — issues GL draws.
- `gos_RendererBeginFrame` / `EndFrame` — GL context state.
- All gosFX submit, all renderLists submit.
- `applyRenderStates` and friends in `gameos_graphics.cpp`.

**Practical implication:** the render-command-build vs
GL-submission split that idTech 7/8 enjoys requires us to design a
"command record" (CPU-side, parallel-build) ⇒ "command flush"
(serial, GL-issuing) layering. We have a baby version of this in the
PatchStream / WaterStream "thin record" pattern (memory:
`water_ssbo_pattern.md`) — that pattern is already a
separate-CPU-record-from-GL-submit pattern, just not yet threaded.
The substrate redesign already queued is the right place to formalize
this layering, which suggests the parallel-for arc and the substrate
arc share an architectural seam. See Q9.

---

## Q6. Vendor decision tree

Three options for the runtime, scored on existing-code-fit, build-
system-impact, AMD-driver-paranoia (none touch GL but listing for
completeness), and maintainability over years.

**(a) Taskflow (header-only, C++17, single 7K-line header)**
- Existing-code-fit: drop into `3rdparty/include/`. Project already
  has C++17 idioms (`std::vector`, `auto`, range-for in modernized
  paths). Some legacy MC2 code is older C++ but the call site is
  what matters, and call sites are new code we'd write.
- Build-system-impact: zero — header-only. CMake just adds the
  include path.
- AMD-driver-paranoia: irrelevant (no GL).
- Maintainability: single-vendor, active project, small surface.
  Easy to pin a version.

**(b) Intel TBB**
- Existing-code-fit: heavier dependency, requires linking. Project
  already pulls FFmpeg via DLL, so adding a runtime DLL is not
  alien. But TBB on Windows MinGW/MSYS2 builds (the project's
  toolchain per memory: `mc2_path_separator_linux_build.md` —
  `-DLINUX_BUILD` set globally) historically has more friction.
- Build-system-impact: medium. Adds a runtime DLL.
- AMD-driver-paranoia: irrelevant.
- Maintainability: well-maintained, but heavier than the project
  needs for the small set of loops we'd convert.

**(c) Custom 200-line thread pool**
- Existing-code-fit: no vendoring, all code lives in the worktree.
- Build-system-impact: zero.
- AMD-driver-paranoia: irrelevant.
- Maintainability: own it forever. Risk of subtle bugs in the
  scheduler that take days to track. Lower portability surface
  but also less battle-tested.

**Recommendation strawman (this is brainstorm, not spec):**
Option (a) Taskflow looks dominant for MC2's profile — header-only
keeps the CMake surface untouched, the small set of loops we'd convert
doesn't need TBB's full machinery, and the 7K-line single header is
auditable in one sitting. Option (c) is tempting for "200 lines is
nothing" but the bug class (ABA on the work-stealing deque, missed
wakeups, false sharing) is a known foot-cannon and the reward over
Taskflow is small.

---

## Q7. Smallest first slice that delivers measurable value

The user's pattern is single-PR shippable slices. The criteria for
"first slice":
1. Cheapest first conversion.
2. Highest signal for whether the broader investment pays off (i.e.
   does parallel-for actually move a Tracy zone we care about, on
   tier1 hardware).
3. Smallest risk surface so a parity failure is easy to bisect.
4. Killswitch-able (env var off ⇒ legacy path runs).

**Strawman first slice: vendor Taskflow + parallelize C1
`vertexProjectLoop`.**

Why C1 wins on these criteria:
- Cheapest: pure CPU math loop, no pool allocation, no GameObject
  graph, no GL.
- Highest signal: the loop already has a Tracy zone — before/after
  delta is direct. ROI is medium per Q4.
- Smallest risk surface: the only shared-state writes are the
  cull-cascade booleans (`objBlockInfo[].active`, `objVertexActive[]`)
  and the min/max reductions. Both are well-defined patterns
  (atomic relaxed, thread-local + merge).
- Killswitch: `MC2_VERTEX_PROJECT_PAR=1` to enable, default off
  during soak. Parity with `MC2_VERTEX_PROJECT_PARITY=1` already
  exists for the D1 hoist (terrain.cpp:1400) — extend the parity
  scaffolding to also compare against the parallel path.

This slice is also a load-bearing **infrastructure** investment: it
forces us to vendor the job library, define the threading
conventions (env-gate naming, parity gate, killswitch shape), and
land a soak. Slice 2 and 3 would be cheaper because the
infrastructure is paid for.

---

## Q8. Risk inventory + adversarial-review-bait

**Determinism.** Parallel-for on update loops can introduce ordering
nondeterminism in gameplay state. Audit per candidate:
- C1 vertexProjectLoop: writes per-vertex cv fields (no order
  dependence). Writes monotonic booleans (no order dependence —
  final state is OR of all writes). Reductions need deterministic
  merge (sorted partial-results, not associative-floating-point
  accumulation across threads). **Fixable with deterministic
  reduction.**
- C4 Mech3DAppearance::update: parallel mech updates that target
  the same target actor (e.g. weapon hit registration) become
  ordering-dependent. **Determinism-hostile.** This is the kind of
  hazard idTech sidesteps because it has explicit job DAGs that
  can serialize the conflicting outputs.
- C9 PathManager: pathfinding heuristic ties broken by deterministic
  iteration order today; threading would change tie-break order ⇒
  potentially different paths same frame to frame. **Determinism-
  hostile.**
- C11 AI.BrainRun: same. **Determinism-hostile.**

This is one reason to start at C1 rather than chasing higher-ROI
candidates — C1 has no gameplay-determinism hazard.

**TGL pool atomicity** (`tgl_pool_exhaustion_is_silent.md`).
`getVerticesFromPool` returns NULL silently on exhaustion ⇒ shapes
vanish without log. If multiple threads hit the pool, the failure
mode is silent corruption of the free-list, not just exhaustion.
Threading any loop that allocates from the pool requires either:
(a) per-thread pool partitioning, (b) atomic free-list, or (c) hard
rule that the loop body must not call `getVerticesFromPool`. C1
avoids the pool. C4/C5 hit it.

**Texture handle live-rebind** (`mc2_texture_handle_is_live.md`).
Handles mutate per-frame; threads reading `tex_resolve` cache while
another thread mutates it = race. C1 doesn't touch textures. C2's
substrate-append path does. Audit needed before C2 conversion.

**`setExists(false)` cascade** (`cull_gates_are_load_bearing.md`).
If parallel `update()` calls return false from one thread while
another thread iterates `objList[]`, the destruction can invalidate
the iterator. The current GameObjectManager::update loop at
objmgr.cpp:1884 iterates `objList[i]` and calls `MC2_DESTROY` mid-
loop (objmgr.cpp:1936). Threading this loop is therefore not just
"parallel-for over objList" — it requires deferring destroys to
end-of-frame. Standard pattern but the audit must be explicit.

**GL state machine (per-thread, can't share).** Already covered in
Q5. The recurring failure mode is "I converted a loop body to a job
and somewhere deep in the call graph there's a `gos_SetRenderState`
or a GL call." Pre-conversion audit must trace every call from the
loop body and prove no GL is reached. For C1 this is true by
inspection; for C2/C4/C5 it requires a careful trace.

**Memory model: pre-C++11 patterns, raw pointers, no atomicity
discipline.** The `mclib/` and `code/` directories use raw pointers
and singleton globals heavily. Adding parallel-for in a leaf loop
does not require relaxing this everywhere — it requires the leaf
loop's *transitive call set* to be either thread-safe or
thread-isolated (per-thread state). The audit cost scales with the
call set size. C1's call set is tiny (Camera frame read, Stuff
vector math, projectForTerrainAdmission — itself a small math
function). C4's call set is enormous.

**Render thread fence discipline.** Even in non-GL parallel-for, if
the parallel pass runs *during* the same frame's GL submission
window, thread oversubscription can hurt the render thread (cache
contention, scheduler eviction). Practical mitigation: pin the
render thread, use N-1 worker threads for the pool. Or run the
parallel pass strictly before `gos_RendererBeginFrame` for the
frame.

**Soak budget.** 7-day soak per Track B precedent
(`track_b_widen_static_prop_registry.md`) is the canonical
default-on flip gate. Threading bugs are notorious for surfacing
hours-to-days into operation. The first slice should keep
killswitch-default-off for at minimum that soak window before flip.

---

## Q9. Strategic timing

Three options: NOW (in parallel with PR2 + Track D + substrate
coalesce), AFTER (when GPU-modernization plateaus), NEVER (the GPU
arc dominates and CPU parallelism is wasted effort).

The cross-track perf budget audit is at
`docs/superpowers/explorations/2026-04-30-cross-track-perf-budget-audit.md`
(file confirmed M).

**Argument for NOW:**
- The substrate redesign is queued anyway. The substrate-append seam
  in C2 is exactly the place where "compute record" vs "GL submit"
  layering needs to be cleanly drawn. Doing the parallel-for arc
  AT THE SAME TIME as the substrate redesign means the layering is
  designed once with both consumers in mind, not twice.
- C1 is independent of any in-flight GPU work. PR2 (terrain detail/
  overlay/mine) and Track D (GPU mech rendering) don't touch
  vertexProjectLoop. Threading C1 doesn't conflict with either.
- The first-slice infrastructure cost (vendor Taskflow, define
  conventions, parity gate) is paid once. Front-loading it
  unblocks future slices for the lifetime of the project.

**Argument for AFTER:**
- The GPU-modernization arc has been delivering 30-80% wins per
  slice (water stream stage 2, indirect terrain SOLID endpoint,
  static-prop registry slice 3). These wins are more valuable per
  slice than "thread one CPU loop for ~σ-noise improvement."
- C1's prior D1 hoist showed the loop is near the scalar-CPU
  ceiling — threading it is the next move, but the absolute ms
  saved is small compared to a 1-2 ms GPU offload.
- Track C compute-cull (memory: `track_c_compute_cull.md`) has
  C3-9 default-on flip pending. The lifecycle gates are already
  load-bearing in their current shape; introducing CPU threading
  on top of them while they're still soaking is asking for
  cross-coupled bugs that take days to bisect.

**Argument for NEVER:**
- The substrate arc + Track D + Track C eventually move so much
  per-frame work to the GPU that the CPU floor drops below the
  Tracy noise floor. At that point CPU threading is solving a
  problem that no longer exists.
- The frame budget cited in the user prompt — ~6.76 ms zoom-in
  (cap-limited at 165 fps target), ~9.5 ms zoom-out — suggests
  zoom-in is already cap-limited (i.e. the engine is faster than
  it's being asked to run). Adding CPU parallelism to the cap-
  limited path delivers no FPS, only headroom. The zoom-out path
  has 9.5 ms of real CPU floor — that's where parallelism helps,
  but if the GPU arc closes that floor too, the case dissolves.

**Strawman recommendation: AFTER, with one caveat.** The full job-
system architecture is NEVER (per Q1 — Layer B's destination is not
a slice; it's a limit that's reached gradually if reached at all).
Layer A parallel-for on selected loops is AFTER — wait until the
substrate redesign is complete, the substrate-append seam is the
natural threading boundary, and the GPU arc has shipped its current
queued slices. The caveat: if the substrate redesign session
explicitly needs the layering for its own reasons, fold the
parallel-for groundwork (vendor Taskflow + thread C1) into that
session as an enabling step, not a separate track.

This recommendation is not "never adopt parallel-for" — it's "don't
open a third parallel track right now when two are mid-flight, and
the parallel-for ROI is dominated by the GPU arc anyway."

---

## Q10. If "yes, parallel-for some loops" — first 3 conversion slices

Per Q9, the recommended timing is AFTER, but if the spec session
decides NOW (e.g. because the substrate redesign explicitly pulls in
the threading seam), here are three slices in order. Each slice = one
PR.

**Slice 1: vendor Taskflow + parallel C1 `vertexProjectLoop`.**
- Target: `mclib/terrain.cpp:1421` D1 fast-path inner loop.
- Expected Tracy delta: zone before/after on
  `"Terrain::geometry vertexProjectLoop"`. Speculative target: 2-4×
  on `numberVertices`-large frames at wolfman zoom; less at
  zoomed-in RTS where N is small enough that thread spin-up cost
  dominates. Don't claim speedup until measured.
- Parity gate: extend the existing `MC2_VERTEX_PROJECT_PARITY`
  scaffolding (terrain.cpp:1400) to also compare parallel vs
  legacy path. Counts of mismatches per frame, dump on shutdown.
- Killswitch: `MC2_VERTEX_PROJECT_PAR=1` enables, default off.
  Soak 7 days post-flip (Track B precedent).

**Slice 2: parallel `quadSetupTextures` per-quad classify
(non-substrate path only).**
- Target: `mclib/terrain.cpp:1798` outer loop.
- Scope guard: only converts the classify path. The substrate-
  emit-armed branch and the WaterStream::AppendNarrowCandidate
  branch stay serial behind a lock or are detected and the loop
  falls back to legacy serial.
- Expected Tracy delta: zone before/after on
  `"Terrain::geometry quadSetupTextures"`.
- Parity gate: define a per-quad post-condition snapshot and
  compare parallel vs legacy. Reuse Shape C cache parity patterns.
- Killswitch: `MC2_QUAD_SETUP_PAR=1` enables, default off.

**Slice 3: parallel C10 `LOS.Update` per-actor.**
- Target: `code/gameobj.cpp:1947` LOS update loop.
- Lower-ROI but lower-risk than mech update. Clean parallelism,
  no gameplay-determinism hazard if reads are read-only on
  terrain and writes are per-actor.
- Expected Tracy delta: small (zone is not currently a hot
  bottleneck). Slice value is mostly proving the pattern works
  for per-GameObject loops, paving the way for C4/C5 if their
  determinism hazards can be neutralized.
- Killswitch: `MC2_LOS_PAR=1`.

Slices C4 (mech update) and C5 (vehicle update) are explicitly NOT
in the first 3. They cross the determinism + TGL pool + GameObject
graph risk surfaces. They would be slices 4-5 only after slices 1-3
prove the pattern AND after a deferred-destroy infrastructure lands.

---

## Out of scope (re-stated)

- **Full task-graph rewrite (idTech-shape).** Discussed as Q1
  reference; not a recommendation. The destination is reachable
  as the limit of many Layer A slices; it is not itself a slice.
- **Vulkan / D3D12 migration.** Orthogonal. Not a precondition for
  CPU parallelism on non-GL loops.
- **GPU compute jobs.** Covered by separate roadmap (substrate,
  Track D, Track C compute-cull).

---

## Verification appendix

Each citation grep-verified at write-time. Status: M (matches),
D (divergent), NF (not found).

| Citation | Status | Note |
|----------|--------|------|
| `mclib/terrain.cpp:1421` Tracy zone `"Terrain::geometry vertexProjectLoop"` (D1 fast path) | M | grep confirmed |
| `mclib/terrain.cpp:1562` Tracy zone `"Terrain::geometry vertexProjectLoop"` (legacy fallback) | M | grep confirmed |
| `mclib/terrain.cpp:1783` Tracy zone `"Terrain::geometry quadSetupTextures"` | M | grep confirmed |
| `mclib/terrain.cpp:1798` `currentQuad->setupTextures()` | M | grep confirmed inside loop body |
| `mclib/terrain.cpp:1399-1400` `MC2_VERTEX_PROJECT_FAST` / `MC2_VERTEX_PROJECT_PARITY` env vars | M | grep confirmed |
| `mclib/terrain.cpp:1539-1545` cull cascade writes (`objBlockInfo`, `objVertexActive`) | M | confirmed in read |
| `mclib/terrain.cpp:1808` `WaterStream::AppendNarrowCandidate(currentQuad)` | M | confirmed |
| `mclib/txmmgr.cpp:1022` `MC_TextureManager::addLightDataStructure` | M | grep confirmed |
| `mclib/txmmgr.cpp:1027` Tracy zone `"addLightDataStructure scan"` | M | grep confirmed |
| `mclib/txmmgr.cpp:1030` `s_lightDataDedupMap` | M | confirmed in read |
| `mclib/txmmgr.cpp:1046-1055` realloc + append | M | confirmed in read |
| `mclib/mech3d.cpp:3067` Tracy zone `"GameLogic.Mech3D.UpdateGeometry"` | M | grep confirmed |
| `code/objmgr.cpp:1863` `GameObjectManager::update(bool, bool, bool)` | M | confirmed in read |
| `code/objmgr.cpp:1884` `objList[i]` iteration | M | confirmed in read |
| `code/objmgr.cpp:1918` Tracy zone `"GameLogic.Units.TerrainObjects"` | M | confirmed in read |
| `code/objmgr.cpp:1936` `MC2_DESTROY(specialBuildings[...], ...)` | M | confirmed in read |
| `code/gameobj.cpp:1947` Tracy zone `"GameLogic.LOS.Update"` | M | grep confirmed |
| `code/warrior.cpp:2127` Tracy zone `"GameLogic.AI.BrainRun"` | M | grep confirmed |
| `code/weaponbolt.cpp:382` Tracy zone `"GameLogic.Projectile.Update"` | M | grep confirmed |
| `mclib/move.cpp:4582,4753,5010` Tracy zones `GameLogic.PathManager.CalcPath{1,2,3}` | M | grep confirmed |
| `code/gvehicl.cpp:2798` `GroundVehicle::updateAIControl` | D | cited via `docs/asan-follow-ups.md:202`; the user prompt says :3155; not directly grep'd at write-time. Treat the file:line as approximate; the file `code/gvehicl.cpp` exists per grep. |
| `code/terrobj.cpp:603-609` `appearance->update()` (per slice3 recon) | D | recon doc cites :523/:603/:608, handoff cites :715. Line numbers drift between revisions; the function exists per multiple grep hits in docs. Not directly read at write-time of this brainstorm. |
| `2026-05-02-object-offload-scope.md` `~2.4 ms` for appearanceUpdate | M | confirmed in grep of doc |
| `docs/superpowers/explorations/2026-04-30-cross-track-perf-budget-audit.md` | M | file exists per ls |
| `3rdparty/include/` contents (`GL`, `SDL2`, `unistd.h`, `zconf.h`, `zlib.h`) | M | confirmed in ls |
| Absence of Taskflow/EnkiTS/TBB/std::async/std::thread/thread_pool in app code | M | grep returned 3 hits all in Tracy |
| Absence of std::mutex/std::atomic/CreateThread in app code | M | grep returned 31 files all in Tracy/SDL2/docs |
| Memory: `tgl_pool_exhaustion_is_silent.md` | M | indexed in MEMORY.md |
| Memory: `cull_gates_are_load_bearing.md` | M | indexed in MEMORY.md |
| Memory: `mc2_texture_handle_is_live.md` | M | indexed in MEMORY.md |
| Memory: `track_b_widen_static_prop_registry.md` 7-day soak | M | indexed in MEMORY.md |
| Memory: `vertex_project_loop_d1_asymptotic.md` (D1 hoist asymptotic) | M | indexed in MEMORY.md |
| Memory: `patchstream_shape_c.md` (Shape C cache 3.40→2.99 ms) | M | indexed in MEMORY.md |
| Memory: `indirect_terrain_solid_endpoint.md` | M | indexed in MEMORY.md |
| Memory: `water_ssbo_pattern.md` | M | indexed in MEMORY.md |
| Memory: `track_c_compute_cull.md` | M | indexed in MEMORY.md |
| `quadlist_is_camera_windowed.md` | M | indexed in MEMORY.md |
| Tracy file evidence in `3rdparty/tracy/` | M | confirmed in ls |

**Divergent (D) items above are flagged for the spec session to
re-verify before committing to symbol-level claims. The brainstorm
treats them as approximate.**

---

## Closing — ready-for-spec? needs-more-recon-on-X?

**Ready-for-spec on the strategic question** (NOW vs AFTER vs NEVER):
yes. The recommendation is AFTER (with the substrate-redesign caveat).
The data backing this is grep-confirmed: the GPU arc is delivering
30-80% per slice, the CPU floor candidates are mostly small per-
iteration with sharp risk surfaces, and the substrate redesign is
already queued and is the natural threading seam.

**Needs-more-recon items the spec session would inherit:**

1. **Re-grep `Mech3DAppearance::update` and `GroundVehicle::update`
   call graphs.** This brainstorm has D-status citations for the
   exact file:line of these functions. A spec that proposes
   threading them must first re-grep the actual entry point and
   walk the call graph to enumerate every shared-state touch.
2. **Verify the C3 hash-table optimization actually closed the
   addLightDataStructure zone.** The function code at txmmgr.cpp:
   1024-1027 *describes* the optimization in a comment but the
   spec session should grab a fresh Tracy capture to confirm the
   zone dropped from ~12 µs to ~200-300 ns. If it did not, C3
   moves up the priority list.
3. **Define deterministic reduction pattern for C1.** The
   `leastZ`/`mostZ`/`leastW`/`mostW`/`leastWY`/`mostWY` reduction
   needs a deterministic merge (sorted partial results, not naive
   floating-point associativity). The pattern is standard but the
   spec should specify exactly which floats are involved and the
   merge order.
4. **Audit deferred-destroy plumbing for objmgr.** Slices 4-5
   (mech, vehicle update threading) need destroys queued to end-
   of-frame to avoid mid-loop iterator invalidation. This is a
   load-bearing infrastructure change to objmgr.cpp:1936 and
   siblings; spec it before depending on it.
5. **Per-mission profile of `numberVertices` (C1's N).** ROI
   estimate for slice 1 depends on N. Wolfman zoom has many
   vertices; close-up RTS view has fewer. Spec should sample N
   across tier1's 5 missions to set realistic expectations on
   the parallel speedup.
6. **Re-verify `objBlockInfo` and `objVertexActive` are write-only
   `= true` from inside vertexProjectLoop.** This brainstorm
   asserts the writes are monotonic-true within the loop. The
   spec session should grep for any clears or resets within the
   same call graph that would break the assumption.
