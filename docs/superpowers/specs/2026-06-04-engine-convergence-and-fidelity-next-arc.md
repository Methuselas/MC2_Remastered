# Engine Convergence + Visual Fidelity — Next-Arc Scoping (2026-06-04)

Status: scoping doc. The forward delta over the two living roadmaps.
Supersedes neither — it points into both:

- `2026-05-22-engine-convergence-roadmap.md` (20-item engine-as-API tally,
  P2 meta-fixes, Tier ladder, S1–S11 pillars) — now backfilled with the
  2026-06-04 ship-log.
- `2026-05-22-visual-fidelity-roadmap.md` (Track V V1–V11) — now backfilled
  with a 2026-06-04 status table.
- `observations/2026-05-14-render-pipeline-matrix.md` + `observations/2026-05-25-pipeline-master-index.md`
  (the render-pipeline contract map) — master-index now committed; matrix
  Status column flagged stale.

---

## 0. Why this doc exists

The two roadmaps are ~50% done, not greenfield. The honest framing matters:
the value now is **separating shipped from frontier**, not re-listing
everything as "next." June 2026 closed the perf debt that was blocking a
feature/authoring push. This doc is the completion-true map of what actually
remains to reach the target.

**Target, read correctly:** NOT Unreal/Unity-the-product. The lane (per the
convergence roadmap's "Project identity") is a *modern open-source
RTS-specialized 3D engine*. "Unreal/Unity-like" decomposes to three concrete,
bounded gaps:

1. **Authoring/pipeline** — import any DCC mesh → cooked asset with
   LODs/materials/capability flags → drop into the world. (Track G.)
2. **Features** — UE4/MW5-era visual consistency: PBR + IBL + post-stack +
   stable shadows + decals + particles. (Track V.)
3. **Inspectable engine** — click a pixel, see what rendered it; an engine you
   can debug like a DCC tool. (Track E, riding Object-ID + manifest.)

Everything UE5 (Nanite, Lumen, HW-RT, virtual textures, mega-streaming, full
editor, Vulkan backend) stays in negative space (§7).

---

## 1. What June closed (state of arc)

A perf-debt closeout. Multi-second → sub-ms across every static subsystem,
which is what unblocks the feature push (you cannot add HDR/SSAO/decals on top
of a frame that is already 5.8s):

| Win | Before → After | Where |
|---|---|---|
| Foliage impostor far-LOD | `Render.GpuStaticProps` 6.77s→161µs (~40,000×); frame 5.84s→55ms | `01f3c1b6..d12f7c2b` |
| quadSetupTextures + GPU water recipe | per-quad loop 1.01ms→~0.6µs; CPU water walk retired | nifty `6ecace0a` |
| Static-prop snapshot fill dirty-only | `ExtractRenderSnapshot` 1.68ms→36.7µs | nifty `cf654080` |
| Permanent instance lights + upload split | `LightDataUpload` 224µs→7µs | nifty |
| DrawPacket v8 | static-prop live-builder retired; snapshot sole owner | nifty `3f9f1da2` |
| GOM readback shadow-copy | BAR-read stall 10ms→360ns | SHADOW-COPY-1 |
| Tier-3 cleanup | rain ~500 draws→1; DynamicShadowPass 192µs→40µs | nifty `a23250ef` |
| Model-override system | glTF import → texture bind → register → textured trees render | branch `c1fbc9ac` |

**The model-override system is the sleeper.** It already does
glTF-import → MC2-texture-binding → registry → GPU-instanced render, end to
end, for trees. That is the Track G asset path in prototype. Neither roadmap
captured this. It is the beachhead (§3, Arc G).

---

## 2. Completion ledger (the ~50% view)

Cross-track, honest. **DONE** = shipped + default-on or substrate-complete.
**PARTIAL** = foundation shipped, finish pass remains. **NEXT** = genuine
frontier, not started.

### Track R — engine architecture (most complete: ~75%)
- **DONE:** RenderWorld API (StaticProp+Mech), Object-ID buffer (fully
  realized), DrawPacket v8, PipelineDesc + PassContract registry, FeatureRegistry
  (COUNT=30) + CI gate, FrameArena, Render/View/Debug registries, snapshot
  extraction v3 + dirty-only.
- **PARTIAL:** extraction authority-flip (snapshot proven, not yet primary),
  multi-view (registry records views, does not dispatch through them).
- **NEXT:** `RenderWorld::render()` single frame entrypoint; sim/render interp
  (item 17, trivial after authority-flip); FROZEN-STATIC-CULL-RECORDS metafix
  (in execution on `claude/perf-gpucull-ownership-1`).

### Track G — authoring/cook pipeline (least complete: ~15%, the long pole)
- **DONE (vendor):** Assimp import phase 0 (`889cebff`), meshOptimizer
  (`774f074a`), gltfpack build (`.claude/gltfpack_build`). Tree LOD bake +
  impostor bake proven (`tree_lod_bake.py`, `tree_export_impostor.py`).
- **PARTIAL:** model-override glTF import path (works for trees; load-time, not
  offline-cooked).
- **NEXT:** offline GLB staging, KTX2 cook, `manifest.json` (P2-9), `.cdag`
  meshlet/LOD sidecar, MeshCapability + `[RENDER_PATH v1]` (P2-3/P2-4).

### Track V — visual fidelity (~50%)
- **DONE:** terrain PBR splat (reference), static-prop MaterialGpu default-on,
  HDRI sky, colormap KTX2 atlas, foliage impostor (V8, early).
- **PARTIAL:** V1 (mech sampling blocked), V2 (sky+SH substrate, not object IBL),
  V3 (shadows work, no stable-CSM), V6 (bake only), V7 (GPU particles, no
  soft/lit), V9 (splat only), V10 (substrate, no inspect-UX).
- **NEXT:** **V4 HDR post-stack** (doc's own #1 ROI), **V5 SSAO**, V11 reactive
  surface (hero, late).

### Track E — editor/authoring UX (~25%, rides behind G)
- **DONE:** asset-viewer stage 1 (`mc2_asset_viewer` exe), ImGui inspector
  windows (Renderer Features, Render Explain), Object-ID pick substrate.
- **PARTIAL:** DebugRenderer M1 (`flushWorldPrims`).
- **NEXT:** Object-ID click-inspect workflow (V10), DebugRenderer M2, asset-viewer
  stage 2 (spec ready), manifest-inspector (needs G3), Blender round-trip.

---

## 3. The four arcs — genuine remainder only

### Arc R — frame-ownership completion (steady cadence, parallel)
The Vulkan-tomorrow spine. Low blast radius, infra-paced.
- **R-flip** — extraction authority-flip: snapshot becomes primary for static
  props, then mechs. (Item 3.)
- **R-view** — multi-view dispatch: `setCurrentView(kShadowDirectional0ViewId)`
  around `flushShadow()`. First real dispatch through the view registry. (Item 7.)
- **R-metafix** — **FROZEN-STATIC-CULL-RECORDS** (in execution,
  `claude/perf-gpucull-ownership-1`): frozen pool-aligned cull records so
  `record-index == pool-slot`, dissolving the H4 bimodal cull divergence and
  retiring the per-frame static-cull rebuild (the CPU win). M1 = substrate order
  reversal (frozen static prefix `[0,S)` + dynamic `[S,S+D)`). Run
  `adversarial-plan-review` before coding the reindex.
- **R-entry** — `RenderWorld::render()` single frame entrypoint (gated on
  R-flip + R-view + DrawPacket). (Item 20.)

### Arc G — authoring/cook pipeline (KEYSTONE, sequential, gated)
The literal "Unreal/Unity authoring/pipeline" gap. One asset end-to-end before
generalizing. **Capping principle: data before behavior — manifest before
loader, capability before fallback.**
- **G1** — Assimp → canonical GLB offline staging (wire the vendored importer to
  an *offline* path, not load-time; staging dir, not git). (Item 8 / Simpl. 1.)
- **G2** — gltfpack / glTF-Transform / KTX2 cook, one building end-to-end.
- **G3** — `manifest.json` per asset: source, cookVersion, meshCapabilities,
  materials, bounds, deps. **The link between cook and RenderWorld handle
  creation** — write this early, it is the contract. (P2-9.)
- **G4** — meshOptimizer LOD chain + meshlets for one building, `.cdag` sidecar.
  Generalizes the proven tree LOD/impostor bake into the asset path. (Item 8.)
- **G5** — MeshCapability flags at import → `[RENDER_PATH v1]` capability log;
  renderer chooses path from capability, not env-flag. Kills the `MC2_*` maze as
  architecture (flags demote to overrides/diagnostics). (P2-3/P2-4/P2-10.)
- **Beachhead:** promote the model-override import into the canonical G path —
  generalize from trees to all static props. The hard part (glTF→texture→render)
  is already working.

### Arc V — visual post-stack (parallel, highest visible ROI, cook-independent)
Track R prereqs are satisfied → V proper is unblocked. Order by ROI:
- **V4** — HDR stack: RGBA16F scene → bloom (half-res ping-pong) → ACES tonemap
  → grade → gamma → post-tonemap UI. Extend the existing post-FBO; do not rewrite.
  **Single highest visual jump.**
- **V5** — SSAO/GTAO-lite: half-res 16-tap, normal-aware bilateral blur, consume
  ViewUniforms. New path — do NOT resurrect the legacy SSAO. (Default OFF first.)
- **V2-Ph0** — wire object IBL: global cubemap + irradiance SH + BRDF-LUT,
  reusing the shipped HDRI sky + SH-L2 substrate. Makes PBR read *correct*, not
  just different.
- **V1-finish** — MaterialGpu → mech sampling (resolve the texture-model
  decision first); terrain-mat V2 (macro variation / detail normals / slope blend).
- **V3** — stable CSM: texel-snap, fixed light-space pivot, per-cascade debug
  view. The shadow work is half-done; this formalizes cascades.
- **+New reasonable (see §5):** V6 decal *lifetime* system; S5 TAA once
  HDR + ViewUniforms land.

### Arc E — editor/authoring UX (rides behind G + Object-ID)
The "inspectable engine" gap. ~60% of editor value without a full editor.
- **E1** — Object-ID click-inspect (V10): pixel → handle / mesh / material / LOD
  / pipeline / draw-packet / render-path / visibility-source. Substrate already
  exists; this is a consumer.
- **E2** — DebugRenderer M2: per-handle inspection, selection bracket, name
  overlay, consuming `[GAMEPLAY_PICK v1]`.
- **E3** — asset-viewer stage 2 (spec ready, `claude/asset-viewer-stage2`).
- **E4** — manifest-inspector: viewer reads the G3 `manifest.json` → asset-database
  view. (Gated on G3.)
- **E5** — Blender round-trip: export MC2 → glTF → author → re-cook. The
  authoring loop. (Gated on G; collaborator-facing.)

---

## 4. Sequencing — three parallel lanes

```
Lane 1 (KEYSTONE, sequential)   Arc G:  G1→G2→G3→G4→G5   [long pole; gates E4,E5,V8-general]
Lane 2 (quick visible wins)     Arc V:  V4→V5→V2-Ph0→V1-finish→V3   [cook-independent; ship now]
Lane 3 (infra cadence)          Arc R:  R-flip, R-view, R-metafix → R-entry
Behind Lane 1                   Arc E:  E1,E2,E3 (now) → E4,E5 (after G3)
```

Dependency edges that actually bind:
- `V8 generalization` → needs `G4` (.cdag LOD) + `G5` (HasImpostor capability).
- `E4 manifest-inspector` → needs `G3`.
- `R-entry` → needs `R-flip` + `R-view`.
- `V11-S0` → needs `V1` finish; `V11-S1` → needs `V7` soft/lit.
- Everything else is parallelizable. V4/V5/E1 can start immediately.

**Capping principle (verbatim, load-bearing):** *"Manifest before loader.
Capability before fallback. Object-ID before editor. MaterialGpu before PBR
polish. Pass contract before render graph. DrawPacket before backend.
PresentationBand before zoom hacks."* → data first, behavior second, pretty third.

**Recommended immediate picks (next 2–3 slices):**
1. **V4 HDR post-stack** — highest visible ROI, zero dependencies, unblocked.
2. **G1+G2+G3** — start the keystone; manifest is the contract everything waits on.
3. **R-metafix** (already in flight) — finish FROZEN-STATIC-CULL-RECORDS.

---

## 5. What else is reasonable (delta beyond the prior roadmaps)

New since the 2026-05-22 docs — surfaced by the June ships + this re-scope:

1. **Model-override system = the Track G prototype.** Name it explicitly: the
   override import path is the cook pipeline in miniature. G generalizes it
   (trees → all static props) rather than building import from scratch. Biggest
   single de-risking insight.
2. **Generated contract-map status (meta-fix).** The `render-pipeline-matrix`
   Status column drifts because it is hand-maintained. Tie it to FeatureRegistry
   so default/experimental/shipped status is *generated*, not edited. One owner
   for "what path is default." Kills the staleness class the backfill just had
   to fix by hand.
3. **`[RENDER_PATH v1]` capability log** as the bridge between cook
   (MeshCapability) and renderer (path decision) — closes P2-3/P2-10 and makes
   "why did this asset take the legacy path?" answerable from a log line.
4. **V6 decal *lifetime* system** (not just bake) — scorch / track / impact with
   bounded lifetime + material-ID + object-ID exclusion. High RTS-visible ROI;
   the bake foundation already exists. Underweighted in the prior doc.
5. **S5 TAA / temporal stack**, once HDR + ViewUniforms land. The temporal
   multiplier that makes half-res SSAO (and future SSR) clean. Genuinely
   UE4-tier; reasonable mid-term, not a UE5 reach.

---

## 6. Negative space (reaffirmed — do NOT build)

Unchanged from the convergence roadmap's 12-item list. The "Unreal/Unity-like"
framing does NOT license any of these:

full ECS · full render-graph scheduler · full Vulkan backend (until OGL
saturated) · full Nanite/virtual-geometry · runtime glTF (cook offline always) ·
streaming/residency (until LOD assets exist) · virtual texturing / terrain
clipmaps · deferred-renderer rewrite (forward/forward+ + material tables suffice) ·
full editor (Object-ID + DebugRenderer + manifest = ~60% of the value) ·
GPU-driven-everything · general job system first · hero weather/heat FX before
PBR + particles.

---

## 7. Pointers

- Engine-as-API tally + P2 meta-fixes + Tier ladder + S1–S11:
  `2026-05-22-engine-convergence-roadmap.md` (2026-06-04 ship-log at top).
- Track V V1–V11 + status table:
  `2026-05-22-visual-fidelity-roadmap.md` (2026-06-04 status backfill at top).
- Render-pipeline contract map: `observations/2026-05-14-render-pipeline-matrix.md` (submission-path /
  ownership / lifecycle; Status column stale — re-validate vs ship-log) +
  `observations/2026-05-25-pipeline-master-index.md` (frame spine + binding-point
  quick-ref) + per-subsystem maps `observations/2026-05-25-*-pipeline-map.md`.
- Blender authoring loop roadmap: `memory/roadmap_blender_authoring_loop.md`.
</content>
