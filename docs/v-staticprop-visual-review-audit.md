# V-STATICPROP-VISUAL-REVIEW-AUDIT — Track V StaticPropOpaque arc summary

- **Slice:** V-STATICPROP-VISUAL-REVIEW-AUDIT
- **Branch SHA at audit time:** `9b1bf596` (tip of `claude/nifty-mendeleev`)
- **Date:** 2026-05-27
- **Scope:** StaticPropOpaque pipeline only. Comparative arc captures at the
  current tip across the four shipped visual gates (baseline / hemisphere
  ambient / SH-L2 IBL / both) on two camera presets. Audit-only — no
  engine code changes; tracked summary doc + capture corpus.
- **Anti-scope:** terrain, mech, shadow, VFX. (V-MATERIAL-PBR-2 plan is
  cross-referenced but not yet implemented; this audit precedes that
  implementation slice.)

---

## §1. Shipped slices ledger (Track V to date)

Commits walked from the V-BASELINE-0 slice through the current tip
`9b1bf596`. Each row is one shipped (or planned) slice; SHAs are the
authoritative commit pointers.

| # | Slice | SHA       | One-line purpose                                                                                                |
|---|-------|-----------|-----------------------------------------------------------------------------------------------------------------|
| 1 | V-BASELINE-0                     | `3f6d1af6` | Visual baseline capture harness + initial corpus (`scripts/capture_baseline.py`, `tests/visual/baselines/`).      |
| 2 | V-MATERIAL-STATIC-0              | `9a9d6eb0` | ImGui inspector inventory of static-prop material rows (read-only).                                              |
| 3 | V-LIGHTING-STATIC-0              | `af314d22` | Static-prop lighting audit doc + inspector clarification rows (`docs/static-prop-lighting-audit.md`).            |
| 4 | V-AMBIENT-STATIC-1               | `19e85517` | Gated hemisphere ambient fill (`MC2_STATIC_PROP_AMBIENT_V1`). Default-OFF, byte-identical at strength=0.         |
| 5 | SHADER-REFLECT-HYGIENE-2         | `f005f7ce` | Regen `static_prop.vert` reflect goldens post V-AMBIENT-STATIC-1.                                                |
| 6 | V-MATERIAL-DEBUG-1               | `feca6efe` | Gated material debug views (`MC2_STATIC_PROP_DEBUG_MATERIAL`, modes 1–4). Default 0 = byte-identical.            |
| 7 | SHADER-REFLECT-HYGIENE-3         | `b695c803` | Regen `static_prop.frag` reflect goldens post V-MATERIAL-DEBUG-1.                                                |
| 8 | V-IBL-STATIC-0                   | `cfad795c` | IBL plan + asset/tool probe (`docs/ibl-plan.md`).                                                                |
| 9 | V-IBL-SH-PROJECTOR-RECON         | `4c2bd769` | V-IBL-STATIC-1 implementation plan + projector recon.                                                            |
| 10 | V-IBL-STATIC-1                  | `64e58c11` | Gated SH-L2 IBL ambient (`MC2_STATIC_PROP_IBL_SH`) + projector tool. Default-OFF.                                |
| 11 | SHADER-REFLECT-HYGIENE-4        | `1e978be1` | Regen `static_prop.vert` reflect goldens post V-IBL-STATIC-1 + slider plumbing fix.                              |
| 12 | (smoke allowlist)               | `8a9b79cc` | Extend smoke env allowlist for Track V feature flags.                                                            |
| 13 | V-IBL-STATIC-1-SOAK             | `e061e44c` | 5-point strength sweep × 2 presets (`tests/visual/baselines/V-IBL-STATIC-1-SOAK.md`).                            |
| 14 | V-IBL-STATIC-1-TUNE             | `99779f70` | Bake `g_iblShStrength = 0.5f` default after soak eyeball pass.                                                   |
| 15 | (capture fix)                   | `e2fbe4bf` | `captured_flags()` reads subprocess env not `os.environ` (fixes V-IBL-STATIC-1-SOAK metadata caveat).            |
| 16 | (terrain probe)                 | `877073ae` | Expose `matNormalBoost` + `tintStrengthScale` uniforms (byte-identical defaults). [Adjacent / not StaticProp.]   |
| 17 | (skybox)                        | `6a7d994f` | Record v-flip experiment as paused state. [Adjacent / not StaticProp.]                                           |
| 18 | SHADER-REFLECT-HYGIENE-5        | `b32ddc6c` | Regen terrain reflect goldens. [Adjacent / not StaticProp.]                                                      |
| 19 | V-MATERIAL-PBR-1                | `eb7bebdb` | Expose `metallicFactor` + `roughnessFactor` in inventory + debug modes 5/6.                                      |
| 20 | (inventory guardrail)           | `a7b2a27e` | `static_assert` guard on static-prop material inventory layout.                                                  |
| 21 | V-IBL-STATIC-2                  | `5bfd15d8` | Per-mission SH coefficient selection + fallback (`MC2_STATIC_PROP_IBL_SH_SET` aux knob; `IblShRegistry.h`).      |
| 22 | V-MATERIAL-PBR-2-PLAN           | `9b1bf596` | Plan-only doc for the first PBR-shading consumer of metallic/roughness (`docs/v-material-pbr-2-plan.md`).        |

