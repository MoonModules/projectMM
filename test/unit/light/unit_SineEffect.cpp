// @module SineEffect

#include "doctest.h"
#include "light/effects/SineEffect.h"
#include "light/layouts/GridLayout.h"

// Render the effect on a w×h×d grid for one tick; returns the layer (buffer populated).
static void buildLayer(mm::Layouts& layouts, mm::GridLayout& grid, mm::Layer& layer,
                       mm::SineEffect& sine, mm::lengthType w, mm::lengthType h, mm::lengthType d) {
    grid.width = w; grid.height = h; grid.depth = d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&sine);
    layer.onBuildState();
    layer.loop();
}

TEST_CASE("SineEffect writes non-zero RGB data") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::SineEffect sine;
    buildLayer(layouts, grid, layer, sine, 8, 8, 1);
    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    bool nonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) if (buf.data()[i]) { nonZero = true; break; }
    CHECK(nonZero);
}

TEST_CASE("SineEffect amplitude 0 yields a black buffer") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::SineEffect sine;
    sine.amplitude = 0;
    buildLayer(layouts, grid, layer, sine, 8, 8, 1);
    auto& buf = layer.buffer();
    for (size_t i = 0; i < buf.bytes(); i++) CHECK(buf.data()[i] == 0);
}

TEST_CASE("SineEffect varies across the x axis (R channel follows x)") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::SineEffect sine;
    sine.frequency = 8;   // a full wave across an 8-wide grid → adjacent columns differ
    buildLayer(layouts, grid, layer, sine, 8, 1, 1);
    auto& buf = layer.buffer();
    // R of column 0 vs column 4 should differ (half a wave apart at freq 8 on width 8).
    CHECK(buf.data()[0] != buf.data()[4 * 3]);
}

TEST_CASE("SineEffect survives a 0x0x0 grid") {
    mm::Layouts layouts; mm::GridLayout grid; mm::Layer layer; mm::SineEffect sine;
    buildLayer(layouts, grid, layer, sine, 0, 0, 0);   // must not crash
    CHECK(true);
}
