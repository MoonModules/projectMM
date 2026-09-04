// @module draw
// @also Canvas

// draw::splat draws a point at a fractional position by spreading its light over the neighboring
// pixels. What makes it correct rather than merely soft is conservation: a point contributes exactly
// its own brightness, wherever it lands. If the weights summed to more than one pixel's worth, a
// moving point would pulse brighter as it crossed cell boundaries; if less, it would dim.

#include "doctest.h"
#include "light/draw.h"

using namespace mm;

namespace {
struct Surface {
    Buffer buf;
    draw::Canvas cv;
    Surface(lengthType w, lengthType h, lengthType d = 1, uint8_t cpl = 3) {
        buf.allocate(static_cast<nrOfLightsType>(w) * h * (d > 0 ? d : 1), cpl);
        buf.clear();
        cv = draw::Canvas::of(buf, w, h, d);
    }
    /// Total light in the buffer — the quantity a splat must conserve.
    uint32_t total() const {
        uint32_t s = 0;
        for (size_t i = 0; i < buf.bytes(); i++) s += buf.data()[i];
        return s;
    }
    uint8_t at(lengthType x, lengthType y, uint8_t ch = 0) const {
        return buf.data()[(static_cast<size_t>(y) * cv.dims.x + x) * cv.cpl + ch];
    }
};
}  // namespace

TEST_CASE("splat on an exact pixel lights only that pixel") {
    Surface s(8, 8);
    draw::splat(s.cv, draw::toSub(3), draw::toSub(4), RGB{200, 0, 0});
    CHECK(s.at(3, 4) == 200);
    CHECK(s.at(4, 4) == 0);      // no bleed when there is no fraction
    CHECK(s.at(3, 5) == 0);
}

// The point of the primitive: a position between pixels lights both, in proportion.
TEST_CASE("splat between two pixels splits the light by coverage") {
    Surface s(8, 8);
    // Exactly halfway between x=3 and x=4.
    draw::splat(s.cv, draw::toSub(3) + 128, draw::toSub(4), RGB{200, 0, 0});
    const uint8_t left = s.at(3, 4), right = s.at(4, 4);
    CHECK(left > 0);
    CHECK(right > 0);
    CHECK(left == right);        // a halfway point is symmetric
}

TEST_CASE("splat weights follow the distance to each neighbour") {
    Surface s(8, 8);
    // A quarter of the way from x=2 toward x=3: the nearer pixel gets ~3x the light.
    draw::splat(s.cv, draw::toSub(2) + 64, draw::toSub(4), RGB{240, 0, 0});
    const uint8_t nearer = s.at(2, 4), further = s.at(3, 4);   // `near`/`far` are Windows macros
    CHECK(nearer > further);
    CHECK(further > 0);
    CHECK(nearer > 2 * further);       // 3:1 by coverage, allowing for rounding
}

// Conservation, the property that keeps motion smooth: total light is the same wherever the point
// sits, so a moving dot does not pulse as it crosses pixel boundaries.
TEST_CASE("splat conserves total brightness across sub-pixel positions") {
    const uint32_t reference = [] {
        Surface s(8, 8);
        draw::splat(s.cv, draw::toSub(3), draw::toSub(3), RGB{240, 0, 0});
        return s.total();
    }();

    for (int frac = 0; frac < 256; frac += 16) {
        Surface s(8, 8);
        draw::splat(s.cv, draw::toSub(3) + frac, draw::toSub(3) + frac, RGB{240, 0, 0});
        const uint32_t got = s.total();
        // Integer weights round, so allow a few units of slack — but nothing like a doubling or a
        // halving, which is what a weight bug produces.
        CHECK(got > reference - 16);
        CHECK(got < reference + 16);
    }
}

TEST_CASE("splat is additive, so two points on one pixel brighten it") {
    Surface s(8, 8);
    draw::splat(s.cv, draw::toSub(2), draw::toSub(2), RGB{60, 0, 0});
    draw::splat(s.cv, draw::toSub(2), draw::toSub(2), RGB{60, 0, 0});
    CHECK(s.at(2, 2) == 120);
}

