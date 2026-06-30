// tests/unit/test_gpu_buffer_owner.cpp
// GPU-BUFFER-OWNER-SKELETON-1: GL-free unit tests for the GpuBufferOwner record
// and the 3 new RenderResourceIds. No GL context, no engine, pure value logic.
#include "doctest.h"
#include "GpuBufferOwner.h"
#include "RenderResourceRegistry.h"
#include <cstring>
#include <type_traits>

using namespace RenderCore;

TEST_SUITE("GpuBufferOwner") {

TEST_CASE("GpuBufferOwner default is invalid (zero handle, null name, Unset)") {
    GpuBufferOwner o{};
    CHECK((o.id == RenderResourceId::Unknown));
    CHECK((o.lifetime == RenderResourceLifetime::Unset));
    CHECK(o.debugName == nullptr);
    CHECK(o.glName == 0u);
    CHECK_FALSE(o.valid());
}

TEST_CASE("GpuBufferOwner valid() requires a non-zero handle AND a non-null name") {
    GpuBufferOwner o{};
    o.id        = RenderResourceId::ViewUniformsUbo;
    o.lifetime  = RenderResourceLifetime::Persistent;
    o.debugName = "ViewUniformsUbo";
    o.glName    = 7u;
    CHECK(o.valid());

    // Zero handle -> invalid even with a name.
    GpuBufferOwner noHandle = o;
    noHandle.glName = 0u;
    CHECK_FALSE(noHandle.valid());

    // Null name -> invalid even with a handle.
    GpuBufferOwner noName = o;
    noName.debugName = nullptr;
    CHECK_FALSE(noName.valid());
}

TEST_CASE("GpuBufferOwner id and lifetime round-trip the stored fields") {
    GpuBufferOwner o{};
    o.id        = RenderResourceId::TerrainCementSsbo;
    o.lifetime  = RenderResourceLifetime::Mission;
    o.debugName = "TerrainCementSsbo";
    o.glName    = 4242u;

    CHECK((o.id == RenderResourceId::TerrainCementSsbo));
    CHECK((o.lifetime == RenderResourceLifetime::Mission));
    CHECK(o.glName == 4242u);
    CHECK(std::strcmp(o.debugName, "TerrainCementSsbo") == 0);
    CHECK(o.valid());
}

// TERRAIN-HEIGHT-SSBO-OWNER-1: the LOD-chunk height SSBO (id 14, already-existing,
// already-registered) is now narrowed behind a GpuBufferOwner like type/cement.
TEST_CASE("TERRAIN-HEIGHT-SSBO-OWNER-1 height owner round-trips id/lifetime/name and valid->invalid") {
    GpuBufferOwner o{
        RenderResourceId::TerrainHeightSsbo,
        RenderResourceLifetime::Mission,
        "TerrainHeightSsbo",
        0u};
    // Newly-constructed (glName 0) -> not yet allocated -> invalid.
    CHECK_FALSE(o.valid());

    // After glGen stores a handle -> valid, fields preserved.
    o.glName = 14u;
    CHECK((o.id == RenderResourceId::TerrainHeightSsbo));
    CHECK((o.lifetime == RenderResourceLifetime::Mission));
    CHECK(std::strcmp(o.debugName, "TerrainHeightSsbo") == 0);
    CHECK(o.valid());

    // Invalidate-on-destroy (glName cleared) -> invalid again.
    o.glName = 0u;
    CHECK_FALSE(o.valid());

    CHECK(std::strcmp(toString(RenderResourceId::TerrainHeightSsbo), "TerrainHeightSsbo") == 0);
    CHECK(int(RenderResourceId::TerrainHeightSsbo) < int(RenderResourceId::Count));
}

TEST_CASE("MECH-MATERIAL-SSBO-OWNER-1 material owner round-trips id/lifetime/name and valid->invalid") {
    GpuBufferOwner o{
        RenderResourceId::MaterialGpuBuffer,
        RenderResourceLifetime::Persistent,
        "MaterialGpuBuffer",
        0u};
    // Newly-constructed (glName 0) -> not yet allocated -> invalid.
    CHECK_FALSE(o.valid());

    // After glGen stores a handle -> valid, fields preserved.
    o.glName = 22u;
    CHECK((o.id == RenderResourceId::MaterialGpuBuffer));
    CHECK((o.lifetime == RenderResourceLifetime::Persistent));
    CHECK(std::strcmp(o.debugName, "MaterialGpuBuffer") == 0);
    CHECK(o.valid());

    // Invalidate-on-destroy (glName cleared) -> invalid again.
    o.glName = 0u;
    CHECK_FALSE(o.valid());

    CHECK(std::strcmp(toString(RenderResourceId::MaterialGpuBuffer), "MaterialGpuBuffer") == 0);
    CHECK(int(RenderResourceId::MaterialGpuBuffer) < int(RenderResourceId::Count));
}

TEST_CASE("GpuBufferOwner is a trivially-copyable POD") {
    CHECK(std::is_trivially_copyable<GpuBufferOwner>::value);
}

// The 3 new ids round-trip through toString and sit strictly below Count.
TEST_CASE("GPU-BUFFER-OWNER-SKELETON-1 new ids toString and ordering") {
    CHECK(std::strcmp(toString(RenderResourceId::ViewUniformsUbo),   "ViewUniformsUbo")   == 0);
    CHECK(std::strcmp(toString(RenderResourceId::TerrainTypeSsbo),   "TerrainTypeSsbo")   == 0);
    CHECK(std::strcmp(toString(RenderResourceId::TerrainCementSsbo), "TerrainCementSsbo") == 0);

    CHECK(int(RenderResourceId::ViewUniformsUbo)   < int(RenderResourceId::Count));
    CHECK(int(RenderResourceId::TerrainTypeSsbo)   < int(RenderResourceId::Count));
    CHECK(int(RenderResourceId::TerrainCementSsbo) < int(RenderResourceId::Count));
}

} // TEST_SUITE("GpuBufferOwner")

