# Track A — Render Headroom: Status Snapshot

**Date:** 2026-04-29
**Scope:** Status of Phase B / Phase C precursors after the M2 chain landed earlier this week. Track A is the Modder's Paradise roadmap precondition (§6, [`2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md)).

---

## Current state

The M2 chain shipped over 2026-04-28 / 2026-04-29 in this worktree. Notable commits, in landing order:

- `12e9e4c` — ThinRecord 48→32B; `ensureRecipeForQuad` / `appendThinRecordDirect`; TerrainType packed into recipe `_wp0`.
- `21d204a` / `b447ff9` — drop unused `terrainHandle` param; remove dead `FogValue` from all shaders; thin VS reads TerrainType from recipe.
- `678b3b0` — direct thin-record emit branch in `quad.cpp` (no `gos_VERTEX[6]` construction in fast path).
- `2cf570a` — exclude overlay/detail quads from initial fast path.
- `8da7007` — M2b/c/c-ext loop hoist + water-interest fast path.
- `258e584` — M2d overlay fast path.
- `aee39cc` — Shape C `MC2_MODERN_TERRAIN_PATCHES` flipped default-on.

**M2 perf result** (per [`memory/m2_thin_record_cpu_reduction_results.md`](../../../../../../../C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/m2_thin_record_cpu_reduction_results.md), mc2_01 max-zoom-out, ~75% water):

| Metric | Pre-M2 | Post-M2d |
|---|---|---|
| `Terrain::render drawPass` | ~25 ms | **1.46 ms** |
| FPS | 50–60 | **100–145** |
| FP fast-path quads/frame | 82 | **14,000 (all)** |
| Legacy quads/frame | ~10,700 | **0** |

**Gates 5 and 6 — what was actually validated:**

- **Gate 5 (Tracy 9 ms → 5–7 ms on `quadSetupTextures`)** — *not validated as written.* The shipped M2 chain attacked `Terrain::render drawPass` (the surrounding sibling zone), not `quadSetupTextures`. The 25 → 1.46 ms result satisfies the *spirit* of the perf gate (CPU drawPass effectively eliminated). `quadSetupTextures` is still ~1.17 ms/frame and now flagged as the next CPU bucket — see [`2026-04-29-quadsetuptextures-next-slice-handoff.md`](../specs/2026-04-29-quadsetuptextures-next-slice-handoff.md). This is a stronger result on a different zone, not a missed gate, but it should be re-stated explicitly before declaring "M2 perf gate green."
- **Gate 6 (tier1 smoke)** — passes 5/5 PASS, +0 destroys delta, but **only with the FASTPATH env vars off**. `scripts/run_smoke.py:232–237` env passthrough does not propagate `MC2_PATCHSTREAM_THIN_RECORDS*` or `MC2_PATCHSTREAM_THIN_RECORD_FASTPATH`, so tier1 currently exercises the legacy path as a regression check — not the new fast path. The fast-path env vars are still **default-off** despite shape C being default-on.

**Net status:** M2 architectural goals met; perf well over budget on drawPass; fast path validated visually + via parity gate but not yet exercised by the smoke runner. Default-on flip for the FASTPATH env vars is the residual M2 closeout.

---

## Existing design artifacts for Phase B / Phase C

There is **no standalone Phase B or Phase C design spec** in `docs/superpowers/{specs,plans,explorations}/`. The only authoritative material is:

