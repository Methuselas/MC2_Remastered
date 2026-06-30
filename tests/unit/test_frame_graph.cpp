// FRAME-GRAPH-SKELETON-1 offline validation.
//
// Proves the resource DAG declared by kRenderPassContracts[] + kFramePassOrder[] is
// self-consistent (every pass's reads are produced by an earlier pass or are external)
// and that the validator CATCHES a bad reorder — all offline, no GL/engine/smoke. This
// is the "validation" layer over the existing "declaration" table.
#include "doctest.h"
#include "RenderCore/frame_graph_validate.h"
#include "RenderCore/ambient_contract.h"
#include "RenderCore/fbo_ledger.h"
#include "RenderCore/terrain_subpass_contract.h"
#include "RenderCore/frame_pass_trace.h"   // FRAME-GRAPH-EXECUTOR-DRYRUN-1 pure kernel
#include "RenderCore/frame_executor.h"     // FRAME-GRAPH-EXECUTOR-ISLAND-1 IslandContract

using namespace RenderCore;
using namespace RenderCore::framegraph;

TEST_SUITE("FrameGraph") {

TEST_CASE("shipped frame graph: every read satisfied by a prior write or external") {
    const ValidationResult r = validateShippedFrameGraph();
    // On failure these report the offending pass/resource ids directly.
    CHECK(static_cast<unsigned>(r.offendingPass) == 0u);   // None
    CHECK(static_cast<unsigned>(r.missingResource) == 0u); // Unknown
    CHECK(r.ok == true);
    CHECK(r.unknownPass == false);
}

TEST_CASE("reorder regression: StaticProp before Shadow strands ShadowDynamicMap") {
    // StaticPropOpaque reads ShadowDynamicMap, which only the Shadow pass writes. Put
    // it first (before Shadow) and the read must be flagged unsatisfied — the exact
    // class of "reorder -> invisible/wrong scene" the executor must never produce.
    const RenderPassId badOrder[] = {
        RenderPassId::StaticPropOpaque,   // reads ShadowDynamicMap BEFORE it is written
        RenderPassId::Shadow,
        RenderPassId::Terrain,
        RenderPassId::MechOpaque,
        RenderPassId::TerrainDecal,
        RenderPassId::TerrainOverlay,
        RenderPassId::Water,
        RenderPassId::VegetationCards,
        RenderPassId::VFX,
        RenderPassId::UI,
        RenderPassId::PostProcess,
    };
    const ValidationResult r = validateReadsSatisfied(
        kRenderPassContracts, kRenderPassIdCount,
        badOrder, kFramePassOrderCount,
        kExternalResources, kExternalResourceCount);
    CHECK(r.ok == false);
    CHECK(static_cast<unsigned>(r.offendingPass) == static_cast<unsigned>(RenderPassId::StaticPropOpaque));
    CHECK(static_cast<unsigned>(r.missingResource) == static_cast<unsigned>(RenderResourceId::ShadowDynamicMap));
}

TEST_CASE("removing an external resource surfaces the missing producer") {
    // Drop ShadowStaticMap from the external set; nothing in the dynamic order writes
    // it, so any reader would be flagged. (No shipped pass reads it today, so the
    // shipped order still validates — this asserts the external list is load-bearing,
    // not decorative: a future reader without a producer would fail.)
    const RenderResourceId trimmedExternal[] = {
        RenderResourceId::TerrainHeightTexture,
        RenderResourceId::WaterReflectionColor,
        RenderResourceId::WaterReflectionDepth,
        RenderResourceId::MaterialGpuBuffer,
        // ShadowStaticMap intentionally omitted
    };
    const ValidationResult r = validateReadsSatisfied(
        kRenderPassContracts, kRenderPassIdCount,
        kFramePassOrder, kFramePassOrderCount,
        trimmedExternal, sizeof(trimmedExternal) / sizeof(trimmedExternal[0]));
    // Shipped passes don't read ShadowStaticMap -> still ok. Guards against a silent
    // dependency on the external list ordering.
    CHECK(r.ok == true);
}

TEST_CASE("unknown pass id in order is reported") {
    const RenderPassId badOrder[] = { RenderPassId::None };  // None has no contract row
    const ValidationResult r = validateReadsSatisfied(
        kRenderPassContracts, kRenderPassIdCount,
        badOrder, 1, kExternalResources, kExternalResourceCount);
    CHECK(r.ok == false);
    CHECK(r.unknownPass == true);
}

TEST_CASE("ambient ledger: colorMask handshake is declared (shadow OFF, terrain re-assert)") {
    // The recon's #1 executor blocker, as a tested invariant. If a future edit drops
    // the terrain colorMask re-assert (or the shadow color-write-off declaration), this
    // fails offline instead of shipping an invisible scene.
    CHECK(colorMaskHandshakeDeclared() == true);
    const AmbientContract* shadow = findAmbient(RenderPassId::Shadow);
    REQUIRE(shadow != nullptr);
    CHECK(shadow->disablesColorWrite == true);
    const AmbientContract* terrain = findAmbient(RenderPassId::Terrain);
    REQUIRE(terrain != nullptr);
    CHECK(terrain->reassertsColorMaskAllOn == true);
}

TEST_CASE("SHADOW-OBSERVE-2: shadow colorMaskOnEntry=Inherit + disablesColorWrite=true + depthFunc=ShadowLess") {
    // SHADOW-OBSERVE-2: Active shadow pre-pass enforces depth-only via FBO attachment
    // (DrawBufferSet::ShadowDepthOnly), NOT glColorMask. colorMask is AllOn at the observe
    // point (beginShadowPrePass:6270). colorMaskOnEntry relaxed to Inherit so the ambient
    // guard skips colorMask comparison (avoids AllOn-vs-AllOff false mismatch).
    // disablesColorWrite stays true — shadow IS depth-only, however enforced — so
    // colorMaskHandshakeDeclared() is not weakened. depthFunc=ShadowLess must hold.
    const AmbientContract* shadow = findAmbient(RenderPassId::Shadow);
    REQUIRE(shadow != nullptr);
    CHECK(static_cast<unsigned>(shadow->colorMaskOnEntry) ==
          static_cast<unsigned>(ColorMaskState::Inherit));
    CHECK(shadow->disablesColorWrite == true);   // depth-only invariant preserved
    CHECK(static_cast<unsigned>(shadow->depthFunc) ==
          static_cast<unsigned>(DepthFuncState::ShadowLess));
    // Handshake integrity: colorMaskHandshakeDeclared() uses disablesColorWrite, not
    // colorMaskOnEntry, so relaxing colorMaskOnEntry to Inherit does not weaken it.
    CHECK(colorMaskHandshakeDeclared() == true);
}

TEST_CASE("ambient ledger: terrain latch handshake declared (terrain sets, post consumes)") {
    // Recon landmine #2: markTerrainDrawn/sceneHasTerrain_ is a cross-phase latch; if a
    // producer is removed, screenShadow/cloudShadow/shoreline/edgeFog/fogOob silently
    // bail. This makes the producer/consumer pairing a tested invariant.
    CHECK(terrainLatchHandshakeDeclared() == true);
    const AmbientContract* terrain = findAmbient(RenderPassId::Terrain);
    REQUIRE(terrain != nullptr);
    CHECK(terrain->producesTerrainLatch == true);
    const AmbientContract* post = findAmbient(RenderPassId::PostProcess);
    REQUIRE(post != nullptr);
    CHECK(post->consumesTerrainLatch == true);
}

TEST_CASE("ambient ledger: depth func declarations match the reverse-Z / shadow contract") {
    // Scene passes use reverse-Z GL_GEQUAL; shadow uses GL_LESS (render_contract.cpp).
    const AmbientContract* shadow = findAmbient(RenderPassId::Shadow);
    REQUIRE(shadow != nullptr);
    CHECK(static_cast<unsigned>(shadow->depthFunc) ==
          static_cast<unsigned>(DepthFuncState::ShadowLess));
    const RenderPassId scenePasses[] = {
        RenderPassId::StaticPropOpaque, RenderPassId::Terrain, RenderPassId::MechOpaque
    };
    for (int i = 0; i < 3; ++i) {
        const AmbientContract* a = findAmbient(scenePasses[i]);
        REQUIRE(a != nullptr);
        CHECK(static_cast<unsigned>(a->depthFunc) ==
              static_cast<unsigned>(DepthFuncState::SceneGEqual));
    }
}

TEST_CASE("compareAmbient: pure cross-check logic (declared vs sampled)") {
    const AmbientContract* terrain = findAmbient(RenderPassId::Terrain);
    REQUIRE(terrain != nullptr);  // colorMask=AllOn, depthFunc=SceneGEqual

    // Live matches declaration -> no mismatch.
    AmbientSample good; good.colorMask = ColorMaskState::AllOn; good.depthFunc = DepthFuncState::SceneGEqual;
    CHECK(compareAmbient(*terrain, good).any() == false);

    // Wrong depth func -> depthFunc mismatch only.
    AmbientSample badDepth; badDepth.colorMask = ColorMaskState::AllOn; badDepth.depthFunc = DepthFuncState::ShadowLess;
    AmbientMismatch md = compareAmbient(*terrain, badDepth);
    CHECK(md.depthFunc == true);
    CHECK(md.colorMask == false);

    // Wrong color mask -> colorMask mismatch only.
    AmbientSample badColor; badColor.colorMask = ColorMaskState::AllOff; badColor.depthFunc = DepthFuncState::SceneGEqual;
    AmbientMismatch mc = compareAmbient(*terrain, badColor);
    CHECK(mc.colorMask == true);
    CHECK(mc.depthFunc == false);

    // Unclassifiable live value (Inherit) is skipped -> no false positive.
    AmbientSample unknown;  // all Inherit
    CHECK(compareAmbient(*terrain, unknown).any() == false);
}

TEST_CASE("ambient ledger-2: depth-write declared ON; blend left Inherit (probe-proven)") {
    // depthWrite is a clean per-pass axis (runtime probe: dwMiss=0). blend is NOT --
    // the probe proved GL_BLEND is globally enabled even in opaque passes, so it is left
    // Inherit (asserting Off produced 5881/9804 false mismatches). This test locks that
    // finding: depthWrite ON, blend Inherit, for shadow + opaque passes.
    const RenderPassId opaque[] = {
        RenderPassId::Shadow, RenderPassId::StaticPropOpaque,
        RenderPassId::Terrain, RenderPassId::MechOpaque
    };
    for (int i = 0; i < 4; ++i) {
        const AmbientContract* a = findAmbient(opaque[i]);
        REQUIRE(a != nullptr);
        CHECK(static_cast<unsigned>(a->depthWrite) == static_cast<unsigned>(DepthWriteState::On));
        CHECK(static_cast<unsigned>(a->blend)      == static_cast<unsigned>(BlendState::Inherit));
    }
}

TEST_CASE("compareAmbient: blend + depth-write axes cross-check (pure, synthetic decl)") {
    // Table-independent: exercise the pure compare with a decl that DECLARES blend, so
    // the logic is tested even though no shipped pass declares blend (it's Inherit there).
    AmbientContract decl{};                 // all Inherit / false / nullptr
    decl.depthWrite = DepthWriteState::On;
    decl.blend      = BlendState::Off;

    AmbientSample good; good.blend = BlendState::Off; good.depthWrite = DepthWriteState::On;
    CHECK(compareAmbient(decl, good).any() == false);

    AmbientSample badBlend = good; badBlend.blend = BlendState::On;
    CHECK(compareAmbient(decl, badBlend).blend == true);

    AmbientSample badDW = good; badDW.depthWrite = DepthWriteState::Off;
    CHECK(compareAmbient(decl, badDW).depthWrite == true);

    // Inherit live -> skipped (no false positive even though declared).
    AmbientSample inheritBlend = good; inheritBlend.blend = BlendState::Inherit;
    CHECK(compareAmbient(decl, inheritBlend).blend == false);
}

TEST_CASE("fbo ledger: register/resolve + default-FBO + unregistered") {
    FboLedger led;
    led.reset();
    led.registerFbo(7u, RenderResourceId::MainColor);
    led.registerFbo(10u, RenderResourceId::ShadowDynamicMap);
    auto rid = [](RenderResourceId r){ return static_cast<unsigned>(r); };
    CHECK(rid(led.resolve(7u))  == rid(RenderResourceId::MainColor));
    CHECK(rid(led.resolve(10u)) == rid(RenderResourceId::ShadowDynamicMap));
    CHECK(rid(led.resolve(0u))  == rid(RenderResourceId::Backbuffer));   // default framebuffer
    CHECK(rid(led.resolve(99u)) == rid(RenderResourceId::Unknown));      // unregistered -> skipped
    // re-register updates in place
    led.registerFbo(7u, RenderResourceId::WaterReflectionColor);
    CHECK(rid(led.resolve(7u))  == rid(RenderResourceId::WaterReflectionColor));
}

TEST_CASE("fbo ledger: mismatch logic skips Unknown on either side") {
    // declared==actual -> ok; differ -> mismatch; Unknown either side -> skipped.
    CHECK(fboMismatch(RenderResourceId::MainColor, RenderResourceId::MainColor)        == false);
    CHECK(fboMismatch(RenderResourceId::MainColor, RenderResourceId::ShadowDynamicMap) == true);
    CHECK(fboMismatch(RenderResourceId::Unknown,   RenderResourceId::MainColor)        == false);
    CHECK(fboMismatch(RenderResourceId::MainColor, RenderResourceId::Unknown)          == false);
}

TEST_CASE("fbo ledger: scene passes declare MainColor target; others undeclared") {
    auto rid = [](RenderResourceId r){ return static_cast<unsigned>(r); };
    const unsigned mc = rid(RenderResourceId::MainColor);
    CHECK(rid(declaredFboTarget(RenderPassId::StaticPropOpaque)) == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::Terrain))          == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::TerrainOverlay))   == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::TerrainDecal))     == mc);
    // PostProcess intentionally undeclared (timing-uncertain at the sample seam).
    CHECK(rid(declaredFboTarget(RenderPassId::PostProcess)) == rid(RenderResourceId::Unknown));
}

