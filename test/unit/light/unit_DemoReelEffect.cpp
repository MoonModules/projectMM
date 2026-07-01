// @module DemoReelEffect

#include "doctest.h"
#include "light/effects/DemoReelEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/draw.h"                // draw::fill — clear the buffer between hosted renders

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
    // a real effect against the live grid. None of it may crash. The buffer is cleared before each
    // render so a per-host render check measures THIS host's output (not pixels left by the prior
    // one); effects that legitimately draw nothing in a single frame (e.g. an audio effect with no
    // audio, a still-fading start) are counted over the run rather than asserted every frame.
    const uint8_t n = reel.eligibleCountForTest();
    int rendered = 0;
    for (int step = 0; step < n * 2 + 3; step++) {
        reel.advanceForTest();
        const char* cur = reel.currentTypeForTest();
        REQUIRE(cur != nullptr);
        CHECK(std::strcmp(cur, "DemoReelEffect") != 0);
        draw::fill(s.layer.buffer(), {0, 0, 0});   // clear so the count measures THIS host's output
        s.layer.loop();                            // render the newly-hosted effect — must not crash
        if (s.anyNonZero()) rendered++;
    }
    CHECK(rendered > 0);   // over the run, hosted effects render into the freshly-cleared buffer

    reel.teardown();                   // frees the last hosted child cleanly (no leak/double-free)
}
