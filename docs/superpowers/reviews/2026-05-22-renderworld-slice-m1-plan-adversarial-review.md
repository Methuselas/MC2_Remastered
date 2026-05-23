# Adversarial review: RenderWorld Slice M1 plan (2026-05-22)

- Target: `docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`
- Spec: `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`
- Prior spec review: `docs/superpowers/reviews/2026-05-22-renderworld-boundary-spec-adversarial-review.md`
- Reviewer pass: code-grounded; every cited file/line/symbol re-grepped at write time.
- Mandate: try to BREAK the plan.

## Verdict

EXECUTE WITH FIXES. The plan is well-structured and the 5-site audit
holds verbatim under fresh grep, but it has one CRITICAL boundary
failure (RenderCore drags GL+mclib headers via the batcher include),
one CRITICAL bdactor edit pattern that drops a load-bearing H4 fix if
applied literally, two MAJOR scope omissions vs the user's D3 resolution
and the spec's Section 12 firewall-scope list, and several MINOR drifts.
None are architectural — they are precise mechanical corrections.

Counts: 2 CRITICAL, 4 MAJOR, 5 MINOR.

---

## CRITICAL findings

### C1. `RenderCore/RenderObjectDesc.h` transitively pulls GL and mclib headers — plan's own "no GL, no game headers" invariant is violated on Task 2

- Plan claim (line 130): "`RenderCore/` -- pure types; no GL, no game headers".
- Plan Task 2 Step 2 (line 332): `RenderCore/RenderObjectDesc.h` includes
  `"../GameOS/gameos/gos_static_prop_batcher.h"` to reach `GpuStaticPropInstance`.
- Grep `GameOS/gameos/gos_static_prop_batcher.h:3-10` shows its includes:
  ```
  #include <cstdint>
  #include <cstddef>
  #include <vector>
  #include <unordered_map>
  #include <GL/glew.h>            <-- GL!
  #include "Stuff/Stuff.hpp"      <-- mclib (mclib/Stuff/)
  #include "tgl.h"                <-- mclib (mclib/tgl.h, defines TG_MultiShape forward at :562)
  #include "msl.h"                <-- mclib (mclib/msl.h:241 defines class TG_MultiShape)
  ```
- Consequence: any TU including `RenderCore/RenderObjectDesc.h` (i.e.
  `RenderWorld/RenderWorld.cpp`, `GameAdapters/StaticPropRenderAdapter.cpp`,
  and any future RenderCore consumer) drags in `<GL/glew.h>` plus three
  mclib headers. The plan's stated "RenderCore is pure types" invariant
  is false on first task that compiles. Task 2 Step 1 explicitly tells
  the executor to STOP and re-decide D2 if this happens; the plan should
  follow its own escalation.
- Note on spec firewall: spec Section 12 forbids includes of the
  Appearance hierarchy + mission/warrior/objectmanager. `tgl.h`, `msl.h`,
  `Stuff.hpp` are engine-side math/geometry primitives and NOT on the
  Section 12 forbidden list, so this is not a spec-firewall violation.
  But the PLAN'S stated "no GL" invariant for `RenderCore/` is broken,
  and that invariant is what makes RenderCore Vulkan-shape.
- Recommended fix: D2 reconsideration is warranted (the plan's own Task 2
  Step 1 says so). Either (a) define `RenderCore/StaticPropInstance.h` as a
  POD mirror of `GpuStaticPropInstance` and let the adapter copy between
  them; or (b) move `StaticPropDesc` out of RenderCore into RenderWorld/
  so RenderCore remains GL-free; or (c) keep current shape but DROP the
  "no GL, no game headers" claim from line 130 and document the transitive
  GL drag as an M1 carve-out matched to the registry's existing posture.
  Decision needs user sign-off.

### C2. Task 8 / Task 10 block-replacement drops the load-bearing H4 follow-up flag (`needsFullBakeNextFrame = true`)

- Plan Task 8 Step 3 (around plan line 1142) shows "Existing" as 5 lines
  ending at `staticReg.shape = bldgShape;` and "Replace with" as 9 lines
  ending the same way. The replacement keeps the assignment surface.
