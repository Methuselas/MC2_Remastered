# Substrate multi-draw coalesce — design

**Status:** DESIGN ONLY. No source changes. Awaiting `adversarial-plan-review`
pass before any implementation slice is opened.

**Origin context:** memory `track_c_substrate_regression.md`. At mc2_01 normal
zoom, `MC2_GPU_CULL_SUBSTRATE=1` inflates `Render.GpuStaticProps` from ~120 µs
to ~2 ms. CPU-side patch dispatch is fine (`elapsed_us=4` across 323 type
buckets). The cost is GPU pipeline serialization — one `glDrawElementsIndirect`
per type forces a flush point per type. Multi-draw lets the driver pipeline
the commands.

**Worktree where the design code lives:** `claude/nifty-mendeleev`
(this worktree). All file:line citations resolve there.

---

## 1. Code grounding

Each cited symbol grep-verified at write-time per worktree CLAUDE.md
"Documentation Discipline."

### 1.1 The per-bucket draw loop (the regression site)

**Memory file claim:** `gpu_cull_compute.cpp`. **Actual:** the loop lives in
the batcher's flush path, not in `gpu_cull_compute.cpp`. The compute file
ends at the `GL_COMMAND_BARRIER_BIT` and hands off to the batcher.

- Loop head:
  `GameOS/gameos/gos_static_prop_batcher.cpp:1542` — `for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID)`
- Inner per-packet loop:
  `GameOS/gameos/gos_static_prop_batcher.cpp:1652` — `for (uint32_t p = 0; p < type.packetCount; ++p)`
- The per-call site:
  `GameOS/gameos/gos_static_prop_batcher.cpp:1704` —
  `glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<const void*>(cmdOffset));`
- Indirect buffer bind/unbind around each call:
  `gos_static_prop_batcher.cpp:1702` and `:1706` (re-binds and zeroes
  `GL_DRAW_INDIRECT_BUFFER` per call — itself a state-thrash cost).
- Per-packet texture bind: `gos_static_prop_batcher.cpp:1670–1671`.
- Per-packet uniform writes: `gos_static_prop_batcher.cpp:1685` (`u_materialFlags`),
  `:1687` (`u_packetID`).

**Important correctness observation surfaced during recon (NOT a multi-draw
issue, but unavoidable when reasoning about the loop body):** the inner
packet loop calls `glDrawElementsIndirect` against a per-type indirect
command whose `count` is `totalIndexCount = sum(packet.indexCount)` for the
type — see `batcher_getTypeDrawInfo` at
`gos_static_prop_batcher.cpp:1970–1980`. So each per-packet iteration draws
the whole type's index range, with `gl_VertexID`/`gl_InstanceID` rebased per
draw, just with a different bound texture and `u_packetID` uniform. The
packet loop is currently providing **per-packet texture/uniform variation
across the same vertex range**, not partitioning the index range. This
materially changes the multi-draw schema (Section 3) — a multi-draw call
cannot rebind a texture between commands inside the same multi-draw.

The actual GPU draw count therefore is
`Σ_t packetCount[t]`, NOT the bucket count. With 323 types and an average
~1.x packet per type (most static props have a single material), the
practical draw count is in the 300–500 range. The memory file's "323
buckets" matches the indirect-buffer entry count but understates the draw
issue count by the average packet multiplier.

### 1.2 Indirect command buffer construction site

- Buffer name declaration: `GameOS/gameos/gpu_cull_compute.cpp:81` —
  `static GLuint s_indirectCmdBuf = 0;`
- `DrawCmd` struct (the one the GL spec calls
  `DrawElementsIndirectCommand`): `gpu_cull_compute.cpp:507–514`. Five
  `GLuint`/`GLint` fields: `count, instanceCount, firstIndex, baseVertex,
  baseInstance`. `static_assert(sizeof(DrawCmd) == 20, …)` at line 514.
- Allocation + initial upload: `gpu_cull_compute.cpp:551–555` —
  `glBufferData(GL_DRAW_INDIRECT_BUFFER, indirectBytes, cmds.data(),
  GL_DYNAMIC_DRAW);` Total size = `typeCount * 20` bytes.
- Per-frame `instanceCount` patch (GPU-authoritative): the patch compute
  shader writes via SSBO binding `INDIRECT_CMD_BINDING` at
  `gpu_cull_compute.cpp:798` and dispatch at `:805`.
- Accessor used by the batcher: `compute_getIndirectCmdBuf()` at
  `gpu_cull_compute.cpp:621` (returns `s_indirectCmdBuf`).