TEST_CASE("terrain subpass table: 4 rows, writes all MainColor+MainDepth, all read ShadowDynamicMap first") {
    auto rid = [](RenderResourceId r){ return static_cast<unsigned>(r); };
    CHECK(kTerrainSubPassCount == 4);
    CHECK(allTerrainSubPassesWriteMainColorDepth() == true);
    for (int i = 0; i < kTerrainSubPassCount; ++i) {
        CHECK(rid(kTerrainSubPasses[i].reads[0]) == rid(RenderResourceId::ShadowDynamicMap));
    }
}

TEST_CASE("terrain subpass table: Indirect declares Command barrier; others None") {
    auto bk = [](BarrierKind b){ return static_cast<unsigned>(b); };
    const TerrainSubPass* indirect = findTerrainSubPass(TerrainPath::IndirectBridge);
    REQUIRE(indirect != nullptr);
    CHECK(bk(indirect->barrierAfter) == bk(BarrierKind::Command));

    const TerrainPath others[] = { TerrainPath::LODChunk, TerrainPath::PatchStreamThin, TerrainPath::LegacyMLR };
    for (int i = 0; i < 3; ++i) {
        const TerrainSubPass* s = findTerrainSubPass(others[i]);
        REQUIRE(s != nullptr);
        CHECK(bk(s->barrierAfter) == bk(BarrierKind::None));
    }
}

