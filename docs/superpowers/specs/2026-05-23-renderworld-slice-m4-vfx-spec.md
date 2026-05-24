# RenderWorld Slice M4 — VFX Adapter (Deferral + Prohibition)

Date: 2026-05-23
Author: spec author (Opus 4.7 1M)
Status: DRAFT — deferral-shape + prohibition; 6 open Qs for user review
Sources: `docs/superpowers/explorations/2026-05-23-renderworld-slice-m4-vfx-recon.md` (primary), `docs/renderworld_migration_guide.md`, `RenderWorld/RenderWorld.h`, `RenderCore/Handle.h`

---

## 1. Header

**Goal.** Formalize the `RenderObjectKind::Vfx = 3` reservation already noted at
`RenderWorld/RenderWorld.h:134` and ENCODE a permanent prohibition: VFX shaders
MUST NOT write `R32_UINT` color-attachment-2 (the M1.5 ObjectID substrate).
Allocate a handle-index base for future use, but issue no handles in v1.

**Architecture.** Deferral + prohibition + firewall. M4 adds zero GPU writes,
zero engine adapters, zero per-frame work. It adds: (a) an enum value, (b) a
documented handle-base constant, (c) a CI grep gate that fails the build if
any VFX shader declares `layout(location=2) out`, (d) a migration-guide note
explaining the additive-blend last-write-wins trap that motivates the
prohibition.

**Tech stack.** GLSL 4.3+, GL 4.5 (additive-blend semantics per §17.3.6),
existing `RenderWorld/`, `RenderCore/Handle.h`, `scripts/check-*.sh` firewall
family.

**Relationship to prior slices.** Smaller than every prior RenderWorld slice.
M1, M2 added adapters and routing. M1.5, M2.5 added substrate writes. M1.6,
M2.6 added pick consumers. M4 adds NONE of these. It is a slice that retires
a future bug class before any contributor has the chance to land it.

---

## 2. Purpose / Non-Goals

### Purpose

1. Formalize `RenderObjectKind::Vfx = 3` (currently only a comment at
   `RenderWorld.h:134`).
2. Allocate `kVfxHandleBase = 0x80000` in `RenderWorld.cpp` alongside
   `kMechHandleBase = 0x10000` — documented, unused.
3. Ship a firewall gate (`scripts/check-vfx-no-objectid.sh`) that fails CI
   if any VFX shader (currently `shaders/particle_billboard.{vert,frag}` and
   any future addition) declares `layout(location=2) out`.
4. Document the additive-blend trap in the migration guide so future
   contributors understand WHY the prohibition exists.
5. Encode the corrected handle-base partitioning (Section 8) so future
   slices do not repeat the `0x200000` overflow trap that a prior draft hit.

### Non-Goals (explicit)

- **M4 does NOT add GPU writes.** No `objectIdRaw` field on any particle
  SSBO. No `layout(location=2) out` declaration in any VFX shader. The
  prohibition is enforced mechanically, not aspirationally.
- **M4 does NOT add an adapter.** No `GameAdapters/VfxRenderAdapter.{h,cpp}`.
  No `RenderWorld::registerEffect / destroyEffect` API.
