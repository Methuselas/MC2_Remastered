// BRAIN-DECISION-INTENT-QUEUE-1: stub for getBrainTickIndex().
// In the full engine this is defined in warrior.cpp (static var accessor).
// The harness never enables MC2_BRAIN_INTENT_QUEUE so emitBrainIntent is never
// called, but the linker still needs the symbol resolved.
#include <cstdint>
uint32_t getBrainTickIndex() { return 0; }
