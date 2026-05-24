# RenderWorld Slice M4 — VFX Prohibition + Scaffold — Plan

**Date:** 2026-05-24
**Author:** plan author (Opus 4.7 1M)
**Branch:** `claude/nifty-mendeleev`
**Parent commit (verified at write time):** `16a461b`
**Status:** READY FOR EXECUTE
**Spec:** `docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md`
**Resolutions sidecar:** `docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md` (Q1-Q6 answered)

---

## Pre-flight (READ FIRST)

This plan is intentionally **TIGHT**. M4 ships:

- 1 enum value (`RenderObjectKind::Vfx = 3`)
- 1 constant (`kVfxHandleBase = 0x00080000u`)
- 1 new firewall script (`scripts/check-vfx-no-objectid.sh`)
- 1 new allowlist (`scripts/check-vfx-no-objectid.allowlist` — expected empty forever)
- 2 documentation edits (migration guide §3.6 + §12 chart update; CLAUDE.md SHIPPED entry)

**NO:** adapter, registerEffect/destroyEffect API, per-emitter handles,
`objectIdRaw` fields on any particle SSBO, runtime gates, env vars,
shader writes. Slice resolutions Q1-Q6 are locked in the sidecar; do
not re-litigate.

**Greybeard ruling (per spec §11):** META-FIX. Retires the bug class
"future contributor adds VFX attachment-2 write, silently breaks M2.6
mech-pick under particles" before it can occur. Substitutive proof:
firewall grep returns 0 hits across VFX scan set today AND any future
commit that would re-introduce a match fails CI.

**Order-of-operations note (sidecar §"Order of operations"):** M3+M5 atomic
commit ships BEFORE M4. At plan-write time (`16a461b`), M3 has not yet
landed — `kTerrainHandleBase` is absent from `RenderWorld/RenderWorld.cpp`
and `RenderObjectKind::Terrain = 2` is absent from `RenderWorld/RenderWorld.h:132-136`
(grep-verified — current enum reads `StaticProp=0, Mech=1, // Future: Terrain=2, Vfx=3, Overlay=4`).
The plan accommodates both cases (see Step 2 and Step 3 below) so M4
can ship cleanly whether M3 has shipped first or not.

---

## Lean intake checklist (completed at plan-write time)

- [x] Read sidecar primary: `docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md` (113 lines)
- [x] Read M4 spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md` (667 lines)
- [x] Read M6 firewall script for style: `scripts/check-no-raw-gl-from-game.sh` (132 lines)
- [x] Verified `RenderWorld/RenderWorld.h:131-136` — `RenderObjectKind` enum (StaticProp=0, Mech=1, comment "Future: Terrain=2, Vfx=3, Overlay=4")
- [x] Verified `RenderWorld/RenderWorld.cpp:117-121` — `kMechHandleBase = 0x00010000u` (line 121 exactly)
- [x] Verified `shaders/particle_billboard.frag:17-27` clean (writes only `outColor` at line 22; NO `layout(location=2) out`)
- [x] Verified `shaders/` VFX file census via glob: ONLY `particle_billboard.{vert,frag}` (no `*fx*`, `*effect*`, `*smoke*`, `*spark*`, `*explos*`, `*tracer*`, `*flame*`, `*impact*`, `*muzzle*` matches)
- [x] Verified `mech.frag:47` and `static_prop.frag:71` are the ONLY `layout(location=2) out` declarations in shader tree (allowed — these are M1.5/M2.5 substrate writers)
- [x] Verified `docs/renderworld_migration_guide.md` has §3.5 (raw-GL prohibition, lines 170-195) and §12 (handle-base chart, lines 541-563) — M4 inserts §3.6 between them and rewrites §12 chart
- [x] Verified `scripts/check-no-raw-gl-from-game.allowlist` shape (header comment + path-per-line + `#` comments)
- [x] Verified parent branch `claude/nifty-mendeleev` HEAD = `16a461b`

## Analysis-paralysis guard

5+ consecutive Read/Grep/Glob without Edit/Write/Bash = STOP. Either
write or report blocked. (Plan-write phase already completed intake;
execution should not need more than minimal grep to confirm file:line
stability immediately before each Edit.)

---

## Task 1 — VFX prohibition + scaffold (SINGLE COMMIT)

This is a single atomic commit comprising 16 numbered steps.

### Files modified (4)

| Path | Change |
|---|---|
| `RenderWorld/RenderWorld.h` | Add `Vfx = 3` enum value |
| `RenderWorld/RenderWorld.cpp` | Add `kVfxHandleBase = 0x00080000u` constant + comment block with corrected partitioning |
| `docs/renderworld_migration_guide.md` | Add §3.6 prohibition note; rewrite §12 handle-base chart |
| `CLAUDE.md` (worktree) | Add M4 SHIPPED entry under Active campaigns + gosFX dev-override caveat note |

### Files created (2)

| Path | Purpose |
|---|---|
| `scripts/check-vfx-no-objectid.sh` | Firewall grep gate; forbids `layout(location=2) out` in VFX shaders |
| `scripts/check-vfx-no-objectid.allowlist` | Empty allowlist; header explains it is expected empty forever |

---

### Step 1 — CMakeCache verification (worktree build dir pin)

Verify the build is configured for the correct worktree, NOT the root
checkout's stale `build64/` (CLAUDE.md "Key paths" rule).

```bash
grep -c "nifty-mendeleev" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/CMakeCache.txt"
```

