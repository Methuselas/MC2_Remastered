# RenderWorld Slice M2.6 -- Mech Pickup Integration Spec (inspect-only v1)

- SPEC STATUS: REVISED -- adversarial CONDITIONAL-PASS (1 CRIT + 4 MAJOR) findings applied
- Date: 2026-05-23 (revised same-day post adversarial review)
- Author: spec-author session, post-M2.5 ship
- Predecessor slices (all SHIPPED 2026-05-23):
  - **M1.5** -- ObjectID buffer substrate (R32_UINT MRT attachment-2,
    `RenderWorld::lookupAtPixel`, `s_objectRecords` unified table).
    Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`
  - **M1.6** -- static-prop pick wiring (Shift+LMB, `[STATIC_PROP_PICK v1]`
    log, `StaticPropSelectionDebugState`).
    Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md`
  - **M2-pre** -- gameplay-pick META-FIX extraction
    (`tryGameplayPick(req)` spine + `screenToFboPixel(...)` coord helper).
    Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md`
  - **M2** -- route-only `MechRenderAdapter`; every live `Mech3DAppearance`
    carries a `RenderObjectHandle` (`mechRenderHandle`); unified
    `s_objectRecords` slot at `kMechHandleBase=0x00010000` with `kind=Mech`.
    Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md`
  - **M2.5** -- mech object-ID substrate; `mech.frag` emits
    `Handle.raw()` to `GL_COLOR_ATTACHMENT2` under
    `#ifdef MC2_OBJECT_ID_BUFFER`. `[MECH_OBJECT_ID_SELFTEST v1]` validates
    the substrate end-to-end. Empirical `mlr_mech_draws=0` across all
    tier1 missions.
    Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-spec.md`
- Pre-spec user resolutions (AUTHORITATIVE; overrides recon "leans"):
  `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-6-mech-pickup-spec-resolutions.md`
- Recon (mandatory companion; trust file:line, grep-verified at write-time):
  `docs/superpowers/explorations/2026-05-23-renderworld-slice-m2-6-mech-pickup-recon.md`
- Required follow-ups before EXECUTABLE:
  - `adversarial-plan-review` pass (per worktree CLAUDE.md "Review discipline")
  - `greybeard` pass (per worktree CLAUDE.md "Meta-fix discipline") --
    Section 13 already proposes the META-FIX ruling; the greybeard pass
    should rule on whether the unified-schema retirement at sample-size-
    of-two clears the bug-class hinge criterion.
- DOC-ONLY: no code in this artifact. Pseudocode in Sections 4, 5, 6, 7
  is illustrative.

## Adversarial findings applied (post-review revision)

Review file: `docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-6-spec-adversarial.md`
(CONDITIONAL PASS, 1 CRITICAL + 4 MAJOR + several MINOR).

| Finding | Pre-revision claim | Post-revision change |
|---|---|---|
| **CRITICAL-1** | `partId` was claimed lifetime-stable; spec proposed populating `desc.gameObjectId = (uint32_t)getPartId()` at `MechRenderAdapter::syncSpawn` so log + future select had a stable cookie (Option D + Option B). Reviewer proved `code/mission.cpp:2987` reassigns `partId` during group/commander setup AFTER `mover->init(true, objType)` (which invokes `syncSpawn` at `code/mech.cpp:1338`) has already stamped the cookie. The stability claim was WRONG. | **PIVOT to Option B ALONE.** Drop the cookie populate entirely. `code/mech.cpp:1338` keeps `syncSpawn(*m3d, 0u)` (no behavior delta vs M2 ship). Drop `getPartId()` from the spec entirely. Reverse lookup is a pure linear scan over `ObjectManager::getMover(i)` matching on `getRenderWorldHandle() == h`. The HANDLE is lifetime-stable (M2 destroyMech retires it; new mech gets new handle). No cookie maintenance needed. Stale-handle case is the nullptr branch. Log line emits handle bits/index/generation + the resolved mech pointer, NOT a `gameObjectId` field. Section 4.2, 6.1, 6.2, 6.3, 8 (table), 11 (log schema), 13/14 (threat model), 15 (resolved decisions) all updated. |
| **MAJOR-1** | Fog predicate said "`((Mover*)mech)->conStat < CONTACT_SENSOR_QUALITY_1`" -- missed the `!ShowMovers && !(MPlayer && MPlayer->allUnitsDestroyed[...])` outer carve-outs at `code/missiongui.cpp:1272`. Inconsistency: under ShowMovers, CPU pick selects the mech but M2.6 would be silent. | Section 6.3 fog predicate now mirrors the FULL gate verbatim from `missiongui.cpp:1272-1278` (spot-checked at revision time). `MC2_MECH_PICK_PIERCE_FOG=1` short-circuits the whole predicate (returns visible-unconditional). |
| **MAJOR-2** | Gate 6 grep set covered code but not `RenderWorld.h` doc comments (which retain `[STATIC_PROP_PICK v1]` references); env var name `MC2_STATIC_PROP_PICK` retained with no acknowledgment of the schema/var-name asymmetry. | Section 10 Gate 6 grep set now explicitly includes header doc comments (`**/*.h` + `**/*.cpp` for the literal `[STATIC_PROP_PICK v1]`, `StaticPropSelectionDebugState`, `setLastStaticPropPick`) AND `tests/`, `scripts/`, `.claude/` for any in-repo consumer. Section 9 (Gating) adds a paragraph explicitly acknowledging that the env-var name `MC2_STATIC_PROP_PICK` is INTENTIONALLY retained (back-compat with M1.6 user muscle memory; deferred rename owner = next CLAUDE.md known-env-var section refactor slice). |
| **MAJOR-3** | Q1 (in-repo `setLastStaticPropPick` consumer survey) was deferred to plan-stage; greybeard META-FIX-vs-PATCH ruling depends on knowing whether full retirement is possible. | Survey done NOW at spec stage. Greps over `tests/`, `scripts/`, `.claude/` (excluding the spec/planning/review docs that will be naturally updated): **ZERO in-repo consumers found.** Q1 moves from "Open" to "RESOLVED: full retirement" (no shim). Doc-comment hits in `RenderWorld.h:97` and `RenderWorld.cpp:93/484/599/605` will be renamed in the same META-FIX commit (now scoped explicitly in Section 5 + Gate 6 grep). |
| **MAJOR-4** | MLR fallback was acknowledged via M2.5 empirical `mlr_mech_draws=0` data with no conditional code chosen. Reviewer flagged: if `mlr_mech_draws > 0` EVER occurs (future regression, mod, edge mission), the gating must be per-INSTANCE (per-click), not per-mission. | Section 14 (T3) extended with explicit per-pixel/per-click graceful-no-op statement: substrate `lookupAtPixel` on an MLR-rendered mech's pixels returns `Handle::invalid()` (raw=0), spine outcome is `miss` for those specific clicks; system fails closed on a per-click basis. NO conditional code required even when MLR re-engages. |
| **MINOR (header decl)** | Spec said `findMechByHandle` header decl MAY be forward-declared. | Changed to MUST be forward-declared (matches M2 `ForAdapter` accessor pattern). Section 4.2. |
| **MINOR (CLAUDE.md env-vars)** | T-final docs commit listed CLAUDE.md M1.6-entry update but not the `MC2_MECH_PICK*` env-var section. | Section 5 + Section 14 T9 amended: T-final also adds `MC2_MECH_PICK`, `MC2_MECH_PICK_DEBUG`, `MC2_MECH_PICK_PIERCE_FOG` to CLAUDE.md "Tier-1 instrumentation env vars" section. |
| **MINOR (self-test step 6)** | Step 6 of `RunMechPickSelfTest` was at risk of being vacuous (linear scan at init-time returns nullptr trivially because no real mechs exist yet). | Section 10 Gate 4 step 6 reworded: the test validates the SCAN ITSELF (returns nullptr cleanly OR returns a non-null mech with handle matching), not the data state. |
| **MINOR (two-readback cost)** | Section 7 carried "< 200us per click" as a cost claim. | Flagged as "estimated; not measured" inline (Section 7). |

This document specifies the **gameplay-side wiring that closes the
RenderWorld pickup arc**: shift+LMB on a visible hostile mech now
returns a `kind=Mech` hit through the M2-pre spine, the mech is
resolved to a live `BattleMech*` via the adapter's reverse lookup,
and an inspect-only `[GAMEPLAY_PICK v1] hit kind=Mech ...` log line
fires. No selection, no attack routing, no command queue mutation.

M2.6 is also the META-FIX slice that retires the
`[STATIC_PROP_PICK v1]` log schema and the
`StaticPropSelectionDebugState` struct in favor of unified-by-kind
forms (`[GAMEPLAY_PICK v1] kind=...` log;
`GameplaySelectionDebugState` struct with a `RenderObjectKind kind`
discriminator). The unification is forced by adding `LookupResult.kind`
-- the moment that field exists, every consumer needs a kind-dispatch
discipline, and the schema/state surface naturally generalizes.

---

## 1. Purpose / non-goals

### Purpose

Close the M1 -> M1.5 -> M1.6 -> M2 -> M2.5 -> **M2.6** arc by making
mechs **pickable** through the same M2-pre spine that pushes static
props today. The substrate already round-trips mech handles (M2.5
self-test PASS). M2.6 is the gameplay-side consumer that turns "GPU
attachment-2 carries a mech handle" into "the player can shift+click
on a hostile mech and see its identity logged."

The slice is **strictly inspect-only**. It is the smallest delta that
proves the end-to-end loop works and exposes the substrate for
log-driven inspection / future HUD work. Selection, attack routing,
and any gameplay-state mutation are explicitly out-of-scope (see
non-goals); they land in a future M2.7 once the inspect path is
proven stable.

### META-FIX scope (this slice's bug-class retirement)

Per user resolution Q3=A (`spec-resolutions.md`):

1. **Latent post-M2.5 mislabel bug.** Today (post-M2.5,
   pre-M2.6) the M1.6 wrapper at
   `MissionInterfaceManager::tryStaticPropPick`
   (`code/missiongui.cpp:6186-6271`) assumes any `Outcome::hit` is a
   static-prop hit. M2.5 broke that invariant by adding mech writes to
   the same substrate. A Shift+click on a mech pixel today returns
   `outcome=hit` with a Mech handle, and the wrapper stores it as a
   static-prop pick + emits a misleading `recipe=-1` log line. M2.6
   fixes this with a 2-line kind guard.
2. **Log-schema retirement.** `[STATIC_PROP_PICK v1] hit ...` becomes
   `[GAMEPLAY_PICK v1] hit kind=StaticProp ...`; mech adds
   `[GAMEPLAY_PICK v1] hit kind=Mech ...`. The schema is unified
   AHEAD of M3/M4 (terrain, VFX) adding their own kind categories --
   retiring the bug class now is cheaper than retiring it 3 slices
   later when there are 4 parallel schemas.
3. **Debug-state retirement.**
   `RenderWorld::StaticPropSelectionDebugState` becomes
   `GameplaySelectionDebugState` with a `RenderObjectKind kind`
   discriminator. Setter becomes `setLastGameplayPick(kind, ...)`.
   Same mutex-guarded single slot serves all kinds.

These three retirements are coherent: `LookupResult.kind` (the
mandatory 2-line addition) is the load-bearing surface change. The
schema and state-slot retirements ride on top because every
consumer is now KIND-aware by compile-time signal.

### Non-goals (explicit)

- **Not selection.** `Team::setSelected`, attack routing, command
  queue mutation, mover selection state -- all untouched. The
  existing mover-first short-circuit in `tryGameplayPick`
  (`code/gameplay_pick.cpp` -- Outcome::gated) is preserved verbatim.
- **Not visible highlight.** Mech inspect produces a log line + a
  debug-state slot update only. No on-screen bounds box, no color
  tint, no HUD readout. M2.7+ territory.
- **Not friendly-mech pickup.** Friendly mechs land in the legacy
  mover-first short-circuit (`moverSelectedThisFrame=true` at
  `code/missiongui.cpp:1477/1512/1741/1764`) and never reach the
  spine's `lookupAtPixel` call. Friendly mech inspect would require
  a wholly different gesture and is out of scope.
- **Not attack-replacement.** Shift+LMB on a visible hostile mech
  STILL triggers `doAttack()` via the legacy CPU pick path. M2.6
  fires AFTER the legacy CPU path, emits an inspect log line, and
  does not gate or block the attack. The user-visible distinction
  "Shift gives me both an attack AND an inspect log" is acceptable
  for v1 because the log is debug-only and M2.6 mutates nothing.
- **Not MLR fallback handling.** Per M2.5 empirical data
  (`mlr_mech_draws=0` across all 5 tier1 missions), MLR-rendered
  mechs never get a chance to reach the inspect path. The spine
  handles them transparently as `outcome=miss` (substrate reads `0`
  at the pixel). No conditional code in M2.6.
- **Not a new gesture.** Shift+LMB is shared with static-prop pick
  per user resolution "Gesture mapping". Mover-first short-circuit
  ensures no collision with friendly-mech additive-select.
- **Not async readback.** Same per-click stall budget as M1.6
  (synchronous `glReadPixels` inside `lookupAtPixel`).
- **Not a new env var for substrate gating.** `MC2_OBJECT_ID_BUFFER`
  reused unchanged.
- **Not a `setSceneDrawBuffers` extension.** No new FBO writers; the
  M1.5 helper covers the policy unchanged.
- **Not a shader edit.** Zero edits under `shaders/`. No
  `event=shader_ok` emit needed (and the schema overlap concern
  raised in the input is therefore N/A).

### Open questions (carry to next pass)

See Section 16. With Q1/Q2/Q3 pre-resolved and handle-lookup +
gesture + log-schema + debug-state-shape + MLR-fallback all pre-
decided, the spec carries only **two** open questions for the human
(target: <=2 per input requirement).

---

## 2. Relationship to M2-pre / M2 / M2.5 (substrate readiness)

### Substrate readiness (verified)

| Substrate piece | Location | Status |
|---|---|---|
| `RenderWorld::LookupResult` (no `kind` today) | `RenderWorld/RenderWorld.h:157-167` | M2.6 EXTENDS (+1 field) |
| `RenderObjectRecord.kind` populated for mechs | `RenderWorld/RenderWorld.h:146` | shipped by M2 |
| `lookupAtPixel` copies kind from record | `RenderWorld/RenderWorld.cpp:717-726` | M2.6 EXTENDS (+1 copy line) |
| `tryGameplayPick(req)` spine | `code/gameplay_pick.h:78` | shipped by M2-pre |
| `screenToFboPixel(...)` helper | `code/gameplay_pick.h:90` | shipped by M2-pre |
| M1.6 caller wrapper | `code/missiongui.cpp:6186-6271` | M2.6 PATCHES (kind guard) |
| `mechRenderHandle` on Mech3DAppearance | `mclib/mech3d.h:478` | shipped by M2 |
| Mech adapter spawn/destroy | `GameAdapters/MechRenderAdapter.cpp:87-142` | shipped by M2 |
| `mech.frag` ObjectID emit | `shaders/mech.frag` location=2 | shipped by M2.5 |
| `[MECH_OBJECT_ID_SELFTEST v1] result=PASS` | `RenderWorld/RenderWorld.cpp:368-463` | shipped by M2.5 |
| Fog-of-war predicate (CPU pick) | `code/missiongui.cpp:1273-1278` | reused by M2.6 |
| Mover `conStat` field | `code/mover.h:730,951` | reused by M2.6 |
| `Mover::getContactStatus` | `code/mover.h:1264` | reused by M2.6 |
| `CONTACT_SENSOR_QUALITY_1` constant | `code/missiongui.cpp:1276` (call site) | reused by M2.6 |
| `ObjectManager::getNumMovers` / `getMover` | `code/objmgr.h:450,501` | reused by M2.6 |
| `GameObject::isMech()` predicate | `code/gameobj.h:462-464` | reused by M2.6 |

### Why M2.6 is now safe (post-M2.5)

M1.6 Section 2 documented the M2.6 unblock condition: "until M2
writes mech IDs into attachment-2, GPU pickup MUST be a fallback
after legacy mover picking." M2.5 wrote the mech IDs. The fallback
ordering is STILL correct (mover-first short-circuit preserves
friendly-mech additive-select), but now the spine's `lookupAtPixel`
returns Mech handles instead of garbage when the cursor is on a
mech.

### What changes vs M1.6

- `LookupResult` gains `RenderObjectKind kind`. The 2-line addition
  is the load-bearing surface change.
- M1.6's `tryStaticPropPick` body gains a kind guard so the latent
  mislabel bug stops at compile-aware code.
- Log schema migrates from `[STATIC_PROP_PICK v1]` to
  `[GAMEPLAY_PICK v1] kind=...`. M1.6 emit becomes the
  StaticProp branch of the unified schema.
- `StaticPropSelectionDebugState` -> `GameplaySelectionDebugState`
  with `RenderObjectKind kind` discriminator. Old setters become
  thin shims that forward to the unified setter (or are removed
  outright -- see Section 7).
- New mech-pick consumer in the same caller TU
  (`code/missiongui.cpp`). Helper name strawman:
  `tryMechPick(...)`. Mirrors `tryStaticPropPick` shape.
- New `findMechByHandle(RenderCore::RenderObjectHandle h) ->
  BattleMech*` in `GameAdapters::Mech::`. Linear scan over
  `ObjectManager` movers; O(N <= ~50 per mission). This is the SOLE
  reverse-resolution mechanism (revised post adversarial; the
  pre-revision spec proposed populating `desc.gameObjectId` with
  `(uint32_t)getPartId()` as a stable cookie -- CRITICAL-1 disproved
  the stability claim; `partId` is reassigned during group/commander
  setup at `code/mission.cpp:2987` AFTER syncSpawn has already
  stamped the value. The `syncSpawn` call at `code/mech.cpp:1338`
  stays `(*m3d, 0u)` -- byte-identical to M2 ship).

### Surface map after M2.6

```
Shift+LMB
  -> updateOldStyle / updateAOEStyle body (legacy mover gates fire first)
  -> setSelected(true) on friendly hits: moverSelectedThisFrame=true
  -> tail: tryStaticPropPick(...) -- M1.6 caller
                                  -> tryGameplayPick spine
                                  -> outcome=hit -> kind=StaticProp -> log+state
                                                    kind=Mech -> SKIP (let mech caller fire)
                                  -> outcome=gated/skipped/miss -> M1.6 path
  -> tail: tryMechPick(...) -- M2.6 caller (NEW)
                                  -> tryGameplayPick spine (SAME instance)
                                  -> outcome=hit -> kind=Mech ->
                                       findMechByHandle -> fog check -> log+state
                                                    kind=StaticProp -> SKIP
                                  -> outcome=gated/skipped/miss -> silent
