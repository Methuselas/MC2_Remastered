# M2.6 Recon

Reconnaissance for RenderWorld Slice M2.6 — Mech Pickup Integration. No
code edits made. All file:line citations grep-verified at write time
against HEAD of `claude/nifty-mendeleev` worktree.

## Summary

- **`tryGameplayPick` substrate is already kind-agnostic and reusable verbatim.** Its `Outcome` enum + `LookupResult` carry no static-prop-specific state; the M1.6 category gate (`IsStaticPropPickEnabled`) lives in the *caller wrapper*, not the spine. M2.6 adds a sibling caller wrapper, not a spine change.
- **Existing CPU pick (`ObjectManager::findObjectByMouse`) ALREADY picks both friendly and visible-hostile mechs.** GPU pick is therefore additive — the natural M2.6 niche is **fog-of-war-piercing hostile pick** (CPU pick nulls hostile targets at `missiongui.cpp:1273-1278` when sensor contact is below `CONTACT_SENSOR_QUALITY_1`). Whether M2.6 should pierce fog is a design decision (recommend: NO for default; YES under a dev/debug env-flag).
- **`LookupResult` currently does NOT expose `kind` or `debugCookie`.** The record carries both (`RenderWorld.h:146,149`), but `LookupResult` (`RenderWorld.h:157-167`) only surfaces mesh/material/lod/pipeline/drawPacket/pathReason/gameObjectId. M2.6 needs ONE field added: `RenderObjectKind kind`. (Optional: `debugCookie` for log only.)
- **Recommended handle→BattleMech path: Option D (populate `RenderObjectRecord.gameObjectId` with a watchID-style cookie that the adapter resolves on pick).** Option C (raw `Mech3DAppearance*` in `debugCookie`) is unsafe to dereference per the firewall comment at `RenderWorld.h:148` ("never dereferenced by engine"); Option A (sparse map in adapter) duplicates state the unified record already has slots for; Option B (linear scan of ~50 mechs) is cheap but bypasses the record table the substrate built for exactly this purpose.
- **Recommended gesture: same Shift+LMB shared with static-prop pick.** They never collide (CPU pick consumes friendly-mover Shift first via mover-first gate; static-prop pick lands on building/tree pixels; mech pick fills the hostile-mech-pixel gap). Unified `[GAMEPLAY_PICK v1] kind=Mech|StaticProp` log preferred over parallel schemas.

## Current mech selection — site map

The 4 `setSelected(true)` writer sites identified by M1.6 Q6:

| Site | File:Line | Style | Gesture | Mech kind selected | Notes |
|------|-----------|-------|---------|--------------------|-------|
| 1 | `code/missiongui.cpp:1472` | OldStyle | Shift+LMB, friendly + own-commander | **Friendly only** | Additive-select; line 1459 gates on `target->getCommanderId() == Commander::home->getId()`. Sets `moverSelectedThisFrame=true` at :1477. |
| 2 | `code/missiongui.cpp:1509` | OldStyle | Plain LMB, friendly + own-commander | **Friendly only** | Whack-all-then-add. Line 1496 gates on commander. Sets gate at :1512. |
| 3 | `code/missiongui.cpp:1736` | AOEStyle | Shift+LMB, own-commander | **Friendly only** | Mirrors site 1. Gate at :1741. |
| 4 | `code/missiongui.cpp:1761` | AOEStyle | Plain LMB, own-commander | **Friendly only** | Mirrors site 2. Gate at :1764. |

**Hostile mechs are NEVER `setSelected(true)`.** The closest hostile-mech interaction is `controlGui.setInfoWndMover((Mover*)target)` (info-window only) at sites :1444, :1495, :1525, :1675, :1714 — which fires on hostile + own-team mechs when `CONTACT_VISUAL` per `getContactStatus()` (visible to player).

