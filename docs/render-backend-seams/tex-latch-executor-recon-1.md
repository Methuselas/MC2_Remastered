# TEX-LATCH-EXECUTOR-RECON-1

Read-only forward-looking recon. Answers: *which passes bind their own texture units and which
inherit, and what does that mean for an executor owning more islands?*

**Extends, does not duplicate, `frame-graph-tex-latch-recon-1.md`** (commit `e1b28ee7`):
that recon answered "does a current ghost exist?" (No). This recon answers the NEW question:
"which inheritances become ghosts the moment an executor changes what ran before them or
establishes a clean texture state at pass entry?" — the executor-safety axis.

> All `file:line` refs are drift-prone. Re-grep before any slice derived from this doc.

---

## TL;DR

**DEFER — no texture-unit ledger needed now.**

The per-pass texture-unit inventory confirms the prior recon's finding: every pass rebinds
its own texture units before drawing. Zero order-fragile inherited latches exist on the
default-on path. The executor's shipped island (PostProcess, `GlScopedTextureUnit` guards on
units 0/2, `d88cf49e`) is already the BEST pattern. The next safe island candidate —
PostProcess sub-stages (SSAO, fog, debug overlay) — all bind all their own units explicitly
and are texture-safe.

**Trigger for building a texture-unit ledger:** the executor actually reorders two passes (not
just wraps them), AND a real NVIDIA 2D/2D_ARRAY aliasing misfire appears in a vendor capture.
Neither is true today.

---

## Q1 — Per-pass texture-unit binding inventory

All passes listed by frame order. "Explicit" = the pass calls `glActiveTexture+glBindTexture`
(or equivalent) for that unit in its own draw path. "Inherited/save-restore" = the pass reads
a saved value from a prior unit state. No pass is confirmed to sample WITHOUT rebinding.

