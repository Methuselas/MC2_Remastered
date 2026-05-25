# Substrate Multi-Draw Coalesce — Implementation Plan v3.8 (against spec v2r20)

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Spec:** `docs/superpowers/specs/2026-05-08-substrate-coalesce-design-v2.md` (**v2r20** — implementation-ready). Spec is the single source of truth for *what* and *why*; this plan covers *order* and *mechanics*. **This is plan v3.8** — supersedes v1..v3.7. v3.8 is a documentation-residue cleanup pass after the v3.7 fresh-session adversarial review (0 CRITICAL, 2 MAJOR, 3 MINOR). v3.8 changes (delta from v3.7): (1) Step group 11 invariants block (§3 ASCII diagram spec ref + "Save/restore for slots 4 + 15…" line) re-stated to slot 4 + 2D_ARRAY only; slot 15 callout points to Step 10.3. (2) Verification appendix table row "Save SSBO 0–3 …" right-column re-stated similarly. (3) Architecture summary's slot-15 paragraph adds an explicit spec-vs-plan literal-text note acknowledging that spec §3.X / §9 wording ("in `flush()`") is cosmetically stale post v3.6 cleanup — the contract is honored at the mutation site in `compute_dispatch()`. (4) Step 10.3 cull→patch barrier citation corrected from `:843` to `:820` (the post-patch `:843` barrier is unaffected and noted separately). No code-design changes. v3.7 history below.

**v3.7 history:** v3.7 is a slot-15-envelope-relocation revision after the fresh-session adversarial review of v3.6 returned 1 CRITICAL: v3.6's removal of the slot-15 save/restore in the draw branch was correct (no draw shader reads slot 15), but its rationale that "the existing 11.7.a/j save/restore covers downstream callers" was internally false — that envelope only ever covered slot 4 + 2D_ARRAY. The slot-15 binding-hygiene contract from spec §3.X / §9 is now honored at the slot-15 mutation site itself in `compute_dispatch()` (Step 10.3), where the rebind has always lived. Spec is unchanged. v3.7 changes (delta from v3.6):
- Step 10.3 gains a `glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 15, &prev)` save before the bind and a `glBindBufferBase(... 15, prev)` restore after the patch dispatch. The shader needs slot 15 only during the dispatch; restore-immediately-after preserves spec §3.X / §9 binding hygiene without changing the per-frame barrier logic added in v3.6.
- Front-matter changelog corrected: removed the false "11.7.a/j save/restore covers slot 15" claim. Architecture summary updated to point at the Step 10.3 envelope.
- Step 11.7.a/f/j wording corrected: the draw branch envelope covers slot 4 + 2D_ARRAY only; slot 15 is saved/restored at its mutation site in `compute_dispatch()` (Step 10.3). No code change in 11.7 — only the comments/rationale that explain why slot 15 is absent from this envelope.
- Step 5.10.c rollback body factored into a `static void coalesceRollbackTexBuild(std::vector<DWORD>& tempPins)` helper (file-scope, near the other coalesce helpers from Step 2.4 / 2.6). Step 5.10.c's normal failure path calls it; Step 12B.5's forced-size-mismatch hook calls it. Removes the v3.6 ambiguity ("`goto coalesceSizeMismatchRollback;` // or refactor as the existing failure-path call") — there is now exactly one rollback site.
- Step 15.4(c) verification re-pointed to Step 12.5's `finalizeGeometry`-time `event=permutation_state` log line. Under forced-disarm, coalesce never arms, so "first armed flush" never fires; Step 12.5 (which IS reachable under all four force-disarm modes) is the correct site.
- Iteration history (preserved for archeology):
  - v3.6 changes (delta from v3.5):
    - Step 11.1 anchor corrected ("after `uploadAllBucketsIfNeeded()` returns at `:1325`" — the `:1260–1263` legacy fence-wait is internal to `uploadAllBucketsIfNeeded`, not a `flush()` site).
    - New Step group 12B adds `MC2_COALESCE_FORCE_DISARM` test hooks so Step 15.4 forced-disarm regression tests are executable instead of aspirational.
    - Step 10.3 gains an explicit "no per-frame barrier needed for permutation SSBO" note.
    - Step 5.4 / Step 5.6 factored as a real `allocPermutationSsboAsIdentity(typeCount)` static helper; usage hint flipped to `GL_STATIC_DRAW` (matches spec §9 lifecycle table).
    - Step 11.7.f's defensive slot-15 rebind in the draw branch removed (rationale corrected in v3.7: no draw shader consumes slot 15; envelope migrated to Step 10.3).
    - Indirect-command stride exported as a shared constant `kDrawElementsIndirectCommandSize` from `gpu_cull_compute.h`; literal `20u` at Step 11.7.h replaced.
    - Front-matter "Runtime disarm fallback limitation" rewritten — corruption is ALL FIVE indirect-command fields (count, instanceCount, firstIndex, baseVertex, baseInstance), not just stale baseInstance.
    - `MC2_SUBSTRATE_COALESCE_VALIDATE` references demoted to "future-slice" (no validate code added in this slice; gates that referenced it are reframed as inspection-only or removed).
  - v1 had blackout / collapsed-props / binding-13 collision bugs → spec v2r15. v2 had slot-1-not-inherited / cap-mismatch / loadProgramsIfNeeded-latch bugs → spec v2r16/v2r17. v3 had unguarded edge cases → plan v3.1 patches. v3.1 patch #6 was based on a misread of mode 4 (reads `v_argb`, not `colors_.c[...]`); reverted in v2r18 + v3.2. v3.2 review caught: (a) Step 9.2 SORTED branch failed to populate `bucketCaps[]`/`bucketBases[]` — would have made every coalesce draw invisible; (b) `batcher_getTypeDrawInfo` returns true for zero-packet types — `hasGeometry` ternary was misframed; (c) `glMultiDrawElementsIndirect` with `{count=0, instanceCount>0}` is driver-defined. v3.3 fixed three issues. v3.3 review caught the `cumBase` shadow + mid-mission disarm baseInstance corruption + unspecified `batcher_getInstanceCap`. v3.4 fixed all four — but introduced the mid-frame-rebuild blackout. v3.5 reverted to flag-only disarm (accepted limitation). v3.6 is documentation + test-hook cleanup; no design changes. v3.7 relocates the slot-15 save/restore to its mutation site (Step 10.3); no design changes.

**Goal:** Replace `flush()`'s per-type/per-packet `glDrawElementsIndirect` loop with two `glMultiDrawElementsIndirect` calls (one per alpha-test group). Cuts `Render.GpuStaticProps` from ~2 ms to ≤200 µs at mc2_01 normal zoom; unblocks substrate default-on flip (separate slice).

**Architecture (v2r18-current):**
- **Legacy finalize is authoritative** (§5.0). `gos_static_prop_batcher.cpp:707–795` runs UNCONDITIONALLY. Coalesce build is a side-attempt; failures disarm coalesce only.
- **Probe + env-decision happen INSIDE `loadProgramsIfNeeded()`** (§6 / out4-CRIT-3). The one-shot `s_programLoadTried` latch at `:201` makes pre-latch ordering essential — `s_hasShaderDrawParams` and `s_coalesceEnvDisabled` must be set BEFORE either `makeProgram` call.
- Two-program shader split (`s_staticPropProgram` legacy + `s_staticPropProgramCoalesce`) gated by extension probe.
- `s_coalesceInstanceSsbo` (capacity-based per-type slots, ring-buffered, persistent-coherent-mapped); `s_perDrawSsbo` (one entry per type, sorted, **binding 4**); per-group `GL_TEXTURE_2D_ARRAY` (BGRA, same-size assertion); `s_permutationSsbo` (**binding 15** — verified free after two flips: 13 collided with `BlockVis`, 14 collided with diagnostic readback at `gpu_cull_compute.cpp:855`).
- Three-flag state machine: `s_coalesceLayoutReady` / `s_coalesceEnabled` / `s_coalesceArmed`. Strengthened `IsCoalesceEnabled()` per §7.
- **Two cap sources** (§5.6a): natural/legacy branch uses `batcher_getTypeDrawInfo`'s legacy `s_instanceCapacity` stride; coalesce/sorted branch uses `batcher_getInstanceCap(typeID)` per-type. Names `legacyVisibleIdsCap` / `coalesceInstanceCap` enforced in code.
- **Slot 1 colors NOT bound by coalesce branch** (v2r18 §3.X.1 — verified by grep that `colors_.c[...]` is unread in any live shader path; all debug modes 0–7 read `v_argb`). Legacy code at `:1557–1559` still binds it per-type but no shader consumes the data — dead state on legacy too. Future-shader caveat: any slice that reintroduces `colors_.c[...]` reads owns either the absolute-offset conversion or forcing that path back to legacy.
- **Per-program `ProgramLocs` uniform location caches** (§6.X). Five SHARED uniforms (`terrainMVP`, `u_terrainViewport`, `u_mvp`, `u_fogValue`, `u_debugAddrMode`) cached for both programs; coalesce branch in `flush()` re-uploads them after `glUseProgram(s_staticPropProgramCoalesce)`.
- Legacy `flush()` prologue at `:1430–1474` (slot 2 per-type bind only — slot 1 is per-type in the loop) runs unconditionally — coalesce branch inherits slot 2.
- SSBO save/restore for slot 4 + unit-0 `GL_TEXTURE_BINDING_2D_ARRAY` wraps the coalesce draw branch in `flush()`. **Slot 15 binding hygiene is honored at its mutation site** in `compute_dispatch()` (Step 10.3 envelope: `glGetIntegeri_v` save → bind → patch dispatch → `glBindBufferBase` restore). The draw branch never binds slot 15. v3.7 corrects v3.6's mistaken claim that "11.7.a/j covers slot 15"; spec §3.X / §9 binding-hygiene contract is preserved by the Step 10.3 envelope. **Spec-vs-plan literal-text note (v3.7):** spec §3.X (lines 575–587) and §9 (lines 2359–2363) describe the slot-15 save/restore as living "in `flush()`" because at spec-write time the draw branch was the only site that bound slot 15. v3.6's cleanup correctly removed that bind from `flush()` (no draw shader reads slot 15 — only the patch compute shader does), and v3.7 relocates the save/restore to the actual mutation site in `compute_dispatch()`. The contract (every site that mutates slot 15 saves and restores) is honored; the spec's literal locator ("in `flush()`") is now cosmetically stale and is preserved in the spec as a historical record. Do NOT re-introduce a no-op slot-15 bind in `flush()` for spec-fidelity reasons — that re-introduces dead state without adding any safety, and was the v3.6 cleanup the user signed off on.

**Tech Stack:** GL 4.3 + `GL_ARB_shader_draw_parameters` (first `#extension` precedent), persistent-coherent-mapped SSBOs, compute shader (`gpu_cull_patch.comp`), `glMultiDrawElementsIndirect`, `glTexImage3D` 2D arrays.

**Single-PR ship rule (spec §13):** all step groups land in a single commit.