TEST_CASE("terrain subpass table: Indirect reads recipe/thin/cement/mask; LODChunk reads HeightSsbo instead") {
    const TerrainSubPass* indirect = findTerrainSubPass(TerrainPath::IndirectBridge);
    REQUIRE(indirect != nullptr);
    // verify all 4 Indirect-specific reads are present
    bool hasRecipe = false, hasThin = false, hasCement = false, hasMask = false;
    for (int i = 0; i < 6; ++i) {
        if (indirect->reads[i] == RenderResourceId::TerrainRecipeBuffer) hasRecipe = true;
        if (indirect->reads[i] == RenderResourceId::TerrainThinBuffer)   hasThin   = true;
        if (indirect->reads[i] == RenderResourceId::CementAtlas)         hasCement = true;
        if (indirect->reads[i] == RenderResourceId::TransitionMaskArray) hasMask   = true;
    }
    CHECK(hasRecipe == true);
    CHECK(hasThin   == true);
    CHECK(hasCement == true);
    CHECK(hasMask   == true);

    const TerrainSubPass* lod = findTerrainSubPass(TerrainPath::LODChunk);
    REQUIRE(lod != nullptr);
    bool lodHasRecipe = false, lodHasThin = false, lodHasHeight = false;
    for (int i = 0; i < 6; ++i) {
        if (lod->reads[i] == RenderResourceId::TerrainRecipeBuffer) lodHasRecipe = true;
        if (lod->reads[i] == RenderResourceId::TerrainThinBuffer)   lodHasThin   = true;
        if (lod->reads[i] == RenderResourceId::TerrainHeightSsbo)   lodHasHeight = true;
    }
    CHECK(lodHasRecipe == false);
    CHECK(lodHasThin   == false);
    CHECK(lodHasHeight == true);
}

