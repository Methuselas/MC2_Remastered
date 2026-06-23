# GPU-BUFFER-WRAPPER-TIER0-LIGHT-1 — recon + verdict (light SSBO ring conversion)

**Arc:** RENDER-BACKEND-SEAMS / VULKAN-CONTRACT-MANIFEST · recon + decision only, NO code · 2026-06-22
**Verdict:** **DEFER-PENDING-NVIDIA** (with a smaller PROCEED-able intermediate step — see §7).
The light SSBO is the bandwidth prize (1.85 MB/frame), but the code shows it is the *hard* case the
HUD pilot deliberately avoided: it **grows** (monotonic high-water per mission, ~1026+ records),
it is **read by every lit draw for the whole frame** (binding 20, incl. the GPU mech/static-prop
batchers in a different phase), and its per-frame orphan is a **hand-tuned NVIDIA no-stall fix that
is AMD-tested-only here**. A persistent-coherent ring is plausible but must reproduce all three and
be proven on NVIDIA before default-on. Do not rubber-stamp PROCEED.

## Inputs (read, not re-derived)
- `gpu-buffer-wrapper-tier0-recon-2.md` — HUD-first decision; light SSBO = the high-impact follow-up.
- `gpu-buffer-wrapper-tier0-recon-1.md` — live `[GPUBUF v1]` counter (`light: 1 call / 1,855,008 B`).
- `gpu-buffer-owner-recon-1.md` — buffer census (light SSBO row: "persistent (grows)").
- `gpu-buffer-wrapper-design-1.md` §4 row C (TIER0-POSTPROCESS bundles the light SSBO), §2 ring API,
  OD-2 (backing/wait policy), OD-4 (hitch-accounting parity).
- Shipped `GameOS/gameos/utils/gpu_ring_buffer.h` (`mc2gpu::GpuMeshRing`) — the pattern to extend.

> **Line drift note.** recon-2 cited an earlier HEAD (light block ~:8609–8693). At this recon's HEAD
> (`6978f419`) the light SSBO lives at `GameOS/gameos/gameos_graphics.cpp:8731–8853`. All file:line
> below are re-grepped against `6978f419`.

---

## 1. Lifecycle map (gos_LightDataSsbo_*, gameos_graphics.cpp @ 6978f419)

Single owner: `static GLuint s_lightDataSsbo` (`:8731`), size tracked by `s_lightDataSsboBytes`
(`:8732`). Raw GL (no `gos` STORAGE buffer type exists); all writes route through the
`MC2_GL_BufferData_Owner(..., LightSsbo)` hitch-accounting macro so they appear in `[GPUBUF v1]`.

| Phase | Site | Mechanism (quoted) |
|---|---|---|
| **Eager create + full upload** | ctor `mclib/txmmgr.cpp:484/493` | `lightData_ = new TG_HWLightsData[128]`; then `gos_LightDataSsbo_Upload(lightData_, 128 * sizeof(TG_HWLightsData))`. **Eager, not lazy** — comment `:485` "EAGER create+bind here … the GPU static-prop/mech batcher … reads LightsData via explicit layout(binding=20) and runs in a different phase than the txmmgr per-frame upload." Establishes binding 20 before any consumer. |
| **First create** | `:8739-8744` | `glGenBuffers` → `glBindBuffer(GL_SHADER_STORAGE_BUFFER,…)` → `MC2_GL_BufferData_Owner(…, bytes, data, GL_DYNAMIC_DRAW, LightSsbo)` → `s_lightDataSsboBytes = bytes` → `glBindBufferBase(…, LIGHT_DATA_SSBO_BINDING=20, …)`. |
| **Grow-realloc** | `:8754-8767` | `if ((GLsizeiptr)bytes > s_lightDataSsboBytes)` → `MC2_GL_BufferData_Owner(…, bytes, data, GL_DYNAMIC_DRAW, LightSsbo)` (re-specs to the new larger size, copying data). RF2 comment `:8755-8760`: buffer→binding-point (`glBindBufferBase`) is **context** state and must re-follow new storage; program block→binding (`glShaderStorageBlockBinding`) is **program** state, UNAFFECTED — explicitly NOT re-issued on grow. |
| **Per-frame orphan (below-cap)** | `:8768-8781` | **LIGHTSSBO-ORPHAN-1.** `MC2_GL_BufferData_Owner(…, s_lightDataSsboBytes, nullptr, GL_STREAM_DRAW, LightSsbo)` (orphan: discard old store) then `glBufferSubData(…, 0, bytes, data)` (full re-fill). This is the **1 call / 1.85 MB/frame** the counter measures. |
| **Per-frame orphan (split)** | `gos_LightDataSsbo_UploadSplit :8786-8827` | create/grow → falls back to `_Upload` (`:8795-8796`). Otherwise orphan (`:8811`, same `GL_STREAM_DRAW nullptr`) then prefix `glBufferSubData(…,0,prefixBytes,…)` (`:8813`) + suffix `glBufferSubData(…,prefixBytes,…)` (`:8816`). **NB:** orphan kills the old store, so the `prefixDirty` skip is DISABLED — prefix re-uploads unconditionally (comment `:8806-8810`). The split is a *which-bytes-dirty* optimization that the orphan partially defeats; it survives only as two SubData calls vs one. |
| **Bind (context)** | `:8744 / :8782 / :8819` | `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, s_lightDataSsbo)` after every upload. |
| **Bind (program block)** | `gos_BindLightDataStorageBlock :8845-8853` | per-draw, per lit material: `glGetProgramResourceIndex(shp, GL_SHADER_STORAGE_BLOCK, "LightsData")` → `glShaderStorageBlockBinding(shp, idx, 20)`. Unconditional/idempotent (comment `:8839`: immune to shader hot-reload relink which resets program block bindings). Callers: `txmmgr.cpp:2065` (ShapeRenderer::render) and `gameos_graphics.cpp:7202` (drawIndexedTris). |
| **Destroy** | `:8829-8836` | `glDeleteBuffers(1,&s_lightDataSsbo)`; called from `txmmgr.cpp:559` on teardown. No per-mission unload/recreate — buffer is session-lifetime, grows monotonically. |

