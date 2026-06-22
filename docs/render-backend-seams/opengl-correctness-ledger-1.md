# OPENGL-CORRECTNESS-LEDGER-1

Scoreboard for the OpenGL correctness campaign. Goal: **every known GL correctness
debt is either FIXED, PROVEN_DEAD, PROVEN_COVERED, DEFERRED_LOW_RISK with reason, a
tracked LIVE_BUG, or NEEDS_VENDOR_TEST.** Not a cleanup binge — one correctness issue
per slice, each with before/after trace, test, or reasoned proof.

Part of the RENDER-BACKEND-SEAMS arc. Sibling: `gpu-buffer-owner-recon-1.md`.

## Status taxonomy
- **FIXED** — shipped + validated; commit recorded.
- **PROVEN_DEAD** — code path has no live caller / no loader; documented, ignore.
- **PROVEN_COVERED** — already handled by existing code; evidence recorded.
- **DEFERRED_LOW_RISK** — real but low impact; reason recorded, revisit later.
- **LIVE_BUG** — confirmed correctness bug; needs a fix slice.
- **NEEDS_VENDOR_TEST** — suspected vendor-specific; needs NVIDIA/AMD run to confirm.

## Operating rules (for every slice + agent)
1. One correctness issue per slice.
2. No speculative visual changes. No PBR/color changes. No Vulkan code. No broad rewrites.
3. Every fix needs a before/after trace, a test, or a reasoned proof.
4. Every "not a bug" claim needs code-path evidence (file:line).

## Definition of done (campaign exit)
No known: live GL-state leaks · unfenced buffer reuse · missing barriers · raw-texture
binding leaks on live paths · high-severity GL debug messages · unexplained
vendor-specific disappearances. All dead/legacy paths documented or ignored.

## Work order
1. ✅ WATER-THINRING-FENCE-1 — **FIXED** `bc424dc2`
2. NVIDIA-TREE-DISAPPEAR-TRACE-2 / fix — recon in flight
3. OVERLAY-MISSING-TEXTURE-TRACE-1 / fix — recon in flight
4. TEXTURE-HANDLE-LEAK-RECON-1 — recon in flight
5. GPU-BUFFER-WRAPPER-DESIGN-1 — queued (architecture, after correctness)
6. GLSTATE-TEXTURE-ARRAY-RESTORE-1 (if still live) — recon in flight
7. PASS-DESC-POSTPROCESS-RECON-1 (dead bloom/HDR vs live) — recon in flight
8. GLSL-UB-AUDIT-2 — recon in flight
9. VENDOR-CERT-PACK-1 — capstone (run the NEEDS_VENDOR_TEST set on both vendors)

Principle: **known correctness bugs first, architecture wrappers second, fidelity features later.**

## Ledger
Row schema: `id | subsystem | risk | vendor | status | evidence | commit | test | remaining action`

