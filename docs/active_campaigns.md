# Active campaigns

> Pointer doc. Detail in memory/ handoffs and docs/. Add new campaigns at top.

## Asset Modernization Pipeline — v0 SHIPPED (merged nifty `fdb7c470`, 2026-06-17)

| Slice | Status | Notes |
|-------|--------|-------|
| P1-A→D: ASE → GLB + sidecar roundtrip tooling | SHIPPED | `tools/ase_to_glb.py`, `tools/validate_glb.py`, `tools/asset_baseline.py`, `.mcasset.json` sidecar spec |
| Phase 2: texture cook (BC7 KTX2, AI-upscaled sources) | SHIPPED | `tools/mc2texcook/cook_pbr_maps.py`; `fireantrgb.ktx2` + `a_hangar.ktx2` deployed rc1 |
| Phase 3: mech PBR (StandardLit GGX) | ALREADY LIVE | Metal061B+PaintedMetal003 active in mech3d pipeline |
| P1-E: HangarGLB building GLB load | SHIPPED | Positive load confirmed mc2_01 via ASSIMP_TRACE; `data/tgl/HangarGLB.glb` + sidecar in rc1 |
| P1-F: material classification + CorrugatedSteel006A cook | SHIPPED | `tools/classify_materials.py`; BC7 KTX2 pack in `gameassets/materials/CorrugatedSteel006A/` |
| Runtime `MC2_ASSIMP_IMPORT=0` kill switch | SHIPPED | `mclib/msl.cpp LoadFromFile`; bdactor/genactor/gvactor all probe `[Import] Source=` |

**In flight / next:**

- **P1-G: building MaterialGpu SSBO wiring + `building.frag` PBR** — branch `claude/building-materialgpu-v1`
  - Status: NOT STARTED — foundation merged, new branch required
  - Kill switch: `MC2_BUILDING_PBR=0` (default OFF)
  - Blocked on: nothing — unblocked after v0 merge
  - First step: wire `CorrugatedSteel006A` BC7 KTX2 into MaterialGpu SSBO per building instance

---

## 2026-06-16 — NVIDIA hardening S2 — SHIPPED + merged to nifty (`97be4e5c`)

7 hardening items targeting correctness gaps AMD silently tolerates but NVIDIA exposes:

| Item | Result | Commit |
|------|--------|--------|
| 1. `gpuBindSsboRange` — 24 SSBO bind sites + alignment assert | DONE | `61b3684b` |
| 2. GPU buffer zero-init audit | CLEAN | `bb550ed4` |
| 3. `assertPassContract` — depth-func + blend aborts under `MC2_RENDER_CONTRACT_ASSERT=1` | DONE | `50ee010a` |
| 4. `[SHADER WARN]` on successful compile/link | DONE | `594eafe3` |
| 5. PBO unbind audit | CLEAN — `GlPixelStoreGuard` already correct | — |
| 6. Pass-entry state audit | CLEAN — mech via `applyPipeline`, decal+post explicit | — |
| 7. `barrierBitsFor` table extended + cardcloud migrated | DONE | `90b6aa5a` |
| 8. NVIDIA validation smoke checklist | PENDING — requires NVIDIA HW | — |

Invariants now in `docs/critical_inline_rules.md` (GPU resource invariants section).
Full plan: `docs/nvidia-hardening-s2.md`.
Tier1 5/5 PASS. Item 8 fires when NVIDIA hardware available.

## 2026-06-14 — Proof-machine S-program + GlStateGuard + RC reconcile + FX/pixel gate (nifty `1f0419ac`)

**SHIPPED (S11-S20 proof machine):**
- S12 unified manifest schema `ee4a583f` (`scripts/manifest_schema.py` + checker + tests + cockpit adoption)
- S9/S13 pixel gate `8461e3e0` (`scripts/run_visual.py` capture/compare/verify, byte-hash, refuses non-blessed) + **Baseline-A v2 `7e997e8f` (all 5 tier1, 15 golden frames, coherent off `1f0419ac`)**
- S11/S15 `c9421306` (release-install report w/ S12 identity; residency slim report sha+keep/drop/fail/unknown)
- S14 render-pass reader `0017bb2b` (`scripts/render_pass_report.py` + --compare)
- S20 GlStateGuard slice 2 `783406a8` (RAII depth/blend/cull on terrain chunk draw, A/B pixel-neutral)
- S19 FX fixture `1f0419ac` (`MC2_FX_FORCE_SPAWN` mech-fire PPC tubes in MechWarrior::updateActions)
- RC→nifty reconcile `d69611f1`/`7d087f16`/`77beb69c`/`4855fcf9`/`745a8a5b` (5 fixes incl. dark-water uninit-VRAM); merged engine set + importer `d821018c`/`eca37ed3`
- MC2_LOG hitch gate `47ef382f` (stdout→NUL default kills the 400ms printf hitch; harness sets MC2_LOG=1)

