/***************************************************************
* FILENAME: EditorObjectMgr.cpp
* DESCRIPTION: Implements Editor object catalog loading, naming, and object management.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Added screen-space single-click object picking while preserving drag-select behavior.
****************************************************************/

#define EDITOROBJECTMGR_CPP
#include "stdafx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Throwaway diagnostic: throttled trace to editor-startup.log. Gated on
// MC2_EDITOR_TRACE env var (same convention as EditorDataTrace in
// EditorData.cpp). ASCII only. Remove with the rest of the editor render
// probes when the object-loop GPU port slice is verified.
#include "../GameOS/gameos/gos_crashbundle.h"
static void EditorObjMgrTrace(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	// Always feed crash ring — visible in last_trace.txt on CTD.
	crashbundle_append(buf);

	if (getenv("MC2_EDITOR_TRACE") == NULL)
		return;
	FILE* f = fopen("editor-startup.log", "a");
	if (!f)
		return;
	fputs(buf, f);
	fputc('\n', f);
	fclose(f);
}

#ifndef BDACTOR_H
#include "bdActor.h"
#endif

#include "EditorObjectMgr.h"
#include "../GameOS/gameos/gos_static_prop_batcher.h"


#ifndef FILE_H
#include "File.h"
#endif

#ifndef UTILITIES_H
#include "Utilities.h"
#endif

#ifndef TERRAIN_H
#include "Terrain.h"
#endif

#ifndef ERR_H
#include "Err.h"
#endif

#ifndef CAMERA_H
#include "Camera.h"
#endif

#ifndef PATHS_H
#include "Paths.h"
#endif

#ifndef CIDENT_H
#include "cident.h"
#endif

#ifndef PACKET_H
#include "Packet.h"
#endif

#ifndef CAMERA_H
#include "camera.h"
#endif

#ifndef BDACTOR_H
#include "bdActor.h"
#endif

#ifndef MECH3D_H
#include "Mech3d.h"
#endif

#ifndef MESSAGEBOX_H
#include "MessageBox.h"
#endif

#ifndef APPRTYPE_H
#include "apprtype.h"
#endif

#ifndef TACMAP_H
#include "tacMap.h"
#endif

#ifndef EDITORDATA_H
#include "EditorData.h"
#endif

#include "resource.h"

#include "McLibResource.h"

#include "BuildingLink.h"
#include "Utilities.h"

#include "malloc.h"

#include "assert.h"

// ARM
#include "../ARM/Microsoft.Xna.Arm.h"
#include "EditorResourceFallback.h"
#include "EditorResourceCatalog.h"
#include "../GameOS/gameos/gos_static_prop_registry.h"
#include "../GameAdapters/MechRenderAdapter.h"
using namespace Microsoft::Xna::Arm;

EditorObjectMgr*	EditorObjectMgr::s_instance = NULL;
extern HSTRRES gameResourceHandle;

extern bool renderObjects;
extern bool renderTrees;

extern bool justResaveAllMaps;

#define TREE_GROUP 1

// by Methuselas: the legacy "mc2res.dll:N Not defined" sentinel means missing
// Editor catalog text. Treat it as absent data and generate safe UI names;
// never display the sentinel as a real object or group label.
static bool EditorObjectMgr_IsMissingEditorText(const char* text)
{
	if (!text || !text[0])
		return true;

	return (strstr(text, "mc2res.dll:") != NULL) ||
		   (strstr(text, "Not defined") != NULL);
}

static void EditorObjectMgr_CopySafeName(char* dst, long dstLen, const char* src, const char* fallbackPrefix, long fallbackValue)
{
	if (!dst || dstLen <= 0)
		return;

	if (src && src[0] && !EditorObjectMgr_IsMissingEditorText(src))
	{
		strncpy(dst, src, dstLen - 1);
		dst[dstLen - 1] = 0;
		return;
	}

	_snprintf(dst, dstLen, "%s_%ld", fallbackPrefix ? fallbackPrefix : "Unnamed", fallbackValue);
	dst[dstLen - 1] = 0;
}


inline float agsqrt( float _a, float _b )
{
	return sqrt(_a*_a + _b*_b);
}

static bool EditorObjectMgr_PointInsideAppearance(ObjectAppearance* pAppearance, int screenX, int screenY, bool allowPerPoly)
{
	if (!pAppearance)
		return false;

	int left = pAppearance->upperLeft.x;
	int top = pAppearance->upperLeft.y;
	int right = pAppearance->lowerRight.x;
	int bottom = pAppearance->lowerRight.y;

	if (right < left)
	{
		int tmp = left;
		left = right;
		right = tmp;
	}

	if (bottom < top)
	{
		int tmp = top;
		top = bottom;
		bottom = tmp;
	}

	if (right == left && bottom == top)
		return false;

	if (screenX < left || screenX > right || screenY < top || screenY > bottom)
		return false;

	if (allowPerPoly && pAppearance->canBeSeen())
		return pAppearance->PerPolySelect(screenX, screenY) ? true : false;

	return true;
}

static void EditorObjectMgr_ConsiderScreenCenter(EditorObject* pObject, int screenX, int screenY, float& bestDistanceSq, EditorObject*& pBestObject)
{
	if (!pObject || !pObject->appearance())
		return;

	ObjectAppearance* pApp = pObject->appearance();

	Stuff::Vector4D center;
	eye->projectZ(pApp->position, center);

	// Size-aware pick radius: project a world point one extent-radius from the
	// object center and measure the screen-space gap, so large objects (walls,
	// big buildings) are pickable across their whole footprint rather than only
	// within a fixed pixel radius of the projected center. The legacy fixed 32px
	// fallback made click-selection feel unreliable -- it only hit near the
	// center and ignored the (often stale/3D) screen bounds.
	float worldRadius = pApp->getRadius();
	if (worldRadius < 1.0f)
		worldRadius = 1.0f;
	Stuff::Vector3D edgeWorld = pApp->position;
	edgeWorld.x += worldRadius;
	Stuff::Vector4D edge;
	eye->projectZ(edgeWorld, edge);
	float rdx = edge.x - center.x;
	float rdy = edge.y - center.y;
	float screenRadius = sqrtf(rdx * rdx + rdy * rdy);
	// Floor so small or zoomed-out objects stay clickable.
	if (screenRadius < 14.0f)
		screenRadius = 14.0f;

	float dx = center.x - (float)screenX;
	float dy = center.y - (float)screenY;
	float distanceSq = dx * dx + dy * dy;

	if (distanceSq <= screenRadius * screenRadius && distanceSq < bestDistanceSq)
	{
		bestDistanceSq = distanceSq;
		pBestObject = pObject;
	}
}


EditorObjectMgr::EditorObjectMgr()
{ 
	gosASSERT( !s_instance ); 
	s_instance = this;   // should only be one of these
}

//*************************************************************************************************
EditorObjectMgr::~EditorObjectMgr()
{
	clear();

	if ( appearanceTypeList )
		delete appearanceTypeList;
	appearanceTypeList = NULL;

	for( GROUP_LIST::EIterator groupIter = groups.Begin();
			!groupIter.IsDone(); groupIter++ )
	{

		for( EList< Building, Building& >::EIterator buildIter = (*groupIter).buildings.Begin();
			!buildIter.IsDone(); buildIter++ )
		{
			Building teste = (*buildIter);

			if (  (*buildIter ).varNames )
			{
				for ( int i = 0; i < 16; i++ )
				{
					if ( (*buildIter).varNames[i] )
					{
						free ( ( *buildIter).varNames[i] );
						(*buildIter).varNames[i] = NULL;
					}
				}

				//Kinda need to delete this OUTSIDE the above loop.
				// Yum, Yum!
				free ( ( *buildIter ).varNames );
				( *buildIter ).varNames = NULL;
			}
		}
	}
	s_instance = 0;
}

//*************************************************************************************************
void EditorObjectMgr::clear()
{
		for ( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			delete *iter;
		}

		for ( UNIT_LIST::EIterator mIter = units.Begin();
			!mIter.IsDone(); mIter++ )
			{
				Unit* pMech = *mIter;
				delete pMech;
			}

			
		buildings.Clear();
		units.Clear();

		for ( LINK_LIST::EIterator liter = links.Begin(); !liter.IsDone(); liter++ )
			delete *liter;

		links.Clear();

		selectedObjects.Clear();
		selectedObjectsNeedsToBeSynched = false;

		for ( DROP_LIST::EIterator dIter = dropZones.Begin(); !dIter.IsDone(); dIter++ )
			delete *dIter;

		dropZones.Clear();
		
		resetAvailableSquadNums();
		
	for( GROUP_LIST::EIterator groupIter = groups.Begin();!groupIter.IsDone(); groupIter++ )
	{
		for( EList< Building, Building& >::EIterator buildIter = (*groupIter).buildings.Begin();!buildIter.IsDone(); buildIter++ )
		{
			(*buildIter).appearanceType = NULL;
		}
	}

	for ( FOREST_LIST::EIterator fIter = forests.Begin(); !fIter.IsDone(); fIter++ )
	{
		delete ( *fIter );
	}

	forests.Clear();

}