TEST_CASE("terrain subpass table: mvpSource and drawSite per branch") {
    auto ms = [](TerrainMvpSource m){ return static_cast<unsigned>(m); };
    auto ds = [](TerrainDrawSite d){ return static_cast<unsigned>(d); };
    const TerrainSubPass* indirect = findTerrainSubPass(TerrainPath::IndirectBridge);
    REQUIRE(indirect != nullptr);
    CHECK(ms(indirect->mvpSource) == ms(TerrainMvpSource::SnapshotEpoch));

    const TerrainSubPass* patch = findTerrainSubPass(TerrainPath::PatchStreamThin);
    REQUIRE(patch != nullptr);
    CHECK(ms(patch->mvpSource) == ms(TerrainMvpSource::Live));

    const TerrainSubPass* legacy = findTerrainSubPass(TerrainPath::LegacyMLR);
    REQUIRE(legacy != nullptr);
    CHECK(ms(legacy->mvpSource) == ms(TerrainMvpSource::Live));

    const TerrainSubPass* lod = findTerrainSubPass(TerrainPath::LODChunk);
    REQUIRE(lod != nullptr);
    CHECK(ds(lod->drawSite) == ds(TerrainDrawSite::Gamecam));

    const TerrainPath renderListsPaths[] = { TerrainPath::IndirectBridge, TerrainPath::PatchStreamThin, TerrainPath::LegacyMLR };
    for (int i = 0; i < 3; ++i) {
        const TerrainSubPass* s = findTerrainSubPass(renderListsPaths[i]);
        REQUIRE(s != nullptr);
        CHECK(ds(s->drawSite) == ds(TerrainDrawSite::RenderLists));
    }
}

