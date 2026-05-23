# Adversarial Review — RenderWorld Slice M2.5 Mech ObjectID Substrate Spec

- Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-spec.md`
- Date: 2026-05-23
- Reviewer mode: adversarial; grep-verified every load-bearing cite
- HEAD: `terrain-pbr-mod` worktree `nifty-mendeleev`

## Executive summary (5 lines)

The spec is technically sound on the load-bearing axes: std430 layout
math is correct, `flat` qualifier is mandated on both vert/frag sides,
`MC2_OBJECT_ID_BUFFER` macro propagation mirrors the shipped static-prop
pattern verbatim, and the META-FIX argument holds. **CONDITIONAL-PASS**
with 1 MAJOR (firewall allowlist claim is misleading — `GameOS/` is not
in `SCOPE_DIRS`, so the "allowlist add" is a no-op rather than a
required edit; spec implies enforcement that does not exist) and 3
MINORs (ring-size cost framing inaccurate, hot-reload risk discussion
missing the C++ struct-size lockstep failure mode, Q3 lean is correct
but the per-actor handle read is *not* free in env-OFF builds because
`getRenderWorldHandle()` returns a value, not a reference). No
CRITICALs found. Recommend addressing the MAJOR before plan-writing.

---

## Methodology

Per `.claude/skills/adversarial-plan-review.md`. Grep'd every cited
file:line in the spec; opened sibling files (`static_prop.vert/frag`)
to confirm the "mirrors M1.5" claim is structural not just nominal;
opened the firewall script to verify the allowlist mechanism.

Symbols verified (each `file:line` opened, content matched against
spec claim):

| Spec claim | File:line | Result |
|---|---|---|
| `GpuMechInstance` 48B + offset asserts | `gos_mech_batcher.h:35-51` | MATCH |
| `GpuMechSubmitDesc` field list | `gos_mech_batcher.h:88-106` | MATCH |
| `MECH_RING_FRAMES = 3u` | `gos_mech_batcher.h:119` | MATCH |
| `mech.vert` GLSL `GpuMechInstance` struct | `shaders/mech.vert:30-40` | MATCH |
| `mech.vert` instIdx + inst fetch | `shaders/mech.vert:79-80` | MATCH |
| `mech.frag` location 0/1 outs | `shaders/mech.frag:36-37` | MATCH |
| `mech.frag` end-of-main writes | `shaders/mech.frag:75-77` | MATCH |
| `static_prop.frag:44` `PerDrawEntry.objectIdRaw` | `static_prop.frag:44` | MATCH (`int objectIdRaw`) |
| `static_prop.frag:56-60` legacy `u_objectIdRaw` | `static_prop.frag:56-60` | MATCH |
| `static_prop.frag:68-72` location=2 out | `static_prop.frag:68-72` | MATCH |
| `static_prop.frag:174-183` body emit under macro | `static_prop.frag:174-181` | MATCH (line drift, content matches) |
| `gos_static_prop_batcher.cpp:510-521` prefix injection pattern | `gos_static_prop_batcher.cpp:510-521` | MATCH |
| `gos_static_prop_batcher.cpp:3` includes `RenderWorld.h` | `gos_static_prop_batcher.cpp:3` | MATCH |
| `gos_mech_batcher.cpp` does NOT include `RenderWorld.h` | grep returned 0 hits | CONFIRMED — spec correctly flagged recon discrepancy |
| `RenderWorld.h:74-85` `IsObjectIdBufferEnabled()` | `RenderWorld.h:73-85` | MATCH |
| `RenderWorld.h:116-120` `RenderObjectKind` | `RenderWorld.h:116-120` | MATCH |
| MLR fallback runtime gate (`g_useGpuMechs && ...`) | `mech3d.cpp:2531-2608` | MATCH (gate at 2538, fallback at 2608) |

---

## Findings

### CRITICAL

None.

### MAJOR

#### M1. Firewall allowlist claim is misleading — `GameOS/` is OUTSIDE the firewall scope

**Spec §4.2.2 + §9 + Q5** say:

> "The firewall script (`scripts/check-include-firewall.sh`) treats
> GameOS as the only valid TU set that may reach into `RenderWorld/`
> for the public header. Adding a second GameOS-side includer is
> allowlisted by precedent."

and

> "M2.5 adds: `gos_mech_batcher.cpp: includes RenderWorld/RenderWorld.h for IsObjectIdBufferEnabled (M2.5)`"

**Code reality** (`scripts/check-include-firewall.sh:22`):

```
SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL"
```

The script only walks `SCOPE_DIRS` and flags forbidden includes
*found in those dirs*. `GameOS/` is NOT in `SCOPE_DIRS`, so including
`RenderWorld/RenderWorld.h` from anywhere under `GameOS/` is **not
even examined** by the firewall script. The "precedent" of
`gos_static_prop_batcher.cpp:3` having the same include is real, but
neither include is policed.

The allowlist file (`scripts/check-include-firewall.allowlist`) is
for files **inside** `SCOPE_DIRS` that need a carve-out (e.g.
`RenderWorld/legacy/static_prop_backend.cpp` legitimately includes
`gos_static_prop_batcher.h`). Adding `gos_mech_batcher.cpp` to it is
mechanical no-op — the script never looks at that path.

**Impact:**
- Spec readers (and the plan author) will believe a firewall edit is
  required. It is not. The "no-op" allowlist entry is still harmless,
  but the spec creates a false sense of governance.
- More importantly: if M2.6+ slices propose policy enforcement on the
  `GameOS → RenderWorld` boundary, the spec's framing implies that
  enforcement already exists. It does not.
- The actual direction-firewall (engine-side `GameOS/` MUST reach
  RenderWorld only via public header) is enforced only by **header
  hygiene + reviewer discipline** today, not by the script.

**Recommendation:**
- Drop the allowlist edit from §9 / §4.2.2 / Q5. Replace with a single
  sentence: "`GameOS/gameos/gos_mech_batcher.cpp` adds
  `#include \"../../RenderWorld/RenderWorld.h\"`. Firewall script
  does not police `GameOS/`; the include is governed by reviewer
  discipline + the M1 spec's stated direction rule (`GameOS/ →
  RenderWorld/RenderWorld.h` only)."
- Optionally raise a follow-up: extend `SCOPE_DIRS` to include
  `GameOS GameAdapters mclib code` and write the inverse allowlist
  (these dirs MAY include only `RenderWorld/RenderWorld.h`,
  `RenderCore/Handle.h`, etc.). Out of scope for M2.5.

### MINOR

#### m1. Ring-size cost framing is inaccurate — `s_instanceCapacity` is dynamic, not capped at 512

**Spec §7 says:**

> "instance capacity per frame = s_instanceCapacity (~512 default;
> capped by ring init)"
> "ring delta = 3 * (32 - 24) = 24 KB"

**Code reality** (`gos_mech_batcher.cpp:283-298`):

```
neededInstances > s_instanceCapacity ||
...
s_instanceCapacity = std::max(neededInstances,
    s_instanceCapacity ? s_instanceCapacity * 2 : kInitialInstancesPerFrame);
