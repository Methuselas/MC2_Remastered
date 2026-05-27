//---------------------------------------------------------------------
//
//
// This class will manage the texture memory provided by GOS
// GOS gives me a maximum of 256 256x256 pixel texture pages.
// I want GOS to think I only use 256x256 textures.  This class
// will insure that GOS believes that completely and provided
// smaller texture surfaces out of the main surface if necessary
// as well as returning the necessary UVs to get to the other surface.
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef TXMMGR_H
#include"txmmgr.h"
#include "gos_crashbundle.h"
#endif
#include"tex_resolve_table.h"

#ifndef TGAINFO_H
#include"tgainfo.h"
#endif

#ifndef FILE_H
#include"file.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef LZ_H
#include"lz.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"

#include"terrain.h"   // VPL-#shadow Phase 1+2: Terrain::mapData for the full-map static-shadow build

#ifndef PATHS_H
#include"paths.h"
#endif

#include<gameos.hpp>
#include<mlr/mlr.hpp>
#include<gosfx/gosfxheaders.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <chrono>   // [LIGHTBRIDGE v1] coarse per-frame populate sizing
#include <utils/gl_utils.h>
#include "gos_postprocess.h"
#include "gos_profiler.h"
#include "../GameOS/gameos/gos_static_prop_batcher.h"
#include "../GameOS/gameos/gos_static_prop_registry.h"  // Stage 3.C: flush()
#include "../GameOS/gameos/gos_mech_batcher.h"
#include "../GameOS/gameos/gos_validate.h"  // drainGLErrors (Tier-1 instr §4)
#include "../GameOS/gameos/gos_terrain_patch_stream.h"
#include "../GameOS/gameos/gos_terrain_indirect.h"
#include "../GameOS/gameos/gos_terrain_bridge.h"   // [TERRAIN_SURFACE] PR-2 surface validation draw
#include "../GameOS/gameos/gos_terrain_mask_dispatch.h"  // B4 Stage 1b: mask-SOLID draw
#include "../GameOS/gameos/gpu_cull_compute.h"  // C1b: compute_dispatch() moved here from mission.cpp
#include "../GameOS/gameos/gpu_cull_substrate.h"

// NS3 boundary: effectStream belongs to the texture/effect subsystem, not
// to whatever game/tool main links it. Previously redefined in every main.
Stuff::MemoryStream *effectStream = NULL;

//---------------------------------------------------------------------------
// static globals
MC_TextureManager *mcTextureManager = NULL;
gos_VERTEXManager *MC_TextureManager::gvManager = NULL;
gos_RenderShapeManager<TG_RenderShape> *MC_TextureManager::rsManager = NULL;
MemoryPtr			MC_TextureManager::lzBuffer1 = NULL;
MemoryPtr			MC_TextureManager::lzBuffer2 = NULL;
int				MC_TextureManager::iBufferRefCount = 0;

bool MLRVertexLimitReached = false;
extern bool useFog;
extern DWORD BaseVertexColor;
extern uint32_t g_mc2FrameCounter;

// CP-1: file-scope so a per-mission hook can re-prime the static terrain shadow
// accumulation for the new mission.  Previously a function-local static inside
// renderLists(); promoted here so mc_ResetTerrainShadowPrimed() can clear it.
static bool s_terrainShadowPrimed = false;
void mc_ResetTerrainShadowPrimed() { s_terrainShadowPrimed = false; }

// MC2_TEX_LIFECYCLE_TRACE=1 — diagnostic for the static-prop black-billboard bug
// under MC2_STATIC_UPDATE_SKIP=1. Logs lifecycle event types under a single
// schema (also emitted by msl.cpp, gos_static_prop_batcher.cpp,
// gos_static_prop_registry.cpp):
//   event=evict           — per cacheOut at MC_TextureManager::update / flushCache
//   event=evict_skipped   — per pinRefCount > 0 block at the four eviction sites
//   event=update_summary  — per call to MC_TextureManager::update
//   event=recache_multi   — per call to TG_TypeMultiShape::SetTextureHandle (msl.cpp)
//   event=draw_black      — per static-prop draw with invalid handle (batcher)
//   event=pin / event=unpin / event=pin_summary — registry-side pin lifecycle
// Cross-reference logs by `nodeIdx` to identify nodes that are evicted but never
// re-cached. See docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
static const bool s_texLifecycleTrace =
    (getenv("MC2_TEX_LIFECYCLE_TRACE") != nullptr);

// T1.15 [SPOT_DIAG v1] pack-probe state (GatherLightsParameters).
// First-shape always-on (one stderr line on the first call after process start).
// Per-summary every 600 GatherLightsParameters calls when env=1. `calls=N`
// because GatherLightsParameters runs once per submitMultiShape (many per frame).
static const bool s_spotDiagPackEnabled = (getenv("MC2_SPOT_DIAG") != nullptr);
static bool          s_spotDiagPackFirstHit  = false;
static unsigned long s_spotDiagPackCalls     = 0;
static unsigned long s_spotDiagPackActiveSum = 0;
static unsigned long s_spotDiagPackInactSum  = 0;
static unsigned long s_spotDiagPackPointSum  = 0;
#define TEX_LC(fmt, ...) \
    do { if (s_texLifecycleTrace) { printf("[TEX_LIFECYCLE v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

// Shared per-process dedup for event=evict_skipped across the four eviction
// sites (one in MC_TextureManager::update, three in flushCache). Without
// dedup, ~thousands of pinned nodes × 60Hz eviction sweeps = unsustainable
// log volume. Key buckets per ~64-turn window so the same skip emits at
// most once per minute-ish.
static inline bool s_evictSkipShouldEmit(long nodeIdx, long curTurn) {
    if (!s_texLifecycleTrace) return false;
    static thread_local std::unordered_set<uint64_t> s_dedup;
    const uint64_t key = (uint64_t(curTurn >> 6) << 32) | uint64_t(uint32_t(nodeIdx));
    return s_dedup.insert(key).second;
}
#define EVICT_SKIPPED(nodeIdx, refcount, site)                                          \
    do {                                                                                \
        if (s_evictSkipShouldEmit((long)(nodeIdx), turn)) {                             \
            printf("[TEX_LIFECYCLE v1] event=evict_skipped reason=pinned "              \
                   "nodeIdx=%ld pinRefCount=%lu site=%s turn=%ld\n",                    \
                   (long)(nodeIdx), (unsigned long)(refcount), (site), (long)turn);     \
            fflush(stdout);                                                             \
        }                                                                               \
    } while (0)

// --- Shadow shape collection (file-scope global, no struct layout impact) ---
struct ShadowShapeEntry {
	HGOSBUFFER vb;
	HGOSBUFFER ib;
	HGOSVERTEXDECLARATION vdecl;
	float worldMatrix[16];
};
static const int MAX_SHADOW_SHAPES = 512;
static ShadowShapeEntry g_shadowShapes[MAX_SHADOW_SHAPES];
static int g_numShadowShapes = 0;

static bool isAllConcreteTerrainBatch(const gos_VERTEX* vertices, DWORD totalVertices)
{
	if (!vertices || totalVertices == 0)
		return false;

	for (DWORD vi = 0; vi < totalVertices; ++vi)
	{
		if ((vertices[vi].frgb & 0x000000ff) != 3)
			return false;
	}

	return true;
}

void addShadowShape(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, const float* worldEntries16) {
	if (g_numShadowShapes >= MAX_SHADOW_SHAPES) return;
	ShadowShapeEntry& ss = g_shadowShapes[g_numShadowShapes++];
	ss.vb = vb;
	ss.ib = ib;
	ss.vdecl = vdecl;
	memcpy(ss.worldMatrix, worldEntries16, 16 * sizeof(float));
}

void clearShadowShapes() {
	g_numShadowShapes = 0;
}

DWORD actualTextureSize = 0;
DWORD compressedTextureSize = 0;
static int64_t gTxmRealizedTotal = 0;

static const DWORD MC_TEXCACHE_FILE_LZ = 0xF0000000;
static const DWORD MC_TEXCACHE_FILE_RAW = 0xE0000000;
static const DWORD MC_TEXCACHE_MEM_RAW = 0xD0000000;
static const DWORD MC_TEXCACHE_SIZE_MASK = 0x0FFFFFFF;
static const long MC_TEXCACHE_RAW_THRESHOLD = 256 * 1024;

static bool tryReadTgaLogicalSize(File& textureFile, DWORD uvScale, DWORD& logicalWidth, DWORD& logicalHeight)
{
	logicalWidth = 0;
	logicalHeight = 0;

	if (textureFile.fileSize() < sizeof(TGAFileHeader))
		return false;

	TGAFileHeader header;
	if (textureFile.read((MemoryPtr)&header, sizeof(header)) != sizeof(header))
	{
		textureFile.seek(0);
		return false;
	}

	textureFile.seek(0);

	if (header.width <= 0 || header.height <= 0)
		return false;

	const DWORD scale = uvScale ? uvScale : 1;
	logicalWidth = static_cast<DWORD>(header.width) / scale;
	logicalHeight = static_cast<DWORD>(header.height) / scale;
	return logicalWidth && logicalHeight;
}

#define MAX_SENDDOWN		10002

//------------------------------------------------------
// Frees up gos_VERTEX manager memory
void MC_TextureManager::freeVertices(void)
{
	if (gvManager)
	{
		gvManager->destroy();
		delete gvManager;
		gvManager = NULL;
	}
}

void MC_TextureManager::freeShapes(void)
{
	if (rsManager)
	{
		rsManager->destroy();
		delete rsManager;
		rsManager = NULL;
	}
}
		
//------------------------------------------------------
// Creates gos_VERTEX Manager and allocates RAM.  Will not allocate if already done!
void MC_TextureManager::startVertices (long maxVertices)
{
	if (gvManager == NULL)
	{
		gvManager = new gos_VERTEXManager;
		gvManager->init(maxVertices);
		gvManager->reset();
	}
}

void MC_TextureManager::startShapes(uint32_t maxShapes)
{
	if (rsManager == NULL)
	{
		rsManager = new gos_RenderShapeManager<TG_RenderShape>;
		rsManager->init(maxShapes);
		rsManager->reset();
	}
}
	 
//----------------------------------------------------------------------
// Class MC_TextureManager
void MC_TextureManager::start (void)
{
	ZoneScopedN("MC_TextureManager::start");
	init();

	//------------------------------------------
	// Create nodes from systemHeap.
	long nodeRAM = MC_MAXTEXTURES * sizeof(MC_TextureNode);
	masterTextureNodes = (MC_TextureNode *)systemHeap->Malloc(nodeRAM);
	gosASSERT(masterTextureNodes != NULL);

	for (long i=0;i<MC_MAXTEXTURES;i++)
		masterTextureNodes[i].init();
		
	//-------------------------------------------
	// Create VertexNodes from systemHeap
	nodeRAM = MC_MAXTEXTURES * sizeof(MC_VertexArrayNode);
	masterVertexNodes = (MC_VertexArrayNode *)systemHeap->Malloc(nodeRAM);
	gosASSERT(masterVertexNodes != NULL);
	
	memset(masterVertexNodes,0,nodeRAM);

	nodeRAM = MC_MAXTEXTURES * sizeof(MC_HardwareVertexArrayNode);
	masterHardwareVertexNodes = (MC_HardwareVertexArrayNode *)systemHeap->Malloc(nodeRAM);
	gosASSERT(masterHardwareVertexNodes != NULL);
	
	memset(masterHardwareVertexNodes,0,nodeRAM);

	textureCacheHeap = new UserHeap;
	textureCacheHeap->init(TEXTURE_CACHE_SIZE,"TXMCache");
	textureCacheHeap->setMallocFatals(false);
	
	textureStringHeap = new UserHeap;
	textureStringHeap->init(512000,"TXMString");

	if (!textureManagerInstrumented)
	{
		StatisticFormat( "" );
		StatisticFormat( "MechCommander 2 Texture Manager" );
		StatisticFormat( "===============================" );
		StatisticFormat( "" );

		AddStatistic("Handles Used","Handles",gos_DWORD, &(currentUsedTextures), Stat_Total);

		AddStatistic("Cache Misses","",gos_DWORD, &(totalCacheMisses), Stat_Total);

		StatisticFormat( "" );
		StatisticFormat( "" );

		textureManagerInstrumented = true;
	}
	
	indexArray = (WORD *)systemHeap->Malloc(sizeof(WORD) * MC_MAXFACES);
	for (int i=0;i<MC_MAXFACES;i++)
		indexArray[i] = i;
		
	//Add an Empty Texture node for all untextured triangles to go down into.
	masterTextureNodes[0].gosTextureHandle = 0;
	masterTextureNodes[0].nodeName = NULL;
	masterTextureNodes[0].uniqueInstance = false;
	masterTextureNodes[0].neverFLUSH = 0x1;
	masterTextureNodes[0].numUsers = 0;
	masterTextureNodes[0].key = gos_Texture_Solid;
	masterTextureNodes[0].hints = 0; 
	masterTextureNodes[0].width = 0;
	masterTextureNodes[0].logicalWidth = 0;
	masterTextureNodes[0].logicalHeight = 0;
	masterTextureNodes[0].lastUsed = -1;
	masterTextureNodes[0].textureData = NULL;

    lightDataStructuresCapacity = 128;
    lightDataStructuresCount = 0;
    lightData_ = new TG_HWLightsData[lightDataStructuresCapacity];
	// [LIGHTSSBO v1] EAGER create+bind here (not lazy). The old UBO was
	// created in this constructor so SSBO/UBO binding always had a valid
	// buffer for EVERY consumer regardless of frame phase — notably the
	// GPU static-prop/mech batcher, which reads LightsData via explicit
	// layout(binding=20) and runs in a different phase than the txmmgr
	// per-frame upload site. Lazy-create broke that lifetime invariant
	// (batcher drew before first upload -> binding 20 empty -> black
	// props). Allocate-once/update-many is the correct lifetime model.
	gos_LightDataSsbo_Upload(
		lightData_,
		(size_t)lightDataStructuresCapacity * sizeof(TG_HWLightsData));

    sceneData_ = new TG_HWSceneData;
	sceneDataBuffer_ = gos_CreateBuffer(gosBUFFER_TYPE::UNIFORM, gosBUFFER_USAGE::STATIC_DRAW, sizeof(TG_HWSceneData), 1, NULL);
	gos_BindBufferBase(sceneDataBuffer_, SCENE_DATA_ATTACHMENT_SLOT);

	initTexResolveTable();
}

extern Stuff::MemoryStream *effectStream;
extern MidLevelRenderer::MLRClipper * theClipper;
//----------------------------------------------------------------------
void MC_TextureManager::destroy (void)
{
	if (masterTextureNodes)
	{
		//-----------------------------------------------------
		// Traverses list of texture nodes and frees each one.
		long usedCount = 0;
		for (long i=0;i<MC_MAXTEXTURES;i++)
			masterTextureNodes[i].destroy();		// Destroy for nodes whacks GOS Handle
		
		currentUsedTextures = usedCount;			//Can this have been the damned bug all along!?
	}
	
	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	delete MidLevelRenderer::MLRTexturePool::Instance;
	MidLevelRenderer::MLRTexturePool::Instance = NULL; 

	delete theClipper;
	theClipper = NULL;
	
	gos_PopCurrentHeap();

	//------------------------------------------------------
	// Shutdown the GOS FX and MLR.
	gos_PushCurrentHeap(gosFX::Heap);
	
	delete gosFX::EffectLibrary::Instance;
	gosFX::EffectLibrary::Instance = NULL;

	delete effectStream;
	effectStream = NULL;
	
	delete gosFX::LightManager::Instance;
	gosFX::LightManager::Instance = NULL;

	gos_PopCurrentHeap();

	//------------------------------------------
	// free SystemHeap Memory
	systemHeap->Free(masterTextureNodes);
	masterTextureNodes = NULL;
	
	systemHeap->Free(masterVertexNodes);
	masterVertexNodes = NULL;
	
	delete textureCacheHeap;
	textureCacheHeap = NULL;

	delete textureStringHeap;
	textureStringHeap = NULL;

	gos_LightDataSsbo_Destroy();  // [LIGHTSSBO v1]

    delete[] lightData_;
    lightData_ = nullptr;

	if(sceneDataBuffer_)
		gos_DestroyBuffer(sceneDataBuffer_);
	sceneDataBuffer_ = nullptr;

    delete sceneData_;
    sceneData_ = nullptr;
}

//----------------------------------------------------------------------
MC_TextureManager::~MC_TextureManager (void)
{
	MC_TextureManager::iBufferRefCount--;
	if (0 == MC_TextureManager::iBufferRefCount)
	{
		if (lzBuffer1)
		{
			gosASSERT(lzBuffer2 != NULL);
			if (textureCacheHeap)
			{
				textureCacheHeap->Free(lzBuffer1);
				textureCacheHeap->Free(lzBuffer2);
			}
			lzBuffer1 = NULL;
			lzBuffer2 = NULL;
		}
	}

	destroy();
}

//----------------------------------------------------------------------
void MC_TextureManager::flush (bool justTextures)
{
	if (masterTextureNodes)
	{
		{
			char _cbbuf[128];
			snprintf(_cbbuf, sizeof(_cbbuf),
				"[TXM v1] event=mission_unload peak_textures=%ld/%d",
				peakUsedTextures, MAX_MC2_GOS_TEXTURES);
			puts(_cbbuf);
			crashbundle_append(_cbbuf);
			fflush(stdout);
		}
		peakUsedTextures = 0;

		//-----------------------------------------------------
		// Traverses list of texture nodes and frees each one.
		long usedCount = 0;
		for (long i=0;i<MC_MAXTEXTURES;i++)
		{
			if (!masterTextureNodes[i].neverFLUSH)
				masterTextureNodes[i].destroy();		// Destroy for nodes whacks GOS Handle
		}
		
		currentUsedTextures = usedCount;				//Can this have been the damned bug all along!?
	}
	
	//If we just wanted to free up RAM, just return and let the MUNGA stuff go later.
	if (justTextures)
		return;

	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	delete MidLevelRenderer::MLRTexturePool::Instance;
	MidLevelRenderer::MLRTexturePool::Instance = NULL; 

	delete theClipper;
	theClipper = NULL;
	
	gos_PopCurrentHeap();

	//------------------------------------------------------
	// Shutdown the GOS FX and MLR.
	gos_PushCurrentHeap(gosFX::Heap);
	
	delete gosFX::EffectLibrary::Instance;
	gosFX::EffectLibrary::Instance = NULL;

	delete effectStream;
	effectStream = NULL;
	
	delete gosFX::LightManager::Instance;
	gosFX::LightManager::Instance = NULL;

	gos_PopCurrentHeap();

	//------------------------------------------------------
	//Restart MLR and the GOSFx
	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	MidLevelRenderer::TGAFilePool *pool = new MidLevelRenderer::TGAFilePool("data" PATH_SEPARATOR "tgl" PATH_SEPARATOR "128" PATH_SEPARATOR);
	MidLevelRenderer::MLRTexturePool::Instance = new MidLevelRenderer::MLRTexturePool(pool);

	MidLevelRenderer::MLRSortByOrder *cameraSorter = new MidLevelRenderer::MLRSortByOrder(MidLevelRenderer::MLRTexturePool::Instance);
	theClipper = new MidLevelRenderer::MLRClipper(0, cameraSorter);
	
	gos_PopCurrentHeap();

	//------------------------------------------------------
	// ReStart the GOS FX.
	gos_PushCurrentHeap(gosFX::Heap);
	
	gosFX::EffectLibrary::Instance = new gosFX::EffectLibrary();
	Check_Object(gosFX::EffectLibrary::Instance);

	FullPathFileName effectsName;
	effectsName.init(effectsPath,"mc2.fx","");

	File effectFile;
	long result = effectFile.open(effectsName);
	if (result != NO_ERR)
		STOP(("Could not find MC2.fx"));
		
	long effectsSize = effectFile.fileSize();
	MemoryPtr effectsData = (MemoryPtr)systemHeap->Malloc(effectsSize);
	effectFile.read(effectsData,effectsSize);
	effectFile.close();
	
	effectStream = new Stuff::MemoryStream(effectsData,effectsSize);
	gosFX::EffectLibrary::Instance->Load(effectStream);
	
	gosFX::LightManager::Instance = new gosFX::LightManager();

	gos_PopCurrentHeap();

	systemHeap->Free(effectsData);
}

//----------------------------------------------------------------------
void MC_TextureManager::removeTextureNode (DWORD textureNode)
{
	if (textureNode != 0xffffffff)
	{
		//-----------------------------------------------------------
		masterTextureNodes[textureNode].destroy();
		if (masterTextureNodes[textureNode].textureData)
		{
			textureCacheHeap->Free(masterTextureNodes[textureNode].textureData);
			masterTextureNodes[textureNode].textureData = NULL;

			if (masterTextureNodes[textureNode].nodeName)
			{
				textureStringHeap->Free(masterTextureNodes[textureNode].nodeName);
				masterTextureNodes[textureNode].nodeName = NULL;
			}
		}
	}
}

//----------------------------------------------------------------------
void MC_TextureManager::removeTexture (DWORD gosHandle)
{
	//-----------------------------------------------------------
    long i = 0;
	for (;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].gosTextureHandle == gosHandle)
		{
			masterTextureNodes[i].numUsers--;
			break;			
		}
	}
	
	if (i < MC_MAXTEXTURES && masterTextureNodes[i].numUsers == 0)
	{
		masterTextureNodes[i].destroy();
		if (masterTextureNodes[i].textureData)
		{
			textureCacheHeap->Free(masterTextureNodes[i].textureData);
			masterTextureNodes[i].textureData = NULL;

			if (masterTextureNodes[i].nodeName)
			{
				textureStringHeap->Free(masterTextureNodes[i].nodeName);
				masterTextureNodes[i].nodeName = NULL;
			}
		}
	}
}

#define cache_Threshold		150
//----------------------------------------------------------------------
bool MC_TextureManager::flushCache (void)
{
	bool cacheNotFull = false;
	totalCacheMisses++;
	currentUsedTextures = 0;
	int poolPinned = 0;
	int poolUnique = 0;
	int poolFlushableIdle = 0;

	//Count ACTUAL number of textures being used.
	// ALSO can't count on turn being right.  Logistics does not update unless simple Camera is up!!
	for (long i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff))
		{
			currentUsedTextures++;
			if (currentUsedTextures > peakUsedTextures) peakUsedTextures = currentUsedTextures;
			const bool pinned = (masterTextureNodes[i].neverFLUSH & 1) != 0;
			const bool unique = masterTextureNodes[i].uniqueInstance != 0;
			const bool refPinned = masterTextureNodes[i].pinRefCount > 0;
			if (pinned || refPinned) poolPinned++;
			if (unique) poolUnique++;
			if (!pinned && !refPinned && !unique && masterTextureNodes[i].lastUsed <= (turn - cache_Threshold))
				poolFlushableIdle++;
		}
	}

	TracyPlot("Txm pool used", int64_t(currentUsedTextures));
	TracyPlot("Txm pool pinned", int64_t(poolPinned));
	TracyPlot("Txm pool unique", int64_t(poolUnique));
	TracyPlot("Txm pool flushable idle", int64_t(poolFlushableIdle));

	//If we are now below the magic number, return that the cache is NOT full.
	if (currentUsedTextures < MAX_MC2_GOS_TEXTURES)
		return true;

	for (int i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff) &&
			(!masterTextureNodes[i].uniqueInstance))
		{
			if (masterTextureNodes[i].lastUsed <= (turn-cache_Threshold))
			{
				if (masterTextureNodes[i].pinRefCount > 0) {
					EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "flushCache_cacheThreshold");
					continue;
				}
				//----------------------------------------------------------------
				// Cache this badboy out.  Textures don't change.  Just Destroy!
				if (masterTextureNodes[i].gosTextureHandle)
					gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle);

				masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;

				currentUsedTextures--;
				cacheNotFull = true;
				return cacheNotFull;
			}
		}
	}
	
	for (int i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff) &&
			(masterTextureNodes[i].gosTextureHandle) &&
			(!masterTextureNodes[i].uniqueInstance))
		{
			if (masterTextureNodes[i].lastUsed <= (turn-30))
			{
				if (masterTextureNodes[i].pinRefCount > 0) {
					EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "flushCache_turn30");
					continue;
				}
				//----------------------------------------------------------------
				// Cache this badboy out.  Textures don't change.  Just Destroy!
				if (masterTextureNodes[i].gosTextureHandle)
					gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle);

				masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;

				currentUsedTextures--;
				cacheNotFull = true;
				return cacheNotFull;
			}
		}
	}
	
	for (int i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff) &&
			(!masterTextureNodes[i].uniqueInstance))
		{
			if (masterTextureNodes[i].lastUsed <= (turn-1))
			{
				if (masterTextureNodes[i].pinRefCount > 0) {
					EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "flushCache_turn1");
					continue;
				}
				//----------------------------------------------------------------
				// Cache this badboy out.  Textures don't change.  Just Destroy!
				if (masterTextureNodes[i].gosTextureHandle)
					gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle);

				masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;

				currentUsedTextures--;
				cacheNotFull = true;
				return cacheNotFull;
			}
		}
	}
	
  	//gosASSERT(cacheNotFull);
	return cacheNotFull;
}

