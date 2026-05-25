//---------------------------------------------------------------------------
//
// GameCam.h -- File contains the Game camera class definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef GAMECAM_H
#include"gamecam.h"
#include"tex_resolve_table.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef WEATHER_H
#include"weather.h"
#endif

#include<mlr/mlr.hpp>
#include <tracy/Tracy.hpp>
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (RAII scope)
#include "../GameOS/gameos/gos_static_prop_registry.h"  // Stage 3.C: frameBegin()
#include "particles/batcher.h"  // GPU particle batcher flush (Stage 2' and beyond)
#include "../GameOS/gameos/gos_particle_bridge.h"  // B2 P1: camera basis bridge
#include "../GameOS/gameos/debug_renderer.h"
#include "../GuiRuntime/EditorInspector.h"  // IMG-INSPECT-3 flushDebugHighlight

//---------------------------------------------------------------------------
CameraPtr eye = NULL;

extern bool useShadows;
extern bool useFog;
extern bool DisplayCameraAngle;

extern MidLevelRenderer::MLRClipper * theClipper;

#define MAX_SHADOW_PITCH_CHANGE	(5.0f)

extern bool drawOldWay;

extern bool useNonWeaponEffects;
GenericAppearance *theSky = NULL;
//---------------------------------------------------------------------------
void GameCamera::destroy (void)
{
	if (theSky)
	{
		delete theSky;
		theSky = NULL;
	}

	if (compass)
	{
		delete compass;
		compass = NULL;
	}

	Camera::destroy();
}

