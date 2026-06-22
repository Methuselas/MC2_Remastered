# CROSS-SUBSYSTEM-AUDIT-RECON-1

**Arc:** CROSS-SUBSYSTEM-AUDIT-RECON-1
**Base:** nifty HEAD `da67f030` (clean worktree `A:/Games/mc2-cross-subsystem-audit-recon`)
**Branch:** `claude/cross-subsystem-audit-recon-1`
**Mode:** RECON-ONLY. No production touch. No fixes. Ranked next-slice ledger.
**Method:** 5 parallel read-only subagent sweeps (GameOS, shader/material, texture/asset, EditorBridge+editor, smoke/deploy) + 1 dedup-oracle sweep over prior harness-arc docs. Live-bug suspects re-verified against current HEAD.

---

## 1. Executive summary

After the harness / memory-safety / importer arcs, the **high-value pure-contract surface is mostly drained.** The remaining opportunities cluster in two cheap, real buckets and one design-bucket:

1. **Smoke verdict-layer self-testing has a real RED gap** — the gate evaluator (oracle for every smoke verdict) has unit tests on dataclass inputs but **only 2 of ~12 failure buckets have end-to-end synthetic fixtures.** A regressed gate (inverted exit-code check, renamed bucket) ships green. Pure-Python, no engine, ~1h. **#1 recommendation.**
2. **Pure GameOS helpers are extractable** — timing/clamp math + ASCII case-fold + path-separator normalization are pure but currently game-launch-only to test. Behavior-preserving header extractions, doctest harnesses. Linux/Vulkan portability dividend. **#2–#3.**
3. **Shader-variant coverage is partial** — `reflect.py` covers static_prop/mech variants; MRT_ENABLED (33 #ifdef/17 shaders), TERRAIN_NORMAL_ARRAY, MC2_SHADOW_CSM_MAX are injected-but-no-variant-table. A pure-parse SHADER-VARIANT-MATRIX-HARNESS (no GL compile) catches dead/missing #ifdef coverage. Feasible, LOW risk.

**No NEW live bugs confirmed.** The one shader-agent live-bug suspect (SSBO slot-20 collision) was re-verified and is **ALREADY COVERED** — documented cross-pass reuse in `binding-slot-occupancy.md`, WARN-level in `check-binding-slots.py`, mutually-exclusive passes. Two previously-known-and-fixed items (OMT-1 overlay magenta, R2B cachedFrame) re-confirmed shipped. Texture/asset subsystem is hardened; EditorBridge firewall is clean.

---

## 2. Ranked top 10 candidate next slices

| # | Slice | Class | Type | Risk | Prod-touch | Why |
|---|---|---|---|---|---|---|
| 1 | **SMOKE-SYNTHETIC-FIXTURES-ALL-BUCKETS** | FIX-GAP+HARNESS | Python | LOW | none (test-only) | 10/12 buckets lack e2e fixtures; gate-logic regressions ship green |
| 2 | **GAMEOS-TIMING-MATH-HARNESS-1** | YELLOW | C++ doctest | LOW | none (extract) | frame-delta clamp/ticks2ms pure; smoke never stresses stutter clamp |
| 3 | **GAMEOS-CASEFOLD-HARNESS-1** | YELLOW | C++ doctest | LOW | none (extract) | ASCII case-fold platform-split (`_strlwr` vs tolower); FST key correctness |
| 4 | **SHADER-VARIANT-MATRIX-HARNESS-1** | GREEN | Python (parse) | LOW | none (CI gate) | pure-parse define-injection vs #ifdef coverage; catches dead/missing variants, no GL |
| 5 | **GAMEOS-PATHSEP-HARNESS-1** | YELLOW | C++ doctest+fuzz | MED | none (extract) | backslash→`/` normalize contract; silent FST miss on mixed separators |
| 6 | **SMOKE-FINGERPRINT-FULL-COVERAGE-1** | YELLOW | Python | LOW | opt→default gate | exe fingerprinted but PDB/shader staleness not gated pre-smoke; stale-shader fake-green |
| 7 | **SMOKE-EVIDENCE-CLASSIFIER-1** | YELLOW | Python | LOW | none (advisory) | crash_evidence.json captured but no rule-set → GPU_TDR/PROCESS_KILL/CHURN/RARE |
| 8 | **EDITORBRIDGE-SCREENPICK-UNIT-1** | GREEN | C++ doctest | LOW | none (extract) | `screenPickCompute()` pure coord-transform; zero GL/GameOS deps |
| 9 | **SHADER-MRT-LOCATION-AUDIT-1** | YELLOW | Python/reflect ext | MED | none (CI) | 33 MRT_ENABLED #ifdef, no variant table; location=1 GBuffer1 consistency unchecked |
| 10 | **EDITORBRIDGE-TILE-BOUNDS-FUZZER-1** | GREEN | C++ doctest | MED | none (extract) | `drawTerrainTileOutline` OOB guard fuzz vs realVerticesMapSide |

---

## 3. Per-subsystem findings

### 3.1 GameOS / platform boundary

| Finding | Class | Source-of-truth | Bug class caught |
|---|---|---|---|
| Timing/clamp math | YELLOW | `GameOS/gameos/utils/timing.cpp:58-78`, `gameos.cpp:136-160` (MaxTimeDelta clamp) | frame-delta clamp inversion, ticks2ms platform divergence, non-monotonic time |
| ASCII case-fold | YELLOW | `GameOS/src/platform_str.cpp:81-89` (`S_strlwr`), `utils/string_utils.cpp:61-78` | platform-split fold (`_strlwr` vs tolower loop) → FST key mismatch |
| Path-separator normalize | YELLOW | `mclib/fst_hash.cpp:28-39` (`fst_normalize_key`), `gameos_fileio.cpp:111-124` | mixed `\`/`/` → silent FST lookup miss; double-normalize idempotency |
| FST elfHash + normalize | ALREADY COVERED | `mclib/fst_hash.{h,cpp}` + `tests/unit/test_hashing.cpp` | — (pure header extraction done) |
| Pillarbox rect math | ALREADY COVERED | `mclib/camera_frustum_math.h:67-80` + `camera_frustum_harness` | — (CAMERA-FRUSTUM-HARNESS-1) |
| Mouse 4:3-box remap | YELLOW | `gameos_input.cpp:33-63` / `120-144` | drawable-vs-logical dim confusion, pillarbox round-trip; needs SDL-decouple extract |
| Crash diagnostics (SEH/minidump/dbghelp) | RED | `gos_crashbundle.cpp` | Win32-only, SEH-coupled, no automated crash oracle — defer |
| Memory heap / global new-delete | RED | `memorymanager.hpp`, `gameos.cpp:176-198` | global allocator coupling; not game-free — defer |

**Hard Win32 assumptions blocking Linux/Vulkan:** `MAX_PATH` 260-cap (paths >260 fail silently — fix: `std::string`/`std::filesystem` internal, MAX_PATH only at Win32 interop); `QueryPerformanceCounter/Frequency` (already abstracted in `timing::`, no blocker); `dbghelp.h` (already `#ifndef _WIN32`-gated, Linux stub no-op); `_strlwr` (Windows-only, see case-fold slice). Input already SDL2-abstracted.

### 3.2 Shader variant + material contract

**Covered (do not re-propose):** `shader_contract_harness` (file-refs, dead post-fx bloom/ACES/FXAA/godray absent), `reflect.py` Tier 1.2 (binding/location/offset goldens + REQUIRED_INVARIANTS for static_prop+mech variants), render-pass-table harness, render-state contract harness, icon-atlas harness.

**Live-injected + COVERED:** `MC2_COALESCE` (10 sites, inject `gos_static_prop_batcher.cpp:1161`, 4 variants), `MC2_OBJECT_ID_BUFFER` (9 sites, `v_objectId@location=2` invariant), `MC2_USE_VIEW_UNIFORMS` (9 sites — but env `MC2_VIEW_UNIFORMS=0` disable path is **not** a variant entry; CI-env-vs-prod-env risk, YELLOW-low).

**YELLOW gaps (injected, no variant table):**
- `MRT_ENABLED` — 33 #ifdef / 17 shaders, inject `gameos_graphics.cpp:362`. FBO-attach drives compile branch; shader declares `location=1` but write silently lost if attach fails. → SHADER-MRT-LOCATION-AUDIT-1.
- `TERRAIN_NORMAL_ARRAY` — 5 sites (`gameos_graphics.cpp:366,4898,4924,4965`), sampler2D↔sampler2DArray type swap on env, no golden. → add reflect.py variants.
- `MC2_SHADOW_CSM_MAX` — parameterized cascade count baked at compile (`gos_postprocess.cpp:516,528`, `gameos_graphics.cpp:373`, `gos_terrain_lod_chunk.cpp:356`); runtime env change = no effect, no MAX-vs-array-size check.
- `MC2_STATICPROP_PBR_SLOTS` — runtime `s_ormSlotsEnabled` gate (not a prefix string) yet auto-mapped sampler binding; doc-or-variant decision.
- `ENABLE_TEXTURE1` (gos_vertex*.frag) — `#ifdef` never defined → dead path; remove or `#define =0`.
- `ALPHA_TEST` (5 sites) — not C++-injected, driven by draw-flags not compile; clarify dead-vs-intended.

**SSBO slot-20 dual-use (LightsData vs SurfaceVertexBuf): ALREADY COVERED** — re-verified: documented in `docs/render-backend-seams/binding-slot-occupancy.md`, WARN-level in `scripts/check-binding-slots.py`, mutually-exclusive passes (lighting-compute vs terrain-surface). Not a live collision. (Matches the multiplexed-per-pass binding model from GPU-BINDING-SLOTS-LOCKSTEP-1.)

**SHADER-VARIANT-MATRIX-HARNESS feasible without GL:** YES. Pure-parse — grep `makeProgram()` prefix strings → `#define NAME VAL` set per (shader,variant); grep shader `#ifdef`/`#if defined` → used-macro set; cross-check. Catches dead #ifdef + missing SHADER_VARIANTS entries, <1s, no tool deps. Limits: cannot resolve multi-define `&&` conditions, include-redefines, or binary layout (that's reflect.py's job — this is a *pre*-reflect gate).

### 3.3 Texture / asset policy — HARDENED (mostly GREEN/COVERED)

Missing-texture behavior is safe everywhere (no crashes, explicit fallbacks):
- Mech `resetPaintScheme`: missing `.tga` → untextured render, no error (GREEN).
- GLB import: no base-color → `"NULLTXM"` sentinel skip (GREEN, covered by `mech_texname_harness`).
- Overlay/decal: resolve-fail → 1×1 magenta bind (**OMT-1 shipped `36d6a254`**, ALREADY COVERED).
- Terrain colormap: null-guarded → black + log (shipped, ALREADY COVERED).
- KTX2 route-2 (`txmmgr.cpp:3976-4043`): `.tga` miss → `.ktx2` sidecar; **both-missing → `gosASSERT` (line 4044)** but mode=1 is fallback-only; GREEN, smoke-tested. (Minor: no isolated `MC2_TEXMGR_KTX_PRIMARY=2` force-mode A/B harness — YELLOW-low.)

**Case-sensitivity:** mitigated — `S_strlwr` prepass (`file.cpp:981`) + `deriveName` lowercase + `fst_normalize_key`. No Linux-unsafe derivation found; `S_strlwr` itself is Windows-only (audit when Linux port activates).

**External packs:** IBL-HDRI + mech-GLB manifested+contracted (ALREADY COVERED). Loose `/128/*.tga` = DEAD-LEGACY (superseded by `.ktx2` sidecars; `fst_repack_drop.py` is the manual cleanup). Stale-decal rebake: no live regression on HEAD (R2B `07a1f8ac` cachedFrame fix holds; MONITOR only).

### 3.4 EditorBridge + Editor runtime — CLEAN (GREEN)

- **Firewall enforced:** `EditorBridge/EditorRenderBridge.cpp` is the sole carve-out allowed game+engine includes (in `check-include-firewall.allowlist`); public `.h` exposes only RenderCore::Handle + RenderWorld::VisibilityRequest. **Zero File/FST creep** into bridge.
- **All 6 APIs live** (`pickAt`, `queryVisibility`, `drawSelectionBounds`, `drawTerrainTileOutline`, init/shutdown/isEnabled); no legacy cruft. Edge cases guarded (null land/eye, OOB tile `:259-261`, behind-camera all-or-nothing, `MC2_EDITOR_MODE=0` early-out).
- **Editor buildable:** `EditRel` target present, MSVC-guarded, `MC2_IMGUI` mandatory (FATAL if OFF), GPU-only (no `gameos_main` link; CLAUDE.md "no CPU fallback in editor TU" holds — but enforced by link-order, fragile → optional CMake link-assert).
- **Deploy segregation correct:** `tools/terrain_gen/**` editor-only + FATAL if missing (`deploy_payload.py:395-406`); no accidental game-runtime dependency on editor assets.

Harness candidates: EDITORBRIDGE-SCREENPICK-UNIT-1 (GREEN, pure `screenPickCompute()` `ScreenPick.h:36-44`), EDITORBRIDGE-TILE-BOUNDS-FUZZER-1 (GREEN), EDITORPICK-TERRAIN-FIXTURE-1 (YELLOW, mock camera+terrain), EDITOR-SAVE-FORMAT-CONTRACT-1 (YELLOW, .ini/.modproject/.foliage.json round-trip). Fake-green watch: Path-A(GPU)/Path-B(terrain) pick can both hit → add `MC2_EDITOR_PICK_TRACE` mismatch diag (LOW, dev-only).

### 3.5 Smoke / deploy / release operations

- **Fingerprint (Q1):** YELLOW — only **exe** carries `[BUILD_FINGERPRINT v1]` (`smoke_lib/fingerprint.py`); PDB staleness hardened in deploy (`deploy_payload.py:462-469`) but **shader staleness only caught at deploy post-copy diff, not gated pre-smoke**; `--verify-preflight` is opt-in. Stale shader → NaN frame → smoke still PASS. → SMOKE-FINGERPRINT-FULL-COVERAGE-1.
- **Synthetic buckets (Q2):** RED — `gates.evaluate()` is the verdict oracle, unit-tested on dataclasses (`test_gates.py`), but `tests/smoke/fixtures/` has only `fake_mc2_pass.py` + `fake_mc2_hang.py`. **10/12 buckets** (crash_silent, crash_no_summary, engine_reported_fail, heartbeat_freeze_load/play, gl_error, pool_null, asset_oob, shader_error, missing_file, instrumentation_missing) have **no end-to-end fixture.** All reproducible synthetically (stdout patterns + exit code, no engine). → SMOKE-SYNTHETIC-FIXTURES-ALL-BUCKETS (**top pick**).
- **Crash evidence (Q3):** GREEN — `crash_evidence.py` captures exit/phase/markers/event-log-TDR/GPU-info/concurrent-mc2/minidumps (`SMOKE-CRASH-SILENT-EVIDENCE-1` shipped). Missing: a **classifier rule-set** to turn evidence.json → GPU_TDR/PROCESS_KILL/CHURN/RARE (YELLOW → SMOKE-EVIDENCE-CLASSIFIER-1).
- **Concurrency (Q4):** GREEN — exclusive lock + PID-liveness + path-aware process filter (`run_smoke.py:489-553`); `--kill-existing` forbidden + purged from matrices (`089b79b3`,`de039708`); concurrent mc2 recorded in evidence.
- **resolve_mc2_target / smoke_artifact_summarize (Q5):** ALREADY COVERED elsewhere — target via `deploy_payload.py` presets + `--exe`; summary via `report.render_markdown/json`. Standalone funcs not needed.

---

## 4. ALREADY COVERED (do NOT re-propose)

Framework + 13 registered harnesses: `contract_harness.h` template; `shader_contract_harness`; `render_state_contract_harness` (+clean-link fix); `objmgr_contract_harness`; `ibl_registry_contract_harness`; `render_pass_table_harness`; `logistics_csv_harness`; `camera_frustum_harness`; `mech_texname_harness`; `icon_atlas_harness`; `deploy_asset_contract_harness.py`; `deploy_release_tree_contract_harness.py`; `mech_glb_pack_contract_harness.py`. Plus: `reflect.py` shader-binding goldens; IBL external manifest; mech-GLB external manifest; SMOKE-ORACLEPARSE-COVERAGE-1; SMOKE-GATES-BUCKET-COVERAGE-1 (unit); crash-evidence capture; concurrency lock; binding-slot occupancy doc + `check-binding-slots.py`; OMT-1 overlay guard; R2B cachedFrame fix; FST elfHash unit test. Dev-efficiency: `slice_preflight` (mandatory), `build_contract_harnesses.py`, `make_handoff.py`.

---

## 5. RED / deferred (with reasons)

| Target | Reason |
|---|---|
| FitIniFile / .fit parse | File-IO + systemHeap + FST + gosASSERT coupled; over-stub risk (see `fit-parse-harness-1-recon.md`) |
| Logistics purchase/variants/inventory; pilot/warrior; campaign metadata; save/load | CSVFile+PacketFile+FitIniFile entangled; only objmgr watch-policy had a clean seam |
| Sound/music/.bik refs | runtime-dynamic IDs, FST-fallback by design |
| GameOS crash SEH/minidump; global heap/new-delete | Win32-coupled, no game-free oracle |
| Missing-texture magenta policy; .ktx2/.tga sidecar derive; shader *compile* permutation | GL-welded side-effects, not returnable values |
| PassIdentity↔RenderPassId mapping | no mapping exists in code yet — untestable until one added |
| EditorPick GPU Path-A | needs object-ID RT + RenderWorld integration; high render dep |

---

## 6. Recommended next 3 slices — GO / NO-GO

### GO — SMOKE-SYNTHETIC-FIXTURES-ALL-BUCKETS (#1)
Pure-Python fixtures (one per bucket, model on `fake_mc2_pass.py`) + `test_runner.py` that runs each through the real stdout→LogSummary→`gates.evaluate()` chain and asserts the expected bucket. **Why smoke is a bad oracle:** smoke *is* the system under test; the gate classifier is the oracle and currently only its pure logic (not the parse+evaluate chain) is tested. **Fake-green avoidance:** fixtures must drive the real runner parse path, not call `evaluate()` directly. Risk LOW, prod-touch none, ~1h. **GO.**

### GO — GAMEOS-TIMING-MATH-HARNESS-1 (#2) [+ CASEFOLD #3 as sibling]
Extract `ticks2ms` + frame-delta-clamp into `GameOS/gameos/utils/timing_math.h` (callers unchanged), doctest the clamp/monotonicity/round-trip. Pairs naturally with GAMEOS-CASEFOLD-HARNESS-1 (extract ASCII fold into `ascii_case.h`). **Why smoke bad oracle:** 30s steady tier1 never hits stutter-clamp or mixed-case asset paths. **Fake-green avoidance:** harness + production call the *same* extracted function (no reimplementation). Risk LOW, prod-touch = behavior-preserving extraction only. **GO** (do timing first; casefold if portability work continues).

### GO-WITH-CAVEAT — SHADER-VARIANT-MATRIX-HARNESS-1 (#4)
Pure-parse coverage gate (define-injection vs #ifdef). **Caveat:** scope it as a *coverage/dead-symbol* reporter (pre-reflect gate), NOT a layout validator — do not duplicate reflect.py. Ship as WARN-first (like `check-binding-slots.py`) to avoid CI churn on known gaps (MRT/CSM). Risk LOW, prod-touch none. **GO** if shader work is on the near roadmap; otherwise defer behind the two GOs above.

**NO-GO this round:** EDITORPICK-GPU, FitParse, any RED-table item, SMOKE-EVIDENCE-CLASSIFIER (wait until evidence corpus shows a recurring class worth a rule).

---

## 7. Live bugs found

**None new confirmed.** All correctness-suspect findings resolved to ALREADY-FIXED or BY-DESIGN:
- SSBO slot-20 "collision" → BY-DESIGN cross-pass reuse, WARN-covered (`check-binding-slots.py`).
- OMT-1 overlay magenta, R2B cachedFrame tree-vanish → previously shipped, re-confirmed on HEAD.
- KTX2 both-missing `gosASSERT` → real assert but mode=1 is fallback-only; not a live crash path in stock/slim deploy. Note as low-risk hardening candidate, not a bug.

The closest thing to a *correctness exposure* is structural, not a bug: **stale-shader fake-green** (#6) — smoke can pass on a stale/NaN shader because pre-smoke shader-manifest staleness isn't gated. That's a harness gap, not a code defect.

---

## 8. "Do NOT do this" warnings

1. **Do NOT over-stub.** A stub returning a "nice" value masks the corruption under test. Link the real TU; stub only pure-environment globals.
2. **Do NOT link half the engine.** Blows the <1s + game-free guarantee. Extract the smallest pure unit.
3. **Do NOT duplicate logic into the harness.** Testing a reimplemented copy ≠ testing production. Extract a shared TU both call.
4. **Do NOT ship a green harness that misses the fixed code path.** Worse than none.
5. **Do NOT re-derive already-fixed bugs.** Base-drift trap (OBJMGR-WATCHID, fit-parse divisor, slot-14 particle leak were all re-discovered stale). **MANDATORY `slice_preflight` before any recon-derived fix slice.**
6. **Do NOT re-flag SSBO slot-20** (or any multiplexed binding slot) as a collision — the binding model is intentionally per-pass-multiplexed; `check-binding-slots.py` already governs it.
7. **Do NOT touch foreign nifty WIP** (`gos_mech_batcher.*`, `assimp_importer.cpp`, `mech3d.cpp`, `mech_anim_runtime.h`, `golden-sets.json` + many untracked recon docs were dirty at `da67f030`). This recon worktree was forked clean to avoid them.
8. **Do NOT pair a FIX+EXTRACTION slice without the real bug fix** (objmgr/logistics precedent) — extraction is justified only by a driven correctness path.

---

*Recon complete. Subagent sweeps: GameOS, shader/material, texture/asset, EditorBridge+editor, smoke/deploy, prior-harness dedup-oracle. Live-bug suspects re-verified vs HEAD `da67f030`.*
