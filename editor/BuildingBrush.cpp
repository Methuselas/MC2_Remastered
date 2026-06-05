#define BUILDINGBRUSH_CPP
/*************************************************************************************************\
BuildingBrush.cpp	: Implementation of the BuildingBrush component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "stdafx.h"
#include "BuildingBrush.h"

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#ifndef TERRAIN_H
#include "Terrain.h"
#endif

#ifndef EDITOROBJECTS_H
#include "EditorObjects.h"
#endif

#ifndef EDITORINTERFACE_H
#include "EditorInterface.h"
#endif

#include "resource.h"

BuildingBrush::BuildingBrush( int Group, int IndexInGroup, int Alignment )
{
	group = Group;
	indexInGroup = IndexInGroup;
	pAction = NULL;
	pCursor = EditorObjectMgr::instance()->getAppearance( Group, IndexInGroup );
	pCursor->teamId = Alignment;
	pCursor->setInView(true);
	pCursor->setVisibility(true,true);
	pCursor->position = eye->getPosition();
	pCursor->update();
	// GL port: placed buildings render through the GPU static-prop batcher, which
	// only draws appearances that own a recipe (registered at mission load via
	// EditorObjectMgr::registerStaticPropsForMissionLoad -> app->registerStatic()).
	// The preview cursor is created AFTER mission load and was never registered,
	// so its per-frame appearance()->render() submit produced no geometry -> no
	// placement ghost. Register it on menu-select so the ghost (and its live
	// mouse-follow position + wheel rotation) renders like a placed object.
	pCursor->registerStatic();
	alignment = Alignment;
}

BuildingBrush::BuildingBrush()
{
	pCursor = NULL;
	group = indexInGroup = -1;
	pAction = NULL;
}

BuildingBrush::~BuildingBrush()
{
	if ( pCursor )
		delete pCursor;
}

bool BuildingBrush::canPaint( Stuff::Vector3D& worldPos, int screenX, int screenY, int flags )
{
	if ( !EditorObjectMgr::instance()->canAddBuilding( worldPos, pCursor->rotation, group, indexInGroup ) )
	{
		pCursor->setHighlightColor( 0x007f0000 );
		return false; // no two things on top of each other
	}

	pCursor->setHighlightColor( 0x00007f00 );
	return true;
}

bool BuildingBrush::beginPaint()
{
	gosASSERT( !pAction );

	pAction = new BuildingAction;
	
	return true; // need to set up undo here
}

bool BuildingBrush::tryWallSnap( const Stuff::Vector3D& inPos, Stuff::Vector3D& outPos )
{
	if ( !pCursor )
		return false;
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if ( mgr->getSpecialType( mgr->getID( group, indexInGroup ) ) != EditorObjectMgr::WALL )
		return false;
	EditorInterface* ui = EditorInterface::instance();
	if ( !ui || !ui->magneticWallsEnabled() )
		return false;
	return mgr->findWallEndpointSnap( inPos, pCursor->rotation, pCursor->getRadius(), NULL, outPos );
}

bool BuildingBrush::paint( Stuff::Vector3D& worldPos, int screenX, int screenY )
{
	// Patch: Editor placement must not be blocked by the legacy appearance-heap
	// warning threshold.  New/editor-created maps can report very low remaining
	// appearance heap even when the actual addBuilding path can still allocate
	// and place the object.  The old dialog prevented placement before the real
	// operation was attempted, which also blocked save testing.

	// Magnetic wall joins: if a neighbour endpoint is in range, place at the
	// snapped (off-grid) pose so the wall run connects at its rotated angle.
	// bSnapToCell=false stores the exact snapped position; cell bookkeeping still
	// derives from it. Non-wall / no-neighbour placement is unchanged (grid).
	Stuff::Vector3D placePos = worldPos;
	const bool snapped = tryWallSnap( worldPos, placePos );

	EditorObject* pInfo = snapped
		? EditorObjectMgr::instance()->addBuilding( placePos, group, indexInGroup, alignment, pCursor->rotation, 1.0f, false )
		: EditorObjectMgr::instance()->addBuilding( worldPos, group, indexInGroup, alignment, pCursor->rotation );
	if ( pInfo && pAction )
		pAction->addBuildingInfo( *pInfo );

	return (pInfo != NULL);
}

Action* BuildingBrush::endPaint( )
{
	if (pAction)
	{
		if ( !pAction->objInfoPtrList.Count() )
		{
			delete pAction;
			pAction = NULL;
		}
	}
	Action* pRetAction = pAction;
	pAction = NULL;
	return pRetAction;
}

bool BuildingBrush::BuildingAction::undo()
{
	bool bRetVal = true;

	/*
	OBJ_APPEAR_LIST::EIterator iter3 = buildingAppearanceCopies.Begin();
	for ( OBJ_INFO_LIST::EIterator iter = positions.Begin();
		!iter.IsDone(); iter++ )
	{
		EditorObject* pObj = EditorObjectMgr::instance()->getObjectAtLocation((*iter3).position.x, (*iter3).position.y);
		if (pObj)
		{
			bRetVal = EditorObjectMgr::instance()->deleteBuilding( pObj ) && bRetVal;
		}
		else
		{
			gosASSERT(false);
		}

		iter3++;
	}
	*/
	
	for ( OBJ_INFO_PTR_LIST::EIterator iter = objInfoPtrList.Begin(); !iter.IsDone(); iter++ )
	{
		ObjectAppearance *pAppearance = (*(*iter)).appearance();
		EditorObject* pObj = EditorObjectMgr::instance()->getObjectAtLocation(pAppearance->position.x, pAppearance->position.y);
		if (pObj)
		{
			bRetVal = EditorObjectMgr::instance()->deleteBuilding( pObj ) && bRetVal;
		}
		else
		{
			gosASSERT(false);
		}
	}

	return bRetVal;
}