The CPU-side build loop iterating types is at
`gpu_cull_compute.cpp:522–545`. It reads geometry via
`batcher_getTypeDrawInfo` (declared at `gos_static_prop_batcher.h:269`,
defined at `gos_static_prop_batcher.cpp:1954`) and computes a cumulative
`baseInstance` (= base offset into `visibleIds[]`).

### 1.3 Bucket / per-type metadata source

- `GpuStaticPropType`: `GameOS/gameos/gos_static_prop_batcher.h:87–92`.
  Fields: `firstPacket, packetCount, vertexCount, source` (TG_TypeShape*).
  **There is no per-type program/blend/depth state field.**
- `GpuStaticPropPacket`: `gos_static_prop_batcher.h:38–48`. Per-packet:
  `firstIndex, indexCount, baseVertex, textureSlot, materialFlags,
  owningTypeID`. **The state delta between packets is exactly:** texture
  (resolved at draw time via `textureSlot`), and `materialFlags` (alpha-test
  bit only, per `STATIC_PROP_FLAG_ALPHA_TEST` at `gos_static_prop_batcher.h:58`).

### 1.4 Whole-pass GL state setup (shared across all buckets)

- Program bind: `gos_static_prop_batcher.cpp:1433` — single
  `glUseProgram(s_staticPropProgram);`
- VAO bind: `gos_static_prop_batcher.cpp:1434` — single
  `glBindVertexArray(s_sharedVao);`
- Depth: `gos_static_prop_batcher.cpp:1437–1439` — `GL_DEPTH_TEST` on,
  `glDepthMask(GL_TRUE)`, `glDepthFunc(GL_LEQUAL)`.
- Blend: `gos_static_prop_batcher.cpp:1440` — `glDisable(GL_BLEND)`.
- Cull: `gos_static_prop_batcher.cpp:1441–1442` — `GL_CULL_FACE` on,
  `glCullFace(GL_BACK)`.
- AMD attribute-0 hygiene: `gos_static_prop_batcher.cpp:727` (in
  `setupSharedVao`) — `glEnableVertexAttribArray(0);` once at VAO creation,
  not per draw.

**Verified property:** every per-bucket draw inherits identical
program/VAO/depth/blend/cull/colormask/sampler state. The only
inter-bucket dirty state is (a) texture binding on unit 0, (b)
`u_materialFlags` int uniform, (c) `u_packetID` int uniform, (d) bound
SSBO ranges on bindings 0 and 1 (instance + color, per type).

### 1.5 Barrier sequence around the dispatch

- After cull dispatch: `gpu_cull_compute.cpp:790` —
  `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);`
- After patch dispatch (the one this design depends on):
  `gpu_cull_compute.cpp:813` —
  `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);`
- Sequencing in `txmmgr.cpp:1762–1766` —
  `compute_dispatch()` (which contains the barrier above) is called
  **before** `GpuStaticPropBatcher::flush()` so the indirect buffer's
  GPU-written `instanceCount` is visible to the draw.

The `GL_COMMAND_BARRIER_BIT` is what makes the GPU-driven instance counts
visible to **any** indirect-draw entrypoint. Multi-draw is just one such
entrypoint, so the barrier is reused as-is — no schema change here.

### 1.6 PR1 indirect-terrain SOLID precedent

- `GameOS/gameos/gameos_graphics.cpp:2410` —
  `glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, (GLsizei)cmdCount, 0);`
- Banner comment block: `gameos_graphics.cpp:2210–2213` covers the
  AMD attr-0, sampler, and "no EBO" hygiene the indirect-terrain path
  established.
- Reference design: `docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md`.

**NOTE on memory accuracy:** the existing memory file
`indirect_terrain_solid_endpoint.md` cites `gameos_graphics.cpp:2321` for the
multi-draw call. The actual line at write-time is **2410**. The line shifted
by ~89 lines after the cement-multi-sampler PR2 landed (commit dd...). This
design treats the symbol, not the line, as authoritative. Cite verified.

---

## 2. State-class grouping analysis

Multi-draw issues N commands under one shared GL state set. We must
enumerate every dimension that varies per bucket and either (a) prove it
shares a value across all 323 buckets (collapse to one group), or (b)
partition the buckets into groups by that dimension.

### 2.1 Per-bucket state dimensions, with measured cardinality

