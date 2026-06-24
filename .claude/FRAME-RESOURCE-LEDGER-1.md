# FRAME-RESOURCE-LEDGER-1

Ledger of mid-frame render resources that VFX / transparent passes can sample
without a read-from-bound-attachment GL feedback loop. "Sample-safe" = a pass may
read the resource while the live render target is still bound, because the
resource is a *copy* taken at a known frame point (not the active attachment).

| Resource | Status | File:line | Gate | Copy cost | Sample-safe |
|---|---|---|---|---|---|
| `sceneDepthCopyTex_` (DEPTH24_STENCIL8, full-res) | EXISTS | `GameOS/gameos/gos_postprocess.cpp` `copySceneDepthForParticles()` | `MC2_VFX_SOFT_PARTICLES` | 1× `glCopyImageSubData` | YES |
| `sceneColorCopyTex_` (RGBA16F, full-res) | EXISTS | `GameOS/gameos/gos_postprocess.cpp` `copySceneColorForVfx()` | `MC2_VFX_SCENECOLOR_GRAB` (default OFF) | 1× `glCopyImageSubData` | YES |

## Notes

- `sceneColorCopyTex_` — added by **VFX-SCENECOLOR-GRAB-1**
  (`docs/VFX-SCENECOLOR-GRAB-1.md`). FRAME_RESOURCE_SUBSTRATE: produces the
  resource only. **No consumer yet** — distortion / refraction / soft-color
  particles are future slices. Gate OFF = byte-identical (not allocated, not
  written). Gate ON = the copy happens but nothing samples it -> still no visual
  change. Copy is taken in the in-scene VFX flush after opaque scene color is
  resolved, before VFX draws, so the snapshot is the pre-VFX scene. Mirrors
  `copySceneDepthForParticles()` exactly (lazy alloc, full-res, freed +
  re-created on resize).
- `sceneColorTex_` itself is the LIVE render target (`sceneFBO_`
  `COLOR_ATTACHMENT0`) — sampling it while bound is a feedback loop; that is why
  the copy is required.
