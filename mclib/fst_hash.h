//===========================================================================//
// mclib/fst_hash.h
//
// Leaf-TU declarations for the FST hash + key-normalization primitives.
// No transitive engine dependencies; safe to include from a doctest-driven
// unit test target.
//
// Background:
//   - elfHash() is the standard ELF dictionary hash used by FastFileFind
//     (see mclib/fastfile.cpp historical home; definition relocated here
//     so the tests/unit target can link it without pulling heap.h / ffile.h
//     / the full mclib graphics stack).
//   - fst_normalize_key() collapses backslashes to forward slashes. This
//     mirrors the inline loop in mclib/file.cpp around the elfHash() call
//     (memory/fst_forward_slash_invariant.md). Several callers normalize
//     in place; this leaf version writes to a separate dst.
//
// Engine code keeps using elfHash() via the existing fastfile.h decl;
// fastfile.h is left untouched.
//===========================================================================//

#ifndef MCLIB_FST_HASH_H
#define MCLIB_FST_HASH_H

#ifdef __cplusplus
extern "C" {
#endif

// ELF dictionary hash. Byte-stream-deterministic; case-sensitive.
// Matches the historical definition in mclib/fastfile.cpp 1:1.
unsigned long elfHash(const char* name);

// Write a slash-normalized copy of src into dst (null-terminated).
// Every '\\' byte becomes '/'; all other bytes pass through.
// dst must have room for strlen(src)+1 bytes.
void fst_normalize_key(char* dst, const char* src);

#ifdef __cplusplus
}
#endif

#endif // MCLIB_FST_HASH_H
