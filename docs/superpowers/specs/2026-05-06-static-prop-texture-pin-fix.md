# Spec: Static-Prop Registry Texture Pinning — Fix B for `MC2_STATIC_UPDATE_SKIP=1` Black-Billboard Bug

> **Status:** Revision 3 — incorporates two adversarial-plan-review passes (Opus + Sonnet) and Track B-specific corrections from outside review. Rev 3 fixes a compile error in the rev 2 `registerRecipe` body (used non-existent accessor `getMultiType()` and protected struct members), specifies the combined `destroy()` body authoritatively to prevent a silent drop of either Track B's or this spec's additions during merge, defines the missing `TEX_LC_PIN` macro and pin-call counter infrastructure, and makes the high-pressure `Out of texture handles` verification gate unconditional.
> **Supersedes the alpha-test/material-classification framing as the root-cause fix for the visible symptom.** Path 4 (in tree, uncommitted) remains valuable as material-classification hardening but is not this bug's fix.
> **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
> **Companion handoff:** `docs/superpowers/specs/2026-05-06-static-prop-texture-cache-handoff.md`
> **Sister plan (must update in lockstep):** `docs/superpowers/plans/2026-05-06-track-b-widen-registry.md`
> **Trace artifact (mechanism confirmation):** `tests/smoke/artifacts/2026-05-06T10-52-34/mc2_01.log` (137,541 `[TEX_LIFECYCLE v1]` events; 259 evictions; 39 distinct typeIDs caught with `glTexId=0`).

---

## Mechanism (confirmed by trace + pause/unpause discriminator)

```text
Mission load: AppearanceType::init() registers each multi-shape with the batcher
  → GpuStaticPropBatcher::registerMultiShape (bdactor.cpp:787, :3699 etc.)
    → registerType → typeShape->SetTextureHandle(j, nodeIdx) primes the leaf's
      listOfTextures[].gosTextureHandle (gos_static_prop_batcher.cpp:809)
First render of an actor: per-instance recipe registration
  → GpuStaticPropRegistry::registerRecipe (bdactor.cpp:1668, :4313)
    → s_recipeRanges.push_back(rng); rng.multi = multi
MC2_STATIC_UPDATE_SKIP=1 substitution branch (code/terrobj.cpp:740-755):
    if (s_staticUpdateSkip && gpuPath && appearanceClaimsStatic && !ownerForcesDynamic) {
        appearance->touch();  // skips TransformMultiShape, skips re-cache
    } else {
        appearance->update(); // normal path
    }
Per-frame eviction (mclib/txmmgr.cpp ~line 2035-2050 in MC_TextureManager::update):
  predicate: !uniqueInstance && !(neverFLUSH & 1) && lastUsed <= turn-60
  → gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle)
  → masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE (0xFFFFFACE)
The leaf's listOfTextures[].gosTextureHandle is NOT updated (no back-reference).
Static-prop registry flush (gos_static_prop_registry.cpp:128 onward):
  → batcher reads src->listOfTextures[pkt.textureSlot].gosTextureHandle
    (gos_static_prop_batcher.cpp:1753 area, post-instrumentation)
  → gos_GetGLTextureId(staleHandle) returns 0
  → glBindTexture(GL_TEXTURE_2D, 0) → black quad
```

The pause/unpause discriminator confirms it. `mcTextureManager->update()` is gated at `code/mission.cpp:507-510` on the running-game predicate. Pause stops new evictions — the visible bug suspends. Unpause resumes — bug returns within seconds. The minority subset that doesn't recover on pause are actors whose texture was already stale at the pause instant; pause freezes broken state, doesn't repair it.

## Solution: registry-owned, recipe-range-scoped texture pinning

Each `RecipeRange` owns a list of pinned `mcTextureNodeIndex` values. The pin lifetime matches the range lifetime. Refcount-style accumulation in the texture manager handles the case where multiple ranges share the same underlying multi-type — confirmed by trace (gosHandle 0x220 shared by typeIDs 47/52/62/67 during the diagnostic run).

### New API on `MC_TextureManager`

```cpp
// mclib/txmmgr.h, MC_TextureManager class:
void pinNode(DWORD nodeIdx);     // pinRefCount++; asserts nodeIdx < MC_MAXTEXTURES
                                 //                AND masterTextureNodes[nodeIdx].numUsers > 0
                                 //                (catch orphan-pin: pinning a free slot)
void unpinNode(DWORD nodeIdx);   // pinRefCount--; asserts pinRefCount > 0 first
DWORD getPinCount(DWORD nodeIdx) const;  // diagnostics

// mclib/txmmgr.h, MC_TextureNode struct, sibling to neverFLUSH (~line 138):
DWORD pinRefCount;   // Registry-driven pin. Excludes node from cacheout when > 0.
```

