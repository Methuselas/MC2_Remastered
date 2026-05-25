# Making `MC2_GPU_CULL_LIFECYCLE=1` safe at normal zoom — design exploration

Date: 2026-05-07
Status: DESIGN/EXPLORATION ONLY — no source modified.
Worktree: `agent-a4ac146013248f12b` (forked off `claude/nifty-mendeleev` HEAD).
Authoritative project context: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md`.

Scope: enable `MC2_GPU_CULL_LIFECYCLE=1` by default at normal (non-wolfman) zoom on
`mc2_01` without the popping/vanishing failure flagged in
`memory/track_c_substrate_regression.md`. Out of scope: bucket-coalescing the
substrate `glDrawElementsIndirect` storm, making readback latency lower, or any
GPU-cull substrate work.

---

## 1 — Code grounding (grep-verified)

All paths absolute under
`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`.

### 1.1 Readback ring depth (corrects memory)

`GameOS/gameos/gpu_cull_readback.cpp:40`:

```
constexpr uint32_t RING_FRAMES           = 3u;
```

`s_readbackFence[RING_FRAMES]` declared at `gpu_cull_readback.cpp:75`. Ring
indexed by `s_currentSlot` (next slot to write a fence to,
`gpu_cull_readback.cpp:76`).

The three-tier consumption strategy is documented inline at
`gpu_cull_readback.cpp:5-7`:

```
T1: N-1 slot ready     → use it
T2: N-2 slot ready     → use it, emit readback_fallback_n2
T3: both not ready     → conservative (all visible), emit readback_fallback_conservative
```

So the actual ring exposes **two** previously-finished frames (N-1 and N-2),
with a T3 fail-open. RING_FRAMES=3 in memory `track_c_compute_cull.md` is
correct, but T3 is fail-OPEN (all visible), not "T3 fallback" — confirmed at
`gpu_cull_readback.cpp:382-386` (`event=readback_fallback_conservative`) and
`gpu_cull_readback.cpp:552-553` (`memset(s_actorVis, 1, ...)`).

### 1.2 Lifecycle-gate consumer

```
GameOS/gameos/gpu_cull_readback.cpp:635   bool readback_isActorVisibleLagged(uint32_t actorId)
GameOS/gameos/gpu_cull_readback.cpp:637   if (!readback_isEnabled() || s_lastGoodSlot == UINT32_MAX) return true;
GameOS/gameos/gpu_cull_readback.cpp:638   if (s_lastGoodVisibleCount == UINT32_MAX) return true;
GameOS/gameos/gpu_cull_readback.cpp:639   if (actorId == 0u || actorId >= MAX_ACTOR_HANDLE) return true;
GameOS/gameos/gpu_cull_readback.cpp:640   return s_actorVis[actorId] != 0u;
```

A flat `uint8_t s_actorVis[MAX_ACTOR_HANDLE]` (`gpu_cull_readback.cpp:546`,
`MAX_ACTOR_HANDLE = 4096u`).

### 1.3 Snapshot builder

```
GameOS/gameos/gpu_cull_readback.cpp:548   void readback_buildActorVisSnapshot(uint32_t maxActorHandle)
```

Walks the substrate slot **`(s_lastGoodSlot + 1u) % 3u`** to map record-array
indices back to `actorId` (`gpu_cull_readback.cpp:592`). Memory-default-fill
to all-visible at `gpu_cull_readback.cpp:553`; only writes 0 for actors
explicitly listed as invisible in the readback. Called once per frame at
`code/objmgr.cpp:1873`:

```
code/objmgr.cpp:1871   if (s_gpuCullLifecycle) {
code/objmgr.cpp:1872       // Max handle is bounded by maxObjects + slack; 4096 is safe for MC2 (~2000 max).
code/objmgr.cpp:1873       gpu_cull::readback_buildActorVisSnapshot(4096u);
code/objmgr.cpp:1874   }
```

### 1.4 `s_lastGoodSlot` ring-phase logic

```
GameOS/gameos/gpu_cull_readback.cpp:288   const uint32_t n1Slot = (s_currentSlot + RING_FRAMES - 1u) % RING_FRAMES;
GameOS/gameos/gpu_cull_readback.cpp:289   const uint32_t n2Slot = (s_currentSlot + RING_FRAMES - 2u) % RING_FRAMES;
GameOS/gameos/gpu_cull_readback.cpp:307   s_lastGoodSlot = n1Slot;     (Tier 1 path)
GameOS/gameos/gpu_cull_readback.cpp:349   s_lastGoodSlot = n2Slot;     (Tier 2 path)
GameOS/gameos/gpu_cull_readback.cpp:376-380  // M-4 stale-frame abandonment
```

`MAX_STALE_FRAMES = 10u` (line 90); after that many consecutive T3 fences,
`s_lastGoodSlot` resets to UINT32_MAX so subsequent calls fail-open.

### 1.5 `Mech3DAppearance::update` lifecycle gate site

Memory `track_c_compute_cull.md` cited `mech3d.cpp:4170`. **Verified line is
shifted** — actual lifecycle gate that consumes the lagged-visibility result
is `mclib/mech3d.cpp:4256-4262`:

```
4257     const bool gpuVis = s_gpuCullLifecycle
4258         ? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
4259         : inView;
4260     if ((turn < 3) || gpuVis || (currentGestureId == GestureJump) || g_useGpuStaticProps)
4261         updateGeometry();
```

`s_gpuCullLifecycle` defined at `mclib/mech3d.cpp:23`:

```
static const bool s_gpuCullLifecycle = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);
```

There are **fourteen** other `readback_isActorVisibleLagged` call sites in
`mech3d.cpp` (search for the symbol; the cluster spans lines 737, 785, 827,
871, 2318, 2322, 2436, 2944, 3806, 3866, 4258, 5179, 5222 — covering hit-shape
sampling, sensor-circle render, etc.). The 4256-4262 block is the load-bearing
`updateGeometry()` gate — that is the one whose miss causes
`TransformMultiShape` to be skipped while the actor is on screen.

### 1.6 `GVAppearance::update` lifecycle gate site

Memory cited `gvactor.cpp:2702`. **Verified line is shifted** — the actual
`updateGeometry()` lifecycle gate is `mclib/gvactor.cpp:2773-2779`:

```
2774     const bool gpuVis = s_gpuCullLifecycle
2775         ? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
2776         : inView;
2777     if ((turn < 3) || gpuVis || g_useGpuStaticProps)
2778         updateGeometry();
```

`s_gpuCullLifecycle` defined at `mclib/gvactor.cpp:78`.

### 1.7 `MC2_GPU_CULL_LIFECYCLE` env-var read sites

Five reads (one per consumer file):

```
mclib/mech3d.cpp:23     static const bool s_gpuCullLifecycle = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);
mclib/gvactor.cpp:78    static const bool s_gpuCullLifecycle = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);
code/objmgr.cpp:126     static const bool s_gpuCullLifecycle = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);
code/mech.cpp:134       static const bool s_gpuCullLifecycle = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);
code/gvehicl.cpp:118    static const bool s_gpuCullLifecycle = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);
```

All five resolve identically at TU init time — no central authority. Means a
fix landed in one file (e.g. mech3d) does not automatically apply to the
gvactor mirror; both must be patched.

### 1.8 Camera angular-velocity / motion data — DOES NOT EXIST

Greps `angularVelocity`/`deltaOrientation`/`cameraDelta`/`prevRot`/`prevPos`/
`lastCameraRotation` against `mclib/` and `code/` returned **zero hits** in the
camera path. The only `m_angularVelocity` matches are in `gosfx/`
(particle-system spin), unrelated.

`Camera::update()` at `mclib/camera.cpp:1507` reads `cameraRotation`
(scalar yaw degrees, set across `mclib/camera.cpp:1019-1035, 2950-2984`) and
`position` but never stores prior-frame values for delta computation. The
camera does have `velocity` (linear, `mclib/camera.cpp:1593-1604`) which is
the *goal* velocity from input pacing, not a measured frame-to-frame delta.

Implication: any solution that needs `|cameraDeltaOrientation|` per frame
must add the storage itself. Cheap (two floats: `prevCameraRotation`,
`prevCameraTilt` plus optionally `prevCameraPositionXY` for pan).

### 1.9 APPEAR_ROUTE / touch path

```
code/terrobj.cpp:113   uint64_t g_routeTouchByClass[256];
code/terrobj.cpp:114   uint64_t g_routeUpdateByClass[256];
code/terrobj.cpp:143-154   summary print every 600 frames
code/terrobj.cpp:726       resolve appearance class once per gate event
```

Memory `track_c_substrate_regression.md` line 20 says
`APPEAR_ROUTE class=0x0e/0x0f shows touch=0 with LIFECYCLE=0` — confirmed:
the `touch()` cheap path is gated behind `MC2_STATIC_UPDATE_SKIP=1`
(`code/terrobj.cpp:703-716` per
`docs/superpowers/specs/2026-05-06-static-prop-texture-cache-handoff.md:54`),
which is itself flagged as having an independent regression
(`memory/update_skip_touch_regression.md`). LIFECYCLE alone does not route to
`touch()`; it just skips `updateGeometry()` inside the existing `update()`.

---

## 2 — Quantifying the staleness window

### 2.1 Latency floor

At 60 FPS:
- T1 hit (N-1 slot): **~16.6 ms** stale (one frame back).
- T2 hit (N-2 slot): **~33.3 ms** stale (two frames back).
- T3 conservative: fail-open (no staleness, but no cull benefit either).

The compute pass writes its result inside frame N's draw list; the fence is
inserted at end of frame N. The earliest a CPU read can see frame N's result
is the start of frame N+1, and only if the GPU has already retired it
(fence ready). On AMD RDNA3 this is the common case at 60 FPS but not
guaranteed — hence T2.

### 2.2 Camera angular velocity at normal zoom

No instrumentation captures live camera angular velocity in the codebase
(see §1.8). However, MC2's mouse-edge pan / Q-E rotate are key-driven and
linear-time. From the camera-rotation transition path
(`mclib/camera.cpp:1011-1035`, `goalRotTime` linearly closes
`goalCameraRotation`), a mid-mission user yaw is on the order of
**45-90°/second** during pan; whole-screen rotates (Q/E) close at the input
rate of ~135°/second.

At 60 FPS that is roughly **0.75°-2.25° per frame**. With a horizontal FOV
near `MIN_PERSPECTIVE = 10°` at full zoom-in (camera.cpp:108) up through the
mid-30s at normal zoom and 88° at wolfman (camera.cpp:110), 1° of yaw is a
non-trivial fraction of the visible frustum.

### 2.3 Frustum churn at the edge

Mc2_01 typically holds 30-200 substrate records on screen at normal zoom
(`[GPU_CULL v1] event=substrate_ready records=N capacity=M`, range cited in
`memory/track_c_compute_cull.md` line 50). With ~50 visible actors and ~1-2°/
frame yaw, the leading frustum edge sweeps ~1-2% of the screen-aligned actor
strip per frame. The actors crossing the edge are a small absolute count
(estimate: 0-3 per frame on mc2_01, scaling with actor density), but each
miss is a visible pop.

The structural tightness: **at T1, the gate decision was made for a frustum
~16.6 ms ago and 1-2° rotated.** Any actor whose center crossed the frustum
boundary during that window is mis-gated:

- Edge-entering at frame N-1: gate=invisible, render=visible →
  `updateGeometry()` skipped → `TransformMultiShape` not run →
  `TG_Shape::Render` reads stale `listOfVertices` → silent vanish or pop.
- Edge-leaving at frame N-1: gate=visible, render=invisible → benign waste.

The first failure mode is what the regression note describes
(`memory/track_c_substrate_regression.md:19`).

### 2.4 Floor

No amount of pipelining brings T1 below ~16 ms unless the readback consumer
runs *after* the same frame's compute pass (i.e. CPU-stalls on the fence).
That is precisely what readback exists to avoid.

The staleness window is intrinsic. Any safe-at-normal-zoom design must either:
(a) admit more conservatively, (b) read CPU motion data to predict the
window, or (c) gate by camera state and accept giving back the wolfman win.

---

## 3 — Solution candidates

### (A) Conservative-OR dilation

Mark actor visible if ANY of (N-1 readback, N-2 readback, current substrate
record set) says visible.

Implementation locus: `readback_buildActorVisSnapshot` itself —
`gpu_cull_readback.cpp:548` walks the last-good slot's records. Extension:
also walk the *current* substrate slot (which has freshly emitted records but
no GPU readback yet) and mark every emitted actor visible. Then optionally
walk the slot one frame older than `s_lastGoodSlot`.

Pros:
- Single point of change (snapshot builder); no per-actor state.
- Eliminates the entering-edge mis-gate by definition (current substrate
  records every actor that emit-time legacy cull thought might be visible).

Cons:
- Effectively reverts to legacy `inView` semantics for newly-emitted
  actors. The substrate-record set is built from `obj->inView ||
  obj->canBeSeen`, which is the same predicate the lifecycle gate replaced.
  Net perf give-back is large for actors at the edge.
- Two-slot OR (N-1 ∪ N-2) on its own does not solve the entering case —
  both slots predate the new edge crossing.
- Substrate records carry the readback's actor IDs but not visibility
  truth — they are admission predicates, not GPU outputs. Treating
  "emitted" as "visible" is exactly the legacy bypass.

Verdict: insufficient on its own. The "walk current substrate" half is
*equivalent* to fail-open for newly-admitted actors. Cheap to implement but
returns most of the LIFECYCLE perf to the legacy path at normal zoom — at
which point LIFECYCLE adds zero value and is harmful (snapshot overhead
> savings, per `track_c_compute_cull.md`).

### (B) Predictive readback (CPU motion forecast)

Per actor, run sphere-frustum against the *current* camera, not the lagged
readback. Use that to fail-open if CPU prediction differs from N-1 lagged.

Cons (unrecoverable):
- Sphere-frustum on CPU is exactly what GPU compute cull replaced. Doing it
  per actor in the snapshot path costs ~`numActors` plane tests per frame.
  At 200 actors × 6 planes that is ~1200 ops, comparable to the snapshot
  walk itself; at 1000+ actors it dominates the savings LIFECYCLE was meant
  to deliver.
- Substrate radius is a placeholder (`memory/track_c_compute_cull.md:42`)
  and the CPU does not currently have ergonomic access to per-actor world
  AABB without re-deriving from `appearance->getExtents()`.

Verdict: rejected — defeats the LIFECYCLE-shipped value proposition.

### (C) Camera-motion-gated fail-open

Track `prevCameraRotation` and `prevCameraPosition` at end of `Camera::update()`.
Compute one float `cameraMotionScore` per frame. When score exceeds threshold,
short-circuit `readback_isActorVisibleLagged()` to return `true` for the next
N frames (where N = `RING_FRAMES + 1` to drain the pipeline).

Implementation locus:
- `mclib/camera.cpp` — append `g_cameraMotionFrames` (atomic) at end of
  `Camera::update()` based on `|rotation - prevRotation| > k_yawThreshold`
  or `|position - prevPos|.length > k_panThreshold`.
- `GameOS/gameos/gpu_cull_readback.cpp:635` —
  `readback_isActorVisibleLagged` checks `g_cameraMotionFrames > 0` and
  returns `true` (fail-open) when set; the snapshot builder zeros the
  `s_actorVis` to all-1 and skips the populate path on motion frames.

Pros:
- Single-commit change. ~15 lines C++. No actor-side state.
- Stationary camera (which is most of the time at normal zoom — see
  §6 deferred-to-spec) keeps full LIFECYCLE benefit.
- During pan/rotate, behavior is **byte-identical to LIFECYCLE=0**, the
  current safe default. There is no new failure mode introduced by motion
  frames.
- Threshold is tunable; conservative initial values
  (`k_yaw = 0.5°`, `k_pan = 8 world units`) are easy.

Cons:
- Wolfman-zoom regression: at wolfman zoom the camera also rotates during
  cinematic pans, but the visibility set is stable enough that LIFECYCLE
  was already a perf win there. Camera-motion-gating gives back the win
  during those pans. Mitigation: detect zoom level
  (`Camera::cameraZoom[currentView]`) and use a tighter motion threshold at
  high zoom-out — keeps LIFECYCLE active during wolfman cinematic.
- Threshold tuning is empirical. Below threshold = pop bug returns; above
  threshold = perf win lost. Needs a measurement pass to dial.

### (D) On-screen-history bit per actor

Per-actor 1-bit memory: "was this actor rendered last frame?" Set during
render, read by the lifecycle gate.

`s_actorVis[id]` becomes a small bitfield with `vis_history` and
`vis_readback` lanes. Lifecycle gate fires `updateGeometry()` if
`vis_history || vis_readback || current_substrate_emit`. An actor that was
rendered last frame stays in the update path for one more frame even if
readback says it just left the frustum — gives a one-frame "trailing window"
on the leaving edge. For the entering edge, it matches (A): only "current
substrate emit" catches it.

Pros:
- Very precise: only relaxes the gate where the actor was actually being
  used.
- No camera-side instrumentation, no global motion detection.

Cons:
- Requires a setter call from the render path. Mech3DAppearance::render and
  GVAppearance::render are the natural sites, plus Tree/Bldg if they later
  consume LIFECYCLE.
- Doesn't help the entering edge — that is the actual failure mode in
  `memory/track_c_substrate_regression.md`. (Leaving edge is benign: stale
  matrix on an off-screen actor doesn't cause a visible pop.)
- Two-commit minimum: one to add the setter to every render path that can
  consume it, one to gate the snapshot read on the bit. Each render path
  is a separate audit (mech, GV, future tree/building when LIFECYCLE
  expands). Easier to leave a hole than to find it.

Verdict: most semantically correct on the leaving edge, but the failure
mode in the regression note is the *entering* edge (camera rotates *toward*
an actor that was off-screen 2 frames ago). On-screen-history of N-1 is
**0** for that actor, so D does not help. Reject.

### (E) Camera-motion-gated fail-open + edge-band conservative-OR (hybrid)

Variant of (C). Same camera-motion gate, plus during stationary frames also
walk current-substrate emit set into the snapshot to catch the rare case
where an actor enters because of *its own* motion at a stationary camera.

Pros:
- Catches the AI-mover-into-frustum case (gunship flying in while camera
  is stationary) that pure (C) misses.

Cons:
- Slightly larger snapshot walk; almost certainly not measurable on
  mc2_01 actor counts. Adds a code path requiring the substrate slot
  ring-phase logic to be doubled (snapshot already walks `(s_lastGoodSlot
  + 1) % 3`; this would also walk `(s_currentSlot + RING_FRAMES - 1) %
  RING_FRAMES`).
- Substrate emit at frame N-1 was based on legacy-cull `inView`, which is
  the same admission gate the legacy path uses. So (E) collapses to "be
  legacy-correct on actors at the edge, GPU-correct on stable interior" —
  arguably the strongest correctness/performance pareto, but with the
  largest code surface (camera, snapshot, ring-phase).

---

## 4 — Recommendation

**Ship (C) first as the default-on enabler. Hold (E) as a follow-up if
mover-into-frustum-at-stationary-camera proves to be a measurable artifact.**

Reasoning:

1. (C) is **structurally safe**: during motion, behavior collapses to
   LIFECYCLE-disabled, which is the current shipping default. There is no
   new failure mode introduced — only a performance regression bound by
   "fraction of frames where camera is moving."
2. (C) is **single-commit**. Each of the four `s_gpuCullLifecycle` consumer
   sites already gates on `s_gpuCullLifecycle` — feeding them a runtime
   modifier from a global motion flag is mechanical, and the flag itself
   sits on `Camera` natively (the data is already there at line 2977; only
   the `prev` storage and delta computation are new).
3. (C)'s **cost is bounded**: a stationary-camera estimate of even 50% of
   frames keeps half of LIFECYCLE's win at normal zoom. The actual
   stationary fraction during typical mc2_01 play is the open recon
   item — probably higher (camera tends to settle on the player's mech).
4. (D) does not address the actual failure mode (entering-edge mis-gate).
5. (B) defeats the value LIFECYCLE was built for.
6. (A) collapses to legacy semantics at the edge and adds snapshot
   overhead — strictly worse than (C) at the regime it would fix.

Irreducible tradeoff: **wolfman-zoom cinematic pans pay a transient
LIFECYCLE-disabled cost during camera motion.** This is acceptable because
(a) wolfman pans are cinematic / infrequent, (b) the LIFECYCLE-disabled
behavior at wolfman zoom is the same code path that has been shipping for
weeks and is known stable, and (c) per §3.C cons, a zoom-aware threshold
keeps LIFECYCLE active during slow cinematic motion at wolfman zoom.

---

## 5 — Risk inventory

### Risk 1 — `setExists(false)` cascade via skipped lifecycle progression

`memory/cull_gates_are_load_bearing.md` and worktree CLAUDE.md "Load-Bearing
Cull Infrastructure" are explicit: **`update()` returning false →
`setExists(false)` → permanent destruction.** The lifecycle gate at
`mech3d.cpp:4256-4262` and `gvactor.cpp:2773-2779` only short-circuits
`updateGeometry()`, NOT `update()` itself — the surrounding `update()`
function still runs to completion and returns `TRUE`. So skipping
`updateGeometry()` does NOT cascade into destruction.

However, any future expansion of the lifecycle gate (Track C C3-9 work?)
that wraps `update()` itself with the same pattern would cascade. The
recommendation **explicitly preserves `update()` as the wrapper and only
modifies the inner `updateGeometry()` gate.** The motion flag must NOT be
plumbed up to `objmgr.cpp:1881-1903` to influence the framesSinceActive
sweep — that sweep contributes to `setExists` decisions.

Mitigation: lock the motion-gate effect to `readback_isActorVisibleLagged`
return value when called from `mech3d.cpp:4258` and `gvactor.cpp:2775`
specifically. The simplest impl is to fail-open at the function (returns
`true` always during motion), but that also affects the
`framesSinceActive` accumulator at `code/objmgr.cpp:1891`. The accumulator
treats "visible" as "active this frame" — fail-open during motion is a
slight under-count of `framesSinceActive`, NOT a misattribution. That is
benign for destruction (longer keepalive only), but should be called out.

### Risk 2 — APPEAR_ROUTE / `touch()` path interaction

Per memory `track_c_substrate_regression.md:20` and §1.9 above: `touch=0`
with LIFECYCLE=0 is **expected** because the touch path is gated by
`MC2_STATIC_UPDATE_SKIP=1`, not by LIFECYCLE. LIFECYCLE only controls
whether `updateGeometry()` is called *inside* `update()`; it does not route
to `touch()`.

Implication for option (C): the motion gate has no interaction with the
touch path. Trees/buildings under `MC2_STATIC_UPDATE_SKIP=1` continue to
hit their (broken) `touch()` cheap path, which is a *different* regression
flagged in `memory/update_skip_touch_regression.md`. Don't conflate.

If a future iteration tries to land `MC2_STATIC_UPDATE_SKIP=1` on by
default, it must be done **independent of** LIFECYCLE; the two flags fix
different problems. Recommendation: leave `MC2_STATIC_UPDATE_SKIP` off in
the launcher even after LIFECYCLE flips on.

### Risk 3 — Five independent env-var reads

Per §1.7, `s_gpuCullLifecycle` is read in five separate TUs at static-init
time. The motion-gate change (option C) lives inside
`gpu_cull_readback.cpp` (one TU) and is consumed by all five via the
existing `readback_isActorVisibleLagged` call. This is the right surgical
seam: one place to land the change, all five consumers automatically pick
it up.

The risk: if a future consumer adds its own `readback_isActorVisibleLagged`
call but bypasses the motion gate (e.g. by reading `s_actorVis` directly,
or adding a new query function), the gate has a hole. Mitigation: the
snapshot builder (`readback_buildActorVisSnapshot`) is the only writer of
`s_actorVis`. Putting the motion-gate short-circuit *inside the snapshot
builder* (memset-to-1 and early-return on motion) ensures every reader
picks up the relaxation, not just `readback_isActorVisibleLagged`.

### Risk 4 (added during code reading) — `framesSinceActive` correctness

`code/objmgr.cpp:1881-1903` runs the framesSinceActive sweep using
`readback_isActorVisibleLagged` directly (line 1891) when LIFECYCLE +
READBACK are both on. This is the same function we're proposing to make
motion-aware. During motion frames, it would treat **every** actor as
active-this-frame, resetting `framesSinceActive=0` for all of them. That
suppresses a legitimate aging signal that `setExists`-style cleanup
(if any consumer of `framesSinceActive` exists) relies on.

Grep for `framesSinceActive` consumers as a verification step before
landing — if any consumer triggers destruction at high counts, the
reset-during-motion behavior must be considered. The current spec is
"aging accumulator only" per the `[STABILITY]` instrumentation comments,
so probably benign, but should be confirmed.

---

## 6 — Deferred-to-spec / recon items

These are measurements that should land before the implementation commit,
not within it:

1. **Camera stationary fraction on typical mc2_01 play.** Add a 600-frame
   summary print to `Camera::update` that emits
   `[CAMERA_MOTION] frames=N stationary=N moving=N stationary_pct=F`.
   Capture across one full mc2_01 mission (start to first scripted event,
   ~3 minutes). If stationary_pct < 30%, option (C) is not delivering
   meaningful LIFECYCLE benefit and should be redesigned.

2. **Threshold values for yaw and pan.** Pre-instrument a histogram of
   per-frame `|cameraRotation - prev|` and `|position - prevPos|.length`
   over the same mission. Pick thresholds at the 75th percentile of
   stationary-class samples, NOT round numbers. Document in the eventual
   plan.

3. **Wolfman-zoom interaction.** Repeat the histogram at wolfman zoom
   (`Camera::cameraZoom[currentView] >= 80°`). Decide whether to use a
   zoom-scaled threshold or a flat one.

4. **`framesSinceActive` consumers audit.** Confirm that no destruction
   path keys off it. (Risk 4 above.)

5. **Mover-into-frustum at stationary camera.** Stress mc2_01 with AI
   movers crossing into the camera frustum while camera is stationary.
   If artifact present, escalate from (C) to (E) before default-on flip.

6. **Snapshot-builder cost on motion frames.** During motion, the proposed
   short-circuit replaces the substrate-walk with `memset(s_actorVis, 1,
   ...)`. Tracy delta vs. current cost per frame: should be a small win,
   not a regression.

---

## 7 — Verification appendix (M / D / NF per cited symbol)

Legend: M = matches memory, D = differs from memory (corrected here),
NF = not found.

| Symbol | Status | Location |
|---|---|---|
| `readback_isActorVisibleLagged()` | M | `GameOS/gameos/gpu_cull_readback.cpp:635` |
| `readback_buildActorVisSnapshot()` | M | `GameOS/gameos/gpu_cull_readback.cpp:548` |
| `s_lastGoodSlot` | M | `GameOS/gameos/gpu_cull_readback.cpp:77,150,268,307,349,376-380,449,524,556,575,584,592,601,630,637` |
| `RING_FRAMES = 3u` | M | `GameOS/gameos/gpu_cull_readback.cpp:40` |
| `MAX_STALE_FRAMES = 10u` | M (additional context) | `GameOS/gameos/gpu_cull_readback.cpp:90` |
| `MAX_ACTOR_HANDLE = 4096u` | M (additional context) | `GameOS/gameos/gpu_cull_readback.cpp:545` |
| `Mech3DAppearance::update` lifecycle gate (memory: `mech3d.cpp:4170`) | **D** — actual gate is `mech3d.cpp:4256-4262` (line drift since memory was written) | `mclib/mech3d.cpp:4256-4262` |
| `GVAppearance::update` lifecycle gate (memory: `gvactor.cpp:2702`) | **D** — actual gate is `gvactor.cpp:2773-2779` | `mclib/gvactor.cpp:2773-2779` |
| `MC2_GPU_CULL_LIFECYCLE` env-var read (5 TUs) | M | `mech3d.cpp:23, gvactor.cpp:78, objmgr.cpp:126, mech.cpp:134, gvehicl.cpp:118` |
| Camera angular-velocity tracking | **NF** | None in `mclib/camera.cpp` or `code/`; only `gosfx/` particle spin |
| `cameraRotation` (scalar yaw) | M (informational) | `mclib/camera.cpp:1019-1035, 2170, 2188, 2202, 2950-2984` |
| APPEAR_ROUTE counters | M | `code/terrobj.cpp:113-114,143-154,726` |
| `touch=0 with LIFECYCLE=0` claim | M | Memory matches: touch path is gated by `MC2_STATIC_UPDATE_SKIP`, not LIFECYCLE; documented at `code/terrobj.cpp:703-716` per spec `2026-05-06-static-prop-texture-cache-handoff.md:54` |
| `readback_buildActorVisSnapshot` call site | M | `code/objmgr.cpp:1873` |
| `framesSinceActive` sweep using lagged visibility | M (additional context) | `code/objmgr.cpp:1881-1903` (line 1891 reads `readback_isActorVisibleLagged`) |
| T1/T2/T3 tier model | M | `GameOS/gameos/gpu_cull_readback.cpp:5-7, 288-389` |

Two memory files had stale line numbers (Mech3D and GV gate sites). Both
fixes documented above; the call sites themselves and the surrounding
predicates (`turn < 3 || gpuVis || ...`) are correctly described in memory
— only the line numbers drifted.

---

End of design exploration.
