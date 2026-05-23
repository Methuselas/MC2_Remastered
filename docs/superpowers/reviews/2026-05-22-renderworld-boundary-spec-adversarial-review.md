# Adversarial review: RenderWorld boundary spec (2026-05-22)

- Target: `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md` (DRAFT)
- Reviewer pass: code-grounded; every cited symbol re-grepped at write time.
- Mandate: try to BREAK the spec. Findings categorized CRITICAL / MAJOR / MINOR.

## Verdict

CONDITIONAL PROMOTE. The spec is conceptually solid: every symbol it names
in Appendix A exists, the prior-art claims hold, and the firewall sections
are coherent. However, the first-slice scope (Section 13) has a load-bearing
inconsistency between two stated audit predicates ("registerRecipe call
sites" vs "all GpuStaticPropRegistry clients"), Section 12 admits a header
forward-declaration of game-side `Appearance` that the firewall language
forbids, and several class-name drifts (Mech3D vs Mech3DAppearance) will
trip the first writer of the actual code. Fix the listed CRITICAL/MAJOR
items, then promote. No CRITICALs are architectural — they are precision
fixes the spec already wants in spirit.

Counts: 0 CRITICAL, 5 MAJOR, 6 MINOR.

---

## CRITICAL findings

None. The spec is doc-only and does not assert any signature or layout
that grep refutes outright. (The class-name drift below is MAJOR rather
than CRITICAL because it is a rename, not a fictional symbol.)

---

## MAJOR findings

### M1. Section 13 audit predicate is inconsistent with stated scope

- Spec section: 13 "First migration target" + Q13.1 resolution.
- Claim A: "Slice scope is the full set of Appearance subclasses currently
  routed through `GpuStaticPropRegistry`."
- Claim B (audit step): "Audit `GpuStaticPropRegistry::registerRecipe`
  call sites at slice start."
- Grep result for `registerRecipe` (4 hits, all in `mclib/bdactor.cpp`):
  - `bdactor.cpp:1471` BldgAppearance first-render fallback
  - `bdactor.cpp:2802` BldgAppearance bulk-register path
  - `bdactor.cpp:4269` TreeAppearance first-render fallback
  - `bdactor.cpp:4855` TreeAppearance bulk-register path
- Grep result for `GpuStaticPropRegistry::registerStaticProp` (1 hit in
  shipping code outside `bdactor.cpp`):
  - `code/warrior.cpp:7593` late-spawn registration entry point.
    `registerStaticProp` is the dispatcher that ends up calling
    `registerRecipe` through `app->registerStatic()` per the registry
    header doc-comment.
- Why this matters: a slice that audits only `registerRecipe` call sites
  misses `warrior.cpp:7593`, the late-spawn registration ENTRY POINT. If
  the adapter is wired around `registerRecipe` only, warrior.cpp continues
  to call directly into the registry, half-bypassing the new boundary.
- Recommended fix: replace "registerRecipe call sites" with "all
  `GpuStaticPropRegistry::register*` entry points (registerRecipe AND
  registerStaticProp)" in Q13.1 resolution and Section 13 audit step.
  Or explicitly defer `registerStaticProp` to a later slice and document.

### M2. Section 12 firewall vs. existing forward-declare of `Appearance`

- Spec section: 12 forbidden-deps + 10 firewall rule.
- Spec asserts: "RenderWorld and everything below may NOT include any
  gameData header. Violation = boundary failure."
- Grep result for the registry header:
  - `GameOS/gameos/gos_static_prop_registry.h:9` declares `class Appearance;`
    (forward decl) and `:31` exposes `bool registerStaticProp(Appearance* app)`.
  - `Appearance` is defined in `mclib/appear.h` (game-side base; verified
    by class hierarchy: `BldgAppearance : ObjectAppearance : Appearance`
    per `bdactor.h:189`, `bdactor.h:512`, `objectappearance.h:49`).
- Why this matters: `gos_static_prop_registry.h` is exactly the
  backend the spec keeps as "Phase 1 thin forwarder" for the first slice
  (Section 13). It already exposes an API that names a game-side type
  by pointer. Q12.1 leans "yes, any include is a dependency" but the
  registry header presents `Appearance*` in its public signature without
  including a gameData header — a forward declaration is the existing
  mitigation. The spec does not say whether forward-decl of a gameData
  type counts as "depending on" it.
- Recommended fix: Section 12 must explicitly classify this case.
  Either (a) declare forward-decls allowed (matches existing code; aligns
  with the "Phase 1 documentary" posture) and add the `registerStaticProp`
  signature to an explicit grandfathered list, or (b) the registry's
  `Appearance*` API must move into the adapter TU before the firewall
  goes from Phase 1 (documentary) to Phase 2 (debug assertions).

### M3. Class name drift: `Mech3D::destroy` vs `Mech3DAppearance::destroy`

- Spec section: review request mentions `Mech3D::destroy`; spec text
  Section 10 names "MechRenderAdapter takes Mech3D" and Appendix A says
  "mclib/mech3d.h -- `Mech3D` engine-side mech appearance".
- Grep results:
  - `mclib/mech3d.h:299`: `class Mech3DAppearance: public ObjectAppearance`
  - `mclib/mech3d.h:105`: `class Mech3DAppearanceType: public AppearanceType`
  - `mclib/mech3d.cpp:5312`: `void Mech3DAppearance::destroy (void)`
  - Grep for `class Mech3D\b` in mclib/: no match.
- Why this matters: the spec's adapter signatures cite `Mech3D` as the
  type the `MechRenderAdapter` consumes, but the actual class is
  `Mech3DAppearance`. A writer following the spec literally would write
  `void sync(RenderWorld&, const Mech3D&)` and fail to compile. Appendix
  A says "Mech3D engine-side mech appearance" — the symbol is wrong.
- Recommended fix: globally replace `Mech3D` with `Mech3DAppearance` in
  Section 10, Appendix A, and Glossary where the adapter argument type
  is discussed. (`Mech3D` may exist in the source tree as something
  else — `mech3d.cpp` is 5000+ lines — but it is NOT the polymorphic
  appearance type the adapter should consume.)

### M4. Section 12 list in spec EXPANDS the roadmap list; document why

- Spec section: 12 "RenderWorld must not depend on" list.
- Roadmap source (`2026-05-22-engine-convergence-roadmap.md:482-503`)
  lists `BldgAppearance` alone among gameData appearance classes.
- Spec text adds: "`BldgAppearance` / `Appearance` (or any other
  game-side appearance class)."
