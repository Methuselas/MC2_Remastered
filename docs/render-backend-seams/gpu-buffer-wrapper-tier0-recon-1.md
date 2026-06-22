# GPU-BUFFER-WRAPPER-TIER0-RECON-1 — data-driven first wrapper target

**Arc:** VULKAN-CONTRACT-MANIFEST-ARC option B · recon only, no code · 2026-06-22
**Source data:** GPU-UPDATE-BUFFER-COUNTER-1 (`MC2_GPUBUF_COUNTER=1`), slice-4 build `eab7924d`, deployed v0.4c, tier1 mission mc2_01 steady state.

## Measured orphan-on-write traffic (per frame, steady state)

```
[GPUBUF v1] orphan_calls=72  orphan_bytes=1,927,788 (~1.84 MB/frame)  by_owner=
  light:            1 call  / 1,855,008 B   ← 96% of bytes
  hud:             66 calls /    31,500 B   ← 92% of calls
  sp_shadow:        4 calls /    41,216 B
  gos_UpdateBuffer: 1 call  /        64 B   ← negligible
```

Numbers are stable frame-to-frame in steady state (identical across frames 4194–4199). Postprocess quad does NOT appear → it is a static (init-once) buffer, confirming the earlier recon that it is a poor wrapper target.

## What the data says (overturns the priors)

- **Light SSBO is the prize by BYTES**: it re-specs the *entire* light buffer via `glBufferData` every frame — 1.85 MB/frame, 96% of all orphan bytes. A persistent-mapped/ring (or simply grow-once + `glBufferSubData`) buffer eliminates ~1.85 MB/frame of orphan churn. This is by far the highest-impact target by volume.
- **HUD (gosMesh) is the prize by CALL COUNT**: 66 small orphan `glBufferData` calls/frame (the private 5-arg `updateBuffer` per draw-batch), but only 31 KB total. Impact is call/driver overhead, not bandwidth.
- **gos_UpdateBuffer is confirmed near-dead** (1 call, 64 B) — do not prioritize it (matches the buffer-owner recon).
- **sp_shadow** is minor (4 calls, 41 KB) and transient.

## Recommendation (the advisor's "prove hottest/cleanest before adopting")

Two defensible Tier-0 pilots; the data splits them:

| Target | Impact | Risk | Verdict |
|---|---|---|---|
| **HUD gosMesh** (`gl_utils.cpp` 5-arg `updateBuffer`) | low bytes, high call count | LOWEST — self-contained, no fence/grow logic, no foreign-WIP files | **Pilot first** — proves `GpuRingBuffer<N>` adoption (`gpu-buffer-wrapper-design-1.md` slice B) safely |
| **Light SSBO** (`gameos_graphics.cpp` `s_lightDataSsbo`) | **highest bytes (1.85 MB/fr)** | MEDIUM — grow-realloc (size tracks light count); ring must handle growth | **Adopt second**, once the pattern is proven — biggest single bandwidth win |

**Do NOT start wrapper adoption on the light SSBO** despite its byte dominance: it is a grow-realloc whose size varies with light count, so it needs the ring's growth path validated first. Prove the wrapper on HUD (mechanical, bounded), then port the proven `GpuRingBuffer` to the light SSBO for the real bandwidth win. Mech/static-prop/terrain already have fenced rings (do not touch — and they live in foreign-WIP-dirty files).

## Next slice shape (when adoption is approved)
`GPU-BUFFER-WRAPPER-TIER0-HUD-1` — replace the HUD `updateBuffer` orphan with the designed `GpuRingBuffer<N>` (3-frame persistent-coherent), verify `[GPUBUF v1]` `hud` call count drops to ~0, tier1 byte-identical visual. Then `…-LIGHT-2` for the 1.85 MB/frame win. Cross-ref `gpu-buffer-owner-recon-1.md` (ring template `gos_mech_batcher.cpp`) and `gpu-buffer-wrapper-design-1.md` (§4 adoption slices; §3 flat-enum is SUPERSEDED).

## Caveat
Captured mc2_01 only (representative steady state); busier missions (mc2_24) will scale `hud`/`sp_shadow` with on-screen geometry but the `light` per-frame full-respec is structural and present everywhere. Re-run `MC2_GPUBUF_COUNTER=1` on mc2_24 before sizing the light-SSBO win precisely.