**`init()` update** — `MC_TextureNode::init()` at `mclib/txmmgr.h:168-191` initializes every field on fresh allocation. Add `pinRefCount = 0;` immediately after `neverFLUSH = false;` at line 179 so the field is initialized on every node-slot reset. Without this, `removeTextureNode` followed by a fresh `loadTexture` into the same slot would inherit a stale `pinRefCount` from the prior owner.

**Orphan-pin invariant** — `pinNode`'s `numUsers > 0` assertion catches the case where a registry tries to pin a slot whose underlying texture has been freed. In the current code path this is impossible (static-prop textures load at mission init and aren't freed during gameplay), but the assertion is cheap and guards against future code paths that might call `removeTexture` on a registry-pinned texture. The assertion will fire loudly during dev/testing if the invariant breaks, rather than silently corrupting refcounts.

`pinNode` operates on the cache-slot identity (the master node), not the GPU object. It is safe to call on a node currently in `CACHED_OUT_HANDLE` state — eviction predicates already short-circuit on `CACHED_OUT`, and the next `loadTexture` re-realization will see `pinRefCount > 0` and stay live thereafter.

### Eviction-predicate updates — purely additive

Add `&& pinRefCount == 0` to all four predicates. **This is purely additive — `pinRefCount` defaults to 0, so any node that nobody pins behaves exactly as today.**

| Site | Function | Existing predicate | Add |
|---|---|---|---|
| `mclib/txmmgr.cpp` ~583 | `flushCache` cacheThreshold variant | `!uniqueInstance` | `&& pinRefCount == 0` |
| `mclib/txmmgr.cpp` ~606 | `flushCache` turn-30 variant | `!uniqueInstance` | `&& pinRefCount == 0` |
| `mclib/txmmgr.cpp` ~628 | `flushCache` turn-1 aggressive variant | `!uniqueInstance` | `&& pinRefCount == 0` |
| `mclib/txmmgr.cpp` ~2035 | `MC_TextureManager::update` per-frame | `!uniqueInstance && !(neverFLUSH & 1)` | `&& pinRefCount == 0` |

(Line numbers approximate — current values shift due to in-tree `[TEX_LIFECYCLE]` instrumentation. Implementer must re-grep for the predicate signatures `!uniqueInstance` / `!(neverFLUSH & 1)` at write-time, not trust these numbers.)

**flushCache counting-loop trap (DO NOT modify):** `flushCache` has FOUR loop bodies, only THREE of which are evictions. The loop at `mclib/txmmgr.cpp:554-568` is a counting/diagnostic pass — its predicate has the form `if (!pinned && !unique && masterTextureNodes[i].lastUsed <= (turn - cache_Threshold)) poolFlushableIdle++;` and it does NOT call `gos_DestroyTexture`. **Do not mechanically apply `&& pinRefCount == 0` to all `!uniqueInstance` occurrences — only to the three eviction-loop predicates at ~583, ~606, ~628.** For diagnostic accuracy, the counting-loop's `poolFlushableIdle` count SHOULD also exclude `pinRefCount > 0` nodes (mirrors the existing `pinned` exclusion), but that's a separate ergonomic update — implementer applies it as a small additional change in the same commit.

**`event=evict_skipped` emit sites:** four total, mirroring the four predicate sites. One emit inside `MC_TextureManager::update()` (line ~2035 area, when the new `pinRefCount > 0` guard fires); three emits inside the `flushCache` evictions (lines ~583, ~606, ~628). All four use a single shared per-process unique-set dedup keyed by `(turn-bucket, nodeIdx)` so total log volume is bounded across the four sites.

**Explicitly NOT in this spec:** the latent bug where the three `flushCache` variants ignore `neverFLUSH` (terrain loaders pass `nFlush=0x1` expecting permanent pinning, but `flushCache` can still evict them). That is a separate, sibling issue with its own behavior-change risk profile (high cache pressure may surface `PAUSE("Out of texture handles!")` at `mclib/txmmgr.cpp:2475` instead of silent evict-and-reload). **Defer to a follow-up spec** — keep this commit's blast radius strictly to the new pin path.

### File-scope additions in `gos_static_prop_registry.cpp`

Three additions to the existing anonymous namespace, alongside the file's existing `s_enabled` / `s_trace` flags and `SP_TRACE` macro:

