// tests/unit/test_rendercore.cpp
// GL-free unit tests for RenderCore substrate invariants.
// No game startup, no GL context, no asset loading required.
#include "doctest.h"
#include "RenderResourceRegistry.h"
#include "RenderDebugView.h"
#include "RendererFeatureRegistry.h"
#include "IblShRegistry.h"
#include "PipelineDesc.h"               // PIPELINE-DESC-SCAFFOLD-1: target type
#include "render_contract_pipeline.h"   // PIPELINE-DESC-SCAFFOLD-1: adapter under test
#include <cstring>

using namespace RenderCore;

// ---------------------------------------------------------------------------
// RenderResourceRegistry
// ---------------------------------------------------------------------------
// NOTE: s_registry is process-global static. Tests below run in definition
// order and each TEST_CASE leaves the registry in a clean state (count == 0).

TEST_SUITE("RenderCore") {

TEST_CASE("RenderResourceRegistry initial count is zero") {
    CHECK(getRenderResourceCount() == 0);
}

TEST_CASE("RenderResourceRegistry register get invalidate") {
    RenderResourceDesc d{};
    d.id        = RenderResourceId::MainColor;
    d.kind      = RenderResourceKind::Texture2D;
    d.format    = RenderResourceFormat::RGBA8;
    d.debugName = "MainColor";
    d.width     = 1920;
    d.height    = 1080;
    d.valid     = true;
    registerOrUpdateRenderResource(d);

    CHECK(getRenderResourceCount() == 1);

    const RenderResourceDesc* got = getRenderResource(RenderResourceId::MainColor);
    REQUIRE(got != nullptr);
    CHECK(got->width == 1920);
    CHECK(got->height == 1080);
    CHECK((got->format == RenderResourceFormat::RGBA8));
    CHECK((got->kind == RenderResourceKind::Texture2D));

    // Mark invalid -- slot hidden from count and get.
    RenderResourceDesc inv{};
    inv.id    = RenderResourceId::MainColor;
    inv.valid = false;
    registerOrUpdateRenderResource(inv);

    CHECK(getRenderResourceCount() == 0);
    CHECK(getRenderResource(RenderResourceId::MainColor) == nullptr);
}

TEST_CASE("RenderResourceRegistry Unknown id is no-op") {
    size_t before = getRenderResourceCount();
    CHECK(before == 0);  // state invariant: prior tests must leave registry clean
    RenderResourceDesc d{};
    d.id    = RenderResourceId::Unknown;
    d.valid = true;
    registerOrUpdateRenderResource(d);
    CHECK(getRenderResourceCount() == 0);
    CHECK(getRenderResource(RenderResourceId::Unknown) == nullptr);
}

TEST_CASE("RenderResourceRegistry byIndex enumerates in slot order") {
    RenderResourceDesc d{};
    d.valid = true;

    d.id    = RenderResourceId::MainDepth;
    d.width = 200;
    registerOrUpdateRenderResource(d);

    d.id    = RenderResourceId::MainColor;
    d.width = 100;
    registerOrUpdateRenderResource(d);

    CHECK(getRenderResourceCount() == 2);

    // Dense index follows ascending slot order (MainColor=1 < MainDepth=2).
    const RenderResourceDesc* r0 = getRenderResourceByIndex(0);
    const RenderResourceDesc* r1 = getRenderResourceByIndex(1);
    const RenderResourceDesc* r2 = getRenderResourceByIndex(2);

    REQUIRE(r0 != nullptr);
    REQUIRE(r1 != nullptr);
    CHECK(r2 == nullptr);
    CHECK((r0->id == RenderResourceId::MainColor));
    CHECK((r1->id == RenderResourceId::MainDepth));

    // Clean up.
    d.valid = false;
    d.id = RenderResourceId::MainColor;
    registerOrUpdateRenderResource(d);
    d.id = RenderResourceId::MainDepth;
    registerOrUpdateRenderResource(d);
    CHECK(getRenderResourceCount() == 0);
}

TEST_CASE("RenderResourceRegistry toString") {
    CHECK(std::strcmp(toString(RenderResourceId::Unknown),   "Unknown")   == 0);
    CHECK(std::strcmp(toString(RenderResourceId::MainColor), "MainColor") == 0);
    CHECK(std::strcmp(toString(RenderResourceKind::Texture2D), "Texture2D") == 0);
    CHECK(std::strcmp(toString(RenderResourceKind::Buffer),    "Buffer")    == 0);
    CHECK(std::strcmp(toString(RenderResourceFormat::RGBA8),   "RGBA8")     == 0);
    CHECK(std::strcmp(toString(RenderResourceFormat::Depth32F),"Depth32F")  == 0);
}

// ---------------------------------------------------------------------------
// RenderDebugView
// ---------------------------------------------------------------------------

TEST_CASE("RenderDebugView all names non-null and non-empty") {
    for (int i = 0; i < int(RenderDebugView::_Count); ++i) {
        const char* name = RenderDebugViewName(RenderDebugView(i));
        REQUIRE(name != nullptr);
        CHECK(name[0] != '\0');
    }
}

TEST_CASE("RenderDebugView all descriptions non-null") {
    for (int i = 0; i < int(RenderDebugView::_Count); ++i) {
        const char* desc = RenderDebugViewDescription(RenderDebugView(i));
        CHECK(desc != nullptr);
    }
}

TEST_CASE("RenderDebugView spot-check known names") {
    CHECK(std::strcmp(RenderDebugViewName(RenderDebugView::Final),  "Final")  == 0);
    CHECK(std::strcmp(RenderDebugViewName(RenderDebugView::Albedo), "Albedo") == 0);
}

TEST_CASE("RenderDebugView StaticPropOpaque mask covers expected views") {
    CHECK(RenderDebugViewSupported(RenderDebugView::Final,        kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::Albedo,       kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::Normal,       kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::Roughness,    kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::Metallic,     kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::MaterialIdx,  kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::TexArrayLayer,kDebugViewMask_StaticPropOpaque));
}

TEST_CASE("RenderDebugView StaticPropOpaque mask excludes lighting-only views") {
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::LightingOnly, kDebugViewMask_StaticPropOpaque));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::IblOnly,      kDebugViewMask_StaticPropOpaque));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::SpecularOnly, kDebugViewMask_StaticPropOpaque));
}

