# RenderWorld Slice M3 — TerrainRenderAdapter (Deferral Spec)

- **Date:** 2026-05-23
- **Status:** DRAFT — deferral-shape; 5 open Qs for user review
- **Predecessors:** M1, M1.5, M1.6, M2-pre, M2, M2.5, M2.6 (all SHIPPED 2026-05-23)
- **Recon source:** `docs/superpowers/explorations/2026-05-23-renderworld-slice-m3-terrain-recon.md`
- **Migration guide:** `docs/renderworld_migration_guide.md`
- **Boundary spec:** `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`

## Goal

Formalize the `RenderObjectKind::Terrain = 2` reservation that has lived as
a comment in `RenderWorld/RenderWorld.h:131-135` since M2. Ship NO writer,
NO adapter, NO consumer. This is a **reservation + documentation slice**:
it locks the enum slot, allocates a handle-index range for forward
compatibility, and records the future-trigger contract that would flip M3
from "deferred" to "implemented" if a consumer ever materializes.

## Architecture (this slice)

- **Enum value:** Append `Terrain = 2` to `RenderObjectKind` in
  `RenderWorld/RenderWorld.h`. Replace the `// Future: Terrain=2, ...`
  comment with a real enumerator. Document inline that no writer is wired
  in v1.
- **Handle-base constant:** Add `static constexpr uint32_t
  kTerrainHandleBase = 0x40000;` in `RenderWorld/RenderWorld.cpp`
  alongside the existing `kMechHandleBase = 0x00010000`. Document that
  this base is RESERVED — no code allocates from it in v1.
- **Defensive assert in `lookupAtPixel`:** If a `RenderObjectRecord` ever
  reports `kind == Terrain` while M3 v1 is shipped, that is a bug (no
  writer should be producing such records). Log a one-shot diagnostic
  `[RENDER_WORLD v1] WARN: unexpected kind=Terrain at pixel=(x,y)
  handle=N` and treat it as `isValid=false` for the caller. This is the
  trip-wire for a future implementation slipping in without flipping the
  M3 status.

## Tech stack

C++17 (engine); no GLSL changes; no GL state changes; no SSBO changes.
The only edits are header/source additions in `RenderWorld/` plus the
defensive lookup branch.

## Purpose / non-goals

**Purpose.** Lock the enum slot and handle-base now so a future "we need
per-quad terrain identity" request becomes a focused implementation slice
(M3.1+) rather than a re-spec exercise. Encode the reservation as
runtime-visible code, not just a comment, so the firewall and any future
grep for `RenderObjectKind` finds the slot and its allocated range.

**Non-goals (explicit).**

1. **No GPU writes.** None of the 5 terrain frag shaders
   (`shaders/gos_terrain.frag`, `gos_terrain_water_mdi.frag`,
   `gos_terrain_mine_static.frag`, `terrain_overlay.frag`,
   `gos_terrain_thin.vert` paired with `gos_terrain.frag`) is touched.
   No `layout(location=2) out uint v_objectId` declaration is added.
   No `MC2_OBJECT_ID_BUFFER` `#ifdef` is injected into terrain
   programs.
2. **No adapter.** No `GameAdapters/TerrainRenderAdapter.{h,cpp}`. No
   `RenderWorld::registerTerrain` / `destroyTerrain` API. No per-frame
   `terrain=T` counter in the banner.
3. **No CPU pick-path retirement.** `Terrain::IsGameSelectTerrainPosition`
   and the `wPos`-driven ground-click chain at
   `code/missiongui.cpp:773-789,1414,1652,2117,3660,4032` stay verbatim.
   The CPU `inverseProject -> wPos -> worldToTile` path remains the
   canonical terrain interaction.
4. **No env var.** No `MC2_TERRAIN_PICK`, no `MC2_TERRAIN_DEBUG`. The
   slice's runtime footprint is the enum value and the trip-wire
   warning.
5. **No new self-test.** The existing M1.5 `RunSubstrateSelfTest()`
   already validates the record-table generation/alive lifecycle; the
   trip-wire branch in `lookupAtPixel` is exercised passively whenever a
   stale terrain record exists (none should ever).

## Relationship to M2.6 + migration guide

