# M2.6 Spec — Pre-spec User Decisions Resolved

**Date:** 2026-05-23
**Resolves:** open questions surfaced by recon at
`docs/superpowers/explorations/2026-05-23-renderworld-slice-m2-6-mech-pickup-recon.md`

Plan-writer + spec-author MUST read this file ALONGSIDE the recon.

---

## Q1 — Fog-of-war handling

**Resolution: A — Respect fog by default. Optional pierce via env.**

Preserve sensor gameplay. GPU object-ID must NOT become a cheat path.

```
Default:
  GPU mech pick must respect existing fog / sensor visibility rules.

Optional debug:
  MC2_MECH_PICK_PIERCE_FOG=1
    allows inspect through fog for debugging only.
```

If the object-ID buffer can see a mech but gameplay cannot, `tryGameplayPick`
must return `None` (or fall back to legacy behavior) unless the debug env is
set.

**Plan implication:**
- After `lookupAtPixel` returns `kind=Mech`, the mech-pick consumer must
  query the mech's gameplay visibility (sensor / contact / LOS — whatever the
  CPU pick path consults at `code/missiongui.cpp:1273-1278` / `objmgr.cpp:3093`).
- If invisible-to-gameplay AND `MC2_MECH_PICK_PIERCE_FOG` not set: outcome
  is `gated` (per spine semantics — not `miss`, not `hit`). Log line emitted
  only when the env var IS set, to keep the default path silent.
- Spec must name the exact gameplay visibility predicate to query; recon
  cites `objmgr.cpp:3093` area as the source of truth.

---

## Q2 — Inspect-only vs select

**Resolution: A — Inspect-only v1. Mirror M1.6.**

```
M2.6 mech pick:
  log / inspect / debug-state only
  no actual selection mutation
  no attack target
  no command routing
  no gameplay state change
```

This keeps M2.6 as a consumer-substrate proof, not a gameplay semantics
slice. Selection/attack lands in a future M2.7 after the inspect path is
proven stable.

**Plan implication:**
- The mech-pick consumer emits a log line + updates the unified
  `GameplaySelectionDebugState` (per Q3). NOTHING else.
- No edits to `Team::setSelected`, attack routing, command queue, mover
  selection state. The existing mover-first short-circuit in
  `tryGameplayPick` is preserved verbatim.

---

## Q3 — META-FIX scope

**Resolution: A — Take both META-FIX opportunities now.**

The latent post-M2.5 bug is the trigger event (M1.6's `tryStaticPropPick`
assumes any `outcome=hit` is a static-prop hit; M2.5 broke that invariant by
adding mech writes to the same substrate). Fix is mandatory. While we're
there, retire the bug class: unify the log schema and debug-state slot
before M3/M4 add terrain/VFX as third+fourth categories that inherit the
same trap.

```
M2.6 META-FIX scope:
  LookupResult.kind                    (mandatory; 2-line addition)
  kind checks in every consumer        (mandatory; latent-bug fix)
  fix static-prop mislabel bug         (mandatory; result of the above)
  unified [GAMEPLAY_PICK v1] log schema   (META-FIX; retires log-name-per-kind)
  unified GameplaySelectionDebugState     (META-FIX; retires state-slot-per-kind)
```

**Plan implication:**
- New field: `RenderObjectKind kind` on `RenderWorld::LookupResult`
  (`RenderWorld/RenderWorld.h:157-167`). Copy from `RenderObjectRecord.kind`
  at `RenderWorld.cpp:717-726`.
- Update `code/gameplay_pick.cpp::tryGameplayPick` Result to carry `kind`
  (probably already there — verify via grep).
- M1.6 wrapper at `MissionInterfaceManager::tryStaticPropPick` (in
  `code/missiongui.cpp`): add `if (result.kind != RenderObjectKind::StaticProp) return;`
  guard BEFORE `setLastStaticPropPick` to fix the latent mislabel bug.
- Log schema: change `[STATIC_PROP_PICK v1] hit handle=...` to
  `[GAMEPLAY_PICK v1] kind=StaticProp hit handle=...`. Mech pick emits
  `[GAMEPLAY_PICK v1] kind=Mech hit handle=... mech_handle=PTR ...`.
- Debug-state: rename `RenderWorld::StaticPropSelectionDebugState` to
  `RenderWorld::GameplaySelectionDebugState` with a `RenderObjectKind kind`
  discriminator field. The setter becomes `setLastGameplayPick(kind, ...)`.
  Single mutex-guarded slot serves both kinds (and future kinds).
- CLAUDE.md M1.6 entry will be UPDATED in the SHIPPED docs commit to
  reflect new log schema (so future archaeologists grep correctly).

---

## Resulting M2.6 contract (user-provided)

