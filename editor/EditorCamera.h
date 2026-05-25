#ifndef EDITORCAMERA_H
#define EDITORCAMERA_H
/*************************************************************************************************\
EditorCamera.h			: Interface for the EditorCamera component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/
/***************************************************************
* FILENAME: EditorCamera.h
* DESCRIPTION: EditorCamera renders the editor's terrain, sky, and objects.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/29/2026
* MODIFICATION: by Methuselas
* CHANGES: Hooked the Editor camera into the Remastered terrain shader
*          uniforms (terrainMVP, terrainViewport, camera position, light
*          direction). Mirrors the GameCamera::render() composition block
*          from code/gamecam.cpp so the editor terrain path uses the same
*          shader uniforms the engine expects. Editor-only change; no
*          engine, shader, or mclib files were modified.
****************************************************************/

#ifndef CAMERA_H
#include "Camera.h"
#endif

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#ifndef OBJSTATUS_H
#include "objstatus.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Throwaway diagnostic helper: throttled trace to editor-startup.log,
// gated on MC2_EDITOR_TRACE. Header is included by exactly one TU
// (EditorInterface.cpp) so file-scope static linkage is safe. ASCII only.
static void EditorCameraTrace(const char* fmt, ...)
{
	if (getenv("MC2_EDITOR_TRACE") == NULL)
		return;
	FILE* f = fopen("editor-startup.log", "a");
	if (!f)
		return;
	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fclose(f);
}

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
EditorCamera:  draws the terrain and objects and stuff
**************************************************************************************************/
extern bool useShadows;
extern bool s_bSensorMapEnabled;
extern bool drawOldWay;
extern bool useFog;

extern MidLevelRenderer::MLRClipper * theClipper;

extern bool useLOSAngle;	// NS3: def in mclib/camera.cpp (editor never reads it)

class EditorCamera : public Camera
{
	//Data Members
	//-------------
public:

		AppearancePtr			compass;
		AppearancePtr			theSky;
		bool					drawCompass;
		float					lastShadowLightPitch;
		long					cameraLineChanged;
		long					oldSkyNumber;

		EditorCamera (void)
		{
			init();
		}

		virtual void init (void)
		{
			Camera::init();
			compass = NULL;
			drawCompass = true;
			cameraLineChanged = 0;
			theSky = NULL;
		}

	virtual void reset (void)
	{
		//Must toss these between map loads to clear out their texture memory!!
		delete compass;
		compass = NULL;
		
		delete theSky;
		theSky = NULL;
	}
	
