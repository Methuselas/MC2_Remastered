# Integrated Plan v2 — gosFX Retirement + GPU Particle Pipeline + Light Dead-Code Cleanup

- **Status:** REVIEWED v2 — greybeard PASS (round 1); adversarial round 1 (2 MAJOR + 3 MINOR) folded in `1407354`; **external review round 2 (1 CRITICAL + 4 MAJOR + 2 MINOR) folded in v2 (this revision).** Pending: one more adversarial-plan-review pass to confirm execute-ready.
- **Date:** 2026-05-20 (v2 fold-in)
- **Authoring branch:** `claude/nifty-mendeleev`
- **Authoritative specs:**
  - (A) `docs/superpowers/specs/2026-05-20-gosfx-retirement-or-replacement-design.md`
  - (B) `docs/superpowers/specs/2026-05-20-gpu-particle-pipeline-design.md`
  - (C-cleanup, A5) `memory/lights_are_dead_code_use_light_appearance_undef.md`
- **F3 must-not-regress floor:** `mlr_total worst_window_p95 = 408us` (R2 confirmation `41b6853`, user-driven mc2_10, ~16% motion) — see `memory/f3_mc2_10_worstcase_2026_05_20.md`. R3 (`cf43c4f`, ~17%) = 413us; floor is robust within 1%.
- **All file:line citations re-grepped at plan-write time against `nifty-mendeleev` HEAD `d40185e`. See Appendix A for the diff vs spec citations.**
- **Stage scaffold:** A0 → A1 → A2 → A3 → B1 (Stage 0'–5' internal) → A4 → A5. Out of scope: B2 (PertCloud/ShapeCloud/DebrisCloud/EffectCloud), D (real light illumination plumbing).

## v2 fold-in summary (external review round 2)

- **CRITICAL (A4 self-contradiction)** — USER-RESOLVED to **full deletion**: editor/Viewer/aseconv targets will fail to build after A4; tracked as separate follow-on slice. Plan updated throughout §6, Appendix A negative space, Appendix B open items.
- **MAJOR-1 (B1 per-primitive enumeration infeasible)** — Producer-site repointing is now **one atomic enumerator commit** at end of Stage 2', not per-primitive. Per-primitive commits in Stage 2' wire the GPU spawn path; atomic sweep flips all 31+ callers. Soft-fail shim REJECTED per `gpu_offload_must_be_substitutive_not_additive.md`.
- **MAJOR-2 (trace oracle gosFX-coupled)** — Trace module relocated to neutral `mclib/fx_trace/` BEFORE B1 Stage 2'; env var renamed `MC2_FX_TRACE=1`. Survives A4 unchanged.
- **MAJOR-3 (CI grep gate scoping unresolved)** — Two-phase definition: B1 phase forbids only projection APIs inside `mclib/particles/`; A4 phase tightens to also forbid `theClipper`/`MLRClipper` anywhere outside (now-deleted) `mclib/gosfx/`.
- **MAJOR-4 (std430 guard too weak)** — Per-field `offsetof` + `alignof` static_asserts added to B1 Stage 1'; one-source schema generation filed as B2 polish debt.
- **MINOR-1 (A1 internal contradiction)** — A1 gate moves entirely inside `gosFX::Effect::Draw` no-op; 16 clipper assignments continue executing.
- **MINOR-2 (A5 default)** — A5 default is **no-op stub** (delete bodies, leave shells). Light type registration stays so mission .fit parsing continues.

---

## 0. Plan-write-time grep-verification deltas (CRITICAL — load-bearing)

