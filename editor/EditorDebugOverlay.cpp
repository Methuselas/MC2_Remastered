//-------------------------------------------------------------------------------------------------
// EditorDebugOverlay.cpp -- see EditorDebugOverlay.h.
//
// World overlays are drawn as screen-space gos line strips: sample world points along
// each grid line, project them with Camera::projectForScreenXY, and connect successive
// admitted points with gos_DrawLines (the exact primitive the editor's other overlays
// -- ScatterBrush/HeightBrush/selection -- use).  A line breaks (no segment) wherever a
// sample point fails projection (off-frustum / behind near plane) so a single bad point
// never spawns a screen-spanning streak.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"
#include "EditorDebugOverlay.h"

#include "Camera.h"
#include "Terrain.h"
#include "FoliageRender.h"

#ifdef MC2_IMGUI
#include "imgui.h"
#endif

#include <math.h>
#include <vector>

// The editor's terrain instance (same global ScatterBrush/StampBrush use).
extern Terrain* land;

namespace
{
	// --- overlay state (file static; pure UI, never saved) -----------------------
	bool  s_showChunkGrid      = false;
	bool  s_showSuperchunkGrid = false;
	bool  s_showWaterDebug     = false;
	bool  s_showFoliageDebug   = false;   // ImGui stats only; no world draw

	int   s_superchunkChunks   = 3;       // 1..8 chunks per superchunk
	float s_lineOpacity        = 0.65f;   // 0..1
	float s_heightBias         = 8.0f;    // world units lifted above the sample plane
	bool  s_gridOnWaterPlane   = true;    // true: draw on water plane (water diag);
	                                      // false: follow terrain elevation

	const int   kChunkCells          = 20;     // chunk = 20 terrain cells (= 20 verts)
	const float kMaxSamplesPerLine   = 4096.f; // safety cap

	bool terrainLoaded()
	{
		return ( land != NULL && Terrain::realVerticesMapSide > 1 );
	}

	// World height for a grid sample at (wx, wy).
	float sampleZ( float wx, float wy )
	{
		if ( s_gridOnWaterPlane )
			return Terrain::waterElevation + s_heightBias;

		Stuff::Vector3D p;
		p.x = wx; p.y = wy; p.z = 0.0f;
		return land->getTerrainElevation( p ) + s_heightBias;
	}

	// Project a world point; returns false if it should break the line strip.
	bool projectPt( Camera* eye, float wx, float wy, float wz, float& sx, float& sy )
	{
		Stuff::Vector3D world;  world.x = wx; world.y = wy; world.z = wz;
		Stuff::Vector4D scr;
		if ( !eye->projectForScreenXY( world, scr ) )
			return false;
		if ( scr.w <= 1e-4f )                       return false;   // behind near plane
		if ( !( scr.x == scr.x ) || !( scr.y == scr.y ) ) return false;   // NaN
		sx = scr.x; sy = scr.y;
		return true;
	}

	// Draw a connected world polyline (list of world XY samples at a given plane).
	// horizontal=true -> line runs along +X at fixed row world-y; else along -Y.
	void drawGridLine( Camera* eye, bool horizontal, float fixedWorld,
	                   float start, float end, float stepWorld, DWORD argb )
	{
		gos_VERTEX seg[2];
		memset( seg, 0, sizeof( seg ) );
		seg[0].rhw = seg[1].rhw = 1.0f;
		seg[0].argb = seg[1].argb = argb;

		bool havePrev = false;
		float px = 0.f, py = 0.f;

		int   n = 0;
		float t = start;
		const float dir = ( end >= start ) ? 1.0f : -1.0f;
		for ( ; ( dir > 0 ? t <= end : t >= end ) && n < kMaxSamplesPerLine; t += stepWorld * dir, ++n )
		{
			float wx, wy;
			if ( horizontal ) { wx = t;          wy = fixedWorld; }
			else              { wx = fixedWorld;  wy = t;          }

			float sx, sy;
			bool ok = projectPt( eye, wx, wy, sampleZ( wx, wy ), sx, sy );
			if ( ok && havePrev )
			{
				seg[0].x = px; seg[0].y = py;
				seg[1].x = sx; seg[1].y = sy;
				gos_DrawLines( seg, 2 );
			}
			havePrev = ok;
			px = sx; py = sy;
		}
	}

	DWORD packARGB( int r, int g, int b )
	{
		int a = (int)( s_lineOpacity * 255.0f );
		if ( a < 0 ) a = 0; if ( a > 255 ) a = 255;
		return ( (DWORD)a << 24 ) | ( (DWORD)r << 16 ) | ( (DWORD)g << 8 ) | (DWORD)b;
	}

	// Draw a square grid every `stepCells` vertices across the whole map.
	void drawGrid( Camera* eye, int stepCells, DWORD argb )
	{
		const long  side = Terrain::realVerticesMapSide;
		const float wupv = Terrain::worldUnitsPerVertex;
		const float tlx  = Terrain::mapTopLeft3d.x;
		const float tly  = Terrain::mapTopLeft3d.y;

		const float minX = tlx;
		const float maxX = tlx + (float)( side - 1 ) * wupv;
		const float minY = tly - (float)( side - 1 ) * wupv;   // bottom (largest row)
		const float maxY = tly;                                 // top (row 0)

		// Sample along each line at chunk resolution so terrain-follow mode tracks the
		// surface; on the water plane this is a straight line anyway.
		const float sampleStep = wupv * (float)kChunkCells;

		// Vertical lines (fixed column index c -> fixed world x), run top->bottom.
		for ( long c = 0; c < side; c += stepCells )
		{
			float wx = tlx + (float)c * wupv;
			drawGridLine( eye, /*horizontal=*/false, wx, maxY, minY, sampleStep, argb );
		}
		// Horizontal lines (fixed row index r -> fixed world y), run left->right.
		for ( long r = 0; r < side; r += stepCells )
		{
			float wy = tly - (float)r * wupv;
			drawGridLine( eye, /*horizontal=*/true, wy, minX, maxX, sampleStep, argb );
		}
	}

