// @module Layers
// @also Layer

#include "doctest.h"
#include "light/layers/Layers.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/CheckerboardEffect.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/drivers/Drivers.h"
#include "platform/platform.h"

#include <cstring>

// RAII guard that restores the platform test clock on scope exit even if a
// REQUIRE/CHECK throws — without this, a mid-test failure would leave the
// global setTestNowMs override in place and pollute later test cases.
struct ClockGuard {
    ~ClockGuard() { mm::platform::setTestNowMs(0); }
};

// The Layers container is a thin pass-through with one child Layer: behaviour
// must match what a bare Layer produced before the shape change. These tests
// pin that — anyone changing Layers::loop() will know immediately if the
// single-child path stops being a no-op.
//
// Composition (alpha-blend across multiple Layers) is not yet wired — the
// second test exercises the multi-Layer path enough to confirm each child
// Layer's loop runs and writes a populated buffer. Once composition lands,
// add a third test asserting the composed output blends as documented.

// A Layers container with one child Layer must produce the same output as that Layer used directly (no-op container).
TEST_CASE("Layers with one Layer produces the same output as a bare Layer") {
    // Pin virtual time so both Layer paths read the same elapsed value from
    // RainbowEffect's platform::millis() phase. Without this, the two loop()
    // calls land microseconds apart on the real clock and Rainbow's hue rotates
    // between them — making byte-exact comparison impossible (the structural
    // compare this test used to do hid the actual contract).
    mm::platform::setTestNowMs(1000);
    ClockGuard clockGuard;  // restores setTestNowMs(0) even if a REQUIRE below fails

    // --- Reference: bare Layer (no Layers container) ---
    mm::Layouts layoutsA;
    mm::GridLayout gridA;
    gridA.width = 16;
    gridA.height = 16;
    gridA.depth = 1;
    layoutsA.addChild(&gridA);

    mm::Layer bareLayer;
    bareLayer.setLayouts(&layoutsA);
    bareLayer.setChannelsPerLight(3);
    mm::RainbowEffect bareEffect;
    bareLayer.addChild(&bareEffect);
    bareLayer.onBuildState();
    bareLayer.loop();

    // --- New shape: Layers container wrapping one Layer ---
    mm::Layouts layoutsB;
    mm::GridLayout gridB;
    gridB.width = 16;
    gridB.height = 16;
    gridB.depth = 1;
    layoutsB.addChild(&gridB);

    mm::Layers layersContainer;
    mm::Layer childLayer;
    childLayer.setChannelsPerLight(3);
    layersContainer.addChild(&childLayer);
    layersContainer.setLayouts(&layoutsB);  // propagates to childLayer
    mm::RainbowEffect childEffect;
    childLayer.addChild(&childEffect);

    layersContainer.onBuildState();
    // Layers::loop runs each child Layer in order; for the single-child case
    // that's exactly one bareLayer.loop() equivalent.
    layersContainer.loop();

    // --- Both buffers must be byte-identical at the same elapsed time ---
    auto& bufA = bareLayer.buffer();
    auto& bufB = childLayer.buffer();
    REQUIRE(bufA.bytes() == bufB.bytes());
    REQUIRE(bufA.bytes() == static_cast<size_t>(16 * 16 * 3));
    CHECK(std::memcmp(bufA.data(), bufB.data(), bufA.bytes()) == 0);
    // clockGuard restores setTestNowMs(0) on scope exit
}

// With two child Layers, each one's loop() runs and writes its own buffer (the container iterates all enabled children).
TEST_CASE("Layers with two Layers: each child Layer's loop runs and writes its buffer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layers layersContainer;

    mm::Layer layerA;
    layerA.setChannelsPerLight(3);
    mm::RainbowEffect effectA;
    layerA.addChild(&effectA);

    mm::Layer layerB;
    layerB.setChannelsPerLight(3);
    mm::CheckerboardEffect effectB;
    layerB.addChild(&effectB);

    layersContainer.addChild(&layerA);
    layersContainer.addChild(&layerB);
    layersContainer.setLayouts(&layouts);
    layersContainer.onBuildState();
    layersContainer.loop();

    // Both child Layer buffers must be populated — each Layer renders its own
    // buffer here; the Drivers composite of those buffers is pinned by the
    // "Drivers composites two enabled Layers" case below. (Checkerboard with
    // default controls writes a checker pattern; Rainbow writes a hue gradient.)
    auto& bufA = layerA.buffer();
    auto& bufB = layerB.buffer();
    REQUIRE(bufA.bytes() == static_cast<size_t>(8 * 8 * 3));
    REQUIRE(bufB.bytes() == static_cast<size_t>(8 * 8 * 3));

    bool aHasNonZero = false, bHasNonZero = false;
    for (size_t i = 0; i < bufA.bytes(); i++) if (bufA.data()[i] != 0) { aHasNonZero = true; break; }
    for (size_t i = 0; i < bufB.bytes(); i++) if (bufB.data()[i] != 0) { bHasNonZero = true; break; }
    CHECK_MESSAGE(aHasNonZero, "Layer A (Rainbow) wrote no pixels");
    CHECK_MESSAGE(bHasNonZero, "Layer B (Checkerboard) wrote no pixels");
}