//---------------------------------------------------------------------------
void GameCamera::render (void)
{
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

	MidLevelRenderer::MLRState default_state;
	default_state.SetBackFaceOn();
	default_state.SetDitherOn();
	default_state.SetTextureCorrectionOn();
	default_state.SetZBufferCompareOn();
	default_state.SetZBufferWriteOn();

	default_state.SetFilterMode(MidLevelRenderer::MLRState::BiLinearFilterMode);

	float z = 1.0f;
	Stuff::RGBAColor fColor;
	fColor.red = ((fogColor >> 16) & 0xff);
	fColor.green = ((fogColor >> 8) & 0xff);
	fColor.blue = ((fogColor) & 0xff);

	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	screenResolution.x = viewMulX;
	screenResolution.y = viewMulY;
	calculateProjectionConstants();

	TG_Shape::SetViewport(viewMulX,viewMulY,viewAddX,viewAddY);

	userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

	gos_TextSetRegion(viewAddX,viewAddY,viewMulX,viewMulY);
	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	screenResolution.x = viewMulX;
	screenResolution.y = viewMulY;
	calculateProjectionConstants();

	globalScaleFactor = getScaleFactor();
	globalScaleFactor *= viewMulX / 640.0;		//Scale Mechs to ScreenRES
	
	//-----------------------------------------------
	// Set Ambient for this pass of rendering	
	DWORD lightRGB = (ambientRed<<16)+(ambientGreen<<8)+ambientBlue;
		
	eye->setLightColor(1,lightRGB);
	eye->setLightIntensity(1,1.0);

	MidLevelRenderer::PerspectiveMode = usePerspective;
	{
		// F3 CPU projection cost-baseline: mlr_total bucket — wraps StartDraw
		// (per-frame MLR setup). RenderNow gets a second scope further below.
		::mc2_cpu_proj_cost::Scope _f3_mlr_start_scope(
		    ::mc2_cpu_proj_cost::BUCKET_MLR_TOTAL);
		theClipper->StartDraw(cameraOrigin, cameraToClip, fColor, &fColor, default_state, &z);
	}
	MidLevelRenderer::GOSVertex::farClipReciprocal = (1.0f-cameraToClip(2, 2))/cameraToClip(3, 2);

	if (active && turn > 1)
	{
		ZoneScopedN("GameCamera::render activeScene");
		//----------------------------------------------------------
		// Turn stuff on line by line until perspective is working.

		// Compose terrainMVP: MC2 world coords -> GL clip coords
		{
			ZoneScopedN("Camera.BuildMVP");
			// F3 CPU projection cost-baseline: matrix_build site (a).
			::mc2_cpu_proj_cost::Scope _f3_mvp_scope(
			    ::mc2_cpu_proj_cost::BUCKET_MATRIX_BUILD);
			// F1 Stage A unified-projection: single producer call.
			// gos_SetWorldToClipGL composes nothing here — the matrix is
			// built once by Camera::worldToClipGL() (axisSwap *
			// worldToCamera * cameraToClip, with R-clipw polarity folded
			// into kAxisSwapMC2toGL). gos_SetWorldToClipGL handles the
			// column-major -> row-major repackage internally AND writes
			// the terrain_mvp_ cache so all gos_GetTerrainMVPMat4()
			// callers (CullUBO, mech-batcher, static-prop-batcher,
			// particle-bridge, etc.) inherit transparently.
			gos_SetWorldToClipGL(eye->worldToClipGL());

			// Camera position in MC2 world space for TCS distance LOD
			Stuff::Vector3D camOrig = getCameraOrigin();
			gos_SetTerrainCameraPos(camOrig.x, camOrig.y, camOrig.z);

			// Light direction in raw MC2 world space (x, y, elevation)
			// NOT swizzled — fragment shader normals are in tangent space where Z = up,
			// which matches raw MC2 coords (Z = elevation).
			gos_SetTerrainLightDir(lightDirection.x, lightDirection.y, lightDirection.z);

			#undef WTC
		}

		if (Environment.Renderer != 3)
		{
			ZoneScopedN("GameCamera::render sky");
			theSky->render(1);
		}

		{
			ZoneScopedN("GameCamera::render terrain");
			GpuStaticPropRegistry::frameBegin();  // Stage 3.C: reset live-instance list
			land->render();								//render the Terrain
		}

		if (Environment.Renderer != 3)
		{
			ZoneScopedN("GameCamera::render craters");
			craterManager->render();					//render the craters and footprints
		}

		{
			ZoneScopedN("GameCamera::render objects");
			// F3 CPU projection cost-baseline: mark we are inside the render
			// loop so eventdriven projectZ attribution can distinguish
			// render-time calls from AI/picking/input calls.
			::mc2_cpu_proj_cost::RenderLoopGuard _f3_render_guard;
			ObjectManager->render(true, true, true);	//render all other objects
		}

		{
			ZoneScopedN("GameCamera::render water");
			land->renderWater();						//Draw Water Last!
		}

		if (useShadows && Environment.Renderer != 3)
		{
			ZoneScopedN("GameCamera::render shadows");
			ObjectManager->renderShadows(true, true, true);
		}

		if (mission && mission->missionInterface)
		{
			ZoneScopedN("GameCamera::render drawVTOL");
			mission->missionInterface->drawVTOL();
		}

		if (!drawOldWay && !inMovieMode)
		{
			if (compass && (turn > 3) && drawCompass)
			{
				ZoneScopedN("GameCamera::render compass");
				compass->render(-1);		//Force this to zBuffer in front of everything
			}
		}

		if (!drawOldWay)
		{
			ZoneScopedN("GameCamera::render textureManagerRenderLists");
			mcTextureManager->renderLists();			//This sends triangles down to the card.  All "rendering" to this point has been setting up tri lists
			endFrameTexResolve();              // close the per-frame window — clears frameActive,
			                                   // accumulates resolved-count, emits 600-frame summary
			                                   // when due. No-op when killswitch OFF or already inactive.

			// Stage 2 of renderWater architectural slice (CPU→GPU offload).
			// MUST run after renderLists so terrain has flushed before we
			// alpha-blend water on top. No-op when MC2_RENDER_WATER_FASTPATH
			// is unset; legacy water already drained inside renderLists().
			if (land) {
				ZoneScopedN("GameCamera::render waterFastPath");
				land->renderWaterFastPath();
			}

			// GPU particle batcher flush — Stage 2' and beyond.
			// MUST run after renderLists() so the scene depth buffer is
			// populated before alpha-blended billboards composite on top
			// (memory/gpu_direct_renderer_bringup_checklist.md trap #6).
			// No-op when MC2_GPU_PARTICLES is unset (default OFF).
			// Stage 1' canary (hardcoded orange billboard) removed now
			// that real gosFX producers call BeginGroup+Emit via the
			// SpawnCard*/SpawnCardCloud paths.
			{
				ZoneScopedN("GameCamera::render particlesFlush");

				// B2 P1: publish current camera basis to particle bridge before flush.
				// cameraOrigin is the camera's world transform (LinearMatrix4D);
				// GetLocalRightInWorld / GetLocalUpInWorld return vectors in MC2/Stuff
				// world space (x=east, y=north, z=elevation).  Apply the same axis swap
				// used in particle_billboard.vert (GL_x=-Stuff_x, GL_y=Stuff_z, GL_z=Stuff_y)
				// so the bridge vectors land in the same space as worldPos in the shader.
				{
					Stuff::UnitVector3D stuffRight, stuffUp;
					cameraOrigin.GetLocalRightInWorld(&stuffRight);
					cameraOrigin.GetLocalUpInWorld(&stuffUp);
					float camRight[3] = { -stuffRight.x,  stuffRight.z,  stuffRight.y };
					float camUp[3]    = { -stuffUp.x,     stuffUp.z,     stuffUp.y    };
					gos_SetActiveCamera(camRight, camUp);
				}

				::mc2::particles::Batcher::Instance().ResolveTextures();  // resolve MLR->GOS after renderLists
				::mc2::particles::Batcher::Instance().Flush();

				gos_ClearActiveCamera();
			}
		}

		if (drawOldWay)
		{
			//Last thing drawn were shadows which are not Gouraud Shaded!!!
			// MLR to be "efficient" doesn't set this state by default at startup!
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		}

		{
			ZoneScopedN("GameCamera::render clipperRenderNow");
			// F3 CPU projection cost-baseline: mlr_total bucket — RenderNow
			// scope (joined with the StartDraw scope above into mlr_total).
			::mc2_cpu_proj_cost::Scope _f3_mlr_rendernow_scope(
			    ::mc2_cpu_proj_cost::BUCKET_MLR_TOTAL);
			theClipper->RenderNow();		//Draw the FX
		}


		if (useNonWeaponEffects)
		{
			ZoneScopedN("GameCamera::render weather");
			weather->render();				//Draw the weather
		}

		// DebugRenderer world primitives -- depth-tested, before post-process.
		// No-op when MC2_DEBUG_RENDERER is unset.
		{
			ZoneScopedN("GameCamera::render debugRendererFlushWorldPrims");
#ifdef MC2_IMGUI
			EditorInspector::flushDebugHighlight();  // IMG-INSPECT-3: queue highlight prims same-frame
#endif
			DebugRenderer::flushWorldPrims();
		}
	}

	if (drawOldWay && !inMovieMode)
	{
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_Perspective, 1);

		if (compass && (turn > 3) && drawCompass)
			compass->render();
	}
	
	//---------------------------------------------------------	
	//Check if we are inMovieMode and should be letterboxed.
	// draw letterboxes here.
	if (inMovieMode && (letterBoxPos != 0.0f))
	{
		//Figure out the two faces we need to draw based on letterBox Pos and Alpha
		float barTopX = screenResolution.y * letterBoxPos;
		float barBotX = screenResolution.y - barTopX;

		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_Texture, 0);
		
 		//------------------------------------
		gos_VERTEX gVertex[4];

		gVertex[0].x		= 0.0f;
		gVertex[0].y		= 0.0f;
		gVertex[0].z		= 0.00001f;
		gVertex[0].rhw		= 0.00001f;
		gVertex[0].u		= 0.0f;
		gVertex[0].v		= 0.0f;
		gVertex[0].argb		= (letterBoxAlpha << 24);
		gVertex[0].frgb		= 0xff000000;

		gVertex[1].x		= 0.0f;
		gVertex[1].y		= barTopX;
		gVertex[1].z		= 0.00001f;               
		gVertex[1].rhw		= 0.00001f;               
		gVertex[1].u		= 0.0f;                  
		gVertex[1].v		= 0.0f;                  
		gVertex[1].argb		= (letterBoxAlpha << 24);
		gVertex[1].frgb		= 0xff000000;            

		gVertex[2].x		= screenResolution.x;
		gVertex[2].y		= barTopX; 
		gVertex[2].z		= 0.00001f;               
		gVertex[2].rhw		= 0.00001f;               
		gVertex[2].u		= 0.0f;                  
		gVertex[2].v		= 0.0f;                  
		gVertex[2].argb		= (letterBoxAlpha << 24);
		gVertex[2].frgb		= 0xff000000;            

		gVertex[3].x		= screenResolution.x;
		gVertex[3].y		= 0.0f;
		gVertex[3].z		= 0.00001f;               
		gVertex[3].rhw		= 0.00001f;               
		gVertex[3].u		= 0.0f;                  
		gVertex[3].v		= 0.0f;                  
		gVertex[3].argb		= (letterBoxAlpha << 24);
		gVertex[3].frgb		= 0xff000000;            
		
		gos_DrawQuads(gVertex, 4);
		
		gVertex[0].x		= 0.0f;
		gVertex[0].y		= barBotX;
		gVertex[0].z		= 0.00001f;
		gVertex[0].rhw		= 0.00001f;
		gVertex[0].u		= 0.0f;
		gVertex[0].v		= 0.0f;
		gVertex[0].argb		= (letterBoxAlpha << 24);
		gVertex[0].frgb		= 0xff000000;

		gVertex[1].x		= screenResolution.x;
		gVertex[1].y		= barBotX;
		gVertex[1].z		= 0.00001f;               
		gVertex[1].rhw		= 0.00001f;               
		gVertex[1].u		= 0.0f;                  
		gVertex[1].v		= 0.0f;                  
		gVertex[1].argb		= (letterBoxAlpha << 24);
		gVertex[1].frgb		= 0xff000000;            

		gVertex[2].x		= screenResolution.x;
		gVertex[2].y		= screenResolution.y; 
		gVertex[2].z		= 0.00001f;               
		gVertex[2].rhw		= 0.00001f;               
		gVertex[2].u		= 0.0f;                  
		gVertex[2].v		= 0.0f;                  
		gVertex[2].argb		= (letterBoxAlpha << 24);
		gVertex[2].frgb		= 0xff000000;            

		gVertex[3].x		= 0.0f; 
		gVertex[3].y		= screenResolution.y;
		gVertex[3].z		= 0.00001f;               
		gVertex[3].rhw		= 0.00001f;               
		gVertex[3].u		= 0.0f;                  
		gVertex[3].v		= 0.0f;                  
		gVertex[3].argb		= (letterBoxAlpha << 24);
		gVertex[3].frgb		= 0xff000000;            
		
		gos_DrawQuads(gVertex, 4);
	}

	if (inMovieMode && (fadeAlpha != 0x0))
	{
		//We are fading to something other then clear screen.
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_Texture, 0);
		
 		//------------------------------------
		gos_VERTEX gVertex[4];

		gVertex[0].x		= 0.0f;
		gVertex[0].y		= 0.0f;
		gVertex[0].z		= 0.00001f;
		gVertex[0].rhw		= 0.00001f;
		gVertex[0].u		= 0.0f;
		gVertex[0].v		= 0.0f;
		gVertex[0].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[0].frgb		= 0xff000000;

		gVertex[1].x		= 0.0f;
		gVertex[1].y		= screenResolution.y;
		gVertex[1].z		= 0.00001f;               
		gVertex[1].rhw		= 0.00001f;               
		gVertex[1].u		= 0.0f;                  
		gVertex[1].v		= 0.0f;                  
		gVertex[1].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[1].frgb		= 0xff000000;            

		gVertex[2].x		= screenResolution.x;
		gVertex[2].y		= screenResolution.y; 
		gVertex[2].z		= 0.00001f;               
		gVertex[2].rhw		= 0.00001f;               
		gVertex[2].u		= 0.0f;                  
		gVertex[2].v		= 0.0f;                  
		gVertex[2].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[2].frgb		= 0xff000000;            

		gVertex[3].x		= screenResolution.x;
		gVertex[3].y		= 0.0f;
		gVertex[3].z		= 0.00001f;               
		gVertex[3].rhw		= 0.00001f;               
		gVertex[3].u		= 0.0f;                  
		gVertex[3].v		= 0.0f;                  
		gVertex[3].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[3].frgb		= 0xff000000;            
		
		gos_DrawQuads(gVertex, 4);
	}
	
	//-----------------------------------------------------
}	

