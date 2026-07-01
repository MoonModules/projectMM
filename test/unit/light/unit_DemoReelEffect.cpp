// @module DemoReelEffect

#include "doctest.h"
#include "light/effects/DemoReelEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/layouts/GridLayout.h"

#include <cstring>

using namespace mm;

namespace {
// Build a real 16×16 Layer the reel renders into (the MetaballsEffect test harness).
struct Scene {
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    Scene() {
        grid.width = 16; grid.height = 16; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
    }
    bool anyNonZero() {
        auto& buf = layer.buffer();
        for (size_t i = 0; i < buf.bytes(); i++) if (buf.data()[i]) return true;
        return false;
    }
};
}  // namespace

// The reel enumerates the effect registry, hosts one effect at a time, renders it, and advances
// through the whole list without crashing — the create/teardown/delete churn every tick is the
// robustness path this pins. Registering two real effects + the reel gives it something to cycle.
TEST_CASE("DemoReelEffect cycles registered effects and renders each") {
    REQUIRE(ModuleFactory::registerType<RainbowEffect>("RainbowEffect"));
    REQUIRE(ModuleFactory::registerType<NoiseEffect>("NoiseEffect"));
    REQUIRE(ModuleFactory::registerType<DemoReelEffect>("DemoReelEffect"));

    Scene s;
    DemoReelEffect reel;
    reel.interval = 1;                 // 1 s per effect (advance is driven explicitly below)
    s.layer.addChild(&reel);
    s.layer.onBuildState();            // builds the eligible list + stands up the first child

    // At least the two real effects are eligible; the reel must NOT include itself.
    CHECK(reel.eligibleCountForTest() >= 2);
    const char* first = reel.currentTypeForTest();
    REQUIRE(first != nullptr);
    CHECK(std::strcmp(first, "DemoReelEffect") != 0);   // never hosts itself (no infinite recursion)

    // One tick renders the hosted effect into the shared buffer.
    s.layer.loop();
    CHECK(s.anyNonZero());

    // Advance through a full lap plus extra — every swap is a create → build → teardown → delete of
    // a real effect against the live grid. None of it may crash, and each hosted effect renders.
    const uint8_t n = reel.eligibleCountForTest();
    for (int step = 0; step < n * 2 + 3; step++) {
        reel.advanceForTest();
        const char* cur = reel.currentTypeForTest();
        REQUIRE(cur != nullptr);
        CHECK(std::strcmp(cur, "DemoReelEffect") != 0);
        s.layer.loop();               // render the newly-hosted effect — must not crash
    }
    CHECK(s.anyNonZero());

    reel.teardown();                   // frees the last hosted child cleanly (no leak/double-free)
}
