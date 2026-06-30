// @module draw

#include "doctest.h"
#include "light/draw.h"

using namespace mm;

namespace {
// A small helper: read the RGB at (x,y,z) on a w×h×d, cpl=3 buffer.
RGB at(Buffer& b, Coord3D dims, lengthType x, lengthType y, lengthType z) {
    const size_t off = (static_cast<size_t>(z) * dims.y * dims.x
                        + static_cast<size_t>(y) * dims.x + x) * 3;
    const uint8_t* d = b.data();
    return {d[off], d[off + 1], d[off + 2]};
}
bool isBlack(RGB c) { return c.r == 0 && c.g == 0 && c.b == 0; }
}  // namespace

// drawPixel writes inside the grid and silently clips outside it (no out-of-bounds write).
TEST_CASE("draw: pixel writes in-bounds and clips out-of-bounds") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(dims.x * dims.y * dims.z, 3));

    draw::pixel(buf, dims, {2, 1, 0}, {10, 20, 30});
    CHECK(at(buf, dims, 2, 1, 0).r == 10);
    CHECK(at(buf, dims, 2, 1, 0).g == 20);
    CHECK(at(buf, dims, 2, 1, 0).b == 30);

    // Out-of-bounds (negative + past the edge) must be a no-op, not a crash/overwrite.
    draw::pixel(buf, dims, {-1, 0, 0}, {99, 99, 99});
    draw::pixel(buf, dims, {4, 0, 0}, {99, 99, 99});
    draw::pixel(buf, dims, {0, 0, 5}, {99, 99, 99});
    // The one written pixel is still the only non-black one.
    int lit = 0;
    for (lengthType y = 0; y < dims.y; y++)
        for (lengthType x = 0; x < dims.x; x++)
            if (!isBlack(at(buf, dims, x, y, 0))) lit++;
    CHECK(lit == 1);
}

// A 1D line (a row): every pixel from a.x to b.x inclusive is lit.
TEST_CASE("draw: line fills a 1D row inclusive of both endpoints") {
    Buffer buf;
    Coord3D dims{8, 1, 1};
    REQUIRE(buf.allocate(8, 3));
    draw::line(buf, dims, {2, 0, 0}, {6, 0, 0}, {255, 0, 0});
    for (lengthType x = 0; x < 8; x++) {
        const bool shouldLit = (x >= 2 && x <= 6);
        CHECK(isBlack(at(buf, dims, x, 0, 0)) == !shouldLit);
    }
}

// A 2D diagonal: endpoints are lit and the line is contiguous (one pixel per step on the main
// diagonal of a square).
TEST_CASE("draw: line draws a 2D diagonal, endpoints inclusive") {
    Buffer buf;
    Coord3D dims{5, 5, 1};
    REQUIRE(buf.allocate(25, 3));
    draw::line(buf, dims, {0, 0, 0}, {4, 4, 0}, {0, 255, 0});
    // The exact diagonal cells are lit.
    for (lengthType i = 0; i < 5; i++) CHECK_FALSE(isBlack(at(buf, dims, i, i, 0)));
    // An off-diagonal corner is not.
    CHECK(isBlack(at(buf, dims, 4, 0, 0)));
}

// A 3D line: drives all three axes, endpoints lit, no out-of-bounds on a small cube.
TEST_CASE("draw: line spans a 3D cube diagonal") {
    Buffer buf;
    Coord3D dims{4, 4, 4};
    REQUIRE(buf.allocate(64, 3));
    draw::line(buf, dims, {0, 0, 0}, {3, 3, 3}, {0, 0, 255});
    CHECK_FALSE(isBlack(at(buf, dims, 0, 0, 0)));   // start endpoint
    CHECK_FALSE(isBlack(at(buf, dims, 3, 3, 3)));   // end endpoint
}

// A line running off the grid clips: it draws the on-grid part and stops, no crash.
TEST_CASE("draw: a line partly off the grid clips cleanly") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(16, 3));
    draw::line(buf, dims, {2, 2, 0}, {10, 2, 0}, {255, 255, 255});  // runs off the right edge
    CHECK_FALSE(isBlack(at(buf, dims, 2, 2, 0)));   // on-grid start lit
    CHECK_FALSE(isBlack(at(buf, dims, 3, 2, 0)));   // last on-grid cell lit
    // (cells x>=4 don't exist; the test passing without a crash proves the clip)
}