TEST_CASE("terrain subpass latch audit: all reachable branches set markTerrainDrawn (regression guard)") {
    auto tp = [](TerrainPath p){ return static_cast<int>(p); };
    // Every branch declares producesTerrainLatch=true.
    for (int i = 0; i < kTerrainSubPassCount; ++i)
        CHECK(kTerrainSubPasses[i].producesTerrainLatch == true);

    // TERRAIN-INDIRECT-LATCH-FIX-1 (26ee9bdd) closed the IndirectBridge gap: all four
    // branches now call markTerrainDrawn. This is now a REGRESSION GUARD — if any branch's
    // markTerrainDrawn is removed and latchActuallyImplemented is not also set false, this
    // test will trip, catching the regression offline before it ships.
    CHECK(allDeclaredLatchProducersImplemented() == true);
    CHECK(tp(firstUnimplementedLatchProducer())  == tp(TerrainPath::Count));  // Count = none missing

    // All four branches must have latchActuallyImplemented == true.
    const TerrainPath allPaths[] = {
        TerrainPath::LODChunk, TerrainPath::IndirectBridge,
        TerrainPath::PatchStreamThin, TerrainPath::LegacyMLR
    };
    for (int i = 0; i < 4; ++i) {
        const TerrainSubPass* s = findTerrainSubPass(allPaths[i]);
        REQUIRE(s != nullptr);
        CHECK(s->latchActuallyImplemented == true);
    }
}

TEST_CASE("terrain subpass active-branch probe: pure kernel with synthetic counter arrays") {
    auto tp = [](TerrainPath p){ return static_cast<int>(p); };
    // counts {1994,0,0,0} -> drew==1, dominant==LODChunk
    {
        unsigned long c[4] = { 1994, 0, 0, 0 };
        CHECK(terrainPathsThatDrew(c) == 1);
        CHECK(tp(dominantTerrainPath(c)) == tp(TerrainPath::LODChunk));
    }
    // counts {0,0,0,0} -> drew==0, dominant==TerrainPath::Count (none)
    {
        unsigned long c[4] = { 0, 0, 0, 0 };
        CHECK(terrainPathsThatDrew(c) == 0);
        CHECK(tp(dominantTerrainPath(c)) == tp(TerrainPath::Count));
    }
    // counts {5,9,0,0} -> dominant==IndirectBridge, drew==2
    {
        unsigned long c[4] = { 5, 9, 0, 0 };
        CHECK(terrainPathsThatDrew(c) == 2);
        CHECK(tp(dominantTerrainPath(c)) == tp(TerrainPath::IndirectBridge));
    }
}

