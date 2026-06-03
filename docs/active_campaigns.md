# Active campaigns

> Pointer doc. Detail in memory/ handoffs and docs/. Add new campaigns at top.

## Current state (2026-06-01)

**Nifty HEAD:** see MEMORY.md handoffs for current commit.

**In-flight / recently shipped:**
- Center-screen flicker + cinematic water (9d55bcbc, 47198695) — SHIPPED. Mech/water flicker (SimpleCamera cull MVP + mech readback conservative-OR); cinematic GPU water (SimpleCamera renderWaterFastPath + GL_CULL_FACE management). See 2026-06-01 handoff.
- TERRAIN-CLASSIFY-TUNING-1 + Mech ImGui section (3d5457d0) — see handoff
- PERF-GPU-CULL-READBACK-ID-CACHE-1 (ef03162a) — merge to nifty pending
- VFX-WEAPON-FX-RESTORE-OPUS-1 (9dfff4a9) — shipped
- Colormap BC7 KTX2 arc (7768d4e5) — shipped; .burnin soak + dead helpers pending
- perf session: setupTextures gate + water intro + recalcBounds (d65552ab)

**Outstanding work items:**
- WATER-REFLECTION-CLIP-1: terrain-RT empty at 20° camera (reflected projection + Lengyel oblique near-plane)
- VFX GPU sim: PARITY-ID-1 (per-particle IDs + forces)
- Shadow cherry-pick to nifty (v0.4 crashes shadow-on at batcher.cpp:4778)
- Colormap: delete dead helpers in mapdata.cpp + .burnin files after soak

## Infrastructure / permanent decisions

- **Track V post+grounding** (merged d8ccd032): MC2_HDR_POST/BLOOM/TONEMAP_ACES/SSAO all default-OFF. Soak doc: `docs/trackv-post-grounding-soak-1.md`.
- **Shadow lane** (merged 69522900; default-ON since c0525b27): dynamic shadows + 733 prop casters working. Kill-switch MC2_SHADOW_ENABLE=0.
- **ViewUniforms UBO** (default-ON): binding=3, kill-switch MC2_VIEW_UNIFORMS=0.
- **MaterialGpu** (default-ON): MC2_MATERIAL_GPU / MC2_MATERIAL_GPU_SAMPLE.
- **DrawPacket v7** (default-ON): kill-switch MC2_STATIC_PROP_LEGACY_DISPATCH=1.
- **DrawPacket v8 — live-builder retired** (merged `96c27c2a`, default-ON): snapshot is sole static-prop draw-packet owner; flush() skips per-flush live build + compare. Kill-switch MC2_STATIC_PROP_LIVE_BUILDER=1. Commit-1 (retire); follow-ups: live-builder DELETE after soak (STATIC-PROP-LIVE-BUILDER-DELETE) + the Extract.SP.Fill dirty-list (PERF-EXTRACT-SNAPSHOT-FILL-DIRTYLIST-1, owned by static-prop/lighting sessions). Spec/plan in docs/superpowers/{specs,plans}/2026-06-02-static-prop-snapshot-finish*.
- **GOM.readbackSnapshot eliminated** (SHADOW-COPY-1 `99519cbc`/`00bfc28a`): GPU-cull vis bits shadow-copied to system RAM; killed the per-frame BAR-read stall (360ns now). Kill-switch MC2_GPU_CULL_READBACK=0.
- **TRACKV CPU quick wins** (merged `26974734`): Probe-6 thin-canary readback gated MC2_THIN_CANARY default-OFF; mech MaterialGpu table persistent + dirty-gated upload; ShapeRenderer material-handle cache. renderLists self-time carve ABANDONED (audit rank-2 stale; the 985µs is static-prop registry/light/shadow = other-session lighting-ownership domain, per-instance light slots U=K). Audit: `docs/trackv-whole-frame-cpu-optimization-audit.md`.
- **HZB substrate** (merged 5864882c): depth pyramid built, no culling yet. Next: HZB-STATICPROP-CULL-CONSUMER-0 (default-OFF, 1e-4 margin, discontinuity guard).
- **Water reflection** (merged 68343329): SH-L2 sky, default-OFF. MC2_WATER_REFLECTION=1.
- **C++17** (CMAKE_CXX_STANDARD 17 in root CMakeLists).

## Unstarted campaigns (queued)

- **Terrain continuous surface** (all forks ruled 2026-05-18, design complete): see `terrain_continuous_surface_forks_ruled_option1_killlegacy.md`
- **DrawPass retirement Slice B**: conjunction gate implemented but not shipped (`terrain.cpp`)
- **Unified-projection F1**: plan at `docs/superpowers/plans/2026-05-22-unified-projection-v2-f1-atomic-plan.md`
- **VFX GPU sim Cardcloud parity**: `docs/vfx-gpu-sim-spec.md`
- **HZB static-prop cull consumer**: first draw-affecting slice, spec needed

## RenderWorld arc

Steady-state ledger: `docs/renderworld_arc_status.md`. M1-M2 shipped; M3-M5 = DECISIONS (not ongoing work). Migration guide: `docs/renderworld_migration_guide.md`.
