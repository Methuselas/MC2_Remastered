# FX-GPU-1 B2 — Bolt-Weapon Trails + View-Aligned Billboards

**Status:** DRAFT — pending review
**Date:** 2026-05-25
**Branch:** `claude/nifty-mendeleev`
**Predecessor:** FX-GPU-1 B1 (commits `95a83942`, `10a07a3f`) — GPU path emits all five gosFX effect types as textured screen-facing sprites under `MC2_GPU_PARTICLES=1`.

---

## 1. Goal

Replace the broken gosFX moving-emitter Tube/Card path for **bolt-style weapons** (missiles, PPC, SRM, LRM, MRM, AC, gauss) with a purpose-built GPU trail emitter driven directly from the weapon-bolt entity. Fix view-aligned billboards so existing CardCloud/etc effects remain correct under camera rotation. Deliver a GPU path good enough that `MC2_GPU_PARTICLES` can be flipped default-ON after this ships.

Out of scope (deferred to backlog):
- Per-particle rotation (`SpinningCloud::m_localRotation`)
- RenderWorld flush-hook migration (current `gamecam.cpp` site works; revisit when RenderWorld M3+ lands)
- Default-ON flip itself (separate post-ship gate after a tier-1 smoke pass)
- Tube spawner live-geometry reading (obsoleted — PPC is a moving projectile, not a static tube)

---

## 2. Motivation

B1 handoff documented two broken effect classes:

1. **Missile smoke trails** — gosFX bakes particle positions at `parent_age=0.5f`. For a moving emitter, every `Draw()` emits at the same baked world position. No trail strings behind the missile.
2. **PPC beam** — `spawn_tube.cpp` reads `parentToWorld` origin only, never the live tube endpoints.

Investigation surfaced two facts that simplify the fix:

- **No PPC special-case code exists.** All bolt weapons are generic `WeaponBolt` instances differentiated only by INI config (`fireEffect`, `trailEffect` handles). One call-site hook in `WeaponBolt::update()` serves every bolt weapon.
- **`WeaponBolt::trailEffect` field already exists** (`weaponbolt.h:51`) but is never invoked. We are filling a stub, not replacing a live path.

PPC turns out to be a moving projectile with a bright head sprite and a faint trail — not a static beam. Collapses onto the same primitive as missile trail (different tunings, same emitter family).

---

## 3. Architecture

### 3.1 Hook site

Single per-frame hook in `WeaponBolt::update()` (`code/weaponbolt.cpp` ~L380):

```cpp
void WeaponBolt::update()
{
    Stuff::Vector3D prev = position;          // NEW: snapshot
    /* …existing position update… */
    if (gpu_trail_kind != GpuTrailKind::None)  // NEW: trail spawn
        GpuTrailEmitter::Spawn(gpu_trail_kind, prev, position, deltaT);
}
```

`prev_position` lives on `WeaponBolt` only — do not bloat the base `GameObject`. `deltaT` already available from the surrounding update tick.

### 3.2 New module: `mclib/particles/gpu_trail.{h,cpp}`

**Shader policy (load-bearing):** Do **not** add a new `particle_trail.vert/frag` program. Trails are camera-facing sprites that differ from existing GPU particles only in texture, color, size, lifetime, blend mode, and a kind/head flag. All of those ride through the existing billboard shader + bridge + group batching that B1 stabilized. A second shader program adds variant maintenance and risks diverging from the fixed gosFX path. **Rule:** one particle billboard shader; group-level texture/blend; per-particle color/size/lifetime/kind; no new shader program for B2 unless profiling or visuals prove it necessary.

