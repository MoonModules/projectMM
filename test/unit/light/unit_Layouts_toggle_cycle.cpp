// @module Layouts
// @also Layer, Drivers

#include "doctest.h"
#include "light/layers/Layers.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/drivers/Drivers.h"

namespace {

// Stub driver: records the buffer it was given on each loop call so we can
// inspect what reached the driver after each pipeline step.
class CaptureDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer* buf) override { source_ = buf; }
    void loop() override {
        if (source_ && source_->data()) {
            lastBytes = source_->bytes();
            lastNonZero = false;
            for (size_t i = 0; i < source_->bytes(); i++) {
                if (source_->data()[i] != 0) { lastNonZero = true; break; }
            }
        } else {
            lastBytes = 0;
            lastNonZero = false;
        }
    }
    size_t lastBytes = 0;
    bool lastNonZero = false;
private:
    mm::Buffer* source_ = nullptr;
};

// Simulate Scheduler::buildState() + one tick: walk the top-level modules calling
// onBuildState() then loop().
void rebuildAndTick(mm::Layouts& layouts, mm::Layers& layersC, mm::Drivers& driversC) {
    layouts.onBuildState();
    layersC.onBuildState();
    driversC.setLayer(layersC.activeLayer());
    driversC.onBuildState();
    layersC.loop();
    driversC.loop();
}

} // namespace

// Disabling the only layout child and re-enabling it must not crash Drivers, and rendering resumes cleanly.
TEST_CASE("Toggle a single layout off then on: pipeline survives and renders again") {
    // Regression: disabling the only layout child crashed Drivers::loop with a
    // null-pointer blendMap on the next tick — Layer::onBuildState bailed
    // early on physicalCount==0 without dropping the old LUT, while Drivers
    // reallocated its output buffer to 0 bytes. The fix tears down the LUT and
    // buffer on empty so Drivers takes the no-LUT path cleanly.
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4; grid.height = 4; grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layers layersC;
    mm::Layer layer;
    layer.setChannelsPerLight(3);
    mm::RainbowEffect effect;
    layer.addChild(&effect);
    // Include a modifier so the LUT is actually populated (hasLUT == true); the
    // crash only occurs in this path because the no-LUT path doesn't read the
    // output buffer.
    mm::MirrorModifier mirror;
    layer.addChild(&mirror);
    layersC.addChild(&layer);
    layersC.setLayouts(&layouts);

    mm::Drivers driversC;
    CaptureDriver capture;
    driversC.addChild(&capture);

    // Initial pipeline run.
    rebuildAndTick(layouts, layersC, driversC);
    CHECK_MESSAGE(capture.lastBytes > 0, "initial frame: driver saw no buffer");
    CHECK_MESSAGE(capture.lastNonZero, "initial frame: driver saw zero pixels");

    // Disable the only layout child. Drivers must survive (no crash) and see
    // an empty source — assert both the structural teardown AND that the driver
    // actually received an empty buffer (catches a regression where a stale
    // buffer would forward through to the driver after layer teardown).
    grid.setEnabled(false);
    rebuildAndTick(layouts, layersC, driversC);
    CHECK(layouts.totalLightCount() == 0);
    CHECK(layer.physicalLightCount() == 0);
    CHECK_FALSE(layer.lut().hasLUT());  // LUT torn down, not stale
    CHECK_MESSAGE(capture.lastBytes == 0, "after disable, driver saw stale buffer");
    CHECK_FALSE_MESSAGE(capture.lastNonZero, "after disable, driver saw stale pixels");

    // Re-enable. Driver MUST see non-zero pixels again.
    grid.setEnabled(true);
    rebuildAndTick(layouts, layersC, driversC);
    CHECK_MESSAGE(capture.lastBytes > 0, "after re-enable, driver saw no buffer");
    CHECK_MESSAGE(capture.lastNonZero, "after re-enable, driver saw zero pixels");
}