**LEFT:** S16 modder round-trip (`verify_mod_roundtrip.py`), S17 editor authoring depth, S18 asset-browser thumbnails, S13+ HTML gallery + advisory smoke-gate auto-wire, S15+ cook.json provenance + texture-res validator, #5 Tube/render merge (unblocked). Full: MEMORY.md 2026-06-14 handoff.

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
- **DrawPacket v8 — live-builder retired** (merged `96c27c2a`, default-ON): snapshot is sole static-prop draw-packet owner; flush() skips per-flush live build + compare. Kill-switch MC2_STATIC_PROP_LIVE_BUILDER=1. Commit-1 (retire); follow-ups: live-builder DELETE after soak (STATIC-PROP-LIVE-BUILDER-DELETE) + the Extract.SP.Fill dirty-list — **shipped as STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1, see below.** Spec/plan in docs/superpowers/{specs,plans}/2026-06-02-static-prop-snapshot-finish*.
- **Snapshot static-prop Fill dirty-only — SHIPPED + merged `cf654080` (2026-06-04), default-ON** (`STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1`, branch `8a209710..60cb4cf8`): `ExtractRenderSnapshot` skips `Extract.SP.Fill`+`WriteLoop` on clean (regGen+cullVer unchanged) frames and memcpy's cached rows into the snapshot arena → **median 1.68ms→36.7µs (−97%, user Tracy)**. Gate `MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY` (=0 kill-switch). Review chain complete; a material-cache staleness hole (`staticPropCacheTypePrimaryMaterial` bumped no generation, compare-oracle blind) caught + fixed `60cb4cf8`. **Contract: any registry mutation of a snapshot-captured field MUST bump `s_registryGeneration`.** Companion compare gate `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE`. Greybeard meta-fix debt (registry-owned incremental flat-row store = real end-state) logged in `memory/staticprop_per_frame_cpu_retire_campaign.md`.
- **GOM.readbackSnapshot eliminated** (SHADOW-COPY-1 `99519cbc`/`00bfc28a`): GPU-cull vis bits shadow-copied to system RAM; killed the per-frame BAR-read stall (360ns now). Kill-switch MC2_GPU_CULL_READBACK=0.
- **TRACKV CPU quick wins** (merged `26974734`): Probe-6 thin-canary readback gated MC2_THIN_CANARY default-OFF; mech MaterialGpu table persistent + dirty-gated upload; ShapeRenderer material-handle cache. renderLists self-time carve ABANDONED (audit rank-2 stale; the 985µs is static-prop registry/light/shadow = other-session lighting-ownership domain, per-instance light slots U=K). Audit: `docs/trackv-whole-frame-cpu-optimization-audit.md`.
- **HZB substrate** (merged 5864882c): depth pyramid built, no culling yet. Next: HZB-STATICPROP-CULL-CONSUMER-0 (default-OFF, 1e-4 margin, discontinuity guard).
- **Water reflection** (merged 68343329): SH-L2 sky, default-OFF. MC2_WATER_REFLECTION=1.
- **C++17** (CMAKE_CXX_STANDARD 17 in root CMakeLists).

## Hitch stability track (H-series)

H-series is recon/diagnostic only — no behavior changes. Must complete before Phase 8z deletion.

| Slice | Status | Detail |
|---|---|---|
| H1a — GL/resource attribution | **SHIPPED** `30dfd015` | `[HITCH]` / `[HITCH_GL]` / `[HITCH_TERRAIN_TEX]` / `[HITCH_STATIC_FLUSH]` / `[HITCH_WATER]` |
| H1b — WaterFastPath CPU sub-scopes | **SHIPPED** (2026-06-09) | `[HITCH_WATER_DETAIL]` guards/recipe/buildWindow/upload/dispatch sub-times |
| H1c — Broad frame-phase attribution | **QUEUED** | `[HITCH_PHASE]` logic/render/present/sleep/unknown; explains Category 5 unattributed hitches |
| H2 — Fast-path disruption / setupTextures guard | **RECON** | See `docs/superpowers/specs/2026-06-09-h2-fastpath-disruption-recon.md` |

**H2 priority:** run before Phase 8z (legacy terrain deletion). Reason: 8z deletes `setupTextures`/makeLists; H2 must confirm no runtime path still depends on them being resurrected, especially water and editor fallback.

## Unstarted campaigns (queued)

- **Terrain continuous surface** (all forks ruled 2026-05-18, design complete): see `terrain_continuous_surface_forks_ruled_option1_killlegacy.md`
- **DrawPass retirement Slice B**: conjunction gate implemented but not shipped (`terrain.cpp`)
- **Unified-projection F1**: plan at `docs/superpowers/plans/2026-05-22-unified-projection-v2-f1-atomic-plan.md`
- **VFX GPU sim Cardcloud parity**: `docs/vfx-gpu-sim-spec.md`
- **HZB static-prop cull consumer**: first draw-affecting slice, spec needed

## RenderWorld arc

Steady-state ledger: `docs/renderworld_arc_status.md`. M1-M2 shipped; M3-M5 = DECISIONS (not ongoing work). Migration guide: `docs/renderworld_migration_guide.md`.
