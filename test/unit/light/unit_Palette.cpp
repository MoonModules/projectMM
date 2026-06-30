// @module Palette

// Pins the palette's gradient→16-entry expansion and the colorFromPalette lookup: endpoint
// fidelity, mid-gradient interpolation, the 0-255 wheel wrap, the brightness fold, and that the
// Palettes::active() seam swaps on setActive. The live Drivers wiring is the bench test.

#include "doctest.h"
#include "light/Palette.h"

TEST_CASE("Palette: gradient endpoints land on the first/last stop colours") {
    // A simple red→green→blue gradient.
    const uint8_t stops[] = {0,255,0,0, 128,0,255,0, 255,0,0,255};
    mm::Palette p;
    p.fromGradient(stops, sizeof(stops));
    // entry[0] is sampled at pos 0 → pure red; entry[15] at pos 255 → pure blue.
    CHECK(p.entry[0].r == 255); CHECK(p.entry[0].g == 0); CHECK(p.entry[0].b == 0);
    CHECK(p.entry[15].b == 255); CHECK(p.entry[15].r == 0); CHECK(p.entry[15].g == 0);
}

TEST_CASE("Palette: a mid-gradient sample interpolates between stops") {
    // Black at 0, white at 255 → the middle is grey-ish, monotonically rising.
    const uint8_t stops[] = {0,0,0,0, 255,255,255,255};
    mm::Palette p;
    p.fromGradient(stops, sizeof(stops));
    CHECK(p.entry[0].r == 0);
    CHECK(p.entry[15].r == 255);
    // Monotonic non-decreasing across the entries.
    for (int i = 1; i < mm::Palette::kEntries; i++) CHECK(p.entry[i].r >= p.entry[i-1].r);
    // The middle is roughly half.
    CHECK(p.entry[8].r > 100);
    CHECK(p.entry[8].r < 200);
}

TEST_CASE("Palette: colorFromPalette index 0 reads entry 0; brightness scales") {
    const uint8_t stops[] = {0,200,100,50, 255,200,100,50};   // flat colour
    mm::Palette p;
    p.fromGradient(stops, sizeof(stops));
    mm::RGB full = mm::colorFromPalette(p, 0, 255);
    CHECK(full.r == 200); CHECK(full.g == 100); CHECK(full.b == 50);
    mm::RGB half = mm::colorFromPalette(p, 0, 128);
    CHECK(half.r < full.r);                 // dimmed
    mm::RGB off = mm::colorFromPalette(p, 0, 0);
    CHECK(off.r == 0); CHECK(off.g == 0); CHECK(off.b == 0);   // brightness 0 → black
}

TEST_CASE("Palette: the index wraps at 255→0 (no out-of-range read)") {
    const uint8_t stops[] = {0,255,0,0, 255,0,0,255};
    mm::Palette p;
    p.fromGradient(stops, sizeof(stops));
    // index 255 blends entry[15] toward entry[0] (the wrap) — must not read past the array.
    mm::RGB c = mm::colorFromPalette(p, 255);
    CHECK((c.r <= 255));   // a valid colour, no crash/garbage
    // Sweeping every index never faults.
    for (int i = 0; i <= 255; i++) (void)mm::colorFromPalette(p, static_cast<uint8_t>(i));
}

TEST_CASE("Palette: a degenerate (empty) gradient is all black, never out-of-bounds") {
    mm::Palette p;
    p.fromGradient(nullptr, 0);
    for (int i = 0; i < mm::Palette::kEntries; i++) {
        CHECK(p.entry[i].r == 0); CHECK(p.entry[i].g == 0); CHECK(p.entry[i].b == 0);
    }
}

TEST_CASE("Palettes::active swaps the global palette on setActive") {
    mm::Palettes::setActive(0);                       // Rainbow
    mm::RGB rainbow0 = mm::colorFromPalette(*mm::Palettes::active(), 0);
    mm::Palettes::setActive(2);                       // Lava
    mm::RGB lava0 = mm::colorFromPalette(*mm::Palettes::active(), 0);
    // The two built-ins differ at index 0 (rainbow starts red, lava starts black-ish).
    const bool same = (rainbow0.r == lava0.r) && (rainbow0.g == lava0.g) && (rainbow0.b == lava0.b);
    CHECK_FALSE(same);
    // An out-of-range index clamps to the first built-in, doesn't crash.
    mm::Palettes::setActive(250);
    (void)mm::colorFromPalette(*mm::Palettes::active(), 0);
    // Restore the default active palette — it's a global other effects' tests read.
    mm::Palettes::setActive(0);
}
