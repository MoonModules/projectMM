// @module draw

#include "doctest.h"
#include "light/draw.h"

#include <cstdlib>

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

// mm::draw::pixel() writes inside the grid and silently clips outside it (no out-of-bounds write).
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

// The `shorten` parameter pulls the far endpoint back toward `a` by shorten/255 (with WLEDMM *2
// rounding), so an effect can sweep a partial segment. For a→b = (0,0)→(8,0): shorten 255 draws the
// whole line (tip at 8), 128 ≈ half (tip at (16*128/255+1)/2 = 4), 1 = just the start pixel (tip 0),
// 0 = nothing. This pins the rounding of the shorten branch.
TEST_CASE("draw: line shorten pulls the far endpoint back toward the start") {
    Coord3D dims{9, 1, 1};
    auto litUpTo = [&](uint8_t shorten) {
        Buffer buf; REQUIRE(buf.allocate(9, 3));
        draw::line(buf, dims, {0, 0, 0}, {8, 0, 0}, {0, 0, 255}, shorten);
        int last = -1;
        for (lengthType x = 0; x < 9; x++) if (!isBlack(at(buf, dims, x, 0, 0))) last = x;
        return last;   // highest lit x, or -1 if nothing drawn
    };
    CHECK(litUpTo(255) == 8);   // full line reaches the far endpoint
    CHECK(litUpTo(128) == 4);   // ~half: tip pulled back to x=4
    CHECK(litUpTo(1)   == 0);   // only the start pixel
    CHECK(litUpTo(0)   == -1);  // shorten 0 draws nothing
    // Monotonic: a larger shorten never draws a shorter segment.
    CHECK(litUpTo(200) >= litUpTo(100));
}

