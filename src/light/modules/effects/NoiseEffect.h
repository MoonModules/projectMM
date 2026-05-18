#pragma once

#include "light/EffectBase.h"
#include "light/Noise.h"

namespace mm::light {

class NoiseEffect : public EffectBase {
public:
    const char* name() const override { return "Noise 2D"; }

    void addControls() override {
        speedIdx_ = MoonModule::addControl("speed", uint16_t(1), uint16_t(1), uint16_t(255));
        scaleIdx_ = MoonModule::addControl("scale", uint16_t(30), uint16_t(1), uint16_t(255));
    }

    void loop() override {
        auto px = pixels();
        int16_t w = width();
        uint16_t spd = control(speedIdx_)->u16.value;
        uint16_t scl = control(scaleIdx_)->u16.value;
        uint32_t f = frame();

        for (size_t i = 0; i < px.size(); ++i) {
            int16_t x = (w > 0) ? static_cast<int16_t>(i % w) : 0;
            int16_t y = (w > 0) ? static_cast<int16_t>(i / w) : 0;

            // 2D noise field with time animation
            uint16_t nx = static_cast<uint16_t>(x * scl + f * spd);
            uint16_t ny = static_cast<uint16_t>(y * scl);

            uint8_t n = noise8(nx, ny);
            px[i] = RGB::fromHSV(n, 255, 255);
        }
    }

private:
    uint8_t speedIdx_ = 0;
    uint8_t scaleIdx_ = 0;
};

} // namespace mm::light
