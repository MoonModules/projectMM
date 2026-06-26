// @module RegionModifier

#include "doctest.h"
#include "light/modifiers/RegionModifier.h"

// RegionModifier carves the layer to a sub-rectangle of the physical box, given as
// percentages. modifyLogicalSize reports the region size; modifyLogical folds a
// PHYSICAL coord into region-local space (subtract the start offset) and rejects any
// physical light outside the region. Half-open [start, end): abutting regions tile.

// The region size for a given physical box.
static mm::Coord3D regionSize(mm::RegionModifier& r, mm::Coord3D box) {
    r.modifyLogicalSize(box);
    return box;
}

// Fold a physical coord; returns whether it's inside the region, leaving region-local
// pos in p. `box` is the physical box; logical is the region size.
static bool fold(mm::RegionModifier& r, mm::lengthType x, mm::lengthType y, mm::lengthType z,
                 mm::Coord3D box, mm::Coord3D& p) {
    mm::Coord3D logical = box;
    r.modifyLogicalSize(logical);   // stashes the start offset + region size
    p = {x, y, z};
    return r.modifyLogical(p);
}

// Default region (0/100 on every axis) is the full box: identity size, no rejection.
TEST_CASE("RegionModifier default region is the full box") {
    mm::RegionModifier r;
    CHECK(regionSize(r, {128, 64, 4}) == mm::Coord3D{128, 64, 4});

    mm::Coord3D p;
    CHECK(fold(r, 0, 0, 0, {128, 64, 4}, p));   // physical (0,0,0) is in the region
    CHECK(p == mm::Coord3D{0, 0, 0});           // and stays at region-local (0,0,0)
}

// Half of an axis, half-open: end=50 on 128 → region width 64, not 65.
TEST_CASE("RegionModifier half region is exact (half-open end)") {
    mm::RegionModifier r;
    r.endX = 50;  // 0..50% of 128
    CHECK(regionSize(r, {128, 64, 1}) == mm::Coord3D{64, 64, 1});   // half x, full y
}

// Two abutting regions tile a 128-wide axis with no overlap and no gap.
TEST_CASE("RegionModifier abutting regions tile exactly") {
    mm::RegionModifier left;  left.startX = 0;  left.endX = 50;
    mm::RegionModifier right; right.startX = 50; right.endX = 100;
    const mm::lengthType lw = regionSize(left,  {128, 1, 1}).x;
    const mm::lengthType rw = regionSize(right, {128, 1, 1}).x;
    CHECK(lw == 64);
    CHECK(rw == 64);
    CHECK(lw + rw == 128);   // no overlap, no gap

    // The seam: physical pixel 63 belongs to the LEFT region (region-local 63), pixel
    // 64 belongs to the RIGHT (region-local 0). Each is in exactly one region.
    mm::Coord3D p;
    CHECK(fold(left,  63, 0, 0, {128, 1, 1}, p)); CHECK(p.x == 63);
    CHECK_FALSE(fold(left, 64, 0, 0, {128, 1, 1}, p));   // 64 is past the left region
    CHECK(fold(right, 64, 0, 0, {128, 1, 1}, p)); CHECK(p.x == 0);
    CHECK_FALSE(fold(right, 63, 0, 0, {128, 1, 1}, p));  // 63 is before the right region
}

// A physical coord inside the region folds to region-local (subtract the start pixel);
// a coord outside is rejected.
TEST_CASE("RegionModifier folds physical to region-local and rejects outside") {
    mm::RegionModifier r;
    r.startX = 50; r.endX = 100;   // x: pixels 64..127 on a 128-wide axis
    r.startY = 0;  r.endY = 50;    // y: pixels 0..63 on a 128-tall axis
    mm::Coord3D p;

    // Physical (64, 0) → region-local (0, 0).
    CHECK(fold(r, 64, 0, 0, {128, 128, 1}, p)); CHECK(p == mm::Coord3D{0, 0, 0});
    // Physical (65, 2) → region-local (1, 2).
    CHECK(fold(r, 65, 2, 0, {128, 128, 1}, p)); CHECK(p == mm::Coord3D{1, 2, 0});
    // Physical (0, 0) is left of the x-region → rejected.
    CHECK_FALSE(fold(r, 0, 0, 0, {128, 128, 1}, p));
    // Physical (64, 100) is below the y-region → rejected.
    CHECK_FALSE(fold(r, 64, 100, 0, {128, 128, 1}, p));
}

