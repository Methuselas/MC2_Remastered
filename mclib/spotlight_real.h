//---------------------------------------------------------------------------
// mclib/spotlight_real.h
//
// (E) SpotLight_ -> real illumination — env gate.
//
// Plan: docs/superpowers/plans/2026-05-20-spotlight-real-illumination-plan.md
// Task: T1.2 — env-gate plumbing for default-off gated implementation.
//
// Reads MC2_SPOTLIGHT_REAL once at process startup and exposes the boolean
// via an inline getter so any TU (mclib bdactor/mech3d, GameOS static-prop /
// mech batcher) can branch on it cheaply. Default-off in Stage 1; flipped
// to default-on in Stage 2 (T2.1) once visual canary passes; the entire gate
// is deleted in Stage 3 (T3.1).
//
// Banner contribution: gameosmain.cpp's [INSTR v1] enabled: ... line picks
// up the spotlight_real=N field at startup, matching the [INSTR v1] schema
// family convention.
//---------------------------------------------------------------------------
#ifndef MCLIB_SPOTLIGHT_REAL_H
#define MCLIB_SPOTLIGHT_REAL_H

namespace mc2_spotlight_real {

// One-shot env read; called from gameosmain.cpp at the same place the rest
// of the MC2_*_TRACE / MC2_GPU_* gates are read for the [INSTR v1] banner.
// Result is cached in a file-scope static inside spotlight_real.cpp.
void initFromEnv();

// Stage 1 / T1.2: default-off. Returns true ONLY when MC2_SPOTLIGHT_REAL is
// set to any non-"0" value at process startup. Stage 2 / T2.1 will invert
// the default; Stage 3 / T3.1 deletes the gate entirely.
bool isEnabled();

}  // namespace mc2_spotlight_real

#endif  // MCLIB_SPOTLIGHT_REAL_H