**Expected:** at least 1 occurrence (the source path is rooted in the
worktree). If 0, STOP — the build dir is misconfigured; do not proceed.

---

### Step 2 — Edit `RenderWorld/RenderWorld.h`: add `Vfx = 3`

**Pre-edit grep (mandatory before Edit):**

```bash
grep -n "RenderObjectKind\|StaticProp = 0\|Mech.*= 1\|Future: Terrain" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.h"
```

Expected to confirm current enum body at lines 132-136.

#### Case A: M3 has NOT shipped (current state at plan-write time `16a461b`)

The enum currently reads:

```cpp
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    // Future: Terrain=2, Vfx=3, Overlay=4
};
```

Edit (Existing → Replace):

**Existing:**
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
    Vfx        = 3,   // M4: RESERVED. No writer ships in v1.
                      // VFX shaders are PROHIBITED from writing color-attachment-2
                      // (R32_UINT objectID substrate) — last-write-wins on integer
                      // attachments clobbers M2.6 mech-pick under translucent/additive
                      // particles. Enforced by scripts/check-vfx-no-objectid.sh.
                      // See docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md.
    // Future: Terrain=2 (reserved in M3), Overlay=4
};
```

The `Vfx = 3` value matches the sidecar Q-resolutions table (M4 ship
scope: enum value `RenderObjectKind::Vfx = 3`). The gap at value `2` is
intentional and harmless — `Terrain` will be added later by M3 with
value 2. Enum stability rule: never renumber; only append.

#### Case B: M3 has already shipped (Terrain = 2 already present)

If M3 shipped first, the enum will read:

```cpp
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    Terrain    = 2,   // M3: RESERVED. ...
    // Future: Vfx=3, Overlay=4
};
```

In that case, re-read the file at execution time and replace the
trailing `// Future: Vfx=3, Overlay=4` comment with the `Vfx = 3,` enum
member (same body block as Case A but inserted after `Terrain = 2,`).
**Use exact Existing/Replace blocks read from the live file** — do not
trust this plan's text for Case B; M3's exact comment phrasing is
unknown at plan-write time.

**Verification after edit:**

```bash
grep -n "Vfx\s*=\s*3" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.h"
```

Must return exactly 1 hit.

---

### Step 3 — Edit `RenderWorld/RenderWorld.cpp`: add `kVfxHandleBase`

**Pre-edit grep (mandatory):**

```bash
grep -n "kMechHandleBase\|kTerrainHandleBase\|kVfxHandleBase" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.cpp"
```

At plan-write time the result is:
- Lines 114, 117, 120, 121, 821, 824, 926 reference `kMechHandleBase`
- No `kTerrainHandleBase` or `kVfxHandleBase` exist

#### Case A: M3 has NOT shipped (current state)

Insert the `kVfxHandleBase` declaration immediately after the existing
`kMechHandleBase = 0x00010000u;` at line 121.

**Existing:**
```cpp
// kMechHandleBase: handle index base for mechs in the unified s_objectRecords table.
// Must exceed the maximum static-prop recipe index across all tier1 missions.
// Known max: 2641 (mc2_24). 65536 provides 24x headroom.
// INVARIANT: max static-prop recipe index < kMechHandleBase.
static constexpr uint32_t kMechHandleBase = 0x00010000u;
```

**Replace with:**
```cpp
// kMechHandleBase: handle index base for mechs in the unified s_objectRecords table.
// Must exceed the maximum static-prop recipe index across all tier1 missions.
// Known max: 2641 (mc2_24). 65536 provides 24x headroom.
// INVARIANT: max static-prop recipe index < kMechHandleBase.
static constexpr uint32_t kMechHandleBase = 0x00010000u;

// M4: VFX handle base. RESERVED PER SPEC; NO WRITER ISSUES HANDLES IN v1.
//
// VFX shaders are PROHIBITED from writing color-attachment-2 (the M1.5
// R32_UINT objectID substrate). Integer color attachments do not blend
// (GL 4.5 §17.3.6) — additive/alpha-blended particles would last-write-wins
// clobber any mech/static-prop ID underneath, silently breaking M2.6
// mech-pick on any mech occluded by a translucent particle. The
// prohibition is enforced by scripts/check-vfx-no-objectid.sh.
//
// This constant exists ONLY for handle-range allocation bookkeeping so a
// future slice (if any) that opts in to per-emitter handles has a stable
// reserved base to use. The current recon recommends "never use this."
//
// Full corrected partitioning (do NOT re-derive; see migration guide §12):
//   StaticProp:  0x00000..0x0FFFF   (max observed mc2_24 = 2641)
//   Mech:        0x10000..0x3FFFF   (max observed mc2_24 = 46)
//   Terrain:     0x40000..0x7FFFF   (reserved by M3; no writer in v1)
//   Vfx:         0x80000..0xBFFFF   (this slot; PROHIBITED writers per §3.6)
//   Overlay:     0xC0000..0xFFFFE   (reserved/deferred per M5 sidecar)
//   Sentinel:    0xFFFFF            (bug-bait; never allocate)
//
// 20-bit index mask (RenderCore/Handle.h:34) limits the absolute max to
// 0xFFFFF (1,048,575). A prior draft proposed 0x200000 — that OVERFLOWS
// the mask and would silently truncate to index=0, colliding with
// static-prop slot 0. Do not repeat that trap.
static constexpr uint32_t kVfxHandleBase = 0x00080000u;
```

