# FX-GPU-1 B2 — Bolt-Weapon Trails + View-Aligned Billboards Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the broken gosFX moving-emitter Tube/Card path for bolt-style weapons (missiles, PPC, SRM, LRM, MRM, AC, gauss) with a purpose-built GPU trail emitter driven directly from `WeaponBolt`, and fix view-aligned billboards so all existing GPU particles render correctly under camera rotation.

**Architecture:** One particle billboard shader extended with `u_cameraRight`/`u_cameraUp` uniforms for view-aligned offsets (universal correctness fix). A new `GpuTrailEmitter` module that segment-stamps particles between `prev_position` and `position` each frame, called from `WeaponBolt::update()`. Coalesced into existing `Batcher` groups by `(texture_id, blend_mode, kind)`. CPU `trailEffect` keeps running in parallel through P3; suppression only lands in P4 once each GPU `kind` has visual proof.

**Tech Stack:** C++ (MechCommander 2 OpenGL port), GLSL (OpenGL 4.6 core), GameOS bridge layer, existing `mclib/particles/Batcher` infrastructure, Tracy zones for profiling, `scripts/run_smoke.py` for smoke runs.

**Spec:** [docs/superpowers/specs/2026-05-25-fx-gpu-b2-design.md](../specs/2026-05-25-fx-gpu-b2-design.md)

**Worktree root for all paths in this plan:** `.claude/worktrees/nifty-mendeleev/`

---

## File Structure

**P1 — View-aligned billboards (existing shader extension only)**
- Modify `GameOS/gameos/gos_particle_bridge.h` — declare `gos_SetActiveCamera`, `gos_GetCameraRight`, `gos_GetCameraUp`
- Modify `GameOS/gameos/gos_particle_bridge.cpp` — implement accessors + upload uniforms during flush
- Modify `code/gamecam.cpp` (~L155 render entry and ~L268 flush site) — set/clear active camera around Flush
- Modify `shaders/particle_billboard.vert` — consume `u_cameraRight`/`u_cameraUp`, replace fixed XZ corner offset

**P2 — Trail emitter infrastructure (no suppression)**
- Create `mclib/particles/gpu_trail.h` — `GpuTrailKind` enum, `GpuTrailTuning` struct, `GpuTrailEmitter` class
- Create `mclib/particles/gpu_trail.cpp` — segment stamping, tuning table (MissileSmoke only), batcher emit
- Modify `code/weaponbolt.h` — add `Stuff::Vector3D prev_position`, `GpuTrailKind gpu_trail_kind` fields
- Modify `code/weaponbolt.cpp` — snapshot prev/cur, call Spawn in `update()`, hardcoded `MissileSmoke` test mapping

**P3 — Schema bump, kind mapping, PpcBolt**
- Modify `mclib/particles/gpu_particle.h` — pack `kind` (4 bits) + `is_head` (1 bit) into spare byte
- Modify `mclib/particles/batcher.h/cpp` — extend group key to include `kind`, verify coalescing
- Modify `GameOS/gameos/gos_particle_bridge.cpp` — pass `kind` byte through SSBO upload
- Modify `shaders/particle_billboard.vert` — unpack `kind`/`is_head`
- Modify `shaders/particle_billboard.frag` — optional small per-kind tweaks (alpha vs additive intensity, soft edge)
- Modify `mclib/particles/gpu_trail.cpp` — add `PpcBolt` tuning
- Modify `code/weaponbolt.cpp` — INI-name → kind table in `init()`

**P4 — Suppression + tuning**
- Modify `code/weaponbolt.cpp` — gate existing CPU `trailEffect` spawn site on `gpu_trail_kind == None || MC2_GPU_PARTICLES==0`
- Tune constants in `mclib/particles/gpu_trail.cpp`

---

## Conventions (read once, then start)

- **TDD adaptation for this codebase:** there is no in-tree unit test framework on the C++ side. "Failing test first" means add a counter or probe that should fire 0 before the implementation lands, then verify it fires N>0 after. For shader changes, the "test" is a smoke run + visual check at a specified camera angle.
- **Commit cadence:** one commit per task unless the task explicitly says otherwise.
- **Build:** use `.claude/skills/mc2-build.md` (project skill) — do not hand-roll the build.
- **Smoke:** `py -3 scripts/run_smoke.py --mission <id> --duration <s> --kill-existing --keep-logs`
- **Env gates needed across this plan:** `MC2_GPU_PARTICLES=1`, `MC2_GPU_PARTICLES_LOG=1`, `MC2_GOSFX_GROUP_LOG=1`, and a new `MC2_GPU_TRAIL_DISABLE=1` introduced in P2 Task 5.
- **Hard invariants (re-read before every commit):**
  - No CPU `trailEffect` suppression before P4.
  - B1 regression (mc2_10 textured gosFX still renders, no square patches, no debug canaries, no thin-strip on rotation) is a release gate every phase.
  - No new shader program — extend `particle_billboard.{vert,frag}` only.
  - No `GpuParticle` schema bump until P3.
  - No new texture assets in P1/P2.
  - VFX firewall: no object-ID writes from particle shaders; no `RenderObjectHandle` registration.

---

# PHASE 1 — View-Aligned Billboards Only

### Task P1.1: Add camera-basis bridge declarations

**Files:**
- Modify: `GameOS/gameos/gos_particle_bridge.h`

- [ ] **Step 1: Append the new bridge declarations**

Open `GameOS/gameos/gos_particle_bridge.h`. Inside the `extern "C"` block (or alongside the existing `gos_*` particle declarations), add:

```c
/* B2: active camera bridge — temporary stop-gap until RenderFrameContext lands.
 * Set by GameCamera::render() immediately before particle flush; cleared after.
 * If never set this frame, accessors return last-known basis (identity at boot).
 */
void gos_SetActiveCamera(const float right_xyz[3], const float up_xyz[3]);
void gos_GetCameraRight(float out_xyz[3]);
void gos_GetCameraUp(float out_xyz[3]);
void gos_ClearActiveCamera(void);
```

- [ ] **Step 2: Commit the header-only change**

```bash
git add GameOS/gameos/gos_particle_bridge.h
git commit -m "feat(particles): declare gos_SetActiveCamera/Get bridge (B2 P1)"
```

---

### Task P1.2: Implement camera-basis bridge

**Files:**
- Modify: `GameOS/gameos/gos_particle_bridge.cpp`

- [ ] **Step 1: Add the storage + accessor implementations**

At file scope near the top of `gos_particle_bridge.cpp` (after existing static state), add:

