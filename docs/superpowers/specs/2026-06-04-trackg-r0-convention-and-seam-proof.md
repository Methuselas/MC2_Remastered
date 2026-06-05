# TRACK G — R0: convention freeze + staticprop render-seam proof

Date: 2026-06-04. Mandatory STOP gate for `TRACKG-OFFLINE-GLB-COOK-MANIFEST-1`
(plan: `docs/superpowers/plans/2026-06-04-trackg-offline-glb-cook-manifest-plan.md`).

**Verdict: R0 PASS (proceed to G3a).** The staticprop/building override class renders
in-game; the import convention is frozen; the canonical class spelling is proven against
the real registry. One caveat: the render-proof uses verified on-branch evidence rather
than a fresh current-HEAD run (a concurrent session held the shared runtime — see §6).

---

## 1. Required artifacts (all five)

| # | Artifact | Status | Evidence |
|---|---|---|---|
| 1 | Convention note committed | ✅ | this file |
| 2 | bigbox visible (screenshot/log) | ✅ | `docs/assets/trackg-r0/r0_hangar_override_after.png` — solid override box drawn on the apron where the stock `hangar` stood |
| 3 | bounds/orientation compare | ✅ (placeholder-appropriate) | box on-ground, upright, plausible size (§4). Full stock-footprint parity deferred to 2civliving by design |
| 4 | registration log proving staticprop override path | ✅ | branch logs: `[MODOVERRIDE] staticProp 'hangar': render override applied`; batcher draws the override type (post seam-fix). §3 |
| 5 | class spelling proven vs registry | ✅ | canonical lowercase `staticprop`; case-insensitive accept (§5) |

**A/B (same camera), branch `claude/model-override-system-recon-1`:**
- BEFORE — `docs/assets/trackg-r0/r0_hangar_override_before.png`: empty apron (stock hangar
  displaced by the override; box not yet drawn).
- AFTER — `docs/assets/trackg-r0/r0_hangar_override_after.png`: same camera, a solid orange
  override box rasterized in that spot.
- `r0_magenta_prefix_norender.png`: the earlier broken state (override resolved+imported but
  geometry NOT drawn) — kept to show what a seam regression looks like (the STOP signal).

---

## 2. Convention freeze (DECISION, per plan Patch 2)

**The runtime importer `mclib/assimp_importer.cpp` is the sole authority.** Its default-env
behavior:
- axis: `MC2_GLTF_AXIS=0` → `(-x, -y, z)` (`assimp_importer.cpp:60-69`)
- V: `toMC2V(v) = 1.0 - v` flip APPLIED (`:82`, used `:426`)
- ground/offset: `MC2_GLTF_GROUND` default 2, `MC2_GLTF_YOFF` default 0

**Decision:** the offline cook **bakes orientation/axis into the cooked GLB** so that import
under DEFAULT environment (no `MC2_GLTF_*` set) is correct. The manifest records
`geometry.convention = {axis:0, vflip:true, importer:"assimp_importer.v1"}` **for audit
only**. A cooked asset MUST NOT depend on any non-default `MC2_GLTF_*` env to look right —
runtime env overrides are NOT part of the cooked-asset contract.

