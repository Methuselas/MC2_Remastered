#pragma once

// MISSION-VISUAL-TUNING-1: optional per-mission renderer tuning profiles.
//
// Reads data/visual_tuning.json at mission start and applies global defaults
// then mission-specific overrides to existing renderer globals/tunables.
//
// Precedence: engine defaults < profile defaults < mission overrides < env vars < ImGui.
// Missing file = silent no-op.  Missing mission entry = global defaults only.
// Invalid/unknown key = one-time warning, no crash.
//
// Override profile path with env var MC2_VISUAL_TUNING_FILE.

void visualTuning_applyProfile(const char* missionName);

const char* visualTuning_getProfilePath();
const char* visualTuning_getActiveMission();
bool        visualTuning_hasProfileFile();
int         visualTuning_getAppliedKeyCount();