//---------------------------------------------------------------------------
long GameCamera::activate (void)
{
	//------------------------------------------
	// If camera is already active, just return
	if (ready && active)
		return(NO_ERR);
	
	//---------------------------------------------------------
	// Camera always starts pointing at first mover in lists
	// CANNOT be infinite because we don't allow missions without at least 1 player mech!!
	MoverPtr firstMover = NULL;
	if (ObjectManager->getNumMovers() > 0) {
		long i = 0;
		firstMover = ObjectManager->getMover(i);
		while (firstMover && ((firstMover->getCommander()->getId() != Commander::home->getId()) || !firstMover->isOnGUI()))
		{
			i++;
			if (i == ObjectManager->getNumMovers())
				break;
			firstMover = ObjectManager->getMover(i); 
		}
	}
	
	if (firstMover)
	{
		Stuff::Vector3D newPosition(firstMover->getPosition());
		setPosition(newPosition);
	}

	if (land)
	{
		land->update();
	}
		
	allNormal();
	
	//updateDaylight(true);
	
	lastShadowLightPitch = lightPitch;
	
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
	
	theSky->setSkyNumber(mission->theSkyNumber);
			
 	return NO_ERR;
}

inline GameObjectPtr getCamObject (long partId, bool existsOnly) 
{
	GameObjectPtr obj = NULL;
	if (partId == -1)
		obj = NULL;
	else
		obj = ObjectManager->findByPartId(partId);

	if (existsOnly) 
	{
        STOP((""));
        //sebi: will crash if obj==NULL
		if (obj && 
			obj->getExists() && 
			(obj->getCommanderId() == Commander::home->getId()) || 
			(Team::home->teamLineOfSight(obj->getLOSPosition(),0.0f)))
			return(obj);
		return(NULL);
	}

	return(obj);
}