| Dimension | Source code | Distinct values across 323 buckets | Notes |
|---|---|---|---|
| Program | single bind at `gos_static_prop_batcher.cpp:1433` | **1** (`s_staticPropProgram`) | shared |
| VAO | single bind at `gos_static_prop_batcher.cpp:1434` | **1** (`s_sharedVao`) | shared |
| Vertex layout | `setupSharedVao` at `gos_static_prop_batcher.cpp:~720` | **1** | shared |
| Depth test/func/mask | `gos_static_prop_batcher.cpp:1437–1439` | **1** | shared |
| Blend enable + func | `gos_static_prop_batcher.cpp:1440` | **1** (disabled) | shared |
| Cull face | `gos_static_prop_batcher.cpp:1441–1442` | **1** | shared |
| Colormask | not touched in flush — inherits | **1** (assumed; verify in slice 0) | inherited |
| Sampler unit 0 wrap | per-iter at `:1700–1701` (REPEAT/REPEAT) | **1** (always REPEAT) | redundant per iter |
| Sampler unit 0 filter | not set in loop — inherits texture state | **1** (assumed LINEAR) | inherited; verify |
| Texture binding unit 0 | per-packet at `:1670–1671` | **N_textures** — every packet may have a unique texture | **VARIES per command** |
| `u_materialFlags` uniform | per-packet at `:1685` | **2** values: alpha-test bit on/off | **VARIES per command** |
| `u_packetID` uniform | per-packet at `:1687` | one per packet, ~`packetCount` distinct | **VARIES per command** — but only used for indexing into a per-packet SSBO; can be lifted to SSBO of-size-N |
| SSBO binding 0 (instance) | per-type at `:1554` | **N_types** | per-type — but each type's range is contiguous in `s_instanceSsbo`; can be eliminated by making all draws read a single-bound full-buffer SSBO and addressing via `gl_DrawID + baseInstance` |
| SSBO binding 1 (color) | per-type at `:1557` | **N_types** | same elimination |

### 2.2 Grouping verdict

**Multi-draw groups required by GL state alone: 1.** All 323 buckets share
program/VAO/depth/blend/cull. The "323 → 7 groups" framing in the request
brief turns out to be optimistic in the wrong direction: it assumed multiple
blend/depth states. There are none. Static props are a single-state class.

**However**, three per-command varying inputs prevent a literal one-shot
multi-draw without a shader-side change:

1. **Per-packet texture bind.** Multi-draw cannot rebind a sampler texture
   between commands inside the call. Fix: bindless textures **OR** a
   texture array **OR** a texture atlas. The
   `indirect-terrain-draw-design.md` precedent uses the merged colormap
   atlas (single bind, atlas tile coords in the per-vertex stream). For
   static props, **bindless** is the cleanest match because each
   TG_TypeShape's textures already live as independent `glTexture`
   handles — atlasing 323 types with arbitrary alpha-mask geometry is a
   much larger PR than the multi-draw substrate change. Cement-multi-sampler
   v2 (`docs/superpowers/plans/2026-05-01-cement-multi-sampler-plan-v2.md`)
   is the relevant prior art for the sampler-array variant.

2. **`u_materialFlags` uniform.** Two-valued (alpha-test on/off). Promote
   to a per-packet SSBO entry indexed by `gl_DrawID` (multi-draw exposes
   this); shader does `materialFlags = matFlagsTbl[gl_DrawID];`. Or split
   into TWO multi-draw groups: one alpha-tested, one not. The latter is
   simpler and avoids touching the shader bindings table.

3. **Per-type SSBO range bind (binding 0 instance, binding 1 color).** This
   is the load-bearing one. Today each type binds a *range* into the
   shared `s_instanceSsbo` so the shader reads `gl_InstanceID` 0..N-1 in a
   per-type tight window. Under multi-draw, all commands share one bound
   range. Fix: bind the **whole** `s_instanceSsbo` and `s_colorSsbo`, and
   have the shader compute its instance-buffer index as
   `baseInstance_for_this_draw + gl_InstanceID`. The `baseInstance` field
   of `DrawElementsIndirectCommand` already exists in our struct
   (`gpu_cull_compute.cpp:512`) and is already used for `visibleIds[]`
   indexing — so the same value works for instance/color SSBO indexing as
   long as the per-type ranges are placed at offsets matching `baseInstance`
   in the shared SSBO. Verify that placement invariant in slice 0; today
   the cumulative-base computation at `gpu_cull_compute.cpp:521,541` is
   `cumBase += instanceCap` for each type, so the SSBO layout would need
   to mirror that exactly. Currently the per-type ranges in
   `s_instanceSsbo` use a separate `TypeRangeSsbo` map (looked up at
   `gos_static_prop_batcher.cpp:1543–1556`) — its
   `r.instanceByteOffset` must equal `baseInstance * sizeof(instance)` for
   every type, or the SSBO layout has to be re-packed at registration time.

