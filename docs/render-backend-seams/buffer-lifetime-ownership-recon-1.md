# BUFFER-LIFETIME-OWNERSHIP-RECON-1

Read-only recon. MDI named the submitters; this catalogues the **lifetime** of the
buffers they depend on — who writes, when reset, how resized, whether a fence/ring or a
barrier guards CPU-writes-vs-GPU-reads, and the stale failure mode. Source-verified vs
nifty `fc579d84`. The decisive axis for Vulkan-prep is **how each buffer avoids a
CPU-overwrite-while-GPU-reads hazard**.

## Lifetime classes (the taxonomy)

| Class | How the hazard is avoided | Vulkan-ready? | Members |
|---|---|---|---|
| **A. Fence-guarded ring** | N-slot ring + per-slot `glFenceSync`/`glClientWaitSync` before reuse | ✅ explicit | mech bone/instance (3-slot, `gos_mech_batcher.cpp:1864`), static-prop coalesce (`s_coalesceFence`), static-prop legacy (`s_fence`), terrain thin-record (`g_thinRingFences`, 3-slot), water thin (`kThinRingSlots`=3 + fence), GPU-cull readback (`s_readbackFence`, 3-slot) |
| **B. GPU-produced + memory barrier** | `glMemoryBarrier(SSBO\|COMMAND)` between producer compute and consumer draw | ✅ explicit | terrain indirect cmd (`g_indirectCmdBuffer`, barrier @terrain_indirect:3558), gpu_cull indirect/bucketCounts/actorVis/blockVis (barrier @gpu_cull_compute:1297/1118/1342), water indirect cmd |
| **C. Static / immutable per-mission** | written once at mission load, read-only after | ✅ (no per-frame hazard) | terrain recipe (`g_recipeSSBO`), terrain height (`s_heightSsbo`), water recipe, per-type/per-draw/permutation/bucketCaps SSBOs, static-prop M2a instance+indirect (dirty-gated + `s_staticDrawFence`) |
| **D. Implicit-sync (no fence, no ring, no explicit barrier)** | relies on the GL driver's implicit serialization of `glBufferSubData` / coherent persistent map | ⚠️ **GL-safe, Vulkan HAZARD** | **light data SSBO** (`lightData_` binding 20, whole-buffer upload/frame), **particle SSBO** (`s_ssbo` binding 14), **tube ribbon SSBOs** (14/15/16), **terrain mask indirect** (`s_indirectCmdBuf`/`s_waterIndirectCmdBuf`, CPU glBufferSubData each frame) |
| **E. Sticky-bit temporal accumulator** | mission-load clear only; per-frame `atomicOr` accumulate (intentional) | ✅ (by design) | `s_blockVisBuf` (gpu_cull temporal-superset visibility) |

## The key finding: class D is the Vulkan seam

Class-A/B/C buffers are correctness-safe AND already express their sync explicitly
(fence or barrier) — they port to Vulkan cleanly. **Class D is correctness-safe ONLY
because GL implicitly serializes `glBufferSubData` and coherent-mapped writes** (the
driver stalls the CPU until the GPU has finished reading the buffer). That implicit
guarantee **does not exist in Vulkan** — each class-D buffer will need an explicit
barrier or double-buffer/ring before a Vulkan backend. They are not bugs today; they are
the documented migration debt.

Highest-attention class-D members:
- **`lightData_` (binding 20):** persistent buffer, full re-upload every frame
  (`txmmgr.cpp:2447`), no fence/ring, barrier UNKNOWN. Largest per-frame implicit-sync
  buffer touching every lit draw. Verify the implicit-sync assumption + whether a
  barrier is actually emitted (BONE/PARTICLE-style) — candidate for
  LIGHT-BUFFER-LIFETIME / barrier-verify.
- **particle/tube SSBOs (14/15/16):** `glBufferSubData` per flush, no fence. GL-safe
  (implicit sync) but each upload may stall; a ring would remove the stall AND make it
  Vulkan-ready.
- **terrain mask indirect (`s_indirectCmdBuf`/`s_waterIndirectCmdBuf`):** CPU-filled
  single-buffered indirect, no barrier (relies on glBufferSubData→draw ordering).

## Known telemetry failure mode (carried from MDI arc)

`gpu_drawn_instances` reads CPU-side `TypeRangeSsbo.instanceCount` (`@7011`) which is 0
under the live GPU-authority v6/C1b path (counts live in the GPU indirect buffer, never
read back). It is a **stale telemetry counter on the default path**, classified in
[objbatcher-zero-gpu-drawn-recon-1](objbatcher-zero-gpu-drawn-recon-1.md). Belongs here
as the canonical "readback-gated counter not wired" lifetime failure mode → candidate
STALE-COUNTER-RETIRE-1.

## Buffer inventory (condensed; full detail in the recon source threads)

Static-prop/cull (gos_static_prop_batcher.cpp + gpu_cull_compute.cpp): `s_instanceSsbo`/
`s_colorSsbo` (A), `s_coalesceInstanceSsbo` (A), `s_staticInstanceSsbo`+`s_staticIndirectCmdBuf`
(C, dirty-gated), `s_perType/perDraw/cmdToBucket/permutation/baseInstance/materialGpu` (C),
`s_indirectCmdBuf` (B), `s_visibleIds/bucketCounts/actorVis` (B), `s_bucketCaps` (C),
`s_blockVis` (E), `s_debug/staging/gpuSsbo/stagingBuf` (B + readback fence).
Terrain/water (gos_terrain_indirect.cpp + gos_terrain_water_stream.cpp): `g_recipeSSBO`/
`s_heightSsbo`/water recipe (C), `g_thinRecordSSBO`/water thin (A), `g_indirectCmdBuffer`/
water indirect (B), `g_solidQuadWindowSsbo` (B per-frame scratch), mask indirect (D).
Light/mech/particle: `lightData_` (D), mech `s_boneSsbo`/`s_instanceSsbo` (A),
particle `s_ssbo` + tube SSBOs (D).

## Recommended arc

1. **BUFFER-LIFETIME-TRACE-SCAFFOLD-1** — `[BUFFER_LIFE]` gated trace + a descriptor
   naming each buffer's class/producer/reset/barrier; `check-buffer-lifetime-ownership.py`
   (every class-D buffer is declared implicit-sync / Vulkan-debt; every GPU-produced
   indirect buffer declares a COMMAND barrier; stale counters declared). No behavior change.
2. **STALE-COUNTER-RETIRE-1** — make `gpu_drawn_instances` honest (retire for
   `submitted_instances` or fence-gated readback).
3. **LIGHT-BUFFER-BARRIER-VERIFY-1** / **PARTICLE-BUFFER-LIFETIME-1** — confirm/insert the
   class-D barriers (the freshest, highest-touch risk).

## Do NOT (per TD)
Rewrite rings, centralize glBufferSubData, add readbacks by default, change barriers,
change allocation, retire gpu_drawn_instances in this recon, start Vulkan. Catalogue first.
