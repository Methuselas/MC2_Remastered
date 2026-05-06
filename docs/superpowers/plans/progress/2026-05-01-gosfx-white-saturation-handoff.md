# gosFX/MLR HDR or GL-state feedback bug — fresh-session investigation handoff — 2026-05-01

> **Status:** intermittent recurring bug. Root cause unknown but the search space is now much smaller than the original handoff implied. Two sessions of investigation have eliminated 4+ hypotheses and demoted "composite" and "HUD batch" from suspects to amplifiers. The next session has a focused 5-step diagnostic script with a clear gating data point (RenderDoc pixel float reads).
>
> **Stop thinking of this as bloom.** Bloom is default-off and ruled out as a cause. The likely root is in **gosFX/MLR particle rendering or GL state leakage around it**.
>
> **Renamed 2026-05-01 sess 2** from `2026-05-01-bloom-particle-bug-investigation-handoff.md`. The old file path is a thin redirect.

This is a **self-contained handoff prompt for a fresh Claude Code session**. Paste the entire document into a new session.

---

## TL;DR

A recurring intermittent rendering bug causes the screen to ramp to max brightness over ~1 second, with cursor leaving permanent trails in the dark area while rendering normally over the white-saturated area. User-confirmed correlation: the bug fires **when a gosFX particle effect is visible on screen**, specifically observed on mc2_05 with the large-scale power-generator electricity effect (electricity rising from pylons). mc2_24 (final tier1 mission, heaviest base coverage = most active emitters) is most prone to it.

The bug **pre-dates the indirect-terrain migration** — was present when terrain was still CPU-drawn. Two sessions of investigation have ruled out the bridge state hygiene, the bloom path, the god-rays path, the particle-FBO theories, and "composite alone" + "HUD batch alone" as sole causes. **Composite and HUD may amplify or reveal the bug, but they are probably not the root cause.**

The shortest correct name:

> **gosFX/MLR HDR or GL-state feedback bug** — Not bloom. Not indirect terrain. Not god rays.

---

## Worktree

