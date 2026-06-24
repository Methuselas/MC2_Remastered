# CLASS-D-SYNC-RECON-1

Read-only deep-dive of the three class-D (implicit-GL-sync) buffer groups from
[buffer-lifetime-ownership-recon-1](buffer-lifetime-ownership-recon-1.md), for Vulkan
migration planning. **No code changes.** Source-verified vs nifty `69a738b5`.

## Verdict up front

All three groups are **correctness-safe under OpenGL** (the driver implicitly serializes
`glBufferSubData` / coherent-map writes against in-flight GPU reads). They are **Vulkan
migration debt, not GL bugs.** Inserting explicit GL barriers/fences now would be churn or
risk (the advisor's "don't do class-D blindly") — none is needed for GL correctness, and
the one with a real *perf* win (light) is gated on NVIDIA hardware.

## Group findings

| Group | Producer | Reuse | Size / freq | Cross-frame hazard | Vulkan equivalent | Observed stall |
|---|---|---|---|---|---|---|
| **Light `lightData_` (b20)** | orphan `glBufferData(nullptr)`+`glBufferSubData` (`gameos_graphics.cpp:8879/8935`) | 1 write/frame, read by ALL lit draws all frame | ~1.85 MB/frame, **unbounded grow** | **HIGH** (prior-frame GPU read) | per-frame ring OR grow-once+fence | **~80 ms NVIDIA 1050 Ti** (the documented reason orphan exists) |
| **Particle/tube (b14/15/16)** | in-place `glBufferSubData` (`gos_particle_bridge.cpp:1183/1242`, tube 560-571) | N writes/frame (per group/ribbon), self-contained bind→draw→unbind | 64–256 KB per flush, per-VFX | LOWER (self-contained) | staging+copy (easy) | UNKNOWN (no instrumentation) |
| **Terrain-mask indirect** | in-place `glBufferSubData` 16B (`gameos_graphics.cpp:4497/4658`) | 1 write+draw/frame, self-contained | **16 B fixed**, no grow | NONE | push-constant / inline (trivial) | NONE |

## Why light is NOT the clean first target (correcting the advisor prior)

The advisor suggested lights first "if recon shows a clean path: no GPU producer, no
append/counter weirdness, easy trace." Recon shows the opposite:
- **Unbounded monotonic grow** (`txmmgr.cpp:1692`) → a ring needs ensureCapacity
  drain+remap (the hardest part).
- **Cross-phase whole-frame binding** (slot 20 read by legacy + GPU mech + GPU static-prop
  in different phases, `txmmgr.cpp:485`) → fence must land after the *last lit consumer*,
  not after upload — non-obvious.
- **Fragile NVIDIA-specific opt:** LIGHTSSBO-ORPHAN-1 exists specifically to dodge an ~80 ms
  NVIDIA stall; replacing it is NVIDIA-hardware-gated and only AMD has been tested here.
- Prior art already exists: `docs/.../gpu-buffer-wrapper-tier0-light-1-recon.md` rates the
  full ring **DEFER-PENDING-NVIDIA** and floats a gated grow-once+fence intermediate.

→ Light is a **perf** arc blocked on NVIDIA hardware, not a clean correctness slice.

## Recommended order (when class-D work is unblocked)

1. **Terrain-mask** — mechanically trivial (16 B, no hazard); but **zero GL benefit** —
   purely a Vulkan-time push-constant conversion. Don't touch in GL.
2. **Particles** — staging-buffer shape is the safest *Vulkan* port; in GL, only worth a
   ring if a stall is *measured* (currently UNKNOWN — add a Tracy zone first).
3. **Light** — highest value (1.85 MB/frame) but NVIDIA-gated; pursue the existing
   gpu-buffer-wrapper light recon's grow-once+fence behind a default-OFF gate **only on
   NVIDIA hardware**.

## First *safe* action available now (if any)

None that changes GL behavior. The defensible next step is **measurement, not mutation**:
add a gated Tracy zone / `[BUFFER_LIFE]` trace to the particle flush to learn whether the
implicit-sync `glBufferSubData` actually stalls (the one UNKNOWN). That unblocks a
particle decision without touching sync. Light needs NVIDIA hardware; terrain-mask needs
nothing until Vulkan.

## Proof fixtures (for whoever implements)
- Light: mc2_24 (most baked lights) + mc2_10 (destruction grow); `MC2_GPUBUF_COUNTER=1`
  light orphan bytes → ~0; Tracy `RenderLists.LightDataUpload` no NVIDIA regression.
- Particle: mc2_17 / mc2_10 (VFX); byte-identical explosions; add flush Tracy zone.
- Terrain-mask: mc2_01 byte-identical terrain.
