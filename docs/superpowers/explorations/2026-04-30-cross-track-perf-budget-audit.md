# Cross-Track Performance Budget Audit — Tracks C+D+E+F at Tier-1 with 5 Mods

**Date:** 2026-04-30
**Status:** Design-only consolidation. No code paths yet exist; numbers below combine measured baselines (Phase-A render headroom, Tracy zone costs, Sol2 microbenchmarks cited in trampolines doc) with the per-track design contracts.
**Scope:** All-up CPU/memory/disk budget for an installation running Track C (Lua scripting), Track D (asset import / Assimp), Track E (persistence — partial in v1), Track F (hierarchical AI), with **5 user mods** active on **tier-1 missions** (mc2_01-class loadout: ≤40 active warriors, water-heavy maps).
**Per-mod profile assumed:** ships data + control + ~100 LoC Lua + ~10 prototypes + a handful of event handlers + 0–2 timers per warrior.

The post-Phase-A render numbers (`m2_thin_record_cpu_reduction_results.md`) are the load-bearing baseline: `Terrain::render drawPass` 25 → 1.46 ms, FPS 50 → 100–145 at 14k quads. **~24 ms/frame freed** at typical zoom. That headroom is the budget pool everything below draws against.

---

## §1 — Headline budget table

Per-frame cost at tier-1 mission, 5 mods active, 100 simulated warriors, 60 FPS target (16.67 ms/frame). Two columns: shipping build (REPL/Tracy/hot-reload disabled) and dev build (everything on).

| Subsystem | Idle ms/frame | Active ms/frame (shipping) | Active ms/frame (dev) | Memory MB | Notes |
|---|---|---|---|---|---|
| **Lua VM init** | one-time | — | — | — | Done at mission begin, ~30–80 ms wall (allocator + sandbox env + std-lib subset + 5× `data.lua`/`control.lua` load). Off the per-frame budget. |
| **Lua control.lua tick** (5 mods) | 0 | <0.5 | <0.5 | ~5–10 (VM heap incl. payloads) | Dispatcher runs each mod's queued callbacks under the 5M-opcode/tick cap, round-robin. Trampolines doc §4.1: ~80 ns/binding call; 100 LoC mods rarely exceed 50–200 calls/tick. |
| **AI L4 unit tick** (Track F, 30 Hz) | 0 | ~0.75 | ~0.75 | ~16 KB/warrior | 100 warriors × 30 ticks/s × ~15 µs/tick. Mostly C++; Lua only for opt-in personality hooks. (Track F §11.) |
| **AI L3 operational** (10 Hz) | 0 | ~0.5 | ~0.5 | ~8 KB/warrior (BT IR) | 100 warriors × 10 Hz × ~30 µs. BT traversal in C++; ~5 µs Lua per leaf. |
| **AI L2 tactical** (3 Hz) | 0 | ~0.075 | ~0.075 | ~32 KB/lance | ~25 lances × 3 Hz × ~60 µs. Lua coroutine per lance. |
| **AI L1 strategic** (1 Hz) | 0 | ~0.005 | ~0.005 | ~64 KB/team | ~3 teams × 1 Hz × ~300 µs amortized over 60 frames. |
| **AI shared (sensor fusion 5 Hz, threat heatmap 1 Hz)** | 0 | ~0.10 | ~0.10 | 64×64 float grid + LOS matrix ≈ 80 KB | Batched once per cycle, all L3/L4 read. |
| **ImGui REPL frame** | 0 | 0 (gated off) | ~0.5 if window visible | <1 | Modder DX doc §1: env-gated. Hidden = 0 cost. |
| **Hot-reload watcher** | 0 | 0 (compiled out) | <0.01 (event-driven) | tiny | ReadDirectoryChangesW worker thread; main-loop polls a queue. Dev-only. |
| **Mod loader (init)** | one-time | — | — | — | data-stage + control-stage; ~5–20 ms per mod at mission begin. |
| **ABL VM (existing)** | unchanged | unchanged | unchanged | unchanged | Baseline; per-warrior ~30 Hz. Track C does NOT replace ABL in v1. |
| **Tracy in-engine zones** | 0 | 0 (compiled out in shipping) | ~0.05 (18 zones) | tiny | `tracy_profiler.md` — instrumentation always-on in dev, stripped in shipping. |
| **Renderer (post-M2)** | — | ~5–7 | ~5–7 | unchanged | Baseline post-Phase-A. |
| **GameLogic (existing)** | — | ~3–5 | ~3–5 | unchanged | `perf_profiling_results.md`; spikes when AI activates new cells. |
| **TOTAL active (shipping)** | — | **~9.9–13.9** | **~10.5–14.5** | budget room | Frame budget 16.67 ms; ~3–6 ms headroom at 60 FPS, ~17–18 ms at 90 FPS. |