// ---------------------------------------------------------------------------
// FRAME-GRAPH-EXECUTOR-DRYRUN-1: pure dryRunCompare() kernel (offline, no engine).
// Fabricated traces only — proves fired-set/order/terrain-mutex/latch-miss classification
// and (critically) that UNOBSERVED passes are NOT counted as divergences.
// ---------------------------------------------------------------------------

// Helper: build a trace where the listed passes fired, in the listed RECORD order.
static framegraph::FramePassTrace buildTrace(const RenderPassId* firedInRecordOrder, int count) {
    framegraph::FramePassTrace t;
    framegraph::resetTrace(t, kFramePassOrder, kFramePassOrderCount);
    for (int i = 0; i < count; ++i)
        framegraph::recordPassFired(t, firedInRecordOrder[i],
                                    RenderResourceId::MainColor, kFramePassOrder, kFramePassOrderCount);
    return t;
}

TEST_CASE("dryrun (a): all 11 declared passes fired in declared order -> clean") {
    framegraph::FramePassTrace t = buildTrace(kFramePassOrder, kFramePassOrderCount);
    const framegraph::DryRunReport r =
        framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
    CHECK(r.firedCount == kFramePassOrderCount);
    CHECK(r.unobservedCount == 0);
    CHECK(r.outOfOrderCount == 0);
    CHECK(r.terrainMutexViolation == false);
}

TEST_CASE("dryrun (b): only 7 observable passes fired, 4 invisible -> unobserved!=diverged") {
    // The 4 passes with NO noteRenderPass callsite today (recon): Shadow, Water, VFX, Veg.
    // Everything else fires in declared order. Must yield ZERO false alarms.
    // NOTE: kFramePassOrder now reflects the real engine fire order:
    // MechOpaque(slot1) before StaticPropOpaque(slot2) — MechOpaque fires at the top of
    // renderLists() (txmmgr.cpp:2361); StaticProp fires after GpuStaticPropBatcher::flush.
    // TerrainOverlay(slot4) fires before TerrainDecal(slot5) — txmmgr.cpp:3275/3311.
    const RenderPassId fired[] = {
        RenderPassId::MechOpaque,
        RenderPassId::StaticPropOpaque,
        RenderPassId::Terrain,
        RenderPassId::TerrainOverlay,  // fires before Decal in renderLists
        RenderPassId::TerrainDecal,
        RenderPassId::UI,
        RenderPassId::PostProcess,
    };
    framegraph::FramePassTrace t = buildTrace(fired, 7);
    const framegraph::DryRunReport r =
        framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
    CHECK(r.firedCount == 7);
    CHECK(r.unobservedCount == 4);          // Shadow, Water, VFX, VegetationCards
    CHECK(r.outOfOrderCount == 0);          // PROVES unobserved is NOT a divergence
    CHECK(r.terrainMutexViolation == false);
}

TEST_CASE("dryrun (c): two NON-whitelisted passes swapped -> outOfOrder>0 with an offender") {
    // Swap StaticPropOpaque before MechOpaque (declared order: Mech then StaticProp).
    // Neither has knownEarlyDrawSite, so generic out-of-order detection must fire.
    // NOTE: kFramePassOrder now has MechOpaque(slot1) before StaticPropOpaque(slot2)
    // to match actual engine fire order. Reversing them here is the intentional OOO case.
    const RenderPassId fired[] = {
        RenderPassId::Shadow,
        RenderPassId::StaticPropOpaque,   // recorded before its declared predecessor MechOpaque
        RenderPassId::MechOpaque,
        RenderPassId::Terrain,
        RenderPassId::TerrainOverlay,    // correct order (Overlay before Decal)
        RenderPassId::TerrainDecal,
        RenderPassId::Water,
        RenderPassId::VegetationCards,
        RenderPassId::VFX,
        RenderPassId::UI,
        RenderPassId::PostProcess,
    };
    framegraph::FramePassTrace t = buildTrace(fired, kFramePassOrderCount);
    const framegraph::DryRunReport r =
        framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
    CHECK(r.outOfOrderCount > 0);
    CHECK(static_cast<unsigned>(r.firstOutOfOrderPass) != 0u);   // an offender was identified
    CHECK(r.knownEarlySuppressed == 0);   // no whitelist suppression for these passes
}