//*************************************************************************************************
void EditorObjectMgr::init( const char* bldgListFileName, const char* objectFileName )
{
	File lstFile;

	lstFile.open( const_cast< char* >(bldgListFileName) );

	unsigned char buffer[512];

	lstFile.readLine( buffer, 512 );
	PacketFile objectFile;

	long result = objectFile.open( const_cast< char* >(objectFileName) );
	gosASSERT(result == NO_ERR);
		

	while( lstFile.getLogicalPosition() < lstFile.getLength() )
	{
		
		
		lstFile.readLine( buffer, 512 );

		if ( !strlen( (char*)buffer ) )
			break;

		Building bldg;
		memset(&bldg, 0, sizeof(Building));
		strcpy(bldg.name, "Unnamed_Object");
		strcpy(bldg.fileName, "");
		strcpy(bldg.tgaName, "NONE");
		bldg.appearanceType = NULL;
		bldg.specialType = UNSPECIAL;
		bldg.varNames = 0;
		bldg.forestId = 0;
		bldg.scale = 1.0f;
		
		char tmpBuffer[64];
		int  bufferLength = 64;
		unsigned char* pCur = buffer;
		char GroupName[64];
		strcpy(GroupName, "Group_0");

		// extract the file name
		if (ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
			strcpy( bldg.fileName, tmpBuffer );

		// extract the group
		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			int group = atoi( tmpBuffer );

			// Object menu group labels are editor-sandbox resources now.
			// Do not require mc2res.dll for the Objects menu.
			if (!EditorResourceCatalog::GetObjectGroupNameByIndex(group, GroupName, 64) || !GroupName[0])
				EditorSafeLoadString( IDS_GROUPS + group, GroupName, 64, gameResourceHandle );

			EditorObjectMgr_CopySafeName(GroupName, 64, GroupName, "Group", group);
		}
	
		// extract the building name
		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			int strId = atoi( tmpBuffer );
			bldg.nameID = strId;
			EditorSafeLoadString( strId, tmpBuffer, 64 );
			EditorObjectMgr_CopySafeName(bldg.name, 64, tmpBuffer, bldg.fileName[0] ? bldg.fileName : "Object", strId);
		}

		// extract the type
		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			if ( stricmp( tmpBuffer, "BUILDING" ) == 0 )
				bldg.type = BLDG_TYPE;
			else if ( stricmp( tmpBuffer, "TREE" ) == 0 )
				bldg.type = TREED_TYPE;
			else if ( stricmp( tmpBuffer, "VEHICLE" ) == 0 )
				bldg.type = GV_TYPE;
			else 
				bldg.type = MECH_TYPE;
		}

		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			bldg.fitID = atoi( tmpBuffer );
			if ( NO_ERR != objectFile.seekPacket(bldg.fitID) )
				gosASSERT( false );

			int fileSize = objectFile.getPacketSize();

			FitIniFile bldgFile;
			result = bldgFile.open(&objectFile, fileSize);

			unsigned long lowTemplate, highTemplate;
			bldgFile.readIdULong( "LowTemplate", lowTemplate );
			bldgFile.readIdULong( "HighTemplate", highTemplate );

			bldg.impassability = ((unsigned __int64)highTemplate << 32) | (unsigned __int64)lowTemplate;
			bldgFile.readIdBoolean( "BlocksLineOfFire", bldg.blocksLineOfFire );
			bldgFile.seekBlock("ObjectClass");				
			bldgFile.readIdLong("ObjectTypeNum", bldg.objectTypeNum);
			bldgFile.seekBlock("General");				
			bldg.isHoverCraft = false;
			bldgFile.readIdBoolean("HoverCraft", bldg.isHoverCraft);
			bldgFile.seekBlock("ObjectType");				
			char tmpBuf[2000];
			strcpy(tmpBuf, "");
			bldgFile.readIdString("Name", tmpBuf, 1000);
			if (EditorObjectMgr_IsMissingEditorText(bldg.name) && tmpBuf[0])
				EditorObjectMgr_CopySafeName(bldg.name, 64, tmpBuf, bldg.fileName[0] ? bldg.fileName : "Object", bldg.fitID);
			if (0 == strcmp(tmpBuf, "monsoon")) {
				int i = 11;
			}
			bldgFile.close();
		}

			
		// extract the special type type
		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			if ( strstr( tmpBuffer, "TREE" ) )
			{
				bldg.forestId = atoi( tmpBuffer + 4 );
			}
			else if ( stricmp( tmpBuffer, "GATE" ) == 0 )
				bldg.specialType = EDITOR_GATE;
			else if ( stricmp( tmpBuffer, "GATE_CONTROL" ) == 0 )
				bldg.specialType = GATE_CONTROL;
			else if ( stricmp( tmpBuffer, "POWER_GENERATOR" ) == 0 )
				bldg.specialType = POWER_STATION;
			else if ( stricmp( tmpBuffer, "SENSOR_CONTROL" ) == 0 )
				bldg.specialType = SENSOR_CONTROL;
			else if ( stricmp( tmpBuffer, "SENSOR" ) == 0 )
				bldg.specialType = SENSOR_TOWER;
			else if ( stricmp( tmpBuffer, "SPOTLIGHT_CONTROL" ) == 0 )
				bldg.specialType = SPOTLIGHT_CONTROL;
			else if ( stricmp( tmpBuffer, "SPOTLIGHT" ) == 0 )
				bldg.specialType = SPOTLIGHT;
			else if ( stricmp( tmpBuffer, "LOOKOUT" ) == 0 )
				bldg.specialType = LOOKOUT;
			else if ( stricmp( tmpBuffer, "TURRET_CONTROL" ) == 0 )
				bldg.specialType = TURRET_CONTROL;
			else if ( stricmp( tmpBuffer, "TURRET_GENERATOR" ) == 0 )
				bldg.specialType = TURRET_GENERATOR;
			else if ( stricmp( tmpBuffer, "BRIDGE_CONTROL" ) == 0 )
				bldg.specialType = BRIDGE_CONTROL;
			else if ( stricmp( tmpBuffer, "BRIDGE" ) == 0 )
				bldg.specialType = EDITOR_BRIDGE;		
			else if ( stricmp( tmpBuffer, "DROPZONE" ) == 0 )
				bldg.specialType = DROPZONE;
			else if ( stricmp( tmpBuffer, "TURRET" ) == 0 )
				bldg.specialType = EDITOR_TURRET;
			else if ( stricmp( tmpBuffer, "NAVMARKER" ) == 0 )
				bldg.specialType = NAV_MARKER;
			else if ( stricmp( tmpBuffer, "WALL" ) == 0 )
				bldg.specialType = WALL;
			else if ( stricmp( tmpBuffer, "RPBUILDING" ) == 0 )
				bldg.specialType = RESOURCE_BUILDING;
			else if ( stricmp( tmpBuffer, "HELICOPTER" ) == 0 ) 
				bldg.specialType = HELICOPTER;
		}

		bldg.capturable = ExtractNextInt( pCur ) ? true : false;
		bldg.alignable = ExtractNextInt( pCur ) ? true : false;
		bldg.writeOnTacMap = ExtractNextInt( pCur );
		
		if (ExtractNextString(pCur, tmpBuffer, bufferLength))
			strcpy(bldg.tgaName,tmpBuffer);
		else
			strcpy(bldg.tgaName,"NONE");

		ExtractNextFloat( pCur );
		bldg.scale = ExtractNextFloat( pCur );

				// go out and find the variant file
		CSVFile csvFile;
		FullPathFileName csvFileName;
		csvFileName.init(objectPath,bldg.fileName,".csv");

		char varName[256];
		if ( NO_ERR == csvFile.open( csvFileName ) )
		{
			bldg.varNames = (char**) malloc( sizeof( char* ) * 16 );
			for ( int i = 0; i < 16; ++i )
			{
				if ( NO_ERR == csvFile.readString( 23 + 97 * i, 2, varName, 256 ) && strlen( varName ) )
				{
					bldg.varNames[i] = (char *) malloc( sizeof( char ) * ( strlen( varName ) + 1 ) );
					strcpy( bldg.varNames[i], varName );
				}
				else
					bldg.varNames[i] = NULL;

			}
		}


		if ( bldg.specialType == DROPZONE )
		{
			strcpy( dropZoneGroup.name, GroupName );
			dropZoneGroup.buildings.Append( bldg );
		}
		else
		{
			bool bNeedGroup = true;		
			// now need to add this building
			for ( GROUP_LIST::EIterator iter = groups.Begin(); !iter.IsDone(); iter++ )
			{
				if  ( strcmp( (*iter).name, GroupName ) == 0 )
				{
					bNeedGroup = false;
					(*iter).buildings.Append( bldg );
				}
			}
			if ( bNeedGroup )
			{
				Group grp;
				strcpy( grp.name, GroupName );
				grp.buildings.Append( bldg );
				groups.Append( grp );
			}
		}
	}
}



int EditorObjectMgr::ExtractNextString( unsigned char*& pFileLine, char* pBuffer, int bufferLength )
{
	int i = 0;
	for ( ; i < 512; ++i )
	{
		if ( pFileLine[i] == '\n' )
			break;
		else if ( pFileLine[i] == ',' )
			break;
		else if ( pFileLine[i] == NULL )
			break;
	}

	if ( i == 512 )
		return false;

	gosASSERT( i < bufferLength );
	memcpy( pBuffer, pFileLine, i );
	pBuffer[i] = NULL;
	bufferLength = i + 1;
	pFileLine += i + 1;

	return i;

}

//---------------------------------------------------------------------------
long textToLong (const char *num)
{
	long result = 0;
	
	//------------------------------------
	// Check if Hex Number
	char *hexOffset = (char*)strstr(num,"0x");
	if (hexOffset == NULL)
	{
		result = atol(num);
	}
	else
	{
		hexOffset += 2;
		long numDigits = (long)strlen(hexOffset)-1;
		for (int i=0; i<=numDigits; i++)
		{
			if (!isalnum(hexOffset[i]) || (isalpha(hexOffset[i]) && toupper(hexOffset[i]) > 'F'))
			{
				hexOffset[i] = 0;	// we've reach a "wrong" character. Either start of a comment or something illegal. Either way, stop evaluation here.
				break;
			}
		}
		numDigits = (long)strlen(hexOffset)-1;
		long power = 0;
		for (long count = numDigits;count >= 0;count--,power++)
		{
			unsigned char currentDigit = toupper(hexOffset[count]);
			
			if (currentDigit >= 'A' && currentDigit <= 'F')
			{
				result += (currentDigit - 'A' + 10)<<(4*power);
			}
			else if (currentDigit >= '0' && currentDigit <= '9')
			{
				result += (currentDigit - '0')<<(4*power);
			}
			else
			{
				//---------------------------------------------------------
				// There is a digit in here I don't understand.  Return 0.
				result = 0;
				break;
			}
		}
	}

	
	return(result);
}

int EditorObjectMgr::ExtractNextInt( unsigned char*& pFileLine )
{
	char buffer[1024];

	int count = ExtractNextString( pFileLine, buffer, 1024 );

	if ( count > 0 )
	{
		return textToLong( buffer );
	}

	return -1;
}

float EditorObjectMgr::ExtractNextFloat( unsigned char*& pFileLine )
{
	char buffer[1024];

	int count = ExtractNextString( pFileLine, buffer, 1024 );

	if ( count > 0 )
	{
		return atof( buffer );
	}

	return -1;
}


//-------------------------------------------------------------------------------------------------
bool EditorObjectMgr::canAddBuilding( const Stuff::Vector3D& position,
								float rotation,
								unsigned long group,
								unsigned long indexWithinGroup )
{
	
	int realCellI, realCellJ;
	land->worldToCell( position, realCellJ, realCellI );
	
	EditorObject * cellObject = getObjectAtCell( realCellJ, realCellI );

	if ( cellObject )
	{
		if (cellObject->appearance()->rotation == rotation )
			return false;
	}
	
	//Check if we are ON the map!!
	return land->IsEditorSelectTerrainPosition(position);
}

bool EditorObjectMgr::canAddDropZone( const Stuff::Vector3D& position, int alignment, bool bVTol )
{

	int alignmentCount = 0;
	for ( DROP_LIST::EIterator iter = dropZones.Begin(); !iter.IsDone(); iter++ )
	{
		if  (bVTol)
		{
			if ( (*iter)->isVTol() )
				return false;
		}
		else
		{
			if ( (*iter)->getAlignment() == alignment  && !(*iter)->isVTol() )
				alignmentCount++;
		}
	}

	if ( alignmentCount > 3 )
		return false;

	return true;
}

//-------------------------------------------------------------------------------------------------
bool EditorObjectMgr::addBuilding( EditorObject* pObj )
{
	return addBuilding( pObj->appearance()->position, getGroup( pObj->id ), 
		getIndexInGroup( pObj->id ), pObj->appearance()->teamId, pObj->appearance()->rotation ) ?
		true : false;
}

EditorObject* EditorObjectMgr::addDropZone( const Stuff::Vector3D& position, int alignment, bool bVTol )
{
	gosASSERT( (canAddDropZone( position, alignment, bVTol )  ) );
	
	DropZone* pLast = new DropZone( position, alignment, bVTol );
	
	dropZones.Append( pLast ) ;

	ObjectAppearance* pAppear = getAppearance( &dropZoneGroup.buildings.GetHead() );

	pLast->appearInfo->appearance = pAppear;

	
	int cellRow = 0;
	int cellColumn = 0;
	land->worldToCell( position, cellRow, cellColumn );
	pLast->cellRow = cellRow;
	pLast->cellColumn = cellColumn;
	pLast->id = DROPZONE << 24 | -1<< 16 | -1 << 8;

	Stuff::Vector3D realPos;
	land->getCellPos( pLast->cellRow, pLast->cellColumn, realPos );
	long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
	gosASSERT((8 > alignment) && (-1 <= alignment));
	pLast->appearance()->setObjectParameters( realPos, 0, false, alignment,homeRelations[alignment+1]);

	eye->update();
	pLast->appearance()->update();
	pLast->appearance()->setInView( true );
	pLast->appearance()->rotation = 0;
	pLast->appearance()->teamId = alignment;

	return pLast;
}

//-------------------------------------------------------------------------------------------------
EditorObject* EditorObjectMgr::getObjectAtPosition( const Stuff::Vector3D& position )
{
	// World-space match: nearest object whose footprint (extent radius, with a
	// terrain-cell tolerance floor) contains the given world point. The old path
	// projected each object to screen and tested stale screen bounds + PerPolySelect,
	// which mis-targeted in the GL port -- damage/erase/link hit the wrong object or
	// nothing. This mirrors getObjectAtScreenPosition's matching so every brush that
	// uses getObjectAtPosition picks the same object the Select tool does.
	const float pickFloor = 3.0f * ( land ? land->worldUnitsPerVertex : 64.0f );
	float bestDistSq = 1.0e30f;
	EditorObject* pBest = NULL;

	for ( BUILDING_LIST::EIterator iter = buildings.Begin(); !iter.IsDone(); iter++ )
	{
		EditorObject* o = (*iter);
		if ( !o || !o->appearance() ) continue;
		ObjectAppearance* a = o->appearance();
		float dx = a->position.x - position.x;
		float dy = a->position.y - position.y;
		float d = dx * dx + dy * dy;
		float r = a->getRadius(); if ( r < pickFloor ) r = pickFloor;
		if ( d <= r * r && d < bestDistSq ) { bestDistSq = d; pBest = o; }
	}

	for ( UNIT_LIST::EIterator mIter = units.Begin(); !mIter.IsDone(); mIter++ )
	{
		EditorObject* o = (*mIter);
		if ( !o || !o->appearance() ) continue;
		ObjectAppearance* a = o->appearance();
		float dx = a->position.x - position.x;
		float dy = a->position.y - position.y;
		float d = dx * dx + dy * dy;
		float r = a->getRadius(); if ( r < pickFloor ) r = pickFloor;
		if ( d <= r * r && d < bestDistSq ) { bestDistSq = d; pBest = o; }
	}

	for ( DROP_LIST::EIterator dIter = dropZones.Begin(); !dIter.IsDone(); dIter++ )
	{
		EditorObject* o = (*dIter);
		if ( !o || !o->appearance() ) continue;
		float dx = o->getPosition().x - position.x;
		float dy = o->getPosition().y - position.y;
		float d = dx * dx + dy * dy;
		if ( d <= pickFloor * pickFloor && d < bestDistSq ) { bestDistSq = d; pBest = o; }
	}

	return pBest;
}