`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. Branch `claude/nifty-mendeleev`.

**Required reading (start here):**

1. `memory/gosfx_white_saturation_bug.md` — full symptom catalog, eliminated hypotheses, decision table, code locations to read. The canonical source.
2. `CLAUDE.md` "Critical Rules" + "Debug Instrumentation Rule" sections.

Recent relevant commits (from earlier sessions):
- `e22fa3a` fix(terrain-indirect): slot-keyed cement layer lookup + pure-cement-only filter
- `1a130b0` docs(plan): v2.3 — engine cement-flag fix + 16-bit layer encoding
- `962b15e` feat(terrain-indirect): widen cement layer encoding from 8 to 16 bits
- `f60f5dd` fix(terrtxm): zero-init TerrainTXM::flags after bulk memset(-1)

Session 2 (2026-05-01) made no commits — all diagnostic builds were reverted to baseline.

---

## Symptom catalog

- Screen ramps to max brightness over ~1 second, then resets, then re-triggers in cycles
- Cursor "solitaire-ing": permanent trails of cursor positions accumulate in DARK regions; cursor renders normally in WHITE-saturated regions
- Triangle / wedge-shaped white area expanding across the screen
- HUD widgets themselves render correctly because they overdraw with standard alpha; the bug is in surrounding pixels
- Region-dependent: white area receives fresh writes every frame; dark area is not cleared between frames
- "Things fade after 4-5 frames" of cumulative additive writes
- mc2_24 + mc2_05 most reproducible
- Visible only while certain particle effects are on screen (user-confirmed via mc2_05 power-generator pylon electricity sync)

---

## Hypotheses ELIMINATED (do NOT retrace)

By RenderDoc bisection + diagnostic builds across 2 sessions:

- **gosFX has separate FBO that doesn't clear**: dead. No per-effect FBO; particles draw via `gos_DrawTriangles` into the currently-bound FBO (`sceneFBO_`).
- **bloom ping-pong accumulator**: dead. `bloomEnabled_(false)` default at gos_postprocess.cpp:37; `runBloom` early-returns. Bloom code is irrelevant despite the historical name.
- **god rays additive composite**: dead. `godrayEnabled_(false)` default at gos_postprocess.cpp:83.
- **Particle FBO depth-test interaction**: dead, same reason as the FBO-not-cleared theory.
- **Indirect-terrain bridge state hygiene**: ruled out in session 1 — defensive fixes (force `glColorMask(TRUE,...)`, `useCementAtlas=0`, `glDisableVertexAttribArray(0)`) had no effect.
- **Composite shader is sole writer**: tested via passthrough composite (`FragColor = vec4(0.4, 0.4, 0.4, 1.0)` ignoring sceneTex). White-out STILL appeared. Composite alone is NOT the cause — at most an amplifier.
- **HUD batch is sole writer**: tested via complete `flushHUDBatch` early-return (no HUD draws at all). White-out STILL appeared (and HUD widgets vanished as expected). HUD batch alone is NOT the cause — at most an amplifier.
- **Tonemap-enable as fix**: ACES on its own is insufficient. Even with tonemap on, the sunset filter at postprocess.frag:103-125 multiplies post-tonemap output up to ~1.10 in R channel which clamps to 1.0 = warm-white. Reverted.
- **flushHUDBatch AlphaMode override** (force `OneOne`→`AlphaInvAlpha` per HUD call snapshot): didn't fix the bug. Reverted.

---

## Live theory after session-2 reframe

The strongest unresolved contradiction is:

> The screen ramps over ~1 second, resets, and re-triggers — but `sceneFBO_` is supposedly cleared every frame.

Four refined sub-hypotheses for the ramp:

1. **`sceneFBO_` is not actually clean after clear.** Wrong FBO bound, scissor / color-mask / draw-buffer / viewport state leaked, or clear not hitting the intended attachment.
2. **The particle/effect path writes insane HDR values every frame.** No cross-frame accumulation required; the "ramp" could be the effect animation itself increasing visible additive coverage/intensity.
3. **There is a hidden temporal feedback path.** Some buffer or texture from the previous frame is being sampled/reused indirectly (TAA-style, copy-back, etc.).
4. **The apparent ramp is not framebuffer accumulation.** It may be the power-generator effect's own cyclic animation — particles/electricity rise, saturate, disappear, repeat.

The next session should avoid broad code edits and do **one tight RenderDoc/instrumentation fork**.

---

## Best next diagnostic — DO THIS FIRST

Capture a bug-active frame and **read actual pixel values, not thumbnails**.

### Decision table

| Observation | Meaning |
|---|---|
| `sceneColorTex_` is dirty immediately **after clear** | Clear/state/FBO binding bug (sub-hypothesis 1) |
| `sceneColorTex_` is clean after clear but HDR-hot before composite | Particle/MLR additive output is root (sub-hypothesis 2) |
| `sceneColorTex_` values are sane, but FBO0 becomes white after composite | Composite/postprocess path still involved |
| FBO0 is sane after composite, then white after HUD | HUD/state snapshot path involved |
| All single-frame values sane but ramp appears over multiple captures | Hidden temporal feedback or effect animation cycle |

### Critical RenderDoc gotcha (gating constraint)

The Texture Viewer **auto-normalizes display range** based on the visible region's histogram. A texture that LOOKS clean may actually contain HDR pixels >> 1.0. The next session needs **Pixel Context float values** OR the **Range slider set above 1.0 / 10.0**. A "looks clean" thumbnail is not enough — session 2 was misled by exactly this and burned multiple iteration cycles before realizing.

To check actual values: pick the texture, drag the **Range slider** (top of Texture Viewer) right-side from 1.0 to 10.0+, OR right-click a pixel and read the **Pixel Context** float values directly. Session 2 never successfully extracted a pixel float value — that's the gating data point for next session.

### Recommended next-session script

1. Reproduce on `mc2_05` with the power-generator effect visible.
2. Capture one frame when the white ramp is active.
3. In RenderDoc, inspect pixel float values in:
   - scene color immediately after scene clear
   - scene color after gosFX/effect draw
   - scene color before composite
   - FBO0 after composite
   - FBO0 after HUD
4. Compare one white-region pixel and one black/trail-region pixel.
5. Only then patch.

---

## One code-side probe worth doing (only if RenderDoc inconclusive)

Add an env-gated `MC2_FBO_TRACE=1` (NOT `BLOOM_TRACE` — the name should not perpetuate the misframing). Log/optionally assert these immediately before the main scene clear AND immediately after gosFX/effect rendering:

```cpp
GL_DRAW_FRAMEBUFFER_BINDING
GL_READ_FRAMEBUFFER_BINDING
GL_VIEWPORT
GL_SCISSOR_TEST / GL_SCISSOR_BOX
GL_COLOR_WRITEMASK
GL_DEPTH_WRITEMASK
GL_STENCIL_WRITEMASK
GL_DRAW_BUFFER / GL_DRAW_BUFFERS
GL_BLEND
GL_BLEND_SRC_RGB / GL_BLEND_DST_RGB
GL_DEPTH_TEST
```

Also add one temporary diagnostic build that force-resets clear state before the main scene clear:

```cpp
glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
glViewport(0, 0, width, height);
glDisable(GL_SCISSOR_TEST);
glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
glDepthMask(GL_TRUE);
glStencilMask(0xFF);
glClearColor(0, 0, 0, 1);
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
```

If that fixes it, leaked GL state is the root cause. If it doesn't, focus on the particle/effect writer and HDR values (sub-hypothesis 2).

---

## Code locations to read

- `mclib/mlr/mlrstate.cpp:332`, `mclib/mlr/mlrsorter.cpp:652`, `mclib/txmmgr.cpp:1734/1746` — all 4 places that set `gos_State_AlphaMode = gos_Alpha_OneOne` (additive). The MLR particle path is the suspect; txmmgr.cpp does reset to `AlphaInvAlpha` at line 1841 (so txmmgr isn't the leak).
- `GameOS/gameos/gameosmain.cpp:404-490` — full per-frame render flow: beginScene → clear → UpdateRenderers → endScene → projectz_overlay → flushHUDBatch.
- `GameOS/gameos/gos_postprocess.cpp:283` — `sceneColorTex_` is `GL_RGBA16F`, can hold any positive value (HDR-capable).
- `GameOS/gameos/gos_postprocess.cpp:503-525` (beginScene), `:901-972` (endScene). Verified line numbers as of 2026-05-01.
- `mclib/gosfx/effect.cpp:688` (`Effect::Draw` dispatch), subclasses in `mclib/gosfx/{effectcloud,shapecloud,cardcloud,tube,...}.cpp`.

---

## Critical project rules

- Build: ALWAYS `cmake --build build64 --config RelWithDebInfo --target mc2`. Never plain Release.
- Deploy: NEVER `cp -r`. Use `cp -f` per file + `diff -q`. The `mc2-deploy` skill at `.claude/skills/mc2-deploy.md` automates.
- Smoke (focused investigation): `py -3 scripts/run_smoke.py --mission mc2_05 --kill-existing --duration 30 --keep-logs`. Single mission, 25-30s.
- Don't over-gate from transient observations. Per-frame visual variation under camera motion is expected; wait for clear "this consistently breaks" signals.
- See worktree `CLAUDE.md` for full critical rules.

---

## Anti-patterns (will earn rejection)

- **Don't pursue the indirect-terrain bridge state hygiene.** Already eliminated session 1.
- **Don't pursue "bloom" as a cause.** Default-off, ruled out session 2. Bloom code is dead-code worth deprecating LATER, but removing it now adds diff noise and risks hiding the real cause.
- **Don't spend more time on HUD-only or composite-only theories** unless RenderDoc proves the scene texture is sane. Both sole-writer tests in session 2 failed → these are demoted from "root cause" to "possible amplifier."
- **Don't iterate fixes without pixel data.** Session 2 burned multiple build/deploy cycles trying ACES enable, AlphaMode override, etc. None worked because the underlying mechanism was unverified. Get the pixel value first.
- **Don't add feature-slice complexity to fix this.** It's its own slice.
- **Don't ship a fix without verifying on BOTH mc2_05 (power-generator pylon area) AND mc2_24 (heavy base coverage).**
- **Don't conflate this with other intermittent bugs** (e.g., the "first-launch black/no terrain" issue in CLAUDE.md known issues — different intermittent class).

---

## Success criteria

- Root cause identified with grep/RenderDoc evidence (specifically a pixel float value that locates the offending pass).
- Fix lands as a single-purpose commit with clear before/after smoke results on mc2_05 + mc2_24.
- Memory note `memory/gosfx_white_saturation_bug.md` updated to "fixed in commit X" and reduced to a brief historical reference.
- White-screen / cursor-solitaire is no longer reproducible during 5 sequential mc2_24 runs.