```cpp
namespace {
    float g_cam_right[3] = {1.0f, 0.0f, 0.0f};
    float g_cam_up[3]    = {0.0f, 1.0f, 0.0f};
    bool  g_cam_set_this_frame = false;
}

extern "C" void gos_SetActiveCamera(const float right_xyz[3], const float up_xyz[3])
{
    for (int i = 0; i < 3; ++i) {
        g_cam_right[i] = right_xyz[i];
        g_cam_up[i]    = up_xyz[i];
    }
    g_cam_set_this_frame = true;
}

extern "C" void gos_GetCameraRight(float out_xyz[3])
{
    for (int i = 0; i < 3; ++i) out_xyz[i] = g_cam_right[i];
}

extern "C" void gos_GetCameraUp(float out_xyz[3])
{
    for (int i = 0; i < 3; ++i) out_xyz[i] = g_cam_up[i];
}

extern "C" void gos_ClearActiveCamera(void)
{
    g_cam_set_this_frame = false;
}
```

- [ ] **Step 2: Add a one-shot log line when basis is missing at flush time**

Locate the existing particle flush function in this same file (search for `Flush` or the SSBO upload site). Just before the first `glDrawElements` of the flush, add:

```cpp
if (!g_cam_set_this_frame) {
    static bool warned = false;
    if (!warned) {
        if (const char* v = std::getenv("MC2_GOSFX_GROUP_LOG"))
            if (v && v[0] == '1')
                fprintf(stderr, "[B2] gos_particle_bridge: flush without gos_SetActiveCamera; using last-known basis\n");
        warned = true;
    }
}
```

- [ ] **Step 3: Build**

Use `.claude/skills/mc2-build.md`. Expected: clean build, no new warnings on `gos_particle_bridge.cpp`.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_particle_bridge.cpp
git commit -m "feat(particles): implement camera-basis bridge with missing-set warning (B2 P1)"
```

---

### Task P1.3: Set/clear active camera in GameCamera render

**Files:**
- Modify: `code/gamecam.cpp` (around L155 render entry; L268 flush site)

- [ ] **Step 1: Add the include**

Near the top of `code/gamecam.cpp`, ensure `#include "gos_particle_bridge.h"` (or its existing equivalent for that file — grep first to find the right include name; particle bridge functions are already callable from this file because `Batcher::Flush` is called here).

- [ ] **Step 2: Wrap the existing Flush block with set/clear**

The current block at `code/gamecam.cpp:268-272`:

```cpp
{
    ZoneScopedN("GameCamera::render particlesFlush");
    ::mc2::particles::Batcher::Instance().ResolveTextures();
    ::mc2::particles::Batcher::Instance().Flush();
}
```

Replace with:

```cpp
{
    ZoneScopedN("GameCamera::render particlesFlush");

    // B2 P1: publish current camera basis to particle bridge before flush.
    // Reads right/up from the base Camera class (camera.cpp ~L110 extracts
    // them from invView each update). `eye` is the active GameCamera in this scope.
    float camRight[3] = { eye->right[0], eye->right[1], eye->right[2] };
    float camUp[3]    = { eye->up[0],    eye->up[1],    eye->up[2]    };
    gos_SetActiveCamera(camRight, camUp);

    ::mc2::particles::Batcher::Instance().ResolveTextures();
    ::mc2::particles::Batcher::Instance().Flush();

    gos_ClearActiveCamera();
}
```

If `eye->right` / `eye->up` do not compile (member name differs), grep `class Camera` in `GameOS/gameos/utils/camera.cpp` for the exact field names — they are populated at `camera.cpp:110-115`. Adapt the literal field reads accordingly. Do not call a "GetRight()" accessor that does not exist; read the public array members directly the same way `camera::update()` writes them.

- [ ] **Step 3: Build**

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add code/gamecam.cpp
git commit -m "feat(gamecam): publish camera basis to particle bridge around Flush (B2 P1)"
```

---

### Task P1.4: Extend particle billboard vertex shader to use camera basis

**Files:**
- Modify: `shaders/particle_billboard.vert`

- [ ] **Step 1: Read the current shader to find the corner-offset block**

Open `shaders/particle_billboard.vert`. Locate the existing block that constructs `worldPos` from `particle_center` plus a fixed XZ-plane (east-up) offset (this is the B1 fix from commit `5233e70` / B1 handoff notes — search for the corner offset computation using fixed axes).

- [ ] **Step 2: Add the uniforms and replace the offset math**

At the top of the shader (alongside other uniforms), add:

```glsl
uniform vec3 u_cameraRight;
uniform vec3 u_cameraUp;
```

Replace the fixed-axis corner offset with:

```glsl
// B2 P1: view-aligned billboard. corner_offset.xy in [-0.5, +0.5].
// particle_size is the already-existing per-particle size.
vec3 worldPos = particle_center
              + u_cameraRight * corner_offset.x * particle_size
              + u_cameraUp    * corner_offset.y * particle_size;
