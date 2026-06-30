#pragma once

#include "light/layers/Layer.h"
#include "light/Palette.h"   // colorFromPalette + active palette
#include "core/color.h"

namespace mm {

class PlasmaEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    Dim dimensions() const override { return Dim::D3; }

    uint8_t bpm = 30;
    // Larger scale = smaller per-pixel step (256/scale) = lower spatial frequency = bigger, calmer
    // rolling blobs. The default is high so plasma reads as large blobs, not fine noise; lower it
    // in the UI for a busier field.
    uint8_t scale_x = 48;
    uint8_t scale_y = 48;
    uint8_t hue_shift = 0;

    void onBuildControls() override {
        controls_.addUint8("bpm", bpm, 1, 255);
        controls_.addUint8("scale_x", scale_x, 1, 255);
        controls_.addUint8("scale_y", scale_y, 1, 255);
        controls_.addUint8("hue_shift", hue_shift, 0, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        lengthType d = depth();
        uint8_t cpl = channelsPerLight();

        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        phase_ += static_cast<uint64_t>(dt) * bpm * static_cast<uint64_t>(w) * 64 / 60000;

        uint8_t step_x = static_cast<uint8_t>(256 / scale_x);
        uint8_t step_y = static_cast<uint8_t>(256 / scale_y);
        // z reuses scale_y for spatial frequency — keeps the control surface
        // simple while still varying the field along the third axis.
        uint8_t step_z = step_y;
        uint8_t t1 = static_cast<uint8_t>(phase_);
        uint8_t t2 = static_cast<uint8_t>(phase_ * 2);
        uint8_t t3 = static_cast<uint8_t>(phase_ * 3);

        // For d == 1 take the original 4-sine path unchanged — bit-for-bit
        // identical to the previous 2D-only output. For d > 1 add a 5th sine
        // driven by z so the field varies along the depth axis.
        const bool is3d = (d > 1);
        for (lengthType z = 0; z < d; z++) {
            uint8_t s5_z = is3d
                ? sin8(static_cast<uint8_t>(static_cast<uint8_t>(z) * step_z + t1))
                : 0;
            for (lengthType y = 0; y < h; y++) {
                uint8_t s2_y = sin8(static_cast<uint8_t>(static_cast<uint8_t>(y) * step_y + t2));
                uint8_t yx_off = static_cast<uint8_t>(static_cast<uint8_t>(y) * step_x - t3);
                uint8_t yx_neg = static_cast<uint8_t>(128 - static_cast<uint8_t>(y) * step_y + t1);

                uint8_t* row = buf
                    + (static_cast<size_t>(z) * static_cast<size_t>(h) + static_cast<size_t>(y))
                      * static_cast<size_t>(w) * cpl;
                for (lengthType x = 0; x < w; x++) {
                    uint8_t xs = static_cast<uint8_t>(static_cast<uint8_t>(x) * step_x);
                    uint8_t s1 = sin8(static_cast<uint8_t>(xs + t1));
                    uint8_t s3 = sin8(static_cast<uint8_t>(xs + yx_off));
                    uint8_t s4 = sin8(static_cast<uint8_t>(
                        static_cast<uint8_t>(static_cast<uint8_t>(x) * step_y) + yx_neg));
                    // The 5-term path uses /5 (the 2D path's /4 is the >>2 below); both
                    // average the sines. /5 is kept literal — -O3 lowers it to magic
                    // multiply + shift automatically, so hand-rolling the reciprocal
                    // would produce identical assembly with worse readability.
                    uint8_t hue = is3d
                        ? static_cast<uint8_t>(
                              (static_cast<uint16_t>(s1 + s2_y + s3 + s4 + s5_z) / 5) + hue_shift)
                        : static_cast<uint8_t>(((s1 + s2_y + s3 + s4) >> 2) + hue_shift);
                    RGB c = colorFromPalette(*Palettes::active(), hue);

                    if (cpl >= 1) row[0] = c.r;
                    if (cpl >= 2) row[1] = c.g;
                    if (cpl >= 3) row[2] = c.b;
                    row += cpl;
                }
            }
        }
    }

private:
    uint64_t phase_ = 0;
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
