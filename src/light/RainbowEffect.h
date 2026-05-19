#pragma once

#include "light/Layer.h"
#include "core/color.h"

namespace mm {

class RainbowEffect : public EffectBase {
public:
    uint8_t speed = 60; // BPM

    void onBuildControls() override {
        controls_.addUint8("speed", speed, 1, 255);
    }

    void loop() override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        nrOfLightsType count = nrOfLights();

        // BPM to phase: one full hue cycle (256) per beat
        // phase = elapsed_ms * speed / 60000 * 256
        uint32_t phase = static_cast<uint32_t>(elapsed()) * speed * 256 / 60000;

        for (nrOfLightsType i = 0; i < count; i++) {
            lengthType x = static_cast<lengthType>(i % w);
            lengthType y = static_cast<lengthType>(i / w);

            // Diagonal rainbow: hue varies with x + y + time
            uint8_t hue = static_cast<uint8_t>(
                (static_cast<uint32_t>(x + y) * 256 / (w + h)) + phase
            );

            RGB c = hsvToRgb(hue, 255, 255);
            size_t offset = static_cast<size_t>(i) * cpl;
            buf[offset + 0] = c.r;
            buf[offset + 1] = c.g;
            buf[offset + 2] = c.b;
        }
    }
};

} // namespace mm