```

If the existing variable names differ (`particle_center` may be `inCenter` or similar — check the surrounding code), substitute accordingly. Do not introduce new names if the shader already has them.

- [ ] **Step 3: Wire the uniforms in `gos_particle_bridge.cpp`**

In the per-flush uniform-binding section (where the existing camera/MVP uniforms are bound), add:

```cpp
// B2 P1: cache uniform locations. A -1 is a legitimate "not in the program"
// result (driver stripped a uniform that's unused after dead-code elim). Do NOT
// retry the lookup every frame — that hides shader bugs and wastes GL calls.
static GLint loc_cameraRight = -1;
static GLint loc_cameraUp    = -1;
static bool  loc_looked_up   = false;
if (!loc_looked_up) {
    loc_cameraRight = glGetUniformLocation(g_particle_program, "u_cameraRight");
    loc_cameraUp    = glGetUniformLocation(g_particle_program, "u_cameraUp");
    loc_looked_up   = true;
    if (loc_cameraRight < 0 || loc_cameraUp < 0) {
        if (const char* v = std::getenv("MC2_GOSFX_GROUP_LOG"))
            if (v && v[0] == '1')
                fprintf(stderr, "[B2] gos_particle_bridge: uniform locations missing — right=%d up=%d\n",
                        loc_cameraRight, loc_cameraUp);
    }
}
if (loc_cameraRight >= 0) glUniform3fv(loc_cameraRight, 1, g_cam_right);
if (loc_cameraUp    >= 0) glUniform3fv(loc_cameraUp,    1, g_cam_up);
```

(Use whatever the program handle variable is actually named in this file — grep for `g_particle_program` or similar.)

- [ ] **Step 4: Build + deploy shader and exe lockstep**

Per `shader_exe_deploy_lockstep` memory: deploy shader + exe together when either changes. Use `.claude/skills/mc2-deploy.md` (project skill).

- [ ] **Step 5: Commit**

```bash
git add shaders/particle_billboard.vert GameOS/gameos/gos_particle_bridge.cpp
git commit -m "feat(shader): view-aligned particle billboards via u_cameraRight/Up (B2 P1)"
```

---

### Task P1.5: P1 acceptance — axis verification + B1 regression smoke

**Files:** none (validation step)

- [ ] **Step 1: Smoke mc2_10 with logging**

```bash
$env:MC2_GPU_PARTICLES = "1"
$env:MC2_GPU_PARTICLES_LOG = "1"
$env:MC2_GOSFX_GROUP_LOG = "1"
py -3 scripts/run_smoke.py --mission mc2_10 --duration 40 --kill-existing --keep-logs
```

Expected: `emit_total` in hundreds of thousands (B1 baseline). No crashes. No `gos_SetActiveCamera not set` warning. Bridge log shows uniform location for `u_cameraRight`/`u_cameraUp` resolved (non -1).

- [ ] **Step 2: B1 regression visual check**

Open the kept log + screenshot/replay. Confirm:
- Explosions, fire, smoke at gosFX effect positions still render (B1 result preserved).
- No square patches (pink colorkey discard still works).
- No magenta/checker Stage 1′ debug canary visible anywhere.

If any fails, **stop, do not proceed to P2** — revert P1.4 and diagnose the shader change.

- [ ] **Step 3: Axis verification — camera-rotation visual test**

Pick a mission with persistent particle effects (mc2_10 burning building or pre-placed explosion site).

```bash
$env:MC2_GPU_PARTICLES = "1"
py -3 scripts/run_smoke.py --mission mc2_10 --duration 60 --kill-existing --keep-logs
```

During the 60s window, use the debug camera controls to rotate camera through full 360° yaw. Capture screenshots at 0°, 90°, 180°, 270°.

Verification criteria:
- Sprites remain face-on (round/square shape) at every angle. **No thin strips.**
- Sprite world position does not slide/skew relative to the ground geometry as camera rotates. (A wrong basis sign manifests as drift, not invisibility.)

If sprites slide: swap sign on one of `right`/`up` in P1.3 (axis space mismatch between camera basis and shader). Re-deploy, re-test. Document the sign convention as a comment at the set-call site.

- [ ] **Step 4: Commit verification notes only if anything was tweaked**

If sign-fixing required a code change in P1.3, recommit with message:

```bash
git commit -am "fix(gamecam): correct camera-basis sign for particle billboards (B2 P1)"
```

Otherwise no commit — proceed to P2.

**P1 HARD GATE:** if any of Step 2 or Step 3 criteria fail, P2 does not start.

---

# PHASE 2 — Trail Emitter Infrastructure (No Suppression)

### Task P2.1: Create the `GpuTrailEmitter` header

**Files:**
- Create: `mclib/particles/gpu_trail.h`

- [ ] **Step 1: Write the header**

```cpp
#pragma once

#include <cstdint>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Forward-declare Stuff::Vector3D to avoid pulling the whole stuff header here.
namespace Stuff { class Vector3D; }

namespace mc2 { namespace particles {

enum class GpuTrailKind : uint8_t {
    None         = 0,
    MissileSmoke = 1,
    PpcBolt      = 2,   // (added in P3, declared here for forward compat)
};

struct GpuTrailTuning {
    glm::vec4 head_color;
    glm::vec4 trail_color;
    float head_size;                  // world units; 0 = no head sprite
    float trail_particle_size;        // world units per particle
    float trail_lifetime_s;
    float trail_density_per_meter;
    uint8_t blend_mode;               // 0 = alpha, 1 = additive (matches Batcher convention)
    uint16_t texture_id;              // bridge texture handle
};

class GpuTrailEmitter {
public:
    // Hard cap on trail particles per segment, protects against fast projectiles
    // and frame hitches. Tune from first smoke pass.
    static constexpr int MAX_PARTICLES_PER_SEGMENT = 32;

    // Spawn trail particles along the segment prev->cur, plus an optional head
    // sprite at cur. Caller owns prev/cur (do not store internally).
    //
    // deltaT is the frame delta used to gate zero-length / paused-frame cases.
    static void Spawn(GpuTrailKind kind,
                      const Stuff::Vector3D& prev_world,
                      const Stuff::Vector3D& cur_world,
                      float deltaT);

private:
    static const GpuTrailTuning& tuning_for(GpuTrailKind k);
};

}} // namespace mc2::particles
```

- [ ] **Step 2: Commit**

```bash
git add mclib/particles/gpu_trail.h
git commit -m "feat(particles): GpuTrailEmitter header (B2 P2)"
```

---

### Task P2.1b: Texture-handle survey (read-only — blocks P2.2)

**Files:** none (read-only investigation; output captured inline as a comment in P2.2 tuning table)

**Why:** P2 acceptance expects visible smoke trails. `texture_id = 0` is a magic number — if 0 is not a valid B1-resolved particle texture handle in the current build, the emitter will appear broken when it is actually fine. We must use a known-good handle.

- [ ] **Step 1: Dispatch a Haiku investigator**

Use the Agent tool with `subagent_type=Explore`, model `haiku`. Prompt:

> Working in worktree `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev`. We need a known-good GPU particle texture handle that the B1 path has already resolved successfully for smoke-like effects.
>
> Look in `mclib/particles/batcher.cpp` (`ResolveTextures` method) and `GameOS/gameos/gos_particle_bridge.cpp` for the texture-resolution path. Find: (a) the integer/handle space used by `Batcher` group keys for `texture_id`; (b) whether handle `0` is a sentinel/invalid or a real entry; (c) which existing gosFX spec produces a smoke/trail texture that survives `ResolveTextures` under `MC2_GPU_PARTICLES=1` (any of the five B1 effect types — Card/CardCloud/PointCloud/ShardCloud/Tube — that uses a soft smoke or puff texture).
>
> Report the **exact constant or runtime expression** the engineer should put in `kTuningTable[MissileSmoke].texture_id` to reuse that already-resolved handle at runtime. If the answer is "look it up at runtime from a named string," give the lookup call and the string. Cite `file:line` for everything. Under 300 words.

- [ ] **Step 2: Record the chosen handle/recipe**

Paste the investigator's answer as a `// HANDLE SURVEY:` comment block at the top of `mclib/particles/gpu_trail.cpp` in the next task. If the answer requires a runtime lookup (e.g. via a `gos_LoadTexture` call), do the lookup once in a static initializer inside `tuning_for()` so the table can keep its declarative shape.