### 2.3 Final group count

**Recommendation: 2 multi-draw groups.**
- Group 0: alpha-test OFF (most static props — buildings, generic).
- Group 1: alpha-test ON (trees, fences, gates — sparse minority).

Reasons: avoids a uniform-table SSBO; matches the shader's existing
`u_materialFlags` early-discard branch with no shader change; the alpha
classification is already known at registration time
(`bdactor.cpp` etc., re-resolved at draw time from
`src->listOfTextures[slot].textureAlpha` per `gos_static_prop_batcher.cpp:1677–1680`).

The texture-binding problem is **independent** of group count — it must be
solved by atlas/array/bindless regardless. Solving it pulls in a separate
slice. **Slice the work as Stage A (multi-draw with bindless textures or
sampler array) and Stage B (alpha-test split into two groups).** Stage A is
the perf payoff; Stage B is a mechanical follow-up.

---

## 3. Indirect-command-buffer schema

### 3.1 Today's schema (verified at write-time)

`gpu_cull_compute.cpp:507–514`:

```cpp
struct DrawCmd {
    GLuint count;           // total index count for this type (sum across packets)
    GLuint instanceCount;   // GPU-written each frame by patch shader
    GLuint firstIndex;      // first packet's firstIndex
    GLint  baseVertex;      // first packet's baseVertex
    GLuint baseInstance;    // cumulative base into visibleIds[]
};
static_assert(sizeof(DrawCmd) == 20, "...");
```

Buffer total: `typeCount * 20` bytes, layout strictly indexed by `typeID`.
Patch dispatch writes `cmds[t].instanceCount` for `t in [0, typeCount)`.

### 3.2 New schema for multi-draw

The struct **does not change**. `DrawElementsIndirectCommand` is exactly
what `glMultiDrawElementsIndirect` reads. The change is in:

- **Layout/order:** commands grouped by alpha-test class must be
  contiguous within each group. Today's layout is `cmds[typeID]`, which
  intermixes alpha and non-alpha types arbitrarily by registration order.
- **Group bookkeeping:** the batcher needs `(group_first, group_count)`
  for each multi-draw group. Two pairs total for Stage A+B.

### 3.3 Sort strategy: registration-time (recommendation)

Two options:

**(a) Sort at registration time.** When `compute_buildIndirectBuffer` runs
at mission load (called from `gpu_cull_compute.cpp:482+`), it walks types
0..N-1 to fill `cmds[]`. Add a second pass that produces a permutation
`reordered[g][i] -> originalTypeID` such that:
- group 0 = all types where any packet has `materialFlags & ALPHA_TEST == 0`
- group 1 = all types where any packet has `ALPHA_TEST == 1`

(Mixed-class types — multiple packets, some alpha some not — are rare in
practice; verify by counting at registration time; if non-zero, split the
type into two virtual buckets at registration.)

The permuted `cmds[]` is written in group order. The patch dispatch must
use the same permutation when writing `instanceCount` — the shader knows
each input's groupID via the SSBO it reads from, so the same permutation
is supplied as a uniform array `permutation[N]` or, simpler, baked into
the SSBO indices used at registration.

**Cost:** one pass at mission load, zero per-frame cost. No registration
order is preserved elsewhere — verify by grep'ing `s_types[t]` consumers
and confirming none rely on ordering. Today's consumers (the flush loop
itself, the patch shader, `batcher_getTypeDrawInfo`) all use `typeID` as
an opaque index, so a permutation is invisible to them as long as it's
applied symmetrically.

**(b) Sort per-frame in compute.** Reject. Per-frame sorting requires either
a GPU sort (overkill for ~few hundred entries) or a CPU sort + upload (the
~2 ms savings would be reabsorbed). Use only if registration-time ordering
turns out to break a downstream consumer not yet found.

**Recommend (a).** Verify in slice 0 that registration order has no
load-bearing consumer.

### 3.4 Read-side shader access

For per-instance SSBO indexing without per-type rebinds, the shader needs
to know its absolute instance index = `baseInstance + gl_InstanceID`. In
GLSL, `baseInstance` is exposed as `gl_BaseInstance` only if the
`ARB_shader_draw_parameters` extension is available. Verify GL 4.3 +
extension support before relying on this. Fallback: pack `baseInstance` into
a per-draw SSBO slot indexed by `gl_DrawID` (also requires
`ARB_shader_draw_parameters`).