```

Note: both callers invoke `tryGameplayPick` on the SAME click. The
spine is idempotent (synchronous readback; no side effects; cheap
~< 100us per call). Calling it twice per click is acceptable; an
optional optimization (Section 16 Q2) would call it once and
dispatch on kind. Section 7 chooses the simple form for v1.

---

## 3. Architecture overview -- one substrate extension + one consumer

### Why `LookupResult.kind` is the load-bearing change

Today, `LookupResult` exposes 8 fields
(`RenderWorld/RenderWorld.h:157-167`). None of them tell the caller
"what kind of thing is this?" The record carries a `RenderObjectKind
kind` field (`:146`) that is populated correctly for both static
props and mechs, but `lookupAtPixel` does not copy it into the
result struct (`RenderWorld.cpp:717-726`).

This is the proximate cause of the latent mislabel bug: callers
have no compile-time signal that kind dispatch is required. Adding
`LookupResult.kind` forces every consumer to handle the discriminator
or explicitly ignore it; the 2-line addition is the substrate change
that enables the whole META-FIX scope.

### Why mech-pick is a NEW caller (not a kind branch inside `tryStaticPropPick`)

Per M2-pre Section 10 "Extension contract": when a future caller
adds, it MUST build its own `GameplayPickRequest` and dispatch
through the unchanged spine. Forking `tryStaticPropPick` into a
kind-branching body would either (a) bake mech-specific
category-env logic into the static-prop caller (boundary erosion)
or (b) require a new category enum on the request struct (M2-pre
Q3 leaned AGAINST that for sample-size-two; M2.6 should not flip
that decision without justification).

The cleaner shape: a new `tryMechPick(...)` caller that mirrors
`tryStaticPropPick(...)` and owns the mech-specific category gate,
fog-of-war check, reverse handle resolution, and log emission.
Same spine; different consumer.

### Why the reverse lookup is linear-scan in the adapter (Option B alone)

Per user-resolution "Handle->BattleMech reverse lookup", REVISED
post adversarial CRITICAL-1:

The resolver is a linear scan over `ObjectManager::getMover(i)`
matching on
`mech.getAppearance()->getRenderWorldHandle().raw() == h.raw()`.
N <= ~50 mechs/mission; one call per click (~10/sec max).

**Pre-revision Option D dropped.** The earlier spec proposed
populating `desc.gameObjectId` at `MechRenderAdapter::syncSpawn` with
`(uint32_t)BattleMech::getPartId()` so the cookie carried a stable
identity. Adversarial review proved this WRONG: `code/mission.cpp:2987`
reassigns `partId` during group/commander setup AFTER
`mover->init(true, objType)` has invoked `syncSpawn` at
`code/mech.cpp:1338`. The cookie would be stamped with the pre-
group value and silently desync from the live `partId`. The HANDLE
itself is the only lifetime-stable identifier (M2 `destroyMech`
retires a handle; the next mech to register gets a fresh one with
bumped generation).

The linear scan is the resolver (zero new container, leverages
existing ObjectManager state). Stale-handle race (mech destroyed
between readback and resolver) is the nullptr branch in
`findMechByHandle`. The log line emits the handle's index /
generation / raw bits plus the resolved BattleMech pointer (for
debug), NOT a `gameObjectId` field (there is no stable cookie to
emit).

### Why fog-of-war respect is in the M2.6 CALLER, not the spine

Per user-resolution Q1=A: fog respect is mandatory by default. But
the spine (`tryGameplayPick`) is category-agnostic and KIND-blind;
it cannot know to apply mech-specific sensor rules. The fog gate
is a M2.6 caller concern -- AFTER the spine returns `outcome=hit
kind=Mech` AND `findMechByHandle` returns non-null, the caller
checks the BattleMech's `conStat` (or `getContactStatus(...)`) and
either emits the hit log or emits a `gated` log (silently, unless
`MC2_MECH_PICK_PIERCE_FOG=1`).

### Architecture diagram

```
                  +---------------------------------+
                  | Shift+LMB                       |
                  +---------------------------------+
                            |
                            v
                  +---------------------------------+
                  | Mover-first short-circuit       |
                  |  (legacy setSelected; sets      |
                  |   moverSelectedThisFrame=true   |
                  |   at lines 1477/1512/1741/1764) |
                  +---------------------------------+
                            |
                            v
                  +---------------------------------+
                  | tail of updateOldStyle /        |
                  | updateAOEStyle:                 |
                  |  tryStaticPropPick(...)         |
                  |  tryMechPick(...)               |   <-- M2.6
                  +---------------------------------+
                            |
                            v
                  +---------------------------------+
                  | tryGameplayPick(req)            |
                  | (unchanged spine; gate ladder + |
                  | screenToFboPixel + lookupAtPixel)|
                  +---------------------------------+
                            |
                            v
                  +---------------------------------+
                  | LookupResult {                  |
                  |   isValid, handle, kind, ...    |   <-- M2.6 +kind
                  | }                               |
                  +---------------------------------+
                            |
              +-------------+--------------+
              |                            |
              v (kind=StaticProp)          v (kind=Mech)
   +-------------------------+   +--------------------------------+
   | tryStaticPropPick       |   | tryMechPick                    |
   |   guard kind != Mech    |   |   guard kind != StaticProp     |
   |   setLastGameplayPick   |   |   findMechByHandle             |
   |   [GAMEPLAY_PICK v1]    |   |   conStat fog check            |
   |    hit kind=StaticProp  |   |   if visible OR PIERCE_FOG:    |
   |    ...                  |   |     setLastGameplayPick        |
   +-------------------------+   |     [GAMEPLAY_PICK v1]         |
                                 |      hit kind=Mech ...         |
                                 |   else: silent (or gated log)  |
                                 +--------------------------------+