- **M4 does NOT add a pick consumer.** `lookupAtPixel(x, y)` will continue
  to return the OBJECT BEHIND the VFX (mech, static prop, terrain), which is
  the correct gameplay semantic (the user's mental model of "click on the
  explosion" is "click on what the explosion is decorating").
- **M4 does NOT fix the gosFX dev-override.** That is blocked by MLR
  retirement Slices 1-5 (CLAUDE.md known issue at `:159-167`). M4 ships
  independently of it.
- **M4 does NOT flip any default.** `MC2_GPU_PARTICLES` stays default-off;
  `MC2_DISABLE_GOSFX` stays default-on (gate-disabled). M4 has no
  concrete consumer in default config — see §6.

### What this means in practice

M4 is the smallest meaningful slice in the arc. The deliverable is one enum
value, one constant, one short shell script, and one paragraph in the
migration guide. Tier1 pixel-parity is trivial (no draw state changes). The
greybeard ruling argument (§11) is that this is a META-FIX for a bug class
that has not yet shipped.

---

## 3. Relationship to M2.6 + the 5-Questions Template

Per `docs/renderworld_migration_guide.md:209-222`, every new `RenderObjectKind`
must answer five questions verbatim. Answers for VFX:

### Q1: What creates/destroys the handle?

**NOTHING in v1.** No `registerEffect`, no `registerEmitter`, no
`registerParticle`. The handle-base constant is allocated and documented but
never used.

Per-particle handle allocation is REJECTED outright (recon §4): thousands of
particles per second, lifetimes 0.5-5s, would dwarf static-prop + mech
allocation rates by orders of magnitude.

Per-emitter (gosFX::Effect) handle allocation is DEFERRED to a future slice
if-and-only-if a debug visualizer use case materializes (recon §9 option A,
deferred). The recon's recommended answer is "never" — see Q3 below.

### Q2: What kind does it report?

`RenderObjectKind::Vfx = 3`. This kind is RESERVED only. Because no slice
writes attachment-2 with a VFX handle, `lookupAtPixel` will NEVER return
`kind == Vfx` in v1. The kind value exists so that IF a future slice opts in
(against current recon recommendation), there is a stable enum value to
discriminate on, and the firewall grep gate has a symbol to defend.

### Q3: Does it write object ID?

**NO. PROHIBITED.** This is the load-bearing answer of M4.

VFX shaders MUST NOT contain `layout(location=2) out uint v_objectId`
declarations. The prohibition is enforced by
`scripts/check-vfx-no-objectid.sh` (§9). The trap that motivates this is
described in detail in §4.

Currently the prohibition is already satisfied (recon §1, §2.1 row 7,
§5 option 1): `shaders/particle_billboard.frag:17-27` writes ONLY
`outColor`. M4 makes the satisfied invariant LOAD-BEARING via the firewall
gate.

### Q4: How does lookup/pick/debug consume it?

**NOT YET — and the no-write design choice ensures the correct semantic by
construction.** Because particle fragments do not write attachment-2, the
last opaque draw underneath wins (mech, static prop, terrain, or background
sentinel 0). A Shift+click on a mech that is occluded by a translucent
muzzle flash STILL picks the mech because attachment-2 holds the mech's
objectID — the particle did not clobber it.

If a future debug visualizer wants "what effects are alive," the
recommended path is a `RenderWorld::getVfxAliveCount()` accessor (analog of
`RenderWorld::getMechsAliveCount()` at `RenderWorld.h:308`) + a separate
`[VFX_DEBUG v1]` log channel — NOT GPU substrate writes.

### Q5: What legacy fallback remains?

**N/A.** No rendering path currently consumes IDs from VFX. The legacy
gosFX `RenderNow()` path (`code/gamecam.cpp:309`) submits via the
GOS-vertex shader stable (`gos_vertex.{vert,frag}`, `gos_tex_vertex*`); none
of those FS write `layout(location=2)`. The forward batcher path
(`gos_particle_bridge.cpp:119-205`) also does not. There is no legacy
substrate to fall back from, because there is no substrate.

---

## 4. The Additive-Blending Trap — Explicit Explanation

This is the load-bearing prohibition. Future contributors who do not
understand WHY VFX cannot write attachment-2 will either delete the firewall
gate or wire a write anyway. This section is the durable explanation.

### GL spec ground rule

GL 4.5 §17.3.6 ("Blending"): blend equations are defined ONLY for fixed-point
and floating-point color buffers. For an **integer** color buffer (the M1.5
`R32_UINT` attachment-2), blending is silently treated as `GL_FUNC_ADD` with
`GL_ONE, GL_ZERO` — i.e. **last write wins per fragment**, regardless of
`glBlendFunc` state set for attachment-0.

This means a particle fragment that writes attachment-2 OVERWRITES whatever
the depth-passing opaque draw underneath wrote, even when the particle's
visible color contribution is near-zero.

### Concrete failure mode

If a contributor adds `layout(location=2) out uint v_objectId` to a particle
fragment shader, the following occurs:

1. Mech M renders to (color-0, depth, objectID-2). Fragment writes
   `(color_mech, z_mech, handle_mech.raw())`.
2. Particle P (muzzle flash, alpha=0.05) renders in front of M with
   `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`. Attachment-0 blend:
   `0.95 * color_mech + 0.05 * color_P` — visually still mech.
   Attachment-2 (no blend): `handle_P.raw()` — **overwritten**.
3. User Shift+clicks on the mech. `lookupAtPixel(x, y)` returns
   `handle_P` (or invalid if particle has no kind). M2.6 mech-pick
   **FAILS**.

Particle billboards use `GL_DEPTH_TEST=on`, `GL_GEQUAL`, `glDepthMask=FALSE`
(`gos_particle_bridge.cpp:184-186`). Particles pass the depth test in front
of mechs but do NOT write depth. Attachment-2 writes occur per passing
fragment, regardless of alpha contribution.

### The visible symptom

M2.6 mech-pick silently breaks on any mech that has a visible tracer,
muzzle flash, smoke trail, or impact effect in front of it. The user clicks
on the mech; nothing happens. No log line marks the failure (the lookup
returns a stale-but-alive particle handle, not an invalid result). This is a
class of bug that is hard to trace from the gameplay symptom back to the
particle FS shader source.

### The cure: prohibition + grep gate

VFX shaders MUST NOT contain `layout(location=2) out` declarations. The
prohibition is mechanical, not aspirational:
`scripts/check-vfx-no-objectid.sh` greps the VFX shader set and FAILS if any
match is found. CI runs the script pre-merge; a violation cannot ship.

### Why not "particles write 0 / sentinel"?

The recon's §5 option 2 (particles explicitly write `0u` to attachment-2)
was considered. Verdict: REJECTED. Same overwrite mechanic; only the
overwrite VALUE changes. The mech's `handle.raw()` still gets clobbered by
`0u`, just as cleanly as by `handle_P.raw()`. The lookup returns
`Handle::invalid()` (background sentinel) and M2.6 mech-pick still fails.
Option 1 (no write at all) is strictly safer than option 2.

---

## 5. Architecture

### The reservation pattern

`RenderObjectKind::Vfx = 3` is added as an enum value in
`RenderWorld/RenderWorld.h`. No `s_objectRecords` slot is ever marked
`kind = Vfx` because no `registerEffect` / `destroyEffect` API exists. The
enum value is a defensive declaration: it gives the firewall grep gate a
symbol to defend, and it reserves the value in the stable enum order so a
future slice that DOES introduce a writer cannot accidentally collide with
`Terrain = 2` or `Overlay = 4`.

### The handle-base allocation

`kVfxHandleBase = 0x80000` is declared in `RenderWorld.cpp` alongside the
existing `kMechHandleBase = 0x10000`. It is documented but unused. Future
slices (if any) that opt-in to per-emitter handles MUST use this base.

Critically, the corrected partitioning (Section 8) is documented in the
same header comment block so the next contributor cannot re-introduce the
`0x200000` overflow trap.

### The firewall gate

`scripts/check-vfx-no-objectid.sh` greps the VFX shader set for
`layout(location=2) out` declarations. The script lives alongside the other
firewall scripts in `scripts/check-*.sh` (see `check-include-firewall.sh`,
`check-mlr-leaves-gated.sh`, `check-particles-no-cpu-projection.sh` for the
existing family). It is run pre-commit when any file in `shaders/` matching
the VFX-shader-list pattern changes, and pre-merge in CI.

### The migration-guide note

`docs/renderworld_migration_guide.md` is extended with a §17 ("VFX
prohibition") that links to this spec, documents the additive-blend trap,
names the firewall script, and tells future contributors NOT to add
attachment-2 writes to VFX shaders.

---

## 6. Surfaces (Grep-Verified)

The VFX shader set is currently exactly two files. Both writers in the
forward path and the legacy path satisfy the prohibition at write time.

| Shader file | Path | Writes attachment-0? | Writes attachment-2? | Status |
|---|---|---|---|---|
| `particle_billboard.vert` | `shaders/particle_billboard.vert:24-85` | n/a | n/a | clean (vert) |
| `particle_billboard.frag` | `shaders/particle_billboard.frag:17-27` | YES (`outColor`) | NO | satisfies prohibition |

**Adjacent stable** (not VFX per recon §2.3 but worth noting in firewall):

| Shader pair | Usage | Attachment-2 status |
|---|---|---|
| `gos_vertex.{vert,frag}` | legacy GOS submission path; consumed by gosFX `RenderNow()` chain when dev-override enabled | does NOT write attachment-2 (recon §2.2) |
| `gos_tex_vertex.{vert,frag}` | same; textured variant | does NOT write attachment-2 |
| `gos_tex_vertex_lighted.{vert,frag}` | same; textured + lit | does NOT write attachment-2 |

The firewall script SHOULD include the three GOS-vertex variants in its
scan set, because the gosFX legacy path emits through them and a
contributor who adds attachment-2 writes there would also trigger the
trap.

**Out of scope for the firewall** (recon §2.3): `code/weather.cpp` rain
drops; debug-renderer primitives. These are environmental overlays, not VFX
in the spawn-event sense. They are not in M4 scope.

---

## 7. The 6 Architectural Questions for User

These are the open decisions M4 cannot ship without. Q1 is load-bearing
(the prohibition is the entire spec). The others shape scope.

### Q1 — Confirm the no-attachment-2-write stance

**Recon recommendation:** YES, prohibit. The additive-blend trap (§4)
actively breaks M2.6 mech-pick on any mech occluded by a translucent
particle. No counterargument has been proposed.

**If user says NO:** the spec must be substantially rewritten. A "yes,
write" stance requires (a) per-emitter handle issuance (recon §4 verdict
"DEFENSIBLE for debug-only; OVERKILL for stock gameplay"), (b) an
alpha-test discard threshold in the particle FS so only high-coverage
fragments write, (c) a documented contract for how M2.6 mech-pick degrades
under translucent effects.

**Load-bearing.** Plan cannot proceed without Q1 answer.

### Q2 — Future-proofing scaffold vs wait-for-gate-flip

Ship the enum + firewall gate now (scaffold) — OR — wait until one of
`MC2_GPU_PARTICLES` / `mlr_gate` defaults flips to ON (deferred)?

**Recon leans scaffold.** Reasoning: the firewall gate has zero runtime
cost and protects against a class of bug that is currently latent
(`particle_billboard.frag` clean today, but easy to break). Shipping the
gate now means the FIRST contributor to add a particle FS objectID write
hits a CI failure rather than a player-visible regression months later.

**If user says wait:** M4 reduces to (a) update the recon to flag the
deferral, (b) add a TODO line in `RenderWorld.h` next to the existing
`Vfx=3` reservation comment, (c) close the spec without execution.

### Q3 — If per-emitter identity ever wanted, what's the use case?

The recon (§4) finds none in stock gameplay. The defensible use case is
"editor / debug visualizer to inspect what effects are alive." Even that is
arguably better served by `getVfxAliveCount()` + a `[VFX_DEBUG v1]` log
channel than by per-emitter GPU substrate.

**Recon leans NONE.** If user identifies a use case, document it inline so a
future slice has the requirements written down.

### Q4 — Per-source-game-object lookup

"Which mech fired this explosion?" Should that be a VFX-substrate concern,
or game-logic correlation at higher levels?

**Recon leans game-logic.** The source mech already has a M2 handle. A
weapon-fire event carries (source-mech-handle, target-position) in CPU game
state. There is no need to ask the GPU "which mech is this particle's
source" — the CPU already knows. Adding GPU substrate for this would be
redundant with M2.

**If user says GPU:** spec must be widened to per-emitter handles, which
re-opens Q3.

### Q5 — gosFX dev-override status

CLAUDE.md known issue at `:159-167` documents that `MC2_DISABLE_GOSFX=0`
under unified-projection F1 renders gosFX wrong (stale MLR
`cameraToClip(2,2)/(3,2)` convention). Should M4 spec acknowledge this as a
hard blocker for any future VFX-write substrate?

**Recon leans YES, acknowledge.** Even if Q1 flips to "yes write" in some
future world, the substrate cannot be exercised under the dev-override
until MLR retirement Slices 1-5 ship and the convention is corrected. M4
spec should explicitly link to this dependency so a future "let's add VFX
objectID writes" slice author knows about the prerequisite.

### Q6 — Handle range allocation

Encode the corrected partitioning (§8) now even if no writes ever ship?

**Recon leans YES, encode.** The original draft proposal `0x200000` was
out-of-range (overflow at 20-bit mask). Encoding the corrected partitioning
in a documented constant prevents the next contributor from re-deriving the
allocation badly. The constant costs nothing at runtime; the documentation
saves a future debugging session.

---

## 8. API Extensions (the minimum surface)

Exactly two additions. No new API functions.

### Addition 1: enum value

In `RenderWorld/RenderWorld.h:131-135`, replace the trailing comment with a
formal enum member:

```cpp
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    Vfx        = 3,   // M4: RESERVED. No writer ships in v1; see
                      // docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md
    // Future: Terrain=2, Overlay=4
};
```

Note: leaving `Terrain = 2` and `Overlay = 4` as comments preserves the
stable-numbering invariant. `Vfx` formalizes at its reserved value.

### Addition 2: handle-base constant

In `RenderWorld/RenderWorld.cpp` alongside `kMechHandleBase`:

```cpp
// M2: mech handle base. Disjoint from static props (which start at 0)
// so a stale handle's index alone reveals its intended kind.
static constexpr uint32_t kMechHandleBase = 0x10000;

// M4: VFX handle base. RESERVED. No writer issues handles in v1
// (per spec 2026-05-23-renderworld-slice-m4-vfx-spec.md §7 Q1 + Q6).
// If a future slice introduces per-emitter handles, use this base.
//
// Full corrected partitioning (do NOT re-derive; see spec §8):
//   StaticProp: 0x00000..0x0FFFF   (max observed mc2_24 = 2641)
//   Mech:       0x10000..0x3FFFF   (max observed mc2_24 = 46)
//   Terrain:    0x40000..0x7FFFF   (worst-case ~196k visible quads if M3 writes)
//   Vfx:        0x80000..0xBFFFF   (reserved; recon recommends NO writes)
//   Overlay:    0xC0000..0xFFFFE   (reserved pending M5 clarification)
//   Sentinel:   0xFFFFF            (avoid; common bug-bait value)
//
// The 20-bit index mask (RenderCore/Handle.h:34) limits the absolute
// max to 0xFFFFF (1,048,575). A prior draft proposed 0x200000 — that
// OVERFLOWS the mask and would silently truncate to index=0, colliding
// with static-prop slot 0. Do not repeat that trap.
static constexpr uint32_t kVfxHandleBase = 0x80000;
```

---

## 9. Firewall Gate

### New script: `scripts/check-vfx-no-objectid.sh`

Greps the VFX shader set for `layout(location=2) out` declarations. Fails
non-zero if any match is found.

Scan set (literal file globs; matches the surface table in §6):

```
shaders/particle_billboard.vert
shaders/particle_billboard.frag
shaders/gos_vertex.vert
shaders/gos_vertex.frag
shaders/gos_tex_vertex.vert
shaders/gos_tex_vertex.frag
shaders/gos_tex_vertex_lighted.vert
shaders/gos_tex_vertex_lighted.frag
```

Pattern: `layout\s*\(\s*location\s*=\s*2\s*\)\s+out`

Behavior:
- Exit 0 if zero matches across scan set.
- Exit 1 with diagnostic listing offending file:line if any match.
- Diagnostic includes the explanation "VFX/legacy-GOS shaders MUST NOT
  write attachment-2 due to integer-buffer last-write-wins trap; see
  docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md §4."

Comment-aware: matches must be in active code (single-line `//` comments
stripped before matching; mirror the comment-stripping in
`check-include-firewall.sh`).

### Pre-commit + CI integration

- Pre-commit: list `check-vfx-no-objectid.sh` in the project pre-commit
  hook docs alongside `check-include-firewall.sh`.
- CI: invoke pre-merge in the same job that runs the existing firewall
  scripts.

### Migration-guide note

Append §17 to `docs/renderworld_migration_guide.md`:

```
## 17. VFX prohibition (M4)

VFX shaders are PROHIBITED from writing color-attachment-2 (the M1.5
R32_UINT ObjectID substrate). The prohibition exists because GL 4.5
§17.3.6 specifies that blending is silently disabled on integer color
buffers — additive/alpha-blended particles would last-write-wins clobber
any mech/static-prop ID underneath, silently breaking M2.6 mech-pick.

Enforced mechanically by `scripts/check-vfx-no-objectid.sh`. Do NOT
disable the script; do NOT add `layout(location=2) out` to any shader
in its scan set. If you have a genuine use case for VFX-substrate
writes (Q3 of the M4 spec said no), reopen the spec at
`docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md`
and address Q1-Q5 before touching the firewall.
```

---

## 10. Validation Strategy (small)

M4 has zero runtime behavior changes. Validation is:

1. **Build clean.** RelWithDebInfo build succeeds; `Vfx = 3` enum compiles;
   `kVfxHandleBase` constant compiles.
2. **Tier1 5/5 PASS env-OFF.** Pixel-parity vs the pre-M4 parent commit.
   Trivial: no draw state changed.
3. **Tier1 5/5 PASS env-ON `MC2_OBJECT_ID_BUFFER=1`.** Substrate active;
   M2.6 mech-pick still works; VFX path still no-write. Implicitly proves
   the prohibition is intact (mech-pick would degrade if particles started
   writing attachment-2).
4. **Existing firewall clean.** `scripts/check-include-firewall.sh` PASS
   (M4 touches no headers in SCOPE_DIRS).
5. **NEW: VFX-no-write grep gate PASSES.**
   `scripts/check-vfx-no-objectid.sh` exit 0 (zero matches across scan
   set). Acts as the negative-fixture validator: prove the gate works by
   running it on the current clean tree and seeing PASS.
6. **NEW: positive-fixture test.** Temporarily insert
   `layout(location=2) out uint v_objectId` into a copy of
   `particle_billboard.frag` (e.g. write to `/tmp/test.frag` and pass to
   the script via env override, or include a self-test mode in the
   script). Verify the script exits non-zero. Delete the fixture.

No new self-test in `RenderWorld::init()` is needed — there is nothing
runtime-substrate to validate.

---

## 11. Greybeard Analysis

Per `.claude/skills/greybeard.md`: every fix carries a META-FIX vs PATCH
(justified) ruling.

### Ruling

**META-FIX.** This slice retires the bug class
"future contributor adds VFX objectID write, silently breaks M2.6
mech-pick under particles" before it can occur.

### Why META-FIX (not PATCH)

A PATCH would be "wait until a contributor adds the write, then revert
their change and educate them." A META-FIX retires the class:
- The grep gate makes the prohibition MECHANICAL. A contributor who tries
  to add the write hits CI failure immediately, not three months later
  when a player reports mech-pick is flaky on certain missions.
- The migration-guide note documents WHY, so the contributor learning the
  prohibition is in CI failure does not have to re-derive the
  additive-blend trap from first principles.
- The handle-base allocation pre-emptively blocks the re-derivation of
  `0x200000` (the overflow trap a prior draft hit). Future slices have
  the corrected partitioning written down.

### Substitutive proof (the bug-class retirement hinge)

Without M4, the bug class "VFX writes objectID, clobbers underlay" exists
in a latent state — no shader does it today, but no mechanism prevents one
from doing it tomorrow. With M4, the bug class is closed: `grep
'layout(location=2) out'` across the VFX scan set returns zero, AND any
future commit that would re-introduce a match fails CI. The class is
provably empty AND defended.

### Anti-pattern documented with cure

The additive-blend trap is documented (§4) WITH its mechanical cure (the
grep gate, §9). Future debugging: a contributor seeing "M2.6 mech-pick
fails on certain mechs" can grep the codebase for `check-vfx-no-objectid`
and land on the spec that explains the trap. This is the "anti-pattern +
cure documented at the same level" discipline that retires bug classes
rather than chasing instances.

---

## 12. Threat Model

Three traps identified; each has a named mitigation.

### Trap 1: Additive-blend last-write-wins clobber (the central trap)

**Mechanism:** Particle fragment writes `layout(location=2) out uint
v_objectId` → integer color attachment does not blend → opaque mech's
objectID is overwritten by particle's even when alpha is 0.05 → M2.6
mech-pick silently fails on occluded mechs.

**Mitigation:** Q1 confirms prohibition; `check-vfx-no-objectid.sh`
enforces mechanically.

**Residual risk:** A contributor who explicitly disables the grep gate.
Mitigated by the migration-guide note linking back to this spec — disabling
the gate requires acknowledging the documented trap.

### Trap 2: Handle-index overflow

**Mechanism:** Handle index is 20 bits (`RenderCore/Handle.h:34` mask
`0xFFFFFu`). A draft proposing `kVfxHandleBase = 0x200000` (= 2,097,152)
overflows → silent truncation to index=0 → collision with static-prop
slot 0 → wrong-kind lookup, wrong-object pick.

**Mitigation:** Q6 confirms the corrected partitioning is encoded in the
constant comment block (§8). Future slices that allocate Terrain/Overlay
bases have the chart to consult.

**Residual risk:** A contributor who adds a new kind and picks `0x100000`
or higher without consulting the chart. Mitigated by documentation
proximity — the chart lives inline next to `kVfxHandleBase` in
`RenderWorld.cpp`, not buried in a separate doc.

### Trap 3: Firewall bypass (a future VFX-write substrate accidentally bypassed)

**Mechanism:** A contributor adds a new VFX shader file (e.g.
`shaders/explosion_billboard.frag`) without adding it to the
`check-vfx-no-objectid.sh` scan set. Writes to attachment-2 sneak in.

**Mitigation:** The grep gate is the canary, but its scan set is
hand-maintained. Acceptable for M4 because the VFX shader set is small
(two files in active forward path + three legacy GOS variants). If the set
grows, the script should pivot to a directory-wide glob with an
explicit-exclude list rather than an explicit-include list.

**Residual risk:** scan-set drift. Mitigated by a comment in the script
header: "Update SCAN_SET when adding a new VFX shader file. See M4 spec
§9." Reviewers should flag any new shader file in `shaders/` that touches
particles/effects.

---

## 13. Resolved Decisions Table

All six questions are OPEN — none are resolved. Plan cannot be written
until Q1 is answered (load-bearing).

| ID | Decision | Status | Recon lean |
|----|----------|--------|-------------|
| Q1 | Prohibit VFX attachment-2 writes? | OPEN — for user review | YES, prohibit |
| Q2 | Ship scaffold now vs wait for gate flip? | OPEN — for user review | Scaffold now |
| Q3 | Per-emitter identity use case? | OPEN — for user review | None |
| Q4 | Per-source-game-object lookup substrate vs game-logic? | OPEN — for user review | Game-logic |
| Q5 | Acknowledge gosFX dev-override blocker? | OPEN — for user review | YES, acknowledge |
| Q6 | Encode corrected handle-base partitioning? | OPEN — for user review | YES, encode |

---

## 14. Open Questions for Human

Listed verbatim for morning skim:

**Q1 — Confirm no-attachment-2-write stance for VFX.** Recon strongly
recommends YES (prohibit). The integer-attachment last-write-wins trap
silently breaks M2.6 mech-pick on any mech occluded by a translucent
particle. A "no, allow writes" answer requires substantial spec rework
(alpha-test discard policy, per-emitter handle scheme, degraded mech-pick
contract). **This question is load-bearing — plan cannot proceed without
the answer.**

**Q2 — Future-proofing scaffold vs wait-for-gate-flip.** Ship the enum +
firewall gate now (scaffold), or wait until `MC2_GPU_PARTICLES` or
`mlr_gate` defaults flip to ON? Recon leans scaffold — the gate has zero
runtime cost and protects against a latent bug class that has not yet
shipped.

**Q3 — If per-emitter identity ever wanted, what's the use case?** Recon's
"none" option remains strongest. A defensible answer (e.g. "editor
visualizer to highlight long-lived effects") would shape Q4 and any future
VFX-write substrate spec. If the answer is "no use case identified," that
should be recorded so a future slice does not re-litigate.

**Q4 — Per-source-game-object lookup.** "Which mech fired this explosion?"
— defer to higher-level game-logic correlation (CPU already knows the
source-mech-handle), NOT GPU substrate? Recon leans game-logic; GPU
substrate would be redundant with M2.

**Q5 — gosFX dev-override status.** CLAUDE.md `:159-167` documents that
`MC2_DISABLE_GOSFX=0` under unified-projection F1 renders gosFX wrong
(stale MLR matrix convention). Should M4 spec acknowledge this as a hard
blocker for any future VFX-write substrate (i.e. even if Q1 ever flips,
the substrate cannot be exercised until MLR Slices 1-5 ship)? Recon leans
YES, acknowledge.

**Q6 — Handle range allocation.** Encode the corrected partitioning
(StaticProp 0..0xFFFF / Mech 0x10000..0x3FFFF / Terrain 0x40000..0x7FFFF /
Vfx 0x80000..0xBFFFF / Overlay 0xC0000..0xFFFFE / sentinel 0xFFFFF) in the
constant comment block now even if no writes ever ship? Recon leans YES,
encode — saves a future debugging session and prevents the `0x200000`
overflow trap from being re-derived.

---

SPEC STATUS: DRAFT — deferral-shape + prohibition; 6 open Qs for user review
