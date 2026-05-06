# Handoff prompt — Shape A `TexResolveTable` (M0a) implementation

Copy the block below into a fresh session at the worktree root.

---

You are implementing **Shape A `TexResolveTable` (ModernTerrainSurface M0a)** in this worktree. Spec, plan, memory, and worktree CLAUDE.md were finalized in a prior session and are now your inputs. Do not re-litigate the design — it has been through three rounds of advisor review and is locked.

**Working directory:** `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev` (branch `claude/nifty-mendeleev`).
**Build:** `RelWithDebInfo` via the `/mc2-build` skill. **Deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.2/` via `/mc2-deploy`. **Smoke:** `py -3 .claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing`. Game runs from the deploy dir (NOT the source tree — memory `feedback_deploy_path.md`).

## Required reading (in order, before any code)

1. **Plan (the source of truth for what to do):** `docs/superpowers/plans/2026-04-27-modern-terrain-tex-resolve-table.md`. Read end-to-end; it is task-decomposed and contains the complete header + cpp body to copy into the sidecar. Note the round-2 and round-3 advisor-correction blocks at the top — those describe the design's load-bearing safety properties; do not "simplify" any of them away.
2. **Spec (the why):** `docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md`. Read §3 (audit), §6.3 (why lazy not eager not flag-gated), §10 risk register, §11.1 measurement threshold.
3. **Worktree CLAUDE.md:** the "Critical Rules" section starting with the new "Stock install must remain playable" rule, plus "Debug Instrumentation Rule for reworks" and "Smoke Gate."
4. **Memory hits to skim before any keystroke:**
   - `mc2_texture_handle_is_live.md` — the C10 rule the design rests on.
   - `feedback_deploy_path.md` — deploy to v0.2, not run/.
   - `feedback_subagent_deploy.md` — if you spawn a subagent to build, it must also deploy.
   - `stale_shader_cache_symptom.md` — heads-up if you see weird visuals after deploy.
   - `stock_install_must_remain_playable.md` — Shape A is compliant; do not regress it.

## Execution mode

Use **`superpowers:executing-plans`** (inline, batch with checkpoints) per the operator's recommendation. The plan is structured for two commits:

1. **Implementation commit** (Tasks 1–3): sidecar `mclib/tex_resolve_table.{h,cpp}` + begin/end wiring (`mclib/terrain.cpp` top of `Terrain::geometry`, `code/gamecam.cpp:242` after `renderLists()`, defensive at `code/simplecamera.cpp:209`) + 27 mechanical callsite conversions + instrumentation. The plan contains exact code blocks for the sidecar header and cpp body — copy them, do not re-derive.
2. **Closing-report commit** (Task 4): validate-mode logs + Tracy A/B table + smoke gate results + grep-only residual census + promote/hold/revert recommendation. Do NOT commit `.tracy` binary snapshots; reference by SHA-256 in the report.

Constraints (from operator sign-off, plan header):

1. First implementation default OFF — `MC2_MODERN_TEX_RESOLVE` env defaults unset.
2. `beginFrameTexResolve()` + `endFrameTexResolve()` + `tex_resolve()` + 27 conversions in **one** commit with instrumentation (not split).
3. Convert §7.1 callsites mechanically — exhaustive list in the plan's Files section. Do not touch any §7.2 site (notably **not** `txmmgr.cpp:1228` Shadow.StaticAccum, **not** the non-terrain `renderLists` arms at 1114/1125/1429+, **not** crater/mech/UI sites).
4. Run validate mode (`MC2_MODERN_TEX_RESOLVE_VALIDATE=1`) before any default-ON decision — covered by Task 4.
5. Residual-call census is **grep-only** — diff `baseline-callsites.txt` vs `residual-callsites.txt`. No per-callsite runtime counters.
6. Static-shadow `txmmgr.cpp:1228` opt-in is a **separate** future commit, not part of this plan.

## Specific gotchas the prior session learned the hard way

- **Phase ordering.** `Terrain::geometry` runs in `mission->update()`, NOT in `GameCamera::render`. The init *must* be at top of `Terrain::geometry` so per-quad setup-time reads see an initialized table. The end *must* be after `mcTextureManager->renderLists()` so `frameActive` is cleared before any out-of-window inline-accessor caller.
- **`MC_TextureNode` has NO `flags` field.** `MC2_ISTERRAIN` lives on `MC_VertexArrayNode::flags`. Don't write code that assumes terrain-flagged texture nodes — there's no such thing. The table is index-keyed and domain-agnostic.
- **Validate mode runs both paths every call,** not first-touch only. The §7.2 legacy callsites can trigger `CACHED_OUT_HANDLE` eviction on a node the table memoized earlier in the same frame; first-touch validation would miss this class. Self-heal on mismatch by overwriting the table entry with the legacy result.
- **`MC2_MODERN_TEX_RESOLVE_VALIDATE` implies `MC2_MODERN_TEX_RESOLVE`.** Single env var triggers bake mode. Don't require both.
- **Sentinel is `0xFFFFFFFFu`.** Memset with `0xFF` fills correctly. Validate-mode every-call comparison would catch any real legitimate handle that happens to equal this; AR7 in the spec covers the contingency.
- **`Terrain::geometry` once-per-frame check.** Plan Task 2 Step 5 has a validation observation: count `event=begin_frame` lines vs `event=summary frames=N` over a 60-second run. If begin-count ≫ frames, multi-tick catch-up loop is firing and the table memsets mid-render. Plan describes the gating fix — `bool s_geometryRanThisRender` static. Probably a non-issue, but watch for it on the first ON run.

## Out of scope for this session

- Brainstorming, spec rework, or design alternatives. The design is locked.
- Promoting default-ON. The closing-report commit ends with a *recommendation*; the operator decides whether to do the one-line follow-up flip.
- Folding in static-shadow accum or any §7.2 site.
- ModernTerrainSurface Shape B or Shape C.

## Reporting back

End the session with:
- The two commit SHAs.
- The promote/hold/revert recommendation from the closing report.
- Any followup questions the operator should answer before the next slice.
- A note on the once-per-frame validation observation: did `B == N` or did you have to add the `s_geometryRanThisRender` gate?

Begin by invoking the `superpowers:executing-plans` skill, then read the plan top-to-bottom before opening an editor. The plan tells you exactly what to do; trust it.