```

### Greybeard ruling (anticipated): META-FIX

Section 13 carries the full greybeard analysis. Short form: M2.6
retires the per-kind log/state-slot pattern that would multiply at
M3 (terrain) and M4 (VFX). The substitutive proof is that
`StaticPropSelectionDebugState` and `[STATIC_PROP_PICK v1]` are
RETIRED (not coexisting with the new unified forms); the post-
slice grep `grep '\[STATIC_PROP_PICK v1\]' code/ RenderWorld/`
should return ZERO matches.

---

## 4. API extensions

### 4.1 `LookupResult.kind` (mandatory 2-line addition)

**Existing (`RenderWorld/RenderWorld.h:157-167`, grep-verified at
write time):**

```cpp
struct LookupResult {
    bool                            isValid          = false;
    RenderCore::RenderObjectHandle  handle           = RenderCore::RenderObjectHandle::invalid();
    uint32_t                        meshHandleBits   = 0;
    uint32_t                        materialHandleBits = 0;
    uint8_t                         lodLevel         = 0xFFu;
    uint16_t                        pipelineId       = 0;
    uint32_t                        drawPacketIndex  = 0xFFFFFFFFu;
    uint32_t                        pathReasonCode   = 0;
    uint32_t                        gameObjectId     = 0;
};
```

**Replace with:**

```cpp
struct LookupResult {
    bool                            isValid          = false;
    RenderCore::RenderObjectHandle  handle           = RenderCore::RenderObjectHandle::invalid();
    uint32_t                        meshHandleBits   = 0;
    uint32_t                        materialHandleBits = 0;
    uint8_t                         lodLevel         = 0xFFu;
    uint16_t                        pipelineId       = 0;
    uint32_t                        drawPacketIndex  = 0xFFFFFFFFu;
    uint32_t                        pathReasonCode   = 0;
    uint32_t                        gameObjectId     = 0;
    // M2.6: kind discriminator copied from RenderObjectRecord.kind.
    // Caller MUST check this before consuming kind-specific fields
    // (recipeIndex for StaticProp; BattleMech reverse-lookup for Mech).
    // Defaults to StaticProp to preserve M1.6 caller behavior on an
    // isValid=false return (callers should gate on isValid first
    // anyway; the default is only relevant for compile-time
    // initializer compatibility).
    RenderObjectKind                kind             = RenderObjectKind::StaticProp;
};
```

**Existing copy site (`RenderWorld/RenderWorld.cpp:717-726`,
grep-verified):**

```cpp
    out.isValid            = true;
    out.handle             = h;
    out.meshHandleBits     = rec.meshHandleBits;
    out.materialHandleBits = rec.materialHandleBits;
    out.lodLevel           = rec.lodLevel;
    out.pipelineId         = rec.pipelineId;
    out.drawPacketIndex    = rec.drawPacketIndex;
    out.pathReasonCode     = rec.pathReasonCode;
    out.gameObjectId       = rec.gameObjectId;
    return out;
```

**Replace with (one line added):**

```cpp
    out.isValid            = true;
    out.handle             = h;
    out.meshHandleBits     = rec.meshHandleBits;
    out.materialHandleBits = rec.materialHandleBits;
    out.lodLevel           = rec.lodLevel;
    out.pipelineId         = rec.pipelineId;
    out.drawPacketIndex    = rec.drawPacketIndex;
    out.pathReasonCode     = rec.pathReasonCode;
    out.gameObjectId       = rec.gameObjectId;
    out.kind               = rec.kind;  // M2.6: kind discriminator
    return out;
```

### 4.2 `GameAdapters::Mech::findMechByHandle`

New public free function in `GameAdapters/MechRenderAdapter.{h,cpp}`.

**Header (add to `GameAdapters/MechRenderAdapter.h` after `destroyMech`
declaration around line 52):**

```cpp
// M2.6: handle->BattleMech reverse lookup. Linear scan over
// ObjectManager mover list; matches on
//   mech.getAppearance()->getRenderWorldHandle().raw() == h.raw().
// Returns nullptr on stale/unknown handle (M2.6 inspect-only path
// treats this as outcome=miss for the click).
//
// O(N) where N = num movers per mission (<= ~50; tier1 max mc2_24
// has 46 mechs). Cost negligible vs the lookupAtPixel readback that
// produced the handle. NOT main-loop-safe to call per frame; intended
// for one call per click (~10/sec max).
//
// MUST be called from the main thread (ObjectManager is not
// thread-safe). The inspect path in tryMechPick satisfies this.
BattleMech* findMechByHandle(RenderCore::RenderObjectHandle h);
```

The header MUST forward-declare `class BattleMech;` (existing M2
adapter header already forward-declares `class Mech3DAppearance` --
same pattern, same firewall carve-out per M2 spec Section 12).
Per MINOR (header decl) from adversarial review: consistency with
the M2 `ForAdapter` accessor pattern requires forward-decl, not
full include, to keep `code/mech.h` out of the adapter header's
public surface.

**Implementation (in `GameAdapters/MechRenderAdapter.cpp`):**

```cpp
// M2.6: handle->BattleMech reverse lookup. Inspect-only path.
BattleMech* findMechByHandle(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return nullptr;
    if (ObjectManager == nullptr) return nullptr;
    const uint32_t target = h.raw();
    const long n = ObjectManager->getNumMovers();
    for (long i = 0; i < n; ++i) {
        MoverPtr m = ObjectManager->getMover(i);
        if (m == nullptr) continue;
        if (!m->isMech()) continue;
        BattleMech* bm = static_cast<BattleMech*>(m);
        Mech3DAppearance* app =
            static_cast<Mech3DAppearance*>(bm->getAppearance());
        if (app == nullptr) continue;
        if (app->getRenderWorldHandle().raw() == target) {
            return bm;
        }
    }
    return nullptr;
}
```

**Firewall note.** `GameAdapters/MechRenderAdapter.cpp` is already
the documented exception that includes both `mclib/mech3d.h` AND
`RenderWorld/RenderWorld.h` (header comment at `.cpp:3-7`). M2.6's
addition reaches `code/objmgr.h`, `code/mech.h`, and
`code/mover.h` (for `ObjectManager`, `BattleMech`, `MoverPtr`,
`isMech()`). The `code/` directory is OUTSIDE
`scripts/check-include-firewall.sh`'s `SCOPE_DIRS` (verified by
M2-pre Section 7 firewall analysis: SCOPE_DIRS lists
`RenderCore RenderWorld Visibility MeshRenderer MaterialSystem
DebugRenderer RenderDeviceGL` only; `GameAdapters/` is NOT scoped
either). So adding the `code/` includes to `MechRenderAdapter.cpp`
is unconstrained by the firewall script.

Reviewer-discipline gate: the includes are engine-adapter -> game-
side classes (the adapter IS the bridge layer; bridging is its
documented purpose). The header still forbids `code/` includes;
only the `.cpp` reaches in. M2-pre's precedent applies.

**Spec-author note on alternative.** A future evolution could
maintain an `unordered_map<uint32_t, BattleMech*>` inside the
adapter, updated on `syncSpawn`/`destroyMech`. M2.6 deliberately
ships the linear scan because (a) the per-click cost is dominated
by the readback that produced the handle, (b) the map adds a
container with its own lifecycle bug surface, and (c) the
recon-time analysis showed N <= 50 mechs in flight at any time on
tier1. Promote to map IF profiling later shows it matters.

### 4.3 `GameplaySelectionDebugState` (META-FIX rename + discriminator)

**Existing (`RenderWorld/RenderWorld.h:185-194`, grep-verified):**

```cpp
struct StaticPropSelectionDebugState {
    bool                            valid              = false;
    RenderCore::RenderObjectHandle  handle             = RenderCore::RenderObjectHandle::invalid();
    int32_t                         recipeIndex        = -1;
    int32_t                         lastPickMouseX     = 0;
    int32_t                         lastPickMouseY     = 0;
    int32_t                         lastPickGlX        = 0;
    int32_t                         lastPickGlY        = 0;
    uint64_t                        lastPickFrameIndex = 0;
};
```

**Replace with (rename + discriminator + mech-specific payload field):**

```cpp
// M2.6: most-recent gameplay pick debug state. Renamed from
// StaticPropSelectionDebugState (META-FIX: retires the per-kind
// state-slot pattern that would multiply at M3/M4). Single
// mutex-guarded slot; latest pick across all kinds wins.
//
// Kind-specific payload:
//   kind == StaticProp -> recipeIndex carries the recipe (M1.6 semantic)
//   kind == Mech       -> handle alone carries the identity. Callers
//                          re-resolve via findMechByHandle to avoid
//                          stale-pointer dereference after destroyMech.
//                          NOTE (post adversarial CRITICAL-1): the
//                          pre-revision spec had a `gameObjectId`
//                          field here populated from a partId cookie;
//                          dropped because partId is reassigned
//                          post-syncSpawn at code/mission.cpp:2987.
//                          The handle is the only stable identity.
//   future kinds       -> add a tagged-union payload field at that
//                          slice; do NOT widen the struct prematurely.
//
// Cleared on per-mission RenderWorld::destroy() (same lifecycle as
// the M1.6 StaticPropSelectionDebugState).
struct GameplaySelectionDebugState {
    bool                            valid              = false;
    RenderObjectKind                kind               = RenderObjectKind::StaticProp;
    RenderCore::RenderObjectHandle  handle             = RenderCore::RenderObjectHandle::invalid();
    int32_t                         recipeIndex        = -1;     // kind==StaticProp only; -1 otherwise
    // (no kind==Mech-specific payload: handle alone identifies the mech;
    //  callers re-resolve via findMechByHandle. See CRITICAL-1 note above.)
    int32_t                         lastPickMouseX     = 0;
    int32_t                         lastPickMouseY     = 0;
    int32_t                         lastPickGlX        = 0;
    int32_t                         lastPickGlY        = 0;
    uint64_t                        lastPickFrameIndex = 0;
};
```

**Design choice (overloaded-setter vs tagged-union):** M2.6 picks
**overloaded setter** (single struct with kind-specific "live"
fields; non-matching fields hold sentinel values). Rationale:

- Two consumers today (StaticProp + Mech); a tagged union for two
  cases over-engineers for the current consumer count.
- Sentinel value (`recipeIndex=-1` for non-StaticProp kinds) matches
  the existing field default; no extra cognitive load for callers
  inspecting the struct. (Post adversarial CRITICAL-1 the kind==Mech
  branch has NO kind-specific payload field; the handle suffices.)
- Tagged union would force every reader to dispatch on `kind`
  even for fields like `lastPickMouseX` that are shared across
  all kinds.
- If M3 / M4 add genuinely incompatible payloads (e.g. terrain
  needs a worldspace AABB and VFX needs a particle ID), promote
  the kind-specific fields into a tagged-union THEN. M2.6 picks
  the smaller delta.

**API surface (replaces M1.6's static-prop-named accessors):**

```cpp
// M2.6: populate from a valid LookupResult. Caller passes the
// pre-checked kind (must match res.kind). Kind-specific payload
// is extracted from the appropriate source:
//   kind==StaticProp: recipeIndex sampled from handleToRecipeIndex
//                     (existing M1.6 path)
//   kind==Mech:       no kind-specific payload; handle alone
//                     identifies the mech (see CRITICAL-1 note).
void setLastGameplayPick(RenderObjectKind kind,
                         const LookupResult& res,
                         int32_t mouseX, int32_t mouseY,
                         int32_t glX,    int32_t glY);

// M2.6: reset to default (valid=false). Idempotent.
void clearLastGameplayPick();

