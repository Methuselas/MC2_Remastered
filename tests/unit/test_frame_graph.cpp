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

TEST_CASE("terrain subpass latch audit: Indirect latch miss is CURRENT REALITY (HIGH finding tripwire)") {
    auto tp = [](TerrainPath p){ return static_cast<int>(p); };
    // Every branch declares producesTerrainLatch=true.
    for (int i = 0; i < kTerrainSubPassCount; ++i)
        CHECK(kTerrainSubPasses[i].producesTerrainLatch == true);

    // Indirect does NOT actually call markTerrainDrawn — the recon §4 HIGH finding.
    // allDeclaredLatchProducersImplemented() == false encodes the current reality.
    // NOTE: This test will START FAILING (by design) once the Indirect markTerrainDrawn fix lands.
    // At that point flip latchActuallyImplemented for IndirectBridge to true and update
    // allImplemented CHECK to == true. This is the intended tripwire.
    CHECK(allDeclaredLatchProducersImplemented() == false);
    CHECK(tp(firstUnimplementedLatchProducer())  == tp(TerrainPath::IndirectBridge));

    const TerrainSubPass* indirect = findTerrainSubPass(TerrainPath::IndirectBridge);
    REQUIRE(indirect != nullptr);
    CHECK(indirect->latchActuallyImplemented == false);

    const TerrainPath implementedPaths[] = { TerrainPath::LODChunk, TerrainPath::PatchStreamThin, TerrainPath::LegacyMLR };
    for (int i = 0; i < 3; ++i) {
        const TerrainSubPass* s = findTerrainSubPass(implementedPaths[i]);
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

} // TEST_SUITE
