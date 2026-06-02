// @module PlasmaEffect
// @also NoiseEffect

#include "doctest.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/layouts/GridLayout.h"

// One tick on an 8×8 grid produces at least one non-zero byte.
TEST_CASE("PlasmaEffect writes non-zero RGB data to buffer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::PlasmaEffect plasma;
    layer.addChild(&plasma);

    layer.onBuildState();
    layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);

    bool hasNonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { hasNonZero = true; break; }
    }
    CHECK(hasNonZero);
}

// Opposite corners of a 16×16 grid differ in colour (the plasma is not flat-filling).
TEST_CASE("PlasmaEffect produces spatial variation") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::PlasmaEffect plasma;
    layer.addChild(&plasma);

    layer.onBuildState();
    layer.loop();

    auto* data = layer.buffer().data();
    uint8_t r0 = data[0], g0 = data[1], b0 = data[2];
    size_t lastIdx = (16 * 16 - 1) * 3;
    uint8_t r1 = data[lastIdx], g1 = data[lastIdx + 1], b1 = data[lastIdx + 2];
    CHECK((r0 != r1 || g0 != g1 || b0 != b1));
}

// Plasma and Noise produce visibly different frames on the same grid (sanity check that they're distinct algorithms).
TEST_CASE("PlasmaEffect produces different output than NoiseEffect") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer1;
    layer1.setLayouts(&layouts);
    layer1.setChannelsPerLight(3);
    mm::PlasmaEffect plasma;
    layer1.addChild(&plasma);
    layer1.onBuildState();
    layer1.loop();

    mm::Layer layer2;
    layer2.setLayouts(&layouts);
    layer2.setChannelsPerLight(3);
    mm::NoiseEffect noise;
    layer2.addChild(&noise);
    layer2.onBuildState();
    layer2.loop();

    bool differs = false;
    for (size_t i = 0; i < layer1.buffer().bytes(); i++) {
        if (layer1.buffer().data()[i] != layer2.buffer().data()[i]) {
            differs = true;
            break;
        }
    }
    CHECK(differs);
}
