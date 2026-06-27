// @module RotateModifier

#include "doctest.h"
#include "light/modifiers/RotateModifier.h"

// RotateModifier is the one DYNAMIC modifier: it overrides modifyLive (per-frame
// backward map, dest→source via an explicit 2×2 rotation matrix) and reports
// hasModifyLive() so the Layer runs its live pass. At the initial angle (0) the
// rotation is identity. loop() advances the angle; a unit test without a Layer keeps
// it at 0, so these pin the angle-0 identity and the in-box invariants.

// Apply the live remap to (x,y) in a w×h box; returns the source coord it samples.
static mm::Coord3D live(mm::RotateModifier& m, mm::lengthType x, mm::lengthType y,
                        mm::lengthType w, mm::lengthType h) {
    mm::Coord3D p{x, y, 0};
    m.modifyLive(p, {w, h, 1});
    return p;
}

TEST_CASE("RotateModifier advertises a live (per-frame) modifier") {
    mm::RotateModifier m;
    CHECK(m.hasModifyLive());
    CHECK(m.dimensions() == mm::Dim::D2);
}

// At the initial angle (0) the rotation matrix is the identity — every cell samples
// itself.
TEST_CASE("RotateModifier at angle 0 is identity") {
    mm::RotateModifier m;
    const mm::lengthType w = 8, h = 8;
    for (mm::lengthType y = 0; y < h; y++)
        for (mm::lengthType x = 0; x < w; x++)
            CHECK(live(m, x, y, w, h) == mm::Coord3D{x, y, 0});
}

// z passes through (2D rotation) — a 3D coord's z is untouched.
TEST_CASE("RotateModifier leaves z untouched") {
    mm::RotateModifier m;
    mm::Coord3D p{3, 4, 2};
    m.modifyLive(p, {8, 8, 4});
    CHECK(p.z == 2);
}

// An empty box doesn't divide-by-zero or wrap: the remap is a no-op-ish transform
// that the Layer's live pass then treats as out-of-box (dark), never a crash.
TEST_CASE("RotateModifier tolerates an empty box") {
    mm::RotateModifier m;
    mm::Coord3D p{0, 0, 0};
    m.modifyLive(p, {0, 0, 0});   // must not crash
    CHECK(true);
}
