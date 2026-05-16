# Render observations -- 2026-05-15 -- vpl-deferred-retirement-topology

**Source:** Post-VPL-retirement stock-take session. Read docs/superpowers/VPL-RETIREMENT-DEFERRED.md (items 1-13) + docs/render-perf-snapshot.md; dispatched the cpu-gpu-offload methodology advisor to grep-verify the GPU-vs-CPU migration table and derive a meta-retirement seam topology. No code changed except doc repairs (CLAUDE.md stale-example removal, snapshot row 47 cmd-patch RETIRED).

**Scope:** Terrain-indirect projection authority, demoted-CPU-path inventory, static-prop LOD/cull ownership, depth-bias seam for the zoom-only z-fight, deferred-item ownership ledger.

---

## Confirmed facts about current code

- Cmd-patch dispatch is RETIRED in VPL-retirement Step 2b -- NOT a queued future slice. Evidence (grep-verified this session): `gos_terrain_indirect.cpp:1485` ("cmd-patch program retired. Primary compute is..."), `:1538-1539` (`g_locCmdVPE`/`g_locCmdCC` removed, program not compiled), `:2289` (dispatch retired), `:2388` (compile retired), `:2953` (parity probe retired with cmd-patch). Grep `cmd-patch` in that file for current lines.
- Terrain quad projection is GPU sole-authority: `shaders/gpu_driven_terrain_solid.comp` exists (NOT absent -- the worktree CLAUDE.md previously claimed it doesn't), `clipPos[4]` at `:90`, `tr.clipPos[*]` writes `:470-473` (Fix B `005ebc7`). Thin VS no longer touches `terrainMVP`.
- CPU pack path is DEMOTED behind `MC2_TERRAIN_INDIRECT_CPU_FALLBACK` (default-off), `gos_terrain_indirect.cpp:1668`. Parity infra fully deleted 2026-05-15; body deletion is deferred item 3.
- Static-prop LOD-0 pin (`a2a6058`) is live at `bdactor.cpp:1390` region. `bdactor.cpp:4402` comment states tree GPU cost is negligible (supports the user's tree-vs-building attribution dispute on item 13).
- Item 4 FNV residual is live and UNGATED at `gameos_graphics.cpp:2686-2719` (`if (drawFp != dispatchFp)` at `:2696`) -- unlike the Step 9 writers gated by `g_envRingTrace`. Tracker cites `:2683-2724`; actual `:2686-2719` (Rule-0 line drift, symbols stable).
- Picking is intentionally CPU-resident by VPL Path-A design: `camera.cpp:593`/`:741` (frustum x quad-AABB, recursion-free), projection via `projectForSelectionPicking` wrapper. GPU port was rejected (reintroduces the 1-frame-lag bug class Fix A/B killed). This is the deliberate CPU endpoint, not a future-slice candidate.

---

## New observations not currently in MEMORY.md or render-contract.md

- **Meta-retirement seam topology.** The ~9-item VPL deferred tail is not 9 slices; it collapses to TWO commit-classes plus an irreducible remainder:
  - *Seam 1 -- "dead-write GC" commit-class:* items 1 (dead `hazeFactor` field), 2 (`.codex_tmp_isolate` git rm), 4 (ungated FNV residual), 3-body (demoted CPU-pack body). Shared property: each is provably inert by construction. One audited commit, single invariant: "delete iff zero live reader, proven by opposite-direction grep" (per `feedback_data_flow_audit_asymmetry`). Lockstep edits (item 1) stay one-commit per `cpp_glsl_ubo_struct_lockstep`.
  - *Seam 2 -- unified `TERRAIN_DEPTH_FUDGE` header constant:* item 10's three TU-local `#define`s (`quad.cpp:1997`, `tgl.cpp:2868`, stale codex copy) collapse into one shared-header constant, and that header IS the insertion seam for the distance-proportional depth bias that fixes the zoom-only z-fight. The meta-cleanup and the regression fix are the same move.
  - *Irreducible (cannot collapse, and why):* 5 (Tracy re-capture = measurement, user-driven), 6 (interactive picking UAT = input-injection coverage gap), 7 (overlay-decal GPU port = full sibling slice with own SSBO/parity/soak arc), 8 (water `fogRGB` consumption = blocked on an unresolved data-flow grep), 9 (accepted, stock-masked, doc-only), 11 + 13 (pre-existing bugs needing own debug slices, catastrophic-axis), 3-pattern (probe-placement methodology is irreducible even though 3-body folds into Seam 1).
  Where this should live: docs/render-contract.md (a "retirement seam topology" subsection under the terrain-indirect retirement section) | the VPL-RETIREMENT-DEFERRED.md tracker should gain a "Seam" column.

- **GPU-move lever applies to exactly 3 of the 13 deferred/regression items**, not the tail broadly. Moving work to a compute shader / proper pipeline action is the right lever ONLY for: item 7 (overlay-decal -> typed GPU batch), item 10 (constant depth fudge -> vertex-stage distance-proportional clip-space-z bias), item 13 (CPU LOD-0 pin -> GPU-side distance->LOD selector in the cull compute, `gpu_cull_predicate.glsl`). The rest are deletes of dead/inert CPU code (1/3-body/4), measurements (5/9), coverage gaps (6), or non-GPU bugs (11 = deserialization). Anti-pattern flagged: do NOT "GPU-ify" dead code or a savegame-deserialization bug to satisfy a modernization reflex; the modern move there is deletion or a CPU debug slice, not a compute port.
  Where this should live: MEMORY.md topic file (methodology -- "GPU-move lever is selective, not a blanket modernization") | render-contract.md.

- **Ownership / touchability ledger** for the deferred + surfaced-regression set (the durable answer to "who owns what and when can we touch it"):

  | Item | Owning advisor | GPU-move? | Touchable now? | Gate before touch |
  |---|---|---|---|---|
  | 1 hazeFactor field | terrain-indirect | no (delete) | yes | Seam 1 lockstep-one-commit |
  | 2 codex scratch rm | (hygiene) | no | yes | rides Seam 1 |
  | 3-body CPU-pack | terrain-indirect | no (delete) | yes | Seam 1; zero-reader proof |
  | 3-pattern probe placement | (methodology) | no | yes | fix-or-retire decision, own |
  | 4 FNV residual | terrain-indirect | no (delete/gate) | yes | Seam 1; ring data-flow check |
  | 5 Tracy slimReduce | render-perf | no | NO | user-driven Tracy |
  | 6 interactive picking UAT | render | no | NO | user-driven smoke (input inject) |
  | 7 overlay-decal GPU port | render + terrain-indirect | YES | yes | own slice; full parity/soak arc |
  | 8 water fogRGB clamp | shader | maybe | partial | unresolved grep settles first |
  | 9 GetApproximateLength | (accepted) | no | n/a | none (doc-only) |
  | 10 zoom z-fight | shader + render | YES | yes | Seam 2; adversarial review (depth, structural) |
  | 11 savegame mechs | mission-data / mech-update-geometry | no | NO | user-driven savegame repro |
  | 13 GpuStaticProps LOD | render-perf + terrain-indirect | YES | NO (attribution) | tree-vs-building count first |

  Where this should live: docs/render-contract.md (ownership ledger section) | the DOMAINS.md routing table already owns the advisor column; the "touchable now / gate" columns are new and should land in the tracker.

- **Vulkan-aligned depth-bias ruling (item 10 direction, user-elicited 2026-05-15):** for the zoom-only z-fight fix, the modern Vulkan-transition-friendly approach is a VERTEX-STAGE distance-proportional clip-space-z bias, NOT `glPolygonOffset` and NOT fragment `gl_FragDepth`. Rationale: `glPolygonOffset` is implicit fixed-function GL pipeline state (the exact category `vulkan_prep_explicit_device_discipline` is moving away from; its constant-factor units are depth-format-dependent = a GL<->Vulkan portability footgun). In-shader bias is self-contained logic mapping 1:1 to SPIR-V with zero implicit cross-call state. Must be vertex-stage: fragment `gl_FragDepth` disables early-Z and is an AMD RX 7900 XTX driver-rule violation. CAVEAT TO VERIFY at slice time: stale memory `terrain_depth_bleed.md` claims terrain already writes `gl_FragDepth = UndisplacedDepth + 0.0005` (a constant bias -- which is itself the zoom-failure root cause); the #10 owner must grep the current terrain frag for any live `gl_FragDepth` write before choosing where the bias lands, and must respect the `[0,1]` `glClipControl` depth convention (a vulkan-prep protected pattern).
  Where this should live: MEMORY.md topic file (sibling to `vulkan_prep_explicit_device_discipline`).

- **Debug-loop tightening principle (user-stated goal "tighter and tighter"):** when a regression's root-cause data is currently gated behind a user-driven external-tool capture (RenderDoc one-frame, savegame load the smoke harness cannot drive), prefer converting it into an env-gated in-process probe (per `debug_instrumentation_rule`) that emits the SAME discriminating datum from one ordinary user-driven smoke. Item 13 is the canonical case: substrate=0/=1 Tracy gives only TOTAL prop cost and structurally CANNOT settle the tree-vs-building attribution; a `[STATICPROP v1] event=category_count` probe emitting per-category instance+primitive counts converts "needs RenderDoc skill" into "run one smoke, read the log." This is a strictly tighter loop and is itself codeable without any of the blocked data.
  Where this should live: MEMORY.md topic file (methodology, extends `debug_instrumentation_rule`).

---

## Contradictions found

- **docs/render-perf-snapshot.md row 47** said "Cmd-patch dispatch retirement | QUEUED -- design drafted". Current code: RETIRED in Step 2b (`gos_terrain_indirect.cpp:1485/1538-1539/2289/2388/2953`). Resolution: row corrected to RETIRED this session.
- **Worktree CLAUDE.md** (grep-discipline example, line ~78) claimed `gpu_driven_terrain_solid.comp` "doesn't exist on disk". Current code: it exists (`shaders/gpu_driven_terrain_solid.comp`, `clipPos[4]` at `:90`) and is Fix B's sole terrain-quad projection authority. Resolution: stale example removed this session, recursively, by the same grep discipline.
- **docs/superpowers/VPL-RETIREMENT-DEFERRED.md item 4** cites FNV residual at `gameos_graphics.cpp:2683-2724`; actual `:2686-2719`. Resolution: Rule-0 line drift; symbols authoritative, re-grep before acting (not corrected in the tracker this session -- flagged for the tracker's own next pass).
- **docs/superpowers/VPL-RETIREMENT-DEFERRED.md item 11** leads with a `Mech3DAppearance::copyFrom` `status`-not-restored (`mech3d.cpp:5327`) invisibility hypothesis. Data-flow DISPROVES it as the cause: `status` feeds only `lightsOut`/ambient shading (`mech3d.cpp:2546-2550`), never a submit/visibility gate. Real mechanism: per-type Track D batch missing because the invisible mech type's `registerTypeLod` is skipped/late vs `finalizeGeometry()` (`mission.cpp:3115`), failing `TypeLodKey` lookup (`gos_mech_batcher.cpp:575-579`) for all instances while `mechGpuCullSkip` suppresses CPU fallback (`mech3d.cpp:2582`). Confirmed by user RenderDoc (visible type = EID 3931 (36,4); invisible type = no draw at all). Resolution: CORRECTION block appended to item 11 this session; trigger (savegame load-order) deferred to mission-data advisor; `[MECHRESTORE v1]` probe specced. Where this should live: render-contract.md (Track D per-type-keyed submit + zero-instance-type=no-draw is a load-bearing batcher contract not currently documented).