TEST_CASE("RenderDebugView placeholder lane masks are zero") {
    CHECK(kDebugViewMask_Terrain == 0u);
    CHECK(kDebugViewMask_Shadow  == 0u);
}

TEST_CASE("RenderDebugView Vfx mask covers expected views") {
    // VFX-DEBUG-VIEWS-1: only Final/Albedo map to the canonical enum. The
    // shader's Alpha/ParticleKind/Overdraw modes are VFX-local (no enum slot).
    CHECK(RenderDebugViewSupported(RenderDebugView::Final,  kDebugViewMask_Vfx));
    CHECK(RenderDebugViewSupported(RenderDebugView::Albedo, kDebugViewMask_Vfx));
    // Particles have no normal/roughness/metallic/IBL/specular/materialIdx data.
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Normal,        kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Roughness,     kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Metallic,      kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::LightingOnly,  kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::IblOnly,       kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::SpecularOnly,  kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::MaterialIdx,   kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::TexArrayLayer, kDebugViewMask_Vfx));
}

TEST_CASE("RenderDebugView Mech mask covers expected views") {
    // MECH-DEBUG-VIEWS-1: mech.frag has Final/Albedo/Normal/LightingOnly branches.
    CHECK(RenderDebugViewSupported(RenderDebugView::Final,        kDebugViewMask_Mech));
    CHECK(RenderDebugViewSupported(RenderDebugView::Albedo,       kDebugViewMask_Mech));
    CHECK(RenderDebugViewSupported(RenderDebugView::Normal,       kDebugViewMask_Mech));
    CHECK(RenderDebugViewSupported(RenderDebugView::LightingOnly, kDebugViewMask_Mech));
    // mech.frag has no roughness/metallic/IBL/specular/materialIdx/texArrayLayer data.
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Roughness,     kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Metallic,      kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::MaterialIdx,   kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::TexArrayLayer, kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::IblOnly,       kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::SpecularOnly,  kDebugViewMask_Mech));
}

