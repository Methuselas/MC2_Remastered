#define MISSIONRESULTS_CPP
/*************************************************************************************************\
MissionResults.cpp			: Implementation of the MissionResults component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"missionresults.h"
#include"mclib.h"
#include"objmgr.h"
#include"mech.h"
#include"logisticsvariant.h"
#include"mechicon.h"
#include"logisticsdata.h"
#include"salvagemecharea.h"
#include"pilotreviewarea.h"
#include"gamesound.h"
#include "../resource.h"
#include"../GuiRuntime/GuiRuntime.h"   // GetDisplaySize (MECH-ICON-BLANK-1 gui bridge)
#include <cstdlib>   // std::getenv (MC2_SOAK_AUTOWIN results auto-dismiss)

bool MissionResults::FirstTimeResults = true;
MissionResults::MissionResults()
{
	pSalvageScreen = 0;
	pPilotScreen = 0;
	bDone = 0;
	bPilotStarted = 0;
}

MissionResults::~MissionResults()
{
	if (pSalvageScreen)
		delete pSalvageScreen;
	pSalvageScreen = NULL;

	if (pPilotScreen)
		delete pPilotScreen;
	pPilotScreen = NULL;
}

void MissionResults::begin()
{

	if ( soundSystem )
		soundSystem->stopSupportSample();


	if ( !MPlayer )
	{
		char path[256];
		FitIniFile file;

		if ( !LogisticsData::instance->skipSalvageScreen() )
		{
			pSalvageScreen = new SalvageMechScreen();
			strcpy( path, artPath );
			strcat( path, "mcui_mr_layout.fit" );
			
			if ( NO_ERR != file.open( path ) )
			{
				char error[256];
				sprintf( error, "couldn't open file %s", path );
				Assert( 0, 0, error );
				return;
			}
			pSalvageScreen->init( &file );
			file.close();	
			bDone = false;
		}
			
		if ( !LogisticsData::instance->skipPilotReview() )
		{
		
			strcpy( path, artPath );
			strcat( path, "mcui_mr_layout.fit" );

			pPilotScreen = new PilotReviewScreen();
			if ( NO_ERR != file.open( path ) )
			{
				char error[256];
				sprintf( error, "couldn't open file %s", path );
				Assert( 0, 0, error );
				return;
			}
			pPilotScreen->init( &file );
			bDone = 0;
			if ( !LogisticsData::instance->skipSalvageScreen() )
				bPilotStarted = 0;
			else
				bPilotStarted = 1;
		}
	}
	else
	{
		if ( !mpStats.staticCount )
		{
			mpStats.init();

		}

		if ( mpStats.getStatus() != LogisticsScreen::RUNNING )
			mpStats.begin();

		bDone = false;
	}
}

void MissionResults::init()
{

}

void MissionResults::end()
{
	//Need to save the game here so salvage and pilot promotion get saved as well!
	// Pity we never call ::end!
	const char* pMissionName = LogisticsData::instance->getLastMission();

	if ( pMissionName && !LogisticsData::instance->isSingleMission() && !LogisticsData::instance->campaignOver() && !MPlayer )
	{
		FitIniFile file;
		char name[256];
		cLoadString( IDS_AUTOSAVE_NAME, name, 255 );
		char fullName[256];
		sprintf( fullName, name, pMissionName );
		FullPathFileName path;
		path.init( savePath, fullName, ".fit" );
		if ( NO_ERR == file.create( path ) )
			LogisticsData::instance->save( file );
	}
}

void MissionResults::update()
{
	userInput->setMouseCursor( mState_NORMAL );
	userInput->mouseOn();

	// CRASH-SOAK harness (MC2_SOAK_AUTOWIN): the post-win results flow
	// (salvage mech screen, then pilot promotion/review screen) needs a
	// "Done" click to dismiss each panel. Under the soak gate, synthesize
	// that click programmatically so the campaign walks itself unattended.
	// We mirror exactly what each screen's Done button does:
	//   - SalvageMechScreen::handleMessage  -> who == 101
	//   - PilotReviewScreen::handleMessage  -> who == aMSG_DONE (110), but
	//     it self-gates on entryAnim.isDone(), so a repeated nudge is safe.
	// A short settle dwell per panel lets entry anims finish (matches the
	// human-pace dismissal) before we fire. Single-player only; default OFF
	// (env unset) is byte-identical. Cached getenv at first call.
	static const bool s_soakAutoWin =
		( std::getenv("MC2_SOAK_AUTOWIN") != nullptr );
	if ( s_soakAutoWin && !MPlayer )
	{
		static float s_soakResultsDwell = 0.0f;
		s_soakResultsDwell += frameLength;
		if ( s_soakResultsDwell >= 0.75f )
		{
			s_soakResultsDwell = 0.0f;
			if ( pSalvageScreen && !pSalvageScreen->donePressed() )
			{
				printf("[SOAK] results dismiss screen=salvage\n");
				fflush(stdout);
				pSalvageScreen->handleMessage( 101, 101 );
			}
			else if ( pPilotScreen )
			{
				printf("[SOAK] results dismiss screen=pilot\n");
				fflush(stdout);
				bPilotStarted = true; // salvage normally sets this on its Done
				pPilotScreen->handleMessage( aMSG_DONE, aMSG_DONE );
			}
		}
	}

	if ( MPlayer )
	{
		mpStats.update();
		if ( mpStats.getStatus() != LogisticsScreen::RUNNING )
			bDone = true;
	}

	else if ( !pSalvageScreen && !pPilotScreen )
		bDone = true;

	if ( pSalvageScreen )
	{
		pSalvageScreen->update();
		if ( pSalvageScreen->donePressed() )
		{
			bPilotStarted = true;		
		}

		if ( pSalvageScreen->isDone() )
		{
			pSalvageScreen->updateSalvage();
			delete pSalvageScreen;
			pSalvageScreen = NULL;

			if ( !pPilotScreen )
			{
				bDone = true;
			}
		}

	}
	if ( pPilotScreen && bPilotStarted)
	{
		pPilotScreen->update();
		if ( pPilotScreen->isDone() )
		{
			pPilotScreen->updatePilots();
			delete pPilotScreen;
			pPilotScreen = 0;
			// take this out!
		 
			bDone = true;



			//Also should stop the support sample here. 
			soundSystem->stopSupportSample();
		}
	}

//	if (!soundSystem->isDigitalMusicPlaying())
	{
		//We're done with the win or lose tune.
		// Start the mission results tune as a loop.
		soundSystem->playDigitalMusic(RESULTS_LOOP);
	}
}

void MissionResults::render()
{
	// MECH-ICON-BLANK-1: full gui bridge for the whole after-action stack
	// (salvage list, pilot review, MP stats) — same fix as the Mech Bay
	// deployment icons: with the defs replacement page active, legacy
	// gos_DrawQuads are buried under the ImGui page. The gui bridge also
	// routes text (aText falls back to the gui-bridge scales), so widget
	// quads AND labels land on the ImGui HUD layer at display scale.
	float tbDw = 0.f, tbDh = 0.f, tbSx = 1.f, tbSy = 1.f;
	if ( GuiRuntime::GetDisplaySize( tbDw, tbDh ) &&
		 Environment.screenWidth > 0 && Environment.screenHeight > 0 )
	{
		tbSx = tbDw / (float)Environment.screenWidth;
		tbSy = tbDh / (float)Environment.screenHeight;
	}
	aObject::beginGuiBridge( tbSx, tbSy );

	if ( MPlayer )
	{
		mpStats.render(0, 0);
	}

	else if ( pSalvageScreen )
	{
		pSalvageScreen->render();

		//Tutorial -- ONLY do first time we get into the salvage screen.
		if (FirstTimeResults)
		{
			soundSystem->playSupportSample(-1, "tut_4a");
			FirstTimeResults = false;
		}
	}

	if ( pPilotScreen && bPilotStarted )
		pPilotScreen->render();

	aObject::endGuiBridge();
}

void MissionResults::setHostLeftDlg( const char* pName )
{
	if ( MPlayer && mpStats.getStatus() == LogisticsScreen::RUNNING )
	{
		mpStats.setHostLeftDlg( pName );
	}
}

