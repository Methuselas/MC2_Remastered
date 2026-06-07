//---------------------------------------------------------------------------
//
// Terrain.cpp -- File contains calss definitions for the Terrain
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef ERR_H
#include"err.h"   // C1b temporal-superset: 3-arg Assert() for worldToBlockIdx guard
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef TERRTXM_H
#include"terrtxm.h"
#include"tex_resolve_table.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"
#include"../GameOS/gameos/gos_terrain_water_stream.h"
#include"../GameOS/gameos/gpu_driven_common.h"
#include"../GameOS/gameos/gos_terrain_indirect.h"
#include"../GameOS/gameos/gos_terrain_surface.h"  // PR-1: continuous-surface mission-load generation
#include"../GameOS/gameos/gos_terrain_mask_dispatch.h"
#include"../GameOS/gameos/gos_terrain_bridge.h"
#include"../GameOS/gameos/gos_terrain_lighting.h"
#include"../GameOS/gameos/gos_terrain_height_tex.h"  // TERRAIN-NORMALS-FROM-HEIGHT-1
#include"../GameOS/gameos/gos_terrain_lod_chunk.h"   // Terrain LOD chunk Phase 1
#include"../GameOS/gameos/utils/logging.h"            // Terrain LOD chunk Phase 2: throttled false-negative log
#include"terrain_admission_mode.h"  // F6 T2: shared isModern() flag for terrain.cpp + quad.cpp

#include <vector>
#include <cstdint>

// Externals from quad.cpp / mapdata.cpp / mechcmd2.cpp used by the water fast path.
extern float MaxMinUV;
extern float cloudScrollX;
extern float cloudScrollY;
extern long  sprayFrame;
extern bool  useWaterInterestTexture;

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef USERINPUT_H
#include"userinput.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef PACKET_H
#include"packet.h"
#endif

#ifndef INIFILE_H
#include"fitinifile.h"
#endif

#ifndef TGAINFO_H
#include"tgainfo.h"
#endif

//---------------------------------------------------------------------------
// Static Globals
float worldUnitsPerMeter = 5.01f;
float metersPerWorldUnit = 0.2f;
long terrainLineChanged = 0;

MapDataPtr					Terrain::mapData = NULL;
TerrainTexturesPtr			Terrain::terrainTextures = NULL;
TerrainColorMapPtr			Terrain::terrainTextures2 = NULL;

const long					Terrain::verticesBlockSide = 20;			//Changes for new terrain?
long						Terrain::blocksMapSide = 0;					//Calced during load.

long						Terrain::visibleVerticesPerSide = 0;		//Passed in.

const float					Terrain::worldUnitsPerVertex = 128.0;
const float					Terrain::worldUnitsPerCell = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
const float					Terrain::halfWorldUnitsPerCell = Terrain::worldUnitsPerCell / 2.0f;
const float					Terrain::metersPerCell = Terrain::worldUnitsPerCell * metersPerWorldUnit;
const float					Terrain::worldUnitsBlockSide = Terrain::worldUnitsPerVertex * Terrain::verticesBlockSide;
const float					Terrain::oneOverWorldUnitsPerVertex = 1.0f / Terrain::worldUnitsPerVertex;
const float					Terrain::oneOverWorldUnitsPerCell = 1.0f / Terrain::worldUnitsPerCell;
const float					Terrain::oneOverMetersPerCell = 1.0f / Terrain::metersPerCell;
const float					Terrain::oneOverVerticesBlockSide = 1.0f / Terrain::verticesBlockSide;

float						Terrain::worldUnitsMapSide = 0.0;		//Calced during load.
float						Terrain::oneOverWorldUnitsMapSide = 0.0f;
long						Terrain::halfVerticesMapSide = 0;
long						Terrain::realVerticesMapSide = 0;

Stuff::Vector3D				Terrain::mapTopLeft3d;					//Calced during load.

UserHeapPtr					Terrain::terrainHeap = NULL;			//Setup at load time.
char *						Terrain::terrainName = NULL;
char * 						Terrain::colorMapName = NULL;

// C1 tactical material profile (see terrain.h). Default LEGACY = exact
// pre-C1 byte-for-byte behavior; only mc2_24 currently flips to a
// non-LEGACY profile.
int							g_terrainMaterialProfile = TERRAIN_MAT_PROFILE_LEGACY;

long		   				Terrain::numObjBlocks = 0;
ObjBlockInfo				*Terrain::objBlockInfo = NULL;
bool						*Terrain::objVertexActive = NULL;

float 						*Terrain::tileRowToWorldCoord = NULL;
float 						*Terrain::tileColToWorldCoord = NULL;
float 						*Terrain::cellToWorldCoord = NULL;
float 						*Terrain::cellColToWorldCoord = NULL;
float 						*Terrain::cellRowToWorldCoord = NULL;

float 						Terrain::waterElevation = 0.0f;
float						Terrain::frameAngle = 0.0f;
float 						Terrain::frameCos = 1.0f;
float						Terrain::frameCosAlpha = 1.0f;
DWORD						Terrain::alphaMiddle = 0xaf000000;
DWORD						Terrain::alphaEdge = 0x3f000000;
DWORD						Terrain::alphaDeep = 0xff000000;
float						Terrain::waterFreq = 4.0f;
float						Terrain::waterAmplitude = 10.0f;

long						Terrain::userMin = 0;
long						Terrain::userMax = 0;
unsigned long				Terrain::baseTerrain = 0;
unsigned char				Terrain::fractalThreshold = 1;
unsigned char				Terrain::fractalNoise = 0;
bool						Terrain::recalcShadows = false;
bool						Terrain::recalcLight = false;

// Terrain LOD chunk Phase 1 static members (MC2_TERRAIN_LOD_CHUNK=1 gate).
TerrainBlockMeta*			Terrain::s_blockMeta       = nullptr;
SuperchunkMeta*				Terrain::s_superchunkMeta  = nullptr;
TerrainDrawCommand*			Terrain::s_drawCmds        = nullptr;
float*						Terrain::s_skirtDepths     = nullptr;
int							Terrain::s_cmdCount        = 0;
unsigned long				Terrain::gCurrentFrame     = 0;
int							Terrain::s_terrainChunkSide = 0;
int							Terrain::s_superchunkSide   = 0;

// ---------------------------------------------------------------------------
// Terrain LOD chunk Phase 5 — per-block distance LOD selection.
// LOD_STEPS[i] is the vertex stride baked into each patch VBO.
// LOD_DIST_THRESH[k] is the upper distance (world units) for lodLevel k.
// Each block is 20 quads * 128 wu/quad = 2560 wu per side.
// ---------------------------------------------------------------------------
static const int   LOD_STEPS[6]      = {1, 2, 4, 5, 10, 20};
static const float LOD_DIST_THRESH[5] = {
    3000.0f,   // lodLevel 0: lodStep=1  — within 3 K wu
    7000.0f,   // lodLevel 1: lodStep=2  — within 7 K wu
    15000.0f,  // lodLevel 2: lodStep=4  — within 15 K wu
    30000.0f,  // lodLevel 3: lodStep=5  — within 30 K wu
    60000.0f,  // lodLevel 4: lodStep=10 — within 60 K wu
               // lodLevel 5: lodStep=20 — beyond 60 K wu
};

// Choose LOD level (0-5) for a block given squared distance and previous level.
// Promotion (going finer) is immediate; demotion (going coarser) uses 10%
// hysteresis in linear distance (= 1.21x in distSq) to prevent flickering.
static uint8_t chooseLodLevel(float distSq, uint8_t prevLevel)
{
    uint8_t desired = 5;
    for (int k = 0; k < 5; ++k) {
        if (distSq < LOD_DIST_THRESH[k] * LOD_DIST_THRESH[k]) {
            desired = (uint8_t)k;
            break;
        }
    }
    if (desired > prevLevel) {
        // Demotion — only commit if clearly past the current fine threshold.
        float thresh = LOD_DIST_THRESH[desired - 1];
        if (distSq < thresh * thresh * 1.21f)
            return prevLevel; // stay fine
    }
    return desired;
}

bool 						drawTerrainGrid = false;		//Override locally in editor so game don't come with these please!  Love -fs
bool						drawLOSGrid = false;
bool						drawTerrainTiles = true;
bool						drawTerrainOverlays = true;
bool						drawTerrainMines = true;
bool						renderObjects = true;
bool						renderTrees = true;

TerrainPtr					land = NULL;

long 						*usedBlockList;					//Used to determine what objects to deal with.
long 						*moverBlockList;

unsigned long 				blockMemSize = 0;				//Misc Flags.
bool 						useOldProject = FALSE;
bool 						projectAll = FALSE;
bool 						useFog = true;
bool 						useVertexLighting = true;
bool 						useFaceLighting = false;
extern bool					useRealLOS;

unsigned char 				godMode = 0;			//Can I simply see everything, enemy and friendly?

extern long 				DrawDebugCells;

#define						MAX_TERRAIN_HEAP_SIZE		1024000

long						visualRangeTable[256];
// NS3 boundary: justResaveAllMaps canonical def (was code/ leak; editor consumes it).
bool justResaveAllMaps = false;
//---------------------------------------------------------------------------
// These are used to determine what terrain objects to process.
// They date back to GenCon 1996!!
void addBlockToList (long blockNum)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	for (long i=0;i<totalBlocks;i++)
	{
		if (usedBlockList[i] == blockNum)
		{
			return;
		}
		else if (usedBlockList[i] == -1)
		{
			usedBlockList[i] = blockNum;
			return;
		}
	}
}

//---------------------------------------------------------------------------
void addMoverToList (long blockNum)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	for (long i=0;i<totalBlocks;i++)
	{
		if (moverBlockList[i] == blockNum)
		{
			return;
		}
		else if (moverBlockList[i] == -1)
		{
			moverBlockList[i] = blockNum;
			return;
		}
	}
}

//---------------------------------------------------------------------------
void clearList (void)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	blockMemSize = totalBlocks * sizeof(long);
	
	if (usedBlockList)
		memset(usedBlockList,-1,blockMemSize);
}

//---------------------------------------------------------------------------
void clearMoverList (void)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	blockMemSize = totalBlocks * sizeof(long);
	
	if (moverBlockList)
		memset(moverBlockList,-1,blockMemSize);
}

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Terrain LOD chunk Phase 1 static helpers.
// Only called when MC2_TERRAIN_LOD_CHUNK is set; all access already gated.

static void recomputeBlockAabb(TerrainBlockMeta& bm)
{
    // Scan the (quadCountX+1) x (quadCountY+1) inclusive vertex footprint.
    // OOB vertices (past map edge) use blankVertex elevation (33.0f).
    float mn =  1e30f;
    float mx = -1e30f;
    int mapSide = (int)Terrain::realVerticesMapSide;
    PostcompVertexPtr blks = Terrain::mapData->getBlocks();
    for (int dy = 0; dy <= bm.quadCountY; ++dy) {
        for (int dx = 0; dx <= bm.quadCountX; ++dx) {
            int vx = bm.originX + dx;
            int vy = bm.originY + dy;
            float elev;
            if (vx >= mapSide || vy >= mapSide)
                elev = 33.0f;   // blankVertex elevation (mapdata.cpp:641)
            else
                elev = blks[vx + vy * mapSide].elevation;
            if (elev < mn) mn = elev;
            if (elev > mx) mx = elev;
        }
    }
    bm.minElev   = mn;
    bm.maxElev   = mx;
    bm.dirtyAabb = false;
}

static void recomputeSuperchunkAabb(int scX, int scY)
{
    SuperchunkMeta& sc = Terrain::s_superchunkMeta[scX + scY * Terrain::s_superchunkSide];
    float xMin =  1e30f, xMax = -1e30f;
    float yMin =  1e30f, yMax = -1e30f;
    float zMin =  1e30f, zMax = -1e30f;

    float halfMap = Terrain::worldUnitsMapSide * 0.5f;

    for (int dy = 0; dy < 4; ++dy) {
        for (int dx = 0; dx < 4; ++dx) {
            int bx = scX * 4 + dx, by = scY * 4 + dy;
            if (bx >= Terrain::s_terrainChunkSide || by >= Terrain::s_terrainChunkSide) continue;
            const TerrainBlockMeta& bm =
                Terrain::s_blockMeta[bx + by * Terrain::s_terrainChunkSide];
            float wMinX =  float(bm.originX)                  * 128.0f - halfMap;
            float wMaxX =  float(bm.originX + bm.quadCountX)  * 128.0f - halfMap;
            float wMaxY =  halfMap - float(bm.originY)                  * 128.0f;
            float wMinY =  halfMap - float(bm.originY + bm.quadCountY)  * 128.0f;
            if (wMinX < xMin) xMin = wMinX;
            if (wMaxX > xMax) xMax = wMaxX;
            if (wMinY < yMin) yMin = wMinY;
            if (wMaxY > yMax) yMax = wMaxY;
            if (bm.minElev < zMin) zMin = bm.minElev;
            if (bm.maxElev > zMax) zMax = bm.maxElev;
        }
    }
    sc.worldMinX = xMin; sc.worldMaxX = xMax;
    sc.worldMinY = yMin; sc.worldMaxY = yMax;
    sc.worldMinZ = zMin; sc.worldMaxZ = zMax;
    sc.inFrustum = false;
}

//---------------------------------------------------------------------------
// class Terrain
void Terrain::init (void)
{
	vertexList = NULL;
	numberVertices = 0;
	
	quadList = NULL;
	numberQuads = 0;
}

//---------------------------------------------------------------------------
void Terrain::initMapCellArrays (void)
{
	if (!tileRowToWorldCoord)
	{
		tileRowToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * realVerticesMapSide);
		gosASSERT(tileRowToWorldCoord != NULL);
	}

	if (!tileColToWorldCoord)
	{
		tileColToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * realVerticesMapSide); 
		gosASSERT(tileColToWorldCoord != NULL);
	}

	if (!cellToWorldCoord)
	{
		cellToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * MAPCELL_DIM); 
		gosASSERT(cellToWorldCoord != NULL);
	}

	if (!cellColToWorldCoord)
	{
		cellColToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * realVerticesMapSide * MAPCELL_DIM); 
		gosASSERT(cellColToWorldCoord != NULL);
	}

	if (!cellRowToWorldCoord)
	{
		cellRowToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * realVerticesMapSide * MAPCELL_DIM); 
		gosASSERT(cellRowToWorldCoord != NULL);
	}

	long i=0;

	long height = realVerticesMapSide, width = height;
	for (i = 0; i < height; i++)
		tileRowToWorldCoord[i] = (worldUnitsMapSide / 2.0) - (i * worldUnitsPerVertex);

	for (i = 0; i < width; i++)
		tileColToWorldCoord[i] = (i * worldUnitsPerVertex) - (worldUnitsMapSide / 2.0);

	for (i = 0; i < MAPCELL_DIM; i++)
		cellToWorldCoord[i] = (worldUnitsPerVertex / (float)MAPCELL_DIM) * i;

	long maxCell = height * MAPCELL_DIM;
	for (i = 0; i < maxCell; i++)
		cellRowToWorldCoord[i] = (worldUnitsMapSide / 2.0) - (i * worldUnitsPerCell);

	maxCell = width * MAPCELL_DIM;
	for (i = 0; i < maxCell; i++)
		cellColToWorldCoord[i] = (i * worldUnitsPerCell) - (worldUnitsMapSide / 2.0);
}	