 	virtual void render (void)
	{
		// Diagnostic probe: throttled per-frame trace of EditorCamera::render
		// entry. Active/turn gate inside this function may skip the bulk of
		// rendering when (active && turn > 1) is false; logging entry tells
		// us whether render() is even being driven.
		static long s_ecrFrame = 0;
		++s_ecrFrame;
		const bool s_ecrLog = (s_ecrFrame == 1 || s_ecrFrame == 5 || s_ecrFrame == 30 || s_ecrFrame == 120);
		if (s_ecrLog)
			EditorCameraTrace("EditorCamera::render frame=%ld enter active=%d turn=%ld drawOldWay=%d",
				s_ecrFrame, active ? 1 : 0, (long)turn, drawOldWay ? 1 : 0);

		//------------------------------------------------------
		// At present, these actually draw.  Later they will
		// add elements to the draw list and sort and draw.
		// The later time has arrived.  We begin sorting immediately.
		// NO LONGER NEED TO SORT!
		// ZBuffer time has arrived.  Share and Enjoy!
		// Everything SIMPLY draws at the execution point into the zBuffer
		// at the correct depth.  Miracles occur at that point!
		// Big code change but it removes a WHOLE bunch of code and memory!

		//--------------------------------------------------------
		// Get new viewport values to scale stuff.  No longer uses
		// VFX stuff for this.  ALL GOS NOW!
		gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
		//--------------------------------------------------------
		// Get new viewport values to scale stuff.  No longer uses
		// VFX stuff for this.  ALL GOS NOW!
		screenResolution.x = viewMulX;
		screenResolution.y = viewMulY;
		calculateProjectionConstants();
	
		TG_Shape::SetViewport(viewMulX,viewMulY,viewAddX,viewAddY);
	

		globalScaleFactor = getScaleFactor();
		globalScaleFactor *= viewMulX / Environment.screenWidth;		//Scale Mechs to ScreenRES
		
		//-----------------------------------------------
		// Set Ambient for this pass of rendering	
		DWORD lightRGB = (ambientRed<<16)+(ambientGreen<<8)+ambientBlue;
			
		eye->setLightColor(1,lightRGB);
		eye->setLightIntensity(1,1.0);
	
		MidLevelRenderer::MLRState default_state;
		default_state.SetBackFaceOn();
		default_state.SetDitherOn();
		default_state.SetTextureCorrectionOn();
		default_state.SetZBufferCompareOn();
		default_state.SetZBufferWriteOn();
	
		default_state.SetFilterMode(MidLevelRenderer::MLRState::BiLinearFilterMode);
		
		float z = 1.0f;
		Stuff::RGBAColor fColor;
		fColor.red = (float)((fogColor >> 16) & 0xff);
		fColor.green = (float)((fogColor >> 8) & 0xff);
		fColor.blue = (float)((fogColor) & 0xff);
	
		MidLevelRenderer::PerspectiveMode = usePerspective;
		theClipper->StartDraw(cameraOrigin, cameraToClip, fColor, &fColor, default_state, &z);
		MidLevelRenderer::GOSVertex::farClipReciprocal = (1.0f-cameraToClip(2, 2))/cameraToClip(3, 2);

		if (s_ecrLog)
			EditorCameraTrace("EditorCamera::render frame=%ld gate(active && turn>1)=%d",
				s_ecrFrame, (active && turn > 1) ? 1 : 0);
		if (active && turn > 1)
		{
			// REBUILD 2026-05-25: hand-forked per-frame chain deleted.
			// Manual mcTextureManager->renderLists() orchestration and the
			// hand-forked terrain MVP swap (gos_SetTerrainViewport,
			// gos_SetTerrainCameraPos, gos_SetTerrainLightDir) are gone.
			// land->render(), EditorObjectMgr->render/renderShadows(),
			// land->renderWater(), and the renderLists() calls were a
			// partial gamecam.cpp mirror; the canonical chain will be
			// re-wired in S2 (mirrors gamecam.cpp:176-257 exactly).
			// See docs/superpowers/plans/2026-05-25-editor-rebuild.md §S2.
			//
			// KEPT: gos_SetWorldToClipGL (S0 §5 step 1; canonical first
			// per-frame call), sky/compass/EditorInterface ImGui glue.
			gos_SetWorldToClipGL(eye->worldToClipGL());

			if (theSky)
				theSky->render(1);

			if (!drawOldWay)
			{
				if (compass && (turn > 3) && drawCompass)
					compass->render(-1);		//Force this to zBuffer in front of everything
			}

			/* The editor interface needs to be drawn last, as it draws things "on top" of the
			rendered scene. */
			if ( EditorInterface::instance() )
			{
				EditorInterface::instance()->render();
			}
		}
	
	 	//-----------------------------------------------------
		if (drawOldWay)
		{
			gos_SetRenderState( gos_State_ZCompare, 0);
			gos_SetRenderState(	gos_State_ZWrite, 0);
			gos_SetRenderState( gos_State_Perspective, 1);
	
			if (compass && (turn > 3) && drawCompass)
				compass->render();
		}
 	}

	virtual long activate (void)
	{
		// If camera is already active, just return
		if (ready && active)
			return(NO_ERR);
		
		//Can set initial position and stuff here.
		//updateDaylight(true);
		
		lastShadowLightPitch = lightPitch;

		allNormal();

		return NO_ERR;
	}

