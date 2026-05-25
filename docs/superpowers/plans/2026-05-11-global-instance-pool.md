# Static-Prop Global Instance Pool — Plan

**Status:** Plan, not implementation. No code yet.
**Branch:** TBD on top of `claude/nifty-mendeleev` HEAD `b2fb1cc` plus uncommitted per-packet rework + leaf propagation + damage-shape force-load.
**Goal:** Replace per-type slot allocation in the substrate-coalesce path with a single global instance pool, eliminating the per-type cap and unblocking unlimited static-prop counts (current and future denser maps).

---

## Why this slice exists

The current substrate-coalesce implementation (per-packet rework as of 2026-05-11) allocates `s_coalesceInstanceSsbo` as a **fixed slot range per type**:

- `MIN_PER_TYPE_CAP = 256` (the floor) × `typeCount ≈ 548` × `sizeof(GpuStaticPropInstance) = 112 B` × `RING_FRAMES = 3` ≈ 47 MB.
- `cmd.baseInstance` for every packet of type T points into T's group-relative slot range.
- When CPU-side bucket fill exceeds `type.instanceCap`, the coalesce path fires `event=disarmed reason=type_overflow` and falls back to the legacy per-bucket draw branch — which is left holding a stale sorted-layout cmd buffer (per the in-code "Runtime disarm fallback limitation" comment).

Real-world failure mode: at wolfman zoom on stock `mc2_10`, type=280 (a tree) hit count=259 with cap=256 → mid-mission disarm. Visual: most static props vanish. Performance: legacy fallback path is the slow per-call-per-type loop the coalesce was designed to retire.

Bumping the cap is a band-aid:
- Flat 1024 → ~190 MB. Tolerable, but 1k+ trees of one type still overflow on tree-dense maps.
- Flat 10k → ~1.84 GB. Fits on a 7900 XTX (24 GB) but blows portability.
- Per-type peak-sized at finalize → bounded memory for known mission, but **breaks the future "unlimited / denser maps" goal** because future content can always exceed the registry's mission-load count (dynamic spawns, runtime variation, Track-D-style high-poly mech-prop content).

**Per-type allocation is the wrong frame.** Memory should scale with `O(maxVisibleActorsPerFrame)`, not `O(types × per_type_cap)`.

