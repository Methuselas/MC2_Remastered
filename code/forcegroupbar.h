#ifndef FORCEGROUPBAR_H
#define FORCEGROUPBAR_H
/*************************************************************************************************\
ForceGroupBar.h			: Interface for the ForceGroupBar component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef MCLIB_H
#include"mclib.h"
#endif

//*************************************************************************************************

class ForceGroupIcon;
class Mover;
class MechWarrior;
class StaticInfo;
class Camera;

#define MAX_ICONS 16

#include"mechicon.h"

#define FORCEGROUP_TOP	ForceGroupIcon::selectionRect[0].top


/**************************************************************************************************
CLASS DESCRIPTION
ForceGroupBar:
**************************************************************************************************/
class ForceGroupBar
{
	public:

		ForceGroupBar();
		~ForceGroupBar();

		bool addMech( Mover* pMover );
		bool addVehicle( Mover* pMover );
		void removeMover (Mover* mover);
		void update();
		void render();

		// Tactical Overview: draw each unit's HUD icon over its world position on
		// the main 3D screen. alpha is the overview cross-fade [0,1].
		void renderOverviewIcons( Camera* eye, float alpha );

		void removeAll();

		bool inRegion( int x, int y );

		void init( FitIniFile& file, StaticInfo* pCoverIcon );
		void swapResolutions();
		int  getIconCount(){ return iconCount; }

		bool setPilotVideo( const char* pVideo, MechWarrior* pPilot );
		bool isPlayingVideo();

		bool flashJumpers (long numFlashes);
		
	private:

		static float iconWidth;
		static float iconHeight;
		static int	 iconsPerRow;

		static StaticInfo* s_coverIcon;

		//Tutorial
		long forceNumFlashes;
		float forceFlashTime;


		int iconCount;
		ForceGroupIcon*	icons[MAX_ICONS];

		friend class ControlGui;

};


//*************************************************************************************************
#endif  // end of file ( ForceGroupBar.h )