#### Case B: M3 has shipped (kTerrainHandleBase already present)

Re-read the file at execution time; insert the `kVfxHandleBase` block
immediately AFTER the existing `kTerrainHandleBase = 0x00040000u;`
declaration. The Replace-with block above is otherwise identical (the
partitioning chart already names Terrain at 0x40000..0x7FFFF).

**Verification after edit:**

```bash
grep -n "kVfxHandleBase\s*=\s*0x00080000u" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.cpp"
```

Must return exactly 1 hit.

**Critical:** `kVfxHandleBase` MUST NOT be referenced anywhere else in
`RenderWorld.cpp` in this commit — it is unused-by-design. The compiler
may warn about an unused constant; that warning is acceptable (the
constant is reserved-future-use bookkeeping per spec §8). If the build
treats unused-constant warnings as errors and the compiler complains,
suppress with a `[[maybe_unused]]` attribute or add a `(void)kVfxHandleBase;`
self-test in an existing function — DO NOT pull the constant into use.
(Self-check: the existing `kMechHandleBase` is used in 4 places at plan-write
time; the analogous unused-constant suppression need is not expected to fire
in MSVC `RelWithDebInfo` defaults but plan for it.)

---

### Step 4 — Create `scripts/check-vfx-no-objectid.sh`

New file, modeled on `scripts/check-no-raw-gl-from-game.sh` (132 lines;
the M6 firewall reference).

**Content:**

```sh
#!/bin/sh
# scripts/check-vfx-no-objectid.sh
#
# RenderWorld Slice M4: firewall enforcing the rule that VFX shaders
# MUST NOT write color-attachment-2 (the M1.5 R32_UINT objectID substrate).
#
# Why this exists: integer color attachments do not blend (GL 4.5 §17.3.6).
# An additive/alpha-blended particle fragment that writes attachment-2 will
# LAST-WRITE-WINS clobber the opaque mech/static-prop objectID underneath,
# silently breaking M2.6 mech-pick on any mech occluded by a translucent
# particle (muzzle flash, smoke, tracer, impact effect). The user clicks
# the mech, nothing happens — no log line marks the failure. Hard to
# debug from the gameplay symptom back to the FS shader source.
#
# Cure: prohibit `layout(location=2) out` declarations in VFX shaders.
# Enforced mechanically — a contributor who tries to add the write hits
# CI failure immediately rather than three months later when a player
# reports flaky mech-pick.
#
# Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md (§4 trap, §9 gate)
# Resolutions: docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md (Q1 confirms)
# Migration guide: docs/renderworld_migration_guide.md §3.6
#
# Allowlist: scripts/check-vfx-no-objectid.allowlist
# (Expected to be EMPTY forever. Adding an entry means you read the
# allowlist header and accept the documented trap. See the header.)
#
# Scan set: VFX shader basenames. At M4 ship time the only VFX shader pair
# is shaders/particle_billboard.{vert,frag}. Extend BASENAMES below when
# adding a new VFX shader file. See M4 spec §6 (surface table) + §12
# trap 3 (scan-set drift risk).
#
# Negative-test (to verify the script catches violations):
#   1. Temporarily add `layout(location=2) out uint v_test;` to
#      shaders/particle_billboard.frag (the declaration alone is enough;
#      no need to write to it).
#   2. Run this script. Expect exit 1 + the file:line reported.
#   3. Revert the injection (`git checkout shaders/particle_billboard.frag`).
#   4. Re-run. Expect exit 0.
#
# Exit 0 = clean (zero non-allowlisted matches across scan set)
# Exit 1 = at least one non-allowlisted violation

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# VFX shader basenames. Hand-maintained; reviewers must flag any new
# shader file in shaders/ that renders particles/effects.
BASENAMES="
particle_billboard.vert
particle_billboard.frag
"

ALLOWLIST="scripts/check-vfx-no-objectid.allowlist"
PATTERN='layout[[:space:]]*\([[:space:]]*location[[:space:]]*=[[:space:]]*2[[:space:]]*\)[[:space:]]*out'

VIOLATIONS=0

allowlisted() {
    # $1 = path
    [ -f "$ALLOWLIST" ] || return 1
    while IFS= read -r _aline; do
        case "$_aline" in
            ""|"#"*) continue ;;
        esac
        if [ "$1" = "$_aline" ]; then
            return 0
        fi
    done < "$ALLOWLIST"
    return 1
}

# Strip the path:lineno: prefix and check if the remaining content is a
# GLSL comment-only line. Mirrors check-no-raw-gl-from-game.sh shape.
is_comment_line() {
    body="$(printf '%s' "$1" | sed 's/^[^:]*:[0-9][0-9]*://' | sed 's/^[[:space:]]*//')"
    case "$body" in
        "//"*) return 0 ;;
        "/*"*) return 0 ;;
        "*"*)  return 0 ;;
    esac
    return 1
}

TMPHITS="$(mktemp 2>/dev/null || echo "/tmp/vfx-no-objectid.$$")"
trap 'rm -f "$TMPHITS"' EXIT

: > "$TMPHITS"

# Iterate the curated basename list. Each must exist under shaders/;
# missing files are an error (the basename list is stale).
echo "$BASENAMES" | while IFS= read -r name; do
    [ -z "$name" ] && continue
    src="shaders/$name"
    if [ ! -f "$src" ]; then
        echo "scripts/check-vfx-no-objectid.sh: WARN scan-set entry missing: $src" >&2
        continue
    fi
    grep -nE "$PATTERN" "$src" 2>/dev/null | while IFS= read -r match; do
        [ -z "$match" ] && continue
        printf '%s:%s\n' "$src" "$match"
    done
done > "$TMPHITS"

if [ -s "$TMPHITS" ]; then
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        file="$(printf '%s' "$line" | cut -d: -f1)"
        if allowlisted "$file"; then
            continue
        fi
        if is_comment_line "$line"; then
            continue
        fi
        echo "VIOLATION: VFX shader writes objectID attachment-2 in ${line}" >&2
        VIOLATIONS=$((VIOLATIONS+1))
    done < "$TMPHITS"
fi

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "" >&2
    echo "scripts/check-vfx-no-objectid.sh: ${VIOLATIONS} violation(s)" >&2
    echo "VFX shaders MUST NOT write color-attachment-2 (R32_UINT objectID)." >&2
    echo "Integer attachments do not blend; particle writes last-write-wins clobber" >&2
    echo "underlying mech/static-prop IDs, silently breaking M2.6 mech-pick." >&2
    echo "" >&2
    echo "Spec:           docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md (§4)" >&2
    echo "Migration:      docs/renderworld_migration_guide.md §3.6" >&2
    echo "Resolutions:    docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md (Q1)" >&2
    echo "" >&2
    echo "If you have a genuine use case requiring a VFX writer, reopen the spec" >&2
    echo "and address Q1-Q5 (do NOT add to scripts/check-vfx-no-objectid.allowlist" >&2
    echo "without reading its header first)." >&2
    exit 1
fi

echo "scripts/check-vfx-no-objectid.sh: clean (VFX shaders satisfy attachment-2 prohibition)"
exit 0
```

