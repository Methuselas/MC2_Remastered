# Tactical Overview Formation Line v1

## Goal

Add a default-off tactical overview command that lets the player draw a line on the battlefield and order the selected squad to occupy evenly spaced positions along that line.

Feature gate:

```text
MC2_TACMAP_FORMATION_LINE=1
```

Default: OFF.

## Scope

Formation line only.

No assault line. No fallback line. No facing. No spacing wheel. No role-aware assignment. No pathfinding rewrite.

## Input

Input lives in the main loop in `code/mechcmd2.cpp` near the existing F6 tactical overview polling, not in the mission command table.

State machine lives in `code/tacticaloverview.*`:

```text
IDLE
  L while F6 active and feature enabled -> ARMED

ARMED
  LMB down on terrain -> DRAGGING
  Esc / exit F6 -> IDLE

DRAGGING
  mouse move -> update line end
  LMB release -> issue orders -> IDLE
  Esc / right-click / exit F6 -> cancel -> IDLE
```

`L` is consumed only while the F6 tactical overview overlay is active.

## World mapping

Convert drag start/end from screen to approximate world ground positions using the same GL-consistent projection family as the tactical overlays.

Rules:

- Use modern GL projection path, not legacy D3D-style projectZ.
- Ground approximation is acceptable.
- Normal unit pathfinding handles final movement.

## Unit set

On `DRAGGING` start, snapshot selected friendly movers.

Selection source follows the existing roster/selection pattern:

- selected
- friendly/home commander
- movable actor only

On release, revalidate that each mover still exists and is commandable. Drop invalid movers silently.

## Slot generation

Let `N = selected mover count`.

- If `N == 0`, cancel.
- If `N == 1`, slot is midpoint of the line.
- If `N > 1`, generate `N` evenly spaced points from line start to line end, inclusive.

## Slot assignment

Use greedy nearest assignment.

Algorithm:

1. Sort movers by distance to the drawn line.
2. For each mover, assign the nearest free slot.
3. Remove assigned slot from the free list.

This minimizes obvious crossing without needing role/tonnage/path analysis.

## Orders

For each mover/slot pair, issue a single-unit move order:

```text
TacticalOrder
  code = TACTICAL_ORDER_MOVETO_POINT
  target = slot world position

pMover->handleTacticalOrder(order)
```

Bypass group clustering paths such as `handleOrders` / `calcMoveGoals`, because those would re-cluster the slots and defeat the line.

## Visuals

During `ARMED`:

- show small "DRAW FORMATION LINE" cursor hint if cheap
- no slots until drag begins

During `DRAGGING`:

- draw ghost line
- draw slot pips
- optionally draw small selected-unit count

Visual rules:

- project pips with `projectModernClipGL`
- wrap draw in `gos_SetHudScaleExempt(true)`
- cull only on `w > 0`
- pips green in v1
- no reachability color yet

## Files

Expected files:

- `code/tacticaloverview.h`
- `code/tacticaloverview.cpp`
- `code/mechcmd2.cpp`

Optional tiny accessor/helper only if needed:

- `code/controlgui.cpp`
- `code/missiongui.cpp`

Forbidden:

- no `mclib` changes
- no pathfinding changes
- no mission command table dependency
- no weapon/sensor overlay changes

## Acceptance

With env OFF:

- no hotkey behavior
- no draw behavior
- tier1 unchanged

With env ON:

- enter mission
- activate F6 tactical overview
- select 2-8 friendly movers
- press `L`
- drag line on terrain
- see ghost line and N slot pips
- release mouse
- each selected mover receives a move order to an assigned slot
- Esc cancels
- exiting F6 cancels
- tier1 5/5 passes

## Known risk

Units may path through or around each other because v1 issues independent move orders. This is acceptable. The feature expresses tactical intent; normal pathfinding resolves movement.

## Reference anchors (recon 2026-06-11)

- Right-click move chain: `missiongui.cpp:1519` (input) -> `doMove` `:3135` -> `tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false)` `:3165` -> `handleOrders` `:2467` -> `pMover->handleTacticalOrder(order)` `:2529`.
- Selection enumeration pattern: `missiongui.cpp:2487-2500` (`Team::home` roster, `isSelected()`, home-commander filter).
- `MechWarrior::orderMoveToPoint` signature: `warrior.h:1979`, impl `warrior.cpp:5962`. No facing parameter exists — facing is a later slice.
- Group clustering to bypass: `MoverGroup::calcMoveGoals` `group.cpp:401`; `sortMovers`/`selectionIndex` `group.cpp:374`.
- Overlay projection rules: see `memory/tactical_overview_camera.md` (projectModernClipGL, gos_SetHudScaleExempt, w>0 cull, worldUnitsPerMeter=5.01).