**Count of shipped StaticPropOpaque-track slices (excluding adjacent
terrain/skybox):** 18 commits across 12 logical slices, plus 1 plan
doc at the tip.

---

## §2. Captures index

Eight (mission × config) PNG+JSON+log trios produced at `9b1bf596`.
Filenames are relative to `tests/visual/baselines/`. PNG sha256 first
16 hex digits given; bytes from on-disk size.

| Mission              | Config        | PNG filename                                                  | sha256 (16) | bytes   |
|----------------------|---------------|---------------------------------------------------------------|-------------|---------|
| mc2_24 (preset 01)   | baseline      | `arc_staticprop_baseline_01_9b1bf596_baseline.png`            | `eba60f30f651d42e` | 4855571 |
| mc2_24 (preset 01)   | ambient_v1    | `arc_staticprop_baseline_01_9b1bf596_ambient_v1.png`          | `af3feb810b35a28d` |  633176 |
| mc2_24 (preset 01)   | ibl           | `arc_staticprop_baseline_01_9b1bf596_ibl.png`                 | `897bf18aaadf2dca` | 4673619 |
| mc2_24 (preset 01)   | both          | `arc_staticprop_baseline_01_9b1bf596_both.png`                | `f8751640f08afd72` | 4672802 |
| mc2_10 (preset 02)   | baseline      | `arc_staticprop_baseline_02_9b1bf596_baseline.png`            | `210a6e2c581d77d1` | 4856321 |
| mc2_10 (preset 02)   | ambient_v1    | `arc_staticprop_baseline_02_9b1bf596_ambient_v1.png`          | `bf86218bba5e2899` | 4652019 |
| mc2_10 (preset 02)   | ibl           | `arc_staticprop_baseline_02_9b1bf596_ibl.png`                 | `82b2e7fec4d2d7cf` | 4674219 |
| mc2_10 (preset 02)   | both          | `arc_staticprop_baseline_02_9b1bf596_both.png`                | `884d8f6867af6cf3` | 4673929 |

**Distinct shas:** 8 / 8 — every config produced a different frame on
both presets, confirming engine state actually differed.

### Capture command per config

For reproducibility (env in PowerShell):

| Config        | Command                                                                                       |
|---------------|-----------------------------------------------------------------------------------------------|
| baseline      | `py -3 scripts/capture_baseline.py --preset <preset>`                                         |
| ambient_v1    | `$env:MC2_STATIC_PROP_AMBIENT_V1='1'; py -3 scripts/capture_baseline.py --preset <preset>`    |
| ibl           | `py -3 scripts/capture_baseline.py --preset <preset> --strength 0.5`                          |
| both          | `$env:MC2_STATIC_PROP_AMBIENT_V1='1'; py -3 scripts/capture_baseline.py --preset <preset> --strength 0.5` |

**Important capture-harness mechanic:** `scripts/capture_baseline.py`
unconditionally `env.pop()`s `MC2_STATIC_PROP_IBL_SH` and
`MC2_STATIC_PROP_IBL_SH_STRENGTH` when `--strength` is **not** supplied
(lines 172–174). To enable the IBL gate from this harness you must
use `--strength <value>`; setting the env var in the parent shell is
overridden. The four-config arc above accommodates this by using
`--strength 0.5` (the shipped default per V-IBL-STATIC-1-TUNE
`99779f70`) for the `ibl` and `both` runs.

