#include "doctest.h"
#include "light/LayoutGroup.h"
#include "light/modules/layouts/GridLayout.h"

using namespace mm::light;

// GridLayout needs to implement LayoutBase for LayoutGroup to hold it.
// We create a thin adapter since GridLayout uses templates.
class GridLayoutAdapter : public GridLayout, public LayoutBase {
public:
    size_t pixelCount() const override { return GridLayout::pixelCount(); }
    void forEachCoord(CoordCallback cb, void* ctx) const override {
        GridLayout::forEachCoord(cb, ctx);
    }
};

TEST_CASE("LayoutGroup with one layout") {
    GridLayoutAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(4)); // 4x4x1
    grid.setControl(1, uint16_t(4));

    LayoutGroup fixture;
    fixture.addLayout(&grid);

    CHECK(fixture.totalPixelCount() == 16);
}

TEST_CASE("LayoutGroup iterate yields all coordinates") {
    GridLayoutAdapter grid;
    grid.addControls();
    grid.setControl(0, uint16_t(3));
    grid.setControl(1, uint16_t(2));
    grid.setControl(2, uint16_t(1));

    LayoutGroup fixture;
    fixture.addLayout(&grid);

    size_t count = 0;
    fixture.forEachCoord(
        [](void* ctx, uint32_t, int16_t, int16_t, int16_t) {
            ++(*static_cast<size_t*>(ctx));
        },
        &count
    );
    CHECK(count == 6);
}

TEST_CASE("LayoutGroup with multiple layouts offsets indices") {
    GridLayoutAdapter grid1, grid2;
    grid1.addControls();
    grid2.addControls();
    grid1.setControl(0, uint16_t(2)); grid1.setControl(1, uint16_t(2)); // 4 pixels
    grid2.setControl(0, uint16_t(3)); grid2.setControl(1, uint16_t(1)); // 3 pixels

    LayoutGroup fixture;
    fixture.addLayout(&grid1);
    fixture.addLayout(&grid2);

    CHECK(fixture.totalPixelCount() == 7);

    uint32_t maxIdx = 0;
    fixture.forEachCoord(
        [](void* ctx, uint32_t idx, int16_t, int16_t, int16_t) {
            auto* m = static_cast<uint32_t*>(ctx);
            if (idx > *m) *m = idx;
        },
        &maxIdx
    );
    CHECK(maxIdx == 6); // 0-3 from grid1, 4-6 from grid2
}