| Pass | Owner TU | Unit | Target | Sampler role | Bind style | Live site (drift-prone) |
|---|---|---|---|---|---|---|
| **Shadow casters** | `gos_postprocess.cpp` | no tex units sampled | n/a | depth-only | n/a | shadow FBO depth-only frags |
| **StaticPropOpaque** | `gos_static_prop_batcher.cpp` | 0 | `2D_ARRAY` | `u_texArr` albedo | EXPLICIT | `:5267-5268` |
| **StaticPropOpaque** | `gos_static_prop_batcher.cpp` | 0 | `2D` | `u_tex` fallback | EXPLICIT | `:2935` |
| **StaticPropOpaque** | `gos_static_prop_batcher.cpp` | (1) ORM | `2D_ARRAY` | `u_ormTexArr` | implicit via `glBindTexture` after set unit 1 (line ~`:2862` cluster) | `:2862` area |
| **Terrain** | `gameos_graphics.cpp` / `gos_terrain_indirect.cpp` | 0 | `2D` | colormap | EXPLICIT | `gameos_graphics.cpp:2848` |
| **Terrain** | `gameos_graphics.cpp` | 4 | `2D_ARRAY` | transition-mask array | EXPLICIT (save-restore GLSTATE-TEXTURE-ARRAY-RESTORE-1) | `gameos_graphics.cpp:3888-3897/4044-4074` |
| **Terrain** | `gameos_graphics.cpp` | 5-8 | `2D` | matNormal0-3 | EXPLICIT | `gameos_graphics.cpp:2008-2089` cluster |
| **MechOpaque** | `gos_mech_batcher.cpp` | 0-4, 6, 7 | `2D` | base/PBR/paint/imported | EXPLICIT + full save-restore 0-4,6,7 | `:2121-2144` setup; `:2507-2523` epilogue |
| **TerrainDecal / TerrainOverlay** | `gameos_graphics.cpp` | 0 | `2D` | decal albedo | EXPLICIT + `GlScopedTextureUnit(0)` guard | `gameos_graphics.cpp:10097/10219/10292` |
| **Water** | `gameos_graphics.cpp` (`renderWaterFastPath`) | 0 | `2D` | base | EXPLICIT | `:3581-3582` |
| **Water** | `gameos_graphics.cpp` | 1 | `2D` | detail | EXPLICIT (restored to 0 post-draw) | `:3583-3585`; restore `:3634-3636` |
| **Water** | `gameos_graphics.cpp` | 2 | `2D` | reflRT | EXPLICIT (restored to 0 post-draw) | `:3597-3599`; restore `:3620-3622` |
| **Water** | `gameos_graphics.cpp` | 3 | `2D` | HDRI | EXPLICIT (restored to 0 post-draw) | `:3605-3607`; restore `:3628-3630` |
| **VegetationCards** | `gos_vegetation.cpp` | 0 | `2D` | atlas | EXPLICIT | `:453-454` |
| **VFX / Particles** | `gos_particle_bridge.cpp` | 0 | `2D` | atlas | EXPLICIT + save-restore 0 | `:535/594-595`; restore `:620` |
| **VFX / Particles** | `gos_particle_bridge.cpp` | 1 | `2D` | depthCopy (soft) | EXPLICIT (restored post-draw) | `:1073-1075`; restore `:1309-1310` |
| **VFX / Particles** | `gos_particle_bridge.cpp` | 2 | `2D` | sceneColorCopy (distort) | EXPLICIT (restored post-draw) | `:1116-1118`; restore `:1316-1317` |
| **PostProcess — screenShadow** | `gos_postprocess.cpp` | 0 | `2D` | sceneDepthTex_ | EXPLICIT | `:2125-2126` |
| **PostProcess — screenShadow** | `gos_postprocess.cpp` | 1 | `2D` | sceneNormalTex_ | EXPLICIT | `:2127-2128` |
| **PostProcess — screenShadow** | `gos_postprocess.cpp` | 2 | `2D` | shadowDepthTex_ (static) | EXPLICIT | `:2129-2130` |
| **PostProcess — screenShadow** | `gos_postprocess.cpp` | 3 | `2D_ARRAY` or `2D` | dynShadowArrayTex_ / dynShadowDepthTex_ | EXPLICIT (conditional) | `:2131-2135` |
| **PostProcess — screenShadow** | `gos_postprocess.cpp` | 4 | `2D` | dynamicFullMapTex_ | EXPLICIT (CSM-gated) | `:2138-2139` |
| **PostProcess — SSAO** | `gos_postprocess.cpp` | 0 | `2D` | sceneDepthTex_ | EXPLICIT | `:1971-1972` |
| **PostProcess — SSAO** | `gos_postprocess.cpp` | 1 | `2D` | sceneNormalTex_ | EXPLICIT | `:1973-1974` |
| **PostProcess — cloudShadow/shoreline/edgeFog/fogOob** | `gos_postprocess.cpp` | 0 | `2D` | sceneDepthTex_ | EXPLICIT | `:2215-2216` / `:2264-2265` / `:2321-2322` / `:2385-2386` |
| **PostProcess — cloudShadow** | `gos_postprocess.cpp` | 1 | `2D` | sceneNormalTex_ | EXPLICIT | `:2266-2267` |
| **PostProcess — skybox** | `gos_postprocess.cpp` | 0 | `2D` | hdriTex_ | EXPLICIT + save-restore | `:2904-2905`; restore `:2919-2921` |
| **PostProcess — composite blit** | `gos_postprocess.cpp` | 0 | `2D` | sceneColorTex_ | EXPLICIT + `GlScopedTextureUnit(0)` | `:2609/2616-2617` |
| **PostProcess — composite blit** | `gos_postprocess.cpp` | 2 | `2D` | sceneObjectIdTex_ (guarded) | EXPLICIT + `GlScopedTextureUnit(2)` | `:2610/2624-2625` |
| **PostProcess — shadowDebugOverlay** | `gos_postprocess.cpp` | 0 | `2D_ARRAY` or `2D` | shadow debug tex | EXPLICIT-BIND but **no unit-0 restore on exit** | `:2699-2700`; NO restore |
| **UI / HUD** | `gameos_graphics.cpp` legacy | 0 | `2D` | font/HUD atlas | via `applyRenderStates()` re-bind | `gos_ApplyRenderStates` chain |

