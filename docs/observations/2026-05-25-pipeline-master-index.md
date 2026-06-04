# MC2 OpenGL — Render Pipeline Master Index

> Branch: `claude/nifty-mendeleev` · Mapped: 2026-05-25 · drawPackets/MDI: **not enabled**
>
> Seven subsystem maps, each with a three-zone overview (`[game data]` ↔ `[API]` ↔ `[engine]`),
> full Mermaid flowchart, ASCII fallback, call sites, env gates, and known limits.

---

## Frame spine (gamecam.cpp ~L215)

```
camera->update()
  │
  ├─ land->render()                 ← TERRAIN (solid quads)
  ├─ craterManager->render()        ← CRATERS (decal batch)
  ├─ ObjectManager->render()        ← MECHS + STATIC PROPS (GPU batcher / MLR fallback)
  ├─ land->renderWater()            ← WATER CPU path (runs before flush)
  ├─ mcTextureManager->renderLists()← FLUSH POINT (masterVertexNodes + masterHardwareVertexNodes)
  │     └─ shadow pre-pass          ← SHADOW (depth-only, before flush)
  │     └─ post-process composite   ← POST-PROCESS (after flush)
  ├─ land->renderWaterFastPath()    ← WATER GPU path (runs after flush, armed only)
  ├─ Batcher::Flush()               ← FX particles (GPU path, MC2_GPU_PARTICLES=1)
  └─ HUD batch replay               ← HUD (after post-process shadow darkening)
```

---

## Subsystem map

| Subsystem | Map | API path | GPU gate | Notes |
|---|---|---|---|---|
| **Terrain** | [terrain-pipeline-map.md](2026-05-25-terrain-pipeline-map.md) | `gos_VERTEX` → `masterVertexNodes` (CPU) / `TerrainPatchStream` SSBO (GPU) | `MC2_TERRAIN_INDIRECT=1` (ON) | Dual-path runs in parallel on armed frames; tessellation SSBO binding=23 (AMD TES workaround) |
| **Water** | [water-pipeline-map.md](2026-05-25-water-pipeline-map.md) | `gos_VERTEX` (CPU) / `WaterStream` SSBO (GPU) | `MC2_GPU_DRIVEN_WATER` (OFF) | 5-condition AND gate; CPU path default; z-fight is architectural |
| **Craters** | [crater-pipeline-map.md](2026-05-25-crater-pipeline-map.md) | `gos_PushDecal` → `decalBatch_` → `gos_DrawDecals` | none (CPU-only) | Separate from `masterVertexNodes`; same infra as cement overlay; cyclic buffer, no save/load |
| **Mechs** | [mech-pipeline-map.md](2026-05-25-mech-pipeline-map.md) | `GpuMechBatcher` → `glDrawElementsInstanced` / MLR fallback (immediate) | `MC2_GPU_MECHS=1` (ON) | MLR is immediate-draw exception; bone SSBO re-uploaded every frame; RenderWorld M2/M2.5/M2.6 shipped |
| **Static props** | [staticprops-pipeline-map.md](2026-05-25-staticprops-pipeline-map.md) | `GpuStaticPropBatcher` → GPU cull substrate → `glDrawElementsInstanced` | `MC2_GPU_OBJECTS=1` | `IsStaticNow()` first-frame vs steady-state split; per-actor light index snapshot load-bearing |
| **Shadow + post-process** | [shadow-postprocess-pipeline-map.md](2026-05-25-shadow-postprocess-pipeline-map.md) | FBO bind/unbind via `gos_postprocess` API | `MC2_SHADOW` | 4096² shadow map; gradient-adaptive Poisson PCF (0.8×–3.2×); reverse-Z via `glClearDepth()` swap |
| **FX / particles** | [../2026-05-25-fx-pipeline-map.md](2026-05-25-fx-pipeline-map.md) | `Batcher::Emit` → SSBO → `gos_particle_bridge` | `MC2_GPU_PARTICLES=1` (OFF) | No particle persistence; Flush() clears every frame; B2 trail ring-buffer workaround |
| **HUD** | [hud-pipeline-map.md](2026-05-25-hud-pipeline-map.md) | `gos_DrawQuads` buffered → replayed after post-process | `gos_State_IsHUD=1` | Screen-space vertices (no world-to-clip); `GBuffer1.a=1.0` shadowHandled flag exempts from shadow darkening |

---

## API submission taxonomy

```
masterVertexNodes[]      — legacy gos_VERTEX ring (terrain solid, water CPU)
masterHardwareVertexNodes[] — TG_RenderShape queue (static props, flushed by renderLists)
decalBatch_              — WorldOverlayVert ring (craters, cement overlay)
GpuMechBatcher           — per-actor SSBO + bone SSBO → glDrawElementsInstanced
GpuStaticPropBatcher     — instance SSBO → GPU cull → glDrawElementsInstanced
Batcher (particles)      — staging vec → SSBO → gos_particle_bridge
gos_DrawQuads (HUD)      — screen-space buffered replay, post-shadow
gos_postprocess          — FBO orchestration (shadow map + bloom + composite)
```

---

## Known limits that cut across pipelines

| Limit | Affects |
|---|---|
| `renderLists()` flush is the single choke point — everything enqueued before it stalls here | terrain, water CPU, static props |
| drawPackets / MDI not enabled — all instanced draws are `glDrawElementsInstanced` per bucket | mechs, static props |
| Dual CPU+GPU emit on armed frames — parity slippage risk | terrain |
| No particle persistence — Batcher::Flush() clears staging every frame | FX |
| `IsStaticNow()` first-frame re-trigger on LOD/visibility change | static props |
| Water z-fight is architectural (coplanar depth bias can't close the gap) | water |
| Craters: CPU-only, no GPU cull, cyclic overwrite | craters |
| HUD depth 0.9999f is load-bearing — any change risks z-fighting with 3D scene | HUD |
