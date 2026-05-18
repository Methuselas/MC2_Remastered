//---------------------------------------------------------------------------
//
//	bdactor.cpp - This file contains the code for the building and tree appearance classes
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef BDACTOR_H
#include"bdactor.h"
#endif

// [TOBJSPLIT v1] RDTSC includes for the BldgAppearance/TreeAppearance probe
// points. __rdtsc() overhead ~5-10ns (cost_split_instrumentation_is_observer_
// effect_dominated.md). Accumulators defined in code/terrobj.cpp.
#include <intrin.h>
#include <stdlib.h>

#include "gos_static_prop_killswitch.h"
#include "gos_static_prop_batcher.h"
#include "gos_static_prop_registry.h"  // Stage 3.C: static-registry fast path
#include <unordered_map>  // LODBUG probe: tracks per-actor previous bldgShape*
#include "gos_object_parity_query.h"  // IsDualEmitArmed — Stage 2.D.2 dual-emit hook
#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon-zero
#include "gos_profiler.h"  // PERF DIAGNOSTIC 2026-05-06: ZoneScopedN for per-update breakdown

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef OBJSTATUS_H
#include"objstatus.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef ERR_H
#include"err.h"
#endif

#ifndef TXMMGR_H
#include"txmmgr.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#include "../code/unitdesg.h" /* just for definition of MIN_TERRAIN_PART_ID and MAX_MAP_CELL_WIDTH */
#include "../code/static_update_counters.h" /* [TOBJSPLIT v1] g_tobjAngularCyc / g_tobjProjCyc extern decls */
#include "gos_static_prop_batcher.h"
//******************************************************************************************
extern float	worldUnitsPerMeter;
extern bool 	drawTerrainGrid;
extern bool		useFog;

extern long 	mechRGBLookup[];
extern long 	mechRGBLookup2[];

extern int		ObjectTextureSize;

extern bool		reloadBounds;
extern float	metersPerWorldUnit;
extern bool		useShadows;

extern MidLevelRenderer::MLRClipper * theClipper;

extern bool useNonWeaponEffects;
extern bool useHighObjectDetail;
extern bool MLRVertexLimitReached;

