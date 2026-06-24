// @module GridLayout
// @also Layouts

#include "doctest.h"
#include "light/layouts/GridLayout.h"

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

// A 4×4×1 grid yields 16 lights iterated row-major: x sweeps fastest, then y, then z.
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

// Serpentine reverses x on odd rows (boustrophedon), so the strip snakes back and forth: driver
// index advances linearly while the emitted x zigzags. Even rows L→R, odd rows R→L. The COORDINATE
// is always the true (x,y) — only the index→position order changes, which is what makes the
// mapping non-identity.
TEST_CASE("GridLayout serpentine reverses x on odd rows") {
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 3;
    grid.depth = 1;
    grid.serpentine = true;

    std::vector<CoordEntry> coords;
    grid.forEachCoord(collectCoord, &coords);
    REQUIRE(coords.size() == 12);

    // Row 0 (even): left→right, x = 0,1,2,3 at idx 0..3
    CHECK(coords[0].x == 0); CHECK(coords[0].y == 0);
    CHECK(coords[3].x == 3); CHECK(coords[3].y == 0);
    // Row 1 (odd): right→left, x = 3,2,1,0 at idx 4..7 — the serpentine turn
    CHECK(coords[4].idx == 4); CHECK(coords[4].x == 3); CHECK(coords[4].y == 1);
    CHECK(coords[5].x == 2); CHECK(coords[5].y == 1);
    CHECK(coords[7].x == 0); CHECK(coords[7].y == 1);
    // Row 2 (even again): left→right, x = 0,1,2,3 at idx 8..11
    CHECK(coords[8].x == 0); CHECK(coords[8].y == 2);
    CHECK(coords[11].x == 3); CHECK(coords[11].y == 2);

    // Non-serpentine is unchanged: index i lands at natural box order.
    grid.serpentine = false;
    coords.clear();
    grid.forEachCoord(collectCoord, &coords);
    REQUIRE(coords.size() >= 6);   // guard the index accesses below (clear test failure, not UB)
    CHECK(coords[4].x == 0);   // row 1 starts at x=0 again
    CHECK(coords[5].x == 1);
}

// A 3D 2×2×2 grid yields 8 lights with z-plane separation (indices 0-3 at z=0, 4-7 at z=1).
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

// A single-light grid (1×1×1) is a valid layout: one coordinate at (0,0,0).
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

// Layouts with a single child delegates totalLightCount and forEachCoord to that child directly.
TEST_CASE("Layouts with one layout") {
    mm::Layouts group;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;

    group.addChild(&grid);
    CHECK(group.totalLightCount() == 16);

    std::vector<CoordEntry> coords;
    group.forEachCoord(collectCoord, &coords);
    CHECK(coords.size() == 16);
}

// Two child layouts produce contiguous physical indices: the second layout's coords are offset by the first's lightCount.
TEST_CASE("Layouts with two layouts offsets physical indices") {
    mm::Layouts group;

    mm::GridLayout grid1;
    grid1.width = 2;
    grid1.height = 2;
    grid1.depth = 1;

    mm::GridLayout grid2;
    grid2.width = 3;
    grid2.height = 1;
    grid2.depth = 1;

    group.addChild(&grid1);
    group.addChild(&grid2);

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
