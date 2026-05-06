# Static-Prop Registry — Engine-Side Material Classification Hardening

> **Status:** Implemented (rev 3.3 source); SCOPE CLARIFIED (rev 3.4) — does NOT close the visible black-billboard symptom; that's a different bug class (texture-cache eviction; see `pause_unpause_diagnostic_for_static_render_bugs.md` and the new-session handoff at `2026-05-06-static-prop-texture-cache-handoff.md`). This spec ships as **Track B prep groundwork only.**
> **Branch label:** *Track B prep: static-prop material classification hardening*
> **Scope:** Closes the **registration-time alpha-test material-classification gap**: load paths that load alpha-test-needing TG_TypeShapes without calling `SetAlphaTest(true)` (e.g., `treeDmgShape`/`treeDmgShadowShape`, `GenericAppearance` props) leave their recipes with `pkt.materialFlags=0`. Path 4 detects the `a_`/`A_` asset-name prefix at register-time and OR's in the flag. **Does NOT fix the `MC2_STATIC_UPDATE_SKIP=1` visible black-billboard regression** — pause/unpause diagnostic at user-test 2026-05-06 confirmed that symptom is texture-cache eviction in `mcTextureManager->update()` not getting re-cached because `touch()` substitution doesn't fire `SetTextureHandle`. See the handoff doc.
> **Predecessor:** This bug investigation
> (recharacterization of `2026-05-06-update-skip-touch-regression-handoff.md`).
> **Successor (parallel track):** Track B implementation. This branch is
> *Track B prep*, not *Track B partial implementation* — Track B widens the
> registry to all world-static-prop geometry; this branch makes the
> registration payload more correct regardless of when registration runs.

---

## Framing

This spec sits at the engine/content layer boundary documented in
`memory/open_rts_engine_framing.md`. It moves a load-bearing rendering
property (alpha-test enablement) from "gameplay code must call
`SetAlphaTest(true)`" to "engine reads asset naming convention." Aligns
with two project memories:

- `mc3_modernization_philosophy.md` — engine modernization defaults to GPU
  / modern patterns.
- `open_rts_engine_framing.md` — engine and gameplay are separable layers;
  engine learns about content via asset properties, not via gameplay-side
  function calls.

This branch is also **Track B preparatory groundwork.** Per the
`docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md`
brainstorm:

- Q4 — Track B uses mission-load bulk register + register-on-spawn API,
  rejecting lazy first-render registration long-term.
- Q6 — first-frame registry behavior becomes structural, not lazy.

This branch strengthens both decisions by making the registration *payload*
correct regardless of registration *timing*. Track B's mission-load
ordering will solve some fragility; it will *not* fix load paths that
never set `alphaTestOn=true`. Path 4 (this spec) does.

---

## Bug as characterized

### Visual symptom

`MC2_STATIC_UPDATE_SKIP=1` produces solid-black quads for ~20% of
in-view alpha-test-needing static props (trees, fences, foliage,
scattered decoratives). Buildings unaffected. Pattern is partially
visible in baseline (UPDATE_SKIP=0) for ~0.5% of actors at cull-frustum
edges; UPDATE_SKIP=1 amplification moves the problem from "rare edge
case" to "widespread immediate." User-confirmed visually across
multiple smoke runs.

### Root cause — registration-time material classification is under-specified

**Important reframing from earlier draft.** The earlier hypothesis was
"registration timing race." That was wrong. `TreeAppearance::init`'s
`registerMultiShape` IS correctly ordered after `TreeAppearanceType::init`'s
`SetAlphaTest(true)` — the 33 typeIDs that successfully register with
`alphaTestOn=1` are exactly the tree-LOD shapes whose
`TreeAppearanceType::init` completed `SetAlphaTest(true)` before the
first `TreeAppearance::init` triggered `registerMultiShape`.

**The actual root cause:** non-tree code paths and tree damage variants
load alpha-test-needing assets *without* calling `SetAlphaTest(true)`.
Specifically:

- `treeDmgShape` and `treeDmgShadowShape` at
  [`bdactor.cpp:3438`](../../../mclib/bdactor.cpp:3438) and
  [`bdactor.cpp:3454`](../../../mclib/bdactor.cpp:3454) are loaded via
  `LoadTGMultiShapeFromASE` with no subsequent `SetAlphaTest` call.
  Negative-claim grep verified: `grep "SetAlphaTest" mclib/bdactor.cpp`
  returns 3 hits, none reference `treeDmg*`.
- `GenericAppearance` shapes (fences, scattered props) — same pattern.
- Likely additional load paths I haven't audited.

The 38 permanent-black typeIDs in the diagnostic smoke are owned by **36
distinct multi-shapes** (cross-referenced via `[REG_MULTI v1]` events).
No single load-site patch (Path 3) covers them all. They span multiple
appearance classes; "find every site missing `SetAlphaTest` and add the
call" would be a brittle whack-a-mole sweep.

### How the broken flag chain produces black quads

1. `registerType` at
   [`gos_static_prop_batcher.cpp:542`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:542)
   captures `typeShape->alphaTestOn` synchronously into
   `pkt.materialFlags` at line **641**. This is the SOLE source of
   `STATIC_PROP_FLAG_ALPHA_TEST` at registration time.
