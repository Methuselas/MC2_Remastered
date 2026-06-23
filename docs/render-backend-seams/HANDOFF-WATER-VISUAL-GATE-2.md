# HANDOFF — WATER-VISUAL-GATE-2 (caveman, subagent-heavy)

START HERE. Job = harden capture harness, then land water byte-A/B -> VISUAL_PROVEN,
then clear the other 3 pending passes. Subagent-heavy: dispatch `general-purpose`
agents for deep grep/recon/Write; `mc2-render-expert`/`mc2-shader-expert` read-only.

## WHERE
- Canonical worktree: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev` · branch `claude/nifty-mendeleev`.
- Preflight before edit: `py -3 tools/repo_intel/repo_query.py preflight --expect-branch claude/nifty-mendeleev --expect-root A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`
- Build from ISOLATED worktree, never nifty's tree (foreign WIP makes it unbuildable). Pattern:
  `git worktree add --detach A:/Games/mc2-<name> $(git rev-parse HEAD)` ; `mkdir kbuild` ;
  cmake `-G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=<nifty>/3rdparty -DCMAKE_LIBRARY_ARCHITECTURE=x64 -DSDL2_DIR=<nifty>/3rdparty/cmake -DSDL2_mixer_DIR=... -DSDL2_ttf_DIR=... -DMC2_IMGUI=ON -DMC2_ZLIB=<nifty>/3rdparty/lib/x64/zlib.lib -B kbuild/build64` ;
  build `--build kbuild/build64 --config RelWithDebInfo --target mc2 mc2_launcher` (need launcher for deploy).
- Deploy: `py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-v0.4c --source-root . --build-dir kbuild/build64/RelWithDebInfo --exe-name mc2.exe`
- ★ NEVER launch the game (smoke/capture) without asking. Normal smoke does NOT grab mouse; golden CAPTURE warps it (now cursor-restored). v0.4c currently = routed HEAD build `051946c3`.

## ARC CONTEXT (what this is)
Vulkan-prep: promote each render pass UNREGISTERED -> DESCRIPTIVE_REGISTERED -> ROUTED_BY_APPLYPIPELINE
-> VISUAL_PROVEN -> SPIRV_ELIGIBLE. Source of truth = `docs/render-backend-seams/pipeline-pass-coverage-ledger.json`
(+ `.md`), enforced by `scripts/check-pass-coverage.py` (in check-contracts.sh as `pass_coverage`).
Sibling checkers: `check-pipeline-key.py` (pipeline_key), `check-pipeline-desc.py` (pipeline_desc).
LEDGER NOW: SPIRV_ELIGIBLE=MechOpaque · VISUAL_PROVEN=Shadow{Terrain,StaticProp,Mech} ·
ROUTED+pending=VFX(nondeterministic_visual_gate_pending) + TerrainOverlay/TerrainDecal(pass_not_exercised_in_smoke)
+ Water(byte_ab_capture_pending) · ROUTED=StaticPropOpaque/Depth · DESCRIPTIVE=StaticPropAlphaTest ·
UNREGISTERED=Terrain(main solid), VegetationCards, PostProcess, MineStatic · DO_NOT_MODEL=UI, Picking, RoadsRunways.
proofStatus taxonomy (check-pass-coverage PROOF_LANDED vs PROOF_PENDING; guard FORBIDS VISUAL_PROVEN while pending):
 LANDED = {byte_identical, perceptual_ab, oracle_coverage}; PENDING = {nondeterministic_visual_gate_pending,
 pass_not_exercised_in_smoke, byte_ab_capture_pending}.

## STEP 1 — HARDEN THE HARNESS (do first; unblocks all 4 pending passes)
Files: `scripts/pipeline_visual_gate.py` (wrapper, PROFILES, `water` profile), `scripts/run_visual_capture.py`
(capture tool — has `--no-kill` + cursor save/restore already). Fix THESE 4 capture-infra bugs found this session:
1. **PID-scoped cleanup.** `--no-kill` skips the blanket `taskkill /F /IM mc2.exe` (good — never kill foreign,
   e.g. a concurrent `releases/mc2-win64-v0.5.0` instance), BUT it then leaves the run's OWN spawned mc2.exe to
   self-exit slowly; back-to-back runs pile up + starve captures. FIX: track the launched child PID(s) and kill
   ONLY those at run end (taskkill /PID, or Popen.terminate). Keep foreign instances alive.
2. **Single-pose bookmark silently fails to fire capture** (engine_capture_fired=True, present=False). A >=2-pose
   bookmark fires fine. FIX: either make single-pose work, or have the harness WARN/require >=2 poses. (Root cause
   in the engine's bookmark/capture iterator — `gos_visual_capture.cpp` / `MC2_VISUAL_BOOKMARK_CAPTURE`; recon it.)
3. **Multi-run `--runs N` out-dir collision.** run_visual_capture multi-run writes to `out_dir.parent/mc2_01_rN`
   (LEAF IGNORED), so two gate runs with different leaves but same parent clobber each other -> MISSING. FIX: make
   multi-run honor the full out-dir (e.g. `<out_dir>/rN`) so gate before/after dirs don't collide.
4. **Capture flaky under contention** (present=False) while a foreign mc2.exe runs. Mostly a consequence of (1);
   also prefer single-run capture for A/B (more reliable than multi-run here).
Acceptance for Step 1: pipeline_visual_gate dry-run still OK; a real water capture (>=2 poses) on the routed build
reproduces sha `cb5a700ebd00dd0b`; no foreign mc2.exe killed; cursor restored; no leftover child mc2.exe after.

## STEP 2 — LAND WATER BYTE-A/B -> VISUAL_PROVEN (fast once Step 1 done)
VERIFIED FACTS (banked): mc2_01 bookmark `tests/visual/bookmarks/mc2_01_water.json` pose `water_overview_high`
(pos[0,0,0] rot45 proj55 alt1900) = an ENTIRELY-WATER frame; ROUTED build captures it byte-stable
**sha cb5a700ebd00dd0b** (confirmed twice). WaterArmed [PIPELINE_BIND] = `depth=GreaterEqual cull=None frontFace=Ccw
polygonOffset=false`, fires ~once/frame. Deterministic combo (run_visual_capture applies): `MC2_SMOKE_MODE=1 +
MC2_SMOKE_FIXED_TIMESTEP=1 + MC2_SMOKE_SEED=0xC0FFEE` + sim-freeze at trigger frame (`mission.cpp:531`).
A/B recipe: BEFORE = revert the water edit (see below) -> build -> full-deploy v0.4c -> capture water_overview_high
(>=2 poses, hardened harness, single-run); AFTER sha = cb5a700e. Byte-identical -> set Water proofStatus=byte_identical,
status=VISUAL_PROVEN. RUN ONLY when NO foreign mc2.exe is up (`tasklist | grep mc2.exe`; check path via
`powershell Get-CimInstance Win32_Process -Filter "name='mc2.exe'"`).
The water routing to revert (for BEFORE): `gameos_graphics.cpp` renderWaterFastPath ~:3275, the
`applyPipeline(getPipelineDesc(RenderCore::PipelineId::WaterArmed), "WaterArmed")` call -> restore the 5 hand-set
lines: `glDisable(GL_CULL_FACE); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_GEQUAL); glEnable(GL_BLEND);
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);`.

## STEP 3+ — clear the other pending passes (reuse hardened harness)
- VFX (nondeterministic): fixture EXISTS — `MC2_FX_FORCE_SPAWN` (`code/warrior.cpp:4884-4927`, 8 mechs fire all
  weapons once, dmgDone=0) + ready bookmark `tests/visual/bookmarks/mc2_01_werewolf.json` (--trigger-frame 147
  --settle 4 ~ fire frame 151). Pass rows VfxBillboard/Tube/Mesh{Alpha,Additive}. Gate: byte (if fixed-clock makes
  it deterministic) else perceptual (`scripts/visual_compare.py` + `visual-tolerance-policy.json`) +
  `MC2_VFX_ORACLE_TUBE_COVERAGE` occlusion oracle (`gos_particle_bridge.cpp:699/779` = "pass drew" samples_passed).
- TerrainOverlay: needs a CEMENT-perimeter-map bookmark (no stock tier1 map has it; TCE / mc2-overlay-mask-1 maps). No code.
- TerrainDecal: needs a deterministic decal-force fixture (no crater from MC2_FX_FORCE_SPAWN dmgDone=0) — add
  `MC2_DECAL_FORCE_SPAWN` OR an oracle-coverage-only gate.
Recon doc: `docs/render-backend-seams/pipeline-visual-gate-harness-recon-1.md` (full reuse inventory + per-pass).

## REUSABLE PRIMITIVE for "did the pass draw?"
`MC2_VFX_ORACLE_TUBE_COVERAGE` wraps a draw in a GL occlusion query (SAMPLES_PASSED) — generalize per pass as a
non-pixel "pass rasterized N fragments" gate. The `[PIPELINE_BIND]` trace (MC2_PIPELINE_BIND_TRACE, default OFF,
whitelisted in run_smoke) confirms which pipeline row bound + its state.

## LESSONS / GOTCHAS (all bit me this arc — don't repeat)
- ★ deploy_payload SILENTLY skips the exe update if a concurrent mc2.exe from THAT deploy dir runs -> ALWAYS
  `md5sum deployed vs kbuild/.../mc2.exe` after deploy. A stale exe = false trace/capture results.
- ★ A FRESH deploy dir (deploy_payload to a brand-new path) gets the 157-file payload but NOT the game data
  (missions/FST) -> exe can't load a mission -> empty log / no capture. Deploy into a FULL install (v0.4c) or
  swap exe-only (but exe+payload shader-version skew can break it).
- ★ Each worktree has its OWN `tests/smoke/artifacts/` AND `tests/visual/` — grep the worktree you RAN from, not nifty.
  Smoke per-mission logs may be named by mission file, but tier1 logs here were `mc2_NN.log`.
- ★ NEVER kill a concurrent/foreign mc2.exe (other session). Check path with `Get-CimInstance Win32_Process`.
- ★ Water fast path is DEFAULT-ON (real latch `terrain.cpp:2779`, `gamecam.cpp:530`); `gameosmain.cpp:852`
  MC2_RENDER_WATER_FASTPATH is only a DIAG flag, NOT the gate.
- ★ run_smoke REPLACES subprocess env (Popen) + forwards only a WHITELIST of MC2_* vars — a new MC2_ trace/gate
  var must be added to the whitelist (MC2_PIPELINE_BIND_TRACE, MC2_GPU_PARTICLES, MC2_FX_FORCE_SPAWN, MC2_RENDER_WATER_FASTPATH already in).
- ★ No `{`/`}` in PipelineId/BlendMode enum-MEMBER comments — the regex checkers parse `enum...{...}` and truncate.
- ★ Any new PipelineDesc field must be added to check-pipeline-desc.py PIPELINE_DESC_FIELDS (positional, struct order)
  or pipeline_desc FAILs.
- ★ recon prose is a HINT, not authority — verify state from the actual gl* draw block (recon got water's blend+cull
  WRONG; got the water-fastpath default WRONG). Read the code.
- BlendMode is split: AlphaBlend / AdditiveOneOne (ONE/ONE, tube) / AdditiveSrcAlphaOne (SRC_ALPHA/ONE,
  billboard+mesh); legacy `Additive`(=ONE/ONE) is render-contract-bridge ONLY (forbidden in registered rows by checker).

## KEY FILES
- `scripts/pipeline_visual_gate.py` (harness wrapper), `scripts/run_visual_capture.py` (capture + --no-kill + cursor),
  `scripts/visual_compare.py` (perceptual), `tests/visual/bookmarks/mc2_01_water.json` (water, 2-pose),
  `tests/visual/bookmarks/mc2_01_werewolf.json` (VFX weapon-fire).
- `docs/render-backend-seams/`: pipeline-pass-coverage-ledger.{json,md} + check-pass-coverage.py;
  pipeline-visual-gate-harness-recon-1.md; per-pass slice docs (vfx-*, water-armed-*, terrain-overlay-decal-*,
  blendmode-additive-vocabulary-1, etc.).
- Routing call sites: gameos_graphics.cpp (shadow brackets, terrain overlay/decal :9732/9850/9924, water :3275),
  gos_particle_bridge.cpp (:1127 billboard, :767 tube), gos_vfx_mesh_bridge.cpp (:311), pipeline_binder.cpp (applyPipeline),
  RenderCore/PipelineRegistry.{h,cpp} (PipelineId enum + s_descs), RenderCore/PipelineDesc.h (BlendMode/DepthFunc/etc).

## COMMITS THIS ARC (recent -> older)
efc9240f water-gate progress · 7adbe73b harness foundation · a0dadfd8 harness recon + water default-on fix ·
3159e7db water routing · b8250999 terrain overlay/decal routing · a4dea8ac vfx routing · 2f8f371c blendmode split ·
e135d714 vfx-route recon · 81a31095 vfx registration · ... (see git log render-seams).

## DISCIPLINE
Build isolated-worktree only; stage ONLY your files; verify foreign WIP md5 unchanged
(`mclib/mech3d.cpp`, `mclib/txmmgr.h`, `code/ablmc2.cpp`, `tests/visual/golden-sets.json` [has live ub201-pre-mc2_17
foreign WIP], `mclib/assimp_importer.cpp`, `mclib/mech_skel_import.cpp`). check-contracts.sh: the 4 pre-existing
fails (env_registry, include_firewall, no_raw_gl_from_game, render_contract_gbuf1) are foreign-WIP, ignore.
Update the ledger proofStatus HONESTLY — never claim VISUAL_PROVEN without a landed gate (the checker enforces it).
Memory: `~/.claude/projects/A--Games-mc2-opengl-src/memory/spirv-consumer-pilot-working.md` (full arc tail).
