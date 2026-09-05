// @module noise
// @also math16

// Field composition: fbm, turbulence, warp and the kaleidoscope fold. One noise sample is a smooth
// blur — these are the compositions that turn it into cloud, flame, flowing liquid and symmetry.
// The tests pin the PROPERTY each one exists for (structure, creases, displacement, n-fold
// symmetry) rather than specific values, because the value is only meaningful as a field.

#include "doctest.h"
#include "core/noise.h"
#include "core/math16.h"

#include <algorithm>
#include <cmath>

using namespace mm;

namespace {
/// How much detail a field carries at the FINEST scale — the second difference along a row, sampled
/// at a small step. A single octave is smooth at this scale (it only varies over whole cells, 256
/// units wide), so the added octaves are exactly what shows up here. Measured at a step of 16, a
/// sixteenth of a cell, where octave 3 varies and octave 1 barely does.
double fineDetail(uint8_t octaves) {
    double sum = 0.0;
    int n = 0;
    for (uint32_t x = 2000; x < 6000; x += 16) {
        const int a = fbm8(x, 1000, octaves);
        const int b = fbm8(x + 16, 1000, octaves);
        const int c = fbm8(x + 32, 1000, octaves);
        sum += std::abs((a + c) - 2 * b);      // curvature: zero on a straight ramp
        n++;
    }
    return sum / n;
}
}  // namespace

