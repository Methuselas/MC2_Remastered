// EDITOR-only stub for mc2_unitprofile_collect_witness.
//
// The shared debug-state-dump TU is compiled into the editor lib and references
// mc2_unitprofile_collect_witness (added by the UNIT-PROFILE-SEAM work). The editor does NOT
// need unit-profile witness data (a game-runtime debug feature), and pulling the real
// code/unitprofile_fit.cpp into EditRel cascades engine dependencies. Provide a no-op definition
// so EditRel links. The game target (mc2) links the real definition; this stub is editor-only
// (picked up by the editor/*.cpp glob), so there is exactly one definition per target.
//
// extern "C" linkage => the symbol name is unmangled, so a forward-declared (incomplete) pointer
// parameter matches the call site for linking; we never dereference it.

struct UnitProfileWitnessRow;

extern "C" int mc2_unitprofile_collect_witness(UnitProfileWitnessRow* /*out*/, int /*maxRows*/)
{
    return 0;  // no witness rows in the editor
}
