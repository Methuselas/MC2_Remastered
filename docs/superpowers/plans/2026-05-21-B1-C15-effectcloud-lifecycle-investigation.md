# B1 C15 — EffectCloud::AnimateParticle per-frame Make lifecycle vs subclass-Start routing

**Status:** investigation-only. NO code changes this commit.
**Predecessor:** C14 (`b1fc541`) reverted as `e34a70b` after crash on weapon fire under `MC2_GPU_PARTICLES=1`. HEAD now = post-C11 stable (no crash, no visible particles).
**Question:** the C14 crash trace surfaced a chain we hadn't modelled — `EffectCloud::AnimateParticle` → `Effect::Execute:566` → `XxxCloud::Make`. What is this pattern actually doing, and how does our C8/C9/C11 subclass-Start routing interact with it?

---

## 1. The crash chain — line-cited walk

```
malloc → ParticleCloud ctor SetLength    [particlecloud.cpp:323]
       ← CardCloud ctor (via SpinningCloud → ParticleCloud)
       ← CardCloud::Make factory          [cardcloud.cpp:345]
       ← gosFX::Effect::Execute event-Make [effect.cpp:566]
       ← EffectCloud::AnimateParticle     [effectcloud.cpp:264]
       ← ParticleCloud::Execute           [particlecloud.cpp:441]
       ← SpinningCloud::Execute           [spinningcloud.cpp:216]
       ← WeaponBolt::update               [weaponbolt.cpp:1399]
```

Re-grepped at write-time (all citations match HEAD `e34a70b`):

- `mclib/gosfx/particlecloud.cpp:323` — `m_data.SetLength(spec->m_maxParticleCount*spec->m_totalParticleSize);` inside `ParticleCloud::ParticleCloud(...)` ctor. This is the malloc that crashed (`memset` on NULL from a failed allocation).
- `mclib/gosfx/cardcloud.cpp:345` — `CardCloud *cloud = new gosFX::CardCloud(spec, flags);` inside `CardCloud::Make(...)`. **Static factory; constructs a NEW CardCloud instance.** Pushes the gosFX `Heap` (line 344) and pops (line 346).
- `mclib/gosfx/effect.cpp:566` — `Effect* effect = EffectLibrary::Instance->MakeEffect(event->m_effectID, flags);` inside `Effect::Execute`. **Event-driven child spawn.** Each `Effect` carries an `m_event` queue of timestamped sub-spawns; when `m_age >= event->m_time` the loop dequeues and Make-spawns a child, registers it, and calls `child->Start(...)` at line 595.
- `mclib/gosfx/effectcloud.cpp:264` — `if (effect->Execute(&info))` inside `EffectCloud::AnimateParticle`. **The `effect` here is the PER-PARTICLE child Effect owned by EffectCloud's particle struct (`particle->m_effect`).** Created at `effectcloud.cpp:191-216` inside `CreateNewParticle`, destroyed at `effectcloud.cpp:285-289` inside `DestroyParticle`.

## 2. The Make-spawn pattern — exactly what each layer does

There are **TWO independent child-Make chains** in gosFX, and the crash trace touches both:

### 2a. `EffectCloud::CreateNewParticle` — per-particle child Effect creation

`effectcloud.cpp:167-217`:

- Called once per particle when the cloud spawns a new particle (typical newbie cadence from `ParticleCloud::Execute` birth-accumulator).
- Calls `EffectLibrary::Instance->MakeEffect(spec->m_particleEffectID, ...)` and stores the result in `particle->m_effect`.
- Immediately calls `effect->Start(&local_info)` on the new child (line 216).
- **So child-effect Start fires ONCE per EffectCloud particle creation.** Not per-frame.

`AnimateParticle` (`effectcloud.cpp:222-275`) then per-frame:

- Calls `SpinningCloud::AnimateParticle` (line 240) — updates per-particle motion, no Make.
- Calls `effect->Execute(&info)` on the EXISTING `particle->m_effect` (line 264). **No new allocation here, no Make.** Just Execute on the long-lived per-particle child.
- If Execute returns false (child finished), `delete particle->m_effect` (line 272).