```cpp
enum class GpuTrailKind : uint8_t {
    None = 0,
    MissileSmoke = 1,   // white, soft, long lifetime, no head sprite
    PpcBolt      = 2,   // bright blue head sprite + short additive tail
};

struct GpuTrailTuning {
    glm::vec4 head_color;
    glm::vec4 trail_color;
    float head_size;          // world units, 0 = no head
    float trail_particle_size;
    float trail_lifetime_s;
    float trail_density_per_meter;  // particles spawned per meter of segment
    uint8_t blend_mode;       // alpha / additive
    uint16_t texture_id;      // bridge texture handle
};

class GpuTrailEmitter {
public:
    static void Spawn(GpuTrailKind kind,
                      const Stuff::Vector3D& prev_world,
                      const Stuff::Vector3D& cur_world,
                      float deltaT);
private:
    static const GpuTrailTuning& tuning_for(GpuTrailKind k);
};
```

`Spawn()`:
1. Look up tuning by kind.
2. Compute segment length `len = |cur - prev|`. If `deltaT <= 0 || len < epsilon`: spawn head only (if `tuning.head_size > 0`), return — no trail particles.
3. Otherwise compute count `N = clamp(ceil(len * tuning.trail_density_per_meter), 0, MAX_PARTICLES_PER_SEGMENT)`. The cap is a hard ceiling that protects against fast projectiles, low frame rate, and teleport-like updates. Start `MAX_PARTICLES_PER_SEGMENT = 32`; tune from smoke output.
4. For `i in [0..N)`: spawn one `GpuParticle` at `lerp(prev, cur, i/N)` with `birth_time = now`, `lifetime = tuning.trail_lifetime_s`, color/size from tuning, `kind` packed into the spare schema byte. Emit into the **current frame's** group for `(texture_id, blend_mode, kind)` — create that group only if it does not already exist this frame. Producers emit particles; the batcher coalesces. The API must make per-Spawn group creation impossible.
5. If `tuning.head_size > 0`: spawn one head particle at `cur_world`, lifetime = single frame (re-spawned next tick), `is_head` bit set in the kind byte so the shader can size/color it differently.

No `prev_position` storage in `GpuTrailEmitter` — caller owns it.

### 3.3 Weapon-config → kind mapping

`WeaponBolt` ctor reads existing weapon INI fields and assigns `gpu_trail_kind` once at construction:

```cpp
// pseudocode in WeaponBolt::init() after INI load
if (trailEffectName == "missilesmoke" || strstr(trailEffectName, "smoke"))
    gpu_trail_kind = GpuTrailKind::MissileSmoke;
else if (strstr(weaponName, "ppc") || trailEffectName == "ppcbolt")
    gpu_trail_kind = GpuTrailKind::PpcBolt;
else
    gpu_trail_kind = GpuTrailKind::None;
```

Mapping table lives in `gpu_trail.cpp`. If a bolt has `None`, no GPU trail spawns — CPU gosFX path remains responsible (default behavior when `MC2_GPU_PARTICLES=0`).

When `MC2_GPU_PARTICLES=1` AND `gpu_trail_kind != None`: GPU emitter runs. CPU gosFX `trailEffect` is suppressed **only in P4, and only for kinds with confirmed GPU visual proof** (see §10 hard invariants). In P2/P3 both CPU and GPU trails run in parallel — that is the proof mechanism.

### 3.4 View-aligned billboards

**Bridge accessor (new, temporary):** `GameOS/gameos/gos_particle_bridge.cpp`

```cpp
void gos_GetCameraRight(float out[3]);
void gos_GetCameraUp(float out[3]);
```

Implementation reads from the active `Camera` instance (`camera.cpp:110-115` already extracts `right`/`up` from `invView`). Add a static `Camera* g_active_camera = nullptr` set by `GameCamera::render()` before particle flush, cleared after.

**Future-API note (load-bearing):** `g_active_camera` is a stop-gap. The target API form is a `RenderFrameContext` passed into particle flush:

```cpp
struct RenderFrameContext {
    glm::mat4 worldToClip;
    glm::vec3 cameraRight, cameraUp, cameraForward;
    glm::ivec4 viewport;
    bool reverseZ;
    float frameTime;
};
void Batcher::Flush(const RenderFrameContext& ctx);
```