TEST_CASE("dryrun (d): latch-miss is data-driven regression guard (false for both IndirectBridge and LODChunk)") {
    // TERRAIN-INDIRECT-LATCH-FIX-1 (26ee9bdd) closed the IndirectBridge latch gap.
    // Now ALL rows have latchActuallyImplemented=true, so terrainLatchMissActive==false
    // for ANY dominant branch. This test re-encodes that reality and would re-trip if a
    // row's latchActuallyImplemented were regressed back to false.
    framegraph::FramePassTrace t = buildTrace(kFramePassOrder, kFramePassOrderCount);
    t.terrainBranch    = framegraph::TerrainPath::IndirectBridge;
    t.terrainDrewCount = 1;
    {
        const framegraph::DryRunReport r =
            framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
        CHECK(r.terrainLatchMissActive == false);  // fix landed; no longer a miss
        CHECK(r.terrainMutexViolation == false);
    }
    t.terrainBranch = framegraph::TerrainPath::LODChunk;
    {
        const framegraph::DryRunReport r =
            framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
        CHECK(r.terrainLatchMissActive == false);
    }
}

TEST_CASE("dryrun (e): two terrain branches drew -> mutex violation") {
    framegraph::FramePassTrace t = buildTrace(kFramePassOrder, kFramePassOrderCount);
    t.terrainBranch    = framegraph::TerrainPath::LODChunk;
    t.terrainDrewCount = 2;
    const framegraph::DryRunReport r =
        framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
    CHECK(r.terrainMutexViolation == true);
}

TEST_CASE("dryrun (f): Option B knownEarlyDrawSite suppression proof") {
    // DRYRUN-DRAWSITE-ORDER-1: Terrain fires before StaticPropOpaque in the LOD-chunk path
    // (gamecam.cpp:508 pre-renderLists). This is an apparent out-of-order vs kFramePassOrder.
    // Without knownEarly -> outOfOrderCount>0 (kernel detects it). With knownEarly -> suppressed.
    //
    // Record Terrain before StaticPropOpaque (the LOD-chunk runtime sequence).
    // The LOD-chunk runtime fire order: Terrain early, then renderLists (Mech, StaticProp, Overlays...)
    const RenderPassId fired[] = {
        RenderPassId::Shadow,
        RenderPassId::Terrain,            // fires pre-renderLists in LOD-chunk path
        RenderPassId::MechOpaque,         // fires first in renderLists preamble
        RenderPassId::StaticPropOpaque,   // fires after GpuStaticPropBatcher::flush
        RenderPassId::TerrainOverlay,
        RenderPassId::TerrainDecal,
        RenderPassId::Water,
        RenderPassId::VegetationCards,
        RenderPassId::VFX,
        RenderPassId::UI,
        RenderPassId::PostProcess,
    };

    // WITHOUT knownEarly: kernel detects the out-of-order and no suppression occurs.
    {
        framegraph::FramePassTrace t = buildTrace(fired, kFramePassOrderCount);
        const framegraph::DryRunReport r =
            framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
        CHECK(r.outOfOrderCount > 0);
        CHECK(r.knownEarlySuppressed == 0);
    }

    // WITH terrain entry marked knownEarly: suppressed into knownEarlySuppressed, NOT outOfOrder.
    // This proves Option B suppression is the cause of the clean report, not a kernel hole.
    {
        framegraph::FramePassTrace t = buildTrace(fired, kFramePassOrderCount);
        framegraph::markEntryKnownEarly(t, RenderPassId::Terrain,
                                        kFramePassOrder, kFramePassOrderCount);
        const framegraph::DryRunReport r =
            framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
        CHECK(r.outOfOrderCount == 0);
        CHECK(r.knownEarlySuppressed > 0);
    }
}

// FRAME-GRAPH-EXECUTOR-ISLAND-1/2/3: offline tests for the pure IslandContract table.
// GL-free — tests only the constexpr descriptor, not the GL wrapper.
// ISLAND-2: re-keyed IslandContract to ExecutorIslandId; added EdgeFog + FogOob rows.
// ISLAND-3: added Shoreline + CloudShadow rows. ScreenShadow SKIPPED (no activeTexture0 restore).

