#include "doctest.h"
#include "light/modules/modifiers/MirrorModifier.h"

using namespace mm::light;

TEST_CASE("MirrorModifier default: all axes on") {
    MirrorModifier mirror;
    mirror.addControls();
    CHECK(mirror.mirrorX() == true);
    CHECK(mirror.mirrorY() == true);
    CHECK(mirror.mirrorZ() == true);
    CHECK(mirror.controlCount() == 3);
}

TEST_CASE("MirrorModifier multiplier") {
    MirrorModifier mirror;
    mirror.addControls();
    CHECK(mirror.multiplier() == 8); // X*Y*Z = 2*2*2

    mirror.setControl(2, false); // Z off
    CHECK(mirror.multiplier() == 4); // X*Y = 2*2

    mirror.setControl(1, false); // Y off
    CHECK(mirror.multiplier() == 2); // X only

    mirror.setControl(0, false); // all off
    CHECK(mirror.multiplier() == 1);
}

TEST_CASE("MirrorModifier logical dimensions") {
    MirrorModifier mirror;
    mirror.addControls();

    // All on: dimensions halved
    CHECK(mirror.logicalWidth(128) == 64);
    CHECK(mirror.logicalHeight(128) == 64);
    CHECK(mirror.logicalDepth(1) == 1); // (1+1)/2 = 1

    // Odd dimensions: round up
    CHECK(mirror.logicalWidth(9) == 5); // (9+1)/2 = 5
}

TEST_CASE("MirrorModifier XY mirror maps 1:4") {
    MirrorModifier mirror;
    mirror.addControls();
    mirror.setControl(2, false); // Z off, X+Y on

    // Physical 10x10, logical 5x5
    // Logical pixel (0,0) maps to physical (0,0), (9,0), (0,9), (9,9)
    uint16_t out[8];
    uint8_t count = mirror.mapToPhysical(0, 0, 0, 10, 10, 1, out, 10);
    CHECK(count == 4);
}

TEST_CASE("MirrorModifier centre pixel deduplicates") {
    MirrorModifier mirror;
    mirror.addControls();
    mirror.setControl(1, false); // Y off
    mirror.setControl(2, false); // Z off, X only

    // Physical width 10, logical width 5
    // Logical pixel at x=4 mirrors to x=5 — no duplicate
    // But logical pixel at x=4 in a width-9 grid mirrors to x=4 — duplicate
    uint16_t out[8];
    // Width 9: centre is x=4, mirrors to 9-1-4=4 → same → deduplicated
    uint8_t count = mirror.mapToPhysical(4, 0, 0, 9, 1, 1, out, 9);
    CHECK(count == 1); // deduplicated
}

TEST_CASE("MirrorModifier X only maps 1:2") {
    MirrorModifier mirror;
    mirror.addControls();
    mirror.setControl(1, false); // Y off
    mirror.setControl(2, false); // Z off

    uint16_t out[8];
    // Physical 10x1, logical 5x1
    // Logical (2,0,0) → physical (2,0,0) and (7,0,0)
    uint8_t count = mirror.mapToPhysical(2, 0, 0, 10, 1, 1, out, 10);
    CHECK(count == 2);
    CHECK(out[0] == 2);
    CHECK(out[1] == 7);
}