No global camera pointer, no `gos_GetCameraRight`, no set/clear ceremony. B2 ships the temporary bridge because it is the smallest path; the `RenderFrameContext` migration is a separate phase tracked alongside the RenderWorld flush-hook relocation.

**Axis verification (P1 blocker):** B1 carried a load-bearing Stuff→MC2 axis conversion. Camera `right`/`up` from `invView` are in **view space inverse → world space** in `camera.cpp` terms, but `particle_center` arrives in Stuff world space after the existing axis swap in `particle_billboard.vert`. P1 must explicitly verify that `cameraRight`/`cameraUp` are in the same world space the shader uses for `particle_center` *after* its axis swap. The verification recipe matches what `static_prop` and the B1 axis fix used: park camera at a known yaw, place a persistent particle effect at a known world point, rotate, and confirm sprites do not slide/skew relative to the ground. A wrong basis will look like billboards drifting under camera rotation, not like outright invisibility — so this needs deliberate visual check, not just a smoke pass.

**Uniform upload:** Inside `gos_particle_bridge` per-flush setup (alongside existing camera uniforms):

```cpp
glUniform3fv(u_cameraRight_loc, 1, cameraRight);
glUniform3fv(u_cameraUp_loc,    1, cameraUp);
```

**Shader change:** `shaders/particle_billboard.vert`

Replace fixed XZ-plane corner offsets with:

```glsl
uniform vec3 u_cameraRight;
uniform vec3 u_cameraUp;

vec3 worldPos = particle_center
              + u_cameraRight * corner_offset.x * particle_size
              + u_cameraUp    * corner_offset.y * particle_size;
```

This single shader change benefits **all** GPU particles (existing CardCloud/PointCloud/etc), not just the new trail emitters. Removes the "thin strip on east-west camera rotation" regression.

### 3.5 Shader: extend existing `particle_billboard.{vert,frag}` — no new program

Trails ride the existing billboard shader. The only schema addition is one byte packing `kind` (4 bits — supports up to 16 kinds) and `is_head` (1 bit) into a previously spare slot of the 64-byte `GpuParticle`. Per-particle color/size/lifetime are already in schema.

Frag-shader use of `kind`:
- Texture and blend mode are group-level (Batcher routes by `(texture_id, blend_mode, kind)`), so the shader does not switch blend func or texture by branch.
- The shader may optionally use `kind`/`is_head` to gate small fragment-side tweaks (e.g. additive intensity multiplier for `is_head`, soft-edge falloff for `MissileSmoke`). Keep these to a few lines of branching at most. If a kind needs anything more elaborate, that is a signal to revisit "one shader" — but it is **not** an automatic green light to add a new program.

Rule from §3.2 stands: no new shader program for B2 unless profiling or visuals prove it necessary.

### 3.6 Textures

- `missilesmoke.tga` — soft white puff (existing gosFX trail texture if usable; otherwise new asset).
- `ppcbolt.tga` — bright blue radial glow with white core.

Both registered via the existing `Batcher::ResolveTextures()` path. No new texture-resolution code needed.

---

## 4. Data flow

```
WeaponBolt::update()
    ├─ snapshot prev_position
    ├─ existing position update
    └─ GpuTrailEmitter::Spawn(kind, prev, cur, dt)
            └─ Batcher::EmitGroup(...)   [per-frame, additive into ring buffer]

GameCamera::render()  (existing site)
    ├─ g_active_camera = this
    ├─ renderLists()
    ├─ Batcher::ResolveTextures()
    ├─ Batcher::Flush()
    │     └─ gos_particle_bridge
    │           ├─ glUniform3fv(cameraRight/Up)  [NEW]
    │           ├─ glUniform existing camera/MVP
    │           └─ glDrawElements per group
    └─ g_active_camera = nullptr
```