The migration guide (`docs/renderworld_migration_guide.md` §4) prescribes
five questions for every new `RenderObjectKind`. M3 v1 answers them as
follows — each answer is the v1 contract, NOT a placeholder:

1. **What creates/destroys the handle?**
   **NOTHING in v1.** No call site allocates from `kTerrainHandleBase`. The
   reservation is dormant. If a future M3.1 ships per-quad identity, the
   creator will be `Terrain::init` (mission-load lifetime) and the
   destroyer will be `Terrain::destroy` (`mclib/terrain.cpp:895-896`).
2. **What kind does it report?**
   `RenderObjectKind::Terrain = 2`. Reserved-only; never produced by any
   live code path. The defensive assert in `lookupAtPixel` (§Architecture)
   logs and downgrades to `isValid=false` if the kind ever surfaces.
3. **Does it write object ID?**
   **NO.** Terrain frags continue to write only attachment-0 (`FragColor`)
   and, where present, attachment-1 (`GBuffer1`). Attachment-2 stays at
   the per-frame `glClearBufferuiv(GL_COLOR, 2, 0)` value across all
   terrain pixels. `lookupAtPixel` over a terrain pixel correctly returns
   `LookupResult{isValid=false}` via the existing `pixel==0` guard.
4. **How does lookup/pick/debug consume it?**
   **NOT YET.** No consumer exists. Shift+LMB on terrain returns
   `Outcome::miss` from `tryGameplayPick` (per M2.6 behavior; no
   regression). Plain LMB / RMB ground-click continues through the
   existing `wPos`-driven path and is unaffected.
5. **What legacy fallback remains?**
   The CPU `wPos` ground-click chain
   (`Terrain::IsGameSelectTerrainPosition + doMove(wPos)` plus the
   `worldToTile` / `getTerrain` / `getTerrainElevation` getters at
   `mclib/terrain.h:266-268,368,387`). This IS the canonical path for
   terrain interaction — it is not a fallback in the M2.5 MLR sense.
   Per recon §6, there is no MLR-rendered terrain path; the
   `mlr_mech_draws` analog does not exist for terrain.

## Surfaces (cited from recon §2.1, grep-verified at write-time)

Five terrain programs would acquire `v_objectId` writes IF M3 v2 ever
ships. M3 v1 touches NONE of them:

| Shader | Attachment-0 | Attachment-1 | M3 v1 touches? |
|---|---|---|---|
| `shaders/gos_terrain.frag:32,34` | FragColor | GBuffer1 | NO |
| `shaders/gos_terrain_water_mdi.frag:24,25` | FragColor | GBuffer1 | NO |
| `shaders/gos_terrain_mine_static.frag:29,31` | FragColor | GBuffer1 | NO |
| `shaders/terrain_overlay.frag:18` | FragColor | — | NO |
| (Shared with `gos_terrain.frag`: `gos_terrain_thin.vert` and `gos_terrain_surface.vert`) | — | — | NO |

`shaders/shadow_terrain.frag` writes depth only and is not part of the
main scene MRT — irrelevant in any era.

The `setSceneDrawBuffers` substrate is already armed for terrain pixels
per recon §2.3 (`gos_postprocess.cpp:488,497`). When M3 v2 ships, the
shader edit is the ONLY delta required — no helper change.

## The five architectural questions (for user review)

The recon (§9) raises five decisions that this spec cannot resolve
without input. The recon's lean is recorded against each; the spec
adopts the lean as a tentative v1 commitment, and asks the user to
confirm or override.

### Q1 — Identity unit choice

Per-quad / per-chunk / per-coord / per-triangle / NONE.

- **Recon lean: NONE for v1** (Option 5 in recon §3). No consumer drives
  the writer; the CPU `worldToTile` path already returns exact tile R/C,
  terrain type, and elevation without a GPU readback. Adding a writer
  would touch 5 frag shaders to encode an identity that nothing
  consumes — pure additive cost.
