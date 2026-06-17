// @module RandomMapModifier
// @also Layer

#include "doctest.h"
#include "light/modifiers/RandomMapModifier.h"
#include "light/layers/Layers.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"

#include <vector>

// RandomMapModifier remaps every light to another light — a 1:1 permutation — and
// reshuffles on a bpm timer. These tests pin the mapping properties directly via
// mapToPhysical (no Layer needed): the permutation is a true bijection, deterministic
// for a fixed generation, changes on reshuffle, and degrades safely on an empty grid.

namespace {

// Map a single light; returns its destination index (outCount is always 1 for a remap).
mm::nrOfLightsType mapOne(mm::RandomMapModifier& m,
                          mm::lengthType x, mm::lengthType y, mm::lengthType z,
                          mm::lengthType w, mm::lengthType h, mm::lengthType d) {
    mm::nrOfLightsType phys[4];
    mm::nrOfLightsType count = 0;
    m.mapToPhysical(x, y, z, w, h, d, phys, count, 4);
    CHECK(count == 1);
    return phys[0];
}

// Collect the destination of every light in a w×h×d grid, in index order.
std::vector<mm::nrOfLightsType> mapAll(mm::RandomMapModifier& m,
                                       mm::lengthType w, mm::lengthType h, mm::lengthType d) {
    std::vector<mm::nrOfLightsType> out;
    for (mm::lengthType z = 0; z < d; z++)
        for (mm::lengthType y = 0; y < h; y++)
            for (mm::lengthType x = 0; x < w; x++)
                out.push_back(mapOne(m, x, y, z, w, h, d));
    return out;
}

} // namespace

// A remap doesn't resize the logical box.
TEST_CASE("RandomMapModifier logicalDimensions are identity") {
    mm::RandomMapModifier m;
    mm::lengthType lw, lh, ld;
    m.logicalDimensions(64, 32, 4, lw, lh, ld);
    CHECK(lw == 64);
    CHECK(lh == 32);
    CHECK(ld == 4);
}

TEST_CASE("RandomMapModifier maxMultiplier is 1") {
    mm::RandomMapModifier m;
    CHECK(m.maxMultiplier() == 1);
}

// The core property: the mapping is a true bijection over [0, w*h*d) — every
// destination index appears exactly once (no gaps, no duplicates).
TEST_CASE("RandomMapModifier is a bijection (every pixel mapped once)") {
    mm::RandomMapModifier m;
    const mm::lengthType w = 8, h = 8, d = 1;
    const mm::nrOfLightsType n = static_cast<mm::nrOfLightsType>(w) * h * d;
    auto dests = mapAll(m, w, h, d);

    REQUIRE(dests.size() == n);
    std::vector<int> seen(n, 0);
    for (auto dst : dests) {
        REQUIRE(dst < n);          // in range
        seen[dst]++;
    }
    for (mm::nrOfLightsType i = 0; i < n; i++)
        CHECK(seen[i] == 1);       // each destination used exactly once
}

// A fresh modifier with the same generation produces the same permutation
// (deterministic seed → reproducible, which is what makes it testable).
TEST_CASE("RandomMapModifier is deterministic for a fixed generation") {
    mm::RandomMapModifier a, b;
    auto da = mapAll(a, 8, 8, 1);
    auto db = mapAll(b, 8, 8, 1);
    CHECK(da == db);
}

// Reshuffling (a beat) changes the mapping, and the result is still a bijection.
TEST_CASE("RandomMapModifier reshuffle changes the mapping, stays a bijection") {
    mm::RandomMapModifier m;
    const mm::lengthType w = 8, h = 8, d = 1;
    const mm::nrOfLightsType n = static_cast<mm::nrOfLightsType>(w) * h * d;

    auto before = mapAll(m, w, h, d);
    m.reshuffle();                 // what loop() does on a beat
    auto after = mapAll(m, w, h, d);

    CHECK(before != after);        // a genuinely different permutation

    std::vector<int> seen(n, 0);
    for (auto dst : after) { REQUIRE(dst < n); seen[dst]++; }
    for (mm::nrOfLightsType i = 0; i < n; i++) CHECK(seen[i] == 1);  // still a bijection
}

// Robustness: an empty (0×0×0) grid must not crash — maxOut 0 yields no destination.
TEST_CASE("RandomMapModifier tolerates an empty grid") {
    mm::RandomMapModifier m;
    mm::nrOfLightsType phys[4];
    mm::nrOfLightsType count = 7;          // sentinel
    m.mapToPhysical(0, 0, 0, 0, 0, 0, phys, count, 0);
    CHECK(count == 0);                     // nothing emitted, no crash
}

// A resize (different box count) rebuilds the permutation to the new size, still a bijection.
TEST_CASE("RandomMapModifier rebuilds on a grid resize") {
    mm::RandomMapModifier m;
    auto small = mapAll(m, 4, 4, 1);       // 16 lights
    CHECK(small.size() == 16);

    auto big = mapAll(m, 8, 8, 1);         // 64 lights — forces a resize+rebuild
    CHECK(big.size() == 64);
    const mm::nrOfLightsType n = 64;
    std::vector<int> seen(n, 0);
    for (auto dst : big) { REQUIRE(dst < n); seen[dst]++; }
    for (mm::nrOfLightsType i = 0; i < n; i++) CHECK(seen[i] == 1);
}

// loop() timer behaviour, exercised through a real Layer (the modifier reads the
// Layer's elapsed() clock and calls its onBuildState() on a beat). We observe the
// MODIFIER'S MAPPING (mapToPhysical) before vs after a timed run — not the
// rendered buffer, which an animating effect would change on its own and mask the
// signal. A beat reshuffles the permutation (mapping differs); bpm=0 freezes it
// (mapping identical) however far time advances. Mirrors the Layer + test-clock
// pattern in unit_Layer_phase_animation.
namespace {
// Build a Layer with the modifier, run layer.loop() across total_ms of virtual
// time, and return whether the modifier's mapping changed over the run.
bool mappingChangesOverMs(uint8_t bpm, int total_ms) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::RandomMapModifier mod;
    mod.bpm = bpm;
    layer.addChild(&mod);
    layouts.onBuildState();
    layer.onBuildState();

    const auto before = mapAll(mod, 8, 8, 1);
    uint32_t now = 1000;
    for (int e = 0; e <= total_ms; e += 50) {
        mm::platform::setTestNowMs(now + e);
        layer.loop();   // sets the Layer clock, then ticks the modifier's loop()
    }
    const auto after = mapAll(mod, 8, 8, 1);
    mm::platform::setTestNowMs(0);
    return before != after;
}
} // namespace

TEST_CASE("RandomMapModifier loop() reshuffles on a beat (bpm 60 ≈ 1/s)") {
    // bpm 60 → one beat per 1000ms; 1500ms spans a boundary, so the mapping changes.
    CHECK(mappingChangesOverMs(60, 1500) == true);
}

TEST_CASE("RandomMapModifier loop() with bpm 0 never reshuffles (frozen)") {
    // bpm 0 → no beat ever; the permutation stays put no matter how far time runs.
    CHECK(mappingChangesOverMs(0, 5000) == false);
}