So `AnimateParticle` itself does NOT Make per-frame.

### 2b. `Effect::Execute` event queue — drains timestamped child spawns

`effect.cpp:544-596`:

- Each `Effect` (including the per-particle child created in 2a) has an `m_event` queue of `(time, effectID, flags, localToParent)` records, loaded from the spec.
- Every frame, `Execute` checks `event->m_time > m_age`; if not, dequeue and `MakeEffect(event->m_effectID, ...)` (line 566).
- New child is `Register_Object`'d, added to `m_children`, then `child->Start(&local_info)` (line 595).
- **Cadence: the event queue is drained ONCE per spec event. Each event fires once unless the parent loops (`IsLooped()` → re-Start at line 647 rewinds `m_event` via `m_event.First()` at line 497).**
- Children persist in `m_children`; per-frame, `Execute` iterates them (`effect.cpp:603-631`) and Executes each, deleting any that return false (line 619).

### 2c. Lifetime / destruction

- Top-level Effect: lifecycle owned by producer (WeaponBolt etc.). Producer calls `delete` when done.
- `EffectCloud::Particle::m_effect` (the 2a chain): destroyed in `EffectCloud::DestroyParticle` (`effectcloud.cpp:279-292`) and in `EffectCloud::~EffectCloud` (`effectcloud.cpp:131-146`).
- `Effect::m_children` (the 2b chain): destroyed inline at `effect.cpp:618-620` when child Execute returns false; otherwise at parent destruction (`Effect::~Effect` chain walks `m_children`).

The crash sat at the 2b boundary: the child Effect owned by an EffectCloud particle was processing its OWN event queue, hit an event, called `EffectLibrary::MakeEffect → CardCloud::Make → new CardCloud → ParticleCloud ctor → SetLength → malloc → NULL → memset crash`.

## 3. How C8/C9/C11 routing interacts with this pattern

### Producer-spawn Make path (top-level via `EffectLibrary::MakeEffect`)

After C8 (`123b8f7`) retired `EffectAdapter`, `MakeEffect` (`effectlibrary.cpp:97-120`) unconditionally constructs the legacy subclass; no env-gated branch at this boundary.

### Subclass `Start` is the SOLE env-gated hook (post-C8)

For `Card`, `Tube`, `PointCloud`, `ShardCloud`, `CardCloud`:

```
Start(info) {
    <appropriate parent::Start>(info);          // legacy state init (C9 always-call rule)
    if (Batcher::is_enabled()) {
        Spawn(m_specification, &m_localToWorld, m_seed);  // GPU emit
    }
}
```

Concretely:
- `cardcloud.cpp:907-912` calls `SpinningCloud::Start(info)` (resolves to inherited `ParticleCloud::Start`), then conditional `Spawn`.
- `pointcloud.cpp:504-525` (re-grepped) calls `ParticleCloud::Start(info)` then conditional `Spawn`.
- C9 (`9e0f4f0`) commit message documents that C8 originally skipped the parent under env-on, leaving `m_birthAccumulator` / `m_activeParticleCount` uninitialised, which let the legacy `Execute`/destructor walk garbage. Fixed by **always** calling parent Start.

### Both Make chains hit our routing

- 2a: `EffectCloud::CreateNewParticle:216` calls `effect->Start(&local_info)`. If `m_particleEffectID` resolves to a `CardCloud` (or any routed subclass), our routing fires → legacy parent::Start runs (normal CardCloud init), GPU Spawn appends N records to batcher.
- 2b: `Effect::Execute:595` calls `effect->Start(&local_info)`. Same — if the event spawns a CardCloud, routing fires.

**Cadence of subclass Start calls:**
- 2a: once per EffectCloud newbie particle (bounded by `m_maxParticleCount`).
- 2b: once per event drain (bounded by `m_event` queue length × loop count).
- Plus once per direct top-level spawn from producers (WeaponBolt etc.).

