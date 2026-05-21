# MLR Work-Leaf Audit (v3 round-3 pre-impl)

Sibling artifact to `2026-05-20-integrated-gosfx-retirement-gpu-particles-plan.md`.
Drives the A1 gate placement per the v3 user decision: **gate ALL MLR
work-leaves, not just `Effect::Draw`**. Round-2 review found `DrawEffect`
as a second leaf; this round-3 audit goes exhaustive over `mclib/mlr/`.

Grep-verified at write-time against HEAD `2c68c8f`.

## 1. Audit criteria

A function in `mclib/mlr/` is a "work-leaf" if its body satisfies ANY of:

- Reads `worldToClipMatrix` directly
- Reads `cameraToClip` directly (the F3 attribution `cameraToClip(2,2)` pattern)
- Calls `TransformAndClip` (the primitive-level clip operation)
- Calls `sorter->AddPrimitive`, `sorter->DrawPrimitive`, `sorter->AddEffect`,
  or `sorter->AddScreenQuads` (the four sorter-accumulation entry points;
  full set per `mlrsorter.hpp:117-119`)
- Reads any per-frame projection-derived data (`worldToCameraMatrix`,
  `farClipReciprocal`, etc.)

**(v4 round-3 review fold-in):** v3 criteria list was missing
`sorter->AddEffect` and `sorter->AddScreenQuads`. Adversarial review
found the cloud `Draw` implementations (`mlrcardcloud.cpp:158`,
`mlrtrianglecloud.cpp:131`, `mlrlinecloud.cpp:98`, etc.) call
`sorter->AddEffect`. These are structurally guarded by
`MLRClipper::DrawEffect` being gated (cloud Draws are reached ONLY via
`DrawEffect → dInfo->effect->Draw`); no new gate site required. The
criteria are completed for predicate-completeness rather than to add
gates. `AddScreenQuads` is called only from `MLRClipper::DrawScreenQuads
:745`, also structurally guarded.

## 2. Inventory

Grep over `mclib/mlr/` for the criteria predicates produced 31 file hits
(see §6 raw grep). Headers are template declarations; the actual function
bodies live in 4 places:

### 2.1 `mclib/mlr/mlrclipper.cpp` — the front-door entry points

| Function | Defn line | Criteria hit |
|---|---|---|
| `MLRClipper::StartDraw` | `:117` | Builds `worldToCameraMatrix`/`worldToClipMatrix`; calls `sorter->StartDraw`; reads `cameraToClip(2,2)` at `:207` (THE F3 site) |
| `MLRClipper::DrawShape` | `:400` | Reads `worldToClipMatrix` `:420`; `TransformAndClip` `:508`; `sorter->DrawPrimitive` `:514`, `:526` |
| `MLRClipper::DrawScalableShape` | `:565` | Reads `worldToClipMatrix` `:575, :591, :595`; `TransformAndClip` `:623`; `sorter->DrawPrimitive` `:629, :648`; `sorter->AddPrimitive` `:633, :652` |
| `MLRClipper::DrawEffect` | `:668` | Reads `worldToClipMatrix` `:685` (via `SetEffectToClipMatrix`); delegates to `dInfo->effect->Draw(...)` which dispatches to `MLR*Cloud::Draw` (§2.3) |
| `MLRClipper::DrawScreenQuads` | `:697` | Uses sorter accumulation downstream; reads viewport |
| `MLRClipper::Clear` | `:755` | Bookkeeping only — **NOT a work-leaf** |

### 2.2 `mclib/mlr/mlrsortbyorder.cpp` — sorter flush (downstream of front door)

| Function | Defn line | Notes |
|---|---|---|
| `MLRSortByOrder::RenderNow` | `:227` (reads `farClipReciprocal` `:239`; `TransformAndClip` `:325, :395`) | **NOT a work-leaf for gating purposes** — runs over the sorter's accumulated TBDPs; if the front-door leaves accumulate nothing, this loop iterates over zero. Structurally guarded by §2.1 gating. |
| `MLRSortByOrder::AddEffect` | `:125` | Sorter-accumulation entry; called ONLY from cloud `Draw` (`mlr*cloud.cpp`) which are reached only via gated `MLRClipper::DrawEffect`. Structurally guarded. |
| `MLRSortByOrder::AddScreenQuads` | `:185` | Sorter-accumulation entry; called ONLY from gated `MLRClipper::DrawScreenQuads :745`. Structurally guarded. |

### 2.3 `MLREffect::Draw` virtual chain — per-effect Draw implementations