// M2.6: read-only access. Caller MUST check .valid before consuming
// any other field; then dispatch on .kind for payload semantics.
GameplaySelectionDebugState getLastGameplayPick();
```

**META-FIX retirement (load-bearing).** The old symbols
`setLastStaticPropPick`, `clearLastStaticPropPick`,
`getLastStaticPropPick`, and `StaticPropSelectionDebugState` are
REMOVED in this slice. The substitutive proof is
`grep -E 'setLastStaticPropPick|getLastStaticPropPick|clearLastStaticPropPick|StaticPropSelectionDebugState'`
returns ZERO matches across `RenderWorld/`, `code/`, and `GameAdapters/`
after the slice ships. NO shim. If retaining a one-release shim
proves necessary at plan stage (e.g. tooling consumes the M1.6
schema by name), the spec author should be re-consulted -- the
META-FIX claim depends on full retirement.

### 4.4 `RenderWorld::destroy()` lifecycle update

`RenderWorld::destroy()` already calls `clearLastStaticPropPick()`
(per M1.6 spec Section 6 "Lifecycle"). M2.6 changes this to
`clearLastGameplayPick()` -- one symbol rename, same call site.
No behavior change other than the unified semantics.

---

## 5. Code changes -- M1.6 wrapper kind guard (latent-bug fix)

### Existing M1.6 wrapper hit branch (`code/missiongui.cpp:6216-6243`)

```cpp
case GameplayPickResult::Outcome::hit: {
    // Update RenderWorld debug state. Single-slot; latest wins.
    RenderWorld::setLastStaticPropPick(r.lookup,
                                       r.ctx.mouseX, r.ctx.mouseY,
                                       r.ctx.glX,    r.ctx.glY);
    const RenderWorld::StaticPropSelectionDebugState picked =
        RenderWorld::getLastStaticPropPick();
    std::fprintf(stderr,
        "[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "
        "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
        "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
        r.lookup.handle.bits,
        (unsigned)r.lookup.handle.index(),
        (unsigned)r.lookup.handle.generation(),
        (int)picked.recipeIndex,
        r.ctx.mouseX, r.ctx.mouseY,
        r.ctx.glX,    r.ctx.glY,
        r.ctx.fboX,   r.ctx.fboY,
        r.ctx.vMulX,  r.ctx.vMulY,
        r.ctx.vAddX,  r.ctx.vAddY,
        r.ctx.drawableWidth, r.ctx.drawableHeight);
    break;
}
```

### Replace with (kind guard + unified log schema)

```cpp
case GameplayPickResult::Outcome::hit: {
    // M2.6 latent-bug fix: post-M2.5, the substrate may return a
    // Mech handle. The static-prop caller MUST guard before
    // consuming -- a Mech handle has no recipe index, and the M2.6
    // mech caller (tryMechPick) owns mech-kind hits.
    if (r.lookup.kind != RenderWorld::RenderObjectKind::StaticProp) {
        // Not our category. Silent skip; the mech caller (or future
        // terrain/VFX caller) handles its own kinds.
        break;
    }
    RenderWorld::setLastGameplayPick(RenderWorld::RenderObjectKind::StaticProp,
                                     r.lookup,
                                     r.ctx.mouseX, r.ctx.mouseY,
                                     r.ctx.glX,    r.ctx.glY);
    const RenderWorld::GameplaySelectionDebugState picked =
        RenderWorld::getLastGameplayPick();
    // M2.6 unified log schema: [GAMEPLAY_PICK v1] kind=StaticProp ...
    std::fprintf(stderr,
        "[GAMEPLAY_PICK v1] hit kind=StaticProp handle=%u idx=%u gen=%u "
        "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
        "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
        r.lookup.handle.bits,
        (unsigned)r.lookup.handle.index(),
        (unsigned)r.lookup.handle.generation(),
        (int)picked.recipeIndex,
        r.ctx.mouseX, r.ctx.mouseY,
        r.ctx.glX,    r.ctx.glY,
        r.ctx.fboX,   r.ctx.fboY,
        r.ctx.vMulX,  r.ctx.vMulY,
        r.ctx.vAddX,  r.ctx.vAddY,
        r.ctx.drawableWidth, r.ctx.drawableHeight);
    break;
}
```

### Miss-branch rename (mechanical)

Existing `code/missiongui.cpp:6244-6263` block uses
`clearLastStaticPropPick` and `[STATIC_PROP_PICK v1] miss`. Rename
to `clearLastGameplayPick` and `[GAMEPLAY_PICK v1] miss`.

### Boot-banner rename (mechanical)

`RenderWorld/RenderWorld.cpp:484` emits:

```cpp
std::fprintf(stderr, "[STATIC_PROP_PICK v1] enabled=%d debug=%d\n", ...);
```

Rename to:

```cpp
std::fprintf(stderr, "[GAMEPLAY_PICK v1] static_prop_enabled=%d static_prop_debug=%d\n", ...);
```

(Mech pick gate added in the same banner per Section 6.)

### Doc-comment scope (per MAJOR-2(a))

The Gate 6 grep set is extended to cover doc comments. M2.6 must
rename:

- `RenderWorld/RenderWorld.h:97` doc comment (mentions
  `[STATIC_PROP_PICK v1] miss`)
- `RenderWorld/RenderWorld.cpp:93` doc comment (mentions
  `setLastStaticPropPick`)
- `RenderWorld/RenderWorld.cpp:484` boot-banner literal (already
  scheduled above)
- `RenderWorld/RenderWorld.cpp:599` doc comment (mentions
  `setLastStaticPropPick`)
- `RenderWorld/RenderWorld.cpp:605` doc comment (mentions
  `[STATIC_PROP_PICK v1] miss`)

All five hits must rename in the same META-FIX commit so Gate 6
passes.

### CLAUDE.md updates (mandatory in same SHIPPED docs commit)

The M1.6 entry in worktree CLAUDE.md ("Active campaigns" section)
references `[STATIC_PROP_PICK v1] hit handle=...` as the log
format. M2.6's docs commit MUST update the entry to point at
`[GAMEPLAY_PICK v1] hit kind=StaticProp ...` so future log-archaeology
greps land in the right place. Without this update, anyone running
`grep '\[STATIC_PROP_PICK v1\]'` after M2.6 ships will get zero hits
and incorrectly conclude the path was deleted.

**Additional (per adversarial MINOR -- CLAUDE.md env-vars).** The
CLAUDE.md "Tier-1 instrumentation env vars" section must add the
three new M2.6 env vars (`MC2_MECH_PICK`, `MC2_MECH_PICK_DEBUG`,
`MC2_MECH_PICK_PIERCE_FOG`) in the SAME SHIPPED docs commit. Without
this, the env-var section drifts out of sync with the live gating
surface. T-final task includes both updates as a single block.

---

## 6. Code changes -- mech consumer (no adapter populate)

### 6.1 `MechRenderAdapter::syncSpawn` -- NO CHANGE

**Pre-revision (DROPPED).** The pre-revision spec proposed changing
the call site at `code/mech.cpp:1338` from
`GameAdapters::Mech::syncSpawn(*m3d, 0u);` to
`GameAdapters::Mech::syncSpawn(*m3d, (uint32_t)this->getPartId());`
to give the resolved `LookupResult.gameObjectId` a stable
identity cookie.

**Adversarial CRITICAL-1 finding:** `partId` is NOT lifetime-stable.
`code/mission.cpp:2987` reassigns `partId` during group/commander
setup AFTER `mover->init(true, objType)` (which invokes `syncSpawn`
at `code/mech.cpp:1338`) has already stamped the cookie into
`s_objectRecords`. The cookie would silently desync from the live
`partId` for any mech that joins a group or is promoted to
commander.

**Revised: `code/mech.cpp:1338` stays `GameAdapters::Mech::syncSpawn(*m3d, 0u);`
byte-identical to M2 ship.** The adapter's `gameObjectId` parameter
is preserved for the future-extensibility reason it was originally
introduced; M2.6 just does not populate it with a non-stable value.
There is no engine-side stable cookie for the BattleMech available
at `syncSpawn` time that survives the post-init `partId` reassignment;
the HANDLE itself is the only stable identity, and the resolver
(Section 6.3) finds the BattleMech from the handle.

### 6.2 No BattleMech identifier choice needed

**Pre-revision Section 6.2 (DROPPED entirely).** The earlier spec
chose `getPartId()` as the cookie source. With Option D abandoned
(Section 6.1), no identifier choice is needed; `findMechByHandle`
returns the live BattleMech pointer directly.

If a future slice needs a stable identity cookie at register-time
(e.g. for save-game cross-reference), the right path is to plumb
through a value set AFTER the group/commander reassignment phase,
or to use a different stamping point in the BattleMech lifecycle.
That is out of M2.6 scope.

### 6.3 `MissionInterfaceManager::tryMechPick(...)` (new caller)

New method on `MissionInterfaceManager` (declaration in
`code/missiongui.h`; body in `code/missiongui.cpp` adjacent to
`tryStaticPropPick`).

**Declaration (mirrors M1.6 `tryStaticPropPick` signature):**

```cpp
void tryMechPick(bool moverSelectedThisFrame,
                 bool shiftDn,
                 bool leftClicked,
                 bool bGui,
                 bool bLeftDouble,
                 int  mouseX,
                 int  mouseY);
