// @module AuroraEffect
// @also polar, oscillators, noise

// Aurora is a composition rather than a picture of anything, so what is pinned here is that the
// composition behaves: that curtains appear and are distinct rather than an even haze, that the
// contrast control decides how much of the field lights, that the layers move independently, and
// that raising the cost knob costs something. The golden pins the plumbing; these pin the look.

#include "doctest.h"
#include "light/effects/AuroraEffect.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "platform/platform.h"

#include <cstdint>
#include <vector>

using namespace mm;

namespace {

/// Render Aurora with `configure` applied, and return the final frame.
template <typename F>
std::vector<uint8_t> render(lengthType w, lengthType h, F configure, uint16_t frames = 60, uint32_t startMs = 1000) {
    platform::setTestNowMs(startMs);
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    AuroraEffect effect;
    configure(effect);
    grid.width = w; grid.height = h; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&effect);
    layer.applyState();
    for (uint16_t f = 0; f < frames; f++) {
        platform::setTestNowMs(startMs + static_cast<uint32_t>(f) * 20);
        layer.tick();
    }
    auto& buf = layer.buffer();
    return std::vector<uint8_t>(buf.data(), buf.data() + buf.bytes());
}

/// The share of channels that are lit at all.
double litShare(const std::vector<uint8_t>& f) {
    std::size_t lit = 0;
    for (uint8_t v : f) lit += v > 8 ? 1 : 0;
    return double(lit) / f.size();
}

/// How many channels differ between two frames.
std::size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); i++) n += a[i] != b[i] ? 1 : 0;
    return n;
}

}  // namespace

TEST_CASE("the contrast control decides how much of the field lights up") {
    // This is what makes Aurora curtains rather than cloud: a high window leaves only the peaks of
    // the field visible, a low one lets most of it through.
    const auto sharp = render(32, 32, [](AuroraEffect& e) { e.contrast = 200; });
    const auto soft  = render(32, 32, [](AuroraEffect& e) { e.contrast = 40; });
    CHECK(litShare(sharp) < litShare(soft));
    CHECK(litShare(sharp) < 0.5);      // a minority of the panel is curtain
    CHECK(litShare(soft) > 0.5);       // and the field itself covers it
}

TEST_CASE("curtains appear rather than an even wash of light") {
    // A field that lit every pixel equally would be a blur. The frame must have real dark and real
    // bright in it at the default contrast.
    const auto frame = render(32, 32, [](AuroraEffect&) {});
    uint8_t lo = 255, hi = 0;
    for (uint8_t v : frame) { lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
    CHECK(lo < 16);                    // somewhere is genuinely dark
    CHECK(hi > 200);                   // somewhere is genuinely bright
}

TEST_CASE("the composition keeps moving, and no two moments look alike") {
    const auto early = render(32, 32, [](AuroraEffect&) {}, 30);
    const auto late  = render(32, 32, [](AuroraEffect&) {}, 240);
    CHECK(differing(early, late) > early.size() / 8);
}

TEST_CASE("a still speed holds the picture, so a fixture can be frozen") {
    const auto a = render(32, 32, [](AuroraEffect& e) { e.speed = 0; }, 30);
    const auto b = render(32, 32, [](AuroraEffect& e) { e.speed = 0; }, 240);
    CHECK(differing(a, b) == 0);
}

TEST_CASE("each layer adds structure, so the cost knob buys something") {
    // One layer is a single field; three layers interfere. If more layers changed nothing, the
    // effect's main control would be paying for nothing.
    const auto one   = render(32, 32, [](AuroraEffect& e) { e.layers = 1; });
    const auto three = render(32, 32, [](AuroraEffect& e) { e.layers = 3; });
    CHECK(differing(one, three) > one.size() / 4);
}

TEST_CASE("the kaleidoscope fold makes the composition symmetric") {
    // Folding the angle into wedges must actually repeat the field around the center.
    const auto folded = render(33, 33, [](AuroraEffect& e) { e.segments = 4; e.twist = 0; });
    // Sample a ring of pixels and check the fold repeats: opposite wedges carry the same field.
    const std::size_t w = 33;
    std::size_t same = 0, total = 0;
    for (std::size_t k = 4; k < 16; k++) {
        const std::size_t left  = (16 * w + (16 - k)) * 3;
        const std::size_t right = (16 * w + (16 + k)) * 3;
        const int d = folded[left] > folded[right] ? folded[left] - folded[right]
                                                   : folded[right] - folded[left];
        same += d < 32 ? 1 : 0;
        total++;
    }
    CHECK(same * 2 >= total);          // the mirrored halves largely agree
}

TEST_CASE("Aurora renders on a grid too small to have a center") {
    // Robustness: any size, any order. A 1x1 and a 2x1 grid must not fault or divide by zero.
    const auto tiny = render(1, 1, [](AuroraEffect&) {}, 5);
    CHECK(tiny.size() == 3);
    const auto strip = render(2, 1, [](AuroraEffect&) {}, 5);
    CHECK(strip.size() == 6);
}

TEST_CASE("Aurora renders the same picture whether or not the polar table is available") {
    // The table is an optimization, not part of the look: a device that cannot spare the memory
    // gets the same composition.
    const auto tabled = render(32, 32, [](AuroraEffect& e) { e.widePolarTable = true; });
    const auto exact  = render(32, 32, [](AuroraEffect& e) { e.usePolarTable = false; });
    CHECK(differing(tabled, exact) == 0);
}
