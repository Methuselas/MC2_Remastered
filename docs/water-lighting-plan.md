# Water First-Visual Plan (WATER-LIGHTING-PLAN-0)

Plans the first real water visual improvement now that debug views
(WATER-DEBUG-VIEWS-1) and live tuning (WATER-TUNING-UI-1) exist. **Docs-only.**

Lane: `claude/water-rv-lane`. HEAD `a096fcab`. Target FS:
[`shaders/gos_terrain_water_mdi.frag`](../shaders/gos_terrain_water_mdi.frag)
(the modern water-v1 MDI path; the only V-lane target). Pairs with
[`docs/water-rv-arc-recon.md`](water-rv-arc-recon.md).

## TL;DR — recommendation

**First visual slice = a gated, camera-INDEPENDENT sky/horizon tint** (a small
additive pull of the water color toward a sky/horizon color), **default gate
OFF / strength 0** (exact no-op until toggled), live ImGui slider in the Water
panel. Tiny, low-risk, no new view/resource dependencies, no reflection.

**NOT fresnel.** True fresnel is view-angle dependent → it re-introduces the
camera-dependence the user explicitly **rejected on 2026-05-17** (the reason
the S3 reflection block is dead-stripped). Fresnel stays shelved unless the
user re-opens that decision (see §2).

This slice **qualifies as tiny/low-risk**, so WATER-VISUAL-FIRST-SLICE is
authorized to proceed (per the Batch-2 brief).

## 1. First-visual target — options weighed

Ranked by (visual value × safety), respecting the camera-independence ruling
and the shoreline z-fight known issue:

| Option | Value | Risk | Camera-indep? | Verdict |
|---|---|---|---|---|
| **A. Sky/horizon tint** (additive pull toward a sky color, gated) | modest | **low** | YES | **RECOMMENDED first slice** |
| B. Fog/sky-color coherence with terrain | low (correctness) | low | YES | Good follow-up; more a parity fix than an improvement |
| C. Ripple / glint strength | — | — | YES | **Already shipped** as live tuning (WATER-TUNING-UI-1); not a new slice |
| D. Alpha / depth absorption | — | — | YES | **Already tunable** (WATER-TUNING-UI-1); not a new slice |
| E. Shoreline blend | medium | **high** | YES | Touches the z-fight-sensitive shore band ([known_issues.md:34](known_issues.md)); defer |
| F. Fresnel tint | high | med | **NO** | **Blocked** by the camera-independence ruling (§2) |
| G. SSR / cubemap / scene-color refraction | high | high | NO | **Out of scope** (Batch-2 hard constraint) |

Rationale for A: water-v1 today is a Beer-Lambert deep↔shallow gradient + fBm
ripple. Against a bright sky it can read slightly dark/heavy at the surface.
A small constant lift toward the sky/horizon color (camera-independent, f() of
nothing but a uniform color + strength) makes the surface sit more naturally in
the scene **without** any view-dependent term, any reflection, or any geometry
change. It composes with the existing `SKY_AMBIENT` floor already in the FS.

## 2. Revive the dormant S3 fresnel/reflection scaffolding?

**No — not in the first slice, and not without a user decision.** The S3 block
(`gos_terrain_water_mdi.frag:132-161`, `S3_REFLECTION_ENABLED=false`) is
compile-time dead by deliberate ruling: *"any perceptible camera-dependence in
the water was rejected (user 2026-05-17)… a reflection is inherently
camera-dependent, so it cannot satisfy that."* Schlick fresnel
(`pow(1 - max(vdir.z,0), 5)`) is camera-dependent for the same reason.

If the user later **re-opens** camera-dependence, the scaffolding (uniforms,
C++ bind, atlas plumbing, probe `MC2_WATER_REFL_TRACE`) is retained dormant and
revivable as a *separate, explicitly-approved* slice — not folded into the
first visual slice. The first slice must stay inside the standing ruling.

## 3. Global vs per-mission

