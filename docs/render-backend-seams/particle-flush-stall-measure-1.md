# PARTICLE-FLUSH-STALL-MEASURE-1

A gated, measure-only CPU-timing probe for the class-D particle/tube flush paths. Answers
the one open class-D question from [class-d-sync-recon-1](class-d-sync-recon-1.md): *does
the implicit-sync `glBufferSubData` in particle/tube flush actually STALL, or is it pure
Vulkan migration debt?* **No sync mutation, no buffer-lifetime change, no rendering change.**

## What it is

`GameOS/gameos/particle_stall_probe.h` — header-only RAII `mc2::ScopedFlushTimer`, gated by
`MC2_PARTICLE_FLUSH_STALL_TRACE` (default OFF, byte-for-byte zero cost when off). Placed at
the entry of the three flush bodies in `gos_particle_bridge.cpp`:
- `gos_particle_bridge_flush` → `path=billboard`
- `gos_tube_ribbon_flush` → `path=tube`
- `gos_tube_ribbon_flush_deferred` → `path=tube_deferred`

It accumulates per-path `calls / avg_us / max_us` and emits a summary every 50 flushes
(one early one-shot at 10):

```
[PARTICLE_FLUSH_STALL] path=billboard calls=50 avg_us=12.4 max_us=18.0 (max~avg: no stall)
[PARTICLE_FLUSH_STALL] path=tube      calls=50 avg_us=9.1  max_us=2100.0 (SPIKE: possible implicit-sync stall)
```

## How to interpret

- **max_us ≈ avg_us, both small** → no stall; the implicit-sync upload is fine under GL.
  The class-D particle path is then **migration debt only** (port to staging/ring at
  Vulkan time, no GL action).
- **max_us ≫ avg_us (the SPIKE annotation: max > 8×avg and > 500 µs)** → the
  `glBufferSubData` is blocking on a prior-frame GPU read → a real stall → justifies a
  particle ring / staging implementation slice in GL.

## Measurement finding (DECISIVE)

**The instrumented particle/tube bridge flush path is GATED OFF by default.** The GPU
particle/tube bridge (`gos_particle_bridge_flush` / `gos_tube_ribbon_flush*` and their
class-D SSBO uploads, bindings 14/15/16) only runs when **`MC2_GPU_PARTICLES`** is enabled
(`mclib/particles/batcher.cpp:79`, `mclib/gosfx/effect.cpp:45`) — default OFF. With the
gate off, particles/tubes render via the **legacy CPU path** and the class-D
`glBufferSubData` uploads never execute.

Evidence:
- Automated capture (mc2_10, `run_visual_capture`, even with `MC2_FX_FORCE_SPAWN=1` firing
  8 weapon events): zero flushes.
- **Interactive real gameplay (mc2_01, full-screen, user fired weapons + explosions, 8045
  frames): zero `[PARTICLE_FLUSH_STALL]`, zero particle/tube markers** — because only
  `MC2_PARTICLE_FLUSH_STALL_TRACE=1` was set, not `MC2_GPU_PARTICLES=1`. Build confirmed
  (OBJBATCHER `gpu_drawn_note=legacy_path_only_v6_uses_submitted` = this build).

→ **Conclusion: the class-D particle/tube SSBO implicit-sync concern is MOOT on the
default config — the buffers are dormant.** A stall is only possible (and only worth
measuring) when `MC2_GPU_PARTICLES=1`. To get the number, re-run interactively with BOTH
`MC2_GPU_PARTICLES=1` and `MC2_PARTICLE_FLUSH_STALL_TRACE=1` (and `MC2_LOG=1`), drive
sustained FX, then read `[PARTICLE_FLUSH_STALL]`.

Like the terrain-solid MDI bridge, the GPU particle bridge is a present-but-not-live path
under default settings — so class-D particle work is unjustified unless/until
`MC2_GPU_PARTICLES` becomes default or is being actively used.

## Acceptance status

- Build green; gate OFF → byte-identical (probe self-gates, clock reads skipped). ✓
- Gate ON emits timing when the path is exercised (≥10 flushes). ✓ (mechanism verified;
  not triggered under capture — see finding).
- No `glMemoryBarrier`, no buffer-upload-strategy change, no ring/readback, no sync
  mutation, no particle spawn/render-output change. ✓
- 2-mission smoke PASS (gate OFF). ✓

## Verdict for the class-D particle decision

**Inconclusive under automation — measurement deferred to an interactive session.** Until a
real FX-heavy run shows a `(SPIKE…)` summary, do NOT implement a particle ring/barrier:
per CLASS-D-SYNC-RECON-1 the path is GL-safe; the only justification for a GL sync slice is
a measured stall, which the harness can't produce. Light class-D remains
DEFER-PENDING-NVIDIA.
