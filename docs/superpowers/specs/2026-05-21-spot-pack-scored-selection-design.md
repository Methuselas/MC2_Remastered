# Per-Shape Light-Pack SpotLight_ Priority Lift — Design Spec

- **Status:** DRAFT v3 — addresses round-2 adversarial review (1 CRITICAL fundamentally re-framed; 2 secondary CRITICALs resolved)
- **Date:** 2026-05-21
- **Worktree:** `claude/nifty-mendeleev`
- **Slice ID:** (E') — extends (E) SpotLight_ real illumination; in (E)'s scope per greybeard ruling
- **Review history:**
  - **v1** — round-1 (opus) BLOCKING-CRITICAL: per-actor scored selection would have killed LIGHTBRIDGE template-cache → 8.5ms regression. Plus 7+ caller-survey errors.
  - **v2** — round-2 (sonnet) NEEDS-REVISION: foundational mis-read about which array `s_listOfLights[]` points to. `getWorldLights()` returns `activeLights[]` (compacted per-frame), NOT `worldLights[]` (sparse registration storage).
  - **v3 (this version)** — fix happens in `Camera::updateLights` `activeLights[]` assembly loop. ~25 LOC. No parallel array. No accessor change. No LIGHTBRIDGE template-key disruption.
- **All file:line citations grep-verified at write time against `.claude/worktrees/nifty-mendeleev/`.**

---

## 1. What v1 and v2 got wrong (record for learning continuity)

**v1 (round 1, opus, BLOCKING-CRITICAL):**
- C1: conflated `s_listOfLights[]` index space with `worldLights[]` slot index.
- C2: caller survey had 7+ errors including a false `gvactor.cpp` `CacheGpuLightData` citation.
- C3: per-actor scored selection would have unique-keyed every actor in `sceneLightTemplateKey` → 0% template hit rate → 8.5ms `addLightDataStructure` regression.

**v2 (round 2, sonnet, NEEDS-REVISION):**
- v2 corrected C1 by clarifying that `s_listOfLights[]` is set from `eye->getWorldLights()`. BUT v2 then claimed `getWorldLights()` returns `worldLights[]` (the sparse 1024-slot registration storage). **WRONG.** [camera.h:777](mclib/camera.h): `TG_LightPtr *getWorldLights (void) { return activeLights; }` — returns the compacted per-frame view, NOT the sparse storage.
- v2's M3 "parallel `s_orderedLights[]` array" was over-engineering aimed at preserving `worldLights[]` slot indices — but `s_listOfLights[]` never indexes into `worldLights[]` in the first place. The slot-stability concern was real for `lightId` storage (which IS indexed into `worldLights[]`), but it doesn't conflict with reordering `activeLights[]` (which is built fresh every frame).

**The real architecture:**
- `worldLights[1024]` — sparse registration storage. `addWorldLight` returns a slot index. `removeWorldLight` clears by slot. `BldgAppearance::lightId` etc. store these indices for lifetime management. **Stable across frames** within a light's lifetime.
- `activeLights[1024]` — compacted per-frame view. Rebuilt from scratch each frame by `Camera::updateLights` ([camera.cpp:1887-1983](mclib/camera.cpp)). Walks `worldLights[]`, gates each by type/visibility, appends to `activeLights[]`. **Index in `activeLights[]` is NOT the same as index in `worldLights[]`** — it's the ordinal among visible-this-frame lights.
- `getWorldLights()` returns `activeLights[]` (despite the misleading name). Every `SetLightList` caller passes `activeLights[]` to `s_listOfLights[]`.
- `GatherLightsParameters` walks `s_listOfLights[]` = `activeLights[]` (the compacted per-frame view) up to 16 entries.

So the FIFO truncation happens on `activeLights[]` assembly order, which is `worldLights[]` SCAN order in current `Camera::updateLights`. Lights registered later (higher `worldLights[]` slot indices) appear later in `activeLights[]`. Same observable behavior as v1's misframing; the fix needs to act on `activeLights[]` assembly, not on `worldLights[]` reorder.

## 2. Correct symptom→cause framing (v3)

### What the data shows
T1.16 diagnostic: of 54 (E)-owned `worldLights[]` slots, ~12-15% land `active=true` in `Camera::updateLights` summary windows. mc2_10 intro shows visible mech/GV bodies not illuminating despite their slots being in the active-true subset.

### Real bug class
`Camera::updateLights` walks `worldLights[0..MAX_LIGHTS_IN_WORLD]` in slot order and appends visible POINT/SPOT lights to `activeLights[]` in that same order. (E)'s lights register LATER (at higher `worldLights[]` slot indices) than base-scene lights (AMBIENT, INFINITE, weapon-bolt POINTs). So they appear LATER in `activeLights[]` too.

`GatherLightsParameters` then walks `activeLights[]` FIFO and stops after 16 entries. The first 16 win. (E)'s SpotLight_ POINT lights, deeper in the iteration order, get truncated.

**Selection criterion today: `activeLights[]` assembly order = `worldLights[]` registration order.**

### Why the fix is "priority lift during activeLights assembly"
~5-7 SpotLight_ POINTs active simultaneously in mc2_10 (T1.16 evidence) — well under the 16-cap. Reordering `activeLights[]` assembly to append SpotLight_-tagged POINTs FIRST guarantees they survive the FIFO truncation by construction.

Properties preserved:
- `activeLights[]` pointer stays the same → `sceneLightTemplateKey`'s `listPtr` hash unchanged → LIGHTBRIDGE template cache hits unchanged
- `worldLights[]` is not touched → all `lightId` slot-stable storage on actors keeps working
- All 15 `SetLightList` callers consume the same `eye->getWorldLights()` interface, unchanged
- `addLightDataStructure` dedup unaffected: shapes within a frame still see the same reordered `activeLights[]` → same `TG_HWLightsData`

If/when content scale exceeds 16 simultaneously-active SpotLight_ POINTs, scored selection becomes necessary; that's a future (F)-class problem.

## 3. META-FIX: SpotLight_ priority append in Camera::updateLights

### 3.1 Implementation site

`Camera::updateLights` at [camera.cpp:1887-1983](mclib/camera.cpp). Currently the POINT/SPOT branch at lines 1927-1968 unconditionally appends to `activeLights[]`. Modify to two-pass append within the same loop pass: SpotLight_-tagged POINT/SPOT lights go into `activeLights[]` immediately; non-tagged POINT/SPOT lights go into a temporary deferred buffer; after the main loop, the deferred buffer is flushed into `activeLights[]`.

### 3.2 Code sketch

```cpp
void Camera::updateLights()
{
    numActiveLights = numTerrainLights = 0;
    
    // (E') deferred buffer for non-priority POINT/SPOT — appended after the
    // main loop so SpotLight_-tagged lights survive the GatherLightsParameters
    // 16-slot FIFO truncation. See docs/superpowers/specs/2026-05-21-spot-pack-scored-selection-design.md
    TG_LightPtr deferredNonPriority[MAX_LIGHTS_IN_WORLD];
    long numDeferred = 0;
    
    for (long i = 0; i < MAX_LIGHTS_IN_WORLD; ++i)
    {
        if (!worldLights[i]) continue;
        TG_LightPtr light = worldLights[i];
        
        // TG_LIGHT_TERRAIN handling — unchanged
        if (light->lightType == TG_LIGHT_TERRAIN) {
            // ... existing code at :1902-1915 ...
            continue;
        }
        
        // AMBIENT/INFINITE handling — unchanged (these stay at the front by their existing handling)
        if (light->lightType == TG_LIGHT_AMBIENT ||
            light->lightType == TG_LIGHT_INFINITE) {
            light->active = true;
            activeLights[numActiveLights++] = light;
            terrainLights[numTerrainLights++] = light;
            continue;
        }
        
        // POINT/SPOT — (E') priority split based on SpotLight_-tag membership
        if (light->lightType >= TG_LIGHT_POINT &&
            light->lightType < TG_LIGHT_TERRAIN) {
            Stuff::Vector4D dummy;
            light->active = projectForLightingShadow(light->position, dummy);
            terrainLights[numTerrainLights++] = light;  // terrain pipeline unaffected
            
            if (mc2_spotlight_diag::is_e_slot(i, nullptr)) {
                // Priority lift: append immediately so this slot wins FIFO truncation
                activeLights[numActiveLights++] = light;
            } else {
                // Non-priority: defer to after the main loop
                deferredNonPriority[numDeferred++] = light;
            }
        }
    }
    
    // (E') flush deferred non-priority POINT/SPOT into activeLights[]
    // after all priority lights have been appended. FIFO truncation in
    // GatherLightsParameters now reaches priority lights first.
    for (long j = 0; j < numDeferred && numActiveLights < MAX_LIGHTS_IN_WORLD; ++j) {
        activeLights[numActiveLights++] = deferredNonPriority[j];
    }
}
```

### 3.3 Properties (audited)

**(a) LIGHTBRIDGE template-cache preserved.**
[txmmgr.cpp:1020-1021](mclib/txmmgr.cpp) `sceneLightTemplateKey` hashes `reinterpret_cast<uintptr_t>(listOfLights)`. The `listOfLights` value is `activeLights` (the array pointer). The pointer itself is stable — the array is allocated once at camera init and only its contents are rewritten per frame. **Pointer unchanged → listPtr hash unchanged → cache key unchanged.** Template hits at the existing rate.

**(b) `addLightDataStructure` dedup preserved.**
[txmmgr.cpp:1283-1322](mclib/txmmgr.cpp) FNV+memcmps the full `TG_HWLightsData`. After the priority lift, all shapes within a frame still see the same reordered `activeLights[]` → same `TG_HWLightsData` content → dedup hit rate unchanged.

**(c) `worldLights[]` slot indices untouched.**
The reorder happens on `activeLights[]` (the compacted view). `worldLights[]` storage is not modified. `BldgAppearance::lightId`, `Mech3DAppearance::pointLight`/`lightId` (anubis), `spotlightSlotIds_` vectors on all three (E) classes — all continue to work via `worldLights[storedSlotId]` reads.

**(d) `removeWorldLight` semantics preserved.**
[camera.h:822-827](mclib/camera.h) checks `worldLights[lightNum] == light` before nulling. Both fields unchanged. Removal still works.

**(e) CPU vertex lighting path (`tgl.cpp:1968`) automatically benefits.**
Reads `s_listOfLights[]` = `activeLights[]` = the reordered view. Same priority lift applies to CPU path without explicit code change. **No CPU/GPU divergence.**

**(f) `[SPOT_DIAG v1]` probes still work for their intended purpose.**
The camera-overwrite probe at [camera.cpp:1965-1975](mclib/camera.cpp) reports `slot=<worldLights[] index>` for unique slots seen during the loop. Slot index is unchanged. The probe correctly reflects camera-gate rejection rate. It doesn't show the post-loop priority-lift effect (that's a `activeLights[]` ordering, not a `worldLights[]` property) — but it doesn't need to; the visual canary is the meaningful test.

