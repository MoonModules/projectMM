#include "doctest.h"
#include "light/effects/MetaballsEffect.h"
#include "light/layouts/GridLayout.h"

TEST_CASE("MetaballsEffect writes non-zero RGB data to buffer") {
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layoutGroup.addChild(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::MetaballsEffect metaballs;
    layer.addChild(&metaballs);

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

TEST_CASE("MetaballsEffect produces spatial variation") {
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 32;
    grid.height = 32;
    grid.depth = 1;
    layoutGroup.addChild(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::MetaballsEffect metaballs;
    layer.addChild(&metaballs);

    layer.onAllocateMemory();
    layer.loop();

    auto* data = layer.buffer().data();
    uint8_t r0 = data[0], g0 = data[1], b0 = data[2];
    size_t lastIdx = (32 * 32 - 1) * 3;
    uint8_t r1 = data[lastIdx], g1 = data[lastIdx + 1], b1 = data[lastIdx + 2];
    CHECK((r0 != r1 || g0 != g1 || b0 != b1));
}
