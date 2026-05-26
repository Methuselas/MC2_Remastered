# DrawPacket v4C Instance Count Pipeline Recon

**Date:** 2026-05-26  
**Purpose:** Architectural understanding of instance count availability for DrawPacket v4C coalesce-path static prop drawing.

---

## Q1: GPU Cull Compute — Where Do Per-Packet Instance Counts Land?

### Key Finding: GPU-cull counts are WRITE-ONLY to indirect buffer; no CPU-readable snapshot

**Source:** gpu_cull_compute.cpp lines 798-804

Returns just the GL buffer ID; no mapped buffer or CPU-accessible copy.

### Indirect Buffer Structure (line 551-560)

DrawElementsIndirectCommand struct has instanceCount field, overwritten per-frame by patch dispatch.

### instanceCount Write Path

1. **GPU Cull Shader**: Appends visible actor IDs to visibleIds SSBO (binding 9), increments per-bucket counters (binding 10).
2. **Patch Dispatch**: Reads perBucketCount from binding 10, writes instanceCount into indirect buffer.
3. **Barrier**: compute_dispatch() inserts GL_COMMAND_BARRIER_BIT before flush() call.

### CPU-Readable Location?

NO direct CPU read without stall.

- Option A (blocking): glGetNamedBufferSubData() — stalls GPU, ~6ms penalty on mc2_10.
- Option B (async): Persistent-mapped buffer next frame — 1-frame latency, not currently implemented.
- Option C (shadow mode C1a): CPU computes counts from snapshot, GPU runs alongside for parity.

**Conclusion:** In C1b (GPU authority), per-packet instance counts exist ONLY on GPU in indirect buffer, pending draw call. No per-packet count available on CPU at dispatch time without GPU stall.

---

## Q2: batcher_prepareBaseInstanceTable() — What Instance Counts Does It Know About?

### Key Finding: CPU-side counts are per-TYPE, sourced from snapshot accumulation in s_bucketsByType

**Source:** gos_static_prop_batcher.cpp lines 4802-4898

### Per-Frame Function Behavior

batcher_prepareBaseInstanceTable() advances coalesce ring slot, waits on fence, then computes baseInstance prefix-sum for each type by reading bucket.instances.size() from THIS FRAME's s_bucketsByType.

### What is s_offGroupCountThisFrame?

- **Type:** uint32_t
- **Semantics:** Sum of alpha-OFF (opaque) instance counts across ALL types; boundary offset separating opaque from alpha-ON in global pool.
- **NOT per-packet:** Accumulated from per-type bucket sizes.
- **Update:** Set once per frame in prepareBaseInstanceTable() line 4894; read during flush() memcpy line 3597.

### Source of Instance Counts: s_bucketsByType

Unordered_map populated by batcher_submit() (line 2524) during GpuStaticPropRegistry::submitMultiShape(). Data source: snapshot-provided GpuStaticPropInstance records (frame-local).

Count at dispatch: kvIt->second.instances.size() — purely CPU-side, no GPU reads.

### s_baseInstanceByCmdSsbo Purpose

Persistent-mapped SSBO (binding 16 in global-pool mode) holding per-packet baseInstance prefix-sum. Updated by prepareBaseInstanceTable() lines 4883-4891. Read by patch shader when global-pool mode active.

---

## Q3: Legacy Mode Instance Counts

### Key Finding: Legacy mode uses CPU-computed per-type counts from snapshot; NO GPU reads

**Source:** gos_static_prop_batcher.cpp lines 3949-4120

### Legacy Draw Loop

Iterates typeID, reads r.instanceCount from TypeRangeSsbo, passes to glDrawElementsInstancedBaseVertex.

### Where r.instanceCount Comes From

TypeRangeSsbo struct (line 189-195) populated in uploadAllBucketsIfNeeded() (lines 3141-3164):

r.instanceCount = static_cast<uint32_t>(b.instances.size());

This is the snapshot count at that moment.

### Timeline

1. registerType() — immutable after finalizeGeometry
2. Frame N: snapshot collection — GpuStaticPropRegistry populates s_bucketsByType
3. Frame N: uploadAllBucketsIfNeeded() — TypeRangeSsbo built from bucket size
4. Frame N: flush() — legacy loop reads r.instanceCount

**No GPU communication required.**

---

## Q4: Snapshot-Side Instance Count Availability

### Key Finding: Snapshot carries per-type counts implicitly via frame-local bucket accumulation; no explicit count field

**Source:** gameosmain.cpp lines 1175-1302

### Snapshot Structure

snap.staticProps is a vector of GpuStaticPropInstance (frame-local). DrawPacketEmitStats carries emitted count and derived stats, but NO per-type breakdown.