TEST_CASE("RenderDebugView out-of-range returns false") {
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView(-1),  kDebugViewMask_StaticPropOpaque));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView(100), 0xFFFFFFFFu));
}

// ---------------------------------------------------------------------------
// RendererFeatureRegistry
// ---------------------------------------------------------------------------

TEST_CASE("RendererFeatureRegistry COUNT is 30") {
    CHECK(static_cast<int>(RendererFeature::COUNT) == 30);
}

TEST_CASE("RendererFeatureRegistry kFeatureTable length matches COUNT") {
    constexpr size_t len = sizeof(kFeatureTable) / sizeof(kFeatureTable[0]);
    CHECK(len == static_cast<size_t>(RendererFeature::COUNT));
}

TEST_CASE("RendererFeatureRegistry all entries have non-null featureId and doc") {
    for (int i = 0; i < static_cast<int>(RendererFeature::COUNT); ++i) {
        REQUIRE(kFeatureTable[i].featureId != nullptr);
        CHECK(kFeatureTable[i].featureId[0] != '\0');
        CHECK(kFeatureTable[i].doc != nullptr);
    }
}

TEST_CASE("RendererFeatureRegistry ViewUniforms is default-on with expected envVar") {
    const EnvVarDesc& e = kFeatureTable[static_cast<int>(RendererFeature::ViewUniforms)];
    CHECK(e.defaultOn == true);
    REQUIRE(e.envVar != nullptr);
    CHECK(std::strcmp(e.envVar, "MC2_VIEW_UNIFORMS") == 0);
}

TEST_CASE("RendererFeatureRegistry always-on features have null envVar") {
    // TerrainTessellation and ReverseZ have no env var kill-switch.
    CHECK(kFeatureTable[static_cast<int>(RendererFeature::TerrainTessellation)].envVar == nullptr);
    CHECK(kFeatureTable[static_cast<int>(RendererFeature::ReverseZ)].envVar           == nullptr);
}

TEST_CASE("RendererFeatureRegistry Retired entries have Retired kind") {
    CHECK((kFeatureTable[static_cast<int>(RendererFeature::GpuMechs)].kind              == EnvVarKind::Retired));
    CHECK((kFeatureTable[static_cast<int>(RendererFeature::StaticPropIndirect)].kind   == EnvVarKind::Retired));
    CHECK((kFeatureTable[static_cast<int>(RendererFeature::DrawPacketStaticPropV6)].kind == EnvVarKind::Retired));
}

// ---------------------------------------------------------------------------
// IblShRegistry
// ---------------------------------------------------------------------------

TEST_CASE("IblShRegistry default set exists") {
    const IblShSet* s = findShSetByName("default");
    REQUIRE(s != nullptr);
    CHECK(std::strcmp(s->name, "default") == 0);
    CHECK(s->coeffs != nullptr);
}

TEST_CASE("IblShRegistry findShSetByName is case-insensitive") {
    CHECK(findShSetByName("DEFAULT") != nullptr);
    CHECK(findShSetByName("Default") != nullptr);
}

TEST_CASE("IblShRegistry findShSetByName returns null for unknown") {
    CHECK(findShSetByName("nonexistent_set") == nullptr);
    CHECK(findShSetByName(nullptr)           == nullptr);
    CHECK(findShSetByName("")                == nullptr);
}

TEST_CASE("IblShRegistry lookupShSet always returns default fallback") {
    const IblShSet& def = lookupShSet(nullptr);
    CHECK(std::strcmp(def.name, "default") == 0);

    const IblShSet& unknown = lookupShSet("totally_unknown_mission");
    CHECK(std::strcmp(unknown.name, "default") == 0);

    // kMissionShMap currently empty; any real mission name falls to default.
    const IblShSet& mc201 = lookupShSet("mc2_01");
    CHECK(std::strcmp(mc201.name, "default") == 0);
}

TEST_CASE("IblShRegistry kIblShSetCount is at least 1") {
    CHECK(kIblShSetCount >= 1u);
    CHECK(std::strcmp(kIblShSets[0].name, "default") == 0);
}

} // TEST_SUITE("RenderCore")

