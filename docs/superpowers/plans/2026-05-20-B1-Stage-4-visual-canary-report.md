# B1 Stage 4' — visual canary report (gate substitution)

Date: 2026-05-21.
Branch: `claude/nifty-mendeleev` @ `a64d990` (post-Stage-3'-parity-report).
Plan: `docs/superpowers/plans/2026-05-20-integrated-gosfx-retirement-gpu-particles-plan.md`.

## TL;DR

**Plan v6 §5.7 specified Stage 4' as "AUTOMATED via golden-frame
diff-self harness" assuming the Tier 5 RenderDoc harness (commit
`5a4fb6f`) could measure within-regime consecutive-frame pixel
stability and preserve representative frames for user aesthetic
review.** Execution attempted Stage 4' as specified and discovered a
tool-capability mismatch: the Tier 5 harness is a pipeline-state XML
diff, not a pixel/image harness. It cannot produce the metrics or
artifacts Stage 4' was designed against.

**Gate substitution (user-approved 2026-05-21):** the aesthetic
acceptability gate moves to **user-driven game play under
`MC2_GPU_PARTICLES=1`**. Stage 3' parity (98.1% spawn-event match;
report at commit `a64d990`) remains the spawn-correctness gate. The
combination of Stage 3' (mechanical spawn-correctness) + user-driven
visual judgment (mechanical-can't-measure aesthetic acceptability) is
the operative Stage 4' equivalent.

## What the Tier 5 harness actually is (vs what Plan v6 §5.7 assumed)

| Aspect | Plan v6 §5.7 assumption | Actual Tier 5 (commit `5a4fb6f`) |
|---|---|---|
| Capture cadence | Consecutive frames | Single frame at `MC2_RDC_CAPTURE_FRAME` |
| Diff type | Pixel/image (mean-pixel-delta) | `difflib.unified_diff` over normalized RenderDoc XML |
| Stability metric | mean / p95 / max diff magnitude | Binary: lines equal or diff lines exist |
| `diff-self` semantic | Within-regime consecutive-frame stability | Same-frame-captured-twice harness determinism check |
| User artifact | Per-mission frames for aesthetic comparison | `.rdc` (RenderDoc UI only) + `.xml` (textual pipeline dump) |
| Animation/noise handling | Stability metric absorbs minor float noise | XML normalization strips per-frame floats to `N` precisely so they DON'T trip the diff (a particle pipeline that flickers visually but keeps the same bind/program chunks would produce zero diff lines and "PASS") |

The harness IS useful for B1, just for a different question: pairwise
Regime-A-vs-Regime-B pipeline-state diff would validate the
ARCHITECTURAL substitution at the GL state level (Regime B binds the
particle billboard program; binds Particles SSBO at slot 14; emits
billboard draws instead of MLR enqueues). User declined this addition
as overkill for the architectural-sanity question alone — the spawn
parity in Stage 3' already covers the production-path correctness.

## Operative Stage 4' equivalent (user-driven aesthetic play)

To gate Stage 5' default-flip readiness:

1. Launch `mc2.exe` with `MC2_GPU_PARTICLES=1` set:
   ```cmd
   cd /d A:\Games\mc2-opengl\mc2-win64-v0.4
   set MC2_GPU_PARTICLES=1
   mc2.exe
   ```
2. Play a few missions (recommended: mc2_10 for terrain particle
   density, mc2_24 for combat density — both confirmed gosFX-heaviest
   in Stage 0' content recon).
3. Observe and judge acceptability of:
   - Weapon firing effects (muzzle flash, bolt geometry — geometry
     unchanged; particle decoration via new pipeline)
   - Missile launches + trails (note: per C6 B2 debt, Tube swept-mesh
     becomes a single marker particle — beams will appear as points;
     this is a known fidelity gap)
   - Hit sparks + carnage explosions (Card/PointCloud/ShardCloud via
     new pipeline)
   - Building dust on destruction (gosFX dust via new pipeline)
   - VTOL effects during briefing (if briefing-skipped, ignore)

## Known B2 visual fidelity gaps (expected; file under "polish debt")

These were filed during C5/C6 implementation as schema-extension polish
debt. Expect to see them under `MC2_GPU_PARTICLES=1`; they are NOT
B1-blocking unless user decides aesthetic acceptability requires them.

1. **ShardCloud rotation/angularity** (filed C5, commit `3670833`):
   legacy `gosFX::ShardCloud::Draw` draws each particle as
   camera-aligned TRIANGLE with per-vertex offsets driven by
   `m_angle = sin(spec->m_angularity) + m_localRotation`. Current
   64-byte `GpuParticle` has no rotation/angularity slot — particles
   render as quad billboards at correct footprint instead of rotated
   triangles. Aesthetic fidelity loss; position+size correct.
2. **Tube swept-mesh** (filed C6, commit `9c750c2`): `gosFX::Tube` is
   a swept-mesh primitive — continuous Profile cross-section emission
   over Effect lifetime, building a continuous `MLRIndexedTriangleCloud`
   with one of 8 profile shapes. **Fundamentally incompatible with the
   billboard quad schema.** Current implementation emits ONE marker
   `GpuParticle` per Tube spawn at parent origin sized to
   `max(emitterSize)`. Visible result: weapon beam trails / contrails /
   PPC beams appear as single sized points instead of swept beams.
3. **Pert/Shape/Debris/EffectCloud spawn** (filed C7-revised in `Spawn`
   dispatcher): not implemented; Spawn returns false for these spec
   types. Per Stage 0' recon they do NOT appear in tier1 stock content
   (0 spawns observed), so visual impact in stock play is none. Mod
   content / non-tier1 stock missions may exercise these types.

## User decision after aesthetic review

Two paths after user reviews particles under `MC2_GPU_PARTICLES=1`:

- **Acceptable:** spawn a separate executor session for **Stage 5'
  default-flip** (set `MC2_GPU_PARTICLES` default ON) + **A4 atomic
  deletion** (delete `mclib/gosfx/` + `mclib/mlr/` runtime trees) +
  **A5 Light dead-code cleanup**. Per plan v6 §6 these three are the
  remaining stages.
- **Not acceptable:** evaluate which B2 polish item(s) block ship.
  Tube swept-mesh is the most architecturally significant gap (would
  need a new GPU primitive type beyond billboard quads). ShardCloud
  rotation needs a `GpuParticle` schema extension (~4-byte rotation
  field). Pert/Shape/Debris/EffectCloud need per-type implementations
  if mod content cares.

## Cross-references

- Stage 3' parity report: commit `a64d990` (`docs/superpowers/plans/2026-05-20-B1-Stage-3-parity-report.md`)
- Stage 0' content recon: commit `817fb1e` (`docs/superpowers/plans/2026-05-20-B1-Stage-0-content-recon.md`)
- C5 ShardCloud B2 debt: commit `3670833`
- C6 Tube swept-mesh B2 debt: commit `9c750c2`
- C7-revised EffectAdapter + Spawn dispatcher: commit `5861ee3`
- Plan v6 §5.7 (the original Stage 4' specification): in
  `docs/superpowers/plans/2026-05-20-integrated-gosfx-retirement-gpu-particles-plan.md`
- Tier 5 harness implementation: commit `5a4fb6f`
  (`scripts/renderdoc_capture.py`, `GameOS/gameos/gos_rdoc_capture.{h,cpp}`)
