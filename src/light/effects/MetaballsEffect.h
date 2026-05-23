#pragma once

#include "light/Layer.h"
#include "core/color.h"

namespace mm {

class MetaballsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t bpm = 30;
    uint8_t radius = 28;
    uint8_t hue_shift = 0;

    static constexpr uint8_t NUM_BALLS = 4;

    void onBuildControls() override {
        controls_.addUint8("bpm", bpm, 1, 255);
        controls_.addUint8("radius", radius, 4, 255);
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
        phase_ += static_cast<uint64_t>(dt) * bpm * 256 / 60000;
        uint8_t t = static_cast<uint8_t>(phase_);

        int16_t bx[NUM_BALLS];
        int16_t by[NUM_BALLS];
        static constexpr uint8_t SPEED_MUL[NUM_BALLS]  = { 1, 2, 3, 1 };
        static constexpr uint8_t PHASE_X[NUM_BALLS]    = { 0, 30, 60, 120 };
        static constexpr uint8_t PHASE_Y[NUM_BALLS]    = { 64, 94, 124, 184 };
        for (uint8_t b = 0; b < NUM_BALLS; b++) {
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
                for (uint8_t b = 0; b < NUM_BALLS; b++) {
                    int32_t dx = static_cast<int32_t>(x) - bx[b];
                    int32_t dy = static_cast<int32_t>(y) - by[b];
                    int32_t d2 = dx * dx + dy * dy + 1;
                    field += static_cast<uint32_t>((r2 * 64) / d2);
                }
                uint8_t bright = field > 255 ? 255 : static_cast<uint8_t>(field);
                uint8_t hue = static_cast<uint8_t>((field >> 1) + hue_shift);
                RGB c = hsvToRgb(hue, 240, bright);

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