Citations in (A) and (B) drifted between spec-write and plan-write. Re-grep is the discipline (`docs/superpowers/specs/...`'s `brainstorm_code_grounding_lesson.md`). Deltas the plan operates against:

### 0.1 (A) §1.3 line-number drift — 16-site clipper census

Spec → actual (HEAD `d40185e`):

| File | Spec lines | Actual lines |
|---|---|---|
| `mclib/bdactor.cpp` | 1544, 1588 | **1550, 1594** |
| `mclib/gvactor.cpp` | 2201, 2234, 2269, 2310 | **2207, 2240, 2275, 2316** |
| `mclib/mech3d.cpp` | 2648, 2993, 3035 | **2654, 2999, 3041** |
| `code/artlry.cpp` | 1339, 1423 | 1339, 1423 (matches) |
| `code/carnage.cpp` | 830 | 830 (matches) |
| `code/missiongui.cpp` | 2904, 2927 | 2904, 2927 (matches) |
| `code/terrobj.cpp` | 974 | 974 (matches) |
| `code/weaponbolt.cpp` | 1610 | 1610 (matches) |

Total: 16 sites in 8 files (unchanged count). The plan operationalizes A1 against a **compile-time enumerator**, not the grep list — `Effect::DrawInfo::m_clipper` is removed from the struct and the compiler enumerates broken sites (per (A) §4 ¶1 / (B) §4.14 substitutive-test discipline). The grep table is a sanity floor only.

### 0.2 (A) §1.2 `theClipper->RenderNow` line drift

Spec cites `code/gamecam.cpp:269`. Actual: **`code/gamecam.cpp:287`** (matches the (B) spec citation; (A) is stale). `StartDraw` is at `:148` (spec `:142` stale). `simplecamera.cpp:168` matches.

### 0.3 (A) §1.2 `theClipper` lifecycle — `txmmgr.cpp` has DUAL teardowns

Spec lists `txmmgr.cpp:369, :475` for deletes. Re-grep confirms:
- `mechcmd2.cpp:1647` (new), `:1940` (delete)
- `txmmgr.cpp:503` (new), `:369` (delete), `:475` (delete) — **two deletes in txmmgr, one new**

A4 deletion must hit ALL FIVE sites; the `txmmgr.cpp` double-teardown is the `mission_load_inits_mirror_init_per_subsystem.md` shape (mission re-init asymmetry) — exactly the failure-mode that has bitten three times in this codebase.

### 0.4 `gosFX::LightManager` lifecycle — same DUAL-PATH shape, SEVEN sites (not 2)

(B) §4.5 cites `mechcmd2.cpp:1676`/`:2053` (singleton plumbing). Re-grep finds **seven** lifecycle sites:
- `code/mechcmd2.cpp:1676` (new), `:2053` (delete)
- `mclib/txmmgr.cpp:530` (new), `:384` (delete), `:385` (NULL), `:490` (delete), `:491` (NULL)

A4 (or B1 if LightManager moves first per (B) §5 Q5) must hit ALL SEVEN. Mission re-init double-teardown is identical pattern to `theClipper`.

### 0.5 (B) §4.14 producer-site count is severely under (~3×)

Spec lists 11 direct `EffectLibrary::Instance->Find` sites in (B) §4.14 and self-flags MAJOR. Re-grep finds **31+ sites** across 8 files:
- `code/artlry.cpp:793, 811, 1785, 1823` (4)
- `code/carnage.cpp:913` (1)
- `code/missiongui.cpp:4357, 4405, 4426, 5838, 5863, 5884` (6)
- `code/terrobj.cpp:1237` (1)
- `code/weaponbolt.cpp:2388, 2408, 2428, 2448, 2469` (5)
- `mclib/bdactor.cpp:1255, 2415` (2)
- `mclib/gvactor.cpp:1076, 1977, 2893, 2961` (4)
- `mclib/mech3d.cpp:1472, 1493, 1514, 1532, 4729, 4747, 4855, +` (8+)

The plan therefore **mandates compile-time enumeration** for B1 spawn-site repointing (delete the old `EffectLibrary::Instance->Find` API after B1 Stage 2' moves the parser; the compiler enumerates the 31+ remaining call sites). The grep list is a floor sanity check only.

### 0.6 (B) §4.2 SSBO `binding=10` CLAIM IS WRONG — slot is taken

(B) §4.2 reserves SSBO `binding=10` for the `Particles` buffer and self-flags MAJOR "not exhaustively grep-verified." Re-grep finds:
- `shaders/gpu_cull.comp:81` — `layout(std430, binding = 10) coherent buffer BucketCounts`
- `shaders/gpu_cull_patch.comp:46` — same binding, readonly

**Action: the plan picks new binding indices in B1 Stage 1'.** Use SSBO `binding=13` (`Particles`), `binding=14` (`ParticleSpecTable`). Pre-flight grep at plan-execute time:

```
grep -rEn 'binding *= *(13|14)\b' shaders/
```

If either is taken, escalate at execute time. Lights are 20; static_prop is 0–3; gpu_cull is 10; reserve a safe window 13–14.

### 0.7 USE_LIGHT_APPEARANCE — 3 `#ifdef` sites confirmed, never defined

Re-grep `code/light.cpp:120, 142, 167`. Confirms `memory/lights_are_dead_code_use_light_appearance_undef.md`. A5 scope is exact.

---

## 1. Stage A0 — `[FX_TRACE v1]` invocation counter (neutral module)

### 1.1 Goal
Per-spec invocation counter on `Effect::Draw` (and a per-`Find()`-name spawn counter), env-gated `MC2_FX_TRACE=1`, default-off. Produces the per-mission histogram (B) §3.3 Stage 0' / Stage 3' uses for coverage equivalence.

**(v2 MAJOR-2 fold-in):** Module is **neutral** (not gosFX-coupled). Lives at `mclib/fx_trace/`, not `mclib/gosfx/`. Env var is `MC2_FX_TRACE=1`, not `MC2_FX_TRACE=1`. This is load-bearing: at B1 Stage 2', the same counters are emitted by the new `mclib::particles::Spawn` API using identical key schema. At A4, gosFX deletes but `mclib/fx_trace/` survives unchanged — no mid-arc oracle gap. The counters become the per-mission histogram for the new path too.

### 1.2 Inputs
- F3 baseline 408us memory.
- `debug_instrumentation_rule.md` macro pattern (`MC2_DEBUG_SHADOW_COLLECT` analog).

### 1.3 Files modified/created
- **NEW** `mclib/fx_trace/fx_trace.h` (macro + counter table; one-line lifecycle prints at lib-load only). Public API: `FX_TRACE_SPAWN(specName)`, `FX_TRACE_DRAW(specName)`.
- **NEW** `mclib/fx_trace/fx_trace.cpp` (counter aggregation; atexit / mission-end dump).
- `mclib/gosfx/effect.cpp` (or wherever `Effect::Draw` lives — grep at execute) — `FX_TRACE_DRAW` in `Effect::Draw`.
- `mclib/gosfx/effectlibrary.cpp` — `FX_TRACE_SPAWN` in `Find()` (or wrap `Find` call sites via macro).
- `mclib/CMakeLists.txt` — add the new `fx_trace` TU. `fx_trace` is a sibling of `gosfx/`, with NO dependency on gosFX headers (one-way: gosFX includes `fx_trace.h`, never the reverse).

### 1.4 Default state at end of stage
- `MC2_FX_TRACE` env-opt-in, OFF by default. Zero behavior change otherwise.
- Banner `[INSTR v1] enabled: ... fx_trace` follows existing pattern (CLAUDE.md "Tier-1 instrumentation env vars").

### 1.5 Verification gates
- Build: `--clean-first --config RelWithDebInfo`.
- Deploy: `cp -f` + `diff -q` (one file: `mc2.exe`).
- Smoke: `tier1 5/5 30s` env-OFF. Expect zero behavior diff.
- Smoke: one mc2_10 run with `MC2_FX_TRACE=1` env-set. Verify `[FX_TRACE v1]` lines in stderr; verify per-mission histogram dumped at mission-end.

### 1.6 Rollback
Trivial: set `MC2_FX_TRACE=0` (default). Code revert: one commit, atomic.

### 1.7 Commit shape (atomic)
- Commit 1: add `mclib/fx_trace/{fx_trace.h,fx_trace.cpp}` + CMake (compiles but inert; no caller).
- Commit 2: wire `FX_TRACE_DRAW` into `Effect::Draw` + `FX_TRACE_SPAWN` into `EffectLibrary::Find` behind env-gate. **A0 ships in 2 commits.**

### 1.8 Handoff to A1
A0 is shippable independently. A1 can begin once the counter is verified producing histograms on at least one tier1 mission.

---

## 2. Stage A1 — `MC2_DISABLE_GOSFX=1` env-gate

### 2.1 Goal
Single env-gate that short-circuits the gosFX render WORK. When ON: `Effect::Draw` early-returns no-op at its entry point. `theClipper->StartDraw` and `theClipper->RenderNow` continue running but draw zero (because no `Effect::Draw` ever enqueues into the clipper). 16 `drawInfo.m_clipper = theClipper` assignments **continue executing** (cheap pointer stores; structural integrity preserved for A4's clean cutover). Default OFF.

**(v2 MINOR-1 fold-in):** Previous wording said the clipper assignments "skipped" then "continue executing" — contradictory. The CORRECT design (and the one implemented here) is: gate fires INSIDE `gosFX::Effect::Draw` to no-op the work, NOT at the clipper-assignment site. This preserves the call-chain shape so A4's atomic deletion remains a clean cutover. Clipper state still gets set up; it just never receives effect geometry.

### 2.2 Inputs
- A0 trace counter (still active; `FX_TRACE_DRAW` expected to drop to zero per spec when A1 gates ON; `FX_TRACE_SPAWN` still nonzero — spawn still happens; only draw is gated).

### 2.3 Files modified/created
Per §0.1 compile-time enumeration discipline: **do NOT touch the 16 sites.** Instead:
- `mclib/gosfx/effect.cpp` (or where `Effect::Draw` lives — grep at execute): single env-check `if (s_disableGosFx) return;` as first statement of `Effect::Draw`.
- A1 introduces NO other gate point. `theClipper->StartDraw`, `theClipper->RenderNow`, and the 16 `m_clipper = theClipper` assignments at the producer sites all continue to execute. This is intentional per MINOR-1.

The 16 assignment sites continue executing (cheap pointer store); the no-op happens at `Effect::Draw`. **Important: gate at the leaf, not 16 different sites.** Single gate point = single rollback risk. Cull cascade (`memory/cull_gates_are_load_bearing.md`) sees identical code path on/off (no `update()` path divergence).

### 2.4 Default state
`MC2_DISABLE_GOSFX=0` (default). gosFX renders exactly as today. ON = no particles.

### 2.5 Verification gates
- Build/deploy as A0.
- Smoke `tier1 5/5 30s` env-OFF + `MC2_FX_TRACE=1` → counters match A0 baseline ±5% (cull-driven variance acceptable).
- Smoke `tier1 5/5 30s` env-ON + `MC2_FX_TRACE=1` → counters at zero for `Effect::Draw`; `Find()` counters still nonzero (spawn still happens; render is gated).
- **NO user canary at A1 (default-OFF).** A2 owns the visual-canary step.

### 2.6 Rollback
Set env to 0. Code revert: one commit.

### 2.7 Commit shape
- Commit 1: add the env-gate plumbing + early-return at leaf sites. Atomic.

### 2.8 Handoff to A2
A1 ships default-OFF; A2 is just the default flip.

---

## 3. Stage A2 — flip `MC2_DISABLE_GOSFX` default ON; gosFX no-op

### 3.1 Goal
Default-on the env-gate. Particles disappear in stock play. This is the transitional-regression state acknowledged in (A) §3 / §5 Q1. **Internal canary only — NOT shipped externally** per (B) §6 sequencing table.

### 3.2 Inputs
A1 gate landed and verified.

### 3.3 Files modified/created
- One LOC: invert the env-var-read default in the gate-plumbing header from A1.

### 3.4 Default state
**`MC2_DISABLE_GOSFX=1` default; particles gone.** `MC2_DISABLE_GOSFX=0` restores legacy gosFX path.

### 3.5 Verification gates
- Build/deploy as A0.
- Smoke `tier1 5/5 30s` (default; gate ON). Visual: no particles. Progression: 5/5 PASS.
- **USER-DRIVEN visual canary mc2_10 60s** with default ON. User observes: no in-game particle visuals (explosions, muzzle, dust, contrails, VTOL effects, recovery beam). User confirms mission completable, save/load works.
- **USER-DRIVEN heavy-combat canary (mc2_24, 60s)** with default ON. Same checks.
- **CPU_PROJ recapture (user-driven mc2_10, 60s, moderate motion ~16%, `MC2_CPU_PROJ_COST_SPLIT=1`).** Expected: `mlr_total worst_window_p95 → 0us` (gate eliminates MLR work). Compare against the 408us R2 baseline (`memory/f3_mc2_10_worstcase_2026_05_20.md`). **Gate: mlr_total must read ≤5us.** Any residual indicates the gate doesn't actually short-circuit MLR work — STOP and investigate.

### 3.6 Rollback
Set `MC2_DISABLE_GOSFX=0`. Re-runs legacy path. Or revert the one-LOC default flip.

### 3.7 Commit shape
- Commit 1: flip default. Atomic.

### 3.8 Handoff to A3
A2 ships internally. Next stage soaks the default-on state.

---

## 4. Stage A3 — soak default-on; user canary across tier1 + savegame

### 4.1 Goal
Confirm default-on doesn't regress progression / save-load / mission completion / cull lifecycle / object pools. No code change; soak only. Calendar-waived per `feedback_soak_waiver_with_probes_and_reviews_validated.md` because A2 has a parity-equivalent oracle (mlr_total → 0; A0 counter → 0).

### 4.2 Inputs
A2 default-on shipped.

### 4.3 Files modified/created
None.

### 4.4 Default state
Unchanged from A2.

### 4.5 Verification gates
- Smoke `tier1 5/5 30s` clean PASS (default).
- **USER-DRIVEN savegame canary** per `memory/mission_load_inits_mirror_init_per_subsystem.md`: load tier1 mission, play 30s, quicksave, quickload, continue 30s. No crash, no missing actor types. (gosFX is transient per (A) §1.5 — savegame should be inherently clean, but the canary discipline is mandatory.)
- **USER-DRIVEN mission-completion canary** in one tier1 mission (player-chosen). Mission must complete to victory/defeat screen.
- TGL pool trace `MC2_TGL_POOL_TRACE=1` for one tier1 run: NULL trace at zero (no pool exhaustion induced by the gate flip).

### 4.6 Rollback
Set env to 0; tracked back to A2.

### 4.7 Commit shape
No commits (soak only). State-of-soak captured in commit-message tag or a memory entry only if anomalies appear.

### 4.8 Handoff to B1
A3 PASS means gosFX is provably no-op. B1 can land the new particle path. **The 408us → 0us CPU savings is realized at A2; A3 confirms it's safe.**

---

## 5. Stage B1 — `mclib/particles/` parser + GPU billboard batcher + spawn-site repoint

B1 is the externally-visible ship event. It expands per (B) §3.3 into Stage 0'–5'. Each sub-stage carries its own verification gate. **The whole of B1 is one atomic ship event from the user's perspective: particles return.**

### 5.1 Goal
Ship GPU particle pipeline covering `CardCloud` + `PointCloud` + `ShardCloud` + `Tube` from stock `mc2.fx`. Default-on after Stage 5' flip. Defer `PertCloud` / `ShapeCloud` / `DebrisCloud` / `EffectCloud` to B2 (post-A4).

### 5.2 Inputs
- A3 PASS (gosFX provably no-op).
- F3 baseline (must-not-regress 408us; new pipeline's CPU cost must stay below this floor).
- `gpu_direct_renderer_bringup_checklist.md` (all 10 traps).
- `cpp_glsl_ubo_struct_lockstep.md` (Particle SSBO schema lockstep).
- (B) §4 negative-space, §3.4 CI grep gate, §5 resolved Qs.

### 5.3 Files modified/created (high-level; per-substage below)
**NEW directory:** `mclib/particles/`
- `mclib/particles/spec_library.{h,cpp}` — `mc2.fx` parser MOVED from `mclib/gosfx/effectlibrary.cpp` (no copy; B1 owns the file move).
- `mclib/particles/batcher.{h,cpp}` — SSBO-backed billboard batcher with post-`renderLists()` hook.
- `mclib/particles/atlas.{h,cpp}` — texture atlas build at startup.
- `mclib/particles/light_manager.{h,cpp}` — MOVED `gosFX::LightManager` (verbatim API; namespace-only change).
- `mclib/particles/spawn_api.{h,cpp}` — `gpu_fx::Spawn(specName, pos, ...)` API.
- `mclib/particles/particle_schema.h` + `shaders/include/particle_schema.hglsl` — std430-lockstep paired header (`static_assert(sizeof(GpuParticle)==64)`).
- **NEW shader pair:** `shaders/gpu_particle.vert` + `shaders/gpu_particle.frag` (one billboard shader; bucket per blend mode via uniform).
- **NEW** `scripts/check-particles-no-cpu-projection.sh` (CI grep gate per (B) §3.4).
- `code/gamecam.cpp` — new flush hook between `:270` (water fast path) and the now-A1-gated `:287` clipper site.
- `code/mechcmd2.cpp:1657, :1661, :1674, :1676, :2053` — repoint `EffectLibrary::Instance` lifecycle from `gosFX::` to `mclib::particles::SpecLibrary::Instance`; repoint `LightManager::Instance` lifecycle.
- `mclib/txmmgr.cpp:384, :385, :490, :491, :530` — same LightManager lifecycle repoint (mission re-init pair).
- 31+ producer call sites (per §0.5): repoint `gosFX::EffectLibrary::Instance->Find(...)` → `gpu_fx::Spawn(...)`. **Driven by compile-time enumeration**: delete the old `Find` API in the same commit that adds `Spawn`; compiler enumerates broken sites.

### 5.4 Sub-stages (B1 internal)

#### Stage 0' — content recon
- **Goal:** per-spec inventory of `mc2.fx` by primitive type + per-mission spawn frequency.
- **Files:** none (read-only; uses A0 traces).
- **Verification:** run `MC2_FX_TRACE=1` on tier1 5/5 with `MC2_DISABLE_GOSFX=0` (legacy ON for trace; **(MINOR-2 fold-in)** this is an opt-in env-set FOR THIS TRACE RUN ONLY — it does NOT revert the A2 default. After the trace artifact is captured, no env-set persists; A2 default-on remains in effect for all other smoke / canary runs in Stage 0'/1'/2'/3' until B1 Stage 5' flips `MC2_GPU_PARTICLES` default-on). Collect per-spec histogram. Output: coverage table named "tier1 spawn count per `mc2.fx` spec name."
- **Default state:** no code change.
- **Handoff:** coverage table committed under `docs/observations/`. Picks the v1-vs-v2 split-list authoritatively from data, not from class names.

#### Stage 1' — scaffold (the 10-trap bring-up)
- **Goal:** SSBO batcher + post-`renderLists()` hook + one hardcoded `Card` test effect. Validates the 10-trap checklist end-to-end.
- **Files:** `mclib/particles/batcher.{h,cpp}`, `mclib/particles/particle_schema.h`, `shaders/include/particle_schema.hglsl`, `shaders/gpu_particle.{vert,frag}`, `code/gamecam.cpp` (hook), `mclib/CMakeLists.txt`, `mc2/CMakeLists.txt`.
- **Trap remediation (per (B) §4.1):**
  - VAO=0 silent drop → batcher owns its own VAO; never inherits.
  - Sampler-state inheritance → `glGenSamplers` per-class; LINEAR+mipmaps+CLAMP_TO_EDGE; save/restore.
  - Depth-state → explicit `glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE)`.
  - AMD auto-LOD → `textureLod(uTex, uv, 0.0)` in FS — non-negotiable.
  - Blend state inheritance → two draws: additive (`GL_ONE,GL_ONE`) and alpha (`GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA`); explicit reset at flush start.
  - AMD attribute-0 rule (`docs/amd-driver-rules.md:6`) → VS references `gl_VertexID` or declares a dummy `layout(location=0)` attribute.
  - GL_ELEMENT_ARRAY_BUFFER per-VAO ownership.
  - texture handle resolution per-flush (not at parse time per `memory/mc2_texture_handle_is_live.md`).
  - **(MAJOR-1 fold-in)** in-front test discipline per `memory/clip_w_sign_trap.md`: VS billboard expansion must use `projectZ()` (or equivalent `pz ∈ [0,1)` check) for in-front culling, NOT `sign(clip.w)`. Stuff's matrix produces both signs for visible verts in MC2.
- **SSBO bindings (per §0.6):** `Particles` = 13, `ParticleSpecTable` = 14. **(round 1 MINOR-1 fold-in)** Pre-flight grep `grep -rEn 'binding *= *(13|14)\b' shaders/` MUST run at the Stage 1' Commit 4 boundary (hook-wired commit), not earlier. If either slot is taken by an interim landing between plan-write and execute, escalate to user and pick new slots before Commit 4 lands.
- **std430 schema (per (B) §4.3) — (v2 MAJOR-4 fold-in):** `sizeof()` alone is necessary but not sufficient. Schema lockstep requires:
  - C++ TU at `mclib/particles/particle_schema.h`:
    - `static_assert(sizeof(GpuParticle) == 64, "GpuParticle size drift")`.
    - `static_assert(alignof(GpuParticle) == 16, "GpuParticle alignment drift")` (std430 vec4 alignment requirement).
    - **Per-field `static_assert(offsetof(GpuParticle, FIELD) == EXPECTED, ...)` for EVERY field GLSL reads.** Suggested baseline (subject to schema design at execute):
      - `position` (vec3+pad) at offset 0
      - `color` (vec4) at offset 16
      - `velocity` (vec3+pad) at offset 32
      - `lifetime, age, size, atlasIndex` (4× float/uint) at offsets 48,52,56,60
    - Exact field set is fixed at Stage 1' Commit 2 (schema commit); asserts MUST be added in the same commit. Any change to the C++ struct in a later commit MUST update the asserts in the same atomic commit.
  - GLSL shader at `shaders/include/particle_schema.hglsl`:
    - Parallel `// offset N: <field>` comment block above the `struct Particle` declaration, citing each C++ offsetof for visual cross-reference. Comment block is the human-side schema doc; static_asserts are the compile-side enforcement.
  - One-source schema generation (single declarative file emitting both C++ + GLSL) is the proper long-term fix; **filed as B2 polish debt** (per task spec) rather than v1 scope.
- **CI gate (per (B) §3.4) — (v2 MAJOR-3 fold-in, B1 phase):** `scripts/check-particles-no-cpu-projection.sh` lands in this stage and is wired to the existing pre-commit hooks. **B1-phase scope (lands here):**
  ```
  FORBIDDEN_B1='cameraToClip|Camera::projectZ|worldToClipMatrix|projectForObjectAdmission|projectForEffectAdmission'
  grep -rEn "$FORBIDDEN_B1" mclib/particles/  # fail if matches inside mclib/particles/
  ```
  B1 phase forbids ONLY projection APIs inside `mclib/particles/` (the 7 wrappers + `Camera::projectZ` + `cameraToClip` + `worldToClipMatrix`). Does NOT forbid `theClipper`/`MLRClipper` patterns at this phase because gosFX/MLR are still alive in the same build and transitional B1 code may incidentally reference them. Script header comment documents the phase explicitly. A4 tightens to add `theClipper|MLRClipper` patterns repo-wide (see §6.3 / §6.7).
- **Env-gate:** `MC2_GPU_PARTICLES=1` opt-in (default OFF). With OFF, batcher does not flush.
- **Verification:** build/deploy/smoke `tier1 5/5 30s` default-OFF (zero behavior diff). Then env-ON one mc2_01 run: hardcoded test card appears in correct world position; depth-test correct; blend correct; no GL errors (`MC2_GL_DEBUG_FATAL=1`).
- **Commit shape (atomic per CLAUDE.md):** 
  - Commit 1: scaffolding compiles (new TUs + CMake; inert; no hook).
  - Commit 2: paired schema header + static_assert.
  - Commit 3: shader pair + CI gate script + register pre-commit.
  - Commit 4: hook wired; env-gate default OFF.
- **Rollback:** set `MC2_GPU_PARTICLES=0` (default); branch-revert the 4 commits if needed.

#### Stage 2' — content authority + GPU spawn wiring + atomic producer-site enumerator

**(v2 MAJOR-1 fold-in):** Round 1 plan said "delete `EffectLibrary::Find` per primitive type." That's infeasible: `Find` is one name-based surface — deleting it enumerates ALL 31+ call sites at once, not per-primitive. Soft-fail shim (record unsupported spawn) REJECTED per `gpu_offload_must_be_substitutive_not_additive.md`. **Revised approach:** the per-primitive-type progression (Card → Point → Shard → Tube) wires the GPU-side spawn/emit path per type; producer-site repointing is **one atomic enumerator commit** at the END of Stage 2'.

- **Goal:** (a) `mclib/particles/spec_library` MOVED from `mclib/gosfx/effectlibrary.cpp` (one commit). (b) Per primitive type Card → Point → Shard → Tube: implement spec → GPU-particle emission path inside `mclib/particles/`. The new `mclib::particles::Spawn(name, pos, ...)` API exists from the first per-type commit but is not yet called by producers — it's reachable only via a test harness / one canary site. (c) One atomic enumerator commit at the end: delete `gosFX::EffectLibrary::Find` API, flip ALL 31+ producer sites to `mclib::particles::Spawn` in one commit; compiler enumerates any missed site.
- **Files:** `mclib/particles/spec_library.{h,cpp}` (moved from gosfx); `mclib/particles/spawn_api.{h,cpp}` (new in this stage); per-type emit code under `mclib/particles/`; producer call sites (31+; touched once, atomically, in the enumerator commit).
- **Move discipline:** B1 owns atomic move per (B) §5 Q1. The (A)-side `mclib/gosfx/effectlibrary.cpp` is deleted in this stage (NOT A4) — because A4 deletes `mclib/gosfx/` as a tree, but the parser file moves out FIRST.
- **Per-type wiring discipline:** Each per-type commit implements the GPU spawn path for that primitive type (e.g. Card billboard SSBO push, color/blend selection, atlas-index resolution). The `Spawn` API surface stays stable; only its internal coverage grows per commit. A type that is not yet implemented returns a "spec not yet wired" diagnostic to the trace counter but DOES NOT silently no-op — this is necessary-not-sufficient enforcement before the enumerator commit lands.
- **Atomic enumerator commit (the cutover):** Lands at the END of Stage 2', AFTER all four per-type GPU-spawn paths are wired. This commit:
  1. Deletes the `gosFX::EffectLibrary::Find` symbol declaration + definition.
  2. Repoints ALL 31+ producer sites (per §0.5 census; compiler enumerates any drift since plan-write) to `mclib::particles::Spawn(name, ...)`.
  3. Wires `mclib::particles::Spawn` to emit to the SAME `fx_trace` counter as the old `Find` did (`FX_TRACE_SPAWN(name)`), preserving the per-mission histogram across the cutover (no oracle gap).
  4. Builds clean at the commit boundary. Required: `--clean-first` build verifying zero residual `EffectLibrary::Find` references.
- **LightManager move:** also moves in Stage 2' (verbatim API; `mechcmd2.cpp` + `txmmgr.cpp` lifecycle repoint per §0.4). Producer side only; SSBO consumer unchanged. Separate commit from the per-type wiring and from the enumerator commit.
- **Env-gate:** `MC2_GPU_PARTICLES=1` (still opt-in). `MC2_DISABLE_GOSFX=1` (still A2 default; legacy still gated off). After the enumerator commit, producers no longer call `gosFX::EffectLibrary::Find` at all; `MC2_DISABLE_GOSFX` continues to gate the (now-empty-of-new-effects) gosFX `Effect::Draw` path. Existing in-flight legacy effect instances drain via the gated `Effect::Draw`.
- **Verification (per primitive type wiring commit):**
  - Build/deploy.
  - Smoke `tier1 5/5 30s` with `MC2_GPU_PARTICLES=1 MC2_FX_TRACE=1 MC2_DISABLE_GOSFX=1`. Default A2 state: producers still call old `Find` (until the enumerator commit lands), so legacy `FX_TRACE_SPAWN` counters stay populated; new `Spawn` is reachable only via canary harness — verify no regression.
  - **NO user-driven canary per per-type wiring commit** (too granular). Mass canary at Stage 4'.
- **Verification (atomic enumerator commit):**
  - Build `--clean-first` MUST succeed with zero `EffectLibrary::Find` references — proves enumeration is complete.
  - Smoke `tier1 5/5 30s` with `MC2_GPU_PARTICLES=1 MC2_FX_TRACE=1 MC2_DISABLE_GOSFX=1`. `FX_TRACE_SPAWN` counters now sourced from `mclib::particles::Spawn` — per-spec histogram should match Stage 0' coverage table for the four implemented types within ±10% (parity gate). Out-of-scope types (Pert/Shape/Debris/EffectCloud) show zero spawns post-cutover (filed as B2 debt).
  - **NO user-driven canary at the enumerator commit itself** (deferred to Stage 4').
- **Commit shape:**
  - Commit 1: file move (mechanical, namespace-only) of `effectlibrary.cpp` → `mclib/particles/spec_library.{h,cpp}`. Build/test parity.
  - Commit 2: LightManager move (verbatim API; lifecycle repoint per §0.4).
  - Commit 3–6: per-type GPU-spawn wiring (Card, Point, Shard, Tube — 4 commits). Each adds emit-path coverage; `Spawn` API gains type without producer migration.
  - Commit 7: **atomic enumerator cutover** — delete `EffectLibrary::Find`, repoint all 31+ sites to `mclib::particles::Spawn`, wire `FX_TRACE_SPAWN`. ONE commit. Build clean at boundary.
  - **7 atomic commits total in Stage 2'.**
- **Rollback per per-type commit:** branch-revert that commit; `Spawn` loses that type but producer migration hasn't happened yet so no caller breaks.
- **Rollback of the enumerator commit:** branch-revert restores `EffectLibrary::Find` and all 31+ caller call-site lines; per-type GPU-spawn wiring remains. This is a single-commit rollback at the highest-risk boundary in B1 — explicit design property.

#### Stage 3' — coverage gates
- **Goal:** machine-checked equivalence for each shipped primitive type.
- **Files:** none (uses A0 traces + Stage 0' coverage table).
- **Verification:** tier1 5/5 with `MC2_GPU_PARTICLES=1 MC2_FX_TRACE=1`. For each `Find()`-name in the coverage table whose primitive type is in v1 scope, require gpu-side spawn-event count within ±10% of legacy baseline (Stage 0' capture). Out-of-scope types (Pert/Shape/Debris/EffectCloud) report zero spawns and are filed as B2 debt.
- **No commits.** Soak-style stage.

#### Stage 4' — visual canary soak + ADVERSARIAL REVIEW
- **Goal:** user-driven visual sign-off across full tier1 + mc2_10 + one heavy-combat mission. Mandatory adversarial review of the integrated B1 plan-state per (B) §3.3 Stage 4'.
- **Heavy-combat mission pick (per (B) §5.1 deferred decision):** pick mc2_17 vs mc2_24 based on per-mission spawn count from Stage 0' coverage table. **Default pick: mc2_24** (combat-heaviest per `memory/feedback_offload_scope_stock_only.md` stock-mission canon); confirm at execute time.
- **Verification:**
  - `tier1 5/5 30s` with `MC2_GPU_PARTICLES=1` (still opt-in).
  - **USER-DRIVEN mc2_10 60s** with `MC2_GPU_PARTICLES=1`. User observes per-spec visuals: `Mech_Explosion`, `Gauss_flare`, `LRM_Smoke`, `VTOL_Effect`, `Recovery_Effect`, `Damaged_fire`, `Critical_hit`, tube contrails. Subjective accept per spec.
  - **USER-DRIVEN mc2_24 (or mc2_17) 60s** with `MC2_GPU_PARTICLES=1`. Same subjective per-spec accept.
  - **SAVEGAME canary** per (B) §4.11 + EVENING handoff lesson: quicksave / quickload mid-mission with particles active.
  - **CPU_PROJ recapture (user-driven mc2_10, ~16% motion, `MC2_GPU_PARTICLES=1 MC2_CPU_PROJ_COST_SPLIT=1`).** Compare against A2's 0us mlr_total: new `gpu_fx_update` Tracy bucket appears as additive cost (per (B) §4.13). **GATE: total CPU projection (sum of all buckets) must not exceed 408us** (the (A)-deletion floor must not be regressed by B1). Per (B) §6 closing paragraph: expected `gpu_fx_update < 50us`; expected net `≈ -350us` vs F3 baseline.
  - **CPU_PROJ recapture (user-driven mc2_24, sustained-combat ~20%+ motion).** Per F3 memory: "Sustained-combat captures may push higher." This is the *stress-test* sample. Document the worst-window number. If `gpu_fx_update worst_window_p95 ≥ 200us`, escalate as a budget concern (still below the 408us floor, but a signal that v1's CPU dispatch cost wasn't sized right).
  - **MANDATORY adversarial-plan-review skill dispatch** against the current B1 plan-state — file findings; iterate Stage 1'–3' or escalate if CRITICAL.
- **Commit shape:** no code commits (soak stage). Findings folded into the plan doc (a fold-in commit).

#### Stage 5' — flip `MC2_GPU_PARTICLES` default ON; file B2 debt
- **Goal:** ship particles externally.
- **Files:** one-LOC default flip; new memory entry for B2 (Pert/Shape/Debris/EffectCloud) per (B) §1.4 ¶3 with enumerated `mc2.fx` spec names from Stage 0' coverage table.
- **Verification:** `tier1 5/5 30s` default. **USER-DRIVEN visual canary mc2_10 60s + mc2_24 60s default-ON.** Both pass = B1 ships.
- **Commit shape:** 1 commit for default flip; 1 commit for B2 debt-file (or just a memory entry).
- **Rollback:** revert the default-flip commit (set back to opt-in).

### 5.5 Rollback (whole-B1)
- Per-sub-stage: branch-revert.
- Whole-B1: B1 is one logical ship event. If Stage 4'/5' surface a blocker, branch-revert all B1 commits back to A3 state (gosFX no-op default; no particles). A4 then waits until B1 reships.

### 5.6 Handoff to A4
B1 Stage 5' flipped default-ON and user-soaked. A4 begins.

---

## 6. Stage A4 — atomic FULL deletion of `mclib/gosfx/` + `mclib/mlr/` trees

### 6.1 Goal
**(v2 CRITICAL fold-in, USER-RESOLVED):** **Full deletion.** Delete `mclib/gosfx/` tree (minus the parser already moved by B1 Stage 2') and `mclib/mlr/` tree entirely from the repository. Remove from `mc2.exe` build AND from editor / Viewer / `aseconv` targets. **editor/Viewer/aseconv targets WILL FAIL TO BUILD after A4 ships. This is intentional and accepted debt;** tracked in a follow-on slice spec to be filed at A4 ship time. Per `feedback_editor_must_converge_with_runtime_paths.md`, eventual editor convergence is a separate worktree's responsibility (the editor's forked render loop ends at `renderLists()` anyway, so its current dependency on gosFX/MLR is itself legacy debt). Round 1's "kept linkable" guarantee is RETRACTED in v2.

### 6.2 Inputs
- B1 Stage 5' shipped default-on and soaked.
- (A) Stage 3 staging guidance.
- User decision (binding): editor breakage is accepted; file follow-on slice.

### 6.3 Files modified/created
- **DELETE:** all `.cpp/.hpp` under `mclib/gosfx/` EXCEPT the parser already moved to `mclib/particles/` in B1 Stage 2'.
- **DELETE:** all `.cpp/.hpp` under `mclib/mlr/`.
- **MODIFY:** `mclib/CMakeLists.txt` + top-level CMake — remove gosFX/MLR from `mc2.exe` link list AND from editor / Viewer / `aseconv` target link lists. After this commit, those targets reference nonexistent symbols and FAIL TO BUILD; this is documented in the CMake comment alongside the change.
- **DELETE:** `theClipper` declaration; the 16 `drawInfo.m_clipper = theClipper` sites (now dead because `m_clipper` field of `Effect::DrawInfo` is also deleted — compile-time enumeration).
- **DELETE:** `code/mechcmd2.cpp:1647` (new), `:1940` (delete) — `theClipper` lifecycle.
- **DELETE:** `mclib/txmmgr.cpp:369, :475, :503` — `theClipper` lifecycle (the double-teardown per §0.3).
- **DELETE:** `code/gamecam.cpp:148, :287`; `code/simplecamera.cpp:168` — `StartDraw` / `RenderNow`.
- **DELETE:** `MC2_DISABLE_GOSFX` env-gate plumbing from A1/A2 (dead).
- **KEEP:** `MC2_FX_TRACE` env-gate + `mclib/fx_trace/` module survive A4 unchanged (neutral oracle per §1, MAJOR-2 fold-in). `FX_TRACE_DRAW` callers in deleted `Effect::Draw` go away naturally; `FX_TRACE_SPAWN` callers in new `mclib::particles::Spawn` (from B1 Stage 2' enumerator commit) continue to emit.
- **MODIFY:** `scripts/check-particles-no-cpu-projection.sh` — **(v2 MAJOR-3 fold-in, A4 phase)** tighten gate from B1-phase scope to A4-phase scope:
  ```
  FORBIDDEN_A4='cameraToClip|Camera::projectZ|worldToClipMatrix|projectForObjectAdmission|projectForEffectAdmission|theClipper|MLRClipper'
  grep -rEn "$FORBIDDEN_A4" mclib/particles/  # fail if matches inside mclib/particles/ (unchanged)
  grep -rEn 'theClipper|MLRClipper' .          # fail if matches ANYWHERE in repo (NEW in A4)
  ```
  The A4 tightening catches accidental re-introduction of clipper / MLR references anywhere in the tree after the trees are gone. Tightening lands in the same atomic bundle as the deletions (see §6.7 Commit 5).

### 6.4 Default state
gosFX + MLR code physically removed from runtime build. Particles continue rendering via the B1 GPU path. `mc2.fx` stays on disk (per (A) §4 ¶7) — the spec library B1 needs.

### 6.5 Verification gates
- Build: `--clean-first --config RelWithDebInfo`. Confirm `mc2.exe` target links cleanly without gosFX/MLR symbols.
- **(v2 CRITICAL fold-in, supersedes round 1 MINOR-3):** Editor / Viewer / `aseconv` targets are EXPECTED to fail to build after A4. **Action at A4 gate:** explicitly build `mc2.exe` ONLY (not the `ALL_BUILD` target). If using `cmake --build . --config RelWithDebInfo --target mc2`, confirm success. Editor target failure is NOT a regression — it is the documented intentional debt. File follow-on slice spec at A4 ship time per task: `docs/superpowers/specs/<date>-editor-viewer-aseconv-gosfx-retirement-followon.md` (stub; details to come from a separate editor-convergence worktree).
- CI grep gate: run `sh scripts/check-particles-no-cpu-projection.sh` after the tightening commit lands; verify it now fires on any residual `theClipper|MLRClipper` reference anywhere in the tree (expected: zero matches post-deletion).
- Deploy.
- Smoke `tier1 5/5 30s`. Expected: byte-identical (visually) to B1 Stage 5' end state — A4 is **dormant-code deletion**. **No perf delta expected** (the 408us savings was realized at A2/A3; B1's additive cost was measured at Stage 4').
- **CPU_PROJ recapture optional / sanity-only** (per task spec). If captured, expect identical numbers to Stage 4'.
- **MANDATORY adversarial-plan-review** per CLAUDE.md "high-stakes plan (architectural endpoints, legacy retirement)". A4 deletes ~19 .cpp/hpp pairs across two trees — exactly the high-blast-radius shape that requires the skill.

### 6.6 Rollback
- Revert the A4 deletion commits. gosFX + MLR + `theClipper` come back; B1's particle path keeps working in parallel (B1 doesn't depend on gosFX being present, only being absent of CPU-projection coupling).
- Risk window: small. Per (A) §3.2, deletion is layer-clean (gosFX → MLR → nothing else in runtime exe).

### 6.7 Commit shape
**(round 1 MAJOR-2 fold-in)** Ordering: the callers-deletion commit MUST precede the `mclib/gosfx/` tree deletion, otherwise build breaks at the tree-deletion commit because callers still reference `Effect::DrawInfo::m_clipper`. **(v2 CRITICAL + MAJOR-2 + MAJOR-3 fold-ins)** Commit 2 now removes gosFX/MLR from editor/Viewer/aseconv targets too (full deletion); `MC2_FX_TRACE` is KEPT (neutral oracle); CI gate tightens in a dedicated commit. Revised order:

- Commit 1: delete the 16 `drawInfo.m_clipper = theClipper` assignments + delete the `Effect::DrawInfo::m_clipper` field declaration + delete `theClipper` global declaration + delete `theClipper` lifecycle sites (`mechcmd2.cpp:1647, :1940`; `txmmgr.cpp:369, :475, :503`) + delete `code/gamecam.cpp:148, :287` + delete `code/simplecamera.cpp:168`. **All-or-nothing**: this is the compile-time-enumerator commit; nothing else in this commit. After this lands, `mclib/gosfx/` and `mclib/mlr/` are unreferenced from `mc2.exe` link graph but still in the tree. Editor / Viewer / `aseconv` targets DO still reference these trees at this commit — they will fail to build (intentional; Commit 2 makes it explicit at CMake level).
- Commit 2: CMake scope change — gosFX/MLR removed from `mc2.exe` link list AND from editor / Viewer / `aseconv` target link lists. **Those targets fail to build after this commit; this is the documented intentional debt per §6.1 / §6.5.** Build `mc2.exe` target only at this and subsequent commit boundaries.
- Commit 3: delete `mclib/gosfx/` tree (minus the parser moved to `mclib/particles/` in B1 Stage 2'). `mc2.exe` builds clean.
- Commit 4: delete `mclib/mlr/` tree. `mc2.exe` builds clean.
- Commit 5: CI grep gate tightening — modify `scripts/check-particles-no-cpu-projection.sh` per §6.3 to add the repo-wide `theClipper|MLRClipper` forbidance. Run the script post-edit; verify zero matches.
- Commit 6: delete `MC2_DISABLE_GOSFX` env-gate plumbing (now dead). `MC2_FX_TRACE` and `mclib/fx_trace/` SURVIVE — neutral oracle per §1 (v2 MAJOR-2). `FX_TRACE_DRAW` callers go away naturally with the tree deletions; `FX_TRACE_SPAWN` callers in `mclib::particles::Spawn` continue to emit.
- Commit 7 (paired with A4 ship): file editor/Viewer/aseconv follow-on slice spec stub at `docs/superpowers/specs/<date>-editor-viewer-aseconv-gosfx-retirement-followon.md`. Lists broken targets, points at `feedback_editor_must_converge_with_runtime_paths.md`, defers solution to separate worktree.
- **7 atomic commits, REVISED ORDER.** `mc2.exe` build must succeed at every commit boundary 1–6; verify via `--clean-first --target mc2` build after each. Editor target build NOT verified post-Commit 2 (intentional).

### 6.8 Handoff to A5
A4 PASS = gosFX + MLR gone from runtime exe.

---

## 7. Stage A5 — Light dead-code retirement

### 7.1 Goal
**(v2 MINOR-2 fold-in):** **A5 default is no-op stub.** Light GameObject type registration STAYS so mission `.fit` Light entries continue to populate the ghost pool. `Light::init` / `update` / `render` bodies become no-op stubs (delete the `#ifdef`'d-out code, leave the function shells). A5 does NOT delete the type registration — that would require a separate loader-side proof that's not in A5 scope and would risk mission .fit parse failures across the stock missions.

Concretely: delete the `USE_LIGHT_APPEARANCE`-gated bodies in `code/light.cpp` (3 sites per §0.7); leave the surrounding function shells (now empty bodies) intact; leave the Light class declaration and GameObject type registration intact; mission `.fit` Light parsing continues to populate ghost-pool entries that do nothing at update/render time. **No loader change required.**

### 7.2 Inputs
- `memory/lights_are_dead_code_use_light_appearance_undef.md`.
- A4 PASS (no dependency, but conservative ordering).

### 7.3 Files modified/created
- `code/light.cpp` — delete the 3 `#ifdef USE_LIGHT_APPEARANCE`/`#endif` blocks (Light::init's VFXAppearance allocation, Light::update's `onScreen()` call, Light::render). Function shells (`void Light::init(...) { }`, etc.) remain so the v-table and type registration stay valid.
- `code/light.h` — **KEEP the Light class declaration intact.** Do NOT delete the class — mission `.fit` Light type registration depends on it. Compile-time enumeration of callers is NOT performed in A5; deletion of the class is explicitly out of scope per v2 MINOR-2.
- Mission `.fit` Light loader — **NO CHANGE.** Light type registration stays; loader continues to instantiate Light ghosts.

### 7.4 Default state
Light GameObject continues to exist as a no-op stub. Mission `.fit` Light entries continue to ghost-allocate with zero behavior at update/render. **No visual delta** (the code was already dead per `lights_are_dead_code_use_light_appearance_undef.md`); **no loader delta** (type registration unchanged).

### 7.5 Verification gates
- Build/deploy.
- Smoke `tier1 5/5 30s`. Expected byte-identical to A4 (paths were dead).
- **USER-DRIVEN mission-load canary one tier1 mission**: confirm mission loads, no crash on `.fit` parse, no `[LIGHT_PARSE]` errors.
- **CPU_PROJ recapture optional / sanity-only** per task spec. Ghosts cost ~0us per `lights_are_dead_code_use_light_appearance_undef.md`; no signature.
- Adversarial review NOT mandatory (low blast radius; well-characterized as dead code in memory). Optional greybeard pass for sanity.

### 7.6 Rollback
Revert the A5 commits. Light ghosts return; zero behavior diff.

### 7.7 Commit shape
- Commit 1: delete the 3 `#ifdef` blocks in `code/light.cpp`. Function shells remain.
- **1 atomic commit (v2 MINOR-2 reduction).** No second commit: Light type registration stays per v2 MINOR-2; loader unchanged.

### 7.8 R5 ordering trap is NOT in scope for A5
Per task spec + `memory/lights_are_dead_code_use_light_appearance_undef.md`: the `mclib/bdactor.cpp:2180-2183` "Stage 2.D.2 fix" ordering trap belongs to the FUTURE (D) arc (real light illumination plumbing — `docs/superpowers/specs/2026-05-20-light-real-illumination-plumbing-design.md`). **Do not touch `bdactor.cpp:2180-2183` in A5.**

### 7.9 Handoff
A5 PASS = integrated arc complete. File B2 (PertCloud/ShapeCloud/DebrisCloud/EffectCloud) as named debt with `mc2.fx` spec names enumerated from Stage 0' coverage. File (D) light-illumination as future fidelity arc.

---

## 8. Cross-cutting discipline

### 8.1 CI grep gate (per (B) §3.4) — TWO-PHASE per v2 MAJOR-3 fold-in
`scripts/check-particles-no-cpu-projection.sh` lands in B1 Stage 1' (B1-phase scope) and tightens in A4 (A4-phase scope). Wires into the existing pre-commit hooks (`scripts/check-destroy-invariant.sh`, `scripts/check-asset-scale-callers.sh`, `scripts/check-render-contract-gbuffer1.sh`).

**B1-phase scope (lands at B1 Stage 1' Commit 3):**
```
FORBIDDEN_B1='cameraToClip|Camera::projectZ|worldToClipMatrix|projectForObjectAdmission|projectForEffectAdmission'
grep -rEn "$FORBIDDEN_B1" mclib/particles/  # fail if matches inside mclib/particles/
```
Forbids ONLY projection APIs inside `mclib/particles/`. Does NOT forbid `theClipper`/`MLRClipper` because gosFX/MLR are still alive in the same build during B1; transitional code may reference them. Script header comment cites the phase explicitly.

**A4-phase scope (tightening lands at A4 Commit 5):**
```
FORBIDDEN_A4='cameraToClip|Camera::projectZ|worldToClipMatrix|projectForObjectAdmission|projectForEffectAdmission|theClipper|MLRClipper'
grep -rEn "$FORBIDDEN_A4" mclib/particles/  # particles-scope clipper ban
grep -rEn 'theClipper|MLRClipper' .          # repo-wide clipper ban
```
Repo-wide forbidance catches accidental re-introduction post-deletion. Pre-deletion this would false-positive against `gosfx/`/`mlr/` themselves; post-deletion both trees are gone so the gate is meaningful.

Both phases run via the same script file; the A4 commit edits the script body. No CI infrastructure rewiring required.

### 8.2 Pre-commit invariants (existing + new)
- `sh scripts/check-destroy-invariant.sh` — must run if A4/A5 touch object lifecycle.
- `sh scripts/check-asset-scale-callers.sh` — n/a unless `code/mechicon.cpp` touched.
- `sh scripts/check-render-contract-gbuffer1.sh` — must run if B1 Stage 1' shader pair touches the gbuffer1 channel.
- `sh scripts/check-particles-no-cpu-projection.sh` — NEW from B1 Stage 1'.
- `sh scripts/check-claude-md-pointer.sh` — must run if root CLAUDE.md touched (should not happen).

### 8.3 No-emoji, no wall-clock (CLAUDE.md "Critical inline rules")
- This plan contains no pictographic emoji.
- This plan contains no wall-clock time projections; named handoffs are the only ordering mechanism.

### 8.4 Build / deploy discipline
- Every build: `--clean-first --config RelWithDebInfo`. CLAUDE.md "Build" rule + `feedback_class_layout_change_needs_clean_first.md`.
- Every deploy: `cp -f` per file + `diff -q`. NEVER `cp -r` per CLAUDE.md "Deploy."
- Every shader edit: deploy the shader tree in the SAME pass as the exe per `memory/shader_exe_deploy_lockstep.md`.

### 8.5 Vulkan-prep discipline
All B1 GPU resources use explicit device-mediated binding (`device.bindVertexBuffer(vb)`, NOT `vb.bind()`) per CLAUDE.md "Vulkan-prep." Assume zero implicit cross-call GL state. Enqueue/flush pattern matches `GpuStaticPropBatcher` precedent.

### 8.6 Greybeard rulings
- **A0/A1/A2/A3**: PATCH (justified). The meta-fix is A4 (atomic deletion); A0–A3 are reversibility plumbing — the staged-gate approach is the documented soak-waiver pattern for catastrophic-axis retirements (`feedback_soak_waiver_with_probes_and_reviews_validated.md`).
- **B1**: PATCH (justified). The meta-fix is (A) itself; B1 is capability-restoration filed against (A)'s deliberate transitional regression. Greybeard guardrail = CI grep gate (§8.1). Per (B) §3.1 ¶5.
- **A4**: META-FIX. This is the actual upstream change that retires the bug class (`mlrclipper.cpp:206` CPU read of `cameraToClip(2,2)`). Per (A) §3.1.
- **A5**: META-FIX (small-scope). Retires the dead-code-occupying-pool-slot anti-pattern. Per `lights_are_dead_code_use_light_appearance_undef.md`.

---

## 9. Perf gate summary (load-bearing)

| Handoff | Capture | Mission | Expected | Gate |
|---|---|---|---|---|
| After A2 default-on | CPU_PROJ recapture user-driven mc2_10 60s ~16% motion | mc2_10 | `mlr_total worst_window_p95 → 0us` | **≤ 5us required.** Any residual = gate doesn't work; STOP. |
| After B1 Stage 4' | CPU_PROJ recapture user-driven mc2_10 60s ~16% motion | mc2_10 | `mlr_total = 0`, new `gpu_fx_update worst_window_p95 < 50us` | **Total projection ≤ 408us required** (must not regress past F3 floor). |
| After B1 Stage 4' (stress) | CPU_PROJ recapture user-driven mc2_24 60s sustained combat | mc2_24 | `gpu_fx_update worst_window_p95` documented | If ≥ 200us, escalate. |
| After A4 | Optional / sanity only | mc2_10 | identical to Stage 4' end-state | No-op gate. |
| After A5 | Optional / sanity only | any tier1 | identical to A4 end-state | No-op gate. |

**Net frame-budget delta after the whole arc:** approximately `-408us + gpu_fx_update_cost` (expected `≈ -350us`).

---

## 10. User involvement boundaries

The executor stops and hands off to user at these named points:

1. **A2 visual canary (mc2_10 60s default-ON)** — user confirms no-particle transitional state is visually acceptable as internal canary.
2. **A2 heavy-combat canary (mc2_24 60s default-ON)** — same.
3. **A2 CPU_PROJ recapture (mc2_10)** — user drives the camera ~16% motion; executor reads the artifact.
4. **A3 savegame canary** — user drives quicksave/quickload cycle.
5. **A3 mission-completion canary** — user plays one mission to completion.
6. **B1 Stage 4' mc2_10 visual canary** — user confirms per-spec visuals (`Mech_Explosion`, `Gauss_flare`, `LRM_Smoke`, `VTOL_Effect`, `Recovery_Effect`, `Damaged_fire`, `Critical_hit`, contrails).
7. **B1 Stage 4' mc2_24 visual canary** — user same accept set for heavy combat.
8. **B1 Stage 4' savegame canary** — user.
9. **B1 Stage 4' CPU_PROJ recaptures (mc2_10 + mc2_24)** — user drives motion.
10. **B1 Stage 5' default-ON ship canary (mc2_10 + mc2_24)** — user.
11. **A5 mission-load canary** — user.

Per CLAUDE.md "Smoke sessions are USER-DRIVEN": agent does NOT ask user to re-run with X env var or reproduce manually outside the smoke command. Executor invokes `run_smoke.py` with the right env vars; user observes live.

---

## 11. Stop-and-report conditions (executor)

Per task spec:
- (a) Spec set internal contradictions blocking planning → STOP.
- (b) F3 baseline numbers contradict spec assertions → STOP.
- (c) Greybeard or adversarial review surfaces CRITICAL finding requiring re-design → STOP.

Plan-write-time observation: none of (a)/(b)/(c) fired. The §0 grep deltas are MAJOR-class (correctable in-plan), not CRITICAL.

---

## Appendix A — write-time grep verification log

All citations re-grepped against `nifty-mendeleev` HEAD `d40185e` on 2026-05-20. Deltas vs spec citations captured in §0. The plan operates against the re-grepped lines, not the spec lines.

| Citation | Status |
|---|---|
| `code/gamecam.cpp:148` (StartDraw) | confirmed |
| `code/gamecam.cpp:287` (RenderNow) | confirmed (spec (A) drift `:269`) |
| `code/simplecamera.cpp:168` (StartDraw) | confirmed |
| `code/mechcmd2.cpp:1647, :1940` (theClipper lifecycle) | confirmed |
| `mclib/txmmgr.cpp:369, :475, :503` (theClipper lifecycle) | confirmed; double-teardown per §0.3 |
| 16 `drawInfo.m_clipper = theClipper` sites | confirmed; lines drifted per §0.1 |
| `code/mechcmd2.cpp:1676, :2053` (LightManager) | confirmed |
| `mclib/txmmgr.cpp:384, :385, :490, :491, :530` (LightManager) | confirmed; §0.4 expansion to 7 sites |
| 31+ `gosFX::EffectLibrary::Instance->Find` callers | confirmed; §0.5 (spec (B) §4.14 listed 11; ~3× under) |
| `code/light.cpp:120, :142, :167` (USE_LIGHT_APPEARANCE) | confirmed; never defined |
| `shaders/gpu_cull.comp:81, gpu_cull_patch.comp:46` (SSBO binding=10 IN USE) | confirmed; §0.6 reassigns to binding 13/14 |

## Appendix C — review verdicts (folded in this commit)

### Greybeard ruling (per `.claude/skills/greybeard.md`)

Inline pass against the integrated plan; the skill file is not registered as a callable Skill in this harness, so the protocol was executed inline.

1. **Subsystem pin.** (A) `mclib/gosfx/` MLR-consumer + `theClipper` global; (B) new `mclib/particles/` batcher + parser-move; A5 `code/light.cpp` `#ifdef`-gated dead code. Pinned with re-grepped first-hand evidence in §0.
2. **Symptom vs cause.** Symptom: 408us/frame CPU projection in MLR clip math. Cause: gosFX is the only live MLR consumer and reads `cameraToClip(2,2)` CPU-side.
3. **Meta-fix.** A4 atomic deletion of `mclib/gosfx/` + `mclib/mlr/` from runtime. A5 deletes the dead `Light` ghost.
4. **Substitutive test.** B1 Stage 2' deletes old `Find` API in same commit that adds `Spawn` (compile-time enumerator over §0.5's 31+ producer sites). A4 deletes producer (`Effect::DrawInfo::m_clipper` field) and consumer (`mclib/mlr/`) atomically.
5. **Verdict per stage:** A0/A1/A2/A3 = `PATCH (justified)` (soak-waiver scaffolding for catastrophic-axis retirement; meta-fix is A4 itself; named & deferral-justified per `feedback_soak_waiver_with_probes_and_reviews_validated.md`). B1 = `PATCH (justified)` (capability-restoration under CI grep gate guardrail per (B) §3.1 ¶5). **A4 = `META-FIX`.** **A5 = `META-FIX` (small scope).**

**Greybeard verdict: PASS.** No revisions required.

### Adversarial-plan-review (per `.claude/skills/adversarial-plan-review.md`)

Inline pass; skill not registered as callable Skill in this harness. Process executed:
- Step 2: every cited symbol re-grepped at write time; deltas in §0 / Appendix A.
- Step 3 (interacts-with): hook at `gamecam.cpp:270` post-water (precedent); A1 leaf-gate at `Effect::Draw`; A4 field-deletion → compile-time enumeration.
- Step 4 (mechanical feasibility): §0.5 census confirms compile-time enumeration is the only sane approach.
- Step 5 (perf claims): §9 explicit gates against 408us floor.
- Step 6 (load-bearing constraints): see findings.
- Step 7 (per-mission lifecycle): §0.3 / §0.4 / §6 address `theClipper` and `LightManager` mission re-init double-paths.
- Step 8 (partial-landing hazard): see findings.
- Step 9 (global-convention census): N/A — B1 does not flip a cross-cutting convention (no depth-func / blend / cull-winding flip). §0.5 producer-site re-grep performed the census in the spirit of the step.

**Findings:**

- **CRITICAL:** none.
- **MAJOR-1:** B1 Stage 1' did not explicitly cite `clip_w_sign_trap.md` for billboard VS in-front test. **Folded in** §5.4 Stage 1' trap-remediation list.
- **MAJOR-2:** A4 commit ordering was wrong — `Effect::DrawInfo::m_clipper` field deletion (the compile-time enumerator) was listed as Commit 4, after Commit 2's `mclib/gosfx/` tree deletion which contains the struct itself. Build would break at Commit 2. **Folded in** §6.7 with revised order: callers-deletion is now Commit 1, CMake scope change Commit 2, tree deletions 3 & 4, env-gate cleanup 5.
- **MINOR-1:** Pre-flight grep for SSBO `binding=13/14` timing was vague. **Folded in** §5.4 Stage 1' — must run at Commit 4 boundary, not earlier.
- **MINOR-2:** Stage 0' transient `MC2_DISABLE_GOSFX=0` was ambiguous wrt A2 default. **Folded in** §5.4 Stage 0' — opt-in env-set FOR THE TRACE RUN ONLY; does not revert A2 default.
- **MINOR-3:** A4 editor-build wording was vague. **Folded in** §6.5 — editor build OUT of scope; CMake change verified by diff inspection.

**Architectural decisions needing user sign-off:** none. All findings were mechanical and folded in.

**Adversarial verdict (round 1): 0 CRITICAL, 2 MAJOR (folded), 3 MINOR (folded).** Plan is execution-ready PENDING round 2.

### Adversarial-plan-review round 2 (external)

External review surfaced 1 CRITICAL + 4 MAJOR + 2 MINOR. All folded in v2 (this revision):
- **CRITICAL** (A4 self-contradiction) — USER-RESOLVED to full deletion; editor/Viewer/aseconv intentionally broken; follow-on slice filed at A4 ship time. Folded throughout §6, §6.1/6.3/6.5/6.7, and Appendix A negative space below.
- **MAJOR-1** (B1 per-primitive enumeration infeasible) — Folded into §5.4 Stage 2' as atomic enumerator commit at end of stage.
- **MAJOR-2** (trace oracle gosFX-coupled) — Folded into §1 (relocated to `mclib/fx_trace/`) and §6 (KEPT through A4).
- **MAJOR-3** (CI grep gate scoping unresolved) — Folded into §5.4 Stage 1' (B1 phase) + §6.3/§6.7 (A4 phase) + §8.1 (two-phase summary).
- **MAJOR-4** (std430 guard too weak) — Folded into §5.4 Stage 1' (per-field offsetof + alignof asserts; one-source schema deferred to B2 debt).
- **MINOR-1** (A1 internal contradiction) — Folded into §2.1 (gate inside `Effect::Draw`; clipper assignments continue executing).
- **MINOR-2** (A5 default) — Folded into §7.1/7.3/7.4/7.7 (no-op stub default; type registration kept; 2 commits → 1 commit).

**Adversarial verdict (round 2): 1 CRITICAL (USER-RESOLVED + folded), 4 MAJOR (folded), 2 MINOR (folded).** v2 plan is pending one more adversarial-plan-review dispatch per CLAUDE.md "high-stakes plans" discipline.

---

## Appendix A.neg — Negative space (intentionally NOT in scope) — UPDATED v2

- **(v2 CRITICAL fold-in)** editor / Viewer / `aseconv` build correctness AFTER A4 — those targets will fail to build; the round-1 "kept linkable into editor/Viewer/aseconv targets" guarantee is **RETRACTED**. Editor convergence to the modern runtime path is the responsibility of a separate worktree (per `feedback_editor_must_converge_with_runtime_paths.md`). A follow-on slice spec is FILED at A4 ship time (§6.7 Commit 7) but its implementation is not part of this plan.
- B2 primitive types (PertCloud, ShapeCloud, DebrisCloud, EffectCloud) — filed as debt at B1 Stage 5'.
- (D) real light illumination plumbing — separate spec.
- One-source schema generation for `GpuParticle` C++/GLSL pair — filed as B2 polish debt at v2 (MAJOR-4).
- `bdactor.cpp:2180-2183` "Stage 2.D.2 fix" ordering trap — belongs to future (D) arc, NOT A5 (§7.8).

---

## Appendix B — open items for execute time

1. **Heavy-combat mission pick** (B1 Stage 4') — confirm mc2_24 vs mc2_17 from Stage 0' coverage table.
2. **SSBO binding 13/14 pre-flight grep** (B1 Stage 1' Commit 4) — confirm no drift since plan-write.
3. **CI grep gate scoping** — RESOLVED in v2: two-phase definition specified (§5.4 Stage 1', §6.3, §8.1). No execute-time decision required.
4. **(v2 CRITICAL fold-in, replaces round 1 item 4)** **File editor/Viewer/aseconv breakage spec at A4 ship time** — concrete deliverable: `docs/superpowers/specs/<date>-editor-viewer-aseconv-gosfx-retirement-followon.md`. Should enumerate broken targets, cite `feedback_editor_must_converge_with_runtime_paths.md`, and defer implementation to a separate editor-convergence worktree.
5. **Light GameObject loader site** — RESOLVED in v2 (MINOR-2): type registration STAYS; no loader change; no grep required at execute. A5 is `code/light.cpp` only.