// Screen-space object pick helper: project the object's ACTUAL world position
// (real elevation) to screen pixels with the GL-correct forward transform
// (Camera::projectForScreenXY -- the same one FoliageRender renders with) and
// match by pixel distance to the click. This replaces the old inverseProject
// (screen->world unproject) pick, which diverged from the status-bar/placement
// transform (screenToGroundPlaneApprox) and, under the default-on LOD-chunk
// terrain path, returned a far/garbage world point on a heightfield-raycast miss
// -- so a click on empty foliage latched onto the nearest mech across the map.
//
// Forward projection is elevation-EXACT (uses position.z, no z=0 parallax) and
// needs no unproject, no t-range guard, and no nearest-vertex fallback. A true
// miss leaves pBest NULL. The per-object pixel tolerance is derived from the
// object's own world radius projected to screen (so big buildings stay clickable
// near and small props don't over-grab far), floored by a base click slop.
// GL-correct forward projection for editor screen-space picking. Projects a
// world point to screen pixels through worldToClipGL (projectModernClipGL),
// which keeps the SIGNED clip.w. The legacy projectForScreenXY default (Legacy
// mode) routes through projectZ, which does screen.w = fabs(rhw) (camera.h:508)
// and so DISCARDS the w-sign: a point BEHIND the camera came back with positive
// w, sailed through the `sp.w <= 1e-4` near-plane guard, and its mirrored
// screen-XY matched the click -> "selecting things behind the camera". Using the
// signed-w GL transform makes the near-plane reject correct. No frustum bool
// cull (a2cebf34): off-screen-but-in-front points get a real screen.xy and are
// filtered by the caller's pixel-distance test. Same transform the scene renders
// with, so the pick lines up with what is on screen.
static bool EditorObjectMgr_ProjectScreenXY_GL(const Stuff::Vector3D& wp, Stuff::Vector4D& sp)
{
	if (!eye)
		return false;
	ModernClipResult r = eye->projectModernClipGL(wp);
	if (r.clip.w <= 1e-4f)            // at/behind the near plane (SIGNED w)
		return false;
	float vmx = 0.f, vmy = 0.f, vax = 0.f, vay = 0.f;
	gos_GetViewport(&vmx, &vmy, &vax, &vay);
	const float ndcX = r.clip.x / r.clip.w;
	const float ndcY = r.clip.y / r.clip.w;
	sp.x = vax + (ndcX * 0.5f + 0.5f) * vmx;
	sp.y = vay + (1.0f - (ndcY * 0.5f + 0.5f)) * vmy;  // GL origin bottom-left -> screen-Y flip
	sp.z = r.clip.z / r.clip.w;
	sp.w = r.clip.w;
	return true;
}

static void EditorObjectMgr_ConsiderScreenPick(EditorObject* pObject,
	float clickX, float clickY, float pixelFloor, float& bestPixSq, EditorObject*& pBest)
{
	if (!pObject || !pObject->appearance())
		return;
	ObjectAppearance* pApp = pObject->appearance();

	Stuff::Vector3D wp = pApp->position;   // real position, real elevation
	Stuff::Vector4D sp;
	if (!EditorObjectMgr_ProjectScreenXY_GL(wp, sp))  // behind near plane (signed w) / no camera
		return;
	if (sp.w <= 1e-4f)                     // at/behind the near plane (defensive; helper checks)
		return;
	if (!(sp.x == sp.x) || !(sp.y == sp.y))  // NaN guard
		return;

	// Per-object screen radius: project a point one world-radius away in X and
	// measure the pixel delta. Falls back to the click-slop floor.
	float tol = pixelFloor;
	float worldR = pApp->getRadius();
	if (worldR > 1.0f)
	{
		Stuff::Vector3D wr = wp;
		wr.x += worldR;
		Stuff::Vector4D sr;
		if (EditorObjectMgr_ProjectScreenXY_GL(wr, sr) && sr.w > 1e-4f
		    && (sr.x == sr.x) && (sr.y == sr.y))
		{
			float rpx = sr.x - sp.x;
			float rpy = sr.y - sp.y;
			float screenR = sqrtf(rpx * rpx + rpy * rpy);
			if (screenR > tol)
				tol = screenR;
		}
	}

	float dx = sp.x - clickX;
	float dy = sp.y - clickY;
	float pixSq = dx * dx + dy * dy;
	if (pixSq <= tol * tol && pixSq < bestPixSq)
	{
		bestPixSq = pixSq;
		pBest = pObject;
	}
}

EditorObject* EditorObjectMgr::getObjectAtScreenPosition( int screenX, int screenY )
{
	// Match objects in SCREEN space by forward-projecting each object's real
	// world position (see EditorObjectMgr_ConsiderScreenPick). This is what the
	// user actually sees on screen, so the pick lines up with the cursor; it does
	// not depend on the fragile screen->world unproject.
	if (!eye)
		return NULL;

	const float clickX = (float)screenX;
	const float clickY = (float)screenY;

	// Base click slop in pixels (objects with a larger projected radius widen it).
	const float pixelFloor = 12.0f;

	float bestPixSq = 1.0e30f;
	EditorObject* pBest = NULL;

	for( BUILDING_LIST::EIterator iter = buildings.Begin(); !iter.IsDone(); iter++ )
		EditorObjectMgr_ConsiderScreenPick(*iter, clickX, clickY, pixelFloor, bestPixSq, pBest);

	for( UNIT_LIST::EIterator mIter = units.Begin(); !mIter.IsDone(); mIter++ )
		EditorObjectMgr_ConsiderScreenPick(*mIter, clickX, clickY, pixelFloor, bestPixSq, pBest);

	for ( DROP_LIST::EIterator dIter = dropZones.Begin(); !dIter.IsDone(); dIter++ )
		EditorObjectMgr_ConsiderScreenPick(*dIter, clickX, clickY, pixelFloor, bestPixSq, pBest);

	return pBest;
}

static bool areCloseEnough(float f1, float f2)
{
	bool retval = false;
	float diff = fabs(f1 - f2);
	float bigger = fabs(f1);
	if (fabs(f2) > bigger) {
		bigger = fabs(f2);
	}
	float proportion_diff = 0.0;
	if (0.0 != bigger) {
		proportion_diff = diff / bigger;
	}
	if ((pow(10.0, -FLT_DIG) * 10.0/*arbitrary*/) > proportion_diff) {
		retval = true;
	}
	return retval;
}