long cameraLineChanged = 0;
bool useLOSAngle = true;
//---------------------------------------------------------------------------
long GameCamera::update (void)
{
	if (lookTargetObject != -1)
		targetObject = getCamObject(lookTargetObject,true);
		
	if (targetObject && 
		targetObject->getExists() && 
		((targetObject->getCommanderId() == Commander::home->getId()) || 
		!targetObject->isMover() ||
		(targetObject->isMover() && ((Mover *)targetObject)->conStat >= CONTACT_SENSOR_QUALITY_1) ))
	{
		setPosition(targetObject->getPosition(),false);
	}
	else
	{
		targetObject = NULL;
	}

	//Force CameraAltitude to be less than max based on angle.  This keeps poly load relatively even	
	float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
	float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);
	
	if (cameraAltitude > testMax)
		cameraAltitude = testMax;

	if ((cameraAltitude < testMax) && (cameraAltitudeDesired > testMax))
		cameraAltitude = testMax;
												  
	// calculate new near and far plane distance based on 
	// Current altitude above terrain.
	float altitudePercent = (cameraAltitude - AltitudeMinimum) / (testMax - AltitudeMinimum);
	Camera::NearPlaneDistance = MinNearPlane + ((MaxNearPlane - MinNearPlane) * altitudePercent);
	Camera::FarPlaneDistance = MinFarPlane + ((MaxFarPlane - MinFarPlane) * altitudePercent);
	
	if (userInput->getKeyDown(KEY_LBRACKET) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		useLOSAngle ^= true;
	}		