```cpp
namespace {
    // ... existing namespace contents (s_enabled, s_recipes, s_recipeRanges, etc.) ...

    // [TEX_LIFECYCLE v1] env gate — same flag used by mclib/txmmgr.cpp and
    // mclib/msl.cpp so all pin/unpin/evict events stream together under a
    // single MC2_TEX_LIFECYCLE_TRACE=1 invocation.
    static const bool s_texPinTrace =
        (getenv("MC2_TEX_LIFECYCLE_TRACE") != nullptr);
    #define TEX_LC_PIN(fmt, ...)                                            \
        do { if (s_texPinTrace) {                                           \
            printf("[TEX_LIFECYCLE v1] " fmt "\n", ##__VA_ARGS__);          \
            fflush(stdout);                                                 \
        } } while (0)

    // Pin-call accounting for the pin_summary event in destroy(). leakedPins
    // = totalPinCalls - totalUnpinCalls; non-zero is a refcount imbalance
    // bug. Counters reset to 0 in destroy() after summary emit so per-mission
    // accounting is clean across mission load/unload cycles.
    static uint64_t s_totalPinCalls   = 0;
    static uint64_t s_totalUnpinCalls = 0;
}
```

### Registry call sites — recipe-range-owned

Extend the existing `RecipeRange` struct in `GameOS/gameos/gos_static_prop_registry.cpp` (~line 16-30 area, the anonymous-namespace struct). Track B's plan also extends this struct (with `registeredOnFrame` / `firstFlushSeen` fields) — this spec adds two more fields. **Both extensions must coexist; lockstep with Track B is required.**

```cpp
struct RecipeRange {
    uint32_t       first;
    uint32_t       count;
    TG_MultiShape* multi;
    // Track B (sibling plan):
    uint32_t       registeredOnFrame;
    bool           firstFlushSeen;
    // This spec:
    std::vector<DWORD> pinnedTextureNodes;  // exact nodes pinned for this range
    bool               pinsReleased;        // guards against double-release
};
```

#### `registerRecipe` (gos_static_prop_registry.cpp ~line 90-102)

After the `s_recipeRanges.push_back(rng)` call, walk the underlying type's texture list via the existing `TG_MultiShape` public API and pin each `mcTextureNodeIndex`. Record what was pinned in `rng.pinnedTextureNodes` for paired release.

**Use the existing public API. Do not access `TG_TypeMultiShape::numTextures` or `listOfTextures` directly — those are protected (`mclib/msl.h:75,77`).**

- `TG_MultiShape::GetNumTextures()` is public at `mclib/msl.h:454-457`; delegates to `myMultiType->GetNumTextures()`.
- `TG_MultiShape::GetTextureHandle(j)` is public at `mclib/msl.h:467-470`; returns `myMultiType->listOfTextures[j].mcTextureNodeIndex` directly — exactly the value we want to pin.

```cpp
// Find the RecipeRange we just pushed (in-place mutation):
RecipeRange& storedRng = s_recipeRanges.back();
storedRng.pinsReleased = false;
if (multi && mcTextureManager) {
    const long numTex = multi->GetNumTextures();
    for (long j = 0; j < numTex; ++j) {
        const DWORD nodeIdx = multi->GetTextureHandle(j);
        if (nodeIdx != 0xffffffff) {
            mcTextureManager->pinNode(nodeIdx);
            storedRng.pinnedTextureNodes.push_back(nodeIdx);
            ++s_totalPinCalls;
            TEX_LC_PIN("event=pin nodeIdx=%lu refcount=%lu regIdx=%d multi=%p",
                       (unsigned long)nodeIdx,
                       (unsigned long)mcTextureManager->getPinCount(nodeIdx),
                       regIdx, (void*)multi);
        }
    }
}
```

No new accessor in `msl.h` is required — `GetNumTextures()` + `GetTextureHandle()` already provide everything the pin path needs.

#### `invalidate(regIdx)` (gos_static_prop_registry.cpp ~line 111-120)

Before tombstoning the range (`rng.count = 0; rng.multi = nullptr`), release pins.