EditorObject* EditorObjectMgr::getObjectAtLocation( float x, float y )
{
	for( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
	{
		if (areCloseEnough((*iter)->getPosition().x, x) && areCloseEnough((*iter)->getPosition().y, y))
		{
			return (*iter);
		}
	}

	for( UNIT_LIST::EIterator mIter = units.Begin(); !mIter.IsDone(); mIter++ )
	{
		if (areCloseEnough((*mIter)->getPosition().x, x) && areCloseEnough((*mIter)->getPosition().y, y))
		{
			return (*mIter);
		}
	}

	for ( DROP_LIST::EIterator dIter = dropZones.Begin(); !dIter.IsDone(); dIter++ )
	{
		// need to change this when we get drop zone art
		if (areCloseEnough((*dIter)->getPosition().x, x) && areCloseEnough((*dIter)->getPosition().y, y))
		{
			return (*dIter);
		}
	}

	return NULL;
}


//-------------------------------------------------------------------------------------------------
EditorObject* EditorObjectMgr::addBuilding( const Stuff::Vector3D& position, unsigned long group,
									 unsigned long indexWithinGroup, int alignment, float rotation,
									 float scale, bool bSnapToCell )
{
	if ( group == 255 )
		return addDropZone( position, alignment, 0 );
	
	int type = getType( group, indexWithinGroup );
	int specialType = getSpecialType( getID( group, indexWithinGroup ) );
	
	EditorObject* info = NULL;
	
	if ( specialType == NAV_MARKER )
	{
		info = new NavMarker;	
		buildings.Append( info );
		info->appearInfo->appearance = getAppearance( group, indexWithinGroup );
	}
	else if ( (type == BLDG_TYPE) || (type == TREED_TYPE) )
	{

		info = new EditorObject;	
		buildings.Append( info );
		info->appearInfo->appearance = getAppearance( group, indexWithinGroup );
	}
	else if ( type == GV_TYPE  || type == MECH_TYPE )
	{
		Unit* tmp = new Unit( alignment );
		units.Append( tmp );
		info = tmp;
		info->appearInfo->appearance = getAppearance( group, indexWithinGroup );

		unsigned long c1, c2, c3;
		tmp->getColors(c1, c2, c3);
		tmp->appearance()->resetPaintScheme(c1, c2, c3);
	}
	
	int cellRow = 0;
	int cellColumn = 0;
	land->worldToCell( position, cellRow, cellColumn );
	info->cellRow = cellRow;
	info->cellColumn = cellColumn;
	info->id = type << 24 | group << 16 | indexWithinGroup << 8;

	Stuff::Vector3D realPos;
	land->getCellPos( info->cellRow, info->cellColumn, realPos );
	long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
	gosASSERT((8 > alignment) && (-1 <= alignment));
    Stuff::Vector3D pos = bSnapToCell ? realPos : position;
	info->appearance()->setObjectParameters( pos, 0, false, alignment,homeRelations[alignment+1]);

	eye->update();
	info->appearance()->update();
	info->appearance()->setInView( true );
	info->appearance()->rotation = rotation;
	info->appearance()->teamId = isAlignable( info->id ) ? alignment : EDITOR_TEAMNONE;
	if ( scale != 1.0 )
		info->appearance()->scale( scale );


	return info;
}

//*************************************************************************************************
bool	EditorObjectMgr::deleteBuilding( const EditorObject* pInfo )
{
	BuildingLink* pLink = getLinkWithParent( pInfo );
	if ( pLink )
	{
		deleteLink( pLink );
	}
	{
		pLink = getLinkWithBuilding( pInfo );
		if ( pLink )
		{
			pLink->RemoveObject( pInfo );
		}
	}
	if ( buildings.Count() && pInfo )
	{
		for ( BUILDING_LIST::EIterator iter = buildings.End(); !iter.IsDone(); iter--  )
		{
			if( pInfo->cellColumn == (*iter)->cellColumn
				&& pInfo->cellRow == (*iter)->cellRow
				&& pInfo->appearance() == (*iter)->appearance() )
			{
				delete (*iter);
				selectedObjects.RemoveIfThere(*iter);
				EditorData::instance->handleObjectInvalidation(*iter);
				buildings.Delete( iter );
				return true;
			}
		}
	}

	if ( units.Count() && pInfo )
	{
		for ( UNIT_LIST::EIterator mIter = units.End(); !mIter.IsDone(); mIter-- )
		{
			if( pInfo->cellColumn == (*mIter)->cellColumn
				&& pInfo->cellRow == (*mIter)->cellRow )
			{
				delete( *mIter );
				selectedObjects.RemoveIfThere(*mIter);
				EditorData::instance->handleObjectInvalidation(*mIter);
				units.Delete( mIter );
				return true;
			}		
		}
	}

	for ( DROP_LIST::EIterator dIter = dropZones.Begin(); !dIter.IsDone(); dIter++ )
	{
			if( pInfo->cellColumn == (*dIter)->cellColumn
				&& pInfo->cellRow == (*dIter)->cellRow )
			{
				delete( *dIter );
				selectedObjects.RemoveIfThere(*dIter);
				EditorData::instance->handleObjectInvalidation(*dIter);
				dropZones.Delete( dIter );
				return true;
			}		
	
	}

	return false;
}


//*************************************************************************************************
int EditorObjectMgr::getBuildingGroupCount() const
{
	return groups.Count();
}

//*************************************************************************************************
int EditorObjectMgr::getNumberBuildingsInGroup( int group ) const
{
	if ( group < 0 || group >= groups.Count() )
		return 0;

	return groups[group].buildings.Count();
}

//*************************************************************************************************
void EditorObjectMgr::getBuildingGroupNames( const char** names, int& NumberOfNames ) const
{
	if ( NumberOfNames < groups.Count() )
		NumberOfNames = groups.Count();

	int i = 0;
	for ( GROUP_LIST::EConstIterator iter = groups.Begin();
		!iter.IsDone(); iter++, i++ )
		{
			if ( i >= NumberOfNames )
				return;
			
			names[i] = (*iter).name;
		}
}

//*************************************************************************************************
void EditorObjectMgr::getBuildingNamesInGroup( int group, const char** names, int& NumberOfNames ) const
{
	if ( group < 0 || group >= groups.Count() )
	{
		NumberOfNames = 0;
		return;
	}

	Group* pGroup = &groups[group];
	
	if ( NumberOfNames < pGroup->buildings.Count() )
		NumberOfNames = pGroup->buildings.Count();

	int i = 0;
	for ( EList< Building, Building& >::EIterator iter = pGroup->buildings.Begin();
		!iter.IsDone(); iter++, i++ )
		{
			if ( i >= NumberOfNames )
				return;
			
			names[i] = (*iter).name;
		}

	NumberOfNames = i;
}

// --------------------------------------------------------------------------
// GV-slide diagnostics + fix knobs (editor-only).
//
// Placed ground-vehicles (VEHICLE_APPR_TYPE) ride GpuStaticPropBatcher, which
// draws them every frame with the LIVE terrain MVP while reading each GV's
// last-baked shapeToWorld (gvactor.cpp TransformMultiShape, run only from
// GVAppearance::update()->updateGeometry()).  The editor gated that refresh
// behind recalcBounds()==inView, so a cluster of GVs swinging to the frustum
// edge on camera rotate skipped update() and rendered with a stale,
// terrain-conformed transform -> the group slid off the ground.  Buildings
// don't (static, no per-frame terrain conform); mechs use a different batcher.
//
// MC2_EDITOR_MOVER_REFRESH (default-ON): refresh placed-mover transforms every
//   frame regardless of inView, matching what BldgAppearance already does.
//   Set =0 to restore the legacy inView-gated behavior (bisect / A-B).
// MC2_EDITOR_MOVER_TRACE (default-OFF): per-frame recalcBounds() pass/fail
//   counts split GV-vs-mech across the update and render loops, to confirm the
//   mechanism.  Emitted once per render() frame at the trace throttle points.
static bool editorMoverRefreshOn()
{
	static int v = -1;
	if (v < 0) { const char* e = getenv("MC2_EDITOR_MOVER_REFRESH");
		v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1; }   // default-on; exactly "0" disables
	return v == 1;
}
static bool editorMoverTraceOn()
{
	static int v = -1;
	if (v < 0) { const char* e = getenv("MC2_EDITOR_MOVER_TRACE");
		v = (e && e[0] && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0; }  // default-off
	return v == 1;
}
// per-frame counters: update() fills *Upd*, render() fills *Rnd*; logged in render().
static long s_dbgUpdGvIn = 0, s_dbgUpdGvOut = 0, s_dbgUpdMechIn = 0, s_dbgUpdMechOut = 0;
static long s_dbgRndGvIn = 0, s_dbgRndGvOut = 0, s_dbgRndMechIn = 0, s_dbgRndMechOut = 0;

//*************************************************************************************************
void EditorObjectMgr::update()
{
	static long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
	if (renderObjects)
	{
		for ( BUILDING_LIST::EIterator iter = buildings.Begin();
			!iter.IsDone(); iter++ )
			{
				currentFloatHelp = 0;
				// Guard: objects with no loaded appearance are skipped.
				ObjectAppearance* pBAppr = (*iter) ? (*iter)->appearance() : NULL;
				if (!pBAppr) continue;
				if (renderTrees || (!renderTrees && (pBAppr->getAppearanceClass() != TREE_APPR_TYPE)))
				{
					if ( pBAppr->recalcBounds() )
					{
						Stuff::Vector3D pos = pBAppr->position;
						// Keep the building level with the terrain WITHOUT snapping it to the
						// cell grid. The legacy path called moveBuilding(worldToCell(pos)) here,
						// which re-derived x,y from the cell CENTRE every frame -- making free
						// (sub-cell) placement impossible (a dragged building snapped straight
						// back to tile intersections). We only need to refresh z to the terrain
						// height at the building's (possibly sub-cell) x,y.
						pos.z = land->getTerrainElevation(pos);
						pBAppr->position = pos;
						gosASSERT((8 > pBAppr->teamId) && (-1 <= pBAppr->teamId));
						pBAppr->setObjectParameters(pos, pBAppr->rotation, pBAppr->selected, pBAppr->teamId, homeRelations[pBAppr->teamId+1]);
						pBAppr->update();
						pBAppr->setVisibility(true,true);
					}
				}
			}
	}

	//Always draw the Mechs/Vehicles
		// Reset per-frame mover trace counters (update() runs before render() each frame).
		s_dbgUpdGvIn = s_dbgUpdGvOut = s_dbgUpdMechIn = s_dbgUpdMechOut = 0;
		s_dbgRndGvIn = s_dbgRndGvOut = s_dbgRndMechIn = s_dbgRndMechOut = 0;
		const bool moverRefresh = editorMoverRefreshOn();
		const bool moverTrace   = editorMoverTraceOn();
		for ( UNIT_LIST::EIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
		{
			EditorObject* pObj = (*mIter);
			currentFloatHelp = 0;
			// Guard: units with no loaded appearance are skipped.
			ObjectAppearance* pUAppr = pObj ? pObj->appearance() : NULL;
			if (!pUAppr) continue;
			const bool uInView = pUAppr->recalcBounds();
			if (moverTrace)
			{
				const bool isGv = (pUAppr->getAppearanceClass() == VEHICLE_APPR_TYPE);
				if (isGv) { if (uInView) ++s_dbgUpdGvIn;   else ++s_dbgUpdGvOut; }
				else      { if (uInView) ++s_dbgUpdMechIn; else ++s_dbgUpdMechOut; }
			}
			// GV-slide fix: refresh the mover transform every frame (not only when
			// inView), so static-prop-batched GVs never render with a shapeToWorld
			// stale from a frame they swung off the rotating frustum.  Visibility is
			// still only asserted when actually in view.
			if ( uInView || moverRefresh )
			{
				// Vehicles and mech forced to ground in Object classes in game now.
				// NOT in the appearance anymore since they can fly when they die!
				// Must force here as well.
				Stuff::Vector3D pos = pUAppr->position;
				pos.z = land->getTerrainElevation(pos);
				if (getIsHoverCraft(pObj->id)) {
					float waterElevation = land->getWaterElevation();
					if (waterElevation > pos.z) {
						pos.z = waterElevation;
					}
				}
				gosASSERT((8 > pUAppr->teamId) && (-1 <= pUAppr->teamId));
				pUAppr->setObjectParameters(pos, pUAppr->rotation, pUAppr->selected, pUAppr->teamId, homeRelations[pUAppr->teamId+1]);
				if (pObj->getSpecialType() == HELICOPTER)
					pUAppr->setMoverParameters(0.0f,0.0f,0.0f,true);
				pUAppr->update();
				if ( uInView )
					pUAppr->setVisibility(true,true);
			}
		}
}

//*************************************************************************************************
void EditorObjectMgr::render()
{
	// S2.5 (2026-05-25): actor walk only. Frame-level orchestration
	// (renderLists, MVP, batcher flush, terrain dispatch) lives in the
	// canonical game render-world chain wired by S2 -- DO NOT re-add here.
	// Each appearance()->render() enqueues into the static-prop / mech
	// batcher; the flush happens later via mcTextureManager->renderLists().
	// See docs/superpowers/plans/2026-05-25-editor-rebuild.md S2 followup.

	static long s_emrFrame = 0;
	++s_emrFrame;
	const bool s_emrLog = (s_emrFrame == 1 || s_emrFrame == 5 || s_emrFrame == 30 || s_emrFrame == 120);
	if (s_emrLog)
		EditorObjMgrTrace("EditorObjectMgr::render frame=%ld renderObjects=%d renderTrees=%d buildings=%ld units=%ld links=%ld dropZones=%ld",
			s_emrFrame, renderObjects ? 1 : 0, renderTrees ? 1 : 0,
			(long)buildings.Count(), (long)units.Count(),
			(long)links.Count(), (long)dropZones.Count());

	long bTreeFiltered = 0;
	long bRecalcPassed = 0;
	long bRendered = 0;
	if (renderObjects)
	{
		if (s_emrLog)
			EditorObjMgrTrace("EditorObjectMgr::render frame=%ld buildings-loop enter count=%ld",
				s_emrFrame, (long)buildings.Count());
		for ( BUILDING_LIST::EIterator iter = buildings.Begin();
			!iter.IsDone(); iter++ )
		{
			currentFloatHelp = 0;
			if (renderTrees || (!renderTrees && ((*iter)->appearance()->getAppearanceClass() != TREE_APPR_TYPE)))
			{
				++bTreeFiltered;
				if ( (*iter)->appearance()->recalcBounds() )
				{
					++bRecalcPassed;
					if ( (*iter)->getDamage() )
						(*iter)->appearance()->drawBars();
					// BldgAppearance::render() submits into the static-prop batcher's
					// per-frame buckets via submitMultiShape() (bdactor.cpp).
					// Flush is issued later from mcTextureManager->renderLists().
					(*iter)->appearance()->render();
					++bRendered;
					if ( (*iter)->isSelected() )
						(*iter)->appearance()->drawSelectBrackets( 0xffffff00 );  // selection highlight (yellow)
					else if ( (*iter)->getColor() & 0xff000000 )
						(*iter)->appearance()->drawSelectBrackets( (*iter)->getColor() );
				}
			}
		}
		if (s_emrLog)
			EditorObjMgrTrace("EditorObjectMgr::render frame=%ld buildings-loop exit treeFiltered=%ld recalcPassed=%ld rendered=%ld",
				s_emrFrame, bTreeFiltered, bRecalcPassed, bRendered);
	}
	else if (s_emrLog)
	{
		EditorObjMgrTrace("EditorObjectMgr::render frame=%ld buildings-loop SKIPPED (renderObjects=false)", s_emrFrame);
	}

	// Always draw the Mechs/Vehicles
	long uRecalcPassed = 0;
	long uRendered = 0;
	if (s_emrLog)
		EditorObjMgrTrace("EditorObjectMgr::render frame=%ld units-loop enter count=%ld",
			s_emrFrame, (long)units.Count());
	const bool moverTrace = editorMoverTraceOn();
	for ( UNIT_LIST::EIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
	{
		EditorObject* pObj = (*mIter);
		currentFloatHelp = 0;
		// Capture recalcBounds() ONCE (it has LOD-swap / inView side effects;
		// calling it twice would re-swap and re-invalidate the baked transform).
		const bool rInView = pObj->appearance()->recalcBounds();
		if (moverTrace)
		{
			const bool isGv = (pObj->appearance()->getAppearanceClass() == VEHICLE_APPR_TYPE);
			if (isGv) { if (rInView) ++s_dbgRndGvIn;   else ++s_dbgRndGvOut; }
			else      { if (rInView) ++s_dbgRndMechIn; else ++s_dbgRndMechOut; }
		}
		if ( rInView )
		{
			++uRecalcPassed;
			// Mech3DAppearance::render() submits into the mech batcher via
			// submitActor() (mech3d.cpp). Flush is via renderLists().
			pObj->appearance()->render();
			++uRendered;
			if ( (*mIter)->getDamage() )
				pObj->appearance()->drawBars();
			if ( (*mIter)->isSelected() )
				pObj->appearance()->drawSelectBrackets( 0xffffff00 );  // selection highlight (yellow)
			else if ( (*mIter)->getColor() & 0xff000000 )
				pObj->appearance()->drawSelectBrackets( pObj->getColor() );
		}
	}
	if (s_emrLog)
		EditorObjMgrTrace("EditorObjectMgr::render frame=%ld units-loop exit recalcPassed=%ld rendered=%ld",
			s_emrFrame, uRecalcPassed, uRendered);
	if (moverTrace)
		EditorObjMgrTrace("[MoverTrace] frame=%ld GV upd(in=%ld out=%ld) rnd(in=%ld out=%ld) | MECH upd(in=%ld out=%ld) rnd(in=%ld out=%ld)",
			s_emrFrame, s_dbgUpdGvIn, s_dbgUpdGvOut, s_dbgRndGvIn, s_dbgRndGvOut,
			s_dbgUpdMechIn, s_dbgUpdMechOut, s_dbgRndMechIn, s_dbgRndMechOut);

	// Building links -- 2D overlay, NOT batcher scope.
	for ( LINK_LIST::EIterator lIter = links.Begin();
		!lIter.IsDone(); lIter++ )
	{
		(*lIter)->render();
	}

	// Drop zones -- 2D overlay, NOT batcher scope.
	for ( DROP_LIST::EIterator dIter = dropZones.Begin();
		!dIter.IsDone(); dIter++ )
	{
		Stuff::Vector3D pos = (*dIter)->getPosition();
		Stuff::Vector4D screen;

		eye->projectZ( pos, screen );

		GUI_RECT Rect;
		Rect.top = screen.y - 3;
		Rect.left = screen.x - 3;
		Rect.bottom = screen.y + 3;
		Rect.right = screen.x + 3;
		drawRect( Rect, (*dIter)->isVTol() ? 0xff0000ff : 0xffffff00 );
	}
}

//*************************************************************************************************
bool EditorObjectMgr::save( PacketFile& PakFile, int whichPacket )
{
	File file;
	
	if ( !buildings.Count() )
	{
		PakFile.writePacket( whichPacket, NULL );
		PakFile.writePacket( whichPacket + 1, NULL );
	}

	int bufferSize = buildings.Count() * (4 * sizeof(float) + 6 * sizeof(long) ) + 2 * sizeof(long);
	char* pBuffer = (char *) malloc( bufferSize );
	long* pTacMapPoints = (long *)malloc( sizeof(long) * ( buildings.Count() * 2 + 1 ) );
	long* pPoints = pTacMapPoints + 1;
	long pointCounter = 0;
	
	file.open(pBuffer, bufferSize );
	file.writeLong( buildings.Count() );

	gos_VERTEX point;
	
	for ( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			int id = (*iter)->id;
			Building& bldg = groups[(id >> 16) & 0x000000ff].buildings[(id >> 8) & 0x000000ff];

			if ( bldg.writeOnTacMap )
			{
				TacMap::worldToTacMap( (*iter)->appearance()->position, 0, 0, 115, 115, point );
				*pPoints++ = point.x;
				*pPoints++ = point.y;
				pointCounter++;
			}
			
			file.writeLong( bldg.fitID );
			file.writeFloat( (*iter)->appearance()->position.x );
			file.writeFloat( (*iter)->appearance()->position.y );
			file.writeFloat( (*iter)->appearance()->position.z );

			file.writeFloat( (*iter)->appearance()->rotation );

			file.writeLong( (*iter)->appearance()->damage );

			
			file.writeLong( (*iter)->appearance()->teamId );
			BuildingLink* pLink = getLinkWithChild(*iter);
			if ( pLink )
			{
				Stuff::Vector3D pos = pLink->GetParentPosition();
				// convert to cells
				int cellI, cellJ;
				land->worldToCell( pos, cellJ, cellI );

				long val = (cellJ << 16) | cellI;

				//How about BEFORE we write this, we make sure there's really a building there!
				EditorObject* pObject = getObjectAtCell( cellJ, cellI );
				if (!pObject)
				{
					//Dissolve the link here.  Maybe we should warn?
					val = 0xffffffff;
					PAUSE(("Turret/Gate linked to bldg @ R %d, C %d  X:%f Y:%f No Bldg there!  Link Deleted",cellJ,cellI,pos.x,pos.y));
				}

				file.writeLong( val );
			}
			else
				file.writeLong( 0xffffffff );
			
			// forest stuff
			file.writeLong( (*iter)->getForestID() + 1 );
			if ( (*iter)->getForestID() != -1 )
				file.writeLong( (*iter)->getScale()  );
			else
				file.writeLong( 0 );
		}

	int pos = file.getLogicalPosition();
	file.seek( 0 );

	if ( !PakFile.writePacket( whichPacket, (unsigned char*)pBuffer, pos ) )
	{
		gosASSERT( false );
	}

	*pTacMapPoints = pointCounter;

	PakFile.writePacket( whichPacket + 1, (BYTE*)pTacMapPoints, ( pointCounter * 2 + 1 ) * sizeof( long ) );

	free( pTacMapPoints );
	free( pBuffer );

	if (!justResaveAllMaps) {
		int numTurrets = 0;
		for ( BUILDING_LIST::EIterator iter = buildings.Begin(); !iter.IsDone(); iter++ )
		{
			switch ((*iter)->getSpecialType())
			{
			case EDITOR_TURRET:
				{
					numTurrets += 1;
				}
				break;
			}
		}
		if (64/*max turrets*/ < numTurrets )
		{
			CString CStr;
			CStr.Format(IDS_TOO_MANY_TURRETS, numTurrets);
			AfxMessageBox(CStr);
		}
	}

	return true;
}

bool EditorObjectMgr::load( PacketFile& PakFile, int whichPacket )
{
	PakFile.seekPacket( whichPacket );
	int size = PakFile.getPacketSize( );
	char* pBuffer = (char *)malloc(size);
	PakFile.readPacket( whichPacket, (unsigned char*)pBuffer );
	
	File file;
	file.open( pBuffer, size );
	int count = file.readLong();

	for ( int i = 0; i < count; ++i )
	{
		int id = file.readLong();

		Stuff::Vector3D vector;
		vector.x = file.readFloat();
		vector.y = file.readFloat();
		vector.z = file.readFloat();

		float rotation = file.readFloat();
		int damage = file.readLong();
		int teamId = file.readLong();
		int cells = file.readLong();
		int forestId = file.readLong() - 1;
		long scale = file.readLong();

		unsigned long group, index;
		if ( !getBuildingFromID(id, group, index, false ) )
		{
			// Unknown object ids are valid in modded/remastered sandboxes.
			// The packet record has already been consumed above; skipping here
			// keeps the stream aligned for the remaining objects.
			continue;
		}

		EditorObject* pInfo = (EditorObject*)addBuilding( vector, group, index, EDITOR_TEAMNONE, rotation );
		if ( !pInfo || !pInfo->appearance() )
		{
			continue;
		}

		pInfo->appearance()->rotation = rotation;

		if ( damage )
			pInfo->appearance()->setDamage(1);

		pInfo->appearance()->teamId = teamId;
		//Most everything can have an alignment now
		// -fs  6/14/2000
//		if ( !isAlignable( pInfo->id ) )
//			pInfo->appearance()->teamId = -1;

		if ( cells && (cells != 0xffffffff)) // this building is linked
		{
			long cellJ = cells >> 16;
			long cellI = cells & 0x0000ffff;

			Stuff::Vector3D pos;

			land->cellToWorld( cellJ, cellI, pos );
			pos.z = land->getTerrainElevation( pos );

			bool bFound = false;
			for ( LINK_LIST::EIterator iter = links.Begin();
			!iter.IsDone(); iter++ )
			{
				if ( (*iter)->GetParentPosition() == pos )
				{
					// we've got the link
					(*iter)->AddChild( pInfo );
					bFound = true;
					break;
				}
			}

			if ( !bFound )
			{
				BuildingLink* pLink = new BuildingLink( pos );
				pLink->AddChild( pInfo );
				links.Append( pLink );       
			}
		}

		// forest stuff
		pInfo->setForestID( forestId );
		if ( pInfo->getForestID() != -1 )
			pInfo->setScale( scale );

	}

	LINK_LIST linksToDelete;

	// gotta go through all the links and make sure they have id's
	for ( LINK_LIST::EIterator iter = links.Begin();
			!iter.IsDone(); iter++ )
			{
				if ( (*iter)->parent.m_ID == -1 )
				{
					// This absolutely positively could never have worked!
					// Why?  Because getObjectAtPosition depends upon the object BEING ON SCREEN!!!!!!!
					// At this point, we're still loading!!!
					// We should, instead, be checking getObjectAtCell which ALWAYS works!
					// -fs
					int cellR, cellC;
					land->worldToCell((*iter)->parent.pos,cellR,cellC);

					EditorObject* pObject = getObjectAtCell( cellR, cellC );
					gosASSERT( pObject );

					if ( pObject )
						(*iter)->parent.m_ID = pObject->getID();
					else
						linksToDelete.Append( *iter );
				}
			}

	for ( LINK_LIST::EIterator iter = linksToDelete.Begin();	!iter.IsDone(); iter++ )
	{
		deleteLink( *iter );
	}
	


	free( pBuffer );

	return true;

}

//*************************************************************************************************
// this will NOT RETURN MECHS AND VEHICLES
bool EditorObjectMgr::getBuildingFromID( int fitID, unsigned long& group, unsigned long& index, bool canBeMech )
{
	group = 0;
	index = 0;
	
	for( GROUP_LIST::EIterator groupIter = groups.Begin();
			!groupIter.IsDone(); groupIter++ )
			{
				index = 0;

				for( EList< Building, Building& >::EIterator buildIter = (*groupIter).buildings.Begin();
					!buildIter.IsDone(); buildIter++ )
					{
						if ( (*buildIter).fitID == fitID &&
								( canBeMech || 
									(*buildIter).type != GV_TYPE
								 && (*buildIter).type != MECH_TYPE )  )
						{
							return true;
						}

						index++;
					}

					group++;
			}

	return false;
}

//*************************************************************************************************
int	EditorObjectMgr::getFitID( int id ) const
{
	return groups[getGroup(id)].buildings[getIndexInGroup(id)].fitID;
}


//*************************************************************************************************
bool EditorObjectMgr::getBlocksLineOfFire( int id ) const
{
	return groups[getGroup(id)].buildings[getIndexInGroup(id)].blocksLineOfFire;

}
//*************************************************************************************************
bool EditorObjectMgr::getIsHoverCraft( int id ) const
{
	return groups[getGroup(id)].buildings[getIndexInGroup(id)].isHoverCraft;

}
//*************************************************************************************************
__int64	EditorObjectMgr::getImpassability( int id )
{
	return groups[getGroup(id)].buildings[getIndexInGroup(id)].impassability;
}

void EditorObjectMgr::select( const Stuff::Vector4D& pos1, const Stuff::Vector4D& pos2 )
{
	int xMin, xMax;
	int yMin, yMax;
	
	if ( pos1.x < pos2.x )
	{
		xMin = pos1.x;
		xMax = pos2.x;
	}
	else
	{
		xMin = pos2.x;
		xMax = pos1.x;
	}

	if ( pos1.y < pos2.y )
	{
		yMin = pos1.y;
		yMax = pos2.y;
	}
	else
	{
		yMin = pos2.y;
		yMax = pos1.y; 
	}

	Stuff::Vector4D screenPos;

	for ( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			EditorObject* pObject = (*iter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( !pAppearance )
				continue;

			eye->projectZ( pAppearance->position, screenPos );
			if ( screenPos.x >= xMin && screenPos.x <= xMax			
				 && screenPos.y >= yMin && screenPos.y <= yMax )
			{
				pAppearance->selected = true;
				selectedObjects.AddUnique(pObject);
			}
		}

		for ( UNIT_LIST::EIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
		{
			EditorObject* pObject = (*mIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( !pAppearance )
				continue;

			eye->projectZ( pAppearance->position, screenPos );
			if ( screenPos.x >= xMin && screenPos.x <= xMax			
				 && screenPos.y >= yMin && screenPos.y <= yMax )
			{
				pAppearance->selected = true;
				selectedObjects.AddUnique(pObject);
			}
		}

		for ( DROP_LIST::EIterator dIter = dropZones.Begin();
		!dIter.IsDone(); dIter++ )
		{
			EditorObject* pObject = (*dIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( !pAppearance )
				continue;

			Stuff::Vector3D pos = pObject->getPosition();
			eye->projectZ( pos, screenPos );
		
			if ( screenPos.x >= xMin && screenPos.x <= xMax			
				 && screenPos.y >= yMin && screenPos.y <= yMax )
			{
				pAppearance->selected = true;
				selectedObjects.AddUnique(pObject);
			}
			
		}
			 

}

void EditorObjectMgr::select( EditorObject &object, bool bSelect )
{
	ObjectAppearance* pAppearance = object.appearance();
	if ( !pAppearance )
	{
		// by Methuselas: left-click selection is live again under the embedded
		// renderer.  Treat appearance-less legacy objects as non-selectable
		// instead of dereferencing a partially loaded object.
		return;
	}

	// by Methuselas: keep single-click selection on the same render flag path
	// as the now-working drag-select code.  Drag-select writes `true`, which is
	// equivalent to DRAW_BARS.  Avoid DRAW_TEXT because it can enter legacy
	// cLoadString() name rendering, and avoid DRAW_BRACKETS here until the
	// remastered Editor bracket path is separately proven safe for single-click.
	pAppearance->selected = bSelect ? DRAW_BARS : 0;

	if (true == bSelect)
	{
		selectedObjects.AddUnique(&object);
	}
	else 
	{
		selectedObjects.RemoveIfThere(&object);
	}
}

void EditorObjectMgr::unselectAll()
{
	// mh: at somepoint we can change the code so that it iterates over the
	// selectedObjects list instead of the lists of all the buildings and units
	for ( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			EditorObject* pObject = (*iter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( pAppearance )
				pAppearance->selected = false;
		}
		for ( UNIT_LIST::EIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
		{
			EditorObject* pObject = (*mIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( pAppearance )
				pAppearance->selected = false;
		}

		for ( DROP_LIST::EIterator dIter = dropZones.Begin();
		!dIter.IsDone(); dIter++ )
		{
			EditorObject* pObject = (*dIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( pAppearance )
				pAppearance->selected = false;
		}

		selectedObjects.Clear();
}


bool EditorObjectMgr::hasSelection()
{
#if 1
	syncSelectedObjectPointerList();
	if (0 < selectedObjects.Count())
	{
		return true;
	}
#else
	for ( BUILDING_LIST::EConstIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			if ((*iter)->appearance()->selected )				
				return true;
		}
		for ( UNIT_LIST::EConstIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
		{
			if ((*mIter)->appearance()->selected )				
				return true;
		}
#endif

	return false;
}

int EditorObjectMgr::getSelectionCount()
{
#if 1
	syncSelectedObjectPointerList();
	return selectedObjects.Count();
#else
	int counter = 0;
	for ( BUILDING_LIST::EConstIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			if ((*iter)->appearance()->selected )				
				counter++;
		}
		for ( UNIT_LIST::EConstIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
		{
			if ((*mIter)->appearance()->selected )				
				counter++;
		}

	return counter;
#endif
}

EditorObjectMgr::EDITOR_OBJECT_LIST EditorObjectMgr::getSelectedObjectList()
{
	syncSelectedObjectPointerList();
	return selectedObjects;
}

void EditorObjectMgr::syncSelectedObjectPointerList()
{
	if (!selectedObjectsNeedsToBeSynched)
	{
		return;
	}
	selectedObjects.Clear();
	for ( BUILDING_LIST::EConstIterator iter = buildings.Begin()
		;!iter.IsDone(); iter++ )
	{
		const EditorObject* pObject = (*iter);
		const ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
		if (pAppearance && pAppearance->selected )				
			selectedObjects.Append(*iter);
	}
	for ( UNIT_LIST::EConstIterator mIter = units.Begin();
	!mIter.IsDone(); mIter++ )
	{
		const EditorObject* pObject = (*mIter);
		const ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
		if (pAppearance && pAppearance->selected )				
			selectedObjects.Append(*mIter);
	}
	for ( DROP_LIST::EIterator dIter = dropZones.Begin();
	!dIter.IsDone(); dIter++ )
	{
		const EditorObject* pObject = (*dIter);
		const ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
		if (pAppearance && pAppearance->selected )				
			selectedObjects.Append(*dIter);
	}

	selectedObjectsNeedsToBeSynched = false;
}

void EditorObjectMgr::deleteSelectedObjects()
{
	for ( BUILDING_LIST::EIterator iter = buildings.End();
		!iter.IsDone();  )
		{
			if ( (*iter)->appearance()->selected )
			{
				BUILDING_LIST::EIterator tmpIter = iter;
				iter --;
				buildings.Delete( tmpIter );
			}
			else
				iter--;
		}

		for ( UNIT_LIST::EIterator uIter = units.End();
		!uIter.IsDone(); )
		{
			if ( (*uIter)->appearance()->selected )
			{
				UNIT_LIST::EIterator tmpIter = uIter;
				uIter --;
				units.Delete( tmpIter );
			}
			else
				uIter--;
		}

		for ( DROP_LIST::EIterator dIter = dropZones.End();
		!dIter.IsDone(); )
		{
			if ( (*dIter)->appearance()->selected )
			{
				DROP_LIST::EIterator tmpIter = dIter;
				dIter --;
				dropZones.Delete( tmpIter );
			}
			else
				dIter--;
		
		}

		selectedObjects.Clear();
}

void EditorObjectMgr::adjustObjectsToNewTerrainHeights()
{
	for ( BUILDING_LIST::EIterator iter = buildings.End();
		!iter.IsDone(); iter++  )
	{
		float Z = land->getTerrainElevation( (*iter)->appearance()->position );
		(*iter)->appearance()->position.z = Z;
		(*iter)->appearance()->update();
	}

	for ( LINK_LIST::EIterator lIter = links.Begin(); !lIter.IsDone(); lIter++ )
	{
		(*lIter)->FixHeights();	
	}

	for ( UNIT_LIST::EIterator uIter = units.Begin(); !uIter.IsDone(); uIter++ )
	{
		float Z = land->getTerrainElevation( (*uIter)->appearance()->position );
		(*uIter)->appearance()->position.z = Z;
		(*uIter)->appearance()->update();
	}

}

ObjectAppearance* EditorObjectMgr::getAppearance( EditorObjectMgr::Building* pBuilding )
{
	ObjectAppearance* appearance = NULL;

	if ( !appearanceTypeList )
	{
		appearanceTypeList = new AppearanceTypeList;
		gosASSERT(appearanceTypeList != NULL);
		
		appearanceTypeList->init(81920000);   // 10x the real engine's spriteHeapSize (8192000); editor edits many appearances/forests
	}

	//MUST ALWAYS CALL GET, EVEN IF WE HAVE AN APPEARANCE TYPE OR REFERENCE COUNT DOES NOT INCREASE!
	pBuilding->appearanceType = appearanceTypeList->getAppearance( pBuilding->type << 24, pBuilding->fileName );
	
	if ( pBuilding->type == BLDG_TYPE )
		appearance = new BldgAppearance;
	
	else if ( pBuilding->type == MECH_TYPE )
		appearance = new Mech3DAppearance;
		
	else if ( pBuilding->type == GV_TYPE )
		appearance = new GVAppearance;
	
	else
		appearance = new TreeAppearance;


	appearance->init( pBuilding->appearanceType );
	appearance->setObjectNameId( pBuilding->nameID );

	return appearance;

}

ObjectAppearance* EditorObjectMgr::getAppearance( unsigned long group, unsigned long indexWithinGroup )
{
	gosASSERT( group >= 0 && group < groups.Count() );

	Group* pGroup = &groups[group];

	gosASSERT( pGroup->buildings.Count() > indexWithinGroup );
	Building* pBuilding = &pGroup->buildings[indexWithinGroup];

	return getAppearance( pBuilding );

}

// Pre-loads every building AppearanceType from the object catalogue and
// registers its TG_TypeMultiShape(s) with GpuStaticPropBatcher so that all
// object types are available before finalizeGeometry() is called.  Without
// this, any object type not already present on the loaded map hits the
// late-register path (registerType after s_geometryFinalized == true) and
// renders solid black when first placed.
//
// Bridges and other rarely-used building types are the most common victims
// because they are absent from most stock starter maps.
void EditorObjectMgr::primeAllBuildingAppearanceTypes()
{
	EditorObjMgrTrace("EditorObjectMgr::primeAllBuildingAppearanceTypes enter groups=%ld",
		(long)groups.Count());

	if (!appearanceTypeList)
	{
		appearanceTypeList = new AppearanceTypeList;
		gosASSERT(appearanceTypeList != NULL);
		appearanceTypeList->init(2048000);
	}

	long nPrimed = 0;
	for (GROUP_LIST::EIterator groupIter = groups.Begin(); !groupIter.IsDone(); groupIter++)
	{
		Group& grp = *groupIter;
		for (EList<Building, Building&>::EIterator bldgIter = grp.buildings.Begin();
			 !bldgIter.IsDone(); bldgIter++)
		{
			Building& bldg = *bldgIter;

			// Skip types that are not rendered via the GPU static-prop batcher
			// (mechs, vehicles, nav-markers are handled by their own batchers).
			if (bldg.type != BLDG_TYPE && bldg.type != TREED_TYPE)
				continue;

			// Load the AppearanceType if not already cached (idempotent).
			// MUST ALWAYS CALL GET even if non-null — the reference count must
			// be incremented (see comment in getAppearance(Building*)).
			bldg.appearanceType = appearanceTypeList->getAppearance(
				bldg.type << 24, bldg.fileName);

			if (!bldg.appearanceType)
				continue;

			if (bldg.type == BLDG_TYPE)
			{
				BldgAppearanceType* bat = static_cast<BldgAppearanceType*>(bldg.appearanceType);
				for (int lod = 0; lod < MAX_LODS; ++lod)
					GpuStaticPropBatcher::instance().registerMultiShape(bat->bldgShape[lod]);
				GpuStaticPropBatcher::instance().registerMultiShape(bat->bldgDmgShape);
				++nPrimed;
			}
			else
			{
				// TREED_TYPE — TreeAppearanceType; registration mirrors
				// TreeAppearance::init (treeactor.cpp).  We only need the
				// shape registered, not a full appearance instance.
				// TreeAppearanceType stores its shape as treeShape[lod];
				// cast and call registerMultiShape for each LOD.
				// NOTE: if TreeAppearanceType layout changes, revisit this cast.
				// For now use the same pattern as BldgAppearanceType (both derive
				// from AppearanceType; shapes are TG_TypeMultiShape*).
				// We skip trees here because the TreeAppearance::init already
				// calls registerMultiShape unconditionally at placement time and
				// tree types are typically present in the map (forests).
				// Bridging the gap only for buildings is sufficient for the bug.
				(void)0;
			}
		}
	}

	EditorObjMgrTrace("EditorObjectMgr::primeAllBuildingAppearanceTypes exit nPrimed=%ld", nPrimed);
}

bool  EditorObjectMgr::loadMechs( FitIniFile& file )
{
	// disable animation loading to decrease loading time
	Mech3DAppearanceType::disableAnimationLoading();

	unsigned long count;
	file.seekBlock( "Parts" );
	file.readIdULong( "NumParts", count );

	int alternativeInstancesCounter = count + 1;
	Stuff::Vector3D position;
	position.x = position.y = position.z = 0;
	char buffer[256];
	for ( int i = 1; i < count + 1; ++i )
	{
		sprintf( buffer, "Part%ld", i );
		file.seekBlock( buffer );
		unsigned long fitID;
		file.readIdULong( "ObjectNumber",fitID );
		
		unsigned long group, index;
		if ( !getBuildingFromID( fitID, group, index, true ) )
		{
			// Mission references an object type absent from the (possibly
			// incomplete) editor object catalog. getBuildingFromID leaves
			// group/index past-the-end on a miss; addBuilding/getType would
			// then OOB-deref (the gosASSERT bounds check is a no-op in this
			// build config). Skip this part instead of crashing the whole
			// mission load. Real fix is a complete object data env.
			continue;
		}

		EditorObject* pObject = (EditorObject*)(addBuilding( position, group, index, 0, 0 ));
		if ( !pObject )
			continue;

		gosASSERT( dynamic_cast<Unit*>(pObject)  );

		pObject->load( &file, i );

		if (Mech3DAppearance* m3d = dynamic_cast<Mech3DAppearance*>(pObject->appearance()))
			GameAdapters::Mech::syncSpawn(*m3d, 0u);

		alternativeInstancesCounter += 1;
		int tmpI = ((Unit *)(pObject))->tmpAlternativeStartIndex;
		if (0 < tmpI)
		{
			alternativeInstancesCounter = tmpI;
		}

		int j;
		for (j = 0; j < ((Unit *)(pObject))->tmpNumAlternativeInstances; j += 1)
		{
			sprintf( buffer, "Part%ld", alternativeInstancesCounter );
			file.seekBlock( buffer );
			unsigned long fitID;
			file.readIdULong( "ObjectNumber",fitID );
			
			unsigned long group, index;
			getBuildingFromID( fitID, group, index, true );

			EditorObject* pAltObject = (EditorObject*)(addBuilding( position, group, index, 0, 0 ));
			gosASSERT( dynamic_cast<Unit*>(pAltObject)  );
			pAltObject->load( &file, alternativeInstancesCounter );		
			((Unit *)(pObject))->pAlternativeInstances->Append(*((Unit *)(pAltObject)));
			deleteBuilding(pAltObject);

			alternativeInstancesCounter += 1;
		}
	}

	return true;
	
}

bool		EditorObjectMgr::saveMechs( FitIniFile& file )
{
	EditorObjMgrTrace("EditorObjectMgr::saveMechs: enter units.Count=%d", units.Count());
	file.writeBlock( "Warriors" );

	UNIT_LIST unitsByPlayer[8/*max players*/];
	{
		for ( UNIT_LIST::EIterator iter = units.Begin(); !iter.IsDone(); iter++ )
		{
			int alignment = (*iter)->getAlignment();
			if (8/*max players*/ > alignment)
			{
				unitsByPlayer[alignment].Append((*iter));
			}
			else 
			{
				assert(false);
			}
		}
	}

	UNIT_LIST reorderedUnits;
	UNIT_LIST reorderedUnitsByPlayer[8/*max players*/];
	for (int playerNum = 0; 8/*max players*/ > playerNum; playerNum += 1)
	{
		/*O(n-squared) sort. Who cares.*/
		int numUnitsOfThisPlayer = unitsByPlayer[playerNum].Count();
		for (int i = 0; i < numUnitsOfThisPlayer; i+= 1)
		{
			int lowestSquadNum = INT_MAX;
			UNIT_LIST::EIterator iter;
			for ( iter = unitsByPlayer[playerNum].Begin(); !iter.IsDone(); iter++ )
			{
				if ((*iter)->getSquad() < lowestSquadNum)
				{
					lowestSquadNum = (*iter)->getSquad();
				}
			}
			for ( iter = unitsByPlayer[playerNum].Begin(); !iter.IsDone(); iter++ )
			{
				if (lowestSquadNum == (*iter)->getSquad())
				{
					break;
				}
			}
			if (!iter.IsDone())
			{
				reorderedUnitsByPlayer[playerNum].Append(*iter);
				/*we delete the unit from unitsByPlayer[playerNum] because more than one unit
				may have the same squad number*/
				unitsByPlayer[playerNum].Delete(iter);
			}
			else
			{
				assert(false);
			}
		}
		UNIT_LIST::EIterator iter;
		for ( iter = reorderedUnitsByPlayer[playerNum].Begin(); !iter.IsDone(); iter++ )
		{
			reorderedUnits.Append(*iter);
		}
	}
	assert(reorderedUnits.Count() == units.Count());

	{
		/*When saving, each player's units need to be grouped into "lances" of 12 units
		for historical reasons.*/
		for (int playerNum = 0; 8/*max players*/ > playerNum; playerNum += 1)
		{
			int lanceGroup = 0;
			int mateIndex = 0;
			UNIT_LIST::EIterator iter;
			for ( iter = reorderedUnitsByPlayer[playerNum].Begin(); !iter.IsDone(); iter++ )
			{
				(*iter)->setLanceInfo(lanceGroup, mateIndex);
				mateIndex += 1;
				if (12/*just twelve*/ >= mateIndex)
				{
					mateIndex = 0;
					lanceGroup += 1;
				}
			}
		}
	}

	/*
	if ( 1 > reorderedUnitsByPlayer[EDITOR_TEAM1].Count() )
	{
		EMessageBox( IDS_MUST_HAVE_PLAYER, IDS_CANT_SAVE, MB_OK );
		return false;
	}
	*/

	if (!justResaveAllMaps)
	{
		if (EditorData::instance->IsSinglePlayer())
		{
			bool enemyUnitFound = false;
			for (int playerNum = 1/*first non-user player*/; 8/*max players*/ > playerNum; playerNum += 1)
			{
				if (0/*player1's (user's) team*/ != EditorData::instance->PlayersRef().PlayerRef(playerNum).DefaultTeam())
				{
					if (0 < reorderedUnitsByPlayer[playerNum].Count())
					{
						enemyUnitFound = true;
						break;
					}
				}
			}
			if (!enemyUnitFound)
			{
				EMessageBox( IDS_NO_ENEMY_UNITS, IDS_WARNING, MB_OK );
			}

			if (0 >= reorderedUnitsByPlayer[0/*user player*/].Count())
			{
				EMessageBox( IDS_NO_PLAYER_UNITS, IDS_WARNING, MB_OK );
			}
		}
		else
		{
			for (int playerNum = 0; EditorData::instance->MaxPlayers() > playerNum; playerNum += 1)
			{
				if (1 > reorderedUnitsByPlayer[playerNum].Count())
				{
					EMessageBox( IDS_PLAYERS_WITH_NO_UNITS, IDS_WARNING, MB_OK );
					break;
				}
			}
		}
	}


	file.writeIdULong( "NumWarriors", units.Count() );

	if (!justResaveAllMaps && (units.Count() > 104))
	{
		EMessageBox( IDS_TOO_MANY_PILOTS, IDS_WARNING, MB_OK);
	}

	file.writeBlock( "Parts" );
	file.writeIdULong( "NumParts", units.Count() );
	//file.writeIdBoolean( "AlliedTeam", alignmentCount[NONE] ? true : false );

	char buffer[256];
	int counter = 1; // for some unknown reason we index from 1
	int alternativeInstancesCounter = reorderedUnits.Count() + 1;

	UNIT_LIST::EIterator iter;
	for ( iter = reorderedUnits.Begin(); !iter.IsDone(); counter++, iter++ )
	{
		sprintf( buffer, "Warrior%ld", counter );
		file.writeBlock( buffer );
		// ARM

		// Make a new name of the form "level23_warrior8"
		char armName[512] = {0};
		strcpy(armName, file.getFilename());
		_strlwr(armName);
		char * fitExt = strstr(armName, ".fit");
		// Guard: if getFilename() returns a path without .fit extension, skip ARM relationship.
		if (!fitExt) {
			EditorObjMgrTrace("EditorObjectMgr::saveMechs: armName has no .fit: '%s' (skipping ARM warrior relationship)", armName);
		} else {
			*fitExt = '_';
			sprintf(fitExt+1, "warrior%02d", counter);
			mapAsset->AddRelationship("warrior", armName);
		}

		IProviderAssetPtr mechAssetPtr = armProvider->OpenAsset(
			fitExt ? armName : "", (AssetType)1, ProviderType_Primary);
		mechAsset = (IProviderAsset*)mechAssetPtr;

		(*iter)->save( &file, counter );

		mechAssetPtr->Close();
		mechAssetPtr = NULL;
		mechAsset = NULL;
		
		file.writeIdULong( "AlternativeStartIndex", alternativeInstancesCounter + 1 );

		long IndicesOfAlternatives[15/*max alternatives*/] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
		int numAlternatives = (*iter)->pAlternativeInstances->Count();
		if (15/*max alternatives*/ < numAlternatives)
		{
			gosASSERT(false);
			numAlternatives = 15/*max alternatives*/;
		}
		int j;
		for (j = 0; j <numAlternatives ; j++)
		{
			IndicesOfAlternatives[j] = (alternativeInstancesCounter + 1) + j;
		}
		file.writeIdLongArray("IndicesOfAlternatives", IndicesOfAlternatives, 15);


		alternativeInstancesCounter += 1;
		int i;
		for (i = 0; i < (*iter)->pAlternativeInstances->Count(); i++)
		{
			sprintf( buffer, "Warrior%ld", alternativeInstancesCounter );
			file.writeBlock( buffer );
			(*((*iter)->pAlternativeInstances))[i].save( &file, alternativeInstancesCounter );
			alternativeInstancesCounter += 1;
		}
	}


	// you should be able to get the force groups frm the units themselves 
	file.writeBlock( "Teams" );

	//bool alliedFlag = alignmentCount[2] || alignmentCount[3] || alignmentCount[4] || alignmentCount[5] || alignmentCount[6] || alignmentCount[7];
	//file.writeIdBoolean( "AlliedTeam", alliedFlag ? true : false );

	{
		/*When saving, each player's units need to be grouped into "lances" of 12 units
		for historical reasons.*/
		int reorderedUnitIndex = 1; /*1-based indexing by convention*/
		for (int playerNum = 0; 8/*max players*/ > playerNum; playerNum += 1)
		{
			int reorderedUnitByPlayerIndex = 0;
			int lanceGroup = 0;
			for ( lanceGroup = 0; lanceGroup * (12/*just twelve*/) < reorderedUnitsByPlayer[playerNum].Count(); lanceGroup += 1 )
			{
				long mates[12/*just twelve*/];
				int mateIndex = 0;
				for (mateIndex = 0; 12/*just twelve*/ > mateIndex; mateIndex += 1)
				{
					if (reorderedUnitByPlayerIndex < reorderedUnitsByPlayer[playerNum].Count())
					{
						mates[mateIndex] = reorderedUnitIndex;
						reorderedUnitIndex += 1;
						reorderedUnitByPlayerIndex += 1;
					}
					else
					{
						mates[mateIndex] = 0;
					}
				}
				EString commanderBlock;
				commanderBlock.Format("Commander%ldGroup:%ld", playerNum, lanceGroup);

				file.writeBlock( commanderBlock.Data() );

				file.writeIdLongArray( "Mates", &(mates[0]), 12 );
			}
		}
	}

	return true;

}

bool EditorObjectMgr::saveForests( FitIniFile& file )
{
	long counter = 0;
	char header[256];
	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++, counter++)
	{
		sprintf( header, "Forest%ld", counter );
		file.writeBlock( header );
		(*iter)->save( file );
	}

	return true;
}

bool EditorObjectMgr::loadForests( FitIniFile& file ) 
{
	long counter = 0;
	char header[256];
	long tmp;
	
	while( true )
	{
		sprintf( header, "Forest%ld", counter );
		if ( NO_ERR != file.seekBlock( header ) )
			break;

		file.readIdString( "FileName", header, 255 );
		file.readIdLong( "ID", tmp );

		Forest* pForest = new Forest( tmp );
		pForest->setFileName( header );

		pForest->init( file );
		pForest->ID = counter;

		forests.Append( pForest );
		counter++;
	}

	return true;
}

int EditorObjectMgr::getNextAvailableForestID()
{
	int max = -1;

	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++)
	{
		if ( (*iter)->getID() > max )
		{
			max = (*iter)->getID();
		}
	}

	int ID = max + 1;
	return ID;
}

long EditorObjectMgr::createForest( const Forest& forest )
{
	int ID = getNextAvailableForestID();
	Forest* pForest = new Forest( ID );
	CString oldForestName = pForest->getFileName();
	*pForest = forest;
	pForest->ID =  ID;
	if ( !strstr( pForest->getFileName(), ".fit" ) )
		pForest->setFileName( oldForestName );

	forests.Append( pForest );
	
	// here I need to go about adding the trees -- will do in a sec
	doForest( *pForest );

	return ID;

}

void EditorObjectMgr::editForest( long& oldID, const Forest& forest )
{
	land->unselectAll();

	for ( BUILDING_LIST::EIterator bIter = buildings.End(); !bIter.IsDone();  )
	{
		if ( (*bIter)->getForestID() == oldID )
		{
			long cellI, cellJ;
			(*bIter)->getCells( cellJ, cellI );
			long tileI = cellI/3;
			long tileJ = cellJ/3;

			land->selectVertex( tileJ, tileI, true );

			delete *bIter;

			BUILDING_LIST::EIterator tmpIter = bIter;
			bIter --;
			buildings.Delete( tmpIter );
		}
		else
			bIter--;
	}

	// update the forest object
	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getID() == oldID )
		{
			*(*iter) = forest;
			doForest( *(*iter) );
			break;
		}
	}

	land->unselectAll();
	
}

void EditorObjectMgr::removeForest( const Forest& forest )
{
	unselectAll();
	for ( BUILDING_LIST::EIterator bIter = buildings.End(); !bIter.IsDone();  )
	{
		if ( (*bIter)->getForestID() == forest.getID() )
		{
			delete *bIter;
			BUILDING_LIST::EIterator tmpIter = bIter;
			bIter--;
			buildings.Delete( tmpIter );
		}
		else
		{
			bIter--;
		}
	}

	// now remove the forest from the list
	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getID() == forest.getID() )
		{
			delete (*iter);
			forests.Delete( iter );
			return;
		}
	}
}

void EditorObjectMgr::selectForest( long ID )
{
	unselectAll();

	for ( BUILDING_LIST::EIterator bIter = buildings.Begin(); !bIter.IsDone(); bIter++  )
	{
		if ( (*bIter)->getForestID() == ID )
		{
			select( *(*bIter), true );
		}
	}
}

void EditorObjectMgr::doForest( const Forest& forest )
{
	float centerX = forest.centerX;
	float centerY = forest.centerY;

	float maxDensity = forest.maxDensity/9;
	float minDensity = forest.minDensity/9;

	long probabilities[FOREST_TYPES];

	long probabilityTotal = 0;
	int i = 0;
	for ( i= 0; i < FOREST_TYPES; i++ )
	{
		probabilityTotal += forest.percentages[i];
	}

	for ( i = 0; i < FOREST_TYPES; i++ )
	{
		probabilities[i] = forest.percentages[i]/probabilityTotal * 100;
		if ( i > 0 )
		{
			probabilities[i] = probabilities[i-1] + probabilities[i];
		}
	}



	float r2 = forest.radius; // temporary... calc real radius at some point
	r2 *= r2; // square it

	for ( int j = 0; j < land->realVerticesMapSide; j++ )
	{
		for ( int i = 0; i < land->realVerticesMapSide; i++ )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				long cellI = i * 3;
				long cellJ = j * 3;

				for ( int l = -1; l < 2; l++ )
				{
					for ( int m = -1; m < 2; m++ )
					{
						Stuff::Vector3D pos;

						if ( cellJ + l < 0 || cellJ + l > land->realVerticesMapSide * 3 )
							continue;
						if ( cellI + m < 0 || cellI + m > land->realVerticesMapSide * 3 )
							continue;

						EditorObject* pObject = getObjectAtCell( cellJ + l, cellI + m );
						if ( pObject&& pObject->getForestID() == -1 ) // only forests allow multiples in a cell
						{
							continue;
						}
						
						// figure out how far from center we are
						land->cellToWorld( cellJ + l, cellI + m, pos );
						pos.z = land->getTerrainElevation( pos );

						// distance 
						float deltaX = centerX - pos.x;
						float deltaY = centerY - pos.y;
						float tmpR  = deltaX * deltaX + deltaY * deltaY;
						tmpR = sqrt( tmpR );

						float fm = (minDensity - maxDensity)/forest.radius;
						float density = maxDensity + fm * ( tmpR );
						long lDensity = (long)density;

						float remainder = density - (float)lDensity;

						long count = (int)density + (RandomNumber( 100 ) < remainder * 100 ? 1 : 0);

						// OK, now we have the density randomly (if possible)
						// populate the tile with these...
						// might want to break down by cell
						
						for ( int t = 0; t < count; t++ )
						{
							// oh god, now we need to pick a tree
							long random = RandomNumber( 100 );
							int group = -1;
							int index = -1;
							for ( int k = 0; k < FOREST_TYPES; k++ )
							{
								if ( random < probabilities[k] )
								{
									getRandomTreeFromGroup( k+1, group, index );
									break;
								}
							}

							if ( group == -1 || index == -1 )
							{
								continue;
							}

							Stuff::Vector3D treePos = pos;

							// now place equally randomly.
							treePos.x += (float)RandomNumber( Terrain::worldUnitsPerCell ) - Terrain::worldUnitsPerCell/2;
							treePos.y += (float)RandomNumber( Terrain::worldUnitsPerCell ) - Terrain::worldUnitsPerCell/2;

							int rotation = RandomNumber( 32 );

							float scale = RandomNumber( 100 * (forest.maxHeight - forest.minHeight) );
							scale = forest.minHeight + (scale)/100.f;

							if ((AppearanceTypeList::appearanceHeap->totalCoreLeft() < 1000/*arbitrary*/)
								|| (AppearanceTypeList::appearanceHeap->totalCoreLeft() < 0.01/*arbitrary*/ * AppearanceTypeList::appearanceHeap->size()))
							{
								AfxMessageBox(IDS_APPEARANCE_HEAP_EXHAUSTED);
								/*exit all loops*/
								t = count;
								m = 2;
								l = 2;
								i = land->realVerticesMapSide;
								j = land->realVerticesMapSide;
							}
							else
							{
								EditorObject* pObject = (EditorObject*)addBuilding( treePos, group, 
									index, -1, rotation * 11.25, scale, !forest.bRandom );

								pObject->setForestID( forest.getID() );
							}
						}
					}

				}
			}
		}
	}
}

const Forest* EditorObjectMgr::getForest( long ID )
{
	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++ )
		{
			if ( (*iter)->getID() == ID )
				return (*iter);
		}

	gosASSERT( 0 ); // you shouldn't get here

	return NULL;
}

long EditorObjectMgr::getForests( Forest** pForests, long& count )
{

	if ( forests.Count() > count )
	{
		count = forests.Count();
		return -1;
	}

	count = 0;

	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++ )
	{
		pForests[count++] = (*iter);
	}

	return count;
}

void EditorObjectMgr::getRandomTreeFromGroup( int treeGroup, int& group, int& index )
{
	group = TREE_GROUP;
	index  = 0;

	int treeCount = 0;
	// figure out how many trees are in the group
	Group* pGroup = &groups[TREE_GROUP];
	if ( pGroup )
	{
		EList< Building, Building& >::EIterator buildIter;
		for( buildIter = groups[group].buildings.Begin();
			!buildIter.IsDone(); buildIter++ )
			{
				if ( (*buildIter).forestId == treeGroup )
					treeCount++;
			}

		int whichOne = RandomNumber( treeCount );
		treeCount = 0;

		int counter = 0;
		for( buildIter = groups[group].buildings.Begin();
			!buildIter.IsDone(); buildIter++, counter++ )
			{
				if ( (*buildIter).forestId == treeGroup  )
				{
					if ( treeCount == whichOne )
					{
						index = counter;
					}
					treeCount++;
				}
			}

	}


}


bool EditorObjectMgr::saveDropZones( FitIniFile& file )
{
	int counter = 0;
	char Header[256];

	for ( DROP_LIST::EIterator iter = dropZones.Begin(); !iter.IsDone(); iter++, counter++ )
	{
		sprintf( Header, "DropZone%ld", counter );
		file.writeBlock( Header );
		(*iter)->save( &file, counter );
	}

	// going to do nav markers here
	// need to sort this list first.  ick.
	EditorObject** pObjects = (EditorObject**)_alloca( sizeof( EditorObject* )  * buildings.Count() );
	int i = 0;
	for ( BUILDING_LIST::EIterator bIter = buildings.Begin(); !bIter.IsDone(); bIter++ )
	{
		pObjects[i++] = *bIter;
	}

	long count = i;

	for ( i = 1; i < count; ++i )
	{
		EditorObject* cur = pObjects[i];
		for ( int j = 0; j < i; ++j )
		{
			if ( (getFitID(cur->getID()) < getFitID(pObjects[j]->getID())) && j != i )
			{
				pObjects[i] = pObjects[j];
				pObjects[j] = cur;
				break;
			}
		}
	}

	counter = 0;
	for ( i = 0; i < count; i++ )
	{
		if ( pObjects[i]->save(& file, counter ) )
			counter++;
	}

	// so  it writes out afterward....
	file.writeBlock( "NavMarkers" );
	file.writeIdULong( "NumNavMarkers", counter );

	return true;
}

bool EditorObjectMgr::loadDropZones( FitIniFile& file )
{
	int counter = 0;
	char Header[256];

	while( true )
	{
		sprintf( Header, "DropZone%ld", counter );
		if ( NO_ERR != file.seekBlock( Header ) )
			return counter > 0 ? true : false;

		bool bVTol;
		file.readIdBoolean( "IsVTOL", bVTol );

		Stuff::Vector3D pos;
		file.readIdFloat( "PositionX", pos.x );
		file.readIdFloat( "PositionY", pos.y );

		pos.z = land->getTerrainElevation( pos );

		// assuming single player for now
		addDropZone( pos, 0, bVTol );

		counter++;

	}

	// no need to read in nav markers, they are saved in the building file as well

	return true;
	
	
}

int EditorObjectMgr::getType( unsigned long group, unsigned long indexWithinGroup )
{
	// gosASSERT is a no-op in this build, so it provides no real bounds
	// protection; an unresolved mission object ID (caller passed past-the-end
	// group/index) would OOB-deref here. Guard explicitly.
	if ( group >= (unsigned long)groups.Count() )
		return -1;
	Group* pGroup = &groups[group];

	gosASSERT( pGroup->buildings.Count() > indexWithinGroup );
	if ( indexWithinGroup >= (unsigned long)pGroup->buildings.Count() )
		return -1;
	Building* pBuilding = &pGroup->buildings[indexWithinGroup];

	return pBuilding->type;

}

EditorObjectMgr::Building::~Building()
{

}

bool EditorObjectMgr::moveBuilding( EditorObject* pInfo, long cellJ, long cellI )
{
	
	if (getObjectAtCell( cellJ, cellI ) && (getObjectAtCell( cellJ, cellI ) != pInfo))
	{
		// a building is already there
		return false;
	}

	BuildingLink* pLink = getLinkWithParent( pInfo );
	if ( pLink )
	{
		Stuff::Vector3D worldPos;
		land->cellToWorld( cellJ, cellI, worldPos );
		worldPos.z = land->getTerrainElevation( worldPos );
		pLink->SetParentPosition( worldPos );
	}

	pLink = getLinkWithChild( pInfo );
	if ( pLink )
	{
		pLink->RemoveObject( pInfo );
	}
	
	Stuff::Vector3D position;
	land->cellToWorld( cellJ, cellI, position );

	position.z = land->getTerrainElevation( position );

	pInfo->appearance()->position = position;
	pInfo->cellRow = cellJ; 
	pInfo->cellColumn = cellI;

	if ( pLink )
		pLink->AddChild( pInfo );

	return true;
}

EditorObject* EditorObjectMgr::getObjectAtCell( long realCellJ, long realCellI )
{
	for( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			if (  labs( realCellI - ((*iter)->cellColumn - 4)) <= 8
				&& labs( realCellJ - ((*iter)->cellRow - 4)) <= 8 )
			{
				__int64 FootPrint = getImpassability( (*iter)->id );
				
				int i = realCellI - ((*iter)->cellColumn - 4);
				int j = realCellJ - ((*iter)->cellRow - 4);
				if ( (FootPrint >> ( j * 8 + i )) & 0x00000001 || (i == 4 && j == 4))
					return (*iter);
					
			}

		}

		for( UNIT_LIST::EIterator uIter = units.Begin();
		!uIter.IsDone(); uIter++ )
		{
			if (  labs( realCellI - ((*uIter)->cellColumn - 4)) <= 8
				&& labs( realCellJ - ((*uIter)->cellRow - 4)) <= 8 )
			{
				__int64 FootPrint = getImpassability( (*uIter)->id );
				
				int i = realCellI - ((*uIter)->cellColumn - 4);
				int j = realCellJ - ((*uIter)->cellRow - 4);
				if ( (FootPrint >> ( j * 8 + i )) & 0x00000001 || (i == 4 && j == 4))
					return (*uIter);
					
			}

		}

	return NULL;

}

EditorObject* EditorObjectMgr::getBuilding( const EditorObject &building )
{
	for( BUILDING_LIST::EIterator iter = buildings.Begin(); !iter.IsDone(); iter++ )
	{
		if( building.cellColumn == (*iter)->cellColumn
			&& building.cellRow == (*iter)->cellRow )
		{
			return (*iter);
		}
	}
	return NULL;
}

BuildingLink* EditorObjectMgr::getLinkWithBuilding( const EditorObject* pObj )
{
	for ( LINK_LIST::EIterator iter = links.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->HasParent( pObj ) || (*iter)->HasChild( pObj ) )
			return *iter;
	}

	return NULL;
}

BuildingLink* EditorObjectMgr::getLinkWithParent( const EditorObject* pObj )
{
	for ( LINK_LIST::EIterator iter = links.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->HasParent( pObj ) )
			return *iter;
	}

	return NULL;
}

BuildingLink* EditorObjectMgr::getLinkWithChild( const EditorObject* pObj )
{
	for ( LINK_LIST::EIterator iter = links.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->HasChild( pObj ) )
			return *iter;
	}

	return NULL;
}

void EditorObjectMgr::addLink( BuildingLink* pLink )
{
	for ( LINK_LIST::EIterator iter = links.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter) == pLink )
			return;
	}

	links.Append( pLink );
}