// A minimal driver that just records the source buffer it's handed each tick,
// so a test can inspect the composited output without a real network/LED sink.
class CaptureDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer* buf) override { src_ = buf; }
    mm::Buffer* src_ = nullptr;
};

// Multi-layer composition: Drivers blends ≥2 enabled Layers into its own output
// buffer and hands THAT to drivers (not a single Layer's buffer). Bottom layer
// overwrites; top layer blends per its blendMode/opacity. This is the end-to-end
// pin for the composite loop in Drivers::loop.
TEST_CASE("Drivers composites two enabled Layers into one output buffer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4; grid.height = 1; grid.depth = 1;   // 4 lights, dense (no LUT)
    layouts.addChild(&grid);

    mm::Layers layersContainer;
    // Bottom layer: a checkerboard base.
    mm::Layer bottom; bottom.setChannelsPerLight(3);
    mm::CheckerboardEffect base; bottom.addChild(&base);
    // Top layer: rainbow, additive at full opacity → bottom + top, clamped.
    mm::Layer top; top.setChannelsPerLight(3);
    mm::RainbowEffect over; top.addChild(&over);
    top.blendMode = 1;   // additive
    top.opacity = 255;

    layersContainer.addChild(&bottom);
    layersContainer.addChild(&top);
    layersContainer.setLayouts(&layouts);

    mm::Drivers drivers;
    CaptureDriver cap;
    drivers.addChild(&cap);
    drivers.setLayers(&layersContainer);

    layersContainer.onBuildState();
    drivers.onBuildState();      // sizes + allocates the composite output buffer
    layersContainer.loop();      // both layers render their own buffers
    drivers.loop();              // composite into outputBuffer_, hand it to cap

    REQUIRE(layersContainer.enabledLayerCount() == 2);
    // The driver was handed the composite buffer (4 lights × 3ch), not a raw layer.
    REQUIRE(cap.src_ != nullptr);
    REQUIRE(cap.src_->bytes() == static_cast<size_t>(4 * 3));

    // The composite must equal additive(bottom, top) per channel, clamped — i.e.
    // for every byte, output >= bottom (top only adds) and output >= top's contribution.
    auto& outBuf = *cap.src_;
    auto& botBuf = bottom.buffer();
    auto& topBuf = top.buffer();
    REQUIRE(botBuf.bytes() == outBuf.bytes());
    REQUIRE(topBuf.bytes() == outBuf.bytes());
    bool sawSum = false;
    for (size_t i = 0; i < outBuf.bytes(); i++) {
        uint16_t expect = static_cast<uint16_t>(botBuf.data()[i]) + topBuf.data()[i];
        if (expect > 255) expect = 255;
        CHECK(outBuf.data()[i] == static_cast<uint8_t>(expect));
        if (botBuf.data()[i] && topBuf.data()[i]) sawSum = true;
    }
    CHECK_MESSAGE(sawSum, "expected at least one light where both layers contribute (proves real compositing)");
}

// Disabling the top layer drops cleanly to the single (bottom) layer — no crash,
// the driver now sees the bottom layer's content. Pins the robustness path.
TEST_CASE("Drivers composition drops to single layer when one is disabled") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4; grid.height = 1; grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layers layersContainer;
    mm::Layer bottom; bottom.setChannelsPerLight(3);
    mm::CheckerboardEffect base; bottom.addChild(&base);
    mm::Layer top; top.setChannelsPerLight(3);
    mm::RainbowEffect over; top.addChild(&over);
    layersContainer.addChild(&bottom);
    layersContainer.addChild(&top);
    layersContainer.setLayouts(&layouts);

    mm::Drivers drivers;
    CaptureDriver cap;
    drivers.addChild(&cap);
    drivers.setLayers(&layersContainer);

    top.setEnabled(false);             // only the bottom layer remains
    layersContainer.onBuildState();
    drivers.onBuildState();
    layersContainer.loop();
    drivers.loop();

    CHECK(layersContainer.enabledLayerCount() == 1);
    REQUIRE(cap.src_ != nullptr);      // driver still has a valid buffer, no crash
    REQUIRE(cap.src_->bytes() == static_cast<size_t>(4 * 3));
}