```cpp
void invalidate(int32_t regIdx) {
    if (!s_enabled) return;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    RecipeRange& rng = s_recipeRanges[static_cast<uint32_t>(regIdx)];
    releasePinsForRange(rng);   // NEW
    for (uint32_t i = 0; i < rng.count; ++i)
        s_recipes[rng.first + i] = GpuStaticPropInstance{};
    SP_TRACE("invalidate regIdx=%d (was count=%u)", regIdx, rng.count);
    rng.count = 0;
    rng.multi  = nullptr;
}

// Free function in the same anonymous namespace:
void releasePinsForRange(RecipeRange& rng) {
    if (rng.pinsReleased) return;  // double-release guard
    if (mcTextureManager) {
        for (DWORD nodeIdx : rng.pinnedTextureNodes) {
            mcTextureManager->unpinNode(nodeIdx);
            ++s_totalUnpinCalls;
            TEX_LC_PIN("event=unpin nodeIdx=%lu refcount=%lu",
                       (unsigned long)nodeIdx,
                       (unsigned long)mcTextureManager->getPinCount(nodeIdx));
        }
    }
    rng.pinnedTextureNodes.clear();
    // No shrink_to_fit() — parent s_recipeRanges is also cleared on
    // destroy(), so the per-range vector destructor handles deallocation.
    rng.pinsReleased = true;
}
```

#### Combined `destroy()` body — AUTHORITATIVE when both this spec and Track B land

`gos_static_prop_registry.cpp:79-83` (the current 4-line `destroy`) is mutated by BOTH this spec and `docs/superpowers/plans/2026-05-06-track-b-widen-registry.md` Step 4.3. Whichever lands first will silently drop the other's additions unless the implementer uses the combined body below. **This is the authoritative version — both plans defer to it.**

```cpp
void destroy() {
    // Track B (Step 4.3): first-frame skip-count summary.
    fprintf(stderr,
        "[STATIC_FIRST_FRAME v1] event=summary skip_count=%llu\n",
        (unsigned long long)s_firstFrameSkipCount);
    fflush(stderr);
    s_firstFrameSkipCount = 0;

    // Texture-pin spec: release any unreleased pins (mission-teardown
    // safety net — covers ranges that were never explicitly invalidated).
    for (auto& rng : s_recipeRanges) {
        releasePinsForRange(rng);
    }

    // Texture-pin spec: pin-call accounting summary.  leakedPins != 0 is a
    // bug signal (refcount imbalance between registerRecipe and invalidate).
    if (s_texPinTrace) {
        printf("[TEX_LIFECYCLE v1] event=pin_summary mission_end "
               "totalPinCalls=%llu totalUnpinCalls=%llu leakedPins=%lld\n",
               (unsigned long long)s_totalPinCalls,
               (unsigned long long)s_totalUnpinCalls,
               (long long)(s_totalPinCalls - s_totalUnpinCalls));
        fflush(stdout);
    }
    s_totalPinCalls   = 0;
    s_totalUnpinCalls = 0;

    // Existing teardown (unchanged).
    s_recipes.clear();          s_recipes.shrink_to_fit();
    s_recipeRanges.clear();     s_recipeRanges.shrink_to_fit();
    s_liveRangeIndices.clear(); s_liveRangeIndices.shrink_to_fit();
}
```

If only one of the two specs has landed at the time of merge, the implementer copies only the corresponding section into `destroy()` and leaves a comment block referencing the other spec's pending addition, so the next merge-train sees the gap.

### First-render fallback path inherits the pin path automatically

The current first-render fallback at `bdactor.cpp:1668` (`BldgAppearance`) and `:4313` (`TreeAppearance`) calls `GpuStaticPropRegistry::registerRecipe` directly. Because the pin happens *inside* `registerRecipe`, the fallback gets pinning for free. No second pin path is required.

When Track B's HC-3 retirement of the first-render fallback proceeds (gated on `event=type_unknown_at_late_spawn` invariant proof), the texture-pin path follows automatically — Track B's mission-load bulk register will also call `registerRecipe` (or its equivalent), which already pins. **Confirms the advisor requirement that the fallback must use the same pin path while it remains.**

## Instrumentation

Reuse the `[TEX_LIFECYCLE v1]` schema, gated by the existing `MC2_TEX_LIFECYCLE_TRACE=1` env var (already wired in `mclib/txmmgr.cpp`, `mclib/msl.cpp`, `gos_static_prop_batcher.cpp`). New event types:

```text
[TEX_LIFECYCLE v1] event=pin nodeIdx=N refcount=R regIdx=I typeMulti=0x...
[TEX_LIFECYCLE v1] event=unpin nodeIdx=N refcount=R
[TEX_LIFECYCLE v1] event=evict_skipped reason=pinned nodeIdx=N pinRefCount=R
[TEX_LIFECYCLE v1] event=pin_summary mission_end totalPinCalls=P totalUnpinCalls=U leakedPins=L
```

