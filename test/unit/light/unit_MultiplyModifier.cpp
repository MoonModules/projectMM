// @module MultiplyModifier

#include "doctest.h"
#include "light/modifiers/MultiplyModifier.h"

// MultiplyModifier tiles the logical image across the physical box `multiply` times
// per axis, optionally reflecting alternate tiles (the kaleidoscope mirror). Under the
// fold build it works PHYSICAL→logical: modifyLogicalSize shrinks the box, and
// modifyLogical folds a physical coord onto its logical cell (`pos % logicalSize`, odd
// tiles reflected). N physical lights folding onto one logical cell IS the fan-out — so
// these cases pin the fold direction, the inverse of the old emit-N-destinations form.

// Fold a physical coord through modifyLogical; returns the logical cell it lands on.
// modifyLogicalSize must run first so the modifier stashes its tile size.
static mm::Coord3D fold(mm::MultiplyModifier& m, mm::lengthType x, mm::lengthType y, mm::lengthType z,
                        mm::Coord3D box) {
    mm::Coord3D logical = box;
    m.modifyLogicalSize(logical);
    mm::Coord3D p{x, y, z};
    m.modifyLogical(p);
    return p;
}

// Fold a coord and report whether it's kept (false = rejected, no tile).
static bool foldKeep(mm::MultiplyModifier& m, mm::lengthType x, mm::lengthType y, mm::lengthType z,
                     mm::Coord3D box, mm::Coord3D& out) {
    mm::Coord3D logical = box;
    m.modifyLogicalSize(logical);
    out = {x, y, z};
    return m.modifyLogical(out);
}

static mm::Coord3D logicalSize(mm::MultiplyModifier& m, mm::Coord3D box) {
    m.modifyLogicalSize(box);
    return box;
}

TEST_CASE("MultiplyModifier advertises D3 dimensions") {
    mm::MultiplyModifier m;
    CHECK(m.dimensions() == mm::Dim::D3);
}

// Defaults (multiply 2/2/1) → a 128×128 physical grid folds to a 64×64 logical box.
TEST_CASE("MultiplyModifier default logical size = mirror-XY fold") {
    mm::MultiplyModifier m;
    CHECK(logicalSize(m, {128, 128, 1}) == mm::Coord3D{64, 64, 1});
}

TEST_CASE("MultiplyModifier logical size on Z") {
    mm::MultiplyModifier m;
    m.multiplyZ = 2;
    CHECK(logicalSize(m, {128, 128, 4}) == mm::Coord3D{64, 64, 2});
}

// FAN-OUT (fold direction): with the defaults (mult 2, mirror XY), all four physical
// CORNERS fold onto the single logical pixel (0,0) — the inverse of the old "logical
// (0,0) → 4 physical corners". This is the kaleidoscope fold made concrete.
TEST_CASE("MultiplyModifier four corners fold to logical (0,0)") {
    mm::MultiplyModifier m;  // defaults: mult 2/2/1, mirror true/true/false
    const mm::Coord3D box{128, 128, 1};
    CHECK(fold(m, 0,   0,   0, box) == mm::Coord3D{0, 0, 0});   // tile (0,0) identity
    CHECK(fold(m, 127, 0,   0, box) == mm::Coord3D{0, 0, 0});   // tile (1,0) mirror x
    CHECK(fold(m, 0,   127, 0, box) == mm::Coord3D{0, 0, 0});   // tile (0,1) mirror y
    CHECK(fold(m, 127, 127, 0, box) == mm::Coord3D{0, 0, 0});   // tile (1,1) both
}

