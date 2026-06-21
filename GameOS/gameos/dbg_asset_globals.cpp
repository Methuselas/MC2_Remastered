// dbg_asset_globals.cpp — definitions for the per-mission debug asset globals.
//
// LINK-CONFIG-FIX: debug_state_dump.cpp (compiled into gameos AND gameos_editor)
// reads g_dbgSkyNumber / g_dbgLoadScreen via `extern`. Their definitions used to
// live in code/gamecam.cpp, which is compiled ONLY into mc2.exe's source list.
// Any target that links a gameos-family lib but builds its own main (EditRel via
// gameos_editor, the data_tools via gameos) therefore failed to link with these
// two symbols unresolved.
//
// Hosting the definitions here — a gameos TU shared by gameos and gameos_editor —
// resolves all of them at once. Game code that needs to write these (gamecam.cpp,
// logmain.cpp, loadscreen.cpp) keeps doing so via its existing `extern`
// declarations; only the single definition site moved out of gamecam.cpp.
//
// Set at mission-load / loadscreen-pick by the game layer.

long g_dbgSkyNumber      = -1;
char g_dbgLoadScreen[64]  = "";