- **Trade-offs documented:**
  - Per-quad (Option 1) would fit comfortably in the 20-bit handle index
    (worst case ~196K mission-total quads vs 786K slots reserved in
    `[0x40000, 0xFFFFF]`).
  - Per-chunk (Option 2) is 16× coarser; same handle range; no
    additional consumer enabled.
  - Per-coord (Option 3) and per-triangle (Option 4) are NOT
    recommended (recon §3) — vertex interpolation defeats identity for
    Option 3; LOD instability defeats Option 4.
  - NONE (Option 5) is the strict-minimum ship; everything else is
    additive without a named consumer.

### Q2 — Picking semantic

If a consumer emerges, would it want: inspect / coord readback /
quad-select / metadata?

- **Recon lean: NONE for v1.** All four semantics (recon §8) are
  CPU-satisfiable today via the `inverseProject -> wPos` path and the
  `worldToTile` family. The GPU substrate adds info ONLY when the
  consumer wants "pick the terrain pixel even though a transparent
  overlay sits in front of it depth-wise" — no current backlog item
  asks for that.

### Q3 — Editor use case

Is there a current or planned mission editor that needs per-quad GPU
identity for terrain?

- **Recon observation:** The codebase has vestiges of editor support
  (`Terrain::selectVertex` at `mclib/terrain.h:294`, a `bool selected`
  debug bit at `mclib/quad.h:87` under `_DEBUG`). The recon did NOT read
  PROJECT.md per lean-intake scope, so cannot confirm whether the
  project's north star includes an editor.
- **Decision rule:** If YES, that editor is the consumer that flips M3
  from deferral to implementation. The spec should ship Option 1
  (per-quad) + Semantic C (quad-select) on the SAME release that lands
  the editor's first user-visible UI hook. If NO, Option 5 stands.

### Q4 — Water / decal sub-kind split

If terrain DOES get an identity, do water quads
(`gos_terrain_water_mdi.frag`) and decal/overlay quads
(`terrain_overlay.frag`) share `Terrain` kind, or split into
`Terrain::Base` / `Terrain::Water` / `Terrain::Decal` / `Terrain::Mine`?

- **Recon lean: share (single `Terrain` kind) for v1.** If the consumer
  cares about water vs decal vs mine, it can read tile metadata via
  `Terrain::getTerrain(tileR, tileC)` after the handle resolves. Per
  the `RenderObjectKind` "stable across releases — never renumber,
  only append" rule (`RenderWorld/RenderWorld.h:130`), splitting later
  costs only an enum append (Q4-yes path adds `Water=5, Decal=6,
  Mine=7` after the existing reservations).

### Q5 — `IsGameSelectTerrainPosition` retirement

Recon recommends **NO**. The CPU ground-click path is the right shape
for movement-target gameplay (`code/missiongui.cpp:1414, 1652, 3660,
4032`); 6+ call sites consume `wPos`, not a `RenderObjectHandle`. The
GPU substrate is not a substitute because:

- Inverse-projection of mouse XY against the depth buffer is already
  pixel-accurate (recon §8 observation).
- The consumers want `wPos` (a `Stuff::Vector3D`), not an identity
  handle — replacing the path adds a `Handle -> wPos` round-trip with
  zero new info.
- Retirement would be a much larger refactor than M3 should bear.

## API extensions

Exactly two source-level additions; both are minimal and discoverable.

### 1. Enum value

In `RenderWorld/RenderWorld.h` (replaces the `// Future: ...` comment):

```cpp
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    // M3 v1 (2026-05-23): reservation only. No writer is wired and no
    // call site allocates from kTerrainHandleBase. lookupAtPixel logs
    // a one-shot warning and returns isValid=false if this kind ever
    // surfaces in a record — that would indicate an unintended writer
    // has been introduced (see Slice M3 spec for the future-trigger
    // contract).
    Terrain    = 2,
    // Future: Vfx=3, Overlay=4
};
```

### 2. Handle-base constant

In `RenderWorld/RenderWorld.cpp` (alongside `kMechHandleBase`):

