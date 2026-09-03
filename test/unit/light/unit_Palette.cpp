// @module Palette

// Pins the palette's gradient→16-entry expansion and the colorFromPalette lookup: endpoint
// fidelity, mid-gradient interpolation, the 0-255 wheel wrap, the brightness fold, and that the
// Palettes::active() seam swaps on setActive. The live Drivers wiring is the bench test.

#include "doctest.h"
#include "light/Palette.h"

#include <cstring>

TEST_CASE("Palette: gradient endpoints land on the first/last stop colors") {
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
    const uint8_t stops[] = {0,200,100,50, 255,200,100,50};   // flat color
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
    CHECK((c.r <= 255));   // a valid color, no crash/garbage
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

// The HomeKit-color-wheel → palette mapping (MQTT/Homebridge). Each palette's representative
// (hue, sat) is computed from its expanded entries; nearestForHue picks the closest. A vivid hue
// snaps to that hue's palette family; a low-saturation target snaps to the desaturated Rainbow.
TEST_CASE("Palettes::nearestForHue maps a color to the closest palette") {
    // A vivid red hue lands on a red/orange-family palette (Party≈13° / Lava≈24° are the reds),
    // never on the all-hue Rainbow (index 0, which has ~0 saturation).
    const uint8_t redIdx = mm::Palettes::nearestForHue(5, 255);
    CHECK(redIdx != 0);
    uint16_t rh = 0, rs = 0; mm::Palettes::representativeHueSat(redIdx, rh, rs);
    CHECK(rh < 45);                       // the picked palette is genuinely red/orange

    // A vivid blue hue lands on a blue-family palette (Ocean≈195° / Fierce Ice≈213°).
    const uint8_t blueIdx = mm::Palettes::nearestForHue(210, 255);
    CHECK(blueIdx != 0);
    uint16_t bh = 0, bs = 0; mm::Palettes::representativeHueSat(blueIdx, bh, bs);
    CHECK(bh > 150);
    CHECK(bh < 260);                      // genuinely blue, not green or red

    // A vivid green hue lands on a green-family palette (Forest≈111°).
    const uint8_t greenIdx = mm::Palettes::nearestForHue(120, 255);
    uint16_t gh = 0, gs = 0; mm::Palettes::representativeHueSat(greenIdx, gh, gs);
    CHECK(gh > 60);
    CHECK(gh < 180);

    // Very low saturation (the wheel's desaturated centre) → the low-sat Rainbow (index 0).
    CHECK(mm::Palettes::nearestForHue(0, 5) == 0);

    // Hue wraps: 359° is adjacent to 0°, so it picks the same red family as ~0°.
    const uint8_t wrapIdx = mm::Palettes::nearestForHue(359, 255);
    uint16_t wh = 0, ws = 0; mm::Palettes::representativeHueSat(wrapIdx, wh, ws);
    CHECK((wh < 45 || wh > 315));         // red/orange either side of the 0/360 seam
}

// Regression (reviewer #4): a hue >= 360 (a broker client can send "400,…" on hsv/set) must not
// overflow the int32 squared-distance math — nearestForHue folds any input into 0..359 up front.
// 400 % 360 == 40 (orange), 720 % 360 == 0 (red), so both resolve to a valid, sensible index.
TEST_CASE("Palettes::nearestForHue folds an out-of-range hue instead of overflowing") {
    const uint8_t i400 = mm::Palettes::nearestForHue(400, 255);   // == 40°
    const uint8_t i40  = mm::Palettes::nearestForHue(40, 255);
    CHECK(i400 == i40);                    // 400 folds to 40 → same palette
    const uint8_t i720 = mm::Palettes::nearestForHue(720, 255);   // == 0°
    const uint8_t i0   = mm::Palettes::nearestForHue(0, 255);
    CHECK(i720 == i0);                     // 720 folds to 0 → same palette
    // A large value doesn't crash / return garbage — any index < kCount is acceptable.
    CHECK(mm::Palettes::nearestForHue(65535, 200) < mm::palettes::kCount);
}

// Regression: /api/state crashed with SIGSEGV (strlen on a dangling pointer) whenever a device
// carried .mlp files. The LivePalettes seam references its publisher's arrays, a throwaway Drivers
// (the /api/modules probe) published on construction and was immediately destroyed, and the seam
// kept pointing into the freed object. The fix is twofold: publication moved to prepare() (a probe
// never runs it), and clear() takes the caller's array so a departing publisher can only ever
// unpublish itself. These pin the seam half.

TEST_CASE("LivePalettes: a departing publisher cannot unpublish its successor") {
    static const char* aNames[] = {"a.mlp"};
    static const char* aTags[]  = {""};
    static const char* bNames[] = {"b.mlp", "c.mlp"};
    static const char* bTags[]  = {"", ""};
    mm::LivePalettes::set(aNames, aTags, 1);
    mm::LivePalettes::set(bNames, bTags, 2);          // B takes the seam over
    mm::LivePalettes::clear(aNames);                  // A is destroyed later: must be a no-op
    CHECK(mm::LivePalettes::count() == 2);
    CHECK(std::strcmp(mm::LivePalettes::nameAt(0), "b.mlp") == 0);
    mm::LivePalettes::clear(bNames);                  // the owner clears: the seam empties
    CHECK(mm::LivePalettes::count() == 0);
}

TEST_CASE("LivePalettes: an unconditional clear detaches whoever owns the seam") {
    static const char* names[] = {"x.mlp"};
    static const char* tags[]  = {""};
    mm::LivePalettes::set(names, tags, 1);
    mm::LivePalettes::clear();                        // no argument: the reset everyone may use
    CHECK(mm::LivePalettes::count() == 0);
    CHECK(std::strcmp(mm::LivePalettes::nameAt(0), "") == 0);   // and reads stay safe
}
