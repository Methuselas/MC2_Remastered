# STATIC-PROP-PATH-OWNERSHIP-RECON-1

Read-only recon. Makes every static-prop draw path confess which pipe it is, so the
"we routed a bridge but the live path is elsewhere" terrain rake can't repeat here.
Source-verified against nifty `985e17f7` (worktree `static-prop-path-ownership-recon-1`),
grounded by live `[PIPELINE_BIND]` + `[FRAME_PLAN]` traces on mc2_24 and mc2_10.

## The paths (live source)

All in `GameOS/gameos/gos_static_prop_batcher.cpp` unless noted.

| Path | Function | Draw | applyPipeline (PipelineId @ line) | dbgName? | Writes | Alpha |
|---|---|---|---|---|---|---|
| **Color (main)** | `flush()` | MDI / instanced (many variants: M2a split, coalesce v5/v6, BC7 bucket, legacy) | `StaticPropOpaque` @ **5376** | **NO** | color0 + GBuffer1 + objectId(`#ifdef`) | opaque + alpha-test folded |
| Color re-test | `flush()` Equal-depth re-render | (variant) | `StaticPropOpaque` (modified desc, depthFunc=Equal) @ **6428** | NO | same | — |
| **Depth prepass** | `flushDepthPrepassV6()` | instanced | `StaticPropDepth` @ **5139** (color-masked off) | **NO** | none (depth only) | alpha-test discard |
| **Shadow (dynamic)** | `flushShadow()` / cascade | instanced | `ShadowStaticProp` @ **7646** | **"ShadowStaticProp"** | none | discard via alpha prog |
| Shadow (static bldg) | static-building-shadow draw | instanced | `ShadowStaticProp` @ **7799** | **"ShadowStaticProp"** | none | rigid (no discard) |
| Shadow (raw variant) | `resolvePropShadowProgram` path | instanced | raw `glUseProgram(shadowProg)` @ **7444** | n/a (raw GL) | none | discard |
| Legacy CPU/MLR fallback | (when GPU path unavailable) | — | legacy `gos_SetRenderState` | n/a | ? | ? |

### Alpha-test is NOT a separate routed path — and shouldn't be
`PipelineId::StaticPropAlphaTest` exists and is used for **DrawPacket metadata**
(4990/5021/6311/8565/8636) + `bindProgram` (1207), but is **never `applyPipeline`'d**.
The color draw always issues `applyPipeline(StaticPropOpaque)`; alpha-test packets
differ only by **shader discard + texture-array switch**, not fixed-function state.

**Why that is correct:** the `StaticPropOpaque` and `StaticPropAlphaTest` PipelineDesc
rows are byte-identical through `applyPipeline` — both `blend` map to
`glDisable(GL_BLEND)+glBlendFunc(ONE,ZERO)` (Opaque and AlphaTest cases are identical in
`pipeline_binder.cpp`), same `{t,t,f}` attachments, same GEQUAL/Back/Ccw. So a separate
`applyPipeline(StaticPropAlphaTest)` would emit **identical GL** — a no-op. **Routing
alpha-test separately is NOT warranted** (it would be churn + a misleading second bind).

## Live trace (mc2_24 + mc2_10, MC2_PIPELINE_BIND_TRACE + FRAME_PLAN)

```
[PIPELINE_BIND] ShadowStaticProp ...   x9072 (mc2_24) / x9208 (mc2_10)   ← labeled, visible
StaticPropOpaque / StaticPropDepth     ← ABSENT from [PIPELINE_BIND] and [FRAME_PLAN]
```

**The gap:** the live **color** + **depth-prepass** static-prop draws ARE routed through
`applyPipeline`, but those calls pass **no dbgName**, so they emit neither
`[PIPELINE_BIND]` nor `[FRAME_PLAN]`. The frame cannot self-report its single most
important static-prop pass. This is precisely the terrain blind spot (FRAME-PLAN was the
weapon that fixed terrain) — only `ShadowStaticProp` (which passes a dbgName) is visible.

(Incidental, out of scope: mc2_10 shows `gpu_drawn_instances=0` + `cpu_fallback=4` —
the known OBJBATCHER GPU-draw gap, tracked elsewhere; NOT a routing/ownership issue.)

## Ownership status per path

| Path | PipelineDesc row | AttachmentContract | DrawBufferSet | colorMask | PassTrace label | Routed | bind-positive |
|---|---|---|---|---|---|---|---|
| Color StaticPropOpaque | ✅ (row 1) | ✅ StaticPropOpaque {0,1} | ✅ MainSceneMRT | n/a (no opt-in) | ❌ **missing** | ✅ (5376) | ❌ (unlabeled) |
| Color alpha-test | ✅ (row 2, descriptive) | ✅ StaticPropOpaque path | ✅ (shares Opaque) | n/a | ❌ | ✅ (via Opaque) | ❌ |
| Depth prepass | ✅ (row 4) | depth-only {f,f,f} | Unspecified | n/a | ❌ **missing** | ✅ (5139, gated) | ❌ |
| Shadow | ✅ (row 6) | depth-only | ShadowDepthOnly | n/a | ✅ "ShadowStaticProp" | ✅ (7646/7799) | ✅ |
| Shadow raw variant | row 6 | depth-only | ShadowDepthOnly | n/a | ❌ | ❌ raw GL (7444) | — |
| Legacy CPU fallback | — | — | — | — | ❌ | ❌ | — |
| Picking / editor preview | — | — | — | — | — | DO_NOT_MODEL | — |

## What to harden first (the pick)

NOT alpha-test routing (proven no-op). **The color + depth-prepass observability gap.**
Hardening these so the live path is bind-positive observable is the high-value move:
add a dbgName to the `applyPipeline(StaticPropOpaque)` @5376 / `(StaticPropDepth)` @5139
calls + a `render_frame_plan::trace` for the color pass. Behaviorally byte-identical
(dbgName only feeds the gated `[PIPELINE_BIND]` line; trace is gated) and it gives the
static-prop color path the same self-report terrain got.

## Recommended arc (revised by findings)

1. **STATIC-PROP-FAMILY-LEDGER-1** — encode this table as `{json,md}` + a checker that
   asserts every live static-prop path has a PipelineDesc row, attachment contract,
   draw-buffer set, PassTrace label, and (if routed) bind-positive proof.
2. **STATIC-PROP-COLOR-LABEL-HARDENING-1** — add dbgName + frame_plan trace to the
   color & depth-prepass `applyPipeline` calls (closes the blind spot; bind-positive
   proof for the live color path). *Replaces the TD's alpha-test-routing slice, which
   the recon proved is a no-op.*
3. **STATIC-PROP-LEGACY-PATH-CLASSIFY-1** — classify the raw-GL shadow variant (7444)
   and the CPU/MLR fallback: route, leave, or DO_NOT_MODEL.

## Do NOT
Alpha-test separate routing (no-op), static-prop material/batching rewrite, objectId/
picking cleanup, editor preview, all legacy paths in one slice, the OBJBATCHER
gpu_drawn_instances=0 gap (separate track), Vulkan backend.
