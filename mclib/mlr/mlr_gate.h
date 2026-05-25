//==========================================================================//
// File:    mlr_gate.h                                                       //
// Contents: MC2_DISABLE_GOSFX env-gate macro for the 4 MLR work-leaves.    //
//           Plan v6 §2 (Stage A1).                                          //
//                                                                           //
// MC2_GOSFX_GATE_EARLY_RETURN() expands to:                                 //
//   if (mc2::mlr_gate::is_disabled()) return;                               //
// where is_disabled() reads MC2_DISABLE_GOSFX at first call and caches.    //
// A2 flips the default; A4 deletes this header alongside mclib/mlr/.       //
//===========================================================================//

#pragma once

namespace mc2 {
namespace mlr_gate {

// Cached at first call; latches MC2_DISABLE_GOSFX env value.
bool is_disabled();

} // namespace mlr_gate
} // namespace mc2

#define MC2_GOSFX_GATE_EARLY_RETURN() \
    do { if (::mc2::mlr_gate::is_disabled()) return; } while (0)