---

## 5. Components & boundaries

| Unit | Files | Responsibility | Depends on |
|---|---|---|---|
| `WeaponBolt` trail hook | `code/weaponbolt.{h,cpp}` | snapshot prev_position, call Spawn each tick | `GpuTrailEmitter`, kind enum |
| `GpuTrailEmitter` | `mclib/particles/gpu_trail.{h,cpp}` | segment-stamping, tuning lookup, batcher emit, per-segment cap | `Batcher`, `GpuParticle` schema |
| Kind→tuning table | `mclib/particles/gpu_trail.cpp` (const data) | declarative tuning per weapon class | none |
| Camera basis bridge (temp) | `GameOS/gameos/gos_particle_bridge.{cpp,h}`, `GameOS/gameos/utils/camera.cpp` | expose right/up from active camera; superseded by `RenderFrameContext` | `GameCamera` set/clear |
| Particle shader | `shaders/particle_billboard.{vert,frag}` (extended only — no new program) | view-aligned corner offset, optional per-kind frag tweaks | uniforms `u_cameraRight/Up`, `kind`/`is_head` in particle |
| Schema bump | `mclib/particles/gpu_particle.h` | pack `kind` (4 bits) + `is_head` (1 bit) into spare byte of 64B record | none |

`GpuTrailEmitter` is the only new module. It has no external state and no consumers besides the WeaponBolt hook — fully isolatable, testable by feeding synthetic prev/cur positions and inspecting batcher emissions.

---

## 6. Error handling

- `Spawn()` with `kind == None`: no-op (caller already gated, defense in depth).
- Zero-length segment (`prev == cur`, e.g. paused or first frame): spawn head only (if any), skip trail particles. Log nothing.
- Batcher full: existing batcher overflow path applies (drop with debug log under `MC2_GPU_PARTICLES_LOG=1`).
- `g_active_camera == nullptr` at flush time: bridge uses last-known basis or fallback identity; log once under `MC2_GOSFX_GROUP_LOG=1`. This should never happen in normal play but is non-fatal if it does.

No new failure modes that need user-visible recovery. Worst case under bug: trail invisible — degrades to current B1 behavior, which is already shipping.

---

## 7. Env gates

| Var | Effect |
|---|---|
| `MC2_GPU_PARTICLES=1` | Enables GPU path (existing). New trail emitters fire. CPU gosFX `trailEffect` for bolts with `gpu_trail_kind != None` is suppressed. |
| `MC2_GPU_PARTICLES_LOG=1` | Existing SPAWN_PROBE/RESOLVE_PROBE. New: TRAIL_SPAWN_PROBE (once per kind, dump tuning + first segment). |
| `MC2_GOSFX_GROUP_LOG=1` | Existing bridge UV dump + missing-tex errors. Now also flags missing camera basis. |
| `MC2_GPU_TRAIL_DISABLE=1` | **New.** Force-off the trail emitter even when `MC2_GPU_PARTICLES=1`. Lets us isolate trail bugs from other GPU-particle bugs during smoke. |

Default-OFF on all. Default-ON flip for `MC2_GPU_PARTICLES` is a separate post-ship decision after a clean mc2_10 smoke pass.

---

## 8. Testing

### 8.1 Smoke

`mc2_10` is the only tier-1 mission exercising gosFX (`mc2_10_is_only_tier1_with_gosfx` memory). Add a second mission to the matrix that fires bolt weapons heavily — pick from existing tier-1 set based on which has the most missile/PPC fire (investigator follow-up if needed).

```
py -3 scripts/run_smoke.py --mission mc2_10  --duration 40 --kill-existing --keep-logs
py -3 scripts/run_smoke.py --mission <bolt-heavy> --duration 40 --kill-existing --keep-logs
```

