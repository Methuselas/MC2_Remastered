// T1.16 — (E)-owned spotlight slot tagging registry implementation.
//
// See spotlight_diag.h. File-scope unordered_map<long, uint8_t> mapping
// slotId -> SourceClass. Cheap O(1) lookups in Camera::updateLights; only
// queried when MC2_SPOT_DIAG is set (callers check is_enabled() first to
// avoid the lookup entirely in the default-off case).

#include "spotlight_diag.h"

#include <cstdlib>
#include <unordered_map>

namespace mc2_spotlight_diag {

namespace {

std::unordered_map<long, uint8_t>& slot_map()
{
    static std::unordered_map<long, uint8_t> s_map;
    return s_map;
}

bool s_enabled_cached = (std::getenv("MC2_SPOT_DIAG") != nullptr);

} // namespace

void tag_slot(long slot, SourceClass src)
{
    slot_map()[slot] = static_cast<uint8_t>(src);
}

void untag_slot(long slot)
{
    slot_map().erase(slot);
}

bool is_e_slot(long slot, SourceClass* outSrc)
{
    auto& m = slot_map();
    auto it = m.find(slot);
    if (it == m.end()) return false;
    if (outSrc) *outSrc = static_cast<SourceClass>(it->second);
    return true;
}

void reset()
{
    slot_map().clear();
}

bool is_enabled()
{
    return s_enabled_cached;
}

} // namespace mc2_spotlight_diag