namespace {
// Reference blur (the FastLED blur1d carryover-seep, written the slow-but-obvious way) along x for
// one row, used to pin draw::blur's fast byte-level pass to the canonical behavior. Mirrors
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
// behavior as FastLED blur1d / MoonLight blurRows), and is symmetric around a centerd bright pixel.
TEST_CASE("draw: blur matches the reference carryover-seep on a 1D row") {
    Buffer got, ref;
    Coord3D dims{5, 1, 1};
    REQUIRE(got.allocate(5, 3));
    REQUIRE(ref.allocate(5, 3));
    // A single white pixel in the center of both buffers.
    draw::pixel(got, dims, {2, 0, 0}, {255, 255, 255});
    draw::pixel(ref, dims, {2, 0, 0}, {255, 255, 255});

    draw::blur(got, dims, 128);
    refBlurRowX(ref, dims, 0, 0, 128);

    for (lengthType x = 0; x < 5; x++) {
        const RGB g = at(got, dims, x, 0, 0), r = at(ref, dims, x, 0, 0);
        CHECK(g.r == r.r); CHECK(g.g == r.g); CHECK(g.b == r.b);
    }
    // Center stays brightest, the two immediate neighbors are equally lit (symmetry), the center
    // still has the most energy, and it spread outward (neighbors non-black).
    CHECK(at(got, dims, 1, 0, 0).r == at(got, dims, 3, 0, 0).r);
    CHECK(at(got, dims, 2, 0, 0).r > at(got, dims, 1, 0, 0).r);
    CHECK_FALSE(isBlack(at(got, dims, 1, 0, 0)));
    CHECK_FALSE(isBlack(at(got, dims, 3, 0, 0)));
}

// blur runs separably on every axis with extent>1: a 2D blur spreads a center pixel to all four
// orthogonal neighbors; a 3D blur reaches the z neighbors too. And it never writes out of bounds.
TEST_CASE("draw: blur spreads in 2D and 3D and is safe at degenerate sizes") {
    {   // 2D: center pixel of a 5×5 reaches its 4 orthogonal neighbors.
        Buffer buf; Coord3D dims{5, 5, 1};
        REQUIRE(buf.allocate(25, 3));
        draw::pixel(buf, dims, {2, 2, 0}, {255, 255, 255});
        draw::blur(buf, dims, 160);
        CHECK_FALSE(isBlack(at(buf, dims, 1, 2, 0)));   // -x
        CHECK_FALSE(isBlack(at(buf, dims, 3, 2, 0)));   // +x
        CHECK_FALSE(isBlack(at(buf, dims, 2, 1, 0)));   // -y
        CHECK_FALSE(isBlack(at(buf, dims, 2, 3, 0)));   // +y
        // x/y symmetry: the four orthogonal neighbors carry equal energy.
        CHECK(at(buf, dims, 1, 2, 0).r == at(buf, dims, 2, 1, 0).r);
    }
    {   // 3D: the z neighbors light up too.
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
        draw::blur(buf, dims, 255);                     // nothing to seep: must be a safe no-op
        CHECK(at(buf, dims, 0, 0, 0).r == 200);
        draw::blur(buf, dims, 0);                       // amt 0 returns immediately
        CHECK(at(buf, dims, 0, 0, 0).r == 200);
    }
}

// A glyph blits in the correct orientation: neither X-mirrored (a 'b' as a 'd') nor Y-flipped.
// 'L' is the ideal probe: its vertical bar must be on the LEFT and its foot on the BOTTOM row. This
// guards the column-bit and row-direction reads, so the DemoReel name overlay renders each letter
// upright and un-mirrored.
TEST_CASE("draw: glyph renders upright and un-mirrored (the 'L' probe)") {
    Buffer buf;
    const auto& f = fonts::kFont6x8;                 // 6x8: 'L' bar at col 1, foot on row 6
    Coord3D dims{static_cast<lengthType>(f.width), static_cast<lengthType>(f.height), 1};
    REQUIRE(buf.allocate(f.width * f.height, 3));
    draw::glyph(buf, dims, f, 'L', 0, 0, {255, 255, 255});

    // The vertical bar is on the LEFT (column 1 lit down the height), not mirrored to the right.
    int litLeft = 0, litRight = 0;
    for (lengthType y = 0; y < f.height; y++) {
        if (!isBlack(at(buf, dims, 1, y, 0))) litLeft++;                       // left bar column
        if (!isBlack(at(buf, dims, static_cast<lengthType>(f.width - 1), y, 0))) litRight++;  // right edge
    }
    CHECK(litLeft >= 6);    // the bar runs down most of the glyph on the left
    CHECK(litRight <= 1);   // the right edge only carries the foot's last pixel (not a mirrored bar)

    // The foot is on the BOTTOM row (highest y), and the top row is just the bar (not the foot).
    int bottomLit = 0;
    for (lengthType x = 0; x < f.width; x++)
        if (!isBlack(at(buf, dims, x, static_cast<lengthType>(f.height - 2), 0))) bottomLit++;  // row 6 (row 7 is blank)
    CHECK(bottomLit >= 4);  // the foot spans several columns near the bottom
    // The top row has only the single bar pixel, not the foot: so top != bottom (Y not flipped).
    int topLit = 0;
    for (lengthType x = 0; x < f.width; x++)
        if (!isBlack(at(buf, dims, x, 0, 0))) topLit++;
    CHECK(topLit == 1);     // just the bar at the top
}

// draw::sprite, the multi-color sibling of glyph: palette-indexed frames with index 0 as the
// transparent key. These pin the contract a screensaver effect stands on: the right frame at
// the right place, holes where the key is, silence at the edges and on bad indices.
TEST_CASE("draw::sprite blits the requested frame with transparent holes") {
    Buffer buf;
    Coord3D dims{6, 4, 1};
    REQUIRE(buf.allocate(static_cast<nrOfLightsType>(dims.x) * dims.y, 3));
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, dims.x, dims.y, 1);

    static constexpr RGB pal[] = {{0, 0, 0}, {10, 20, 30}, {40, 50, 60}};
    // 2x2, 2 frames: frame 0 all color 1; frame 1 = color 2 with a transparent hole at (1,0).
    static constexpr uint8_t px[] = {1, 1, 1, 1,   2, 0, 2, 2};
    constexpr draw::sprites::Sprite s{px, pal, 2, 2, 2, 3};

    draw::sprite(cv, s, 1, 1, 1);
    CHECK(at(buf, dims, 1, 1, 0).r == 40);          // frame 1's color, not frame 0's
    CHECK(isBlack(at(buf, dims, 2, 1, 0)));          // the transparent hole
    CHECK(at(buf, dims, 1, 2, 0).r == 40);
    CHECK(at(buf, dims, 2, 2, 0).r == 40);
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));          // untouched background
}