void MC_TextureManager::addRenderShape(DWORD nodeId, TG_RenderShape* render_shape, DWORD flags)
{
	//This function adds the actual vertex data to the texture Node.
	if (nodeId < MC_MAXTEXTURES)
	{
		if (masterTextureNodes[nodeId].hardwareVertexData &&
			masterTextureNodes[nodeId].hardwareVertexData->flags == flags)
		{
			TG_RenderShape* shapes = masterTextureNodes[nodeId].hardwareVertexData->currentShape;
			if (!shapes && !masterTextureNodes[nodeId].hardwareVertexData->shapes)
			{
				masterTextureNodes[nodeId].hardwareVertexData->currentShape =
					shapes =
					masterTextureNodes[nodeId].hardwareVertexData->shapes =
					rsManager->getBlock(masterTextureNodes[nodeId].hardwareVertexData->numShapes);
			}

			if (shapes < (masterTextureNodes[nodeId].hardwareVertexData->shapes + masterTextureNodes[nodeId].hardwareVertexData->numShapes))
			{
				*shapes = *render_shape;
				shapes++;
			}

			masterTextureNodes[nodeId].hardwareVertexData->currentShape = shapes;
		}
		else if (masterTextureNodes[nodeId].hardwareVertexData2 &&
			masterTextureNodes[nodeId].hardwareVertexData2->flags == flags)
		{
			TG_RenderShape* shapes = masterTextureNodes[nodeId].hardwareVertexData2->currentShape;

			//sebi: looks like assert may happen if more vertices added than was calculated on stage when addTriange was called. As one can see in (*) first time we go here, we allocate enough memory for all potential vertices, but if it is not enough this assert will trigger
#if defined( _DEBUG) || defined(_ARMOR)
			TG_RenderShape* oldShapes = shapes;
			TG_RenderShape* oldStart = (masterTextureNodes[nodeId].hardwareVertexData2->shapes + masterTextureNodes[nodeId].hardwareVertexData2->numShapes);
#endif
			gosASSERT(oldShapes < oldStart);

			// (*)
			if (!shapes && !masterTextureNodes[nodeId].hardwareVertexData2->shapes)
			{
				masterTextureNodes[nodeId].hardwareVertexData2->currentShape =
					shapes =
					masterTextureNodes[nodeId].hardwareVertexData2->shapes =
					rsManager->getBlock(masterTextureNodes[nodeId].hardwareVertexData2->numShapes);
			}

			if (shapes < (masterTextureNodes[nodeId].hardwareVertexData2->shapes + masterTextureNodes[nodeId].hardwareVertexData2->numShapes))
			{
				*shapes = *render_shape;
				shapes++;
			}

			masterTextureNodes[nodeId].hardwareVertexData2->currentShape = shapes;
		}
		else if (masterTextureNodes[nodeId].hardwareVertexData3 &&
			masterTextureNodes[nodeId].hardwareVertexData3->flags == flags)
		{
			TG_RenderShape * shapes = masterTextureNodes[nodeId].hardwareVertexData3->currentShape;

#if defined(_DEBUG) || defined(_ARMOR)
			TG_RenderShape * oldShapes = shapes;
			TG_RenderShape * oldStart = (masterTextureNodes[nodeId].hardwareVertexData3->shapes + masterTextureNodes[nodeId].hardwareVertexData3->numShapes);
#endif
			gosASSERT(oldShapes < oldStart);

			if (!shapes && !masterTextureNodes[nodeId].hardwareVertexData3->shapes)
			{
				masterTextureNodes[nodeId].hardwareVertexData3->currentShape =
					shapes =
					masterTextureNodes[nodeId].hardwareVertexData3->shapes =
					rsManager->getBlock(masterTextureNodes[nodeId].hardwareVertexData3->numShapes);
			}

			if (shapes < (masterTextureNodes[nodeId].hardwareVertexData3->shapes + masterTextureNodes[nodeId].hardwareVertexData3->numShapes))
			{
				*shapes = *render_shape;
				shapes++;
			}

			masterTextureNodes[nodeId].hardwareVertexData3->currentShape = shapes;
		}
		else	//If we got here, something is really wrong
		{
#ifdef _DEBUG
			SPEW(("GRAPHICS", "Flags do not match either set of render shapes Data\n"));
#endif
		}
	}
	else
	{
		if (hardwareVertexData && hardwareVertexData->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData->currentShape;
			if (!shapes && !hardwareVertexData->shapes)
			{
				hardwareVertexData->currentShape =
					shapes =
					hardwareVertexData->shapes =
					rsManager->getBlock(hardwareVertexData->numShapes);
			}

			if (shapes <= (hardwareVertexData->shapes + hardwareVertexData->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData->currentShape = shapes;
		}
		else if (hardwareVertexData2 && hardwareVertexData2->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData2->currentShape;
			if (!shapes && !hardwareVertexData2->shapes)
			{
				hardwareVertexData2->currentShape =
					shapes =
					hardwareVertexData2->shapes =
					rsManager->getBlock(hardwareVertexData2->numShapes);
			}

			if (shapes <= (hardwareVertexData2->shapes + hardwareVertexData2->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData2->currentShape = shapes;
		}
		else if (hardwareVertexData3 && hardwareVertexData3->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData3->currentShape;
			if (!shapes && !hardwareVertexData3->shapes)
			{
				hardwareVertexData3->currentShape =
					shapes =
					hardwareVertexData3->shapes =
					rsManager->getBlock(hardwareVertexData3->numShapes);
			}

			if (shapes <= (hardwareVertexData3->shapes + hardwareVertexData3->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData3->currentShape = shapes;
		}
		else if (hardwareVertexData4 && hardwareVertexData4->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData4->currentShape;
			if (!shapes && !hardwareVertexData4->shapes)
			{
				hardwareVertexData4->currentShape =
					shapes =
					hardwareVertexData4->shapes =
					rsManager->getBlock(hardwareVertexData4->numShapes);
			}

			if (shapes <= (hardwareVertexData4->shapes + hardwareVertexData4->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData4->currentShape = shapes;
		}
		else if (hardwareVertexData5 && hardwareVertexData5->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData5->currentShape;
			if (!shapes && !hardwareVertexData5->shapes)
			{
				hardwareVertexData5->currentShape =
					shapes =
					hardwareVertexData5->shapes =
					rsManager->getBlock(hardwareVertexData5->numShapes);
			}

			if (shapes <= (hardwareVertexData5->shapes + hardwareVertexData5->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData5->currentShape = shapes;
		}
		else	//If we got here, something is really wrong
		{
#ifdef _DEBUG
			SPEW(("GRAPHICS", "Flags do not match any set of untextured shapes\n"));
#endif
		}
	}
}

// PERF FIX 2026-05-07: hash-based dedup map. The previous linear-scan
// implementation walked all existing entries doing 900-byte memcmp per
// comparison. Tracy capture (728 trees, default config) showed 8.5 ms
// total per frame — every actor scanning every prior actor's terrain-
// light-scaled struct, all unique due to per-actor aRGB. Hash-first
// approach: O(1) average lookup; memcmp only on hash match (collision
// verify). Map is reset by resetLightData() at frame start to mirror
// lightDataStructuresCount=0 reset.
//
// Hash collisions: 64-bit FNV-1a over ~1KB struct. Birthday-paradox
// probability ~10⁻⁷ per 728-actor frame; even on collision, memcmp
// fails-safe by falling through to the append path. Logical duplicates
// from collisions are correct (same-data → same-result), just waste a
// UBO slot. Acceptable.
namespace {
    static std::unordered_map<uint64_t, uint32_t> s_lightDataDedupMap;

    struct CachedSceneLightTemplate {
        TG_HWLightsData data;
        uint32_t actorLightSlot;
    };

    static std::unordered_map<uint64_t, CachedSceneLightTemplate> s_sceneLightTemplateMap;
    static uint32_t s_sceneLightTemplateFrame = 0xFFFFFFFFu;

    static inline uint64_t fnv1a_64_bytes(uint64_t h, const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    static inline uint64_t fnv1a_64_struct(const void* data, size_t bytes) {
        // FNV-1a 64-bit. Walk in uint64_t chunks for fewer iterations
        // (~110 iters for ~900 bytes vs ~900 iters byte-at-a-time).
        const uint64_t* p64 = static_cast<const uint64_t*>(data);
        const size_t chunks = bytes / sizeof(uint64_t);
        const size_t tail   = bytes % sizeof(uint64_t);
        uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
        for (size_t i = 0; i < chunks; ++i) {
            h ^= p64[i];
            h *= 0x100000001b3ULL;            // FNV prime
        }
        if (tail) {
            const uint8_t* tailp = reinterpret_cast<const uint8_t*>(&p64[chunks]);
            for (size_t i = 0; i < tail; ++i) {
                h ^= tailp[i];
                h *= 0x100000001b3ULL;
            }
        }
        return h;
    }

    static inline uint64_t fnv1a_64_u32(uint64_t h, uint32_t v) {
        return fnv1a_64_bytes(h, &v, sizeof(v));
    }

    static inline uint64_t fnv1a_64_bool(uint64_t h, bool v) {
        const uint8_t b = v ? 1 : 0;
        return fnv1a_64_bytes(h, &b, sizeof(b));
    }

    static inline uint32_t decomposeFirstActiveLightColor(TG_HWLightsData* lights) {
        const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
        const DWORD numLights = TG_Shape::s_numLights;

        for (DWORD iLight = 0; iLight < numLights; ++iLight) {
            if ((listOfLights[iLight] != NULL) && listOfLights[iLight]->active) {
                const DWORD startLight = listOfLights[iLight]->GetaRGB();
                lights->lightColor[0][0] = ((startLight >> 16) & 0x000000ff) / 255.0f;
                lights->lightColor[0][1] = ((startLight >> 8) & 0x000000ff) / 255.0f;
                lights->lightColor[0][2] = ((startLight) & 0x000000ff) / 255.0f;
                lights->lightColor[0][3] = 1.0f;
                return 0;
            }
        }

        return 0xFFFFFFFFu;
    }

    static inline uint32_t firstActiveLightSourceIndex() {
        const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
        const DWORD numLights = TG_Shape::s_numLights;

        if (!listOfLights)
            return 0xFFFFFFFFu;

        for (DWORD iLight = 0; iLight < numLights; ++iLight) {
            if ((listOfLights[iLight] != NULL) && listOfLights[iLight]->active)
                return iLight;
        }
        return 0xFFFFFFFFu;
    }

    static uint64_t sceneLightTemplateKey(uint32_t actorLightSourceIndex) {
        const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
        const DWORD numLights = TG_Shape::s_numLights;

        uint64_t h = 0xcbf29ce484222325ULL;
        h = fnv1a_64_u32(h, g_mc2FrameCounter);
        h = fnv1a_64_u32(h, numLights);

        const uintptr_t listPtr = reinterpret_cast<uintptr_t>(listOfLights);
        h = fnv1a_64_bytes(h, &listPtr, sizeof(listPtr));

        for (DWORD iLight = 0; iLight < numLights; ++iLight) {
            const TG_LightPtr light = listOfLights ? listOfLights[iLight] : NULL;
            h = fnv1a_64_bool(h, light != NULL);
            if (!light)
                continue;

            h = fnv1a_64_bool(h, light->active);
            if (!light->active)
                continue;

            h = fnv1a_64_u32(h, light->lightType);
            h = fnv1a_64_bytes(h, &light->lightToWorld, sizeof(light->lightToWorld));
            h = fnv1a_64_bytes(h, &light->closeDistance, sizeof(light->closeDistance));
            h = fnv1a_64_bytes(h, &light->farDistance, sizeof(light->farDistance));
            h = fnv1a_64_bytes(h, &light->oneOverDistance, sizeof(light->oneOverDistance));

            // Per-actor terrain scaling mutates the first active light color
            // before CacheGpuLightData(). Other active light colors remain part
            // of the scene key. If future gameplay makes more colors per-actor,
            // widen this patch/key rule instead of reusing the template.
            if (iLight != actorLightSourceIndex) {
                const DWORD argb = light->GetaRGB();
                h = fnv1a_64_u32(h, argb);
            }
        }

        return h;
    }

    // [LIGHTBRIDGE v1] C5/C6 populate sizing recon (env-gated, measure-only,
    // demote-not-delete). The handoff's prescribed shape_emit_ns counter was
    // grep-proven to wrap only the C1 legacy leaf (tgl.cpp:2602 scope, gated
    // !eligibleForGpuObjects) which is structurally dead for the GPU-batched
    // population this slice targets; the C5/C6 path
    // (addLightDataStructureWithPerActorColor, the sole caller route from
    // GatherGpuObjectLightDataOnly) had NO armed-path attribution. This is
    // that attribution: ONE std::chrono pair per call (NOT a per-call Tracy
    // zone / not nested -> ~30-50ns/call observer effect, << the claimed
    // multi-ms lever; the 6-nested-scope cost-split apparatus that inflated
    // terrain numbers is the anti-pattern this deliberately avoids), summed
    // per-frame, drained at resetLightData() (frame-start). Gated on the SAME
    // MC2_OBJECT_RECON_TRACY the handoff capture protocol already sets, so the
    // protocol is unchanged. tmpl_hit counts the template-cache-hit calls
    // whose trailing FNV+memcmp is the redundancy the slice retires.
    static bool     s_lbInit = false;
    static bool     s_lbEnabled = false;
    static uint64_t s_lbFrameNs = 0,    s_lbMonoNs = 0;
    static uint64_t s_lbFrameCalls = 0, s_lbMonoCalls = 0;
    static uint64_t s_lbFrameHit = 0,   s_lbMonoHit = 0;
    static uint64_t s_lbFrameMiss = 0,  s_lbMonoMiss = 0;
    static uint64_t s_lbFrameNo = 0,    s_lbMonoNo = 0;
    static uint32_t s_lbFirstDataFrame = 0;

    static inline void lbInitFromEnv() {
        if (s_lbInit) return;
        s_lbInit = true;
        const char* e = std::getenv("MC2_OBJECT_RECON_TRACY");
        s_lbEnabled = (e != nullptr && e[0] != '\0' && e[0] != '0');
        if (s_lbEnabled) {
            std::puts("[LIGHTBRIDGE v1] event=enabled note=c5c6_populate_sizing_active");
            std::fflush(stdout);
        }
    }

    struct LbScope {
        std::chrono::steady_clock::time_point t0;
        LbScope() : t0(std::chrono::steady_clock::now()) {}
        ~LbScope() {
            s_lbFrameNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
            ++s_lbFrameCalls;
        }
    };

    static void lbDrainPerFrame(uint32_t frame) {
        if (!s_lbInit) lbInitFromEnv();
        const bool hadData = (s_lbFrameCalls != 0);
        if (hadData && s_lbFirstDataFrame == 0) s_lbFirstDataFrame = frame;

        if (s_lbEnabled && hadData) {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "[LIGHTBRIDGE v1] frame=%u populate={ns:%llu,calls:%llu,"
                "tmpl_hit:%llu,tmpl_miss:%llu,no_actor_light:%llu}",
                (unsigned)frame,
                (unsigned long long)s_lbFrameNs,  (unsigned long long)s_lbFrameCalls,
                (unsigned long long)s_lbFrameHit, (unsigned long long)s_lbFrameMiss,
                (unsigned long long)s_lbFrameNo);
            std::puts(buf); crashbundle_append(buf); std::fflush(stdout);
        }

        s_lbMonoNs   += s_lbFrameNs;   s_lbMonoCalls += s_lbFrameCalls;
        s_lbMonoHit  += s_lbFrameHit;  s_lbMonoMiss  += s_lbFrameMiss;
        s_lbMonoNo   += s_lbFrameNo;
        s_lbFrameNs = s_lbFrameCalls = s_lbFrameHit = s_lbFrameMiss = s_lbFrameNo = 0;

        if (frame > 0 && (frame % 600) == 0 && s_lbMonoCalls != 0) {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "[LIGHTBRIDGE v1] summary=%u populate={ns:%llu,calls:%llu,"
                "tmpl_hit:%llu,tmpl_miss:%llu,no_actor_light:%llu} "
                "ns_per_call=%llu first_data_frame=%u",
                (unsigned)frame,
                (unsigned long long)s_lbMonoNs,  (unsigned long long)s_lbMonoCalls,
                (unsigned long long)s_lbMonoHit, (unsigned long long)s_lbMonoMiss,
                (unsigned long long)s_lbMonoNo,
                (unsigned long long)(s_lbMonoCalls ? s_lbMonoNs / s_lbMonoCalls : 0),
                (unsigned)s_lbFirstDataFrame);
            std::puts(buf); crashbundle_append(buf); std::fflush(stdout);
        }
    }

    // ---- [LIGHTBRIDGE v1] SUBSTITUTIVE REPOINT (slice, not recon) ----------
    // Retires the per-call 1792B fnv1a_64_struct + 1792B memcmp for the
    // C5/C6/C7 GPU-object populate. On the 99.3% template-cache-hit common
    // path the slot is resolved by a tiny (templateKey + per-actor-aRGB)
    // key instead. See docs/superpowers/plans/
    // 2026-05-17-addlightdatastructure-bridge-retirement.md. Kill-switch
    // MC2_LIGHTBRIDGE (default ON; =0 restores legacy FNV/memcmp bit-for-
    // bit). Slot cache is per-frame, cleared in resetLightData alongside
    // s_lightDataDedupMap (same frame-start slot-reset invariant).
    struct LightSlotEntry { uint64_t tmpl; uint32_t actorARGB; uint32_t slot; };
    static std::unordered_map<uint64_t, LightSlotEntry> s_lightSlotByActorKey;

    static bool s_lbRepointInit = false;
    static bool s_lbRepointEnabled = true;          // default ON
    static bool s_lbFirstPopulateLogged = false;
    static bool s_lbC6FastpathLogged = false;

    static inline void lbRepointInitFromEnv() {
        if (s_lbRepointInit) return;
        s_lbRepointInit = true;
        const char* e = std::getenv("MC2_LIGHTBRIDGE");
        // OFF only on explicit "0"; any other value (or unset) = ON.
        s_lbRepointEnabled = !(e != nullptr && e[0] == '0' && e[1] == '\0');
        std::printf("[LIGHTBRIDGE v1] event=enabled mode=%s\n",
                    s_lbRepointEnabled ? "repoint" : "legacy");
        std::fflush(stdout);
    }

    // ---- [LIGHTBAKE v1] static-actor mission-load lighting bake --------
    // Static bdactor bldg/tree actors have a mission-constant per-actor
    // light (position-derived getTerrainLight + frozen sun/nightFactor;
    // no dynamic emitters -- lighting_is_mission_load_static_no_dynamic_
    // emitters.md). Lazily bake the post-decompose TG_HWLightsData once
    // (keyed by monotonic-never-reused registry recipeIndex) then re-emit
    // the constant into a per-frame slot WITHOUT the per-frame
    // GatherLights+decompose+template recompute (the retired CPU zone).
    // Shape-C precedent: recompute dies, the O(1) per-frame slot WRITE
    // stays. Post-SSBO this is the full design (no window/partition --
    // see docs/superpowers/plans/2026-05-17-static-lighting-bake-SIMPLIFIED.md).
    // s_bakedStaticLight = mission-scoped struct SOURCE (recipeIndex ->
    // baked TG_HWLightsData; cleared on mission unload + per-recipe on
    // invalidate; NOT per-frame). [LIGHTBAKE v2] persistent static table:
    // each static recipe owns a PERMANENT lightData_ slot == recipeIndex
    // in [0..s_staticLightHighWater); written ONCE at bake (CPU mirror),
    // re-shipped idempotently by the unchanged per-frame whole-buffer
    // upload (no per-frame addLightDataStructure / FNV / memcmp). S =
    // max(recipeIndex)+1; resetLightData rebases the dynamic count to S
    // so dynamic appends never collide into [0..S).
    static std::unordered_map<int32_t, TG_HWLightsData> s_bakedStaticLight;
    static uint32_t                                      s_staticLightHighWater = 0;
    static bool s_bakeInit = false;
    static bool s_bakeEnabled = true;          // default ON
    static bool s_bakeFirstLogged = false;

    static inline void bakeInitFromEnv() {
        if (s_bakeInit) return;
        s_bakeInit = true;
        const char* e = std::getenv("MC2_LIGHTBAKE");
        s_bakeEnabled = !(e != nullptr && e[0] == '0' && e[1] == '\0');
        std::printf("[LIGHTBAKE v1] event=enabled mode=%s\n",
                    s_bakeEnabled ? "bake" : "passthrough");
        std::fflush(stdout);
    }
}

// Cross-TU kill-switch view (C6 repoint lives in msl.cpp). Free function
// to avoid pulling txmmgr.h into msl.cpp. Lazy-inits the env read.
bool mc2LightBridgeRepointEnabled()
{
    lbRepointInitFromEnv();
    return s_lbRepointEnabled;
}

// ---- [LIGHTBAKE v1] cross-TU free fns (bdactor/msl/registry call these;
// free fns avoid pulling txmmgr.h into those TUs) ----
bool mc2LightBakeEnabled()
{
    bakeInitFromEnv();
    return s_bakeEnabled;
}

bool mc2GetBakedStaticLight(int32_t recipeIndex, TG_HWLightsData& out)
{
    if (recipeIndex < 0) return false;
    auto it = s_bakedStaticLight.find(recipeIndex);
    if (it == s_bakedStaticLight.end()) return false;
    out = it->second;
    return true;
}

void mc2SetBakedStaticLight(int32_t recipeIndex, const TG_HWLightsData& in)
{
    if (recipeIndex < 0) return;
    s_bakedStaticLight[recipeIndex] = in;
    if (!s_bakeFirstLogged) {
        s_bakeFirstLogged = true;
        std::printf("[LIGHTBAKE v1] event=first_bake recipe=%d\n", recipeIndex);
        std::fflush(stdout);
    }
}

// Erased on destruction/LOD multi-swap (via invalidateStaticRegistration
// -> GpuStaticPropRegistry::invalidate) so the next CacheGpuLightData
// lazily re-bakes the same position-derived constant for the new multi.
void mc2EraseBakedStaticLight(int32_t recipeIndex)
{
    if (recipeIndex < 0) return;
    s_bakedStaticLight.erase(recipeIndex);
}

// Mission unload: recipeIndex restarts per mission -> a stale entry
// would alias a different actor. Drop the whole mission-scoped map.
void mc2ClearAllBakedStaticLight()
{
    // Co-located with GpuStaticPropRegistry::destroy s_recipeRanges.clear()
    // (gos_static_prop_registry.cpp) via mission.cpp -> next mission's
    // recipeIndex restarts at 0 against a fresh prefix.
    s_bakedStaticLight.clear();
    s_staticLightHighWater = 0;
}

// [LIGHTBAKE v2] Persistent static slot write. Replaces the retired
// per-frame mc2SubmitBakedLightSlot (which re-ran addLightDataStructure
// -> 1792B FNV + 1792B memcmp every frame per recipe). Called ONCE per
// recipe at bake (and again only on invalidate re-bake): mirror the
// constant into the permanent CPU slot lightData_[recipeIndex] and
// advance S. The unchanged per-frame whole-buffer upload then ships it
// to the GPU every frame idempotently (resetLightData never memsets
// lightData_ contents). No GL call, no FNV, no memcmp.
void mc2WriteStaticLightSlot(int32_t recipeIndex, const TG_HWLightsData& baked)
{
    if (recipeIndex < 0 || !mcTextureManager) return;
    mcTextureManager->bakeStaticLightSlot(recipeIndex, baked);
}

uint32_t MC_TextureManager::addLightDataStructure(TG_HWLightsData* light_data)
{
    // Tracy zone retained — formerly named "scan", now wraps the whole
    // function so old captures remain comparable. Should drop from
    // ~12 µs/call (linear scan) to ~200-300 ns/call (hash + map ops).
    ZoneScopedN("addLightDataStructure scan");

    const uint64_t hash = fnv1a_64_struct(light_data, sizeof(TG_HWLightsData));
    auto it = s_lightDataDedupMap.find(hash);
    if (it != s_lightDataDedupMap.end()) {
        const uint32_t slot = it->second;
        // Verify with memcmp on hash match (collision safety).
        if (slot < lightDataStructuresCount &&
            0 == memcmp(lightData_ + slot, light_data, sizeof(TG_HWLightsData))) {
            return slot;
        }
        // Hash collision (vanishingly rare) — fall through to append.
        // We don't update the map; future lookups of the colliding hash
        // will continue to find the existing slot via memcmp-verify.
    }

    // unique data passed, so add it
    if(lightDataStructuresCount + 1 >= lightDataStructuresCapacity)
    {
        TG_HWLightsData* new_lights_data = new TG_HWLightsData[lightDataStructuresCapacity + 128];
        memcpy(new_lights_data, lightData_, sizeof(TG_HWLightsData)*lightDataStructuresCount);
        delete[] lightData_;
        lightData_ = new_lights_data;
        lightDataStructuresCapacity += 128;
    }

    lightData_[lightDataStructuresCount] = *light_data;
    uint32_t rv = lightDataStructuresCount;
    lightDataStructuresCount++;
    s_lightDataDedupMap.emplace(hash, rv);  // O(1) avg insert

    // PERF DIAGNOSTIC 2026-05-06: log table growth periodically. 1 line per
    // 256 new entries. DEMOTED 2026-05-17 to env-gated (its own "demote
    // once the regression is closed" instruction): the dedup-growth
    // regression is closed (D2 + SSBO + static-bake shipped); it was
    // emitting ~6k lines/run and polluting frame-time captures.
    // Capability kept (debug_instrumentation_rule: demote-not-delete) --
    // set MC2_LIGHT_DEDUP_TRACE=1 to re-enable.
    static const bool s_lightDedupTrace =
        (std::getenv("MC2_LIGHT_DEDUP_TRACE") != nullptr);
    if (s_lightDedupTrace && (lightDataStructuresCount & 0xFF) == 0) {
        printf("[LIGHT_DEDUP v1] count=%u capacity=%u memcmp_per_call_bytes_max=%zu\n",
               lightDataStructuresCount,
               lightDataStructuresCapacity,
               (size_t)lightDataStructuresCount * sizeof(TG_HWLightsData));
        fflush(stdout);
    }
    return rv;
}

uint32_t MC_TextureManager::addLightDataStructureWithPerActorColor(TG_HWLightsData* light_data)
{
    gosASSERT(light_data);
    LbScope _lb_;  // [LIGHTBRIDGE v1] C5/C6 populate sizing (RAII, all return paths)

    if (s_sceneLightTemplateFrame != g_mc2FrameCounter) {
        s_sceneLightTemplateMap.clear();
        s_sceneLightTemplateFrame = g_mc2FrameCounter;
    }

    const uint32_t actorLightSource = firstActiveLightSourceIndex();
    if (actorLightSource == 0xFFFFFFFFu) {
        ++s_lbFrameNo;  // [LIGHTBRIDGE v1] no per-actor light (direct passthrough)
        GatherLightsParameters(light_data);
        return addLightDataStructure(light_data);
    }

    const uint64_t key = sceneLightTemplateKey(actorLightSource);
    auto it = s_sceneLightTemplateMap.find(key);
    if (it == s_sceneLightTemplateMap.end()) {
        ++s_lbFrameMiss;  // [LIGHTBRIDGE v1] template miss (GatherLightsParameters runs)
        CachedSceneLightTemplate entry;
        GatherLightsParameters(&entry.data);
        entry.actorLightSlot = decomposeFirstActiveLightColor(&entry.data);
        it = s_sceneLightTemplateMap.emplace(key, entry).first;
    } else {
        ++s_lbFrameHit;  // [LIGHTBRIDGE v1] template hit (trailing FNV+memcmp = retirable redundancy)
    }

    *light_data = it->second.data;
    const bool perActor = (it->second.actorLightSlot != 0xFFFFFFFFu);
    if (perActor)
        decomposeFirstActiveLightColor(light_data);

    // [LIGHTBRIDGE v1] substitutive repoint: on the (template + per-actor-
    // color) cache hit, return the resolved slot directly and SKIP the
    // 1792B fnv1a_64_struct + 1792B memcmp in addLightDataStructure.
    // Symmetry invariant (load-bearing — do not break without re-deriving
    // the key): actorLightSource == firstActiveLightSourceIndex() is the
    // SAME light decompose mutates into lightColor[0][0..3] AND the SAME
    // light sceneLightTemplateKey deliberately excludes from the template
    // key (txmmgr.cpp ~:1017-1020). actorARGB closes exactly that excluded
    // gap. perActor==false => no decompose => template key alone suffices
    // (actorARGB folds to 0, combined == key).
    if (!s_lbRepointInit) lbRepointInitFromEnv();
    if (s_lbRepointEnabled) {
        const uint32_t actorARGB = perActor
            ? (uint32_t)TG_Shape::s_listOfLights[actorLightSource]->GetaRGB()
            : 0u;
        const uint64_t combined =
            key ^ ((uint64_t)actorARGB * 0x9E3779B97F4A7C15ULL);
        auto sit = s_lightSlotByActorKey.find(combined);
        if (sit != s_lightSlotByActorKey.end() &&
            sit->second.tmpl == key && sit->second.actorARGB == actorARGB) {
            return sit->second.slot;   // retired: no FNV, no memcmp
        }
        const uint32_t slot = addLightDataStructure(light_data);
        s_lightSlotByActorKey[combined] =
            LightSlotEntry{ key, actorARGB, slot };
        if (!s_lbFirstPopulateLogged) {
            s_lbFirstPopulateLogged = true;
            std::puts("[LIGHTBRIDGE v1] event=first_populate");
            std::fflush(stdout);
        }
        return slot;
    }

    return addLightDataStructure(light_data);
}

void MC_TextureManager::resetLightData()
{
    // [LIGHTBRIDGE v1] frame-start boundary: flush the just-completed frame's
    // C5/C6 populate sizing (same boundary the dedup-map reset relies on).
    lbDrainPerFrame(g_mc2FrameCounter);

    // [LIGHTBAKE v2] Rebase the DYNAMIC allocator base to S (the static
    // prefix high-water) when the bake is on, so dynamic appends start
    // above [0..S) and addLightDataStructure never returns a slot < S
    // (rv = count, count never < S -> the dedup maps below stay
    // dynamic-only by construction, exactly as before but rebased). With
    // MC2_LIGHTBAKE=0 the persistent table is off -> base 0 (else the
    // dynamic allocator would collide into a non-rebased prefix).
    lightDataStructuresCount = mc2LightBakeEnabled() ? s_staticLightHighWater : 0;
    // PERF FIX 2026-05-07: clear the dedup map alongside the count reset.
    // Both must reset together — dynamic slot indices restart from the
    // base (S or 0) each frame, so any stale hash→slot entries from the
    // prior frame are invalid. (Static [0..S) is NEVER cleared here:
    // s_bakedStaticLight + lightData_[0..S) persist across frames; that
    // is the whole point — resetLightData does not memset lightData_.)
    s_lightDataDedupMap.clear();
    s_sceneLightTemplateMap.clear();
    s_lightSlotByActorKey.clear();  // [LIGHTBRIDGE v1] per-frame slot cache
    s_sceneLightTemplateFrame = 0xFFFFFFFFu;
}

// [LIGHTBAKE v2] Persistent static slot writer (member: needs private
// lightData_/capacity access). Grow lightData_ so [recipeIndex] is
// addressable (preserving ALL existing contents -- static prefix AND any
// transient dynamic entries -- via the same realloc+memcpy pattern as
// the addLightDataStructure grow), mirror the baked constant into the
// permanent slot, advance S. Called once per recipe at bake / invalidate
// re-bake -- NOT per frame.
void MC_TextureManager::bakeStaticLightSlot(int32_t recipeIndex, const TG_HWLightsData& baked)
{
    if (recipeIndex < 0) return;
    const uint32_t ri = static_cast<uint32_t>(recipeIndex);
    if (ri + 1 >= lightDataStructuresCapacity)
    {
        uint32_t newCap = lightDataStructuresCapacity;
        while (ri + 1 >= newCap) newCap += 128;            // +128 chunks, like the dynamic grow
        TG_HWLightsData* grown = new TG_HWLightsData[newCap];
        // Preserve the FULL old array (static prefix is in [0..S); copying
        // only `count` would drop persisted static slots).
        memcpy(grown, lightData_, sizeof(TG_HWLightsData) * lightDataStructuresCapacity);
        delete[] lightData_;
        lightData_ = grown;
        lightDataStructuresCapacity = newCap;
    }
    lightData_[ri] = baked;                                 // CPU mirror (persists)
    if (ri + 1 > s_staticLightHighWater) s_staticLightHighWater = ri + 1;
    // CRITICAL: the frame a recipe FIRST bakes, this-frame's count was
    // set to the OLD (smaller) S at frame-start resetLightData. Without
    // this bump, a later dynamic addLightDataStructure append THIS frame
    // could land on slot `ri` and clobber the just-written permanent
    // static slot (wrong-light, never re-baked). Raising the live count
    // to S keeps same-frame dynamic appends strictly above [0..S). Only
    // skips some low dynamic slots that frame (harmless -- dynamic is
    // per-frame ephemeral, rebuilt from S next resetLightData).
    if (lightDataStructuresCount < s_staticLightHighWater)
        lightDataStructuresCount = s_staticLightHighWater;
}

// Diagnostic body — declaration in txmmgr.h. See header for rationale.
MC_TextureManager::LightSlotPeek MC_TextureManager::peekLightSlot(uint32_t idx) const
{
    LightSlotPeek p = {-1, -1, 0.0f, 0.0f, 0.0f};
    if (idx >= lightDataStructuresCount || !lightData_) return p;
    const TG_HWLightsData& d = lightData_[idx];
    p.numLights = d.numLights_;
    if (d.numLights_ > 0) {
        // light_dir[i].w carries the light type (TG_LIGHT_AMBIENT=0, INFINITE=1,
        // INFINITEWITHFALLOFF=2, POINT=3, SPOT=4, TERRAIN=5). Mirrors GLSL
        // ObjectLights.light_dir[i].w in shaders/include/lighting.hglsl.
        p.firstType   = static_cast<int>(d.lightDir[0][3]);
        p.firstColorR = d.lightColor[0][0];
        p.firstColorG = d.lightColor[0][1];
        p.firstColorB = d.lightColor[0][2];
    }
    return p;
}

mat4 gos2my(Stuff::Matrix4D& m)
{
	mat4 m2(
		m.entries[0], m.entries[1], m.entries[2], m.entries[3],
		m.entries[4], m.entries[5], m.entries[6], m.entries[7],
		m.entries[8], m.entries[9], m.entries[10], m.entries[11],
		m.entries[12], m.entries[13], m.entries[14], m.entries[15]);
	return m2;
}

mat4 gos2my(Stuff::LinearMatrix4D& m)
{
	mat4 m2(
		m.entries[0], m.entries[1], m.entries[2], m.entries[3],
		m.entries[4], m.entries[5], m.entries[6], m.entries[7],
		m.entries[8], m.entries[9], m.entries[10], m.entries[11],
		0.0f, 0.0f, 0.0f, 1.0f);
	return m2;
}


////////////////////////////////////////////////////////////////////////////////
class ShapeRenderer {

	mat4* world_;
	mat4* view_;
	mat4* wvp_;
	float* viewport_;
	HGOSBUFFER lights_data_;

public:

	void setup(mat4* world, mat4* view, mat4* wvp, float* viewport)
	{
		gosASSERT(world && view && wvp && viewport);
		world_ = world;
		view_ = view;
		wvp_ = wvp;
		viewport_ = viewport;
	}

	void set_lights_data(const HGOSBUFFER lights_data)
	{
		lights_data_ = lights_data;
	}

	void render(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, DWORD texture_id, int light_index, bool isHudElement = false)
	{
		gos_SetRenderState(gos_State_Texture, texture_id);
		gos_SetRenderViewport(viewport_[2], viewport_[3], viewport_[0], viewport_[1]);

		HGOSRENDERMATERIAL mat = texture_id == 0 ? gos_getRenderMaterial("gos_vertex_lighted") : gos_getRenderMaterial("gos_tex_vertex_lighted");

		gos_SetRenderMaterialParameterMat4(mat, "world_", (const float*)*world_);
		//gos_SetRenderMaterialParameterMat4(mat, "view_", (const float*)*view_);
		gos_SetRenderMaterialParameterMat4(mat, "wvp_", (const float*)*wvp_);

        float ld[4] = { (float)light_index, 0.0f, 0.0f, 0.0f};
		gos_SetRenderMaterialParameterFloat4(mat, "light_offset_", ld);

		// GPU projection via terrainMVP (skip for HUD elements which need legacy viewport projection)
		gos_SetRenderMaterialParameterInt(mat, "gpuProjection", isHudElement ? 0 : 1);

		// [LIGHTSSBO v1] FORK-2: LightsData is now an SSBO; the UBO-
		// reflection bind below would silently no-op (SSBO blocks are not
		// in GL_ACTIVE_UNIFORM_BLOCKS) -> legacy lit meshes would render
		// garbage lighting. Bind the storage block explicitly instead.
		// SceneData stays a UBO.
		gos_SetRenderMaterialUniformBlockBindingPoint(mat, "SceneData", SCENE_DATA_ATTACHMENT_SLOT);

		gos_ApplyRenderMaterial(mat);
		gos_BindLightDataStorageBlock(mat);

		// Bind shadow maps + terrainMVP after apply() (requires active program)
		if (!isHudElement) {
			gos_SetupObjectShadows(mat);
		}

		gos_RenderIndexedArray(ib, vb, vdecl);

	}

};

void GatherLightsParameters(TG_HWLightsData* lights)
{
	gosASSERT(lights);

	// Stage 2.D.2 diagnostic: dump gathered lights when MC2_OBJECT_PARITY_TRACE=1.
	// Fired once per session to avoid per-frame spam. Shows what GPU UBO gets.
	static bool s_lightDumpDone = false;
	const bool doLightTrace = (!s_lightDumpDone && [](){
		const char* v = getenv("MC2_OBJECT_PARITY_TRACE");
		return v && v[0] == '1' && v[1] == '\0';
	}());

	uint32_t num_lights = 0;
	const uint32_t max_num_lights = MAX_HW_LIGHTS_IN_WORLD;

	const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
	const DWORD numLights = TG_Shape::s_numLights;

	// T1.15 [SPOT_DIAG v1] per-call active/inactive/point_active tally.
	unsigned diagActiveLights = 0;
	unsigned diagInactiveLights = 0;
	unsigned diagPointActive = 0;

	for (uint32_t iLight = 0; iLight < numLights; iLight++)
	{
		if (num_lights == max_num_lights)
			break;

		// T1.15 [SPOT_DIAG v1] count active/inactive lights in the source list
		// before the active-filter culls them. Read `active` ONCE here; the
		// canonical filter below reads it again, but the field is plain DWORD
		// and not externally mutated between these two reads in this loop.
		if (listOfLights[iLight] != NULL) {
			const DWORD t = listOfLights[iLight]->lightType;
			if (listOfLights[iLight]->active) {
				++diagActiveLights;
				if (t == TG_LIGHT_POINT) ++diagPointActive;
			} else {
				++diagInactiveLights;
			}
		}

		if ((listOfLights[iLight] != NULL) && (listOfLights[iLight]->active))
		{

			const DWORD type = listOfLights[iLight]->lightType;

			Stuff::LinearMatrix4D light2world;
			if (TG_LIGHT_AMBIENT != type)
				light2world = listOfLights[iLight]->lightToWorld;
			else
				light2world = Stuff::LinearMatrix4D::Identity;

			memcpy(lights->lightToWorld[num_lights], (const float*)light2world, 12*sizeof(float));
			lights->lightToWorld[num_lights][12] = lights->lightToWorld[num_lights][13] = lights->lightToWorld[num_lights][14] = 0.0f;
			lights->lightToWorld[num_lights][15] = 1.0f;

			Stuff::UnitVector3D uVec;
			light2world.GetLocalForwardInWorld(&uVec);
			lights->lightDir[num_lights][0] = uVec.x;
			lights->lightDir[num_lights][1] = uVec.y;
			lights->lightDir[num_lights][2] = uVec.z;

			lights->lightDir[num_lights][3] = (float)type;

			DWORD startLight = listOfLights[iLight]->GetaRGB();

			lights->lightColor[num_lights][0] = ((startLight >> 16) & 0x000000ff) / 255.0f;
			lights->lightColor[num_lights][1] = ((startLight >> 8) & 0x000000ff) / 255.0f;
			lights->lightColor[num_lights][2] = ((startLight) & 0x000000ff) / 255.0f;
			lights->lightColor[num_lights][3] = 1.0f;

			// Slice 2 (object-offload) — Stage 2.C: per-light falloff fields.
			// GLSL `GetFalloff` reads .x=closeDistance, .y=farDistance,
			// .z=oneOverDistance. Source on the CPU side is
			// TG_Light::{closeDistance,farDistance,oneOverDistance} at
			// mclib/tgl.h:193-195. AMBIENT lights don't use distance falloff
			// (the GLSL kernel hits the AMBIENT case before reading falloff),
			// but populate the fields anyway for cache uniformity.
			lights->lightFalloff[num_lights][0] = listOfLights[iLight]->closeDistance;
			lights->lightFalloff[num_lights][1] = listOfLights[iLight]->farDistance;
			lights->lightFalloff[num_lights][2] = listOfLights[iLight]->oneOverDistance;
			lights->lightFalloff[num_lights][3] = 0.0f;

			switch (type)
			{
			case TG_LIGHT_AMBIENT:
				break;
			case TG_LIGHT_INFINITE:
			case TG_LIGHT_INFINITEWITHFALLOFF:
				break;
			case TG_LIGHT_POINT:
				break;
			case TG_LIGHT_TERRAIN:
				break;
			case TG_LIGHT_SPOT:
				break;
			default:
				STOP(("Unknown light type id: %d", type));
			}

			if (doLightTrace) {
				std::fprintf(stderr,
					"[PARITY_DIAG v2] GatherLightsParameters iLight=%u type=%u "
					"aRGB=0x%08X dir=(%.4f,%.4f,%.4f) color=(%.4f,%.4f,%.4f)\n",
					num_lights,
					(unsigned)type,
					(unsigned)listOfLights[iLight]->GetaRGB(),
					lights->lightDir[num_lights][0],
					lights->lightDir[num_lights][1],
					lights->lightDir[num_lights][2],
					lights->lightColor[num_lights][0],
					lights->lightColor[num_lights][1],
					lights->lightColor[num_lights][2]);
				std::fflush(stderr);
			}

			num_lights++;
		}
	}

	if (doLightTrace) {
		std::fprintf(stderr,
			"[PARITY_DIAG v2] GatherLightsParameters numLights=%u\n",
			num_lights);
		std::fflush(stderr);
		s_lightDumpDone = true;
	}

	lights->numLights_ = num_lights;

	// T1.15 [SPOT_DIAG v1] pack-probe emit. First-call always-on; summary
	// every 600 calls when env=1.
	++s_spotDiagPackCalls;
	s_spotDiagPackActiveSum += diagActiveLights;
	s_spotDiagPackInactSum  += diagInactiveLights;
	s_spotDiagPackPointSum  += diagPointActive;
	if (!s_spotDiagPackFirstHit) {
		s_spotDiagPackFirstHit = true;
		std::fprintf(stderr,
			"[SPOT_DIAG v1] event=pack_first_shape shape=%p active_lights=%u "
			"inactive_lights=%u point_lights_active=%u\n",
			(void*)lights, diagActiveLights, diagInactiveLights, diagPointActive);
		std::fflush(stderr);
	}
	if (s_spotDiagPackEnabled && (s_spotDiagPackCalls % 600) == 0) {
		double avgA = (double)s_spotDiagPackActiveSum / 600.0;
		double avgI = (double)s_spotDiagPackInactSum  / 600.0;
		double avgP = (double)s_spotDiagPackPointSum  / 600.0;
		std::fprintf(stderr,
			"[SPOT_DIAG v1] event=pack_summary calls=%lu "
			"avg_active_per_shape=%.3f avg_inactive_per_shape=%.3f "
			"avg_point_active_per_shape=%.3f\n",
			s_spotDiagPackCalls, avgA, avgI, avgP);
		std::fflush(stderr);
		s_spotDiagPackActiveSum = 0;
		s_spotDiagPackInactSum  = 0;
		s_spotDiagPackPointSum  = 0;
	}
}



//----------------------------------------------------------------------
// Draws all textures with isTerrain set that are solid first,
// then draws all alpha with isTerrain set.
void MC_TextureManager::renderLists (void)
{
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendDecal);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 1);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
		
	DWORD fogColor = eye->fogColor;
	//-----------------------------------------------------
	// FOG time.  Set Render state to FOG on!
	if (useFog)
	{
		//gos_SetRenderState( gos_State_Fog, (int)&fogColor);
		gos_SetRenderState( gos_State_Fog, fogColor); // sebi
	}
	else
	{
		gos_SetRenderState( gos_State_Fog, 0);
	}

	static bool bSkip = true;

	gos_SetRenderState(gos_State_Culling, gos_Cull_CW);

    // copy global list of light data into GPU buffer
    
    // [LIGHTSSBO v1] Upload-size FLOOR retained (NOT a removable UBO-window
    // artifact — falsified 2026-05-17). The engine deliberately tolerates
    // transient over-count lightDataIndex for cull-stale actors whose
    // update() was skipped offscreen (see gos_static_prop_registry.cpp
    // comment "...points at a slot ... beyond the upload count"). The
    // floor guarantees those indices still read valid backing memory
    // instead of past-end (-> zero -> black props). std::max keeps the
    // old max(count, 64) semantics; lightData_ is capacity(128)-sized so
    // sourcing 64 entries is in-bounds.
    constexpr uint32_t kLightUploadFloor = 64u;
    const size_t lightUploadCount =
        std::max<uint32_t>(lightDataStructuresCount, kLightUploadFloor);
    gos_LightDataSsbo_Upload(
        lightData_,
        lightUploadCount * sizeof(TG_HWLightsData));
    //
    
    // update scene data uniform buffer
    Stuff::Vector3D cp = eye->getCameraOrigin();
    {
        ZoneScopedN("Camera.SceneDataUpload");
        sceneData_->fog_start = eye->fogStart;
        sceneData_->fog_end = eye->fogFull;
        sceneData_->min_haze_dist = Camera::MinHazeDistance;
        sceneData_->dist_factor = Camera::DistanceFactor;
        sceneData_->cam_pos[0] = cp.x;
        sceneData_->cam_pos[1] = cp.y;
        sceneData_->cam_pos[2] = cp.z;
        sceneData_->cam_pos[3] = 1.0f;
        vec4 fc = uint32_to_vec4(eye->fogColor);
        sceneData_->fog_color[0] = fc.z;
        sceneData_->fog_color[1] = fc.y;
        sceneData_->fog_color[2] = fc.x;
        sceneData_->fog_color[3] = fc.w;
        sceneData_->baseVertexColor = uint32_to_vec4(BaseVertexColor).zyxw();
        gos_UpdateBuffer(sceneDataBuffer_, sceneData_, 0, sizeof(TG_HWSceneData));
    }
    
    

	{
		ZoneScopedN("Render.3DObjects");
		TracyGpuZone("Render.3DObjects");
	for (size_t i = 0; i<nextAvailableHardwareVertexNode; i++)
	{
		if ((masterHardwareVertexNodes[i].flags & MC2_DRAWSOLID) &&
			(masterHardwareVertexNodes[i].shapes))
		{
			if (masterHardwareVertexNodes[i].flags & MC2_ISTERRAIN)
				gos_SetRenderState(gos_State_TextureAddress, gos_TextureClamp);
			else
				gos_SetRenderState(gos_State_TextureAddress, gos_TextureWrap);

			uint32_t totalShapes = masterHardwareVertexNodes[i].numShapes;
			// in case less shapes were addded in Render() that it was "promised" in Update(), generally etter to investigate and remove all such cases
			if (masterHardwareVertexNodes[i].currentShape != (masterHardwareVertexNodes[i].shapes + masterHardwareVertexNodes[i].numShapes))
			{
				totalShapes = masterHardwareVertexNodes[i].currentShape - masterHardwareVertexNodes[i].shapes;
			}
			for (uint32_t sh = 0; sh < totalShapes; ++sh)
			{
				DWORD textureIndex = masterHardwareVertexNodes[i].textureIndex;
				if (textureIndex == 1227 && bSkip)
					continue;

				static bool b_old_way = false;
				if (b_old_way)
				{
					gos_SetRenderState(gos_State_Texture, masterTextureNodes[textureIndex].get_gosTextureHandle());
					TG_RenderShape* rs = masterHardwareVertexNodes[i].shapes + sh;
					gos_SetRenderViewport(rs->viewport_[2], rs->viewport_[3], rs->viewport_[0], rs->viewport_[1]);


					//gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);
					// TODO: set mvp_ in a separate function, like gos_set_render_camera(mvp_)...
					gos_RenderIndexedArray(rs->ib_, rs->vb_, rs->vdecl_, (const float*)rs->mvp_);
				}
				else
				{
					DWORD texture = masterTextureNodes[textureIndex].get_gosTextureHandle();
					TG_RenderShape* rs = masterHardwareVertexNodes[i].shapes + sh;


					mat4 view_mat = gos2my(TG_Shape::s_worldToCamera);
					mat4 world_mat = gos2my(rs->mw_);
					mat4 wvp_mat = gos2my(rs->mvp_);

					ShapeRenderer shape_renderer;
					shape_renderer.setup(&world_mat, &view_mat, &wvp_mat, rs->viewport_);
					shape_renderer.render(rs->vb_, rs->ib_, rs->vdecl_, texture, rs->light_data_buffer_index_, rs->isHudElement_);
				}

			}

			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterHardwareVertexNodes[i].currentShape = masterHardwareVertexNodes[i].shapes;
			//masterHardwareVertexNodes[i].numShapes = 0;
		}
	}
	} // end Render.3DObjects zone
	drainGLErrors("objects_3d");

	// [Moved in Phase 4 debug] flush() was originally here (after
	// Render.3DObjects). But Render.TerrainSolid runs AFTER us on line
	// ~1287, so terrain was overwriting our building pixels. Flush is
	// now relocated further down, after Render.TerrainSolid completes.

	// restore state as all old-style geometry is culled on CPU and all vertices are already pretransformed
	gos_SetRenderState(gos_State_Culling, gos_Cull_None);

	// restore viewport
	gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);

	// VPL-#shadow Phase 1+2 (arch-doc docs/plans/static-terrain-shadow-
	// architecture.md): build the static terrain shadow from the FULL map
	// ONCE. Was: a prime + a camera-windowed accumulate behind a >100u
	// camera-move gate -> the shadow FBO was fed only the ~110 visible
	// terrain nodes and never cleared -> a near-empty depth atlas ->
	// soft half-map shadow wash. Root cause is feed-scope and was
	// probe-proven: the world-fixed ortho light matrix is correct and
	// built-once ([SHADOWFRUSTUM v1] n=1 mapHalfExtent=6400
	// orthoHalf=9503.5; build & sample share getLightSpaceMatrix()).
	// Phase 1 retires the prime block, the camera-motion gate, and the
	// gos_*ShadowRebuild* API (this was their only caller). The build is
	// gated solely by the gos_StaticLightMatrixBuilt() latch, which
	// Terrain::destroy re-arms per mission (C-1) so mission 2+ rebuilds
	// against fresh blocks[]. The MapData full-map feed is stock-safe
	// (no-ops if blocks[] unallocated -> shadow simply absent, never a
	// crash, never worse than a missing shadow).
	if (gos_IsTerrainTessellationActive() && !gos_StaticLightMatrixBuilt() &&
	    Terrain::mapData) {
		ZoneScopedN("Shadow.StaticFullMapBuild");
		TracyGpuZone("Shadow.StaticFullMapBuild");

		gos_BuildStaticLightMatrix();   // world-fixed, camera-independent
		gos_MarkStaticLightMatrixBuilt();

		// Any valid terrain colormap: the shadow prepass is depth-only
		// (shadow_terrain.tese = plain lightSpaceMatrix*worldPos, not
		// sampled for depth), so the exact texture is irrelevant -- bind
		// the first terrain node's, else the solid default (idx 0).
		unsigned long shTex = masterTextureNodes[0].get_gosTextureHandle();
		for (long si = 0; si < nextAvailableVertexNode; si++) {
			if ((masterVertexNodes[si].flags & MC2_DRAWSOLID) &&
				(masterVertexNodes[si].flags & MC2_ISTERRAIN)) {
				shTex = masterTextureNodes[masterVertexNodes[si].textureIndex].get_gosTextureHandle();
				break;
			}
		}

		gos_BeginShadowPrePass(true);   // one-shot clear (no accumulate)
		Terrain::mapData->renderStaticTerrainShadowFullMap(indexArray, shTex);
		gos_EndShadowPrePass();
	}
	// GPU-driven dynamic sun shadow (Phase 1): frustum-fit + flushShadow.
	// Runs BEFORE gpu_cull::compute_dispatch so the static-prop shadow uses the
	// full camera-visible per-type ranges (not the cull-narrowed indirect).
	// Casters = the camera-visible (inView) batched set (Phase 1 scope; the
	// off-screen-caster low-sun shadow is the documented Phase-2 gap).
	{
		extern bool g_useGpuObjects;
		extern bool g_useGpuMechs;
		if (gos_IsTerrainTessellationActive() && (g_useGpuObjects || g_useGpuMechs)) {
			// Unproject 8 NDC corners through clipToWorld (-> STUFF space), then
			// swizzle Stuff->MC2 (-x, z, y) EXACTLY as Camera::inverseProjectZ does.
			// inverseProjectZ convention: Multiply(in, clipToWorld), then if
			// xformCoords.w < 0 call Negate (negates all 4 components), then
			// perspective-divide x/y/z by w, then swizzle (-x, z, y).
			// For raw NDC input (w=1) the perspective divide is required (unlike
			// inverseProjectZ which pre-bakes 1/screen.w into coords.w).
			static const float ndc[8][3] = {
				{-1.0f,-1.0f, 0.0f},{ 1.0f,-1.0f, 0.0f},
				{-1.0f, 1.0f, 0.0f},{ 1.0f, 1.0f, 0.0f},
				{-1.0f,-1.0f, 1.0f},{ 1.0f,-1.0f, 1.0f},
				{-1.0f, 1.0f, 1.0f},{ 1.0f, 1.0f, 1.0f}
			};
			float cornersMC2[8][3];
			// clipToWorld is protected; derive it from the public getWorldToClip().
			// Invert(src) stores the inverse of src into *this (matrix.hpp:584).
			Stuff::Matrix4D clipToWorld;
			clipToWorld.Invert(eye->getWorldToClip());
			for (int c = 0; c < 8; ++c) {
				Stuff::Vector4D in, out;
				in.x = ndc[c][0]; in.y = ndc[c][1]; in.z = ndc[c][2]; in.w = 1.0f;
				out.Multiply(in, clipToWorld);        // row-vector * matrix, arg order per camera.cpp:1977
				if (out.w < 0.0f)
					out.Negate(out);                  // mirrors inverseProjectZ:1979-1980
				float inv = (fabsf(out.w) > 1e-6f) ? (1.0f / out.w) : 0.0f;
				float sx = out.x * inv;
				float sy = out.y * inv;
				float sz = out.z * inv;
				cornersMC2[c][0] = -sx;               // Stuff->MC2: (-x, z, y) per camera.cpp:1982-1984
				cornersMC2[c][1] =  sz;
				cornersMC2[c][2] =  sy;
			}
			float lx, ly, lz;
			gos_GetTerrainLightDir(&lx, &ly, &lz);   // same accessor used by old shim
			gos_BuildDynamicLightMatrix(-lx, -ly, -lz, cornersMC2);  // sign matches old shim
			gos_BeginDynamicShadowPass();             // no-op if shadowsEnabled_ false
			GpuStaticPropBatcher::instance().flushShadow();
			GpuMechBatcher::instance().flushShadow();
			gos_EndDynamicShadowPass();
		}
	}
	g_numShadowShapes = 0;

	// No special depth state for DRAWSOLID terrain

	{
		ZoneScopedN("Render.TerrainSolid");
		TracyGpuZone("Render.TerrainSolid");

		// Modern path. flush() returns true on success and false on overflow
		// / not-ready / not-killswitched. On false we fall through to the
		// legacy loop for the WHOLE FRAME — never partial-frame. The legacy
		// ring data has been kept in sync by addVertices/fillTerrainExtra
		// running unconditionally in quad.cpp.
		// Bucket-census instrumentation (env-gated MC2_BUCKET_CENSUS=1).
		// Count legacy-eligible nodes BEFORE flush() runs (both the legacy
		// draw branch at ~1369 and the alternate reset branch at ~1382
		// zero currentVertex per node, so end-of-zone undercounts). The
		// filter mirrors the legacy DRAWSOLID|ISTERRAIN draw-emission
		// predicate so it is apples-to-apples with PatchStream's modern
		// scope.
		static const bool s_bucketCensusOn =
			(getenv("MC2_BUCKET_CENSUS") != NULL);
		uint32_t legacyEligible = 0;
		if (s_bucketCensusOn) {
			for (long ci = 0; ci < nextAvailableVertexNode; ++ci) {
				const DWORD cf = masterVertexNodes[ci].flags;
				if ((cf & MC2_DRAWSOLID) && (cf & MC2_ISTERRAIN) &&
				    masterVertexNodes[ci].vertices &&
				    masterVertexNodes[ci].currentVertex !=
				        masterVertexNodes[ci].vertices)
				{
					++legacyEligible;
				}
			}
		}

		bool modernHandled = false;
		if (gos_terrain_indirect::IsFrameSolidArmed()) {
			// Indirect SOLID owns this frame. The SOLID gate-off in setupTextures()
			// already fired, so TerrainPatchStream has no SOLID records — do NOT fall
			// back to flush() when DrawIndirect returns false (plan v2 advisor
			// stop-the-line #1). A false return is a hard failure: logged, arming
			// disabled process-wide; operator advice in event=hard_failure line.
			modernHandled = gos_terrain_indirect::DrawIndirect();
		} else if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
			// Un-armed frame: gate-off did not fire, legacy admits filled
			// TerrainPatchStream normally. M2 thin-record-direct draw runs SOLID.
			modernHandled = TerrainPatchStream::flush();
		}

		// [TERRAIN_SURFACE] PR-2 (Wave 1, ADDITIVE / DEFAULT-OFF / DELETES
		// NOTHING). Screen-agnostic continuous-surface VALIDATION draw: runs
		// on EVERY frame regardless of arming (design Convergence C-1 --
		// surface existence is decoupled from IsFrameSolidArmed). A no-op
		// unless MC2_TERRAIN_SURFACE is set (gos_terrain_surface::IsEnabled,
		// checked inside the bridge), so the default path is byte-for-byte
		// behaviour-neutral. When ON, the surface draws ON TOP of the still-
		// running legacy/indirect terrain above for visual validation of the
		// V-ssbo VS + Fork D clip-space pre-divide reverse-Z bias. NO legacy
		// kill site lands here -- the substitutive draw-kill is PR-4.
		gos_terrain_surface_bridge_draw();

		bool bSkip_DRAWSOLID = false;
		for (long i=0;i<nextAvailableVertexNode && !bSkip_DRAWSOLID;i++)
		{
				if ((masterVertexNodes[i].flags & MC2_DRAWSOLID) &&
					(masterVertexNodes[i].vertices))
				{
					if (modernHandled && (masterVertexNodes[i].flags & MC2_ISTERRAIN)) {
						masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
						continue;
					}

					if (masterVertexNodes[i].flags & MC2_ISTERRAIN) {
						gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
						gos_SetRenderState( gos_State_Terrain, 1 );
					} else {
						gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
						gos_SetRenderState( gos_State_Terrain, 0 );
					}

					DWORD totalVertices = masterVertexNodes[i].numVertices;
					if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
					{
						totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
					}

					// Set per-node terrain extras for tessellation VBO alignment
					if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) && masterVertexNodes[i].extras) {
						int extraCount = masterVertexNodes[i].currentExtra
							? (int)(masterVertexNodes[i].currentExtra - masterVertexNodes[i].extras)
							: 0;
						gos_SetTerrainBatchExtras(masterVertexNodes[i].extras, extraCount);
					} else {
						gos_SetTerrainBatchExtras(NULL, 0);
					}

					if (totalVertices && (totalVertices < MAX_SENDDOWN))
					{
						gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));
						gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
					}
					else if (totalVertices > MAX_SENDDOWN)
					{
						gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));

						//Must divide up vertices into batches of 10,000 each to send down.
						// Somewhere around 20000 to 30000 it really gets screwy!!!
						long currentVertices = 0;
						while (currentVertices < totalVertices)
						{
							gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
							long tVertices = totalVertices - currentVertices;
							if (tVertices > MAX_SENDDOWN)
								tVertices = MAX_SENDDOWN;

							gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

							currentVertices += tVertices;
						}
					}
					//Reset the list to zero length to avoid drawing more then once!
					//Also comes in handy if gameLogic is not called.
					masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
				}
			}
		// Emit one [BUCKET_CENSUS v1] line per frame (env-gated). Runs
		// after flush() has populated the modern-side stats and after
		// either branch has reset the legacy ring; legacy_eligible was
		// captured pre-flush above. emitCensus() is a no-op when the
		// env var is unset.
		if (s_bucketCensusOn) {
			TerrainPatchStream::emitCensus(legacyEligible);
		}
	}   // end Render.TerrainSolid zone
	drainGLErrors("terrain");

	// Task 10 flush() — moved here from after Render.3DObjects because
	// terrain renders AFTER 3D objects in this codebase; placing our
	// flush earlier meant terrain overwrote our pixels. Running after
	// terrain but before overlays gives buildings the right layering:
	// on top of terrain, below decals/roads.
	{
		ZoneScopedN("Render.GpuStaticProps");
		TracyGpuZone("Render.GpuStaticProps");
		extern bool g_useGpuStaticProps;
		extern bool g_useGpuObjects;
		if (g_useGpuStaticProps || g_useGpuObjects) {
			// Stage 3.C: inject static-registry instances into batcher buckets
			// BEFORE flush(), so they're drawn in the same combined GPU pass.
			// C1b GPU authority flip: registry flush ALSO appends static prop
			// substrate records (category = Cat_StaticProp | typeID<<4) so the
			// cull shader can scatter them into the correct per-type bucket.
			GpuStaticPropRegistry::flush();

			// Step 4.6 (global-pool slice 1): compute per-cmd baseInstance prefix-sum
			// and advance the coalesce ring slot BEFORE compute_dispatch() so the
			// patch shader can read baseInstanceByCmd[] in the same dispatch.
			batcher_prepareBaseInstanceTable();

			// C1b GPU authority flip: compute_dispatch() is now called HERE
			// (moved from mission.cpp) so it processes BOTH dynamic actor records
			// (from substrate_flushUpload in objmgr::update) AND the static prop
			// records appended by GpuStaticPropRegistry::flush() above.
			// The patch shader then writes GPU-computed instanceCounts into the
			// indirect command buffer. GpuStaticPropBatcher::flush() below uses
			// glMultiDrawElementsIndirect which reads those GPU-authoritative counts.
#if defined(MC2_SUBSTRATE_COUNT_PARITY)
				gpu_cull::substrate_countParityCheck();
#endif
			if (gpu_cull::compute_isEnabled()) {
				gpu_cull::compute_dispatch();
			}

			GpuStaticPropBatcher::instance().flush();
		}

		// GPU mech batcher Slice A flush — runs after static-prop flush,
		// inside renderLists() so terrain has already been emitted by the
		// patch stream and the depth state is set up. Independent of
		// g_useGpuStaticProps; gated on its own MC2_GPU_MECHS env var
		// inside the flush itself.
		{
			ZoneScopedN("Render.GpuMechs");
			TracyGpuZone("Render.GpuMechs");
			GpuMechBatcher::instance().flush();
		}
	}

	// DRAWSOLID done

	// B4 Slice Stage 1b — mask-SOLID dual-run dispatch.
	// Draws the same SOLID quads as the legacy drawPass (which is still active
	// in Stage 1b — both run; parity comparator validates the masks match).
	// Default-off: IsFrameMaskSolidArmed() returns false unless
	// MC2_TERRAIN_MASK_DISPATCH=1 AND MC2_TERRAIN_MASK_DISPATCH_SOLID != "0".
	{
		ZoneScopedN("Render.TerrainMask.Solid");
		TracyGpuZone("Render.TerrainMask.Solid");
		if (gos_terrain_mask_dispatch::IsMaskDispatchReady()
		 && gos_terrain_mask_dispatch::IsFrameMaskSolidArmed()) {
			gos_terrain_mask_dispatch::DrawMaskSolid();
		}
	}

	// ── New world-space overlay batches ──────────────────────────────────────
	// These draw calls flush batches accumulated during land->render() and
	// craterManager->render().  They set their own GL state and restore it.
	// Render order matches design: terrain → cement overlays → decals → (old overlays).
	{
		ZoneScopedN("Render.TerrainOverlays");
		TracyGpuZone("Render.TerrainOverlays");
		gos_DrawTerrainOverlays();
	}
	// Slice A — cement-overlay static-bake draw. Mirrors the Render.Terrain
	// Mines hook below EXACTLY: gated on IsFrameOverlayArmed() (default OFF
	// unless MC2_TERRAIN_INDIRECT_OVERLAY=1). When armed, the per-quad M2d
	// gos_PushTerrainOverlay producer is skipped (quad.cpp gate-off) and
	// gos_DrawTerrainOverlays above flushes an empty batch (early-return);
	// DrawDecalStatic draws the persistent static bake instead. Placed right
	// after Render.TerrainOverlays so the static cement composites in the
	// same slot the per-frame batch used (before mines/decals/old overlays).
	{
		ZoneScopedN("Render.TerrainOverlaysStatic");
		TracyGpuZone("Render.TerrainOverlaysStatic");
		if (gos_terrain_indirect::IsFrameOverlayArmed()) {
			gos_terrain_indirect::DrawDecalStatic();
		}
	}
	// PR2c Stage 2c — mine static-bake draw. Hooks between TerrainOverlays
	// and Decals so mines composite ABOVE cement/road overlays and BENEATH
	// crater decals (state=2 blown-mine sprites coexist with crater decals).
	// Default-off: IsFrameMineArmed() returns false unless
	// MC2_TERRAIN_INDIRECT_MINE=1 AND the texture-array has been built.
	{
		ZoneScopedN("Render.TerrainMines");
		TracyGpuZone("Render.TerrainMines");
		if (gos_terrain_indirect::IsFrameMineArmed()) {
			gos_terrain_indirect::DrawMineStatic();
		}
	}
	{
		ZoneScopedN("Render.Decals");
		TracyGpuZone("Render.Decals");
		gos_DrawDecals();
	}
	// ── End new world-space overlay batches ───────────────────────────────────

	{
		ZoneScopedN("Render.Overlays");
		TracyGpuZone("Render.Overlays");
	if (Environment.Renderer == 3)
	{
		//Do NOT draw the water as transparent in software
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	}
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	}
	
    // sebi: split in 2 parts, first draw objects which have alpha test off, then with alpha test on
    for(int states = 0; states < 2; ++states) 
    {   
        gos_SetRenderState( gos_State_AlphaTest, states);

        for (int i=0;i<nextAvailableVertexNode;i++)
        {
            if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    (masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (masterVertexNodes[i].vertices))
            {
                // The legacy non-water terrain alpha/detail layer sits on the original
                // flat terrain plane and shows through displaced tess terrain as the
                // dark striped under-pattern. Keep water passes, but drop this layer.
                if (!(masterVertexNodes[i].flags & MC2_ISWATER) &&
                    !(masterVertexNodes[i].flags & MC2_ISWATERDETAIL)) {
                    masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
                    continue;
                }

                {
                    int waterMode = 0;
                    if (masterVertexNodes[i].flags & MC2_ISWATER) waterMode = 1;
                    else if (masterVertexNodes[i].flags & MC2_ISWATERDETAIL) waterMode = 2;
                    gos_SetRenderState(gos_State_Water, waterMode);
                }

                DWORD totalVertices = masterVertexNodes[i].numVertices;
                if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
                {
                    totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
                }

                if (totalVertices && (totalVertices < MAX_SENDDOWN))
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
                    gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
                }
                else if (totalVertices > MAX_SENDDOWN)
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());

                    //Must divide up vertices into batches of 10,000 each to send down.
                    // Somewhere around 20000 to 30000 it really gets screwy!!!
                    long currentVertices = 0;
                    while (currentVertices < totalVertices)
                    {
                        gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
                        long tVertices = totalVertices - currentVertices;
                        if (tVertices > MAX_SENDDOWN)
                            tVertices = MAX_SENDDOWN;

                        gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

                        currentVertices += tVertices;
                    }
                }

                //Reset the list to zero length to avoid drawing more then once!
                //Also comes in handy if gameLogic is not called.
                masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
            }
        }
    }
    //reset alpha test at the end
    gos_SetRenderState( gos_State_AlphaTest, 0);


	//<< sebi: added this section to draw objects which do not have terrain underlayer (those are added in quad.cpp, see (*) there )
	{ ZoneScopedN("Render.NoUnderlayer");
	  TracyGpuZone("Render.NoUnderlayer");
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
			!(masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
			(masterVertexNodes[i].flags & MC2_GPUOVERLAY) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
	
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState( gos_State_ZCompare, 0);
			gos_SetRenderState(gos_State_Overlay, 1);
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
			gos_SetRenderState(gos_State_Overlay, 0);
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
			
			//Reset the list to zero length to avoid drawing more then once!			
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	} // end ZoneScopedN("Render.NoUnderlayer")
	//<< sebi: end of added block

	// Cement overlays (MC2_ISCRATERS|MC2_ISTERRAIN) and decals (!MC2_ISTERRAIN|MC2_ISCRATERS)
	// are now drawn by gos_DrawTerrainOverlays() and gos_DrawDecals() before Render.Overlays.
	// The old Render.CraterOverlays and non-terrain crater loops are removed.

	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState(	gos_State_ZWrite, 1);
		gos_SetRenderState( gos_State_ZCompare, 2);
	}
	else
	{
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Specular, 1);
		// sebi: shadows do not draw in depth, we do not want z-fighting
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_ZCompare, 2);
	}

	//NEVER draw shadows in Software.
	if (Environment.Renderer != 3)
	{
		for (int i=0;i<nextAvailableVertexNode;i++)
		{
			 if	((masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
				(masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
				(masterVertexNodes[i].vertices))
			{
				DWORD totalVertices = masterVertexNodes[i].numVertices;
				if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
				{
					totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
				}
			
				if (totalVertices && (totalVertices < MAX_SENDDOWN))
				{
					gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
					gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
				}
				else if (totalVertices > MAX_SENDDOWN)
				{
					gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
					
					//Must divide up vertices into batches of 10,000 each to send down.
					// Somewhere around 20000 to 30000 it really gets screwy!!!
					long currentVertices = 0;
					while (currentVertices < totalVertices)
					{
						gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
						long tVertices = totalVertices - currentVertices;
						if (tVertices > MAX_SENDDOWN)
							tVertices = MAX_SENDDOWN;
						
						gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
						
						currentVertices += tVertices;
					}
				}
				
				//Reset the list to zero length to avoid drawing more then once!
				//Also comes in handy if gameLogic is not called.
				masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
			}
		}
	}


	gos_SetRenderState( gos_State_ZCompare, 1);
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
	
    // sebi: split in 2 parts, first draw objects which have alpha test off, then with alpha test on
    for(int states = 0; states < 2; ++states) 
    {   
        gos_SetRenderState( gos_State_AlphaTest, states);
        for (int i=0;i<nextAvailableVertexNode;i++)
        {
            if (!(masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    !(masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
                    !(masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
                    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    (masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (masterVertexNodes[i].vertices))
            {
                DWORD totalVertices = masterVertexNodes[i].numVertices;
                if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
                {
                    totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
                }

                if (totalVertices && (totalVertices < MAX_SENDDOWN))
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
                    gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
                }
                else if (totalVertices > MAX_SENDDOWN)
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());

                    //Must divide up vertices into batches of 10,000 each to send down.
                    // Somewhere around 20000 to 30000 it really gets screwy!!!
                    long currentVertices = 0;
                    while (currentVertices < totalVertices)
                    {
                        gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
                        long tVertices = totalVertices - currentVertices;
                        if (tVertices > MAX_SENDDOWN)
                            tVertices = MAX_SENDDOWN;

                        gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

                        currentVertices += tVertices;
                    }
                }

                //Reset the list to zero length to avoid drawing more then once!
                //Also comes in handy if gameLogic is not called.
                masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
            }
        }
    }
    //reset alpha test at the end
    gos_SetRenderState( gos_State_AlphaTest, 0);

	
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState( gos_State_Fog, 0);
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneOne);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_MonoEnable, 1);
	}
	else
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState( gos_State_Fog, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneOne);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_MonoEnable, 0);
	}
				
	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISEFFECTS) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	gos_SetRenderState(	gos_State_ZWrite, 1);
	
	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISSPOTLGT) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	gos_SetRenderState( gos_State_ZWrite, 0);
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState( gos_State_Perspective, 1);
 	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_AlphaTest, 1);
	
 	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	gos_SetRenderState( gos_State_Filter, gos_FilterNone);
	
 	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISHUDLMNT) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}

	//Must turn zCompare back on for FXs
	gos_SetRenderState( gos_State_ZCompare, 1 );

	// Reset terrain extra buffer after rendering — will be re-filled during next frame's TerrainQuad::draw() calls
	gos_TerrainExtraReset();
	} // end Render.Overlays zone
}

