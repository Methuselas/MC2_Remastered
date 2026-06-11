// tools/asset_viewer/CentralManifestMerge.h
// S5: safe reversible install of one override record into a central models.json.
// Preserves all unrelated records; replaces a same-key record. Writes <file>.bak
// before any change; round-trips the result through ModelOverrideRegistry.
// tools/ is exempt from check-json-isolation.sh, so this TU may use nlohmann.
#pragma once
#include <string>
#include "OverrideManifest.h"   // WorkbenchOverride

struct MergeResult {
    bool ok = false;
    std::string message;
    int recordCount = 0;
    bool replacedExisting = false;
};

MergeResult MergeIntoCentralManifest(const std::string& manifestPath,
                                     const WorkbenchOverride& rec);