If neither extension is available on the target driver: **the design
collapses to single-bind-of-whole-instance-buffer + per-draw uniform
update**, which defeats multi-draw. Slice 0 must include an ARB extension
probe and a hard fail-out (fall back to current loop) if missing.

The AMD RX 7900 XTX target does support `ARB_shader_draw_parameters` (core
in 4.6, extension in 4.3). The MC2 GL context per worktree CLAUDE.md
"Critical Rules" is 4.3 + extensions, so the probe is necessary but
expected to pass.

---

## 4. Sequencing

The per-frame order does NOT change. Only the inner draw issue changes.

### 4.1 Today's order (verified)

`mclib/txmmgr.cpp:1753–1766`:

```
GpuStaticPropRegistry::flush();        // appends substrate records
gpu_cull::compute_dispatch();          // cull → barrier → patch → barrier
GpuStaticPropBatcher::instance().flush();
```

Inside `compute_dispatch`:
- `glDispatchCompute(cullGroups, ...)` (gpu_cull_compute.cpp:785)
- `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` (:790)
- `glDispatchCompute(patchGroups, ...)` (:805)
- `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)` (:813)

Inside `flush()`:
- per-bucket loop (`:1542`) → per-packet inner loop (`:1652`) → per-call
  `glDrawElementsIndirect` (`:1704`).

### 4.2 New order (Stage A, multi-draw with bindless textures)

```
GpuStaticPropRegistry::flush();
gpu_cull::compute_dispatch();          // unchanged: barrier already covers indirect+SSBO

// Inside flush():
glUseProgram(s_staticPropProgram);
glBindVertexArray(s_sharedVao);
glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDepthFunc(GL_LEQUAL);
glDisable(GL_BLEND); glEnable(GL_CULL_FACE); glCullFace(GL_BACK);

// Bind whole instance + color SSBOs (no ranges).
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo);
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s_colorSsbo);

// Bind bindless texture handle table (new SSBO populated at registration).
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, K, s_textureHandleTable);

// Sampler hygiene (REPEAT, set ONCE not per draw).
// ...

glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());

// Group 0: alpha-test off.
glUniform1i(loc_materialFlags, 0);
glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
    reinterpret_cast<const void*>(group0_first * sizeof(DrawCmd)),
    group0_count, sizeof(DrawCmd));

// Group 1: alpha-test on.
glUniform1i(loc_materialFlags, STATIC_PROP_FLAG_ALPHA_TEST);
glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
    reinterpret_cast<const void*>(group1_first * sizeof(DrawCmd)),
    group1_count, sizeof(DrawCmd));

glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
```

Two GL draw issues replace 300+. Texture binding is gone (lifted into
shader sampling against `gl_DrawID`-indexed bindless table). Per-packet
uniform writes are gone (one per group, two total).

### 4.3 Stage B fallback (no bindless, sampler array)

If bindless is rejected (driver coverage), use a sampler array bound to
N texture units (or a 2D array texture if all static-prop textures can be
re-loaded at the same dimensions — they cannot, in general). The cement
multi-sampler v2 plan covers exactly this trade-off; reuse its conclusions.
Without any of {bindless, sampler array, atlas}, **the multi-draw cannot
proceed** and the design has to fall back to "fewer-draws-per-packet via
type-level batching" only — perf savings drop from ~1.9 ms to maybe
~30 % (one fewer call per type, not one call total).

---

## 5. Parity gate

### 5.1 Failure mode

Silent visual corruption: a bucket grouped into the wrong multi-draw
group reads the wrong `u_materialFlags`, so an alpha-test-required tree
sprite renders as a solid quad (no discard), or vice versa. With bindless
textures, a wrong handle-table index reads a different texture entirely —
buildings textured as trees. None of these crash; smoke tier 1 might or
might not catch them depending on which assets are visible in the 5 fixed
missions.

### 5.2 Parity test

Three layers, in order of increasing rigor:

**(a) Per-bucket draw-count + state assertion (cheap, default-on).** Add
a debug-mode env (`MC2_GPU_CULL_MULTIDRAW_AUDIT=1`) that, after the new
multi-draw issue, walks the indirect command buffer with
`glGetBufferSubData` and asserts:
- `Σ instanceCount` matches the legacy loop's per-frame
  `s_counters.gpu_drawn_instances` total.
- Group 0 contains only types where every packet has `ALPHA_TEST == 0`.
- Group 1 contains only types where every packet has `ALPHA_TEST == 1`.

If any assertion fires, print `[GPU_CULL_MULTIDRAW v1] event=audit_fail
group=N typeID=T expected=X got=Y` and fall back to legacy loop for that
frame.