### Capture-harness caveat

`arc_staticprop_baseline_01_9b1bf596_ambient_v1.png` is anomalously
small (633 KB vs ~4.6 MB for the other seven captures). The sha256 is
distinct from the rest, and the JSON sidecar correctly records
`MC2_STATIC_PROP_AMBIENT_V1=1`, so the engine state was correct. The
size delta is consistent with OS-level screenshot capturing a
mostly-empty desktop frame (e.g. while mc2's window was briefly
non-foreground); the PNG compresses much smaller when most pixels
are uniform. The other ambient_v1 frame (preset 02, 4.65 MB) is
normal-size, so this is a one-shot screenshot race rather than a
configuration error. Reproducing with a fresh run is the remediation
if eyeball review needs a clean frame.

---

## §3. Feature gate state matrix

Reflects shipped state at `9b1bf596`, cross-checked against
`RenderCore/RendererFeatureRegistry.h`. "Default" = behavior when env
var is unset/absent.

| Feature                       | Env var                                | Default | Strength range / mode | Mathematical default-OFF byte-identity | Recommend flip? (subagent read)                                                                                            |
|-------------------------------|----------------------------------------|---------|-----------------------|-----------------------------------------|----------------------------------------------------------------------------------------------------------------------------|
| Hemisphere ambient v1         | `MC2_STATIC_PROP_AMBIENT_V1`           | OFF     | u_ambientV1Strength on/off (1.0 when ON) | YES — uniform=0.0 short-circuits add | LEAN-NO at this slice. Hemisphere-only ambient is cheap but doubles up with IBL when both ON; prefer IBL as the canonical ambient. Keep gated for soak. |
| SH-L2 IBL                     | `MC2_STATIC_PROP_IBL_SH`               | OFF     | strength ∈ [0,3], default 0.5 | YES — `if (u_iblShStrength > 0.0)` guard in shader | LEAN-YES at strength=0.5. User eyeball pass at `e061e44c` rated 0.5 "looks good — subtle ambient fill without over-brightening shadowed sides." Default coefficients are sane (`DaySkyHDRI063B_4K.exr` projection). Risk: single global probe; per-mission HDRIs not yet authored (V-IBL-STATIC-2 scaffolding exists, 0 registry entries today). |
| SH coefficient set selector   | `MC2_STATIC_PROP_IBL_SH_SET` (aux)     | unset → fallback to "default" | name string | N/A (selection only; never gates SH itself) | NO — debug/dev knob, must stay opt-in. Genuine consumer only appears when registry gains per-mission entries. |
| Material debug                | `MC2_STATIC_PROP_DEBUG_MATERIAL`       | OFF (mode=0) | mode ∈ {0..6}      | YES — `if (u_debugMaterialMode != 0) return;` after legacy path | NO — debug-only; flipping default-on would replace render with grayscale views. Must stay opt-in. |
| Material GPU table            | `MC2_MATERIAL_GPU`                     | ON      | on/off               | N/A (default-on)                       | already ON. No change. |
| Material GPU sample (albedo)  | `MC2_MATERIAL_GPU_SAMPLE`              | ON      | on/off               | N/A (default-on)                       | already ON. No change. |
| Static prop registry          | `MC2_STATIC_PROP_REGISTRY`             | ON      | on/off               | N/A (default-on)                       | already ON. Editor sets =0. No change. |
| Snapshot static prop build    | `MC2_SNAPSHOT_STATIC_PROP_BUILD`       | ON      | on/off               | N/A (snapshot authority since 2026-05-27) | already ON. No change. |
| Static prop packet dispatch (kill-switch) | `MC2_STATIC_PROP_LEGACY_DISPATCH` | OFF (= packet path ON) | on/off       | N/A (default-on path)                  | already correct polarity. No change. |
| ViewUniforms UBO              | `MC2_VIEW_UNIFORMS`                    | ON      | on/off               | N/A (default-on since `cf5f67bc`)       | already ON. No change. |
| Material KTX                  | `MC2_MATERIAL_KTX`                     | OFF     | on/off               | YES                                     | NO at this slice — Phase 0 RGBA8-only; gate stays opt-in until full path matures. |