| id | subsystem | risk | vendor | status | evidence | commit | test | remaining action |
|---|---|---|---|---|---|---|---|---|
| WATER-THINRING-FENCE-1 | terrain/water thin-record ring | H | both (NVIDIA-leaning) | FIXED | g_thinBuffer was barrier-only; solid ring fenced. gos_terrain_water_stream.cpp | `bc424dc2` | tier1 5/5 normal+trace; MC2_WATER_THINRING_TRACE shows waited 0→1, 0 GL_TIMEOUT_EXPIRED | none |
| RENDER-PASS-CONTRACT-ENFORCEMENT-1 | render-pass scope discipline (tooling) | — | both | FIXED | render_contract.cpp scope tracker | `8d250041` | tier1 5/5 gate on/off, 0 violations | extend scopes (ENFORCEMENT-2) |
| TREE-N8 | static-prop cull → indirect draw barrier | H | NVIDIA | PROVEN_COVERED | barrier present + intentional: `gpu_cull_compute.cpp:1297` `glMemoryBarrier(SHADER_STORAGE\|COMMAND)` after patch dispatch, before MDI draw (verified) | — | none (refutes the #1 NVIDIA-tree hypothesis) |
| TREE-N6/N7 | static-prop tree ring (align + fence) | L | NVIDIA | PROVEN_COVERED | alignment query+disarm `gos_static_prop_batcher.cpp:2622-2656`; fence ring `5479-5484`; project_nvidia_hardening_s2 | — | none |
| TREE-N2 | veg cards blockVis SSBO OOB read | M | NVIDIA | NEEDS_VENDOR_TEST | SSBO 1024 slots `gos_vegetation.cpp:228-237` vs unbounded `chunkSide²` index `gos_vegetation_card.vert:74-77`; NVIDIA robust-read→0u→clip-corner cull. ONLY live if `MC2_VEGETATION_CARDS` set | log blockCount vs 1024; run >32×32 map on NVIDIA | size SSBO to actual blockCount; bounds-guard |
| TREE-N1/N3 | veg cards instance VBO reuse / depth state | M | both | NEEDS_VENDOR_TEST | unfenced `glBufferData(GL_STATIC_DRAW)` per frame `gos_vegetation.cpp:305-307`; `GL_GREATER` vs TERRAIN_DEPTH_FUDGE `366-376` | `MC2_VEG_DEBUG_FORCE_VISIBLE=1`; depth-func override | only if veg-cards path is the reporter's symptom |
| OMT-1 | overlay/decal batch texture bind | M | HIGH (bind-0 sampling driver-defined) | LIVE_BUG (latent) | binds GL name 0 on resolve-fail: `gameos_graphics.cpp:9407,9524,9598`; `lookupBatchTextureOrWarn` null `1225-1242`. Masked by producer `overlayTexId==0` short-circuit `quad.cpp:1757` | force invalid handle; watch capped stderr "dropping invalid texture handle" | skip draw on null OR bind 1×1 white instead of 0 |
| OMT-2 | engine missing-texture magenta fallback | M | low (deterministic) | PROVEN_COVERED | fill `gameos_graphics.cpp:1185-1191`; probe wired `1198-1204` (`MC2_OVERLAY_MAGENTA_TRACE`) | `MC2_OVERLAY_MAGENTA_TRACE=1 MC2_DIAG_TAGS=OVERLAY_MAGENTA` | run trace on offending mod mission |
| OMT-3 | colormap "no-data" magenta texels | M | low (CONTENT) | DEFERRED_LOW_RISK | haze guard `terrain_overlay.frag:176-187`; only fires when `mapHalfExtent>0`. All sightings on MOD missions. Primary cause of observed magenta = content, not GL | check `getMapHalfExtent()` on failing map (0→guard dead) | confirm mapHalfExtent set; widen band only if needed |
| GLSTATE-GUARDS-1 | live draw-path GL-state restore (texunit0, 2D_ARRAY unit4, SSBO, depth/blend/cull, clipControl) | M | NVIDIA | PROVEN_COVERED | `gl_state_guard.h` RAII suite; overlay/decal/decalStatic guards `gameos_graphics.cpp:9398/9516/9589`; indirect 2D_ARRAY restore `:3888-3897/:4044-4074` | `00adb32d`,`783406a8`,`b51dbc41`,`cfa538cf`,`615ead17` | tier1 + `MC2_GL_DEBUG_FATAL=1` clean | none — no live leaks in default-on paths |
| GLSTATE-TEXTURE-ARRAY-RESTORE-1 | terrain indirect transition-mask sampler2DArray (unit4) | M | NVIDIA | PROVEN_COVERED | capture `gameos_graphics.cpp:3888-3895`, restore `:4044-4050` | `615ead17` | tier1 `MC2_GL_DEBUG_FATAL=1` | none (explicitly NOT live) |
| GLSTATE-SHADOWDEBUG-2DARRAY-1 | postprocess shadow-debug overlay leaves GL_TEXTURE_2D_ARRAY on unit0 | L | NVIDIA | DEFERRED_LOW_RISK | `gos_postprocess.cpp:2632-2645` binds tex, never rebinds 0; gated `showShadowDebug_` (default off, ImGui-only) | toggle showShadowDebug_ + `MC2_GL_DEBUG_FATAL=1` | add `glBindTexture(target,0)` at :2645 IF ever default-on |
| GLSTATE-DEBUG-CALLBACK-1 | GL debug HIGH-severity handling | — | both | PROVEN_COVERED | `gameosmain.cpp:738` print HIGH/MED, `:754-763` opt-in abort `MC2_GL_DEBUG_FATAL=1`, register `:1149` | — | tier1 `MC2_GL_DEBUG_FATAL=1` → no abort = no live HIGH msgs | none |
| POSTPROCESS-CONTRACT-OBS-1 | postprocess passes not pass-scope-tracked | L | n/a (observability) | DEFERRED_LOW_RISK | render_contract scopes only on TerrainOverlay/Decal/UI/PostProcess-outer; no inner gos_postprocess pass scopes | — | — | fold into ENFORCEMENT-2 / PASS-DESC-POSTPROCESS so the tracker can catch leaks in bloom/shadow/skybox |

| UB2-02 | mech.frag StandardLit PBR | H | both | LIVE_BUG (latent) | non-uniform branch on `flat in v_mechSunFound` wraps `texture()`+`dFdx/dFdy` (applyPbrNormal/sampleTriplanar): `mech.frag:236`, `pbr_common.hglsl:114-163`. Default-ON `mech.frag:50` | RGP/RenderDoc quad-divergence on mech silhouette edges, AMD vs NVIDIA | hoist samples/derivatives above the branch |
| UB2-01 | terrain_lod_chunk.frag detail normals + POM | H/M | both | LIVE_BUG (latent) | implicit-LOD `texture(matNormalArray)` inside per-fragment weight branches `:281-306`,`:195-201`; POM loop+break `:205-222,:265` | mip-seam A/B at material-boundary tiles | sample-always × weight, or textureGrad with pre-branch UVs |
| UB2-05 | building_pbr.frag | M | both | LIVE_BUG (latent) | conditional `discard` (ALPHA_TEST) precedes deriv+`texture()`: `:51-59`,`:35-38` | alpha-tested building edge A/B | move samples above discard / textureLod |
| UB2-04 | shadow.hglsl PCF | M | both | NEEDS_VENDOR_TEST | `dFdx/dFdy(projCoords.z)`+`texture(shadow)` after divergent early-returns `:35-48,62-73,...`. Single-mip shadow blunts real impact | force-constant adaptiveScale A/B | compute gradient before returns, or document-and-accept |
| UB2-06/07 | shadow_depth.frag / shadow_instanced.frag empty main | H(claimed)→ | AMD | NEEDS_VENDOR_TEST | empty `void main(){}` (`shadow_depth.frag:1-6`, `shadow_instanced.frag:2`); amd-driver-rules says needs explicit gl_FragDepth. **COUNTER-EVIDENCE: dev runs 7900 XTX, campaigns soak-clean WITH shadows → empirically working on AMD.** Carried unfixed since 2026-05-19 | confirm on AMD: do prop/mech shadows render? (they do today) | likely no-op; if ever broken, 1-line `gl_FragDepth=gl_FragCoord.z` |
| UB2-09 | uniform uint (cardcloud_sim.comp / particle_billboard.vert) | L | both | PROVEN_COVERED | `shader_builder.cpp:641-648` parses uint as CONSTANT_INT since VFX-FLIPBOOK; 2 live decls compile. **memory `uniform_uint_crash.md` is now STALE/over-broad** | already live | amend memory: was-crash, parse-time-fixed, int+cast now optional |
| UB2-10 | gos_grass.geom | — | — | PROVEN_DEAD | no loader (MEMORY terrain-overlay handoff) | — | none |
| TEXHANDLE-1 | raw GL texture-id extraction (gos_GetTextureGLId / gos_GetGLTextureId), 15 call sites | L | n/a (architecture, not correctness) | PROVEN_COVERED | all 15 callers hold a live gosHandle or resolve from a validated live source each frame; all glBindTexture null-safe; NO use-after-free, NO stale-id-cached-across-frames. Sites: gameos_graphics.cpp:7606/5329/2621/2550/9407/9524/9598, gos_terrain_indirect.cpp:1165/3964, gos_mech_batcher.cpp:2168, gos_static_prop_batcher.cpp:3140/7131/7518/7809 | tier1 RenderDoc | NONE for correctness. Opaque-handle migration is GpuBuffer-arc work; easy-first order: font atlas → mech diag → static-prop buildtime dedupe |

| POST-BLOOM/ACES/FXAA-DEAD-1 | postprocess bloom + ACES tonemap + FXAA | L | none | PROVEN_DEAD | bloom gated `hdrPostEnabled_&&bloomEnabled_` both default-OFF + composite force-0 `gos_postprocess.cpp:1824/1826`; FXAA `fxaaEnabled_=false` never set `:130`; shaders compiled but never dispatched | set the gates → confirm no change | safe-DELETE runBloom + bloomFBO_/textures + fxaa branch (separates dead from live; wrong-for-RTS per user) |
| POST-GODRAYS-FBBIND-1 | god-rays pass-2 FBO restore | L | none | LIVE_BUG (benign) | `gos_postprocess.cpp:1698` binds sceneFBO_, never rebinds default; endScene `:1798` corrects it → semantic gap only | RAlt+6 godrays on | 1-line `glBindFramebuffer(0)` at :1722 |
| POST-WATERREFL-PLACEHOLDER-1 | waterReflFBO_ allocated, no producer | L | none | DEFERRED_LOW_RISK | `gos_postprocess.cpp:692-749` Phase-C placeholder, reads black | — | keep + doc "producer TBD" |
| POST-STATE-RESTORE-1 | postprocess pass GL-state restore | M | both | PROVEN_COVERED | every live pass (HZB/SSAO/screenShadow/shoreline/godray) saves+restores viewport/depth/blend; endScene ends with gos_InvalidateRenderStateCache `:1917` | tier1 `MC2_GL_DEBUG_FATAL=1` | none (95% correct + cache-invalidate backstop) |

## Prioritized fix queue (distilled from all 6 recons)
Correctness bugs first, then cleanup, then architecture. Each = its own behavior-preserving, smoke-gated slice.
1. **TREE-N2** — veg-cards blockVis SSBO OOB (`gos_vegetation.cpp:228-237` 1024-slot vs unbounded `chunkSide²` index). Veg cards are SHIPPED-ENABLED. Fix = size SSBO to actual blockCount + shader bounds-guard. Correct-by-construction; no NVIDIA repro needed. **Top pick.**
2. **UB2-02** — `mech.frag` StandardLit non-uniform `texture()`/derivative (default-ON, every mech). Hoist samples above the `v_mechSunFound` branch.
3. **UB2-01** — `terrain_lod_chunk.frag` detail-normals/POM non-uniform LOD. Sample-always × weight.
4. **OMT-1** — overlay/decal bind GL texture 0 on resolve-fail. Skip-draw or bind 1×1 white.
5. **UB2-05** — building_pbr discard-before-sample. Move samples above discard.
6. **POST-BLOOM/ACES/FXAA-DEAD-1** — delete dead bloom/ACES/FXAA (cleanup, not a bug; shrinks surface; user says these are wrong for an RTS).
7. **POST-GODRAYS-FBBIND-1** — 1-line FBO-restore tidy.

NEEDS_VENDOR_TEST (park until a vendor run is possible): UB2-04 (shadow PCF derivatives), UB2-06/07 (empty shadow frags — empirically fine on dev 7900 XTX), OMT-4 (indirect cement path).
Hygiene: amend memory `uniform_uint_crash` (parse-time-fixed, rule over-broad).

## Campaign status
Of 8 reconned items: **most GL-correctness debt is PROVEN_COVERED** (GL-state guards, texture-handle leaks, texture-array restore, cull→draw barrier all already handled). Real remaining LIVE_BUGs are narrow and listed above — dominated by shader non-uniform-flow UB + the veg-cards OOB. No high-severity GL debug messages on the live path. Wrapper design (item 5) tracked separately in gpu-buffer-wrapper-design-1.md.

### Recon status — ALL 6 background agents MERGED (2026-06-21)
- ✅ item 2 NVIDIA-TREE-DISAPPEAR-TRACE-2 — lead refuted (barrier present); narrowed to veg-cards path.
- ✅ item 3 OVERLAY-MISSING-TEXTURE-TRACE-1 — magenta = CONTENT (mod colormaps); latent GL bug OMT-1.
- ✅ item 4 TEXTURE-HANDLE-LEAK-RECON-1 — 15 sites all PROVEN_COVERED, 0 correctness bugs.
- ✅ item 6 GLSTATE / GL-state-leak audit — gl_state_guard.h suite covers it; no live leaks.
- ✅ item 7 PASS-DESC-POSTPROCESS — bloom/ACES/FXAA PROVEN_DEAD; state restore covered.
- ✅ item 8 GLSL-UB-AUDIT-2 — the real findings (UB2-01/02/05 non-uniform flow).

### Resolved: NVIDIA-tree config fork
User confirmed `MC2_VEGETATION_CARDS` is **shipped-ENABLED** (but not on user's own machine; can't repro NVIDIA easily). → veg-cards path is live → **TREE-N2 (blockVis SSBO OOB) is the lead**, and is fixable correct-by-construction without an NVIDIA repro. Static-prop path remains PROVEN_COVERED.