This plan also unblocks:
- **GPU terrain** (denser quad-stream content needs the same global-pool indexing model).
- **GPU mech batching** (Track D Slice C — same instance-pool pattern with per-actor records).
- **Tree-as-particle-cloud** future content (hundreds of thousands of instances; per-type slot model can't represent this).

The user's framing: *"everything else (like gpu terrain) is actually blocked on this."*

---

## Target architecture

### Data structures (proposed)

**Replace** `s_coalesceInstanceSsbo` (per-type slot ranges) with a **single flat pool**:

```cpp
GLuint s_globalInstancePool;          // GL_SHADER_STORAGE_BUFFER, persistent-mapped, RING_FRAMES x s_globalInstanceCapacity
size_t s_globalInstanceCapacity;      // single global budget; default 65536, env-tunable
```

The pool holds `GpuStaticPropInstance` records in a **type-sorted contiguous layout** within each ring frame.

### Per-frame fill (GPU-driven)

The cull dispatch (`gpu_cull.comp`) already produces a per-bucket count and writes per-actor visibility bits. Two sub-options:

**Option A — atomicAdd cursor + sort.** Cull writes visible records into the global pool with `atomicAdd` on a per-bucket cursor table. A second-pass sort (or radix sort) brings them into per-type-contiguous order.

**Option B — two-pass scan.** First pass counts per-bucket. Prefix sum gives per-type firstInstance offsets. Second pass writes records at deterministic positions.

Option B is the cleaner path for ordering correctness (no atomicAdd contention, no sort step) but doubles the cull dispatch count. Option A is simpler to ship but has an ordering-dependent edge for the post-cull patch shader.

**Decision pending — pick at design-write time, not now.** Write the recon doc that grounds both options against the existing cull shader before committing.

### Patch shader output

`gpu_cull_patch.comp` reads the per-bucket prefix sums (or post-sort cursors), then for each cmd `c`:
- `cmd.firstInstance = bucketFirstInstance[cmdToBucket[c]]` (replaces today's per-type-cap accumulator)
- `cmd.instanceCount = bucketCount[cmdToBucket[c]]` (unchanged)

The `cmdToBucket` SSBO from the per-packet rework stays — it's what makes per-packet cmds in one type share the type's instance range.

### Rendering side (mostly unchanged)

`GpuStaticPropBatcher::flush()` per-packet draw branch:
- Uses `glMultiDrawElementsIndirect` exactly as today.
- The two `glBindBufferRange` calls (alpha-OFF and alpha-ON groups) become **a single bind** of `s_globalInstancePool` covering the entire pool (no group split — alpha behavior is per-fragment via per-packet `materialFlags`).
- `s_perDrawSsbo` (per-packet) stays unchanged: `texArrayLayer`, `materialFlags`, `uvScale` all remain per-packet.
- No `s_alphaOff/OnCmdCount` split needed — single multidraw covers everything (or two for binding-the-right-tex-array, see below).

### Texture array binding

The current alpha-OFF / alpha-ON texture array split exists to bind one of `s_texArrayOff` / `s_texArrayOn` per draw. Per-packet textures live in whichever group their type was assigned to. Two paths:

1. **Keep the two-array split.** Two `glMultiDrawElementsIndirect` calls — one per group — like today. cmdToBucket and the global pool work the same.
2. **Single merged texture array.** All packet textures go into one array; one draw call for everything. Simpler, but memory cost = `(N_off + N_on) × max(W,H)²` (more padding waste on small textures).

Recommend (1) for the first slice: minimal disruption, keeps the alpha-test path and shader inputs unchanged. (2) is a follow-up.

### Memory math

- Default `s_globalInstanceCapacity = 65536` instances. At 112 B each: 7.3 MB. × RING_FRAMES = 21 MB. Compare to current 47 MB at flat 256 cap.
- Stress configurations (denser maps) bump the global cap. Single env knob (`MC2_STATIC_PROP_GLOBAL_CAP`) replaces today's per-type tuning.
- No more memory-vs-disarm tradeoff — overflow is "too many actually visible at once" globally, which is much harder to hit and the right thing to fail on.

---

## Migration steps

The per-packet rework (2026-05-11) is the foundation. This plan extends it.

1. **Recon doc.** Audit `gpu_cull.comp` cull-write site for the bucket-cursor and visibility-bit semantics. Identify what changes for Option A vs B. Output: a design doc with Option B's prefix-sum dispatch sketched as GLSL.
2. **Design doc.** `s_globalInstancePool` layout, ring-buffer fence semantics, cull-shader changes, patch-shader changes, flush() draw-side changes. Memory budget. Forced-disarm hooks (still needed for malformed_type / tex_evicted / alloc_failed paths). Soak test plan.
3. **Implementation slice 1 — global pool only.** Allocate the new pool, drop `s_coalesceInstanceSsbo`'s per-type-cap math, keep the SSBO write side as a CPU `memcpy` from `bucket.instances` into a global cursor (CPU-driven, single pool). Cull/patch unchanged. Verify per-packet rendering still correct. *This is the smallest-blast-radius change that lets the cap go away.*
4. **Implementation slice 2 — GPU-driven write.** Cull writes directly to `s_globalInstancePool`. CPU-side `bucket.instances` retired. This is where the perf-win comes from — per-frame CPU memcpy of all visible instances goes to ~0.
5. **Implementation slice 3 — single texture array.** Only after 1 + 2 are stable. Merge `s_texArrayOff` and `s_texArrayOn` into one. One multidraw per frame total.

Each slice ends with tier1 5/5 PASS and a `mc2_10` 90s wolfman-zoom verification (no disarm, no FPS cliff).

---

## Acceptance criteria

| Criterion | How to verify |
|---|---|
| No `event=disarmed reason=type_overflow` at wolfman zoom on `mc2_10` | 90s smoke run, grep stderr |
| No visual regression vs current per-packet path | screenshot diff against `tests/smoke/artifacts/diag-shots/mc2_10-dmg-fix-t{14,26,30}s.png` |
| Tier1 substrate=ON 5/5 PASS, all missions ≥ ship-config baseline (128 fps avg on mc2_10) | `MC2_GPU_CULL_SUBSTRATE=1 py -3 scripts/run_smoke.py --tier tier1 --duration 30` |
| Memory regression ≤ 0 vs current path on any tier1 mission | log `[COALESCE v1] event=ready` line size; compare |
| `render textureManager` Tracy zone time drops vs current 2.1 ms (after slice 2) | Tracy capture mid-mission |

Slice 1 is allowed to be *no perf change* — the goal is correctness + cap-removal. Slice 2 is where the perf win lands.

---

## What this slice does NOT do

- **Does not rework per-packet draw cmds.** That's already shipped (2026-05-11).
- **Does not retire the legacy fallback** (`MC2_SUBSTRATE_COALESCE_LEGACY=1`). That stays as a safety net through the soak window.
- **Does not address the gameplay-side 16.81 ms `Mission.Update`** Tracy zone. That's the parallel `MC2_STATIC_UPDATE_SKIP=1` regression investigation (`memory/update_skip_touch_regression.md`).
- **Does not unify mech / vehicle / static-prop instance pools.** Future slice. The static-prop pool is a working pattern others can adopt.

---

## Risks

| Risk | Mitigation |
|---|---|
| Cull-shader rework introduces visual regression | Slice 1 keeps cull unchanged; only slice 2 touches it. |
| Atomic-add ordering issues in Option A | Pick Option B at design time (deterministic prefix-sum). |
| Per-mission peak still exceeds global cap | Single env knob to bump; failure mode is bounded (one global allocation, not 548 per-type allocations). |
| Per-frame ring fence stalls under denser content | Already bounded by RING_FRAMES = 3; bigger pool doesn't change the fence cycle. |

---

## Why this is the durable answer

The user framed it precisely: *"this should be flexible enough that in the future we don't have to rework it."* The per-type-cap model is wrong because it hard-codes an assumption (uniform per-type budget) that doesn't hold for the content this engine wants to support. The global-pool model removes that assumption — memory scales with what's actually visible, not with how many type slots we pre-allocated. Future content can be 10× denser without touching the architecture; only the global cap moves.

Once shipped, the same instance-pool pattern is reusable for:
- GPU mech batching (Track D Slice C+).
- GPU terrain quad streams.
- Particle-cloud trees / vegetation.
- Any GPU-driven instanced rendering this engine grows into.