**"First K in submission order" approximation (spec §5.5, documented behavior):** the coalesce path intentionally preserves the existing substrate approximation. CPU fills `s_coalesceInstanceSsbo` with ALL submitted instances (Step 11.5's `memcpy` copies `bucket.instances.size()` entries). The patch shader writes `instanceCount = bucketCountData[b]` = K visible instances per cull. The multi-draw issues K instances starting at `gl_BaseInstanceARB`, reading the FIRST K CPU-submitted instances in that type's slice — NOT the K instances GPU cull selected as visible. This matches today's C1b substrate path (spec §2.1 "first-wins single-draw"). Visual canary against legacy substrate at Step 17 validates equivalence. **Do not flag this as a CRIT in future reviews — it is documented spec behavior.**

**Runtime disarm fallback limitation (v3.5 design, scope clarified v3.6):** runtime disarm via Step 11.3 (`type_overflow`) or Step 11.4 (`tex_evicted`) AFTER `compute_buildIndirectBuffer` ran with sorted layout leaves the indirect buffer populated in sorted layout. Subsequent legacy `glDrawElementsIndirect` calls (`gos_static_prop_batcher.cpp:1703`) at `cmdOffset = typeID * stride` read the entire indirect command struct (`count`, `instanceCount`, `firstIndex`, `baseVertex`, `baseInstance`) against natural-typeID semantics — but those slots hold the SORTED-position type's values. **All five fields can be wrong** until next mission load. Visible impact is likely "props show wrong geometry / wrong textures / vanish or render at wrong scale," not a subtle offset drift; v3.5 wording ("stale baseInstance") understated the scope. The accept-and-document conclusion is unchanged: disarms are expected to be rare (the §5.1 cap formula is generous (2× average), and texture eviction is mitigated by the `pinNode()` refcount-aware pin (§CRITICAL-B)); both events log `[COALESCE v1] event=disarmed`; the operator sees the symptom in the smoke artifacts and can re-load the mission to recover. **Do NOT attempt mid-frame indirect-buffer rebuild** — `compute_dispatch()` runs before `flush()` and has already patched `instanceCount` for THIS frame's draws; `glBufferData` orphans the GL handle and zeroes the freshly-uploaded `cmds[]`, blacking out the disarm frame entirely. v3.4 attempted this rebuild and reverted in v3.5 after both opus and sonnet adversarial reviews independently traced the blackout chain. **Do not flag this as a CRIT in future reviews — it is the explicit minimum-blast-radius design.**

**Pre-existing limitation — `bucketCaps[]` staleness on legacy ring grow (deferred to a separate slice, v3.6 documentation):** `compute_buildIndirectBuffer()` populates `bucketCaps[t] = s_instanceCapacity` once at mission load; the legacy ring grow at `gos_static_prop_batcher.cpp:240–272` can double `s_instanceCapacity` mid-mission, leaving `bucketCaps[]` stale. Cull shader's `if (slot >= cap)` overflow gate then fires earlier than the actual buffer capacity allows — silent under-cull. This is INHERITED from the existing substrate path (not introduced by v2 coalesce; both natural and sorted branches share the same mission-static `bucketCaps[]`). **Fix is out of scope for this slice** — deferred to a separate "bucketCaps refresh on legacy ring grow" slice. Do NOT add a `compute_buildIndirectBuffer()` re-call hook into the grow path here; ad-hoc rebuild paths into `s_indirectCmdBuf` are easy to get wrong (v3.4 → v3.5 mid-frame rebuild blackout was that exact lesson).

**Build / deploy / smoke:**
- Build: `cmake --build build64 --config RelWithDebInfo --clean-first`.
- Deploy: `cp -f` of `build64/RelWithDebInfo/mc2.exe` and every modified shader to BOTH `mc2-win64-v0.2/` and `mc2-win64-v0.3/`, with `diff -q` after each. Never `cp -r`.
- Smoke: `py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing`.

---

## Step group 1 — Header + struct field additions

**File:** `GameOS/gameos/gos_static_prop_batcher.h`

Spec refs: §5.1, §5.1b, §CRITICAL-B, §5.2, §5.6b.

- [ ] **1.1** Extend `struct GpuStaticPropType` at `:87–92` with four fields: `uint32_t instanceCap;`, `uint32_t coalesceByteOffsetWithinGroup;`, `uint32_t lastSeenGosHandle;`, `uint8_t alphaClass;`. Document above the struct that these are populated by `finalizeGeometry()` and undefined before then.
- [ ] **1.2** Append §5.6b exports after `:269`: `batcher_getSortedTypeOrder`, `batcher_getAlphaOffCount`, `batcher_getAlphaOnCount`, `batcher_getInstanceCap`, `batcher_getCoalesceInstanceSsbo`, `batcher_getPerDrawSsbo`, `batcher_getTexArrayOff`, `batcher_getTexArrayOn`, `batcher_getPermutationSsbo`, `batcher_getCoalescePerFrameInstanceBytes`, `batcher_isCoalesceLayoutReady`, `batcher_isCoalesceArmed`. Signatures verbatim from spec §5.6b.
- [ ] **1.2a** Define the bodies for the new accessors in `gos_static_prop_batcher.cpp` (file-scope, alongside the existing `batcher_getTypeCount` / `batcher_getTypeDrawInfo` definitions near `:1955`). Pre-finalize calls return safe sentinels (0 / nullptr / empty) so no caller deref-faults if invoked before `s_geometryFinalized`:
  ```
  uint32_t batcher_getInstanceCap(uint32_t typeID) {
      if (!s_geometryFinalized || typeID >= s_types.size()) return 0;
      return s_types[typeID].instanceCap;
  }
  const uint32_t* batcher_getSortedTypeOrder() {
      return s_geometryFinalized && !s_sortedTypeOrder.empty()
             ? s_sortedTypeOrder.data() : nullptr;
  }
  uint32_t batcher_getAlphaOffCount() { return s_alphaOffCount; }
  uint32_t batcher_getAlphaOnCount()  { return s_alphaOnCount;  }
  GLuint   batcher_getCoalesceInstanceSsbo() { return s_coalesceInstanceSsbo; }
  GLuint   batcher_getPerDrawSsbo()          { return s_perDrawSsbo; }
  GLuint   batcher_getTexArrayOff()          { return s_texArrayOff; }
  GLuint   batcher_getTexArrayOn()           { return s_texArrayOn; }
  GLuint   batcher_getPermutationSsbo()      { return s_permutationSsbo; }
  size_t   batcher_getCoalescePerFrameInstanceBytes() {
      return s_coalescePerFrameInstanceBytes;  // populated in Step 5.7
  }
  bool batcher_isCoalesceLayoutReady() { return s_coalesceLayoutReady; }
  bool batcher_isCoalesceArmed()       { return s_coalesceArmed;       }
  ```
  Note signature: `batcher_getCoalescePerFrameInstanceBytes` returns `size_t` (not `uint32_t`) so it can carry sub-4GB ring totals without truncation. The header declaration in Step 1.2 must match.
- [ ] **1.3** Build header consumers: `cmake --build build64 --config RelWithDebInfo --target gameos`.
- [ ] **1.4** **Export `kDrawElementsIndirectCommandSize` from `GameOS/gameos/gpu_cull_compute.h`** (v3.6 — replaces the literal `20u` previously hard-coded at Step 11.7.h). Place inside the `gpu_cull` namespace alongside the existing public exports:
  ```cpp
  namespace gpu_cull {
  // Stride of one DrawElementsIndirectCommand (5 GLuint/GLint fields, 20 bytes).
  // The matching std430 GLSL struct is in shaders/gpu_cull_patch.comp:24–30.
  // The C++ struct is defined inside compute_buildIndirectBuffer at
  // gpu_cull_compute.cpp:534–540 with a `static_assert(sizeof(DrawCmd) == 20)`
  // at :541. This constant is the link site for batcher.cpp's multi-draw
  // offsets and any future slice that needs the stride at a TU boundary.
  static constexpr GLsizei kDrawElementsIndirectCommandSize = 20;
  } // namespace gpu_cull
  ```
  Step 11.7.h (and any future cmd-stride consumer) `#include`s this header and uses the constant instead of repeating `20`.

---

## Step group 2 — File-scope state additions + header struct support

**Files:** `GameOS/gameos/gos_static_prop_batcher.cpp` (Steps 2.1–2.4) **and** `GameOS/gameos/gos_static_prop_batcher.h` (Step 2.5 — `PerDrawEntry` struct + asserts placed alongside `GpuStaticPropInstance`).

Spec refs: §9 inventory.

- [ ] **2.1** Near `:62–172`, add file-scope statics:
  - GL handles: `s_coalesceInstanceSsbo`, `s_texArrayOff`, `s_texArrayOn`, `s_perDrawSsbo`, `s_permutationSsbo`, `s_staticPropProgramCoalesce`.
  - Map pointer: `void* s_coalesceInstanceMap = nullptr;`.
  - Fence array: `GLsync s_coalesceFence[RING_FRAMES] = {};` (separate from legacy `s_fence[]`).
  - Sort vectors / counts: `std::vector<uint32_t> s_sortedTypeOrder;`, `uint32_t s_alphaOffCount = 0;`, `uint32_t s_alphaOnCount = 0;`, `size_t s_offGroupTotalBytes = 0;`.
  - Pin tracker: `std::vector<DWORD> s_coalescePinnedNodes;`.
  - State-machine flags: `bool s_coalesceLayoutReady = false;`, `bool s_coalesceEnabled = false;`, `bool s_coalesceArmed = false;`.
  - Per-mission ready latch: `bool s_coalesceFirstFlushDone = false;`.
  - Per-frame total bytes (cached for accessor): `size_t s_coalescePerFrameInstanceBytes = 0;` (populated in Step 5.7).
  - On-group running total (file-scope, mirrors `s_offGroupTotalBytes`): `size_t s_onGroupTotalBytes = 0;` — populated in Step 5.7 as the alpha-ON cumulative `instanceCap * sizeof(GpuStaticPropInstance)`. Used to compute `s_coalescePerFrameInstanceBytes`.
  - Env-resolved-once: `bool s_coalesceEnvDisabled = false;`.
  - Probe persisting: `bool s_hasShaderDrawParams = false;`.
- [ ] **2.2** Define `ProgramLocs` struct (§6.X). Fields: `GLint terrainMVP, terrainViewport, mvp, fogValue, debugAddrMode;` (shared); `GLint maxLocalVertexID, materialFlags, packetID;` (legacy-only); `GLint drawIDBase, texArr;` (coalesce-only). Add `static ProgramLocs s_locsLegacy;` and `static ProgramLocs s_locsCoalesce;`. Per spec §9: `glGetUniformLocation` returns -1 in the variant where the uniform is `#ifdef`-removed; treat -1 as "skip upload".
- [ ] **2.5** **Define `PerDrawEntry` C++ struct** (mirror of GLSL block in spec §5.3 / Step 8.3). Place near the existing `GpuStaticPropInstance` struct in `gos_static_prop_batcher.h` (~line 30) so the `static_assert` lives alongside its sibling:
  ```
  struct PerDrawEntry {
      int32_t packetID;
      int32_t materialFlags;
      int32_t maxLocalVertexID;
      int32_t texArrayLayer;
      float   uvScaleX;
      float   uvScaleY;
      int32_t _pad0;
      int32_t _pad1;
  };
  static_assert(sizeof(PerDrawEntry) == 32, "PerDrawEntry std430 size");
  static_assert(offsetof(PerDrawEntry, packetID)         == 0,  "packetID offset");
  static_assert(offsetof(PerDrawEntry, materialFlags)    == 4,  "materialFlags offset");
  static_assert(offsetof(PerDrawEntry, maxLocalVertexID) == 8,  "maxLocalVertexID offset");
  static_assert(offsetof(PerDrawEntry, texArrayLayer)    == 12, "texArrayLayer offset");
  static_assert(offsetof(PerDrawEntry, uvScaleX)         == 16, "uvScaleX offset");
  static_assert(offsetof(PerDrawEntry, uvScaleY)         == 20, "uvScaleY offset");
  static_assert(offsetof(PerDrawEntry, _pad0)            == 24, "_pad0 offset");
  static_assert(offsetof(PerDrawEntry, _pad1)            == 28, "_pad1 offset");
  ```
  Place AFTER the existing `GpuStaticPropInstance` static_assert block at `:35` and BEFORE the `GpuStaticPropPacket` declaration at `:38`. (Plan v3.1 said "~line 30" — corrected: the gap is at `:36–37`.)
  Required so the C++ producer in Step 5.11 and the GLSL consumer in Step 8.3 stay in lockstep per `cpp_glsl_ubo_struct_lockstep.md`. Without explicit C++ definition + asserts, the executor invents an inline struct that may not match GLSL `std430` and silently produces wrong-field reads at draw time.
- [ ] **2.3** Add forward declaration `static bool IsCoalesceEnabled();` at the top of the anon namespace.
- [ ] **2.4** Add `coalesce_resetEnvOnce()` helper body verbatim from spec §7 (memoized `getenv("MC2_SUBSTRATE_COALESCE_LEGACY")` lookup setting `s_coalesceEnvDisabled` once). Per v2r17 §7, called from BOTH `loadProgramsIfNeeded()` (top, before latch — primary) AND `IsCoalesceEnabled()` (idempotent guard).
- [ ] **2.6** **Factor `allocPermutationSsboAsIdentity(uint32_t typeCount)` as a real static helper** (v3.6 — the spec / plan invariant "every return from `finalizeGeometry()` after legacy-finalize-true must have run this" becomes a single grep-able function call instead of inlined `glGenBuffers + glBufferData` blocks scattered across return paths). Body:
  ```cpp
  static void allocPermutationSsboAsIdentity(uint32_t typeCount) {
      if (typeCount == 0) return;
      std::vector<uint32_t> identity(typeCount);
      for (uint32_t i = 0; i < typeCount; ++i) identity[i] = i;
      if (s_permutationSsbo == 0) glGenBuffers(1, &s_permutationSsbo);
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
      glBufferData(GL_SHADER_STORAGE_BUFFER,
                   typeCount * sizeof(uint32_t),
                   identity.data(),
                   GL_STATIC_DRAW);  // v3.6: matches spec §9 lifecycle table
                                     // (was GL_DYNAMIC_DRAW in v3.5; the
                                     // buffer is finalize-uploaded then
                                     // read-only thereafter — STATIC matches
                                     // semantics + driver expectations).
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }
  ```
  Place near the `coalesce_resetEnvOnce()` helper from Step 2.4. Step 5.4 calls this; Step 5.6 / 5.12 (sorted overwrite) uses `glBufferSubData` against the already-allocated handle.

---

## Step group 3 — `onMapLoad()` reset

**File:** `GameOS/gameos/gos_static_prop_batcher.cpp:490–503`

- [ ] **3.1** After `:498` (`s_geometryFinalized = false;`), append clears for: `s_sortedTypeOrder.clear();`, `s_alphaOffCount = 0;`, `s_alphaOnCount = 0;`, `s_offGroupTotalBytes = 0;`, `s_onGroupTotalBytes = 0;`, `s_coalescePerFrameInstanceBytes = 0;`, `s_coalesceLayoutReady = false;`, `s_coalesceEnabled = false;`, `s_coalesceArmed = false;`, `s_coalesceFirstFlushDone = false;`. Do NOT clear `s_coalesceEnvDisabled` / `s_hasShaderDrawParams` (process-lifetime). Do NOT clear `s_coalescePinnedNodes` (owned by `onMapUnload()`).

---

## Step group 4 — `onMapUnload()` cleanup

**File:** `GameOS/gameos/gos_static_prop_batcher.cpp:505–513`

- [ ] **4.1** Refcount-aware unpin loop over `s_coalescePinnedNodes` calling `mcTextureManager->unpinNode()` per entry; clear vector. Pattern: `gos_static_prop_registry.cpp:115`.
- [ ] **4.2** Unconditional fence cleanup over `RING_FRAMES`: `glDeleteSync` and zero each non-null `s_coalesceFence[i]`.
- [ ] **4.3** Unmap and delete `s_coalesceInstanceSsbo`. The two operations are independent — handle the case where allocation succeeded but mapping failed (Step 5.9 partial-failure path):
  ```
  if (s_coalesceInstanceSsbo) {
      if (s_coalesceInstanceMap) {
          glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_coalesceInstanceSsbo);
          glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
          s_coalesceInstanceMap = nullptr;
      }
      glDeleteBuffers(1, &s_coalesceInstanceSsbo);
      s_coalesceInstanceSsbo = 0;
  }
  ```
- [ ] **4.4** Delete `s_texArrayOff`, `s_texArrayOn`, `s_perDrawSsbo`, `s_permutationSsbo` (only if non-zero). Reset to 0.

---

## Step group 5 — `finalizeGeometry()` extension (CORE EDIT, §5.0 ordering)

**File:** `GameOS/gameos/gos_static_prop_batcher.cpp:698`

Spec refs: §5.0 authoritative ordering rule; §5.2; §5.4; §5.3; §5.6; §6 forward-compat; §CRITICAL-C; §6 v2r17 (probe inside `loadProgramsIfNeeded`, NOT here).

- [ ] **5.1** Insert §CRITICAL-C invariant comment block at top of `finalizeGeometry()` (after `:699` early-return guard, before `:707`). Pure documentation.
- [ ] **5.2** **Legacy finalize runs unchanged.** Existing body `:707–795` (`loadProgramsIfNeeded()` → `glGenVertexArrays` → VBO/IBO/per-type SSBO/`s_geometryFinalized = true`) is NOT moved or wrapped. **Note:** per Step group 7, `loadProgramsIfNeeded()` itself is modified to probe extension and decide on coalesce-program compile inside its own body — `finalizeGeometry()` doesn't need a separate probe step here.
- [ ] **5.3** **Resolve env decision** (defensive idempotent guard) by calling `coalesce_resetEnvOnce();` at the top of the post-legacy phase. (`loadProgramsIfNeeded()` already called it inside, but the second call is harmless and makes §5.0's pseudo-code self-contained.) Compute `bool coalesceWanted = !s_coalesceEnvDisabled && s_hasShaderDrawParams;`. (Both flags are now populated by the just-completed `loadProgramsIfNeeded()`.)
- [ ] **5.4** **ALWAYS call the Step 2.6 helper UNCONDITIONALLY — BEFORE the Step 5.5 early-return.** Sized to `s_types.size()`. The helper handles `glGenBuffers` if zero + `glBufferData(...GL_STATIC_DRAW)` with identity content. **Order is load-bearing:** Step 10.3 binds slot 15 unconditionally before every patch dispatch via `glBindBufferBase(15, batcher_getPermutationSsbo())`. If `s_permutationSsbo == 0` at that point (because Step 5.5 returned before 5.4 ran), the bind silently unbinds slot 15 and the patch shader reads from a null-bound SSBO → AMD-specific undefined behavior, likely all-zero `cmds[].instanceCount` writes → invisible static props on the legacy fallback path. The canonical sequence:
  ```
  coalesce_resetEnvOnce();
  const bool coalesceWanted =
      !s_coalesceEnvDisabled && s_hasShaderDrawParams;

  // ALWAYS first — before any coalesce-not-wanted return.
  allocPermutationSsboAsIdentity(s_types.size());  // Step 2.6 helper

  if (!coalesceWanted) { /* Step 5.5 below */ }
  ```
- [ ] **5.5** **Coalesce-not-wanted early return.** If `!coalesceWanted` (env-killed or extension absent — and therefore `s_staticPropProgramCoalesce == 0` from Step 7.3): set the three flags false, log `[COALESCE v1] event=disarmed reason=no_extension|env_killswitch`, `return`. Identity permutation from Step 5.4 is in place; legacy is finalized.
- [ ] **5.6** **Compute alpha-class per type** per §CRITICAL-C. Walk packets and OR-reduce, **mirroring the existing legacy guard at `:1677–1679` exactly** to handle malformed types without crashing:
  ```
  for (each packet pkt of type) {
      bool pktAlpha = (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0;
      if (type.source &&
          type.source->listOfTextures &&
          pkt.textureSlot < type.source->numTextures &&
          type.source->listOfTextures[pkt.textureSlot].textureAlpha) {
          pktAlpha = true;
      }
      typeHasAlpha |= pktAlpha;
  }
  type.alphaClass = typeHasAlpha ? 1 : 0;
  ```
  An unguarded `type.source->listOfTextures[...]` access here is a null-deref / OOB at finalize time for any malformed type slot — that is a hard crash at map load, not a silent miscompare. If any type has packets internally disagreeing on alpha after slot resolution (one packet alpha-on, another alpha-off in the same type), log `mixed_alpha type=N`, set flags false, `return`.
- [ ] **5.7** **Build sort + per-type caps + group totals.** Build `s_sortedTypeOrder` (alpha-OFF first stable in registration order, then alpha-ON). Set `s_alphaOffCount`, `s_alphaOnCount`. Compute per-type `instanceCap` per §5.1 formula. Compute `s_offGroupTotalBytes` (running sum of `instanceCap * sizeof(GpuStaticPropInstance)` over alpha-OFF types) AND `s_onGroupTotalBytes` (running sum over alpha-ON types). Compute `s_coalescePerFrameInstanceBytes = s_offGroupTotalBytes + s_onGroupTotalBytes;`. Compute each `type.coalesceByteOffsetWithinGroup` (group-relative running sum within sorted order; 0 for first type per group). Set `s_coalesceLayoutReady = true;`.
- [ ] **5.8** **Verification asserts** (§3.Z): `sizeof(GpuStaticPropInstance) % GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT == 0`; `s_offGroupTotalBytes` alignment-clean; per-type `coalesceByteOffsetWithinGroup` alignment-clean.
- [ ] **5.9** **Allocate `s_coalesceInstanceSsbo` ring.** `glGenBuffers + glBindBuffer + glBufferStorage(RING_FRAMES * per_frame_total_bytes, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT)`. Then `glMapBufferRange(...0, total, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT)` and persist pointer in `s_coalesceInstanceMap`. On failure: flags false, log `alloc_failed`, return.
- [ ] **5.10** **Build per-group `GL_TEXTURE_2D_ARRAY`** per §5.4. **Before the group loop**, declare at `finalizeGeometry()` function scope (NOT inside the per-group loop — destruction-before-Step-5.11 hazard):
  ```
  std::vector<int32_t> layerForType(s_types.size(), -1);
  ```
  `-1` sentinel marks "not assigned" (zero-packet types stay -1; Step 5.11 detects and emits a no-op draw entry). Then for each non-empty group:
  - 5.10.a **Inside the per-group loop body** (so rollback in 5.10.c only unpins the current group's temp pins), declare:
    ```
    std::vector<DWORD> newlyPinnedThisBuild;
    ```
    Walk types in sorted order. **Zero-packet guard** (mirrors `batcher_getTypeDrawInfo` pattern at `:1982–1989`): if `type.packetCount == 0`, leave `layerForType[typeID] = -1` and `continue`. Otherwise resolve `firstPkt = s_packets[type.firstPacket]`. Read `gosHandle = src->listOfTextures[firstPkt.textureSlot].gosTextureHandle` (live re-resolve, same null/OOB guards as Step 5.6 if `type.source` could be null here — registered types should always have non-null source, but be defensive). Set `type.lastSeenGosHandle = gosHandle`. Pin: `mcTextureManager->pinNode(src->listOfTextures[firstPkt.textureSlot].mcTextureNodeIndex)`. Append nodeId to `newlyPinnedThisBuild` — NOT directly to `s_coalescePinnedNodes`.
  - 5.10.b Read each unique GL texture's dimensions via `glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH/HEIGHT, ...)`. Deduplicate by `glTexId`. Assign `texArrayLayer` per unique. **Record per-type layer in the `layerForType` vector declared at function scope above:** `layerForType[typeID] = layer;` (cast to `int32_t` from the unique-layer index). Layers are GROUP-relative — alpha-OFF group's layers run `[0, uniqueOff)` against `s_texArrayOff`, alpha-ON group `[0, uniqueOn)` against `s_texArrayOn`. Types with `packetCount == 0` keep their `-1` sentinel.
  - 5.10.c **Same-size assertion.** If any unique differs from group's first dimensions: log `size_mismatch group=off|on expected=WxH got=WxH`, then call the `coalesceRollbackTexBuild(newlyPinnedThisBuild)` helper (defined alongside the other coalesce helpers from Step 2.4 / 2.6 — see helper definition below). Set flags false; `return`.

    **Helper definition** (place near `allocPermutationSsboAsIdentity` from Step 2.6, file-scope in `gos_static_prop_batcher.cpp`):
    ```cpp
    // v3.7: factored from the Step 5.10.c rollback path so Step 12B.5's
    // forced-size-mismatch test hook reuses the same code instead of
    // duplicating it (or reaching for a goto). Per v2r15 out3-MIN-F
    // rollback contract: delete BOTH texture arrays if either is non-zero
    // (the OFF group may have succeeded before the ON group fails — the
    // half-built array still leaks if not deleted), then unpin only the
    // CURRENT group's temp pins. Do NOT touch the other group's
    // already-promoted pins in s_coalescePinnedNodes — those release at
    // onMapUnload().
    static void coalesceRollbackTexBuild(std::vector<DWORD>& tempPins) {
        if (s_texArrayOff) { glDeleteTextures(1, &s_texArrayOff); s_texArrayOff = 0; }
        if (s_texArrayOn)  { glDeleteTextures(1, &s_texArrayOn);  s_texArrayOn  = 0; }
        for (DWORD nodeId : tempPins) {
            mcTextureManager->unpinNode(nodeId);
        }
        tempPins.clear();
    }
    ```
    Caller responsibility: set `s_coalesceLayoutReady`, `s_coalesceEnabled`, `s_coalesceArmed` to `false` AFTER calling the helper, then `return`. The helper deliberately does NOT mutate the flags so Step 12B.5 (which logs a different "(forced)" reason string before invoking the rollback) keeps logging-vs-state-change ordering symmetric with the natural failure path.
  - 5.10.d `glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, W, H, numLayers, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr)` per `mc2_argb_packing.md` and precedent at `gos_terrain_indirect.cpp:1735–1755`.
  - 5.10.e Per unique texture: alloc `W*H*4` bytes; `glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, buf)`; `glTexSubImage3D(... layer, 0, GL_BGRA, GL_UNSIGNED_BYTE, buf)`. Free buffers.
  - 5.10.f `glGenerateMipmap(GL_TEXTURE_2D_ARRAY)`. Set `GL_TEXTURE_MAX_LEVEL` to full mip count (forestall AMD strict-fail). Sampler params: `GL_REPEAT` wrap, `GL_LINEAR_MIPMAP_LINEAR` min, `GL_LINEAR` mag.
  - 5.10.g On group success, **promote** temp pins: `s_coalescePinnedNodes.insert(... newlyPinnedThisBuild ...);` clear temp.
- [ ] **5.11** **Build per-draw SSBO `s_perDrawSsbo`** per §5.3 — uses the C++ `PerDrawEntry` struct defined in Step 2.5. One entry per type in sorted order across both groups. For each type at sorted slot `i`:
  ```
  PerDrawEntry entry{};
  if (type.packetCount == 0 || layerForType[typeID] < 0) {
      // Zero-packet sentinel — Step 9.2 already emits a zero-instance
      // cmd for this slot; the entry's contents are unread by any draw
      // but must be present to keep array indexing aligned.
      entry.texArrayLayer = 0;
      // ... other fields zero ...
  } else {
      entry.packetID         = (int32_t)type.firstPacket;
      entry.materialFlags    = type.alphaClass ? (int32_t)STATIC_PROP_FLAG_ALPHA_TEST : 0;
                               // type.alphaClass was OR-reduced from BOTH
                               // pkt.materialFlags AND per-slot textureAlpha
                               // in Step 5.6 — using only first packet's
                               // materialFlags would miss textureAlpha-only
                               // types and break the shader's alpha-test
                               // discard path (CRITICAL-C).
      entry.maxLocalVertexID = (type.vertexCount > 0u) ? (int32_t)(type.vertexCount - 1u) : 0;
                               // Matches legacy upload at :1567.
      entry.texArrayLayer    = layerForType[typeID];
                               // Group-relative; recorded in Step 5.10.b.
      entry.uvScaleX         = 1.0f;
      entry.uvScaleY         = 1.0f;
      entry._pad0            = 0;
      entry._pad1            = 0;
  }
  ```
  `glGenBuffers + glBindBuffer + glBufferData(N * sizeof(PerDrawEntry), data, GL_STATIC_DRAW)`. Use `sizeof(PerDrawEntry)` (= 32 by static_assert) rather than literal `32` so the call site auto-tracks any future struct change.
- [ ] **5.12** **Sort permutation overwrite — LAST step on success.** Build `permutation[typeID] = sortedSlot` mapping; `glBufferSubData(s_permutationSsbo, 0, typeCount * sizeof(uint32_t), permutation.data())`. Identity from 5.4 stays if any earlier step returned.
- [ ] **5.13** Set `s_coalesceEnabled = true; s_coalesceArmed = true;`. Log `event=armed types=N off_types=A on_types=B unique_tex_off=U unique_tex_on=V per_frame_inst_bytes=B elapsed_ms=T`. Capture elapsed via `std::chrono::steady_clock` from finalize entry.

---

## Step group 6 — Identity-permutation invariant verification

- [ ] **6.1** Inspect Step 5 in order. Verify EVERY return path (5.5 no-extension, 5.6 mixed-alpha, 5.9 alloc-failed, 5.10.c size-mismatch) reaches function exit AFTER Step 5.4's identity-permutation alloc, AND with `s_geometryFinalized == true` (Step 5.2 ran first per §5.0). Failure to enforce this re-introduces the v2r14 blackout bug.
- [ ] **6.2** No new code. Pure structural verification before moving on.

---

## Step group 7 — Two shader programs in `loadProgramsIfNeeded()` (CORE EDIT, v2r17 ordering)

**File:** `GameOS/gameos/gos_static_prop_batcher.cpp:192–238`

Spec refs: §6 (probe inside `loadProgramsIfNeeded()` BEFORE latch), §6.X shared-uniform contract, §7 strengthened `IsCoalesceEnabled()`, §6.Y `flushShadow()` forward-compat.

**Critical v2r17 ordering inside `loadProgramsIfNeeded()`:**
```
if (s_programLoadTried) return;
s_programLoadTried = true;
coalesce_resetEnvOnce();                              // env decision
s_hasShaderDrawParams = glewIsSupported(...);         // extension probe
[ legacy program compile + s_locsLegacy fill ]
if (s_hasShaderDrawParams && !s_coalesceEnvDisabled) {
    [ coalesce program compile + s_locsCoalesce fill + u_texArr unit-0 bind ]
}
```

The probe + env-decision MUST land BEFORE the latch fires, otherwise the coalesce program never compiles. v2r14 spec was silent on this; v2r15/v2r16 misordered it after `:795`; v2r17 fixed it inside `loadProgramsIfNeeded()`.

- [ ] **7.1** Replace single `kShaderPrefix = "#version 430\n"` at `:208` with two: `kShaderPrefixLegacy = "#version 430\n"` and `kShaderPrefixCoalesce = "#version 430\n#extension GL_ARB_shader_draw_parameters : require\n#define MC2_COALESCE 1\n"`. Compile-time string-literal concatenation.
- [ ] **7.2** Inside `loadProgramsIfNeeded()`, after `s_programLoadTried = true;` at `:201` and BEFORE the existing `:209` `glsl_program::makeProgram` call, insert: `coalesce_resetEnvOnce();` then `s_hasShaderDrawParams = glewIsSupported("GL_ARB_shader_draw_parameters");` (pattern verified at `gameosmain.cpp:930`).
- [ ] **7.3** First `makeProgram` call (currently `:209–213`) keeps program name `"static_prop"` and uses `kShaderPrefixLegacy`. Result GLuint goes to `s_staticPropProgram` (legacy) — no rename.
- [ ] **7.4** **Populate `s_locsLegacy`** after legacy program link, before any coalesce work. Cache all eight uniform locations via `glGetUniformLocation(s_staticPropProgram, "<exact-glsl-name>")`. **GLSL string literals are mixed-prefix — reproduce exactly:**
  ```cpp
  s_locsLegacy.terrainMVP        = glGetUniformLocation(s_staticPropProgram, "terrainMVP");          // NO u_ prefix
  s_locsLegacy.terrainViewport   = glGetUniformLocation(s_staticPropProgram, "u_terrainViewport");
  s_locsLegacy.mvp               = glGetUniformLocation(s_staticPropProgram, "u_mvp");
  s_locsLegacy.fogValue          = glGetUniformLocation(s_staticPropProgram, "u_fogValue");
  s_locsLegacy.debugAddrMode     = glGetUniformLocation(s_staticPropProgram, "u_debugAddrMode");
  s_locsLegacy.maxLocalVertexID  = glGetUniformLocation(s_staticPropProgram, "u_maxLocalVertexID");
  s_locsLegacy.materialFlags     = glGetUniformLocation(s_staticPropProgram, "u_materialFlags");
  s_locsLegacy.packetID          = glGetUniformLocation(s_staticPropProgram, "u_packetID");
  ```
  Verified against existing `flush()` upload sites: `:1447` `terrainMVP` (no prefix — `static_prop.vert:69`), `:1454` `u_terrainViewport`, `:1457` `u_mvp`, `:1461` `u_debugAddrMode`, `:1466` `u_fogValue`, `:1567` `u_maxLocalVertexID`, `:1685` `u_materialFlags`, `:1687` `u_packetID`. Coalesce-only fields (`drawIDBase`, `texArr`) stay -1 in `s_locsLegacy`.
- [ ] **7.5** Add second `makeProgram` call gated on `if (s_hasShaderDrawParams && !s_coalesceEnvDisabled)`. Distinct program name `"static_prop_coalesce"` (cache at `shader_builder.cpp:611–614` rejects duplicate names — distinct name required). Uses `kShaderPrefixCoalesce`. Result goes to `s_staticPropProgramCoalesce`. If link fails (returned 0 OR `is_valid()` false), leave `s_staticPropProgramCoalesce = 0` — strengthened `IsCoalesceEnabled()` (Step 7.7) catches this.
- [ ] **7.6** **Populate `s_locsCoalesce`** after coalesce link succeeds. Same exact GLSL strings as Step 7.4 for the five shared uniforms. Coalesce-only uniforms add `"u_drawIDBase"` and `"u_texArr"`. Legacy-only uniforms (`u_materialFlags`, `u_maxLocalVertexID`, `u_packetID`) are removed by the `#define MC2_COALESCE 1` preprocessor branch in `static_prop.frag` (Step 8.5), so `glGetUniformLocation` returns -1 for them on the coalesce program — that's expected; `s_locsCoalesce.materialFlags`/`maxLocalVertexID`/`packetID` stay -1 and Step 11.7.d's upload helper skips -1 locations.
  ```cpp
  s_locsCoalesce.terrainMVP        = glGetUniformLocation(s_staticPropProgramCoalesce, "terrainMVP");          // NO u_ prefix
  s_locsCoalesce.terrainViewport   = glGetUniformLocation(s_staticPropProgramCoalesce, "u_terrainViewport");
  s_locsCoalesce.mvp               = glGetUniformLocation(s_staticPropProgramCoalesce, "u_mvp");
  s_locsCoalesce.fogValue          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_fogValue");
  s_locsCoalesce.debugAddrMode     = glGetUniformLocation(s_staticPropProgramCoalesce, "u_debugAddrMode");
  s_locsCoalesce.drawIDBase        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_drawIDBase");
  s_locsCoalesce.texArr            = glGetUniformLocation(s_staticPropProgramCoalesce, "u_texArr");
  // s_locsCoalesce.materialFlags / maxLocalVertexID / packetID stay -1 (legacy-only, removed under MC2_COALESCE).
  ```
  Bind `u_texArr` to texture unit 0 once: `glUseProgram(s_staticPropProgramCoalesce); glUniform1i(s_locsCoalesce.texArr, 0); glUseProgram(0);` (out-MAJ-5 / spec §6 sampler hygiene).
- [ ] **7.7** **Strengthened `IsCoalesceEnabled()` body** (file-scope helper, defined in same TU as `flush()`):
  ```
  coalesce_resetEnvOnce();
  if (s_coalesceEnvDisabled)             return false;
  if (!s_hasShaderDrawParams)            return false;
  if (!s_geometryFinalized)              return false;
  if (!s_coalesceLayoutReady)            return false;
  if (!s_coalesceEnabled)                return false;
  if (!s_coalesceArmed)                  return false;
  if (s_staticPropProgramCoalesce == 0)  return false;
  if (s_coalesceInstanceSsbo == 0)       return false;
  if (s_perDrawSsbo == 0)                return false;
  if (s_permutationSsbo == 0)            return false;
  if (s_alphaOffCount == 0 && s_alphaOnCount == 0) return false;
  if (gos_object_parity::IsParityCheckEnabled())   return false;
  return true;
  ```
- [ ] **7.8** **`flushShadow()` forward-compat note** (§6.Y / out3-MIN-E). Add inline comment at `:1794`: `// IsCoalesceEnabled() is irrelevant for the shadow path. Future Task 13 implementation MUST use the legacy/shadow program path; see spec §6.Y.` No behavioral change.

---

## Step group 8 — Shader edits

**Files:** `shaders/static_prop.vert`, `shaders/static_prop.frag`

Spec refs: §5.7, §CRITICAL-E, §5.3, §5.3a, §6 `#ifdef MC2_COALESCE`.

**v2r17 cadence note (out4-MIN-1):** `u_maxLocalVertexID` is uploaded **per-TYPE at `:1567`** (NOT per-packet). `u_materialFlags` and `u_packetID` ARE uploaded per-packet at `:1685` and `:1687`. PerDrawEntry is the right destination for all three (sorted-slot indexing matches per-type cadence; per-packet uniforms in coalesce mode collapse to per-type because the multi-draw issues one cmd per type). Plan prose conflated this in v2.

- [ ] **8.1** `static_prop.vert:111–112`: wrap `Instance inst = instances_.i[gl_InstanceID];` in `#ifdef MC2_COALESCE / #else / #endif`. The `#ifdef` branch reads `instances_.i[gl_BaseInstanceARB + gl_InstanceID]` (ARB-suffixed names mandatory under `#version 430` + extension).
- [ ] **8.2** `static_prop.vert`: add `flat out uint v_drawID;` to outs near `:103–109`. In `main()`, set `v_drawID = uint(gl_DrawIDARB);` under `#ifdef MC2_COALESCE`, else `v_drawID = 0u;`. **Note on parity path:** the existing parity-readback writes at `static_prop.vert:276, :286` use bare `gl_InstanceID`. They are NOT wrapped in `#ifdef MC2_COALESCE`. This is correct because `IsCoalesceEnabled()` (Step 7.7) returns false when `gos_object_parity::IsParityCheckEnabled()` is true — parity always runs the legacy program where bare `gl_InstanceID` is the right index. No change needed.
- [ ] **8.3** `static_prop.frag` near `:27`: add `flat in uint v_drawID;` after `flat in uint v_localVertexID;`. Add `#ifdef MC2_COALESCE`-guarded `uniform int u_drawIDBase;` (`int` per `uniform_uint_crash.md`). Add `PerDrawEntry` struct + `layout(std430, binding = 4) readonly buffer PerDrawData { PerDrawEntry entries[]; } perDraw_;` declaration under `#ifdef MC2_COALESCE`.
- [ ] **8.4** `static_prop.frag:29` `uniform sampler2D u_tex;` → wrap: `#ifdef MC2_COALESCE` → `uniform sampler2DArray u_texArr;`, `#else` → `uniform sampler2D u_tex;`, `#endif`.
- [ ] **8.5** `static_prop.frag`: branch the three legacy uniforms (`u_materialFlags` per-packet at `:1685`, `u_packetID` per-packet at `:1687`, `u_maxLocalVertexID` per-type at `:1567`) into local variables. Under `#ifdef MC2_COALESCE`, read from `perDraw_.entries[v_drawID + uint(u_drawIDBase)]`. Under `#else`, read directly from legacy uniforms. Replace all in-shader uses (`:51, :56, :62, :84` per current line refs).
- [ ] **8.6** `static_prop.frag:49` `texture(u_tex, v_uv)` → `#ifdef MC2_COALESCE` → `texture(u_texArr, vec3(v_uv, float(perDraw_.entries[v_drawID + uint(u_drawIDBase)].texArrayLayer)))`, else legacy. UVs at scale 1.0 per §5.4.
- [ ] **8.7** Build with `cmake --build build64 --config RelWithDebInfo --target gameos`. Hot-reload fails silently — visual-inspect build log for shader_builder errors.

---

## Step group 9 — `compute_buildIndirectBuffer()` body refactor

**File:** `GameOS/gameos/gpu_cull_compute.cpp:509–572`

Spec refs: §5.5 two-branch pseudo-code; §5.6a cap-semantics naming convention; §3.Z group-relative addressing.

**v2r17 naming convention (§5.6a, mandatory):** sorted/coalesce branch uses `coalesceInstanceCap`; natural/legacy branch uses `legacyVisibleIdsCap`. Bare `instanceCap` is forbidden in this file going forward.

- [ ] **9.1** At top of body after `cmds(typeCount)` allocation at `:543`, branch on `batcher_isCoalesceLayoutReady()`. Two branches stay distinct — different SSBOs, different `cmd.baseInstance` semantics.

  **Hoist `cumBase` declaration to function scope** (CRITICAL — convergent finding from v3.3 review). Existing code declares `uint32_t cumBase = 0;` INSIDE the original natural-only loop preamble at `:548`. With Step 9.1's branch split, this declaration must move to function scope BEFORE the branch so BOTH branches feed the same accumulator that `:574` (`totalVisibleSlots = cumBase`) reads to size `s_visibleIdsBuf` at `:587–591`. Without this hoist, the sorted branch leaves `cumBase = 0`, the buffer allocates as 4 bytes (the `> 0 ? : 1` clamp), and the cull shader writes hundreds of MB OOB.

  **CRITICAL EXECUTOR INSTRUCTION (v3.4 review fix):** DELETE the existing `uint32_t cumBase = 0;` declaration at `:548`. There is exactly ONE function-scope `cumBase`, and BOTH sorted and natural branches accumulate into it. Leaving the `:548` declaration in place — even with the function-scope hoist added above — produces either (a) a redeclaration compile error, or (b) silent inner-scope shadowing where natural writes to the inner shadow and `:574` reads the outer (zero-initialized) — recapitulating the v3.3 bug v3.4 was designed to fix.
  ```
  // BEFORE the if/else split, at function scope (REPLACE the :548 declaration):
  uint32_t cumBase = 0;  // visibleIds[] allocator, keyed by natural-typeID semantics
                         // — used by both sorted and natural branches.
  ```
- [ ] **9.2** **SORTED branch.** This branch maintains TWO INDEPENDENT layouts:
  - **Cull-side `visibleIds[]` layout**, keyed by NATURAL `typeID`: `bucketCaps[typeID]` and `bucketBases[typeID]` arrays. The cull shader at `gpu_cull.comp:91-93, 231-241` reads `capsData[bucket]` and `capsData[u_nBuckets + bucket]` keyed by natural typeID; if these are zero, the cull's `if (slot >= cap)` overflow gate fires for every visible static prop and `bucketCountData[typeID]` stays 0 → patch writes `instanceCount=0` everywhere → invisible draws. The sorted branch must populate them with the same legacy semantics as the natural branch (per opus C1 finding).
  - **Draw-command layout**, indexed by SORTED slot `i`: `cmds[i]` with `baseInstance` as the group-relative coalesce-instance-buffer offset (per §3.Z).

  Read `sortedOrder = batcher_getSortedTypeOrder()`, `N_off = batcher_getAlphaOffCount()`. Initialize the two coalesce-side accumulators: `uint32_t cumCapOff = 0, cumCapOn = 0;`. (`cumBase` was already hoisted to function scope in Step 9.1 and is shared with the natural branch.) Iterate `i ∈ [0, typeCount)`: `typeID = sortedOrder[i]`.

  Note on `batcher_getTypeDrawInfo` contract (`gos_static_prop_batcher.cpp:1963–2001`): the function returns `true` even for zero-packet types — it returns `false` only when geometry isn't finalized or `typeID` is out of range. **It still returns the legacy visibleIds stride (`s_instanceCapacity`) regardless of `packetCount`** — so `legacyVisibleIdsCap` is `s_instanceCapacity` for ALL valid types, including zero-packet ones. Zero geometry is represented by `cmd.count == 0`, NOT by `legacyVisibleIdsCap == 0`.

  ```
  uint32_t indexCount = 0, firstIndex = 0;
  int32_t  baseVertex = 0;
  uint32_t legacyVisibleIdsCap = 0;
  batcher_getTypeDrawInfo(typeID, &indexCount, &firstIndex,
                          &baseVertex, &legacyVisibleIdsCap);
  const uint32_t coalesceInstanceCap = batcher_getInstanceCap(typeID);

  // Draw-command layout (sorted slot indexing).
  DrawCmd& cmd = cmds[i];
  cmd.count         = indexCount;             // 0 for zero-packet types — sentinel for no-op draw
  cmd.instanceCount = 0;                      // patch shader writes per-frame; clamps to 0 when count==0 (Step 10.2)
  cmd.firstIndex    = firstIndex;
  cmd.baseVertex    = baseVertex;
  cmd.baseInstance  = (i < N_off) ? cumCapOff : cumCapOn;

  // Group-relative coalesce-instance-buffer accumulators advance UNCONDITIONALLY
  // so subsequent baseInstance values stay aligned with type.coalesceByteOffsetWithinGroup.
  if (i < N_off) cumCapOff += coalesceInstanceCap;
  else           cumCapOn  += coalesceInstanceCap;

  // Cull-visibleIds layout (natural typeID indexing) — populate using the SAME
  // function-scope `cumBase` that the natural branch uses. The coalesce draw
  // path doesn't read visibleIds[], but the cull shader still writes into it
  // (keyed by natural typeID via `bucket = rec.category >> 4`), and the
  // `:574 totalVisibleSlots = cumBase` line allocates the buffer for both
  // branches. Bases are unique per typeID; non-overlapping under any
  // permutation order.
  bucketCaps [typeID] = legacyVisibleIdsCap;
  bucketBases[typeID] = cumBase;
  cumBase             += legacyVisibleIdsCap;
  ```

  Empty-type / zero-packet handling: no special-case `continue` needed. `legacyVisibleIdsCap` is non-zero (= `s_instanceCapacity`), so each type gets a non-overlapping `visibleIds[]` slice; `cmd.count = 0` makes `glMultiDrawElementsIndirect` skip the entry without reading instances; patch shader's Step 10.2 clamp (`if (cmds[sortedSlot].count == 0u) n = 0u;`) ensures `instanceCount = 0` even if cull dispatch wrote a non-zero `bucketCountData` for the zero-packet bucket.
- [ ] **9.3** **NATURAL branch** (legacy / coalesce-disabled): preserved **semantically** from existing `:548–572`, with the local `instanceCap` renamed to `legacyVisibleIdsCap` (per §5.6a naming convention applied to BOTH branches; option Y of v3.3 review). The accumulator is the function-scope `cumBase` (hoisted in Step 9.1), unchanged from existing behavior. `visibleIds[]` allocation downstream at `:587–591` is unchanged. The empty-type early-`continue` at `:553–558` (`if (!batcher_getTypeDrawInfo(...)) { cmds[t] = {0,0,0,0,cumBase}; bucketCaps[t] = 0; bucketBases[t] = cumBase; continue; }`) is preserved verbatim — it fires only on out-of-range / not-finalized typeIDs (which should never happen post-finalize), not on zero-packet types.
- [ ] **9.4** Existing `glBufferData(s_indirectCmdBuf, cmds.data())` at `:581` is unchanged.
- [ ] **9.5** Verify by inspection: `code/mission.cpp:3094` is the only caller. No double-build.

---

## Step group 10 — `gpu_cull_patch.comp` permutation read (binding **15**)

**File:** `shaders/gpu_cull_patch.comp:35–55`

Spec refs: §5.6 (binding 15 — v2r17 final).

**Binding history (cite once for executor benefit):** v2r2..v2r14 specified 13 (collided with `BlockVis` at `gpu_cull_block_rollup.comp:58`); v2r15 flipped to 14 (collided with diagnostic readback at `gpu_cull_compute.cpp:855`); v2r16+ uses **15** (verified free against shader declarations AND C++ binding readbacks).

- [ ] **10.1** Add new SSBO declaration after the existing `IndirectCmds` block at `:39–42`: `layout(std430, binding = 15) readonly buffer PermutationBuf { uint permutation[]; };`. Verified free — no shader under `shaders/` declares `binding = 15`. **Also remove `writeonly` from the existing `IndirectCmds` block at `:40`** — Step 10.2 needs to READ `cmds[sortedSlot].count` to gate the `instanceCount` write. Change `:40` from `layout(std430, binding = 11) writeonly buffer IndirectCmds { ... };` to `layout(std430, binding = 11) buffer IndirectCmds { ... };` (default is read/write).
- [ ] **10.2** Modify `main()` body at `:49–55`: after `if (b >= uint(u_nBuckets)) return;`, write through permutation **with a zero-geometry clamp** to avoid `glMultiDrawElementsIndirect` consuming `{count=0, instanceCount>0}` on zero-packet types (driver-defined per OpenGL 4.6 spec §10.5; AMD typically no-ops, some drivers crash):
  ```glsl
  uint sortedSlot = permutation[b];
  uint n = bucketCountData[b];

  // Defensive zero-geometry guard (plan v3.3 stop-line fix).
  // Even if cull dispatch wrote a non-zero bucket count for a zero-packet
  // type (cull doesn't filter on packet count), force instanceCount=0
  // when the draw command's index count is 0. Centralizes the rule at
  // the only point that writes instanceCount per frame.
  if (cmds[sortedSlot].count == 0u) {
      n = 0u;
  }
  cmds[sortedSlot].instanceCount = n;
  ```
  Identity-loaded permutation (`permutation[typeID] == typeID`) preserves legacy semantics on disarm; the clamp also fires for legacy-mode zero-packet types, which is harmless because legacy already handled them via `cmds[t] = {0,0,0,0,cumBase}` in `compute_buildIndirectBuffer` natural branch.
- [ ] **10.3** Bind binding 15 in C++ around the patch dispatch at `gpu_cull_compute.cpp:835` (`glDispatchCompute(patchGroups, 1, 1)`), wrapped in a save/restore envelope per spec §3.X / §9 binding-hygiene contract. Insert the save and bind between `:828` (existing `INDIRECT_CMD_BINDING` bind) and `:830` (`glUseProgram(s_patchProgram)`). Insert the restore immediately after `:835` (`glDispatchCompute`):
  ```cpp
  // ---- Slot 15: save → bind → dispatch → restore (Step 10.3, v3.7) ----
  // Slot 15 history: 13 collided with BlockVis, 14 collided with the
  // diagnostic at :855. Slot 15 is private to the patch shader; this
  // rebind is mandatory for legacy-fallback correctness. The save/restore
  // envelope honors spec §3.X / §9 binding hygiene at the slot-15
  // mutation site (the draw branch in flush() does NOT bind slot 15, so
  // its 11.7.a/j envelope covers slot 4 + 2D_ARRAY only).
  GLint prevSsbo15 = 0;
  glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 15, &prevSsbo15);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, batcher_getPermutationSsbo());
  // existing :830  glUseProgram(s_patchProgram);
  // existing :835  glDispatchCompute(patchGroups, 1, 1);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, (GLuint)prevSsbo15);
  ```
  The shader needs slot 15 only during the dispatch; restore-immediately-after is sufficient. The patch shader cannot observe a stale binding because GL serializes the dispatch's SSBO reads before subsequent client state queries. Memory barriers are unaffected — the existing cull→patch barrier at `gpu_cull_compute.cpp:820` continues to handle `bucketCountData[]` cross-dispatch ordering, and the post-patch (patch→draw) barrier at `:843` is unchanged.

  **No per-frame barrier needed for the permutation SSBO** (v3.6 clarification, retained): `s_permutationSsbo` is finalize-uploaded by Step 2.6's `glBufferData` (and at most overwritten once by Step 5.12's `glBufferSubData` in the same finalize pass) and is read-only thereafter. GL ordering guarantees the upload is visible to all subsequent dispatches without an explicit `glMemoryBarrier`. Do not add a `GL_BUFFER_UPDATE_BARRIER_BIT` for permutation.

---

## Step group 11 — `flush()` extensions (CORE EDIT)

**File:** `GameOS/gameos/gos_static_prop_batcher.cpp:1316–1726`

Spec refs: §3 ASCII diagram; §3.X SSBO save/restore (slot 4 + 2D_ARRAY in flush()'s draw branch; slot 15 at its mutation site in `compute_dispatch()` per Step 10.3 — v3.7); §3.X.1 legacy prologue inheritance (slot 2 only — slot 1 is unread by live shader per v2r18); §3.Z addressing invariant; §5.1b CPU write loop; §5.3a per-group `u_drawIDBase`; §5.4 texture-eviction detect; §6.X shared-uniform upload.

**Key v2r18 invariants:**
- Legacy prologue at `:1430–1474` runs unconditionally (slot 2 inherited).
- Slot 1 is NOT bound by the coalesce branch (per v2r18 §3.X.1 — `colors_.c[...]` unread by live shader).
- Save/restore for slot 4 + unit-0 `GL_TEXTURE_BINDING_2D_ARRAY` in flush()'s draw branch (Step 11.7.a/j). Slot 15 is saved/restored at its mutation site in `compute_dispatch()` per Step 10.3 — the draw branch never binds slot 15.
- Shared uniforms re-uploaded to coalesce program after `glUseProgram`.

- [ ] **11.1** **Unconditional coalesce fence cleanup** — runs **after `uploadAllBucketsIfNeeded()` returns at `:1325`, before the Step 11.2 coalesce CPU write loop.** (v3.6 anchor fix: prior wording referenced `:1260–1263`, but that legacy fence-wait is internal to `uploadAllBucketsIfNeeded` and only runs when `s_lastUploadedSlot != s_frameSlot` advances the ring; the coalesce cleanup must hang off the post-upload return point so it shares the same "ring slot is stable for this frame" invariant.) If `s_coalesceFence[s_frameSlot]` non-null, `glClientWaitSync(GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED)` (matching legacy fence pattern), `glDeleteSync`, zero. Runs always — out-MAJ-2 ensures fences from disarmed-mid-mission frames drain. Setting `s_coalesceFirstFlushDone = true` here is wrong — the latch is for first ARMED flush; place the latch set in 11.7.k after the draw issues.
- [ ] **11.2** **Coalesce per-frame CPU write loop** — runs `if (IsCoalesceEnabled())`. Runs after `uploadAllBucketsIfNeeded()` returns at `:1325`, BEFORE the existing `:1554` legacy bucket bind loop. Compute `fr_off_bytes = s_frameSlot * batcher_getCoalescePerFrameInstanceBytes()`. Set `coalesceMapBase = (uint8_t*)s_coalesceInstanceMap + fr_off_bytes`. Iterate `s_bucketsByType` (order doesn't matter — each write addresses its own offset). For each bucket: read `type = s_types[typeID]`; compute `groupBase_bytes = (type.alphaClass == 1) ? s_offGroupTotalBytes : 0`; compute `dst = coalesceMapBase + groupBase_bytes + type.coalesceByteOffsetWithinGroup`.
- [ ] **11.3** **Per-type overflow guard** inside 11.2 loop, BEFORE `memcpy`: if `bucket.instances.size() > type.instanceCap`, log `[COALESCE v1] event=disarmed reason=type_overflow type=N count=K cap=C`, `s_coalesceArmed = false;`, `break`. Out-CRIT-4: check on `bucket.instances.size()`, NOT patched GPU `instanceCount`. **Runtime disarm only flips the flag — it does NOT trigger an indirect-buffer rebuild.** The legacy draw path may render with stale sorted-layout `cmd.baseInstance` values for the rest of the mission; this is a known visual-fallback limitation (see plan front matter "Runtime disarm fallback limitation"). Do NOT attempt mid-frame `compute_buildIndirectBuffer` rebuild — the patch shader has already written `instanceCount` into `s_indirectCmdBuf` this frame, and `glBufferData` would orphan and zero those counts → blackout the disarm frame entirely.
- [ ] **11.4** **Eviction-detect** (one pass, ~323 types): for each, compare `type.lastSeenGosHandle` against `src->listOfTextures[firstPkt.textureSlot].gosTextureHandle`. On mismatch: log `[COALESCE v1] event=disarmed reason=tex_evicted type=N old_handle=H new_handle=H`, `s_coalesceArmed = false;`, break. Same flag-only-disarm semantics as 11.3.
- [ ] **11.5** Memcpy: `std::memcpy(dst, bucket.instances.data(), bucket.instances.size() * sizeof(GpuStaticPropInstance));`.
- [ ] **11.6** **Legacy write path stays intact** — does NOT gate off when coalesce is armed (parity / debug / kill-switch hot-toggle).
- [ ] **11.7** **Coalesce draw branch.** Runs `if (IsCoalesceEnabled())`. (Per v2r18 spec, no `debugAddrMode_` carve-out is needed — all debug modes 0–7 read `v_argb`, computed identically in legacy and coalesce vert paths.)

  **Compute group-size locals at top of branch** (used by 11.7.g/h `glBindBufferRange` calls). Hoisted to branch scope so 11.7.g and 11.7.h share one declaration site:
  ```
  const size_t fr_off_bytes =
      s_frameSlot * batcher_getCoalescePerFrameInstanceBytes();
  const size_t off_total_bytes = s_offGroupTotalBytes;
  const size_t on_total_bytes  =
      batcher_getCoalescePerFrameInstanceBytes() - s_offGroupTotalBytes;
  ```

  - 11.7.a **Save SSBO binding 4 + unit-0 `GL_TEXTURE_BINDING_2D_ARRAY`** (§3.X invariant). The existing flush() prologue at `:1419` already saves `GL_ACTIVE_TEXTURE` and `:1420` sets unit to `GL_TEXTURE0`; the existing epilogue at `:1747` already restores active-texture. Do NOT re-save `GL_ACTIVE_TEXTURE` here — that would shadow the outer save and the inner save would always capture `GL_TEXTURE0` (because prologue already set it). Save only what the outer envelope misses:
    ```
    GLint prevSsbo4 = 0, prevTex2DArray = 0;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING,  4, &prevSsbo4);
    glGetIntegerv  (GL_TEXTURE_BINDING_2D_ARRAY,          &prevTex2DArray);
    ```
    Active texture is already `GL_TEXTURE0` at this point (set by prologue at `:1420`), so the `GL_TEXTURE_BINDING_2D_ARRAY` query reads unit 0's array binding without an explicit `glActiveTexture` call. **Slot 15 is NOT saved here** (v3.6, rationale corrected v3.7): the draw branch never binds slot 15 — only the patch compute shader (`gpu_cull_patch.comp`) reads slot 15, and `compute_dispatch()` binds AND restores it around the patch dispatch via the Step 10.3 envelope. Saving slot 15 in the draw branch would capture whatever the Step 10.3 restore set the slot to (the caller's pre-patch binding), then redundantly restore it back to the same value — a no-op pair. The spec §3.X / §9 binding-hygiene contract is honored at the Step 10.3 mutation site.
  - 11.7.b **Verify legacy prologue ran** (§3.X.1). Existing block at `:1430–1474` is NOT skipped under coalesce-armed mode. Trace by inspection in this step. Slot 2 (`s_perTypeSsbo`) bind at `:1472–1474` is inherited.
  - 11.7.c `glUseProgram(s_staticPropProgramCoalesce);`.
  - 11.7.d **Upload shared uniforms to coalesce program** (§6.X / out3-CRIT-2). Source values match the existing legacy upload at `:1447–1466`:
    ```
    if (s_locsCoalesce.terrainMVP       >= 0) glUniformMatrix4fv(s_locsCoalesce.terrainMVP,    1, GL_FALSE, gos_GetTerrainMVPMat4());
    if (s_locsCoalesce.terrainViewport  >= 0) glUniform4fv      (s_locsCoalesce.terrainViewport, 1, gos_GetTerrainViewportVec4());
    if (s_locsCoalesce.mvp              >= 0) glUniformMatrix4fv(s_locsCoalesce.mvp,           1, GL_TRUE,  gos_GetProj2ScreenMat4());
    if (s_locsCoalesce.fogValue         >= 0) glUniform1f       (s_locsCoalesce.fogValue,      1.0f);
    if (s_locsCoalesce.debugAddrMode    >= 0) glUniform1i       (s_locsCoalesce.debugAddrMode, debugAddrMode_);
    ```
    -1 locations are skipped (legacy-only uniforms removed in coalesce variant).
  - 11.7.e **Slot 1 is NOT bound by the coalesce branch** (per v2r18 §3.X.1). Verified by grep against `shaders/`: `colors_.c[...]` is unread in any live shader path; all debug modes 0–7 read `v_argb` (per-vertex lit color computed in vert shader from per-instance data, `static_prop.frag:55–80`, `static_prop.vert:249`). The legacy code at `:1557–1559` still binds slot 1 per-type but no shader actually consumes it — dead state on the legacy side as well. v2r15..v2r17 prescribed an explicit whole-buffer slot-1 bind here under the misreading that mode 4 read `colors_`; v2r18 reverts that. If a future slice reintroduces `colors_.c[...]` reads, that slice owns either converting `firstColorOffset` to an absolute ring-frame offset OR forcing the affected debug/path back to legacy.
  - 11.7.f **Bind slot 4 (PerDraw) only:**
    ```
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, s_perDrawSsbo);
    ```
    Slot 4 is consumed by the coalesce fragment shader (`PerDrawData` block per Step 8.3) and MUST be bound here. **Slot 15 is NOT rebound in the draw branch** (v3.6 cleanup): no draw-stage shader reads slot 15 (only the patch compute shader does, and Step 10.3 binds it immediately before the patch dispatch and restores it immediately after — see Step 10.3's envelope for the binding-hygiene contract). Per the "no defensive mechanisms unless live-code-required" rule, the v3.5 draw-branch rebind has been removed; v3.6's claim that "11.7.a/j covers slot 15 for downstream callers" was incorrect (that envelope only ever covered slot 4 + 2D_ARRAY) and is corrected in v3.7 by relocating the slot-15 envelope to its mutation site in `compute_dispatch()`.
  - 11.7.g **Alpha-OFF group draw** (skip if `s_alphaOffCount == 0`):
    ```
    glUniform1i(s_locsCoalesce.drawIDBase, 0);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_coalesceInstanceSsbo,
                      fr_off_bytes + 0, off_total_bytes);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                reinterpret_cast<const void*>(static_cast<uintptr_t>(0)),
                                s_alphaOffCount, 0);
    ```
  - 11.7.h **Alpha-ON group draw** (skip if `s_alphaOnCount == 0`):
    ```
    glUniform1i(s_locsCoalesce.drawIDBase, (int)s_alphaOffCount);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_coalesceInstanceSsbo,
                      fr_off_bytes + off_total_bytes, on_total_bytes);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
    const uintptr_t alphaOnOffset =
        static_cast<uintptr_t>(s_alphaOffCount) *
        static_cast<uintptr_t>(gpu_cull::kDrawElementsIndirectCommandSize);
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                reinterpret_cast<const void*>(alphaOnOffset),
                                s_alphaOnCount, 0);
    ```
    Cast pattern matches legacy at `:1704–1705`. The stride comes from the Step 1.4 header export `gpu_cull::kDrawElementsIndirectCommandSize` (= 20 bytes; the matching `static_assert(sizeof(DrawCmd) == 20)` lives at `gpu_cull_compute.cpp:541`). v3.6 replaces the v3.5 literal `20u` with the named constant so a future `DrawCmd` size change fails to link cleanly instead of silently producing the wrong byte offset.
  - 11.7.i `glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);` — restore indirect-buffer binding.
  - 11.7.j **Restore SSBO binding 4 + unit-0 `GL_TEXTURE_BINDING_2D_ARRAY`**:
    ```
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, prevSsbo4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, prevTex2DArray);
    ```
    Active texture is already `GL_TEXTURE0` (still set by prologue at `:1420`; the coalesce branch never changes it), so no explicit `glActiveTexture(GL_TEXTURE0)` call needed before the array bind. The existing flush() epilogue at `:1747` then restores active-texture to `prevActiveTex` (saved at `:1419`). Slot 1 is not bound by this branch (per 11.7.e), so no slot-1 restore is needed; the existing 0–3 save/restore envelope at `:1422–1425, 1740–1743` covers slot 1 if any prior code path had bound it. Slot 15 is not bound by this branch (per 11.7.f), so no slot-15 restore is needed either — slot-15 binding hygiene is honored at the Step 10.3 envelope in `compute_dispatch()`, which save/restores around the only site that mutates slot 15. Texture-array binding restore is required because the existing envelope at `:1421` saves only `GL_TEXTURE_BINDING_2D` for unit 0.
  - 11.7.k **Set first-flush latch** (out3-MIN-C, per-mission): if `!s_coalesceFirstFlushDone`, log `event=ready buckets_off=N buckets_on=N elapsed_us=T` and set `s_coalesceFirstFlushDone = true`.
- [ ] **11.8** When `!IsCoalesceEnabled()`: legacy per-type/per-packet loop at `:1542–1716` runs unchanged.
- [ ] **11.9** Insert coalesce fence AFTER all draws issued: `if (IsCoalesceEnabled()) s_coalesceFence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);`. Existing legacy fence at `:1726` unchanged.

---

## Step group 12 — `[COALESCE v1]` log events

Spec ref: §11 step 4 event timing table.

- [ ] **12.1** Add session helper per CLAUDE.md "Debug Instrumentation Rule": `static const bool s_coalesceTrace = (getenv("MC2_SUBSTRATE_COALESCE_TRACE") != nullptr);` plus `COALESCE_TRACE(...)` macro. Lifecycle events (`armed`/`disarmed`/`ready`) always-on; other events env-gated. Format strings exactly per §11 step 4.
- [ ] **12.2** Emit `event=armed types=N off_types=A on_types=B unique_tex_off=U unique_tex_on=V per_frame_inst_bytes=B elapsed_ms=T` at Step 5.13.
- [ ] **12.3** Emit `event=disarmed reason=...` at every disarm site: `mixed_alpha`, `size_mismatch`, `no_extension`, `env_killswitch`, `alloc_failed`, `alpha_class_drift`, `type_overflow`, `tex_evicted`.
- [ ] **12.4** `event=ready` is emitted from Step 11.7.k (per-mission gated on `s_coalesceFirstFlushDone`, reset in onMapLoad per Step 3.1).
- [ ] **12.5** **`event=permutation_state` log line (v3.6 — supports forced-disarm inspection gates).** Emitted ONCE per mission load, immediately after Step 5.4's `allocPermutationSsboAsIdentity` returns. Gated on `MC2_COALESCE_FORCE_DISARM != None` so production runs are unaffected:
  ```cpp
  if (s_coalesceForceDisarm != CoalesceForceDisarm::None) {
      uint32_t first4[4] = { 0xAAAAAAAAu, 0xAAAAAAAAu, 0xAAAAAAAAu, 0xAAAAAAAAu };
      const uint32_t want = std::min<uint32_t>(4u, (uint32_t)s_types.size());
      if (s_permutationSsbo && want > 0) {
          glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
          glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, want * sizeof(uint32_t), first4);
          glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
      }
      printf("[COALESCE v1] event=permutation_state ssbo=%u typeCount=%zu first4=%u,%u,%u,%u\n",
             s_permutationSsbo, s_types.size(), first4[0], first4[1], first4[2], first4[3]);
      fflush(stdout);
  }
  ```
  This is the readback that the §11 step 8 sub-step / step 11 inspection gates rely on. Bounded to ~16 bytes per mission load when forced; zero cost when env unset.

---

## Step group 12B — Forced-disarm test hooks (v3.6)

Spec ref: §11 step 11 (forced-disarm regression for the v2r14 blackout class). Without these hooks, Step 15.4 is "regression asserted by inspection only" — exactly the gap that produced the v2r14 blackout. v3.6 adds tiny env-gated short-circuits inside the existing failure points so each disarm path is reachable without a debugger or hand-crafted mission. **Hooks ONLY activate when `MC2_COALESCE_FORCE_DISARM` is set; default runs are byte-for-byte unchanged.**

- [ ] **12B.1** **Env variable contract.** New process-once env: `MC2_COALESCE_FORCE_DISARM`. Recognized values (string match, exact, case-sensitive): `mixed_alpha`, `size_mismatch`, `no_extension`, `alloc_failed`. Any other value (including unset) = no forced disarm. Encode as a small enum:
  ```cpp
  enum class CoalesceForceDisarm : uint8_t {
      None = 0,
      MixedAlpha,
      SizeMismatch,
      NoExtension,
      AllocFailed,
  };
  static CoalesceForceDisarm s_coalesceForceDisarm = CoalesceForceDisarm::None;
  ```
  Resolve once. Extend `coalesce_resetEnvOnce()` (Step 2.4) so the parse happens alongside `s_coalesceEnvDisabled`:
  ```cpp
  inline void coalesce_resetEnvOnce() {
      static bool s_done = false;
      if (s_done) return;
      s_coalesceEnvDisabled = (getenv("MC2_SUBSTRATE_COALESCE_LEGACY") != nullptr);
      const char* fd = getenv("MC2_COALESCE_FORCE_DISARM");
      if (fd) {
          if      (!strcmp(fd, "mixed_alpha"))   s_coalesceForceDisarm = CoalesceForceDisarm::MixedAlpha;
          else if (!strcmp(fd, "size_mismatch")) s_coalesceForceDisarm = CoalesceForceDisarm::SizeMismatch;
          else if (!strcmp(fd, "no_extension"))  s_coalesceForceDisarm = CoalesceForceDisarm::NoExtension;
          else if (!strcmp(fd, "alloc_failed"))  s_coalesceForceDisarm = CoalesceForceDisarm::AllocFailed;
          // unrecognized values silently leave None.
      }
      s_done = true;
  }
  ```
- [ ] **12B.2** **`no_extension` hook in `loadProgramsIfNeeded()` Step 7.2.** Immediately after the `glewIsSupported` probe assigns `s_hasShaderDrawParams`, insert:
  ```cpp
  if (s_coalesceForceDisarm == CoalesceForceDisarm::NoExtension) {
      s_hasShaderDrawParams = false;  // force the no-extension code path
  }
  ```
  Step 5.5's `coalesceWanted` check then naturally routes through the `event=disarmed reason=no_extension` log + identity-permutation alloc path. No additional plumbing.
- [ ] **12B.3** **`alloc_failed` hook in Step 5.9.** Immediately after the `glBufferStorage`/`glMapBufferRange` calls but before the success log, insert:
  ```cpp
  if (s_coalesceForceDisarm == CoalesceForceDisarm::AllocFailed) {
      // Simulate the late-stage alloc failure path: clean up any partial
      // resources just like the real failure would, then return.
      if (s_coalesceInstanceMap) {
          glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_coalesceInstanceSsbo);
          glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
          s_coalesceInstanceMap = nullptr;
      }
      if (s_coalesceInstanceSsbo) { glDeleteBuffers(1, &s_coalesceInstanceSsbo); s_coalesceInstanceSsbo = 0; }
      s_coalesceLayoutReady = false;
      s_coalesceEnabled     = false;
      s_coalesceArmed       = false;
      LOG("[COALESCE v1] event=disarmed reason=alloc_failed (forced)");
      return;
  }
  ```
- [ ] **12B.4** **`mixed_alpha` hook in Step 5.6.** Just before the existing alpha-class walk completes (after computing `typeHasAlpha`, before the OR-merge that sets `type.alphaClass`), inject:
  ```cpp
  if (s_coalesceForceDisarm == CoalesceForceDisarm::MixedAlpha &&
      s_types.size() > 0) {
      // Simulate one type's packets disagreeing on alpha after slot-resolution.
      LOG("[COALESCE v1] event=disarmed reason=mixed_alpha type=0 (forced)");
      s_coalesceLayoutReady = false;
      s_coalesceEnabled     = false;
      s_coalesceArmed       = false;
      return;
  }
  ```
  Place inside the per-type loop body so the function exits before reaching the buffer/texture-array build steps (preserves §5.0 invariant: legacy is already finalized; coalesce side-attempt only).
- [ ] **12B.5** **`size_mismatch` hook in Step 5.10.c.** Inside the per-group texture-array build loop body, immediately after dimension readout but before the size-assertion compare. Calls the same `coalesceRollbackTexBuild` helper that Step 5.10.c's natural failure path calls (defined in Step 5.10.c above). v3.7: the v3.6 `goto coalesceSizeMismatchRollback;` ambiguity is removed — there is exactly one rollback site, reused by both paths.
  ```cpp
  if (s_coalesceForceDisarm == CoalesceForceDisarm::SizeMismatch &&
      groupIndex == 0 /* alpha-OFF */ && uniqueIdx == 1 /* second unique */) {
      LOG("[COALESCE v1] event=disarmed reason=size_mismatch group=off "
          "expected=%dx%d got=%dx%d (forced)", W0, H0, W0+1, H0+1);
      coalesceRollbackTexBuild(newlyPinnedThisBuild);
      s_coalesceLayoutReady = false;
      s_coalesceEnabled     = false;
      s_coalesceArmed       = false;
      return;
  }
  ```
- [ ] **12B.6** **No production-path overhead.** Every hook above is a single `if (s_coalesceForceDisarm == ...)` branch on a process-static byte. When the env var is unset, all branches fall through with branch-predictor-friendly cost (well below 1 ns each). Confirm by inspection: zero hooks fire on `MC2_COALESCE_FORCE_DISARM` unset; all four fire deterministically on the matching value.
- [ ] **12B.7** **Smoke runner integration.** No code change here — the smoke harness simply runs four extra invocations of `run_smoke.py --tier tier1 --kill-existing` with the env var set to each value in turn (Step 15.4 below), then verifies the `[COALESCE v1] event=disarmed reason=<value>` log line appears AND the legacy fallback rendered the mission to completion (`tier1` success exit code).

---

## Step group 13 — Build (full relink)

- [ ] **13.1** `cmake --build build64 --config RelWithDebInfo --clean-first`. CLAUDE.md "Full relink before deploy" — `--clean-first` mandatory because Step 1.1 changes `GpuStaticPropType` (struct read from inline-eligible code paths).
- [ ] **13.2** Inspect build log for shader_builder compile errors, GL_INVALID_ENUM warnings, cross-TU symbol mismatches on new `batcher_*` exports. Final binary: `build64/RelWithDebInfo/mc2.exe`.

---

## Step group 14 — Deploy

- [ ] **14.1** `cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe`. `diff -q` no diff.
- [ ] **14.2** Same for `mc2-win64-v0.2/`.
- [ ] **14.3** Deploy modified shaders: `static_prop.vert`, `static_prop.frag`, `gpu_cull_patch.comp`. `cp -f` per file to `data/shaders/` under each install dir. `diff -q` source vs deployed.
- [ ] **14.4** Verify via `/mc2-check` skill — dry-run check that source and both deployed copies match.

---

## Step group 15 — Smoke gate

- [ ] **15.1** Kill-switch ON: `MC2_SUBSTRATE_COALESCE_LEGACY=1 MC2_GPU_CULL=1 MC2_GPU_CULL_SUBSTRATE=1 py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing`. Expected: exit 0; `event=disarmed reason=env_killswitch` once per mission.
- [ ] **15.2** Kill-switch OFF: drop `MC2_SUBSTRATE_COALESCE_LEGACY`. Expected: exit 0; one `event=armed` per mission load; one `event=ready` per mission (per-mission reset); zero `type_overflow` over 600 frames.
- [ ] **15.3** Inspect `tests/smoke/artifacts/<timestamp>/` if any failure. STOP on tier1 failure.
- [ ] **15.4** **Forced-disarm legacy fallback test** (§11 step 11, regression for v2r14 blackout — now executable via Step group 12B). Run smoke once per value:
  ```
  MC2_COALESCE_FORCE_DISARM=mixed_alpha   py -3 scripts/run_smoke.py --tier tier1 --kill-existing
  MC2_COALESCE_FORCE_DISARM=size_mismatch py -3 scripts/run_smoke.py --tier tier1 --kill-existing
  MC2_COALESCE_FORCE_DISARM=no_extension  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
  MC2_COALESCE_FORCE_DISARM=alloc_failed  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
  ```
  For each: confirm exit 0, the matching `[COALESCE v1] event=disarmed reason=<value>` line appears in the artifact log per mission, and (by inspection of any one run's logs) (a) `s_geometryFinalized == true` after finalizeGeometry returns, (b) legacy path rendered the mission to completion (smoke exit 0 implies this), (c) `s_permutationSsbo` is non-zero with identity content — verified via the `[COALESCE v1] event=permutation_state ssbo=N typeCount=N first4=0,1,2,3` log line emitted from `finalizeGeometry` per Step 12.5 (which fires under all four force-disarm modes; "first armed flush" never fires under forced disarm because coalesce never arms), (d) all three coalesce flags false.
- [ ] **15.5** Spec §11 step 5 (extension-absent compile guard): covered by `MC2_COALESCE_FORCE_DISARM=no_extension` from Step 15.4. The `s_hasShaderDrawParams=false` path is exercised structurally — `loadProgramsIfNeeded` skips the coalesce-variant compile (Step 7.5 is gated on the flag), `IsCoalesceEnabled()` returns false (Step 7.7), legacy loop runs without crash. Confirm by inspecting the run's startup log: exactly one `[COALESCE v1] event=disarmed reason=no_extension (forced)` per mission load, zero `event=armed`.

---

## Step group 16 — Tracy capture (perf gate)

Spec ref: §11 step 2 — `Render.GpuStaticProps` ≤200µs at mc2_01 normal zoom; ≥90% reduction.

- [ ] **16.1** Launch with Tracy attached. Load mc2_01 normal zoom. ≥600 steady-state frames.
- [ ] **16.2** `Render.GpuStaticProps` zone median ≤200µs. Capture trace. Investigate failures; do NOT ship.
- [ ] **16.3** §11 step 9 mission-load gate: `event=armed elapsed_ms=N` ≤ baseline + 200ms.

---

## Step group 17 — Visual canary three-way + shared-uniform coverage

- [ ] **17.1** Screenshot canary on mc2_01 + mc2_03 at fixed positions for: (a) `MC2_SUBSTRATE_COALESCE_LEGACY=1` substrate+legacy, (b) coalesce armed, (c) `MC2_GPU_CULL_SUBSTRATE=0` substrate-OFF baseline.
- [ ] **17.2** (a) vs (b): visually identical.
- [ ] **17.3** (c) vs (b): may show texture differences for multi-packet types (first-wins vs per-packet-correct). Document.
- [ ] **17.4** **Shared-uniform coverage canary** (§11 step 12). With coalesce armed on mc2_01: **first verify `[COALESCE v1] event=armed` is in the run log** (without it, the test passes-by-coincidence on the legacy fallback). Then confirm (i) static props at correct world positions (`terrainMVP` upload effective — props NOT collapsed to clip-space origin), (ii) fog blends correctly (`u_fogValue`), (iii) RAlt+9 cycles debug modes correctly on coalesce-rendered props (`u_debugAddrMode` propagated).
- [ ] **17.5** Confirm zero `event=mixed_alpha` over tier1 5/5; zero `event=size_mismatch`. Document any that fire.
- [ ] **17.6** §11 step 10 empty-group: tier1 5/5 missions all have both alpha-OFF (buildings) and alpha-ON (trees) static-prop populations, so the empty-group skip is not exercised organically. v3.6 leaves this as **inspection-only** rather than constructing a synthetic fixture: the executor confirms by reading Step 11.7.g/h that the `if (s_alphaOffCount > 0)` and `if (s_alphaOnCount > 0)` guards wrap the entire bind+draw block (per spec §3 / out-CRIT-5). Adding a synthetic empty-group fixture is deferred — Step 12B's forced-disarm hooks already exercise the more dangerous failure paths; empty-group skip is a defensive guard for a content shape that may never appear in stock data and is mechanically obvious by inspection.
- [ ] **17.7** **Single commit** covering all changes. Commit message cites spec v2r20, this plan (v3.7), Tracy delta from Step 16.2, visual canary outcome from Step 17.2/17.3/17.4.

---

## Self-review

0. **v3.7 slot-15-envelope-relocation coverage:**
   - **Slot 15 binding hygiene:** Step 10.3 wraps the patch-dispatch slot-15 bind in `glGetIntegeri_v` save → bind → dispatch → `glBindBufferBase` restore. Spec §3.X / §9 contract honored at the mutation site. The draw-branch envelope (Step 11.7.a/j) covers slot 4 + 2D_ARRAY only, never slot 15 — v3.6's claim it covered slot 15 was internally false (envelope only saved/restored slots 4 and 2D_ARRAY) and is corrected in v3.7.
   - **Step 11.7.a/f/j wording:** all three sub-steps now point to Step 10.3's envelope when explaining why the draw branch does not save/restore slot 15. No code change at any 11.7.x site.
   - **Rollback helper:** Step 5.10.c factors the texture-array failure body into `coalesceRollbackTexBuild(std::vector<DWORD>& tempPins)`. Step 12B.5's forced-`size_mismatch` hook calls the same helper. v3.6's `goto coalesceSizeMismatchRollback;` ambiguity removed; one rollback site, two callers.
   - **Step 15.4(c) verification:** re-pointed to Step 12.5's `finalizeGeometry`-time `event=permutation_state` log line (which fires under all four force-disarm modes). v3.6's "first armed flush" log line never fires under forced disarm because coalesce never arms.
0a. **v3.6 cleanup-pass coverage (carried forward):**
   - Step 11.1 anchor → "after `uploadAllBucketsIfNeeded()` returns at `:1325`" (was misleading `:1260–1263`).
   - Step 1.4 exports `gpu_cull::kDrawElementsIndirectCommandSize`; Step 11.7.h consumes it.
   - Step 2.6 factors `allocPermutationSsboAsIdentity(typeCount)` as a real static helper with `GL_STATIC_DRAW` (matches spec §9 lifecycle table); Step 5.4 calls it.
   - Step 10.3 carries an explicit "no per-frame barrier needed for permutation SSBO" note (retained alongside v3.7's save/restore envelope).
   - Step 11.7.f's defensive slot-15 rebind in the draw branch removed (rationale corrected v3.7).
   - Step group 12B adds `MC2_COALESCE_FORCE_DISARM` test hooks; Step 15.4 is now executable, Step 15.5 is covered by the same env path.
   - Front matter "Runtime disarm fallback limitation" rewritten to honest scope (all 5 indirect-command fields can be wrong, not just baseInstance) + new pre-existing `bucketCaps[]` staleness paragraph deferred to a separate slice.
   - `MC2_SUBSTRATE_COALESCE_VALIDATE` references demoted: Step 17.6 empty-group test reframed as inspection-only; spec §11 sub-steps that referenced VALIDATE are pinned to "inspection-only or future slice" pending a separate validate slice.

1. **Spec coverage (v2r18):**
   - **CRIT-1** legacy-finalize-must-complete → Steps 5.2/5.5/5.6/5.9/5.10.c (returns AFTER legacy), Step 6 verification, Step 15.4 regression test.
   - **CRIT-2** shared uniforms → Step 2.2 ProgramLocs, Step 7.4/7.6 cache for both programs, Step 11.7.d upload, Step 17.4 canary.
   - **CRIT-3** binding 15 (final after 13 → 14 → 15 flips) → Step 10.1/10.3, plan front-matter.
   - **out4-CRIT-1** slot 1 colors not in prologue (v2r18 reverted: slot 1 is declared-but-unread; coalesce branch does NOT bind it) → Step 11.7.e documents the invariant; future-shader caveat noted in §3.X.1.
   - **out4-CRIT-2** cap-semantics naming → Step 9.2/9.3 `legacyVisibleIdsCap` vs `coalesceInstanceCap`.
   - **out4-CRIT-3** loadProgramsIfNeeded latch → Step 7.2 probe + env decision INSIDE function before latch.
   - **MAJ-A** legacy prologue inheritance → Step 11.7.b verification.
   - **MAJ-B** group-relative addressing → Step 5.8 alignment asserts, Step 9.2 sorted, Step 11.7.g/h per-group bind.
   - SSBO save/restore: slot 4 + 2D_ARRAY → Step 11.7.a save, Step 11.7.j restore (draw branch); slot 15 → Step 10.3 envelope around the patch-dispatch site in `compute_dispatch()` (the only site that mutates slot 15).
   - Alignment guards → Step 5.8.
   - `coalesce_resetEnvOnce()` defined → Step 2.4 helper, Step 7.2 primary call, Step 7.7 idempotent guard.
   - Strengthened `IsCoalesceEnabled()` → Step 7.7 (handle / state / parity check predicate).
   - Per-mission ready latch → Step 3.1 reset, Step 11.7.k set.
   - Env name wired → Step 2.4 helper body.
   - `flushShadow()` forward-compat → Step 7.8.
   - `u_maxLocalVertexID` per-TYPE clarification → Step 8.5.
2. **No placeholders.** Every step has file path + function/line anchor + specific edit.
3. **Type / signature consistency.** All field names, struct fields, flag names consistent across step groups.
4. **Single-PR rule.** Step 17.7.

---

## Verification appendix (re-grepped at v3 plan-write time, 2026-05-09)

### `GameOS/gameos/gos_static_prop_batcher.h`

| Symbol | Line | Current contents |
|---|---|---|
| `STATIC_PROP_RING_FRAMES = 3u` | `:56` | constant |
| `STATIC_PROP_FLAG_ALPHA_TEST` | `:58` | `1u << 0` |
| `struct GpuStaticPropPacket` | `:38` | 6 fields |
| `struct GpuStaticPropType` | `:87–92` | 4 fields today; Step 1.1 adds 4 more |
| `void finalizeGeometry()` | `:114` | declaration |
| `batcher_getTypeCount` | `:260` | `uint32_t batcher_getTypeCount();` |
| `batcher_getTypeDrawInfo` | `:269` | 5-arg signature |

### `GameOS/gameos/gos_static_prop_batcher.cpp`

| Symbol | Line | Current contents |
|---|---|---|
| `RING_FRAMES = 3` | `:62` | constant |
| `INITIAL_INSTANCES_PER_FRAME = 4096` | `:68` | constexpr |
| `s_instanceSsbo` | `:86` | legacy ring SSBO |
| `s_fence[RING_FRAMES]` | `:90` | legacy fences |
| `s_frameSlot` | `:91` | shared frame index |
| `s_geometryFinalized` | `:165` | one-shot guard |
| `s_staticPropProgramObj` | `:171` | `glsl_program*` (legacy) |
| `s_staticPropProgram` GLuint | `:172` | legacy program handle |
| `s_programLoadTried` | `:201` | one-shot latch — **load-bearing for v2r17 §6 ordering** |
| `loadProgramsIfNeeded()` | `:192` | **modified by Step 7** — extension probe + env decision MUST land at top before this latch fires |
| `kShaderPrefix` | `:208` | Step 7.1 splits into Legacy + Coalesce |
| `glsl_program::makeProgram(...)` | `:209–213` | single legacy call; Step 7.5 adds gated coalesce call |
| Legacy uniform location caches | `:226–228` | `s_loc_u_parityWrite` etc. — parity uniforms keep separate file-scope statics; ProgramLocs is for the five shared + variant-specific only |
| `onMapLoad()` | `:490–503` | Step 3.1 extends |
| `onMapUnload()` | `:505–513` | Step 4 extends |
| `pkt.materialFlags = ...ALPHA_TEST` | `:640` | shape-level alpha at register time |
| `finalizeGeometry()` opener | `:698` | Step 5 extends |
| Legacy block runs `:707–795` | preserved verbatim per §5.0 |
| `s_geometryFinalized = true;` | `:795` | end of legacy success |
| `bool uploadAllBucketsIfNeeded()` | `:1214` | grow-on-demand legacy ring writer |
| `s_frameSlot ring advance` | `:1259` | per-frame |
| Legacy fence wait | `:1260–1263` | Step 11.1 adds parallel coalesce cleanup |
| `slotInstByteBase` | `:1266` | legacy per-frame instance base |
| `flush()` opener | `:1316` | hot-path entry |
| Top-level guard `if (!s_geometryFinalized || s_fatalRegistrationFailure) return;` | `:1320` | depends on §5.0 invariant |
| Save SSBO 0–3 | `:1422–1425` | existing envelope; Step 11.7.a/j adds slot 4 + 2D_ARRAY (slot 15 envelope lives in `compute_dispatch()` per Step 10.3, not here) |
| Restore SSBO 0–3 | `:1740–1743` | existing |
| Legacy uniform uploads | `:1447–1466` | source values for Step 11.7.d |
| Slot 2 per-type bind (PROLOGUE) | `:1472–1474` | inherited by coalesce branch |
| `useC1bIndirect = ...` | `:1519` | legacy substrate predicate |
| Legacy outer per-type loop | `:1542` | unchanged in coalesce-disarmed mode |
| Slot 0 instance per-type bind (INSIDE legacy loop) | `:1554–1556` | per-type `glBindBufferRange(... 0, s_instanceSsbo, ...)` — covered by existing 0–3 save/restore envelope |
| Slot 1 colors per-type bind (NOT prologue, INSIDE legacy loop) | `:1557–1559` | per-type `glBindBufferRange(... 1, s_colorSsbo, ...)`; coalesce branch does NOT bind slot 1 (per v2r18 §3.X.1 — slot 1 is unread by live shader) |
| `u_maxLocalVertexID` upload (per-TYPE) | `:1567` | Step 8.5 clarification anchor |
| Legacy inner per-packet loop | `:1652` | unchanged |
| Live `gosTextureHandle` re-resolve | `:1662` | basis for Step 11.4 eviction-detect |
| `effectiveMaterialFlags |= ALPHA_TEST` | `:1676–1680` | basis for §CRITICAL-C alpha walk |
| `useC1bIndirect` branch | `:1691` | legacy substrate path |
| `u_materialFlags` upload (per-PACKET) | `:1685` | Step 8.5 anchor |
| `u_packetID` upload (per-PACKET) | `:1687` | Step 8.5 anchor |
| `cmdOffset = typeID * 20` | `:1703` | legacy cmd stride |
| Legacy fence insert | `:1726` | Step 11.9 adds parallel coalesce fence |
| `flushShadow()` stub | `:1794–1796` | empty body; Step 7.8 adds forward-compat comment |

### `GameOS/gameos/gpu_cull_compute.cpp`

| Symbol | Line | Current contents |
|---|---|---|
| `BLOCK_VIS_BINDING = 13u` | `:47` | constant — basis for v2r15 13 → 14 flip |
| `s_indirectCmdBuf` | `:81` | indirect cmd buffer |
| `s_visibleIdsBuf` | `:82` | visibleIds[] SSBO |
| `compute_buildIndirectBuffer` | `:509` | Step 9 extends body |
| `struct DrawCmd` | `:534–541` | 5-uint, `static_assert(sizeof == 20)` at `:541` |
| `cumBase = 0` | `:548` | natural accumulator |
| `cmds[t].baseInstance = cumBase` | `:564` | natural per-type |
| `cumBase += instanceCap` | `:568` | natural increment — Step 9.3 renames local to `legacyVisibleIdsCap` |
| `glBufferData(s_indirectCmdBuf, ...)` | `:581` | per-mission upload |
| **Existing `INDIRECT_CMD_BINDING` bind (slot 11)** | `:828` | Step 10.3 inserts slot-15 bind between this and `:830` |
| `glUseProgram(s_patchProgram)` | `:830` | patch dispatch program bind |
| `glDispatchCompute(patchGroups, 1, 1)` | `:835` | patch dispatch — Step 10.3 ensures slot 15 is bound BEFORE this |
| **Diagnostic readback at slot 14** | `:855` | env-gated; basis for v2r16 14 → 15 flip |

### `code/mission.cpp`

| Symbol | Line | Current contents |
|---|---|---|
| `finalizeGeometry()` call | `:3088` | `GpuStaticPropBatcher::instance().finalizeGeometry();` |
| `compute_buildIndirectBuffer()` call | `:3094` | single call site |

### `shaders/`

| File:Line | Current contents |
|---|---|
| `gpu_cull.comp:81, gpu_cull_patch.comp:35` | `binding = 10` BucketCounts |
| `gpu_cull.comp:91` | `binding = 11` BucketCaps |
| `gpu_cull_patch.comp:40` | `binding = 11` IndirectCmds |
| `gpu_cull.comp:97, gpu_cull_block_rollup.comp:51` | `binding = 12` ActorVis |
| `gpu_cull_block_rollup.comp:58` | `binding = 13` BlockVis |
| `binding = 14` | (no shader uses; C++ diagnostic at `:855` reads it) |
| `binding = 15` | (verified free for permutation — no shader, no C++ outside coalesce path) |
| `static_prop.vert:35` | `uint firstColorOffset;` — currently per-type-relative; future-shader caveat per §3.X.1 |
| `static_prop.vert:55–57` | `binding = 0` Instances, `binding = 1` Colors (declared, unread by live shader body), `binding = 2` PerType |
| `static_prop.vert:103–109` | out decls (Step 8.2 adds `v_drawID`) |
| `static_prop.vert:112` | `Instance inst = instances_.i[gl_InstanceID];` (Step 8.1 wraps) |
| `static_prop.frag:27` | `flat in uint v_localVertexID;` (Step 8.3 inserts `v_drawID` after) |
| `static_prop.frag:29` | `uniform sampler2D u_tex;` (Step 8.4 wraps) |
| `static_prop.frag:30, :33, :34` | `u_materialFlags`, `u_maxLocalVertexID`, `u_packetID` (Step 8.5 branches) |
| `static_prop.frag:49` | `texture(u_tex, v_uv)` (Step 8.6 branches) |

### `mclib/txmmgr.h` / `gos_static_prop_registry.cpp` / `gos_object_parity.cpp`

| Symbol | Line | Current contents |
|---|---|---|
| `pinNode(DWORD)` | `txmmgr.h:1279` | refcount-aware pin |
| `unpinNode(DWORD)` | `txmmgr.h:1280` | refcount-aware unpin |
| `pinRefCount` | `txmmgr.h:139` | refcount field |
| Registry `pinNode` precedent | `gos_static_prop_registry.cpp:237` | refcount-aware coexistence |
| Registry `unpinNode` precedent | `gos_static_prop_registry.cpp:115` | release pattern |
| `IsParityCheckEnabled()` | `gos_object_parity.cpp:55` | function definition |

### Drift findings

- **None.** All v2r18 spec citations match HEAD as of 2026-05-09 (v2r18 changes vs v2r17 are §3.X.1 prose only — slot-1 invariant inverted; no new code paths to grep).
- Legacy `s_staticPropProgram` GLuint at `:172` and companion `s_staticPropProgramObj` at `:171` preserved (cross-TU symbol). Plan introduces only `s_staticPropProgramCoalesce` GLuint.

---

## Adversarial review handoff

Plan v3.2 against spec v2r18. Review lineage:
- v2r14 plan v1 → 4 review passes → 12 findings → spec v2r15.
- v2r15 plan v2 → 2 review passes (opus + sonnet) → 6 CRITs → spec v2r16.
- v2r16 spec → outside review → editorial-only → spec v2r17.
- v2r17 plan v3 → review pair → 6 outside findings → plan v3.1 patches.
- v3.1 plan → review pair → opus CRIT-1 invalidated patch #6 (mode 4 reads `v_argb`, not `colors_`) → spec v2r18 + plan v3.2 (this document).
- Recommended next: one more adversarial pass to confirm v3.2 against v2r18 before executor handoff.

---

## Execution Handoff

**Plan v3 saved to** `docs/superpowers/plans/2026-05-09-substrate-coalesce-implementation-plan.md`.

1. **Subagent-Driven (recommended)** — fresh subagent per step group, two-stage review between groups. REQUIRED SUB-SKILL: `superpowers:subagent-driven-development`.
2. **Inline Execution** — execute step groups in-session with checkpoints at build (Step 13) / smoke (Step 15) / Tracy (Step 16) gate. REQUIRED SUB-SKILL: `superpowers:executing-plans`.

Recommendation: subagent-driven, with adversarial-plan-review pass as a hard gate before the first executor subagent runs.