2. The shader at
   [`shaders/static_prop.frag:30,51`](../../../shaders/static_prop.frag:30)
   gates `tex_color.a < 0.5` discard on
   `(u_materialFlags & ALPHA_TEST_BIT) != 0`. With the bit clear, no
   discard fires; the foliage texture's transparent (typically
   opaque-black in the source asset) regions render as solid black.
3. There IS a draw-time rescue at
   [`gos_static_prop_batcher.cpp:1663`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:1663)
   that re-OR's in `STATIC_PROP_FLAG_ALPHA_TEST` if
   `src->listOfTextures[pkt.textureSlot].textureAlpha` is true at draw
   time. It rescues ~56% of cases. **It does NOT rescue the ~20% case
   where `pkt.textureSlot`'s `textureAlpha` is also false.** Those 38
   typeIDs (in this smoke) emit `effective=0x0` for every draw and
   render permanently black.

---

## Proposal

### Component 1 — Path 4: register-time material classification from asset name

**Add a small helper near the batcher-local utilities:**

```cpp
// File: gos_static_prop_batcher.cpp (file-scope, near line 25)
static bool isAlphaTestTextureName(const char* name) {
    // The "a_" prefix is the established asset-naming convention for
    // alpha-test (cutout) textures, used at 27 sites across mclib for
    // gating gos_Texture_Alpha vs gos_Texture_Solid loading. Engine reads
    // the same convention to derive STATIC_PROP_FLAG_ALPHA_TEST.
    return name &&
           (name[0] == 'a' || name[0] == 'A') &&
           name[1] == '_';
}
```

**Change `registerType`'s signature to take the parent multi-shape:**

The existing signature `registerType(TG_TypeShape*)` does not have access
to texture names — `TG_TypeShape::listOfTextures` is `TG_TinyTexturePtr`,
which has only `{mcTextureNodeIndex, gosTextureHandle, textureAlpha}` (no
`textureName` field; verified at [`mclib/tgl.h:339-345`](../../../mclib/tgl.h:339)).
Texture names live on the parent `TG_TypeMultiShape::listOfTextures`
(`TG_TexturePtr`, [`mclib/msl.h:77`](../../../mclib/msl.h:77)).

Two clean options for accessing the parent's name array; this spec picks
**Option 1** (signature change):

```cpp
// gos_static_prop_batcher.cpp:542
void GpuStaticPropBatcher::registerType(TG_TypeShape* typeShape,
                                        TG_TypeMultiShape* multiShape);
```

`registerType` is called from one place only — `registerMultiShape` at
[`gos_static_prop_batcher.cpp:690`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:690).
The signature change is internal; no public API touched.

**Implementer note:** the header declaration at
[`gos_static_prop_batcher.h:105`](../../../GameOS/gameos/gos_static_prop_batcher.h:105)
must be updated to match. The single caller at
[`.cpp:690`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:690) needs
to pass the multi-shape: `registerType(typeShape, multiShape);`. The
`multiShape` variable is the parameter of the enclosing `registerMultiShape`
function (in scope at line 690).