**(b) Pixel-level dual-emit parity (existing infra).** The static-prop
parity SSBO at `gos_static_prop_batcher.cpp:1483+` (Stage 2.D) already
captures per-instance, per-vertex parity bytes for CPU/GPU comparison.
Wire the new multi-draw path into the same parity hooks: the shader still
writes `parityOut[gl_InstanceID * vertsPerType + gl_VertexID]` (line 1571);
the only change is `gl_InstanceID` is now offset by the multi-draw's
`baseInstance`, so the receiving code reads from the correct slot via the
recorded type range. `RecordParityTypeRange` at `:1635` accepts the cursor
the cpu computed; for multi-draw, supply
`baseInstance_of_this_type * vertsPerType * sizeof(uint32_t)` instead of
the per-type SSBO bind cursor.

**(c) Per-pixel screen-comparison soak.** Run `tier1` with
`MC2_GPU_CULL_SUBSTRATE=1` first under legacy loop, then under multi-draw,
capture the same fixed frame from each, diff. Acceptance: zero non-zero
pixels (or a documented error budget tied to bindless-handle ordering
non-determinism, if any). This is the same shape as the
`renderwater-fastpath` parity gate — `2026-04-29-renderwater-fastpath-design.md`
section "Parity check" is the canonical recipe.

Layer (a) is mandatory for the first slice. Layer (b) is mandatory before
default-on. Layer (c) is the soak gate.

---

## 6. Perf gate

**Tracy zone:** `Render.GpuStaticProps` at mc2_01 normal zoom, both with
and without `MC2_GPU_CULL_SUBSTRATE=1`.

**Baseline measurements at design-write time:** NOT YET CAPTURED.
The design author did not run a Tracy capture. The baseline numbers cited
in `track_c_substrate_regression.md` (~120 µs off, ~2 ms on) are from a
prior session and must be re-measured at slice 0 of the implementation
plan, not earlier. Marking this as `M` (Measured-on-implementation, not
on-design). The implementation plan's first slice MUST capture both
numbers and paste into this doc before any code change.

**Gate target:** `Render.GpuStaticProps ≤ 200 µs` at mc2_01 normal zoom
with `MC2_GPU_CULL_SUBSTRATE=1` enabled. This sets a 10× margin over the
"all-off" baseline (~120 µs for the legacy CPU path) — multi-draw in
principle should match or beat the legacy path because the legacy path
also serializes per-bucket binds and uniform writes.

**Stretch target:** `Render.GpuStaticProps ≤ 100 µs`. Plausible if (1)
bindless textures eliminate the GL_TEXTURE_2D rebind cost, (2) the per-call
indirect-buffer rebind/unbind at lines 1702/1706 is hoisted out of the loop.

---

## 7. Risk inventory

Mirroring the R1–R8 style of
`docs/superpowers/brainstorms/2026-05-01-detail-overlay-consolidation-scope.md`.

### R1. AMD attribute-0 trap (HIGH)

`docs/amd-driver-rules.md` and the indirect-terrain banner at
`gameos_graphics.cpp:2210–2213` both call out: every multi-draw call
inside the same VAO must have `glEnableVertexAttribArray(0)` live. The
shared VAO already has it enabled at setup time
(`gos_static_prop_batcher.cpp:727`), so this should hold across the two
new multi-draw issues. **Verify in slice 0** with a `glGet` probe before
each multi-draw.

### R2. Per-bucket sampler set may diverge if state grouping is too aggressive (HIGH)

The current per-iter `glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_*, GL_REPEAT)`
at `:1700–1701` is set on the **bound texture**, not the sampler unit. If
multi-draw uses bindless textures, each texture's intrinsic
`glTextureParameteri` state is what wins — the legacy per-iter call
becomes a no-op because there's no single bound texture to mutate.
Solution: at registration time, for every TG_TypeShape's texture, ensure
its WRAP_S/WRAP_T are set to REPEAT once. Mirror the
`gameos_graphics.cpp:2210` "Per-texture wrap mode set at upload" precedent.
Filter (LINEAR) — confirm at registration too.

### R3. Compute-side ordering changes break the readback ring's actor-id mapping (HIGH)

