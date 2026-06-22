# GAMEOS-PATHSEP-HARNESS-1

**Type:** behavior-preserving extraction + doctest. **Production-touch:**
`mclib/file.cpp` (key_source), `mclib/fst_hash.{h,cpp}`, `tests/unit/test_hashing.cpp`.

## Gap

Two path-key normalizers exist in the file subsystem:

- `fst_normalize_key` (FST elfHash path) — slash-only — **already extracted +
  unit-tested** (`tests/unit/test_hashing.cpp`).
- `NormalizeKey` (`mclib/file.cpp`, the loose-file / mod-overlay index key) —
  folds **case + slashes** in one pass — was a TU-local `static`, **untested**.

A miss in the loose-key path means a mod overlay silently fails to shadow a base
asset because two spellings of the same path canonicalize to different index
keys. Smoke is a poor oracle: stock data is already lowercase/forward-slash, so
the canonicalization only matters for mixed-case/backslash mod paths that a 30s
tier1 never exercises.

## Change (behavior-preserving)

Moved the `NormalizeKey` body **verbatim** into the existing `fst_hash` leaf TU
as `std::string fst_normalize_loose_key(const char*)` (backslash→`/`, else
`tolower` — identical to the prior inline loop). `file.cpp::NormalizeKey` now
delegates to it. The leaf TU has no engine includes, so the contract is
unit-testable game-free; `file.cpp` behavior is unchanged (same code, same
locale `tolower`).

Distinct from `fst_normalize_key`: that is slash-only (the engine lowercases
separately via `S_strlwr` on the FST path); the loose key folds case AND slashes
together. Both now live in `mclib/fst_hash.{h,cpp}`.

## Tests (`tests/unit/test_hashing.cpp`, game-free)

- folds case + backslashes in one pass
- canonicalizes spelling variants to ONE key (the mod-overlay contract)
- idempotent
- empty / null

## Why not a pure ASCII rewrite

`tolower` is locale-dependent for bytes ≥0x80; the extraction MOVES the existing
`tolower` loop rather than replacing it with a pure ASCII fold, so it is
byte-identical (no behavior change). Asset paths are ASCII, where `tolower` is
deterministic. (This is the discipline that made GAMEOS-CASEFOLD-HARNESS-1 RED —
there the live function was `_strlwr`, not movable as-is.)

## Gate

`file.cpp` is key_source. Verified: `mc2_tests` 7/7 (my 4 loose-key cases + 3
existing) ; full `mc2` build clean (no file.cpp/fst_hash warnings) ; tier1 **5/5**
on v0.4 (src_commit matches branch). NOTE: an unrelated pre-existing
`test_rendercore.cpp` case (`PipelineId::Count_` 5 vs expected 4, a parallel
lane's drift) fails in the full `mc2_tests` run — not touched by this slice.