```

**Body (pseudocode; mirrors `tryStaticPropPick` shape, swaps category):**

```cpp
void MissionInterfaceManager::tryMechPick(bool moverSelectedThisFrame,
                                          bool shiftDn,
                                          bool leftClicked,
                                          bool bGui,
                                          bool bLeftDouble,
                                          int  mouseX,
                                          int  mouseY)
{
    // Category gate STAYS in caller. M2.6's mech-pick opt-in.
    if (!RenderWorld::IsMechPickEnabled())
        return;

    GameplayPickRequest req;
    req.mouseX                  = mouseX;
    req.mouseY                  = mouseY;
    req.shiftDn                 = shiftDn;
    req.leftClicked             = leftClicked;
    req.bGui                    = bGui;
    req.bLeftDouble             = bLeftDouble;
    req.moverSelectedThisFrame  = moverSelectedThisFrame;

    const GameplayPickResult r = tryGameplayPick(req);

    switch (r.outcome) {
    case GameplayPickResult::Outcome::hit: {
        // Kind guard: M2.6 caller only handles Mech kind.
        if (r.lookup.kind != RenderWorld::RenderObjectKind::Mech)
            break;

        // Reverse-resolve to BattleMech.
        BattleMech* bm =
            GameAdapters::Mech::findMechByHandle(r.lookup.handle);
        if (bm == nullptr) {
            // Stale handle race (mech destroyed between readback and
            // resolver). Treat as miss for the click; emit a debug-
            // gated miss log if MC2_MECH_PICK_DEBUG=1.
            if (RenderWorld::IsMechPickDebugEnabled()) {
                std::fprintf(stderr,
                    "[GAMEPLAY_PICK v1] miss kind=Mech reason=stale_handle "
                    "handle=%u idx=%u gen=%u screen=(%d,%d) gl=(%d,%d)\n",
                    r.lookup.handle.bits,
                    (unsigned)r.lookup.handle.index(),
                    (unsigned)r.lookup.handle.generation(),
                    r.ctx.mouseX, r.ctx.mouseY,
                    r.ctx.glX,    r.ctx.glY);
            }
            break;
        }

        // Fog-of-war gate (per user resolution Q1=A).
        //
        // Mirrors the FULL CPU-pick predicate at
        // code/missiongui.cpp:1272-1278 (spot-checked at spec-revision
        // time; line numbers may drift -- ground in the symbol set:
        // ShowMovers + MPlayer->allUnitsDestroyed + getTeamId +
        // isDisabled + conStat < CONTACT_SENSOR_QUALITY_1).
        //
        // The CPU pick at :1272 nulls a hostile-mover target only when
        // ALL of these are true:
        //   !ShowMovers
        //   !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID])
        //   target->getTeamId() != Team::home->getId()
        //   !target->isDisabled()
        //   ((Mover*)target)->conStat < CONTACT_SENSOR_QUALITY_1
        // i.e. fog SUPPRESSES the pick ONLY when all five hold.
        //
        // ShowMovers (debug) and the multiplayer-defeat carve-out
        // (MAJOR-1 finding) bypass fog suppression. M2.6 must mirror
        // BOTH or the inspect log will be silent on a mech the CPU
        // pick successfully selected.
        //
        // MC2_MECH_PICK_PIERCE_FOG=1 short-circuits the whole predicate
        // (visible = true unconditionally).
        const bool pierce = RenderWorld::IsMechPickPierceFogEnabled();
        bool visible;
        if (pierce) {
            visible = true;
        } else {
            const bool showMovers = ShowMovers;
            const bool mpDefeat   = (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]);
            const bool hostile    = (bm->getTeamId() != Team::home->getId());
            const bool disabled   = bm->isDisabled();
            const bool sub_q1     = (((Mover*)bm)->conStat < CONTACT_SENSOR_QUALITY_1);
            const bool fogSuppresses =
                !showMovers && !mpDefeat && hostile && !disabled && sub_q1;
            visible = !fogSuppresses;
        }
        if (!visible) {
            // Fog-gated: silent unless PIERCE_FOG=1 toggles the gate
            // (the env-var-on log line below). Per Q1=A: default
            // behavior emits NO log so the substrate cannot become a
            // sensor cheat tool.
            //
            // Note: this is OUTCOME=gated semantically, but the spine
            // already returned outcome=hit. We do NOT emit any log on
            // the default path. If MC2_MECH_PICK_DEBUG=1 is set, emit
            // a gated diagnostic so the user can confirm fog is
            // suppressing the pick (vs e.g. wrong gesture).
            if (RenderWorld::IsMechPickDebugEnabled()) {
                std::fprintf(stderr,
                    "[GAMEPLAY_PICK v1] gated kind=Mech reason=fog_of_war "
                    "handle=%u screen=(%d,%d) gl=(%d,%d)\n",
                    r.lookup.handle.bits,
                    r.ctx.mouseX, r.ctx.mouseY,
                    r.ctx.glX,    r.ctx.glY);
            }
            break;
        }

        // Hit. Update unified debug state + emit unified log line.
        RenderWorld::setLastGameplayPick(RenderWorld::RenderObjectKind::Mech,
                                         r.lookup,
                                         r.ctx.mouseX, r.ctx.mouseY,
                                         r.ctx.glX,    r.ctx.glY);
        // Log line carries handle bits/index/generation + the resolved
        // BattleMech pointer (debug; not a stable cookie). NO
        // gameObjectId / partId fields: CRITICAL-1 disproved partId
        // stability; the handle IS the identity.
        std::fprintf(stderr,
            "[GAMEPLAY_PICK v1] hit kind=Mech handle=%u idx=%u gen=%u "
            "mech=%p screen=(%d,%d) gl=(%d,%d) "
            "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)%s\n",
            r.lookup.handle.bits,
            (unsigned)r.lookup.handle.index(),
            (unsigned)r.lookup.handle.generation(),
            (void*)bm,
            r.ctx.mouseX, r.ctx.mouseY,
            r.ctx.glX,    r.ctx.glY,
            r.ctx.fboX,   r.ctx.fboY,
            r.ctx.vMulX,  r.ctx.vMulY,
            r.ctx.vAddX,  r.ctx.vAddY,
            r.ctx.drawableWidth, r.ctx.drawableHeight,
            pierce ? " pierce_fog=1" : "");
        break;
    }
    case GameplayPickResult::Outcome::miss: {
        // Mech caller is silent on plain miss (background pixel,
        // MLR-rendered mech). Verbose miss is the static-prop
        // caller's territory (high-frequency empty-Shift+click
        // gesture). For mechs, emit only if MC2_MECH_PICK_DEBUG=1.
        if (RenderWorld::IsMechPickDebugEnabled()) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK v1] miss kind=Mech screen=(%d,%d) gl=(%d,%d)\n",
                r.ctx.mouseX, r.ctx.mouseY,
                r.ctx.glX,    r.ctx.glY);
        }
        break;
    }
    case GameplayPickResult::Outcome::gated:
    case GameplayPickResult::Outcome::skipped:
        // No-op; legacy mover path consumed (gated) or gesture
        // filter rejected (skipped).
        break;
    }
}
```

### 6.4 Call-site additions (tails of the two style bodies)

Add adjacent to the existing `tryStaticPropPick` calls at
`code/missiongui.cpp:1539-1545` and `:1782-1788`:

```cpp
    tryStaticPropPick(moverSelectedThisFrame, shiftDn, leftClicked,
                      bGui, bLeftDouble, mouseX, mouseY);
    tryMechPick(moverSelectedThisFrame, shiftDn, leftClicked,
                bGui, bLeftDouble, mouseX, mouseY);  // M2.6
```

**Order note.** Static-prop caller fires first; mech caller fires
second. Both inspect `r.lookup.kind` and skip on non-matching kind.
Since `lookupAtPixel` returns at most one kind per pixel (static-
prop and mech handle ranges are disjoint per M2 design --
static-prop indices < 2641, mech indices >= 65536), at most one
caller's hit branch fires per click. The other caller sees the
same `outcome=hit` but bails on the kind guard, silently.

### 6.5 New env vars

Three new env-var gates (mirrors M1.6's three-gate stack shape):

| Env var | Default | Owner | Purpose |
|---|---|---|---|
| `MC2_MECH_PICK` | OFF | gameplay (input wiring) | Master enable for `tryMechPick` |
| `MC2_MECH_PICK_DEBUG` | OFF | gameplay (log verbosity) | Enables miss / gated / stale-handle diag logs |
| `MC2_MECH_PICK_PIERCE_FOG` | OFF | dev/debug | Allows inspect through sensor fog |

Cached at process start via same pattern as M1.6's accessors:

```cpp
namespace RenderWorld {
bool IsMechPickEnabled();
bool IsMechPickDebugEnabled();
bool IsMechPickPierceFogEnabled();
}
```

Boot banner extension (mechanical):

```cpp
std::fprintf(stderr,
    "[GAMEPLAY_PICK v1] static_prop_enabled=%d static_prop_debug=%d "
    "mech_enabled=%d mech_debug=%d mech_pierce_fog=%d\n",
    IsStaticPropPickEnabled() ? 1 : 0,
    IsStaticPropPickDebugEnabled() ? 1 : 0,
    IsMechPickEnabled() ? 1 : 0,
    IsMechPickDebugEnabled() ? 1 : 0,
    IsMechPickPierceFogEnabled() ? 1 : 0);
