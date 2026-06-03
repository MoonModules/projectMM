// @module ParticlesEffect

#include "doctest.h"
#include "light/effects/ParticlesEffect.h"
#include "light/layouts/GridLayout.h"

// The trail buffer sizes to width × height × 3 bytes (one RGB per cell, used to fade existing pixels).
TEST_CASE("ParticlesEffect allocates trail buffer when enabled") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::ParticlesEffect particles;
    layer.addChild(&particles);

    layer.onBuildState();
    CHECK(particles.dynamicBytes() == 16 * 16 * 3);
}

// A single tick is enough to paint particles into the buffer.
TEST_CASE("ParticlesEffect renders non-zero buffer after one frame") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::ParticlesEffect particles;
    layer.addChild(&particles);

    layer.onBuildState();
    layer.loop();

    auto& buf = layer.buffer();
    bool hasNonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { hasNonZero = true; break; }
    }
    CHECK(hasNonZero);
}

// Disabling the effect releases the trail buffer (dynamicBytes returns to 0).
TEST_CASE("ParticlesEffect frees trail buffer when disabled") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::ParticlesEffect particles;
    layer.addChild(&particles);

    layer.onBuildState();
    CHECK(particles.dynamicBytes() > 0);

    particles.setEnabled(false);
    particles.onBuildState();
    CHECK(particles.dynamicBytes() == 0);
}
