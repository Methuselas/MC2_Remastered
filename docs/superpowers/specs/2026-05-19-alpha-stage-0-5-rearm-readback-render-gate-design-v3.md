# alpha-Stage 0.5 - Re-arm the readback render gate (design v3)

> **POSTSCRIPT 2026-05-20 LATE-EVENING — §4 EMPIRICALLY NO-GO (visual canary).**
>
> Tentative-ship of §4 as commit `40a54b7` followed by user-driven mc2_10
> visual canary (90s, fast pan + spin + corner, MC2_GPU_CULL=1
> MC2_GPU_CULL_READBACK=1) returned **visual NO-GO**. Reverted as `dc2e8f6`.
> Observed artifacts:
> - Static-prop / tree / mech POPPING in and out during camera motion
>   (readback false-negative class — the 2.8%/30% probe envelope made
>   visual)
> - **BLACK building textures** — RESURRECTED 2026-05-05 black-tree bug
>   class. §4 keeps update-gate on coarse `inView` (LEAVE-site §2.1) but
>   moves render-gate to readback; readback false-positives admit actors
>   whose `update()` was cull-skipped, `cachedGpuLightIndex_` is stale,
>   registry::flush emits draws with stale light data. The 2026-05-05
>   black-tree fix (`cachedFrame_` stamp gate at
>   `gos_static_prop_registry::flush()`) protects the batcher path only;
>   §4's gate flip routes through the legacy submit path which has no
>   equivalent stamp gate. See
>   `memory/black_tree_bug_investigation_state.md`.
> - Z-fighting at distance + colored→blank at ~5000 unit LOD transition,
>   both attributable to the same readback false-positive class
>   admitting actors whose registration state wasn't refreshed.
>
> Root cause analysis: §4 in its current shape has TWO structural bugs,
> not one. The v3 spec only modelled bug #1 (readback non-superset →
> dropouts); the spec missed bug #2 entirely (update/render gate split
> resurrects the cull-aware staleness class that 2026-05-05 fixed for
> coarse-cull-only configs). Three possible meta-fixes:
> 1. **Defer to alpha-Stage 1** (deeper readback-quality work). Both
>    bugs need to be solved before §4 lands.
> 2. **v4 reframe — gate render on `blockVisBits[]` directly under
>    sticky-bit.** Block-level admit is strict-superset-by-construction
>    per-mission under sticky (no readback dependency, no false-positive
>    class, no update/render split). The gate is coarser (whole block
>    lights up at once) but that's a stock-default safety feature, not
>    a bug. Lifetime aligns with R-NEW-8 admit-only-grows analysis.
> 3. **Couple update gate to render gate.** If §4 ships as specced, the
>    update gate at `terrobj.cpp:796` must also move off coarse — that's
>    the alpha-Stage 1 carve-out the spec explicitly defers. Bringing
>    it into Stage 0.5 substantially expands scope.
>
> §2.5 sticky-bit (`91b6991`) stays shipped — independent durable value
> (retired K-window META-FIX of `056c365`). Do NOT re-attempt §4 in
> current shape; full reasoning + the user's
> "K was meant to fix this" finding (which is correct for indirect-batcher
> path, doesn't apply to readback channel) at
> `memory/stage_0_5_section_4_blocked_on_readback_non_superset.md`.

> **POSTSCRIPT 2026-05-20 EVENING — §4 BLOCKED, §2.5 SHIPPED INDEPENDENTLY.** (Superseded by LATE-EVENING postscript above; preserved for arc context.)
>
> §2.5 sticky-bit shipped as commit `91b6991` on `claude/nifty-mendeleev`
> (META-FIX of `056c365`; independent durable value — net -9 LOC,
> greybeard concurred, tier1 5/5 PASS).
>
> §3 precondition canary on mc2_10 worst-case (user-driven full pan +
> spin + corner, 90s, `MC2_TOBJ_PARITY=1 MC2_GPU_CULL=1
> MC2_GPU_CULL_READBACK=1`): **2.814% sustained / 30.308% worst-window**
> across 62 summary lines, 7.99M samples. STOP per §3.2 (gate is
> < 0.5% sustained AND no window >= 2%; we exceeded by 5-15x).
>
> **Structural cause — candidate (a) confirmed; (b)/(c) ruled out by grep.**
> Sticky-bit (§2.5) widens the indirect-draw bucket admit set only;
> readback writes raw per-actor frustum at `gpu_cull.comp:239` BEFORE
> the temporal `else if (cat == CAT_STATIC_PROP)` runs. The two paths
> are independent by design — sticky cannot close the readback
> non-superset gap. The probe header at `code/terrobj.cpp:214-225`
> documents this as the well-known Task 7 10-60% dropout envelope that
> caused `aeceb2c` to be reverted via `e0ea027` in the first place.
> v3 implicitly assumed sticky-bit would lift readback into a coarse-
> superset; that assumption is structurally wrong.
>
> **§4 (the substitutive contract, the `aeceb2c` un-revert) is deferred
> to alpha-Stage 1 readback-quality work.** Do NOT attempt to land §4
> on HEAD; it would broaden coarse-cull pop into observable render pop
> at 5-15x the accepted budget under any readback-on config. §2.5 stays
> shipped and is independent of this decision.
>
> Possible v4 reframe (for next planner): gate render on `blockVisBits[]`
> directly under sticky-bit. Under sticky, that buffer is a strict-
> by-construction per-mission superset at the block level — no readback
> non-superset gap because it doesn't read the readback. Different
> lifetime semantics (sticky never narrows; readback can) but those
> align with v3 R-NEW-8 "admit-only-grows" analysis. Not in any spec;
> surface to planner when revisiting.
>
> Full reasoning trail: `memory/stage_0_5_section_4_blocked_on_readback_non_superset.md`.
> Raw data: `tests/smoke/artifacts/2026-05-20T10-02-00/mc2_10.log`.



Date: 2026-05-19. Worktree: `claude/nifty-mendeleev`, HEAD `c8b7ac0`
(advanced from v2's `0b41e87` by one MC2_GL_DEBUG_FATAL test-tooling
commit `c8b7ac0`; neither v2 §0 nor any v2-cited symbol is touched by
this commit — re-grep confirmed every v2 file:line still resolves).
Status: DESIGN v3. Supersedes v2 (preserved at
`2026-05-19-alpha-stage-0-5-rearm-readback-render-gate-design-v2.md`)
and v1 (preserved). Two USER-LOCKED mechanism changes (A: sticky-bit
visibility supersedes K=12 rolling window; B.1: reviewer dissolution
of K-warmup; B.2: reviewer fail-open semantics check). All cited
symbols re-grepped in this invocation.

## Provenance + diff-from-v2

v2 stood as a sound Stage 0.5 design but its load-bearing K=12 rolling
admit window has been retired by user decision in favour of a
strictly-conservative sticky-bit admit (once-seen = forever-admitted).
This dissolves the M-2 first-K-frames transient by construction (no K,
no transient), retires the §3.3 K-extension ladder (no K to extend),
and renders §4.5 K-warmup obsolete (no admit set to warm — independently
addresses the external reviewer's "do not run full Mission::update in a
loading-screen pre-warm" major). The substantive v2 contract — §0
prerequisite Mission::load `compute_buildIndirectBuffer`, §1.2
render-only scope + alpha-Stage 0.6 shadow disclaimer, §2 substitutive
contract incl. all 6 gate sites and the LEAVE-site table, §6
substitutive proof gate, §6.4 TOBJPARITY tautology framing — carries
forward unchanged. v3 adds a §2.5 sticky-bit shader change (a separate
sibling commit), restructures §3 precondition remediation (no K-ladder),
deletes §4.5, and updates §5 risk surface to surface (a) admit-only-grows
interaction with destroyed actors and (b) the B.2 fail-open grep result.

---

## Greybeard ruling — Change A (sticky-bit admit)

Run as a fresh ruling for the mechanism shift; NOT a re-derivation of
the Stage 0.5 verdict (that carries forward from v2 unchanged — see
next section).

**A.1 Subsystem pin.** The artifact owned by this mechanism shift is
the *block-temporal admit predicate* of the C1b indirect static-prop
cull — `shaders/gpu_cull.comp:286-288` grep-verified at HEAD:
```
bool effVisible = (rec.blockIdx < uint(uBlockCount)) &&
    ((uint(uFrameStamp) - blockVisBits[rec.blockIdx]) <
     uint(uHistoryK));
```
and the rollup writer at `shaders/gpu_cull_block_rollup.comp:80`
grep-verified:
```
atomicMax(blockVisBits[blk], uint(uFrameStamp));
```
The mechanism's job is to decide "should the cull re-admit a
*currently-frustum-culled* static prop because its block was recently
seen?" — i.e. the C1b temporal-superset admit, NOT the strict
frustum-cull, NOT the dynamic mech/GV path (option-(b) `else if`
confines temporal to `cat == CAT_STATIC_PROP` — `gpu_cull.comp:275`
grep-verified). The render consumer (the §2 gates this Stage 0.5
re-arms) is downstream of the readback, not directly of `blockVisBits`;
this mechanism shift narrows the *producer* envelope and Stage 0.5
inherits the resulting strict-superset admit set.

**A.2 Symptom vs cause.** Proximate symptom v2's K-window addressed:
"static prop drops for 1-2 frames on the frustum edge during fast pan
because the C1b indirect path is stateless." Proximate symptom that
made K-window itself problematic: a first-K-frame transient at mission
start where every block has stamp 0 against `s_cullFrameIdx` seeded at
K (`gpu_cull_compute.cpp:79` grep-verified
`s_cullFrameIdx = gpu_cull::GPU_CULL_BLOCK_TEMPORAL_K`) — the `(F-0) <
K` test fails uniformly until enough rollup passes have stamped each
visible block. v2's §4.5 K-warmup-via-loading-screen attempted to absorb
this transient invisibly; the external reviewer flagged the candidate
implementation (full Mission::update in the loading-screen wait loop)
as a worse class of risk than the artifact being patched.

Upstream condition both symptoms share: the admit predicate is *bounded
in time*, requiring re-stamping to maintain admission. Removing the
time bound dissolves both symptoms simultaneously.

**A.3 The meta-fix.** Replace the rolling-K-window with a sticky-bit:
once a block's `blockVisBits[i]` is non-zero (ever stamped during this
mission), it stays admitted permanently. Mechanically the rollup
becomes `atomicOr(blockVisBits[i], 1u)` (idempotent set-once) and the
cull gate becomes `blockVisBits[rec.blockIdx] != 0u`. Strictly
*conservative* — sticky-bit admits a SUPERSET of K-window for any K,
because "ever-seen" ⊇ "seen in the last K frames" ⊇ "raw-visible
this frame". The per-mission reset (one-shot zero at
`gpu_cull_compute.cpp:779-784` grep-verified, the `glClearNamedBufferSubData`
immediately after `glBufferData(...,s_blockVisBuf,...)`) is PRESERVED
exactly — it is what stops one mission's sticky stamps from leaking
into the next. The first-K-frame transient that v2 §4.5 existed to mask
*does not exist* under sticky-bit: blocks in the intro-pan frustum are
admitted on their first frame of frustum entry, and stay admitted; no
K-frame warmup window exists to be transient.