// Rounding rule on a small panel: start floors, end ceils to an exclusive pixel.
// start 33 / end 66 on a 4-wide axis → floor(1.32)=1 .. ceil(2.64)=3 → pixels 1,2.
TEST_CASE("RegionModifier rounding on a small panel (floor start, ceil end)") {
    mm::RegionModifier r;
    r.startX = 33; r.endX = 66;
    CHECK(regionSize(r, {4, 1, 1}).x == 2);   // pixels 1,2

    mm::Coord3D p;
    CHECK(fold(r, 1, 0, 0, {4, 1, 1}, p)); CHECK(p.x == 0);   // physical 1 → region-local 0
    CHECK(fold(r, 2, 0, 0, {4, 1, 1}, p)); CHECK(p.x == 1);   // physical 2 → region-local 1
    CHECK_FALSE(fold(r, 0, 0, 0, {4, 1, 1}, p));              // physical 0 is before the region
    CHECK_FALSE(fold(r, 3, 0, 0, {4, 1, 1}, p));              // physical 3 is after the region
}

// A region that rounds to nothing still gets a 1-pixel floor.
TEST_CASE("RegionModifier never produces a zero-width region") {
    mm::RegionModifier r;
    r.startX = 40; r.endX = 41;
    CHECK(regionSize(r, {2, 1, 1}).x >= 1);
}

// OFF-SCREEN: a window slid half off the left edge keeps its FULL size (the effect
// renders at a fixed scale); only the visible half maps to physical lights.
// startX=-50 on 64 → window [−32, 32), span 64. Physical x 0..31 land at window-local
// 32..63 (the right half of the window — the visible part); the left half of the
// window (0..31) has no physical light, so it's dark. The effect isn't rescaled.
TEST_CASE("RegionModifier slides a window off-screen without rescaling") {
    mm::RegionModifier r;
    r.startX = -50; r.endX = 50;        // window [−32, 32) on a 64-wide axis
    CHECK(regionSize(r, {64, 1, 1}).x == 64);   // FULL window span, not clamped to 32

    mm::Coord3D p;
    // Physical x=0 → window-local 0 − (−32) = 32 (lands in the right half, visible).
    CHECK(fold(r, 0,  0, 0, {64, 1, 1}, p)); CHECK(p.x == 32);
    CHECK(fold(r, 31, 0, 0, {64, 1, 1}, p)); CHECK(p.x == 63);
    // Physical x=32 would be window-local 64 ≥ span 64 → rejected (past the window).
    CHECK_FALSE(fold(r, 32, 0, 0, {64, 1, 1}, p));
}

// A window entirely off the box maps NO lights — the layer goes dark on that axis,
// which is how an effect is moved completely out of view. The box still has a valid
// size (the effect renders), nothing just reaches the screen.
TEST_CASE("RegionModifier fully off-screen window renders nothing") {
    mm::RegionModifier r;
    r.startX = -100; r.endX = 0;        // window [−64, 0) — entirely left of the box
    CHECK(regionSize(r, {64, 1, 1}).x == 64);   // full window span, the effect still renders

    mm::Coord3D p;
    for (mm::lengthType x = 0; x < 64; x++)
        CHECK_FALSE(fold(r, x, 0, 0, {64, 1, 1}, p));   // no physical light is in the window
}

// A window stretched WIDER than the box (start<0 and end>100) renders the full span;
// the box shows the middle slice. startX=-50,endX=150 on 64 → window [−32, 96), span 128.
TEST_CASE("RegionModifier window wider than the box") {
    mm::RegionModifier r;
    r.startX = -50; r.endX = 150;       // window [−32, 96) on 64
    CHECK(regionSize(r, {64, 1, 1}).x == 128);   // 2× the axis — a growing window

    mm::Coord3D p;
    // Physical x=0 → window-local 32 (the middle of the 128-wide window is on-screen).
    CHECK(fold(r, 0,  0, 0, {64, 1, 1}, p)); CHECK(p.x == 32);
    CHECK(fold(r, 63, 0, 0, {64, 1, 1}, p)); CHECK(p.x == 95);
}

// Degenerate axes don't crash: a 1-wide axis stays 1, a 0-extent axis yields 0.
TEST_CASE("RegionModifier handles degenerate axes") {
    mm::RegionModifier r;
    CHECK(regionSize(r, {1, 0, 4}) == mm::Coord3D{1, 0, 4});
}