// Drivers' composition/output-buffer allocation contract (architecture.md §
// Adaptive allocation). The driver output buffer exists ONLY when the pipeline
// must blend into physical space; otherwise the lone layer's buffer is handed to
// drivers directly (zero-copy). dynamicBytes() reflects outputBuffer_.bytes(), so
// it's 0 ⇔ no buffer. Pins all three cases in one place:
//   1. one identity (no-LUT) layer  → NO output buffer (zero-copy)
//   2. two enabled layers           → output buffer (must composite)
//   3. one layer WITH a LUT         → output buffer (must map logical→physical)
TEST_CASE("Drivers allocates the output buffer only when compositing or mapping is needed") {
    // --- Case 1: a single identity (dense-grid, no-LUT) layer → no output buffer ---
    {
        mm::Layouts layouts; mm::GridLayout grid;
        grid.width = 8; grid.height = 8; grid.depth = 1;
        layouts.addChild(&grid);
        mm::Layers layers;
        mm::Layer only; only.setChannelsPerLight(3);
        mm::CheckerboardEffect eff; only.addChild(&eff);
        layers.addChild(&only);
        layers.setLayouts(&layouts);
        mm::Drivers drivers; CaptureDriver cap; drivers.addChild(&cap);
        drivers.setLayers(&layers);
        layers.onBuildState(); drivers.onBuildState();

        CHECK_FALSE(only.lut().hasLUT());            // dense grid → identity, no LUT
        CHECK(layers.enabledLayerCount() == 1);
        CHECK(drivers.dynamicBytes() == 0);          // NO output buffer allocated
        REQUIRE(cap.src_ != nullptr);                // driver reads the layer buffer directly
        CHECK(cap.src_ == &only.buffer());           // zero-copy: it's the layer's own buffer
    }

    // --- Case 2: two enabled layers → output buffer (must composite) ---
    {
        mm::Layouts layouts; mm::GridLayout grid;
        grid.width = 8; grid.height = 8; grid.depth = 1;
        layouts.addChild(&grid);
        mm::Layers layers;
        mm::Layer a; a.setChannelsPerLight(3); mm::CheckerboardEffect ea; a.addChild(&ea);
        mm::Layer b; b.setChannelsPerLight(3); mm::RainbowEffect eb; b.addChild(&eb);
        layers.addChild(&a); layers.addChild(&b);
        layers.setLayouts(&layouts);
        mm::Drivers drivers; CaptureDriver cap; drivers.addChild(&cap);
        drivers.setLayers(&layers);
        layers.onBuildState(); drivers.onBuildState();

        CHECK(layers.enabledLayerCount() == 2);
        CHECK(drivers.dynamicBytes() == static_cast<size_t>(8 * 8 * 3));  // output buffer allocated
        REQUIRE(cap.src_ != nullptr);
        CHECK(cap.src_ != &a.buffer());              // driver reads the composite, not a raw layer
    }

    // --- Case 3: a single layer WITH a LUT (a mirror modifier) → output buffer ---
    {
        mm::Layouts layouts; mm::GridLayout grid;
        grid.width = 8; grid.height = 8; grid.depth = 1;
        layouts.addChild(&grid);
        mm::Layers layers;
        mm::Layer only; only.setChannelsPerLight(3);
        mm::CheckerboardEffect eff; only.addChild(&eff);
        mm::MultiplyModifier mirror; mirror.mirrorX = true; only.addChild(&mirror);
        layers.addChild(&only);
        layers.setLayouts(&layouts);
        mm::Drivers drivers; CaptureDriver cap; drivers.addChild(&cap);
        drivers.setLayers(&layers);
        layers.onBuildState(); drivers.onBuildState();

        CHECK(only.lut().hasLUT());                  // mirror modifier → a real LUT
        CHECK(layers.enabledLayerCount() == 1);
        CHECK(drivers.dynamicBytes() > 0);           // output buffer allocated (map target)
        REQUIRE(cap.src_ != nullptr);
        CHECK(cap.src_ != &only.buffer());           // driver reads the mapped output, not the logical buffer
    }

    // --- Case 4: a live layer is DISABLED → drivers transition to idle, no stale buffer ---
    // The real-world sequence: a frame is published with the layer enabled, then the
    // user disables it and the pipeline rebuilds. activeLayer() still surfaces the
    // (now disabled) layer so geometry stays queryable, but output selection must use
    // the *enabled* source — with none, the driver's source buffer goes null so it
    // emits nothing instead of re-sending the last frame off the disabled layer.
    {
        mm::Layouts layouts; mm::GridLayout grid;
        grid.width = 8; grid.height = 8; grid.depth = 1;
        layouts.addChild(&grid);
        mm::Layers layers;
        mm::Layer only; only.setChannelsPerLight(3);
        mm::CheckerboardEffect eff; only.addChild(&eff);
        // A LUT modifier so the pre-fix bug would route through the output path —
        // proves the disabled gate, not just the no-LUT zero-copy branch.
        mm::MultiplyModifier mirror; mirror.mirrorX = true; only.addChild(&mirror);
        layers.addChild(&only);
        layers.setLayouts(&layouts);
        mm::Drivers drivers; CaptureDriver cap; drivers.addChild(&cap);
        drivers.setLayers(&layers);

        // Enabled first: the driver has a valid source buffer (a real frame).
        layers.onBuildState(); drivers.onBuildState();
        CHECK(layers.firstEnabledLayer() == &only);
        CHECK(layers.enabledLayerCount() == 1);
        REQUIRE(cap.src_ != nullptr);                // a frame is being published

        // Now disable the only layer and rebuild — the driver must drop to idle.
        only.setEnabled(false);
        layers.onBuildState(); drivers.onBuildState();
        CHECK(layers.activeLayer() == &only);        // fallback for geometry
        CHECK(layers.firstEnabledLayer() == nullptr);// no enabled source
        CHECK(layers.enabledLayerCount() == 0);
        CHECK(drivers.dynamicBytes() == 0);          // no output buffer allocated
        CHECK(cap.src_ == nullptr);                  // driver idle — the prior frame is NOT re-sent
    }
}