**Permissions:** The script must be executable (`chmod +x`). On Windows
git, executable bit may be managed via `git update-index --chmod=+x`.
Other firewall scripts in `scripts/` already carry the bit — match
their state by reading one of them first.

**Verification after creation:**

```bash
sh "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-vfx-no-objectid.sh"
echo "exit=$?"
```

Expected output ends with `clean (VFX shaders satisfy attachment-2 prohibition)`
and `exit=0`.

---

### Step 5 — Create `scripts/check-vfx-no-objectid.allowlist`

New empty allowlist with header comment explaining when entries should
be added (rationale: NEVER expected to have entries; allowlist exists
for future-emergency only).

**Content (exact, including trailing newline):**

```
# M4 VFX no-objectId allowlist
# Each entry is a path RELATIVE to the worktree root.
#
# EXPECTED TO BE EMPTY. VFX shaders must NEVER write to attachment-2 because
# additive/translucent blending + last-write-wins on R32_UINT clobbers any
# object-ID underneath, breaking M2.6 mech-pick through muzzle flashes,
# smoke, tracers, and impacts. The correct behavior is "VFX is click-through;
# pick the object underneath."
#
# If you think you need to add an entry here, STOP and read:
# - docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md
# - docs/renderworld_migration_guide.md §3.5 / §12
# The right answer is almost certainly "don't write attachment-2 from VFX."
```

(Note: the comment references §3.5 + §12 because §3.6 is the NEW section
being added in this same commit and the §3.5 raw-GL prohibition shares
the firewall-discipline philosophy a contributor needs to absorb.)

---

### Step 6 — Edit `docs/renderworld_migration_guide.md`: add §3.6 prohibition

Insert a new subsection §3.6 immediately after §3.5 (which currently
ends at line 195 with `--- ` separator) and before §4 (starts at line 197).

**Pre-edit grep:**

```bash
grep -n "^## " "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/renderworld_migration_guide.md" | head -20
```

Confirms §3.5 at line 170 and §4 at line 197.

**Existing (the section break + start of §4):**
```

---

## 4. How to add a new `RenderObjectKind`
```

**Replace with:**
```

---

## 3.6 VFX shaders are PROHIBITED from writing object IDs

VFX shaders (`shaders/particle_billboard.{vert,frag}`, any future
particle/effect shader) MUST NOT contain `layout(location=2) out uint`
declarations.

**Why:** The object-ID buffer is `R32_UINT` with last-write-wins on
integer attachments (GL 4.5 §17.3.6). Translucent/additive particle
fragments would clobber the mech/static-prop ID underneath even when
visually transparent. This breaks M2.6 mech-pick through muzzle flashes,
smoke, tracers, impacts — ANY effect rendered in front of a pickable
object.

**Correct behavior:** VFX is click-through. `lookupAtPixel` returns the
object UNDER the effect (mech, static prop, terrain). Game logic that
needs "which mech fired this explosion" looks it up at weapon/fire-event
time, NOT via GPU pixel readback (the source mech handle is already
known in CPU game state when the effect is spawned).

**Enforced by:** `scripts/check-vfx-no-objectid.sh` (CI / pre-commit).

**Allowlist:** `scripts/check-vfx-no-objectid.allowlist` — expected to
be empty. Adding an entry requires understanding why the allowlist
exists (read the file header).

**Reserved enum slot:** `RenderObjectKind::Vfx = 3` exists for handle-
range bookkeeping only. No writer should produce a Vfx handle in v1.
`kVfxHandleBase = 0x00080000u` reserved in the partition chart in §12.

**gosFX dev-override caveat:** `MC2_DISABLE_GOSFX=0` (developer override
re-enabling MLR gosFX) is BROKEN under unified-projection F1 (see
worktree `CLAUDE.md` known-issues section). Even if a future slice were
to flip the M4 prohibition (against current recon recommendation), the
substrate could not be validated under the gosFX dev-override path until
MLR retirement Slices 1-5 ship and the convention is corrected. Do NOT
use `MC2_DISABLE_GOSFX=0` as a substrate proof path for any VFX work.

---

## 4. How to add a new `RenderObjectKind`
```