//----------------------------------------------------------------------
// Registry-driven texture pinning. See
// docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
//
// pinNode    asserts the slot is in-range AND has a live texture allocation
//            (numUsers > 0). Pinning a free slot is a bug — the texture might
//            be reallocated to a different consumer before unpinNode runs.
// unpinNode  asserts pinRefCount > 0 before decrement to catch
//            unpaired-release / double-release.
// getPinCount is non-mutating; usable from logging paths.
void MC_TextureManager::pinNode (DWORD nodeIdx)
{
	gosASSERT(nodeIdx < (DWORD)MC_MAXTEXTURES);
	gosASSERT(masterTextureNodes[nodeIdx].numUsers > 0);
	masterTextureNodes[nodeIdx].pinRefCount++;
}

void MC_TextureManager::unpinNode (DWORD nodeIdx)
{
	gosASSERT(nodeIdx < (DWORD)MC_MAXTEXTURES);
	gosASSERT(masterTextureNodes[nodeIdx].pinRefCount > 0);
	masterTextureNodes[nodeIdx].pinRefCount--;
}

DWORD MC_TextureManager::getPinCount (DWORD nodeIdx) const
{
	if (nodeIdx >= (DWORD)MC_MAXTEXTURES) return 0;
	return masterTextureNodes[nodeIdx].pinRefCount;
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::update (void)
{
	ZoneScopedN("MC_TextureManager::update");
	DWORD numTexturesFreed = 0;
	currentUsedTextures = 0;
	
	{
		ZoneScopedN("MC_TextureManager::update scanNodes");
		for (long i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff))
		{
			if (!masterTextureNodes[i].uniqueInstance &&
				!(masterTextureNodes[i].neverFLUSH & 1))		//Only uncachable if BIT 1 is set, otherwise, cache 'em out!
			{
				if (masterTextureNodes[i].lastUsed <= (turn-60))
				{
					if (masterTextureNodes[i].pinRefCount > 0) {
						EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "update_turn60");
					} else {
						//----------------------------------------------------------------
						// Cache this badboy out.  Textures don't change.  Just Destroy!
						{
							ZoneScopedN("MC_TextureManager::update cacheOut");
							if (masterTextureNodes[i].gosTextureHandle)
								gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle);
						}

						TEX_LC("event=evict nodeIdx=%ld turn=%ld lastUsed=%ld gosHandle=0x%08x",
						       i, turn, masterTextureNodes[i].lastUsed,
						       masterTextureNodes[i].gosTextureHandle);

						masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
						numTexturesFreed++;
					}
				}
			}

			//Count ACTUAL number of textures being used.
			// ALSO can't count on turn being right.  Logistics does not update unless simple Camera is up!!
			if (masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE)
			{
				currentUsedTextures++;
				if (currentUsedTextures > peakUsedTextures) peakUsedTextures = currentUsedTextures;
			}
		}
		}
	}

	if (s_texLifecycleTrace) {
		TEX_LC("event=update_summary turn=%ld evicted=%lu currentUsed=%ld",
		       turn, (unsigned long)numTexturesFreed, (long)currentUsedTextures);
	}

	return numTexturesFreed;
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::textureFromMemory (DWORD *data, gos_TextureFormat key, DWORD hints, DWORD width, DWORD bitDepth)
{
	ZoneScopedN("MC_TextureManager::textureFromMemory");
	long i=0;

	//--------------------------------------------------------
	// If we called this, we KNOW the texture is NOT loaded!
	//
	// Find first empty NODE
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory findSlot");
		for (i=0;i<MC_MAXTEXTURES;i++)
		{
			if (masterTextureNodes[i].gosTextureHandle == 0xffffffff)
			{
				break;
			}
		}
	}

	if (i == MC_MAXTEXTURES)
		STOP(("TOO Many textures in game.  We have exceeded 4096 game handles"));
		
	//--------------------------------------------------------
	// New Method.  Just store memory footprint of texture.
	// DO NOT create GOS handle until we need it.
 	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	masterTextureNodes[i].nodeName = NULL;

	masterTextureNodes[i].numUsers = 1;
	masterTextureNodes[i].key = key;
	masterTextureNodes[i].hints = hints;
	masterTextureNodes[i].logicalWidth = width;
	masterTextureNodes[i].logicalHeight = width;

	//------------------------------------------
	// Find and store the width.
	masterTextureNodes[i].width = width;
	long txmSize = width * width * bitDepth;
	
	if (!lzBuffer1)
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory lzBuffers");
		lzBuffer1 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer1 != NULL);
		
		lzBuffer2 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer2 != NULL);
	}
	
	actualTextureSize += txmSize;
	DWORD txmCompressSize;
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory LZCompress");
		txmCompressSize = LZCompress(lzBuffer2,(MemoryPtr)data,txmSize);
	}
	compressedTextureSize += txmCompressSize;
	
 	//-------------------------------------------------------
	// Create a block of cache memory to hold this texture.
	if (!masterTextureNodes[i].textureData )
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory cacheAlloc");
		masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(txmCompressSize);
	}
	
	//No More RAM.  Do not display this texture anymore.
	if (masterTextureNodes[i].textureData == NULL)
		masterTextureNodes[i].gosTextureHandle = 0;
	else
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory cacheCopy");
		memcpy(masterTextureNodes[i].textureData,lzBuffer2,txmCompressSize);
		masterTextureNodes[i].lzCompSize = txmCompressSize;
	}
	
	//------------------	
	return(i);
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::textureInstanceExists (const char *textureFullPathName, gos_TextureFormat key, DWORD hints, DWORD uniqueInstance, DWORD nFlush)
{
	long i=0;

	//--------------------------------------
	// Is this texture already Loaded?
	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].nodeName)
		{
			if (S_stricmp(masterTextureNodes[i].nodeName,textureFullPathName) == 0)
			{
				if (uniqueInstance == masterTextureNodes[i].uniqueInstance)
				{
					masterTextureNodes[i].numUsers++;
					return(i);							//Return the texture Node Id Now.
				}
				else
				{
					//------------------------------------------------
					// Copy the texture from old Handle to a new one.
					// Return the NEW handle.
					//
					// There should be no code here!!!
				}
			}
		}
	}
	return 0;
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::loadTexture (const char *textureFullPathName, gos_TextureFormat key, DWORD hints, DWORD uniqueInstance, DWORD nFlush)
{
	ZoneScopedN("MC_TextureManager::loadTexture");
	long i=0;

	//--------------------------------------
	// Is this texture already Loaded?
	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].nodeName && (S_stricmp(masterTextureNodes[i].nodeName,textureFullPathName) == 0))
		{
			if (uniqueInstance == masterTextureNodes[i].uniqueInstance)
			{
				masterTextureNodes[i].numUsers++;
				return(i);							//Return the texture Node Id Now.
			}
			else
			{
				//------------------------------------------------
				// Copy the texture from old Handle to a new one.
				// Return the NEW handle.
				//
				// There should be no code here!!!
			}
		}
	}

	//--------------------------------------------------
	// If we get here, texture has not been loaded yet.
	// Load it now!
	//
	// Find first empty NODE
	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].gosTextureHandle == 0xffffffff)
		{
			break;
		}
	}

	if (i == MC_MAXTEXTURES)
		STOP(("TOO Many textures in game.  We have exceeded 4096 game handles"));
		
	if (key == gos_Texture_Alpha && Environment.Renderer == 3)
	{
		key = gos_Texture_Keyed;
	}

 	//--------------------------------------------------------
	// New Method.  Just store memory footprint of texture.
	// DO NOT create GOS handle until we need it.
 	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	masterTextureNodes[i].nodeName = (char *)textureStringHeap->Malloc(strlen(textureFullPathName) + 1);
	gosASSERT(masterTextureNodes[i].nodeName != NULL);

	strcpy(masterTextureNodes[i].nodeName,textureFullPathName);
	masterTextureNodes[i].numUsers = 1;
	masterTextureNodes[i].key = key;
	masterTextureNodes[i].hints = hints;
	masterTextureNodes[i].uniqueInstance = uniqueInstance;
	masterTextureNodes[i].neverFLUSH = nFlush;
	masterTextureNodes[i].logicalWidth = 0;
	masterTextureNodes[i].logicalHeight = 0;

	//----------------------------------------------------------------------------------------------
	// Store a cache-format marker and fileSize in width so that cache knows to create new texture from memory.
	// This way, we never need to know anything about the texture AND we can store PMGs
	// in memory instead of TGAs which use WAY less RAM!
	File textureFile;
