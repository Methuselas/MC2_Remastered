// GameOS/gameos/ibl_sh_runtime.h
//
// V-IBL-STATIC-1 runtime cross-TU bridge.
//
// Defines (in gos_static_prop_batcher.cpp):
//   float g_iblShStrength;     // ImGui slider value, default 1.0f.
//
// The env var MC2_STATIC_PROP_IBL_SH (read once at process start as
// s_iblShEnabled inside the batcher TU) is the AUTHORITATIVE GATE -- the
// slider only modulates strength when the env-gate is on. Per-frame upload
// computes `s_iblShEnabled ? g_iblShStrength : 0.0f` so env-unset always
// uploads 0.0 (byte-identical OFF, regardless of slider value).
//
// Consumers: GuiRuntime/EditorInspector.cpp (ImGui slider).
//
// Firewall: no GL, no game-side includes. Header-only extern decl.

#pragma once

extern float g_iblShStrength;
