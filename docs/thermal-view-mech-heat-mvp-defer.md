# Thermal ViewMode real-data MVP — DEFER (THERMAL-VIEW-MECH-HEAT-MVP-1)

Status: **DEFERRED.** Slice 2 of `GAMEADAPTERS-VISUAL-STATE-BRIDGE-OPUS-1`.
No renderer change shipped. This records why, and the exact split a future
lane should take. The data substrate it needs already shipped in Slice 1
(`MECH-VISUAL-STATE-BRIDGE-1`).

## Why deferred

Two independent blockers, either of which is sufficient:

### 1. There is no heat to show — `USEHEAT` is compiled out
The MechWarrior heat simulation (`heat`, `heatDissipation`, `updateHeat()`,
`NumHeatLevels`) lives entirely behind `#ifdef USEHEAT` in `code/mech.cpp`
(lines 238, 391, 1170, 2792, 3012, 3101, 3312, 3454, 3615, 3634, 6963, …).
`USEHEAT` is **never defined** anywhere in the repo. `BattleMech::updateHeat()`
does not run; there is no live runtime heat value to read. Reviving it is a
**mech-simulation change**, explicitly out of scope for a renderer bridge.

Consequence: a "real mech heat" Thermal view is impossible without first
shipping a separate USEHEAT-revival gameplay arc. `MechVisualState::heat01`
is wired through the bridge but is always `0.0`.

### 2. The remaining proxy (damage01) needs a broad new GPU resource
Slice 1 bridges `damage01` (composite health from `getStatusRating()`), which
*could* drive a "damage = hot" Thermal stylization. But the data lands in the
per-instance `GpuMechInstance` SSBO, indexed by **draw-instance slot**. The
Thermal postprocess pass identifies a pixel's object only through the
object-ID buffer (`sceneObjectIdTex_`, `GL_R32UI`, a `RenderObjectHandle`:
`[19:0]` index, `[31:20]` generation — `gos_postprocess.cpp:175`, written by
`mech.frag:85,198`). There is **no handle→instance map** available in the
composite, so the shader cannot reach `GpuMechInstance.visualDamage01` today.

Bridging it requires a NEW per-object GPU table keyed by handle index
(`objectIdRaw & 0xFFFFF`), uploaded each frame, plus a `postprocess.frag`
change. That is the "broad SSBO/resource addition" the slice's own stop
condition says to **stop and split** on.

## What ships instead

Nothing in the renderer. Thermal remains the shipped **luminance placeholder**
(`shaders/postprocess.frag:164-179`, iron palette over scene luma), which is
honest and already documented as a placeholder in `docs/thermal-ir-design.md`.
Do **not** relabel it as real thermal.

## Split path for a future `THERMAL-VIEW-MECH-DAMAGE-MVP` lane

1. **Per-handle visual SSBO (renderer-owned).** A compact array sized to the
   max live RenderObjectHandle index, holding `{ uint8 kind; unorm8 damage01; }`
   (or just `damage01`). Populated CPU-side each frame from the mech batcher
   (which knows both the handle and the Slice-1 `damage01`) — and optionally
   from `RenderObjectRecord::kind` for non-mech classification. Self-contained
   in the renderer; no firewall crossing (damage01 already arrived via the
   Slice-1 game→appearance feed).
2. **Bind + sample in Thermal.** The OID texture is already bound at composite
   unit 2 and declared (`usampler2D u_objectIdTex`). Thermal frag:
   `idx = texture(u_objectIdTex, uv).r & 0xFFFFFu; d = ssbo[idx].damage01;`
   then colorize mech pixels by damage (cool→hot), terrain/statics cooler,
   emissive/VFX stay luminance-hot.
3. **Gate.** ViewMode-only (`MC2_VIEWMODE_FRAMEWORK=1`, mode 3 Thermal). Visual
   mode (0) is untouched → no default behavior change.
4. **Validate.** Visual mode byte-identical; Thermal shows damaged mechs hotter;
   0 GL errors.

## Stop conditions for that future lane
- If the per-handle SSBO upload proves hot-path-expensive at mech-heavy
  missions, fall back to a coarse object-class (mech/terrain/static) buffer.
- If `damage01` reads as misleading (it folds weapon-effectiveness + pilot
  wounds, not pure armor), expose a pure-armor ratio on `MechVisualState`
  instead — still game-side, sanitized.

## Cross-references
- `RenderCore/MechVisualState.h` — the bridged state (Slice 1).
- `docs/thermal-ir-design.md` — placeholder + Phase 2 MRT-heat plan.
- `docs/sensor-contact-presentation-recon.md` — same firewall constraint.
- `docs/viewmode-capture-matrix.md` — the framework Thermal plugs into.
