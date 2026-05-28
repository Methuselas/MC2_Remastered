# Water R→V Arc Audit (WATER-RV-ARC-AUDIT-0)

Closure audit for the water R→V lane after Batch 1 (recon / debug / baseline)
and Batch 2 (tuning UI / lighting plan / first visual). Pass-level analog to
[`static-prop-rv-arc-audit.md`](static-prop-rv-arc-audit.md) /
[`terrain-rv-arc-audit.md`](terrain-rv-arc-audit.md). **Docs/validation slice —
no feature work.**

Lane: `claude/water-rv-lane` (off `claude/nifty-mendeleev`). Audit HEAD
`9501bede`. Pairs with [`water-rv-arc-recon.md`](water-rv-arc-recon.md) and
[`water-lighting-plan.md`](water-lighting-plan.md).

## Verdict: 🟢🟡 GREEN / YELLOW-GREEN

The **MDI water visual lane is coherent, debuggable, tunable, and good enough
for now.** The user visually confirmed water looks good (2026-05-28). The first
visual feature (sky tint) is shipped but **gated default-OFF** and not currently
needed. Reflection / fresnel / refraction are **intentionally deferred** under
the 2026-05-17 camera-independence ruling. No must-fix items.

Why YELLOW (not full GREEN): two Track-R substrate gaps remain (water still on
legacy `u_worldToClipGL`, no RenderResourceRegistry entries) and one
environment caveat (capture foreground-race blocks automated pixel proof).
Neither blocks "good enough for now"; both are non-visual hygiene.

## Lane ledger (6 commits, all shipped)

| Slice | Commit | Result |
|---|---|---|
| WATER-ARC-RECON-0 | `3e807afa` | pipeline mapped end-to-end |
| WATER-DEBUG-VIEWS-1 | `ffb0f33b` | MDI FS `u_waterDebugMode` (6 real modes) |
| WATER-BASELINE-0 | `906524b0` | water presets + `--water-debug-mode` capture tooling |
| WATER-TUNING-UI-1 | `a096fcab` | 6 consts → uniforms + Graphics Options > Water subpanel |
| WATER-LIGHTING-PLAN-0 | `85f59bed` | first-visual plan |
| WATER-VISUAL-FIRST-SLICE | `9501bede` | gated camera-independent sky tint (`MC2_WATER_SKYTINT`) |

## 1. Authority chain (unchanged from recon — preserved)

| Data | Owner | Status |
|---|---|---|
| Water plane height `waterElevation` (`= wDepth + sDepth`, single global) | [mapdata.cpp:611](../mclib/mapdata.cpp); [terrain.cpp:138](../mclib/terrain.cpp) | **untouched** — gameplay ground truth preserved |
| Per-vertex `PostcompVertex::water` byte (bit0 underwater, bits6/7 wave-bob) | [mapdata.cpp:588-595](../mclib/mapdata.cpp) | **untouched** |
| MDI path (geometry + material) | recipe SSBO 5 / thin 6 / percmd 7; [gos_terrain_water_mdi.frag](../shaders/gos_terrain_water_mdi.frag) | **active V-lane target** — all visual work here |
| Legacy path (`gos_tex_vertex.frag` sin-wave) | [gameos_graphics.cpp](../GameOS/gameos/gameos_graphics.cpp) | **untouched** — fallback, ignores all new uniforms |
| Shoreline post-process (`shoreline.frag`) | [gos_postprocess.cpp:773-799](../GameOS/gameos/gos_postprocess.cpp) | **untouched** — screen-space foam, default-on |

No authority changed this arc. All water visual work is render-side on the MDI
FS; no gameplay/height/flag/physics path was modified.

## 2. Feature / debug matrix

| Control | Default | Path req | Effect |
|---|---|---|---|
| `MC2_WATER_DEBUG_MODE` (env) / Graphics Options combo | 0 Final | MDI (`MC2_GPU_DRIVEN_WATER=1`) | 1 Tint · 2 Alpha · 3 Normal · 4 Depth · 5 Shore · 6 Lighting. Mode 0 byte-identical. |
| `MC2_GPU_DRIVEN_WATER` | (config default) | — | Arms the MDI FS. All new uniforms are no-ops on the legacy FS. |
| `MC2_WATER_SKYTINT` | **OFF** (strength 0) | MDI | Gated camera-independent sky tint. `=1` → default strength 0.15; ImGui authoritative. |
| Graphics Options > **Water** panel | — | MDI | Debug-mode combo; sliders: absorption `0.022`, max alpha `0.87`, ripple `0.22`, glint `0.30`; deep/shallow colors; sky-tint strength/color; per-control + reset-all. |
| Capture metadata (`capture_baseline.py`) | — | — | `TRACKED_FLAGS` += `MC2_WATER_DEBUG_MODE`, `MC2_GPU_DRIVEN_WATER`, `MC2_RENDER_WATER_FASTPATH`; `--water-debug-mode` flag; `water_final_24`/`water_heavy_01` presets. |