`gpu_cull_readback.cpp:111` (`readback_getCurrentSlotOffset`) and the actor
visibility consumers at `gvactor.cpp` / `mech.cpp` / `gvehicl.cpp`
(see Track C3b commits in `git log`) read per-actor visibility out of an
SSBO whose layout is keyed by submission order via
`substrate_submitDynamicActor` (`gpu_cull_substrate.cpp:200`). If the
multi-draw permutation reorders type buckets, the **bucket** index shifts
but the **actor** index does not — actors are submitted by population, not
by type. So the readback ring is unaffected; verify by grep'ing every
consumer for `typeID` reads off the readback path. Slice 0 must do this
grep and document NOT_FOUND or list affected sites.

### R4. Mixed-alpha types (MEDIUM)

A type whose multiple packets have different `materialFlags` would land in
both groups if naively split. Need to either split the type into two
virtual buckets at registration or assert at registration that no type is
mixed (and fall back to legacy loop if any is found). Verify cardinality
at slice 0 by walking `s_packets` per type and counting alpha-class
distinctness.

### R5. `ARB_shader_draw_parameters` extension dependency (MEDIUM)

Section 3.4. Slice 0 must probe and abort cleanly to legacy loop if absent.
The 7900 XTX is fine; older test rigs may not be.

### R6. Bindless texture handle leak across mission unload (MEDIUM)

Bindless handles obtained via `glGetTextureHandleARB` must be
`glMakeTextureHandleNonResidentARB`'d on mission unload, or the handle
table grows unbounded across mission cycles. The handle table SSBO
itself must be deleted in `onMapUnload` paths. Pattern reference:
`onMapUnload` at `gos_static_prop_batcher.h:99`.

### R7. Patch shader writes to the indirect buffer in original typeID order (MEDIUM)

`gpu_cull_compute.cpp:798–805`: the patch shader binds the indirect cmd
buffer at SSBO binding `INDIRECT_CMD_BINDING` and writes
`cmds[t].instanceCount` for `t in [0, typeCount)`. If we permute the
indirect buffer by group, the patch shader must use the permutation map
to write `cmds[permutation[t]].instanceCount` instead. Either (a) supply
the permutation as a small uniform array, or (b) re-permute the patch
shader's input ordering at registration time so the natural `t` index in
the patch shader matches the new layout. (b) is simpler.

### R8. Stage 2.D parity SSBO cursor invariant (MEDIUM)