- Why this matters: the review request explicitly asked for the spec
  Section 12 to match the roadmap "verbatim per advisor Addition 2."
  The spec is STRICTER than roadmap (includes the polymorphic base, not
  just BldgAppearance). That is the right direction architecturally
  (matches the actual class hierarchy where the issue lives), but it
  contradicts the "verbatim" mandate. Either the roadmap should be
  amended in the same merge, or the spec should explicitly call out
  "deliberate expansion vs roadmap" so the next reader does not flag it.
- Recommended fix: add a one-line note at the top of Section 12 reading
  "expanded from advisor Addition 2 to cover the full appearance
  hierarchy; the roadmap's `BldgAppearance` is preserved as the
  exemplar, and `Appearance` is added because the actual derivation
  chain bottoms out there per `mclib/appear.h`." Also add a parallel
  note when the roadmap is next touched.

### M5. Section 7 cites `TacticalVisibilityService` as established precedent

- Spec section: 7 invariant "`version` increments only when admitted
  set changes, enabling consumer skip (e.g., TacticalVisibilityService
  S11 `TacticalVisibilityResult.version` precedent)."
- Grep result for `TacticalVisibilityService` and
  `TacticalVisibilityResult`: 3 hits, all in spec documents
  (`renderworld-boundary-spec`, `engine-convergence-roadmap`,
  `command-readability-zoom-presentation-seed`). Zero hits in C++/h
  source under `mclib/`, `code/`, `GameOS/`, `shaders/`.
- Why this matters: the spec invokes "precedent" for a versioned result
  pattern, but the cited precedent is itself unshipped spec text. The
  pattern may be sound, but it is not a precedent — it is a peer
  proposal. Calling it precedent overstates the design's grounding.
- Recommended fix: re-word to "mirrors the proposed S11
  TacticalVisibilityResult pattern (sibling spec, not yet shipped)" or
  drop the citation and just state the invariant on its own merits.

---

## MINOR findings

### m1. Section 6 sort-key material_id width vs MaterialHandle.index width

- Section 3 declares `MaterialHandle::index : 20`. Section 6 sort-key
  allocates `[31:16] material id (16 bits ...)`. Spec does not say how
  the 20-bit handle index reduces to a 16-bit sort-key field. If the
  intent is "high 16 bits of the index" or "hash of (material, lod)",
  spec should say. As written this is ambiguous; the first reader will
  truncate, which is wrong if material count exceeds 65535.
- Fix: clarify "material id in sort key is a hash/group bucket, not the
  full handle index" OR shorten MaterialHandle.index.

### m2. Section 6 sort-key pipeline_id width vs MeshHandle width

- Same structural issue for `pipeline id (24 bits)`. Section 3
  MeshHandle is a 20-bit index. Pipeline is not the mesh, but the spec
  does not introduce a separate PipelineId type with declared width.
  Section 6 says "PipelineId / PipelineDesc cache key" — what is its
  type? A passing reader assumes uint32_t. Make explicit.

### m3. `recipeIndex` sentinel and `Handle::invalid()` interplay

- Section 3 "Prior art" describes `registerRecipe` returning
  `int32_t recipeIndex` with sentinel -1. Section 3 invariants say
  "`Handle::invalid()` is the only sentinel; do not overload with -1 / 0."
- Phase 1 first-slice path (Section 13) maps `RenderObjectHandle`
  through the existing recipe path, so the adapter must translate
  -1 → `Handle::invalid()` AND the reverse on every boundary
  call. Spec does not state this; it will be a routine source of
  bugs in the first executor session.
- Fix: add a single line to Section 10 (adapter rules) requiring
  sentinel translation at the boundary and forbidding -1 leakage
  upward.

