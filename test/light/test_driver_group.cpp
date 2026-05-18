#include "doctest.h"
#include "light/DriverGroup.h"
#include "light/modules/layouts/GridLayout.h"

using namespace mm::light;

class GridAdapter : public GridLayout, public LayoutBase {
public:
    size_t pixelCount() const override { return GridLayout::pixelCount(); }
    void forEachCoord(CoordCallback cb, void* ctx) const override {
        GridLayout::forEachCoord(cb, ctx);
    }
};

class MockDriver : public DriverBase {
public:
    const char* name() const override { return "mock"; }
    int loopCount = 0;
    size_t lastBufferSize = 0;
    void loop() override {
        ++loopCount;
        lastBufferSize = outputBuffer().size();
    }
};

TEST_CASE("DriverGroup calls all drivers") {
    MockDriver d1, d2;
    DriverGroup dg;
    dg.addDriver(&d1);
    dg.addDriver(&d2);
    dg.allocateOutput(4);
    dg.loop();

    CHECK(d1.loopCount == 1);
    CHECK(d2.loopCount == 1);
}

TEST_CASE("DriverGroup passes output buffer to drivers") {
    MockDriver d;
    DriverGroup dg;
    dg.addDriver(&d);
    dg.allocateOutput(10);
    dg.loop();

    CHECK(d.lastBufferSize == 10);
}

TEST_CASE("DriverGroup blendMap fills output buffer") {
    GridAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(4)); // 4x1x1
    grid.setControl(1, uint16_t(1));

    LayoutGroup lg;
    lg.addLayout(&grid);

    Layer layer;
    layer.setLayoutGroup(&lg);
    layer.rebuildLUT();
    layer.buffer()[0] = {100, 0, 0};
    layer.buffer()[1] = {0, 200, 0};

    MockDriver d;
    DriverGroup dg;
    dg.addDriver(&d);
    dg.setLayers(&layer, 1);
    dg.allocateOutput(4);
    dg.loop();

    // Output buffer should have blended data
    CHECK(dg.outputBuffer()[0].r == 100);
    CHECK(dg.outputBuffer()[1].g == 200);
}

TEST_CASE("DriverGroup MAX_DRIVERS overflow") {
    DriverGroup dg;
    MockDriver drivers[DriverGroup::MAX_DRIVERS + 2];
    for (uint8_t i = 0; i < DriverGroup::MAX_DRIVERS + 2; ++i) {
        dg.addDriver(&drivers[i]);
    }
    CHECK(dg.driverCount() == DriverGroup::MAX_DRIVERS);
}
