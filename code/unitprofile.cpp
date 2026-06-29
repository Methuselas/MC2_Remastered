#include "unitprofile.h"
#include <cstdlib>
bool mc2_unitprofile_parse_enabled(const char* e) {
    return e && e[0] != '\0' && e[0] != '0';
}
bool mc2UnitProfileDataEnabled() {
    static const bool s = mc2_unitprofile_parse_enabled(getenv("MC2_UNIT_PROFILE_DATA"));
    return s;
}