**Expected with `MC2_GPU_PARTICLES=1`:**
- `emit_total` increases vs B1 baseline (extra trail particles per bolt).
- New `trail_emit_total` counter > 0 if any bolts fired.
- Draw-group count stays bounded under missile volleys (no per-Spawn inflation).
- No crashes, no missing-texture errors in bridge log.
- Visual: missile trails arc behind launched missiles; PPC bolts show blue head + faint tail traveling to target.

### 8.1.1 B1 regression criterion (every phase)

mc2_10 intro/early gameplay:
- B1 textured gosFX (explosions, fire, smoke) still visible.
- No square patches.
- No debug canaries.
- Camera rotation through full 360° does not turn sprites into thin strips.

If any of the above fails, the phase does not ship — fix or revert before merge.

### 8.2 Unit-level

`GpuTrailEmitter::Spawn()` is pure given inputs. Drive it with synthetic prev/cur positions in a test harness (gtest-style if available, otherwise a small ad-hoc exe) and assert:
- `N` trail particles spawn for a segment of length `L` with density `D`, where `N = ceil(L*D)`.
- All spawned positions are colinear between prev and cur.
- Zero-length segment spawns 0 trail particles (head still optional).

### 8.3 View-aligned visual verification

Camera-rotation manual test: load a mission with persistent particle effects (e.g. fire/smoke from destroyed building in mc2_10), rotate camera through full 360° using debug camera controls, confirm sprites remain face-on at every angle (no thin-strip regression).

---

## 9. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Suppressing CPU `trailEffect` for `gpu_trail_kind != None` breaks the CPU fallback path | Med | `MC2_GPU_PARTICLES=0` short-circuits the suppression; CPU path runs unchanged. Verified by smoke with gate off. |
| Schema bump (kind byte) breaks bridge unpack | Med | One-commit lockstep change: schema header + bridge + shader updated together. Existing `shader_exe_deploy_lockstep` discipline applies. |
| Camera basis stale (set/clear race on multi-camera frames) | Low | Active-camera pointer set immediately before particle flush, cleared immediately after, within same render method. No async access. |
| Trail density too high → batcher overflow on heavy missile mission | Low | `trail_density_per_meter` tunable per kind. Start conservative (e.g. 8/m for missile, 4/m for PPC), tune from smoke output. |
| Weapon INI mapping wrong (misses an existing weapon name) | Low | Default `None` = falls back to CPU gosFX, no visual regression vs B1. Discoverable via smoke + manual fire test. |

---

## 10. Implementation phases

**Hard invariants across all phases:**
- **No CPU `trailEffect` suppression** until a specific `GpuTrailKind` has visual proof on the GPU side. Hardcoded test mappings are allowed in early phases *only without suppression* — otherwise a regression hides behind the test hack.
- **B1 regression is a release gate every phase.** Each phase's smoke run must confirm B1 textured gosFX still renders on mc2_10 (no square patches, no debug canaries, no thin-strip behavior on camera rotation).
- **Hard boundary at P1.** No trail work begins until P1 ships and the camera-rotation visual verification passes.
- **Texture reuse early.** P1 and P2 must use only existing in-tree textures. New texture assets are P3/P4 polish work. Reason: a trail bug must not be confounded with "asset missing/wrong" during early debugging.
- **Schema bump deferred.** Do **not** bump `GpuParticle` schema in P1. Avoid in P2 unless implementation forces it. The 4-bit `kind` + 1-bit `is_head` bump belongs in P3 when multiple kinds and head/tail distinction become real. Lockstep schema/bridge/shader commit when it lands (`shader_exe_deploy_lockstep` discipline).
- **VFX firewall.** Particle shader changes must not write object IDs. Particles must not register `RenderObjectHandle`s with RenderWorld. VFX stays transient and outside the extraction path (continues the B1 firewall pattern).

