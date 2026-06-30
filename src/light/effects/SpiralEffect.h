#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"   // colorFromPalette + active palette
#include "core/color.h"
#include "core/math8.h"   // sin8/cos8/dist8/atan2_8

namespace mm {

class SpiralEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t bpm = 40;
    uint8_t twist = 4;
    uint8_t hue_shift = 0;

    void onBuildControls() override {
        controls_.addUint8("bpm", bpm, 1, 255);
        controls_.addUint8("twist", twist, 1, 255);
        controls_.addUint8("hue_shift", hue_shift, 0, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        // Accumulate the raw (dt * bpm) product; divide only at the read site.
        // Per-tick `dt*bpm*256/60000` rounds to 0 on desktop (dt ≈ 0..1ms) and
        // freezes the animation; see MetaballsEffect for the same fix.
        phase_num_ += static_cast<uint64_t>(dt) * bpm;
        uint8_t t = static_cast<uint8_t>((phase_num_ * 256) / 60000);

        int16_t cx = static_cast<int16_t>(w >> 1);
        int16_t cy = static_cast<int16_t>(h >> 1);

        for (lengthType y = 0; y < h; y++) {
            int16_t dy = static_cast<int16_t>(y) - cy;
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                int16_t dx = static_cast<int16_t>(x) - cx;
                uint8_t angle = atan2_8(dy, dx);
                uint8_t dist = dist8(dx, dy);
                uint8_t hue = static_cast<uint8_t>(
                    angle + static_cast<uint8_t>(dist * twist) - t + hue_shift);
                RGB c = colorFromPalette(*Palettes::active(), hue);

                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    // Numerator-only accumulator (units of dt*bpm). See loop() for why.
    uint64_t phase_num_ = 0;
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
