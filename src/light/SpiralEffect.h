#pragma once

#include "light/Layer.h"
#include "core/color.h"

namespace mm {

class SpiralEffect : public EffectBase {
public:
    const char* tags() const override { return "🌀"; }

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
        phase_ += static_cast<uint64_t>(dt) * bpm * 256 / 60000;
        uint8_t t = static_cast<uint8_t>(phase_);

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
                RGB c = hsvToRgb(hue, 255, 255);

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