- Grep `mclib/bdactor.cpp:1465-1483` shows the FULL existing block:
  ```
  if (submittedToGpu && !staticReg.registered
          && GpuStaticPropRegistry::isEnabled()
          && !needsFullBakeNextFrame
          && isStaticEligible()) {
      const auto& batch =
          GpuStaticPropBatcher::instance().getLastBuiltBatch();
      staticReg.recipeIndex = GpuStaticPropRegistry::registerRecipe(
          bldgShape, batch);
      staticReg.registered  = (staticReg.recipeIndex >= 0);
      staticReg.shape       = bldgShape;
      if (staticReg.registered) {
          // H4 follow-up (2026-05-07): per-frame re-registration
          // after damage/shape swap has the same lightData_ gap as
          // mission-load registerStatic(). Force one full update()...
          needsFullBakeNextFrame = true;
      }
  }
  ```
- The plan's "Existing" block shows only lines 1469-1474; the H4 block
  at 1475-1482 is NOT shown. A literal "replace these N lines" execution
  is ambiguous about whether the H4 follow-up is preserved.
- Symmetric concern at `bdactor.cpp:4264-4287` (Task 10) — the
  `if (staticReg.registered) { needsFullBakeNextFrame = true; }` block
  is present in the source and absent from the plan's "Existing" snippet.
- Why this matters: dropping `needsFullBakeNextFrame = true` regresses
  the documented H4 black-actor / black-tree fix (lightData_ gap on
  per-frame re-registration after damage / shape swap). Smoke might not
  catch this within 30s tier1 because the bug manifests after damage
  events; user would land a regression the existing 2026-05-07 fix was
  added precisely to prevent.
- Recommended fix: Tasks 8 and 10 must show the FULL surrounding block
  in "Existing" (including the trailing H4 `if (staticReg.registered)
  { needsFullBakeNextFrame = true; }`) and the FULL replacement so the
  preservation of H4 is explicit. Don't trust the executor to "see"
  unchanged trailing code; either show it or call it out by line range.

---

## MAJOR findings

### M1. Adapter surface does not match user's D3 resolution

- User resolution recorded at plan lines 22-26: D3 stateful TU; required
  entry points are `beginMission(RenderWorld&)`, `endMission(RenderWorld&)`,
  `syncStaticProp(...)`, `destroyStaticProp(...)`.
- Plan Task 7 Step 1 defines the adapter surface as:
  `registerStaticPropRecipe(...)`, `registerStaticPropLateSpawn(...)`,
  `destroyStaticProp(...)` (plan lines 900, 915, 922). NEITHER
  `beginMission` nor `endMission` is defined; `syncStaticProp` is named
  `registerStaticPropRecipe` instead.