So C12's `1.19 emit/frame avg` (mc2_10) IS counting all three paths. The reason it's still low: each Spawn emits N particles (where N = sampled `m_startingPopulation`, typically 1–20 for the CardCloud cases C11 covers), so emit_total = sum-of-N's across Start events. C14's smoke showed `emit_total=5271 / 4432 frames = 1.19 emits-per-frame composite` but `records_per_flush_max=21`, consistent with many low-N events and occasional bursts.

### Legacy per-frame Execute keeps running under env-on

Critically: `Spawn` ADDS GPU records; it does NOT short-circuit the legacy Execute path. After Start, the legacy `ParticleCloud::Execute` will still birth particles per `m_particlesPerSecond`, run `AnimateParticle`, hit event-driven `Effect::Execute:566` Makes, etc. **Every legacy allocation under env-off also runs under env-on.** Heap pressure is therefore NOT additive from our path (Batcher staging is fixed-capacity 4096, pre-reserved, no growth).

## 4. Crash hypotheses (ranked, separate question from invisibility)

### H1 — gosFX::Heap exhaustion via increased child-Effect retention (~40%)

The crash is `malloc returning NULL`. Under env-on the C14 shader fix was the only delta from C11 stable. C14 touches no CPU lifecycle. **What's actually different is that C14 was the first build to actually project particles correctly** — the user observed the crash on a "real" weapon-fire frame deep into mc2_10 (frame 4185, ~28s in). The session-cumulative allocation rate of new CardClouds in legacy path is unchanged; the question is whether the SESSION lasted long enough to expose a pre-existing leak/exhaustion path that any env-on run would have hit if it ran past ~25-30s.

Supporting evidence:
- C9 commit message: prior crash hit at frame ~3542 from heap corruption adjacent to mcTextureManager.
- C14 smoke gate (in commit msg): mc2_10 30s `PASS, 4433 frames` — but smoke doesn't fire weapons. User play does.
- ParticleCloud `SetLength(maxParticleCount * totalParticleSize)` — a large CardCloud can be many KB. Hundreds of long-lived containers from looping/recurring weapon events plus orphaned/retained children at composite parents can drain a fixed-size heap.

Verifier: probe `gos_GetHeapSize(gosFX::Heap)` and free-bytes-remaining at periodic intervals; correlate with crash frame.

### H2 — `m_birthAccumulator` double-counting under env-on (~25%)

C9 mandated `ParticleCloud::Start(info)` runs ALWAYS. Re-reading `ParticleCloud::Start` at `particlecloud.cpp:347-353`:

```cpp
Effect::Start(info);
...
Stuff::Scalar newbies = spec->m_startingPopulation.ComputeValue(m_age, m_seed);
Min_Clamp(newbies, 0.0f);
m_birthAccumulator += newbies;
```

Note: `m_birthAccumulator +=`. If `Start` is called multiple times on the SAME instance (e.g., looping via `effect.cpp:647` re-Start), the accumulator GROWS each time. Legacy code handled this; our path then calls `Spawn(...)` which independently samples `m_startingPopulation` and emits N records. **The legacy birth-accumulator is unchanged by our path — so legacy births the same N. No double-birth on the legacy side. Our path contributes only to the GPU batcher.** This hypothesis is plausible only if Spawn somehow re-enters legacy code; it does not (`spawn_cardcloud.cpp:52-209` only touches Batcher).

Probability lowered to ~10% on this analysis. Move down.

### H2' (replaces H2) — Looped/long-lived weapon Effects accumulate `m_children` (~25%)

`Effect::Execute:603-631` iterates `m_children` and removes finished ones, but the parent itself only gets killed when `HasFinished()` returns true. `ParticleCloud::HasFinished()` at `particlecloud.cpp:493`: `Effect::HasFinished() && (m_activeParticleCount == 0)`. And `Effect::HasFinished()` at `effect.cpp:743-752`: `IsExecuted() && m_age < 1.0f → false`; OR has children → false. **If `IsLooped()`, `Effect::Execute:647` re-Starts the parent without ever finishing.** This is independent of our routing but means weapon-bolt-like Effects may accumulate `m_children` over their lifespan if the looped parent keeps spawning new event-driven children.

