# Slice 2 (object-offload) — Stage 2.E — Pinned-camera screenshot diff (handoff prompt)

> **Role for a fresh session reading this:** You are picking up the
> object-offload arc at **Stage 2.E — pinned-camera screenshot diff
> harness**. Stages 2.A-2.D are COMPLETE. The slice 2 substrate has
> been validated numerically by Stage 2.D's parity check (29 stock
> missions, 0 mismatches, +0 destroys). Stage 2.E adds visual-fidelity
> validation: deterministic camera positioning + screenshot capture +
> tolerance-based diff between `MC2_GPU_OBJECTS=0` (CPU baseline) and
> `MC2_GPU_OBJECTS=1` (GPU). This is the LAST gate before default-on
> flip of `MC2_GPU_OBJECTS`. After Stage 2.E lands, the slice 2 PR
> can ship as default-on.

---

## Current state (as of 2026-05-04)

**Stage 2.D arc complete.** HEAD is `1935502` "docs(objects): stage 2.D complete — handoff + recon docs + parity-substrate lesson".

Verify before starting:
```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git rev-parse --short HEAD
# expect 1935502 OR a more recent commit on claude/nifty-mendeleev
git log --oneline | grep -E "stage 2\.[A-E]|2\.C\.4|substrate" | head -10
# expect to see the 2.D commit chain present:
#   1935502 stage 2.D complete docs
#   ee46cc5 stage 2.C.4 substrate fix
#   95baa44 stage 2.D.3 sampled parity plumbing
#   f3ab114 stage 2.D.2 record correction
#   014ceb8 stage 2.D.2.1 corrective fixes
#   38ba240 stage 2.D.2 dual-emit + parity zero mismatches
#   566a0f0 stage 2.D.1.1 slot-overflow accounting
#   653e8ce stage 2.D.1 parity readback harness
```

If any of these are missing, escalate to user — the arc may have been reverted or the worktree drifted.

---

## Worktree + branch