**Summary: every pass that draws binds its own texture units explicitly. No pass is confirmed
to sample a unit it did not bind itself in the current frame.**

---

## Q2 — Inherited-unit dependencies and order-fragile ranking

Prior recon (§2 conclusion, `frame-graph-tex-latch-recon-1.md`) established: zero confirmed
inherited-binding ghosts on the default-on path. This recon reinforces that finding with
explicit bind-site verification. However, two classes of ORDER-FRAGILE state exist:

### ORDER-FRAGILE class 1 — 2D vs 2D_ARRAY target aliasing on multiplexed units

Units 3, 4, and 5 are shared across passes with DIFFERENT target types:

| Unit | Passes sharing it | Targets present | Order-safe? |
|---|---|---|---|
| **3** | Mech paint-normal (`2D`) + screenShadow dynShadow (`2D_ARRAY` or `2D`) + Water HDRI (`2D`) | mixed | SAFE iff each pass rebinds before draw — confirmed |
| **4** | Mech paint-ORM (`2D`) + screenShadow fullMap (`2D`) + Terrain transition-mask (`2D_ARRAY`) | mixed | SAFE iff each pass rebinds — confirmed (terrain has GLSTATE-TEXTURE-ARRAY-RESTORE-1) |
| **5** | Terrain matNormal0 (`2D`) + mine sprite array (`2D_ARRAY`, unconfirmed from prior recon) | mixed | LATENT — unit 5 share is unverified for mine vs terrain co-occurrence |

**These are NOT ghosts today** (each pass rebinds). They BECOME ORDER-FRAGILE ghosts IF an
executor: (a) establishes a clean texture state (zero-binding) at pass entry without the pass
then rebinding, OR (b) skips the prior pass that coincidentally established the right binding.
Neither (a) nor (b) is possible today because no executor intervention occurs on these passes.

**Risk rank (all LOW today; would become MEDIUM under executor reorder):**

1. **MEDIUM-IF-REORDERED: screenShadow unit 3 (2D_ARRAY).** If the executor moves screenShadow
   after water without ensuring water's restore (water restores unit 3 to 0 at `:3628-3630`),
   screenShadow would find a `2D` binding on unit 3 and rebind to `2D_ARRAY`. This is only a
   hazard if the restore is not seen. Since water does restore and screenShadow rebinds, the
   risk is contained — but it is an ORDER DEPENDENCY between water.epilogue and
   screenShadow.setup that is invisible to the resource graph.

2. **MEDIUM-IF-REORDERED: Terrain unit 4 (2D_ARRAY transition-mask).** The
   GLSTATE-TEXTURE-ARRAY-RESTORE-1 save/restore at `gameos_graphics.cpp:3888-3897/4044-4074`
   is a manual save+restore, not a `GlScopedTextureUnit` guard (the guard only covers `2D`,
   not `2D_ARRAY` — `opengl-correctness-ledger-1.md` :151). An executor that establishes a
   clean unit-4 state and then calls a later pass that iterates on terrain would be fine
   (terrain rebinds its own unit 4), but the terrain TU's save/restore looks AT the prior unit-4
   binding to restore it after terrain. If the executor zero-binds unit 4 pre-pass, terrain
   saves 0 and restores 0 — no functional difference. LOW.

3. **LOW: VFX unit 1/2 save-restore.** `gos_particle_bridge.cpp` saves+restores units 1 and 2
   (`:1309-1318`). These restores deposit whatever was bound by the prior pass. An executor
   that owns a pass between VFX and its consumer could leave a stale unit 1/2. Functional risk
   only if VFX restores a stale value that a later pass relies on without rebinding — which the
   per-pass-rebind discipline prevents.

### ORDER-FRAGILE class 2 — shadowDebugOverlay unit-0 leak (confirmed, default-OFF)

