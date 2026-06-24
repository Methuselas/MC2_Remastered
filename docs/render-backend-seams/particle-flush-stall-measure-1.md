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

## Measurement finding (important)

The probe **cannot be exercised by the automated deterministic capture harness.** On
mc2_10 (the gosFX mission) under `run_visual_capture` — even with `MC2_FX_FORCE_SPAWN=1`
firing 8 weapon events — the instrumented flush paths emitted **nothing** (≤10 calls,
below the one-shot threshold), and no particle/tube activity markers appeared. The
fixed-timestep + frozen-camera capture fires weapon *events* but does not tick the gosFX
simulation enough to drive sustained particle/tube flushes.

→ **The stall measurement must be taken in an interactive / soak session** (real-time
gameplay with sustained FX — explosions, PPC fire, smoke), launched with
`MC2_PARTICLE_FLUSH_STALL_TRACE=1` and `MC2_LOG=1`, then read the `[PARTICLE_FLUSH_STALL]`
summaries from the log. That is a user-driven run, not an automated-capture step.

This is itself a useful result: the particle/tube flush path is **not hot under the
deterministic harness**, so any future class-D particle work must be validated against an
interactive FX-heavy session, not tier1 capture.

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
