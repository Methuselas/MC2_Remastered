# Release / Train / Worktree Governance Manual

**Status:** Process codification (v1, 2026-06-11)
**Scope:** How we branch, merge, baseline, gate flags, deploy, and document on the MC2 OpenGL modernization project. This codifies practice already observed in `CLAUDE.md`, `docs/critical_inline_rules.md`, `docs/active_campaigns.md`, memory handoffs, and git history — it invents nothing new.

Sources cited throughout:
- `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md` (canonical worktree context)
- `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/critical_inline_rules.md`
- `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/active_campaigns.md`
- `A:/Games/mc2-opengl-src/.claude/engine-lane-separation-strategy.md`
- `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` (handoff index)

---

## 1. North star

Ship a modern GPU-driven renderer (chunk terrain, GPU cull, PBR, shadows) inside a 25-year-old game **without ever breaking the playable build**. Every change rides a train of verifiable gates: env-flag → A/B oracle → tier1 smoke → visual confirm → default-ON with opt-out → legacy deletion only after a baseline exists to detect regression. Process exists to make "it works on my branch" impossible to confuse with "it works in the deployed exe the smoke harness actually runs."

Corollary rules (from `critical_inline_rules.md`):
- Don't touch what you don't have to; when you must touch, bring to modern standard.
- Every cited file:line is grep-verified at write-time.
- Measure the FIX, not just the hotspot (pick-recon handoff lesson: a rect-prefilter "fix" was reverted as net-negative).

---

## 2. Branch / worktree topology

### One campaign = one worktree = one branch