**Global for now.** All water shares one palette today (no per-biome data;
recon §1). The new tint is a global uniform + ImGui knob, exactly like the
WATER-TUNING-UI-1 material params. Per-mission/per-biome water material is a
larger arc ("water-v2" per the FS spec comment) that needs a data source
(`.fit` keys or a per-mission water material block) and is **out of scope**.
The tint uniform is structured so a future per-mission system can override it
without a shader change.

## 4. Does water need ViewUniforms first?

**No.** A constant additive sky tint needs no view matrix and no camera angle
(camera-independent by design). Water already has `cameraPos` (used only for
`waveLOD` distance fade, not angle). The ViewUniforms migration (water is 100%
legacy `u_worldToClipGL`; recon §5) is a worthwhile **Track-R** cleanup but is
**not a prerequisite** for this visual slice and should be its own lane item.

## 5. Does water need RenderResourceRegistry entries?

**No** — not for this slice. A sky tint adds a uniform, not a resource. The
recon-noted candidates (recipe/thin/window/bucket SSBOs) remain a separate
Track-R hygiene item with no bearing on the first visual.

## 6. Gate / env name for the first visual improvement

- Env: **`MC2_WATER_SKYTINT`** (default unset/OFF). Register in
  `check-env-registry.sh` + `docs/tier1_env_vars.md`.
- Runtime: a strength scalar **`u_waterSkyTintStrength`** (default **0.0** =
  exact no-op) + a sky-tint color **`u_waterSkyTintColor`**, both uploaded in
  the MDI bind block; live ImGui slider/color in the Water panel.
- Default-OFF semantics: strength 0 → the tint term contributes nothing →
  **byte-identical** to current water. The env gate flips the *default*
  strength to a small nonzero (e.g. 0.15) for quick on/off A/B, but the knob is
  authoritative once touched (mirrors the terrain NfH gate+slider pattern).
- **No feature default flip**: strength stays 0 / gate OFF at rest.

## 7. Capture matrix that proves it

Using the WATER-BASELINE-0 tooling (`water_final_24` preset, mc2_24 armed):

| Capture | Flags | Proves |
|---|---|---|
| default | (none) | unchanged baseline (byte-identical to pre-slice) |
| gate ON | `MC2_GPU_DRIVEN_WATER=1 MC2_WATER_SKYTINT=1` | the tint visibly lifts surface color |
| debug Tint | `…=1 MC2_WATER_DEBUG_MODE=1` | tint enters the base-color term, not ripple/alpha |
| debug Lighting | `…=1 MC2_WATER_DEBUG_MODE=6` | ripple/glint term **unchanged** by the tint |

Plus: tier1 5/5 gate OFF (byte-identical); mc2_24 gate ON `gl_errors=0`,
`WATER_MDI prog` compiled; shader_reflect golden refresh (new uniforms);
env_registry PASS. **Caveat:** pixel screenshots need the desktop not-foreground
(see [[capture-baseline-foreground-race]]); otherwise rely on smoke + the user
driving the camera for the A/B.

## 8. Explicitly out of scope (first slice)

- Screen-space reflection, cubemap reflection, scene-color refraction.
- Any camera-DEPENDENT term (fresnel, reflections) — blocked by the 2026-05-17
  ruling unless separately re-opened.
- Water geometry / wave-shape changes; shoreline depth-bias rewrite (z-fight).
- Shoreline blend changes (deferred — high risk near the z-fight band).
- Per-mission/per-biome water material (water-v2 arc).
- ViewUniforms migration, RenderResourceRegistry entries (separate Track-R).
- Default flips, legacy `gos_tex_vertex.frag` visual work, post-process/tonemap.

## 9. Risk verdict for WATER-VISUAL-FIRST-SLICE

**Tiny / low-risk → authorized.** It is: one additive term in one FS, behind a
strength scalar that defaults to 0 (exact no-op), camera-independent (honors the
standing ruling), gated by a new env, no new view/resource deps, no reflection,
no geometry, no default flip. Validation is the §7 matrix. If implementation
surfaces any coupling to the shore band or depth bias, **stop** — that exceeds
"tiny."

---

**Status:** docs-only. Slice 2 of Batch 2 (WATER-LIGHTING-PLAN-0). Recommends
proceeding to WATER-VISUAL-FIRST-SLICE = gated camera-independent sky tint.