//---------------------------------------------------------------------------
long Terrain::init (PacketFile* pakFile, int whichPacket, unsigned long visibleVertices, volatile float& percent,
					float percentRange )
{
	clearList();
	clearMoverList();
	
	long result = pakFile->seekPacket( whichPacket );
	if (result != NO_ERR)
		STOP(("Unable to seek Packet %d in file %s",whichPacket,pakFile->getFilename()));
	
	int tmp = pakFile->getPacketSize();
	realVerticesMapSide = sqrt( float(tmp/ sizeof(PostcompVertex)));
	
	if (!justResaveAllMaps)
	{
		if (realVerticesMapSide < 60 || realVerticesMapSide > 2048)
			STOP(("Terrain grid size %d out of supported range [60, 2048]", realVerticesMapSide));
		// Check quads (vertices-1), not vertices, so partial-edge blocks are permitted
		// under MC2_TERRAIN_LOD_CHUNK=1 (e.g. 120-vertex map: 119 quads, last block = 19).
		if ((realVerticesMapSide - 1) % verticesBlockSide != 0) {
			if (!getenv("MC2_TERRAIN_LOD_CHUNK"))
				STOP(("Terrain quad count %d not divisible by verticesBlockSide (%d)",
				      realVerticesMapSide - 1, verticesBlockSide));
			// Partial-edge blocks permitted under MC2_TERRAIN_LOD_CHUNK=1.
			// quadCountX = min(verticesBlockSide, (realVerticesMapSide-1) - originX).
		}
	}
	
	init( realVerticesMapSide, pakFile, visibleVertices, percent, percentRange );	
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
void Terrain::getColorMapName (FitIniFile *file)
{
	if (file)
	{
		if (file->seekBlock("ColorMap") == NO_ERR)
		{
			char mapName[1024];
			if (file->readIdString("ColorMapName",mapName,1023) == NO_ERR)
			{
				colorMapName = new char[strlen(mapName)+1];
				strcpy(colorMapName,mapName);
				return;
			}
		}
	}

	colorMapName = NULL;
}

//---------------------------------------------------------------------------
void Terrain::setColorMapName (char *mapName)
{
	if (colorMapName)
	{
		delete [] colorMapName;
		colorMapName = NULL;
	}

	if (mapName)
	{
		colorMapName = new char [strlen(mapName)+1];
		strcpy(colorMapName,mapName);
	}
}

//---------------------------------------------------------------------------
void Terrain::saveColorMapName (FitIniFile *file)
{
	if (file && colorMapName)
	{
		file->writeBlock("ColorMap");
		file->writeIdString("ColorMapName",colorMapName);
	}
}

//---------------------------------------------------------------------------
long Terrain::init( unsigned long verticesPerMapSide, PacketFile* pakFile, unsigned long visibleVertices,
				   volatile float& percent,
					float percentRange)
{
	ZoneScopedN("Terrain::init");
	realVerticesMapSide = verticesPerMapSide;
	halfVerticesMapSide = realVerticesMapSide >> 1;
	blocksMapSide = realVerticesMapSide / verticesBlockSide;
	worldUnitsMapSide = realVerticesMapSide * worldUnitsPerVertex;
	if (worldUnitsMapSide > Stuff::SMALL)
		oneOverWorldUnitsMapSide = 1.0f / worldUnitsMapSide;
	else
		oneOverWorldUnitsMapSide = 0.0f;

	// Tell GameOS the map extent for static shadow projection
	gos_SetMapHalfExtent(worldUnitsMapSide * 0.5f);

	Terrain::numObjBlocks = blocksMapSide * blocksMapSide;
	visibleVerticesPerSide = visibleVertices;
	terrainHeapSize = MAX_TERRAIN_HEAP_SIZE;

	//-----------------------------------------------------------------
	// Startup to Terrain Heap
	if( !terrainHeap )
	{
		ZoneScopedN("Terrain::init terrainHeap");
		terrainHeap = new UserHeap;
		gosASSERT(terrainHeap != NULL);
		terrainHeap->init(terrainHeapSize,"TERRAIN");
	}

	percent += percentRange/5.f;
	//-----------------------------------------------------------------
	// Startup the Terrain Texture Maps
	if ( !terrainTextures )
	{
		ZoneScopedN("Terrain::init terrainTextures");
		char baseName[256];
		if (pakFile)
		{
			_splitpath(pakFile->getFilename(),NULL,NULL,baseName,NULL);
		}
		else
		{
			strcpy(baseName,"newmap");
		}

		terrainTextures = new TerrainTextures;
		terrainTextures->init("textures",baseName);
	}

	percent += percentRange/5.f;


	if ( !pakFile && !realVerticesMapSide )
		return NO_ERR;

	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
	if ( !terrainTextures2 && pakFile)
	{
		ZoneScopedN("Terrain::init terrainColorMap");
		char name[1024];

		_splitpath(pakFile->getFilename(),NULL,NULL,name,NULL);
		terrainName = new char[strlen(name)+1];
		strcpy(terrainName,name);

		if (colorMapName)
			strcpy(name,colorMapName);

		FullPathFileName tgaColorMapName;
		tgaColorMapName.init(texturePath,name,".tga");
		
		FullPathFileName tgaColorMapBurninName;
		tgaColorMapBurninName.init(texturePath,name,".burnin.tga");

		FullPathFileName tgaColorMapJPGName;
		tgaColorMapJPGName.init(texturePath,name,".burnin.jpg");
				
		if (fileExists(tgaColorMapName) || fileExists(tgaColorMapBurninName) || fileExists(tgaColorMapJPGName))
		{
			terrainTextures2 = new TerrainColorMap;		//Otherwise, this will stay NULL and we know not to use them
		}
	}

	percent += percentRange/5.f;


	mapTopLeft3d.x = -worldUnitsMapSide / 2.0f;
	mapTopLeft3d.y = worldUnitsMapSide / 2.0f;

	percent += percentRange/5.f;


	//----------------------------------------------------------------------
	// Setup number of blocks
	long numberBlocks = blocksMapSide * blocksMapSide;
	
	numObjBlocks = numberBlocks;
	objBlockInfo = (ObjBlockInfo *)terrainHeap->Malloc(sizeof(ObjBlockInfo)*numObjBlocks);
	gosASSERT(objBlockInfo != NULL);
	
	memset(objBlockInfo,0,sizeof(ObjBlockInfo)*numObjBlocks);
	
	objVertexActive = (bool *)terrainHeap->Malloc(sizeof(bool) * realVerticesMapSide * realVerticesMapSide);
	gosASSERT(objVertexActive != NULL);
	
	memset(objVertexActive,0,sizeof(bool)*numObjBlocks);
	
	moverBlockList = (long *)terrainHeap->Malloc(sizeof(long) * numberBlocks);
	gosASSERT(moverBlockList != NULL);
	
	usedBlockList = (long *)terrainHeap->Malloc(sizeof(long) * numberBlocks);
	gosASSERT(usedBlockList != NULL);
	
	clearList();
	clearMoverList();

	//----------------------------------------------------------------------
	// Calculate size of each mapblock
	long blockSize = verticesBlockSide * verticesBlockSide;
	blockSize *= sizeof(PostcompVertex);

	//----------------------------------------------------------------------
	// Create the MapBlock Manager and allocate its RAM
	if ( !mapData )
	{
		mapData = new MapData;
		if ( pakFile )
			mapData->newInit( pakFile, realVerticesMapSide*realVerticesMapSide);
		else
			mapData->newInit( realVerticesMapSide*realVerticesMapSide );

		mapTopLeft3d.z = mapData->getTopLeftElevation();

		// TERRAIN-NORMALS-FROM-HEIGHT-1: upload an R32F height texture from
		// the now-resident heightfield so the terrain fragment shader can
		// derive macroscopic surface normals when MC2_TERRAIN_NORMALS_FROM_HEIGHT
		// is set. Visual-only; gameplay height (getTerrainElevation) remains
		// authoritative. sizeof(PostcompVertex) = 32 with float elevation at
		// byte 12 (mclib/vertex.h:32-60).
		if (mapData->getBlocks()) {
			gos_uploadTerrainHeightTex(
				(int)realVerticesMapSide,
				mapData->getBlocks(),
				(int)sizeof(PostcompVertex),
				/*elevationOffset=*/12,
				mapTopLeft3d.x,
				mapTopLeft3d.y,
				worldUnitsPerVertex);
		}
	}

	percent += percentRange/5.f;

	//----------------------------------------------------------------------
	// Terrain LOD chunk Phase 1 — allocate per-block + superchunk metadata.
	// MC2_TERRAIN_LOD_CHUNK=1 gate. mapData must be live before this block.
	if (getenv("MC2_TERRAIN_LOD_CHUNK")) {
		// terrainChunkSide = ceil((vertices-1) / verticesBlockSide)
		s_terrainChunkSide = ((realVerticesMapSide - 1) + verticesBlockSide - 1) / verticesBlockSide;
		s_superchunkSide   = (s_terrainChunkSide + 3) / 4;

		int nBlocks      = s_terrainChunkSide * s_terrainChunkSide;
		int nSuperchunks = s_superchunkSide   * s_superchunkSide;

		s_blockMeta      = (TerrainBlockMeta*)  terrainHeap->Malloc(sizeof(TerrainBlockMeta)   * nBlocks);
		s_superchunkMeta = (SuperchunkMeta*)    terrainHeap->Malloc(sizeof(SuperchunkMeta)      * nSuperchunks);
		s_drawCmds       = (TerrainDrawCommand*)terrainHeap->Malloc(sizeof(TerrainDrawCommand)  * nBlocks);
		s_skirtDepths    = new float[nBlocks];

		gosASSERT(s_blockMeta      != NULL);
		gosASSERT(s_superchunkMeta != NULL);
		gosASSERT(s_drawCmds       != NULL);
		gosASSERT(s_skirtDepths    != NULL);

		memset(s_blockMeta,      0, sizeof(TerrainBlockMeta)   * nBlocks);
		memset(s_superchunkMeta, 0, sizeof(SuperchunkMeta)      * nSuperchunks);
		memset(s_drawCmds,       0, sizeof(TerrainDrawCommand)  * nBlocks);
		memset(s_skirtDepths,    0, sizeof(float)               * nBlocks);

		gCurrentFrame = 1;
		s_cmdCount    = 0;

		// Initialize per-block metadata.
		for (int by = 0; by < s_terrainChunkSide; ++by) {
			for (int bx = 0; bx < s_terrainChunkSide; ++bx) {
				TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];
				bm.originX    = bx * (int)verticesBlockSide;
				bm.originY    = by * (int)verticesBlockSide;
				int qx = (int)(realVerticesMapSide - 1) - bm.originX;
				int qy = (int)(realVerticesMapSide - 1) - bm.originY;
				bm.quadCountX = (qx < (int)verticesBlockSide) ? qx : (int)verticesBlockSide;
				bm.quadCountY = (qy < (int)verticesBlockSide) ? qy : (int)verticesBlockSide;
				bm.dirtyAabb  = false;
				bm.inFrustum  = false;
				bm.lodLevel   = 0;
				if (mapData && mapData->getBlocks())
					recomputeBlockAabb(bm);
			}
		}

		// Initialize superchunk AABBs.
		for (int scY = 0; scY < s_superchunkSide; ++scY)
			for (int scX = 0; scX < s_superchunkSide; ++scX)
				recomputeSuperchunkAabb(scX, scY);

		// Terrain LOD chunk Phase 3 — upload full heightfield to GPU SSBO.
		{
			int n = (int)realVerticesMapSide * (int)realVerticesMapSide;
			std::vector<float> elev((size_t)n);
			const PostcompVertex* blks = mapData->getBlocks();
			for (int i = 0; i < n; ++i)
				elev[i] = blks[i].elevation;
			gos_TerrainLodChunk_UploadHeightFull(elev.data(), (int)realVerticesMapSide);
		}
	}

	//----------------------------------------------------------------------
	// Create the VertexList
	numberVertices = 0;
	vertexList = (VertexPtr)terrainHeap->Malloc(sizeof(Vertex) * visibleVertices * visibleVertices);
	gosASSERT(vertexList != NULL);
	memset(vertexList,0,sizeof(Vertex) * visibleVertices * visibleVertices);

	//----------------------------------------------------------------------
	// Create the QuadList
	numberQuads = 0;
	quadList = (TerrainQuadPtr)terrainHeap->Malloc(sizeof(TerrainQuad) * visibleVertices * visibleVertices);
	gosASSERT(quadList != NULL);
	memset(quadList,0,sizeof(TerrainQuad) * visibleVertices * visibleVertices);

	//-------------------------------------------------------------------
	initMapCellArrays();

	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
	if ( terrainTextures2  && !(terrainTextures2->colorMapStarted))
	{
		if (colorMapName)
			terrainTextures2->init(colorMapName);
		else
			terrainTextures2->init(terrainName);
	}

	//-----------------------------------------------------------------
	// C1 tactical material profile selection. Whitelist-only; default
	// LEGACY preserves byte-for-byte pre-C1 rendering on every other
	// mission. Profile is read by gos_terrain.frag / terrain_common.hglsl
	// classifier branches via the `g_terrainMaterialProfile` uniform.
	{
		const char *profileKey = colorMapName ? colorMapName
		                       : terrainName  ? terrainName
		                                      : "";
		g_terrainMaterialProfile = TERRAIN_MAT_PROFILE_LEGACY;
		// TERRAIN-CLASSIFY-TUNING-1: sync dirt sat window with profile so the
		// ImGui-tunable uniforms start at the right defaults for this mission.
		// Sand_M24 washes out to low-saturation sand; widen the dirt sat gate.
		extern void gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi);
		if (profileKey[0] != '\0' && _stricmp(profileKey, "mc2_24") == 0) {
			g_terrainMaterialProfile = TERRAIN_MAT_PROFILE_SAND_M24;
			// Sand_M24: widen R-G band + raise brightness ceiling for bright sun-lit sand
			gos_SetTerrainClassDirt(-0.02f, 0.12f, 0.18f, 0.80f);
		} else {
			gos_SetTerrainClassDirt(-0.02f, 0.06f, 0.22f, 0.45f);
		}
	}

	return NO_ERR;
}

void Terrain::resetVisibleVertices (long maxVisibleVertices)
{
	terrainHeap->Free(vertexList);
	vertexList = NULL;

	terrainHeap->Free(quadList);
	quadList = NULL;

	visibleVerticesPerSide = maxVisibleVertices;
	//----------------------------------------------------------------------
	// Create the VertexList
	numberVertices = 0;
	vertexList = (VertexPtr)terrainHeap->Malloc(sizeof(Vertex) * visibleVerticesPerSide * visibleVerticesPerSide);
	gosASSERT(vertexList != NULL);
	memset(vertexList,0,sizeof(Vertex) * visibleVerticesPerSide * visibleVerticesPerSide);

	//----------------------------------------------------------------------
	// Create the QuadList
	numberQuads = 0;
	quadList = (TerrainQuadPtr)terrainHeap->Malloc(sizeof(TerrainQuad) * visibleVerticesPerSide * visibleVerticesPerSide);
	gosASSERT(quadList != NULL);
	memset(quadList,0,sizeof(TerrainQuad) * visibleVerticesPerSide * visibleVerticesPerSide);

	
}

//---------------------------------------------------------------------------
void Terrain::primeMissionTerrainCache (volatile float& progress, float progressRange)
{
	if (!mapData || !terrainTextures2)
		return;

	const float buildRange = progressRange * 0.5f;
	const float warmRange = progressRange - buildRange;
	{
		ZoneScopedN("Terrain::primeMissionTerrainCache build");
		mapData->buildTerrainFaceCache(&progress, buildRange);
	}
	{
		ZoneScopedN("Terrain::primeMissionTerrainCache warm");
		mapData->warmTerrainFaceCacheResidency(&progress, warmRange);
	}

	// Stage 2 of the renderWater architectural slice (CPU→GPU offload):
	// build the static, map-keyed WaterRecipe array. Iterates MapData::blocks
	// directly (mission-immutable) — independent of quadList which is
	// camera-windowed and reshuffles each frame. Spec:
	// docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.
	{
		ZoneScopedN("Terrain::primeMissionTerrainCache water_stream_build");
		WaterStream::Reset();
		WaterStream::Build();
	}

	// Stage 2 of the indirect-terrain SOLID-only PR1 (CPU→GPU offload):
	// build the dense TerrainQuadRecipe array (mapSide² × 144 B) indexed by
	// vertexNum. Called AFTER buildTerrainFaceCache (line 585) so the Shape C
	// cache is ready when buildRecipeSlot reads UV data from it.
	// Gated on IsEnabled() OR IsParityCheckEnabled() — no allocation when both
	// are unset.
	// PR-1 (terrain continuous-surface producer): the surface generator
	// de-duplicates the dense recipe corners as its stock-derivable source
	// (gos_terrain_surface.cpp, design M-4), so the recipe MUST be built when
	// the surface kill-switch is on even if MC2_TERRAIN_INDIRECT is off. This
	// is the M-4 stock-only contract: the surface generation source is the
	// mission-load recipe, with no arming precondition.
	if (gos_terrain_indirect::IsEnabled() ||
	    gos_terrain_indirect::IsParityCheckEnabled() ||
	    gos_terrain_surface::IsEnabled()) {
		gos_terrain_indirect::ResetDenseRecipe();
		gos_terrain_indirect::BuildDenseRecipe();
	}

	// PR-1: mission-load continuous-surface generation (Wave 1, ADDITIVE,
	// default-OFF). No-op unless MC2_TERRAIN_SURFACE is set. Runs AFTER
	// BuildDenseRecipe (its stock-derivable generation source) and emits the
	// [TERRAIN_SURFACE v1] mission-load lifecycle + stock-only-fence prints.
	// PR-1 generates only -- the surface is NOT drawn/consumed yet (PR-2..4).
	gos_terrain_surface::GenerateForMission();

	// Slice B4 Stage 1a — mask-dispatch lifecycle. Same call site as
	// BuildDenseRecipe (no-op when MC2_TERRAIN_MASK_DISPATCH unset).
	gos_terrain_mask_dispatch::Init(realVerticesMapSide);

	// PR2c Stage 1c — mine static-bake lifecycle.
	// CPU-clear only; do NOT build here. Build is deferred to first
	// MissionMap::setMine event (typically the per-cell init loop at
	// move.cpp:991) followed by a paint-cycle invocation of
	// RebuildMineStaticVBOIfDirty from the Stage 2c bridge. This avoids
	// the R7 timing trap (mineTextureHandle/blownTextureHandle are still
	// 0xffffffff at primeMissionTerrainCache time — they lazy-load only
	// when TerrainQuad::setupTextures fires per-quad in the first paint
	// cycle).
	gos_terrain_indirect::ResetMineStaticVBO();
	gos_terrain_indirect::ResetMineTextureArray();

	// Slice A — cement-overlay static-bake lifecycle. CPU-clear only; do NOT
	// build here. Mirrors the mine R7 timing-trap mitigation EXACTLY: the
	// overlay texture handles lazy-load in TerrainQuad::setupTextures during
	// the first paint cycle (before Render.TerrainOverlaysStatic fires), so
	// the build is deferred to the first armed DrawDecalStatic via
	// RebuildDecalStaticVBOIfDirty. ResetDecalStaticVBO leaves the dirty flag
	// set so that first armed draw bakes.
	gos_terrain_indirect::ResetDecalStaticVBO();
}

