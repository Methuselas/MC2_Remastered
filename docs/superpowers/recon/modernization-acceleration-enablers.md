# Modernization Acceleration Enablers — Recon Synthesis

**Date:** 2026-06-11 | **Synthesis of:** scratch-accel-{1-deploy,2-passes,3-visual,4-scaffold}.md | **Status:** RECON ONLY, no code changes.
**Constraints honored:** no render-graph rewrite, no Vulkan/DX12, no in-process PIE, run_smoke verdict path untouched, advisory-before-hard-gate, bridges over rewrites.

---

## 1. Executive verdict — top 5 enabling pieces (ranked)

1. **Deploy payload completeness check (exe-lock + hash + shader-diff + PDB manifest).** Prevents: *stale-exe / stale-shader / wrong-target debug cycles* (scratch-accel-1-deploy.md:7-35 — Mistakes A/B/C/D, "hours/days false-path investigation").
2. **Env-allowlist drift check (advisory) for run_smoke.py:305-460.** Prevents: *new MC2_* gate silently OFF in smoke → feature merged with zero regression coverage* (scratch-accel-1-deploy.md:61-68, MC2_GL_DEBUG_FATAL already dropped).

   > **CORRECTION (2026-06-11):** The specific MC2_GL_DEBUG_FATAL silent-drop claim was false. It is already in passthrough as of c8b7ac03. runner.py inherits parent env with os.environ.copy(). The allowlist check remains useful as convention/drift tooling, not proof that gates are currently dropped.
3. **[RENDER_PASS v1] per-pass telemetry off render_contract.cpp assertPassContract.** Prevents: *"which pass broke it" archaeology and pixel-squint attribution* — gives composition deltas (draws/binds/FBO switches) before pixels (scratch-accel-2-passes.md:54-71, scratch-accel-3-visual.md:53-62 advisory ladder step 3-4).
4. **visual_diff.py layers 1-3 + HTML triptych gallery (S3 of visual lab).** Prevents: *"Joe eyeball this again" loops* — shader change verification collapses from launch+fly+squint to "diff within 0.5%? PASS" (scratch-accel-3-visual.md:66-82). Non-colliding complement to the RUNNING Baseline A session (it owns capture, NOT diff metrics/blessing — scratch-accel-3-visual.md:88-93).
5. **Feature-flag registry auto-gen (flags.auto.yaml from RendererFeatureRegistry.h) + check-env-vars-documented.sh.** Prevents: *tier1_env_vars.md staleness (STALE since 2026-06-01) and untracked getenv() drift (~53 untracked of ~400 hits)* (scratch-accel-4-scaffold.md:87-115, 193-198).

Honorable 6th: **GlStateGuard RAII** — prevents inherited-GL-state transparency-saga class bugs (DebugOverlay/tactical violators, scratch-accel-2-passes.md:85-87) — but **deferred post-Baseline-A by standing decision**; recon-only spec allowed.

---

## 2. Problem taxonomy — repeated modernization taxes