	// Water-plane bounds rectangle (4 edges) at exactly waterElevation.
	void drawWaterBounds( Camera* eye )
	{
		const long  side = Terrain::realVerticesMapSide;
		const float wupv = Terrain::worldUnitsPerVertex;
		const float tlx  = Terrain::mapTopLeft3d.x;
		const float tly  = Terrain::mapTopLeft3d.y;
		const float minX = tlx, maxX = tlx + (float)( side - 1 ) * wupv;
		const float minY = tly - (float)( side - 1 ) * wupv, maxY = tly;

		const bool savedMode = s_gridOnWaterPlane;
		s_gridOnWaterPlane = true;                 // force exact water plane for the box
		const float savedBias = s_heightBias;
		s_heightBias = 0.0f;
		const DWORD argb = packARGB( 60, 140, 255 );
		const float sampleStep = wupv * (float)kChunkCells;
		drawGridLine( eye, true,  maxY, minX, maxX, sampleStep, argb );  // top
		drawGridLine( eye, true,  minY, minX, maxX, sampleStep, argb );  // bottom
		drawGridLine( eye, false, minX, maxY, minY, sampleStep, argb );  // left
		drawGridLine( eye, false, maxX, maxY, minY, sampleStep, argb );  // right
		s_gridOnWaterPlane = savedMode;
		s_heightBias = savedBias;
	}
}

namespace EditorDebugOverlay
{

void RenderWorldOverlay( Camera* eye )
{
	if ( !eye || !terrainLoaded() )
		return;
	if ( !s_showChunkGrid && !s_showSuperchunkGrid && !s_showWaterDebug )
		return;

	// Overlay render state -- match FoliageRender: alpha-blended, no texture, drawn
	// always-on-top (ZCompare off) so the grid is readable over water/terrain.
	gos_SetRenderState( gos_State_Texture,   0 );
	gos_SetRenderState( gos_State_AlphaMode,  gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest,  0 );
	gos_SetRenderState( gos_State_ZCompare,   0 );
	gos_SetRenderState( gos_State_ZWrite,     0 );

	if ( s_showChunkGrid )
		drawGrid( eye, kChunkCells, packARGB( 0, 200, 255 ) );          // cyan

	if ( s_showSuperchunkGrid )
	{
		int chunks = s_superchunkChunks; if ( chunks < 1 ) chunks = 1; if ( chunks > 8 ) chunks = 8;
		drawGrid( eye, kChunkCells * chunks, packARGB( 255, 225, 40 ) ); // bright yellow
	}

	if ( s_showWaterDebug )
		drawWaterBounds( eye );

	// Restore state for subsequent editor overlays (selection/brush quads).
	gos_SetRenderState( gos_State_ZCompare, 1 );
	gos_SetRenderState( gos_State_ZWrite,   1 );
}

#ifdef MC2_IMGUI

void RenderImGui()
{
	ImGui::SetNextWindowSize( ImVec2( 300.f, 0.f ), ImGuiCond_Once );
	ImGui::SetNextWindowPos ( ImVec2( 16.f, 540.f ), ImGuiCond_Once );
	if ( !ImGui::Begin( "Debug Overlays" ) ) { ImGui::End(); return; }

	ImGui::Checkbox( "Show Chunk Grid",      &s_showChunkGrid );
	ImGui::Checkbox( "Show Superchunk Grid", &s_showSuperchunkGrid );
	ImGui::Checkbox( "Show Water Debug",     &s_showWaterDebug );
	ImGui::Checkbox( "Show Foliage Debug",   &s_showFoliageDebug );

	ImGui::Separator();
	ImGui::SetNextItemWidth( 160.f );
	ImGui::SliderInt( "Superchunk = N chunks", &s_superchunkChunks, 1, 8 );
	ImGui::SetNextItemWidth( 160.f );
	ImGui::SliderFloat( "Line opacity", &s_lineOpacity, 0.05f, 1.0f, "%.2f" );
	ImGui::Checkbox( "Grid on water plane (vs follow terrain)", &s_gridOnWaterPlane );
	ImGui::SetNextItemWidth( 160.f );
	ImGui::SliderFloat( "Height bias", &s_heightBias, 0.0f, 64.0f, "%.0f" );

	ImGui::Separator();
	if ( terrainLoaded() )
	{
		const long  side  = Terrain::realVerticesMapSide;
		const int   chunks = ( s_superchunkChunks < 1 ) ? 1 : s_superchunkChunks;
		const int   chunksAcross = (int)( ( side + kChunkCells - 1 ) / kChunkCells );
		const int   superAcross  = ( chunksAcross + chunks - 1 ) / chunks;
		ImGui::Text( "Terrain: %ld x %ld verts", side, side );
		ImGui::Text( "Chunks across: %d  (chunk = %d cells)", chunksAcross, kChunkCells );
		ImGui::Text( "Superchunks across: %d  (= %d cells)", superAcross, kChunkCells * chunks );
		ImGui::Text( "Water elevation: %.1f", Terrain::waterElevation );
	}
	else
	{
		ImGui::TextDisabled( "(no terrain loaded)" );
	}

	if ( s_showFoliageDebug )
	{
		ImGui::Separator();
		ImGui::Text( "Foliage instances: %d", FoliageRender::Count() );
		ImGui::Text( "Foliage preview: %s", FoliageRender::Visible() ? "visible" : "hidden" );
	}

	ImGui::End();
}

#endif // MC2_IMGUI

} // namespace EditorDebugOverlay
