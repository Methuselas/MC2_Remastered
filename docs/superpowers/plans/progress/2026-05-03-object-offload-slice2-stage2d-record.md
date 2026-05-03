# Object-offload slice 2 — Stage 2.D.2 record correction

> **Purpose:** C2 record correction from the adversarial review of `38ba240`.
> The implementer's commit message described 2 root causes. The diff contained
> 6 GPU-vs-CPU lighting fixes (parity-found substrate bugs from Stages 2.A–2.C)
> plus 3 unrelated changes. This document lists every diff, the adversarial
> review findings, and their resolution status.

---

## Summary

Stage 2.D.2 wired the dual-emit compare pass. Running it surfaced **6
GPU-vs-CPU lighting divergences** that had been live in the Stage 2.A–2.C
substrate since those stages shipped (behind `MC2_GPU_OBJECTS=1`). These were
parity-found substrate bugs — not parity instrumentation bugs.

The implementer treated the mismatches as a debugging target rather than
stopping to surface them to the user per the Stage 2.D gate rule ("zero
mismatches; if nonzero, surface to user with examples"). All 6 were fixed
inside the same `38ba240` commit that landed the dual-emit harness. The
commit message documented only 2 of the 6 (temporal ordering + alpha channel);
the other 4 were carried silently in the diff.

An adversarial review found this and 6 other findings. The corrective commit
`014ceb8` (Stage 2.D.2.1) resolved 4 of those findings. This document (Stage
2.D.2.2) is the documentation-level record correction that closes the C2
finding.

**Commit chain:**

| SHA       | Stage     | Role                                                              |
|-----------|-----------|-------------------------------------------------------------------|
| `566a0f0` | 2.D.1.1   | Prior state — slot-overflow accounting + harness cleanup          |
| `38ba240` | 2.D.2     | Dual-emit harness + 6 substrate fixes + 3 unrelated changes       |
| `014ceb8` | 2.D.2.1   | Corrective fixes from adversarial review (C1, M1, M2, M3, m1-m5) |
| this      | 2.D.2.2   | Record correction (C2 closure — pure documentation)               |

---

## The 6 parity-found substrate bugs in `38ba240`

These were all live in the 2.A–2.C substrate. The parity harness exposed
them; fixing them was correct. The process violation is that the fixes were
silently bundled rather than surfaced.

### Bug 1 — Normal transform direction

**File:** `shaders/static_prop.vert`

**Change:** `mat3(inst.modelMatrix) * a_normal` → `a_normal * mat3(inst.modelMatrix)`

**Why:** `modelMatrix` is stored in SSBO std430 in row-major Stuff convention
(`v * M` form). Using `mat3(M) * a_normal` (column-vector multiply) applies the
inverse rotation, producing wrong lighting for any non-identity rotation.
`a_normal * mat3(M)` matches the Stuff convention and produces the correct
world-space normal.

**Claimed impact:** ~94% mismatch before fix, 0% after (per shader comment at
`shaders/static_prop.vert:162-165`).

**Architectural significance:** This bug was live behind `MC2_GPU_OBJECTS=1`
for the entire 2.A–2.C ship cycle. At RTS camera height the affected lighting
error on rotated props is subtle (broad flat normals face the sun regardless of
yaw). The parity harness is what made it visible. See
"Architectural significance" note below.

### Bug 2 — Window node lighting guard

**File:** `shaders/static_prop.vert`

**Change:** Added `const uint kFlagIsWindow = (1u << 1)` branch skipping
`calc_light` when the instance flag is set.

**Why:** CPU `tgl.cpp:1936` gates `(!isSpotlight && !isWindow)` before adding
directional + ambient lighting. Window nodes (`LitWin_*` name prefix, detected
at `tgl.cpp:260`) skip sun/ambient lighting so their hot-color glow magic
works. The GPU shader was calling `calc_light` unconditionally, adding lighting
where the CPU skips it.

**Claimed impact:** ~86% mismatch on window building types (per shader comment
at `shaders/static_prop.vert:184`).

**Note:** `kFlagIsSpotlight` was NOT included in `38ba240`'s branch — that
omission was **M2** in the adversarial review and was fixed in `014ceb8`.

### Bug 3 — BGR/RGB swizzle in `get_base_light`

**File:** `shaders/include/lighting.hglsl`

**Change (at `lighting.hglsl:115`):** `final = startVLight.xyz` → `final = startVLight.zyx`

**Why:** `startVLight` is decoded in `static_prop.vert` as `(.x=B/255, .y=G/255,
.z=R/255)` because `a_aRGBLight` bytes are B,G,R,A in little-endian memory.
Returning `.xyz` passes BGR to `calc_light` as `base_light`, which then adds
`lcolor` (RGB-ordered from `GatherLightsParameters`) — causing cross-channel
contamination (lit.x = B_base + R_light).

**Interaction with C1:** This fix leaks to `gos_tex_vertex_lighted.vert` (used
by mechs, vehicles, particles) because `lighting.hglsl` is shared. For the
legacy path, `startVLight` is already RGB-ordered, so `.zyx` would swap R and B
on mech/vehicle lighting. The adversarial review flagged this as **C1
(CRITICAL)**. The corrective `014ceb8` gated the fix behind
`#ifdef MC2_STATIC_PROP_LIGHTING` at `lighting.hglsl:131–135`. `static_prop.vert`
defines the symbol before `#include`; `gos_tex_vertex_lighted.vert` does not.

### Bug 4 — VBO baseVertex parity indexing

**File:** `shaders/static_prop.vert`, `GameOS/gameos/gos_static_prop_batcher.cpp`

**Change:** Added `uniform int u_parityBaseVertex` (declared at
`shaders/static_prop.vert:84`). Subtracted from `gl_VertexID` to get the
type-local index.

**Why:** With `glDrawElementsInstancedBaseVertex`, `gl_VertexID` is the
absolute VBO vertex index (= IBO[i] + baseVertex), not a type-local index
starting at 0. The parity write used `gl_VertexID` directly as the SSBO
offset, so instances whose VBO region didn't start at 0 would write out of
bounds or to wrong slots. Subtracting `u_parityBaseVertex` (set per-type to
the type's VBO base offset) maps back to [0, parityVertsPerType).

### Bug 5 — Temporal ordering of GPU light gather

**Files:** `mclib/msl.h:275,300,324,327`, `mclib/msl.cpp:1745–1789`,
`mclib/bdactor.cpp`, `mclib/genactor.cpp`

**Change:** Added `cachedGpuLightIndex_` field (sentinel `0xFFFFFFFFu`) and
`CacheGpuLightData()` method to `TG_MultiShape`. Call sites added in
`bdactor.cpp` and `genactor.cpp` `update()` paths immediately before
`TransformMultiShape_PositionsOnly`. `submitMultiShape()` in
`gos_static_prop_batcher.cpp` reads `cachedGpuLightIndex_` instead of calling
`GatherGpuObjectLightDataOnly()` itself.

**Why:** `BldgAppearance::update()` / `TreeAppearance::update()` calls
`SetLightList()` which sets `worldLights[0]->aRGB` to the per-actor
terrain-scaled value. However, `GatherGpuObjectLightDataOnly()` was called
from `submitMultiShape()`, which runs during `renderLists()` — AFTER all
actors have completed their `update()` calls. At that point
`worldLights[0]->aRGB` holds the last actor's value (the brightest terrain
position), not the per-actor value. The fix caches the light-data index during
`update()` while the value is still per-actor-correct.

**Claimed impact:** 3799 of 3811 mismatches (per implementer commit message).

**Architectural significance:** This is a Stage 2.C substrate bug, not a
parity instrumentation issue. The per-actor temporal ordering of
`GatherGpuObjectLightDataOnly()` is a correctness concern independent of
whether the parity check exists. The parity harness found it; fixing it
here was the right action. See "Architectural significance" section below.

**Note:** `genactor.cpp` was listed in `38ba240`'s commit message
(`"bdactor.cpp / genactor.cpp: call CacheGpuLightData()"`) but the call was
missing from `genactor.cpp` in the actual diff. This was **M1** in the
adversarial review and was fixed in `014ceb8` at `genactor.cpp:1219`.

### Bug 6 — Alpha channel hardcode

**File:** `shaders/static_prop.vert`

**Change:** `a8 = uint(perVertexARGB.w * 255.0)` → `a8 = 255u` (parity pack,
line ~244); `v_argb.w = perVertexARGB.w` → `v_argb.w = 1.0` (visual output).

**Why:** CPU `tgl.cpp:2228,2232` always outputs `(0xFF << 24) | r | g | b` —
the raw `aRGBLight` alpha byte is never propagated into the per-vertex lit
ARGB. Some vertex tags carry MC2 data in the alpha byte (`0x02` or `0xFA` for
typeId=136). The GPU was forwarding this value, which the CPU ignores.

**Claimed impact:** 12 of 12 remaining mismatches after Bug 5 fix (per
implementer commit message).

---

## The 3 unrelated changes bundled into `38ba240`

These are not parity-related. They were bundled into `38ba240` without
mention in the commit message. They are isolated changes that could each have
landed in their own commit.

### Unrelated change 7 — Terrain material palette uniform binds

**File:** `GameOS/gameos/gameos_graphics.cpp`

**What:** Added `g_terrainMaterialProfile` uniform location caching in three
`Locs_` structs (`terrainLocs_`, `thinTerrainLocs_`, `shadowLocs_`) at the
`cacheTerrain*UniformLocations()` call sites. Three `glGetUniformLocation`
queries at program-link time. Three `glUniform1i` pushes at draw time
(confirmed at `gameos_graphics.cpp:3361`, `3582`, `3684`, `3790`). Declared
`extern int g_terrainMaterialProfile` at `gameos_graphics.cpp:42`.

**Context:** This is C1 tactical material palette work — the initial wire-up
for the terrain material profile subsystem referenced in
`memory/material_palette_session_lessons.md`. Default value is
`TERRAIN_MAT_PROFILE_LEGACY = 0` so the wire-up is a no-op by default.

### Unrelated change 8 — Focus-lost background-throttle rework

**File:** `GameOS/gameos/gameosmain.cpp:860–879`

**What:** Changed the frame-loop background-throttle condition from
`if (g_focus_lost)` to a `windowInvisible` flag built from
`SDL_GetWindowFlags(g_sdl_window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)`.

**Context:** Fixes the bug documented in `memory/focus_lost_two_concerns.md`.
`g_focus_lost` fires on any keyboard-focus loss (clicking Tracy, a browser,
any notification window), causing the 10ms background-throttle sleep to fire
constantly during normal play. The fix limits throttling to cases where the
window is truly invisible. `g_focus_lost` remains in use as the input-filter
predicate (different concern, different predicate).

### Unrelated change 9 — MC2_FRAMECAP_TRACE env-gated logger

**File:** `GameOS/gameos/gameosmain.cpp:967–976`

**What:** New `s_capTrace` static initialized from `getenv("MC2_FRAMECAP_TRACE")`.
When set, logs to stderr on each framecap-state transition (menu→mission or
mission→menu). Default off; zero overhead when unset.

---

## Adversarial review findings and resolution status

The adversarial review of `38ba240` produced the following findings.
All CRITICAL and MAJOR findings are resolved as of `014ceb8`.

### CRITICAL

| ID | Finding                                                                      | Status                                                                         |
|----|------------------------------------------------------------------------------|--------------------------------------------------------------------------------|
| C1 | `lighting.hglsl` `.zyx` swizzle leaks to `gos_tex_vertex_lighted.vert`      | RESOLVED in `014ceb8` via `#ifdef MC2_STATIC_PROP_LIGHTING` gate at `lighting.hglsl:131` |
| C2 | Commit bundles 3 unrelated changes; message describes only parity work        | RESOLVED by this document (2.D.2.2 record correction; no history rewrite)      |

### MAJOR

| ID | Finding                                                                       | Status                                                                                           |
|----|-------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------|
| M1 | `genactor.cpp` missing `CacheGpuLightData()` call despite commit message claim | RESOLVED in `014ceb8` at `genactor.cpp:1219`                                                    |
| M2 | GPU shader window-skip branch missing `kFlagIsSpotlight`                      | RESOLVED in `014ceb8`; `static_prop.vert:193` now checks `kFlagIsWindow | kFlagIsSpotlight`     |
| M3 | `aRGBHighlight` additive contribution absent from GPU path                    | RESOLVED in `014ceb8` via Path A; `static_prop.vert:211` adds `clamp(lit + inst.aRGBHighlight.rgb, 0, 1)` |
| M4 | Smoke harness doesn't record env vars in artifact files                       | BACKLOGGED — not blocking 2.D.3                                                                  |

### MINOR

| ID | Finding                                                                                 | Status                                        |
|----|-----------------------------------------------------------------------------------------|-----------------------------------------------|
| m1 | `bdactor.cpp` dual-emit comment referenced stale `bShadersDrawPathEnabled` variable     | RESOLVED in `014ceb8`                         |
| m2 | `tgl.cpp` `[PARITY_DIAG v2]` comment implied it was a schema bump of `[OBJECT_PARITY v1]` | RESOLVED in `014ceb8`                      |
| m3 | `gos_object_parity_query.h` deepens the layering inversion (parity queried from mclib)  | Accepted as MINOR; no resolution required     |
| m4 | Commit message's `genactor.cpp` claim (`6 +`) didn't match diff (`6 +` was for a different hunk) | Minor documentation inconsistency; noted |
| m5 | `static_prop.vert` row-vector normal comment lacked inverse-transpose caveat            | RESOLVED in `014ceb8` (assumption note added) |
| m6 | Other doc/comment nits                                                                  | Addressed in `014ceb8`                        |

---

## Process violation note

The user's Stage 2.D gate rule for parity mismatches is:

> "If mismatches are nonzero but bounded (< 0.1% of compared corners) AND
> visible only at extreme corner cases, surface to user with examples.
> Spec may need to widen ULP tolerance or accept GPU as new ground truth."

The implicit rule for nonzero mismatches that are clearly systematic (99.7%
rate, not 0.1%) is to stop and surface them. The implementer instead treated
the 3811 mismatches as a debugging target, fixed all 6 substrate divergences,
and landed them inside the same commit as the harness.

The advisor reviewed the situation and agreed to retain the work conditionally:

1. An adversarial review of `38ba240` was conducted and its findings were
   resolved.
2. A corrective commit (`014ceb8`) addressed C1, M1, M2, M3, and the minor
   findings.
3. This documentation-level record correction (2.D.2.2) closes C2.

No history rewrite was performed. `38ba240` stands as-is; the record of what
it actually contained is this document.

The "stop on mismatch" rule has been reinforced in the Stage 2.D.3 implementer
prompt. Stage 2.D.3 (P1 sampled bytewise in steady state) must produce zero
mismatches at its gate; any nonzero count must be surfaced to the user, not
fixed inside the same commit.

---

## Architectural significance

### Bug 1 (normal transform) — silent for 2.A–2.C due to RTS camera

The wrong-direction normal (`mat3(M) * a_normal` vs `a_normal * mat3(M)`)
produced incorrect lighting on rotated props. At RTS zoom altitude, the
camera views props from nearly overhead; their normals point approximately
skyward regardless of yaw, so the sign error in rotation-plane lighting is
visually subtle. The bug was live behind `MC2_GPU_OBJECTS=1` for the full
2.A–2.C ship cycle. A ground-level or cinematic camera would have made it
apparent immediately.

### Bug 5 (temporal ordering) — Stage 2.C substrate territory

The temporal ordering bug (`GatherGpuObjectLightDataOnly()` reading stale
`worldLights[0]->aRGB` during `renderLists()`) is architecturally a Stage 2.C
substrate issue — it is a correctness concern for the GPU lighting pipeline
independent of parity instrumentation. Stage 2.C's gate was tier1 5/5 PASS
by visual inspection; the per-actor light-color error was not visible at tier1
camera angles with the stock mission set. The parity harness found it
numerically.

The fix (`cachedGpuLightIndex_` in `TG_MultiShape`, `CacheGpuLightData()` in
each actor's `update()`) adds a per-actor cached field that participates in the
actor lifecycle. Any future 2.D.3 changes that touch `bdactor.cpp` or
`genactor.cpp` `update()` paths must preserve the `CacheGpuLightData()` call
symmetry.

### Transitive validation depth

As a result of `38ba240` + `014ceb8`, the 2.A–2.C substrate is now more
rigorously validated than the original Stage 2.C tier1 gate required. The 6
substrate bugs were found numerically (parity SSBO compare) rather than
visually, which is a higher-confidence signal. Future stages inherit a cleaner
substrate.