---

### Step 7 — Edit `docs/renderworld_migration_guide.md`: rewrite §12 handle-base chart

Current §12 (lines 541-563) has a chart with TBD entries for Terrain,
Vfx, Overlay. Replace the chart with the corrected partitioning.

**Pre-edit grep:**

```bash
grep -n "^## 12\.\|kMechHandleBase = 0x10000\|StaticProp.*0.*2641" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/renderworld_migration_guide.md"
```

Confirms §12 header at line 541.

**Existing:**
```
## 12. The `kMechHandleBase = 0x10000` pattern

The unified `s_objectRecords` table is indexed by `handle.index()` (20 bits,
`[0..1048575]`). Different kinds allocate disjoint index ranges so a stale
handle's index alone reveals what kind it was meant to be (useful in logs
even when generation is wrong).

Existing allocations:

| Kind        | Base       | Max observed (tier1 mc2_24) | Headroom |
|-------------|------------|-----------------------------|----------|
| StaticProp  | 0          | 2641                        | ~24x to mech base |
| Mech        | 0x00010000 | ~50                         | huge     |
| Terrain     | TBD (0x00020000 recommended) | -- | -- |
| Vfx         | TBD (0x00040000 recommended) | -- | -- |
| Overlay     | TBD (0x00080000 recommended) | -- | -- |

Allocation rule: pick a base with at least one decimal order of magnitude
headroom over the projected max. Power-of-two bases let you visually
disambiguate index ranges from a single log line.

When you allocate a base, document it in `RenderWorld/RenderWorld.cpp`
alongside `kMechHandleBase` so the next contributor can find the convention.
```

**Replace with:**
```
## 12. The handle-base partitioning pattern

The unified `s_objectRecords` table is indexed by `handle.index()` (20 bits,
`[0..1048575]`). Different kinds allocate disjoint index ranges so a stale
handle's index alone reveals what kind it was meant to be (useful in logs
even when generation is wrong).

Allocations (corrected per M3 + M4 sidecar resolutions; supersedes earlier
TBD draft):

| Kind        | Handle base           | Slot count | Notes |
|-------------|-----------------------|-----------:|-------|
| StaticProp  | `0x00000..0x0FFFF`    | 64k        | Max observed mc2_24 = 2641 |
| Mech        | `0x10000..0x3FFFF`    | 192k       | Max observed mc2_24 = 46 |
| Terrain     | `0x40000..0x7FFFF`    | 262k       | Reserved (M3); no writer in v1 |
| Vfx         | `0x80000..0xBFFFF`    | 262k       | Reserved (M4); PROHIBITED writers per §3.6 |
| Overlay     | `0xC0000..0xFFFFE`    | 262k-1     | Deferred indefinitely (M5); slot may be reclaimed |
| Sentinel    | `0xFFFFF`             | 1          | Reserved bug-bait; never allocate |

20-bit index mask = `0xFFFFF` (per `RenderCore/Handle.h:34`). Bases above
`0xFFFFF` SILENTLY TRUNCATE to `index=0`, colliding with static-prop slot 0
— do NOT propose any base `>= 0x100000`. A prior draft hit this trap with
`0x200000`.

Allocation rule: pick a base with at least one decimal order of magnitude
headroom over the projected max. Power-of-two bases let you visually
disambiguate index ranges from a single log line.

When you allocate a base, document it in `RenderWorld/RenderWorld.cpp`
alongside `kMechHandleBase` so the next contributor can find the convention.
```

(Note: §12 heading text changed from "The `kMechHandleBase = 0x10000` pattern"
to "The handle-base partitioning pattern" because the section now documents
the entire partition, not just the Mech entry. Any other section that
cross-references §12 by heading text would need updating — at plan-write
time, grep `\#12|§12|section 12|Section 12` in the migration guide and
in `docs/superpowers/` returns no cross-references to the old heading.)

**Cross-reference verification (post-edit, mandatory):**

```bash
grep -rn "kMechHandleBase = 0x10000" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/" 2>/dev/null
```

Should return 0 hits after the §12 rewrite (the only docs reference was
in the heading; the code-level constant in `RenderWorld.cpp` is
unchanged and not under `docs/`).

---

### Step 8 — Edit `CLAUDE.md` (worktree): add M4 SHIPPED entry

Add a single bullet to the "Active campaigns" section. The section
already has M1, M1.5, M1.6, M2-pre, M2, M2.5, M2.6, M6 entries; M4
goes after M6 (M3+M5 atomic commit will land between M6 and M4 per
sidecar order-of-operations, but since M3+M5 ships first the M4 entry
appears after M3+M5 in CLAUDE.md once both are present).

**Pre-edit grep:**

```bash
grep -n "RenderWorld Slice M6\|## Active campaigns" "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md"
```

The M4 bullet text to add (positioned after the M3+M5 entry if present,
otherwise immediately after the M6 entry):