// GPU-BUFFER-OWNER-LIFETIME-CHECK-1: offline guard that the 4 GPU-owner
// RenderResourceIds (view-UBO / terrain type / cement / height), when
// registered as valid Buffer resources, carry a concrete (non-Unset) lifetime
// and pass the registry lifetime validator. This locks in the prior owner
// slices against a future registration that defaults the lifetime to Unset.
// GL-free, offline; uses only the existing registry API.
TEST_SUITE("GpuBufferOwnerLifetimeCheck") {

namespace {
// The 4 owner ids paired with their registered lifetime + debugName literal.
struct OwnerExpectation {
    RenderCore::RenderResourceId     id;
    RenderCore::RenderResourceLifetime lifetime;
    const char*                      name;
};
const OwnerExpectation kOwners[] = {
    { RenderCore::RenderResourceId::ViewUniformsUbo,   RenderCore::RenderResourceLifetime::Persistent, "ViewUniformsUbo"   },
    { RenderCore::RenderResourceId::TerrainTypeSsbo,   RenderCore::RenderResourceLifetime::Mission,     "TerrainTypeSsbo"   },
    { RenderCore::RenderResourceId::TerrainCementSsbo, RenderCore::RenderResourceLifetime::Mission,     "TerrainCementSsbo" },
    { RenderCore::RenderResourceId::TerrainHeightSsbo, RenderCore::RenderResourceLifetime::Mission,     "TerrainHeightSsbo" },
};
} // namespace

TEST_CASE("the 4 GPU-owner ids register as Buffer resources with non-Unset lifetimes that pass the validator") {
    REQUIRE(getRenderResourceCount() == 0);  // clean registry from prior tests

    for (const auto& e : kOwners) {
        RenderResourceDesc d{};
        d.id        = e.id;
        d.kind      = RenderResourceKind::Buffer;
        d.format    = RenderResourceFormat::BufferRaw;
        d.lifetime  = e.lifetime;
        d.debugName = e.name;
        d.glName    = 100u + uint32_t(e.id);  // nonzero handle
        d.sizeBytes = 4096u;
        d.valid     = true;
        registerOrUpdateRenderResource(d);
    }
    REQUIRE(getRenderResourceCount() == 4);

    // Every valid resource carries a lifetime -> validator clean.
    RenderResourceId off = RenderResourceId::MainColor;  // poison
    CHECK(validateRenderResourceLifetimes(&off));
    CHECK((off == RenderResourceId::Unknown));

    // Per-owner contract: Buffer kind, non-Unset lifetime, nonzero handle,
    // and toString(id) matches the debugName literal.
    for (const auto& e : kOwners) {
        const RenderResourceDesc* got = getRenderResource(e.id);
        REQUIRE(got != nullptr);
        CHECK((got->kind == RenderResourceKind::Buffer));
        CHECK((got->lifetime != RenderResourceLifetime::Unset));
        CHECK((got->lifetime == e.lifetime));
        CHECK(got->glName != 0u);
        CHECK(got->valid);
        CHECK(std::strcmp(toString(e.id), e.name) == 0);
    }

    // Clean up so sibling suites see an empty registry.
    for (const auto& e : kOwners) {
        RenderResourceDesc inv{};
        inv.id = e.id; inv.valid = false;
        registerOrUpdateRenderResource(inv);
    }
    CHECK(getRenderResourceCount() == 0);
}

// Guards the failure direction: an owner id registered with a DEFAULTED
// (Unset) lifetime must be caught by the validator and named. This is exactly
// the future regression this slice exists to block.
TEST_CASE("a GPU-owner id registered with a defaulted Unset lifetime is detected by the validator") {
    REQUIRE(getRenderResourceCount() == 0);

    RenderResourceDesc bad{};
    bad.id        = RenderResourceId::TerrainHeightSsbo;
    bad.kind      = RenderResourceKind::Buffer;
    bad.debugName = "TerrainHeightSsbo";
    bad.glName    = 314u;
    bad.valid     = true;
    // bad.lifetime deliberately left at the struct default (Unset).
    REQUIRE((bad.lifetime == RenderResourceLifetime::Unset));
    registerOrUpdateRenderResource(bad);

    RenderResourceId off = RenderResourceId::Unknown;
    CHECK_FALSE(validateRenderResourceLifetimes(&off));
    CHECK((off == RenderResourceId::TerrainHeightSsbo));

    // Clean up.
    RenderResourceDesc inv{};
    inv.id = RenderResourceId::TerrainHeightSsbo; inv.valid = false;
    registerOrUpdateRenderResource(inv);
    CHECK(getRenderResourceCount() == 0);
}

} // TEST_SUITE("GpuBufferOwnerLifetimeCheck")
