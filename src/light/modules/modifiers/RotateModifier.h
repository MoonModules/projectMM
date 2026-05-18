#pragma once

#include "core/MoonModule.h"
#include "light/Pixel.h"
#include "platform/Alloc.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace mm::light {

// Dynamic modifier: rotates the 2D pixel buffer around its centre.
// The angle increases each frame based on speed.
// Has per-pixel cost on the hot path.
class RotateModifier : public MoonModule {
public:
    const char* name() const override { return "Rotate"; }

    void addControls() override {
        speedIdx_ = MoonModule::addControl("speed", uint16_t(1), uint16_t(1), uint16_t(255));
    }

    uint16_t speed() const { return control(speedIdx_)->u16.value; }

    void transformPixels(std::span<RGB> pixels, uint32_t frame,
                         int16_t w, int16_t h) const {
        if (pixels.empty() || w <= 0 || h <= 0) return;

        size_t n = pixels.size();

        // Allocate temp buffer (cold-path alloc, but acceptable for now)
        auto* tmp = static_cast<RGB*>(platform::alloc(n * sizeof(RGB)));
        if (!tmp) return;
        std::memset(tmp, 0, n * sizeof(RGB));

        // Rotation angle: increases with frame and speed
        constexpr double PI = 3.14159265358979323846;
        double angle = (frame * speed() * PI) / (180.0 * 2.0);
        double cosA = std::cos(angle);
        double sinA = std::sin(angle);

        double cx = w / 2.0;
        double cy = h / 2.0;

        // For each destination pixel, sample from rotated source
        for (int16_t dy = 0; dy < h; ++dy) {
            for (int16_t dx = 0; dx < w; ++dx) {
                // Rotate (dx,dy) back to find source
                double rx = (dx - cx) * cosA + (dy - cy) * sinA + cx;
                double ry = -(dx - cx) * sinA + (dy - cy) * cosA + cy;

                auto sx = static_cast<int16_t>(rx);
                auto sy = static_cast<int16_t>(ry);

                size_t dstIdx = static_cast<size_t>(dx + dy * w);
                if (sx >= 0 && sx < w && sy >= 0 && sy < h && dstIdx < n) {
                    size_t srcIdx = static_cast<size_t>(sx + sy * w);
                    if (srcIdx < n) {
                        tmp[dstIdx] = pixels[srcIdx];
                    }
                }
            }
        }

        // Copy back
        std::memcpy(pixels.data(), tmp, n * sizeof(RGB));
        platform::free(tmp);
    }

private:
    uint8_t speedIdx_ = 0;
};

} // namespace mm::light