`event=evict_skipped` MUST use the same per-process unique-set dedup as `event=draw_black` (already in tree at the batcher, ~line 1779 area). Without dedup, ~thousands of pinned nodes × 60Hz eviction sweeps = unsustainable log volume. Dedup pattern:

```cpp
static thread_local std::unordered_set<uint64_t> s_seenEvictSkip;
const uint64_t key = (uint64_t(turn) >> 6 << 32) | uint64_t(nodeIdx);  // bucket per ~60-turn window
if (s_seenEvictSkip.insert(key).second) { ... emit ... }
```

The `pin_summary` line at mission unload makes refcount leaks visible in tier1 logs without grep gymnastics: `leakedPins != 0` is a clear bug signal.

## Verification plan

### Phase 1 — current architecture (pre-Track-B)

```bash
MC2_STATIC_UPDATE_SKIP=1 MC2_TEX_LIFECYCLE_TRACE=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs
```

**Pass criteria:**

1. `grep -c 'event=draw_black' <log>` → **0**. Zero static-prop draws with `glTexId=0` (was 39 in the diagnostic run).
2. `grep -c 'event=evict_skipped reason=pinned' <log>` → **>0**. Pin mechanism is firing (proof of execution).
3. `grep -c 'event=evict ' <log>` → **>0** but reduced. Eviction still works for unpinned textures.
4. `event=pin_summary` line shows `leakedPins == 0`. No refcount leak across mission load/unload.
5. **Out-of-handles gate (mandatory soak):** Run mc2_24 with `--duration 90 MC2_STATIC_UPDATE_SKIP=1 MC2_TEX_LIFECYCLE_TRACE=1` unconditionally. `grep -c 'Out of texture handles' <log>` → **0**. mc2_24 has the highest texture variety per the smoke gate and the 90s soak is the canary for any pool-pressure surface. Failure here means the pin path is reserving more texture pool than the engine can spare — investigate before defaulting-on. **Do not skip this gate even if standard 30s tier1 looks clean** — pool-pressure failures are duration-sensitive.
6. **Interactive (user-driven):** `MC2_STATIC_UPDATE_SKIP=1` + 30s play + pause/unpause. Trees, props, fences render correctly throughout. The minority "didn't clear on pause" subset from the diagnostic run resolves alongside the 99% case.

### Phase 2 — Track B mode (when Track B's mission-load bulk register is enabled)

7. With `MC2_STATIC_PROP_MISSION_LOAD_REG=1` (Track B env flag): widened registered population must not leak pins (`leakedPins == 0`), must not explode pin counts beyond `DWORD` range (`event=pin_summary` totalPinCalls < 10M as a sanity ceiling), must not surface `Out of texture handles`.
8. `event=type_unknown_at_late_spawn` (Track B HC-3 invariant counter): if non-zero AND `event=draw_black` is non-zero in the same run, the fallback's pin path is missing some types — investigate before flipping anything default-on.

### Phase 3 — baseline regression

9. Tier1 with `MC2_STATIC_UPDATE_SKIP=0` (default): no FPS regression vs `main`, no `[DESTROY v1]` delta (per `tests/smoke/artifacts/<run>/report.md`), no GL errors. This proves we haven't accidentally pinned in the default code path.

## Track B interaction (replacement language for the Track B plan's coordination block)

> **Texture-pin prep closes the `MC2_STATIC_UPDATE_SKIP=1` black-billboard regression** by giving registry-owned static-prop recipes a texture-lifetime contract. Pin ownership attaches to `GpuStaticPropRegistry`'s `RecipeRange` lifetime: `registerRecipe` pins every referenced `mcTextureNodeIndex` and records the pinned set on the range; `invalidate(regIdx)` releases pins for that range; `destroy()` is the mission-teardown safety net for any unreleased ranges. First-render fallback (`bdactor.cpp:1668`, `:4313`) routes through `registerRecipe` and inherits pinning automatically. **Alpha-test self-awareness remains a sibling hardening slice** for material classification (covers `treeDmgShape`/`GenericAppearance` paths that load alpha-test assets without `SetAlphaTest`); it is no longer the root-cause fix for the visible black-billboard symptom.

This replaces the current Track B plan's coordination block at lines 22-31 (which reads "Closes the `MC2_STATIC_UPDATE_SKIP=1` black-billboard regression" as a property of the alpha-test prep). That language is now stale and must be updated when this spec lands. **Track B plan update is the second half of the work — sequenced after this spec's adversarial review and implementation.**

## Anti-patterns to avoid (per advisor + handoff)