Code changes (specified §2.5 below):
- DELETE the K mechanism: `uHistoryK` uniform decl in
  `gpu_cull.comp:124`, the `(uint(uFrameStamp) - blockVisBits[...]) <
  uint(uHistoryK)` gate at `gpu_cull.comp:287-288`, the `uHistoryK`
  upload at `gpu_cull_compute.cpp:1000-1002`, the
  `GPU_CULL_BLOCK_TEMPORAL_K` constexpr at `gpu_cull_compute.h:40`.
- Replace the cull gate with `blockVisBits[rec.blockIdx] != 0u` guarded
  by the same M3 short-circuit on `rec.blockIdx < uint(uBlockCount)`.
- Replace the rollup write at `gpu_cull_block_rollup.comp:80` with
  `atomicOr(blockVisBits[blk], 1u)`.
- DELETE the `uFrameStamp` uniform pipeline IF (and only if) no other
  consumer references it — see A.4 / §2.5 grep at write-time.

**Is `s_cullFrameIdx` retirable?** Grep at v3 write-time: the only
readers of `s_cullFrameIdx` are at `gpu_cull_compute.cpp:79`
(decl + seed), `:952-953` (`++s_cullFrameIdx`; `const GLint
frameStamp = (GLint)s_cullFrameIdx`), and the lockstep upload sites at
`:999` (`glUniform1i(locFS, frameStamp)` on cull program) and `:1143`
(rollup program). No other consumer (e.g. C3 routing, readback, draw,
or batcher) references it — confirmed by opposite-direction grep
shown above (`Found ... gpu_cull_compute.cpp / gpu_cull.comp /
gpu_cull_block_rollup.comp` only). After sticky-bit, all four sites
delete: the counter exists solely to feed `uFrameStamp`, which sticky-bit
does not need. Same applies to `GPU_CULL_BLOCK_TEMPORAL_K` — only
referenced at `gpu_cull_compute.cpp:79` (seed) and `:1002`
(`uHistoryK` upload), both of which delete under §2.5.

**Is the M3 fail-OPEN clamp retired?** No — it stays. The clamp's
purpose is structural: when `s_blockVisBuf == 0` (props-less mission or
pre-allocation), `uBlockCount = 0` and the short-circuit
`rec.blockIdx < uint(uBlockCount)` prevents indexing an unbound
binding. This invariant is independent of K vs sticky-bit; the
shader-side `bool effVisible = (rec.blockIdx < uint(uBlockCount)) &&
(blockVisBits[rec.blockIdx] != 0u);` form retains the short-circuit.
DO NOT remove the M3 clamp; it is load-bearing for cross-mission
props-less safety (Gate2 MAJOR-1, v5 plan §Slice 2 R8).

**A.4 Substitutive test.** Mechanism is SUBSTITUTIVE not additive: the
K-window code is DELETED (header constexpr, uniform decls + uploads,
GLSL gate expression, frameStamp counter) — not OR'd alongside sticky.
Same buffer (`s_blockVisBuf`), same binding (13), same per-mission
reset, same M3 clamp, same rollup-then-cull frame ordering, same
mech/GV byte-identical path (the `else if (cat == CAT_STATIC_PROP)`
gate confines the temporal term as before). The mechanism shift is
strictly net-negative code — counter, two uniforms, one constexpr, one
constant decode lost.