void EditorObjectMgr::deleteLink( BuildingLink* pLink )
{
	for ( LINK_LIST::EIterator iter = links.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter) == pLink )
		{
			delete (*iter);

			links.Delete( iter );
			return;
		}
	}
}

const char*	EditorObjectMgr::getObjectName( int ID ) const
{
	unsigned long group = getGroup(ID);
	unsigned long index = getIndexInGroup(ID);

	if ( group >= (unsigned long)groups.Count() )
		return "Unknown Object";

	if ( index >= (unsigned long)groups[group].buildings.Count() )
		return "Unknown Object";

	const char* name = groups[group].buildings[index].name;
	return (name && name[0]) ? name : "Unknown Object";
}

const char* EditorObjectMgr::getGroupName( int ID ) const
{
	if ( ID < 0 || ID >= groups.Count() )
		return "Unknown Group";

	const char* name = groups[ID].name;
	return (name && name[0]) ? name : "Unknown Group";
}

void EditorObjectMgr::getUnitGroupNames( const char** names, int* ids, int& numberOfEm ) const
{
	int counter = 0;
	int index = 0;
	
	for ( GROUP_LIST::EConstIterator iter = groups.Begin(); !iter.IsDone(); index++, iter++ )
	{
		if ( (*iter).buildings.Count() && ((*iter).buildings.GetHead().type == MECH_TYPE || (*iter).buildings.GetHead().type == GV_TYPE ) )
		{
			if ( counter < numberOfEm )
			{
				names[counter] = (*iter).name;
				ids[counter] = index;
			}

			counter++;
		}
	}

	numberOfEm = counter;
}