#define SPINRATE					90.0f
#define BASE_NODE_RECYCLE_TIME		0.25f
#define MAX_WEAPON_NODES			4
//-----------------------------------------------------------------------------
// class BldgAppearanceType
void BldgAppearanceType::init (const char * fileName)
{
	AppearanceType::init(fileName);

	//----------------------------------------------
	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
		STOP(("Could not find building appearance INI file %s",iniName));

	result = iniFile.seekBlock("TGLData");
	if (result != NO_ERR)
		Fatal(result,"Could not find block in building appearance INI file");

	result = iniFile.readIdBoolean("SpinMe",spinMe);
	if (result != NO_ERR)
		spinMe = false;
		
	float nFrameRate = 0.0f;
	result = iniFile.readIdFloat("FrameRate",nFrameRate);
	if (result != NO_ERR)
		nFrameRate = 0.0f;

	result = iniFile.readIdBoolean("ForestClump",isForestClump);
	if (result != NO_ERR)
		isForestClump = false;
	   
	DWORD hotPinkRGB, hotGreenRGB, hotYellowRGB;
	result = iniFile.readIdULong("HotPinkRGB",hotPinkRGB);
	if (result != NO_ERR)
		hotPinkRGB = 0xffff00ff;
		
	result = iniFile.readIdULong("HotGreenRGB",hotGreenRGB);
	if (result != NO_ERR)
		hotGreenRGB = 0xff00ff00;
		
	result = iniFile.readIdULong("HotYellowRGB",hotYellowRGB);
	if (result != NO_ERR)
		hotYellowRGB = 0xffffff00;

	result = iniFile.readIdULong("TerrainLightRGB",terrainLightRGB);
	if (result != NO_ERR)
	{
		terrainLightRGB = 0xffffffff;
	}
	else
	{
		result = iniFile.readIdFloat("TerrainLightIntensity",terrainLightIntensity);
		if (result != NO_ERR)
			terrainLightIntensity = 0.5f;
			
		result = iniFile.readIdFloat("TerrainLightInnerRadius",terrainLightInnerRadius);
		if (result != NO_ERR)
			terrainLightInnerRadius = 100.0f;
			
		result = iniFile.readIdFloat("TerrainLightOuterRadius",terrainLightOuterRadius);
		if (result != NO_ERR)
			terrainLightOuterRadius = 250.0f;
	}
	
	char aseFileName[512];
	result = iniFile.readIdString("FileName",aseFileName,511);
	if (result != NO_ERR)
	{
		//Check for LOD filenames instead
		for (int i=0;i<MAX_LODS;i++)
		{
			char baseName[256];
			char baseLODDist[256];
			sprintf(baseName,"FileName%d",i);
			sprintf(baseLODDist,"Distance%d",i);
			
			result = iniFile.readIdString(baseName,aseFileName,511);
			if (result == NO_ERR)
			{
				result = iniFile.readIdFloat(baseLODDist,lodDistance[i]);
				if (result != NO_ERR)
					STOP(("LOD %d has no distance value in file %s",i,fileName));
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				//----------------------------------------------
				// Base LOD shape.  In stand Pose by default.
				bldgShape[i] = new TG_TypeMultiShape;
				gosASSERT(bldgShape[i] != NULL);
			
				FullPathFileName bldgName;
				bldgName.init(tglPath,aseFileName,".ase");
			
				bldgShape[i]->LoadTGMultiShapeFromASE(bldgName);
			}
			else if (!i)
			{
				STOP(("No base LOD for shape %s",fileName));
			}
		}
	}
	else
	{
		//----------------------------------------------
		// Base shape.  In stand Pose by default.
		bldgShape[0] = new TG_TypeMultiShape;
		gosASSERT(bldgShape[0] != NULL);
	
		FullPathFileName bldgName;
		bldgName.init(tglPath,aseFileName,".ase");
	
		bldgShape[0]->LoadTGMultiShapeFromASE(bldgName);
	}

	result = iniFile.readIdString("ShadowName",aseFileName,511);
	if (result == NO_ERR)
	{
		//----------------------------------------------
		// Base Shadow shape.
		bldgShadowShape = new TG_TypeMultiShape;
		gosASSERT(bldgShadowShape != NULL);
	
		FullPathFileName bldgName;
		bldgName.init(tglPath,aseFileName,".ase");
	
		bldgShadowShape->LoadTGMultiShapeFromASE(bldgName);
	}
 
	//destroyed state.
	result = iniFile.seekBlock("TGLDamage");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",aseFileName,511);
		if (result != NO_ERR)
			Fatal(result,"Could not find ASE FileName in building appearance INI file");
	
		FullPathFileName dmgName;
		dmgName.init(tglPath,aseFileName,".ase");
	
		bldgDmgShape = new TG_TypeMultiShape;
		gosASSERT(bldgDmgShape != NULL);
		bldgDmgShape->LoadTGMultiShapeFromASE(dmgName);

		if (!bldgDmgShape->GetNumShapes())
		{
			delete bldgDmgShape;
			bldgDmgShape = NULL;
		}
		else
		{
			// 2026-05-11 force-load damage-shape textures at appearType init.
			// LoadTGMultiShapeFromASE only sets texture NAMES, not handles —
			// the per-instance texture-load loop in setObjStatus only fires
			// when destruction happens at runtime. That's too late for
			// GpuStaticPropBatcher::finalizeGeometry, which builds its
			// per-packet texture array at mission-load. Without this loop,
			// damage-shape packets get layerForPacket=-1 and either render
			// with the wrong texture (orange-rectangle ghost) or get culled
			// from the multidraw, leaving destroyed buildings invisible.
			// Mirror the per-instance loop (bdactor.cpp:618-653) at the
			// type-level: load textures into mcTextureManager, set the
			// handles + alpha bits on the shared TG_TypeMultiShape so
			// every per-instance clone via CreateFrom inherits them.
			for (long i = 0; i < bldgDmgShape->GetNumTextures(); i++)
			{
				char txmName[1024];
				bldgDmgShape->GetTextureName(i, txmName, 256);
				char texturePath[1024];
				sprintf(texturePath, "%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
				FullPathFileName textureName;
				textureName.init(texturePath, txmName, "");
				if (fileExists(textureName))
				{
					if (S_strnicmp(txmName, "a_", 2) == 0)
					{
						DWORD gosHandle = mcTextureManager->loadTexture(
							textureName, gos_Texture_Alpha,
							gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosHandle != 0xffffffff);
						bldgDmgShape->SetTextureHandle(i, gosHandle);
						bldgDmgShape->SetTextureAlpha(i, true);
					}
					else
					{
						DWORD gosHandle = mcTextureManager->loadTexture(
							textureName, gos_Texture_Solid,
							gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosHandle != 0xffffffff);
						bldgDmgShape->SetTextureHandle(i, gosHandle);
						bldgDmgShape->SetTextureAlpha(i, false);
					}
				}
				else
				{
					bldgDmgShape->SetTextureHandle(i, 0xffffffff);
				}
			}
		}
		
		//Shadow for destroyed state.
		result = iniFile.readIdString("ShadowName",aseFileName,511);
		if (result == NO_ERR)
		{
			//----------------------------------------------
			// Base Shadow shape.
			bldgDmgShadowShape = new TG_TypeMultiShape;
			gosASSERT(bldgDmgShadowShape != NULL);
		
			FullPathFileName bldgName;
			bldgName.init(tglPath,aseFileName,".ase");
		
			bldgDmgShadowShape->LoadTGMultiShapeFromASE(bldgName);
			if (!bldgDmgShadowShape->GetNumShapes())
			{
				delete bldgDmgShadowShape;
				bldgDmgShadowShape = NULL;
			}
		}
	}
	else
	{
		bldgDmgShape = NULL;
		bldgDmgShadowShape = NULL;
	}

	result = iniFile.seekBlock("TGLDestructEffect");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",destructEffect,59);
		if (result != NO_ERR)
			STOP(("Could not Find DestructEffectName in building appearance INI file"));
	
	}
	else
	{
		destructEffect[0] = 0;
	}

	//--------------------------------------------------------------------
	// Load Animation Information.
	// We can load up to 10 Animation States.
	for (int i=0;i<MAX_BD_ANIMATIONS;i++)
	{
		char blockId[512];
		sprintf(blockId,"Animation:%d",i);
		
		result = iniFile.seekBlock(blockId);
		if (result == NO_ERR)
		{
			char animName[512];
			result = iniFile.readIdString("AnimationName",animName,511);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("LoopAnimation",bdAnimLoop[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("Reverse",bdReverse[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("Random",bdRandom[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdLong("StartFrame",bdStartF[i]);
			if (result != NO_ERR)
				bdStartF[i] = 0;
				
 			//-------------------------------
			// We have an animation to load.
			FullPathFileName animPath;
			animPath.init(tglPath,animName,".ase");

			FullPathFileName otherPath;
			otherPath.init(tglPath,animName,".agl");

			if (fileExists(animPath) || fileExists(otherPath))
			{
				bdAnimData[i] = new TG_AnimateShape;
				gosASSERT(bdAnimData[i] != NULL);
	
				//--------------------------------------------------------
				// If this animation does not exist, it is not a problem!
				// Building will simply freeze until animation is "over"
				bdAnimData[i]->LoadTGMultiShapeAnimationFromASE(animPath,bldgShape[0]);
			}
			else
				bdAnimData[i] = NULL;
		}
		else
		{
			bdAnimData[i] = NULL;
		}
	}
	
	//--------------------------------------------------------------------
	// We can also load the node to pitch and yaw for spotlights/turrets.
	result = iniFile.seekBlock("AnimationNode");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("AnimationNodeId",rotationalNodeId,24);
		gosASSERT(result == NO_ERR);
	}
	else
	{
		strcpy(rotationalNodeId,"NONE");
	}
	
	if (nFrameRate != 0.0f)
	{
		for (long i=0;i<MAX_BD_ANIMATIONS;i++)
			setFrameRate(i,nFrameRate);
	}

	//-----------------------------------------------
	// Load up the Weapon Node Data.
	numWeaponNodes = 0;
	nodeData = NULL;
	result = iniFile.seekBlock("WeaponNode");
	if (result == NO_ERR)
	{
		nodeData = (NodeData *)AppearanceTypeList::appearanceHeap->Malloc(sizeof(NodeData)*(MAX_WEAPON_NODES));
		gosASSERT(nodeData != NULL);
		
		for (int i=0;i<MAX_WEAPON_NODES;i++)
		{
			char blockId[512];
			sprintf(blockId,"WeaponNodeId%d",i);
			
			char weaponName[512];
			result = iniFile.readIdString(blockId,weaponName,511);
			if (result != NO_ERR)
			{
				strcpy(weaponName,"NONE");
			}
			
			nodeData[i].nodeId = (char *)AppearanceTypeList::appearanceHeap->Malloc(strlen(weaponName)+1);
			gosASSERT(nodeData[i].nodeId != NULL);
				
			strcpy(nodeData[i].nodeId,weaponName);
			nodeData[i].weaponType = 0;
			numWeaponNodes++;
 		}
	}

	for (int i=0;i<MAX_LODS;i++)
	{
		if (bldgShape[i])
			bldgShape[i]->SetLightRGBs(hotPinkRGB, hotGreenRGB, hotYellowRGB);
	}
}

//----------------------------------------------------------------------------
void BldgAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	for (long i=0;i<MAX_LODS;i++)
	{
		if (bldgShape[i])
		{
			delete bldgShape[i];
			bldgShape[i] = NULL;
		}
	}

	if (bldgShadowShape)
	{
		delete bldgShadowShape;
		bldgShadowShape = NULL;
	}
	
 	if (bldgDmgShape)
	{
		delete bldgDmgShape;
		bldgDmgShape = NULL;
	}
	
	if (bldgDmgShadowShape)
	{
		delete bldgDmgShadowShape;
		bldgDmgShadowShape = NULL;
	}
	
 	for (int i=0;i<MAX_BD_ANIMATIONS;i++)
	{
		if (bdAnimData[i])
		{
			delete bdAnimData[i];
			bdAnimData[i] = NULL;
		}
	}
}

//-----------------------------------------------------------------------------
void BldgAppearanceType::setAnimation (TG_MultiShapePtr shape, DWORD animationNum)
{
	gosASSERT(shape != NULL);
	gosASSERT(animationNum != 0xffffffff);
	gosASSERT(animationNum < MAX_BD_ANIMATIONS);

	if (bdAnimData[animationNum])
		bdAnimData[animationNum]->SetAnimationState(shape);
	else
		shape->ClearAnimation();
}

//-----------------------------------------------------------------------------
// class BldgAppearance
void BldgAppearance::setWeaponNodeUsed (long weaponNode)
{
	weaponNode -= appearType->numWeaponNodes;
   	if ((weaponNode >= 0) && (weaponNode < appearType->numWeaponNodes))
	{
		nodeUsed[weaponNode]++;
		nodeRecycle[weaponNode] = BASE_NODE_RECYCLE_TIME;
	}
}

//-----------------------------------------------------------------------------
Stuff::Vector3D BldgAppearance::getWeaponNodePosition (long nodeId)
{
	Stuff::Vector3D result = position;
	if ((nodeId < 0) || (nodeId >= appearType->numWeaponNodes))
		return result;
	
	//We already know we are using this node.  Do NOT increment recycle or nodeUsed!
		
   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = land->getTerrainElevation(position);
   	xlatPosition.z = position.y;
   
   	Stuff::UnitQuaternion torsoRot;
   	torsoRot = Stuff::EulerAngles(0.0f,(turretYaw * DEGREES_TO_RADS),0.0f);
	if (rotationalNodeId == -1)
	{
		if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
			rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
		else
			rotationalNodeId = -2;
	}
   
	if (rotationalNodeId >= 0)
	   	bldgShape->SetNodeRotation(rotationalNodeId,&torsoRot);

	result = bldgShape->GetTransformedNodePosition(&xlatPosition,&qRotation,appearType->nodeData[nodeId].nodeId);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
 	return result;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D BldgAppearance::getHitNode (void)
{
	if (hitNodeId == -1)
		hitNodeId = bldgShape->GetNodeNameId("hitnode");

	Stuff::Vector3D result = getNodeIdPosition(hitNodeId);
 	return result;
}

//-----------------------------------------------------------------------------
long BldgAppearance::getWeaponNode (long weaponType)
{
	//------------------------------------------------
	// Scan all weapon nodes and find least used one.
	long leastUsed = 999999999;
	long bestNode = -1;
	for (long i=0;i<appearType->numWeaponNodes;i++)
	{
		if (nodeUsed[i] < leastUsed)
		{
			leastUsed = nodeUsed[i];
			bestNode = i;
		}
	}
		
   	if ((bestNode < 0) || (bestNode >= appearType->numWeaponNodes))
   		return -1;

 	return bestNode;
}
		
//-----------------------------------------------------------------------------
float BldgAppearance::getWeaponNodeRecycle (long node)
{
	if ((node >= 0) && (node < appearType->numWeaponNodes))
		return nodeRecycle[node];
		
	return 0.0f;
}

//-----------------------------------------------------------------------------
void BldgAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	appearType = (BldgAppearanceType *)tree;

	shapeMin.x = shapeMin.y = -25;
	shapeMax.x = shapeMax.y = 50;

    status = OBJECT_STATUS_NORMAL; // sebi: init so will not be garbage

	bdAnimationState =-1;
	currentFrame = 0.0f;
	bdFrameRate = 0.0f;
	isReversed = false;
	isLooping = false;
	setFirstFrame = false;
	canTransition = true;

	// Slice 2 (object-offload) substrate: never set true in Stage 2.A.
	needsFullBakeNextFrame = false;

	// Stage 3.D: zero-init static-registry state (mirror of TreeAppearance::init).
	staticReg = {};

	paintScheme = -1;
	objectNameId = 30469;
	hazeFactor = 0.0f;

	rotationalNodeId = -1;
	hitNodeId = activityNodeId = activityNode1Id = -1;

	currentFlash = duration = flashDuration = 0.0f;
	flashColor = 0x00000000;
	drawFlash = false;

	pointLight = NULL;
	lightId = 0xffffffff;
	forceLightsOut = false;
	
	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	position.Zero();
	rotation = 0.0f;
	selected = 0;
	teamId = -1;
	homeTeamRelationship = 0;
	actualRotation = rotation;

	currentLOD = 0;
 	
	turretYaw = turretPitch = 0.0f;
	
	destructFX = NULL;
	activity = NULL;
	activity1 = NULL;
	isActivitying = false;

	OBBRadius = -1.0f;
	highZ = -1.0f;
	
	nodeUsed = NULL;
	nodeRecycle = NULL;
	
	beenInView = false;

	fogLightSet = false;
	if (appearType)
	{
		bldgShape = appearType->bldgShape[0]->CreateFrom();

		//-------------------------------------------------
		// Load the texture and store its handle.
		for (int i=0;i<bldgShape->GetNumTextures();i++)
		{
			char txmName[1024];
			bldgShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (fileExists(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					bldgShape->SetTextureHandle(i,gosTextureHandle);
					bldgShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					bldgShape->SetTextureHandle(i,gosTextureHandle);
					bldgShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				bldgShape->SetTextureHandle(i,0xffffffff);
			}
		}
		
		if (appearType->bldgShadowShape)
		{
			bldgShadowShape = appearType->bldgShadowShape->CreateFrom();
	
			//-------------------------------------------------
			// Load the texture and store its handle.
			for (long i=0;i<bldgShadowShape->GetNumTextures();i++)
			{
				char txmName[1024];
				bldgShadowShape->GetTextureName(i,txmName,256);
		
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (fileExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						bldgShadowShape->SetTextureHandle(i,gosTextureHandle);
						bldgShadowShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						bldgShadowShape->SetTextureHandle(i,gosTextureHandle);
						bldgShadowShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					bldgShadowShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}
		else
		{
			bldgShadowShape = NULL;
		}
 		
		Stuff::Vector3D boxCoords[8];
		Stuff::Vector3D nodeCenter = bldgShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
 		float testRadius = 0.0;
		
		for (int i=0;i<8;i++)
		{
			testRadius = boxCoords[i].GetLength();
			if (OBBRadius < testRadius)
				OBBRadius = testRadius;

			if (boxCoords[i].z > highZ)
				highZ = boxCoords[i].z;
		}
		
		appearType->boundsUpperLeftX = (-OBBRadius * 2.0);
		appearType->boundsUpperLeftY = (-OBBRadius * 2.0);
		   					 
		appearType->boundsLowerRightX = (OBBRadius * 2.0);
		appearType->boundsLowerRightY = (OBBRadius);
		
		if (!appearType->getDesignerTypeBounds())
		{
			Stuff::Vector3D nodeCenter = bldgShape->GetRootNodeCenter();
			appearType->typeUpperLeft.Add(bldgShape->GetMinBox(),nodeCenter);
			appearType->typeLowerRight.Add(bldgShape->GetMaxBox(),nodeCenter);
		}
		
 		if (appearType->numWeaponNodes)
		{
			nodeUsed = (long *)AppearanceTypeList::appearanceHeap->Malloc(sizeof(long) * appearType->numWeaponNodes);
			gosASSERT(nodeUsed != NULL);
			memset(nodeUsed,0,sizeof(long) * appearType->numWeaponNodes);
			
			nodeRecycle = (float *)AppearanceTypeList::appearanceHeap->Malloc(sizeof(float) * appearType->numWeaponNodes);
			gosASSERT(nodeRecycle != NULL);
			
			for (long i=0;i<appearType->numWeaponNodes;i++)
				nodeRecycle[i] = 0.0f;
		}

		// Register this building's TG_TypeShape variants with the GPU static
		// prop batcher. Idempotent -- duplicate calls across instances are
		// cheap. Covers all LOD base shapes plus destroyed/damaged variants
		// and their shadow proxies. Registration happens after texture
		// handles are resolved so packets capture the correct GL handle.
		for (int i = 0; i < MAX_LODS; ++i)
			GpuStaticPropBatcher::instance().registerMultiShape(appearType->bldgShape[i]);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->bldgShadowShape);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->bldgDmgShape);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->bldgDmgShadowShape);
	}
}

//-----------------------------------------------------------------------------
void BldgAppearance::setObjStatus (long oStatus)
{
	if (status != oStatus)
	{
		if ((oStatus == OBJECT_STATUS_DESTROYED) || (oStatus == OBJECT_STATUS_DISABLED))
		{
			if (appearType->bldgDmgShape)
			{
				if (bldgShape)
				{
					bldgShape->ClearAnimation();
					delete bldgShape;
					bldgShape = NULL;
				}
				
				bldgShape = appearType->bldgDmgShape->CreateFrom();
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
				
				beenInView = false; 
				currentLOD = 0;
			}
			
			if (appearType->bldgDmgShadowShape)
			{
				if (bldgShadowShape)
				{
					bldgShadowShape->ClearAnimation();
					delete bldgShadowShape;
					bldgShadowShape = NULL;
				}
				
				bldgShadowShape = appearType->bldgDmgShadowShape->CreateFrom();
				
				//Do shadows need to animate??
				//if (bdAnimationState != -1)
					//appearType->setAnimation(bldgShadowShape,bdAnimationState);
				
				beenInView = false; 
			}

			stopActivity();
		}
		
		if (oStatus == OBJECT_STATUS_NORMAL)
		{
			if (appearType->bldgShape[0])
			{
				if (bldgShape)
				{
					bldgShape->ClearAnimation();
					delete bldgShape;
					bldgShape = NULL;
				}
				
				bldgShape = appearType->bldgShape[0]->CreateFrom();
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
				
				beenInView = false; 
			}
			
			if (appearType->bldgShadowShape)
			{
				if (bldgShadowShape)
				{
					bldgShadowShape->ClearAnimation();
					delete bldgShadowShape;
					bldgShadowShape = NULL;
				}
				
				bldgShadowShape = appearType->bldgShadowShape->CreateFrom();
				
				//Do shadows need to animate??
//				if (bdAnimationState != -1)
					//appearType->setAnimation(bldgShadowShape,bdAnimationState);
				
				beenInView = false; 
			}
		}
		
		if (bldgShape)
		{
			//-------------------------------------------------
			// Load the texture and store its handle.
			for (long i=0;i<bldgShape->GetNumTextures();i++)
			{
				char txmName[1024];
				bldgShape->GetTextureName(i,txmName,256);
	
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (fileExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						bldgShape->SetTextureHandle(i,gosTextureHandle);
						bldgShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						bldgShape->SetTextureHandle(i,gosTextureHandle);
						bldgShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					//PAUSE(("Warning: %s texture name not found",textureName));
					bldgShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}

		if (bldgShadowShape)
		{
			//-------------------------------------------------
			// Load the texture for the shadow and store its handle.
			for (long i=0;i<bldgShadowShape->GetNumTextures();i++)
			{
				char txmName[1024];
				bldgShadowShape->GetTextureName(i,txmName,256);
	
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (fileExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						bldgShadowShape->SetTextureHandle(i,gosTextureHandle);
						bldgShadowShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						bldgShadowShape->SetTextureHandle(i,gosTextureHandle);
						bldgShadowShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					bldgShadowShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}
	}
	
	status = oStatus;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D BldgAppearance::getNodeNamePosition (const char *nodeName)
{
	Stuff::Vector3D result = position;
	
   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;
   
	result = bldgShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeName);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D BldgAppearance::getNodeIdPosition (long nodeId)
{
	Stuff::Vector3D result = position;
	
   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;
   
	result = bldgShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeId);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

//-----------------------------------------------------------------------------
bool BldgAppearance::PerPolySelect (long mouseX, long mouseY)
{
	return bldgShape->PerPolySelect(mouseX,mouseY);
}

//-----------------------------------------------------------------------------
void BldgAppearance::setGesture (unsigned long gestureId)
{
	//------------------------------------------------------------
	// Check if state is possible.
	if (gestureId >= MAX_BD_ANIMATIONS)
		return;

	//------------------------------------------------------------
	// Check if object destroyed.  If so, no animation!
	if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
		return;
		
	if (gestureId == bdAnimationState)
		return;

	//----------------------------------------------------------------------
	// If state is OK, set animation data, set first frame, set loop and 
	// reverse flag, and start it going until you hear otherwise.
	appearType->setAnimation(bldgShape,gestureId);
	bdAnimationState = gestureId;
	currentFrame = 0.0f;
	if (appearType->bdStartF[gestureId])
		currentFrame = appearType->bdStartF[gestureId];
		
	isReversed = false;
	
	if (appearType->isReversed(bdAnimationState))
	{
		currentFrame = appearType->getNumFrames(bdAnimationState)-1;
		isReversed = true;
	}
	
	if (appearType->isRandom(bdAnimationState))
	{
		currentFrame = RandomNumber(appearType->getNumFrames(bdAnimationState)-1);
	}
	
	isLooping = appearType->isLooped(bdAnimationState);
	
	bdFrameRate = appearType->getFrameRate(bdAnimationState);
	
	setFirstFrame = true;
	if (bdFrameRate > Stuff::SMALL)
		canTransition = false;
	else
		canTransition = true;		//We can change immediately to another animation because we have no animation for this state!
}

//-----------------------------------------------------------------------------
void BldgAppearance::setMoverParameters (float turretRot, float lArmRot, float rArmRot, bool isAirborne)
{
	turretYaw = turretRot;
	turretPitch = lArmRot;
}

//-----------------------------------------------------------------------------
void BldgAppearance::setObjectParameters (const Stuff::Vector3D &pos, float Rot, long sel, long team, long homeRelations)
{
	rotation = Rot;

	position = pos;

	selected = sel;

	actualRotation = Rot;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

//-----------------------------------------------------------------------------
bool BldgAppearance::isMouseOver (float px, float py)
{
	if (inView)
	{
		if ((px <= lowerRight.x) && (py <= lowerRight.y) &&
			(px >= upperLeft.x) &&
			(py >= upperLeft.y))
		{
			return inView;
		}
		else
		{
			return FALSE;
		}
	}
	
	return(inView);
}	

//-----------------------------------------------------------------------------
bool BldgAppearance::recalcBounds (void)
{
	// [TOBJSPLIT v1] accumulators declared in code/static_update_counters.h
	// (included above via ../code/static_update_counters.h).
	// s_tobjSplitEnabled is a file-static duplicate (one getenv per TU;
	// process-start-constant -- no observable cost when disabled).
	static bool s_tobjSplitEnabled = (getenv("MC2_TOBJ_COST_SPLIT") != nullptr);

	Stuff::Vector4D tempPos;
	inView = false;

	float distanceToEye = 0.0f;

	if (eye)
	{
		//-------------------------------------------------------------------
		//NEW METHOD from the WAY BACK Days
		inView = true;

		// [TOBJSPLIT v1] ANGULAR bracket: matrix-free sphere angular clip.
		// Disjoint from PROJ below; reads cycle counter immediately before/after.
		{
		unsigned long long _tsA = s_tobjSplitEnabled ? __rdtsc() : 0ULL;
		if (eye->usePerspective)
		{
			Stuff::Vector3D cameraPos;
			cameraPos.x = -eye->getCameraOrigin().x;
			cameraPos.y = eye->getCameraOrigin().z;
			cameraPos.z = eye->getCameraOrigin().y;
			float vClipConstant = eye->verticalSphereClipConstant;
			float hClipConstant = eye->horizontalSphereClipConstant;

			Stuff::Vector3D objectCenter;
			objectCenter.Subtract(position,cameraPos);
			Camera::cameraFrame.trans_to_frame(objectCenter);
			distanceToEye = objectCenter.GetApproximateLength();
			float clip_distance = fabs(1.0f / objectCenter.y);

			//Is vertex on Screen OR close enough to screen that its triangle MAY be visible?
			// WE have removed the atans here by simply taking the tan of the angle we want above.
			float object_angle = fabs(objectCenter.z) * clip_distance;
			float extent_angle = bldgShape->GetExtentRadius() / distanceToEye;
			if (object_angle > (vClipConstant + extent_angle))
			{
				//In theory, we would return here.  Object is NOT on screen.
				inView = false;
			}
			else
			{
				object_angle = fabs(objectCenter.x) * clip_distance;
				if (object_angle > (hClipConstant + extent_angle))
				{
					//In theory, we would return here.  Object is NOT on screen.
					inView = false;
				}
			}
		}
		if (s_tobjSplitEnabled) g_tobjAngularCyc += __rdtsc() - _tsA;
		}  // end ANGULAR bracket

		//Can we be seen at all?
		// If yes, check if we are behind fog plane.
		// [TOBJSPLIT v1] PROJ bracket: projectForScreenXY + 8-corner box + fog.
		// Disjoint from ANGULAR above; this is the body targeted for deletion.
		unsigned long long _tsP = s_tobjSplitEnabled ? __rdtsc() : 0ULL;
		if (inView)
		{
			//ALWAYS need to do this or select is YAYA
			// But now inView is correct!!
			// [PROJECTZ:ScreenXYOracle id=bdactor_screen_pos_a]
			eye->projectForScreenXY(position,screenPos);

			if (eye->usePerspective)
			{
				if (distanceToEye > Camera::MaxClipDistance)
				{
					hazeFactor = 1.0f;
					inView = false;
				}
				else if (distanceToEye > Camera::MinHazeDistance)
				{
					Camera::HazeFactor = (distanceToEye - Camera::MinHazeDistance) * Camera::DistanceFactor;
					inView = true;
				}
				else
				{
					Camera::HazeFactor = 0.0f;
					inView = true;
				}
			}
			else
			{
				Camera::HazeFactor = 0.0f;
				inView = true;
			}
		}
		
		//If we were not behind fog plane, do a bunch O math we need later!!
		if (inView)
		{
			if (reloadBounds)
				appearType->reinit();

			appearType->boundsLowerRightY = (OBBRadius * eye->getTiltFactor() * 2.0f);
			
			//-------------------------------------------------------------------------
			// do a rough check if on screen.  If no where near, do NOT do the below.
			// Mighty mighty slow!!!!
			// Use the original check done before all this 3D madness.  Dig out sourceSafe tomorrow!
			tempPos = screenPos;
			upperLeft.x = tempPos.x;
			upperLeft.y = tempPos.y;
			
			lowerRight.x = tempPos.x;
			lowerRight.y = tempPos.y;
			
			upperLeft.x += (appearType->boundsUpperLeftX * eye->getScaleFactor());
			upperLeft.y += (appearType->boundsUpperLeftY * eye->getScaleFactor());
	
			lowerRight.x += (appearType->boundsLowerRightX * eye->getScaleFactor());
			lowerRight.y += (appearType->boundsLowerRightY * eye->getScaleFactor());

			if ((lowerRight.x >= 0) && (lowerRight.y >= 0) &&
				(upperLeft.x <= eye->getScreenResX()) &&
				(upperLeft.y <= eye->getScreenResY()))
			{
				//We are on screen.  Figure out selection box.
				Stuff::Vector3D boxCoords[8];
				Stuff::Vector4D bcsp[8];

				Stuff::Vector3D boxStart;
				boxStart.x = -appearType->typeUpperLeft.x;
				boxStart.y = appearType->typeUpperLeft.z;
				boxStart.z = appearType->typeUpperLeft.y;

				Stuff::Vector3D boxEnd;
				boxEnd.x = -appearType->typeLowerRight.x;
				boxEnd.y = appearType->typeLowerRight.z;
				boxEnd.z = appearType->typeLowerRight.y;
				
				Stuff::Vector3D addCoords;
		
				addCoords.x = boxStart.x;
				addCoords.y = boxStart.y;
				addCoords.z = boxEnd.z;
				if (rotation != 0.0f)
					Rotate(addCoords,-rotation);
				
				boxCoords[0].Add(position,addCoords);
		
				addCoords.x = boxStart.x;
				addCoords.y = boxEnd.y;  
				addCoords.z = boxEnd.z;  		
				if (rotation != 0.0f)
					Rotate(addCoords,-rotation);
				
				boxCoords[1].Add(position,addCoords);
		
				addCoords.x = boxEnd.x; 
				addCoords.y = boxEnd.y; 
				addCoords.z = boxEnd.z; 		
				if (rotation != 0.0f)
					Rotate(addCoords,-rotation);
				
				boxCoords[2].Add(position,addCoords);
				
				addCoords.x = boxEnd.x;   
				addCoords.y = boxStart.y; 
				addCoords.z = boxEnd.z;   		
				if (rotation != 0.0f)
					Rotate(addCoords,-rotation);
				
				boxCoords[3].Add(position,addCoords);
				
				addCoords.x = boxStart.x;
				addCoords.y = boxStart.y; 
				addCoords.z = boxStart.z; 		
				if (rotation != 0.0f)
					Rotate(addCoords,-rotation);
				
				boxCoords[4].Add(position,addCoords);
							  
				addCoords.x = boxEnd.x;   
				addCoords.y = boxStart.y;   
				addCoords.z = boxStart.z; 
				if (rotation != 0.0f)
					Rotate(addCoords,-rotation);
				
				boxCoords[5].Add(position,addCoords);
				
				addCoords.x = boxEnd.x;   
				addCoords.y = boxEnd.y;   
				addCoords.z = boxStart.z; 
				if (rotation != 0.0f)
					Rotate(addCoords,-rotation);
				
				boxCoords[6].Add(position,addCoords);
				
				addCoords.x = boxStart.x; 
				addCoords.y = boxEnd.y;   
				addCoords.z = boxStart.z; 
				if (rotation != 0.0f)
					Rotate(addCoords,-rotation);
				
				boxCoords[7].Add(position,addCoords);
				
				float maxX = 0.0f, maxY = 0.0f;
				float minX = 0.0f, minY = 0.0f;

				for (long i=0;i<8;i++)
				{
					// [PROJECTZ:ScreenXYOracle id=bdactor_box_rect_a]
					eye->projectForScreenXY(boxCoords[i],bcsp[i]);
					if (!i)
					{
						maxX = minX = bcsp[i].x;
						maxY = minY = bcsp[i].y;
					}

					if (i)
					{
						if (bcsp[i].x > maxX)
							maxX = bcsp[i].x;

						if (bcsp[i].x < minX)
							minX = bcsp[i].x;

						if (bcsp[i].y > maxY)
							maxY = bcsp[i].y;
						
						if (bcsp[i].y < minY)
							minY = bcsp[i].y;
					}
				}
		
				upperLeft.x = minX;
				upperLeft.y = minY;
				lowerRight.x = maxX;
				lowerRight.y = maxY;
				
				if ((lowerRight.x >= 0) && (lowerRight.y >= 0) &&
					(upperLeft.x <= eye->getScreenResX()) &&
					(upperLeft.y <= eye->getScreenResY()))
				{
					inView = true;

					if ((status != OBJECT_STATUS_DESTROYED) && (status != OBJECT_STATUS_DISABLED))
					{
						//-------------------------------------------------------------------------------
						//Set LOD of Model here because we have the distance and we KNOW we can see it!
						bool baseLOD = true;
						DWORD selectLOD = 0;
						// 2026-05-12 TEMP WORKAROUND: pin every BldgAppearance-class actor
						// (buildings, fences, small props, beacons, sandbags, etc.) to LOD 0.
						// The original LOD-selection logic that lived here (animation skip
						// + useHighObjectDetail distance check + lowest-LOD fallback) was
						// observed to render LOD-1+ actors as fully invisible: post-swap
						// submitMultiShape returns true and the instance lands in
						// bucket.instances, but the actor produces zero fragments. The bug
						// is downstream of submit() acceptance; root cause not yet isolated
						// (MC2_STATIC_FORCE_ADMIT and MC2_FORCE_DYNAMIC_BUILDINGS both
						// failed to recover visibility, ruling out substrate-cull rejection
						// and the static-replay/recipe path). LODBUG investigation probes
						// remain in this file (env-gated MC2_LODBUG_TRACE) and in
						// gos_static_prop_batcher.cpp / static_prop.frag (mode 8) so the
						// investigation can resume — see also the sibling
						// memory/bldg_animation_lod_swap_unsafe.md which already documents
						// LOD-swap unsafety for animated buildings and predates this
						// broader pinning.
						//
						// Cost: a few extra vertices per static prop at distance. On modern
						// hardware (the engine targets >=2010 GL 4.3) this is negligible —
						// stock missions cap ~300 static prop instances per type per frame.
						// Trees use TreeAppearance (a separate class) and are NOT pinned
						// here; they get a sibling treatment / proper LOD fix as a follow-up.
						//
						// Restore the original logic by reverting this hunk once the LOD-1
						// invisibility root cause is fixed.
						(void)useHighObjectDetail;
						
						// we are at this LOD level.
						if (selectLOD != currentLOD)
						{
							// LODBUG probe: capture the pre-swap LOD so the
							// trace line below can log the transition.  Env-
							// gated MC2_LODBUG_TRACE.  Hypothesis under
							// investigation: appearType->bldgShape[1+]'s leaves
							// (different TG_TypeShape* pointers than LOD-0's)
							// were never registered by the static-prop batcher
							// at type-init time, so on the very first LOD swap
							// for a building, submit() rejects every leaf and
							// the actor renders nothing under GPU mode.  See
							// memory/bldg_animation_lod_swap_unsafe.md for the
							// sibling per-type-state trap.
							const DWORD oldLOD_lodbug = currentLOD;

							currentLOD = selectLOD;

							bldgShape->ClearAnimation();
							delete bldgShape;
							bldgShape = NULL;

							bldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
							if (bdAnimationState != -1)
								appearType->setAnimation(bldgShape,bdAnimationState);

							// LODBUG trace point.  One line per swap event +
							// one per leaf.  User correlates leaf typeNode
							// pointers here against the batcher's once-per-
							// type "[GPUPROPS] unregistered type" prints in
							// stderr — match by pointer to confirm whether the
							// new LOD's leaves were registered.
							static const bool s_lodBugTrace =
								(getenv("MC2_LODBUG_TRACE") != nullptr);
							if (s_lodBugTrace) {
								const long nLeaves = bldgShape ? bldgShape->GetNumShapes() : -1;
								printf("[LODBUG v1] event=lod_swap actor=%p "
								       "oldLOD=%u newLOD=%u appearType=%p "
								       "newShape=%p dist=%.1f numLeaves=%ld\n",
								       (void*)this,
								       (unsigned)oldLOD_lodbug,
								       (unsigned)currentLOD,
								       (void*)appearType,
								       (void*)bldgShape,
								       (float)distanceToEye,
								       nLeaves);
								for (long li = 0; li < nLeaves; ++li) {
									const TG_ShapeRec* rec = bldgShape->GetShapeRec((int)li);
									if (!rec || !rec->node) continue;
									// rec->node->getNodeName() is the type's
									// nodeId (public passthrough at tgl.h:964).
									// User correlates the `shape=` pointer
									// printed below against the batcher's
									// "[GPUPROPS] unregistered type 0x.. for
									// shape 0x.." stderr lines.
									const char* name = rec->node->getNodeName();
									printf("[LODBUG v1] event=lod_swap_leaf "
									       "actor=%p leaf=%ld shape=%p "
									       "node=\"%s\"\n",
									       (void*)this, li,
									       (void*)rec->node,
									       name ? name : "(null)");
								}
								fflush(stdout);
							}

							//-------------------------------------------------
							// Load the texture and store its handle.
							for (long j=0;j<bldgShape->GetNumTextures();j++)
							{
								char txmName[1024];
								bldgShape->GetTextureName(j,txmName,256);

								char texturePath[1024];
								sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);

								FullPathFileName textureName;
								textureName.init(texturePath,txmName,"");

								if (fileExists(textureName))
								{
									if (S_strnicmp(txmName,"a_",2) == 0)
									{
										DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
										gosASSERT(gosTextureHandle != 0xffffffff);
										bldgShape->SetTextureHandle(j,gosTextureHandle);
										bldgShape->SetTextureAlpha(j,true);
									}
									else
									{
										DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
										gosASSERT(gosTextureHandle != 0xffffffff);
										bldgShape->SetTextureHandle(j,gosTextureHandle);
										bldgShape->SetTextureAlpha(j,false);
									}
								}
								else
								{
									//PAUSE(("Warning: %s texture name not found",textureName));
									bldgShape->SetTextureHandle(j,0xffffffff);
								}
							}
						}

						//ONLY change if we need
						if (currentLOD && baseLOD)
						{
						// we are at the Base LOD level.
							currentLOD = 0;
							
							bldgShape->ClearAnimation();
							delete bldgShape;
							bldgShape = NULL;
							
							bldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
							if (bdAnimationState != -1)
								appearType->setAnimation(bldgShape,bdAnimationState);
							
							//-------------------------------------------------
							// Load the texture and store its handle.
							for (long i=0;i<bldgShape->GetNumTextures();i++)
							{
								char txmName[1024];
								bldgShape->GetTextureName(i,txmName,256);
										
								char texturePath[1024];
								sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
						
								FullPathFileName textureName;
								textureName.init(texturePath,txmName,"");
										
								if (fileExists(textureName))
								{
									if (S_strnicmp(txmName,"a_",2) == 0)
									{
										DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
										gosASSERT(gosTextureHandle != 0xffffffff);
										bldgShape->SetTextureHandle(i,gosTextureHandle);
										bldgShape->SetTextureAlpha(i,true);
									}
									else
									{
										DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
										gosASSERT(gosTextureHandle != 0xffffffff);
										bldgShape->SetTextureHandle(i,gosTextureHandle);
										bldgShape->SetTextureAlpha(i,false);
									}
								}
								else
								{
									//PAUSE(("Warning: %s texture name not found",textureName));
									bldgShape->SetTextureHandle(i,0xffffffff);
								}
							}
						}
					}
				}
				else
				{
					inView = false;		//Did alot of extra work checking this, but WHY draw and insult to injury?
				}
			}
			else
			{
				inView = false;
			}
		}
		if (s_tobjSplitEnabled) g_tobjProjCyc += __rdtsc() - _tsP;
		// end PROJ bracket
	}


	return(inView);
}

//-----------------------------------------------------------------------------
bool BldgAppearance::playDestruction (void)
{
	//Check if there is a Destruct FX
	if (appearType->destructEffect[0])
	{
		//--------------------------------------------
		// Yes, load it on up.
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(appearType->destructEffect);
		
		if (gosEffectSpec)
		{
			destructFX = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(destructFX != NULL);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		
			Stuff::Point3D			tPosition;
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			
            //Stuff::Vector3D offsetPosition;
			//offsetPosition.x = Terrain::worldUnitsPerVertex / 3.0f;
			//offsetPosition.y = -(Terrain::worldUnitsPerVertex / 3.0f);
			//offsetPosition.z = 0.0f;

			//OppRotate(offsetPosition,rotation);

			Stuff::Vector3D actualPosition = position;
			//actualPosition.Add(position,offsetPosition);

			tPosition.x = -actualPosition.x;
			tPosition.y = actualPosition.z;
			tPosition.z = actualPosition.y;

			float yaw = (180.0f + rotation) * DEGREES_TO_RADS;
			Stuff::UnitQuaternion rot;
			rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);

			shapeOrigin.BuildRotation(rot);
			shapeOrigin.BuildTranslation(tPosition);
			
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
			destructFX->Start(&info);
			
			return true;
		}
		
		return false;
	}
	
	return false;		//We didn't have a destruct effect.  Tell the object to play its default.
}

//-----------------------------------------------------------------------------
long BldgAppearance::render (long depthFixup)
{
	// GPU-batcher path bypasses inView here — the whole point of C2 is
	// letting the GPU clipper decide visibility. The legacy angular-cull
	// recalcBounds has a ~87% false-negative rate at wolfman zoom; under
	// the GPU path we render every actor and trust the GPU.
	if (inView || g_useGpuStaticProps)
	{
		uint32_t color = SD_BLUE;
		uint32_t highLight = 0x007f7f7f;
		if ((teamId > -1) && (teamId < 8)) {
			static unsigned long highLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
			static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE | 0xff000000, SB_RED| 0xff000000};
			color = colorTable[homeTeamRelationship];
			highLight = highLightTable[homeTeamRelationship];
		}

		if (selected & DRAW_COLORED)
		{
			bldgShape->SetARGBHighLight(highLight);
		}
		else
		{
			bldgShape->SetARGBHighLight(highlightColor);
		}
		
		if (drawFlash)
		{
			bldgShape->SetARGBHighLight(flashColor);
		}
		
		//---------------------------------------------
		// Call Multi-shape render stuff here.
		// Slice 1 path (g_useGpuObjects). No cull bypass; submitMultiShape
		// is per-child Layer-B by construction. Returns false only when
		// EVERY child is ineligible.
		//
		// Caller-side accounting: recordEligibleActor() fires unconditionally
		// when slice 1 reaches this site (so a null shape or skipped submit
		// still counts toward eligible_actors). recordCpuFallback() fires
		// when no submit succeeded.
		bool submittedToGpu = false;
		if (g_useGpuObjects)
		{
			GpuStaticPropBatcher::instance().recordEligibleActor(
				GpuStaticPropPopulation::Building);

			// Stage 3.D: static registry fast path (mirror of TreeAppearance
			// at bdactor.cpp:4123). Set MC2_FORCE_DYNAMIC_BUILDINGS=1 to force
			// fallback to dynamic submitMultiShape for boundary diagnosis.
			// 2026-05-10 diag: per-frame counters to localise buildings-don't-
			// render bug (substrate=ON misses buildings; killswitch shows them).
			static uint64_t s_diag_render_calls = 0;
			static uint64_t s_diag_static_now_true = 0;
			static uint64_t s_diag_lightidx_uintmax = 0;
			static uint64_t s_diag_markVisible = 0;
			static uint64_t s_diag_static_now_false_reg = 0;
			static uint64_t s_diag_static_now_false_eligible = 0;
			static uint64_t s_diag_static_now_false_other = 0;
			static uint64_t s_diag_dyn_submit = 0;
			++s_diag_render_calls;
			const bool isnow = IsStaticNow();
			if (isnow) ++s_diag_static_now_true;
			else {
				if (!staticReg.registered) ++s_diag_static_now_false_reg;
				else if (!isStaticEligible()) ++s_diag_static_now_false_eligible;
				else ++s_diag_static_now_false_other;
			}
			if (isnow) {
				static const bool s_forceDynamicBldgs =
				    (getenv("MC2_FORCE_DYNAMIC_BUILDINGS") != nullptr);
				if (s_forceDynamicBldgs) {
					invalidateStaticRegistration();
					// Fall through to the dynamic path below.
				} else if (bldgShape && bldgShape->getCachedGpuLightIndex() == UINT32_MAX) {
					// Light gather failed this frame — invalidate so dynamic
					// path re-runs and re-registers next frame.
					++s_diag_lightidx_uintmax;
					invalidateStaticRegistration();
				} else {
					// 2026-05-11: pass per-actor captured lightDataIndex so
					// flush() can read it (when MC2_STATIC_PER_INSTANCE_LIGHT=1)
					// instead of multi->getCachedGpuLightIndex() — the per-multi
					// scratch slot is last-writer-wins across sibling instances.
					GpuStaticPropRegistry::markVisible(staticReg.recipeIndex,
					                                  staticReg.lightDataIndex);
					++s_diag_markVisible;
					submittedToGpu = true;
				}
			}
			static const bool s_bldgDiagTrace = (getenv("MC2_BLDG_DIAG_TRACE") != nullptr);
			if (s_bldgDiagTrace && (s_diag_render_calls % 600) == 0) {
				fprintf(stderr,
					"[BLDG_DIAG v1] event=summary calls=%llu staticNow=%llu "
					"notreg=%llu notelig=%llu other=%llu lightidxUM=%llu markVis=%llu dynSubmit=%llu\n",
					(unsigned long long)s_diag_render_calls,
					(unsigned long long)s_diag_static_now_true,
					(unsigned long long)s_diag_static_now_false_reg,
					(unsigned long long)s_diag_static_now_false_eligible,
					(unsigned long long)s_diag_static_now_false_other,
					(unsigned long long)s_diag_lightidx_uintmax,
					(unsigned long long)s_diag_markVisible,
					(unsigned long long)s_diag_dyn_submit);
				fflush(stderr);
			}
			(void)s_diag_dyn_submit;  // updated below if we go to the dyn path

			if (!submittedToGpu && bldgShape)
			{
				// Stage 3.D: shape-swap invalidation. IsStaticNow's
				// staticReg.shape==bldgShape check routed us here when
				// bldgShape was reassigned (LOD swap, damage→bldgDmgShape),
				// but staticReg.registered=true still blocks the registration
				// block below. Invalidate the stale entry first.
				// PERF DIAGNOSTIC 2026-05-07: see TreeAppearance::render for the
				// per-frame churn analysis. Buildings have damage-state swaps
				// (intact → dmg shape) but typically no LOD swap; this rate
				// should be much lower than the tree counterpart.
				if (staticReg.registered && staticReg.shape != bldgShape) {
					invalidateStaticRegistration();
				}

				// Slice 2 (object-offload) — Stage 2.C+: pass appearType->name
				// as callerName so [OBJBATCHER v1] event=late_register can
				// identify which actor class owns the unregistered type.
				const char* callerName = (appearType ? appearType->name : nullptr);
				submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
					bldgShape, GpuStaticPropPopulation::Building, callerName);
				if (submittedToGpu) ++s_diag_dyn_submit;
				// Slice 2 (object-offload) — Stage 2.B: late-registration
				// recovery flag. When submitMultiShape failed because a leaf
				// type was unregistered, mark the actor for full-bake on the
				// NEXT update — defensive hygiene that ensures positions-only
				// is never run on this actor before its type registers.
				if (!submittedToGpu &&
				    GpuStaticPropBatcher::instance().wasLastFailureLateRegistration())
				{
					needsFullBakeNextFrame = true;
					invalidateStaticRegistration();  // clear any stale registration
				}

				// Stage 3.D: registration block. On the first successful full-bake
				// submission with no late-reg flag AND with this instance currently
				// static-eligible, snapshot the leaf batch into the registry.
				// Subsequent frames use the static path above.
				if (submittedToGpu && !staticReg.registered
				        && GpuStaticPropRegistry::isEnabled()
				        && !needsFullBakeNextFrame
				        && isStaticEligible()) {
					const auto& batch =
						GpuStaticPropBatcher::instance().getLastBuiltBatch();
					staticReg.recipeIndex = GpuStaticPropRegistry::registerRecipe(
						bldgShape, batch);
					staticReg.registered  = (staticReg.recipeIndex >= 0);
					staticReg.shape       = bldgShape;
					if (staticReg.registered) {
						// H4 follow-up (2026-05-07): per-frame re-registration
						// after damage/shape swap has the same lightData_ gap as
						// mission-load registerStatic(). Force one full update()
						// so touch() cannot resubmit default-zero lightData_.
						// Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
						needsFullBakeNextFrame = true;
					}
				}
			}
			if (!submittedToGpu)
			{
				GpuStaticPropBatcher::instance().recordCpuFallback(
					GpuStaticPropPopulation::Building);
			}
		}
		// Legacy bypass-cull path (g_useGpuStaticProps). Mutually exclusive
		// with slice 1 — gated on !g_useGpuObjects so the two paths cannot
		// coexist. Tagged Legacy so Gate F's fallback-rate is computed only
		// over slice-1 populations. See spec R1.
		if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && bldgShape)
		{
			submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
				bldgShape, GpuStaticPropPopulation::Legacy);
		}
		if (!submittedToGpu)
		{
			if (appearType->spinMe)
				bldgShape->Render(false,0.00001f);
			else if (!depthFixup)
				bldgShape->Render();
			else if (depthFixup > 0)
				bldgShape->Render(false,0.9999999f);
			else if (depthFixup < 0)
				bldgShape->Render(false,0.00001f);
		}

		// LODBUG probe — post-swap submit observation.  When the actor's
		// bldgShape pointer changed since the last render() call for this
		// actor, log the submit outcome.  Almost all shape-pointer changes
		// are LOD swaps (recalcBounds:1437/1450 reassigns bldgShape via
		// CreateFrom); damage swaps would also trigger but are rare during
		// 30s passive smoke.  Off by default — env-gated.  Tracks state via
		// a static unordered_map keyed on the actor pointer so we don't
		// touch the BldgAppearance class layout.
		{
			static const bool s_lodBugTrace =
				(getenv("MC2_LODBUG_TRACE") != nullptr);
			if (s_lodBugTrace) {
				static std::unordered_map<BldgAppearance*, TG_MultiShape*>
					s_prevShape;
				auto it = s_prevShape.find(this);
				TG_MultiShape* prev =
					(it != s_prevShape.end()) ? it->second : nullptr;
				if (prev && prev != bldgShape) {
					printf("[LODBUG v1] event=post_swap_render actor=%p "
					       "prevShape=%p newShape=%p currentLOD=%u "
					       "inView=%d submittedToGpu=%d\n",
					       (void*)this, (void*)prev, (void*)bldgShape,
					       (unsigned)currentLOD, (int)inView,
					       (int)submittedToGpu);
					fflush(stdout);
				}
				s_prevShape[this] = bldgShape;
			}
		}

		if (selected & DRAW_BARS)
		{
			if (!appearType->spinMe)
			{
				drawBars();

				//drawSelectBrackets(color);
			}
		}

		if ( selected & DRAW_TEXT )
		{
			if (objectNameId != -1)
			{
				char tmpString[255];
				cLoadString(objectNameId, tmpString, 254);

				drawTextHelp(tmpString, color);
			}
		}
		
		//------------------------------------------
		// Render GOS FX if necessary
		if (destructFX && destructFX->IsExecuted())
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;
			
			MidLevelRenderer::MLRState mlrState;
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;

 			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::Point3D			tPosition;
			
			//Stuff::Vector3D offsetPosition;
			//offsetPosition.x = Terrain::worldUnitsPerVertex / 3.0f;
			//offsetPosition.y = -(Terrain::worldUnitsPerVertex / 3.0f);
			//offsetPosition.z = 0.0f;

			//OppRotate(offsetPosition,rotation);

			Stuff::Vector3D actualPosition = position;
			//actualPosition.Add(position,offsetPosition);

			tPosition.x = -actualPosition.x;
			tPosition.y = actualPosition.z;
			tPosition.z = actualPosition.y;

			float yaw = (180.0f + rotation) * DEGREES_TO_RADS;
			Stuff::UnitQuaternion rot;
			rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);
 			shapeOrigin.BuildRotation(rot);
			shapeOrigin.BuildTranslation(tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
			
			if (!MLRVertexLimitReached)
				destructFX->Draw(&drawInfo);
		}
		
		if (isActivitying)
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;

			MidLevelRenderer::MLRState mlrState;
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;

			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;

			if (activityNodeId == -1)
				activityNodeId = bldgShape->GetNodeNameId("activity_node");
			Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeId);

			if (rotationalNodeId == -1)
			{
				if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
	   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
				else
					rotationalNodeId = -2;
			}

			if (rotationalNodeId >= 0)
				dustPos = getNodeIdPosition(rotationalNodeId);
				
			Stuff::Point3D wakePos;
			wakePos.x = -dustPos.x;
			wakePos.y = dustPos.z;
			wakePos.z = dustPos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(wakePos);
					
			/*
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			*/

			drawInfo.m_parentToWorld = &shapeOrigin;
			if (!MLRVertexLimitReached && activity)
				activity->Draw(&drawInfo);
			
			if (activity1)
			{
				if (activityNodeId == -1)
					activityNodeId = bldgShape->GetNodeNameId("activity_node");
				Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeId);
	
				if (rotationalNodeId == -1)
				{
					if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
		   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
					else
						rotationalNodeId = -2;
				}
	
				if (rotationalNodeId >= 0)
					dustPos = getNodeIdPosition(rotationalNodeId);
					
				Stuff::Point3D wakePos;
				wakePos.x = -dustPos.x;
				wakePos.y = dustPos.z;
				wakePos.z = dustPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(wakePos);
						
				/*
				Stuff::UnitQuaternion effectRot;
				effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
				localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
				localResult.Multiply(localToWorld,shapeOrigin);
				*/
	
				drawInfo.m_parentToWorld = &shapeOrigin;
				if (!MLRVertexLimitReached)
					activity1->Draw(&drawInfo);
			}
		}

 			   
//		selected = FALSE;
//#define DRAW_BOX
#ifdef DRAW_BOX
		//---------------------------------------------------------
		// Render the Bounding Box to see if it is OK.
		Stuff::Vector3D nodeCenter = bldgShape->GetRootNodeCenter();
		Stuff::Vector3D boxStart;
		Stuff::Vector3D boxEnd;
		boxStart.x = -(bldgShape->GetMinBox().x + nodeCenter.x);
		boxStart.z = bldgShape->GetMinBox().y + nodeCenter.y;
		boxStart.y = bldgShape->GetMinBox().z + nodeCenter.z;
		
		boxEnd.x = -(bldgShape->GetMaxBox().x + nodeCenter.x);
		boxEnd.z = bldgShape->GetMaxBox().y + nodeCenter.y;
		boxEnd.y = bldgShape->GetMaxBox().z + nodeCenter.z;
		
 		Stuff::Vector3D boxCoords[8];
		Stuff::Vector3D addCoords;
		
		addCoords.x = boxStart.x;
		addCoords.y = boxStart.y;
		addCoords.z = boxEnd.z;
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[0].Add(position,addCoords);

		addCoords.x = boxStart.x;
		addCoords.y = boxEnd.y;  
		addCoords.z = boxEnd.z;  		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[1].Add(position,addCoords);

 		addCoords.x = boxEnd.x; 
		addCoords.y = boxEnd.y; 
		addCoords.z = boxEnd.z; 		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[2].Add(position,addCoords);
		
 		addCoords.x = boxEnd.x;   
		addCoords.y = boxStart.y; 
		addCoords.z = boxEnd.z;   		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[3].Add(position,addCoords);
		
 		addCoords.x = boxStart.x;
		addCoords.y = boxStart.y; 
		addCoords.z = boxStart.z; 		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[4].Add(position,addCoords);
 					  
 		addCoords.x = boxEnd.x;   
		addCoords.y = boxStart.y;   
		addCoords.z = boxStart.z; 
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[5].Add(position,addCoords);
		
 		addCoords.x = boxEnd.x;   
		addCoords.y = boxEnd.y;   
		addCoords.z = boxStart.z; 
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[6].Add(position,addCoords);
		
 		addCoords.x = boxStart.x; 
		addCoords.y = boxEnd.y;   
		addCoords.z = boxStart.z; 
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[7].Add(position,addCoords);

		Stuff::Vector4D screenPos[8];
		for (long i=0;i<8;i++)
		{
			// [PROJECTZ:ScreenXYOracle id=bdactor_box_wire_a]
			eye->projectForScreenXY(boxCoords[i],screenPos[i]);
		}

		{
			LineElement newElement(screenPos[0],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
#endif
#undef DRAW_BOX
	}
	return NO_ERR;
}

//-----------------------------------------------------------------------------
long BldgAppearance::renderShadows (void)
{
	// Skip legacy blob shadows when shadow maps are active
	if (gos_IsTerrainTessellationActive())
		return NO_ERR;

	if (inView && visible && !appearType->spinMe)
	{
		//---------------------------------------------
		// Call Multi-shape render stuff here.
		if (bldgShadowShape)
			bldgShadowShape->RenderShadows();
		else
			bldgShape->RenderShadows();
	}
	return NO_ERR;
}

//-----------------------------------------------------------------------------
// [LIGHTBAKE v1] Static-actor lighting mission-load bake gate. Replaces
// the raw shape->CacheGpuLightData() at the 4 static (bldg/tree) call
// sites. The trailing staticReg.lightDataIndex =
// shape->getCachedGpuLightIndex() per-instance capture is UNCHANGED
// (both CacheGpuLightData and EmitBakedGpuLightData set
// cachedGpuLightIndex_). Key = monotonic-never-reused registry
// recipeIndex; invalidate (destruction/LOD swap) routes through
// invalidateStaticRegistration -> GpuStaticPropRegistry::invalidate ->
// mc2EraseBakedStaticLight -> lazy re-bake of the same position-derived
// constant. Kill-switch MC2_LIGHTBAKE (=0 -> unchanged D2 path
// bit-for-bit). Mechs never reach this (mech3d.cpp calls
// CacheGpuLightData directly); generic props take the no-actor-light
// path. C++-only. See docs/superpowers/plans/
// 2026-05-17-static-lighting-bake-SIMPLIFIED.md
static void mc2CacheOrBakeStaticGpuLight(TG_MultiShape* shape,
                                         bool registered, int32_t recipeIndex)
{
	extern bool mc2LightBakeEnabled();
	extern bool mc2GetBakedStaticLight(int32_t, TG_HWLightsData&);
	extern void mc2SetBakedStaticLight(int32_t, const TG_HWLightsData&);
	extern void mc2WriteStaticLightSlot(int32_t, const TG_HWLightsData&);  // [LIGHTBAKE v2]
	if (!shape) return;
	if (!mc2LightBakeEnabled() || !registered || recipeIndex < 0) {
		shape->CacheGpuLightData();                  // unchanged D2/legacy path
		return;
	}
	TG_HWLightsData baked;
	if (mc2GetBakedStaticLight(recipeIndex, baked)) {
		shape->EmitBakedGpuLightData(recipeIndex, baked);    // HIT: recompute retired
	} else {
		shape->CacheGpuLightData();                          // MISS: real gather (frame 1 / post-invalidate)
		// C1 (adversarial review): CacheGpuLightData early-returns when
		// !g_useGpuObjects && !g_useGpuMechs (supported MC2_GPU_OBJECTS=0
		// operator config), leaving cachedGpuLightIndex_ at the
		// 0xFFFFFFFF sentinel and leaf->lightData_ stale. Only persist
		// the bake if the gather actually ran (valid index) -- else leave
		// uncached so it retries next frame; never persist a no-op
		// snapshot (would poison s_bakedStaticLight until invalidate).
		const TG_HWLightsData* leaf = shape->peekCachedLeafLightData();
		if (leaf && shape->getCachedGpuLightIndex() != 0xFFFFFFFFu) {
			mc2SetBakedStaticLight(recipeIndex, *leaf);      // mission source-of-truth (re-bake/invalidate)
			// [LIGHTBAKE v2] write the PERMANENT static slot once
			// (lightData_[recipeIndex] CPU mirror + advance S), then
			// point this multi at it -- identical end-state to the HIT
			// path, so from this frame on there is NO per-frame
			// addLightDataStructure for this recipe.
			mc2WriteStaticLightSlot(recipeIndex, *leaf);
			shape->EmitBakedGpuLightData(recipeIndex, *leaf);
		}
	}
}

//-----------------------------------------------------------------------------
long BldgAppearance::update (bool animate)
{
	::mc2_object_recon::Scope _recon_bldg_(
		&::mc2_object_recon::g_per_frame.bldg_update_ns,
		&::mc2_object_recon::g_per_frame.bldg_update_calls);
	Stuff::Point3D xlatPosition;
	Stuff::UnitQuaternion rot;

	//----------------------------------------
	// Recycle the weapon Nodes
	if (nodeRecycle)
	{
		for (long i=0;i<appearType->numWeaponNodes;i++)
		{
			if (nodeRecycle[i] > 0.0f)
			{
				nodeRecycle[i] -= frameLength;
				if (nodeRecycle[i] < 0.0f)
					nodeRecycle[i] = 0.0f;
			}
		}
	}

   	if (appearType->terrainLightRGB != 0xffffffff && (eye->nightFactor > 0.0f) && !forceLightsOut)
   	{
   		if (!pointLight)
   		{
   			pointLight = (TG_LightPtr)malloc(sizeof(TG_Light));
   			pointLight->init(TG_LIGHT_TERRAIN);
   			lightId = eye->addWorldLight(pointLight);
   	
   			pointLight->SetaRGB(appearType->terrainLightRGB);
   			pointLight->SetIntensity(appearType->terrainLightIntensity);
   			pointLight->SetFalloffDistances(appearType->terrainLightInnerRadius, appearType->terrainLightOuterRadius);
   		}
		
		if (pointLight)
		{
			Stuff::Point3D ourPosition;
			ourPosition.x = -position.x;
			ourPosition.y = position.z;
			ourPosition.z = position.y;
	
			pointLight->direction = ourPosition;
	
			Stuff::LinearMatrix4D lightToWorldMatrix;
			lightToWorldMatrix.BuildTranslation(ourPosition);
			lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			pointLight->SetLightToWorld(&lightToWorldMatrix);
			pointLight->SetPosition(&position);
			pointLight->SetIntensity(appearType->terrainLightIntensity * eye->getNightFactor());
		}
   	}
	else
	{
		//Turn the lights off!
		//Need to kill the light source here too!
		if (pointLight)
		{
			eye->removeWorldLight(lightId,pointLight);
			free(pointLight);
			pointLight = NULL;
		}
	}

	if (forceLightsOut)
		bldgShape->SetLightsOut(true);
		
	//Update flashing regardless of view!!!
	if (duration > 0.0f)
	{
		duration -= frameLength;
		currentFlash -= frameLength;
		if (currentFlash < 0.0f)
		{
			drawFlash ^= true;
			currentFlash = flashDuration;
		}
	}
	else
	{
		drawFlash = false;
	}

	// Under the GPU static-prop path, compute xlatPosition/rot + fog/light
	// for every building so the later TransformMultiShape (also gated on
	// g_useGpuStaticProps) has valid inputs.
	if (inView || g_useGpuStaticProps)
	{
		if (appearType->spinMe)
			rotation += SPINRATE * frameLength;

 		if (rotation > 180)
			rotation -= 360;

		if (rotation < -180)
			rotation += 360;

		//-------------------------------------------
		// Does math necessary to draw Tree
		float yaw = rotation * DEGREES_TO_RADS;
		rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);
	
		if (appearType->spinMe && land)
		{
			//Make sure we are above the water level
			if (position.z < Terrain::waterElevation)
				position.z = Terrain::waterElevation;
		}

		xlatPosition.x = -position.x;
		xlatPosition.y = position.z;
		xlatPosition.z = position.y;

		if (!fogLightSet)
		{
			unsigned char lightr,lightg,lightb;
			float lightIntensity = 1.0f;
			if (land)
				lightIntensity = land->getTerrainLight(position);

			lightr = eye->getLightRed(lightIntensity);
			lightg = eye->getLightGreen(lightIntensity);
			lightb = eye->getLightBlue(lightIntensity);

			lightRGB = (lightr<<16) + (lightg<<8) + lightb;

			fogRGB = 0xff<<24;
			float fogStart = eye->fogStart;
			float fogFull = eye->fogFull;

			if (xlatPosition.y < fogStart)
			{
				float fogFactor = fogStart - xlatPosition.y;
				if (fogFactor < 0.0)
					fogRGB = 0xff<<24;
				else
				{
					fogFactor /= (fogStart - fogFull);
					if (fogFactor <= 1.0)
					{
						fogFactor *= fogFactor;
						fogFactor = 1.0 - fogFactor;
						fogFactor *= 256.0;
					}
					else
					{
						fogFactor = 256.0;
					}

					unsigned char fogResult = float2long(fogFactor);
					fogRGB = fogResult << 24;
				}
			}
			else
			{
				fogRGB = 0xff<<24;
			}

			fogLightSet = true;
		}
	
		eye->setLightColor(0,lightRGB);
		eye->setLightIntensity(0,1.0);

		if (useFog)
			bldgShape->SetFogRGB(fogRGB);
		else
			bldgShape->SetFogRGB(0xffffffff);
	
		Stuff::UnitQuaternion turretRot;
		turretRot = Stuff::EulerAngles((turretPitch * DEGREES_TO_RADS),(turretYaw * DEGREES_TO_RADS),0.0f);
		if (rotationalNodeId == -1)
	   		rotationalNodeId = bldgShape->SetNodeRotation(appearType->rotationalNodeId,&turretRot);
   
	   	bldgShape->SetNodeRotation(rotationalNodeId,&turretRot);
	}

	float oldFrame = currentFrame;
	if (animate && bdFrameRate != 0.0f)
	{
		//--------------------------------------------------------
		// Make sure animation runs no faster than bdFrameRate fps.
		float frameInc = bdFrameRate * frameLength;
		
		//---------------------------------------
		// Increment Frames -- Everything else!
		if (frameInc != 0.0f)
		{
			if (!setFirstFrame)		//DO NOT ANIMATE ON FIRST FRAME!  Wait a bit!
			{
				if (isReversed)
					currentFrame -= frameInc;
				else
					currentFrame += frameInc;
			}
			else
			{
				setFirstFrame = false;
			}
	
			//--------------------------------------
			//Check Positive overflow of Animation
			if (currentFrame >= appearType->getNumFrames(bdAnimationState))
			{
				if (isLooping)
					currentFrame -= appearType->getNumFrames(bdAnimationState);
				else
					currentFrame = appearType->getNumFrames(bdAnimationState) - 1;
					
				canTransition = true;		//Whenever we have completed one cycle or at last frame, OK to move on!
			}
			
	
			//--------------------------------------
			//Check negative overflow of gesture
			if (currentFrame < 0)
			{
				if (isLooping)
					currentFrame += appearType->getNumFrames(bdAnimationState); 
				else
					currentFrame = 0.0f; 
					
				canTransition = true;		//Whenever we have completed one cycle or at last frame, OK to move on!
			}
		}
		
		bldgShape->SetFrameNum(currentFrame);
	}

	// Under the GPU static-prop path we need listOfColors / shapeToWorld
	// fresh every frame regardless of inView so the batcher can safely
	// memcpy from shape->listOfColors during submit().
	if (inView || g_useGpuStaticProps)
	{
		bool checkShadows = ((!beenInView) || (appearType->spinMe) || (eye->forceShadowRecalc) || (currentFrame != oldFrame));
		if (bldgShadowShape)
			bldgShape->SetUseShadow(false);
		else
			bldgShape->SetRecalcShadows(checkShadows);

		bldgShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice 2 (object-offload) — Stage 2.B: eligibility hoist.
		// Branch lives INSIDE the existing inView||g_useGpuStaticProps cull
		// gate to preserve slice 1's R1 invariant (no cull bypass).
		// Run positions-only when:
		//   - g_useGpuObjects is on, AND
		//   - this actor did not hit a late-registration recovery last frame
		//     (needsFullBakeNextFrame is a NEW bool from Stage 2.A; set by
		//     BldgAppearance::render when submitMultiShape returns false with
		//     wasLastFailureLateRegistration() true), AND
		//   - the multi-shape's leaves are all registered with the slice 1
		//     batcher (isMultiShapeEligibleForGpuObjects mirrors slice 1's
		//     render-time per-child gates EXCEPT late-reg, which is handled
		//     via the recovery flag above).
		// Otherwise full bake. The full bake clears the recovery flag —
		// re-establishing valid .argb before render reads it.
		// PERF DIAGNOSTIC 2026-05-06: Tracy zones to attribute the 9.52 µs/call
		// observed in TerrainObject::update appearanceUpdate. Six theories under
		// investigation; these zones discriminate between them. Demote to silent
		// (or remove) once the regression is identified.
		bool gpuEligible;
		{
			gpuEligible = g_useGpuObjects &&
			              !needsFullBakeNextFrame &&
			              GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(bldgShape);
		}

		if (gpuEligible)
		{
			// Stage 2.D.2 fix: cache GPU light data NOW, while worldLights[0]->aRGB
			// is the per-actor terrain-scaled value set at line 2144 above.
			// By the time submitMultiShape() runs (during renderLists()), later
			// actors have overwritten worldLights[0]->aRGB for their positions.
			{
				mc2CacheOrBakeStaticGpuLight(bldgShape, staticReg.registered, staticReg.recipeIndex);
				// 2026-05-11 per-instance capture: snapshot the multi's just-written
				// cache slot for THIS actor before sibling actors of the same
				// multi-type overwrite it. Ferried to RecipeRange via markVisible().
				staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
			}
			{
				bldgShape->TransformMultiShape_PositionsOnly (&xlatPosition,&rot);
			}
			// Stage 2.D.2: on the dual-emit frame (latch Armed), also run
			// the full bake so listOfTriangles[].aRGBLight is populated for
			// the parity snapshot captured in submit(). This call is a pure
			// CPU-side data write; it does NOT affect GPU output (the shader
			// uses a_aRGBLight from the type-level VBO, not listOfVertices.argb).
			// No addRenderShape: GPU-eligible actors reach this branch only
			// when g_useGpuObjects is true. The legacy Render() path (which
			// calls addTriangle) is bypassed because submitMultiShape()
			// handles the GPU draw instead. In Renderer 3 (GL 4.3), the
			// addTriangle queue is never flushed to hardware — only the GPU
			// batcher's direct draw is visible. So calling TransformMultiShape
			// here (for the parity snapshot) does NOT result in double-draw.
			// Stage 2.D.3: per-actor gate. Bootstrap arm returns true for
			// every shape; sample arm returns true only for the picked actor.
			if (gos_object_parity::IsDualEmitArmedForActor(bldgShape)) {
				bldgShape->TransformMultiShape (&xlatPosition,&rot);
			}
		}
		else
		{
			bldgShape->TransformMultiShape (&xlatPosition,&rot);
			// 2026-05-10: also seed cachedGpuLightIndex_ in the full-bake
			// branch. Without this, the first-frame transition out of the
			// H4 latch (set by registerStatic at :2754) leaves the index
			// at UINT32_MAX, and the static-path render gate at :1612
			// (`getCachedGpuLightIndex() == UINT32_MAX → invalidate`)
			// invalidates the registration on the very next render —
			// markVisible() never fires, registry::flush() short-circuits,
			// substrate gets no static-prop records, and the cull writes
			// 0 to all bucketCountData. The fix mirrors the gpuEligible
			// branch's CacheGpuLightData call at :2314 so any path
			// through update() seeds the light index. Cheap: same call
			// already runs unconditionally in submitMultiShape; here we
			// just hoist its effect to be visible to render() this frame.
			mc2CacheOrBakeStaticGpuLight(bldgShape, staticReg.registered, staticReg.recipeIndex);
			// 2026-05-11 per-instance capture: see gpuEligible branch above.
			staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
			needsFullBakeNextFrame = false;
		}

		// Skip the legacy-blob-shadow per-frame transform when shadow maps are
		// active. BldgAppearance::renderShadows() at line 2010 already early-
		// returns under the same condition (gos_IsTerrainTessellationActive),
		// meaning bldgShadowShape's transformed state is never consumed in this
		// pipeline. Per-actor saving = 1 TransformMultiShape call per visible
		// building per frame (~3 µs of pure waste).
		if (bldgShadowShape && useShadows && !gos_IsTerrainTessellationActive())
		{
			bldgShadowShape->SetRecalcShadows(checkShadows);
			bldgShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			bldgShadowShape->TransformMultiShape (&xlatPosition,&rot);
		}
 		
		if ((turn > 3) && useShadows)
			beenInView = true;
			
		//------------------------------------------------
		// Update GOSFX
		if (destructFX && destructFX->IsExecuted())
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D 	localToWorld;
			Stuff::Point3D			tPosition;
				
			//Stuff::Vector3D offsetPosition;
			//offsetPosition.x = Terrain::worldUnitsPerVertex / 3.0f;
			//offsetPosition.y = -(Terrain::worldUnitsPerVertex / 3.0f);
			//offsetPosition.z = 0.0f;

			//OppRotate(offsetPosition,rotation);

			Stuff::Vector3D actualPosition = position;
			//actualPosition.Add(position,offsetPosition);

			tPosition.x = -actualPosition.x;
			tPosition.y = actualPosition.z;
			tPosition.z = actualPosition.y;

			float yaw = (180.0f + rotation) * DEGREES_TO_RADS;
			Stuff::UnitQuaternion rot;
			rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);
 			shapeOrigin.BuildRotation(rot);
			shapeOrigin.BuildTranslation(tPosition);

	 		Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = destructFX->Execute(&info);
			if (!result)
			{
				destructFX->Kill();
				delete destructFX;
				destructFX = NULL;
			}
		}
		
		if (isActivitying)
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
					
			if (activityNodeId == -1)
				activityNodeId = bldgShape->GetNodeNameId("activity_node");
			Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeId);

			if (rotationalNodeId == -1)
			{
				if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
	   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
				else
					rotationalNodeId = -2;
			}

			if (rotationalNodeId >= 0)
				dustPos = getNodeIdPosition(rotationalNodeId);
 			
			Stuff::Point3D wakePos;
			wakePos.x = -dustPos.x;
			wakePos.y = dustPos.z;
			wakePos.z = dustPos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(wakePos);
					
			/*
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			*/
			
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
			
			// sebi: make as all other do with Execute(), otherwise ther is constanrt assert in Execute() function (at line Verify(IsExecuted()) )
			if (activity && activity->IsExecuted())
			{
				bool result = activity->Execute(&info);
				if (!result)
				{
					activity->Kill();		//Effect is over.  Otherwise, wait until hit!
					delete activity;
					activity = NULL;
				}

			}
			
			if (activity1)
			{
				if (activityNodeId == -1)
					activityNodeId = bldgShape->GetNodeNameId("activity_node");
				Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeId);
	
				if (rotationalNodeId == -1)
				{
					if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
		   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
					else
						rotationalNodeId = -2;
				}
	
				if (rotationalNodeId >= 0)
					dustPos = getNodeIdPosition(rotationalNodeId);
				
				Stuff::Point3D wakePos;
				wakePos.x = -dustPos.x;
				wakePos.y = dustPos.z;
				wakePos.z = dustPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(wakePos);
						
				/*
				Stuff::UnitQuaternion effectRot;
				effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
				localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
				localResult.Multiply(localToWorld,shapeOrigin);
				*/
				
				Stuff::OBB boundingBox;
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
				
				//sebi:
				if (activity1 && activity1->IsExecuted())
				{
					bool result = activity1->Execute(&info);
					if (!result)
					{
						activity1->Kill();		//Effect is over.  Otherwise, wait until hit!
						delete activity1;
						activity1 = NULL;
					}
				}
				//

			}
		}
	}
	
	return TRUE;
}