```
- **RenderWorld Slice M4** (SHIPPED 2026-05-24): VFX prohibition + scaffold. Adds `RenderObjectKind::Vfx = 3` enum value + `kVfxHandleBase = 0x00080000u` constant (reserved, unused) + NEW `scripts/check-vfx-no-objectid.sh` firewall grep gate. **VFX shaders are PROHIBITED from writing attachment-2** — additive/translucent blending + R32_UINT last-write-wins would clobber M2.6 mech-pick under particles (muzzle flashes, smoke, tracers, impacts). Migration guide §3.6 documents the rule + rationale; allowlist scripted but expected empty forever. NO adapter, registerEffect, per-emitter handles, objectIdRaw fields, or shader writes. Source-game-object lookup ("which mech fired this explosion?") stays in game logic (source known at fire-event time; GPU should not rediscover via pixel). Caveat: gosFX dev-override (`MC2_DISABLE_GOSFX=0`) remains broken under unified-projection F1 (see known-issues section) — future VFX work must NOT use that path as a substrate proof. Tier1 5/5 PASS env-OFF. Greybeard ruling: META-FIX (retires bug-class "future contributor adds VFX objectID write, breaks M2.6 mech-pick" before it can occur; substitutive proof = firewall returns 0 hits + future commit re-introducing one fails CI). Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md`. Resolutions: `docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md`. Plan: `docs/superpowers/plans/2026-05-24-renderworld-slice-m4-plan.md`.
```

**Length-cap discipline (CLAUDE.md rule "Keep this file under 200 lines"):**
The bullet above is single-paragraph (single bullet item) consistent
with existing slice entries. If after insert the file approaches the
200-line cap, the older M1/M1.5 entries are candidates for extraction
to a memory file per the worktree CLAUDE.md "Memory & CLAUDE.md
discipline" rule. Do NOT preemptively extract in this commit — extract
only if the cap is breached. At plan-write time the file is
substantially over 200 lines already (slice entries are large), so this
is an existing-debt note, not an M4 action.

---

### Step 9 — Pre-commit branch check

Verify on the correct branch before building:

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" && git rev-parse --abbrev-ref HEAD
```

Expected: `claude/nifty-mendeleev`. If different, STOP and resolve
before any further action.

---

### Step 10 — Build

Worktree build dir pin + `RelWithDebInfo` mandatory (CLAUDE.md
"Critical inline rules"):

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" \
    --config RelWithDebInfo
```

**Expected:** build succeeds. The only modified C++ files are
`RenderWorld/RenderWorld.h` (enum addition — header touch will
recompile all `RenderWorld.h` includers; this is unavoidable but cheap)
and `RenderWorld/RenderWorld.cpp` (constant addition only).

**Watch for:** unused-constant warnings on `kVfxHandleBase` (see Step 3
"Critical" note). If warning-as-error fires, suppress per Step 3
guidance.

**Full-relink NOT required:** no class-layout change, no inline-function
body change, no template/static-state change (per CLAUDE.md "Full
relink before deploy when load-bearing functions change"). Adding a
new enum value (appended, not renumbered) and a new constexpr constant
do not alter existing function bodies or class layouts.

---

### Step 11 — Deploy

Per CLAUDE.md "Deploy" rule: never `cp -r`; always `cp -f` per file +
`diff -q`.

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
        "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```

**Expected:** `diff -q` exits 0 (files identical) — confirms cp
succeeded.

**Shader tree NOT redeployed:** No shader files were modified by M4.
The "shaders deploy in lockstep" rule (CLAUDE.md) does not apply — but
verify nothing slipped in:

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev" && git status --short shaders/
```

Expected: empty output. If anything shows, STOP — the slice is no
longer shader-clean and the deploy step needs the shader tree too.

---

### Step 12 — Tier1 5/5 smoke (env-OFF)

Canonical invocation (verbatim from CLAUDE.md):

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

**Expected:** exit 0; all 5 missions pass (`mc2_01`, `mc2_03`, `mc2_10`,
`mc2_17`, `mc2_24`). Pixel-parity is trivial — M4 adds no draw state
changes, no shader writes, no path fires. The enum value and constant
are declared but never read from any code path. If any mission fails,
inspect `tests/smoke/artifacts/<latest>/` per CLAUDE.md "Smoke gate"
rules.

**No env-ON tier1 needed:** M4 has zero runtime substrate. Per spec
§10 validation point 2, env-OFF parity is the canonical gate. The
spec's optional env-ON `MC2_OBJECT_ID_BUFFER=1` tier1 (spec §10 point
3) is covered by the existing M2.6 SHIPPED state — M4 changes nothing
that would affect it. The M4 plan does NOT re-run it (analysis-paralysis
guard; M4 is correctness-by-non-action).

---

### Step 13 — Run new firewall script (positive validation)

```bash
sh "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-vfx-no-objectid.sh"
echo "exit=$?"
```

**Expected:**
```
scripts/check-vfx-no-objectid.sh: clean (VFX shaders satisfy attachment-2 prohibition)
exit=0
```

This is the substitutive proof for the greybeard META-FIX ruling: the
bug-class "VFX writes attachment-2" is provably empty in the current
tree.

---

### Step 14 — Negative-injection test (script-works-when-tripped proof)

This is the gate that distinguishes a working firewall from a placebo
(the script could pass for the wrong reason — e.g. wrong pattern,
wrong scan set). Negative injection proves the gate actually catches
violations.

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"

# 1. Inject violation
printf '\nlayout(location=2) out uint v_test_injected;\n' >> shaders/particle_billboard.frag

# 2. Run firewall; expect FAIL
sh scripts/check-vfx-no-objectid.sh
INJECTED_EXIT=$?
echo "injected_exit=$INJECTED_EXIT"

# 3. Revert injection
git checkout -- shaders/particle_billboard.frag

# 4. Re-run firewall; expect PASS
sh scripts/check-vfx-no-objectid.sh
CLEAN_EXIT=$?
echo "clean_exit=$CLEAN_EXIT"

# 5. Confirm working tree is clean
git status --short shaders/
```