**Upload extent driver** (`mclib/txmmgr.cpp:2311-2347`, `ZoneScopedN("RenderLists.LightDataUpload")`):
`lightUploadCount = max(lightDataStructuresCount, 64)`; `totalBytes = lightUploadCount * 1808`. Split
path used when `mc2StaticLightUploadSplitEnabled() && mc2LightBakeEnabled()`, else legacy whole-buffer.
The 64 floor (`kLightUploadFloor`) is preserved (comment `:2324`) so cull-stale offscreen actors with
transient over-count `lightDataIndex` still read valid backing.

---

## 2. THE KEY QUESTION — does it grow, and how often? → **YES, it grows. Unbounded by design.**

**Stride = `sizeof(TG_HWLightsData)` = 1808 B** (static_assert `mclib/tgl.h:340`; 16 lights/record:
`lightToWorld[16][16]` + `lightDir/Color/Falloff[16][4]` + `numLights_` + `pad[3]`). MAX_HW_LIGHTS_IN_WORLD=16
(`tgl.h:299`) is the *per-record* light cap, NOT a record-count cap.

**Record count is what grows, and it is the whole reason the UBO→SSBO conversion happened.** The header
comment (`gameos_graphics.cpp:8722-8725`) is explicit: the old std140 UBO was `ObjectLights light[64]`
(a 64-slot ceiling); it was converted to "an **unbounded** std430 SSBO … to remove the 64-slot ceiling
(mc2_17 was 57/64 … one dense mission from silent corruption)." **There is no MAX light-record count.**

**CPU backing grows in +128-record steps** (`txmmgr.cpp:1692-1699`):
```
if (lightDataStructuresCount + 1 >= lightDataStructuresCapacity) {
    new_lights_data = new TG_HWLightsData[lightDataStructuresCapacity + 128];
    memcpy(...); delete[] lightData_; lightData_ = new_lights_data;
    lightDataStructuresCapacity += 128;
}
```
Initial capacity 128 (`:482`). `addLightDataStructure` (FNV+memcmp dedup, `:1666`) appends a new record
on a dedup miss; the dedup map and count rebase **per frame** at `resetLightData()` to `S`
(static high-water) or 0 (`:1815`), so per-frame count varies but the **GPU high-water is monotonic
within a mission**.

**Quantify (from the counter).** `1,855,008 B ÷ 1808 = exactly 1026 records` at capture (mc2_01 steady
state). So the live per-frame upload was **1026 records ≈ 16× the old 64 ceiling**, and the CPU capacity
was ≥1152 (9 grow steps from 128). The GPU SSBO re-specs to a new larger size each time per-frame
`totalBytes` exceeds the prior high-water (`:8754`). Cadence:
- **Per-frame respec (orphan, full 1.85 MB):** EVERY frame (the headline cost). Not a grow — same size.
- **GPU grow-realloc (`:8761`):** when per-frame count climbs past the previous high-water — i.e.
  early-mission warm-up and on light-population spikes (mech spawns, building destruction adding lights).
  Monotonic, so it **settles** mid-mission; it is NOT every-frame, but it is NOT one-time either, and
  there is no upper bound to size the ring to once.

