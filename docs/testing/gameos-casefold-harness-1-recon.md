# GAMEOS-CASEFOLD-HARNESS-1 — RECON RESULT: RED (no clean seam)

**Verdict:** Do NOT build. No behavior-preserving + valuable extraction target exists.
**Base:** nifty HEAD at recon time (post timing/shader/smoke integration).

## What was proposed

CROSS-SUBSYSTEM-AUDIT-RECON-1 listed GAMEOS-CASEFOLD-HARNESS-1 (YELLOW) — extract
the ASCII case-fold into a pure header + C++ harness, sibling to
GAMEOS-TIMING-MATH-HARNESS-1.

## Why it is RED on inspection

The casefold surface splits into a **live-but-not-cleanly-extractable** function
and **cleanly-extractable-but-dead** functions:

- `GameOS/src/platform_str.cpp:81` `S_strlwr(char*)` — **31 callers** (the live
  one). Windows path is `_strlwr` (C-locale dependent); non-Windows path is a
  `tolower()` loop (also locale dependent). A pure, locale-independent ASCII
  fold is **not byte-identical** to `_strlwr`/`tolower` for bytes >= 0x80.
  Replacing it would be a semantic change to a 31-caller function — precisely
  the "casefold becomes a behavior refactor" risk flagged in review. **Reject.**

- `GameOS/gameos/utils/string_utils.cpp:61/73` `StringToLower` /
  `StringToLowerSafe` — pure (clampable bounds + NUL + null-guard), but **dead**:
  declared in `string_utils.h`, **no callers** anywhere in the tree. Harnessing
  them tests code nothing runs (fake value).

- FST key normalization (`S_strlwr` + backslash->forward + `elfHash`) — the path
  the recon actually cared about — is **already extracted and unit-tested** in
  `mclib/fst_hash.{h,cpp}` (+ `tests/unit/test_hashing.cpp`). No gap there.

## Options if revived later (none recommended now)

1. **Drop.** Cleanest. No live, ASCII-guaranteed, cleanly-extractable fold exists.
2. Pure `ascii_tolower` helper that is NOT wired into production (visibility-only)
   — low value; production `S_strlwr` stays untested.
3. A FST-key-specific casefold contract *iff* FST keys are proven ASCII-only at
   all 31 `S_strlwr` call sites — large audit for marginal gain; fst_hash already
   covers the normalization contract.

## Disposition

Closed RED. Mirrors the discipline of marking a target RED on inspection rather
than manufacturing a behavior-changing or fake-green harness. Re-open only with a
concrete ASCII-guaranteed live seam (do not re-derive from the YELLOW recon row).
