#pragma once

#include "light/EffectBase.h"

namespace mm::light {

class RainbowEffect : public EffectBase {
public:
    const char* name() const override { return "Rainbow 2D"; }

    void addControls() override {
        speedIdx_ = MoonModule::addControl("speed", uint16_t(1), uint16_t(1), uint16_t(255));
    }

    void loop() override {
        auto px = pixels();
        int16_t w = width();
        int16_t h = height();
        uint16_t spd = control(speedIdx_)->u16.value;
        uint32_t f = frame();

        for (size_t i = 0; i < px.size(); ++i) {
            int16_t x = (w > 0) ? static_cast<int16_t>(i % w) : 0;
            int16_t y = (w > 0) ? static_cast<int16_t>(i / w) : 0;

            // Diagonal rainbow: hue from x+y, animated by frame
            auto hue = static_cast<uint8_t>(
                (x * 256 / (w > 0 ? w : 1) + y * 256 / (h > 0 ? h : 1) + f * spd) & 0xFF);
            px[i] = RGB::fromHSV(hue, 255, 255);
        }
    }

private:
    uint8_t speedIdx_ = 0;
};

} // namespace mm::light
