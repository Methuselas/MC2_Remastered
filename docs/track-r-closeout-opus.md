# Track R Closeout — Opus Audit (2026-05-29)

Branch `claude/nifty-mendeleev`, HEAD `7e5fc448` (MechOpaque wired through PipelineDesc).

Five recon subagents (A ledger, B EngineView/Visibility, C PipelineDesc, D
debug-inspect, E CI/docs) audited code vs roadmap. **Deliverable is truth, not
code.** Verdict below. No production code changed this opus (one stale
doc-string comment corrected; this doc written).

---

## 1. Track R status table

12 engine-convergence items. Evidence is file or commit.

| # | Item | Status | Evidence | Next action |
|---|------|--------|----------|-------------|
| 1 | Engine-as-API handles | **SHIPPED** | `RenderCore/Handle.h` (20/12 split, 5 tag types); M1 `60bcb3aa` | none; API stable |
| 2 | Render contracts / PipelineDesc | **SHIPPED** | `RenderCore/PipelineDesc.h`; consumers StaticPropOpaque (`gos_static_prop_batcher.cpp:3833`) + MechOpaque (`gos_mech_batcher.cpp:1719`); `47196ff2` | optional Water consumer (low value) |
| 3 | Snapshot / extraction | **SHIPPED** | `GameOS/gameos/render_snapshot.{h,cpp}` v1.1; static-prop snapshot-primary default-ON; mech extract gated | consumer adoption only |
| 4 | VisibilityRequest | **SHIPPED (v0+V1A)** | `RenderWorld/VisibilityRequest.h`; reporting-only, owns NO dispatch | V1B GPU readback = post-Track-R |
| 5 | EngineView / multi-view | **SHIPPED (substrate)** | `RenderCore/EngineView.h`; 4 ViewIds, shadow views registered `gos_postprocess.cpp:1397/1707` | shader-consume = optional, see §3 |
| 6 | MaterialGpu / resource model | **SHIPPED** | `RenderCore/MaterialGpu.h` 32B std430; static-prop sample default-ON; mech compare-only | mech texture-model arc = Track V |
| 7 | DrawPacket | **SHIPPED** | `RenderCore/DrawPacket.h` 64B; v6 dispatch + snapshot compare live | v3+ submission wiring deferred |
| 8 | FrameArena / resource lifetime | **SHIPPED** | `RenderCore/FrameArena.h`; render_snapshot ping-pong 2×1MiB; tests `eee18be4` | adopt in HZB/sort rings |
| 9 | Feature registry | **SHIPPED** | `RenderCore/RendererFeatureRegistry.h` (COUNT=30); CI `check-env-registry.sh` in contract gate | monitor drift |
| 10 | Debug / object inspection | **PARTIAL — good enough for substrate, pixel-pick MISSING** | `RenderDebugView.h` (10 modes) + ImGui wired; ObjectID buffer WRITTEN but never read back at pixel | `DEBUG-OBJECT-INSPECT-1` candidate (§ queue) |
| 11 | Render resource / debug view registries | **SHIPPED** | `RenderCore/RenderResourceRegistry.{h,cpp}` 9 slots, auto-register, JSON dump | UI/profiler consumers |
| 12 | Asset cook / streaming / interpolation | **DEFER / POST-TRACK-R** | offline cookers `cdag_cooker`/`mc2texcook` present, ZERO runtime consumers; KtxLoader RGBA8-only; streaming not started | scope as new arc |

Roadmap docs (`renderworld_arc_status.md`, `renderworld_migration_guide.md`,
`tier1_env_vars.md`) audited line-by-line: **ACCURATE, no stale premises.**

---

## 2. Completion estimate

- **Strict full roadmap:** ~92%. (11/12 items shipped; item 12 untouched but
  legitimately post-Track-R.)
- **Done-enough-for-Track-V:** ~98%. Every substrate Track V needs (handles,
  PipelineDesc, snapshot, MaterialGpu, DrawPacket, FrameArena, feature
  registry, resource/debug registries) is shipped and CI-guarded.
- **Remaining true closeout:** ~2–5%. One diagnostics gap (pixel-pick
  readback) + cheap test/doc hygiene. No architectural hole.

Track R is **effectively closed.** What remains is hygiene + one optional
high-leverage diagnostics slice — not core convergence work.

---

## 3. Subagent verdicts (truth corrections)

**B — EngineView/Visibility:** Shadow + main views all REGISTERED. Only
MainScene CONSUMES the UBO (binding 3) in mech/static-prop shaders. Shadow
passes use a separate `lightSpaceMatrix` flat-uniform path and work correctly.
Proposed `ENGINEVIEW-SHADER-CONSUME-1` (route shadow matrices through the same
UBO) is **architectural unification, not a gap** — shadows already function.
**Verdict: DEFER (gold-plating). Not Track R closeout.**

**C — PipelineDesc:** StaticPropOpaque + MechOpaque are both HONEST production
consumers (verified `applyPipeline(getPipelineDesc(...))` call sites). Manual
GL fixed-function state still inline in terrain, water, VFX, post-process,
shadow. Safest next consumer = **Water** (single isolated `glDepthFunc` +
save/restore already present). But it **closes no Track R gap** — two real
consumers prove the pattern. **Verdict: PipelineDesc coverage may STOP for
Track R; Water/Terrain are optional Track-R2/S polish.**

