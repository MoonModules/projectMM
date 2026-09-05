// @module noise

#include "doctest.h"
#include "core/noise.h"

#include <cmath>   // std::abs on doubles: GCC does not get it transitively

#include <set>

using namespace mm;

// Determinism: the same coordinate always gives the same value (a pure function of position),
// so a field is reproducible frame to frame and across the 2D/3D entry points at z = 0.
TEST_CASE("noise: inoise8 is deterministic and the lower-D calls agree at zero on the extra axes") {
    CHECK(inoise8(1234u) == inoise8(1234u));
    CHECK(inoise8(50u, 80u) == inoise8(50u, 80u));
    CHECK(inoise8(7u, 9u, 11u) == inoise8(7u, 9u, 11u));
    // 1D is NOT 2D at y=0: it draws ±1 gradients of its own (core/noise.h says why).
    // 3D at z=0 equals 2D at the same (x,y).
    CHECK(inoise8(640u, 128u, 0u) == inoise8(640u, 128u));
}

// Smoothness: neighboring positions WITHIN a cell (sub-256 steps) differ only a little: that's
// what makes it noise rather than a raw hash (which would jump randomly every step).
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

// --- The 16-bit tier ---------------------------------------------------------------------------
//
// The reason it exists: 256 levels band visibly on a large fixture, so a field an effect draws
// against carries 16. Both tests below pin a property that was WRONG when the tier was first
// written, and neither failure was visible from reading the output — only from counting it.

TEST_CASE("16-bit noise fills the range instead of stepping through 256 levels") {
    // fbm16 summed its octaves after shifting each sample down by 8, which fits a 32-bit
    // accumulator but throws away the low byte — the result was an 8-bit field in a uint16_t,
    // exactly the banding this tier removes. Counting distinct values is what catches that: the
    // output LOOKS like plausible noise either way.
    std::set<uint16_t> values;
    int lowByteSet = 0;
    for (uint32_t i = 0; i < 20000; i++) {
        const uint16_t v = fbm16(i * 137u, i * 911u, 4);
        values.insert(v);
        if (v & 0xFF) lowByteSet++;
    }
    CHECK(values.size() > 5000);      // a byte-quantised field tops out at 256
    CHECK(lowByteSet > 1000);         // and never sets the low byte at all
}

TEST_CASE("16-bit interpolation stays exact across the full range") {
    // lerp16's delta*t reaches 4.29e9 against an INT32_MAX of 2.15e9: signed overflow, undefined
    // behaviour, on roughly a quarter of samples. It happened to produce the right low bits on
    // wrap-around hardware, so only the endpoints and the midpoint reveal it.
    CHECK(noise::lerp16(0, 65535, 0) == 0);
    CHECK(noise::lerp16(0, 65535, 65535) == 65534);       // t is a fraction of 65536, not of 65535
    CHECK(noise::lerp16(1000, 1000, 40000) == 1000);      // equal endpoints never move
    // The worst case for the product, from both directions — a wrapped intermediate lands far away.
    CHECK(noise::lerp16(0, 65535, 49152) == 49151);      // exact: integer arithmetic
    CHECK(noise::lerp16(65535, 0, 49152) == 16383);
    // Monotonic: interpolating further along never goes backwards.
    uint16_t prev = 0;
    for (uint32_t t = 0; t <= 65535u; t += 251) {
        const uint16_t v = noise::lerp16(0, 65535, static_cast<uint16_t>(t));
        CHECK(v >= prev);
        prev = v;
    }
}

TEST_CASE("16-bit noise is smooth where the 8-bit form would step") {
    // The whole point of the tier: sampling finer than an 8-bit fraction resolves must produce
    // intermediate values rather than a staircase. Four cells along x at a fixed y, sixteen samples
    // per 8-bit step: a field stepping at 8 bits could show at most 4 * 256 distinct values.
    std::set<uint16_t> values;
    for (uint32_t f = 0; f < 4u * 4096u; f++) values.insert(inoise16(0x10000u + f * 16u, 0x18000u));
    CHECK(values.size() > 2000);
}

