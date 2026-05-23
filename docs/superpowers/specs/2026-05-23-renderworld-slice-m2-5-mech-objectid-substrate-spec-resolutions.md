# M2.5 Spec — Open Questions Resolved

**Date:** 2026-05-23
**Resolves:** `2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-spec.md` Section 12 / Open Questions

Plan-writer MUST read this file alongside the spec. Resolutions override
spec author "leans" where they differ. Q6 includes a spec amendment.

---

## Q1 — Self-test canary shape

**Resolution: B — Separate `[MECH_OBJECT_ID_SELFTEST v1]` canary.**

Rationale: static props and mechs use different producer surfaces (M1.5
coalesce/legacy vs M2.5 GpuMechBatcher SSBO). Separate failure signals are
worth the extra log line. Matches M2-pre precedent
(`[GAMEPLAY_PICK_SELFTEST v1]` separate from `[RENDER_WORLD_SELFTEST v1]`).

**Plan implication:** add a NEW self-test wired into `RenderWorld::init()`
after the existing `runSubstrateSelfTest()` and `RunGameplayPickSelfTest()`.
Naming: `RunMechObjectIdSelfTest()` or similar. Log schema:
`[MECH_OBJECT_ID_SELFTEST v1] result=PASS|FAIL ...`

---

## Q2 — Reserved field naming in `GpuMechInstance`

**Resolution: A — Generic `_pad1/_pad2/_pad3`.**

Do NOT pre-name future terrain/VFX/overlay fields before those specs exist.
Rename only the one field actually consumed now: `objectIdRaw`.

**Plan implication:** struct grows 48B -> 64B by adding ONE field
(`uint32_t objectIdRaw`) plus generic padding to maintain std430 alignment.
Padding fields keep their `_padN` naming until a future spec consumes them.

---

## Q3 — Submit-time gating of `desc.objectIdRaw`

**Resolution: A — Unconditional CPU fill.**

`desc.objectIdRaw = mech.getRenderWorldHandle().raw();` always fires
regardless of `MC2_OBJECT_ID_BUFFER` env. The env gates the *shader output*
and *FBO attachment*, NOT CPU-side data preparation. Keeps instance data
stable so env-ON is a pure render-path toggle.

**Plan implication:** no `if (envFlag)` wrapper at submit site. Cost = 4B per
instance always (already accounted for in struct growth).

---

## Q4 — `mech_id_writes=N` counter timing

**Resolution: A — Ship in M2.5.**

M2.5 owns the mech writer, so M2.5 owns writer observability. Counter on
the existing `[MECHBATCHER v1] event=summary` line:

```
[MECHBATCHER v1] event=summary ... mech_id_writes=N
```

Gives an immediate substrate gate before M2.6 (pickup) depends on it.
M2.6 can then assert non-zero `mech_id_writes` before testing picks.

**Plan implication:** thread one `uint64_t` counter through `flush()`.
Increment on each instance fill when `objectIdRaw != 0`. Emit on summary.

---

## Q5 — Recon-vs-code discrepancy on `RenderWorld.h` include

**Resolution: no user action — spec already accommodates the add.**

Recon claimed `gos_mech_batcher.cpp` includes `RenderWorld.h`; grep proves
it doesn't. Spec adds the include in section 4.2.2. Adversarial review
should re-verify the firewall implication (is `gos_mech_batcher.cpp` in
SCOPE_DIRS? allowlist needed?).

---

## Q6 — MLR/CPU-fallback gap

**Resolution: A — Accept the gap. SPEC AMENDMENT: make it measurable.**

Do NOT block M2.5 or M2.6 on MLR retirement. But document the gap hard
and add an MLR-side observability counter so the assumption "MLR is rare"
becomes verifiable rather than asserted.

### Required documentation language (verbatim in spec + CLAUDE.md known-issues)

> MLR-rendered mechs do not write object IDs in M2.5.
> M2.6 pickup works only for GPU-batched mech pixels.
> If tier1 exercises MLR, pickup must fall back to legacy mover selection
> for those mechs and cannot claim full mech GPU-pick coverage.

### Required counter additions

Two counters on the existing `[MECHBATCHER v1] event=summary` line (M2.5)
AND a new always-on MLR-path counter wherever MLR mech draws fire:

```
[MECHBATCHER v1] event=summary ... gpu_mech_id_writes=N mlr_mech_draws=M
```

(or split across two log lines if MLR draws live in a different TU; either
shape is acceptable as long as both counters are surfaced per-mission.)

### Gating decision for M2.6 (read at M2.6 planning time, not now)

If tier1 ever shows `mlr_mech_draws > 0` on any mission:
- M2.6 MUST preserve mover-first legacy fallback for those mechs
- M2.6 CANNOT claim full mech GPU-pick coverage in CLAUDE.md
- The MLR path becomes a named blocker for the next slice that wants
  full coverage (e.g. an "M2.7 MLR-mech object-ID write" or "M2.7 MLR
  retirement" follow-up)

If tier1 consistently shows `mlr_mech_draws == 0` across all 5 missions
for ~3 ship cycles, the gap is provably-rare-in-practice and M2.6 can
ship without the conditional fallback warning.

**Plan implication:** grep for MLR mech draw sites (the agent can search
`mclib/mlr/` and `mclib/mech3d.cpp` for the CPU-fallback render path)
and add a draw-call increment. The counter is always-on (not env-gated)
because it informs the M2.5/M2.6 coverage decision.

---

## Adversarial review status

A background adversarial review of the spec is in flight as of these
resolutions. The reviewer may surface additional findings that override
or refine these answers. Plan-writer reads in order:

1. Spec (section 1-12)
2. This resolutions file (overrides spec leans)
3. Adversarial review file (overrides both IF the finding is CRITICAL or
   MAJOR; MINOR findings are advisory)

If adversarial review verdict is FAIL or CONDITIONAL-PASS, do NOT proceed
to plan-writer until those findings are addressed in a spec revision pass.