### 3.4 Tagging mechanism — `mc2_spotlight_diag::is_e_slot()`

Already exists from T1.16. [mclib/spotlight_diag.{h,cpp}](mclib/spotlight_diag.h):
- `tag_slot(long, SourceClass)` — called UNCONDITIONALLY (not env-gated) from (E)'s lazy-init blocks in `bdactor.cpp`, `mech3d.cpp`, `gvactor.cpp` after each successful `addWorldLight`.
- `is_e_slot(long, SourceClass*)` — queries the underlying `unordered_map<long, uint8_t>` for membership.
- `untag_slot(long)` — called from destroy hooks alongside `removeWorldLight`.

The `MC2_SPOT_DIAG` env var only gates the `[SPOT_DIAG v1]` PRINT lines, not the map's tag/untag/query operations. **The map IS populated in production.** Round-2's CRITICAL-2 ("`is_e_slot()` env-gated, zero-op in production") was based on a misread; tag_slot fires regardless of env.

Decision: the priority-lift code path uses `mc2_spotlight_diag::is_e_slot(i, nullptr)` unconditionally. No env gate. The diagnostic infrastructure becomes the authoritative source for "is this a SpotLight_-tagged slot" classification. Document this transition in code comment.

### 3.5 Anubis interaction (response to v2 §A3 and round-2 G)