//-----------------------------------------------------------------------------
void BldgAppearance::startActivity (long effectId, bool loop)
{
	//Check if we are already playing one.  If not, be active!
	
	//First, check if its even loaded.
	// can easily preload this.  Should we?  NO.  We don't know what will be passed in.
	if (!activity && useNonWeaponEffects)
	{
   		if (strcmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;
			if (!loop)
				flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));
			
			if (gosEffectSpec)
			{
				activity = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(activity != NULL);
				
				Stuff::Vector3D testPos = getNodeNamePosition("activity_node1");
				if (testPos != position)
				{
					activity1 = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
					gosASSERT(activity1 != NULL);
				}

  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (!isActivitying && activity)		//Start the effect if we are not running it yet!!
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
		
		if (activityNodeId == -1)
   			activityNodeId = bldgShape->GetNodeNameId("activity_node");
   		Stuff::Vector3D nodePos = getNodeIdPosition(activityNodeId);

   		if (rotationalNodeId == -1)
   		{
   			if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
      				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
   			else
   				rotationalNodeId = -2;
   		}

   		if (rotationalNodeId >= 0)
   			nodePos = getNodeIdPosition(rotationalNodeId);

 		Stuff::Point3D wakePos;
		wakePos.x = -nodePos.x;
		wakePos.y = nodePos.z;	//Wake is at Water Level!
		wakePos.z = nodePos.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		/*
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		*/
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);

		activity->Start(&info);
		
		if (activity1)
		{
			if (activityNode1Id == -1)
				activityNode1Id = bldgShape->GetNodeNameId("activity_node1");
			Stuff::Vector3D nodePos = getNodeIdPosition(activityNode1Id);

			if (rotationalNodeId == -1)
			{
				if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
	   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
				else
					rotationalNodeId = -2;
			}

			if (rotationalNodeId >= 0)
				nodePos = getNodeIdPosition(rotationalNodeId);
			
 			Stuff::Point3D wakePos;
			wakePos.x = -nodePos.x;
			wakePos.y = nodePos.z;	//Wake is at Water Level!
			wakePos.z = nodePos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(wakePos);
					
			/*
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			*/
				
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
	
			activity1->Start(&info);
		}
		
		isActivitying = true;
	}
}