```
Shift+LMB:
  existing mover path still wins where applicable
  GPU object-ID lookup next
  if kind=StaticProp:
    static-prop inspect path
  if kind=Mech:
    fog/sensor check
    if visible or MC2_MECH_PICK_PIERCE_FOG=1:
      mech inspect path
    else:
      no GPU pick / fallback
  else:
    none

Default behavior:
  no gameplay selection/targeting changes
  inspect/log/debug-state only
```

---

## Handle→BattleMech reverse lookup

**Resolution: Recon's recommendation — Option D + Option B (combined).**

- M2.6 sets `RenderObjectRecord.gameObjectId` at mech-register time. The
  value carries enough information for the reverse lookup (TBD — spec
  decides: BattleMech `getPartId()`? `getWatchID()`? Some identifier the
  game-side already maintains).
- The reverse resolver (in `MechRenderAdapter` or `code/gameplay_pick.cpp`)
  does a linear scan over live `BattleMech` instances and matches by either
  `gameObjectId` or `getRenderWorldHandle() == h`. N ≤ ~50 per mission;
  cost is negligible compared to the readback already incurred by
  `lookupAtPixel`.

**Plan implication:**
- M2 currently sets `desc.gameObjectId = 0` at `MechRenderAdapter::syncSpawn`.
  M2.6 changes this to a real value sourced from the BattleMech. Spec must
  identify which identifier — preferably one that already has a public
  game-side accessor and stable lifetime.
- Adapter gains a public `findMechByHandle(RenderCore::RenderObjectHandle h)
  -> BattleMech*` or similar. Returns nullptr on stale/unknown handle. The
  M2-pre extracted spine consumer in `gameplay_pick.cpp` calls this.

---

## Gesture mapping

**Resolution: Recon's recommendation — share Shift+LMB with static-prop pick.**

- Mover-first short-circuit already handles friendly mechs (caught by
  legacy `setSelected` before `tryGameplayPick` fires).
- Static-prop and mech writes occupy disjoint pixel sets at the substrate
  layer (different handle ranges: static-prop indices < 2641, mech indices
  ≥ kMechHandleBase=65536). So `lookupAtPixel` returns at most one kind per
  pixel.
- On visible hostile mech click: M2.6 fires AFTER the existing CPU pick
  `doAttack()` path completes its work — preserves all existing combat
  semantics. Inspect-only v1 makes this safe.
- No new modifier needed. No new menu state.

---

## Log schema (META-FIX form)

**Resolution: unified `[GAMEPLAY_PICK v1] kind=X ...` form.**

Existing M1.6 emit:
```
[STATIC_PROP_PICK v1] hit handle=N idx=N gen=N recipe=N screen=(x,y) gl=(x,y)
```

New unified emit (M2.6 retires the M1.6 schema):
```
[GAMEPLAY_PICK v1] hit kind=StaticProp handle=N idx=N gen=N recipe=N screen=(x,y) gl=(x,y)
[GAMEPLAY_PICK v1] hit kind=Mech       handle=N idx=N gen=N gameObjectId=N screen=(x,y) gl=(x,y)
[GAMEPLAY_PICK v1] miss screen=(x,y) gl=(x,y)
[GAMEPLAY_PICK v1] gated kind=Mech reason=fog_of_war screen=(x,y) gl=(x,y)
```

Plan must include a CLAUDE.md M1.6 entry update + memory file note for the
schema change (anyone grepping for `[STATIC_PROP_PICK v1]` after M2.6 ship
needs to find a pointer to the rename).

---

## Debug-state slot (META-FIX form)

**Resolution: rename + add kind discriminator.**

`RenderWorld::StaticPropSelectionDebugState` -> `RenderWorld::GameplaySelectionDebugState`.

Struct gains `RenderObjectKind kind` field. Kind-specific fields stay (e.g.
`recipe` for static-prop, `gameObjectId` for mech). Setter becomes
`setLastGameplayPick(kind, ...)` overloaded or with a single tagged-union /
optional payload (spec author's choice — pick whichever fits codebase style).

Cleared on `RenderWorld::destroy()` (same per-mission lifetime as before).

---

## MLR fallback

**Resolution: no conditional logic. Recon's "free win" analysis applies.**

MLR-rendered mechs write no objectID, so the substrate pixel reads as
`raw==0`. `lookupAtPixel` returns `Handle::invalid()`. Spine returns
`outcome=miss`. Existing miss-path code handles it transparently.

Empirical tier1 data: `mlr_mech_draws=0` across all 5 missions. So the
miss-path is rarely (or never) exercised in practice. Belt-and-suspenders:
no conditional code; the system fails gracefully without anyone noticing.

---

## Read-order precedence

If recon, this file, spec, plan, and adversarial reviews disagree:

1. **Spec** (after this resolutions file is folded in)
2. **This resolutions file** (overrides recon "leans" where they differ)
3. **Recon** (factual ground truth except where resolved differently here)
4. **Plan adversarial review** (overrides plan body IF CRITICAL or MAJOR;
   MINOR is advisory)

If user instruction directly contradicts any of the above, user wins.