// activeLayer() returns the first enabled child, or the only child if all are disabled (so dimensions stay queryable during boot/toggle-off).
TEST_CASE("Layers::activeLayer returns first enabled child, or nullptr when empty") {
    mm::Layers empty;
    CHECK(empty.activeLayer() == nullptr);

    mm::Layers oneChild;
    mm::Layer onlyLayer;
    oneChild.addChild(&onlyLayer);
    CHECK(oneChild.activeLayer() == &onlyLayer);

    // Disabling the only child still surfaces it as the fallback (so dimensions
    // can still be queried for buffer allocation — important during boot or a
    // toggle-everything-off state).
    onlyLayer.setEnabled(false);
    CHECK(oneChild.activeLayer() == &onlyLayer);

    // With two children, a disabled first one yields the second as active.
    mm::Layers twoChildren;
    mm::Layer first, second;
    twoChildren.addChild(&first);
    twoChildren.addChild(&second);
    first.setEnabled(false);
    CHECK(twoChildren.activeLayer() == &second);
}

// firstEnabledLayer() is the output-selection counterpart to activeLayer(): it never
// falls back to a disabled layer, so it returns nullptr exactly when nothing renders.
TEST_CASE("Layers::firstEnabledLayer returns first enabled child, nullptr when all disabled") {
    mm::Layers empty;
    CHECK(empty.firstEnabledLayer() == nullptr);

    mm::Layers layers;
    mm::Layer first, second;
    layers.addChild(&first);
    layers.addChild(&second);
    CHECK(layers.firstEnabledLayer() == &first);     // both enabled → first

    first.setEnabled(false);
    CHECK(layers.firstEnabledLayer() == &second);    // skips the disabled first
    CHECK(layers.activeLayer() == &second);          // agrees while one stays enabled

    second.setEnabled(false);
    CHECK(layers.firstEnabledLayer() == nullptr);    // none enabled → nothing renders
    CHECK(layers.activeLayer() == &first);           // but geometry fallback still resolves
}

// If the container holds only non-Layer children, activeLayer() returns nullptr (the role-guard skips, never miscasts).
TEST_CASE("Layers::activeLayer returns nullptr when no child has role Layer") {
    // The role-guard in activeLayer (and setLayouts) skips non-Layer children
    // rather than miscasting. Today the UI's acceptsChildren mapping keeps
    // non-Layer children out, but the engine doesn't enforce it — so the
    // engine must degrade gracefully. Pin the contract: a Layers container
    // populated only with non-Layer children returns nullptr from
    // activeLayer(), not a miscast pointer.
    struct GenericChild : public mm::MoonModule {};

    mm::Layers layers;
    GenericChild stranger;
    layers.addChild(&stranger);
    CHECK(stranger.role() == mm::ModuleRole::Generic);  // sanity check the stub
    CHECK(layers.activeLayer() == nullptr);             // skipped, not miscast
}