```cpp
// kMechHandleBase: M2 base for mech handles. Range [0x10000, 0x3FFFF].
static constexpr uint32_t kMechHandleBase = 0x00010000;

// kTerrainHandleBase: M3 v1 reservation (2026-05-23). RESERVED — no
// code allocates from this base in v1. If/when a M3.1 implementation
// slice ships per-quad terrain identity, this is the base. Range
// [0x40000, 0xFFFFF] reserves 786,431 slots — comfortably above the
// worst-case ~196K mission-total terrain quads on
// GameVisibleVertices=200. See docs/superpowers/specs/
// 2026-05-23-renderworld-slice-m3-terrain-spec.md.
static constexpr uint32_t kTerrainHandleBase = 0x00040000;
```

The constant is unreferenced in v1; a `(void)kTerrainHandleBase;` line
or `[[maybe_unused]]` attribute suppresses the unused-variable warning
without making the symbol disappear from the binary.

## Code changes (Existing / Replace blocks)

### Change 1 — enum value

**Existing (`RenderWorld/RenderWorld.h:131-135`):**

```cpp
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    // Future: Terrain=2, Vfx=3, Overlay=4
};
```

**Replace with:**

```cpp
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    // M3 v1 (2026-05-23): RESERVATION ONLY. No writer is wired in v1.
    // See docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md
    // for the future-trigger contract that would flip M3 to an
    // implementation slice. lookupAtPixel emits a one-shot WARN and
    // returns isValid=false if this kind ever surfaces in a record —
    // that is the trip-wire for an unintended writer.
    Terrain    = 2,
    // Future: Vfx=3, Overlay=4
};
```

### Change 2 — handle-base constant

**Existing (`RenderWorld/RenderWorld.cpp`, near `kMechHandleBase`
definition):** no `kTerrainHandleBase` declaration.

**Add (adjacent to `kMechHandleBase`):**

```cpp
// kTerrainHandleBase: M3 v1 reservation (2026-05-23). RESERVED — no
// code allocates from this base in v1. If/when a M3.1 implementation
// slice ships per-quad terrain identity, this is the base. Range
// [0x40000, 0xFFFFF] reserves 786,431 slots — comfortably above the
// worst-case ~196K mission-total terrain quads on
// GameVisibleVertices=200. See docs/superpowers/specs/
// 2026-05-23-renderworld-slice-m3-terrain-spec.md.
[[maybe_unused]] static constexpr uint32_t kTerrainHandleBase = 0x00040000;
```

### Change 3 — defensive lookup branch (trip-wire)

**Existing (`RenderWorld/RenderWorld.cpp::lookupAtPixel`, post-record
fetch, pre-return):** the lookup currently returns a populated
`LookupResult` for any alive record.

**Insert (after generation/alive check, before populating fields):**

```cpp
// M3 v1 trip-wire: no writer should ever produce a Terrain record in
// v1. If we see one, an unintended writer has slipped in. Log once
// and downgrade to invalid.
if (rec.kind == RenderObjectKind::Terrain) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::fprintf(stderr,
            "[RENDER_WORLD v1] WARN: unexpected kind=Terrain at "
            "pixel=(%d,%d) handle=0x%08x — M3 is reservation-only; "
            "see slice-m3 spec\n",
            x, y, handle.raw());
    }
    return LookupResult{};  // isValid=false default
}
```

### Change 4 — migration guide cross-reference (optional documentation)

**In `docs/renderworld_migration_guide.md` §12 (the `kMechHandleBase`
allocation table):** update the Terrain row from `TBD (0x00020000
recommended)` to the actual landed value `0x00040000` and mark "RESERVED
v1 (no allocator) — see slice-m3 spec".

The recon (§7) and the migration guide (§12) currently disagree on the
recommended base (recon: `0x40000`; guide: `0x20000`). The spec adopts
`0x40000` per recon's reasoning that the `[0x10000, 0x3FFFF]` range
should remain available as mech-expansion headroom (245,760 slots,
~4900× the current mech max of ~50). User-confirmable; see Q-bonus
below if there is a reason to prefer `0x20000`.

## Validation strategy

Small, because the slice is small.

1. **Tier1 5/5 PASS env-OFF.** Pixel-parity vs the M2.6 parent commit.
   No code path fires under env-OFF — the enum slot is unused, the
   constant is unreferenced, and the trip-wire branch is unreachable
   (no record carries `kind=Terrain`).
