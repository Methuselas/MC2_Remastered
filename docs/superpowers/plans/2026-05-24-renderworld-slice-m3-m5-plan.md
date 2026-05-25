# RenderWorld Slice M3 + M5 — Atomic Plan

- **Date:** 2026-05-24
- **Slice:** M3 (terrain reservation/deferral) + M5 (overlay defer-indefinitely)
- **Predecessor:** M6 SHIPPED 2026-05-24 (HEAD anchor for parity baseline)
- **Spec(s):**
  - `docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md`
  - `docs/superpowers/specs/2026-05-23-renderworld-slice-m5-overlay-spec.md`
- **Resolutions sidecar (load-bearing):**
  `docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md`
- **Branch:** `claude/nifty-mendeleev`
- **Build dir:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/` (RelWithDebInfo only)
- **Deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.4/`

## 0. Why one commit covers both slices

M3 substantive scope and M5 deferral scope **both touch the same
`RenderObjectKind` enum block** (`RenderWorld/RenderWorld.h:131-135`).
Splitting into two commits would force a two-step edit of the same
comment line and add a synthetic mid-state ("M3 landed, M5 comment
half-written"). The resolutions sidecar §"Execution plan" (line 91)
explicitly pre-approves one-commit-for-both.

- **M3 substantive:** enum value `Terrain = 2`, `kTerrainHandleBase`
  constant, trip-wire branch in `lookupAtPixel`, migration guide §12
  table update, CLAUDE.md SHIPPED entry.
- **M5 fold-in:** enum block comment update (deferral pointer), CLAUDE.md
  deferred note (NOT a SHIPPED entry — nothing implemented).

Commit message acknowledges both slices explicitly.

## 1. Pre-flight verification (grep-verified at write-time)

| Anchor | File:line | Notes |
|---|---|---|
| Enum block | `RenderWorld/RenderWorld.h:131-136` | `enum class RenderObjectKind` + `// Future: Terrain=2, Vfx=3, Overlay=4` comment |
| Mech handle base | `RenderWorld/RenderWorld.cpp:117-121` | `static constexpr uint32_t kMechHandleBase = 0x00010000u;` |
| lookupAtPixel | `RenderWorld/RenderWorld.cpp:702-779` | post-alive-check field-population begins at `:768` (`out.isValid = true;`) — trip-wire inserts BEFORE `:768` |
| Migration guide §12 table | `docs/renderworld_migration_guide.md:550-556` | Terrain row at `:554` reads `TBD (0x00020000 recommended)` |
| Migration guide §6 walk | `docs/renderworld_migration_guide.md:218` | mentions `0x00020000` for terrain — needs same update |
| Migration guide §15 cheat | `docs/renderworld_migration_guide.md:632` | mentions `0x00020000` for terrain — needs same update |
| CLAUDE.md insertion | `CLAUDE.md:197-211` (M6 entry) | New entries append AFTER M6 |

All anchors confirmed via Grep/Read at plan write-time. Symbols are
stable; line numbers may drift before execute — re-grep before editing.

## 2. Task 1 — M3+M5 atomic reservation/deferral (single commit)

This is a single task producing a single commit. Steps below are
sequential.

### Step 0 — CMakeCache pin verification

Confirm the worktree build dir is pinned to the worktree source (not the
root-checkout stale build). Per CLAUDE.md "Key paths."

```powershell
Select-String -Path A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\CMakeCache.txt -Pattern "CMAKE_HOME_DIRECTORY|CMAKE_SOURCE_DIR" -SimpleMatch | Select-Object -First 4
```

Expected: both paths point to
`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`. If they
point at the root checkout, STOP and report blocked — the cache must be
regenerated against the worktree.

### Step 1 — Branch check (pre-edit)

```powershell
git rev-parse --abbrev-ref HEAD
```

Expected: `claude/nifty-mendeleev`. If anything else, STOP.

### Step 2 — Edit `RenderWorld/RenderWorld.h` (enum + comment, covers both M3 and M5)

Read `RenderWorld/RenderWorld.h` lines 126-136 first to confirm exact
indentation before editing.

**Existing (`RenderWorld/RenderWorld.h:132-136`):**

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
    // M3 v1 (2026-05-24): RESERVATION ONLY. No writer is wired in v1.
    // See docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md
    // and the resolutions sidecar
    // docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md
    // for the future-trigger contract that would flip M3 to an
    // implementation slice. lookupAtPixel emits a one-shot WARN and
    // returns isValid=false if this kind ever surfaces in a record —
    // that is the trip-wire for an unintended writer. Future terrain
    // variants (water/decal/mine) use a `subKind` payload field, NOT
    // additional RenderObjectKind values.
    Terrain    = 2,
    // Future: Vfx=3 (reserved in M4).
    // Overlay reserved/deferred (M5 2026-05-24): the word "overlay" had
    // 7 in-tree meanings without an identity-needing consumer. See
    // docs/superpowers/specs/2026-05-23-renderworld-slice-m5-overlay-spec.md
    // for the clarification rationale. If a future use case emerges,
    // ship as a new named slice (HoverKindIndicator /
    // RenderWorldDebugOverlay / M5-perf overlay-decal GPU port) — NOT
    // as "M5 Overlay."
};
```

### Step 3 — Edit `RenderWorld/RenderWorld.cpp` — add `kTerrainHandleBase`

Read `RenderWorld/RenderWorld.cpp` lines 117-122 first to confirm
exact indentation before editing.

**Existing (`RenderWorld/RenderWorld.cpp:117-121`):**

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

// kTerrainHandleBase: M3 v1 reservation (2026-05-24). RESERVED — no code
// allocates from this base in v1. If/when a future M3.1 implementation
// slice ships per-quad terrain identity (editor-driven; see resolutions
// sidecar), this is the base. Range [0x00040000, 0x000FFFFF] reserves
// 786,431 slots — comfortably above the worst-case ~196K mission-total
// terrain quads on GameVisibleVertices=200. Leaves
// [0x00010000..0x0003FFFF] (~245K slots) as mech-expansion headroom.
// Tripwire-protected: see lookupAtPixel below. Spec:
// docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md.
[[maybe_unused]] static constexpr uint32_t kTerrainHandleBase = 0x00040000u;
```