```

---

## 7. Caller pattern decisions

### One spine call per click, two callers (chosen for v1)

Both `tryStaticPropPick` and `tryMechPick` independently build a
request and call `tryGameplayPick`. This is two synchronous readbacks
per click. Acceptable cost (< 200us total per click; ~10 clicks/sec
ceiling means < 2ms/sec ambient cost; substrate is gated default-OFF).

**Cost claim caveat (post adversarial MINOR):** the < 200us number
is ESTIMATED, NOT MEASURED. The spec author did not profile two
synchronous `glReadPixels` round-trips back-to-back on the target
hardware. Plan-stage MAY add a Tracy zone around `tryGameplayPick`
to validate, but this is not blocking for v1 (substrate is opt-in
+ click-rate-bounded so even a 10x miss on the estimate is below
noise floor).

Alternative: a single shared helper that calls the spine once and
dispatches on `r.lookup.kind`. Section 16 Q2 carries this as a
plan-stage consideration. M2.6 picks the simpler form to keep the
two callers structurally parallel; the optimization is mechanical
to apply later if it matters.

### Why not collapse both callers into one body

A single body that dispatches on `kind` would mean:

- One env-gate ladder needs to know about both `IsStaticPropPickEnabled`
  and `IsMechPickEnabled` (loss of category isolation).
- Adding M3 / M4 means editing the single body, not adding a sibling
  caller (worse blast radius per slice).
- The static-prop caller's miss-line semantics differ from the
  mech caller's (M1.6 emits unconditional miss when DEBUG=1; M2.6
  mech path is silent on miss because background-pixel Shift+click
  is high-frequency for terrain but uninteresting for mechs).

Two callers + one spine matches the M2-pre extension contract
(Section 10) cleanly: "M2.6 adds a new caller; does NOT change the
spine." Verified spine signature unchanged.

---

## 8. Surfaces (grep-verified file:line at write time)

| Surface | File | Line(s) | Verified |
|---|---|---|---|
| `LookupResult` shape (add `kind` here) | `RenderWorld/RenderWorld.h` | 157-167 | yes |
| `RenderObjectRecord.kind` | `RenderWorld/RenderWorld.h` | 146 | yes |
| `RenderObjectKind` enum | `RenderWorld/RenderWorld.h` | 116-120 | yes |
| `lookupAtPixel` copy site (add kind copy here) | `RenderWorld/RenderWorld.cpp` | 717-726 | yes |
| `setLastStaticPropPick` impl (rename target) | `RenderWorld/RenderWorld.cpp` | 729-754 | yes |
| `clearLastStaticPropPick` impl (rename target) | `RenderWorld/RenderWorld.cpp` | 756-759 | yes |
| `StaticPropSelectionDebugState` (rename target) | `RenderWorld/RenderWorld.h` | 185-194 | yes |
| `setLastStaticPropPick` decl (rename target) | `RenderWorld/RenderWorld.h` | 201-203 | yes |
| `clearLastStaticPropPick` decl | `RenderWorld/RenderWorld.h` | 208 | yes |
| `getLastStaticPropPick` decl | `RenderWorld/RenderWorld.h` | 213 | yes |
| `IsObjectIdBufferEnabled` (reused) | `RenderWorld/RenderWorld.h` | 85 | yes |
| `IsStaticPropPickEnabled` (preserved) | `RenderWorld/RenderWorld.h` | 94 | yes |
| `IsStaticPropPickDebugEnabled` (preserved) | `RenderWorld/RenderWorld.h` | 103 | yes |
| Boot banner emit (rename target) | `RenderWorld/RenderWorld.cpp` | 484 | yes |
| Self-test wire-up (extension point) | `RenderWorld/RenderWorld.cpp` | 488-496 | yes |
| `tryGameplayPick` decl | `code/gameplay_pick.h` | 78 | yes |
| `GameplayPickRequest` shape | `code/gameplay_pick.h` | 23-40 | yes |
| `GameplayPickResult::Outcome` | `code/gameplay_pick.h` | 58-63 | yes |
| `tryStaticPropPick` body (patch target) | `code/missiongui.cpp` | 6186-6271 | yes |
| `tryStaticPropPick` call site (OldStyle) | `code/missiongui.cpp` | 1539 | yes (recon cited 1538) |
| `tryStaticPropPick` call site (AOEStyle) | `code/missiongui.cpp` | 1782 | yes (recon cited 1781) |
| 4-site `moverSelectedThisFrame=true` | `code/missiongui.cpp` | 1477/1512/1741/1764 | yes |
| 4-site `setSelected(true)` writers | `code/missiongui.cpp` | 1472/1509/1736/1761 | yes |
| `findObjectByMouse` (CPU pick) | `code/missiongui.cpp` | 1267 | yes |
| Fog-of-war predicate (CPU pick) | `code/missiongui.cpp` | 1273-1278 | yes |
| `MoverPtr::conStat` field | `code/mover.h` | 730, 951 | yes |
| `Mover::getContactStatus` decl | `code/mover.h` | 1264 | yes |
| `ObjectManager::getNumMovers` | `code/objmgr.h` | 450 | yes |
| `ObjectManager::getMover` | `code/objmgr.h` | 501 | yes |
| `GameObject::isMech` | `code/gameobj.h` | 462-464 | yes |
| `GameObject::getAppearance` | `code/gameobj.h` | ~394 | yes |
| `Mech3DAppearance::getRenderWorldHandle` | `mclib/mech3d.h` | 487-489 | yes |
| `Mech3DAppearance::mechRenderHandle` storage | `mclib/mech3d.h` | 478 | yes |
| `MechRenderAdapter::syncSpawn` (NO CHANGE post adversarial CRITICAL-1) | `GameAdapters/MechRenderAdapter.cpp` | 87-120 | yes |
| `MechRenderAdapter.h` (add findMechByHandle decl, fwd-decl BattleMech) | `GameAdapters/MechRenderAdapter.h` | EOF (after line 52) | yes |
| `syncSpawn` call site (UNCHANGED; stays `(*m3d, 0u)`) | `code/mech.cpp` | 1338 | yes |
| Fog predicate gate (full mirror) | `code/missiongui.cpp` | 1272-1278 | yes (MAJOR-1) |
| `ShowMovers` global | `code/missiongui.cpp` (extern) | -- | yes |
| `MPlayer->allUnitsDestroyed` array | `code/multplyr.h` (extern) | -- | yes |
| `destroyMech` call sites | `code/mech.cpp` | 1318, 3777 | yes |
| `class BattleMech : public Mover` | `code/mech.h` | 340 | yes |
| `CLAUDE.md` "Active campaigns" M1.6 entry (update target) | `.claude/worktrees/nifty-mendeleev/CLAUDE.md` | M1.6 entry | yes (per current CLAUDE.md read) |

Note on adapter line citations: the recon-time `:97` (`desc.gameObjectId = 0u`)
no longer applies post-M2 (the current code at `MechRenderAdapter.cpp:97`
is `desc.gameObjectId = gameObjectId;` -- adapter receives the value
from the caller). Post adversarial CRITICAL-1, M2.6 makes NO change
at `code/mech.cpp:1338`: the call stays `syncSpawn(*m3d, 0u)` because
no engine-side stable cookie exists at syncSpawn time
(`partId` is reassigned after `mover->init` at `code/mission.cpp:2987`).
The pre-revision spec proposed `(uint32_t)this->getPartId()` here;
that change is DROPPED.

---

## 9. Gating

### Substrate gate (reused)

`MC2_OBJECT_ID_BUFFER` (M1.5 cached bool, `RenderWorld.h:85`) is the
mandatory upstream substrate gate. The spine (`tryGameplayPick`)
already short-circuits when it is OFF.

### Category gates (NEW for M2.6)

- `MC2_MECH_PICK` (default OFF) -- master enable for the mech-pick
  wiring. `tryMechPick` short-circuits at the top when this is OFF.
- `MC2_MECH_PICK_DEBUG` (default OFF) -- enables miss / gated /
  stale-handle diag logs for the mech caller.
- `MC2_MECH_PICK_PIERCE_FOG` (default OFF) -- dev/debug override
  that allows inspect through sensor fog. Default OFF preserves
  stock gameplay (cannot incidentally reveal undetected enemy
  mechs via inspect log).

### Five-gate opt-in stack

| `MC2_OBJECT_ID_BUFFER` | `MC2_MECH_PICK` | `MC2_MECH_PICK_DEBUG` | `MC2_MECH_PICK_PIERCE_FOG` | Behavior |
|---|---|---|---|---|
| 0 | * | * | * | Substrate dormant. tryMechPick returns at category gate. |
| 1 | 0 | * | * | Substrate active; mech wiring dormant. |
| 1 | 1 | 0 | 0 | Mech pick active; fog-respect; default-silent miss. |
| 1 | 1 | 1 | 0 | + verbose miss / gated / stale-handle logs. |
| 1 | 1 | 0 | 1 | Mech pick active; fog-pierce; default-silent miss. |
| 1 | 1 | 1 | 1 | + verbose miss / gated / stale-handle logs (PIERCE marker on hit). |

Static-prop gates (`MC2_STATIC_PROP_PICK`,
`MC2_STATIC_PROP_PICK_DEBUG`) preserved verbatim; M2.6 does not
change them (only the log SCHEMA they emit changes from
`[STATIC_PROP_PICK v1]` to `[GAMEPLAY_PICK v1] kind=StaticProp`).

### Env-var naming asymmetry (intentional; deferred rename)

Per adversarial MAJOR-2(b): post-M2.6 the log schema and debug state
are unified under `[GAMEPLAY_PICK v1]` / `GameplaySelectionDebugState`,
but the env vars retain the `MC2_STATIC_PROP_PICK*` and
`MC2_MECH_PICK*` per-category names. This asymmetry is INTENTIONAL
for v1:

- Back-compat with M1.6 user muscle memory (the user has already
  driven smoke gates with `MC2_STATIC_PROP_PICK=1`; renaming
  silently would break their wiring).
- Functional gating is per-category by design (the user must be
  able to enable static-prop pick independently of mech pick), so
  a single unified env var would be a regression.

Deferred rename owner: **next CLAUDE.md known-env-var section
refactor slice.** When that slice consolidates the `MC2_*_PICK*`
family (e.g. into a `MC2_GAMEPLAY_PICK_CATEGORIES=staticprop,mech`
bitmask or similar), the M2.6 names are migrated then. No M2.6
change.

### Lifecycle

All gates cached at process start; restart required to flip
(same discipline as M1.5/M1.6/M2.5).

---

## 10. Validation strategy

### Gate 1: Tier1 5/5 env-OFF (mandatory; zero pixel delta)

```
(all MC2_OBJECT_ID_BUFFER / MC2_MECH_PICK / etc unset)
```

Expected: bit-for-bit identical pixels + behavior vs M2.5 HEAD. The
added `LookupResult.kind` field plus the new `tryMechPick` method
are dormant on the default path (no env-OFF code path reads them
or fires the new caller's body beyond its category-gate return).

Standard invocation:

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Pass criterion: tier1 5/5 PASS; zero `[GAMEPLAY_PICK v1]` lines in
logs.

### Gate 2: Tier1 5/5 env-ON substrate

```
MC2_OBJECT_ID_BUFFER=1
```

Expected: substrate active, no visual delta beyond M2.5 baseline.
Mech wiring still dormant (`MC2_MECH_PICK=0` default).

Pass criterion: tier1 5/5 PASS; `[MECH_OBJECT_ID_SELFTEST v1]
result=PASS` (inherited from M2.5); no `[GAMEPLAY_PICK v1] hit
kind=Mech` lines (the gate is off).

### Gate 3: Tier1 5/5 env-ON mech wiring

```
MC2_OBJECT_ID_BUFFER=1 MC2_MECH_PICK=1
```

Expected: mech wiring live; no manual clicks during tier1 so no
hits/misses expected from the smoke harness. Self-tests run; banner
emits the new schema; M1.6 static-prop path coexists cleanly.

Pass criterion: tier1 5/5 PASS; `[GAMEPLAY_PICK v1] enabled` banner
present; new self-test (Gate 4) emits PASS.

### Gate 4: `MC2_MECH_PICK_SELFTEST` automated self-test (mandatory)

New `RunMechPickSelfTest()` function in
`code/gameplay_pick.cpp` (or as a sibling next to
`RunGameplayPickSelfTest` per M2-pre convention; helper TU may
also live in `RenderWorld.cpp` next to `RunMechObjectIdSelfTest`
if test discipline prefers per-domain co-location -- plan-stage
decision).

**Procedure:**

1. Gated on `MC2_MECH_PICK_SELFTEST=1` AND
   `IsObjectIdBufferEnabled()` (substrate must be on; otherwise
   vacuous).
2. Register a synthetic mech via
   `RenderWorld::registerMech({0u, 0xC0FFEEu, 0u})`. Capture handle.
3. Build a synthetic `GameplayPickRequest` with all gates clean
   (shiftDn=true, leftClicked=true, bGui=false, bLeftDouble=false,
   moverSelectedThisFrame=false, mouseX/Y at a known viewport-
   center pixel).
4. Call `tryGameplayPick(req)` directly. Assert one of two
   outcomes:
   - `outcome=hit` AND `r.lookup.handle == h` AND
     `r.lookup.kind == Mech` -- the substrate happened to land on
     the synthetic mech pixel. Tight pass.
   - `outcome=hit kind != Mech` OR `outcome=miss` -- the spine
     reached `lookupAtPixel` but the substrate read a different
     pixel (no mech rendered at the synthetic pixel during the
     test frame). Still a pass for the SPINE+LOOKUP path; gates
     5/6 below cover the round-trip independently.
5. Independently exercise the kind round-trip without relying on
   pixel content: directly call
   `lookupAtPixel(known-pixel-of-synthetic-write)` via
   instrumented produce-then-read pattern. Alternative: skip
   step 5 if the M2.5 `RunMechObjectIdSelfTest` already covers
   this end-to-end (it does -- see `RunMechObjectIdSelfTest`
   step 3 round-trip at `RenderWorld.cpp:432-440`).
6. Exercise `findMechByHandle` SCAN INVARIANTS (per MINOR: the
   test validates the SCAN ITSELF, not the data state -- this self-
   test runs at `RenderWorld::init()` time, before any real
   BattleMech instances exist, so an "expect non-null" check would
   be vacuous):
   - With a non-registered handle (`Handle::invalid()`): expect
     `nullptr` (PASS: scan correctly rejects invalid input).
   - With the synthetic handle from step 2: PASS if scan returns
     nullptr (no BattleMech in ObjectManager matches the synthetic
     handle, which is correct -- the synthetic mech is a RenderWorld-
     only record); PASS if scan returns a non-null mech with a
     matching `getRenderWorldHandle() == h` (which would only happen
     if a real mech happens to be registered with the same handle
     bits, i.e. never in practice at init-time). Both outcomes
     prove the scan implementation is well-formed; the test does
     NOT depend on init-time mech population.
7. Cleanup: `RenderWorld::destroyMech(h)`.
8. Emit `[MECH_PICK_SELFTEST v1] result=PASS step=all` or
   `result=FAIL step=N reason=...`.

**Wire-up:** in `RenderWorld::init()` after `RunMechObjectIdSelfTest()`
at `RenderWorld.cpp:496` (mirrors the M2.5 pattern of one self-test
per substrate slice).

Pass criterion: `result=PASS step=all` on first launch under
`MC2_MECH_PICK_SELFTEST=1 MC2_OBJECT_ID_BUFFER=1` on any tier1
mission. mc2_24 preferred (highest mech count).

### Gate 5: Tier1 5/5 env-ON mech + pierce-fog

```
MC2_OBJECT_ID_BUFFER=1 MC2_MECH_PICK=1 MC2_MECH_PICK_PIERCE_FOG=1
```

Pass criterion: tier1 5/5 PASS; no fog regressions in legacy CPU
pick path (M2.6 does not touch it); banner shows `mech_pierce_fog=1`.

### Gate 6: Substitutive proof (META-FIX -- load-bearing)

Run these greps AGAINST the post-slice HEAD; ALL must return zero
matches. The grep set is EXTENDED per adversarial MAJOR-2(a) to
cover doc comments AND in-repo consumer dirs (the pre-revision spec
covered only source dirs):

```
# Source code (symbols + schema literal):
grep -rnE 'setLastStaticPropPick|clearLastStaticPropPick|getLastStaticPropPick' \
    RenderWorld/ code/ GameAdapters/
grep -rnE 'StaticPropSelectionDebugState' RenderWorld/ code/ GameAdapters/
grep -rn '\[STATIC_PROP_PICK v1\]' RenderWorld/ code/ GameAdapters/

# Header doc comments (RenderWorld.h carries M1.6-era references at
# line ~97 and RenderWorld.cpp at lines 93/484/599/605; all must be
# renamed in the META-FIX commit):
grep -rnE '\[STATIC_PROP_PICK v1\]|StaticPropSelectionDebugState|setLastStaticPropPick' \
    --include='*.h' --include='*.cpp' .

# In-repo consumers outside source dirs (tests, scripts, .claude
# tooling, planning docs that reference live symbols):
grep -rnE 'setLastStaticPropPick|getLastStaticPropPick|clearLastStaticPropPick|StaticPropSelectionDebugState|\[STATIC_PROP_PICK v1\]' \
    tests/ scripts/ .claude/