// ---------------------------------------------------------------------------
// PipelineDesc + render_contract -> PipelineDesc adapter
// PIPELINE-DESC-SCAFFOLD-1: GL-free conversion substrate (no production caller
// yet; this proves the adapter lowers a PassStateContract correctly).
// ---------------------------------------------------------------------------

namespace {

// Field-by-field equality. PipelineDesc deliberately has NO operator== (the
// scaffold slice changes the struct by zero bytes); the test owns comparison.
bool pipelineDescEqual(const PipelineDesc& a, const PipelineDesc& b) {
    return a.glProgramName        == b.glProgramName
        && a.blend                == b.blend
        && a.depthTestEnable      == b.depthTestEnable
        && a.depthWriteEnable     == b.depthWriteEnable
        && a.depthFunc            == b.depthFunc
        && a.cullMode             == b.cullMode
        && a.colorAttachments.color0 == b.colorAttachments.color0
        && a.colorAttachments.color1 == b.colorAttachments.color1
        && a.colorAttachments.color2 == b.colorAttachments.color2
        && a.objectIdWriteEnabled == b.objectIdWriteEnabled
        && a.ssboBindingsMask     == b.ssboBindingsMask;
}

// Representative opaque MRT+objectId contract, mirroring kStaticPropState in
// render_contract.cpp (which the GL-free test target cannot link).
render_contract::PassStateContract makeStaticPropContract() {
    render_contract::PassStateContract c{};
    c.requiresDepthTest   = true;
    c.requiresDepthWrite  = true;
    c.blend               = render_contract::PassStateContract::BlendMode::Opaque;
    c.requiresMRT         = true;
    c.attachmentCount     = 3;
    c.attachments         = render_contract::RequiredAttachments{true, true, true};
    c.expectedFBO         = "scene HDR FBO (MRT + objectId)";
    c.restoresStateOnExit = false;
    return c;
}

} // namespace