1. **Don't pin by `gosTextureHandle`.** Pin by `mcTextureNodeIndex`. The handle is the volatile cached GPU object; the node is the stable asset/cache identity.
2. **Don't globally disable eviction.** Bug is specific to registry-owned static-prop recipes whose update path is skipped.
3. **Don't fix by forcing static actors to call `update()` again.** Defeats the purpose of `MC2_STATIC_UPDATE_SKIP` and Track B.
4. **Don't use draw-time "if glTexId==0 then reload" as the fix.** Turns rendering into an implicit cache-repair path; may stall draw; hides the lifetime contract failure rather than fixing it.
5. **Don't repurpose `neverFLUSH` high bits as the refcount.** Conflates owner-pinned-permanently (bit 0, set at `loadTexture`) with registry-pinned-temporarily. Add a sibling `pinRefCount` field instead.
6. **Don't use a `bool pinned` field.** Refcount is required because trace confirmed multiple typeIDs share the same nodeIdx.
7. **Don't pin in `flush()` / draw path.** Pin at `registerRecipe` (lifecycle event), unpin at `invalidate`/`destroy`. Per-draw pinning fights eviction in a hot loop and breaks the lifecycle-only instrumentation rule (`memory/debug_instrumentation_rule.md`).
8. **Don't store pin ownership in a global `s_pinnedNodes` vector loosely correlated with recipes.** Track B introduces paired register/unregister/invalidate semantics; ownership must live on `RecipeRange` itself so per-range release is unambiguous.
9. **Don't ship the latent flushCache `neverFLUSH` tightening in this commit.** Defer to a sibling spec — it has independent risk (out-of-handles surfacing) and independent value.
10. **Don't create a second pin path for the first-render fallback.** Pin lives inside `registerRecipe`; fallback already calls `registerRecipe`; same path serves both.
11. **Don't access `TG_TypeMultiShape` members directly from `gos_static_prop_registry.cpp`.** `numTextures` and `listOfTextures` are protected (`mclib/msl.h:75,77`) and inaccessible to non-friend callers. Use the public `TG_MultiShape::GetNumTextures()` (`mclib/msl.h:454-457`) and `TG_MultiShape::GetTextureHandle(j)` (`mclib/msl.h:467-470`) — the latter returns `mcTextureNodeIndex` directly, exactly the value to pin. Do not propose adding a new `getMultiType()` accessor; the existing public API is sufficient. (Caught by Sonnet adversarial review of rev 2; the broken code had `multi->getMultiType()->numTextures` which fails to compile on three counts.)

## Verification appendix (per docs-discipline rule — every cited symbol grep-confirmed at write-time)