//-----------------------------------------------------------------------------
void BldgAppearance::stopActivity (void)
{
	if (isActivitying)		//Stop the effect if we are running it!!
	{
		if(activity) //sebi
			activity->Kill();
		if (activity1)
			activity1->Kill();
	}
	
	isActivitying = false;
}

//-----------------------------------------------------------------------------
void BldgAppearance::flashBuilding (float dur, float fDuration, DWORD color)
{
	duration = dur;
	flashDuration = fDuration;
	flashColor = color;
	drawFlash = true;
	currentFlash = flashDuration;
}

//-----------------------------------------------------------------------------
// Stage 3.D: BldgAppearance static-registry path. Mirror of TreeAppearance's
// IsStaticNow / touch / invalidateStaticRegistration / isStaticEligible.
// Registry-replay eligibility for buildings is stricter than for trees because
// buildings have many dynamic states (animation, spin, flash, destruct FX).
// memory/bldg_animation_lod_swap_unsafe.md is the load-bearing reason: animated
// types share LOD-0 node-index state across instances, so replaying a recipe
// for an animated building drives the wrong node when LOD swaps.

namespace {
	// Type-level "any animation defined for this building type". Even if the
	// current instance isn't actively animating, an animated TYPE is excluded
	// from the static path because LOD swap could surface the animation later
	// and break the cached recipe.
	bool bldgTypeHasAnimations(const BldgAppearanceType* t) {
		if (!t) return false;
		for (long i = 0; i < MAX_BD_ANIMATIONS; ++i) {
			if (t->bdAnimData[i] != nullptr) return true;
		}
		return false;
	}
} // anon namespace

