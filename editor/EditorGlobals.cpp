// EditorGlobals.cpp — editor-side definitions for globals that mclib reads
// from the hosting binary via extern.  The game binary (mc2.exe) supplies
// these from code/ (mechcmd2.cpp, gamecam.cpp, terrobj.cpp).  EditRel does
// not link code/, so they are defined here with editor-appropriate defaults.
//
// NS3 note: every variable here is a candidate for migration to an engine
// TU (mclib) with a proper init/configure API.  Track in the NS3 backlog.

// ---- terrain/quad rendering ----
// UV coordinate clamp scale used by mclib/quad.cpp and mclib/terrain.cpp.
float MaxMinUV = 8.0f;

// ---- camera / LOS ----
// When true, mclib/camera.cpp adjusts LOS checks for terrain slope angle.
// False = flat-map LOS, safe default for the editor.
bool useLOSAngle = false;

// ---- clouds ----
// Cloud layer toggle referenced by EditorInterface.cpp.
// Editor starts without clouds; user can toggle via menu.
bool useClouds = false;

// ---- TOBJSPLIT performance counters (code/static_update_counters.h) ----
// Cycle-count accumulators written by mclib/bdactor.cpp when the
// s_tobjSplitBdOn flag is set.  Zero-initialize; editor does not display them.
#include <atomic>
std::atomic<unsigned long long> g_tobjAngularCyc{0ULL};
std::atomic<unsigned long long> g_tobjProjCyc{0ULL};

// ---- game object manager ----
// MechRenderAdapter.cpp reads ObjectManager to reverse-look up mechs.
// Editor starts without a game session loaded; null is a valid sentinel
// (MechRenderAdapter guards: if (ObjectManager == nullptr) return nullptr).
class GameObjectManager;           // fwd-decl; code/objmgr.h defines the class
GameObjectManager* ObjectManager = nullptr;

// ---- RenderWorld self-test stubs ----
// RunGameplayPickSelfTest() is now compiled directly into the editor via
// EDITOR_BRIDGE_SOURCES (gameplay_pick.cpp).  The mech self-test
// (RunMechPickSelfTest) is in GameAdapters/MechRenderAdapter.cpp and
// resolved via the gameadapters library.