2. **Tier1 5/5 PASS env-ON `MC2_OBJECT_ID_BUFFER=1
   MC2_MECH_PICK=1`.** Substrate active for mechs/static props; still
   inert for terrain. The trip-wire branch should NOT log under any
   tier1 mission — if it does, that is a P0 spec bug (a producer has
   leaked).
3. **Firewall clean.** `sh scripts/check-include-firewall.sh` exits 0.
   No new includes; no allowlist changes needed.
4. **Grep gate.** Confirm no shader file under `shaders/` mentions
   `RenderObjectKind::Terrain` or `kTerrainHandleBase`. The reservation
   is C++-only; if a shader edit slips in alongside this slice, it is
   out of M3 v1 scope.
5. **Grep gate (positive).** `grep -rn "RenderObjectKind::Terrain"
   RenderWorld/ GameAdapters/` returns exactly 1 hit (the enum
   declaration in `RenderWorld.h`) plus the trip-wire branch in
   `RenderWorld.cpp` — total 2 occurrences. Any more = an unintended
   producer.

## Greybeard analysis — deferral as META-FIX

Per `.claude/skills/greybeard.md`, every fix carries an explicit ruling.
For M3 v1, the ruling argument is:

- **The "bug class" being retired:** the temptation to ship additive
  substrate without a consumer. The M2 entries in `CLAUDE.md` (and the
  migration guide §13) explicitly warn against this:
  `memory/feedback_offload_must_be_substitutive_not_additive.md`
  documents the additive-slice-nets-~0ms anti-pattern. Shipping a
  terrain writer here would touch 5 fragment shaders, add per-pixel
  `uint` writes across ~40k visible quads/frame, add a per-mission
  counter, and provide zero new information to any consumer. That is
  the canonical additive-without-substitution shape.
- **The substitutive proof for the deferral:** there is no live shape
  to substitute (no terrain writer exists pre-M3). The substitution is
  in the contract — by formalizing the reservation in code (enum +
  constant + trip-wire) instead of as a comment, the future M3.1
  implementation slice has a fixed target rather than a re-spec
  exercise.
- **Trigger condition for M3.1 (implementation):** a named consumer
  emerges. Candidates documented in recon §9:
  - A mission editor with per-quad terrain authoring (Q3 path).
  - A debug "click any pixel and read its terrain type" inspector that
    needs to pierce transparent overlays (recon §8).
  - A new gameplay gesture that wants pixel-precise terrain identity
    without the CPU `inverseProject` round-trip (no current backlog).
- **Ruling: META-FIX.** The deferral encodes the future contract; it
  retires the "every kind reservation lives as a comment until someone
  re-derives the slot allocation" bug class for future kinds (Vfx=3,
  Overlay=4 should follow the same reservation-with-handle-base
  pattern when they come up).

Alternative ruling (for reviewer to challenge): **PATCH (justified)** —
the reservation is a single-purpose code addition, not a structural
refactor; the META-FIX is the "always ship reservations as code, not
comments" discipline applied prospectively. If reviewer prefers
PATCH-justified, the named trigger remains: the next time a comment-only
reservation appears in the codebase, it gets the M3-shaped treatment.

## Threat model

Two traps; both mitigated by the spec's small surface area.

### Trap 1 — handle-base collision when M3.1 ships

A future M3.1 (or later kind) might pick `kTerrainHandleBase = 0x40000`
unaware that M3 v1 already reserved it (the constant is `[[maybe_unused]]`
and easy to miss). Mitigation:

- The migration guide §12 table is updated in this slice (Change 4) to
  list the landed value.
- The `RenderWorld.cpp` comment block at `kMechHandleBase` is extended
  to mention `kTerrainHandleBase`. Discoverability via the standard
  "where do I pick a base?" lookup.
- The trip-wire branch in `lookupAtPixel` provides runtime evidence if
  two future slices pick overlapping bases (a Terrain handle would
  surface in a non-Terrain record, or vice-versa, and the kind check
  would fire).

### Trap 2 — `lookupAtPixel` returning `kind=Terrain` while M3 v1 is
shipped