```

`s_instanceCapacity` doubles on overflow; "default" is
`kInitialInstancesPerFrame` *until first growth event*, then it
ratchets. The 24 KB claim is a lower-bound for fresh-process
worst-case; sustained heavy missions ratchet higher and the delta
scales with the ratcheted capacity.

**Impact:** Negligible to the cost story (even at 4096 instances
ratcheted, ring delta = 3 × 4096 × 16 = 192 KB, still trivial). But
the spec's framing "capped by ring init" is wrong — it is **floored**
by ring init and grows from there.

**Recommendation:** Replace "capped by ring init (~512)" with "floored
by `kInitialInstancesPerFrame` and doubles on overflow; ring delta is
3 × capacity × 16 bytes; even at 4096 ratchet (8× growth) the delta
is ~192 KB. Still negligible vs bone SSBO."

#### m2. Threat-model §11 "Lockstep edit risk" lists the artifacts but understates the hot-reload failure mode

**Spec §11 "Lockstep edit risk"** correctly enumerates the three
artifacts and demands a single commit. But the discussion frames the
risk as "GLSL reads garbage at `inst.objectIdRaw`."

**Missed failure mode:** if shader hot-reload fires (a developer edits
`mech.vert`/`mech.frag` and the engine reloads them mid-session)
WITHOUT a matching C++ relink, the GLSL struct now expects 64 B per
record but the CPU ring is still writing 48 B strided records. The
GPU reads `inst.objectIdRaw` from what is actually the *next instance's*
`typeLodRecordIndex` — every instance after the first writes a wrong
type-index lookup, not just a wrong objectId. This is a **mesh-swap
class** wrong-mech crash, not a "wrong handle" misclick.

The AMD driver's behavior on size-mismatched std430 reads is to silently
return data from adjacent memory — no validation error, no GL_DEBUG
message. The bug only surfaces visually (wrong mech meshes rendering)
or via the M1.5 selftest asserting nonsense handles.

**Recommendation:** Add to §11 lockstep risk: "Hot-reload of shader
WITHOUT C++ relink produces silent stride-mismatch — the GPU reads
`objectIdRaw` from offset 48 of the OLD 48 B stride, which lands in
the *next* instance's `typeLodRecordIndex`. Visual symptom: wrong
mech meshes (worse than wrong handles). Mitigation: shader hot-reload
must be disabled during the lockstep commit, OR the developer must
`./mc2.exe` restart after every shader edit. Both are existing
project practice; spec just needs to call it out for this slice."

#### m3. Q3 lean ("unconditional read is free") understates the cost

**Spec Q3 says:**

> "Pros of unconditional: zero branching at submit time; one fewer
> code path; cleaner diff. Cons: every-actor every-frame read of a
> member that may be `Handle::invalid()` (cost: one mov; trivial)."

**Reality check** (`mech3d.h:487-489`):

The accessor returns a `RenderCore::RenderObjectHandle` **by value**
(POD struct, 32-bit bits field). Calling `.raw()` returns the bits.
On a release build with `-O2` this is plausibly inlined to one mov,
but in `RelWithDebInfo` (the project default per CLAUDE.md "always
`--config RelWithDebInfo`") inlining is decided per-call and the
accessor may emit a real function call. Plus the assignment to
`desc.objectIdRaw` is a separate store.

Cost is still trivial *relative to the submit-site work* (texture
slot resolution, fog packing, etc.) but the spec's "one mov; trivial"
framing presumes optimization that the project's standard build
flags don't always grant.

**Recommendation:** Replace "cost: one mov; trivial" with "cost: one
member load + one store at submit time; <10 ns at sustained submit
rate; not measurable against the existing submit-site work." Keeps
the lean (unconditional) but doesn't overclaim the optimization.

---

## Adversarial focus areas (per dispatch instructions)

### 1. SSBO layout grep-verify

**Verdict: MATCH.** std430 layout math holds.

- Existing `GpuMechInstance` (`gos_mech_batcher.h:35-51`): 4 × `uint32`
  (16 B) + `float[4]` (vec4, 16 B) + `float[4]` (vec4, 16 B) = 48 B.
  `static_assert(sizeof == 48)` confirmed.
- GLSL std430 mirror (`shaders/mech.vert:30-37`): same field order,
  same scalar/vec4 types. Compatible.
- Adding `uint32_t objectIdRaw` at offset 48 (4-byte aligned in
  std430, no padding needed before it) + three `_padN` to reach 64 B
  total. std430 trailing-pad for struct-in-array aligns to max member
  alignment which is `vec4` = 16 B. Without explicit `_pad`, the
  struct still occupies 64 B in the array (compiler trail-pads).
  With explicit `_pad`, the C++ side matches GLSL byte-exactly.
- New `static_assert(offsetof(objectIdRaw) == 48)` is correct.

**No finding.** Spec authors did the math; padding rationale is
sound; explicit `_pad` matches the `GpuMechVertex` precedent
(`gos_mech_batcher.h:25-31`) for layout hygiene.

### 2. Lockstep edit risk

See MINOR m2 above. Spec calls out the risk but understates the
silent-stride-mismatch failure mode. Add hot-reload caveat.

### 3. `flat` qualifier

**Verdict: MANDATED CORRECTLY.**

Spec §4.3.2: `flat out uint v_objectIdRaw;` (vert side).
Spec §4.4.1: `flat in uint v_objectIdRaw;` (frag side).
Spec §11 "`flat` qualifier mandatory for integer varyings" repeats
the discipline and notes link failure surfaces via
`[MECHBATCHER v1] event=shader_fail`.

Sibling precedent: `static_prop.vert:103-108` uses `flat out uint
v_flags / v_localVertexID / v_drawID`; `static_prop.frag:23-28`
uses `flat in uint` mirrors. M2.5 follows the same pattern.

**No finding.** Spec is correct and matches shipped precedent.

### 4. `MC2_OBJECT_ID_BUFFER` macro propagation

**Verdict: CORRECT.**

Spec §4.2.2 mirrors `gos_static_prop_batcher.cpp:510-521` verbatim:
build the prefix as `std::string`, append `"#define
MC2_OBJECT_ID_BUFFER 1\n"` when `RenderWorld::IsObjectIdBufferEnabled()`,
pass to `makeProgram(...)`. Confirmed `makeProgram` passes the prefix
through to BOTH vertex and fragment compile units (verified by reading
the static_prop call site at 524-528 — single prefix shared across
.vert + .frag). So both stages receive the macro definition.

