// @module FireEffect

#include "doctest.h"
#include "light/effects/FireEffect.h"
#include "light/layouts/GridLayout.h"

// On a 16×16 grid the heat buffer sizes to width × height bytes (one byte of heat per cell).
TEST_CASE("FireEffect allocates heat buffer when enabled") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::FireEffect fire;
    layer.addChild(&fire);

    layer.onBuildState();
    CHECK(fire.dynamicBytes() == 16 * 16);
}

// With sparking at max, the buffer contains non-zero pixels within 50 frames (sparks emerge and propagate).
TEST_CASE("FireEffect renders non-zero buffer after enough sparks") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::FireEffect fire;
    fire.sparking = 255;
    layer.addChild(&fire);

    layer.onBuildState();

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

// Disabling the effect releases its heat buffer back (dynamicBytes drops to 0).
TEST_CASE("FireEffect frees heat buffer when disabled") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::FireEffect fire;
    layer.addChild(&fire);

    layer.onBuildState();
    CHECK(fire.dynamicBytes() > 0);

    // Disable + rebuild via the parent's lifecycle — same path the
    // production scheduler uses, not a direct child call which would
    // bypass the propagation tested elsewhere.
    fire.setEnabled(false);
    layer.onBuildState();
    CHECK(fire.dynamicBytes() == 0);
}