**Expected:**
- `injected_exit=1` (firewall caught the injection)
- Stderr from injected run includes `VIOLATION: VFX shader writes objectID attachment-2 in shaders/particle_billboard.frag:NN:layout(location=2) out uint v_test_injected;`
- `clean_exit=0` (firewall passes after revert)
- `git status --short shaders/` is empty (no leftover changes)

If `injected_exit=0`, the firewall is broken — STOP and debug the
pattern / scan-set before commit. Possible causes: pattern regex
mismatch (e.g. extra `[[:space:]]+` requirement that doesn't match the
injected text), scan-set missing `particle_billboard.frag`, or
`is_comment_line` false positive (the injection has no leading `//`,
so this should not fire).

If `git status --short shaders/` shows leftover changes, STOP and
revert manually — the commit MUST NOT carry the injection.

---

### Step 15 — Verify M6 firewall still passes

The M6 raw-GL firewall is orthogonal to M4 but is the closest neighbor.
Confirm M4 changes did not break it (e.g. by introducing a raw GL call
in a sample comment block that the script's comment-strip miscategorizes).

```bash
sh "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-no-raw-gl-from-game.sh"
echo "exit=$?"
```

**Expected:** `exit=0` with `scripts/check-no-raw-gl-from-game.sh: clean (scope: code mclib)`.

The M4 commit modifies `RenderWorld/RenderWorld.{h,cpp}`, neither of
which is under `code/` or `mclib/` (the M6 SCOPE_DIRS) — so M6 is
unaffected by construction. This step is a defensive re-run, not a
substantive validation.

---

### Step 16 — Commit (single atomic commit)

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add \
    RenderWorld/RenderWorld.h \
    RenderWorld/RenderWorld.cpp \
    scripts/check-vfx-no-objectid.sh \
    scripts/check-vfx-no-objectid.allowlist \
    docs/renderworld_migration_guide.md \
    CLAUDE.md \
    docs/superpowers/plans/2026-05-24-renderworld-slice-m4-plan.md
```

(Explicit per-file `git add` — never `git add -A` or `git add .` per
CLAUDE.md "Git safety protocol" / general sensitive-file hygiene.)

```bash
git commit -m "$(cat <<'EOF'
feat(renderworld): M4 VFX prohibition + scaffold

Adds RenderObjectKind::Vfx = 3 enum value + kVfxHandleBase = 0x00080000u
constant (reserved, unused) + NEW scripts/check-vfx-no-objectid.sh
firewall grep gate. VFX shaders are PROHIBITED from writing
color-attachment-2 (R32_UINT objectID substrate) — additive/translucent
blending + last-write-wins on integer attachments would clobber M2.6
mech-pick under particles (muzzle flashes, smoke, tracers, impacts).

Migration guide §3.6 documents the rule + rationale; §12 handle-base
chart updated with corrected partitioning (StaticProp 0..0xFFFF / Mech
0x10000..0x3FFFF / Terrain 0x40000..0x7FFFF / Vfx 0x80000..0xBFFFF /
Overlay 0xC0000..0xFFFFE / Sentinel 0xFFFFF). Allowlist scripted but
expected empty forever; header explains why.

NO adapter, registerEffect/destroyEffect API, per-emitter handles,
objectIdRaw fields on any particle SSBO, runtime gates, env vars, or
shader writes. Source-game-object lookup ("which mech fired this
explosion?") stays in game logic (source known at fire-event time;
GPU should not rediscover via pixel readback).

Caveat (CLAUDE.md known-issues): gosFX dev-override (MC2_DISABLE_GOSFX=0)
remains broken under unified-projection F1. Future VFX work must NOT
use that path as a substrate proof until MLR retirement Slices 1-5 ship.

Greybeard ruling: META-FIX. Retires the bug class "future contributor
adds VFX attachment-2 write, silently breaks M2.6 mech-pick" before it
can occur. Substitutive proof: firewall grep returns 0 hits across VFX
scan set today AND any future commit re-introducing one fails CI.
Negative-injection test passed at execution time.

Tier1 5/5 PASS env-OFF (pixel-parity vs parent; no path fires).

Spec:        docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md
Resolutions: docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md
Plan:        docs/superpowers/plans/2026-05-24-renderworld-slice-m4-plan.md
EOF
)"
```

(The plan file itself is included in the commit so the SHIPPED artifact
travels with the slice — same convention as prior RenderWorld slices.)

**Post-commit verification:**

```bash
git log -1 --stat
git status --short
```

Expected: 6 files in the commit stat (4 modified + 2 created); clean
working tree.

---

## Total step count

16 steps, single commit. M4 is the smallest meaningful slice in the
RenderWorld arc per spec §2.

---

## Failure-mode triage (if a step blocks)

| Step | Blocking outcome | First-pass triage |
|---|---|---|
| 1 | Build dir misconfigured | Re-run cmake configure pinned to worktree source; do NOT proceed if root `build64/` is in use |
| 2 | Enum edit fails (Existing block mismatch) | Re-grep `RenderObjectKind` block; M3 may have shipped between plan-write and execute — use Case B path |
| 3 | Constant edit fails | Re-grep `kMechHandleBase` / `kTerrainHandleBase`; insert position depends on M3 state |
| 4 | Script content mismatch / permission denied | `chmod +x`; check git executable bit matches other firewall scripts |
| 10 | Build fails on unused-constant warning | Add `[[maybe_unused]]` to `kVfxHandleBase`; do NOT pull constant into use |
| 12 | Tier1 fails | Likely UNRELATED to M4 (no runtime change). Inspect `tests/smoke/artifacts/<latest>/`. If failure is M4-related (unexpected), bisect against parent `16a461b` |
| 13 | Firewall script returns nonzero on clean tree | Bug in script — debug pattern / scan-set; do NOT commit a broken firewall |
| 14 | Injected violation NOT caught (exit=0) | Script is placebo — STOP, fix pattern/scan-set, re-run from Step 13 |
| 14 | Inject-revert leaves dirty working tree | Manually `git checkout -- shaders/particle_billboard.frag`; verify `git status` clean before commit |
| 16 | Commit blocked by pre-commit hook | Investigate hook output; create NEW commit after fix per CLAUDE.md git-safety rule (do NOT --amend) |

---

## Out-of-scope (explicit non-goals; flag for separate work if surfaced)

Per spec §2 + sidecar:

- **No adapter.** `GameAdapters/VfxRenderAdapter.{h,cpp}` is NOT created.
- **No registerEffect / destroyEffect.** No `RenderWorld::` API additions
  beyond enum + constant.
- **No `objectIdRaw` on particle SSBO.** `GpuParticleInstance` (if it
  exists) is untouched.
- **No shader writes.** `particle_billboard.frag` keeps its current
  single-output (`outColor`).
- **No gate-flip.** `MC2_GPU_PARTICLES` stays default-off;
  `MC2_DISABLE_GOSFX` stays default-on. M4 has zero env vars of its own.
- **No M3 work.** Terrain enum / constant / tripwire ships separately
  per sidecar order-of-operations.
- **No M5 work.** Overlay deferred indefinitely per sidecar; folded into
  M3 commit (enum block comment update only).
- **No gosFX dev-override fix.** Documented caveat only; the fix is
  blocked by MLR retirement Slices 1-5.

---

## Greybeard ruling (mandatory inline per CLAUDE.md "Meta-fix discipline")

**Ruling:** META-FIX.

**Bug class retired:** "Future contributor adds `layout(location=2) out
uint v_objectId` to a VFX shader (intentionally, copying the M1.5/M2.5
pattern from `mech.frag`/`static_prop.frag` without understanding the
integer-blend semantics), silently breaking M2.6 mech-pick on any mech
occluded by a translucent particle."

**Substitutive proof:**
1. `scripts/check-vfx-no-objectid.sh` exit 0 today (current tree has 0
   `layout(location=2) out` declarations across the VFX scan set —
   grep-verified at plan-write time).
2. Any future commit that re-introduces a match fails CI (negative-
   injection test at Step 14 confirms the gate triggers).
3. The trap is documented at three levels: spec §4 (mechanism), migration
   guide §3.6 (rule), script header + diagnostic stderr (point-of-violation
   pointer). A contributor cannot disable the gate without absorbing the
   reason.

**Why not PATCH (justified):** A PATCH ruling would be "wait until a
contributor adds the write, then revert + educate." That requires the
bug to ship at least once, with user-visible mech-pick flakiness as
the discovery surface. The firewall gate retires the class
preemptively at zero runtime cost.

**Anti-pattern + cure documented at same level:** Per CLAUDE.md
"meta-engineering / bug-class-retirement" discipline. The cure (grep
gate) and the anti-pattern (additive-blend integer-attachment clobber)
are co-located in the migration guide §3.6 and the script header.

---

## Commit-message-ready summary

```
feat(renderworld): M4 VFX prohibition + scaffold