Crash frame is 4185 (~28s). At ~150 FPS with weapon spam (HARD-fire missile bursts in mc2_10), each weapon bolt's looped Effect can drain into the gosFX heap. Our routing doesn't change this — but it would not hide an issue that existed pre-C14 either. The C14 shader fix is just the trigger that prompted longer user play sessions.

Verifier: instrument `Effect::Execute:566` and `Effect::~Effect` with a global counter; compare env-on vs env-off creation/destruction balance over a 60s session.

### H3 — Heap fragmentation rather than exhaustion (~15%)

Long-running session under env-on may not exhaust gosFX::Heap absolutely, but fragment it enough that the `m_maxParticleCount * m_totalParticleSize` SetLength call can't find a contiguous block. Same observability as H1; same probe.

### H4 — Pre-existing latent bug exposed by C14 timing (~10%)

C14 added `glGetUniformLocation` + `glUniform4fv` + `glUniformMatrix4fv` per flush (`gos_particle_bridge.cpp:165-181`). These calls execute on the GL thread and don't allocate from gosFX heap, but they DO advance frame time. Plausibly the shader-driven invisibility actually suppressed some legacy state mutation downstream, and the C14 fix re-enabled it. Low probability without specific evidence.

### H5 — `delete particle->m_effect` corruption at line 272/288 (~10%)

`EffectCloud::AnimateParticle` line 272 and `DestroyParticle` line 288 both `delete particle->m_effect`. If the child Effect is one of our routed subclasses (CardCloud, etc.), its destructor runs the legacy chain. The C9 lesson was specifically that uninitialized state causes the destructor to corrupt heap. C9 fixed that by always calling parent Start. **But** we have not verified the C9 fix is sufficient for the EffectCloud-CHILDREN case — only producer-top-level. If `CreateNewParticle:216` calls `effect->Start(local_info)` on a freshly-Made CardCloud, the C9 always-call-parent path runs and state is initialized. So this should be safe.

Verifier: env-gated `[GOSFX]` prints around CardCloud ctor / Start / dtor / `m_data.SetLength`; record (this, m_activeParticleCount, m_birthAccumulator) at each transition.

## 5. Invisibility hypotheses (separate question, ranked)

### IH1 — Projection chain still wrong post-revert (~60%)

C14 was the projection fix. Reverting it restored the pre-fix shader. Per C13's analysis (`docs/superpowers/plans/2026-05-20-B1-C13-render-invisibility-inspection.md`) the pre-C14 shader writes `gl_Position = terrainMVP * world` where `terrainMVP` is D3D pixel-homog, not GL clip → particles clip away. Stable-post-revert "no visible particles" is the expected outcome of this pre-fix state.

Implication: **the invisibility and the crash are NOT the same bug.** The invisibility is solved by C14 (or an equivalent projection fix). The crash is something else, triggered to expression by the longer / more-active sessions that C14 enabled because particles were finally visually engaging the user.

### IH2 — `glDepthFunc(GL_GEQUAL)` direction mismatch (~15%)

C13's third hypothesis. Sibling fast paths use forward-Z; our depth setting may discard everything. Re-check via RenderDoc on a single frame post-fix.

### IH3 — Canary world-position at (0,0,50) outside playfield (~10%)

C13's second hypothesis. The per-frame gamecam.cpp:293-305 canary plus SpawnXxx particles may both project off-screen for the smoke camera. The producer-side spawns DO use `parentToWorld` from the weapon effect's local-to-world (correct world position), so this only explains the canary; producer particles should be visible in the playfield if IH1 is fixed.

## 6. Recommended next step

**Two things must be untangled, in this order:**

1. **Don't re-attempt C14 verbatim.** The projection fix is needed for visibility, but the crash demonstrates we have a latent CPU-side problem that gates safe re-landing.