`gos_static_prop_batcher.cpp:1605` computes parity-byte cursor as
"cumulative usage so far this frame," sequentially across types in
`s_types` order. Multi-draw still issues per-type parity writes (the
shader hasn't changed), but the cursor now must follow the **multi-draw
order** (group 0 then group 1), not the registration order, or readback
slot decoding mismatches what the shader wrote. Cursor advances per
type are derived from `parityVerts * r.instanceCount`; permute the
sweep order to match the indirect-buffer layout.

### Top three (per request)
1. **R1** AMD attribute-0 — first failure to materialize on real hardware.
2. **R2** sampler state on bindless textures — silent visual corruption.
3. **R3** readback-ring actor-id mapping — load-bearing for Track C3b
   GPU-visibility consumers (`gvactor.cpp`, `mech.cpp`, `gvehicl.cpp`).

---

## 8. Verification appendix

Status legend (matches brainstorm convention):
- **D** = directly grep-verified at write-time
- **M** = measured at write-time (Tracy/run-time number)
- **NF** = NOT_FOUND despite grep — flagged
- **NM** = not measured (deferred to slice 0)

| Symbol / claim | Status | Citation |
|---|---|---|
| `glDrawElementsIndirect` per-bucket call site | **D** | `gos_static_prop_batcher.cpp:1704` |
| Bucket loop head | **D** | `gos_static_prop_batcher.cpp:1542` |
| Inner per-packet loop | **D** | `gos_static_prop_batcher.cpp:1652` |
| `s_indirectCmdBuf` declaration | **D** | `gpu_cull_compute.cpp:81` |
| `DrawCmd` struct + 20-byte assert | **D** | `gpu_cull_compute.cpp:507–514` |
| Indirect buffer allocation | **D** | `gpu_cull_compute.cpp:551–555` |
| Patch shader dispatch | **D** | `gpu_cull_compute.cpp:798–805` |
| Post-patch barrier `SHADER_STORAGE | COMMAND` | **D** | `gpu_cull_compute.cpp:813` |
| `compute_dispatch` ordering before `flush` | **D** | `txmmgr.cpp:1762–1766` |
| `glMultiDrawArraysIndirect` PR1 precedent | **D** | `gameos_graphics.cpp:2410` (memory file said `:2321` — superseded) |
| `GpuStaticPropType` struct fields | **D** | `gos_static_prop_batcher.h:87–92` |
| `GpuStaticPropPacket` struct fields | **D** | `gos_static_prop_batcher.h:38–48` |
| `STATIC_PROP_FLAG_ALPHA_TEST` value | **D** | `gos_static_prop_batcher.h:58` |
| `glUseProgram(s_staticPropProgram)` once | **D** | `gos_static_prop_batcher.cpp:1433` |
| `glBindVertexArray(s_sharedVao)` once | **D** | `gos_static_prop_batcher.cpp:1434` |
| Depth/blend/cull state set once | **D** | `gos_static_prop_batcher.cpp:1437–1442` |
| `glEnableVertexAttribArray(0)` at VAO setup | **D** | `gos_static_prop_batcher.cpp:727` |
| `batcher_getTypeDrawInfo` declaration | **D** | `gos_static_prop_batcher.h:269` |
| `batcher_getTypeDrawInfo` definition | **D** | `gos_static_prop_batcher.cpp:1954` |
| Per-type `count = sum(packet.indexCount)` | **D** | `gos_static_prop_batcher.cpp:1973–1980` |
| Per-iter sampler `glTexParameteri` | **D** | `gos_static_prop_batcher.cpp:1700–1701` |
| Per-packet texture bind | **D** | `gos_static_prop_batcher.cpp:1670–1671` |
| Per-packet `u_materialFlags` write | **D** | `gos_static_prop_batcher.cpp:1685–1686` |
| Per-packet `u_packetID` write | **D** | `gos_static_prop_batcher.cpp:1687–1688` |
| Per-type SSBO range bind (binding 0) | **D** | `gos_static_prop_batcher.cpp:1554–1556` |
| Per-type SSBO range bind (binding 1) | **D** | `gos_static_prop_batcher.cpp:1557–1559` |
| `GpuStaticPropRegistry::flush` callsite | **D** | `txmmgr.cpp:1753` |
| MC2_GPU_CULL_SUBSTRATE env flag | **D** | `gpu_cull_substrate.cpp:53` |
| Substrate enable accessor | **D** | `gpu_cull_substrate.cpp:57` |
| Tracy zone `Render.GpuStaticProps` ≤200 µs target | **NM** | baseline must be captured by implementation slice 0 |
| Mixed-alpha-class type cardinality | **NM** | walk `s_packets` per type at slice 0 |
| `ARB_shader_draw_parameters` runtime probe | **NM** | required at slice 0 |
| Per-type SSBO `r.instanceByteOffset == baseInstance * sizeof(instance)` | **NM** | must verify or re-pack at registration |
| Readback ring's `typeID` consumers (R3) | **NM** | grep `typeID` in `gvactor.cpp`, `mech.cpp`, `gvehicl.cpp` (Track C3b commits added these) |
| Memory file line `:2321` for PR1 multi-draw | **NF** (line shifted to **2410**) | symbol verified, line stale |
| `gpu_cull_compute.cpp` as the location of the per-bucket loop (memory file claim) | **NF** (loop is in `gos_static_prop_batcher.cpp:1542` — batcher, not compute) | corrected here |

---

## 9. Adversarial review readiness

Per worktree CLAUDE.md "Review Discipline" (load-bearing), this design
**must** receive an `adversarial-plan-review` pass before any
implementation slice opens. It qualifies as high-stakes by all of:

- Architectural endpoint touch (the GPU substrate draw path)
- Indirect-buffer schema change (registration-time permutation)
- Performance gate ≥30 % (target 95 %+ reduction in `Render.GpuStaticProps`
  with substrate on)
- Adds an extension dependency (`ARB_shader_draw_parameters`) and possibly
  a second extension (`ARB_bindless_texture`) that the codebase has not
  used before

The dispatch prompt for the review must contain verbatim:
"use the adversarial-plan-review skill in `.claude/skills/`."

The reviewer is expected to:
- Re-grep every cited symbol (regression catch: the memory file's
  `:2321` and `gpu_cull_compute.cpp` claims both turned out wrong here —
  prior memory decay).
- Verify the per-type SSBO offset invariant (Section 3) actually holds in
  current code, OR document the re-pack work needed.
- Verify the actor-id mapping risk R3 is grep-defended in both directions
  (find every reader of the readback ring; not just the obvious ones).
- Confirm or reject the "2 multi-draw groups" verdict by either finding a
  state dimension this design missed, or signing off the table in
  Section 2.1.

Until that review returns clean (CRITICAL=0), no source under
`GameOS/gameos/gos_static_prop_batcher.cpp` or `GameOS/gameos/gpu_cull_compute.cpp`
should be touched on the basis of this design.