1. [`2026-04-29-m2-thin-record-cpu-reduction.md`](../specs/2026-04-29-m2-thin-record-cpu-reduction.md) §"Phase B Preview (GPU Lighting)" — two paragraphs sketching: sun + shadow diffuse moves to VS (`WorldNorm × terrainLightDir`), point lights move to a small per-frame SSBO, thin record shrinks 32 B → 16 B (recipeIdx + handle + flags + pad).
2. [`2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §6 Track A — outcome gate: `quadSetupTextures < 1 ms at max Wolfman zoom on tier1`. Recipe SSBO + 16B thin record default-on.
3. [`2026-04-27-terrain-shader-input-map.md`](../explorations/2026-04-27-terrain-shader-input-map.md) — already inventories `terrainLightDir` as a uniform on `gameos_graphics.cpp:2724`, available to the thin VS today via `bindThinUniforms` (`2026-04-29-patchstream-m1g-thin-vs-draw.md:347`). The plumbing is in place.
4. `shaders/gos_terrain_thin.vert` — already receives `WorldNorm` and projects through `terrainMVP`. Currently emits `Color = unpackARGB(lrgb)` directly from CPU-computed `lightRGB`. Phase B replaces that line with a diffuse-from-normal computation.

No design doc references "Phase C" beyond the roadmap one-liner. The closest existing artifact is [`2026-04-27-modern-terrain-surface-findings.md`](../explorations/2026-04-27-modern-terrain-surface-findings.md) §1.4–§2.2 (the cost-decomposition table that locates the next CPU buckets after `quadSetupTextures`).

---

## Gaps before Phase B can safely start

1. **Close M2 properly.** Flip `MC2_PATCHSTREAM_THIN_RECORDS / _DRAW / _FASTPATH` to default-on (the M0a-style flip Shape C just received in `aee39cc`). Until the fast path is the default, "removing `lightRGBs` from the thin record" doesn't actually save any CPU work — the legacy expanded path still runs.
2. **Patch the smoke runner env passthrough.** Add the three thin-record env vars to `scripts/run_smoke.py:232–237` so tier1 actually exercises the fast path. Without this, every Phase B slice ships with no regression coverage of the path it touches.
3. **Re-state the perf gate.** Replace "Gate 5: 9 ms → 5–7 ms on `quadSetupTextures`" with a result we actually have: drawPass 25 → 1.46 ms. Then add a Phase B-specific gate (e.g. "lightRGB SSBO reads + VS diffuse compute add < 0.3 ms vs. M2d baseline at Wolfman zoom").
4. **Capture a clean Tracy baseline at fast-path-default-on.** All current Tracy snapshots predate the M2c/d/aee39cc landings. Phase B needs a same-build `quadSetupTextures` + `Terrain::geometry` reference number.
5. **Decide Phase B's `lightRGB` policy explicitly.** The current `lightRGB` per-vertex value already encodes selection highlight + the `terrainTextures2` whiteout override (`effectiveLightRGB` in M2 spec). Pure diffuse-from-normal can't reproduce these without a per-quad/per-corner override flag staying in the thin record. Resolve: either keep one bit of override metadata in the 16 B record, or move selection rendering to a separate overlay pass.

---

## Proposed first slice for Phase B

Mirror the M0/M1/M2 cadence: small, env-gated, parity-validated. Two candidate first slices, both single-commit-shaped:

### **Slice B0 — VS-side diffuse, gated, no struct change yet** (recommended)

Goal: prove the math without touching the SSBO record layout. Add a new env gate `MC2_PATCHSTREAM_GPU_LIGHTING=1` that, in `gos_terrain_thin.vert`, replaces the `Color = unpackARGB(lrgb)` line with:

```glsl
float diffuse = max(dot(worldNorm, normalize(terrainLightDir.xyz)), 0.0);
float lit = mix(0.35, 1.20, diffuse);   // matches existing terrain.frag clamp
Color = vec4(vec3(lit), unpackARGB(lrgb).a);
```

CPU still writes `lightRGB`; shader ignores the RGB (alpha kept for selection / `terrainTextures2`). Parity gate: visual diff side-by-side at Wolfman zoom on `mc2_01`, `mc2_03`, `mc2_24`. If the diffuse term reproduces stock lighting visually, the per-corner CPU `lightRGB` computation is dead code; if not, we need to inventory what the CPU lighting was actually doing (point lights, lightning glow, fog tint) before we can drop it.

This is the equivalent of M0e: prove the path is reachable end-to-end before changing record geometry. Nothing structural, no perf gain expected, fully reversible.

### **Slice B1 — Drop `lightRGBs` from thin record, shrink to 16 B** (depends on B0)

Once B0 visual-validates, the slice that actually lands the CPU win:

- Remove `uvec4 lightRGBs` from `TerrainQuadThinRecord` (C++ + GLSL).
- Remove `effectiveLightRGB` calls from `quad.cpp` fast-path emit (4 calls × 14,000 quads/frame = the headline saving).
- Carry one `selectionMask` bit per corner in the existing `flags` field (or a new `uvec4` if 4 bits aren't free) so the selection-highlight override survives.
- `kPatchStreamThinRecordBytesPerSlot` 32 → 16.
- Phase B1 gate: drawPass + thin-record-emit zone delta vs. M2d baseline; expect a ~0.3–0.5 ms drop on water-light maps where every quad is in the fast path.

**Defer to Phase B2:** point lights / lightning glow per-frame SSBO. Most missions have zero point lights most of the time; the SSBO can be conditionally bound and the shader can early-out on `numPointLights == 0`. This is its own slice once B1 lands.

---

## References

**Specs / explorations:**
- [`docs/superpowers/specs/2026-04-29-m2-thin-record-cpu-reduction.md`](../specs/2026-04-29-m2-thin-record-cpu-reduction.md) — M2 spec; Phase B preview at §"Phase B Preview"
- [`docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) — Track A framing, §6
- [`docs/superpowers/specs/2026-04-29-quadsetuptextures-next-slice-handoff.md`](../specs/2026-04-29-quadsetuptextures-next-slice-handoff.md) — next CPU bucket; explicit Tracy decision tree
- [`docs/superpowers/explorations/2026-04-27-modern-terrain-surface-findings.md`](2026-04-27-modern-terrain-surface-findings.md) — cost decomposition table
- [`docs/superpowers/explorations/2026-04-27-terrain-shader-input-map.md`](2026-04-27-terrain-shader-input-map.md) — current uniform inventory (`terrainLightDir` already on the thin VS)
- [`docs/superpowers/plans/2026-04-29-patchstream-m1g-thin-vs-draw.md`](../plans/2026-04-29-patchstream-m1g-thin-vs-draw.md) — `bindThinUniforms` plumbing for `terrainLightDir`

**Memory:**
- [`memory/m2_thin_record_cpu_reduction_results.md`](../../../../../../../C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/m2_thin_record_cpu_reduction_results.md) — perf results, parity-log gotcha
- [`memory/patchstream_shape_c.md`](../../../../../../../C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/patchstream_shape_c.md) — Shape C cache-read default-on
- [`memory/terrain_lighting_range_ceiling.md`](../../../../../../../C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/terrain_lighting_range_ceiling.md) — `mix(0.35, 1.20, diffuse)` ceiling, load-bearing for any VS diffuse rewrite

**Source files Phase B will touch:**
- `shaders/gos_terrain_thin.vert` (RGB compute moves here)
- `GameOS/gameos/gos_terrain_patch_stream.h` (`TerrainQuadThinRecord` 32 → 16 B)
- `GameOS/gameos/gos_terrain_patch_stream.cpp` (`appendThinRecordDirect`, `kPatchStreamThinRecordBytesPerSlot`)
- `mclib/quad.cpp` (drop `effectiveLightRGB` calls in fast path)
- `scripts/run_smoke.py:232–237` (env passthrough for fast-path coverage)