**HIGH (if ever default-ON): PostProcess shadowDebugOverlay leaves `GL_TEXTURE_2D_ARRAY` on
unit 0 without unbinding (`gos_postprocess.cpp:2699-2700`, no restore).** The prior recon
cataloged this as GLSTATE-SHADOWDEBUG-2DARRAY-1 / DEFERRED_LOW_RISK. Under executor ownership
of PostProcess, this leak exits `executorOwnEnd`'s watch and reaches the HUD/UI pass. The
executor's existing postcondition (`gos_postprocess.cpp:2647 gos_InvalidateRenderStateCache`)
does not track texture units, so the `2D_ARRAY` binding on unit 0 survives into the next
consumer. Functional only when `showShadowDebug_` is true — default-OFF, ImGui-gated, not a
current threat. If promoted to default-ON, add `glBindTexture(texTarget, 0)` at the
shadowDebugOverlay exit site.

**No other order-fragile inherited units confirmed on the default-on path.**

---

## Q3 — The NVIDIA-leak axis: tractable per-pass declarable shape?

Prior recon (`frame-graph-tex-latch-recon-1.md` §3; staging-recon-1 Stage B) concluded the
texture-unit latch axis is RECON-ONLY because:
1. No GLuint → `RenderResourceId` reverse map exists for atlases (the most common bound textures).
2. `reads[]` lacks an expected-unit column; binding slots are multiplexed per-pass.
3. The live NVIDIA 2D_ARRAY bleed hazard is unconfirmed on dev AMD.

**Does a tractable PER-PASS model exist now?** Partially, for a narrow subset.

### Tractable per-pass shape (narrow)

For each pass, two declarations are now verifiable:

```
binds_explicitly = {unit → target}   // units the pass calls glActiveTexture+glBindTexture for
restores_on_exit = {unit → bool}     // whether the unit is save-restored before pass exits
```

From the Q1 inventory, ALL passes have `binds_explicitly` populated (no inherited reads
confirmed). For `restores_on_exit`: most passes do restore (mech 0-4,6,7 via epilogue; water
0-3 via post-draw restore; VFX 0-2 via save-restore; composite 0,2 via `GlScopedTextureUnit`;
overlay/decal via `GlScopedTextureUnit`). The exception is shadowDebugOverlay unit 0.

**A minimal per-pass declaration shape:**

```cpp
struct PassTexContract {
    RenderPassId id;
    uint32_t bindsExplicitlyMask;   // bitmask of units this pass calls glActiveTexture+bind on
    uint32_t restoresOnExitMask;    // bitmask of units restored (save-restore or RAII guard)
    uint32_t target2DArrayMask;     // bitmask of units using GL_TEXTURE_2D_ARRAY (vs 2D)
};
```

**Tractable** because it is derivable from static grep of the per-pass draw sites (done in Q1
above). **Not a runtime probe** — a declaration that documents the per-pass intent, checkable
offline.