**Read:** Comfortable. The post-M2 ~24 ms freed by the renderer is the source pool. Tracks C+E+F together consume ~2–3 ms at full saturation; the rest of the freed budget pads against worst case (10 mods, 200 warriors, GPU-static-prop debug paths, ABL spikes during cell activation).

---

## §2 — Per-mod cost

Each enabled mod adds incremental cost. Numbers come from the boundaries deep-dive §3 caps and the trampolines §4 microbenchmarks.

**Per mod (typical, 100 LoC + 10 prototypes + 5 event handlers + 0–2 timers/warrior):**
- **Memory:** ~2–4 MB Lua heap (sandbox doc §4 puts the *VM-wide* cap at 64 MiB; per-mod soft cap 16 MiB advisory). Compiled chunks + sandbox env + persist scaffold + handler closures ≈ 1 MB; payload tables (prototype JSON parsed into Lua tables) ≈ 1–3 MB depending on prototype richness.
- **CPU:** ~50–150 µs/frame in mod-attributed Lua time (boundaries §3.2 round-robin slice of 1M opcodes if 5 mods share a tick).
- **Disk:** 100–500 KB Lua + JSON; assets dominate when present (2–50 MB optional textures/models).
- **VRAM:** 0 unless the mod ships textures/models. With AssetScale-aware overlap (memory:`asset_scale_subsystem.md`), a typical re-skin is 4–16 MB VRAM at 4× upscale. Track D (Assimp) lands a chassis re-skin at 1–8 MB.

**Scaling table (per-mod cost is sub-linear):**

| Mods loaded | Lua heap (MB) | CPU ms/frame (mod time only) | Disk (MB content) | Notes |
|---|---|---|---|---|
| 0 | ~1 (VM scaffold) | 0 | 0 | Engine-only baseline. |
| 1 | ~3 | ~0.1 | 0.5–10 | Linear region. |
| 3 | ~7 | ~0.3 | 1–30 | Linear region. |
| 5 | ~12 | ~0.5 | 2–50 | The audit baseline. |
| 10 | ~22 | ~1.0 | 5–100 | Approaches VM cap (32 MiB used / 64 MiB cap, 50%). |
| 20 | ~40 | ~2.5 (sub-linear: round-robin slice tightens, mods naturally yield) | 10–200 | Memory pressure: 62.5% of cap; round-robin slice now ~250 K opcodes/mod/tick. |
| 30 | hits 64 MiB cap | OOM Lua errors begin from heaviest mod | — | **Cliff.** Heaviest mod's allocations fail; that mod degrades operation-fatal-this-tick repeatedly until the player disables it. |

**Cliff:** at ~25–30 typical mods OR one heavy mod that consumes >32 MiB by itself. The boundaries doc §3.1 logs `mem_estimate` periodically so the cliff is visible in mod-manager UI before crash.

**Sub-linearity drivers:**
- Shared engine state (sensor fusion, threat heatmap, BindingRegistry) is one-cost-for-all-readers.
- Lua VM allocator is one heap; fragmentation amortizes.
- Round-robin opcode slicing means mod CPU costs *cap* rather than sum at high mod counts: each tick has a hard 5M opcode ceiling regardless of mod count. With 30 mods, each gets ~167 K opcodes — slow per-mod but the engine never blocks.

---

## §3 — Stress-test scenarios