- **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Branch:** `claude/nifty-mendeleev`
- **Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.3/`

---

## REQUIRED READS (in order — non-skippable)

### Spec context

1. `.claude/worktrees/nifty-mendeleev/CLAUDE.md` — load-bearing project rules. Especially:
   - Documentation Discipline (grep at write-time)
   - Review Discipline (Stage 2.E is parity-validation-class — adversarial review of the harness PLAN before implementation; prose review of the harness CODE since it's tooling, not substrate)
   - Critical Rules (build / deploy / shader version)
   - Tier-1 Instrumentation Env Vars
   - Smoke Gate command (drop `--menu-canary` per `feedback_smoke_no_canary.md`)

2. `docs/superpowers/specs/2026-05-02-object-offload-slice2-design.md` — search for "Stage 2.E — Pinned-camera screenshot diff" (~line 366). The official spec for this stage. Quoted verbatim:

   > ### Stage 2.E — Pinned-camera screenshot diff (separate PR, gates default-on)
   >
   > Same harness as slice 1's Stage 1.E. If slice 1's Stage 1.E hasn't landed yet, this stage builds it; if it has, slice 2 reuses it.
   >
   > Files:
   > - `tests/smoke/object_visual_diff.py`: deterministic camera-pin + screenshot capture + tolerance-based diff against baseline reference.
   > - Baseline captured with `MC2_GPU_OBJECTS=0`.
   > - Diff captured with `MC2_GPU_OBJECTS=1`.
   > - Threshold: ≤0.5% pixels diffed by ≤2 LSB.
   >
   > **Gate**: pixel-diff under threshold. Required for default-on flip; NOT for flagged merge.

3. `docs/superpowers/specs/2026-05-02-object-offload-slice2-stage2d-handoff-prompt.md` — read the WHOLE file. Especially the "Update 2026-05-04 — Stage 2.D COMPLETE" section near the top. Background for what 2.E builds on.

4. `docs/superpowers/plans/progress/2026-05-03-object-offload-slice2-stage2d-record.md` — Stage 2.D arc record. The substrate bugs found there are the WHY of Stage 2.E. They were CPU/GPU numerical divergences invisible to visual smoke at top-down RTS camera. Stage 2.E exists because numerical parity ≠ visual parity (a visual regression could exist that the parity check happens to not sample, or that's outside the lighting kernel). Visual diff is the last guardrail.

### Memory files (load-bearing — read each)

5. **⭐ `~/.claude/projects/A--Games-mc2-opengl-src/memory/parity_finds_gpu_substrate_bugs_visual_smoke_misses.md`** — the lesson from 2.D. Stage 2.E is the inverse safety net: parity caught numerical bugs visual smoke missed; pinned-camera diff catches visual bugs parity sampling might miss. Read for the "validation surface needs to be wider than tier1" warning.

6. `~/.claude/projects/A--Games-mc2-opengl-src/memory/feedback_smoke_no_canary.md` — drop `--menu-canary` from smoke commands.

7. `~/.claude/projects/A--Games-mc2-opengl-src/memory/feedback_smoke_serial_only.md` — never two `mc2.exe` instances concurrently. Stage 2.E will need at minimum two sequential runs per mission (baseline + test).

8. `~/.claude/projects/A--Games-mc2-opengl-src/memory/feedback_smoke_duration.md` — `--duration 10` default, but Stage 2.E may want longer to let the camera settle deterministically.

9. `~/.claude/projects/A--Games-mc2-opengl-src/memory/feedback_smoke_mission_filter.md` — `--mission mc2_NN` for single-mission iteration; full tier1+tier2 only at gates.

10. `~/.claude/projects/A--Games-mc2-opengl-src/memory/feedback_subagent_no_cmake_configure.md` — NEVER `cmake -B build64`.

11. `~/.claude/projects/A--Games-mc2-opengl-src/memory/feedback_subagent_cmake_path_explicit.md` — use `cmake` directly OR the explicit path; **NO** `where cmake` / `find` / candidate-path probing.

12. `~/.claude/projects/A--Games-mc2-opengl-src/memory/feedback_offload_scope_stock_only.md` — tier1+tier2 stock only; mod content out of scope for the gate.

13. Any memory file that mentions "screenshot," "canary," "pinned-camera," or "visual diff" — grep memory/ for these terms before writing infra; you may find prior work to reuse.

### Code references to inspect

14. `tests/smoke/run_smoke.py` — the existing smoke runner. Stage 2.E will likely be invoked via this OR a new sibling script. Understand the runner's mission-launch + duration + artifact collection mechanism before designing the new harness.

15. `tests/smoke/README.md` — tier definitions, baseline-update rules, canary limitations. The "menu canary is desktop-bound and screen-coordinate-bound" caveat (per `feedback_smoke_no_canary.md`) is a CRITICAL CONSTRAINT — your harness MUST NOT inherit that limitation. Whatever screenshot capture mechanism you use must be display-independent and CI-stable (or have a clear-eyed limitation note).

16. `tests/smoke/baselines/` (if it exists; was noted as untracked in earlier worktree status) — possibly existing scaffolding for visual baselines. Investigate.

17. Search for prior Stage 1.E work (slice 1):
   ```bash
   grep -rn "Stage 1\.E\|object_visual_diff\|pinned.camera\|pinned-camera" docs/ tests/ scripts/ 2>/dev/null | head -20
   grep -rn "Stage 1\.E\|object_visual_diff" docs/superpowers/plans/ docs/superpowers/specs/ 2>/dev/null | head -20
   ```
   If Stage 1.E for slice 1 (static-prop batcher) has already landed, you reuse the harness. If not, you build it from scratch and the slice 1 work can also use it.

18. Existing screenshot infrastructure in the engine: grep for `glReadPixels`, `screenshot`, `dumpFrame`:
   ```bash
   grep -rn "glReadPixels\|screenshot\|dumpFrame\|capture.*frame\|saveFrame" GameOS/ mclib/ code/ 2>/dev/null | grep -v "\.o:" | head -20
   ```
   Engine may already have a screenshot mechanism (e.g., for menu canary, asset-scale verification, etc.). Reuse if present.

---

## Stage 2.E scope (single sentence)

Land `tests/smoke/object_visual_diff.py` (or equivalent) plus any engine-side hooks needed for deterministic camera positioning, capable of running pinned-camera screenshot capture twice per mission (baseline `MC2_GPU_OBJECTS=0` + test `MC2_GPU_OBJECTS=1`), diffing the captures with ≤0.5% / ≤2 LSB tolerance, and producing a pass/fail verdict.

---

## What Stage 2.E explicitly does NOT do

- **Default-on flip.** That's a SEPARATE PR after 2.E gate passes. Stage 2.E ships behind whatever invocation the user explicitly calls; nothing changes in the runtime default.
- **Modify substrate (Stages 2.A-2.C) or parity (Stage 2.D).** The substrate is locked. The parity check is locked. Stage 2.E is purely tooling; if you find yourself editing `gos_static_prop_batcher.{h,cpp}`, `gos_object_parity.{h,cpp}`, `mclib/{tgl,msl,bdactor,genactor}.cpp`, `shaders/static_prop.vert`, or `shaders/include/lighting.hglsl` — STOP and surface. The fix scope for this stage is harness, not substrate.
- **Add new SSBO bindings, schemas, or shader uniforms** beyond what's strictly needed for camera pinning (which probably doesn't need any).
- **Modify the existing smoke harness** beyond minimal additions. If you need to extend `run_smoke.py`, prefer adding a sibling script that calls into shared utilities.
- **Capture screenshots of UI / HUD / menu states.** Scope is in-mission render output (terrain + objects + sky + post-process). HUD content is non-deterministic across runs (clock, mouse position, etc.) and must be excluded from the diff region.
- **Cover non-stock missions.** Per `feedback_offload_scope_stock_only.md`, mod content (Carver5O, Magic, MCO, Wolfman, MC2X) is out of scope. tier1 + tier2 stock only.

---

## Stage 2.E load-bearing rules (carried forward from 2.D)

### 1. Hardened stop rules (non-negotiable)

> **If any baseline/test screenshot pair shows visual divergence beyond threshold, STOP IMMEDIATELY. Do NOT diagnose. Do NOT patch. Do NOT commit. Surface the first divergent mission + a side-by-side example to the controller.**

> **If the harness produces inconsistent results across multiple runs (run-to-run variance > the per-run tolerance), STOP. Surface the variance data — the harness needs determinism work before it's a useful gate.**

> **If you discover any new substrate bug while implementing the harness, STOP and surface BLOCKED.** Do NOT silently fix substrate from 2.E findings — that was the Stage 2.D.2 process violation. The harness's job is to surface findings, not to fix them.

### 2. Determinism is the load-bearing requirement

A pinned-camera diff is only useful if the camera is in the SAME WORLD POSITION + ORIENTATION across baseline and test runs. MC2 has multiple sources of non-determinism:
- AI/RNG state (mission-script driven; varies run-to-run)
- Mission timer (some events fire on real-time, not frame count)
- LOD selection (camera-distance-driven; if camera position drifts, LOD swap differs)
- Wind / particle / cloud / fog state (animated)
- Mech and vehicle positions (post-mission-load AI movement)

Your camera-pin mechanism MUST snapshot AS EARLY AS POSSIBLE post-mission-load (before AI runs, before missions tick) AND at a FIXED frame number, NOT real-time. Frame-driven capture is the discipline that worked for parity-check frame-N timing in Stage 2.D; reuse it.

Suggested pin point: frame N (where N ≈ 60-120) immediately after `primeMissionTerrainCache` completes and the first eligible-actor frame fires. Camera position should be set to a deterministic per-mission default (e.g., the mission's spawn-camera coordinates as written in the mission FIT/script).

### 3. Determinism failure-mode catalog

Likely sources of run-to-run variance the harness must defend against:
- **Wind clock**: animated terrain clouds, grass, water surface — driven by frame counter, deterministic IF capture is frame-driven not time-driven.
- **AI state**: at frame ≥1, AI may have already started moving units. Capture at frame 0 if possible OR before any AI tick fires.
- **Initial LOD selection**: depends on camera-to-actor distance; deterministic if camera is pinned.
- **Mission script timers**: some scripts trigger on `gameTime > N` even on frame 0. May need to disable mission script execution during capture, OR pin the capture before script init.
- **Floating-point determinism**: lighting math accumulating slightly differently between runs (rare on the same hardware/driver but possible). Build the harness to tolerate this via the spec's ≤2 LSB tolerance.

The harness's first capture pass should validate determinism by capturing the SAME mission TWICE in the SAME config (e.g., baseline run twice) and confirming the two captures match within tolerance. If they don't, the harness's pinning isn't sufficient yet.

### 4. Display-independence (per `feedback_smoke_no_canary.md`)

The menu canary is "desktop-bound and screen-coordinate-bound to the recording environment." The Stage 2.E harness MUST NOT inherit this. Acceptable mechanisms:
- Engine-side `glReadPixels` from the post-process FBO, written to PNG via a built-in or sidecar PNG writer.
- Headless-mode rendering (if MC2 supports it; investigate before assuming).
- Frame buffer capture via a debug command at a fixed frame.

NOT acceptable:
- OS-level screen capture (depends on display resolution, window decorations, multi-monitor layout).
- Any mechanism that requires user input or mouse positioning.

### 5. Tolerance specifics

Spec says "≤0.5% pixels diffed by ≤2 LSB." Decompose:
- **Per-pixel comparison**: each channel (R, G, B, optionally A) compared independently. A pixel "differs" if ANY channel exceeds ±2 LSB tolerance.
- **Pass criterion**: count(differing pixels) / count(total pixels in diff region) ≤ 0.005.
- **Image dimensions**: at 1920×1080 = 2,073,600 pixels, 0.5% = 10,368 pixels. At 800×600 = 480,000, 0.5% = 2,400 pixels.
- **Diff region**: should EXCLUDE HUD overlays. The post-process FBO captures the scene; if HUD is composited on top, capture BEFORE HUD composite (e.g., from `bShadersDrawPathEnabled` post-render but pre-HUD).

If the implementation can't easily exclude HUD, capture full frame and mask out the HUD pixel ranges with a known fixed mask per resolution.

### 6. Carry-forward from 2.D's process discipline

- **Verify-before-fix.** If the harness produces a diff failure, dispatch a recon to understand the cause before changing code. Don't trust hypothesis chains — get runtime evidence.
- **Adversarial review of the plan.** Before writing code for the harness, write a brief plan and dispatch an adversarial review against `.claude/skills/adversarial-plan-review.md`. The harness has many design choices (capture timing, file format, mask handling, threshold tuning) that benefit from a code-grounded review.
- **Single-file commits where possible.** If the harness is a Python script + a tiny engine hook + a baseline image set, separate commits for each layer ease bisection.
- **No history rewrites.** Don't `git rebase`, don't `--amend`. Forward commits only.

### 7. cmake / smoke discipline

- Build via `cmake --build build64 --config RelWithDebInfo --target mc2` directly. NEVER `cmake -B build64`. NO discovery commands (`where cmake`, `find`, `which`).
- Smoke serial only. No menu canary. `--duration 10-20` for fast iteration; longer if camera-pin requires settling time.
- Per-file `cp -f` + `diff -q` to deploy target. NEVER `cp -r`.

---

## Implementation phases (suggested)

The handoff spec doesn't pre-split 2.E. Suggested decomposition for clean bisection:

### Phase 0 — Recon (NO CODE)

Read items 14-18 above. Specifically answer:
- Does slice 1's Stage 1.E exist? If yes, where, what's its shape, can slice 2 reuse it?
- What screenshot capture mechanism exists in the engine? `glReadPixels`? PNG writer?
- What's the camera-pinning mechanism? Engine debug command? Save-game state? Mission-script override?
- What does `tests/smoke/baselines/` contain (if anything)?

Output: a short recon doc at `docs/superpowers/explorations/2026-05-04-stage2e-recon.md` documenting what you found.

If Stage 1.E or comparable harness EXISTS, Phase 1 is "thin wrapper for slice 2." If NOT, Phase 1 builds the harness from scratch.

### Phase 1 — Camera-pin mechanism

Land the engine-side hook (or invoke an existing mechanism) that deterministically positions the camera at a known per-mission default at frame N. Verify by running the same mission twice with the same env config and confirming both captures are byte-identical (or within tolerance if the tolerance is non-zero).

**Gate:** harness produces deterministic captures within self-tolerance across two runs of the same config.

### Phase 2 — Screenshot capture + PNG writer

Add the capture mechanism that fires at the pinned frame N and writes the FBO contents to a PNG (or BMP / TGA / whatever's easiest with existing engine infra). Mask out HUD if needed.

**Gate:** captures fire at the right frame; PNG files land in the artifacts directory; image content is the rendered scene at the pinned camera.

### Phase 3 — Baseline capture + storage

Run baseline captures (`MC2_GPU_OBJECTS=0`) for tier1's 5 missions.

**Storage policy (advisor decision 2026-05-04): DO NOT commit binary
baselines yet.** Start with artifact-side / sidecar storage (e.g.,
`tests/smoke/baselines/visual/mc2_NN.png` written but `.gitignore`-d).
Once Phase 0/1 proves determinism is solid AND the final image count
is settled, revisit whether to commit a small curated tier1 baseline
set. Committed binary baselines become long-term repo weight; defer
that decision until you have the data to make it well.

Implications:
- Baselines are regenerable via the harness's capture mode (`--capture-baseline`).
- A fresh checkout can't validate the gate without running a baseline-capture pass first. Document this in `tests/smoke/README.md`.
- The harness's pass/fail report should clearly distinguish "baseline missing" from "baseline present, diff exceeds threshold."

**Gate:** tier1 baselines captured + stored sidecar; reproducible via the capture mode; clear regeneration path documented.

### Phase 4 — Diff algorithm + threshold check

Implement the per-pixel ≤2 LSB tolerance + ≤0.5% pixel-count threshold. Output:
- Pass/fail verdict per mission
- For failures: a "diff image" highlighting the offending pixels (red overlay on the test image)
- Aggregate report

**Gate:** diff produces pass/fail; sample manual run on tier1 with `MC2_GPU_OBJECTS=0` vs `MC2_GPU_OBJECTS=0` (same config twice) shows PASS; with `MC2_GPU_OBJECTS=0` vs `MC2_GPU_OBJECTS=1` shows the actual gate result.

### Phase 5 — Tier2 sweep + final gate

Capture tier2 baselines, run the full diff, surface result.

**Gate (this is the Stage 2.E gate):** all 29 stock missions show ≤0.5% pixels diffed by ≤2 LSB. If any mission exceeds threshold, STOP and surface — that's a real visual-fidelity divergence and 2.E's value just paid out.

---

## Build / deploy / smoke discipline (carried forward from 2.D)

**Build:** `cmake --build build64 --config RelWithDebInfo --target mc2` ONLY. NEVER `cmake -B build64`. Per `feedback_subagent_no_cmake_configure.md`.

**Deploy:** per-file `cp -f` + `diff -q` to `A:/Games/mc2-opengl/mc2-win64-v0.3/`. NEVER `cp -r`. Files for Stage 2.E will likely be `mc2.exe` + `mc2.pdb` (if engine hooks added) plus the new Python harness (deployed to `tests/smoke/`).

**Smoke discipline:** see Phase notes above; serial only, no canary, deterministic mission seeds.

**Pool peak comparisons:** same-mission baseline-vs-test only per `feedback_pool_peak_compare_same_mission.md`. Trust +0 destroys delta as the primary Gate-E proxy.

**Stock missions only** for validation per `feedback_offload_scope_stock_only.md`.

**Git:** never push. HEREDOC for commit messages. NEVER amend.

---

## Acceptance criteria (Stage 2.E gate ladder)

The Stage 2.E PR is ship-ready when:

1. `tests/smoke/object_visual_diff.py` (or equivalent) exists and is documented in `tests/smoke/README.md`.
2. Engine-side camera-pin + screenshot capture mechanism is wired (env-gated by something like `MC2_VISUAL_DIFF_CAPTURE=1` to keep default-off behavior unchanged).
3. Baselines for tier1 (5 missions) are captured + stored.
4. Diff algorithm with ≤0.5%/≤2 LSB tolerance is implemented + tested (idempotent same-config runs PASS; opposite-config runs produce a clear pass/fail).
5. Full tier1+tier2 (29 missions) diff PASSES with `MC2_GPU_OBJECTS=0` baseline vs `MC2_GPU_OBJECTS=1` test.
6. Determinism check: each mission runs twice in the same config and produces matching captures within tolerance.
7. Default-off invariant: with no env vars set, Stage 2.E adds zero overhead and zero behavior change to normal gameplay.
8. Documentation:
   - Update `tests/smoke/README.md` with the visual-diff harness section.
   - Update the Stage 2.E handoff (this file) with the completion note + final gate summary.
   - Memory note IF the implementation surfaces a load-bearing lesson worth durably recording.

**If acceptance criterion 5 fails** (i.e., visual divergence > threshold on any mission): STOP and surface. The substrate has a visual-only bug that parity didn't catch. That's a substrate finding, not a harness bug. Same discipline as Stage 2.D: stop-on-finding, recon-first, fix-with-narrow-scope.

**If acceptance criterion 6 fails** (determinism check): the harness's pinning isn't deterministic yet. STOP and surface — this is a harness-design issue, not a substrate issue.

---

## Out of arc (do not pursue without separate brainstorm)

- Default-on flip of `MC2_GPU_OBJECTS` (separate PR after Stage 2.E gates pass).
- Mech offload / GV offload (a different CPU→GPU substrate arc; should reuse the parity infrastructure pattern from Stage 2.D and the visual-diff harness from Stage 2.E).
- GPU shadow port for static props (separate slice or arc).
- Removing legacy `g_useGpuStaticProps` and the 5 cull-bypass sites.
- Anything that touches substrate (`mclib/tgl.cpp` lighting kernel, shader lighting math, etc.).

---

## When you finish

**Stage 2.E PR:**
1. tier1+tier2 visual diff PASSES at threshold (≤0.5% / ≤2 LSB).
2. Determinism check PASSES (same-config repeat captures match).
3. Default-off behavior unchanged (verified via tier1 unset smoke 5/5 PASS, +0 destroys).
4. Baselines committed (or sidecar-stored with a clear regeneration path).
5. Documentation updated (README + this handoff completion note + optional memory note).

**Stage 2.E completion docs commit:**
```
docs(objects): stage 2.E complete — pinned-camera visual diff gate passed

[body explaining: harness shape, baseline storage decision, gate results,
 how to regenerate baselines, what 2.E unblocks]

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

**After Stage 2.E lands:** the slice 2 default-on flip is unblocked. The flip itself is a separate (small) PR — likely a single-line change in the env-gate default + a smoke run to verify nothing broke under default-on.

---

## Hand-off

Stage 2.E is **tooling**, not substrate. The substrate is settled. The work is:
- Camera determinism (the hardest engineering problem)
- Screenshot capture (mostly mechanical)
- Diff math (trivial)
- Baseline storage (judgment call on git-vs-sidecar)
- Tier1+tier2 pass/fail (the gate)

If the work surfaces a visual-only substrate bug, Stage 2.E paid out — that's exactly its job. Stop, surface, recon, fix; repeat the Stage 2.D discipline.

If the work goes smoothly, Stage 2.E is a 1-2 day arc and unlocks default-on flip.

Execute the phases in order. Surface real blockers to the user; never paper over them with hacks or skipped tests.

Read `parity_finds_gpu_substrate_bugs_visual_smoke_misses.md` once more before starting. Stage 2.E is the opposite-direction safety net. Same discipline.