TEST_CASE("splat saturates rather than wrapping to black") {
    Surface s(8, 8);
    for (int i = 0; i < 6; i++) draw::splat(s.cv, draw::toSub(1), draw::toSub(1), RGB{200, 0, 0});
    CHECK(s.at(1, 1) == 255);    // clamped, never wrapped
}

// Clipping: a point at or past the edge contributes only the part that lands on the grid, and never
// writes outside it.
TEST_CASE("splat clips at the grid edge without writing outside it") {
    Surface s(4, 4);
    draw::splat(s.cv, draw::toSub(3) + 128, draw::toSub(1), RGB{200, 0, 0});   // half past the last column
    CHECK(s.at(3, 1) > 0);       // the in-grid half landed
    CHECK(s.total() > 0);
    CHECK(s.total() < 200);      // the out-of-grid half was dropped, not wrapped to x=0

    Surface off(4, 4);
    draw::splat(off.cv, draw::toSub(-5), draw::toSub(-5), RGB{255, 255, 255});
    CHECK(off.total() == 0);     // fully outside: nothing drawn, no crash
}

// A strand is the degenerate case the dimension-generic rule promises: the same call works, and the
// axis with extent 1 simply has no second neighbour to share with.
TEST_CASE("splat works on a 1D strand") {
    Surface s(16, 1);
    draw::splat(s.cv, draw::toSub(5) + 128, 0, RGB{200, 0, 0});
    CHECK(s.at(5, 0) > 0);
    CHECK(s.at(6, 0) > 0);
    CHECK(s.at(5, 0) == s.at(6, 0));
}

TEST_CASE("splat spreads over eight neighbours in a volume") {
    Surface s(4, 4, 4);
    // Dead centre of a 2x2x2 cell block: all eight corners get an equal share.
    draw::splat(s.cv, draw::toSub(1) + 128, draw::toSub(1) + 128, draw::toSub(1) + 128,
                RGB{200, 0, 0});
    int lit = 0;
    for (lengthType z = 1; z <= 2; z++)
        for (lengthType y = 1; y <= 2; y++)
            for (lengthType x = 1; x <= 2; x++) {
                const size_t off = (static_cast<size_t>(z) * 4 * 4 + y * 4 + x) * 3;
                if (s.buf.data()[off]) lit++;
            }
    CHECK(lit == 8);
}

TEST_CASE("toPixel floors so a negative fraction lands in the pixel that contains it") {
    CHECK(draw::toPixel(draw::toSub(3)) == 3);
    CHECK(draw::toPixel(draw::toSub(3) + 200) == 3);      // still inside pixel 3
    CHECK(draw::toPixel(-128) == -1);                     // half a pixel left of 0 is pixel -1
    CHECK(draw::toPixel(draw::toSub(-2) + 64) == -2);
}


// An effect must write only the channels its lights have. Three bytes into a 1- or 2-channel buffer
// stays in bounds but spills into the NEXT light, which no crash test can see — the bug class that
// hid in WaveEffect and SolidEffect until a reviewer pointed at it. A canary in the light after the
// written one is what detects it.
TEST_CASE("a per-channel write never spills into the next light") {
    for (uint8_t cpl : {uint8_t{1}, uint8_t{2}}) {
        CAPTURE(cpl);
        Surface s(4, 1, 1, cpl);
        // Mark every byte, then write one light through the primitive under test.
        for (size_t i = 0; i < s.buf.bytes(); i++) s.buf.data()[i] = 0x5A;
        draw::pixel(s.cv, {1, 0, 0}, RGB{10, 20, 30});

        // Light 1 took the colour it could hold...
        CHECK(s.buf.data()[1 * cpl] == 10);
        if (cpl >= 2) CHECK(s.buf.data()[1 * cpl + 1] == 20);   // green, where the light has one
        // ...and light 2 is untouched.
        CHECK(s.buf.data()[2 * cpl] == 0x5A);
    }
}