int EditorObjectMgr::getUnitGroupCount( ) const
{
	int counter = 0;
	
	for ( GROUP_LIST::EConstIterator iter = groups.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter).buildings.Count() && ( (*iter).buildings.GetHead().type == MECH_TYPE ||(*iter).buildings.GetHead().type == GV_TYPE ) )
		{
			counter++;
		}

	}

	return counter;

}

void EditorObjectMgr::getSelectedUnits( UNIT_LIST& selUnits )
{
	for ( UNIT_LIST::EIterator iter = units.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->isSelected() )
			selUnits.Append( (*iter) );
	}
}

int			EditorObjectMgr::getNumberOfVariants( int group, int indexInGroup ) const
{
		Building bldg = groups[group].buildings[indexInGroup];

		if ( !bldg.varNames  )
			return 0;

        int i = 0;
        for ( i = 0; i < 16; ++i )
        {
            if ( !bldg.varNames[i] )
                break;
        }

        return i + 1;			
}

void		EditorObjectMgr::getVariantNames( int group, int indexInGroup, const char** names, int& numberOfNames ) const
{
	Building bldg = groups[group].buildings[indexInGroup];

    int i = 0;
    for ( i = 0; i < 16 && i < numberOfNames; ++i )
    {
        if ( !bldg.varNames[i] )
            break;

        names[i] = bldg.varNames[i];
    }

    numberOfNames =  i;			

}