| Cited claim | File:line | Verification |
|---|---|---|
| `MC_TextureNode.neverFLUSH` field | `mclib/txmmgr.h:138` | grep `neverFLUSH` (this session) |
| `MC_TextureNode.uniqueInstance` field | `mclib/txmmgr.h:137` | grep (this session) |
| `init()` initialization band | `mclib/txmmgr.h:168-191` | Read offset 125-185 (this session) |
| Per-frame eviction predicate | `mclib/txmmgr.cpp` ~2035 (predicate) / ~2040 (`gos_DestroyTexture`) | Predicate line shifts with in-tree instrumentation; implementer re-greps `!(masterTextureNodes\[i\]\.neverFLUSH & 1)` |
| `flushCache` eviction predicate sites | `mclib/txmmgr.cpp` ~583 / ~606 / ~628 | grep `!masterTextureNodes\[i\]\.uniqueInstance` |
| `flushCache` `gos_DestroyTexture` calls | `mclib/txmmgr.cpp` ~590 / ~613 / ~635 | one site per predicate, ~7 lines below |
| Pause-time eviction gate | `code/mission.cpp:509-512` | quoted in handoff and `memory/pause_unpause_diagnostic_for_static_render_bugs.md` (Sonnet review noted comment is :509, branch is :511-512) |
| `MC2_STATIC_UPDATE_SKIP` substitution branch | `code/terrobj.cpp:740-755` | Read offset 695-770 (this session) — `if (s_staticUpdateSkip && gpuPath ...) ... touch();` else `... update();` |
| `MC2_STATIC_UPDATE_SKIP` comment block | `code/terrobj.cpp:705-716` | Read offset 695-770 (do not confuse with the actual branch above) |
| `registerRecipe` definition | `GameOS/gameos/gos_static_prop_registry.cpp:90-102` | Read offset 75-130 (this session) |
| `invalidate` definition | `GameOS/gameos/gos_static_prop_registry.cpp:111-120` | Read offset 75-130 (this session) |
| `destroy` definition | `GameOS/gameos/gos_static_prop_registry.cpp:79-83` | Read offset 75-130 (this session) |
| `registerRecipe` callsites | `mclib/bdactor.cpp:1668`, `mclib/bdactor.cpp:4313` | grep `registerRecipe` (this session) |
| `registerMultiShape` callsites | `mclib/bdactor.cpp:787-790`, `:3699-3702`, `mclib/genactor.cpp:387-388`, `mclib/gvactor.cpp:1028-1030` | grep (this session) |
| Draw-time stale-handle resolve | `gos_static_prop_batcher.cpp:1762-1765` | grep + Read this session (Sonnet review confirmed line) |
| `TG_TypeMultiShape::SetTextureHandle` definition | `mclib/msl.cpp:996` (signature comment at :989) | grep (Sonnet review noted off-by-7-line correction) |
| `loadTexture` sets `neverFLUSH = nFlush` | `mclib/txmmgr.cpp:2250` | grep `masterTextureNodes\[i\]\.neverFLUSH = nFlush` (this session) |
| `masterTextureNodes[0].neverFLUSH = 0x1` | `mclib/txmmgr.cpp:275` | grep (this session) |
| `CACHED_OUT_HANDLE = 0xFFFFFACE` | `mclib/txmmgr.h:45` | grep (this session) |
| `Out of texture handles` PAUSE point | `mclib/txmmgr.cpp:2475` | grep (Sonnet review confirmed; rev 2 cited :2473) |
| `terrtxm.cpp` uses `nFlush=0x1` (terrain pin) | `mclib/terrtxm.cpp:270, 412, 426, 439, 452, 465` | grep (Sonnet review verified) |
| Track B's RecipeRange struct extension | `docs/superpowers/plans/2026-05-06-track-b-widen-registry.md:544-554` | Read this session (rev 2 cited :518-524 pre-update) |
| `TG_MultiShape::GetNumTextures()` (public) | `mclib/msl.h:454-457` | grep (Sonnet review verified) |
| `TG_MultiShape::GetTextureHandle(j)` (public, returns `mcTextureNodeIndex`) | `mclib/msl.h:467-470` | grep (Sonnet review verified) — replaces the previously-flagged "accessor TBD" gap |
| `TG_TypeMultiShape::numTextures` is protected | `mclib/msl.h:75` | grep (Sonnet review verified — direct access from `gos_static_prop_registry.cpp` would fail to compile) |
| `TG_TypeMultiShape::listOfTextures` is protected | `mclib/msl.h:77` | grep (Sonnet review verified — direct access would fail to compile) |

## Open-question dispositions (from adversarial review)

| # | Question | Disposition |
|---|---|---|
| 1 | `pinRefCount` naming | OK as-is. Sibling-to-`neverFLUSH` is unambiguous; `staticPropRefs` is also defensible but doesn't add clarity. |
| 2 | Where is registry teardown | **`GpuStaticPropRegistry::destroy()` at `gos_static_prop_registry.cpp:79-83`**, called per-mission from `code/mission.cpp:3174` (advisor verified). Spec now names it explicitly. |
| 3 | Pin on fresh-loaded node before first update | Safe by construction. Eviction predicates short-circuit on `CACHED_OUT_HANDLE` and `0xffffffff`; pin is independent of realization state. Documented inline in spec. |
| 4 | `pinNode` on already-evicted (`CACHED_OUT_HANDLE`) | Safe — pin operates on cache-slot identity, not GPU object. Re-realization in `get_gosTextureHandle` fills the slot; subsequent eviction sees `pinRefCount > 0` and skips. Documented inline. |
| 5 | Latent `flushCache` `neverFLUSH` bug — current impact | **Deferred to sibling spec.** Tightening surfaces previously-masked pool exhaustion (`PAUSE("Out of texture handles!")`) under high cache pressure. Independent risk profile. This spec only adds `pinRefCount` checks (purely additive). |
| 6 | Track B bulk-register pin overflow | No realistic risk. Track B registers per-TYPE not per-INSTANCE; DWORD = 4 billion ceiling; max plausible refcount low millions. Sanity ceiling 10M added to verification. |
| 7 | `event=evict_skipped` rate-limiting | Required. Per-process unique-set dedup pattern (matches existing `event=draw_black` at batcher line ~1779). Spec spells out the dedup snippet. |

## Architectural decisions sign-off (from review)

