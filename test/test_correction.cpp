#include "doctest.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Drivers.h"
#include "light/layouts/GridLayout.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/effects/NoiseEffect.h"

#include <cstdint>

// Pins the per-driver output correction: brightness LUT, channel reorder, and RGBW
// white derivation. The Drivers container owns a Correction, rebuilds it on a
// brightness/light-preset change, and hands it to each physical driver, which calls
// apply() per light. These tests pin the transform so a regression in the LUT fill,
// the preset→order mapping, or the white math fails here.

using mm::Correction;
using mm::LightPreset;

TEST_CASE("Correction brightness LUT: full brightness is identity") {
    Correction c;
    c.rebuild(255, LightPreset::RGB);
    for (int v = 0; v < 256; v++) CHECK(c.briLut[v] == v);
}

TEST_CASE("Correction brightness LUT: half brightness halves each value (scale8)") {
    Correction c;
    c.rebuild(128, LightPreset::RGB);
    CHECK(c.briLut[0] == 0);
    CHECK(c.briLut[255] == 128);   // (255*128)/255 = 128
    CHECK(c.briLut[128] == 64);    // (128*128)/255 = 64
    CHECK(c.briLut[2] == 1);       // (2*128)/255 = 1
}

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

// --- Selective control-change rebuild gate -------------------------------------
// handleSetControl rebuilds the pipeline only when controlChangeTriggersBuildState() is
// true. These pin which module types opt in: Layout + Modifier rebuild (their
// controls change physical dims / LUT shape); effects and Drivers do not (their
// controls are values read in the hot path). Regression target: a change here that
// made effect controls rebuild would re-introduce the slider stutter we just fixed.

TEST_CASE("controlChangeTriggersBuildState: Layout and Modifier opt in") {
    mm::GridLayout layout;
    mm::MirrorModifier modifier;
    CHECK(layout.controlChangeTriggersBuildState("width"));
    CHECK(modifier.controlChangeTriggersBuildState("mirrorX"));
}

TEST_CASE("controlChangeTriggersBuildState: effects and Drivers do NOT rebuild") {
    mm::NoiseEffect effect;
    mm::Drivers drivers;
    CHECK_FALSE(effect.controlChangeTriggersBuildState("scale"));
    CHECK_FALSE(drivers.controlChangeTriggersBuildState("brightness"));
}
