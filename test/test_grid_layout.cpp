#include "doctest.h"
#include "light/GridLayout.h"

#include <vector>

namespace {

struct CoordEntry {
    mm::nrOfLightsType idx;
    mm::lengthType x, y, z;
};

void collectCoord(void* ctx, mm::nrOfLightsType idx, mm::lengthType x, mm::lengthType y, mm::lengthType z) {
    static_cast<std::vector<CoordEntry>*>(ctx)->push_back({idx, x, y, z});
}

} // namespace

TEST_CASE("GridLayout 4x4x1 produces 16 coords in row-major order") {
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;

    CHECK(grid.lightCount() == 16);

    std::vector<CoordEntry> coords;
    grid.forEachCoord(collectCoord, &coords);

    REQUIRE(coords.size() == 16);

    // First row: x=0..3, y=0, z=0
    CHECK(coords[0].idx == 0);
    CHECK(coords[0].x == 0);
    CHECK(coords[0].y == 0);
    CHECK(coords[0].z == 0);

    CHECK(coords[1].idx == 1);
    CHECK(coords[1].x == 1);
    CHECK(coords[1].y == 0);

    CHECK(coords[3].idx == 3);
    CHECK(coords[3].x == 3);
    CHECK(coords[3].y == 0);

    // Second row: y=1
    CHECK(coords[4].idx == 4);
    CHECK(coords[4].x == 0);
    CHECK(coords[4].y == 1);

    // Last coord
    CHECK(coords[15].idx == 15);
    CHECK(coords[15].x == 3);
    CHECK(coords[15].y == 3);
    CHECK(coords[15].z == 0);
}

TEST_CASE("GridLayout 2x2x2 produces 8 coords with z") {
    mm::GridLayout grid;
    grid.width = 2;
    grid.height = 2;
    grid.depth = 2;

    CHECK(grid.lightCount() == 8);

    std::vector<CoordEntry> coords;
    grid.forEachCoord(collectCoord, &coords);

    REQUIRE(coords.size() == 8);

    // z=0 plane
    CHECK(coords[0].z == 0);
    CHECK(coords[3].z == 0);

    // z=1 plane
    CHECK(coords[4].z == 1);
    CHECK(coords[4].x == 0);
    CHECK(coords[4].y == 0);

    CHECK(coords[7].z == 1);
    CHECK(coords[7].x == 1);
    CHECK(coords[7].y == 1);
}

TEST_CASE("GridLayout 1x1x1 edge case") {
    mm::GridLayout grid;
    grid.width = 1;
    grid.height = 1;
    grid.depth = 1;

    CHECK(grid.lightCount() == 1);

    std::vector<CoordEntry> coords;
    grid.forEachCoord(collectCoord, &coords);

    REQUIRE(coords.size() == 1);
    CHECK(coords[0].idx == 0);
    CHECK(coords[0].x == 0);
    CHECK(coords[0].y == 0);
    CHECK(coords[0].z == 0);
}

TEST_CASE("LayoutGroup with one layout") {
    mm::LayoutGroup group;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;

    group.addLayout(&grid);
    CHECK(group.totalLightCount() == 16);

    std::vector<CoordEntry> coords;
    group.forEachCoord(collectCoord, &coords);
    CHECK(coords.size() == 16);
}

TEST_CASE("LayoutGroup with two layouts offsets physical indices") {
    mm::LayoutGroup group;

    mm::GridLayout grid1;
    grid1.width = 2;
    grid1.height = 2;
    grid1.depth = 1;

    mm::GridLayout grid2;
    grid2.width = 3;
    grid2.height = 1;
    grid2.depth = 1;

    group.addLayout(&grid1);
    group.addLayout(&grid2);

    CHECK(group.totalLightCount() == 7);

    std::vector<CoordEntry> coords;
    group.forEachCoord(collectCoord, &coords);

    REQUIRE(coords.size() == 7);

    // First layout: indices 0-3
    CHECK(coords[0].idx == 0);
    CHECK(coords[3].idx == 3);

    // Second layout: indices 4-6 (offset by 4)
    CHECK(coords[4].idx == 4);
    CHECK(coords[4].x == 0);
    CHECK(coords[6].idx == 6);
    CHECK(coords[6].x == 2);
}