### Step 4 — Edit `RenderWorld/RenderWorld.cpp::lookupAtPixel` — add trip-wire branch

Read `RenderWorld/RenderWorld.cpp` lines 760-780 first to confirm the
post-alive-check region (the trip-wire inserts AFTER the alive check at
`:764-766`, BEFORE the `out.isValid = true;` field-population at
`:768`). Line numbers may drift; re-grep `out.isValid            = true;`
to relocate.

**Existing (`RenderWorld/RenderWorld.cpp:759-769`):**

```cpp
    // Generation check: stale pixel (rendered before slot recycle)
    // returns invalid even though the raw value parses to a Handle.
    if (rec.generation != static_cast<uint16_t>(h.generation())) {
        return out;
    }
    if ((rec.flags & kRenderObjectFlagAlive) == 0u) {
        return out;
    }

    out.isValid            = true;
    out.handle             = h;
```

**Replace with:**

```cpp
    // Generation check: stale pixel (rendered before slot recycle)
    // returns invalid even though the raw value parses to a Handle.
    if (rec.generation != static_cast<uint16_t>(h.generation())) {
        return out;
    }
    if ((rec.flags & kRenderObjectFlagAlive) == 0u) {
        return out;
    }

    // M3 v1 trip-wire (2026-05-24): no writer should produce a Terrain
    // record in v1 (kTerrainHandleBase is unused; no terrain frag shader
    // writes attachment-2). If we see one, an unintended writer has
    // slipped in. Log once and downgrade to invalid. Removing this branch
    // is part of the M3.1 implementation slice if it ever ships.
    if (rec.kind == RenderObjectKind::Terrain) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[RENDER_WORLD v1] WARN: lookupAtPixel returned kind=Terrain but no writer should produce it (M3 reservation; M3.1 would change this)\n");
        }
        return out;  // isValid=false (default)
    }

    out.isValid            = true;
    out.handle             = h;
```

### Step 5 — Edit `docs/renderworld_migration_guide.md` §12 handle-base table

Read `docs/renderworld_migration_guide.md` lines 548-560 first to
confirm table structure.

**Existing (`docs/renderworld_migration_guide.md:550-556`):**

```
| Kind        | Base       | Max observed (tier1 mc2_24) | Headroom |
|-------------|------------|-----------------------------|----------|
| StaticProp  | 0          | 2641                        | ~24x to mech base |
| Mech        | 0x00010000 | ~50                         | huge     |
| Terrain     | TBD (0x00020000 recommended) | -- | -- |
| Vfx         | TBD (0x00040000 recommended) | -- | -- |
| Overlay     | TBD (0x00080000 recommended) | -- | -- |
```

**Replace with:**

