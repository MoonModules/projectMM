// @module RotateModifier

#include "doctest.h"
#include "light/modifiers/RotateModifier.h"

// RotateModifier remaps each light to its rotated source. At angle 0 the map is
// identity; the modifier never fans out (outCount 0 or 1); the logical box is
// unchanged. These test the mapping directly via mapToPhysical (no Layer needed —
// the angle starts at 0, and loop() is what advances it).

namespace {

mm::nrOfLightsType mapOne(mm::RotateModifier& m,
                          mm::lengthType x, mm::lengthType y,
                          mm::lengthType w, mm::lengthType h,
                          mm::nrOfLightsType& count) {
    mm::nrOfLightsType phys[4];
    count = 0;
    m.mapToPhysical(x, y, 0, w, h, 1, phys, count, 4);
    return count ? phys[0] : 0;
}

} // namespace

TEST_CASE("RotateModifier logicalDimensions are identity") {
    mm::RotateModifier m;
    mm::lengthType lw, lh, ld;
    m.logicalDimensions(32, 16, 4, lw, lh, ld);
    CHECK(lw == 32);
    CHECK(lh == 16);
    CHECK(ld == 4);
}

TEST_CASE("RotateModifier maxMultiplier is 1") {
    mm::RotateModifier m;
    CHECK(m.maxMultiplier() == 1);
}

// At the initial angle (0) the rotation is identity — every light maps to itself.
TEST_CASE("RotateModifier at angle 0 is identity") {
    mm::RotateModifier m;
    const mm::lengthType w = 8, h = 8;
    mm::nrOfLightsType count;
    for (mm::lengthType y = 0; y < h; y++)
        for (mm::lengthType x = 0; x < w; x++) {
            const mm::nrOfLightsType expected = static_cast<mm::nrOfLightsType>(y) * w + x;
            CHECK(mapOne(m, x, y, w, h, count) == expected);
            CHECK(count == 1);
        }
}

// Every emitted destination is in range (no out-of-bounds index), and the count
// is always 0 or 1 (a remap never fans out).
TEST_CASE("RotateModifier emits at most one in-range destination") {
    mm::RotateModifier m;
    const mm::lengthType w = 8, h = 8;
    const mm::nrOfLightsType n = static_cast<mm::nrOfLightsType>(w) * h;
    mm::nrOfLightsType count;
    for (mm::lengthType y = 0; y < h; y++)
        for (mm::lengthType x = 0; x < w; x++) {
            mm::nrOfLightsType dst = mapOne(m, x, y, w, h, count);
            CHECK(count <= 1);
            if (count == 1) CHECK(dst < n);
        }
}

TEST_CASE("RotateModifier tolerates an empty grid") {
    mm::RotateModifier m;
    mm::nrOfLightsType phys[4];
    mm::nrOfLightsType count = 9;   // sentinel
    m.mapToPhysical(0, 0, 0, 0, 0, 0, phys, count, 0);
    CHECK(count == 0);
}