| # | Decision | Resolution |
|---|---|---|
| 1 | Pin tracking location: batcher vs registry | **Registry, recipe-range-owned.** Pin ownership lives on `RecipeRange.pinnedTextureNodes`; release is paired with `invalidate`/`destroy`. |
| 2 | `flush()` predicate for mission teardown | Not modified by this spec. Pin/unpin lifecycle is sufficient; `flush()` predicate change is the deferred sibling spec. |
| 3 | Harden `removeTexture`/`removeTextureNode` against pinned-node destruction | Out of scope for v1. Spec explicitly assumes no overlap between `mcTextureManager->removeTexture` callers and registry-pinned nodes. `pinNode`'s `numUsers > 0` assertion (per M5) catches violations of this assumption loudly during dev/testing. If the assertion ever fires in production, follow-up spec needed. |
| 4 | Latent `flushCache` `neverFLUSH` fix in same commit | **Deferred** — separate sibling spec. |
| 5 | High-cache-pressure verification gate | Phase 1 criterion 5 — mc2_24 90s soak runs unconditionally as the canary (rev 3 made this mandatory; rev 2 hedged it as conditional). |
| 6 | Public-API access vs new accessor for `multi → listOfTextures` walk | **Use existing public API.** `TG_MultiShape::GetNumTextures()` + `GetTextureHandle(j)` (`msl.h:454-470`) provide the value directly. No new accessor in `msl.h`. (Sonnet review caught rev 2's compile error; rev 3 fixes the `registerRecipe` body.) |
| 7 | Combined `destroy()` body authority | **This spec is authoritative** for the merged `destroy()` body when both this and Track B's Step 4.3 land. The body is shown explicitly in the "Combined `destroy()` body" section above. Track B plan defers to this section to prevent silent additions-drop on first-merge. |

## Track B interaction summary

This fix is **Track B-compatible and Track B-required**. Track B widens `GpuStaticPropRegistry` from cull-approved fast-path replay to all world-static-prop instances. Without registry-owned texture lifetime, Track B amplifies the stale-handle exposure surface. The pin path attaches to `RecipeRange` lifetime — same lifetime as Track B's `registeredOnFrame` / `firstFlushSeen` extension — so the two extensions stack cleanly.

**Sequence:**

1. This spec → adversarial review → implementation → Phase 1 verification.
2. Track B plan update: replace stale "alpha-test prep closes black-billboard regression" coordination text with the language in this spec's Track B Interaction section. Add `pinnedTextureNodes` / `pinsReleased` to the Track B `RecipeRange` extension table.
3. Track B execution proceeds with both extensions in place. Phase 2 verification gates run with Track B enabled.

## Scope boundaries

- **In scope:** `mclib/txmmgr.{h,cpp}` (field + API + four predicate adds + instrumentation), `GameOS/gameos/gos_static_prop_registry.{h,cpp}` (RecipeRange field + pin call in registerRecipe + release in invalidate/destroy), `mclib/msl.h` if a `TG_MultiShape::getTypeMulti()`-style accessor is needed.
- **Out of scope:** Path 4 alpha-test work (in tree, stays). Track B's bulk-register change (sister plan). Any change to `appearance->touch()` semantics. Any change to `mcTextureManager->update()`'s `lastUsed <= turn-60` policy. Tightening the `flushCache` `neverFLUSH` predicate (sibling spec).
- **Sequenced after this spec lands:** update Track B plan's coordination language and `RecipeRange` extension table.
- **Deferred:** Whether to default-on-flip `MC2_STATIC_UPDATE_SKIP=1` after this fix. Separate decision with its own tier1 + interactive verification gate.

## Definition of done

1. Pass criteria 1-9 above met (Phases 1, 2 if Track B available, and 3).
2. Spec adversarial-plan-reviewed; review findings addressed (or explicitly waived with rationale in this doc).
3. Commit lands with `[TEX_LIFECYCLE v1]` pin/unpin/evict_skipped/pin_summary instrumentation gated off-by-default and demoted-not-deleted post-stabilization (per `memory/debug_instrumentation_rule.md`).
4. **Track B plan update committed in the same merge train**: coordination block replaced with this spec's Track B Interaction section; `RecipeRange` extension table updated.
5. Handoff doc `2026-05-06-static-prop-texture-cache-handoff.md` updated to reflect that this spec is the chosen fix path.
6. `memory/update_skip_touch_regression.md` rewritten with the confirmed root-cause framing (replacing the current "lighting wrong" text).
7. Sibling spec for `flushCache` `neverFLUSH` predicate-tightening filed as a follow-up.
