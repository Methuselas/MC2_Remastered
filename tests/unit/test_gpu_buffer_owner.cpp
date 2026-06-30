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