```
| Kind        | Base       | Max observed (tier1 mc2_24) | Headroom |
|-------------|------------|-----------------------------|----------|
| StaticProp  | 0          | 2641                        | ~24x to mech base |
| Mech        | 0x00010000 | ~50                         | huge     |
| Terrain     | 0x00040000 (M3 v1: RESERVED, no allocator) | -- | 786K slots reserved; [0x10000..0x3FFFF] left as mech-expansion headroom |
| Vfx         | TBD (0x00080000 recommended in M4) | -- | -- |
| Overlay     | DEFERRED indefinitely (M5 2026-05-24); see slice-m5 spec | -- | -- |

**Terrain variants note (M3 v1):** if a future M3.1 ships per-quad terrain
identity (editor-driven), water / decal / mine variants use a `subKind`
payload field on `RenderObjectRecord` — do NOT proliferate
`RenderObjectKind` values for terrain flavors. Per the
`RenderObjectKind` "stable across releases — never renumber, only
append" rule, splitting later costs only an enum append, but the
resolutions sidecar explicitly picks the single-kind path for v1.
```

### Step 6 — Edit `docs/renderworld_migration_guide.md` §6 walk paragraph

The walk paragraph at `:218` currently recommends `0x00020000` for
terrain — update to match the new reserved base.

Read `docs/renderworld_migration_guide.md` lines 213-225 first.

**Existing (`docs/renderworld_migration_guide.md:214-219`):**

```
2.  **Allocate a handle-index base.** Static props live `[0..2641]` (tier1
    mc2_24 max). Mechs live `[0x00010000..]` (`kMechHandleBase = 0x10000`,
    65536; ~24x headroom over the static-prop max). Pick a base with at
    least one decimal-order headroom over the projected max. For terrain
    blocks (a 64x64 grid = 4096 max), `kTerrainHandleBase = 0x00020000`
    keeps it cleanly disjoint from both static props and mechs.
```

**Replace with:**

```
2.  **Allocate a handle-index base.** Static props live `[0..2641]` (tier1
    mc2_24 max). Mechs live `[0x00010000..]` (`kMechHandleBase = 0x10000`,
    65536; ~24x headroom over the static-prop max). Pick a base with at
    least one decimal-order headroom over the projected max. For terrain
    blocks the M3 v1 reservation is `kTerrainHandleBase = 0x00040000`
    (see §12 table); this leaves `[0x00010000..0x0003FFFF]` as
    mech-expansion headroom.
```

### Step 7 — Edit `docs/renderworld_migration_guide.md` §15 cheat sheet

The cheat sheet at `:632` also says `0x00020000` — update to the actual
landed value.

Read `docs/renderworld_migration_guide.md` lines 627-640 first.

**Existing (`docs/renderworld_migration_guide.md:632`):**

```
2.  Pick handle-base (e.g. `kTerrainHandleBase = 0x00020000`).
```

**Replace with:**

```
2.  Pick handle-base (M3 v1 landed `kTerrainHandleBase = 0x00040000`).
```

### Step 8 — Edit `CLAUDE.md` — append M3 SHIPPED entry

Read `CLAUDE.md` lines 195-211 first to confirm the M6 entry boundary
(end of file).

Append the following entry IMMEDIATELY after the M6 entry (after line
211). Use Edit with the M6 entry's closing line as anchor.

**Insert after** (anchor; do NOT replace, just locate):

```
  `docs/superpowers/explorations/2026-05-23-renderworld-slice-m6-firewall-audit-recon.md`.
```

**Append (new lines, immediately after the anchor):**

```
- **RenderWorld Slice M3** (SHIPPED 2026-05-24): terrain reservation/deferral. Adds `RenderObjectKind::Terrain = 2` enum value + `kTerrainHandleBase = 0x40000` constant + defensive `lookupAtPixel` tripwire (warn if `kind=Terrain` ever returned — no writer should produce it). Recon proved GPU terrain identity has no current consumer; CPU `Terrain::worldToTile` already returns tile R/C + type + elevation. Forward-compat: if M3.1 ever ships per-quad terrain identity (editor-driven), use `subKind = Base/Water/Decal/Mine` payload (NOT separate enum values). `Terrain::IsGameSelectTerrainPosition` preserved (ground-click path is canonical). No shaders edited, no adapter, no env var, no consumer. Tier1 5/5 PASS (no source path fires). Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md`. Resolutions: `docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md`.
- **RenderWorld Slice M5** (DEFERRED INDEFINITELY 2026-05-24): "overlay" had 7 in-tree meanings without identity-needing consumers. Enum slot `Overlay` un-reserved (comment-only deferral note in `RenderObjectKind`). If a future use case emerges, ship as a new named slice (HoverKindIndicator / RenderWorldDebugOverlay / M5-perf overlay-decal GPU port — NOT as "M5 Overlay"). Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m5-overlay-spec.md`. Resolutions: `docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md`.
```

