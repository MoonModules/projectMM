// @module RegionModifier

#include "doctest.h"
#include "light/modifiers/RegionModifier.h"

// RegionModifier carves the layer to a sub-rectangle of the physical box, given
// as percentages. logicalDimensions reports the region size; mapToPhysical
// translates a region-local cell to its box cell at the region's start offset.
// Half-open [start, end): abutting regions tile exactly. Defaults 0/100 = full box.

static mm::nrOfLightsType mapOne(mm::RegionModifier& r,
                                 mm::lengthType x, mm::lengthType y, mm::lengthType z,
                                 mm::lengthType w, mm::lengthType h, mm::lengthType d,
                                 mm::nrOfLightsType& count) {
    mm::nrOfLightsType phys[8];
    count = 0;
    r.mapToPhysical(x, y, z, w, h, d, phys, count, 8);
    return count ? phys[0] : 0;
}

// Default region (0/100 on every axis) is the full box: identity dimensions.
TEST_CASE("RegionModifier default region is the full box") {
    mm::RegionModifier r;
    mm::lengthType logW, logH, logD;
    r.logicalDimensions(128, 64, 4, logW, logH, logD);
    CHECK(logW == 128);
    CHECK(logH == 64);
    CHECK(logD == 4);

    // (0,0,0) → box index 0; the last cell → the last box index.
    mm::nrOfLightsType count;
    CHECK(mapOne(r, 0, 0, 0, 128, 64, 4, count) == 0);
    CHECK(count == 1);
}

// Half of an axis, half-open: end=50 on 128 → pixels 0..63 (width 64), not 65.
TEST_CASE("RegionModifier half region is exact (half-open end)") {
    mm::RegionModifier r;
    r.endX = 50;  // 0..50% of 128
    mm::lengthType logW, logH, logD;
    r.logicalDimensions(128, 64, 1, logW, logH, logD);
    CHECK(logW == 64);   // exact half, not 65
    CHECK(logH == 64);   // untouched axis stays full
    CHECK(logD == 1);
}

// Two abutting regions tile a 128-wide axis with no overlap and no gap:
// 0..50 → [0,64), 50..100 → [64,128). The seam pixel 64 belongs to exactly one.
TEST_CASE("RegionModifier abutting regions tile exactly") {
    mm::RegionModifier left;  left.startX = 0;  left.endX = 50;
    mm::RegionModifier right; right.startX = 50; right.endX = 100;
    mm::lengthType lw, h, d, rw;
    left.logicalDimensions(128, 1, 1, lw, h, d);
    right.logicalDimensions(128, 1, 1, rw, h, d);
    CHECK(lw == 64);
    CHECK(rw == 64);
    CHECK(lw + rw == 128);   // no overlap, no gap

    // Right region's local x=0 maps to box pixel 64 (where the left region ended).
    mm::nrOfLightsType count;
    CHECK(mapOne(right, 0, 0, 0, 128, 1, 1, count) == 64);
    CHECK(count == 1);
}

// Region-local coordinates are translated by the start-pixel offset on each axis.
TEST_CASE("RegionModifier maps region-local cells to the offset box cell") {
    mm::RegionModifier r;
    r.startX = 50; r.endX = 100;   // x: pixels 64..127 on a 128-wide axis
    r.startY = 0;  r.endY = 50;    // y: pixels 0..63 on a 128-tall axis
    mm::nrOfLightsType count;

    // Local (0,0) → box (64, 0) → index 0*128 + 64 = 64.
    CHECK(mapOne(r, 0, 0, 0, 128, 128, 1, count) == 64);
    CHECK(count == 1);

    // Local (1,2) → box (65, 2) → index 2*128 + 65 = 321.
    CHECK(mapOne(r, 1, 2, 0, 128, 128, 1, count) == 321);
    CHECK(count == 1);
}

// Rounding rule on a small panel: start floors, end ceils to an exclusive pixel.
// start 33 / end 66 on a 4-wide axis → floor(1.32)=1 .. ceil(2.64)=3 → pixels 1,2.
TEST_CASE("RegionModifier rounding on a small panel (floor start, ceil end)") {
    mm::RegionModifier r;
    r.startX = 33; r.endX = 66;
    mm::lengthType logW, logH, logD;
    r.logicalDimensions(4, 1, 1, logW, logH, logD);
    CHECK(logW == 2);   // pixels 1,2

    mm::nrOfLightsType count;
    CHECK(mapOne(r, 0, 0, 0, 4, 1, 1, count) == 1);   // local 0 → box pixel 1
    CHECK(mapOne(r, 1, 0, 0, 4, 1, 1, count) == 2);   // local 1 → box pixel 2
}

// A region that rounds to nothing still gets a 1-pixel floor (never empties the
// layer). start 40 / end 41 on a 2-wide axis → would be 0 wide; clamped to 1.
TEST_CASE("RegionModifier never produces a zero-width region") {
    mm::RegionModifier r;
    r.startX = 40; r.endX = 41;
    mm::lengthType logW, logH, logD;
    r.logicalDimensions(2, 1, 1, logW, logH, logD);
    CHECK(logW >= 1);
}

// Negative / >100 percentages are legal on the wire; the carve math clamps them
// into the box rather than reading off the ends.
TEST_CASE("RegionModifier clamps out-of-range percentages to the box") {
    mm::RegionModifier r;
    r.startX = -50; r.endX = 200;   // both out of [0,100]
    mm::lengthType logW, logH, logD;
    r.logicalDimensions(64, 1, 1, logW, logH, logD);
    CHECK(logW == 64);   // clamps to the full axis, not past it

    mm::nrOfLightsType count;
    CHECK(mapOne(r, 0, 0, 0, 64, 1, 1, count) == 0);   // start clamps to pixel 0
}

// Degenerate axes don't crash: a 1-wide axis stays 1, a 0-extent axis yields 0.
TEST_CASE("RegionModifier handles degenerate axes") {
    mm::RegionModifier r;
    mm::lengthType logW, logH, logD;
    r.logicalDimensions(1, 0, 4, logW, logH, logD);
    CHECK(logW == 1);
    CHECK(logH == 0);
    CHECK(logD == 4);
}

// Never fans out — at most one destination, same family as CheckerboardModifier.
TEST_CASE("RegionModifier maxMultiplier is 1") {
    mm::RegionModifier r;
    CHECK(r.maxMultiplier() == 1);
}