TEST_CASE("draw::sprite clips at every edge and survives bad frame and palette indices") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(static_cast<nrOfLightsType>(dims.x) * dims.y, 3));
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, dims.x, dims.y, 1);

    static constexpr RGB pal[] = {{0, 0, 0}, {99, 0, 0}};
    static constexpr uint8_t px[] = {1, 1, 1, 1};   // 2x2, 1 frame, all color 1
    constexpr draw::sprites::Sprite s{px, pal, 2, 2, 1, 2};

    draw::sprite(cv, s, 0, -1, -1);                  // upper-left: only (0,0) lands
    CHECK(at(buf, dims, 0, 0, 0).r == 99);
    draw::sprite(cv, s, 0, 3, 3);                    // lower-right: only (3,3) lands
    CHECK(at(buf, dims, 3, 3, 0).r == 99);
    CHECK(isBlack(at(buf, dims, 2, 2, 0)));

    draw::sprite(cv, s, 200, 1, 1);                  // out-of-range frame clamps to the last
    CHECK(at(buf, dims, 1, 1, 0).r == 99);

    static constexpr uint8_t bad[] = {9, 9, 9, 9};   // indices past the palette: render nothing
    constexpr draw::sprites::Sprite sBad{bad, pal, 2, 2, 1, 2};
    buf.clear();
    draw::sprite(cv, sBad, 0, 1, 1);
    CHECK(isBlack(at(buf, dims, 1, 1, 0)));
}

// Art that faces one way serves both: flipX mirrors the sprite's READ, so the blit still lands
// at the same (x, y) with the same footprint rather than needing a mirrored copy of every frame.
TEST_CASE("draw::sprite mirrors horizontally without moving the sprite") {
    Buffer buf;
    Coord3D dims{4, 2, 1};
    REQUIRE(buf.allocate(static_cast<nrOfLightsType>(dims.x) * dims.y, 3));
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, dims.x, dims.y, 1);

    // Asymmetric on purpose: one lit pixel at the LEFT of the top row.
    static constexpr RGB pal[] = {{0, 0, 0}, {10, 20, 30}};
    static constexpr uint8_t px[] = {1, 0, 0, 0,
                                     0, 0, 0, 0};
    constexpr draw::sprites::Sprite s{px, pal, 4, 2, 1, 2};

    draw::sprite(cv, s, 0, 0, 0, 1, /*flipX=*/false);
    CHECK(at(buf, dims, 0, 0, 0).r == 10);       // unflipped: leftmost column
    CHECK(isBlack(at(buf, dims, 3, 0, 0)));

    buf.clear();
    draw::sprite(cv, s, 0, 0, 0, 1, /*flipX=*/true);
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));      // flipped: the same footprint, mirrored
    CHECK(at(buf, dims, 3, 0, 0).r == 10);
}

// draw::decay is the framerate-independent trail fade: a duration, not a per-frame amount. These
// pin the property a user actually sees, which is that the same effect looks the same on a slow
// device and a fast one.

TEST_CASE("decay dims a plane by half over one half-life at a realistic frame time") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(16, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 4, 4, 1);
    draw::fill(cv, RGB{200, 200, 200});
    for (int i = 0; i < 10; i++) draw::decay(cv, 500, 50);      // 500 ms at a 20 fps cadence
    const RGB c = at(buf, dims, 1, 1, 0);
    CHECK(c.r > 90);                       // half of 200, allowing for integer rounding
    CHECK(c.r < 105);
}

TEST_CASE("a 16-bit trail plane decays at the same rate whatever the framerate") {
    // The property a byte plane CANNOT hold: re-rounding a byte hundreds of times a second either
    // erases the trail (truncating) or freezes it solid (rounding). Measured, decaying 200 over a
    // 500 ms half-life in 500 ms of frames, where the exact answer is 100: a byte plane gives 96 at
    // 50 ms frames, 73 at 5 ms and 0 at 1 ms. The wide plane below holds 100/101/102.
    auto runWide = [](int frames, uint32_t dt) {
        uint16_t plane[16];
        for (uint16_t& v : plane) v = 200 * 257;               // 200 widened to 16 bits
        for (int i = 0; i < frames; i++) draw::decay16(plane, 16, 500, dt);
        return static_cast<int>(plane[5] / 257);               // narrowed back to a byte
    };
    const int slow = runWide(10, 50);       // 20 fps
    const int mid  = runWide(100, 5);       // 200 fps
    const int fast = runWide(500, 1);       // 1000 fps
    CHECK(slow > 95);
    CHECK(slow < 105);
    CHECK(std::abs(slow - mid) <= 3);
    CHECK(std::abs(slow - fast) <= 3);      // the framerate independence the half-life form is for
}