**Verdict on grow:** This is **NOT** the HUD "size-to-max-no-grow" case. HUD has a compile-time fixed
capacity with overflow rejection; the light SSBO is explicitly *unbounded* and grows in data-driven
steps. A ring for it **must implement `ensureCapacity`** (drain all N fences → delete+recreate immutable
storage → re-`glMapBufferRange` → re-`glBindBufferBase`), the exact drain-remap path the shipped
`GpuMeshRing` intentionally omits (header comment lines 9-15). This is the single hardest part of
`GpuRingBuffer<N>` and it is unavoidable here.

---

## 3. READ-AFTER-WRITE hazard → **write-once-read-many, whole-frame, cross-phase. Ring slot must stay live all frame.**

**Pattern confirmed: written ONCE per frame, read by EVERY lit draw + the GPU batchers.**
- **Write:** one site, `RenderLists.LightDataUpload` (`txmmgr.cpp:2343/2345`), once per frame.
- **Read (whole frame, binding 20):** every lit shader. `gos_BindLightDataStorageBlock` is called
  per-draw from `txmmgr.cpp:2065` (legacy ShapeRenderer) and `gameos_graphics.cpp:7202` (drawIndexedTris);
  the program block→slot-20 wiring means **all lit materials read slot 20 for the entire frame**.
- **Cross-phase readers (the sharp edge):** the eager-create comment (`txmmgr.cpp:485-492`) states the
  **GPU static-prop and mech batchers read LightsData via explicit `layout(binding=20)` in a DIFFERENT
  phase** than the txmmgr upload. The owner-recon confirms mech.frag and static-prop coalesce consume
  binding 20 ("consumed not owned"). So slot 20 must be the live, fully-written buffer across mech +
  static-prop + terrain + legacy passes — the *whole* frame, not a single draw.

