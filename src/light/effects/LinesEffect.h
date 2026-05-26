#pragma once

#include "light/layers/Layer.h"

namespace mm {

// Three planes sweep in sync at a given BPM:
//   Red   — YZ plane sweeps left→right (x oscillates)
//   Green — XZ plane sweeps top→bottom (y oscillates)
//   Blue  — XY plane sweeps front→back (z oscillates)
// Useful for verifying preview axis orientation: each colour names its axis.
// Port of MoonLight's Lines effect via projectMM-v1/LinesEffect.h.
class LinesEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }

    uint8_t speed = 30;   // BPM
    uint8_t axis  = 0;    // 0=all 1=x(red) 2=y(green) 3=z(blue)

    void onBuildControls() override {
        static constexpr const char* kAxisOptions[] = {"all", "x (red)", "y (green)", "z (blue)"};
        controls_.addUint8("speed", speed, 1, 240);
        controls_.addSelect("axis", axis, kAxisOptions, 4);
    }

    void loop() override {
        uint8_t* buf = buffer();
        const lengthType w   = width();
        const lengthType h   = height();
        const lengthType d   = depth();
        const uint8_t    cpl = channelsPerLight();

        memset(buf, 0, static_cast<size_t>(w) * h * d * cpl);

        // Sawtooth 0–65535 at speed BPM. Use 64-bit to avoid overflow.
        const uint32_t period = 60000u / static_cast<uint32_t>(speed ? speed : 1);
        const uint16_t beat   = static_cast<uint16_t>(
            (static_cast<uint64_t>(elapsed() % period) * 65535u) / period
        );

        auto setRGB = [&](lengthType x, lengthType y, lengthType z,
                          uint8_t r, uint8_t g, uint8_t b) {
            size_t off = (static_cast<size_t>(z) * h * w
                         + static_cast<size_t>(y) * w + x) * cpl;
            if (cpl >= 1) buf[off + 0] = r;
            if (cpl >= 2) buf[off + 1] = g;
            if (cpl >= 3) buf[off + 2] = b;
        };

        // Red — YZ plane at x = beat position, sweeps left→right
        if (w > 1 && (axis == 0 || axis == 1)) {
            const lengthType x = static_cast<lengthType>(
                static_cast<uint32_t>(beat) * (w - 1) / 65535u);
            for (lengthType z = 0; z < d; z++)
                for (lengthType y = 0; y < h; y++)
                    setRGB(x, y, z, 255, 0, 0);
        }

        // Green — XZ plane at y = beat position, sweeps top→bottom
        if (h > 1 && (axis == 0 || axis == 2)) {
            const lengthType y = static_cast<lengthType>(
                static_cast<uint32_t>(beat) * (h - 1) / 65535u);
            for (lengthType z = 0; z < d; z++)
                for (lengthType x = 0; x < w; x++)
                    setRGB(x, y, z, 0, 255, 0);
        }

        // Blue — XY plane at z = beat position, sweeps front→back (3D only)
        if (d > 1 && (axis == 0 || axis == 3)) {
            const lengthType z = static_cast<lengthType>(
                static_cast<uint32_t>(beat) * (d - 1) / 65535u);
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++)
                    setRGB(x, y, z, 0, 0, 255);
        }
    }
};

} // namespace mm
