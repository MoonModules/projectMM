// @module draw
// @also EffectBase, Layer

// draw::Canvas binds a buffer to the dimensions that address it. What matters is not that it holds
// three fields, but the two failure modes it removes: a caller cannot pair a buffer with extents
// that do not belong to it, and a 2D layer's zero depth cannot zero the z stride — the bug the
// private `depthDim()` helper guards against in sixteen effects, each with its own copy.

#include "doctest.h"
#include "light/draw.h"
#include "light/effects/SolidEffect.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"

using namespace mm;

namespace {
/// A standalone buffer + canvas, so the addressing rules are testable without a Layer.
struct Surface {
    Buffer buf;
    draw::Canvas cv;
    Surface(lengthType w, lengthType h, lengthType d, uint8_t cpl = 3) {
        buf.allocate(static_cast<nrOfLightsType>(w) * h * (d > 0 ? d : 1), cpl);
        buf.clear();
        cv = draw::Canvas::of(buf, w, h, d);
    }
};
}  // namespace

// The depth guard, which is the whole reason sixteen effects carry a private helper: a 2D layer
// reports depth 0, and an unguarded z stride of 0 collapses every z onto the same plane.
TEST_CASE("Canvas gives a 2D layer a depth of one, not zero") {
    Surface s(8, 4, 0);                       // depth 0 — what a 2D layer reports
    CHECK(s.cv.dims.z == 1);
    // With z=1 the addressing is a plain 2D grid: consecutive rows are w*cpl apart.
    CHECK(s.cv.offsetOf({0, 0, 0}) == 0);
    CHECK(s.cv.offsetOf({0, 1, 0}) == 8u * 3u);
}

TEST_CASE("Canvas addresses x fastest, then y, then z") {
    Surface s(4, 3, 2);
    CHECK(s.cv.offsetOf({1, 0, 0}) == 3u);              // one light along x
    CHECK(s.cv.offsetOf({0, 1, 0}) == 4u * 3u);         // one row
    CHECK(s.cv.offsetOf({0, 0, 1}) == 4u * 3u * 3u);    // one plane
}

// Out-of-grid coordinates report the buffer size, which every draw call treats as "skip" — the
// clipping contract, expressed once instead of at each call site.
TEST_CASE("Canvas reports out-of-grid coordinates as unwritable") {
    Surface s(4, 4, 1);
    CHECK(s.cv.offsetOf({-1, 0, 0}) == s.cv.bytes);
    CHECK(s.cv.offsetOf({0, -1, 0}) == s.cv.bytes);
    CHECK(s.cv.offsetOf({4, 0, 0}) == s.cv.bytes);      // x == width is outside
    CHECK(s.cv.offsetOf({0, 4, 0}) == s.cv.bytes);
    CHECK(s.cv.offsetOf({0, 0, 1}) == s.cv.bytes);      // z == depth on a 2D surface
}

TEST_CASE("Canvas pixel writes land where get reads them, and clip outside") {
    Surface s(6, 6, 1);
    draw::pixel(s.cv, {2, 3, 0}, RGB{10, 20, 30});
    const RGB c = draw::get(s.cv, {2, 3, 0});
    CHECK(c.r == 10);
    CHECK(c.g == 20);
    CHECK(c.b == 30);

    // A write outside the grid is silently dropped, and reading there is black — never a crash and
    // never a stray byte in a neighbouring light (the robustness rule).
    draw::pixel(s.cv, {99, 99, 0}, RGB{255, 255, 255});
    const RGB out = draw::get(s.cv, {99, 99, 0});
    CHECK(out.r == 0);
    CHECK(out.b == 0);
    // ...and the buffer's last light is untouched by that clipped write.
    const RGB last = draw::get(s.cv, {5, 5, 0});
    CHECK(last.r == 0);
}

// A 4-channel (RGBW) surface: the W channel belongs to the driver, so a pixel write leaves it
// alone — the same contract the (Buffer&, dims) form already has, preserved through Canvas.
TEST_CASE("Canvas leaves the white channel of an RGBW light untouched") {
    Surface s(4, 4, 1, 4);
    s.buf.data()[0 * 4 + 3] = 200;                      // a W the driver set
    draw::pixel(s.cv, {0, 0, 0}, RGB{1, 2, 3});
    CHECK(s.buf.data()[0 * 4 + 0] == 1);
    CHECK(s.buf.data()[0 * 4 + 3] == 200);              // W survives
}

// The Canvas and (Buffer&, dims) forms must address identically, or the migration would silently
// move pixels. This is the property the golden-frame tests depend on.
TEST_CASE("Canvas and the buffer+dims form write the same bytes") {
    Surface a(7, 5, 1), b(7, 5, 1);
    const Coord3D dims{7, 5, 1};
    for (lengthType y = 0; y < 5; y++)
        for (lengthType x = 0; x < 7; x++) {
            const RGB c{static_cast<uint8_t>(x * 3), static_cast<uint8_t>(y * 5), 7};
            draw::pixel(a.cv, {x, y, 0}, c);            // Canvas form
            draw::pixel(b.buf, dims, {x, y, 0}, c);     // legacy form
        }
    CHECK(std::memcmp(a.buf.data(), b.buf.data(), a.buf.bytes()) == 0);
}

// EffectBase::canvas() is what removes the per-effect preamble, so it must report the layer's live
// extents — including the depth guard — rather than anything cached.
TEST_CASE("EffectBase canvas reflects the layer it is attached to") {
    Layouts layouts; GridLayout grid; Layer layer; SolidEffect solid;
    grid.width = 9; grid.height = 5; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&solid);
    layer.applyState();
    layer.tick();

    const draw::Canvas cv = solid.canvas();
    CHECK(cv.dims.x == 9);
    CHECK(cv.dims.y == 5);
    CHECK(cv.dims.z >= 1);                              // never zero, whatever the layer reports
    CHECK(cv.cpl == 3);
    CHECK(cv.data == layer.buffer().data());
    CHECK(cv.bytes == layer.buffer().bytes());
}
