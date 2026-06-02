// Minimal assert-based runner. Returns nonzero on any failure.
// Fixtures dir comes from argv[1]; the compiled binary lives in the build tree.
#include "model_override_registry.h"
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++failures; } }while(0)

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "fixtures";
    auto fx = [&](const char* f){ return dir + "/" + f; };

    {   ModelOverrideRegistry r;
        int n = r.loadFromFile(fx("valid.json"), dir);
        CHECK(n == 1);
        CHECK(r.count() == 1);
        const ModelOverrideRecord* m = r.resolve("staticProp", "example_name");
        CHECK(m != nullptr);
        if (m) {
            CHECK(m->overrideClass == "staticprop");
            CHECK(m->appearanceName == "example_name");
            CHECK(m->scale == 1.0f);
            CHECK(m->sourceRelPath == "source/props/example.glb");
        }
        CHECK(r.resolve("STATICPROP", "Example_Name") != nullptr);
        CHECK(r.resolve("tree", "example_name") == nullptr);
        CHECK(r.resolve(nullptr, "example_name") == nullptr);
        CHECK(r.resolve("staticProp", nullptr) == nullptr);
    }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("not_renderonly.json"), dir) == 0); }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("bad_fallback.json"), dir) == 0); }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("bad_scale.json"), dir) == 0); }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("unsafe_paths.json"), dir) == 0); }
    {   ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("dup.json"), dir) == 1);
        const ModelOverrideRecord* m = r.resolve("staticProp", "dup_name");
        CHECK(m != nullptr);
        if (m) CHECK(m->sourceRelPath == "first.glb");
    }
    {   ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("non_object_entry.json"), dir) == 1);
        CHECK(r.resolve("tree", "ok") != nullptr);
    }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("malformed.json"), dir) == 0); }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("does_not_exist.json"), dir) == 0); }
    printf(failures ? "TESTS FAILED (%d)\n" : "ALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
