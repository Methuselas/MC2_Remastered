# VL-1 Plan — Premise Correction Directive + Contract-Compliance Review

**From:** Session B (render-contract owner).
**To:** Session A's VL-1 plan
(`A:/Games/mc2-trackv-visual-fidelity/docs/superpowers/plans/2026-06-04-vl1-filmic-color-baseline-plan.md`,
v2 / commit `698dd812`).
**Authority:** [2026-06-04-trackv-coherent-render-pipeline.md](2026-06-04-trackv-coherent-render-pipeline.md)
(the render contract) + the terrain-colormap default-format investigation below.
**Scope:** premise + verification corrections only. **Not** a rerank, **not** a
competing roadmap, **not** a code change. The VL-1 plan's architecture
(`MC2_FILMIC` atomic bundle) is endorsed unchanged.

This document is the deliverable for "patch directive to Session A's VL-1 plan."
It is a directive (a precise change-list Session A applies), not a direct edit of
Session A's branch — deliberately, to avoid a cross-session commit underneath an
active worktree. The final section is the contract-compliance review.

---

## 0. The one fact that drives this directive (grep-grounded)

**By default, ALL scene albedo is uploaded LINEAR / no-decode — including
terrain.** There is no terrain-vs-rest decode asymmetry today.

Grounding (worktree `nifty-mendeleev`, deploy trees probed directly):

- Terrain colormap upload is a two-level gate:
  - env `MC2_COLORMAP_KTX2` (default **ON**, kill-switch `=0`,
    `terrtxm2.cpp:202-208`),
  - **AND** the `<map>.burnin.ktx2` sidecar must physically exist
    (`terrtxm2.cpp:1616-1628`), else the branch at
    `gos_terrain_indirect.cpp:876` falls through to the RGBA8 path at `:940`.
- **v0.4 (the smoke-test / current deploy target):** `data/textures/` has **no
  `.burnin.ktx2`** (only `.burnin.jpg`). So `ktx2ColormapPath` stays empty, the
  BC7 branch is skipped, and the colormap uploads `GL_RGBA8` (linear) at
  `gos_terrain_indirect.cpp:940`.
- **v0.3 (sidecars present):** the BC7 branch runs, but the shipped
  `.burnin.ktx2` atlases are **vkFormat=145 (`VK_FORMAT_BC7_UNORM_BLOCK`)**, not
  146. The cook `bake_colormap_ktx2.py` deliberately passes
  `--format R8G8B8A8_UNORM --assign-tf linear` *"so BC7 with vkFormat=145 (UNORM)
  matches the existing GL_RGBA8 linear path."* The runtime branch
  `gos_terrain_indirect.cpp:881` selects
  `vkFormat==146 ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM`,
  so vkFormat=145 → `GL_COMPRESSED_RGBA_BPTC_UNORM` = **linear, no decode.**

**Conclusion:** terrain albedo is linear-no-decode in *both* deploys. The
"terrain decodes sRGB while props/mechs do not" asymmetry is **false**. The real
hazard is **latent**: default-OFF partial-decode gates (e.g.
`MC2_TEXMGR_COMPRESSED_UPLOAD`, with 3,811 armed `vkFormat=146` mech/shared
sidecars; per-path BC7 gates) that, if flipped piecemeal, would decode some
surfaces and not others. This is exactly contract Section 1 / Section 3.4.

---

## 1. Premise corrections (replace the old framing)

### 1.1 Executive verdict (plan line 17)

REPLACE:

> Two systems already diverge: **terrain albedo is uploaded sRGB**
> (`gos_terrain_indirect.cpp:881`, vkFormat 146) while **static-prop and mech
> albedo are uploaded linear** ...

WITH:

> **By default every scene albedo is uploaded LINEAR / no-decode** — terrain
> colormap (`GL_RGBA8` at `gos_terrain_indirect.cpp:940` in v0.4; the BC7 atlas
> path at `:881` is vkFormat=**145 UNORM = linear** by deliberate cook, so it
> resolves to `GL_COMPRESSED_RGBA_BPTC_UNORM`, also undecoded), static props
> (`:2531/:3119/:2578/:3196`), and mechs (`gl_utils.cpp:182` → `GL_RGBA8`). There
> is **no existing terrain-vs-rest asymmetry.** The pipeline is uniformly
> wrong-but-consistent: sRGB-encoded texels sampled as linear, lit in the wrong
> space, written unencoded. The danger is **latent**: default-OFF partial-decode
> gates (`MC2_TEXMGR_COMPRESSED_UPLOAD` with 3,811 armed vkFormat=146 sidecars;
> per-path BC7 gates) that decode *some* surfaces if flipped piecemeal. VL-1's
> job is to make decode/encode coherent across all albedo lanes in one bundle so
> the latent gun is never half-fired.

### 1.2 Upload-side map (plan line 36) — the actionable bug