Pre-existing anubis searchlight at [mech3d.cpp:3333-3344](mclib/mech3d.cpp) uses `eye->addWorldLight(pointLight)` but DOES NOT call `mc2_spotlight_diag::tag_slot()`. So anubis is in the "non-priority POINT/SPOT" deferred bucket → appended after priority slots → potentially truncated.

**Two options:**
- **Option X: leave anubis untagged.** It's a pre-existing optional feature, has been shipping unmodified; (E')'s priority lift may incidentally reduce anubis's slot survival rate in busy scenes. Acceptable IF anubis is rarely visible (it requires `eye->isNight && visible && sensorLevel > 4`).
- **Option Y: tag anubis.** Add `mc2_spotlight_diag::tag_slot(lightId, Mech)` after the anubis `addWorldLight` at mech3d.cpp:3344. Anubis enters the priority bucket alongside (E)'s spotlights.

Recommendation: **Option Y**. The architectural intent is "spotlight-class lights have priority over generic POINT/SPOT." Anubis is a spotlight by every measure (semantic: searchlight node; gate: night+visible+sensor; type: TG_LIGHT_SPOT with directional cone). Tagging it preserves architectural consistency. 1-line code change.

### 3.6 Reorder cost

Per-frame: O(MAX_LIGHTS_IN_WORLD) walk (already paid by existing `Camera::updateLights`) + O(num_active_non_priority_POINT_SPOT) deferred-buffer flush. Net additional cost: ~1024 conditional branches + one extra `MAX_LIGHTS_IN_WORLD` stack-allocated pointer array per frame. **~5-10µs/frame.** Below the 100µs CPU-projection budget.