Adds RenderObjectKind::Vfx = 3 enum value + kVfxHandleBase = 0x00080000u
constant (reserved, unused) + NEW scripts/check-vfx-no-objectid.sh
firewall grep gate. VFX shaders are PROHIBITED from writing
color-attachment-2 (R32_UINT objectID substrate) — additive/translucent
blending + last-write-wins on integer attachments would clobber M2.6
mech-pick under particles (muzzle flashes, smoke, tracers, impacts).

Migration guide §3.6 documents the rule + rationale; §12 handle-base
chart updated with corrected partitioning. Allowlist scripted but
expected empty forever.

NO adapter, registerEffect/destroyEffect API, per-emitter handles,
objectIdRaw fields, runtime gates, env vars, or shader writes.
Source-game-object lookup stays in game logic (source known at
fire-event time; GPU should not rediscover via pixel readback).

Caveat: gosFX dev-override (MC2_DISABLE_GOSFX=0) remains broken under
unified-projection F1; future VFX work must NOT use that path as a
substrate proof until MLR retirement Slices 1-5 ship.

Greybeard ruling: META-FIX (retires bug-class before it can occur;
substitutive proof = firewall returns 0 hits + injection-test triggers).

Tier1 5/5 PASS env-OFF (pixel-parity; no path fires).

Spec:        docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md
Resolutions: docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md
Plan:        docs/superpowers/plans/2026-05-24-renderworld-slice-m4-plan.md
```

---

PLAN STATUS: READY FOR EXECUTE