#ifdef DEBUG_CAMERA
	if (userInput->getKeyDown(KEY_RBRACKET) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		Camera::NearPlaneDistance += 10.0f;
	}		

	if (userInput->getKeyDown(KEY_APOSTROPHE) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		Camera::FarPlaneDistance -= 1005.00f;
	}		

	if (userInput->getKeyDown(KEY_SEMICOLON) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		Camera::FarPlaneDistance += 1005.0f;
	}		

	char text[1024];
	sprintf(text,"Near Plane: %f     Far Plane: %f",Camera::NearPlaneDistance,Camera::FarPlaneDistance);

	DWORD width, height;
	Stuff::Vector4D moveHere;
	moveHere.x = 10.0f;
	moveHere.y = 10.0f;

	gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
	gos_TextStringLength(&width,&height,text);

	moveHere.z = width;
	moveHere.w = height;

	globalFloatHelp[currentFloatHelp].setHelpText(text);
	globalFloatHelp[currentFloatHelp].setScreenPos(moveHere);
	globalFloatHelp[currentFloatHelp].setForegroundColor(SD_GREEN);
	globalFloatHelp[currentFloatHelp].setBackgroundColor(SD_BLACK);
	globalFloatHelp[currentFloatHelp].setScale(1.0f);
	globalFloatHelp[currentFloatHelp].setProportional(true);
	globalFloatHelp[currentFloatHelp].setBold(false);
	globalFloatHelp[currentFloatHelp].setItalic(false);
	globalFloatHelp[currentFloatHelp].setWordWrap(false);

	currentFloatHelp++;

	gosASSERT(currentFloatHelp < MAX_FLOAT_HELPS);