## 4. Greybeard 5-question ruling (v3)

1. **Subsystem pin.** `activeLights[]` assembly order in `Camera::updateLights` at [camera.cpp:1887](mclib/camera.cpp). Currently the assembly is `worldLights[]` scan-order; this is the upstream condition that places (E)'s late-registered POINT/SPOT lights deep in `activeLights[]` where FIFO truncation kills them.

2. **Symptom vs cause.** Symptom: mech/GV bodies don't visibly illuminate from their SpotLight_ POINT registrations. Cause: assembly-order-equals-registration-order places (E)'s priority spotlights behind base-scene lights in `activeLights[]`, causing FIFO truncation in `GatherLightsParameters` to consume its budget on the base lights before reaching the spotlights.

3. **The meta-fix.** Two-pass append within `Camera::updateLights`: SpotLight_-tagged POINT/SPOT lights go into `activeLights[]` immediately; non-tagged POINT/SPOT defer to after the main loop. FIFO truncation reaches priority lights first by construction. **Modifies one function (`Camera::updateLights`) plus one line at anubis (`tag_slot` call after its `addWorldLight`).**

4. **Substitutive test.**
   - Visual canary on mc2_10 intro: (E) mech/GV bodies show yellow contribution from their attached spotlights. Buildings keep their yellow glow.
   - `[SPOT_DIAG v1]` summaries: priority slots active rate unchanged (still gated by camera projection at slot 1907); the priority lift acts AFTER the rejection so rejected slots don't appear in the priority bucket anyway. (E)'s ~5-7 active SpotLight_ POINTs per mc2_10 frame all show up at `activeLights[]` indices 0-N before the FIFO truncation point.
   - Anubis verification: tag anubis, confirm anubis enters the priority bucket too, ensure no regression on a mech-heavy mission with `sensorLevel > 4` (mc2_24).

5. **Verdict.** **META-FIX, in (E)'s scope.** Bug class retired: "later-registered POINT/SPOT lights lose FIFO contest against earlier-registered base lights." ~25 LOC + 1 anubis tag. Compatible with future scored selection (replace the deferred buffer with a sort by distance-to-camera-focus). Compatible with future clustered lighting (the priority lift becomes irrelevant once cluster-lookup replaces FIFO).

## 5. Vulkan-prep audit (unchanged)

| Requirement | This slice |
|---|---|
| Explicit device-mediated binding | Not applicable — no new GL bindings |
| No implicit cross-call GL state | Not applicable — no new GL state |
| std430 lockstep | Not applicable — SSBO layout unchanged |
| `[0,1]` depth | Not applicable — no depth |
| Enqueue/flush patterns | Not applicable — runs at `Camera::updateLights` |
| No full RHI ahead of need | Compliant — pure CPU-side reorder of an existing array |

**Vulkan-ready by absence.**

## 6. Future-extension hooks

- **Scored selection** (when content scale exceeds 16 simultaneously-active SpotLight_): replace the deferred-buffer flush with a sort by distance-to-camera-focus-position. Comparator-only change; rest of the code path stays the same.
- **Clustered lighting** (if scene complexity ever justifies it): replace the `Camera::updateLights` loop entirely with per-cluster light-list building. The 15 `SetLightList` callers, the `GatherLightsParameters` walk, the LIGHTBRIDGE cache, and the dedup all become legacy. Out of scope here; this slice doesn't preclude it.

## 7. Adversarial considerations

