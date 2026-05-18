#pragma once

#include "core/MoonModule.h"
#include <cstdint>

namespace mm::light {

class GridLayout : public MoonModule {
public:
    const char* name() const override { return "Grid"; }

    void addControls() override {
        widthIdx_  = MoonModule::addControl("width",  uint16_t(128), uint16_t(1), uint16_t(1024));
        heightIdx_ = MoonModule::addControl("height", uint16_t(128), uint16_t(1), uint16_t(1024));
        depthIdx_  = MoonModule::addControl("depth",  uint16_t(1),  uint16_t(1), uint16_t(64));
    }

    void onChange(uint8_t) override { markDirty(); }

    int16_t width() const  { return static_cast<int16_t>(control(widthIdx_)->u16.value); }
    int16_t height() const { return static_cast<int16_t>(control(heightIdx_)->u16.value); }
    int16_t depth() const  { return static_cast<int16_t>(control(depthIdx_)->u16.value); }
    size_t pixelCount() const { return size_t(width()) * height() * depth(); }

    template<typename Fn>
    void forEachCoord(Fn&& fn) const {
        int16_t w = width(), h = height(), d = depth();
        uint32_t idx = 0;
        for (int16_t z = 0; z < d; ++z)
            for (int16_t y = 0; y < h; ++y)
                for (int16_t x = 0; x < w; ++x)
                    fn(idx++, x, y, z);
    }

    using CoordCallback = void(*)(void* ctx, uint32_t idx, int16_t x, int16_t y, int16_t z);
    void forEachCoord(CoordCallback cb, void* ctx) const {
        forEachCoord([&](uint32_t idx, int16_t x, int16_t y, int16_t z) {
            cb(ctx, idx, x, y, z);
        });
    }

private:
    uint8_t widthIdx_ = 0, heightIdx_ = 0, depthIdx_ = 0;
};

} // namespace mm::light
