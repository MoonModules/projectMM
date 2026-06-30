#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"   // colorFromPalette + active palette
#include "core/math8.h"   // sin8 (+ color.h: hsvToRgb)

namespace mm {

// Two interfering sine waves whose sum drives the hue — a flowing, moiré-like colour
// field. The horizontal and vertical waves run at independent frequencies and slightly
// different time rates, so they beat against each other. 2D (Layer::extrude lifts it to
// 3D). Ported from WLED's "Distortion Waves".
//
// Integer-only: angles are uint8_t (256 = full turn), sin8() returns 0..255. The two
// sines are averaged into a hue byte. WLED runs the vertical wave's time ~1.3× the
// horizontal; we approximate 1.3 as (t*333)>>8 = t*1.301..., staying in integer math.
// hsvToRgb(hue, 240, 255) keeps WLED's slightly-desaturated look.
//
// Prior art: MoonLight E_WLED.h (the WLED port); projectMM v1/v2 DistortionWaves (those
// used float sinf — this is the integer-sin8 equivalent).
class DistortionWavesEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight / WLED origin
    Dim dimensions() const override { return Dim::D2; }

    uint8_t freq_x = 3;   // horizontal wave frequency, 1..8
    uint8_t freq_y = 3;   // vertical wave frequency, 1..8
    uint8_t speed = 50;   // animation speed, 0..100 (0 = frozen)

    void onBuildControls() override {
        controls_.addUint8("freq_x", freq_x, 1, 8);
        controls_.addUint8("freq_y", freq_y, 1, 8);
        controls_.addUint8("speed", speed, 0, 100);
    }

    void loop() override {
        uint8_t* buf = buffer();
        const lengthType w = width();
        const lengthType h = height();
        const uint8_t cpl = channelsPerLight();

        const uint32_t now = elapsed();
        const uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        // speed 0 freezes (no time advance). Otherwise accumulate dt*speed and read the
        // high byte as the time phase (uint8 angle), same accumulator idiom as elsewhere.
        if (speed) phase_ += static_cast<uint64_t>(dt) * speed;
        const uint8_t t = static_cast<uint8_t>((phase_ * 256) / 60000);
        const uint8_t ty = static_cast<uint8_t>((static_cast<uint16_t>(t) * 333) >> 8);  // ~1.3·t

        for (lengthType y = 0; y < h; y++) {
            const uint8_t sy = sin8(static_cast<uint8_t>(static_cast<uint8_t>(y) * freq_y + ty));
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                const uint8_t sx = sin8(static_cast<uint8_t>(static_cast<uint8_t>(x) * freq_x + t));
                // Average the two sines (each 0..255) → a hue byte. The interference of
                // the two frequencies + the 1.3× time skew is what makes the pattern move.
                const uint8_t hue = static_cast<uint8_t>((static_cast<uint16_t>(sx) + sy) >> 1);
                const RGB c = colorFromPalette(*Palettes::active(), hue);
                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    uint64_t phase_ = 0;
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
