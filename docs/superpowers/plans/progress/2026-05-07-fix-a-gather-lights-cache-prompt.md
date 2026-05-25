# Execution prompt — Fix A: `GatherLightsParameters` per-frame cache

**Paste this into a fresh session to execute.** Self-contained briefing.

---

You are executing a small follow-up perf fix on the MC3 rendering modernization arc. After yesterday's hash-dedup fix (commit `996aff4`) collapsed `addLightDataStructure` from 8.5 ms to 0.4 ms per frame, the next-largest cost in `TerrainObject::update appearanceUpdate` is `GatherLightsParameters` itself — copying ~900 bytes of light-config struct per actor per frame.

## Worktree

`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. All paths relative.

## Required reading (in order)

1. `CLAUDE.md` — worktree rules (build with `--config RelWithDebInfo`; deploy with `cp -f` per-file + `diff -q`; documentation discipline).
2. **`mclib/txmmgr.cpp:1011-1090` — `GatherLightsParameters`.** This is the function you're optimizing. Read it carefully before designing the fix.
3. **`mclib/tgl.cpp:2848-2852` — `TG_Shape::GatherGpuObjectLightDataOnly`.** The single caller of `GatherLightsParameters` in the per-actor hot path. Calls `GatherLightsParameters(&lightData_)` then `mcTextureManager->addLightDataStructure(&lightData_)`.
4. **`mclib/txmmgr.cpp:883-980` — `addLightDataStructure` (the just-shipped hash dedup).** Understand how it interacts with whatever you do here — cache invalidation must include the dedup map's reset semantics.
5. **`mclib/bdactor.cpp:2287-2308`** (BldgAppearance::update) and **`bdactor.cpp:4647-4672`** (TreeAppearance::update) — sites where `CacheGpuLightData()` is called per-frame. Note that `worldLights[0]->aRGB` is set per-actor at `bdactor.cpp:2150` (the terrain-elevation-scaled value) BEFORE `CacheGpuLightData` runs.
6. Commit `996aff4` (`perf(lighting): O(1) hash dedup`) — the predecessor fix. Read its commit message for the architectural concern that motivates Fix A.
7. `memory/cull_gates_are_load_bearing.md` — mandatory read before any rendering-touching change.

## The optimization opportunity

`TG_HWLightsData` (~900 bytes) is filled by `GatherLightsParameters` on every per-actor `CacheGpuLightData()` call (~1000 calls/frame). Tracy capture confirmed `GatherGpuObjectLightDataOnly` averages ~911 ns/call total, of which ~388 ns is the dedup (now hash-based) and ~523 ns is `GatherLightsParameters` struct-fill work. Total: ~523 µs/frame in the struct-fill alone.

**The structural observation:** `TG_HWLightsData`'s fields are mostly scene-global, with one per-actor field:

| Field | Per-light | Per-actor variation |
|---|---|---|
| `lightToWorld[N][16]` | scene global (light's transform in world) | none |
| `lightDir[N][4]` | derived from lightToWorld | none |
| `lightFalloff[N][4]` | scene global | none |
| `lightColor[0][4]` | per-actor (terrain-light-scaled `worldLights[0]->aRGB`) | **YES — the only per-actor field** |
| `lightColor[1..N][4]` | typically scene-global | none in normal play |

So 99% of the struct is identical across actors in the same frame; the only variation is `lightColor[0]`.

## The fix

Per-frame cache: compute the full `TG_HWLightsData` ONCE per frame (call it the "scene template"). Per actor, copy the template + patch `lightColor[0]` from the per-actor terrain-scaled value. Hash + dedup the patched result via the existing `addLightDataStructure`.

Expected: `GatherLightsParameters` cost drops from ~523 ns/call to ~50 ns/call (just the `lightColor[0]` patch + the existing dedup work). Total per-frame `GatherGpuObjectLightDataOnly` would drop from ~911 µs to ~440 µs (the dedup work remains).

Estimated win: another ~470 µs/frame.

## Implementation outline (you'll grep + verify these at write-time)

1. Add a static cached `TG_HWLightsData s_sceneTemplate` and a `uint32_t s_sceneTemplateFrame` (matched against `g_mc2FrameCounter`).

2. Add a new function `MC_TextureManager::addLightDataStructureWithPerActorColor(const TG_HWLightsData& template, uint32_t perActorARGB)`. It:
   - If `s_sceneTemplateFrame != currentFrame`, fill `s_sceneTemplate` via the existing `GatherLightsParameters` work (one-time per frame).
   - Build the per-actor struct: `TG_HWLightsData local = s_sceneTemplate; local.lightColor[0] = decompose(perActorARGB);`
   - Call existing `addLightDataStructure(&local)` — gets the dedup + hash benefit for free.

3. Modify `TG_Shape::GatherGpuObjectLightDataOnly` (`tgl.cpp:2848`) to use the new path. The per-actor color comes from `worldLights[0]->aRGB` which is the same source `GatherLightsParameters` reads at `txmmgr.cpp:1059`. Trace the data flow before changing.

4. Reset `s_sceneTemplateFrame` when something invalidates the scene lights (e.g., lightlist changes mid-frame). Probably handled implicitly by the frame-counter compare; verify there's no mid-frame light update that the cache misses.

5. Update `resetLightData` to also clear `s_sceneTemplateFrame = UINT32_MAX` (force re-fill on first call after reset).

## Skill to invoke

**`superpowers:subagent-driven-development`** — small enough to be inline, but the cache-invalidation logic warrants careful review.

## Gates

- **Build clean:** `cmake --build build64 --config RelWithDebInfo --target mc2`.
- **Visual smoke:** `py -3 scripts/run_smoke.py --mission mc2_01 --duration 15 --kill-existing` — must show no visual regression (lighting on actors should look unchanged).
- **`[DESTROY v1]` count parity** vs baseline (commits `396effa` should be HEAD when this slice starts).
- **Tracy zone target:** `addLightDataStructure scan` zone unchanged at ~388 µs (the dedup itself isn't changing). New zone or replacement: total `GatherGpuObjectLightDataOnly` should drop from ~911 µs to ~440 µs.

## What NOT to do

- **Don't change the dedup.** It works (commit `996aff4`). Build on top of it; don't re-implement.
- **Don't try to skip the addLightDataStructure call entirely.** The per-actor struct STILL needs to land in the dedup table because the registry's flush() reads the slot index for each actor. The win is in CACHING the GATHER work, not skipping the dedup.
- **Don't optimize for the case where `lightColor[1..N]` are also per-actor.** They're scene-global in normal play. If a future feature makes them per-actor, the cache invalidation logic will need updating — flag with a TODO comment.
- **Don't apply to non-static-prop callers without verifying.** `GatherGpuObjectLightDataOnly` is called from `CacheGpuLightData` only. Other callers of `GatherLightsParameters` (if any — grep for them) may have different semantics. Verify.

## When blocked

- **Per-actor struct differs in unexpected fields:** investigate which fields and why. May indicate a feature (e.g., per-actor mech-bay spotlights) that the cache doesn't account for. Document and either add to the cache key or skip the cache for that case.
- **Visual regression after fix:** the cache-invalidation timing is the most likely culprit. Try `s_sceneTemplateFrame = UINT32_MAX` at the start of every frame to force re-fill — if that fixes the visual issue, the bug is in invalidation logic; tighten the invalidation conditions.
- **Tracy doesn't show expected reduction:** the time may be elsewhere (e.g., `GatherGpuObjectLightDataOnly` zone wraps something other than just `GatherLightsParameters` and `addLightDataStructure`). Add finer-grained zones to discriminate.

## Deliverable

Single commit on `claude/nifty-mendeleev`:
- Title: `perf(lighting): cache GatherLightsParameters scene template (911us -> ~440us)`
- Body: explain the cache + per-actor-color-patch pattern; cite Tracy measurements before/after; reference commit `996aff4` as predecessor; note the architectural concern (per-actor scaling could be moved to instance data — that's a future arc).

After this ships, **Fix B** (the LOD-swap-related black-trees fix at `docs/superpowers/plans/progress/2026-05-07-fix-b-lod-swap-needsfullbake-prompt.md`) is the next slice. They're independent and can ship in either order, but Fix B is correctness-not-perf so is also an option to do first.