TEST_CASE("fbm keeps its full range however many octaves are summed") {
    // Octaves are near-independent, so their spread grows like the root of the sum of squares while
    // the normalizer divides by the sum of amplitudes. Left uncorrected the field narrows with every
    // octave added: 4 octaves measured 54..199 of 0..255, so an effect stretching the top of the
    // field could never reach full brightness and every fbm read flatter than the noise under it.
    for (uint8_t octaves = 1; octaves <= 4; octaves++) {
        uint8_t lo = 255, hi = 0;
        for (uint32_t y = 0; y < 1200; y += 7)
            for (uint32_t x = 0; x < 1200; x += 7) {
                const uint8_t v = fbm8(x * 40, y * 40, octaves);
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        CHECK(lo < 32);        // reaches the dark end
        CHECK(hi > 224);       // and the bright end, at every octave count
    }
}

TEST_CASE("16-bit fbm keeps its range too") {
    for (uint8_t octaves = 1; octaves <= 4; octaves++) {
        uint16_t lo = 65535, hi = 0;
        for (uint32_t y = 0; y < 900; y += 7)
            for (uint32_t x = 0; x < 900; x += 7) {
                const uint16_t v = fbm16(x * 2600, y * 2600, octaves);
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        CHECK(lo < 8192);
        CHECK(hi > 57343);
    }
}

// The contract that makes the field library dimension-generic: a 2D call is the 3D call with the
// missing axis at zero. Without it a volumetric fixture and a panel would sample different fields
// for the same coordinates, and an effect could not simply pass z through.
TEST_CASE("every field kernel's 2D form is its 3D form with z at zero") {
    for (uint32_t y = 0; y < 4000; y += 231) {
        for (uint32_t x = 0; x < 4000; x += 197) {
            CHECK(fbm8(x, y, 2) == fbm8(x, y, 0u, 2));
            CHECK(fbm8(x, y, 4) == fbm8(x, y, 0u, 4));
            CHECK(fbm16(x, y, 2) == fbm16(x, y, 0u, 2));
            CHECK(fbm16(x, y, 3) == fbm16(x, y, 0u, 3));
            CHECK(turbulence8(x, y, 2) == turbulence8(x, y, 0u, 2));
            CHECK(warp8(x, y, 240, 1) == warp8(x, y, 0u, 240, 1));
            CHECK(warp8(x, y, 512, 2) == warp8(x, y, 0u, 512, 2));
        }
    }
}

TEST_CASE("the z axis actually changes the field, rather than being carried and ignored") {
    // The other half of the contract: passing z must do something, or "3D support" is a signature
    // change. A volumetric fixture's slices have to differ from each other.
    int differing = 0, total = 0;
    for (uint32_t y = 0; y < 3000; y += 311) {
        for (uint32_t x = 0; x < 3000; x += 271) {
            total++;
            if (fbm8(x, y, 0u, 2) != fbm8(x, y, 3000u, 2)) differing++;
        }
    }
    REQUIRE(total > 50);
    CHECK(differing * 4 > total * 3);      // three quarters of samples move with z
}

TEST_CASE("a curl field has no sources or sinks, so what it carries cannot pile up") {
    // The reason curl exists rather than sampling noise straight into a velocity. A field with
    // divergence has places where flow converges (anything carried there collects into a clump) and
    // places where it diverges (the medium thins to nothing). Curl is the perpendicular gradient of
    // a potential, so its divergence is zero by construction, and what it carries keeps its shape.
    //
    // Measured here against the naive alternative over the same points: two noise samples used
    // directly as vx and vy.
    double curlDiv = 0, naiveDiv = 0;
    int n = 0;
    constexpr uint32_t kCell = 1u << 16, kEps = 4096;
    for (uint32_t y = kCell * 2; y < kCell * 10; y += kCell / 2) {
        for (uint32_t x = kCell * 2; x < kCell * 10; x += kCell / 2) {
            int32_t ax, ay, bx, by, cx2, cy2, dx2, dy2;
            mm::curl16(x + kEps, y, 1000, ax, ay);
            mm::curl16(x - kEps, y, 1000, bx, by);
            mm::curl16(x, y + kEps, 1000, cx2, cy2);
            mm::curl16(x, y - kEps, 1000, dx2, dy2);
            curlDiv += std::abs(static_cast<double>(ax - bx) + static_cast<double>(cy2 - dy2));

            const auto naive = [](uint32_t a, uint32_t b, bool second) {
                const int32_t v = second ? static_cast<int32_t>(mm::inoise16(a + 0x9E37u, b + 0x7C15u))
                                         : static_cast<int32_t>(mm::inoise16(a, b));
                return static_cast<double>(v - 32768) * 1000.0 / 32768.0;
            };
            naiveDiv += std::abs((naive(x + kEps, y, false) - naive(x - kEps, y, false))
                                 + (naive(x, y + kEps, true) - naive(x, y - kEps, true)));
            n++;
        }
    }
    REQUIRE(n > 0);
    // Two orders of magnitude apart, measured: curl ~0.3, noise-as-velocity ~90.
    CHECK(curlDiv / n < 5.0);
    CHECK(naiveDiv / n > 20.0);
}
