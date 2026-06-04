#pragma once
#include "OverrideManifest.h"
#include <string>
struct ExportResult { bool ok=false; std::string message, bundleDir, manifestPath; };
// Writes <outRoot>/<id>/{<glb>, models.generated.json}; rewrites rec.sourceRelPath
// to "<id>/<glb>"; refuses if a BLOCK rule trips OR the registry drops the record.
ExportResult ExportBundle(const std::string& outRoot, const std::string& bundleId,
                          const std::string& srcGlbPath, WorkbenchOverride rec);
