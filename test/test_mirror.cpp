#include "doctest.h"
#include "light/modifiers/MirrorModifier.h"

TEST_CASE("MirrorModifier advertises D3 dimensions") {
    // MirrorModifier handles all three axes via mirrorX/Y/Z toggles, so the
    // factory dim chip should be 🧊. Pins the default for ModifierBase too.
    mm::MirrorModifier mirror;
    CHECK(mirror.dimensions() == mm::Dim::D3);
}

TEST_CASE("MirrorModifier logicalDimensions even grid") {
    mm::MirrorModifier mirror;
    mm::lengthType logW, logH, logD;

    mirror.logicalDimensions(128, 128, 1, logW, logH, logD);
    CHECK(logW == 64);
    CHECK(logH == 64);
    CHECK(logD == 1);
}

TEST_CASE("MirrorModifier logicalDimensions odd grid") {
    mm::MirrorModifier mirror;
    mm::lengthType logW, logH, logD;

    mirror.logicalDimensions(127, 127, 1, logW, logH, logD);
    CHECK(logW == 64); // (127+1)/2 = 64
    CHECK(logH == 64);
    CHECK(logD == 1);
}

TEST_CASE("MirrorModifier logicalDimensions mirrorZ") {
    mm::MirrorModifier mirror;
    mirror.mirrorZ = true;
    mm::lengthType logW, logH, logD;

    mirror.logicalDimensions(128, 128, 4, logW, logH, logD);
    CHECK(logW == 64);
    CHECK(logH == 64);
    CHECK(logD == 2);
}

TEST_CASE("MirrorModifier corner pixel produces 4 positions with mirrorXY") {
    mm::MirrorModifier mirror;
    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;

    // Grid 128x128, pixel (0,0) should map to 4 corners
    mirror.mapToPhysical(0, 0, 0, 128, 128, 1, physicals, count, 8);
    CHECK(count == 4);

    // Verify the 4 positions: (0,0), (127,0), (0,127), (127,127)
    // In row-major: idx = y*128 + x
    CHECK(physicals[0] == 0);           // (0, 0)
    CHECK(physicals[1] == 127);         // (127, 0)
    CHECK(physicals[2] == 127 * 128);   // (0, 127)
    CHECK(physicals[3] == 127 * 128 + 127); // (127, 127)
}

TEST_CASE("MirrorModifier centre pixel deduplication on odd grid") {
    mm::MirrorModifier mirror;
    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;

    // Grid 127x127, pixel (63,63): mirrored x = 127-1-63 = 63 (same!)
    mirror.mapToPhysical(63, 63, 0, 127, 127, 1, physicals, count, 8);
    CHECK(count == 1); // All mirrors deduplicate to same position
}

TEST_CASE("MirrorModifier no mirrors produces 1 position") {
    mm::MirrorModifier mirror;
    mirror.mirrorX = false;
    mirror.mirrorY = false;
    mirror.mirrorZ = false;
    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;

    mirror.mapToPhysical(5, 10, 0, 128, 128, 1, physicals, count, 8);
    CHECK(count == 1);
    CHECK(physicals[0] == 10 * 128 + 5); // row-major: y*W + x
}

TEST_CASE("MirrorModifier mirrorX only produces 2 positions") {
    mm::MirrorModifier mirror;
    mirror.mirrorX = true;
    mirror.mirrorY = false;
    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;

    mirror.mapToPhysical(5, 10, 0, 128, 128, 1, physicals, count, 8);
    CHECK(count == 2);
    CHECK(physicals[0] == 10 * 128 + 5);   // (5, 10)
    CHECK(physicals[1] == 10 * 128 + 122); // (122, 10) = (127-5, 10)
}