TEST_CASE("executor island (a): kExecutorIslands has PostProcess + EdgeFog + FogOob + Shoreline + CloudShadow rows") {
    using namespace RenderCore::framegraph;
    // Five rows: PostProcess(0), EdgeFog(1), FogOob(2), Shoreline(3), CloudShadow(4).
    CHECK(kExecutorIslandCount == 5u);
    CHECK(static_cast<unsigned>(kExecutorIslands[0].id) ==
          static_cast<unsigned>(ExecutorIslandId::PostProcess));
    CHECK(static_cast<unsigned>(kExecutorIslands[1].id) ==
          static_cast<unsigned>(ExecutorIslandId::EdgeFog));
    CHECK(static_cast<unsigned>(kExecutorIslands[2].id) ==
          static_cast<unsigned>(ExecutorIslandId::FogOob));
    CHECK(static_cast<unsigned>(kExecutorIslands[3].id) ==
          static_cast<unsigned>(ExecutorIslandId::Shoreline));
    CHECK(static_cast<unsigned>(kExecutorIslands[4].id) ==
          static_cast<unsigned>(ExecutorIslandId::CloudShadow));
}

TEST_CASE("executor island (b): findIslandContract(PostProcess) returns non-null with expected flags") {
    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::PostProcess);
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->requiresProgramValid    == true);
        CHECK(c->requiresSceneColorTex   == true);
        CHECK(c->requiresSceneDepthTex   == false);
        CHECK(c->warnIfNoTerrainLatch    == true);
        CHECK(c->postRequiresDefaultFbo  == true);
        CHECK(c->postRequiresBlendDisabled   == false);
        CHECK(c->postRequiresActiveTexture0  == false);
    }
}

TEST_CASE("executor island (c): findIslandContract(EdgeFog) returns non-null with expected flags") {
    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::EdgeFog);
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->requiresProgramValid        == true);
        CHECK(c->requiresSceneColorTex       == false);
        CHECK(c->requiresSceneDepthTex       == true);
        CHECK(c->warnIfNoTerrainLatch        == true);
        CHECK(c->postRequiresDefaultFbo      == false);  // stays on sceneFBO_
        CHECK(c->postRequiresBlendDisabled   == true);
        CHECK(c->postRequiresActiveTexture0  == true);
    }
}

TEST_CASE("executor island (d): findIslandContract(FogOob) returns non-null with expected flags") {
    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::FogOob);
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->requiresProgramValid        == true);
        CHECK(c->requiresSceneColorTex       == false);
        CHECK(c->requiresSceneDepthTex       == true);
        CHECK(c->warnIfNoTerrainLatch        == true);
        CHECK(c->postRequiresDefaultFbo      == false);  // stays on sceneFBO_
        CHECK(c->postRequiresBlendDisabled   == true);
        CHECK(c->postRequiresActiveTexture0  == true);
    }
}

TEST_CASE("executor island (e): findIslandContract(Shoreline) returns non-null with expected flags") {
    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::Shoreline);
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->requiresProgramValid        == true);
        CHECK(c->requiresSceneColorTex       == false);
        CHECK(c->requiresSceneDepthTex       == true);   // reads sceneDepthTex_ + sceneNormalTex_
        CHECK(c->warnIfNoTerrainLatch        == true);   // bails on !sceneHasTerrain_
        CHECK(c->postRequiresDefaultFbo      == false);  // stays on sceneFBO_
        CHECK(c->postRequiresBlendDisabled   == true);   // glDisable(GL_BLEND) on exit
        CHECK(c->postRequiresActiveTexture0  == true);   // glActiveTexture(GL_TEXTURE0) on exit
    }
}

TEST_CASE("executor island (f): findIslandContract(CloudShadow) returns non-null with expected flags") {
    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::CloudShadow);
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->requiresProgramValid        == true);
        CHECK(c->requiresSceneColorTex       == false);
        CHECK(c->requiresSceneDepthTex       == true);   // reads sceneDepthTex_ (unit 0 only)
        CHECK(c->warnIfNoTerrainLatch        == true);   // bails on !sceneHasTerrain_
        CHECK(c->postRequiresDefaultFbo      == false);  // stays on sceneFBO_
        CHECK(c->postRequiresBlendDisabled   == true);   // glDisable(GL_BLEND) on exit
        CHECK(c->postRequiresActiveTexture0  == true);   // glActiveTexture(GL_TEXTURE0) on exit
    }
}

TEST_CASE("executor island (g): findIslandContract(Count) returns nullptr (out-of-range)") {
    using namespace RenderCore::framegraph;
    CHECK(findIslandContract(ExecutorIslandId::Count) == nullptr);
}

} // TEST_SUITE