### Step 9 — Pre-commit branch re-check

```powershell
git rev-parse --abbrev-ref HEAD
```

Expected: `claude/nifty-mendeleev`. STOP if anything else.

### Step 10 — Full relink build

Per CLAUDE.md "Full relink before deploy" rule: this slice changes
`RenderWorld/RenderWorld.h` (an inlined enum) and
`RenderWorld/RenderWorld.cpp`. Header change forces a relink to be safe.

```powershell
Remove-Item A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 15
```

Expected: build succeeds; final line shows `mc2.vcxproj -> ...mc2.exe`
or similar. If errors, STOP and report.

### Step 11 — Deploy

Per CLAUDE.md "Deploy: NEVER `cp -r`. ALWAYS `cp -f` per file":

```powershell
Copy-Item -Force A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe
```

No shader files edited in this slice; no shader tree redeploy needed
(the lockstep rule does not trigger).

### Step 12 — Tier1 5/5 smoke (env-OFF; trip-wire should NOT fire)

Canonical invocation per CLAUDE.md "Smoke gate":

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: exit 0; 5/5 missions PASS.

### Step 13 — Verify trip-wire did NOT fire

The trip-wire's stderr line is the unique substring
`lookupAtPixel returned kind=Terrain`. It must NOT appear in any
artifact log — if it does, an unintended writer has been introduced and
the slice has a P0 bug.

Find the latest artifact dir and grep:

```powershell
$latest = Get-ChildItem A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts -Directory | Sort-Object Name -Descending | Select-Object -First 1
Get-ChildItem $latest.FullName -Recurse -Filter "*.log" | Select-String -Pattern "lookupAtPixel returned kind=Terrain" -SimpleMatch | Measure-Object | Select-Object -ExpandProperty Count
```

Expected: `0`. Nonzero = STOP, do not commit, report.

### Step 14 — Stage files

```powershell
git add A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.h A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\docs\renderworld_migration_guide.md A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\CLAUDE.md
```

Verify:

```powershell
git diff --cached --stat
```

Expected: 4 files changed (`RenderWorld/RenderWorld.h`,
`RenderWorld/RenderWorld.cpp`, `docs/renderworld_migration_guide.md`,
`CLAUDE.md`). No other files should be in the staging area for this
commit. If extra files leaked in (e.g. `.claude/settings.local.json`,
`mc2srcdata` submodule pointer churn), `git restore --staged` them.

### Step 15 — Commit (single commit covering M3 + M5)

Use a HEREDOC (Bash) per CLAUDE.md commit-format rule.

```bash
git commit -m "$(cat <<'EOF'
feat(RenderWorld): ship M3 (terrain reservation) + M5 (overlay deferral) [atomic]

M3 substantive (terrain reservation/deferral):
- enum: append RenderObjectKind::Terrain = 2 (was a Future:= comment)
- constant: add [[maybe_unused]] kTerrainHandleBase = 0x00040000u
  alongside kMechHandleBase. Range [0x40000..0xFFFFF] reserves 786K
  slots; leaves [0x10000..0x3FFFF] as mech-expansion headroom.
- lookupAtPixel trip-wire: if any record reports kind=Terrain, log
  once and downgrade to isValid=false. No writer should ever produce
  it under M3 v1 (no terrain frag shader writes attachment-2). This
  is the runtime witness for an unintended writer slipping in.
- migration guide section 12 handle-base table updated: 0x40000
  (was TBD 0x20000 recommended). Cheat sheet (section 15) + walk
  paragraph (section 6) updated to match.
- subKind forward-compat note encoded in enum comment + migration
  guide: future terrain variants (water/decal/mine) use a subKind
  payload field, NOT additional RenderObjectKind values.

M5 fold-in (overlay defer-indefinitely):
- enum comment block updated to record the deferral pointer and the
  rescope guidance (HoverKindIndicator / RenderWorldDebugOverlay /
  M5-perf are the future paths if a use case emerges; NOT
  "M5 Overlay"). No code change beyond the comment.

Non-goals (explicit):
- no terrain frag shader edited (the 5 terrain shaders stay
  attachment-0/1 only).
- no adapter, no registerTerrain/destroyTerrain, no per-frame banner
  counter, no env var, no new self-test.
- Terrain::IsGameSelectTerrainPosition preserved (CPU ground-click is
  canonical for movement-target gameplay).

Validation:
- tier1 5/5 PASS env-OFF (no code path fires).
- trip-wire WARN line NOT present in any artifact log (grep gate).
- firewall script unchanged; no new includes; no allowlist update.

Specs:
  docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md
  docs/superpowers/specs/2026-05-23-renderworld-slice-m5-overlay-spec.md
Resolutions sidecar (load-bearing):
  docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md
EOF
)"
```

