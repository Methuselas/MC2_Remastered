# Sensor / contact presentation — recon (SENSOR-CONTACT-PRESENTATION-RECON-1)

Status: **DEFER. No renderer-only implementation is possible today.** This doc
records the recon verdict (Subagent E, 2026-05-29) and the minimal bridge a
future lane would need. No code shipped in this opus.

## The data exists — but only on the gameplay side

Contact state is fully structured and updated every frame in `code/`:

- `code/dcontact.h` — `ContactStatus` enum: `CONTACT_NONE`,
  `CONTACT_SENSOR_QUALITY_1..4`, `CONTACT_VISUAL`.
- `code/contact.h` — `ContactInfo::contactStatus[MAX_TEAMS]`, `SensorSystem`,
  `TeamSensorSystem::getContactStatus(MoverPtr, bool)`; global `SensorManager`.
- `code/mover.h` — `Mover::conStat` (int32_t), a per-frame cached copy of the
  home-team contact status (refreshed in `mech.cpp` / `gvehicl.cpp`, then fed to
  the **legacy** `appearance->setSensorLevel(...)` path only).

## The renderer cannot see it

- `RenderWorld` / `GameOS` / GPU mech batcher have **zero** contact access.
  `RenderMechDesc` carries `mechTypeId` / `gameObjectId` / `debugCookie` only.
- `setSensorLevel()` drives the legacy CPU MLR appearance
  (`sensorTriangleShape`/`sensorSquareShape`), not the GPU batcher.
- The `RenderWorld.h` firewall forbids game-side includes, so the renderer
  cannot reach `conStat` without an adapter bridge.
- Net: **no renderer-only presentation is possible now** — any attempt is
  either a no-op (no data) or a firewall violation.

## Minimal bridge (future, gameplay-collaborator + adapter lanes)

1. **Adapter call (small):** `RenderWorld::updateMechContactStatus(handle, uint8_t)`
   driven from the same sites that call `appearance->setSensorLevel()`
   (`mech.cpp`, `gvehicl.cpp`). Translate `CONTACT_*` → compact renderer enum.
2. **Storage:** `uint8_t contactStatus` in `RenderObjectRecord` (CPU-side only;
   no GPU impact).
3. **Consume:** either CPU-side in the GPU batcher (gate submit / set a
   per-draw uniform — lowest risk) or pass as an instance attribute for
   per-tier shader visuals (sensor-quality 1..4 distinct looks).

## Boundary (must hold)

```
GAMEPLAY (code/)                         RENDERER (RenderWorld/GameOS/mclib)
ContactInfo::contactStatus[]             RenderObjectRecord::contactStatus  [MISSING]
Mover::conStat (per-frame)         →→→   [needs adapter bridge — GameAdapters only]
SensorSystem / TeamSensorSystem          (gameplay-owned; must never cross)
gametacmap drawBlip/drawSensor           (code/-side 2D; not a renderer object)
```

## Deferred to gameplay/collaborator lanes
- All detection logic (range, ECM/ECCM, LOS) — already correct in `code/`.
- **Stale "last known position"** ghost contacts — MC2 does not track a last-seen
  position today; needs new gameplay-side data.
- Minimap/radar overlay modernization (`code/gametacmap.cpp`).

## Recommendation
Name the `updateMechContactStatus` API and land the adapter call FIRST (gameplay
collaborator + small adapter lane). Only then start a renderer visual lane.
Starting renderer-side now yields a no-op or a firewall breach.

## Cross-references
- `docs/thermal-ir-design.md` (same firewall constraint for heat data)
- `docs/viewmode-capture-matrix.md` (the presentation framework a future
  sensor mode would plug into as ViewMode::TacticalOverlay / a new mode)
