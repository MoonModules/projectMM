#include "doctest.h"
#include "light/FireEffect.h"
#include "light/GridLayout.h"

TEST_CASE("FireEffect allocates heat buffer when enabled") {
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layoutGroup.addChild(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::FireEffect fire;
    layer.addChild(&fire);

    layer.onAllocateMemory();
    CHECK(fire.dynamicBytes() == 16 * 16);
}

TEST_CASE("FireEffect renders non-zero buffer after enough sparks") {
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layoutGroup.addChild(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::FireEffect fire;
    fire.sparking = 255;
    layer.addChild(&fire);

    layer.onAllocateMemory();

    // Run several frames so sparks emerge and propagate
    bool hasNonZero = false;
    for (int frame = 0; frame < 50 && !hasNonZero; frame++) {
        layer.loop();
        auto& buf = layer.buffer();
        for (size_t i = 0; i < buf.bytes(); i++) {
            if (buf.data()[i] != 0) { hasNonZero = true; break; }
        }
    }
    CHECK(hasNonZero);
}

TEST_CASE("FireEffect frees heat buffer when disabled") {
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layoutGroup.addChild(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::FireEffect fire;
    layer.addChild(&fire);

    layer.onAllocateMemory();
    CHECK(fire.dynamicBytes() > 0);

    fire.setEnabled(false);
    fire.onAllocateMemory();
    CHECK(fire.dynamicBytes() == 0);
}