// mirrorX only: two physical columns fold to the same logical column (original + its
// horizontal reflection). The logical box is 64 wide.
TEST_CASE("MultiplyModifier mirrorX folds reflected columns together") {
    mm::MultiplyModifier m;
    m.multiplyX = 2; m.mirrorX = true;
    m.multiplyY = 1; m.mirrorY = false;
    m.multiplyZ = 1;
    const mm::Coord3D box{128, 128, 1};
    // logical width 64. Physical x=5 → tile 0 → logical x=5; physical x=122 (=127-5)
    // → tile 1 (odd), within=122%64=58, mirror → 64-1-58 = 5. Both fold to x=5.
    CHECK(fold(m, 5,   10, 0, box) == mm::Coord3D{5, 10, 0});
    CHECK(fold(m, 122, 10, 0, box) == mm::Coord3D{5, 10, 0});
}

// All multipliers 1 → identity: the box is unchanged and every coord folds to itself.
TEST_CASE("MultiplyModifier identity when all multipliers are 1") {
    mm::MultiplyModifier m;
    m.multiplyX = 1; m.multiplyY = 1; m.multiplyZ = 1;
    CHECK(logicalSize(m, {128, 128, 1}) == mm::Coord3D{128, 128, 1});
    CHECK(fold(m, 5, 10, 0, {128, 128, 1}) == mm::Coord3D{5, 10, 0});
}

// Tiling WITHOUT mirror repeats (does not reflect): physical x=64 (tile 1) folds to
// logical x=0, same as physical x=0 — both tiles map identically, no reflection.
TEST_CASE("MultiplyModifier tiles without mirror (repeat, not fold)") {
    mm::MultiplyModifier m;
    m.multiplyX = 2; m.mirrorX = false;
    m.multiplyY = 1; m.mirrorY = false;
    m.multiplyZ = 1;
    const mm::Coord3D box{128, 128, 1};   // logical width 64
    CHECK(fold(m, 0,  0, 0, box).x == 0);   // tile 0, x=0 → 0
    CHECK(fold(m, 64, 0, 0, box).x == 0);   // tile 1, x=64 → 64%64 = 0 (repeat, not 63)
}

// multiplyZ on a 2D (depth-1) layout is a no-op: the effective multiplier clamps to
// the axis extent (1), so depth stays 1 and the layer isn't blanked.
TEST_CASE("MultiplyModifier multiplyZ on 2D does nothing") {
    mm::MultiplyModifier m;
    m.multiplyX = 1; m.multiplyY = 1; m.multiplyZ = 4;  // Z multiply on a flat grid
    CHECK(logicalSize(m, {64, 64, 1}) == mm::Coord3D{64, 64, 1});   // depth stays 1, not 0
    CHECK(fold(m, 5, 10, 0, {64, 64, 1}) == mm::Coord3D{5, 10, 0}); // identity
}

// A multiplier larger than the axis extent clamps to the extent.
TEST_CASE("MultiplyModifier clamps a multiplier above the axis extent") {
    mm::MultiplyModifier m;
    m.multiplyX = 64; m.multiplyY = 1; m.multiplyZ = 1;
    m.mirrorX = false;
    CHECK(logicalSize(m, {16, 16, 1}) == mm::Coord3D{1, 16, 1});   // 16/16 = 1, not 16/64 = 0
}

// REGRESSION (🐇): a non-divisible extent leaves a leftover edge strip that the tiles
// don't cover — those pixels must be DROPPED, not wrapped back into a tile (which would
// duplicate the edge). 5-wide, multiply 2 → tile width 2, covers pixels 0..3; pixel 4 is
// the leftover and has no tile.
TEST_CASE("MultiplyModifier drops the leftover strip on a non-divisible extent") {
    mm::MultiplyModifier m;
    m.multiplyX = 2; m.multiplyY = 1; m.multiplyZ = 1;
    m.mirrorX = false;
    const mm::Coord3D box{5, 1, 1};
    CHECK(logicalSize(m, box).x == 2);   // 5/2 = 2 tile width

    mm::Coord3D p;
    CHECK(foldKeep(m, 0, 0, 0, box, p));        // tile 0
    CHECK(foldKeep(m, 3, 0, 0, box, p));        // tile 1, within the covered 0..3
    CHECK_FALSE(foldKeep(m, 4, 0, 0, box, p));  // leftover edge — dropped, NOT folded to 0
}