//---------------------------------------------------------------------------
long Terrain::worldToBlockIdx (float wx, float wy)
{
	// Verbatim transcription of GameObject::getBlockAndVertexNumber's block
	// math (gameobj.cpp). float2long truncation, the >>7 (==/128), and the
	// Y-flip are ALL load-bearing — do NOT "clean up" the float math.
	Assert(Terrain::worldUnitsPerVertex==128,0," block >>7 broken ");

	long mx = (float2long(wx) >> 7) + Terrain::halfVerticesMapSide;
	long blockX = float2long(mx * Terrain::oneOverVerticesBlockSide);

	long my = Terrain::halfVerticesMapSide - ((float2long(wy) >> 7) + 1);
	long blockY = float2long(my * Terrain::oneOverVerticesBlockSide);

	return blockX + (blockY * Terrain::blocksMapSide);
}

//---------------------------------------------------------------------------
bool Terrain::IsValidTerrainPosition (const Stuff::Vector3D pos)
{
	float metersCheck = (Terrain::worldUnitsMapSide / 2.0f);

	if ((pos.x > -metersCheck) &&
		(pos.x < metersCheck) &&
		(pos.y > -metersCheck) &&
		(pos.y < metersCheck))
	{
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------
bool Terrain::IsEditorSelectTerrainPosition (const Stuff::Vector3D pos)
{
	float metersCheck = (Terrain::worldUnitsMapSide / 2.0f) - Terrain::worldUnitsPerVertex;

	if ((pos.x > -metersCheck) &&
		(pos.x < metersCheck) &&
		(pos.y > -metersCheck) &&
		(pos.y < metersCheck))
	{
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------
bool Terrain::IsGameSelectTerrainPosition (const Stuff::Vector3D pos)
{
	float metersCheck = (Terrain::worldUnitsMapSide / 2.0f) - (Terrain::worldUnitsPerVertex * 2.0f);

	if ((pos.x > -metersCheck) &&
		(pos.x < metersCheck) &&
		(pos.y > -metersCheck) &&
		(pos.y < metersCheck))
	{
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------
void Terrain::purgeTransitions (void)
{
	terrainTextures->purgeTransitions();
	mapData->calcTransitions();
}

//---------------------------------------------------------------------------
void Terrain::destroy (void)
{
	// VPL-#shadow C-1 (CRITICAL): re-arm the one-shot full-map static
	// terrain shadow so the NEXT mission rebuilds it against fresh
	// blocks[]. Without this, the build-once latch stays set process-
	// lifetime and mission 2+ would project mission 1's frozen shadow
	// over mission 2's terrain (strictly worse than the original bug).
	// Must pair with the Phase-1 camera-accumulate retirement (same
	// commit). blocks[] is one-shot repopulated at next MapData::newInit.
	gos_ResetStaticLightMatrix();

	// Per-mission dense recipe teardown (Stage 2 indirect-terrain PR1).
	// Called from Mission::destroy → land->destroy() once per mission exit.
	// CPU-clears state; GL buffer is kept for reuse by next mission's Build.
	if (gos_terrain_indirect::IsEnabled() ||
	    gos_terrain_indirect::IsParityCheckEnabled()) {
		gos_terrain_indirect::ResetDenseRecipe();
	}
	// PR-1: continuous-surface per-mission teardown. Unconditional and
	// idempotent (no-op when nothing was generated / kill-switch OFF); emits
	// the [TERRAIN_SURFACE v1] teardown lifecycle print when it had state.
	gos_terrain_surface::ResetForMission();
	// Unconditional — mirrors Init() placement (not gated on IsEnabled/IsParityCheck).
	// Stage 1b/1c may add per-mission state inside Reset(); guarding it here would
	// silently skip teardown when MC2_TERRAIN_MASK_DISPATCH=1 but MC2_TERRAIN_INDIRECT=0.
	gos_terrain_mask_dispatch::Reset();

	// Phase 1: terrain lighting GPU compute shutdown (per-mission teardown).
	gos_terrain_lighting::mission_shutdown();

	// PR2c Stage 1c — mine static-bake teardown. CPU-clear; keep GL buffer
	// + texture-array allocations for next-mission reuse.
	gos_terrain_indirect::ResetMineStaticVBO();
	gos_terrain_indirect::ResetMineTextureArray();

	// Slice A — cement-overlay static-bake teardown. CPU-clear; keep the
	// GL_STATIC_DRAW buffer allocation for next-mission reuse (mirror
	// ResetMineStaticVBO teardown placement).
	gos_terrain_indirect::ResetDecalStaticVBO();

	// Terrain LOD chunk Phase 1 teardown — free before terrainHeap destroy.
	// MC2_TERRAIN_LOD_CHUNK=1 gate; idempotent (NULL guards prevent double-free).
	if (getenv("MC2_TERRAIN_LOD_CHUNK")) {
		if (s_blockMeta)      { terrainHeap->Free(s_blockMeta);      s_blockMeta      = nullptr; }
		if (s_superchunkMeta) { terrainHeap->Free(s_superchunkMeta); s_superchunkMeta = nullptr; }
		if (s_drawCmds)       { terrainHeap->Free(s_drawCmds);       s_drawCmds       = nullptr; }
		delete[] s_skirtDepths; s_skirtDepths = nullptr;
		s_terrainChunkSide = 0;
		s_superchunkSide   = 0;
		s_cmdCount         = 0;
	}

	if (terrainTextures)
	{
		terrainTextures->destroy();
		delete terrainTextures;
		terrainTextures = NULL;
	}

	if (terrainTextures2)
	{
		terrainTextures2->destroy();
		delete terrainTextures2;
		terrainTextures2 = NULL;
	}

	delete mapData;
	mapData = NULL;

	if (terrainName)
	{
		delete [] terrainName;
		terrainName = NULL;
	}

	if (colorMapName)
	{
		delete [] colorMapName;
		colorMapName = NULL;
	}

	if (tileRowToWorldCoord)
	{
		terrainHeap->Free(tileRowToWorldCoord);
		tileRowToWorldCoord = NULL;
	}

	if (tileColToWorldCoord)
	{
		terrainHeap->Free(tileColToWorldCoord); 
		tileColToWorldCoord = NULL;
	}

	if (cellToWorldCoord)
	{
		terrainHeap->Free(cellToWorldCoord); 
		cellToWorldCoord = NULL;
	}

	if (cellColToWorldCoord)
	{
		terrainHeap->Free(cellColToWorldCoord); 
		cellColToWorldCoord = NULL;
	}

	if (cellRowToWorldCoord)
	{
		terrainHeap->Free(cellRowToWorldCoord); 
		cellRowToWorldCoord = NULL;
	}

	if (moverBlockList)
	{
		terrainHeap->Free(moverBlockList);
		moverBlockList = NULL;
	}

	if (usedBlockList)
	{
		terrainHeap->Free(usedBlockList);
		usedBlockList = NULL;
	}

	if (vertexList)
	{
		terrainHeap->Free(vertexList);
		vertexList = NULL;
	}

	if (quadList)
	{
		terrainHeap->Free(quadList);
		quadList = NULL;
	}

	if (objBlockInfo)
	{
		terrainHeap->Free(objBlockInfo);
		objBlockInfo = NULL;
	}
	
	if (objVertexActive)
	{
		terrainHeap->Free(objVertexActive);
		objVertexActive = NULL;
	}
	
 	if (terrainHeap)
	{
		terrainHeap->destroy();
		delete terrainHeap;
		terrainHeap = NULL;
	}
	
	numberVertices =
	numberQuads =
	
	halfVerticesMapSide = 
	realVerticesMapSide = 
		
	visibleVerticesPerSide =
	blocksMapSide = 0;
	
	worldUnitsMapSide = 0.0f;
	
	mapTopLeft3d.Zero();
		
	numObjBlocks = 0;

	recalcShadows = 
	recalcLight = false;

	//Reset these.  This will fix the mine problem.
	TerrainQuad::rainLightLevel = 1.0f;
	TerrainQuad::lighteningLevel = 0;
	TerrainQuad::mineTextureHandle = 0xffffffff;
	TerrainQuad::blownTextureHandle = 0xffffffff;
}

extern float textureOffset;
//---------------------------------------------------------------------------
long Terrain::update (void)
{
	ZoneScopedN("Terrain::update");
	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
	if ( terrainTextures2  && !(terrainTextures2->colorMapStarted))
	{
		ZoneScopedN("Terrain::update startColorMap");
		if (colorMapName)
			terrainTextures2->init(colorMapName);
		else
			terrainTextures2->init(terrainName);
	}

	//----------------------------------------------------------------
	// Nothing is ever visible.  We recalc every frame.  True LOS!
//	Terrain::VisibleBits->resetAll(0);
		
	if (godMode)	
	{
//		Terrain::VisibleBits->resetAll(0xff);
	}

	if (turn > terrainLineChanged+10)
	{
		ZoneScopedN("Terrain::update debugHotkeys");
		if (userInput->getKeyDown(KEY_UP) && userInput->ctrl() && userInput->alt() && !userInput->shift())
		{
			textureOffset += 0.1f;;
			terrainLineChanged = turn;
		}
		
		if (userInput->getKeyDown(KEY_DOWN) && userInput->ctrl() && userInput->alt() && !userInput->shift())
		{
			textureOffset -= 0.1f;;
			terrainLineChanged = turn;
		}
	}
	
 	//---------------------------------------------------------------------
	{
		ZoneScopedN("Terrain::update mapDataUpdate");
		Terrain::mapData->update();
	}
	// Terrain LOD chunk Phase 4: skip legacy vertex/quad list build when chunk
	// path owns rendering. makeLists populates vertexList/quadList used by the
	// per-quad draw() loop; that loop is suppressed under the same env gate.
	if (!getenv("MC2_TERRAIN_LOD_CHUNK"))
	{
		ZoneScopedN("Terrain::update makeLists");
		Terrain::mapData->makeLists(vertexList,numberVertices,quadList,numberQuads);
	}

	// -------------------------------------------------------------------------
	// Terrain LOD chunk Phase 4 — two-level AABB frustum cull.
	// MC2_TERRAIN_LOD_CHUNK=1 gate (s_blockMeta is nullptr when env unset).
	// Builds s_drawCmds[]; submitted via Terrain::flushDrawCommands() from
	// code/gamecam.cpp after shadow pass, before mcTextureManager->renderLists().
	if (s_blockMeta && s_superchunkMeta && s_drawCmds && eye)
	{
		ZoneScopedN("Terrain::update lodChunkCull");

		// Increment per-frame counter.
		++gCurrentFrame;

		// Reset draw-command list.
		s_cmdCount = 0;

		// Cache frustum planes once for this frame (eye->cacheFrustumPlanes()
		// is idempotent per-frame).
		eye->cacheFrustumPlanes();
		const float (*planes)[4] = eye->getCachedFrustumPlanes();

		const float halfMap = worldUnitsMapSide * 0.5f;

		// Camera position in MC2 world space for LOD distance metric.
		// getCameraOrigin() is in raw MC2 space; X/Y match the terrain world coords.
		Stuff::Vector3D camOriginLod = eye->getCameraOrigin();
		const float eyeX = camOriginLod.x;
		const float eyeY = camOriginLod.y;

		// --- Phase 5 Pass 1: superchunk cull + per-block frustum + LOD selection ---
		for (int scY = 0; scY < s_superchunkSide; ++scY)
		{
			for (int scX = 0; scX < s_superchunkSide; ++scX)
			{
				SuperchunkMeta& sc = s_superchunkMeta[scX + scY * s_superchunkSide];

				Stuff::Vector3D scMn, scMx;
				scMn.x = sc.worldMinX; scMn.y = sc.worldMinY; scMn.z = sc.worldMinZ;
				scMx.x = sc.worldMaxX; scMx.y = sc.worldMaxY; scMx.z = sc.worldMaxZ;

				sc.inFrustum = eye->quadAabbInFrustum(planes, scMn, scMx);
				if (!sc.inFrustum)
				{
					// Cull all constituent blocks without testing them.
					for (int dy = 0; dy < 4; ++dy)
					{
						for (int dx = 0; dx < 4; ++dx)
						{
							int bx = scX * 4 + dx, by = scY * 4 + dy;
							if (bx >= s_terrainChunkSide || by >= s_terrainChunkSide) continue;
							s_blockMeta[bx + by * s_terrainChunkSide].inFrustum = false;
						}
					}
					continue;
				}

				// --- Level 2: block cull + LOD selection within visible superchunk ---
				for (int dy = 0; dy < 4; ++dy)
				{
					for (int dx = 0; dx < 4; ++dx)
					{
						int bx = scX * 4 + dx, by = scY * 4 + dy;
						if (bx >= s_terrainChunkSide || by >= s_terrainChunkSide) continue;

						TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];

						// Recompute dirty AABB before testing.
						if (bm.dirtyAabb)
							recomputeBlockAabb(bm);

						// World-space AABB (same formula as recomputeSuperchunkAabb).
						Stuff::Vector3D bmMn, bmMx;
						bmMn.x =  float(bm.originX)                  * 128.0f - halfMap;
						bmMx.x =  float(bm.originX + bm.quadCountX)  * 128.0f - halfMap;
						bmMx.y =  halfMap - float(bm.originY)                  * 128.0f;
						bmMn.y =  halfMap - float(bm.originY + bm.quadCountY)  * 128.0f;
						bmMn.z = bm.minElev;
						bmMx.z = bm.maxElev;

						// CRIT-1: write inFrustum BEFORE any continue.
						bool passedCull = eye->quadAabbInFrustum(planes, bmMn, bmMx);
						bm.inFrustum = passedCull;

						// Phase 5: compute block center in MC2 world space and choose LOD.
						// Block center vertex = (originX + quadCountX*0.5, originY + quadCountY*0.5).
						// MC2 world: worldX = mapX * 128 - halfMap; worldY = halfMap - mapY * 128.
						float cX = float(bm.originX) * 128.0f + float(bm.quadCountX) * 0.5f * 128.0f - halfMap;
						float cY = halfMap - (float(bm.originY) * 128.0f + float(bm.quadCountY) * 0.5f * 128.0f);
						float dx2 = cX - eyeX, dy2 = cY - eyeY;
						float distSq = dx2 * dx2 + dy2 * dy2;
						bm.lodLevel = chooseLodLevel(distSq, bm.lodLevel);
					}
				}
			}
		}

		// --- Phase 5 Pass 2: neighbor LOD delta clamp (visible blocks only) ---
		// Iterates until stable so chains (e.g., LOD0 next to LOD5) propagate.
		{
			bool lodChanged = true;
			while (lodChanged)
			{
				lodChanged = false;
				for (int by = 0; by < s_terrainChunkSide; ++by)
				{
					for (int bx = 0; bx < s_terrainChunkSide; ++bx)
					{
						TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];
						if (!bm.inFrustum) continue;

						// Check 4 axis-aligned neighbors: E, W, S, N
						const int nbDx[4] = { 1, -1,  0,  0};
						const int nbDy[4] = { 0,  0,  1, -1};
						for (int n = 0; n < 4; ++n)
						{
							int nx = bx + nbDx[n], ny = by + nbDy[n];
							if (nx < 0 || nx >= s_terrainChunkSide) continue;
							if (ny < 0 || ny >= s_terrainChunkSide) continue;
							TerrainBlockMeta& nbm = s_blockMeta[nx + ny * s_terrainChunkSide];
							if (!nbm.inFrustum) continue;
							int delta = (int)bm.lodLevel - (int)nbm.lodLevel;
							if (delta > 1) {
								bm.lodLevel = nbm.lodLevel + 1;
								lodChanged = true;
							} else if (delta < -1) {
								nbm.lodLevel = bm.lodLevel + 1;
								lodChanged = true;
							}
						}
					}
				}
			}
		}

		// --- Phase 5 Pass 3: emit draw commands using lodLevel -> lodStep ---
		// --- Phase 6: also compute per-block skirt depth (parallel array) ---
		for (int by = 0; by < s_terrainChunkSide; ++by)
		{
			for (int bx = 0; bx < s_terrainChunkSide; ++bx)
			{
				const TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];
				if (!bm.inFrustum) continue;
				s_drawCmds[s_cmdCount].blockOriginX     = bm.originX;
				s_drawCmds[s_cmdCount].blockOriginY     = bm.originY;
				s_drawCmds[s_cmdCount].lodStep          = LOD_STEPS[bm.lodLevel];
				s_drawCmds[s_cmdCount].quadCountsPacked = (bm.quadCountX & 0xFF) | ((bm.quadCountY & 0xFF) << 8);
				// Phase 6: skirt depth = max(64, (maxElev - minElev) + 32).
				// bm.minElev/maxElev are populated by recomputeBlockAabb() before this loop.
				{
					float elevRange = bm.maxElev - bm.minElev;
					float depth     = elevRange + 32.0f;
					s_skirtDepths[s_cmdCount] = (depth > 64.0f) ? depth : 64.0f;
				}
				++s_cmdCount;
			}
		}

		// Phase 5 LOD telemetry — one log line per 180 frames.
		if (gCurrentFrame % 180 == 0 && s_cmdCount > 0)
		{
			int lodCounts[6] = {};
			for (int ci = 0; ci < s_cmdCount; ++ci)
			{
				const int step = s_drawCmds[ci].lodStep;
				int lvl = (step == 1)  ? 0
				        : (step == 2)  ? 1
				        : (step == 4)  ? 2
				        : (step == 5)  ? 3
				        : (step == 10) ? 4 : 5;
				lodCounts[lvl]++;
			}
			printf("[TerrainLOD v1] frame=%lu cmds=%d LOD0=%d LOD1=%d LOD2=%d LOD3=%d LOD4=%d LOD5=%d\n",
			       (unsigned long)gCurrentFrame, s_cmdCount,
			       lodCounts[0], lodCounts[1], lodCounts[2],
			       lodCounts[3], lodCounts[4], lodCounts[5]);
			fflush(stdout);
		}

		// s_drawCmds[] is submitted in Terrain::flushDrawCommands() from gamecam.cpp.
	}

	// Set terrain light direction for normal map shader
	if (eye)
	{
		ZoneScopedN("Terrain::update cameraParams");
		// Light direction now set from gamecam.cpp with proper MC2->GL swizzle
		// gos_SetTerrainLightDir(eye->lightDirection.x, eye->lightDirection.y, eye->lightDirection.z);

		// Pass camera world position in raw MC2 space (matching vs_WorldPos for TCS distance LOD)
		Stuff::Vector3D camOrigin = eye->getCameraOrigin();
		gos_SetTerrainCameraPos(camOrigin.x, camOrigin.y, camOrigin.z);

		// Pass camera look direction for POM (direction camera looks toward terrain)
		Stuff::Vector3D lookDir = eye->getLookVector();
		// Swizzle same as camera pos, then normalize
		float lx = -lookDir.x, ly = lookDir.z, lz = lookDir.y;
		float len = sqrtf(lx*lx + ly*ly + lz*lz);
		if (len > 0.001f) { lx /= len; ly /= len; lz /= len; }
		gos_SetTerrainViewDir(lx, ly, lz);
	}

	return TRUE;
}

//---------------------------------------------------------------------------
void Terrain::setOverlayTile (long block, long vertex, long offset)
{
	mapData->setOverlayTile(block,vertex,offset);
}	

//---------------------------------------------------------------------------
void Terrain::setOverlay( long tileR, long tileC, Overlays type, DWORD offset )
{
	mapData->setOverlay( tileR, tileC, type, offset );
	// Slice A — public cement-overlay mutation chokepoint. Any caller that
	// changes a tile's overlay (bridge destroy routes through here via
	// Terrain::mapData->setOverlay at bldng.cpp, plus any future caller)
	// invalidates the mission-static decal bake. Mirrors MarkMineDirty at
	// the setMine chokepoint; idempotent (dirty-flag debounced).
	gos_terrain_indirect::MarkDecalDirty();
}

//---------------------------------------------------------------------------
void Terrain::setTerrain( long tileR, long tileC, int terrainType )
{
	mapData->setTerrain( tileR, tileC, terrainType );
}

//---------------------------------------------------------------------------
int Terrain::getTerrain( long tileR, long tileC )
{
	return mapData->getTerrain( tileR, tileC );
}

//---------------------------------------------------------------------------
void Terrain::calcWater (float waterDepth, float waterShallowDepth, float waterAlphaDepth)
{
	mapData->calcWater(waterDepth, waterShallowDepth, waterAlphaDepth);
}	

//---------------------------------------------------------------------------
long Terrain::getOverlayTile (long block, long vertex)
{
	return (mapData->getOverlayTile(block,vertex));
}	

//---------------------------------------------------------------------------
void Terrain::getOverlay( long tileR, long tileC, enum Overlays& type, DWORD& Offset )
{
	mapData->getOverlay( tileR, tileC, type, Offset );
}

//---------------------------------------------------------------------------
void Terrain::setVertexHeight( int VertexIndex, float Val )
{
	if ( VertexIndex > -1 && VertexIndex < realVerticesMapSide * realVerticesMapSide )
	{
		mapData->setVertexHeight( VertexIndex, Val );

		// Terrain LOD chunk Phase 3 — dirty-patch upload after edit.
		if (s_blockMeta && getenv("MC2_TERRAIN_LOD_CHUNK"))
		{
			int vx = VertexIndex % (int)realVerticesMapSide;
			int vy = VertexIndex / (int)realVerticesMapSide;
			int bx = vx / (int)verticesBlockSide;
			int by = vy / (int)verticesBlockSide;
			if (bx >= s_terrainChunkSide) bx = s_terrainChunkSide - 1;
			if (by >= s_terrainChunkSide) by = s_terrainChunkSide - 1;
			int blockIdx = bx + by * s_terrainChunkSide;
			s_blockMeta[blockIdx].dirtyAabb = true;

			const TerrainBlockMeta& bm = s_blockMeta[blockIdx];
			int rows = bm.quadCountY + 1;
			int cols = bm.quadCountX + 1;
			std::vector<float> patch((size_t)rows * cols);
			const PostcompVertex* blks = mapData->getBlocks();
			for (int dy = 0; dy < rows; ++dy)
				for (int dx = 0; dx < cols; ++dx)
					patch[dx + dy * cols] = blks[
						(bm.originX + dx) + (bm.originY + dy) * (int)realVerticesMapSide].elevation;
			gos_TerrainLodChunk_UploadHeightPatch(
				patch.data(), bm.originX, bm.originY,
				bm.quadCountX, bm.quadCountY, (int)realVerticesMapSide);
		}
	}
}

//---------------------------------------------------------------------------
float Terrain::getVertexHeight( int VertexIndex )
{
	if ( VertexIndex > -1 && VertexIndex < realVerticesMapSide * realVerticesMapSide )
		return mapData->getVertexHeight(VertexIndex);

	return -1.f;
}

//---------------------------------------------------------------------------
void Terrain::render (void)
{
	//-----------------------------------
	// render the cloud layer
	//-----------------------------------
	// Draw resulting terrain quads. The drawPass zone wraps the WHOLE loop
	// (one zone per frame, not per-quad). Per-quad zones were stripped on
	// 2026-05-07 because zone overhead dominated; a single zone wrapping the
	// ~14-40K iteration loop attributes the 2.02 ms drawPass cost without
	// re-introducing the per-call overhead. PatchStream sub-zones (Flush,
	// MemoryBarrier, BucketSort, etc.) live inside this and break out the
	// ~290 us PatchStream slice; the residual ~1.7 ms is non-PatchStream
	// per-quad CPU work that stays attributed to drawPass at coarse level.
	DWORD fogColor = eye->fogColor;

	// Slice B4 Stage 1a — mask-dispatch build runs alongside the legacy
	// drawPass (does NOT replace it). IsMaskDispatchEnabled() gates on
	// MC2_TERRAIN_MASK_DISPATCH + dense recipe ready + Init() success.
	gos_terrain_mask_dispatch::BeginFrame();
	if (drawTerrainTiles && gos_terrain_mask_dispatch::IsMaskDispatchEnabled()) {
		ZoneScopedN("Terrain::render maskBuild");
		gos_terrain_mask_dispatch::BuildAndUploadMasksForFrame(quadList, numberQuads);
	}

	if (drawTerrainTiles)
	{
		ZoneScopedN("Terrain::render drawPass");
		// Terrain LOD chunk Phase 4 kill switch: when MC2_TERRAIN_LOD_CHUNK=1 the
		// chunk path owns terrain rendering; suppress the per-quad draw() loop
		// entirely to avoid double-draw. Mine pass is NOT suppressed (Phase 7).
		// drawPass-retirement Slice B (mirrors the proven minePass gate at
		// the sibling loop below). The per-quad draw() loop is retired only
		// when BOTH producers it bundles are GPU-covered:
		//   - SOLID base terrain  -> GPU indirect path (IsFrameSolidArmed)
		//   - cement/road decals  -> Slice-A static bake (IsFrameOverlayArmed)
		// DRAWALPHA detail is unconditionally dead (pixel-suppressed since
		// 521d83a; A2-confirmed via legacy_drawalpha_detail_quads counter).
		// MC2_TERRAIN_INDIRECT_OVERLAY is DEFAULT-ON since the 60f2ef8
		// Stage-6 flip (IsOverlayEnabled(): only literal "0" opts out).
		// So on the stock/default path BOTH IsFrameSolidArmed() and
		// IsFrameOverlayArmed() are true -> the conjunction is true ->
		// the per-quad draw() loop below is SKIPPED (the else branch is
		// the live default branch; the drawPass zone is ~empty). The
		// conjunction is still load-bearing, but for the opposite reason
		// the old comment claimed: it ensures the MC2_TERRAIN_INDIRECT_
		// OVERLAY=0 revert (overlay disabled, the code-proof fallback)
		// STILL runs draw() so decals fall back to the M2d per-quad emit
		// and do not vanish = the 9964d5a-regression guard. Gating on
		// IsFrameSolidArmed() alone would kill decals on the =0 revert.
		if (!getenv("MC2_TERRAIN_LOD_CHUNK"))
		{
			if (!(gos_terrain_indirect::IsFrameSolidArmed()
			      && gos_terrain_indirect::IsFrameOverlayArmed()))
			{
				TerrainQuadPtr currentQuad = quadList;
				for (long i = 0; i < numberQuads; i++)
				{
					// M2b loop-level pure-water hoist: skip the function call entirely for
					// quads with no base terrain, no overlay, and no detail handle. ~28K
					// quads/frame on water-heavy maps. Mirror of the in-draw() early-exit;
					// the in-function check is the fallback if useOverlayTexture /
					// useWaterInterestTexture globals get toggled at runtime.
					if (currentQuad->terrainHandle == 0
					    && currentQuad->overlayHandle == 0xffffffff
					    && currentQuad->terrainDetailHandle == 0xffffffff)
					{
						currentQuad++;
						continue;
					}
					currentQuad->draw();
					currentQuad++;
				}
			}
			else
			{
				// [SUBSYS] lifecycle line (env-gated, one-shot) per the
				// debug-instrumentation rule. Mirrors the mine retirement
				// trace. drawPass zone is now ~empty on armed frames -> the
				// Tracy total-frame delta is the substitutive proof.
				static bool s_drawPassRetiredLogged = false;
				if (!s_drawPassRetiredLogged
				    && gos_terrain_indirect::IsTraceEnabled())
				{
					s_drawPassRetiredLogged = true;
					printf("[TERRAIN_DRAWPASS v1] event=retired "
					       "reason=solid+overlay_armed "
					       "(SOLID->gpu_indirect, decals->static_bake, "
					       "detail->dead)\n");
					fflush(stdout);
				}
			}
		}
	}

	// Terrain LOD chunk Phase 7A — mine enqueue from visible tile grid.
	// When MC2_TERRAIN_LOD_CHUNK=1, the per-quad mine enqueue that was embedded
	// in setupTextures() is suppressed (see quad.cpp setupTextures s_lodChunkActive
	// guard). This block replaces it: enqueue runs over the full quadList once per
	// frame, identical population to the minePass drawMine loop below.
	// Must precede the minePass drawMine loop (draw reads mineResult set here).
	// Not gated on drawTerrainTiles — mine enqueue is a booking step that must
	// fire regardless of whether the LOD mesh draw is active this frame.
	if (getenv("MC2_TERRAIN_LOD_CHUNK")
	    && !gos_terrain_indirect::IsFrameMineArmed()
	    && quadList
	    && numberQuads > 0)
	{
		ZoneScopedN("Terrain::render lodChunkMineEnqueue");
		TerrainQuad::enqueueMinesFromGrid(quadList, numberQuads);
	}

	if (drawTerrainTiles)
	{
		ZoneScopedN("Terrain::render minePass");
		// PR2c Stage 2c — when armed, the indirect path owns mine drawing for
		// this frame (Render.TerrainMines zone in txmmgr.cpp). Skip the entire
		// per-quad drawMine loop here. This is the bulk of the ~1.83ms minePass
		// retirement (the loop fires drawMine on ALL ~14K visible quads, not
		// just the few mine-bearing ones, due to drawMine's early-return
		// pattern at quad.cpp:4242-4243).
		if (!gos_terrain_indirect::IsFrameMineArmed()) {
			TerrainQuadPtr currentQuad = quadList;
			for (long i = 0; i < numberQuads; i++)
			{
				currentQuad->drawMine();
				currentQuad++;
			}
		}
	}

	if (drawTerrainGrid || DrawDebugCells || drawLOSGrid)
	{
		ZoneScopedN("Terrain::render debugOverlays");
		TerrainQuadPtr currentQuad = quadList;
		for (long i = 0; i < numberQuads; i++)
		{
			if (drawTerrainGrid)
			{
				if (useFog) gos_SetRenderState(gos_State_Fog, 0);
				currentQuad->drawLine();
				if (useFog) gos_SetRenderState(gos_State_Fog, fogColor);
			}
			else if (DrawDebugCells)
			{
				if (useFog) gos_SetRenderState(gos_State_Fog, 0);
				currentQuad->drawDebugCellLine();
				if (useFog) gos_SetRenderState(gos_State_Fog, fogColor);
			}
			else if (drawLOSGrid)
			{
				if (useFog) gos_SetRenderState(gos_State_Fog, 0);
				currentQuad->drawLOSLine();
				if (useFog) gos_SetRenderState(gos_State_Fog, fogColor);
			}
			currentQuad++;
		}
	}
}

//---------------------------------------------------------------------------
// Terrain LOD chunk Phase 4 — flush pending draw commands to the GPU.
// Called from code/gamecam.cpp after the shadow pass and before
// mcTextureManager->renderLists(). No-op when MC2_TERRAIN_LOD_CHUNK is unset
// (s_blockMeta is nullptr, s_cmdCount is 0). No GL calls in mclib/.
void Terrain::flushDrawCommands (void)
{
	if (s_blockMeta && s_cmdCount > 0)
		gos_TerrainLodChunk_SubmitDrawCommands(s_drawCmds, s_skirtDepths, s_cmdCount);
}

//---------------------------------------------------------------------------
// Single-source predicate: all conditions that allow renderWater() to skip the
// legacy loop and quad.cpp::setupTextures() to skip the armed-frame (ii) writes.
// Definition lives here (terrain.cpp) because this is the only TU that sees
// WaterStream + gpu_driven. Declared in gos_terrain_indirect.h (already
// included by both terrain.cpp and quad.cpp; zero new includes in either).
bool gos_terrain_indirect::WaterFastPathOwnsArmedDraw()
{
	static const bool s_fastPath =
	    (getenv("MC2_RENDER_WATER_FASTPATH") != nullptr) ||
	    gpu_driven::IsWaterEnabled();
	// PERF-MISSION-INTRO-ARMED-RENDER-1: water fast path now arms independently
	// of terrain solid. Water resources (WaterStream recipes, terrainTextures2)
	// are built during primeMissionTerrainCache() — fully ready on frame 1.
	// Removing the IsFrameSolidArmed() dependency lets water render on intro pans
	// where solid may not yet be armed (e.g. first few frames before LUT resolves).
	// MVP fallback: gameos_graphics.cpp uses gos_GetTerrainMVPMat4() when
	// !IsFrameSolidArmed(), already handled (fix #3, water_fastpath_interim_fixes).
	// Kill-switch: MC2_MISSION_INTRO_LEGACY_RENDER=1 restores solid-arm dependency.
	static const bool s_introLegacy =
	    (getenv("MC2_MISSION_INTRO_LEGACY_RENDER") != nullptr);
	const bool g1 = s_fastPath;
	// g2: solid-arm dependency — removed by default, restored by kill-switch.
	const bool g2 = s_introLegacy ? gos_terrain_indirect::IsFrameSolidArmed() : true;
	const bool g3 = WaterStream::IsReady();
	const bool g4 = (WaterStream::GetRecipeCount() > 0);
	const bool g5 = (Terrain::terrainTextures2 != nullptr);
	// S2.15 gate diag: editor fails to arm FAST water path - print once
	// per (mission/state-change) what gate fails. Throttled by signature.
	static const bool s_gateDiag = (getenv("MC2_WATER_GATE_DIAG") != nullptr);
	if (s_gateDiag)
	{
		const bool solidArmed = gos_terrain_indirect::IsFrameSolidArmed();
		uint32_t sig = (g1?1:0) | (solidArmed?2:0) | (g3?4:0) | (g4?8:0) | (g5?16:0);
		static uint32_t s_lastSig = 0xffffffff;
		if (sig != s_lastSig)
		{
			printf("[WATER_GATE] fastPath=%d armed=%d streamReady=%d recipes=%u tex2=%d introLegacy=%d (sig=0x%x)\n",
			    g1?1:0, solidArmed?1:0, g3?1:0,
			    (unsigned)WaterStream::GetRecipeCount(),
			    g5?1:0, s_introLegacy?1:0, sig);
			fflush(stdout);
			s_lastSig = sig;
		}
	}
	return g1 && g2 && g3 && g4 && g5;
}

//---------------------------------------------------------------------------
void Terrain::renderWater (void)
{
	ZoneScopedN("Terrain::renderWater");

	// MC2_WATER_DEBUG=1: post-warmup population recon for the renderWater slice.
	// Reports per-frame how many quads are pure-skip (waterHandle == 0xffffffff,
	// out-of-frustum or non-water by map data) vs handle-valid (the upper bound
	// on the actually-emitting subset). Mirrors the MC2_THIN_DEBUG pattern in
	// quad.cpp: prints 5 frames after a 1200-frame warmup hold-off, then dormant.
	static const bool s_waterDebugOn = (getenv("MC2_WATER_DEBUG") != nullptr);
	static uint32_t s_total = 0;
	static uint32_t s_handleValid = 0;
	static uint32_t s_detailEligibleByHandle = 0;
	static uint32_t s_framesPrinted = 0;
	static uint32_t s_frameCounter = 0;
	static uint64_t s_qpcFreq = 0;
	static uint64_t s_qpcStart = 0;
	constexpr uint32_t kWaterWarmupHoldoffFrames = 1200;
	if (s_waterDebugOn && s_qpcFreq == 0)
		QueryPerformanceFrequency((LARGE_INTEGER*)&s_qpcFreq);
	if (s_waterDebugOn)
		QueryPerformanceCounter((LARGE_INTEGER*)&s_qpcStart);

	// Predicate is now single-sourced in gos_terrain_indirect::WaterFastPathOwnsArmedDraw().
	// renderWater() is once-per-frame, so this is the correct (non-hot) site
	// for the S6 armed-skip probe - it observes the EXACT predicate the
	// quad.cpp setupTextures (ii) gate uses, so armedSkip=1 here == "(ii)
	// legacy draw-side skipped this frame, GPU fast path owns it".
	const bool s6FastPathOwns = gos_terrain_indirect::WaterFastPathOwnsArmedDraw();
	{
		static const bool s_waterS6Trace = (getenv("MC2_WATER_S6_TRACE") != nullptr);
		if (s_waterS6Trace)
		{
			static int s_lastS6 = -1;
			int s6 = s6FastPathOwns ? 1 : 0;
			if (s6 != s_lastS6)
			{
				printf("[WATER_S6 v1] event=state armedSkip=%d (1=GPU fast path owns armed draw; legacy (ii) draw-side skipped this frame)\n", s6);
				fflush(stdout);
				s_lastS6 = s6;
			}
		}
	}
	if (s6FastPathOwns)
	{
		// Skip legacy loop entirely; renderWaterFastPath() does the work.
		return;
	}

	// [DEPTH_TRANSITION v1] reset the CPU-water REAL screen-z nearest-vertex
	// search once per CPU-water frame (env-gated; silent default). Reached
	// ONLY when the legacy loop runs (s6FastPathOwns early-returned above),
	// i.e. exactly the frames CPU water is the live producer. The stamp bump
	// lets the transition dump in gos_terrain_indirect.cpp detect a STALE
	// CPU sample on armed frames (CPU water and the GPU fast path are
	// mutually exclusive per frame). Pure writes, zero behavior change.
	{
		static const bool s_depthTransProbe =
		    (getenv("MC2_DEPTH_TRANSITION_PROBE") != nullptr);
		if (s_depthTransProbe)
		{
			extern float              g_cpuWaterProbeZ;
			extern double             g_cpuWaterProbeBestD2;
			extern bool               g_cpuWaterProbeAny;
			extern unsigned long long g_cpuWaterProbeStamp;
			(void)g_cpuWaterProbeZ;
			g_cpuWaterProbeAny = false;
			g_cpuWaterProbeBestD2 = 0.0;
			++g_cpuWaterProbeStamp;
		}
	}

	//-----------------------------------
	// Draw resulting terrain quads
	TerrainQuadPtr currentQuad = quadList;

	const bool collect = s_waterDebugOn && (s_framesPrinted < 5)
	                     && (s_frameCounter >= kWaterWarmupHoldoffFrames);
	uint32_t traceTotal = 0;
	uint32_t traceHandleValid = 0;
	uint32_t traceDetailEligible = 0;

	for (long i=0;i<numberQuads;i++)
	{
		++traceTotal;
		if (currentQuad->waterHandle != 0xffffffff)
		{
			++traceHandleValid;
			if (currentQuad->waterDetailHandle != 0xffffffff)
				++traceDetailEligible;
		}
		if (collect)
		{
			++s_total;
			if (currentQuad->waterHandle != 0xffffffff)
			{
				++s_handleValid;
				if (currentQuad->waterDetailHandle != 0xffffffff)
					++s_detailEligibleByHandle;
			}
		}

		if (drawTerrainTiles)
			currentQuad->drawWater();

		currentQuad++;
	}

	{
		static bool s_haveLast = false;
		static uint32_t s_lastHandleValid = 0;
		const bool disappeared = (s_haveLast && s_lastHandleValid > 0 && traceHandleValid == 0);
		const bool recovered = (s_haveLast && s_lastHandleValid == 0 && traceHandleValid > 0);
		if (disappeared || recovered || !s_haveLast) {
			fprintf(stderr,
			        "[WATER_LEGACY v1] event=population total=%u handle_valid=%u "
			        "detail_eligible=%u state=%s prev_handle_valid=%u\n",
			        traceTotal, traceHandleValid, traceDetailEligible,
			        disappeared ? "disappeared" : (recovered ? "recovered" : "initial"),
			        s_lastHandleValid);
			fflush(stderr);
		}
		s_haveLast = true;
		s_lastHandleValid = traceHandleValid;
	}

	if (s_waterDebugOn)
	{
		uint64_t qpcEnd = 0;
		QueryPerformanceCounter((LARGE_INTEGER*)&qpcEnd);
		const uint64_t elapsedTicks = qpcEnd - s_qpcStart;
		const double elapsedUs = (s_qpcFreq > 0)
		    ? (1000000.0 * (double)elapsedTicks / (double)s_qpcFreq)
		    : 0.0;
		if (s_framesPrinted < 5)
		{
			++s_frameCounter;
			if (s_frameCounter >= kWaterWarmupHoldoffFrames)
			{
				fprintf(stderr,
				        "[WATER_DEBUG v1] event=population frame=%u (post-warmup) "
				        "total=%u handle_valid=%u detail_eligible=%u "
				        "elapsed_us=%.1f\n",
				        s_framesPrinted, s_total, s_handleValid,
				        s_detailEligibleByHandle, elapsedUs);
				fflush(stderr);
				s_total = s_handleValid = s_detailEligibleByHandle = 0;
				++s_framesPrinted;
			}
		}
	}
}

//---------------------------------------------------------------------------
// Stage 2 of renderWater architectural slice. Called from gamecam.cpp AFTER
// mcTextureManager->renderLists() so terrain has been flushed and
// depth-written. This is required for the alpha-blend-on-top semantics:
// running fast-path INSIDE renderWater() (before renderLists) means terrain
// hasn't drawn yet and overwrites our water.
void Terrain::renderWaterFastPath (void)
{
	// MC2_RENDER_WATER_FASTPATH: legacy fast-path gate. MC2_GPU_DRIVEN_WATER
	// also enables the fast-path entry (the MDI branch inside the bridge).
	static const bool s_fastPath =
	    (getenv("MC2_RENDER_WATER_FASTPATH") != nullptr) ||
	    gpu_driven::IsWaterEnabled();
	if (!s_fastPath) return;
	if (!WaterStream::IsReady()) return;
	if (WaterStream::GetRecipeCount() == 0) return;
	if (!Terrain::terrainTextures2) return;

	ZoneScopedN("Terrain::renderWaterFastPath");

	// MC2_WATER_DEBUG=1 parallel timer (matches the legacy renderWater printer
	// style at terrain.cpp:1004-1077, post-warmup window of 5 frames). Lets
	// gate B Tracy/perf comparison run side-by-side with one env var.
	static const bool s_waterDebugOn = (getenv("MC2_WATER_DEBUG") != nullptr);
	static uint64_t s_qpcFreq2 = 0;
	static uint64_t s_qpcStart2 = 0;
	static uint32_t s_framesPrinted2 = 0;
	static uint32_t s_frameCounter2  = 0;
	constexpr uint32_t kFastWarmupHoldoffFrames = 1200;
	if (s_waterDebugOn && s_qpcFreq2 == 0)
		QueryPerformanceFrequency((LARGE_INTEGER*)&s_qpcFreq2);
	if (s_waterDebugOn)
		QueryPerformanceCounter((LARGE_INTEGER*)&s_qpcStart2);

	// getWater*Handle() returns mcTextureManager's textureIndex (master node
	// id), NOT the engine's gosTextureHandle. tex_resolve() chases the lazy
	// first-touch indirection — same pattern as M2d overlay at quad.cpp:2084.
	const DWORD waterTexIdx =
	    Terrain::terrainTextures2->getWaterTextureHandle();
	const DWORD waterDetailTexIdx =
	    Terrain::terrainTextures2->getWaterDetailHandle(sprayFrame);
	const DWORD waterTexHandle =
	    (waterTexIdx != 0xffffffff) ? tex_resolve(waterTexIdx) : 0u;
	const DWORD waterDetailTexHandle =
	    (waterDetailTexIdx != 0xffffffff) ? tex_resolve(waterDetailTexIdx) : 0xffffffffu;

	const float oneOverWaterTF =
	    Terrain::terrainTextures2->getWaterDetailTilingFactor()
	    / Terrain::worldUnitsMapSide;
	const float oneOverTF =
	    Terrain::terrainTextures2->getWaterTextureTilingFactor()
	    / Terrain::worldUnitsMapSide;

	const float cloudOffsetX =
	    cosf(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollX) * 0.1f;
	const float cloudOffsetY =
	    sinf(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollY) * 0.1f;
	const float sprayOffsetX = cloudScrollX * 10.0f;
	const float sprayOffsetY = cloudScrollY * 10.0f;

	{
		static bool s_dumped = false;
		if (!s_dumped && getenv("MC2_WATER_STREAM_DEBUG") != nullptr) {
			s_dumped = true;
			fprintf(stderr,
			        "[WATER_FAST v1] event=alpha_uniforms waterElevation=%.3f "
			        "alphaDepth=%.3f alphaEdgeByte=%u alphaMiddleByte=%u alphaDeepByte=%u\n",
			        (double)Terrain::waterElevation, (double)MapData::alphaDepth,
			        (unsigned)((Terrain::alphaEdge   >> 24) & 0xFFu),
			        (unsigned)((Terrain::alphaMiddle >> 24) & 0xFFu),
			        (unsigned)((Terrain::alphaDeep   >> 24) & 0xFFu));
			fflush(stderr);
		}
	}
	gos_terrain_bridge_renderWaterFast(
	    WaterStream::GetRecipeCount(),
	    (unsigned int)waterTexHandle,
	    (unsigned int)waterDetailTexHandle,
	    Terrain::waterElevation,
	    MapData::alphaDepth,
	    (unsigned int)((Terrain::alphaEdge   >> 24) & 0xFFu),
	    (unsigned int)((Terrain::alphaMiddle >> 24) & 0xFFu),
	    (unsigned int)((Terrain::alphaDeep   >> 24) & 0xFFu),
	    Terrain::mapTopLeft3d.x,
	    Terrain::mapTopLeft3d.y,
	    Terrain::frameCos,
	    Terrain::frameCosAlpha,
	    oneOverTF,
	    oneOverWaterTF,
	    cloudOffsetX,
	    cloudOffsetY,
	    sprayOffsetX,
	    sprayOffsetY,
	    MaxMinUV);

	// Stage 3 parity check (env-gated, silent on pass). Runs AFTER the bridge
	// so g_thinStaging is already populated by UploadAndBindThinRecords. The
	// check is CPU-only; it does not alter GPU state. See
	// `gos_terrain_water_stream.h` "Stage 3 parity check" doc-comment for
	// scope and field-level granularity.
	{
		ZoneScopedN("WaterFast.Parity");
		WaterStream::ParityFrameUniforms pu;
		pu.waterElevation             = Terrain::waterElevation;
		pu.alphaDepth                 = MapData::alphaDepth;
		pu.alphaEdgeDword             = Terrain::alphaEdge;
		pu.alphaMiddleDword           = Terrain::alphaMiddle;
		pu.alphaDeepDword             = Terrain::alphaDeep;
		pu.mapTopLeftX                = Terrain::mapTopLeft3d.x;
		pu.mapTopLeftY                = Terrain::mapTopLeft3d.y;
		pu.frameCos                   = Terrain::frameCos;
		pu.frameCosAlpha              = Terrain::frameCosAlpha;
		pu.oneOverTF                  = oneOverTF;
		pu.oneOverWaterTF             = oneOverWaterTF;
		pu.cloudOffsetX               = cloudOffsetX;
		pu.cloudOffsetY               = cloudOffsetY;
		pu.sprayOffsetX               = sprayOffsetX;
		pu.sprayOffsetY               = sprayOffsetY;
		pu.maxMinUV                   = MaxMinUV;
		pu.useWaterInterestTexture    = useWaterInterestTexture;
		pu.waterDetailHandleSentinel  = (uint32_t)waterDetailTexHandle;
		pu.terrainTextures2Present    = (Terrain::terrainTextures2 != nullptr);
		WaterStream::CheckParityFrame(pu);
	}

	if (s_waterDebugOn)
	{
		uint64_t qpcEnd = 0;
		QueryPerformanceCounter((LARGE_INTEGER*)&qpcEnd);
		const uint64_t elapsedTicks = qpcEnd - s_qpcStart2;
		const double elapsedUs = (s_qpcFreq2 > 0)
		    ? (1000000.0 * (double)elapsedTicks / (double)s_qpcFreq2) : 0.0;
		++s_frameCounter2;
		if (s_frameCounter2 >= kFastWarmupHoldoffFrames && s_framesPrinted2 < 5)
		{
			fprintf(stderr,
			        "[WATER_FAST v1] event=elapsed frame=%u (post-warmup) "
			        "recipeCount=%u elapsed_us=%.1f\n",
			        s_framesPrinted2,
			        (unsigned)WaterStream::GetRecipeCount(),
			        elapsedUs);
			fflush(stderr);
			++s_framesPrinted2;
		}
	}
}

float cosineEyeHalfFOV = 0.0f;
#define MAX_CAMERA_RADIUS		(250.0f)
#define CLIP_THRESHOLD_DISTANCE	(768.0f)

//a full triangle.
#define VERTEX_EXTENT_RADIUS	(384.0f)

extern bool InEditor;

// --- [SLIMSPLIT v1] -------------------------------------------------------
// RDTSC sub-decomposition of the "Terrain::geometry slimReduce" per-vertex
// loop. Distinct env gate MC2_SLIM_COST_SPLIT -- NOT the observer-effect-
// dominated MC2_TERRAIN_COST_SPLIT chrono scopes (memory/cost_split_
// instrumentation_is_observer_effect_dominated.md). __rdtsc() per-leaf
// bracket, no Tracy zone by design (a per-vertex hot loop busts the 100ns
// floor; same sanctioned exception as [LIGHT_COST_SPLIT v1] in tgl.cpp).
// Isolates the three independent costs the single slimReduce Tracy zone
// conflates, so the elimination campaign cuts in ROI order:
//   PROJ : eye->projectForTerrainAdmission (survives for cull + raster)
//   CULL : clipInfo + setObjBlock/VertexActive + solid-window append
//   RED  : leastZ/mostZ/leastW/mostW reduction (retired Phase 4 2026-05-19; bracket retained as dead-instrumentation envelope until SLIMSPLIT demote/delete)
// "front/other" (onScreenR sphere/cone math + raster px/pz write) =
// Tracy slimReduce total - (PROJ+CULL+RED); not separately bracketed to
// hold the per-vertex rdtsc-pair count at 3. Demote-not-delete after the
// attribution lands (debug_instrumentation_rule.md).
#include <intrin.h>
#include <stdlib.h>
#include <stdio.h>
namespace {
	bool               g_ssInit = false, g_ssOn = false;
	unsigned long long g_ssProjCyc = 0, g_ssCullCyc = 0, g_ssRedCyc = 0;
	unsigned long long g_ssProjCall = 0, g_ssVtx = 0, g_ssFrames = 0;
	bool SlimSplitOn()
	{
		if (!g_ssInit) { g_ssOn = (getenv("MC2_SLIM_COST_SPLIT") != nullptr); g_ssInit = true; }
		return g_ssOn;
	}
	void SlimSplitRollAndMaybeEmit()
	{
		if (!g_ssOn) return;
		++g_ssFrames;
		if (g_ssFrames % 600ULL != 0ULL) return;
		const double f = 600.0;
		fprintf(stderr,
			"[SLIMSPLIT v1] event=summary frames=600 "
			"vtx_per_frame=%.0f proj_cyc_per_frame=%.0f proj_calls_per_frame=%.0f "
			"cull_cyc_per_frame=%.0f red_cyc_per_frame=%.0f\n",
			(double)g_ssVtx / f, (double)g_ssProjCyc / f, (double)g_ssProjCall / f,
			(double)g_ssCullCyc / f, (double)g_ssRedCyc / f);
		g_ssProjCyc = g_ssCullCyc = g_ssRedCyc = 0;
		g_ssProjCall = g_ssVtx = 0;
	}
}  // namespace

//---------------------------------------------------------------------------
void Terrain::geometry (void)
{
	ZoneScopedN("Terrain::geometry");

	// Shape A (M0a) — per-frame texture-handle memoization. Initialized at
	// the EARLIEST terrain frame boundary because converted setup-time reads
	// in TerrainQuad::setupTextures, ensureTerrainFaceCacheEntryResident, and
	// terrtxm{,2}.h accessors fire during this function (mission-update phase),
	// before GameCamera::render. See
	// docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md.
	{
		static uint64_t s_texResolveFrameCounter = 0;
		beginFrameTexResolve(++s_texResolveFrameCounter);
	}

	//---------------------------------------------------------------------
	//-----------------------------------

	Stuff::Vector3D cameraPos;
	cameraPos.x = -eye->getCameraOrigin().x;
	cameraPos.y = eye->getCameraOrigin().z;
	cameraPos.z = eye->getCameraOrigin().y;

	float vClipConstant = eye->verticalSphereClipConstant;
	float hClipConstant = eye->horizontalSphereClipConstant;

	// S2.13: MC2_TERRAIN_CULL_WIDE=1 forces the angular sphere cull to
	// admit-all (constants set to 1e9). Investigation revealed editor
	// terrain coverage at ~25% of map vertices passing the angular cull
	// (885/3474 verts on mc2_01 default camera). Whether this matches
	// game-side behavior is unverified; this gate exists as an opt-in
	// workaround so visual A/B can confirm or refute the cull as root cause
	// of the "terrain clips to small center region in editor" symptom.
	// Default OFF: bit-identical legacy behavior. See handoff S2.13.
	if (getenv("MC2_TERRAIN_CULL_WIDE") != NULL) {
		vClipConstant = 1.0e9f;
		hClipConstant = 1.0e9f;
	}


	// VPL retirement: the MC2_VPL_CULL / MC2_VPL_REDUCE getenv reads are
	// KEPT solely to gate the one-shot event=retired lifecycle lines
	// emitted just before the slim reduce loop below (8c-part-2: the VPL
	// body those probes compared against is deleted, so any relocated
	// self-comparison would be tautological/false-alarm; demote-not-
	// silently-delete per the worktree Debug-instrumentation rule).
	static const bool s_vplCull = (getenv("MC2_VPL_CULL") != nullptr);
	static const bool s_vplReduce = (getenv("MC2_VPL_REDUCE") != nullptr);

	// F6 T1/T2: terrain admission Modern path. Replaces per-vertex
	// projectForTerrainAdmission with a world-space frustum-plane test:
	//   extractFrustumPlanes (one-shot per frame) + quadAabbInFrustum
	//   on a degenerate AABB (point, min==max). Default Legacy preserves
	//   the red-band fallback per spec §8.
	// F6 T2: mode flag moved to mc2_terrain_admission::isModern()
	// (terrain_admission_mode.h) shared with quad.cpp setupTextures sites.
	// Cache populated below; quad.cpp reads via eye->getCachedFrustumPlanes().
	// Env: MC2_TERRAIN_ADMISSION_MODERN=1 flips to Modern.
	// Reference: docs/observations/2026-05-22-terrain-admission-hotpath-recon.md
	const bool s_admissionModern = mc2_terrain_admission::isModern();
	static bool s_admissionModeLogged = false;
	if (!s_admissionModeLogged) {
		s_admissionModeLogged = true;
		fprintf(stderr, "[TERRAIN_ADMISSION v1] event=mode_select mode=%s\n",
			s_admissionModern ? "modern" : "legacy");
		fflush(stderr);
	}

	long i=0;

	// VPL retirement Step 6: self-contained slim per-vertex loop. Originally
	// re-homed the leastZ/mostZ/leastW/mostW/leastWY/mostWY reduction feeding
	// the inverse-projection consumer; that RED reduction was deleted in
	// Phase 4 (2026-05-19) once the consumer chain was retired in Phase 3
	// (6d61801). The per-vertex projectForTerrainAdmission call
	// SURVIVES because its return drives the clipInfo / objBlock cull cascade
	// AND the rv->px/py/pz/pw raster writes consumed by the legacy
	// TerrainQuad::draw() immediate path on un-armed frames.
	// VPL retirement Step 8c-part-2: the VertexProjectLoop body (fast +
	// legacy-twin) is DELETED here â the slim reduce loop below is the
	// proven sole producer of BOTH the cull cascade and the
	// leastZ/mostZ/... reduction (8c-part-1 static + camera-swept
	// superset_violations=0 + bit-identity proof). The legacy-reference
	// sources the MC2_VPL_CULL and MC2_VPL_REDUCE probes compared against
	// died with the body, so a relocated self-comparison would be a pure
	// tautology / false-alarm; both probes are RETIRED (demote-not-
	// silently-delete per the worktree Debug-instrumentation rule + plan
	// v3.3:467 + v3.5:530) to a one-shot lifecycle line, env-gated by the
	// surviving getenv so it only prints when someone had the probe on.
	// See docs/superpowers/reviews/2026-05-15-step8-vpl-body-deletion-
	// adversarial-review.md CRIT-1/Â§6 + the v3.5 plan amendment.
	if (s_vplCull)
	{
		static bool s_vplCullRetiredLogged = false;
		if (!s_vplCullRetiredLogged) {
			s_vplCullRetiredLogged = true;
			fprintf(stderr,
				"[VPL_CULL v1] event=retired reason=vpl_body_deleted_slim_is_sole_producer\n");
			fflush(stderr);
		}
	}
	if (s_vplReduce)
	{
		static bool s_vplReduceRetiredLogged = false;
		if (!s_vplReduceRetiredLogged) {
			s_vplReduceRetiredLogged = true;
			fprintf(stderr,
				"[VPL_REDUCE v1] event=retired reason=vpl_body_deleted_slim_is_sole_reduction_producer\n");
			fflush(stderr);
		}
	}
	// Approach A (lag-free): reset the per-frame camera-windowed solid
	// recipe-index window IMMEDIATELY before the slim loop that fills it.
	// The slim loop runs BEFORE gos_terrain_indirect::ComputeDispatch()
	// (called later in geometry()), so the window collected here is consumed
	// by THIS frame's dispatch — same-frame, no 1-frame lag.  The append
	// (inside the `if (rv->clipInfo)` block below) rides the existing slim
	// iteration; NO new per-frame walk is introduced.
	gos_terrain_indirect::BeginFrameSolidWindow();
	const bool s_solidNarrowOn = gos_terrain_indirect::SolidWindowEnabled();

	// F6 T1/T2: cache frustum planes once per frame (O(1)) into Camera member.
	// Both slimReduce below AND quad.cpp::setupTextures water-corner sites read
	// via eye->getCachedFrustumPlanes(). Single extraction shared across all sites.
	// MC2_BLOCK_FRUSTUM_FALLBACK also needs planes; cache unconditionally when
	// either feature is active so both share the single extraction.
	static const bool s_blockFrustumFallback =
		(getenv("MC2_BLOCK_FRUSTUM_FALLBACK") != nullptr);
	if (s_admissionModern || s_blockFrustumFallback) {
		eye->cacheFrustumPlanes();
	}

	{
		ZoneScopedN("Terrain::geometry slimReduce");
		const bool ssOn = SlimSplitOn();  // [SLIMSPLIT v1] latched, loop-local
		// CULL de-inline: hoist the two loop-invariant cull-cascade bounds
		// (mission-load constants; written only in init/destroy, never in
		// geometry() -- grep-verified) and inline the bounds-checked array
		// writes, restoring the legacy s_vpFast fast-path form (0c8e06b^
		// :1633-1645; the retirement's own :1520 comment: "inlined ... so the
		// compiler doesn't gamble on inlining setObjBlockActive"). The slim
		// re-home regressed to the slow-path setObj*Active member calls
		// (~40k non-inlined calls/frame == the [SLIMSPLIT v1] CULL bucket).
		// Byte-identical to Terrain::setObjBlockActive/setObjVertexActive
		// (terrain.cpp:2042/2056) by construction: same predicate, same store.
		const long ssNumObjB        = numObjBlocks;
		const long ssNumActiveVerts = realVerticesMapSide * realVerticesMapSide;
		VertexPtr rv = vertexList;
		for (long ri = 0; ri < numberVertices; ++ri, ++rv)
		{
			if (ssOn) ++g_ssVtx;
			bool onScreenR = false;

			if (eye->usePerspective)
			{
				onScreenR = true;

				Stuff::Vector3D vPosition;
				vPosition.x = rv->vx;
				vPosition.y = rv->vy;
				vPosition.z = rv->pVertex->elevation;

				Stuff::Vector3D objectCenter;
				objectCenter.Subtract(vPosition,cameraPos);
				Camera::cameraFrame.trans_to_frame(objectCenter);
				float distanceToEye = objectCenter.GetApproximateLength();

				Stuff::Vector3D clipVector = objectCenter;
				clipVector.z = 0.0f;
				float distanceToClip = clipVector.GetApproximateLength();
				float clip_distance = fabs(1.0f / objectCenter.y);

				if (distanceToClip > CLIP_THRESHOLD_DISTANCE)
				{
					float object_angle = fabs(objectCenter.z) * clip_distance;
					float extent_angle = VERTEX_EXTENT_RADIUS / distanceToEye;
					if (object_angle > (vClipConstant + extent_angle))
					{
						onScreenR = false;
					}
					else
					{
						object_angle = fabs(objectCenter.x) * clip_distance;
						if (object_angle > (hClipConstant + extent_angle))
						{
							onScreenR = false;
						}
					}
				}

				if (onScreenR)
				{
					//---------------------------------------
					// Vertex is at edge of world or beyond.
					Stuff::Vector3D vPos(rv->vx,rv->vy,rv->pVertex->elevation);
					bool isVisible = Terrain::IsGameSelectTerrainPosition(vPos) || drawTerrainGrid;
					if (!isVisible)
					{
						onScreenR = true;
					}
				}
			}
			else
			{
				onScreenR = true;
			}

			// VPL Step 8c-part-1 (CRIT-1, catastrophic axis): the
			// cull-cascade write below MUST be emitted on the
			// onScreenR/clipR decision and BEFORE the reduction-admission
			// `if (!clipR || !inViewR) continue;` gate. The legacy
			// `if (!onScreenR) continue;` early-out is therefore REMOVED:
			// the projection now runs for every vertex so the cull write
			// is reached unconditionally. clipR uses the IDENTICAL formula
			// to the legacy VPL `clipInfo` write (terrain.cpp `eye->
			// usePerspective && Environment.Renderer != 3 ? onScreenR :
			// inViewR`); since onScreenR == legacy onScreen byte-for-byte
			// and Environment.Renderer is only ever 0, clipR == legacy
			// clipInfo. Placing the cull write AFTER this gate (or gating
			// it on inViewR) makes the slim active-set a STRICT SUBSET of
			// the loose 768u/384u-dilated legacy {onScreen} cull contract
			// and edge/off-rect objects and mechs VANISH (cull_gates_are
			// _load_bearing.md; mechs iterate last = canary).
			Stuff::Vector3D vertex3D(rv->vx,rv->vy,rv->pVertex->elevation);
			Stuff::Vector4D sp(-10000.0f,-10000.0f,-10000.0f,-10000.0f);
			// Cost restoration (slimReduce was structurally heavier than the
			// VPL it replaced): the pre-8c production fast path projected
			// ONLY onScreen vertices (0c8e06b^ terrain.cpp:1589-1604 `if
			// (onScreen) { inView = projectForTerrainAdmission(...); ... }
			// else { sentinel }`). The slim loop made the projection
			// UNCONDITIONAL, which is the regression. Restore the gate: in
			// stock (usePerspective && Renderer!=3) clipR == onScreenR and
			// is computed WITHOUT the projection, the re-home writes the
			// sentinel (not sp) for !onScreenR, and the reduction continues
			// on !clipR -- so projecting !onScreenR verts is pure waste. In
			// the Renderer!=3 / ortho branch clipR == inViewR which DOES
			// need the projection for every vertex, so it must still run
			// there. The cull-cascade write below is TEXTUALLY UNCHANGED
			// and clipR is computed identically to before in both branches
			// -> the cull superset is bit-identical by construction (the
			// catastrophic-axis invariant; cull_gates_are_load_bearing.md).
			bool inViewR = false;
			const bool clipUsesOnScreen = (eye->usePerspective && Environment.Renderer != 3);
			if (onScreenR || !clipUsesOnScreen)
			{
				unsigned long long _ssT = ssOn ? __rdtsc() : 0ULL;  // [SLIMSPLIT v1] PROJ
				if (s_admissionModern) {
					// F6 T1: frustum-plane test replaces projectZ matrix-mul
					// + homogeneous divide. Degenerate AABB (single point:
					// mn==mx). near-plane test rejects behind-camera verts
					// via dot(rZ,swizzled_point) < 0 -- correct red-band
					// class behavior without clip.w sign reliance.
					// sp stays sentinel (-10000); px/py/pz/pw write below is
					// gated out under Modern (approach a -- see commit msg).
					Stuff::Vector3D vPos(rv->vx, rv->vy, rv->pVertex->elevation);
					inViewR = eye->quadAabbInFrustum(eye->getCachedFrustumPlanes(), vPos, vPos);
				} else {
					inViewR = eye->projectForTerrainAdmission(vertex3D,sp);
				}
				if (ssOn) { g_ssProjCyc += __rdtsc() - _ssT; ++g_ssProjCall; }
			}

			bool clipR = clipUsesOnScreen ? onScreenR : inViewR;

			// --- cull-cascade write (CRIT-1: BEFORE the reduction gate) ---
			// Replicates the legacy VPL semantic verbatim: clipInfo =
			// clipR (== legacy onScreen-derived clipInfo in stock); the
			// active-set write fires `if (clipInfo)` exactly as the legacy
			// `if (currentVertex->clipInfo)` site, NOT gated on inViewR.
			unsigned long long _ssC = ssOn ? __rdtsc() : 0ULL;  // [SLIMSPLIT v1] CULL
			rv->clipInfo = clipR;

			if (rv->clipInfo)				//ONLY set TRUE ones.  Otherwise we just reset the FLAG each vertex!
			{
				// De-inlined == Terrain::setObjBlockActive/setObjVertexActive
				// (terrain.cpp:2042/:2056) verbatim: same bounds predicate,
				// same store, active==true folded. Legacy s_vpFast form.
				const long blockNum = rv->getBlockNumber();
				if ((blockNum >= 0) && (blockNum < ssNumObjB))
					objBlockInfo[blockNum].active = true;
				const long vertNum = rv->vertexNum;
				if ((vertNum >= 0) && (vertNum < ssNumActiveVerts))
					objVertexActive[vertNum] = true;
				// Approach A (lag-free collect): rv->vertexNum is the same
				// map-stable vertexNum space as the recipe index / RecipeFor-
				// VertexNum.  Appending every cull-active vertexNum is a
				// correct SUPERSET — a non-corner/edge vn dispatches one
				// thread that edge-skips/no-ops; it cannot drop a visible
				// quad.  Gated on the recipe being live so window entries are
				// always resolvable; NO corner/edge filter (correctness over
				// thread-count optimization).
				if (s_solidNarrowOn) {
					const int32_t svn = (int32_t)rv->vertexNum;
					if (gos_terrain_indirect::RecipeForVertexNum(svn))
						gos_terrain_indirect::AppendSolidWindowCandidate(svn);
				}
			}
			if (ssOn) g_ssCullCyc += __rdtsc() - _ssC;  // [SLIMSPLIT v1] CULL end

			// VPL Step 8b re-home: Step 8b (12ad8dc) deleted the per-vertex
			// px/py/pz/pw writes on the premise the GPU-driven indirect path
			// made them dead. They are NOT dead for the LEGACY immediate
			// raster terrain path (quad.cpp TerrainQuad::draw() reads
			// vertices[c]->px/py/pz/pw to build gVertex[]); that path runs on
			// frames where the indirect fast path is UNARMED -- the mission
			// deployment / unit-select screen never calls ComputePreflight()
			// so IsFrameSolidArmed() is false there. tier1 smoke is always
			// armed in-mission so it never exercised this consumer. Re-home
			// (not re-derive) the EXACT pre-8b semantics: gate on onScreenR
			// (== legacy `onScreen` byte-for-byte, NOT clipR/clipInfo --
			// faithful in the Renderer!=3 branch too), source the accepted
			// write from the already-computed sp (same projectForTerrain-
			// Admission output the old VPL body baked in), and reproduce the
			// off-screen sentinel verbatim. Placed BEFORE the reduction gate
			// below so off-rect-but-onscreen quads the raster path still
			// draws are not skipped (cull_gates_are_load_bearing.md).
			// F6 T1 approach (a): Modern path leaves sp at sentinel
			// (-10000); writing sentinel to rv->px/py/pz/pw would corrupt
			// legacy raster coords. Skip write entirely under Modern. Safe
			// because drawPass default-ON (GPU-indirect path) only hits
			// legacy raster on un-armed frames (mission-deploy/unit-select),
			// and MC2_TERRAIN_ADMISSION_MODERN is opt-in. If un-armed frames
			// are active with Modern=1, rv->px/py retain prior-frame values.
			// Interaction: MC2_TERRAIN_ADMISSION_MODERN=1 + MC2_TERRAIN_INDIRECT=0
			// on un-armed frames = undefined raster coords (acceptable for
			// opt-in experimental env; user canary gates default flip).
			if (!s_admissionModern)
			{
				if (onScreenR)
				{
					rv->px = sp.x;
					rv->py = sp.y;
					rv->pz = sp.z;
					rv->pw = sp.w;
				}
				else
				{
					rv->px = rv->py = 10000.0f;
					rv->pz = -0.5f;
					rv->pw = 0.5f;
				}
			}

			// --- reduction-admission gate (decoupled from the cull write
			// above; the reduction may legitimately use the tighter set,
			// the cull MUST use the loose {onScreen} set per CRIT-1) ---
			if (!clipR || !inViewR)
				continue;

			unsigned long long _ssR = ssOn ? __rdtsc() : 0ULL;  // [SLIMSPLIT v1] RED
			// Phase 4 (2026-05-19): leastZ/mostZ/leastW/mostW/leastWY/mostWY
			// RED-reduction writes deleted. The sole consumer chain (the
			// eye-side inverse-projection setter + downstream picking/Z helpers)
			// was retired in Phase 3 (6d61801). Globals + per-frame reset retained pending Phase 5
			// (quad.cpp water-block still writes them; full removal lands then).
			if (ssOn) g_ssRedCyc += __rdtsc() - _ssR;  // [SLIMSPLIT v1] RED end
		}
		SlimSplitRollAndMaybeEmit();  // [SLIMSPLIT v1] once/frame (slimReduce is 1/frame)
	}

	// MC2_BLOCK_FRUSTUM_FALLBACK: post-slimReduce block-level frustum AABB pass.
	// Widens object-block admission for cameras where the per-vertex angular cull
	// incorrectly rejects in-frustum blocks (wolfman / low-angle view). Additive
	// only — never deactivates a block the legacy vertex pass already activated.
	// Sets both objBlockInfo[b].active AND objVertexActive[vertNum] so the
	// per-object gate in objmgr (objVertexActive[obj->getVertexNum()]) also passes.
	// CRIT-1 superset invariant preserved: new active set = old ∪ aabb_activated.
	if (s_blockFrustumFallback) {
		const float (*planes)[4] = eye->getCachedFrustumPlanes();
		const long ssNumActiveVerts = realVerticesMapSide * realVerticesMapSide;
		static constexpr float kBlockZMin = -200.0f;
		static constexpr float kBlockZMax = 2500.0f;
		for (long b = 0; b < numObjBlocks; ++b) {
			if (objBlockInfo[b].active) continue;
			const long bx = b % blocksMapSide;
			const long by = b / blocksMapSide;
			Stuff::Vector3D mn(
				float(bx * verticesBlockSide - halfVerticesMapSide) * worldUnitsPerVertex,
				float(halfVerticesMapSide - (by + 1) * verticesBlockSide) * worldUnitsPerVertex,
				kBlockZMin);
			Stuff::Vector3D mx(
				mn.x + worldUnitsBlockSide,
				mn.y + worldUnitsBlockSide,
				kBlockZMax);
			if (!eye->quadAabbInFrustum(planes, mn, mx)) continue;
			objBlockInfo[b].active = true;
			// Propagate to per-vertex active flags: objmgr checks
			// objVertexActive[obj->getVertexNum()] as a second per-object gate.
			// Assumes row-major vertex storage: vertexNum = row*realVMS + col.
			const long rowStart = by * verticesBlockSide;
			const long rowEnd   = rowStart + verticesBlockSide;
			const long colStart = bx * verticesBlockSide;
			const long colEnd   = colStart + verticesBlockSide;
			for (long row = rowStart; row < rowEnd && row < realVerticesMapSide; ++row) {
				for (long col = colStart; col < colEnd && col < realVerticesMapSide; ++col) {
					const long vn = row * realVerticesMapSide + col;
					if (vn < ssNumActiveVerts)
						objVertexActive[vn] = true;
				}
			}
		}
	}

	//-----------------------------------
	// setup terrain quad textures
	// Also sets up mine data.
	TerrainQuadPtr currentQuad = quadList;

	{
		ZoneScopedN("Terrain::geometry quadSetupTextures");
		// Stage 3: preflight arming — walks live quadList BEFORE the loop so
		// IsFrameSolidArmed() is stable for all setupTextures() calls.
		// On un-armed frames (recipe not ready, disabled, etc.) this returns
		// false with zero side-effects; setupTextures runs as normal.
		gos_terrain_indirect::ComputePreflight();
		// Phase 1: terrain lighting GPU compute — per-frame trio (design doc Q5).
		// BeginFrame advances ring slot; PackAndDispatch packs + dispatches;
		// CopyResultsToVertexPool (Stage 3): T1/T2/T3 non-blocking tryConsume
		// writes GPU lightRGB/fogRGB into vertices[i] BEFORE the setupTextures loop.
		gos_terrain_lighting::BeginFrame();
		gos_terrain_lighting::PackAndDispatch();
		gos_terrain_lighting::CopyResultsToVertexPool(quadList, numberQuads);
		// Phase C: SOLID compute dispatch. MUST be AFTER PackAndDispatch above
		// so Phase 1's post-dispatch barrier has published the lighting SSBO.
		gos_terrain_indirect::ComputeDispatch();
		// Water-fast-path narrow walk: reset the candidate vector once per
		// frame, then append every quad that passes UploadThin's eligibility
		// gate immediately after setupTextures() establishes waterHandle.
		// Predicate MUST match UploadThin's exactly — see
		// gos_terrain_water_stream.cpp:UploadAndBindThinRecords.
		WaterStream::BeginFrameNarrow();
		// WATER-GPU-FULL-RECIPE-CULL-1B: when authoritative full-recipe GPU cull owns
		// the water draw, the GPU culls the whole world-indexed recipe set directly and
		// the CPU narrow candidate walk feeds nothing — skip it (this is the ~0.16ms
		// per-frame walk this lane set out to retire). Proven byte-identical by 1A parity.
		const bool s_waterNarrowOn =
			WaterStream::NarrowEnabled() && !WaterStream::IsFullRecipeAuthoritative();
		// S6 coarse cost A/B instrument: ONE QPC pair around the WHOLE
		// per-frame setupTextures loop (NOT per-quad - the per-quad
		// std::chrono COST_SPLIT scopes are observer-effect-poisoned and
		// disqualified; capped FPS is also useless). Env-gated, prints a
		// min/mean/max summary every 600 frames (MC2_TGL_POOL_TRACE idiom).
		// Used to A/B armed ((ii) skipped) vs MC2_GPU_DRIVEN_WATER=0
		// ((ii) runs) - the only setupTextures delta between those is (ii),
		// so this isolates (ii)'s real per-frame CPU contribution.
		static const bool s_s6CostOn = (getenv("MC2_WATER_S6_COST") != nullptr);
		static uint64_t s_s6QpcFreq = 0;
		uint64_t s_s6QpcStart = 0;
		if (s_s6CostOn)
		{
			if (s_s6QpcFreq == 0)
				QueryPerformanceFrequency((LARGE_INTEGER*)&s_s6QpcFreq);
			QueryPerformanceCounter((LARGE_INTEGER*)&s_s6QpcStart);
		}
		// QUADSETUP-ARMED-SKIP-WALK-1: when the GPU terrain path fully owns the
		// frame, the per-quad setupTextures() call is dead work — its consumers
		// (draw()/drawWater(), the recipe + mine-enqueue blocks, clipInfo) are
		// all either skipped or self-gated to no-op when armed, and decals/mines
		// come from the static bakes (DrawDecalStatic/DrawMineStatic, default-ON
		// since 2026-05-17). The water producer is the narrow-candidate walk
		// below, which reads pVertex directly under the fast path and does NOT
		// need setupTextures. Picking is independent (Camera::inverseProject
		// forward-projects fresh from vx/vy/elevation — Task 0 verified: zero
		// wx/wy/wz/clipInfo dependency). The narrow walk MUST still run every
		// frame because quadList is a camera-relative sliding window (its slots
		// remap to different world tiles as the view pans), so the eligible-water
		// set is NOT cacheable across frames. Gate default-OFF; MC2_QUADSETUP_ARMED_SKIP
		// (unset/"0" = OFF kill-switch, any other value = ON). NarrowEnabled() is
		// in the predicate so we only skip when the pVertex water producer is live.
		// DEFAULT-ON since 2026-06-03 (clean Tracy mc2_01 1.01ms->372us, tier1 5/5,
		// water/mines/decals/picking user-verified): only literal "0" opts out
		// (bisection / revert escape hatch), any other value INCLUDING UNSET opts in.
		static const bool s_armedSkipOn = []() {
			const char* v = getenv("MC2_QUADSETUP_ARMED_SKIP");
			return !(v && v[0] == '0' && v[1] == '\0');
		}();
		const bool fullyArmed =
			gos_terrain_indirect::IsFrameSolidArmed() &&
			gos_terrain_indirect::IsFrameOverlayArmed() &&
			gos_terrain_indirect::IsFrameMineArmed() &&
			gos_terrain_indirect::WaterFastPathOwnsArmedDraw() &&
			(Terrain::terrainTextures2 != NULL) &&
			!drawTerrainGrid &&
			WaterStream::NarrowEnabled();
		const bool skipSetup = s_armedSkipOn && fullyArmed;
		long quadsSkipped = 0;
		long waterCandidates = 0;
		// Both per-quad bodies are no-ops in the default-ON steady state (Slice A
		// skips setupTextures() + 1B's GPU water cull owns selection so the narrow
		// walk is off): the loop would iterate ~40K quads doing nothing. Skip it
		// whole — retires the residual ~32µs bare-iteration cost. currentQuad is a
		// pure cursor with no post-loop reader, so it need not advance here.
		if (skipSetup && !s_waterNarrowOn)
		{
			quadsSkipped = numberQuads;
		}
		else
		for (i=0;i<numberQuads;i++)
		{
			if (skipSetup)
				++quadsSkipped;
			else
				currentQuad->setupTextures();
			if (s_waterNarrowOn) {
				const TerrainQuad& q = *currentQuad;
				if (q.vertices[0] && q.vertices[1] &&
				    q.vertices[2] && q.vertices[3] &&
				    q.vertices[0]->vertexNum >= 0 &&
				    q.vertices[1]->vertexNum >= 0 &&
				    q.vertices[2]->vertexNum >= 0 &&
				    q.vertices[3]->vertexNum >= 0) {
					bool append;
					if (gos_terrain_indirect::WaterFastPathOwnsArmedDraw()) {
						// Water fast path owns this frame (solid+water armed, or water-only intro).
						// draw-side (ii) is skipped in setupTextures() so waterHandle IS 0xffffffff;
						// use the vertex water-tile predicate to avoid stale-sentinel false negatives.
						// Fix A (staircase): also include submerged tiles that lack water&1.
						// UploadAndBindThinRecords mirrors this predicate exactly.
						const bool waterFlagged =
						    (q.vertices[0]->pVertex->water & 1) ||
						    (q.vertices[1]->pVertex->water & 1) ||
						    (q.vertices[2]->pVertex->water & 1) ||
						    (q.vertices[3]->pVertex->water & 1);
						const float we = Terrain::waterElevation;
						// Shore-extension: include tiles slightly ABOVE waterElevation.
						// VS positions them at terrain surface; FS fades via negative-WT smoothstep.
						const float shoreExt = MapData::alphaDepth * 0.5f > 0.0f
						                       ? MapData::alphaDepth * 0.5f : 15.0f;
						const bool submergedSand = !waterFlagged && (
						    q.vertices[0]->pVertex->elevation < we + shoreExt ||
						    q.vertices[1]->pVertex->elevation < we + shoreExt ||
						    q.vertices[2]->pVertex->elevation < we + shoreExt ||
						    q.vertices[3]->pVertex->elevation < we + shoreExt);
						append = waterFlagged || submergedSand;
					} else {
						// Legacy path: waterHandle is set by setupTextures for water&1 tiles.
						const bool waterHandleSet = (q.waterHandle != 0xffffffffu);
						const float we = Terrain::waterElevation;
						const float shoreExt = MapData::alphaDepth * 0.5f > 0.0f
						                       ? MapData::alphaDepth * 0.5f : 15.0f;
						const bool submergedSand = !waterHandleSet && (
						    q.vertices[0]->pVertex->elevation < we + shoreExt ||
						    q.vertices[1]->pVertex->elevation < we + shoreExt ||
						    q.vertices[2]->pVertex->elevation < we + shoreExt ||
						    q.vertices[3]->pVertex->elevation < we + shoreExt);
						append = waterHandleSet || submergedSand;
					}
					if (append) {
						WaterStream::AppendNarrowCandidate(currentQuad);
						++waterCandidates;
					}
				}
			}
			currentQuad++;
		}
		// QUADSETUP-ARMED-SKIP-WALK-1 bounded telemetry (every 600 frames, only
		// when the gate is enabled so production-with-feature-off stays silent).
		if (s_armedSkipOn) {
			static uint32_t s_qsSkipFrames = 0;
			if ((++s_qsSkipFrames % 600) == 0) {
				printf("[QUADSETUP_SKIP v1] fullyArmed=%d skip=%d quadsSkipped=%ld waterCandidates=%ld pickingCarveout=none\n",
				       fullyArmed ? 1 : 0, skipSetup ? 1 : 0,
				       quadsSkipped, waterCandidates);
				fflush(stdout);
			}
		}
		if (s_s6CostOn)
		{
			uint64_t s6End = 0;
			QueryPerformanceCounter((LARGE_INTEGER*)&s6End);
			double s6Ms = (double)(s6End - s_s6QpcStart) * 1000.0 / (double)s_s6QpcFreq;
			static uint32_t s_s6Frames = 0;
			static double   s_s6Sum = 0.0;
			static double   s_s6Min = 1e30;
			static double   s_s6Max = 0.0;
			s_s6Frames++;
			s_s6Sum += s6Ms;
			if (s6Ms < s_s6Min) s_s6Min = s6Ms;
			if (s6Ms > s_s6Max) s_s6Max = s6Ms;
			if ((s_s6Frames % 600) == 0)
			{
				printf("[WATER_S6COST v1] event=summary frames=%u quadSetupTextures_ms mean=%.4f min=%.4f max=%.4f (window of 600)\n",
				       s_s6Frames, s_s6Sum / 600.0, s_s6Min, s_s6Max);
				fflush(stdout);
				s_s6Sum = 0.0; s_s6Min = 1e30; s_s6Max = 0.0;
			}
		}
		// Stage 1 cost-split: roll per-frame nanosecond accumulators (no-op
		// when MC2_TERRAIN_COST_SPLIT unset). ParityFrameTick advances the
		// summary cadence; Stage 2 passes the actual quads-checked count.
		// Stage 2: terrain lighting parity check — AFTER the setupTextures loop
		// so CPU has written all lightRGB/fogRGB for this frame.
		// GetMappedOutputForParity() synchronously waits on current-frame fence
		// (parity mode only — production path skips this entirely).
		if (gos_terrain_lighting::IsParityCheckEnabled()) {
			const gos_terrain_lighting::GpuTerrainLightingOutput* mappedOut =
				gos_terrain_lighting::GetMappedOutputForParity();
			gos_terrain_lighting::Parity_CompareFrame(quadList, numberQuads, mappedOut);
		}
		gos_terrain_indirect::CostSplit_RollFrame();
		{
			int quadsChecked = 0;
			if (gos_terrain_indirect::IsParityCheckEnabled())
				quadsChecked = gos_terrain_indirect::ParityCompareRecipeFrame();
			gos_terrain_indirect::ParityFrameTick(quadsChecked);
		}
	}

}

//---------------------------------------------------------------------------
float Terrain::getTerrainElevation (const Stuff::Vector3D &position)
{
	float result = mapData->terrainElevation(position);
	return(result);
}

//---------------------------------------------------------------------------
float Terrain::getTerrainElevation( long tileR, long tileC )
{
	return mapData->terrainElevation( tileR, tileC );
}

//---------------------------------------------------------------------------
unsigned long Terrain::getTexture( long tileR, long tileC )
{
	return mapData->getTexture( tileR, tileC );
}

//---------------------------------------------------------------------------
float Terrain::getTerrainAngle (const Stuff::Vector3D &position, Stuff::Vector3D* normal)
{
	float result = mapData->terrainAngle(position, normal);
	return(result);
}

//---------------------------------------------------------------------------
float Terrain::getTerrainLight (const Stuff::Vector3D &position)
{
	float result = mapData->terrainLight(position);
	return(result);
}

//---------------------------------------------------------------------------
Stuff::Vector3D Terrain::getTerrainNormal (const Stuff::Vector3D &position)
{
	Stuff::Vector3D result = Terrain::mapData->terrainNormal(position);
	return(result);
}

//---------------------------------------------------------------------------
// Uses a simple value to mark radius.  It never changes now!!
// First value in range table!!
void Terrain::markSeen (const Stuff::Vector3D &looker, byte who, float specialUnitExpand)
{
	return;

	/*		Not needed anymore.  Real LOS now.
	//-----------------------------------------------------------
	// This function marks vertices has being seen by a given side.
	Stuff::Vector3D position = looker;
	position.x -= mapTopLeft3d.x;
	position.y = mapTopLeft3d.y - looker.y;
	
	Stuff::Vector2DOf<float> upperLeft;
	upperLeft.x = floor(position.x * oneOverWorldUnitsPerVertex);
	upperLeft.y = floor(position.y * oneOverWorldUnitsPerVertex);

	Stuff::Vector2DOf<long> meshOffset;
	meshOffset.x = float2long(upperLeft.x);
	meshOffset.y = float2long(upperLeft.y);

	unsigned long xCenter = meshOffset.x;
	unsigned long yCenter = meshOffset.y;

	//Figure out altitude above minimum terrain altitude and look up in table.
	float baseElevation = MapData::waterDepth;
	if (MapData::waterDepth < Terrain::userMin)
		baseElevation = Terrain::userMin;

	float altitude = position.z - baseElevation;
	float altitudeIntegerRange = (Terrain::userMax - baseElevation) * 0.00390625f;
	long altLevel = 0;
	if (altitudeIntegerRange > Stuff::SMALL)
		altLevel = altitude / altitudeIntegerRange;
	
	if (altLevel < 0)
		altLevel = 0;

	if (altLevel > 255)
		altLevel = 255;

	float radius = visualRangeTable[altLevel];
	
	radius += (radius * specialUnitExpand);

	if (radius <= 0.0f)
		return;

	//-----------------------------------------------------
	// Who is the shift value to create the mask
	BYTE wer = (1 << who);

	VisibleBits->setCircle(xCenter,yCenter,float2long(radius),wer);
	*/
}

//---------------------------------------------------------------------------
// Uses dist passed in as radius.
void Terrain::markRadiusSeen (const Stuff::Vector3D &looker, float dist, byte who)
{
	return;

	//Not needed.  Real LOS now!
	/*
	if (dist <= 0.0f)
		return;

	//-----------------------------------------------------------
	// This function marks vertices has being seen by
	// a given side.
	dist *= worldUnitsPerMeter;
	dist *= Terrain::oneOverWorldUnitsPerVertex;
	
	Stuff::Vector3D position = looker;
	position.x -= mapTopLeft3d.x;
	position.y = mapTopLeft3d.y - looker.y;
	
	Stuff::Vector2DOf<float> upperLeft;
	upperLeft.x = floor(position.x * oneOverWorldUnitsPerVertex);
	upperLeft.y = floor(position.y * oneOverWorldUnitsPerVertex);

	Stuff::Vector2DOf<long> meshOffset;
	meshOffset.x = floor(upperLeft.x);
	meshOffset.y = floor(upperLeft.y);

	unsigned long xCenter = meshOffset.x;
	unsigned long yCenter = meshOffset.y;

	//-----------------------------------------------------
	// Who is the shift value to create the mask
	BYTE wer = (1 << who);

	VisibleBits->setCircle(xCenter,yCenter,dist,wer);
	*/
}

//---------------------------------------------------------------------------
void Terrain::setObjBlockActive (long blockNum, bool active)
{
	if ((blockNum >= 0) && (blockNum < numObjBlocks))
		objBlockInfo[blockNum].active = active;	
}	

//---------------------------------------------------------------------------
void Terrain::clearObjBlocksActive (void)
{
	for (long i = 0; i < numObjBlocks; i++)
		setObjBlockActive(i, false);
}	

//---------------------------------------------------------------------------
void Terrain::setObjVertexActive (long vertexNum, bool active)
{
	if ( (vertexNum >= 0) && (vertexNum < (realVerticesMapSide * realVerticesMapSide)) )
		objVertexActive[vertexNum] = active;	
}	

//---------------------------------------------------------------------------
void Terrain::clearObjVerticesActive (void)
{
	memset(objVertexActive,0,sizeof(bool) * realVerticesMapSide * realVerticesMapSide);
}

//---------------------------------------------------------------------------
long Terrain::save( PacketFile* fileName, int whichPacket, bool quickSave )
{ 
	if (!quickSave)
	{
		recalcShadows = true;
		mapData->calcLight();
	}
	else
	{
		recalcShadows = false;
	}
		
	return mapData->save( fileName, whichPacket ); 
}


//-----------------------------------------------------
bool Terrain::save( FitIniFile* fitFile )
{
	// write out the water info
#ifdef _DEBUG
	long result = 
#endif
	fitFile->writeBlock( "Water" );
	gosASSERT( result > 0 );


	fitFile->writeIdFloat( "Elevation", mapData->waterDepth );
	fitFile->writeIdFloat( "Frequency", waterFreq );
	fitFile->writeIdFloat( "Ampliture", waterAmplitude );
	fitFile->writeIdULong( "AlphaShallow", alphaEdge );
	fitFile->writeIdULong( "AlphaMiddle", alphaMiddle );
	fitFile->writeIdULong( "AlphaDeep", alphaDeep );
	fitFile->writeIdFloat( "AlphaDepth", mapData->alphaDepth );
	fitFile->writeIdFloat( "ShallowDepth", mapData->shallowDepth );

	fitFile->writeBlock( "Terrain" );
	fitFile->writeIdLong( "UserMin", userMin );
	fitFile->writeIdLong( "UserMax", userMax );
	fitFile->writeIdFloat( "TerrainMinX", tileColToWorldCoord[0] );
	fitFile->writeIdFloat( "TerrainMinY", tileRowToWorldCoord[0] );
	fitFile->writeIdUChar( "Noise", fractalNoise);
	fitFile->writeIdUChar( "Threshold", fractalThreshold);

	if (terrainTextures2)
	{
		terrainTextures2->saveTilingFactors(fitFile);
	}
	return true;
}

bool Terrain::load( FitIniFile* fitFile )
{
	// write out the water info
	long result = fitFile->seekBlock( "Water" );
	gosASSERT( result == NO_ERR );

	result = fitFile->readIdFloat( "Elevation", mapData->waterDepth );
	gosASSERT( result == NO_ERR );
	waterElevation = mapData->waterDepth;
	result = fitFile->readIdFloat( "Frequency", waterFreq );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdFloat( "Ampliture", waterAmplitude );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdULong( "AlphaShallow", alphaEdge );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdULong( "AlphaMiddle", alphaMiddle );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdULong( "AlphaDeep", alphaDeep );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdFloat( "AlphaDepth", mapData->alphaDepth );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdFloat( "ShallowDepth", mapData->shallowDepth );
	gosASSERT( result == NO_ERR );

	fitFile->seekBlock( "Terrain" );
	fitFile->readIdLong( "UserMin", userMin );
	fitFile->readIdLong( "UserMax", userMax );

	fitFile->readIdUChar( "Noise", fractalNoise);
	fitFile->readIdUChar( "Threshold", fractalThreshold);

	return true;

}

//---------------------------------------------------------------------------
void Terrain::unselectAll()
{
	mapData->unselectAll();
}

//---------------------------------------------------------------------------
void Terrain::selectVerticesInRect( const Stuff::Vector4D& topLeft, const Stuff::Vector4D& bottomRight, bool bToggle )
{
	Stuff::Vector3D worldPos;
	Stuff::Vector4D screenPos;

	int xMin, xMax;
	int yMin, yMax;

	if ( topLeft.x < bottomRight.x )
	{
		xMin = topLeft.x;
		xMax = bottomRight.x;
	}
	else
	{
		xMin = bottomRight.x;
		xMax = topLeft.x;
	}

	if ( topLeft.y < bottomRight.y )
	{
		yMin = topLeft.y;
		yMax = bottomRight.y;
	}
	else
	{
		yMin = bottomRight.y;
		yMax = topLeft.y;
	}
	
	for ( int i = 0; i < realVerticesMapSide; ++i )
	{
		for ( int j = 0; j < realVerticesMapSide; ++j )
		{
			worldPos.y = tileRowToWorldCoord[j];
			worldPos.x = tileColToWorldCoord[i];
			worldPos.z = mapData->terrainElevation( j, i );

			// [PROJECTZ:SelectionPicking id=picking_terrain_rect_select]
			PROJECTZ_SITE("picking_terrain_rect_select", "SelectionPicking");
			eye->projectForSelectionPicking( worldPos, screenPos );

			if ( screenPos.x >= xMin && screenPos.x <= xMax &&
				 screenPos.y >= yMin && screenPos.y <= yMax )
			{
				mapData->selectVertex( j, i, true, bToggle );		
			}
		}
	}
}

//---------------------------------------------------------------------------
bool Terrain::hasSelection()
{
	return mapData->selection();
}

//---------------------------------------------------------------------------
bool Terrain::isVertexSelected( long tileR, long tileC )
{
	return mapData->isVertexSelected( tileR, tileC );
}

//---------------------------------------------------------------------------
bool Terrain::selectVertex( long tileR, long tileC, bool bSelect )
{
	//We never use the return value so just send back false.
	if ( (tileR <= -1) || (tileR >= realVerticesMapSide) )
		return false;

	if ( (tileC <= -1) || (tileC >= realVerticesMapSide) )
		return false;

	mapData->selectVertex( tileR, tileC, bSelect, 0 );
	return true;
}

//---------------------------------------------------------------------------
float Terrain::getHighestVertex( long& tileR, long& tileC )
{
	float highest = -9999999.; // an absurdly small number
	for ( int i = 0; i < realVerticesMapSide * realVerticesMapSide; ++i )
	{
		float tmp = getVertexHeight( i );
		if ( tmp > highest )
		{
			highest = tmp;
			tileR = i/realVerticesMapSide;
			tileC = i % realVerticesMapSide;
		}
	}

	return highest;
}

//---------------------------------------------------------------------------
float Terrain::getLowestVertex(  long& tileR, long& tileC )
{
	float lowest = 9999999.; // an absurdly big number
	for ( int i = 0; i < realVerticesMapSide * realVerticesMapSide; ++i )
	{
		float tmp = getVertexHeight( i );
		if ( tmp < lowest )
		{
			lowest = tmp;
			tileR = i/realVerticesMapSide;
			tileC = i % realVerticesMapSide;
		}
	}

	return lowest;
}

//---------------------------------------------------------------------------
void  Terrain::setUserSettings( long min, long max, int terrainType )
{
	userMin = min;
	userMax = max;
	baseTerrain = terrainType;
}

//---------------------------------------------------------------------------
void Terrain::getUserSettings( long& min, long& max, int& terrainType )
{
	min = userMin;
	max = userMax;
	terrainType = baseTerrain;
}

//---------------------------------------------------------------------------
void Terrain::recalcWater()
{
	mapData->recalcWater();
}

//---------------------------------------------------------------------------
void Terrain::reCalcLight(bool doShadows)
{
	recalcLight = true;
	recalcShadows = doShadows;
	
	//Do a new burnin for the colormap
	if (terrainTextures2)
	{
		if (colorMapName)
			terrainTextures2->recalcLight(colorMapName);
		else
			terrainTextures2->recalcLight(terrainName);
	}
}

//---------------------------------------------------------------------------
void Terrain::clearShadows()
{
	mapData->clearShadows();
}

//---------------------------------------------------------------------------

long Terrain::getWater (const Stuff::Vector3D& worldPos) {
	//-------------------------------------------------
	// Get elevation at this point and compare to deep
	// water altitude for this map.
	float elevation = getTerrainElevation(worldPos);
	
	if (elevation < (waterElevation - MapData::shallowDepth))
		return(2);
	if (elevation < waterElevation)
		return(1);
	return(0);
}

//---------------------------------------------------------------------------