#endif

	if (DisplayCameraAngle)
	{
		char text[1024];
		sprintf(text,"Camera Angle: %f degrees    Camera Altitude: %f    CameraPosition: X=%f Y=%f Z=%f   CameraRotation: %f",projectionAngle,cameraAltitude,position.x,position.y,position.z,cameraRotation);
		
		DWORD width, height;
		Stuff::Vector4D moveHere;
		moveHere.x = 10.0f;
		moveHere.y = 10.0f;

		gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
		gos_TextStringLength(&width,&height,text);

		moveHere.z = width;
		moveHere.w = height;

		globalFloatHelp->setFloatHelp(text,moveHere,SD_GREEN,SD_BLACK,1.0f,true,false,false,false);
	}

	if (!compass)	//Create it!
	{
		AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "compass" );
		compass = new BldgAppearance;
		compass->init( appearanceType );
	}

	long result = Camera::update();
	
//	if ((day2NightTransitionTime > 0.0f) && !getIsNight() && (fabs(lastShadowLightPitch-lightPitch) > MAX_SHADOW_PITCH_CHANGE))
//	{
//		forceShadowRecalc = true;
//		lastShadowLightPitch = lightPitch;
//	}
//	else
//	{
//		forceShadowRecalc = false;
//	}
	
	//Always TRUE for right now.  Debugging....
	//-fs
	//forceShadowRecalc = true;
	
	bool oldFog = useFog;
	bool oldShadows = useShadows;
	useFog = false;
	useShadows = false;
		
  	if (compass && (turn > 3))
	{
  		
   		compass->setObjectParameters(getPosition(),0.0f,false,0,0);
   		compass->setMoverParameters(0.0f);
   		compass->setGesture(0);
   		compass->setObjStatus(OBJECT_STATUS_DESTROYED);
   		compass->setInView(true);
   		compass->setVisibility(true,true);
   		compass->setFilterState(true);
		compass->setIsHudElement();
   		compass->update();		   //Force it to try and draw or stuff will not work!
	}

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
	
	return result;
}

//---------------------------------------------------------------------------
//
// Edit log
//
//---------------------------------------------------------------------------