TEST_SUITE("RenderCore") {

TEST_CASE("PipelineDesc value-init zeroes every field") {
    PipelineDesc d{};
    CHECK(d.glProgramName == 0u);
    CHECK((d.blend == BlendMode::Opaque));            // enum value 0
    CHECK(d.depthTestEnable  == false);
    CHECK(d.depthWriteEnable == false);
    CHECK((d.depthFunc == DepthFunc::LessEqual));     // enum value 0
    CHECK((d.cullMode  == CullMode::None));           // enum value 0
    CHECK(d.colorAttachments.color0 == false);
    CHECK(d.colorAttachments.color1 == false);
    CHECK(d.colorAttachments.color2 == false);
    CHECK(d.objectIdWriteEnabled == false);
    CHECK(d.ssboBindingsMask == 0u);
}

TEST_CASE("PipelineDesc field-equality helper agrees on identical descs") {
    PipelineDesc a = render_contract::pipelineDescFromPassContract(makeStaticPropContract());
    PipelineDesc b = render_contract::pipelineDescFromPassContract(makeStaticPropContract());
    CHECK(pipelineDescEqual(a, b));

    b.depthWriteEnable = !b.depthWriteEnable;          // perturb one field
    CHECK_FALSE(pipelineDescEqual(a, b));
}

TEST_CASE("pipelineDescFromPassContract converts a representative pass contract") {
    render_contract::PassStateContract c = makeStaticPropContract();
    PipelineDesc d = render_contract::pipelineDescFromPassContract(
        c,
        /*glProgramName*/ 42u,
        /*depthFunc*/     DepthFunc::GreaterEqual,
        /*cullMode*/      CullMode::Back,
        /*objectIdWrite*/ true,
        /*ssboMask*/      (1u << 16));   // BASE_INSTANCE slot

    // Derived from the contract.
    CHECK(d.depthTestEnable  == true);
    CHECK(d.depthWriteEnable == true);
    CHECK((d.blend == BlendMode::Opaque));
    CHECK(d.colorAttachments.color0 == true);
    CHECK(d.colorAttachments.color1 == true);
    CHECK(d.colorAttachments.color2 == true);

    // Passed through explicitly (PassStateContract carries none of these).
    CHECK(d.glProgramName == 42u);
    CHECK((d.depthFunc == DepthFunc::GreaterEqual));
    CHECK((d.cullMode  == CullMode::Back));
    CHECK(d.objectIdWriteEnabled == true);
    CHECK(d.ssboBindingsMask == (1u << 16));
}

TEST_CASE("pipelineDescFromPassContract applies documented MC2 defaults") {
    // Minimal contract; all explicit args omitted -> defaults must hold.
    render_contract::PassStateContract c{};
    c.blend       = render_contract::PassStateContract::BlendMode::Opaque;
    c.attachments = render_contract::RequiredAttachments{true, false, false};

    PipelineDesc d = render_contract::pipelineDescFromPassContract(c);
    CHECK(d.glProgramName == 0u);                       // unregistered
    CHECK((d.depthFunc == DepthFunc::GreaterEqual));     // reverse-Z default
    CHECK((d.cullMode  == CullMode::Back));              // opaque-geometry default
    CHECK(d.objectIdWriteEnabled == false);
    CHECK(d.ssboBindingsMask == 0u);
}

TEST_CASE("pipelineDescFromPassContract maps every blend mode") {
    using BM = render_contract::PassStateContract::BlendMode;
    render_contract::PassStateContract c{};
    c.attachments = render_contract::RequiredAttachments{true, false, false};

    c.blend = BM::Opaque;
    CHECK((render_contract::pipelineDescFromPassContract(c).blend == BlendMode::Opaque));
    c.blend = BM::AlphaBlend;
    CHECK((render_contract::pipelineDescFromPassContract(c).blend == BlendMode::AlphaBlend));
    c.blend = BM::AlphaTest;
    CHECK((render_contract::pipelineDescFromPassContract(c).blend == BlendMode::AlphaTest));
    c.blend = BM::Additive;
    CHECK((render_contract::pipelineDescFromPassContract(c).blend == BlendMode::Additive));
}

TEST_CASE("pipelineDescFromPassContract preserves attachment/draw-buffer contract") {
    // Each row is an actual contract attachment pattern from render_contract.cpp.
    struct Row { bool c0, c1, c2; } rows[] = {
        {false, false, false},   // ShadowCaster: depth-only FBO
        {true,  false, false},   // PostProcess / UI: single attachment
        {true,  true,  false},   // TerrainBase / Water: MRT, no objectId
        {true,  true,  true},    // StaticProp / OpaqueObject: MRT + objectId
    };
    for (const Row& r : rows) {
        render_contract::PassStateContract c{};
        c.attachments = render_contract::RequiredAttachments{r.c0, r.c1, r.c2};
        PipelineDesc d = render_contract::pipelineDescFromPassContract(c);
        CHECK(d.colorAttachments.color0 == r.c0);
        CHECK(d.colorAttachments.color1 == r.c1);
        CHECK(d.colorAttachments.color2 == r.c2);
    }
}

TEST_CASE("pipelineDescFromPassContract carries depth test/write independently") {
    render_contract::PassStateContract c{};
    c.attachments = render_contract::RequiredAttachments{true, true, false};

    // AlphaObject pattern: depth test ON, depth write OFF.
    c.requiresDepthTest  = true;
    c.requiresDepthWrite = false;
    PipelineDesc d = render_contract::pipelineDescFromPassContract(c);
    CHECK(d.depthTestEnable  == true);
    CHECK(d.depthWriteEnable == false);

    // PostProcess pattern: both OFF.
    c.requiresDepthTest  = false;
    c.requiresDepthWrite = false;
    d = render_contract::pipelineDescFromPassContract(c);
    CHECK(d.depthTestEnable  == false);
    CHECK(d.depthWriteEnable == false);
}

TEST_CASE("PipelineDesc stays within its hot-path size budget") {
    // Mirrors the static_assert in PipelineDesc.h; a runtime guard so a struct
    // growth shows up as a failing test, not only a compile error elsewhere.
    CHECK(sizeof(PipelineDesc) <= 20u);
}

} // TEST_SUITE("RenderCore")