Called only from `MLRClipper::DrawEffect` `:687` via
`dInfo->effect->Draw(dInfo, &allVerticesToDraw, sorter)`. Includes:

- `MLRCardCloud::Draw` (`mlrcardcloud.cpp:144`)
- `MLRIndexedTriangleCloud::Draw` (`mlrindexedtrianglecloud.cpp:123`)
- `MLRLineCloud::Draw` (`mlrlinecloud.cpp:84`)
- `MLRNGonCloud::Draw` (`mlrngoncloud.cpp:122`)
- `MLRPointCloud::Draw` (`mlrpointcloud.cpp:80`)
- `MLRTriangleCloud::Draw` (`mlrtrianglecloud.cpp:117`)

**NOT gated directly** — gating `MLRClipper::DrawEffect` short-circuits
the dispatch BEFORE `effect->Draw` runs. These are structurally guarded.

### 2.4 `mclib/mlr/gosvertex.cpp` — static `farClipReciprocal` definition

Pure storage. Not a work-leaf.

## 3. Caller cross-reference (gosFX → mlr/)

All `theClipper->` callers in the codebase (grep `theClipper->`, all
source trees):

| Site | Purpose |
|---|---|
| `code/gamecam.cpp:148` | `theClipper->StartDraw(...)` — per-frame setup, **outer Tracy scope** |
| `code/gamecam.cpp:287` | `theClipper->RenderNow()` — per-frame flush, **outer Tracy scope** |
| `code/simplecamera.cpp:168` | `theClipper->StartDraw(...)` — editor/sim camera variant |

All `info->m_clipper->Draw*` callers (grep `info->m_clipper->`, all source):

| Site | Method | Count |
|---|---|---|
| `mclib/gosfx/card.cpp:547` | DrawEffect | 1 |
| `mclib/gosfx/cardcloud.cpp:873` | DrawEffect | 1 |
| `mclib/gosfx/pertcloud.cpp:678` | DrawEffect | 1 |
| `mclib/gosfx/pointcloud.cpp:476` | DrawEffect | 1 |
| `mclib/gosfx/shardcloud.cpp:659` | DrawEffect | 1 |
| `mclib/gosfx/tube.cpp:1282` | DrawEffect | 1 |
| `mclib/gosfx/shape.cpp:317` | DrawScalableShape | 1 |
| `mclib/gosfx/shapecloud.cpp:394, :459, :525, :568` | DrawScalableShape | 4 |
| `mclib/gosfx/debriscloud.cpp:851` | DrawScalableShape | 1 |

**External callers of `DrawShape` and `DrawScreenQuads`: zero.** Grep of
all source trees outside `mclib/mlr/` returned no hits. These two leaves
have no live entry path; gating them is defensive coverage only.

**No non-gosFX caller of any `MLRClipper::Draw*` exists.** Confirms the
MEMORY.md census `architecture_md_stale_objectmanager_via_mlr.md`: gosFX
is the SOLE live MLR consumer.

## 4. Gate placement decision per leaf

Per user decision Step 2:

| Leaf | Gate kind | Rationale |
|---|---|---|
| `DrawShape` | Standard early-return at function entry | No external callers; caller list empty so trivially side-effect safe |
| `DrawScalableShape` | Standard early-return at function entry | Callers in §3 don't read post-state (verified §5) |
| `DrawEffect` | Standard early-return at function entry | Callers in §3 don't read post-state (verified §5) |
| `DrawScreenQuads` | Standard early-return at function entry | No external callers; defensive |
| `StartDraw` | **NOT GATED** (outer scope) | Per user Step 2: gate inner leaves only. Preserves `mlr_total` Tracy zone integrity for post-A2 measurement. |
| `RenderNow` | **NOT GATED** (outer scope) | Same. Iterates over empty sorter when front-door gates fire. |
| `Clear` | **NOT GATED** | Not a work-leaf; bookkeeping only. |