### Step 16 — Post-commit verification

```powershell
git log -1 --stat
git status
```

Expected: single new commit on `claude/nifty-mendeleev`; working tree
clean for the touched files. Submodule / `.claude/` noise that pre-dates
this session may remain untouched and is fine.

## 3. Rollback

If any step 9+ fails after step 10 (i.e. build/smoke fails post-edit but
pre-commit), revert via working-tree restore:

```powershell
git restore A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.h A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\RenderWorld\RenderWorld.cpp A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\docs\renderworld_migration_guide.md A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\CLAUDE.md
```

Slice is single-commit; if it fails post-commit, `git revert HEAD` is
clean (no dependents land before M4).

## 4. Threat model (per spec §"Threat model")

- **Trap 1 — handle-base collision when M3.1 ships.** Mitigated by the
  migration guide §12 table update (Step 5) + the `kTerrainHandleBase`
  comment in `RenderWorld.cpp` (Step 3). M4's `kVfxHandleBase` resolution
  (`0x00080000` per sidecar) is disjoint.
- **Trap 2 — `lookupAtPixel` returning `kind=Terrain` while M3 v1 is
  shipped.** Mitigated by trip-wire (Step 4) + Step 13 grep gate. Trip-wire
  references the spec by name so a future maintainer hitting the warn
  finds the contract.
- **Trap 3 — silent enum reflection drift.** Per spec, no shader
  consumes `RenderObjectKind` directly, so the shader reflection CI
  (`tools/shader_reflect/reflect.py`) does NOT need golden updates for
  this slice.

## 5. Greybeard ruling

**META-FIX** per spec §"Greybeard analysis — deferral as META-FIX." The
bug class being retired is "every kind reservation lives as a comment
until someone re-derives the slot allocation." By formalizing the
reservation in code (enum + constant + trip-wire) and updating the
migration guide table to the actual landed value, M4's `Vfx` reservation
and any future kind follow the same shape mechanically.

The M5 fold-in is a clarification-only doc deferral; no greybeard ruling
applies (no fix, no code change beyond the comment).

## 6. Pre-execute checklist

- [ ] Branch is `claude/nifty-mendeleev`
- [ ] Build dir CMakeCache pinned to worktree (Step 0)
- [ ] All 4 edited files exist at the expected paths and current line
      anchors match (re-grep before each Edit)
- [ ] No other in-flight slice on the same branch (`git status` clean
      for the 4 target files)
- [ ] User has approved the slice scope per resolutions sidecar (sidecar
      itself IS the approval)

---

PLAN STATUS: READY FOR EXECUTE

## Commit-message-ready summary

```
feat(RenderWorld): ship M3 (terrain reservation) + M5 (overlay deferral) [atomic]

M3 substantive: RenderObjectKind::Terrain = 2, kTerrainHandleBase =
0x40000 ([[maybe_unused]]), defensive lookupAtPixel trip-wire (WARN +
downgrade-to-invalid if kind=Terrain ever surfaces). Migration guide
section 12 handle-base table + section 6 walk + section 15 cheat sheet
updated to the landed 0x40000. subKind forward-compat note encoded.
No shader, no adapter, no env var; CPU Terrain::worldToTile +
IsGameSelectTerrainPosition preserved as canonical terrain interaction.

M5 fold-in: enum comment block records overlay defer-indefinitely (7
in-tree meanings of "overlay" without identity-needing consumers).
Future paths if a use case emerges are HoverKindIndicator /
RenderWorldDebugOverlay / M5-perf overlay-decal GPU port — NOT
"M5 Overlay."

Specs:
  docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md
  docs/superpowers/specs/2026-05-23-renderworld-slice-m5-overlay-spec.md
Resolutions: docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md
Tier1 5/5 PASS env-OFF; trip-wire WARN not present in any artifact log.
```