The plan's row marks terrain BC7 as "already sRGB ✅ — leave it." That is wrong
and would leave the BC7 colormap undecoded under `MC2_FILMIC`.

REPLACE the row:

> | Terrain albedo (BC7 KTX2) | `gos_terrain_indirect.cpp:881` | `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM` if vkFormat==146 | sRGB ✅ | sRGB |

WITH:

> | Terrain albedo (BC7 KTX2) | `gos_terrain_indirect.cpp:881` | `GL_COMPRESSED_RGBA_BPTC_UNORM` (shipped atlases are **vkFormat=145**, not 146) | LINEAR ❌ | sRGB |

### 1.3 Propagate the same fix to every "BC7 :881 already correct, leave it"

These plan locations all assume terrain BC7 is already decoded and must be
corrected to "needs decode under FILMIC":

- **Line 63** ("terrain albedo (BC7 already ✅ + RGBA8 fallback)") → "terrain
  albedo (BC7 `:881` **and** RGBA8 fallback `:940` — both linear, both decode
  under FILMIC)".
- **Line 104** ("BC7 path `:881` already sRGB — leave.") → "BC7 path `:881` is
  vkFormat=145 (linear); under FILMIC it must also select the sRGB BPTC variant —
  see Task 4 amendment".
- **Task 4 / line 210** ("(BC7 `:881` already correct)") → see 2.1.

### 1.4 Net effect on scope

The v0.4 smoke target is *unaffected at runtime* (no `.burnin.ktx2` → the `:940`
RGBA8 swap the plan already has covers it). The correction matters for **v0.3 and
any future colormap-`.ktx2` deploy**, where leaving `:881` untouched would make
the BC7 colormap the single terrain surface that does **not** decode under
FILMIC — a visible terrain-vs-itself seam. So the premise correction is not
cosmetic; it adds one required upload-path change (2.1).

---

## 2. Required plan amendments

### 2.1 Task 4 — add the BC7 colormap path

Amend Task 4 (terrain color paths) to ALSO switch the BC7 colormap branch under
the gate, by either of (Session A picks; both are contract-clean):

- **(a) Runtime (preferred, matches Task 3's static-prop BC7 approach):** at
  `gos_terrain_indirect.cpp:881`, select
  `bptcInternalFormatFor(gos_FilmicEnabled())` — i.e. force the sRGB BPTC variant
  under FILMIC **regardless of the file's vkFormat** (the atlas content is
  sRGB-authored; vkFormat=145 is a cook label, not a content fact). Gate-off =
  unchanged (`GL_COMPRESSED_RGBA_BPTC_UNORM`).
- **(b) Asset (cleaner long-term):** re-cook `*.burnin.ktx2` with
  `--assign-tf srgb` (vkFormat=146) so `:881` decodes via the existing branch.
  Requires re-deploying the atlases; defer if the cook/deploy lockstep is not in
  this slice.

Add a gate-off byte-identity check for the BC7 path specifically (run on a
mission whose `.burnin.ktx2` exists, e.g. under v0.3 or a sidecar-present build).

### 2.2 Keep `MC2_FILMIC` as the atomic decode+tonemap+encode bundle — ENDORSED