	virtual long update (void)
	{
		// calculate new near and far plane distance based on 
		// Current altitude above terrain.
		float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

		float altitudePercent = (cameraAltitude - AltitudeMinimum) / (testMax - AltitudeMinimum);
		Camera::NearPlaneDistance = MinNearPlane + ((MaxNearPlane - MinNearPlane) * altitudePercent);
		Camera::FarPlaneDistance = MinFarPlane + ((MaxFarPlane - MinFarPlane) * altitudePercent);
	
		if (!compass && (turn > 3))	//Create it!
		{
			//Gotta check for the list too because a NEW map has no objects on it and this list
			// Doesn't exist until objects are placed.  Strange but true...
			if ( !appearanceTypeList )
			{
				appearanceTypeList = new AppearanceTypeList;
				gosASSERT(appearanceTypeList != NULL);
				
				appearanceTypeList->init(2048000);
			}

			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "compass" );
			compass = new BldgAppearance;
			compass->init( appearanceType );
		}
		
		if (!theSky && (turn > 3))
		{
			//Startup the SKYBox
			long appearanceType = (GENERIC_APPR_TYPE << 24);
		
			AppearanceTypePtr genericAppearanceType = NULL;
			genericAppearanceType = appearanceTypeList->getAppearance(appearanceType,"skybox");
			if (!genericAppearanceType)
			{
				char msg[1024];
				sprintf(msg,"No Generic Appearance Named %s","skybox");
				Fatal(0,msg);
			}
			  
			theSky = new GenericAppearance;
			gosASSERT(theSky != NULL);
		
			//--------------------------------------------------------------
			gosASSERT(genericAppearanceType->getAppearanceClass() == GENERIC_APPR_TYPE);
			theSky->init((GenericAppearanceType*)genericAppearanceType, NULL);
			
			theSky->setSkyNumber(EditorData::instance->TheSkyNumber());
			oldSkyNumber = EditorData::instance->TheSkyNumber();
		}

		//Did they change the skyNumber on us?
		if (theSky && (oldSkyNumber != EditorData::instance->TheSkyNumber()))
		{
			theSky->setSkyNumber(EditorData::instance->TheSkyNumber());
			oldSkyNumber = EditorData::instance->TheSkyNumber();
		}
		
		long result = Camera::update();

		if ((cameraLineChanged + 10) < turn)
		{
			if (userInput->getKeyDown(KEY_BACKSLASH) && !userInput->ctrl() && !userInput->alt() && !userInput->shift())
			{
				drawCompass ^= true;
				cameraLineChanged = turn;
			}
		}

		#define MAX_SHADOW_PITCH_CHANGE	(5.0f)
		//Always recalc here or shadows never change in editor!!
		forceShadowRecalc = true;

		if (compass && (turn > 3))
   		{
   	   		bool oldFog = useFog;
   			bool oldShadows = useShadows;
   	   		useFog = false;
   			useShadows = false;
   	   		
   	   		Stuff::Vector3D pos = getPosition();
   	   		compass->setObjectParameters(pos,0.0f,false,0,0);
   	   		compass->setMoverParameters(0.0f);
   	   		compass->setGesture(0);
   	   		compass->setObjStatus(OBJECT_STATUS_DESTROYED);
   	   		compass->setInView(true);
   	   		compass->setVisibility(true,true);
   	   		compass->setFilterState(true);
   			compass->setIsHudElement();
   	   		compass->update();		   //Force it to try and draw or stuff will not work!
			
			if (theSky)
			{
				Stuff::Vector3D pos = getPosition();
				
				theSky->setObjectParameters(pos,0.0f,false,0,0);
				theSky->setMoverParameters(0.0f);
				theSky->setGesture(0);
				theSky->setObjStatus(OBJECT_STATUS_NORMAL);
				theSky->setInView(true);
				theSky->setVisibility(true,true);
				theSky->setFilterState(true);
				theSky->setIsHudElement();
				theSky->update();		   //Force it to try and draw or stuff will not work!
			}
					
   	   		useFog = oldFog;
   			useShadows = oldShadows;
   		}

		return result;
	}


};

//*************************************************************************************************
#endif  // end of file ( EditorCamera.h )
