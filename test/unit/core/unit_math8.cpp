// @module math8

#include "doctest.h"
#include "core/math8.h"

using namespace mm;

// sin8: a 256-entry sine LUT centred on 128, peaking near 255 and 0 a quarter and three-quarters
// of the way round. cos8 is sin8 shifted a quarter turn.
TEST_CASE("math8: sin8 / cos8 trace a sine over the 256-step circle") {
    CHECK(sin8(0) == 128);                 // zero crossing rising
    CHECK(sin8(64) >= 254);                // peak near +1
    CHECK(sin8(192) <= 1);                 // trough near -1
    CHECK(cos8(0) == sin8(64));            // cos leads sin by a quarter turn
}

// triwave8: linear up 0→255 then down 255→0, peaking at the midpoint.
TEST_CASE("math8: triwave8 is a symmetric triangle") {
    CHECK(triwave8(0) == 0);
    CHECK(triwave8(127) >= 254);           // peak at the midpoint
    CHECK(triwave8(255) == 0);
    CHECK(triwave8(64) == triwave8(static_cast<uint8_t>(255 - 64)));  // symmetric about the peak
}

// qadd8/qsub8 clamp at the 0..255 ends instead of wrapping.
TEST_CASE("math8: qadd8 / qsub8 saturate, never wrap") {
    CHECK(qadd8(200, 100) == 255);         // 300 clamps to 255, not 44
    CHECK(qadd8(10, 20) == 30);            // in range = plain add
    CHECK(qsub8(20, 100) == 0);            // underflow clamps to 0, not 176
    CHECK(qsub8(100, 20) == 80);
}

// nscale8 is the recognisable spelling of scale8 (n/256 channel scale), so nscale8(x,255)==x.
TEST_CASE("math8: nscale8 scales a byte by n/256") {
    CHECK(nscale8(255, 255) == 255);
    CHECK(nscale8(255, 128) == 128);       // half
    CHECK(nscale8(100, 0) == 0);
}

// beat8: a sawtooth completing `bpm` cycles per minute. At t=0 it's 0; halfway through a beat ~128.
TEST_CASE("math8: beat8 ramps 0..255 once per beat") {
    CHECK(beat8(60, 0) == 0);              // start of a beat
    const uint8_t mid = beat8(60, 500);    // 60 bpm = 1000 ms/beat, so 500 ms is half
    CHECK(mid >= 120);
    CHECK(mid <= 135);
    CHECK(beat8(0, 1234) == 0);            // 0 bpm is inert, not a divide-by-zero
}

// beatsin8: a sine oscillating in [low,high] at bpm. Stays in range across the cycle and actually
// moves (not stuck at one value).
TEST_CASE("math8: beatsin8 oscillates within [low,high]") {
    uint8_t lo = 255, hi = 0;
    for (uint32_t ms = 0; ms < 1000; ms += 20) {       // one full beat at 60 bpm
        const uint8_t v = beatsin8(60, ms, 50, 200);
        CHECK(v >= 50);
        CHECK(v <= 200);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    CHECK(hi - lo >= 100);                 // swept most of the range, so it genuinely oscillates
}

// Random8: a seeded PRNG — same seed gives the same sequence (determinism), and below(n) stays
// under n. Two different seeds diverge.
TEST_CASE("math8: Random8 is deterministic per seed and below(n) is bounded") {
    Random8 a(12345), b(12345);
    bool identical = true;
    for (int i = 0; i < 64; i++) if (a.next8() != b.next8()) identical = false;
    CHECK(identical);                      // same seed → same stream

    Random8 r(999);
    for (int i = 0; i < 256; i++) CHECK(r.below(10) < 10);   // bounded

    Random8 c(1), d(2);
    bool diverged = false;
    for (int i = 0; i < 16; i++) if (c.next8() != d.next8()) diverged = true;
    CHECK(diverged);                       // different seeds → different streams
}

// atan2_8 / dist8: the geometry helpers moved here from color.h still behave.
TEST_CASE("math8: atan2_8 and dist8 cover the basics") {
    CHECK(atan2_8(0, 100) == 0);           // +x axis = angle 0
    CHECK(dist8(0, 0) == 0);
    CHECK(dist8(10, 0) == 10);             // axis-aligned = exact
    CHECK(dist8(-10, 0) == 10);            // sign-independent
    CHECK(dist8(10, 10) > 10);             // diagonal longer than one axis
}

// map8 rescales 0..255 onto [lo,hi] inclusively — the top of the input must REACH hi (FastLED's
// map8 == map(in,0,255,lo,hi)). Regression: an earlier scale8-based form left hi unreachable, so a
// one-step span (a bar height of 1) collapsed to 0 — the bug GEQ3D's height mapping hit.
TEST_CASE("math8: map8 reaches both range ends, including one-step spans") {
    CHECK(map8(0, 0, 255) == 0);           // input floor → range floor
    CHECK(map8(255, 0, 255) == 255);       // input top → range top (no wrap on the 256-span)
    CHECK(map8(0, 10, 20) == 10);
    CHECK(map8(255, 10, 20) == 20);        // reaches hi exactly
    CHECK(map8(255, 0, 1) == 1);           // a one-step span: height 1 IS reachable (was 0)
    CHECK(map8(0, 0, 1) == 0);
    CHECK(map8(128, 0, 8) >= 4);           // mid input lands mid range (bar half-height)
}
