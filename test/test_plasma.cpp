#include "doctest.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/layouts/GridLayout.h"

TEST_CASE("PlasmaEffect writes non-zero RGB data to buffer") {
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layoutGroup.addChild(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::PlasmaEffect plasma;
    layer.addChild(&plasma);

    layer.onAllocateMemory();
    layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);

    bool hasNonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { hasNonZero = true; break; }
    }
    CHECK(hasNonZero);
}

TEST_CASE("PlasmaEffect produces spatial variation") {
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layoutGroup.addChild(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::PlasmaEffect plasma;
    layer.addChild(&plasma);

    layer.onAllocateMemory();
    layer.loop();

    auto* data = layer.buffer().data();
    uint8_t r0 = data[0], g0 = data[1], b0 = data[2];
    size_t lastIdx = (16 * 16 - 1) * 3;
    uint8_t r1 = data[lastIdx], g1 = data[lastIdx + 1], b1 = data[lastIdx + 2];
    CHECK((r0 != r1 || g0 != g1 || b0 != b1));
}

TEST_CASE("PlasmaEffect produces different output than NoiseEffect") {
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layoutGroup.addChild(&grid);

    mm::Layer layer1;
    layer1.setLayoutGroup(&layoutGroup);
    layer1.setChannelsPerLight(3);
    mm::PlasmaEffect plasma;
    layer1.addChild(&plasma);
    layer1.onAllocateMemory();
    layer1.loop();

    mm::Layer layer2;
    layer2.setLayoutGroup(&layoutGroup);
    layer2.setChannelsPerLight(3);
    mm::NoiseEffect noise;
    layer2.addChild(&noise);
    layer2.onAllocateMemory();
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