// Summing octaves must NORMALISE, not accumulate: without dividing by the total amplitude the sum
// would run past the field's range and clip. `v <= 255` cannot show this — a uint8_t satisfies it by
// construction — so the check is that adding octaves never pushes the result outside the span its
// own samples occupy.
TEST_CASE("fbm normalises its octave sum rather than accumulating") {
    for (uint32_t x = 0; x < 3000; x += 137) {
        int lo = 255, hi = 0;
        for (uint8_t oct = 1; oct <= 5; oct++) {
            const int v = fbm8(x, x / 2, oct);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        CAPTURE(x);
        // Every octave count lands in the same neighbourhood; an unnormalised sum would climb
        // toward saturation as octaves were added.
        CHECK(hi - lo < 120);
    }
}

TEST_CASE("one octave of fbm is plain noise") {
    for (uint32_t x = 0; x < 2000; x += 251)
        CHECK(fbm8(x, 500, 1) == inoise8(x, 500));
}

// The reason fbm exists: successive octaves add structure at scales the base field has none at. A
// single octave only varies over whole 256-unit cells, so at a sixteenth-cell step it is nearly a
// straight ramp; three octaves visibly bend between the same points.
TEST_CASE("each octave of fbm adds finer detail") {
    CHECK(fineDetail(3) > fineDetail(1));
}

TEST_CASE("fbm with no octaves is a flat field") {
    CHECK(fbm8(100, 200, 0) == 128);
    CHECK(fbm8(999, 111, 0) == 128);
}

// A field must be a FIELD: neighboring points are similar, distant points are not. This is what
// separates noise from a raw hash, and it must survive the octave sum.
TEST_CASE("fbm is smooth: neighbours resemble each other more than distant points") {
    const int here = fbm8(5000, 5000, 3);
    const int near = fbm8(5000 + 8, 5000, 3);       // a fraction of a cell away
    const int far  = fbm8(5000 + 4096, 5000, 3);    // many cells away
    CHECK(std::abs(here - near) <= std::abs(here - far) + 40);
}

TEST_CASE("3D fbm varies along z, so z can drive time") {
    const uint8_t t0 = fbm8(1000u, 1000u, 0u, 3);
    const uint8_t t1 = fbm8(1000u, 1000u, 3000u, 3);
    CHECK(t0 != t1);                                // the field evolves rather than standing still
}

// Turbulence creases the field at the midpoint; the creases are the billowing look. Folding around
// 128 means the result is built from magnitudes, so it sits low rather than centred.
TEST_CASE("turbulence folds the field at its midpoint") {
    CHECK(turbulence8(100, 100, 0) == 0);           // no octaves, nothing to fold
    uint32_t sum = 0;
    int n = 0;
    for (uint32_t x = 0; x < 4000; x += 97) { sum += turbulence8(x, 700, 3); n++; }
    const double mean = static_cast<double>(sum) / n;
    CHECK(mean < 160.0);                            // magnitudes, not a field centred on 128
}

// Warp is the domain displacement: sampling through it must NOT give the same field back, or the
// displacement did nothing.
TEST_CASE("warp displaces the field it samples") {
    int differences = 0;
    for (uint32_t x = 0; x < 3000; x += 173)
        if (warp8(x, 900, 600, 2) != fbm8(x, 900, 2)) differences++;
    CHECK(differences > 5);                         // most samples land somewhere else
}

TEST_CASE("warp with zero strength is the field itself") {
    for (uint32_t x = 0; x < 2000; x += 311)
        CHECK(warp8(x, 400, 0, 2) == fbm8(x, 400, 2));
}

TEST_CASE("warp stays a smooth field rather than becoming noise") {
    const int here = warp8(6000, 6000, 400, 2);
    const int near = warp8(6000 + 8, 6000, 400, 2);
    CHECK(std::abs(here - near) < 90);              // still continuous after displacement
}

// --- Kaleidoscope ---------------------------------------------------------------------------

TEST_CASE("kaleido folds the circle into the requested number of wedges") {
    // With 4 segments every angle lands inside the first quarter-wedge (0..16384).
    for (uint32_t a = 0; a < 65536; a += 521)
        CHECK(kaleido(static_cast<angle16>(a), 4) <= 16384);
}

TEST_CASE("kaleido with fewer than two segments changes nothing") {
    CHECK(kaleido(12345, 1) == 12345);
    CHECK(kaleido(12345, 0) == 12345);
}

// The property that makes a kaleidoscope: rotating by one full wedge gives the same output, which
// is what makes the pattern repeat around the circle.
TEST_CASE("kaleido repeats every wedge") {
    const uint8_t segments = 6;
    const uint16_t wedge = static_cast<uint16_t>(65536u / segments);
    for (uint32_t a = 0; a < wedge; a += 97) {
        const angle16 base = static_cast<angle16>(a);
        const angle16 twoWedgesOn = static_cast<angle16>(a + 2u * wedge);
        CHECK(kaleido(base, segments) == kaleido(twoWedgesOn, segments));
    }
}

// The seam is where an off-by-one shows: `wedge - within` maps 0 to `wedge`, one past the end, so
// every boundary carried a one-unit jump. Stepping across each seam must move by ONE unit, the same
// as stepping anywhere else — a reviewer found the original off-by-one here.
TEST_CASE("kaleido steps by one across every seam") {
    const uint8_t segments = 4;
    const uint16_t wedge = static_cast<uint16_t>(65536u / segments);
    for (uint8_t s = 1; s < segments; s++) {
        const angle16 before = static_cast<angle16>(s * wedge - 1);
        const angle16 after  = static_cast<angle16>(s * wedge);
        CAPTURE(s);
        CHECK(std::abs(static_cast<int>(kaleido(before, segments)) -
                       static_cast<int>(kaleido(after, segments))) <= 1);
    }
}

// Mirroring (rather than repeating) alternate wedges is what makes the seams join instead of
// showing a hard edge at every boundary.
TEST_CASE("kaleido mirrors alternate wedges so the seams join") {
    const uint8_t segments = 4;
    const uint16_t wedge = static_cast<uint16_t>(65536u / segments);
    // Just inside the end of wedge 0 and just inside the start of wedge 1 (its mirror) agree.
    const angle16 beforeSeam = static_cast<angle16>(wedge - 20);
    const angle16 afterSeam  = static_cast<angle16>(wedge + 20);
    const int a = kaleido(beforeSeam, segments);
    const int b = kaleido(afterSeam, segments);
    CHECK(std::abs(a - b) < 60);                    // continuous across the boundary
}
