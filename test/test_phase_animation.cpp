#include "doctest.h"
#include "light/layers/Layers.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/CheckerboardEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/NoiseEffect.h"
#include "platform/platform.h"

#include <vector>

// Regression: per-tick phase accumulators computed `dt * bpm * 256 / 60000`,
// which truncates to 0 on desktop where dt ≈ 0..1 ms — the animations froze.
// Fix accumulates the raw (dt*bpm) numerator and only divides at the read site.
// These tests pin animation across a short time gap for every affected effect.

namespace {

// Run the effect through a sequence of short ticks totalling `total_ms`, take
// snapshots along the way, and return true if at least two snapshots differ.
// The robustness matters because some effects' visible state flips on parity of
// phaseCell (CheckerboardEffect) — a single before/after pair can land on the
// same parity by accident. Multiple samples across a longer interval can't.
template <typename Effect>
bool animates_over_ms(int total_ms) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    // 32×32 covers CheckerboardEffect's default cell_size=4 (8×8 cells) and is
    // small enough to keep the test fast.
    grid.width = 32; grid.height = 32; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    Effect e;
    layer.addChild(&e);
    layouts.onBuildState();
    layer.onBuildState();

    layer.loop();
    std::vector<uint8_t> baseline(layer.buffer().data(), layer.buffer().data() + layer.buffer().bytes());
    const int step_ms = 20;
    for (int elapsed = 0; elapsed < total_ms; elapsed += step_ms) {
        mm::platform::delayMs(step_ms);
        layer.loop();
        std::vector<uint8_t> now(layer.buffer().data(), layer.buffer().data() + layer.buffer().bytes());
        if (now != baseline) return true;
    }
    return false;
}

} // namespace

TEST_CASE("MetaballsEffect animates over a 100ms gap (desktop dt ≈ 0..1ms)") {
    CHECK(animates_over_ms<mm::MetaballsEffect>(100));
}

TEST_CASE("CheckerboardEffect animates over a 100ms gap") {
    CHECK(animates_over_ms<mm::CheckerboardEffect>(100));
}

TEST_CASE("LavaLampEffect animates over a 100ms gap") {
    CHECK(animates_over_ms<mm::LavaLampEffect>(100));
}

TEST_CASE("SpiralEffect animates over a 100ms gap") {
    CHECK(animates_over_ms<mm::SpiralEffect>(100));
}

// Replace path: swap one effect for another mid-flight (same shape as
// HttpServerModule::handleReplaceModule) and confirm the new effect animates.
TEST_CASE("Replacing an effect at runtime: new effect still animates") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    auto* noise = new mm::NoiseEffect();
    layer.addChild(noise);

    layouts.onBuildState();
    layer.onBuildState();
    layer.loop();

    auto* fresh = new mm::MetaballsEffect();
    mm::MoonModule* old = layer.replaceChildAt(0, fresh);
    fresh->onBuildControls();
    fresh->setup();
    fresh->onBuildState();
    if (old) { old->teardown(); delete old; }
    layer.onBuildState();

    layer.loop();
    std::vector<uint8_t> first(layer.buffer().data(), layer.buffer().data() + layer.buffer().bytes());
    mm::platform::delayMs(100);
    layer.loop();
    std::vector<uint8_t> second(layer.buffer().data(), layer.buffer().data() + layer.buffer().bytes());
    CHECK_FALSE_MESSAGE(first == second, "Replaced Metaballs frozen across 100ms gap");
}