bool BldgAppearance::isStaticEligible() const
{
	// Type-level disqualifiers: this building TYPE is dynamic by design.
	if (!appearType)                          return false;
	if (appearType->spinMe)                   return false;
	if (bldgTypeHasAnimations(appearType))    return false;
	// Instance-level disqualifiers: this PARTICULAR building is currently
	// mutating in a way the cached recipe can't reflect.
	if (drawFlash)                            return false;
	if (destructFX)                           return false;
	if (activity)                             return false;
	if (activity1)                            return false;
	if (bdAnimationState != -1)               return false;  // currently animating
	return true;
}

// Task 5 (Track B): mission-load bulk static-prop registration.
// Called from GameObjectManager::registerStaticPropsForMissionLoad() after
// primeTerrainObjectsForMissionLoad() has set position/rotation on every
// actor. Populates shapeToWorld matrices via TransformMultiShape_PositionsOnly,
// builds a recipe batch per leaf via buildRecipeFromShape, and registers with
// GpuStaticPropRegistry. HC-1: writes directly to typed staticReg member.
void BldgAppearance::registerStatic() {
	if (staticReg.registered) return;
	if (!bldgShape)           return;
	if (!GpuStaticPropRegistry::isEnabled()) return;
	if (!isStaticEligible())  return;

	// Compute transform — same coordinate convention as BldgAppearance::update().
	// At mission-load time position.z may not yet hold terrain elevation (set by
	// bldng.cpp:810 on first update), so use getTerrainElevation() directly.
	float yaw = rotation * DEGREES_TO_RADS;
	Stuff::UnitQuaternion rot;
	rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);
	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y = land ? land->getTerrainElevation(position) : 0.0f;
	xlatPosition.z = position.y;
	bldgShape->TransformMultiShape_BuildRecipe(&xlatPosition, &rot);

	// Build per-leaf recipe batch.
	// Use public GetNumShapes()/GetShapeRec() — numTG_Shapes/listOfShapes are protected.
	std::vector<GpuStaticPropInstance> batch;
	const int numShapes = static_cast<int>(bldgShape->GetNumShapes());
	batch.reserve(numShapes);
	// 2026-05-10 diag: count outcomes for buildings to see why so few reach count>1.
	int diag_total = 0, diag_skip_processMe = 0, diag_skip_helper = 0,
	    diag_skip_unreg = 0, diag_added = 0;
	for (int i = 0; i < numShapes; ++i) {
		++diag_total;
		const TG_ShapeRec* rec = bldgShape->GetShapeRec(i);
		if (!rec || !rec->processMe || !rec->node) { ++diag_skip_processMe; continue; }
		TG_Shape* child = rec->node;
		// 2026-05-10 fix: skip non-SHAPE_NODE children (helpers, spotlight
		// emitters, animation roots) — mirrors GpuStaticPropBatcher::submitMultiShape
		// at gos_static_prop_batcher.cpp:2047. Without this, the loop hits a
		// helper, buildRecipeFromShape's static_cast<TG_TypeShape*>(myType)
		// produces a pointer that isn't in s_typeIndex, returns false, and
		// the entire building registration aborts on its FIRST helper. This
		// is why mc2_10 buildings (warehouses, S_admin, control) previously
		// failed registerStatic and never reached the substrate.
		if (!child->IsShapeNode()) { ++diag_skip_helper; continue; }
		uint32_t flags = 0;
		if (child->GetLightsOut())   flags |= (1u << 0);
		if (child->GetIsWindow())    flags |= (1u << 1);
		if (child->GetIsSpotlight()) flags |= (1u << 2);
		// rec->shapeToWorld is LinearMatrix4D; convert to Matrix4D for buildRecipeFromShape().
		Stuff::Matrix4D xform(rec->shapeToWorld);
		GpuStaticPropInstance inst;
		if (!GpuStaticPropBatcher::instance().buildRecipeFromShape(
				child, xform,
				static_cast<uint32_t>(child->GetARGBHighlight()),
				static_cast<uint32_t>(child->GetFogRGB()),
				flags, &inst)) {
			++diag_skip_unreg;
			return;  // unregistered type — abort; first-render fallback covers it
		}
		batch.push_back(inst);
		++diag_added;
	}
	// 2026-05-10 diag: env-gated per-building outcome. MC2_BLDG_REG_TRACE=1.
	{
		static const bool s_trace = (getenv("MC2_BLDG_REG_TRACE") != nullptr);
		static int s_loggedCount = 0;
		if (s_trace && s_loggedCount < 80) {
			++s_loggedCount;
			fprintf(stderr,
				"[BLDG_REG_DIAG v1] appearType=%s numShapes=%d total=%d processMe_skip=%d "
				"helper_skip=%d unreg_skip=%d added=%d\n",
				(appearType ? appearType->name : "<null>"),
				numShapes, diag_total, diag_skip_processMe, diag_skip_helper,
				diag_skip_unreg, diag_added);
			fflush(stderr);
		}
		(void)diag_total; (void)diag_skip_processMe; (void)diag_skip_helper;
		(void)diag_skip_unreg; (void)diag_added;
	}
	if (batch.empty()) return;

	const int32_t regIdx = GpuStaticPropRegistry::registerRecipe(bldgShape, batch);
	if (regIdx >= 0) {
		staticReg.registered  = true;
		staticReg.shape       = bldgShape;
		staticReg.recipeIndex = regIdx;
		// H4 fix (2026-05-06): registerStatic only ran TransformMultiShape_BuildRecipe
		// (positions only); leaf TG_Shape::lightData_ is still default/zero. Without
		// this flag, IsStaticNow() returns true on the very next frame, UPDATE_SKIP
		// fires, touch() re-submits the empty lightData_ via addLightDataStructure
		// → all-zero lighting slot → black actor. Setting needsFullBakeNextFrame
		// uses the existing late-reg recovery mechanism: IsStaticNow() returns
		// false until the next update() runs a full TransformMultiShape and clears
		// the flag (bdactor.cpp:2313). One-time cost of one extra update() per
		// mission-load-registered actor; recovers the UPDATE_SKIP perf win
		// every frame thereafter. Spec:
		// docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md
		needsFullBakeNextFrame = true;
	}
}

bool BldgAppearance::isStaticRegistered() const { return staticReg.registered; }

bool BldgAppearance::IsStaticNow() const
{
	return staticReg.registered
		&& staticReg.shape == bldgShape
		&& !needsFullBakeNextFrame
		&& isStaticEligible();
}

void BldgAppearance::touch()
{
	// MC2_STATIC_UPDATE_SKIP defaults TRUE (terrobj.cpp:92); touch() is the
	// DEFAULT path. update() runs only when the env var is explicitly cleared.
	if (bldgShape) {
		// [LIGHTBRIDGE v1] C6 retirement: repoint to the primed 38d8720 slot
		// (zero FNV/memcmp; cachedFrame_ stamped). MISS keeps the legacy
		// resubmit (NOT CacheGpuLightData -- terrain-color-staleness,
		// msl.cpp:1874-1887). MC2_LIGHTBAKE=0 -> legacy path bit-for-bit.
		extern bool mc2LightBakeEnabled();
		extern bool mc2GetBakedStaticLight(int32_t, TG_HWLightsData&);
		TG_HWLightsData baked;
		if (mc2LightBakeEnabled()
		    && staticReg.registered && staticReg.recipeIndex >= 0
		    && mc2GetBakedStaticLight(staticReg.recipeIndex, baked)) {
			bldgShape->EmitBakedGpuLightData(staticReg.recipeIndex, baked);
		} else {
			bldgShape->ResubmitCachedGpuLightData();
		}
		// 2026-05-11 per-instance capture: snapshot the just-resubmitted slot
		// for THIS actor before sibling instances of the same multi-type
		// overwrite multi->cachedGpuLightIndex_ in the same update phase.
		staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
		bldgShape->Touch();
	}
}

void BldgAppearance::invalidateStaticRegistration()
{
	if (staticReg.registered && staticReg.recipeIndex >= 0)
		GpuStaticPropRegistry::invalidate(staticReg.recipeIndex);
	staticReg = {};
}

//-----------------------------------------------------------------------------
void BldgAppearance::destroy (void)
{
	// Stage 3.D: NULL the registry's RecipeRange::multi pointer before
	// bldgShape is freed below. Mirrors TreeAppearance::destroy ordering.
	invalidateStaticRegistration();

	if ( bldgShape )
	{
		delete bldgShape;
		bldgShape = NULL;
	}

	if (bldgShadowShape)
	{
		delete bldgShadowShape;
		bldgShadowShape = NULL;
	}

	if (destructFX)
	{
		destructFX->Kill();
		delete destructFX;
		destructFX = NULL;
	}
	
	//Turn the lights off!
	//Need to kill the light source here too!
	if (pointLight)
	{
		if (eye)
			eye->removeWorldLight(lightId,pointLight);

		free(pointLight);
		pointLight = NULL;
	}

	if (activity)
	{
		activity->Kill();
		delete activity;
		activity = NULL;
	}

	if (activity1)
	{
		activity1->Kill();
		delete activity1;
		activity1 = NULL;
	}

	appearanceTypeList->removeAppearance(appearType);
}

#define HEIGHT_THRESHOLD 10.0f

//-----------------------------------------------------------------------------

long BldgAppearance::calcCellsCovered (Stuff::Vector3D& pos, short* cellList) {

	gosASSERT((Terrain::realVerticesMapSide * MAPCELL_DIM) == GameMap->width);
	long numCoords = 0;
	long maxCoords = cellList[0];

	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		bldgShape->ClearAnimation();
		delete bldgShape;
		bldgShape = NULL;
	
		bldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
		if (bdAnimationState != -1)
			appearType->setAnimation(bldgShape,bdAnimationState);
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	for (int i=0;i<bldgShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block passability!!
		if (S_strnicmp(bldgShape->GetNodeId(i),"_PAB",4) != 0)
		{
			for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++) 
			{
				Stuff::Vector3D vertexPos, worldPos;
				vertexPos = bldgShape->GetShapeVertexInEditor(i,j,-rotation);
				worldPos.Add(pos,vertexPos);
	
				bool recordCell = false;
				if (appearType->isForestClump)
					recordCell = (vertexPos.z <= 1.0f);
				else
					recordCell = (vertexPos.z >= 1.0f);
				if (recordCell) 
				{
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR)
						|| (0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						//gosASSERT(false);
						continue;
					}
	//				if (GameMap->inBounds(cellR, cellC)) {
						//-------------------
						// Record the cell...
						if (numCoords > (maxCoords - 2))
							Fatal(numCoords, "BldgAppearance.markMoveMap: too many coords for cellList ");
							
						cellList[numCoords++] = (short)cellR;
						cellList[numCoords++] = (short)cellC;
	//				}
				}
			}
		}
	}
	
	return(numCoords);
}

//-----------------------------------------------------------------------------

