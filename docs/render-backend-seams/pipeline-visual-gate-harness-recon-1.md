# PIPELINE-VISUAL-GATE-HARNESS-RECON-1

**Type:** RECON / scope (no build). How to clear the 4 routed-but-proof-pending
passes (VFX, Water, TerrainOverlay, TerrainDecal) → VISUAL_PROVEN by **reusing**
existing deterministic-capture machinery, not building from scratch.

**VERDICT:** Water + VFX are **GO now** (machinery exists). TerrainOverlay is a
cheap content-defer (needs a cement-map bookmark, no code). TerrainDecal is the
only real gap (no deterministic decal-force fixture). Ship the harness as a thin
wrapper + clear passes incrementally.

Scratch: `.claude/VISUALGATE-HARNESS-RECON.md`.

## REUSE inventory (it's mostly already built)
- **Deterministic capture combo:** `MC2_SMOKE_MODE=1` + `MC2_SMOKE_FIXED_TIMESTEP=1`
  + `MC2_SMOKE_SEED=0xC0FFEE` + **sim-FREEZE at the trigger frame**
  (`mission.cpp:531` pauses water/FX/light/motion) → byte-stable frames.
  Honesty flag `SmokeMode::fixedTimestepEnabled()` stamped in the capture sidecar
  (`gos_visual_capture.cpp:645`).
- **Engine capture:** `gos_visual_capture.cpp` = `glReadPixels` → deterministic PNG + sha.
- **Runner:** `run_visual_capture.py` (bookmark/trigger/settle; `--runs N --warmup 1`
  → byte-stability across runs; `--candidate-set` materializes a golden).
- **★ Weapon-fire fixture ALREADY EXISTS** (the user's memory is right):
  `MC2_FX_FORCE_SPAWN` (`code/warrior.cpp:4884-4927`) forces 8 mechs to fire all
  weapons once, `dmgDone=0` (FX-only). Ready bookmark
  `tests/visual/bookmarks/mc2_01_werewolf.json` (`--trigger-frame 147 --settle 4`
  ≈ fire at frame 151); prior `run/ww_oracle_on` exercised it.
- **Coverage oracle:** `MC2_VFX_ORACLE_TUBE_COVERAGE` occlusion query
  (`gos_particle_bridge.cpp:699/779`) = "this pass actually rasterized" proof —
  generalize per pass as a fallback gate.
- **Perceptual compare:** `scripts/visual_compare.py` + `visual-tolerance-policy.json`
  (byte_exact default; per-family perceptual/SSIM allowlist).

## Per-pass force + capture recipe

| Pass | force in-frame | deterministic? | gate | verdict |
|---|---|---|---|---|
| **Water** | default-on (`renderWaterFastPath` real latch `terrain.cpp:2779`, called uncond `gamecam.cpp:530`); UV scroll frozen by sim-pause | YES (frozen) | byte-A/B | **GO** — only need a water-framed bookmark (stock mc2_01 camera minimizes water) |
| **VFX** | `MC2_FX_FORCE_SPAWN` + `mc2_01_werewolf.json` bookmark | YES under fixed-clock+seed+freeze (3-run stability proves it) | byte-A/B; oracle-coverage fallback | **GO** — fixture+bookmark exist |
| **TerrainOverlay** | cement-perimeter map (no stock tier1 map has it; TCE / mc2-overlay-mask-1 territory) | YES (static overlay) | byte-A/B | **DEFER (cheap)** — needs a cement-map bookmark, NO code |
| **TerrainDecal** | weapon impacts → craters, BUT `MC2_FX_FORCE_SPAWN` is `dmgDone=0` → no guaranteed crater | n/a | — | **DEFER** — needs a new `MC2_DECAL_FORCE_SPAWN` fixture OR an oracle-coverage-only gate |

## Harness shape (recommended)
A thin `scripts/pipeline_visual_gate.py` over `run_visual_capture.py`:
- per-pass profile = {env, mission, bookmark, trigger/settle, gate-type}.
- run `--runs 3 --warmup 1` → require byte-stability; capture BEFORE (stash/prior
  exe) vs AFTER; compare via `visual_compare.py` (byte_exact, else perceptual
  policy); plus the occlusion-coverage oracle as a "pass drew" guard.
- on pass: flip the ledger entry's `proofStatus` to the **landed** value
  (`byte_identical` / `perceptual_ab` / `oracle_coverage`) and status →
  VISUAL_PROVEN (the `check-pass-coverage.py` guard already enforces this).

## Concurrent-safety hazards (MUST fix in the harness)
`run_visual_capture.py` is NOT concurrent-safe:
- `_kill_existing_mc2()` (`:71`, called `:125`) `taskkill /F /IM mc2.exe` — kills
  ALL mc2.exe incl. foreign/other-session instances. **Add `--no-kill` (skip the
  taskkill) and/or PID-scope to only the launched child.**
- `park_cursor_center()` (`:169`) warps the mouse every poll. **Save/restore the
  cursor, or gate behind a flag.**
The harness must default to concurrent-safe (no foreign kill, cursor-restore);
only capture when no foreign mc2.exe — or after the safe-kill fix lands.

## Recommended order
1. **WATER-VISUAL-GATE-1** — add a water-framed mc2_01 bookmark, byte-A/B → Water
   VISUAL_PROVEN. (Easiest; deterministic; draws by default.)
2. **VFX-VISUAL-GATE-1** — reuse `MC2_FX_FORCE_SPAWN` + `mc2_01_werewolf.json`,
   3-run byte-stability (or perceptual + oracle) → VFX VISUAL_PROVEN.
3. **TERRAINOVERLAY-VISUAL-GATE-1** — find/author a cement-map bookmark.
4. **TERRAINDECAL** — needs a `MC2_DECAL_FORCE_SPAWN` fixture first (own small slice).
Build the `--no-kill`/cursor-safe wrapper as part of step 1.

## Correction
The Water ledger `scope_note` previously said "MC2_RENDER_WATER_FASTPATH=1,
default-OFF" — WRONG. `gameosmain.cpp:852` is only a diagnostic flag; the armed
fast path is the **default** render path (real latch `terrain.cpp:2779`). Fixed
in the ledger alongside this recon.