- Consequence: the user-acknowledged per-mission lifecycle hooks are
  not in the code. Banner counters reset via `RenderWorld::init/destroy`
  (process-lifetime per Task 13), not via per-mission scope. Plan
  acknowledges this implicitly in RenderWorld.h doc comments ("M1 does
  not yet model mission scope"), but D3 resolution promised the hooks.
- Recommended fix: add `GameAdapters::beginMission(RenderWorld&)` and
  `endMission(RenderWorld&)` no-op stubs (or wire them to
  `RenderWorld::init/destroy` calls) and rename `registerStaticPropRecipe`
  to `syncStaticProp` per D3. OR surface to user that D3 names were
  aspirational and the plan's chosen names are equivalent; get sign-off
  on the rename.

### M2. Firewall script's SCOPE_DIRS is narrower than spec Section 12 list

- Spec Section 12 enforcement plan (lines 905-909): "case-sensitive grep
  across `RenderCore/`, `RenderWorld/`, `Visibility/`, `MeshRenderer/`,
  `MaterialSystem/`, `DebugRenderer/`, `RenderDeviceGL/`".
- Plan Task 15 Step 2 (plan line 1619): `SCOPE_DIRS="RenderCore RenderWorld"`.
- Plan acknowledges this implicitly: "M1 only creates RenderCore +
  RenderWorld; the others don't exist yet." That is true but the script
  silently passes when a future slice introduces (e.g.) `Visibility/` and
  forgets to extend `SCOPE_DIRS`. The script as written is single-slice
  scoped, not boundary-scoped.
- Recommended fix: enumerate the full Section 12 list with `[ -d $dir ]
  || continue` guards; existing logic already skips missing dirs at line
  1648. Cost: ~5 lines of text in the script; immune to future slice
  drift. Or: add an explicit "Phase 1 scope: RenderCore + RenderWorld
  only; future slices extend this list" comment in the script header.

### M3. Sentinel translation lives in TWO places — spec says "exclusively at the adapter boundary"

- Spec Section 10 (lines 664-679): "The adapter MUST translate at the
  boundary, both directions. The adapter is the ONLY place this
  translation is allowed."
- Plan implements translation in `StaticPropRenderAdapter.cpp::registerStaticPropRecipe`
  (plan lines 986-990) AND in `RenderWorld.cpp::recipeIndexToHandleIndex`
  / `handleToRecipeIndex` (plan lines 681-693). The plan calls this
  "both endpoints translate" (plan line 651-652).
- The duplication is semantically harmless because RenderWorld.cpp is
  the engine-side forwarder and the adapter is the game-side bridge,
  but the spec word "exclusively" is violated as written. Plan R1
  mitigation (line 1959-1969) acknowledges "the translation lives in
  exactly two places" — so the plan is self-aware about this but does
  not flag it as a spec deviation.
- Recommended fix: either (a) remove the engine-side helpers and have
  `RenderWorld::upsertStaticProp` return `int32_t`, with the adapter the
  sole translator (matches spec literally; means RenderWorld API leaks
  int32_t which contradicts the M1 "no -1 above the adapter" rule); or
  (b) keep current shape and surface to user that "exclusively" in the
  spec means "exclusively at the game/engine seam, which has TWO
  endpoints in M1 because the engine boundary itself crosses the
  registry's int32_t legacy API." Recommend (b) with spec amendment.

### M4. `RenderObjectHandle::raw()` packing inconsistent with bit layout

- Plan Task 1 Step 2 (plan line 232): `return (generation << 20) | index;`
- The struct declares `uint32_t index : 20; uint32_t generation : 12;`
  (plan lines 220-221). In a typical little-endian MSVC bitfield layout,
  `index` occupies bits [0:19] and `generation` occupies [20:31]. The
  `raw()` formula `(generation << 20) | index` reproduces that exact
  bit packing — but ONLY if the compiler picked that layout (MSVC
  guarantees it for same-type adjacent bitfields, but it is implementation-defined).
- The compile-time `static_assert(sizeof(Handle) == sizeof(uint32_t))`
  protects size but NOT layout. If a future port (Vulkan, clang) lays
  bits differently, `raw()` and the in-memory representation diverge —
  Handle::invalid() (index=0, gen=0) still works, but any cross-process
  / cross-target wire-format use breaks silently.
- Recommended fix: drop bitfields and use explicit shift/mask: `uint32_t
  bits;` plus `index() const { return bits & 0xFFFFFu; }` / `generation()
  const { return bits >> 20; }`. Vulkan-shape stays identical; layout is
  no longer implementation-defined; `raw()` is just `bits`. (This is the
  same fix the spec's own M1.5 ObjectID R32_UINT attachment will need.)

---

## MINOR findings

### m1. Task 13 cites a wrong file for `init/destroy` insertion point

- Plan Task 13 Step 1 (plan line 1460) suggests grep'ing
  `GameOS/gameos/gameosmain.cpp` for the hook.
- Grep result for `GpuStaticPropRegistry::init` across `code/ mclib/ GameOS/`:
  - `code/mission.cpp:1693` — `GpuStaticPropRegistry::init();`
  - `code/mission.cpp:3279` — `GpuStaticPropRegistry::destroy();`
- The plan's secondary "grep for init" instruction will land the
  executor at `mission.cpp`, which is correct, but the primary hint
  (`gameosmain.cpp`) is misleading. The init/destroy are per-MISSION,
  not per-process, despite Task 6's anon-namespace state assuming
  process scope. This contradicts the plan's stated "process-singleton"
  lifecycle (plan line 562). Either RenderWorld::init/destroy is
  per-mission (matches reality) or per-process (matches comment).
  Reality: per-mission. Fix the doc comment + Task 13 hint.

### m2. `frameBannerTick` insertion point is hand-waved

- Plan Task 14 Step 1 grep'ing for `gos_RendererEndFrame|frameBegin|drawScene`
  in `code/gamecam.cpp`. Then mentions "mirrors the existing
  GpuStaticPropRegistry::frameBegin placement."
- Grep `code/gamecam.cpp:196`: `GpuStaticPropRegistry::frameBegin()` —
  this is at frame BEGIN, not frame END. The plan wants frame-END for
  banner emit ("AFTER the rendering completes for the frame"). Following
  the "mirror frameBegin" hint would place the banner at frame begin
  with last frame's count — off-by-one, not catastrophic but wrong.
- Recommended fix: name the exact symbol (e.g., after `gos_RendererEndFrame`
  in `code/gamecam.cpp`) and re-grep to confirm at execution time.

### m3. Phase A gate claim "no `[RENDER_WORLD v1]` lines yet" contradicts Task 6 code

- Plan Phase A gate (plan line 1088): "No `[RENDER_WORLD v1]` lines yet
  (init not wired)".
- But `RenderWorld::init()` prints `[RENDER_WORLD v1] event=init` (plan
  line 705) and `destroy()` prints `event=destroy` (plan line 709).
  Phase A ends before Task 13 wires the init call — so init() is never
  called, the banner never prints — OK, statement is correct under
  current Phase ordering.
- However, the banner counter-display function (`frameBannerTick`)
  prints `frame=N objects=M ...`, and if any other engine code happens
  to call any RenderWorld function (none should in M1), banner could
  fire. Defensive but low-risk.

### m4. Plan's `[RENDER_WORLD v1] objects=` counter is NOT semantically equal to `[STATIC_PROP_REGISTRY v1]` active count

- Plan Task 14 Step 3 (plan line 1545): "objects=N matches the legacy
  static-prop active count."
- Plan Task 6 implementation (plan line 771-773): `active = s_upsertOk -
  s_destroyCalls`. This counts cumulative-upsert minus cumulative-destroy
  which is the LIVE registration count.
- `[STATIC_PROP_REGISTRY v1]` banner (grep `gos_static_prop_registry.cpp`
  for the actual print) — schema not in scope here but it counts active
  recipes via internal range list, not via upsert/destroy delta.
- Risk: if the registry tombstones a slot for reasons other than an
  adapter-side `destroy(handle)` call (e.g., internal invalidate from
  late-reg recovery), the two counters diverge. Plan Task 16 Step 4 says
  "within +/- 1 for race-free closure" — accepts +/-1 drift implicitly.
  In practice the live invalidate paths in `bdactor.cpp` (`invalidateStaticRegistration`)
  bypass the adapter, so the adapter's destroy counter undercounts
  invalidations. Banner WILL drift over time.
- Recommended fix: either route invalidate through the adapter too
  (expand M1 scope by 1 site — but invalidate isn't in the 5-site
  audit) or compute `objects=` by querying the registry directly
  rather than by adapter-side delta. Low-stakes; document the drift.

### m5. Task 7 adapter's `registerStaticPropLateSpawn` does NOT translate the boundary

- Plan Task 7 Step 2 `registerStaticPropLateSpawn` (plan lines 1002-1013)
  simply forwards to `GpuStaticPropRegistry::registerStaticProp(app)`
  and returns the bool. It does NOT go through `RenderWorld::upsertStaticProp`,
  does NOT increment `[RENDER_WORLD v1]` upsert_ok/fail counters, does
  NOT produce a `RenderObjectHandle`.
- Plan acknowledges this in comments: "handle is NOT consumed by
  warrior.cpp in M1; the bool return preserves the existing site behavior
  bit-for-bit. The handle is computed for `[RENDER_WORLD v1]` counter
  coverage" — but the code does NOT compute the handle. It returns
  early after the bool call.
- Consequence: warrior.cpp:7593 routes through GameAdapters NAMESPACE
  but bypasses RenderWorld. `[RENDER_WORLD v1] objects=` count omits
  late-spawn waypoint markers. This is a half-migration through the
  adapter alias only.
- Recommended fix: after `registerStaticProp(app)` succeeds, the adapter
  should query the registry for the resulting recipe slot (or refactor
  `registerStaticProp` to return both) and call
  `RenderWorld::upsertStaticProp`-equivalent counter bump, OR document
  this as a known Phase 1 gap and surface it in the M1 closing commit.

---

## Strengths confirmed under grep

- All 5 audit call sites confirmed at exact cited line numbers:
  - `mclib/bdactor.cpp:1471` (BldgAppearance first-render fallback)
  - `mclib/bdactor.cpp:2802` (BldgAppearance bulk-register)
  - `mclib/bdactor.cpp:4269` (TreeAppearance first-render fallback)
  - `mclib/bdactor.cpp:4855` (TreeAppearance bulk-register)
  - `code/warrior.cpp:7593` (late-spawn waypoint)
- Exhaustive grep for `GpuStaticPropRegistry::registerRecipe` and
  `registerStaticProp` across the worktree returns exactly these
  5 sites plus one COMMENT reference (`code/objmgr.cpp:1376`); no new
  registration paths have appeared since the 2026-05-22 spec audit.
- Registry signature verified bit-for-bit at
  `GameOS/gameos/gos_static_prop_registry.h:47-48` (`registerRecipe`),
  `:32` (`registerStaticProp`), `:68-69` (`markVisible`), `:74`
  (`invalidate`), `:77` (`isReady`). Plan's forwarder bodies in Task 6
  match these signatures exactly.
- `class Appearance;` forward-decl at `gos_static_prop_registry.h:8`
  confirmed; adversarial-review M2 carve-out justified.
- `class TG_MultiShape` defined at `mclib/msl.h:241`; forward-declared
  at `mclib/tgl.h:562`. The plan's RenderObjectDesc.h forward-decl
  of `TG_MultiShape` is mechanically correct.
- CMake `add_subdirectory` block at `CMakeLists.txt:156-165` matches
  plan claim; `target_link_libraries(mc2 ...)` at line 274 matches
  plan claim. Insertion points are correct.
- Substitutive discipline: Phase B gate (plan line 1410-1424) grep for
  remaining direct callers in `mclib/` and `code/` correctly excludes
  `GameAdapters/`, `RenderWorld/`, `GameOS/`. After Phase B, zero
  legacy callers remain — the slice is genuinely substitutive, not
  additive.
- Full-relink discipline acknowledged in every Phase B task (plan
  lines 1167, 1240, 1287, 1383) and Risk R6 specifically calls out
  `code/warrior.cpp` relink time. CLAUDE.md `Remove-Item mc2.exe` step
  appears in every compile task.
- Greybeard pass is scheduled (Task 17, plan lines 1833-1879) with
  explicit dispatch text and recorded ruling. PATCH (justified) outcome
  with named follow-up deletion criteria matches Spec Section 10.
- Vulkan-prep restatement (plan lines 2132-2148) maps each new type
  to a Vulkan analog; only `Handle::raw()` bit-packing (see M4) is
  shape-incompatible across compilers.

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **C1 resolution.** Does RenderCore tolerate transitive GL+mclib drag
   via `gos_static_prop_batcher.h`, or does `StaticPropDesc` move to
   `RenderWorld/` so RenderCore stays pure? The plan's own Task 2 Step 1
   escalation language says to STOP if game-side header reach is
   detected — but it tests only for `appear.h`/`bdactor.h`/`mclib/*`
   game-side names, missing the GL drag. User must explicitly accept
   the GL drag (then update line 130 to drop "no GL") or pick one of
   the three fixes above.

2. **M1 resolution (D3 adapter surface naming).** D3 named the entry
   points `beginMission/endMission/syncStaticProp/destroyStaticProp`.
   The plan ships `registerStaticPropRecipe/registerStaticPropLateSpawn/
   destroyStaticProp`. Pick one: rename adapter to match D3, or
   user signs off on the substitution and updates D3 retroactively.

3. **M3 resolution (sentinel translation duplication).** Spec says
   "exclusively at the adapter boundary." Plan implements at both
   engine and adapter endpoints. Accept the duplication as M1 reality
   (the engine boundary itself crosses the registry's int32_t API) and
   amend Spec Section 10 wording from "exclusively" to "at every
   game/engine seam, which has two endpoints in M1" — OR drop the
   engine-side translator and accept int32_t in RenderWorld's API.

4. **m5 resolution (warrior late-spawn handle gap).** Should the
   adapter compute a `RenderObjectHandle` for `registerStaticPropLateSpawn`
   even though warrior.cpp doesn't consume it, so `[RENDER_WORLD v1]
   objects=` count is complete? Or document the late-spawn gap as a
   known M1 phase-1 limitation closed in a follow-up slice?

End of review.
