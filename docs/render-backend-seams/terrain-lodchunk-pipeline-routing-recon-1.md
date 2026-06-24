# RECON — TERRAIN-LODCHUNK-PIPELINE-ROUTING-RECON-1

Read-only. The frame-trace proved the LIVE default terrain-solid path is the LOD-chunk path
(`mc2TerrainLodChunkEnabled()` default-ON, suppresses the bridge). With the colorMask keystone
(COLORMASK-ROLLOUT-1) landed, route this path. This recon answers the 9 questions; NO routing.

## 1. Exact current LOD-chunk draw state (gos_terrain_lod_chunk.cpp)
Draw: `glDrawElements(GL_TRIANGLES, ...)` @ 887 (+ skirts @ 914). State set @ 565-592:
- `glEnable(GL_DEPTH_TEST)` (588) -> depthTestEnable = true
- `glDepthMask(GL_TRUE)` (589) -> depthWriteEnable = true
- `glDisable(GL_BLEND)` (590) -> blend = Opaque
- `glDepthFunc(s_wantDepthFunc)` (591), `s_wantDepthFunc = s_depthAlways ? GL_ALWAYS : GL_GEQUAL`
  (565) -> **GreaterEqual** (reverse-Z) in the default path; `s_depthAlways` is a DEBUG override
  (default OFF).
- `glDisable(GL_CULL_FACE)` (592, "double-sided") -> cullMode = None
- frontFace: NOT set -> ambient (Ccw, per TERRAIN-CULL-STATE-PROBE-1).
Teardown 941-945 RESTORES prevCullFace/prevDepthMask/prevDepthTest/prevBlend/prevDepthFunc
(saved 575-587) -> the path is **self-contained save→set→restore** (like the bridges).

## 2. colorMask expectation
NOT set at the draw; relies on the ambient mask. With the COLORMASK-ROLLOUT-1 keystone
(beginScene asserts all-TRUE before the first MRT draw), the mask is all-attachments-writable,
so terrain writes color0 + GBuffer1 correctly. No colorMask handling needed at the draw.

## 3. colorAttachments
`terrain_lod_chunk.frag`: `layout(location=0) out vec4 fragColor` + `layout(location=1) out
vec4 GBuffer1` (74-75); NO location=2 objectId. -> **colorAttachments = { true, true, false }**.

## 4. Match vs TerrainSolid row
| field | TerrainSolid row | LOD-chunk live | match |
|---|---|---|---|
| blend | Opaque | Opaque | ✓ |
| depthTestEnable | true | true | ✓ |
| depthWriteEnable | true | true | ✓ |
| depthFunc | GreaterEqual | GreaterEqual (default) | ✓ |
| cullMode | None | None | ✓ |
| frontFace | Ccw | Ccw (ambient) | ✓ |
| colorAttachments | {t,t,f} | {t,t,f} | ✓ |
| objectIdWriteEnabled | false | false | ✓ |
| polygonOffsetEnable | false | false | ✓ |
**EXACT match.** The TerrainSolid PipelineDesc row fits the LOD-chunk draw with no change.

## 5. markTerrainDrawn()
Called at gos_terrain_lod_chunk.cpp:931 (the path explicitly sets the latch — added for the
8z cutover because the legacy sites don't fire here). Preserved by routing (the call is
outside the state block we'd replace).

## 6. sceneHasTerrain_ / prevFrameHadTerrain_
Set via markTerrainDrawn() (931) -> sceneHasTerrain_=true this frame; beginScene saves it to
prevFrameHadTerrain_ + resets. Routing the STATE block does not touch the latch flow ->
preserved. (PASS_TRACE_FIELD candidate per the state-owner audit — add later.)

## 7. Reuse TerrainSolid vs new row — REUSE TerrainSolid
State is identical (§4) and the pass is semantically the same opaque terrain solid. Per the
advisor compromise: **PassTrace pass label = TerrainSolidLODChunk, PipelineDesc = TerrainSolid**
("path is LOD-chunk, state is TerrainSolid"). No new PipelineId needed. The existing frame
trace already labels this path `TerrainSolidLODChunk`.

## 8. applyPipeline call site
Replace the hand-set state block gos_terrain_lod_chunk.cpp:588-592 with
`pipeline_binder::applyPipeline(getPipelineDesc(PipelineId::TerrainSolid), "TerrainSolid")`.
KEEP the save (575-587) + restore (941-945) teardown (self-contained; applyPipeline sets,
teardown restores). glProgramName=0 -> the lod-chunk program bind stays. No colorMask line
needed (keystone owns it).

## 9. PassTrace after routing
The txmmgr dispatch trace currently emits `pass=TerrainSolidLODChunk path=RawGL`. After
routing it should read `path=ApplyPipeline pipeline=TerrainSolid` — but that trace is at the
dispatch chokepoint (txmmgr), which can't see the inner applyPipeline. Update the txmmgr trace
branch for the lod-chunk case to `PathKind::ApplyPipeline, "TerrainSolid"`, and confirm via
`[PIPELINE_BIND] TerrainSolid` binds>0 (the bind trace will now fire — the real proof the
recon-2 conclusion was right and the route is live).

## ONE divergence to note
The debug `s_depthAlways` (default OFF) makes the hand-set use GL_ALWAYS; applyPipeline(TerrainSolid)
forces GEQUAL, dropping that debug override. Default-off -> no production impact. Either accept
(consistent with how other debug overrides were treated) or guard the applyPipeline call behind
`!s_depthAlways`. Recommend ACCEPT + note (the override is a depth-debug aid, not a feature).

## VERDICT -> TERRAIN-LODCHUNK-APPLYPIPELINE-ROUTING-1 (clean GO)
Reuse TerrainSolid; route at 588-592; keep save/restore + markTerrainDrawn; colorMask via
keystone; update the txmmgr lod-chunk trace to ApplyPipeline. Acceptance: build green;
`[FRAME_PLAN] pass=TerrainSolidLODChunk path=ApplyPipeline`; `[PIPELINE_BIND] TerrainSolid`
binds>0 (capture); byte/visual gate clean (sha cb5a700e); markTerrainDrawn/sceneHasTerrain
preserved; legacy MLR not routed; bridge routing not used as proof. THIS is the real promotion
path -> ROUTED_BY_APPLYPIPELINE only after binds>0 + byte-clean.
