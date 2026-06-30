// @module noise

#include "doctest.h"
#include "core/noise.h"

using namespace mm;

// Determinism: the same coordinate always gives the same value (a pure function of position),
// so a field is reproducible frame to frame and across the 1D/2D/3D entry points at z/y = 0.
TEST_CASE("noise: inoise8 is deterministic and the lower-D calls agree at zero on the extra axes") {
    CHECK(inoise8(1234u) == inoise8(1234u));
    CHECK(inoise8(50u, 80u) == inoise8(50u, 80u));
    CHECK(inoise8(7u, 9u, 11u) == inoise8(7u, 9u, 11u));
    // 2D at y=0 equals 1D at the same x (the hash uses 0 for the absent axes in both).
    CHECK(inoise8(300u, 0u) == inoise8(300u));
    // 3D at z=0 equals 2D at the same (x,y).
    CHECK(inoise8(640u, 128u, 0u) == inoise8(640u, 128u));
}

// Smoothness: neighbouring positions WITHIN a cell (sub-256 steps) differ only a little — that's
// what makes it value noise rather than a raw hash (which would jump randomly every step).
TEST_CASE("noise: inoise8 varies smoothly inside a cell") {
    // Walk across one cell (x from 0x100 to 0x1FF — cell index 1) in small steps; consecutive
    // samples must not jump wildly. (Across a cell BOUNDARY it can change more — that's expected.)
    int maxStep = 0;
    uint8_t prev = inoise8(0x100u);
    for (uint32_t x = 0x110u; x <= 0x1F0u; x += 0x10u) {
        const uint8_t v = inoise8(x);
        const int step = v > prev ? v - prev : prev - v;
        if (step > maxStep) maxStep = step;
        prev = v;
    }
    CHECK(maxStep < 96);                    // smooth: no hash-like full-range jumps within a cell
}

// Range: output is a full byte; over a swept field it uses a wide span (not stuck near one value).
TEST_CASE("noise: inoise8 spans a wide range across a field") {
    uint8_t lo = 255, hi = 0;
    for (uint32_t y = 0; y < 0x800u; y += 0x40u)
        for (uint32_t x = 0; x < 0x800u; x += 0x40u) {
            const uint8_t v = inoise8(x, y);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    CHECK(hi - lo >= 128);                  // a real field, not a flat plane
}
