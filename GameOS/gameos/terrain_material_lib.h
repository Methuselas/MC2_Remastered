#pragma once

// TERRAIN-MATERIAL-LIB-1: optional data-defined terrain material tuning.
//
// Generalizes the visual_tuning_profile.cpp TinyJson reader pattern into a
// dedicated terrain_materials.json schema covering ALL per-layer chunk-path
// tunables (tiling, tint, normalBoost, class thresholds, roughness, AO,
// blend params) -- not just the ~6 keys the visual tuning profile touches.
//
// Precedence (per USER RULING): env var wins over JSON for any overlapping
// knob (e.g. snowBrightnessDampen already has MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN).
// JSON is applied AFTER visual_tuning.json at mission load, so it wins for
// terrain material keys if both set the same key.
//
// Gate: MC2_TERRAIN_MATERIAL_LIB, default OFF. OFF -> terrainMaterials_apply()
// returns immediately (no file read, no member writes) -> gosRenderer members
// keep their C++ initializer values verbatim (byte-identical to pre-slice).
//
// File path override: MC2_TERRAIN_MATERIAL_LIB_FILE (default data/terrain_materials.json).
// Missing file = silent no-op (same convention as visual_tuning.json).
// This is a SEPARATE file from data/visual_tuning.json (kept separate per
// user ruling: material lib = per-layer authoring, visual_tuning = global
// post/lighting). Do not merge them.

void terrainMaterials_apply(const char* missionName);