Observed practice:
- Integration branch **`claude/nifty-mendeleev`** lives in worktree `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. It is the canonical trunk for the modernization arc ("0.4 gpu-driven-rendering arc merged 2026-05-18", worktree CLAUDE.md line 3).
- Feature campaigns get their own branch + worktree: `claude/terrain-gen-pcg` (editor pick/dock), `claude/gosfx-tube-ribbon-1` (Tube, staged in `.claude/glsg-tube-eyeson/`), `claude/perf-gpucull-ownership-1`, `claude/cook-m2a-merge`, `claude/model-override-system-recon-1` — all named in MEMORY.md handoffs.
- The repo **root checkout (`terrain-pbr-mod`) is STALE** — do not build, edit, or cite from it. Root `build64/` is stale too (CLAUDE.md "Key paths"). The root `CLAUDE.md` is a thin pointer only (see §7).

Rules:
1. New campaign → new `claude/<campaign-name>` branch in a fresh worktree under `.claude/worktrees/` (gitignored since `ca4d9784`). Build with the worktree's own `build64/`.
2. A worktree hosts exactly one campaign. Don't reuse a finished campaign's worktree for unrelated work — stale env, stale build artifacts, stale CLAUDE.md context all leak.
3. Subagents are dispatched **into the worktree path**, never the root (see §9, stale-root-edit trap).

### Integration branch role

`claude/nifty-mendeleev` is:
- The merge target for completed feature campaigns.
- The branch from which deploys to `mc2-win64-v0.4` / `0.4c` are built.
- The branch whose HEAD is recorded in every handoff ("Branch `claude/nifty-mendeleev` HEAD `<sha>`").

Feature → integration flow observed: `claude/terrain-gen-pcg` was **fast-forwarded** into nifty (`d02f063a` → `94db49a7`, +111 commits, 2026-06-10 handoff) once its campaign shipped; dirty WIP in the feature worktree was deliberately left untouched. FF is the preferred merge mode — it implies the feature branch was kept rebased/linear on nifty and that nifty had no divergent commits in the window. When FF is impossible, a merge commit on a `claude/<x>-merge` staging branch is used first (e.g. `claude/cook-m2a-merge` `5ecb3dc2`).

---

## 3. Merge windows and gates

A merge into `claude/nifty-mendeleev` requires ALL of:

1. **tier1 5/5 PASS on the feature branch HEAD** (see §8). Every shipped slice in MEMORY.md records this explicitly ("Tier1 5/5 PASS all commits").
2. **Oracle/parity counters clean** for the touched subsystem (FN=0, dropped=0, mismatches=0 — e.g. 8a/8b/8c terrain extraction shipped only with `maxVertexFN=0, maxRealBlockFN=0, maxWindowFN=0`).
3. **No open user-gated visual confirm** on the merged feature path (headless smokes cannot see pixels; "VISUAL CONFIRM PENDING" blocks default-ON, and default-ON blocks merge of dependent work).
4. **The merge window is open.** A window is *closed* when a blessing or capture activity is in flight. Current example (MEMORY.md, 8z closeout handoff): **"Do NOT yet: merge Tube, start GlStateGuard"** — the Tube merge is explicitly gated on **Baseline A** being captured off `mc2-win64-0.4c` first. Rationale: merging a large branch before the baseline is blessed would contaminate the golden frames' provenance.
5. **Review discipline** per `docs/disciplines.md` — adversarial-plan-review before large slices (mandated before GPU-cull M1, 2026-06-04 handoff), code review chain for shipped slices.

**Who decides:** the user (project owner) opens/closes merge windows and performs all visual-gate confirmations; agents may *propose* a merge but never execute one across a closed window. Deploy to the live game dir is likewise user-windowed ("v0.4 deploy PENDING (game-closed window)").

---

## 4. Baseline lifecycle

**Definition.** A baseline is a blessed reference capture taken off a *verified deployed build*, consisting of:
- **Golden frames** (screenshot captures on fixed camera paths / missions),
- **Per-pass GPU/CPU timings** (Tracy coarse per-pass zones — never sub-100ns; per-element zones forbidden, CLAUDE.md "Profiling"),
- **Oracle counters** (parity probes, FN/dropped counts, `MC2_*` instrumentation per `docs/tier1_env_vars.md`).

Current instance: **Baseline A**, to be captured off verified build **`mc2-win64-0.4c`**, post-terrain-8z, pre-GlStateGuard (MEMORY.md 8z closeout). Earlier `.claude/baseline-A-logs/` artifacts exist at repo root.

**When captured:** immediately after a major architectural closeout lands and is verified (8z made game terrain chunk/GPU-only → capture now, before the next destabilizing arc starts).

**When re-blessed:** a baseline is invalidated and must be recaptured when (a) a default-ON flip changes the rendered output intentionally, (b) the deployed reference build is rebuilt, or (c) the capture harness/oracle definitions change. Until re-blessed, comparisons against it are advisory only.

**Ordering rule:** baseline capture is a *merge-window-closing* event. Nothing that changes render output merges between "baseline scheduled" and "baseline blessed."

---

## 5. Feature-flag gate ladder (default-OFF → default-ON)

Every render-affecting feature climbs this ladder. Observed end-to-end on the terrain LOD chunk path (`MC2_TERRAIN_LOD_CHUNK`, cutover `a7b090be`) and on shadow lane, DrawPacket v7/v8, ViewUniforms, MaterialGpu (all in `docs/active_campaigns.md` "Infrastructure / permanent decisions"):

1. **Env-gated, default-OFF.** New path behind `MC2_<FEATURE>=1`. Zero cost when off.
2. **Shadow/A/B oracle producers.** Run new path *in parallel* writing shadow arrays only, compare against legacy production output, count false-negatives/mismatches (pattern: `MC2_TERRAIN_ACTIVE_AB`, `MC2_TERRAIN_SOLID_AB`, `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE`). Gate: FN=0 / mismatch=0 on tier1 AND on the adversarial fixture (e.g. the 1K oversized map).
3. **tier1 5/5 PASS flag-ON**, non-vacuous (verify the oracle actually exercised data — "legacy window 1k-9k, non-vacuous"; smoke missions are idle fly-throughs, so FX-type counters read zero there and need interactive runs).
4. **Visual confirm (user-gated).** Headless cannot see pixels. Launch script (e.g. `launch_lod.bat`), user confirms textured/lit/no-artifacts. Bisect visual bugs with diagnostic bitmask envs (`MC2_TERRAIN_LOD_CHUNK_DIAG`).
5. **Interactive gameplay sanity (user-gated).** Picking, gates/turrets, selection — things smokes never exercise.
6. **Flip default-ON with opt-out env.** Single-source gate function (e.g. `mc2TerrainLodChunkEnabled()`, opt-out `MC2_TERRAIN_LOD_CHUNK=0`; kill-switches `MC2_SHADOW_ENABLE=0`, `MC2_STATIC_PROP_LIVE_BUILDER=1`). Re-run tier1 5/5 with NO flags set.
7. **Soak, then delete legacy.** Legacy path survives a soak window behind the opt-out; deletion (the "8z" step) only after baseline capture + soak + a dependency-extraction audit (Phase 8 lesson: deletion was "NOT a pure deletion" — five hidden dependencies had to be re-homed first).

**Hard rule:** never delete the legacy path in the same slice that flips the default. Cutover commit and deletion commit are separated by a soak + baseline.

---

## 6. Deploy-target map + verification rules

| Target | Path | Runs |
|---|---|---|
| **Game (live)** | `A:/Games/mc2-opengl/mc2-win64-v0.4/` | `mc2.exe` — the user's game and the smoke harness exe |
| **Editor / verified-build line** | `mc2-win64-0.4c/` | `EditRel.exe` via `run-editor.bat` (captures stderr; launching the .exe directly does NOT); also the verified-build label for Baseline A |
| **sbs** | side-by-side compare install | editor deploys also land here (EditRel deploys to v0.4 + 0.4c + sbs) |

**The split-brain trap (codified as a rule):** building and deploying to 0.4c does **NOT** update the game exe. A prior session "fixed in 0.4c," left v0.4 stale, and burned a full debug cycle re-hitting the already-fixed 0xC crash (2026-06-08b handoff). Therefore:

1. After every fix, copy `build64/RelWithDebInfo/mc2.exe` to **each** target that will run it — explicitly, per target.
2. **Verify deployed exe mtime ≥ fix commit time** before testing or declaring a fix verified.
3. **Smoke runs the DEPLOYED exe**, not the build-tree exe (dynamic-pipeline-oracle handoff trap: `cp build64 mc2.exe` to v0.4 is part of the smoke workflow). A green build with a stale deploy is an untested fix.
4. Deploy mechanics per `critical_inline_rules.md`: never `cp -r` (silently fails on Windows/MSYS2) — `cp -f` per file + `diff -q`; **shaders deploy in lockstep with the exe**; full relink (`rm` exe + changed `.obj`, or `--clean-first`) when load-bearing/inline/class-layout code changes.
5. Build config is ALWAYS `RelWithDebInfo` (Release crashes with `GL_INVALID_ENUM`).

---

## 7. Stale-root-edit prevention

- Root `A:/Games/mc2-opengl-src/CLAUDE.md` is a **thin pointer** to the worktree CLAUDE.md and must stay that way. Enforcement: `sh .claude/worktrees/nifty-mendeleev/scripts/check-claude-md-pointer.sh` — exit 0 = pointer intact; nonzero = drift, revert. Run it before committing any change to the root file.
- Root checkout (`terrain-pbr-mod`) and root `build64/` are stale; all source work happens in the worktree (`CLAUDE.md` "Key paths").
- **Subagent dispatch rule:** every subagent prompt must contain the absolute worktree path; observed failure: "subagent edited stale ROOT mech3d.cpp not worktree" (dynamic-pipeline-oracle handoff). Verify a subagent's diff landed under `.claude/worktrees/nifty-mendeleev/` before accepting it.
- Project rules, build config, smoke gates, session notes never go in root CLAUDE.md — worktree CLAUDE.md is authoritative, kept under 100 lines, with detail extracted to `docs/` topic files.

---

## 8. Smoke tiers

Canonical invocation (verbatim, from worktree CLAUDE.md — subagents copy-paste):

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```