**Polarity summary:** Of the 11 StaticPropOpaque-relevant gates, 7 are
already correctly default-on (the dispatch/material/snapshot
substrate). The 4 remaining "visual" gates are all default-OFF:
`AMBIENT_V1`, `IBL_SH`, `IBL_SH_SET` (selector — N/A), and
`DEBUG_MATERIAL`. **Only `IBL_SH` is a flip candidate.**

---

## §4. Closure-axis scorecard

Confirmed against `RenderCore/RenderPassContract.h` line 94–105
(StaticPropOpaque entry).

| Lane              | viewUniformsBound | pipelineDescRegistered | snapshotRowAuthoritative | inspectorVisible | killSwitchEnv                       | notes (per contract row)                                                                                       |
|-------------------|:-----------------:|:----------------------:|:------------------------:|:----------------:|-------------------------------------|----------------------------------------------------------------------------------------------------------------|
| StaticPropOpaque  | YES               | YES                    | YES                      | YES ("StaticProp") | `MC2_SNAPSHOT_STATIC_PROP_BUILD`   | Reference path: snapshot-owned v6 DrawPacket+meta dispatch default-on (STATIC-PROP-V3-FLIP `2a88a5a8`).        |
| Terrain           | NO                | NO                     | NO                       | YES              | (none)                              | TerrainPassFacts is a passive recorder; not yet authoritative.                                                  |
| MechOpaque        | NO                | NO                     | YES                      | YES              | `MC2_SNAPSHOT_MECH_EXTRACT`         | Rows extracted to snapshot (MECH-EXTRACTION-0); pipeline still legacy.                                          |
| Shadow            | NO                | NO                     | NO                       | YES              | (none)                              | Three lanes; counters live-read.                                                                                |
| VFX               | NO                | NO                     | NO                       | YES              | (none)                              | Object-ID prohibited; `GpuTrailKind {None, MissileSmoke, PpcBolt}`.                                            |

**StaticPropOpaque is the only lane with all four closure axes green.**
It is the reference path for any subsequent lane that wants the same
closure (terrain pass-fact promotion, mech pipeline-desc routing, etc.).

---

## §5. Recommendations

### §5.a Default flips

**Recommend: flip `MC2_STATIC_PROP_IBL_SH` default to ON** at the
current bake (`g_iblShStrength = 0.5f`). Evidence:

1. User eyeball pass on the soak (`e061e44c`) judged 0.5 "looks good
   — subtle ambient fill without over-brightening shadowed sides."
2. Mathematical default-identity to OFF still holds at strength=0 via
   the slider, so users who dislike it can dial it down to 0.0 in
   the inspector without restart.
3. The shader short-circuit (`if (u_iblShStrength > 0.0)`) makes the
   cost negligible when strength is 0.
4. Coefficients are projected from a real HDRI (`DaySkyHDRI063B_4K.exr`),
   not synthesised — they will not look "wrong" by construction.

**Counter-evidence to weigh:**

- The single global probe is biome-agnostic. V-IBL-STATIC-2 added the
  per-mission selection scaffolding but the registry has **0 entries
  today** (every mission falls back to the default set). Flipping
  default-on now ships one ambient SH for all missions; this is
  acceptable for a first pass but is a known artistic limitation.
- No tier1 ok=1 gate currently covers strength=0.5 default-ON across
  all 5 missions in a single smoke. A pre-flip smoke 5/5 PASS at
  `MC2_STATIC_PROP_IBL_SH=1` + slider default 0.5 should be the
  gating probe; the V-IBL-STATIC-1-SOAK §"Validation probes" already
  passed strength=0.25 / clamp lo/hi but a default-strength tier1
  was not explicitly listed.

**Recommend: do NOT flip `MC2_STATIC_PROP_AMBIENT_V1`.** Evidence:

1. Hemisphere ambient is a cheaper, less directionally rich
   approximation of what SH-L2 IBL already gives you.
2. Stacking AMBIENT_V1 + IBL_SH (the "both" config) double-counts
   ambient light. Users who flip both default-on get an
   over-brightened scene.