```

Pass criterion: ALL grep families return ZERO matches in code
files / doc comments. The `.claude/` subtree may surface SPEC /
PLAN / RECON / REVIEW docs that name the retired symbols
(legitimate: those are dated artifacts describing the retired
state). Plan-stage MAY exclude `.claude/worktrees/*/docs/superpowers/`
from the consumer-survey grep if it adds noise; the load-bearing
zero-match requirement is on CODE + DOC COMMENTS + TEST/SCRIPT
TOOLING.

This is the substitutive proof that the META-FIX retired the bug
class (per worktree CLAUDE.md "Meta-fix discipline" +
`memory/feedback_offload_must_be_substitutive_not_additive.md`).

If any of the above greps returns matches in code/header/doc-comment/
test/script scope, the META-FIX claim fails -- the old symbol/
schema/state is coexisting with the new, which is the additive-
anti-pattern.

**Spec-stage in-repo consumer survey result (per MAJOR-3):**
Pre-revision grep `tests/`, `scripts/`, `.claude/` for the M1.6
symbols + schema literal returned ZERO hits (excluding planning
docs that describe the retired state -- those are SPEC / PLAN /
REVIEW / RECON / HANDOFF artifacts which are dated and not "live"
consumers). Q1 (Section 16) is RESOLVED as full retirement; NO
SHIM.

### Gate 7: New-schema appearance check

Verify the new schema is present:

```
grep -rn '\[GAMEPLAY_PICK v1\]' RenderWorld/ code/ GameAdapters/
```

Pass criterion: matches found at the new emit sites (banner,
static-prop hit/miss, mech hit/miss/gated/stale-handle).

### Gate 8: User-driven canary (DEFERRED to user)

Spec records expected gestures; the user executes the canary post-
ship:

| Gesture | Expected result |
|---|---|
| Shift+click on friendly mech | Legacy mover-toggle fires; mover-first short-circuit means `moverSelectedThisFrame=true`; both callers see `Outcome::gated`; NO `[GAMEPLAY_PICK v1]` log |
| Shift+click on hostile mech, visible (sensor) | Legacy `doAttack()` fires AND new `[GAMEPLAY_PICK v1] hit kind=Mech ...` log line emits |
| Shift+click on hostile mech, fog-of-war (no sensor), default | Legacy CPU pick returns NULL target; M2.6 spine returns hit kind=Mech; fog-gate suppresses the log (no `[GAMEPLAY_PICK v1]` line) |
| Shift+click on hostile mech, fog-of-war, `MC2_MECH_PICK_PIERCE_FOG=1` | Spine returns hit kind=Mech; log emits with `pierce_fog=1` marker |
| Shift+click on static prop (still works post-rename) | `[GAMEPLAY_PICK v1] hit kind=StaticProp ...` log (renamed from M1.6's `[STATIC_PROP_PICK v1]`) |
| Shift+click on terrain/sky | `outcome=miss` from spine; static-prop caller emits `[GAMEPLAY_PICK v1] miss ...` if DEBUG=1; mech caller silent (no kind discriminator on a miss) |

Pass criterion: user observes the table above on `mc2_03` (highest
prop-count tier1 mission) and ideally `mc2_24` (highest mech-count
tier1 mission) with all relevant env vars set.

### Gate 9: log byte-shape grep

Plan-stage executable verification:

```
grep -hE '\[GAMEPLAY_PICK v1\] hit kind=(StaticProp|Mech)' <smoke-log>
```

Pass criterion: each hit line carries the field set documented in
Section 11 with the same field order.

---

## 11. Log schema (META-FIX form -- replaces `[STATIC_PROP_PICK v1]`)

### Boot banner

```
[GAMEPLAY_PICK v1] static_prop_enabled=<0|1> static_prop_debug=<0|1>
  mech_enabled=<0|1> mech_debug=<0|1> mech_pierce_fog=<0|1>
```

### Hit (StaticProp)

```
[GAMEPLAY_PICK v1] hit kind=StaticProp handle=<u32> idx=<u32> gen=<u32>
  recipe=<i32> screen=(<x>,<y>) gl=(<x>,<y>) fbo=(<x>,<y>)
  vMul=(<f>,<f>) vAdd=(<f>,<f>) draw=(<w>,<h>)
```

Field order + names preserved from M1.6 except for the prepended
`kind=StaticProp` and the subsystem rename.

### Hit (Mech)

```
[GAMEPLAY_PICK v1] hit kind=Mech handle=<u32> idx=<u32> gen=<u32>
  mech=<ptr> screen=(<x>,<y>) gl=(<x>,<y>)
  fbo=(<x>,<y>) vMul=(<f>,<f>) vAdd=(<f>,<f>) draw=(<w>,<h>)
  [pierce_fog=1]
```

`mech=<ptr>` is the resolved BattleMech pointer (debug-only; not
a stable identifier across runs). Post adversarial CRITICAL-1 the
log line carries NEITHER `gameObjectId` NOR `partId`: `partId` was
disproven as lifetime-stable, and `gameObjectId` is no longer
populated at `syncSpawn` (stays `0u`). The handle index/generation/
bits ARE the stable identity for any log-driven correlation.

`pierce_fog=1` appended ONLY when the pierce-fog env override is
on (suffix marker; not always present). Allows log analysis to
distinguish stock-rule hits from override hits.

### Miss

```
[GAMEPLAY_PICK v1] miss [kind=Mech] screen=(<x>,<y>) gl=(<x>,<y>)
  [fbo=(<x>,<y>) vMul=(<f>,<f>) vAdd=(<f>,<f>) draw=(<w>,<h>)]
```

- Static-prop caller emits the verbose form (with fbo/vMul/etc) ONLY
  when `MC2_STATIC_PROP_PICK_DEBUG=1` -- backward-compatible with
  M1.6 minus the schema prefix.
- Mech caller emits short form (no fbo block) only when
  `MC2_MECH_PICK_DEBUG=1`. Plain `miss` from the mech caller
  appears with `kind=Mech` to distinguish from a static-prop miss
  in mixed logs.

### Gated (fog-of-war suppression, debug-only)

```
[GAMEPLAY_PICK v1] gated kind=Mech reason=fog_of_war handle=<u32>
  screen=(<x>,<y>) gl=(<x>,<y>)
```

Default OFF (silent). Emitted when `MC2_MECH_PICK_DEBUG=1` AND the
fog-gate fired. Lets a tester confirm a missing log line is fog-
suppression vs e.g. wrong gesture.

### Stale-handle (debug-only)

```
[GAMEPLAY_PICK v1] miss kind=Mech reason=stale_handle handle=<u32>
  idx=<u32> gen=<u32> screen=(<x>,<y>) gl=(<x>,<y>)
```

`reason=stale_handle` distinguishes the race condition (mech
destroyed between readback and resolver) from the plain background-
pixel miss.

### Schema version note

Subsystem rename is `v1` -> `v1` (no version bump). Rationale: M1.6
shipped `[STATIC_PROP_PICK v1]`; M2.6 retires it and ships
`[GAMEPLAY_PICK v1]`. These are DIFFERENT subsystems by name; each
gets its own `v1` schema. Any future M2.6.X extension that adds
fields bumps to `v2`.

### Grep test for plans

```
grep -E '^\[GAMEPLAY_PICK v1\] (hit|miss|gated) ' <smoke-log>
```

---

## 12. Firewall (no SCOPE_DIRS changes expected; verify)

### Verified at write time

`scripts/check-include-firewall.sh:22` `SCOPE_DIRS` list:

```
RenderCore RenderWorld Visibility MeshRenderer MaterialSystem
DebugRenderer RenderDeviceGL
```

`GameAdapters/` is NOT scoped. `code/` is NOT scoped.

### M2.6 include changes

| File | New include(s) | Direction | Policed? |
|---|---|---|---|
| `GameAdapters/MechRenderAdapter.cpp` | `code/objmgr.h`, `code/mover.h`, `code/mech.h` | bridge -> game | NO (GameAdapters not scoped) |
| `code/missiongui.cpp` | `GameAdapters/MechRenderAdapter.h` (or fwd-decl `findMechByHandle`) | game -> bridge | NO (code not scoped) |

### Firewall script edits

NONE. The script's allowlist
(`scripts/check-include-firewall.allowlist`) already exempts
`GameAdapters/MechRenderAdapter.cpp` from the
`Mech3DAppearance` forbidden-symbol check (M2 added this). The new
M2.6 includes do not introduce additional forbidden symbols in
SCOPE_DIRS files.

### Reviewer-discipline gate

The adapter `.cpp` is the documented bridging TU; reaching into
`code/` from there is its purpose. The header still forbids `code/`
includes (the new `findMechByHandle` decl can take a
forward-declared `class BattleMech` instead of requiring a
full include; matches the existing forward-decl of
`Mech3DAppearance`).

---

## 13. Greybeard analysis (META-FIX ruling)

### Lean: META-FIX

The ruling rests on three pillars:

1. **`LookupResult.kind` is the substrate change that retires a
   bug class.** Adding the field makes every consumer compile-aware
   of kind dispatch. Without this, post-M2.5 mech writes silently
   poison the M1.6 static-prop wrapper. WITH this, the wrapper
   either guards on kind (M2.6 explicit) or fails to compile
   (future extensions). The bug class "substrate consumer assumes
   single-kind invariant that the substrate has since broken" is
   retired.

2. **Schema/state retirement is substitutive, not additive.** The
   M1.6 `[STATIC_PROP_PICK v1]` schema and
   `StaticPropSelectionDebugState` struct are REMOVED, not
   complemented with parallel `[MECH_PICK v1]` and
   `MechSelectionDebugState`. Substitutive proof: post-slice grep
   for the old names returns zero matches (Gate 6). This avoids
   the documented additive-anti-pattern from
   `memory/feedback_offload_must_be_substitutive_not_additive.md`.

3. **Closes the RenderWorld arc's user-visible endpoint.** M1 -> M1.5
   -> M1.6 -> M2 -> M2.5 -> **M2.6** is the planned arc. M2.6 is
   the slice where the substrate becomes inspectable for the
   dominant gameplay-selectable class (mechs). Subsequent slices
   (M2.7 selection, M3 terrain, M4 VFX, M5 overlay) extend this
   foundation but do not change the META-FIX surface; M2.6's
   schema/state shape is the contract they conform to.

### Anticipated greybeard counter

A fair greybeard counter: "you have ONE second consumer (mech)
joining ONE first consumer (static-prop). That is sample-size-of-
two. The M1.5 `setSceneDrawBuffers` META-FIX had FIVE simultaneous
call sites consolidated. M2.6 is additive at sample-size-two."

Response: M2.6's META-FIX is not the kind-dispatch surface (which
IS sample-size-two), but the RETIREMENT of the per-kind schema/
state pattern AHEAD of M3/M4. Deferring retirement to M3 means:
(a) M3 ships `[TERRAIN_PICK v1]` and `TerrainSelectionDebugState`
adding a third parallel set; (b) M4 adds a fourth; (c) the
unification slice then has FOUR parallel schemas to retire instead
of ONE -- and the retirement breaks four user-driven canary
contracts instead of one. The retire-now decision is the smaller
debt path.

Additionally, the latent-bug fix (M1.6 wrapper mislabel) is
mandatory and cannot be deferred -- M2.5 already broke the
invariant. Once that fix lands, kind-dispatch is in the code; the
schema/state unification is the consistent surface.

### Substitutive-not-additive proof (load-bearing)

Per worktree CLAUDE.md "Meta-fix discipline" and the cited memory
file. Gate 6 (Section 10) IS the substitutive proof: three greps
for the retired symbols/schemas; ALL must return zero matches.
If retention is required (e.g. for tooling parsing the M1.6
schema), the META-FIX claim downgrades to PATCH and the spec
author must be re-consulted before plan execution.

### Alternative (PATCH-only) -- REJECTED

PATCH would: keep `[STATIC_PROP_PICK v1]` schema; add parallel
`[MECH_PICK v1]`; leave M1.6 wrapper untouched (preserving the
latent mislabel bug); ship M2.6 as a pure additive caller.

Rejected because:
- Latent bug fix is mandatory; cannot defer.
- Once `LookupResult.kind` exists, schema/state unification is
  the consistent surface; not unifying creates a permanent
  inconsistency between the type system (kind-aware) and the
  log/state slot (per-kind).
- Future M3/M4 retirement cost grows linearly with the number of
  parallel schemas retained.

### Sub-judgment on `findMechByHandle` placement

The reverse lookup belongs in `GameAdapters::Mech` because:

- It crosses the engine -> game boundary (RenderObjectHandle on
  one side, BattleMech on the other). The adapter is the
  documented bridging layer (M2 spec Section 10).
- It requires reach into `code/objmgr.h` and `code/mech.h`;
  putting it in `code/missiongui.cpp` (the caller) would still
  work but mixes adapter-style traversal logic with input
  dispatch.
- A future M2.7 select-by-handle slice will also need it; sharing
  through the adapter is the natural home.

This sub-extraction is META-FIX-ish (centralizes the resolver) but
at sample-size-of-one until M2.7 lands. Greybeard pass should rule
on whether the placement is justified at the current consumer
count; default lean is YES (the adapter pattern is established).

---

## 14. Threat model -- known traps

### T1. Stale handle race

**Trap.** Mech destroyed between `lookupAtPixel` returning the
handle and `findMechByHandle` running on the main thread. Race is
impossible by construction (single-threaded gameplay-pick path per
`gameplay_pick.h:77`; both operations on main thread back-to-back
within the click handler), but defensive design assumes it can
happen.

**Defense.** `findMechByHandle` returns `nullptr` on no-match. The
M2.6 caller branches on null -> emits debug-gated stale-handle log
+ silent no-op on default path. No crash, no garbage log.

### T2. Latent post-M2.5 wrapper poisoning (already-armed bug)

**Trap.** M2.5 shipped 2026-05-23 and started writing mech handles
to the substrate. M1.6's `tryStaticPropPick` wrapper has no kind
guard. A Shift+click on a mech pixel TODAY returns
`outcome=hit kind=Mech`, the wrapper stores it as a static-prop
pick, and emits `recipe=-1` in the log line. The
`StaticPropSelectionDebugState.recipeIndex` is set to -1 because
`handleToRecipeIndex` cannot resolve a mech handle.

**Defense.** Section 5 kind guard. Mandatory; not deferrable to
M2.7.

### T3. MLR fallback ambiguity (per-pixel graceful no-op)

**Trap.** A mech rendered through the MLR/`ShapeRenderer` legacy
path does NOT write to attachment-2 (M2.5 spec Section 6). The
substrate reads `0` at the pixel, `lookupAtPixel` returns
`isValid=false`, the spine returns `outcome=miss`. The user
Shift+clicks on a visible mech, sees no inspect log, and concludes
M2.6 is broken.

**Defense.** Per M2.5 empirical data
(`memory/HANDOFF_*` references + CLAUDE.md "Known issues":
"MLR-rendered mechs do not write object IDs"; `mlr_mech_draws=0`
across all 5 tier1 missions), the gap is rare-in-practice. M2.6
emits no special MLR handling. NO conditional code. If a future
tier1 mission shows `mlr_mech_draws>0`, M2.6 must NOT be re-spec'd
with a per-mission gate; the failure mode is benign (silent miss)
and the M2.5 counter `mlr_mech_draws=M` per
`[MECHBATCHER v1] event=mlr_mech_summary` makes the gap measurable.

**Per-instance / per-click granularity (per adversarial MAJOR-4).**
If a future change introduces a path where `mlr_mech_draws > 0` for
a mission, the substrate `lookupAtPixel` for THAT mech's pixels
returns `Handle::invalid()` (raw=0). The spine outcome is `miss`
for those specific clicks -- no per-mission gate needed. The graceful
no-op is per-pixel/per-click, not per-mission, by construction. NO
code change required even when MLR re-engages; the system fails
closed on a per-click basis. A mixed mission (some GPU-batched
mechs + some MLR-fallback mechs) works correctly: clicks on
batched-mech pixels resolve normally, clicks on MLR-mech pixels
silently miss. The user-visible defect rate equals the MLR-rendered
fraction of mech pixels in the cursor area, not a binary
mission-wide failure.

### T4. Coordinate-space confusion

**Trap.** UI mouseX/Y is in viewport-relative space (top-left
origin); `lookupAtPixel` expects GL pixel (bottom-left origin).
Anyone re-implementing the coord transform in M2.6 risks a Y-flip
bug.

**Defense.** M2.6 caller MUST NOT re-implement. The spine
(`tryGameplayPick`) already calls `screenToFboPixel`; the M2.6
caller's request struct uses the same fields as M1.6. No coord
math in `tryMechPick`.

### T5. `event=shader_ok` schema overlap

Not applicable: M2.6 has zero shader edits. Schema overlap concern
raised in the spec inputs is N/A.

### T6. Code-archaeology hazard on log-schema rename

**Trap.** Anyone grepping `[STATIC_PROP_PICK v1]` after M2.6 ships
gets zero matches and concludes the path was deleted. They miss
the rename to `[GAMEPLAY_PICK v1] kind=StaticProp`.

**Defense.** CLAUDE.md M1.6 entry update IN THE SAME SHIPPED docs
commit (per Section 5 + per user-resolution Q3 "CLAUDE.md M1.6
entry will be UPDATED in the SHIPPED docs commit"). This is the
T-final task in the plan, not a follow-up.

### T7. Adapter include explosion

**Trap.** `GameAdapters/MechRenderAdapter.cpp` already includes
`mech3d.h` AND `RenderWorld.h`. M2.6 adds `objmgr.h`, `mover.h`,
`mech.h`. The TU's include graph grows; future maintainers may
assume the adapter is the kitchen sink.

**Defense.** Adapter header stays strictly bridge-shaped
(forward-declare `class BattleMech`; do not include `mech.h` from
the header). The `.cpp` is the documented exception; comment at
top of the file already explains. M2.6 extends the comment to note
the M2.6 additions.

### T8. Spine signature change pressure

**Trap.** M2.6 finds it "needs" to add a category enum to
`GameplayPickRequest` to short-circuit the lookup for mech-only
pixels. Doing so violates M2-pre's spine-stability contract
(Section 10).

**Defense.** M2.6 explicitly does NOT change the spine. Both
callers build identical requests. Kind dispatch happens AFTER the
spine returns. If a future slice genuinely needs spine
modification, that is the signal for an M2.6-pre extraction slice
(per M2-pre Section 12 Q3 "what triggers an M2-pre redesign").

### T9. CLAUDE.md M1.6 entry + env-var section drift

**Trap.** The CLAUDE.md M1.6 entry references specific symbol
names + line numbers (`[STATIC_PROP_PICK v1] hit handle=...`); the
"Tier-1 instrumentation env vars" section enumerates active env
vars. The M2.6 docs commit must update BOTH; if it does not, the
entries ship stale.

**Defense.** Plan T-final task: "Update CLAUDE.md M1.6 entry to
reflect renamed schema AND add `MC2_MECH_PICK*` to the env-var
section." Mandatory; blocking on docs commit. (Per adversarial
MINOR -- CLAUDE.md env-vars.)

---

## 15. Resolved decisions (summary)

| Decision | Resolution | Source |
|---|---|---|
| Q1 Fog-of-war | A: Respect by default; pierce via `MC2_MECH_PICK_PIERCE_FOG=1` | user resolutions Q1 |
| Q2 Inspect vs select | A: Inspect-only v1 (mirror M1.6) | user resolutions Q2 |
| Q3 META-FIX scope | A: Take both META-FIX opportunities now | user resolutions Q3 |
| Handle->BattleMech | Option B alone (linear scan resolver); Option D dropped post adversarial CRITICAL-1 (partId NOT lifetime-stable per `code/mission.cpp:2987`) | user resolutions + Section 6.1/6.2 |
| Cookie identifier | NONE -- handle is the only stable identity; `syncSpawn` stays `(*m3d, 0u)` | Section 6.1 (revised) |
| Gesture | Share Shift+LMB with static-prop pick | user resolutions |
| Log schema | Unified `[GAMEPLAY_PICK v1] kind=...` | user resolutions |
| Debug-state | Rename + kind discriminator (`GameplaySelectionDebugState`) | user resolutions |
| Debug-state shape | Overloaded setter (single struct + sentinel fields) | Section 4.3 |
| MLR fallback | No conditional code; transparent miss-path | user resolutions |
| Spine modification | NONE (M2-pre extension contract) | M2-pre Section 10 |
| Adapter resolver placement | `GameAdapters::Mech::findMechByHandle` | Section 4.2 |
| `lookupAtPixel` reentry per click | Two callers, two spine calls (simple form) | Section 7 |
| New env vars | `MC2_MECH_PICK`, `MC2_MECH_PICK_DEBUG`, `MC2_MECH_PICK_PIERCE_FOG` | Section 9 |

---

## 16. Open questions (post adversarial revision: all RESOLVED)

- **Q1. RESOLVED -- full retirement (no shim).**
  Spec-stage in-repo consumer survey (per adversarial MAJOR-3) on
  `tests/`, `scripts/`, `.claude/` for
  `setLastStaticPropPick|getLastStaticPropPick|clearLastStaticPropPick|StaticPropSelectionDebugState|[STATIC_PROP_PICK v1]`
  returned ZERO live consumers (excluding planning artifacts: SPEC /
  PLAN / RECON / REVIEW / HANDOFF docs that legitimately reference
  the retired state in their dated descriptions).
  Decision: ship full retirement; no shim. Gate 6 enforces zero
  matches in code + header doc comments + test/script tooling.

- **Q2. RESOLVED -- two calls (simple form).**
  Per Section 7 lean. Both callers independently dispatch; kind
  guard at each. Cost is estimated (not measured) but well below
  noise floor at the per-click stall budget. Optimization to a
  single spine call + kind dispatch is mechanical and deferred to
  M3 (when a third caller joins; the refactor amortizes at three+
  consumers). No M2.6 change.

---

## 17. Spec author notes (not part of the contract)

- This spec was written against M2.5 SHIPPED HEAD. Every cited
  symbol/line grep-verified at write time per worktree CLAUDE.md
  "Documentation discipline."
- Recon citations spot-checked:
  - `lookupAtPixel` copy site (recon: `RenderWorld.cpp:717-726`,
    verified -- exact match).
  - `LookupResult` shape (recon: `RenderWorld.h:157-167`,
    verified -- exact match).
  - `MechRenderAdapter.cpp:97` `desc.gameObjectId = 0` (recon:
    correct AS OF M2 ship; post-M2 the adapter accepts the
    value as a parameter; current call site at `code/mech.cpp:1338`
    passes `0u`. Post adversarial CRITICAL-1: M2.6 makes NO change
    here -- the call stays `(*m3d, 0u)`).
  - Mover-select sites (recon cited `:1472/1509/1736/1761`,
    matches HEAD; M1.6 spec cited `:1460/1487/1690/1705` which
    drifted -- M2-pre already noted this. Spec uses current
    `:1472/1509/1736/1761` for setSelected and
    `:1477/1512/1741/1764` for moverSelectedThisFrame).
  - Fog predicate (recon: `code/missiongui.cpp:1273-1278` --
    verified exact match; the predicate is
    `(target->getTeamId() != Team::home->getId()) &&
    !target->isDisabled() &&
    (((Mover*)target)->conStat < CONTACT_SENSOR_QUALITY_1)`).
- BattleMech identifier choice DROPPED post adversarial CRITICAL-1.
  Pre-revision spec chose `getPartId()`; adversarial review proved
  `partId` is NOT lifetime-stable (reassigned at
  `code/mission.cpp:2987` during group/commander setup AFTER
  `syncSpawn` has already stamped the cookie). Reverse lookup is
  Option B alone (linear scan on handle); no cookie populated.
- The Q3 META-FIX scope is the most ambitious part of the slice.
  Plan-stage execution should sequence:
  1. `LookupResult.kind` substrate change (1 commit; isolated;
     no consumer impact yet).
  2. M1.6 wrapper kind guard (1 commit; latent-bug fix lands).
  3. `findMechByHandle` resolver in `GameAdapters::Mech::` (1
     commit; M2.6 substrate-ready). NO `MechRenderAdapter` cookie
     populate -- post adversarial CRITICAL-1 the `syncSpawn` call
     site at `code/mech.cpp:1338` stays unchanged.
  4. `tryMechPick` consumer + env vars + boot banner (1 commit;
     mech wiring lands gated).
  5. Schema/state retirement (`StaticPropSelectionDebugState`
     rename + log schema rename + CLAUDE.md update) (1 commit;
     META-FIX completes).
  6. `RunMechPickSelfTest` validator (1 commit; Gate 4 lands).
- ASCII only; no emoji.

---

SPEC STATUS: REVISED -- adversarial CONDITIONAL-PASS (1 CRIT + 4 MAJOR + MINORs) findings applied

Open questions for human: NONE.

- **Q1.** RESOLVED -- full retirement, no shim (in-repo consumer
  survey returned zero hits per adversarial MAJOR-3).
- **Q2.** RESOLVED -- two calls (simple form) for v1; optimization
  deferred to M3 when a third caller joins.

If you find a real contradiction between recon and current code,
flag it as an open question. None found at write time -- the only
recon-vs-current drift was the `MechRenderAdapter.cpp:97` semantic
(recon described M2's zero-write; spec correctly identifies the
M2.6 change site at the caller `code/mech.cpp:1338`).

Adversarial findings applied: see "Adversarial findings applied"
table near the top of this spec for the per-finding change log.
Key CRITICAL-1 PIVOT: dropped `MechRenderAdapter::syncSpawn` cookie
populate entirely (`partId` not lifetime-stable per
`code/mission.cpp:2987`); reverse lookup is Option B alone (linear
scan); `code/mech.cpp:1338` stays byte-identical to M2 ship.
