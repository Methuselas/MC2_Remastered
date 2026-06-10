//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

// newsinglemission.cpp : implementation file
//

#include "stdafx.h"
#include "resource.h"
#include "newsinglemission.h"
#include "ModPicker.h"
#include "EditorRecent.h"
#include <string>

// When the user picks a Recent mission and clicks Edit Mission, this holds the path
// so EditorInterface loads it directly instead of popping the file dialog. Empty =
// browse as before. Read+cleared by the ID_LOAD_MISSION branch in EditorInterface.cpp.
std::string g_editorPreselectMission;

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// NewSingleMission dialog


NewSingleMission::NewSingleMission(CWnd* pParent /*=NULL*/)
	: CDialog(NewSingleMission::IDD, pParent)
{
	//{{AFX_DATA_INIT(NewSingleMission)
		// NOTE: the ClassWizard will add member initialization here
	//}}AFX_DATA_INIT
}


void NewSingleMission::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(NewSingleMission)
		// NOTE: the ClassWizard will add DDX and DDV calls here
	//}}AFX_DATA_MAP
}


BEGIN_MESSAGE_MAP(NewSingleMission, CDialog)
	//{{AFX_MSG_MAP(NewSingleMission)
	ON_BN_CLICKED(ID_LOAD_MISSION, OnLoadMission)
	ON_BN_CLICKED(ID_NEWMISSION, OnNewmission)
	ON_BN_CLICKED(ID_MAPGENERATOR, OnMapGenerator)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// NewSingleMission message handlers

BOOL NewSingleMission::OnInitDialog()
{
	CDialog::OnInitDialog();

	// Populate the Mod combo: "None (stock)" + every folder under mods/. Pick the
	// mod BEFORE loading a mission so its data/* shadows base data/* (mod missions'
	// appearance assets resolve); default None keeps stock editing clean.
	CComboBox* combo = (CComboBox*)GetDlgItem( IDC_MI_MOD_COMBO );
	if ( combo )
	{
		ModPicker::ScanMods();
		combo->ResetContent();
		int noneIdx = combo->AddString( "None (stock)" );
		combo->SetItemData( noneIdx, (DWORD_PTR)-1 );
		int sel = noneIdx;
		const char* active = ModPicker::ActiveMod();
		for ( int i = 0; i < ModPicker::ModCount(); ++i )
		{
			int idx = combo->AddString( ModPicker::ModName( i ) );
			combo->SetItemData( idx, (DWORD_PTR)i );
			if ( active && active[0] && strcmp( active, ModPicker::ModName( i ) ) == 0 )
				sel = idx;
		}
		combo->SetCurSel( sel );
	}

	// Recent missions: index 0 = "(browse...)", then most-recent first.
	CComboBox* recent = (CComboBox*)GetDlgItem( IDC_MI_RECENT_COMBO );
	if ( recent )
	{
		recent->ResetContent();
		recent->AddString( "(browse for file...)" );
		for ( int i = 0; i < EditorRecent::Count(); ++i )
		{
			const char* p = EditorRecent::Path( i );
			// Show the file name (after the last slash) to keep the combo readable.
			const char* base = p;
			for ( const char* c = p; *c; ++c )
				if ( *c == '\\' || *c == '/' ) base = c + 1;
			recent->AddString( base );
		}
		recent->SetCurSel( 0 );
	}
	return TRUE;
}

// Read the combo and activate that mod (or stock) before the dialog's load path runs.
void NewSingleMission::applySelectedMod()
{
	CComboBox* combo = (CComboBox*)GetDlgItem( IDC_MI_MOD_COMBO );
	if ( !combo ) return;
	int cur = combo->GetCurSel();
	if ( cur < 0 ) return;
	int modIdx = (int)(INT_PTR)combo->GetItemData( cur );
	if ( modIdx < 0 ) ModPicker::Activate( "" );                       // None (stock)
	else              ModPicker::Activate( ModPicker::ModName( modIdx ) );
}

void NewSingleMission::OnLoadMission()
{
	applySelectedMod();

	// If a Recent (index > 0) is chosen, hand its full path to EditorInterface so it
	// loads directly; otherwise leave empty -> the file-browse dialog runs as before.
	g_editorPreselectMission.clear();
	CComboBox* recent = (CComboBox*)GetDlgItem( IDC_MI_RECENT_COMBO );
	if ( recent )
	{
		int sel = recent->GetCurSel();
		if ( sel > 0 )   // 0 == "(browse...)"
		{
			const char* p = EditorRecent::Path( sel - 1 );
			if ( p && p[0] ) g_editorPreselectMission = p;
		}
	}

	EndDialog( ID_LOAD_MISSION );
}

void NewSingleMission::OnNewmission()
{
	applySelectedMod();
	EndDialog( ID_NEWMISSION );

}

void NewSingleMission::OnMapGenerator()
{
	applySelectedMod();
	EndDialog( ID_MAPGENERATOR );
}

void NewSingleMission::OnCancel() 
{
	// TODO: Add extra cleanup here
	
	CDialog::OnCancel();
}
