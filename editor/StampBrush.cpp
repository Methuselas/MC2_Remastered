/*************************************************************************************************\
StampBrush.cpp : see StampBrush.h
\*************************************************************************************************/

#include "StampBrush.h"

#ifndef CAMERA_H
#include "Camera.h"
#endif
#ifndef TERRAIN_H
#include "Terrain.h"
#endif

#include "../GameOS/gameos/gos_terrain_indirect.h"
#include "mapdata.h"
#include <math.h>
#include <set>

extern CameraPtr eye;
extern Terrain*  land;

StampBrush::StampBrush( Type type, float radius, float strength )
	: m_type( type ), m_radius( radius ), m_strength( strength ),
	  m_applied( false ), m_pAction( NULL )
{
}

StampBrush::~StampBrush()
{
}

bool StampBrush::beginPaint()
{
	m_pAction = new ActionPaintTile();
	m_applied = false;
	return true;
}

// 0 at the rim, 1 at the centre (smoothstep).
static inline float stampFalloff( float t )
{
	if ( t >= 1.0f ) return 0.0f;
	if ( t < 0.0f )  t = 0.0f;
	return 1.0f - t * t * ( 3.0f - 2.0f * t );
}

bool StampBrush::paint( Stuff::Vector3D& worldPos, int /*screenX*/, int /*screenY*/ )
{
	if ( !land || !m_pAction || m_applied )
		return false;   // one-shot per click-stroke

	const float wupv = land->worldUnitsPerVertex;
	const int   side = land->realVerticesMapSide;
	if ( wupv <= 0.0f || side <= 0 )
		return false;

	const int cx = (int)floor( ( worldPos.x - land->mapTopLeft3d.x ) / wupv + 0.5f );
	const int cy = (int)floor( ( land->mapTopLeft3d.y - worldPos.y ) / wupv + 0.5f );
	const int vr = (int)ceil( m_radius / wupv ) + 1;

	const float centerElev = land->getTerrainElevation( worldPos );

	std::set<int> touched;
	for ( int j = cy - vr; j <= cy + vr; ++j )
	{
		if ( j < 0 || j >= side ) continue;
		for ( int i = cx - vr; i <= cx + vr; ++i )
		{
			if ( i < 0 || i >= side ) continue;

			const float vwx = land->mapTopLeft3d.x + (float)i * wupv;
			const float vwy = land->mapTopLeft3d.y - (float)j * wupv;
			const float dx  = vwx - worldPos.x;
			const float dy  = vwy - worldPos.y;
			const float dist = sqrtf( dx * dx + dy * dy );
			if ( dist > m_radius ) continue;

			const float f = stampFalloff( dist / m_radius );
			const int   vn  = j * side + i;
			const float cur = land->getTerrainElevation( j, i );
			float nv = cur;
			switch ( m_type )
			{
				case PAD:    nv = cur + ( centerElev - cur ) * f; break;   // flatten toward centre
				case CRATER: nv = cur - m_strength * f;            break;  // bowl down
				case HILL:   nv = cur + m_strength * f;            break;  // dome up
			}

			m_pAction->addChangedVertexInfo( j, i );   // undo: stores old height
			land->setVertexHeight( vn, nv );
			touched.insert( vn );
		}
	}

	m_applied = true;

	// Recompute normals so the stamped area lights correctly, then re-bake the
	// affected GPU recipes (same as the sculpt brush).
	if ( Terrain::mapData )
		Terrain::mapData->calcLight();
	for ( std::set<int>::const_iterator it = touched.begin(); it != touched.end(); ++it )
	{
		const int vn = *it;
		const int r = vn / side, c = vn % side;
		const int q[4][2] = { { r, c }, { r, c - 1 }, { r - 1, c }, { r - 1, c - 1 } };
		for ( int k = 0; k < 4; ++k )
		{
			const int rr = q[k][0], cc = q[k][1];
			if ( rr >= 0 && rr < side - 1 && cc >= 0 && cc < side - 1 )
				gos_terrain_indirect::InvalidateRecipeForVertexNum( rr * side + cc );
		}
	}

	return !touched.empty();
}

Action* StampBrush::endPaint()
{
	ActionPaintTile* pRet = m_pAction;
	if ( m_pAction && !m_pAction->vertexInfoList.Count() )
	{
		delete m_pAction;
		pRet = NULL;
	}
	if ( land )
		land->recalcWater();
	m_pAction = NULL;
	m_applied = false;
	return pRet;
}

void StampBrush::render( int screenX, int screenY )
{
	if ( !eye || !land )
		return;

	Stuff::Vector3D center;
	Stuff::Vector2DOf<long> sp;
	sp.x = screenX; sp.y = screenY;
	eye->inverseProject( sp, center );
	center.z = land->getTerrainElevation( center );

	Stuff::Vector4D centerS;
	eye->projectZ( center, centerS );

	const int N = 48;
	Stuff::Vector4D rim[N + 1];
	for ( int k = 0; k <= N; ++k )
	{
		const float ang = (float)k / (float)N * 6.2831853f;
		Stuff::Vector3D wp = center;
		wp.x += cosf( ang ) * m_radius;
		wp.y += sinf( ang ) * m_radius;
		wp.z = land->getTerrainElevation( wp );
		eye->projectZ( wp, rim[k] );
	}

	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	// Always-visible cursor overlay: disable depth test/write (rhw=1 z=0 verts
	// otherwise fail the reverse-Z chunk-terrain depth test and vanish). Mirrors
	// FoliageRender; restored below.
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite,   0 );

	// Translucent cyan disc (stamp footprint).
	for ( int k = 0; k < N; ++k )
	{
		gos_VERTEX t[3];
		memset( t, 0, sizeof( t ) );
		t[0].x = centerS.x; t[0].y = centerS.y; t[0].rhw = 1.0f; t[0].argb = 0x3000ffff;
		t[1].x = rim[k].x;  t[1].y = rim[k].y;  t[1].rhw = 1.0f; t[1].argb = 0x3000ffff;
		t[2].x = rim[k+1].x;t[2].y = rim[k+1].y;t[2].rhw = 1.0f; t[2].argb = 0x3000ffff;
		gos_DrawTriangles( t, 3 );
	}
	for ( int k = 0; k < N; ++k )
	{
		gos_VERTEX v[2];
		memset( v, 0, sizeof( v ) );
		v[0].x = rim[k].x;   v[0].y = rim[k].y;   v[0].rhw = 1.0f; v[0].argb = 0xff00ffff;
		v[1].x = rim[k+1].x; v[1].y = rim[k+1].y; v[1].rhw = 1.0f; v[1].argb = 0xff00ffff;
		gos_DrawLines( v, 2 );
	}

	gos_SetRenderState( gos_State_ZCompare, 1 );
	gos_SetRenderState( gos_State_ZWrite,   1 );
}