TEST_CASE("decay leaves a plane alone when no time has passed, and clears it after a long stall") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(16, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 4, 4, 1);
    draw::fill(cv, RGB{123, 45, 67});

    draw::decay(cv, 500, 0);               // a frame that took no time changes nothing
    CHECK(at(buf, dims, 0, 0, 0).r == 123);
    draw::decay(cv, 0, 100);               // no half-life asked for: also nothing
    CHECK(at(buf, dims, 0, 0, 0).g == 45);

    draw::decay(cv, 10, 100000);           // a long stall goes black rather than wrapping bright
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));
}

// draw::advect moves a plane along a velocity field: the transport half of a flow, and what a
// trail is made of. It samples BACKWARD, so every destination pixel is written exactly once.

TEST_CASE("advect carries the picture along the flow, one whole pixel at a time") {
    Buffer src, dst;
    Coord3D dims{8, 8, 1};
    REQUIRE(src.allocate(64, 3));
    REQUIRE(dst.allocate(64, 3));
    const draw::Canvas s = draw::Canvas::of(src, 8, 8, 1);
    const draw::Canvas d = draw::Canvas::of(dst, 8, 8, 1);
    draw::pixel(s, {2, 3, 0}, RGB{200, 100, 50});

    // One pixel to the right per frame, so what was at x=2 must be found at x=3.
    draw::advect(d, s, [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
        vx = draw::kSubOne; vy = 0;
    });
    CHECK(at(dst, dims, 3, 3, 0).r == 200);
    CHECK(isBlack(at(dst, dims, 2, 3, 0)));
}

TEST_CASE("a uniform field survives being advected, so a flow does not dim what it carries") {
    // The property that separates transport from blur: moving a region of equal values must not
    // change them, whatever the sub-pixel offset. A half-pixel step is the worst case, since it
    // blends two neighbors at full weight.
    Buffer src, dst;
    Coord3D dims{8, 8, 1};
    REQUIRE(src.allocate(64, 3));
    REQUIRE(dst.allocate(64, 3));
    const draw::Canvas s = draw::Canvas::of(src, 8, 8, 1);
    const draw::Canvas d = draw::Canvas::of(dst, 8, 8, 1);
    draw::fill(s, RGB{180, 180, 180});
    draw::advect(d, s, [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
        vx = draw::kSubOne / 2; vy = draw::kSubOne / 2;
    });
    CHECK(at(dst, dims, 4, 4, 0).r == 180);
}

TEST_CASE("the edge rule decides whether a flow loops the grid or leaves it") {
    Buffer src, dst;
    Coord3D dims{4, 4, 1};
    REQUIRE(src.allocate(16, 3));
    REQUIRE(dst.allocate(16, 3));
    const draw::Canvas s = draw::Canvas::of(src, 4, 4, 1);
    const draw::Canvas d = draw::Canvas::of(dst, 4, 4, 1);
    draw::pixel(s, {3, 1, 0}, RGB{255, 0, 0});    // lit at the right edge

    auto right = [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
        vx = draw::kSubOne; vy = 0;
    };
    // Wrapping: what leaves the right edge arrives at the left.
    draw::advect(d, s, right, draw::Edge::Wrap);
    CHECK(at(dst, dims, 0, 1, 0).r == 255);
    // Clamping: it does not come back, and the edge column holds its own value instead.
    draw::fill(d, RGB{0, 0, 0});
    draw::advect(d, s, right, draw::Edge::Clamp);
    CHECK(isBlack(at(dst, dims, 0, 1, 0)));
}