### m4. Section 4 lifecycle "Retired" recycle gating is hand-wavy

- "Retired slots are recycled at frame boundary AFTER any in-flight
  GPU work referencing them has retired." This is correct in spirit
  but does not say how that retirement is signaled. The existing
  `GpuStaticPropBatcher` uses `wasLastFailureLateRegistration()` and
  in-flight batch refcounting; the spec hand-waves it to
  "`RenderDeviceGL` resource manager" without grounding. For Phase 1
  documentary this is fine; before Phase 2 (debug assertions) the
  signaling mechanism must be named.

### m5. Section 8 "missing material" sentinel design (Q8.3) left open

- Spec leans "magenta debug material; never crashes." Per
  `memory/stock_install_must_remain_playable.md` referenced in
  Section 9, missing material at runtime SHOULD NOT happen on stock
  install. Spec should state the stock-compat path returns a real
  legacy GOS material on miss, and magenta is only for cooked-asset
  miss in dev builds.

### m6. Section 11 ObjectID buffer "Tier 1.5 mandatory" but Q11.2 hedges

- Section 11 says "every `RenderObjectHandle` is recoverable from a
  pixel" as a substrate requirement. Q11.2 then asks "is
  'immediately after first slice' acceptable, since the first slice
  is route-only?" These are not compatible: either the substrate is
  mandatory at first-slice merge, or it is not. Pick one and resolve
  Q11.2 before promotion.

---

## Strengths confirmed under grep

- **Section 3 prior art** — `GpuStaticPropRegistry` namespace shape,
  `registerRecipe`/`invalidate`/`isReady`/`markVisible` signatures, and
  the `int32_t recipeIndex` sentinel all match the spec verbatim
  (`gos_static_prop_registry.h` confirms).
- **Appendix A render_contract.h citation** — `enum class PassIdentity :
  std::uint8_t` (line 33) and `struct PassStateContract` (line 73) both
  present; header has no gameData includes.
- **Appendix A LightsData binding** —
  `glGetProgramResourceIndex(shp, GL_SHADER_STORAGE_BLOCK, "LightsData")`
  confirmed at `GameOS/gameos/gameos_graphics.cpp:6907`.
- **Appendix A indirect backends** — `gos_static_prop_batcher.{h,cpp}`,
  `gos_mech_batcher.cpp`, `gos_static_prop_registry.cpp` all present
  under `GameOS/gameos/`.
- **Appendix A Appearance base** — `class Appearance` in `mclib/appear.h`
  confirmed; the polymorphic base used in Section 10 adapter signatures.
  Hierarchy verified: `Appearance` (appear.h) -> `ObjectAppearance`
  (objectappearance.h:49) -> `BldgAppearance` / `TreeAppearance` /
  `GVAppearance` / `Mech3DAppearance` / `GenericAppearance`.
- **First-slice scope is genuinely small.** Only TWO Appearance
  subclasses actually register recipes today (Bldg + Tree). The adapter
  surface is bounded; "all GpuStaticPropRegistry clients" is not an
  open-ended phrase.
- **Section 3 handle bit split (20/12) is empirically comfortable.**
  Object count: `ObjectManager` carries a runtime `maxObjects` (typically
  hundreds, not thousands; mission-load reservation in `mission.cpp:2809`
  is "getMaxObjects() + 25%"). Mech destroy/respawn churn under sustained
  combat is bounded by mission duration and concurrent mech count; 4096
  generation reuses per slot has multi-order-of-magnitude headroom.
  Decals/VFX explicitly excluded from the same handle table per Section
  4 Q4.4 lean.
- **Section 12 firewall is real on the RenderCore side.**
  `mclib/render_contract.h` currently includes only `<cstdint>`. The
  type infrastructure that the spec wants on the RenderCore side already
  ships clean. The boundary failure surface is the storage backends
  (registry/batcher headers), not the contract types.
- **F1 view-convention statement is current.** Section 5's invariant
  ("`viewToClipGL` in the unified GL convention from F1") matches the
  active campaign documented in MEMORY.md handoff `HANDOFF_2026_05_22_
  unified_projection_F1_ready_for_execution.md` (Phase 0+1 complete,
  Phase 2 next).

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **M2 resolution (forward-decl exception).** Does the firewall in
   Section 12 admit forward-declarations of game-side types? If yes,
   document the carve-out. If no, the existing `registerStaticProp`
   API on `gos_static_prop_registry.h` must move into a new adapter TU
   in the first slice, expanding Section 13's slice scope.
2. **M1 resolution (audit predicate).** Is `registerStaticProp` in
   first-slice scope, or deferred? Affects whether `code/warrior.cpp:7593`
   is migrated in slice M1 or in a follow-up slice.
3. **M4 resolution (roadmap vs spec list).** Should the roadmap be
   amended to match the spec's expanded forbidden-deps list, or should
   the spec narrow back to the roadmap's `BldgAppearance` only?
4. **m6 resolution (Q11.2).** Object-ID buffer at first-slice merge, or
   parallel slice? Pick one and lock the spec.

End of review.
