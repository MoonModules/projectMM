// @module CheckerboardModifier

#include "doctest.h"
#include "light/modifiers/CheckerboardModifier.h"

// CheckerboardModifier masks the layer: lights in "off" squares are dropped
// (modifyLogical returns false), lights in "on" squares pass through unchanged
// (returns true, pos untouched). A mask leaves the logical box unchanged.

// Run the fold on one coord; returns whether it passes (true) and leaves pos in p.
static bool keep(mm::CheckerboardModifier& c, mm::lengthType x, mm::lengthType y, mm::lengthType z,
                 mm::Coord3D box, mm::Coord3D& p) {
    mm::Coord3D size = box;
    c.modifyLogicalSize(size);   // a mask: identity size
    p = {x, y, z};
    return c.modifyLogical(p);
}

// A mask leaves the logical box unchanged.
TEST_CASE("CheckerboardModifier does not resize the logical box") {
    mm::CheckerboardModifier c;
    mm::Coord3D size{64, 32, 4};
    c.modifyLogicalSize(size);
    CHECK(size == mm::Coord3D{64, 32, 4});
}

// size=1: every cell is its own square; parity = (x+y+z)&1. Default (invert
// false) keeps even-parity cells, drops odd-parity. Passing cells keep their coord.
TEST_CASE("CheckerboardModifier size 1 keeps even-parity, drops odd") {
    mm::CheckerboardModifier c;
    c.size = 1;
    const mm::Coord3D box{8, 8, 1};
    mm::Coord3D p;

    CHECK(keep(c, 0, 0, 0, box, p));            // parity 0 → on
    CHECK(p == mm::Coord3D{0, 0, 0});           // pass-through: coord unchanged
    CHECK_FALSE(keep(c, 1, 0, 0, box, p));      // parity 1 → dropped
    CHECK(keep(c, 1, 1, 0, box, p));            // parity 0 → on
    CHECK(p == mm::Coord3D{1, 1, 0});
}

// invert flips which parity passes.
TEST_CASE("CheckerboardModifier invert flips the kept squares") {
    mm::CheckerboardModifier c;
    c.size = 1;
    c.invert = true;
    const mm::Coord3D box{8, 8, 1};
    mm::Coord3D p;

    CHECK_FALSE(keep(c, 0, 0, 0, box, p));      // parity 0, inverted → off
    CHECK(keep(c, 1, 0, 0, box, p));            // parity 1, inverted → on
}

// size>1 groups cells into squares: with size=2, the 2×2 block at the origin is
// all one square (parity 0), so all four pass; the next block over drops.
TEST_CASE("CheckerboardModifier size 2 groups into squares") {
    mm::CheckerboardModifier c;
    c.size = 2;
    const mm::Coord3D box{8, 8, 1};
    mm::Coord3D p;

    for (mm::lengthType y = 0; y < 2; y++)
        for (mm::lengthType x = 0; x < 2; x++)
            CHECK(keep(c, x, y, 0, box, p));    // whole origin block passes

    CHECK_FALSE(keep(c, 2, 0, 0, box, p));      // adjacent block (parity 1) drops
}