CLAUDE.md rule honored: GLSL macros do NOT inherit C++ build flags;
the C++-side prefix is the only mechanism.

**No finding.**

### 5. Recon Q5 (RenderWorld.h include)

**Verdict: SPEC IS CORRECT, RECON WAS WRONG.**

Independent grep of `gos_mech_batcher.cpp` for `RenderWorld\.h|IsObjectIdBufferEnabled`:
**zero matches**. Spec §4.2.2 / Q5 correctly flag the recon
discrepancy and propose adding the include. Comparison with
`gos_static_prop_batcher.cpp:3` (`#include "../../RenderWorld/RenderWorld.h"
// M1.5: IsObjectIdBufferEnabled + objectIdRawForStaticPropRecipe`)
confirms the relative-path form.

**Firewall implications:** see MAJOR M1 above. Spec overstates the
governance — `GameOS/` is not in `SCOPE_DIRS`, so the allowlist
edit is a no-op.

### 6. Cost claim

See MINOR m1 above. The 24 KB figure is correct at default capacity
but `s_instanceCapacity` is dynamic, not "capped." Reframe.

### 7. MLR fallback gap — does any tier1 mission exercise it?

**Verdict: NEAR-ZERO IN PRACTICE; spec's lean is defensible.**

Runtime gate confirmed at `mclib/mech3d.cpp:2531-2608`:

