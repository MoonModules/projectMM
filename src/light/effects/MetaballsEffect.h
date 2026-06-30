#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"   // colorFromPalette + active palette
#include "core/color.h"
#include "core/math8.h"   // sin8/cos8/dist8/atan2_8

namespace mm {

class MetaballsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t bpm = 30;
    uint8_t radius = 28;
    uint8_t count = 4;   // number of balls (1..MAX_BALLS); each follows its own sine path
    uint8_t hue_shift = 0;

    static constexpr uint8_t MAX_BALLS = 8;

    void onBuildControls() override {
        controls_.addUint8("bpm", bpm, 1, 255);
        controls_.addUint8("radius", radius, 4, 255);
        controls_.addUint8("count", count, 1, MAX_BALLS);
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
        // Accumulate the raw (dt * bpm) product; divide by 60000/256 at the read
        // site. Doing the divide per-tick truncates sub-millisecond ticks to 0
        // — on desktop with dt=0..1ms, `dt*bpm*256/60000` rounds to 0 and the
        // animation freezes. Keeping the numerator in the accumulator preserves
        // every increment.
        phase_num_ += static_cast<uint64_t>(dt) * bpm;
        uint8_t t = static_cast<uint8_t>((phase_num_ * 256) / 60000);

        const uint8_t n = count < MAX_BALLS ? count : MAX_BALLS;
        int16_t bx[MAX_BALLS];
        int16_t by[MAX_BALLS];
        static constexpr uint8_t SPEED_MUL[MAX_BALLS]  = { 1, 2, 3, 1, 2, 3, 1, 2 };
        static constexpr uint8_t PHASE_X[MAX_BALLS]    = { 0, 30, 60, 120, 160, 200, 90, 220 };
        static constexpr uint8_t PHASE_Y[MAX_BALLS]    = { 64, 94, 124, 184, 16, 210, 150, 40 };
        for (uint8_t b = 0; b < n; b++) {
            uint8_t tb = static_cast<uint8_t>(t * SPEED_MUL[b]);
            bx[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + PHASE_X[b])) * w) >> 8);
            by[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + PHASE_Y[b])) * h) >> 8);
        }

        // Field strength: sum of r^2 / (d^2 + 1)
        int32_t r2 = static_cast<int32_t>(radius) * radius;

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * w * cpl;
            for (lengthType x = 0; x < w; x++) {
                uint32_t field = 0;
                for (uint8_t b = 0; b < n; b++) {
                    int32_t dx = static_cast<int32_t>(x) - bx[b];
                    int32_t dy = static_cast<int32_t>(y) - by[b];
                    int32_t d2 = dx * dx + dy * dy + 1;
                    field += static_cast<uint32_t>((r2 * 64) / d2);
                }
                uint8_t bright = field > 255 ? 255 : static_cast<uint8_t>(field);
                uint8_t hue = static_cast<uint8_t>((field >> 1) + hue_shift);
                RGB c = colorFromPalette(*Palettes::active(), hue, bright);

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