**A.5 Verdict.** `META-FIX` of `056c365`. The K-window was the legacy
mechanism whose original constraint (bound the admit superset to a
finite history so overdraw budget stays K-bounded) no longer holds:
user accepts the unbounded admit (per `feedback_ram_cost_not_a_concern_below_500mb.md`
the headroom is plentiful; overdraw cost is `bucketCapacity` cap-bounded
not stamp-bounded; sticky-bit blocks accumulate at worst proportional
to the mission's traversal coverage). Sticky-bit is the modern shim:
it dissolves the first-K-frame transient and the K-warmup question
together. K-window is the bug, not its symptoms.

---

## Inherited Stage 0.5 verdict (carry forward from v2 §1)

v2's greybeard ruling for Stage 0.5 itself stands unchanged: `META-FIX`
for the render-consumer repoint from coarse `inView` to
readback-driven `renderVisible`, with explicit acknowledgement that a
DEEPER META-FIX exists (alpha-Stage 1: drive lifecycle + destroy gates
off the readback too, then delete the CPU coarse-cull producer
entirely) and a sibling alpha-Stage 0.6 (static-shadow consumer
migration via `gos_static_prop_batcher.cpp` + `mclib/txmmgr.cpp` —
neither reads `Appearance::renderVisible`) covers shadow popping
out-of-scope here. Scope-pin (v2 §1.1, §1.2) preserved; the
inherited-debt language at v2 §1.4 stands; v2 §1.5 K-warmup-as-primitive
DISSOLVED by Change A and replaced by "no warmup needed under
sticky-bit" (§4.5 below). v2 §1.6 substitutive contract for Stage 0.5
(render-gate repoint, not Tracy-zone retirement) stands.

---

## 0. Prerequisite commit: `Mission::load` `compute_buildIndirectBuffer`

CARRIED FORWARD FROM v2 §0, UNCHANGED. Re-grep at v3 write-time:

- `code/saveload.cpp:1584-1585` grep-verified:
  ```
  GpuStaticPropBatcher::instance().finalizeGeometry();
  GpuMechBatcher::instance().finalizeGeometry();
  ```
  matches v2's cited insertion point exactly; no drift.
- `code/saveload.cpp:1567` grep-verified `loadProgress = 100.f;`.
- `gpu_cull::compute_isEnabled()` declared `gpu_cull_compute.h:52`
  (re-grep at write-time of patch; was at `:52` per v2).
- `gpu_cull::compute_buildIndirectBuffer(uint32_t)` declared
  `gpu_cull_compute.h:72` (re-grep at write-time of patch).
- `batcher_getTypeCount()` declared `gos_static_prop_batcher.h:297`
  (re-grep at write-time).

**The patch (unchanged from v2 §0):** in `code/saveload.cpp` immediately
after the `finalizeGeometry()` pair at lines 1584-1585:
```cpp
// VPL-deferred item 11 FIX (b): mirror Mission::init's
// compute_buildIndirectBuffer call. Without this, savegame restore
// runs against the prior mission's indirect cmd buffer + block-stamp
// state; the 056c365 sticky-bit superset is undefined.
// Mission::init tail at mission.cpp:3128-3136 is the reference.
if (gpu_cull::compute_isEnabled()) {
    gpu_cull::compute_buildIndirectBuffer(batcher_getTypeCount());
}
```

(v3 note: the rationale string mentions "sticky-bit superset" instead
of v2's "block-temporal superset" — semantics identical, the per-mission
reset is the load-bearing operation either way; sticky-bit makes the
reset MORE load-bearing not less, since without reset a savegame loads
into mission A's stale sticky stamps and admits unrelated blocks
forever until next mission restart.)

**Validation gate (prerequisite, NOT Stage 0.5):** savegame-restore
smoke (quicksave + reload + observe no static-prop disappearance vs
unsaved baseline). Tier1 smoke alone is INSUFFICIENT — tier1 always
cold-starts via `Mission::init`. Use `MC2_MECH_RESTORE_TRACE=1` if
needed to confirm the restore path executed. Land this commit before
moving to Stage 0.5.

**Commit message tag suggestion:** `fix(saveload): mirror Mission::init
compute_buildIndirectBuffer in Mission::load (VPL-deferred item 11 FIX
(b); alpha-Stage 0.5 prerequisite)`.

---

## 1.2 Render-only scope + alpha-Stage 0.6 disclaimer

CARRIED FORWARD FROM v2 §1.2 UNCHANGED. Stage 0.5 does NOT fix
static-prop / tree shadow popping under the default tessellated
rendering path. `BldgAppearance::renderShadows` at `bdactor.cpp:1832`
early-returns at `:1835` `if (gos_IsTerrainTessellationActive())
return ...` BEFORE reaching the `:1838` gate (re-grep at write-time);
same for `TreeAppearance::renderShadows` at `bdactor.cpp:4262`
(early-return at `:4265`, gate at `:4268`). Under default tessellated
path those two gate flips are dead code. We keep them anyway because
(a) they mirror `aeceb2c` verbatim (minimum-diff revert is safest),
(b) they preserve coverage if `gos_IsTerrainTessellationActive()`
ever returns false on a debug path or future config, and (c) alpha-Stage
0.6 (static-shadow consumer migration) can leverage `renderVisible`
without re-adding the gates. If shadow popping is observed alongside
render popping post-Stage-0.5, that is alpha-Stage 0.6's problem.

---

## 2. Substitutive contract

CARRIED FORWARD FROM v2 §2 UNCHANGED. Re-grep at v3 write-time of every
cited symbol:

### Added (re-introduced from `aeceb2c`, with line-drift reconciliation)

- `mclib/appear.h`: `bool renderVisible;` member + ctor/init defaults
  (`= TRUE`, fail-open) + `canRenderBeSeen()` accessor +
  `setRenderVisible(bool)` setter. Verbatim restoration of the
  `aeceb2c` diff (~45 lines).
- `mclib/bdactor.cpp`: `BldgAppearance::recalcBounds` and
  `TreeAppearance::recalcBounds` get `renderVisible = inView;`
  immediately before each method's terminal `return(inView);`
  (Bldg: search current HEAD for terminal `return(inView)` in
  `BldgAppearance::recalcBounds`; Tree: same in
  `TreeAppearance::recalcBounds`). Mover overrides
  (`Mech3DAppearance::recalcBounds` at `mech3d.cpp:2132`,
  `GVAppearance::recalcBounds` at `gvactor.cpp:1614`) are FULL
  overrides that do NOT call base — untouched. Mover render gates use
  separate readback wiring (`MC2_GPU_CULL_LIFECYCLE`); out of scope.
- `code/terrobj.cpp`: in `TerrainObject::update`, inject after the
  lifecycle block at `:796` (grep-verified `if (inView)`) and before
  the TOBJPARITY probe site:
  ```cpp
  appearance->setRenderVisible(
      gpu_cull::readback_isEnabled()
          ? gpu_cull::readback_isActorVisibleLagged(
                static_cast<uint32_t>(getHandle()))
          : inView);
  ```
  User-locked decision per §5 R-NEW-9 (a): fail-open value is the
  just-computed coarse `inView` (in scope from `recalcBounds()` at
  `code/terrobj.cpp:793`), NOT `true`. Under stock-default
  (`MC2_GPU_CULL_READBACK` unset → `readback_isEnabled() == false`,
  `g_useGpuStaticProps == false`), this is byte-identical to legacy
  coarse cull. Under readback-on, preserves Stage 0.5 design intent.
  Both `readback_isEnabled` (`gpu_cull_readback.h:28`, grep-verified
  this session — `:27` is the comment, `:28` is the declaration) and
  `readback_isActorVisibleLagged` declared `gpu_cull_readback.h:82`.

### Modified (full new conditions; `|| g_useGpuStaticProps` PRESERVED)

Grep-verified at HEAD this session:

1. `code/terrobj.cpp:945`
   - OLD: `if (appearance->canBeSeen() || g_useGpuStaticProps)`
   - NEW: `if (appearance->canRenderBeSeen() || g_useGpuStaticProps)`
   - Site: `TerrainObject::render`.

2. `code/terrobj.cpp:1015`
   - OLD: `if (appearance->canBeSeen())`
   - NEW: `if (appearance->canRenderBeSeen())`
   - Site: `TerrainObject::renderShadows` (dead under default
     tessellated path; see §1.2).

3. `mclib/bdactor.cpp:1302`
   - OLD: `if (inView || g_useGpuStaticProps)`
   - NEW: `if (renderVisible || g_useGpuStaticProps)`
   - Site: `BldgAppearance::render` submit gate.

4. `mclib/bdactor.cpp:1838`
   - OLD: `if (inView && visible && !appearType->spinMe)`
   - NEW: `if (renderVisible && visible && !appearType->spinMe)`
   - Site: `BldgAppearance::renderShadows`. Dead under default
     tessellated path (early-return at `:1835`). Kept per §1.2.

5. `mclib/bdactor.cpp:4007`
   - OLD: `if (inView || g_useGpuStaticProps)`
   - NEW: `if (renderVisible || g_useGpuStaticProps)`
   - Site: `TreeAppearance::render` submit gate.

6. `mclib/bdactor.cpp:4268`
   - OLD: `if (inView && visible)`
   - NEW: `if (renderVisible && visible)`
   - Site: `TreeAppearance::renderShadows`. Dead under default
     tessellated path (early-return at `:4265`). Kept per §1.2.

### Deleted

The standalone `[TOBJPARITY v1]` framing at `terrobj.cpp:214-262 +
:902-933` (the `e0ea027` standalone-instrument hunk). Comment rewritten
to "passing-gate guarding the sticky-bit readback-superset invariant
(under §0 + §2.5 v3)"; accumulator stays (demote-not-delete per
`debug_instrumentation_rule.md`). Tautology caveat at §6.4.

