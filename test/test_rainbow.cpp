#include "doctest.h"
#include "light/effects/RainbowEffect.h"
#include "light/layouts/GridLayout.h"

TEST_CASE("RainbowEffect writes non-zero RGB data to buffer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    layer.addChild(&rainbow);

    layer.onBuildState();

    // Simulate a frame at elapsed=0 (effect uses platform::millis())
    layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 16);

    // At least some lights should be non-zero (rainbow produces color everywhere)
    bool hasNonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { hasNonZero = true; break; }
    }
    CHECK(hasNonZero);
}

TEST_CASE("RainbowEffect pixel 0,0 produces valid RGB") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    layer.addChild(&rainbow);

    layer.onBuildState();
    layer.loop();

    auto* data = layer.buffer().data();
    // Pixel (0,0): hue depends on elapsed, but should always have full saturation/value
    // At least one channel should be 255 (full value with hsvToRgb at v=255)
    uint8_t r = data[0], g = data[1], b = data[2];
    CHECK((r > 0 || g > 0 || b > 0));
    // With hsvToRgb(h, 255, 255), the max channel is always 255
    CHECK((r == 255 || g == 255 || b == 255));
}

TEST_CASE("RainbowEffect different positions produce different hues") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    layer.addChild(&rainbow);

    layer.onBuildState();
    layer.loop();

    auto* data = layer.buffer().data();
    // Pixel (0,0) and pixel (8,8) should have different colors
    uint8_t r0 = data[0], g0 = data[1], b0 = data[2];
    size_t idx88 = (8 * 16 + 8) * 3;
    uint8_t r1 = data[idx88], g1 = data[idx88 + 1], b1 = data[idx88 + 2];
    CHECK((r0 != r1 || g0 != g1 || b0 != b1));
}