No change. The plan's single-gate bundle (decode on all albedo + ACES on + one
composite encode, gate-off byte-identical) is exactly contract Section 3.4
("VL-1 must land the decode edge and the encode edge together, as one atomic
contract flip"). This is the correct architecture; do not split it. The premise
correction does not touch it — it only widens the decode set by one path.

### 2.3 Add the cross-lane verification requirement (contract Section 11)

The plan's validation (pixel history + A/B soak) is good but does not yet require
the two debug views the contract makes mandatory for a reviewable color flip. Add
to the Validation plan:

- **Albedo-only, cross-lane debug view (REQUIRED, contract S1/S2 + §11).** A debug
  mode that renders *decoded albedo only* for terrain + static-prop + mech +
  terrain-overlay in one frame, so a reviewer can confirm the same-albedo-family
  test: a grey building on grey ground reads as one mid-grey; a mech reads the
  same family as the ground it stands on. Without this view the FILMIC result is
  not reviewable — it is the acceptance gate for VL-1, not a nice-to-have. (The
  terrain albedo view exists but is outside the canonical `RenderDebugView`
  registry; promoting it needs `kDebugViewMask_Terrain` non-zero + a
  `TerrainViewToShaderMode` map — no shader edit.)
- **Encode verification (REQUIRED, contract INV-COLOR-1/2).** Prove *exactly one
  decode and exactly one encode*:
  - On a terrain↔prop↔mech boundary pixel: RenderDoc pixel history (already in the
    plan) must show a single sRGB-storage decode at sample and a single
    `linearToSrgb` at composite — no manual `pow()` decode, no second encode, no
    `GL_FRAMEBUFFER_SRGB`.
  - A **HUD swatch readback** proving the UI path is **not** encoded (composited
    after the single encode) — confirms INV-COLOR-5 and the manual-encode-not-
    `GL_FRAMEBUFFER_SRGB` decision.
  - A **known-value swatch** (e.g. mid-grey 0.5) round-trip: sample → light=1 →
    encode → read; confirms the decode/encode pair is identity for unlit texels
    (no net shift), which is the contract's "default-OFF byte-identical" cousin
    for the on-path.

These map 1:1 to the contract's "Minimum bar for VL-1 to be reviewable: the
linear-vs-encoded toggle and the albedo-only all-lanes view must exist."

---

## 3. Contract-compliance review of VL-1 plan v2 (deliverable for task 4)

Reviewed strictly against the render contract — color/output only, plus the
depth/atmosphere clauses VL-1 touches. **No code, no rerank.**

| Contract clause | Plan v2 status | Verdict |
|---|---|---|
| §3.2.1 sRGB vs linear textures (albedo decode; data linear) | albedo (incl. legacy TGA) decoded; ORM/normal/mask/UI left linear (lines 63-64, 100, 108) | **PASS** (after 2.1 adds BC7 colormap) |
| §3.2.2 decode once, at sample, hardware sRGB | hardware sRGB internal formats, no manual `pow()` decode (line 55) | **PASS** |
| §3.2.4 tonemap once on linear HDR before encode | ACES forced on via gate, runs before grade (line 53, 116) | **PASS** |
| §3.2.5 encode once, manual at composite, NOT `GL_FRAMEBUFFER_SRGB` | `linearToSrgb` in `postprocess.frag` before `FragColor`; explicit no-FB-sRGB rationale (lines 74-83) | **PASS** (matches contract exactly) |
| §3.2.6 UI/HUD after encode, excluded from tonemap | UI composites to FB0 after `endScene`; not tagged sRGB; `u_viewMode==0` guard (lines 54, 118, 124) | **PASS** |
| §3.3 INV-COLOR-1 (at most one decode) | no shader manual decode alongside sRGB storage (line 55) | **PASS** — add the encode-verification check (2.3) to *prove* it |
| §3.3 INV-COLOR-2 (at most one encode) | single composite encode site; UI excluded; no FB-sRGB | **PASS** |
| §3.3 INV-COLOR-3a/3b (sceneFBO RGBA16F; no OETF on location-0 surface writes) | RGBA16F confirmed; encode only at composite, not surface shaders | **PASS** |
| §3.4 atomic decode+encode flip, default-OFF byte-identical | single `MC2_FILMIC` bundle; gate-off byte-identical at every task | **PASS** — the model the contract prescribes |
| §6.1 reverse-Z untouched | no depth changes | **PASS** |
| §6.7 MRT: composite must not corrupt COLOR1 normals / COLOR2 objectId | called out as a stop condition (line 165) | **PASS** |
| §7 INV-POST-5 (re-tune the always-on grade with VL-1) | Task 7 re-tunes sunset grade/vignette/exposure + procedural constants (lines 119, 233-238) | **PASS** |
| §11 debug views (albedo-only cross-lane; encode verify) | **MISSING** in v2 | **GAP → close via 2.3** |
| §12 stop-condition #8 (GlStateGuard on every GPU-direct pass touched) | water/terrain/prop/mech upload paths touched; plan does not name the GlStateGuard discipline | **MINOR GAP** — add a note that any touched GPU-direct upload/pass respects save/restore + `gos_InvalidateRenderStateCache()` |
| §12 stop-condition #9 (MaterialGpu/uniform upload uses `glProgramUniform*`/correct transpose) | new `enableSrgbEncode` uniform via `compositeProg_->setInt` — single program, low risk; fine | **PASS** |
| Premise: "terrain sRGB / props linear" | **WRONG** (Section 0) | **MUST FIX → §1** |

**Overall verdict: COMPLIANT-AFTER-PATCH.** The plan's architecture is contract-
correct and well reviewed; the `MC2_FILMIC` atomic bundle is exactly what the
contract demands. Two substantive items block a clean compliance sign-off:

1. **Premise + terrain BC7 bug (§1, §2.1)** — HIGH. Without it the BC7 colormap
   is undecoded under FILMIC on v0.3/sidecar deploys.
2. **Missing cross-lane debug views (§2.3)** — HIGH. Contract makes them the
   minimum reviewability bar for any color flip.

Plus one MINOR (GlStateGuard note). Everything else passes. None of these change
the task order or the gate design.

---

## 4. What this directive does NOT do

- Does not rerank VL-1..VL-6 or the V1..V11 roadmap.
- Does not propose new features or a competing plan.
- Does not edit Session A's branch (directive only; Session A applies).
- Does not touch the `MC2_FILMIC` architecture — it is endorsed.
