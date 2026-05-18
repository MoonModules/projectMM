#include "doctest.h"
#include "light/BlendMap.h"
#include "light/modules/layouts/GridLayout.h"

using namespace mm::light;

class GridAdapter : public GridLayout, public LayoutBase {
public:
    size_t pixelCount() const override { return GridLayout::pixelCount(); }
    void forEachCoord(CoordCallback cb, void* ctx) const override {
        GridLayout::forEachCoord(cb, ctx);
    }
};

TEST_CASE("blendMap single layer 1:1 copies pixels") {
    GridAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(4)); // 4x1x1
    grid.setControl(1, uint16_t(1));

    LayoutGroup lg;
    lg.addLayout(&grid);

    Layer layer;
    layer.setLayoutGroup(&lg);
    layer.rebuildLUT();

    layer.buffer()[0] = {10, 20, 30};
    layer.buffer()[1] = {40, 50, 60};
    layer.buffer()[2] = {70, 80, 90};
    layer.buffer()[3] = {100, 110, 120};

    Buffer dest;
    dest.allocate(4);
    blendMap(dest.pixels(), &layer, 1);

    CHECK(dest[0].r == 10);
    CHECK(dest[1].g == 50);
    CHECK(dest[2].b == 90);
    CHECK(dest[3].r == 100);
}

TEST_CASE("blendMap single layer with mirror X LUT") {
    GridAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(4)); // 4x1x1
    grid.setControl(1, uint16_t(1));

    LayoutGroup lg;
    lg.addLayout(&grid);

    MirrorModifier mirror;
    mirror.addControls();
    mirror.setControl(1, false); // Y off
    mirror.setControl(2, false); // Z off, X only

    Layer layer;
    layer.setLayoutGroup(&lg);
    layer.addModifier(&mirror);
    layer.rebuildLUT();

    // Logical buffer is 2 pixels (half of 4)
    CHECK(layer.buffer().count() == 2);
    layer.buffer()[0] = {10, 0, 0};
    layer.buffer()[1] = {20, 0, 0};

    Buffer dest;
    dest.allocate(4);
    blendMap(dest.pixels(), &layer, 1);

    // Mirror X: logical 0 → phys 0,3; logical 1 → phys 1,2
    CHECK(dest[0].r == 10);
    CHECK(dest[3].r == 10);
    CHECK(dest[1].r == 20);
    CHECK(dest[2].r == 20);
}

TEST_CASE("blendMap two layers additive blend") {
    GridAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(4)); // 4x1x1
    grid.setControl(1, uint16_t(1));

    LayoutGroup lg;
    lg.addLayout(&grid);

    Layer layers[2];
    layers[0].setLayoutGroup(&lg);
    layers[0].rebuildLUT();
    layers[1].setLayoutGroup(&lg);
    layers[1].rebuildLUT();

    layers[0].buffer()[0] = {100, 0, 0};
    layers[0].buffer()[1] = {0, 100, 0};
    layers[1].buffer()[0] = {50, 0, 0};
    layers[1].buffer()[1] = {0, 50, 0};

    Buffer dest;
    dest.allocate(4);
    blendMap(dest.pixels(), layers, 2);

    CHECK(dest[0].r == 150); // 100 + 50
    CHECK(dest[1].g == 150); // 100 + 50
}

TEST_CASE("blendMap additive blend clamps to 255") {
    GridAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(2)); // 2x1x1
    grid.setControl(1, uint16_t(1));

    LayoutGroup lg;
    lg.addLayout(&grid);

    Layer layers[2];
    layers[0].setLayoutGroup(&lg);
    layers[0].rebuildLUT();
    layers[1].setLayoutGroup(&lg);
    layers[1].rebuildLUT();

    layers[0].buffer()[0] = {200, 200, 200};
    layers[1].buffer()[0] = {200, 200, 200};

    Buffer dest;
    dest.allocate(2);
    blendMap(dest.pixels(), layers, 2);

    CHECK(dest[0].r == 255); // clamped
    CHECK(dest[0].g == 255);
    CHECK(dest[0].b == 255);
}

TEST_CASE("blendMap empty layer list leaves dest zeroed") {
    Buffer dest;
    dest.allocate(4);
    dest.fill({99, 99, 99});

    blendMap(dest.pixels(), nullptr, 0);

    CHECK(dest[0].r == 0);
    CHECK(dest[0].g == 0);
    CHECK(dest[3].b == 0);
}