Also **NOT GATED at `gosFX::Effect::Draw`** (v2's placement). The v3
placement moves the gate one layer down into `mclib/mlr/` so that:

- Any future MLR consumer (none today, but defensive) is also gated.
- The `mlr_total` worst-window measurement at A2 reflects gate efficacy
  at the actual work site, not at the gosFX dispatch.
- A4 deletes `mclib/mlr/` wholesale; the gates self-delete with the
  tree-deletion commit (no orphan gate code).

## 5. Side-effect audit (Step 4)

For each gated leaf, confirm early-return is safe:

### `DrawShape`
- No external callers. Trivially safe.

### `DrawScalableShape`
Callers: `gosFX::Shape::Draw` (`shape.cpp:317`), `gosFX::ShapeCloud::Draw`
(4 sites), `gosFX::DebrisCloud::Draw` (`debriscloud.cpp:851`). All
callsites match the pattern:

```cpp
info->m_clipper->DrawScalableShape(&dinfo);
Singleton::Draw(info);   // parent class bookkeeping; doesn't read post-clipper state
```

No caller reads return value (function is `void`), reads sorter state,
queries the clipper, or queries `dInfo` for post-draw mutations. The
parent `Singleton::Draw(info)` call is gosFX-internal lifecycle/age
tracking that does NOT depend on whether the draw geometry made it to
the sorter. Safe.

### `DrawEffect`
Callers: 6 gosFX cloud types, identical pattern to DrawScalableShape:
fire-and-forget. The `dInfo->effect->Draw` dispatch happens INSIDE
DrawEffect (line `:687`); early-return at entry prevents that dispatch
but the caller doesn't care. No counter or sorter query downstream.
Safe.

### `DrawScreenQuads`
No external callers. Safe.

**Result: all four work-leaves are simple early-return safe.** No leaf
requires side-effect-preserving gate complexity. **No STOP condition.**

## 6. Raw grep evidence

```
$ grep -rEn 'worldToClipMatrix|cameraToClip\b|TransformAndClip|sorter->(AddPrimitive|DrawPrimitive)|worldToCameraMatrix|farClipReciprocal' mclib/mlr/
```

31 files matched (full hit list in tool log). All non-trivial hits resolved
into either §2.1 (front-door MLRClipper methods) or §2.2 (sorter flush,
structurally guarded). Template-header hits (`*.hpp`) are inline declarations
inherited from `MLRPrimitiveBase`, not standalone work bodies.

```
$ grep -rn 'theClipper->' --include='*.cpp' --include='*.hpp' --include='*.h' code/ mclib/ GameOS/ editor/ Viewer/ aseconv/
code/gamecam.cpp:148:        theClipper->StartDraw(...)
code/gamecam.cpp:287:        theClipper->RenderNow();        //Draw the FX
code/simplecamera.cpp:168:        theClipper->StartDraw(...)
```

```
$ grep -rEn 'DrawShape|DrawScalableShape|DrawEffect|DrawScreenQuads' mclib/gosfx/
[16 callsites, all matching §3 table]
```

```
$ grep -rEn 'DrawScreenQuads|->DrawShape\b' code/ mclib/ GameOS/ editor/ Viewer/ aseconv/ | grep -v 'mclib/mlr/'
[zero hits]
```

## 7. Compile-time enumeration mechanism (Step 3)

Two options were considered:

**Option A: registry array + static_assert in header.** Add
`mclib/mlr/mlr_work_leaves.h` with a `constexpr` array of gated function
names and `MLR_WORK_LEAF_GATE()` macro that registers each gate at
definition. Pro: build-time enforcement. Con: requires touching every
gated function body plus a header-discipline ritual; the symbol list
duplicates the gate sites.

**Option B: shell check script** (`scripts/check-mlr-leaves-gated.sh`),
following the existing `scripts/check-destroy-invariant.sh` /
`check-particles-no-cpu-projection.sh` pattern. Greps `mclib/mlr/` for
the work-leaf criteria predicates; for each function body that matches,
greps that body for the gate macro `MC2_GOSFX_GATE_EARLY_RETURN();`
(or whatever sentinel A1 uses). Any function that matches the predicates
but lacks the sentinel = violation, script exits nonzero, hooked into
the `gsd-staleness-monitor` Stop hook + pre-commit invariant list
(CLAUDE.md "Pre-commit invariant scripts").

**Decision: Option B.** Rationale:

- Matches the existing pattern (`check-destroy-invariant.sh`,
  `check-asset-scale-callers.sh`, `check-particles-no-cpu-projection.sh`
  per (B) §3.4). One more script in the same shape.
- No header-structure invasion. Gates are still standard early-returns;
  the script enforces presence rather than the type system.
- Per `feedback_data_flow_audit_asymmetry.md`: don't trust prose
  discipline alone — the script IS the discipline.
- A4 deletes `mclib/mlr/` entirely, including this script's target
  directory. The script self-retires when its scan range goes empty;
  the script itself can be deleted in the A4 commit.

### Script outline (to be authored in A1 Commit 2)

```sh
#!/bin/sh
# scripts/check-mlr-leaves-gated.sh
# Enforces: every work-leaf in mclib/mlr/ carries MC2_GOSFX_GATE_EARLY_RETURN()
# at function entry. Work-leaf = function body matching the criteria in
# docs/superpowers/plans/2026-05-20-integrated-gosfx-retirement-INSTRUMENTATION-AUDIT-v3.md §1.

set -e
violations=0

# Canonical leaf list from audit §2.1 §4 (gated set).
LEAVES='MLRClipper::DrawShape MLRClipper::DrawScalableShape MLRClipper::DrawEffect MLRClipper::DrawScreenQuads'

for leaf in $LEAVES; do
    fn=${leaf##*::}
    body=$(awk -v fn="$fn" '
        /^[[:space:]]*MLRClipper::'"$fn"'[[:space:]]*\(/,/^}/
    ' mclib/mlr/mlrclipper.cpp)
    if ! printf '%s\n' "$body" | grep -q 'MC2_GOSFX_GATE_EARLY_RETURN'; then
        echo "[INVARIANT] $leaf missing MC2_GOSFX_GATE_EARLY_RETURN()"
        violations=1
    fi
done

# Defense: detect new MLR functions reading projection state that aren't in LEAVES.
# (v4: predicate completed to include AddEffect / AddScreenQuads per §1.)
NEW=$(grep -rEn 'worldToClipMatrix|TransformAndClip|sorter->(AddPrimitive|DrawPrimitive|AddEffect|AddScreenQuads)' \
    mclib/mlr/*.cpp \
    | grep -v 'mlrclipper.cpp' \
    | grep -v 'mlrsortbyorder.cpp' \
    | grep -v 'gosvertex.cpp' \
    | grep -v 'mlrcardcloud.cpp\|mlrindexedtrianglecloud.cpp\|mlrlinecloud.cpp\|mlrngoncloud.cpp\|mlrpointcloud.cpp\|mlrtrianglecloud.cpp' \
    || true)
if [ -n "$NEW" ]; then
    echo "[INVARIANT] new MLR projection-reading function detected — add to LEAVES + gate it:"
    echo "$NEW"
    violations=1
fi

[ "$violations" -eq 0 ] && echo "OK"
exit $violations
```

The whitelist of "known structurally-guarded files" (sorter flush, cloud
Draw children) makes the "added new leaf" trip-wire actionable: any new
.cpp in `mclib/mlr/` that reads `worldToClipMatrix` etc. trips the script
unless explicitly whitelisted, forcing a deliberate audit-update decision.

## 8. Round-3 history

- **Round 1:** original v1 plan, gate at producer sites (16 sites). Adversarial
  review rejected — too many touch points.
- **Round 2:** v2 MINOR-1 fold-in, single gate inside `gosFX::Effect::Draw`.
  Adversarial pre-impl re-audit found `MLRClipper::DrawEffect` reads
  `worldToClipMatrix` directly via `SetEffectToClipMatrix` AND
  `MLRClipper::DrawScalableShape` is a second front-door leaf that v2's
  `Effect::Draw` gate would have missed entirely (shape.cpp:317 dispatches
  into DrawScalableShape, NOT Effect::Draw — different path).
- **Round 3 (this audit):** exhaustive `mclib/mlr/` sweep finds 4 front-door
  leaves. Gate all 4. CI script enforces.

## 9. Status

- All 4 leaves are simple early-return safe (no STOP condition from §5).
- Compile-time enumeration mechanism chosen: shell check script (Option B).
- v3 plan rewrite owns A1 §2.3 / §2.7 fold-in of this audit.

## 10. Round-3 review fold-in (v4 deltas)

Inline adversarial review of v3 produced 0 CRITICAL, 1 MAJOR, 2 MINOR:

- **MAJOR (predicate completeness):** v3 §1 criteria list missing
  `sorter->AddEffect` / `sorter->AddScreenQuads`. No new gate site
  required (both structurally guarded by §2.1 leaves) but criteria
  list and CI script predicate were both completed for correctness.
  Folded into §1 + §2.2 (RenderNow row plus two new rows) + §7 script
  outline (predicate completed).
- **MINOR (citation drift):** v3 §2.2 said `MLRSorter::RenderNow (or
  analog) :239+`. Actual: `MLRSortByOrder::RenderNow` at `:227`
  (`:239` is the inner `farClipReciprocal` read). Folded into §2.2.
- **MINOR (layer smell):** `MC2_DISABLE_GOSFX`-named gate plumbing
  lives inside `mclib/mlr/`. Semantic mismatch (mlr is below gosFX in
  the layer cake). A4 retires both trees simultaneously, so this is
  cosmetic at worst. NOT folded — accepted as transitional.

Round 3 verdict: PASS (≤2 MAJOR mechanical, 0 CRITICAL).
**Executor-ready.**