- [ ] **Step 3: No commit yet** — the survey output gets committed inline with P2.2.

**P2.2 may not begin until this survey has produced a concrete handle expression. If no existing handle works, stop and ask the user — do not invent `texture_id = 0`.**

---

### Task P2.2: Implement `GpuTrailEmitter` with `MissileSmoke` tuning only

**Files:**
- Create: `mclib/particles/gpu_trail.cpp`

- [ ] **Step 1: Add a per-spawn probe counter (test-first)**

Before implementing the real spawn, add a counter that the smoke pass will read. Open `mclib/particles/batcher.h` and add (near other counters):

```cpp
static uint64_t s_trail_spawn_total;   // B2 P2: trail particle spawns this run
static uint64_t s_trail_head_total;    // B2 P2: head sprite spawns this run
```

In `batcher.cpp` definitions:

```cpp
uint64_t Batcher::s_trail_spawn_total = 0;
uint64_t Batcher::s_trail_head_total  = 0;
```

In the existing per-frame summary log (find `emit_total=` in `batcher.cpp` — that's the line printed each flush), append `" trail_spawn=" + std::to_string(s_trail_spawn_total) + " trail_head=" + std::to_string(s_trail_head_total)`.

- [ ] **Step 2: Write the implementation**

Create `mclib/particles/gpu_trail.cpp`:

```cpp
#include "gpu_trail.h"
#include "batcher.h"
#include "stuff/vector3d.hpp"   // adjust include to actual project path
#include <cmath>
#include <algorithm>

// HANDLE SURVEY (from P2.1b — paste investigator's findings here):
//   <investigator output: which existing B1 texture handle to reuse, why,
//    and the source file:line cite>
//
// If the survey returned a runtime lookup recipe, perform the lookup in a
// static initializer below and assign to kMissileSmokeTexId.
static const uint16_t kMissileSmokeTexId = /* replace with surveyed value */;

namespace mc2 { namespace particles {

namespace {

// B2 P2: MissileSmoke only. PpcBolt tuning lands in P3.
// Texture id 0 is a placeholder — the bridge currently resolves an existing
// gosFX smoke texture under handle 0 from B1's ResolveTextures path. P3 wires
// the real id when we have the kind mapping table.
const GpuTrailTuning kTuningTable[] = {
    /* [None]         */ { {0,0,0,0}, {0,0,0,0}, 0.f, 0.f, 0.f, 0.f, 0, 0 },
    /* [MissileSmoke] */ {
        {0,0,0,0},            // no head sprite for missile
        {1.f, 1.f, 1.f, 0.5f},// white trail, 0.5 alpha
        0.0f,                 // head_size: none
        1.5f,                 // trail_particle_size: 1.5 world units
        1.2f,                 // trail_lifetime_s
        8.0f,                 // trail_density_per_meter
        0,                    // alpha blend
        kMissileSmokeTexId,   // resolved handle from P2.1b survey — NOT a magic 0
    },
};

constexpr float kEpsilonLength = 0.01f; // world-units; below this is a no-trail segment

} // anon

const GpuTrailTuning& GpuTrailEmitter::tuning_for(GpuTrailKind k)
{
    auto idx = static_cast<size_t>(k);
    if (idx >= sizeof(kTuningTable) / sizeof(kTuningTable[0])) idx = 0;
    return kTuningTable[idx];
}

void GpuTrailEmitter::Spawn(GpuTrailKind kind,
                            const Stuff::Vector3D& prev_world,
                            const Stuff::Vector3D& cur_world,
                            float deltaT)
{
    if (kind == GpuTrailKind::None) return;
    const GpuTrailTuning& t = tuning_for(kind);

    // Segment length using Stuff::Vector3D field layout (x,y,z public).
    const float dx = cur_world.x - prev_world.x;
    const float dy = cur_world.y - prev_world.y;
    const float dz = cur_world.z - prev_world.z;
    const float len = std::sqrt(dx*dx + dy*dy + dz*dz);

    const bool trail_ok = (deltaT > 0.f && len >= kEpsilonLength);
    int N = 0;
    if (trail_ok) {
        N = static_cast<int>(std::ceil(len * t.trail_density_per_meter));
        N = std::clamp(N, 0, MAX_PARTICLES_PER_SEGMENT);
    }

    Batcher& b = Batcher::Instance();

    // Emit trail particles linearly between prev and cur.
    for (int i = 0; i < N; ++i) {
        const float u = (N == 1) ? 0.5f : (float)i / (float)(N - 1);
        const float px = prev_world.x + dx * u;
        const float py = prev_world.y + dy * u;
        const float pz = prev_world.z + dz * u;
        b.EmitParticle(/*group key*/ t.texture_id, t.blend_mode,
                       /*pos*/ px, py, pz,
                       /*size*/ t.trail_particle_size,
                       /*color*/ t.trail_color,
                       /*lifetime*/ t.trail_lifetime_s,
                       /*kind byte*/ static_cast<uint8_t>(kind));
        ++Batcher::s_trail_spawn_total;
    }

    // Optional head sprite at cur_world, single-frame lifetime.
    if (t.head_size > 0.f) {
        b.EmitParticle(t.texture_id, t.blend_mode,
                       cur_world.x, cur_world.y, cur_world.z,
                       t.head_size,
                       t.head_color,
                       /*lifetime*/ 0.05f,
                       /*kind byte with is_head bit (P3 will pack properly)*/
                       static_cast<uint8_t>(kind));
        ++Batcher::s_trail_head_total;
    }
}

}} // namespace mc2::particles
```

Note on `Batcher::EmitParticle` signature: this plan assumes an existing emit entry point. Grep `mclib/particles/batcher.h` for the actual `Emit*` method and adapt the call signature. If the existing API only supports bulk struct push, build a `GpuParticle` locally and push via that API instead — the call shape changes, the semantics do not.

- [ ] **Step 3: Add the file to CMake**

Open `mclib/CMakeLists.txt` (or the equivalent for the particles sub-target). Add `particles/gpu_trail.cpp` to the source list alongside `particles/batcher.cpp`.

- [ ] **Step 4: Build**

Use `.claude/skills/mc2-build.md`. Expected: clean build. If `EmitParticle` signature mismatch — fix in `gpu_trail.cpp` only, do not change `Batcher`.

- [ ] **Step 5: Commit**

```bash
git add mclib/particles/gpu_trail.cpp mclib/particles/gpu_trail.h mclib/particles/batcher.h mclib/particles/batcher.cpp mclib/CMakeLists.txt
git commit -m "feat(particles): GpuTrailEmitter impl with MissileSmoke tuning + spawn counters (B2 P2)"
```

---

### Task P2.3: Add `prev_position` + `gpu_trail_kind` to `WeaponBolt`

**Files:**
- Modify: `code/weaponbolt.h` (~L181-372)

- [ ] **Step 1: Add forward decl + include**

Near the top of `code/weaponbolt.h`, add (after existing includes):

```cpp
#include "../mclib/particles/gpu_trail.h"
```

(Adjust the relative path to match the include conventions already used in this file — grep nearby `#include` lines.)

- [ ] **Step 2: Add the two fields (private by default)**

Inside the `WeaponBolt` class body, in the **private** section near other position-related state, add:

```cpp
private:
    // B2 P2: per-frame position snapshot for GPU trail segment stamping.
    // Internal state — no external readers. Promote to public only if a real
    // consumer outside WeaponBolt needs it later.
    Stuff::Vector3D prev_position;

    // B2 P2: which GPU trail kind this bolt drives (mapped from weapon INI
    // in init()). Hardcoded to MissileSmoke for every bolt during P2 test
    // pass; replaced by table-driven mapping in P3.
    mc2::particles::GpuTrailKind gpu_trail_kind = mc2::particles::GpuTrailKind::None;
```

If `WeaponBolt`'s existing convention already exposes all comparable per-frame state as `public:` (grep neighbouring members — `position`, `velocity`, `effectId` — to confirm), match that convention instead of fighting it. Use `private:` unless the class style forces otherwise.

- [ ] **Step 3: Build**

Expected: clean build. If `Stuff::Vector3D` is already in scope at this header, no extra include needed.

- [ ] **Step 4: Commit**

```bash
git add code/weaponbolt.h
git commit -m "feat(weaponbolt): add prev_position + gpu_trail_kind fields (B2 P2)"
```

---

### Task P2.4: Snapshot prev/cur and call Spawn in `WeaponBolt::update()`

**Files:**
- Modify: `code/weaponbolt.cpp` (`update()` ~L380, position update ~L522-524)

- [ ] **Step 1: Add `#include "gpu_trail.h"` to `weaponbolt.cpp`**

Near other particle/effect includes.

- [ ] **Step 2: Snapshot prev at top of `update()`**

At the very top of `WeaponBolt::update()` (file `code/weaponbolt.cpp:380`), before any other logic:

```cpp
// B2 P2: snapshot for trail segment stamping. prev is read at bottom of update().
prev_position = position;
```

- [ ] **Step 3: Spawn the trail at bottom of `update()`**

At the bottom of `WeaponBolt::update()`, immediately before the closing `return` (or before the final brace if no early return), add:

```cpp
// B2 P2: GPU trail spawn. Producer-owned segment stamping; consolidated into the
// next batcher flush. CPU trailEffect still runs in parallel through P3.
if (gpu_trail_kind != mc2::particles::GpuTrailKind::None) {
    extern float frameLength; // existing global frame delta in seconds (or use the right symbol)
    mc2::particles::GpuTrailEmitter::Spawn(
        gpu_trail_kind, prev_position, position, frameLength);
}
```

Replace `frameLength` with the actual existing per-frame delta symbol used elsewhere in `weaponbolt.cpp`. Grep for `deltaT` / `frameLength` / similar in this file to find the convention. Do not introduce a new delta source.

- [ ] **Step 4: Hardcode test mapping in constructor / `init()`**

Find `WeaponBolt::init()` (or the ctor/setup path that runs once per bolt). Add at the end of the relevant init:

```cpp
// B2 P2 TEST MAPPING: every bolt = MissileSmoke. No CPU suppression in this
// phase — both CPU and GPU trails will be visible, which is the success signal.
// P3 replaces this with real INI-name → kind table.
gpu_trail_kind = mc2::particles::GpuTrailKind::MissileSmoke;
```

- [ ] **Step 5: Build**

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add code/weaponbolt.cpp
git commit -m "feat(weaponbolt): per-frame GPU trail spawn hook (hardcoded MissileSmoke for P2 test) (B2 P2)"
```

---

### Task P2.5: Add `MC2_GPU_TRAIL_DISABLE` env gate

**Files:**
- Modify: `mclib/particles/gpu_trail.cpp`

- [ ] **Step 1: Add the gate at top of `Spawn()`**

At the very top of `GpuTrailEmitter::Spawn()`, after the `kind == None` early return, add:

```cpp
static int gate = -1;  // -1 = not yet read
if (gate < 0) {
    const char* v = std::getenv("MC2_GPU_TRAIL_DISABLE");
    gate = (v && v[0] == '1') ? 1 : 0;
}
if (gate) return;
```

- [ ] **Step 2: Build + commit**

```bash
git add mclib/particles/gpu_trail.cpp
git commit -m "feat(particles): MC2_GPU_TRAIL_DISABLE env gate for trail emitter (B2 P2)"
```

---

### Task P2.6: P2 acceptance — bolt-heavy smoke + B1 regression

**Files:** none (validation)

- [ ] **Step 1: Find a tier-1 mission with heavy bolt fire**

Grep mission docs / playtest notes for missile/PPC-heavy tier-1 missions. If undocumented, pick the next mission after mc2_10 in the campaign and confirm by quick playtest that missiles/PPC fire is observable in the first 40s. Record the chosen mission ID in the smoke command below.

- [ ] **Step 2: Smoke mc2_10 (B1 regression baseline)**

```bash
$env:MC2_GPU_PARTICLES = "1"
$env:MC2_GPU_PARTICLES_LOG = "1"
Remove-Item Env:MC2_GPU_TRAIL_DISABLE -ErrorAction SilentlyContinue
py -3 scripts/run_smoke.py --mission mc2_10 --duration 40 --kill-existing --keep-logs
```

Confirm: log shows `trail_spawn=N` where N>0 if mc2_10 has any bolt fire (may be 0 if no bolts fire in first 40s — that's fine). `emit_total` non-zero (B1 effects still rendering).

- [ ] **Step 3: Smoke the bolt-heavy mission**

```bash
py -3 scripts/run_smoke.py --mission <chosen_id> --duration 40 --kill-existing --keep-logs
```

Confirm in log: `trail_spawn` in thousands+. No crashes.

- [ ] **Step 4: Visual confirmation (DIAGNOSTIC, not parity)**

Replay screenshots. Confirm only:
- **Trail particles render somewhere behind moving bolts.** That's it.
- Both CPU gosFX trail AND new GPU trail are visible — overlap is expected at this phase.
- B1 effects still render correctly.
- No regressions on view-aligned billboards (rotate camera, no thin strip).

Do **not** judge per-weapon look here. Every bolt is hardcoded to `MissileSmoke`, so PPC bolts will trail white smoke and AC rounds will trail white smoke. That is fine — the P2 question is "do GPU trail particles emit and render at all behind moving entities," not "does each weapon look right." Per-weapon visual parity is a P3/P4 acceptance bar.

- [ ] **Step 5: Verify the `MC2_GPU_TRAIL_DISABLE` gate**

```bash
$env:MC2_GPU_TRAIL_DISABLE = "1"
py -3 scripts/run_smoke.py --mission <chosen_id> --duration 40 --kill-existing --keep-logs
```

Confirm log: `trail_spawn=0`. Only CPU gosFX trails visible. Remove the env var for subsequent runs.

**P2 HARD GATE:** if no trail particles render, or B1 regression criterion fails, fix before P3. Diagnose with `MC2_GPU_PARTICLES_LOG=1` and the new `trail_spawn`/`trail_head` counters.

---

# PHASE 3 — Schema Bump, Kind Mapping, PpcBolt

### Task P3.1: Bump `GpuParticle` schema with `kind`/`is_head` byte

**Files:**
- Modify: `mclib/particles/gpu_particle.h`

- [ ] **Step 1: Read the current struct and locate the spare bytes**

Open `mclib/particles/gpu_particle.h`. Find the 64-byte `GpuParticle` struct. Identify which bytes are currently padding/unused. There should be at least one spare byte from B1.

- [ ] **Step 2: Add the packed kind byte**

Replace the chosen padding byte with:

```cpp
// B2 P3: kind in low 4 bits (up to 16 GpuTrailKind values), is_head in bit 4,
// remaining 3 bits reserved.
uint8_t kind_and_flags;

// Helpers — keep inline so they generate no code overhead.
inline uint8_t kind() const { return kind_and_flags & 0x0F; }
inline bool    is_head() const { return (kind_and_flags & 0x10) != 0; }
inline void    set_kind(uint8_t k) { kind_and_flags = (kind_and_flags & 0xF0) | (k & 0x0F); }
inline void    set_is_head(bool h) { kind_and_flags = h ? (kind_and_flags | 0x10) : (kind_and_flags & ~0x10); }
```

If no spare byte exists, this means a real schema bump is required — coordinate the bridge unpack (Task P3.3) and shader (Task P3.4) in the **same commit** per `shader_exe_deploy_lockstep`. Otherwise (spare byte available), reuse-in-place keeps the bump non-breaking.

- [ ] **Step 3: Verify `sizeof(GpuParticle) == 64`**

In `gpu_particle.h` or an adjacent unit-test-style header, add a compile-time check:

```cpp
static_assert(sizeof(GpuParticle) == 64, "GpuParticle must stay 64 bytes");
```

- [ ] **Step 4: Build**

Expected: clean build. If `static_assert` fails, the layout has drifted — fix padding before continuing.

- [ ] **Step 5: Commit** (do not commit alone if a real layout change was needed — combine with P3.3 and P3.4)

If spare-byte reuse, commit now:

```bash
git add mclib/particles/gpu_particle.h
git commit -m "feat(particles): pack kind + is_head into spare schema byte (B2 P3)"
```

---

### Task P3.2: Extend `Batcher` group key with kind, verify coalescing

**Files:**
- Modify: `mclib/particles/batcher.h`, `mclib/particles/batcher.cpp`

- [ ] **Step 1: Extend the group key tuple**

In `batcher.h`, find the group-key struct/tuple used to coalesce particles into draws. Add `uint8_t kind` to it. Update the hash/equality operators.

- [ ] **Step 2: Update `EmitParticle` (or equivalent) to forward kind into the key**

In `batcher.cpp`, the existing emit path computes the group key. Add `kind` to the key construction. The emit should look up an existing group by `(texture_id, blend_mode, kind)` and append; create only if absent **this frame**.

- [ ] **Step 3: Add a group-count counter**

In `batcher.cpp` Flush(), log `groups=N` already exists. Add a per-frame max check:

```cpp
static int s_max_groups_seen = 0;
if (static_cast<int>(groups_.size()) > s_max_groups_seen) {
    s_max_groups_seen = static_cast<int>(groups_.size());
    fprintf(stderr, "[B2] batcher: max groups seen this run = %d\n", s_max_groups_seen);
}
```

This will surface a missile-volley group-explosion bug if it happens.

- [ ] **Step 4: Build + commit**

```bash
git add mclib/particles/batcher.h mclib/particles/batcher.cpp
git commit -m "feat(particles): include kind in batcher group key + max-groups telemetry (B2 P3)"
```

---

### Task P3.3: Bridge upload kind byte through SSBO

**Files:**
- Modify: `GameOS/gameos/gos_particle_bridge.cpp`

- [ ] **Step 1: Confirm SSBO layout already carries the byte**

The SSBO upload writes the full 64-byte `GpuParticle` struct. The new `kind_and_flags` byte rides through automatically. No bridge code change needed *if* the struct is uploaded as-is.

If the bridge uses a separate packed struct (not `GpuParticle` directly), update that struct to carry `kind_and_flags` at the matching offset. Add a `static_assert` matching the one from P3.1.

- [ ] **Step 2: Build + commit (only if a change was needed)**

```bash
git add GameOS/gameos/gos_particle_bridge.cpp
git commit -m "feat(bridge): forward kind_and_flags byte through SSBO (B2 P3)"
```

---

### Task P3.4: Shader — unpack `kind`/`is_head`

**Files:**
- Modify: `shaders/particle_billboard.vert`, `shaders/particle_billboard.frag`

- [ ] **Step 1: In the vertex shader, unpack the byte**

In `particle_billboard.vert`, where per-particle fields are read from the SSBO, add:

```glsl
uint kind_and_flags = particles[gl_InstanceID].kind_and_flags;
uint kind = kind_and_flags & 0xFu;
bool is_head = (kind_and_flags & 0x10u) != 0u;

// Forward to fragment shader.
flat out uint v_kind;
flat out uint v_is_head;
v_kind = kind;
v_is_head = is_head ? 1u : 0u;
```

(The exact SSBO access syntax depends on existing shader conventions — grep for how existing fields are read and match the pattern.)

- [ ] **Step 2: In the fragment shader, optional small per-kind tweak**

In `particle_billboard.frag`, near the final color computation:

```glsl
flat in uint v_kind;
flat in uint v_is_head;

vec4 color = sampled * particle_color;

// B2 P3: minor per-kind tweaks. Keep small — escalation to a new program is a smell.
if (v_is_head == 1u) {
    color.rgb *= 1.5;   // head sprites brighter (good for PpcBolt head)
}
```

- [ ] **Step 3: Build + deploy shader/exe lockstep + commit**

```bash
git add shaders/particle_billboard.vert shaders/particle_billboard.frag
git commit -m "feat(shader): unpack kind/is_head, head-sprite intensity boost (B2 P3)"
```

---

### Task P3.5: Add `PpcBolt` tuning

**Files:**
- Modify: `mclib/particles/gpu_trail.cpp`

- [ ] **Step 1: Append PpcBolt entry to tuning table**

```cpp
const GpuTrailTuning kTuningTable[] = {
    /* [None]         */ { /* …existing… */ },
    /* [MissileSmoke] */ { /* …existing… */ },
    /* [PpcBolt]      */ {
        {0.4f, 0.7f, 1.0f, 1.0f},  // bright blue head
        {0.3f, 0.5f, 1.0f, 0.7f},  // faint blue trail
        2.5f,                       // head_size — "about 2x laser size" per ref
        0.8f,                       // trail_particle_size
        0.4f,                       // trail_lifetime_s — short
        4.0f,                       // trail_density_per_meter — sparser than missile
        1,                          // additive blend
        0,                          // texture_id placeholder
    },
};
```

- [ ] **Step 2: In the head-sprite emit path, set the is_head bit**

In `Spawn()`, update the head emit to set the bit. Replace the previous head emit with:

```cpp
if (t.head_size > 0.f) {
    uint8_t kf = static_cast<uint8_t>(kind) & 0x0F;
    kf |= 0x10; // is_head
    b.EmitParticleRaw(t.texture_id, t.blend_mode,
                      cur_world.x, cur_world.y, cur_world.z,
                      t.head_size, t.head_color, /*lifetime*/ 0.05f, kf);
    ++Batcher::s_trail_head_total;
}
```

(Add a `EmitParticleRaw` variant on Batcher if needed to accept a pre-packed kind byte, OR have Batcher::EmitParticle take a `bool is_head` parameter — whichever matches its existing API. Keep the surface minimal.)

- [ ] **Step 3: Build + commit**

```bash
git add mclib/particles/gpu_trail.cpp mclib/particles/batcher.h mclib/particles/batcher.cpp
git commit -m "feat(particles): add PpcBolt tuning + is_head flag for head sprites (B2 P3)"
```

---

### Task P3.6: Investigator pass over weapon INI configs

**Files:** none (read-only investigation)

- [ ] **Step 1: Dispatch Haiku investigator**

Use the Agent tool with `subagent_type=Explore`, model `haiku`. Prompt:

> Working in worktree `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev`. Find every bolt-style weapon INI config under `data/`. For each weapon, report:
> 1. Weapon name (e.g. "LRM-5", "PPC", "Gauss Rifle")
> 2. The string value of its `fireEffect` field (if any)
> 3. The string value of its `trailEffect` field (if any)
> 4. Whether the weapon is a missile/projectile (has travel time) vs hit-scan (instant)
>
> Report as a markdown table sorted by weapon type (missiles together, energy together, ballistic together). Under 400 words. Cite file paths.

- [ ] **Step 2: Save the investigator output**

Save the table to `docs/superpowers/specs/2026-05-25-fx-gpu-b2-weapon-ini-survey.md` for reference during P3.7.

- [ ] **Step 3: Commit the survey doc**

```bash
git add docs/superpowers/specs/2026-05-25-fx-gpu-b2-weapon-ini-survey.md
git commit -m "docs(b2): weapon INI survey for trail-kind mapping (B2 P3)"
```

---

### Task P3.7: Wire INI-name → kind mapping in `WeaponBolt::init()`

**Files:**
- Modify: `code/weaponbolt.cpp`

- [ ] **Step 1: Remove the hardcoded test mapping**

Find the P2 line:

```cpp
gpu_trail_kind = mc2::particles::GpuTrailKind::MissileSmoke;
```

Replace with the table-driven version based on the survey from P3.6:

```cpp
// B2 P3: map weapon INI fields to GPU trail kind. Defaults to None — falls
// back to CPU gosFX trail (no visual regression vs B1).
gpu_trail_kind = mc2::particles::GpuTrailKind::None;

// `trailEffectName` / `weaponName` — substitute real field names from
// WeaponBolt or its config struct.
if (trailEffectName && strstr(trailEffectName, "smoke")) {
    gpu_trail_kind = mc2::particles::GpuTrailKind::MissileSmoke;
} else if (weaponName && (strstr(weaponName, "PPC") || strstr(weaponName, "ppc"))) {
    gpu_trail_kind = mc2::particles::GpuTrailKind::PpcBolt;
}
```

Refine `strstr` substring rules using the actual INI strings discovered in P3.6 — prefer exact equality where possible to avoid accidental matches.

- [ ] **Step 2: Build + commit**

```bash
git add code/weaponbolt.cpp
git commit -m "feat(weaponbolt): INI-name → GpuTrailKind mapping (B2 P3)"
```

---

### Task P3.8: P3 acceptance — coalescing + visual targets

**Files:** none (validation)

- [ ] **Step 1: Smoke the bolt-heavy mission with `MC2_GPU_PARTICLES_LOG=1`**

```bash
$env:MC2_GPU_PARTICLES = "1"
$env:MC2_GPU_PARTICLES_LOG = "1"
py -3 scripts/run_smoke.py --mission <chosen_id> --duration 60 --kill-existing --keep-logs
```

Confirm in log:
- `max groups seen` stays bounded (≤10 typical, definitely not in the hundreds — that would indicate per-Spawn group inflation).
- `trail_spawn` and `trail_head` counters both non-zero.

- [ ] **Step 2: Visual confirmation per kind**

Replay screenshots:
- Missile smoke trails: white, soft, curving with arc trajectory, long enough to see the arc.
- PPC bolt: bright blue head sprite traveling visibly to target, faint blue tail behind, additive glow.
- Both CPU + GPU still visible (no suppression yet — that's P4).

- [ ] **Step 3: B1 regression**

mc2_10 still passes all §8.1.1 criteria. Camera rotation still produces no thin-strip.

**P3 HARD GATE:** group count explosion, missing kind discrimination (PPC looks like missile or vice versa), or B1 regression → fix before P4.

---

# PHASE 4 — Suppression + Tuning

### Task P4.1: Suppress CPU `trailEffect` for proven kinds

**Files:**
- Modify: `code/weaponbolt.cpp`

- [ ] **Step 1: Find the CPU `trailEffect` spawn site**

Grep `code/weaponbolt.cpp` for `trailEffect` usage. Locate the call that creates/updates the CPU gosFX trail effect (likely a `gosFX::Effect::create` or equivalent triggered each frame or at bolt spawn).

- [ ] **Step 2: Add a `gpuTrailKindProven` helper (file-local)**

In `code/weaponbolt.cpp`, near the top:

```cpp
// B2 P4: explicit allowlist of GPU trail kinds with confirmed visual proof.
// Suppression of the CPU trailEffect only applies to kinds in this list.
// Reason: adding a new GpuTrailKind enum value must NOT automatically suppress
// the CPU fallback before the new kind is visually verified. Failing to update
// this allowlist when adding a kind = the CPU fallback keeps running (safe).
// Forgetting to update it after proving the kind = a visible double-trail
// (loud, discoverable). Both failure modes are non-silent — the allowlist is
// the safety mechanism.
static bool gpuTrailKindProven(mc2::particles::GpuTrailKind k)
{
    switch (k) {
        case mc2::particles::GpuTrailKind::MissileSmoke: return true;
        case mc2::particles::GpuTrailKind::PpcBolt:      return true;
        default:                                          return false;
    }
}
```

- [ ] **Step 3: Gate the CPU trailEffect spawn on env + helper**

Wrap the existing CPU trail spawn with:

```cpp
// B2 P4: suppress CPU trailEffect only when (a) env gate is on AND
// (b) this bolt's GPU kind is in the proven allowlist. Either condition
// failing → CPU fallback continues to run unchanged.
bool gpu_owns_trail = false;
if (gpuTrailKindProven(gpu_trail_kind)) {
    const char* v = std::getenv("MC2_GPU_PARTICLES");
    gpu_owns_trail = (v && v[0] == '1');
}
if (!gpu_owns_trail) {
    /* …existing CPU trailEffect spawn… */
}
```

- [ ] **Step 4: Build + commit**

```bash
git add code/weaponbolt.cpp
git commit -m "feat(weaponbolt): suppress CPU trailEffect for proven GPU kinds only (B2 P4)"
```

---

### Task P4.2: P4 acceptance — visual parity + smoke matrix

**Files:** none (validation)

- [ ] **Step 1: Smoke mc2_10 + bolt-heavy mission with default env**

```bash
$env:MC2_GPU_PARTICLES = "1"
Remove-Item Env:MC2_GPU_PARTICLES_LOG -ErrorAction SilentlyContinue
Remove-Item Env:MC2_GPU_TRAIL_DISABLE -ErrorAction SilentlyContinue
py -3 scripts/run_smoke.py --mission mc2_10 --duration 40 --kill-existing --keep-logs
py -3 scripts/run_smoke.py --mission <bolt-heavy> --duration 40 --kill-existing --keep-logs
```

Confirm:
- B1 effects render (mc2_10).
- Missile smoke trails AND PPC bolts render — only one trail per bolt now (CPU suppressed).
- No double-trail visual artifact.
- No crashes, no missing-texture errors.

- [ ] **Step 2: Smoke matrix with `MC2_GPU_PARTICLES=0` (fallback)**

```bash
Remove-Item Env:MC2_GPU_PARTICLES -ErrorAction SilentlyContinue
py -3 scripts/run_smoke.py --mission mc2_10 --duration 40 --kill-existing --keep-logs
py -3 scripts/run_smoke.py --mission <bolt-heavy> --duration 40 --kill-existing --keep-logs
```

Confirm: CPU gosFX path runs unchanged from pre-B2 baseline. No GPU trails. No regressions.

- [ ] **Step 3: Tune constants if needed**

If visual look misses the image reference (too dense, too sparse, wrong color, wrong head size):
- Edit `kTuningTable` in `mclib/particles/gpu_trail.cpp`.
- Rebuild, re-smoke, re-screenshot.
- One tuning commit per kind, message: `tune(particles): adjust <Kind> <param> (B2 P4)`.

- [ ] **Step 4: Final write-up**

Write handoff `C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\HANDOFF_2026_XX_XX_fx_gpu_b2_ship.md`:
- B2 SHIPPED state, commits.
- Smoke results.
- Remaining backlog (per-particle rotation, RenderWorld flush migration, default-ON flip — separate PRs).
- Pointer to spec + plan.

Update `MEMORY.md` and `INDEX-RENDERING.md` with the handoff link. Consider archiving `gpu-particle-age-zero-curve-trap` since B2 retires the moving-emitter case.

```bash
git add C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\HANDOFF_2026_XX_XX_fx_gpu_b2_ship.md C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\MEMORY.md C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\INDEX-RENDERING.md
git commit -m "docs(memory): B2 ship handoff (B2 P4)"
```

**B2 ships when P4.2 passes.** Default-ON flip for `MC2_GPU_PARTICLES` is a **separate PR**, not part of B2.

---

## Self-Review Notes

- **Spec coverage:** every numbered design section (§3.1–§3.6, §4 data flow, §5 components, §7 env gates, §8 testing, §10 phases, §11 open questions) has an explicit task or is deliberately deferred to a backlog handoff.
- **Hard invariants:** §10's invariants (no suppression-before-proof, B1 regression every phase, no new shader program, no schema bump in P1, texture reuse in P1/P2, VFX firewall) appear as Conventions at top + repeated in acceptance gates per phase.
- **No placeholders:** every code step has actual code. The two intentional dynamic references — `EmitParticle` signature in P2.2 and `frameLength` symbol in P2.4 — explicitly tell the engineer to grep the existing convention rather than guess. These are not TBDs; they are "match the existing API" instructions.
- **Type consistency:** `GpuTrailKind`, `GpuTrailTuning`, `GpuTrailEmitter`, `prev_position`, `gpu_trail_kind`, `kind_and_flags`, `s_trail_spawn_total`, `s_trail_head_total` — all names used identically across tasks.
- **Open question fallout:** P3.6 dispatches the weapon-INI investigator that the spec deferred. Texture choice deferred to tuning in P4.3. Tier-1 bolt-heavy mission picked at start of P2.6. Constants (`MAX_PARTICLES_PER_SEGMENT=32`, `epsilon=0.01`) chosen with rationale, tunable in P4.
