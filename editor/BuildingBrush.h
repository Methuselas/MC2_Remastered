#ifndef BUILDINGBRUSH_H
#define BUILDINGBRUSH_H
/*************************************************************************************************\
BuildingBrush.h		: Interface for the BuildingBrush component. The thing you use to paint 
						buildings
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef BRUSH_H
#include "Brush.h"
#endif

#ifndef ACTION_H
#include "Action.h"
#endif

#ifndef EDITOROBJECTMGR_H
#include "EditorObjects.h"
#endif

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
BuildingBrush:
**************************************************************************************************/
class BuildingBrush: public Brush
{
	public:

		BuildingBrush( int group, int indexInGroup, int Alignment );
		virtual ~BuildingBrush();

		virtual bool beginPaint();
		virtual Action* endPaint();
		virtual bool paint( Stuff::Vector3D& worldPos, int screenX, int screenY );
		virtual bool canPaint( Stuff::Vector3D& worldPos, int screenX, int screenY, int flags );
		virtual void render( int ScreenMouseX, int ScreenMouseY);
		virtual void update( int screenX, int screenY );
		void rotateBrush( int direction );
		void addRotationDegrees( float deg );

		// Read-only accessors so the object companion panel can show/highlight
		// the active object and reselect siblings without re-walking the menu.
		int getGroup() const { return group; }
		int getIndexInGroup() const { return indexInGroup; }
		int getAlignment() const { return alignment; }

		// Magnetic wall joins: if this brush is a wall, magnetic mode is on, and a
		// neighbour wall endpoint is within range of inPos's endpoints, set outPos
		// to the snapped center and return true. Otherwise return false.
		bool tryWallSnap( const Stuff::Vector3D& inPos, Stuff::Vector3D& outPos );

		
		class BuildingAction : public Action
		{
		public:
			
			virtual ~BuildingAction(){}
			virtual bool redo();
			virtual bool undo();
			virtual void addBuildingInfo(EditorObject& info);

			class OBJ_INFO_PTR_LIST : public EList<EditorObject *, EditorObject *> {
			public:
				~OBJ_INFO_PTR_LIST() {
					EIterator it;
					for (it = Begin(); !it.IsDone(); it++) {
						delete (*it);
					}
				}
			};
			
			OBJ_INFO_PTR_LIST objInfoPtrList;
		};

		protected:

		// suppression
		BuildingBrush( const BuildingBrush& buildingBrush );
		BuildingBrush& operator=( const BuildingBrush& buildingBrush );
		BuildingBrush();

		int group;
		int indexInGroup;
		float curRotation;

		BuildingAction*		pAction;

		ObjectAppearance*	pCursor;

		int					alignment;
};


//*************************************************************************************************
#endif  // end of file ( BuildingBrush.h )
