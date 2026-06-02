// @module NoiseEffect
// @also PlasmaEffect, RainbowEffect

#include "doctest.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/layouts/GridLayout.h"

// Hash one z-slice of the layer buffer (used by 3D-depth tests below).
static uint32_t hashSlice(const uint8_t* data, size_t sliceBytes) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sliceBytes; i++) { h ^= data[i]; h *= 16777619u; }
    return h;
}

// One tick on an 8×8 grid leaves at least one non-zero byte (noise paints somewhere).
TEST_CASE("NoiseEffect writes non-zero RGB data to buffer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::NoiseEffect noise;
    layer.addChild(&noise);

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

// Opposite corners of a 16×16 grid carry different colours (noise is not flat).
TEST_CASE("NoiseEffect produces spatial variation") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::NoiseEffect noise;
    layer.addChild(&noise);

    layer.onBuildState();
    layer.loop();

    auto* data = layer.buffer().data();
    // Compare corners — noise should produce different values
    uint8_t r0 = data[0], g0 = data[1], b0 = data[2];
    size_t lastIdx = (16 * 16 - 1) * 3;
    uint8_t r1 = data[lastIdx], g1 = data[lastIdx + 1], b1 = data[lastIdx + 2];
    CHECK((r0 != r1 || g0 != g1 || b0 != b1));
}

// Noise and Rainbow produce visibly different frames on the same grid (sanity check that they're distinct algorithms).
TEST_CASE("NoiseEffect produces different output than RainbowEffect") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    // Render rainbow
    mm::Layer layer1;
    layer1.setLayouts(&layouts);
    layer1.setChannelsPerLight(3);
    mm::RainbowEffect rainbow;
    layer1.addChild(&rainbow);
    layer1.onBuildState();
    layer1.loop();

    // Render noise
    mm::Layer layer2;
    layer2.setLayouts(&layouts);
    layer2.setChannelsPerLight(3);
    mm::NoiseEffect noise;
    layer2.addChild(&noise);
    layer2.onBuildState();
    layer2.loop();

    // Compare buffers — should differ
    bool differs = false;
    for (size_t i = 0; i < layer1.buffer().bytes(); i++) {
        if (layer1.buffer().data()[i] != layer2.buffer().data()[i]) {
            differs = true;
            break;
        }
    }
    CHECK(differs);
}

// Z-axis variation tests: with depth > 1 each z-slice must differ from the
// next. A 2D-only effect (the previous behaviour) produced identical slices —
// these tests pin the bug fixed.

// With depth > 1, adjacent and distant z-slices each render differently (3D noise, not a stack of identical 2D slices).
TEST_CASE("NoiseEffect produces different output per z-slice with depth > 1") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 8;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::NoiseEffect noise;
    layer.addChild(&noise);
    layer.onBuildState();
    layer.loop();

    const size_t sliceBytes = static_cast<size_t>(grid.width) * grid.height * 3;
    REQUIRE(layer.buffer().bytes() == sliceBytes * grid.depth);

    uint32_t h0 = hashSlice(layer.buffer().data() + 0 * sliceBytes, sliceBytes);
    uint32_t h1 = hashSlice(layer.buffer().data() + 1 * sliceBytes, sliceBytes);
    uint32_t h4 = hashSlice(layer.buffer().data() + 4 * sliceBytes, sliceBytes);
    CHECK(h0 != h1);  // adjacent slices differ
    CHECK(h0 != h4);  // distant slices differ
}

// Same z-slice variation requirement holds for Plasma — each depth plane renders differently.
TEST_CASE("PlasmaEffect produces different output per z-slice with depth > 1") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 8;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::PlasmaEffect plasma;
    layer.addChild(&plasma);
    layer.onBuildState();
    layer.loop();

    const size_t sliceBytes = static_cast<size_t>(grid.width) * grid.height * 3;
    REQUIRE(layer.buffer().bytes() == sliceBytes * grid.depth);

    uint32_t h0 = hashSlice(layer.buffer().data() + 0 * sliceBytes, sliceBytes);
    uint32_t h2 = hashSlice(layer.buffer().data() + 2 * sliceBytes, sliceBytes);
    uint32_t h7 = hashSlice(layer.buffer().data() + 7 * sliceBytes, sliceBytes);
    CHECK(h0 != h2);
    CHECK(h0 != h7);
}