**Hostile-mech click gestures today:**
- Plain LMB on hostile mech (visible) → `doAttack()` (`:1527`, `:1677`)
- Shift+LMB on hostile mech (visible) → `controlGui.setInfoWndMover((Mover*)target)` at :1525 if cursor is `mState_INFO`, otherwise `doAttack()`
- Plain or Shift LMB on hostile mech BELOW SENSOR THRESHOLD → `target=NULL` at :1277, click silently ignored (or hits whatever's behind it)
- Static-prop pick (M1.6) fires only when `moverSelectedThisFrame=false` AND substrate returns a non-zero pixel — so it never collides with hostile-mech-attack.

**Implication:** "Shift+LMB on hostile mech" today has two outcomes by visibility:
1. Visible hostile → info-window OR attack (gesture overload)
2. Fog-of-war hostile → silent no-op (CPU pick nulled target)

Case 2 is where the GPU pick has unique signal. The GPU writes `objectId` regardless of sensor state (mech.frag writes `Handle.raw()` per M2.5 unconditionally — see `[MECHBATCHER v1] event=mech_id_summary` counters showing 19K-1.2M writes/mission).

## tryGameplayPick extension surface

`code/gameplay_pick.h:23-40` — **`GameplayPickRequest`**:
- `mouseX, mouseY` (viewport space, top-left)
- `shiftDn, leftClicked, bGui, bLeftDouble` (gesture flags)
- `moverSelectedThisFrame` (mover-first fallback)

**No category/kind field.** The dispatcher is kind-agnostic by design (header comment at `:6-7`: "both callers ... call `tryGameplayPick()` instead of duplicating the inline machinery"). M2.6 mech wrapper would build an *identical* request and dispatch to the same spine.

`code/gameplay_pick.h:57-67` — **`GameplayPickResult::Outcome`**:
- `skipped, gated, miss, hit` — **DOES NOT NEED a new variant for mech-vs-prop.** The `hit` outcome carries a `RenderWorld::LookupResult` which is where the kind discriminator should live. A new `Outcome::mechHit` would be a category leak into the spine.

`code/gameplay_pick.h:65` — **Result carries `RenderWorld::LookupResult lookup`** + `GameplayPickContext ctx`. The `LookupResult` (`RenderWorld/RenderWorld.h:157-167`) currently exposes:
- `isValid`, `handle`
- `meshHandleBits`, `materialHandleBits`, `lodLevel`, `pipelineId`, `drawPacketIndex`, `pathReasonCode`
- `gameObjectId`

**Gap:** `LookupResult` does NOT surface `RenderObjectRecord::kind` (defined at `RenderWorld/RenderWorld.h:146`) or `debugCookie` (`:149`). The record has both; `lookupAtPixel` at `RenderWorld/RenderWorld.cpp:717-726` copies 8 fields out but NOT kind/cookie. **M2.6 mandatory change: add `RenderObjectKind kind` to `LookupResult` + 1-line copy in `lookupAtPixel`.**

**How M1.6 consumes the result** — at `code/missiongui.cpp:6213-6271`: wrapper builds request, calls `tryGameplayPick(req)`, switches on `r.outcome`. On `hit`: calls `RenderWorld::setLastStaticPropPick(r.lookup, ...)` + emits `[STATIC_PROP_PICK v1] hit ...`. **No kind check.** Today this is safe because M1.6 shipped before mech IDs were written; post-M2.5, a Shift+click on a mech pixel will return `outcome=hit` with `kind=Mech`, and the M1.6 wrapper will store it as a static-prop pick + emit a misleading `recipe=...` log line. (`setLastStaticPropPick` looks up the recipe via the record table; for a mech record `recipeIndex` returns -1 or stale data.)

**M1.6-wrapper post-M2.5 bug (greybeard hat):** after M2.5 shipped, `tryStaticPropPick` should filter on `r.lookup.kind == RenderObjectKind::StaticProp` before consuming the hit. This is a 2-line fix that M2.6 should make alongside adding the kind field. Otherwise tier1 with `MC2_STATIC_PROP_PICK=1` will start logging garbage on mech-pixel Shift+clicks.

## Handle-to-game-object reachability — option analysis

When `lookupAtPixel(x,y)` returns `LookupResult{kind=Mech, handle.index()=N}` (where N >= `kMechHandleBase=0x00010000`), the caller needs to reach the `Mech3DAppearance*` or `BattleMech*`. Today, the only stored back-pointer is `RenderObjectRecord.debugCookie = reinterpret_cast<uintptr_t>(&mech3DAppearance)` (`GameAdapters/MechRenderAdapter.cpp:98`).

### Option A: sparse `handle.index() → Mech3DAppearance*` table in adapter

- **Pros:** keeps mapping in the adapter (correct firewall boundary); explicit lifetime mgmt; cleanly invalidates on `destroyMech`.
- **Cons:** duplicates state the unified record table already has slots for; another container to keep in sync with `s_objectRecords`; introduces a "the truth lives in two places" risk that the M2 design carefully avoided.
- **Verdict:** redundant; substrate already has the cookie.

### Option B: linear scan of live `BattleMech` objects, match by `getRenderWorldHandle().raw() == h.bits`

- **Pros:** zero new state; uses existing `ObjectManager->getMover(i)` + `BattleMech::getAppearance()` + `mech.getRenderWorldHandle()` chain (`mclib/mech3d.h:487-489`). Cheap (N ~50 mechs/mission); only fires on click (~10/sec max).
- **Cons:** Requires reach from `RenderWorld`/`gameplay_pick` to `BattleMech` (firewall violation if done in spine; OK if done in M2.6 caller wrapper in `missiongui.cpp` which already includes everything). O(N) — fine for picks but smells like "we have a perfectly good index in our hand".
- **Verdict:** Acceptable; pragmatic; no new state. **Reasonable runner-up.**

### Option C: store raw `Mech3DAppearance*` in `RenderObjectRecord.debugCookie` (ALREADY done in M2)

- **Pros:** Zero new state; cookie is already populated at `MechRenderAdapter.cpp:98`.
- **Cons:** **Explicit firewall violation per `RenderWorld.h:148` comment: "opaque debug cookie. ... never dereferenced by engine."** Using this for live game logic re-types the field from "log-only" to "load-bearing pointer", which means stale-handle safety (mech destroyed between click and pick) now depends on careful cleanup. The cookie is cleared on `destroyMech` → `clearAllMechRecords` paths but the record's `flags & alive` bit is the authoritative liveness signal, and the cookie field semantics weren't designed to encode that.
- **Verdict:** Violates documented design intent; would require renaming `debugCookie` and re-spec'ing its lifecycle. **Don't.**

### Option D: populate `RenderObjectRecord.gameObjectId` with a stable engine-side cookie; adapter resolves on pick

- The `gameObjectId` field (`RenderWorld.h:142`) already exists as a "u32 opaque engine-side cookie", is `0` in M2 (`MechRenderAdapter.cpp:97`), and IS already surfaced in `LookupResult.gameObjectId` (`RenderWorld.cpp:725`).
- M2.6 populates it from `BattleMech`'s watchID (or `partId`, or `getHandle()` — the engine-side stable object IDs MC2 already uses for save/load).
- On pick: `MechRenderAdapter` exposes a free function `BattleMech* findMechByGameObjectId(uint32_t)` that does the resolution. Either O(N) linear (cheap) OR adapter maintains a small `unordered_map<u32, BattleMech*>` updated on `syncSpawn`/`destroyMech`.
- **Pros:** Uses the field exactly as designed; surfaces through existing `LookupResult.gameObjectId` (no new field); the resolution function lives in `GameAdapters/MechRenderAdapter` where the firewall puts the bridge code; cookie semantics are clean (not "this is a pointer, don't deref" — it's an opaque integer that adapter knows how to translate).
- **Cons:** Needs to choose the cookie source (watchID vs partId vs new field) and ensure stability across destroy/respawn. M2.6 should NOT invent a new ID system — pick something MC2 already serializes for save-games.
- **Verdict:** **Recommended.** Matches the field's documented intent; matches the M2 adapter pattern; uses already-surfaced `LookupResult` field; lifecycle is naturally tied to the record (cleared in `destroyMech`).

### Recommendation

**Option D for the resolution path + Option B for the resolver implementation** (linear scan in adapter; O(50) per click; no new container). If profiling later shows it matters (won't), promote to an `unordered_map` inside the adapter.

Concretely:
1. `MechRenderAdapter::syncSpawn` populates `desc.gameObjectId = mech.getOwner()->getWatchID()` (or equivalent stable engine-side ID — needs grep of `BattleMech` to confirm best field).
2. `RenderWorld::registerMech` already stores `gameObjectId` in the record (verify at `RenderWorld.cpp` registerMech impl — I have not opened that function; M2.6 plan must grep-confirm).
3. New free function `GameAdapters::Mech::findByGameObjectId(uint32_t) -> BattleMech*` in `MechRenderAdapter.{h,cpp}`. Body: iterate `ObjectManager->getNumMovers()` + `getMover(i)`, return first match on `BattleMech::getWatchID() == id`. Returns NULL on miss (stale handle / destroyed mech).
4. M2.6 caller wrapper: on `outcome=hit && kind=Mech`, call resolver; NULL result = stale, log + no-op; non-NULL = the mech to "pick".

**Stale-handle safety:** `lookupAtPixel` already filters on generation + alive (`RenderWorld.cpp:710-715`), so a destroyed mech's old pixel returns `isValid=false`. The only remaining race is a mech destroyed between `lookupAtPixel` returning and the resolver running on the main thread — single-threaded gameplay-pick path (per `gameplay_pick.h:77` "main thread only") means this race is impossible at the gesture level. The resolver returning NULL is the defensive belt.

## Gesture mapping recommendation

**Use the same Shift+LMB gesture shared with static-prop pick. Single category dispatch via shared `tryGameplayPick` spine.**

Rationale:
1. **No collision exists.** Mover-first gate at `missiongui.cpp:1539` (legacy OldStyle) and `:1782` (AOEStyle) preserves friendly-mech behavior verbatim. CPU `findObjectByMouse` consumes friendly mechs first; if friendly mover was Shift+selected, `moverSelectedThisFrame=true` and `tryGameplayPick` returns `Outcome::gated` without reading any pixel.
2. **Hostile-mech pixels under cursor today produce one of two CPU behaviors:** visible-hostile (`target` set; `setInfoWndMover` or `doAttack` fires under various sub-gestures) or fog-of-war-hostile (`target=NULL`). In BOTH cases, `moverSelectedThisFrame` stays `false`, so the GPU pick path is reachable.
3. **Three distinct outcomes by intent:**
   - Friendly mech under cursor: CPU path wins (mover-first short-circuit fires; no GPU pick).
   - Static prop under cursor: GPU pick fires, returns `kind=StaticProp` → M1.6 wrapper consumes it.
   - Hostile mech under cursor (visible OR fog-of-war): GPU pick fires, returns `kind=Mech` → M2.6 wrapper consumes it.
4. **The "Shift+LMB on hostile mech triggers attack" overload:** today, Shift+LMB on a visible hostile mech does NOT enter the `target->getCommanderId() == Commander::home->getId()` branch (it's hostile) — control falls through to the `else` at `:1519`, which fires `doAttack()` for visible hostiles. The GPU pick at the tail of the function fires AFTER the attack, so M2.6 on a visible hostile would emit both an attack AND a `[GAMEPLAY_PICK v1] kind=Mech` log line. This is **ambiguous: is M2.6 supposed to inspect (log only) or replace the attack?** Strong recommendation: **inspect-only in v1**, matching M1.6 semantics — log + update debug state, do NOT mutate gameplay. Visible-hostile attack continues to work via CPU path; M2.6 adds passive observation. The user-visible distinction "Shift gives me both an attack AND an inspect log" is acceptable for v1 since the log is debug-only.

**Alternative gesture (Ctrl+LMB)** — REJECTED. Adds a new modifier the engine doesn't currently bind for pick; creates discoverability + accessibility issues; no clear benefit over sharing Shift+LMB given there's no collision.

**Fog-of-war consideration (load-bearing design Q):** the CPU pick nulls under-threshold hostile targets at `:1273-1278`. M2.6 GPU-pick fires regardless of sensor state (M2.5 writes IDs for ALL drawn mechs unconditionally per `mech.frag` write at fragment level). **Recommendation: M2.6 v1 RESPECTS fog-of-war** — the caller wrapper, after resolving handle → BattleMech, checks `mech->getContactStatus(Team::home->getId(), true) >= CONTACT_SENSOR_QUALITY_1`, and emits `[GAMEPLAY_PICK v1] miss reason=fog_of_war` on under-threshold targets. Dev override `MC2_MECH_PICK_PIERCE_FOG=1` for debug. This preserves stock gameplay rules (cannot incidentally reveal undetected enemy mechs via inspect log) while keeping the substrate maximally useful.

## Debug-state slot recommendation

**Recommend: unified `GameplaySelectionDebugState` carrying a `RenderObjectKind kind` discriminator + a union/variant of kind-specific data.**

Rationale:
- M1.6's `StaticPropSelectionDebugState` (`RenderWorld.h:185-194`) is a single-slot mutex-guarded latest-wins struct holding `handle`, `recipeIndex`, mouse/GL coords, frame index. Adding a parallel `MechSelectionDebugState` doubles the mutex/clear/destroy lifecycle code for no real gain — there is only one "most recent pick" semantically (the user clicked once).
- A unified struct with `kind` + a small union (recipeIndex for StaticProp; gameObjectId/mech-pointer for Mech) keeps the mutex/clear/destroy plumbing single-sourced.
- Migration: rename `StaticPropSelectionDebugState` → `GameplaySelectionDebugState`; add `kind`; deprecate `setLastStaticPropPick`/`getLastStaticPropPick` (forward to unified `setLastGameplayPick`/`getLastGameplayPick`); keep the old names as 1-line shims for one release to avoid M1.6 caller churn.
- **`destroy()` lifecycle hook:** unchanged — single `clearLastGameplayPick()` called from `RenderWorld::destroy()`.

If the refactor is judged too invasive for a single slice: ship parallel `MechSelectionDebugState` for M2.6 and pay the unification debt at M3+ when terrain/VFX kinds land. (Greybeard ruling preference: META-FIX now, not 3 slices later when there are 4 parallel structs.)

## MLR-fallback edge case

Per M2.5 amendment + CLAUDE.md "Known issues" entry: `mlr_mech_draws=0` across all 5 tier1 missions. M2.6 ships WITHOUT conditional MLR fallback logic.

**Graceful no-op verification:** when a click lands on an MLR-rendered mech pixel:
1. MLR path does NOT write to attachment-2 (no `layout(location=2)` in MLR shader).
2. `lookupAtPixel` reads the cleared `0u` from attachment-2 at that pixel.
3. `raw == 0u` → `lookupAtPixel` returns `LookupResult{isValid=false}` at `RenderWorld.cpp:690-693`.
4. `tryGameplayPick` returns `Outcome::miss` at `gameplay_pick.cpp:127`.
5. M2.6 wrapper sees `outcome=miss`, no-ops (or emits debug-gated miss log).

**This is exactly the same code path as "Shift+click on sky/terrain"** — already exercised by M1.6 every frame on background pixels and confirmed working (M1.6 user-driven canary on mc2_03: 11 misses on terrain/sky). M2.6 inherits this behavior for free.

**Belt-and-braces sanity check (recommended for plan, not the slice):** add an M2.6 `[GAMEPLAY_PICK v1]` per-mission counter for `mech_pixel_misses` that increments when `r.lookup.kind == Mech` is requested but `outcome=miss` AND `mlr_mech_draws > 0` for the mission. If a future tier1 ever shows `mlr_mech_draws > 0`, this counter will flag the impacted mech-pick gaps; under current tier1 it stays 0.

## Log schema recommendation

**Recommend: unified `[GAMEPLAY_PICK v1] kind=Mech|StaticProp hit ...` schema. Deprecate `[STATIC_PROP_PICK v1]` after one release.**

Concrete schema (mirrors M1.6 field set + adds kind):
```
[GAMEPLAY_PICK v1] hit kind=StaticProp handle=N idx=N gen=N recipe=N screen=(x,y) gl=(x,y) fbo=(x,y) vMul=(W,H) vAdd=(X,Y) draw=(W,H)
[GAMEPLAY_PICK v1] hit kind=Mech       handle=N idx=N gen=N gameObjectId=N watchID=N mech=PTR screen=(x,y) gl=(x,y) fbo=(...) ...
[GAMEPLAY_PICK v1] miss screen=(x,y) gl=(x,y) ...
[GAMEPLAY_PICK v1] miss reason=fog_of_war kind=Mech handle=N ...   (M2.6 fog-respect path)
```

Rationale:
- Per CLAUDE.md "Debug instrumentation rule": `\[SUBSYS v[0-9]+\]` schema. M1.6 chose `STATIC_PROP_PICK` because it was the only kind; with M2.6 introducing a second consumer, the natural subsystem name is the spine (`GAMEPLAY_PICK`), not the category.
- Adopting unified schema NOW (before M2.6 ships its first log line) avoids a third schema rename when M3 terrain pick lands.
- **Backcompat:** keep `[STATIC_PROP_PICK v1]` lines emitted as well for one release if user-driven canary tooling parses them — but mark for removal in the M2.6 commit message. Mirror at adapter level (caller wrapper for static-prop emits BOTH `[STATIC_PROP_PICK v1] hit ...` AND `[GAMEPLAY_PICK v1] hit kind=StaticProp ...`).
- **Alternative (parallel `[MECH_PICK v1]`):** REJECTED — parallel schemas multiply per slice without any consumer asking for them; the M1.6 instrumentation rule favors one schema per subsystem, not one per category.

## File:line citations table

Every claim above grep-verified at write time (UTC 2026-05-23) against the
`claude/nifty-mendeleev` worktree HEAD.

| Claim | File | Line(s) | Verified |
|-------|------|---------|----------|
| `tryGameplayPick` dispatcher signature | `code/gameplay_pick.h` | 78 | yes |
| `GameplayPickRequest` field set | `code/gameplay_pick.h` | 23-40 | yes |
| `GameplayPickResult::Outcome` enum | `code/gameplay_pick.h` | 58-63 | yes |
| `Outcome::hit` carries `LookupResult` | `code/gameplay_pick.h` | 65 | yes |
| Spine implementation (env+gesture+mover+coord+lookup) | `code/gameplay_pick.cpp` | 79-129 | yes |
| `lookupAtPixel` returns `isValid=false` on `raw==0` | `RenderWorld/RenderWorld.cpp` | 690-693 | yes |
| `lookupAtPixel` generation+alive filter | `RenderWorld/RenderWorld.cpp` | 710-715 | yes |
| `LookupResult` fields (NO kind/cookie surface) | `RenderWorld/RenderWorld.h` | 157-167 | yes |
| `RenderObjectRecord.kind` and `.debugCookie` fields | `RenderWorld/RenderWorld.h` | 146,149 | yes |
| `RenderObjectKind` enum | `RenderWorld/RenderWorld.h` | 116-120 | yes |
| `registerMech` signature | `RenderWorld/RenderWorld.h` | 244 | yes |
| `RenderMechDesc` field set | `RenderWorld/RenderWorld.h` | 228-232 | yes |
| `kMechHandleBase = 0x00010000` | `RenderWorld/RenderWorld.cpp` | 114 | yes |
| `MechRenderAdapter::syncSpawn` populates `debugCookie = &mech` | `GameAdapters/MechRenderAdapter.cpp` | 98 | yes |
| `MechRenderAdapter::syncSpawn` populates `gameObjectId` = 0 (M2 unused slot) | `GameAdapters/MechRenderAdapter.cpp` | 97 | yes |
| `Mech3DAppearance::getRenderWorldHandle` accessor | `mclib/mech3d.h` | 487-489 | yes |
| `Mech3DAppearance::mechRenderHandle` storage | `mclib/mech3d.h` | 478 | yes |
| Mover site 1 (`setSelected(true)` + gate) — OldStyle Shift+additive | `code/missiongui.cpp` | 1472,1477 | yes |
| Mover site 2 — OldStyle plain LMB | `code/missiongui.cpp` | 1509,1512 | yes |
| Mover site 3 — AOEStyle Shift+additive | `code/missiongui.cpp` | 1736,1741 | yes |
| Mover site 4 — AOEStyle plain LMB | `code/missiongui.cpp` | 1761,1764 | yes |
| `findObjectByMouse` CPU pick | `code/missiongui.cpp` | 1267 | yes |
| `findObjectByMouse` impl (mover-first then commander=-1 fallback) | `code/objmgr.cpp` | 3093-3127 | yes |
| Fog-of-war target null at `missiongui.cpp` | `code/missiongui.cpp` | 1273-1278 | yes |
| `tryStaticPropPick` caller wrapper | `code/missiongui.cpp` | 6186-6271 | yes |
| `tryStaticPropPick` call from OldStyle tail | `code/missiongui.cpp` | 1539-1545 | yes |
| `tryStaticPropPick` call from AOEStyle tail | `code/missiongui.cpp` | 1782-1788 | yes |
| `[STATIC_PROP_PICK v1] hit` log format | `code/missiongui.cpp` | 6229-6241 | yes |
| `StaticPropSelectionDebugState` shape | `RenderWorld/RenderWorld.h` | 185-194 | yes |
| `IsStaticPropPickEnabled` category gate | `RenderWorld/RenderWorld.h` | 94 | yes |
| `setLastStaticPropPick`/`clearLastStaticPropPick`/`getLastStaticPropPick` | `RenderWorld/RenderWorld.h` | 201-213 | yes |
| `isLeftDoubleClick`, `shift()`, `ctrl()`, `alt()` available | `mclib/userinput.h` | 351,366,381,499 | yes |
| `GameObject::getAppearance` virtual | `code/gameobj.h` | 393 | yes |
| `setInfoWndMover` sites (hostile/own info-window) | `code/missiongui.cpp` | 1444,1495,1525,1675,1714 | yes |

RECON STATUS: COMPLETE