- **tier1** = 5 stock missions (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`), 30s each, idle fly-throughs. Exit 0 = pass; nonzero → inspect `tests/smoke/artifacts/<timestamp>/`. **Required (5/5) for: every slice gate, every merge, every default-ON flip.** Inner loop may use the 2-mission subset (`--mission mc2_01 --mission mc2_24`) but never as a gate.
- **tier3** = campaign/mod mission smokes (6 campaign missions: poar_01, torrin, clearwater, ruins, area16, coldstone — mc2x-compat handoff). **Required when** touching mission loading, packet files, asset resolution, mod VFS, or anything content-shaped; not required for pure render-path slices.
- Invariants: ALWAYS `--keep-logs`; NEVER `--with-menu-canary`; NEVER `--duration` >30s for gates (60s allowed for visual iteration only); NEVER concurrent with another smoke or mc2.exe trace; NEVER `--kill-existing` (taskkills concurrent mc2.exe → false `crash_silent`). Enforced by `scripts/check-smoke-matrices.py`.
- Smokes cannot validate: pixels (visual gates), interactive input paths (picking, pathing), weapon-fire FX (idle missions → FX counters zero). Those need user-gated runs (§5 steps 4-5).

---

## 9. Doc / handoff conventions

**Commit docs vs code:**
- Code slices commit with the slice (oracle + gate + implementation in reviewable commits; conventional-commit style: `feat(hitch):`, `fix(terrain):`, `docs(asset-viewer):` — see git log).
- Specs/plans land in `docs/superpowers/{specs,plans,strategy,recon}/` *before* implementation (e.g. `2026-06-09-h2-fastpath-disruption-recon.md`); plan-stage claims pass adversarial-plan-review.
- Ledger updates: `docs/active_campaigns.md` gets the one-paragraph shipped-slice entry with commit shas, gate env names, and kill-switch; permanent decisions move to its "Infrastructure / permanent decisions" section.
- No session narratives in CLAUDE.md; dated logs go to commit messages or memory files.

**Handoff docs:** every session that ends mid-arc writes a `HANDOFF_<date>_<topic>.md` in memory/ plus a one-block index entry in `MEMORY.md` containing: branch + HEAD sha, what SHIPPED (with shas), what is PENDING/BLOCKED and on what gate, explicit "Do NOT yet" list, and traps hit. Index entries stay under ~200 chars where possible (MEMORY.md is size-limited); detail lives in the topic file.

---

## 10. Trap rulebook

Codified from traps that each cost at least one debug cycle:

1. **`| tail` masks build exit codes.** In bash, `cmake --build ... | tail` returns tail's status. Use `PIPESTATUS[0]` or don't pipe the build. (Dynamic-pipeline-oracle handoff.)
2. **TaskStop orphans the mc2.exe child.** Stopping a background task that launched the game leaves mc2.exe running → next smoke sees a concurrent instance → false failures. Kill the child explicitly; never run smokes while any mc2.exe trace is live.
3. **`STOP(...)` is a no-op in RelWithDebInfo.** Never use it as a guard; it falls through (caused the GameMap==NULL READ-at-0x8/0xC crash family, 2026-06-08 handoff). Use real null guards + fallback synthesis.
4. **Stale-root subagent edits.** See §7 — absolute worktree paths in every dispatch; verify diff location.
5. **Deploy split-brain.** See §6 — verify mtime ≥ commit time on EVERY target.
6. **`cp -r` silently fails on Windows/MSYS2.** `cp -f` per file + `diff -q`.
7. **`PacketFile::writePacket(ANY_PACKET_TYPE)` compresses+decompresses per call** — use `STORAGE_TYPE_RAW` in per-element loops (editor save was O(areas+doors×2)×3 slow).
8. **Bolt-on GL passes inherit state.** Any new draw pass must set ALL state it depends on (depth test/mask, blend, cull) explicitly — the chunk-terrain transparency saga (`f375e0ba`) was inherited glDepthMask FALSE.
9. **GLSL does not inherit C++ build flags**; explicit-program uniform uploads (`glProgramUniform*`) when you hold a program id; shader hot-reload fails silently — check console. (All in `critical_inline_rules.md`.)
10. **Per-input expensive work hides as "render slow."** `Camera::inverseProject` on the cursor/scroll hot path caused multi-second editor freezes; precise math on click, O(1) approximation per-frame (`screenToGroundPlaneApprox`).

---

## 11. Anti-goals

- **No giant modularization branch** — no TerrainSubsystem/PropSubsystem file moves, no compile-independence quest (`engine-lane-separation-strategy.md` "What NOT to do"). One ELS slice retires one dependency.
- **No solving all coupling in one PR.**
- **No deleting legacy in the cutover commit** (§5).
- **No editor CPU fallbacks** — editor is a GPU-only test bed; "X breaks in editor" is fixed by making X work on the game's GPU path (`critical_inline_rules.md` "Editor discipline").
- **No wall-clock time estimates** in plans; complexity in code dimensions only.
- **No pushing to `alariq/mc2` origin** — all work local.
- **No emoji in any file.**
- **No micro-zones in Tracy** (<100ns / per-element forbidden).
- **No merging across a closed window** (Baseline A currently closes the Tube/GlStateGuard window).

---

## 12. First 5 process-improvement slices

1. **Deploy-verify script** (`scripts/check-deploy-fresh.sh`): given a commit sha, assert mtime of `mc2.exe`/`EditRel.exe` in v0.4, 0.4c, sbs ≥ commit time; wire into `/mc2-deploy` skill and run_smoke preflight. Retires trap §10.5 structurally.
2. **Merge-window state file** (`docs/merge-window.md` or a `.claude/MERGE_WINDOW` flag file): single line OPEN/CLOSED + reason + owner; pre-merge hook refuses FF into nifty when CLOSED. Makes the "Do NOT yet merge Tube" rule machine-checkable.
3. **Baseline manifest** (`tests/baseline/A/manifest.json`): build sha, exe hash, capture date, oracle env set, blessed-by; comparison tooling refuses to compare against an unblessed or hash-mismatched baseline.
4. **Gate-ladder checklist template** (`docs/superpowers/specs/_flag-ladder-template.md`): the 7 rungs of §5 as a literal checklist embedded in every new feature spec; ledger entry in `active_campaigns.md` links the filled-in copy.
5. **Smoke-runs-deployed-exe assertion**: run_smoke.py logs the hash of the exe it launched and compares against `build64/RelWithDebInfo/mc2.exe`; mismatch prints a loud `DEPLOY STALE` warning (not failure — intentional old-build runs exist, e.g. baseline capture).

## 13. Three follow-up prompts

1. "Implement slice 1 of the governance manual: write `scripts/check-deploy-fresh.sh` in the nifty-mendeleev worktree that takes a git sha and verifies mc2.exe/EditRel.exe mtimes in v0.4/0.4c/sbs are newer than the commit; integrate as a preflight into scripts/run_smoke.py behind `--check-deploy`."
2. "Audit docs/active_campaigns.md against MEMORY.md handoffs and git log for drift: the 'Current state' header says 2026-06-01 but terrain 8z closeout, pick/dock merge, and H1a shipped since — bring the ledger current and move closed items to shipped."
3. "Write the Baseline A capture runbook (docs/superpowers/specs/baseline-A-runbook.md): exact missions, camera paths, Tracy zone list, oracle env vars, golden-frame storage layout, and the blessing checklist — grounded in `.claude/baseline-A-logs/` and the mc2-render-state capture tooling."
