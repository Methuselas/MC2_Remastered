// RenderCore/SceneLighting.h
//
// SCENE-LIGHTING-STATE-1: a canonical, read-only MIRROR of today's fragmented
// lighting state. It does NOT change any rendering — it is a single place to
// observe (and parity-check) the scattered sun/ambient/terrain/shadow/IBL
// sources that currently feed different render paths independently.
//
// This is NOT the final lighting model. It is a diagnostic snapshot captured
// per frame-window from the existing Camera (eye) / gos terrain light dir /
// post-process / static-prop-batcher SH sources. See LINEAR-COLOR-AUDIT-1.md
// and LIGHTING-SYSTEM-RECON-1 for the surrounding arc.
//
// Pure POD (float arrays only) — no GL/Stuff/engine type dependencies so it can
// be included from both the GameOS and game-logic layers.
#pragma once

struct SceneLighting {
    // Directional sun as the OBJECT path sees it (Camera worldLights[0] /
    // eye->lightDirection). MC2 frame.
    float sun_dir[3]       = {0.0f, 0.0f, 1.0f};
    float sun_color[3]     = {1.0f, 1.0f, 1.0f};   // unpacked from getLightColor(0) aRGB
    float sun_intensity    = 1.0f;

    // Ambient (Camera worldLights[1] / getLightColor(1)).
    float ambient_color[3] = {0.0f, 0.0f, 0.0f};

    // Sun as the TERRAIN path sees it (gos_GetTerrainLightDir). May DIVERGE from
    // sun_dir — the object path historically used a corrected terrain sun because
    // worldLights[0] was ~90deg off (see static_prop.vert STATIC-PROP-TERRAIN-SUN-DIFFUSE).
    float terrain_light_dir_current[3] = {0.0f, 0.0f, 1.0f};

    // Sun direction used to build the shadow maps. DERIVED: gameos_graphics.cpp
    // passes -terrainLightDir to buildStaticLightMatrix (gameos_graphics.cpp:8834),
    // so shadow_sun_dir == -terrain_light_dir_current by construction today.
    float shadow_sun_dir[3] = {0.0f, 0.0f, -1.0f};

    int   world_light_count  = 0;   // Camera::numLights (slot capacity, ~MAX)
    int   active_light_count = 0;   // Camera::getNumActiveLights() (frustum-filtered)

    int   sky_number   = -1;        // gos_GetSkyNumber() (post-process current sky), -1 = none
    bool  ibl_sh_present = false;    // g_iblShStrength > 0 (static-prop SH ambient active)
};

// Capture + emit one trace line per frame-window when MC2_SCENE_LIGHTING_TRACE=1.
// When MC2_SCENE_LIGHTING_ASSERT=1, additionally emit a WARN line (never aborts)
// when the terrain sun and camera sun diverge beyond a small angle — that
// divergence is EXPECTED today and is the headline fragmentation this slice
// surfaces. Safe to call every frame; it self-throttles. No-op when neither
// gate is set. Implemented in code/gamecam.cpp (where the global camera lives).
void mc2SceneLightingTrace();