```
if (g_useGpuMechs && g_useGpuMechCull) { ... }        // 2531
if (g_useGpuMechs && !mechGpuCullSkip) { ... }        // 2538
    gpuMechSubmitted = GpuMechBatcher::instance().submitActor(desc);
}                                                     // 2586
if (!gpuMechSubmitted && g_useGpuMechs) {             // 2595
    GpuMechBatcher::instance().recordCpuFallback(...)
}
mechShape->Render(true);  // CPU path — unchanged    // 2608
```

`g_useGpuMechs` defaults ON since 2026-05-09 (per spec §6 and CLAUDE.md
campaign notes); `submitActor` only returns false on (unregistered
type | u8 bone overflow | ring overflow | shader init failure). On
tier1 missions all mech types are registered at `Mission::init`
finalize time so Path B fires only for:
- `g_useGpuMechs=0` override (dev flag; not default)
- Late spawn between finalize and `finalizePending` call (per
  `gos_mech_batcher.h:140-146` campaign-resume edge case)
- Ring overflow at >512 instances (mc2_24 max=46, not reachable)

So **no tier1 mission under default config exercises Path B in
sustained gameplay.** Late-spawn frames may briefly hit it.

User-visible effect on those frames: Shift+click on a fallback-frame
mech returns `Handle::invalid()` → outcome=`miss` → caller falls back
to legacy 2D-bounds selection. Mech is briefly un-pickable for that
frame; resolves next frame.

**Spec's lean (accept the gap) is correct.** The dual-queue retirement
campaign will eventually delete `mechShape->Render(true)` entirely;
adding a separate `u_objectIdRaw` upload to `gos_tex_vertex_lighted`
solely to close this gap is anti-substitutive.

**No finding.** Acknowledge in plan as a known limit.

### 8. Open questions Q1-Q6 leans

| Q | Spec lean | Reviewer verdict |
|---|---|---|
| Q1 (extend M1.5 canary vs new) | Extend | **AGREE.** One canary, two sample modes — reuses `RenderWorld.cpp:233` infrastructure and the existing SKIP-on-background semantics. New env var adds noise. |
| Q2 (`_padN` vs reservation-named) | Generic `_padN` | **AGREE.** Speculative names age badly; rename when M3/M4 lands. |
| Q3 (unconditional submit-side write) | Unconditional | **AGREE with caveat (see MINOR m3).** Lean is right; cost framing is slightly overoptimistic. |
| Q4 (`mech_id_writes=N` counter) | Ship in M2.5 | **AGREE.** Cheap; visibility useful for both M2.6 and the MLR retirement campaign measurement. |
| Q5 (recon-vs-code RenderWorld.h discrepancy) | Add include + allowlist | **PARTIAL DISAGREE — see MAJOR M1.** Add the include; drop the allowlist claim. |
| Q6 (accept MLR gap for ship) | Accept (a) | **AGREE.** Per analysis in focus area #7; M2.5+M2.6 ship with the documented limit; substitutive closure is a successor slice or MLR retirement deletion. |

