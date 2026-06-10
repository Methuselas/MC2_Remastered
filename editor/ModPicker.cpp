//-------------------------------------------------------------------------------------------------
// ModPicker.cpp -- see ModPicker.h.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"
#include "ModPicker.h"

#ifdef MC2_IMGUI
#include "imgui.h"
#endif

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// mclib/file.h -- reads MC2_ACTIVE_MOD, clears + re-indexes mods/<mod>/data/* (+ deps).
extern void InitModSearchPaths(const char* modsRoot);

namespace
{
	std::vector<std::string> s_mods;       // discovered mod folder names
	bool  s_scanned  = false;
	int   s_selected = 0;                  // 0 = None (stock); else s_mods[idx-1]
	char  s_active[128] = "";              // active mod folder name ("" = stock)

	void scanMods()
	{
		s_mods.clear();
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA( "mods\\*", &fd );
		if ( h != INVALID_HANDLE_VALUE )
		{
			do
			{
				if ( ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) &&
				     fd.cFileName[0] != '.' )
					s_mods.push_back( fd.cFileName );
			}
			while ( FindNextFileA( h, &fd ) );
			FindClose( h );
		}
		s_scanned = true;
	}

	// Activate a mod ("" = stock). Sets MC2_ACTIVE_MOD then re-indexes via the same
	// path the game/editor startup uses. InitModSearchPaths clears g_modIndex first,
	// so switching mods (or back to stock) is clean.
	void applyMod( const char* modId )
	{
		_putenv_s( "MC2_ACTIVE_MOD", modId ? modId : "" );
		InitModSearchPaths( "./mods/" );
		strncpy( s_active, modId ? modId : "", sizeof( s_active ) - 1 );
		s_active[sizeof( s_active ) - 1] = '\0';
	}
}

namespace ModPicker
{

const char* ActiveMod() { return s_active; }

#ifdef MC2_IMGUI
void Draw()
{
	if ( !s_scanned )
		scanMods();

	ImGui::TextUnformatted( "Mod content" );
	ImGui::SameLine();
	const char* cur = ( s_selected == 0 || s_selected > (int)s_mods.size() )
	                ? "None (stock)"
	                : s_mods[s_selected - 1].c_str();
	ImGui::SetNextItemWidth( 160.0f );
	if ( ImGui::BeginCombo( "##modpicker", cur ) )
	{
		if ( ImGui::Selectable( "None (stock)", s_selected == 0 ) )
		{
			s_selected = 0;
			applyMod( "" );
		}
		for ( int i = 0; i < (int)s_mods.size(); ++i )
		{
			bool sel = ( s_selected == i + 1 );
			if ( ImGui::Selectable( s_mods[i].c_str(), sel ) )
			{
				s_selected = i + 1;
				applyMod( s_mods[i].c_str() );
			}
		}
		ImGui::EndCombo();
	}
	if ( ImGui::IsItemHovered() )
		ImGui::SetTooltip(
			"Mount a mod's data/ over base data/ BEFORE loading a mission.\n"
			"Mods can shadow stock files -- pick None for stock missions.\n"
			"Dependencies (mod.json) are pulled in automatically.\n"
			"Change this, THEN load the mission to apply." );

	ImGui::SameLine();
	if ( ImGui::SmallButton( "Rescan" ) )
		scanMods();
}
#endif

}