void BldgAppearance::markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
{
	if (appearType->spinMe)			//We are a marker
		return;						//Do not mark impassable
		
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		bldgShape->ClearAnimation();
		delete bldgShape;
		bldgShape = NULL;
	
		bldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
		if (bdAnimationState != -1)
			appearType->setAnimation(bldgShape,bdAnimationState);
	}

	int cellR, cellC;
	land->worldToCell(position, cellR, cellC);
	if (appearType->isForestClump)
	{
		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i=0;i<bldgShape->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(bldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++)
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = bldgShape->GetShapeVertexInEditor(i,j,-rotation);
					worldPos.Add(position,vertexPos);
		
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					
					_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);
	
					if (vertexPos.z <= 1.0f)
					{
						pTmp->passable = true;
						pTmp->gate = false;
						pTmp->forest = true;
						//pTmp->specialType = type;
						//pTmp->specialID = counter;
					}
					
					float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
					if (cellLocalHeight > 15.0f)
						cellLocalHeight = 15.0f;
						
					//ONLY mark LOS on cells that are impassable with forests.  Maybe everything?
					if (pTmp->passable && (pTmp->lineOfSight < cellLocalHeight))
						pTmp->lineOfSight = cellLocalHeight+0.5f;
				}
			}
		}
	}
	else
	{
		if ((type == SPECIAL_GATE) || (type == SPECIAL_WALL))
		{
			if (appearType->bldgShape[0])
			{
				bldgShape->ClearAnimation();
				delete bldgShape;
				bldgShape = NULL;
					
				bldgShape = appearType->bldgShape[0]->CreateFrom();
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
			}
		}

		if (type == SPECIAL_LAND_BRIDGE)
		{
			if (appearType->bldgDmgShape)
			{
				bldgShape->ClearAnimation();
				delete bldgShape;
				bldgShape = NULL;
					
				bldgShape = appearType->bldgDmgShape->CreateFrom();
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
			}
		}

		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i=0;i<bldgShape->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(bldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++)
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = bldgShape->GetShapeVertexInEditor(i,j,-rotation);
					worldPos.Add(position,vertexPos);
		
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);
	
					if (vertexPos.z >= 1.0f)
					{
						pTmp->passable = false;
						
						if (((type == SPECIAL_GATE) || (type == SPECIAL_WALL)))
						{
							pTmp->passable = true;
							pTmp->specialID = counter;
							pTmp->specialType = type;
							if (type == SPECIAL_GATE)
								pTmp->gate = true;
						}
						else if (type == SPECIAL_LAND_BRIDGE)
						{
							pTmp->passable = true;
							pTmp->specialID = counter;
							pTmp->specialType = type;
						}
						else if (type == 18)
						{
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
							pTmp->passable = true;
						}
						else
						{
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
						}
							
						if (type != 18)
						{
							float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
							if (cellLocalHeight > 15.0f)
								cellLocalHeight = 15.0f;
								
							if (pTmp->lineOfSight < cellLocalHeight)
								pTmp->lineOfSight = cellLocalHeight+0.5f;
						}
					}
				}
			}
		}
		
 		//Switch to destroyed state to mark impassable.  The destroyed impassability will NEVER change!!
		// When a gate opens or a wall or gate is destroyed, we only want to mark stuff that is going 
		// away passable and long range capable.
		if ((type == SPECIAL_GATE) || (type == SPECIAL_WALL))
		{
			if (appearType->bldgDmgShape)
			{
				bldgShape->ClearAnimation();
				delete bldgShape;
				bldgShape = NULL;
					
				bldgShape = appearType->bldgDmgShape->CreateFrom();
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
			}
		}

		if (type == SPECIAL_LAND_BRIDGE)
		{
			if (appearType->bldgShape[0])
			{
				bldgShape->ClearAnimation();
				delete bldgShape;
				bldgShape = NULL;
					
				bldgShape = appearType->bldgShape[0]->CreateFrom();
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
			}
		}

			
		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i=0;i<bldgShape->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(bldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++)
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = bldgShape->GetShapeVertexInEditor(i,j,-rotation);
					worldPos.Add(position,vertexPos);
		
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);
	
					if (vertexPos.z >= 1.0f)
					{
						if (type == 18)
						{
							pTmp->passable = true;
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
						}
						else
						{
							pTmp->passable = false;
							pTmp->gate = false;			//Perfectly OK to mark these again,  They are no longer special!!
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
						}
					}
				}
			}
		}
			
		if ((status != OBJECT_STATUS_DESTROYED) && appearType->bldgShape[0])
		{
			bldgShape->ClearAnimation();
			delete bldgShape;
			bldgShape = NULL;
						
			bldgShape = appearType->bldgShape[0]->CreateFrom();
			if (bdAnimationState != -1)
				appearType->setAnimation(bldgShape,bdAnimationState);
		}
		else if ((status == OBJECT_STATUS_DESTROYED) && appearType->bldgDmgShape)
		{
			bldgShape->ClearAnimation();
			delete bldgShape;
			bldgShape = NULL;
					
			bldgShape = appearType->bldgDmgShape->CreateFrom();
			if (bdAnimationState != -1)
				appearType->setAnimation(bldgShape,bdAnimationState);
		}
	}
}

//-----------------------------------------------------------------------------

long BldgAppearance::markMoveMap (bool passable, long* lineOfSightRect, bool useHeight, short* cellList)
{
	int minRow = 9999;
	int maxRow = 0;
	int minCol = 9999;
	int maxCol = 0;

	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	TG_MultiShapePtr tempBldgShape = bldgShape;

	if (currentLOD)
	{
		tempBldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
		if (bdAnimationState != -1)
			appearType->setAnimation(tempBldgShape,bdAnimationState);
	}

	int numCoords = 0;
	if (cellList) {
		gosASSERT(!useHeight);
		//----------------------------------------------------------------------------------
		// Store the max number of coords allowed in the first cell. Can overwrite it now...
		int maxCoords = cellList[0];
		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i = 0; i < tempBldgShape->GetNumShapes(); i++) 
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(tempBldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<tempBldgShape->GetNumVerticesInShape(i);j++) 
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = tempBldgShape->GetShapeVertexInWorld(i,j,-rotation);
					worldPos.Add(position,vertexPos);
	
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					
					//----------------------------
					// Building lineOfSightRect...
					if (cellR < minRow)
						minRow = cellR;
					if (cellR > maxRow)
						maxRow = cellR;
					if (cellC < minCol)
						minCol = cellC;
					if (cellC > maxCol)
						maxCol = cellC;
						
					//-------------------
					// Record the cell...
					if (numCoords > (maxCoords - 2))
						Fatal(numCoords, "BldgAppearance.markMoveMap: too many coords for cellList ");
					cellList[numCoords++] = (short)cellR;
					cellList[numCoords++] = (short)cellC;
				}
			}
		}
	}
	else 
	{
		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i=0;i<tempBldgShape->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(tempBldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<tempBldgShape->GetNumVerticesInShape(i);j++)
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = tempBldgShape->GetShapeVertexInWorld(i,j,-rotation);
					worldPos.Add(position,vertexPos);
	
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					
					//----------------------------
					// Building lineOfSightRect...
					if (cellR < minRow)
						minRow = cellR;
					if (cellR > maxRow)
						maxRow = cellR;
					if (cellC < minCol)
						minCol = cellC;
					if (cellC > maxCol)
						maxCol = cellC;
						
					//----------------
					// Mark the map...
					MapCellPtr curCell = GameMap->getCell(cellR, cellC);
					if (appearType->isForestClump) {
						if (vertexPos.z <= 1.0f)
							curCell->setPassable(passable);
						}
					else {
						if (vertexPos.z >= 1.0f)
							curCell->setPassable(passable);
					}
				}
			}
		}
	}
	
	if (lineOfSightRect) {
		lineOfSightRect[0] = minRow;
		lineOfSightRect[1] = minCol;
		lineOfSightRect[2] = maxRow;
		lineOfSightRect[3] = maxCol;
	}

	if (tempBldgShape != bldgShape)
	{
		tempBldgShape->ClearAnimation();
		delete tempBldgShape;
		tempBldgShape = NULL;
	}

	return(numCoords/2);
}

//-----------------------------------------------------------------------------

void BldgAppearance::markLOS (bool clearIt)
{
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	TG_MultiShapePtr tempBldgShape = bldgShape;
	if (currentLOD)
	{
		tempBldgShape = appearType->bldgShape[0]->CreateFrom();
		if (bdAnimationState != -1)
			appearType->setAnimation(tempBldgShape,bdAnimationState);
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	for (int i=0;i<tempBldgShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block LOS!!
		// Probably should check for light cones,too!
		
		if ((S_strnicmp(tempBldgShape->GetNodeId(i),"LOS_",4) != 0) &&
			(S_strnicmp(tempBldgShape->GetNodeId(i),"SpotLight_",10) != 0))
		{
			for (int j=0;j<tempBldgShape->GetNumVerticesInShape(i);j++)
			{
				Stuff::Vector3D vertexPos, worldPos;
				vertexPos = tempBldgShape->GetShapeVertexInEditor(i,j,-rotation);
//				vertexPos = tempBldgShape->GetShapeVertexInWorld(i,j,-rotation);
				worldPos.Add(position,vertexPos);
	
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
					(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
				{
					continue;
				}
				
				//----------------
				// Mark the map...
				MapCellPtr curCell = GameMap->getCell(cellR, cellC);

				if (!clearIt)
				{
					float currentCellHeight = curCell->getLocalHeight();
					
					float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
					if (cellLocalHeight > 15.0f)
						cellLocalHeight = 15.0f;

					if (cellLocalHeight > currentCellHeight)
						curCell->setLocalHeight(cellLocalHeight+0.5f);
				}
				else	//We want to clear all LOS height INFO.  We're about to change shape!!
				{
					curCell->setLocalHeight(0.0f);
				}
			}
		}
	}

	if (tempBldgShape != bldgShape)
	{
		tempBldgShape->ClearAnimation();
		delete tempBldgShape;
		tempBldgShape = NULL;
	}
}

//-----------------------------------------------------------------------------

void BldgAppearance::calcAdjCell (long& row, long& col)
{
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		bldgShape->ClearAnimation();
		delete bldgShape;
		bldgShape = NULL;
	
		bldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
		if (bdAnimationState != -1)
			appearType->setAnimation(bldgShape,bdAnimationState);
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	int numVert = 0;
	for (int i=0;i<bldgShape->GetNumShapes();i++)
	{
		for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++)
		{
			Stuff::Vector3D vertexPos, worldPos;
			vertexPos = bldgShape->GetShapeVertexInWorld(i,j,-rotation);
			worldPos.Add(position,vertexPos);

			{
				numVert++;
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				
				//MapCellPtr curCell = GameMap->getCell(cellR, cellC);
				//curCell->setPassable(passable);	
			}
		}
	}
}

//-----------------------------------------------------------------------------
// class TreeAppearanceType
void TreeAppearanceType::init (const char * fileName)
{
	AppearanceType::init(fileName);

	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
		Fatal(result,"Could not find building appearance INI file");

	result = iniFile.seekBlock("TGLData");
	if (result != NO_ERR)
		Fatal(result,"Could not find block in building appearance INI file");

	result = iniFile.readIdBoolean("ForestClump",isForestClump);
	if (result != NO_ERR)
		isForestClump = false;
		
 	char aseFileName[512];
	result = iniFile.readIdString("FileName",aseFileName,511);
	if (result != NO_ERR)
	{
		//Check for LOD filenames instead
		for (long i=0;i<MAX_LODS;i++)
		{
			char baseName[256];
			char baseLODDist[256];
			sprintf(baseName,"FileName%d",i);
			sprintf(baseLODDist,"Distance%d",i);
			
			result = iniFile.readIdString(baseName,aseFileName,511);
			if (result == NO_ERR)
			{
				result = iniFile.readIdFloat(baseLODDist,lodDistance[i]);
				if (result != NO_ERR)
					STOP(("LOD %d has no distance value in file %s",i,fileName));
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				//----------------------------------------------
				// Base LOD shape.  In stand Pose by default.
				treeShape[i] = new TG_TypeMultiShape;
				gosASSERT(treeShape[i] != NULL);
			
				FullPathFileName treeName;
				treeName.init(tglPath,aseFileName,".ase");
			
				treeShape[i]->LoadTGMultiShapeFromASE(treeName);
				
				//---------------------------------------------------------
				// Should only be necessary for trees.  Easy to data drive
				treeShape[i]->SetAlphaTest(true);
				treeShape[i]->SetFilter(true);
			}
			else if (!i)
			{
				STOP(("No base LOD for shape %s",fileName));
			}
		}
	}
	else
	{
		//----------------------------------------------
		// Base shape.  In stand Pose by default.
		treeShape[0] = new TG_TypeMultiShape;
		gosASSERT(treeShape[0] != NULL);
	
		FullPathFileName treeName;
		treeName.init(tglPath,aseFileName,".ase");
	
		treeShape[0]->LoadTGMultiShapeFromASE(treeName);
		
		//---------------------------------------------------------
		// Should only be necessary for trees.  Easy to data drive
		treeShape[0]->SetAlphaTest(true);
		treeShape[0]->SetFilter(true);
	}

	result = iniFile.readIdString("ShadowName",aseFileName,511);
	if (result == NO_ERR)
	{
		//----------------------------------------------
		// Base Shadow shape.
		treeShadowShape = new TG_TypeMultiShape;
		gosASSERT(treeShadowShape != NULL);
	
		FullPathFileName treeName;
		treeName.init(tglPath,aseFileName,".ase");
	
		treeShadowShape->LoadTGMultiShapeFromASE(treeName);
		
		//---------------------------------------------------------
		// Should only be necessary for trees.  Easy to data drive
		treeShadowShape->SetAlphaTest(true);
		treeShadowShape->SetFilter(true);
	}
	
	result = iniFile.seekBlock("TGLDamage");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",aseFileName,511);
		if (result != NO_ERR)
			Fatal(result,"Could not find ASE FileName in building appearance INI file");
	
		FullPathFileName dmgName;
		dmgName.init(tglPath,aseFileName,".ase");
	
		treeDmgShape = new TG_TypeMultiShape;
		gosASSERT(treeDmgShape != NULL);
		treeDmgShape->LoadTGMultiShapeFromASE(dmgName);

		if (!treeDmgShape->GetNumShapes())
		{
			delete treeDmgShape;
			treeDmgShape = NULL;
		}
		
		//Shadow for destroyed state.
		result = iniFile.readIdString("ShadowName",aseFileName,511);
		if (result == NO_ERR)
		{
			//----------------------------------------------
			// Base Shadow shape.
			treeDmgShadowShape = new TG_TypeMultiShape;
			gosASSERT(treeDmgShadowShape != NULL);
		
			FullPathFileName treeName;
			treeName.init(tglPath,aseFileName,".ase");
		
			treeDmgShadowShape->LoadTGMultiShapeFromASE(treeName);
			if (!treeDmgShadowShape->GetNumShapes())
			{
				delete treeDmgShadowShape;
				treeDmgShadowShape = NULL;
			}
		}
	}
	else
	{
		treeDmgShape = NULL;
		treeDmgShadowShape = NULL;
	}

 	//No Animations at present.
}

//----------------------------------------------------------------------------
void TreeAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	for (long i=0;i<MAX_LODS;i++)
	{
		if (treeShape[i])
		{
			delete treeShape[i];
			treeShape[i] = NULL;
		}
	}

	if (treeDmgShape)
	{
		delete treeDmgShape;
		treeDmgShape = NULL;
	}
	
	if (treeDmgShadowShape)
	{
		delete treeDmgShadowShape;
		treeDmgShadowShape = NULL;
	}
	
 	if (treeShadowShape)
	{
		delete treeShadowShape;
		treeShadowShape = NULL;
	}
}

//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// class TreeAppearance
void TreeAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	appearType = (TreeAppearanceType *)tree;

	shapeMin.x = shapeMin.y = -25;
	shapeMax.x = shapeMax.y = 50;
	
	paintScheme = -1;
	objectNameId = 30862;
	
	hazeFactor = 0.0f;

	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	position.Zero();
	rotation = 0.0;;
	selected = 0;
	teamId = 0;
	homeTeamRelationship = 0;
	actualRotation = rotation;

	OBBRadius = -1.0f;

	currentLOD = 0;
	
	beenInView = false;
	
	fogLightSet = false;
	lightRGB = fogRGB = 0xffffffff;

    // sebi: init so will not be garbage
    status = OBJECT_STATUS_NORMAL;
    forceLightsOut = false;
    // Slice 2 (object-offload) substrate flag; set true by GPU batcher on late
    // registration to force a full TransformMultiShape next frame.
    needsFullBakeNextFrame = false;
    staticReg = {};  // Stage 3.C: zero-init StaticRegistration
    treeShape = NULL;
    //

	if (appearType)
	{
		treeShape = appearType->treeShape[0]->CreateFrom();

		//-------------------------------------------------
		// Load the texture and store its handle.
		for (long i=0;i<treeShape->GetNumTextures();i++)
		{
			char txmName[1024];
			treeShape->GetTextureName(i,txmName,256);
	
			char texturePath[1024];
			sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (fileExists(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					treeShape->SetTextureHandle(i,gosTextureHandle);
					treeShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					treeShape->SetTextureHandle(i,gosTextureHandle);
					treeShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				treeShape->SetTextureHandle(i,0xffffffff);
			}
		}
		
		if (appearType->treeShadowShape)
		{
			treeShadowShape = appearType->treeShadowShape->CreateFrom();
	
			//-------------------------------------------------
			// Load the texture and store its handle.
			for (long i=0;i<treeShadowShape->GetNumTextures();i++)
			{
				char txmName[1024];
				treeShadowShape->GetTextureName(i,txmName,256);
		
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (fileExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShadowShape->SetTextureHandle(i,gosTextureHandle);
						treeShadowShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShadowShape->SetTextureHandle(i,gosTextureHandle);
						treeShadowShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					treeShadowShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}
		else
		{
			treeShadowShape = NULL;
		}
		
		Stuff::Vector3D boxCoords[8];
		Stuff::Vector3D nodeCenter = treeShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		float testRadius = 0.0;
		
		for (int i=0;i<8;i++)
		{
			testRadius = boxCoords[i].GetLength();
			if (OBBRadius < testRadius)
				OBBRadius = testRadius;
		}

		
		appearType->boundsUpperLeftX = (-OBBRadius * 2.0);
		appearType->boundsUpperLeftY = (-OBBRadius * 2.0);
		   					 
		appearType->boundsLowerRightX = (OBBRadius * 2.0);
		appearType->boundsLowerRightY = (OBBRadius);
		
		if (!appearType->getDesignerTypeBounds())
		{
			appearType->typeUpperLeft = treeShape->GetMinBox();
			appearType->typeLowerRight = treeShape->GetMaxBox();
		}

		// GPU static-prop batcher: register this tree's type shapes + variants.
		for (int i = 0; i < MAX_LODS; ++i)
			GpuStaticPropBatcher::instance().registerMultiShape(appearType->treeShape[i]);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->treeShadowShape);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->treeDmgShape);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->treeDmgShadowShape);
	}

	pitch = yaw = 0.0f;
}

