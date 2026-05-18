#include "doctest.h"
#include "light/Layer.h"
#include "light/modules/layouts/GridLayout.h"

using namespace mm::light;

// Adapter for LayoutGroup to hold GridLayout
class GridAdapter : public GridLayout, public LayoutBase {
public:
    size_t pixelCount() const override { return GridLayout::pixelCount(); }
    void forEachCoord(CoordCallback cb, void* ctx) const override {
        GridLayout::forEachCoord(cb, ctx);
    }
};

TEST_CASE("Layer rebuildLUT with grid fixture, no modifiers") {
    GridAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(4)); // 4x4x1
    grid.setControl(1, uint16_t(4));

    LayoutGroup fixture;
    fixture.addLayout(&grid);

    Layer layer;
    layer.setLayoutGroup(&fixture);
    layer.rebuildLUT();

    CHECK(layer.buffer().count() == 16);
    CHECK(layer.lut().logicalCount() == 16);

    // Without modifiers, each pixel maps 1:1
    for (size_t i = 0; i < 16; ++i) {
        CHECK(layer.lut().destinationCount(i) == 1);
    }
}

TEST_CASE("Layer rebuildLUT with mirror X modifier") {
    GridAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(4)); // 4x1x1
    grid.setControl(1, uint16_t(1));

    LayoutGroup fixture;
    fixture.addLayout(&grid);

    MirrorModifier mirror;
    mirror.addControls();
    mirror.setControl(1, false); // Y off
    mirror.setControl(2, false); // Z off, X only

    Layer layer;
    layer.setLayoutGroup(&fixture);
    layer.addModifier(&mirror);
    layer.rebuildLUT();

    // Physical 4x1, logical 2x1 (mirror X halves width)
    CHECK(layer.buffer().count() == 2); // logical buffer is half
    CHECK(layer.lut().logicalCount() == 2);

    // Each logical pixel maps to 2 physical pixels (1:2)
    CHECK(layer.lut().destinationCount(0) == 2); // lx=0 → phys 0 and 3
    CHECK(layer.lut().destinationCount(1) == 2); // lx=1 → phys 1 and 2
}

TEST_CASE("Layer render with rotate modifier") {
    GridAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(4)); // 4x4x1
    grid.setControl(1, uint16_t(4));

    LayoutGroup fixture;
    fixture.addLayout(&grid);

    RotateModifier rotate;
    rotate.addControls();

    Layer layer;
    layer.setLayoutGroup(&fixture);
    layer.addModifier(&rotate);
    layer.rebuildLUT();

    // Fill with a recognizable pattern
    for (size_t i = 0; i < layer.buffer().count(); ++i) {
        layer.buffer()[i] = {static_cast<uint8_t>(i * 10), 0, 0};
    }

    // Render at frame 0 — no rotation
    layer.render(0);
    CHECK(layer.buffer()[0].r == 0); // unchanged

    // Render at different frame — should be different
    // Refill pattern first
    for (size_t i = 0; i < layer.buffer().count(); ++i) {
        layer.buffer()[i] = {static_cast<uint8_t>(i * 10), 0, 0};
    }
    layer.render(90); // significant rotation

    // At least some pixels should have moved
    bool changed = false;
    for (size_t i = 0; i < layer.buffer().count(); ++i) {
        if (layer.buffer()[i].r != static_cast<uint8_t>(i * 10)) {
            changed = true;
            break;
        }
    }
    CHECK(changed);
}