**D — Debug inspect:** Frame-snapshot already captures kind/handle/material/
pipeline/packet. The ONE missing piece: pixel-click → snapshot bridge
(`glReadPixels` from ObjectID attachment-2 + ObjectID-decode + ImGui surface).
**Highest-leverage remaining slice; orthogonal to render paths; unblocks Track
V diagnostics.**

**E — CI/docs:** `check-contracts.sh` **8/8 PASS**, `mc2_tests` **57 pass /
2213 assertions**, shader-schema PASS. One red — `check-unified-projection-
retirement.sh` (5 surviving `gos_SetTerrainMVP` sites) — is the in-flight F1
arc tracker, **deliberately not in the contract gate, expected red.** Two
stale doc-strings flagged; ShadowMaps one fixed this opus. Test gap: no machine
check that the ~12 newest Track-V gates default-OFF (their entire safety story
is "default-OFF = byte-identical").

---

## 4. Final prioritized queue (≤5)

| # | Slice | Why | Collision risk | Size | Type | Validation |
|---|-------|-----|----------------|------|------|-----------|
| 1 | `TRACKV-GATE-DEFAULT-OFF-TEST-1` | machine-guard the "default-OFF = byte-identical" safety story before Track V flips gates; one table-scan asserting `defaultOn==false` for V-lane features | none (test-only) | XS | impl | `mc2_tests --ts=RenderCore` |
| 2 | `DEBUG-OBJECT-INSPECT-1` | click pixel → object/material/pipeline/packet; ObjectID written but never read back; high ROI for Track V diagnosis | low (additive, gated `MC2_DEBUG_OBJECT_INSPECT=1`, no render-path edit) | S | impl | smoke tier1 5/5 gate-OFF byte-identical + manual pick |
| 3 | `TRACKR-DOC-SYNC-1` | residual stale doc-strings/comments (ShadowMaps fixed; sweep for others) | none | XS | doc | check-contracts |
| 4 | `PIPELINEDESC-WATER-2` | optional 3rd consumer; isolated `glDepthFunc`+save/restore; proves pattern beyond 2 | low-med (touches water render path → needs deploy+visual) | S | impl | deploy + water visual + tier1 |
| 5 | `ENGINEVIEW-SHADER-CONSUME-1` | unify shadow matrices through UBO | med (touches shadow shaders) | M | impl | deploy + shadow visual + tier1 |

**Recommended next slice: #1 then #2.** #1 is XS insurance directly enabling
safe Track V acceleration. #2 is the single genuinely-missing Track-R-flavored
capability (diagnostics) and de-risks all of Track V. #4/#5 are optional polish
— do only if explicitly wanted; neither closes a real gap.

---

## 5. No-op / already-shipped — STOP revisiting

- Feature registry CI (`check-env-registry.sh` in contract gate) — SHIPPED
- FrameArena substrate (+ tests `eee18be4`) — SHIPPED
- StaticProp snapshot-primary (default-ON, live baseInstance/instanceCount xref) — SHIPPED
- StaticPropOpaque PipelineDesc consumer — SHIPPED
- MechOpaque PipelineDesc consumer (`7e5fc448`, PipelineId=3, Count_=4) — SHIPPED
- Engine-as-API handles, MaterialGpu, DrawPacket v6, EngineView substrate,
  RenderResourceRegistry, RenderDebugView, VisibilityRequest v0+V1A — SHIPPED
- Roadmap docs accuracy — VERIFIED current; no rewrite needed

---

## 6. Deferred / post-Track-R

- **Asset cook / streaming / interpolation** (item 12) — new arc; needs KTX2
  bindless decision + streaming granularity + mech texture unification
- **Full snapshot independence** (extraction owns build with no live xref) — post-R
- **Full render graph** — post-R
- **VisibilityRequest V1B** (GPU compute readback) — post-R
- **DrawPacket v3+ submission wiring** — post-R
- **ENGINEVIEW-SHADER-CONSUME-1** — optional unification, shadows already work
- **PipelineDesc Terrain/VFX/post-process consumers** — Track R2/S polish

---

## 7. Validation state (this opus)

Recon-only + 1 doc-string fix + this doc. Per subagent E (run on canonical this session):

```
scripts/check-contracts.sh        → 8 passed, 0 failed (exit 0)
mc2_tests.exe (RelWithDebInfo)    → 57 passed, 0 failed, 1 skipped; 2213 assertions
  --ts=RenderCore                   covered (RenderResourceRegistry, RenderDebugView,
                                     FeatureRegistry COUNT=30, PipelineDesc adapter, FrameArena)
scripts/check-shader-schema.sh    → PASS interfaces=3
```

Known intentional red: `check-unified-projection-retirement.sh` (F1 in-flight
arc tracker, not in contract gate). No production render path changed → no
smoke required this opus. The doc-string comment edit
(`RendererFeatureRegistry.h:213`) is non-functional; re-run `mc2_tests
--ts=RenderCore` if a build is taken (registry strings not asserted by tests).

---

## 8. Recommended next slice

**`TRACKV-GATE-DEFAULT-OFF-TEST-1`** (XS, test-only) — closes the last cheap
hygiene gap and is the precondition for safely accelerating Track V gate flips.
Then **`DEBUG-OBJECT-INSPECT-1`** (S, gated) as the one real diagnostics
closeout. Everything else in Track R is shipped or legitimately deferred.