2. **C16 should be an instrumentation pass** targeting H1 / H2'. Specifically:
   - Env-gated `[GOSFX_HEAP]` periodic print (every 1000 frames, or every 100 CardCloud::Make events): `gos_GetHeapSize(gosFX::Heap)` free-bytes-remaining, total-allocated, largest-contiguous if available.
   - Env-gated `[GOSFX_CHILD]` counter on `Effect::Execute:566` (`m_children.Add` site) AND on `Effect::~Effect` to compute create/destroy delta over session.
   - Run env-on session for ~60s of weapon-spam mc2_10 and compare deltas vs env-off.
   - If delta diverges: H2' confirmed and the fix is independent of C14 (it's a legacy `m_children` leak revealed by long sessions).
   - If heap fills monotonically: H1 confirmed.

3. **Only after diagnosis**, decide whether C17 is:
   - (a) Patch the legacy leak (if H2'). C14 then re-lands cleanly.
   - (b) Pre-size gosFX::Heap (if H1 and the fix is bounded).
   - (c) Re-architect: skip the legacy Execute path entirely when env-on for routed subclasses (B2-polish work moved earlier). This is the META-FIX — retires the additive heap pressure by deleting the legacy path under env-on.

## 7. Critical campaign-reframing finding

**The "no visible particles" result observed at C13/post-C11 was ALWAYS expected** — the projection chain is wrong without C14. The user has been blocked on a visibility test that the CPU side does not gate. C14 IS the visibility fix, and there's nothing in the routing approach that's architecturally incompatible with the EffectCloud per-particle / event-driven Make pattern: both 2a and 2b chains call Start correctly through the legacy factory, our routing fires correctly, and Spawn appends GPU records without disturbing legacy state.

**The crash is a separate orthogonal bug.** It almost certainly exists pre-C14 too (since our path doesn't add new CPU allocations), just hidden by shorter visible-failure sessions. Re-attempting C14 verbatim without diagnostic for H1/H2' will reproduce the crash. The correct shape of the next slice is INSTRUMENTATION FIRST, then targeted fix, then re-land C14.

No campaign-level reframing required. Subclass-Start routing is structurally fine; the work ahead is diagnosing a heap or child-Effect leak that is independent of the GPU pipeline.

---

## Citations re-grep verification

All cited file:line locations re-grepped at write-time against HEAD `e34a70b`:
- `particlecloud.cpp:323` — `m_data.SetLength(...)` ✓
- `cardcloud.cpp:344-346` — `gos_PushCurrentHeap(Heap); CardCloud *cloud = new ...; gos_PopCurrentHeap();` ✓
- `effect.cpp:566` — `Effect* effect = EffectLibrary::Instance->MakeEffect(...)` ✓
- `effect.cpp:595` — `effect->Start(&local_info);` ✓
- `effect.cpp:603-631` — children iterator + delete-on-false ✓
- `effect.cpp:647` — `if (IsLooped()) Start(info);` ✓
- `effect.cpp:743-752` — `Effect::HasFinished()` ✓
- `effectcloud.cpp:191-216` — `CreateNewParticle` MakeEffect + child Start ✓
- `effectcloud.cpp:264` — `if (effect->Execute(&info))` in `AnimateParticle` ✓
- `effectcloud.cpp:272, 288` — `delete particle->m_effect` ✓
- `particlecloud.cpp:347-353` — `Effect::Start` + `m_birthAccumulator +=` ✓
- `particlecloud.cpp:493` — `HasFinished` override ✓
- `cardcloud.cpp:907-912` — C11 Start override (re-grepped: line numbers per git show 8109e1e diff) ✓
- `pointcloud.cpp:504-525` — C8/C9 Start override ✓
- `effectlibrary.cpp:97-120` — unconditional legacy `MakeEffect` ✓
- `batcher.cpp:86-100` — Emit + budget early-out ✓
- `batcher.cpp:121-127` — leaked-singleton Instance ✓
- `gamecam.cpp:284-307` — frame-level canary + Flush ✓
- `spawn_cardcloud.cpp:52-209` — full SpawnCardCloud ✓