### 2.1 LEAVE sites

CARRIED FORWARD UNCHANGED FROM v2 §2.1. The full table of 10 sites
that intentionally remain on coarse `canBeSeen()` / `inView` — Building
(`bldng.cpp:1081`), Gate (`gate.cpp:599`), Artillery (`artlry.cpp:1407`),
Turret (`turret.cpp:2034, :2048`), mouse-pick gates (`objmgr.cpp:2645,
:2714, :2757`), inner pre-submit work-prep (`bdactor.cpp:1145, :1991,
:2137, :3908, :4369`), mover combat gates (`mech.cpp:6453-6513,
gvehicl.cpp:3944-3959`), mover `oldInView` cache (`mover.cpp:3470`),
producer-side parity reference (`objmgr.cpp:207`
`emitGpuCullRecord`), and TerrainObject lifecycle gate (`terrobj.cpp:796`
— contract-coarse per `cull_gates_are_load_bearing.md`; alpha-Stage 1's
job, not 0.5). Re-grep at write-time of each prior to patch.

---

## 2.5 Sticky-bit shader change (NEW — Change A)

Separate sibling commit, lands **before or alongside** the §2/§4
revert-of-e0ea027. §4.6 specifies ordering. This commit MUST land
intact (do not split shader from C++):

### 2.5.1 Edits

**`shaders/gpu_cull_block_rollup.comp`:**

DELETE the `uniform int uFrameStamp;` decl at `:67` (re-grep at
write-time).