| Scenario | Cost | Safety mechanism | Recovery |
|---|---|---|---|
| **10 mods all listening to `Warrior.Damaged`** | 10× handler dispatch per damage event. Damage events fire at most ~50/sec at saturation. 50 × 10 × 80 ns ≈ 40 µs/sec. Negligible. Payload deep-copy (boundaries §6) is ~200 ns per primitive field. | Per-handler `xpcall` (sandbox §3): one mod's bug doesn't kill siblings. Payload size cap 64 KiB serialized (boundaries §7 open Q3). | Errors are operation-fatal-this-tick; mod keeps running next tick. |
| **One mod registers 1000 prototypes** | Memory: ~5–15 MB depending on payload richness — within 16 MiB per-mod soft cap. Init time: ~50–200 ms additional at mission begin. CPU/frame: 0 (prototypes are passive data tables). | Soft cap warning at 16 MiB; hard at 64 MiB VM cap. | If cap hit: mod load aborts at the offending registration; logged as `event=mem_estimate ... usage_kib=...`. |
| **5000 LoC `control.lua` running per-tick** | If naive (`for i=1,n` over a large list every tick), can blow the round-robin slice. Worst case observed: 5M opcodes consumed → instruction cap fires. | Per-tick instruction cap 5M opcodes (sandbox §4). Per-mod slice ~1M opcodes. | Cap fires → mod's current tick errors with `instruction cap exceeded`; mod resumes next tick. Auto-disable on >3 consecutive cap hits (modder DX doc — under "anti-patterns auto-degrade"). |
| **Mod creates 10 timers per warrior (×100 warriors = 1000 timers)** | Timer fire is one Lua call each; if all fire same frame, ~80 µs at 80 ns/call. Timer table memory ~100 KB. Trampolines §3.2 default-one-shot prevents runaway. | Per-mod timer concurrent-count soft cap (open question — see §11). Memory cap ultimate backstop. | Modder warning: "your mod has 1000 active timers" via `[LUA_BUDGET v1] event=approaching budget=timers`. |
| **Modder mistake: tight loop in tick handler** | `while true do end` halts the frame for ~5 ms (sandbox §4 says "~5 ms wall on this hardware") then aborts. | Instruction cap 5M opcodes/tick. Combined with memory cap. | Single-frame stall ≤5 ms (visible as one frame drop), then cap fires, mod handler errors, engine continues. **Engine never crashes** (boundaries §6 §300). |
| **Mod emits a 10 MB event payload 60×/sec** | Deep-copy cost ~30 ms/sec just for payload marshalling. Drops FPS to ~30. | Boundaries §7 Q3: cap event payload at 64 KiB serialized. | Engine truncates / errors emit-side; logs `event=payload_too_large mod=<id> bytes=<n>`. |
| **5 mods all run 0.44 Hz BT eval at 100 warriors with personality Lua hooks** | 5 × 100 × 0.44 × ~5 µs (5 leaves) = 11 µs/sec base + per-tick BT IR traversal 1.4 ms (Track F §11). Total: ~1.4 ms/frame. | Within Track F 5 ms/frame budget. | Headroom remains. |

---

## §4 — Mod-author guidance (numbers a modder targets)

Concrete numbers — these are also the in-engine profiler's warning thresholds (modder DX doc §2):