//-----------------------------------------------------------------------------
void TreeAppearance::setObjStatus (long oStatus)
{
	if (status != oStatus)
	{
		if ((oStatus == OBJECT_STATUS_DESTROYED) || (oStatus == OBJECT_STATUS_DISABLED))
		{
			if (appearType->treeDmgShape)
			{
				if (treeShape)
				{
					treeShape->ClearAnimation();
					delete treeShape;
					treeShape = NULL;
				}
				
				treeShape = appearType->treeDmgShape->CreateFrom();
				beenInView = false; 
			}
			
			if (appearType->treeDmgShadowShape)
			{
				if (treeShadowShape)
				{
					treeShadowShape->ClearAnimation();
					delete treeShadowShape;
					treeShadowShape = NULL;
				}
				
				treeShadowShape = appearType->treeDmgShadowShape->CreateFrom();
				
				beenInView = false; 
			}
		}
		
		if (oStatus == OBJECT_STATUS_NORMAL)
		{
			if (appearType->treeShape[0])
			{
				if (treeShape)
				{
					treeShape->ClearAnimation();
					delete treeShape;
					treeShape = NULL;
				}
				
				treeShape = appearType->treeShape[0]->CreateFrom();
				beenInView = false; 
			}
			
			if (appearType->treeShadowShape)
			{
				if (treeShadowShape)
				{
					treeShadowShape->ClearAnimation();
					delete treeShadowShape;
					treeShadowShape = NULL;
				}
				
				treeShadowShape = appearType->treeShadowShape->CreateFrom();
				
				beenInView = false;
			}
		}
		
		//-------------------------------------------------
		// Load the texture and store its handle.
		if (treeShape)
		{
			for (long i=0;i<treeShape->GetNumTextures();i++)
			{
				char txmName[1024];
				treeShape->GetTextureName(i,txmName,256);
		
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (fileExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShape->SetTextureHandle(i,gosTextureHandle);
						treeShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShape->SetTextureHandle(i,gosTextureHandle);
						treeShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					//PAUSE(("Warning: %s texture name not found",textureName));
					treeShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}

		if (treeShadowShape)
		{
			//-------------------------------------------------
			// Load the texture and store its handle.
			for (long i=0;i<treeShadowShape->GetNumTextures();i++)
			{
				char txmName[1024];
				treeShadowShape->GetTextureName(i,txmName,256);
		
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (fileExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShadowShape->SetTextureHandle(i,gosTextureHandle);
						treeShadowShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShadowShape->SetTextureHandle(i,gosTextureHandle);
						treeShadowShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					//PAUSE(("Warning: %s texture name not found",textureName));
					treeShadowShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}
	}
	
	status = oStatus;
}

//-----------------------------------------------------------------------------
void TreeAppearance::setObjectParameters (const Stuff::Vector3D &pos, float Rot, long sel, long team, long homeRelations)
{
	rotation = Rot;

	position = pos;

	selected = sel;

	actualRotation = Rot;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

//-----------------------------------------------------------------------------
void TreeAppearance::setMoverParameters (float pitchAngle, float lArmRot, float rArmRot, bool isAirborne)
{
	pitch = pitchAngle;
}

//-----------------------------------------------------------------------------
bool TreeAppearance::isMouseOver (float px, float py)
{
	if (inView)
	{
		if ((px <= lowerRight.x) && (py <= lowerRight.y) &&
			(px >= upperLeft.x) &&
			(py >= upperLeft.y))
		{
			return inView;
		}
		else
		{
			return FALSE;
		}
	}
	
	return(inView);
}	

//-----------------------------------------------------------------------------
bool TreeAppearance::recalcBounds (void)
{
	// [TOBJSPLIT v1] accumulators declared in code/static_update_counters.h
	// (included above via ../code/static_update_counters.h).
	// s_tobjSplitEnabled is a file-static duplicate (one getenv per TU;
	// process-start-constant -- no observable cost when disabled).
	static bool s_tobjSplitEnabled = (getenv("MC2_TOBJ_COST_SPLIT") != nullptr);

	Stuff::Vector4D tempPos;
	inView = false;

	float distanceToEye = 0.0f;

	if (eye)
	{
		//-------------------------------------------------------------------
		//NEW METHOD from the WAY BACK Days
		inView = true;

		// [TOBJSPLIT v1] ANGULAR bracket: matrix-free sphere angular clip.
		// Disjoint from PROJ below; reads cycle counter immediately before/after.
		{
		unsigned long long _tsA = s_tobjSplitEnabled ? __rdtsc() : 0ULL;
		if (eye->usePerspective)
		{
			Stuff::Vector3D cameraPos;
			cameraPos.x = -eye->getCameraOrigin().x;
			cameraPos.y = eye->getCameraOrigin().z;
			cameraPos.z = eye->getCameraOrigin().y;
			float vClipConstant = eye->verticalSphereClipConstant;
			float hClipConstant = eye->horizontalSphereClipConstant;

			Stuff::Vector3D objectCenter;
			objectCenter.Subtract(position,cameraPos);
			Camera::cameraFrame.trans_to_frame(objectCenter);
			distanceToEye = objectCenter.GetApproximateLength();
			float clip_distance = fabs(1.0f / objectCenter.y);

			//Is vertex on Screen OR close enough to screen that its triangle MAY be visible?
			// WE have removed the atans here by simply taking the tan of the angle we want above.
			float object_angle = fabs(objectCenter.z) * clip_distance;
			float extent_angle = treeShape->GetExtentRadius() / distanceToEye;
			if (object_angle > (vClipConstant + extent_angle))
			{
				//In theory, we would return here.  Object is NOT on screen.
				inView = false;
			}
			else
			{
				object_angle = fabs(objectCenter.x) * clip_distance;
				if (object_angle > (hClipConstant + extent_angle))
				{
					//In theory, we would return here.  Object is NOT on screen.
					inView = false;
				}
			}
		}
		if (s_tobjSplitEnabled) g_tobjAngularCyc += __rdtsc() - _tsA;
		}  // end ANGULAR bracket

		//Can we be seen at all?
		// If yes, check if we are behind fog plane.
		// [TOBJSPLIT v1] PROJ bracket: projectForScreenXY + 8-corner box + fog.
		// Disjoint from ANGULAR above; this is the body targeted for deletion.
		unsigned long long _tsP = s_tobjSplitEnabled ? __rdtsc() : 0ULL;
		if (inView)
		{
			//ALWAYS need to do this or select is YAYA
			// But now inView is correct.
			// [PROJECTZ:ScreenXYOracle id=bdactor_screen_pos_b]
			eye->projectForScreenXY(position,screenPos);
		
			if (eye->usePerspective)
			{
				if (distanceToEye > Camera::MaxClipDistance)
				{
					hazeFactor = 1.0f;
					inView = false;
				}
				else if (distanceToEye > Camera::MinHazeDistance)
				{
					Camera::HazeFactor = (distanceToEye - Camera::MinHazeDistance) * Camera::DistanceFactor;
					inView = true;
				}
				else
				{
					Camera::HazeFactor = 0.0f;
					inView = true;
				}
			
			}
			else
			{
				Camera::HazeFactor = 0.0f;
				inView = true;
			}
		}
		
		//If we were not behind fog plane, do a bunch O math we need later!!
		if (inView)
		{
			//We are on screen.  Figure out selection box.
			Stuff::Vector3D boxCoords[8];
			Stuff::Vector4D bcsp[8];

			Stuff::Vector3D minBox;
			minBox.x = -appearType->typeUpperLeft.x;
			minBox.y = appearType->typeUpperLeft.z;
			minBox.z = appearType->typeUpperLeft.y;

			Stuff::Vector3D maxBox;
			maxBox.x = -appearType->typeLowerRight.x;
			maxBox.y = appearType->typeLowerRight.z;
			maxBox.z = appearType->typeLowerRight.y;

			if (rotation != 0.0f)
				Rotate(minBox,-rotation);

			if (rotation != 0.0f)
				Rotate(maxBox,-rotation);

			boxCoords[0].x = position.x + minBox.x;
			boxCoords[0].y = position.y + minBox.y;
			boxCoords[0].z = position.z + minBox.z;

			boxCoords[1].x = position.x + minBox.x;
			boxCoords[1].y = position.y + maxBox.y;
			boxCoords[1].z = position.z + minBox.z;

			boxCoords[2].x = position.x + maxBox.x;
			boxCoords[2].y = position.y + minBox.y;
			boxCoords[2].z = position.z + minBox.z;

			boxCoords[3].x = position.x + maxBox.x;
			boxCoords[3].y = position.y + maxBox.y;
			boxCoords[3].z = position.z + minBox.z;

			boxCoords[4].x = position.x + maxBox.x;
			boxCoords[4].y = position.y + maxBox.y;
			boxCoords[4].z = position.z + maxBox.z;

			boxCoords[5].x = position.x + maxBox.x;
			boxCoords[5].y = position.y + minBox.y;
			boxCoords[5].z = position.z + maxBox.z;

			boxCoords[6].x = position.x + minBox.x;
			boxCoords[6].y = position.y + maxBox.y;
			boxCoords[6].z = position.z + maxBox.z;

			boxCoords[7].x = position.x + minBox.x;
			boxCoords[7].y = position.y + minBox.y;
			boxCoords[7].z = position.z + maxBox.z;

			float maxX = 0.0f, maxY = 0.0f;
			float minX = 0.0f, minY = 0.0f;

			for (long i=0;i<8;i++)
			{
				// [PROJECTZ:ScreenXYOracle id=bdactor_box_rect_b]
				eye->projectForScreenXY(boxCoords[i],bcsp[i]);
				if (!i)
				{
					maxX = minX = bcsp[i].x;
					maxY = minY = bcsp[i].y;
				}
				
				if (i)
				{
					if (bcsp[i].x > maxX)
						maxX = bcsp[i].x;
					
					if (bcsp[i].x < minX)
						minX = bcsp[i].x;
						
					if (bcsp[i].y > maxY)
						maxY = bcsp[i].y;
					
					if (bcsp[i].y < minY)
						minY = bcsp[i].y;
				}
			}
	
			upperLeft.x = minX;
			upperLeft.y = minY;
			lowerRight.x = maxX;
			lowerRight.y = maxY;
			
			if ((lowerRight.x >= 0) && (lowerRight.y >= 0) &&
				(upperLeft.x <= eye->getScreenResX()) &&
				(upperLeft.y <= eye->getScreenResY()))
			{
				inView = true;
		
				if ((status != OBJECT_STATUS_DESTROYED) && (status != OBJECT_STATUS_DISABLED))
				{
					//-------------------------------------------------------------------------------
					//Set LOD of Model here because we have the distance and we KNOW we can see it!
					bool baseLOD = true;
					DWORD selectLOD = 0;
					// Trees use low-LOD crossed cards that light per-plane, which
					// creates visible bright/dark self-intersections once valid
					// lightData_ is restored under UPDATE_SKIP. Keep visible trees
					// at LOD 0; tree GPU cost is negligible after renderer offload
					// and these assets are expected to be replaced later.

					// we are at this LOD level.
					if (selectLOD != currentLOD)
					{
						currentLOD = selectLOD;

						treeShape->ClearAnimation();
						delete treeShape;
						treeShape = NULL;

						treeShape = appearType->treeShape[currentLOD]->CreateFrom();
						//-------------------------------------------------
						// Load the texture and store its handle.
						for (long j=0;j<treeShape->GetNumTextures();j++)
						{
							char txmName[1024];
							treeShape->GetTextureName(j,txmName,256);

							char texturePath[1024];
							sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);

							FullPathFileName textureName;
							textureName.init(texturePath,txmName,"");

							if (fileExists(textureName))
							{
								if (S_strnicmp(txmName,"a_",2) == 0)
								{
									DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
									gosASSERT(gosTextureHandle != 0xffffffff);
									treeShape->SetTextureHandle(j,gosTextureHandle);
									treeShape->SetTextureAlpha(j,true);
								}
								else
								{
									DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
									gosASSERT(gosTextureHandle != 0xffffffff);
									treeShape->SetTextureHandle(j,gosTextureHandle);
									treeShape->SetTextureAlpha(j,false);
								}
							}
							else
							{
								//PAUSE(("Warning: %s texture name not found",textureName));
								treeShape->SetTextureHandle(j,0xffffffff);
							}
						}
					}
						
					//ONLY change if we need
					if (currentLOD && baseLOD)
					{
					// we are at the Base LOD level.
						currentLOD = 0;
						
						treeShape->ClearAnimation();
						delete treeShape;
						treeShape = NULL;
						
						treeShape = appearType->treeShape[currentLOD]->CreateFrom();
						
						//-------------------------------------------------
						// Load the texture and store its handle.
						for (long i=0;i<treeShape->GetNumTextures();i++)
						{
							char txmName[1024];
							treeShape->GetTextureName(i,txmName,256);
									
							char texturePath[1024];
							sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
					
							FullPathFileName textureName;
							textureName.init(texturePath,txmName,"");
									
							if (fileExists(textureName))
							{
								if (S_strnicmp(txmName,"a_",2) == 0)
								{
									DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
									gosASSERT(gosTextureHandle != 0xffffffff);
									treeShape->SetTextureHandle(i,gosTextureHandle);
									treeShape->SetTextureAlpha(i,true);
								}
								else
								{
									DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
									gosASSERT(gosTextureHandle != 0xffffffff);
									treeShape->SetTextureHandle(i,gosTextureHandle);
									treeShape->SetTextureAlpha(i,false);
								}
							}
							else
							{
								//PAUSE(("Warning: %s texture name not found",textureName));
								treeShape->SetTextureHandle(i,0xffffffff);
							}
						}
					}
				}
			}
			else
			{
				inView = false;		//Did alot of extra work checking this, but WHY draw and insult to injury?
			}
		}
		if (s_tobjSplitEnabled) g_tobjProjCyc += __rdtsc() - _tsP;
		// end PROJ bracket
	}

	return(inView);
}

//-----------------------------------------------------------------------------
long TreeAppearance::render (long depthFixup)
{
	// Mirror BldgAppearance::render: bypass inView under GPU path — the
	// GPU clipper decides visibility, and the legacy angular cull has a
	// ~87% false-negative rate at wolfman zoom.
	if (inView || g_useGpuStaticProps)
	{
		long color = SD_BLUE;
		//unsigned long highLight = 0x007f7f7f;
		if ((teamId > -1) && (teamId < 8)) {
			//static unsigned long highLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
			static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE | 0xff000000, SB_RED | 0xff000000};
			color = colorTable[homeTeamRelationship];
			//highLight = highLightTable[homeTeamRelationship];
		}
		//---------------------------------------------
		// Call Multi-shape render stuff here.
		// Slice 1 path (g_useGpuObjects). Same shape as BldgAppearance::render.
		bool submittedToGpu = false;
		if (g_useGpuObjects)
		{
			GpuStaticPropBatcher::instance().recordEligibleActor(
				GpuStaticPropPopulation::Tree);

			// Stage 3.C: static registry fast path. If this tree's instance was
			// previously registered and position/shape are stable, inject it into
			// the batcher via markVisible() (processed at registry flush) instead of
			// running the full submitMultiShape() compute path.
			// CacheGpuLightData() is called here (not in touch()) so the light-index
			// refresh is co-located with the render-side emission that needs it.
			// The UINT32_MAX guard handles the degenerate case where no light data
			// is available: invalidate and fall through to the dynamic path.
			// Does NOT return early — selection visualization (drawBars/drawBrackets)
			// at lines 4141-4161 must still run if selected is non-zero.
			if (IsStaticNow()) {
				// Diagnostic 2026-05-05 (advisor-recommended boundary test): set
				// MC2_FORCE_DYNAMIC_TREES=1 to force the static path to fall back
				// to dynamic submitMultiShape. If "black billboard square" trees
				// disappear with the env var set, the static replay path is the
				// failing boundary. If they remain, shared draw/material/global
				// state is guilty. Revert by unsetting the env var (no rebuild).
				static const bool s_forceDynamicTrees =
				    (getenv("MC2_FORCE_DYNAMIC_TREES") != nullptr);
				if (s_forceDynamicTrees) {
					invalidateStaticRegistration();
					// Fall through to the dynamic path below.
				} else if (treeShape->getCachedGpuLightIndex() == UINT32_MAX) {
					// Light-data gather failed this frame — invalidate so the dynamic
					// path re-runs and re-registers next frame with correct lights.
					invalidateStaticRegistration();
					// Fall through to the if (!submittedToGpu && treeShape) dynamic path below.
				} else {
					// 2026-05-11: see BldgAppearance::render markVisible site.
					GpuStaticPropRegistry::markVisible(staticReg.recipeIndex,
					                                  staticReg.lightDataIndex);
					submittedToGpu = true;
				}
			}
			if (!submittedToGpu && treeShape)
			{
				// Stage 3.C / M1: shape-swap invalidation. IsStaticNow()'s
				// staticReg.shape==treeShape check routes us here when treeShape was
				// reassigned (LOD swap at bdactor.cpp:3984, damage at ~3596/3626), but
				// staticReg.registered==true still blocks the registration block below.
				// Invalidate the stale entry first so the new shape gets registered.
				// PERF DIAGNOSTIC 2026-05-07: count this branch — per capture 3
				// analysis, LOD-swap-driven invalidate+re-register on trees was
				// running tens of times per frame, leaking recipe slots and
				// pin-count churn. Tracy zone makes the rate visible per-frame.
				if (staticReg.registered && staticReg.shape != treeShape) {
					invalidateStaticRegistration();
				}

				// Stage 2.C+: see BldgAppearance::render for callerName intent.
				const char* callerName = (appearType ? appearType->name : nullptr);
				submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
					treeShape, GpuStaticPropPopulation::Tree, callerName);
				// Slice 2 (object-offload) — Stage 2.B: see BldgAppearance::render
				// for full rationale on the late-reg recovery flag.
				if (!submittedToGpu &&
				    GpuStaticPropBatcher::instance().wasLastFailureLateRegistration())
				{
					needsFullBakeNextFrame = true;
					invalidateStaticRegistration();  // clear stale registration if any
				}
				// Stage 3.C: registration block. On the first successful full-bake
				// submission with no late-reg flag, snapshot the leaf batch into the
				// registry. Subsequent frames use the static path above.
				// Pass treeShape as multi so flush() can patch lightDataIndex each frame.
				if (submittedToGpu && !staticReg.registered
				        && GpuStaticPropRegistry::isEnabled()
				        && !needsFullBakeNextFrame) {
					const auto& batch =
						GpuStaticPropBatcher::instance().getLastBuiltBatch();
					staticReg.recipeIndex = GpuStaticPropRegistry::registerRecipe(
						treeShape, batch);
					staticReg.registered  = (staticReg.recipeIndex >= 0);
					staticReg.shape        = treeShape;
					if (staticReg.registered) {
						// H4 follow-up (2026-05-07): per-frame re-registration
						// after LOD/shape swap has the same lightData_ gap as
						// mission-load registerStatic(). Force one full update()
						// so touch() cannot resubmit default-zero lightData_.
						// Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
						needsFullBakeNextFrame = true;
					}
				}
			}
			if (!submittedToGpu)
			{
				GpuStaticPropBatcher::instance().recordCpuFallback(
					GpuStaticPropPopulation::Tree);
			}
		}
		// Legacy bypass-cull path. Mutually exclusive with slice 1 — gated on
		// !g_useGpuObjects. Tagged Legacy so Gate F's fallback-rate is computed
		// only over slice-1 populations. See spec R1.
		if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && treeShape)
		{
			submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
				treeShape, GpuStaticPropPopulation::Legacy);
		}
		if (!submittedToGpu)
			treeShape->Render();

		if (selected & DRAW_BARS)
		{
			drawBars();
		}

		if ( selected & DRAW_BRACKETS )
		{
			drawSelectBrackets(color);
		}

		if ( selected & DRAW_TEXT )
		{

			if (objectNameId != -1)
			{
				char tmpString[255];
				cLoadString(objectNameId, tmpString, 254);

				drawTextHelp(tmpString, color);
			}
		}

		// I don't want my selection reset each time I draw HKG
//		selected = FALSE;

		//---------------------------------------------------------
		// Render the Bounding Box to see if it is OK.
#ifdef DRAW_BOX
		Stuff::Vector3D nodeCenter = treeShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + treeShape->minBox.x + nodeCenter.x;
		boxCoords[0].y = position.y + treeShape->minBox.z + nodeCenter.z;
		boxCoords[0].z = position.z + treeShape->maxBox.y + nodeCenter.y;
		
		boxCoords[1].x = position.x + treeShape->minBox.x + nodeCenter.x;
		boxCoords[1].y = position.y + treeShape->maxBox.z + nodeCenter.z;
		boxCoords[1].z = position.z + treeShape->maxBox.y + nodeCenter.y;
		
		boxCoords[2].x = position.x + treeShape->maxBox.x + nodeCenter.x;
		boxCoords[2].y = position.y + treeShape->maxBox.z + nodeCenter.z;
		boxCoords[2].z = position.z + treeShape->maxBox.y + nodeCenter.y;
		
		boxCoords[3].x = position.x + treeShape->maxBox.x + nodeCenter.x;
		boxCoords[3].y = position.y + treeShape->minBox.z + nodeCenter.z;
		boxCoords[3].z = position.z + treeShape->maxBox.y + nodeCenter.y;
		
		boxCoords[4].x = position.x + treeShape->minBox.x + nodeCenter.x;
		boxCoords[4].y = position.y + treeShape->minBox.z + nodeCenter.z;
		boxCoords[4].z = position.z + treeShape->minBox.y + nodeCenter.y;
		
		boxCoords[5].x = position.x + treeShape->maxBox.x + nodeCenter.x;
		boxCoords[5].y = position.y + treeShape->minBox.z + nodeCenter.z;
		boxCoords[5].z = position.z + treeShape->minBox.y + nodeCenter.y;
		
		boxCoords[6].x = position.x + treeShape->maxBox.x + nodeCenter.x;
		boxCoords[6].y = position.y + treeShape->maxBox.z + nodeCenter.z;
		boxCoords[6].z = position.z + treeShape->minBox.y + nodeCenter.y;
		
		boxCoords[7].x = position.x + treeShape->minBox.x + nodeCenter.x;
		boxCoords[7].y = position.y + treeShape->maxBox.z + nodeCenter.z;
		boxCoords[7].z = position.z + treeShape->minBox.y + nodeCenter.y;

		Stuff::Vector4D screenPos[8];
		for (long i=0;i<8;i++)
		{
			// [PROJECTZ:ScreenXYOracle id=bdactor_box_wire_b]
			eye->projectForScreenXY(boxCoords[i],screenPos[i]);
		}

		{
			LineElement newElement(screenPos[0],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}

#endif
	}
	return NO_ERR;
}

//-----------------------------------------------------------------------------
long TreeAppearance::renderShadows (void)
{
	// Skip legacy blob shadows when shadow maps are active
	if (gos_IsTerrainTessellationActive())
		return NO_ERR;

	if (inView && visible)
	{
		//---------------------------------------------
		// Call Multi-shape render stuff here.
		if (treeShadowShape)
			treeShadowShape->RenderShadows();
		else
			treeShape->RenderShadows();
	}
	
	return NO_ERR;
}

//-----------------------------------------------------------------------------
long TreeAppearance::update (bool animate)
{
	::mc2_object_recon::Scope _recon_tree_(
		&::mc2_object_recon::g_per_frame.tree_update_ns,
		&::mc2_object_recon::g_per_frame.tree_update_calls);
	if (rotation > 180)
		rotation -= 360;

	if (rotation < -180)
		rotation += 360;

	//-------------------------------------------
	// Does math necessary to draw Tree
	Stuff::UnitQuaternion rot;
	float yawAngle = (rotation * DEGREES_TO_RADS) + (yaw * DEGREES_TO_RADS);
	float pitchAngle = (pitch * DEGREES_TO_RADS);
	rot = Stuff::EulerAngles(pitchAngle, yawAngle, 0.0f);

	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y = position.z;
	xlatPosition.z = position.y;

	if (!fogLightSet)
	{
		unsigned char lightr,lightg,lightb;
		float lightIntensity = 1.0f;
		if (land)
			lightIntensity = land->getTerrainLight(position);

		lightr = eye->getLightRed(lightIntensity);
		lightg = eye->getLightGreen(lightIntensity);
		lightb = eye->getLightBlue(lightIntensity);

		lightRGB = (lightr<<16) + (lightg<<8) + lightb;

		fogRGB = 0xff<<24;
		float fogStart = eye->fogStart;
		float fogFull = eye->fogFull;

		if (xlatPosition.y < fogStart)
		{
			float fogFactor = fogStart - xlatPosition.y;
			if (fogFactor < 0.0)
				fogRGB = 0xff<<24;
			else
			{
				fogFactor /= (fogStart - fogFull);
				if (fogFactor <= 1.0)
				{
					fogFactor *= fogFactor;
					fogFactor = 1.0 - fogFactor;
					fogFactor *= 256.0;
				}
				else
				{
					fogFactor = 256.0;
				}

				unsigned char fogResult = float2long(fogFactor);
				fogRGB = fogResult << 24;
			}
		}
		else
		{
			fogRGB = 0xff<<24;
		}

		fogLightSet = true;
	}

	if (useFog)
		treeShape->SetFogRGB(fogRGB);
	else
		treeShape->SetFogRGB(0xffffffff);

	DWORD oldRGB = eye->getLightColor(1);

	eye->setLightColor(1,lightRGB);
	eye->setLightIntensity(1,1.0);

	if (forceLightsOut)
		treeShape->SetLightsOut(true);

	// Under the GPU static-prop path we need listOfColors / shapeToWorld
	// fresh every frame regardless of inView so submitMultiShape can safely
	// read shape->listOfVertices during submit().
	if (inView || g_useGpuStaticProps)
	{
		bool checkShadows = ((!beenInView) || (eye->forceShadowRecalc));

		if (treeShadowShape)
			treeShape->SetUseShadow(false);
		else
			treeShape->SetRecalcShadows(checkShadows);

		TG_LightPtr light = eye->getWorldLight(0);
		light->active = false;

		treeShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice 2 (object-offload) — Stage 2.B: eligibility hoist.
		// See BldgAppearance::update for the full rationale; same shape.
		// Branch lives INSIDE the existing inView||g_useGpuStaticProps cull
		// gate to preserve slice 1's R1 invariant.
		// PERF DIAGNOSTIC 2026-05-06: Tracy zones — see BldgAppearance::update
		// for the same instrumentation set. Same theories under investigation.
		bool gpuEligible;
		{
			gpuEligible = g_useGpuObjects &&
			              !needsFullBakeNextFrame &&
			              GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(treeShape);
		}

		if (gpuEligible)
		{
			// Stage 2.D.2 fix: cache GPU light data while lights are per-actor-correct.
			{
				mc2CacheOrBakeStaticGpuLight(treeShape, staticReg.registered, staticReg.recipeIndex);
				// 2026-05-11 per-instance capture (mirror of BldgAppearance::update).
				staticReg.lightDataIndex = treeShape->getCachedGpuLightIndex();
			}
			{
				treeShape->TransformMultiShape_PositionsOnly (&xlatPosition,&rot);
			}
			// Stage 2.D.2: dual-emit full bake — same rationale as BldgAppearance
			// above. Populates listOfTriangles[].aRGBLight for snapshot in submit().
			// Stage 2.D.3: per-actor gate (see BldgAppearance::update above).
			if (gos_object_parity::IsDualEmitArmedForActor(treeShape)) {
				treeShape->TransformMultiShape (&xlatPosition,&rot);
			}
		}
		else
		{
			treeShape->TransformMultiShape (&xlatPosition,&rot);
			// 2026-05-10: mirror of the BldgAppearance fix at :2339-2341.
			// Seed cachedGpuLightIndex_ in the full-bake branch so the
			// next render() doesn't fail the UINT32_MAX gate at :4341
			// and invalidate the freshly-set staticReg.
			mc2CacheOrBakeStaticGpuLight(treeShape, staticReg.registered, staticReg.recipeIndex);
			// 2026-05-11 per-instance capture (mirror of gpuEligible branch).
			staticReg.lightDataIndex = treeShape->getCachedGpuLightIndex();
			needsFullBakeNextFrame = false;
		}

		light->active = true;

		// Skip the legacy-blob-shadow per-frame transform when shadow maps are
		// active — same rationale as BldgAppearance::update. TreeAppearance::
		// renderShadows() at line 4544 already early-returns under the same
		// condition; treeShadowShape's transformed state is never consumed.
		if (treeShadowShape && useShadows && !gos_IsTerrainTessellationActive())
		{
			treeShadowShape->SetRecalcShadows(checkShadows);
			treeShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			treeShadowShape->TransformMultiShape (&xlatPosition,&rot);
		}
		
		if ((turn > 3) && useShadows)
			beenInView = true;
	}
	
	//Set Ambient back to normal color.
	eye->setLightColor(1,oldRGB);

	return TRUE;
}

//-----------------------------------------------------------------------------

void TreeAppearance::markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
{
	//MUST force tree to HIGHEST LOD!!!  Impassability data is only valid at this LOD!!
	// Tree will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		treeShape->ClearAnimation();
		delete treeShape;
		treeShape = NULL;
	
		treeShape = appearType->treeShape[currentLOD]->CreateFrom();
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	for (int i=0;i<treeShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block passability!!
		if (S_strnicmp(treeShape->GetNodeId(i),"_PAB",4) != 0)
		{
			for (int j=0;j<treeShape->GetNumVerticesInShape(i);j++)
			{
				Stuff::Vector3D vertexPos, worldPos;
				vertexPos = treeShape->GetShapeVertexInEditor(i,j,-rotation);
				worldPos.Add(position,vertexPos);
	
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
					(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
				{
					continue;
				}
				_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);

				if (vertexPos.z >= 1.0f)
				{
					pTmp->forest = true;
						
					float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
					if (cellLocalHeight > 15.0f)
						cellLocalHeight = 15.0f;
						
					if (pTmp->lineOfSight < cellLocalHeight)
						pTmp->lineOfSight = cellLocalHeight+0.5f;
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
void TreeAppearance::markLOS (bool clearIt)
{
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		treeShape->ClearAnimation();
		delete treeShape;
		treeShape = NULL;
	
		treeShape = appearType->treeShape[currentLOD]->CreateFrom();
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	for (int i=0;i<treeShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block LOS!!
		// Probably should check for light cones,too!
		
		if ((S_strnicmp(treeShape->GetNodeId(i),"LOS_",4) != 0) &&
			(S_strnicmp(treeShape->GetNodeId(i),"SpotLight_",10) != 0))
		{
			for (int j=0;j<treeShape->GetNumVerticesInShape(i);j++)
			{
				Stuff::Vector3D vertexPos, worldPos;
				vertexPos = treeShape->GetShapeVertexInEditor(i,j,-rotation);
//				vertexPos = treeShape->GetShapeVertexInWorld(i,j,-rotation);
				worldPos.Add(position,vertexPos);
	
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
					(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
				{
					continue;
				}
				
				//----------------
				// Mark the map...
				MapCellPtr curCell = GameMap->getCell(cellR, cellC);

				float currentCellHeight = curCell->getLocalHeight();
				
				float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
				if (cellLocalHeight > 15.0f)
					cellLocalHeight = 15.0f;
				
				if (!clearIt)
				{
					if (cellLocalHeight > currentCellHeight)
						curCell->setLocalHeight(cellLocalHeight+0.5f);
				}
				else	//We want to clear all LOS height INFO.  We're about to change shape!!
				{
					curCell->setLocalHeight(0.0f);
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------

bool TreeAppearance::IsStaticNow() const
{
	return staticReg.registered
		&& staticReg.shape == treeShape
		&& !needsFullBakeNextFrame;
}

void TreeAppearance::touch()
{
	// Stage 3.C: called by the outer-skip gate instead of update() when this
	// tree is registered and stable. Re-submits the cached lightData_ (set
	// during the last update() call) to get a fresh UBO slot index for this
	// frame — no s_listOfLights dependency, no terrain lookup needed.
	// Touch() advances lastTurnTransformed so TG_Shape::Render()'s staleness
	// guard doesn't suppress the legacy fallback path.
	if (treeShape) {
		// [LIGHTBRIDGE v1] C6 retirement: repoint to the primed 38d8720 slot
		// (zero FNV/memcmp; cachedFrame_ stamped). MISS keeps the legacy
		// resubmit (NOT CacheGpuLightData -- terrain-color-staleness,
		// msl.cpp:1874-1887). MC2_LIGHTBAKE=0 -> legacy path bit-for-bit.
		extern bool mc2LightBakeEnabled();
		extern bool mc2GetBakedStaticLight(int32_t, TG_HWLightsData&);
		TG_HWLightsData baked;
		if (mc2LightBakeEnabled()
		    && staticReg.registered && staticReg.recipeIndex >= 0
		    && mc2GetBakedStaticLight(staticReg.recipeIndex, baked)) {
			treeShape->EmitBakedGpuLightData(staticReg.recipeIndex, baked);
		} else {
			treeShape->ResubmitCachedGpuLightData();
		}
		// 2026-05-11 per-instance capture: see BldgAppearance::touch.
		staticReg.lightDataIndex = treeShape->getCachedGpuLightIndex();
		treeShape->Touch();
	}
}

void TreeAppearance::invalidateStaticRegistration()
{
	if (staticReg.registered && staticReg.recipeIndex >= 0)
		GpuStaticPropRegistry::invalidate(staticReg.recipeIndex);
	staticReg = {};
}

// Task 5 (Track B): mission-load bulk static-prop registration (mirror of
// BldgAppearance::registerStatic). HC-1: writes directly to typed staticReg.
void TreeAppearance::registerStatic() {
	if (staticReg.registered) return;
	if (!treeShape)           return;
	if (!GpuStaticPropRegistry::isEnabled()) return;

	// Compute transform — same coordinate convention as TreeAppearance::update().
	// yaw includes the per-instance yaw offset (matches first-render path exactly).
	float yawAngle = (rotation * DEGREES_TO_RADS) + (yaw * DEGREES_TO_RADS);
	float pitchAngle = (pitch * DEGREES_TO_RADS);
	Stuff::UnitQuaternion rot;
	rot = Stuff::EulerAngles(pitchAngle, yawAngle, 0.0f);
	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y = land ? land->getTerrainElevation(position) : 0.0f;
	xlatPosition.z = position.y;
	treeShape->TransformMultiShape_BuildRecipe(&xlatPosition, &rot);

	// Build per-leaf recipe batch.
	// Use public GetNumShapes()/GetShapeRec() — numTG_Shapes/listOfShapes are protected.
	std::vector<GpuStaticPropInstance> batch;
	const int numShapes = static_cast<int>(treeShape->GetNumShapes());
	batch.reserve(numShapes);
	int t_diag_total=0,t_diag_skip_pm=0,t_diag_skip_h=0,t_diag_skip_unreg=0,t_diag_added=0;
	for (int i = 0; i < numShapes; ++i) {
		++t_diag_total;
		const TG_ShapeRec* rec = treeShape->GetShapeRec(i);
		if (!rec || !rec->processMe || !rec->node) { ++t_diag_skip_pm; continue; }
		TG_Shape* child = rec->node;
		// 2026-05-10 fix: skip non-SHAPE_NODE helpers (mirror of submitMultiShape's
		// filter) — see BldgAppearance::registerStatic for full rationale.
		if (!child->IsShapeNode()) { ++t_diag_skip_h; continue; }
		uint32_t flags = 0;
		if (child->GetLightsOut())   flags |= (1u << 0);
		if (child->GetIsWindow())    flags |= (1u << 1);
		if (child->GetIsSpotlight()) flags |= (1u << 2);
		Stuff::Matrix4D xform(rec->shapeToWorld);
		GpuStaticPropInstance inst;
		if (!GpuStaticPropBatcher::instance().buildRecipeFromShape(
				child, xform,
				static_cast<uint32_t>(child->GetARGBHighlight()),
				static_cast<uint32_t>(child->GetFogRGB()),
				flags, &inst)) {
			++t_diag_skip_unreg;
			return;  // unregistered type — abort; first-render fallback covers it
		}
		batch.push_back(inst);
		++t_diag_added;
	}
	{
		static const bool s_trace = (getenv("MC2_TREE_REG_TRACE") != nullptr);
		static int s_treeRegLogged = 0;
		if (s_trace && s_treeRegLogged < 80) {
			++s_treeRegLogged;
			fprintf(stderr,
				"[TREE_REG_DIAG v1] appearType=%s numShapes=%d total=%d pm_skip=%d "
				"h_skip=%d unreg_skip=%d added=%d\n",
				(appearType ? appearType->name : "<null>"),
				numShapes, t_diag_total, t_diag_skip_pm, t_diag_skip_h,
				t_diag_skip_unreg, t_diag_added);
			fflush(stderr);
		}
		(void)t_diag_total; (void)t_diag_skip_pm; (void)t_diag_skip_h;
		(void)t_diag_skip_unreg; (void)t_diag_added;
	}
	if (batch.empty()) return;

	const int32_t regIdx = GpuStaticPropRegistry::registerRecipe(treeShape, batch);
	if (regIdx >= 0) {
		staticReg.registered  = true;
		staticReg.shape       = treeShape;
		staticReg.recipeIndex = regIdx;
		// H4 fix (2026-05-06): see BldgAppearance::registerStatic for full
		// rationale. registerStatic only ran TransformMultiShape_BuildRecipe
		// (positions only); leaf TG_Shape::lightData_ is still default/zero.
		// needsFullBakeNextFrame = true forces the first post-registration
		// frame through full update() (populating lightData_); subsequent
		// frames proceed via UPDATE_SKIP / static-replay with valid cached data.
		// Spec: docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md
		needsFullBakeNextFrame = true;
	}
}

bool TreeAppearance::isStaticRegistered() const { return staticReg.registered; }

//-----------------------------------------------------------------------------

void TreeAppearance::destroy (void)
{
	invalidateStaticRegistration(); // Stage 3.C: NULL RecipeRange::multi before treeShape is freed

	if ( treeShape )
	{
		delete treeShape;
		treeShape = NULL;
	}

	appearanceTypeList->removeAppearance(appearType);
}


//*****************************************************************************