All MDI tunable defaults match the former shader consts **exactly** → default
render byte-identical (reviewer-verified each slice).

## 3. Shader / pass matrix

| Shader / pass | This arc | State |
|---|---|---|
| `gos_terrain_water_mdi.frag` | +8 uniforms (1 debug, 6 tunables, 2 sky-tint), 1 debug branch, 1 gated tint term | **V-lane target — coherent** |
| `gos_tex_vertex.frag` (legacy) | none | **intentionally untouched** (ignores MDI uniforms) |
| `shoreline.frag` (post foam) | none | **intentionally untouched** (default-on) |
| `gos_terrain_water_fast(_mdi).vert` | none | untouched (VS geometry + its own `debugMode`) |
| `gpu_driven_water.comp` | none | untouched (cull/pack) |
| `gos_terrain_mask_water.vert` + `DrawMaskWater` | none | **dead code** — left as-is (out of scope) |

`shader_reflect`: **77 checked / 0 drifted**; the MDI FS golden tracks all 8
new uniforms. No other shader touched.

## 4. Visual verdict

- **MDI water is acceptable / good enough for now** — user-confirmed
  (2026-05-28): Beer-Lambert deep↔shallow gradient + dual-fBm ripple + crest
  glint, camera-independent, reads well in-scene.
- **Sky tint is optional and default-OFF.** Available behind
  `MC2_WATER_SKYTINT` + the Water panel slider; not needed currently.
- **No further water feature work needed immediately.**
- **Fresnel / reflection / refraction remain deferred** — view-angle dependent,
  in conflict with the 2026-05-17 camera-independence ruling. S3 scaffolding
  stays dead-stripped and revivable only via an explicit re-open.

## 5. Rollback / failure modes

| To disable | How | Effect |
|---|---|---|
| Sky tint | `MC2_WATER_SKYTINT` unset (default) or strength slider → 0 / reset | strength 0 → `mix(col,tint,0)` no-op → byte-identical |
| Debug views | `MC2_WATER_DEBUG_MODE` unset / combo → 0 Final | normal water |
| All new tuning | Graphics Options > Water "Reset ALL" | restores exact former-const defaults |
| Whole MDI material lane | `MC2_GPU_DRIVEN_WATER=0` (legacy path) | legacy sin-wave FS, ignores every new uniform — **untouched fallback** |

Failure-mode notes:
- **GPU-driven water gate**: all new visuals require the MDI path armed; with it
  off, water silently falls back to legacy and looks like pre-arc (safe).
- **Capture foreground-race** ([[capture-baseline-foreground-race]]): automated
  screenshots grab the desktop in an interactive session — use smoke logs for
  functional proof and drive the camera for visual A/B.
- **Smoke flake**: do NOT use `run_smoke --kill-existing`
  ([[run-smoke-kill-existing-flaky-crash]]) — it caused random per-batch
  `crash_silent`; without it tier1 is a clean 5/5.

## 6. Recommendations

- **Must fix:** none. Lane is shippable as-is.
- **Should fix before sky-tint default-ON / broad use:** get a real pixel A/B
  (camera-driven or desktop-not-foreground capture) to pick a strength; today
  only functional (gl_errors=0, prog-compiled) proof exists for the tint.
- **Optional future polish:** sky-tint/fog coherence tuning; expose remaining
  `SKY_AMBIENT`/wave params if wanted; per-mission water palette ("water-v2").
- **Deferred major features:** fresnel / planar / SSR / cubemap reflection /
  scene-color refraction — all blocked by the camera-independence ruling until
  re-opened; scaffolding (S3) retained dormant.
- **Track-R hygiene (non-visual, separate lane):** migrate water off legacy
  `u_worldToClipGL` to ViewUniforms (binding=3); register water SSBOs in
  RenderResourceRegistry; add a water section to debug-state JSON. None block
  visuals.

## Validation run (this audit)

- `shader_reflect`: 77 checked / **0 drifted** (goldens current).
- `check-env-registry.sh`: **PASS** (232 envs; `MC2_WATER_DEBUG_MODE`,
  `MC2_WATER_SKYTINT`, `MC2_GPU_DRIVEN_WATER` registered).
- No smoke run (nothing changed; prior slices' tier1 was a genuine 5/5 with
  `--kill-existing` omitted).
- Tree clean.

---

**Status:** docs/validation slice. No code touched. Verdict GREEN/YELLOW-GREEN:
water MDI visual lane closed as "good enough for now"; sky tint optional /
default-OFF; reflection/fresnel/refraction intentionally deferred. Recommended
next major lane = **water ViewUniforms + resource-registry Track-R hygiene** or
move on to another subsystem.