TEST_CASE("advect moves every slice of a volume, so a cube flows like a panel") {
    // 3D is the default shape for this phase: the bench fixture is a 20-cube. A D2 rule leaves z
    // alone, and each slice must still be carried.
    Buffer src, dst;
    Coord3D dims{4, 4, 3};
    REQUIRE(src.allocate(48, 3));
    REQUIRE(dst.allocate(48, 3));
    const draw::Canvas s = draw::Canvas::of(src, 4, 4, 3);
    const draw::Canvas d = draw::Canvas::of(dst, 4, 4, 3);
    draw::pixel(s, {1, 1, 0}, RGB{90, 0, 0});
    draw::pixel(s, {1, 1, 2}, RGB{200, 0, 0});     // a different value in the far slice

    draw::advect(d, s, [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
        vx = draw::kSubOne; vy = 0;
    });
    CHECK(at(dst, dims, 2, 1, 0).r == 90);         // each slice carried its own content
    CHECK(at(dst, dims, 2, 1, 2).r == 200);
}

// disc and sphere: the SDF-shaded fills, where a sub-pixel center means a small shape can move
// between cells instead of jumping one at a time.

TEST_CASE("a disc lights its interior and softens its edge") {
    Buffer buf;
    Coord3D dims{9, 9, 1};
    REQUIRE(buf.allocate(81, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 9, 9, 1);
    draw::disc(cv, draw::toSub(4), draw::toSub(4), draw::toSub(3), RGB{255, 255, 255});

    CHECK(at(buf, dims, 4, 4, 0).r == 255);            // the middle is solid
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));            // a far corner is untouched
    // The rim is partial: neither full nor black, which is the anti-aliasing.
    const uint8_t rim = at(buf, dims, 4, 1, 0).r;
    CHECK(rim > 0);
    CHECK(rim < 255);
}

TEST_CASE("two overlapping discs brighten where they meet, because light adds") {
    Buffer buf;
    Coord3D dims{8, 8, 1};
    REQUIRE(buf.allocate(64, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 8, 8, 1);
    draw::disc(cv, draw::toSub(3), draw::toSub(4), draw::toSub(2), RGB{100, 0, 0});
    const uint8_t single = at(buf, dims, 4, 4, 0).r;
    draw::disc(cv, draw::toSub(5), draw::toSub(4), draw::toSub(2), RGB{100, 0, 0});
    CHECK(at(buf, dims, 4, 4, 0).r > single);          // the overlap is brighter than one alone
}

TEST_CASE("a sphere fills a volume, so a cube gets a ball rather than a stack of discs") {
    Buffer buf;
    Coord3D dims{7, 7, 7};
    REQUIRE(buf.allocate(343, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 7, 7, 7);
    draw::sphere(cv, draw::toSub(3), draw::toSub(3), draw::toSub(3), draw::toSub(2),
                 RGB{255, 255, 255});
    CHECK(at(buf, dims, 3, 3, 3).r == 255);            // the center of the volume
    CHECK(at(buf, dims, 3, 3, 1).r > 0);               // and it reaches along z
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));            // but not into the corners
}

// The velocity rules: plain functions, so an effect can drive them from anything.

TEST_CASE("the wind blows every point the same way") {
    draw::pos_t vx = 0, vy = 0;
    draw::flowWind(0, 256, vx, vy);                    // angle 0 is +x
    CHECK(vx > 200);
    CHECK(vy > -20);
    CHECK(vy < 20);
    draw::flowWind(16384, 256, vx, vy);                // a quarter turn is +y
    CHECK(vy > 200);
}

TEST_CASE("a radial flow points away from its center, and inward when reversed") {
    draw::pos_t vx = 0, vy = 0;
    draw::flowRadial(6, 3, 3, 3, 256, vx, vy);         // to the right of center: pushed further right
    CHECK(vx > 100);
    draw::flowRadial(6, 3, 3, 3, -256, vx, vy);        // negative speed draws it back in
    CHECK(vx < -100);
    // The center itself has no direction to move, and must not divide by zero reaching for one.
    draw::flowRadial(3, 3, 3, 3, 256, vx, vy);
    CHECK(vx == 0);
    CHECK(vy == 0);
}

TEST_CASE("a spiral is a radial flow with a turn added, so it both circles and escapes") {
    draw::pos_t rx = 0, ry = 0, sx = 0, sy = 0;
    draw::flowRadial(6, 3, 3, 3, 256, rx, ry);
    draw::flowSpiral(6, 3, 3, 3, 256, 256, sx, sy);
    CHECK(sx == rx);                                   // the outward part is the same
    CHECK(sy != ry);                                   // and the angular part is what it adds
}