| Tax | Incidents | Source |
|---|---|---|
| **Stale deploy** (exe lock, cp -r no-overwrite, wrong target v0.3/v0.4/0.4c, stale PDB, stale .obj) | A-D,F; "full debug cycle wasted", "days" | accel-1:7-35 |
| **Silent env-gate drop in smoke** | MC2_GL_DEBUG_FATAL; any new gate | accel-1:61-68 |
| **Inherited GL state** (bolt-on passes don't set depth/blend; no RAII guard) | transparency saga f375e0ba; DebugOverlay/tactical-arc violators | accel-2:85-87; MEMORY 10.3 |
| **D3D↔GL projection split-brain** | cull X-mirror a280dde2, shadows, props, editor pick | MEMORY (recurring class) |
| **Eyeball-only visual verification** | tier1 idle = zero FX spawns; visual wrongness invisible | accel-3:7-17 |
| **Doc/registry drift** | tier1_env_vars.md stale; allowlist unbounded ~200; handoff↔worktree divergence; deploy-target creep | accel-4:193-198 |
| **Hand-written lane ceremony** | every lane re-writes handoff/plan/paths/smoke commands | accel-4:21-49 |
| **Subagent edits stale ROOT files** instead of worktree | dynamic-pipeline incident; rank-4 check | accel-4:174; MEMORY 06-09 |

---

## 3. Dependency map

```
Deploy payload checks (accel-1)          Lane starter kits (accel-4)
  exe-lock/hash/shader/PDB manifest        flags.auto.yaml ──> check-env-vars-documented
  env-allowlist check ─────────────┐       new-lane.py (needs flag registry)
        │                          │       done-report parser
        v                          v
  S8 package/install lane  <── manifest format owner (accel-1 sec 3)
        │
        v
Visual capture (accel-3) ── S9 roadmap
  [RUNNING Baseline A session: readback/state.json/determinism]
  + visual_diff.py (S3) + blessing (S4) + bookmarks (S2)  <- NOT owned by running session
        ^
        │ composition-delta feed (render_counts.json)
RenderWorld pass graph (accel-2)
  [RENDER_PASS v1] telemetry ──> per-pass counters ──> visual lab oracle pre-gate (S5)
  pass-graph open questions (5) ──> GlStateGuard spec (DEFERRED post-Baseline-A)
        │
        v
  Baseline A bless ──> Tube merge / GlStateGuard impl / VFX default-on
```

Key edges: deploy checks gate **everything** (any capture/diff against a stale exe is garbage — accel-3 advisory ladder step 1 is exe identity). Telemetry feeds diff metrics (numbers before pixels). Flag registry feeds both env-allowlist check and new-lane.py. Baseline A bless is the choke point for Tube/GlStateGuard.

---

## 4. Recommended next 10 slices

| # | Slice | Owner doc/lane | Branch | Diff | Risk | Unblocks | Validation gate (agent-checkable) | NOT touch | Failure prevented |
|---|---|---|---|---|---|---|---|---|---|
| 1 | Deploy payload completeness (exe-lock tasklist, post-copy sha256, shader diff `\|\| exit 1`, PDB check, `.deployed_manifest.csv`) | accel-1 sec 2/4; S8 lane seed | `claude/sp-s8-deploy-manifest-1` | S | Low | All capture/diff work; S8 | tier1 5/5 + manifest file exists w/ matching exe hash after `/mc2-deploy`; intentionally-stale exe → nonzero exit | run_smoke verdict path; deploy dir layout | Stale exe/shader/PDB/wrong-target (Mistakes A-D) |
| 2 | check-env-allowlist.sh advisory pre-commit (grep new getenv MC2_* vs run_smoke.py:305-460) | accel-1 sec 5 | `claude/sp-env-allowlist-check-1` | S | Low | Safe gate additions; slice 6 | Script flags MC2_GL_DEBUG_FATAL as known miss; zero false-pos on HEAD; tier1 untouched | run_smoke env logic itself (report only) | Mistake E silent-OFF gates |
| 3 | [RENDER_PASS v1] telemetry (7-field PassTelemetry, emitted in assertPassContract scope, env-gated MC2_RENDER_PASS_TELEMETRY) | accel-2 Q4 | `claude/sp-render-pass-telemetry-1` | M | Med (touches render_contract.cpp hot path — must be no-op off) | Oracle pre-gates S5; pass-cost attribution; GlStateGuard recon | tier1 5/5 with flag ON and OFF; OFF = zero new log lines; ON = 13 passes emit counters, drawCallCount>0 for TerrainBase | Pass enum values; verdict path; no per-draw zones <100ns floor | "Which pass broke it" archaeology; pixel-first debugging |
| 4 | visual_diff.py layers 1-3 + HTML triptych | visual-regression-lab S3 | `claude/sp-visual-diff-s3-1` | M | Low (pure offline script) | Baseline A bless (S4); kills eyeball loops | Self-diff identical capture → byte-identical PASS; perturbed TGA → changed-pixel% reported; no engine change so tier1 trivially 5/5 | Engine code; Baseline A session's capture artifacts (read-only consumer) | Eyeball-only verification |
| 5 | flag-registry-auto-gen.py → flags.auto.yaml | accel-4 sec 3/6 rank-1 | `claude/sp-flag-registry-autogen-1` | M | Low | Slices 6,7; lane gen | Emits yaml w/ all RendererFeatureRegistry.h entries; round-trip count stable; idempotent re-run = no diff | RendererFeatureRegistry.h enum values | Registry/doc drift |
| 6 | check-env-vars-documented.sh (getenv vs flags.auto.yaml/tier1_env_vars.md, advisory) | accel-4 rank-2 | `claude/sp-env-doc-check-1` | S | Low | Doc trustworthiness | Reports ~53 known untracked vars on HEAD; exit 0 (advisory mode) | Existing check-env-registry.sh allowlist semantics | tier1_env_vars.md staleness |
| 7 | check-stale-root-file-edits.sh (block worktree-session edits to ROOT mirrors) | accel-4 rank-4 | `claude/sp-root-edit-guard-1` | S | Low | Subagent dispatch safety | Synthetic ROOT edit from worktree → nonzero; clean tree → 0 | Root files themselves | Subagent-edited-stale-ROOT (mech3d incident) |
| 8 | MC2_FX_FORCE_SPAWN destruction fixture (~80 lines mech3d.cpp:69) | vfx-modernization-roadmap sec 9.1 | `claude/sp-fx-force-spawn-1` | M | Med (engine edit; zero-cost when unset) | 3 mesh-FX visual slices; tube/VFX golden frames | tier1 5/5 unset; set → MC2_FX_COUNT_LOG counters nonzero in 30s smoke | FX spec data; render order; verdict path | tier1-idle blindness to FX (accel-3 sec 1) |
| 9 | LANE_COMPLETION_REPORT.md template + parse-done-reports.py | accel-4 sec 4 | `claude/sp-done-report-1` | S | Low | Orchestrator dashboards; S8 payload check rank-3 | Parser extracts frontmatter from a seeded exemplar; drift warning fires on missing artifact | Existing handoff docs (additive) | Handoff↔worktree divergence; deploy-target creep |
| 10 | GlStateGuard RAII **spec-only recon** (codify gos_particle_bridge.cpp:375-661 pattern; list violator sites) | accel-2 Q5 | `claude/sp-glstateguard-spec-1` | S | None (doc only) | Post-Baseline-A implementation lands day-1 | Doc lists all violator sites w/ file:line; zero code change (git diff empty outside docs/) | ANY engine code (standing deferral) | Inherited-GL-state transparency class |

new-lane.py (accel-4 rank-3) intentionally slice 11 — depends on 5+9.

---

## 5. Do now / do later / do never

| Do NOW (parallel-safe, no session collision) | Do LATER (gated) | Do NEVER |
|---|---|---|
| Slices 1,2 (deploy+allowlist checks) | GlStateGuard implementation (post-Baseline-A bless) | Full render-graph rewrite |
| Slice 3 (RENDER_PASS telemetry) | Tube merge (Baseline A gate) | Vulkan/DX12 backend |
| Slice 4 (visual_diff.py — capture-consumer only) | Bookmarks S2 / blessing S4 (after Baseline A session lands artifacts) | In-process PIE |
| Slices 5,6,7,9 (scaffold scripts) | new-lane.py (after flag registry + done-report) | Hard-gate any new check before ≥1 week advisory burn-in |
| Slice 8 (FX fixture, default-off) | VFX default-on, mesh-FX slices (need fixture + golden) | run_smoke verdict-path changes; touching ObjectManager enqueue/immediate split |
| Slice 10 (GlStateGuard spec doc) | Oracle pre-gate S5 wiring into smoke (after 3+4) | Per-vertex/per-quad Tracy zones (<100ns floor) |

---

## 6. Merge/train implications

- **Baseline A post-8z capture (RUNNING):** owns TGA readback, state.json, determinism proof. Slices 4 (diff metrics) and bookmark/blessing work are explicitly the **non-owned remainder** (accel-3 sec 6) — slice 4 may consume its artifacts read-only but must not edit gos_visual_diff.cpp or capture flags until that session closes. Do NOT bless goldens until its determinism proof lands.
- **Tube merge:** stays gated on Baseline A bless (accel-2 Q6; standing decision). Nothing here touches mclib/gosfx/tube.cpp.
- **GlStateGuard:** deferred post-Baseline-A — slice 10 is spec-only to respect that.
- **Formation line drawing commands (RUNNING):** likely lives in gamecam/overlay lane — slice 3 must not refactor overlay draw sites (telemetry emission point is render_contract.cpp only, no gamecam.cpp edits). Slice 10 spec lists overlay violators but changes nothing.
- **S4b FastFile sidecars (idle):** orthogonal; slice 1's manifest is deploy-side, not asset-resolve-side — keep file-resolve/registry-index in the separate future chip (accel-1 sec 3).
- **Render architecture recon (idle):** produced renderworld-pass-graph-audit.md — slice 3 should cite it, not re-audit.
- **Merge-train order suggestion:** scripts-only slices (1,2,5,6,7,9,10,4) can land on nifty-mendeleev anytime (no engine diff). Engine slices 3 and 8 should land **before** Tube merge reopens the window, each behind its env gate, tier1 5/5 each.

---

## 7. Agent routing

| Slice | Model | Why |
|---|---|---|
| 1,2,6,7,9 | **Haiku/Sonnet** (cavecrew-builder for 1-2 file scripts) | Mechanical script + grep work |
| 3 | **Sonnet** impl + **Opus** review of emission-point placement (hot path) | render_contract.cpp is load-bearing |
| 4 | **Sonnet** | Offline Python + HTML, well-specced in visual-lab doc |
| 5 | **Sonnet** | Header parsing, idempotency care |
| 8 | **Sonnet** impl, **Fable/Opus** pre-review (engine edit, mech3d.cpp 5139 lines, stale-ROOT trap history) | Prior subagent stale-file incident on this exact file |
| 10 | **Fable/Opus** | Architecture spec; needs cross-pass judgment |
| Orchestration/dispatch | **Fable** (this tier) | Session-collision awareness |

---

## 8. Ready-to-dispatch prompts

### (a) Deploy payload completeness chip

```
RECON-INFORMED IMPLEMENTATION. Worktree: A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev (edit ONLY here, never repo ROOT).

Task: harden the deploy step against the four documented stale-deploy failure classes (stale exe via process lock, stale shaders via cp -r no-overwrite, wrong deploy target, stale PDB). See docs/superpowers/recon/scratch-accel-1-deploy.md sections 1-4.

Files to touch:
- .claude/skills/mc2-deploy/SKILL.md (or the deploy script it drives) — add Step 0 exe-lock check (tasklist | findstr mc2.exe → fail with message), per-file cp -f for shaders with `diff -q ... || exit 1` verification, post-copy sha256 compare of source build64/RelWithDebInfo/mc2.exe vs deployed exe (fail on mismatch), PDB mtime check.
- NEW scripts/write-deploy-manifest.ps1 (or .sh) — after successful deploy write A:/Games/mc2-opengl/mc2-win64-v0.4/.deployed_manifest.csv with: deployed_version, exe_path, exe_hash_sha256, pdb_hash, shader_count, dll_versions, env_allowlist_commit (git rev-parse HEAD).
Both v0.4 (game) and 0.4c (editor) targets — these are DIFFERENT exes (known trap: fixing 0.4c does not update v0.4).

Constraints: advisory messaging is fine but exe-hash mismatch and shader diff MUST hard-fail the deploy script itself (this is a deploy gate, not a smoke gate). Do NOT modify scripts/run_smoke.py or any verdict path. No engine code changes. Zero behavior change when deploy succeeds cleanly.

Validation gate: (1) run deploy → manifest exists, hash in manifest == Get-FileHash of deployed exe; (2) simulate lock (start mc2.exe or open exe handle) → deploy exits nonzero with clear message; (3) tier1 5/5: py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs (exit 0).
```

### (b) [RENDER_PASS v1] telemetry slice

```
RECON-INFORMED IMPLEMENTATION. Worktree: A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev (edit ONLY here, never repo ROOT — prior incident: subagent edited stale ROOT mech3d.cpp).

Task: add [RENDER_PASS v1] per-pass telemetry per docs/superpowers/recon/scratch-accel-2-passes.md Question 4. Env gate: MC2_RENDER_PASS_TELEMETRY=1, default OFF, zero-cost off (single getenv-cached bool, no allocation, no log lines when off).

Files to touch:
- mclib/render_contract.cpp / render_contract.h — add struct PassTelemetry { PassIdentity id; uint64_t gpuNsElapsed; uint32_t drawCallCount, triangleCount, vertexCount, textureBindCount; uint8_t fboBindCount, shaderSwitchCount; }; accumulate per pass, emit one "[RENDER_PASS v1] pass=<name> draws=<n> tris=<n> ..." line per pass per N frames (N=300 like state dump) inside the assertPassContract trace scope.
- Counter feed: increment at existing centralized draw submission / state-cache points in GameOS/gameos/gameos_graphics.cpp (grep for the state cache + gos_InvalidateRenderStateCache plumbing) — do NOT scatter counters into per-object game code.
Cross-check pass list against docs renderworld-pass-graph-audit.md (13 registered passes, Shadow→PostProcess) — do not add/renumber enum values.

Constraints: env-gated zero-cost-off; no run_smoke.py or verdict-path changes (telemetry is log-only, ADVISORY — no new gate fails on it); no Tracy zones <100ns / no per-draw zones; do not touch gamecam.cpp overlay code (a RUNNING session owns formation-line drawing there); no behavior change to rendering.

Validation gate: tier1 5/5 with flag OFF (and grep artifacts: zero [RENDER_PASS lines); tier1 5/5 with MC2_RENDER_PASS_TELEMETRY=1 — note this requires adding the var to run_smoke.py env allowlist (lines ~305-460) which is allowed (allowlist add, not verdict logic); ON-run artifacts must show [RENDER_PASS v1] lines for ≥10 of 13 passes with drawCallCount>0 for TerrainBase and OpaqueObject.
```

### (c) Lane starter kit / feature-flag registry starter

```
RECON-INFORMED IMPLEMENTATION (scripts only, no engine code). Worktree: A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev.

Task: first two rungs of the lane-scaffold ladder from docs/superpowers/recon/scratch-accel-4-scaffold.md sections 3 & 6.

Files to create:
- scripts/flag-registry-auto-gen.py (~150 LOC): parse RenderCore/RendererFeatureRegistry.h (struct EnvVarDesc fields kind/env_var/default_on/doc, ~lines 61-100 — grep to confirm) → emit docs/flags.auto.yaml with id, kind, env_var, default_on, doc, plus grep-derived hit_count per flag. Idempotent: re-run on unchanged tree produces no diff.
- scripts/check-env-vars-documented.sh (~80 LOC): find every getenv("MC2_*") in RenderCore/ GameOS/ mclib/ code/ Editor/; report any var absent from BOTH flags.auto.yaml and scripts/check-env-registry.sh ALLOWLIST. ADVISORY MODE: always exit 0, print "DRIFT:" lines (expect ~53 untracked on current HEAD per recon). Add a --strict flag for future hard-gating; do NOT wire into pre-commit hard-fail yet.

Constraints: zero engine/code changes; do not modify RendererFeatureRegistry.h, check-env-registry.sh, or run_smoke.py; do not auto-edit docs/tier1_env_vars.md (stale doc cleanup is a later slice); advisory before hard gate.

Validation gate: (1) flag-registry-auto-gen.py runs clean, yaml contains every enum entry from RendererFeatureRegistry.h (spot-check 3: e.g. MC2_OBJECT_ID_BUFFER); (2) double-run = identical yaml (git diff empty); (3) check script exits 0 and lists known-untracked vars; (4) tier1 5/5 unchanged (no engine diff, run once to prove environment intact).
```

---

## 9. Open questions needing user decision

1. **Hard-fail threshold for deploy checks:** exe-hash + shader-diff hard-fail immediately (recommended), or burn-in advisory first like other gates?
2. **visual_diff.py changed-pixel PASS threshold** (recon suggests 0.5%) — bless number, or per-capture-set config?
3. **Baseline A blessing authority:** who blesses goldens (user eyeball once, then frozen)? Needed before S4.
4. **MC2_FX_FORCE_SPAWN scope:** ship now (default-off, engine edit during pre-Tube quiet window) or hold for the VFX lane reopen?
5. **flags.auto.yaml as single source of truth:** eventually retire tier1_env_vars.md, or keep both with drift check?
6. **new-lane.py priority:** worth a slice this train, or after S8/S9 land?
7. **Extractor conflict to resolve:** accel-2 says all raw-GL sites call gos_InvalidateRenderStateCache (line 48) yet flags DebugOverlay/tactical as violators with NO state restore (line 87) — invalidate-cache ≠ restore-state; confirm GlStateGuard spec (slice 10) treats these separately.

---

## 10. Caveman ledger

1. Four extractors agree: biggest taxes = stale deploy, silent env-drop, eyeball-only visual check, doc/flag drift, hand ceremony.
2. Deploy payload check = rank 1. Cheap (S), kills Mistakes A-D, gates all capture work.
3. Env-allowlist advisory check = rank 2. Kills "merged with gate OFF" class.
4. RENDER_PASS telemetry = rank 3. Numbers before pixels; feeds visual-lab oracle pre-gate.
5. visual_diff.py = rank 4. Running Baseline A session does NOT own it — safe parallel.
6. Flag registry auto-gen + doc check = rank 5. Fixes stale tier1_env_vars + ~53 untracked getenvs.
7. 10 slices specced w/ branches + gates; 8 scripts-only (land anytime), 2 engine (3,8) env-gated.
8. GlStateGuard = spec-only now (standing post-Baseline-A deferral). Tube merge untouched.
9. No collisions: avoid gos_visual_diff.cpp + capture flags (Baseline A RUNNING), gamecam overlay (formation-lines RUNNING).
10. Three dispatch prompts ready (sec 8): deploy chip, RENDER_PASS slice, flag-registry starter. Next session can fire immediately.