void EditorObjectMgr::registerSquadNum(unsigned long squadNum)
{
	if (squadNum >= nextAvailableSquadNum)
	{
		nextAvailableSquadNum = squadNum + 1;
	}
}

Unit* EditorObjectMgr::findUnitByMechHandle(RenderCore::RenderObjectHandle h)
{
	for (BUILDING_LIST::EIterator iter = buildings.Begin(); !iter.IsDone(); iter++)
	{
		EditorObject* obj = *iter;
		if (!obj || !obj->appearance())
			continue;
		Unit* unit = dynamic_cast<Unit*>(obj);
		if (!unit)
			continue;
		if (Mech3DAppearance* m3d = dynamic_cast<Mech3DAppearance*>(unit->appearance()))
		{
			if (m3d->getRenderWorldHandle().bits == h.bits)
				return unit;
		}
	}
	return nullptr;
}

void EditorObjectMgr::registerStaticPropsForMissionLoad()
{
	if (!GpuStaticPropRegistry::isMissionLoadRegEnabled())
		return;
	static const long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
	int total = 0, registered = 0;
	for (BUILDING_LIST::EIterator iter = buildings.Begin(); !iter.IsDone(); iter++)
	{
		EditorObject* obj = *iter;
		if (!obj || !obj->appearance())
			continue;
		++total;
		ObjectAppearance* app = obj->appearance();
		int teamIdx = (app->teamId + 1 >= 0 && app->teamId + 1 < 9) ? app->teamId + 1 : 0;
		app->setObjectParameters(app->position, app->rotation, app->selected,
		                         app->teamId, homeRelations[teamIdx]);
		app->registerStatic();
		if (app->isStaticRegistered())
			++registered;
	}
	fprintf(stderr, "[STATIC_PROP_REG v1] event=editor_load enumerated=%d registered=%d skipped=%d\n",
	        total, registered, total - registered);
	fflush(stderr);
}

//*************************************************************************************************
// end of file ( EditorObjectMgr.cpp )