**Workbench divergence (advisory):** the asset-viewer `GlbMeshLoader` uses a different chain
(two-step `srcToGl`, relies on Assimp's auto-V-flip, no manual `1-v`) — see
`tools/asset_viewer/GlbMeshLoader.cpp`. Its preview orientation MAY differ from runtime. The
workbench preview is advisory; the runtime importer is truth. Reconcile (make the viewer
match the runtime importer) or clearly label "preview ≠ runtime orientation" — tracked as a
follow-up, not an R0 blocker.

---

## 3. Staticprop render-seam — wired + proven

The five seams a cooked staticprop asset plugs into (all present on the branch, all
exercised by the hangar→bigbox proof):

1. **Override import at appearType init** — `bdactor.cpp:344-390` (bldg/staticProp): registry
   `resolve("staticprop", name)` → `ImportGeometryFromFile` (try/catch → stock fallback) →
   `LoadOverrideRenderShapeTextures`.
2. **Register BEFORE finalize** — `bdactor.cpp:2934 registerStatic()` →
   `registerMultiShape(getBldgRenderShape(i), isOverride=true)` in the mission-load pre-pass,
   before `finalizeGeometry` freezes the VBO. **This was the load-bearing fix**: the original
   failure (`r0_magenta_prefix_norender.png`) was registration AFTER finalize → override type
   `late registerType … CPU-fallback`, `gpu_drawn_instances=0`. Pre-register fixed it.
3. **isStaticEligible bdAnim skip** — `bdactor.cpp:2904`
   `overrideStatic = bldgRenderShape && !bldgTypeHasAnimations` admits the override recipe
   past the animating-guard.
4. **layerForPacket override route** — `gos_static_prop_batcher.cpp:3043-3089`:
   `type.isOverride && !uniques.empty()` routes the NULLTXM packet to layer 0 (visible, borrowed
   albedo — hence the orange box) instead of the `layer=-1` skip.
5. **GPU-INSTANCE-SKIP-POOLS** — `bdactor.cpp:2501` zero-pool walk for registered types.

`MC2_MODOVERRIDE_TRACE=1` lists 56 static props + 11 trees loaded on mc2_01; a 14-prop
override manifest produced `render override applied` for all 14 (crateswithtarp, dumpster,
geodesicdome, hangar, hqtent, junkpile, lookouttower, Quonset, quonset2, sandbagbunker,
sandbagwall, tent, wirefence, woodencrates) — these staticprop appearance names are confirmed
present in mc2_01 and available as cook targets.

**Conclusion: the staticprop class is NOT blocked (STOP #1 cleared).** The seam renders;
texture binding is the remaining polish (G2), not a seam gap.

---

## 4. Bounds / orientation

`bigbox.glb` is a placeholder unit box (`data/model_overrides/source/props/bigbox.glb`, 1936 B).
Stock-footprint parity is intentionally NOT expected for a placeholder (a box ≠ a hangar) —
that gate belongs to the real-asset slice (2civliving, G-E2E). For R0 the orientation check is
visual: the box renders **on-ground, upright, axis-correct, plausible scale** (no mirror, no
upside-down, no float, no giant/tiny) — confirmed in `r0_hangar_override_after.png` and the
HEAD commit ("box on-ground"). The `TglMeshLoader`-oracle footprint/pivot parity test is wired
in G1 and asserted in G-E2E Slice B.

---

## 5. Class spelling — proven against the real registry (plan Patch 5)

`mclib/model_override_registry.cpp`:
- `normalizeKey()` lowercases every key char (`:23`).
- accept compare is against the literal lowercase `"staticprop"` (`:77`).
- `resolve()` normalizes its input class+name too (`:150`).

**Canonical = lowercase `staticprop`** (and `tree`). **Input is case-insensitive**:
`staticProp` / `StaticProp` / `STATICPROP` are all accepted → normalized to `staticprop`.
The camelCase `staticProp` seen in some docs is ONLY the `logDrop` message string at `:77`
("class not staticProp|tree") — cosmetic, not the stored/accepted key.

**Locked for G3a:** schema `class` field `enum: ["staticprop","tree"]`; the cook MUST emit
lowercase; `validate_manifest.py` rejects non-lowercase in emitted manifests; a registry unit
test asserts mixed-case input resolves (the case-insensitive accept) while the manifest writer
always produces lowercase.

---

## 6. Caveat — render-proof provenance + no-central-write

- **Provenance:** the render-proof is verified **on-branch** evidence (A/B screenshots +
  documented `MODOVERRIDE` logs from `MODEL-OVERRIDE-GPU-BATCHER-SEAM-PROBE-1`), NOT a fresh
  run on the literal current build exe. Reason: at R0 time a concurrent session held the
  shared runtime (`mc2.exe` PID running, v0.4 deploy exe being rewritten live), so swapping
  the v0.4 exe / taskkilling would have disrupted it. The current build exe
  (`build64/RelWithDebInfo/mc2.exe`, Jun 4 16:12) postdates the seam fix and HEAD `ef05919a`
  is the validated commit → high confidence.
  - **✅ RE-STAMP DONE (2026-06-04, runtime free):** fresh `--validate --frames 30 -mission mc2_01`
    on the current override build (+ the G5 log), cooked-bundle entry
    `staticprop:hangar → cooked/bigbox/bigbox.glb`. Result: `[MODOVERRIDE] staticProp 'hangar':
    render override applied (…/cooked/bigbox/bigbox.glb)`, `[RENDER_PATH v1] key=staticprop:Hangar
    isOverride=1 path=override_multidraw pools=skipped`, validate.json exit 0 / gl_errors=[] /
    shader_errors=[]. Also empirically confirmed case-insensitive class/name accept (`Hangar` ↔
    `hangar`). v0.4 restored to as-found. Evidence: `docs/assets/trackg-r0/restamp_g5_e2e_evidence.txt`.
    Caveat CLOSED.
- **No-central-write (clean):** R0 did NOT modify the deploy central manifest
  `A:/Games/mc2-opengl/mc2-win64-v0.4/data/model_overrides/models.json`. The render evidence
  predates this session; this session only READ deploy data. Consistent with STOP #5.

---

## GO decision

R0 PASS. The staticprop seam renders, the convention is frozen, the class spelling is proven.
**Proceed to G3a (manifest schema first — the contract).** Carry forward: the runtime-importer
convention freeze, `enum:["staticprop","tree"]`, the `TglMeshLoader` parity oracle for G1, and
the free-runtime re-stamp as a non-blocking follow-up.
</content>