(Rejected alternative: add `MC_TextureManager::getNodeName(DWORD nodeId)`
public accessor and look up names via `mcTextureManager` from inside
`registerType`. Would work, but adds a public API surface for what
should be a private internal cross-reference. Filed in §"Implementation
options considered" below.)

**Inside `registerType`, at the per-packet flag-bake site
([line 641](../../../GameOS/gameos/gos_static_prop_batcher.cpp:641)):**

```cpp
// Path 4: register-time material classification from texture name.
// Per-pkt.textureSlot (matches the existing draw-time rescue at line 1663).
// Engine reads the asset naming convention directly; gameplay-side
// SetAlphaTest(true) calls remain valid but are no longer load-bearing.
char texNameBuf[256];
texNameBuf[0] = '\0';
if (multiShape && pkt.textureSlot < multiShape->GetNumTextures()) {
    multiShape->GetTextureName(pkt.textureSlot, texNameBuf, sizeof(texNameBuf));
}
const bool alphaByName = isAlphaTestTextureName(texNameBuf);

pkt.materialFlags = 0;
const char* flagSource = "none";
if (typeShape->alphaTestOn && alphaByName)        flagSource = "alphaTestOn,textureName";
else if (typeShape->alphaTestOn)                  flagSource = "alphaTestOn";
else if (alphaByName)                             flagSource = "textureName";

if (typeShape->alphaTestOn || alphaByName) {
    pkt.materialFlags |= STATIC_PROP_FLAG_ALPHA_TEST;
}

// [REG_TYPE v2] event=pkt — per-packet flag-set evidence, fires inside
// the loop. Distinct from the per-type [REG_TYPE v2] print at line 665
// (which fires once after the loop closes with aggregate counts). Both
// streams are useful: per-type is the canonical "this type registered"
// record; per-pkt resolves which slot triggered the flag.
if (s_regTypeTrace) {
    printf("[REG_TYPE v2] event=pkt typeID=%u pkt=%u slot=%u "
           "alphaTestOn=%d alphaByName=%d flagSource=%s "
           "materialFlags=0x%x\n",
           newTypeID, (unsigned)packetCountForThisType,
           (unsigned)pkt.textureSlot,
           (int)typeShape->alphaTestOn, (int)alphaByName, flagSource,
           pkt.materialFlags);
    fflush(stdout);
}
```

The existing per-type `[REG_TYPE v2]` print at
[`gos_static_prop_batcher.cpp:665-672`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:665)
stays unchanged. Operators get two trace streams:
- `[REG_TYPE v2] frame=N typeShape=P typeID=N alphaTestOn=N numTris=N packets=N` — per-type, fires once after loop closes (existing).
- `[REG_TYPE v2] event=pkt typeID=N pkt=N slot=N alphaTestOn=N alphaByName=N flagSource=X materialFlags=0xN` — per-pkt, fires inside loop (NEW).

**Critical narrowing per advisor:** detection is **per-`pkt.textureSlot`**,
not "any texture in type." This mirrors the draw-time rescue's slot-specific
behavior. If a tree has a trunk-texture packet (slot 0, opaque, no `a_`)
and a leaves-texture packet (slot 1, alpha-test, `a_` prefix), each
packet's `pkt.materialFlags` reflects only its own slot. No false
positives forcing alpha-test on opaque packets.

(Future-stage fallback: if a smoke turns up cases where the alpha asset
lives on a sibling slot while the draw packet's `textureSlot` points at
the opaque slot, document it with trace evidence and consider widening to
"any texture in type." Out of scope for this spec.)

### Component 2 — Forward-compat trap (printf-only, env-gated, **per-type**)

After the per-pkt loop closes (i.e., adjacent to the existing
[per-type `[REG_TYPE v2]` print at line 665](../../../GameOS/gameos/gos_static_prop_batcher.cpp:665)),
walk the type's full slot range to detect "type has at least one `a_`
texture but `alphaTestOn=false`" — the audit-driving condition.

**Why per-type, not per-pkt:** Component 4 (audit follow-up) needs to
identify *load sites* missing `SetAlphaTest`. A type whose trunk-pkt has
no `a_` and whose leaves-pkt has `a_` would *miss* a per-pkt warn on the
trunk-pkt even though the load site clearly needs `SetAlphaTest(true)`.
Per-type warn (any slot has `a_` AND `alphaTestOn=0`) closes that gap.
The per-pkt flag-set logic in Component 1 stays per-pkt for false-positive
safety; this warn is a separate per-type concern.

```cpp
// After the per-pkt loop closes; just before / alongside the existing
// per-type [REG_TYPE v2] print at line 665.
if (s_regTypeTrace) {
    bool typeHasAlphaTextureName = false;
    if (multiShape) {
        const long ntx = multiShape->GetNumTextures();
        char buf[256];
        for (long s = 0; s < ntx; ++s) {
            buf[0] = '\0';
            multiShape->GetTextureName((DWORD)s, buf, sizeof(buf));
            if (isAlphaTestTextureName(buf)) {
                typeHasAlphaTextureName = true;
                break;
            }
        }
    }

    // Existing per-type print (unchanged shape; consumers continue working):
    printf("[REG_TYPE v2] frame=%u typeShape=%p typeID=%u alphaTestOn=%d "
           "numTris=%u packets=%u typeHasAlphaTextureName=%d\n",
           g_mc2FrameCounter, (void*)typeShape, newTypeID,
           (int)typeShape->alphaTestOn, numTris,
           (unsigned)packetCountForThisType,
           (int)typeHasAlphaTextureName);
    fflush(stdout);

    // Per-type forward-compat trap: any slot has a_ AND alphaTestOn==0.
    // This is the canonical Component 4 audit signal — surfaces load sites
    // that need SetAlphaTest, regardless of which slot the draw packets target.
    if (typeShape->alphaTestOn == 0 && typeHasAlphaTextureName) {
        printf("[REG_TYPE v2] event=warn typeShape=%p typeID=%u "
               "reason=type_has_a_prefix_but_no_SetAlphaTest\n",
               (void*)typeShape, newTypeID);
        fflush(stdout);
    }
}
```

Per user direction: **printf-only, env-gated, no hard assert.**
Stock-install constraint takes precedence
(`memory/stock_install_must_remain_playable.md`). Track E (legacy
retirement, post-soak) is the right venue to consider promoting this to
an assertion once the convention is proven stable.

**Trace surface, summarized:**
- `[REG_TYPE v2] frame=N typeShape=P typeID=N alphaTestOn=N numTris=N packets=N typeHasAlphaTextureName=N` — per-type, fires once per type at register time. Existing line, extended with `typeHasAlphaTextureName`.
- `[REG_TYPE v2] event=pkt typeID=N pkt=N slot=N alphaTestOn=N alphaByName=N flagSource=X materialFlags=0xN` — per-pkt, fires inside the loop. NEW (Component 1).
- `[REG_TYPE v2] event=warn typeShape=P typeID=N reason=type_has_a_prefix_but_no_SetAlphaTest` — per-type, fires only when audit condition triggers. NEW (this component).

**Operator-awareness note for Component 4 audit:** because
`TG_TypeMultiShape::SetAlphaTest(true)` propagates to ALL leaves of the
multi-shape (see `mclib/msl.h:214-218`), a multi-shape with N leaves
where the load site missed `SetAlphaTest` will emit N warn lines (one
per leaf at register time), all pointing to the same load-site fix
(`multiShape->SetAlphaTest(true)` at the originating bdactor/genactor/etc.
load site). The audit signal is correct (it identifies the multi-shape
needing the fix) but the line count over-reports by leaf-count. When
walking warn lines, dedupe by `typeShape*` ownership chain back to the
originating multi-shape, OR accept the inflation and trust each fix
collapses N warns to zero.

### Component 3 — `effectiveSource` field on the existing `[ALPHA_TEST]` trace

At the existing draw-time rescue
([`gos_static_prop_batcher.cpp:1663-1671`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:1663)),
extend the existing `[ALPHA_TEST]` print to record which mechanism set
the alpha-test bit at draw time. Distinguish four cases (not three —
collapsing "both fired" into `"packet"` loses the audit signal that
Track E needs to decide whether the rescue is still load-bearing
post-Path-4):

```cpp
// PRESERVE the existing init line at gos_static_prop_batcher.cpp:1667:
uint32_t effectiveMaterialFlags = pkt.materialFlags;

const bool packetHadFlag = (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0;
bool rescueWouldFire = false;
if (src && src->listOfTextures && pkt.textureSlot < src->numTextures &&
    src->listOfTextures[pkt.textureSlot].textureAlpha) {
    effectiveMaterialFlags |= STATIC_PROP_FLAG_ALPHA_TEST;
    rescueWouldFire = true;
}

const char* effectiveSource;
if (packetHadFlag && rescueWouldFire)         effectiveSource = "packet+textureAlphaRescue";
else if (packetHadFlag)                       effectiveSource = "packet";
else if (rescueWouldFire)                     effectiveSource = "textureAlphaRescue";
else                                          effectiveSource = "none";

ALPHA_TRACE("draw type=%u pkt=%u slot=%u pktFlags=0x%x effective=0x%x "
            "texAlpha=%d effectiveSource=%s",
            typeID, p, pkt.textureSlot, pkt.materialFlags, effectiveMaterialFlags,
            (src && src->listOfTextures && pkt.textureSlot < src->numTextures)
                ? (int)src->listOfTextures[pkt.textureSlot].textureAlpha : -1,
            effectiveSource);
```

The four `effectiveSource` values:
- `packet+textureAlphaRescue` — Path 4 set the flag AND the textureAlpha rescue would also have fired. Both safety nets active. The redundant case.
- `packet` — Path 4 set the flag, rescue would NOT have fired. Path 4 is load-bearing for this draw.
- `textureAlphaRescue` — Path 4 missed it, rescue caught it. POST-FIX: should be rare or zero (would indicate Path 4's `a_` detection has a gap).
- `none` — neither fired, draw renders without alpha-test. POST-FIX: should be zero for alpha-cutout typeIDs.

**Success criterion:** after Path 4 lands, every formerly permanent-black
typeID should emit `effective=0x1` with `effectiveSource` ∈
{`packet`, `packet+textureAlphaRescue`, `textureAlphaRescue`}. Zero
`effective=0x0` events for known alpha-cutout typeIDs. Track E uses the
distribution among the three success values to decide whether the rescue
is still load-bearing — `packet+textureAlphaRescue` indicates redundant
coverage (both safety nets active); `packet` indicates Path 4 is
load-bearing for that draw; `textureAlphaRescue` indicates Path 4 missed
a case (post-fix, this should be rare or zero — would warrant
investigation).

### Component 4 — (Optional, audit-only) Path 3 follow-up

After Path 4 lands and a smoke confirms zero permanent-black actors:

- Walk the `[REG_TYPE v2] event=warn` lines from a soak run.
- For each warning, identify the originating load path and add
  `SetAlphaTest(true)` there. This reduces reliance on Path 4's
  asset-name detection and migrates each known load site to the
  explicit-engine-call pattern.
- **Audit-only, evidence-driven.** Do NOT preemptively add `SetAlphaTest`
  calls before Path 4 ships. The 36 distinct multi-shape spread guarantees
  whack-a-mole if attempted up-front.

This component is housekeeping, not a blocker. Could ship as a separate
small PR per load site, or fold into Track B's mission-load bulk register
design.

### Components NOT included (and why)

- **Path 1 — broaden the draw-time rescue.** Symptom-level. Doesn't
  establish engine self-awareness. Track B inherits the same
  gameplay-trust dependency.
- **Path 2 — re-order registration ad-hoc to fire after `SetAlphaTest`.**
  That's Track B's job. Doing it now in isolation either gets thrown
  away when Track B lands or preempts Track A→B dependency ordering.
- **Hard `gosASSERT` instead of printf.** Stock-install constraint.
- **Walking ALL slots for the rescue** (broaden detection from
  `pkt.textureSlot` to all slots in type). Doesn't help the 38
  permanent-black types where ALL slots have `textureAlpha=false`. May be
  a future-stage refinement if smoke turns up sibling-slot cases (see
  Component 1 narrowing note).
- **`MC_TextureManager::getNodeName(DWORD)` accessor.** Considered but
  rejected — adds public API surface for what's a private internal
  cross-reference. The signature change to `registerType` is internal
  and cleaner.

---

## Implementation options considered (architectural decision: signature change)

The reviewer flagged that the original spec's pseudocode (`typeShape->listOfTextures[].textureName`) doesn't compile — that field is on the parent `TG_TypeMultiShape`, not the leaf `TG_TypeShape`. Three mechanical options for resolving this:

| Option | Approach | Pros | Cons |
|---|---|---|---|
| **1 (chosen)** | Signature change: `registerType(TG_TypeShape*, TG_TypeMultiShape*)`. Pass parent through. | No new public API. registerType has only one caller (`registerMultiShape`). Names accessed via `multiShape->GetTextureName()` — existing public method on TG_TypeMultiShape ([`msl.h:139`](../../../mclib/msl.h:139)). | Internal signature change. |
| 2 | Add `MC_TextureManager::getNodeName(DWORD nodeId)` public accessor. Look up names via `mcTextureManager` from inside `registerType` using `typeShape->listOfTextures[s].mcTextureNodeIndex`. | No registerType signature change. | Adds a public API surface for an internal cross-reference. Requires verifying `mcTextureNodeIndex` is set before `registerType` runs (likely fine — `CreateListOfTextures` populates at multi-shape ASE-load time, well before `registerMultiShape`). |
| 3 | Add a back-pointer to multi-shape on each `TG_TypeShape` during `CreateListOfTextures`. | Names always available from leaf. | Heaviest churn; touches struct layout (TG_TypeShape memory cost +8 bytes per type-shape × ~314 types = ~2.5KB resident). Not justified. |

**Option 1 chosen.** Cleanest of the three.

---

## Risks (post-revision)

| Risk | Likelihood | Mitigation |
|---|---|---|
| **R1.** A non-foliage opaque texture happens to start with `a_` (false positive) → forces alpha-test on a packet that doesn't need it. | Low. The `a_` prefix is the established convention used at 27 sites across the codebase; false positives would already break existing `gos_Texture_Alpha` load decisions. | Shader's `tex_color.a < 0.5` discard is a no-op on fully-opaque textures (alpha=1.0 always passes). Cost: negligible (alpha-test is essentially free on AMD 7900 XTX for opaque textures). Driver-rules check: no AMD-specific concern with discard pattern in `docs/amd-driver-rules.md`. |
| **R2.** A foliage texture without the `a_` prefix → false negative → still permanently black after Path 4. | Low. Asset convention has been consistent across observed sites. | Component 2's `event=warn` trap surfaces these for asset-author follow-up. Component 4's audit closes the loop. |
| **R3.** Per-`pkt.textureSlot` narrowing misses cases where the alpha asset lives on a sibling slot. | Low (would require artists to use `a_` on a non-pkt slot). | Documented as future-stage fallback; widen to "any texture in type" only if smoke evidence demonstrates the case. Not in this spec's scope. |
| **R4.** Path 4's `pkt.materialFlags` change causes test-counter divergence in `[ALPHA_TEST]` traces. | Medium (this IS the intended change — recipes formerly with `pktFlags=0x0 effective=0x1` via rescue will now emit `pktFlags=0x1 effective=0x1` directly). | This is *recovery* of correct registration, not a regression. Smoke parity check confirms `effective` (the GPU-visible value) is unchanged — only the *path* to it. The `flagSource`/`effectiveSource` fields make the change auditable. |
| **R5.** Track B inherits Path 4's runtime detection rather than ordering-fix. | Medium-High (intended). | Path 4 is the safety net; Track B's mission-load ordering is the structural fix. Both ship; Path 4 stays as a fallback that catches any future load-site regression. Q4 in `2026-05-06-track-abc-brainstorm-decisions.md` confirms compatibility. |
| **R6.** `multiShape->GetTextureName()` returns empty/garbage at register time (texture not loaded yet). | Low. `LoadTGMultiShapeFromASE` populates the multi-shape's `listOfTextures.textureName` field at ASE-load time, well before `registerMultiShape` is called. Verified by trace: existing `TEX_HANDOFF` rescue reads `listOfTextures[pkt.textureSlot].gosTextureHandle` at register time, so the array IS populated. | Defensive: empty buffer + `isAlphaTestTextureName(empty)` returns false → no alpha-test forced → existing behavior. Safe by default. |

---

## Verification appendix (per CLAUDE.md "Documentation Discipline")

All file:line citations grep-verified at write-time of this revision against HEAD `afcd75b`.

### Confirmed

- **`registerType` defined at [`gos_static_prop_batcher.cpp:542`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:542)**. Single caller at line 690 inside `registerMultiShape`. Internal-only; signature change safe.
- **`pkt.materialFlags = typeShape->alphaTestOn ? STATIC_PROP_FLAG_ALPHA_TEST : 0;`** exists at [line 641](../../../GameOS/gameos/gos_static_prop_batcher.cpp:641).
- **Existing draw-time rescue** at [line 1663](../../../GameOS/gameos/gos_static_prop_batcher.cpp:1663) with comment "Re-resolve materialFlags at draw time so the textureAlpha flag set during bdactor.cpp init (after registerType) is captured." Path 4 mirrors this pattern but at register-time.
- **Shader's alpha-test discard** at [`shaders/static_prop.frag:30`](../../../shaders/static_prop.frag:30) (`uniform int u_materialFlags;`) and [line 51](../../../shaders/static_prop.frag:51) (`if ((u_materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) { discard; }`).
- **`SetAlphaTest(true)` calls** in `TreeAppearanceType::init` at [`bdactor.cpp:3382`](../../../mclib/bdactor.cpp:3382), [`:3405`](../../../mclib/bdactor.cpp:3405), [`:3424`](../../../mclib/bdactor.cpp:3424). (Earlier draft cited 3373/3396/3424 — adversarial review caught the drift; refreshed now.)
- **`treeDmgShape` and `treeDmgShadowShape`** at [`bdactor.cpp:3438`](../../../mclib/bdactor.cpp:3438) and [`:3454`](../../../mclib/bdactor.cpp:3454) loaded via `LoadTGMultiShapeFromASE` with no subsequent `SetAlphaTest`. Negative-claim grep `SetAlphaTest.*treeDmg` returns 0 hits.
- **`a_` prefix sites:** 27 total across mclib — 12 in `bdactor.cpp`, 2 in `genactor.cpp`, 8 in `gvactor.cpp`, 5 in `mech3d.cpp`. (Earlier draft claimed 6 in bdactor — adversarial review caught the double-count; refreshed now.)
- **`TG_TypeMultiShape::GetTextureName(DWORD textureNum, char *textureName, long nameLength)`** declared at [`msl.h:139`](../../../mclib/msl.h:139), defined at [`msl.cpp:966`](../../../mclib/msl.cpp:966). Already a public method; no new API needed for Option 1.
- **`TG_MultiShape::GetTextureName`** delegates to `myMultiType->GetTextureName` at [`msl.h:449-451`](../../../mclib/msl.h:449) — same name, instance-side.
- **`TG_TinyTexture` struct layout** at [`mclib/tgl.h:339-345`](../../../mclib/tgl.h:339): `{mcTextureNodeIndex, gosTextureHandle, textureAlpha}` — confirmed no `textureName` field. This is the C1 finding from adversarial review; the revised spec uses `multiShape->GetTextureName()` instead.
- **Smoke artifact** at `tests/smoke/artifacts/2026-05-06T08-53-38/mc2_01.log` exists; contains:
  - 314 `[REG_TYPE v2]` events (all frame=0)
  - 33 with `alphaTestOn=1`, 281 with `alphaTestOn=0`
  - **36059** `effective=0x0` ALPHA_TEST events (earlier draft claimed 36848 — adversarial review caught the drift; refreshed now)
  - 38 permanent-black drawn types owned by 36 distinct multi-shapes

### Smoke verification gate (post-implementation, pre-merge)

1. **`MC2_STATIC_UPDATE_SKIP=1 MC2_REG_TYPE_TRACE=1 MC2_ALPHA_TEST_TRACE=1`** — pre/post comparison.
   - Pre baseline: **36059** events with `effective=0x0` (the bug surface).
   - Post target: **0** `effective=0x0` events for known alpha-cutout typeIDs. Acceptable: a small residual count of non-`a_`-prefix alpha-test types that surface as `[REG_TYPE v2] event=warn` lines (Path 4's known false-negative class).
   - Per-typeID drill-down: every formerly-permanent-black typeID (38 in the baseline run) should now emit `effective=0x1` with `effectiveSource=packet` (Path 4 caught it at register-time) or `effectiveSource=textureAlphaRescue` (existing fallback caught it).

2. **`MC2_STATIC_UPDATE_SKIP=0`** baseline — confirm no regression for the non-skip path.
   - Target: 0 `effective=0x0` events at draw, < 5 black billboards visually (the pre-existing 0.5% cull-edge case is unchanged by this fix and is a separate Track-A/B concern).

3. **`[DESTROY v1]` count parity** — confirm `+0` actor-destroy delta vs baseline (per `memory/cull_gates_are_load_bearing.md` discipline; Path 4 doesn't touch cull or lifecycle, but verify).

4. **Visual interactive verification** per `memory/feedback_smoke_policy_30s_mc2_01.md` and the original handoff's "Definition of done." 30s mc2_01 + interactive turn-away-and-back. **Zero black billboards.**

5. **Buildings remain unaffected.** Spot-check a building cluster across LOD swaps — no new visual artifacts. (Buildings have `alphaTestOn=false` AND no `a_` textures → Path 4 leaves them unchanged.)

### Class-layout / build hygiene

- **No struct layout change** — `pkt.materialFlags` field already exists at [`gos_static_prop_batcher.h:46`](../../../GameOS/gameos/gos_static_prop_batcher.h:46). No `rm -f mc2.exe` ritual required (per `memory/msvc_incremental_link_silent_staleness.md`).
- Build with `--config RelWithDebInfo` per worktree CLAUDE.md.
- Deploy via `cp -f` per file (never `cp -r`).

---

## Track B handoff hooks

Once Path 4 ships and soaks, the following pattern is established for
Track B to inherit:

1. **Engine-side asset-property-driven material classification is the
   canonical pattern.** Track B's mission-load bulk registration walks
   every static prop and registers types — at that point, `Path 4`'s
   `isAlphaTestTextureName` check runs once per type's per-slot pair.
   Track B doesn't need to add per-load-site `SetAlphaTest` calls; the
   engine reads the asset property directly.

2. **`SetAlphaTest` calls in `TreeAppearanceType::init` become belt-and-suspenders**
   once Path 4 ships. They can stay (harmless, redundant) or be demoted
   in Track E (legacy retirement) once the asset-name pattern is proven
   stable.

3. **The `event=warn` trap from Component 2 becomes a CI / soak signal.**
   Track B's adversarial-plan-review pass should grep for any
   `event=warn` lines in soak logs and treat each as a content-author or
   load-site-author bug to fix during Track B's bulk registration walk.

4. **`open_rts_engine_framing.md` gets a concrete win.** This branch is
   the first slice in the codebase where engine-side rendering reads
   asset properties directly instead of being told what to do by
   gameplay-side code. Cite it in the framing memory's "When this
   framing becomes load-bearing" section as the reference example.

5. **Track B is NOT preempted.** This branch is *Track B prep* — it
   makes the registration *payload* correct regardless of registration
   *timing*. Track B's job (mission-load enumeration, register-on-spawn
   API, persistent SSBO sized for total population) is unchanged. The
   two slices stack.

---

## References

- `docs/superpowers/mc3-rendering-modernization-roadmap.md` — Track B is
  the structural successor to this prep slice.
- `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` —
  Q4 (mission-load bulk register), Q6 (first-frame structural).
- `docs/superpowers/specs/2026-05-06-update-skip-touch-regression-handoff.md` —
  the original (mis-scoped) handoff this investigation re-characterized.
- `docs/superpowers/explorations/2026-05-02-track-d-mvp-adversarial-findings.md`
  — prior adversarial review caught the same `TG_TinyTexture` vs
  `TG_Texture` confusion. Lesson now propagated.
- `memory/open_rts_engine_framing.md` — engine/content layer separation;
  this branch is the first concrete win.
- `memory/mc3_modernization_philosophy.md` — modernization defaults.
- `memory/stock_install_must_remain_playable.md` — printf-not-assert
  rationale.
- `memory/feedback_dont_paper_over_bugs.md` — "engine reads asset
  property" is a structural inversion of the dependency, not a symptom
  hack.
- `memory/feedback_always_dispatch_adversarial_review.md` — adversarial
  review on the revised spec is mandatory before implementation.
- Smoke artifact (definitive evidence):
  `tests/smoke/artifacts/2026-05-06T08-53-38/mc2_01.log`.

---

## Revision history

- **2026-05-06 (revision 1):** Original draft. Reviewed; yellowlight.
  Issues caught: C1 (pseudocode references nonexistent `TG_TypeShape::listOfTextures[].textureName`), M1 (line numbers stale), M2 (`a_` site count double-counted), M3 (smoke `effective=0x0` count mismatch).
- **2026-05-06 (revision 3.5 — implementation reverted; spec preserved as design record):**
  - The Path 4 source-side implementation (signature change to `registerType`, `isAlphaTestTextureName` helper, per-pkt + per-type detection, `[REG_TYPE v2]` schema bump, `effectiveSource` field on `[ALPHA_TEST]`) was collapsed/reverted by commit `5a40f15` ("revert: collapse experimental fixes — keep only minimal pin proof") as part of a sibling session's pivot to the texture-cache pin fix (commits `52237d0` + `d03ee3d`).
  - **The actual visible black-billboard symptom turned out to be texture-cache eviction, not material classification.** Path 4 was a real but adjacent fix; in retrospect, landing it interleaved with the texture-cache investigation made the diagnostic signals harder to read.
  - This spec is preserved as a design record. If Track B's mission-load bulk register surfaces the underlying alpha-test classification gap as a real problem (e.g., destroyed-tree damage state rendering as black quads), the design here is a starting point. The pause/unpause diagnostic captured in `memory/pause_unpause_diagnostic_for_static_render_bugs.md` is the load-bearing learning from this thread.

- **2026-05-06 (revision 3.4 — scope clarified, branch lands as Track B prep only):**
  - **The pause/unpause diagnostic from user-test 2026-05-06 disambiguates this from the visible black-billboard symptom.** Trees/props/fence render correctly when paused (Esc menu), break immediately on unpause. Alpha-test material flags don't change between pause and unpause; texture cache state does (`mission.cpp:509` comment: `mcTextureManager->update() uncaches things which only objectManager->update can cache back in`). Under `UPDATE_SKIP=1`, `touch()` substitutes for `update()`, so the re-cache pathway via `TransformMultiShape→SetTextureHandle` never fires for static-claiming actors. That's a SEPARATE bug from this spec's alpha-test classification gap.
  - Spec scope clarified: this branch fixes the registration-time alpha-test classification gap (real, valid, Track-B-aligned, ~150 affected types in mc2_01) but does NOT fix the user-visible symptom they originally reported. The visible symptom requires investigating `mcTextureManager->update()` eviction.
  - Path 4 + the per-type widening from rev 3.3 stays in tree as preparatory groundwork for Track B's mission-load bulk register. It correctly addresses an asymmetry where `treeDmgShape`/`treeDmgShadowShape`/`GenericAppearance` paths never call `SetAlphaTest(true)` despite shipping alpha-test assets — when those paths' recipes ARE rendered (e.g., destroyed-tree damage state), they would have rendered as black quads without this fix.
  - New-session handoff document created at `docs/superpowers/specs/2026-05-06-static-prop-texture-cache-handoff.md` to investigate the actual visible-symptom bug from a clean framing.

- **2026-05-06 (revision 3.3 — per-type widening, advisor-pre-approved fallback activated by smoke):**
  - **Trigger:** validation smoke at `2026-05-06T10-19-06` showed Path 4 with per-pkt narrowing missed 32,581 events (`effectiveSource=none`) despite 150 `event=warn` lines. User visual: trees, props, fence still black; LOD-cutoff "lit→stale→black" mechanism empirically confirmed (was theory, now fact). The pre-swap LOD's pkt happened to target a slot with `a_` (worked); post-swap LOD's pkt targets a sibling slot without `a_` while the alpha texture is on a different slot of the same type — per-pkt narrowing skips the flag-set.
  - **Fix:** widened Path 4 from per-pkt `alphaByName` to per-type `typeHasAlphaTextureName`. The flag is set if EITHER `typeShape->alphaTestOn`, OR pkt's own slot has `a_` prefix, OR any slot in the type has `a_` prefix. `typeHasAlphaTextureName` now computed once before the per-pkt loop and reused at the per-type print site (was duplicated; now single source of truth).
  - **Rationale (per advisor):** "Only widen to 'any texture in type' if the smoke proves the alpha asset lives on a sibling slot... If that happens, document it as a second-stage fallback with trace evidence." The smoke proved the case. Trunk-on-opaque-slot pkt now gets alpha-test enabled — texture has alpha=1.0 throughout, shader's discard branch never fires, identical rendering.
  - **New `flagSource` values:** `alphaTestOn,typeWide` and `typeWide` added (in addition to the existing `alphaTestOn,textureName`, `alphaTestOn`, `textureName`, `none`).
  - **Buildings unchanged:** building types have no `a_` slots → `typeHasAlphaTextureName=false` → no widening kicks in → no behavior change.

- **2026-05-06 (revision 3.2 — schema version bump per advisor):**
  - All `[REG_TYPE v1]` references in the spec are renamed to `[REG_TYPE v2]`. The trace schema now includes new `event=` discriminators (`event=pkt`, `event=warn`), new fields (`typeHasAlphaTextureName`, `flagSource`, `alphaByName`), and a different consumer-interpretation contract; per worktree CLAUDE.md "Tier-1 Instrumentation" the version bumps. Prior `[REG_TYPE v1]` smoke artifacts remain historical baselines (the smoke at `tests/smoke/artifacts/2026-05-06T08-53-38/mc2_01.log` cited in the verification appendix is `v1`; post-fix smokes will emit `v2`). Comparison gates use `effective=0x0` count (subsystem-agnostic) as the primary signal; per-typeID drill-down on the granular `effectiveSource` values is post-fix-only.
  - `[ALPHA_TEST]` tag stays unversioned. The `effectiveSource=` field is a backward-compatible append-only addition; no existing `[ALPHA_TEST]` parser exists. If the codebase later introduces versioning for that subsystem, this addition can be folded into a future `[ALPHA_TEST v2]` introduction.

- **2026-05-06 (revision 3.1 — minor mechanical fixes from third adversarial GREENLIGHT):**
  - Implementer note added: `gos_static_prop_batcher.h:105` declaration must be updated alongside `.cpp:542` (header-update-implicit issue from round 3).
  - Component 3 snippet now explicitly shows `uint32_t effectiveMaterialFlags = pkt.materialFlags;` init line preserved (was implied; now shown for clarity).
  - Smoke gate success criteria widened to include `effectiveSource=packet+textureAlphaRescue` (third success value, was omitted from gate but documented as success in enum).
  - Operator-awareness note added: per-leaf warn over-counts on multi-leaf multi-shapes due to `TG_TypeMultiShape::SetAlphaTest` propagation. Audit signal still correct.

- **2026-05-06 (revision 3):** Per second adversarial review:
  - Component 1 pseudocode scope clarified — added new `[REG_TYPE v2] event=pkt` line that fires inside the per-pkt loop with full per-pkt resolution (`alphaByName`, `flagSource`, `materialFlags` per slot). Existing per-type `[REG_TYPE v2]` print stays at line 665 and gets a single new field `typeHasAlphaTextureName` (computed from a per-type slot walk). Two trace streams — neither stream lost a consumer.
  - Component 2 (warn) moved to per-type scope. Walks the type's full slot range; fires when `alphaTestOn==0` AND any slot has `a_` prefix. Closes the gap where the per-pkt warn would miss a type whose draw-pkt slot is opaque but whose sibling slot has the alpha texture. Audit (Component 4) now gets every load site that needs `SetAlphaTest`, regardless of which slot the draw packets target.
  - Component 3 `effectiveSource` granularity expanded to 4 values: `packet+textureAlphaRescue` (both fired), `packet` (Path 4 only), `textureAlphaRescue` (Path 4 missed, rescue caught), `none` (bug — neither). Distinguishes the case where rescue would have fired anyway from the case where Path 4 is load-bearing — needed for Track E "demote rescue" decisions.

- **2026-05-06 (revision 2):** Per advisor input + first adversarial findings:
  - Reframed root cause from "registration timing race" to "registration-time material classification under-specified."
  - Component 1 narrowed from "any texture in type" to per-`pkt.textureSlot` (mirrors existing draw-time rescue scope).
  - Added `isAlphaTestTextureName` helper; both flag computation and warning use the same predicate.
  - Switched from compile-broken `typeShape->listOfTextures[].textureName` to `multiShape->GetTextureName()` via signature change to `registerType` (Option 1; Options 2/3 documented in §"Implementation options considered").
  - Added `flagSource=` field to `[REG_TYPE v2]` and `effectiveSource=` field to `[ALPHA_TEST]` traces for unambiguous post-fix verification.
  - All file:line citations refreshed against HEAD `afcd75b` (line 542, 641, 1663 for the three primary sites; 3382/3405/3424 for SetAlphaTest sites).
  - `a_` prefix site count corrected: 27 across the codebase (12 bdactor, 2 genactor, 8 gvactor, 5 mech3d).
  - Smoke `effective=0x0` baseline corrected: 36059.
  - Branch label clarified: "Track B prep: static-prop material classification hardening" (NOT "Track B partial implementation").
  - Component 4 (Path 3 audit) explicitly demoted to post-smoke / evidence-driven; do not preemptively patch load sites.
