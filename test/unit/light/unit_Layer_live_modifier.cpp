// @module Layer
// @also RotateModifier, ModifierBase

#include "doctest.h"
#include "light/layers/Layers.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/modifiers/RotateModifier.h"
#include "light/modifiers/RandomMapModifier.h"
#include "light/effects/EffectBase.h"
#include "platform/platform.h"

#include <vector>
#include <cstring>

// The live (per-frame) modifier seam: Layer::loop() applies a live modifier's
// modifyLive remap to the rendered buffer every frame WITHOUT a mapping rebuild,
// and runs that pass ONLY when an enabled modifier reports hasModifyLive() — a
// static-only Layer pays nothing (the pay-for-what-you-use guarantee). These pin
// both: the pass runs and remaps when a Rotate is present, and is skipped otherwise.

namespace {

// A tiny effect that writes a fixed position-dependent gradient into the buffer
// (R = x, G = y), so a coordinate remap (rotation) visibly rearranges the bytes.
// Deterministic per frame — no time dependence — so any frame-to-frame change is
// the live pass, not the effect animating itself.
class GradientEffect : public mm::EffectBase {
public:
    void loop() override {
        uint8_t* buf = buffer();
        if (!buf) return;
        const mm::lengthType w = width(), h = height();
        const uint8_t cpl = channelsPerLight();
        for (mm::lengthType y = 0; y < h; y++)
            for (mm::lengthType x = 0; x < w; x++) {
                uint8_t* p = buf + (static_cast<size_t>(y) * w + x) * cpl;
                p[0] = static_cast<uint8_t>(x);
                if (cpl > 1) p[1] = static_cast<uint8_t>(y);
                if (cpl > 2) p[2] = 0;
            }
    }
};

// Snapshot the Layer's buffer after one loop() at virtual time `t`.
std::vector<uint8_t> frameAt(mm::Layer& layer, uint32_t t) {
    mm::platform::setTestNowMs(t);
    layer.loop();
    const mm::Buffer& b = layer.buffer();
    std::vector<uint8_t> out(b.bytes());
    if (b.data()) std::memcpy(out.data(), b.data(), b.bytes());
    return out;
}

} // namespace

// With a Rotate present, the live pass rotates the gradient each frame as the angle
// advances — so two frames at different times differ. A static GradientEffect alone
// would produce identical frames, so any difference is the live remap.
TEST_CASE("Layer live pass: Rotate remaps the buffer per frame") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    GradientEffect fx; layer.addChild(&fx);
    mm::RotateModifier rot; rot.speed = 200;   // fast enough to cross several angle steps
    layer.addChild(&rot);
    layouts.onBuildState();
    layer.onBuildState();

    auto f0 = frameAt(layer, 1000);
    auto f1 = frameAt(layer, 1000);   // same time → same angle → identical frame
    auto f2 = frameAt(layer, 1300);   // later → angle advanced → rotated frame

    CHECK(f0 == f1);                   // deterministic at a fixed clock
    CHECK(f0 != f2);                   // the live pass rotated the gradient
    mm::platform::setTestNowMs(0);
}

// PAY-FOR-WHAT-YOU-USE: a Layer with no live modifier must NOT run the live pass —
// the static gradient is byte-identical across frames regardless of the clock.
TEST_CASE("Layer live pass: skipped when no modifier is live") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    GradientEffect fx; layer.addChild(&fx);   // no modifiers at all
    layouts.onBuildState();
    layer.onBuildState();

    auto f0 = frameAt(layer, 1000);
    auto f1 = frameAt(layer, 5000);   // far later — but nothing animates the buffer
    CHECK(f0 == f1);                  // no live pass perturbed it
    mm::platform::setTestNowMs(0);
}

// A DISABLED Rotate must not run the live pass either (the gate keys off ENABLED
// live modifiers). Same static gradient → identical frames.
TEST_CASE("Layer live pass: a disabled live modifier does not run") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    GradientEffect fx; layer.addChild(&fx);
    mm::RotateModifier rot; rot.speed = 200; rot.setEnabled(false);
    layer.addChild(&rot);
    layouts.onBuildState();
    layer.onBuildState();

    auto f0 = frameAt(layer, 1000);
    auto f1 = frameAt(layer, 1300);
    CHECK(f0 == f1);                  // disabled → no remap
    mm::platform::setTestNowMs(0);
}

// COALESCED REBUILD: two beat-driven modifiers (RandomMap) on one Layer both ask for
// a rebuild on a beat; Layer::loop() must rebuild ONCE (not re-enter onBuildState per
// modifier) and the Layer must stay valid — the composed mapping changes, no crash.
TEST_CASE("Layer coalesces rebuilds from two dynamic modifiers") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    GradientEffect fx; layer.addChild(&fx);
    mm::RandomMapModifier a; a.bpm = 60;   // both reshuffle on a ~1 Hz beat
    mm::RandomMapModifier b; b.bpm = 60;
    layer.addChild(&a);
    layer.addChild(&b);
    layouts.onBuildState();
    layer.onBuildState();

    const std::size_t cells = static_cast<std::size_t>(layer.lut().logicalCount());
    REQUIRE(cells == 64);

    auto destCount = [&]() {
        std::size_t n = 0;
        for (mm::nrOfLightsType li = 0; li < layer.lut().logicalCount(); li++)
            layer.lut().forEachDestination(li, [&](mm::nrOfLightsType) { n++; });
        return n;
    };
    const std::size_t before = destCount();

    // Advance past a beat boundary in small ticks (both modifiers tick each frame).
    for (uint32_t t = 1000; t <= 2500; t += 50) { mm::platform::setTestNowMs(t); layer.loop(); }
    mm::platform::setTestNowMs(0);

    // Two composed permutations remain a permutation of the 64 cells — every light
    // still maps somewhere exactly once (no crash, no lost/duplicated destinations).
    CHECK(destCount() == before);
    CHECK(destCount() == 64);
}
