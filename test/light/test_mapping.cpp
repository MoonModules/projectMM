#include "doctest.h"
#include "light/MappingLUT.h"

using namespace mm::light;

TEST_CASE("MappingLUT 1:1 direct mapping") {
    MappingLUT lut;
    // 4 logical pixels, each maps to one physical pixel
    REQUIRE(lut.allocate(4, 4));

    uint16_t p0 = 0, p1 = 1, p2 = 2, p3 = 3;
    lut.setMapping(0, &p0, 1);
    lut.setMapping(1, &p1, 1);
    lut.setMapping(2, &p2, 1);
    lut.setMapping(3, &p3, 1);
    lut.finalize();

    CHECK(lut.logicalCount() == 4);
    CHECK(lut.destinationCount(0) == 1);
    CHECK(lut.destinationCount(1) == 1);
    CHECK(lut.destinations(0)[0] == 0);
    CHECK(lut.destinations(3)[0] == 3);
}

TEST_CASE("MappingLUT 1:0 skip") {
    MappingLUT lut;
    // 3 logical pixels: first mapped, second skipped, third mapped
    REQUIRE(lut.allocate(3, 2));

    uint16_t p0 = 10;
    lut.setMapping(0, &p0, 1);
    lut.setMapping(1, nullptr, 0);  // skip
    uint16_t p2 = 20;
    lut.setMapping(2, &p2, 1);
    lut.finalize();

    CHECK(lut.destinationCount(0) == 1);
    CHECK(lut.destinationCount(1) == 0);  // skipped
    CHECK(lut.destinationCount(2) == 1);
    CHECK(lut.destinations(0)[0] == 10);
    CHECK(lut.destinations(2)[0] == 20);
}

TEST_CASE("MappingLUT 1:N mirror") {
    MappingLUT lut;
    // 2 logical pixels: first maps to 3 physical, second maps to 1
    REQUIRE(lut.allocate(2, 4));

    uint16_t mirror[] = {5, 10, 15};
    lut.setMapping(0, mirror, 3);
    uint16_t single = 20;
    lut.setMapping(1, &single, 1);
    lut.finalize();

    CHECK(lut.destinationCount(0) == 3);
    CHECK(lut.destinations(0)[0] == 5);
    CHECK(lut.destinations(0)[1] == 10);
    CHECK(lut.destinations(0)[2] == 15);
    CHECK(lut.destinationCount(1) == 1);
    CHECK(lut.destinations(1)[0] == 20);
}

TEST_CASE("MappingLUT out of bounds returns safe values") {
    MappingLUT lut;
    lut.allocate(2, 2);
    uint16_t p = 0;
    lut.setMapping(0, &p, 1);
    lut.setMapping(1, &p, 1);
    lut.finalize();

    CHECK(lut.destinationCount(99) == 0);
    CHECK(lut.destinations(99) == nullptr);
}

TEST_CASE("MappingLUT move") {
    MappingLUT a;
    a.allocate(2, 2);
    uint16_t p0 = 0, p1 = 1;
    a.setMapping(0, &p0, 1);
    a.setMapping(1, &p1, 1);
    a.finalize();

    MappingLUT b(std::move(a));
    CHECK(b.logicalCount() == 2);
    CHECK(b.destinations(0)[0] == 0);
    CHECK(a.logicalCount() == 0);
}

TEST_CASE("MappingLUT free and reuse") {
    MappingLUT lut;
    lut.allocate(4, 4);
    lut.free();
    CHECK(lut.logicalCount() == 0);

    // Reallocate
    REQUIRE(lut.allocate(2, 2));
    CHECK(lut.logicalCount() == 2);
}