#ifdef _DEBUG
	long textureFileOpenResult = 
#endif
		textureFile.open(textureFullPathName);
	gosASSERT(textureFileOpenResult == NO_ERR);

	if (textureFile.isLoadedFromDisk())
		masterTextureNodes[i].uvScale = 4;

	tryReadTgaLogicalSize(textureFile, masterTextureNodes[i].uvScale,
		masterTextureNodes[i].logicalWidth, masterTextureNodes[i].logicalHeight);

	long txmSize = textureFile.fileSize();
	
	if (!lzBuffer1)
	{
		ZoneScopedN("MC_TextureManager::loadTexture lzBuffers");
		lzBuffer1 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer1 != NULL);
		
		lzBuffer2 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer2 != NULL);
	}

	//Try reading the RAW data out of the fastFile.
	// If it succeeds, we just saved a complete compress, decompress and two memcpys!!
	//
	long result;
	{
		ZoneScopedN("MC_TextureManager::loadTexture readRAW");
		result = textureFile.readRAW(masterTextureNodes[i].textureData,textureCacheHeap);
	}
	if (!result)
	{
		gosASSERT(txmSize <= MAX_LZ_BUFFER_SIZE);
		{
			ZoneScopedN("MC_TextureManager::loadTexture fileRead");
			textureFile.read(lzBuffer1,txmSize);
		}

		textureFile.close();

		actualTextureSize += txmSize;
		const bool storeRawFileData = (txmSize >= MC_TEXCACHE_RAW_THRESHOLD);
		const DWORD cacheBytes = storeRawFileData ? txmSize : [&]() -> DWORD {
			DWORD txmCompressSize;
			{
				ZoneScopedN("MC_TextureManager::loadTexture LZCompress");
				txmCompressSize = LZCompress(lzBuffer2,lzBuffer1,txmSize);
			}
			compressedTextureSize += txmCompressSize;
			return txmCompressSize;
		}();

		{
			ZoneScopedN("MC_TextureManager::loadTexture cacheAlloc");
			masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(cacheBytes);
		}
		if (masterTextureNodes[i].textureData == NULL)
			masterTextureNodes[i].gosTextureHandle = 0;
		else
		{
			ZoneScopedN("MC_TextureManager::loadTexture cacheCopy");
			memcpy(masterTextureNodes[i].textureData, storeRawFileData ? lzBuffer1 : lzBuffer2, cacheBytes);
			}

		masterTextureNodes[i].lzCompSize = cacheBytes;
		masterTextureNodes[i].width = (storeRawFileData ? MC_TEXCACHE_FILE_RAW : MC_TEXCACHE_FILE_LZ) + txmSize;
	}
	else
	{
		masterTextureNodes[i].lzCompSize = result;
		masterTextureNodes[i].width = MC_TEXCACHE_FILE_LZ + txmSize;
	}

 	//-------------------
	return(i);
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::textureFromMemoryRaw (DWORD *data, gos_TextureFormat key, DWORD hints, DWORD width, DWORD bitDepth)
{
	ZoneScopedN("MC_TextureManager::textureFromMemoryRaw");
	long i=0;

	{
		ZoneScopedN("MC_TextureManager::textureFromMemoryRaw findSlot");
		for (i=0;i<MC_MAXTEXTURES;i++)
		{
			if (masterTextureNodes[i].gosTextureHandle == 0xffffffff)
			{
				break;
			}
		}
	}

	if (i == MC_MAXTEXTURES)
		STOP(("TOO Many textures in game.  We have exceeded 4096 game handles"));

	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	masterTextureNodes[i].nodeName = NULL;
	masterTextureNodes[i].numUsers = 1;
	masterTextureNodes[i].key = key;
	masterTextureNodes[i].hints = hints;
	masterTextureNodes[i].logicalWidth = width;
	masterTextureNodes[i].logicalHeight = width;

	const DWORD txmSize = width * width * bitDepth;
	masterTextureNodes[i].width = MC_TEXCACHE_MEM_RAW + txmSize;
	masterTextureNodes[i].lzCompSize = txmSize;
	actualTextureSize += txmSize;
	compressedTextureSize += txmSize;

	{
		ZoneScopedN("MC_TextureManager::textureFromMemoryRaw cacheAlloc");
		masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(txmSize);
	}

	if (masterTextureNodes[i].textureData == NULL)
		masterTextureNodes[i].gosTextureHandle = 0;
	else
	{
		ZoneScopedN("MC_TextureManager::textureFromMemoryRaw cacheCopy");
		memcpy(masterTextureNodes[i].textureData, data, txmSize);
	}

	return(i);
}