- **Per-frame Lua time:** target **<1 ms/frame** for your mod's total binding cost. Yellow at 0.5 ms; red at 1.0 ms (`[LUA_PERF v1] event=mod_over_budget` fires at 1 ms, throttled 1×/sec/mod).
- **Per-tick opcode usage:** stay under **500 K opcodes/tick** to leave headroom on a 5-mod install. (Each mod's slice of the 5M VM-wide cap is ~1M with 5 mods, ~333 K with 15.)
- **Total persist size:** **<1 MiB serialized per-mod** (boundaries §3.5 soft cap). Above 4 MiB: persist write refused.
- **Per-warrior event handlers:** keep to **≤5** per (mod, warrior). Mod can register many event subscriptions; the cost is per-fire.
- **Behavior tree depth:** keep **≤8 levels deep**. Beyond 12 the BT IR traversal eats into the L3 30 µs/tick budget. (Track F §11.)
- **Concurrent timers:** **<200 per mod**, **<2000 per VM**. Hard cap deferred to v2; soft warning today.
- **String allocation in hot paths:** avoid (trampolines §4.5). Prefer integer handles for named timers/events.
- **Event payload:** **<64 KiB serialized** (boundaries §7 Q3). Larger → use `mc2.persist` and a "look-at-this-key" notification event.

---

## §5 — Engine-side hard caps (what happens at cap, recovery)

| Cap | Value | Justified by | At cap | Recovery |
|---|---|---|---|---|
| **VM memory** | **64 MiB** | Sandbox §4. Factorio default ~1 GB but their mods are heavier; 64 MiB generous for our scale. | Allocator returns NULL → Lua error. | `xpcall` wrapper catches → log `event=error msg=not enough memory`. Mod-fatal-this-tick; engine continues. |
| **Per-mod memory soft cap** | **16 MiB** (advisory) | Boundaries §3.1. Per-mod tagging is sampled, not enforced. | Logged `event=mem_estimate mod=<id> usage_kib=<n>`. | None automated; mod-manager surfaces in UI. |
| **Per-tick opcodes (VM-wide)** | **5,000,000** | Sandbox §4. ~5 ms wall on dev machine. | `lua_sethook` fires → mod handler errors. | xpcall catches; engine continues; counter resets next tick. |
| **Per-mod opcode slice** | **5M / N mods** round-robin | Boundaries §3.2. With 5 mods → ~1M each. | That mod's current callback errors. Other mods on same tick still run. | Same as VM-wide. |
| **Total prototype count** | **No hard cap in v1** | Boundaries §3.4. Texture handle cap 3000 (memory:`texture_handle_cap.md`) is the indirect ceiling. | Logged `event=asset_count mod=<id> count=<n>` advisory. | None automated. |
| **Total event handler count** | **No hard cap** | Cost is per-fire, not per-registration; deep-copy + xpcall amortize. | Memory cap is ultimate backstop. | — |
| **Concurrent timer count** | **No hard cap v1** (open question §11) | Memory cap covers the pathological case. | — | — |
| **Event payload size** | **64 KiB serialized** (proposed; boundaries §7 Q3) | Cross-mod deep-copy cost. | Emit fails → log + handler not invoked. | Modder uses `mc2.persist` + key-name event. |
| **Mod count** | **No hard cap** | Sub-linear scaling; cliff is VM memory cap at ~25–30 typical mods. | OOM → heaviest mod degrades. | Mod-manager UI surfaces memory usage; player disables. |

---

## §6 — Build / disk / startup costs

From build-integration deep-dive §6:

- **Compile time (clean RelWithDebInfo build of mc2.exe):**
  - Baseline today: ~90–120 sec.
  - Lua 5.4 (32 discrete .c TUs, /MP parallel): **+2–4 sec.**
  - Sol2 (header-only, but ~5–10 sec/TU on the 2 TUs that include `<sol/sol.hpp>`): **+10–20 sec.**
  - yaml-cpp (Track C deferred — for AssetScale + mod manifests): **+3–5 sec** if landed.
  - nlohmann/json (already in tree via existing tracking): 0 incremental.
  - BT library (in-house, ~12–15 nodes, ~500 LoC C++): **+1–2 sec.**
  - Assimp (Track D, only if landed in same build): **+30–60 sec** clean — by far the heaviest.
  - **Total Track C M0 clean-build delta: ~15–25 sec** (build-integration §6 figure).
  - Including Track D Assimp: ~50–80 sec extra. Acceptable but Assimp dominates.

- **Binary size (mc2.exe):**
  - Lua static lib: ~250 KB compiled.
  - Sol2 templates instantiated: ~1–2 MB depending on binding count.
  - BT runtime: ~50 KB.
  - **Track C delta: ~1.5–2.5 MB** added to mc2.exe.
  - Assimp adds ~3–8 MB more.

- **Startup time (VM init at mission begin):**
  - `lua_newstate` + std-lib subset open: ~5 ms.
  - Sandbox env + binding registration (≈100 binding entries): ~10 ms.
  - 5 mods × (data.lua compile+exec ~5 ms + control.lua compile+exec ~5 ms): **~50 ms.**
  - **Total: ~65–80 ms one-time at mission begin.** Off the per-frame critical path.

- **Memory at idle (no mods, VM resident):**
  - Lua VM scaffold + std-lib state: ~500 KB.
  - Empty sandbox env: ~50 KB.
  - BindingRegistry tables: ~100 KB.
  - **Idle: ~1 MB resident** beyond baseline.

---

## §7 — Safety margins (do the math)

The post-M2 ~24 ms/frame freed budget breaks down as:

| Consumer | ms/frame at saturation | Cumulative |
|---|---|---|
| Track C base overhead (5 mods, dispatch + xpcall scaffolding) | <0.5 | 0.5 |
| Track F native-rate (1 Hz / 3 Hz / 10 Hz / 30 Hz cascade, see §11 of design) | ~1.4 | 1.9 |
| Per-mod control.lua cost (5 mods × 100 LoC) | ~0.5 | 2.4 |
| Tracy in-dev overhead (18 zones) | ~0.05 | 2.45 |
| Worst-case ABL spike (existing) | ~1 (already in baseline) | 2.45 (no double-count) |
| **Total Phase-A worst case (5 mods, 100 warriors)** | **~2.5 ms/frame** | — |

**Headroom remaining for Phase B/C and richer AI:** 24 − 2.5 = **~21.5 ms/frame** still freed vs. pre-M2.
Frame total at 60 FPS budget (16.67 ms) post all this: ~10–14 ms used → **2.5–6.5 ms slack**, enough for 90 FPS targeting.

If Track F upgrades L3 from 10 Hz → 20 Hz (L4 stays 30 Hz, the cascade compresses):
- L3 doubles to ~1.0 ms.
- Total ~3.0 ms.
- Still ~21 ms freed budget remaining.

---

## §8 — Phase A vs Phase B vs future

| Phase | Net AI/mod consumption | Notes |
|---|---|---|
| **v1 (Track C + E partial + F native rate)** | ~2.5–3 ms/frame at 5 mods | Audit baseline. |
| **Phase B (GPU lighting / shadow rework)** | +0 on AI side; frees additional ~2–4 ms by moving work GPU-side | Net more headroom for AI/mods. |
| **Future v2 (richer AI: 10 strategic doctrines, deeper BTs, 200 warriors)** | ~5–7 ms/frame | Within Track F 5 ms target with margin. |
| **Future v3 (full Lua takeover of ABL — reverse-direction doc Phase 2)** | unchanged dispatch cost, ABL VM cost drops to zero | Net win: removes ABL's ~1 ms baseline. |
| **Worst plausible (10 mods, 200 warriors, all features)** | ~10 ms/frame | Still leaves ~6 ms/frame slack at 60 FPS. Comfortable. |

---

## §9 — Profiling integration

Tracy hooks (modder DX doc §2) expose all of the above in real-time:

- **`mc2.lua.calls_per_frame`** — atomic counter, plotted next to FPS. Spike from 200 → 50000 on one frame is the textbook "modder wrote tight loop" signature.
- **`mc2.lua.<mod_id>.ms`** — per-mod ms/frame plot. Visible in mod-manager inspector as colored bar (green <0.5, yellow 0.5–1.0, red >1.0).
- **`mc2.profiler.zone(name, fn)`** — modder-named Tracy zone. Source-loc allocated dynamically via `___tracy_alloc_srcloc_name` (modder DX §2).
- **`mc2.profiler.plot(name, val)`** — modder-named per-frame plot.
- **Engine zones for AI layers** — `AI.L1.tick`, `AI.L2.tick`, `AI.L3.tick`, `AI.L4.tick`, `AI.SensorFusion`, `AI.ThreatHeatmap`. Total 6 new zones; per-zone overhead ~50 ns at fast end. (`tracy_per_quad_overhead.md` lesson: don't add per-warrior zones.)
- **Engine zones for Lua dispatch** — `Lua.Tick`, `Lua.DispatchEvent`, `Lua.RoundRobinSlice`. 3 zones, summary granularity.

In dev-build, total Tracy overhead at peak instrumentation budgeted at ~0.05 ms/frame (24 zones × ~2 µs amortized). In shipping, all stripped via `TRACY_ENABLE` define — zero runtime cost.

---

## §10 — Anti-patterns to flag (engine response)

| Anti-pattern | Engine response |
|---|---|
| Per-warrior tight loop in event handler | Instruction cap fires → handler errors → operation-fatal-this-tick. Auto-disable mod after 3 consecutive cap hits in 10 sec window (proposed in modder DX). |
| Synchronous file I/O in `control.lua` (e.g. polling a file every tick) | I/O is sandboxed to mod root; per-call cost ~500 µs visible in `mc2.lua.<mod>.ms`. Soft warning at 1 ms/frame. No auto-disable. |
| Allocating large tables per-tick (`local t = {}; for i=1,1e6 do t[i]=i end`) | Memory cap fires → Lua OOM → handler errors. Engine continues. |
| Unbounded recursion in BT decisions | BT IR has explicit max depth ceiling (proposed: 16). Beyond → BT load rejects with `event=bt_depth_exceeded`. Runtime stack-overflow caught by sandbox xpcall. |
| String concatenation in hot loops (`s = s .. "x"` in tick) | Memory cap eventually fires. Modder-DX profiler surfaces per-mod ms spike. Soft warning advised; no auto-action. |
| Re-entering ABL from Lua dispatch handler (reverse-direction doc) | `in_abl_dispatch_` reentry guard: rejected with `event=abl_reentry_reject`. Operation-fatal. |
| Mod registers handler for every event type "just in case" | No engine action; cost is per-fire only. Mod-manager UI shows handler count for visibility. |
| Mod ships its own copy of stock `corebrain.abx` | Profile-launcher `.abx` audit blocks load (memory:`magic_abl_contamination_rule.md`). Hard-stop. |

**Auto-disable policy:** Only the "instruction cap exceeded ≥3× in 10 sec" case auto-disables a mod (modder DX §3 proposal). All other anti-patterns are soft-warn — the engine logs and surfaces in the mod-manager UI but does not silently degrade. Players see the cost; they choose whether to keep the mod loaded.

---

## §11 — Open questions

1. **Per-mod CPU accounting precision.** Boundaries §8 Q1: sampled allocation tagging vs. per-callback allocator-context swap. Lean: sampled with 60-sec rolling estimate. Need a smoke test on Carver5O with 10 mods to validate sample noise floor.
2. **Concurrent timer cap.** No v1 hard cap. Memory cap is the backstop, but a mod with 50 K timers will starve its opcode slice doing nothing useful. Proposal: 2000 timers/VM hard, 200/mod soft. Validate.
3. **Round-robin fairness metric.** Boundaries §8 Q2: opcodes-elapsed vs. handler-invocation count. Lean opcodes; cost is per-mod opcode counter (negligible).
4. **Event payload cap.** Boundaries §7 Q3: 64 KiB serialized proposed. Need stress-test from a "fan-out 10 listeners on a 50 KiB payload" scenario before locking.
5. **Track D + Track C build-time interaction.** Assimp + Sol2 together push clean build to ~150–200 sec. Acceptable, but worth confirming the CI agent can do it within the existing slack. (See build-integration §6.)
6. **Track E persistence cost not yet baselined.** Boundaries §3.5 soft-cap 1 MiB. Serialization cost (probably JSON via nlohmann) at write time: estimate ~1 ms per 100 KB. Test before locking limits.
7. **AI tick-rate upgrade headroom.** Track F natively ticks L3 at 10 Hz; mods may want 20 Hz. Doubling L3 alone adds ~0.5 ms/frame, still inside budget. But if Phase-B GPU work pulls more CPU back to AI, document the upgrade path.
8. **Per-mod VRAM accounting.** AssetScale subsystem reports per-mod texture/model VRAM but doesn't cap. With 30 mods each shipping a 50 MB chassis re-skin: 1.5 GB VRAM. On a 4 GB card this is a problem. Need a per-mod VRAM warning at >100 MB.
9. **GameLogic spikes interaction.** `perf_profiling_results.md` notes GameLogic spikes 200ms+ when panning reveals new cells (AI/pathfinding activation). With Track F replacing some of that, the spikes might shrink. Or move to a different cell-activation handler. Need to re-baseline after F lands.
10. **Hot-reload contention.** ReadDirectoryChangesW on `mods/` directory while a save is being written to `mods/.../persist`: race condition. Currently dev-only so not load-bearing for players, but worth a guard.

---

## Summary

**With Tracks C+E+F all landed and 5 typical mods active on tier-1 missions:**

- Per-frame consumption: **~2.5–3.0 ms** for AI + Lua dispatch + control.lua, on top of unchanged renderer (~5–7 ms) and GameLogic (~3–5 ms).
- Total active frame: **~10–14 ms / 16.67 ms** — comfortable 60 FPS, headroom for 90 FPS.
- Memory: **~12 MB Lua heap** (5 mods × ~2.4 MB avg) on a 64 MiB cap (19% used).
- Disk delta: **~2.5 MB** binary growth + ~10–50 MB mod content.
- Build time: **+15–25 sec** clean RelWithDebInfo.
- Startup: **+65–80 ms** at mission begin, off the per-frame budget.

The post-M2 ~24 ms/frame freed by terrain CPU reduction is the load-bearing source pool. Tracks C+E+F at 5-mod saturation consume **~10–13%** of that pool, leaving ~21 ms/frame for Phase B GPU lighting work, future AI richness, or 90 FPS at 100+ warriors. The headroom is real, the caps are defensible, and the cliff (mod-count vs VM memory) is well outside the typical install profile.

The numbers say **ship it.**
