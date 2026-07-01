// @module PinwheelModifier

#include "doctest.h"
#include "light/modifiers/PinwheelModifier.h"

#include <set>

// PinwheelModifier folds the box into `petals` angular wedges. modifyLogicalSize reshapes the box:
//  - 2D+ (incoming y > 1): {petals, radius+1, 1} — petals on X, radius along Y.
//  - 1D  (incoming y == 1): {1, petals, 1} — petals on Y (the D1-runs-along-Y convention).
// modifyLogical maps a physical (x,y) to the petal index on the axis the reshape chose. The 1D case
// is the regression here: routing the petal onto X (extent 1) would drop every petal but 0, collapsing
// the pinwheel to a single lit cell — the layer rejects a logical x >= 1.

static mm::Coord3D sizeFor(mm::PinwheelModifier& p, mm::Coord3D box) {
    p.modifyLogicalSize(box);
    return box;
}

TEST_CASE("PinwheelModifier 2D reshape puts petals on X, radius on Y") {
    mm::PinwheelModifier p;
    p.petals = 8;
    // A 2D box (y > 1) → {petals, radius+1, 1}: 8 on x, some radius on y, flat z.
    const mm::Coord3D s = sizeFor(p, {16, 16, 1});
    CHECK(s.x == 8);        // petals on x
    CHECK(s.y > 1);         // radius along y
    CHECK(s.z == 1);
}

TEST_CASE("PinwheelModifier 1D reshape puts petals on Y (1 x petals x 1)") {
    mm::PinwheelModifier p;
    p.petals = 8;
    // A 1D box (y == 1) → {1, petals, 1}: petals live on Y so they survive the layer's logical-x bound.
    const mm::Coord3D s = sizeFor(p, {16, 1, 1});
    CHECK(s.x == 1);        // x collapses to a single column
    CHECK(s.y == 8);        // petals on y
    CHECK(s.z == 1);
}

// The regression: on a 1D layer, sweeping the source x must map to MORE THAN ONE petal cell, and each
// mapped coordinate must land within the reshaped {1, petals, 1} box (x == 0, 0 <= y < petals). Before
// the fix the petal index went to pos.x, so every cell mapped to x == value >= 1 (out of the 1-wide
// logical box) except petal 0 — the pinwheel collapsed to a single petal.
TEST_CASE("PinwheelModifier 1D maps the sweep across distinct petals on Y") {
    mm::PinwheelModifier p;
    p.petals = 8;
    mm::Coord3D box{16, 1, 1};
    p.modifyLogicalSize(box);   // stash geometry (middle_, petalWidth_) for the fold

    std::set<int> petalYs;
    for (mm::lengthType x = 0; x < 16; x++) {
        mm::Coord3D pos{x, 0, 0};
        REQUIRE(p.modifyLogical(pos));
        CHECK(pos.x == 0);                 // stays in the single logical column
        CHECK(pos.y >= 0);
        CHECK(pos.y < 8);                  // within the reshaped petals-on-y box
        petalYs.insert(pos.y);
    }
    // The sweep must touch more than one petal — the whole point of the effect. (Was 1 before the fix.)
    CHECK(petalYs.size() > 1);
}