- **A1. `MAX_LIGHTS_IN_WORLD = 1024` stack allocation.** `TG_LightPtr deferredNonPriority[MAX_LIGHTS_IN_WORLD]` is ~8KB on the stack. Acceptable on the engine's main thread (typical stack ≥1MB). Verify Windows MSVC default stack is large enough; if concern, hoist to a `static thread_local` or to a member variable.
- **A2. Anubis tagging side effects.** Tagging anubis with `mc2_spotlight_diag::tag_slot` means anubis lights show up in T1.16 `[SPOT_DIAG]` summary lines as `src=mech`. Cosmetic only; doesn't conflict with anything. Worth a comment to disambiguate (anubis-tagged vs (E)-registered).
- **A3. `is_e_slot()` lookup cost.** `unordered_map<long, uint8_t>` lookup is O(1) amortized. Per-frame ~1024 lookups during the assembly loop ≈ ~5-10µs total. Fine.
- **A4. Empty `is_e_slot()` map (no SpotLight_ registered).** First frame of a mission, before any (E) lazy init has fired, `is_e_slot()` returns false for all slots. The priority lift becomes a no-op; `activeLights[]` assembly behaves as today. Correct fallback.
- **A5. Frame ordering: `Camera::updateLights` vs actor-update `addWorldLight`.** If an actor's `addWorldLight + tag_slot` runs AFTER `Camera::updateLights` within the same frame, the new tag isn't seen until the NEXT `Camera::updateLights`. So newly-spawned (E) lights have a 1-frame lag before priority lift. Acceptable; not a visual artifact (the spawn frame is typically not visible to the player).
- **A6. `removeWorldLight + untag_slot` ordering.** When (E) actor is destroyed, `untag_slot` fires alongside `removeWorldLight`. The slot becomes NULL in `worldLights[]` (so `Camera::updateLights` skips it) AND removed from `is_e_slot()` map. Both must run; one without the other leaves a stale tag (would mark a NULL slot as priority, but the `worldLights[i] == nullptr` check at line 1896 short-circuits). Even with the mismatch, no harm.
- **A7. Multiple cameras.** If MC2 ever calls `Camera::updateLights` for multiple cameras (e.g. shadow map + main), each camera maintains its own `activeLights[]` and the priority lift applies to each independently. Currently (per grep) there's only the main camera; verify before declaring complete.

## 8. Open questions for plan-phase

- **OQ1.** Anubis tagging — Option Y (tag in [mech3d.cpp:3344](mclib/mech3d.cpp) after `addWorldLight`). Confirm semantic: anubis is currently the only TG_LIGHT_SPOT in active production; should we tag it? Recommend yes per §3.5 rationale.
- **OQ2.** Stack vs static allocation of `deferredNonPriority[]`. Stack is simpler. Static would save 8KB stack but adds thread-safety concerns if `Camera::updateLights` becomes multi-threaded later. Recommend stack.
- **OQ3.** Probe enhancement (optional): a new `[SPOT_PRIORITY_LIFT v1]` first-hit log on the first frame the deferred buffer is non-empty, showing priority-vs-deferred counts. Helps verify the lift fires in production. Recommend yes.
- **OQ4.** Test coverage for the `is_e_slot()` map lifetime — confirm that an untag (on actor destroy) doesn't fire BEFORE the next `Camera::updateLights` consumes the tag. Should be safe (destroy is in same frame as removeWorldLight; both happen before next frame's updateLights).

## 9. Implementation scope

- **One function modified**: `Camera::updateLights` at [mclib/camera.cpp:1887-1983](mclib/camera.cpp).
- **One line added**: `mc2_spotlight_diag::tag_slot(lightId, Mech)` after anubis `addWorldLight` at [mech3d.cpp:3344](mclib/mech3d.cpp).
- **One optional probe**: `[SPOT_PRIORITY_LIFT v1]` first-hit/summary.
- **Total LOC**: ~25 in `camera.cpp`, ~1 in `mech3d.cpp`, ~15 if probe added.
- **No shader change. No SSBO layout change. No `worldLights[]` reorder. No accessor signature change. No new TU. No threading actor position. No LIGHTBRIDGE disruption.**

This is the smallest viable META-FIX for the bug class. Sized correctly for stock content (~5-7 active SpotLight_ POINTs). Compatible with future scoring/clustering as outlined in §6.

## 10. Cross-references

- v1 BLOCKING-CRITICAL review: round-1 adversarial findings (opus) preserved as §1
- v2 NEEDS-REVISION review: round-2 adversarial findings (sonnet) — `getWorldLights()` returns `activeLights[]` not `worldLights[]`
- Spec (E): [2026-05-20-spotlight-real-illumination-design.md](2026-05-20-spotlight-real-illumination-design.md)
- (F) lighting MODEL rework brief: [terrain_lighting_compute_kernel_saturation.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\terrain_lighting_compute_kernel_saturation.md)
- LIGHTBRIDGE template-cache origin: commit `996aff4` (8.5ms→0.4ms optimization)
- T1.16 diagnostic artifact: `tests/smoke/artifacts/2026-05-21T06-48-58/mc2_10.log`
- `mc2_spotlight_diag` infrastructure: [mclib/spotlight_diag.h](mclib/spotlight_diag.h)
- Greybeard skill: [.claude/skills/greybeard.md](../../.claude/skills/greybeard.md)
- Adversarial review skill: [.claude/skills/adversarial-plan-review.md](../../.claude/skills/adversarial-plan-review.md)