//----------------------------------------------------------------------
long MC_TextureManager::saveTexture (DWORD textureIndex, const char *textureFullPathName)
{
	if ((MC_MAXTEXTURES <= textureIndex) || (NULL == masterTextureNodes[textureIndex].textureData))
	{
		return (~NO_ERR);
	}
	File textureFile;
	long textureFileOpenResult = textureFile.create(textureFullPathName);
	if (NO_ERR != textureFileOpenResult)
	{
		textureFile.close();
		return textureFileOpenResult;
	}

	{
		if (masterTextureNodes[textureIndex].width == 0)
		{
			textureFile.close();
			return (~NO_ERR);		//These faces have no texture!!
		}

		{
			//------------------------------------------
			const DWORD cacheFormat = masterTextureNodes[textureIndex].width & 0xF0000000;
			const DWORD originalSize = masterTextureNodes[textureIndex].width & MC_TEXCACHE_SIZE_MASK;
			if (cacheFormat == MC_TEXCACHE_FILE_RAW || cacheFormat == MC_TEXCACHE_MEM_RAW)
			{
				textureFile.write((MemoryPtr)masterTextureNodes[textureIndex].textureData, originalSize);
			}
			else
			{
				// Badboys are now LZ Compressed in texture cache.
				long origSize = LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)masterTextureNodes[textureIndex].textureData,masterTextureNodes[textureIndex].lzCompSize,MAX_LZ_BUFFER_SIZE);
				if (origSize != originalSize)
					STOP(("Decompressed to different size from original!  Txm:%s  Width:%d  DecompSize:%d",masterTextureNodes[textureIndex].nodeName,originalSize,origSize));

				if (origSize >= MAX_LZ_BUFFER_SIZE)
					STOP(("Texture TOO large: %s",masterTextureNodes[textureIndex].nodeName));

				textureFile.write(MC_TextureManager::lzBuffer2, origSize);
			}
		}
		textureFile.close();
	}

	return NO_ERR;
}