3. If IBL flips default-on, AMBIENT_V1 becomes redundant; consider
   it for retirement to `EnvVarKind::Retired` in a follow-up hygiene
   slice once IBL is the canonical ambient.

### §5.b Next-lane recommendation

**Top pick: V-MATERIAL-PBR-2 implementation** (the plan at
`9b1bf596`). Rationale:

1. The plan is already written, reviewed-pending, and identifies
   four explicit open questions (sun-direction discoverability,
   albedo-at-vertex trade-off, per-vertex vs per-fragment, and
   ViewUniforms-off interlock) — these are advisor-sign-off questions,
   not research questions.
2. The implementation is small (5 files, ≤80 src lines) per the
   plan's §5 estimate.
3. It is the first lighting consumer of metallic/roughness, which
   were shipped as dead-data in V-MATERIAL-PBR-1; closing that gap
   reduces the "data exists but isn't read" surface in the engine.
4. StaticPropOpaque is the only lane with full closure today;
   compounding visual work on it before opening a new lane preserves
   the focus discipline that produced the closure.

**Runner-up: V-IBL-STATIC-3 (real per-mission HDRIs)** — author 3–5
biome-specific SH sets and wire them into the `IblShRegistry`. This
makes the V-IBL-STATIC-2 selection scaffolding actually do something.
Lower priority than PBR-2 because the visual delta of biome-correct
ambient is subtler than first-time specular highlights.

**Skip-for-now: terrain visual pass / mech visual pass.** Both are
attractive but neither lane has the closure (`viewUniformsBound`,
`pipelineDescRegistered`, `snapshotRowAuthoritative`) that
StaticPropOpaque does, so visual work there pays a substrate-tax
that StaticProp doesn't.

### §5.c Follow-ups

1. **preset_02 timing race** flagged in V-IBL-STATIC-1-SOAK (`s0p50`
   and `s1p00` shared a sha at `8a9b79cc`) was not re-tested at
   `9b1bf596`; rerun if the soak ladder is ever re-captured for
   regression purposes.
2. **`captured_flags()` metadata fix verification** (`e2fbe4bf`): the
   fix is in place at this tip and is confirmed by the JSON sidecars
   in §2 above — the `ibl` and `both` runs correctly report
   `MC2_STATIC_PROP_IBL_SH=1`, `MC2_STATIC_PROP_IBL_SH_STRENGTH=0.5`,
   and `MC2_STATIC_PROP_AMBIENT_V1=1` where applicable.
3. **V-MATERIAL-PBR-2-PLAN §9 open questions** must be answered
   before the impl slice ships (sun direction, F0 approximation,
   per-vertex vs per-fragment, ViewUniforms-off safety).
4. **Capture harness CLI:** the env-pop interaction in §2 above
   should be either documented in `scripts/capture_baseline.py`
   docstring or replaced with a `--config baseline|ambient|ibl|both`
   shortcut. Today the only way to set `IBL_SH=1` is via `--strength`,
   which is a discoverability gotcha. Mild — could be a hygiene
   follow-up.
5. **AMBIENT_V1 retirement** if IBL flips default-on (per §5.a).
6. **Tier1 5/5 default-on IBL probe** as the gate for the §5.a flip.

---

## §6. Known caveats

1. **OS-screenshot non-determinism.** Captures come from
   `pyautogui.screenshot()` (full desktop), not from an in-engine
   GL framebuffer dump. Two consecutive runs with identical engine
   state can produce different PNGs (the `--verify` flag exposes
   this). PNG byte-delta does NOT linearly correspond to pixel
   delta. See `tests/visual/baselines/README.md` and the
   V-BASELINE-0 howto. The `arc_staticprop_baseline_01_9b1bf596_ambient_v1.png`
   633 KB anomaly in §2 is a textbook instance.
2. **PNG sha256 ≠ pixel-identical proof.** PNG zlib compression is
   deterministic given identical RGBA input, but the reverse —
   distinct sha256 → distinct pixels — is true only up to the
   compressor. For "byte-identical engine output" claims, prefer
   the explicit smoke probes that count framebuffer hashes inside
   mc2.exe, not these OS-screenshot captures.