REPLACE `:80`:
```glsl
// OLD:
atomicMax(blockVisBits[blk], uint(uFrameStamp));
// NEW (sticky-bit):
atomicOr(blockVisBits[blk], 1u);
```
Rewrite the surrounding comment (`:74-79`) to describe sticky-bit
semantics ("once stamped, stays stamped for the mission; per-mission
reset at `gpu_cull_compute.cpp:779-784` is the sole zeroing point").

**`shaders/gpu_cull.comp`:**

DELETE the uniform decls at `:123-124` (re-grep at write-time):
```glsl
uniform int uFrameStamp;
uniform int uHistoryK;
```
KEEP `uniform int uBlockCount;` at `:125` — M3 fail-OPEN clamp is
preserved.

REPLACE `:286-288`:
```glsl
// OLD:
bool effVisible = (rec.blockIdx < uint(uBlockCount)) &&
    ((uint(uFrameStamp) - blockVisBits[rec.blockIdx]) <
     uint(uHistoryK));
// NEW (sticky-bit; M3 short-circuit clamp PRESERVED):
bool effVisible = (rec.blockIdx < uint(uBlockCount)) &&
    (blockVisBits[rec.blockIdx] != 0u);
```
Rewrite the surrounding `:276-285` comment block to "sticky-bit admit:
this prop's terrain block has been frustum-visible at any point during
this mission (set by the rollup `atomicOr`, cleared only by the
per-mission one-shot zero at `gpu_cull_compute.cpp:779-784`). The
admit set is monotonically growing per mission; superset-by-construction
of any K-frame window." Preserve `:282-285` M3 fail-OPEN commentary
(short-circuit semantics still load-bearing for unbound binding-13
safety).

**`GameOS/gameos/gpu_cull_compute.h`:**

DELETE `:40`:
```cpp
static constexpr uint32_t GPU_CULL_BLOCK_TEMPORAL_K = 12u;
```
Opposite-direction grep at write-time confirms only readers are in
`gpu_cull_compute.cpp` (the two delete sites below) — no other TU
references the constant. If a stray reference is discovered at
write-time, the deletion is blocked; surface to user.

**`GameOS/gameos/gpu_cull_compute.cpp`:**

DELETE `:79` (the file-scope counter decl):
```cpp
static uint32_t  s_cullFrameIdx = gpu_cull::GPU_CULL_BLOCK_TEMPORAL_K;
```

DELETE `:946-953` (the per-dispatch increment + lockstep stamp; the
comment block at `:946-951` deletes with it):
```cpp
// (DELETE)
++s_cullFrameIdx;
const GLint frameStamp = (GLint)s_cullFrameIdx;
```

DELETE `:997-1002` (the cull-program uFrameStamp + uHistoryK uploads):
```cpp
// (DELETE entire block)
const GLint locFS = glGetUniformLocation(s_c1bCullProgram, "uFrameStamp");
if (locFS >= 0) glUniform1i(locFS, frameStamp);
const GLint locHK = glGetUniformLocation(s_c1bCullProgram, "uHistoryK");
if (locHK >= 0) glUniform1i(locHK, (int)gpu_cull::GPU_CULL_BLOCK_TEMPORAL_K);
```

DELETE the rollup-program `uFrameStamp` upload at `:1143` (re-grep at
write-time; the `glGetUniformLocation(s_rollupProgram, "uFrameStamp")`
+ paired `glUniform1i` go together).

**KEEP:**
- `:986` `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BLOCK_VIS_BINDING,
  s_blockVisBuf)` on the cull program (R1; the M3 clamp + sticky read
  both need binding-13 bound).
- `:1003-1005` `uBlockCount` upload (M3 fail-OPEN clamp; unchanged).
- `:1133` rollup-side `glBindBufferBase` (rollup still writes to binding
  13).
- The per-mission one-shot zero at `:779-784` (sticky-bit's sole
  initializer).
- The props-less early-return free at `:541-542`
  `glDeleteBuffers(&s_blockVisBuf); s_blockVisBuf=0; s_blockCount=0;`
  (Gate2 MAJOR-1; the M3 clamp + cross-mission safety invariant
  carries through unchanged).
- The DELETED per-frame `glClearNamedBufferSubData(s_blockVisBuf,...)`
  at v5-plan's `~:921-923` STAYS DELETED. Sticky-bit makes this even
  more obviously correct: a per-frame zero would re-arm every block to
  "never seen" every frame and reduce sticky to strict-only.

### 2.5.2 What's preserved vs deleted (audit)

| Item | Disposition |
|---|---|
| `blockVisBits[]` SSBO (binding 13, `gpu_cull_compute.cpp:769-771`) | KEEP |
| Per-mission one-shot zero (`gpu_cull_compute.cpp:779-784`) | KEEP |
| Per-frame zero (deleted in `056c365`) | STAYS DELETED |
| Props-less typeCount==0 free + s_blockCount=0 (`:541-542`) | KEEP |
| Rollup binding-13 bind (`:1133`) | KEEP |
| Cull binding-13 bind (`:986`) | KEEP |
| `uBlockCount` upload (`:1003-1005`) | KEEP — M3 clamp |
| M3 short-circuit `rec.blockIdx < uint(uBlockCount)` (`gpu_cull.comp:286`) | KEEP |
| `s_cullFrameIdx` (`:79, :952-953`) | DELETE |
| `GPU_CULL_BLOCK_TEMPORAL_K` constexpr (`gpu_cull_compute.h:40`) | DELETE |
| `uFrameStamp` uniform decl + uploads | DELETE (both programs) |
| `uHistoryK` uniform decl + upload | DELETE |
| Rollup `atomicMax(...,uint(uFrameStamp))` (`gpu_cull_block_rollup.comp:80`) | REPLACE with `atomicOr(...,1u)` |
| Cull `(F - blockVisBits[i]) < K` gate (`gpu_cull.comp:287-288`) | REPLACE with `blockVisBits[i] != 0u` |
| `else if (cat == CAT_STATIC_PROP)` sibling (`gpu_cull.comp:275`) — option (b) confinement | KEEP — temporal admit still confined to static props |
| `actorVisBits[i] = visible` raw self-latch (`:250`) | KEEP — R4 invariant unchanged |
| Rollup → barrier → cull frame ordering | UNCHANGED |

### 2.5.3 Per-mission reset is load-bearing under sticky-bit

The per-mission one-shot zero at `gpu_cull_compute.cpp:779-784` is now
the SOLE delimiter of admit-set lifetime. If a future refactor moves,
folds, or removes that clear, sticky-bit stamps leak across missions
and the admit set grows unboundedly across the session. Add a comment
at the zero site stating "sticky-bit invariant: this is the only
clearing point; per-frame zero is intentionally absent; cross-mission
free in compute_freeC1b (line ~488) is the only other path that
resets state." Reviewer-discipline checkpoint at the §0 prerequisite
review — savegame restore relies on this zero firing on
`compute_buildIndirectBuffer` (`Mission::load` patch).

### 2.5.4 Build/deploy notes

Shader edit + C++ edit MUST deploy in lockstep
(`memory/shader_exe_deploy_lockstep.md`). Class-layout-equivalent C++
change is bounded (file-scope `static` removal — no header layout
change at this commit; the §4 `appear.h` change is a separate commit).
`--clean-first` is NOT strictly required for this commit alone, but
RECOMMENDED because the `uHistoryK` / `uFrameStamp` symbol removal can
leak via the GLSL include / cull_program build path. Mandatory log
check: cull + rollup program link logs CLEAN; absence of
"uniform 'uFrameStamp' not found" warnings.

---

## 3. The empirical precondition (MANDATORY — slice STOPS if not met)

The claim **"readback is a same-frame superset of the coarse visible
set on HEAD, on mc2_10 at the zoomed-out worst-case operating point,
under sticky-bit admit"** is what the slice rests on.

### 3.1 Per-mission risk acknowledgment

Per v2 §3.1 user sign-off: precondition is **mc2_10 ONLY**. Other four
tier1 missions (mc2_01, mc2_03, mc2_17, mc2_24) NOT
precondition-validated. Risk-mitigated by post-impl tier1 5/5 smoke +
user-driven play. v3 unchanged from v2 here.

### 3.2 Procedure

1. Build a clean HEAD deploy (§0 prerequisite applied; §2.5 sticky-bit
   shader edit applied; NO §4 Stage 0.5 patch applied).
2. Run user-driven with `MC2_TOBJ_PARITY=1 MC2_GPU_CULL=1
   MC2_GPU_CULL_READBACK=1` on mc2_10 only, worst-case zoomed-out
   big-map camera, fast pan over screen-edge transitions.
   `MC2_TOBJ_PARITY` is in the run_smoke env allowlist
   (`scripts/run_smoke.py:281` grep-verified this session).
3. Collect at least 5 `[TOBJPARITY v1] event=summary` lines from the
   120-frame roll.
4. Compute `violations / samples`. **Two-tier go/no-go (K-ladder
   RETIRED):**

   - **GO (re-arm justified):** sustained violation rate `< 0.5%` AND
     zero 120-frame window above 2%. Ship Stage 0.5.
   - **STOP:** sustained `>= 0.5%` OR any 120-frame window `>= 2%`.
     Investigate WHY sticky-bit (a strictly-conservative superset) is
     failing to be a superset of the coarse cull. Candidates: (a) the
     readback's `conservative-OR + dilation` chain (`gpu_cull_readback.cpp:71,
     :670, :737`) is not actually producing a coarse superset — a
     separately-tracked readback-quality gap, defer to alpha-Stage 1;
     (b) per-actor readback handle-key mismatch (snapshot built at
     `objmgr.cpp:1911` for a different actor-handle space than the
     `getHandle()` injected at the patch site — surface as a §4 audit
     item); (c) cross-frame race between
     `readback_buildActorVisSnapshot` and the §2 read site (re-verify
     `objmgr.cpp:1911` runs BEFORE the TerrainObjects update zone).
     STOP and surface to user; do NOT escalate K (no K exists).

### 3.3 K-extension ladder — RETIRED

v2 §3.3's K=12 → K=24 → K=36 → STOP ladder is gone. Sticky-bit has no
K; YELLOW remediation under v2 was "extend the admit window in time"
which sticky-bit does in the most extreme form possible (infinity). If
sticky-bit fails as a superset, the failure is NOT a too-narrow window;
it is structural — typically (a) readback non-superset, (b) actor-handle
mismatch, or (c) snapshot-ordering race. Each requires a different
investigation, not a K bump. Surface to user with the suspect class
identified.

### 3.4 First-window special-case — STILL REMOVED

v2 §3.4 (carved-out by §4.5 K-warmup) is retired by deletion in v3.
Sticky-bit has no first-window transient by construction: the FIRST
frame a block enters the frustum, the rollup sets its bit; from that
frame forward, every subsequent frame admits it. The "first K frames
inert" failure mode is gone.

---

## 4. The implementation shape

"**Revert `e0ea027` minus the comment-only hunks**" with line-drift
reconciliation — UNCHANGED from v2. Recipe:

1. `git show aeceb2c` produces the canonical add-diff. Apply by hand
   (not via `git revert` of the revert).
2. Drop the aeceb2c `objmgr.cpp` comment-only hunk.
3. Reconcile each cited line against current HEAD:
   - `appear.h`: ctor / init / accessor / setter.
   - `terrobj.cpp:796` (lifecycle), `:945` (render), `:1015` (shadow).
   - `bdactor.cpp:1302, :1838, :4007, :4268` (4 gate flips);
     recalcBounds insertion for Bldg (HEAD method start around the
     terminal `return(inView)` in `BldgAppearance::recalcBounds`) and
     Tree (HEAD `TreeAppearance::recalcBounds`).

### 4.1 Subclass init chain

CARRIED FORWARD FROM v2 §4.1. All 6 `*Appearance::init` overrides
(`VFXAppearance` `code/actor.cpp:227`, `BldgAppearance`
`mclib/bdactor.cpp:611`, `TreeAppearance` `mclib/bdactor.cpp:3541`,
`GenericAppearance` `mclib/genactor.cpp:251`, `GVAppearance`
`mclib/gvactor.cpp:739`, `Mech3DAppearance` `mclib/mech3d.cpp:1060`)
chain to base `Appearance::init` on line 2 of body. Base must
initialize `renderVisible = TRUE` (fail-open). Write-time re-grep
required if a new subclass added between v3 and execution.

### 4.2 Single atomic commit (§2 render-gate repoint)

Same scope as v2 §4.2: `appear.h` ~+45, `terrobj.cpp` ~+30 net,
`bdactor.cpp` ~+30 net (6 gate flips + 2 recalcBounds defaults).

### 4.3 Full relink mandatory

Header change at `appear.h` alters Appearance class layout. Per
`feedback_class_layout_change_needs_clean_first.md`:

```
cmake --build build64 --config RelWithDebInfo --target mc2 --clean-first
```

### 4.4 Deploy

`cp -f` per file + `diff -q`. NEVER `cp -r`.

### 4.5 K-warmup — REMOVED (v2 §4.5 superseded by Change A)

v2's §4.5 K-warmup-via-loading-screen pattern is OBSOLETE. Sticky-bit
admit dissolves the first-K-frames transient by construction (the
intro pan naturally admits blocks on first-frustum-entry and they stay
admitted permanently — no rolling K-frame window exists to be in a
warmup state). The external reviewer's B.1 concern (running full
`Mission::update()` inside the loading-screen wait loop risks
gameplay-state side effects: AI, triggers, animations, scripted events,
sounds, timers — a worse class of bug than a render pop) is dissolved
by Change A. No warmup code is needed; the §4.5 pattern is HISTORICAL.
v2's §4.5 (paragraphs 1-5) is preserved by reference at
`docs/superpowers/specs/2026-05-19-alpha-stage-0-5-rearm-readback-render-gate-design-v2.md`
§4.5 for the K-window context if future work resurrects the pattern.

### 4.6 Commit ordering between §2.5 (sticky-bit) and §4 (revert-of-e0ea027)

The §2.5 shader/C++ sticky-bit edit and the §4 Appearance-renderVisible
revert are independent in code (no overlapping files except both
deploy into v0.4 lockstep). Order:

1. §0 prerequisite (saveload.cpp `compute_buildIndirectBuffer`) lands
   first, gated by savegame-restore smoke.
2. §2.5 sticky-bit lands second, gated by §3 precondition GO on mc2_10
   (the precondition explicitly runs against sticky-bit live, NOT
   against K=12).
3. §4 (§2 substitutive contract) lands third, atomic single commit,
   `--clean-first` mandatory, gated by §6 substitutive proof gate
   (post-impl tier1 5/5 + visual canary on mc2_10).

If §3 precondition fails under sticky-bit, §4 is blocked — investigate
the suspect class per §3.2 STOP, do NOT proceed.

---

## 5. Risk surface

v2's risk surface is mostly carried forward; updates noted.

**R-aeceb2c-1 / -2 / -3 / -4** (v2): the `aeceb2c` historical inner-gate
lesson is still applied; mover overrides still don't call base; v2 §6.5
visual canary is still the post-impl gate. UNCHANGED.

**R-NEW-1 — Memory cost of `blockVisBits[]`:** Sized at 128*128
uint32 = 64 KiB (`gpu_cull_compute.cpp:767-770` grep-verified). Not a
concern per `feedback_ram_cost_not_a_concern_below_500mb.md`. Under
sticky-bit, the buffer's lifetime per mission is unchanged (one-shot
zero at allocation, props-less free + zero, `Mission::destroy` free at
`:483-488`). UNCHANGED from v2.

**R-NEW-2 — First-K-frames transient:** DISSOLVED. Sticky-bit has no K.
v2's "DISSOLVED by §4.5 K-warmup" framing replaced with "DISSOLVED by
Change A sticky-bit." Status: not a Stage 0.5 risk.

**R-NEW-3 — Savegame restore stale block state:** ADDRESSED by §0
prerequisite commit. Under sticky-bit this risk is MORE acute (not
less): without §0, savegame restore loads against the prior mission's
sticky stamps and admits unrelated blocks for the entire restored
session. §0 + the per-mission one-shot zero at
`gpu_cull_compute.cpp:779-784` together close the loophole — but ONLY
if §0 lands. Status: blocking-pre-Stage-0.5; same as v2.

**R-NEW-4 — TOBJPARITY duplicate expression:** Cosmetic. Probe-tautology
caveat at §6.4 unchanged. UNCHANGED from v2.

**R-NEW-5 — Conservative-OR + dilation primitives:** `gpu_cull_readback.cpp`
`useOr` at `:71`, `:670`, motion-tolerance at `:737`. Verify via
precondition `event=motion_tolerance` summary line. UNCHANGED from v2.

**R-NEW-6 — Static-shadow popping out of scope:** alpha-Stage 0.6's
problem. UNCHANGED from v2.

**R-NEW-7 — mc2_10-only precondition envelope:** Tier1 5/5 post-impl +
user-driven play surfaces per-mission gaps. UNCHANGED from v2.

**R-NEW-8 (NEW v3) — Sticky-bit "admit-only-grows" interaction with
prop destruction.** Under K=12, when a static prop was destroyed
mid-mission, its block's stamp aged out within 12 frames and the block
returned to "not temporally admitted" — implicit garbage collection
via the time bound. Under sticky-bit, the block stays admitted
PERMANENTLY for the remainder of the mission.

Concretely: per-actor render gating still flows through Stage 0.5's
`renderVisible` → readback chain, which is keyed on actor handle. When
an actor is destroyed, `TerrainObject::destroy` at `code/terrobj.cpp:1026`
(grep-verified) removes it from the world; subsequent
`readback_buildActorVisSnapshot` at `objmgr.cpp:1911` no longer sees
the actor; the actor's `GpuActorRecord` slot no longer feeds
`recs[]` and is not re-stamped by the rollup. So the destroyed prop
itself stops rendering immediately — sticky-bit does NOT stale-hold
*destroyed actors*.

What sticky-bit DOES hold permanently is the block's "this block has
ever been seen during this mission" bit, which means: *other surviving
static props whose `blockIdx == destroyed prop's blockIdx`* keep their
temporal-admit forever after the block has been observed once. That
is correct sticky-bit semantics ("admit-only-grows = monotonic superset
of frustum-visible"); it is what the user explicitly accepts. The
overdraw consequence is bounded by `bucketCapacity` (the per-bucket
slot cap in `gpu_cull.comp:266-268`), not by stamp count.

Per `cull_gates_are_load_bearing.md` and
`parallel_mission_setup_paths_probe_which_one.md`: the destroy gate
itself stays on coarse `inView` (LEAVE-site §2.1 / v2 §2.1) — that
gate is the contract-coarse lifecycle gate at `terrobj.cpp:796`. The
visual canary at §6 must verify "no observable lag in static-prop
drop-out when an actor is destroyed" — destroyed-prop drop-out is the
per-actor render gate (`renderVisible` driven by readback handle) and
should be immediate, NOT bound to block-stamp lifetime. K-window
implicitly aged out the block stamp; sticky-bit does not — but the
per-actor `renderVisible` is what actually gates the render, so this
is a correctness reasoning step, NOT a code change. The §6.5 visual
canary explicitly checks this.

**R-NEW-9 (NEW v3) — Fail-open `renderVisible = TRUE` under
stock-default config.** Reviewer concern B.2. Grep result this session:
- `MC2_GPU_CULL_READBACK` defaults OFF (`gpu_cull_readback.h:8`
  "default 0"; `gpu_cull_readback.cpp:62` `s_enabled = (getenv("MC2_GPU_CULL_READBACK") != nullptr)`).
- `g_useGpuStaticProps` defaults FALSE (`gos_static_prop_batcher.cpp:28`
  `bool g_useGpuStaticProps = false`; toggled at runtime only via RAlt+0
  at `GameOS/gameos/gameosmain.cpp:345`).

**Stock-default path (env unset, RAlt+0 not pressed):**
- `MC2_GPU_CULL_READBACK=0` → `readback_isEnabled() == false` →
  `TerrainObject::update` injection sets
  `appearance->setRenderVisible(true)` (the `?:` falls to the `true`
  branch).
- `g_useGpuStaticProps == false`.
- Render gate `if (renderVisible || g_useGpuStaticProps)` becomes
  `if (true || false) == if (true)` → render EVERY static prop
  unconditionally.

The OLD gate `if (inView || g_useGpuStaticProps)` was `if (coarseCull
|| false) == if (coarseCull)` — the load-bearing coarse-angular cull.
Under stock-default, **Stage 0.5 broadens the render path from
coarse-cull to all-statics-unconditionally.** This IS a real perf
regression risk in the stock-playable configuration.

Two implications:

1. **Stock install must remain playable** per
   `memory/stock_install_must_remain_playable.md`. A configuration
   where `g_useGpuStaticProps=false` AND `MC2_GPU_CULL_READBACK=0` is
   stock-default. Rendering every static prop unconditionally on a
   bigger mission (Carver5O-style high-prop maps, though those are
   out-of-scope per `feedback_offload_scope_stock_only.md`) could
   regress frame time materially.

2. **Resolution (USER-LOCKED 2026-05-20):** option (a) adopted. Fail-open
   value is `inView` (the just-computed coarse cull from `recalcBounds()`,
   in scope at the injection site `code/terrobj.cpp:~796`), NOT `true`.
   Concretely, the injection at `terrobj.cpp` is
   `setRenderVisible(readback_isEnabled() ? readback_isActorVisibleLagged(handle) : inView)`.
   Under stock-default (`MC2_GPU_CULL_READBACK` unset AND
   `g_useGpuStaticProps == false`), `renderVisible` becomes `inView`,
   gate evaluates `(inView || false) == inView` — byte-identical to
   pre-Stage-0.5 coarse cull. Under readback-on, `renderVisible`
   becomes the lagged readback, gate evaluates `(readback || g_useGpuStaticProps)`
   — the Stage 0.5 design intent.

   The (b) alternative (document broadening as intentional fail-open;
   rely on readback being default-on in nifty-mendeleev) is REJECTED:
   contradicts `memory/stock_install_must_remain_playable.md`.

**v3 specification:** ADOPT (a) — the injection is
`setRenderVisible(readback_isEnabled() ? readback_isActorVisibleLagged(...) : inView)`.
The fail-open value becomes `inView` (the coarse cull just computed by
`recalcBounds`) rather than blanket `true`. Under stock-default this
makes the gate read `(inView || false) == inView` — byte-identical to
pre-Stage-0.5 behaviour. Under readback-on it makes the gate read
`(readback || g_useGpuStaticProps)` — the Stage 0.5 design intent.
Documents in commit message: "Fail-open changed from `true` to `inView`
per v3 reviewer-B.2 stock-playable invariant." The `Appearance::init`
default of `renderVisible = TRUE` is fine because it is overwritten by
the injection every frame; the init default only matters for the brief
window between construction and first `TerrainObject::update`, which
is dominated by other lifecycle work anyway.

(If user prefers (b) — pure fail-open per `aeceb2c` — that decision
should be made explicit at execution; (a) is the conservative
recommendation here.)

---

## 6. Substitutive proof gate (Stage 0.5)

Order matters; do not advance past a failure.

1. **§0 prerequisite committed** + savegame-restore smoke passed.
2. **§2.5 sticky-bit committed**, deployed lockstep with shaders.
   Cull + rollup program link logs CLEAN.
3. **§3 precondition GO** on mc2_10 worst-case under sticky-bit.
   If STOP, investigate per §3.2 candidate classes; do NOT escalate K
   (no K).
4. Build §4 `--clean-first` (class layout change). Deploy v0.4
   per-file.

### 6.3 Tier1 smoke

`--tier tier1 --duration 30 --kill-existing`, no `--with-menu-canary`.
5/5 missions complete; no streaks; no destroys regression; mech canary
clean (per `cull_gates_are_load_bearing.md`).

**KHR_debug fold (Tier 1.2):** add `--gl-debug-fatal` (run_smoke flag
at `scripts/run_smoke.py:165-169` + env propagation at `:346-347`
grep-verified) if the Tier 1.2 abort path has landed (`MC2_GL_DEBUG_FATAL`
in `gameosmain.cpp` / `gos_graphics.cpp`). The 0b41e87→c8b7ac0 commit
on this branch added `MC2_GL_DEBUG_FATAL` env handling — fold IS
available; default to ON for Stage 0.5 smoke.

### 6.4 TOBJPARITY — NOT load-bearing as passing gate

UNCHANGED FROM v2 §6.4. Post-patch the probe reads `renderVisible` (the
value the patch just stored) — tautological zero by construction. The
load-bearing gate is the §3 PRE-patch precondition; the probe is a
desync canary, not a passing gate.

### 6.5 Visual canary (render-only)

User-driven worst-case zoomed-out big-map fast pan on mc2_10: no
observable static-prop / tree pop on the screen edge during fast pan.
60s duration per `feedback_visual_iteration_smoke_60s.md`. Render
popping ONLY (shadow popping = alpha-Stage 0.6).

**NEW v3 canary (sticky-bit-specific):** during the 60s mc2_10 run,
trigger destruction of at least one static prop (artillery hit, demolish
order, or scripted destroy) and verify:
- The destroyed prop disappears immediately (within 1-2 frames of the
  destroy gate firing).
- No "ghost render" of the destroyed prop persists for the rest of
  the mission.
- Other surviving props in the same block continue rendering normally
  (sticky-bit "admit-only-grows" should not visibly degrade survivors).

The destroy gate stays on coarse `inView` (LEAVE-site §2.1
`terrobj.cpp:796`; alpha-Stage 1 carve-out). The per-actor render gate
(`renderVisible` driven by readback per-handle) is what causes the
destroyed prop to stop rendering; sticky-bit operates on block-level
admit, not per-actor admit. Both gates fire — the canary is the
end-to-end interaction.

### 6.6 Tracy telemetry (informational, NOT a gate)

Capture `GameLogic.Units.TerrainObjects` mean before/after at the same
camera. Sub-ms recovery bonus, not success-definition. Charter §2
baseline is 1.17ms.

**Failure analysis (v3-updated):**
- §6.3 (tier1) fails on a non-mc2_10 mission → R-NEW-7 hit; STOP,
  file per-mission gap, defer.
- §6.5 (visual) fails render-popping → discrepancy between TOBJPARITY
  tautology and visual: investigate (handle-keyed snapshot
  structurally blind to a per-actor failure mode; missing injection
  site; subclass added since §4.1).
- §6.5 (destroyed-prop ghost-render): destroyed prop persists →
  readback handle desync OR `setRenderVisible` injection runs on a
  destroyed actor (post-destroy update path). Surface; do NOT ship.
- §6.5 (block survivors): if surviving props in destroyed-prop's
  block ALSO drop out → sticky-bit logic error (block bit cleared on
  destroy somehow; should NOT happen since destroy doesn't touch
  `blockVisBits`). Surface; investigate `:541-542` props-less free or
  cross-mission reset firing mid-mission.
- §6.3 + §6.5 pass but shadow popping → R-NEW-6; ship Stage 0.5,
  defer to alpha-Stage 0.6.

---

## 7. Open questions (v3 only — distill from v3 changes)

For the EXECUTOR session:

1. **`uFrameStamp` orphan check (§2.5).** Opposite-direction grep at
   write-time for any OTHER consumer of `uFrameStamp` /
   `s_cullFrameIdx` / `GPU_CULL_BLOCK_TEMPORAL_K` beyond the 4 sites
   identified (decl/seed, increment, two uniform uploads, header
   constexpr, rollup uniform decl). Surfaced this session: NONE.
   Confirm at write-time.

2. **`gpu_cull_patch.comp:64` note about binding 13 collision (v5
   plan reference).** Grep at write-time — the patch shader's binding
   15 history note may still reference binding-13 as "BlockVis"; that
   is informational, not a code path. If the note is now stale (e.g.
   "binding 13 was BlockVis, now sticky"), refresh the comment in the
   sticky commit.

3. **`readback_isEnabled` declaration line.** `gpu_cull_readback.h:28`
   (v2's citation was correct; v3 re-grep adversarial-review corrected an
   off-by-one — `:27` is the comment, `:28` is the declaration). Verify at
   write-time when applying the §2 injection.

4. ~~Fail-open injection: `true` vs `inView` (R-NEW-9 / §5).~~ RESOLVED
   2026-05-20: USER-LOCKED to `inView`. §2 injection updated accordingly.

5. **Base `Appearance::init` initializes `renderVisible = TRUE`.** As
   v2 §4.1: write-time confirm in `mclib/appear.cpp` (or ctor body) —
   the value matters until the first `TerrainObject::update` overwrites
   it. With v3's R-NEW-9 (a) recommendation, the default is harmless
   (overwritten immediately by the injection's `: inView` branch); with
   (b) it is the broadening risk surface.

6. **Per-mission reset comment refresh (§2.5.3).** Add a comment at
   `gpu_cull_compute.cpp:781` (the canonical `glClearNamedBufferSubData`
   call; the surrounding scope is `:779-784`) — "sticky-bit invariant:
   this is the sole clearing point; per-frame zero intentionally absent"
   — so a future refactor doesn't silently fold the one-shot zero into a
   per-frame zero and degrade sticky to strict-only.

For the USER (block only if §3 precondition is STOP under sticky-bit):

7. **Fail-open recommendation.** Confirm R-NEW-9 (a) — fail-open to
   `inView` not `true` — is the intended default. Pre-authorized by
   v3 recommendation but a structural design choice worth confirming.

---

## 8. References

- Commits: `aeceb2c` (original meta-fix), `e0ea027` (de-risk revert),
  `056c365` (block-temporal superset META-FIX — K-window producer),
  `bebb455` (original TOBJPARITY probe), `c8b7ac0` (HEAD; adds
  MC2_GL_DEBUG_FATAL).
- v2 spec (preserved):
  `docs/superpowers/specs/2026-05-19-alpha-stage-0-5-rearm-readback-render-gate-design-v2.md`.
- v1 spec (preserved):
  `docs/superpowers/specs/2026-05-19-alpha-stage-0-5-rearm-readback-render-gate-design.md`.
- v5 plan (K-window mechanism v3 supersedes):
  `docs/superpowers/plans/2026-05-18-gpu-cull-block-temporal-superset-meta-fix-v5.md`.
- Campaign charter:
  `docs/superpowers/specs/2026-05-19-gamelogic-decoupling-campaign-charter.md`
  §5.1 / §5.4.
- Testing strategy (Tier 1.2): `docs/testing-strategy.md`.
- Memories: `cull_gates_are_load_bearing.md`,
  `feedback_offload_must_be_substitutive_not_additive.md`,
  `gpu_direct_renderer_bringup_checklist.md`,
  `stock_install_must_remain_playable.md`,
  `feedback_offload_scope_stock_only.md`,
  `feedback_ram_cost_not_a_concern_below_500mb.md`,
  `feedback_active_worktree_is_gpu_driven_rendering.md`,
  `parity_probe_100pct_can_be_correct_redesign_report.md`,
  `parallel_mission_setup_paths_probe_which_one.md`,
  `feedback_class_layout_change_needs_clean_first.md`,
  `shader_exe_deploy_lockstep.md`,
  `feedback_visual_iteration_smoke_60s.md`.
- Code (re-grepped this invocation):
  `mclib/appear.h` (no `renderVisible` present — revert in place);
  `code/terrobj.cpp:796, 945, 1015, 1026`;
  `mclib/bdactor.cpp:611, 1163, 1296, 1302, 1832, 1838, 3541, 3926,
   4002, 4007, 4262, 4268`;
  `code/saveload.cpp:1567, 1584-1585`;
  `code/mission.cpp:2822 (MC2_GPU_CULL_READBACK default-off note)`;
  `code/objmgr.cpp:1907-1911 (snapshot ordering)`;
  `shaders/gpu_cull.comp:113-125, 247-300`;
  `shaders/gpu_cull_block_rollup.comp:50-82`;
  `shaders/gpu_cull_patch.comp:64`;
  `GameOS/gameos/gpu_cull_readback.h:8, 27, 75, 82`;
  `GameOS/gameos/gpu_cull_readback.cpp:62, 617`;
  `GameOS/gameos/gpu_cull_compute.h:40, 52, 72`;
  `GameOS/gameos/gpu_cull_compute.cpp:47, 79, 483-488, 541-542,
   767-784, 940-1005, 1133, 1143`;
  `GameOS/gameos/gameosmain.cpp:173, 343-345, 731-736, 835`;
  `GameOS/gameos/gos_static_prop_batcher.cpp:28`;
  `GameOS/gameos/gos_static_prop_killswitch.h:8`;
  `GameOS/gameos/gos_static_prop_batcher.h:297`;
  `scripts/run_smoke.py:165-169, 281, 303, 305, 346-347`.