This is a P0 bug indicator. Mitigation: the trip-wire branch (Change 3)
logs once and downgrades to `isValid=false`. The log line includes the
pixel, handle, and references the spec — a future maintainer hitting
this knows exactly which contract is being violated. The
`MC2_OBJECT_ID_BUFFER=1` tier1 env-ON gate exercises every pixel that
could hit this branch; tier1 5/5 PASS is the validation.

### Trap 3 (minor) — silent enum reflection drift

The shader reflection CI (`tools/shader_reflect/reflect.py`, per
HANDOFF 2026-05-23 NIGHT) does not currently enumerate
`RenderObjectKind` values. Adding `Terrain = 2` to the enum does not
require a reflection golden update because no shader consumes the enum
directly. If a future kind adds a GLSL `#define MC2_KIND_TERRAIN 2`
prefix, the reflection contract may need extending — out of scope for
M3 v1.

## Resolved decisions (table)

| ID | Decision | Status |
|---|---|---|
| D-Q1 | Identity unit for v1 | **OPEN** — for user review (recon lean: NONE) |
| D-Q2 | Picking semantic if consumer emerges | **OPEN** — for user review (recon lean: NONE for v1) |
| D-Q3 | Editor use case drives M3 v2 trigger | **OPEN** — for user review (depends on PROJECT.md north star) |
| D-Q4 | Water/decal sub-kind split | **OPEN** — for user review (recon lean: share single Terrain kind) |
| D-Q5 | Retire `IsGameSelectTerrainPosition` | **OPEN** — for user review (recon lean: NO) |
| D-A | Enum value `Terrain = 2` | DRAFT (landed in this spec) |
| D-B | `kTerrainHandleBase = 0x40000` | DRAFT (landed in this spec; recon recommends; migration guide §12 said `0x20000` — spec adopts `0x40000`) |
| D-C | Trip-wire branch in `lookupAtPixel` | DRAFT (landed in this spec) |
| D-D | No env var, no adapter, no shader edit | DRAFT (landed in this spec; explicit non-goals) |
| D-E | No new self-test (M1.5 self-test exercises the table) | DRAFT (landed in this spec) |

## Open questions (for human)

The five Qs from the recon, verbatim with leans, for morning skim:

1. **Q1 — Identity unit for v1.** Per-quad / per-chunk / per-coord /
   per-triangle / NONE?
   *Recon lean: NONE for v1.*

2. **Q2 — Picking semantic if a consumer ever emerges.** Inspect /
   coord readback / quad-select / metadata?
   *Recon lean: NONE for v1 (all four are CPU-satisfiable today via
   `worldToTile` + getters).*

3. **Q3 — Editor use case.** Is there a current or planned mission
   editor in the project north star (PROJECT.md) that needs per-quad
   GPU identity for terrain? If YES, that is the trigger to flip M3
   to an implementation slice now.
   *Recon lean: unknown — PROJECT.md not consulted under lean-intake.*

4. **Q4 — Water/decal sub-kind split.** If terrain ever gets an
   identity, do water + decal + mine quads share `Terrain` kind, or
   split into `Terrain::Base / Water / Decal / Mine`?
   *Recon lean: share single Terrain kind for v1; split is an
   append-only enum change when needed.*

5. **Q5 — `IsGameSelectTerrainPosition` retirement.** Replace the CPU
   `wPos` ground-click path with a GPU-handle-driven path?
   *Recon lean: NO. CPU path is correct, fast, used by 6+ call sites,
   and the GPU substrate adds zero new info for movement-target
   gameplay.*

**Bonus Q (handle base).** Recon recommends `kTerrainHandleBase =
0x40000`; the existing migration guide §12 table suggests `0x20000`.
Spec adopts `0x40000` per recon's reasoning that `[0x10000, 0x3FFFF]`
should remain mech-expansion headroom. Confirm or override.

---

SPEC STATUS: DRAFT — deferral-shape; 5 open Qs for user review

- Q1: identity unit (lean: NONE)
- Q2: picking semantic (lean: NONE)
- Q3: editor use case (lean: unknown — depends on PROJECT.md)
- Q4: water/decal sub-kind split (lean: share single Terrain kind)
- Q5: `IsGameSelectTerrainPosition` retirement (lean: NO)
- Bonus: handle-base value `0x40000` vs guide's `0x20000` (lean: `0x40000`)