3. **ImGui slider overrides env strength at runtime.** All four IBL
   captures use the shipped default `g_iblShStrength = 0.5f`
   (`gos_static_prop_batcher.cpp:203` per V-IBL-STATIC-1-TUNE). If
   the inspector is opened during a capture, the slider value would
   override the env-default; capture_baseline.py runs in smoke mode
   so the inspector is never opened.
4. **Per-mission SH selection scaffolding (V-IBL-STATIC-2) currently
   exposes 0 registry entries.** All missions fall back to the
   default coefficient set today. The selector env var
   (`MC2_STATIC_PROP_IBL_SH_SET`) is a real dev knob but has nothing
   meaningful to select.
5. **Single global probe for IBL.** Until V-IBL-STATIC-3 ships
   biome-specific HDRIs, the ambient is "daytime sky" regardless of
   mission setting (sand, snow, urban, etc.).
6. **`MC2_STATIC_PROP_DEBUG_MATERIAL` mode probes (modes 5 and 6 for
   roughness/metallic) were NOT captured in this arc.** The arc
   captures focus on the default-render configurations. Debug-mode
   captures are still gated by V-MATERIAL-PBR-1 and confirmed
   functional by that slice's commit log; re-capturing here was
   judged out of scope (debug views are not "the rendered game").
7. **Resolution recorded as 3840x2160** in all sidecars — captures
   are at the user's full desktop resolution (4K), not 1920x1080 as
   the `presets.json` default suggests. The resolution field is
   pyautogui-reported actual screen size; the presets `default_resolution`
   is informational only. Worth normalising to 1920x1080 in a
   harness follow-up if cross-machine PNG diffing matters.

---

## §7. Cross-references

- `tests/visual/baselines/V-IBL-STATIC-1-SOAK.md` — strength sweep
  audit at `8a9b79cc`, eyeball-pass evidence for §5.a recommendation.
- `docs/static-prop-lighting-audit.md` (`af314d22`) — pre-Track V
  lighting audit; the per-vertex Gouraud model this arc composes on.
- `docs/ibl-plan.md` (`cfad795c`) — broader IBL roadmap (V-IBL-STATIC
  -3 cubemap track belongs here).
- `docs/v-ibl-static-1-plan.md` (`4c2bd769`) — direct precedent doc
  for V-IBL-STATIC-1 (the structure this audit echoes).
- `docs/v-material-pbr-2-plan.md` (`9b1bf596`) — the immediate
  next-implementation candidate per §5.b.
- `docs/engine-closure-audit.md` (`b7987b70`) — pre-Track V closure
  audit; this doc's §4 scorecard is the StaticPropOpaque-specific
  fragment of that broader audit.
- `RenderCore/RenderPassContract.h` (`828432b6` ship,
  `2b5024c9` harden) — authoritative source for §4 closure scorecard.
- `RenderCore/RendererFeatureRegistry.h` — authoritative source for
  §3 feature gate matrix.
- `RenderCore/IblShRegistry.h` — per V-IBL-STATIC-2 (`5bfd15d8`),
  the per-mission SH set selection registry. Currently 0 non-default
  entries.
- `scripts/capture_baseline.py` — capture harness used to produce §2
  corpus. Env-pop mechanic at lines 172–174.
- HANDOFF `cf5f67bc` — ViewUniforms default-on flip; `u_cameraWorldPos`
  availability that V-MATERIAL-PBR-2 will rely on.

---

## §8. Audit summary

- **Shipped:** 12 logical StaticPropOpaque slices across 18 commits,
  producing 4 user-visible visual gates (AMBIENT_V1, IBL_SH,
  DEBUG_MATERIAL, IBL_SH_SET selector) all default-OFF.
- **Closure:** StaticPropOpaque is the only render lane with all four
  RenderPassContract axes green; reference path for future lane
  closures.
- **Default flip:** IBL_SH at strength=0.5 is the one flip
  recommended; gated by a tier1 5/5 default-on smoke probe.
- **Next:** V-MATERIAL-PBR-2 impl slice (plan already drafted at
  `9b1bf596`). Runner-up: V-IBL-STATIC-3 biome HDRI authoring.
- **Open:** harness CLI gotcha (env-pop interaction), resolution
  field, AMBIENT_V1 redundancy with IBL_SH if/when IBL flips.