DWORD MC_TextureManager::copyTexture( DWORD texNodeID )
{
	gosASSERT( texNodeID < MC_MAXTEXTURES );
	if ( masterTextureNodes[texNodeID].gosTextureHandle != -1 )
	{
		masterTextureNodes[texNodeID].numUsers++;
		return texNodeID;
	}
	else
	{
		STOP(( "tried to copy an invalid texture" ));
	}

	return -1;

}
//----------------------------------------------------------------------
// MC_TextureNode
DWORD MC_TextureNode::get_gosTextureHandle (void)	//If texture is not in VidRAM, cache a texture out and cache this one in.
{
	// PERF 2026-05-07: stripped MC_TextureNode::get_gosTextureHandle Tracy
	// scopes/plots from this hot accessor; cache-miss work remains unchanged.
	if (gosTextureHandle == 0xffffffff)
	{
		//Somehow this texture is bad.  Probably we are using a handle which got purged between missions.
		// Just send back, NO TEXTURE and we should be able to debug from there because the tri will have no texture!!
		PAUSE(("txmmgr: Bad texture handle!"));
		return 0x0;
	}
	
	if (gosTextureHandle != CACHED_OUT_HANDLE)
	{
		lastUsed = turn;
		return gosTextureHandle;
	}
	else
	{
		if ((mcTextureManager->currentUsedTextures >= MAX_MC2_GOS_TEXTURES) && !mcTextureManager->flushCache())
		{
			PAUSE(("txmmgr: Out of texture handles!"));
			return 0x0;		//No texture!
		}
	   
		if (width == 0)
		{
			{
				char _cbbuf[256];
				snprintf(_cbbuf, sizeof(_cbbuf),
					"[TXM] zero-width texture node: nodeName=%s handle=%u lzCompSize=%u",
					nodeName ? nodeName : "<null>",
					(unsigned)gosTextureHandle, (unsigned)lzCompSize);
				puts(_cbbuf); fflush(stdout); crashbundle_append(_cbbuf);
			}
			PAUSE(("txmmgr: Textur has zero width!"));
			return 0;		//These faces have no texture!!
		}

		if (!textureData)
		{
			PAUSE(("txmmgr: Cache is out of RAM!"));
			return 0x0;		//No Texture.  Cache is out of RAM!!
		}

		const DWORD cacheFormat = width & 0xF0000000;
		if ((cacheFormat == MC_TEXCACHE_FILE_LZ) || (cacheFormat == MC_TEXCACHE_FILE_RAW) || (cacheFormat == MC_TEXCACHE_MEM_RAW))
		{
			const DWORD originalSize = width & MC_TEXCACHE_SIZE_MASK;
			BYTE* textureBytes = (BYTE*)textureData;
			{
				if (cacheFormat == MC_TEXCACHE_FILE_LZ)
				{
					//------------------------------------------
					// Cache this badboy IN.
					// Badboys are now LZ Compressed in texture cache.
					long origSize;
					{
						origSize = LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)textureData,lzCompSize,MAX_LZ_BUFFER_SIZE);
					}
					if (origSize != originalSize)
						STOP(("Decompressed to different size from original!  Txm:%s  Width:%d  DecompSize:%d",nodeName,originalSize,origSize));

					if (origSize >= MAX_LZ_BUFFER_SIZE)
						STOP(("Texture TOO large: %s",nodeName));
					textureBytes = (BYTE*)MC_TextureManager::lzBuffer2;
				}
				else if (cacheFormat == MC_TEXCACHE_MEM_RAW)
				{
					textureBytes = (BYTE*)textureData;
				}
			}

			if (cacheFormat == MC_TEXCACHE_MEM_RAW)
			{
				{
					gosTextureHandle = gos_NewEmptyTexture(key,nodeName,logicalWidth ? logicalWidth : width,hints);
				}
				TEXTUREPTR pTextureData;
				{
					gos_LockTexture(gosTextureHandle, 0, 0, &pTextureData);
				}
				{
					memcpy(pTextureData.pTexture, textureBytes, originalSize);
				}
				{
					gos_UnLockTexture(gosTextureHandle);
				}
			}
			else
			{
				{
					gosTextureHandle = gos_NewTextureFromMemory(key,nodeName,textureBytes,originalSize,hints);
				}
			}
			mcTextureManager->currentUsedTextures++;
			if (mcTextureManager->currentUsedTextures > mcTextureManager->peakUsedTextures)
				mcTextureManager->peakUsedTextures = mcTextureManager->currentUsedTextures;
			++gTxmRealizedTotal;
			lastUsed = turn;

			return gosTextureHandle;
		}
		else
		{
			{
				gosTextureHandle = gos_NewEmptyTexture(key,nodeName,width,hints);
			}
			mcTextureManager->currentUsedTextures++;
			if (mcTextureManager->currentUsedTextures > mcTextureManager->peakUsedTextures)
				mcTextureManager->peakUsedTextures = mcTextureManager->currentUsedTextures;
			++gTxmRealizedTotal;

			//------------------------------------------
			// Cache this badboy IN.
			TEXTUREPTR pTextureData;
			{
				gos_LockTexture(gosTextureHandle, 0, 0, &pTextureData);
			}
		 
			//-------------------------------------------------------
			// Create a block of cache memory to hold this texture.
			DWORD txmSize = pTextureData.Height * pTextureData.Height * sizeof(DWORD);
			gosASSERT(textureData);

			{
				LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)textureData,lzCompSize,MAX_LZ_BUFFER_SIZE);
			}
			{
				memcpy(pTextureData.pTexture,MC_TextureManager::lzBuffer2,txmSize);
			}

			//------------------------
			// Unlock the texture
			{
				gos_UnLockTexture(gosTextureHandle);
			}
			 
			lastUsed = turn;
			return gosTextureHandle;
		}
	}
}

//----------------------------------------------------------------------
void MC_TextureNode::destroy (void)
{
	if ((gosTextureHandle != CACHED_OUT_HANDLE) && (gosTextureHandle != 0xffffffff) && (gosTextureHandle != 0x0))
	{
		gos_DestroyTexture(gosTextureHandle);
	}
	
	mcTextureManager->textureStringHeap->Free(nodeName);
	mcTextureManager->textureCacheHeap->Free(textureData);
	init();
}

//----------------------------------------------------------------------
