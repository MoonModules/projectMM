// @module CheckerboardModifier

#include "doctest.h"
#include "light/modifiers/CheckerboardModifier.h"

// CheckerboardModifier masks the layer: lights in "off" squares are dropped
// (mapToPhysical returns outCount=0), lights in "on" squares pass through
// unchanged. The logical box is unchanged (identity dimensions).

static mm::nrOfLightsType mapOne(mm::CheckerboardModifier& c,
                                 mm::lengthType x, mm::lengthType y, mm::lengthType z,
                                 mm::lengthType w, mm::lengthType h, mm::lengthType d,
                                 mm::nrOfLightsType& count) {
    mm::nrOfLightsType phys[8];
    count = 0;
    c.mapToPhysical(x, y, z, w, h, d, phys, count, 8);
    return count ? phys[0] : 0;
}

// Identity dimensions — a mask doesn't resize the logical box.
TEST_CASE("CheckerboardModifier logicalDimensions are identity") {
    mm::CheckerboardModifier c;
    mm::lengthType logW, logH, logD;
    c.logicalDimensions(64, 32, 4, logW, logH, logD);
    CHECK(logW == 64);
    CHECK(logH == 32);
    CHECK(logD == 4);
}

// size=1: every cell is its own square; parity = (x+y+z)&1. Default (invert
// false) keeps even-parity cells, drops odd-parity.
TEST_CASE("CheckerboardModifier size 1 keeps even-parity, drops odd") {
    mm::CheckerboardModifier c;
    c.size = 1;
    mm::nrOfLightsType count;

    // (0,0,0): parity 0 → on. Passes through at identity index 0.
    CHECK(mapOne(c, 0, 0, 0, 8, 8, 1, count) == 0);
    CHECK(count == 1);

    // (1,0,0): parity 1 → off. Dropped.
    mapOne(c, 1, 0, 0, 8, 8, 1, count);
    CHECK(count == 0);

    // (1,1,0): parity 0 → on. Identity index 1*8+1 = 9.
    CHECK(mapOne(c, 1, 1, 0, 8, 8, 1, count) == 9);
    CHECK(count == 1);
}

// invert flips which parity passes — the cell that was dropped now passes and
// vice versa.
TEST_CASE("CheckerboardModifier invert flips the kept squares") {
    mm::CheckerboardModifier c;
    c.size = 1;
    c.invert = true;
    mm::nrOfLightsType count;

    // (0,0,0): parity 0, but inverted → off. Dropped.
    mapOne(c, 0, 0, 0, 8, 8, 1, count);
    CHECK(count == 0);

    // (1,0,0): parity 1, inverted → on. Passes at index 1.
    CHECK(mapOne(c, 1, 0, 0, 8, 8, 1, count) == 1);
    CHECK(count == 1);
}

// size>1 groups cells into squares: with size=2, the 2×2 block at the origin is
// all one square (parity of 0/2=0), so all four pass; the next block over drops.
TEST_CASE("CheckerboardModifier size 2 groups into squares") {
    mm::CheckerboardModifier c;
    c.size = 2;
    mm::nrOfLightsType count;

    // Origin 2×2 block (x,y in 0..1): square (0,0) parity 0 → on.
    for (mm::lengthType y = 0; y < 2; y++)
        for (mm::lengthType x = 0; x < 2; x++) {
            mapOne(c, x, y, 0, 8, 8, 1, count);
            CHECK(count == 1);  // whole block passes
        }

    // Adjacent block (x in 2..3): square (1,0) parity 1 → off.
    mapOne(c, 2, 0, 0, 8, 8, 1, count);
    CHECK(count == 0);
}

// Never fans out — at most one destination.
TEST_CASE("CheckerboardModifier maxMultiplier is 1") {
    mm::CheckerboardModifier c;
    CHECK(c.maxMultiplier() == 1);
}
