// @module DistortionWavesEffect

#include "doctest.h"
#include "light/effects/DistortionWavesEffect.h"
#include "light/layouts/GridLayout.h"

static void buildLayer(mm::Layouts& layouts, mm::GridLayout& grid, mm::Layer& layer,
                       mm::DistortionWavesEffect& fx,
                       mm::lengthType w, mm::lengthType h, mm::lengthType d) {
    grid.width = w; grid.height = h; grid.depth = d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&fx);
    layer.onBuildState();
    layer.loop();
}

TEST_CASE("DistortionWavesEffect writes non-zero RGB data") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::DistortionWavesEffect fx;
    buildLayer(layouts, grid, layer, fx, 16, 16, 1);
    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    bool nonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) if (buf.data()[i]) { nonZero = true; break; }
    CHECK(nonZero);
}

TEST_CASE("DistortionWavesEffect produces spatial variation") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::DistortionWavesEffect fx;
    buildLayer(layouts, grid, layer, fx, 16, 16, 1);
    auto& buf = layer.buffer();
    // Two distant lights should not be identical across all three channels.
    const uint8_t* a = buf.data();                      // light (0,0)
    const uint8_t* b = buf.data() + (8 * 16 + 8) * 3;   // light (8,8)
    CHECK(!(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]));
}

TEST_CASE("DistortionWavesEffect speed 0 is frozen (stable across ticks)") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::DistortionWavesEffect fx;
    fx.speed = 0;
    buildLayer(layouts, grid, layer, fx, 8, 8, 1);
    auto& buf = layer.buffer();
    uint8_t first = buf.data()[0];
    layer.loop();   // a second tick — frozen, so the value must not move
    CHECK(buf.data()[0] == first);
}

TEST_CASE("DistortionWavesEffect survives a 0x0x0 grid") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::DistortionWavesEffect fx;
    buildLayer(layouts, grid, layer, fx, 0, 0, 0);
    CHECK(true);
}
