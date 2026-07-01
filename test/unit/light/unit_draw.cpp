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

namespace {
// Reference blur (the FastLED blur1d carryover-seep, written the slow-but-obvious way) along x for
// one row, used to pin draw::blur's fast byte-level pass to the canonical behaviour. Mirrors
// MoonLight's blurRows for a single row.
void refBlurRowX(Buffer& b, Coord3D dims, lengthType y, lengthType z, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt), seep = static_cast<uint8_t>(amt >> 1);
    uint8_t* d = b.data();
    auto at3 = [&](lengthType x) { return d + (static_cast<size_t>(z) * dims.y * dims.x + static_cast<size_t>(y) * dims.x + x) * 3; };
    uint8_t cr = 0, cg = 0, cb = 0;
    for (lengthType x = 0; x < dims.x; x++) {
        uint8_t* px = at3(x);
        const uint8_t pr = scale8(px[0], seep), pg = scale8(px[1], seep), pb = scale8(px[2], seep);
        px[0] = qadd8(scale8(px[0], keep), cr); px[1] = qadd8(scale8(px[1], keep), cg); px[2] = qadd8(scale8(px[2], keep), cb);
        if (x) { uint8_t* pv = at3(x - 1); pv[0] = qadd8(pv[0], pr); pv[1] = qadd8(pv[1], pg); pv[2] = qadd8(pv[2], pb); }
        cr = pr; cg = pg; cb = pb;
    }
    if (dims.x) { uint8_t* last = at3(dims.x - 1); last[0] = qadd8(last[0], cr); last[1] = qadd8(last[1], cg); last[2] = qadd8(last[2], cb); }
}
}  // namespace

// draw::blur on a 1D row matches the canonical carryover-seep reference byte-for-byte (same
// behaviour as FastLED blur1d / MoonLight blurRows), and is symmetric around a centred bright pixel.
TEST_CASE("draw: blur matches the reference carryover-seep on a 1D row") {
    Buffer got, ref;
    Coord3D dims{5, 1, 1};
    REQUIRE(got.allocate(5, 3));
    REQUIRE(ref.allocate(5, 3));
    // A single white pixel in the centre of both buffers.
    draw::pixel(got, dims, {2, 0, 0}, {255, 255, 255});
    draw::pixel(ref, dims, {2, 0, 0}, {255, 255, 255});

    draw::blur(got, dims, 128);
    refBlurRowX(ref, dims, 0, 0, 128);

    for (lengthType x = 0; x < 5; x++) {
        const RGB g = at(got, dims, x, 0, 0), r = at(ref, dims, x, 0, 0);
        CHECK(g.r == r.r); CHECK(g.g == r.g); CHECK(g.b == r.b);
    }
    // Centre stays brightest, the two immediate neighbours are equally lit (symmetry), the centre
    // still has the most energy, and it spread outward (neighbours non-black).
    CHECK(at(got, dims, 1, 0, 0).r == at(got, dims, 3, 0, 0).r);
    CHECK(at(got, dims, 2, 0, 0).r > at(got, dims, 1, 0, 0).r);
    CHECK_FALSE(isBlack(at(got, dims, 1, 0, 0)));
    CHECK_FALSE(isBlack(at(got, dims, 3, 0, 0)));
}

// blur runs separably on every axis with extent>1: a 2D blur spreads a centre pixel to all four
// orthogonal neighbours; a 3D blur reaches the z neighbours too. And it never writes out of bounds.
TEST_CASE("draw: blur spreads in 2D and 3D and is safe at degenerate sizes") {
    {   // 2D: centre pixel of a 5×5 reaches its 4 orthogonal neighbours.
        Buffer buf; Coord3D dims{5, 5, 1};
        REQUIRE(buf.allocate(25, 3));
        draw::pixel(buf, dims, {2, 2, 0}, {255, 255, 255});
        draw::blur(buf, dims, 160);
        CHECK_FALSE(isBlack(at(buf, dims, 1, 2, 0)));   // -x
        CHECK_FALSE(isBlack(at(buf, dims, 3, 2, 0)));   // +x
        CHECK_FALSE(isBlack(at(buf, dims, 2, 1, 0)));   // -y
        CHECK_FALSE(isBlack(at(buf, dims, 2, 3, 0)));   // +y
        // x/y symmetry: the four orthogonal neighbours carry equal energy.
        CHECK(at(buf, dims, 1, 2, 0).r == at(buf, dims, 2, 1, 0).r);
    }
    {   // 3D: the z neighbours light up too.
        Buffer buf; Coord3D dims{3, 3, 3};
        REQUIRE(buf.allocate(27, 3));
        draw::pixel(buf, dims, {1, 1, 1}, {255, 255, 255});
        draw::blur(buf, dims, 160);
        CHECK_FALSE(isBlack(at(buf, dims, 1, 1, 0)));   // -z
        CHECK_FALSE(isBlack(at(buf, dims, 1, 1, 2)));   // +z
    }
    {   // Degenerate: amt=0 is a no-op; 1×1×1 and a single-pixel axis don't crash.
        Buffer buf; Coord3D dims{1, 1, 1};
        REQUIRE(buf.allocate(1, 3));
        draw::pixel(buf, dims, {0, 0, 0}, {200, 100, 50});
        draw::blur(buf, dims, 255);                     // nothing to seep — must be a safe no-op
        CHECK(at(buf, dims, 0, 0, 0).r == 200);
        draw::blur(buf, dims, 0);                       // amt 0 returns immediately
        CHECK(at(buf, dims, 0, 0, 0).r == 200);
    }
}
