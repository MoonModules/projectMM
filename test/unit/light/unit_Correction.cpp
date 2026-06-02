// @module Correction

#include "doctest.h"
#include "light/drivers/Correction.h"

#include <cstdint>

// Pins the per-driver output correction: brightness LUT, channel reorder, and RGBW
// white derivation. The Drivers container owns a Correction, rebuilds it on a
// brightness/light-preset change, and hands it to each physical driver, which calls
// apply() per light. These tests pin the transform so a regression in the LUT fill,
// the preset→order mapping, or the white math fails here.

using mm::Correction;
using mm::LightPreset;

// At brightness=255, the LUT maps every input value to itself (no scaling).
TEST_CASE("Correction brightness LUT: full brightness is identity") {
    Correction c;
    c.rebuild(255, LightPreset::RGB);
    for (int v = 0; v < 256; v++) CHECK(c.briLut[v] == v);
}

// At brightness=128, every entry is roughly halved using scale8 (255→128, 128→64, 2→1).
TEST_CASE("Correction brightness LUT: half brightness halves each value (scale8)") {
    Correction c;
    c.rebuild(128, LightPreset::RGB);
    CHECK(c.briLut[0] == 0);
    CHECK(c.briLut[255] == 128);   // (255*128)/255 = 128
    CHECK(c.briLut[128] == 64);    // (128*128)/255 = 64
    CHECK(c.briLut[2] == 1);       // (2*128)/255 = 1
}

// RGB preset at full brightness passes the source RGB through unchanged (3 output channels, no white).
TEST_CASE("Correction RGB preset: apply is identity at full brightness") {
    Correction c;
    c.rebuild(255, LightPreset::RGB);
    CHECK(c.outChannels == 3);
    CHECK_FALSE(c.deriveWhite);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[3] = {};
    c.apply(src, out);
    CHECK(out[0] == 10);
    CHECK(out[1] == 20);
    CHECK(out[2] == 30);
}

// GRB preset swaps R and G in the output (G first, then R, then B) — for WS2812-like drivers.
TEST_CASE("Correction GRB preset: channels reordered, 3 output channels") {
    Correction c;
    c.rebuild(255, LightPreset::GRB);
    CHECK(c.outChannels == 3);
    CHECK_FALSE(c.deriveWhite);
    const uint8_t src[3] = {10, 20, 30};  // R=10 G=20 B=30
    uint8_t out[3] = {};
    c.apply(src, out);
    CHECK(out[0] == 20);  // G
    CHECK(out[1] == 10);  // R
    CHECK(out[2] == 30);  // B
}

// BGR preset reverses the channel order entirely (B, G, R).
TEST_CASE("Correction BGR preset: full reverse") {
    Correction c;
    c.rebuild(255, LightPreset::BGR);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[3] = {};
    c.apply(src, out);
    CHECK(out[0] == 30);  // B
    CHECK(out[1] == 20);  // G
    CHECK(out[2] == 10);  // R
}

// RGBW preset adds a fourth white channel derived as min(R, G, B) per pixel.
TEST_CASE("Correction RGBW preset: 4 channels, white = min(r,g,b)") {
    Correction c;
    c.rebuild(255, LightPreset::RGBW);
    CHECK(c.outChannels == 4);
    CHECK(c.deriveWhite);
    const uint8_t src[3] = {10, 20, 30};  // min = 10
    uint8_t out[4] = {};
    c.apply(src, out);
    CHECK(out[0] == 10);  // R
    CHECK(out[1] == 20);  // G
    CHECK(out[2] == 30);  // B
    CHECK(out[3] == 10);  // W = min(10,20,30)
}

// GRBW preset combines the GRB reorder with the W derivation (G, R, B, W=min).
TEST_CASE("Correction GRBW preset: reordered RGB + white") {
    Correction c;
    c.rebuild(255, LightPreset::GRBW);
    CHECK(c.outChannels == 4);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[4] = {};
    c.apply(src, out);
    CHECK(out[0] == 20);  // G
    CHECK(out[1] == 10);  // R
    CHECK(out[2] == 30);  // B
    CHECK(out[3] == 10);  // W = min
}

// Brightness scaling runs before white derivation so W = min of the *scaled* RGB values.
TEST_CASE("Correction: brightness applied BEFORE white derivation") {
    // White must be min of the *scaled* channels, not the raw ones.
    Correction c;
    c.rebuild(128, LightPreset::RGBW);  // half brightness
    const uint8_t src[3] = {100, 200, 60};  // scaled: 50, 100, 30 → min = 30
    uint8_t out[4] = {};
    c.apply(src, out);
    CHECK(out[0] == 50);   // (100*128)/255
    CHECK(out[1] == 100);  // (200*128)/255
    CHECK(out[2] == 30);   // (60*128)/255
    CHECK(out[3] == 30);   // min(50,100,30) — proves white uses scaled values
}

// rebuild() can switch the output channel count between RGB (3) and RGBW (4) on the fly.
TEST_CASE("Correction: rebuild switches output channel count RGB<->RGBW") {
    Correction c;
    c.rebuild(255, LightPreset::RGB);
    CHECK(c.outChannels == 3);
    c.rebuild(255, LightPreset::RGBW);
    CHECK(c.outChannels == 4);
    c.rebuild(255, LightPreset::GRB);
    CHECK(c.outChannels == 3);
    CHECK_FALSE(c.deriveWhite);
}