bool BuildingBrush::BuildingAction::redo()
{
	bool bRetVal = true;

	for ( OBJ_INFO_PTR_LIST::EIterator iter = objInfoPtrList.Begin(); !iter.IsDone(); iter++ )
	{
		ObjectAppearance *pAppearance = (*(*iter)).appearance();
		EditorObject* pObj = EditorObjectMgr::instance()->addBuilding( pAppearance->position, EditorObjectMgr::instance()->getGroup( (*(*iter)).getID() ), EditorObjectMgr::instance()->getIndexInGroup( (*(*iter)).getID() ), pAppearance->teamId, pAppearance->rotation );
		if (pObj)
		{
			(*pObj).CastAndCopy((*(*iter)));
		}
		else
		{
			gosASSERT(false);
			bRetVal = false;
		}
	}

	return bRetVal;

}

void BuildingBrush::BuildingAction::addBuildingInfo(EditorObject& info)
{
	EditorObject *pCopy = info.Clone();
	gosASSERT(pCopy);
	objInfoPtrList.Append(pCopy);
}

void BuildingBrush::update( int ScreenMouseX, int ScreenMouseY )
{
	if ( !pCursor )
		return;
	
	Stuff::Vector3D pos;
	Stuff::Vector2DOf<long> pt;
	pt.x = ScreenMouseX;
	pt.y = ScreenMouseY;
	eye->inverseProject( pt, pos );

	// Preview the magnetic wall snap so the ghost shows where it will actually
	// land. Blue tint signals "snapped to a neighbour endpoint".
	Stuff::Vector3D snappedPos;
	const bool wallSnapped = tryWallSnap( pos, snappedPos );
	if ( wallSnapped )
		pos = snappedPos;

	if ( wallSnapped )
		pCursor->setHighlightColor( 0x00003fbf );   // blue-ish: magnetic snap active
	else if ( !EditorObjectMgr::instance()->canAddBuilding( pos, pCursor->rotation, group, indexInGroup ) )
		pCursor->setHighlightColor( 0x00400000 );
	else
		pCursor->setHighlightColor( 0x00004000 );

	pCursor->position = pos;
	pCursor->recalcBounds();
	pCursor->update();			//Safe tp call here now because we run the first update in the constructor which caches in texture
								//NOT TRUE WITH RIA CODE!!!!!  Must have a separate update or NO Triangles get added!!!
	pCursor->setVisibility( true, true );
}

void BuildingBrush::render( int ScreenMouseX, int ScreenMouseY )
{
	if ( !pCursor )
		return;
	
	/*
	Stuff::Vector3D pos;
	Stuff::Vector2DOf<long> pt;
	pt.x = ScreenMouseX;
	pt.y = ScreenMouseY;
	eye->inverseProject( pt, pos );
	
	if ( !EditorObjectMgr::instance()->canAddBuilding( pos, group, indexInGroup ) )
		pCursor->setHighlightColor( 0x00400000 );
	else
		pCursor->setHighlightColor( 0x00004000 );

	pCursor->position = pos;
	pCursor->recalcBounds();
	pCursor->update();			//Safe tp call here now because we run the first update in the constructor which caches in texture
								//NOT TRUE WITH RIA CODE!!!!!  Must have a separate update or NO Triangles get added!!!
	pCursor->setVisibility( true, true );
	*/		//This may cause cursor to lag.  Check it and see.
	pCursor->render();
}

void BuildingBrush::rotateBrush( int direction )
{
	int ID = EditorObjectMgr::instance()->getID( group, indexInGroup );
	int fitID = EditorObjectMgr::instance()->getFitID(ID);
	float step = ((EditorObjectMgr::WALL == EditorObjectMgr::instance()->getSpecialType(ID)) || (33/*repair bay*/ == fitID)) ? 90.0f : 45.0f;
	addRotationDegrees( direction * step );
}

// Continuous rotation entry point used by the mouse wheel (EditorInterface::OnMouseWheel).
// rotateBrush() above remains the discrete [/] keyboard 45/90 stepper.
void BuildingBrush::addRotationDegrees( float deg )
{
	if ( pCursor )
		pCursor->rotation += deg;
}

//*************************************************************************************************
// end of file ( BuildingBrush.cpp )
