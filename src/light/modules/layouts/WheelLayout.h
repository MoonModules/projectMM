#pragma once

#include "core/MoonModule.h"
#include <cmath>
#include <cstdint>

namespace mm::light {

class WheelLayout : public MoonModule {
public:
    const char* name() const override { return "Wheel"; }

    void addControls() override {
        spokesIdx_        = MoonModule::addControl("spokes",       uint16_t(8),  uint16_t(2),  uint16_t(64));
        ledsPerSpokeIdx_  = MoonModule::addControl("ledsPerSpoke", uint16_t(10), uint16_t(1),  uint16_t(256));
    }

    int16_t spokes() const       { return static_cast<int16_t>(control(spokesIdx_)->u16.value); }
    int16_t ledsPerSpoke() const { return static_cast<int16_t>(control(ledsPerSpokeIdx_)->u16.value); }
    size_t pixelCount() const    { return size_t(spokes()) * ledsPerSpoke(); }

    template<typename Fn>
    void forEachCoord(Fn&& fn) const {
        int16_t nSpokes = spokes();
        int16_t nLeds = ledsPerSpoke();
        int16_t center = nLeds;
        constexpr double PI = 3.14159265358979323846;
        uint32_t idx = 0;

        for (int16_t s = 0; s < nSpokes; ++s) {
            double angle = (2.0 * PI * s) / nSpokes;
            double cosA = std::cos(angle);
            double sinA = std::sin(angle);
            for (int16_t led = 0; led < nLeds; ++led) {
                double r = static_cast<double>(led + 1);
                auto x = static_cast<int16_t>(center + static_cast<int>(std::round(cosA * r)));
                auto y = static_cast<int16_t>(center + static_cast<int>(std::round(sinA * r)));
                fn(idx++, x, y, int16_t(0));
            }
        }
    }

    using CoordCallback = void(*)(void* ctx, uint32_t idx, int16_t x, int16_t y, int16_t z);
    void forEachCoord(CoordCallback cb, void* ctx) const {
        forEachCoord([&](uint32_t idx, int16_t x, int16_t y, int16_t z) {
            cb(ctx, idx, x, y, z);
        });
    }

    int16_t gridSize() const { return static_cast<int16_t>(2 * ledsPerSpoke() + 1); }

private:
    uint8_t spokesIdx_ = 0, ledsPerSpokeIdx_ = 0;
};

} // namespace mm::light