**P1 — View-aligned billboard only**
- Existing shader path only. No new module. **No schema bump.**
- Add `gos_GetCameraRight/Up` + `g_active_camera` set/clear bridge.
- Extend `particle_billboard.vert` to consume `u_cameraRight/Up` for corner offsets.
- **Axis verification:** static-prop-style camera rotation test. Place persistent particle effect, rotate camera 360°, confirm no slide/skew.
- **P1 acceptance (in addition to §8.1.1 B1 regression criterion):**
  - No Stage 1′ debug canary path reachable (the magenta/checker canary used in B1 visibility debugging must not fire under any code path in this build).
  - Pink colorkey discard still works after shader changes — transparent regions on existing gosFX textures remain transparent, not pink squares.
- Smoke mc2_10. Confirm B1 effects survive + rotate correctly. **Hard gate.**

**P2 — Trail emitter infrastructure, no suppression**
- New `GpuTrailEmitter` module with `MissileSmoke` tuning only.
- Add `prev_position` to `WeaponBolt`, snapshot + Spawn call in `update()`.
- Conservative `MAX_PARTICLES_PER_SEGMENT` cap.
- Hardcoded test mode allowed: every bolt = `MissileSmoke`. **CPU trail still runs.** Both visible = correct.
- Smoke a bolt-heavy mission + mc2_10. Confirm trails appear AND B1 still renders.

**P3 — Batching + kind mapping**
- Coalesce groups by `(texture_id, blend_mode, kind)` — verify no per-Spawn group inflation under a missile volley (Tracy zone or counter).
- Investigator pass over `data/` weapon configs to lock the INI name set.
- Add `PpcBolt` tuning. Wire INI→kind table in `WeaponBolt::init()`.
- **No suppression yet.** CPU + GPU both run — confirms visual targets per-kind.

**P4 — Suppression + visual parity**
- Suppress CPU `trailEffect` only for kinds with confirmed GPU visual proof.
- Tune densities/colors/head sizes against image reference.
- mc2_10 + bolt-heavy mission smoke matrix pass.
- Default-ON proposal is a **separate PR** after P4, not part of B2.

Each phase commits independently with a smoke result in the message.

---

## 11. Open questions (defer to plan phase)

- Exact set of weapon INI names for kind mapping — needs short investigator pass over `data/` weapon configs.
- Head-sprite texture: reuse an existing asset (which?) or new `ppcbolt.tga`?
- Trail texture: reuse gosFX `tracesmoke.tga` if present, or new `missilesmoke.tga`?
- Tier-1 mission with heaviest bolt fire — pick during P2 planning.
- `MAX_PARTICLES_PER_SEGMENT` starting value (`32` is a placeholder) — tune from first smoke pass.
- `epsilon` for zero-length segment detection — pick a world-space threshold matching a fraction of the smallest expected per-frame missile displacement.

## 12. Future-API target (post-B2)

B2 ships a working trail system but with two pieces of debt the renderer-API rewrite should eliminate:

1. **`g_active_camera` global** → `RenderFrameContext` parameter on `Batcher::Flush` and any future producer-facing flush call.
2. **Producer-owned segment stamping** in `WeaponBolt::update()` → a higher-level VFX API:

   ```cpp
   Vfx::TrailBolt(kind, prev, cur, weaponContext);
   ```

   The renderer owns camera basis, batching, texture resolution, blend state, shader choice, per-frame budgets, and debug visibility. Producers describe *what* happened; the renderer decides *how* it draws.

These are intentionally out of scope for B2 — the goal is to deliver shippable trails without blocking on an API redesign. But the B2 code shape should not foreclose them: `GpuTrailEmitter::Spawn` is already the embryo of `Vfx::TrailBolt`.

---

## 13. Memory updates (after ship)

- New handoff: `HANDOFF_2026_XX_XX_fx_gpu_b2_ship.md`
- INDEX-RENDERING: new entry under "GPU particle path" linking the handoff
- Consider promoting `gpu-particle-age-zero-curve-trap` to `archive/` once B2 lands — moving-emitter case is no longer affected.