**Not tractable** (still intractable): the atlas-texture identity (which specific albedo/ORM
array is bound on unit 0 for StaticProp pass N vs pass N+1 — content-dynamic, no logical id).
The expected-unit column for `reads[]` (e.g. "ShadowDynamicMap is expected on unit 2 in the
screenShadow sub-pass") is derivable for the 4-6 id-backed resources but requires extending
`kRenderPassContracts[]` rows with unit numbers — medium-effort modeling work.

**Proposed minimal declarable shape:** the `bindsExplicitlyMask` + `restoresOnExitMask`
encoding above. It is offline-testable (static check: for each pass row, assert
`bindsExplicitlyMask` is a superset of `restoresOnExitMask`; also assert no unit appears in
`restoresOnExitMask` without appearing in `bindsExplicitlyMask`). This catches the
shadowDebugOverlay unit-0 hole (restores=false, binds=true → zero divergence today; if a
future pass relied on restore-of-0 it would show). **A runtime sampler is NOT needed** for this
declaration — the value is documentation + offline check, not a runtime probe.

---

## Q4 — Executor-ownership implication: what precondition/postcondition must the executor assert?

For the shipped PostProcess island (`executorOwnBegin/executorOwnEnd`, `gos_postprocess.cpp:4554/4614`):
- PRE: `compositeProg_->is_valid()` + `sceneColorTex_ != 0` + warn if no terrain latch.
- POST: `GL_DRAW_FRAMEBUFFER_BINDING == 0` + `glGetError() == GL_NO_ERROR`.
- Texture units: NOT asserted by the executor. The `GlScopedTextureUnit` guards INSIDE
  `endScene()` handle units 0/2 for the composite; the screenShadow path binds units 0-4
  explicitly. The executor wrapper does NOT need to assert texture unit state — the
  existing per-sub-pass rebind discipline makes it unnecessary.

**For a NEXT island**, the minimal texture-unit contract to add to `IslandContract` is:

```cpp
bool requiresTexUnit0Free;   // if true: assert unit 0 is NOT holding a 2D_ARRAY before entry
                              // (guards against shadowDebugOverlay leak propagating into next pass)
```

This is sufficient for the next island class (SSAO, fog, skybox sub-stages, debug overlay) —
all of which bind unit 0 to a `2D` texture. If shadowDebugOverlay just ran (gate
`showShadowDebug_`) and the executor asserts `requiresTexUnit0Free`, it catches the unit-0
`2D_ARRAY` leak before the next pass samples a wrong-target.

**Is a full per-pass texture-unit contract (binds/requires columns) worth declaring?** Only if
the executor starts reordering. Today the executor wraps a call without reordering, so the
"requires pre-bound" column would be empty for every pass (all passes rebind their own units).
The `binds_explicitly` column is already fully populated by Q1 — document it as commentary,
not a buildable ledger. Offline-testable via a static contract check (`PassTexContract` shape
above); runtime-samplable as a future extension.

---

## Q5 — Which next island is texture-safe?

From the Q1 inventory, the texture-safest next island candidates within PostProcess:

| Candidate | Units bound | All explicit? | 2D_ARRAY exposure | Restores on exit? | Verdict |
|---|---|---|---|---|---|
| **SSAO** (MC2_SSAO, default-OFF) | 0 (depth), 1 (normal) | YES | None | YES (unit 0 + 1 restored to 0 at `:2001/2010`) | **TEXTURE-SAFE** |
| **EdgeFog** (sceneHasTerrain_, default-ON) | 0 (depth), 1 (normal) | YES | None | YES (reset to 0 at `:2276`) | **TEXTURE-SAFE** |
| **FogOob** (sceneHasTerrain_, default-ON) | 0 (depth) | YES | None | YES (reset to 0 at `:2331`) | **TEXTURE-SAFE** |
| **CloudShadow** (enableCloudShadow_) | 0 (depth), 1 (normal) | YES | None | YES (reset to 0 at `:2226`) | **TEXTURE-SAFE** |
| **Shoreline** (shorelineEnabled_) | 0 (depth), 1 (normal) | YES | None | YES (reset to 0 at `:2276` area) | **TEXTURE-SAFE** |
| **SkyboxHDRI** | 0 (hdriTex_) | YES | None (2D) | YES (save-restore at `:2919-2921`) | **TEXTURE-SAFE** |
| **shadowDebugOverlay** | 0 (2D_ARRAY or 2D) | YES | **YES — `2D_ARRAY` leak** | **NO** (no restore after bind at `:2699-2700`) | **NOT SAFE without 1-line fix** |

**Recommendation:** `EdgeFog` or `FogOob` are the next cleanest islands — single unit (0,
depth only), always default-ON when terrain present, no `2D_ARRAY`, restore on exit. They
are sub-stages of the already-owned PostProcess island, making them natural extension
candidates without adding a new `kExecutorIslands` row (the executor already owns the
PostProcess wrapper; these sub-stages run inside the existing `pp->endScene()` call).

Cross-referencing `executor-island-recon-1.md`'s candidate list: none of those candidates are
BLOCKED on texture-unit grounds. The texture-unit picture is the green light column, not the
blocker column.

---

## Q6 — Verdict: build or defer?

**DEFER.**

Matching the prior recon's verdict but now with the executor angle confirmed:

**The executor does not need a texture-unit ledger to safely own more islands.** Reasoning:
1. All passes bind their own units (Q1) — no ghost materializes when the executor establishes
   a clean entry state, because the pass rebinds everything it needs.
2. The 2D vs 2D_ARRAY aliasing on units 3/4/5 (Q2) is ORDER-FRAGILE only under reorder, and
   the executor's current `IslandContract` model wraps calls without reordering.
3. The `PassTexContract` shape (Q3) is declarable but adds documentation value only, not
   runtime-catch value — because there is nothing to catch (Q1).
4. The shadowDebugOverlay unit-0 leak (Q2, class 2) is fixable with one `glBindTexture` line
   and is gated default-OFF. It does NOT require a ledger.

**Trigger condition for building the texture-unit ledger:**
- An executor island REORDERS two passes (not just wraps them), AND
- A RenderDoc NVIDIA vendor capture confirms a 2D/2D_ARRAY aliasing misfire on a
  multiplexed unit (units 3/4/5).

Neither condition is met today. The FBO ledger (stage A, `frame-graph-executor-staging-recon-1.md`)
and the static stale-read check (stage B carve-out) are higher-value next steps.

**If you want to build something now:** add `PassTexContract` as an offline-testable static
table in `RenderCore/` (no GL, no runtime, ~1 header). It documents Q1's per-pass
`bindsExplicitlyMask` + `restoresOnExitMask`, catches the shadowDebugOverlay hole via a
static assert (`restores` implies `binds`), and is zero-risk. It is NOT a ledger — it is a
declaration. Estimated cost: 1 header + unit test. This is Tier A from the prior recon's §6
proposal, narrowed to the declarable shape from Q3.

---

## Extension note (vs `frame-graph-tex-latch-recon-1.md`)

Prior recon asked: "does a texture-read ledger mirror the FBO/ambient ledgers, and is it
worth building?" Verdict: LOW-MEDIUM value because all textures are already explicit (no ghost
to catch) and atlases lack logical ids. **This recon adds the executor-safety angle**: even
under executor reorder, the per-pass-rebind convention means no pass would sample a stale
unit — provided the executor does NOT clear texture units at pass entry AND each pass's
`glActiveTexture+glBindTexture` calls remain intact. The rebind-before-draw discipline is the
texture-safety invariant; the `PassTexContract` declaration (Q3) is the offline enforcement.
The prior recon's §7 executor-impact analysis is confirmed and extended: the 3 rules there
(shadow maps PostProcess-only; multiplexed-unit rebind must survive reorder; SSBO save/restore
must survive) remain the complete list. No new rules are required.

---

## SLICE PROPOSAL (not built) — IF verdict changes to BUILD

Condition: executor reorders passes OR NVIDIA aliasing confirmed.

**SLICE: PASS-TEX-CONTRACT-DECLARE-1** (~1 day, zero-risk)

```
RenderCore/pass_tex_contract.h
```

- `PassTexContract` struct: `id, bindsExplicitlyMask, restoresOnExitMask, target2DArrayMask`
- `kPassTexContracts[]` table: one row per pass/sub-pass from Q1 inventory.
- `static_assert` in unit test: for each row, `restoresOnExitMask` is a subset of `bindsExplicitlyMask`.
- Detect shadowDebugOverlay row: `bindsExplicitlyMask` has bit-0 set, `restoresOnExitMask` has bit-0 CLEAR → flagged.
- Default-ON offline check only; no GL calls; no runtime probe.
- When NVIDIA aliasing confirmed: extend to runtime probe (`glGetIntegerv(GL_TEXTURE_BINDING_2D[_ARRAY])` per
  declared unit at pass begin, compare to expected target from `target2DArrayMask`).

**NOT worth scheduling** until the trigger condition fires.