### How Per-Type Counts Become Available

1. GpuStaticPropRegistry::submitMultiShape() iterates snapshot instances, calls batcher_submit() per instance.
2. batcher_submit() appends to s_bucketsByType[typeID].instances[].
3. uploadAllBucketsIfNeeded() reads bucket.instances.size(), builds TypeRangeSsbo.instanceCount.

Accumulation location: s_bucketsByType — per-frame module-local map.

### Per-Type Peak Tracking

s_perTypePeak tracks peak instance count per typeID since mission load (reset per-mission). Foundation for Slice-2 GPU-emit.

---

## Q5: s_opaqueViews / StaticPropOpaquePacketView

### Key Finding: v4A dispatch draws per-packet without querying instance count; uses glDrawElementsBaseVertex (single instance per draw)

**Source:** gos_static_prop_batcher.cpp lines 3355-3423

### v4A Substitutive Opaque Dispatch

For each candidate packet, binds per-type SSBO range and calls glDrawElementsBaseVertex (not instanced).

### Instance Count for v4A

- NOT queried dynamically — v4A per-packet draw does NOT reference r.instanceCount.
- Uses glDrawElementsBaseVertex() (single instance geometry), not instanced.
- Per-type instanceCount still in tr (used for stats line 3958), but NOT passed to draw.

v4A is substitutive: replaces legacy per-type loop with per-packet draws for opaque types only. Each packet drawn once; per-type instance count is irrelevant for individual packet geometry.

---

## Summary: Instance Count Sources at CPU Dispatch Time

### Available WITHOUT GPU Stall

| Source | Scope | When | Usage |
|--------|-------|------|-------|
| s_bucketsByType[typeID].instances.size() | Per-type | Snapshot accumulation | Legacy draw, coalesce memcpy, baseInstance prefix-sum |
| s_typeRanges[typeID].instanceCount | Per-type | uploadAllBucketsIfNeeded | Legacy glDrawElementsInstancedBaseVertex |
| s_offGroupCountThisFrame | Global | prepareBaseInstanceTable | Coalesce off/on group boundary |
| s_perTypePeak[typeID] | Peak | Per-frame update | Stats; Slice-2 future |

### Available WITH GPU Stall (stalls ~6ms)

| Source | Scope | Usage |
|--------|-------|-------|
| bucketCountsBuf (binding 10) | Per-type | Parity diagnostic (dev-only) |

### GPU-Authority-Only

| Source | Scope | Access |
|--------|-------|--------|
| s_indirectCmdBuf instanceCount | Per-bucket | GPU reads only; glDrawElementsIndirect |

---

## Architectural Recommendation for DrawPacket v4C

### Which Instance Count Source?

**For v4C coalesce-path, use s_bucketsByType counts (CPU-side, zero-stall):**

1. batcher_prepareBaseInstanceTable() already called before flush(), ensuring s_bucketsByType finalized.
2. No GPU round-trip; counts snapshot-derived and CPU-computed.
3. Granularity: per-type; if v4C per-packet, scale proportionally or use per-packet granularity.
4. Coalesce integration: s_bucketsByType already feeds baseInstance computation, memcpy size, global-pool groupCursor.
5. Legacy fallback: r.instanceCount from same snapshot source — consistency guaranteed.

### Avoid GPU-Cull Counts for CPU Dispatch

Per-packet GPU counts (in s_indirectCmdBuf post-patch) are GPU-authoritative but CPU-inaccessible without stall. Use case: GPU-driven rendering, not CPU dispatch decisions.

---

## Key Invariants

1. **C1b snapshot-sync:** CPU counts should approximate GPU counts modulo visibility changes. Sticky-bit temporal expansion absorbs 1-frame lag.

2. **Global-pool CRIT-2:** coalesce ring slot advanced in prepareBaseInstanceTable() BEFORE guards; memcpy/draw always read fresh slot (line 4809).

3. **Coalesce disarm gate:** If totalCount > globalInstanceCap, disarm and log; legacy absorbs fallback (line 4866-4871).

4. **Per-packet vs per-type:** Sorted (coalesce) branch: cmds[] per-packet; all packets of one type share baseInstanceForType[typeID]. Natural (legacy): cmds[] per-type. Both converge on per-type counts.

---

## Related Files

- **GPU Cull:** gpu_cull_compute.cpp, gpu_cull.comp, gpu_cull_patch.comp
- **Batcher:** gos_static_prop_batcher.cpp, gos_static_prop_batcher.h
- **Registry:** GpuStaticPropRegistry (gameosmain.cpp, lines 1050-1400)
- **Snapshot:** emitStaticPropDrawPackets() (gameosmain.cpp)
