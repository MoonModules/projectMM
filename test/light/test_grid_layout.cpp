#include "doctest.h"
#include "light/modules/layouts/GridLayout.h"
#include <vector>

using namespace mm::light;

TEST_CASE("GridLayout default dimensions") {
    GridLayout grid;
    grid.addControls();
    CHECK(grid.width() == 128);
    CHECK(grid.height() == 128);
    CHECK(grid.depth() == 1);
    CHECK(grid.pixelCount() == 16384);
    CHECK(grid.controlCount() == 3);
}

TEST_CASE("GridLayout forEachCoord yields correct coordinates") {
    GridLayout grid;
    grid.addControls();
    grid.setControl(0, uint16_t(3)); // width
    grid.setControl(1, uint16_t(2)); // height
    grid.setControl(2, uint16_t(1)); // depth

    std::vector<std::tuple<uint32_t, int16_t, int16_t, int16_t>> coords;
    grid.forEachCoord([&](uint32_t idx, int16_t x, int16_t y, int16_t z) {
        coords.emplace_back(idx, x, y, z);
    });

    REQUIRE(coords.size() == 6);
    CHECK(std::get<0>(coords[0]) == 0); // idx
    CHECK(std::get<1>(coords[0]) == 0); // x
    CHECK(std::get<2>(coords[0]) == 0); // y
    CHECK(std::get<1>(coords[1]) == 1); // x=1
    CHECK(std::get<1>(coords[3]) == 0); // x=0, second row
    CHECK(std::get<2>(coords[3]) == 1); // y=1
}

TEST_CASE("GridLayout change width updates pixelCount") {
    GridLayout grid;
    grid.addControls();
    CHECK(grid.pixelCount() == 16384);
    grid.setControl(0, uint16_t(8));
    CHECK(grid.pixelCount() == 1024);
}

TEST_CASE("GridLayout 3D") {
    GridLayout grid;
    grid.addControls();
    grid.setControl(0, uint16_t(4));
    grid.setControl(1, uint16_t(4));
    grid.setControl(2, uint16_t(4));
    CHECK(grid.pixelCount() == 64);

    // Verify z coordinate is set
    bool foundZ = false;
    grid.forEachCoord([&](uint32_t, int16_t, int16_t, int16_t z) {
        if (z > 0) foundZ = true;
    });
    CHECK(foundZ);
}
