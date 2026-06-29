#ifndef UNITPROFILE_H
#define UNITPROFILE_H
// UNIT-PROFILE-SEAM-1: static baseline facets (ObjectType) + current runtime
// facets (Mover). Slice 1 facet = capability.
#include "capability.h"

bool mc2_unitprofile_parse_enabled(const char* envValue);   // pure, testable
bool mc2UnitProfileDataEnabled();                            // cached getenv

class UnitProfile {                  // static, on ObjectType (flyweight)
  public:
    CapabilitySet baselineCapabilities;
};
struct UnitRuntimeState {            // mutable, on Mover
    CapabilitySet currentCapabilities;
};

class FitIniFile;                      // fwd; keep heavy I/O header out of unitprofile.h
void populateProfileFromFit(FitIniFile& fit, UnitProfile& profile);
#endif // UNITPROFILE_H