**Is a 3-frame ring safe?** Yes in principle, and *stricter* than HUD:
- HUD: orphan→bind-by-target→draw→unbind, self-contained in one call; the fence trivially covers it.
- Light: the slot written in frame N is `glBindBufferBase`'d at slot 20 and must remain bound and
  unmodified through ALL of frame N's lit draws (incl. the later-phase batchers), then is reused at
  frame N+3 only after its fence (set after the last consumer of frame N) signals. This works **only if
  the `endFrameFence()` is placed after the LAST lit consumer of the frame, not after the upload**.
  Mis-placing the fence (e.g. right after upload, mirroring HUD's per-draw model) would fence before the
  batchers read → unsafe reuse. The fence point is non-obvious and is itself a slice risk.
- No compute pass *writes* the light SSBO (cull/patch compute do not touch binding 20 per the census);
  it is read-only on the GPU. So the only hazard is CPU-overwrites-while-GPU-reads, which the ring's
  per-slot fence is designed for — provided the fence wraps the whole frame's reads.

---

## 4. LIGHTSSBO-ORPHAN-1 — what it is, and whether a ring preserves it

**What it is** (`gameos_graphics.cpp:8769-8780`, verbatim intent): `glBufferSubData` onto a buffer the
GPU is still reading from the *prior frame's* draws forces the **NVIDIA** driver to **block the CPU
until the GPU finishes — observed as ~80 ms in the `RenderLists.LightDataUpload` Tracy zone on a
1050 Ti**. AMD tolerates it silently. The fix: `glBufferData(…, nullptr, GL_STREAM_DRAW)` orphans
(discards) the old store immediately; the driver retires it async once the GPU is done and hands the CPU
a fresh store with **no sync stall**. `GL_STREAM_DRAW` (not `GL_DYNAMIC_DRAW`) is deliberate so NVIDIA
does not place it in VRAM and route the write through PCI-E with sync.

**Does a persistent-coherent ring preserve it?** *In principle, yes — that is exactly what a ring does:*
N distinct slots + a fence mean the CPU writes slot N+3 only after the GPU finished reading it, so there
is no write-into-in-flight-store and hence **no implicit driver sync to stall on** — the same hazard the
orphan dodges, solved structurally instead of by discard. The mech/static-prop rings already run this
way on both vendors. So a correctly-fenced ring should *subsume* LIGHTSSBO-ORPHAN-1 and eliminate the
per-frame 1.85 MB orphan entirely (the counter win).

**BUT — three regression risks specific to this conversion:**
1. **It is AMD-tested-only here.** The ~80 ms stall and its fix were characterized on a 1050 Ti; this
   repo's smoke/visual gates run on AMD (7900 XTX). A ring that is fence-correct on AMD could still
   reintroduce a stall on NVIDIA if a slot's fence is mis-placed (see §3) or if `glClientWaitSync`
   blocks because the ring is too shallow for the light buffer's read duration. **The orphan's entire
   reason to exist is an NVIDIA behavior we cannot observe in CI.**
2. **Coherent-map + grow interaction.** On `ensureCapacity` (which WILL fire, §2) the ring must
   `glClientWaitSync` ALL slots, unmap, delete, recreate immutable storage, remap, re-bind base 20. A
   grow mid-frame-sequence is a forced full stall — acceptable if rare (monotonic, settles), but the
   grow path's NVIDIA stall behavior is *also* unobserved here.
3. **`GL_STREAM_DRAW` semantics are lost.** `glBufferStorage(PERSISTENT|COHERENT)` is a different memory
   class than `glBufferData(STREAM_DRAW)`. The persistent map should be the *better* class for
   write-once-per-frame, but only NVIDIA testing confirms it doesn't regress the very stall the current
   code was tuned around.

**Flag (load-bearing):** LIGHT-1 acceptance MUST include explicit NVIDIA verification that the ring does
not reintroduce the `RenderLists.LightDataUpload` stall. AMD-green is necessary but NOT sufficient.

---

## 5. Regression gates for the eventual slice

1. tier1 5/5 (`mc2_01 mc2_03 mc2_10 mc2_17 mc2_24`), 30 s each, exit 0, gate OFF **and** gate ON.
2. **Byte-identical lighting** vs gate-OFF baseline on the lighting-stress missions — **mc2_24** (most
   props/buildings → most baked static light records, highest count/high-water) and **mc2_10**
   (building destruction → dynamic light-population churn → exercises the grow path). Capture via the
   visual golden harness; add a dedicated baseline key — do **NOT** edit the foreign-WIP
   `tests/visual/golden-sets.json`.
3. `[GPUBUF v1]` (`MC2_GPUBUF_COUNTER=1`, gate ON): the `light` owner orphan **bytes → ~0** per frame
   (the ring memcpys into the coherent map; no per-frame `glBufferData`). This is the 1.85 MB/frame prize.
   `hud`, `sp_shadow`, `gos_UpdateBuffer` lines unchanged.
4. **NVIDIA no-stall verification (mandatory, see §4):** no `RenderLists.LightDataUpload` Tracy
   regression on NVIDIA — the ring must match or beat the orphan's ~free upload. AMD-only is insufficient.
5. Grow A/B: force a high light count (e.g. mc2_24) and confirm `ensureCapacity` drains fences, remaps,
   re-binds base 20 with no corruption and no missing-fence assert. Split-upload parity (or a justified
   decision to drop split under a full-frame coherent ring — likely correct, since the ring memcpy is
   already cheap and the orphan already defeats `prefixDirty`).
6. Fence-per-frame: exactly one `endFrameFence()` per `beginFrame()` (the `GpuMeshRing` debug assert +
   `begin_without_end` log), with the fence placed **after the last lit consumer of the frame** (§3),
   not after upload. No new GL errors under `MC2_GL_DEBUG_FATAL=1`; binding-20 invariant (program block
   binding unaffected by realloc) preserved.
7. `MC2_GPUBUF_RESIDENCY=1` lists the light ring (tag, kind, N×perSlotBytesAligned, live fences).

---

## 6. Proposed slice shape

**Reuse** the shipped `mc2gpu` ring pattern, but a `GpuMeshRing` is VBO/IBO + draw-by-`baseVertex`;
the light buffer is an SSBO bound by base slot 20 with a grow path. So LIGHT-1 needs a **sibling
ring type** (`GpuStorageRing` / extend `GpuRingBuffer<N>` per design §2.2) that adds:
- `bindBase(slot)` (→ `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, buf)` at the slot's offset, or
  `glBindBufferRange` at `slot*perSlotCapAligned*stride` with `gpuSsboOffsetAlignment` rounding — design
  §2.1 step 2; NVIDIA rejects misaligned range binds);
- `ensureCapacity(elems)` (the drain-remap path `GpuMeshRing` omits — design §2.2);
- whole-frame fence discipline: `endFrameFence()` after the LAST lit draw, NOT after the memcpy.

**Backing/wait (design OD-2):** PersistentCoherent backing + `BlockIgnoreTimeout` wait (mech/static-prop
policy), 3 frames (`kHudRingFrames`/`MECH_RING_FRAMES` parity).

**Gate:** **extend `MC2_GPUBUF_RING`** (do not add a new gate). It already exists
(`gameos_graphics.cpp:620`, `RendererFeatureRegistry.h:1066`) and gates the HUD ring; one gate keeps the
GpuBuffer-adoption family A/B-comparable and reversible as a unit. If finer control is wanted later, a
`MC2_GPUBUF_RING_LIGHT` sub-gate can be added, but start unified.

**Files (expected):** new `GameOS/gameos/utils/gpu_storage_ring.h` (or extend `gpu_ring_buffer.h`);
`GameOS/gameos/gameos_graphics.cpp` (the `gos_LightDataSsbo_*` block `:8731-8853` + the fence-after-last-
consumer hook). **Do NOT touch** `mclib/mech3d.cpp`, `mclib/assimp_importer.cpp`,
`tests/visual/golden-sets.json` (foreign WIP at this HEAD). Note `mclib/txmmgr.cpp` is the *caller* and
also currently foreign-WIP-adjacent in other lanes — keep the change inside `gameos_graphics.cpp` behind
the existing `gos_LightDataSsbo_Upload` API so the caller is untouched. Run `slice-preflight` first.

---

## 7. Recommendation: **DEFER-PENDING-NVIDIA**, with a PROCEED-able intermediate

The code shows BOTH risk multipliers the recon-2 prologue warned about are real and present:
- the **grow path is live, unbounded, and not one-time** (§2: ~1026 records, +128 steps, monotonic
  high-water — the `ensureCapacity` drain-remap is unavoidable), AND
- the **NVIDIA no-stall opt is fragile and unobservable in CI** (§4: ~80 ms 1050 Ti stall, AMD-tested-
  only here; whole-frame cross-phase reads make the fence point non-obvious — §3).

Per the dispatch instruction ("if the grow path is hot/unbounded AND the NVIDIA opt is fragile,
recommend DEFER or a smaller intermediate — do not rubber-stamp PROCEED"), this is the DEFER case.
**Do not ship a full coherent-ring LIGHT-1 default-on without NVIDIA hardware in the loop.**

**Two ways forward, in preference order:**

**(A) Smaller intermediate — `LIGHT-GROW-ONCE-SUBDATA-1` (PROCEED-able now, AMD-only-safe).**
Keep the single buffer (no ring), but stop the per-frame full orphan: grow capacity to high-water (as
today), then per-frame do `glBufferSubData(0, usedBytes, data)` onto a buffer whose write is fenced
against the prior frame's reads via a **single** `GLsync` (the minimal fence the orphan was emulating).
This captures most of the 1.85 MB/frame win (no orphan re-spec) with a far smaller blast radius and no
ring/grow-remap, and it is the structural fix the orphan was approximating. It still wants NVIDIA
confirmation that the fence replaces the orphan's stall-avoidance, but the failure mode is a *single*
fence wait, not a 3-slot remap — much easier to reason about and revert. recon-1 itself floated this:
"a persistent-mapped/ring (or **simply grow-once + glBufferSubData**) buffer eliminates ~1.85 MB/frame."

**(B) Full LIGHT-1 (the design §4 row C ring) — gate it, and BLOCK default-on on NVIDIA verification.**
Build the `GpuStorageRing` with `ensureCapacity`, behind `MC2_GPUBUF_RING`, default-OFF, and ship it
OFF. Default-on is contingent on: an NVIDIA pass of gate #4 (no `LightDataUpload` stall) + gate #5
(grow A/B). Until someone runs the NVIDIA leg, it stays a gated, soaking, opt-in path — never the default.

**Net:** if the goal is the bandwidth win soon and safely on AMD, do (A) first. If the goal is the
Vulkan-readiness ring shape, build (B) gated/OFF and hold default-on for NVIDIA. Either way, **LIGHT-1
as a default-on coherent ring is premature until NVIDIA is in the verification loop** — that is the
DEFER. HUD-1 proved the ring shape; the light SSBO is the case that proves the ring's *grow* + *whole-
frame-fence* + *driver-stall* edges, none of which HUD exercised and one of which (NVIDIA) CI cannot see.

## Cross-references
- `gpu-buffer-wrapper-tier0-recon-2.md` (HUD-first; LIGHT-1 sequenced second).
- `gpu-buffer-wrapper-design-1.md` §2 (ring API + `ensureCapacity`), §4 row C, OD-2/OD-4.
- `gpu-buffer-owner-recon-1.md` (light SSBO row "persistent (grows)"; binding 20 consumed-not-owned).
- `GameOS/gameos/utils/gpu_ring_buffer.h` (`GpuMeshRing` — the pattern; note its deliberate NO-grow scope).
- `binding-slot-occupancy.{md,json}` + `scripts/check-binding-slots.py` (slot 20 = LIGHT_DATA, C++↔GLSL lockstep pair).
