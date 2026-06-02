// @module Layers
// @also Layer

#include "doctest.h"
#include "light/layers/Layers.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/CheckerboardEffect.h"
#include "platform/platform.h"

#include <cstring>

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

    mm::platform::setTestNowMs(0);
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

    // Both child Layer buffers must be populated — composition isn't wired
    // yet so we just verify each Layer's loop ran. (Checkerboard with default
    // controls writes a checker pattern; Rainbow writes a hue gradient.)
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
