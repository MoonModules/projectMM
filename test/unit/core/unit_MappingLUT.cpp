// @module MappingLUT

#include "doctest.h"
#include "light/layers/MappingLUT.h"

#include <vector>

// A fresh LUT carries no mapping (hasLUT==false, logicalCount==0); BlendMap takes the fast identity copy path.
TEST_CASE("MappingLUT default is identity (no LUT)") {
    mm::MappingLUT lut;
    CHECK(!lut.hasLUT());
    CHECK(lut.logicalCount() == 0);
}

// setIdentity(N) declares a 1:1 mapping for N lights without allocating a LUT; forEachDestination still iterates correctly.
TEST_CASE("MappingLUT setIdentity") {
    mm::MappingLUT lut;
    lut.setIdentity(256);
    CHECK(!lut.hasLUT());
    CHECK(lut.logicalCount() == 256);

    // forEachDestination in identity mode returns the logical index
    std::vector<mm::nrOfLightsType> dests;
    lut.forEachDestination(42, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 1);
    CHECK(dests[0] == 42);
}

// Each logical light can map to a different count of physical lights; forEachDestination yields every mapped index in order.
TEST_CASE("MappingLUT 1:N mapping") {
    mm::MappingLUT lut;
    // 4 logical lights, each maps to different number of physical lights
    CHECK(lut.build(4, 10));
    CHECK_FALSE(!lut.hasLUT());

    // Logical 0 → physical {0, 3}
    mm::nrOfLightsType map0[] = {0, 3};
    lut.setMapping(0, map0, 2);

    // Logical 1 → physical {1, 2, 5, 7}
    mm::nrOfLightsType map1[] = {1, 2, 5, 7};
    lut.setMapping(1, map1, 4);

    // Logical 2 → physical {4}
    mm::nrOfLightsType map2[] = {4};
    lut.setMapping(2, map2, 1);

    // Logical 3 → physical {6, 8, 9}
    mm::nrOfLightsType map3[] = {6, 8, 9};
    lut.setMapping(3, map3, 3);

    lut.finalize();

    CHECK(lut.logicalCount() == 4);
    CHECK(lut.destinationCount() == 10);

    // Verify each logical index maps correctly
    std::vector<mm::nrOfLightsType> dests;

    dests.clear();
    lut.forEachDestination(0, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 2);
    CHECK(dests[0] == 0);
    CHECK(dests[1] == 3);

    dests.clear();
    lut.forEachDestination(1, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 4);
    CHECK(dests[0] == 1);
    CHECK(dests[1] == 2);
    CHECK(dests[2] == 5);
    CHECK(dests[3] == 7);

    dests.clear();
    lut.forEachDestination(2, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 1);
    CHECK(dests[0] == 4);

    dests.clear();
    lut.forEachDestination(3, [&](mm::nrOfLightsType idx) { dests.push_back(idx); });
    REQUIRE(dests.size() == 3);
    CHECK(dests[0] == 6);
    CHECK(dests[1] == 8);
    CHECK(dests[2] == 9);
}

// free() releases memory and resets counts; build() can be called again to install a fresh mapping.
TEST_CASE("MappingLUT free and rebuild") {
    mm::MappingLUT lut;
    lut.build(4, 8);
    lut.free();
    CHECK(!lut.hasLUT());
    CHECK(lut.logicalCount() == 0);

    // Can rebuild after free
    CHECK(lut.build(2, 4));
    mm::nrOfLightsType map[] = {0, 1};
    lut.setMapping(0, map, 2);
    lut.setMapping(1, map, 2);
    lut.finalize();
    CHECK(lut.logicalCount() == 2);
}