### Load-bearing memory cross-reference (skill §6)

Walked the CLAUDE.md / MEMORY.md load-bearing list:

| Memory file | Applies? | Spec addresses? |
|---|---|---|
| `glsl_preprocessor_does_not_inherit_cpp_build_flags.md` | YES | YES — §4.2.2 + §5 + §11 |
| `uniform_uint_crash.md` | NO (spec adds no `uniform uint`) | YES — explicitly noted §11 |
| `feedback_offload_must_be_substitutive_not_additive.md` | YES (META-FIX claim) | YES — §10 substitutive argument |
| `cull_gates_are_load_bearing.md` | INDIRECT (mech adapter lifecycle, M2 territory) | N/A — M2.5 reads existing `mechRenderHandle`; no lifecycle change |
| `shader_exe_deploy_lockstep.md` | YES (shaders + cpp ship together) | YES — §11 "Full relink discipline" |
| `mc2_argb_packing.md` | NO (no color packing changes) | N/A |
| `terrain_mvp_gl_false.md` / `clip_w_sign_trap.md` | NO (no matrix uploads added) | N/A |
| `feedback_offload_scope_stock_only.md` | YES (default-build correctness) | YES — §6 default-build incidence near-zero |

No load-bearing memory missed.

### Global-convention exhaustive census (skill §9)

**Does this slice change a cross-cutting state primitive?** Borderline.
M2.5 adds a *writer* to an existing MRT attachment (M1.5 substrate);
it does not change a global rendering convention (depth, blend, cull,
clip control, etc.). It IS a per-instance SSBO-field add, but the
SSBO is a single-binding-slot per-batcher buffer — there is no
"other writer of the same SSBO" to enumerate.

**Census not required** per skill §9 trigger criteria. The
attachment-2-writer set (`static_prop.frag`, `mech.frag`) is explicitly
enumerated in §8 grep gate and §2 roadmap.

**No finding.**

### Partial-landing hazard (skill §8)

**Verdict: SPEC CALLS THIS OUT.**

§11 lockstep risk + §10 ruling both demand a single commit covering
(`gos_mech_batcher.h` + `mech.vert` + `gos_mech_batcher.cpp`). The
plan author should add an explicit "do not land partial" gate but the
spec's posture is correct.

**Recommendation for the plan (not the spec):** explicit gate
"single PR; CI rejects PR if `git show --stat HEAD` does not touch
all three artifacts."

---

## Recommendations

### Before plan writing (must address)

1. **MAJOR M1:** Drop the firewall-allowlist claim. Replace with the
   reviewer-discipline framing. Document the absence of `GameOS/`
   from `SCOPE_DIRS` as a known gap (not M2.5's problem to solve).

### Before execution (should address)

2. **MINOR m1:** Reframe cost analysis §7 — `s_instanceCapacity` is
   dynamic; show worst-case ratcheted delta.
3. **MINOR m2:** Add hot-reload silent-stride-mismatch to §11 lockstep.
4. **MINOR m3:** Soften Q3 cost claim from "one mov; trivial" to
   "one load + one store; <10 ns; immeasurable."

### Architectural decisions that need user/advisor sign-off before revision pass

- **The firewall script gap** (MAJOR M1): is extending `SCOPE_DIRS`
  to police `GameOS → RenderWorld` direction in-scope for an M2.5+
  follow-up slice, or is it a permanent reviewer-discipline carve-out?
  This decision affects future RenderWorld arc slices (M3 terrain,
  M4 VFX) which will likely add more `GameOS/` → `RenderWorld/`
  includes. Defer if no opinion, but the spec implies governance that
  does not exist.

- **Q6 acceptance of MLR gap**: spec's lean is correct technically.
  Confirmation requested that "M2.6 ships with a known
  fallback-frame-mech un-pickable window" is acceptable product
  behavior. Cosmetic UX issue, not correctness.

---

REVIEW STATUS: CONDITIONAL-PASS
